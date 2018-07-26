/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 */

#ifndef __MESON_VDEC_ESPARSER_H_
#define __MESON_VDEC_ESPARSER_H_

#include "vdec.h"

int esparser_init(struct platform_device *pdev, struct vdec_core *core);
int esparser_power_up(struct vdec_session *sess);
int esparser_queue_eos(struct vdec_session *sess);
void esparser_queue_all_src(struct work_struct *work);

#endif