#pragma once

#include <cstdint>
#include <vector>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC Video Parameter Set (VPS)
 * -----------------------------------------------------------
 * H.266 §7.3.2.3. Only the leading, unambiguous fields of the
 * RBSP are modelled here.
 */
struct VideoParameterSet {

    std::uint8_t vps_id = 0;

    std::uint8_t max_layers_minus1 = 0;

    std::uint8_t max_sublayers_minus1 = 0;

    std::uint8_t num_ptls_minus1 = 0;

    bool all_independent_layers = false;

    std::vector<std::uint8_t> layer_ids;


    [[nodiscard]]
    bool valid() const noexcept
    {
        return vps_id <= 15;
    }
};

} // namespace vvc
} // namespace bs
