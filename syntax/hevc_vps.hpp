#pragma once

#include "hevc_common.hpp"
#include "hevc_hrd.hpp"
#include "hevc_profile_tier_level.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bs {

/*
 * H.265 Video Parameter Set
 *
 * Corresponds primarily to:
 *
 *     7.3.2.1 video_parameter_set_rbsp()
 *
 * The structure contains decoded syntax values.
 *
 * It intentionally does NOT contain parser state or
 * bitstream-specific objects.
 */


/*
 * One VPS layer-set entry.
 *
 * layer_id_included_flag[j][i]
 *
 * describes whether layer j is included in layer set i.
 *
 * vps_max_layer_id is at most 63.
 */
struct VpsLayerSet {
    std::vector<bool> layer_id_included_flag;

    /*
     * The derived list of actual layer IDs is useful to
     * consumers and avoids repeatedly walking the flags.
     */
    std::vector<std::uint8_t> layer_ids;

    [[nodiscard]]
    bool contains(std::uint8_t layer_id) const noexcept
    {
        if (layer_id >= layer_id_included_flag.size()) {
            return false;
        }

        return layer_id_included_flag[layer_id];
    }

    [[nodiscard]]
    std::size_t layer_count() const noexcept
    {
        return layer_ids.size();
    }
};


/*
 * One VPS HRD parameter-set entry.
 *
 * H.265 associates HRD parameters with a layer set.
 */
struct VpsHrdParameter {
    /*
     * layer_set_idx_for_ols_minus1
     *
     * Depending on the VPS syntax context this identifies
     * the layer set to which the HRD parameters apply.
     */
    std::uint32_t layer_set_idx_for_ols_minus1 = 0;

    /*
     * common_inf_present_flag
     *
     * Passed into hrd_parameters().
     */
    bool common_inf_present_flag = false;

    HrdParameters hrd{};
};


/*
 * VPS timing information.
 *
 * H.265 signals timing information independently from the
 * SPS VUI timing structure.
 */
struct VpsTimingInfo {
    bool timing_info_present_flag = false;

    std::uint32_t num_units_in_tick = 0;
    std::uint32_t time_scale = 0;

    bool poc_proportional_to_timing_flag = false;

    std::uint32_t num_ticks_poc_diff_one_minus1 = 0;

    /*
     * Number of HRD parameter sets.
     */
    std::uint32_t num_hrd_parameters = 0;

    std::vector<VpsHrdParameter> hrd_parameters;

    [[nodiscard]]
    bool valid() const noexcept
    {
        if (!timing_info_present_flag) {
            return true;
        }

        return num_units_in_tick != 0 &&
               time_scale != 0;
    }
};


/*
 * VPS extension information.
 *
 * The exact extension syntax is profile/tool dependent.
 *
 * We retain the flags and opaque extension RBSP payload
 * separately rather than pretending that all extension
 * syntax is part of the base VPS.
 */
struct VpsExtension {
    bool extension_flag = false;

    /*
     * Base VPS extensions can contain additional syntax.
     *
     * The parser can populate these when the corresponding
     * extension is supported.
     */
    bool extension_data_flag = false;
};


/*
 * Complete Video Parameter Set.
 */
struct VideoParameterSet {

    /*
     * -------------------------------------------------------
     * VPS identification
     * -------------------------------------------------------
     */

    /*
     * vps_video_parameter_set_id
     *
     * u(4)
     */
    std::uint8_t vps_video_parameter_set_id = 0;


    /*
     * -------------------------------------------------------
     * Base-layer configuration
     * -------------------------------------------------------
     */

    bool vps_base_layer_internal_flag = false;

    bool vps_base_layer_available_flag = false;


    /*
     * -------------------------------------------------------
     * Layer / temporal configuration
     * -------------------------------------------------------
     */

    /*
     * vps_max_layers_minus1
     *
     * u(6)
     */
    std::uint8_t vps_max_layers_minus1 = 0;

    /*
     * vps_max_sub_layers_minus1
     *
     * u(3)
     *
     * Maximum value is 7.
     */
    std::uint8_t vps_max_sub_layers_minus1 = 0;

    bool vps_temporal_id_nesting_flag = false;


    /*
     * Reserved:
     *
     * vps_reserved_0xffff_16bits
     *
     * The parser should validate this equals 0xFFFF.
     */
    std::uint16_t vps_reserved_0xffff_16bits = 0;


    /*
     * -------------------------------------------------------
     * Profile / tier / level
     * -------------------------------------------------------
     */

    ProfileTierLevel profile_tier_level{};


    /*
     * -------------------------------------------------------
     * Sub-layer ordering
     * -------------------------------------------------------
     */

    bool vps_sub_layer_ordering_info_present_flag = false;

    /*
     * One entry for each coded temporal sub-layer.
     *
     * Depending on vps_sub_layer_ordering_info_present_flag,
     * entries may be explicitly signaled starting from
     * sub-layer 0 or only from the highest sub-layer with
     * lower entries inferred.
     */
    std::array<SubLayerOrderingInfo, 8>
        sub_layer_ordering_info{};


    /*
     * -------------------------------------------------------
     * Layer-set configuration
     * -------------------------------------------------------
     */

    /*
     * vps_max_layer_id
     *
     * u(6)
     */
    std::uint8_t vps_max_layer_id = 0;

    /*
     * vps_num_layer_sets_minus1
     *
     * ue(v)
     */
    std::uint32_t vps_num_layer_sets_minus1 = 0;

    /*
     * layer_sets[0] is the default base layer set.
     *
     * Additional layer sets are represented explicitly.
     */
    std::vector<VpsLayerSet> layer_sets;


    /*
     * -------------------------------------------------------
     * Timing / HRD
     * -------------------------------------------------------
     */

    VpsTimingInfo timing{};


    /*
     * -------------------------------------------------------
     * Extension
     * -------------------------------------------------------
     */

    VpsExtension extension{};


    /*
     * -------------------------------------------------------
     * Helpers
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr std::size_t max_layers() const noexcept
    {
        return static_cast<std::size_t>(
            vps_max_layers_minus1) + 1;
    }

    [[nodiscard]]
    constexpr std::size_t max_sub_layers() const noexcept
    {
        return static_cast<std::size_t>(
            vps_max_sub_layers_minus1) + 1;
    }

    [[nodiscard]]
    constexpr std::size_t layer_set_count() const noexcept
    {
        return static_cast<std::size_t>(
            vps_num_layer_sets_minus1) + 1;
    }

    [[nodiscard]]
    bool valid() const noexcept
    {
        return vps_video_parameter_set_id < 16 &&
               vps_max_layers_minus1 < 64 &&
               vps_max_sub_layers_minus1 < 8;
    }

    [[nodiscard]]
    const VpsLayerSet* layer_set(
        std::size_t index) const noexcept
    {
        if (index >= layer_sets.size()) {
            return nullptr;
        }

        return &layer_sets[index];
    }

    [[nodiscard]]
    VpsLayerSet* layer_set(
        std::size_t index) noexcept
    {
        if (index >= layer_sets.size()) {
            return nullptr;
        }

        return &layer_sets[index];
    }
};


/*
 * -----------------------------------------------------------
 * VPS construction helpers
 * -----------------------------------------------------------
 */


/*
 * Initialize the sub-layer ordering array.
 *
 * The array always has eight entries so that parser code
 * doesn't need optional allocations.
 */
inline void initialize_vps_sub_layer_ordering(
    VideoParameterSet& vps)
{
    for (auto& entry : vps.sub_layer_ordering_info) {
        entry = {};
    }
}


/*
 * Initialize layer sets.
 *
 * H.265 always has layer set 0.
 *
 * Additional layer sets are signaled by
 *
 *     vps_num_layer_sets_minus1
 */
inline void initialize_vps_layer_sets(
    VideoParameterSet& vps)
{
    const std::size_t count =
        static_cast<std::size_t>(
            vps.vps_num_layer_sets_minus1) + 1;

    vps.layer_sets.clear();
    vps.layer_sets.resize(count);

    for (auto& layer_set : vps.layer_sets) {
        layer_set.layer_id_included_flag.resize(
            static_cast<std::size_t>(
                vps.vps_max_layer_id) + 1,
            false);

        layer_set.layer_ids.clear();
    }

    /*
     * Layer set 0 contains layer 0.
     *
     * This is part of the semantics of the base layer set.
     */
    if (!vps.layer_sets.empty()) {
        auto& layer_set = vps.layer_sets[0];

        if (!layer_set.layer_id_included_flag.empty()) {
            layer_set.layer_id_included_flag[0] = true;
            layer_set.layer_ids.push_back(0);
        }
    }
}


/*
 * Build the derived layer-id list for a layer set.
 */
inline void derive_vps_layer_ids(
    VpsLayerSet& layer_set)
{
    layer_set.layer_ids.clear();

    for (std::size_t layer_id = 0;
         layer_id < layer_set.layer_id_included_flag.size();
         ++layer_id) {

        if (layer_set.layer_id_included_flag[layer_id]) {
            layer_set.layer_ids.push_back(
                static_cast<std::uint8_t>(layer_id));
        }
    }
}


/*
 * Build derived layer-id lists for all VPS layer sets.
 */
inline void derive_vps_layer_ids(
    VideoParameterSet& vps)
{
    for (auto& layer_set : vps.layer_sets) {
        derive_vps_layer_ids(layer_set);
    }
}


/*
 * -----------------------------------------------------------
 * Sub-layer ordering access
 * -----------------------------------------------------------
 */


/*
 * Return the decoded DPB buffering limit:
 *
 *     max_dec_pic_buffering_minus1 + 1
 */
[[nodiscard]]
constexpr std::uint32_t
max_dec_pic_buffering(
    const VideoParameterSet& vps,
    std::size_t sub_layer) noexcept
{
    if (sub_layer >= 8) {
        return 0;
    }

    return vps.sub_layer_ordering_info[sub_layer]
               .max_dec_pic_buffering_minus1 + 1;
}


/*
 * Return the maximum number of reorder pictures.
 */
[[nodiscard]]
constexpr std::uint32_t
max_num_reorder_pics(
    const VideoParameterSet& vps,
    std::size_t sub_layer) noexcept
{
    if (sub_layer >= 8) {
        return 0;
    }

    return vps.sub_layer_ordering_info[sub_layer]
        .max_num_reorder_pics;
}


/*
 * Return max latency increase.
 *
 * H.265 syntax stores:
 *
 *     max_latency_increase_plus1
 *
 * A value of zero means the syntax indicates no
 * latency-increase value.
 */
[[nodiscard]]
constexpr std::uint32_t
max_latency_increase(
    const VideoParameterSet& vps,
    std::size_t sub_layer) noexcept
{
    if (sub_layer >= 8) {
        return 0;
    }

    const auto value =
        vps.sub_layer_ordering_info[sub_layer]
            .max_latency_increase_plus1;

    return value == 0 ? 0 : value - 1;
}


/*
 * -----------------------------------------------------------
 * VPS timing helpers
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr double vps_time_scale(
    const VideoParameterSet& vps) noexcept
{
    if (!vps.timing.timing_info_present_flag ||
        vps.timing.num_units_in_tick == 0) {
        return 0.0;
    }

    return static_cast<double>(
               vps.timing.time_scale) /
           static_cast<double>(
               vps.timing.num_units_in_tick);
}


/*
 * -----------------------------------------------------------
 * Validation
 * -----------------------------------------------------------
 */


/*
 * H.265 limits.
 */
inline constexpr std::uint8_t
    kMaxVpsLayersMinus1 = 63;

inline constexpr std::uint8_t
    kMaxVpsSubLayersMinus1 = 7;

inline constexpr std::uint8_t
    kMaxVpsLayerId = 63;


/*
 * Validate the base VPS fields.
 */
[[nodiscard]]
constexpr bool validate_vps_base(
    const VideoParameterSet& vps) noexcept
{
    if (vps.vps_video_parameter_set_id >= 16) {
        return false;
    }

    if (vps.vps_max_layers_minus1 >
        kMaxVpsLayersMinus1) {
        return false;
    }

    if (vps.vps_max_sub_layers_minus1 >
        kMaxVpsSubLayersMinus1) {
        return false;
    }

    if (vps.vps_max_layer_id >
        kMaxVpsLayerId) {
        return false;
    }

    /*
     * The reserved field is required to be all ones.
     */
    if (vps.vps_reserved_0xffff_16bits != 0xFFFF) {
        return false;
    }

    return true;
}


/*
 * Validate that layer-set dimensions match VPS limits.
 */
[[nodiscard]]
inline bool validate_vps_layer_sets(
    const VideoParameterSet& vps) noexcept
{
    if (vps.layer_sets.empty()) {
        return false;
    }

    if (vps.layer_sets.size() !=
        static_cast<std::size_t>(
            vps.vps_num_layer_sets_minus1) + 1) {
        return false;
    }

    const std::size_t expected_flags =
        static_cast<std::size_t>(
            vps.vps_max_layer_id) + 1;

    for (const auto& set : vps.layer_sets) {
        if (set.layer_id_included_flag.size() !=
            expected_flags) {
            return false;
        }
    }

    return true;
}

} // namespace bs