#pragma once

#include <cstdint>
#include <vector>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC Picture Parameter Set (PPS)
 * -----------------------------------------------------------
 * H.266 §7.3.2.5  Full syntax per ffmpeg cbs_h266
 * pps() ~lines 1675-2315.
 *
 * Leading fields (pps_id, sps_id, mixed_nalu_types,
 * pic_width/height) remain compatible with the previous
 * leading-fields-only version.
 */
struct PictureParameterSet {
    std::uint8_t pps_id = 0;  // pps_pic_parameter_set_id u(6)
    std::uint8_t sps_id = 0;  // pps_seq_parameter_set_id u(4)

    bool mixed_nalu_types_in_pic = false;

    std::uint32_t pic_width_in_luma_samples = 0;
    std::uint32_t pic_height_in_luma_samples = 0;

    bool conformance_window_flag = false;
    std::uint32_t conf_win_left_offset = 0;
    std::uint32_t conf_win_right_offset = 0;
    std::uint32_t conf_win_top_offset = 0;
    std::uint32_t conf_win_bottom_offset = 0;

    bool scaling_window_explicit_signalling_flag = false;
    std::int32_t scaling_win_left_offset = 0;
    std::int32_t scaling_win_right_offset = 0;
    std::int32_t scaling_win_top_offset = 0;
    std::int32_t scaling_win_bottom_offset = 0;

    bool output_flag_present_flag = false;
    bool no_pic_partition_flag = false;
    bool subpic_id_mapping_present_flag = false;

    std::uint32_t num_subpics_minus1 = 0;
    std::uint8_t subpic_id_len_minus1 = 0;
    std::vector<std::uint32_t> subpic_id;  // size num_subpics_minus1+1

    // tiles / partitions (when !no_pic_partition_flag)
    std::uint8_t log2_ctu_size_minus5 = 0;
    std::uint32_t num_exp_tile_columns_minus1 = 0;
    std::uint32_t num_exp_tile_rows_minus1 = 0;
    std::vector<std::uint32_t> tile_column_width_minus1;
    std::vector<std::uint32_t> tile_row_height_minus1;

    std::uint32_t num_tile_columns = 0;
    std::uint32_t num_tile_rows = 0;
    std::uint32_t num_tiles_in_pic = 0;
    // col_width_val / row_height_val derived (for slice parsing)
    std::vector<std::uint32_t> col_width_val;
    std::vector<std::uint32_t> row_height_val;

    bool loop_filter_across_tiles_enabled_flag = false;
    bool rect_slice_flag = false;
    bool single_slice_per_subpic_flag = false;
    std::uint32_t num_slices_in_pic_minus1 = 0;
    bool tile_idx_delta_present_flag = false;

    std::vector<std::uint32_t> slice_width_in_tiles_minus1;
    std::vector<std::uint32_t> slice_height_in_tiles_minus1;
    std::vector<std::uint32_t> num_exp_slices_in_tile;
    std::vector<std::vector<std::uint32_t>> exp_slice_height_in_ctus_minus1;
    std::vector<std::int32_t> tile_idx_delta_val;

    // Derived per-slice info for completeness
    std::vector<std::uint32_t> slice_top_left_tile_idx;
    std::vector<std::uint32_t> num_slices_in_tile;
    std::vector<std::uint32_t> slice_height_in_ctus;

    bool loop_filter_across_slices_enabled_flag = false;

    // after partition
    bool cabac_init_present_flag = false;
    std::uint32_t num_ref_idx_default_active_minus1[2] = {0, 0};
    bool rpl1_idx_present_flag = false;
    bool weighted_pred_flag = false;
    bool weighted_bipred_flag = false;
    bool ref_wraparound_enabled_flag = false;
    std::uint32_t pic_width_minus_wraparound_offset = 0;
    std::int32_t init_qp_minus26 = 0;
    bool cu_qp_delta_enabled_flag = false;
    bool chroma_tool_offsets_present_flag = false;
    std::int32_t cb_qp_offset = 0;
    std::int32_t cr_qp_offset = 0;
    bool joint_cbcr_qp_offset_present_flag = false;
    std::int32_t joint_cbcr_qp_offset_value = 0;
    bool slice_chroma_qp_offsets_present_flag = false;
    bool cu_chroma_qp_offset_list_enabled_flag = false;
    std::uint32_t chroma_qp_offset_list_len_minus1 = 0;
    std::vector<std::int32_t> cb_qp_offset_list;
    std::vector<std::int32_t> cr_qp_offset_list;
    std::vector<std::int32_t> joint_cbcr_qp_offset_list;

    bool deblocking_filter_control_present_flag = false;
    bool deblocking_filter_override_enabled_flag = false;
    bool deblocking_filter_disabled_flag = false;
    bool dbf_info_in_ph_flag = false;
    std::int32_t luma_beta_offset_div2 = 0;
    std::int32_t luma_tc_offset_div2 = 0;
    std::int32_t cb_beta_offset_div2 = 0;
    std::int32_t cb_tc_offset_div2 = 0;
    std::int32_t cr_beta_offset_div2 = 0;
    std::int32_t cr_tc_offset_div2 = 0;

    bool rpl_info_in_ph_flag = false;
    bool sao_info_in_ph_flag = false;
    bool alf_info_in_ph_flag = false;
    bool wp_info_in_ph_flag = false;
    bool qp_delta_info_in_ph_flag = false;

    bool picture_header_extension_present_flag = false;
    bool slice_header_extension_present_flag = false;
    bool extension_flag = false;

    [[nodiscard]]
    bool valid() const noexcept {
        return pps_id <= 63;
    }
};

}  // namespace vvc
}  // namespace bs
