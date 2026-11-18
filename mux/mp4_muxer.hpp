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
 * Files are written in streaming fashion: ftyp and a fixed mdat header
 * are emitted first, framed samples are appended as they arrive, and a
 * moov built from the recorded sample/chunk tables is appended (or
 * patched into a reserved region) at finish time.
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

/* Media timescale: an explicit config value wins; otherwise audio falls
 * back to its sample rate (the natural audio clock) and video to 1 kHz. */
[[nodiscard]] inline std::uint32_t mp4_track_timescale(const TrackConfig& cfg) noexcept {
    if (cfg.timescale != 0u) {
        return cfg.timescale;
    }
    if (cfg.is_audio && !cfg.is_video && cfg.sample_rate != 0u) {
        return cfg.sample_rate;
    }
    return 1000u;
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

/* True when an Annex-B sample contains only parameter-set NALs (no VCL).
 * Such samples are produced by demux::Demuxer as a leading pseudo-sample and
 * must be folded into the next real access unit, not written standalone. */
[[nodiscard]]
inline bool is_param_set_only_sample(Codec codec, std::span<const std::uint8_t> data) {
    if (!is_h26x(codec) || data.empty()) {
        return false;
    }
    const auto nals = split_annexb(data);
    if (nals.empty()) {
        return false;
    }
    for (const auto nal : nals) {
        if (nal.empty()) {
            continue;
        }
        const unsigned type = codec == Codec::Avc
            ? (static_cast<unsigned>(nal[0]) & 0x1Fu)
            : ((static_cast<unsigned>(nal[0]) >> 1u) & 0x3Fu);
        const bool is_param = codec == Codec::Avc
            ? (type == 7u || type == 8u)
            : (type == 32u || type == 33u || type == 34u);
        if (!is_param) {
            return false;
        }
    }
    return true;
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
 * hvcC / vvcC: minimal placeholder configurations with spec-correct
 * lengthSizeMinusOne offsets.  numOfArrays stays zero so no phantom
 * parameter-set samples are synthesized; callers should supply a real
 * TrackConfig::codec_config for interop.
 *
 * HEVC (ISO/IEC 14496-15 8.3.3.1): lengthSizeMinusOne is the low 2 bits
 * of byte 21, numOfArrays is byte 22 (23-byte record).
 * VVC  (ISO/IEC 14496-15 11.2.4): byte 1 = reserved(5) | lsm1(2) |
 * ptl_present_flag(1); with ptl_present_flag = 0 numOfArrays is byte 2.
 */
inline Mp4Bytes make_minimal_hvc_config(bool is_vvc) {
    Mp4Bytes cfg;
    if (is_vvc) {
        cfg.assign(3u, std::uint8_t{0});
        cfg[0] = 1u;
        cfg[1] = static_cast<std::uint8_t>(0xF8u | (3u << 1u)); // lsm1=3, ptl=0
        cfg[2] = 0u; // numOfArrays
        return box("vvcC", cfg);
    }
    cfg.assign(23u, std::uint8_t{0});
    cfg[0] = 1u;  // configurationVersion
    cfg[21] = 3u; // lengthSizeMinusOne (low 2 bits) = 3 -> 4-byte NAL lengths
    cfg[22] = 0u; // numOfArrays
    return box("hvcC", cfg);
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
        default:
            return box("vpcC", Mp4Bytes(8u, std::uint8_t{0}));
    }
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

/* ------- Audio sample entry helpers ------- */

/* Minimal AudioSpecificConfig for AAC-LC.  Only used when the caller did not
 * supply cfg.audio_config.  The bit layout is: 5-bit audioObjectType (2 =
 * AAC LC), 4-bit samplingFrequencyIndex, 4-bit channelConfiguration, then
 * the three GASpecificConfig flags (all zero for the plain LC profile). */
[[nodiscard]] inline Mp4Bytes make_default_aac_config(const TrackConfig& cfg) {
    static constexpr std::uint32_t kRates[] = {
        96000u, 88200u, 64000u, 48000u, 44100u, 32000u,
        24000u, 22050u, 16000u, 12000u, 11025u, 8000u, 7350u
    };

    const std::uint32_t rate = cfg.sample_rate != 0u ? cfg.sample_rate : 44100u;
    unsigned index = 4u; // 44100 Hz fallback
    for (unsigned i = 0u; i < 13u; ++i) {
        if (kRates[i] == rate) {
            index = i;
            break;
        }
    }

    unsigned channels = cfg.channels != 0u ? cfg.channels : 2u;
    if (channels > 6u) {
        channels = 2u; // >6 needs a PCE; keep the simple path
    }

    Mp4Bytes out;
    out.push_back(static_cast<std::uint8_t>((2u << 3u) | (index >> 1u)));
    out.push_back(static_cast<std::uint8_t>(((index & 1u) << 7u) | (channels << 3u)));
    return out;
}

/* ISO-BMFF / ISO 14496-1 expandable descriptor length (7 bits per byte,
 * continuation bit on every byte but the last). */
inline void mp4_push_desc_length(Mp4Bytes& out, std::size_t length) {
    std::uint8_t groups[4];
    int count = 0;
    do {
        groups[count++] = static_cast<std::uint8_t>(length & 0x7Fu);
        length >>= 7u;
    } while (length != 0u && count < 4);
    for (int i = count - 1; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>(groups[i] | (i != 0 ? 0x80u : 0u)));
    }
}

inline void mp4_push_descriptor(Mp4Bytes& out, std::uint8_t tag, const Mp4Bytes& payload) {
    out.push_back(tag);
    mp4_push_desc_length(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

/* esds: ES_Descriptor -> DecoderConfigDescriptor (AAC-LC, MP4 audio stream)
 * -> DecoderSpecificInfo (AudioSpecificConfig) + SLConfigDescriptor. */
inline Mp4Bytes make_esds(const TrackConfig& cfg) {
    Mp4Bytes dsi = cfg.audio_config;
    if (dsi.empty()) {
        dsi = make_default_aac_config(cfg);
    }

    Mp4Bytes dec_specific;
    mp4_push_descriptor(dec_specific, 0x05u, dsi);

    Mp4Bytes dcd;
    dcd.push_back(0x40u); // objectTypeIndication: ISO/IEC 14496-3 audio
    dcd.push_back(0x15u); // streamType 5 (audio), upStream 0, reserved 1
    put_zeros(dcd, 3u);   // bufferSizeDB
    be32(dcd, 0u);        // maxBitrate
    be32(dcd, 0u);        // avgBitrate
    dcd.insert(dcd.end(), dec_specific.begin(), dec_specific.end());

    Mp4Bytes dec_config;
    mp4_push_descriptor(dec_config, 0x04u, dcd);

    Mp4Bytes sl;
    sl.push_back(0x02u); // predefined for MP4
    Mp4Bytes sl_config;
    mp4_push_descriptor(sl_config, 0x06u, sl);

    Mp4Bytes es;
    be16(es, 0u);     // ES_ID
    es.push_back(0u); // flags
    es.insert(es.end(), dec_config.begin(), dec_config.end());
    es.insert(es.end(), sl_config.begin(), sl_config.end());

    Mp4Bytes es_descriptor;
    mp4_push_descriptor(es_descriptor, 0x03u, es);

    Mp4Bytes payload;
    put_zeros(payload, 4u); // version + flags
    payload.insert(payload.end(), es_descriptor.begin(), es_descriptor.end());
    return box("esds", payload);
}

/* SoundMediaHeader: version/flags, balance (0.0), reserved. */
inline Mp4Bytes make_smhd() {
    Mp4Bytes b;
    put_zeros(b, 4u); // version + flags
    be16(b, 0u);      // balance
    be16(b, 0u);      // reserved
    return box("smhd", b);
}

/* VideoMediaHeader: version/flags + graphicsmode(0) + opcolor(0,0,0). */
inline Mp4Bytes make_vmhd() {
    Mp4Bytes b;
    put_zeros(b, 4u);  // version + flags
    be16(b, 0u);       // graphicsmode
    be16(b, 0u);       // opcolor[0]
    be16(b, 0u);       // opcolor[1]
    be16(b, 0u);       // opcolor[2]
    return box("vmhd", b);
}

/* DataInformation + DataReference with one self-contained 'url ' entry.
 * Required by ISO-BMFF: every sample entry's data_reference_index=1 must
 * resolve to a data reference. */
inline Mp4Bytes make_dinf() {
    Mp4Bytes url;
    put_zeros(url, 4u); // version + flags=1 (self-contained, media in this file)
    url[3] = 1u;
    Mp4Bytes dref;
    put_zeros(dref, 4u); // version + flags
    be32(dref, 1u);      // entry_count
    append_box_bytes(dref, box("url ", url));
    Mp4Bytes dinf;
    append_box_bytes(dinf, box("dref", dref));
    return box("dinf", dinf);
}

/* CompositionTimeToSample: signed PTS-DTS offsets, version 1. */
inline Mp4Bytes make_ctts(const std::vector<std::int64_t>& offsets) {
    bool any = false;
    for (const auto v : offsets) {
        if (v != 0) {
            any = true;
            break;
        }
    }
    if (!any) {
        return {};
    }
    Mp4Bytes b;
    b.push_back(1u);      // version 1 (signed offsets)
    put_zeros(b, 3u);     // flags
    be32(b, static_cast<std::uint32_t>(offsets.size()));
    for (const auto v : offsets) {
        // ISO-BMFF 8.6.6: sample_count THEN sample_offset.
        be32(b, 1u);      // sample_count per entry
        const std::uint32_t u = static_cast<std::uint32_t>(v);
        be32(b, u);       // signed offset (version 1)
    }
    return box("ctts", b);
}

/* Minimal pcmC box for integer PCM sample entries. */
inline Mp4Bytes make_pcmc() {
    Mp4Bytes b;
    put_zeros(b, 4u); // version + flags
    be16(b, 0u);      // format: integer
    be16(b, 0u);      // reserved
    return box("pcmC", b);
}

/* AudioSampleEntry: reserved/dri, reserved[2], channels, sample size,
 * pre_defined, reserved, sample rate as a 16.16 fixed-point value. */
inline Mp4Bytes audio_sample_entry(
    const char* fourcc, const TrackConfig& cfg, const Mp4Bytes& config
) {
    Mp4Bytes b;
    put_zeros(b, 6u); // reserved
    be16(b, 1u);      // data_reference_index
    put_zeros(b, 8u); // reserved[2]
    be16(b, cfg.channels != 0u ? cfg.channels : 2u);
    be16(b, static_cast<std::uint16_t>(audio_bits_per_sample(cfg.audio_codec)));
    be16(b, 0u); // pre_defined
    be16(b, 0u); // reserved
    const std::uint32_t rate = cfg.sample_rate != 0u ? cfg.sample_rate : 44100u;
    be32(b, std::min(rate, 0xFFFFu) << 16u); // 16.16 fixed point
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
    if (is_audio) {
        // Audio tracks have no spatial dimensions (0.16 fixed-point).
        be32(b, 0u);
        be32(b, 0u);
    } else {
        be32(b, static_cast<std::uint32_t>(dim16(width)) << 16u);
        be32(b, static_cast<std::uint32_t>(dim16(height)) << 16u);
    }
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

/* Audio-aware stsd.  For video tracks, honor a caller-supplied codec_config
 * (a real avcC/hvcC/vvcC/av1C/vpcC box from the demuxer/encoder) so the file
 * carries genuine parameter sets instead of an empty placeholder.  Fall back
 * to the minimal synthesized config only when none was supplied. */
inline Mp4Bytes make_stsd_for_track(
    const TrackConfig& cfg, const std::vector<Sample>& samples = {}
) {
    if (!cfg.is_audio || cfg.is_video) {
        Mp4Bytes b;
        put_zeros(b, 4u); // version + flags (FullBox)
        be32(b, 1u); // entry_count
        Mp4Bytes config;
        if (cfg.codec_config.empty()) {
            config = make_minimal_codec_config(cfg.codec, samples);
        } else {
            // TrackConfig::codec_config holds a raw decoder-configuration
            // record (the same bytes Matroska stores in CodecPrivate), so
            // wrap it in the ISO-BMFF box the sample entry expects.
            const char* cfg_fourcc = "vpcC";
            switch (cfg.codec) {
                case Codec::Avc:  cfg_fourcc = "avcC"; break;
                case Codec::Hevc: cfg_fourcc = "hvcC"; break;
                case Codec::Vvc:  cfg_fourcc = "vvcC"; break;
                case Codec::Av1:  cfg_fourcc = "av1C"; break;
                default: break;
            }
            config = box(cfg_fourcc, cfg.codec_config);
        }
        const Mp4Bytes entry = visual_sample_entry(
            mp4_fourcc(cfg.codec), cfg.width, cfg.height, config
        );
        b.insert(b.end(), entry.begin(), entry.end());
        return box("stsd", b);
    }

    Mp4Bytes b;
    put_zeros(b, 4u); // version + flags (FullBox)
    be32(b, 1u);      // entry_count
    if (cfg.audio_codec == AudioCodec::Aac) {
        append_box_bytes(b, audio_sample_entry("mp4a", cfg, make_esds(cfg)));
    } else {
        // "sowt" is little-endian signed PCM (matches A_PCM/INT/LIT in MKV).
        append_box_bytes(b, audio_sample_entry("sowt", cfg, make_pcmc()));
    }
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

[[nodiscard]] inline bool needs_annexb_conversion(
    Codec codec, const Sample& sample, const TrackConfig& config
) noexcept {
    // Audio codecs legitimately leave the default Codec value set; never try
    // to reinterpret PCM/AAC payloads as Annex-B NAL units.
    if (config.is_audio && !config.is_video) {
        return false;
    }
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

/* ------------------------------------------------------------------ */
/* Streaming MP4 writer                                                */
/* ------------------------------------------------------------------ */

/* Frame one sample for storage in mdat: Annex-B -> 4-byte length-prefixed
 * for H.26x, otherwise copied verbatim.  Caller writes the result via
 * FileSink::write() and records the returned offset. */
[[nodiscard]]
inline Mp4Bytes frame_mp4_sample(
    Codec codec, const Sample& sample, const TrackConfig& config
) {
    if (needs_annexb_conversion(codec, sample, config)) {
        return annexb_to_length_prefixed(sample.data);
    }
    return Mp4Bytes(sample.data.begin(), sample.data.end());
}

/* Start a streamed file: ftyp + a 16-byte mdat header whose largesize is
 * patched in finish_mp4_stream().  Returns the absolute offset of the
 * mdat header so the muxer can remember it. */
/* Returns the mdat header offset; writes the reserve offset through the
 * out parameter so finish can patch it exactly. */
inline std::size_t start_mp4_stream(
    FileSink& sink, std::size_t moov_reserve, std::size_t& reserve_offset_out
) {
    const Mp4Bytes ftyp = make_ftyp();
    emit_box(sink, ftyp);

    reserve_offset_out = sink.size();
    if (moov_reserve >= 8u) {
        Mp4Bytes free_box;
        be32(free_box, static_cast<std::uint32_t>(moov_reserve));
        put4(free_box, "free");
        put_zeros(free_box, moov_reserve - 8u);
        emit_box(sink, free_box);
    }

    Mp4Bytes header;
    be32(header, 1u); // extended-size marker
    put4(header, "mdat");
    be64(header, 16u); // placeholder; patched at finish
    const std::size_t offset = sink.size();
    emit_box(sink, header);
    return offset;
}

/*
 * Finish a streamed file: patch mdat's largesize in place, then append a
 * moov built from the recorded sample/chunk tables.  `records` is indexed
 * by track; `chunks` is in physical write order (caller-driven interleaving).
 */
inline bool finish_mp4_stream(
    FileSink& sink,
    std::size_t mdat_header_offset,
    const Metadata& metadata,
    const std::vector<TrackConfig>& tracks,
    const std::vector<std::vector<SampleRecord>>& records,
    const std::vector<ChunkRecord>& chunks,
    std::size_t moov_reserve = 0,
    std::size_t moov_reserve_offset = 0
) {
    const std::size_t n = tracks.size();

    // 1. Patch mdat largesize = 16 + payload bytes written so far.
    const std::uint64_t mdat_payload =
        static_cast<std::uint64_t>(sink.size()) - (mdat_header_offset + 16u);
    Mp4Bytes patch;
    be64(patch, 16u + mdat_payload);
    if (!sink.patch(mdat_header_offset + 8u, patch)) {
        return false;
    }

    // 2. Per-track tables.
    std::vector<std::vector<std::uint32_t>> sizes(n);
    std::vector<std::vector<std::uint32_t>> durations(n);
    std::vector<std::vector<std::uint32_t>> sync(n);
    std::vector<std::vector<std::int64_t>> ctts(n);
    std::vector<std::uint32_t> timescales(n, 1000u);
    std::vector<std::uint32_t> track_ids(n, 0u);
    std::uint32_t max_track_id = 0u;
    std::uint64_t movie_duration_ms = 0u;
    std::vector<std::uint32_t> track_duration_ms(n, 0u);  // movie timescale (1000)
    std::vector<std::uint32_t> track_duration(n, 0u);     // per-track timescale

    for (std::size_t i = 0u; i < n; ++i) {
        const TrackConfig& cfg = tracks[i];
        const auto& rs = records[i];
        timescales[i] = mp4_track_timescale(cfg);
        track_ids[i] = cfg.id != 0u ? cfg.id : static_cast<std::uint32_t>(i + 1u);
        max_track_id = std::max(max_track_id, track_ids[i]);
        sizes[i].reserve(rs.size());
        durations[i].reserve(rs.size());
        sync[i].reserve(rs.size());
        ctts[i].reserve(rs.size());

        std::uint64_t total = 0u;
        bool has_non_key = false;
        for (std::size_t j = 0u; j < rs.size(); ++j) {
            sizes[i].push_back(static_cast<std::uint32_t>(std::min<std::size_t>(rs[j].size, 0xFFFFFFFFu)));
            const std::int64_t d = rs[j].duration > 0 ? rs[j].duration : 1000;
            durations[i].push_back(clamp_duration(d));
            ctts[i].push_back(static_cast<std::int64_t>(std::clamp<std::int64_t>(
                rs[j].pts - rs[j].dts, -0x80000000LL, 0x7FFFFFFFLL)));
            if (rs[j].is_key) {
                sync[i].push_back(static_cast<std::uint32_t>(j + 1u));
            } else {
                has_non_key = true;
            }
            total += static_cast<std::uint64_t>(d);
        }
        if (!has_non_key) {
            sync[i].clear();
        }
        // mdhd.duration is in the track timescale; tkhd/mvhd are in the
        // movie timescale (1000).  Keep both instead of reusing the ms value.
        track_duration[i] = static_cast<std::uint32_t>(std::min<std::uint64_t>(total, 0xFFFFFFFFu));
        const std::uint64_t ms = total != 0u ? (total * 1000u + timescales[i] / 2u) / timescales[i] : 0u;
        track_duration_ms[i] = static_cast<std::uint32_t>(std::min<std::uint64_t>(ms, 0xFFFFFFFFu));
        movie_duration_ms = std::max(movie_duration_ms, ms);
    }

    // 3. Chunk tables from the physical write order.  stco/co64 is PER
    // TRACK: each track lists only its own chunks, while stsc.first_chunk
    // is a 1-based index into that track's own chunk list.
    std::vector<std::vector<std::uint32_t>> chunk_offsets32(n);
    std::vector<std::vector<std::uint64_t>> chunk_offsets64(n);
    std::vector<bool> any_large_offset(n, false);
    for (const auto& c : chunks) {
        if (c.track >= n) {
            continue;
        }
        if (c.offset > 0xFFFFFFFFull) {
            any_large_offset[c.track] = true;
        }
        chunk_offsets64[c.track].push_back(c.offset);
        chunk_offsets32[c.track].push_back(
            static_cast<std::uint32_t>(std::min<std::uint64_t>(c.offset, 0xFFFFFFFFull))
        );
    }

    // stsc entries: first_chunk / samples_per_chunk / run length, per track.
    struct StscRun {
        std::uint32_t first_chunk;
        std::uint32_t samples_per_chunk;
        std::uint32_t chunks;
    };
    std::vector<std::vector<StscRun>> stsc(n);
    std::vector<std::uint32_t> per_track_chunk_count(n, 0u);
    for (const auto& c : chunks) {
        if (c.track >= n) {
            continue;
        }
        const std::uint32_t first = per_track_chunk_count[c.track] + 1u;
        const std::uint32_t spc = static_cast<std::uint32_t>(c.sample_count);
        auto& entries = stsc[c.track];
        if (!entries.empty() && entries.back().samples_per_chunk == spc &&
            entries.back().first_chunk + entries.back().chunks == first) {
            // extend the previous run
            entries.back().chunks += 1u;
        } else {
            entries.emplace_back(first, spc, spc);
        }
        ++per_track_chunk_count[c.track];
    }

    // 4. Build moov.
    Mp4Bytes identity;
    be32(identity, 0x00010000u); be32(identity, 0u); be32(identity, 0u);
    be32(identity, 0u); be32(identity, 0x00010000u); be32(identity, 0u);
    be32(identity, 0u); be32(identity, 0u); be32(identity, 0x40000000u);

    Mp4Bytes body = make_mvhd(
        static_cast<std::uint32_t>(std::min<std::uint64_t>(movie_duration_ms, 0xFFFFFFFFu)),
        max_track_id + 1u, identity, mp4_time(metadata.creation_time)
    );

    for (std::size_t i = 0u; i < n; ++i) {
        const TrackConfig& cfg = tracks[i];
        const std::uint32_t sample_count = static_cast<std::uint32_t>(sizes[i].size());

        Mp4Bytes stbl = make_stsd_for_track(cfg);
        append_box_bytes(stbl, make_stts(durations[i]));

        // stsc from the recorded runs; fall back to a single run when empty.
        Mp4Bytes stsc_payload;
        put_zeros(stsc_payload, 4u);
        if (stsc[i].empty()) {
            be32(stsc_payload, sample_count != 0u ? 1u : 0u);
            if (sample_count != 0u) {
                be32(stsc_payload, 1u); be32(stsc_payload, sample_count); be32(stsc_payload, 1u);
            }
        } else {
            be32(stsc_payload, static_cast<std::uint32_t>(stsc[i].size()));
            for (const auto& e : stsc[i]) {
                be32(stsc_payload, e.first_chunk);
                be32(stsc_payload, e.samples_per_chunk);
                be32(stsc_payload, 1u);
            }
        }
        append_box_bytes(stbl, box("stsc", stsc_payload));
        append_box_bytes(stbl, make_stsz(sizes[i]));
        if (!sync[i].empty()) {
            append_box_bytes(stbl, make_stss(sync[i]));
        }
        if (!ctts[i].empty()) {
            bool any_offset = false;
            for (const auto v : ctts[i]) {
                if (v != 0) { any_offset = true; break; }
            }
            if (any_offset) {
                Mp4Bytes cp;
                put_zeros(cp, 4u); // version 1 for signed offsets
                cp[0] = 1u;
                be32(cp, static_cast<std::uint32_t>(ctts[i].size()));
                for (const auto v : ctts[i]) {
                    be32(cp, 1u); // sample_count
                    be32(cp, static_cast<std::uint32_t>(v)); // signed offset
                }
                append_box_bytes(stbl, box("ctts", cp));
            }
        }

        if (any_large_offset[i]) {
            Mp4Bytes cp;
            put_zeros(cp, 4u);
            be32(cp, static_cast<std::uint32_t>(chunk_offsets64[i].size()));
            for (const auto v : chunk_offsets64[i]) {
                be64(cp, v);
            }
            append_box_bytes(stbl, box("co64", cp));
        } else {
            Mp4Bytes cp;
            put_zeros(cp, 4u);
            be32(cp, static_cast<std::uint32_t>(chunk_offsets32[i].size()));
            for (const auto v : chunk_offsets32[i]) {
                be32(cp, v);
            }
            append_box_bytes(stbl, box("stco", cp));
        }

        append_box_bytes(body, [&]() {
            Mp4Bytes trak;
            append_box_bytes(
                trak,
                make_tkhd(track_ids[i], track_duration_ms[i], cfg.width, cfg.height,
                          cfg.is_audio && !cfg.is_video, identity, mp4_time(metadata.creation_time))
            );
            Mp4Bytes mdia;
            append_box_bytes(mdia, make_mdhd(timescales[i], track_duration[i],
                                             mp4_time(metadata.creation_time), cfg.language));
            append_box_bytes(mdia, make_hdlr(cfg.is_audio && !cfg.is_video));
            Mp4Bytes minf;
            if (cfg.is_audio && !cfg.is_video) {
                append_box_bytes(minf, make_smhd());
            } else {
                append_box_bytes(minf, make_vmhd());
            }
            append_box_bytes(minf, make_dinf());
            append_box_bytes(minf, box("stbl", stbl));
            append_box_bytes(mdia, box("minf", minf));
            append_box_bytes(trak, box("mdia", mdia));
            if (mp4_metadata_has_tags(cfg.metadata)) {
                append_box_bytes(trak, make_mp4_udta(cfg.metadata));
            }
            return box("trak", trak);
        }());
    }

    append_box_bytes(body, make_mp4_udta(metadata, true));
    Mp4Bytes moov = box("moov", body);

    if (moov_reserve >= moov.size() + 8u) {
        /* Fast-start: overwrite the reserved region in place.  Pad with a
         * 'free' box so the mdat offset (and all stco/co64 values) stay put. */
        Mp4Bytes region = moov;
        const std::size_t pad = moov_reserve - moov.size();
        if (pad >= 8u) {
            Mp4Bytes free_box;
            be32(free_box, static_cast<std::uint32_t>(pad));
            put4(free_box, "free");
            put_zeros(free_box, pad - 8u);
            region.insert(region.end(), free_box.begin(), free_box.end());
        }
        return sink.patch(moov_reserve_offset, region);
    }

    emit_box(sink, moov);
    return true;
}

}  // namespace detail
}  // namespace mux
}  // namespace bs
