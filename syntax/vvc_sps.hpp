// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstdint>
#include <vector>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC Sequence Parameter Set (SPS)
 * -----------------------------------------------------------
 * H.266 §7.3.2.4  Full syntax (profile_tier_level + DPB/HRD,
 * RPL, scaling lists, ALF/CCALF/LMCS, virtual boundaries,
 * VUI, HRD, extension, trailing).
 *
 * Leading fields remain compatible with the previous
 * leading-fields-only version.
 */
struct SequenceParameterSet {
    std::uint8_t sps_id = 0;

    std::uint8_t vps_id = 0;

    std::uint8_t max_sublayers_minus1 = 0;

    std::uint8_t chroma_format_idc = 0;

    std::uint8_t log2_ctu_size_minus5 = 0;

    bool ptl_dpb_hrd_params_present = false;

    bool gdr_enabled_flag = false;

    bool ref_pic_resampling_enabled_flag = false;

    bool subpic_info_present_flag = false;

    std::uint8_t subpic_id_len_minus1 = 0;

    /*
     * Number of extra slice-header bits (NumExtraShBits).
     */
    std::uint8_t num_extra_sh_bits = 0;

    /*
     * -------------------------------------------------------
     * Picture order count configuration (§7.3.2.4)
     * -------------------------------------------------------
     */
    std::uint8_t log2_max_pic_order_cnt_lsb_minus4 = 0;

    bool poc_msb_cycle_flag = false;

    std::uint8_t poc_msb_cycle_len_minus1 = 0;

    /*
     * Number of extra PH bits (NumExtraPhBits), derived from
     * sps_num_extra_ph_bytes / sps_extra_ph_bit_present_flag.
     */
    std::uint8_t num_extra_ph_bits = 0;

    // -------------------------------------------------------
    // Full SPS fields (§7.3.2.4) – ffmpeg cbs_h266 sps() order
    // -------------------------------------------------------
    bool res_change_in_clvs_allowed_flag = false;

    std::uint32_t pic_width_max_in_luma_samples = 0;
    std::uint32_t pic_height_max_in_luma_samples = 0;

    bool conformance_window_flag = false;
    std::uint32_t conf_win_left_offset = 0;
    std::uint32_t conf_win_right_offset = 0;
    std::uint32_t conf_win_top_offset = 0;
    std::uint32_t conf_win_bottom_offset = 0;

    std::uint32_t num_subpics_minus1 = 0;
    bool independent_subpics_flag = true;
    bool subpic_same_size_flag = false;
    bool subpic_id_mapping_explicitly_signalled_flag = false;
    bool subpic_id_mapping_present_flag = false;

    std::uint8_t bitdepth_minus8 = 0;

    bool entropy_coding_sync_enabled_flag = false;
    bool entry_point_offsets_present_flag = false;

    std::uint8_t num_extra_ph_bytes = 0;
    std::uint8_t num_extra_sh_bytes = 0;

    bool sublayer_dpb_params_flag = false;

    std::uint32_t log2_min_luma_coding_block_size_minus2 = 0;
    bool partition_constraints_override_enabled_flag = false;
    std::uint32_t log2_diff_min_qt_min_cb_intra_slice_luma = 0;
    std::uint32_t max_mtt_hierarchy_depth_intra_slice_luma = 0;
    std::uint32_t log2_diff_max_bt_min_qt_intra_slice_luma = 0;
    std::uint32_t log2_diff_max_tt_min_qt_intra_slice_luma = 0;
    bool qtbtt_dual_tree_intra_flag = false;
    std::uint32_t log2_diff_min_qt_min_cb_intra_slice_chroma = 0;
    std::uint32_t max_mtt_hierarchy_depth_intra_slice_chroma = 0;
    std::uint32_t log2_diff_max_bt_min_qt_intra_slice_chroma = 0;
    std::uint32_t log2_diff_max_tt_min_qt_intra_slice_chroma = 0;
    std::uint32_t log2_diff_min_qt_min_cb_inter_slice = 0;
    std::uint32_t max_mtt_hierarchy_depth_inter_slice = 0;
    std::uint32_t log2_diff_max_bt_min_qt_inter_slice = 0;
    std::uint32_t log2_diff_max_tt_min_qt_inter_slice = 0;

    bool max_luma_transform_size_64_flag = false;

    bool transform_skip_enabled_flag = false;
    std::uint32_t log2_transform_skip_max_size_minus2 = 0;
    bool bdpcm_enabled_flag = false;

    bool mts_enabled_flag = false;
    bool explicit_mts_intra_enabled_flag = false;
    bool explicit_mts_inter_enabled_flag = false;

    bool lfnst_enabled_flag = false;

    bool joint_cbcr_enabled_flag = false;
    bool same_qp_table_for_chroma_flag = false;

    bool sao_enabled_flag = false;
    bool alf_enabled_flag = false;
    bool ccalf_enabled_flag = false;
    bool lmcs_enabled_flag = false;

    bool weighted_pred_flag = false;
    bool weighted_bipred_flag = false;

    bool long_term_ref_pics_flag = false;
    bool inter_layer_prediction_enabled_flag = false;

    bool idr_rpl_present_flag = false;
    bool rpl1_same_as_rpl0_flag = false;

    std::uint32_t num_ref_pic_lists[2] = {0, 0};

    struct RefPicListEntry {
        bool inter_layer_ref_pic_flag = false;
        bool st_ref_pic_flag = false;
        std::uint32_t abs_delta_poc_st = 0;
        bool strp_entry_sign_flag = false;
        std::uint32_t rpls_poc_lsb_lt = 0;
        std::uint32_t ilrp_idx = 0;
    };
    struct RefPicListStruct {
        std::uint32_t num_ref_entries = 0;
        bool ltrp_in_header_flag = false;
        std::vector<RefPicListEntry> entries;
    };
    // sps_ref_pic_list_struct[listIdx][rplsIdx]
    std::vector<RefPicListStruct> ref_pic_lists[2];

    bool ref_wraparound_enabled_flag = false;

    bool temporal_mvp_enabled_flag = false;
    bool sbtmvp_enabled_flag = false;

    bool amvr_enabled_flag = false;

    bool bdof_enabled_flag = false;
    bool bdof_control_present_in_ph_flag = false;

    bool smvd_enabled_flag = false;

    bool dmvr_enabled_flag = false;
    bool dmvr_control_present_in_ph_flag = false;

    bool mmvd_enabled_flag = false;
    bool mmvd_fullpel_only_enabled_flag = false;

    std::uint32_t six_minus_max_num_merge_cand = 0;

    bool sbt_enabled_flag = false;

    bool affine_enabled_flag = false;
    std::uint32_t five_minus_max_num_subblock_merge_cand = 0;
    bool six_param_affine_enabled_flag = false;
    bool affine_amvr_enabled_flag = false;
    bool affine_prof_enabled_flag = false;
    bool prof_control_present_in_ph_flag = false;

    bool bcw_enabled_flag = false;
    bool ciip_enabled_flag = false;

    bool gpm_enabled_flag = false;
    std::uint32_t max_num_merge_cand_minus_max_num_gpm_cand = 0;

    std::uint32_t log2_parallel_merge_level_minus2 = 0;

    bool isp_enabled_flag = false;
    bool mrl_enabled_flag = false;
    bool mip_enabled_flag = false;

    bool cclm_enabled_flag = false;
    bool chroma_horizontal_collocated_flag = true;
    bool chroma_vertical_collocated_flag = true;

    bool palette_enabled_flag = false;
    bool act_enabled_flag = false;
    std::uint32_t min_qp_prime_ts_minus4 = 0;

    bool ibc_enabled_flag = false;
    std::uint32_t six_minus_max_num_ibc_merge_cand = 0;

    bool ladf_enabled_flag = false;
    std::uint32_t num_ladf_intervals_minus2 = 0;
    std::int32_t ladf_lowest_interval_qp_offset = 0;

    bool explicit_scaling_list_enabled_flag = false;
    // task aliases / detailed scaling flags
    bool scaling_matrix_for_lfnst_disabled_flag = false;
    bool scaling_matrix_for_alternative_colour_space_disabled_flag = false;
    bool scaling_matrix_designated_colour_space_flag = false;
    bool scaling_enabled_flag = false;  // alias for explicit_scaling_list_enabled_flag

    bool dep_quant_enabled_flag = false;
    bool sign_data_hiding_enabled_flag = false;

    bool virtual_boundaries_enabled_flag = false;
    bool virtual_boundaries_present_flag = false;
    std::uint32_t num_ver_virtual_boundaries = 0;
    std::uint32_t num_hor_virtual_boundaries = 0;
    std::vector<std::uint32_t> virtual_boundary_pos_x_minus1;
    std::vector<std::uint32_t> virtual_boundary_pos_y_minus1;

    bool timing_hrd_params_present_flag = false;
    bool sublayer_cpb_params_present_flag = false;

    // general HRD (sps_general_hrd_params)
    bool general_nal_hrd_params_present_flag = false;
    bool general_vcl_hrd_params_present_flag = false;
    bool general_same_pic_timing_in_all_ols_flag = false;
    bool general_du_hrd_params_present_flag = false;
    std::uint8_t bit_rate_scale = 0;
    std::uint8_t cpb_size_scale = 0;
    std::uint8_t cpb_size_du_scale = 0;
    std::uint32_t hrd_cpb_cnt_minus1 = 0;
    std::uint8_t tick_divisor_minus2 = 0;

    bool field_seq_flag = false;
    bool vui_parameters_present_flag = false;
    std::uint32_t vui_payload_size_minus1 = 0;

    bool extension_flag = false;
    bool range_extension_flag = false;
    std::uint8_t extension_7bits = 0;
    bool extended_precision_flag = false;
    bool ts_residual_coding_rice_present_in_sh_flag = false;
    bool rrc_rice_extension_flag = false;
    bool persistent_rice_adaptation_enabled_flag = false;
    bool reverse_last_sig_coeff_enabled_flag = false;

    [[nodiscard]]
    std::uint32_t max_pic_order_cnt_lsb() const noexcept {
        return std::uint32_t{1} << (log2_max_pic_order_cnt_lsb_minus4 + 4);
    }

    [[nodiscard]]
    std::uint32_t ctb_size() const noexcept {
        return std::uint32_t{1} << (log2_ctu_size_minus5 + 5);
    }

    [[nodiscard]]
    bool valid() const noexcept {
        return sps_id <= 15 && chroma_format_idc <= 3;
    }
};

}  // namespace vvc
}  // namespace bs
