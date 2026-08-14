#pragma once

#include "../bitstream/rbsp_bitstream_reader.hpp"
#include "../syntax/hevc_hrd.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace bs {

/*
 * H.265 HRD parser
 *
 * Annex E:
 *
 *     E.2.2 hrd_parameters()
 *     E.2.3 sub_layer_hrd_parameters()
 *
 * The parser operates directly on RbspBitstreamReader.
 *
 * No RBSP bytes are copied.
 */

/*
 * -----------------------------------------------------------
 * Constants
 * -----------------------------------------------------------
 */

inline constexpr std::uint8_t kMaxHrdSubLayers = 8;

inline constexpr std::uint8_t kMaxCpbCntMinus1 = 31;

/*
 * -----------------------------------------------------------
 * Parse result
 * -----------------------------------------------------------
 */

struct HrdParseResult {
    bool ok = false;

    std::size_t bits_consumed = 0;
};

/*
 * -----------------------------------------------------------
 * Validation helpers
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool valid_hrd_sub_layers(std::uint8_t max_num_sub_layers_minus1) noexcept {
    return max_num_sub_layers_minus1 < kMaxHrdSubLayers;
}

[[nodiscard]]
constexpr bool valid_cpb_count(std::uint32_t cpb_cnt_minus1) noexcept {
    return cpb_cnt_minus1 <= kMaxCpbCntMinus1;
}

/*
 * -----------------------------------------------------------
 * sub_layer_hrd_parameters()
 * -----------------------------------------------------------
 *
 * Syntax:
 *
 * for(j = 0; j <= cpbCntMinus1; j++) {
 *
 *     bit_rate_value_minus1
 *     cpb_size_value_minus1
 *
 *     if(sub_pic_hrd_params_present_flag) {
 *         cpb_size_du_value_minus1
 *         bit_rate_du_value_minus1
 *     }
 *
 *     cbr_flag
 * }
 *
 * The caller supplies the sub-picture flag and CPB count.
 */
inline void parse_sub_layer_hrd_parameters(
    RbspBitstreamReader& bs,
    std::uint8_t cpb_cnt_minus1,
    bool sub_pic_hrd_params_present_flag,
    SubLayerHrdParameters& result
) {
    if (!valid_cpb_count(cpb_cnt_minus1)) {
        throw std::invalid_argument("HRD: cpb_cnt_minus1 > 31");
    }

    const std::size_t cpb_count = static_cast<std::size_t>(cpb_cnt_minus1) + 1;

    result.cpb_entries.clear();

    result.cpb_entries.resize(cpb_count);

    for (std::size_t j = 0; j < cpb_count; ++j) {
        auto& cpb = result.cpb_entries[j];

        /*
         * bit_rate_value_minus1
         *
         * ue(v)
         */
        cpb.bit_rate_value_minus1 = bs.read_ue();

        /*
         * cpb_size_value_minus1
         *
         * ue(v)
         */
        cpb.cpb_size_value_minus1 = bs.read_ue();

        /*
         * Sub-picture CPB fields.
         */
        if (sub_pic_hrd_params_present_flag) {
            /*
             * cpb_size_du_value_minus1
             */
            cpb.cpb_size_du_value_minus1 = bs.read_ue();

            /*
             * bit_rate_du_value_minus1
             */
            cpb.bit_rate_du_value_minus1 = bs.read_ue();

        } else {
            /*
             * These fields are absent from the bitstream.
             */
            cpb.cpb_size_du_value_minus1 = 0;
            cpb.bit_rate_du_value_minus1 = 0;
        }

        /*
         * cbr_flag
         *
         * u(1)
         */
        cpb.cbr_flag = bs.read_bit();
    }
}

/*
 * -----------------------------------------------------------
 * Parse common HRD information
 * -----------------------------------------------------------
 *
 * This corresponds to the common portion controlled by:
 *
 *     commonInfPresentFlag
 *
 * IMPORTANT:
 *
 * timing_info_present_flag and the timing values are part of
 * the surrounding VUI syntax, not hrd_parameters().
 *
 * Therefore this function intentionally does not consume
 * them.
 */
inline void parse_hrd_common_info(RbspBitstreamReader& bs, HrdCommonInfo& common) {
    /*
     * nal_hrd_parameters_present_flag
     */
    common.nal_hrd_parameters_present_flag = bs.read_bit();

    /*
     * vcl_hrd_parameters_present_flag
     */
    common.vcl_hrd_parameters_present_flag = bs.read_bit();

    /*
     * If neither NAL nor VCL HRD parameters are present,
     * the remaining common HRD syntax is absent.
     */
    if (!common.nal_hrd_parameters_present_flag && !common.vcl_hrd_parameters_present_flag) {
        common.sub_pic_hrd_params_present_flag = false;

        common.tick_divisor_minus2 = 0;

        common.du_cpb_removal_delay_increment_length_minus1 = 0;

        common.sub_pic_cpb_params_in_pic_timing_sei_flag = false;

        common.dpb_output_delay_du_length_minus1 = 0;

        common.bit_rate_scale = 0;
        common.cpb_size_scale = 0;
        common.cpb_size_du_scale = 0;

        common.initial_cpb_removal_delay_length_minus1 = 0;

        common.au_cpb_removal_delay_length_minus1 = 0;

        common.dpb_output_delay_length_minus1 = 0;

        return;
    }

    /*
     * -------------------------------------------------------
     * Sub-picture HRD
     * -------------------------------------------------------
     */

    common.sub_pic_hrd_params_present_flag = bs.read_bit();

    if (common.sub_pic_hrd_params_present_flag) {
        /*
         * tick_divisor_minus2
         *
         * u(8)
         */
        common.tick_divisor_minus2 = bs.read_u8(8);

        /*
         * du_cpb_removal_delay_increment_length_minus1
         *
         * u(5)
         */
        common.du_cpb_removal_delay_increment_length_minus1 =
            static_cast<std::uint8_t>(bs.read_bits(5));

        /*
         * sub_pic_cpb_params_in_pic_timing_sei_flag
         *
         * u(1)
         */
        common.sub_pic_cpb_params_in_pic_timing_sei_flag = bs.read_bit();

        /*
         * dpb_output_delay_du_length_minus1
         *
         * u(5)
         */
        common.dpb_output_delay_du_length_minus1 = static_cast<std::uint8_t>(bs.read_bits(5));

    } else {
        common.tick_divisor_minus2 = 0;

        common.du_cpb_removal_delay_increment_length_minus1 = 0;

        common.sub_pic_cpb_params_in_pic_timing_sei_flag = false;

        common.dpb_output_delay_du_length_minus1 = 0;
    }

    /*
     * -------------------------------------------------------
     * Scaling factors
     * -------------------------------------------------------
     *
     * bit_rate_scale
     *
     * u(4)
     */
    common.bit_rate_scale = static_cast<std::uint8_t>(bs.read_bits(4));

    /*
     * cpb_size_scale
     *
     * u(4)
     */
    common.cpb_size_scale = static_cast<std::uint8_t>(bs.read_bits(4));

    /*
     * cpb_size_du_scale
     *
     * u(4)
     *
     * Only when sub-picture HRD is present.
     */
    if (common.sub_pic_hrd_params_present_flag) {
        common.cpb_size_du_scale = static_cast<std::uint8_t>(bs.read_bits(4));

    } else {
        common.cpb_size_du_scale = 0;
    }

    /*
     * -------------------------------------------------------
     * Delay field lengths
     * -------------------------------------------------------
     *
     * All are u(5).
     */

    common.initial_cpb_removal_delay_length_minus1 = static_cast<std::uint8_t>(bs.read_bits(5));

    common.au_cpb_removal_delay_length_minus1 = static_cast<std::uint8_t>(bs.read_bits(5));

    common.dpb_output_delay_length_minus1 = static_cast<std::uint8_t>(bs.read_bits(5));
}

/*
 * -----------------------------------------------------------
 * Parse one HRD sub-layer
 * -----------------------------------------------------------
 */
inline void parse_hrd_sub_layer(
    RbspBitstreamReader& bs, const HrdCommonInfo& common, HrdSubLayer& sub_layer
) {
    /*
     * -------------------------------------------------------
     * fixed_pic_rate_general_flag
     * -------------------------------------------------------
     */
    sub_layer.fixed_pic_rate_general_flag = bs.read_bit();

    /*
     * -------------------------------------------------------
     * fixed_pic_rate_within_cvs_flag
     * -------------------------------------------------------
     *
     * Present only when:
     *
     *     fixed_pic_rate_general_flag == 0
     */
    if (!sub_layer.fixed_pic_rate_general_flag) {
        sub_layer.fixed_pic_rate_within_cvs_flag = bs.read_bit();

    } else {
        /*
         * Inferred to one when the general flag is one.
         */
        sub_layer.fixed_pic_rate_within_cvs_flag = true;
    }

    /*
     * -------------------------------------------------------
     * elemental_duration_in_tc_minus1
     * -------------------------------------------------------
     *
     * Present when fixed_pic_rate_within_cvs_flag == 1.
     */
    if (sub_layer.fixed_pic_rate_within_cvs_flag) {
        sub_layer.elemental_duration_in_tc_minus1 = bs.read_ue();

        /*
         * low_delay_hrd_flag is not present in this branch.
         *
         * Infer it to false.
         */
        sub_layer.low_delay_hrd_flag = false;

    } else {
        /*
         * ---------------------------------------------------
         * low_delay_hrd_flag
         * ---------------------------------------------------
         */
        sub_layer.low_delay_hrd_flag = bs.read_bit();

        /*
         * elemental_duration_in_tc_minus1 is absent.
         */
        sub_layer.elemental_duration_in_tc_minus1 = 0;
    }

    /*
     * -------------------------------------------------------
     * cpb_cnt_minus1
     * -------------------------------------------------------
     *
     * Present only when low_delay_hrd_flag == 0.
     */
    if (!sub_layer.low_delay_hrd_flag) {
        const auto value = bs.read_ue();

        if (!valid_cpb_count(value)) {
            throw std::runtime_error("HRD: cpb_cnt_minus1 outside 0..31");
        }

        sub_layer.cpb_cnt_minus1 = static_cast<std::uint8_t>(value);

    } else {
        /*
         * When low_delay_hrd_flag == 1:
         *
         *     cpb_cnt_minus1 is inferred to 0.
         */
        sub_layer.cpb_cnt_minus1 = 0;
    }

    /*
     * -------------------------------------------------------
     * NAL HRD
     * -------------------------------------------------------
     */
    if (common.nal_hrd_parameters_present_flag) {
        parse_sub_layer_hrd_parameters(
            bs, sub_layer.cpb_cnt_minus1, common.sub_pic_hrd_params_present_flag, sub_layer.nal
        );

    } else {
        sub_layer.nal.cpb_entries.clear();
    }

    /*
     * -------------------------------------------------------
     * VCL HRD
     * -------------------------------------------------------
     */
    if (common.vcl_hrd_parameters_present_flag) {
        parse_sub_layer_hrd_parameters(
            bs, sub_layer.cpb_cnt_minus1, common.sub_pic_hrd_params_present_flag, sub_layer.vcl
        );

    } else {
        sub_layer.vcl.cpb_entries.clear();
    }
}

/*
 * -----------------------------------------------------------
 * Main hrd_parameters()
 * -----------------------------------------------------------
 *
 * commonInfPresentFlag is a parameter to the syntax.
 *
 * maxNumSubLayersMinus1 is also supplied by the caller.
 */
inline HrdParseResult parse_hrd_parameters(
    RbspBitstreamReader& bs,
    bool common_inf_present_flag,
    std::uint8_t max_num_sub_layers_minus1,
    HrdParameters& hrd
) {
    if (!valid_hrd_sub_layers(max_num_sub_layers_minus1)) {
        throw std::invalid_argument("HRD: MaxNumSubLayersMinus1 > 7");
    }

    const std::size_t start = bs.bit_position();

    /*
     * Reset destination.
     */
    hrd = {};

    hrd.common_info_present = common_inf_present_flag;

    hrd.max_num_sub_layers_minus1 = max_num_sub_layers_minus1;

    /*
     * -------------------------------------------------------
     * Common information
     * -------------------------------------------------------
     */
    if (common_inf_present_flag) {
        parse_hrd_common_info(bs, hrd.common);
    }

    /*
     * -------------------------------------------------------
     * Sub-layers
     * -------------------------------------------------------
     */
    for (std::size_t i = 0; i <= max_num_sub_layers_minus1; ++i) {
        parse_hrd_sub_layer(bs, hrd.common, hrd.sub_layers[i]);
    }

    return {true, bs.bit_position() - start};
}

/*
 * -----------------------------------------------------------
 * Convenience overload
 * -----------------------------------------------------------
 */
[[nodiscard]]
inline HrdParameters parse_hrd_parameters(
    RbspBitstreamReader& bs, bool common_inf_present_flag, std::uint8_t max_num_sub_layers_minus1
) {
    HrdParameters result{};

    parse_hrd_parameters(bs, common_inf_present_flag, max_num_sub_layers_minus1, result);

    return result;
}

/*
 * -----------------------------------------------------------
 * Validate parsed HRD
 * -----------------------------------------------------------
 */
[[nodiscard]]
inline bool validate_hrd_parameters(const HrdParameters& hrd) noexcept {
    if (!valid_hrd_sub_layers(hrd.max_num_sub_layers_minus1)) {
        return false;
    }

    for (std::size_t i = 0; i < hrd.sub_layer_count(); ++i) {
        const auto& layer = hrd.sub_layers[i];

        if (layer.cpb_cnt_minus1 > kMaxCpbCntMinus1) {
            return false;
        }

        const auto expected = static_cast<std::size_t>(layer.cpb_cnt_minus1) + 1;

        if (hrd.common.nal_hrd_parameters_present_flag) {
            if (layer.nal.cpb_entries.size() != expected) {
                return false;
            }
        }

        if (hrd.common.vcl_hrd_parameters_present_flag) {
            if (layer.vcl.cpb_entries.size() != expected) {
                return false;
            }
        }
    }

    return true;
}

/*
 * -----------------------------------------------------------
 * CPB helpers
 * -----------------------------------------------------------
 *
 * These operate on the syntax structure and use the scaling
 * helpers already defined in hevc_hrd.hpp.
 */

[[nodiscard]]
inline std::uint64_t nal_cpb_bit_rate(
    const HrdParameters& hrd, std::size_t sub_layer, std::size_t cpb_index
) {
    if (!hrd.common.nal_hrd_parameters_present_flag) {
        throw std::logic_error("HRD: NAL HRD parameters are absent");
    }

    if (sub_layer >= hrd.sub_layer_count()) {
        throw std::out_of_range("HRD: invalid sub-layer");
    }

    const auto& entries = hrd.sub_layers[sub_layer].nal.cpb_entries;

    if (cpb_index >= entries.size()) {
        throw std::out_of_range("HRD: invalid CPB index");
    }

    return cpb_bit_rate(hrd, entries[cpb_index]);
}

[[nodiscard]]
inline std::uint64_t nal_cpb_size(
    const HrdParameters& hrd, std::size_t sub_layer, std::size_t cpb_index
) {
    if (!hrd.common.nal_hrd_parameters_present_flag) {
        throw std::logic_error("HRD: NAL HRD parameters are absent");
    }

    if (sub_layer >= hrd.sub_layer_count()) {
        throw std::out_of_range("HRD: invalid sub-layer");
    }

    const auto& entries = hrd.sub_layers[sub_layer].nal.cpb_entries;

    if (cpb_index >= entries.size()) {
        throw std::out_of_range("HRD: invalid CPB index");
    }

    return cpb_size(hrd, entries[cpb_index]);
}

[[nodiscard]]
inline std::uint64_t vcl_cpb_bit_rate(
    const HrdParameters& hrd, std::size_t sub_layer, std::size_t cpb_index
) {
    if (!hrd.common.vcl_hrd_parameters_present_flag) {
        throw std::logic_error("HRD: VCL HRD parameters are absent");
    }

    if (sub_layer >= hrd.sub_layer_count()) {
        throw std::out_of_range("HRD: invalid sub-layer");
    }

    const auto& entries = hrd.sub_layers[sub_layer].vcl.cpb_entries;

    if (cpb_index >= entries.size()) {
        throw std::out_of_range("HRD: invalid CPB index");
    }

    return cpb_bit_rate(hrd, entries[cpb_index]);
}

[[nodiscard]]
inline std::uint64_t vcl_cpb_size(
    const HrdParameters& hrd, std::size_t sub_layer, std::size_t cpb_index
) {
    if (!hrd.common.vcl_hrd_parameters_present_flag) {
        throw std::logic_error("HRD: VCL HRD parameters are absent");
    }

    if (sub_layer >= hrd.sub_layer_count()) {
        throw std::out_of_range("HRD: invalid sub-layer");
    }

    const auto& entries = hrd.sub_layers[sub_layer].vcl.cpb_entries;

    if (cpb_index >= entries.size()) {
        throw std::out_of_range("HRD: invalid CPB index");
    }

    return cpb_size(hrd, entries[cpb_index]);
}

}  // namespace bs