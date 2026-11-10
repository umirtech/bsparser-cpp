// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "stream.hpp"

#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace bs {
namespace demux {
namespace avi {

/*
 * -----------------------------------------------------------
 * AVI (RIFF) demuxer (limited)
 * -----------------------------------------------------------
 *
 * Extracts the first video stream from an AVI file as zero-copy spans:
 *
 *   H.264 / HEVC  -> Annex-B or length-prefixed NAL spans
 *   VP8 / VP9     -> raw frame spans
 *   AV1           -> OBU spans
 */

namespace detail {

inline std::uint32_t read_u32(std::span<const std::uint8_t> d, std::size_t p) {
    if (p + 4 > d.size()) {
        throw std::out_of_range("AVI: truncated chunk field");
    }
    /* RIFF/AVI sizes are little-endian. */
    return static_cast<std::uint32_t>(d[p]) | (static_cast<std::uint32_t>(d[p + 1]) << 8) |
           (static_cast<std::uint32_t>(d[p + 2]) << 16) |
           (static_cast<std::uint32_t>(d[p + 3]) << 24);
}

inline bool is_fourcc(std::span<const std::uint8_t> d, std::size_t p, const char* s) {
    return p + 4 <= d.size() && std::memcmp(d.data() + p, s, 4) == 0;
}

struct Chunk {
    std::size_t start = 0;
    std::size_t data = 0;
    std::size_t end = 0;
    std::size_t size = 0;
    char fourcc[5] = {0, 0, 0, 0, 0};
    bool is_list = false;
};

/*
 * AVI chunks are word-aligned: an odd-sized chunk is padded with one
 * byte.  `size` is the real data length; `end` is the aligned boundary
 * (data + size + pad) so callers can simply advance with `end`.
 */
inline Chunk chunk_at(std::span<const std::uint8_t> d, std::size_t p) {
    Chunk c;
    c.start = p;

    if (p + 8 > d.size()) {
        return c;
    }

    std::memcpy(c.fourcc, d.data() + p, 4);

    const std::uint32_t size = read_u32(d, p + 4);

    c.size = size;
    c.data = p + 8;
    c.end = c.data + size + (size & 1u);
    if (c.end > d.size()) {
        c.end = d.size();
    }

    c.is_list = std::memcmp(c.fourcc, "LIST", 4) == 0;

    return c;
}

}  // namespace detail

}  // namespace avi
}  // namespace demux
}  // namespace bs
