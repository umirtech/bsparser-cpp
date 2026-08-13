#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace bs {

/*
 * H.265 profile_tier_level()
 *
 * 7.3.3
 */

struct ProfileTierLevelSubLayer {

    /*
     * sub_layer_profile_space[i]
     *
     * u(2)
     */
    std::uint8_t profile_space = 0;

    /*
     * sub_layer_tier_flag[i]
     *
     * u(1)
     */
    bool tier_flag = false;

    /*
     * sub_layer_profile_idc[i]
     *
     * u(5)
     */
    std::uint8_t profile_idc = 0;

    /*
     * sub_layer_profile_compatibility_flag[i][j]
     *
     * 32 flags
     */
    std::array<bool, 32>
        profile_compatibility_flag{};

    /*
     * Constraint flags.
     */
    bool progressive_source_flag = false;

    bool interlaced_source_flag = false;

    bool non_packed_constraint_flag = false;

    bool frame_only_constraint_flag = false;

    /*
     * Reserved constraint bits.
     *
     * Stored exactly as signaled.
     */
    std::uint64_t reserved_zero_44bits = 0;

    /*
     * sub_layer_level_idc[i]
     *
     * u(8)
     */
    std::uint8_t level_idc = 0;


    [[nodiscard]]
    constexpr bool
    profile_compatible(
        std::size_t profile) const noexcept
    {
        if (profile >=
            profile_compatibility_flag.size()) {
            return false;
        }

        return profile_compatibility_flag[profile];
    }
};


struct ProfileTierLevel {

    /*
     * MaxNumSubLayersMinus1 passed to
     * profile_tier_level().
     */
    std::uint8_t max_sub_layers_minus1 = 0;


    /*
     * -------------------------------------------------------
     * General profile information
     * -------------------------------------------------------
     */

    /*
     * general_profile_space
     *
     * u(2)
     */
    std::uint8_t general_profile_space = 0;

    /*
     * general_tier_flag
     *
     * u(1)
     */
    bool general_tier_flag = false;

    /*
     * general_profile_idc
     *
     * u(5)
     */
    std::uint8_t general_profile_idc = 0;

    /*
     * general_profile_compatibility_flag[32]
     */
    std::array<bool, 32>
        general_profile_compatibility_flag{};


    /*
     * -------------------------------------------------------
     * General constraint flags
     * -------------------------------------------------------
     */

    bool general_progressive_source_flag = false;

    bool general_interlaced_source_flag = false;

    bool general_non_packed_constraint_flag = false;

    bool general_frame_only_constraint_flag = false;


    /*
     * -------------------------------------------------------
     * Reserved constraint bits
     * -------------------------------------------------------
     */

    std::uint64_t general_reserved_zero_44bits = 0;


    /*
     * -------------------------------------------------------
     * General level
     * -------------------------------------------------------
     */

    /*
     * general_level_idc
     *
     * u(8)
     */
    std::uint8_t general_level_idc = 0;


    /*
     * -------------------------------------------------------
     * Sub-layer presence flags
     * -------------------------------------------------------
     */

    std::array<bool, 8>
        sub_layer_profile_present_flag{};

    std::array<bool, 8>
        sub_layer_level_present_flag{};


    /*
     * -------------------------------------------------------
     * Sub-layer profile / level data
     * -------------------------------------------------------
     */

    std::array<ProfileTierLevelSubLayer, 8>
        sub_layer{};


    /*
     * -------------------------------------------------------
     * Helpers
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr std::size_t
    max_sub_layers() const noexcept
    {
        return static_cast<std::size_t>(
            max_sub_layers_minus1) + 1;
    }


    [[nodiscard]]
    constexpr bool
    profile_compatible(
        std::size_t profile) const noexcept
    {
        if (profile >=
            general_profile_compatibility_flag.size()) {
            return false;
        }

        return general_profile_compatibility_flag[
            profile];
    }


    [[nodiscard]]
    constexpr bool
    profile_or_compatible(
        std::size_t profile) const noexcept
    {
        return
            general_profile_idc == profile ||
            profile_compatible(profile);
    }


    [[nodiscard]]
    constexpr bool
    progressive_only() const noexcept
    {
        return
            general_progressive_source_flag &&
            !general_interlaced_source_flag;
    }


    [[nodiscard]]
    constexpr bool
    frame_only() const noexcept
    {
        return general_frame_only_constraint_flag;
    }


    [[nodiscard]]
    constexpr bool
    non_packed() const noexcept
    {
        return general_non_packed_constraint_flag;
    }
};

} // namespace bs