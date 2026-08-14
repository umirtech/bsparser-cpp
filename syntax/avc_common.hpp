#pragma once

#include <cstddef>
#include <cstdint>

namespace bs {
namespace avc {

/*
 * -----------------------------------------------------------
 * H.264 / AVC NAL unit types (Table 7-1)
 * -----------------------------------------------------------
 */

enum class NalUnitType : std::uint8_t {
    SliceNonIdr = 1,
    SliceDataPartitionA = 2,
    SliceDataPartitionB = 3,
    SliceDataPartitionC = 4,
    SliceIdr = 5,
    Sei = 6,
    Sps = 7,
    Pps = 8,
    AccessUnitDelimiter = 9,
    EndOfSequence = 10,
    EndOfStream = 11,
    FillerData = 12,
    SpsExtension = 13,
    PrefixNal = 14,
    SubsetSps = 15,
    AuxCodedPicture = 19,
    SliceSvcExtension = 20,
    SliceMvcExtension = 21,
    SliceAvc3dExtension = 22,
    ReservedStart = 24
};

[[nodiscard]]
constexpr bool is_vcl_nal_unit(NalUnitType type) noexcept {
    const auto value = static_cast<std::uint8_t>(type);

    return value >= 1 && value <= 5;
}

[[nodiscard]]
constexpr bool is_idr_nal_unit(NalUnitType type) noexcept {
    return type == NalUnitType::SliceIdr;
}

/*
 * -----------------------------------------------------------
 * Slice types
 * -----------------------------------------------------------
 *
 * slice_type is Exp-Golomb coded as 0..9.  Values 5..9 are
 * the same five semantic types as 0..4.
 */

enum class SliceType : std::uint8_t { P = 0, B = 1, I = 2, SP = 3, SI = 4 };

[[nodiscard]]
constexpr SliceType decode_slice_type(std::uint32_t raw) noexcept {
    switch (raw % 5) {
        case 0:
            return SliceType::P;
        case 1:
            return SliceType::B;
        case 2:
            return SliceType::I;
        case 3:
            return SliceType::SP;
        default:
            return SliceType::SI;
    }
}

/*
 * -----------------------------------------------------------
 * Chroma formats (chroma_format_idc)
 * -----------------------------------------------------------
 */

enum class ChromaFormat : std::uint8_t { Monochrome = 0, Yuv420 = 1, Yuv422 = 2, Yuv444 = 3 };

/*
 * -----------------------------------------------------------
 * Parameter-set id limits
 * -----------------------------------------------------------
 */

inline constexpr std::size_t kMaxSpsCount = 32;   // seq_parameter_set_id 0..31
inline constexpr std::size_t kMaxPpsCount = 256;  // pic_parameter_set_id 0..255

/*
 * -----------------------------------------------------------
 * High-profile detection
 * -----------------------------------------------------------
 *
 * Profile ids that include the high-profile syntax block
 * (chroma_format_idc etc.) in seq_parameter_set_data().
 */

[[nodiscard]]
constexpr bool is_high_profile(std::uint8_t profile_idc) noexcept {
    switch (profile_idc) {
        case 44:
        case 83:
        case 86:
        case 100:
        case 110:
        case 118:
        case 122:
        case 128:
        case 134:
        case 135:
        case 138:
        case 139:
        case 244:
            return true;
        default:
            return false;
    }
}

}  // namespace avc
}  // namespace bs