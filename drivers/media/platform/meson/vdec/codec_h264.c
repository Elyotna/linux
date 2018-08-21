// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 */

#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "codec_h264.h"
#include "codec_helpers.h"
#include "canvas.h"
#include "dos_regs.h"

#define SIZE_EXT_FW	(20 * SZ_1K)
#define SIZE_WORKSPACE	0x1ee000
#define SIZE_SEI	(8 * SZ_1K)

/* Offset added by the firmware which must be substracted
 * from the workspace phyaddr
 */
#define WORKSPACE_BUF_OFFSET	0x1000000

#define SEI_DATA_READY BIT(15)

/* ISR status */
#define CMD_SET_PARAM		1
#define CMD_FRAMES_READY	2
#define CMD_FATAL_ERROR		6
#define CMD_BAD_WIDTH		7
#define CMD_BAD_HEIGHT		8

/* Size of Motion Vector per macroblock */
#define MB_MV_SIZE 96

struct codec_h264 {
	/* H.264 decoder requires an extended firmware */
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

static int codec_h264_can_recycle(struct amvdec_core *core)
{
	return !amvdec_read_dos(core, AV_SCRATCH_7) ||
	       !amvdec_read_dos(core, AV_SCRATCH_8);
}

static void codec_h264_recycle(struct amvdec_core *core, u32 buf_idx)
{
	/* Tell the decoder he can recycle this buffer.
	 * AV_SCRATCH_8 serves the same purpose.
	 */
	if (!amvdec_read_dos(core, AV_SCRATCH_7))
		amvdec_write_dos(core, AV_SCRATCH_7, buf_idx + 1);
	else
		amvdec_write_dos(core, AV_SCRATCH_8, buf_idx + 1);
}

static int codec_h264_start(struct amvdec_session *sess) {
	u32 workspace_offset;
	struct amvdec_core *core = sess->core;
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

	while (amvdec_read_dos(core, DCAC_DMA_CTRL) & 0x8000) { }
	while (amvdec_read_dos(core, LMEM_DMA_CTRL) & 0x8000) { }

	amvdec_write_dos(core, POWER_CTL_VLD, amvdec_read_dos(core, POWER_CTL_VLD) | (1 << 9) | (1 << 6));

	amvdec_write_dos(core, PSCALE_CTRL, 0);
	amvdec_write_dos(core, AV_SCRATCH_0, 0);

	workspace_offset = h264->workspace_paddr - WORKSPACE_BUF_OFFSET;
	amvdec_write_dos(core, AV_SCRATCH_1, workspace_offset);
	amvdec_write_dos(core, AV_SCRATCH_G, h264->ext_fw_paddr);
	amvdec_write_dos(core, AV_SCRATCH_I, h264->sei_paddr - workspace_offset);

	amvdec_write_dos(core, AV_SCRATCH_7, 0);
	amvdec_write_dos(core, AV_SCRATCH_8, 0);
	amvdec_write_dos(core, AV_SCRATCH_9, 0);

	/* Enable "error correction", don't know what it means */
	amvdec_write_dos(core, AV_SCRATCH_F, (amvdec_read_dos(core, AV_SCRATCH_F) & 0xffffffc3) | (1 << 4) | (1 << 7));

	amvdec_write_dos(core, MDEC_PIC_DC_THRESH, 0x404038aa);
	return 0;
}

static int codec_h264_stop(struct amvdec_session *sess)
{
	struct codec_h264 *h264 = sess->priv;
	struct amvdec_core *core = sess->core;

	if (h264->ext_fw_vaddr)
		dma_free_coherent(core->dev, SIZE_EXT_FW,
				  h264->ext_fw_vaddr, h264->ext_fw_paddr);
	
	if (h264->workspace_vaddr)
		dma_free_coherent(core->dev, SIZE_WORKSPACE,
				  h264->workspace_vaddr, h264->workspace_paddr);
	
	if (h264->ref_vaddr)
		dma_free_coherent(core->dev, h264->ref_size,
				  h264->ref_vaddr, h264->ref_paddr);
	
	if (h264->sei_vaddr)
		dma_free_coherent(core->dev, SIZE_SEI,
				  h264->sei_vaddr, h264->sei_paddr);
	
	return 0;
}

static int codec_h264_load_extended_firmware(struct amvdec_session *sess, const u8 *data, u32 len)
{
	struct codec_h264 *h264;
	struct amvdec_core *core = sess->core;
	
	h264 = kzalloc(sizeof(*h264), GFP_KERNEL);
	if (!h264)
		return -ENOMEM;
		
	sess->priv = h264;

	if (len != SIZE_EXT_FW)
		return -EINVAL;
	
	h264->ext_fw_vaddr = dma_alloc_coherent(core->dev, SIZE_EXT_FW,
						&h264->ext_fw_paddr, GFP_KERNEL);
	if (!h264->ext_fw_vaddr) {
		dev_err(core->dev,
			"Couldn't allocate memory for H.264 extended fw\n");
		return -ENOMEM;
	}

	memcpy(h264->ext_fw_vaddr, data, SIZE_EXT_FW);

	return 0;
}

/* Configure the H.264 decoder when the esparser finished parsing
 * the first keyframe
 */
static void codec_h264_set_param(struct amvdec_session *sess) {
	struct amvdec_core *core = sess->core;
	struct codec_h264 *h264 = sess->priv;
	u32 max_reference_size;
	u32 parsed_info, mb_width, mb_height, mb_total;
	u32 actual_dpb_size = v4l2_m2m_num_dst_bufs_ready(sess->m2m_ctx);
	u32 max_dpb_size = 4;

	sess->keyframe_found = 1;

	amvdec_write_dos(core, AV_SCRATCH_7, 0);
	amvdec_write_dos(core, AV_SCRATCH_8, 0);
	amvdec_write_dos(core, AV_SCRATCH_9, 0);

	parsed_info = amvdec_read_dos(core, AV_SCRATCH_1);

	/* Total number of 16x16 macroblocks */
	mb_total = (parsed_info >> 8) & 0xffff;

	/* Number of macroblocks per line */
	mb_width = parsed_info & 0xff;

	/* Number of macroblock lines */
	mb_height = mb_total / mb_width;

	max_reference_size = (parsed_info >> 24) & 0x7f;

	/* Align to a multiple of 4 macroblocks */
	mb_width = ALIGN(mb_width, 4);
	mb_height = ALIGN(mb_height, 4);
	mb_total = mb_width * mb_height;

	amcodec_helper_set_canvases(sess, core->dos_base + ANC0_CANVAS_ADDR);

	if (max_reference_size > max_dpb_size)
		max_dpb_size = max_reference_size;

	max_reference_size++;
	dev_dbg(core->dev,
		"max_ref_size = %u; max_dpb_size = %u; actual_dpb_size = %u\n",
		max_reference_size, max_dpb_size, actual_dpb_size);

	h264->ref_size = mb_total * MB_MV_SIZE * max_reference_size;
	h264->ref_vaddr = dma_alloc_coherent(core->dev, h264->ref_size,
					     &h264->ref_paddr, GFP_KERNEL);
	if (!h264->ref_vaddr) {
		dev_err(core->dev, "Failed to allocate memory for refs (%u)\n",
			h264->ref_size);
		amvdec_abort(sess);
		return;
	}

	/* Address to store the references' MVs */
	amvdec_write_dos(core, AV_SCRATCH_1, h264->ref_paddr);
	/* End of ref MV */
	amvdec_write_dos(core, AV_SCRATCH_4, h264->ref_paddr + h264->ref_size);

	amvdec_write_dos(core, AV_SCRATCH_0, (max_reference_size << 24) |
					     (actual_dpb_size << 16) |
					     (max_dpb_size << 8));
}

static void codec_h264_frames_ready(struct amvdec_session *sess, u32 status)
{
	struct amvdec_core *core = sess->core;
	int error_count;
	int error;
	int num_frames;
	int frame_status;
	unsigned int buffer_index;
	int i;

	error_count = amvdec_read_dos(core, AV_SCRATCH_D);
	num_frames = (status >> 8) & 0xff;
	if (error_count) {
		dev_warn(core->dev,
			"decoder error(s) happened, count %d\n", error_count);
		amvdec_write_dos(core, AV_SCRATCH_D, 0);
	}

	for (i = 0; i < num_frames; i++) {
		frame_status = amvdec_read_dos(core, AV_SCRATCH_1 + i*4);
		buffer_index = frame_status & 0x1f;
		error = frame_status & 0x200;

		/* A buffer decode error means it was decoded,
		 * but part of the picture will have artifacts.
		 * Typical reason is a temporarily corrupted bitstream
		 */
		if (error)
			dev_info(core->dev, "Buffer %d decode error\n",
				 buffer_index);

		amvdec_dst_buf_done_idx(sess, buffer_index);
	}
}

static irqreturn_t codec_h264_threaded_isr(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;
	u32 status;
	u32 size;
	u8 cmd;

	status = amvdec_read_dos(core, AV_SCRATCH_0);
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
		size = (amvdec_read_dos(core, AV_SCRATCH_1) + 1) * 16;
		dev_err(core->dev, "Unsupported video width: %u\n", size);
		goto abort;
	case CMD_BAD_HEIGHT:
		size = (amvdec_read_dos(core, AV_SCRATCH_1) + 1) * 16;
		dev_err(core->dev, "Unsupported video height: %u\n", size);
		goto abort;
	case 9: /* Unused but not worth printing for */
		break;
	default:
		dev_info(core->dev, "Unexpected H264 ISR: %08X\n", cmd);
		break;
	}

	if (cmd != CMD_SET_PARAM)
		amvdec_write_dos(core, AV_SCRATCH_0, 0);

	/* Decoder has some SEI data for us ; ignore */
	if (amvdec_read_dos(core, AV_SCRATCH_J) & SEI_DATA_READY)
		amvdec_write_dos(core, AV_SCRATCH_J, 0);

	return IRQ_HANDLED;
abort:
	amvdec_abort(sess);
	return IRQ_HANDLED;
}

static irqreturn_t codec_h264_isr(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;

	amvdec_write_dos(core, ASSIST_MBOX1_CLR_REG, 1);

	return IRQ_WAKE_THREAD;
}

struct amvdec_codec_ops codec_h264_ops = {
	.start = codec_h264_start,
	.stop = codec_h264_stop,
	.load_extended_firmware = codec_h264_load_extended_firmware,
	.isr = codec_h264_isr,
	.threaded_isr = codec_h264_threaded_isr,
	.can_recycle = codec_h264_can_recycle,
	.recycle = codec_h264_recycle,
};

