#pragma once

#include "avc_common.hpp"
#include "avc_scaling_list.hpp"
#include "avc_vui.hpp"

#include <array>
#include <cstdint>

namespace bs {
namespace avc {

/*
 * H.264 / AVC sequence parameter set (7.3.2.1.1).
 */
struct SequenceParameterSet {
    std::uint8_t profile_idc = 0;
    std::array<bool, 6> constraint_set_flag{};
    std::uint8_t level_idc = 0;
    std::uint8_t seq_parameter_set_id = 0;

    /*
     * High-profile syntax block (present when
     * is_high_profile(profile_idc)).
     */
    std::uint8_t chroma_format_idc = 1;  // default 4:2:0 for non-high profiles
    bool separate_colour_plane_flag = false;
    std::uint8_t bit_depth_luma_minus8 = 0;
    std::uint8_t bit_depth_chroma_minus8 = 0;
    bool qpprime_y_zero_transform_bypass_flag = false;
    ScalingList scaling_lists{};

    std::uint8_t log2_max_frame_num_minus4 = 0;
    std::uint8_t pic_order_cnt_type = 0;
    std::uint8_t log2_max_pic_order_cnt_lsb_minus4 = 0;

    bool delta_pic_order_always_zero_flag = false;
    std::int32_t offset_for_non_ref_pic = 0;
    std::int32_t offset_for_top_to_bottom_field = 0;
    std::uint8_t num_ref_frames_in_pic_order_cnt_cycle = 0;
    std::array<std::int32_t, 255> offset_for_ref_frame{};

    std::uint8_t max_num_ref_frames = 0;
    bool gaps_in_frame_num_value_allowed_flag = false;
    std::uint32_t pic_width_in_mbs_minus1 = 0;
    std::uint32_t pic_height_in_map_units_minus1 = 0;
    bool frame_mbs_only_flag = false;
    bool mb_adaptive_frame_field_flag = false;
    bool direct_8x8_inference_flag = false;

    bool frame_cropping_flag = false;
    std::uint32_t frame_crop_left_offset = 0;
    std::uint32_t frame_crop_right_offset = 0;
    std::uint32_t frame_crop_top_offset = 0;
    std::uint32_t frame_crop_bottom_offset = 0;

    bool vui_parameters_present_flag = false;
    VuiParameters vui{};

    /*
     * -------------------------------------------------------
     * Derived helpers
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr bool is_high() const noexcept {
        return is_high_profile(profile_idc);
    }

    [[nodiscard]]
    constexpr bool is_monochrome() const noexcept {
        return chroma_format_idc == static_cast<std::uint8_t>(ChromaFormat::Monochrome);
    }

    [[nodiscard]]
    constexpr std::uint32_t log2_max_frame_num() const noexcept {
        return static_cast<std::uint32_t>(log2_max_frame_num_minus4) + 4;
    }

    [[nodiscard]]
    constexpr std::uint32_t log2_max_pic_order_cnt_lsb() const noexcept {
        return static_cast<std::uint32_t>(log2_max_pic_order_cnt_lsb_minus4) + 4;
    }

    /*
     * Crop unit sizes (7.4.2.1.1).
     */
    [[nodiscard]]
    constexpr std::uint32_t crop_unit_x() const noexcept {
        if (chroma_format_idc == static_cast<std::uint8_t>(ChromaFormat::Monochrome) ||
            chroma_format_idc == static_cast<std::uint8_t>(ChromaFormat::Yuv444) ||
            separate_colour_plane_flag) {
            return 1;
        }

        return 2;
    }

    [[nodiscard]]
    constexpr std::uint32_t crop_unit_y() const noexcept {
        const std::uint32_t multiplier = frame_mbs_only_flag ? 1 : 2;

        if (chroma_format_idc == static_cast<std::uint8_t>(ChromaFormat::Monochrome) ||
            chroma_format_idc == static_cast<std::uint8_t>(ChromaFormat::Yuv444) ||
            separate_colour_plane_flag) {
            return multiplier;
        }

        if (chroma_format_idc == static_cast<std::uint8_t>(ChromaFormat::Yuv422)) {
            return multiplier;
        }

        // 4:2:0
        return multiplier * 2;
    }

    [[nodiscard]]
    constexpr std::uint32_t pic_width_in_luma_samples() const noexcept {
        const std::uint32_t crop_x = crop_unit_x();

        const std::uint32_t base = (pic_width_in_mbs_minus1 + 1) * 16;

        if (!frame_cropping_flag) {
            return base;
        }

        return base - crop_x * (frame_crop_left_offset + frame_crop_right_offset);
    }

    [[nodiscard]]
    constexpr std::uint32_t pic_height_in_luma_samples() const noexcept {
        const std::uint32_t multiplier = frame_mbs_only_flag ? 1 : 2;

        const std::uint32_t crop_y = crop_unit_y();

        const std::uint32_t base = (pic_height_in_map_units_minus1 + 1) * 16 * multiplier;

        if (!frame_cropping_flag) {
            return base;
        }

        return base - crop_y * (frame_crop_top_offset + frame_crop_bottom_offset);
    }
};

}  // namespace avc
}  // namespace bs