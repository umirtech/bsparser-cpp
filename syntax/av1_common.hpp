#pragma once

#include <cstdint>

namespace bs {
namespace av1 {

/*
 * -----------------------------------------------------------
 * AV1 OBU types (AV1 §5.1)
 * -----------------------------------------------------------
 */
enum class ObuType : std::uint8_t {
    Reserved0 = 0,
    SequenceHeader = 1,
    TemporalDelimiter = 2,
    FrameHeader = 3,
    TileGroup = 4,
    Metadata = 5,
    Frame = 6,
    RedundantFrameHeader = 7,
    TileList = 8,
    Reserved9 = 9,
    Reserved10 = 10,
    Reserved11 = 11,
    Reserved12 = 12,
    Reserved13 = 13,
    Reserved14 = 14,
    Padding = 15
};


/*
 * -----------------------------------------------------------
 * AV1 operating mode / frame types
 * -----------------------------------------------------------
 */
enum class FrameType : std::uint8_t {
    KeyFrame = 0,
    InterFrame = 1,
    IntraOnly = 2,
    Switch = 3
};

} // namespace av1
} // namespace bs
