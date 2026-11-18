// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "stream.hpp"

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

    if (data.size() >= 12 && std::memcmp(data.data(), "RIFF", 4) == 0) {
        if (std::memcmp(data.data() + 8, "AVI ", 4) == 0) {
            return Container::Avi;
        }
        if (std::memcmp(data.data() + 8, "WAVE", 4) == 0) {
            return Container::Wav;
        }
    }

    if (data.size() >= 4 && data[0] == 0x1A && data[1] == 0x45 && data[2] == 0xDF &&
        data[3] == 0xA3) {
        return Container::Mkv;
    }

    const std::size_t scan = std::min<std::size_t>(data.size(), 64);

    /* QuickTime files often start with 'skip'/'free' and place 'moov' at the
     * very end.  Scan both the head and the tail for ISO-BMFF box types. */
    for (std::size_t i = 0; i + 8 <= scan; ++i) {
        if (std::memcmp(data.data() + i + 4, "ftyp", 4) == 0 ||
            std::memcmp(data.data() + i + 4, "moov", 4) == 0 ||
            std::memcmp(data.data() + i + 4, "moof", 4) == 0) {
            return Container::Mp4;
        }
    }
    if (data.size() > 64) {
        /* moov can be anywhere near the end of a QuickTime file.  Walk real
         * top-level boxes from the start to find it, with a cap on work. */
        std::size_t p = 0;
        for (unsigned steps = 0; steps < 64 && p + 8 <= data.size(); ++steps) {
            std::uint64_t size = 0;
            for (unsigned k = 0; k < 4 && p + k < data.size(); ++k) {
                size = (size << 8) | data[p + k];
            }
            if (size < 8 || size > data.size() - p) {
                break;
            }
            if (std::memcmp(data.data() + p + 4, "moov", 4) == 0) {
                return Container::Mp4;
            }
            p += static_cast<std::size_t>(size);
        }
        // Fallback: scan the last 1 KiB for a moov fourcc.
        const std::size_t tail = data.size() - std::min<std::size_t>(data.size(), 1024);
        for (std::size_t i = tail; i + 8 <= data.size(); ++i) {
            if (std::memcmp(data.data() + i + 4, "moov", 4) == 0) {
                return Container::Mp4;
            }
        }
    }

    if (data.size() >= 4 && data[0] == 0x4F && data[1] == 0x67 && data[2] == 0x67 &&
        data[3] == 0x53) {
        return Container::Ogg;
    }

    if (data.size() >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01 &&
        data[3] == 0xBA) {
        return Container::Ps;
    }

    for (std::size_t i = 0; i + 8 <= scan; ++i) {
        if (std::memcmp(data.data() + i + 4, "moof", 4) == 0) {
            return Container::Mp4;
        }
        if (std::memcmp(data.data() + i + 4, "sidx", 4) == 0) {
            return Container::Mp4;
        }
    }

    if (data.size() > 376 && data[0] == 0x47 && data[188] == 0x47 && data[376] == 0x47) {
        return Container::MpegTs;
    }

    return Container::Unknown;
}

}  // namespace demux
}  // namespace bs
