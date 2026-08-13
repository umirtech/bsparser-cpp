#pragma once

#include "hevc_common.hpp"
#include "hevc_hrd.hpp"

#include <cstdint>

namespace bs {

/*
 * H.265 vui_parameters()
 *
 * The VUI is optional SPS metadata describing how the coded
 * pictures should be interpreted/displayed.
 *
 * This structure intentionally represents decoded syntax
 * values, rather than the bitstream encoding itself.
 */
struct VuiParameters {

    /*
     * -------------------------------------------------------
     * Source / coding characteristics
     * -------------------------------------------------------
     */

    bool progressive_source_flag = false;
    bool interlaced_source_flag = false;

    /*
     * Indicates that the source is non-packed.
     */
    bool non_packed_constraint_flag = false;

    /*
     * Indicates that the source is not projected.
     *
     * Kept separately because newer H.265 revisions and
     * profiles may make use of this information.
     */
    bool non_projected_constraint_flag = false;


    /*
     * -------------------------------------------------------
     * Aspect ratio
     * -------------------------------------------------------
     */

    bool aspect_ratio_info_present_flag = false;

    /*
     * aspect_ratio_idc == 255 means Extended_SAR.
     */
    AspectRatio aspect_ratio{};


    /*
     * -------------------------------------------------------
     * Overscan
     * -------------------------------------------------------
     */

    bool overscan_info_present_flag = false;

    bool overscan_appropriate_flag = false;


    /*
     * -------------------------------------------------------
     * Video signal
     * -------------------------------------------------------
     */

    VideoSignalType video_signal{};


    /*
     * -------------------------------------------------------
     * Chroma sample location
     * -------------------------------------------------------
     */

    ChromaLocationInfo chroma_location{};


    /*
     * -------------------------------------------------------
     * Picture interpretation
     * -------------------------------------------------------
     */

    bool neutral_chroma_indication_flag = false;

    bool field_seq_flag = false;

    bool frame_field_info_present_flag = false;


    /*
     * -------------------------------------------------------
     * Default display window
     * -------------------------------------------------------
     */

    bool default_display_window_flag = false;

    Window default_display_window{};


    /*
     * -------------------------------------------------------
     * Timing
     * -------------------------------------------------------
     */

    bool vui_timing_info_present_flag = false;

    /*
     * H.265 VUI timing.
     *
     * Kept separate from HRD timing because these are
     * conceptually VUI timing parameters.
     */
    TimingInfo timing{};


    /*
     * -------------------------------------------------------
     * HRD
     * -------------------------------------------------------
     */

    bool hrd_parameters_present_flag = false;

    HrdParameters hrd{};




    /*
    * -------------------------------------------------------
    * Bitstream restriction
    * -------------------------------------------------------
    */

    bool bitstream_restriction_flag = false;

    bool tiles_fixed_structure_flag = false;

    bool motion_vectors_over_pic_boundaries_flag = false;

    bool restricted_ref_pic_lists_flag = false;

    std::uint32_t min_spatial_segmentation_idc = 0;

    std::uint32_t max_bytes_per_pic_denom = 0;

    std::uint32_t max_bits_per_min_cu_denom = 0;

    std::uint32_t log2_max_mv_length_horizontal = 0;

    std::uint32_t log2_max_mv_length_vertical = 0;



    /*
     * -------------------------------------------------------
     * Helpers
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr bool has_aspect_ratio() const noexcept
    {
        return aspect_ratio_info_present_flag;
    }

    [[nodiscard]]
    constexpr bool has_video_signal() const noexcept
    {
        return video_signal.present;
    }

    [[nodiscard]]
    constexpr bool has_chroma_location() const noexcept
    {
        return chroma_location.present;
    }

    [[nodiscard]]
    constexpr bool has_default_display_window() const noexcept
    {
        return default_display_window_flag;
    }

    [[nodiscard]]
    constexpr bool has_timing() const noexcept
    {
        return vui_timing_info_present_flag;
    }

    [[nodiscard]]
    constexpr bool has_hrd() const noexcept
    {
        return hrd_parameters_present_flag;
    }
};


/*
 * -----------------------------------------------------------
 * Aspect ratio helpers
 * -----------------------------------------------------------
 *
 * H.265 aspect_ratio_idc values 1..16 have predefined
 * sample aspect ratios.
 *
 * 255 = Extended_SAR.
 */

struct SampleAspectRatioTableEntry {
    std::uint8_t idc;
    std::uint16_t width;
    std::uint16_t height;
};


/*
 * ISO/IEC / ITU-T predefined aspect ratios.
 *
 * These are useful for consumers of parsed SPS metadata.
 */
inline constexpr SampleAspectRatioTableEntry
    kAspectRatioTable[] = {
        { 1,   1,   1   }, // 1:1
        { 2,  12,  11  }, // 12:11
        { 3,  10,  11  }, // 10:11
        { 4,  16,  11  }, // 16:11
        { 5,  40,  33  }, // 40:33
        { 6,  24,  11  }, // 24:11
        { 7,  20,  11  }, // 20:11
        { 8,  32,  11  }, // 32:11
        { 9,  80,  33  }, // 80:33
        { 10,  18,  11  }, // 18:11
        { 11,  15,  11  }, // 15:11
        { 12,  64,  33  }, // 64:33
        { 13,  160, 99  }, // 160:99
        { 14,  4,   3   }, // 4:3
        { 15,  3,   2   }, // 3:2
        { 16,  2,   1   }  // 2:1
    };


[[nodiscard]]
constexpr SampleAspectRatio
aspect_ratio_from_idc(
    std::uint8_t idc) noexcept
{
    for (const auto& entry : kAspectRatioTable) {
        if (entry.idc == idc) {
            return {
                entry.width,
                entry.height
            };
        }
    }

    /*
     * Unknown / reserved aspect_ratio_idc.
     *
     * Return an invalid SAR rather than inventing a ratio.
     */
    return {
        0,
        0
    };
}


/*
 * -----------------------------------------------------------
 * Display dimensions
 * -----------------------------------------------------------
 *
 * VUI window offsets are syntax values.
 *
 * The actual number of luma/chroma samples removed depends
 * on chroma_format_idc and is therefore calculated later.
 */

struct DisplayDimensions {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};


/*
 * Calculate the display dimensions after applying the
 * SPS conformance window.
 *
 * The offsets are expressed in units determined by
 * chroma_format_idc.
 */
[[nodiscard]]
constexpr DisplayDimensions
apply_window(
    std::uint32_t width,
    std::uint32_t height,
    ChromaFormat chroma_format,
    const Window& window) noexcept
{
    std::uint32_t sub_width_c = 1;
    std::uint32_t sub_height_c = 1;

    switch (chroma_format) {
    case ChromaFormat::Monochrome:
        sub_width_c = 1;
        sub_height_c = 1;
        break;

    case ChromaFormat::YUV420:
        sub_width_c = 2;
        sub_height_c = 2;
        break;

    case ChromaFormat::YUV422:
        sub_width_c = 2;
        sub_height_c = 1;
        break;

    case ChromaFormat::YUV444:
        sub_width_c = 1;
        sub_height_c = 1;
        break;
    }

    const std::uint64_t horizontal_crop =
        static_cast<std::uint64_t>(
            window.left_offset +
            window.right_offset) *
        sub_width_c;

    const std::uint64_t vertical_crop =
        static_cast<std::uint64_t>(
            window.top_offset +
            window.bottom_offset) *
        sub_height_c;

    if (horizontal_crop >= width ||
        vertical_crop >= height) {
        return {};
    }

    return {
        static_cast<std::uint32_t>(
            width - horizontal_crop),

        static_cast<std::uint32_t>(
            height - vertical_crop)
    };
}


/*
 * -----------------------------------------------------------
 * VUI timing helpers
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr double frame_rate(
    const VuiParameters& vui) noexcept
{
    if (!vui.vui_timing_info_present_flag) {
        return 0.0;
    }

    if (vui.timing.num_units_in_tick == 0 ||
        vui.timing.time_scale == 0) {
        return 0.0;
    }

    /*
     * This is the basic timing ratio.
     *
     * The actual picture rate may additionally depend on
     * fixed_pic_rate / temporal-layer / HRD semantics.
     */
    return static_cast<double>(
               vui.timing.time_scale) /
           (2.0 *
            static_cast<double>(
                vui.timing.num_units_in_tick));
}


/*
 * Return whether the VUI describes a progressive source.
 *
 * This is a semantic convenience function. It should not
 * be interpreted as a complete decoder picture-structure
 * determination.
 */
[[nodiscard]]
constexpr bool progressive_source(
    const VuiParameters& vui) noexcept
{
    return vui.progressive_source_flag &&
           !vui.interlaced_source_flag;
}


/*
 * Return whether the VUI describes an interlaced source.
 */
[[nodiscard]]
constexpr bool interlaced_source(
    const VuiParameters& vui) noexcept
{
    return vui.interlaced_source_flag;
}

} // namespace bs