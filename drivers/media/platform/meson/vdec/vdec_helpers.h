#ifndef __MESON_VDEC_HELPERS_H_
#define __MESON_VDEC_HELPERS_H_

#include "vdec.h"

/**
 * amvdec_set_canvases() - Map VB2 buffers to canvases
 *
 * @sess: current session
 * @reg_base: Registry bases of where to write the canvas indexes
 * @reg_num: number of contiguous registers after each reg_base (including it)
 */
int amvdec_set_canvases(struct amvdec_session *sess,
			u32 reg_base[], u32 reg_num[]);

u32 amvdec_read_dos(struct amvdec_core *core, u32 reg);
void amvdec_write_dos(struct amvdec_core *core, u32 reg, u32 val);
void amvdec_write_dos_bits(struct amvdec_core *core, u32 reg, u32 val);
void amvdec_clear_dos_bits(struct amvdec_core *core, u32 reg, u32 val);
u32 amvdec_read_parser(struct amvdec_core *core, u32 reg);
void amvdec_write_parser(struct amvdec_core *core, u32 reg, u32 val);

u32 amvdec_am21c_body_size(u32 width, u32 height);
u32 amvdec_am21c_head_size(u32 width, u32 height);
u32 amvdec_am21c_size(u32 width, u32 height);

void amvdec_dst_buf_done_idx(struct amvdec_session *sess, u32 buf_idx,
			     u32 field);
void amvdec_dst_buf_done(struct amvdec_session *sess,
			 struct vb2_v4l2_buffer *vbuf, u32 field);

/**
 * amvdec_add_ts_reorder() - Add a timestamp to the list in chronological order
 *
 * @sess: current session
 * @ts: timestamp to add
 */
void amvdec_add_ts_reorder(struct amvdec_session *sess, u64 ts);
void amvdec_remove_ts(struct amvdec_session *sess, u64 ts);
void amvdec_rm_first_ts(struct amvdec_session *sess);

void amvdec_abort(struct amvdec_session *sess);
#endif