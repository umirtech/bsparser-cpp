// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "vvc_pps.hpp"

#include <cstdint>
#include <vector>
#include <type_traits>

namespace bs {
namespace vvc {

namespace detail_pps {

inline unsigned ceil_log2_pps(std::uint32_t v) noexcept {
    unsigned bits = 0;
    std::uint32_t p = 1;
    while (p < v) {
        p <<= 1;
        ++bits;
    }
    return bits;
}

// generic has_more_rbsp_data : works with RbspReader and RbspBitstreamReader
template <typename Reader>
inline bool has_more_rbsp_data_generic(Reader& r) {
    if constexpr (requires { r.more_rbsp_data(); }) {
        return r.more_rbsp_data();
    } else {
        if (!r.has_more_bits())
            return false;
        Reader tmp = r;
        bool first = false;
        try {
            first = tmp.read_bit();
        } catch (...) {
            return false;
        }
        if (!first)
            return true;
        while (!tmp.byte_aligned()) {
            if (!tmp.has_more_bits())
                break;
            bool b = false;
            try {
                b = tmp.read_bit();
            } catch (...) {
                return false;
            }
            if (b)
                return true;
        }
        if (tmp.has_more_bits())
            return true;
        return false;
    }
}

template <typename Reader>
inline void byte_align_generic(Reader& r) noexcept {
    if constexpr (requires { r.align_to_byte(); }) {
        r.align_to_byte();
    } else {
        r.byte_align();
    }
}

template <typename Reader>
inline void parse_extension_data_generic(Reader& r) {
    while (has_more_rbsp_data_generic(r)) {
        (void)r.read_bit();
    }
}

}  // namespace detail_pps

/*
 * -----------------------------------------------------------
 * VVC PPS parser (H.266 §7.3.2.5) — Full syntax
 * -----------------------------------------------------------
 * Mirrors ffmpeg cbs_h266 pps() ~lines 1675-2315.
 * Bit-accurate: every ue(v)/se(v)/u(n)/flag is consumed
 * with correct conditional branching.
 */
template <typename Reader>
[[nodiscard]]
inline PictureParameterSet parse_pps(Reader& r) {
    PictureParameterSet pps;

    pps.pps_id = static_cast<std::uint8_t>(r.read_bits(6));
    pps.sps_id = static_cast<std::uint8_t>(r.read_bits(4));
    pps.mixed_nalu_types_in_pic = r.read_bit();
    pps.pic_width_in_luma_samples = r.read_ue();
    pps.pic_height_in_luma_samples = r.read_ue();

    pps.conformance_window_flag = r.read_bit();
    if (pps.conformance_window_flag) {
        pps.conf_win_left_offset = r.read_ue();
        pps.conf_win_right_offset = r.read_ue();
        pps.conf_win_top_offset = r.read_ue();
        pps.conf_win_bottom_offset = r.read_ue();
    } else {
        pps.conf_win_left_offset = 0;
        pps.conf_win_right_offset = 0;
        pps.conf_win_top_offset = 0;
        pps.conf_win_bottom_offset = 0;
    }

    pps.scaling_window_explicit_signalling_flag = r.read_bit();
    if (pps.scaling_window_explicit_signalling_flag) {
        pps.scaling_win_left_offset = r.read_se();
        pps.scaling_win_right_offset = r.read_se();
        pps.scaling_win_top_offset = r.read_se();
        pps.scaling_win_bottom_offset = r.read_se();
    } else {
        pps.scaling_win_left_offset = static_cast<std::int32_t>(pps.conf_win_left_offset);
        pps.scaling_win_right_offset = static_cast<std::int32_t>(pps.conf_win_right_offset);
        pps.scaling_win_top_offset = static_cast<std::int32_t>(pps.conf_win_top_offset);
        pps.scaling_win_bottom_offset = static_cast<std::int32_t>(pps.conf_win_bottom_offset);
    }

    pps.output_flag_present_flag = r.read_bit();
    pps.no_pic_partition_flag = r.read_bit();
    pps.subpic_id_mapping_present_flag = r.read_bit();

    if (pps.subpic_id_mapping_present_flag) {
        if (!pps.no_pic_partition_flag) {
            pps.num_subpics_minus1 = r.read_ue();
        } else {
            pps.num_subpics_minus1 = 0;
        }
        pps.subpic_id_len_minus1 = static_cast<std::uint8_t>(r.read_ue());
        pps.subpic_id.clear();
        pps.subpic_id.reserve(pps.num_subpics_minus1 + 1);
        unsigned bits = static_cast<unsigned>(pps.subpic_id_len_minus1) + 1;
        if (bits > 32)
            bits = 32;
        for (std::uint32_t i = 0; i <= pps.num_subpics_minus1; ++i) {
            std::uint32_t id = r.read_bits(bits);
            pps.subpic_id.push_back(id);
        }
    } else {
        pps.num_subpics_minus1 = 0;
        pps.subpic_id_len_minus1 = 0;
        pps.subpic_id.clear();
    }

    // ---- tiles / slices ----
    if (!pps.no_pic_partition_flag) {
        // pps_log2_ctu_size_minus5 is constrained to SPS value but we still parse
        pps.log2_ctu_size_minus5 = static_cast<std::uint8_t>(r.read_bits(2));
        pps.num_exp_tile_columns_minus1 = r.read_ue();
        pps.num_exp_tile_rows_minus1 = r.read_ue();

        // cap for storage (spec max ~20, but allow larger truncated)
        if (pps.num_exp_tile_columns_minus1 > 63)
            pps.num_exp_tile_columns_minus1 = 63;
        if (pps.num_exp_tile_rows_minus1 > 63)
            pps.num_exp_tile_rows_minus1 = 63;

        pps.tile_column_width_minus1.clear();
        pps.tile_column_width_minus1.reserve(pps.num_exp_tile_columns_minus1 + 1);
        for (std::uint32_t i = 0; i <= pps.num_exp_tile_columns_minus1; ++i) {
            std::uint32_t v = r.read_ue();
            pps.tile_column_width_minus1.push_back(v);
        }
        pps.tile_row_height_minus1.clear();
        pps.tile_row_height_minus1.reserve(pps.num_exp_tile_rows_minus1 + 1);
        for (std::uint32_t i = 0; i <= pps.num_exp_tile_rows_minus1; ++i) {
            std::uint32_t v = r.read_ue();
            pps.tile_row_height_minus1.push_back(v);
        }

        // Derive NumTileColumns / Rows like ffmpeg
        std::uint32_t ctb_size = std::uint32_t{1} << (pps.log2_ctu_size_minus5 + 5);
        std::uint32_t pic_width_in_ctbs = (pps.pic_width_in_luma_samples + ctb_size - 1) / ctb_size;
        std::uint32_t pic_height_in_ctbs =
            (pps.pic_height_in_luma_samples + ctb_size - 1) / ctb_size;
        if (pic_width_in_ctbs == 0)
            pic_width_in_ctbs = 1;
        if (pic_height_in_ctbs == 0)
            pic_height_in_ctbs = 1;

        pps.col_width_val.clear();
        pps.row_height_val.clear();

        // columns
        {
            std::uint32_t remaining = pic_width_in_ctbs;
            std::uint32_t i = 0;
            for (; i <= pps.num_exp_tile_columns_minus1; ++i) {
                std::uint32_t w = pps.tile_column_width_minus1[i] + 1;
                if (w > remaining)
                    w = remaining;  // guard malformed
                pps.col_width_val.push_back(w);
                if (remaining >= w)
                    remaining -= w;
                else
                    remaining = 0;
            }
            std::uint32_t unified = 0;
            if (!pps.tile_column_width_minus1.empty())
                unified = pps.tile_column_width_minus1.back() + 1;
            else
                unified = 1;
            if (unified == 0)
                unified = 1;
            while (remaining > 0) {
                std::uint32_t w = remaining < unified ? remaining : unified;
                pps.col_width_val.push_back(w);
                remaining -= w;
            }
            pps.num_tile_columns = static_cast<std::uint32_t>(pps.col_width_val.size());
        }
        // rows
        {
            std::uint32_t remaining = pic_height_in_ctbs;
            std::uint32_t i = 0;
            for (; i <= pps.num_exp_tile_rows_minus1; ++i) {
                std::uint32_t h = pps.tile_row_height_minus1[i] + 1;
                if (h > remaining)
                    h = remaining;
                pps.row_height_val.push_back(h);
                if (remaining >= h)
                    remaining -= h;
                else
                    remaining = 0;
            }
            std::uint32_t unified = 0;
            if (!pps.tile_row_height_minus1.empty())
                unified = pps.tile_row_height_minus1.back() + 1;
            else
                unified = 1;
            if (unified == 0)
                unified = 1;
            while (remaining > 0) {
                std::uint32_t h = remaining < unified ? remaining : unified;
                pps.row_height_val.push_back(h);
                remaining -= h;
            }
            pps.num_tile_rows = static_cast<std::uint32_t>(pps.row_height_val.size());
        }
        pps.num_tiles_in_pic = pps.num_tile_columns * pps.num_tile_rows;

        if (pps.num_tiles_in_pic > 1) {
            pps.loop_filter_across_tiles_enabled_flag = r.read_bit();
            pps.rect_slice_flag = r.read_bit();
        } else {
            pps.loop_filter_across_tiles_enabled_flag = false;
            pps.rect_slice_flag = true;
        }

        if (pps.rect_slice_flag) {
            pps.single_slice_per_subpic_flag = r.read_bit();
        } else {
            pps.single_slice_per_subpic_flag = true;  // inferred per ffmpeg when !rect
        }

        if (pps.rect_slice_flag && !pps.single_slice_per_subpic_flag) {
            pps.num_slices_in_pic_minus1 = r.read_ue();
            if (pps.num_slices_in_pic_minus1 > 599)
                pps.num_slices_in_pic_minus1 = 599;  // VVC_MAX_SLICES-1

            if (pps.num_slices_in_pic_minus1 > 1)
                pps.tile_idx_delta_present_flag = r.read_bit();
            else
                pps.tile_idx_delta_present_flag = false;

            // Prepare slice arrays size num_slices_minus1+1 (but we only loop to minus1-1 for delta
            // etc)
            std::uint32_t num_slices = pps.num_slices_in_pic_minus1 + 1;
            pps.slice_width_in_tiles_minus1.assign(num_slices, 0);
            pps.slice_height_in_tiles_minus1.assign(num_slices, 0);
            pps.num_exp_slices_in_tile.assign(num_slices, 0);
            pps.exp_slice_height_in_ctus_minus1.assign(num_slices, {});
            pps.tile_idx_delta_val.assign(num_slices, 0);
            pps.slice_top_left_tile_idx.assign(num_slices, 0);
            pps.num_slices_in_tile.assign(num_slices, 1);
            pps.slice_height_in_ctus.assign(num_slices, 0);

            // Need slice_top_left_ctu coords for accounting (not stored beyond loop)
            std::vector<std::uint32_t> slice_top_left_ctu_x(num_slices, 0);
            std::vector<std::uint32_t> slice_top_left_ctu_y(num_slices, 0);

            std::uint32_t tile_idx = 0;
            for (std::uint32_t i = 0; i < pps.num_slices_in_pic_minus1;) {
                // track tile position
                std::uint32_t tile_x = pps.num_tile_columns ? tile_idx % pps.num_tile_columns : 0;
                std::uint32_t tile_y = pps.num_tile_columns ? tile_idx / pps.num_tile_columns : 0;

                pps.slice_top_left_tile_idx[i] = tile_idx;

                if (tile_x != pps.num_tile_columns - 1) {
                    pps.slice_width_in_tiles_minus1[i] = r.read_ue();
                } else {
                    pps.slice_width_in_tiles_minus1[i] = 0;
                }

                if (tile_y != pps.num_tile_rows - 1 &&
                    (pps.tile_idx_delta_present_flag || tile_x == 0)) {
                    pps.slice_height_in_tiles_minus1[i] = r.read_ue();
                } else {
                    if (tile_y == pps.num_tile_rows - 1)
                        pps.slice_height_in_tiles_minus1[i] = 0;
                    else if (i > 0)
                        pps.slice_height_in_tiles_minus1[i] =
                            pps.slice_height_in_tiles_minus1[i - 1];
                    else
                        pps.slice_height_in_tiles_minus1[i] = 0;
                }

                // compute ctu_x, ctu_y for this tile
                std::uint32_t ctu_x = 0, ctu_y = 0;
                for (std::uint32_t j = 0; j < tile_x && j < pps.col_width_val.size(); ++j)
                    ctu_x += pps.col_width_val[j];
                for (std::uint32_t j = 0; j < tile_y && j < pps.row_height_val.size(); ++j)
                    ctu_y += pps.row_height_val[j];

                if (pps.slice_width_in_tiles_minus1[i] == 0 &&
                    pps.slice_height_in_tiles_minus1[i] == 0 &&
                    tile_y < pps.row_height_val.size() && pps.row_height_val[tile_y] > 1) {
                    // extra slices in tile
                    pps.num_exp_slices_in_tile[i] = r.read_ue();
                    if (pps.num_exp_slices_in_tile[i] > pps.row_height_val[tile_y] - 1)
                        pps.num_exp_slices_in_tile[i] = pps.row_height_val[tile_y] - 1;

                    if (pps.num_exp_slices_in_tile[i] == 0) {
                        pps.num_slices_in_tile[i] = 1;
                        pps.slice_height_in_ctus[i] = pps.row_height_val[tile_y];
                        slice_top_left_ctu_x[i] = ctu_x;
                        slice_top_left_ctu_y[i] = ctu_y;
                    } else {
                        // need to read exp heights
                        std::uint32_t remaining_h = pps.row_height_val[tile_y];
                        pps.exp_slice_height_in_ctus_minus1[i].clear();
                        pps.exp_slice_height_in_ctus_minus1[i].reserve(
                            pps.num_exp_slices_in_tile[i]
                        );
                        std::uint32_t ctu_y_tmp = ctu_y;
                        std::uint32_t j = 0;
                        for (; j < pps.num_exp_slices_in_tile[i]; ++j) {
                            std::uint32_t v = r.read_ue();
                            pps.exp_slice_height_in_ctus_minus1[i].push_back(v);
                            std::uint32_t h = v + 1;
                            std::uint32_t idx = i + j;
                            if (idx < num_slices) {
                                pps.slice_height_in_ctus[idx] = h;
                                slice_top_left_ctu_x[idx] = ctu_x;
                                slice_top_left_ctu_y[idx] = ctu_y_tmp;
                            }
                            if (remaining_h >= h)
                                remaining_h -= h;
                            else
                                remaining_h = 0;
                            ctu_y_tmp += h;
                        }
                        std::uint32_t uniform = 1;
                        if (!pps.exp_slice_height_in_ctus_minus1[i].empty())
                            uniform = pps.exp_slice_height_in_ctus_minus1[i].back() + 1;
                        else if (pps.row_height_val[tile_y] > 0)
                            uniform = pps.row_height_val[tile_y];
                        if (uniform == 0)
                            uniform = 1;
                        std::uint32_t j_off = j;
                        // uniform slices
                        while (remaining_h > uniform) {
                            std::uint32_t idx = i + j_off;
                            if (idx < num_slices) {
                                pps.slice_height_in_ctus[idx] = uniform;
                                slice_top_left_ctu_x[idx] = ctu_x;
                                slice_top_left_ctu_y[idx] = ctu_y_tmp;
                            }
                            ctu_y_tmp += uniform;
                            remaining_h -= uniform;
                            ++j_off;
                        }
                        if (remaining_h > 0) {
                            std::uint32_t idx = i + j_off;
                            if (idx < num_slices) {
                                pps.slice_height_in_ctus[idx] = remaining_h;
                                slice_top_left_ctu_x[idx] = ctu_x;
                                slice_top_left_ctu_y[idx] = ctu_y_tmp;
                            }
                            ++j_off;
                        }
                        pps.num_slices_in_tile[i] = j_off;
                        // mark tile idx for those slices
                        for (std::uint32_t k = 0; k < pps.num_slices_in_tile[i]; ++k) {
                            if (i + k < num_slices)
                                pps.slice_top_left_tile_idx[i + k] = tile_idx;
                        }
                        // skip ahead
                        std::uint32_t consumed = pps.num_slices_in_tile[i];
                        // read delta for the last of the group? spec handles delta per i in loop;
                        // we need to handle delta after this group before next iteration
                        // Advance i by consumed-1; the for loop will handle delta reading below
                        // but we need to ensure delta is read for each i < num_slices_minus1
                        // We'll do delta handling after group
                        i += consumed - 1;
                        // delta for old_i..i if needed? Only one delta per logical slice entry
                        // except last group? Simplified: handle tile_idx update for the last
                        // processed entry
                        if (i < pps.num_slices_in_pic_minus1) {
                            if (pps.tile_idx_delta_present_flag) {
                                pps.tile_idx_delta_val[i] = r.read_se();
                                // prevent zero delta error but still consume
                                tile_idx = static_cast<std::uint32_t>(
                                    static_cast<std::int32_t>(tile_idx) + pps.tile_idx_delta_val[i]
                                );
                            } else {
                                pps.tile_idx_delta_val[i] = 0;
                                tile_idx += pps.slice_width_in_tiles_minus1[i] + 1;
                                if (pps.num_tile_columns && tile_idx % pps.num_tile_columns == 0) {
                                    tile_idx +=
                                        pps.slice_height_in_tiles_minus1[i] * pps.num_tile_columns;
                                }
                            }
                        }
                        // move to next slice
                        ++i;
                        continue;
                    }
                    // mark tile idx for slices in tile (when 0 exp case already done, otherwise
                    // loop above did)
                    for (std::uint32_t k = 0; k < pps.num_slices_in_tile[i]; ++k) {
                        if (i + k < num_slices)
                            pps.slice_top_left_tile_idx[i + k] = tile_idx;
                    }
                    // advance i
                    std::uint32_t consumed = pps.num_slices_in_tile[i];
                    if (consumed > 1) {
                        // already handled tile idx for first, need delta after group
                        // For 0-exp case consumed==1, no extra
                        i += consumed - 1;
                    }
                } else {
                    pps.num_exp_slices_in_tile[i] = 0;
                    pps.exp_slice_height_in_ctus_minus1[i].clear();
                    pps.num_slices_in_tile[i] = 1;
                    // compute height across tiles
                    std::uint32_t h = 0;
                    for (std::uint32_t j = 0; j <= pps.slice_height_in_tiles_minus1[i]; ++j) {
                        if (tile_y + j < pps.row_height_val.size())
                            h += pps.row_height_val[tile_y + j];
                    }
                    pps.slice_height_in_ctus[i] = h;
                    slice_top_left_ctu_x[i] = ctu_x;
                    slice_top_left_ctu_y[i] = ctu_y;
                }

                if (i < pps.num_slices_in_pic_minus1) {
                    if (pps.tile_idx_delta_present_flag) {
                        pps.tile_idx_delta_val[i] = r.read_se();
                        tile_idx = static_cast<std::uint32_t>(
                            static_cast<std::int32_t>(tile_idx) + pps.tile_idx_delta_val[i]
                        );
                    } else {
                        pps.tile_idx_delta_val[i] = 0;
                        tile_idx += pps.slice_width_in_tiles_minus1[i] + 1;
                        if (pps.num_tile_columns && tile_idx % pps.num_tile_columns == 0) {
                            tile_idx += pps.slice_height_in_tiles_minus1[i] * pps.num_tile_columns;
                        }
                    }
                }
                ++i;
            }
            // last slice (i == num_slices_minus1)
            {
                std::uint32_t i = pps.num_slices_in_pic_minus1;
                if (i < num_slices) {
                    std::uint32_t tile_x =
                        pps.num_tile_columns ? tile_idx % pps.num_tile_columns : 0;
                    std::uint32_t tile_y =
                        pps.num_tile_columns ? tile_idx / pps.num_tile_columns : 0;
                    pps.slice_top_left_tile_idx[i] = tile_idx;
                    pps.num_slices_in_tile[i] = 1;
                    pps.num_exp_slices_in_tile[i] = 0;
                    pps.exp_slice_height_in_ctus_minus1[i].clear();
                    // inferred widths/heights for last slice
                    if (pps.num_tile_columns) {
                        pps.slice_width_in_tiles_minus1[i] = pps.num_tile_columns - tile_x - 1;
                    } else {
                        pps.slice_width_in_tiles_minus1[i] = 0;
                    }
                    if (pps.num_tile_rows) {
                        pps.slice_height_in_tiles_minus1[i] = pps.num_tile_rows - tile_y - 1;
                    } else {
                        pps.slice_height_in_tiles_minus1[i] = 0;
                    }
                    std::uint32_t h = 0;
                    for (std::uint32_t j = 0; j <= pps.slice_height_in_tiles_minus1[i]; ++j) {
                        if (tile_y + j < pps.row_height_val.size())
                            h += pps.row_height_val[tile_y + j];
                    }
                    pps.slice_height_in_ctus[i] = h;
                    std::uint32_t ctu_x = 0, ctu_y = 0;
                    for (std::uint32_t j = 0; j < tile_x && j < pps.col_width_val.size(); ++j)
                        ctu_x += pps.col_width_val[j];
                    for (std::uint32_t j = 0; j < tile_y && j < pps.row_height_val.size(); ++j)
                        ctu_y += pps.row_height_val[j];
                    slice_top_left_ctu_x[i] = ctu_x;
                    slice_top_left_ctu_y[i] = ctu_y;
                }
            }
        } else {
            // !rect || single
            if (pps.no_pic_partition_flag) {
                pps.num_slices_in_pic_minus1 = 0;
            } else if (pps.single_slice_per_subpic_flag) {
                // infer from SPS num_subpics, but without SPS we infer 0 -> will be overwritten if
                // SPS known? For now infer 0; actual value is sps_num_subpics_minus1 which we don't
                // have Keep 0 or try to use num_subpics from subpic mapping if present
                if (pps.subpic_id_mapping_present_flag) {
                    pps.num_slices_in_pic_minus1 = pps.num_subpics_minus1;
                } else {
                    pps.num_slices_in_pic_minus1 = 0;
                }
            }
            pps.tile_idx_delta_present_flag = false;
            // allocate single slice arrays
            std::uint32_t num_slices = pps.num_slices_in_pic_minus1 + 1;
            pps.slice_width_in_tiles_minus1.assign(num_slices, 0);
            pps.slice_height_in_tiles_minus1.assign(num_slices, 0);
            pps.num_exp_slices_in_tile.assign(num_slices, 0);
            pps.exp_slice_height_in_ctus_minus1.assign(num_slices, {});
            pps.tile_idx_delta_val.assign(num_slices, 0);
            pps.slice_top_left_tile_idx.assign(num_slices, 0);
            pps.num_slices_in_tile.assign(num_slices, 1);
            pps.slice_height_in_ctus.assign(num_slices, 0);
            // when single_slice_per_subpic, widths are subpic sizes – not needed for parsing
        }

        if (!pps.rect_slice_flag || pps.single_slice_per_subpic_flag ||
            pps.num_slices_in_pic_minus1 > 0) {
            pps.loop_filter_across_slices_enabled_flag = r.read_bit();
        } else {
            pps.loop_filter_across_slices_enabled_flag = false;
        }
    } else {
        // no_pic_partition_flag == 1
        pps.log2_ctu_size_minus5 = 0;
        pps.num_exp_tile_columns_minus1 = 0;
        pps.num_exp_tile_rows_minus1 = 0;
        pps.tile_column_width_minus1.clear();
        pps.tile_row_height_minus1.clear();
        // implied single tile covering picture
        // compute ctb size? without SPS we use 32 as default (log2 5)
        // Use pps log2? In this branch log2 is inferred from SPS, but we set 0 => 32
        std::uint32_t ctb_size = 32;  // fallback
        std::uint32_t pic_width_in_ctbs = (pps.pic_width_in_luma_samples + ctb_size - 1) / ctb_size;
        std::uint32_t pic_height_in_ctbs =
            (pps.pic_height_in_luma_samples + ctb_size - 1) / ctb_size;
        if (pic_width_in_ctbs == 0)
            pic_width_in_ctbs = 1;
        if (pic_height_in_ctbs == 0)
            pic_height_in_ctbs = 1;
        pps.tile_column_width_minus1.push_back(pic_width_in_ctbs - 1);
        pps.tile_row_height_minus1.push_back(pic_height_in_ctbs - 1);
        pps.col_width_val = {pic_width_in_ctbs};
        pps.row_height_val = {pic_height_in_ctbs};
        pps.num_tile_columns = 1;
        pps.num_tile_rows = 1;
        pps.num_tiles_in_pic = 1;
        pps.loop_filter_across_tiles_enabled_flag = false;
        pps.rect_slice_flag = true;
        pps.single_slice_per_subpic_flag = true;
        pps.num_slices_in_pic_minus1 = 0;
        pps.tile_idx_delta_present_flag = false;
        pps.slice_width_in_tiles_minus1 = {0};
        pps.slice_height_in_tiles_minus1 = {0};
        pps.num_exp_slices_in_tile = {0};
        pps.exp_slice_height_in_ctus_minus1 = {{}};
        pps.tile_idx_delta_val = {0};
        pps.slice_top_left_tile_idx = {0};
        pps.num_slices_in_tile = {1};
        pps.slice_height_in_ctus = {pic_height_in_ctbs};
        pps.loop_filter_across_slices_enabled_flag = false;
    }

    pps.cabac_init_present_flag = r.read_bit();
    for (int i = 0; i < 2; ++i) {
        pps.num_ref_idx_default_active_minus1[i] = r.read_ue();
        if (pps.num_ref_idx_default_active_minus1[i] > 14)
            pps.num_ref_idx_default_active_minus1[i] = 14;
    }
    pps.rpl1_idx_present_flag = r.read_bit();
    pps.weighted_pred_flag = r.read_bit();
    pps.weighted_bipred_flag = r.read_bit();
    pps.ref_wraparound_enabled_flag = r.read_bit();
    if (pps.ref_wraparound_enabled_flag) {
        pps.pic_width_minus_wraparound_offset = r.read_ue();
    } else {
        pps.pic_width_minus_wraparound_offset = 0;
    }

    pps.init_qp_minus26 = r.read_se();
    pps.cu_qp_delta_enabled_flag = r.read_bit();
    pps.chroma_tool_offsets_present_flag = r.read_bit();
    if (pps.chroma_tool_offsets_present_flag) {
        pps.cb_qp_offset = r.read_se();
        pps.cr_qp_offset = r.read_se();
        pps.joint_cbcr_qp_offset_present_flag = r.read_bit();
        if (pps.joint_cbcr_qp_offset_present_flag) {
            pps.joint_cbcr_qp_offset_value = r.read_se();
        } else {
            pps.joint_cbcr_qp_offset_value = 0;
        }
        pps.slice_chroma_qp_offsets_present_flag = r.read_bit();
        pps.cu_chroma_qp_offset_list_enabled_flag = r.read_bit();
        if (pps.cu_chroma_qp_offset_list_enabled_flag) {
            pps.chroma_qp_offset_list_len_minus1 = r.read_ue();
            if (pps.chroma_qp_offset_list_len_minus1 > 5)
                pps.chroma_qp_offset_list_len_minus1 = 5;
            pps.cb_qp_offset_list.clear();
            pps.cr_qp_offset_list.clear();
            pps.joint_cbcr_qp_offset_list.clear();
            pps.cb_qp_offset_list.reserve(pps.chroma_qp_offset_list_len_minus1 + 1);
            pps.cr_qp_offset_list.reserve(pps.chroma_qp_offset_list_len_minus1 + 1);
            pps.joint_cbcr_qp_offset_list.reserve(pps.chroma_qp_offset_list_len_minus1 + 1);
            for (std::uint32_t i = 0; i <= pps.chroma_qp_offset_list_len_minus1; ++i) {
                std::int32_t cb = r.read_se();
                std::int32_t cr = r.read_se();
                pps.cb_qp_offset_list.push_back(cb);
                pps.cr_qp_offset_list.push_back(cr);
                if (pps.joint_cbcr_qp_offset_present_flag) {
                    std::int32_t jcb = r.read_se();
                    pps.joint_cbcr_qp_offset_list.push_back(jcb);
                } else {
                    pps.joint_cbcr_qp_offset_list.push_back(0);
                }
            }
        } else {
            pps.chroma_qp_offset_list_len_minus1 = 0;
            pps.cb_qp_offset_list.clear();
            pps.cr_qp_offset_list.clear();
            pps.joint_cbcr_qp_offset_list.clear();
        }
    } else {
        pps.cb_qp_offset = 0;
        pps.cr_qp_offset = 0;
        pps.joint_cbcr_qp_offset_present_flag = false;
        pps.joint_cbcr_qp_offset_value = 0;
        pps.slice_chroma_qp_offsets_present_flag = false;
        pps.cu_chroma_qp_offset_list_enabled_flag = false;
        pps.chroma_qp_offset_list_len_minus1 = 0;
        pps.cb_qp_offset_list.clear();
        pps.cr_qp_offset_list.clear();
        pps.joint_cbcr_qp_offset_list.clear();
    }

    pps.deblocking_filter_control_present_flag = r.read_bit();
    if (pps.deblocking_filter_control_present_flag) {
        pps.deblocking_filter_override_enabled_flag = r.read_bit();
        pps.deblocking_filter_disabled_flag = r.read_bit();
        if (!pps.no_pic_partition_flag && pps.deblocking_filter_override_enabled_flag) {
            pps.dbf_info_in_ph_flag = r.read_bit();
        } else {
            pps.dbf_info_in_ph_flag = false;
        }
        if (!pps.deblocking_filter_disabled_flag) {
            pps.luma_beta_offset_div2 = r.read_se();
            pps.luma_tc_offset_div2 = r.read_se();
            if (pps.chroma_tool_offsets_present_flag) {
                pps.cb_beta_offset_div2 = r.read_se();
                pps.cb_tc_offset_div2 = r.read_se();
                pps.cr_beta_offset_div2 = r.read_se();
                pps.cr_tc_offset_div2 = r.read_se();
            } else {
                pps.cb_beta_offset_div2 = pps.luma_beta_offset_div2;
                pps.cb_tc_offset_div2 = pps.luma_tc_offset_div2;
                pps.cr_beta_offset_div2 = pps.luma_beta_offset_div2;
                pps.cr_tc_offset_div2 = pps.luma_tc_offset_div2;
            }
        } else {
            pps.luma_beta_offset_div2 = 0;
            pps.luma_tc_offset_div2 = 0;
            pps.cb_beta_offset_div2 = 0;
            pps.cb_tc_offset_div2 = 0;
            pps.cr_beta_offset_div2 = 0;
            pps.cr_tc_offset_div2 = 0;
        }
    } else {
        pps.deblocking_filter_override_enabled_flag = false;
        pps.deblocking_filter_disabled_flag = false;
        pps.dbf_info_in_ph_flag = false;
        pps.luma_beta_offset_div2 = 0;
        pps.luma_tc_offset_div2 = 0;
        pps.cb_beta_offset_div2 = 0;
        pps.cb_tc_offset_div2 = 0;
        pps.cr_beta_offset_div2 = 0;
        pps.cr_tc_offset_div2 = 0;
    }

    if (!pps.no_pic_partition_flag) {
        pps.rpl_info_in_ph_flag = r.read_bit();
        pps.sao_info_in_ph_flag = r.read_bit();
        pps.alf_info_in_ph_flag = r.read_bit();
        if ((pps.weighted_pred_flag || pps.weighted_bipred_flag) && pps.rpl_info_in_ph_flag) {
            pps.wp_info_in_ph_flag = r.read_bit();
        } else {
            pps.wp_info_in_ph_flag = false;
        }
        pps.qp_delta_info_in_ph_flag = r.read_bit();
    } else {
        pps.rpl_info_in_ph_flag = false;
        pps.sao_info_in_ph_flag = false;
        pps.alf_info_in_ph_flag = false;
        pps.wp_info_in_ph_flag = false;
        pps.qp_delta_info_in_ph_flag = false;
    }

    pps.picture_header_extension_present_flag = r.read_bit();
    pps.slice_header_extension_present_flag = r.read_bit();
    pps.extension_flag = r.read_bit();
    if (pps.extension_flag) {
        detail_pps::parse_extension_data_generic(r);
    }

    // rbsp_trailing_bits
    if constexpr (requires { r.read_rbsp_trailing_bits(); }) {
        // RbspBitstreamReader
        if (detail_pps::has_more_rbsp_data_generic(r) || !r.byte_aligned()) {
            try {
                r.read_rbsp_trailing_bits();
            } catch (...) {
                // fallback manual
                try {
                    (void)r.read_bit();
                    while (!r.byte_aligned() && detail_pps::has_more_rbsp_data_generic(r))
                        (void)r.read_bit();
                    // consume remaining alignment via generic align
                    while (!r.byte_aligned()) {
                        try {
                            (void)r.read_bit();
                        } catch (...) {
                            break;
                        }
                    }
                } catch (...) {
                }
            }
        }
    } else {
        // RbspReader
        if (r.has_more_bits()) {
            try {
                (void)r.read_bit();  // stop_one_bit
                while (!r.byte_aligned() && r.has_more_bits())
                    (void)r.read_bit();
            } catch (...) {
            }
        }
    }

    return pps;
}

}  // namespace vvc
}  // namespace bs
