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

    if (data.size() >= 12 && std::memcmp(data.data(), "RIFF", 4) == 0 &&
        std::memcmp(data.data() + 8, "AVI ", 4) == 0) {
        return Container::Avi;
    }

    if (data.size() >= 4 && data[0] == 0x1A && data[1] == 0x45 && data[2] == 0xDF &&
        data[3] == 0xA3) {
        return Container::Mkv;
    }

    const std::size_t scan = std::min<std::size_t>(data.size(), 64);

    for (std::size_t i = 0; i + 8 <= scan; ++i) {
        if (std::memcmp(data.data() + i + 4, "ftyp", 4) == 0 ||
            std::memcmp(data.data() + i + 4, "moov", 4) == 0 ||
            std::memcmp(data.data() + i + 4, "moof", 4) == 0) {
            return Container::Mp4;
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
