#pragma once

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC Picture Parameter Set (PPS)
 * -----------------------------------------------------------
 * H.266 §7.3.2.5. Leading fields only.
 */
struct PictureParameterSet {

    std::uint8_t pps_id = 0;

    std::uint8_t sps_id = 0;

    bool mixed_nalu_types_in_pic = false;

    std::uint32_t pic_width_in_luma_samples = 0;

    std::uint32_t pic_height_in_luma_samples = 0;


    [[nodiscard]]
    bool valid() const noexcept
    {
        return pps_id <= 63;
    }
};

} // namespace vvc
} // namespace bs
