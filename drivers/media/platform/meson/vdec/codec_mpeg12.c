// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 */

#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "codec_mpeg12.h"
#include "codec_helpers.h"
#include "dos_regs.h"

#define SIZE_WORKSPACE	(2 * SZ_64K)
#define SIZE_CCBUF	(5 * SZ_1K)

/* map FW registers to known MPEG1/2 functions */
#define MREG_SEQ_INFO		AV_SCRATCH_4
#define MREG_PIC_INFO		AV_SCRATCH_5
#define MREG_PIC_WIDTH		AV_SCRATCH_6
#define MREG_PIC_HEIGHT		AV_SCRATCH_7
#define MREG_BUFFERIN		AV_SCRATCH_8
#define MREG_BUFFEROUT		AV_SCRATCH_9
#define MREG_CMD		AV_SCRATCH_A
#define MREG_CO_MV_START	AV_SCRATCH_B
#define MREG_ERROR_COUNT	AV_SCRATCH_C
#define MREG_FRAME_OFFSET	AV_SCRATCH_D
#define MREG_WAIT_BUFFER	AV_SCRATCH_E
#define MREG_FATAL_ERROR	AV_SCRATCH_F

struct codec_mpeg12 {
	/* Buffer for the MPEG1/2 Workspace */
	void      *workspace_vaddr;
	dma_addr_t workspace_paddr;
};

static int codec_mpeg12_can_recycle(struct amvdec_core *core)
{
	return !amvdec_read_dos(core, MREG_BUFFERIN);
}

static void codec_mpeg12_recycle(struct amvdec_core *core, u32 buf_idx)
{
	amvdec_write_dos(core, MREG_BUFFERIN, buf_idx + 1);
}

static int codec_mpeg12_start(struct amvdec_session *sess) {
	struct amvdec_core *core = sess->core;
	struct codec_mpeg12 *mpeg12 = sess->priv;
	int ret;

	mpeg12 = kzalloc(sizeof(*mpeg12), GFP_KERNEL);
	if (!mpeg12)
		return -ENOMEM;

	sess->priv = mpeg12;

	/* Allocate some memory for the MPEG1/2 decoder's state */
	mpeg12->workspace_vaddr = dma_alloc_coherent(core->dev, SIZE_WORKSPACE, &mpeg12->workspace_paddr, GFP_KERNEL);
	if (!mpeg12->workspace_vaddr) {
		dev_err(core->dev, "Failed to request MPEG 1/2 Workspace\n");
		ret = -ENOMEM;
		goto free_mpeg12;
	}

	amvdec_write_dos(core, POWER_CTL_VLD, (1 << 4));

	amcodec_helper_set_canvases(sess, core->dos_base + AV_SCRATCH_0);
	amvdec_write_dos(core, MREG_CO_MV_START, mpeg12->workspace_paddr + SIZE_CCBUF);

	amvdec_write_dos(core, MPEG1_2_REG, 0);
	amvdec_write_dos(core, PSCALE_CTRL, 0);
	amvdec_write_dos(core, PIC_HEAD_INFO, 0x380);
	amvdec_write_dos(core, M4_CONTROL_REG, 0);
	amvdec_write_dos(core, MREG_BUFFERIN, 0);
	amvdec_write_dos(core, MREG_BUFFEROUT, 0);
	amvdec_write_dos(core, MREG_CMD, (sess->width << 16) | sess->height);
	amvdec_write_dos(core, MREG_ERROR_COUNT, 0);
	amvdec_write_dos(core, MREG_FATAL_ERROR, 0);
	amvdec_write_dos(core, MREG_WAIT_BUFFER, 0);

	return 0;

free_mpeg12:
	kfree(mpeg12);
	return ret;
}

static int codec_mpeg12_stop(struct amvdec_session *sess)
{
	struct codec_mpeg12 *mpeg12 = sess->priv;
	struct amvdec_core *core = sess->core;

	if (mpeg12->workspace_vaddr) {
		dma_free_coherent(core->dev, SIZE_WORKSPACE, mpeg12->workspace_vaddr, mpeg12->workspace_paddr);
		mpeg12->workspace_vaddr = 0;
	}

	return 0;
}

static irqreturn_t codec_mpeg12_isr(struct amvdec_session *sess)
{
	u32 reg;
	u32 buffer_index;
	struct amvdec_core *core = sess->core;

	amvdec_write_dos(core, ASSIST_MBOX1_CLR_REG, 1);

	reg = amvdec_read_dos(core, MREG_FATAL_ERROR);
	if (reg == 1)
		dev_err(core->dev, "MPEG12 fatal error\n");

	reg = amvdec_read_dos(core, MREG_BUFFEROUT);
	if (!reg)
		return IRQ_HANDLED;

	if ((reg >> 16) & 0xfe)
		goto end;

	sess->keyframe_found = 1;
	buffer_index = ((reg & 0xf) - 1) & 7;
	amvdec_dst_buf_done_idx(sess, buffer_index);

end:
	amvdec_write_dos(core, MREG_BUFFEROUT, 0);
	return IRQ_HANDLED;
}

struct amvdec_codec_ops codec_mpeg12_ops = {
	.start = codec_mpeg12_start,
	.stop = codec_mpeg12_stop,
	.isr = codec_mpeg12_isr,
	.can_recycle = codec_mpeg12_can_recycle,
	.recycle = codec_mpeg12_recycle,
};

