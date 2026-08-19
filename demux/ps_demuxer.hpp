#pragma once

#include "stream.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace bs {
namespace demux {
namespace ps {

/*
 * -----------------------------------------------------------
 * MPEG-PS (Program Stream) demuxer — for .mpg/.mpeg/.vob
 * -----------------------------------------------------------
 * Scans for pack headers (0x000001BA) and PES packets
 * (0x000001E0 video). Strips PES headers and returns
 * Annex-B elementary stream.
 */

[[nodiscard]]
inline ElementaryStream demux_ps(std::span<const uint8_t> data) {
    ElementaryStream out;
    if (data.size() < 14)
        return out;

    size_t pos = 0;
    std::vector<uint8_t> stream;
    Codec codec = Codec::Avc;
    bool found = false;

    while (pos + 4 <= data.size()) {
        if (data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x01) {
            uint8_t sid = data[pos + 3];
            if (sid == 0xBA) {  // pack header
                // pack header length: 14 for MPEG2, 12 for MPEG1
                if (pos + 14 > data.size())
                    break;
                uint8_t stuffing = data[pos + 13] & 0x07;
                pos += 14 + stuffing;
                continue;
            } else if (sid == 0xBB) {  // system header
                if (pos + 6 > data.size())
                    break;
                uint16_t len = (uint16_t(data[pos + 4]) << 8) | data[pos + 5];
                pos += 6 + len;
                continue;
            } else if (sid >= 0xE0 && sid <= 0xEF) {  // video PES
                if (pos + 6 > data.size())
                    break;
                uint16_t pes_len = (uint16_t(data[pos + 4]) << 8) | data[pos + 5];
                size_t pes_start = pos + 6;
                size_t pes_end = (pes_len == 0) ? data.size() : pes_start + pes_len;
                if (pes_end > data.size())
                    pes_end = data.size();
                // PES header: parse to find payload offset
                size_t payload_off = pes_start;
                if (payload_off + 3 <= pes_end) {
                    uint8_t c1 = data[payload_off];
                    uint8_t c2 = data[payload_off + 1];
                    uint8_t c3 = data[payload_off + 2];
                    // Check PES header marker 10xxxxxx
                    if ((c1 & 0xC0) == 0x80) {
                        uint8_t pes_header_len = data[payload_off + 2];
                        payload_off += 3 + pes_header_len;
                    } else if ((c1 & 0xC0) == 0x40) {
                        // MPEG1 PES
                        // skip STD_buffer_scale etc - simplified
                        payload_off += 3;
                        // For MPEG1, payload starts after PTS/DTS
                        // Just scan for next start code
                    } else {
                        // Unknown, try to find next start code
                        payload_off += 3;
                    }
                    (void)c2;
                    (void)c3;
                }
                if (payload_off < pes_end) {
                    auto payload = data.subspan(payload_off, pes_end - payload_off);
                    // Heuristic: detect codec from first PES payload
                    if (!found && payload.size() >= 5) {
                        if (payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
                            uint8_t nal_type = payload[4] & 0x1F;
                            if (nal_type == 7)
                                codec = Codec::Avc;
                            else if ((payload[4] >> 1) == 32)
                                codec = Codec::Hevc;  // 0x40
                        }
                    }
                    stream.insert(stream.end(), payload.begin(), payload.end());
                    found = true;
                }
                pos = pes_end;
                continue;
            } else if (sid == 0xBC || sid == 0xBD || (sid >= 0xBE && sid <= 0xBF) || sid == 0xB9 ||
                       sid == 0xB8 || sid == 0xB7) {
                if (pos + 6 > data.size())
                    break;
                uint16_t len = (uint16_t(data[pos + 4]) << 8) | data[pos + 5];
                pos += 6 + len;
                continue;
            }
        }
        pos++;
    }

    if (!found || stream.empty())
        return out;
    out.bytes = std::move(stream);
    out.codec = codec;
    out.framing = NalFramingMode::AnnexB;
    out.ok = true;
    return out;
}

}  // namespace ps
}  // namespace demux
}  // namespace bs
