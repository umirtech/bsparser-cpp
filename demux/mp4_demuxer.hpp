#pragma once

#include "es_reconstruct.hpp"
#include "stream.hpp"

#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace bs {
namespace demux {
namespace mp4 {

/*
 * -----------------------------------------------------------
 * MP4 / ISO-BMFF demuxer (limited)
 * -----------------------------------------------------------
 *
 * Extracts the first video track's samples from a non-fragmented
 * MP4 file and reconstructs a self-contained elementary stream:
 *
 *   avc1 / hvc1 / vvc1  -> Annex-B NAL stream (parameter sets
 *                          from avcC/hvcC/vvcC prepended)
 *   av01                -> low-overhead OBU stream
 *   vp09 / vp08         -> IVF-wrapped frames
 *
 * Fragmented MP4 (moof) is not supported.
 */

namespace detail {

inline std::uint32_t read_u32(std::span<const std::uint8_t> d, std::size_t p) {
    if (p + 4 > d.size()) {
        throw std::out_of_range("MP4: truncated box field");
    }
    return (static_cast<std::uint32_t>(d[p]) << 24) | (static_cast<std::uint32_t>(d[p + 1]) << 16) |
           (static_cast<std::uint32_t>(d[p + 2]) << 8) | static_cast<std::uint32_t>(d[p + 3]);
}

inline std::uint64_t read_u64(std::span<const std::uint8_t> d, std::size_t p) {
    if (p + 8 > d.size()) {
        throw std::out_of_range("MP4: truncated box field");
    }
    return (static_cast<std::uint64_t>(read_u32(d, p)) << 32) | read_u32(d, p + 4);
}

inline bool is_fourcc(std::span<const std::uint8_t> d, std::size_t p, const char* s) {
    return p + 4 <= d.size() && std::memcmp(d.data() + p, s, 4) == 0;
}

struct Box {
    std::size_t start = 0;
    std::size_t data = 0;
    std::size_t end = 0;
};

inline Box box_at(std::span<const std::uint8_t> d, std::size_t p) {
    Box b;
    b.start = p;

    if (p + 8 > d.size()) {
        return b;
    }

    std::uint64_t size = read_u32(d, p);

    if (size == 1) {
        if (p + 16 > d.size()) {
            return b;
        }
        size = read_u64(d, p + 8);
        b.data = p + 16;
    } else if (size == 0) {
        size = d.size() - p;
        b.data = p + 8;
    } else {
        b.data = p + 8;
    }

    b.end = p + static_cast<std::size_t>(size);
    if (b.end > d.size()) {
        b.end = d.size();
    }

    return b;
}

/*
 * Find a child box of `type` within [begin, end). Returns its
 * position, or d.size() when absent.
 */
inline std::size_t find_child(
    std::span<const std::uint8_t> d, std::size_t begin, std::size_t end, const char* type
) {
    std::size_t p = begin;

    while (p + 8 <= end) {
        Box b = box_at(d, p);

        if (b.end <= p) {
            break;
        }

        if (p + 4 <= b.end && is_fourcc(d, p + 4, type)) {
            return p;
        }

        p = b.end;
    }

    return d.size();
}

struct VideoEntry {
    bool found = false;
    std::string fourcc;
    std::size_t config_offset = 0;
    std::size_t config_size = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};

/*
 * Parse stsd, locate the first video sample entry.
 */
inline VideoEntry parse_stsd(std::span<const std::uint8_t> d, std::size_t stsd_pos) {
    Box stsd = box_at(d, stsd_pos);

    VideoEntry e;

    if (stsd.data + 8 > stsd.end) {
        return e;
    }

    const std::uint32_t entry_count = read_u32(d, stsd.data + 4);

    std::size_t p = stsd.data + 8;

    for (std::uint32_t i = 0; i < entry_count && p + 8 <= stsd.end; ++i) {
        Box entry = box_at(d, p);

        if (entry.end <= p) {
            break;
        }

        const std::string type = std::string(reinterpret_cast<const char*>(d.data() + p + 4), 4);

        const bool is_video = type == "avc1" || type == "hvc1" || type == "hev1" ||
                              type == "vvc1" || type == "vvcC" || type == "av01" ||
                              type == "vp09" || type == "vp08";

        if (is_video) {
            e.found = true;
            e.fourcc = type;

            /*
             * Video sample entry layout: 6 reserved + 2 data-ref
             * index + 16 (pre_defined/reserved) + width(2) +
             * height(2) + ... = width/height at offset 24.
             */
            if (p + 24 + 4 <= entry.end) {
                e.width = static_cast<std::uint16_t>(read_u32(d, p + 24) >> 16);
                e.height = static_cast<std::uint16_t>(read_u32(d, p + 24) & 0xFFFFu);
            }

            /*
             * Locate the codec-config box (avcC/hvcC/vvcC/av1C).
             * Its offset within the sample entry varies between
             * muxers, so scan for the fourcc.
             */
            for (std::size_t q = p + 8; q + 8 <= entry.end; ++q) {
                if (is_fourcc(d, q + 4, "hvcC") || is_fourcc(d, q + 4, "avcC") ||
                    is_fourcc(d, q + 4, "vvcC") || is_fourcc(d, q + 4, "av1C")) {
                    Box cfg = box_at(d, q);
                    e.config_offset = cfg.data;
                    e.config_size = cfg.end - cfg.data;
                    break;
                }
            }

            return e;
        }

        p = entry.end;
    }

    return e;
}

struct SampleTable {
    std::vector<std::uint32_t> sizes;
    std::vector<std::uint64_t> chunk_offsets;
    std::vector<std::uint32_t> stsc_first;
    std::vector<std::uint32_t> stsc_count;
    std::uint32_t length_size = 4;
};

/*
 * Some muxers write a config length size that disagrees with the
 * actual sample framing. Fall back to 4 bytes when implausible.
 */
inline std::uint32_t effective_length_size(
    const std::vector<std::span<const std::uint8_t>>& samples, std::uint32_t configured
) {
    if (samples.empty() || samples.front().size() <= configured) {
        return 4;
    }

    const auto& s0 = samples.front();

    std::uint32_t first_len = 0;

    for (std::uint32_t i = 0; i < configured; ++i) {
        first_len = (first_len << 8) | s0[i];
    }

    return (first_len == 0 || first_len > s0.size()) ? 4 : configured;
}

inline std::uint32_t samples_per_chunk(const SampleTable& t, std::uint32_t chunk /* 1-based */) {
    std::uint32_t count = 1;

    for (std::size_t i = 0; i < t.stsc_first.size(); ++i) {
        if (t.stsc_first[i] <= chunk) {
            count = t.stsc_count[i];
        }
    }

    return count;
}

/*
 * Reconstruct an Annex-B stream from length-prefixed samples,
 * optionally prepending parameter-set NALs.
 */
inline void emit_annex_b(
    ElementaryStream& out,
    const std::vector<std::span<const std::uint8_t>>& samples,
    std::uint32_t length_size,
    const std::vector<std::uint8_t>& parameter_sets
) {
    auto put_start_code = [&]() {
        out.bytes.push_back(0x00);
        out.bytes.push_back(0x00);
        out.bytes.push_back(0x01);
    };

    auto put_nal = [&](std::span<const std::uint8_t> nal) {
        put_start_code();
        out.bytes.insert(out.bytes.end(), nal.begin(), nal.end());
    };

    out.bytes.insert(out.bytes.end(), parameter_sets.begin(), parameter_sets.end());

    for (const auto& sample : samples) {
        std::size_t pos = 0;

        while (pos + length_size <= sample.size()) {
            std::uint32_t nal_size = 0;

            for (std::uint32_t i = 0; i < length_size; ++i) {
                nal_size = (nal_size << 8) | sample[pos + i];
            }

            pos += length_size;

            if (pos + nal_size > sample.size()) {
                break;
            }

            put_nal(std::span<const std::uint8_t>(sample.data() + pos, nal_size));

            pos += nal_size;
        }
    }
}

}  // namespace detail

/*
 * Demux an MP4 file into an elementary stream.
 */
[[nodiscard]]
inline ElementaryStream demux_mp4(std::span<const std::uint8_t> data) {
    ElementaryStream out;

    if (data.size() < 16) {
        return out;
    }

    const std::size_t moov = detail::find_child(data, 0, data.size(), "moov");

    if (moov == data.size()) {
        return out;
    }

    detail::Box moov_box = detail::box_at(data, moov);

    const std::size_t trak = detail::find_child(data, moov_box.data, moov_box.end, "trak");

    if (trak == data.size()) {
        return out;
    }

    detail::Box trak_box = detail::box_at(data, trak);

    const std::size_t mdia = detail::find_child(data, trak_box.data, trak_box.end, "mdia");

    if (mdia == data.size()) {
        return out;
    }

    detail::Box mdia_box = detail::box_at(data, mdia);

    const std::size_t minf = detail::find_child(data, mdia_box.data, mdia_box.end, "minf");

    if (minf == data.size()) {
        return out;
    }

    detail::Box minf_box = detail::box_at(data, minf);

    const std::size_t stbl = detail::find_child(data, minf_box.data, minf_box.end, "stbl");

    if (stbl == data.size()) {
        return out;
    }

    detail::Box stbl_box = detail::box_at(data, stbl);

    const std::size_t stsd = detail::find_child(data, stbl_box.data, stbl_box.end, "stsd");

    const std::size_t stsz = detail::find_child(data, stbl_box.data, stbl_box.end, "stsz");

    const std::size_t stsc = detail::find_child(data, stbl_box.data, stbl_box.end, "stsc");

    std::size_t stco = detail::find_child(data, stbl_box.data, stbl_box.end, "stco");
    std::size_t co64 = detail::find_child(data, stbl_box.data, stbl_box.end, "co64");

    if (stsd == data.size() || stsz == data.size() || stsc == data.size() ||
        (stco == data.size() && co64 == data.size())) {
        return out;
    }

    const detail::VideoEntry entry = detail::parse_stsd(data, stsd);

    if (!entry.found) {
        return out;
    }

    detail::SampleTable t;

    {
        detail::Box b = detail::box_at(data, stsz);
        if (b.data + 8 > b.end) {
            return out;
        }
        const std::uint32_t sample_size = detail::read_u32(data, b.data + 4);
        std::uint32_t sample_count = detail::read_u32(data, b.data + 8);

        /*
         * Guard against a malicious sample_count causing a huge
         * allocation (CWE-400). A million samples per stsz far
         * exceeds any real media file.
         */
        constexpr std::uint32_t kMaxSamples = 1000000u;

        if (sample_count > kMaxSamples) {
            sample_count = kMaxSamples;
        }

        if (sample_size != 0) {
            t.sizes.assign(sample_count, sample_size);
        } else {
            for (std::uint32_t i = 0; i < sample_count; ++i) {
                if (b.data + 12 + i * 4 + 4 > b.end) {
                    break;
                }
                t.sizes.push_back(detail::read_u32(data, b.data + 12 + i * 4));
            }
        }
    }

    {
        detail::Box b = detail::box_at(data, stsc);
        if (b.data + 8 > b.end) {
            return out;
        }
        const std::uint32_t count = detail::read_u32(data, b.data + 4);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t p = b.data + 8 + i * 12;
            if (p + 12 > b.end) {
                break;
            }
            t.stsc_first.push_back(detail::read_u32(data, p));
            t.stsc_count.push_back(detail::read_u32(data, p + 4));
        }
    }

    if (co64 != data.size()) {
        detail::Box b = detail::box_at(data, co64);
        const std::uint32_t count = detail::read_u32(data, b.data + 4);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t p = b.data + 8 + i * 8;
            if (p + 8 > b.end) {
                break;
            }
            t.chunk_offsets.push_back(detail::read_u64(data, p));
        }
    } else {
        detail::Box b = detail::box_at(data, stco);
        const std::uint32_t count = detail::read_u32(data, b.data + 4);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t p = b.data + 8 + i * 4;
            if (p + 4 > b.end) {
                break;
            }
            t.chunk_offsets.push_back(detail::read_u32(data, p));
        }
    }

    if (t.chunk_offsets.empty()) {
        return out;
    }

    out.width = entry.width;
    out.height = entry.height;
    out.codec_name = entry.fourcc;

    /*
     * Extract the samples.
     */
    std::vector<std::span<const std::uint8_t>> samples;

    std::uint32_t sample_index = 0;

    for (std::uint32_t chunk = 0; chunk < t.chunk_offsets.size(); ++chunk) {
        const std::uint32_t spc = detail::samples_per_chunk(t, chunk + 1);

        std::uint64_t off = t.chunk_offsets[chunk];

        for (std::uint32_t j = 0; j < spc; ++j) {
            if (sample_index >= t.sizes.size()) {
                break;
            }

            const std::uint32_t size = t.sizes[sample_index++];

            if (off + size > data.size()) {
                continue;
            }

            /* reference the input buffer; no per-sample copy */
            samples.push_back(data.subspan(static_cast<std::size_t>(off), size));

            off += size;
        }
    }

    if (samples.empty()) {
        return out;
    }

    /*
     * Reconstruct the elementary stream per codec.
     */
    if (entry.fourcc == "avc1") {
        t.length_size = 4;
        std::vector<std::uint8_t> param_sets;

        if (entry.config_offset + 6 <= entry.config_offset + entry.config_size) {
            t.length_size = static_cast<std::uint32_t>(data[entry.config_offset + 4] & 3u) + 1u;
        }

        if (entry.config_size >= 7) {
            const std::uint8_t num_sps = data[entry.config_offset + 5] & 0x1Fu;

            std::size_t p = entry.config_offset + 6;

            for (std::uint8_t i = 0; i < num_sps && p + 2 <= data.size(); ++i) {
                const std::uint16_t len = static_cast<std::uint16_t>((data[p] << 8) | data[p + 1]);

                p += 2;

                if (p + len > data.size()) {
                    break;
                }

                param_sets.push_back(0x00);
                param_sets.push_back(0x00);
                param_sets.push_back(0x01);
                param_sets.insert(param_sets.end(), data.begin() + p, data.begin() + p + len);
                p += len;
            }

            if (p < data.size()) {
                const std::uint8_t num_pps = data[p++];

                for (std::uint8_t i = 0; i < num_pps && p + 2 <= data.size(); ++i) {
                    const std::uint16_t len =
                        static_cast<std::uint16_t>((data[p] << 8) | data[p + 1]);

                    p += 2;

                    if (p + len > data.size()) {
                        break;
                    }

                    param_sets.push_back(0x00);
                    param_sets.push_back(0x00);
                    param_sets.push_back(0x01);
                    param_sets.insert(param_sets.end(), data.begin() + p, data.begin() + p + len);
                    p += len;
                }
            }
        }

        out.codec = Codec::Avc;
        out.framing = NalFramingMode::AnnexB;
        t.length_size = detail::effective_length_size(samples, t.length_size);
        detail::emit_annex_b(out, samples, t.length_size, param_sets);
        out.ok = true;
        return out;
    }

    if (entry.fourcc == "hvc1" || entry.fourcc == "hev1" || entry.fourcc == "vvc1") {
        t.length_size = 4;
        std::vector<std::uint8_t> param_sets;

        const bool is_hvc = entry.fourcc == "hvc1" || entry.fourcc == "hev1";

        /* lengthSizeMinusOne: hvcC byte 18, vvcC byte 14. */
        const std::size_t lso = is_hvc ? 18u : 14u;

        if (entry.config_size > lso) {
            t.length_size = static_cast<std::uint32_t>(data[entry.config_offset + lso] & 3u) + 1u;
        }

        t.length_size = detail::effective_length_size(samples, t.length_size);

        out.codec = is_hvc ? Codec::Hevc : Codec::Vvc;
        out.framing = NalFramingMode::AnnexB;

        es::prepend_param_sets(out, data.subspan(entry.config_offset, entry.config_size), true);

        for (const auto& sample : samples) {
            es::emit_length_prefixed(out, sample, t.length_size);
        }

        out.ok = true;
        return out;
    }

    if (entry.fourcc == "av01") {
        out.codec = Codec::Av1;
        out.framing = NalFramingMode::Obu;

        for (const auto& sample : samples) {
            out.bytes.insert(out.bytes.end(), sample.begin(), sample.end());
        }

        out.ok = true;
        return out;
    }

    if (entry.fourcc == "vp09" || entry.fourcc == "vp08") {
        out.codec = entry.fourcc == "vp09" ? Codec::Vp9 : Codec::Vp8;
        out.framing = NalFramingMode::Ivf;

        /*
         * Wrap the frames in a minimal IVF container.
         */
        const char* fourcc = entry.fourcc == "vp09" ? "VP90" : "VP80";

        out.bytes.insert(out.bytes.end(), {'D', 'K', 'I', 'F'});
        out.bytes.insert(out.bytes.end(), {0x00, 0x00}); /* version */
        out.bytes.insert(out.bytes.end(), {0x20, 0x00}); /* header size */
        out.bytes.insert(out.bytes.end(), fourcc, fourcc + 4);
        out.bytes.push_back(static_cast<std::uint8_t>(entry.width & 0xFFu));
        out.bytes.push_back(static_cast<std::uint8_t>(entry.width >> 8));
        out.bytes.push_back(static_cast<std::uint8_t>(entry.height & 0xFFu));
        out.bytes.push_back(static_cast<std::uint8_t>(entry.height >> 8));
        out.bytes.insert(out.bytes.end(), {0x00, 0x00, 0x00, 0x1E}); /* rate */
        out.bytes.insert(out.bytes.end(), {0x00, 0x00, 0x00, 0x01}); /* scale */
        out.bytes.insert(out.bytes.end(), {0x00, 0x00, 0x00, 0x00}); /* frame count */
        out.bytes.insert(out.bytes.end(), {0x00, 0x00, 0x00, 0x00}); /* unused */

        std::uint64_t ts = 0;

        for (const auto& sample : samples) {
            const std::uint32_t size = static_cast<std::uint32_t>(sample.size());

            out.bytes.push_back(static_cast<std::uint8_t>(size & 0xFFu));
            out.bytes.push_back(static_cast<std::uint8_t>((size >> 8) & 0xFFu));
            out.bytes.push_back(static_cast<std::uint8_t>((size >> 16) & 0xFFu));
            out.bytes.push_back(static_cast<std::uint8_t>((size >> 24) & 0xFFu));

            for (int i = 0; i < 8; ++i) {
                out.bytes.push_back(static_cast<std::uint8_t>((ts >> (i * 8)) & 0xFFu));
            }

            ts += 1;

            out.bytes.insert(out.bytes.end(), sample.begin(), sample.end());
        }

        out.ok = true;
        return out;
    }

    return out;
}

}  // namespace mp4
}  // namespace demux
}  // namespace bs
