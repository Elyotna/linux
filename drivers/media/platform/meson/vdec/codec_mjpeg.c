// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 */

#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "codec_mjpeg.h"
#include "codec_helpers.h"

/* DOS registers */
#define VDEC_ASSIST_AMR1_INT8	0x00b4

#define ASSIST_MBOX1_CLR_REG	0x01d4
#define ASSIST_MBOX1_MASK	0x01d8

#define MCPU_INTR_MSK		0x0c10

#define PSCALE_RST		0x2440
#define PSCALE_CTRL		0x2444
#define PSCALE_BMEM_ADDR	0x247c
#define PSCALE_BMEM_DAT		0x2480

#define MDEC_PIC_DC_CTRL	0x2638

#define AV_SCRATCH_0		0x2700
#define AV_SCRATCH_1		0x2704
#define MREG_DECODE_PARAM	0x2708
#define AV_SCRATCH_4		0x2710
#define MREG_TO_AMRISC		0x2720
#define MREG_FROM_AMRISC	0x2724

#define DOS_SW_RESET0		0xfc00

static int codec_mjpeg_can_recycle(struct vdec_core *core)
{
	return !readl_relaxed(core->dos_base + MREG_TO_AMRISC);
}

static void codec_mjpeg_recycle(struct vdec_core *core, u32 buf_idx)
{
	writel_relaxed(buf_idx + 1, core->dos_base + MREG_TO_AMRISC);
}

/* 4 point triangle */
static const uint32_t filt_coef[] = {
	0x20402000, 0x20402000, 0x1f3f2101, 0x1f3f2101,
	0x1e3e2202, 0x1e3e2202, 0x1d3d2303, 0x1d3d2303,
	0x1c3c2404, 0x1c3c2404, 0x1b3b2505, 0x1b3b2505,
	0x1a3a2606, 0x1a3a2606, 0x19392707, 0x19392707,
	0x18382808, 0x18382808, 0x17372909, 0x17372909,
	0x16362a0a, 0x16362a0a, 0x15352b0b, 0x15352b0b,
	0x14342c0c, 0x14342c0c, 0x13332d0d, 0x13332d0d,
	0x12322e0e, 0x12322e0e, 0x11312f0f, 0x11312f0f,
	0x10303010
};

static void codec_mjpeg_init_scaler(struct vdec_core *core)
{
	int i;

	/* PSCALE cbus bmem enable */
	writel_relaxed(0xc000, core->dos_base + PSCALE_CTRL);

	writel_relaxed(0, core->dos_base + PSCALE_BMEM_ADDR);
	for (i = 0; i < ARRAY_SIZE(filt_coef); ++i) {
		writel_relaxed(0, core->dos_base + PSCALE_BMEM_DAT);
		writel_relaxed(filt_coef[i], core->dos_base + PSCALE_BMEM_DAT);
	}

	writel_relaxed(74, core->dos_base + PSCALE_BMEM_ADDR);
	writel_relaxed(0x0008, core->dos_base + PSCALE_BMEM_DAT);
	writel_relaxed(0x60000000, core->dos_base + PSCALE_BMEM_DAT);

	writel_relaxed(82, core->dos_base + PSCALE_BMEM_ADDR);
	writel_relaxed(0x0008, core->dos_base + PSCALE_BMEM_DAT);
	writel_relaxed(0x60000000, core->dos_base + PSCALE_BMEM_DAT);

	writel_relaxed(78, core->dos_base + PSCALE_BMEM_ADDR);
	writel_relaxed(0x0008, core->dos_base + PSCALE_BMEM_DAT);
	writel_relaxed(0x60000000, core->dos_base + PSCALE_BMEM_DAT);

	writel_relaxed(86, core->dos_base + PSCALE_BMEM_ADDR);
	writel_relaxed(0x0008, core->dos_base + PSCALE_BMEM_DAT);
	writel_relaxed(0x60000000, core->dos_base + PSCALE_BMEM_DAT);

	writel_relaxed(73, core->dos_base + PSCALE_BMEM_ADDR);
	writel_relaxed(0x10000, core->dos_base + PSCALE_BMEM_DAT);
	writel_relaxed(81, core->dos_base + PSCALE_BMEM_ADDR);
	writel_relaxed(0x10000, core->dos_base + PSCALE_BMEM_DAT);

	writel_relaxed(77, core->dos_base + PSCALE_BMEM_ADDR);
	writel_relaxed(0x10000, core->dos_base + PSCALE_BMEM_DAT);
	writel_relaxed(85, core->dos_base + PSCALE_BMEM_ADDR);
	writel_relaxed(0x10000, core->dos_base + PSCALE_BMEM_DAT);

	writel_relaxed((1 << 10), core->dos_base + DOS_SW_RESET0);
	writel_relaxed(0, core->dos_base + DOS_SW_RESET0);

	writel_relaxed(0x7, core->dos_base + PSCALE_RST);
	writel_relaxed(0, core->dos_base + PSCALE_RST);
}

static int codec_mjpeg_start(struct vdec_session *sess)
{
	struct vdec_core *core = sess->core;

	writel_relaxed((1 << 7) | (1 << 6), core->dos_base + DOS_SW_RESET0);
	writel_relaxed(0, core->dos_base + DOS_SW_RESET0);

	writel_relaxed(12, core->dos_base + AV_SCRATCH_0);
	writel_relaxed(0x031a, core->dos_base + AV_SCRATCH_1);

	codec_helper_set_canvases(sess, core->dos_base + AV_SCRATCH_4);
	codec_mjpeg_init_scaler(core);

	writel_relaxed(0, core->dos_base + MREG_TO_AMRISC);
	writel_relaxed(0, core->dos_base + MREG_FROM_AMRISC);
	writel_relaxed(0xffff, core->dos_base + MCPU_INTR_MSK);
	writel_relaxed((sess->height << 4) | 0x8000, core->dos_base + MREG_DECODE_PARAM);
	writel_relaxed(1, core->dos_base + ASSIST_MBOX1_CLR_REG);
	writel_relaxed(1, core->dos_base + ASSIST_MBOX1_MASK);
	writel_relaxed(8, core->dos_base + VDEC_ASSIST_AMR1_INT8);

	return 0;
}

static int codec_mjpeg_stop(struct vdec_session *sess)
{
	return 0;
}

static irqreturn_t codec_mjpeg_isr(struct vdec_session *sess)
{
	u32 reg;
	u32 buffer_index;
	struct vdec_core *core = sess->core;

	writel_relaxed(1, core->dos_base + ASSIST_MBOX1_CLR_REG);

	reg = readl_relaxed(core->dos_base + MREG_FROM_AMRISC);
	if (!(reg & 0x7))
		return IRQ_HANDLED;

	buffer_index = ((reg & 0x7) - 1) & 3;
	vdec_dst_buf_done_idx(sess, buffer_index);

	writel_relaxed(0, core->dos_base + MREG_FROM_AMRISC);
	return IRQ_HANDLED;
}

struct vdec_codec_ops codec_mjpeg_ops = {
	.start = codec_mjpeg_start,
	.stop = codec_mjpeg_stop,
	.isr = codec_mjpeg_isr,
	.can_recycle = codec_mjpeg_can_recycle,
	.recycle = codec_mjpeg_recycle,
};
