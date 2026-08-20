// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bs {

/*
 * H.265 HRD
 *
 * Annex E:
 *
 *     E.2.2 HRD parameters syntax
 *     E.2.3 Sub-layer HRD parameters syntax
 *
 * The syntax is parameterized by:
 *
 *     commonInfPresentFlag
 *     MaxNumSubLayersMinus1
 *
 * We model the syntax values directly.
 */

/*
 * One CPB entry.
 *
 * sub_layer_hrd_parameters() contains one or more of
 * these entries.
 *
 * For each CPB:
 *
 *     bit_rate_value_minus1
 *     cpb_size_value_minus1
 *
 * and, when sub_pic_hrd_params_present_flag is true:
 *
 *     cpb_size_du_value_minus1
 *     bit_rate_du_value_minus1
 *
 * followed by:
 *
 *     cbr_flag
 */
struct CpbEntry {
    std::uint32_t bit_rate_value_minus1 = 0;
    std::uint32_t cpb_size_value_minus1 = 0;

    /*
     * Present only when:
     *
     *     sub_pic_hrd_params_present_flag == true
     */
    std::uint32_t cpb_size_du_value_minus1 = 0;
    std::uint32_t bit_rate_du_value_minus1 = 0;

    bool cbr_flag = false;
};

/*
 * sub_layer_hrd_parameters()
 *
 * CpbCnt is cpb_cnt_minus1 for the corresponding
 * temporal sub-layer.
 */
struct SubLayerHrdParameters {
    std::vector<CpbEntry> cpb_entries;

    [[nodiscard]]
    std::size_t cpb_count() const noexcept {
        return cpb_entries.size();
    }

    [[nodiscard]]
    bool empty() const noexcept {
        return cpb_entries.empty();
    }
};

/*
 * Timing information shared by HRD parameters.
 *
 * These fields are present when:
 *
 *     timing_info_present_flag == 1
 */
struct HrdTimingInfo {
    std::uint32_t num_units_in_tick = 0;
    std::uint32_t time_scale = 0;

    [[nodiscard]]
    bool valid() const noexcept {
        return num_units_in_tick != 0 && time_scale != 0;
    }
};

/*
 * Common HRD information.
 *
 * This corresponds to the portion of:
 *
 *     hrd_parameters()
 *
 * controlled by commonInfPresentFlag.
 */
struct HrdCommonInfo {
    /*
     * timing_info_present_flag
     */
    bool timing_info_present_flag = false;

    HrdTimingInfo timing{};

    /*
     * Whether NAL and/or VCL HRD parameters are present.
     */
    bool nal_hrd_parameters_present_flag = false;
    bool vcl_hrd_parameters_present_flag = false;

    /*
     * Sub-picture CPB parameters.
     */
    bool sub_pic_hrd_params_present_flag = false;

    /*
     * Present when sub_pic_hrd_params_present_flag == true.
     *
     * tick_divisor_minus2 is u(8).
     */
    std::uint8_t tick_divisor_minus2 = 0;

    /*
     * Number of bits used for DU CPB removal delay.
     *
     * Syntax:
     *
     *     du_cpb_removal_delay_increment_length_minus1
     */
    std::uint8_t du_cpb_removal_delay_increment_length_minus1 = 0;

    /*
     * Indicates whether sub-picture CPB parameters are
     * carried in picture timing SEI messages.
     */
    bool sub_pic_cpb_params_in_pic_timing_sei_flag = false;

    /*
     * Length of the DU output delay field.
     */
    std::uint8_t dpb_output_delay_du_length_minus1 = 0;

    /*
     * HRD scaling factors.
     */
    std::uint8_t bit_rate_scale = 0;
    std::uint8_t cpb_size_scale = 0;

    /*
     * Present when sub_pic_hrd_params_present_flag == true.
     *
     * H.265 syntax:
     *
     *     cpb_size_du_scale
     */
    std::uint8_t cpb_size_du_scale = 0;

    /*
     * Delay field lengths.
     */
    std::uint8_t initial_cpb_removal_delay_length_minus1 = 0;

    std::uint8_t au_cpb_removal_delay_length_minus1 = 0;

    std::uint8_t dpb_output_delay_length_minus1 = 0;
};

/*
 * HRD information for one temporal sub-layer.
 *
 * The corresponding syntax contains:
 *
 *     fixed_pic_rate_general_flag
 *     fixed_pic_rate_within_cvs_flag
 *     elemental_duration_in_tc_minus1
 *     low_delay_hrd_flag
 *     cpb_cnt_minus1
 *
 * followed by NAL/VCL sub-layer HRD parameters.
 */
struct HrdSubLayer {
    /*
     * fixed_pic_rate_general_flag[i]
     */
    bool fixed_pic_rate_general_flag = false;

    /*
     * Present when:
     *
     *     !fixed_pic_rate_general_flag
     *
     * in the H.265 syntax.
     */
    bool fixed_pic_rate_within_cvs_flag = false;

    /*
     * Present when fixed_pic_rate_within_cvs_flag == true.
     *
     * This is:
     *
     *     elemental_duration_in_tc_minus1
     */
    std::uint32_t elemental_duration_in_tc_minus1 = 0;

    /*
     * low_delay_hrd_flag
     */
    bool low_delay_hrd_flag = false;

    /*
     * cpb_cnt_minus1
     *
     * H.265 constrains this to 0..31.
     */
    std::uint8_t cpb_cnt_minus1 = 0;

    /*
     * NAL HRD parameters for this sub-layer.
     *
     * Present when:
     *
     *     nal_hrd_parameters_present_flag
     */
    SubLayerHrdParameters nal{};

    /*
     * VCL HRD parameters for this sub-layer.
     *
     * Present when:
     *
     *     vcl_hrd_parameters_present_flag
     */
    SubLayerHrdParameters vcl{};

    [[nodiscard]]
    std::size_t cpb_count() const noexcept {
        return static_cast<std::size_t>(cpb_cnt_minus1) + 1;
    }
};

/*
 * Complete hrd_parameters() structure.
 *
 * The maximum number of temporal sub-layers in HEVC
 * syntax is 8:
 *
 *     max_sub_layers_minus1 <= 7
 */
struct HrdParameters {
    /*
     * Indicates whether common HRD information was actually
     * present in this syntax structure.
     *
     * This is a parameter to hrd_parameters(), rather than
     * a syntax element inside it.
     */
    bool common_info_present = false;

    HrdCommonInfo common{};

    /*
     * One entry per temporal sub-layer:
     *
     *     0 .. max_num_sub_layers_minus1
     */
    std::array<HrdSubLayer, 8> sub_layers{};

    std::uint8_t max_num_sub_layers_minus1 = 0;

    [[nodiscard]]
    constexpr std::size_t sub_layer_count() const noexcept {
        return static_cast<std::size_t>(max_num_sub_layers_minus1) + 1;
    }

    [[nodiscard]]
    HrdSubLayer& sub_layer(std::size_t index) noexcept {
        return sub_layers[index];
    }

    [[nodiscard]]
    const HrdSubLayer& sub_layer(std::size_t index) const noexcept {
        return sub_layers[index];
    }
};

/*
 * Semantic helpers
 */

/*
 * Return the number of bits in the initial CPB removal
 * delay field.
 */
[[nodiscard]]
constexpr unsigned initial_cpb_removal_delay_bits(const HrdParameters& hrd) noexcept {
    return static_cast<unsigned>(hrd.common.initial_cpb_removal_delay_length_minus1) + 1;
}

/*
 * Return the number of bits in the AU CPB removal
 * delay field.
 */
[[nodiscard]]
constexpr unsigned au_cpb_removal_delay_bits(const HrdParameters& hrd) noexcept {
    return static_cast<unsigned>(hrd.common.au_cpb_removal_delay_length_minus1) + 1;
}

/*
 * Return the number of bits in the DPB output delay field.
 */
[[nodiscard]]
constexpr unsigned dpb_output_delay_bits(const HrdParameters& hrd) noexcept {
    return static_cast<unsigned>(hrd.common.dpb_output_delay_length_minus1) + 1;
}

/*
 * Return the number of bits in the DU CPB removal
 * delay increment field.
 */
[[nodiscard]]
constexpr unsigned du_cpb_removal_delay_increment_bits(const HrdParameters& hrd) noexcept {
    return static_cast<unsigned>(hrd.common.du_cpb_removal_delay_increment_length_minus1) + 1;
}

/*
 * Return the number of bits in the DPB output delay DU
 * field.
 */
[[nodiscard]]
constexpr unsigned dpb_output_delay_du_bits(const HrdParameters& hrd) noexcept {
    return static_cast<unsigned>(hrd.common.dpb_output_delay_du_length_minus1) + 1;
}

/*
 * H.265 CPB scale.
 *
 * The specification uses bit_rate_scale and cpb_size_scale
 * to derive the actual quantities from the signaled
 * minus1 values.
 */
[[nodiscard]]
constexpr std::uint64_t bit_rate_scale_factor(const HrdParameters& hrd) noexcept {
    return std::uint64_t{1} << (6 + hrd.common.bit_rate_scale);
}

[[nodiscard]]
constexpr std::uint64_t cpb_size_scale_factor(const HrdParameters& hrd) noexcept {
    return std::uint64_t{1} << (4 + hrd.common.cpb_size_scale);
}

[[nodiscard]]
constexpr std::uint64_t cpb_size_du_scale_factor(const HrdParameters& hrd) noexcept {
    return std::uint64_t{1} << (4 + hrd.common.cpb_size_du_scale);
}

/*
 * Decode the actual nominal CPB bit rate from:
 *
 *     bit_rate_value_minus1
 *
 * together with bit_rate_scale.
 *
 * This follows the H.265 HRD semantic scaling.
 */
[[nodiscard]]
constexpr std::uint64_t cpb_bit_rate(const HrdParameters& hrd, const CpbEntry& cpb) noexcept {
    return (static_cast<std::uint64_t>(cpb.bit_rate_value_minus1) + 1) * bit_rate_scale_factor(hrd);
}

/*
 * Decode CPB size.
 */
[[nodiscard]]
constexpr std::uint64_t cpb_size(const HrdParameters& hrd, const CpbEntry& cpb) noexcept {
    return (static_cast<std::uint64_t>(cpb.cpb_size_value_minus1) + 1) * cpb_size_scale_factor(hrd);
}

/*
 * Decode DU CPB size.
 */
[[nodiscard]]
constexpr std::uint64_t cpb_size_du(const HrdParameters& hrd, const CpbEntry& cpb) noexcept {
    return (static_cast<std::uint64_t>(cpb.cpb_size_du_value_minus1) + 1) *
           cpb_size_du_scale_factor(hrd);
}

}  // namespace bs