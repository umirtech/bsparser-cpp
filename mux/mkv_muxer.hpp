// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "mux/stream.hpp"
#include "mux/file_sink.hpp"

#include "demux/es_reconstruct.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bs {
namespace mux {
namespace detail {

/*
 * -----------------------------------------------------------
 * Matroska / WebM (EBML) muxer (limited)
 * -----------------------------------------------------------
 *
 * Writes the minimal subset that demux::build_mkv() indexes:
 *
 *   EBML header -> Segment (unknown size)
 *                    |- Info (TimecodeScale / MuxingApp / WritingApp)
 *                    |- Tracks (one TrackEntry per input track, video or audio)
 *                    |- Cluster (Timecode = 0 + one SimpleBlock per sample)
 *
 * The Segment uses the 8-byte "unknown size" vint so the element does
 * not have to be back-patched once its length is known.
 *
 * H.26x samples are emitted as Annex-B: demuxer-produced samples from
 * MP4 arrive length-prefixed, so each NAL is re-prefixed with a
 * 00 00 00 01 start code.  For conformance, an H.26x TrackEntry carries
 * CodecPrivate from the caller-supplied avcC/hvcC/vvcC.  Note that
 * build_mkv() turns avcC parameter sets into a leading synthetic sample;
 * that is expected for a conforming record.  The writer cannot see future
 * samples, so its callers should supply TrackConfig::codec_config for
 * H.26x tracks.
 */

/* EBML IDs used by the writer. */
constexpr std::uint64_t kMkvEbml = 0x1A45DFA3ull;
constexpr std::uint64_t kMkvEbmlVersion = 0x4286ull;
constexpr std::uint64_t kMkvEbmlReadVersion = 0x42F7ull;
constexpr std::uint64_t kMkvEbmlMaxIdLength = 0x42F2ull;
constexpr std::uint64_t kMkvEbmlMaxSizeLength = 0x42F3ull;
constexpr std::uint64_t kMkvDocType = 0x4282ull;
constexpr std::uint64_t kMkvDocTypeVersion = 0x4287ull;
constexpr std::uint64_t kMkvDocTypeReadVersion = 0x4285ull;

constexpr std::uint64_t kMkvSegment = 0x18538067ull;
constexpr std::uint64_t kMkvInfo = 0x1549A966ull;
constexpr std::uint64_t kMkvTimecodeScale = 0x2AD7B1ull;
constexpr std::uint64_t kMkvMuxingApp = 0x4D80ull;
constexpr std::uint64_t kMkvWritingApp = 0x5741ull;
constexpr std::uint64_t kMkvTitle = 0x7BA9ull;
constexpr std::uint64_t kMkvDateUtc = 0x4461ull;

constexpr std::uint64_t kMkvTags = 0x1254C367ull;
constexpr std::uint64_t kMkvTag = 0x7373ull;
constexpr std::uint64_t kMkvTargets = 0x63C0ull;
constexpr std::uint64_t kMkvSimpleTag = 0x67C8ull;
constexpr std::uint64_t kMkvTagName = 0x45A3ull;
constexpr std::uint64_t kMkvTagString = 0x4487ull;

constexpr std::uint64_t kMkvTracks = 0x1654AE6Bull;
constexpr std::uint64_t kMkvTrackEntry = 0xAEull;
constexpr std::uint64_t kMkvTrackNumber = 0xD7ull;
constexpr std::uint64_t kMkvTrackUid = 0x73C5ull;
constexpr std::uint64_t kMkvTrackType = 0x83ull;
constexpr std::uint64_t kMkvCodecId = 0x86ull;
constexpr std::uint64_t kMkvCodecName = 0x258688ull;
constexpr std::uint64_t kMkvLanguage = 0x22B59Cull;
constexpr std::uint64_t kMkvVideo = 0xE0ull;
constexpr std::uint64_t kMkvPixelWidth = 0xB0ull;
constexpr std::uint64_t kMkvPixelHeight = 0xBAull;

constexpr std::uint64_t kMkvAudio = 0xE1ull;
constexpr std::uint64_t kMkvSamplingFrequency = 0xB5ull;
constexpr std::uint64_t kMkvChannels = 0x9Full;
constexpr std::uint64_t kMkvBitDepth = 0x6264ull;
constexpr std::uint64_t kMkvCodecPrivate = 0x63A2ull;
constexpr std::uint64_t kMkvFlagLacing = 0x55AAull;

constexpr std::uint64_t kMkvCluster = 0x1F43B675ull;
constexpr std::uint64_t kMkvTimecode = 0xE7ull;
constexpr std::uint64_t kMkvSimpleBlock = 0xA3ull;

constexpr std::uint64_t kMkvCues = 0x1C53BB6Bull;
constexpr std::uint64_t kMkvCuePoint = 0xBBull;
constexpr std::uint64_t kMkvCueTime = 0xB3ull;
constexpr std::uint64_t kMkvCueTrackPositions = 0xB7ull;
constexpr std::uint64_t kMkvCueTrack = 0xF7ull;
constexpr std::uint64_t kMkvCueClusterPosition = 0xF1ull;

/* Append `bytes` big-endian integers, most significant first. */
inline void mkv_push_uint(
    std::vector<std::uint8_t>& out, std::uint64_t value, unsigned bytes
) {
    for (unsigned i = bytes; i > 0u; --i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8u * (i - 1u))));
    }
}

/* Append the minimal big-endian representation of an unsigned integer. */
inline void mkv_push_min_uint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    unsigned bytes = 1;
    while (bytes < 8u && value > ((std::uint64_t{1} << (8u * bytes)) - 1u)) {
        ++bytes;
    }
    mkv_push_uint(out, value, bytes);
}

/* Append an EBML ID (marker bits are already part of `id`). */
inline void mkv_push_id(std::vector<std::uint8_t>& out, std::uint64_t id) {
    unsigned bytes = 1;
    if (id > 0xFFFFFFFFFFFFFFull) {
        bytes = 8;
    } else if (id > 0xFFFFFFFFFFFFull) {
        bytes = 7;
    } else if (id > 0xFFFFFFFFFFull) {
        bytes = 6;
    } else if (id > 0xFFFFFFFFull) {
        bytes = 5;
    } else if (id > 0xFFFFFFull) {
        bytes = 4;
    } else if (id > 0xFFFFull) {
        bytes = 3;
    } else if (id > 0xFFull) {
        bytes = 2;
    }
    mkv_push_uint(out, id, bytes);
}

/* Append an EBML size as the shortest-length vint (marker bit included). */
inline void mkv_push_size(std::vector<std::uint8_t>& out, std::uint64_t size) {
    unsigned bytes = 1;
    while (bytes < 8u && size > ((std::uint64_t{1} << (7u * bytes)) - 1u)) {
        ++bytes;
    }

    const std::uint64_t marker = std::uint64_t{1} << (7u * bytes);
    mkv_push_uint(out, size | marker, bytes);
}

/* Append a complete EBML element: id + size + payload. */
inline void mkv_push_element(
    std::vector<std::uint8_t>& out, std::uint64_t id, std::span<const std::uint8_t> payload
) {
    mkv_push_id(out, id);
    mkv_push_size(out, static_cast<std::uint64_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

inline void mkv_push_element(
    std::vector<std::uint8_t>& out, std::uint64_t id, const std::vector<std::uint8_t>& payload
) {
    mkv_push_element(out, id, std::span<const std::uint8_t>(payload));
}

/* Append a vint (with length marker) -- used for SimpleBlock track numbers. */
inline void mkv_push_vint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    unsigned bytes = 1;
    while (bytes < 8u && value > ((std::uint64_t{1} << (7u * bytes)) - 1u)) {
        ++bytes;
    }

    const std::uint64_t marker = std::uint64_t{1} << (7u * bytes);
    mkv_push_uint(out, value | marker, bytes);
}

/* Append a UTF-8 string as an EBML element payload. */
inline void mkv_push_string(std::vector<std::uint8_t>& out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
}

/* 1-based track number carried by both TrackNumber and SimpleBlock. */
[[nodiscard]]
inline std::uint64_t mkv_track_number(const TrackConfig& track, std::size_t index) noexcept {
    return track.id != 0u ? static_cast<std::uint64_t>(track.id)
                          : static_cast<std::uint64_t>(index + 1u);
}

/* Matroska CodecID for an audio track. */
[[nodiscard]]
inline const char* mkv_audio_codec_id(AudioCodec codec) noexcept {
    switch (codec) {
        case AudioCodec::Aac:
            return "A_AAC";
        case AudioCodec::Pcm8:
        case AudioCodec::Pcm16:
        case AudioCodec::Pcm24:
        case AudioCodec::Pcm32:
            return "A_PCM/INT/LIT";
        case AudioCodec::PcmFloat32:
        case AudioCodec::PcmFloat64:
            return "A_PCM/FLOAT/IEEE";
        case AudioCodec::Mp3:
            return "A_MPEG/L3";
        case AudioCodec::Opus:
            return "A_OPUS";
        case AudioCodec::Vorbis:
            return "A_VORBIS";
        case AudioCodec::Ac3:
            return "A_AC3";
        case AudioCodec::Ec3:
            return "A_EAC3";
        case AudioCodec::Dts:
            return "A_DTS";
        case AudioCodec::Flac:
            return "A_FLAC";
        default:
            return "A_PCM/INT/LIT";
    }
}

/* True only for tracks whose samples need Annex-B framing in MKV. */
[[nodiscard]]
inline bool mkv_track_uses_annex_b(const TrackConfig& track) noexcept {
    if ((track.is_audio && !track.is_video) || !is_h26x(track.codec)) {
        return false;
    }
    // Matroska AVC/HEVC/VVC with an avcC/hvcC/vvcC CodecPrivate uses AVCC
    // (length-prefixed) sample data, not Annex-B start codes.
    return track.codec_config.empty();
}

/*
 * Convert a length-prefixed H.26x frame to Annex-B.  Frames that already
 * start with a start code (or cannot be parsed as a complete length-
 * prefixed stream) are copied verbatim.
 */
[[nodiscard]]
inline std::vector<std::uint8_t> mkv_to_annex_b(
    std::span<const std::uint8_t> data, unsigned length_size
) {
    std::vector<std::uint8_t> out;

    if (data.empty()) {
        return out;
    }

    if (length_size == 0u || length_size > 4u) {
        length_size = 4u;
    }

    if (demux::es::fully_length_prefixed(data, length_size)) {
        std::size_t pos = 0;
        while (pos + length_size <= data.size()) {
            std::uint32_t nal_size = 0;
            for (unsigned i = 0; i < length_size; ++i) {
                nal_size = (nal_size << 8u) | data[pos + i];
            }
            pos += length_size;

            if (nal_size == 0u || nal_size > data.size() - pos) {
                break;
            }

            demux::es::annex_b_nal(out, data.subspan(pos, nal_size));
            pos += nal_size;
        }
        return out;
    }

    out.assign(data.begin(), data.end());
    return out;
}

/* Split an Annex-B byte stream into NAL units (start codes stripped).
 * Only `d` is read; the returned spans point into it. */
[[nodiscard]]
inline std::vector<std::span<const std::uint8_t>> mkv_split_annexb(
    std::span<const std::uint8_t> d
) {
    std::vector<std::span<const std::uint8_t>> nals;
    const std::size_t n = d.size();
    if (n < 3u) {
        return nals;
    }

    std::size_t start = n;
    for (std::size_t i = 0u; i + 2u < n; ++i) {
        if (d[i] == 0u && d[i + 1u] == 0u && d[i + 2u] == 1u) {
            start = i + 3u;
            break;
        }
    }
    if (start == n) {
        return nals; // no start code: not Annex-B
    }

    while (start <= n) {
        std::size_t end = n;
        for (std::size_t i = start; i + 2u < n; ++i) {
            if (d[i] == 0u && d[i + 1u] == 0u && d[i + 2u] == 1u) {
                end = i;
                break;
            }
        }

        // Trailing zero bytes belong to the following start code.
        std::size_t e = end;
        while (e > start && d[e - 1u] == 0u) {
            --e;
        }
        if (e > start) {
            nals.emplace_back(d.data() + start, e - start);
        }

        if (end == n) {
            break;
        }
        start = end + 3u;
    }
    return nals;
}

/* Build an avcC (AVCDecoderConfigurationRecord) from the SPS/PPS NALs of
 * one Annex-B access unit.  Returns an empty vector when no usable SPS
 * is present.  Parameter-set counts and lengths are bounded so a
 * malformed input cannot inflate the record. */
[[nodiscard]]
inline std::vector<std::uint8_t> mkv_make_avcc(std::span<const std::uint8_t> annexb) {
    constexpr std::size_t kMaxSps = 31u;  // 5-bit numOfSPS field
    constexpr std::size_t kMaxPps = 255u; // 8-bit numOfPPS field

    std::vector<std::span<const std::uint8_t>> sps;
    std::vector<std::span<const std::uint8_t>> pps;
    for (const auto nal : mkv_split_annexb(annexb)) {
        if (nal.empty() || nal.size() > 0xFFFFu || nal.size() < 4u) {
            continue;
        }
        const unsigned type = static_cast<unsigned>(nal[0]) & 0x1Fu;
        if (type == 7u) {
            if (sps.size() < kMaxSps) {
                sps.push_back(nal);
            }
        } else if (type == 8u) {
            if (pps.size() < kMaxPps) {
                pps.push_back(nal);
            }
        }
    }
    if (sps.empty()) {
        return {};
    }

    std::vector<std::uint8_t> cfg;
    cfg.reserve(6u + sps.size() * 4u + pps.size() * 4u);
    cfg.push_back(1u);                        // configurationVersion
    cfg.push_back(sps.front()[1]);            // AVCProfileIndication
    cfg.push_back(sps.front()[2]);            // profile_compatibility
    cfg.push_back(sps.front()[3]);            // AVCLevelIndication
    cfg.push_back(0xFFu);                     // reserved + lengthSizeMinusOne = 3
    cfg.push_back(static_cast<std::uint8_t>(0xE0u | sps.size()));
    for (const auto nal : sps) {
        mkv_push_uint(cfg, static_cast<std::uint16_t>(nal.size()), 2u);
        cfg.insert(cfg.end(), nal.begin(), nal.end());
    }
    cfg.push_back(static_cast<std::uint8_t>(pps.size()));
    for (const auto nal : pps) {
        mkv_push_uint(cfg, static_cast<std::uint16_t>(nal.size()), 2u);
        cfg.insert(cfg.end(), nal.begin(), nal.end());
    }
    return cfg;
}

/* Minimal hvcC / vvcC placeholder: byte 18 (HEVC) / byte 14 (VVC) carries
 * lengthSizeMinusOne = 3 so the demuxer's length-size probe resolves to
 * 4-byte NAL lengths. */
[[nodiscard]]
inline std::vector<std::uint8_t> mkv_make_minimal_hvc_config(bool is_vvc) {
    std::vector<std::uint8_t> cfg(23u, std::uint8_t{0});
    cfg[0] = 1u; // configurationVersion
    cfg[is_vvc ? 14u : 18u] = 0x03u;
    return cfg;
}

/* Derive CodecPrivate for an H.26x video track from its first keyframe
 * sample.  An empty sample list yields no config (the streaming path
 * relies on a caller-supplied codec_config instead). */
[[nodiscard]]
inline std::vector<std::uint8_t> mkv_derive_codec_config(
    const TrackConfig& track, std::span<const Sample> track_samples
) {
    if ((track.is_audio && !track.is_video) || !is_h26x(track.codec)) {
        return {};
    }

    for (const Sample& sample : track_samples) {
        if (!sample.is_key || sample.data.empty()) {
            continue;
        }
        if (track.codec == Codec::Hevc || track.codec == Codec::Vvc) {
            return mkv_make_minimal_hvc_config(track.codec == Codec::Vvc);
        }
        const std::vector<std::uint8_t> annexb =
            mkv_to_annex_b(sample.data, track.length_size);
        std::vector<std::uint8_t> cfg = mkv_make_avcc(annexb);
        if (!cfg.empty()) {
            return cfg;
        }
    }
    return {};
}

/* Minimal AudioSpecificConfig for AAC-LC, used when the caller did not
 * supply one.  Mirrors mp4_muxer.hpp: 5-bit object type 2 (AAC-LC), a
 * sampling-frequency index and a channel configuration, followed by the
 * three zero GASpecificConfig flags. */
[[nodiscard]]
inline std::vector<std::uint8_t> mkv_make_default_aac_config(const TrackConfig& track) {
    static constexpr std::uint32_t kRates[] = {
        96000u, 88200u, 64000u, 48000u, 44100u, 32000u,
        24000u, 22050u, 16000u, 12000u, 11025u, 8000u, 7350u
    };

    const std::uint32_t rate = track.sample_rate != 0u ? track.sample_rate : 44100u;
    unsigned index = 4u; // 44100 Hz fallback
    for (unsigned i = 0u; i < 13u; ++i) {
        if (kRates[i] == rate) {
            index = i;
            break;
        }
    }

    unsigned channels = track.channels != 0u ? track.channels : 2u;
    if (channels > 6u) {
        channels = 2u; // >6 needs a PCE; keep the simple path
    }

    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>((2u << 3u) | (index >> 1u)));
    out.push_back(static_cast<std::uint8_t>(((index & 1u) << 7u) | (channels << 3u)));
    return out;
}

/* Build the EBML header element. */
[[nodiscard]]
inline std::vector<std::uint8_t> mkv_make_ebml_header(bool webm = false) {
    std::vector<std::uint8_t> header;

    std::vector<std::uint8_t> tmp;
    tmp.push_back(1u);
    mkv_push_element(header, kMkvEbmlVersion, tmp);

    tmp.clear();
    tmp.push_back(1u);
    mkv_push_element(header, kMkvEbmlReadVersion, tmp);

    tmp.clear();
    tmp.push_back(4u);
    mkv_push_element(header, kMkvEbmlMaxIdLength, tmp);

    tmp.clear();
    tmp.push_back(8u);
    mkv_push_element(header, kMkvEbmlMaxSizeLength, tmp);

    tmp.clear();
    mkv_push_string(tmp, webm ? "webm" : "matroska");
    mkv_push_element(header, kMkvDocType, tmp);

    tmp.clear();
    tmp.push_back(webm ? 2u : 4u);
    mkv_push_element(header, kMkvDocTypeVersion, tmp);

    tmp.clear();
    tmp.push_back(2u);
    mkv_push_element(header, kMkvDocTypeReadVersion, tmp);

    std::vector<std::uint8_t> element;
    mkv_push_element(element, kMkvEbml, header);
    return element;
}

/* Build the Info element. */
[[nodiscard]]
inline std::vector<std::uint8_t> mkv_make_info(const Metadata& metadata) {
    std::vector<std::uint8_t> info;

    std::vector<std::uint8_t> tmp;
    mkv_push_min_uint(tmp, 1'000'000u);  // 1 ms timecode scale
    mkv_push_element(info, kMkvTimecodeScale, tmp);

    tmp.clear();
    mkv_push_string(tmp, encoder_signature());
    mkv_push_element(info, kMkvMuxingApp, tmp);

    tmp.clear();
    mkv_push_string(tmp, encoder_signature());
    mkv_push_element(info, kMkvWritingApp, tmp);

    const std::string title = sanitize_metadata_string(metadata.title);
    if (!title.empty()) {
        tmp.clear();
        mkv_push_string(tmp, title);
        mkv_push_element(info, kMkvTitle, tmp);
    }

    if (metadata.creation_time != 0) {
        tmp.clear();
        constexpr std::uint64_t kMaxNs = UINT64_MAX;
        const std::uint64_t secs = static_cast<std::uint64_t>(metadata.creation_time);
        const std::uint64_t ns = (secs > kMaxNs / 1'000'000'000ULL)
                                     ? kMaxNs
                                     : secs * 1'000'000'000ULL;
        mkv_push_uint(tmp, ns, 8u);  // 8-byte big-endian unsigned nanoseconds
        mkv_push_element(info, kMkvDateUtc, tmp);
    }

    std::vector<std::uint8_t> element;
    mkv_push_element(element, kMkvInfo, info);
    return element;
}

/* Build the Segment-level Tags element from file-level metadata. */
[[nodiscard]]
inline std::vector<std::uint8_t> mkv_make_tags(const Metadata& metadata) {
    std::vector<std::uint8_t> tags_payload;

    const auto add_tag = [&tags_payload](std::string_view key, std::string_view value) {
        const std::string safe_key = sanitize_metadata_string(key);
        const std::string safe_value = sanitize_metadata_string(value);
        if (safe_value.empty()) {
            return;
        }
        std::vector<std::uint8_t> name;
        mkv_push_string(name, safe_key);
        std::vector<std::uint8_t> str;
        mkv_push_string(str, safe_value);

        std::vector<std::uint8_t> simple;
        mkv_push_element(simple, kMkvTagName, name);
        mkv_push_element(simple, kMkvTagString, str);

        std::vector<std::uint8_t> tag;
        const std::vector<std::uint8_t> empty;
        mkv_push_element(tag, kMkvTargets, empty);  // empty => segment-level
        mkv_push_element(tag, kMkvSimpleTag, simple);

        mkv_push_element(tags_payload, kMkvTag, tag);
    };

    add_tag("TITLE", metadata.title);
    add_tag("ARTIST", metadata.artist);
    add_tag("COMMENT", metadata.comment);
    add_tag("COPYRIGHT", metadata.copyright);
    add_tag("ENCODER", encoder_signature());
    for (const auto& kv : metadata.custom) {
        add_tag(kv.first, kv.second);
    }

    std::vector<std::uint8_t> element;
    if (!tags_payload.empty()) {
        mkv_push_element(element, kMkvTags, tags_payload);
    }
    return element;
}

/*
 * Build the Tracks element with one TrackEntry per track (video or audio).
 *
 * Derivation from samples is supported by the two-argument overload; the
 * single-argument overload used by the streaming writer passes no samples
 * and therefore emits only a caller-supplied codec_config.
 */
[[nodiscard]]
inline std::vector<std::uint8_t> mkv_make_tracks(
    const std::vector<TrackConfig>& tracks,
    const std::vector<std::vector<Sample>>& samples
) {
    std::vector<std::uint8_t> tracks_payload;

    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const TrackConfig& track = tracks[i];
        const std::uint64_t number = mkv_track_number(track, i);

        // Prefer the caller's config; otherwise derive one when samples are
        // available and this is an H.26x video track.
        std::vector<std::uint8_t> codec_private = track.codec_config;
        if (codec_private.empty() && !samples.empty() && i < samples.size()) {
            codec_private = mkv_derive_codec_config(track, samples[i]);
        }

        std::vector<std::uint8_t> entry;
        std::vector<std::uint8_t> tmp;

        tmp.clear();
        mkv_push_min_uint(tmp, number);
        mkv_push_element(entry, kMkvTrackNumber, tmp);

        tmp.clear();
        mkv_push_min_uint(tmp, number);
        mkv_push_element(entry, kMkvTrackUid, tmp);

        const bool is_audio = track.is_audio && !track.is_video;

        tmp.clear();
        tmp.push_back(is_audio ? 2u : 1u); // 1 = video, 2 = audio
        mkv_push_element(entry, kMkvTrackType, tmp);

        tmp.clear();
        const char* const codec_id =
            is_audio ? mkv_audio_codec_id(track.audio_codec) : mkv_codec_id(track.codec);
        mkv_push_string(tmp, codec_id);
        mkv_push_element(entry, kMkvCodecId, tmp);

        // Sanitize first, then keep only a well-formed 3-letter language code;
        // anything else falls back to "und".
        std::string language = sanitize_metadata_string(track.language);
        const auto is_ascii_alpha = [](unsigned char c) noexcept {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        };
        if (language.size() != 3u ||
            !std::all_of(language.begin(), language.end(), [&](char c) {
                return is_ascii_alpha(static_cast<unsigned char>(c));
            })) {
            language = "und";
        }
        tmp.clear();
        mkv_push_string(tmp, language);
        mkv_push_element(entry, kMkvLanguage, tmp);

        // Matroska requires FlagLacing; 0 disables lacing (each Block holds
        // one frame, which is what SimpleBlock already produces).
        tmp.clear();
        tmp.push_back(0u);
        mkv_push_element(entry, kMkvFlagLacing, tmp);

        const std::string codec_name = sanitize_metadata_string(track.metadata.title);
        if (!codec_name.empty()) {
            tmp.clear();
            mkv_push_string(tmp, codec_name);
            mkv_push_element(entry, kMkvCodecName, tmp);
        }

        if (is_audio) {
            // Audio: SamplingFrequency (float64) + Channels + BitDepth.  AAC
            // additionally carries its AudioSpecificConfig in CodecPrivate.
            std::vector<std::uint8_t> audio;
            const double rate = static_cast<double>(
                track.sample_rate != 0u ? track.sample_rate : 44100u
            );
            tmp.clear();
            mkv_push_uint(tmp, std::bit_cast<std::uint64_t>(rate), 8u);
            mkv_push_element(audio, kMkvSamplingFrequency, tmp);

            tmp.clear();
            mkv_push_min_uint(tmp, track.channels != 0u ? track.channels : 2u);
            mkv_push_element(audio, kMkvChannels, tmp);

            tmp.clear();
            tmp.push_back(static_cast<std::uint8_t>(audio_bits_per_sample(track.audio_codec)));
            mkv_push_element(audio, kMkvBitDepth, tmp);

            mkv_push_element(entry, kMkvAudio, audio);

            if (track.audio_codec == AudioCodec::Aac) {
                std::vector<std::uint8_t> asc = track.audio_config;
                if (asc.empty()) {
                    asc = mkv_make_default_aac_config(track);
                }
                mkv_push_element(entry, kMkvCodecPrivate, asc);
            }
        } else {
            // Video: always emit numeric PixelWidth / PixelHeight so build_mkv
            // sees the fields. Use a small placeholder when the config has none.
            std::uint32_t width = track.width;
            std::uint32_t height = track.height;
            if (width == 0u) {
                width = 16u;
            }
            if (height == 0u) {
                height = 16u;
            }
            if (width > 0xFFFFu) {
                width = 0xFFFFu;
            }
            if (height > 0xFFFFu) {
                height = 0xFFFFu;
            }

            std::vector<std::uint8_t> video;
            tmp.clear();
            mkv_push_uint(tmp, width, 2u);
            mkv_push_element(video, kMkvPixelWidth, tmp);

            tmp.clear();
            mkv_push_uint(tmp, height, 2u);
            mkv_push_element(video, kMkvPixelHeight, tmp);

            mkv_push_element(entry, kMkvVideo, video);

            // H.26x video needs CodecPrivate (avcC / hvcC / vvcC) for
            // conformance.  Other codecs carry their config in-band.
            if (is_h26x(track.codec) && !codec_private.empty()) {
                mkv_push_element(entry, kMkvCodecPrivate, codec_private);
            }
        }

        mkv_push_element(tracks_payload, kMkvTrackEntry, entry);
    }

    std::vector<std::uint8_t> element;
    mkv_push_element(element, kMkvTracks, tracks_payload);
    return element;
}

/* Streaming / no-sample callers: no derivation is attempted. */
[[nodiscard]]
inline std::vector<std::uint8_t> mkv_make_tracks(const std::vector<TrackConfig>& tracks) {
    static const std::vector<std::vector<Sample>> kNoSamples;
    return mkv_make_tracks(tracks, kNoSamples);
}

/*
 * Build the Segment-level Cues element (seek index).
 *
 * The writer emits a single Cluster, so every cue point refers to the same
 * cluster offset.  `clusters_base_offset` is the position of that Cluster
 * relative to the start of the Segment's data (i.e. after the Segment ID
 * and its size vint), which is what CueClusterPosition expects.  Callers
 * that cannot determine it may pass 0: that is a legal value, just not a
 * useful seek target.
 *
 * CueTime is sample.pts; TimecodeScale is 1 ms, so the value is in ms.
 * CueTrack is hard-coded to 1 (the primary/video track).  Empty samples
 * are skipped to stay in sync with the streaming sample appender, which
 * drops them.
 * Returns an empty buffer when there is no keyframe, so the caller can
 * simply omit Cues in that case.
 */
[[nodiscard]]
inline std::vector<std::uint8_t> mkv_make_cues(
    const std::vector<Sample>& samples, std::uint64_t clusters_base_offset
) {
    std::vector<std::uint8_t> cues_payload;

    for (const Sample& sample : samples) {
        if (!sample.is_key || sample.data.empty()) {
            continue;
        }

        std::vector<std::uint8_t> positions;
        std::vector<std::uint8_t> tmp;

        tmp.clear();
        mkv_push_min_uint(tmp, 1u);  // CueTrack = 1 (primary/video track)
        mkv_push_element(positions, kMkvCueTrack, tmp);

        tmp.clear();
        mkv_push_min_uint(tmp, clusters_base_offset);
        mkv_push_element(positions, kMkvCueClusterPosition, tmp);

        std::vector<std::uint8_t> cue_point;
        tmp.clear();
        mkv_push_min_uint(tmp, static_cast<std::uint64_t>(sample.pts));
        mkv_push_element(cue_point, kMkvCueTime, tmp);
        mkv_push_element(cue_point, kMkvCueTrackPositions, positions);

        mkv_push_element(cues_payload, kMkvCuePoint, cue_point);
    }

    std::vector<std::uint8_t> element;
    if (!cues_payload.empty()) {
        mkv_push_element(element, kMkvCues, cues_payload);
    }
    return element;
}

/* ------------------------------------------------------------------ */
/* Streaming MKV writer                                                */
/* ------------------------------------------------------------------ */

/*
 * These three entry points mirror the MP4 streaming design: the EBML
 * header, Segment header, Info and Tracks are emitted once at start,
 * already-framed SimpleBlocks are appended as samples arrive (so memory
 * stays bounded), and the file structure is closed at finish.
 *
 * The Segment carries the 8-byte unknown-size vint, so its size never
 * needs patching.  The single Cluster, however, is written with a fixed
 * 8-byte size slot that is back-patched in finish_mkv_stream() once the
 * number of payload bytes is known.
 */

/* Start a streamed file: EBML header, Segment (unknown size), Info and
 * Tracks, followed by the one Cluster's ID + fixed size slot + Timecode.
 * Returns the absolute file offset of the start of the Segment's data
 * (right after the Segment ID and size vint); CueClusterPosition is
 * expressed relative to that base. */
inline std::uint64_t start_mkv_stream(FileSink& sink,
                                      const Metadata& metadata,
                                      const std::vector<TrackConfig>& tracks) {
    // EBML header.
    const std::vector<std::uint8_t> header = mkv_make_ebml_header();
    sink.write(header);

    // Segment ID + 8-byte unknown-size vint: 0x01 FF FF FF FF FF FF FF.
    std::vector<std::uint8_t> segment_header;
    mkv_push_id(segment_header, kMkvSegment);
    segment_header.push_back(0x01u);
    for (unsigned i = 0; i < 7u; ++i) {
        segment_header.push_back(0xFFu);
    }
    sink.write(segment_header);

    // First byte of the Segment's payload -- the CueClusterPosition base.
    const std::uint64_t segment_data_offset = static_cast<std::uint64_t>(sink.size());

    const std::vector<std::uint8_t> info = mkv_make_info(metadata);
    sink.write(info);
    // No samples are available yet, so CodecPrivate can only come from a
    // caller-supplied TrackConfig::codec_config.  H.26x streaming callers
    // should fill it in (avcC / hvcC / vvcC) before starting the stream.
    const std::vector<std::uint8_t> tracks_element = mkv_make_tracks(tracks);
    sink.write(tracks_element);

    // One Cluster: ID + fixed 8-byte size slot (unknown-size placeholder,
    // patched at finish) + the mandatory Timecode = 0 child.  Using the
    // 8-byte slot keeps the patch in place regardless of the final length.
    std::vector<std::uint8_t> cluster_header;
    mkv_push_id(cluster_header, kMkvCluster);
    cluster_header.push_back(0x01u);
    for (unsigned i = 0; i < 7u; ++i) {
        cluster_header.push_back(0xFFu);
    }
    std::vector<std::uint8_t> timecode;
    timecode.push_back(0u);
    mkv_push_element(cluster_header, kMkvTimecode, timecode);
    sink.write(cluster_header);

    return segment_data_offset;
}

/* Append one already-framed sample as a SimpleBlock inside the single
 * Cluster. */
inline void append_mkv_stream_sample(FileSink& sink,
                                     const TrackConfig& track,
                                     std::uint32_t track_index,
                                     const Sample& sample) {
    if (sample.data.empty()) {
        return;
    }

    const std::uint64_t number =
        mkv_track_number(track, static_cast<std::size_t>(track_index));

    std::vector<std::uint8_t> block;

    // Vint track number, 2-byte signed relative timestamp, flags.
    mkv_push_vint(block, number);

    const std::int64_t rel = sample.pts;  // cluster timecode is 0
    const std::int64_t lo = static_cast<std::int64_t>(
        std::numeric_limits<std::int16_t>::min()
    );
    const std::int64_t hi = static_cast<std::int64_t>(
        std::numeric_limits<std::int16_t>::max()
    );
    const std::int16_t timestamp = static_cast<std::int16_t>(
        std::clamp(rel, lo, hi)
    );
    mkv_push_uint(block, static_cast<std::uint16_t>(timestamp), 2u);

    block.push_back(sample.is_key ? 0x80u : 0x00u);

    // H.26x frames must reach build_mkv() as Annex-B; other payloads are
    // copied raw.
    if (mkv_track_uses_annex_b(track)) {
        const std::vector<std::uint8_t> annex_b =
            mkv_to_annex_b(sample.data, track.length_size);
        block.insert(block.end(), annex_b.begin(), annex_b.end());
    } else {
        block.insert(block.end(), sample.data.begin(), sample.data.end());
    }

    std::vector<std::uint8_t> element;
    mkv_push_element(element, kMkvSimpleBlock, block);
    sink.write(element);
}

/* Finish a stream: back-patch the Cluster size, append Tags and Cues,
 * then return success.  The Segment uses an unknown-size vint, so it
 * needs no size patch. */
inline bool finish_mkv_stream(FileSink& sink,
                              const Metadata& metadata,
                              const std::vector<TrackConfig>& tracks,
                              std::uint64_t segment_data_offset,
                              const std::vector<Sample>& keyframe_samples) {
    // Recompute the sizes of Info + Tracks to locate the Cluster header;
    // the same metadata/tracks were used by start_mkv_stream().  Use the
    // same (sample-less) Tracks overload so the size matches exactly.
    const std::vector<std::uint8_t> info = mkv_make_info(metadata);
    const std::vector<std::uint8_t> tracks_element = mkv_make_tracks(tracks);
    const std::uint64_t cluster_header_offset =
        segment_data_offset + static_cast<std::uint64_t>(info.size())
        + static_cast<std::uint64_t>(tracks_element.size());

    // Cluster ID is 4 bytes; the 8-byte size slot follows it; the payload
    // (Timecode + SimpleBlocks) starts after both.
    const std::size_t size_slot_offset =
        static_cast<std::size_t>(cluster_header_offset) + 4u;
    const std::uint64_t cluster_payload_offset = cluster_header_offset + 4u + 8u;
    const std::uint64_t cluster_size =
        static_cast<std::uint64_t>(sink.size()) - cluster_payload_offset;

    // Fixed 8-byte vint: marker 0x01 followed by 7 big-endian value bytes.
    std::vector<std::uint8_t> patch;
    patch.reserve(8u);
    patch.push_back(0x01u);
    mkv_push_uint(patch, cluster_size, 7u);
    if (!sink.patch(size_slot_offset, patch)) {
        return false;
    }

    // Tags (segment-level) before the seek index.
    const std::vector<std::uint8_t> tags = mkv_make_tags(metadata);
    if (!tags.empty()) {
        sink.write(tags);
    }

    // Cues: CueClusterPosition is relative to the start of Segment data.
    const std::uint64_t clusters_base_offset =
        cluster_header_offset - segment_data_offset;
    const std::vector<std::uint8_t> cues =
        mkv_make_cues(keyframe_samples, clusters_base_offset);
    if (!cues.empty()) {
        sink.write(cues);
    }

    // The Segment's unknown-size vint needs no back-patch here.
    return true;
}

}  // namespace detail
}  // namespace mux
}  // namespace bs
