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
 * 00 00 00 01 start code.  CodecPrivate is intentionally omitted:
 * build_mkv() interprets it as an avcC/hvcC config (and would emit a
 * synthetic parameter-set sample), while leaving it out simply keeps
 * the framing length_size at its 4-byte fallback and preserves the
 * exact sample count.
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
constexpr std::uint64_t kMkvCodecPrivate = 0x63A2ull;

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
    }
    return "A_PCM/INT/LIT";
}

/* True only for tracks whose samples need Annex-B framing in MKV. */
[[nodiscard]]
inline bool mkv_track_uses_annex_b(const TrackConfig& track) noexcept {
    return !(track.is_audio && !track.is_video) && is_h26x(track.codec);
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

/* Build the EBML header element. */
[[nodiscard]]
inline std::vector<std::uint8_t> mkv_make_ebml_header() {
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
    mkv_push_string(tmp, "matroska");
    mkv_push_element(header, kMkvDocType, tmp);

    tmp.clear();
    tmp.push_back(4u);
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

/* Build the Tracks element with one TrackEntry per track (video or audio). */
[[nodiscard]]
inline std::vector<std::uint8_t> mkv_make_tracks(const std::vector<TrackConfig>& tracks) {
    std::vector<std::uint8_t> tracks_payload;

    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const TrackConfig& track = tracks[i];
        const std::uint64_t number = mkv_track_number(track, i);

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

        const std::string codec_name = sanitize_metadata_string(track.metadata.title);
        if (!codec_name.empty()) {
            tmp.clear();
            mkv_push_string(tmp, codec_name);
            mkv_push_element(entry, kMkvCodecName, tmp);
        }

        if (is_audio) {
            // Audio: SamplingFrequency (float64) + Channels.  AAC additionally
            // carries its AudioSpecificConfig in CodecPrivate.
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

            mkv_push_element(entry, kMkvAudio, audio);

            if (track.audio_codec == AudioCodec::Aac && !track.audio_config.empty()) {
                mkv_push_element(entry, kMkvCodecPrivate, track.audio_config);
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

            // CodecPrivate is intentionally omitted for video (see file comment).
        }

        mkv_push_element(tracks_payload, kMkvTrackEntry, entry);
    }

    std::vector<std::uint8_t> element;
    mkv_push_element(element, kMkvTracks, tracks_payload);
    return element;
}

/* Build a Cluster holding one SimpleBlock per (non-empty) sample. */
[[nodiscard]]
inline std::vector<std::uint8_t> mkv_make_cluster(
    const std::vector<TrackConfig>& tracks,
    const std::vector<std::vector<Sample>>& samples
) {
    std::vector<std::uint8_t> cluster;

    std::vector<std::uint8_t> tmp;
    tmp.push_back(0u);  // cluster timecode = 0
    mkv_push_element(cluster, kMkvTimecode, tmp);

    for (std::size_t i = 0; i < tracks.size() && i < samples.size(); ++i) {
        const TrackConfig& track = tracks[i];
        const std::uint64_t number = mkv_track_number(track, i);

        for (const Sample& sample : samples[i]) {
            if (sample.data.empty()) {
                continue;
            }

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

            // H.26x frames must reach build_mkv() as Annex-B; audio payloads
            // (whose Codec field is often left at its default) are copied raw.
            if (mkv_track_uses_annex_b(track)) {
                const std::vector<std::uint8_t> annex_b =
                    mkv_to_annex_b(sample.data, track.length_size);
                block.insert(block.end(), annex_b.begin(), annex_b.end());
            } else {
                block.insert(block.end(), sample.data.begin(), sample.data.end());
            }

            mkv_push_element(cluster, kMkvSimpleBlock, block);
        }
    }

    std::vector<std::uint8_t> element;
    mkv_push_element(element, kMkvCluster, cluster);
    return element;
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
 * are skipped to stay in sync with mkv_make_cluster(), which drops them.
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

/*
 * Write a Matroska file that round-trips through demux::build_mkv().
 * Returns false for an empty track list or when no track uses a codec
 * the MKV container supports.
 *
 * `metadata` carries file-level tags / creation time; each TrackConfig may
 * additionally carry per-track language / title in its own `metadata`.
 */
inline bool write_mkv(FileSink& sink,
                      const Metadata& metadata,
                      const std::vector<TrackConfig>& tracks,
                      const std::vector<std::vector<Sample>>& samples) {
    if (tracks.empty()) {
        return false;
    }

    bool any_supported = false;
    for (const TrackConfig& track : tracks) {
        if (container_supports_codec(Container::Mkv, track.codec)) {
            any_supported = true;
            break;
        }
    }
    if (!any_supported) {
        return false;
    }

    std::vector<std::uint8_t> segment;
    const std::vector<std::uint8_t> info = mkv_make_info(metadata);
    const std::vector<std::uint8_t> tracks_element = mkv_make_tracks(tracks);
    const std::vector<std::uint8_t> cluster = mkv_make_cluster(tracks, samples);

    segment.insert(segment.end(), info.begin(), info.end());
    segment.insert(segment.end(), tracks_element.begin(), tracks_element.end());
    const std::vector<std::uint8_t> tags = mkv_make_tags(metadata);
    if (!tags.empty()) {
        segment.insert(segment.end(), tags.begin(), tags.end());
    }
    // The Cluster begins at `segment`'s current size, measured from the start
    // of the Segment data -- the offset base used by CueClusterPosition.
    const std::uint64_t clusters_base_offset =
        static_cast<std::uint64_t>(segment.size());
    segment.insert(segment.end(), cluster.begin(), cluster.end());

    // Seek index: append Cues after the Cluster. The Segment carries an
    // unknown size, so growing its payload needs no header back-patch.
    // No keyframe -> no Cues element at all.
    if (!samples.empty()) {
        const std::vector<std::uint8_t> cues =
            mkv_make_cues(samples.front(), clusters_base_offset);
        if (!cues.empty()) {
            segment.insert(segment.end(), cues.begin(), cues.end());
        }
    }

    std::vector<std::uint8_t> file;
    const std::vector<std::uint8_t> header = mkv_make_ebml_header();
    file.insert(file.end(), header.begin(), header.end());

    // Segment with the 8-byte unknown-size vint: 0x01 FF FF FF FF FF FF FF.
    mkv_push_id(file, kMkvSegment);
    file.push_back(0x01u);
    for (unsigned i = 0; i < 7u; ++i) {
        file.push_back(0xFFu);
    }
    file.insert(file.end(), segment.begin(), segment.end());

    sink.write(file);

    return true;
}

/* Backward-compatible overload: no metadata. */
inline bool write_mkv(FileSink& sink,
                      const std::vector<TrackConfig>& tracks,
                      const std::vector<std::vector<Sample>>& samples) {
    static const Metadata kEmptyMetadata;
    return write_mkv(sink, kEmptyMetadata, tracks, samples);
}

}  // namespace detail
}  // namespace mux
}  // namespace bs
