// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 */

#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "codec_mpeg4.h"
#include "codec_helpers.h"
#include "canvas.h"
#include "dos_regs.h"

#define SIZE_WORKSPACE		SZ_1M
#define DCAC_BUFF_START_IP	0x02b00000

/* map FW registers to known MPEG4 functions */
#define MP4_PIC_RATIO       AV_SCRATCH_5
#define MP4_ERR_COUNT       AV_SCRATCH_6
#define MP4_PIC_WH          AV_SCRATCH_7
#define MREG_BUFFERIN       AV_SCRATCH_8
#define MREG_BUFFEROUT      AV_SCRATCH_9
#define MP4_NOT_CODED_CNT   AV_SCRATCH_A
#define MP4_VOP_TIME_INC    AV_SCRATCH_B
#define MP4_OFFSET_REG      AV_SCRATCH_C
#define MP4_SYS_RATE        AV_SCRATCH_E
#define MEM_OFFSET_REG      AV_SCRATCH_F
#define MREG_FATAL_ERROR    AV_SCRATCH_L

struct codec_mpeg4 {
	/* Buffer for the MPEG4 Workspace */
	void      *workspace_vaddr;
	dma_addr_t workspace_paddr;
};

static int codec_mpeg4_can_recycle(struct amvdec_core *core)
{
	return !readl_relaxed(core->dos_base + MREG_BUFFERIN);
}

static void codec_mpeg4_recycle(struct amvdec_core *core, u32 buf_idx)
{
	amvdec_write_dos(core, MREG_BUFFERIN, ~(1 << buf_idx));
}

/* The MPEG4 canvas regs are not contiguous,
 * handle it specifically instead of using the helper
 * AV_SCRATCH_0 - AV_SCRATCH_3  ;  AV_SCRATCH_G - AV_SCRATCH_J
 */
void codec_mpeg4_set_canvases(struct amvdec_session *sess) {
	struct v4l2_m2m_buffer *buf;
	struct amvdec_core *core = sess->core;
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

static int codec_mpeg4_start(struct amvdec_session *sess) {
	struct amvdec_core *core = sess->core;
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

	amvdec_write_dos(core, DOS_SW_RESET0, (1<<7) | (1<<6));
	amvdec_write_dos(core, DOS_SW_RESET0, 0);
	readl_relaxed(core->dos_base + DOS_SW_RESET0);

	codec_mpeg4_set_canvases(sess);

	amvdec_write_dos(core, MEM_OFFSET_REG, mpeg4->workspace_paddr - DCAC_BUFF_START_IP);
	amvdec_write_dos(core, PSCALE_CTRL, 0);
	amvdec_write_dos(core, MP4_NOT_CODED_CNT, 0);
	amvdec_write_dos(core, MREG_BUFFERIN, 0);
	amvdec_write_dos(core, MREG_BUFFEROUT, 0);
	amvdec_write_dos(core, MREG_FATAL_ERROR, 0);
	amvdec_write_dos(core, ASSIST_MBOX1_CLR_REG, 1);
	amvdec_write_dos(core, ASSIST_MBOX1_MASK, 1);
	amvdec_write_dos(core, MDEC_PIC_DC_THRESH, 0x404038aa);

	return 0;

free_mpeg4:
	kfree(mpeg4);
	return ret;
}

static int codec_mpeg4_stop(struct amvdec_session *sess)
{
	struct codec_mpeg4 *mpeg4 = sess->priv;
	struct amvdec_core *core = sess->core;

	if (mpeg4->workspace_vaddr) {
		dma_free_coherent(core->dev, SIZE_WORKSPACE, mpeg4->workspace_vaddr, mpeg4->workspace_paddr);
		mpeg4->workspace_vaddr = 0;
	}

	return 0;
}

static irqreturn_t codec_mpeg4_isr(struct amvdec_session *sess)
{
	u32 reg;
	u32 buffer_index;
	struct amvdec_core *core = sess->core;

	reg = readl_relaxed(core->dos_base + MREG_FATAL_ERROR);
	if (reg == 1)
		dev_err(core->dev, "mpeg4 fatal error\n");

	reg = readl_relaxed(core->dos_base + MREG_BUFFEROUT);
	if (reg) {
		sess->keyframe_found = 1;
		readl_relaxed(core->dos_base + MP4_NOT_CODED_CNT);
		readl_relaxed(core->dos_base + MP4_VOP_TIME_INC);
		buffer_index = reg & 0x7;
		amvdec_dst_buf_done_idx(sess, buffer_index);
		amvdec_write_dos(core, MREG_BUFFEROUT, 0);
	}

	amvdec_write_dos(core, ASSIST_MBOX1_CLR_REG, 1);

	return IRQ_HANDLED;
}

struct amvdec_codec_ops codec_mpeg4_ops = {
	.start = codec_mpeg4_start,
	.stop = codec_mpeg4_stop,
	.isr = codec_mpeg4_isr,
	.can_recycle = codec_mpeg4_can_recycle,
	.recycle = codec_mpeg4_recycle,
};

