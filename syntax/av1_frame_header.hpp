// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "av1_common.hpp"

#include <cstdint>

namespace bs {
namespace av1 {

/*
 * -----------------------------------------------------------
 * AV1 frame header (AV1 §5.9.2) — full uncompressed_header
 * through film_grain (AV1 §5.9.2 … §5.9.31)
 * -----------------------------------------------------------
 * Everything is boolean-coded.  Parsed through the
 * `order_hint` field, which is the native presentation-order
 * signal (sort frames by it, wrap-aware).
 */

struct FilmGrainParams {
    bool apply_grain = false;
    std::uint16_t grain_seed = 0;
    bool update_grain = false;
    std::uint8_t film_grain_params_ref_idx = 0;
    std::uint8_t num_y_points = 0;
    std::uint8_t point_y_value[14] = {};
    std::uint8_t point_y_scaling[14] = {};
    bool chroma_scaling_from_luma = false;
    std::uint8_t num_cb_points = 0;
    std::uint8_t point_cb_value[10] = {};
    std::uint8_t point_cb_scaling[10] = {};
    std::uint8_t num_cr_points = 0;
    std::uint8_t point_cr_value[10] = {};
    std::uint8_t point_cr_scaling[10] = {};
    std::uint8_t grain_scaling_minus_8 = 0;
    std::uint8_t ar_coeff_lag = 0;
    std::uint8_t ar_coeffs_y_plus_128[24] = {};
    std::uint8_t ar_coeffs_cb_plus_128[25] = {};
    std::uint8_t ar_coeffs_cr_plus_128[25] = {};
    std::uint8_t ar_coeff_shift_minus_6 = 0;
    std::uint8_t grain_scale_shift = 0;
    std::uint8_t cb_mult = 0;
    std::uint8_t cb_luma_mult = 0;
    std::uint16_t cb_offset = 0;
    std::uint8_t cr_mult = 0;
    std::uint8_t cr_luma_mult = 0;
    std::uint16_t cr_offset = 0;
    bool overlap_flag = false;
    bool clip_to_restricted_range = false;
};

struct FrameHeader {
    FrameType frame_type = FrameType::InterFrame;

    bool show_frame = false;

    bool show_existing_frame = false;

    std::uint8_t frame_to_show_map_idx = 0;

    std::uint32_t frame_presentation_time = 0;
    std::uint32_t display_frame_id = 0;

    bool showable_frame = false;

    bool error_resilient_mode = false;

    bool disable_cdf_update = false;

    bool allow_screen_content_tools = false;

    bool force_integer_mv = false;

    // frame id / size override / order hint
    std::uint32_t current_frame_id = 0;
    bool frame_size_override_flag = false;

    /*
     * order_hint (AV1 §5.9.2): the presentation-order signal.  Its width is
     * OrderHintBits from the sequence header; frames are presented in order
     * of order_hint (compared wrap-aware via get_relative_dist).
     */
    std::uint32_t order_hint = 0;

    /*
     * Decode-order index of this frame, filled by the unified dispatch layer.
     */
    std::int32_t presentation_order = 0;

    bool buffer_removal_time_present_flag = false;
    std::uint32_t buffer_removal_time[32] = {};

    std::uint8_t primary_ref_frame = 7;  // 7 = PRIMARY_REF_NONE

    // frame size / superres / render size
    std::uint16_t frame_width = 0;
    std::uint16_t frame_height = 0;
    std::uint16_t frame_width_minus_1 = 0;
    std::uint16_t frame_height_minus_1 = 0;
    bool use_superres = false;
    std::uint8_t coded_denom = 8;
    std::uint8_t superres_denom = 8;
    std::uint16_t upscaled_width = 0;
    bool render_and_frame_size_different = false;
    std::uint16_t render_width = 0;
    std::uint16_t render_height = 0;
    std::uint16_t render_width_minus_1 = 0;
    std::uint16_t render_height_minus_1 = 0;

    bool found_ref[7] = {};
    std::uint8_t refresh_frame_flags = 0;

    bool allow_intrabc = false;

    std::uint8_t ref_order_hint[8] = {};
    bool frame_refs_short_signaling = false;
    std::uint8_t last_frame_idx = 0;
    std::uint8_t golden_frame_idx = 0;
    std::int8_t ref_frame_idx[7] = {};
    std::uint32_t delta_frame_id_minus1[7] = {};

    bool allow_high_precision_mv = false;
    bool is_filter_switchable = false;
    std::uint8_t interpolation_filter = 0;  // 0 EIGHTTAP .. 4 SWITCHABLE
    bool is_motion_mode_switchable = false;
    bool use_ref_frame_mvs = false;

    bool disable_frame_end_update_cdf = false;

    // tile info
    bool uniform_tile_spacing_flag = false;
    std::uint8_t tile_cols_log2 = 0;
    std::uint8_t tile_rows_log2 = 0;
    std::uint8_t tile_cols = 0;
    std::uint8_t tile_rows = 0;
    std::uint8_t tile_start_col_sb[64] = {};
    std::uint8_t tile_start_row_sb[64] = {};
    std::uint8_t width_in_sbs_minus_1[64] = {};
    std::uint8_t height_in_sbs_minus_1[64] = {};
    std::uint16_t context_update_tile_id = 0;
    std::uint8_t tile_size_bytes_minus1 = 0;

    // quantization
    std::uint8_t base_q_idx = 0;
    std::int8_t delta_q_y_dc = 0;
    bool diff_uv_delta = false;
    std::int8_t delta_q_u_dc = 0;
    std::int8_t delta_q_u_ac = 0;
    std::int8_t delta_q_v_dc = 0;
    std::int8_t delta_q_v_ac = 0;
    bool using_qmatrix = false;
    std::uint8_t qm_y = 0;
    std::uint8_t qm_u = 0;
    std::uint8_t qm_v = 0;

    // segmentation
    bool segmentation_enabled = false;
    bool segmentation_update_map = false;
    bool segmentation_temporal_update = false;
    bool segmentation_update_data = false;
    bool feature_enabled[8][8] = {};
    std::int16_t feature_value[8][8] = {};

    // delta q / lf
    bool delta_q_present = false;
    std::uint8_t delta_q_res = 0;
    bool delta_lf_present = false;
    std::uint8_t delta_lf_res = 0;
    bool delta_lf_multi = false;

    // loop filter
    std::uint8_t loop_filter_level[4] = {};
    std::uint8_t loop_filter_sharpness = 0;
    bool loop_filter_delta_enabled = false;
    bool loop_filter_delta_update = false;
    bool update_ref_delta[8] = {};
    std::int8_t loop_filter_ref_deltas[8] = {};
    bool update_mode_delta[2] = {};
    std::int8_t loop_filter_mode_deltas[2] = {};

    // cdef
    std::uint8_t cdef_damping_minus_3 = 0;
    std::uint8_t cdef_bits = 0;
    std::uint8_t cdef_y_pri_strength[8] = {};
    std::uint8_t cdef_y_sec_strength[8] = {};
    std::uint8_t cdef_uv_pri_strength[8] = {};
    std::uint8_t cdef_uv_sec_strength[8] = {};

    // loop restoration
    std::uint8_t lr_type[3] = {};
    std::uint8_t lr_unit_shift = 0;
    std::uint8_t lr_uv_shift = 0;

    std::uint8_t tx_mode = 0;  // 0 ONLY_4X4 .. etc.
    bool reference_select = false;
    bool skip_mode_present = false;

    bool allow_warped_motion = false;
    bool reduced_tx_set = false;

    // global motion
    bool is_global[8] = {};
    bool is_rot_zoom[8] = {};
    bool is_translation[8] = {};
    std::uint32_t gm_params[8][6] = {};

    FilmGrainParams film_grain{};

    // derived helpers
    bool frame_is_intra = false;

    [[nodiscard]]
    bool valid() const noexcept {
        return true;
    }
};

}  // namespace av1
}  // namespace bs
