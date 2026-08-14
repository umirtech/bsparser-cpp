#pragma once

#include <rbsp_bitstream_reader.hpp>
#include <hevc_profile_tier_level.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace bs {

/*
 * Parser for:
 *
 *     profile_tier_level(
 *         profileTierPresentFlag,
 *         MaxNumSubLayersMinus1
 *     )
 *
 * H.265:
 *
 *     7.3.3.1 General profile, tier, and level syntax
 *
 * This parser operates directly on RbspBitstreamReader.
 *
 * No input data is copied.
 */

/*
 * -----------------------------------------------------------
 * Parser result
 * -----------------------------------------------------------
 */

struct ProfileTierLevelParseResult {
    bool ok = false;

    /*
     * Number of sub-layers represented by the syntax.
     */
    std::uint8_t max_sub_layers_minus1 = 0;

    /*
     * Reader position after the syntax structure.
     */
    std::size_t bits_consumed = 0;
};

/*
 * -----------------------------------------------------------
 * Validation helpers
 * -----------------------------------------------------------
 */

inline constexpr std::uint8_t kMaxProfileTierLevelSubLayers = 8;

/*
 * H.265 profile_idc is 5 bits.
 */
inline constexpr std::uint8_t kMaxGeneralProfileIdc = 31;

/*
 * -----------------------------------------------------------
 * Low-level parser
 * -----------------------------------------------------------
 */

inline void parse_general_profile(RbspBitstreamReader& bs, ProfileTierLevel& ptl) {
    /*
     * general_profile_space
     *
     * u(2)
     */
    ptl.general_profile_space = static_cast<std::uint8_t>(bs.read_bits(2));

    /*
     * general_tier_flag
     *
     * u(1)
     */
    ptl.general_tier_flag = bs.read_bit();

    /*
     * general_profile_idc
     *
     * u(5)
     */
    ptl.general_profile_idc = static_cast<std::uint8_t>(bs.read_bits(5));

    /*
     * general_profile_compatibility_flag[32]
     */
    for (std::size_t i = 0; i < 32; ++i) {
        ptl.general_profile_compatibility_flag[i] = bs.read_bit();
    }

    /*
     * The constraint information is represented by the
     * ProfileTierLevel model as raw fields.
     *
     * Keep this section explicit rather than hiding the
     * reserved bits inside a magic integer.
     */

    ptl.general_progressive_source_flag = bs.read_bit();

    ptl.general_interlaced_source_flag = bs.read_bit();

    ptl.general_non_packed_constraint_flag = bs.read_bit();

    ptl.general_frame_only_constraint_flag = bs.read_bit();

    /*
     * The remaining constraint bits are version/profile
     * dependent.
     *
     * The syntax has a fixed-width reserved region whose
     * interpretation depends on profile compatibility.
     *
     * Preserve it exactly.
     */
    ptl.general_reserved_zero_44bits = bs.read_bits(44);
}

/*
 * -----------------------------------------------------------
 * Sub-layer profile parser
 * -----------------------------------------------------------
 */

inline void parse_sub_layer_profile(
    RbspBitstreamReader& bs, ProfileTierLevel& ptl, std::size_t sub_layer
) {
    if (sub_layer >= ptl.sub_layer.size()) {
        throw std::out_of_range("profile_tier_level: invalid sub-layer");
    }

    auto& layer = ptl.sub_layer[sub_layer];

    /*
     * sub_layer_profile_space[i]
     *
     * u(2)
     */
    layer.profile_space = static_cast<std::uint8_t>(bs.read_bits(2));

    /*
     * sub_layer_tier_flag[i]
     *
     * u(1)
     */
    layer.tier_flag = bs.read_bit();

    /*
     * sub_layer_profile_idc[i]
     *
     * u(5)
     */
    layer.profile_idc = static_cast<std::uint8_t>(bs.read_bits(5));

    /*
     * sub_layer_profile_compatibility_flag[i][j]
     */
    for (std::size_t j = 0; j < 32; ++j) {
        layer.profile_compatibility_flag[j] = bs.read_bit();
    }

    /*
     * Constraint flags.
     */
    layer.progressive_source_flag = bs.read_bit();

    layer.interlaced_source_flag = bs.read_bit();

    layer.non_packed_constraint_flag = bs.read_bit();

    layer.frame_only_constraint_flag = bs.read_bit();

    /*
     * Reserved constraint bits.
     */
    layer.reserved_zero_44bits = bs.read_bits(44);
}

/*
 * -----------------------------------------------------------
 * Main parser
 * -----------------------------------------------------------
 */

inline ProfileTierLevelParseResult parse_profile_tier_level(
    RbspBitstreamReader& bs,
    bool profile_tier_present_flag,
    std::uint8_t max_num_sub_layers_minus1,
    ProfileTierLevel& ptl
) {
    if (max_num_sub_layers_minus1 >= kMaxProfileTierLevelSubLayers) {
        throw std::invalid_argument(
            "profile_tier_level: invalid "
            "MaxNumSubLayersMinus1"
        );
    }

    /*
     * Keep the starting position so callers can inspect
     * parser progress.
     */
    const std::size_t start = bs.bit_position();

    /*
     * Reset the destination.
     */
    ptl = {};

    ptl.max_sub_layers_minus1 = max_num_sub_layers_minus1;

    /*
     * -------------------------------------------------------
     * General profile information
     * -------------------------------------------------------
     *
     * profileTierPresentFlag == 1
     */
    if (profile_tier_present_flag) {
        parse_general_profile(bs, ptl);
    }

    /*
     * -------------------------------------------------------
     * general_level_idc
     * -------------------------------------------------------
     *
     * This field is always present.
     */
    ptl.general_level_idc = static_cast<std::uint8_t>(bs.read_bits(8));

    /*
     * -------------------------------------------------------
     * Sub-layer profile/level presence flags
     * -------------------------------------------------------
     *
     * The syntax loops downward:
     *
     *     for(i = MaxNumSubLayersMinus1 - 1;
     *         i >= 0;
     *         i--)
     *
     * and signals:
     *
     *     sub_layer_profile_present_flag[i]
     *     sub_layer_level_present_flag[i]
     */
    for (std::size_t i = 0; i < max_num_sub_layers_minus1; ++i) {
        ptl.sub_layer_profile_present_flag[i] = bs.read_bit();

        ptl.sub_layer_level_present_flag[i] = bs.read_bit();
    }

    /*
     * H.265 byte alignment:
     *
     * if(MaxNumSubLayersMinus1 > 0)
     *     for(i = MaxNumSubLayersMinus1;
     *         i < 8;
     *         i++)
     *         reserved_zero_2bits
     *
     * There are:
     *
     *     2 * (8 - (MaxNumSubLayersMinus1 + 1))
     *
     * bits.
     */
    if (max_num_sub_layers_minus1 > 0) {
        const std::size_t reserved_count =
            2 * (8 - (static_cast<std::size_t>(max_num_sub_layers_minus1) + 1));

        for (std::size_t i = 0; i < reserved_count; ++i) {
            const bool reserved = bs.read_bit();

            /*
             * Reserved bits are required to be zero for a
             * conforming bitstream. We don't fail merely
             * because a decoder encounters a future/reserved
             * value; preserve it through the normal reader
             * behavior.
             */
            (void)reserved;
        }
    }

    /*
     * -------------------------------------------------------
     * Sub-layer profile/level
     * -------------------------------------------------------
     *
     * Profile information is present only when its flag is
     * set.
     */
    for (std::size_t i = 0; i < max_num_sub_layers_minus1; ++i) {
        if (ptl.sub_layer_profile_present_flag[i]) {
            parse_sub_layer_profile(bs, ptl, i);
        }

        if (ptl.sub_layer_level_present_flag[i]) {
            ptl.sub_layer[i].level_idc = static_cast<std::uint8_t>(bs.read_bits(8));
        }
    }

    /*
     * Store parser progress.
     */
    return ProfileTierLevelParseResult{true, max_num_sub_layers_minus1, bs.bit_position() - start};
}

/*
 * -----------------------------------------------------------
 * Convenience overload
 * -----------------------------------------------------------
 */

inline ProfileTierLevelParseResult parse_profile_tier_level(
    RbspBitstreamReader& bs, bool profile_tier_present_flag, std::uint8_t max_num_sub_layers_minus1
) {
    ProfileTierLevel result{};

    return parse_profile_tier_level(
        bs, profile_tier_present_flag, max_num_sub_layers_minus1, result
    );
}

/*
 * -----------------------------------------------------------
 * Validation
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline bool validate_profile_tier_level(const ProfileTierLevel& ptl) noexcept {
    if (ptl.max_sub_layers_minus1 >= kMaxProfileTierLevelSubLayers) {
        return false;
    }

    if (ptl.general_profile_space > 3) {
        return false;
    }

    if (ptl.general_profile_idc > kMaxGeneralProfileIdc) {
        return false;
    }

    /*
     * The profile compatibility array must have exactly
     * 32 entries because that is part of the syntax.
     */
    return true;
}

/*
 * -----------------------------------------------------------
 * Profile compatibility helpers
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline bool profile_compatible(const ProfileTierLevel& ptl, std::size_t profile_idc) noexcept {
    if (profile_idc >= ptl.general_profile_compatibility_flag.size()) {
        return false;
    }

    return ptl.general_profile_compatibility_flag[profile_idc];
}

/*
 * A profile is compatible with itself by definition of the
 * profile compatibility flag semantics used by HEVC.
 */
[[nodiscard]]
inline bool profile_or_compatible(const ProfileTierLevel& ptl, std::size_t profile_idc) noexcept {
    if (ptl.general_profile_idc == profile_idc) {
        return true;
    }

    return profile_compatible(ptl, profile_idc);
}

/*
 * -----------------------------------------------------------
 * Level helpers
 * -----------------------------------------------------------
 */

/*
 * H.265 general_level_idc uses values such as:
 *
 *     30  -> Level 1
 *     60  -> Level 2
 *     63  -> Level 2.1
 *     90  -> Level 3
 *     93  -> Level 3.1
 *     ...
 *
 * Keep the raw value in the syntax structure.
 */
[[nodiscard]]
constexpr bool is_valid_level_idc(std::uint8_t level_idc) noexcept {
    switch (level_idc) {
        case 30:
        case 60:
        case 63:
        case 90:
        case 93:
        case 120:
        case 123:
        case 150:
        case 153:
        case 156:
        case 180:
        case 183:
        case 186:
        case 255:
            return true;

        default:
            return false;
    }
}

/*
 * Return level number multiplied by 10.
 *
 * Examples:
 *
 *     30  -> 10
 *     60  -> 20
 *     63  -> 21
 *     93  -> 31
 *     150 -> 50
 */
[[nodiscard]]
constexpr std::uint16_t level_number_x10(std::uint8_t level_idc) noexcept {
    switch (level_idc) {
        case 30:
            return 10;
        case 60:
            return 20;
        case 63:
            return 21;
        case 90:
            return 30;
        case 93:
            return 31;
        case 120:
            return 40;
        case 123:
            return 41;
        case 150:
            return 50;
        case 153:
            return 51;
        case 156:
            return 52;
        case 180:
            return 60;
        case 183:
            return 61;
        case 186:
            return 62;

        /*
         * 255 is the escape/reserved level value in the syntax.
         */
        default:
            return 0;
    }
}

/*
 * -----------------------------------------------------------
 * Constraint helpers
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool is_progressive_only(const ProfileTierLevel& ptl) noexcept {
    return ptl.general_progressive_source_flag && !ptl.general_interlaced_source_flag;
}

[[nodiscard]]
constexpr bool is_frame_only(const ProfileTierLevel& ptl) noexcept {
    return ptl.general_frame_only_constraint_flag;
}

[[nodiscard]]
constexpr bool is_non_packed(const ProfileTierLevel& ptl) noexcept {
    return ptl.general_non_packed_constraint_flag;
}

}  // namespace bs