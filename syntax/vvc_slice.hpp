#pragma once

#include "vvc_ph.hpp"

#include <cstdint>
#include <vector>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC slice type
 * -----------------------------------------------------------
 */
enum class SliceType : std::uint8_t { B = 0, P = 1, I = 2 };

/*
 * -----------------------------------------------------------
 * VVC slice segment header
 * -----------------------------------------------------------
 * H.266 §7.3.2.11  Full syntax (ffmpeg cbs_h266 slice_header).
 * Leading fields remain compatible with the previous
 * leading-fields-only version.
 */
struct SliceHeader {
    std::uint8_t pps_id = 0;

    bool first_slice_segment_in_pic = false;

    bool independent_slice_segment = true;

    std::uint32_t slice_segment_address = 0;

    // legacy alias for new name
    std::uint32_t slice_address = 0;

    SliceType slice_type = SliceType::I;

    /*
     * sh_picture_header_in_slice_header_flag: the picture header is embedded
     * in this slice header (rather than signalled in a PH NAL).
     */
    bool picture_header_in_slice_header_flag = false;

    /*
     * The picture header of the picture this slice belongs to (embedded or
     * from the stored PH NAL), with the POC fields populated.
     */
    PictureHeader ph{};

    // Subpic / slice addressing
    std::uint32_t subpic_id = 0;
    std::uint32_t curr_subpic_idx = 0;
    std::uint32_t num_tiles_in_slice_minus1 = 0;

    std::vector<bool> extra_bits;

    bool no_output_of_prior_pics_flag = false;

    // ALF (when !pps_alf_info_in_ph_flag)
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

    bool lmcs_used_flag = false;
    bool explicit_scaling_list_used_flag = false;

    PictureHeaderRefPicLists ref_pic_lists{};

    bool num_ref_idx_active_override_flag = false;
    std::uint32_t num_ref_idx_active_minus1[2] = {0, 0};
    std::uint32_t num_ref_idx_active[2] = {0, 0};

    bool cabac_init_flag = false;
    bool collocated_from_l0_flag = true;
    std::uint32_t collocated_ref_idx = 0;

    std::int32_t qp_delta = 0;
    std::int32_t cb_qp_offset = 0;
    std::int32_t cr_qp_offset = 0;
    std::int32_t joint_cbcr_qp_offset = 0;

    bool cu_chroma_qp_offset_enabled_flag = false;

    bool sao_luma_used_flag = false;
    bool sao_chroma_used_flag = false;

    bool deblocking_params_present_flag = false;
    bool deblocking_filter_disabled_flag = false;
    std::int32_t luma_beta_offset_div2 = 0;
    std::int32_t luma_tc_offset_div2 = 0;
    std::int32_t cb_beta_offset_div2 = 0;
    std::int32_t cb_tc_offset_div2 = 0;
    std::int32_t cr_beta_offset_div2 = 0;
    std::int32_t cr_tc_offset_div2 = 0;

    bool dep_quant_used_flag = false;
    bool sign_data_hiding_used_flag = false;
    bool ts_residual_coding_disabled_flag = false;
    std::uint32_t ts_residual_coding_rice_idx_minus1 = 0;
    bool reverse_last_sig_coeff_flag = false;

    std::uint32_t slice_header_extension_length = 0;
    std::vector<std::uint8_t> slice_header_extension_data;

    std::uint32_t num_entry_points = 0;
    std::uint32_t entry_offset_len_minus1 = 0;
    std::vector<std::uint32_t> entry_point_offset_minus1;

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
