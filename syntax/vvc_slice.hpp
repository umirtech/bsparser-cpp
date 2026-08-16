#pragma once

#include "vvc_ph.hpp"

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC slice type
 * -----------------------------------------------------------
 */
enum class SliceType : std::uint8_t { B = 0, P = 1, I = 2 };

/*
 * -----------------------------------------------------------
 * VVC slice segment header
 * -----------------------------------------------------------
 * H.266 §7.3.4.1.  Leading fields, including the picture header
 * (which may be embedded in the slice header).
 */
struct SliceHeader {
    std::uint8_t pps_id = 0;

    bool first_slice_segment_in_pic = false;

    bool independent_slice_segment = true;

    std::uint32_t slice_segment_address = 0;

    SliceType slice_type = SliceType::I;

    /*
     * sh_picture_header_in_slice_header_flag: the picture header is embedded
     * in this slice header (rather than signalled in a PH NAL).
     */
    bool picture_header_in_slice_header_flag = false;

    /*
     * The picture header of the picture this slice belongs to (embedded or
     * from the stored PH NAL), with the POC fields populated.
     */
    PictureHeader ph{};

    /*
     * Presentation-order POC (H.266 §8.3.1), filled by the unified dispatch
     * layer's vvc::PocState.
     */
    std::int32_t derived_poc = 0;

    [[nodiscard]]
    bool valid() const noexcept {
        return pps_id <= 63;
    }
};

}  // namespace vvc
}  // namespace bs
