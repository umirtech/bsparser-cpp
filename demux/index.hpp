// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "file_source.hpp"
#include "stream.hpp"

#include "mp4_demuxer.hpp"
#include "mkv_demuxer.hpp"
#include "ts_demuxer.hpp"
#include "flv_demuxer.hpp"
#include "avi_demuxer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bs {
namespace demux {
namespace detail {

/*
 * Result of indexing a container: track metadata plus one sample
 * index per track.  Sample payloads are not copied; SampleEntry
 * stores file offsets and sizes.
 */
struct IndexBuildResult {
    std::vector<TrackView> tracks;
    std::vector<std::vector<SampleEntry>> samples;

    /*
     * Small parameter-set blobs extracted from codec-config boxes.
     * They are copied once (they are tiny) so the parser can consume
     * them without reconstructing an Annex-B stream.
     */
    std::vector<std::vector<std::uint8_t>> param_sets;

    /* File-level tags (MP4 udta/meta/ilst, MKV Info + Tags). */
    ContainerMetadata metadata;
};

inline Codec codec_from_fourcc(const std::string& f) {
    if (f == "avc1" || f == "AVC1" || f == "avc3") return Codec::Avc;
    if (f == "hvc1" || f == "hev1" || f == "HEVC") return Codec::Hevc;
    if (f == "vvc1" || f == "vvi1" || f == "VVC1") return Codec::Vvc;
    if (f == "av01" || f == "AV01" || f == "av1c") return Codec::Av1;
    if (f == "vp09" || f == "VP09" || f == "vp9 ") return Codec::Vp9;
    if (f == "vp08" || f == "VP08" || f == "vp8 ") return Codec::Vp8;
    return Codec::Hevc;
}

inline NalFramingMode framing_for_codec(Codec codec) {
    if (codec == Codec::Av1) return NalFramingMode::Obu;
    if (codec == Codec::Vp9 || codec == Codec::Vp8) return NalFramingMode::RawFrame;
    return NalFramingMode::LengthPrefixed;
}

/* ------------------------------------------------------------------ */
/* MP4 / ISO-BMFF                                                     */
/* ------------------------------------------------------------------ */

namespace mp4_index {

using mp4::detail::Box;
using mp4::detail::box_at;
using mp4::detail::find_child;
using mp4::detail::is_fourcc;
using mp4::detail::read_u32;
using mp4::detail::read_u64;
using mp4::detail::VideoEntry;
using mp4::detail::AudioEntry;

/*
 * -----------------------------------------------------------
 * MP4 metadata (udta/meta/ilst + mdhd language / creation time)
 * -----------------------------------------------------------
 *
 * The parser mirrors the layout written by the mux writers. Every
 * offset is bounds-checked and malformed boxes are skipped; nothing
 * here throws.
 */

/* MP4 uses 1904-01-01 as its epoch; Unix time uses 1970-01-01. */
inline constexpr std::uint32_t kMp4UnixEpoch = 2082844800u;

inline std::int64_t mp4_creation_time(std::span<const std::uint8_t> data, std::size_t box_pos) {
    const Box b = box_at(data, box_pos);
    if (b.data + 8 > b.end) {
        return 0;
    }

    const std::uint8_t version = data[b.data];

    if (version == 0) {
        const std::uint32_t raw = read_u32(data, b.data + 4);
        if (raw > kMp4UnixEpoch) {
            return static_cast<std::int64_t>(raw - kMp4UnixEpoch);
        }
    } else if (version == 1 && b.data + 12 <= b.end) {
        const std::uint64_t raw = read_u64(data, b.data + 4);
        if (raw > kMp4UnixEpoch) {
            return static_cast<std::int64_t>(raw - kMp4UnixEpoch);
        }
    }

    return 0;
}

/*
 * Decode the ISO-639-2/T 3x5-bit language field of an mdhd box.
 * Returns an empty string for unset / implausible values so callers
 * can leave an existing language untouched.
 */
inline std::string mp4_language(std::span<const std::uint8_t> data, std::size_t box_pos) {
    const Box b = box_at(data, box_pos);
    if (b.data >= b.end) {
        return {};
    }

    const std::uint8_t version = data[b.data];
    const std::size_t off = (version == 1) ? b.data + 32 : b.data + 20;
    if (off + 2 > b.end) {
        return {};
    }

    const std::uint16_t v = static_cast<std::uint16_t>((data[off] << 8) | data[off + 1]);
    const int c0 = static_cast<int>((v >> 10) & 0x1Fu) + 'a' - 1;
    const int c1 = static_cast<int>((v >> 5) & 0x1Fu) + 'a' - 1;
    const int c2 = static_cast<int>(v & 0x1Fu) + 'a' - 1;

    if (c0 < 'a' || c0 > 'z' || c1 < 'a' || c1 > 'z' || c2 < 'a' || c2 > 'z') {
        return {};
    }

    return std::string{static_cast<char>(c0), static_cast<char>(c1), static_cast<char>(c2)};
}

/*
 * Parse an Apple `meta` FullBox. Children start after the 4-byte
 * version/flags word. `keys` (mdta entries) supplies the names for
 * numbered custom `ilst` items; `hdlr` is ignored.
 */
inline void parse_meta_box(
    std::span<const std::uint8_t> data, std::size_t meta_pos, ContainerMetadata& out
) {
    const Box meta = box_at(data, meta_pos);
    if (meta.data + 4 > meta.end) {
        return;
    }

    std::vector<std::string> keys;
    std::size_t ilst_pos = data.size();

    std::size_t p = meta.data + 4;
    while (p + 8 <= meta.end) {
        const Box child = box_at(data, p);
        if (child.end <= p || child.end > meta.end) {
            break;
        }

        if (is_fourcc(data, p + 4, "keys")) {
            /* keys: FullBox, u32 entry_count, then `mdta` boxes. */
            if (child.data + 8 <= child.end) {
                const std::uint32_t count = read_u32(data, child.data + 4);
                std::size_t q = child.data + 8;
                for (std::uint32_t i = 0; i < count && q + 8 <= child.end; ++i) {
                    const Box e = box_at(data, q);
                    if (e.end <= q || e.end > child.end) {
                        break;
                    }
                    if (is_fourcc(data, q + 4, "mdta") && e.data + 4 <= e.end) {
                        /* Cap metadata strings/counts: a tiny file must not be
                         * able to force unbounded allocations. */
                        constexpr std::size_t kMaxTagLen = 64u * 1024u;
                        constexpr std::size_t kMaxTags = 4096u;
                        if (keys.size() >= kMaxTags) {
                            break;
                        }
                        const std::size_t key_len = std::min<std::size_t>(
                            e.end - e.data - 4, kMaxTagLen
                        );
                        std::string key(
                            reinterpret_cast<const char*>(data.data() + e.data + 4), key_len
                        );
                        while (!key.empty() && key.back() == '\0') {
                            key.pop_back();
                        }
                        if (!key.empty()) {
                            keys.push_back(std::move(key));
                        }
                    }
                    q = e.end;
                }
            }
        } else if (is_fourcc(data, p + 4, "ilst")) {
            ilst_pos = p;
        }

        p = child.end;
    }

    if (ilst_pos == data.size()) {
        return;
    }

    const Box ilst = box_at(data, ilst_pos);
    std::size_t q = ilst.data;
    while (q + 8 <= ilst.end) {
        const Box item = box_at(data, q);
        if (item.end <= q || item.end > ilst.end) {
            break;
        }

        const std::uint32_t type = read_u32(data, q + 4);
        std::string custom_key;
        std::string* dst = nullptr;

        switch (type) {
            case 0xA96E616Du: /* (c)nam */
                dst = &out.title;
                break;
            case 0xA9415254u: /* (c)ART */
                dst = &out.artist;
                break;
            case 0xA9636D74u: /* (c)cmt */
                dst = &out.comment;
                break;
            case 0xA9637079u: /* (c)cpy */
                dst = &out.copyright;
                break;
            default:
                if (!keys.empty() && type >= 1u && type <= keys.size()) {
                    custom_key = keys[type - 1u];
                }
                break;
        }

        const std::size_t data_pos = find_child(data, item.data, item.end, "data");
        if (data_pos != data.size()) {
            const Box db = box_at(data, data_pos);
            if (db.data + 8 <= db.end) {
                /* data: FullBox version/flags + u32 locale + UTF-8 value. */
                constexpr std::size_t kMaxTagLen = 64u * 1024u;
                constexpr std::size_t kMaxTags = 4096u;
                const std::size_t vpos = db.data + 8;
                const std::size_t value_len = std::min<std::size_t>(db.end - vpos, kMaxTagLen);
                std::string value(
                    reinterpret_cast<const char*>(data.data() + vpos), value_len
                );
                if (dst != nullptr) {
                    *dst = std::move(value);
                } else if (!custom_key.empty() && out.custom.size() < kMaxTags) {
                    out.custom.emplace_back(std::move(custom_key), std::move(value));
                }
            }
        }

        q = item.end;
    }
}

inline std::vector<std::size_t> top_level_boxes(
    std::span<const std::uint8_t> data, const char* type
) {
    std::vector<std::size_t> out;
    std::size_t p = 0;

    while (p + 8 <= data.size()) {
        Box b = box_at(data, p);

        if (b.end <= p) {
            break;
        }

        if (p + 4 <= b.end && std::memcmp(data.data() + p + 4, type, 4) == 0) {
            out.push_back(p);
        }

        p = b.end;
    }

    return out;
}

inline std::uint32_t read_track_id(std::span<const std::uint8_t> data, Box b) {
    if (b.data + 4 > b.end) {
        return 0;
    }

    const std::uint8_t version = data[b.data];
    const std::size_t off = (version == 1) ? b.data + 20 : b.data + 12;

    if (off + 4 > b.end) {
        return 0;
    }

    return read_u32(data, off);
}

inline std::uint32_t read_mdhd_timescale(std::span<const std::uint8_t> data, Box b) {
    if (b.data + 4 > b.end) {
        return 0;
    }

    const std::uint8_t version = data[b.data];
    const std::size_t off = (version == 1) ? b.data + 20 : b.data + 12;

    if (off + 4 > b.end) {
        return 0;
    }

    return read_u32(data, off);
}

struct ParsedTrack {
    TrackView view;
    VideoEntry entry;
    AudioEntry audio_entry;
    std::size_t stsz = 0;
    std::size_t stsc = 0;
    std::size_t stco = 0;
    std::size_t co64 = 0;
    std::size_t stts = 0;
    std::size_t ctts = 0;
    std::size_t stss = 0;
    bool video = false;
    bool audio = false;
    std::uint32_t timescale = 0;
};

inline void parse_moov_tracks(
    std::span<const std::uint8_t> data, std::size_t moov, std::vector<ParsedTrack>& out
) {
    Box moov_box = box_at(data, moov);
    std::size_t trak = find_child(data, moov_box.data, moov_box.end, "trak");

    while (trak != data.size()) {
        Box trak_box = box_at(data, trak);
        ParsedTrack pt;

        const std::size_t tkhd = find_child(data, trak_box.data, trak_box.end, "tkhd");
        if (tkhd != data.size()) {
            pt.view.id = read_track_id(data, box_at(data, tkhd));
        }

        const std::size_t mdia = find_child(data, trak_box.data, trak_box.end, "mdia");

        if (mdia != data.size()) {
            Box mdia_box = box_at(data, mdia);

            const std::size_t hdlr = find_child(data, mdia_box.data, mdia_box.end, "hdlr");
            std::string handler;
            if (hdlr != data.size()) {
                Box b = box_at(data, hdlr);
                if (b.data + 12 <= b.end) {
                    handler = std::string(reinterpret_cast<const char*>(data.data() + b.data + 8), 4);
                }
            }

            const std::size_t mdhd = find_child(data, mdia_box.data, mdia_box.end, "mdhd");
            if (mdhd != data.size()) {
                pt.timescale = read_mdhd_timescale(data, box_at(data, mdhd));
                pt.view.timescale = pt.timescale;
            }

            const std::size_t minf = find_child(data, mdia_box.data, mdia_box.end, "minf");
            if (minf != data.size()) {
                Box minf_box = box_at(data, minf);
                const std::size_t stbl = find_child(data, minf_box.data, minf_box.end, "stbl");
                if (stbl != data.size()) {
                    Box stbl_box = box_at(data, stbl);
                    const std::size_t stsd =
                        find_child(data, stbl_box.data, stbl_box.end, "stsd");
                    if (stsd != data.size()) {
                        pt.entry = mp4::detail::parse_stsd(data, stsd);
                        pt.audio_entry = mp4::detail::parse_stsd_audio(data, stsd);
                    }
                    pt.stsz = find_child(data, stbl_box.data, stbl_box.end, "stsz");
                    pt.stsc = find_child(data, stbl_box.data, stbl_box.end, "stsc");
                    pt.stco = find_child(data, stbl_box.data, stbl_box.end, "stco");
                    pt.co64 = find_child(data, stbl_box.data, stbl_box.end, "co64");
                    pt.stts = find_child(data, stbl_box.data, stbl_box.end, "stts");
                    pt.ctts = find_child(data, stbl_box.data, stbl_box.end, "ctts");
                    pt.stss = find_child(data, stbl_box.data, stbl_box.end, "stss");
                }
            }

            pt.video = (handler == "vide");
            pt.audio = (handler == "soun");

            if (pt.video && pt.entry.found) {
                pt.view.is_video = true;
                pt.view.codec_name = pt.entry.fourcc;
                pt.view.width = pt.entry.width;
                pt.view.height = pt.entry.height;
                pt.view.codec = codec_from_fourcc(pt.entry.fourcc);

                if (pt.view.codec == Codec::Av1) {
                    pt.view.framing = NalFramingMode::Obu;
                } else if (pt.view.codec == Codec::Vp9 || pt.view.codec == Codec::Vp8) {
                    pt.view.framing = NalFramingMode::RawFrame;
                } else {
                    pt.view.framing = NalFramingMode::LengthPrefixed;
                }

                const std::size_t lso =
                    (pt.entry.fourcc == "hvc1" || pt.entry.fourcc == "hev1")
                        ? 18u
                        : (pt.entry.fourcc == "vvc1" ? 14u : 4u);

                if (pt.entry.config_size > lso) {
                    pt.view.length_size =
                        static_cast<unsigned>(data[pt.entry.config_offset + lso] & 3u) + 1u;
                }
            } else if (pt.audio) {
                pt.view.is_audio = true;
                pt.view.framing = NalFramingMode::RawFrame;
                if (pt.audio_entry.found) {
                    pt.view.audio_codec = pt.audio_entry.codec;
                    pt.view.codec_name = pt.audio_entry.fourcc;
                    pt.view.sample_rate = pt.audio_entry.sample_rate;
                    pt.view.channels = pt.audio_entry.channels;
                    pt.view.bits_per_sample = pt.audio_entry.bits_per_sample;
                    if (pt.audio_entry.config_size != 0u &&
                        pt.audio_entry.config_offset <= data.size() &&
                        pt.audio_entry.config_size <= data.size() - pt.audio_entry.config_offset) {
                        pt.view.audio_config.assign(
                            data.begin() + static_cast<std::ptrdiff_t>(pt.audio_entry.config_offset),
                            data.begin() + static_cast<std::ptrdiff_t>(
                                pt.audio_entry.config_offset + pt.audio_entry.config_size)
                        );
                    }
                }
            }
        }

        // Skip non-media tracks (hint, text metadata, timed metadata, etc.)
        // that have neither a usable video nor audio sample entry.
        if (pt.video || pt.audio) {
            out.push_back(std::move(pt));
        }

        trak = find_child(data, trak_box.end, moov_box.end, "trak");
    }
}

inline std::vector<std::uint8_t> extract_avc_param_sets(
    std::span<const std::uint8_t> data, const VideoEntry& entry
) {
    std::vector<std::uint8_t> out;

    if (entry.config_size < 7) {
        return out;
    }

    const std::size_t limit =
        entry.config_offset <= data.size()
            ? std::min(entry.config_offset + entry.config_size, data.size())
            : data.size();

    const std::uint8_t num_sps = data[entry.config_offset + 5] & 0x1Fu;
    std::size_t p = entry.config_offset + 6;

    auto push_nal = [&](std::size_t nal, std::size_t len) {
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0x01);
        out.insert(out.end(), data.begin() + nal, data.begin() + nal + len);
    };

    for (std::uint8_t i = 0; i < num_sps && p + 2 <= limit; ++i) {
        const std::uint16_t len = static_cast<std::uint16_t>((data[p] << 8) | data[p + 1]);
        p += 2;
        if (p + len > limit) {
            break;
        }
        push_nal(p, len);
        p += len;
    }

    if (p >= limit) {
        return out;
    }

    const std::uint8_t num_pps = data[p++];

    for (std::uint8_t i = 0; i < num_pps && p + 2 <= limit; ++i) {
        const std::uint16_t len = static_cast<std::uint16_t>((data[p] << 8) | data[p + 1]);
        p += 2;
        if (p + len > limit) {
            break;
        }
        push_nal(p, len);
        p += len;
    }

    return out;
}

inline std::vector<std::uint8_t> extract_param_sets(
    std::span<const std::uint8_t> data, const VideoEntry& entry
) {
    std::vector<std::uint8_t> out;

    if (entry.fourcc == "avc1" || entry.fourcc == "avc3") {
        return extract_avc_param_sets(data, entry);
    }

    if (entry.fourcc == "hvc1" || entry.fourcc == "hev1" || entry.fourcc == "vvc1") {
        std::vector<std::uint8_t> tmp;
        es::prepend_param_sets(tmp, data.subspan(entry.config_offset, entry.config_size), true);
        out = std::move(tmp);
    }

    return out;
}

inline void build_sample_table(
    std::span<const std::uint8_t> data, ParsedTrack& pt, std::vector<SampleEntry>& out
) {
    if (pt.stsz == data.size() || pt.stsc == data.size() ||
        (pt.stco == data.size() && pt.co64 == data.size())) {
        return;
    }

    std::vector<std::uint32_t> sizes;

    {
        Box b = box_at(data, pt.stsz);
        if (b.data + 12 > data.size()) {
            return;
        }
        if (b.data + 12 > b.end) {
            return;
        }

        const std::uint32_t sample_size = read_u32(data, b.data + 4);
        std::uint32_t sample_count = read_u32(data, b.data + 8);

        constexpr std::uint32_t kMaxSamples = 1000000u;
        if (sample_count > kMaxSamples) {
            sample_count = kMaxSamples;
        }

        if (sample_size != 0) {
            sizes.assign(sample_count, sample_size);
        } else {
            for (std::uint32_t i = 0; i < sample_count; ++i) {
                if (b.data + 12 + i * 4 + 4 > b.end) {
                    break;
                }
                sizes.push_back(read_u32(data, b.data + 12 + i * 4));
            }
        }
    }

    std::vector<std::uint32_t> stsc_first;
    std::vector<std::uint32_t> stsc_count;

    {
        Box b = box_at(data, pt.stsc);
        if (b.data + 8 > b.end) {
            return;
        }

        const std::uint32_t count = read_u32(data, b.data + 4);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t p = b.data + 8 + i * 12;
            if (p + 12 > b.end) {
                break;
            }
            stsc_first.push_back(read_u32(data, p));
            stsc_count.push_back(read_u32(data, p + 4));
        }
    }

    std::vector<std::uint64_t> chunk_offsets;

    if (pt.co64 != data.size()) {
        Box b = box_at(data, pt.co64);
        if (b.data + 8 > b.end) {
            return;
        }
        const std::uint32_t count = read_u32(data, b.data + 4);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t p = b.data + 8 + i * 8;
            if (p + 8 > b.end) {
                break;
            }
            chunk_offsets.push_back(read_u64(data, p));
        }
    } else {
        Box b = box_at(data, pt.stco);
        if (b.data + 8 > b.end) {
            return;
        }
        const std::uint32_t count = read_u32(data, b.data + 4);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t p = b.data + 8 + i * 4;
            if (p + 4 > b.end) {
                break;
            }
            chunk_offsets.push_back(read_u32(data, p));
        }
    }

    if (chunk_offsets.empty()) {
        return;
    }

    auto samples_per_chunk = [&](std::uint32_t chunk) {
        std::uint32_t count = 1;
        for (std::size_t i = 0; i < stsc_first.size(); ++i) {
            if (stsc_first[i] <= chunk) {
                count = stsc_count[i];
            }
        }
        return count;
    };

    /*
     * Timing runs are attacker-controlled counts; cap them to the number of
     * real samples so a tiny stts/ctts cannot force a multi-gigabyte vector.
     */
    const std::size_t max_timing_entries = sizes.size();

    std::vector<std::int64_t> dts_list;
    std::vector<std::int64_t> dur_list;

    if (pt.stts != data.size()) {
        Box b = box_at(data, pt.stts);
        if (b.data + 8 <= b.end) {
            const std::uint32_t count = read_u32(data, b.data + 4);
            std::size_t p = b.data + 8;
            std::int64_t cur = 0;
            for (std::uint32_t i = 0; i < count && p + 8 <= b.end; ++i) {
                const std::uint32_t c = read_u32(data, p);
                const std::uint32_t d = read_u32(data, p + 4);
                p += 8;
                for (std::uint32_t k = 0; k < c; ++k) {
                    if (dts_list.size() >= max_timing_entries) {
                        break;
                    }
                    dts_list.push_back(cur);
                    dur_list.push_back(d);
                    cur += d;
                }
                if (dts_list.size() >= max_timing_entries) {
                    break;
                }
            }
        }
    }

    std::vector<std::int64_t> ctts_off;
    if (pt.ctts != data.size()) {
        Box b = box_at(data, pt.ctts);
        if (b.data + 8 <= b.end) {
            const std::uint32_t count = read_u32(data, b.data + 4);
            std::size_t p = b.data + 8;
            for (std::uint32_t i = 0; i < count && p + 8 <= b.end; ++i) {
                const std::uint32_t c = read_u32(data, p);
                const std::int32_t off = static_cast<std::int32_t>(read_u32(data, p + 4));
                p += 8;
                for (std::uint32_t k = 0; k < c; ++k) {
                    if (ctts_off.size() >= max_timing_entries) {
                        break;
                    }
                    ctts_off.push_back(off);
                }
                if (ctts_off.size() >= max_timing_entries) {
                    break;
                }
            }
        }
    }

    std::vector<char> is_sync;
    if (pt.stss != data.size()) {
        Box b = box_at(data, pt.stss);
        if (b.data + 8 <= b.end) {
            const std::uint32_t count = read_u32(data, b.data + 4);
            is_sync.assign(sizes.size(), 0);
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::size_t p = b.data + 8 + i * 4;
                if (p + 4 > b.end) {
                    break;
                }
                const std::uint32_t num = read_u32(data, p);
                if (num >= 1 && num <= is_sync.size()) {
                    is_sync[num - 1] = 1;
                }
            }
        }
    } else {
        is_sync.assign(sizes.size(), 1);
    }

    std::uint32_t sample_index = 0;

    for (std::uint32_t chunk = 0; chunk < chunk_offsets.size(); ++chunk) {
        const std::uint32_t spc = samples_per_chunk(chunk + 1);
        std::uint64_t off = chunk_offsets[chunk];

        for (std::uint32_t j = 0; j < spc; ++j) {
            if (sample_index >= sizes.size()) {
                break;
            }

            const std::uint32_t size = sizes[sample_index];

            if (off <= data.size() && static_cast<std::uint64_t>(size) <=
                                          static_cast<std::uint64_t>(data.size() - off)) {
                SampleEntry e;
                e.offset = static_cast<std::size_t>(off);
                e.size = size;
                e.dts = sample_index < dts_list.size() ? dts_list[sample_index]
                                                       : static_cast<std::int64_t>(sample_index) * 1000;
                e.duration = sample_index < dur_list.size()
                                 ? dur_list[sample_index]
                                 : 1000;
                e.pts = e.dts + (sample_index < ctts_off.size() ? ctts_off[sample_index] : 0);
                e.is_key = sample_index < is_sync.size() ? is_sync[sample_index] != 0 : true;
                e.framing = pt.view.framing;
                out.push_back(e);
            }

            off += size;
            ++sample_index;
        }
    }
}

inline bool parse_tfhd(
    std::span<const std::uint8_t> data, std::size_t pos, std::uint32_t& track_id,
    std::uint64_t& base, bool& base_present, std::uint32_t& default_duration,
    std::uint32_t& default_size, std::uint32_t& default_flags
) {
    Box b = box_at(data, pos);
    if (b.data + 8 > b.end) {
        return false;
    }

    std::size_t p = b.data;
    const std::uint32_t flags = read_u32(data, p) & 0x00FFFFFFu;
    p += 4;
    track_id = read_u32(data, p);
    p += 4;

    base = 0;
    base_present = false;
    default_duration = 0;
    default_size = 0;
    default_flags = 0;

    if ((flags & 0x000001u) != 0) {
        if (p + 8 > b.end) {
            return false;
        }
        base = read_u64(data, p);
        p += 8;
        base_present = true;
    }
    if ((flags & 0x000002u) != 0) {
        p += 4;
    }
    if ((flags & 0x000008u) != 0) {
        if (p + 4 > b.end) {
            return false;
        }
        default_duration = read_u32(data, p);
        p += 4;
    }
    if ((flags & 0x000010u) != 0) {
        if (p + 4 > b.end) {
            return false;
        }
        default_size = read_u32(data, p);
        p += 4;
    }
    if ((flags & 0x000020u) != 0) {
        if (p + 4 > b.end) {
            return false;
        }
        default_flags = read_u32(data, p);
        p += 4;
    }

    return true;
}

inline bool parse_tfdt(
    std::span<const std::uint8_t> data, std::size_t pos, std::uint64_t& base_decode_time
) {
    Box b = box_at(data, pos);
    if (b.data + 8 > b.end) {
        return false;
    }

    std::size_t p = b.data;
    const std::uint8_t version = data[p];
    p += 4;

    if (version == 1) {
        if (p + 8 > b.end) {
            return false;
        }
        base_decode_time = read_u64(data, p);
    } else {
        base_decode_time = read_u32(data, p);
    }

    return true;
}

inline void build_fragment_samples(
    std::span<const std::uint8_t> data, std::vector<ParsedTrack>& tracks,
    std::vector<std::vector<SampleEntry>>& samples
) {
    const auto moofs = top_level_boxes(data, "moof");

    for (std::size_t moof : moofs) {
        Box moof_box = box_at(data, moof);
        std::size_t traf = find_child(data, moof_box.data, moof_box.end, "traf");

        while (traf != data.size()) {
            Box traf_box = box_at(data, traf);

            std::uint32_t track_id = 0;
            std::uint64_t base = 0;
            bool base_present = false;
            std::uint32_t default_duration = 0;
            std::uint32_t default_size = 0;
            std::uint32_t default_flags = 0;

            const std::size_t tfhd = find_child(data, traf_box.data, traf_box.end, "tfhd");
            if (tfhd != data.size()) {
                parse_tfhd(
                    data, tfhd, track_id, base, base_present, default_duration, default_size,
                    default_flags
                );
            }

            std::uint64_t base_decode_time = 0;
            const std::size_t tfdt = find_child(data, traf_box.data, traf_box.end, "tfdt");
            if (tfdt != data.size()) {
                parse_tfdt(data, tfdt, base_decode_time);
            }

            if (!base_present) {
                base = moof;
            }

            std::size_t trun = find_child(data, traf_box.data, traf_box.end, "trun");

            while (trun != data.size()) {
                Box b = box_at(data, trun);
                if (b.data + 8 <= b.end) {
                    std::size_t p = b.data;
                    const std::uint32_t flags = read_u32(data, p) & 0x00FFFFFFu;
                    p += 4;
                    const std::uint32_t sample_count = read_u32(data, p);
                    p += 4;

                    int64_t data_offset = 0;
                    if ((flags & 0x000001u) != 0) {
                        if (p + 4 <= b.end) {
                            data_offset = static_cast<std::int32_t>(read_u32(data, p));
                            p += 4;
                        }
                    }

                    std::uint32_t first_sample_flags = 0;
                    if ((flags & 0x000004u) != 0) {
                        if (p + 4 <= b.end) {
                            first_sample_flags = read_u32(data, p);
                            p += 4;
                        }
                    }

                    std::uint64_t offset = static_cast<std::uint64_t>(base) +
                                           static_cast<std::uint64_t>(data_offset);
                    std::int64_t elapsed = 0;

                    for (std::uint32_t i = 0; i < sample_count; ++i) {
                        std::uint32_t duration = default_duration;
                        std::uint32_t size = default_size;
                        std::uint32_t sample_flags = default_flags;

                        if ((flags & 0x000100u) != 0) {
                            if (p + 4 > b.end) {
                                break;
                            }
                            duration = read_u32(data, p);
                            p += 4;
                        }
                        if ((flags & 0x000200u) != 0) {
                            if (p + 4 > b.end) {
                                break;
                            }
                            size = read_u32(data, p);
                            p += 4;
                        }
                        if ((flags & 0x000400u) != 0) {
                            if (p + 4 > b.end) {
                                break;
                            }
                            sample_flags = read_u32(data, p);
                            p += 4;
                        }
                        if ((flags & 0x000800u) != 0) {
                            p += 4;
                        }

                        if (i == 0 && (flags & 0x000004u) != 0) {
                            sample_flags = first_sample_flags;
                        }

                        for (auto& pt : tracks) {
                            if (pt.view.id == track_id &&
                                (pt.view.is_video || pt.view.is_audio)) {
                                SampleEntry e;
                                e.offset = static_cast<std::size_t>(offset);
                                e.size = size;
                                if (offset > data.size() || size > data.size() - offset) {
                                    break;  // skip out-of-range fragment sample
                                }
                                e.dts = static_cast<std::int64_t>(base_decode_time) + elapsed;
                                e.pts = e.dts;
                                e.duration = duration;
                                e.is_key = (sample_flags & 0x00010000u) == 0;
                                e.framing = pt.view.framing;
                                samples[&pt - &tracks[0]].push_back(e);
                                break;
                            }
                        }

                        offset += size;
                        elapsed += static_cast<std::int64_t>(duration);
                    }
                }

                const std::size_t trun_end = box_at(data, trun).end;
                trun = find_child(data, trun_end, traf_box.end, "trun");
            }

            traf = find_child(data, traf_box.end, moof_box.end, "traf");
        }
    }
}

/*
 * Populate container- and per-track metadata. Called after the track
 * views have been copied into `out` so the indices line up with the
 * order in which the `trak` boxes appear under `moov` (the same order
 * parse_moov_tracks uses).
 */
inline void build_mp4_metadata(std::span<const std::uint8_t> data, std::size_t moov, IndexBuildResult& out) {
    if (moov >= data.size()) {
        return;
    }

    const Box moov_box = box_at(data, moov);

    /* File-level tags: udta/meta first, then a direct moov/meta. */
    std::size_t meta_pos = data.size();
    const std::size_t udta = find_child(data, moov_box.data, moov_box.end, "udta");
    if (udta != data.size()) {
        const Box u = box_at(data, udta);
        meta_pos = find_child(data, u.data, u.end, "meta");
    }
    if (meta_pos == data.size()) {
        meta_pos = find_child(data, moov_box.data, moov_box.end, "meta");
    }
    if (meta_pos != data.size()) {
        parse_meta_box(data, meta_pos, out.metadata);
    }

    /* File-level creation time from mvhd (fall back to the first mdhd). */
    const std::size_t mvhd = find_child(data, moov_box.data, moov_box.end, "mvhd");
    if (mvhd != data.size()) {
        out.metadata.creation_time = mp4_creation_time(data, mvhd);
    }

    /* Per-track tags and language, walking trak boxes in order. */
    std::size_t trak = find_child(data, moov_box.data, moov_box.end, "trak");
    std::size_t index = 0;
    while (trak != data.size() && index < out.tracks.size()) {
        const Box tb = box_at(data, trak);

        const std::size_t t_udta = find_child(data, tb.data, tb.end, "udta");
        if (t_udta != data.size()) {
            const Box u = box_at(data, t_udta);
            const std::size_t t_meta = find_child(data, u.data, u.end, "meta");
            if (t_meta != data.size()) {
                parse_meta_box(data, t_meta, out.tracks[index].metadata);
            }
        }

        const std::size_t mdia = find_child(data, tb.data, tb.end, "mdia");
        if (mdia != data.size()) {
            const Box mb = box_at(data, mdia);
            const std::size_t mdhd = find_child(data, mb.data, mb.end, "mdhd");
            if (mdhd != data.size()) {
                const std::string lang = mp4_language(data, mdhd);
                if (!lang.empty()) {
                    out.tracks[index].language = lang;
                }
                if (out.tracks[index].metadata.creation_time == 0) {
                    out.tracks[index].metadata.creation_time = mp4_creation_time(data, mdhd);
                }
                if (index == 0 && out.metadata.creation_time == 0) {
                    out.metadata.creation_time = mp4_creation_time(data, mdhd);
                }
            }
        }

        ++index;
        trak = find_child(data, tb.end, moov_box.end, "trak");
    }
}

}  // namespace mp4_index

inline bool build_mp4(FileSource& src, IndexBuildResult& out) {
    const auto data = src.data();

    if (data.size() < 16) {
        return false;
    }

    const std::size_t moov = mp4_index::top_level_boxes(data, "moov").empty()
                                 ? data.size()
                                 : mp4_index::top_level_boxes(data, "moov").front();
    const bool has_moof = !mp4_index::top_level_boxes(data, "moof").empty();

    if (moov == data.size() && !has_moof) {
        return false;
    }

    std::vector<mp4_index::ParsedTrack> tracks;

    if (moov != data.size()) {
        mp4_index::parse_moov_tracks(data, moov, tracks);
    }

    if (tracks.empty()) {
        return false;
    }

    out.tracks.resize(tracks.size());
    out.samples.resize(tracks.size());
    out.param_sets.resize(tracks.size());

    for (std::size_t i = 0; i < tracks.size(); ++i) {
        out.tracks[i] = tracks[i].view;
        out.tracks[i].mmap_backed = src.mmap_backed();

        if (tracks[i].video) {
            out.param_sets[i] = mp4_index::extract_param_sets(data, tracks[i].entry);

            if (!out.param_sets[i].empty()) {
                SampleEntry e;
                e.is_param_set = true;
                e.offset = 0;
                e.size = out.param_sets[i].size();
                e.framing = NalFramingMode::AnnexB;
                e.is_key = true;
                out.samples[i].push_back(e);
            }

            mp4_index::build_sample_table(data, tracks[i], out.samples[i]);
        } else if (tracks[i].audio) {
            // Audio samples use the same stsz/stco/stts tables; no parameter
            // sets are prepended and framing stays RawFrame.
            mp4_index::build_sample_table(data, tracks[i], out.samples[i]);
        }
    }

    if (has_moof) {
        mp4_index::build_fragment_samples(data, tracks, out.samples);
    }

    /*
     * The lengthSizeMinusOne offset differs between muxers and codec-config
     * boxes.  Sanity-check it against the first real sample and fall back to
     * 4 bytes when implausible.
     */
    for (std::size_t i = 0; i < out.tracks.size(); ++i) {
        if (out.tracks[i].framing == NalFramingMode::LengthPrefixed) {
            unsigned ls = out.tracks[i].length_size;

            for (const auto& e : out.samples[i]) {
                if (e.is_param_set) {
                    continue;
                }

                if (ls < 1 || ls > 4 || e.size <= ls) {
                    ls = 4;
                    break;
                }

                std::uint32_t first_len = 0;
                for (unsigned k = 0; k < ls; ++k) {
                    first_len = (first_len << 8) | data[e.offset + k];
                }

                if (first_len == 0 || first_len > e.size) {
                    ls = 4;
                }
                break;
            }

            out.tracks[i].length_size = ls;
        }
    }

    for (std::size_t i = 0; i < out.tracks.size(); ++i) {
        out.tracks[i].sample_count = out.samples[i].size();
    }

    if (moov != data.size()) {
        mp4_index::build_mp4_metadata(data, moov, out);
    }

    return true;
}


/* ------------------------------------------------------------------ */
/* Matroska / WebM                                                     */
/* ------------------------------------------------------------------ */

namespace mkv_index {

using mkv::detail::kBlock;
using mkv::detail::kBlockGroup;
using mkv::detail::kCluster;
using mkv::detail::kCodecId;
using mkv::detail::kCodecPrivate;
using mkv::detail::kPixelHeight;
using mkv::detail::kPixelWidth;
using mkv::detail::kSegment;
using mkv::detail::kSimpleBlock;
using mkv::detail::kTrackEntry;
using mkv::detail::kTrackNumber;
using mkv::detail::kTracks;
using mkv::detail::kTrackType;
using mkv::detail::kVideo;
using mkv::detail::parse_track_entry;
using mkv::detail::read_id;
using mkv::detail::read_size;
using mkv::detail::read_vint;
using mkv::detail::VideoTrack;

/*
 * -----------------------------------------------------------
 * Matroska metadata (Info / Tags + per-track Language/CodecName)
 * -----------------------------------------------------------
 *
 * ID values are the complete EBML element IDs (marker bits included).
 * All reads are bounds-checked; malformed elements are skipped.
 */
inline constexpr std::uint64_t kInfo = 0x1549A966ull;
inline constexpr std::uint64_t kTitle = 0x7BA9ull;
inline constexpr std::uint64_t kDateUTC = 0x4461ull;
inline constexpr std::uint64_t kTags = 0x1254C367ull;
inline constexpr std::uint64_t kTag = 0x7373ull;
inline constexpr std::uint64_t kSimpleTag = 0x67C8ull;
inline constexpr std::uint64_t kTagName = 0x45A3ull;
inline constexpr std::uint64_t kTagString = 0x4487ull;
inline constexpr std::uint64_t kTrackLanguage = 0x22B59Cull;
inline constexpr std::uint64_t kCodecName = 0x258688ull;

/*
 * Read one EBML element header and expose its payload. Returns false
 * on any malformed / truncated element, leaving `pos` undefined to
 * callers (they stop the loop in that case).
 */
inline bool next_ebml(
    std::span<const std::uint8_t> d,
    std::size_t& pos,
    std::uint64_t& id,
    std::span<const std::uint8_t>& payload
) {
    std::uint64_t id_len = 0;
    std::uint64_t size = 0;

    if (!read_vint(d, pos, id, id_len, true)) {
        return false;
    }
    if (!read_size(d, pos, size)) {
        return false;
    }
    /* An empty element (size 0, e.g. MkvTargets) is valid; pos still
     * advanced past its ID and size vints, so the loop makes progress. */
    if (size > d.size() - pos) {
        return false;
    }

    payload = d.subspan(pos, static_cast<std::size_t>(size));
    pos += static_cast<std::size_t>(size);
    return true;
}

inline std::string as_text(std::span<const std::uint8_t> d) {
    /* Bound metadata string allocation independently of the file size. */
    constexpr std::size_t kMaxTagLen = 64u * 1024u;
    const std::size_t n = std::min(d.size(), kMaxTagLen);
    return std::string(reinterpret_cast<const char*>(d.data()), n);
}

inline void parse_info(std::span<const std::uint8_t> d, ContainerMetadata& out) {
    std::size_t p = 0;
    std::uint64_t id = 0;
    std::span<const std::uint8_t> payload;

    while (p < d.size()) {
        if (!next_ebml(d, p, id, payload)) {
            break;
        }
        if (id == kTitle) {
            out.title = as_text(payload);
        } else if (id == kDateUTC && payload.size() >= 8) {
            std::uint64_t ns = 0;
            for (std::size_t i = 0; i < 8; ++i) {
                ns = (ns << 8) | payload[i];
            }
            out.creation_time = static_cast<std::int64_t>(ns / 1000000000ull);
        }
    }
}

inline void parse_tags(std::span<const std::uint8_t> d, ContainerMetadata& out) {
    std::size_t p = 0;
    std::uint64_t id = 0;
    std::span<const std::uint8_t> tag;

    while (p < d.size()) {
        if (!next_ebml(d, p, id, tag)) {
            break;
        }
        if (id != kTag) {
            continue;
        }

        std::size_t q = 0;
        std::uint64_t tag_id = 0;
        std::span<const std::uint8_t> simple;

        while (q < tag.size()) {
            if (!next_ebml(tag, q, tag_id, simple)) {
                break;
            }
            if (tag_id != kSimpleTag) {
                continue;
            }

            std::string name;
            std::string value;

            std::size_t r = 0;
            std::uint64_t field_id = 0;
            std::span<const std::uint8_t> field;

            while (r < simple.size()) {
                if (!next_ebml(simple, r, field_id, field)) {
                    break;
                }
                if (field_id == kTagName) {
                    name = as_text(field);
                } else if (field_id == kTagString) {
                    value = as_text(field);
                }
            }

            if (name.empty() || value.empty()) {
                continue;
            }
            if (name == "TITLE") {
                if (out.title.empty()) out.title = value;
            } else if (name == "ARTIST") {
                if (out.artist.empty()) out.artist = value;
            } else if (name == "COMMENT") {
                if (out.comment.empty()) out.comment = value;
            } else if (name == "COPYRIGHT") {
                if (out.copyright.empty()) out.copyright = value;
            } else if (out.custom.size() < 4096u) {
                out.custom.emplace_back(std::move(name), std::move(value));
            }
        }
    }
}

/* Language and CodecName live directly under a TrackEntry. */
inline void parse_track_meta(
    std::span<const std::uint8_t> d, ContainerMetadata& out, std::string& language
) {
    std::size_t p = 0;
    std::uint64_t id = 0;
    std::span<const std::uint8_t> payload;

    while (p < d.size()) {
        if (!next_ebml(d, p, id, payload)) {
            break;
        }
        if (id == kTrackLanguage) {
            language = as_text(payload);
        } else if (id == kCodecName) {
            out.title = as_text(payload);
        }
    }
}

inline bool parse_block_header(
    std::span<const std::uint8_t> d, std::uint64_t& track_number, std::int16_t& time,
    std::uint8_t& flags, std::span<const std::uint8_t>& frame
) {
    std::size_t p = 0;
    std::uint64_t tn = 0;
    std::uint64_t tlen = 0;

    if (!read_vint(d, p, tn, tlen, false)) {
        return false;
    }

    if (p + 3 > d.size()) {
        return false;
    }

    track_number = tn;
    time = static_cast<std::int16_t>((d[p] << 8) | d[p + 1]);
    p += 2;
    flags = d[p++];
    frame = d.subspan(p);

    return true;
}

inline std::vector<std::uint8_t> extract_mkv_param_sets(const VideoTrack& track) {
    std::vector<std::uint8_t> out;

    if (track.codec_id == "V_MPEG4/ISO/AVC") {
        mp4_index::VideoEntry e;
        e.found = true;
        e.fourcc = "avc1";
        e.config_offset = 0;
        e.config_size = track.codec_private.size();
        return mp4_index::extract_avc_param_sets(track.codec_private, e);
    }

    if (track.codec_id == "V_MPEGH/ISO/HEVC" || track.codec_id == "V_VVC") {
        std::vector<std::uint8_t> tmp;
        es::prepend_param_sets(tmp, track.codec_private, true);
        out = std::move(tmp);
    }

    return out;
}

}  // namespace mkv_index

inline bool build_mkv(FileSource& src, IndexBuildResult& out) {
    const auto data = src.data();

    if (data.size() < 4 || data[0] != 0x1A || data[1] != 0x45 || data[2] != 0xDF ||
        data[3] != 0xA3) {
        return false;
    }

    std::size_t pos = 0;
    std::size_t seg_begin = 0;
    std::size_t seg_end = data.size();

    while (pos + 4 <= data.size()) {
        std::uint64_t id = 0;
        std::uint64_t size = 0;
        std::uint64_t id_len = 0;

        if (!mkv::detail::read_vint(data, pos, id, id_len, true)) {
            return false;
        }
        if (!mkv::detail::read_size(data, pos, size)) {
            return false;
        }

        if (id == mkv::detail::kSegment) {
            seg_begin = pos;
            seg_end = (size != 0 && size < data.size() - seg_begin)
                          ? seg_begin + static_cast<std::size_t>(size)
                          : data.size();
            break;
        }

        if (size > data.size() - pos || size == 0) {
            return false;
        }

        pos += size;
    }

    if (seg_begin + 4 > data.size()) {
        return false;
    }

    std::vector<mkv::detail::VideoTrack> tracks;
    /* TrackEntry payloads, kept in the same (found-only) order as `tracks`. */
    std::vector<std::span<const std::uint8_t>> track_entries;

    {
        std::size_t p = seg_begin;

        while (p + 4 <= seg_end) {
            std::uint64_t id = 0;
            std::uint64_t size = 0;
            std::uint64_t id_len = 0;

            if (!mkv::detail::read_vint(data, p, id, id_len, true)) {
                break;
            }
            if (!mkv::detail::read_size(data, p, size)) {
                break;
            }
            if (size > seg_end - p || size == 0) {
                break;
            }

            const auto element = data.subspan(p, static_cast<std::size_t>(size));

            if (id == mkv_index::kInfo) {
                mkv_index::parse_info(element, out.metadata);
            } else if (id == mkv_index::kTags) {
                mkv_index::parse_tags(element, out.metadata);
            }

            if (id == mkv::detail::kTracks) {
                std::size_t q = 0;

                while (q + 4 <= element.size()) {
                    std::uint64_t eid = 0;
                    std::uint64_t esize = 0;
                    std::uint64_t elen = 0;

                    if (!mkv::detail::read_vint(element, q, eid, elen, true)) {
                        break;
                    }
                    if (!mkv::detail::read_size(element, q, esize)) {
                        break;
                    }
                    if (q + esize > element.size() || esize == 0) {
                        break;
                    }

                    if (eid == mkv::detail::kTrackEntry) {
                        const auto entry = element.subspan(q, static_cast<std::size_t>(esize));
                        mkv::detail::VideoTrack t;
                        mkv::detail::parse_track_entry(entry, t);
                        if (t.found) {
                            tracks.push_back(std::move(t));
                            track_entries.push_back(entry);
                        }
                    }

                    q += esize;
                }
            }

            p += size;
        }
    }

    if (tracks.empty()) {
        return false;
    }

    out.tracks.resize(tracks.size());
    out.samples.resize(tracks.size());
    out.param_sets.resize(tracks.size());

    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const auto& t = tracks[i];
        TrackView v;
        v.id = static_cast<std::uint32_t>(t.number);
        v.codec_name = t.codec_id;
        v.width = static_cast<std::uint32_t>(t.width);
        v.height = static_cast<std::uint32_t>(t.height);
        v.timescale = 1000;
        v.mmap_backed = src.mmap_backed();

        if (t.track_type == 2u) {
            // Audio track.
            v.is_audio = true;
            v.framing = NalFramingMode::RawFrame;
            v.sample_rate = t.sample_rate;
            v.channels = t.channels;
            v.bits_per_sample = t.bits_per_sample;
            v.audio_config = t.codec_private;
            if (t.codec_id == "A_AAC") v.audio_codec = AudioCodec::Aac;
            else if (t.codec_id == "A_OPUS") v.audio_codec = AudioCodec::Opus;
            else if (t.codec_id == "A_VORBIS") v.audio_codec = AudioCodec::Vorbis;
            else if (t.codec_id == "A_FLAC") v.audio_codec = AudioCodec::Flac;
            else if (t.codec_id == "A_AC3") v.audio_codec = AudioCodec::Ac3;
            else if (t.codec_id == "A_EAC3") v.audio_codec = AudioCodec::Ec3;
            else if (t.codec_id == "A_DTS") v.audio_codec = AudioCodec::Dts;
            else if (t.codec_id == "A_PCM/INT/LIT" || t.codec_id == "A_PCM/INT/BIG") {
                v.audio_codec = t.bits_per_sample == 8u ? AudioCodec::Pcm8
                              : t.bits_per_sample == 24u ? AudioCodec::Pcm24
                              : t.bits_per_sample == 32u ? AudioCodec::Pcm32
                                                         : AudioCodec::Pcm16;
                if (v.bits_per_sample == 0u) {
                    v.bits_per_sample = audio_bits_per_sample(v.audio_codec);
                }
            } else if (t.codec_id == "A_PCM/FLOAT/IEEE") {
                v.audio_codec = t.bits_per_sample == 64u ? AudioCodec::PcmFloat64
                                                         : AudioCodec::PcmFloat32;
            }
        } else if (t.track_type == 0x11u || t.track_type == 0x12u) {
            v.is_subtitle = true;
            v.framing = NalFramingMode::RawFrame;
            v.subtitle_codec = t.codec_id; // e.g. S_TEXT/UTF8, S_TEXT/WEBVTT
        } else if (t.codec_id == "V_MPEG4/ISO/AVC") {
            v.is_video = true;
            v.codec = Codec::Avc;
            v.framing = NalFramingMode::LengthPrefixed;
            v.length_size = t.codec_private.size() > 4
                                ? static_cast<unsigned>(t.codec_private[4] & 3u) + 1u
                                : 4u;
        } else if (t.codec_id == "V_MPEGH/ISO/HEVC") {
            v.is_video = true;
            v.codec = Codec::Hevc;
            v.framing = NalFramingMode::LengthPrefixed;
            v.length_size = t.codec_private.size() > 18
                                ? static_cast<unsigned>(t.codec_private[18] & 3u) + 1u
                                : 4u;
        } else if (t.codec_id == "V_VVC") {
            v.is_video = true;
            v.codec = Codec::Vvc;
            v.framing = NalFramingMode::LengthPrefixed;
            v.length_size = t.codec_private.size() > 14
                                ? static_cast<unsigned>(t.codec_private[14] & 3u) + 1u
                                : 4u;
        } else if (t.codec_id == "V_AV1") {
            v.is_video = true;
            v.codec = Codec::Av1;
            v.framing = NalFramingMode::Obu;
        } else if (t.codec_id == "V_VP9") {
            v.is_video = true;
            v.codec = Codec::Vp9;
            v.framing = NalFramingMode::RawFrame;
        } else if (t.codec_id == "V_VP8") {
            v.is_video = true;
            v.codec = Codec::Vp8;
            v.framing = NalFramingMode::RawFrame;
        } else {
            continue;
        }

        out.tracks[i] = v;

        if (v.is_video &&
            (v.codec == Codec::Avc || v.codec == Codec::Hevc || v.codec == Codec::Vvc)) {
            out.param_sets[i] = mkv_index::extract_mkv_param_sets(t);
            if (!out.param_sets[i].empty()) {
                SampleEntry e;
                e.is_param_set = true;
                e.offset = 0;
                e.size = out.param_sets[i].size();
                e.framing = NalFramingMode::AnnexB;
                e.is_key = true;
                out.samples[i].push_back(e);
            }
        }
    }

    /* Per-track Language / CodecName from the TrackEntry elements. */
    for (std::size_t i = 0; i < out.tracks.size() && i < track_entries.size(); ++i) {
        mkv_index::parse_track_meta(track_entries[i], out.tracks[i].metadata, out.tracks[i].language);
    }

    // Walk clusters and index blocks.
    std::size_t p = seg_begin;

    while (p + 4 <= seg_end) {
        std::uint64_t id = 0;
        std::uint64_t size = 0;
        std::uint64_t id_len = 0;

        if (!mkv::detail::read_vint(data, p, id, id_len, true)) {
            break;
        }
        if (!mkv::detail::read_size(data, p, size)) {
            break;
        }
        if (size > seg_end - p || size == 0) {
            break;
        }

        const auto element = data.subspan(p, static_cast<std::size_t>(size));

        if (id == mkv::detail::kCluster) {
            std::int64_t cluster_time = 0;
            std::size_t q = 0;

            while (q + 4 <= element.size()) {
                std::uint64_t cid = 0;
                std::uint64_t csize = 0;
                std::uint64_t clen = 0;

                if (!mkv::detail::read_vint(element, q, cid, clen, true)) {
                    break;
                }
                if (!mkv::detail::read_size(element, q, csize)) {
                    break;
                }
                if (q + csize > element.size() || csize == 0) {
                    break;
                }

                const auto child = element.subspan(q, static_cast<std::size_t>(csize));

                if (cid == 0xE7 && !child.empty()) {
                    std::int64_t v = 0;
                    for (std::uint8_t b : child) {
                        v = (v << 8) | b;
                    }
                    cluster_time = v;
                }

                std::span<const std::uint8_t> block_data;

                if (cid == mkv::detail::kSimpleBlock) {
                    block_data = child;
                } else if (cid == mkv::detail::kBlockGroup) {
                    std::size_t b = 0;
                    std::uint64_t bid = 0;
                    std::uint64_t bsize = 0;
                    std::uint64_t blen = 0;

                    if (mkv::detail::read_vint(child, b, bid, blen, true) &&
                        mkv::detail::read_size(child, b, bsize) && bid == mkv::detail::kBlock &&
                        b + bsize <= child.size()) {
                        block_data = child.subspan(b, static_cast<std::size_t>(bsize));
                    }
                }

                if (!block_data.empty()) {
                    std::uint64_t track_number = 0;
                    std::int16_t block_time = 0;
                    std::uint8_t flags = 0;
                    std::span<const std::uint8_t> frame;

                    if (mkv_index::parse_block_header(
                            block_data, track_number, block_time, flags, frame
                        )) {
                        for (std::size_t i = 0; i < tracks.size(); ++i) {
                            if (static_cast<std::uint64_t>(tracks[i].number) != track_number) {
                                continue;
                            }

                            SampleEntry e;
                            e.offset = static_cast<std::size_t>(frame.data() - data.data());
                            e.size = frame.size();
                            e.pts = cluster_time + block_time;
                            e.dts = e.pts;
                            e.duration = 0;
                            e.is_key = (flags & 0x80u) != 0;

                            if (out.tracks[i].is_video &&
                                (out.tracks[i].codec == Codec::Avc ||
                                 out.tracks[i].codec == Codec::Hevc ||
                                 out.tracks[i].codec == Codec::Vvc)) {
                                const bool starts_annex_b =
                                    frame.size() >= 4 && frame[0] == 0x00 && frame[1] == 0x00 &&
                                    (frame[2] == 0x01 || (frame[2] == 0x00 && frame[3] == 0x01));
                                const bool annex_b =
                                    starts_annex_b &&
                                    !es::fully_length_prefixed(frame, out.tracks[i].length_size);
                                e.framing = annex_b ? NalFramingMode::AnnexB
                                                     : NalFramingMode::LengthPrefixed;
                            } else {
                                e.framing = out.tracks[i].framing;
                            }

                            out.samples[i].push_back(e);
                            break;
                        }
                    }
                }

                q += csize;
            }
        }

        p += size;
    }

    /*
     * Sanity-check the configured length size against the first real
     * sample; Matroska CodecPrivate offsets vary between muxers.
     */
    for (std::size_t i = 0; i < out.tracks.size(); ++i) {
        if (out.tracks[i].framing == NalFramingMode::LengthPrefixed) {
            unsigned ls = out.tracks[i].length_size;

            for (const auto& e : out.samples[i]) {
                if (e.is_param_set) {
                    continue;
                }

                if (ls < 1 || ls > 4 || e.size <= ls) {
                    ls = 4;
                    break;
                }

                std::uint32_t first_len = 0;
                for (unsigned k = 0; k < ls; ++k) {
                    first_len = (first_len << 8) | data[e.offset + k];
                }

                if (first_len == 0 || first_len > e.size) {
                    ls = 4;
                }
                break;
            }

            out.tracks[i].length_size = ls;
        }

        out.tracks[i].sample_count = out.samples[i].size();
    }

    return true;
}


namespace ts_index {

using ts::detail::kPacketSize;
using ts::detail::kPatPid;
using ts::detail::kSync;
using ts::detail::packet_payload;
using ts::detail::parse_pmt;
using ts::detail::payload_start;
using ts::detail::read_pid;

struct TsStream {
    std::uint16_t pid = 0;
    std::uint8_t stream_type = 0;
};

inline std::vector<TsStream> collect_streams(std::span<const std::uint8_t> data) {
    std::vector<TsStream> out;
    const std::size_t packet_count = data.size() / kPacketSize;
    std::uint16_t pmt_pid = 0;

    for (std::size_t i = 0; i < packet_count; ++i) {
        const auto packet = data.subspan(i * kPacketSize, kPacketSize);
        if (packet[0] != kSync) {
            continue;
        }

        const std::uint16_t pid = read_pid(packet);
        const auto payload = packet_payload(packet);

        if (pid == kPatPid) {
            if (payload_start(packet) && !payload.empty()) {
                std::size_t p = 1 + payload[0];
                p += 8;
                while (p + 4 <= payload.size()) {
                    const std::uint16_t program_number =
                        static_cast<std::uint16_t>((payload[p] << 8) | payload[p + 1]);
                    const std::uint16_t pid2 =
                        static_cast<std::uint16_t>(((payload[p + 2] & 0x1Fu) << 8) | payload[p + 3]);
                    if (program_number != 0) {
                        pmt_pid = pid2;
                        break;
                    }
                    p += 4;
                }
            }
            continue;
        }

        if (pmt_pid != 0 && pid == pmt_pid && payload_start(packet)) {
            std::size_t p = 0;
            if (!payload.empty()) {
                p = 1 + payload[0];
            }
            if (p + 12 > payload.size()) {
                continue;
            }

            const std::uint16_t section_length =
                static_cast<std::uint16_t>(((payload[p + 1] & 0x0Fu) << 8) | payload[p + 2]);
            const std::size_t end = p + 3 + section_length;
            if (end > payload.size()) {
                continue;
            }

            const std::uint16_t program_info_length =
                static_cast<std::uint16_t>(((payload[p + 10] & 0x0Fu) << 8) | payload[p + 11]);
            p += 12 + program_info_length;

            while (p + 5 <= end) {
                const std::uint8_t stream_type = payload[p];
                const std::uint16_t elementary_pid =
                    static_cast<std::uint16_t>(((payload[p + 1] & 0x1Fu) << 8) | payload[p + 2]);

                if (stream_type == 0x1Bu || stream_type == 0x24u || stream_type == 0x33u ||
                    stream_type == 0x06u) {
                    bool exists = false;
                    for (const auto& s : out) {
                        if (s.pid == elementary_pid) {
                            exists = true;
                            break;
                        }
                    }
                    if (!exists) {
                        out.push_back({elementary_pid, stream_type});
                    }
                }

                const std::uint16_t es_info_length =
                    static_cast<std::uint16_t>(((payload[p + 3] & 0x0Fu) << 8) | payload[p + 4]);
                p += 5 + es_info_length;
            }
        }
    }

    return out;
}

inline bool parse_pes_pts(std::span<const std::uint8_t> payload, std::int64_t& pts) {
    if (payload.size() < 14) {
        return false;
    }
    if (payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01) {
        return false;
    }
    if ((payload[7] & 0x80u) == 0) {
        return false;
    }

    const std::uint8_t* p = payload.data() + 9;
    const std::uint64_t v = (static_cast<std::uint64_t>(p[0] & 0x0E) << 29) |
                            (static_cast<std::uint64_t>(p[1]) << 22) |
                            (static_cast<std::uint64_t>(p[2] & 0xFE) << 14) |
                            (static_cast<std::uint64_t>(p[3]) << 7) |
                            (static_cast<std::uint64_t>(p[4]) >> 1);
    pts = static_cast<std::int64_t>(v);
    return true;
}

}  // namespace ts_index

inline bool build_ts(FileSource& src, IndexBuildResult& out) {
    using ts::detail::packet_payload;
    using ts::detail::read_pid;

    const auto data = src.data();
    const std::size_t packet_count = data.size() / ts::detail::kPacketSize;

    if (packet_count < 2) {
        return false;
    }

    const auto streams = ts_index::collect_streams(data);
    if (streams.empty()) {
        return false;
    }

    out.tracks.resize(streams.size());
    out.samples.resize(streams.size());
    out.param_sets.resize(streams.size());

    for (std::size_t s = 0; s < streams.size(); ++s) {
        TrackView v;
        v.id = streams[s].pid;
        v.is_video = true;
        v.timescale = 90000;
        v.mmap_backed = src.mmap_backed();

        switch (streams[s].stream_type) {
            case 0x1B:
                v.codec = Codec::Avc;
                v.framing = NalFramingMode::AnnexB;
                break;
            case 0x24:
                v.codec = Codec::Hevc;
                v.framing = NalFramingMode::AnnexB;
                break;
            case 0x33:
                v.codec = Codec::Vvc;
                v.framing = NalFramingMode::AnnexB;
                break;
            case 0x06:
                v.codec = Codec::Av1;
                v.framing = NalFramingMode::Obu;
                break;
            default:
                v.codec = Codec::Avc;
                v.framing = NalFramingMode::AnnexB;
                break;
        }

        out.tracks[s] = v;

        std::vector<SampleEntry>& samples = out.samples[s];

        bool open = false;
        std::size_t start = 0;
        std::size_t size = 0;
        std::int64_t first_pts = 0;
        bool have_pts = false;

        for (std::size_t i = 0; i < packet_count; ++i) {
            const auto packet = data.subspan(i * ts::detail::kPacketSize, ts::detail::kPacketSize);
            if (packet[0] != ts::detail::kSync) {
                continue;
            }

            if (read_pid(packet) != streams[s].pid) {
                continue;
            }

            const auto payload = packet_payload(packet);
            if (payload.empty()) {
                continue;
            }

            if (ts::detail::payload_start(packet)) {
                if (open && size > 0 && start + size <= data.size()) {
                    SampleEntry e;
                    e.offset = start;
                    e.size = size;
                    e.pts = have_pts ? first_pts : static_cast<std::int64_t>(samples.size()) * 1000;
                    e.dts = e.pts;
                    e.duration = 0;
                    e.is_key = samples.empty();
                    e.framing = out.tracks[s].framing;
                    samples.push_back(e);
                }

                open = true;
                size = 0;
                have_pts = false;

                std::size_t skip = 0;
                if (payload.size() >= 4 && payload[0] == 0x00 && payload[1] == 0x00 &&
                    payload[2] == 0x01) {
                    std::size_t j = 4;
                    for (; j + 3 <= payload.size(); ++j) {
                        if (payload[j] == 0x00 && payload[j + 1] == 0x00 &&
                            payload[j + 2] == 0x01) {
                            skip = j;
                            break;
                        }
                    }
                    if (j + 3 > payload.size()) {
                        skip = payload.size();
                    }
                }

                start = i * ts::detail::kPacketSize +
                       static_cast<std::size_t>(payload.data() - packet.data()) + skip;
                size = payload.size() - skip;

                std::int64_t pts = 0;
                if (ts_index::parse_pes_pts(payload, pts)) {
                    first_pts = pts;
                    have_pts = true;
                }
            } else if (open) {
                size += payload.size();
            }
        }

        if (open && size > 0 && start + size <= data.size()) {
            SampleEntry e;
            e.offset = start;
            e.size = size;
            e.pts = have_pts ? first_pts : static_cast<std::int64_t>(samples.size()) * 1000;
            e.dts = e.pts;
            e.duration = 0;
            e.is_key = samples.empty();
            e.framing = out.tracks[s].framing;
            samples.push_back(e);
        }

        out.tracks[s].sample_count = samples.size();
    }

    return true;
}
namespace flv_index {

using flv::detail::read_u24;
using flv::detail::read_u32;

inline std::vector<std::uint8_t> extract_flv_param_sets(
    std::span<const std::uint8_t> body, std::uint8_t codec_id
) {
    std::vector<std::uint8_t> out;

    if (codec_id == 7) {
        mp4_index::VideoEntry e;
        e.found = true;
        e.fourcc = "avc1";
        e.config_offset = 0;
        e.config_size = body.size();
        return mp4_index::extract_avc_param_sets(body, e);
    }

    if (codec_id == 12) {
        std::vector<std::uint8_t> tmp;
        es::prepend_param_sets(tmp, body, true);
        out = std::move(tmp);
    }

    return out;
}

}  // namespace flv_index

inline bool build_flv(FileSource& src, IndexBuildResult& out) {
    using flv::detail::read_u24;
    using flv::detail::read_u32;

    const auto data = src.data();

    if (data.size() < 9 || data[0] != 'F' || data[1] != 'L' || data[2] != 'V') {
        return false;
    }

    std::uint32_t data_offset = flv::detail::read_u32(data, 5);
    std::size_t pos = data_offset;

    if (pos + 4 < data.size()) {
        const auto is_tag_type = [](std::uint8_t b) {
            return b == 0x08u || b == 0x09u || b == 0x12u;
        };
        if (!is_tag_type(data[pos]) && is_tag_type(data[pos + 4])) {
            pos += 4;
        }
    }

    enum class Kind { None, Avc, Hevc, Av1, Vp8, Vp9 };
    Kind kind = Kind::None;
    bool have_params = false;

    std::vector<SampleEntry> samples;
    std::vector<std::uint8_t> param_sets;

    while (pos + 11 <= data.size()) {
        const std::uint8_t tag_type = data[pos];
        const std::uint32_t tag_size = read_u24(data, pos + 1);

        if (pos + 11 + tag_size > data.size()) {
            break;
        }

        const auto tag = data.subspan(pos + 11, tag_size);

        if (tag_type == 9 && tag_size >= 1) {
            const std::uint8_t codec_id = tag[0] & 0x0Fu;

            if (codec_id == 7 || codec_id == 12 || codec_id == 13) {
                if (tag_size < 2) {
                    pos += 11 + tag_size + 4;
                    continue;
                }

                const std::uint8_t packet_type = tag[1];
                const std::size_t data_off = (codec_id == 7 || codec_id == 12) ? 5u : 2u;

                if (data_off > tag.size()) {
                    pos += 11 + tag_size + 4;
                    continue;
                }

                const auto body = tag.subspan(data_off);

                if (packet_type == 0) {
                    kind = codec_id == 7 ? Kind::Avc
                                         : (codec_id == 12 ? Kind::Hevc : Kind::Av1);
                    param_sets = flv_index::extract_flv_param_sets(body, codec_id);
                    have_params = !param_sets.empty();
                } else if (packet_type == 1) {
                    kind = codec_id == 7 ? Kind::Avc
                                         : (codec_id == 12 ? Kind::Hevc : Kind::Av1);

                    std::int64_t ts = static_cast<std::int64_t>(
                        (static_cast<std::uint32_t>(data[pos + 4]) << 16) |
                        (static_cast<std::uint32_t>(data[pos + 5]) << 8) |
                        static_cast<std::uint32_t>(data[pos + 6]) |
                        (static_cast<std::uint32_t>(data[pos + 7]) << 24)
                    );

                    SampleEntry e;
                    e.offset = static_cast<std::size_t>(body.data() - data.data());
                    e.size = body.size();
                    e.pts = ts;
                    e.dts = ts;
                    e.duration = 0;
                    e.is_key = samples.empty();
                    e.framing = kind == Kind::Av1 ? NalFramingMode::Obu
                                                  : NalFramingMode::LengthPrefixed;
                    samples.push_back(e);
                }
            } else if (codec_id == 10 || codec_id == 11) {
                kind = codec_id == 10 ? Kind::Vp8 : Kind::Vp9;

                std::int64_t ts = static_cast<std::int64_t>(
                    (static_cast<std::uint32_t>(data[pos + 4]) << 16) |
                    (static_cast<std::uint32_t>(data[pos + 5]) << 8) |
                    static_cast<std::uint32_t>(data[pos + 6]) |
                    (static_cast<std::uint32_t>(data[pos + 7]) << 24)
                );

                const auto body = tag.subspan(1);

                SampleEntry e;
                e.offset = static_cast<std::size_t>(body.data() - data.data());
                e.size = body.size();
                e.pts = ts;
                e.dts = ts;
                e.duration = 0;
                e.is_key = samples.empty();
                e.framing = NalFramingMode::RawFrame;
                samples.push_back(e);
            }
        }

        pos += 11 + tag_size + 4;
    }

    if (samples.empty() && !have_params) {
        return false;
    }

    if (kind == Kind::None) {
        return false;
    }

    out.tracks.resize(1);
    out.samples.resize(1);
    out.param_sets.resize(1);

    TrackView v;
    v.id = 1;
    v.is_video = true;
    v.timescale = 1000;
    v.mmap_backed = src.mmap_backed();

    switch (kind) {
        case Kind::Avc:
            v.codec = Codec::Avc;
            v.framing = NalFramingMode::LengthPrefixed;
            v.length_size = 4;
            v.codec_name = "avc1";
            break;
        case Kind::Hevc:
            v.codec = Codec::Hevc;
            v.framing = NalFramingMode::LengthPrefixed;
            v.length_size = 4;
            v.codec_name = "hvc1";
            break;
        case Kind::Av1:
            v.codec = Codec::Av1;
            v.framing = NalFramingMode::Obu;
            v.codec_name = "av01";
            break;
        case Kind::Vp8:
            v.codec = Codec::Vp8;
            v.framing = NalFramingMode::RawFrame;
            v.codec_name = "vp08";
            break;
        case Kind::Vp9:
            v.codec = Codec::Vp9;
            v.framing = NalFramingMode::RawFrame;
            v.codec_name = "vp09";
            break;
        default:
            return false;
    }

    out.tracks[0] = v;
    out.param_sets[0] = std::move(param_sets);

    if (!out.param_sets[0].empty()) {
        SampleEntry e;
        e.is_param_set = true;
        e.offset = 0;
        e.size = out.param_sets[0].size();
        e.framing = NalFramingMode::AnnexB;
        e.is_key = true;
        out.samples[0].push_back(e);
    }

    for (auto& e : samples) {
        out.samples[0].push_back(std::move(e));
    }

    out.tracks[0].sample_count = out.samples[0].size();

    return true;
}
inline bool build_avi(FileSource& src, IndexBuildResult& out) {
    using avi::detail::Chunk;
    using avi::detail::chunk_at;
    using avi::detail::is_fourcc;
    using avi::detail::read_u32;

    const auto data = src.data();

    if (data.size() < 12 || !is_fourcc(data, 0, "RIFF") || !is_fourcc(data, 8, "AVI ")) {
        return false;
    }

    std::string codec_fourcc;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t pos = 12;
    std::size_t movi_pos = data.size();
    bool have_video_stream = false;

    while (pos + 8 <= data.size()) {
        const Chunk c = chunk_at(data, pos);
        if (c.end <= c.start) {
            break;
        }

        if (c.is_list) {
            if (c.data + 4 <= c.end && is_fourcc(data, c.data, "hdrl")) {
                std::size_t p = c.data + 4;

                while (p + 8 <= c.end) {
                    const Chunk item = chunk_at(data, p);
                    if (item.end <= p) {
                        break;
                    }

                    if (item.is_list && item.data + 4 <= item.end &&
                        is_fourcc(data, item.data, "strl")) {
                        std::size_t q = item.data + 4;
                        bool in_video_strl = false;

                        while (q + 8 <= item.end) {
                            const Chunk sub = chunk_at(data, q);
                            if (sub.end <= q) {
                                break;
                            }

                            if (is_fourcc(data, q, "strh") && sub.data + 12 <= sub.end) {
                                if (is_fourcc(data, sub.data, "vids")) {
                                    have_video_stream = true;
                                    in_video_strl = true;
                                    codec_fourcc = std::string(
                                        reinterpret_cast<const char*>(data.data() + sub.data + 4), 4
                                    );
                                }
                            }

                            if (in_video_strl && is_fourcc(data, q, "strf") &&
                                sub.data + 20 <= sub.end) {
                                width = read_u32(data, sub.data + 4);
                                height = read_u32(data, sub.data + 8);
                            }

                            q = sub.end;
                        }
                    }

                    p = item.end;
                }
            }

            if (c.data + 4 <= c.end && is_fourcc(data, c.data, "movi")) {
                movi_pos = c.data + 4;
            }
        } else {
            if (is_fourcc(data, pos, "movi")) {
                movi_pos = c.data;
            }
        }

        pos = c.end;
    }

    if (!have_video_stream || codec_fourcc.empty() || movi_pos == data.size()) {
        return false;
    }

    const bool is_h26x = codec_fourcc == "H264" || codec_fourcc == "X264" ||
                         codec_fourcc == "h264" || codec_fourcc == "HEVC" ||
                         codec_fourcc == "hvc1" || codec_fourcc == "VVC" || codec_fourcc == "vvc1";
    const bool is_vp = codec_fourcc == "VP80" || codec_fourcc == "VP90";
    const bool is_av1 = codec_fourcc == "AV01";

    if (!is_h26x && !is_vp && !is_av1) {
        return false;
    }

    out.tracks.resize(1);
    out.samples.resize(1);
    out.param_sets.resize(1);

    TrackView v;
    v.id = 1;
    v.is_video = true;
    v.codec_name = codec_fourcc;
    v.width = width;
    v.height = height;
    v.timescale = 1000;
    v.mmap_backed = src.mmap_backed();

    if (is_h26x) {
        v.codec = (codec_fourcc == "HEVC" || codec_fourcc == "hvc1")
                      ? Codec::Hevc
                      : (codec_fourcc == "VVC" || codec_fourcc == "vvc1" ? Codec::Vvc
                                                                         : Codec::Avc);
        v.framing = NalFramingMode::AnnexB;
        v.length_size = 4;
    } else if (is_vp) {
        v.codec = codec_fourcc == "VP90" ? Codec::Vp9 : Codec::Vp8;
        v.framing = NalFramingMode::RawFrame;
    } else {
        v.codec = Codec::Av1;
        v.framing = NalFramingMode::Obu;
    }

    out.tracks[0] = v;

    std::size_t p2 = movi_pos;
    std::size_t index = 0;

    while (p2 + 8 <= data.size()) {
        const Chunk c = chunk_at(data, p2);
        if (c.end <= p2) {
            break;
        }

        const bool is_video = is_fourcc(data, p2, "00dc") || is_fourcc(data, p2, "00db") ||
                              is_fourcc(data, p2, "00dw");

        const std::size_t video_size = (c.end > c.data) ? (c.end - c.data) : 0u;

        if (is_video && video_size > 0) {
            const auto frame = data.subspan(c.data, video_size);

            SampleEntry e;
            e.offset = c.data;
            e.size = video_size;
            e.pts = static_cast<std::int64_t>(index) * 1000;
            e.dts = e.pts;
            e.duration = 1000;
            e.is_key = true;

            if (is_h26x) {
                const bool starts_annex_b = frame.size() >= 4 && frame[0] == 0x00 &&
                                            frame[1] == 0x00 &&
                                            (frame[2] == 0x01 || (frame[2] == 0x00 && frame[3] == 0x01));
                e.framing = starts_annex_b ? NalFramingMode::AnnexB
                                           : NalFramingMode::LengthPrefixed;
            } else if (is_vp) {
                e.framing = NalFramingMode::RawFrame;
            } else {
                e.framing = NalFramingMode::Obu;
            }

            out.samples[0].push_back(e);
            ++index;
        }

        p2 = c.end;
    }

    out.tracks[0].sample_count = out.samples[0].size();

    return !out.samples[0].empty();
}
inline bool build_ivf(FileSource& src, IndexBuildResult& out) {
    const auto data = src.data();

    if (data.size() < 32 || std::memcmp(data.data(), "DKIF", 4) != 0) {
        return false;
    }

    const bool vp9 = data[8] == 'V' && data[9] == 'P' && data[10] == '9' && data[11] == '0';

    const std::uint16_t width = static_cast<std::uint16_t>(data[12] | (data[13] << 8));
    const std::uint16_t height = static_cast<std::uint16_t>(data[14] | (data[15] << 8));

    out.tracks.resize(1);
    out.samples.resize(1);
    out.param_sets.resize(1);

    TrackView v;
    v.id = 1;
    v.is_video = true;
    v.codec = vp9 ? Codec::Vp9 : Codec::Vp8;
    v.framing = NalFramingMode::RawFrame;
    v.codec_name = vp9 ? "VP90" : "VP80";
    v.width = width;
    v.height = height;
    v.timescale = 1000;
    v.mmap_backed = src.mmap_backed();
    out.tracks[0] = v;

    std::size_t pos = 32;
    std::size_t index = 0;

    while (pos + 12 <= data.size()) {
        const std::uint32_t size = static_cast<std::uint32_t>(data[pos]) |
                                   (static_cast<std::uint32_t>(data[pos + 1]) << 8) |
                                   (static_cast<std::uint32_t>(data[pos + 2]) << 16) |
                                   (static_cast<std::uint32_t>(data[pos + 3]) << 24);

        std::uint64_t ts = 0;
        for (int i = 0; i < 8; ++i) {
            ts |= static_cast<std::uint64_t>(data[pos + 4 + i]) << (i * 8);
        }

        const std::size_t offset = pos + 12;

        if (offset + size > data.size()) {
            break;
        }

        SampleEntry e;
        e.offset = offset;
        e.size = size;
        e.pts = static_cast<std::int64_t>(ts);
        e.dts = e.pts;
        e.duration = 0;
        e.is_key = index == 0;
        e.framing = NalFramingMode::RawFrame;
        out.samples[0].push_back(e);

        pos = offset + size;
        ++index;
    }

    out.tracks[0].sample_count = out.samples[0].size();

    return !out.samples[0].empty();
}
inline bool build_ogg(FileSource& src, IndexBuildResult& out) {
    const auto data = src.data();

    if (data.size() < 27 || data[0] != 0x4F || data[1] != 0x67 || data[2] != 0x67 ||
        data[3] != 0x53) {
        return false;
    }

    struct StreamInfo {
        std::uint32_t serial = 0;
        Codec codec = Codec::Vp8;
        bool is_video = false;
        bool found = false;
    };

    std::vector<StreamInfo> streams;
    std::size_t pos = 0;

    while (pos + 27 <= data.size()) {
        if (data[pos] != 0x4F || data[pos + 1] != 0x67 || data[pos + 2] != 0x67 ||
            data[pos + 3] != 0x53) {
            break;
        }

        const std::uint8_t header_type = data[pos + 5];
        const bool bos = (header_type & 0x02u) != 0;

        std::uint32_t serial = 0;
        for (int i = 0; i < 4; ++i) {
            serial |= static_cast<std::uint32_t>(data[pos + 14 + i]) << (i * 8);
        }

        const std::uint8_t segments = data[pos + 26];
        const std::size_t seg_table = pos + 27;
        const std::size_t data_off = seg_table + segments;

        if (seg_table + segments > data.size()) break;

        std::size_t total = 0;
        for (std::uint8_t i = 0; i < segments; ++i) {
            total += data[seg_table + i];
        }

        if (data_off + total > data.size()) {
            break;
        }

        const auto page_data = data.subspan(data_off, total);

        if (bos) {
            StreamInfo info;
            info.serial = serial;
            info.is_video = true;
            info.found = true;

            if (page_data.size() >= 5 && std::memcmp(page_data.data(), "OVP80", 5) == 0) {
                info.codec = Codec::Vp8;
            } else if (page_data.size() >= 5 && std::memcmp(page_data.data(), "OVP90", 5) == 0) {
                info.codec = Codec::Vp9;
            } else if (page_data.size() >= 4 &&
                       std::memcmp(page_data.data(), "\x81\x41\x56\x30", 4) == 0) {
                info.codec = Codec::Av1;
            } else if (page_data.size() >= 4 && page_data[0] == 0x01 && page_data[1] == 'H') {
                info.codec = Codec::Hevc;
            } else if (page_data.size() >= 2 && page_data[0] == 0x01 && page_data[1] == 0x42) {
                info.codec = Codec::Avc;
            } else {
                info.codec = Codec::Vp8;
            }

            streams.push_back(info);
        }

        pos = data_off + total;
    }

    if (streams.empty()) {
        return false;
    }

    // Use the first detected video stream.
    const StreamInfo video = streams.front();

    out.tracks.resize(1);
    out.samples.resize(1);
    out.param_sets.resize(1);

    TrackView v;
    v.id = video.serial;
    v.is_video = true;
    v.codec = video.codec;
    v.framing = (video.codec == Codec::Vp8 || video.codec == Codec::Vp9)
                    ? NalFramingMode::RawFrame
                    : (video.codec == Codec::Av1 ? NalFramingMode::Obu
                                                 : NalFramingMode::AnnexB);
    v.timescale = 1000;
    v.mmap_backed = src.mmap_backed();
    out.tracks[0] = v;

    std::size_t p2 = 0;
    std::size_t index = 0;

    while (p2 + 27 <= data.size()) {
        if (data[p2] != 0x4F || data[p2 + 1] != 0x67 || data[p2 + 2] != 0x67 ||
            data[p2 + 3] != 0x53) {
            break;
        }

        std::uint32_t serial = 0;
        for (int i = 0; i < 4; ++i) {
            serial |= static_cast<std::uint32_t>(data[p2 + 14 + i]) << (i * 8);
        }

        const std::uint8_t segments = data[p2 + 26];
        const std::size_t seg_table = p2 + 27;
        const std::size_t data_off = seg_table + segments;

        if (seg_table + segments > data.size()) break;

        std::size_t total = 0;
        for (std::uint8_t i = 0; i < segments; ++i) {
            total += data[seg_table + i];
        }

        if (data_off + total > data.size()) {
            break;
        }

        const bool bos = (data[p2 + 5] & 0x02u) != 0;

        if (serial == video.serial && !bos && total > 0) {
            SampleEntry e;
            e.offset = data_off;
            e.size = total;
            e.pts = static_cast<std::int64_t>(index) * 1000;
            e.dts = e.pts;
            e.duration = 1000;
            e.is_key = index == 0;
            e.framing = out.tracks[0].framing;
            out.samples[0].push_back(e);
            ++index;
        }

        p2 = data_off + total;
    }

    out.tracks[0].sample_count = out.samples[0].size();

    return !out.samples[0].empty();
}
inline bool build_ps(FileSource& src, IndexBuildResult& out) {
    const auto data = src.data();

    if (data.size() < 14) {
        return false;
    }

    std::size_t pos = 0;
    Codec codec = Codec::Avc;
    bool found_codec = false;
    std::vector<SampleEntry> samples;

    while (pos + 4 <= data.size()) {
        if (data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x01) {
            const std::uint8_t sid = data[pos + 3];

            if (sid == 0xBA) {
                if (pos + 14 > data.size()) {
                    break;
                }
                const std::uint8_t stuffing = data[pos + 13] & 0x07;
                pos += 14 + stuffing;
                continue;
            }

            if (sid == 0xBB) {
                if (pos + 6 > data.size()) {
                    break;
                }
                const std::uint16_t len =
                    static_cast<std::uint16_t>((data[pos + 4] << 8) | data[pos + 5]);
                pos += 6 + len;
                continue;
            }

            if (sid >= 0xE0 && sid <= 0xEF) {
                if (pos + 6 > data.size()) {
                    break;
                }

                const std::uint16_t pes_len =
                    static_cast<std::uint16_t>((data[pos + 4] << 8) | data[pos + 5]);
                const std::size_t pes_start = pos + 6;
                std::size_t pes_end = (pes_len == 0) ? data.size() : pes_start + pes_len;
                if (pes_end > data.size()) {
                    pes_end = data.size();
                }

                std::size_t payload_off = pes_start;
                if (payload_off + 3 <= pes_end) {
                    const std::uint8_t c1 = data[payload_off];
                    if ((c1 & 0xC0) == 0x80) {
                        const std::uint8_t pes_header_len = data[payload_off + 2];
                        payload_off += 3 + pes_header_len;
                    } else if ((c1 & 0xC0) == 0x40) {
                        payload_off += 3;
                    } else {
                        payload_off += 3;
                    }
                }

                if (payload_off < pes_end) {
                    const auto payload = data.subspan(payload_off, pes_end - payload_off);

                    if (!found_codec && payload.size() >= 5) {
                        if (payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
                            const std::uint8_t nal_type = payload[4] & 0x1F;
                            if (nal_type == 7) {
                                codec = Codec::Avc;
                            } else if ((payload[4] >> 1) == 32) {
                                codec = Codec::Hevc;
                            }
                        }
                        found_codec = true;
                    }

                    SampleEntry e;
                    e.offset = payload_off;
                    e.size = payload.size();
                    e.pts = static_cast<std::int64_t>(samples.size()) * 1000;
                    e.dts = e.pts;
                    e.duration = 1000;
                    e.is_key = samples.empty();
                    e.framing = NalFramingMode::AnnexB;
                    samples.push_back(e);
                }

                pos = pes_end;
                continue;
            }

            if (sid == 0xBC || sid == 0xBD || (sid >= 0xBE && sid <= 0xBF) || sid == 0xB9 ||
                sid == 0xB8 || sid == 0xB7) {
                if (pos + 6 > data.size()) {
                    break;
                }
                const std::uint16_t len =
                    static_cast<std::uint16_t>((data[pos + 4] << 8) | data[pos + 5]);
                pos += 6 + len;
                continue;
            }
        }

        ++pos;
    }

    if (samples.empty()) {
        return false;
    }

    out.tracks.resize(1);
    out.samples.resize(1);
    out.param_sets.resize(1);

    TrackView v;
    v.id = 1;
    v.is_video = true;
    v.codec = codec;
    v.framing = NalFramingMode::AnnexB;
    v.timescale = 90000;
    v.mmap_backed = src.mmap_backed();
    out.tracks[0] = v;
    out.samples[0] = std::move(samples);
    out.tracks[0].sample_count = out.samples[0].size();

    return true;
}

}  // namespace detail
}  // namespace demux
}  // namespace bs
