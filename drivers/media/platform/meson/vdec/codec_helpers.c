// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 */

#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "codec_helpers.h"

#define NUM_CANVAS_NV12 2
#define NUM_CANVAS_YUV420 3

/* 4 KiB per 64x32 block */
u32 amcodec_am21c_body_size(u32 width, u32 height)
{
	u32 width_64 = ALIGN(width, 64) / 64;
	u32 height_32 = ALIGN(height, 32) / 32;

	return SZ_4K * width_64 * height_32;
}
EXPORT_SYMBOL_GPL(amcodec_am21c_body_size);

/* 32 bytes per 128x64 block */
u32 amcodec_am21c_head_size(u32 width, u32 height)
{
	u32 width_128 = ALIGN(width, 128) / 128;
	u32 height_64 = ALIGN(height, 64) / 64;

	return 32 * width_128 * height_64;
}
EXPORT_SYMBOL_GPL(amcodec_am21c_head_size);

u32 amcodec_am21c_size(u32 width, u32 height)
{
	return ALIGN(amcodec_am21c_body_size(width, height) +
		     amcodec_am21c_head_size(width, height), SZ_64K);
}
EXPORT_SYMBOL_GPL(amcodec_am21c_size);

static int amvdec_alloc_canvas(struct amvdec_session *sess, u8 *canvas_id) {
	int ret;

	if (sess->canvas_num >= MAX_CANVAS) {
		dev_err(sess->core->dev, "Reached max number of canvas\n");
		return -ENOMEM;
	}

	ret = meson_canvas_alloc(sess->core->canvas, canvas_id);
	if (ret)
		return ret;

	sess->canvas_alloc[sess->canvas_num++] = *canvas_id;
	return 0;
}

static int codec_helper_set_canvas_yuv420m(struct amvdec_session *sess,
					   struct vb2_buffer *vb, u32 width,
					   u32 height, u32 reg)
{
	struct amvdec_core *core = sess->core;
	u8 canvas_id[NUM_CANVAS_YUV420]; // Y U/V
	dma_addr_t buf_paddr[NUM_CANVAS_YUV420]; // Y U/V
	int ret, i;

	for (i = 0; i < NUM_CANVAS_YUV420; ++i) {
		ret = amvdec_alloc_canvas(sess, &canvas_id[i]);
		if (ret)
			return ret;

		buf_paddr[i] =
		    vb2_dma_contig_plane_dma_addr(vb, i);
	}

	/* Y plane */
	meson_canvas_config(core->canvas, canvas_id[0], buf_paddr[0],
		width, height, MESON_CANVAS_WRAP_NONE,
		MESON_CANVAS_BLKMODE_LINEAR,
		MESON_CANVAS_ENDIAN_SWAP64);

	/* U plane */
	meson_canvas_config(core->canvas, canvas_id[1], buf_paddr[1],
		width / 2, height / 2, MESON_CANVAS_WRAP_NONE,
		MESON_CANVAS_BLKMODE_LINEAR,
		MESON_CANVAS_ENDIAN_SWAP64);

	/* V plane */
	meson_canvas_config(core->canvas, canvas_id[2], buf_paddr[2],
		width / 2, height / 2, MESON_CANVAS_WRAP_NONE,
		MESON_CANVAS_BLKMODE_LINEAR,
		MESON_CANVAS_ENDIAN_SWAP64);

	amvdec_write_dos(core, reg,
			 ((canvas_id[2]) << 16) |
			 ((canvas_id[1]) << 8)  |
			 (canvas_id[0]));

	return 0;
}

static int codec_helper_set_canvas_nv12m(struct amvdec_session *sess,
					 struct vb2_buffer *vb, u32 width,
					 u32 height, u32 reg)
{
	struct amvdec_core *core = sess->core;
	u8 canvas_id[NUM_CANVAS_NV12]; // Y U/V
	dma_addr_t buf_paddr[NUM_CANVAS_NV12]; // Y U/V
	int ret, i;

	for (i = 0; i < NUM_CANVAS_NV12; ++i) {
		ret = amvdec_alloc_canvas(sess, &canvas_id[i]);
		if (ret)
			return ret;

		buf_paddr[i] =
		    vb2_dma_contig_plane_dma_addr(vb, i);
	}

	/* Y plane */
	meson_canvas_config(core->canvas, canvas_id[0], buf_paddr[0],
		width, height, MESON_CANVAS_WRAP_NONE,
		MESON_CANVAS_BLKMODE_LINEAR,
		MESON_CANVAS_ENDIAN_SWAP64);

	/* U/V plane */
	meson_canvas_config(core->canvas, canvas_id[1], buf_paddr[1],
		width, height / 2, MESON_CANVAS_WRAP_NONE,
		MESON_CANVAS_BLKMODE_LINEAR,
		MESON_CANVAS_ENDIAN_SWAP64);

	amvdec_write_dos(core, reg,
			 ((canvas_id[1]) << 16) |
			 ((canvas_id[1]) << 8)  |
			 (canvas_id[0]));

	return 0;
}

int amcodec_helper_set_canvases(struct amvdec_session *sess,
				u32 reg_base[], u32 reg_num[])
{
	struct v4l2_m2m_buffer *buf;
	u32 pixfmt = sess->pixfmt_cap;
	u32 width = ALIGN(sess->width, 64);
	u32 height = ALIGN(sess->height, 64);
	u32 reg_cur = reg_base[0];
	u32 reg_num_cur = 0;
	u32 reg_base_cur = 0;

	v4l2_m2m_for_each_dst_buf(sess->m2m_ctx, buf) {
		switch (pixfmt) {
		case V4L2_PIX_FMT_NV12M:
			return codec_helper_set_canvas_nv12m(sess, &buf->vb.vb2_buf, width, height, reg_cur);
			break;
		case V4L2_PIX_FMT_YUV420M:
			return codec_helper_set_canvas_yuv420m(sess, &buf->vb.vb2_buf, width, height, reg_cur);
			break;
		default:
			dev_err(sess->core->dev, "Unsupported pixfmt %08X\n",
				pixfmt);
			return -EINVAL;
		};

		reg_num_cur++;
		if (reg_num_cur >= reg_num[reg_base_cur]) {
			reg_base_cur++;
			reg_num_cur = 0;
		}

		if (!reg_base[reg_base_cur])
			return -EINVAL;

		reg_cur = reg_base[reg_base_cur] + reg_num_cur * 4;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(amcodec_helper_set_canvases);