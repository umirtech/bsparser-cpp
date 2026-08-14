#pragma once

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC Picture Header (PH)
 * -----------------------------------------------------------
 * H.266 §7.3.2.6. Leading fields only.
 */
struct PictureHeader {

    std::uint8_t pps_id = 0;

    std::uint32_t poc_lsb = 0;

    std::uint32_t poc_lsb_bits = 0;


    [[nodiscard]]
    bool valid() const noexcept
    {
        return pps_id <= 63;
    }
};

} // namespace vvc
} // namespace bs
