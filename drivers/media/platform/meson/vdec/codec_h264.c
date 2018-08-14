// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 */

#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "codec_h264.h"
#include "codec_helpers.h"
#include "canvas.h"

#define SIZE_EXT_FW	(20 * SZ_1K)
#define SIZE_WORKSPACE	0x1ee000
#define SIZE_SEI	(8 * SZ_1K)

/* Offset added by the firmware which must be substracted
 * from the workspace phyaddr
 */
#define WORKSPACE_BUF_OFFSET	0x1000000

/* DOS registers */
#define ASSIST_MBOX1_CLR_REG	0x01d4
#define ASSIST_MBOX1_MASK	0x01d8

#define LMEM_DMA_CTRL		0x0d40

#define PSCALE_CTRL		0x2444

#define MDEC_PIC_DC_CTRL	0x2638
#define ANC0_CANVAS_ADDR	0x2640
#define MDEC_PIC_DC_THRESH	0x26e0

#define AV_SCRATCH_0		0x2700
#define AV_SCRATCH_1		0x2704
#define AV_SCRATCH_2		0x2708
#define AV_SCRATCH_3		0x270c
#define AV_SCRATCH_4		0x2710
#define AV_SCRATCH_5		0x2714
#define AV_SCRATCH_6		0x2718
#define AV_SCRATCH_7		0x271c
#define AV_SCRATCH_8		0x2720
#define AV_SCRATCH_9		0x2724
#define AV_SCRATCH_D		0x2734
#define AV_SCRATCH_F		0x273c
#define AV_SCRATCH_G		0x2740
#define AV_SCRATCH_H		0x2744
#define AV_SCRATCH_I		0x2748
#define AV_SCRATCH_J		0x274c
	#define SEI_DATA_READY BIT(15)

#define POWER_CTL_VLD		0x3020

#define DCAC_DMA_CTRL		0x3848

#define DOS_SW_RESET0		0xfc00

/* ISR status */
#define CMD_SET_PARAM		1
#define CMD_FRAMES_READY	2
#define CMD_FATAL_ERROR		6
#define CMD_BAD_WIDTH		7
#define CMD_BAD_HEIGHT		8

struct codec_h264 {
	/* H.264 decoder requires an extended firmware loaded in contiguous RAM */
	void      *ext_fw_vaddr;
	dma_addr_t ext_fw_paddr;

	/* Buffer for the H.264 Workspace */
	void      *workspace_vaddr;
	dma_addr_t workspace_paddr;
	
	/* Buffer for the H.264 references MV */
	void      *ref_vaddr;
	dma_addr_t ref_paddr;
	u32	   ref_size;

	/* Buffer for parsed SEI data */
	void      *sei_vaddr;
	dma_addr_t sei_paddr;
};

static int codec_h264_can_recycle(struct vdec_core *core)
{
	return !readl_relaxed(core->dos_base + AV_SCRATCH_7) ||
	       !readl_relaxed(core->dos_base + AV_SCRATCH_8);
}

static void codec_h264_recycle(struct vdec_core *core, u32 buf_idx)
{
	/* Tell the decoder he can recycle this buffer.
	 * AV_SCRATCH_8 serves the same purpose.
	 */
	if (!readl_relaxed(core->dos_base + AV_SCRATCH_7))
		writel_relaxed(buf_idx + 1, core->dos_base + AV_SCRATCH_7);
	else
		writel_relaxed(buf_idx + 1, core->dos_base + AV_SCRATCH_8);
}

static int codec_h264_start(struct vdec_session *sess) {
	u32 workspace_offset;
	struct vdec_core *core = sess->core;
	struct codec_h264 *h264 = sess->priv;

	/* Allocate some memory for the H.264 decoder's state */
	h264->workspace_vaddr =
		dma_alloc_coherent(core->dev, SIZE_WORKSPACE, &h264->workspace_paddr, GFP_KERNEL);
	if (!h264->workspace_vaddr) {
		dev_err(core->dev, "Failed to request H.264 Workspace\n");
		return -ENOMEM;
	}

	/* Allocate some memory for the H.264 SEI dump */
	h264->sei_vaddr =
		dma_alloc_coherent(core->dev, SIZE_SEI, &h264->sei_paddr, GFP_KERNEL);
	if (!h264->sei_vaddr) {
		dev_err(core->dev, "Failed to request H.264 SEI\n");
		return -ENOMEM;
	}

	while (readl_relaxed(core->dos_base + DCAC_DMA_CTRL) & 0x8000) { }
	while (readl_relaxed(core->dos_base + LMEM_DMA_CTRL) & 0x8000) { }

	writel_relaxed((1<<7) | (1<<6) | (1<<4), core->dos_base + DOS_SW_RESET0);
	writel_relaxed(0, core->dos_base + DOS_SW_RESET0);
	readl_relaxed(core->dos_base + DOS_SW_RESET0);

	writel_relaxed((1<<7) | (1<<6) | (1<<4), core->dos_base + DOS_SW_RESET0);
	writel_relaxed(0, core->dos_base + DOS_SW_RESET0);
	writel_relaxed((1<<9) | (1<<8), core->dos_base + DOS_SW_RESET0);
	writel_relaxed(0, core->dos_base + DOS_SW_RESET0);
	readl_relaxed(core->dos_base + DOS_SW_RESET0);

	writel_relaxed(readl_relaxed(core->dos_base + POWER_CTL_VLD) | (1 << 9) | (1 << 6), core->dos_base + POWER_CTL_VLD);

	writel_relaxed(0, core->dos_base + PSCALE_CTRL);
	writel_relaxed(0, core->dos_base + AV_SCRATCH_0);

	workspace_offset = h264->workspace_paddr - WORKSPACE_BUF_OFFSET;
	writel_relaxed(workspace_offset, core->dos_base + AV_SCRATCH_1);
	writel_relaxed(h264->ext_fw_paddr, core->dos_base + AV_SCRATCH_G);
	writel_relaxed(h264->sei_paddr - workspace_offset, core->dos_base + AV_SCRATCH_I);

	writel_relaxed(0, core->dos_base + AV_SCRATCH_7);
	writel_relaxed(0, core->dos_base + AV_SCRATCH_8);
	writel_relaxed(0, core->dos_base + AV_SCRATCH_9);

	/* Enable "error correction", don't know what it means */
	writel_relaxed((readl_relaxed(core->dos_base + AV_SCRATCH_F) & 0xffffffc3) | (1 << 4) | (1 << 7), core->dos_base + AV_SCRATCH_F);

	/* Enable IRQ */
	writel_relaxed(1, core->dos_base + ASSIST_MBOX1_CLR_REG);
	writel_relaxed(1, core->dos_base + ASSIST_MBOX1_MASK);

	writel_relaxed(0x404038aa, core->dos_base + MDEC_PIC_DC_THRESH);
	
	writel_relaxed((1<<12)|(1<<11), core->dos_base + DOS_SW_RESET0);
	writel_relaxed(0, core->dos_base + DOS_SW_RESET0);

	readl_relaxed(core->dos_base + DOS_SW_RESET0);
	return 0;
}

static int codec_h264_stop(struct vdec_session *sess)
{
	struct codec_h264 *h264 = sess->priv;
	struct vdec_core *core = sess->core;

	if (h264->ext_fw_vaddr)
		dma_free_coherent(core->dev, SIZE_EXT_FW, h264->ext_fw_vaddr, h264->ext_fw_paddr);
	
	if (h264->workspace_vaddr)
		dma_free_coherent(core->dev, SIZE_WORKSPACE, h264->workspace_vaddr, h264->workspace_paddr);
	
	if (h264->ref_vaddr)
		dma_free_coherent(core->dev, h264->ref_size, h264->ref_vaddr, h264->ref_paddr);
	
	if (h264->sei_vaddr)
		dma_free_coherent(core->dev, SIZE_SEI, h264->sei_vaddr, h264->sei_paddr);
	
	return 0;
}

static int codec_h264_load_extended_firmware(struct vdec_session *sess, const u8 *data, u32 len)
{
	struct codec_h264 *h264;
	struct vdec_core *core = sess->core;
	
	h264 = kzalloc(sizeof(*h264), GFP_KERNEL);
	if (!h264)
		return -ENOMEM;
		
	sess->priv = h264;

	if (len != SIZE_EXT_FW)
		return -EINVAL;
	
	h264->ext_fw_vaddr = dma_alloc_coherent(core->dev, SIZE_EXT_FW, &h264->ext_fw_paddr, GFP_KERNEL);
	if (!h264->ext_fw_vaddr) {
		dev_err(core->dev, "Couldn't allocate memory for H.264 extended firmware\n");
		return -ENOMEM;
	}

	memcpy(h264->ext_fw_vaddr, data, SIZE_EXT_FW);

	return 0;
}

/* Configure the H.264 decoder when the esparser finished parsing
 * the first buffer.
 */
static void codec_h264_set_param(struct vdec_session *sess) {
	u32 max_reference_size;
	u32 parsed_info, mb_width, mb_height, mb_total;
	u32 mb_mv_byte;
	u32 actual_dpb_size = v4l2_m2m_num_dst_bufs_ready(sess->m2m_ctx);
	u32 max_dpb_size = 4;
	struct vdec_core *core = sess->core;
	struct codec_h264 *h264 = sess->priv;

	sess->keyframe_found = 1;

	writel_relaxed(0, core->dos_base + AV_SCRATCH_7);
	writel_relaxed(0, core->dos_base + AV_SCRATCH_8);
	writel_relaxed(0, core->dos_base + AV_SCRATCH_9);

	parsed_info = readl_relaxed(core->dos_base + AV_SCRATCH_1);

	/* Total number of 16x16 macroblocks */
	mb_total = (parsed_info >> 8) & 0xffff;

	/* Size of Motion Vector per macroblock ? */
	mb_mv_byte = 96;

	/* Number of macroblocks per line */
	mb_width = parsed_info & 0xff;

	/* Number of macroblock lines */
	mb_height = mb_total / mb_width;

	max_reference_size = (parsed_info >> 24) & 0x7f;

	/* Align to a multiple of 4 macroblocks */
	mb_width = (mb_width + 3) & 0xfffffffc;
	mb_height = (mb_height + 3) & 0xfffffffc;
	mb_total = mb_width * mb_height;

	codec_helper_set_canvases(sess, core->dos_base + ANC0_CANVAS_ADDR);

	if (max_reference_size > max_dpb_size)
		max_dpb_size = max_reference_size;

	max_reference_size++;
	dev_dbg(core->dev,
		"max_ref_size = %u; max_dpb_size = %u; actual_dpb_size = %u\n",
		max_reference_size, max_dpb_size, actual_dpb_size);

	h264->ref_size = mb_total * mb_mv_byte * max_reference_size;
	h264->ref_vaddr = dma_alloc_coherent(core->dev, h264->ref_size, &h264->ref_paddr, GFP_KERNEL);
	if (!h264->ref_vaddr) {
		dev_err(core->dev, "Failed to allocate memory for refs (%u)\n", h264->ref_size);
		vdec_abort(sess);
		return;
	}

	/* Address to store the references' MVs ? */
	writel_relaxed(h264->ref_paddr, core->dos_base + AV_SCRATCH_1);
	/* End of ref MV */
	writel_relaxed(h264->ref_paddr + h264->ref_size, core->dos_base + AV_SCRATCH_4);
	writel_relaxed((max_reference_size << 24) | (actual_dpb_size << 16) | (max_dpb_size << 8), core->dos_base + AV_SCRATCH_0);
}

static void codec_h264_frames_ready(struct vdec_session *sess, u32 status)
{
	struct vdec_core *core = sess->core;
	int error_count;
	int error;
	int num_frames;
	int frame_status;
	unsigned int buffer_index;
	int i;

	error_count = readl_relaxed(core->dos_base + AV_SCRATCH_D);
	num_frames = (status >> 8) & 0xff;
	if (error_count) {
		dev_warn(core->dev,
			"decoder error(s) happened, count %d\n", error_count);
		writel_relaxed(0, core->dos_base + AV_SCRATCH_D);
	}

	for (i = 0; i < num_frames; i++) {
		frame_status = readl_relaxed(core->dos_base + AV_SCRATCH_1 + i*4);
		buffer_index = frame_status & 0x1f;
		error = frame_status & 0x200;

		/* A buffer decode error means it was decoded,
		 * but part of the picture will have artifacts.
		 * Typical reason is a temporarily corrupted bitstream
		 */
		if (error)
			dev_info(core->dev, "Buffer %d decode error\n",
				 buffer_index);

		//printk("done %d/%d: %d\n", i, num_frames, buffer_index);
		vdec_dst_buf_done_idx(sess, buffer_index);
	}
}

static irqreturn_t codec_h264_threaded_isr(struct vdec_session *sess)
{
	struct vdec_core *core = sess->core;
	u32 status;
	u32 size;
	u8 cmd;

	status = readl_relaxed(core->dos_base + AV_SCRATCH_0);
	cmd = status & 0xff;

	switch (cmd) {
	case CMD_SET_PARAM:
		codec_h264_set_param(sess);
		break;
	case CMD_FRAMES_READY:
		codec_h264_frames_ready(sess, status);
		break;
	case CMD_FATAL_ERROR:
		dev_err(core->dev, "H.264 decoder fatal error\n");
		goto abort;
	case CMD_BAD_WIDTH:
		size = (readl_relaxed(core->dos_base + AV_SCRATCH_1) + 1) * 16;
		dev_err(core->dev, "Unsupported video width: %u\n", size);
		goto abort;
	case CMD_BAD_HEIGHT:
		size = (readl_relaxed(core->dos_base + AV_SCRATCH_1) + 1) * 16;
		dev_err(core->dev, "Unsupported video height: %u\n", size);
		goto abort;
	case 9: /* Unused but not worth printing for */
		break;
	default:
		dev_info(core->dev, "Unexpected H264 ISR: %08X\n", cmd);
		break;
	}

	if (cmd != CMD_SET_PARAM)
		writel_relaxed(0, core->dos_base + AV_SCRATCH_0);

	/* Decoder has some SEI data for us ; ignore */
	if (readl_relaxed(core->dos_base + AV_SCRATCH_J) & SEI_DATA_READY)
		writel_relaxed(0, core->dos_base + AV_SCRATCH_J);

	return IRQ_HANDLED;
abort:
	vdec_abort(sess);
	return IRQ_HANDLED;
}

static irqreturn_t codec_h264_isr(struct vdec_session *sess)
{
	struct vdec_core *core = sess->core;

	writel_relaxed(1, core->dos_base + ASSIST_MBOX1_CLR_REG);

	return IRQ_WAKE_THREAD;
}

struct vdec_codec_ops codec_h264_ops = {
	.start = codec_h264_start,
	.stop = codec_h264_stop,
	.load_extended_firmware = codec_h264_load_extended_firmware,
	.isr = codec_h264_isr,
	.threaded_isr = codec_h264_threaded_isr,
	.can_recycle = codec_h264_can_recycle,
	.recycle = codec_h264_recycle,
};

