#pragma once

#include "hevc_short_term_ref_pic_set.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace bs {

/*
 * ===========================================================
 * Reference picture manager
 * ===========================================================
 *
 * This layer contains decoder-side reference-picture state.
 *
 * It intentionally does NOT own SPS/PPS/VPS objects.
 *
 * The important distinction is:
 *
 *     syntax structures
 *         =
 *     what the bitstream signaled
 *
 *     reference-picture state
 *         =
 *     decoder state derived from the syntax
 *
 *
 * The manager therefore sits between:
 *
 *     SliceHeader
 *          |
 *          v
 *     RPS syntax
 *          |
 *          v
 *     reference-picture state
 */


/*
 * -----------------------------------------------------------
 * Short-term reference picture
 * -----------------------------------------------------------
 */

struct ShortTermReferencePicture {

    /*
     * Picture order count of the reference picture.
     */
    std::int32_t poc = 0;

    /*
     * Delta POC relative to the current picture.
     */
    std::int32_t delta_poc = 0;

    /*
     * Whether this picture is currently used for reference.
     */
    bool used_by_curr_pic = false;

    /*
     * Long-term flag.
     *
     * false = short-term
     * true  = long-term
     */
    bool long_term = false;

    /*
     * Long-term POC LSB.
     *
     * Only meaningful for long-term references.
     */
    std::uint32_t poc_lsb = 0;

    /*
     * Used by the current picture as a reference.
     */
    [[nodiscard]]
    constexpr bool is_reference() const noexcept
    {
        return used_by_curr_pic;
    }
};


/*
 * -----------------------------------------------------------
 * Reference picture list
 * -----------------------------------------------------------
 */

struct ReferencePictureList {

    std::vector<ShortTermReferencePicture> entries;


    [[nodiscard]]
    std::size_t size() const noexcept
    {
        return entries.size();
    }


    [[nodiscard]]
    bool empty() const noexcept
    {
        return entries.empty();
    }


    void clear()
    {
        entries.clear();
    }


    void reserve(std::size_t count)
    {
        entries.reserve(count);
    }


    [[nodiscard]]
    const ShortTermReferencePicture*
    at(std::size_t index) const noexcept
    {
        if (index >= entries.size()) {
            return nullptr;
        }

        return &entries[index];
    }


    [[nodiscard]]
    ShortTermReferencePicture*
    at(std::size_t index) noexcept
    {
        if (index >= entries.size()) {
            return nullptr;
        }

        return &entries[index];
    }
};


/*
 * -----------------------------------------------------------
 * Current-picture POC state
 * -----------------------------------------------------------
 */

struct PictureOrderCountState {

    /*
     * Current picture POC.
     */
    std::int32_t current_poc = 0;

    /*
     * Previous picture POC.
     */
    std::int32_t previous_poc = 0;

    /*
     * Previous PicOrderCntMsb.
     */
    std::int32_t previous_poc_msb = 0;

    /*
     * Previous PicOrderCntLsb.
     */
    std::uint32_t previous_poc_lsb = 0;

    /*
     * Whether previous-picture state is valid.
     */
    bool previous_valid = false;


    /*
     * Reset temporal POC state.
     */
    void reset() noexcept
    {
        current_poc = 0;
        previous_poc = 0;
        previous_poc_msb = 0;
        previous_poc_lsb = 0;
        previous_valid = false;
    }
};


/*
 * -----------------------------------------------------------
 * Reference-picture manager
 * -----------------------------------------------------------
 */

class ReferencePictureManager {
private:

    PictureOrderCountState poc_state_{};

    /*
     * Decoder reference-picture buffer.
     *
     * This contains pictures known to the reference manager.
     */
    std::vector<ShortTermReferencePicture>
        reference_pictures_{};


public:

    ReferencePictureManager() = default;

    ReferencePictureManager(
        const ReferencePictureManager&) = default;

    ReferencePictureManager(
        ReferencePictureManager&&) noexcept = default;

    ReferencePictureManager& operator=(
        const ReferencePictureManager&) = default;

    ReferencePictureManager& operator=(
        ReferencePictureManager&&) noexcept = default;

    ~ReferencePictureManager() = default;


    /*
     * -------------------------------------------------------
     * Reset
     * -------------------------------------------------------
     */

    void reset() noexcept
    {
        poc_state_.reset();
        reference_pictures_.clear();
    }


    void reset_poc() noexcept
    {
        poc_state_.reset();
    }


    void clear_references() noexcept
    {
        reference_pictures_.clear();
    }


    /*
     * -------------------------------------------------------
     * POC state access
     * -------------------------------------------------------
     */

    [[nodiscard]]
    const PictureOrderCountState&
    poc_state() const noexcept
    {
        return poc_state_;
    }


    [[nodiscard]]
    PictureOrderCountState&
    poc_state() noexcept
    {
        return poc_state_;
    }


    [[nodiscard]]
    std::int32_t
    current_poc() const noexcept
    {
        return poc_state_.current_poc;
    }


    [[nodiscard]]
    std::int32_t
    previous_poc() const noexcept
    {
        return poc_state_.previous_poc;
    }


    /*
     * -------------------------------------------------------
     * PicOrderCntMsb derivation
     * -------------------------------------------------------
     *
     * H.265 derives PicOrderCntMsb from:
     *
     *     prevPicOrderCntMsb
     *     prevPicOrderCntLsb
     *     PicOrderCntLsb
     *
     * and:
     *
     *     MaxPicOrderCntLsb
     *
     * The wraparound threshold is:
     *
     *     MaxPicOrderCntLsb / 2
     */

    [[nodiscard]]
    static std::int32_t
    derive_poc_msb(
        std::int32_t previous_poc_msb,
        std::uint32_t previous_poc_lsb,
        std::uint32_t current_poc_lsb,
        std::uint32_t max_poc_lsb) noexcept
    {
        if (max_poc_lsb == 0) {
            return previous_poc_msb;
        }

        const std::int64_t max_lsb =
            static_cast<std::int64_t>(
                max_poc_lsb);

        const std::int64_t half =
            max_lsb / 2;

        const std::int64_t prev_lsb =
            static_cast<std::int64_t>(
                previous_poc_lsb);

        const std::int64_t curr_lsb =
            static_cast<std::int64_t>(
                current_poc_lsb);

        std::int64_t poc_msb =
            static_cast<std::int64_t>(
                previous_poc_msb);

        if ((curr_lsb < prev_lsb) &&
            ((prev_lsb - curr_lsb) >= half)) {

            poc_msb += max_lsb;
        }
        else if ((curr_lsb > prev_lsb) &&
                 ((curr_lsb - prev_lsb) > half)) {

            poc_msb -= max_lsb;
        }

        if (poc_msb >
                std::numeric_limits<std::int32_t>::max()) {
            return std::numeric_limits<std::int32_t>::max();
        }

        if (poc_msb <
                std::numeric_limits<std::int32_t>::min()) {
            return std::numeric_limits<std::int32_t>::min();
        }

        return static_cast<std::int32_t>(
            poc_msb);
    }


    /*
     * -------------------------------------------------------
     * Current POC derivation
     * -------------------------------------------------------
     *
     * This function handles the ordinary non-IDR case.
     *
     * For IDR pictures the caller can explicitly set the POC
     * state using set_current_poc().
     */

    [[nodiscard]]
    std::int32_t
    derive_current_poc(
        std::uint32_t pic_order_cnt_lsb,
        std::uint32_t max_pic_order_cnt_lsb)
    {
        if (!poc_state_.previous_valid ||
            max_pic_order_cnt_lsb == 0) {

            const auto poc =
                static_cast<std::int32_t>(
                    pic_order_cnt_lsb);

            poc_state_.previous_poc =
                poc;

            poc_state_.previous_poc_msb =
                0;

            poc_state_.previous_poc_lsb =
                pic_order_cnt_lsb;

            poc_state_.current_poc =
                poc;

            poc_state_.previous_valid =
                true;

            return poc;
        }

        const auto poc_msb =
            derive_poc_msb(
                poc_state_.previous_poc_msb,
                poc_state_.previous_poc_lsb,
                pic_order_cnt_lsb,
                max_pic_order_cnt_lsb);

        const std::int64_t poc =
            static_cast<std::int64_t>(
                poc_msb) +
            static_cast<std::int64_t>(
                pic_order_cnt_lsb);

        std::int32_t result = 0;

        if (poc >
            std::numeric_limits<std::int32_t>::max()) {

            result =
                std::numeric_limits<std::int32_t>::max();

        } else if (
            poc <
            std::numeric_limits<std::int32_t>::min()) {

            result =
                std::numeric_limits<std::int32_t>::min();

        } else {

            result =
                static_cast<std::int32_t>(
                    poc);
        }

        poc_state_.previous_poc =
            poc_state_.current_poc;

        poc_state_.previous_poc_msb =
            poc_msb;

        poc_state_.previous_poc_lsb =
            pic_order_cnt_lsb;

        poc_state_.current_poc =
            result;

        poc_state_.previous_valid =
            true;

        return result;
    }


    /*
     * Explicitly set current POC.
     *
     * Useful for IDR pictures and tests.
     */
    void set_current_poc(
        std::int32_t poc) noexcept
    {
        poc_state_.previous_poc =
            poc_state_.current_poc;

        poc_state_.current_poc =
            poc;

        poc_state_.previous_valid =
            true;
    }


    /*
     * -------------------------------------------------------
     * Reference picture buffer
     * -------------------------------------------------------
     */

    [[nodiscard]]
    std::size_t
    reference_picture_count() const noexcept
    {
        return reference_pictures_.size();
    }


    [[nodiscard]]
    const ShortTermReferencePicture*
    reference_picture(
        std::size_t index) const noexcept
    {
        if (index >= reference_pictures_.size()) {
            return nullptr;
        }

        return &reference_pictures_[index];
    }


    void add_reference_picture(
        const ShortTermReferencePicture& picture)
    {
        reference_pictures_.push_back(picture);
    }


    void add_reference_picture(
        ShortTermReferencePicture&& picture)
    {
        reference_pictures_.push_back(
            std::move(picture));
    }


    /*
     * Remove a picture from the reference buffer.
     */
    bool remove_reference_picture(
        std::int32_t poc) noexcept
    {
        const auto it =
            std::find_if(
                reference_pictures_.begin(),
                reference_pictures_.end(),
                [poc](
                    const ShortTermReferencePicture& picture) {
                    return picture.poc == poc;
                });

        if (it == reference_pictures_.end()) {
            return false;
        }

        reference_pictures_.erase(it);
        return true;
    }


    /*
     * Find a reference picture by POC.
     */
    [[nodiscard]]
    const ShortTermReferencePicture*
    find_reference_picture(
        std::int32_t poc) const noexcept
    {
        const auto it =
            std::find_if(
                reference_pictures_.begin(),
                reference_pictures_.end(),
                [poc](
                    const ShortTermReferencePicture& picture) {
                    return picture.poc == poc;
                });

        if (it == reference_pictures_.end()) {
            return nullptr;
        }

        return &*it;
    }


    /*
     * -------------------------------------------------------
     * Explicit short-term RPS
     * -------------------------------------------------------
     *
     * Convert an explicitly coded RPS into actual reference
     * picture entries relative to the current POC.
     */

    [[nodiscard]]
    ReferencePictureList
    build_explicit_short_term_list(
        const ShortTermRefPicSet& rps,
        std::int32_t current_poc) const
    {
        ReferencePictureList result{};

        result.reserve(
            rps.negative_pics.size() +
            rps.positive_pics.size());

        /*
         * Negative pictures.
         */
        for (const auto& picture :
             rps.negative_pics) {

            ShortTermReferencePicture reference{};

            reference.delta_poc =
                picture.delta_poc;

            reference.poc =
                current_poc +
                picture.delta_poc;

            reference.used_by_curr_pic =
                picture.used_by_curr_pic;

            reference.long_term =
                false;

            result.entries.push_back(
                reference);
        }


        /*
         * Positive pictures.
         */
        for (const auto& picture :
             rps.positive_pics) {

            ShortTermReferencePicture reference{};

            reference.delta_poc =
                picture.delta_poc;

            reference.poc =
                current_poc +
                picture.delta_poc;

            reference.used_by_curr_pic =
                picture.used_by_curr_pic;

            reference.long_term =
                false;

            result.entries.push_back(
                reference);
        }

        return result;
    }


    /*
     * -------------------------------------------------------
     * Inter-RPS derivation
     * -------------------------------------------------------
     *
     * H.265 derives:
     *
     *     DeltaRps
     *
     * and then examines:
     *
     *     RefDeltaPoc[j]
     *
     * for:
     *
     *     j = 0 .. NumDeltaPocs[RefRpsIdx]
     *
     * followed by:
     *
     *     DeltaPoc[j]
     */

    [[nodiscard]]
    ReferencePictureList
    build_inter_predicted_list(
        const ShortTermRefPicSet& current,
        const ShortTermRefPicSet& reference,
        std::int32_t current_poc) const
    {
        ReferencePictureList result{};

        const auto& prediction =
            current.inter_prediction;

        /*
         * Build the reference RPS delta list.
         *
         * H.265 ordering is:
         *
         *     negative pictures
         *     positive pictures
         */
        std::vector<std::int32_t>
            reference_delta_pocs;

        reference_delta_pocs.reserve(
            reference.negative_pics.size() +
            reference.positive_pics.size());

        for (const auto& picture :
             reference.negative_pics) {

            reference_delta_pocs.push_back(
                picture.delta_poc);
        }

        for (const auto& picture :
             reference.positive_pics) {

            reference_delta_pocs.push_back(
                picture.delta_poc);
        }


        /*
         * DeltaRps.
         */
        const std::int32_t delta_rps =
            prediction.delta_rps != 0
                ? prediction.delta_rps
                : calculate_delta_rps(
                    prediction.delta_rps_sign,
                    prediction.abs_delta_rps_minus1);


        /*
         * The syntax has one extra entry corresponding to
         * the special RefDeltaPoc value zero.
         *
         * The exact ordering is:
         *
         *     j == NumDeltaPocs[RefRpsIdx]
         *
         * for the zero entry.
         */
        const std::size_t reference_count =
            reference_delta_pocs.size();


        for (std::size_t j = 0;
             j <= reference_count;
             ++j) {

            const std::int32_t
                ref_delta_poc =
                (j < reference_count)
                    ? reference_delta_pocs[j]
                    : 0;

            const std::int32_t
                delta_poc =
                ref_delta_poc +
                delta_rps;

            /*
             * Determine the corresponding prediction flags.
             */
            bool used = false;
            bool use_delta = false;

            if (j < prediction.entries.size()) {

                const auto& entry =
                    prediction.entries[j];

                used =
                    entry.used_by_curr_pic_flag;

                use_delta =
                    entry.use_delta_flag;
            }

            /*
             * The derived RPS contains the entry when:
             *
             *     used_by_curr_pic_flag == 1
             *
             * or:
             *
             *     use_delta_flag == 1
             */
            if (!used && !use_delta) {
                continue;
            }

            ShortTermReferencePicture picture{};

            picture.delta_poc =
                delta_poc;

            picture.poc =
                current_poc +
                delta_poc;

            picture.used_by_curr_pic =
                used;

            picture.long_term =
                false;

            result.entries.push_back(
                picture);
        }

        /*
         * H.265 expects negative POCs before positive POCs.
         *
         * Keep the semantic result ordered by delta POC.
         */
        std::stable_sort(
            result.entries.begin(),
            result.entries.end(),
            [](const auto& lhs,
               const auto& rhs) {
                return lhs.delta_poc <
                       rhs.delta_poc;
            });

        return result;
    }


    /*
     * -------------------------------------------------------
     * Generic RPS derivation
     * -------------------------------------------------------
     */

    [[nodiscard]]
    ReferencePictureList
    build_short_term_rps(
        const std::vector<ShortTermRefPicSet>& rps_sets,
        std::size_t rps_index,
        std::int32_t current_poc) const
    {
        ReferencePictureList empty{};

        if (rps_index >= rps_sets.size()) {
            return empty;
        }

        const auto& rps =
            rps_sets[rps_index];

        if (!rps.inter_ref_pic_set_prediction_flag) {
            return build_explicit_short_term_list(
                rps,
                current_poc);
        }

        const auto reference_index =
            static_cast<std::size_t>(
                rps.inter_prediction.reference_rps_idx);

        if (reference_index >= rps_sets.size()) {
            return empty;
        }

        return build_inter_predicted_list(
            rps,
            rps_sets[reference_index],
            current_poc);
    }


    /*
     * -------------------------------------------------------
     * Build reference lists
     * -------------------------------------------------------
     *
     * HEVC list construction starts from:
     *
     *     RefPicSetStCurrBefore
     *     RefPicSetStCurrAfter
     *     RefPicSetLtCurr
     *
     * For now this manager exposes the short-term semantic
     * lists explicitly. Slice-header list modification can
     * subsequently reorder these entries.
     */

    [[nodiscard]]
    static ReferencePictureList
    build_list0(
        const ReferencePictureList& short_term)
    {
        ReferencePictureList result{};

        /*
         * Negative POCs first.
         */
        for (const auto& picture :
             short_term.entries) {

            if (picture.delta_poc < 0 &&
                picture.used_by_curr_pic) {

                result.entries.push_back(
                    picture);
            }
        }

        /*
         * Then positive POCs.
         */
        for (const auto& picture :
             short_term.entries) {

            if (picture.delta_poc > 0 &&
                picture.used_by_curr_pic) {

                result.entries.push_back(
                    picture);
            }
        }

        return result;
    }


    [[nodiscard]]
    static ReferencePictureList
    build_list1(
        const ReferencePictureList& short_term)
    {
        ReferencePictureList result{};

        /*
         * B-slice List 1 starts with the opposite ordering:
         *
         *     StCurrAfter
         *     StCurrBefore
         */
        for (const auto& picture :
             short_term.entries) {

            if (picture.delta_poc > 0 &&
                picture.used_by_curr_pic) {

                result.entries.push_back(
                    picture);
            }
        }

        for (const auto& picture :
             short_term.entries) {

            if (picture.delta_poc < 0 &&
                picture.used_by_curr_pic) {

                result.entries.push_back(
                    picture);
            }
        }

        return result;
    }


    /*
     * -------------------------------------------------------
     * Reference list modification
     * -------------------------------------------------------
     *
     * list_entry_lx is an index into the original reference
     * picture list.
     */

    [[nodiscard]]
    static ReferencePictureList
    apply_list_modification(
        const ReferencePictureList& source,
        const std::vector<std::uint32_t>& indices)
    {
        ReferencePictureList result{};

        result.entries.reserve(
            indices.size());

        for (const auto index : indices) {

            if (index >= source.entries.size()) {
                continue;
            }

            result.entries.push_back(
                source.entries[index]);
        }

        return result;
    }


    /*
     * -------------------------------------------------------
     * Store RPS references in decoder state
     * -------------------------------------------------------
     */

    void activate_reference_list(
        const ReferencePictureList& list)
    {
        for (const auto& picture :
             list.entries) {

            if (!picture.used_by_curr_pic) {
                continue;
            }

            add_reference_picture(
                picture);
        }
    }


    /*
     * -------------------------------------------------------
     * Remove unused pictures
     * -------------------------------------------------------
     */

    void remove_unused_references()
    {
        reference_pictures_.erase(
            std::remove_if(
                reference_pictures_.begin(),
                reference_pictures_.end(),
                [](const auto& picture) {
                    return !picture.used_by_curr_pic;
                }),
            reference_pictures_.end());
    }
};


/*
 * -----------------------------------------------------------
 * Standalone POC helper
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline std::int32_t
derive_pic_order_cnt_msb(
    std::int32_t previous_poc_msb,
    std::uint32_t previous_poc_lsb,
    std::uint32_t current_poc_lsb,
    std::uint32_t max_poc_lsb) noexcept
{
    return ReferencePictureManager::derive_poc_msb(
        previous_poc_msb,
        previous_poc_lsb,
        current_poc_lsb,
        max_poc_lsb);
}


/*
 * -----------------------------------------------------------
 * Explicit RPS convenience helper
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline ReferencePictureList
derive_short_term_reference_list(
    const ShortTermRefPicSet& rps,
    std::int32_t current_poc)
{
    ReferencePictureManager manager{};

    return manager.build_explicit_short_term_list(
        rps,
        current_poc);
}

} // namespace bs