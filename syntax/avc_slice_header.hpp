#pragma once

#include "avc_common.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace bs {
namespace avc {

/*
 * One ref_pic_list_reordering() operation (7.3.3.1.1).
 */
struct RefPicReorderingOp {
    std::uint32_t reordering_of_pic_nums_idc = 0;
    std::uint32_t abs_diff_pic_num_minus1 = 0;
    std::uint32_t long_term_pic_num = 0;
};

struct RefPicListModification {
    bool modification_flag = false;
    std::vector<RefPicReorderingOp> ops{};
};

/*
 * One memory_management_control_operation (7.3.3.3).
 */
struct MmcoOperation {
    std::uint32_t memory_management_control_operation = 0;
    std::uint32_t difference_of_pic_nums_minus1 = 0;
    std::uint32_t long_term_pic_num = 0;
    std::uint32_t long_term_frame_idx = 0;
    std::uint32_t max_long_term_frame_idx_plus1 = 0;
};

/*
 * pred_weight_table() (7.3.3.2).
 *
 * List 0 and list 1 weights are stored separately; each list
 * can hold up to 32 reference indices.
 */
struct PredWeightTable {
    std::uint32_t luma_log2_weight_denom = 0;
    std::uint32_t chroma_log2_weight_denom = 0;

    std::array<bool, 32> luma_weight_l0_flag{};
    std::array<std::int16_t, 32> luma_weight_l0{};
    std::array<std::int16_t, 32> luma_offset_l0{};
    std::array<bool, 32> chroma_weight_l0_flag{};
    std::array<std::array<std::int16_t, 2>, 32> chroma_weight_l0{};
    std::array<std::array<std::int16_t, 2>, 32> chroma_offset_l0{};

    std::array<bool, 32> luma_weight_l1_flag{};
    std::array<std::int16_t, 32> luma_weight_l1{};
    std::array<std::int16_t, 32> luma_offset_l1{};
    std::array<bool, 32> chroma_weight_l1_flag{};
    std::array<std::array<std::int16_t, 2>, 32> chroma_weight_l1{};
    std::array<std::array<std::int16_t, 2>, 32> chroma_offset_l1{};
};

/*
 * H.264 / AVC slice segment header (7.3.3.1).
 */
struct SliceHeader {
    std::uint32_t first_mb_in_slice = 0;
    SliceType slice_type = SliceType::P;
    std::uint8_t pic_parameter_set_id = 0;

    std::uint32_t frame_num = 0;
    bool field_pic_flag = false;
    bool bottom_field_flag = false;

    std::uint32_t idr_pic_id = 0;

    std::uint32_t pic_order_cnt_lsb = 0;
    std::int32_t delta_pic_order_cnt_bottom = 0;
    std::array<std::int32_t, 2> delta_pic_order_cnt{};

    std::uint32_t redundant_pic_cnt = 0;

    bool direct_spatial_mv_pred_flag = false;

    bool num_ref_idx_active_override_flag = false;
    std::uint8_t num_ref_idx_l0_active_minus1 = 0;
    std::uint8_t num_ref_idx_l1_active_minus1 = 0;

    RefPicListModification ref_pic_list_modification_l0{};
    RefPicListModification ref_pic_list_modification_l1{};

    PredWeightTable pred_weight_table{};

    bool no_output_of_prior_pics_flag = false;
    bool long_term_reference_flag = false;
    bool adaptive_ref_pic_marking_mode_flag = false;
    std::vector<MmcoOperation> mmco_operations{};

    std::uint8_t cabac_init_idc = 0;

    std::int32_t slice_qp_delta = 0;

    std::uint8_t disable_deblocking_filter_idc = 0;
    std::int32_t slice_alpha_c0_offset_div2 = 0;
    std::int32_t slice_beta_offset_div2 = 0;

    std::uint32_t slice_group_change_cycle = 0;

    bool sp_for_switch_flag = false;
    std::int32_t slice_qs_delta = 0;
};

}  // namespace avc
}  // namespace bs