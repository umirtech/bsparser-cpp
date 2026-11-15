// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "stream.hpp"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace bs {
namespace demux {
namespace flv {

/*
 * -----------------------------------------------------------
 * FLV demuxer (limited)
 * -----------------------------------------------------------
 *
 * Extracts video tag spans from an FLV file:
 *
 *   AVC (codec_id 7)  / HEVC (12)  -> length-prefixed NAL spans
 *   AV1 (codec_id 13)               -> OBU spans
 *   VP8 (10) / VP9 (11)             -> raw frame spans
 */

namespace detail {

inline std::uint32_t read_u24(std::span<const std::uint8_t> d, std::size_t p) {
    if (p + 3 > d.size()) {
        throw std::out_of_range("FLV: truncated tag field");
    }
    return (static_cast<std::uint32_t>(d[p]) << 16) | (static_cast<std::uint32_t>(d[p + 1]) << 8) |
           static_cast<std::uint32_t>(d[p + 2]);
}

inline std::uint32_t read_u32(std::span<const std::uint8_t> d, std::size_t p) {
    if (p + 4 > d.size()) {
        throw std::out_of_range("FLV: truncated tag field");
    }
    return (static_cast<std::uint32_t>(d[p]) << 24) | (static_cast<std::uint32_t>(d[p + 1]) << 16) |
           (static_cast<std::uint32_t>(d[p + 2]) << 8) | static_cast<std::uint32_t>(d[p + 3]);
}

}  // namespace detail

}  // namespace flv
}  // namespace demux
}  // namespace bs
