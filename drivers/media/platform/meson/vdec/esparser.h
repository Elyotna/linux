/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 */

#ifndef __MESON_VDEC_ESPARSER_H_
#define __MESON_VDEC_ESPARSER_H_

#include "vdec.h"

int esparser_init(struct platform_device *pdev, struct amvdec_core *core);
int esparser_power_up(struct amvdec_session *sess);

/**
 * esparser_queue_eos() - write End Of Stream sequence to the ESPARSER
 *
 * @core vdec core struct
 */
int esparser_queue_eos(struct amvdec_core *core);

/**
 * esparser_queue_all_src() - work handler that writes as many src buffers
 * as possible to the ESPARSER
 */
void esparser_queue_all_src(struct work_struct *work);

#endif