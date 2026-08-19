#pragma once

#include <cstdint>

namespace bs {
namespace av1 {

/*
 * -----------------------------------------------------------
 * AV1 sequence header (AV1 §5.5, Annex A sequence_header_obu)
 * -----------------------------------------------------------
 * Full syntax: color_config, timing_info, decoder_model_info,
 * operating parameters, frame dimensions, frame-id, feature
 * flags (superblock, filter intra, etc.), order-hint, screen
 * content, superres/cdef/restoration, film grain.
 * Parsed through the feature flags that gate the frame header
 * (`order_hint_bits`, screen-content/integer-MV selection,
 * frame-id signalling).  Everything is boolean-coded.
 */

struct ColorConfig {
    bool high_bitdepth = false;
    bool twelve_bit = false;
    bool mono_chrome = false;

    bool color_description_present_flag = false;
    std::uint8_t color_primaries = 0;
    std::uint8_t transfer_characteristics = 0;
    std::uint8_t matrix_coefficients = 0;

    bool color_range = false;
    std::uint8_t subsampling_x = 1;
    std::uint8_t subsampling_y = 1;
    std::uint8_t chroma_sample_position = 0;
    bool separate_uv_delta_q = false;

    [[nodiscard]]
    std::uint8_t bit_depth() const noexcept {
        if (twelve_bit)
            return 12;
        if (high_bitdepth)
            return 10;
        return 8;
    }
};

struct TimingInfo {
    std::uint32_t num_units_in_display_tick = 0;
    std::uint32_t time_scale = 0;
    bool equal_picture_interval = false;
    std::uint32_t num_ticks_per_picture_minus_1 = 0;
};

struct DecoderModelInfo {
    std::uint8_t buffer_delay_length_minus_1 = 0;
    std::uint32_t num_units_in_decoding_tick = 0;
    std::uint8_t buffer_removal_time_length_minus_1 = 0;
    std::uint8_t frame_presentation_time_length_minus_1 = 0;
};

struct SequenceHeader {
    std::uint8_t seq_profile = 0;

    bool still_picture = false;

    bool reduced_still_picture_header = false;

    // timing / decoder model (Annex A)
    bool timing_info_present_flag = false;
    TimingInfo timing_info{};

    bool decoder_model_info_present_flag = false;
    DecoderModelInfo decoder_model_info{};

    bool initial_display_delay_present_flag = false;
    std::uint8_t operating_points_cnt_minus_1 = 0;
    std::uint16_t operating_point_idc[32] = {};
    std::uint8_t seq_level_idx[32] = {};
    bool seq_tier[32] = {};
    bool decoder_model_present_for_this_op[32] = {};
    std::uint32_t decoder_buffer_delay[32] = {};
    std::uint32_t encoder_buffer_delay[32] = {};
    bool low_delay_mode_flag[32] = {};
    bool initial_display_delay_present_for_this_op[32] = {};
    std::uint8_t initial_display_delay_minus_1[32] = {};

    // frame dimensions
    std::uint8_t frame_width_bits_minus_1 = 0;
    std::uint8_t frame_height_bits_minus_1 = 0;
    std::uint32_t max_frame_width = 0;
    std::uint32_t max_frame_height = 0;
    bool dimensions_present = false;

    /* -------------------------------------------------------
     * Feature flags that gate the frame header.
     * -------------------------------------------------------
     */

    /*
     * Frame-id signalling (needed to skip the current_frame_id /
     * delta_frame_id fields in the frame header).
     */
    bool frame_id_numbers_present_flag = false;
    std::uint8_t delta_frame_id_length_minus_2 = 0;
    std::uint8_t additional_frame_id_length_minus_1 = 0;

    // superblock / filter / compound flags
    bool use_128x128_superblock = false;
    bool enable_filter_intra = false;
    bool enable_intra_edge_filter = false;
    bool enable_interintra_compound = false;
    bool enable_masked_compound = false;
    bool enable_warped_motion = false;
    bool enable_dual_filter = false;

    /*
     * enable_order_hint and order_hint_bits_minus_1; the frame
     * header's order_hint field is f(OrderHintBits) bits.
     */
    bool enable_order_hint = false;
    bool enable_jnt_comp = false;
    bool enable_ref_frame_mvs = false;
    std::uint8_t order_hint_bits_minus_1 = 0;

    // screen content
    bool seq_choose_screen_content_tools = false;
    /*
     * Screen-content / integer-MV signalling, using the AV1 spec enum
     * values (AV1 §5.5.1): 0 = OFF, 1 = ON, 2 = SELECT (the frame
     * header signals it).
     */
    std::uint8_t seq_force_screen_content_tools = 0;
    bool seq_choose_integer_mv = false;
    std::uint8_t seq_force_integer_mv = 0;

    bool enable_superres = false;
    bool enable_cdef = false;
    bool enable_restoration = false;

    ColorConfig color_config{};

    bool film_grain_params_present = false;

    // compatibility alias for reduced still picture level
    std::uint8_t seq_level_idx_0 = 0;
    bool seq_tier_0 = false;

    // legacy timing aliases kept for backward compat
    bool equal_picture_interval = false;
    std::uint8_t buffer_delay_length_minus_1 = 0;
    std::uint8_t buffer_removal_time_length_minus_1 = 0;
    std::uint8_t frame_presentation_time_length_minus_1 = 0;

    [[nodiscard]]
    std::uint8_t order_hint_bits() const noexcept {
        return enable_order_hint ? static_cast<std::uint8_t>(order_hint_bits_minus_1 + 1) : 0;
    }

    [[nodiscard]]
    std::uint8_t frame_id_length() const noexcept {
        return static_cast<std::uint8_t>(
            additional_frame_id_length_minus_1 + delta_frame_id_length_minus_2 + 3
        );
    }

    [[nodiscard]]
    bool valid() const noexcept {
        return seq_profile <= 2;
    }
};

}  // namespace av1
}  // namespace bs
