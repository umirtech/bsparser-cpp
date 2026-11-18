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
    /* Additional video codecs the parser cannot decode but the demuxer
     * can still identify and expose to callers. */
    if (f == "mp4v" || f == "MP4V" || f == "divx" || f == "DIVX" || f == "XVID") return Codec::Mpeg4Visual;
    if (f == "mp1v" || f == "mpeg" ) return Codec::Mpeg1Video;
    if (f == "mp2v" || f == "mpeg2") return Codec::Mpeg2Video;
    if (f == "s263" || f == "h263") return Codec::H263;
    if (f == "theora") return Codec::Theora;
    if (f == "apch" || f == "apcn" || f == "apco" || f == "apcs") return Codec::ProRes;
    if (f == "AVdn" || f == "dnxhd") return Codec::DnvHd;
    if (f == "wmv3" || f == "WVC1") return Codec::Vc1;
    if (f == "avs2") return Codec::Avs2;
    /* Audio. */
    if (f == "mp4a") return Codec::Aac;
    if (f == ".mp3") return Codec::Mp3;
    if (f == "Opus" || f == "opus") return Codec::Opus;
    if (f == "fLaC") return Codec::Flac;
    if (f == "ac-3") return Codec::Ac3;
    if (f == "ec-3") return Codec::Ec3;
    if (f == "dtsc" || f == "dtse" || f == "dtsl") return Codec::Dts;
    if (f == "sowt" || f == "twos") return Codec::Pcm16;
    if (f == "in24" || f == "in32") return Codec::Pcm32;
    if (f == "fl32" || f == "fl64") return Codec::PcmFloat32;
    /* Subtitle. */
    if (f == "tx3g") return Codec::Tx3g;
    return Codec::Unknown;
}

/*
 * AVI records the codec in the stream header's fccHandler.  Its vocabulary
 * differs from ISO-BMFF (e.g. "h264" rather than "avc1", "WMV3" rather
 * than "wmv3"), so map the AVI spellings explicitly and fall back to the
 * generic fourcc table for everything else.  Unrecognised fourccs stay
 * Codec::Unknown — the caller still stores the raw fourcc in codec_name.
 */
inline Codec avi_codec_from_fourcc(const std::string& f) {
    if (f == "H264" || f == "h264" || f == "X264" || f == "x264" ||
        f == "avc1" || f == "AVC1" || f == "avc3") return Codec::Avc;
    if (f == "HEVC" || f == "hevc" || f == "hvc1" || f == "hev1" ||
        f == "HVC1") return Codec::Hevc;
    if (f == "VVC" || f == "vvc" || f == "vvc1" || f == "vvi1") return Codec::Vvc;
    if (f == "VP80" || f == "vp80" || f == "VP08" || f == "vp08") return Codec::Vp8;
    if (f == "VP90" || f == "vp90" || f == "VP09" || f == "vp09") return Codec::Vp9;
    if (f == "AV01" || f == "av01" || f == "AV1 ") return Codec::Av1;

    if (f == "XVID" || f == "xvid" || f == "DIVX" || f == "divx" ||
        f == "MP4V" || f == "mp4v" || f == "DX50" || f == "dx50" ||
        f == "FMP4") return Codec::Mpeg4Visual;
    if (f == "MPG1" || f == "mpg1" || f == "MPEG" || f == "mpeg" ||
        f == "mp1v") return Codec::Mpeg1Video;
    if (f == "MPG2" || f == "mpg2" || f == "MPEG2" || f == "mpeg2" ||
        f == "mp2v") return Codec::Mpeg2Video;
    if (f == "H263" || f == "h263" || f == "S263" || f == "s263" ||
        f == "I263") return Codec::H263;
    if (f == "WMV3" || f == "wmv3" || f == "WVC1") return Codec::Vc1;
    if (f == "APCH" || f == "apch" || f == "APCN" || f == "apcn" ||
        f == "APCO" || f == "apco" || f == "APCS" || f == "apcs") return Codec::ProRes;
    if (f == "AVdn" || f == "AVDN" || f == "dnxhd" || f == "DNXH") return Codec::DnvHd;
    if (f == "AVS2" || f == "avs2") return Codec::Avs2;
    if (f == "THEO" || f == "theora") return Codec::Theora;

    /* Audio fourccs that can appear in an AVI 'auds' stream. */
    if (f == "mp4a" || f == "MP4A") return Codec::Aac;
    if (f == ".mp3" || f == "MP3 " || f == "mp3 ") return Codec::Mp3;
    if (f == "Opus" || f == "opus") return Codec::Opus;
    if (f == "fLaC") return Codec::Flac;
    if (f == "ac-3" || f == "AC-3") return Codec::Ac3;
    if (f == "ec-3" || f == "EC-3") return Codec::Ec3;
    if (f == "dtsc" || f == "dtse" || f == "dtsl") return Codec::Dts;
    if (f == "sowt" || f == "twos") return Codec::Pcm16;
    if (f == "in24") return Codec::Pcm24;
    if (f == "in32") return Codec::Pcm32;
    if (f == "fl32" || f == "fl64") {
        return f == "fl64" ? Codec::PcmFloat64 : Codec::PcmFloat32;
    }
    return Codec::Unknown;
}

/* A fourcc made only of NULs / blanks carries no identity. */
inline bool fourcc_is_blank(const std::string& f) {
    if (f.empty()) return true;
    for (char c : f) {
        if (c != '\0' && c != ' ') return false;
    }
    return true;
}

/* True when every byte is a visible ASCII character. */
inline bool fourcc_is_printable(const std::string& f) {
    for (char c : f) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20u || u > 0x7Eu) return false;
    }
    return true;
}

/* Render an arbitrary byte signature as a short printable identifier. */
inline std::string printable_signature(std::span<const std::uint8_t> d, std::size_t max_len = 16u) {
    std::string s;
    const std::size_t n = std::min(d.size(), max_len);
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t c = d[i];
        s.push_back(c >= 0x20u && c <= 0x7Eu ? static_cast<char>(c) : '?');
    }
    return s;
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
    bool subtitle = false;
    std::string subtitle_codec;
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
            pt.subtitle = (handler == "text" || handler == "subt" || handler == "sbtl");

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
                } else if (pt.view.codec == Codec::Unknown ||
                           (!is_parser_supported(pt.view.codec) &&
                            is_video_codec(pt.view.codec))) {
                    // Container-packed video (MPEG-4, MJPEG, etc.) is not
                    // NAL-framed: expose whole samples as RawFrame.
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
                    pt.view.codec = pt.audio_entry.codec; // unified identity
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
            } else if (pt.subtitle) {
                pt.view.is_subtitle = true;
                pt.view.framing = NalFramingMode::RawFrame;
                pt.subtitle_codec = "tx3g";
                pt.view.subtitle_codec = pt.subtitle_codec;
                pt.view.codec = Codec::Tx3g;
                pt.view.codec_name = "tx3g";
            }
        }

        // Skip non-media tracks (hint, metadata, etc.) that are not video,
        // audio or subtitle.
        if (pt.video || pt.audio || pt.subtitle) {
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
            const std::uint8_t version = data[b.data];
            const std::uint32_t count = read_u32(data, b.data + 4);
            std::size_t p = b.data + 8;
            for (std::uint32_t i = 0; i < count && p + 8 <= b.end; ++i) {
                const std::uint32_t c = read_u32(data, p);
                // ISO-BMFF 8.6.6: version 0 = unsigned offset, version 1 = signed.
                const std::int64_t off = version == 1u
                    ? static_cast<std::int64_t>(static_cast<std::int32_t>(read_u32(data, p + 4)))
                    : static_cast<std::int64_t>(read_u32(data, p + 4));
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
        } else if (tracks[i].audio || tracks[i].subtitle) {
            // Audio and subtitle samples use the same stsz/stco/stts tables;
            // no parameter sets are prepended and framing stays RawFrame.
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
inline constexpr std::uint64_t kTimecodeScale = 0x2AD7B1ull;
inline constexpr std::uint64_t kChapters = 0x1043A770ull;
inline constexpr std::uint64_t kEditionEntry = 0x45B9ull;
inline constexpr std::uint64_t kChapterAtom = 0xB6ull;
inline constexpr std::uint64_t kChapterTimeStart = 0x91ull;
inline constexpr std::uint64_t kChapterDisplay = 0x80ull;
inline constexpr std::uint64_t kChapterString = 0x85ull;
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

/* Parse a Matroska Chapters element, appending to out.chapters. */
inline void parse_chapters(std::span<const std::uint8_t> d, ContainerMetadata& out) {
    std::size_t p = 0;
    std::uint64_t id = 0;
    std::span<const std::uint8_t> payload;
    while (p < d.size()) {
        if (!next_ebml(d, p, id, payload)) break;
        if (id != kEditionEntry) continue;
        std::size_t e = 0;
        std::uint64_t eid = 0;
        std::span<const std::uint8_t> edition;
        while (e < payload.size()) {
            if (!next_ebml(payload, e, eid, edition)) break;
            if (eid != kChapterAtom) continue;
            ContainerMetadata::Chapter chapter;
            std::size_t a = 0;
            std::uint64_t aid = 0;
            std::span<const std::uint8_t> field;
            while (a < edition.size()) {
                if (!next_ebml(edition, a, aid, field)) break;
                if (aid == kChapterTimeStart) {
                    std::uint64_t t = 0;
                    for (std::size_t i = 0; i < field.size() && i < 8; ++i) {
                        t = (t << 8) | field[i];
                    }
                    chapter.time = static_cast<std::int64_t>(t);
                } else if (aid == kChapterDisplay && chapter.title.empty()) {
                    std::size_t c = 0;
                    std::uint64_t cid = 0;
                    std::span<const std::uint8_t> cf;
                    while (c < field.size()) {
                        if (!next_ebml(field, c, cid, cf)) break;
                        if (cid == kChapterString) chapter.title = as_text(cf);
                    }
                }
            }
            if (chapter.time != 0 || !chapter.title.empty()) {
                out.chapters.push_back(chapter);
            }
        }
    }
}

/* Matroska TimecodeScale in nanoseconds (default 1 ms). */
inline std::uint64_t parse_timecode_scale(std::span<const std::uint8_t> d) {
    std::size_t p = 0;
    std::uint64_t id = 0;
    std::span<const std::uint8_t> payload;
    while (p < d.size()) {
        if (!next_ebml(d, p, id, payload)) {
            break;
        }
        if (id == kTimecodeScale) {
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < payload.size() && i < 8; ++i) {
                value = (value << 8) | payload[i];
            }
            if (value != 0u && value <= 1000000000ull) {
                return value;
            }
        }
    }
    return 1000000u; // default 1 ms
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

/* One frame of a (possibly laced) Block.  `time_delta` is added to the
 * Block's own relative timecode. */
struct LacedFrame {
    std::span<const std::uint8_t> data;
    std::int16_t time_delta = 0;
};

/* Split a Block payload into its frames according to flags bits 0..1.
 * Returns a single frame for unlaced blocks. */
inline std::vector<LacedFrame> mkv_decode_lacing(
    std::span<const std::uint8_t> frame, std::uint8_t flags
) {
    std::vector<LacedFrame> result;
    if (frame.empty()) {
        return result;
    }

    const unsigned lacing = (flags >> 1u) & 3u;
    if (lacing == 0u) {
        result.push_back({frame, 0});
        return result;
    }

    std::size_t p = 0;
    const std::size_t count = static_cast<std::size_t>(frame[p++]) + 1u;
    if (count > 256u || p >= frame.size()) {
        return result;
    }

    std::vector<std::size_t> sizes(count, 0u);
    if (lacing == 1u) { // Xiph-style: byte runs summing to each frame size.
        for (std::size_t i = 0; i + 1u < count; ++i) {
            std::size_t sz = 0;
            while (p < frame.size()) {
                const std::uint8_t b = frame[p++];
                sz += b;
                if (b != 0xFFu) {
                    break;
                }
            }
            sizes[i] = sz;
        }
    } else {
        // Fixed-size and EBML lacing both use vints; EBML lacing stores a
        // size *delta* relative to the previous frame.
        std::size_t prev = 0;
        for (std::size_t i = 0; i + 1u < count; ++i) {
            if (p >= frame.size()) {
                return result;
            }
            const std::uint8_t first = frame[p++];
            unsigned bytes = 1;
            std::uint8_t marker = 0x80u;
            while (bytes < 8u && (first & marker) == 0) {
                marker >>= 1;
                ++bytes;
            }
            std::size_t value = first & (marker - 1u);
            for (unsigned k = 1u; k < bytes && p < frame.size(); ++k) {
                value = (value << 8) | frame[p++];
            }
            prev = (lacing == 2u) ? value : prev + value;
            sizes[i] = prev;
        }
    }

    std::size_t off = p;
    std::size_t cumulative = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t sz = (i + 1u < count)
            ? sizes[i]
            : (frame.size() > off ? frame.size() - off : 0u);
        if (sz == 0u || off + sz > frame.size()) {
            return result;
        }
        LacedFrame lf;
        lf.data = frame.subspan(off, sz);
        // Xiph lacing advances each frame's timestamp by the accumulated
        // byte count; fixed/EBML laced frames share the Block timecode.
        lf.time_delta = (lacing == 1u)
            ? static_cast<std::int16_t>(std::min<std::size_t>(cumulative, 32767u))
            : 0;
        result.push_back(lf);
        off += sz;
        cumulative += sz;
    }
    return result;
}

/* True when an EBML size vint carries the all-ones "unknown size" value. */
inline bool mkv_size_unknown(std::uint64_t value, std::uint64_t vint_len) noexcept {
    if (vint_len == 0 || vint_len > 8) {
        return false;
    }
    const std::uint64_t max = (vint_len == 8)
        ? 0x00FFFFFFFFFFFFFFull
        : ((std::uint64_t{1} << (7u * vint_len)) - 1u);
    return value == max;
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
    std::uint64_t timecode_scale_ns = 1000000u; // default 1 ms

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
                timecode_scale_ns = mkv_index::parse_timecode_scale(element);
            } else if (id == mkv_index::kTags) {
                mkv_index::parse_tags(element, out.metadata);
            } else if (id == mkv_index::kChapters) {
                mkv_index::parse_chapters(element, out.metadata);
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
        v.timescale = static_cast<std::uint32_t>(1000000000ull / timecode_scale_ns);
        v.mmap_backed = src.mmap_backed();

        if (t.track_type == 2u) {
            // Audio track.
            v.is_audio = true;
            v.framing = NalFramingMode::RawFrame;
            v.codec = Codec::Unknown;
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
            } else if (t.codec_id == "A_MPEG/L3" || t.codec_id == "A_MPEG/L2") {
                v.audio_codec = Codec::Mp3;
            } else if (t.codec_id.rfind("A_AAC", 0) == 0) {
                v.audio_codec = Codec::Aac;
            }
            // Other CodecIDs (A_TTA1, A_REAL/COOK, A_AAC/MPEG4/LC) keep
            // Unknown but remain identified by codec_name / subtitle_codec.
            v.codec = v.audio_codec; // unified concrete codec identity
        } else if (t.track_type == 0x11u || t.track_type == 0x12u) {
            v.is_subtitle = true;
            v.framing = NalFramingMode::RawFrame;
            v.subtitle_codec = t.codec_id; // e.g. S_TEXT/UTF8, S_TEXT/WEBVTT
            v.codec = Codec::Unknown;
            if (t.codec_id == "S_TEXT/UTF8" || t.codec_id == "S_TEXT/ASCII") {
                v.codec = Codec::Subrip;
            } else if (t.codec_id == "S_TEXT/WEBVTT") {
                v.codec = Codec::Webvtt;
            }
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
            // hvcC: lengthSizeMinusOne is the low 2 bits of byte 21.
            v.length_size = t.codec_private.size() > 21u
                                ? static_cast<unsigned>(t.codec_private[21] & 3u) + 1u
                                : 4u;
        } else if (t.codec_id == "V_VVC") {
            v.is_video = true;
            v.codec = Codec::Vvc;
            v.framing = NalFramingMode::LengthPrefixed;
            // vvcC: byte 1 = reserved(5)|lsm1(2)|ptl_present_flag(1).
            v.length_size = t.codec_private.size() > 1u
                                ? static_cast<unsigned>((t.codec_private[1] >> 1) & 3u) + 1u
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
        } else if (t.track_type == 1u || t.codec_id.rfind("V_", 0) == 0) {
            // Unknown video codec: still expose the track with a concrete
            // CodecID name and whole-sample framing.  V_MS/VFW/FOURCC carries
            // a BITMAPINFOHEADER in CodecPrivate with biCompression at +16.
            v.is_video = true;
            v.codec = Codec::Unknown;
            v.framing = NalFramingMode::RawFrame;
            if (t.codec_id == "V_MS/VFW/FOURCC" && t.codec_private.size() >= 20u) {
                const std::string fourcc(
                    reinterpret_cast<const char*>(t.codec_private.data() + 16), 4
                );
                v.codec = avi_codec_from_fourcc(fourcc);
                v.width = static_cast<std::uint32_t>(
                    static_cast<std::uint32_t>(t.codec_private[4]) |
                    (static_cast<std::uint32_t>(t.codec_private[5]) << 8) |
                    (static_cast<std::uint32_t>(t.codec_private[6]) << 16) |
                    (static_cast<std::uint32_t>(t.codec_private[7]) << 24));
                v.height = static_cast<std::uint32_t>(
                    static_cast<std::uint32_t>(t.codec_private[8]) |
                    (static_cast<std::uint32_t>(t.codec_private[9]) << 8) |
                    (static_cast<std::uint32_t>(t.codec_private[10]) << 16) |
                    (static_cast<std::uint32_t>(t.codec_private[11]) << 24));
                v.codec_name = fourcc_is_blank(fourcc) ? t.codec_id : fourcc;
            } else {
                v.codec_name = t.codec_id;
            }
        } else {
            // Genuinely non-media track entry: skip it from the index so it
            // does not become a phantom default track.
            out.tracks[i] = TrackView{};
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
        std::uint64_t size_len = 0;

        if (!mkv::detail::read_vint(data, p, id, id_len, true)) {
            break;
        }
        if (!mkv::detail::read_vint(data, p, size, size_len, false)) {
            break;
        }
        bool unknown_size = mkv_index::mkv_size_unknown(size, size_len);
        if (unknown_size) {
            size = seg_end - p;
        }
        if (size > seg_end - p || size == 0) {
            break;
        }

        const std::size_t element_offset = p;
        const auto element = data.subspan(p, static_cast<std::size_t>(size));

        if (id == mkv::detail::kCluster) {
            std::int64_t cluster_time = 0;
            std::size_t q = 0;
            bool next_cluster = false;

            while (q + 4 <= element.size()) {
                const std::size_t child_id_offset = q;
                std::uint64_t cid = 0;
                std::uint64_t csize = 0;
                std::uint64_t clen = 0;
                std::uint64_t csize_len = 0;

                if (!mkv::detail::read_vint(element, q, cid, clen, true)) {
                    break;
                }
                if (!mkv::detail::read_vint(element, q, csize, csize_len, false)) {
                    break;
                }
                if (unknown_size && cid == mkv::detail::kCluster) {
                    // The next Cluster starts here; resume the outer loop.
                    p = element_offset + child_id_offset;
                    next_cluster = true;
                    break;
                }
                if (mkv_index::mkv_size_unknown(csize, csize_len)) {
                    csize = element.size() - q;
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
                bool from_block_group = false;
                bool has_reference = false;
                std::int64_t block_duration = 0;

                if (cid == mkv::detail::kSimpleBlock) {
                    block_data = child;
                } else if (cid == mkv::detail::kBlockGroup) {
                    from_block_group = true;
                    std::size_t b = 0;
                    while (b + 2 <= child.size()) {
                        std::uint64_t bid = 0;
                        std::uint64_t bsize = 0;
                        std::uint64_t blen = 0;
                        std::uint64_t bsize_len = 0;

                        if (!mkv::detail::read_vint(child, b, bid, blen, true) ||
                            !mkv::detail::read_vint(child, b, bsize, bsize_len, false)) {
                            break;
                        }
                        if (mkv_index::mkv_size_unknown(bsize, bsize_len)) {
                            bsize = child.size() - b;
                        }
                        if (b + bsize > child.size()) {
                            break;
                        }
                        const auto field = child.subspan(b, static_cast<std::size_t>(bsize));
                        if (bid == mkv::detail::kBlock && block_data.empty()) {
                            block_data = field;
                        } else if (bid == 0xFBu) { // ReferenceBlock
                            has_reference = true;
                        } else if (bid == 0x9Bu) { // BlockDuration
                            std::uint64_t v = 0;
                            for (std::uint8_t byte : field) {
                                v = (v << 8) | byte;
                            }
                            block_duration = static_cast<std::int64_t>(v);
                        }
                        b += static_cast<std::size_t>(bsize);
                    }
                }

                if (!block_data.empty()) {
                    std::uint64_t track_number = 0;
                    std::int16_t block_time = 0;
                    std::uint8_t flags = 0;
                    std::span<const std::uint8_t> payload;

                    if (mkv_index::parse_block_header(
                            block_data, track_number, block_time, flags, payload
                        )) {
                        for (std::size_t i = 0; i < tracks.size(); ++i) {
                            if (static_cast<std::uint64_t>(tracks[i].number) != track_number) {
                                continue;
                            }

                            for (const auto& lf : mkv_index::mkv_decode_lacing(payload, flags)) {
                                SampleEntry e;
                                e.offset = static_cast<std::size_t>(lf.data.data() - data.data());
                                e.size = lf.data.size();
                                e.pts = cluster_time + block_time + lf.time_delta;
                                e.dts = e.pts;
                                e.duration = block_duration;
                                // SimpleBlock flags its keyframes directly; a
                                // BlockGroup Block is a keyframe iff it has no
                                // ReferenceBlock.
                                e.is_key = from_block_group ? !has_reference
                                                            : ((flags & 0x80u) != 0);

                                if (out.tracks[i].is_video &&
                                    (out.tracks[i].codec == Codec::Avc ||
                                     out.tracks[i].codec == Codec::Hevc ||
                                     out.tracks[i].codec == Codec::Vvc)) {
                                    const bool starts_annex_b =
                                        lf.data.size() >= 4 && lf.data[0] == 0x00 &&
                                        lf.data[1] == 0x00 &&
                                        (lf.data[2] == 0x01 ||
                                         (lf.data[3] == 0x01 && lf.data[2] == 0x00));
                                    const bool annex_b =
                                        starts_annex_b &&
                                        !es::fully_length_prefixed(lf.data, out.tracks[i].length_size);
                                    e.framing = annex_b ? NalFramingMode::AnnexB
                                                        : NalFramingMode::LengthPrefixed;
                                } else {
                                    e.framing = out.tracks[i].framing;
                                }

                                out.samples[i].push_back(e);
                            }
                            break;
                        }
                    }
                }

                q += csize;
            }
            if (next_cluster) {
                continue;
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
    bool is_video = true;
    AudioCodec audio_codec = AudioCodec::Unknown;
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

                const std::uint16_t es_info_length =
                    static_cast<std::uint16_t>(((payload[p + 3] & 0x0Fu) << 8) | payload[p + 4]);
                TsStream stream;
                stream.pid = elementary_pid;
                stream.stream_type = stream_type;
                if (stream_type == 0x1Bu || stream_type == 0x24u || stream_type == 0x33u) {
                    stream.is_video = true;
                } else if (stream_type == 0x0Fu) {
                    stream.is_video = false;
                    stream.audio_codec = AudioCodec::Aac;
                } else if (stream_type == 0x03u || stream_type == 0x04u) {
                    stream.is_video = false;
                    stream.audio_codec = AudioCodec::Mp3;
                } else if (stream_type == 0x81u) {
                    stream.is_video = false;
                    stream.audio_codec = AudioCodec::Ac3;
                } else if (stream_type == 0x06u) {
                    // Private data: often AC-3/E-AC-3 or AV1; keep as video
                    // only when a registration descriptor says AV1.  Treated
                    // as audio otherwise (the common broadcast case).
                    stream.is_video = false;
                    stream.audio_codec = AudioCodec::Ac3;
                } else {
                    // Ignore other stream types (teletext, subtitles, data).
                    p += 5 + es_info_length;
                    continue;
                }

                bool exists = false;
                for (const auto& s : out) {
                    if (s.pid == elementary_pid) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    out.push_back(stream);
                }

                p += 5 + es_info_length;
            }
        }
    }

    return out;
}


/* Decode a 33-bit PTS/DTS field starting at `p`. */
inline std::int64_t decode_pes_timestamp(const std::uint8_t* p) {
    const std::uint64_t v = (static_cast<std::uint64_t>(p[0] & 0x0E) << 29) |
                            (static_cast<std::uint64_t>(p[1]) << 22) |
                            (static_cast<std::uint64_t>(p[2] & 0xFE) << 14) |
                            (static_cast<std::uint64_t>(p[3]) << 7) |
                            (static_cast<std::uint64_t>(p[4]) >> 1);
    return static_cast<std::int64_t>(v);
}

/* Parse PTS and (when present) DTS from a PES packet payload. */
inline bool parse_pes_timing(
    std::span<const std::uint8_t> payload, std::int64_t& pts, std::int64_t& dts
) {
    if (payload.size() < 14) {
        return false;
    }
    if (payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01) {
        return false;
    }
    const std::uint8_t flags = payload[7];
    if ((flags & 0x80u) == 0) {
        return false;
    }

    pts = decode_pes_timestamp(payload.data() + 9);
    dts = pts;
    if ((flags & 0x40u) != 0 && payload.size() >= 19) {
        dts = decode_pes_timestamp(payload.data() + 14);
    }
    return true;
}

inline bool parse_pes_pts(std::span<const std::uint8_t> payload, std::int64_t& pts) {
    std::int64_t dts = 0;
    return parse_pes_timing(payload, pts, dts);
}

/* Byte offset of the first ES byte of a PES packet: 9 + the
 * PES_header_data_length field.  Returns 0 for malformed headers. */
inline std::size_t pes_payload_offset(std::span<const std::uint8_t> payload) {
    if (payload.size() < 9) {
        return 0;
    }
    const std::size_t offset = 9u + payload[8];
    return offset < payload.size() ? offset : 0;
}

/* Heuristic keyframe detection from the first NAL of a sample. */
inline bool ts_sample_is_key(
    std::span<const std::uint8_t> data, std::size_t offset, std::size_t size, Codec codec,
    bool is_audio
) {
    if (is_audio || size == 0 || offset + size > data.size()) {
        return true; // every audio access unit is independently decodable
    }
    const auto p = data.subspan(offset, size);
    std::size_t i = 0;
    while (i + 3 < p.size() && !(p[i] == 0 && p[i+1] == 0 && p[i+2] == 1)) {
        ++i;
    }
    std::size_t nal = (i + 3 < p.size()) ? i + 3 : 0;
    if (i + 4 < p.size() && p[i+3] == 0 && p[i+4] == 1) {
        nal = i + 4;
    }
    if (nal >= p.size()) {
        return false;
    }
    if (codec == Codec::Hevc) {
        const unsigned type = (p[nal] >> 1) & 0x3Fu;
        return type == 19u || type == 20u || type == 21u; // IDR_W_RADL/IDR_N_LP/CRA
    }
    if (codec == Codec::Avc) {
        return (p[nal] & 0x1Fu) == 5u; // IDR
    }
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
        v.is_video = streams[s].is_video;
        v.is_audio = !streams[s].is_video;
        v.timescale = 90000;
        v.mmap_backed = src.mmap_backed();

        if (v.is_audio) {
            v.audio_codec = streams[s].audio_codec;
            v.framing = NalFramingMode::RawFrame;
            switch (v.audio_codec) {
                case AudioCodec::Aac: v.codec_name = "mp4a"; break;
                case AudioCodec::Mp3: v.codec_name = ".mp3"; break;
                case AudioCodec::Ac3: v.codec_name = "ac-3"; break;
                default: v.codec_name = "audio"; break;
            }
        } else {
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
            v.codec_name = codec_name(v.codec);
        }

        out.tracks[s] = v;

        std::vector<SampleEntry>& samples = out.samples[s];

        bool open = false;
        std::size_t start = 0;
        std::size_t size = 0;
        std::int64_t first_pts = 0;
        std::int64_t first_dts = 0;
        bool have_pts = false;

        const auto emit = [&]() {
            if (!open || size == 0 || start + size > data.size()) {
                return;
            }
            SampleEntry e;
            e.offset = start;
            e.size = size;
            e.pts = have_pts ? first_pts : static_cast<std::int64_t>(samples.size()) * 1000;
            e.dts = have_pts ? first_dts : e.pts;
            e.duration = 0;
            e.is_key = ts_index::ts_sample_is_key(
                data, start, size, out.tracks[s].codec, out.tracks[s].is_audio
            );
            e.framing = out.tracks[s].framing;
            samples.push_back(e);
        };

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
                emit();

                open = true;
                size = 0;
                have_pts = false;

                // PES header: 6-byte prefix + optional header data.  The
                // old heuristic scanned for 00 00 01 inside the payload and
                // dropped the first packet of multi-packet PES (fatal for
                // audio).  Use the spec-defined length instead.
                const std::size_t skip = ts_index::pes_payload_offset(payload);
                start = i * ts::detail::kPacketSize +
                       static_cast<std::size_t>(payload.data() - packet.data()) + skip;
                size = payload.size() - skip;

                std::int64_t pts = 0;
                std::int64_t dts = 0;
                if (ts_index::parse_pes_timing(payload, pts, dts)) {
                    first_pts = pts;
                    first_dts = dts;
                    have_pts = true;
                }
            } else if (open) {
                size += payload.size();
            }
        }

        emit();

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

    enum class Kind { None, Avc, Hevc, Av1, Vp8, Vp9, H263, Screen };
    Kind kind = Kind::None;

    /* Each distinct video codec gets its own track.  The old single `kind`
     * variable collapsed mixed-codec FLV files into one mislabeled track. */
    struct VideoStream {
        Kind kind = Kind::None;
        std::vector<SampleEntry> samples;
        std::vector<std::uint8_t> param_sets;
    };
    std::vector<VideoStream> videos;

    const auto video_for = [&](Kind k) -> VideoStream& {
        for (auto& v : videos) {
            if (v.kind == k) {
                return v;
            }
        }
        videos.push_back(VideoStream{});
        videos.back().kind = k;
        return videos.back();
    };

    /* Audio is collected independently; it becomes a second track. */
    struct AudioInfo {
        Codec codec = Codec::Unknown;
        std::string codec_name;
        std::uint32_t sample_rate = 0;
        std::uint16_t channels = 0;
        std::uint16_t bits_per_sample = 0;
        std::vector<std::uint8_t> config;
        bool found = false;
    };
    AudioInfo audio;
    std::vector<SampleEntry> audio_samples;

    static constexpr std::uint32_t kFlvRates[4] = {5500u, 11025u, 22050u, 44100u};
    static constexpr std::uint32_t kAacRates[13] = {
        96000u, 88200u, 64000u, 48000u, 44100u, 32000u, 24000u,
        22050u, 16000u, 12000u, 11025u, 8000u, 7350u
    };

    while (pos + 11 <= data.size()) {
        const std::uint8_t tag_type = data[pos];
        const std::uint32_t tag_size = read_u24(data, pos + 1);

        if (pos + 11 + tag_size > data.size()) {
            break;
        }

        const auto tag = data.subspan(pos + 11, tag_size);
        const std::int64_t ts = static_cast<std::int64_t>(
            (static_cast<std::uint32_t>(data[pos + 4]) << 16) |
            (static_cast<std::uint32_t>(data[pos + 5]) << 8) |
            static_cast<std::uint32_t>(data[pos + 6]) |
            (static_cast<std::uint32_t>(data[pos + 7]) << 24)
        );

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
                    video_for(kind).param_sets =
                        flv_index::extract_flv_param_sets(body, codec_id);
                } else if (packet_type == 1) {
                    kind = codec_id == 7 ? Kind::Avc
                                         : (codec_id == 12 ? Kind::Hevc : Kind::Av1);

                    // AVC/HEVC tag header: byte 0 = FrameType<<4 | CodecID,
                    // bytes 2..4 = signed 24-bit composition time offset.
                    std::int64_t cto = 0;
                    if ((codec_id == 7 || codec_id == 12) && tag.size() >= 5) {
                        std::int32_t v = (static_cast<std::int32_t>(tag[2]) << 16) |
                                         (static_cast<std::int32_t>(tag[3]) << 8) |
                                         static_cast<std::int32_t>(tag[4]);
                        if (v & 0x00800000) {
                            v |= static_cast<std::int32_t>(0xFF000000u);
                        }
                        cto = v;
                    }

                    SampleEntry e;
                    e.offset = static_cast<std::size_t>(body.data() - data.data());
                    e.size = body.size();
                    e.pts = ts + cto;
                    e.dts = ts;
                    e.duration = 0;
                    e.is_key = (tag[0] >> 4) == 1u; // FrameType 1 = keyframe
                    e.framing = kind == Kind::Av1 ? NalFramingMode::Obu
                                                  : NalFramingMode::LengthPrefixed;
                    video_for(kind).samples.push_back(e);
                }
            } else if (codec_id == 10 || codec_id == 11) {
                kind = codec_id == 10 ? Kind::Vp8 : Kind::Vp9;

                const auto body = tag.subspan(1);

                SampleEntry e;
                e.offset = static_cast<std::size_t>(body.data() - data.data());
                e.size = body.size();
                e.pts = ts;
                e.dts = ts;
                e.duration = 0;
                e.is_key = (tag[0] >> 4) == 1u;
                e.framing = NalFramingMode::RawFrame;
                video_for(kind).samples.push_back(e);
            } else if (codec_id == 2 || codec_id == 3) {
                /* 2 = H.263, 3 = Screen video (not parser-supported). */
                kind = codec_id == 2 ? Kind::H263 : Kind::Screen;
                const auto body = tag.subspan(1);

                SampleEntry e;
                e.offset = static_cast<std::size_t>(body.data() - data.data());
                e.size = body.size();
                e.pts = ts;
                e.dts = ts;
                e.duration = 0;
                e.is_key = (tag[0] >> 4) == 1u;
                e.framing = NalFramingMode::RawFrame;
                video_for(kind).samples.push_back(e);
            }
        } else if (tag_type == 8 && tag_size >= 1) {
            const std::uint8_t format = static_cast<std::uint8_t>(tag[0] >> 4);
            const std::uint8_t rate_idx = static_cast<std::uint8_t>((tag[0] >> 2) & 0x03u);

            if (!audio.found) {
                audio.found = true;
                switch (format) {
                    case 0:
                    case 3:
                        audio.codec = Codec::Pcm16;
                        audio.codec_name = "pcm_s16";
                        break;
                    case 2:
                    case 14:
                        audio.codec = Codec::Mp3;
                        audio.codec_name = ".mp3";
                        break;
                    case 10:
                        audio.codec = Codec::Aac;
                        audio.codec_name = "mp4a";
                        break;
                    default:
                        audio.codec = Codec::Unknown;
                        audio.codec_name = "audio";
                        break;
                }
                audio.sample_rate = kFlvRates[rate_idx];
                audio.channels = (tag[0] & 0x01u) != 0u ? 2u : 1u;
                audio.bits_per_sample = (tag[0] & 0x02u) != 0u ? 16u : 8u;
                if (audio.codec == Codec::Pcm16) audio.bits_per_sample = 16u;
            }

            const bool aac_sequence_header =
                (format == 10 && tag.size() >= 2 && tag[1] == 0);

            if (aac_sequence_header) {
                /* AAC AudioSpecificConfig: object type (5) + freq index (4)
                 * + channel config (4). */
                const auto asc = tag.subspan(2);
                if (asc.size() >= 2) {
                    const unsigned freq_idx =
                        (static_cast<unsigned>(asc[0] & 0x07u) << 1) |
                        static_cast<unsigned>(asc[1] >> 7);
                    if (freq_idx < 13u) {
                        audio.sample_rate = kAacRates[freq_idx];
                    }
                    audio.channels = static_cast<std::uint16_t>((asc[1] >> 3) & 0x0Fu);
                    audio.config.assign(asc.begin(), asc.end());
                }
            } else {
                const std::size_t audio_off = (format == 10 && tag.size() >= 2) ? 2u : 1u;
                if (tag.size() > audio_off) {
                    SampleEntry e;
                    e.offset = static_cast<std::size_t>(tag.data() - data.data()) + audio_off;
                    e.size = tag.size() - audio_off;
                    e.pts = ts;
                    e.dts = ts;
                    e.duration = 0;
                    e.is_key = true;
                    e.framing = NalFramingMode::RawFrame;
                    audio_samples.push_back(e);
                }
            }
        }

        pos += 11 + tag_size + 4;
    }

    std::size_t video_count = 0;
    for (const auto& vs : videos) {
        if (vs.kind != Kind::None && (!vs.samples.empty() || !vs.param_sets.empty())) {
            ++video_count;
        }
    }
    const bool have_audio = audio.found && !audio_samples.empty();

    if (video_count == 0 && !have_audio) {
        return false;
    }

    const std::size_t track_count = video_count + (have_audio ? 1u : 0u);
    out.tracks.resize(track_count);
    out.samples.resize(track_count);
    out.param_sets.resize(track_count);

    std::size_t vi = 0;

    for (const auto& vs : videos) {
        if (vs.kind == Kind::None || (vs.samples.empty() && vs.param_sets.empty())) {
            continue;
        }
        TrackView v;
        v.id = static_cast<std::uint32_t>(vi + 1);
        v.is_video = true;
        v.timescale = 1000;
        v.mmap_backed = src.mmap_backed();

        switch (vs.kind) {
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
            case Kind::H263:
                v.codec = Codec::H263;
                v.framing = NalFramingMode::RawFrame;
                v.codec_name = "h263";
                break;
            case Kind::Screen:
                v.codec = Codec::Unknown;
                v.framing = NalFramingMode::RawFrame;
                v.codec_name = "screen video";
                break;
            default:
                break;
        }

        out.tracks[vi] = v;
        out.param_sets[vi] = vs.param_sets;

        if (!out.param_sets[vi].empty()) {
            SampleEntry e;
            e.is_param_set = true;
            e.offset = 0;
            e.size = out.param_sets[vi].size();
            e.framing = NalFramingMode::AnnexB;
            e.is_key = true;
            out.samples[vi].push_back(e);
        }

        for (const auto& e : vs.samples) {
            out.samples[vi].push_back(e);
        }
        out.tracks[vi].sample_count = out.samples[vi].size();
        ++vi;
    }

    if (have_audio) {
        TrackView a;
        a.id = static_cast<std::uint32_t>(vi + 1);
        a.is_audio = true;
        a.codec = audio.codec;
        a.audio_codec = audio.codec;
        a.codec_name = audio.codec_name;
        a.framing = NalFramingMode::RawFrame;
        a.sample_rate = audio.sample_rate;
        a.channels = audio.channels;
        a.bits_per_sample = audio.bits_per_sample != 0u
                                ? audio.bits_per_sample
                                : static_cast<std::uint16_t>(audio_bits_per_sample(audio.codec));
        a.audio_config = audio.config;
        a.timescale = 1000;
        a.mmap_backed = src.mmap_backed();

        out.tracks[vi] = a;
        for (auto& e : audio_samples) {
            out.samples[vi].push_back(std::move(e));
        }
        out.tracks[vi].sample_count = out.samples[vi].size();
    }

    return true;
}
inline bool build_avi(FileSource& src, IndexBuildResult& out) {
    using avi::detail::Chunk;
    using avi::detail::chunk_at;
    using avi::detail::is_fourcc;
    using avi::detail::read_u32;

    const auto data = src.data();

    if (data.size() < 12 || !is_fourcc(data, 0, "RIFF")) {
        return false;
    }
    const bool avi_tag = is_fourcc(data, 8, "AVI ") ||
                         (data[8] == 'A' && data[9] == 'V' && data[10] == 'I' &&
                          (data[11] == 0x19 || data[11] == 0x18));
    if (!avi_tag) {
        return false;
    }

    /* One entry per strl chunk (video or audio), in header order. */
    struct Stream {
        bool is_video = false;
        bool is_audio = false;
        std::string fourcc;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t sample_rate = 0;
        std::uint16_t channels = 0;
        std::uint16_t bits_per_sample = 0;
        std::uint16_t format_tag = 0;
    };

    std::vector<Stream> streams;
    std::size_t pos = 12;
    std::size_t movi_pos = data.size();

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
                        Stream s;
                        std::size_t q = item.data + 4;

                        while (q + 8 <= item.end) {
                            const Chunk sub = chunk_at(data, q);
                            if (sub.end <= q) {
                                break;
                            }

                            if (is_fourcc(data, q, "strh") && sub.data + 12 <= sub.end) {
                                const bool vids = is_fourcc(data, sub.data, "vids");
                                const bool auds = is_fourcc(data, sub.data, "auds");
                                if (vids || auds) {
                                    s.is_video = vids;
                                    s.is_audio = auds;
                                    s.fourcc = std::string(
                                        reinterpret_cast<const char*>(data.data() + sub.data + 4), 4
                                    );
                                }
                            }

                            if (is_fourcc(data, q, "strf")) {
                                if (s.is_video && sub.data + 20 <= sub.end) {
                                    /* BITMAPINFOHEADER: width @+4, height @+8,
                                     * biCompression @+16.  Some AVI writers
                                     * leave the strh fourcc blank; use this. */
                                    s.width = read_u32(data, sub.data + 4);
                                    s.height = read_u32(data, sub.data + 8);
                                    if (fourcc_is_blank(s.fourcc) && sub.data + 20 <= sub.end) {
                                        s.fourcc = std::string(
                                            reinterpret_cast<const char*>(data.data() + sub.data + 16),
                                            4
                                        );
                                    }
                                } else if (s.is_audio && sub.data + 16 <= sub.end) {
                                    /* WAVEFORMATEX: tag @+0, channels @+2,
                                     * samples/sec @+4, bits/sample @+14. */
                                    s.format_tag = static_cast<std::uint16_t>(
                                        data[sub.data] | (data[sub.data + 1] << 8)
                                    );
                                    s.channels = static_cast<std::uint16_t>(
                                        data[sub.data + 2] | (data[sub.data + 3] << 8)
                                    );
                                    s.sample_rate = read_u32(data, sub.data + 4);
                                    s.bits_per_sample = static_cast<std::uint16_t>(
                                        data[sub.data + 14] | (data[sub.data + 15] << 8)
                                    );
                                }
                            }

                            q = sub.end;
                        }

                        if (s.is_video || s.is_audio) {
                            streams.push_back(std::move(s));
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

    if (streams.empty() || movi_pos == data.size()) {
        return false;
    }

    const auto audio_codec_from_wave = [](const Stream& s) -> Codec {
        const Codec from_fourcc = avi_codec_from_fourcc(s.fourcc);
        if (from_fourcc != Codec::Unknown && is_audio_codec(from_fourcc)) {
            return from_fourcc;
        }
        switch (s.format_tag) {
            case 0x0001: /* WAVE_FORMAT_PCM */
                return s.bits_per_sample == 8u  ? Codec::Pcm8
                     : s.bits_per_sample == 24u ? Codec::Pcm24
                     : s.bits_per_sample == 32u ? Codec::Pcm32
                                                : Codec::Pcm16;
            case 0x0003: return Codec::PcmFloat32;
            case 0x0055: return Codec::Mp3;
            case 0x00FE:
            case 0x00FF: return Codec::Aac;
            case 0x2000: return Codec::Ac3;
            case 0x2001: return Codec::Dts;
            default: break;
        }
        return Codec::Unknown;
    };

    out.tracks.resize(streams.size());
    out.samples.resize(streams.size());
    out.param_sets.resize(streams.size());

    /* Video tracks whose samples may be NAL-formatted (Annex-B / length). */
    std::vector<bool> annexb_capable(streams.size(), false);

    for (std::size_t i = 0; i < streams.size(); ++i) {
        const Stream& s = streams[i];
        TrackView& v = out.tracks[i];
        v.id = static_cast<std::uint32_t>(i + 1);
        v.timescale = 1000;
        v.mmap_backed = src.mmap_backed();

        if (s.is_audio) {
            const Codec ac = audio_codec_from_wave(s);
            v.is_audio = true;
            v.framing = NalFramingMode::RawFrame;
            v.codec = ac;
            v.audio_codec = ac;
            v.sample_rate = s.sample_rate;
            v.channels = s.channels;
            v.bits_per_sample = s.bits_per_sample != 0u
                                    ? s.bits_per_sample
                                    : static_cast<std::uint16_t>(audio_bits_per_sample(ac));
            v.codec_name = (!fourcc_is_blank(s.fourcc) && fourcc_is_printable(s.fourcc))
                               ? s.fourcc
                               : std::string(codec_name(ac));
        } else {
            v.is_video = true;
            v.width = s.width;
            v.height = s.height;
            v.codec = avi_codec_from_fourcc(s.fourcc);
            v.codec_name = fourcc_is_blank(s.fourcc) ? std::string(codec_name(v.codec)) : s.fourcc;
            v.length_size = 4;

            if (v.codec == Codec::Av1) {
                v.framing = NalFramingMode::Obu;
            } else if (v.codec == Codec::Vp8 || v.codec == Codec::Vp9) {
                v.framing = NalFramingMode::RawFrame;
            } else if (v.codec == Codec::Avc || v.codec == Codec::Hevc ||
                       v.codec == Codec::Vvc || v.codec == Codec::Unknown) {
                v.framing = NalFramingMode::AnnexB;
                annexb_capable[i] = true;
            } else {
                /* MPEG-4 Visual, H.263, ProRes, DNxHD, VC-1, ... store
                 * complete frames; no NAL start codes to look for. */
                v.framing = NalFramingMode::RawFrame;
            }
        }
    }

    /* Prefer the chunk's stream id, but never misroute it to the wrong kind. */
    const auto select_stream = [&](bool want_video, std::size_t hint) -> std::size_t {
        if (hint < streams.size() && (want_video ? streams[hint].is_video
                                                 : streams[hint].is_audio)) {
            return hint;
        }
        for (std::size_t k = 0; k < streams.size(); ++k) {
            if (want_video ? streams[k].is_video : streams[k].is_audio) return k;
        }
        return streams.size();
    };

    std::size_t p2 = movi_pos;
    std::vector<std::size_t> counters(streams.size(), 0);

    while (p2 + 8 <= data.size()) {
        const Chunk c = chunk_at(data, p2);
        if (c.end <= p2) {
            break;
        }

        if (c.is_list) {
            /* A 'rec ' list nests ordinary media chunks inside movi. */
            if (c.data + 4 <= c.end && is_fourcc(data, c.data, "rec ")) {
                p2 = c.data + 4;
                continue;
            }
            p2 = c.end;
            continue;
        }

        const char g2 = c.fourcc[2];
        const char g3 = c.fourcc[3];
        const bool is_video_chunk = (g2 == 'd') && (g3 == 'c' || g3 == 'b' || g3 == 'w');
        const bool is_audio_chunk = (g2 == 'w') && (g3 == 'b');

        if ((is_video_chunk || is_audio_chunk) && c.size > 0 &&
            c.data <= data.size() && c.end >= c.data && c.end - c.data >= c.size) {
            std::size_t hint = streams.size();
            if (c.fourcc[0] >= '0' && c.fourcc[0] <= '9' &&
                c.fourcc[1] >= '0' && c.fourcc[1] <= '9') {
                hint = static_cast<std::size_t>((c.fourcc[0] - '0') * 10 +
                                                (c.fourcc[1] - '0'));
            }
            const std::size_t si = select_stream(is_video_chunk, hint);

            if (si < streams.size()) {
                const auto frame = data.subspan(c.data, c.size);

                SampleEntry e;
                e.offset = c.data;
                e.size = c.size;
                e.pts = static_cast<std::int64_t>(counters[si]) * 1000;
                e.dts = e.pts;
                e.duration = 1000;
                e.is_key = true;

                if (is_audio_chunk) {
                    e.framing = NalFramingMode::RawFrame;
                } else {
                    const Codec codec = out.tracks[si].codec;
                    if (codec == Codec::Av1) {
                        e.framing = NalFramingMode::Obu;
                    } else if (annexb_capable[si]) {
                        const bool starts_annex_b =
                            frame.size() >= 4 && frame[0] == 0x00 && frame[1] == 0x00 &&
                            (frame[2] == 0x01 || (frame[2] == 0x00 && frame[3] == 0x01));
                        e.framing = starts_annex_b
                                        ? NalFramingMode::AnnexB
                                        : (codec == Codec::Unknown ? NalFramingMode::RawFrame
                                                                   : NalFramingMode::LengthPrefixed);
                    } else {
                        e.framing = NalFramingMode::RawFrame;
                    }
                }

                out.samples[si].push_back(e);
                ++counters[si];
            }
        }

        p2 = c.end;
    }

    for (std::size_t i = 0; i < out.tracks.size(); ++i) {
        out.tracks[i].sample_count = out.samples[i].size();
    }

    /* A header-only AVI (or stray audio chunks) is not a usable index. */
    for (const auto& t : out.tracks) {
        if (t.is_video && t.sample_count > 0) {
            return true;
        }
    }
    return false;
}
inline bool build_ivf(FileSource& src, IndexBuildResult& out) {
    const auto data = src.data();

    if (data.size() < 32 || std::memcmp(data.data(), "DKIF", 4) != 0) {
        return false;
    }

    const std::string fourcc(
        reinterpret_cast<const char*>(data.data() + 8), 4
    );

    Codec codec = Codec::Unknown;
    if (fourcc == "VP90" || fourcc == "VP09") codec = Codec::Vp9;
    else if (fourcc == "VP80" || fourcc == "VP08") codec = Codec::Vp8;
    else if (fourcc == "AV01" || fourcc == "av01") codec = Codec::Av1;

    const std::uint16_t width = static_cast<std::uint16_t>(data[12] | (data[13] << 8));
    const std::uint16_t height = static_cast<std::uint16_t>(data[14] | (data[15] << 8));

    out.tracks.resize(1);
    out.samples.resize(1);
    out.param_sets.resize(1);

    TrackView v;
    v.id = 1;
    v.is_video = true;
    v.codec = codec;
    v.framing = codec == Codec::Av1 ? NalFramingMode::Obu : NalFramingMode::RawFrame;
    v.codec_name = fourcc_is_blank(fourcc) ? std::string(codec_name(codec)) : fourcc;
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
        e.framing = out.tracks[0].framing;
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
        Codec codec = Codec::Unknown;
        std::string codec_name;
        bool is_video = false;
        bool is_audio = false;
        std::uint32_t sample_rate = 0;
        std::uint16_t channels = 0;
        std::uint16_t bits_per_sample = 0;
        int header_packets = 1; // BOS identification header
        std::uint64_t last_granule = 0;
        bool have_granule = false;
    };

    const auto read_le64 = [](std::span<const std::uint8_t> d, std::size_t p) -> std::uint64_t {
        std::uint64_t v = 0;
        for (unsigned i = 0; i < 8; ++i) {
            if (p + i >= d.size()) return 0;
            v |= static_cast<std::uint64_t>(d[p + i]) << (8u * i);
        }
        return v;
    };

    const auto read_le32 = [](std::span<const std::uint8_t> d, std::size_t p) -> std::uint32_t {
        if (p + 4 > d.size()) return 0;
        return static_cast<std::uint32_t>(d[p]) |
               (static_cast<std::uint32_t>(d[p + 1]) << 8) |
               (static_cast<std::uint32_t>(d[p + 2]) << 16) |
               (static_cast<std::uint32_t>(d[p + 3]) << 24);
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
            bool duplicate = false;
            for (const auto& s : streams) {
                if (s.serial == serial) {
                    duplicate = true;
                    break;
                }
            }

            if (!duplicate) {
                StreamInfo info;
                info.serial = serial;
                const auto* sig = page_data.data();
                const std::size_t n = page_data.size();

                if (n >= 5 && std::memcmp(sig, "OVP80", 5) == 0) {
                    info.codec = Codec::Vp8;
                    info.is_video = true;
                } else if (n >= 5 && std::memcmp(sig, "OVP90", 5) == 0) {
                    info.codec = Codec::Vp9;
                    info.is_video = true;
                } else if (n >= 4 && std::memcmp(sig, "\x81\x41\x56\x30", 4) == 0) {
                    info.codec = Codec::Av1;
                    info.is_video = true;
                } else if (n >= 4 && sig[0] == 0x01 && sig[1] == 'H') {
                    info.codec = Codec::Hevc;
                    info.is_video = true;
                } else if (n >= 2 && sig[0] == 0x01 && sig[1] == 0x42) {
                    info.codec = Codec::Avc;
                    info.is_video = true;
                } else if (n >= 7 && std::memcmp(sig, "\x80theora", 7) == 0) {
                    info.codec = Codec::Theora;
                    info.is_video = true;
                } else if (n >= 7 && std::memcmp(sig, "\x01vorbis", 7) == 0) {
                    info.codec = Codec::Vorbis;
                    info.is_audio = true;
                    /* Vorbis identification header: byte 0 = 1, "vorbis" @1..6,
                     * version @7..10, channels @11, sample rate @12..15. */
                    if (n >= 16) {
                        info.channels = static_cast<std::uint16_t>(sig[11]);
                        info.sample_rate = read_le32(page_data, 12);
                    }
                } else if (n >= 8 && std::memcmp(sig, "OpusHead", 8) == 0) {
                    info.codec = Codec::Opus;
                    info.is_audio = true;
                    /* OpusHead: version @8, channels @9, pre-skip @10..11,
                     * input sample rate @12..15. */
                    if (n >= 16) {
                        info.channels = static_cast<std::uint16_t>(sig[9]);
                        info.sample_rate = read_le32(page_data, 12);
                    }
                } else if (n >= 4 && std::memcmp(sig, "fLaC", 4) == 0) {
                    info.codec = Codec::Flac;
                    info.is_audio = true;
                    /* fLaC marker + 4-byte metadata block header precede the
                     * STREAMINFO payload at byte 8. */
                    if (n >= 18) {
                        /* STREAMINFO: 20-bit rate, 3-bit channels-1,
                         * 5-bit bits-per-sample-1. */
                        info.sample_rate = (static_cast<std::uint32_t>(sig[18]) << 12) |
                                           (static_cast<std::uint32_t>(sig[19]) << 4) |
                                           (static_cast<std::uint32_t>(sig[20]) >> 4);
                        info.channels = static_cast<std::uint16_t>(((sig[20] >> 1) & 0x07u) + 1u);
                        info.bits_per_sample = static_cast<std::uint16_t>(
                            (((sig[20] & 0x01u) << 4) | (sig[21] >> 4)) + 1u
                        );
                    }
                } else {
                    /* Unknown codec: keep the raw signature as the identity. */
                    info.codec = Codec::Unknown;
                    info.is_video = true;
                    info.codec_name = printable_signature(page_data);
                    if (info.codec_name.empty()) info.codec_name = "unknown";
                }

                // Vorbis and Theora carry three header packets, Opus two;
                // these must not be surfaced as media samples.
                if (info.codec == Codec::Vorbis || info.codec == Codec::Theora) {
                    info.header_packets = 3;
                } else if (info.codec == Codec::Opus) {
                    info.header_packets = 2;
                } else if (info.codec == Codec::Flac) {
                    info.header_packets = 8; // STREAMINFO + common metadata blocks
                }
                if (info.codec != Codec::Unknown) {
                    info.codec_name = codec_name(info.codec);
                }
                streams.push_back(std::move(info));
            }
        }

        pos = data_off + total;
    }

    if (streams.empty()) {
        return false;
    }

    out.tracks.resize(streams.size());
    out.samples.resize(streams.size());
    out.param_sets.resize(streams.size());

    for (std::size_t i = 0; i < streams.size(); ++i) {
        const StreamInfo& s = streams[i];
        TrackView& v = out.tracks[i];
        v.id = s.serial;
        v.codec = s.codec;
        v.codec_name = s.codec_name;
        v.is_video = s.is_video;
        v.is_audio = s.is_audio;
        // Granule positions are in codec-native units: PCM samples for the
        // audio codecs and frame counts for video.  Expose a matching
        // timescale so the granule can be used as pts verbatim.
        if (s.codec == Codec::Vorbis || s.codec == Codec::Flac) {
            v.timescale = s.sample_rate != 0u ? s.sample_rate : 1000u;
        } else if (s.codec == Codec::Opus) {
            v.timescale = 48000u;
        } else if (s.is_video) {
            v.timescale = 30u;
        } else {
            v.timescale = 1000u;
        }
        v.mmap_backed = src.mmap_backed();

        if (s.is_audio) {
            v.framing = NalFramingMode::RawFrame;
            v.audio_codec = s.codec;
            v.sample_rate = s.sample_rate;
            v.channels = s.channels;
            v.bits_per_sample = s.bits_per_sample;
        } else if (s.codec == Codec::Vp8 || s.codec == Codec::Vp9 ||
                   s.codec == Codec::Theora || s.codec == Codec::Unknown) {
            v.framing = NalFramingMode::RawFrame;
        } else if (s.codec == Codec::Av1) {
            v.framing = NalFramingMode::Obu;
        } else {
            /* Ogg AVC / HEVC packets carry Annex-B NAL units. */
            v.framing = NalFramingMode::AnnexB;
        }
    }

    const auto find_stream = [&](std::uint32_t serial) -> std::size_t {
        for (std::size_t i = 0; i < streams.size(); ++i) {
            if (streams[i].serial == serial) return i;
        }
        return streams.size();
    };

    std::vector<int> header_remaining(streams.size(), 0);
    for (std::size_t i = 0; i < streams.size(); ++i) {
        header_remaining[i] = streams[i].header_packets;
    }
    std::size_t p2 = 0;

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

        const std::size_t si = find_stream(serial);
        if (si < streams.size()) {
            StreamInfo& s = streams[si];

            // Granule position is the codec-native timestamp of the final
            // completed packet on this page.
            const std::uint64_t granule = read_le64(data, p2 + 6);
            // All-ones is Ogg's "no completed packet on this page" marker.
            const bool granule_valid = granule != 0xFFFFFFFFFFFFFFFFull;
            const std::uint64_t delta =
                (granule_valid && s.have_granule) ? granule - s.last_granule : 0u;

            // Split the page into packets via the lacing values.  A packet
            // whose final lacing value is 255 continues on the next page;
            // its first fragment is still emitted so no bytes are lost.
            struct Packet { std::size_t offset; std::size_t size; };
            std::vector<Packet> packets;
            std::size_t packet_off = 0;
            std::size_t fragment = 0;
            for (std::uint8_t k = 0; k < segments; ++k) {
                fragment += data[seg_table + k];
                if (data[seg_table + k] < 255u) {
                    if (fragment != 0u) {
                        packets.push_back({data_off + packet_off, fragment});
                    }
                    packet_off += fragment;
                    fragment = 0u;
                }
            }
            if (fragment != 0u) {
                packets.push_back({data_off + packet_off, fragment});
            }

            const std::size_t skip = std::min<std::size_t>(header_remaining[si], packets.size());
            header_remaining[si] -= static_cast<int>(skip);
            const std::size_t media_count = packets.size() - skip;
            const std::uint64_t step = media_count != 0u ? delta / media_count : 0u;

            std::size_t media = 0;
            for (std::size_t idx = skip; idx < packets.size(); ++idx) {
                const auto& p = packets[idx];
                SampleEntry e;
                e.offset = p.offset;
                e.size = p.size;
                e.pts = static_cast<std::int64_t>(
                    (granule_valid && s.have_granule)
                        ? s.last_granule + step * (media + 1u)
                        : (s.have_granule ? s.last_granule : 0u)
                );
                e.dts = e.pts;
                e.duration = static_cast<std::int64_t>(step);
                e.is_key = true;
                e.framing = out.tracks[si].framing;
                out.samples[si].push_back(e);
                ++media;
            }
            if (granule_valid) {
                s.last_granule = granule;
                s.have_granule = true;
            }
        }

        p2 = data_off + total;
    }

    bool have_samples = false;
    for (std::size_t i = 0; i < out.tracks.size(); ++i) {
        out.tracks[i].sample_count = out.samples[i].size();
        if (!out.samples[i].empty()) have_samples = true;
    }

    return have_samples;
}
/*
 * MPEG-PS carries an elementary stream inside PES packets.  Look for the
 * start code and identify the codec from its parameter-set / sequence
 * header NAL.  Unrecognised streams stay Codec::Unknown rather than being
 * mislabelled as AVC.
 */
inline Codec ps_codec_from_payload(std::span<const std::uint8_t> p) {
    std::size_t i = 0;
    for (; i + 3 < p.size() && i < 8; ++i) {
        if (p[i] == 0x00 && p[i + 1] == 0x00 && p[i + 2] == 0x01) break;
    }
    if (i + 3 >= p.size() || i >= 8) return Codec::Unknown;

    const std::uint8_t first = p[i + 3];

    /* MPEG-1/2 Video sequence header. */
    if (first == 0xB3) return Codec::Mpeg2Video;

    if (i + 5 < p.size() && first == 0x00) {
        /* VVC: 2-byte NAL header, nal_unit_type in bits 7:3 of byte 2. */
        const std::uint8_t type = static_cast<std::uint8_t>((p[i + 4] >> 3) & 0x1Fu);
        if (type >= 12u && type <= 20u) return Codec::Vvc;
    }

    if (i + 4 < p.size()) {
        /* HEVC: type = first >> 1 (VPS=32, SPS=33, PPS=34). */
        const std::uint8_t hevc_type = static_cast<std::uint8_t>(first >> 1);
        if (hevc_type == 32u || hevc_type == 33u || hevc_type == 34u) return Codec::Hevc;
    }

    /* AVC: single-byte NAL header, type in the low 5 bits. */
    const std::uint8_t avc_type = static_cast<std::uint8_t>(first & 0x1Fu);
    if (avc_type == 5u || avc_type == 7u || avc_type == 8u) return Codec::Avc;

    return Codec::Unknown;
}

inline bool build_ps(FileSource& src, IndexBuildResult& out) {
    const auto data = src.data();

    if (data.size() < 14) {
        return false;
    }

    std::size_t pos = 0;
    Codec codec = Codec::Unknown;
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

                    if (!found_codec && payload.size() >= 4) {
                        const Codec detected = ps_codec_from_payload(payload);
                        if (detected != Codec::Unknown) {
                            codec = detected;
                            found_codec = true;
                        }
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
    v.codec = found_codec ? codec : Codec::Unknown;
    v.codec_name = codec_name(v.codec);
    v.framing = NalFramingMode::AnnexB;
    v.timescale = 90000;
    v.mmap_backed = src.mmap_backed();
    out.tracks[0] = v;
    out.samples[0] = std::move(samples);
    out.tracks[0].sample_count = out.samples[0].size();

    return true;
}

/* ------------------------------------------------------------------ */
/* WAV / RIFF PCM                                                      */
/* ------------------------------------------------------------------ */

inline bool build_wav(FileSource& src, IndexBuildResult& out) {
    const auto data = src.data();
    if (data.size() < 44 || std::memcmp(data.data(), "RIFF", 4) != 0 ||
        std::memcmp(data.data() + 8, "WAVE", 4) != 0) {
        return false;
    }

    // Locate fmt and data chunks.
    std::size_t fmt_pos = 0;
    std::size_t data_pos = 0;
    std::uint32_t data_size = 0;
    std::uint16_t format_tag = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0, block_align = 0;

    std::size_t p = 12;
    while (p + 8 <= data.size()) {
        const std::uint32_t chunk_size = static_cast<std::uint32_t>(data[p + 4]) |
            (static_cast<std::uint32_t>(data[p + 5]) << 8) |
            (static_cast<std::uint32_t>(data[p + 6]) << 16) |
            (static_cast<std::uint32_t>(data[p + 7]) << 24);
        if (p + 8 + chunk_size > data.size()) {
            break;
        }
        if (std::memcmp(data.data() + p, "fmt ", 4) == 0 && chunk_size >= 16) {
            fmt_pos = p;
            format_tag = static_cast<std::uint16_t>(data[p + 8] | (data[p + 9] << 8));
            channels = static_cast<std::uint16_t>(data[p + 10] | (data[p + 11] << 8));
            rate = static_cast<std::uint32_t>(data[p + 12]) |
                   (static_cast<std::uint32_t>(data[p + 13]) << 8) |
                   (static_cast<std::uint32_t>(data[p + 14]) << 16) |
                   (static_cast<std::uint32_t>(data[p + 15]) << 24);
            block_align = static_cast<std::uint16_t>(data[p + 20] | (data[p + 21] << 8));
            bits = static_cast<std::uint16_t>(data[p + 22] | (data[p + 23] << 8));
        } else if (std::memcmp(data.data() + p, "data", 4) == 0) {
            data_pos = p + 8;
            data_size = chunk_size;
        }
        p += 8 + chunk_size + (chunk_size & 1u);
    }

    if (fmt_pos == 0 || data_pos == 0 || channels == 0u) {
        return false;
    }

    AudioCodec codec = AudioCodec::Unknown;
    if (format_tag == 1) {
        codec = bits == 8u ? Codec::Pcm8 : bits == 24u ? Codec::Pcm24
              : bits == 32u ? Codec::Pcm32 : Codec::Pcm16;
    } else if (format_tag == 3) {
        codec = bits == 64u ? Codec::PcmFloat64 : Codec::PcmFloat32;
    } else if (format_tag == 0xFFFE) {
        // Extensible: read the subformat GUID from the fmt payload.
        const std::size_t sub = fmt_pos + 8 + 24;
        if (sub + 2 <= data.size() && data[sub] == 1 && data[sub + 1] == 0) {
            codec = bits == 8u ? Codec::Pcm8 : bits == 24u ? Codec::Pcm24
                  : bits == 32u ? Codec::Pcm32 : Codec::Pcm16;
        } else {
            codec = bits == 64u ? Codec::PcmFloat64 : Codec::PcmFloat32;
        }
    }
    if (codec == AudioCodec::Unknown) {
        return false;
    }

    out.tracks.resize(1);
    out.samples.resize(1);
    out.param_sets.resize(1);
    TrackView& v = out.tracks[0];
    v.id = 1;
    v.is_audio = true;
    v.framing = NalFramingMode::RawFrame;
    v.codec = codec;
    v.audio_codec = codec;
    v.codec_name = codec_name(codec);
    v.sample_rate = rate;
    v.channels = channels;
    v.bits_per_sample = bits;
    v.timescale = rate != 0u ? rate : 1000u;
    v.mmap_backed = src.mmap_backed();

    // Emit one sample per block-aligned frame.
    const std::size_t align = block_align != 0u ? block_align
                                                : static_cast<std::size_t>(channels) * (bits / 8u);
    if (align == 0u) {
        return false;
    }
    const std::size_t limit = std::min<std::size_t>(data_size, data.size() - data_pos);
    std::uint64_t frame_index = 0;
    for (std::size_t off = 0; off + align <= limit; off += align) {
        SampleEntry e;
        e.offset = data_pos + off;
        e.size = align;
        // One block-aligned frame is one sample period.  The track timescale
        // is the sample rate, so pts/dts advance by 1 tick per frame (not by
        // a byte count), and duration is a single tick.
        e.pts = static_cast<std::int64_t>(frame_index);
        e.dts = e.pts;
        e.duration = 1;
        e.is_key = true;
        e.framing = NalFramingMode::RawFrame;
        out.samples[0].push_back(e);
        ++frame_index;
    }
    v.sample_count = out.samples[0].size();
    return !out.samples[0].empty();
}

}  // namespace detail
}  // namespace demux
}  // namespace bs
