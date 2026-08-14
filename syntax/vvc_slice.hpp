#pragma once

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC slice type
 * -----------------------------------------------------------
 */
enum class SliceType : std::uint8_t {
    B = 0,
    P = 1,
    I = 2
};


/*
 * -----------------------------------------------------------
 * VVC slice segment header
 * -----------------------------------------------------------
 * H.266 §7.3.4.1. Leading fields only.
 */
struct SliceHeader {

    std::uint32_t pps_id = 0;

    bool first_slice_segment_in_pic = false;

    bool independent_slice_segment = true;

    std::uint32_t slice_segment_address = 0;

    SliceType slice_type = SliceType::I;

    std::uint32_t poc_lsb = 0;


    [[nodiscard]]
    bool valid() const noexcept
    {
        return pps_id <= 63;
    }
};

} // namespace vvc
} // namespace bs
