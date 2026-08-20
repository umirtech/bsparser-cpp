// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstdint>
#include <vector>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC Picture Header (PH)
 * -----------------------------------------------------------
 * H.266 §7.3.2.7  Full syntax (ffmpeg cbs_h266 picture_header).
 * Leading fields remain compatible with the previous
 * leading-fields-only version.
 */

struct PictureHeaderRefPicLists {
    struct Entry {
        bool inter_layer_ref_pic_flag = false;
        bool st_ref_pic_flag = false;
        std::uint32_t abs_delta_poc_st = 0;
        bool strp_entry_sign_flag = false;
        std::uint32_t rpls_poc_lsb_lt = 0;
        std::uint32_t ilrp_idx = 0;
    };
    struct List {
        std::uint32_t num_ref_entries = 0;
        bool ltrp_in_header_flag = false;
        std::vector<Entry> entries;
        // for long-term entries where ltrp_in_header_flag == 1
        std::vector<std::uint32_t> poc_lsb_lt;
        std::vector<bool> delta_poc_msb_cycle_present_flag;
        std::vector<std::uint32_t> delta_poc_msb_cycle_lt;
        // cached for dispatch (RplSpsFlag etc.)
        bool rpl_sps_flag = false;
        std::uint32_t rpl_idx = 0;
    };
    List rpl[2];
};

struct PictureHeader {
    std::uint8_t pps_id = 0;

    bool gdr_or_irap_pic_flag = false;

    bool non_ref_pic_flag = false;

    /* Present when gdr_or_irap_pic_flag. */
    bool gdr_pic_flag = false;

    bool inter_slice_allowed_flag = false;

    bool intra_slice_allowed_flag = false;

    std::uint32_t poc_lsb = 0;

    std::uint32_t poc_lsb_bits = 0;

    /* Present when gdr_pic_flag. */
    std::uint32_t recovery_poc_cnt = 0;

    bool poc_msb_cycle_present_flag = false;

    std::uint32_t poc_msb_cycle_val = 0;

    // ph_extra_bits
    std::vector<bool> extra_bits;

    // ALF
    bool alf_enabled_flag = false;
    std::uint8_t num_alf_aps_ids_luma = 0;
    std::vector<std::uint8_t> alf_aps_id_luma;
    bool alf_cb_enabled_flag = false;
    bool alf_cr_enabled_flag = false;
    std::uint8_t alf_aps_id_chroma = 0;
    bool alf_cc_cb_enabled_flag = false;
    std::uint8_t alf_cc_cb_aps_id = 0;
    bool alf_cc_cr_enabled_flag = false;
    std::uint8_t alf_cc_cr_aps_id = 0;

    // LMCS
    bool lmcs_enabled_flag = false;
    std::uint8_t lmcs_aps_id = 0;
    bool chroma_residual_scale_flag = false;

    // Explicit scaling list
    bool explicit_scaling_list_enabled_flag = false;
    std::uint8_t scaling_list_aps_id = 0;

    // Virtual boundaries
    bool virtual_boundaries_present_flag = false;
    std::uint32_t num_ver_virtual_boundaries = 0;
    std::vector<std::uint32_t> virtual_boundary_pos_x_minus1;
    std::uint32_t num_hor_virtual_boundaries = 0;
    std::vector<std::uint32_t> virtual_boundary_pos_y_minus1;

    bool pic_output_flag = true;

    PictureHeaderRefPicLists ref_pic_lists{};

    // Partition constraints override
    bool partition_constraints_override_flag = false;
    std::uint32_t log2_diff_min_qt_min_cb_intra_slice_luma = 0;
    std::uint32_t max_mtt_hierarchy_depth_intra_slice_luma = 0;
    std::uint32_t log2_diff_max_bt_min_qt_intra_slice_luma = 0;
    std::uint32_t log2_diff_max_tt_min_qt_intra_slice_luma = 0;
    std::uint32_t log2_diff_min_qt_min_cb_intra_slice_chroma = 0;
    std::uint32_t max_mtt_hierarchy_depth_intra_slice_chroma = 0;
    std::uint32_t log2_diff_max_bt_min_qt_intra_slice_chroma = 0;
    std::uint32_t log2_diff_max_tt_min_qt_intra_slice_chroma = 0;
    std::uint32_t log2_diff_min_qt_min_cb_inter_slice = 0;
    std::uint32_t max_mtt_hierarchy_depth_inter_slice = 0;
    std::uint32_t log2_diff_max_bt_min_qt_inter_slice = 0;
    std::uint32_t log2_diff_max_tt_min_qt_inter_slice = 0;

    std::uint32_t cu_qp_delta_subdiv_intra_slice = 0;
    std::uint32_t cu_chroma_qp_offset_subdiv_intra_slice = 0;
    std::uint32_t cu_qp_delta_subdiv_inter_slice = 0;
    std::uint32_t cu_chroma_qp_offset_subdiv_inter_slice = 0;

    bool temporal_mvp_enabled_flag = false;
    bool collocated_from_l0_flag = true;
    std::uint32_t collocated_ref_idx = 0;

    bool mmvd_fullpel_only_flag = false;

    bool mvd_l1_zero_flag = false;
    bool bdof_disabled_flag = true;
    bool dmvr_disabled_flag = true;
    bool prof_disabled_flag = true;

    std::int32_t qp_delta = 0;

    bool joint_cbcr_sign_flag = false;

    bool sao_luma_enabled_flag = false;
    bool sao_chroma_enabled_flag = false;

    bool deblocking_params_present_flag = false;
    bool deblocking_filter_disabled_flag = false;
    std::int32_t luma_beta_offset_div2 = 0;
    std::int32_t luma_tc_offset_div2 = 0;
    std::int32_t cb_beta_offset_div2 = 0;
    std::int32_t cb_tc_offset_div2 = 0;
    std::int32_t cr_beta_offset_div2 = 0;
    std::int32_t cr_tc_offset_div2 = 0;

    std::uint32_t extension_length = 0;
    std::vector<std::uint8_t> extension_data;

    /*
     * Presentation-order POC (H.266 §8.3.1), filled by the unified dispatch
     * layer's vvc::PocState.
     */
    std::int32_t derived_poc = 0;

    [[nodiscard]]
    bool valid() const noexcept {
        return pps_id <= 63;
    }
};

}  // namespace vvc
}  // namespace bs
