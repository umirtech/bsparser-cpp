#pragma once

#include <cstdint>

namespace bs {
namespace av1 {

/*
 * -----------------------------------------------------------
 * AV1 sequence header (AV1 §5.5)
 * -----------------------------------------------------------
 * Parsed through the feature flags that gate the frame header
 * (`order_hint_bits_minus_1`, screen-content/integer-MV selection,
 * frame-id signalling).  Everything is boolean-coded.
 */
struct SequenceHeader {
    std::uint8_t seq_profile = 0;

    bool still_picture = false;

    bool reduced_still_picture_header = false;

    /*
     * Max frame dimensions. Present in reduced-still-picture
     * headers; otherwise not parsed here.
     */
    std::uint32_t max_frame_width = 0;
    std::uint32_t max_frame_height = 0;
    bool dimensions_present = false;

    /* -------------------------------------------------------
     * Feature flags that gate the frame header.
     * -------------------------------------------------------
     */

    /*
     * enable_order_hint and order_hint_bits_minus_1; the frame
     * header's order_hint field is f(OrderHintBits) bits.
     */
    bool enable_order_hint = false;
    std::uint8_t order_hint_bits_minus_1 = 0;

    /*
     * Screen-content / integer-MV signalling, using the AV1 spec enum
     * values (AV1 §5.5.1): 0 = OFF, 1 = ON, 2 = SELECT (the frame
     * header signals it).
     */
    std::uint8_t seq_force_screen_content_tools = 0;
    std::uint8_t seq_force_integer_mv = 0;

    /*
     * Frame-id signalling (needed to skip the current_frame_id /
     * delta_frame_id fields in the frame header).
     */
    bool frame_id_numbers_present_flag = false;
    std::uint8_t delta_frame_id_length_minus_2 = 0;
    std::uint8_t additional_frame_id_length_minus_1 = 0;

    /*
     * Timing / decoder-model context (needed to skip the
     * temporal_point_info() and buffer_removal_time() fields).
     */
    bool timing_info_present_flag = false;
    bool equal_picture_interval = false;
    bool decoder_model_info_present_flag = false;
    std::uint8_t operating_points_cnt_minus_1 = 0;
    std::uint8_t buffer_delay_length_minus_1 = 0;
    std::uint8_t buffer_removal_time_length_minus_1 = 0;
    std::uint8_t frame_presentation_time_length_minus_1 = 0;
    std::uint16_t operating_point_idc[32] = {};
    bool decoder_model_present_for_this_op[32] = {};

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
        return seq_profile <= 7;
    }
};

}  // namespace av1
}  // namespace bs
