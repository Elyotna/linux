// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 */

#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "codec_helpers.h"
#include "canvas.h"

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

static void
codec_helper_set_canvas_yuv420m(struct amvdec_session *sess, void *reg_base)
{
	struct amvdec_core *core = sess->core;
	u32 width = ALIGN(sess->width, 64);
	u32 height = ALIGN(sess->height, 64);
	struct v4l2_m2m_buffer *buf;

	/* Setup YUV420 canvases for Decoded Picture Buffer (dpb)
	 * Map them to the user buffers' planes
	 */
	v4l2_m2m_for_each_dst_buf(sess->m2m_ctx, buf) {
		u32 buf_idx    = buf->vb.vb2_buf.index;
		u32 cnv_y_idx  = 128 + buf_idx * 3;
		u32 cnv_u_idx = cnv_y_idx + 1;
		u32 cnv_v_idx = cnv_y_idx + 2;
		dma_addr_t buf_y_paddr  =
			vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);
		dma_addr_t buf_u_paddr =
			vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 1);
		dma_addr_t buf_v_paddr =
			vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 2);

		/* Y plane */
		vdec_canvas_setup(core->dmc_base, cnv_y_idx, buf_y_paddr,
			width, height, MESON_CANVAS_WRAP_NONE,
			MESON_CANVAS_BLKMODE_LINEAR);

		/* U plane */
		vdec_canvas_setup(core->dmc_base, cnv_u_idx, buf_u_paddr,
			width / 2, height / 2, MESON_CANVAS_WRAP_NONE,
			MESON_CANVAS_BLKMODE_LINEAR);

		/* V plane */
		vdec_canvas_setup(core->dmc_base, cnv_v_idx, buf_v_paddr,
			width / 2, height / 2, MESON_CANVAS_WRAP_NONE,
			MESON_CANVAS_BLKMODE_LINEAR);

		writel_relaxed(((cnv_v_idx) << 16) |
			       ((cnv_u_idx) << 8)  |
				(cnv_y_idx), reg_base + buf_idx * 4);
	}
}

static void
codec_helper_set_canvas_nv12m(struct amvdec_session *sess, void *reg_base)
{
	struct amvdec_core *core = sess->core;
	u32 width = ALIGN(sess->width, 64);
	u32 height = ALIGN(sess->height, 64);
	struct v4l2_m2m_buffer *buf;

	/* Setup NV12 canvases for Decoded Picture Buffer (dpb)
	 * Map them to the user buffers' planes
	 */
	v4l2_m2m_for_each_dst_buf(sess->m2m_ctx, buf) {
		u32 buf_idx    = buf->vb.vb2_buf.index;
		u32 cnv_y_idx  = 128 + buf_idx * 2;
		u32 cnv_uv_idx = cnv_y_idx + 1;
		dma_addr_t buf_y_paddr  =
			vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);
		dma_addr_t buf_uv_paddr =
			vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 1);

		/* Y plane */
		vdec_canvas_setup(core->dmc_base, cnv_y_idx, buf_y_paddr,
			width, height, MESON_CANVAS_WRAP_NONE,
			MESON_CANVAS_BLKMODE_LINEAR);

		/* U/V plane */
		vdec_canvas_setup(core->dmc_base, cnv_uv_idx, buf_uv_paddr,
			width, height / 2, MESON_CANVAS_WRAP_NONE,
			MESON_CANVAS_BLKMODE_LINEAR);

		writel_relaxed(((cnv_uv_idx) << 16) |
			       ((cnv_uv_idx) << 8)  |
				(cnv_y_idx), reg_base + buf_idx * 4);
	}
}

void amcodec_helper_set_canvases(struct amvdec_session *sess, void *reg_base)
{
	u32 pixfmt = sess->pixfmt_cap;

	switch (pixfmt) {
	case V4L2_PIX_FMT_NV12M:
		codec_helper_set_canvas_nv12m(sess, reg_base);
		break;
	case V4L2_PIX_FMT_YUV420M:
		codec_helper_set_canvas_yuv420m(sess, reg_base);
		break;
	default:
		dev_err(sess->core->dev, "Unsupported pixfmt %08X\n", pixfmt);
	};
}
EXPORT_SYMBOL_GPL(amcodec_helper_set_canvases);