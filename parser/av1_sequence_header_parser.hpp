#pragma once

#include "av1_sequence_header.hpp"

#include <bitstream/boolean_decoder.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace bs {
namespace av1 {

/*
 * -----------------------------------------------------------
 * AV1 sequence header parser (AV1 §5.5)
 * -----------------------------------------------------------
 * Walks the sequence header up to the feature flags needed by the frame
 * header: timing / decoder-model context, operating points, frame-id
 * signalling and order_hint_bits_minus_1.
 */
[[nodiscard]]
inline SequenceHeader parse_sequence_header(std::span<const std::uint8_t> payload) {
    BooleanDecoder bd{payload};

    SequenceHeader sh;

    sh.seq_profile = static_cast<std::uint8_t>(bd.read_literal(3));

    sh.still_picture = bd.read_bool(128);

    sh.reduced_still_picture_header = bd.read_bool(128);

    if (sh.reduced_still_picture_header) {
        const std::uint8_t seq_level_idx_0 = static_cast<std::uint8_t>(bd.read_literal(5));

        if (seq_level_idx_0 > 7) {
            (void)bd.read_bool(128); /* seq_tier_0 */
        }

        const std::uint32_t frame_width_bits = bd.read_literal(4) + 1u;

        const std::uint32_t frame_height_bits = bd.read_literal(4) + 1u;

        sh.max_frame_width = bd.read_literal(frame_width_bits) + 1u;

        sh.max_frame_height = bd.read_literal(frame_height_bits) + 1u;

        sh.dimensions_present = true;

        /* reduced_still_picture_header: no timing / feature flags. */
        return sh;
    }

    /* -------------------------------------------------------
     * Timing + decoder model.
     * -------------------------------------------------------
     */
    sh.timing_info_present_flag = bd.read_bool(128);

    if (sh.timing_info_present_flag) {
        (void)bd.read_literal(32); /* num_units_in_display_tick */
        (void)bd.read_literal(32); /* time_scale */
        sh.equal_picture_interval = bd.read_bool(128);
        if (sh.equal_picture_interval) {
            (void)bd.read_uvlc(); /* num_ticks_per_picture_minus_1 */
        }

        sh.decoder_model_info_present_flag = bd.read_bool(128);
        if (sh.decoder_model_info_present_flag) {
            sh.buffer_delay_length_minus_1 = static_cast<std::uint8_t>(bd.read_literal(5));
            (void)bd.read_literal(32); /* num_units_in_decoding_tick */
            sh.buffer_removal_time_length_minus_1 = static_cast<std::uint8_t>(bd.read_literal(5));
            sh.frame_presentation_time_length_minus_1 =
                static_cast<std::uint8_t>(bd.read_literal(5));
        }
    }

    /* -------------------------------------------------------
     * Operating points.
     * -------------------------------------------------------
     */
    const bool initial_display_delay_present_flag = bd.read_bool(128);

    sh.operating_points_cnt_minus_1 = static_cast<std::uint8_t>(bd.read_literal(5));

    for (std::uint8_t i = 0; i <= sh.operating_points_cnt_minus_1; ++i) {
        sh.operating_point_idc[i] = static_cast<std::uint16_t>(bd.read_literal(12));

        const std::uint8_t seq_level_idx = static_cast<std::uint8_t>(bd.read_literal(5));
        if (seq_level_idx > 7) {
            (void)bd.read_bool(128); /* seq_tier */
        }

        if (sh.decoder_model_info_present_flag) {
            sh.decoder_model_present_for_this_op[i] = bd.read_bool(128);
            if (sh.decoder_model_present_for_this_op[i]) {
                const std::uint8_t n =
                    static_cast<std::uint8_t>(sh.buffer_delay_length_minus_1 + 1);
                (void)bd.read_literal(n); /* decoder_buffer_delay */
                (void)bd.read_literal(n); /* encoder_buffer_delay */
                (void)bd.read_bool(128);  /* low_delay_mode_flag */
            }
        }

        if (initial_display_delay_present_flag) {
            if (bd.read_bool(128)) {
                (void)bd.read_literal(4); /* initial_display_delay_minus_1 */
            }
        }
    }

    /* -------------------------------------------------------
     * Frame dimensions.
     * -------------------------------------------------------
     */
    const std::uint32_t frame_width_bits = bd.read_literal(4) + 1u;

    const std::uint32_t frame_height_bits = bd.read_literal(4) + 1u;

    sh.max_frame_width = bd.read_literal(frame_width_bits) + 1u;

    sh.max_frame_height = bd.read_literal(frame_height_bits) + 1u;

    sh.dimensions_present = true;

    /* -------------------------------------------------------
     * Frame-id signalling.
     * -------------------------------------------------------
     */
    sh.frame_id_numbers_present_flag = bd.read_bool(128);

    if (sh.frame_id_numbers_present_flag) {
        sh.delta_frame_id_length_minus_2 = static_cast<std::uint8_t>(bd.read_literal(4));
        sh.additional_frame_id_length_minus_1 = static_cast<std::uint8_t>(bd.read_literal(3));
    }

    /* -------------------------------------------------------
     * Feature flags.
     * -------------------------------------------------------
     */
    (void)bd.read_bool(128); /* use_128x128_superblock */
    (void)bd.read_bool(128); /* enable_filter_intra */
    (void)bd.read_bool(128); /* enable_intra_edge_filter */

    (void)bd.read_bool(128); /* enable_interintra_compound */
    (void)bd.read_bool(128); /* enable_masked_compound */
    (void)bd.read_bool(128); /* enable_warped_motion */
    (void)bd.read_bool(128); /* enable_dual_filter */

    sh.enable_order_hint = bd.read_bool(128);

    if (sh.enable_order_hint) {
        (void)bd.read_bool(128); /* enable_jnt_comp */
        (void)bd.read_bool(128); /* enable_ref_frame_mvs */
    }

    /*
     * seq_force_screen_content_tools / seq_force_integer_mv use the
     * spec enum values: 0 = OFF, 1 = ON, 2 = SELECT (AV1 §5.5.1).
     */
    if (bd.read_bool(128)) {
        sh.seq_force_screen_content_tools = 2; /* SELECT_SCREEN_CONTENT_TOOLS */
    } else {
        sh.seq_force_screen_content_tools = static_cast<std::uint8_t>(bd.read_bool(128) ? 1u : 0u);
    }

    if (sh.seq_force_screen_content_tools > 0) {
        if (bd.read_bool(128)) {
            sh.seq_force_integer_mv = 2; /* SELECT_INTEGER_MV */
        } else {
            sh.seq_force_integer_mv = static_cast<std::uint8_t>(bd.read_bool(128) ? 1u : 0u);
        }
    } else {
        sh.seq_force_integer_mv = 0; /* OFF_INTEGER_MV */
    }

    if (sh.enable_order_hint) {
        sh.order_hint_bits_minus_1 = static_cast<std::uint8_t>(bd.read_literal(3));
    }

    return sh;
}

}  // namespace av1
}  // namespace bs
