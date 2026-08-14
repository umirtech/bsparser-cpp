#pragma once

#include "vp8_frame_header.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace bs {
namespace vp8 {

/*
 * -----------------------------------------------------------
 * VP8 frame header parser (RFC 6386 §9.1)
 * -----------------------------------------------------------
 *
 * The header is a byte-aligned fixed-width structure, so no bit
 * reader is required — it reads directly from the frame bytes.
 */
inline FrameHeader parse_frame_header(std::span<const std::uint8_t> data) {
    if (data.size() < 3) {
        throw std::out_of_range("VP8: truncated frame header");
    }

    FrameHeader header;

    const std::uint32_t tag = (static_cast<std::uint32_t>(data[0]) << 16) |
                              (static_cast<std::uint32_t>(data[1]) << 8) |
                              static_cast<std::uint32_t>(data[2]);

    header.key_frame = (tag >> 23) & 1u;

    header.version = static_cast<std::uint8_t>((tag >> 20) & 7u);

    header.show_frame = (tag >> 19) & 1u;

    header.first_part_size = tag & 0x1FFFFFu;

    if (!header.key_frame) {
        return header;
    }

    if (data.size() < 10) {
        throw std::out_of_range("VP8: truncated key frame header");
    }

    header.start_code_ok = data[3] == 0x9du && data[4] == 0x01u && data[5] == 0x2au;

    header.width = static_cast<std::uint16_t>((data[6] | (data[7] << 8)) & 0x3FFFu);

    header.height = static_cast<std::uint16_t>((data[8] | (data[9] << 8)) & 0x3FFFu);

    return header;
}

}  // namespace vp8
}  // namespace bs
