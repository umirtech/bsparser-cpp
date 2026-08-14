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
 *
 * Field order follows the VP9 bitstream specification (and
 * FFmpeg's vp9.c): after the common prefix a KEY frame carries a
 * 24-bit sync code (0x498342) + color config before the frame
 * size.
 */

namespace detail {

inline void skip_color_config(PlainBitReader& r, std::uint8_t profile) {
    if (profile >= 2) {
        (void)r.read_bit(); /* bit_depth */
    }

    const std::uint8_t color_space = static_cast<std::uint8_t>(r.read_bits(3));

    if (color_space == 7) {
        (void)r.read_bit(); /* reserved_zero */

    } else {
        (void)r.read_bit(); /* color_range */

        if (profile == 1 || profile == 3) {
            (void)r.read_bit(); /* subsampling_x */
            (void)r.read_bit(); /* subsampling_y */
            (void)r.read_bit(); /* reserved_zero */
        }
    }
}

}  // namespace detail

inline FrameHeader parse_frame_header(std::span<const std::uint8_t> data) {
    PlainBitReader r{data};

    FrameHeader header;

    header.frame_marker = static_cast<std::uint8_t>(r.read_bits(2));

    if (!header.valid()) {
        throw std::runtime_error("VP9: bad frame marker");
    }

    const std::uint8_t profile_low = static_cast<std::uint8_t>(r.read_bit());

    const std::uint8_t profile_high = static_cast<std::uint8_t>(r.read_bit());

    header.profile = static_cast<std::uint8_t>((profile_high << 1) | profile_low);

    if (header.profile == 3) {
        (void)r.read_bits(2); /* reserved_zero_2bits */
    }

    header.show_existing_frame = r.read_bit();

    if (header.show_existing_frame) {
        header.frame_to_show_map_idx = static_cast<std::uint8_t>(r.read_bits(3));

        return header;
    }

    header.frame_type = r.read_bit() ? FrameType::InterFrame : FrameType::KeyFrame;

    header.show_frame = r.read_bit();

    header.error_resilient_mode = r.read_bit();

    if (header.frame_type == FrameType::KeyFrame) {
        if (r.read_bits(24) != 0x498342u) {
            throw std::runtime_error("VP9: bad sync code");
        }

        detail::skip_color_config(r, header.profile);

        header.width = r.read_bits(16) + 1u;
        header.height = r.read_bits(16) + 1u;
        header.frame_size_present = true;

        return header;
    }

    header.intra_only = header.show_frame ? false : r.read_bit();

    if (header.intra_only) {
        (void)r.read_bits(24); /* sync code */

        detail::skip_color_config(r, header.profile);

        (void)r.read_bits(8); /* refresh_frame_flags */

        header.width = r.read_bits(16) + 1u;
        header.height = r.read_bits(16) + 1u;
        header.frame_size_present = true;

    } else {
        header.frame_size_from_refs = true;
    }

    return header;
}

}  // namespace vp9
}  // namespace bs
