#ifndef __MESON_VDEC_CODEC_HELPERS_H_
#define __MESON_VDEC_CODEC_HELPERS_H_

#include "vdec.h"

void amcodec_helper_set_canvases(struct amvdec_session *sess, void *reg_base);
u32 amcodec_am21c_body_size(u32 width, u32 height);
u32 amcodec_am21c_head_size(u32 width, u32 height);
u32 amcodec_am21c_size(u32 width, u32 height);

#endif