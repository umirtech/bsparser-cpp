#pragma once

#include "vp9_frame_header.hpp"

#include <bitstream/plain_bit_reader.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace bs {
namespace vp9 {

/*
 * -----------------------------------------------------------
 * VP9 uncompressed frame header parser
 * -----------------------------------------------------------
 *
 * Parses the fixed-width start of the uncompressed header using
 * the plain (no emulation-prevention) bit reader.
 */
inline FrameHeader
parse_frame_header(
    std::span<const std::uint8_t> data)
{
    PlainBitReader r{data};

    FrameHeader header;

    header.frame_marker =
        static_cast<std::uint8_t>(r.read_bits(2));

    if (!header.valid()) {
        throw std::runtime_error(
            "VP9: bad frame marker");
    }

    const std::uint8_t profile_low =
        static_cast<std::uint8_t>(r.read_bit());

    const std::uint8_t profile_high =
        static_cast<std::uint8_t>(r.read_bit());

    header.profile =
        static_cast<std::uint8_t>(
            (profile_high << 1) | profile_low);

    if (header.profile == 3) {
        (void)r.read_bits(2); /* reserved_zero_2bits */
    }

    header.show_existing_frame = r.read_bit();

    if (header.show_existing_frame) {

        header.frame_to_show_map_idx =
            static_cast<std::uint8_t>(r.read_bits(3));

        return header;
    }

    header.frame_type =
        r.read_bit()
            ? FrameType::InterFrame
            : FrameType::KeyFrame;

    header.show_frame = r.read_bit();

    header.error_resilient_mode = r.read_bit();

    if (header.frame_type == FrameType::KeyFrame) {

        header.width = r.read_bits(16);
        header.height = r.read_bits(16);
        header.frame_size_present = true;

        return header;
    }

    header.intra_only = r.read_bit();

    if (header.intra_only) {

        header.width = r.read_bits(16);
        header.height = r.read_bits(16);
        header.frame_size_present = true;

    } else {

        header.frame_size_from_refs = true;
    }

    return header;
}

} // namespace vp9
} // namespace bs
