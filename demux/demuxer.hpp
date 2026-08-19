#pragma once

#include "stream.hpp"
#include "avi_demuxer.hpp"
#include "flv_demuxer.hpp"
#include "mkv_demuxer.hpp"
#include "mp4_demuxer.hpp"
#include "ogg_demuxer.hpp"
#include "ps_demuxer.hpp"
#include "ts_demuxer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>

namespace bs {
namespace demux {

/*
 * -----------------------------------------------------------
 * Container detection
 * -----------------------------------------------------------
 */
[[nodiscard]]
inline Container sniff(std::span<const std::uint8_t> data) {
    if (data.size() >= 3 && data[0] == 'F' && data[1] == 'L' && data[2] == 'V') {
        return Container::Flv;
    }

    if (data.size() >= 4 && std::memcmp(data.data(), "DKIF", 4) == 0) {
        return Container::Ivf;
    }

    if (data.size() >= 12 && std::memcmp(data.data(), "RIFF", 4) == 0 &&
        std::memcmp(data.data() + 8, "AVI ", 4) == 0) {
        return Container::Avi;
    }

    if (data.size() >= 4 && data[0] == 0x1A && data[1] == 0x45 && data[2] == 0xDF &&
        data[3] == 0xA3) {
        return Container::Mkv;
    }

    /*
     * MP4 / ISO-BMFF: look for an ftyp/moov/moof box in the
     * leading bytes.
     */
    const std::size_t scan = std::min<std::size_t>(data.size(), 64);

    for (std::size_t i = 0; i + 8 <= scan; ++i) {
        if (std::memcmp(data.data() + i + 4, "ftyp", 4) == 0 ||
            std::memcmp(data.data() + i + 4, "moov", 4) == 0 ||
            std::memcmp(data.data() + i + 4, "moof", 4) == 0) {
            return Container::Mp4;
        }
    }

    if (data.size() >= 4 && data[0]==0x4F && data[1]==0x67 && data[2]==0x67 && data[3]==0x53) {
        return Container::Ogg;
    }

    if (data.size() >= 4 && data[0]==0x00 && data[1]==0x00 && data[2]==0x01 && data[3]==0xBA) {
        return Container::Ps;
    }

    // fMP4 (fragmented) also has ftyp/moov — already caught above; also detect moof
    for (size_t i=0;i+8<=scan;++i) {
        if (std::memcmp(data.data()+i+4, "moof",4)==0) return Container::Mp4;
        if (std::memcmp(data.data()+i+4, "sidx",4)==0) return Container::Mp4;
    }

    /*
     * MPEG-TS: repeated sync bytes at 188-byte intervals.
     */
    if (data.size() > 376 && data[0] == 0x47 && data[188] == 0x47 && data[376] == 0x47) {
        return Container::MpegTs;
    }

    return Container::Unknown;
}

/*
 * -----------------------------------------------------------
 * Demux a specific container.
 * -----------------------------------------------------------
 */
[[nodiscard]]
inline ElementaryStream demux(Container container, std::span<const std::uint8_t> data) {
    ElementaryStream out;

    try {
        switch (container) {
            case Container::Mp4:
                return mp4::demux_mp4(data);

            case Container::MpegTs:
                return ts::demux_ts(data);

            case Container::Flv:
                return flv::demux_flv(data);

            case Container::Avi:
                return avi::demux_avi(data);

            case Container::Ivf: {
                if (data.size() < 32) {
                    return out;
                }

                const bool vp9 =
                    data[8] == 'V' && data[9] == 'P' && data[10] == '9' && data[11] == '0';

                out.codec = vp9 ? Codec::Vp9 : Codec::Vp8;
                out.framing = NalFramingMode::Ivf;
                out.bytes.assign(data.begin(), data.end());
                out.width = static_cast<std::uint16_t>(data[12] | (data[13] << 8));
                out.height = static_cast<std::uint16_t>(data[14] | (data[15] << 8));
                out.ok = true;
                return out;
            }

            case Container::Mkv:
                return mkv::demux_mkv(data);

            case Container::Ogg:
                return ogg::demux_ogg(data);

            case Container::Ps:
                return ps::demux_ps(data);

            default:
                return out;
        }
    } catch (...) {
        /* malformed container: fail gracefully */
        return out;
    }
}

/*
 * -----------------------------------------------------------
 * Auto-detect the container and demux.
 * -----------------------------------------------------------
 */
[[nodiscard]]
inline ElementaryStream demux(std::span<const std::uint8_t> data) {
    return demux(sniff(data), data);
}

}  // namespace demux
}  // namespace bs
