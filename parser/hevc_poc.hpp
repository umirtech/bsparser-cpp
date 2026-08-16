#pragma once

/*
 * ===========================================================================
 * HEVC picture order count derivation (H.265 §8.3.1)
 * ===========================================================================
 *
 * A small, self-contained state machine that computes the presentation-order
 * POC for each picture from its `slice_pic_order_cnt_lsb`.
 *
 *   PicOrderCnt = PicOrderCntMsb + slice_pic_order_cnt_lsb
 *
 * where PicOrderCntMsb is derived from the previous "TID0 picture":
 *
 *   prevTid0Pic = the previous picture in decoding order with TemporalId == 0
 *                 that is NOT a RASL, RADL or sub-layer non-reference picture
 *
 *   - no prevTid0Pic                 -> PicOrderCntMsb = 0
 *   - prevTid0Pic is not a reference -> prev PicOrderCntLsb/Msb = 0
 *   - otherwise                      -> prev = prevTid0Pic's Lsb/Msb
 *
 * IDR pictures (NoRaslOutputFlag == 1) always have PicOrderCnt == 0.
 *
 * The class owns no parser state and only depends on <cstdint>, so it can be
 * unit-tested in isolation.  The NAL-unit-type classification (reference vs
 * non-reference, RASL/RADL, sub-layer non-reference) follows H.265 table 7-1.
 */

#include <cstdint>

namespace bs {

/*
 * HEVC POC state machine.
 *
 * Call derive() once per slice header (it is idempotent for the several
 * slices of one picture, since they share the same POC LSB / NAL type).
 */
class HevcPocState {
   public:
    /*
     * Reset to the "no previous picture" state (use when re-parsing an
     * independent stream with the same State).
     */
    void reset() noexcept {
        prev_valid_ = false;
        prev_reference_ = false;
        prev_msb_ = 0;
        prev_lsb_ = 0;
    }

    /*
     * Derive the picture order count for the current slice.
     *
     *   nal_unit_type          HEVC NAL type (0..63)
     *   temporal_id            nuh_temporal_id_plus1 - 1
     *   pic_order_cnt_lsb      slice_pic_order_cnt_lsb
     *   max_pic_order_cnt_lsb  2^(log2_max_pic_order_cnt_lsb_minus4 + 4)
     */
    [[nodiscard]]
    std::int32_t derive(
        std::uint8_t nal_unit_type,
        std::uint8_t temporal_id,
        std::uint32_t pic_order_cnt_lsb,
        std::uint32_t max_pic_order_cnt_lsb
    ) noexcept {
        std::int32_t poc_msb = 0;
        std::int32_t poc = 0;

        /*
         * IDR_W_RADL (19) / IDR_N_LP (20): NoRaslOutputFlag == 1, so
         * PicOrderCnt == 0 (PicOrderCntMsb = 0 as well).
         */
        if (nal_unit_type == 19 || nal_unit_type == 20) {
            poc_msb = 0;
            poc = 0;

        } else {
            /*
             * PicOrderCntMsb derivation from the previous TID0 picture.
             */
            if (prev_valid_) {
                const std::int32_t prev_msb = prev_reference_ ? prev_msb_ : 0;
                const std::uint32_t prev_lsb = prev_reference_ ? prev_lsb_ : 0;

                poc_msb =
                    derive_poc_msb(prev_msb, prev_lsb, pic_order_cnt_lsb, max_pic_order_cnt_lsb);
            } else {
                poc_msb = 0;
            }

            poc = poc_msb + static_cast<std::int32_t>(pic_order_cnt_lsb);
        }

        /*
         * Update the prevTid0Pic state for the next picture: TemporalId == 0
         * and not a RASL/RADL/sub-layer non-reference picture.
         */
        const bool is_rasl_or_radl =
            (nal_unit_type == 8 || nal_unit_type == 9 || nal_unit_type == 6 || nal_unit_type == 7);

        if (temporal_id == 0 && !is_rasl_or_radl && !is_sub_layer_non_reference(nal_unit_type)) {
            prev_valid_ = true;
            prev_reference_ = is_reference_vcl(nal_unit_type);
            prev_msb_ = poc_msb;
            prev_lsb_ = pic_order_cnt_lsb;
        }

        return poc;
    }

   private:
    bool prev_valid_ = false;
    bool prev_reference_ = false;
    std::int32_t prev_msb_ = 0;
    std::uint32_t prev_lsb_ = 0;

    /*
     * PicOrderCntMsb derivation (H.265 §8.3.1).
     */
    [[nodiscard]]
    static std::int32_t derive_poc_msb(
        std::int32_t previous_poc_msb,
        std::uint32_t previous_poc_lsb,
        std::uint32_t current_poc_lsb,
        std::uint32_t max_poc_lsb
    ) noexcept {
        if (max_poc_lsb == 0) {
            return previous_poc_msb;
        }

        const std::int64_t max_lsb = static_cast<std::int64_t>(max_poc_lsb);
        const std::int64_t half = max_lsb / 2;
        const std::int64_t prev_lsb = static_cast<std::int64_t>(previous_poc_lsb);
        const std::int64_t curr_lsb = static_cast<std::int64_t>(current_poc_lsb);

        std::int64_t poc_msb = static_cast<std::int64_t>(previous_poc_msb);

        if ((curr_lsb < prev_lsb) && ((prev_lsb - curr_lsb) >= half)) {
            poc_msb += max_lsb;
        } else if ((curr_lsb > prev_lsb) && ((curr_lsb - prev_lsb) > half)) {
            poc_msb -= max_lsb;
        }

        if (poc_msb > static_cast<std::int64_t>(INT32_MAX)) {
            return INT32_MAX;
        }
        if (poc_msb < static_cast<std::int64_t>(INT32_MIN)) {
            return INT32_MIN;
        }

        return static_cast<std::int32_t>(poc_msb);
    }

    /*
     * Reference-picture classification (H.265 table 7-1).
     *
     * BLA_W_LP (16) is a reference picture even though it is even; the IRAP
     * range 16..23 is always reference.
     */
    [[nodiscard]]
    static bool is_reference_vcl(std::uint8_t nal_unit_type) noexcept {
        const unsigned v = static_cast<unsigned>(nal_unit_type);

        if (v < 16) {
            /* TRAIL/TSA/STSA/RADL/RASL + reserved VCL: odd = reference. */
            return (v & 1U) != 0;
        }
        if (v <= 23) {
            /* BLA_W_LP/BLA_W_RADL/BLA_N_LP/IDR_W_RADL/IDR_N_LP/CRA/RSV_IRAP. */
            return true;
        }

        /* Reserved VCL 24..31: odd = reference. */
        return (v & 1U) != 0;
    }

    /*
     * "Sub-layer non-reference picture" set (H.265 table 7-1): the *_N
     * VCL types.  These are excluded from the prevTid0Pic selection.
     */
    [[nodiscard]]
    static bool is_sub_layer_non_reference(std::uint8_t nal_unit_type) noexcept {
        switch (nal_unit_type) {
            case 0:  /* TRAIL_N */
            case 2:  /* TSA_N */
            case 4:  /* STSA_N */
            case 6:  /* RADL_N */
            case 8:  /* RASL_N */
            case 10: /* RSV_VCL_N10 */
            case 12: /* RSV_VCL_N12 */
            case 14: /* RSV_VCL_N14 */
            case 24: /* RSV_VCL_N24 */
            case 26: /* RSV_VCL_N26 */
            case 28: /* RSV_VCL_N28 */
            case 30: /* RSV_VCL_N30 */
                return true;
            default:
                return false;
        }
    }
};

}  // namespace bs
