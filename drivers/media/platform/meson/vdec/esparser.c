// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 BayLibre, SAS
 * Author: Maxime Jourdan <mjourdan@baylibre.com>
 *
 * The Elementary Stream Parser is a HW bitstream parser.
 * It reads bitstream buffers and feeds them to the VIFIFO
 */

#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/reset.h>
#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-mem2mem.h>

#include "dos_regs.h"
#include "esparser.h"
#include "vdec_helpers.h"

/* PARSER REGS (CBUS) */
#define PARSER_CONTROL 0x00
	#define ES_PACK_SIZE_BIT	8
	#define ES_WRITE		BIT(5)
	#define ES_SEARCH		BIT(1)
	#define ES_PARSER_START		BIT(0)
#define PARSER_FETCH_ADDR	0x4
#define PARSER_FETCH_CMD	0x8
#define PARSER_CONFIG 0x14
	#define PS_CFG_MAX_FETCH_CYCLE_BIT	0
	#define PS_CFG_STARTCODE_WID_24_BIT	10
	#define PS_CFG_MAX_ES_WR_CYCLE_BIT	12
	#define PS_CFG_PFIFO_EMPTY_CNT_BIT	16
#define PFIFO_WR_PTR 0x18
#define PFIFO_RD_PTR 0x1c
#define PARSER_SEARCH_PATTERN 0x24
	#define ES_START_CODE_PATTERN 0x00000100
#define PARSER_SEARCH_MASK 0x28
	#define ES_START_CODE_MASK	0xffffff00
	#define FETCH_ENDIAN_BIT	27
#define PARSER_INT_ENABLE	0x2c
	#define PARSER_INT_HOST_EN_BIT	8
#define PARSER_INT_STATUS	0x30
	#define PARSER_INTSTAT_SC_FOUND	1
#define PARSER_ES_CONTROL	0x5c
#define PARSER_VIDEO_START_PTR	0x80
#define PARSER_VIDEO_END_PTR	0x84
#define PARSER_VIDEO_HOLE	0x90

#define SEARCH_PATTERN_LEN	512

/* Buffer to send to the ESPARSER to signal End Of Stream.
 * Credits to Endless Mobile.
 */
#define EOS_TAIL_BUF_SIZE 1024
static const u8 eos_tail_data[EOS_TAIL_BUF_SIZE] = {
	0x00, 0x00, 0x00, 0x01, 0x06, 0x05, 0xff, 0xe4, 0xdc, 0x45, 0xe9, 0xbd,
	0xe6, 0xd9, 0x48, 0xb7,	0x96, 0x2c, 0xd8, 0x20, 0xd9, 0x23, 0xee, 0xef,
	0x78, 0x32, 0x36, 0x34, 0x20, 0x2d, 0x20, 0x63,	0x6f, 0x72, 0x65, 0x20,
	0x36, 0x37, 0x20, 0x72, 0x31, 0x31, 0x33, 0x30, 0x20, 0x38, 0x34, 0x37,
	0x35, 0x39, 0x37, 0x37, 0x20, 0x2d, 0x20, 0x48, 0x2e, 0x32, 0x36, 0x34,
	0x2f, 0x4d, 0x50, 0x45,	0x47, 0x2d, 0x34, 0x20, 0x41, 0x56, 0x43, 0x20,
	0x63, 0x6f, 0x64, 0x65, 0x63, 0x20, 0x2d, 0x20,	0x43, 0x6f, 0x70, 0x79,
	0x6c, 0x65, 0x66, 0x74, 0x20, 0x32, 0x30, 0x30, 0x33, 0x2d, 0x32, 0x30,
	0x30, 0x39, 0x20, 0x2d, 0x20, 0x68, 0x74, 0x74, 0x70, 0x3a, 0x2f, 0x2f,
	0x77, 0x77, 0x77, 0x2e,	0x76, 0x69, 0x64, 0x65, 0x6f, 0x6c, 0x61, 0x6e,
	0x2e, 0x6f, 0x72, 0x67, 0x2f, 0x78, 0x32, 0x36,	0x34, 0x2e, 0x68, 0x74,
	0x6d, 0x6c, 0x20, 0x2d, 0x20, 0x6f, 0x70, 0x74, 0x69, 0x6f, 0x6e, 0x73,
	0x3a, 0x20, 0x63, 0x61, 0x62, 0x61, 0x63, 0x3d, 0x31, 0x20, 0x72, 0x65,
	0x66, 0x3d, 0x31, 0x20,	0x64, 0x65, 0x62, 0x6c, 0x6f, 0x63, 0x6b, 0x3d,
	0x31, 0x3a, 0x30, 0x3a, 0x30, 0x20, 0x61, 0x6e,	0x61, 0x6c, 0x79, 0x73,
	0x65, 0x3d, 0x30, 0x78, 0x31, 0x3a, 0x30, 0x78, 0x31, 0x31, 0x31, 0x20,
	0x6d, 0x65, 0x3d, 0x68, 0x65, 0x78, 0x20, 0x73, 0x75, 0x62, 0x6d, 0x65,
	0x3d, 0x36, 0x20, 0x70,	0x73, 0x79, 0x5f, 0x72, 0x64, 0x3d, 0x31, 0x2e,
	0x30, 0x3a, 0x30, 0x2e, 0x30, 0x20, 0x6d, 0x69,	0x78, 0x65, 0x64, 0x5f,
	0x72, 0x65, 0x66, 0x3d, 0x30, 0x20, 0x6d, 0x65, 0x5f, 0x72, 0x61, 0x6e,
	0x67, 0x65, 0x3d, 0x31, 0x36, 0x20, 0x63, 0x68, 0x72, 0x6f, 0x6d, 0x61,
	0x5f, 0x6d, 0x65, 0x3d,	0x31, 0x20, 0x74, 0x72, 0x65, 0x6c, 0x6c, 0x69,
	0x73, 0x3d, 0x30, 0x20, 0x38, 0x78, 0x38, 0x64,	0x63, 0x74, 0x3d, 0x30,
	0x20, 0x63, 0x71, 0x6d, 0x3d, 0x30, 0x20, 0x64, 0x65, 0x61, 0x64, 0x7a,
	0x6f, 0x6e, 0x65, 0x3d, 0x32, 0x31, 0x2c, 0x31, 0x31, 0x20, 0x63, 0x68,
	0x72, 0x6f, 0x6d, 0x61,	0x5f, 0x71, 0x70, 0x5f, 0x6f, 0x66, 0x66, 0x73,
	0x65, 0x74, 0x3d, 0x2d, 0x32, 0x20, 0x74, 0x68,	0x72, 0x65, 0x61, 0x64,
	0x73, 0x3d, 0x31, 0x20, 0x6e, 0x72, 0x3d, 0x30, 0x20, 0x64, 0x65, 0x63,
	0x69, 0x6d, 0x61, 0x74, 0x65, 0x3d, 0x31, 0x20, 0x6d, 0x62, 0x61, 0x66,
	0x66, 0x3d, 0x30, 0x20,	0x62, 0x66, 0x72, 0x61, 0x6d, 0x65, 0x73, 0x3d,
	0x30, 0x20, 0x6b, 0x65, 0x79, 0x69, 0x6e, 0x74,	0x3d, 0x32, 0x35, 0x30,
	0x20, 0x6b, 0x65, 0x79, 0x69, 0x6e, 0x74, 0x5f, 0x6d, 0x69, 0x6e, 0x3d,
	0x32, 0x35, 0x20, 0x73, 0x63, 0x65, 0x6e, 0x65, 0x63, 0x75, 0x74, 0x3d,
	0x34, 0x30, 0x20, 0x72,	0x63, 0x3d, 0x61, 0x62, 0x72, 0x20, 0x62, 0x69,
	0x74, 0x72, 0x61, 0x74, 0x65, 0x3d, 0x31, 0x30,	0x20, 0x72, 0x61, 0x74,
	0x65, 0x74, 0x6f, 0x6c, 0x3d, 0x31, 0x2e, 0x30, 0x20, 0x71, 0x63, 0x6f,
	0x6d, 0x70, 0x3d, 0x30, 0x2e, 0x36, 0x30, 0x20, 0x71, 0x70, 0x6d, 0x69,
	0x6e, 0x3d, 0x31, 0x30,	0x20, 0x71, 0x70, 0x6d, 0x61, 0x78, 0x3d, 0x35,
	0x31, 0x20, 0x71, 0x70, 0x73, 0x74, 0x65, 0x70,	0x3d, 0x34, 0x20, 0x69,
	0x70, 0x5f, 0x72, 0x61, 0x74, 0x69, 0x6f, 0x3d, 0x31, 0x2e, 0x34, 0x30,
	0x20, 0x61, 0x71, 0x3d, 0x31, 0x3a, 0x31, 0x2e, 0x30, 0x30, 0x00, 0x80,
	0x00, 0x00, 0x00, 0x01,	0x67, 0x4d, 0x40, 0x0a, 0x9a, 0x74, 0xf4, 0x20,
	0x00, 0x00, 0x03, 0x00, 0x20, 0x00, 0x00, 0x06,	0x51, 0xe2, 0x44, 0xd4,
	0x00, 0x00, 0x00, 0x01, 0x68, 0xee, 0x32, 0xc8, 0x00, 0x00, 0x00, 0x01,
	0x65, 0x88, 0x80, 0x20, 0x00, 0x08, 0x7f, 0xea, 0x6a, 0xe2, 0x99, 0xb6,
	0x57, 0xae, 0x49, 0x30,	0xf5, 0xfe, 0x5e, 0x46, 0x0b, 0x72, 0x44, 0xc4,
	0xe1, 0xfc, 0x62, 0xda, 0xf1, 0xfb, 0xa2, 0xdb,	0xd6, 0xbe, 0x5c, 0xd7,
	0x24, 0xa3, 0xf5, 0xb9, 0x2f, 0x57, 0x16, 0x49, 0x75, 0x47, 0x77, 0x09,
	0x5c, 0xa1, 0xb4, 0xc3, 0x4f, 0x60, 0x2b, 0xb0, 0x0c, 0xc8, 0xd6, 0x66,
	0xba, 0x9b, 0x82, 0x29,	0x33, 0x92, 0x26, 0x99, 0x31, 0x1c, 0x7f, 0x9b
};

static DECLARE_WAIT_QUEUE_HEAD(wq);
static int search_done;

static irqreturn_t esparser_isr(int irq, void *dev)
{
	int int_status;
	struct amvdec_core *core = dev;

	int_status = amvdec_read_parser(core, PARSER_INT_STATUS);
	amvdec_write_parser(core, PARSER_INT_STATUS, int_status);

	if (int_status & PARSER_INTSTAT_SC_FOUND) {
		amvdec_write_parser(core, PFIFO_RD_PTR, 0);
		amvdec_write_parser(core, PFIFO_WR_PTR, 0);
		search_done = 1;
		wake_up_interruptible(&wq);
	}

	return IRQ_HANDLED;
}

/* Pad the packet to at least 4KiB bytes otherwise the VDEC unit won't trigger
 * ISRs.
 * Also append a start code 000001ff at the end to trigger
 * the ESPARSER interrupt.
 */
static u32 esparser_pad_start_code(struct vb2_buffer *vb)
{
	u32 payload_size = vb2_get_plane_payload(vb, 0);
	u32 pad_size = 0;
	u8 *vaddr = vb2_plane_vaddr(vb, 0) + payload_size;

	if (payload_size < ESPARSER_MIN_PACKET_SIZE) {
		pad_size = ESPARSER_MIN_PACKET_SIZE - payload_size;
		memset(vaddr, 0, pad_size);
	}

	memset(vaddr + pad_size, 0, SEARCH_PATTERN_LEN);
	vaddr[pad_size]     = 0x00;
	vaddr[pad_size + 1] = 0x00;
	vaddr[pad_size + 2] = 0x01;
	vaddr[pad_size + 3] = 0xff;

	return pad_size;
}

static int
esparser_write_data(struct amvdec_core *core, dma_addr_t addr, u32 size)
{
	amvdec_write_parser(core, PFIFO_RD_PTR, 0);
	amvdec_write_parser(core, PFIFO_WR_PTR, 0);
	amvdec_write_parser(core, PARSER_CONTROL,
			    ES_WRITE |
			    ES_PARSER_START |
			    ES_SEARCH |
			    (size << ES_PACK_SIZE_BIT));

	amvdec_write_parser(core, PARSER_FETCH_ADDR, addr);
	amvdec_write_parser(core, PARSER_FETCH_CMD,
			    (7 << FETCH_ENDIAN_BIT) |
			    (size + SEARCH_PATTERN_LEN));

	search_done = 0;
	return wait_event_interruptible_timeout(wq, search_done, (HZ / 5));
}

static u32 esparser_vififo_get_free_space(struct amvdec_session *sess)
{
	u32 vififo_usage;
	struct amvdec_ops *vdec_ops = sess->fmt_out->vdec_ops;
	struct amvdec_core *core = sess->core;

	vififo_usage  = vdec_ops->vififo_level(sess);
	vififo_usage += amvdec_read_parser(core, PARSER_VIDEO_HOLE);
	vififo_usage += (6 * SZ_1K); // 6 KiB internal fifo

	if (vififo_usage > sess->vififo_size) {
		dev_warn(sess->core->dev,
			 "VIFIFO usage (%u) > VIFIFO size (%u)\n",
			 vififo_usage, sess->vififo_size);
		return 0;
	}

	return sess->vififo_size - vififo_usage;
}

int esparser_queue_eos(struct amvdec_core *core)
{
	struct device *dev = core->dev;
	void *eos_vaddr;
	dma_addr_t eos_paddr;
	int ret;

	eos_vaddr = dma_alloc_coherent(dev,
				       EOS_TAIL_BUF_SIZE + SEARCH_PATTERN_LEN,
				       &eos_paddr, GFP_KERNEL);
	if (!eos_vaddr)
		return -ENOMEM;

	memset(eos_vaddr, 0, EOS_TAIL_BUF_SIZE + SEARCH_PATTERN_LEN);
	memcpy(eos_vaddr, eos_tail_data, sizeof(eos_tail_data));
	ret = esparser_write_data(core, eos_paddr, EOS_TAIL_BUF_SIZE);
	dma_free_coherent(dev, EOS_TAIL_BUF_SIZE + SEARCH_PATTERN_LEN,
			  eos_vaddr, eos_paddr);

	return ret;
}

static int
esparser_queue(struct amvdec_session *sess, struct vb2_v4l2_buffer *vbuf)
{
	int ret;
	struct vb2_buffer *vb = &vbuf->vb2_buf;
	struct amvdec_core *core = sess->core;
	struct amvdec_codec_ops *codec_ops = sess->fmt_out->codec_ops;
	struct amvdec_ops *vdec_ops = sess->fmt_out->vdec_ops;
	u32 num_dst_bufs = 0;
	u32 payload_size = vb2_get_plane_payload(vb, 0);
	dma_addr_t phy = vb2_dma_contig_plane_dma_addr(vb, 0);
	s32 offset = 0;
	u32 pad_size;

	if (codec_ops->num_pending_bufs)
		num_dst_bufs = codec_ops->num_pending_bufs(sess);

	num_dst_bufs += v4l2_m2m_num_dst_bufs_ready(sess->m2m_ctx);

	if (esparser_vififo_get_free_space(sess) < payload_size ||
	    atomic_read(&sess->esparser_queued_bufs) >= num_dst_bufs)
		return -EAGAIN;

	v4l2_m2m_src_buf_remove_by_buf(sess->m2m_ctx, vbuf);

	if (vdec_ops->use_offsets())
		offset = amvdec_read_dos(core, VLD_MEM_VIFIFO_WP) -
			 sess->vififo_paddr;

	amvdec_add_ts_reorder(sess, vb->timestamp, offset);
	dev_dbg(core->dev, "esparser: Queuing ts = %llu pld_size = %u\n",
		vb->timestamp, payload_size);

	pad_size = esparser_pad_start_code(vb);
	ret = esparser_write_data(core, phy, payload_size + pad_size);

	if (ret <= 0) {
		dev_warn(core->dev, "esparser: input parsing error\n");
		amvdec_remove_ts(sess, vb->timestamp);
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
		amvdec_write_parser(core, PARSER_FETCH_CMD, 0);

		return 0;
	}

	/* We need to wait until we parse the first keyframe.
	 * All buffers prior to the first keyframe must be dropped.
	 */
	if (!sess->keyframe_found)
		usleep_range(1000, 2000);

	if (sess->keyframe_found)
		atomic_inc(&sess->esparser_queued_bufs);
	else
		amvdec_remove_ts(sess, vb->timestamp);

	vbuf->flags = 0;
	vbuf->field = V4L2_FIELD_NONE;
	v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_DONE);

	return 0;
}

void esparser_queue_all_src(struct work_struct *work)
{
	struct v4l2_m2m_buffer *buf, *n;
	struct amvdec_session *sess =
		container_of(work, struct amvdec_session, esparser_queue_work);

	mutex_lock(&sess->lock);
	v4l2_m2m_for_each_src_buf_safe(sess->m2m_ctx, buf, n) {
		if (esparser_queue(sess, &buf->vb) < 0)
			break;
	}
	mutex_unlock(&sess->lock);
}

int esparser_power_up(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;
	struct amvdec_ops *vdec_ops = sess->fmt_out->vdec_ops;

	reset_control_reset(core->esparser_reset);
	amvdec_write_parser(core, PARSER_CONFIG,
			    (10 << PS_CFG_PFIFO_EMPTY_CNT_BIT) |
			    (1  << PS_CFG_MAX_ES_WR_CYCLE_BIT) |
			    (16 << PS_CFG_MAX_FETCH_CYCLE_BIT));

	amvdec_write_parser(core, PFIFO_RD_PTR, 0);
	amvdec_write_parser(core, PFIFO_WR_PTR, 0);

	amvdec_write_parser(core, PARSER_SEARCH_PATTERN,
			    ES_START_CODE_PATTERN);
	amvdec_write_parser(core, PARSER_SEARCH_MASK, ES_START_CODE_MASK);

	amvdec_write_parser(core, PARSER_CONFIG,
			    (10 << PS_CFG_PFIFO_EMPTY_CNT_BIT) |
			    (1  << PS_CFG_MAX_ES_WR_CYCLE_BIT) |
			    (16 << PS_CFG_MAX_FETCH_CYCLE_BIT) |
			    (2  << PS_CFG_STARTCODE_WID_24_BIT));

	amvdec_write_parser(core, PARSER_CONTROL,
			    (ES_SEARCH | ES_PARSER_START));

	amvdec_write_parser(core, PARSER_VIDEO_START_PTR, sess->vififo_paddr);
	amvdec_write_parser(core, PARSER_VIDEO_END_PTR,
			    sess->vififo_paddr + sess->vififo_size - 8);
	amvdec_write_parser(core, PARSER_ES_CONTROL,
			    amvdec_read_parser(core, PARSER_ES_CONTROL) & ~1);

	if (vdec_ops->conf_esparser)
		vdec_ops->conf_esparser(sess);

	amvdec_write_parser(core, PARSER_INT_STATUS, 0xffff);
	amvdec_write_parser(core, PARSER_INT_ENABLE,
			    BIT(PARSER_INT_HOST_EN_BIT));

	return 0;
}

int esparser_init(struct platform_device *pdev, struct amvdec_core *core)
{
	struct device *dev = &pdev->dev;
	int ret;
	int irq;

	irq = platform_get_irq_byname(pdev, "esparser");
	if (irq < 0) {
		dev_err(dev, "Failed getting ESPARSER IRQ from dtb\n");
		return irq;
	}

	ret = devm_request_irq(dev, irq, esparser_isr, IRQF_SHARED,
			       "esparserirq", core);
	if (ret) {
		dev_err(dev, "Failed requesting ESPARSER IRQ\n");
		return ret;
	}

	core->esparser_reset =
		devm_reset_control_get_exclusive(dev, "esparser");
	if (IS_ERR(core->esparser_reset)) {
		dev_err(dev, "Failed to get esparser_reset\n");
		return PTR_ERR(core->esparser_reset);
	}

	return 0;
}
