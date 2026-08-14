#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <limits>

namespace bs {

/*
 * H.265 short-term reference picture set.
 *
 * Syntax:
 *
 * short_term_ref_pic_set( stRpsIdx ) {
 *
 *     if( stRpsIdx != 0 )
 *         inter_ref_pic_set_prediction_flag
 *
 *     if( inter_ref_pic_set_prediction_flag ) {
 *
 *         if( stRpsIdx == num_short_term_ref_pic_sets )
 *             delta_idx_minus1
 *
 *         delta_rps_sign
 *         abs_delta_rps_minus1
 *
 *         for( j = 0;
 *              j <= NumDeltaPocs[RefRpsIdx];
 *              j++ ) {
 *
 *             used_by_curr_pic_flag
 *
 *             if( !used_by_curr_pic_flag )
 *                 use_delta_flag
 *         }
 *
 *     } else {
 *
 *         num_negative_pics
 *         num_positive_pics
 *
 *         for( i = 0;
 *              i < num_negative_pics;
 *              i++ ) {
 *
 *             delta_poc_s0_minus1
 *             used_by_curr_pic_s0_flag
 *         }
 *
 *         for( i = 0;
 *              i < num_positive_pics;
 *              i++ ) {
 *
 *             delta_poc_s1_minus1
 *             used_by_curr_pic_s1_flag
 *         }
 *     }
 * }
 */

/*
 * One explicitly-coded negative picture.
 *
 * Syntax:
 *
 *     delta_poc_s0_minus1
 *     used_by_curr_pic_s0_flag
 */
struct ShortTermNegativePicture {
    /*
     * Syntax value.
     *
     * Actual delta POC is derived from this.
     */
    std::uint32_t delta_poc_minus1 = 0;

    /*
     * Whether this picture is used by the current picture.
     */
    bool used_by_curr_pic = false;

    /*
     * Derived delta POC.
     *
     * This is normally negative.
     *
     * Keeping it here saves every consumer from having to
     * repeat the H.265 derivation.
     */
    std::int32_t delta_poc = 0;
};

/*
 * One explicitly-coded positive picture.
 *
 * Syntax:
 *
 *     delta_poc_s1_minus1
 *     used_by_curr_pic_s1_flag
 */
struct ShortTermPositivePicture {
    std::uint32_t delta_poc_minus1 = 0;

    bool used_by_curr_pic = false;

    /*
     * Derived delta POC.
     *
     * This is normally positive.
     */
    std::int32_t delta_poc = 0;
};

/*
 * Inter-RPS prediction entry.
 *
 * H.265 has:
 *
 *     used_by_curr_pic_flag[j]
 *
 * and, when that flag is zero:
 *
 *     use_delta_flag[j]
 *
 * There is one entry for:
 *
 *     j = 0 .. NumDeltaPocs[RefRpsIdx]
 */
struct InterRpsPredictionEntry {
    bool used_by_curr_pic_flag = false;

    /*
     * Present only when used_by_curr_pic_flag == false.
     *
     * If false, the referenced delta POC isn't used for
     * deriving the current RPS.
     */
    bool use_delta_flag = false;
};

/*
 * Complete inter-RPS prediction information.
 */
struct InterRpsPrediction {
    /*
     * delta_idx_minus1 is only present when the current RPS
     * is being predicted and:
     *
     *     stRpsIdx == num_short_term_ref_pic_sets
     *
     * i.e. when a slice-level additional RPS is being
     * signaled.
     */
    bool delta_idx_present = false;

    std::uint32_t delta_idx_minus1 = 0;

    /*
     * Derived reference RPS index.
     */
    std::uint32_t reference_rps_idx = 0;

    /*
     * delta_rps_sign
     */
    bool delta_rps_sign = false;

    /*
     * abs_delta_rps_minus1
     */
    std::uint32_t abs_delta_rps_minus1 = 0;

    /*
     * Derived:
     *
     * DeltaRps =
     *     (1 - 2 * delta_rps_sign)
     *     * (abs_delta_rps_minus1 + 1)
     */
    std::int32_t delta_rps = 0;

    /*
     * One entry for every:
     *
     *     j = 0 .. NumDeltaPocs[RefRpsIdx]
     *
     * The vector therefore contains:
     *
     *     NumDeltaPocs[RefRpsIdx] + 1
     *
     * entries.
     */
    std::vector<InterRpsPredictionEntry> entries;
};

/*
 * Complete short-term reference picture set.
 */
struct ShortTermRefPicSet {
    /*
     * RPS index within the SPS short-term RPS array.
     */
    std::uint32_t index = 0;

    /*
     * inter_ref_pic_set_prediction_flag
     */
    bool inter_ref_pic_set_prediction_flag = false;

    /*
     * Explicit RPS representation.
     *
     * Used when:
     *
     *     inter_ref_pic_set_prediction_flag == false
     */
    std::uint32_t num_negative_pics = 0;

    std::uint32_t num_positive_pics = 0;

    std::vector<ShortTermNegativePicture> negative_pics;

    std::vector<ShortTermPositivePicture> positive_pics;

    /*
     * Inter-RPS representation.
     *
     * Used when:
     *
     *     inter_ref_pic_set_prediction_flag == true
     */
    InterRpsPrediction inter_prediction{};

    /*
     * Derived total number of delta POCs.
     *
     * H.265:
     *
     *     NumDeltaPocs[stRpsIdx]
     *
     * is:
     *
     *     NumNegativePics + NumPositivePics
     *
     * for an explicitly coded RPS.
     */
    std::uint32_t num_delta_pocs = 0;

    /*
     * -------------------------------------------------------
     * Helpers
     * -------------------------------------------------------
     */

    [[nodiscard]]
    bool is_explicit() const noexcept {
        return !inter_ref_pic_set_prediction_flag;
    }

    [[nodiscard]]
    bool is_predicted() const noexcept {
        return inter_ref_pic_set_prediction_flag;
    }

    [[nodiscard]]
    std::size_t negative_count() const noexcept {
        return negative_pics.size();
    }

    [[nodiscard]]
    std::size_t positive_count() const noexcept {
        return positive_pics.size();
    }

    [[nodiscard]]
    std::size_t delta_poc_count() const noexcept {
        return static_cast<std::size_t>(num_delta_pocs);
    }
};

/*
 * -----------------------------------------------------------
 * Derived RPS representation
 * -----------------------------------------------------------
 *
 * For decoding, it is often much more convenient to have a
 * flat list of delta POCs instead of repeatedly traversing
 * the syntax-specific representation.
 */

/*
 * A derived short-term reference picture.
 */
struct DerivedShortTermReference {
    /*
     * Delta POC relative to the current picture.
     */
    std::int32_t delta_poc = 0;

    /*
     * Whether the picture is used by the current picture.
     */
    bool used_by_curr_pic = false;

    /*
     * true  -> negative reference picture
     * false -> positive reference picture
     */
    bool negative = false;
};

/*
 * Fully derived short-term RPS.
 *
 * This is a semantic representation, not a direct syntax
 * structure.
 */
struct DerivedShortTermRefPicSet {
    std::vector<DerivedShortTermReference> references;

    /*
     * Number of negative and positive pictures.
     */
    std::uint32_t num_negative_pics = 0;
    std::uint32_t num_positive_pics = 0;

    [[nodiscard]]
    std::size_t size() const noexcept {
        return references.size();
    }

    [[nodiscard]]
    bool empty() const noexcept {
        return references.empty();
    }
};

/*
 * -----------------------------------------------------------
 * Validation helpers
 * -----------------------------------------------------------
 */

/*
 * H.265 permits up to 64 short-term reference picture sets
 * in the SPS syntax context.
 *
 * Keep this as a named constant so parser constraints aren't
 * scattered through the code.
 */
inline constexpr std::uint32_t kMaxShortTermRefPicSets = 64;

/*
 * Validate an RPS index.
 */
[[nodiscard]]
constexpr bool valid_rps_index(std::uint32_t index, std::uint32_t count) noexcept {
    return index < count;
}

/*
 * Validate the explicit RPS counts.
 */
[[nodiscard]]
constexpr bool valid_explicit_rps_counts(
    std::uint32_t num_negative, std::uint32_t num_positive
) noexcept {
    /*
     * The exact maximum is context-dependent through the
     * decoder/reference-picture constraints.
     *
     * Don't impose an arbitrary low limit here.
     */
    return num_negative <= std::numeric_limits<std::uint32_t>::max() &&
           num_positive <= std::numeric_limits<std::uint32_t>::max();
}

/*
 * -----------------------------------------------------------
 * Explicit RPS derivation
 * -----------------------------------------------------------
 *
 * H.265 derives:
 *
 *     DeltaPocS0[0] = -(
 *         delta_poc_s0_minus1[0] + 1
 *     )
 *
 *     DeltaPocS0[i] =
 *         DeltaPocS0[i-1]
 *         - delta_poc_s0_minus1[i] - 1
 *
 * and:
 *
 *     DeltaPocS1[0] =
 *         delta_poc_s1_minus1[0] + 1
 *
 *     DeltaPocS1[i] =
 *         DeltaPocS1[i-1]
 *         + delta_poc_s1_minus1[i] + 1
 */
inline void derive_explicit_rps(ShortTermRefPicSet& rps) {
    rps.num_negative_pics = static_cast<std::uint32_t>(rps.negative_pics.size());

    rps.num_positive_pics = static_cast<std::uint32_t>(rps.positive_pics.size());

    rps.num_delta_pocs = rps.num_negative_pics + rps.num_positive_pics;

    /*
     * Negative POCs.
     */
    std::int64_t previous_negative = 0;

    for (std::size_t i = 0; i < rps.negative_pics.size(); ++i) {
        const auto delta = static_cast<std::int64_t>(rps.negative_pics[i].delta_poc_minus1) + 1;

        if (i == 0) {
            previous_negative = -delta;
        } else {
            previous_negative -= delta;
        }

        rps.negative_pics[i].delta_poc = static_cast<std::int32_t>(previous_negative);
    }

    /*
     * Positive POCs.
     */
    std::int64_t previous_positive = 0;

    for (std::size_t i = 0; i < rps.positive_pics.size(); ++i) {
        const auto delta = static_cast<std::int64_t>(rps.positive_pics[i].delta_poc_minus1) + 1;

        if (i == 0) {
            previous_positive = delta;
        } else {
            previous_positive += delta;
        }

        rps.positive_pics[i].delta_poc = static_cast<std::int32_t>(previous_positive);
    }
}

/*
 * -----------------------------------------------------------
 * Inter-RPS prediction helpers
 * -----------------------------------------------------------
 */

/*
 * Calculate:
 *
 *     DeltaRps =
 *         (1 - 2 * delta_rps_sign)
 *         * (abs_delta_rps_minus1 + 1)
 */
[[nodiscard]]
constexpr std::int32_t calculate_delta_rps(
    bool delta_rps_sign, std::uint32_t abs_delta_rps_minus1
) noexcept {
    const std::int64_t magnitude = static_cast<std::int64_t>(abs_delta_rps_minus1) + 1;

    const std::int64_t signed_value = delta_rps_sign ? -magnitude : magnitude;

    return static_cast<std::int32_t>(signed_value);
}

/*
 * Return the number of inter-RPS prediction entries.
 *
 * The syntax loops:
 *
 *     j = 0 .. NumDeltaPocs[RefRpsIdx]
 *
 * therefore the count is:
 *
 *     NumDeltaPocs + 1
 */
[[nodiscard]]
constexpr std::size_t inter_rps_prediction_entry_count(
    std::uint32_t reference_num_delta_pocs
) noexcept {
    return static_cast<std::size_t>(reference_num_delta_pocs) + 1;
}

/*
 * Initialize the inter-RPS prediction entry array.
 */
inline void initialize_inter_rps_prediction(
    InterRpsPrediction& prediction, std::uint32_t reference_num_delta_pocs
) {
    prediction.entries.clear();

    prediction.entries.resize(inter_rps_prediction_entry_count(reference_num_delta_pocs));
}

/*
 * -----------------------------------------------------------
 * Semantic flattening
 * -----------------------------------------------------------
 *
 * For an explicit RPS this is straightforward.
 *
 * For an inter-predicted RPS, the reference RPS is required
 * to derive the final list. That operation belongs in the
 * parser/decoder layer because it depends on the previously
 * parsed RPS.
 */

/*
 * Convert an explicitly-coded RPS to the semantic form.
 */
[[nodiscard]]
inline DerivedShortTermRefPicSet derive_explicit_references(const ShortTermRefPicSet& rps) {
    DerivedShortTermRefPicSet result{};

    result.references.reserve(rps.negative_pics.size() + rps.positive_pics.size());

    /*
     * Negative references are already ordered according to
     * DeltaPocS0.
     */
    for (const auto& pic : rps.negative_pics) {
        result.references.push_back({pic.delta_poc, pic.used_by_curr_pic, true});
    }

    /*
     * Positive references.
     */
    for (const auto& pic : rps.positive_pics) {
        result.references.push_back({pic.delta_poc, pic.used_by_curr_pic, false});
    }

    result.num_negative_pics = static_cast<std::uint32_t>(rps.negative_pics.size());

    result.num_positive_pics = static_cast<std::uint32_t>(rps.positive_pics.size());

    return result;
}

/*
 * -----------------------------------------------------------
 * RPS lookup
 * -----------------------------------------------------------
 */

/*
 * Find an RPS by index.
 */
[[nodiscard]]
inline const ShortTermRefPicSet* find_short_term_rps(
    const std::vector<ShortTermRefPicSet>& sets, std::uint32_t index
) noexcept {
    if (index >= sets.size()) {
        return nullptr;
    }

    return &sets[index];
}

/*
 * Mutable lookup.
 */
[[nodiscard]]
inline ShortTermRefPicSet* find_short_term_rps(
    std::vector<ShortTermRefPicSet>& sets, std::uint32_t index
) noexcept {
    if (index >= sets.size()) {
        return nullptr;
    }

    return &sets[index];
}

}  // namespace bs