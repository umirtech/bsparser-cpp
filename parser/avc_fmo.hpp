// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/*
 * ===========================================================================
 * H.264 flexible macroblock ordering (FMO), clauses 8.2.2 and 8.2.3
 * ===========================================================================
 *
 * Derives MapUnitToSliceGroupMap and MbToSliceGroupMap for every slice group
 * map type, then exposes NextMbAddress() over the macroblock map.  The map is
 * rebuilt per slice because types 3/4/5 depend on the slice header's
 * slice_group_change_cycle; for a conforming picture every slice derives the
 * same map, but rebuilding keeps malformed change cycles isolated to the
 * slice that carried them.
 *
 * This file contains only spec-defined arithmetic over small byte maps and
 * does not depend on the macroblock parser.
 */

#include "../syntax/avc_pps.hpp"
#include "../syntax/avc_slice_header.hpp"
#include "../syntax/avc_sps.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace bs {
namespace avc {

struct SliceGroupMaps {
    std::vector<std::uint8_t> map_unit_to_group;
    std::vector<std::uint8_t> mb_to_group;
};

namespace detail {

inline bool fmo_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

} // namespace detail

/*
 * 8.2.2: derive MapUnitToSliceGroupMap, then 8.2.2.8: convert it to
 * MbToSliceGroupMap.  `pic_size_in_mbs` is the picture size already accepted
 * by the caller; the derived size must match it exactly.
 */
[[nodiscard]]
inline bool derive_mb_to_slice_group_map(
        const SequenceParameterSet& sps,
        const PictureParameterSet& pps,
        const SliceHeader& header,
        bool mbaff_frame,
        std::uint32_t pic_size_in_mbs,
        SliceGroupMaps& maps,
        std::string* error = nullptr
) {
    maps = SliceGroupMaps{};

    const std::uint64_t width =
            static_cast<std::uint64_t>(sps.pic_width_in_mbs_minus1) + 1u;
    const std::uint64_t height_map_units =
            static_cast<std::uint64_t>(sps.pic_height_in_map_units_minus1) + 1u;
    const std::uint64_t pic_size_in_map_units = width * height_map_units;

    if (width == 0 || height_map_units == 0 ||
        pic_size_in_map_units > std::numeric_limits<std::uint32_t>::max()) {
        return detail::fmo_error(error, "slice group map: picture is too large");
    }

    const std::uint64_t frame_mbs =
            pic_size_in_map_units * (sps.frame_mbs_only_flag ? 1u : 2u);
    const std::uint64_t expected_mbs = header.field_pic_flag
                                               ? pic_size_in_map_units
                                               : frame_mbs;
    if (expected_mbs != pic_size_in_mbs || pic_size_in_mbs == 0) {
        return detail::fmo_error(error, "slice group map: picture size mismatch");
    }

    const std::uint32_t units = static_cast<std::uint32_t>(pic_size_in_map_units);
    const std::uint8_t num_groups = pps.num_slice_groups();

    maps.map_unit_to_group.assign(units, 0);
    auto& unit_map = maps.map_unit_to_group;

    if (pps.num_slice_groups_minus1 == 0) {
        maps.mb_to_group.assign(pic_size_in_mbs, 0);
        return true;
    }

    const SliceGroupMap& map = pps.slice_group_map;
    const std::uint32_t groups_minus1 = pps.num_slice_groups_minus1;

    switch (map.slice_group_map_type) {
        case 0: {
            /* 8.2.2.1, equation 8-18. */
            std::uint32_t i = 0;
            do {
                for (std::uint32_t group = 0;
                     group <= groups_minus1 && i < units; ++group) {
                    const std::uint32_t run = map.run_length_minus1[group];
                    const std::uint64_t end =
                            static_cast<std::uint64_t>(i) + run + 1u;
                    for (std::uint32_t j = 0; j <= run && i + j < units; ++j) {
                        unit_map[i + j] = static_cast<std::uint8_t>(group);
                    }
                    i = end > units ? units : static_cast<std::uint32_t>(end);
                }
            } while (i < units);
            break;
        }

        case 1: {
            /* 8.2.2.2, equation 8-19: dispersed map. */
            for (std::uint32_t i = 0; i < units; ++i) {
                const std::uint32_t x = i % static_cast<std::uint32_t>(width);
                const std::uint32_t y = i / static_cast<std::uint32_t>(width);
                unit_map[i] = static_cast<std::uint8_t>(
                        (x + (y * num_groups) / 2u) % num_groups
                );
            }
            break;
        }

        case 2: {
            /* 8.2.2.3, equation 8-20: foreground with left-over. */
            for (std::uint32_t i = 0; i < units; ++i) {
                unit_map[i] = static_cast<std::uint8_t>(num_groups - 1u);
            }
            for (std::int32_t group = static_cast<std::int32_t>(groups_minus1) - 1;
                 group >= 0; --group) {
                const std::uint32_t top_left = map.top_left[static_cast<std::size_t>(group)];
                const std::uint32_t bottom_right =
                        map.bottom_right[static_cast<std::size_t>(group)];
                if (top_left >= units || bottom_right >= units ||
                    top_left > bottom_right) {
                    return detail::fmo_error(
                            error, "slice group map: foreground rectangle out of range"
                    );
                }
                const std::uint32_t y_top = top_left / static_cast<std::uint32_t>(width);
                const std::uint32_t x_left = top_left % static_cast<std::uint32_t>(width);
                const std::uint32_t y_bottom =
                        bottom_right / static_cast<std::uint32_t>(width);
                const std::uint32_t x_right =
                        bottom_right % static_cast<std::uint32_t>(width);
                if (x_left > x_right || y_top > y_bottom) {
                    return detail::fmo_error(
                            error, "slice group map: foreground rectangle is inverted"
                    );
                }
                for (std::uint32_t y = y_top; y <= y_bottom; ++y) {
                    for (std::uint32_t x = x_left; x <= x_right; ++x) {
                        unit_map[static_cast<std::size_t>(y) * width + x] =
                                static_cast<std::uint8_t>(group);
                    }
                }
            }
            break;
        }

        case 3:
        case 4:
        case 5: {
            /* 8.2.2.4-8.2.2.6 only define the two-group maps. */
            if (pps.num_slice_groups_minus1 != 1) {
                return detail::fmo_error(
                        error, "slice group map: types 3-5 require two groups"
                );
            }

            const std::uint32_t rate = map.slice_group_change_rate_minus1 + 1u;
            const std::uint64_t scaled =
                    static_cast<std::uint64_t>(rate) * header.slice_group_change_cycle;
            const std::uint32_t map_units_group0 = static_cast<std::uint32_t>(
                    scaled < units ? scaled : units
            );
            const bool direction = map.slice_group_change_direction_flag;

            if (map.slice_group_map_type == 3) {
                /* Box-out, equation 8-21. */
                for (std::uint32_t i = 0; i < units; ++i) {
                    unit_map[i] = 1;
                }

                const std::int32_t pic_width = static_cast<std::int32_t>(width);
                const std::int32_t pic_height = static_cast<std::int32_t>(height_map_units);
                const std::int32_t dir = direction ? 1 : 0;

                std::int32_t x = (pic_width - dir) / 2;
                std::int32_t y = (pic_height - dir) / 2;
                std::int32_t left_bound = x;
                std::int32_t top_bound = y;
                std::int32_t right_bound = x;
                std::int32_t bottom_bound = y;
                std::int32_t x_dir = dir - 1;
                std::int32_t y_dir = dir;

                std::uint32_t filled = 0;
                /*
                 * The spiral visits a growing rectangle.  The guard bounds a
                 * malformed map to a finite amount of work; a valid map fills
                 * exactly map_units_group0 cells.
                 */
                std::uint64_t guard = 0;
                const std::uint64_t guard_limit =
                        static_cast<std::uint64_t>(units) * 4u + 16u;
                while (filled < map_units_group0 && guard++ < guard_limit) {
                    const std::int32_t line = y * pic_width + x;
                    if (line >= 0 && static_cast<std::uint32_t>(line) < units &&
                        unit_map[static_cast<std::size_t>(line)] == 1) {
                        unit_map[static_cast<std::size_t>(line)] = 0;
                        ++filled;
                    }

                    if (x_dir == -1 && x == left_bound) {
                        left_bound = left_bound > 0 ? left_bound - 1 : 0;
                        x = left_bound;
                        x_dir = 0;
                        y_dir = 2 * dir - 1;
                    } else if (x_dir == 1 && x == right_bound) {
                        right_bound =
                                right_bound + 1 < pic_width ? right_bound + 1 : pic_width - 1;
                        x = right_bound;
                        x_dir = 0;
                        y_dir = 1 - 2 * dir;
                    } else if (y_dir == -1 && y == top_bound) {
                        top_bound = top_bound > 0 ? top_bound - 1 : 0;
                        y = top_bound;
                        x_dir = 1 - 2 * dir;
                        y_dir = 0;
                    } else if (y_dir == 1 && y == bottom_bound) {
                        bottom_bound = bottom_bound + 1 < pic_height
                                               ? bottom_bound + 1
                                               : pic_height - 1;
                        y = bottom_bound;
                        x_dir = 2 * dir - 1;
                        y_dir = 0;
                    } else {
                        x += x_dir;
                        y += y_dir;
                    }
                }
                if (filled != map_units_group0) {
                    return detail::fmo_error(
                            error, "slice group map: box-out did not converge"
                    );
                }
            } else if (map.slice_group_map_type == 4) {
                /* Raster scan, equation 8-22. */
                const std::uint32_t upper_left = direction
                                                         ? units - map_units_group0
                                                         : map_units_group0;
                for (std::uint32_t i = 0; i < units; ++i) {
                    unit_map[i] = static_cast<std::uint8_t>(
                            i < upper_left ? (direction ? 1u : 0u) : (direction ? 0u : 1u)
                    );
                }
            } else {
                /* Wipe, equation 8-23. */
                const std::uint32_t upper_left = direction
                                                         ? units - map_units_group0
                                                         : map_units_group0;
                std::uint32_t k = 0;
                for (std::uint32_t column = 0; column < width; ++column) {
                    for (std::uint32_t row = 0; row < height_map_units; ++row) {
                        const std::uint32_t index =
                                row * static_cast<std::uint32_t>(width) + column;
                        unit_map[index] = static_cast<std::uint8_t>(
                                k++ < upper_left ? (direction ? 1u : 0u)
                                                 : (direction ? 0u : 1u)
                        );
                    }
                }
            }
            break;
        }

        case 6: {
            /* 8.2.2.7, equation 8-24: explicit map. */
            if (map.slice_group_id.size() != units ||
                map.pic_size_in_map_units_minus1 + 1u != units) {
                return detail::fmo_error(
                        error, "slice group map: explicit map size mismatch"
                );
            }
            for (std::uint32_t i = 0; i < units; ++i) {
                if (map.slice_group_id[i] >= num_groups) {
                    return detail::fmo_error(
                            error, "slice group map: explicit group id out of range"
                    );
                }
                unit_map[i] = map.slice_group_id[i];
            }
            break;
        }

        default:
            return detail::fmo_error(error, "slice group map: unsupported type");
    }

    /* 8.2.2.8, equations 8-25 to 8-27. */
    maps.mb_to_group.assign(pic_size_in_mbs, 0);
    auto& mb_map = maps.mb_to_group;
    const std::uint32_t pic_width = static_cast<std::uint32_t>(width);

    if (sps.frame_mbs_only_flag || header.field_pic_flag) {
        for (std::uint32_t i = 0; i < pic_size_in_mbs; ++i) {
            mb_map[i] = unit_map[i];
        }
    } else if (mbaff_frame) {
        for (std::uint32_t i = 0; i < pic_size_in_mbs; ++i) {
            mb_map[i] = unit_map[i / 2u];
        }
    } else {
        for (std::uint32_t i = 0; i < pic_size_in_mbs; ++i) {
            mb_map[i] = unit_map[(i / (2u * pic_width)) * pic_width +
                                 (i % pic_width)];
        }
    }

    return true;
}

/* 8.2.2, equation 8-17: next macroblock address in the same slice group. */
[[nodiscard]]
inline std::uint32_t next_mb_address(
        const std::vector<std::uint8_t>& mb_to_group,
        std::uint32_t mb_addr,
        std::uint8_t slice_group
) noexcept {
    const std::uint32_t count = static_cast<std::uint32_t>(mb_to_group.size());
    std::uint32_t i = mb_addr + 1u;
    while (i < count && mb_to_group[i] != slice_group) {
        ++i;
    }
    return i;
}

} // namespace avc
} // namespace bs
