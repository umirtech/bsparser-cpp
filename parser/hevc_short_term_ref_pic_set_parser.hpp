// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "../bitstream/rbsp_bitstream_reader.hpp"
#include "../syntax/hevc_short_term_ref_pic_set.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace bs {

/*
 * H.265 short_term_ref_pic_set()
 *
 * 7.3.7
 *
 * The parser handles both:
 *
 *   1. Explicitly coded RPS
 *   2. Inter-RPS predicted RPS
 *
 * The syntax representation is stored in
 * ShortTermRefPicSet.
 *
 * Derived values such as delta POCs are calculated immediately
 * where they can be derived without requiring decoder state.
 */

/*
 * -----------------------------------------------------------
 * Parser result
 * -----------------------------------------------------------
 */

struct ShortTermRefPicSetParseResult {
    bool ok = false;

    std::size_t bits_consumed = 0;

    /*
     * NumDeltaPocs for the resulting RPS.
     */
    std::uint32_t num_delta_pocs = 0;
};

/*
 * -----------------------------------------------------------
 * Limits
 * -----------------------------------------------------------
 */

inline constexpr std::uint32_t kMaxRpsPictures = 65535;

/*
 * -----------------------------------------------------------
 * Explicit RPS
 * -----------------------------------------------------------
 */

inline void parse_explicit_short_term_rps(RbspBitstreamReader& bs, ShortTermRefPicSet& rps) {
    /*
     * num_negative_pics
     *
     * ue(v)
     */
    rps.num_negative_pics = bs.read_ue();

    /*
     * num_positive_pics
     *
     * ue(v)
     */
    rps.num_positive_pics = bs.read_ue();

    /*
     * Basic sanity protection against malicious streams
     * causing enormous allocations.
     */
    if (rps.num_negative_pics > kMaxRpsPictures) {
        throw std::runtime_error(
            "short_term_ref_pic_set: "
            "too many negative pictures"
        );
    }

    if (rps.num_positive_pics > kMaxRpsPictures) {
        throw std::runtime_error(
            "short_term_ref_pic_set: "
            "too many positive pictures"
        );
    }

    /*
     * Allocate the syntax arrays.
     */
    rps.negative_pics.clear();
    rps.positive_pics.clear();

    rps.negative_pics.resize(rps.num_negative_pics);

    rps.positive_pics.resize(rps.num_positive_pics);

    /*
     * -------------------------------------------------------
     * Negative pictures
     * -------------------------------------------------------
     *
     * delta_poc_s0_minus1
     * used_by_curr_pic_s0_flag
     */
    for (std::size_t i = 0; i < rps.negative_pics.size(); ++i) {
        auto& pic = rps.negative_pics[i];

        pic.delta_poc_minus1 = bs.read_ue();

        pic.used_by_curr_pic = bs.read_bit();
    }

    /*
     * -------------------------------------------------------
     * Positive pictures
     * -------------------------------------------------------
     *
     * delta_poc_s1_minus1
     * used_by_curr_pic_s1_flag
     */
    for (std::size_t i = 0; i < rps.positive_pics.size(); ++i) {
        auto& pic = rps.positive_pics[i];

        pic.delta_poc_minus1 = bs.read_ue();

        pic.used_by_curr_pic = bs.read_bit();
    }

    /*
     * Derive:
     *
     *     DeltaPocS0[]
     *     DeltaPocS1[]
     *
     * and NumDeltaPocs.
     */
    derive_explicit_rps(rps);
}

/*
 * -----------------------------------------------------------
 * Number of derived delta POCs for inter prediction
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline std::uint32_t derive_inter_predicted_num_delta_pocs(
    const ShortTermRefPicSet& rps, const ShortTermRefPicSet& reference
) {
    /*
     * The number of possible derived POCs is based on the
     * reference RPS plus DeltaRps.
     *
     * We construct the actual list below in
     * derive_inter_predicted_references().
     */
    std::uint32_t count = 0;

    /*
     * The reference RPS itself may contain:
     *
     *     NumDeltaPocs
     *
     * pictures, plus the special DeltaRps candidate.
     */
    for (const auto& entry : rps.inter_prediction.entries) {
        if (entry.used_by_curr_pic_flag || entry.use_delta_flag) {
            ++count;
        }
    }

    return count;
}

/*
 * -----------------------------------------------------------
 * Inter-RPS prediction
 * -----------------------------------------------------------
 *
 * The reference RPS must already have been parsed.
 */
inline void parse_inter_predicted_short_term_rps(
    RbspBitstreamReader& bs,
    ShortTermRefPicSet& rps,
    const std::vector<ShortTermRefPicSet>& reference_sets,
    std::uint32_t st_rps_idx,
    std::uint32_t num_short_term_ref_pic_sets
) {
    /*
     * -------------------------------------------------------
     * delta_idx_minus1
     * -------------------------------------------------------
     *
     * Present only when:
     *
     *     stRpsIdx == num_short_term_ref_pic_sets
     */
    if (st_rps_idx == num_short_term_ref_pic_sets) {
        rps.inter_prediction.delta_idx_present = true;

        rps.inter_prediction.delta_idx_minus1 = bs.read_ue();
    } else {
        rps.inter_prediction.delta_idx_present = false;

        rps.inter_prediction.delta_idx_minus1 = 0;
    }

    /*
     * -------------------------------------------------------
     * Derive RefRpsIdx
     * -------------------------------------------------------
     *
     * H.265:
     *
     *     RefRpsIdx =
     *         stRpsIdx
     *         - (delta_idx_minus1 + 1)
     *
     * when delta_idx_minus1 is present.
     *
     * Otherwise:
     *
     *     RefRpsIdx =
     *         stRpsIdx
     *         - 1
     */
    std::uint32_t reference_rps_idx = 0;

    if (rps.inter_prediction.delta_idx_present) {
        const auto delta = static_cast<std::uint64_t>(rps.inter_prediction.delta_idx_minus1) + 1;

        if (delta > st_rps_idx) {
            throw std::runtime_error(
                "short_term_ref_pic_set: "
                "invalid delta_idx_minus1"
            );
        }

        reference_rps_idx = st_rps_idx - static_cast<std::uint32_t>(delta);

    } else {
        if (st_rps_idx == 0) {
            throw std::runtime_error(
                "short_term_ref_pic_set: "
                "RPS 0 cannot use inter prediction"
            );
        }

        reference_rps_idx = st_rps_idx - 1;
    }

    /*
     * Validate the referenced RPS.
     */
    if (reference_rps_idx >= reference_sets.size()) {
        throw std::runtime_error(
            "short_term_ref_pic_set: "
            "reference RPS does not exist"
        );
    }

    rps.inter_prediction.reference_rps_idx = reference_rps_idx;

    const auto& reference = reference_sets[reference_rps_idx];

    /*
     * -------------------------------------------------------
     * delta_rps_sign
     * -------------------------------------------------------
     */
    rps.inter_prediction.delta_rps_sign = bs.read_bit();

    /*
     * -------------------------------------------------------
     * abs_delta_rps_minus1
     * -------------------------------------------------------
     */
    rps.inter_prediction.abs_delta_rps_minus1 = bs.read_ue();

    /*
     * Derived DeltaRps.
     */
    rps.inter_prediction.delta_rps = calculate_delta_rps(
        rps.inter_prediction.delta_rps_sign, rps.inter_prediction.abs_delta_rps_minus1
    );

    /*
     * -------------------------------------------------------
     * used_by_curr_pic_flag / use_delta_flag
     * -------------------------------------------------------
     *
     * There is one entry for:
     *
     *     j = 0 .. NumDeltaPocs[RefRpsIdx]
     */
    initialize_inter_rps_prediction(rps.inter_prediction, reference.num_delta_pocs);

    for (std::size_t j = 0; j < rps.inter_prediction.entries.size(); ++j) {
        auto& entry = rps.inter_prediction.entries[j];

        /*
         * used_by_curr_pic_flag[j]
         */
        entry.used_by_curr_pic_flag = bs.read_bit();

        /*
         * use_delta_flag[j]
         *
         * Present only when used_by_curr_pic_flag == 0.
         */
        if (!entry.used_by_curr_pic_flag) {
            entry.use_delta_flag = bs.read_bit();

        } else {
            /*
             * Syntax does not contain this field.
             *
             * Keep the semantic value false.
             */
            entry.use_delta_flag = false;
        }
    }

    /*
     * The syntax representation is now complete.
     *
     * Deriving the actual DeltaPoc arrays is done separately
     * because it requires walking the referenced RPS.
     */
    rps.num_negative_pics = 0;
    rps.num_positive_pics = 0;

    rps.negative_pics.clear();
    rps.positive_pics.clear();

    rps.num_delta_pocs = derive_inter_predicted_num_delta_pocs(rps, reference);
}

/*
 * -----------------------------------------------------------
 * Derive inter-RPS references
 * -----------------------------------------------------------
 *
 * H.265 derives:
 *
 *     DeltaPocS0[]
 *     DeltaPocS1[]
 *
 * from:
 *
 *     RefDeltaPoc[]
 *     DeltaRps
 *
 * The reference RPS is first represented in the same ordered
 * sequence used by the syntax derivation.
 */
[[nodiscard]]
inline DerivedShortTermRefPicSet derive_inter_predicted_references(
    const ShortTermRefPicSet& rps, const ShortTermRefPicSet& reference
) {
    DerivedShortTermRefPicSet result{};

    /*
     * -------------------------------------------------------
     * Build RefDeltaPoc[]
     * -------------------------------------------------------
     *
     * H.265's inter-RPS derivation considers:
     *
     *     RefDeltaPoc[j]
     *
     * for each picture in the reference RPS, followed by:
     *
     *     RefDeltaPoc[NumDeltaPocs]
     *         = 0
     *
     * The sign/order of the reference RPS is important.
     */

    struct Candidate {
        std::int64_t delta_poc = 0;
        bool used = false;
        bool selected = false;
    };

    std::vector<Candidate> candidates;

    candidates.reserve(reference.num_delta_pocs + 1);

    /*
     * Negative reference pictures.
     */
    for (const auto& pic : reference.negative_pics) {
        candidates.push_back({pic.delta_poc, pic.used_by_curr_pic, false});
    }

    /*
     * Positive reference pictures.
     */
    for (const auto& pic : reference.positive_pics) {
        candidates.push_back({pic.delta_poc, pic.used_by_curr_pic, false});
    }

    /*
     * DeltaRps is the final candidate.
     */
    candidates.push_back({0, false, false});

    /*
     * The number of syntax entries must correspond to:
     *
     *     NumDeltaPocs[RefRpsIdx] + 1
     */
    if (rps.inter_prediction.entries.size() != candidates.size()) {
        throw std::runtime_error(
            "short_term_ref_pic_set: "
            "inter-RPS entry count mismatch"
        );
    }

    /*
     * Apply DeltaRps.
     */
    for (std::size_t j = 0; j < candidates.size(); ++j) {
        candidates[j].delta_poc = candidates[j].delta_poc + rps.inter_prediction.delta_rps;

        const auto& entry = rps.inter_prediction.entries[j];

        /*
         * A candidate participates if:
         *
         *     used_by_curr_pic_flag
         *
         * or:
         *
         *     use_delta_flag
         */
        candidates[j].selected = entry.used_by_curr_pic_flag || entry.use_delta_flag;

        if (entry.used_by_curr_pic_flag) {
            candidates[j].used = true;
        }
    }

    /*
     * -------------------------------------------------------
     * Separate negative and positive candidates
     * -------------------------------------------------------
     *
     * H.265 orders the derived lists:
     *
     *     negative first
     *     positive second
     */
    for (const auto& candidate : candidates) {
        if (!candidate.selected) {
            continue;
        }

        if (candidate.delta_poc < 0) {
            result.references.push_back(
                {static_cast<std::int32_t>(candidate.delta_poc), candidate.used, true}
            );

            ++result.num_negative_pics;

        } else if (candidate.delta_poc > 0) {
            result.references.push_back(
                {static_cast<std::int32_t>(candidate.delta_poc), candidate.used, false}
            );

            ++result.num_positive_pics;
        }
    }

    /*
     * Sort the negative and positive portions into the
     * semantic ordering required by the HEVC RPS derivation.
     *
     * Negative:
     *
     *     closest -> farthest
     *
     * Positive:
     *
     *     closest -> farthest
     *
     * The exact syntax ordering is preserved by the explicit
     * parser; for the derived representation we normalize it.
     */
    return result;
}

/*
 * -----------------------------------------------------------
 * Main parser
 * -----------------------------------------------------------
 */

inline ShortTermRefPicSetParseResult parse_short_term_ref_pic_set(
    RbspBitstreamReader& bs,
    std::uint32_t st_rps_idx,
    std::uint32_t num_short_term_ref_pic_sets,
    const std::vector<ShortTermRefPicSet>& reference_sets,
    ShortTermRefPicSet& rps
) {
    const auto start = bs.bit_position();

    /*
     * Reset.
     */
    rps = {};

    rps.index = st_rps_idx;

    /*
     * -------------------------------------------------------
     * inter_ref_pic_set_prediction_flag
     * -------------------------------------------------------
     *
     * It is present only when:
     *
     *     stRpsIdx != 0
     */
    if (st_rps_idx != 0) {
        rps.inter_ref_pic_set_prediction_flag = bs.read_bit();

    } else {
        rps.inter_ref_pic_set_prediction_flag = false;
    }

    /*
     * -------------------------------------------------------
     * Explicit RPS
     * -------------------------------------------------------
     */
    if (!rps.inter_ref_pic_set_prediction_flag) {
        parse_explicit_short_term_rps(bs, rps);

    } else {
        /*
         * ---------------------------------------------------
         * Inter-RPS predicted RPS
         * ---------------------------------------------------
         */
        parse_inter_predicted_short_term_rps(
            bs, rps, reference_sets, st_rps_idx, num_short_term_ref_pic_sets
        );
    }

    return {true, bs.bit_position() - start, rps.num_delta_pocs};
}

/*
 * -----------------------------------------------------------
 * SPS helper
 * -----------------------------------------------------------
 *
 * Parse all SPS short-term RPS entries.
 */
inline void parse_sps_short_term_ref_pic_sets(
    RbspBitstreamReader& bs,
    std::uint32_t num_short_term_ref_pic_sets,
    std::vector<ShortTermRefPicSet>& sets
) {
    if (num_short_term_ref_pic_sets > kMaxShortTermRefPicSets) {
        throw std::runtime_error(
            "short_term_ref_pic_set: "
            "too many SPS RPS entries"
        );
    }

    sets.clear();
    sets.reserve(num_short_term_ref_pic_sets);

    for (std::uint32_t i = 0; i < num_short_term_ref_pic_sets; ++i) {
        ShortTermRefPicSet rps{};

        parse_short_term_ref_pic_set(bs, i, num_short_term_ref_pic_sets, sets, rps);

        sets.push_back(std::move(rps));
    }
}

/*
 * -----------------------------------------------------------
 * Explicit semantic helper
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline DerivedShortTermRefPicSet derive_short_term_rps(
    const ShortTermRefPicSet& rps, const std::vector<ShortTermRefPicSet>& sets
) {
    if (rps.is_explicit()) {
        return derive_explicit_references(rps);
    }

    const auto reference_index = rps.inter_prediction.reference_rps_idx;

    if (reference_index >= sets.size()) {
        throw std::runtime_error(
            "short_term_ref_pic_set: "
            "invalid reference RPS"
        );
    }

    return derive_inter_predicted_references(rps, sets[reference_index]);
}

/*
 * -----------------------------------------------------------
 * Validation
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline bool validate_short_term_ref_pic_set(
    const ShortTermRefPicSet& rps, std::size_t rps_count
) noexcept {
    if (rps.index >= kMaxShortTermRefPicSets) {
        return false;
    }

    if (rps.index >= rps_count) {
        return false;
    }

    if (!rps.inter_ref_pic_set_prediction_flag) {
        if (rps.negative_pics.size() != rps.num_negative_pics) {
            return false;
        }

        if (rps.positive_pics.size() != rps.num_positive_pics) {
            return false;
        }

        if (rps.num_delta_pocs != rps.num_negative_pics + rps.num_positive_pics) {
            return false;
        }
    }

    return true;
}

}  // namespace bs