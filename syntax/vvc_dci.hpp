#pragma once

#include <cstdint>
#include <vector>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC Decoding Capability Information (DCI) NAL
 * -----------------------------------------------------------
 * H.266 §7.3.2.1
 */
struct Dci {
    std::uint32_t num_sps = 0;

    std::vector<std::uint8_t> sps_ids;

    bool bit_rate_present = false;

    bool pic_rate_present = false;

    [[nodiscard]]
    bool valid() const noexcept {
        return num_sps <= 15;
    }
};

}  // namespace vvc
}  // namespace bs
