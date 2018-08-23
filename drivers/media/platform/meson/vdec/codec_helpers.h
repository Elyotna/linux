#ifndef __MESON_VDEC_CODEC_HELPERS_H_
#define __MESON_VDEC_CODEC_HELPERS_H_

#include "vdec.h"

/**
 * amcodec_helper_set_canvases() - Map VB2 buffers to canvases
 *
 * @sess: current session
 * @reg_base: Registry bases of where to write the canvas indexes
 * @reg_num: number of contiguous registers after each reg_base (including it)
 */
int amcodec_helper_set_canvases(struct amvdec_session *sess,
				u32 reg_base[], u32 reg_num[]);

u32 amcodec_am21c_body_size(u32 width, u32 height);
u32 amcodec_am21c_head_size(u32 width, u32 height);
u32 amcodec_am21c_size(u32 width, u32 height);

#endif