#pragma once

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC Sequence Parameter Set (SPS)
 * -----------------------------------------------------------
 * H.266 §7.3.2.4. Only the leading identification fields are
 * modelled here (the full RBSP is far larger).
 */
struct SequenceParameterSet {
    std::uint8_t sps_id = 0;

    std::uint8_t vps_id = 0;

    std::uint8_t max_sublayers_minus1 = 0;

    std::uint8_t chroma_format_idc = 0;

    std::uint8_t log2_ctu_size_minus5 = 0;

    bool ptl_dpb_hrd_params_present = false;

    [[nodiscard]]
    bool valid() const noexcept {
        return sps_id <= 15 && chroma_format_idc <= 3;
    }
};

}  // namespace vvc
}  // namespace bs
