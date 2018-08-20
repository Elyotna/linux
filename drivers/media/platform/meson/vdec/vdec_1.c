// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 */

#include <linux/firmware.h>
#include <linux/clk.h>

#include "vdec_1.h"
#include "dos_regs.h"

/* AO Registers */
#define AO_RTI_GEN_PWR_SLEEP0	0xe8
#define AO_RTI_GEN_PWR_ISO0	0xec
	#define GEN_PWR_VDEC_1 (BIT(3) | BIT(2))

/* Stream Buffer (stbuf) regs (DOS) */
#define POWER_CTL_VLD 0x3020
#define VLD_MEM_VIFIFO_START_PTR 0x3100
#define VLD_MEM_VIFIFO_CURR_PTR 0x3104
#define VLD_MEM_VIFIFO_END_PTR 0x3108
#define VLD_MEM_VIFIFO_CONTROL 0x3110
	#define MEM_FIFO_CNT_BIT	16
	#define MEM_FILL_ON_LEVEL	BIT(10)
	#define MEM_CTRL_EMPTY_EN	BIT(2)
	#define MEM_CTRL_FILL_EN	BIT(1)
#define VLD_MEM_VIFIFO_WP 0x3114
#define VLD_MEM_VIFIFO_RP 0x3118
#define VLD_MEM_VIFIFO_LEVEL 0x311c
#define VLD_MEM_VIFIFO_BUF_CNTL 0x3120
	#define MEM_BUFCTRL_MANUAL	BIT(1)
#define VLD_MEM_VIFIFO_WRAP_COUNT 0x3144

#define MC_SIZE			(4096 * 4)

static int vdec_1_load_firmware(struct amvdec_session *sess, const char* fwname)
{
	const struct firmware *fw;
	struct amvdec_core *core = sess->core;
	struct device *dev = core->dev_dec;
	struct amvdec_codec_ops *codec_ops = sess->fmt_out->codec_ops;
	static void *mc_addr;
	static dma_addr_t mc_addr_map;
	int ret;
	u32 i = 1000;

	ret = request_firmware(&fw, fwname, dev);
	if (ret < 0)
		return -EINVAL;

	if (fw->size < MC_SIZE) {
		dev_err(dev, "Firmware size %zu is too small. Expected %u.\n",
			fw->size, MC_SIZE);
		ret = -EINVAL;
		goto release_firmware;
	}

	mc_addr = dma_alloc_coherent(core->dev, MC_SIZE, &mc_addr_map, GFP_KERNEL);
	if (!mc_addr) {
		dev_err(dev, "Failed allocating memory for firmware loading\n");
		ret = -ENOMEM;
		goto release_firmware;
	 }

	memcpy(mc_addr, fw->data, MC_SIZE);

	amvdec_write_dos(core, MPSR, 0);
	amvdec_write_dos(core, CPSR, 0);

	amvdec_write_dos(core, MDEC_PIC_DC_CTRL, readl_relaxed(core->dos_base + MDEC_PIC_DC_CTRL) & ~(1<<31));

	amvdec_write_dos(core, IMEM_DMA_ADR, mc_addr_map);
	amvdec_write_dos(core, IMEM_DMA_COUNT, MC_SIZE / 4);
	amvdec_write_dos(core, IMEM_DMA_CTRL, (0x8000 | (7 << 16)));

	while (--i && readl(core->dos_base + IMEM_DMA_CTRL) & 0x8000) { }

	if (i == 0) {
		dev_err(dev, "Firmware load fail (DMA hang?)\n");
		ret = -EINVAL;
		goto free_mc;
	}

	if (codec_ops->load_extended_firmware)
		codec_ops->load_extended_firmware(sess, fw->data + MC_SIZE, fw->size - MC_SIZE);

free_mc:
	dma_free_coherent(core->dev, MC_SIZE, mc_addr, mc_addr_map);
release_firmware:
	release_firmware(fw);
	return ret;
}

int vdec_1_stbuf_power_up(struct amvdec_session *sess) {
	struct amvdec_core *core = sess->core;

	amvdec_write_dos(core, VLD_MEM_VIFIFO_CONTROL, 0);
	amvdec_write_dos(core, VLD_MEM_VIFIFO_WRAP_COUNT, 0);
	amvdec_write_dos(core, POWER_CTL_VLD, 1 << 4);

	amvdec_write_dos(core, VLD_MEM_VIFIFO_START_PTR, sess->vififo_paddr);
	amvdec_write_dos(core, VLD_MEM_VIFIFO_CURR_PTR, sess->vififo_paddr);
	amvdec_write_dos(core, VLD_MEM_VIFIFO_END_PTR, sess->vififo_paddr + sess->vififo_size - 8);

	amvdec_write_dos(core, VLD_MEM_VIFIFO_CONTROL, readl_relaxed(core->dos_base + VLD_MEM_VIFIFO_CONTROL) |  1);
	amvdec_write_dos(core, VLD_MEM_VIFIFO_CONTROL, readl_relaxed(core->dos_base + VLD_MEM_VIFIFO_CONTROL) & ~1);

	amvdec_write_dos(core, VLD_MEM_VIFIFO_BUF_CNTL, MEM_BUFCTRL_MANUAL);
	amvdec_write_dos(core, VLD_MEM_VIFIFO_WP, sess->vififo_paddr);

	amvdec_write_dos(core, VLD_MEM_VIFIFO_BUF_CNTL, readl_relaxed(core->dos_base + VLD_MEM_VIFIFO_BUF_CNTL) |  1);
	amvdec_write_dos(core, VLD_MEM_VIFIFO_BUF_CNTL, readl_relaxed(core->dos_base + VLD_MEM_VIFIFO_BUF_CNTL) & ~1);

	amvdec_write_dos(core, VLD_MEM_VIFIFO_CONTROL, readl_relaxed(core->dos_base + VLD_MEM_VIFIFO_CONTROL) | (0x11 << MEM_FIFO_CNT_BIT) | MEM_FILL_ON_LEVEL | MEM_CTRL_FILL_EN | MEM_CTRL_EMPTY_EN);

	return 0;
}

static void vdec_1_conf_esparser(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;

	/* VDEC_1 specific ESPARSER stuff */
	amvdec_write_dos(core, DOS_GEN_CTRL0, 0); // set vififo_vbuf_rp_sel=>vdec
	amvdec_write_dos(core, VLD_MEM_VIFIFO_BUF_CNTL, 1);
	amvdec_write_dos(core, VLD_MEM_VIFIFO_BUF_CNTL, readl_relaxed(core->dos_base + VLD_MEM_VIFIFO_BUF_CNTL) & ~1);
}

static u32 vdec_1_vififo_level(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;

	return readl_relaxed(core->dos_base + VLD_MEM_VIFIFO_LEVEL);
}

static int vdec_1_start(struct amvdec_session *sess)
{
	int ret;
	struct amvdec_core *core = sess->core;
	struct amvdec_codec_ops *codec_ops = sess->fmt_out->codec_ops;

	clk_set_rate(core->vdec_1_clk, 666666666);
	ret = clk_prepare_enable(core->vdec_1_clk);
	if (ret)
		return ret;

	regmap_update_bits(core->regmap_ao, AO_RTI_GEN_PWR_SLEEP0,
		GEN_PWR_VDEC_1, 0);
	udelay(10);

	/* Reset VDEC1 */
	amvdec_write_dos(core, DOS_SW_RESET0, 0xfffffffc);
	amvdec_write_dos(core, DOS_SW_RESET0, 0x00000000);

	amvdec_write_dos(core, DOS_GCLK_EN0, 0x3ff);

	/* VDEC Memories */
	amvdec_write_dos(core, DOS_MEM_PD_VDEC, 0x00000000);
	/* Remove VDEC1 Isolation */
	regmap_write(core->regmap_ao, AO_RTI_GEN_PWR_ISO0, 0x00000000);
	/* Reset DOS top registers */
	amvdec_write_dos(core, DOS_VDEC_MCRCC_STALL_CTRL, 0x00000000);

	amvdec_write_dos(core, GCLK_EN, 0x3ff);
	amvdec_write_dos(core, MDEC_PIC_DC_CTRL, readl_relaxed(core->dos_base + MDEC_PIC_DC_CTRL) & ~(1<<31));

	vdec_1_stbuf_power_up(sess);

	ret = vdec_1_load_firmware(sess, sess->fmt_out->firmware_path);
	if (ret) {
		clk_disable_unprepare(core->vdec_1_clk);
		regmap_update_bits(core->regmap_ao, AO_RTI_GEN_PWR_SLEEP0,
			GEN_PWR_VDEC_1, GEN_PWR_VDEC_1);
		return ret;
	}

	codec_ops->start(sess);

	/* Enable 2-plane output */
	if (sess->pixfmt_cap == V4L2_PIX_FMT_NV12M)
		amvdec_write_dos(core, MDEC_PIC_DC_CTRL, readl_relaxed(core->dos_base + MDEC_PIC_DC_CTRL) | (1 << 17));

	/* Enable firmware processor */
	amvdec_write_dos(core, MPSR, 1);
	/* Let the firmware settle */
	udelay(10);

	return 0;
}

static int vdec_1_stop(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;
	struct amvdec_codec_ops *codec_ops = sess->fmt_out->codec_ops;

	amvdec_write_dos(core, MPSR, 0);
	amvdec_write_dos(core, CPSR, 0);

	codec_ops->stop(sess);

	while (readl_relaxed(core->dos_base + IMEM_DMA_CTRL) & 0x8000) { }

	amvdec_write_dos(core, DOS_SW_RESET0, (1<<12)|(1<<11));
	amvdec_write_dos(core, DOS_SW_RESET0, 0);
	readl_relaxed(core->dos_base + DOS_SW_RESET0);

	amvdec_write_dos(core, ASSIST_MBOX1_MASK, 0);

	amvdec_write_dos(core, MDEC_PIC_DC_CTRL, readl_relaxed(core->dos_base + MDEC_PIC_DC_CTRL) | 1);
	amvdec_write_dos(core, MDEC_PIC_DC_CTRL, readl_relaxed(core->dos_base + MDEC_PIC_DC_CTRL) & ~1);
	readl_relaxed(core->dos_base + MDEC_PIC_DC_STATUS);

	amvdec_write_dos(core, DBLK_CTRL, 3);
	amvdec_write_dos(core, DBLK_CTRL, 0);
	readl_relaxed(core->dos_base + DBLK_STATUS);

	amvdec_write_dos(core, MC_CTRL1, readl_relaxed(core->dos_base + MC_CTRL1) | 0x9);
	amvdec_write_dos(core, MC_CTRL1, readl_relaxed(core->dos_base + MC_CTRL1) & ~0x9);
	readl_relaxed(core->dos_base + MC_STATUS0);

	while (readl_relaxed(core->dos_base + DCAC_DMA_CTRL) & 0x8000) { }

	/* enable vdec1 isolation */
	regmap_write(core->regmap_ao, AO_RTI_GEN_PWR_ISO0, 0xc0);
	/* power off vdec1 memories */
	writel(0xffffffffUL, core->dos_base + DOS_MEM_PD_VDEC);
	regmap_update_bits(core->regmap_ao, AO_RTI_GEN_PWR_SLEEP0,
		GEN_PWR_VDEC_1, GEN_PWR_VDEC_1);

	clk_disable_unprepare(core->vdec_1_clk);

	return 0;
}

struct amvdec_ops vdec_1_ops = {
	.start = vdec_1_start,
	.stop = vdec_1_stop,
	.conf_esparser = vdec_1_conf_esparser,
	.vififo_level = vdec_1_vififo_level,
};