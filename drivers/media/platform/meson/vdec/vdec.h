/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 */

#ifndef __MESON_VDEC_CORE_H_
#define __MESON_VDEC_CORE_H_

#include <linux/regmap.h>
#include <linux/list.h>
#include <media/videobuf2-v4l2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#include "vdec_platform.h"

struct dummy_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct amvdec_buffer {
	struct list_head list;
	struct vb2_buffer *vb;
};

struct amvdec_timestamp {
	struct list_head list;
	u64 ts;
};

struct amvdec_session;

struct amvdec_core {
	void __iomem *dos_base;
	void __iomem *esparser_base;
	void __iomem *dmc_base;
	struct regmap *regmap_ao;

	struct device *dev;
	struct device *dev_dec;
	const struct vdec_platform *platform;

	struct clk *dos_parser_clk;
	struct clk *dos_clk;
	struct clk *vdec_1_clk;
	struct clk *vdec_hevc_clk;

	struct reset_control *esparser_reset;

	struct video_device *vdev_dec;
	struct v4l2_device v4l2_dev;
	
	struct amvdec_session *cur_sess;
	struct mutex lock;
};

/* Describes one of the VDECS (VDEC_1, VDEC_2, VDEC_HCODEC, VDEC_HEVC) */
struct amvdec_ops {
	int (*start)(struct amvdec_session *sess);
	int (*stop)(struct amvdec_session *sess);
	void (*conf_esparser)(struct amvdec_session *sess);
	u32 (*vififo_level)(struct amvdec_session *sess);
};

/* Describes one of the compression standard supported (H.264, HEVC..) */
struct amvdec_codec_ops {
	int (*start)(struct amvdec_session *sess);
	int (*stop)(struct amvdec_session *sess);
	int (*load_extended_firmware)(struct amvdec_session *sess, const u8 *data, u32 len);
	u32 (*num_pending_bufs)(struct amvdec_session *sess);
	int (*can_recycle)(struct amvdec_core *core);
	void (*recycle)(struct amvdec_core *core, u32 buf_idx);
	void (*notify_dst_buffer)(struct amvdec_session *sess, struct vb2_buffer *vb);
	void (*drain)(struct amvdec_session *sess);
	irqreturn_t (*isr)(struct amvdec_session *sess);
	irqreturn_t (*threaded_isr)(struct amvdec_session *sess);
};

/* Describes one of the OUTPUT format that can be decoded */
struct amvdec_format {
	u32 pixfmt;
	u32 min_buffers;
	u32 max_buffers;
	u32 max_width;
	u32 max_height;

	struct amvdec_ops *vdec_ops;
	struct amvdec_codec_ops *codec_ops;

	char *firmware_path;
	u32 pixfmts_cap[4];
};

struct amvdec_session {
	struct amvdec_core *core;
	
	struct v4l2_fh fh;
	struct v4l2_m2m_dev *m2m_dev;
	struct v4l2_m2m_ctx *m2m_ctx;
	struct mutex lock;
	struct mutex codec_lock;
	
	const struct amvdec_format *fmt_out;
	u32 pixfmt_cap;

	u32 width;
	u32 height;
	u32 colorspace;
	u8 ycbcr_enc;
	u8 quantization;
	u8 xfer_func;

	/* Number of buffers currently queued into ESPARSER */
	atomic_t esparser_queued_bufs;

	/* Work for the ESPARSER to process src buffers */
	struct work_struct esparser_queue_work;

	/* Whether capture/output streaming are on */
	unsigned int streamon_cap, streamon_out;
	
	/* Capture sequence counter */
	unsigned int sequence_cap;

	/* Whether userspace signaled EOS via command, empty buffer or
	 * V4L2_BUF_FLAG_LAST
	 */
	unsigned int should_stop;

	/* Is set to 1 once the first keyframe has been parsed */
	unsigned int keyframe_found;

	/* Big contiguous area for the VIFIFO */
	void *vififo_vaddr;
	dma_addr_t vififo_paddr;
	u32 vififo_size;

	/* Buffers that need to be recycled by the HW */
	struct list_head bufs_recycle;
	struct mutex bufs_recycle_lock;
	unsigned int num_recycle;
	/* Thread for recycling buffers into the hardware */
	struct task_struct *recycle_thread;
	
	/* src buffers' timestamps */
	struct list_head bufs;
	spinlock_t bufs_spinlock;

	/* Tracks last time we got a vdec IRQ */
	u64 last_irq_jiffies;

	/* Codec private data */
	void *priv;
};

u32 amvdec_get_output_size(struct amvdec_session *sess);
void amvdec_dst_buf_done_idx(struct amvdec_session *sess, u32 buf_idx);
void amvdec_dst_buf_done(struct amvdec_session *sess, struct vb2_v4l2_buffer *vbuf);
void amvdec_add_ts_reorder(struct amvdec_session *sess, u64 ts);
void amvdec_remove_ts(struct amvdec_session *sess, u64 ts);
void amvdec_abort(struct amvdec_session *sess);

void amvdec_write_dos(struct amvdec_core *core, u32 reg, u32 val);
void amvdec_write_parser(struct amvdec_core *core, u32 reg, u32 val);

#endif
