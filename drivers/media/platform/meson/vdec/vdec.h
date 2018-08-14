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

#define REG_BUF_SIZE 21

struct dummy_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct vdec_buffer {
	struct list_head list;
	struct vb2_buffer *vb;
};

struct vdec_timestamp {
	struct list_head list;
	u64 ts;
};

struct vdec_session;

struct vdec_core {
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
	
	struct vdec_session *cur_sess;
	struct mutex lock;
};

/* Describes one of the VDECS (VDEC_1, VDEC_2, VDEC_HCODEC, VDEC_HEVC) */
struct vdec_ops {
	int (*start)(struct vdec_session *sess);
	int (*stop)(struct vdec_session *sess);
	void (*conf_esparser)(struct vdec_session *sess);
	u32 (*vififo_level)(struct vdec_session *sess);
};

/* Describes one of the compression standard supported (H.264, HEVC..) */
struct vdec_codec_ops {
	int (*start)(struct vdec_session *sess);
	int (*stop)(struct vdec_session *sess);
	int (*load_extended_firmware)(struct vdec_session *sess, const u8 *data, u32 len);
	u32 (*num_pending_bufs)(struct vdec_session *sess);
	int (*can_recycle)(struct vdec_core *core);
	void (*recycle)(struct vdec_core *core, u32 buf_idx);
	void (*notify_dst_buffer)(struct vdec_session *sess, struct vb2_buffer *vb);
	irqreturn_t (*isr)(struct vdec_session *sess);
	irqreturn_t (*threaded_isr)(struct vdec_session *sess);
};

/* Describes one of the format that can be decoded/encoded */
struct vdec_format {
	u32 pixfmt;
	u32 num_planes;
	u32 type;
	u32 min_buffers;
	u32 max_buffers;

	struct vdec_ops *vdec_ops;
	struct vdec_codec_ops *codec_ops;

	char *firmware_path;
};

struct vdec_session {
	struct vdec_core *core;
	
	struct v4l2_fh fh;
	struct v4l2_m2m_dev *m2m_dev;
	struct v4l2_m2m_ctx *m2m_ctx;
	struct mutex lock;
	struct mutex codec_lock;
	
	const struct vdec_format *fmt_out;
	const struct vdec_format *fmt_cap;
	u32 width;
	u32 height;
	u32 colorspace;
	u8 ycbcr_enc;
	u8 quantization;
	u8 xfer_func;

	u32 num_input_bufs;
	u32 num_output_bufs;

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

	/* Is set to 1 once the first keyframe has been parsed/decodeed */
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
	
	/* Buffers queued into the HW */
	struct list_head bufs;
	spinlock_t bufs_spinlock;

	/* Tracks last time we got a vdec IRQ */
	u64 last_irq_jiffies;

	/* Codec private data */
	void *priv;
};

u32 vdec_get_output_size(struct vdec_session *sess);
void vdec_dst_buf_done_idx(struct vdec_session *sess, u32 buf_idx);
void vdec_dst_buf_done(struct vdec_session *sess, struct vb2_v4l2_buffer *vbuf);
void vdec_add_ts_reorder(struct vdec_session *sess, u64 ts);
void vdec_remove_ts(struct vdec_session *sess, u64 ts);
void vdec_queue_recycle(struct vdec_session *sess, struct vb2_buffer *vb);
void vdec_abort(struct vdec_session *sess);

#endif
