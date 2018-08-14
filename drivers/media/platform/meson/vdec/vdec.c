// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 */

#define DEBUG

#include <linux/of_device.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mfd/syscon.h>
#include <linux/slab.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-dev.h>
#include <media/videobuf2-dma-contig.h>

#include "vdec.h"
#include "esparser.h"
#include "canvas.h"

#include "vdec_1.h"

/* 16 MiB for parsed bitstream swap exchange */
#define SIZE_VIFIFO (16 * SZ_1M)

void vdec_abort(struct vdec_session *sess)
{
	dev_info(sess->core->dev, "Aborting decoding session!\n");
	vb2_queue_error(&sess->m2m_ctx->cap_q_ctx.q);
	vb2_queue_error(&sess->m2m_ctx->out_q_ctx.q);
}

static u32 get_output_size(u32 width, u32 height)
{
	return ALIGN(width * height, 64 * SZ_1K);
}

u32 vdec_get_output_size(struct vdec_session *sess)
{
	return get_output_size(sess->width, sess->height);
}

static int vdec_codec_needs_recycle(struct vdec_session *sess)
{
	struct vdec_codec_ops *codec_ops = sess->fmt_out->codec_ops;
	return codec_ops->can_recycle && codec_ops->recycle;
}

static int vdec_recycle_thread(void *data)
{
	struct vdec_session *sess = data;
	struct vdec_core *core = sess->core;
	struct vdec_codec_ops *codec_ops = sess->fmt_out->codec_ops;
	struct vdec_buffer *tmp, *n;

	while (!kthread_should_stop()) {
		mutex_lock(&sess->bufs_recycle_lock);

		list_for_each_entry_safe(tmp, n, &sess->bufs_recycle, list) {
			if (!codec_ops->can_recycle(core) ||
			    sess->num_recycle < 2)
				break;

			codec_ops->recycle(core, tmp->vb->index);
			dev_dbg(core->dev, "Buffer %d recycled\n",
				tmp->vb->index);
			list_del(&tmp->list);
			kfree(tmp);
			sess->num_recycle--;
		}
		mutex_unlock(&sess->bufs_recycle_lock);

		usleep_range(5000, 10000);
	}

	return 0;
}

static int vdec_poweron(struct vdec_session *sess)
{
	int ret;
	struct vdec_ops *vdec_ops = sess->fmt_out->vdec_ops;

	ret = clk_prepare_enable(sess->core->dos_parser_clk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(sess->core->dos_clk);
	if (ret)
		goto disable_dos_parser;

	ret = vdec_ops->start(sess);
	if (ret)
		goto disable_dos;

	esparser_power_up(sess);

	return 0;

disable_dos:
	clk_disable_unprepare(sess->core->dos_clk);
disable_dos_parser:
	clk_disable_unprepare(sess->core->dos_parser_clk);

	return ret;
}

static void vdec_poweroff(struct vdec_session *sess) {
	struct vdec_ops *vdec_ops = sess->fmt_out->vdec_ops;

	vdec_ops->stop(sess);
	clk_disable_unprepare(sess->core->dos_clk);
	clk_disable_unprepare(sess->core->dos_parser_clk);
}

void vdec_queue_recycle(struct vdec_session *sess, struct vb2_buffer *vb)
{
	struct vdec_buffer *new_buf;

	new_buf = kmalloc(sizeof(struct vdec_buffer), GFP_KERNEL);
	new_buf->vb = vb;

	mutex_lock(&sess->bufs_recycle_lock);
	list_add_tail(&new_buf->list, &sess->bufs_recycle);
	sess->num_recycle++;
	mutex_unlock(&sess->bufs_recycle_lock);
}

void vdec_m2m_device_run(void *priv)
{
	struct vdec_session *sess = priv;
	schedule_work(&sess->esparser_queue_work);
}

void vdec_m2m_job_abort(void *priv)
{
	struct vdec_session *sess = priv;
	v4l2_m2m_job_finish(sess->m2m_dev, sess->m2m_ctx);
}

static const struct v4l2_m2m_ops vdec_m2m_ops = {
	.device_run = vdec_m2m_device_run,
	.job_abort = vdec_m2m_job_abort,
};

static int vdec_queue_setup(struct vb2_queue *q,
		unsigned int *num_buffers, unsigned int *num_planes,
		unsigned int sizes[], struct device *alloc_devs[])
{
	struct vdec_session *sess = vb2_get_drv_priv(q);
	const struct vdec_format *fmt_out = sess->fmt_out;
	const struct vdec_format *fmt_cap = sess->fmt_cap;
	
	switch (q->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		sizes[0] = vdec_get_output_size(sess);
		sess->num_input_bufs = *num_buffers;
		*num_planes = fmt_out->num_planes;
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		if (fmt_cap->pixfmt == V4L2_PIX_FMT_NV12M) {
			sizes[0] = vdec_get_output_size(sess);
			sizes[1] = vdec_get_output_size(sess) / 2;
		} else if (fmt_cap->pixfmt == V4L2_PIX_FMT_YUV420M) {
			sizes[0] = vdec_get_output_size(sess);
			sizes[1] = vdec_get_output_size(sess) / 4;
			sizes[2] = vdec_get_output_size(sess) / 4;
		}
		*num_planes = fmt_cap->num_planes;
		*num_buffers = min(max(*num_buffers, fmt_out->min_buffers), fmt_out->max_buffers);
		sess->num_output_bufs = *num_buffers;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void vdec_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vdec_session *sess = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_m2m_ctx *m2m_ctx = sess->m2m_ctx;

	mutex_lock(&sess->lock);
	v4l2_m2m_buf_queue(m2m_ctx, vbuf);

	if (!sess->streamon_out || !sess->streamon_cap)
		goto unlock;

	if (vb->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
	    vdec_codec_needs_recycle(sess))
		vdec_queue_recycle(sess, vb);

	schedule_work(&sess->esparser_queue_work);
unlock:
	mutex_unlock(&sess->lock);
}

static int vdec_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct vdec_session *sess = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *buf;
	int ret;

	mutex_lock(&sess->lock);
	
	if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		sess->streamon_out = 1;
	else
		sess->streamon_cap = 1;

	if (!sess->streamon_out || !sess->streamon_cap) {
		mutex_unlock(&sess->lock);
		return 0;
	}

	sess->vififo_size = SIZE_VIFIFO;
	sess->vififo_vaddr =
		dma_alloc_coherent(sess->core->dev, sess->vififo_size,
				   &sess->vififo_paddr, GFP_KERNEL);
	if (!sess->vififo_vaddr) {
		dev_err(sess->core->dev, "Failed to request VIFIFO buffer\n");
		ret = -ENOMEM;
		goto bufs_done;
	}

	sess->should_stop = 0;
	ret = vdec_poweron(sess);
	if (ret)
		goto vififo_free;

	sess->sequence_cap = 0;
	sess->num_recycle = 0;
	sess->keyframe_found = 0;
	if (vdec_codec_needs_recycle(sess))
		sess->recycle_thread = kthread_run(vdec_recycle_thread, sess,
						   "vdec_recycle");
	mutex_unlock(&sess->lock);

	return 0;

vififo_free:
	dma_free_coherent(sess->core->dev, sess->vififo_size,
			  sess->vififo_vaddr, sess->vififo_paddr);
bufs_done:
	while ((buf = v4l2_m2m_src_buf_remove(sess->m2m_ctx)))
		v4l2_m2m_buf_done(buf, VB2_BUF_STATE_QUEUED);
	while ((buf = v4l2_m2m_dst_buf_remove(sess->m2m_ctx)))
		v4l2_m2m_buf_done(buf, VB2_BUF_STATE_QUEUED);

	if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		sess->streamon_out = 0;
	else
		sess->streamon_cap = 0;
	mutex_unlock(&sess->lock);
	return ret;
}

void vdec_stop_streaming(struct vb2_queue *q)
{
	struct vdec_session *sess = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *buf;

	mutex_lock(&sess->lock);

	if (sess->streamon_out && sess->streamon_cap) {
		if (vdec_codec_needs_recycle(sess))
			kthread_stop(sess->recycle_thread);
		vdec_poweroff(sess);
		dma_free_coherent(sess->core->dev, sess->vififo_size, sess->vififo_vaddr, sess->vififo_paddr);
		INIT_LIST_HEAD(&sess->bufs);
		INIT_LIST_HEAD(&sess->bufs_recycle);
		if (sess->priv) {
			kfree(sess->priv);
			sess->priv = NULL;
		}
	}

	if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		while ((buf = v4l2_m2m_src_buf_remove(sess->m2m_ctx)))
			v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);

		sess->streamon_out = 0;
	} else {
		while ((buf = v4l2_m2m_dst_buf_remove(sess->m2m_ctx)))
			v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);

		sess->streamon_cap = 0;
	}

	mutex_unlock(&sess->lock);
}

static const struct vb2_ops vdec_vb2_ops = {
	.queue_setup = vdec_queue_setup,
	.start_streaming = vdec_start_streaming,
	.stop_streaming = vdec_stop_streaming,
	.buf_queue = vdec_vb2_buf_queue,
};

static int
vdec_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	strlcpy(cap->driver, "meson-vdec", sizeof(cap->driver));
	strlcpy(cap->card, "AMLogic Video Decoder", sizeof(cap->card));
	strlcpy(cap->bus_info, "platform:meson-vdec", sizeof(cap->bus_info));

	return 0;
}

static const struct vdec_format *
find_format(const struct vdec_format *fmts, u32 size, u32 pixfmt, u32 type)
{
	unsigned int i;

	for (i = 0; i < size; i++) {
		if (fmts[i].pixfmt == pixfmt)
			break;
	}

	if (i == size || fmts[i].type != type)
		return NULL;

	return &fmts[i];
}

static const struct vdec_format *
find_format_by_index(const struct vdec_format *fmts, u32 size, u32 index, u32 type)
{
	unsigned int i, k = 0;

	if (index > size)
		return NULL;

	for (i = 0; i < size; i++) {
		if (fmts[i].type != type)
			continue;
		if (k == index)
			break;
		k++;
	}

	if (i == size)
		return NULL;

	return &fmts[i];
}

static const struct vdec_format *
vdec_try_fmt_common(struct vdec_session *sess, u32 size, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pixmp = &f->fmt.pix_mp;
	struct v4l2_plane_pix_format *pfmt = pixmp->plane_fmt;
	const struct vdec_format *fmts = sess->core->platform->formats;
	const struct vdec_format *fmt;

	memset(pfmt[0].reserved, 0, sizeof(pfmt[0].reserved));
	memset(pixmp->reserved, 0, sizeof(pixmp->reserved));

	if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		/* hack: MJPEG only supports YUV420M */
		if (sess->fmt_out->pixfmt == V4L2_PIX_FMT_MJPEG)
			pixmp->pixelformat = V4L2_PIX_FMT_YUV420M;

		/* hack: HEVC only supports NV12M */
		if (sess->fmt_out->pixfmt == V4L2_PIX_FMT_HEVC)
			pixmp->pixelformat = V4L2_PIX_FMT_NV12M;
	}

	fmt = find_format(fmts, size, pixmp->pixelformat, f->type);
	if (!fmt) {
		if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
			pixmp->pixelformat = V4L2_PIX_FMT_NV12M;
		else if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
			pixmp->pixelformat = V4L2_PIX_FMT_H264;
		else
			return NULL;

		fmt = find_format(fmts, size, pixmp->pixelformat, f->type);
		pixmp->width = 1280;
		pixmp->height = 720;
	}

	pixmp->width  = clamp(pixmp->width,  (u32)256, (u32)3840);
	pixmp->height = clamp(pixmp->height, (u32)144, (u32)2160);

	if (pixmp->field == V4L2_FIELD_ANY)
		pixmp->field = V4L2_FIELD_NONE;

	pixmp->num_planes = fmt->num_planes;
	pixmp->flags = 0;

	if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		memset(pfmt[1].reserved, 0, sizeof(pfmt[1].reserved));
		if (pixmp->pixelformat == V4L2_PIX_FMT_NV12M) {
			pfmt[0].sizeimage =
				get_output_size(pixmp->width, pixmp->height);
			pfmt[0].bytesperline = ALIGN(pixmp->width, 64);

			pfmt[1].sizeimage =
			      get_output_size(pixmp->width, pixmp->height) / 2;
			pfmt[1].bytesperline = ALIGN(pixmp->width, 64);
		} else if (pixmp->pixelformat == V4L2_PIX_FMT_YUV420M) {
			pfmt[0].sizeimage =
				get_output_size(pixmp->width, pixmp->height);
			pfmt[0].bytesperline = ALIGN(pixmp->width, 64);

			pfmt[1].sizeimage =
			      get_output_size(pixmp->width, pixmp->height) / 4;
			pfmt[1].bytesperline = ALIGN(pixmp->width, 64) / 2;

			pfmt[2].sizeimage =
			      get_output_size(pixmp->width, pixmp->height) / 4;
			pfmt[2].bytesperline = ALIGN(pixmp->width, 64) / 2;
		}
	} else {
		pfmt[0].sizeimage =
			get_output_size(pixmp->width, pixmp->height);
		pfmt[0].bytesperline = 0;
	}


	return fmt;
}

static int vdec_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vdec_session *sess = container_of(file->private_data, struct vdec_session, fh);

	vdec_try_fmt_common(sess,
		sess->core->platform->num_formats, f);

	return 0;
}

static int vdec_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vdec_session *sess = container_of(file->private_data, struct vdec_session, fh);
	const struct vdec_format *fmt = NULL;
	struct v4l2_pix_format_mplane *pixmp = &f->fmt.pix_mp;

	if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		fmt = sess->fmt_cap;
	else if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		fmt = sess->fmt_out;

	pixmp->pixelformat = fmt->pixfmt;

	if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		pixmp->width = sess->width;
		pixmp->height = sess->height;
		pixmp->colorspace = sess->colorspace;
		pixmp->ycbcr_enc = sess->ycbcr_enc;
		pixmp->quantization = sess->quantization;
		pixmp->xfer_func = sess->xfer_func;
	} else if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		pixmp->width = sess->width;
		pixmp->height = sess->height;
	}

	vdec_try_fmt_common(sess, sess->core->platform->num_formats, f);

	return 0;
}

static int vdec_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vdec_session *sess = container_of(file->private_data, struct vdec_session, fh);
	struct v4l2_pix_format_mplane *pixmp = &f->fmt.pix_mp;
	u32 num_formats = sess->core->platform->num_formats;
	const struct vdec_format *fmt;
	struct v4l2_pix_format_mplane orig_pixmp;
	struct v4l2_format format;
	u32 pixfmt_out = 0, pixfmt_cap = 0;

	orig_pixmp = *pixmp;

	fmt = vdec_try_fmt_common(sess, num_formats, f);

	if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		pixfmt_out = pixmp->pixelformat;
		pixfmt_cap = sess->fmt_cap->pixfmt;
	} else if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		pixfmt_cap = pixmp->pixelformat;
		pixfmt_out = sess->fmt_out->pixfmt;
	}

	memset(&format, 0, sizeof(format));

	format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	format.fmt.pix_mp.pixelformat = pixfmt_out;
	format.fmt.pix_mp.width = orig_pixmp.width;
	format.fmt.pix_mp.height = orig_pixmp.height;
	vdec_try_fmt_common(sess, num_formats, &format);

	if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		sess->width = format.fmt.pix_mp.width;
		sess->height = format.fmt.pix_mp.height;
		sess->colorspace = pixmp->colorspace;
		sess->ycbcr_enc = pixmp->ycbcr_enc;
		sess->quantization = pixmp->quantization;
		sess->xfer_func = pixmp->xfer_func;
	}

	memset(&format, 0, sizeof(format));

	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	format.fmt.pix_mp.pixelformat = pixfmt_cap;
	format.fmt.pix_mp.width = orig_pixmp.width;
	format.fmt.pix_mp.height = orig_pixmp.height;
	vdec_try_fmt_common(sess, num_formats, &format);

	sess->width = format.fmt.pix_mp.width;
	sess->height = format.fmt.pix_mp.height;

	if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		sess->fmt_out = fmt;
	else if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		sess->fmt_cap = fmt;

	return 0;
}

static int vdec_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	struct vdec_session *sess =
		container_of(file->private_data, struct vdec_session, fh);
	const struct vdec_platform *platform = sess->core->platform;
	const struct vdec_format *fmt;

	memset(f->reserved, 0, sizeof(f->reserved));

	fmt = find_format_by_index(platform->formats, platform->num_formats,
				   f->index, f->type);
	if (!fmt)
		return -EINVAL;

	f->pixelformat = fmt->pixfmt;

	return 0;
}

static int vdec_enum_framesizes(struct file *file, void *fh,
				struct v4l2_frmsizeenum *fsize)
{
	struct vdec_session *sess =
		container_of(file->private_data, struct vdec_session, fh);
	const struct vdec_format *formats = sess->core->platform->formats;
	u32 num_formats = sess->core->platform->num_formats;
	const struct vdec_format *fmt;

	fmt = find_format(formats, num_formats, fsize->pixel_format,
			  V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (!fmt) {
		fmt = find_format(formats, num_formats, fsize->pixel_format,
				  V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
		if (!fmt)
			return -EINVAL;
	}

	if (fsize->index)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;

	/* TODO: Store these constants in vdec_format */
	fsize->stepwise.min_width = 256;
	fsize->stepwise.max_width = 3840;
	fsize->stepwise.step_width = 1;
	fsize->stepwise.min_height = 144;
	fsize->stepwise.max_height = 2160;
	fsize->stepwise.step_height = 1;

	return 0;
}

static int
vdec_try_decoder_cmd(struct file *file, void *fh, struct v4l2_decoder_cmd *cmd)
{
	switch (cmd->cmd) {
	case V4L2_DEC_CMD_STOP:
		if (cmd->flags & V4L2_DEC_CMD_STOP_TO_BLACK)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int
vdec_decoder_cmd(struct file *file, void *fh, struct v4l2_decoder_cmd *cmd)
{
	struct vdec_session *sess =
		container_of(file->private_data, struct vdec_session, fh);
	int ret;

	ret = vdec_try_decoder_cmd(file, fh, cmd);
	if (ret)
		return ret;

	mutex_lock(&sess->lock);

	if (!(sess->streamon_out & sess->streamon_cap))
		goto unlock;

	dev_dbg(sess->core->dev, "Received V4L2_DEC_CMD_STOP\n");
	sess->should_stop = 1;

	/* We don't want to trigger EOS as long as we are still getting
	 * IRQs from the current decode session.
	 * EOS will be sent once there is no more vdec activity to purge the
	 * last few frames.
	 * We consider 100ms with no IRQ to be inactive.
	 */
	while (time_is_after_jiffies64(
	       sess->last_irq_jiffies + msecs_to_jiffies(100)))
		msleep(20);

	esparser_queue_eos(sess);

unlock:
	mutex_unlock(&sess->lock);
	return ret;
}

static int vdec_subscribe_event(struct v4l2_fh *fh,
				const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_EOS:
		return v4l2_event_subscribe(fh, sub, 2, NULL);
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ioctl_ops vdec_ioctl_ops = {
	.vidioc_querycap = vdec_querycap,
	.vidioc_enum_fmt_vid_cap_mplane = vdec_enum_fmt,
	.vidioc_enum_fmt_vid_out_mplane = vdec_enum_fmt,
	.vidioc_s_fmt_vid_cap_mplane = vdec_s_fmt,
	.vidioc_s_fmt_vid_out_mplane = vdec_s_fmt,
	.vidioc_g_fmt_vid_cap_mplane = vdec_g_fmt,
	.vidioc_g_fmt_vid_out_mplane = vdec_g_fmt,
	.vidioc_try_fmt_vid_cap_mplane = vdec_try_fmt,
	.vidioc_try_fmt_vid_out_mplane = vdec_try_fmt,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
	.vidioc_enum_framesizes = vdec_enum_framesizes,
	.vidioc_subscribe_event = vdec_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
	.vidioc_try_decoder_cmd = vdec_try_decoder_cmd,
	.vidioc_decoder_cmd = vdec_decoder_cmd,
};

static int m2m_queue_init(void *priv, struct vb2_queue *src_vq,
			  struct vb2_queue *dst_vq)
{
	struct vdec_session *sess = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->ops = &vdec_vb2_ops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->drv_priv = sess;
	src_vq->buf_struct_size = sizeof(struct dummy_buf);
	src_vq->allow_zero_bytesused = 1;
	src_vq->min_buffers_needed = 1;
	src_vq->dev = sess->core->dev;
	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->ops = &vdec_vb2_ops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->drv_priv = sess;
	dst_vq->buf_struct_size = sizeof(struct dummy_buf);
	dst_vq->allow_zero_bytesused = 1;
	dst_vq->min_buffers_needed = 1;
	dst_vq->dev = sess->core->dev;
	ret = vb2_queue_init(dst_vq);
	if (ret) {
		vb2_queue_release(src_vq);
		return ret;
	}

	return 0;
}

static int vdec_open(struct file *file)
{
	struct vdec_core *core = video_drvdata(file);
	struct device *dev = core->dev;
	const struct vdec_format *formats = core->platform->formats;
	struct vdec_session *sess;
	int ret;

	mutex_lock(&core->lock);
	if (core->cur_sess) {
		mutex_unlock(&core->lock);
		return -EBUSY;
	}

	sess = kzalloc(sizeof(*sess), GFP_KERNEL);
	if (!sess) {
		mutex_unlock(&core->lock);
		return -ENOMEM;
	}

	core->cur_sess = sess;
	mutex_unlock(&core->lock);

	sess->core = core;
	sess->fmt_cap = &formats[0];
	sess->fmt_out = &formats[2];
	sess->width = 1280;
	sess->height = 720;
	INIT_LIST_HEAD(&sess->bufs);
	INIT_LIST_HEAD(&sess->bufs_recycle);
	INIT_WORK(&sess->esparser_queue_work, esparser_queue_all_src);
	spin_lock_init(&sess->bufs_spinlock);
	mutex_init(&sess->lock);
	mutex_init(&sess->codec_lock);
	mutex_init(&sess->bufs_recycle_lock);

	sess->m2m_dev = v4l2_m2m_init(&vdec_m2m_ops);
	if (IS_ERR(sess->m2m_dev)) {
		dev_err(dev, "Fail to v4l2_m2m_init\n");
		ret = PTR_ERR(sess->m2m_dev);
		goto err_free_sess;
	}

	sess->m2m_ctx = v4l2_m2m_ctx_init(sess->m2m_dev, sess, m2m_queue_init);
	if (IS_ERR(sess->m2m_ctx)) {
		dev_err(dev, "Fail to v4l2_m2m_ctx_init\n");
		ret = PTR_ERR(sess->m2m_ctx);
		goto err_m2m_release;
	}

	v4l2_fh_init(&sess->fh, core->vdev_dec);
	v4l2_fh_add(&sess->fh);
	sess->fh.m2m_ctx = sess->m2m_ctx;
	file->private_data = &sess->fh;

	return 0;

err_m2m_release:
	v4l2_m2m_release(sess->m2m_dev);
err_free_sess:
	kfree(sess);
	return ret;
}

static int vdec_close(struct file *file)
{
	struct vdec_session *sess =
		container_of(file->private_data, struct vdec_session, fh);
	struct vdec_core *core = sess->core;

	v4l2_m2m_ctx_release(sess->m2m_ctx);
	v4l2_m2m_release(sess->m2m_dev);
	v4l2_fh_del(&sess->fh);
	v4l2_fh_exit(&sess->fh);
	mutex_destroy(&sess->lock);

	kfree(sess);
	core->cur_sess = NULL;

	return 0;
}

void vdec_dst_buf_done(struct vdec_session *sess, struct vb2_v4l2_buffer *vbuf)
{
	unsigned long flags;
	struct vdec_timestamp *tmp;
	struct device *dev = sess->core->dev_dec;

	spin_lock_irqsave(&sess->bufs_spinlock, flags);
	if (list_empty(&sess->bufs)) {
		dev_err(dev, "Buffer %u done but list is empty\n",
			vbuf->vb2_buf.index);

		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
		vdec_abort(sess);
		goto unlock;
	}

	tmp = list_first_entry(&sess->bufs, struct vdec_timestamp, list);

	vbuf->vb2_buf.planes[0].bytesused = vdec_get_output_size(sess);
	vbuf->vb2_buf.planes[1].bytesused = vdec_get_output_size(sess) / 2;
	vbuf->vb2_buf.timestamp = tmp->ts;
	vbuf->sequence = sess->sequence_cap++;

	list_del(&tmp->list);
	kfree(tmp);

	if (sess->should_stop && list_empty(&sess->bufs)) {
		const struct v4l2_event ev = { .type = V4L2_EVENT_EOS };
		dev_dbg(dev, "Signaling EOS\n");
		v4l2_event_queue_fh(&sess->fh, &ev);
		vbuf->flags |= V4L2_BUF_FLAG_LAST;
	}

	v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_DONE);
	atomic_dec(&sess->esparser_queued_bufs);

unlock:
	spin_unlock_irqrestore(&sess->bufs_spinlock, flags);

	/* Buffer done probably means the vififo got freed */
	schedule_work(&sess->esparser_queue_work);
}

static void vdec_rm_first_ts(struct vdec_session *sess)
{
	unsigned long flags;
	struct vdec_buffer *tmp;
	struct device *dev = sess->core->dev_dec;

	spin_lock_irqsave(&sess->bufs_spinlock, flags);
	if (list_empty(&sess->bufs)) {
		dev_err(dev, "Can't rm first timestamp: list empty\n");
		goto unlock;
	}

	tmp = list_first_entry(&sess->bufs, struct vdec_buffer, list);
	list_del(&tmp->list);
	kfree(tmp);

unlock:
	spin_unlock_irqrestore(&sess->bufs_spinlock, flags);
}

void vdec_dst_buf_done_idx(struct vdec_session *sess, u32 buf_idx)
{
	struct vb2_v4l2_buffer *vbuf;
	struct device *dev = sess->core->dev_dec;

	vbuf = v4l2_m2m_dst_buf_remove_by_idx(sess->m2m_ctx, buf_idx);
	if (!vbuf) {
		dev_err(dev, "Buffer %u done but it doesn't exist in m2m_ctx\n",
			buf_idx);
		atomic_dec(&sess->esparser_queued_bufs);
		vdec_rm_first_ts(sess);
		return;
	}

	vdec_dst_buf_done(sess, vbuf);
}

/* Userspace will queue src buffer timestamps that are not
 * in chronological order. Rearrange them here.
 */
void vdec_add_ts_reorder(struct vdec_session *sess, u64 ts)
{
	struct vdec_timestamp *new_ts, *tmp;
	unsigned long flags;

	new_ts = kmalloc(sizeof(*new_ts), GFP_KERNEL);
	new_ts->ts = ts;

	spin_lock_irqsave(&sess->bufs_spinlock, flags);

	if (list_empty(&sess->bufs))
		goto add_core;

	list_for_each_entry(tmp, &sess->bufs, list) {
		if (ts < tmp->ts) {
			list_add_tail(&new_ts->list, &tmp->list);
			goto unlock;
		}
	}

add_core:
	list_add_tail(&new_ts->list, &sess->bufs);
unlock:
	spin_unlock_irqrestore(&sess->bufs_spinlock, flags);
}

void vdec_remove_ts(struct vdec_session *sess, u64 ts)
{
	struct vdec_timestamp *tmp;
	unsigned long flags;

	spin_lock_irqsave(&sess->bufs_spinlock, flags);
	list_for_each_entry(tmp, &sess->bufs, list) {
		if (tmp->ts == ts) {
			list_del(&tmp->list);
			kfree(tmp);
			goto unlock;
		}
	}
	dev_warn(sess->core->dev_dec,
		"Couldn't remove buffer with timestamp %llu from list\n", ts);

unlock:
	spin_unlock_irqrestore(&sess->bufs_spinlock, flags);
}

static const struct v4l2_file_operations vdec_fops = {
	.owner = THIS_MODULE,
	.open = vdec_open,
	.release = vdec_close,
	.unlocked_ioctl = video_ioctl2,
	.poll = v4l2_m2m_fop_poll,
	.mmap = v4l2_m2m_fop_mmap,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = v4l2_compat_ioctl32,
#endif
};

static irqreturn_t vdec_isr(int irq, void *data)
{
	struct vdec_core *core = data;
	struct vdec_session *sess = core->cur_sess;

	sess->last_irq_jiffies = get_jiffies_64();

	return sess->fmt_out->codec_ops->isr(sess);
}

static irqreturn_t vdec_threaded_isr(int irq, void *data)
{
	struct vdec_core *core = data;
	struct vdec_session *sess = core->cur_sess;

	return sess->fmt_out->codec_ops->threaded_isr(sess);
}

static const struct of_device_id vdec_dt_match[] = {
	{ .compatible = "amlogic,gxbb-vdec",
	  .data = &vdec_platform_gxbb },
	{ .compatible = "amlogic,gxm-vdec",
	  .data = &vdec_platform_gxm },
	{ .compatible = "amlogic,gxl-vdec",
	  .data = &vdec_platform_gxl },
	{}
};
MODULE_DEVICE_TABLE(of, vdec_dt_match);

static int vdec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct video_device *vdev;
	struct vdec_core *core;
	struct resource *r;
	const struct of_device_id *of_id;
	int irq;
	int ret;

	core = devm_kzalloc(dev, sizeof(*core), GFP_KERNEL);
	if (!core) {
		dev_err(dev, "No memory for devm_kzalloc\n");
		return -ENOMEM;
	}

	core->dev = dev;
	platform_set_drvdata(pdev, core);

	r = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dos");
	core->dos_base = devm_ioremap_resource(dev, r);
	if (IS_ERR(core->dos_base)) {
		dev_err(dev, "Couldn't remap DOS memory\n");
		return PTR_ERR(core->dos_base);
	}

	r = platform_get_resource_byname(pdev, IORESOURCE_MEM, "esparser");
	core->esparser_base = devm_ioremap_resource(dev, r);
	if (IS_ERR(core->esparser_base)) {
		dev_err(dev, "Couldn't remap ESPARSER memory\n");
		return PTR_ERR(core->esparser_base);
	}

	r = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dmc");
	core->dmc_base = devm_ioremap(dev, r->start, resource_size(r));
	if (IS_ERR(core->dmc_base)) {
		dev_err(dev, "Couldn't remap DMC memory\n");
		return PTR_ERR(core->dmc_base);
	}

	core->regmap_ao = syscon_regmap_lookup_by_phandle(dev->of_node,
						"amlogic,ao-sysctrl");
	if (IS_ERR(core->regmap_ao)) {
		dev_err(dev, "Couldn't regmap AO sysctrl\n");
		return PTR_ERR(core->regmap_ao);
	}

	core->dos_parser_clk = devm_clk_get(dev, "dos_parser");
	if (IS_ERR(core->dos_parser_clk))
		return -EPROBE_DEFER;

	core->dos_clk = devm_clk_get(dev, "dos");
	if (IS_ERR(core->dos_clk))
		return -EPROBE_DEFER;

	core->vdec_1_clk = devm_clk_get(dev, "vdec_1");
	if (IS_ERR(core->vdec_1_clk))
		return -EPROBE_DEFER;

	core->vdec_hevc_clk = devm_clk_get(dev, "vdec_hevc");
	if (IS_ERR(core->vdec_hevc_clk))
		return -EPROBE_DEFER;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
		
	ret = devm_request_threaded_irq(core->dev, irq, vdec_isr,
			vdec_threaded_isr, IRQF_ONESHOT, "vdec", core);
	if (ret)
		return ret;

	ret = esparser_init(pdev, core);
	if (ret)
		return ret;

	ret = v4l2_device_register(dev, &core->v4l2_dev);
	if (ret) {
		dev_err(dev, "Couldn't register v4l2 device\n");
		return -ENOMEM;
	}

	vdev = video_device_alloc();
	if (!vdev) {
		ret = -ENOMEM;
		goto err_vdev_release;
	}

	strlcpy(vdev->name, "meson-video-decoder", sizeof(vdev->name));
	vdev->release = video_device_release;
	vdev->fops = &vdec_fops;
	vdev->ioctl_ops = &vdec_ioctl_ops;
	vdev->vfl_dir = VFL_DIR_M2M;
	vdev->v4l2_dev = &core->v4l2_dev;
	vdev->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;

	ret = video_register_device(vdev, VFL_TYPE_GRABBER, -1);
	if (ret) {
		dev_err(dev, "Failed registering video device\n");
		goto err_vdev_release;
	}

	of_id = of_match_node(vdec_dt_match, dev->of_node);
	core->platform = of_id->data;
	core->vdev_dec = vdev;
	core->dev_dec = dev;
	mutex_init(&core->lock);

	video_set_drvdata(vdev, core);

	return 0;

err_vdev_release:
	video_device_release(vdev);
	return ret;
}

static int vdec_remove(struct platform_device *pdev)
{
	struct vdec_core *core = platform_get_drvdata(pdev);

	video_unregister_device(core->vdev_dec);

	return 0;
}

static struct platform_driver meson_vdec_driver = {
	.probe = vdec_probe,
	.remove = vdec_remove,
	.driver = {
		.name = "meson-vdec",
		.of_match_table = vdec_dt_match,
	},
};
module_platform_driver(meson_vdec_driver);

MODULE_DESCRIPTION("Meson video decoder driver for GXBB/GXL/GXM");
MODULE_AUTHOR("Maxime Jourdan <maxi.jourdan@wanadoo.fr>");
MODULE_LICENSE("GPL");
