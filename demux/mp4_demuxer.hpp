// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "es_reconstruct.hpp"
#include "stream.hpp"

#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace bs {
namespace demux {
namespace mp4 {

/*
 * -----------------------------------------------------------
 * MP4 / ISO-BMFF demuxer (limited)
 * -----------------------------------------------------------
 *
 * Indexes MP4/ISO-BMFF sample spans without copying payloads:
 *
 *   avc1 / hvc1 / vvc1  -> length-prefixed NAL spans (parameter sets
 *                          extracted from avcC/hvcC/vvcC)
 *   av01                -> OBU spans
 *   vp09 / vp08         -> raw frame spans
 *
 * Fragmented MP4 (`moof`) and files whose `moov` atom is at the end are
 * supported.
 */

namespace detail {

inline std::uint32_t read_u32(std::span<const std::uint8_t> d, std::size_t p) {
    if (p + 4 > d.size()) {
        throw std::out_of_range("MP4: truncated box field");
    }
    return (static_cast<std::uint32_t>(d[p]) << 24) | (static_cast<std::uint32_t>(d[p + 1]) << 16) |
           (static_cast<std::uint32_t>(d[p + 2]) << 8) | static_cast<std::uint32_t>(d[p + 3]);
}

inline std::uint64_t read_u64(std::span<const std::uint8_t> d, std::size_t p) {
    if (p + 8 > d.size()) {
        throw std::out_of_range("MP4: truncated box field");
    }
    return (static_cast<std::uint64_t>(read_u32(d, p)) << 32) | read_u32(d, p + 4);
}

inline bool is_fourcc(std::span<const std::uint8_t> d, std::size_t p, const char* s) {
    return p + 4 <= d.size() && std::memcmp(d.data() + p, s, 4) == 0;
}

struct Box {
    std::size_t start = 0;
    std::size_t data = 0;
    std::size_t end = 0;
};

inline Box box_at(std::span<const std::uint8_t> d, std::size_t p) {
    Box b;
    b.start = p;

    if (p + 8 > d.size()) {
        return b;
    }

    std::uint64_t size = read_u32(d, p);

    if (size == 1) {
        if (p + 16 > d.size()) {
            return b;
        }
        size = read_u64(d, p + 8);
        b.data = p + 16;
    } else if (size == 0) {
        size = d.size() - p;
        b.data = p + 8;
    } else {
        b.data = p + 8;
    }

    b.end = p + static_cast<std::size_t>(size);
    if (b.end > d.size()) {
        b.end = d.size();
    }

    return b;
}

/*
 * Find a child box of `type` within [begin, end). Returns its
 * position, or d.size() when absent.
 */
inline std::size_t find_child(
    std::span<const std::uint8_t> d, std::size_t begin, std::size_t end, const char* type
) {
    std::size_t p = begin;

    while (p + 8 <= end) {
        Box b = box_at(d, p);

        if (b.end <= p) {
            break;
        }

        if (p + 4 <= b.end && is_fourcc(d, p + 4, type)) {
            return p;
        }

        p = b.end;
    }

    return d.size();
}

struct VideoEntry {
    bool found = false;
    std::string fourcc;
    std::size_t config_offset = 0;
    std::size_t config_size = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};

/*
 * Parse stsd, locate the first video sample entry.
 */
inline VideoEntry parse_stsd(std::span<const std::uint8_t> d, std::size_t stsd_pos) {
    Box stsd = box_at(d, stsd_pos);

    VideoEntry e;

    if (stsd.data + 8 > stsd.end) {
        return e;
    }

    const std::uint32_t entry_count = read_u32(d, stsd.data + 4);

    std::size_t p = stsd.data + 8;

    for (std::uint32_t i = 0; i < entry_count && p + 8 <= stsd.end; ++i) {
        Box entry = box_at(d, p);

        if (entry.end <= p) {
            break;
        }

        const std::string type = std::string(reinterpret_cast<const char*>(d.data() + p + 4), 4);

        const bool is_video = type == "avc1" || type == "hvc1" || type == "hev1" ||
                              type == "vvc1" || type == "vvcC" || type == "av01" ||
                              type == "vp09" || type == "vp08";

        if (is_video) {
            e.found = true;
            e.fourcc = type;

            /*
             * VisualSampleEntry layout (offsets within the entry):
             *   0-7   size + format
             *   8-13  SampleEntry reserved (6)
             *   14-15 data_reference_index
             *   16-17 pre_defined
             *   18-19 reserved
             *   20-31 pre_defined[3]
             *   32-33 width
             *   34-35 height
             */
            if (p + 36 <= entry.end) {
                e.width = static_cast<std::uint16_t>(read_u32(d, p + 32) >> 16);
                e.height = static_cast<std::uint16_t>(read_u32(d, p + 32) & 0xFFFFu);
            }

            /*
             * Locate the codec-config box (avcC/hvcC/vvcC/av1C).
             * Its offset within the sample entry varies between
             * muxers, so scan for the fourcc.
             */
            for (std::size_t q = p + 8; q + 8 <= entry.end; ++q) {
                if (is_fourcc(d, q + 4, "hvcC") || is_fourcc(d, q + 4, "avcC") ||
                    is_fourcc(d, q + 4, "vvcC") || is_fourcc(d, q + 4, "av1C")) {
                    Box cfg = box_at(d, q);
                    e.config_offset = cfg.data;
                    e.config_size = cfg.end - cfg.data;
                    break;
                }
            }

            return e;
        }

        p = entry.end;
    }

    return e;
}

}  // namespace detail

/*
 * Demux an MP4 file into an elementary stream.
 */
}  // namespace mp4
}  // namespace demux
}  // namespace bs
