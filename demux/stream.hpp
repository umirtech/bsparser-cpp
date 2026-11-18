// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "bsparser.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bs {
namespace demux {

/*
 * -----------------------------------------------------------
 * Container / track metadata
 * -----------------------------------------------------------
 *
 * A codec-neutral view of the standard tag sets carried by MP4
 * (udta/meta/ilst) and Matroska (Info/Tags).  `creation_time` is
 * Unix seconds (0 = unset); `custom` holds container-specific
 * key/value pairs that have no dedicated field.
 */
struct ContainerMetadata {
    std::string title;
    std::string artist;
    std::string comment;
    std::string copyright;
    std::int64_t creation_time = 0;

    std::vector<std::pair<std::string, std::string>> custom;
};

/*
 * -----------------------------------------------------------
 * Container formats
 * -----------------------------------------------------------
 */
enum class Container : std::uint8_t { Unknown, Mp4, MpegTs, Avi, Flv, Ivf, Mkv, Ogg, Ps };

/*
 * Audio codec identifiers.  Kept separate from the video-only bs::Codec so
 * an audio track can be described without abusing a video codec value.
 */
enum class AudioCodec : std::uint8_t {
    Unknown,
    Pcm8, Pcm16, Pcm24, Pcm32, PcmFloat32, PcmFloat64,
    Aac, Mp3, Opus, Vorbis, Ac3, Ec3, Dts, Flac, Alac
};

[[nodiscard]]
inline constexpr unsigned audio_bits_per_sample(AudioCodec codec) noexcept {
    switch (codec) {
        case AudioCodec::Pcm8:  return 8u;
        case AudioCodec::Pcm16: return 16u;
        case AudioCodec::Pcm24: return 24u;
        case AudioCodec::Pcm32: return 32u;
        case AudioCodec::PcmFloat32: return 32u;
        case AudioCodec::PcmFloat64: return 64u;
        default: return 0u; // compressed: no fixed PCM bit depth
    }
}

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

    /* Audio configuration (meaningful when is_audio is true). */
    AudioCodec audio_codec = AudioCodec::Unknown;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 0;
    std::vector<std::uint8_t> audio_config; /* e.g. AAC AudioSpecificConfig */

    /* Subtitle/text identification (meaningful when is_subtitle is true). */
    std::string subtitle_codec; /* e.g. "tx3g", "WebVTT", "S_TEXT/UTF8" */

    std::size_t sample_count = 0;
    unsigned length_size = 4; /* for LengthPrefixed framing */
    bool mmap_backed = false;

    ContainerMetadata metadata; /* per-track tags, when the container carries them */
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
