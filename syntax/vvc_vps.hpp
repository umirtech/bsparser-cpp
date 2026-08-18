#pragma once

#include <cstdint>
#include <vector>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC Video Parameter Set (VPS)
 * -----------------------------------------------------------
 * H.266 §7.3.2.3.  Only the leading, unambiguous fields of the
 * RBSP are modelled here:
 *
 *     vps_video_parameter_set_id (4) · vps_max_layers_minus1 (6) ·
 *     vps_max_sublayers_minus1 (3) ·
 *     [vps_default_ptl_dpb_hrd_max_tid_flag (1)] ·
 *     [vps_all_independent_layers_flag (1)] ·
 *     vps_layer_id[i] (6) × (max_layers_minus1 + 1)
 *
 * (vps_num_ptls_minus1 and the subsequent OLS / PTL syntax come
 * later in the RBSP and are not modelled here.)
 */
struct VideoParameterSet {
    /*
     * vps_video_parameter_set_id u(4).
     */
    std::uint8_t vps_id = 0;

    /*
     * vps_max_layers_minus1 u(6).
     */
    std::uint8_t max_layers_minus1 = 0;

    /*
     * vps_max_sublayers_minus1 u(3).
     */
    std::uint8_t max_sublayers_minus1 = 0;

    /*
     * vps_default_ptl_dpb_hrd_max_tid_flag u(1), signalled when
     * max_layers_minus1 > 0 && max_sublayers_minus1 > 0 (otherwise
     * inferred 1).
     */
    bool default_ptl_dpb_hrd_max_tid_flag = true;

    /*
     * vps_all_independent_layers_flag u(1), signalled when
     * max_layers_minus1 > 0 (otherwise inferred 1).
     */
    bool all_independent_layers = true;

    /*
     * vps_layer_id[i] u(6), one entry per layer
     * (0..max_layers_minus1).
     */
    std::vector<std::uint8_t> layer_ids;

    [[nodiscard]]
    bool valid() const noexcept {
        return vps_id <= 15;
    }
};

}  // namespace vvc
}  // namespace bs