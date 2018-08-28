// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 */

#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "vdec_helpers.h"
#include "dos_regs.h"

#define SIZE_WORKSPACE		SZ_1M
/* Offset added by firmware, to substract from workspace paddr */
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
	return !amvdec_read_dos(core, MREG_BUFFERIN);
}

static void codec_mpeg4_recycle(struct amvdec_core *core, u32 buf_idx)
{
	amvdec_write_dos(core, MREG_BUFFERIN, ~(1 << buf_idx));
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
	mpeg4->workspace_vaddr = dma_alloc_coherent(core->dev, SIZE_WORKSPACE,
						    &mpeg4->workspace_paddr,
						    GFP_KERNEL);
	if (!mpeg4->workspace_vaddr) {
		dev_err(core->dev, "Failed to request MPEG4 Workspace\n");
		ret = -ENOMEM;
		goto free_mpeg4;
	}

	amvdec_set_canvases(sess, (u32[]){ AV_SCRATCH_0, AV_SCRATCH_G, 0 },
				  (u32[]){ 4, 4, 0 });

	amvdec_write_dos(core, MEM_OFFSET_REG,
			 mpeg4->workspace_paddr - DCAC_BUFF_START_IP);
	amvdec_write_dos(core, PSCALE_CTRL, 0);
	amvdec_write_dos(core, MP4_NOT_CODED_CNT, 0);
	amvdec_write_dos(core, MREG_BUFFERIN, 0);
	amvdec_write_dos(core, MREG_BUFFEROUT, 0);
	amvdec_write_dos(core, MREG_FATAL_ERROR, 0);
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
		dma_free_coherent(core->dev, SIZE_WORKSPACE,
				  mpeg4->workspace_vaddr,
				  mpeg4->workspace_paddr);
		mpeg4->workspace_vaddr = 0;
	}

	return 0;
}

static irqreturn_t codec_mpeg4_isr(struct amvdec_session *sess)
{
	u32 reg;
	u32 buffer_index;
	struct amvdec_core *core = sess->core;

	reg = amvdec_read_dos(core, MREG_FATAL_ERROR);
	if (reg == 1)
		dev_err(core->dev, "mpeg4 fatal error\n");

	reg = amvdec_read_dos(core, MREG_BUFFEROUT);
	if (reg) {
		sess->keyframe_found = 1;
		amvdec_read_dos(core, MP4_NOT_CODED_CNT);
		amvdec_read_dos(core, MP4_VOP_TIME_INC);
		buffer_index = reg & 0x7;
		amvdec_dst_buf_done_idx(sess, buffer_index, V4L2_FIELD_NONE);
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
