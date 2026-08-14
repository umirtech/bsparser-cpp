#pragma once

#include "avc_scaling_list.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace bs {
namespace avc {

/*
 * Slice group map (7.3.2.2).
 */
struct SliceGroupMap {
    std::uint8_t slice_group_map_type = 0;

    std::array<std::uint32_t, 8> run_length_minus1{};
    std::array<std::uint32_t, 8> top_left{};
    std::array<std::uint32_t, 8> bottom_right{};

    bool slice_group_change_direction_flag = false;
    std::uint32_t slice_group_change_rate_minus1 = 0;

    std::uint32_t pic_size_in_map_units_minus1 = 0;

    /*
     * type == 6: one entry per map unit, bounded by the
     * decoder's sanity cap (see parser).
     */
    std::vector<std::uint8_t> slice_group_id{};
};

/*
 * H.264 / AVC picture parameter set (7.3.2.2).
 */
struct PictureParameterSet {
    std::uint8_t pic_parameter_set_id = 0;
    std::uint8_t seq_parameter_set_id = 0;

    bool entropy_coding_mode_flag = false;
    bool bottom_field_pic_order_in_frame_present_flag = false;

    std::uint8_t num_slice_groups_minus1 = 0;
    SliceGroupMap slice_group_map{};

    std::uint8_t num_ref_idx_l0_default_active_minus1 = 0;
    std::uint8_t num_ref_idx_l1_default_active_minus1 = 0;

    bool weighted_pred_flag = false;
    std::uint8_t weighted_bipred_idc = 0;

    std::int32_t pic_init_qp_minus26 = 0;
    std::int32_t pic_init_qs_minus26 = 0;
    std::int32_t chroma_qp_index_offset = 0;

    bool deblocking_filter_control_present_flag = false;
    bool constrained_intra_pred_flag = false;
    bool redundant_pic_cnt_present_flag = false;

    bool transform_8x8_mode_flag = false;
    bool pic_scaling_matrix_present_flag = false;
    ScalingList pic_scaling_lists{};
    std::int32_t second_chroma_qp_index_offset = 0;

    [[nodiscard]]
    constexpr std::uint8_t num_slice_groups() const noexcept {
        return static_cast<std::uint8_t>(num_slice_groups_minus1 + 1);
    }
};

}  // namespace avc
}  // namespace bs