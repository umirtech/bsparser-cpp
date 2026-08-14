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
 * AV1 sequence header parser (AV1 §5.5) — leading fields
 * -----------------------------------------------------------
 */
[[nodiscard]]
inline SequenceHeader
parse_sequence_header(
    std::span<const std::uint8_t> payload)
{
    BooleanDecoder bd{payload};

    SequenceHeader sh;

    sh.seq_profile =
        static_cast<std::uint8_t>(bd.read_literal(3));

    sh.still_picture = bd.read_bool(128);

    sh.reduced_still_picture_header = bd.read_bool(128);

    if (sh.reduced_still_picture_header) {

        const std::uint8_t seq_level_idx_0 =
            static_cast<std::uint8_t>(bd.read_literal(5));

        if (seq_level_idx_0 > 7) {
            (void)bd.read_bool(128); /* seq_tier_0 */
        }

        const std::uint32_t frame_width_bits =
            bd.read_literal(4) + 1u;

        const std::uint32_t frame_height_bits =
            bd.read_literal(4) + 1u;

        sh.max_frame_width =
            bd.read_literal(frame_width_bits) + 1u;

        sh.max_frame_height =
            bd.read_literal(frame_height_bits) + 1u;

        sh.dimensions_present = true;
    }

    return sh;
}

} // namespace av1
} // namespace bs
