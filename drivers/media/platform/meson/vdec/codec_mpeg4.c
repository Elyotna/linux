// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 */

#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "codec_mpeg4.h"
#include "codec_helpers.h"
#include "canvas.h"

#define SIZE_WORKSPACE		SZ_1M
#define DCAC_BUFF_START_IP	0x02b00000

/* DOS registers */
#define ASSIST_MBOX1_CLR_REG 0x01d4
#define ASSIST_MBOX1_MASK    0x01d8

#define PSCALE_CTRL 0x2444

#define MDEC_PIC_DC_CTRL   0x2638
#define MDEC_PIC_DC_THRESH 0x26e0

#define AV_SCRATCH_0        0x2700
#define MP4_PIC_RATIO       0x2714
#define MP4_RATE            0x270c
#define AV_SCRATCH_4        0x2710
#define MP4_ERR_COUNT       0x2718
#define MP4_PIC_WH          0x271c
#define MREG_BUFFERIN       0x2720
#define MREG_BUFFEROUT      0x2724
#define MP4_NOT_CODED_CNT   0x2728
#define MP4_VOP_TIME_INC    0x272c
#define MP4_OFFSET_REG      0x2730
#define MP4_SYS_RATE        0x2738
#define MEM_OFFSET_REG      0x273c
#define AV_SCRATCH_G        0x2740
#define MREG_FATAL_ERROR    0x2754

#define DOS_SW_RESET0 0xfc00

struct codec_mpeg4 {
	/* Buffer for the MPEG4 Workspace */
	void      *workspace_vaddr;
	dma_addr_t workspace_paddr;
};

static int codec_mpeg4_can_recycle(struct vdec_core *core)
{
	return !readl_relaxed(core->dos_base + MREG_BUFFERIN);
}

static void codec_mpeg4_recycle(struct vdec_core *core, u32 buf_idx)
{
	writel_relaxed(~(1 << buf_idx), core->dos_base + MREG_BUFFERIN);
}

/* The MPEG4 canvas regs are not contiguous,
 * handle it specifically instead of using the helper
 * AV_SCRATCH_0 - AV_SCRATCH_3  ;  AV_SCRATCH_G - AV_SCRATCH_J
 */
void codec_mpeg4_set_canvases(struct vdec_session *sess) {
	struct v4l2_m2m_buffer *buf;
	struct vdec_core *core = sess->core;
	void *current_reg = core->dos_base + AV_SCRATCH_0;
	u32 width = ALIGN(sess->width, 64);
	u32 height = ALIGN(sess->height, 64);

	/* Setup NV12 canvases for Decoded Picture Buffer (dpb)
	 * Map them to the user buffers' planes
	 */
	v4l2_m2m_for_each_dst_buf(sess->m2m_ctx, buf) {
		u32 buf_idx    = buf->vb.vb2_buf.index;
		u32 cnv_y_idx  = buf_idx * 2;
		u32 cnv_uv_idx = buf_idx * 2 + 1;
		dma_addr_t buf_y_paddr  =
			vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);
		dma_addr_t buf_uv_paddr =
			vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 1);

		/* Y plane */
		vdec_canvas_setup(core->dmc_base, cnv_y_idx, buf_y_paddr, width, height, MESON_CANVAS_WRAP_NONE, MESON_CANVAS_BLKMODE_LINEAR);

		/* U/V plane */
		vdec_canvas_setup(core->dmc_base, cnv_uv_idx, buf_uv_paddr, width, height / 2, MESON_CANVAS_WRAP_NONE, MESON_CANVAS_BLKMODE_LINEAR);

		writel_relaxed(((cnv_uv_idx) << 16) |
			       ((cnv_uv_idx) << 8)  |
				(cnv_y_idx), current_reg);

		current_reg += 4;
		if (current_reg == core->dos_base + AV_SCRATCH_4)
			current_reg = core->dos_base + AV_SCRATCH_G;
	}
}

static int codec_mpeg4_start(struct vdec_session *sess) {
	struct vdec_core *core = sess->core;
	struct codec_mpeg4 *mpeg4 = sess->priv;
	int ret;

	mpeg4 = kzalloc(sizeof(*mpeg4), GFP_KERNEL);
	if (!mpeg4)
		return -ENOMEM;

	sess->priv = mpeg4;

	/* Allocate some memory for the MPEG4 decoder's state */
	mpeg4->workspace_vaddr = dma_alloc_coherent(core->dev, SIZE_WORKSPACE, &mpeg4->workspace_paddr, GFP_KERNEL);
	if (!mpeg4->workspace_vaddr) {
		dev_err(core->dev, "Failed to request MPEG4 Workspace\n");
		ret = -ENOMEM;
		goto free_mpeg4;
	}

	writel_relaxed((1<<7) | (1<<6), core->dos_base + DOS_SW_RESET0);
	writel_relaxed(0, core->dos_base + DOS_SW_RESET0);
	readl_relaxed(core->dos_base + DOS_SW_RESET0);

	codec_mpeg4_set_canvases(sess);

	writel_relaxed(mpeg4->workspace_paddr - DCAC_BUFF_START_IP, core->dos_base + MEM_OFFSET_REG);
	writel_relaxed(0, core->dos_base + PSCALE_CTRL);
	writel_relaxed(0, core->dos_base + MP4_NOT_CODED_CNT);
	writel_relaxed(0, core->dos_base + MREG_BUFFERIN);
	writel_relaxed(0, core->dos_base + MREG_BUFFEROUT);
	writel_relaxed(0, core->dos_base + MREG_FATAL_ERROR);
	writel_relaxed(1, core->dos_base + ASSIST_MBOX1_CLR_REG);
	writel_relaxed(1, core->dos_base + ASSIST_MBOX1_MASK);
	writel_relaxed(0x404038aa, core->dos_base + MDEC_PIC_DC_THRESH);

	return 0;

free_mpeg4:
	kfree(mpeg4);
	return ret;
}

static int codec_mpeg4_stop(struct vdec_session *sess)
{
	struct codec_mpeg4 *mpeg4 = sess->priv;
	struct vdec_core *core = sess->core;

	if (mpeg4->workspace_vaddr) {
		dma_free_coherent(core->dev, SIZE_WORKSPACE, mpeg4->workspace_vaddr, mpeg4->workspace_paddr);
		mpeg4->workspace_vaddr = 0;
	}

	return 0;
}

static irqreturn_t codec_mpeg4_isr(struct vdec_session *sess)
{
	u32 reg;
	u32 buffer_index;
	struct vdec_core *core = sess->core;

	reg = readl_relaxed(core->dos_base + MREG_FATAL_ERROR);
	if (reg == 1)
		dev_err(core->dev, "mpeg4 fatal error\n");

	reg = readl_relaxed(core->dos_base + MREG_BUFFEROUT);
	if (reg) {
		sess->keyframe_found = 1;
		readl_relaxed(core->dos_base + MP4_NOT_CODED_CNT);
		readl_relaxed(core->dos_base + MP4_VOP_TIME_INC);
		buffer_index = reg & 0x7;
		vdec_dst_buf_done_idx(sess, buffer_index);
		writel_relaxed(0, core->dos_base + MREG_BUFFEROUT);
	}

	writel_relaxed(1, core->dos_base + ASSIST_MBOX1_CLR_REG);

	return IRQ_HANDLED;
}

struct vdec_codec_ops codec_mpeg4_ops = {
	.start = codec_mpeg4_start,
	.stop = codec_mpeg4_stop,
	.isr = codec_mpeg4_isr,
	.can_recycle = codec_mpeg4_can_recycle,
	.recycle = codec_mpeg4_recycle,
};

