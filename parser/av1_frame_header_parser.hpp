#pragma once

#include "av1_frame_header.hpp"
#include "av1_sequence_header.hpp"

#include <bitstream/boolean_decoder.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace bs {
namespace av1 {

/*
 * -----------------------------------------------------------
 * AV1 frame header parser (AV1 §5.7, §5.9.2) — leading fields
 * -----------------------------------------------------------
 * Parses the uncompressed header up to and including `order_hint`.  The
 * sequence header provides OrderHintBits, the screen-content / integer-MV
 * selection and the frame-id signalling used to reach it.
 */
[[nodiscard]]
inline FrameHeader parse_frame_header(
    std::span<const std::uint8_t> payload, const SequenceHeader& seq
) {
    BooleanDecoder bd{payload};

    FrameHeader fh;

    if (seq.reduced_still_picture_header) {
        fh.frame_type = FrameType::KeyFrame;
        fh.show_frame = true;
        fh.order_hint = 0;
        return fh;
    }

    fh.show_existing_frame = bd.read_bool(128);

    if (fh.show_existing_frame) {
        fh.frame_to_show_map_idx = static_cast<std::uint8_t>(bd.read_literal(3));

        if (seq.decoder_model_info_present_flag && !seq.equal_picture_interval) {
            (void)bd.read_literal(seq.frame_presentation_time_length_minus_1 + 1u);
        }

        if (seq.frame_id_numbers_present_flag) {
            (void)bd.read_literal(seq.frame_id_length()); /* display_frame_id */
        }

        return fh;
    }

    const std::uint8_t raw_frame_type = static_cast<std::uint8_t>(bd.read_literal(2));

    fh.frame_type = static_cast<FrameType>(raw_frame_type);

    const bool frame_is_intra =
        (fh.frame_type == FrameType::KeyFrame || fh.frame_type == FrameType::IntraOnly);

    fh.show_frame = bd.read_bool(128);

    if (fh.show_frame && seq.decoder_model_info_present_flag && !seq.equal_picture_interval) {
        (void)bd.read_literal(seq.frame_presentation_time_length_minus_1 + 1u);
    }

    if (fh.show_frame) {
        fh.showable_frame = fh.frame_type != FrameType::KeyFrame;
    } else {
        fh.showable_frame = bd.read_bool(128);
    }

    if (fh.frame_type == FrameType::Switch ||
        (fh.frame_type == FrameType::KeyFrame && fh.show_frame)) {
        fh.error_resilient_mode = true;
    } else {
        fh.error_resilient_mode = bd.read_bool(128);
    }

    fh.disable_cdf_update = bd.read_bool(128);

    if (seq.seq_force_screen_content_tools == 0) {
        fh.allow_screen_content_tools = bd.read_bool(128);
    } else {
        fh.allow_screen_content_tools = seq.seq_force_screen_content_tools == 1;
    }

    if (fh.allow_screen_content_tools) {
        if (seq.seq_force_integer_mv == 0) {
            fh.force_integer_mv = bd.read_bool(128);
        } else {
            fh.force_integer_mv = seq.seq_force_integer_mv == 1;
        }
    } else {
        fh.force_integer_mv = false;
    }

    if (frame_is_intra) {
        fh.force_integer_mv = true;
    }

    if (seq.frame_id_numbers_present_flag) {
        (void)bd.read_literal(seq.frame_id_length()); /* current_frame_id */
    }

    const std::uint8_t order_hint_bits = seq.order_hint_bits();

    if (fh.frame_type == FrameType::Switch) {
        (void)0; /* frame_size_override_flag = 1, no read */
    } else if (seq.reduced_still_picture_header) {
        (void)0; /* frame_size_override_flag = 0 */
    } else {
        (void)bd.read_bool(128); /* frame_size_override_flag */
    }

    fh.order_hint = (order_hint_bits != 0) ? bd.read_literal(order_hint_bits) : 0;

    return fh;
}

/*
 * Legacy overload without sequence-header context.  OrderHintBits is 0, so
 * order_hint is not read; useful when only the leading fields are needed.
 */
[[nodiscard]]
inline FrameHeader parse_frame_header(std::span<const std::uint8_t> payload) {
    return parse_frame_header(payload, SequenceHeader{});
}

}  // namespace av1
}  // namespace bs
