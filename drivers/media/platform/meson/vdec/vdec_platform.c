// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 */

#include "vdec_platform.h"
#include "vdec.h"

#include "vdec_1.h"
#include "vdec_hevc.h"
#include "codec_mpeg12.h"
#include "codec_mpeg4.h"
#include "codec_mjpeg.h"
#include "codec_h264.h"
#include "codec_hevc.h"

static const struct vdec_format vdec_formats_gxbb[] = {
	{
		.pixfmt = V4L2_PIX_FMT_NV12M,
		.num_planes = 2,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
	}, {
		.pixfmt = V4L2_PIX_FMT_H264,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 16,
		.max_buffers = 32,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_h264_ops,
		.firmware_path = "meson/gxbb/vh264_mc",
	}, {
		.pixfmt = V4L2_PIX_FMT_HEVC,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 16,
		.max_buffers = 24,
		.vdec_ops = &vdec_hevc_ops,
		.codec_ops = &codec_hevc_ops,
		.firmware_path = "meson/gx/vh265_mc",
	}, {
		.pixfmt = V4L2_PIX_FMT_MPEG1,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 8,
		.max_buffers = 8,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mpeg12_ops,
		.firmware_path = "meson/gx/vmpeg12_mc",
	}, {
		.pixfmt = V4L2_PIX_FMT_MPEG2,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 8,
		.max_buffers = 8,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mpeg12_ops,
		.firmware_path = "meson/gx/vmpeg12_mc",
	}, {
		.pixfmt = V4L2_PIX_FMT_MPEG4,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 8,
		.max_buffers = 8,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mpeg4_ops,
		.firmware_path = "meson/gx/vmpeg4_mc_5",
	}, {
		.pixfmt = V4L2_PIX_FMT_H263,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 8,
		.max_buffers = 8,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mpeg4_ops,
		.firmware_path = "meson/gx/h263_mc",
	}, {
		.pixfmt = V4L2_PIX_FMT_XVID,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 8,
		.max_buffers = 8,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mpeg4_ops,
		.firmware_path = "meson/gx/vmpeg4_mc_5",
	}, {
		.pixfmt = V4L2_PIX_FMT_MJPEG,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 4,
		.max_buffers = 4,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mjpeg_ops,
		.firmware_path = "meson/gx/vmjpeg_mc",
	},
};

static const struct vdec_format vdec_formats_gxl[] = {
	{
		.pixfmt = V4L2_PIX_FMT_NV12M,
		.num_planes = 2,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
	}, {
		.pixfmt = V4L2_PIX_FMT_H264,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 16,
		.max_buffers = 32,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_h264_ops,
		.firmware_path = "meson/gxl/vh264_mc",
	}, {
		.pixfmt = V4L2_PIX_FMT_HEVC,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 16,
		.max_buffers = 24,
		.vdec_ops = &vdec_hevc_ops,
		.codec_ops = &codec_hevc_ops,
		.firmware_path = "meson/gx/vh265_mc",
	}, {
		.pixfmt = V4L2_PIX_FMT_MPEG1,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 8,
		.max_buffers = 8,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mpeg12_ops,
		.firmware_path = "meson/gx/vmpeg12_mc",
	}, {
		.pixfmt = V4L2_PIX_FMT_MPEG2,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 8,
		.max_buffers = 8,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mpeg12_ops,
		.firmware_path = "meson/gx/vmpeg12_mc",
	}, {
		.pixfmt = V4L2_PIX_FMT_MPEG4,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 8,
		.max_buffers = 8,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mpeg4_ops,
		.firmware_path = "meson/gx/vmpeg4_mc_5",
	}, {
		.pixfmt = V4L2_PIX_FMT_H263,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 8,
		.max_buffers = 8,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mpeg4_ops,
		.firmware_path = "meson/gx/h263_mc",
	}, {
		.pixfmt = V4L2_PIX_FMT_XVID,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 8,
		.max_buffers = 8,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mpeg4_ops,
		.firmware_path = "meson/gx/vmpeg4_mc_5",
	}, {
		.pixfmt = V4L2_PIX_FMT_MJPEG,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 4,
		.max_buffers = 4,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mjpeg_ops,
		.firmware_path = "meson/gx/vmjpeg_mc",
	},
};

static const struct vdec_format vdec_formats_gxm[] = {
	{
		.pixfmt = V4L2_PIX_FMT_NV12M,
		.num_planes = 2,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
	}, {
		.pixfmt = V4L2_PIX_FMT_H264,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 16,
		.max_buffers = 32,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_h264_ops,
		.firmware_path = "meson/gxm/vh264_mc",
	}, {
		.pixfmt = V4L2_PIX_FMT_HEVC,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 16,
		.max_buffers = 24,
		.vdec_ops = &vdec_hevc_ops,
		.codec_ops = &codec_hevc_ops,
		.firmware_path = "meson/gx/vh265_mc",
	}, {
		.pixfmt = V4L2_PIX_FMT_MPEG1,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 8,
		.max_buffers = 8,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mpeg12_ops,
		.firmware_path = "meson/gx/vmpeg12_mc",
	}, {
		.pixfmt = V4L2_PIX_FMT_MPEG2,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 8,
		.max_buffers = 8,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mpeg12_ops,
		.firmware_path = "meson/gx/vmpeg12_mc",
	}, {
		.pixfmt = V4L2_PIX_FMT_MPEG4,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 8,
		.max_buffers = 8,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mpeg4_ops,
		.firmware_path = "meson/gx/vmpeg4_mc_5",
	}, {
		.pixfmt = V4L2_PIX_FMT_H263,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 8,
		.max_buffers = 8,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mpeg4_ops,
		.firmware_path = "meson/gx/h263_mc",
	}, {
		.pixfmt = V4L2_PIX_FMT_XVID,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 8,
		.max_buffers = 8,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mpeg4_ops,
		.firmware_path = "meson/gx/vmpeg4_mc_5",
	}, {
		.pixfmt = V4L2_PIX_FMT_MJPEG,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.min_buffers = 4,
		.max_buffers = 4,
		.vdec_ops = &vdec_1_ops,
		.codec_ops = &codec_mjpeg_ops,
		.firmware_path = "meson/gx/vmjpeg_mc",
	},
};

const struct vdec_platform vdec_platform_gxbb = {
	.formats = vdec_formats_gxbb,
	.num_formats = ARRAY_SIZE(vdec_formats_gxbb),
	.revision = VDEC_REVISION_GXBB,
};

const struct vdec_platform vdec_platform_gxl = {
	.formats = vdec_formats_gxl,
	.num_formats = ARRAY_SIZE(vdec_formats_gxl),
	.revision = VDEC_REVISION_GXL,
};

const struct vdec_platform vdec_platform_gxm = {
	.formats = vdec_formats_gxm,
	.num_formats = ARRAY_SIZE(vdec_formats_gxm),
	.revision = VDEC_REVISION_GXM,
};