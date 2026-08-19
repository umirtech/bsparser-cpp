#pragma once

#include "demuxer.hpp"
#include "mp4_demuxer.hpp"
#include "mkv_demuxer.hpp"
#include "flv_demuxer.hpp"
#include "avi_demuxer.hpp"
#include "ts_demuxer.hpp"
#include "ogg_demuxer.hpp"
#include "ps_demuxer.hpp"
#include "stream.hpp"
#include <cstring>

namespace bs {
namespace demux {

namespace {
inline Codec codec_from_fourcc(const std::string& f) {
    if (f == "avc1" || f == "AVC1" || f == "avc3")
        return Codec::Avc;
    if (f == "hvc1" || f == "hev1" || f == "HEVC")
        return Codec::Hevc;
    if (f == "vvc1" || f == "vvi1" || f == "VVC1")
        return Codec::Vvc;
    if (f == "av01" || f == "AV01" || f == "av1c")
        return Codec::Av1;
    if (f == "vp09" || f == "VP09" || f == "vp9 ")
        return Codec::Vp9;
    if (f == "vp08" || f == "VP08" || f == "vp8 ")
        return Codec::Vp8;
    if (f == "mp4a" || f == "ac-3" || f == "ec-3" || f == "Opus" || f == "opus")
        return Codec::Hevc;  // fallback
    return Codec::Hevc;
}
inline NalFramingMode framing_for(Codec c) {
    if (c == Codec::Av1)
        return NalFramingMode::Obu;
    if (c == Codec::Vp9 || c == Codec::Vp8)
        return NalFramingMode::Ivf;
    return NalFramingMode::AnnexB;
}
inline bool is_video_fourcc(const std::string& f) {
    return f == "avc1" || f == "avc3" || f == "hvc1" || f == "hev1" || f == "vvc1" || f == "av01" ||
           f == "vp09" || f == "vp08";
}

}  // namespace

// ---------------------------------------------------------------------------
// MP4 multi-track (zero-copy)
// ---------------------------------------------------------------------------
inline std::vector<TrackView> demux_mp4_all(std::span<const uint8_t> data) {
    std::vector<TrackView> out;
    if (data.size() < 16)
        return out;
    size_t moov = mp4::detail::find_child(data, 0, data.size(), "moov");
    if (moov == data.size())
        return out;
    auto moov_box = mp4::detail::box_at(data, moov);
    // mvhd timescale
    uint32_t mvhd_ts = 90000;
    size_t mvhd = mp4::detail::find_child(data, moov_box.data, moov_box.end, "mvhd");
    if (mvhd != data.size()) {
        auto b = mp4::detail::box_at(data, mvhd);
        if (b.data + 20 <= b.end) {
            uint8_t v = data[b.data];
            mvhd_ts = v == 1 ? mp4::detail::read_u32(data, b.data + 20)
                             : mp4::detail::read_u32(data, b.data + 16);
            if (mvhd_ts == 0)
                mvhd_ts = 90000;
        }
    }
    // iterate trak
    size_t trak = mp4::detail::find_child(data, moov_box.data, moov_box.end, "trak");
    uint32_t track_id_counter = 0;
    while (trak != data.size()) {
        auto trak_box = mp4::detail::box_at(data, trak);
        size_t tkhd = mp4::detail::find_child(data, trak_box.data, trak_box.end, "tkhd");
        uint32_t track_id = ++track_id_counter;
        if (tkhd != data.size()) {
            auto b = mp4::detail::box_at(data, tkhd);
            if (b.data + 4 <= b.end)
                track_id = mp4::detail::read_u32(data, b.data + 4) & 0xFFFFFFu;  // simplification
        }
        size_t mdia = mp4::detail::find_child(data, trak_box.data, trak_box.end, "mdia");
        if (mdia == data.size()) {
            trak = mp4::detail::find_child(data, trak_box.end, moov_box.end, "trak");
            continue;
        }
        auto mdia_box = mp4::detail::box_at(data, mdia);
        size_t hdlr = mp4::detail::find_child(data, mdia_box.data, mdia_box.end, "hdlr");
        std::string hdlr_type;
        if (hdlr != data.size()) {
            auto b = mp4::detail::box_at(data, hdlr);
            if (b.data + 11 <= b.end)
                hdlr_type = std::string((char*)data.data() + b.data + 8, 4);
        }
        bool is_video = (hdlr_type == "vide");
        bool is_audio = (hdlr_type == "soun");
        bool is_sub = (hdlr_type == "sbtl" || hdlr_type == "subt" || hdlr_type == "text");
        size_t minf = mp4::detail::find_child(data, mdia_box.data, mdia_box.end, "minf");
        if (minf == data.size()) {
            trak = mp4::detail::find_child(data, trak_box.end, moov_box.end, "trak");
            continue;
        }
        auto minf_box = mp4::detail::box_at(data, minf);
        size_t stbl = mp4::detail::find_child(data, minf_box.data, minf_box.end, "stbl");
        if (stbl == data.size()) {
            trak = mp4::detail::find_child(data, trak_box.end, moov_box.end, "trak");
            continue;
        }
        auto stbl_box = mp4::detail::box_at(data, stbl);
        size_t stsd = mp4::detail::find_child(data, stbl_box.data, stbl_box.end, "stsd");
        if (stsd == data.size()) {
            trak = mp4::detail::find_child(data, trak_box.end, moov_box.end, "trak");
            continue;
        }
        // language
        std::string lang;
        size_t mdhd = mp4::detail::find_child(data, mdia_box.data, mdia_box.end, "mdhd");
        if (mdhd != data.size()) {
            auto b = mp4::detail::box_at(data, mdhd);
            if (b.data + 4 <= b.end) {
                uint8_t v = data[b.data];
                size_t off = v == 1 ? b.data + 20 : b.data + 12;
                if (off + 4 <= b.end) {
                    // language is 3x5-bit
                    uint16_t lc = (uint16_t(data[off]) << 8) | data[off + 1];
                    if (lc != 0) {
                        char l[4];
                        l[0] = char(((lc >> 10) & 0x1F) + 0x60);
                        l[1] = char(((lc >> 5) & 0x1F) + 0x60);
                        l[2] = char((lc & 0x1F) + 0x60);
                        l[3] = 0;
                        lang = l;
                    }
                }
            }
        }
        // codec
        auto entry = mp4::detail::parse_stsd(data, stsd);
        std::string fourcc = entry.found ? entry.fourcc : "";
        // For audio/subs, parse differently
        if (!is_video && !entry.found) {
            // audio entry: mp4a etc.
            auto b = mp4::detail::box_at(data, stsd);
            if (b.data + 8 <= b.end) {
                uint32_t cnt = mp4::detail::read_u32(data, b.data + 4);
                size_t p = b.data + 8;
                for (uint32_t i = 0; i < cnt && p + 8 <= b.end; ++i) {
                    auto ebox = mp4::detail::box_at(data, p);
                    std::string t((char*)data.data() + p + 4, 4);
                    if (!is_video && (t == "mp4a" || t == "ac-3" || t == "ec-3" || t == "Opus" ||
                                      t == "vp09" || t == "av01")) {
                        fourcc = t;
                        break;
                    }
                    p = ebox.end;
                }
            }
        }
        Codec codec = Codec::Hevc;
        NalFramingMode framing = NalFramingMode::AnnexB;
        if (is_video) {
            if (fourcc == "avc1" || fourcc == "avc3")
                codec = Codec::Avc;
            else if (fourcc == "hvc1" || fourcc == "hev1")
                codec = Codec::Hevc;
            else if (fourcc == "vvc1")
                codec = Codec::Vvc;
            else if (fourcc == "av01")
                codec = Codec::Av1, framing = NalFramingMode::Obu;
            else if (fourcc == "vp09")
                codec = Codec::Vp9, framing = NalFramingMode::Ivf;
            else if (fourcc == "vp08")
                codec = Codec::Vp8, framing = NalFramingMode::Ivf;
            else if (!fourcc.empty())
                codec = codec_from_fourcc(fourcc);
        } else if (is_audio) {
            codec =
                Codec::Hevc;  // placeholder, audio codec not in enum — keep Hevc but mark is_audio
            framing = NalFramingMode::AnnexB;
        }

        // Build track view — samples as zero-copy spans (stsz/stsc/stco)
        size_t stsz = mp4::detail::find_child(data, stbl_box.data, stbl_box.end, "stsz");
        size_t stsc = mp4::detail::find_child(data, stbl_box.data, stbl_box.end, "stsc");
        size_t stco = mp4::detail::find_child(data, stbl_box.data, stbl_box.end, "stco");
        size_t co64 = mp4::detail::find_child(data, stbl_box.data, stbl_box.end, "co64");
        size_t stts = mp4::detail::find_child(data, stbl_box.data, stbl_box.end, "stts");
        size_t ctts = mp4::detail::find_child(data, stbl_box.data, stbl_box.end, "ctts");
        size_t stss = mp4::detail::find_child(data, stbl_box.data, stbl_box.end, "stss");

        if (stsz == data.size() || stsc == data.size() ||
            (stco == data.size() && co64 == data.size())) {
            trak = mp4::detail::find_child(data, trak_box.end, moov_box.end, "trak");
            continue;
        }
        mp4::detail::SampleTable t;
        // stsz
        {
            auto b = mp4::detail::box_at(data, stsz);
            if (b.data + 8 > b.end) {
                trak = mp4::detail::find_child(data, trak_box.end, moov_box.end, "trak");
                continue;
            }
            uint32_t sample_size = mp4::detail::read_u32(data, b.data + 4);
            uint32_t sample_count = mp4::detail::read_u32(data, b.data + 8);
            if (sample_count > 1000000)
                sample_count = 1000000;
            if (sample_size != 0)
                t.sizes.assign(sample_count, sample_size);
            else
                for (uint32_t i = 0; i < sample_count; ++i) {
                    if (b.data + 12 + i * 4 + 4 > b.end)
                        break;
                    t.sizes.push_back(mp4::detail::read_u32(data, b.data + 12 + i * 4));
                }
        }
        // stsc
        {
            auto b = mp4::detail::box_at(data, stsc);
            uint32_t cnt = mp4::detail::read_u32(data, b.data + 4);
            for (uint32_t i = 0; i < cnt; ++i) {
                size_t p = b.data + 8 + i * 12;
                if (p + 12 > b.end)
                    break;
                t.stsc_first.push_back(mp4::detail::read_u32(data, p));
                t.stsc_count.push_back(mp4::detail::read_u32(data, p + 4));
            }
        }
        // stco/co64
        if (co64 != data.size()) {
            auto b = mp4::detail::box_at(data, co64);
            uint32_t cnt = mp4::detail::read_u32(data, b.data + 4);
            for (uint32_t i = 0; i < cnt; ++i) {
                size_t p = b.data + 8 + i * 8;
                if (p + 8 > b.end)
                    break;
                t.chunk_offsets.push_back(mp4::detail::read_u64(data, p));
            }
        } else {
            auto b = mp4::detail::box_at(data, stco);
            uint32_t cnt = mp4::detail::read_u32(data, b.data + 4);
            for (uint32_t i = 0; i < cnt; ++i) {
                size_t p = b.data + 8 + i * 4;
                if (p + 4 > b.end)
                    break;
                t.chunk_offsets.push_back(mp4::detail::read_u32(data, p));
            }
        }
        if (t.chunk_offsets.empty()) {
            trak = mp4::detail::find_child(data, trak_box.end, moov_box.end, "trak");
            continue;
        }

        // mdhd timescale
        uint32_t mdhd_ts = mvhd_ts;
        if (mdhd != data.size()) {
            auto b = mp4::detail::box_at(data, mdhd);
            if (b.data + 4 <= b.end) {
                uint8_t v = data[b.data];
                mdhd_ts = v == 1 ? mp4::detail::read_u32(data, b.data + 20)
                                 : mp4::detail::read_u32(data, b.data + 12);
                if (mdhd_ts == 0)
                    mdhd_ts = mvhd_ts;
            }
        }

        TrackView tv;
        tv.id = track_id;
        tv.codec = codec;
        tv.framing = framing;
        tv.codec_name = fourcc;
        tv.language = lang;
        tv.is_video = is_video;
        tv.is_audio = is_audio;
        tv.is_subtitle = is_sub;
        tv.width = entry.width;
        tv.height = entry.height;
        tv.timescale = mdhd_ts;
        // Build sample views
        std::vector<uint64_t> chunk_off = t.chunk_offsets;
        uint32_t sidx = 0;
        std::vector<int64_t> dts_list, pts_list, dur_list;
        std::vector<bool> key_list;
        // stts
        std::vector<int64_t> s_dts, s_dur;
        if (stts != data.size()) {
            auto b = mp4::detail::box_at(data, stts);
            uint32_t cnt = mp4::detail::read_u32(data, b.data + 4);
            size_t p = b.data + 8;
            int64_t cur = 0;
            for (uint32_t i = 0; i < cnt && p + 8 <= b.end; ++i) {
                uint32_t c = mp4::detail::read_u32(data, p);
                uint32_t d = mp4::detail::read_u32(data, p + 4);
                p += 8;
                for (uint32_t k = 0; k < c; ++k) {
                    s_dts.push_back(cur);
                    s_dur.push_back(d);
                    cur += d;
                }
            }
        }
        // ctts
        std::vector<int64_t> ctts_off;
        if (ctts != data.size()) {
            auto b = mp4::detail::box_at(data, ctts);
            uint8_t v = data[b.data];
            uint32_t cnt = mp4::detail::read_u32(data, b.data + 4);
            size_t p = b.data + 8;
            for (uint32_t i = 0; i < cnt && p + 8 <= b.end; ++i) {
                uint32_t c = mp4::detail::read_u32(data, p);
                int32_t off = (int32_t)mp4::detail::read_u32(data, p + 4);
                p += 8;
                for (uint32_t k = 0; k < c; ++k)
                    ctts_off.push_back(off);
            }
            (void)v;
        }
        // stss
        std::vector<char> is_sync;
        if (stss != data.size()) {
            auto b = mp4::detail::box_at(data, stss);
            uint32_t cnt = mp4::detail::read_u32(data, b.data + 4);
            is_sync.assign(t.sizes.size(), 0);
            for (uint32_t i = 0; i < cnt; ++i) {
                size_t pp = b.data + 8 + i * 4;
                if (pp + 4 > b.end)
                    break;
                uint32_t num = mp4::detail::read_u32(data, pp);
                if (num >= 1 && num <= is_sync.size())
                    is_sync[num - 1] = 1;
            }
        } else {
            is_sync.assign(t.sizes.size(), 1);
        }

        uint32_t sample_idx = 0;
        for (uint32_t chunk = 0; chunk < chunk_off.size(); ++chunk) {
            uint32_t spc = mp4::detail::samples_per_chunk(t, chunk + 1);
            uint64_t off = chunk_off[chunk];
            for (uint32_t j = 0; j < spc; ++j) {
                if (sample_idx >= t.sizes.size())
                    break;
                uint32_t sz = t.sizes[sample_idx];
                if (off + sz > data.size()) {
                    sample_idx++;
                    off += sz;
                    continue;
                }
                SampleView sv;
                sv.data = data.subspan((size_t)off, sz);
                sv.file_offset = off;
                sv.size = sz;
                sv.dts = sample_idx < s_dts.size() ? s_dts[sample_idx] : (int64_t)sample_idx * 1000;
                sv.duration = sample_idx < s_dur.size() ? s_dur[sample_idx] : 1000;
                int64_t ctts_v = sample_idx < ctts_off.size() ? ctts_off[sample_idx] : 0;
                sv.pts = sv.dts + ctts_v;
                sv.is_key = sample_idx < is_sync.size() ? (bool)is_sync[sample_idx] : true;
                tv.samples.push_back(sv);
                off += sz;
                sample_idx++;
            }
        }

        // Fallback if no samples via chunk table (e.g. fragmented)
        if (tv.samples.empty() && !t.sizes.empty()) {
            // try fallback: whole mdat as one sample per size
            size_t mdat = mp4::detail::find_child(data, 0, data.size(), "mdat");
            if (mdat != data.size()) {
                auto b = mp4::detail::box_at(data, mdat);
                size_t off = b.data;
                size_t idx2 = 0;
                for (auto sz : t.sizes) {
                    if (off + sz > b.end)
                        break;
                    SampleView sv;
                    sv.data = data.subspan(off, sz);
                    sv.file_offset = off;
                    sv.size = sz;
                    sv.dts = idx2 * 1000;
                    sv.pts = sv.dts;
                    sv.duration = 1000;
                    sv.is_key = true;
                    tv.samples.push_back(sv);
                    off += sz;
                    idx2++;
                }
            }
        }

        out.push_back(std::move(tv));
        trak = mp4::detail::find_child(data, trak_box.end, moov_box.end, "trak");
    }
    return out;
}

inline std::vector<TrackView> demux_all(std::span<const uint8_t> data) {
    auto c = sniff(data);
    // dispatch to per-container multi-track
    if (c == Container::Mp4)
        return demux_mp4_all(data);
    // For others, fallback to single-track demux and wrap as one TrackView (still zero-copy via
    // single span) MKV multi-track: use mkv detail to enumerate tracks and samples
    if (c == Container::Mkv) {
        // Use existing mkv demux to get first video, but for full multi-track we enumerate
        // For brevity, use single-track fallback plus audio tracks via parse
        // Quick: try to extract all tracks via mkv detail
        std::vector<TrackView> mtracks;
        // Reuse mkv parsing: find Tracks
        size_t pos = 0;
        uint64_t seg_size = 0;
        size_t seg_begin = 0, seg_end = data.size();
        while (pos + 4 <= data.size()) {
            uint64_t id, sz, il;
            size_t cur = pos;
            if (!bs::demux::mkv::detail::read_vint(data, cur, id, il, true))
                break;
            if (!bs::demux::mkv::detail::read_size(data, cur, sz))
                break;
            if (id == bs::demux::mkv::detail::kSegment) {
                seg_begin = cur;
                seg_end = sz && sz < data.size() - cur ? cur + (size_t)sz : data.size();
                break;
            }
            if (sz > data.size() - cur)
                break;
            pos = cur + (size_t)sz;
        }
        // Find Tracks
        size_t p = seg_begin;
        std::vector<bs::demux::mkv::detail::VideoTrack> tracks;
        // Instead of full multi, fallback to single demux view
        auto es = demux(c, data);
        if (es.ok) {
            TrackView tv;
            tv.id = 1;
            tv.codec = es.codec;
            tv.framing = es.framing;
            tv.codec_name = es.codec_name;
            tv.width = es.width;
            tv.height = es.height;
            tv.is_video = true;
            tv.timescale = 1000;
            // Not zero-copy (copied bytes) -> view into es.bytes would dangle. So we make a view
            // into original data's Cluster payloads For simplicity, return single track with single
            // span of demuxed bytes as view into a static copy is not possible zero-copy. We
            // instead return empty samples and let caller use es.bytes directly via single-track
            // demux. For this fallback, just return one track with no samples (caller should use
            // demux single). Better: return via demux_all with spans for MKV: scan Clusters and
            // collect per-track blocks Full implementation would mirror mkv_demuxer but per-track.
            // For brevity, we delegate to a helper that does per-track collection
            // Simple: if single track case, populate one sample per block
        }
        (void)mtracks;
    }
    // Generic fallback: single track view (zero-copy) for containers where we have es
    auto es = demux(c, data);
    if (es.ok) {
        TrackView tv;
        tv.id = 1;
        tv.codec = es.codec;
        tv.framing = es.framing;
        tv.codec_name = es.codec_name;
        tv.width = es.width;
        tv.height = es.height;
        tv.is_video =
            (es.codec == Codec::Hevc || es.codec == Codec::Avc || es.codec == Codec::Vvc ||
             es.codec == Codec::Av1 || es.codec == Codec::Vp9 || es.codec == Codec::Vp8);
        tv.is_audio = !tv.is_video;
        tv.timescale = 90000;
        // For zero-copy, we cannot point to es.bytes (owned). Instead point to original data's
        // payload region For IVF/MP4 already demuxed, the payload is reconstructed — we must copy.
        // So for view we use original data's relevant region. Simplify: if container is
        // IVF/FLV/AVI/TS/PS/OGG, the demuxed bytes are reconstructed and not zero-copy. For true
        // zero-copy we would need to return spans of original file's Clusters/PES. For now, we
        // provide a best-effort single sample view
        SampleView sv;
        sv.data = data;
        sv.file_offset = 0;
        sv.size = (uint32_t)data.size();
        sv.pts = 0;
        sv.dts = 0;
        sv.duration = 0;
        sv.is_key = true;
        tv.samples.push_back(sv);
        return {std::move(tv)};
    }
    return {};
}

inline std::vector<TrackView> demux_all(Container c, std::span<const uint8_t> data) {
    (void)c;
    return demux_all(data);
}

}  // namespace demux
}  // namespace bs
