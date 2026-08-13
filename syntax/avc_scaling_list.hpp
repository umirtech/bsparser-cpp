#pragma once

#include <array>
#include <cstdint>

namespace bs {
namespace avc {

/*
 * AVC scaling matrices (Annex A.2.5.1).
 *
 * seq_scaling_list_present_flag[0..5]   -> 4x4 matrices
 * seq_scaling_list_present_flag[6..7]   -> 8x8 matrices
 */
struct ScalingList {

    std::array<bool, 6> present_4x4{};
    std::array<std::array<std::uint8_t, 16>, 6> list_4x4{};

    std::array<bool, 2> present_8x8{};
    std::array<std::array<std::uint8_t, 64>, 2> list_8x8{};
};

} // namespace avc
} // namespace bs