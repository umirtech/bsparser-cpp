#pragma once

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC Decoding Capability Information (DCI) NAL
 * -----------------------------------------------------------
 * H.266 §7.3.2.1.  Leading fields only: the reserved field, the
 * profile-tier-level count and the extension flag.  The
 * profile_tier_level() blocks themselves are consumed and
 * discarded.
 */
struct Dci {
    /*
     * dci_num_ptls_minus1 u(4).
     */
    std::uint32_t num_ptls_minus1 = 0;

    /*
     * dci_extension_flag u(1).
     */
    bool extension_present = false;

    [[nodiscard]]
    bool valid() const noexcept {
        return num_ptls_minus1 <= 15;
    }
};

}  // namespace vvc
}  // namespace bs
