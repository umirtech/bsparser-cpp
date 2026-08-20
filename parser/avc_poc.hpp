// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/*
 * ===========================================================================
 * AVC picture order count derivation (H.264 §8.2.1)
 * ===========================================================================
 *
 * A small, self-contained state machine that computes the presentation-order
 * POC for each picture from the parsed slice header + SPS.
 *
 *   pic_order_cnt_type == 0  -> POC from PicOrderCntMsb + pic_order_cnt_lsb
 *   pic_order_cnt_type == 1  -> POC from FrameNumOffset + the POC cycle
 *   pic_order_cnt_type == 2  -> POC = 2 * (FrameNumOffset + frame_num)
 *
 * State (PrevFrameNum / prevFrameNumOffset / PrevPicOrderCntMsb /
 * PrevPicOrderCntLsb) follows §8.2.1:
 *
 *   - IDR resets everything to 0.
 *   - otherwise the state is updated only by reference pictures
 *     (nal_ref_idc != 0).
 *
 * The class owns no parser state and only depends on the syntax structs, so
 * it can be unit-tested in isolation.
 */

#include "../syntax/avc_slice_header.hpp"
#include "../syntax/avc_sps.hpp"

#include <cstdint>

namespace bs {
namespace avc {

class PocState {
   public:
    /*
     * Reset to the initial state (use when re-parsing an independent stream).
     */
    void reset() noexcept {
        prev_frame_num_ = 0;
        prev_frame_num_offset_ = 0;
        prev_poc_msb_ = 0;
        prev_poc_lsb_ = 0;
    }

    /*
     * Derive the picture order count for the current picture.
     *
     *   sh            parsed slice header (first slice of the picture)
     *   sps           the SPS referenced by the slice
     *   idr           true when the NAL is an IDR slice
     *   nal_ref_idc   the NAL's nal_ref_idc (0 => non-reference picture)
     */
    [[nodiscard]]
    std::int32_t derive(
        const SliceHeader& sh, const SequenceParameterSet& sps, bool idr, std::uint8_t nal_ref_idc
    ) noexcept {
        const std::uint32_t max_frame_num = std::uint32_t{1} << sps.log2_max_frame_num();

        std::int32_t frame_num_offset = prev_frame_num_offset_;
        if (prev_frame_num_ > sh.frame_num) {
            frame_num_offset += static_cast<std::int32_t>(max_frame_num);
        }

        std::int32_t poc_msb = 0;
        std::int32_t top = 0;
        std::int32_t bottom = 0;
        std::int32_t pic_order_cnt = 0;

        if (idr) {
            pic_order_cnt = 0;

        } else if (sps.pic_order_cnt_type == 0) {
            const std::uint32_t max_poc_lsb = std::uint32_t{1} << sps.log2_max_pic_order_cnt_lsb();
            const std::uint32_t poc_lsb = sh.pic_order_cnt_lsb;

            /*
             * PicOrderCntMsb derivation (§8.2.1).
             */
            if (max_poc_lsb < 16 &&
                (prev_poc_msb_ - 16) >= static_cast<std::int32_t>(prev_poc_lsb_)) {
                poc_msb = prev_poc_msb_ - static_cast<std::int32_t>(max_poc_lsb);
            } else if (poc_lsb < prev_poc_lsb_ && (prev_poc_lsb_ - poc_lsb) >= (max_poc_lsb / 2)) {
                poc_msb = prev_poc_msb_ + static_cast<std::int32_t>(max_poc_lsb);
            } else if (poc_lsb > prev_poc_lsb_ && (poc_lsb - prev_poc_lsb_) > (max_poc_lsb / 2)) {
                poc_msb = prev_poc_msb_ - static_cast<std::int32_t>(max_poc_lsb);
            } else {
                poc_msb = prev_poc_msb_;
            }

            top = poc_msb + static_cast<std::int32_t>(poc_lsb);
            if (!sh.field_pic_flag) {
                bottom = top + sh.delta_pic_order_cnt_bottom;
            } else {
                bottom = top;
            }

        } else if (sps.pic_order_cnt_type == 1) {
            const std::uint32_t cycle = sps.num_ref_frames_in_pic_order_cnt_cycle;

            std::int64_t abs_frame_num =
                (cycle != 0) ? static_cast<std::int64_t>(frame_num_offset) + sh.frame_num : 0;

            if (nal_ref_idc == 0 && abs_frame_num > 0) {
                --abs_frame_num;
            }

            std::int64_t expected_delta = 0;
            for (std::uint32_t i = 0; i < cycle; ++i) {
                expected_delta += sps.offset_for_ref_frame[i];
            }

            std::int64_t expected_poc = 0;
            if (abs_frame_num > 0 && cycle != 0) {
                const std::int64_t cycle_cnt = (abs_frame_num - 1) / cycle;
                const std::int64_t frame_in_cycle = (abs_frame_num - 1) % cycle;

                expected_poc = cycle_cnt * expected_delta;
                for (std::int64_t i = 0; i <= frame_in_cycle; ++i) {
                    expected_poc += sps.offset_for_ref_frame[static_cast<std::size_t>(i)];
                }
            }

            if (nal_ref_idc == 0) {
                expected_poc += sps.offset_for_non_ref_pic;
            }

            top = static_cast<std::int32_t>(expected_poc + sh.delta_pic_order_cnt[0]);
            bottom = top + sps.offset_for_top_to_bottom_field;
            if (!sh.field_pic_flag) {
                bottom += sh.delta_pic_order_cnt[1];
            }

        } else {
            /* pic_order_cnt_type == 2 */
            std::int32_t poc = 2 * (frame_num_offset + static_cast<std::int32_t>(sh.frame_num));
            if (nal_ref_idc == 0) {
                --poc;
            }
            top = poc;
            bottom = poc;
        }

        if (!sh.field_pic_flag) {
            pic_order_cnt = (top < bottom) ? top : bottom;
        } else if (sh.bottom_field_flag) {
            pic_order_cnt = bottom;
        } else {
            pic_order_cnt = top;
        }

        /*
         * State update (§8.2.1): IDR resets everything; otherwise only
         * reference pictures update the previous state.
         */
        if (idr) {
            prev_frame_num_ = 0;
            prev_frame_num_offset_ = 0;
            prev_poc_msb_ = 0;
            prev_poc_lsb_ = 0;
        } else if (nal_ref_idc != 0) {
            if (sps.pic_order_cnt_type == 0) {
                prev_poc_msb_ = poc_msb;
                prev_poc_lsb_ = sh.pic_order_cnt_lsb;
            } else {
                prev_frame_num_ = sh.frame_num;
                prev_frame_num_offset_ = frame_num_offset;
            }
        }

        return pic_order_cnt;
    }

   private:
    std::uint32_t prev_frame_num_ = 0;
    std::int32_t prev_frame_num_offset_ = 0;
    std::int32_t prev_poc_msb_ = 0;
    std::uint32_t prev_poc_lsb_ = 0;
};

}  // namespace avc
}  // namespace bs
