#pragma once

#include "stream.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace bs {
namespace demux {
namespace ts {

/*
 * -----------------------------------------------------------
 * MPEG-TS demuxer (limited)
 * -----------------------------------------------------------
 *
 * Parses the transport stream (188-byte packets), follows
 * PAT -> PMT to the first video stream, strips the PES headers
 * and returns the concatenated Annex-B elementary stream.
 *
 * Supported video stream types: H.264 (0x1B), HEVC (0x24),
 * VVC (0x33).
 */

namespace detail {

constexpr std::size_t kPacketSize = 188;
constexpr std::size_t kSync = 0x47;
constexpr std::uint16_t kPatPid = 0x0000;

struct PmtInfo {
    bool found = false;
    std::uint16_t video_pid = 0;
    std::uint8_t stream_type = 0;
};

inline std::uint16_t read_pid(std::span<const std::uint8_t> p) {
    return static_cast<std::uint16_t>(((p[1] & 0x1Fu) << 8) | p[2]);
}

inline std::uint8_t adaptation_control(std::span<const std::uint8_t> p) {
    return static_cast<std::uint8_t>((p[3] >> 4) & 0x3u);
}

inline bool payload_start(std::span<const std::uint8_t> p) {
    return (p[1] & 0x40u) != 0;
}

/*
 * Return the payload slice of one TS packet (after the header
 * and adaptation field).
 */
inline std::span<const std::uint8_t> packet_payload(std::span<const std::uint8_t> p) {
    std::size_t off = 4;

    const std::uint8_t ac = adaptation_control(p);

    if (ac == 0x02 || ac == 0x03) {
        const std::uint8_t len = p[4];
        off = 5 + len;
    }

    if (off > p.size()) {
        return {};
    }

    return p.subspan(off);
}

/*
 * Parse a PAT (payload starts with pointer_field) to find the
 * PMT PID.
 */
inline std::uint16_t pat_pmt_pid(std::span<const std::uint8_t> payload) {
    std::size_t p = 0;

    if (!payload.empty()) {
        p = 1 + payload[0]; /* pointer_field */
    }

    /* Skip the section header (8 bytes) up to the program list. */
    p += 8;

    while (p + 4 <= payload.size()) {
        const std::uint16_t program_number =
            static_cast<std::uint16_t>((payload[p] << 8) | payload[p + 1]);

        const std::uint16_t pid =
            static_cast<std::uint16_t>(((payload[p + 2] & 0x1Fu) << 8) | payload[p + 3]);

        if (program_number != 0) {
            return pid;
        }

        p += 4;
    }

    return 0;
}

inline PmtInfo parse_pmt(std::span<const std::uint8_t> payload) {
    PmtInfo info;

    std::size_t p = 0;

    if (!payload.empty()) {
        p = 1 + payload[0];
    }

    if (p + 12 > payload.size()) {
        return info;
    }

    const std::uint16_t section_length =
        static_cast<std::uint16_t>(((payload[p + 1] & 0x0Fu) << 8) | payload[p + 2]);

    const std::size_t end = p + 3 + section_length;

    if (end > payload.size()) {
        return info;
    }

    /*
     * program_number(2) + version(1) + section(1) +
     * last_section(1) + PCR_PID(2) = 7 bytes after section_length,
     * then program_info_length(2), then program_info.
     */
    const std::uint16_t program_info_length =
        static_cast<std::uint16_t>(((payload[p + 10] & 0x0Fu) << 8) | payload[p + 11]);

    p += 12 + program_info_length;

    while (p + 5 <= end) {
        const std::uint8_t stream_type = payload[p];

        const std::uint16_t elementary_pid =
            static_cast<std::uint16_t>(((payload[p + 1] & 0x1Fu) << 8) | payload[p + 2]);

        const bool is_video = stream_type == 0x1Bu || /* H.264 */
                              stream_type == 0x24u || /* HEVC */
                              stream_type == 0x33u || /* VVC */
                              stream_type == 0x06u;   /* private (AV1/VP9 in TS) */

        if (is_video) {
            info.found = true;
            info.video_pid = elementary_pid;
            info.stream_type = stream_type;
            return info;
        }

        const std::uint16_t es_info_length =
            static_cast<std::uint16_t>(((payload[p + 3] & 0x0Fu) << 8) | payload[p + 4]);

        p += 5 + es_info_length;
    }

    return info;
}

}  // namespace detail

[[nodiscard]]
inline ElementaryStream demux_ts(std::span<const std::uint8_t> data) {
    ElementaryStream out;

    const std::size_t packet_count = data.size() / detail::kPacketSize;

    if (packet_count < 2) {
        return out;
    }

    std::uint16_t pmt_pid = 0;
    detail::PmtInfo info;

    std::vector<std::uint8_t> stream;

    bool pes_open = false;
    std::size_t pes_skip = 0;

    for (std::size_t i = 0; i < packet_count; ++i) {
        const auto packet = data.subspan(i * detail::kPacketSize, detail::kPacketSize);

        if (packet[0] != detail::kSync) {
            continue;
        }

        const std::uint16_t pid = detail::read_pid(packet);

        const auto payload = detail::packet_payload(packet);

        if (pid == detail::kPatPid) {
            if (detail::payload_start(packet)) {
                pmt_pid = detail::pat_pmt_pid(payload);
            }

            continue;
        }

        if (pmt_pid != 0 && pid == pmt_pid) {
            if (detail::payload_start(packet)) {
                info = detail::parse_pmt(payload);
            }

            continue;
        }

        if (info.found && pid == info.video_pid) {
            if (payload.empty()) {
                continue;
            }

            if (detail::payload_start(packet)) {
                pes_open = true;
                pes_skip = 0;

                /*
                 * A payload-start packet begins a PES. Locate the
                 * first Annex-B start code after the PES header
                 * (whose exact length is muxer-dependent), and
                 * skip everything before it. If none is present in
                 * this packet the data begins in a continuation.
                 */
                if (payload.size() >= 4 && payload[0] == 0x00 && payload[1] == 0x00 &&
                    payload[2] == 0x01) {
                    std::size_t i = 4;

                    for (; i + 3 <= payload.size(); ++i) {
                        if (payload[i] == 0x00 && payload[i + 1] == 0x00 &&
                            payload[i + 2] == 0x01) {
                            pes_skip = i;
                            break;
                        }
                    }

                    if (i + 3 > payload.size()) {
                        pes_skip = payload.size();
                    }
                }
            }

            if (!pes_open) {
                continue;
            }

            if (pes_skip >= payload.size()) {
                pes_skip -= payload.size();
                continue;
            }

            const auto body = payload.subspan(pes_skip);

            stream.insert(stream.end(), body.begin(), body.end());

            pes_skip = 0;
        }
    }

    if (!info.found || stream.empty()) {
        return out;
    }

    out.bytes = std::move(stream);

    switch (info.stream_type) {
        case 0x1B:
            out.codec = Codec::Avc;
            break;

        case 0x24:
            out.codec = Codec::Hevc;
            break;

        case 0x33:
            out.codec = Codec::Vvc;
            break;

        case 0x06: {
            // Private data — probe payload for AV1 OBU or VP9 IVF
            if (stream.size() >= 4 && stream[0]==0x0A) out.codec = Codec::Av1;
            else out.codec = Codec::Av1;
            if (stream.size()>=2 && stream[0]==0x0A) out.framing = NalFramingMode::Obu;
            else out.framing = NalFramingMode::AnnexB;
            out.bytes = std::move(stream);
            out.ok = true;
            return out;
        }

        default:
            return out;
    }

    out.framing = NalFramingMode::AnnexB;
    out.ok = true;

    return out;
}

}  // namespace ts
}  // namespace demux
}  // namespace bs
