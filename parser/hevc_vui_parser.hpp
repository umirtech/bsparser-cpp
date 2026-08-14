#pragma once

#include "rbsp_bitstream_reader.hpp"
#include "hevc_vui.hpp"
#include "hevc_hrd.hpp"
#include "hevc_hrd_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace bs {

/*
 * H.265 vui_parameters()
 *
 * 7.3.2.3
 *
 * VUI syntax is associated with the SPS and describes
 * interpretation/display/timing information.
 *
 * The parser operates directly on RbspBitstreamReader.
 */

/*
 * -----------------------------------------------------------
 * Parse result
 * -----------------------------------------------------------
 */

struct VuiParseResult {
    bool ok = false;

    std::size_t bits_consumed = 0;
};

/*
 * -----------------------------------------------------------
 * Limits
 * -----------------------------------------------------------
 */

inline constexpr std::uint8_t kMaxVuiSubLayers = 8;

/*
 * -----------------------------------------------------------
 * Aspect ratio
 * -----------------------------------------------------------
 */

inline void parse_vui_aspect_ratio(RbspBitstreamReader& bs, VuiParameters& vui) {
    vui.aspect_ratio_info_present_flag = bs.read_bit();

    if (!vui.aspect_ratio_info_present_flag) {
        vui.aspect_ratio = {};
        vui.aspect_ratio.present = false;
        return;
    }

    vui.aspect_ratio.present = true;

    /*
     * aspect_ratio_idc
     *
     * u(8)
     */
    vui.aspect_ratio.aspect_ratio_idc = static_cast<std::uint8_t>(bs.read_bits(8));

    /*
     * 255 = Extended_SAR
     */
    if (vui.aspect_ratio.aspect_ratio_idc == 255) {
        /*
         * sar_width
         *
         * u(16)
         */
        vui.aspect_ratio.sar_width = static_cast<std::uint16_t>(bs.read_bits(16));

        /*
         * sar_height
         *
         * u(16)
         */
        vui.aspect_ratio.sar_height = static_cast<std::uint16_t>(bs.read_bits(16));

    } else {
        /*
         * For predefined aspect ratios, retain the
         * syntax idc and populate the semantic SAR.
         */
        const auto sar = aspect_ratio_from_idc(vui.aspect_ratio.aspect_ratio_idc);

        vui.aspect_ratio.sar_width = sar.width;

        vui.aspect_ratio.sar_height = sar.height;
    }
}

/*
 * -----------------------------------------------------------
 * Overscan
 * -----------------------------------------------------------
 */

inline void parse_vui_overscan(RbspBitstreamReader& bs, VuiParameters& vui) {
    vui.overscan_info_present_flag = bs.read_bit();

    if (!vui.overscan_info_present_flag) {
        vui.overscan_appropriate_flag = false;
        return;
    }

    vui.overscan_appropriate_flag = bs.read_bit();
}

/*
 * -----------------------------------------------------------
 * Video signal type
 * -----------------------------------------------------------
 */

inline void parse_vui_video_signal_type(RbspBitstreamReader& bs, VuiParameters& vui) {
    /*
     * video_signal_type_present_flag
     *
     * This is represented by VideoSignalType::present.
     */
    vui.video_signal.present = bs.read_bit();

    if (!vui.video_signal.present) {
        vui.video_signal = {};
        return;
    }

    /*
     * video_format
     *
     * u(3)
     */
    vui.video_signal.video_format = static_cast<std::uint8_t>(bs.read_bits(3));

    /*
     * video_full_range_flag
     *
     * u(1)
     */
    vui.video_signal.video_full_range_flag = bs.read_bit();

    /*
     * colour_description_present_flag
     */
    vui.video_signal.colour.present = bs.read_bit();

    if (!vui.video_signal.colour.present) {
        return;
    }

    /*
     * colour_primaries
     *
     * u(8)
     */
    vui.video_signal.colour.colour_primaries = static_cast<std::uint8_t>(bs.read_bits(8));

    /*
     * transfer_characteristics
     *
     * u(8)
     */
    vui.video_signal.colour.transfer_characteristics = static_cast<std::uint8_t>(bs.read_bits(8));

    /*
     * matrix_coefficients
     *
     * u(8)
     */
    vui.video_signal.colour.matrix_coefficients = static_cast<std::uint8_t>(bs.read_bits(8));
}

/*
 * -----------------------------------------------------------
 * Chroma sample location
 * -----------------------------------------------------------
 */

inline void parse_vui_chroma_location(RbspBitstreamReader& bs, VuiParameters& vui) {
    vui.chroma_location.present = bs.read_bit();

    if (!vui.chroma_location.present) {
        vui.chroma_location = {};
        return;
    }

    /*
     * chroma_sample_loc_type_top_field
     *
     * ue(v)
     */
    vui.chroma_location.chroma_sample_loc_type_top_field = bs.read_ue();

    /*
     * chroma_sample_loc_type_bottom_field
     *
     * ue(v)
     */
    vui.chroma_location.chroma_sample_loc_type_bottom_field = bs.read_ue();
}

/*
 * -----------------------------------------------------------
 * Default display window
 * -----------------------------------------------------------
 */

inline void parse_vui_default_display_window(RbspBitstreamReader& bs, VuiParameters& vui) {
    vui.default_display_window_flag = bs.read_bit();

    if (!vui.default_display_window_flag) {
        vui.default_display_window = {};
        return;
    }

    /*
     * def_disp_win_left_offset
     */
    vui.default_display_window.left_offset = bs.read_ue();

    /*
     * def_disp_win_right_offset
     */
    vui.default_display_window.right_offset = bs.read_ue();

    /*
     * def_disp_win_top_offset
     */
    vui.default_display_window.top_offset = bs.read_ue();

    /*
     * def_disp_win_bottom_offset
     */
    vui.default_display_window.bottom_offset = bs.read_ue();
}

/*
 * -----------------------------------------------------------
 * Timing information
 * -----------------------------------------------------------
 */

inline void parse_vui_timing(
    RbspBitstreamReader& bs, VuiParameters& vui, std::uint8_t max_num_sub_layers_minus1
) {
    vui.vui_timing_info_present_flag = bs.read_bit();

    if (!vui.vui_timing_info_present_flag) {
        vui.timing = {};
        vui.hrd_parameters_present_flag = false;
        vui.hrd = {};
        return;
    }

    /*
     * num_units_in_tick
     *
     * u(32)
     */
    vui.timing.num_units_in_tick = bs.read_u32(32);

    /*
     * time_scale
     *
     * u(32)
     */
    vui.timing.time_scale = bs.read_u32(32);

    /*
     * poc_proportional_to_timing_flag
     */
    vui.timing.poc_proportional_to_timing_flag = bs.read_bit();

    if (vui.timing.poc_proportional_to_timing_flag) {
        /*
         * num_ticks_poc_diff_one_minus1
         *
         * ue(v)
         */
        vui.timing.num_ticks_poc_diff_one_minus1 = bs.read_ue();

    } else {
        vui.timing.num_ticks_poc_diff_one_minus1 = 0;
    }

    /*
     * hrd_parameters_present_flag
     */
    vui.hrd_parameters_present_flag = bs.read_bit();

    if (!vui.hrd_parameters_present_flag) {
        vui.hrd = {};
        return;
    }

    /*
     * In VUI, HRD is invoked with:
     *
     *     commonInfPresentFlag = 1
     */
    parse_hrd_parameters(bs, true, max_num_sub_layers_minus1, vui.hrd);
}

/*
 * -----------------------------------------------------------
 * Bitstream restriction
 * -----------------------------------------------------------
 */

inline void parse_vui_bitstream_restriction(RbspBitstreamReader& bs, VuiParameters& vui) {
    vui.bitstream_restriction_flag = bs.read_bit();

    if (!vui.bitstream_restriction_flag) {
        vui.tiles_fixed_structure_flag = false;

        vui.motion_vectors_over_pic_boundaries_flag = false;

        vui.restricted_ref_pic_lists_flag = false;

        vui.min_spatial_segmentation_idc = 0;

        vui.max_bytes_per_pic_denom = 0;

        vui.max_bits_per_min_cu_denom = 0;

        vui.log2_max_mv_length_horizontal = 0;

        vui.log2_max_mv_length_vertical = 0;

        return;
    }

    /*
     * tiles_fixed_structure_flag
     */
    vui.tiles_fixed_structure_flag = bs.read_bit();

    /*
     * motion_vectors_over_pic_boundaries_flag
     */
    vui.motion_vectors_over_pic_boundaries_flag = bs.read_bit();

    /*
     * restricted_ref_pic_lists_flag
     */
    vui.restricted_ref_pic_lists_flag = bs.read_bit();

    /*
     * min_spatial_segmentation_idc
     *
     * ue(v)
     */
    vui.min_spatial_segmentation_idc = bs.read_ue();

    /*
     * max_bytes_per_pic_denom
     *
     * ue(v)
     */
    vui.max_bytes_per_pic_denom = bs.read_ue();

    /*
     * max_bits_per_min_cu_denom
     *
     * ue(v)
     */
    vui.max_bits_per_min_cu_denom = bs.read_ue();

    /*
     * log2_max_mv_length_horizontal
     *
     * ue(v)
     */
    vui.log2_max_mv_length_horizontal = bs.read_ue();

    /*
     * log2_max_mv_length_vertical
     *
     * ue(v)
     */
    vui.log2_max_mv_length_vertical = bs.read_ue();
}

/*
 * -----------------------------------------------------------
 * Main VUI parser
 * -----------------------------------------------------------
 */

inline VuiParseResult parse_vui_parameters(
    RbspBitstreamReader& bs, std::uint8_t max_num_sub_layers_minus1, VuiParameters& vui
) {
    if (max_num_sub_layers_minus1 >= kMaxVuiSubLayers) {
        throw std::invalid_argument("VUI: MaxNumSubLayersMinus1 > 7");
    }

    const auto start = bs.bit_position();

    /*
     * Reset destination.
     */
    vui = {};

    /*
     * -------------------------------------------------------
     * aspect_ratio_info_present_flag
     * -------------------------------------------------------
     */
    parse_vui_aspect_ratio(bs, vui);

    /*
     * -------------------------------------------------------
     * overscan
     * -------------------------------------------------------
     */
    parse_vui_overscan(bs, vui);

    /*
     * -------------------------------------------------------
     * video signal type
     * -------------------------------------------------------
     */
    parse_vui_video_signal_type(bs, vui);

    /*
     * -------------------------------------------------------
     * chroma location
     * -------------------------------------------------------
     */
    parse_vui_chroma_location(bs, vui);

    /*
     * -------------------------------------------------------
     * neutral_chroma_indication_flag
     * -------------------------------------------------------
     */
    vui.neutral_chroma_indication_flag = bs.read_bit();

    /*
     * -------------------------------------------------------
     * field_seq_flag
     * -------------------------------------------------------
     */
    vui.field_seq_flag = bs.read_bit();

    /*
     * -------------------------------------------------------
     * frame_field_info_present_flag
     * -------------------------------------------------------
     */
    vui.frame_field_info_present_flag = bs.read_bit();

    /*
     * -------------------------------------------------------
     * default display window
     * -------------------------------------------------------
     */
    parse_vui_default_display_window(bs, vui);

    /*
     * -------------------------------------------------------
     * timing + HRD
     * -------------------------------------------------------
     */
    parse_vui_timing(bs, vui, max_num_sub_layers_minus1);

    /*
     * -------------------------------------------------------
     * bitstream restriction
     * -------------------------------------------------------
     */
    parse_vui_bitstream_restriction(bs, vui);

    return {true, bs.bit_position() - start};
}

/*
 * -----------------------------------------------------------
 * Convenience overload
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline VuiParameters parse_vui_parameters(
    RbspBitstreamReader& bs, std::uint8_t max_num_sub_layers_minus1
) {
    VuiParameters vui{};

    parse_vui_parameters(bs, max_num_sub_layers_minus1, vui);

    return vui;
}

/*
 * -----------------------------------------------------------
 * Validation
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline bool validate_vui_parameters(const VuiParameters& vui) noexcept {
    /*
     * Extended SAR requires both values.
     */
    if (vui.aspect_ratio_info_present_flag && vui.aspect_ratio.aspect_ratio_idc == 255) {
        if (vui.aspect_ratio.sar_width == 0 || vui.aspect_ratio.sar_height == 0) {
            return false;
        }
    }

    /*
     * Timing information must contain non-zero values.
     */
    if (vui.vui_timing_info_present_flag) {
        if (!vui.timing.valid()) {
            return false;
        }
    }

    /*
     * HRD requires timing information in this VUI syntax
     * path.
     */
    if (vui.hrd_parameters_present_flag && !vui.vui_timing_info_present_flag) {
        return false;
    }

    return true;
}

/*
 * -----------------------------------------------------------
 * Semantic helpers
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool vui_has_extended_sar(const VuiParameters& vui) noexcept {
    return vui.aspect_ratio_info_present_flag && vui.aspect_ratio.aspect_ratio_idc == 255;
}

[[nodiscard]]
constexpr bool vui_has_bitstream_restrictions(const VuiParameters& vui) noexcept {
    return vui.bitstream_restriction_flag;
}

[[nodiscard]]
constexpr bool vui_has_sub_picture_hrd(const VuiParameters& vui) noexcept {
    return vui.hrd_parameters_present_flag && vui.hrd.common.sub_pic_hrd_params_present_flag;
}

}  // namespace bs