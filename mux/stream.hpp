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
enum class Container : std::uint8_t { Unknown, Mp4, Mkv, Ivf };

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
    }
    return "hvc1";
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
    }
    return "V_MPEGH/ISO/HEVC";
}

/* IVF fourcc for a codec (VP8 / VP9 only). */
[[nodiscard]]
inline const char* ivf_fourcc(Codec codec) noexcept {
    return codec == Codec::Vp9 ? "VP90" : "VP80";
}

/* Codecs that use Annex-B NAL framing on input. */
[[nodiscard]]
inline bool is_h26x(Codec codec) noexcept {
    return codec == Codec::Avc || codec == Codec::Hevc || codec == Codec::Vvc;
}

}  // namespace mux
}  // namespace bs
