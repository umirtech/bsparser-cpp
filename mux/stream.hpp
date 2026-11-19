// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "bsparser.hpp"
#include "demux/stream.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bs {
namespace mux {

/*
 * -----------------------------------------------------------
 * Container formats (output side)
 * -----------------------------------------------------------
 *
 * These mirror bs::demux::Container for the formats the muxer can
 * write.  to_demux()/from_demux() convert between the two enums so
 * a muxed buffer can be fed straight back through Demuxer::init().
 */
enum class Container : std::uint8_t { Unknown, Mp4, Mkv, Ivf, Wav };

/* Unified codec IDs: the muxer uses the same bs::Codec values as the
 * demuxer and parser.  This alias keeps source compatibility. */
using AudioCodec = Codec;

/* Optional progress / error reporting for a mux session. */
struct MuxerOptions {
    std::size_t expected_size = 0;
    std::size_t initial_map_size = 0;       // 0 -> FileSink::kDefaultChunk
    bool fast_start = false;               // reserve/patch moov at the front
    bool try_mmap = true;                  // false -> force buffered fallback
    // Hold up to this many samples in a bounded dts-ordered queue before
    // writing.  0 keeps the zero-copy immediate path.  A value such as 16-64
    // gives the muxer the same small reorder window a live A/V encoder pair
    // needs; samples are copied into the queue while it is active.
    std::size_t reorder_window = 0;
    void* ctx = nullptr;
    void (*on_progress)(void*, std::uint64_t /*bytes*/, std::uint64_t /*total*/) = nullptr;
    void (*on_error)(void*, const char* /*message*/) = nullptr;
};

[[nodiscard]]
inline constexpr unsigned audio_bits_per_sample(AudioCodec codec) noexcept {
    const unsigned bits = codec_bits_per_sample(codec);
    return bits != 0u ? bits : 16u;
}

/*
 * Write-side mirror of demux::ContainerMetadata.  Same field layout so a
 * muxer's metadata can be fed straight to Demuxer::metadata() in tests.
 */
struct Metadata {
    std::string title;
    std::string artist;
    std::string comment;
    std::string copyright;
    std::int64_t creation_time = 0; /* Unix seconds, 0 = unset */

    std::vector<std::pair<std::string, std::string>> custom;
};

[[nodiscard]]
inline demux::ContainerMetadata to_demux(const Metadata& m) {
    demux::ContainerMetadata out;
    out.title = m.title;
    out.artist = m.artist;
    out.comment = m.comment;
    out.copyright = m.copyright;
    out.creation_time = m.creation_time;
    out.custom = m.custom;
    return out;
}

[[nodiscard]]
inline demux::Container to_demux(Container c) noexcept {
    switch (c) {
        case Container::Mp4:
            return demux::Container::Mp4;
        case Container::Mkv:
            return demux::Container::Mkv;
        case Container::Ivf:
            return demux::Container::Ivf;
        case Container::Wav:
            return demux::Container::Unknown; // demux has no WAV reader
        default:
            return demux::Container::Unknown;
    }
}

[[nodiscard]]
inline Container from_demux(demux::Container c) noexcept {
    switch (c) {
        case demux::Container::Mp4:
            return Container::Mp4;
        case demux::Container::Mkv:
            return Container::Mkv;
        case demux::Container::Ivf:
            return Container::Ivf;
        default:
            return Container::Unknown;
    }
}

/* Pick the container from a file name's extension (empty for unknown). */
[[nodiscard]]
inline Container container_from_extension(std::string_view path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string_view::npos) {
        return Container::Unknown;
    }

    const std::string_view ext = path.substr(dot);

    if (ext == ".mp4" || ext == ".m4v" || ext == ".mov") {
        return Container::Mp4;
    }
    if (ext == ".mkv" || ext == ".webm") {
        return Container::Mkv;
    }
    if (ext == ".ivf") {
        return Container::Ivf;
    }
    if (ext == ".wav") {
        return Container::Wav;
    }
    return Container::Unknown;
}

/*
 * -----------------------------------------------------------
 * Zero-copy track / sample views
 * -----------------------------------------------------------
 *
 * TrackConfig is the write-side mirror of demux::TrackView: it
 * describes one track to create.  Sample is the mirror of
 * demux::SampleView: it carries a span into caller memory plus the
 * timing metadata.  Just like SampleView, the span is NOT copied
 * here -- it must stay alive until Muxer::finalize() consumes it.
 */
struct TrackConfig {
    std::uint32_t id = 0;
    Codec codec = Codec::Hevc;
    NalFramingMode framing = NalFramingMode::AnnexB;

    std::string codec_name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t timescale = 0;
    std::string language;

    bool is_video = true;
    bool is_audio = false;
    bool is_subtitle = false;

    unsigned length_size = 4;

    /* Audio configuration (used by the WAV writer). */
    AudioCodec audio_codec = AudioCodec::Pcm16;
    std::uint32_t sample_rate = 44100;
    std::uint16_t channels = 2;
    std::vector<std::uint8_t> audio_config; /* AAC AudioSpecificConfig */

    /* Video codec private data (avcC / hvcC / vvcC).  When empty, the MKV
     * file writer derives an avcC from the first keyframe's SPS/PPS (or a
     * minimal hvcC/vvcC).  Streaming writers cannot inspect future samples,
     * so they rely on the caller supplying this for H.26x tracks. */
    std::vector<std::uint8_t> codec_config;

    Metadata metadata; /* per-track tags (MP4 trak/udta, MKV TrackEntry) */
};

/*
 * Provenance signature written automatically into the standard encoder tag
 * of every MP4 / MKV output.  Callers cannot override it: reserved encoder
 * keys are stripped from user metadata before the writers run.
 *
 * The value is stored XOR-encoded so no plain-text literal appears in the
 * source or the compiled binary's string table; it is decoded on first use.
 */
namespace detail {

inline constexpr std::uint8_t kEncoderObfuscated[] = {
    0x0Fu, 0x37u, 0x33u, 0x28u, 0x2Eu, 0x3Fu, 0x39u, 0x32u, 0x7Au, 0x17u,
    0x3Fu, 0x3Eu, 0x33u, 0x3Bu, 0x1Fu, 0x34u, 0x3Du, 0x33u, 0x34u, 0x3Fu
};
inline constexpr std::uint8_t kEncoderXorKey = 0x5Au;

[[nodiscard]]
inline std::string decode_encoder_signature() {
    std::string out;
    out.reserve(sizeof(kEncoderObfuscated));
    for (const std::uint8_t b : kEncoderObfuscated) {
        out.push_back(static_cast<char>(b ^ kEncoderXorKey));
    }
    return out;
}

}  // namespace detail

[[nodiscard]]
inline const std::string& encoder_signature() {
    static const std::string value = detail::decode_encoder_signature();
    return value;
}

[[nodiscard]]
inline bool ascii_iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char ac = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
        const char bc = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
        if (ac != bc) {
            return false;
        }
    }
    return true;
}

/* Recognise both the plain "encoder" and the MP4 (c)too spelling. */
[[nodiscard]]
inline bool is_reserved_encoder_key(std::string_view key) noexcept {
    return ascii_iequals(key, "encoder") || key == "\xC2\xA9" "too";
}

/*
 * Make an arbitrary caller string safe to embed in a container tag:
 * drop embedded NULs and replace malformed UTF-8 sequences with U+FFFD.
 * The result is always valid UTF-8 and NUL-free, so demux readers can use
 * std::string(ptr, len) without producing broken text.
 */
inline std::string sanitize_metadata_string(std::string_view input) {
    std::string out;
    out.reserve(input.size());

    const auto push_replacement = [&]() { out.append("\xEF\xBF\xBD"); };

    std::size_t i = 0;
    while (i < input.size()) {
        const std::uint8_t c = static_cast<std::uint8_t>(input[i]);

        if (c == 0x00u) {
            ++i;
            continue;
        }

        if (c < 0x80u) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }

        std::size_t need = 0;
        std::uint32_t cp = 0;
        std::uint32_t min_cp = 0;

        if (c >= 0xC2u && c <= 0xDFu) {
            need = 1;
            cp = c & 0x1Fu;
            min_cp = 0x80u;
        } else if (c >= 0xE0u && c <= 0xEFu) {
            need = 2;
            cp = c & 0x0Fu;
            min_cp = 0x800u;
        } else if (c >= 0xF0u && c <= 0xF4u) {
            need = 3;
            cp = c & 0x07u;
            min_cp = 0x10000u;
        } else {
            push_replacement();
            ++i;
            continue;
        }

        if (i + need >= input.size()) {
            push_replacement();
            ++i;
            continue;
        }

        bool valid = true;
        for (std::size_t k = 1; k <= need; ++k) {
            const std::uint8_t cc = static_cast<std::uint8_t>(input[i + k]);
            if ((cc & 0xC0u) != 0x80u) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }

        if (!valid || cp < min_cp || cp > 0x10FFFFu ||
            (cp >= 0xD800u && cp <= 0xDFFFu)) {
            push_replacement();
            ++i;
            continue;
        }

        out.append(input.substr(i, need + 1));
        i += need + 1;
    }

    return out;
}

/*
 * Write-side mirror of demux::SampleEntry: where one already-written sample
 * lives in the output and how it is timed.  No payload is stored — just a
 * file offset and metadata — so a multi-gigabyte stream keeps only a tiny
 * index in memory (the same design as the demuxer).
 */
struct SampleRecord {
    std::size_t offset = 0;
    std::size_t size = 0;
    std::int64_t pts = 0;
    std::int64_t dts = 0;
    std::int64_t duration = 0;
    bool is_key = true;
};

/* A run of consecutive same-track samples written to one chunk. */
struct ChunkRecord {
    std::size_t track = 0;
    std::size_t first_sample = 0;
    std::size_t sample_count = 0;
    std::uint64_t offset = 0;
};

struct Sample {
    std::uint32_t track_id = 0;
    std::size_t index = 0;

    std::span<const std::uint8_t> data{};

    std::int64_t pts = 0;
    std::int64_t dts = 0;
    std::int64_t duration = 0;
    bool is_key = true;

    NalFramingMode framing = NalFramingMode::AnnexB;

    [[nodiscard]]
    explicit operator bool() const noexcept {
        return !data.empty();
    }
};

/* ----------------------------------------------------------- */
/* Codec → container / muxer-specific identifiers              */
/* ----------------------------------------------------------- */

[[nodiscard]]
inline bool container_supports_codec(Container container, Codec codec) noexcept {
    switch (container) {
        case Container::Ivf:
            return codec == Codec::Vp8 || codec == Codec::Vp9;
        case Container::Mp4:
        case Container::Mkv:
            return true;
        case Container::Wav:
            return true; // audio container: codec lives in AudioCodec, not Codec
        default:
            return false;
    }
}

/* ISO-BMFF sample-entry fourcc for a codec. */
[[nodiscard]]
inline const char* mp4_fourcc(Codec codec) noexcept {
    switch (codec) {
        case Codec::Avc:
            return "avc1";
        case Codec::Hevc:
            return "hvc1";
        case Codec::Vvc:
            return "vvc1";
        case Codec::Av1:
            return "av01";
        case Codec::Vp9:
            return "vp09";
        case Codec::Vp8:
            return "vp08";
        case Codec::Mpeg1Video:
        case Codec::Mpeg2Video:
            return "mpgv";
        case Codec::Mpeg4Visual:
            return "mp4v";
        case Codec::H263:
            return "s263";
        case Codec::Theora:
            return "theo";
        case Codec::ProRes:
            return "apcn";
        case Codec::DnvHd:
            return "AVdn";
        case Codec::Vc1:
            return "vc-1";
        case Codec::Avs2:
            return "avs2";
        default:
            // A generic ISO visual entry instead of falsely claiming HEVC.
            return "mp4v";
    }
}

/* Matroska CodecID string for a codec. */
[[nodiscard]]
inline const char* mkv_codec_id(Codec codec) noexcept {
    switch (codec) {
        case Codec::Avc:
            return "V_MPEG4/ISO/AVC";
        case Codec::Hevc:
            return "V_MPEGH/ISO/HEVC";
        case Codec::Vvc:
            return "V_VVC";
        case Codec::Av1:
            return "V_AV1";
        case Codec::Vp9:
            return "V_VP9";
        case Codec::Vp8:
            return "V_VP8";
        case Codec::Mpeg1Video:
            return "V_MPEG1";
        case Codec::Mpeg2Video:
            return "V_MPEG2";
        case Codec::Mpeg4Visual:
            return "V_MPEG4/ISO/ASP";
        case Codec::H263:
            return "V_SORENSON";
        case Codec::Theora:
            return "V_THEORA";
        case Codec::ProRes:
            return "V_PRORES";
        case Codec::Vc1:
            return "V_VC1";
        case Codec::Avs2:
            return "V_AVS2";
        default:
            // Never mislabel an unknown codec as HEVC: players would try the
            // wrong decoder and fail with a confusing start-code error.
            return "V_UNCOMPRESSED";
    }
}

/* IVF fourcc for a codec (VP8 / VP9 / AV1). */
[[nodiscard]]
inline const char* ivf_fourcc(Codec codec) noexcept {
    switch (codec) {
        case Codec::Vp9: return "VP90";
        case Codec::Av1: return "AV01";
        default: return "VP80";
    }
}

/* Codecs that use Annex-B NAL framing on input. */
[[nodiscard]]
inline bool is_h26x(Codec codec) noexcept {
    return codec == Codec::Avc || codec == Codec::Hevc || codec == Codec::Vvc;
}

}  // namespace mux
}  // namespace bs
