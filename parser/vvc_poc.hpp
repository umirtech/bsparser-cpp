#pragma once

/*
 * ===========================================================================
 * VVC picture order count derivation (H.266 §8.3.1)
 * ===========================================================================
 *
 * A small, self-contained state machine that computes the presentation-order
 * PicOrderCntVal for each picture from the picture header.
 *
 *   PicOrderCntVal = PicOrderCntMsb + ph_pic_order_cnt_lsb
 *
 * where PicOrderCntMsb comes from ph_poc_msb_cycle_val (when present), or is
 * 0 for a CLVSS picture, or is derived from the previous "TID0 picture":
 *
 *   prevTid0Pic = the previous picture in decoding order with the same
 *                 nuh_layer_id, TemporalId == 0, ph_non_ref_pic_flag == 0,
 *                 that is NOT a RASL or RADL picture
 *
 * CLVSS pictures are IRAP pictures (NAL types 7..11) and GDR pictures
 * (type 10) with ph_recovery_poc_cnt == 0.
 */

#include "../syntax/vvc_ph.hpp"

#include <cstdint>

namespace bs {
namespace vvc {

class PocState {
   public:
    void reset() noexcept {
        prev_valid_ = false;
        prev_msb_ = 0;
        prev_lsb_ = 0;
    }

    /*
     * Derive PicOrderCntVal for the picture whose picture header is `ph`.
     *
     *   nal_unit_type   VVC NAL type of a VCL slice of the picture
     *   temporal_id     nuh_temporal_id_plus1 - 1
     *   ph              the picture header (POC fields populated)
     */
    [[nodiscard]]
    std::int32_t derive(
        std::uint8_t nal_unit_type, std::uint8_t temporal_id, const PictureHeader& ph
    ) noexcept {
        const std::uint32_t max_poc_lsb =
            (ph.poc_lsb_bits != 0) ? (std::uint32_t{1} << ph.poc_lsb_bits) : 0;

        std::int32_t poc_msb = 0;

        if (ph.poc_msb_cycle_present_flag) {
            poc_msb = static_cast<std::int32_t>(ph.poc_msb_cycle_val) *
                      static_cast<std::int32_t>(max_poc_lsb);
        } else if (is_clvss_pic(nal_unit_type, ph)) {
            poc_msb = 0;
        } else if (prev_valid_ && max_poc_lsb != 0) {
            const std::int64_t half = static_cast<std::int64_t>(max_poc_lsb) / 2;
            const std::int64_t prev_lsb = static_cast<std::int64_t>(prev_lsb_);
            const std::int64_t curr_lsb = static_cast<std::int64_t>(ph.poc_lsb);
            const std::int64_t max_lsb = static_cast<std::int64_t>(max_poc_lsb);

            std::int64_t msb = static_cast<std::int64_t>(prev_msb_);

            if ((curr_lsb < prev_lsb) && ((prev_lsb - curr_lsb) >= half)) {
                msb += max_lsb;
            } else if ((curr_lsb > prev_lsb) && ((curr_lsb - prev_lsb) > half)) {
                msb -= max_lsb;
            }

            if (msb > static_cast<std::int64_t>(INT32_MAX)) {
                poc_msb = INT32_MAX;
            } else if (msb < static_cast<std::int64_t>(INT32_MIN)) {
                poc_msb = INT32_MIN;
            } else {
                poc_msb = static_cast<std::int32_t>(msb);
            }
        }

        const std::int32_t poc = poc_msb + static_cast<std::int32_t>(ph.poc_lsb);

        /*
         * Update prevTid0Pic: TemporalId == 0, non-reference pictures
         * excluded (ph_non_ref_pic_flag == 0), RASL/RADL excluded.
         */
        const bool is_rasl_radl = (nal_unit_type == 2 || nal_unit_type == 3);

        if (temporal_id == 0 && !ph.non_ref_pic_flag && !is_rasl_radl) {
            prev_valid_ = true;
            prev_msb_ = poc_msb;
            prev_lsb_ = ph.poc_lsb;
        }

        return poc;
    }

   private:
    bool prev_valid_ = false;
    std::int32_t prev_msb_ = 0;
    std::uint32_t prev_lsb_ = 0;

    /*
     * A CLVSS picture: an IRAP (7, 8, 9, 11) or a GDR picture (10) with
     * ph_recovery_poc_cnt == 0.
     */
    [[nodiscard]]
    static bool is_clvss_pic(std::uint8_t nal_unit_type, const PictureHeader& ph) noexcept {
        if ((nal_unit_type >= 7 && nal_unit_type <= 9) || nal_unit_type == 11) {
            return true;
        }
        return nal_unit_type == 10 && ph.recovery_poc_cnt == 0;
    }
};

}  // namespace vvc
}  // namespace bs
