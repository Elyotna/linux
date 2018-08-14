// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Maxime Jourdan <maxi.jourdan@wanadoo.fr>
 * Copyright (C) 2015 Amlogic, Inc. All rights reserved.
 */

#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "codec_hevc.h"
#include "canvas.h"
#include "hevc_regs.h"

/* DOS registers */
#define ASSIST_MBOX1_CLR_REG 0x01d4
#define ASSIST_MBOX1_MASK    0x01d8

#define DOS_SW_RESET3        0xfcd0

/* HEVC reg mapping */
#define HEVC_DEC_STATUS_REG	HEVC_ASSIST_SCRATCH_0
	#define HEVC_ACTION_DONE	0xff
#define HEVC_RPM_BUFFER		HEVC_ASSIST_SCRATCH_1
#define HEVC_DECODE_INFO	HEVC_ASSIST_SCRATCH_1
#define HEVC_SHORT_TERM_RPS	HEVC_ASSIST_SCRATCH_2
#define HEVC_VPS_BUFFER		HEVC_ASSIST_SCRATCH_3
#define HEVC_SPS_BUFFER		HEVC_ASSIST_SCRATCH_4
#define HEVC_PPS_BUFFER		HEVC_ASSIST_SCRATCH_5
#define HEVC_SAO_UP		HEVC_ASSIST_SCRATCH_6
#define HEVC_STREAM_SWAP_BUFFER HEVC_ASSIST_SCRATCH_7
#define H265_MMU_MAP_BUFFER	HEVC_ASSIST_SCRATCH_7
#define HEVC_STREAM_SWAP_BUFFER2 HEVC_ASSIST_SCRATCH_8
#define HEVC_sao_mem_unit	HEVC_ASSIST_SCRATCH_9
#define HEVC_SAO_ABV		HEVC_ASSIST_SCRATCH_A
#define HEVC_sao_vb_size	HEVC_ASSIST_SCRATCH_B
#define HEVC_SAO_VB		HEVC_ASSIST_SCRATCH_C
#define HEVC_SCALELUT		HEVC_ASSIST_SCRATCH_D
#define HEVC_WAIT_FLAG		HEVC_ASSIST_SCRATCH_E
#define RPM_CMD_REG		HEVC_ASSIST_SCRATCH_F
#define LMEM_DUMP_ADR		HEVC_ASSIST_SCRATCH_F
#define DEBUG_REG1		HEVC_ASSIST_SCRATCH_G
#define HEVC_DECODE_MODE2	HEVC_ASSIST_SCRATCH_H
#define NAL_SEARCH_CTL		HEVC_ASSIST_SCRATCH_I
#define HEVC_DECODE_MODE	HEVC_ASSIST_SCRATCH_J
	#define DECODE_MODE_SINGLE 0
#define DECODE_STOP_POS		HEVC_ASSIST_SCRATCH_K
#define HEVC_AUX_ADR		HEVC_ASSIST_SCRATCH_L
#define HEVC_AUX_DATA_SIZE	HEVC_ASSIST_SCRATCH_M
#define HEVC_DECODE_SIZE	HEVC_ASSIST_SCRATCH_N

#define HEVCD_MPP_ANC2AXI_TBL_DATA (0x3464 * 4)

#define HEVC_CM_BODY_START_ADDR	(0x3626 * 4)
#define HEVC_CM_BODY_LENGTH	(0x3627 * 4)
#define HEVC_CM_HEADER_LENGTH	(0x3629 * 4)
#define HEVC_CM_HEADER_OFFSET	(0x362b * 4)

#define AMRISC_MAIN_REQ		 0x04

/* HEVC Constants */
#define MAX_REF_PIC_NUM		24
#define MAX_REF_ACTIVE		16
#define MPRED_MV_BUF_SIZE	0x120000
#define MAX_TILE_COL_NUM	10
#define MAX_TILE_ROW_NUM	20
#define MAX_SLICE_NUM		800
#define INVALID_POC		0x80000000

/* HEVC Workspace layout */
#define IPP_OFFSET       0x00
#define SAO_ABV_OFFSET   (IPP_OFFSET + 0x4000)
#define SAO_VB_OFFSET    (SAO_ABV_OFFSET + 0x30000)
#define SH_TM_RPS_OFFSET (SAO_VB_OFFSET + 0x30000)
#define VPS_OFFSET       (SH_TM_RPS_OFFSET + 0x800)
#define SPS_OFFSET       (VPS_OFFSET + 0x800)
#define PPS_OFFSET       (SPS_OFFSET + 0x800)
#define SAO_UP_OFFSET    (PPS_OFFSET + 0x2000)
#define SWAP_BUF_OFFSET  (SAO_UP_OFFSET + 0x800)
#define SWAP_BUF2_OFFSET (SWAP_BUF_OFFSET + 0x800)
#define SCALELUT_OFFSET  (SWAP_BUF2_OFFSET + 0x800)
#define DBLK_PARA_OFFSET (SCALELUT_OFFSET + 0x8000)
#define DBLK_DATA_OFFSET (DBLK_PARA_OFFSET + 0x20000)
#define MMU_VBH_OFFSET   (DBLK_DATA_OFFSET + 0x40000)
#define MPRED_ABV_OFFSET (MMU_VBH_OFFSET + 0x5000)
#define MPRED_MV_OFFSET  (MPRED_ABV_OFFSET + 0x8000)
#define RPM_OFFSET       (MPRED_MV_OFFSET + MPRED_MV_BUF_SIZE * MAX_REF_PIC_NUM)
#define LMEM_OFFSET      (RPM_OFFSET + 0x100)

/* ISR decode status */
#define HEVC_DEC_IDLE                        0x0
#define HEVC_NAL_UNIT_VPS                    0x1
#define HEVC_NAL_UNIT_SPS                    0x2
#define HEVC_NAL_UNIT_PPS                    0x3
#define HEVC_NAL_UNIT_CODED_SLICE_SEGMENT    0x4
#define HEVC_CODED_SLICE_SEGMENT_DAT         0x5
#define HEVC_SLICE_DECODING                  0x6
#define HEVC_NAL_UNIT_SEI                    0x7
#define HEVC_SLICE_SEGMENT_DONE              0x8
#define HEVC_NAL_SEARCH_DONE                 0x9
#define HEVC_DECPIC_DATA_DONE                0xa
#define HEVC_DECPIC_DATA_ERROR               0xb
#define HEVC_SEI_DAT                         0xc
#define HEVC_SEI_DAT_DONE                    0xd

/* RPM misc_flag0 */
#define PCM_LOOP_FILTER_DISABLED_FLAG_BIT		0
#define PCM_ENABLE_FLAG_BIT				1
#define LOOP_FILER_ACROSS_TILES_ENABLED_FLAG_BIT	2
#define PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT	3
#define DEBLOCKING_FILTER_OVERRIDE_ENABLED_FLAG_BIT	4
#define PPS_DEBLOCKING_FILTER_DISABLED_FLAG_BIT		5
#define DEBLOCKING_FILTER_OVERRIDE_FLAG_BIT		6
#define SLICE_DEBLOCKING_FILTER_DISABLED_FLAG_BIT	7
#define SLICE_SAO_LUMA_FLAG_BIT				8
#define SLICE_SAO_CHROMA_FLAG_BIT			9
#define SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT 10

/* Buffer sizes */
#define SIZE_WORKSPACE ALIGN(LMEM_OFFSET + 0xA00, 64 * SZ_1K)
#define SIZE_AUX (SZ_1K * 16)
#define SIZE_FRAME_MMU (0x1200 * 4)
#define RPM_SIZE 0x80
#define RPS_USED_BIT 14

#define PARSER_CMD_SKIP_CFG_0 0x0000090b
#define PARSER_CMD_SKIP_CFG_1 0x1b14140f
#define PARSER_CMD_SKIP_CFG_2 0x001b1910
static const uint16_t parser_cmd[] = {
	0x0401,	0x8401,	0x0800,	0x0402,
	0x9002,	0x1423,	0x8CC3,	0x1423,
	0x8804,	0x9825,	0x0800,	0x04FE,
	0x8406,	0x8411,	0x1800,	0x8408,
	0x8409,	0x8C2A,	0x9C2B,	0x1C00,
	0x840F,	0x8407,	0x8000,	0x8408,
	0x2000,	0xA800,	0x8410,	0x04DE,
	0x840C,	0x840D,	0xAC00,	0xA000,
	0x08C0,	0x08E0,	0xA40E,	0xFC00,
	0x7C00
};

/* Data received from the HW in this form, do not rearrange */
union rpm_param {
	struct {
		uint16_t data[RPM_SIZE];
	} l;
	struct {
		uint16_t CUR_RPS[MAX_REF_ACTIVE];
		uint16_t num_ref_idx_l0_active;
		uint16_t num_ref_idx_l1_active;
		uint16_t slice_type;
		uint16_t slice_temporal_mvp_enable_flag;
		uint16_t dependent_slice_segment_flag;
		uint16_t slice_segment_address;
		uint16_t num_title_rows_minus1;
		uint16_t pic_width_in_luma_samples;
		uint16_t pic_height_in_luma_samples;
		uint16_t log2_min_coding_block_size_minus3;
		uint16_t log2_diff_max_min_coding_block_size;
		uint16_t log2_max_pic_order_cnt_lsb_minus4;
		uint16_t POClsb;
		uint16_t collocated_from_l0_flag;
		uint16_t collocated_ref_idx;
		uint16_t log2_parallel_merge_level;
		uint16_t five_minus_max_num_merge_cand;
		uint16_t sps_num_reorder_pics_0;
		uint16_t modification_flag;
		uint16_t tiles_flags;
		uint16_t num_tile_columns_minus1;
		uint16_t num_tile_rows_minus1;
		uint16_t tile_width[8];
		uint16_t tile_height[8];
		uint16_t misc_flag0;
		uint16_t pps_beta_offset_div2;
		uint16_t pps_tc_offset_div2;
		uint16_t slice_beta_offset_div2;
		uint16_t slice_tc_offset_div2;
		uint16_t pps_cb_qp_offset;
		uint16_t pps_cr_qp_offset;
		uint16_t first_slice_segment_in_pic_flag;
		uint16_t m_temporalId;
		uint16_t m_nalUnitType;
		uint16_t vui_num_units_in_tick_hi;
		uint16_t vui_num_units_in_tick_lo;
		uint16_t vui_time_scale_hi;
		uint16_t vui_time_scale_lo;
		uint16_t bit_depth;
		uint16_t profile_etc;
		uint16_t sei_frame_field_info;
		uint16_t video_signal_type;
		uint16_t modification_list[0x20];
		uint16_t conformance_window_flag;
		uint16_t conf_win_left_offset;
		uint16_t conf_win_right_offset;
		uint16_t conf_win_top_offset;
		uint16_t conf_win_bottom_offset;
		uint16_t chroma_format_idc;
		uint16_t color_description;
		uint16_t aspect_ratio_idc;
		uint16_t sar_width;
		uint16_t sar_height;
	} p;
};

enum nal_unit_type {
	NAL_UNIT_CODED_SLICE_BLA	= 16,
	NAL_UNIT_CODED_SLICE_BLANT	= 17,
	NAL_UNIT_CODED_SLICE_BLA_N_LP	= 18,
	NAL_UNIT_CODED_SLICE_IDR	= 19,
	NAL_UNIT_CODED_SLICE_IDR_N_LP	= 20,
};

enum slice_type {
	B_SLICE = 0,
	P_SLICE = 1,
	I_SLICE = 2,
};

/* Refers to a frame being decoded */
struct hevc_frame {
	struct list_head list;
	struct vb2_v4l2_buffer *vbuf;
	u32 poc;

	int referenced;
	u32 num_reorder_pic;

	u32 cur_slice_idx;
	u32 cur_slice_type;

	/* 2 lists (L0/L1) ; 800 slices ; 16 refs */
	u32 ref_poc_list[2][MAX_SLICE_NUM][16];
	u32 ref_num[2];
};

struct hevc_tile {
	int width;
	int height;
	int start_cu_x;
	int start_cu_y;

	dma_addr_t sao_vb_start_addr;
	dma_addr_t sao_abv_start_addr;
};

struct codec_hevc {
	/* Current decoding status provided by the ISR */
	u32 dec_status;

	/* Buffer for the HEVC Workspace */
	void      *workspace_vaddr;
	dma_addr_t workspace_paddr;

	/* AUX buffer */
	void      *aux_vaddr;
	dma_addr_t aux_paddr;

	/* Frame MMU buffer (>= GXL) ; unused for now */
	void      *frame_mmu_vaddr;
	dma_addr_t frame_mmu_paddr;

	/* Contains many information parsed from the bitstream */
	union rpm_param rpm_param;

	/* Information computed from the RPM */
	u32 lcu_size; // Largest Coding Unit
	u32 lcu_x_num;
	u32 lcu_y_num;
	u32 lcu_total;

	/* Current Frame being handled */
	struct hevc_frame *cur_frame;
	u32 curr_poc;
	/* Collocated Reference Picture */
	struct hevc_frame *col_frame;
	u32 col_poc;

	/* All ref frames used by the HW at a given time */
	struct list_head ref_frames_list;
	u32 frames_num;

	/* Resolution reported by the hardware */
	u32 width;
	u32 height;

	u32 iPrevTid0POC;
	u32 slice_segment_addr;
	u32 slice_addr;
	u32 ldc_flag;

	/* Tiles */
	u32 num_tile_col;
	u32 num_tile_row;
	struct hevc_tile m_tile[MAX_TILE_ROW_NUM][MAX_TILE_COL_NUM];
	u32 tile_start_lcu_x;
	u32 tile_start_lcu_y;
	u32 tile_width_lcu;
	u32 tile_height_lcu;
};

static u32 codec_hevc_num_pending_bufs(struct vdec_session *sess)
{
	struct codec_hevc *hevc;
	u32 ret;

	mutex_lock(&sess->codec_lock);
	hevc = sess->priv;
	if (!hevc) {
		mutex_unlock(&sess->codec_lock);
		return 0;
	}

	ret = hevc->frames_num;
	mutex_unlock(&sess->codec_lock);

	return ret;
}

/* Update the L0 and L1 reference lists for a given frame */
static void codec_hevc_update_frame_refs(struct vdec_session *sess, struct hevc_frame *frame)
{
	struct codec_hevc *hevc = sess->priv;
	union rpm_param *params = &hevc->rpm_param;
	int i;
	int num_neg = 0;
	int num_pos = 0;
	int total_num;
	int num_ref_idx_l0_active =
		(params->p.num_ref_idx_l0_active > MAX_REF_ACTIVE) ?
		MAX_REF_ACTIVE : params->p.num_ref_idx_l0_active;
	int num_ref_idx_l1_active =
		(params->p.num_ref_idx_l1_active > MAX_REF_ACTIVE) ?
		MAX_REF_ACTIVE : params->p.num_ref_idx_l1_active;
	int ref_picset0[16] = { 0 };
	int ref_picset1[16] = { 0 };

	for (i = 0; i < 16; i++) {
		frame->ref_poc_list[0][frame->cur_slice_idx][i] = 0;
		frame->ref_poc_list[1][frame->cur_slice_idx][i] = 0;
	}

	for (i = 0; i < 16; i++) {
		u16 cur_rps = params->p.CUR_RPS[i];
		int delt = cur_rps & ((1 << (RPS_USED_BIT - 1)) - 1);

		if (cur_rps & 0x8000)
			break;

		if (!((cur_rps >> RPS_USED_BIT) & 1))
			continue;

		if ((cur_rps >> (RPS_USED_BIT - 1)) & 1) {
			ref_picset0[num_neg] =
			       frame->poc - ((1 << (RPS_USED_BIT - 1)) - delt);
			num_neg++;
		} else {
			ref_picset1[num_pos] = frame->poc + delt;
			num_pos++;
		}
	}

	total_num = num_neg + num_pos;

	if (total_num <= 0)
		goto end;

	for (i = 0; i < num_ref_idx_l0_active; i++) {
		int cIdx;
		if (params->p.modification_flag & 0x1)
			cIdx = params->p.modification_list[i];
		else
			cIdx = i % total_num;

		frame->ref_poc_list[0][frame->cur_slice_idx][i] =
			cIdx >= num_neg ? ref_picset1[cIdx - num_neg] :
			ref_picset0[cIdx];
	}

	if (params->p.slice_type != B_SLICE)
		goto end;

	if (params->p.modification_flag & 0x2) {
		for (i = 0; i < num_ref_idx_l1_active; i++) {
			int cIdx;
			if (params->p.modification_flag & 0x1)
				cIdx =
				params->p.modification_list[num_ref_idx_l0_active + i];
			else
				cIdx = params->p.modification_list[i];

			frame->ref_poc_list[1][frame->cur_slice_idx][i] =
				(cIdx >= num_pos) ? ref_picset0[cIdx - num_pos]
				: ref_picset1[cIdx];
		}
	} else {
		for (i = 0; i < num_ref_idx_l1_active; i++) {
			int cIdx = i % total_num;
			frame->ref_poc_list[1][frame->cur_slice_idx][i] =
				cIdx >= num_pos ? ref_picset0[cIdx - num_pos] :
				ref_picset1[cIdx];
		}
	}

end:
	frame->ref_num[0] = num_ref_idx_l0_active;
	frame->ref_num[1] = num_ref_idx_l1_active;

	dev_dbg(sess->core->dev,
		"Frame %u; slice %u; slice_type %u; num_l0 %u; num_l1 %u\n",
		frame->poc, frame->cur_slice_idx, params->p.slice_type,
		frame->ref_num[0], frame->ref_num[1]);
}

static void codec_hevc_update_ldc_flag(struct codec_hevc *hevc)
{
	struct hevc_frame *frame = hevc->cur_frame;
	u32 slice_type = frame->cur_slice_type;
	int i;

	hevc->ldc_flag = 0;

	if (slice_type == I_SLICE)
		return;

	hevc->ldc_flag = 1;
	for (i = 0; (i < frame->ref_num[0]) && hevc->ldc_flag; i++) {
		if (frame->ref_poc_list[0][frame->cur_slice_idx][i] > frame->poc) {
			hevc->ldc_flag = 0;
			break;
		}
	}

	if (slice_type == P_SLICE)
		return;

	for (i = 0; (i < frame->ref_num[1]) && hevc->ldc_flag; i++) {
		if (frame->ref_poc_list[1][frame->cur_slice_idx][i] > frame->poc) {
			hevc->ldc_flag = 0;
			break;
		}
	}
}

/* Tag "old" frames that are no longer referenced */
static void codec_hevc_update_referenced(struct codec_hevc *hevc)
{
	union rpm_param *param = &hevc->rpm_param;
	struct hevc_frame *frame;
	int i;
	u32 curr_poc = hevc->curr_poc;

	list_for_each_entry(frame, &hevc->ref_frames_list, list) {
		int is_referenced = 0;
		u32 poc_tmp;

		if (!frame->referenced)
			continue;

		for (i = 0; i < MAX_REF_ACTIVE; i++) {
			int delt;
			if (param->p.CUR_RPS[i] & 0x8000)
				break;

			delt = param->p.CUR_RPS[i] & ((1 << (RPS_USED_BIT - 1)) - 1);
			if (param->p.CUR_RPS[i] & (1 << (RPS_USED_BIT - 1))) {
				poc_tmp = curr_poc - ((1 << (RPS_USED_BIT - 1)) - delt);
			} else
				poc_tmp = curr_poc + delt;
			if (poc_tmp == frame->poc) {
				is_referenced = 1;
				break;
			}
		}

		frame->referenced = is_referenced;
	}
}

static struct hevc_frame *codec_hevc_get_lowest_poc_frame(struct codec_hevc *hevc)
{
	struct hevc_frame *tmp, *ret = NULL;
	u32 poc = INT_MAX;

	list_for_each_entry(tmp, &hevc->ref_frames_list, list) {
		if (tmp->poc < poc) {
			ret = tmp;
			poc = tmp->poc;
		}
	}

	return ret;
}

/* Try to output as many frames as possible */
static void codec_hevc_output_frames(struct vdec_session *sess)
{
	struct hevc_frame *tmp;
	struct codec_hevc *hevc = sess->priv;

	while ((tmp = codec_hevc_get_lowest_poc_frame(hevc))) {
		if (hevc->curr_poc &&
		 (tmp->referenced || tmp->num_reorder_pic >= hevc->frames_num))
			break;

		dev_dbg(sess->core->dev, "DONE frame poc %u; vbuf %u\n",
			tmp->poc, tmp->vbuf->vb2_buf.index);
		vdec_dst_buf_done(sess, tmp->vbuf);
		list_del(&tmp->list);
		kfree(tmp);
		hevc->frames_num--;
	}
}

/* Configure part of the IP responsible for frame buffer decompression */
static void codec_hevc_setup_decode_head(struct vdec_session *sess)
{
	struct vdec_core *core = sess->core;

	/* TODO */
	writel_relaxed(0, core->dos_base + HEVCD_MPP_DECOMP_CTL1);
	writel_relaxed(0, core->dos_base + HEVCD_MPP_DECOMP_CTL2);
	writel_relaxed(0, core->dos_base + HEVC_CM_BODY_LENGTH);
	writel_relaxed(0, core->dos_base + HEVC_CM_HEADER_OFFSET);
	writel_relaxed(0, core->dos_base + HEVC_CM_HEADER_LENGTH);
}

static void codec_hevc_setup_buffers_gxbb(struct vdec_session *sess)
{
	struct vdec_core *core = sess->core;
	struct v4l2_m2m_buffer *buf;
	u32 buf_size = v4l2_m2m_num_dst_bufs_ready(sess->m2m_ctx);
	dma_addr_t buf_y_paddr = 0;
	dma_addr_t buf_uv_paddr = 0;
	u32 idx = 0;
	u32 val;
	int i;

	writel_relaxed(0, core->dos_base + HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR);

	v4l2_m2m_for_each_dst_buf(sess->m2m_ctx, buf) {
		idx = buf->vb.vb2_buf.index;
		buf_y_paddr  = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);
		buf_uv_paddr = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 1);

		val = buf_y_paddr | ((idx * 2) << 8) | 1;
		writel_relaxed(val, core->dos_base + HEVCD_MPP_ANC2AXI_TBL_CMD_ADDR);
		val = buf_uv_paddr | ((idx * 2 + 1) << 8) | 1;
		writel_relaxed(val, core->dos_base + HEVCD_MPP_ANC2AXI_TBL_CMD_ADDR);
	}

	val = buf_y_paddr | ((idx * 2) << 8) | 1;
	/* Fill the remaining unused slots with the last buffer's Y addr */
	for (i = buf_size; i < MAX_REF_PIC_NUM; ++i)
		writel_relaxed(val, core->dos_base + HEVCD_MPP_ANC2AXI_TBL_CMD_ADDR);

	writel_relaxed(1, core->dos_base + HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR);
	writel_relaxed(1, core->dos_base + HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR);
	for (i = 0; i < 32; ++i)
		writel_relaxed(0, core->dos_base + HEVCD_MPP_ANC_CANVAS_DATA_ADDR);
}

static void codec_hevc_setup_buffers_gxl(struct vdec_session *sess)
{
	struct vdec_core *core = sess->core;
	struct v4l2_m2m_buffer *buf;
	u32 buf_size = v4l2_m2m_num_dst_bufs_ready(sess->m2m_ctx);
	dma_addr_t buf_y_paddr = 0;
	dma_addr_t buf_uv_paddr = 0;
	int i;

	writel_relaxed((1 << 2) | (1 << 1), core->dos_base + HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR);

	v4l2_m2m_for_each_dst_buf(sess->m2m_ctx, buf) {
		buf_y_paddr  = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);
		buf_uv_paddr = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 1);

		writel_relaxed(buf_y_paddr  >> 5, core->dos_base + HEVCD_MPP_ANC2AXI_TBL_DATA);
		writel_relaxed(buf_uv_paddr >> 5, core->dos_base + HEVCD_MPP_ANC2AXI_TBL_DATA);
	}

	/* Fill the remaining unused slots with the last buffer's Y addr */
	for (i = buf_size; i < MAX_REF_PIC_NUM; ++i) {
		writel_relaxed(buf_y_paddr  >> 5, core->dos_base + HEVCD_MPP_ANC2AXI_TBL_DATA);
		//writel_relaxed(buf_uv_paddr >> 5, core->dos_base + HEVCD_MPP_ANC2AXI_TBL_DATA);
	}

	writel_relaxed(1, core->dos_base + HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR);
	writel_relaxed(1, core->dos_base + HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR);
	for (i = 0; i < 32; ++i)
		writel_relaxed(0, core->dos_base + HEVCD_MPP_ANC_CANVAS_DATA_ADDR);
}

static int codec_hevc_setup_workspace(struct vdec_session *sess)
{
	struct vdec_core *core = sess->core;
	struct codec_hevc *hevc = sess->priv;

	/* Allocate some memory for the HEVC decoder's state */
	hevc->workspace_vaddr = dma_alloc_coherent(core->dev, SIZE_WORKSPACE, &hevc->workspace_paddr, GFP_KERNEL);
	if (!hevc->workspace_vaddr) {
		dev_err(core->dev, "Failed to allocate HEVC Workspace\n");
		return -ENOMEM;
	}

	writel_relaxed(hevc->workspace_paddr + IPP_OFFSET,
		core->dos_base + HEVCD_IPP_LINEBUFF_BASE);
	writel_relaxed(hevc->workspace_paddr + RPM_OFFSET,
		core->dos_base + HEVC_RPM_BUFFER);
	writel_relaxed(hevc->workspace_paddr + SH_TM_RPS_OFFSET,
		core->dos_base + HEVC_SHORT_TERM_RPS);
	writel_relaxed(hevc->workspace_paddr + VPS_OFFSET,
		core->dos_base + HEVC_VPS_BUFFER);
	writel_relaxed(hevc->workspace_paddr + SPS_OFFSET,
		core->dos_base + HEVC_SPS_BUFFER);
	writel_relaxed(hevc->workspace_paddr + PPS_OFFSET,
		core->dos_base + HEVC_PPS_BUFFER);
	writel_relaxed(hevc->workspace_paddr + SAO_UP_OFFSET,
		core->dos_base + HEVC_SAO_UP);

	/* MMU */
	//writel_relaxed(hevc->frame_mmu_paddr, core->dos_base + H265_MMU_MAP_BUFFER);
	/* No MMU */
	writel_relaxed(hevc->workspace_paddr + SWAP_BUF_OFFSET,
		core->dos_base + HEVC_STREAM_SWAP_BUFFER);

	writel_relaxed(hevc->workspace_paddr + SWAP_BUF2_OFFSET,
		core->dos_base + HEVC_STREAM_SWAP_BUFFER2);
	writel_relaxed(hevc->workspace_paddr + SCALELUT_OFFSET,
		core->dos_base + HEVC_SCALELUT);
	writel_relaxed(hevc->workspace_paddr + DBLK_PARA_OFFSET,
		core->dos_base + HEVC_DBLK_CFG4);
	writel_relaxed(hevc->workspace_paddr + DBLK_DATA_OFFSET,
		core->dos_base + HEVC_DBLK_CFG5);

	return 0;
}

static int codec_hevc_start(struct vdec_session *sess)
{
	struct vdec_core *core = sess->core;
	struct codec_hevc *hevc;
	int ret;
	int i;

	hevc = kzalloc(sizeof(*hevc), GFP_KERNEL);
	if (!hevc)
		return -ENOMEM;

	sess->priv = hevc;
	INIT_LIST_HEAD(&hevc->ref_frames_list);
	hevc->curr_poc = INVALID_POC;

	ret = codec_hevc_setup_workspace(sess);
	if (ret)
		goto free_hevc;

	writel_relaxed(0x5a5a55aa, core->dos_base + HEVC_PARSER_VERSION);
	writel_relaxed((1 << 14), core->dos_base + DOS_SW_RESET3);
	writel_relaxed(0, core->dos_base + HEVC_CABAC_CONTROL);
	writel_relaxed(0, core->dos_base + HEVC_PARSER_CORE_CONTROL);
	writel_relaxed(readl_relaxed(core->dos_base + HEVC_STREAM_CONTROL) | 1, core->dos_base + HEVC_STREAM_CONTROL);
	writel_relaxed(0x00000100, core->dos_base + HEVC_SHIFT_STARTCODE);
	writel_relaxed(0x00000300, core->dos_base + HEVC_SHIFT_EMULATECODE);
	writel_relaxed((readl_relaxed(core->dos_base + HEVC_PARSER_INT_CONTROL) & 0x03ffffff) |
			(3 << 29) | (2 << 26) | (1 << 24) | (1 << 22) | (1 << 7) | (1 << 4) | 1, core->dos_base + HEVC_PARSER_INT_CONTROL);
	writel_relaxed(readl_relaxed(core->dos_base + HEVC_SHIFT_STATUS) | (1 << 1) | 1, core->dos_base + HEVC_SHIFT_STATUS);
	writel_relaxed((3 << 6) | (2 << 4) | (2 << 1) | 1, core->dos_base + HEVC_SHIFT_CONTROL);
	writel_relaxed(1, core->dos_base + HEVC_CABAC_CONTROL);
	writel_relaxed(1, core->dos_base + HEVC_PARSER_CORE_CONTROL);
	writel_relaxed(0, core->dos_base + HEVC_DEC_STATUS_REG);

	writel_relaxed(0, core->dos_base + HEVC_IQIT_SCALELUT_WR_ADDR);
	for (i = 0; i < 1024; ++i)
		writel_relaxed(0, core->dos_base + HEVC_IQIT_SCALELUT_DATA);

	writel_relaxed(0, core->dos_base + HEVC_DECODE_SIZE);

	writel_relaxed((1 << 16), core->dos_base + HEVC_PARSER_CMD_WRITE);
	for (i = 0; i < ARRAY_SIZE(parser_cmd); ++i)
		writel_relaxed(parser_cmd[i], core->dos_base + HEVC_PARSER_CMD_WRITE);

	writel_relaxed(PARSER_CMD_SKIP_CFG_0, core->dos_base + HEVC_PARSER_CMD_SKIP_0);
	writel_relaxed(PARSER_CMD_SKIP_CFG_1, core->dos_base + HEVC_PARSER_CMD_SKIP_1);
	writel_relaxed(PARSER_CMD_SKIP_CFG_2, core->dos_base + HEVC_PARSER_CMD_SKIP_2);
	writel_relaxed((1 << 5) | (1 << 2) | 1, core->dos_base + HEVC_PARSER_IF_CONTROL);

	writel_relaxed(1, core->dos_base + HEVCD_IPP_TOP_CNTL);
	writel_relaxed((1 << 1), core->dos_base + HEVCD_IPP_TOP_CNTL);

	/* Enable 2-plane reference read mode for MC */
	if (sess->fmt_cap->pixfmt == V4L2_PIX_FMT_NV12M)
		writel_relaxed(1 << 31, core->dos_base + HEVCD_MPP_DECOMP_CTL1);

	writel_relaxed(1, core->dos_base + HEVC_WAIT_FLAG);

	/* clear mailbox interrupt */
	writel_relaxed(1, core->dos_base + HEVC_ASSIST_MBOX1_CLR_REG);
	/* enable mailbox interrupt */
	writel_relaxed(1, core->dos_base + HEVC_ASSIST_MBOX1_MASK);
	/* disable PSCALE for hardware sharing */
	writel_relaxed(0, core->dos_base + HEVC_PSCALE_CTRL);
	/* Let the uCode do all the parsing */
	writel_relaxed(0xc, core->dos_base + NAL_SEARCH_CTL);

	writel_relaxed(0, core->dos_base + DECODE_STOP_POS);
	writel_relaxed(DECODE_MODE_SINGLE, core->dos_base + HEVC_DECODE_MODE);
	writel_relaxed(0, core->dos_base + HEVC_DECODE_MODE2);

	/* AUX buffers */
	hevc->aux_vaddr = dma_alloc_coherent(core->dev, SIZE_AUX, &hevc->aux_paddr, GFP_KERNEL);
	if (!hevc->aux_vaddr) {
		dev_err(core->dev, "Failed to request HEVC AUX\n");
		ret = -ENOMEM;
		goto free_hevc;
	}

	writel_relaxed(hevc->aux_paddr, core->dos_base + HEVC_AUX_ADR);
	writel_relaxed((((SIZE_AUX) >> 4) << 16) | 0, core->dos_base + HEVC_AUX_DATA_SIZE);

	if (core->platform->revision == VDEC_REVISION_GXBB)
		codec_hevc_setup_buffers_gxbb(sess);
	else
		codec_hevc_setup_buffers_gxl(sess);

	if (sess->fmt_cap->pixfmt != V4L2_PIX_FMT_NV12M)
		codec_hevc_setup_decode_head(sess);

	return 0;

free_hevc:
	kfree(hevc);
	return ret;
}

static void codec_hevc_flush_output(struct vdec_session *sess)
{
	struct codec_hevc *hevc = sess->priv;
	struct hevc_frame *tmp, *n;

	list_for_each_entry_safe(tmp, n, &hevc->ref_frames_list, list) {
		vdec_dst_buf_done(sess, tmp->vbuf);
		list_del(&tmp->list);
		kfree(tmp);
		hevc->frames_num--;
	}
}

static int codec_hevc_stop(struct vdec_session *sess)
{
	struct codec_hevc *hevc = sess->priv;
	struct vdec_core *core = sess->core;

	mutex_lock(&sess->codec_lock);
	codec_hevc_flush_output(sess);

	if (hevc->workspace_vaddr) {
		dma_free_coherent(core->dev, SIZE_WORKSPACE,
				  hevc->workspace_vaddr,
				  hevc->workspace_paddr);
		hevc->workspace_vaddr = 0;
	}

	if (hevc->frame_mmu_vaddr) {
		dma_free_coherent(core->dev, SIZE_FRAME_MMU,
				  hevc->frame_mmu_vaddr,
				  hevc->frame_mmu_paddr);
		hevc->frame_mmu_vaddr = 0;
	}

	if (hevc->aux_vaddr) {
		dma_free_coherent(core->dev, SIZE_AUX,
				  hevc->aux_vaddr, hevc->aux_paddr);
		hevc->aux_vaddr = 0;
	}
	mutex_unlock(&sess->codec_lock);

	return 0;
}

static void codec_hevc_update_tiles(struct vdec_session *sess)
{
	struct codec_hevc *hevc = sess->priv;
	struct vdec_core *core = sess->core;
	u32 sao_mem_unit = (hevc->lcu_size == 16 ? 9 : hevc->lcu_size == 32 ? 14 : 24) << 4;
	u32 pic_height_cu = (hevc->height + hevc->lcu_size - 1) / hevc->lcu_size;
	u32 pic_width_cu = (hevc->width + hevc->lcu_size - 1) / hevc->lcu_size;
	u32 sao_vb_size = (sao_mem_unit + (2 << 4)) * pic_height_cu;
	u32 tiles_flags = hevc->rpm_param.p.tiles_flags;

	if (tiles_flags & 1) {
		/* TODO; The samples I'm using have tiles_flags == 0 */
		return;
	}

	hevc->num_tile_col = 1;
	hevc->num_tile_row = 1;
	hevc->m_tile[0][0].width = pic_width_cu;
	hevc->m_tile[0][0].height = pic_height_cu;
	hevc->m_tile[0][0].start_cu_x = 0;
	hevc->m_tile[0][0].start_cu_y = 0;
	hevc->m_tile[0][0].sao_vb_start_addr = hevc->workspace_paddr + SAO_VB_OFFSET;
	hevc->m_tile[0][0].sao_abv_start_addr = hevc->workspace_paddr + SAO_ABV_OFFSET;
	
	hevc->tile_start_lcu_x = 0;
	hevc->tile_start_lcu_y = 0;
	hevc->tile_width_lcu = pic_width_cu;
	hevc->tile_height_lcu = pic_height_cu;

	writel_relaxed(sao_mem_unit, core->dos_base + HEVC_sao_mem_unit);
	writel_relaxed(hevc->workspace_paddr + SAO_ABV_OFFSET, core->dos_base + HEVC_SAO_ABV);
	writel_relaxed(sao_vb_size, core->dos_base + HEVC_sao_vb_size);
	writel_relaxed(hevc->workspace_paddr + SAO_VB_OFFSET, core->dos_base + HEVC_SAO_VB);
}

static struct hevc_frame * codec_hevc_get_frame_by_poc(struct codec_hevc *hevc, u32 poc)
{
	struct hevc_frame *tmp;

	list_for_each_entry(tmp, &hevc->ref_frames_list, list) {
		if (tmp->poc == poc)
			return tmp;
	}

	return NULL;
}

static struct hevc_frame * codec_hevc_prepare_new_frame(struct vdec_session *sess)
{
	struct vb2_v4l2_buffer *vbuf;
	struct hevc_frame *new_frame = NULL;
	struct codec_hevc *hevc = sess->priv;
	union rpm_param *params = &hevc->rpm_param;

	new_frame = kzalloc(sizeof(*new_frame), GFP_KERNEL);
	if (!new_frame)
		return NULL;

	vbuf = v4l2_m2m_dst_buf_remove(sess->m2m_ctx);
	if (!vbuf) {
		dev_err(sess->core->dev, "No dst buffer available\n");
		return NULL;
	}

	new_frame->vbuf = vbuf;
	new_frame->referenced = 1;
	new_frame->poc = hevc->curr_poc;
	new_frame->cur_slice_type = params->p.slice_type;
	new_frame->num_reorder_pic = params->p.sps_num_reorder_pics_0;

	list_add_tail(&new_frame->list, &hevc->ref_frames_list);
	hevc->frames_num++;

	return new_frame;
}

static void codec_hevc_set_sao(struct vdec_session *sess, struct hevc_frame *frame)
{
	struct vdec_core *core = sess->core;
	struct codec_hevc *hevc = sess->priv;
	union rpm_param *param = &hevc->rpm_param;
	dma_addr_t buf_y_paddr = vb2_dma_contig_plane_dma_addr(&frame->vbuf->vb2_buf, 0);
	dma_addr_t buf_u_v_paddr = vb2_dma_contig_plane_dma_addr(&frame->vbuf->vb2_buf, 1);
	u32 misc_flag0 = param->p.misc_flag0;
	u32 slice_deblocking_filter_disabled_flag;
	u32 val, val_2;

	val = (readl_relaxed(core->dos_base + HEVC_SAO_CTRL0) & ~0xf) | ilog2(hevc->lcu_size);
	writel_relaxed(val, core->dos_base + HEVC_SAO_CTRL0);

	writel_relaxed(hevc->width | (hevc->height << 16), core->dos_base + HEVC_SAO_PIC_SIZE);
	writel_relaxed((hevc->lcu_x_num - 1) | (hevc->lcu_y_num - 1) << 16, core->dos_base + HEVC_SAO_PIC_SIZE_LCU);

	writel_relaxed(buf_y_paddr, core->dos_base + HEVC_SAO_Y_START_ADDR);
	writel_relaxed(vdec_get_output_size(sess), core->dos_base + HEVC_SAO_Y_LENGTH);
	writel_relaxed(buf_u_v_paddr, core->dos_base + HEVC_SAO_C_START_ADDR);
	writel_relaxed((vdec_get_output_size(sess) / 2), core->dos_base + HEVC_SAO_C_LENGTH);

	writel_relaxed(buf_y_paddr, core->dos_base + HEVC_SAO_Y_WPTR);
	writel_relaxed(buf_u_v_paddr, core->dos_base + HEVC_SAO_C_WPTR);

	if (frame->cur_slice_idx == 0) {
		writel_relaxed(hevc->width | (hevc->height << 16), core->dos_base + HEVC_DBLK_CFG2);

		val = 0;
		if ((misc_flag0 >> PCM_ENABLE_FLAG_BIT) & 0x1)
			val |= ((misc_flag0 >> PCM_LOOP_FILTER_DISABLED_FLAG_BIT) & 0x1) << 3;

		val |= (param->p.pps_cb_qp_offset & 0x1f) << 4;
		val |= (param->p.pps_cr_qp_offset & 0x1f) << 9;
		val |= (hevc->lcu_size == 64) ? 0 : ((hevc->lcu_size == 32) ? 1 : 2);
		writel_relaxed(val, core->dos_base + HEVC_DBLK_CFG1);
	}

	val = readl_relaxed(core->dos_base + HEVC_SAO_CTRL1) & ~0x3ff3;
	if (sess->fmt_cap->pixfmt == V4L2_PIX_FMT_NV12M)
		val |= 0xff0 | /* Set endianness for 2-bytes swaps (nv12) */
			0x1;   /* disable cm compression */
	else
		val |= 0x3000 | /* 64x32 block mode */
			0x880 | /* 64-bit Big Endian */
			0x2;    /* Disable double write */

	writel_relaxed(val, core->dos_base + HEVC_SAO_CTRL1);

	/* set them all 0 for H265_NV21 (no down-scale) */
	val = readl_relaxed(core->dos_base + HEVC_SAO_CTRL5) & ~0xff0000;
	writel_relaxed(val, core->dos_base + HEVC_SAO_CTRL5);

	val = readl_relaxed(core->dos_base + HEVCD_IPP_AXIIF_CONFIG) & ~0x30;
	val |= 0xf;
	if (sess->fmt_cap->pixfmt != V4L2_PIX_FMT_NV12M)
		val |= 0x30; /* 64x32 block mode */

	writel_relaxed(val, core->dos_base + HEVCD_IPP_AXIIF_CONFIG);

	val = 0;
	val_2 = readl_relaxed(core->dos_base + HEVC_SAO_CTRL0);
	val_2 &= (~0x300);

	/* TODO: handle tiles here if enabled */
	slice_deblocking_filter_disabled_flag = (misc_flag0 >>
			SLICE_DEBLOCKING_FILTER_DISABLED_FLAG_BIT) & 0x1;
	if ((misc_flag0 & (1 << DEBLOCKING_FILTER_OVERRIDE_ENABLED_FLAG_BIT))
		&& (misc_flag0 & (1 << DEBLOCKING_FILTER_OVERRIDE_FLAG_BIT))) {
		val |= slice_deblocking_filter_disabled_flag << 2;

		if (!slice_deblocking_filter_disabled_flag) {
			val |= (param->p.slice_beta_offset_div2 & 0xf) << 3;
			val |= (param->p.slice_tc_offset_div2 & 0xf) << 7;
		}
	} else {
		val |=
			((misc_flag0 >>
			  PPS_DEBLOCKING_FILTER_DISABLED_FLAG_BIT) & 0x1) << 2;

		if (((misc_flag0 >> PPS_DEBLOCKING_FILTER_DISABLED_FLAG_BIT) &
			 0x1) == 0) {
			val |= (param->p.pps_beta_offset_div2 & 0xf) << 3;
			val |= (param->p.pps_tc_offset_div2 & 0xf) << 7;
		}
	}
	if ((misc_flag0 & (1 << PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT))
		&& ((misc_flag0 & (1 << SLICE_SAO_LUMA_FLAG_BIT))
			|| (misc_flag0 & (1 << SLICE_SAO_CHROMA_FLAG_BIT))
			|| (!slice_deblocking_filter_disabled_flag))) {
		val |=
			((misc_flag0 >>
			  SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT)
			 & 0x1)	<< 1;
		val_2 |=
			((misc_flag0 >>
			  SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT)
			& 0x1) << 9;
	} else {
		val |=
			((misc_flag0 >>
			  PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT)
			 & 0x1) << 1;
		val_2 |=
			((misc_flag0 >>
			  PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT)
			 & 0x1) << 9;
	}

	writel_relaxed(val, core->dos_base + HEVC_DBLK_CFG9);
	writel_relaxed(val_2, core->dos_base + HEVC_SAO_CTRL0);
}

static dma_addr_t codec_hevc_get_frame_mv_paddr(struct codec_hevc *hevc, struct hevc_frame *frame)
{
	return hevc->workspace_paddr + MPRED_MV_OFFSET +
		(frame->vbuf->vb2_buf.index * MPRED_MV_BUF_SIZE);
}

/* Update the necessary information for motion prediction with the current slice */
static void codec_hevc_set_mpred(struct vdec_session *sess, struct hevc_frame *frame, struct hevc_frame *col_frame)
{
	struct vdec_core *core = sess->core;
	struct codec_hevc *hevc = sess->priv;
	union rpm_param *param = &hevc->rpm_param;
	u32 *ref_num = frame->ref_num;
	u32 *ref_poc_l0 = frame->ref_poc_list[0][frame->cur_slice_idx];
	u32 *ref_poc_l1 = frame->ref_poc_list[1][frame->cur_slice_idx];
	u32 lcu_size_log2 = ilog2(hevc->lcu_size);
	u32 mv_mem_unit = lcu_size_log2 == 6 ? 0x200 : lcu_size_log2 == 5 ? 0x80 : 0x20;
	u32 slice_segment_address = param->p.slice_segment_address;
	u32 max_num_merge_cand = 5 - param->p.five_minus_max_num_merge_cand;
	u32 plevel = param->p.log2_parallel_merge_level;
	u32 col_from_l0_flag = param->p.collocated_from_l0_flag;
	u32 tmvp_flag = param->p.slice_temporal_mvp_enable_flag;
	u32 is_next_slice_segment = param->p.dependent_slice_segment_flag ? 1 : 0;
	u32 slice_type = param->p.slice_type;
	dma_addr_t col_mv_rd_start_addr, col_mv_rd_ptr, col_mv_rd_end_addr;
	dma_addr_t mpred_mv_wr_ptr;
	u32 mv_rd_en = 1;
	u32 val;
	int i;

	val = readl_relaxed(core->dos_base + HEVC_MPRED_CURR_LCU);

	col_mv_rd_start_addr = codec_hevc_get_frame_mv_paddr(hevc, col_frame);
	mpred_mv_wr_ptr = codec_hevc_get_frame_mv_paddr(hevc, frame) + (hevc->slice_addr * mv_mem_unit);
	col_mv_rd_ptr = col_mv_rd_start_addr + (hevc->slice_addr * mv_mem_unit);
	col_mv_rd_end_addr = col_mv_rd_start_addr + ((hevc->lcu_x_num * hevc->lcu_y_num) * mv_mem_unit);

	writel_relaxed(codec_hevc_get_frame_mv_paddr(hevc, frame), core->dos_base + HEVC_MPRED_MV_WR_START_ADDR);
	writel_relaxed(col_mv_rd_start_addr, core->dos_base + HEVC_MPRED_MV_RD_START_ADDR);

	val = ((hevc->lcu_x_num - hevc->tile_width_lcu) * mv_mem_unit);
	writel_relaxed(val, core->dos_base + HEVC_MPRED_MV_WR_ROW_JUMP);
	writel_relaxed(val, core->dos_base + HEVC_MPRED_MV_RD_ROW_JUMP);

	if (slice_type == I_SLICE)
		mv_rd_en = 0;

	val = slice_type |
			  1 << 2 | // new pic
			  1 << 3 | // new tile
			  is_next_slice_segment << 4 |
			  tmvp_flag << 5 |
			  hevc->ldc_flag << 6 |
			  col_from_l0_flag << 7 |
			  1 << 9 |
			  1 << 10 |
			  mv_rd_en << 11 |
			  1 << 13 |
			  lcu_size_log2 << 16 |
			  3 << 20 | plevel << 24;
	writel_relaxed(val, core->dos_base + HEVC_MPRED_CTRL0);

	val = max_num_merge_cand | 2 << 4 | 3 << 8 | 5 << 12 | 36 << 16;
	writel_relaxed(val, core->dos_base + HEVC_MPRED_CTRL1);

	writel_relaxed(hevc->width | (hevc->height << 16), core->dos_base + HEVC_MPRED_PIC_SIZE);

	val = ((hevc->lcu_x_num - 1) | (hevc->lcu_y_num - 1) << 16);
	writel_relaxed(val, core->dos_base + HEVC_MPRED_PIC_SIZE_LCU);
	val = (hevc->tile_start_lcu_x | hevc->tile_start_lcu_y << 16);
	writel_relaxed(val, core->dos_base + HEVC_MPRED_TILE_START);
	val = (hevc->tile_width_lcu | hevc->tile_height_lcu << 16);
	writel_relaxed(val, core->dos_base + HEVC_MPRED_TILE_SIZE_LCU);

	writel_relaxed((ref_num[1] << 8) | ref_num[0], core->dos_base + HEVC_MPRED_REF_NUM);
	writel_relaxed((1 << ref_num[0]) - 1, core->dos_base + HEVC_MPRED_REF_EN_L0);
	writel_relaxed((1 << ref_num[1]) - 1, core->dos_base + HEVC_MPRED_REF_EN_L1);

	writel_relaxed(hevc->curr_poc, core->dos_base + HEVC_MPRED_CUR_POC);
	writel_relaxed(hevc->col_poc, core->dos_base + HEVC_MPRED_COL_POC);

	for (i = 0; i < MAX_REF_ACTIVE; ++i) {
		writel_relaxed(ref_poc_l0[i], core->dos_base + HEVC_MPRED_L0_REF00_POC + i * 4);
		writel_relaxed(ref_poc_l1[i], core->dos_base + HEVC_MPRED_L1_REF00_POC + i * 4);
	}

	if (slice_segment_address == 0) {
		writel_relaxed(hevc->workspace_paddr + MPRED_ABV_OFFSET, core->dos_base + HEVC_MPRED_ABV_START_ADDR);
		writel_relaxed(mpred_mv_wr_ptr, core->dos_base + HEVC_MPRED_MV_WPTR);
		writel_relaxed(col_mv_rd_start_addr, core->dos_base + HEVC_MPRED_MV_RPTR);
	} else {
		writel_relaxed(col_mv_rd_ptr, core->dos_base + HEVC_MPRED_MV_RPTR);
	}

	writel_relaxed(col_mv_rd_end_addr, core->dos_base + HEVC_MPRED_MV_RD_END_ADDR);
}

/*  motion compensation reference cache controller */
static void codec_hevc_set_mcrcc(struct vdec_session *sess)
{
	struct vdec_core *core = sess->core;
	struct codec_hevc *hevc = sess->priv;
	u32 val, val_2;
	int l0_cnt = hevc->cur_frame->ref_num[0];
	int l1_cnt = hevc->cur_frame->ref_num[1];

	/* reset mcrcc */
	writel_relaxed(0x02, core->dos_base + HEVCD_MCRCC_CTL1);

	if (hevc->cur_frame->cur_slice_type == I_SLICE) {
		/* remove reset -- disables clock */
		writel_relaxed(0, core->dos_base + HEVCD_MCRCC_CTL1);
		return;
	}

	if (hevc->cur_frame->cur_slice_type == P_SLICE) {
		writel_relaxed(1 << 1, core->dos_base + HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR);
		val = readl_relaxed(core->dos_base + HEVCD_MPP_ANC_CANVAS_DATA_ADDR);
		val &= 0xffff;
		val |= (val << 16);
		writel_relaxed(val, core->dos_base + HEVCD_MCRCC_CTL2);

		if (l0_cnt == 1) {
			writel_relaxed(val, core->dos_base + HEVCD_MCRCC_CTL3);
		} else {
			val = readl_relaxed(core->dos_base + HEVCD_MPP_ANC_CANVAS_DATA_ADDR);
			val &= 0xffff;
			val |= (val << 16);
			writel_relaxed(val, core->dos_base + HEVCD_MCRCC_CTL3);
		}
	} else { /* B_SLICE */
		writel_relaxed(0, core->dos_base + HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR);
		val = readl_relaxed(core->dos_base + HEVCD_MPP_ANC_CANVAS_DATA_ADDR);
		val &= 0xffff;
		val |= (val << 16);
		writel_relaxed(val, core->dos_base + HEVCD_MCRCC_CTL2);

		writel_relaxed((16 << 8) | (1 << 1), core->dos_base + HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR);
		val_2 = readl_relaxed(core->dos_base + HEVCD_MPP_ANC_CANVAS_DATA_ADDR);
		val_2 &= 0xffff;
		val_2 |= (val_2 << 16);
		if (val == val_2 && l1_cnt > 1) {
			val_2 = readl_relaxed(core->dos_base + HEVCD_MPP_ANC_CANVAS_DATA_ADDR);
			val_2 &= 0xffff;
			val_2 |= (val_2 << 16);
		}
		writel_relaxed(val, core->dos_base + HEVCD_MCRCC_CTL3);
	}

	/* enable mcrcc progressive-mode */
	writel_relaxed(0xff0, core->dos_base + HEVCD_MCRCC_CTL1);
}

static void codec_hevc_set_ref_list(struct vdec_session *sess,
				u32 ref_num, u32 *ref_poc_list)
{
	struct codec_hevc *hevc = sess->priv;
	struct hevc_frame *ref_frame;
	struct vdec_core *core = sess->core;
	int i;
	u32 ref_frame_id;

	for (i = 0; i < ref_num; i++) {
		ref_frame = codec_hevc_get_frame_by_poc(hevc, ref_poc_list[i]);

		if (!ref_frame) {
			dev_warn(core->dev, "Couldn't find ref. frame %u\n",
				ref_poc_list[i]);
			continue;
		}

		ref_frame_id = ref_frame->vbuf->vb2_buf.index * 2;

		writel_relaxed(((ref_frame_id + 1) << 16) |
				((ref_frame_id + 1) << 8) |
				ref_frame_id,
				core->dos_base + HEVCD_MPP_ANC_CANVAS_DATA_ADDR);
	}
}

static void codec_hevc_set_mc(struct vdec_session *sess, struct hevc_frame *frame)
{
	struct vdec_core *core = sess->core;

	if (frame->cur_slice_type == I_SLICE)
		return;

	writel_relaxed(1, core->dos_base + HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR);
	codec_hevc_set_ref_list(sess, frame->ref_num[0],
		frame->ref_poc_list[0][frame->cur_slice_idx]);

	if (frame->cur_slice_type == P_SLICE)
		return;

	writel_relaxed((16 << 8) | 1, core->dos_base + HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR);
	codec_hevc_set_ref_list(sess, frame->ref_num[1],
		frame->ref_poc_list[1][frame->cur_slice_idx]);
}

static void codec_hevc_update_col_frame(struct codec_hevc *hevc)
{
	struct hevc_frame *cur_frame = hevc->cur_frame;
	union rpm_param *param = &hevc->rpm_param;
	u32 list_no = 0;
	u32 col_ref = param->p.collocated_ref_idx;
	u32 col_from_l0 = param->p.collocated_from_l0_flag;

	if (cur_frame->cur_slice_type == B_SLICE)
		list_no = 1 - col_from_l0;

	if (col_ref >= cur_frame->ref_num[list_no])
		hevc->col_poc = INVALID_POC;
	else
		hevc->col_poc = cur_frame->ref_poc_list[list_no][cur_frame->cur_slice_idx][col_ref];

	if (cur_frame->cur_slice_type == I_SLICE)
		goto end;

	if (hevc->col_poc != INVALID_POC)
		hevc->col_frame = codec_hevc_get_frame_by_poc(hevc, hevc->col_poc);
	else
		hevc->col_frame = hevc->cur_frame;

end:
	if (!hevc->col_frame)
		hevc->col_frame = hevc->cur_frame;
}

static void codec_hevc_update_pocs(struct vdec_session *sess)
{
	struct codec_hevc *hevc = sess->priv;
	union rpm_param *param = &hevc->rpm_param;
	u32 nal_unit_type = param->p.m_nalUnitType;
	u32 temporal_id = param->p.m_temporalId & 0x7;
	int iMaxPOClsb = 1 << (param->p.log2_max_pic_order_cnt_lsb_minus4 + 4);
	int iPrevPOClsb;
	int iPrevPOCmsb;
	int iPOCmsb;
	int iPOClsb = param->p.POClsb;

	if (nal_unit_type == NAL_UNIT_CODED_SLICE_IDR ||
	    nal_unit_type == NAL_UNIT_CODED_SLICE_IDR_N_LP) {
		hevc->curr_poc = 0;
		if ((temporal_id - 1) == 0)
			hevc->iPrevTid0POC = hevc->curr_poc;

		return;
	}

	iPrevPOClsb = hevc->iPrevTid0POC % iMaxPOClsb;
	iPrevPOCmsb = hevc->iPrevTid0POC - iPrevPOClsb;

	if ((iPOClsb < iPrevPOClsb) && ((iPrevPOClsb - iPOClsb) >= (iMaxPOClsb / 2)))
		iPOCmsb = iPrevPOCmsb + iMaxPOClsb;
	else if ((iPOClsb > iPrevPOClsb) && ((iPOClsb - iPrevPOClsb) > (iMaxPOClsb / 2)))
		iPOCmsb = iPrevPOCmsb - iMaxPOClsb;
	else
		iPOCmsb = iPrevPOCmsb;

	if (nal_unit_type == NAL_UNIT_CODED_SLICE_BLA   ||
	    nal_unit_type == NAL_UNIT_CODED_SLICE_BLANT ||
	    nal_unit_type == NAL_UNIT_CODED_SLICE_BLA_N_LP)
		iPOCmsb = 0;

	hevc->curr_poc = (iPOCmsb + iPOClsb);
	if ((temporal_id - 1) == 0)
		hevc->iPrevTid0POC = hevc->curr_poc;
}

static void codec_hevc_process_segment_header(struct vdec_session *sess)
{
	struct codec_hevc *hevc = sess->priv;
	union rpm_param *param = &hevc->rpm_param;

	if (param->p.first_slice_segment_in_pic_flag == 0) {
		hevc->slice_segment_addr = param->p.slice_segment_address;
		if (!param->p.dependent_slice_segment_flag)
			hevc->slice_addr = hevc->slice_segment_addr;
	} else {
		hevc->slice_segment_addr = 0;
		hevc->slice_addr = 0;
	}

	codec_hevc_update_pocs(sess);
}

static int codec_hevc_process_segment(struct vdec_session *sess)
{
	struct codec_hevc *hevc = sess->priv;
	struct vdec_core *core = sess->core;
	union rpm_param *param = &hevc->rpm_param;
	u32 slice_segment_address = param->p.slice_segment_address;

	/* First slice: new frame */
	if (slice_segment_address == 0) {
		codec_hevc_update_referenced(hevc);
		codec_hevc_output_frames(sess);

		hevc->cur_frame = codec_hevc_prepare_new_frame(sess);
		if (!hevc->cur_frame)
			return -1;

		codec_hevc_update_tiles(sess);
	} else {
		hevc->cur_frame->cur_slice_idx++;
	}

	codec_hevc_update_frame_refs(sess, hevc->cur_frame);
	codec_hevc_update_col_frame(hevc);
	codec_hevc_update_ldc_flag(hevc);
	codec_hevc_set_mc(sess, hevc->cur_frame);
	codec_hevc_set_mcrcc(sess);
	codec_hevc_set_mpred(sess, hevc->cur_frame, hevc->col_frame);
	codec_hevc_set_sao(sess, hevc->cur_frame);

	writel_relaxed(readl_relaxed(core->dos_base + HEVC_WAIT_FLAG) | 2, core->dos_base + HEVC_WAIT_FLAG);
	writel_relaxed(HEVC_CODED_SLICE_SEGMENT_DAT, core->dos_base + HEVC_DEC_STATUS_REG);

	/* Interrupt the firmware's processor */
	writel_relaxed(AMRISC_MAIN_REQ, core->dos_base + HEVC_MCPU_INTR_REQ);

	return 0;
}

static int codec_hevc_process_rpm(struct vdec_session *sess)
{
	struct vdec_core *core = sess->core;
	struct codec_hevc *hevc = sess->priv;
	union rpm_param *rpm_param = &hevc->rpm_param;
	u32 lcu_x_num_div, lcu_y_num_div;

	if (rpm_param->p.bit_depth &&
	    sess->fmt_cap->pixfmt == V4L2_PIX_FMT_NV12M) {
		dev_err(sess->core->dev_dec,
		    "V4L2_PIX_FMT_NV12M is only compatible with HEVC 8-bit\n");
		return -EINVAL;
	}

	hevc->width  = rpm_param->p.pic_width_in_luma_samples;
	hevc->height = rpm_param->p.pic_height_in_luma_samples;

	/*if (hevc->width  != sess->width ||
	    hevc->height != sess->height) {
		dev_err(sess->core->dev_dec,
			"Size mismatch: bitstream %ux%u ; driver %ux%u\n",
			hevc->width, hevc->height,
			sess->width, sess->height);
		return -EINVAL;
	}*/

	hevc->lcu_size = 1 << (rpm_param->p.log2_min_coding_block_size_minus3 +
		3 + rpm_param->p.log2_diff_max_min_coding_block_size);

	lcu_x_num_div = (hevc->width / hevc->lcu_size);
	lcu_y_num_div = (hevc->height / hevc->lcu_size);
	hevc->lcu_x_num = ((hevc->width % hevc->lcu_size) == 0) ? lcu_x_num_div : lcu_x_num_div + 1;
	hevc->lcu_y_num = ((hevc->height % hevc->lcu_size) == 0) ? lcu_y_num_div : lcu_y_num_div + 1;
	hevc->lcu_total = hevc->lcu_x_num * hevc->lcu_y_num;

	dev_dbg(core->dev, "lcu_size = %u ; lcu_x_num = %u; lcu_y_num = %u",
		hevc->lcu_size, hevc->lcu_x_num, hevc->lcu_y_num);

	return 0;
}

/* The RPM section within the workspace contains
 * many information regarding the parsed bitstream
 */
static void codec_hevc_fetch_rpm(struct vdec_session *sess)
{
	struct codec_hevc *hevc = sess->priv;
	u16 *rpm_vaddr = hevc->workspace_vaddr + RPM_OFFSET;
	int i, j;

	for (i = 0; i < RPM_SIZE; i += 4)
		for (j = 0; j < 4; j++)
			hevc->rpm_param.l.data[i + j] = rpm_vaddr[i + 3 - j];
}

static irqreturn_t codec_hevc_threaded_isr(struct vdec_session *sess)
{
	struct vdec_core *core = sess->core;
	struct codec_hevc *hevc;

	mutex_lock(&sess->codec_lock);
	hevc = sess->priv;
	if (!hevc)
		goto unlock;

	if (hevc->dec_status != HEVC_SLICE_SEGMENT_DONE) {
		dev_err(core->dev_dec, "Unrecognized dec_status: %08X\n",
			hevc->dec_status);
		vdec_abort(sess);
		goto unlock;
	}

	sess->keyframe_found = 1;
	codec_hevc_fetch_rpm(sess);
	if (codec_hevc_process_rpm(sess)) {
		vdec_abort(sess);
		goto unlock;
	}

	codec_hevc_process_segment_header(sess);
	if (codec_hevc_process_segment(sess))
		vdec_abort(sess);

unlock:
	mutex_unlock(&sess->codec_lock);
	return IRQ_HANDLED;
}

static irqreturn_t codec_hevc_isr(struct vdec_session *sess)
{
	struct vdec_core *core = sess->core;
	struct codec_hevc *hevc = sess->priv;

	hevc->dec_status = readl_relaxed(core->dos_base + HEVC_DEC_STATUS_REG);

	return IRQ_WAKE_THREAD;
}

struct vdec_codec_ops codec_hevc_ops = {
	.start = codec_hevc_start,
	.stop = codec_hevc_stop,
	.isr = codec_hevc_isr,
	.threaded_isr = codec_hevc_threaded_isr,
	.num_pending_bufs = codec_hevc_num_pending_bufs,
};