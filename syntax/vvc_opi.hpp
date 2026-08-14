#pragma once

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC Operating Point Information (OPI) NAL
 * -----------------------------------------------------------
 * H.266 §7.3.2.2
 */
struct Opi {
    bool ols_info_present = false;

    bool ptl_present = false;

    [[nodiscard]]
    bool valid() const noexcept {
        return true;
    }
};

}  // namespace vvc
}  // namespace bs
