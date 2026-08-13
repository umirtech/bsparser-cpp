#pragma once

#include "rbsp_bitstream_reader.hpp"
#include "hevc_vps.hpp"
#include "hevc_hrd_parser.hpp"
#include "hevc_profile_tier_level_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace bs {

/*
 * H.265 video_parameter_set_rbsp()
 *
 * 7.3.2.1
 *
 * This parser consumes the VPS RBSP syntax and populates the
 * existing VideoParameterSet syntax model.
 *
 * The parser is zero-copy with respect to the input RBSP.
 */


/*
 * -----------------------------------------------------------
 * Parser result
 * -----------------------------------------------------------
 */

struct VpsParseResult {
    bool ok = false;
    std::size_t bits_consumed = 0;
};


/*
 * -----------------------------------------------------------
 * Limits
 * -----------------------------------------------------------
 */

inline constexpr std::uint8_t
    kVpsMaxLayersMinus1 = 63;

inline constexpr std::uint8_t
    kVpsMaxSubLayersMinus1 = 7;

inline constexpr std::uint8_t
    kVpsMaxLayerId = 63;


/*
 * -----------------------------------------------------------
 * VPS identification
 * -----------------------------------------------------------
 */

inline void parse_vps_id(
    RbspBitstreamReader& bs,
    VideoParameterSet& vps)
{
    /*
     * vps_video_parameter_set_id
     *
     * u(4)
     */
    vps.vps_video_parameter_set_id =
        static_cast<std::uint8_t>(
            bs.read_bits(4));


    /*
     * vps_base_layer_internal_flag
     *
     * u(1)
     */
    vps.vps_base_layer_internal_flag =
        bs.read_bit();


    /*
     * vps_base_layer_available_flag
     *
     * u(1)
     */
    vps.vps_base_layer_available_flag =
        bs.read_bit();


    /*
     * vps_max_layers_minus1
     *
     * u(6)
     */
    vps.vps_max_layers_minus1 =
        static_cast<std::uint8_t>(
            bs.read_bits(6));


    /*
     * vps_max_sub_layers_minus1
     *
     * u(3)
     */
    vps.vps_max_sub_layers_minus1 =
        static_cast<std::uint8_t>(
            bs.read_bits(3));


    /*
     * vps_temporal_id_nesting_flag
     *
     * u(1)
     */
    vps.vps_temporal_id_nesting_flag =
        bs.read_bit();


    /*
     * vps_reserved_0xffff_16bits
     *
     * u(16)
     */
    vps.vps_reserved_0xffff_16bits =
        static_cast<std::uint16_t>(
            bs.read_bits(16));


    /*
     * Validate immediately because the values determine
     * allocation/loop bounds below.
     */
    if (vps.vps_max_layers_minus1 >
        kVpsMaxLayersMinus1) {

        throw std::runtime_error(
            "VPS: invalid vps_max_layers_minus1");
    }


    if (vps.vps_max_sub_layers_minus1 >
        kVpsMaxSubLayersMinus1) {

        throw std::runtime_error(
            "VPS: invalid vps_max_sub_layers_minus1");
    }


    if (vps.vps_reserved_0xffff_16bits !=
        0xFFFF) {

        throw std::runtime_error(
            "VPS: reserved bits are not 0xFFFF");
    }
}


/*
 * -----------------------------------------------------------
 * Profile / tier / level
 * -----------------------------------------------------------
 */

inline void parse_vps_profile_tier_level(
    RbspBitstreamReader& bs,
    VideoParameterSet& vps)
{
    /*
     * VPS always carries profileTierLevel with
     * profileTierPresentFlag = 1.
     */
    parse_profile_tier_level(
        bs,
        true,
        vps.vps_max_sub_layers_minus1,
        vps.profile_tier_level);
}


/*
 * -----------------------------------------------------------
 * Sub-layer ordering information
 * -----------------------------------------------------------
 */

inline void parse_vps_sub_layer_ordering(
    RbspBitstreamReader& bs,
    VideoParameterSet& vps)
{
    /*
     * vps_sub_layer_ordering_info_present_flag
     *
     * u(1)
     */
    vps.vps_sub_layer_ordering_info_present_flag =
        bs.read_bit();


    const std::size_t first_sub_layer =
        vps.vps_sub_layer_ordering_info_present_flag
            ? 0
            : vps.vps_max_sub_layers_minus1;


    /*
     * If the flag is zero, only the highest sub-layer is
     * explicitly coded. Lower sub-layers are inferred to
     * have the same values.
     */
    for (std::size_t i = first_sub_layer;
         i <= vps.vps_max_sub_layers_minus1;
         ++i) {

        auto& ordering =
            vps.sub_layer_ordering_info[i];


        /*
         * max_dec_pic_buffering_minus1
         *
         * ue(v)
         */
        ordering.max_dec_pic_buffering_minus1 =
            bs.read_ue();


        /*
         * max_num_reorder_pics
         *
         * ue(v)
         */
        ordering.max_num_reorder_pics =
            bs.read_ue();


        /*
         * max_latency_increase_plus1
         *
         * ue(v)
         */
        ordering.max_latency_increase_plus1 =
            bs.read_ue();
    }


    /*
     * Infer lower sub-layers when only the highest layer was
     * explicitly signaled.
     */
    if (!vps.vps_sub_layer_ordering_info_present_flag) {

        const auto& highest =
            vps.sub_layer_ordering_info[
                vps.vps_max_sub_layers_minus1];


        for (std::size_t i = 0;
             i < vps.vps_max_sub_layers_minus1;
             ++i) {

            vps.sub_layer_ordering_info[i] =
                highest;
        }
    }
}


/*
 * -----------------------------------------------------------
 * Layer sets
 * -----------------------------------------------------------
 */

inline void parse_vps_layer_sets(
    RbspBitstreamReader& bs,
    VideoParameterSet& vps)
{
    /*
     * vps_max_layer_id
     *
     * u(6)
     */
    vps.vps_max_layer_id =
        static_cast<std::uint8_t>(
            bs.read_bits(6));


    if (vps.vps_max_layer_id >
        kVpsMaxLayerId) {

        throw std::runtime_error(
            "VPS: invalid vps_max_layer_id");
    }


    /*
     * vps_num_layer_sets_minus1
     *
     * ue(v)
     */
    vps.vps_num_layer_sets_minus1 =
        bs.read_ue();


    /*
     * There is always layer set 0.
     *
     * The existing helper initializes set 0 with layer 0.
     */
    initialize_vps_layer_sets(vps);


    /*
     * layer_id_included_flag[i][j]
     *
     * Layer set 0 is implicit.
     *
     * Explicit flags are present for:
     *
     *     i = 1 .. vps_num_layer_sets_minus1
     */
    for (std::size_t i = 1;
         i < vps.layer_sets.size();
         ++i) {

        auto& layer_set =
            vps.layer_sets[i];


        for (std::size_t j = 0;
             j <= vps.vps_max_layer_id;
             ++j) {

            layer_set.layer_id_included_flag[j] =
                bs.read_bit();
        }
    }


    /*
     * Build the derived layer ID arrays.
     */
    derive_vps_layer_ids(vps);
}


/*
 * -----------------------------------------------------------
 * Timing information
 * -----------------------------------------------------------
 */

inline void parse_vps_timing(
    RbspBitstreamReader& bs,
    VideoParameterSet& vps)
{
    /*
     * vps_timing_info_present_flag
     */
    vps.timing.timing_info_present_flag =
        bs.read_bit();


    if (!vps.timing.timing_info_present_flag) {
        vps.timing = {};
        return;
    }


    /*
     * vps_num_units_in_tick
     *
     * u(32)
     */
    vps.timing.num_units_in_tick =
        static_cast<std::uint32_t>(
            bs.read_bits(32));


    /*
     * vps_time_scale
     *
     * u(32)
     */
    vps.timing.time_scale =
        static_cast<std::uint32_t>(
            bs.read_bits(32));


    /*
     * vps_poc_proportional_to_timing_flag
     */
    vps.timing.poc_proportional_to_timing_flag =
        bs.read_bit();


    if (vps.timing.poc_proportional_to_timing_flag) {

        /*
         * vps_num_ticks_poc_diff_one_minus1
         *
         * ue(v)
         */
        vps.timing.num_ticks_poc_diff_one_minus1 =
            bs.read_ue();

    } else {

        vps.timing.num_ticks_poc_diff_one_minus1 =
            0;
    }


    /*
     * vps_num_hrd_parameters
     *
     * ue(v)
     */
    vps.timing.num_hrd_parameters =
        bs.read_ue();


    vps.timing.hrd_parameters.clear();

    vps.timing.hrd_parameters.reserve(
        vps.timing.num_hrd_parameters);


    /*
     * hrd_parameters(i)
     */
    for (std::size_t i = 0;
         i < vps.timing.num_hrd_parameters;
         ++i) {

        VpsHrdParameter parameter{};


        /*
         * hrd_layer_set_idx
         *
         * ue(v)
         */
        parameter.layer_set_idx_for_ols_minus1 =
            bs.read_ue();


        /*
         * commonInfPresentFlag
         *
         * For the first HRD parameter set this is inferred
         * to one.
         *
         * For subsequent entries it is explicitly signaled.
         */
        if (i == 0) {

            parameter.common_inf_present_flag =
                true;

        } else {

            parameter.common_inf_present_flag =
                bs.read_bit();
        }


        /*
         * hrd_parameters()
         */
        parse_hrd_parameters(
            bs,
            parameter.common_inf_present_flag,
            vps.vps_max_sub_layers_minus1,
            parameter.hrd);


        vps.timing.hrd_parameters.push_back(
            std::move(parameter));
    }
}


/*
 * -----------------------------------------------------------
 * VPS extension
 * -----------------------------------------------------------
 */

inline void parse_vps_extension(
    RbspBitstreamReader& bs,
    VideoParameterSet& vps)
{
    /*
     * vps_extension_flag
     */
    vps.extension.extension_flag =
        bs.read_bit();


    if (!vps.extension.extension_flag) {
        vps.extension.extension_data_flag = false;
        return;
    }


    /*
     * extension_data_flag[i]
     *
     * This syntax continues until rbsp_trailing_bits().
     *
     * The syntax model only has a boolean summary rather than
     * retaining the entire extension payload.
     *
     * Consume every extension_data_flag so that the reader
     * finishes at rbsp_trailing_bits().
     */
    vps.extension.extension_data_flag = false;


    while (bs.more_rbsp_data()) {

        const bool flag =
            bs.read_bit();

        if (flag) {
            vps.extension.extension_data_flag = true;
        }
    }
}


/*
 * -----------------------------------------------------------
 * Main VPS parser
 * -----------------------------------------------------------
 */

inline VpsParseResult parse_video_parameter_set(
    RbspBitstreamReader& bs,
    VideoParameterSet& vps)
{
    const std::size_t start =
        bs.bit_position();


    /*
     * Reset the destination.
     */
    vps = {};


    /*
     * -------------------------------------------------------
     * VPS header
     * -------------------------------------------------------
     */
    parse_vps_id(
        bs,
        vps);


    /*
     * -------------------------------------------------------
     * profile_tier_level()
     * -------------------------------------------------------
     */
    parse_vps_profile_tier_level(
        bs,
        vps);


    /*
     * -------------------------------------------------------
     * sub-layer ordering
     * -------------------------------------------------------
     */
    parse_vps_sub_layer_ordering(
        bs,
        vps);


    /*
     * -------------------------------------------------------
     * layer sets
     * -------------------------------------------------------
     */
    parse_vps_layer_sets(
        bs,
        vps);


    /*
     * -------------------------------------------------------
     * timing + HRD
     * -------------------------------------------------------
     */
    parse_vps_timing(
        bs,
        vps);


    /*
     * -------------------------------------------------------
     * extension
     * -------------------------------------------------------
     */
    parse_vps_extension(
        bs,
        vps);


    return {
        true,
        bs.bit_position() - start
    };
}


/*
 * -----------------------------------------------------------
 * Convenience overload
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline VideoParameterSet parse_video_parameter_set(
    RbspBitstreamReader& bs)
{
    VideoParameterSet vps{};

    parse_video_parameter_set(
        bs,
        vps);

    return vps;
}


/*
 * -----------------------------------------------------------
 * Validation
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline bool validate_video_parameter_set(
    const VideoParameterSet& vps) noexcept
{
    if (!validate_vps_base(vps)) {
        return false;
    }


    if (!validate_vps_layer_sets(vps)) {
        return false;
    }


    if (vps.timing.timing_info_present_flag) {

        if (!vps.timing.valid()) {
            return false;
        }

        if (vps.timing.hrd_parameters.size() !=
            vps.timing.num_hrd_parameters) {
            return false;
        }
    }


    return true;
}

} // namespace bs