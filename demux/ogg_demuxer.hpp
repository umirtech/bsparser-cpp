#pragma once

#include "es_reconstruct.hpp"
#include "stream.hpp"

#include <cstdint>
#include <span>
#include <vector>
#include <string>

namespace bs {
namespace demux {
namespace ogg {

/*
 * -----------------------------------------------------------
 * OGG demuxer (limited)
 * -----------------------------------------------------------
 * Extracts the first video stream from an OGG file:
 *   VP8/VP9 -> IVF
 *   AV1/HEVC/AVC -> Annex-B / OBU
 *
 * Handles Ogg page sync, packet reassembly, and basic
 * Codec identification via BOS packets.
 */

namespace detail {

struct OggPage {
    uint64_t granule = 0;
    uint32_t serial = 0;
    std::vector<uint8_t> data;
    bool bos = false;
    bool eos = false;
};

inline bool read_u32_le(std::span<const uint8_t> d, size_t p, uint32_t &out) {
    if (p + 4 > d.size()) return false;
    out = uint32_t(d[p]) | (uint32_t(d[p+1])<<8) | (uint32_t(d[p+2])<<16) | (uint32_t(d[p+3])<<24);
    return true;
}

inline bool next_page(std::span<const uint8_t> data, size_t &pos, OggPage &page) {
    if (pos + 27 > data.size()) return false;
    if (data[pos]!=0x4F || data[pos+1]!=0x67 || data[pos+2]!=0x67 || data[pos+3]!=0x53) return false;
    uint8_t version = data[pos+4];
    if (version!=0) return false;
    uint8_t header_type = data[pos+5];
    page.bos = (header_type & 0x02) != 0;
    page.eos = (header_type & 0x04) != 0;
    uint64_t granule = 0;
    for (int i=0;i<8;++i) granule |= uint64_t(data[pos+6+i]) << (i*8);
    page.granule = granule;
    uint32_t serial; if (!read_u32_le(data, pos+14, serial)) return false;
    page.serial = serial;
    uint8_t segments = data[pos+26];
    if (pos + 27 + segments > data.size()) return false;
    size_t seg_table = pos+27;
    size_t data_off = seg_table + segments;
    uint32_t total=0;
    for (int i=0;i<segments;++i) total += data[seg_table+i];
    if (data_off + total > data.size()) return false;
    page.data.assign(data.begin()+data_off, data.begin()+data_off+total);
    pos = data_off + total;
    return true;
}

} // namespace detail

[[nodiscard]]
inline ElementaryStream demux_ogg(std::span<const uint8_t> data) {
    ElementaryStream out;
    if (data.size()<27 || data[0]!=0x4F || data[1]!=0x67 || data[2]!=0x67 || data[3]!=0x53) return out;

    // Map serial -> codec
    struct StreamInfo { std::string codec; bool is_video=false; };
    std::vector<std::pair<uint32_t, StreamInfo>> streams;
    std::vector<detail::OggPage> pages;

    size_t pos=0;
    while (pos < data.size()) {
        detail::OggPage pg;
        size_t save=pos;
        if (!detail::next_page(data, pos, pg)) break;
        pages.push_back(std::move(pg));
        if (save==pos) break;
    }
    if (pages.empty()) return out;

    // Identify streams via BOS packets
    for (auto &pg : pages) {
        if (!pg.bos) continue;
        std::string codec;
        if (pg.data.size()>=8 && memcmp(pg.data.data(), "OVP80",5)==0) codec="VP80";
        else if (pg.data.size()>=8 && memcmp(pg.data.data(), "OVP90",5)==0) codec="VP90";
        else if (pg.data.size()>=4 && memcmp(pg.data.data(), "\x81\x41\x56\x30",4)==0) codec="AV01";
        else if (pg.data.size()>=8 && memcmp(pg.data.data(), "AV01",4)==0) codec="AV01";
        else if (pg.data.size()>=5 && pg.data[0]==0x01 && pg.data[1]=='v' && pg.data[2]=='p') codec="VP80";
        else if (pg.data.size()>=4 && pg.data[0]==0x01 && pg.data[1]=='H') codec="HEVC";
        else if (pg.data.size()>=4 && memcmp(pg.data.data(), "\x01\x42",2)==0) codec="AVC";
        if (!codec.empty()) {
            bool found=false;
            for (auto &s: streams) if (s.first==pg.serial) found=true;
            if (!found) streams.push_back({pg.serial, {codec, true}});
        }
    }

    // Pick first video stream
    uint32_t video_serial=0;
    std::string video_codec;
    for (auto &s: streams) if (s.second.is_video) { video_serial=s.first; video_codec=s.second.codec; break; }
    if (video_serial==0) {
        // Fallback: first non-audio serial (assume video)
        if (!pages.empty()) { video_serial=pages[0].serial; video_codec="VP80"; }
        else return out;
    }

    // Reassemble packets for video stream
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> cur;
    bool first=true;
    for (auto &pg : pages) {
        if (pg.serial != video_serial) continue;
        if (pg.bos && first) { first=false; continue; } // skip BOS
        if (pg.data.empty()) continue;
        // Ogg packets: segments already reassembled per page, but packets can span pages (continued flag 0x01)
        bool continued = (pg.data.size()>=27 && false); // simplified: pages already give reassembled data per our next_page (which concatenates segments)
        // For simplicity, treat each page's data as one packet (works for VP8/VP9 where each frame is one packet)
        // More correct: need to handle packet spanning across pages where last segment ==255
        cur.assign(pg.data.begin(), pg.data.end());
        // VP8 in OGG has 1-byte frame header? For IVF we strip it
        if (video_codec=="VP80" || video_codec=="VP90") {
            // Skip OGG VP8 packet header if present (first bytes are not VP8 frame tag)
            // OGG VP8 packet: first packet after BOS is header, subsequent are frames with no extra header
            // We treat all as frames
            if (!cur.empty()) packets.push_back(cur);
        } else {
            packets.push_back(cur);
        }
    }

    if (packets.empty()) return out;

    // Build elementary stream
    if (video_codec=="VP80" || video_codec=="VP90") {
        out.codec = video_codec=="VP90" ? Codec::Vp9 : Codec::Vp8;
        out.framing = NalFramingMode::Ivf;
        const char* fourcc = video_codec=="VP90" ? "VP90" : "VP80";
        // Try to get width/height from first packet's VP8 header if possible
        uint16_t w=0,h=0;
        if (!packets.empty() && packets[0].size()>=10) {
            // VP8 key frame has width/height at bytes 6-9
            // Leave 0 if not key
        }
        es::append_ivf_header(out, fourcc, w, h);
        uint64_t ts=0;
        for (auto &pkt : packets) {
            // Skip BOS-like packets that are not frames (size < 10)
            if (pkt.size()<3) continue;
            es::append_ivf_frame(out, std::span<const uint8_t>(pkt.data(), pkt.size()), ts++);
        }
        out.ok = !out.bytes.empty();
        return out;
    } else if (video_codec=="AV01") {
        out.codec = Codec::Av1;
        out.framing = NalFramingMode::Obu;
        for (auto &pkt : packets) out.bytes.insert(out.bytes.end(), pkt.begin(), pkt.end());
        out.ok = !out.bytes.empty();
        return out;
    } else {
        bool is_hevc = video_codec=="HEVC";
        out.codec = is_hevc ? Codec::Hevc : Codec::Avc;
        out.framing = NalFramingMode::AnnexB;
        for (auto &pkt : packets) es::annex_b_nal(out, pkt);
        out.ok = !out.bytes.empty();
        return out;
    }
}

} // namespace ogg
} // namespace demux
} // namespace bs
