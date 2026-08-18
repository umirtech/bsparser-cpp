#pragma once

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC Operating Point Information (OPI) NAL
 * -----------------------------------------------------------
 * H.266 §7.3.2.2.
 */
struct Opi {
    /*
     * opi_ols_info_present_flag u(1).
     */
    bool ols_info_present = false;

    /*
     * opi_htid_info_present_flag u(1).
     */
    bool htid_info_present = false;

    /*
     * opi_ols_idx ue(v), present when ols_info_present.
     */
    std::uint32_t ols_idx = 0;

    /*
     * opi_htid_plus1 u(3), present when htid_info_present.
     */
    std::uint8_t htid_plus1 = 0;

    /*
     * opi_extension_flag u(1).
     */
    bool extension_present = false;

    [[nodiscard]]
    bool valid() const noexcept {
        return true;
    }
};

}  // namespace vvc
}  // namespace bs
