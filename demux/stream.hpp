// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "bsparser.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bs {
namespace demux {

/*
 * -----------------------------------------------------------
 * Container formats
 * -----------------------------------------------------------
 */
enum class Container : std::uint8_t { Unknown, Mp4, MpegTs, Avi, Flv, Ivf, Mkv, Ogg, Ps };

/*
 * -----------------------------------------------------------
 * Zero-copy track / sample views
 * -----------------------------------------------------------
 *
 * A TrackView describes one container track. A SampleView is one
 * access unit / sample and points into the demuxer's file-backed
 * buffer (usually an mmap of the input file).
 *
 * `mmap_backed` tells the consumer how long the span is valid:
 *
 *   true   the span points into mapped file memory; it stays valid
 *          until seek() / close() / demuxer destruction.
 *   false  the span points into a small internal / borrowed buffer;
 *          copy it if it must outlive the next next() call.
 */
struct TrackView {
    std::uint32_t id = 0;
    Codec codec = Codec::Hevc;
    NalFramingMode framing = NalFramingMode::AnnexB;

    std::string codec_name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t timescale = 0;
    std::string language;

    bool is_video = false;
    bool is_audio = false;
    bool is_subtitle = false;

    std::size_t sample_count = 0;
    unsigned length_size = 4; /* for LengthPrefixed framing */
    bool mmap_backed = false;
};

struct SampleView {
    std::uint32_t track_id = 0;
    std::size_t index = 0;

    std::span<const std::uint8_t> data{};

    std::int64_t pts = 0;
    std::int64_t dts = 0;
    std::int64_t duration = 0;
    bool is_key = true;

    NalFramingMode framing = NalFramingMode::AnnexB;
    bool mmap_backed = false;

    [[nodiscard]]
    explicit operator bool() const noexcept {
        return !data.empty();
    }
};

/*
 * Internal index entry: where one sample lives in the file and how
 * it is timed.  The payload itself is never copied here.
 */
struct SampleEntry {
    std::size_t offset = 0;
    std::size_t size = 0;

    std::int64_t pts = 0;
    std::int64_t dts = 0;
    std::int64_t duration = 0;
    bool is_key = true;

    NalFramingMode framing = NalFramingMode::AnnexB;

    /* Small parameter-set blob owned by the demuxer (not file-backed). */
    bool is_param_set = false;
};

}  // namespace demux
}  // namespace bs
