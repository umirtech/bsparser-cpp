#pragma once

#include "bsparser.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bs {
namespace demux {

/*
 * -----------------------------------------------------------
 * Container formats
 * -----------------------------------------------------------
 */
enum class Container : std::uint8_t { Unknown, Mp4, MpegTs, Avi, Flv, Ivf, Mkv, Ogg, Ps };

/*
 * -----------------------------------------------------------
 * Extracted elementary stream
 * -----------------------------------------------------------
 *
 * A demuxer produces a self-contained, framed elementary stream
 * plus the codec needed to parse it with bs::parse. The bytes
 * are owned by this struct.
 */
struct ElementaryStream {
    Codec codec = Codec::Hevc;
    NalFramingMode framing = NalFramingMode::AnnexB;

    std::vector<std::uint8_t> bytes{};

    std::string codec_name; /* sample-entry fourcc, e.g. "avc1" */
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool ok = false;
};

}  // namespace demux
}  // namespace bs
