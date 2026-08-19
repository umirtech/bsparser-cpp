#pragma once

#include <cstdint>
#include <vector>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC Video Parameter Set (VPS)
 * -----------------------------------------------------------
 * H.266 §7.3.2.3.  Full syntax per ffmpeg cbs_h266.c
 * vps_710-850+:
 *
 *     vps_video_parameter_set_id (4) · vps_max_layers_minus1 (6) ·
 *     vps_max_sublayers_minus1 (3) ·
 *     [vps_default_ptl_dpb_hrd_max_tid_flag (1)] ·
 *     [vps_all_independent_layers_flag (1)] ·
 *     vps_layer_id[i] (6) × (max_layers_minus1 + 1) ·
 *     vps_independent_layer_flag[i] ·
 *     vps_direct_ref_layer_flag[i][j] ·
 *     vps_max_tid_il_ref_pics_plus1[i][j] (3) ·
 *     [vps_each_layer_is_an_ols_flag] ·
 *     [vps_ols_mode_idc (2)] ·
 *     [vps_num_output_layer_sets_minus2 (8) ·
 *      vps_ols_output_layer_flag[i][j]] ·
 *     vps_num_ptls_minus1 (8) ·
 *     vps_pt_present_flag[i] ·
 *     vps_ptl_max_tid[i] (3) ·
 *     vps_ptl_alignment_zero_bit (byte align) ·
 *     profile_tier_level() × (num_ptls_minus1+1) ·
 *     vps_ols_ptl_idx[i] (8) ·
 *     DPB / HRD (counts/flags, consumed) ·
 *     vps_extension_flag
 *
 * Leading fields remain compatible with the 3620ef1
 * leading-fields-only version.
 */
struct VideoParameterSet {
    /*
     * vps_video_parameter_set_id u(4).
     */
    std::uint8_t vps_id = 0;

    /*
     * vps_max_layers_minus1 u(6).
     */
    std::uint8_t max_layers_minus1 = 0;

    /*
     * vps_max_sublayers_minus1 u(3).
     */
    std::uint8_t max_sublayers_minus1 = 0;

    /*
     * vps_default_ptl_dpb_hrd_max_tid_flag u(1), signalled when
     * max_layers_minus1 > 0 && max_sublayers_minus1 > 0 (otherwise
     * inferred 1).
     */
    bool default_ptl_dpb_hrd_max_tid_flag = true;

    /*
     * vps_all_independent_layers_flag u(1), signalled when
     * max_layers_minus1 > 0 (otherwise inferred 1).
     */
    bool all_independent_layers = true;

    /*
     * vps_layer_id[i] u(6), one entry per layer
     * (0..max_layers_minus1).
     */
    std::vector<std::uint8_t> layer_ids;

    /*
     * vps_independent_layer_flag[i] u(1).
     * Size max_layers_minus1+1.  Indexed by layer i.
     * Inferred 1 when not signalled (i==0 or all_independent_layers).
     */
    std::vector<std::uint8_t> independent_layer_flag;

    /*
     * vps_direct_ref_layer_flag[i][j] u(1).
     * Dimensions [max_layers+1][max_layers+1]; only j < i valid.
     * Inferred 0 when not signalled.
     */
    std::vector<std::vector<std::uint8_t>> direct_ref_layer_flag;

    /*
     * vps_max_tid_il_ref_pics_plus1[i][j] u(3).
     * Dimensions [max_layers+1][max_layers+1]; valid when
     * direct_ref_layer_flag[i][j]==1 && max_tid_ref_present.
     * Inferred max_sublayers_minus1+1 otherwise.
     */
    std::vector<std::vector<std::uint8_t>> max_tid_il_ref_pics_plus1;

    /*
     * vps_each_layer_is_an_ols_flag u(1).
     * Signalled when max_layers >0 && all_independent_layers;
     * inferred 0 when max_layers>0 && !all_independent;
     * inferred 1 when max_layers==0.
     */
    bool each_layer_is_an_ols_flag = true;

    /*
     * vps_ols_mode_idc u(2).  Special value 4 means
     * each_layer_is_an_ols_flag==1 (no OLS mode signalled).
     * 0,1,2 are the signalled modes.
     */
    std::uint8_t ols_mode_idc = 0;

    /*
     * vps_num_output_layer_sets_minus2 u(8), signalled when
     * ols_mode_idc == 2.
     */
    std::uint8_t num_output_layer_sets_minus2 = 0;

    /*
     * vps_ols_output_layer_flag[i][j] u(1).
     * Outer dim = total_num_olss (or num_output+2 when mode 2);
     * inner dim = max_layers+1.  Only i=1..num_output+1 valid when
     * ols_mode_idc==2; inferred 0 elsewhere.  Kept as
     * [total_num_olss][max_layers+1] with index 0 unused for clarity.
     */
    std::vector<std::vector<std::uint8_t>> ols_output_layer_flag;

    /*
     * vps_num_ptls_minus1 u(8).  Range 0..total_num_olss-1.
     * Inferred 0 when max_layers==0.
     */
    std::uint8_t num_ptls_minus1 = 0;

    /*
     * vps_pt_present_flag[i] u(1).  Size num_ptls_minus1+1.
     * Inferred 1 for i==0.
     */
    std::vector<std::uint8_t> pt_present_flag;

    /*
     * vps_ptl_max_tid[i] u(3).  Size num_ptls_minus1+1.
     * Inferred max_sublayers_minus1 when default flag is 1.
     */
    std::vector<std::uint8_t> ptl_max_tid;

    /*
     * vps_ols_ptl_idx[i] u(8).  Size total_num_olss.
     * Signalled when num_ptls>0 && num_ptls != total_num_olss;
     * otherwise inferred (0 when num_ptls==0, else i).
     */
    std::vector<std::uint8_t> ols_ptl_idx;

    /*
     * vps_extension_flag u(1).
     */
    bool extension_flag = false;

    [[nodiscard]]
    std::uint16_t total_num_olss() const noexcept {
        if (max_layers_minus1 == 0) {
            return 1;
        }
        if (each_layer_is_an_ols_flag) {
            return static_cast<std::uint16_t>(max_layers_minus1) + 1;
        }
        if (ols_mode_idc == 2) {
            return static_cast<std::uint16_t>(num_output_layer_sets_minus2) + 2;
        }
        return static_cast<std::uint16_t>(max_layers_minus1) + 1;
    }

    [[nodiscard]]
    std::uint16_t num_ptls() const noexcept {
        return static_cast<std::uint16_t>(num_ptls_minus1) + 1;
    }

    [[nodiscard]]
    bool valid() const noexcept {
        return vps_id <= 15;
    }
};

}  // namespace vvc
}  // namespace bs
