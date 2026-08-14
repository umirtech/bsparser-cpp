#pragma once

#include "hevc_nal_unit_header.hpp"
#include "hevc_parameter_set_manager.hpp"

#include "../syntax/hevc_common.hpp"
#include "../syntax/hevc_pps.hpp"
#include "../syntax/hevc_sps.hpp"
#include "../syntax/hevc_vps.hpp"

#include <cstddef>
#include <cstdint>

namespace bs {

/*
 * -----------------------------------------------------------
 * Slice parser context
 * -----------------------------------------------------------
 *
 * A slice header is interpreted in the context of:
 *
 *     VPS
 *       ^
 *       |
 *     SPS
 *       ^
 *       |
 *     PPS
 *       ^
 *       |
 *     Slice
 *
 * This structure contains the resolved parameter sets and
 * derived values needed while parsing a slice header.
 *
 * It does NOT own the parameter sets.
 */

/*
 * -----------------------------------------------------------
 * Resolved parameter-set references
 * -----------------------------------------------------------
 */

struct SliceParameterSets {
    const PictureParameterSet* pps = nullptr;

    const SequenceParameterSet* sps = nullptr;

    const VideoParameterSet* vps = nullptr;

    [[nodiscard]]
    constexpr bool has_pps() const noexcept {
        return pps != nullptr;
    }

    [[nodiscard]]
    constexpr bool has_sps() const noexcept {
        return sps != nullptr;
    }

    [[nodiscard]]
    constexpr bool has_vps() const noexcept {
        return vps != nullptr;
    }

    /*
     * PPS + SPS are the minimum parameter-set chain required
     * for interpreting a slice.
     */
    [[nodiscard]]
    constexpr bool valid() const noexcept {
        return pps != nullptr && sps != nullptr;
    }
};

/*
 * -----------------------------------------------------------
 * Slice parser context
 * -----------------------------------------------------------
 */

struct SliceParserContext {
    /*
     * Resolved parameter sets.
     */
    SliceParameterSets parameter_sets{};

    /*
     * -------------------------------------------------------
     * NAL context
     * -------------------------------------------------------
     */

    /*
     * nal_unit_type
     */
    NalUnitType nal_unit_type = NalUnitType::TRAIL_N;

    /*
     * nuh_layer_id
     */
    std::uint8_t nuh_layer_id = 0;

    /*
     * nuh_temporal_id_plus1
     */
    std::uint8_t nuh_temporal_id_plus1 = 1;

    /*
     * -------------------------------------------------------
     * Derived temporal information
     * -------------------------------------------------------
     */

    /*
     * Temporal ID:
     *
     *     TemporalId = nuh_temporal_id_plus1 - 1
     */
    [[nodiscard]]
    constexpr std::uint8_t temporal_id() const noexcept {
        return nuh_temporal_id_plus1 == 0 ? 0
                                          : static_cast<std::uint8_t>(nuh_temporal_id_plus1 - 1);
    }

    /*
     * -------------------------------------------------------
     * SPS-derived values
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr std::uint32_t pic_width() const noexcept {
        if (parameter_sets.sps == nullptr) {
            return 0;
        }

        return parameter_sets.sps->pic_width_in_luma_samples;
    }

    [[nodiscard]]
    constexpr std::uint32_t pic_height() const noexcept {
        if (parameter_sets.sps == nullptr) {
            return 0;
        }

        return parameter_sets.sps->pic_height_in_luma_samples;
    }

    [[nodiscard]]
    constexpr ChromaFormat chroma_format() const noexcept {
        if (parameter_sets.sps == nullptr) {
            return ChromaFormat::YUV420;
        }

        return parameter_sets.sps->chroma_format;
    }

    [[nodiscard]]
    constexpr std::uint8_t luma_bit_depth() const noexcept {
        if (parameter_sets.sps == nullptr) {
            return 8;
        }

        return parameter_sets.sps->luma_bit_depth();
    }

    [[nodiscard]]
    constexpr std::uint8_t chroma_bit_depth() const noexcept {
        if (parameter_sets.sps == nullptr) {
            return 8;
        }

        return parameter_sets.sps->chroma_bit_depth();
    }

    /*
     * -------------------------------------------------------
     * POC configuration
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr std::uint32_t max_pic_order_cnt_lsb() const noexcept {
        if (parameter_sets.sps == nullptr) {
            return 0;
        }

        return parameter_sets.sps->max_pic_order_cnt_lsb();
    }

    [[nodiscard]]
    constexpr unsigned pic_order_cnt_lsb_bits() const noexcept {
        if (parameter_sets.sps == nullptr) {
            return 0;
        }

        return static_cast<unsigned>(parameter_sets.sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
    }

    /*
     * -------------------------------------------------------
     * Slice defaults from PPS
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr std::uint32_t default_num_ref_idx_l0() const noexcept {
        if (parameter_sets.pps == nullptr) {
            return 0;
        }

        return parameter_sets.pps->num_ref_idx_l0_default_active();
    }

    [[nodiscard]]
    constexpr std::uint32_t default_num_ref_idx_l1() const noexcept {
        if (parameter_sets.pps == nullptr) {
            return 0;
        }

        return parameter_sets.pps->num_ref_idx_l1_default_active();
    }

    [[nodiscard]]
    constexpr std::int32_t initial_qp() const noexcept {
        if (parameter_sets.pps == nullptr) {
            return 26;
        }

        return parameter_sets.pps->initial_qp();
    }

    /*
     * -------------------------------------------------------
     * Slice-type helpers
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr bool is_intra_slice(SliceType type) const noexcept {
        return type == SliceType::I;
    }

    [[nodiscard]]
    constexpr bool is_inter_slice(SliceType type) const noexcept {
        return type == SliceType::P || type == SliceType::B;
    }

    [[nodiscard]]
    constexpr bool has_list1(SliceType type) const noexcept {
        return type == SliceType::B;
    }

    /*
     * -------------------------------------------------------
     * Coding tools
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr bool weighted_prediction_enabled(SliceType type) const noexcept {
        if (parameter_sets.pps == nullptr) {
            return false;
        }

        if (type == SliceType::P) {
            return parameter_sets.pps->weighted_pred_flag;
        }

        if (type == SliceType::B) {
            return parameter_sets.pps->weighted_bipred_flag;
        }

        return false;
    }

    [[nodiscard]]
    constexpr bool tiles_enabled() const noexcept {
        return parameter_sets.pps != nullptr && parameter_sets.pps->tiles.tiles_enabled_flag;
    }

    [[nodiscard]]
    constexpr bool entropy_coding_sync_enabled() const noexcept {
        return parameter_sets.pps != nullptr &&
               parameter_sets.pps->entropy_coding_sync_enabled_flag;
    }

    [[nodiscard]]
    constexpr bool deblocking_control_present() const noexcept {
        return parameter_sets.pps != nullptr &&
               parameter_sets.pps->deblocking.deblocking_filter_control_present_flag;
    }

    [[nodiscard]]
    constexpr bool dependent_slice_segments_enabled() const noexcept {
        return parameter_sets.pps != nullptr &&
               parameter_sets.pps->dependent_slice_segments_enabled_flag;
    }

    [[nodiscard]]
    constexpr bool output_flag_present() const noexcept {
        return parameter_sets.pps != nullptr && parameter_sets.pps->output_flag_present_flag;
    }

    /*
     * -------------------------------------------------------
     * Validation
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr bool valid() const noexcept {
        if (!parameter_sets.valid()) {
            return false;
        }

        if (nuh_temporal_id_plus1 == 0) {
            return false;
        }

        if (nuh_temporal_id_plus1 > 7) {
            return false;
        }

        return true;
    }
};

/*
 * -----------------------------------------------------------
 * Context construction
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline SliceParserContext make_slice_parser_context(
    const ParameterSetManager& manager,
    std::uint32_t pps_id,
    NalUnitType nal_unit_type,
    std::uint8_t nuh_layer_id,
    std::uint8_t nuh_temporal_id_plus1
) {
    SliceParserContext context{};

    const auto resolved = manager.resolve_pps(static_cast<std::uint8_t>(pps_id));

    context.parameter_sets.pps = resolved.pps;

    context.parameter_sets.sps = resolved.sps;

    context.parameter_sets.vps = resolved.vps;

    context.nal_unit_type = nal_unit_type;

    context.nuh_layer_id = nuh_layer_id;

    context.nuh_temporal_id_plus1 = nuh_temporal_id_plus1;

    return context;
}

/*
 * -----------------------------------------------------------
 * Context construction from an already parsed slice header
 * -----------------------------------------------------------
 *
 * Useful after the first PPS ID has been decoded.
 */

template <typename SliceHeader>
[[nodiscard]]
inline SliceParserContext make_slice_parser_context(
    const ParameterSetManager& manager,
    const SliceHeader& header,
    NalUnitType nal_unit_type,
    std::uint8_t nuh_layer_id,
    std::uint8_t nuh_temporal_id_plus1
) {
    return make_slice_parser_context(
        manager,
        static_cast<std::uint32_t>(header.slice_pic_parameter_set_id),
        nal_unit_type,
        nuh_layer_id,
        nuh_temporal_id_plus1
    );
}

/*
 * -----------------------------------------------------------
 * Re-resolve PPS/SPS/VPS
 * -----------------------------------------------------------
 *
 * Useful when the PPS ID has been parsed from the slice
 * header after the initial NAL header has already been read.
 */

inline bool resolve_slice_parameter_sets(
    const ParameterSetManager& manager, std::uint32_t pps_id, SliceParserContext& context
) noexcept {
    const auto resolved = manager.resolve_pps(static_cast<std::uint8_t>(pps_id));

    context.parameter_sets.pps = resolved.pps;

    context.parameter_sets.sps = resolved.sps;

    context.parameter_sets.vps = resolved.vps;

    return context.parameter_sets.valid();
}

}  // namespace bs