#pragma once

#include "hevc_common.hpp"
#include "hevc_short_term_ref_pic_set.hpp"
#include "hevc_nal_unit.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bs {

/*
 * -----------------------------------------------------------
 * Reference picture list modification
 * -----------------------------------------------------------
 *
 * ref_pic_list_modification()
 */

/*
 * List modification information for one reference list.
 */
struct RefPicListModification {
    /*
     * ref_pic_list_modification_flag_l0/l1
     */
    bool modification_flag = false;

    /*
     * list_entry_l0[] / list_entry_l1[]
     *
     * The number of entries is determined by:
     *
     *     NumPocTotalCurr
     */
    std::vector<std::uint32_t> list_entry;

    [[nodiscard]]
    std::size_t size() const noexcept {
        return list_entry.size();
    }

    [[nodiscard]]
    bool empty() const noexcept {
        return list_entry.empty();
    }
};

/*
 * Complete reference picture list modification.
 */
struct RefPicListModificationData {
    RefPicListModification list0{};
    RefPicListModification list1{};

    [[nodiscard]]
    constexpr bool modifies_l0() const noexcept {
        return list0.modification_flag;
    }

    [[nodiscard]]
    constexpr bool modifies_l1() const noexcept {
        return list1.modification_flag;
    }
};

/*
 * -----------------------------------------------------------
 * Long-term reference picture syntax
 * -----------------------------------------------------------
 */

struct SliceLongTermReference {
    /*
     * lt_idx_sps
     *
     * Present when the long-term reference comes from
     * the SPS long-term reference picture list.
     */
    std::uint32_t lt_idx_sps = 0;

    /*
     * poc_lsb_lt
     */
    std::uint32_t poc_lsb_lt = 0;

    /*
     * used_by_curr_pic_lt_flag
     */
    bool used_by_curr_pic_lt_flag = false;

    /*
     * delta_poc_msb_present_flag
     */
    bool delta_poc_msb_present_flag = false;

    /*
     * delta_poc_msb_cycle_lt
     */
    std::uint32_t delta_poc_msb_cycle_lt = 0;
};

/*
 * -----------------------------------------------------------
 * Prediction weight table
 * -----------------------------------------------------------
 *
 * pred_weight_table()
 */

/*
 * Weight information for one reference picture.
 */
struct PredictionWeight {
    /*
     * luma_log2_weight_denom is shared by the complete
     * prediction-weight table.
     *
     * These fields describe one list entry.
     */

    bool luma_weight_luma_flag = false;

    std::int32_t delta_luma_weight = 0;
    std::int32_t luma_offset = 0;

    bool chroma_weight_flag = false;

    std::array<std::int32_t, 2> delta_chroma_weight{};

    std::array<std::int32_t, 2> delta_chroma_offset{};
};

/*
 * Complete prediction weight table.
 */
struct PredictionWeightTable {
    /*
     * luma_log2_weight_denom
     */
    std::uint32_t luma_log2_weight_denom = 0;

    /*
     * delta_chroma_log2_weight_denom
     */
    std::int32_t delta_chroma_log2_weight_denom = 0;

    /*
     * One entry per active reference.
     */
    std::vector<PredictionWeight> l0;

    std::vector<PredictionWeight> l1;

    [[nodiscard]]
    std::uint32_t chroma_log2_weight_denom() const noexcept {
        const auto value =
            static_cast<std::int32_t>(luma_log2_weight_denom) + delta_chroma_log2_weight_denom;

        return value < 0 ? 0 : static_cast<std::uint32_t>(value);
    }
};

/*
 * -----------------------------------------------------------
 * Slice deblocking configuration
 * -----------------------------------------------------------
 */

struct SliceDeblockingFilter {
    /*
     * deblocking_filter_override_flag
     */
    bool override_flag = false;

    /*
     * slice_deblocking_filter_disabled_flag
     */
    bool disabled_flag = false;

    /*
     * slice_beta_offset_div2
     */
    std::int32_t beta_offset_div2 = 0;

    /*
     * slice_tc_offset_div2
     */
    std::int32_t tc_offset_div2 = 0;
};

/*
 * -----------------------------------------------------------
 * Entry-point offsets
 * -----------------------------------------------------------
 *
 * entry_point_offsets()
 */

struct SliceEntryPointOffsets {
    /*
     * num_entry_point_offsets
     */
    std::uint32_t num_entry_point_offsets = 0;

    /*
     * offset_len_minus1
     */
    std::uint32_t offset_len_minus1 = 0;

    /*
     * entry_point_offset_minus1[]
     */
    std::vector<std::uint32_t> entry_point_offset_minus1;

    [[nodiscard]]
    unsigned offset_bits() const noexcept {
        return static_cast<unsigned>(offset_len_minus1 + 1);
    }

    [[nodiscard]]
    std::size_t count() const noexcept {
        return entry_point_offset_minus1.size();
    }
};

/*
 * -----------------------------------------------------------
 * Slice header extension
 * -----------------------------------------------------------
 */

struct SliceHeaderExtension {
    /*
     * slice_header_extension_length
     */
    std::uint32_t length = 0;

    /*
     * slice_header_extension_data_byte[]
     */
    std::vector<std::uint8_t> data;
};

/*
 * -----------------------------------------------------------
 * Slice reference picture information
 * -----------------------------------------------------------
 */

struct SliceReferencePictureInfo {
    /*
     * num_ref_idx_active_override_flag
     */
    bool num_ref_idx_active_override_flag = false;

    /*
     * num_ref_idx_l0_active_minus1
     */
    std::uint32_t num_ref_idx_l0_active_minus1 = 0;

    /*
     * num_ref_idx_l1_active_minus1
     */
    std::uint32_t num_ref_idx_l1_active_minus1 = 0;

    /*
     * ref_pic_list_modification()
     */
    RefPicListModificationData list_modification{};

    /*
     * Number of short/long-term references used by the
     * current picture.
     *
     * This is derived by the parser from the active RPS.
     */
    std::uint32_t num_poc_total_curr = 0;
};

/*
 * -----------------------------------------------------------
 * Slice segment address
 * -----------------------------------------------------------
 */

struct SliceSegmentAddress {
    /*
     * slice_segment_address
     *
     * Width depends on PicSizeInCtbsY.
     */
    std::uint32_t value = 0;

    /*
     * Number of bits used by the syntax.
     */
    std::uint32_t bit_width = 0;

    [[nodiscard]]
    bool valid() const noexcept {
        return bit_width <= 32;
    }
};

/*
 * -----------------------------------------------------------
 * Complete slice segment header
 * -----------------------------------------------------------
 */

struct SliceSegmentHeader {
    /*
     * =======================================================
     * NAL context
     * =======================================================
     */

    /*
     * nal_unit_type is supplied by the NAL parser.
     *
     * It isn't encoded inside the slice segment header.
     */
    std::uint8_t nal_unit_type = 0;

    /*
     * temporal_id
     *
     * Also supplied by the NAL header.
     */
    std::uint8_t temporal_id = 0;

    /*
     * =======================================================
     * First/dependent segment
     * =======================================================
     */

    /*
     * first_slice_segment_in_pic_flag
     */
    bool first_slice_segment_in_pic_flag = false;

    /*
     * no_output_of_prior_pics_flag
     *
     * Present for IRAP pictures.
     */
    bool no_output_of_prior_pics_flag = false;

    /*
     * slice_pic_parameter_set_id
     */
    std::uint32_t slice_pic_parameter_set_id = 0;

    /*
     * dependent_slice_segment_flag
     */
    bool dependent_slice_segment_flag = false;

    /*
     * slice_segment_address
     */
    SliceSegmentAddress slice_segment_address{};

    /*
     * =======================================================
     * Reserved slice header bits
     * =======================================================
     */

    /*
     * slice_reserved_flag[]
     *
     * Number is determined by:
     *
     *     num_extra_slice_header_bits
     */
    std::vector<bool> slice_reserved_flag;

    /*
     * =======================================================
     * Slice type / output
     * =======================================================
     */

    SliceType slice_type = SliceType::P;

    /*
     * pic_output_flag
     */
    bool pic_output_flag = true;

    /*
     * colour_plane_id
     */
    std::uint8_t colour_plane_id = 0;

    /*
     * =======================================================
     * Picture order count
     * =======================================================
     */

    /*
     * slice_pic_order_cnt_lsb
     */
    std::uint32_t slice_pic_order_cnt_lsb = 0;

    /*
     * short_term_ref_pic_set_sps_flag
     */
    bool short_term_ref_pic_set_sps_flag = true;

    /*
     * Index into the SPS short-term RPS list.
     */
    std::uint32_t short_term_ref_pic_set_idx = 0;

    /*
     * Slice-level additional/inter-predicted RPS.
     *
     * Used when:
     *
     *     short_term_ref_pic_set_sps_flag == 0
     */
    ShortTermRefPicSet short_term_ref_pic_set{};

    /*
     * =======================================================
     * Long-term references
     * =======================================================
     */

    /*
     * num_long_term_sps
     */
    std::uint32_t num_long_term_sps = 0;

    /*
     * num_long_term_pics
     */
    std::uint32_t num_long_term_pics = 0;

    std::vector<SliceLongTermReference> long_term_references;

    /*
     * =======================================================
     * Temporal motion vector prediction
     * =======================================================
     */

    /*
     * slice_temporal_mvp_enabled_flag
     */
    bool slice_temporal_mvp_enabled_flag = false;

    bool slice_sao_luma_flag = false;

    bool slice_sao_chroma_flag = false;

    /*
     * collocated_from_l0_flag
     */
    bool collocated_from_l0_flag = true;

    /*
     * collocated_ref_idx
     */
    std::uint32_t collocated_ref_idx = 0;

    /*
     * =======================================================
     * Reference picture lists
     * =======================================================
     */

    SliceReferencePictureInfo reference_pictures{};

    /*
     * =======================================================
     * Motion-vector difference / merge
     * =======================================================
     */

    /*
     * mvd_l1_zero_flag
     */
    bool mvd_l1_zero_flag = false;

    /*
     * cabac_init_flag
     */
    bool cabac_init_flag = false;

    /*
     * five_minus_max_num_merge_cand
     */
    std::uint32_t five_minus_max_num_merge_cand = 0;

    /*
     * =======================================================
     * Prediction weights
     * =======================================================
     */

    bool prediction_weight_table_present = false;

    PredictionWeightTable prediction_weight_table{};

    /*
     * =======================================================
     * QP / chroma QP
     * =======================================================
     */

    /*
     * slice_qp_delta
     */
    std::int32_t slice_qp_delta = 0;

    /*
     * slice_cb_qp_offset
     */
    std::int32_t slice_cb_qp_offset = 0;

    /*
     * slice_cr_qp_offset
     */
    std::int32_t slice_cr_qp_offset = 0;

    /*
     * =======================================================
     * Deblocking
     * =======================================================
     */

    SliceDeblockingFilter deblocking{};

    /*
     * =======================================================
     * Loop filtering
     * =======================================================
     */

    /*
     * slice_loop_filter_across_slices_enabled_flag
     */
    bool slice_loop_filter_across_slices_enabled_flag = false;

    /*
     * =======================================================
     * Entry points
     * =======================================================
     */

    SliceEntryPointOffsets entry_points{};

    /*
     * =======================================================
     * Header extension
     * =======================================================
     */

    SliceHeaderExtension extension{};

    /*
     * =======================================================
     * Derived values
     * =======================================================
     */

    /*
     * POC of the current picture.
     *
     * The actual value requires SPS state and previous
     * picture state, so the parser/decoder populates this.
     */
    std::int32_t derived_poc = 0;

    /*
     * Effective number of active L0 references.
     */
    std::uint32_t effective_num_ref_idx_l0 = 1;

    /*
     * Effective number of active L1 references.
     */
    std::uint32_t effective_num_ref_idx_l1 = 1;

    /*
     * =======================================================
     * Helpers
     * =======================================================
     */

    [[nodiscard]]
    constexpr bool is_first_slice() const noexcept {
        return first_slice_segment_in_pic_flag;
    }

    [[nodiscard]]
    constexpr bool is_dependent_slice() const noexcept {
        return dependent_slice_segment_flag;
    }

    [[nodiscard]]
    constexpr bool is_independent_slice() const noexcept {
        return !dependent_slice_segment_flag;
    }

    [[nodiscard]]
    constexpr bool is_irap() const noexcept {
        return is_irap_nal_unit(nal_unit_type);
    }

    [[nodiscard]]
    constexpr bool is_idr() const noexcept {
        return is_idr_nal_unit(nal_unit_type);
    }

    [[nodiscard]]
    constexpr bool is_intra() const noexcept {
        return slice_type == SliceType::I;
    }

    [[nodiscard]]
    constexpr bool is_predictive() const noexcept {
        return slice_type == SliceType::P;
    }

    [[nodiscard]]
    constexpr bool is_b_slice() const noexcept {
        return slice_type == SliceType::B;
    }

    [[nodiscard]]
    constexpr std::uint32_t active_l0_count() const noexcept {
        return effective_num_ref_idx_l0;
    }

    [[nodiscard]]
    constexpr std::uint32_t active_l1_count() const noexcept {
        return effective_num_ref_idx_l1;
    }

    [[nodiscard]]
    constexpr std::int32_t effective_qp(std::int32_t pps_base_qp) const noexcept {
        return pps_base_qp + slice_qp_delta;
    }

    [[nodiscard]]
    constexpr std::int32_t effective_cb_qp_offset(std::int32_t pps_offset) const noexcept {
        return pps_offset + slice_cb_qp_offset;
    }

    [[nodiscard]]
    constexpr std::int32_t effective_cr_qp_offset(std::int32_t pps_offset) const noexcept {
        return pps_offset + slice_cr_qp_offset;
    }

    [[nodiscard]]
    constexpr std::uint32_t max_merge_candidates() const noexcept {
        if (five_minus_max_num_merge_cand > 4) {
            return 0;
        }

        return 5 - five_minus_max_num_merge_cand;
    }

    [[nodiscard]]
    bool valid() const noexcept {
        if (slice_pic_parameter_set_id > 63) {
            return false;
        }

        if (static_cast<std::uint8_t>(slice_type) > 2) {
            return false;
        }

        if (is_dependent_slice() && first_slice_segment_in_pic_flag) {
            return false;
        }

        return true;
    }
};

/*
 * -----------------------------------------------------------
 * Initialization
 * -----------------------------------------------------------
 */

inline void initialize_slice_segment_header(SliceSegmentHeader& header) {
    header = {};

    header.pic_output_flag = true;

    header.collocated_from_l0_flag = true;

    header.effective_num_ref_idx_l0 = 1;
    header.effective_num_ref_idx_l1 = 1;

    header.slice_type = SliceType::P;
}

/*
 * -----------------------------------------------------------
 * Reference-count helpers
 * -----------------------------------------------------------
 */

inline void derive_slice_reference_counts(
    SliceSegmentHeader& header, std::uint32_t pps_l0_default, std::uint32_t pps_l1_default
) {
    if (header.reference_pictures.num_ref_idx_active_override_flag) {
        header.effective_num_ref_idx_l0 =
            header.reference_pictures.num_ref_idx_l0_active_minus1 + 1;

        header.effective_num_ref_idx_l1 =
            header.reference_pictures.num_ref_idx_l1_active_minus1 + 1;

    } else {
        header.effective_num_ref_idx_l0 = pps_l0_default;

        header.effective_num_ref_idx_l1 = pps_l1_default;
    }

    /*
     * I slices don't use reference lists.
     */
    if (header.slice_type == SliceType::I) {
        header.effective_num_ref_idx_l0 = 0;
        header.effective_num_ref_idx_l1 = 0;
    }

    /*
     * P slices only use L0.
     */
    if (header.slice_type == SliceType::P) {
        header.effective_num_ref_idx_l1 = 0;
    }
}

/*
 * -----------------------------------------------------------
 * Long-term reference count
 * -----------------------------------------------------------
 */

inline void initialize_slice_long_term_references(SliceSegmentHeader& header) {
    const auto count = static_cast<std::size_t>(header.num_long_term_sps) +
                       static_cast<std::size_t>(header.num_long_term_pics);

    header.long_term_references.clear();

    header.long_term_references.resize(count);
}

/*
 * -----------------------------------------------------------
 * Validation helpers
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool validate_slice_type(SliceType type) noexcept {
    return static_cast<std::uint8_t>(type) <= 2;
}

[[nodiscard]]
inline bool validate_slice_segment_header(const SliceSegmentHeader& header) noexcept {
    if (!validate_slice_type(header.slice_type)) {
        return false;
    }

    if (header.slice_pic_parameter_set_id > 63) {
        return false;
    }

    if (header.first_slice_segment_in_pic_flag && header.dependent_slice_segment_flag) {
        return false;
    }

    if (header.five_minus_max_num_merge_cand > 5) {
        return false;
    }

    if (header.entry_points.num_entry_point_offsets !=
        header.entry_points.entry_point_offset_minus1.size()) {
        return false;
    }

    if (header.num_long_term_sps + header.num_long_term_pics !=
        header.long_term_references.size()) {
        return false;
    }

    return true;
}

/*
 * -----------------------------------------------------------
 * Semantic helpers
 * -----------------------------------------------------------
 */

/*
 * Return whether reference-list modification is active.
 */
[[nodiscard]]
constexpr bool reference_list_modification_enabled(const SliceSegmentHeader& header) noexcept {
    return header.reference_pictures.list_modification.modifies_l0() ||
           header.reference_pictures.list_modification.modifies_l1();
}

/*
 * Return whether weighted prediction is signaled for this
 * slice.
 */
[[nodiscard]]
constexpr bool weighted_prediction_present(const SliceSegmentHeader& header) noexcept {
    return header.prediction_weight_table_present;
}

/*
 * Return the number of long-term references.
 *
 * Not constexpr: std::vector::size() is only constexpr in newer standard
 * libraries (libstdc++ >= 12, MSVC STL), which would break builds with
 * older toolchains (e.g. the musl-cross GCC 11).
 */
[[nodiscard]]
inline std::size_t long_term_reference_count(const SliceSegmentHeader& header) noexcept {
    return header.long_term_references.size();
}

/*
 * Return whether entry-point offsets are present.
 */
[[nodiscard]]
constexpr bool has_entry_points(const SliceSegmentHeader& header) noexcept {
    return header.entry_points.num_entry_point_offsets != 0;
}

/*
 * Return whether the slice header has an extension.
 */
[[nodiscard]]
constexpr bool has_slice_header_extension(const SliceSegmentHeader& header) noexcept {
    return header.extension.length != 0;
}

}  // namespace bs