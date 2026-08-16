#pragma once

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC Picture Header (PH)
 * -----------------------------------------------------------
 * H.266 §7.3.2.8.  Leading fields through the picture-order-count
 * configuration (ph_pic_order_cnt_lsb + optional msb-cycle), which gate the
 * POC derivation.
 */
struct PictureHeader {
    std::uint8_t pps_id = 0;

    bool gdr_or_irap_pic_flag = false;

    bool non_ref_pic_flag = false;

    /* Present when gdr_or_irap_pic_flag. */
    bool gdr_pic_flag = false;

    bool inter_slice_allowed_flag = false;

    bool intra_slice_allowed_flag = false;

    std::uint32_t poc_lsb = 0;

    std::uint32_t poc_lsb_bits = 0;

    /* Present when gdr_pic_flag. */
    std::uint32_t recovery_poc_cnt = 0;

    bool poc_msb_cycle_present_flag = false;

    std::uint32_t poc_msb_cycle_val = 0;

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
