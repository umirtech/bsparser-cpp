// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "mux/stream.hpp"
#include "mux/file_sink.hpp"
#include "demux/es_reconstruct.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bs {
namespace mux {
namespace detail {

/*
 * -----------------------------------------------------------
 * Minimal ISO-BMFF (MP4) writer
 * -----------------------------------------------------------
 *
 * Everything is assembled in local byte buffers and emitted in one
 * pass: ftyp, moov, mdat.  The chunk offsets in stco are absolute
 * file offsets, so the moov box is built once with placeholder
 * offsets to learn its size, then rebuilt with the real offsets
 * before anything is handed to the sink.
 *
 * Only the boxes the demuxer actually reads are emitted; the goal
 * is a faithful round-trip through demux::build_mp4(), not a fully
 * general-purpose MP4 muxer.
 */

using Mp4Bytes = std::vector<std::uint8_t>;

inline void be16(Mp4Bytes& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

inline void be32(Mp4Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

inline void be64(Mp4Bytes& out, std::uint64_t v) {
    be32(out, static_cast<std::uint32_t>(v >> 32));
    be32(out, static_cast<std::uint32_t>(v));
}

inline void put4(Mp4Bytes& out, const char* s) {
    out.push_back(static_cast<std::uint8_t>(s[0]));
    out.push_back(static_cast<std::uint8_t>(s[1]));
    out.push_back(static_cast<std::uint8_t>(s[2]));
    out.push_back(static_cast<std::uint8_t>(s[3]));
}

inline void put_zeros(Mp4Bytes& out, std::size_t n) {
    out.insert(out.end(), n, std::uint8_t{0});
}

inline void put_bytes(Mp4Bytes& out, std::span<const std::uint8_t> s) {
    out.insert(out.end(), s.begin(), s.end());
}

/* Size of the box header needed for a payload of this many bytes: 8 bytes in
 * the normal case, 16 bytes when the total box size no longer fits in 32 bits. */
[[nodiscard]] inline std::size_t box_size_bytes(std::uint64_t payload) noexcept {
    return (8u + payload > 0xFFFFFFFFull) ? 16u : 8u;
}

/* A complete box: fourcc + payload, with a 32-bit size header unless the
 * payload is large enough to require the 64-bit extended-size form. */
inline Mp4Bytes box(const char* type, const Mp4Bytes& payload) {
    const std::size_t header = box_size_bytes(payload.size());
    Mp4Bytes out;
    out.reserve(header + payload.size());
    if (header == 16u) {
        be32(out, 1u); // extended size marker
        put4(out, type);
        be64(out, static_cast<std::uint64_t>(16u + payload.size()));
    } else {
        be32(out, static_cast<std::uint32_t>(8u + payload.size()));
        put4(out, type);
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

inline void append_box_bytes(Mp4Bytes& out, const Mp4Bytes& b) {
    out.insert(out.end(), b.begin(), b.end());
}

inline void emit_box(FileSink& sink, const Mp4Bytes& b) {
    sink.write(std::span<const std::uint8_t>(b.data(), b.size()));
}

[[nodiscard]] inline std::uint16_t dim16(std::uint32_t v) noexcept {
    if (v == 0u) {
        return 16u;
    }
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(v, 0xFFFFu));
}

[[nodiscard]] inline std::uint32_t clamp_duration(std::int64_t v) noexcept {
    if (v <= 0) {
        return 1u;
    }
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(v), 0xFFFFFFFFull)
    );
}

/* ------- Metadata helpers ------- */

/* A Metadata has udta "tag" content when any ilst/key field is present. */
[[nodiscard]] inline bool mp4_metadata_has_tags(const Metadata& m) noexcept {
    return !m.title.empty() || !m.artist.empty() || !m.comment.empty() ||
           !m.copyright.empty() || !m.custom.empty();
}

/* Unix seconds -> MP4 seconds (1904 epoch). 0 stays 0 (unset). */
[[nodiscard]] inline std::uint32_t mp4_time(std::int64_t unix_seconds) noexcept {
    constexpr std::int64_t kEpoch = 2082844800LL;
    constexpr std::int64_t kMax = 0xFFFFFFFFLL;
    if (unix_seconds <= 0) {
        return 0u;
    }
    if (unix_seconds > kMax - kEpoch) {
        return 0xFFFFFFFFu;
    }
    return static_cast<std::uint32_t>(unix_seconds + kEpoch);
}

/* ISO-BMFF pack ISO-639-2/T language code (3 chars) into 5-bit groups. */
[[nodiscard]] inline std::uint16_t mp4_language(std::string_view lang) noexcept {
    if (lang.size() != 3u) {
        lang = "und";
    }
    for (const char c : lang) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z'))) {
            lang = "und";
            break;
        }
    }

    unsigned packed = 0u;
    for (const char c : lang) {
        const int lower = std::tolower(static_cast<unsigned char>(c));
        const unsigned v = (static_cast<unsigned>(lower) - static_cast<unsigned>('a') + 1u) & 0x1Fu;
        packed = (packed << 5u) | v;
    }
    return static_cast<std::uint16_t>(packed);
}

/* A box whose 4-byte type is arbitrary bytes (used for ©-prefixed items). */
inline Mp4Bytes box_with_type(const std::uint8_t type[4], const Mp4Bytes& payload) {
    Mp4Bytes out;
    out.reserve(8u + payload.size());
    be32(out, static_cast<std::uint32_t>(8u + payload.size()));
    for (unsigned i = 0u; i < 4u; ++i) {
        out.push_back(type[i]);
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/* A 'data' FullBox: version 0, flags 1 (UTF-8), locale 0, no NUL terminator. */
inline Mp4Bytes make_data_box(const std::string& value) {
    const std::string safe = sanitize_metadata_string(value);
    Mp4Bytes payload;
    payload.push_back(0x00u); // version
    payload.push_back(0x00u); // flags
    payload.push_back(0x00u); // flags
    payload.push_back(0x01u); // flags: UTF-8
    be32(payload, 0u);        // locale
    payload.insert(payload.end(), safe.begin(), safe.end());
    return box("data", payload);
}

inline void append_ilst_item(
    Mp4Bytes& ilst, const std::uint8_t type[4], const std::string& value
) {
    const std::string safe = sanitize_metadata_string(value);
    if (safe.empty()) {
        return;
    }
    append_box_bytes(ilst, box_with_type(type, make_data_box(safe)));
}

/* Build the full 'udta' box for one Metadata (file-level or per-track).
 * file_level adds the muxer-owned ©too encoder item. */
inline Mp4Bytes make_mp4_udta(const Metadata& m, bool file_level = false) {
    /* hdlr (FullBox): version/flags, pre_defined, 'mdir', reserved[3], name. */
    Mp4Bytes hdlr_payload;
    put_zeros(hdlr_payload, 4u); // version + flags
    be32(hdlr_payload, 0u);      // pre_defined
    put4(hdlr_payload, "mdir");  // handler_type
    put_zeros(hdlr_payload, 12u);// reserved[3]
    for (const char c : std::string_view("bsparser")) {
        hdlr_payload.push_back(static_cast<std::uint8_t>(c));
    }
    hdlr_payload.push_back(0u); // NUL terminator

    /* meta (FullBox): version/flags, then hdlr, optional keys, ilst. */
    Mp4Bytes meta_payload;
    put_zeros(meta_payload, 4u); // version + flags
    append_box_bytes(meta_payload, box("hdlr", hdlr_payload));

    if (!m.custom.empty()) {
        Mp4Bytes keys_payload;
        put_zeros(keys_payload, 4u); // version + flags
        be32(keys_payload, static_cast<std::uint32_t>(m.custom.size()));
        for (const auto& kv : m.custom) {
            const std::string key = sanitize_metadata_string(kv.first);
            Mp4Bytes entry;
            put_zeros(entry, 4u); // version + flags
            entry.insert(entry.end(), key.begin(), key.end());
            append_box_bytes(keys_payload, box("mdta", entry));
        }
        append_box_bytes(meta_payload, box("keys", keys_payload));
    }

    Mp4Bytes ilst;
    static constexpr std::uint8_t kName[4] = {0xA9u, 'n', 'a', 'm'};
    static constexpr std::uint8_t kArtist[4] = {0xA9u, 'A', 'R', 'T'};
    static constexpr std::uint8_t kComment[4] = {0xA9u, 'c', 'm', 't'};
    static constexpr std::uint8_t kCopyright[4] = {0xA9u, 'c', 'p', 'y'};
    static constexpr std::uint8_t kEncoder[4] = {0xA9u, 't', 'o', 'o'};
    append_ilst_item(ilst, kName, m.title);
    append_ilst_item(ilst, kArtist, m.artist);
    append_ilst_item(ilst, kComment, m.comment);
    append_ilst_item(ilst, kCopyright, m.copyright);
    if (file_level) {
        append_ilst_item(ilst, kEncoder, encoder_signature());
    }

    std::uint32_t index = 1u; // 1-based into the keys box
    for (const auto& kv : m.custom) {
        if (!kv.second.empty()) {
            const std::uint8_t type[4] = {
                static_cast<std::uint8_t>(index >> 24u),
                static_cast<std::uint8_t>(index >> 16u),
                static_cast<std::uint8_t>(index >> 8u),
                static_cast<std::uint8_t>(index)
            };
            append_ilst_item(ilst, type, kv.second);
        }
        ++index;
    }
    append_box_bytes(meta_payload, box("ilst", ilst));

    Mp4Bytes udta_payload;
    append_box_bytes(udta_payload, box("meta", meta_payload));
    return box("udta", udta_payload);
}

/* ------- Annex-B helpers ------- */

[[nodiscard]] inline bool starts_with_annexb(std::span<const std::uint8_t> d) noexcept {
    return d.size() >= 3u && d[0] == 0u && d[1] == 0u && d[2] == 1u;
}

/* Split an Annex-B byte stream into its NAL units (no start codes). */
inline std::vector<std::span<const std::uint8_t>> split_annexb(std::span<const std::uint8_t> d) {
    std::vector<std::span<const std::uint8_t>> nals;
    const std::size_t n = d.size();

    std::size_t start = n;
    for (std::size_t i = 0u; i + 2u < n; ++i) {
        if (d[i] == 0u && d[i + 1] == 0u && d[i + 2] == 1u) {
            start = i + 3u;
            break;
        }
    }

    while (start <= n) {
        std::size_t end = n;
        for (std::size_t i = start; i + 2u < n; ++i) {
            if (d[i] == 0u && d[i + 1] == 0u && d[i + 2] == 1u) {
                end = i;
                break;
            }
        }

        /* Trailing zero bytes belong to the next start code. */
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

/*
 * Convert an Annex-B sample to 4-byte-length-prefixed NALs.  If no start
 * code is present the bytes are copied verbatim (already framed).
 */
inline Mp4Bytes annexb_to_length_prefixed(std::span<const std::uint8_t> d) {
    Mp4Bytes out;
    const auto nals = split_annexb(d);

    for (const auto nal : nals) {
        if (nal.size() > 0xFFFFFFFFull) {
            continue;
        }
        be32(out, static_cast<std::uint32_t>(nal.size()));
        put_bytes(out, nal);
    }

    if (out.empty()) {
        put_bytes(out, d);
    }
    return out;
}

/*
 * avcC with lengthSizeMinusOne = 3 (4-byte lengths).
 *
 * The parameter-set arrays are deliberately left empty.  Input samples
 * coming from demux::Demuxer already contain the parameter sets as their
 * first Annex-B sample (written into mdat like any other sample); if avcC
 * also advertised them, the demuxer would prepend a duplicate phantom
 * parameter-set pseudo-sample and the sample count would drift on every
 * remux.  Raw Annex-B inputs are fine too: their parameter sets stay
 * in-band in the media samples.
 */
inline Mp4Bytes make_avcc(const std::vector<Sample>& /*samples*/) {
    Mp4Bytes cfg;
    cfg.push_back(1u);    // configurationVersion
    cfg.push_back(0x42u); // AVCProfileIndication
    cfg.push_back(0u);    // profile_compatibility
    cfg.push_back(0x1Eu); // AVCLevelIndication
    cfg.push_back(0xFFu); // 6 reserved bits + lengthSizeMinusOne = 3
    cfg.push_back(0xE0u); // 3 reserved bits + numOfSPS = 0
    cfg.push_back(0u);    // numOfPPS = 0

    return box("avcC", cfg);
}

/*
 * hvcC / vvcC: a minimal placeholder configuration.  numOfArrays stays
 * zero so extract_param_sets() yields nothing; the demuxer still indexes
 * every frame.  Byte 18 (hvc1) / byte 14 (vvc1) are set so this demuxer's
 * length-size probe resolves directly to 4-byte NAL lengths.
 */
inline Mp4Bytes make_minimal_hvc_config(bool is_vvc) {
    Mp4Bytes cfg(23u, std::uint8_t{0});
    cfg[0] = 1u; // configurationVersion
    cfg[is_vvc ? 14u : 18u] = 0x03u; // lengthSizeMinusOne as read by this demuxer
    return box(is_vvc ? "vvcC" : "hvcC", cfg);
}

inline Mp4Bytes make_minimal_codec_config(Codec codec, const std::vector<Sample>& samples) {
    switch (codec) {
        case Codec::Avc:
            return make_avcc(samples);
        case Codec::Hevc:
            return make_minimal_hvc_config(false);
        case Codec::Vvc:
            return make_minimal_hvc_config(true);
        case Codec::Av1: {
            Mp4Bytes cfg{0x81u, 0x00u, 0x0Cu, 0x00u};
            return box("av1C", cfg);
        }
        case Codec::Vp9:
        case Codec::Vp8: {
            Mp4Bytes cfg(8u, std::uint8_t{0});
            return box("vpcC", cfg);
        }
    }
    return box("vpcC", Mp4Bytes(8u, std::uint8_t{0}));
}

/* VisualSampleEntry with the codec-config box as a direct child. */
inline Mp4Bytes visual_sample_entry(
    const char* fourcc, std::uint32_t width, std::uint32_t height, const Mp4Bytes& config
) {
    Mp4Bytes b;
    put_zeros(b, 6u); // reserved
    be16(b, 1u);      // data_reference_index
    be16(b, 0u);      // pre_defined
    be16(b, 0u);      // reserved
    for (unsigned i = 0u; i < 3u; ++i) {
        be32(b, 0u); // pre_defined[3]
    }
    be16(b, dim16(width));
    be16(b, dim16(height));
    be32(b, 0x00480000u); // horizresolution (72 dpi)
    be32(b, 0x00480000u); // vertresolution (72 dpi)
    be32(b, 0u);          // reserved
    be16(b, 1u);          // frame_count
    put_zeros(b, 32u);    // compressorname
    be16(b, 0x0018u);     // depth
    be16(b, 0xFFFFu);     // pre_defined
    b.insert(b.end(), config.begin(), config.end());
    return box(fourcc, b);
}

inline Mp4Bytes make_mvhd(
    std::uint32_t duration_ms, std::uint32_t next_track_id, const Mp4Bytes& identity,
    std::uint32_t creation_time
) {
    Mp4Bytes b;
    b.push_back(0u); // version 0
    put_zeros(b, 3u); // flags
    be32(b, creation_time); // creation_time
    be32(b, creation_time); // modification_time
    be32(b, 1000u);   // timescale
    be32(b, duration_ms);
    be32(b, 0x00010000u); // rate 1.0
    be16(b, 0x0100u);     // volume 1.0
    be16(b, 0u);          // reserved
    be64(b, 0u);          // reserved
    b.insert(b.end(), identity.begin(), identity.end());
    put_zeros(b, 24u); // pre_defined[6]
    be32(b, next_track_id);
    return box("mvhd", b);
}

inline Mp4Bytes make_tkhd(
    std::uint32_t track_id, std::uint32_t duration_ms, std::uint32_t width, std::uint32_t height,
    bool is_audio, const Mp4Bytes& identity, std::uint32_t creation_time
) {
    Mp4Bytes b;
    b.push_back(0u); // version 0
    b.push_back(0u); // flags: enabled | in_movie | in_preview = 0x000007
    b.push_back(0u);
    b.push_back(0x07u);
    be32(b, creation_time); // creation_time
    be32(b, creation_time); // modification_time
    be32(b, track_id);
    be32(b, 0u); // reserved
    be32(b, duration_ms);
    be64(b, 0u); // reserved
    be16(b, 0u); // layer
    be16(b, 0u); // alternate_group
    be16(b, is_audio ? 0x0100u : 0u); // volume
    be16(b, 0u);                      // reserved
    b.insert(b.end(), identity.begin(), identity.end());
    be32(b, static_cast<std::uint32_t>(dim16(width)) << 16u);
    be32(b, static_cast<std::uint32_t>(dim16(height)) << 16u);
    return box("tkhd", b);
}

inline Mp4Bytes make_mdhd(
    std::uint32_t timescale, std::uint32_t duration, std::uint32_t creation_time,
    std::string_view language
) {
    const std::string safe_lang = sanitize_metadata_string(language);
    Mp4Bytes b;
    b.push_back(0u); // version 0
    put_zeros(b, 3u); // flags
    be32(b, creation_time); // creation_time
    be32(b, creation_time); // modification_time
    be32(b, timescale);
    be32(b, duration);
    be16(b, mp4_language(safe_lang)); // language (default "und")
    be16(b, 0u);                     // pre_defined
    return box("mdhd", b);
}

inline Mp4Bytes make_hdlr(bool is_audio) {
    Mp4Bytes b;
    b.push_back(0u); // version 0
    put_zeros(b, 3u); // flags
    be32(b, 0u);      // pre_defined
    put4(b, is_audio ? "soun" : "vide");
    put_zeros(b, 12u); // reserved
    const char* name = is_audio ? "SoundHandler" : "VideoHandler";
    for (const char* p = name; *p != '\0'; ++p) {
        b.push_back(static_cast<std::uint8_t>(*p));
    }
    b.push_back(0u); // NUL terminator
    return box("hdlr", b);
}

inline Mp4Bytes make_stts(const std::vector<std::uint32_t>& durations) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
    for (const std::uint32_t d : durations) {
        if (!runs.empty() && runs.back().second == d) {
            ++runs.back().first;
        } else {
            runs.emplace_back(1u, d);
        }
    }

    Mp4Bytes b;
    put_zeros(b, 4u); // version + flags (FullBox)
    be32(b, static_cast<std::uint32_t>(runs.size()));
    for (const auto& run : runs) {
        be32(b, run.first);
        be32(b, run.second);
    }
    return box("stts", b);
}

inline Mp4Bytes make_stsc(std::size_t sample_count) {
    Mp4Bytes b;
    put_zeros(b, 4u); // version + flags (FullBox)
    be32(b, sample_count == 0u ? 0u : 1u);
    if (sample_count != 0u) {
        be32(b, 1u); // first_chunk
        be32(b, static_cast<std::uint32_t>(sample_count)); // samples_per_chunk
        be32(b, 1u); // sample_description_index
    }
    return box("stsc", b);
}

inline Mp4Bytes make_stsz(const std::vector<std::uint32_t>& sizes) {
    Mp4Bytes b;
    put_zeros(b, 4u); // version + flags (FullBox)
    be32(b, 0u); // sample_size (per-sample table follows)
    be32(b, static_cast<std::uint32_t>(sizes.size()));
    for (const std::uint32_t s : sizes) {
        be32(b, s);
    }
    return box("stsz", b);
}

inline Mp4Bytes make_stco(std::uint32_t sample_count, std::uint32_t offset) {
    Mp4Bytes b;
    put_zeros(b, 4u); // version + flags (FullBox)
    be32(b, sample_count == 0u ? 0u : 1u);
    if (sample_count != 0u) {
        be32(b, offset);
    }
    return box("stco", b);
}

/* co64: same as stco but with 64-bit chunk offsets (FullBox). */
inline Mp4Bytes make_co64(std::uint32_t sample_count, std::uint64_t offset) {
    Mp4Bytes b;
    put_zeros(b, 4u); // version + flags (FullBox)
    be32(b, sample_count == 0u ? 0u : 1u);
    if (sample_count != 0u) {
        be64(b, offset);
    }
    return box("co64", b);
}

/* stss: sync (key) sample table (FullBox). Sample numbers are 1-based. */
inline Mp4Bytes make_stss(const std::vector<std::uint32_t>& sync) {
    Mp4Bytes b;
    put_zeros(b, 4u); // version + flags (FullBox)
    be32(b, static_cast<std::uint32_t>(sync.size()));
    for (const std::uint32_t s : sync) {
        be32(b, s);
    }
    return box("stss", b);
}

inline Mp4Bytes make_stsd(
    Codec codec, std::uint32_t width, std::uint32_t height, const std::vector<Sample>& samples
) {
    Mp4Bytes b;
    put_zeros(b, 4u); // version + flags (FullBox)
    be32(b, 1u); // entry_count
    const Mp4Bytes entry = visual_sample_entry(
        mp4_fourcc(codec), width, height, make_minimal_codec_config(codec, samples)
    );
    b.insert(b.end(), entry.begin(), entry.end());
    return box("stsd", b);
}

inline Mp4Bytes make_ftyp() {
    Mp4Bytes b;
    put4(b, "isom"); // major brand
    be32(b, 0x200u); // minor version
    put4(b, "isom");
    put4(b, "iso2");
    put4(b, "avc1");
    put4(b, "mp41");
    return box("ftyp", b);
}

/* Non-negative, overflow-free PTS delta of b relative to a. */
[[nodiscard]] inline std::int64_t sample_delta(std::int64_t a, std::int64_t b) noexcept {
    if (b <= a) {
        return 0;
    }
    const std::uint64_t d = static_cast<std::uint64_t>(b) - static_cast<std::uint64_t>(a);
    return d > static_cast<std::uint64_t>(INT64_MAX)
               ? INT64_MAX
               : static_cast<std::int64_t>(d);
}

/* Per-sample durations from consecutive PTS differences. */
inline std::vector<std::uint32_t> sample_durations(const std::vector<Sample>& samples) {
    const std::size_t n = samples.size();
    std::vector<std::uint32_t> out(n, 1000u);
    if (n == 0u) {
        return out;
    }

    for (std::size_t i = 0u; i + 1u < n; ++i) {
        std::int64_t d = sample_delta(samples[i].pts, samples[i + 1u].pts);
        if (d <= 0) {
            d = samples[i].duration > 0 ? samples[i].duration : 1000;
        }
        out[i] = clamp_duration(d);
    }

    std::int64_t last = samples[n - 1u].duration > 0 ? samples[n - 1u].duration
                                                     : static_cast<std::int64_t>(out[n - 1u]);
    out[n - 1u] = clamp_duration(last);
    return out;
}

[[nodiscard]] inline bool needs_annexb_conversion(
    Codec codec, const Sample& sample, const TrackConfig& config
) noexcept {
    if (!is_h26x(codec) || sample.data.empty()) {
        return false;
    }
    if (demux::es::fully_length_prefixed(sample.data, 4u)) {
        return false;
    }
    if (starts_with_annexb(sample.data)) {
        return true;
    }
    return sample.framing == NalFramingMode::AnnexB || config.framing == NalFramingMode::AnnexB;
}

/*
 * Write a progressive MP4 containing the supplied tracks/samples.
 * Returns false when there is nothing video-like to write.
 *
 * `metadata` carries file-level tags / creation time; each TrackConfig may
 * additionally carry per-track tags in its own `metadata` field.
 */
inline bool write_mp4(
    FileSink& sink, const Metadata& metadata,
    const std::vector<TrackConfig>& tracks,
    const std::vector<std::vector<Sample>>& samples
) {
    if (tracks.empty()) {
        return false;
    }

    bool has_video = false;
    for (const auto& t : tracks) {
        if (t.is_video) {
            has_video = true;
            break;
        }
    }
    if (!has_video) {
        return false;
    }

    const std::size_t n = tracks.size();

    static const std::vector<Sample> kEmptySamples;
    std::vector<const std::vector<Sample>*> per_track(n, &kEmptySamples);
    for (std::size_t i = 0u; i < n; ++i) {
        if (i < samples.size()) {
            per_track[i] = &samples[i];
        }
    }

    /* 1. Framed sample payloads and their sizes (as stored in mdat). */
    std::vector<Mp4Bytes> payloads(n);
    std::vector<std::vector<std::uint32_t>> sizes(n);
    std::vector<std::vector<std::uint32_t>> durations(n);
    std::vector<std::uint32_t> timescales(n, 1000u);
    std::vector<std::uint32_t> track_ids(n, 0u);
    std::uint32_t max_track_id = 0u;

    for (std::size_t i = 0u; i < n; ++i) {
        const TrackConfig& cfg = tracks[i];
        const std::vector<Sample>& ss = *per_track[i];

        timescales[i] = cfg.timescale != 0u ? cfg.timescale : 1000u;
        track_ids[i] = cfg.id != 0u ? cfg.id : static_cast<std::uint32_t>(i + 1u);
        max_track_id = std::max(max_track_id, track_ids[i]);
        durations[i] = sample_durations(ss);
        sizes[i].reserve(ss.size());

        for (const auto& s : ss) {
            Mp4Bytes framed = needs_annexb_conversion(cfg.codec, s, cfg)
                                  ? annexb_to_length_prefixed(s.data)
                                  : Mp4Bytes(s.data.begin(), s.data.end());

            /* stsz records individual sample sizes as 32-bit values. */
            if (framed.size() > 0xFFFFFFFFull) {
                return false;
            }
            sizes[i].push_back(static_cast<std::uint32_t>(framed.size()));
            payloads[i].insert(payloads[i].end(), framed.begin(), framed.end());
        }
    }

    /* 1b. mdat sizing. Large payloads use a 64-bit extended box header and
     * co64 chunk offsets. moov is built twice, so fix the stco/co64 decision
     * up front from a conservative upper bound on the data offset; that keeps
     * moov's size identical across both builds even for >4 GiB files. */
    std::uint64_t mdat_payload_size = 0u;
    for (const auto& p : payloads) {
        mdat_payload_size += p.size();
    }
    const std::size_t mdat_header_size = box_size_bytes(mdat_payload_size);

    const Mp4Bytes ftyp = make_ftyp();

    const std::uint64_t kMoovUpperBound = 1u << 20; // generous bound on moov's size
    std::uint64_t offset_upper =
        static_cast<std::uint64_t>(ftyp.size()) + kMoovUpperBound + mdat_header_size;
    std::vector<bool> use_co64(n, false);
    for (std::size_t i = 0u; i < n; ++i) {
        use_co64[i] = offset_upper > 0xFFFFFFFFull;
        offset_upper += payloads[i].size();
    }

    /* 2. Movie duration (movie timescale = 1000). */
    std::uint64_t movie_duration = 0u;
    std::vector<std::uint32_t> track_duration_ms(n, 0u);
    for (std::size_t i = 0u; i < n; ++i) {
        std::uint64_t total = 0u;
        for (const std::uint32_t d : durations[i]) {
            total += d;
        }
        const std::uint64_t ms = (total * 1000u + timescales[i] / 2u) / timescales[i];
        track_duration_ms[i] = static_cast<std::uint32_t>(std::min<std::uint64_t>(ms, 0xFFFFFFFFull));
        movie_duration = std::max(movie_duration, ms);
    }
    const std::uint32_t mvhd_duration =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(movie_duration, 0xFFFFFFFFull));

    Mp4Bytes identity;
    be32(identity, 0x00010000u); be32(identity, 0u);          be32(identity, 0u);
    be32(identity, 0u);          be32(identity, 0x00010000u); be32(identity, 0u);
    be32(identity, 0u);          be32(identity, 0u);          be32(identity, 0x40000000u);

    /* moov depends on chunk offsets, which depend on moov's own size. */
    std::vector<std::uint64_t> chunk_offsets(n, 0u);

    const std::uint32_t creation_time = mp4_time(metadata.creation_time);

    auto build_moov = [&]() {
        Mp4Bytes body = make_mvhd(mvhd_duration, max_track_id + 1u, identity, creation_time);

        for (std::size_t i = 0u; i < n; ++i) {
            const TrackConfig& cfg = tracks[i];
            const std::vector<Sample>& ss = *per_track[i];
            const std::uint32_t sample_count = static_cast<std::uint32_t>(sizes[i].size());

            Mp4Bytes stbl;
            append_box_bytes(stbl, make_stsd(cfg.codec, cfg.width, cfg.height, ss));
            append_box_bytes(stbl, make_stts(durations[i]));
            append_box_bytes(stbl, make_stsc(sample_count));
            append_box_bytes(stbl, make_stsz(sizes[i]));

            /* stss: only when at least one sample is non-key, so all-key
             * streams keep the compact "no stss means all sync" form. */
            bool has_non_key = false;
            std::vector<std::uint32_t> sync;
            sync.reserve(ss.size());
            for (std::size_t j = 0u; j < ss.size(); ++j) {
                if (ss[j].is_key) {
                    sync.push_back(static_cast<std::uint32_t>(j + 1u));
                } else {
                    has_non_key = true;
                }
            }
            if (has_non_key && !sync.empty()) {
                append_box_bytes(stbl, make_stss(sync));
            }

            if (use_co64[i]) {
                append_box_bytes(stbl, make_co64(sample_count, chunk_offsets[i]));
            } else {
                append_box_bytes(
                    stbl, make_stco(sample_count, static_cast<std::uint32_t>(chunk_offsets[i]))
                );
            }

            Mp4Bytes minf = box("stbl", stbl);

            Mp4Bytes mdia;
            append_box_bytes(
                mdia, make_mdhd(timescales[i], track_duration_ms[i], creation_time, cfg.language)
            );
            append_box_bytes(mdia, make_hdlr(cfg.is_audio && !cfg.is_video));
            append_box_bytes(mdia, box("minf", minf));

            Mp4Bytes trak;
            append_box_bytes(
                trak,
                make_tkhd(
                    track_ids[i], track_duration_ms[i], cfg.width, cfg.height,
                    cfg.is_audio && !cfg.is_video, identity, creation_time
                )
            );
            append_box_bytes(trak, box("mdia", mdia));
            if (mp4_metadata_has_tags(cfg.metadata)) {
                append_box_bytes(trak, make_mp4_udta(cfg.metadata));
            }
            append_box_bytes(body, box("trak", trak));
        }

        /* File-level udta goes after all trak boxes.  Always emitted: the
         * muxer-owned ©too encoder tag must be present even with no user tags. */
        append_box_bytes(body, make_mp4_udta(metadata, true));

        return box("moov", body);
    };

    const Mp4Bytes moov_placeholder = build_moov();

    const std::uint64_t mdat_data_offset =
        static_cast<std::uint64_t>(ftyp.size()) + moov_placeholder.size() + mdat_header_size;

    std::uint64_t running = mdat_data_offset;
    for (std::size_t i = 0u; i < n; ++i) {
        chunk_offsets[i] = running;
        running += payloads[i].size();
    }

    const Mp4Bytes moov = build_moov();

    /* 3. Emit everything in file order. */
    emit_box(sink, ftyp);
    emit_box(sink, moov);

    Mp4Bytes mdat_header;
    if (mdat_header_size == 16u) {
        be32(mdat_header, 1u); // extended size marker
        put4(mdat_header, "mdat");
        be64(mdat_header, static_cast<std::uint64_t>(16u + mdat_payload_size));
    } else {
        be32(mdat_header, static_cast<std::uint32_t>(8u + mdat_payload_size));
        put4(mdat_header, "mdat");
    }
    emit_box(sink, mdat_header);

    for (const auto& p : payloads) {
        if (!p.empty()) {
            emit_box(sink, p);
        }
    }

    return true;
}

/* Backward-compatible overload: no metadata. */
inline bool write_mp4(
    FileSink& sink, const std::vector<TrackConfig>& tracks,
    const std::vector<std::vector<Sample>>& samples
) {
    static const Metadata kEmptyMetadata;
    return write_mp4(sink, kEmptyMetadata, tracks, samples);
}

}  // namespace detail
}  // namespace mux
}  // namespace bs
