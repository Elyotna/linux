// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 */

#include <linux/firmware.h>
#include <linux/clk.h>

#include "vdec_1.h"
#include "hevc_regs.h"

/* AO Registers */
#define AO_RTI_GEN_PWR_SLEEP0	0xe8
#define AO_RTI_GEN_PWR_ISO0	0xec
	#define GEN_PWR_VDEC_HEVC (BIT(7) | BIT(6))

/* DOS Registers */
#define ASSIST_MBOX1_CLR_REG 0x01d4
#define ASSIST_MBOX1_MASK    0x01d8

#define DOS_GEN_CTRL0	     0xfc08
#define DOS_SW_RESET3        0xfcd0
#define DOS_MEM_PD_HEVC      0xfccc
#define DOS_GCLK_EN3	     0xfcd4

#define MC_SIZE	(4096 * 4)

static int vdec_hevc_load_firmware(struct vdec_session *sess, const char* fwname)
{
	struct vdec_core *core = sess->core;
	struct device *dev = core->dev_dec;
	const struct firmware *fw;
	static void *mc_addr;
	static dma_addr_t mc_addr_map;
	int ret;
	u32 i = 100;

	ret = request_firmware(&fw, fwname, dev);
	if (ret < 0)  {
		dev_err(dev, "Unable to request firmware %s\n", fwname);
		return ret;
	}

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

	writel_relaxed(0, core->dos_base + HEVC_MPSR);
	writel_relaxed(0, core->dos_base + HEVC_CPSR);

	writel_relaxed(mc_addr_map, core->dos_base + HEVC_IMEM_DMA_ADR);
	writel_relaxed(MC_SIZE / 4, core->dos_base + HEVC_IMEM_DMA_COUNT);
	writel_relaxed((0x8000 | (7 << 16)), core->dos_base + HEVC_IMEM_DMA_CTRL);

	while (--i && readl(core->dos_base + HEVC_IMEM_DMA_CTRL) & 0x8000) { }

	if (i == 0) {
		dev_err(dev, "Firmware load fail (DMA hang?)\n");
		ret = -ENODEV;
	}

	dma_free_coherent(core->dev, MC_SIZE, mc_addr, mc_addr_map);
release_firmware:
	release_firmware(fw);
	return ret;
}

static void vdec_hevc_stbuf_init(struct vdec_session *sess)
{
	struct vdec_core *core = sess->core;

	writel_relaxed(readl_relaxed(core->dos_base + HEVC_STREAM_CONTROL) & ~1, core->dos_base + HEVC_STREAM_CONTROL);
	writel_relaxed(sess->vififo_paddr, core->dos_base + HEVC_STREAM_START_ADDR);
	writel_relaxed(sess->vififo_paddr + sess->vififo_size, core->dos_base + HEVC_STREAM_END_ADDR);
	writel_relaxed(sess->vififo_paddr, core->dos_base + HEVC_STREAM_RD_PTR);
	writel_relaxed(sess->vififo_paddr, core->dos_base + HEVC_STREAM_WR_PTR);
}

/* VDEC_HEVC specific ESPARSER configuration */
static void vdec_hevc_conf_esparser(struct vdec_session *sess)
{
	struct vdec_core *core = sess->core;

	/* set vififo_vbuf_rp_sel=>vdec_hevc */
	writel_relaxed(3 << 1, core->dos_base + DOS_GEN_CTRL0);
	writel_relaxed(readl_relaxed(core->dos_base + HEVC_STREAM_CONTROL) | (1 << 3), core->dos_base + HEVC_STREAM_CONTROL);
	writel_relaxed(readl_relaxed(core->dos_base + HEVC_STREAM_CONTROL) | 1, core->dos_base + HEVC_STREAM_CONTROL);
	writel_relaxed(readl_relaxed(core->dos_base + HEVC_STREAM_FIFO_CTL) | (1 << 29), core->dos_base + HEVC_STREAM_FIFO_CTL);
}

static u32 vdec_hevc_vififo_level(struct vdec_session *sess)
{
	return readl_relaxed(sess->core->dos_base + HEVC_STREAM_LEVEL);
}

static int vdec_hevc_stop(struct vdec_session *sess)
{
	struct vdec_core *core = sess->core;
	struct vdec_codec_ops *codec_ops = sess->fmt_out->codec_ops;

	/* Disable interrupt */
	writel_relaxed(0, core->dos_base + HEVC_ASSIST_MBOX1_MASK);
	/* Disable firmware processor */
	writel_relaxed(0, core->dos_base + HEVC_MPSR);

	codec_ops->stop(sess);

	/* Enable VDEC_HEVC Isolation */
	regmap_update_bits(core->regmap_ao, AO_RTI_GEN_PWR_ISO0, 0xc00, 0xc00);

	/* VDEC_HEVC Memories */
	writel_relaxed(0xffffffffUL, core->dos_base + DOS_MEM_PD_HEVC);

	regmap_update_bits(core->regmap_ao, AO_RTI_GEN_PWR_SLEEP0,
		GEN_PWR_VDEC_HEVC, GEN_PWR_VDEC_HEVC);

	clk_disable_unprepare(core->vdec_hevc_clk);

	return 0;
}

static int vdec_hevc_start(struct vdec_session *sess)
{
	int ret;
	struct vdec_core *core = sess->core;
	struct vdec_codec_ops *codec_ops = sess->fmt_out->codec_ops;

	clk_set_rate(core->vdec_hevc_clk, 666666666);
	ret = clk_prepare_enable(core->vdec_hevc_clk);
	if (ret)
		return ret;

	regmap_update_bits(core->regmap_ao, AO_RTI_GEN_PWR_SLEEP0,
		GEN_PWR_VDEC_HEVC, 0);
	udelay(10);

	/* Reset VDEC_HEVC*/
	writel_relaxed(0xffffffff, core->dos_base + DOS_SW_RESET3);
	writel_relaxed(0x00000000, core->dos_base + DOS_SW_RESET3);

	writel_relaxed(0xffffffff, core->dos_base + DOS_GCLK_EN3);

	/* VDEC_HEVC Memories */
	writel_relaxed(0x00000000, core->dos_base + DOS_MEM_PD_HEVC);

	/* Remove VDEC_HEVC Isolation */
	regmap_update_bits(core->regmap_ao, AO_RTI_GEN_PWR_ISO0, 0xc00, 0);

	writel_relaxed(0xffffffff, core->dos_base + DOS_SW_RESET3);
	writel_relaxed(0x00000000, core->dos_base + DOS_SW_RESET3);

	vdec_hevc_stbuf_init(sess);

	ret = vdec_hevc_load_firmware(sess, sess->fmt_out->firmware_path);
	if (ret) {
		vdec_hevc_stop(sess);
		return ret;
	}

	codec_ops->start(sess);

	writel_relaxed((1<<12)|(1<<11), core->dos_base + DOS_SW_RESET3);
	writel_relaxed(0, core->dos_base + DOS_SW_RESET3);
	readl_relaxed(core->dos_base + DOS_SW_RESET3);

	writel_relaxed(1, core->dos_base + HEVC_MPSR);

	return 0;
}

struct vdec_ops vdec_hevc_ops = {
	.start = vdec_hevc_start,
	.stop = vdec_hevc_stop,
	.conf_esparser = vdec_hevc_conf_esparser,
	.vififo_level = vdec_hevc_vififo_level,
};