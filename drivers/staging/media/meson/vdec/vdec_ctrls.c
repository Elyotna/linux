// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 BayLibre, SAS
 * Author: Maxime Jourdan <mjourdan@baylibre.com>
 */

#include "vdec_ctrls.h"

int amvdec_init_ctrls(struct amvdec_session *sess)
{
	struct v4l2_ctrl_handler *ctrl_handler = &sess->ctrl_handler;
	int ret;

	ret = v4l2_ctrl_handler_init(ctrl_handler, 1);
	if (ret)
		return ret;

	sess->ctrl_min_buf_capture = v4l2_ctrl_new_std(ctrl_handler, NULL,
				V4L2_CID_MIN_BUFFERS_FOR_CAPTURE, 1, 32, 1, 1);

	ret = ctrl_handler->error;
	if (ret) {
		v4l2_ctrl_handler_free(ctrl_handler);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(amvdec_init_ctrls);
