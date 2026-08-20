// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

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
 *
 * The three-byte frame tag is little-endian (RFC 6386 §9.1 and the
 * normative reference decoder in §19.1 / §20.4):
 *
 *     tag = c[0] | (c[1] << 8) | (c[2] << 16)
 *     key_frame       = !(tag & 1)      (0 = key, 1 = inter)
 *     version         = (tag >> 1) & 7
 *     show_frame      = (tag >> 4) & 1
 *     first_part_size = (tag >> 5) & 0x7FFFF
 */
inline FrameHeader parse_frame_header(std::span<const std::uint8_t> data) {
    if (data.size() < 3) {
        throw std::out_of_range("VP8: truncated frame header");
    }

    FrameHeader header;

    const std::uint32_t tag = static_cast<std::uint32_t>(data[0]) |
                              (static_cast<std::uint32_t>(data[1]) << 8) |
                              (static_cast<std::uint32_t>(data[2]) << 16);

    header.key_frame = (tag & 1u) == 0u;

    header.version = static_cast<std::uint8_t>((tag >> 1) & 7u);

    header.show_frame = (tag >> 4) & 1u;

    header.first_part_size = (tag >> 5) & 0x7FFFFu;

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
