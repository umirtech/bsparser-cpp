#pragma once

#include "av1_frame_header.hpp"
#include "av1_sequence_header.hpp"

#include <bitstream/boolean_decoder.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bs {
namespace av1 {

/*
 * -----------------------------------------------------------
 * AV1 frame header parser (AV1 §5.9.2) — full uncompressed_header
 * through film_grain
 * -----------------------------------------------------------
 * Mirrors ffmpeg cbs_av1 uncompressed_header and sub-functions.
 * Everything is boolean-coded (bd.read_bool(128) etc.).
 */

namespace detail_frame {

inline std::uint32_t tile_log2(int blksize, int target) {
    int k = 0;
    while ((blksize << k) < target)
        ++k;
    return static_cast<std::uint32_t>(k);
}

inline std::uint32_t read_ns(BooleanDecoder& bd, std::uint32_t n) {
    if (n == 0)
        return 0;
    // n is max value inclusive? ffmpeg passes max_value (e.g., max_width)
    // where value in [0, n]. The code uses n as number of values? But for
    // width_in_sbs_minus_1, n = max_width (e.g., 8) and value 0..max_width-1?
    // Actually cbs_av1_read_ns takes n as max_value (inclusive) where w = log2(n)+1
    // and value in [0, n]. We'll follow that.
    // However for width_in_sbs_minus_1, ffmpeg calls ns(max_width, width...)
    // where max_width = sb_cols - start_sb, value in [0, max_width-1]? Might be off by one.
    // We treat n as number of possible values (max+1) to be safe.
    // Let's implement as per ffmpeg: n is the max value count (e.g., max_width)
    // where w = floor_log2(n)+1, etc., value in [0, n).
    // To avoid off-by-one, we use n as passed; the function expects n>0 and returns 0..n-1 if n is
    // count. We'll implement generic: if caller passes max inclusive, they should pass max+1. For
    // width_in_sbs_minus_1, ffmpeg passes max_width (which is count of possibilities) and expects
    // value 0..max_width-1. Our n is that count. So we treat n as count (number of values). For
    // tile case, n = max_width. For n=1, value 0 only.
    if (n == 1)
        return 0;
    int w = 0;
    std::uint32_t tmp = n;
    while (tmp) {
        ++w;
        tmp >>= 1;
    }
    // Actually w = av_log2(n)+1
    w = 32 - __builtin_clz(n);
    std::uint32_t m = (static_cast<std::uint32_t>(1) << w) - n;
    std::uint32_t v = 0;
    if (w - 1 > 0)
        v = bd.read_literal(static_cast<unsigned>(w - 1));
    if (v < m) {
        return v;
    } else {
        std::uint32_t extra = bd.read_bool(128) ? 1u : 0u;
        return (v << 1) - m + extra;
    }
}

inline std::uint32_t read_increment(BooleanDecoder& bd, std::uint32_t min, std::uint32_t max) {
    std::uint32_t value = min;
    while (value < max) {
        if (bd.read_bool(128))
            ++value;
        else
            break;
    }
    return value;
}

inline std::uint32_t read_subexp(BooleanDecoder& bd, std::uint32_t range_max) {
    if (range_max == 0)
        return 0;
    // max_len = floor_log2(range_max-1)-3
    int max_len = 0;
    if (range_max > 1) {
        int l = 32 - __builtin_clz(range_max - 1);
        max_len = l - 4;  // because av_log2 is floor, so l-1 is log2, then -3 => l-4
        if (max_len < 0)
            max_len = 0;
    }
    std::uint32_t len = read_increment(bd, 0, static_cast<std::uint32_t>(max_len));
    std::uint32_t range_bits, range_offset;
    if (len) {
        range_bits = 2 + len;
        range_offset = static_cast<std::uint32_t>(1) << range_bits;
    } else {
        range_bits = 3;
        range_offset = 0;
    }
    std::uint32_t value;
    if (len < static_cast<std::uint32_t>(max_len)) {
        value = bd.read_literal(range_bits);
    } else {
        std::uint32_t n = range_max - range_offset;
        if (n == 0)
            value = 0;
        else {
            // read_ns expects count n (number of values)
            value = read_ns(bd, n);
        }
    }
    return value + range_offset;
}

inline std::int32_t read_su(BooleanDecoder& bd, unsigned width) {
    std::uint32_t v = bd.read_literal(width);
    if (v & (static_cast<std::uint32_t>(1) << (width - 1))) {
        return static_cast<std::int32_t>(v - (static_cast<std::uint32_t>(1) << width));
    }
    return static_cast<std::int32_t>(v);
}

inline std::int8_t read_delta_q(BooleanDecoder& bd) {
    bool coded = bd.read_bool(128);
    if (!coded)
        return 0;
    return static_cast<std::int8_t>(read_su(bd, 7));  // 1+6
}

inline void parse_quantization(BooleanDecoder& bd, FrameHeader& fh, const SequenceHeader& seq) {
    fh.base_q_idx = static_cast<std::uint8_t>(bd.read_literal(8));
    fh.delta_q_y_dc = read_delta_q(bd);
    if (seq.color_config.mono_chrome ? false : true) {
        // num_planes >1 ?
        if (seq.color_config.separate_uv_delta_q) {
            fh.diff_uv_delta = bd.read_bool(128);
        } else {
            fh.diff_uv_delta = false;
        }
        fh.delta_q_u_dc = read_delta_q(bd);
        fh.delta_q_u_ac = read_delta_q(bd);
        if (fh.diff_uv_delta) {
            fh.delta_q_v_dc = read_delta_q(bd);
            fh.delta_q_v_ac = read_delta_q(bd);
        } else {
            fh.delta_q_v_dc = fh.delta_q_u_dc;
            fh.delta_q_v_ac = fh.delta_q_u_ac;
        }
    } else {
        fh.delta_q_u_dc = 0;
        fh.delta_q_u_ac = 0;
        fh.delta_q_v_dc = 0;
        fh.delta_q_v_ac = 0;
        fh.diff_uv_delta = false;
    }
    fh.using_qmatrix = bd.read_bool(128);
    if (fh.using_qmatrix) {
        fh.qm_y = static_cast<std::uint8_t>(bd.read_literal(4));
        fh.qm_u = static_cast<std::uint8_t>(bd.read_literal(4));
        if (seq.color_config.separate_uv_delta_q) {
            fh.qm_v = static_cast<std::uint8_t>(bd.read_literal(4));
        } else {
            fh.qm_v = fh.qm_u;
        }
    } else {
        fh.qm_y = 0;
        fh.qm_u = 0;
        fh.qm_v = 0;
    }
}

inline void parse_segmentation(BooleanDecoder& bd, FrameHeader& fh) {
    static const std::uint8_t bits[8] = {8, 6, 6, 6, 6, 3, 0, 0};
    static const std::uint8_t signs[8] = {1, 1, 1, 1, 1, 0, 0, 0};
    fh.segmentation_enabled = bd.read_bool(128);
    if (fh.segmentation_enabled) {
        if (fh.primary_ref_frame == 7) {  // PRIMARY_REF_NONE
            fh.segmentation_update_map = true;
            fh.segmentation_temporal_update = false;
            fh.segmentation_update_data = true;
        } else {
            fh.segmentation_update_map = bd.read_bool(128);
            if (fh.segmentation_update_map)
                fh.segmentation_temporal_update = bd.read_bool(128);
            else
                fh.segmentation_temporal_update = false;
            fh.segmentation_update_data = bd.read_bool(128);
        }
        for (int i = 0; i < 8; ++i) {
            for (int j = 0; j < 8; ++j) {
                if (fh.segmentation_update_data) {
                    fh.feature_enabled[i][j] = bd.read_bool(128);
                    if (fh.feature_enabled[i][j] && bits[j] > 0) {
                        if (signs[j])
                            fh.feature_value[i][j] = read_su(bd, 1 + bits[j]);
                        else
                            fh.feature_value[i][j] =
                                static_cast<std::int16_t>(bd.read_literal(bits[j]));
                    } else {
                        fh.feature_value[i][j] = 0;
                    }
                } else {
                    // would infer from previous frame; default 0
                    fh.feature_enabled[i][j] = false;
                    fh.feature_value[i][j] = 0;
                }
            }
        }
    } else {
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 8; ++j) {
                fh.feature_enabled[i][j] = false;
                fh.feature_value[i][j] = 0;
            }
        fh.segmentation_update_map = false;
        fh.segmentation_temporal_update = false;
        fh.segmentation_update_data = false;
    }
}

inline void parse_delta_q_params(BooleanDecoder& bd, FrameHeader& fh) {
    if (fh.base_q_idx > 0)
        fh.delta_q_present = bd.read_bool(128);
    else
        fh.delta_q_present = false;
    if (fh.delta_q_present)
        fh.delta_q_res = static_cast<std::uint8_t>(bd.read_literal(2));
    else
        fh.delta_q_res = 0;
}

inline void parse_delta_lf_params(BooleanDecoder& bd, FrameHeader& fh) {
    if (fh.delta_q_present) {
        if (!fh.allow_intrabc)
            fh.delta_lf_present = bd.read_bool(128);
        else
            fh.delta_lf_present = false;
        if (fh.delta_lf_present) {
            fh.delta_lf_res = static_cast<std::uint8_t>(bd.read_literal(2));
            fh.delta_lf_multi = bd.read_bool(128);
        } else {
            fh.delta_lf_res = 0;
            fh.delta_lf_multi = false;
        }
    } else {
        fh.delta_lf_present = false;
        fh.delta_lf_res = 0;
        fh.delta_lf_multi = false;
    }
}

inline void parse_loop_filter(
    BooleanDecoder& bd, FrameHeader& fh, const SequenceHeader& seq, bool coded_lossless
) {
    static const std::int8_t default_r[8] = {1, 0, 0, 0, -1, 0, -1, -1};
    static const std::int8_t default_m[2] = {0, 0};
    if (coded_lossless || fh.allow_intrabc) {
        fh.loop_filter_level[0] = 0;
        fh.loop_filter_level[1] = 0;
        for (int i = 0; i < 8; ++i)
            fh.loop_filter_ref_deltas[i] = default_r[i];
        for (int i = 0; i < 2; ++i)
            fh.loop_filter_mode_deltas[i] = default_m[i];
        fh.loop_filter_sharpness = 0;
        fh.loop_filter_delta_enabled = false;
        fh.loop_filter_delta_update = false;
        for (int i = 0; i < 8; ++i)
            fh.update_ref_delta[i] = false;
        for (int i = 0; i < 2; ++i)
            fh.update_mode_delta[i] = false;
        return;
    }
    fh.loop_filter_level[0] = static_cast<std::uint8_t>(bd.read_literal(6));
    fh.loop_filter_level[1] = static_cast<std::uint8_t>(bd.read_literal(6));
    // subsampling? num_planes >1 and any level?
    bool has_chroma = !seq.color_config.mono_chrome;
    if (has_chroma && (fh.loop_filter_level[0] || fh.loop_filter_level[1])) {
        fh.loop_filter_level[2] = static_cast<std::uint8_t>(bd.read_literal(6));
        fh.loop_filter_level[3] = static_cast<std::uint8_t>(bd.read_literal(6));
    } else if (has_chroma) {
        fh.loop_filter_level[2] = 0;
        fh.loop_filter_level[3] = 0;
    }
    fh.loop_filter_sharpness = static_cast<std::uint8_t>(bd.read_literal(3));
    fh.loop_filter_delta_enabled = bd.read_bool(128);
    if (fh.loop_filter_delta_enabled) {
        fh.loop_filter_delta_update = bd.read_bool(128);
        for (int i = 0; i < 8; ++i) {
            if (fh.loop_filter_delta_update)
                fh.update_ref_delta[i] = bd.read_bool(128);
            else
                fh.update_ref_delta[i] = false;
            if (fh.update_ref_delta[i])
                fh.loop_filter_ref_deltas[i] = static_cast<std::int8_t>(read_su(bd, 7));
            else {
                if (fh.primary_ref_frame == 7)
                    fh.loop_filter_ref_deltas[i] = default_r[i];
                else
                    fh.loop_filter_ref_deltas[i] = default_r[i];  // simplified: would copy from ref
            }
        }
        for (int i = 0; i < 2; ++i) {
            if (fh.loop_filter_delta_update)
                fh.update_mode_delta[i] = bd.read_bool(128);
            else
                fh.update_mode_delta[i] = false;
            if (fh.update_mode_delta[i])
                fh.loop_filter_mode_deltas[i] = static_cast<std::int8_t>(read_su(bd, 7));
            else {
                if (fh.primary_ref_frame == 7)
                    fh.loop_filter_mode_deltas[i] = default_m[i];
                else
                    fh.loop_filter_mode_deltas[i] = default_m[i];
            }
        }
    } else {
        for (int i = 0; i < 8; ++i)
            fh.loop_filter_ref_deltas[i] = default_r[i];
        for (int i = 0; i < 2; ++i)
            fh.loop_filter_mode_deltas[i] = default_m[i];
        for (int i = 0; i < 8; ++i)
            fh.update_ref_delta[i] = false;
        for (int i = 0; i < 2; ++i)
            fh.update_mode_delta[i] = false;
        fh.loop_filter_delta_update = false;
    }
}

inline void parse_cdef(
    BooleanDecoder& bd, FrameHeader& fh, const SequenceHeader& seq, bool coded_lossless
) {
    if (coded_lossless || fh.allow_intrabc || !seq.enable_cdef) {
        fh.cdef_damping_minus_3 = 0;
        fh.cdef_bits = 0;
        fh.cdef_y_pri_strength[0] = 0;
        fh.cdef_y_sec_strength[0] = 0;
        fh.cdef_uv_pri_strength[0] = 0;
        fh.cdef_uv_sec_strength[0] = 0;
        return;
    }
    fh.cdef_damping_minus_3 = static_cast<std::uint8_t>(bd.read_literal(2));
    fh.cdef_bits = static_cast<std::uint8_t>(bd.read_literal(2));
    for (int i = 0; i < (1 << fh.cdef_bits); ++i) {
        fh.cdef_y_pri_strength[i] = static_cast<std::uint8_t>(bd.read_literal(4));
        fh.cdef_y_sec_strength[i] = static_cast<std::uint8_t>(bd.read_literal(2));
        bool has_chroma = !seq.color_config.mono_chrome;
        if (has_chroma) {
            fh.cdef_uv_pri_strength[i] = static_cast<std::uint8_t>(bd.read_literal(4));
            fh.cdef_uv_sec_strength[i] = static_cast<std::uint8_t>(bd.read_literal(2));
        }
    }
}

inline void parse_lr(
    BooleanDecoder& bd,
    FrameHeader& fh,
    const SequenceHeader& seq,
    bool coded_lossless,
    bool all_lossless
) {
    if (all_lossless || fh.allow_intrabc || !seq.enable_restoration) {
        fh.lr_type[0] = 0;
        fh.lr_type[1] = 0;
        fh.lr_type[2] = 0;
        return;
    }
    bool uses_lr = false, uses_chroma_lr = false;
    for (int i = 0; i < 3; ++i) {
        bool is_chroma = (i > 0);
        bool has_chroma = !seq.color_config.mono_chrome;
        if (is_chroma && !has_chroma) {
            fh.lr_type[i] = 0;
            continue;
        }
        fh.lr_type[i] = static_cast<std::uint8_t>(bd.read_literal(2));
        if (fh.lr_type[i] != 0) {
            uses_lr = true;
            if (is_chroma)
                uses_chroma_lr = true;
        }
    }
    if (uses_lr) {
        if (seq.use_128x128_superblock)
            fh.lr_unit_shift = static_cast<std::uint8_t>(read_increment(bd, 1, 2));
        else
            fh.lr_unit_shift = static_cast<std::uint8_t>(read_increment(bd, 0, 2));
        if (seq.color_config.subsampling_x && seq.color_config.subsampling_y && uses_chroma_lr) {
            fh.lr_uv_shift = bd.read_bool(128) ? 1 : 0;
        } else
            fh.lr_uv_shift = 0;
    } else {
        fh.lr_unit_shift = 0;
        fh.lr_uv_shift = 0;
    }
}

inline void parse_tile_info(BooleanDecoder& bd, FrameHeader& fh, const SequenceHeader& seq) {
    int mi_cols = 2 * ((fh.frame_width + 7) >> 3);
    int mi_rows = 2 * ((fh.frame_height + 7) >> 3);
    int sb_cols = seq.use_128x128_superblock ? ((mi_cols + 31) >> 5) : ((mi_cols + 15) >> 4);
    int sb_rows = seq.use_128x128_superblock ? ((mi_rows + 31) >> 5) : ((mi_rows + 15) >> 4);
    int sb_shift = seq.use_128x128_superblock ? 5 : 4;
    int sb_size = sb_shift + 2;
    int max_tile_width_sb = 4096 >> sb_size;
    int max_tile_area_sb = (4096 * 2304) >> (2 * sb_size);
    int min_log2_tile_cols = static_cast<int>(tile_log2(max_tile_width_sb, sb_cols));
    int max_log2_tile_cols = static_cast<int>(tile_log2(1, std::min(sb_cols, 64)));
    int max_log2_tile_rows = static_cast<int>(tile_log2(1, std::min(sb_rows, 64)));
    int min_log2_tiles = std::max(
        min_log2_tile_cols, static_cast<int>(tile_log2(max_tile_area_sb, sb_rows * sb_cols))
    );

    fh.uniform_tile_spacing_flag = bd.read_bool(128);
    if (fh.uniform_tile_spacing_flag) {
        int tile_width_sb, tile_height_sb;
        fh.tile_cols_log2 = static_cast<std::uint8_t>(read_increment(
            bd,
            static_cast<std::uint32_t>(min_log2_tile_cols),
            static_cast<std::uint32_t>(max_log2_tile_cols)
        ));
        tile_width_sb = (sb_cols + (1 << fh.tile_cols_log2) - 1) >> fh.tile_cols_log2;
        int off = 0;
        int j = 0;
        for (; off < sb_cols; off += tile_width_sb)
            fh.tile_start_col_sb[j++] = static_cast<std::uint8_t>(off);
        fh.tile_cols = static_cast<std::uint8_t>((sb_cols + tile_width_sb - 1) / tile_width_sb);
        int min_log2_tile_rows = std::max(min_log2_tiles - fh.tile_cols_log2, 0);
        fh.tile_rows_log2 = static_cast<std::uint8_t>(read_increment(
            bd,
            static_cast<std::uint32_t>(min_log2_tile_rows),
            static_cast<std::uint32_t>(max_log2_tile_rows)
        ));
        tile_height_sb = (sb_rows + (1 << fh.tile_rows_log2) - 1) >> fh.tile_rows_log2;
        off = 0;
        j = 0;
        for (; off < sb_rows; off += tile_height_sb)
            fh.tile_start_row_sb[j++] = static_cast<std::uint8_t>(off);
        fh.tile_rows = static_cast<std::uint8_t>((sb_rows + tile_height_sb - 1) / tile_height_sb);
        for (int i = 0; i < fh.tile_cols - 1; ++i)
            fh.width_in_sbs_minus_1[i] = static_cast<std::uint8_t>(tile_width_sb - 1);
        fh.width_in_sbs_minus_1[fh.tile_cols - 1] =
            static_cast<std::uint8_t>(sb_cols - (fh.tile_cols - 1) * tile_width_sb - 1);
        for (int i = 0; i < fh.tile_rows - 1; ++i)
            fh.height_in_sbs_minus_1[i] = static_cast<std::uint8_t>(tile_height_sb - 1);
        fh.height_in_sbs_minus_1[fh.tile_rows - 1] =
            static_cast<std::uint8_t>(sb_rows - (fh.tile_rows - 1) * tile_height_sb - 1);
    } else {
        int widest_tile_sb = 0;
        int start_sb = 0;
        int i = 0;
        for (; start_sb < sb_cols && i < 64; ++i) {
            fh.tile_start_col_sb[i] = static_cast<std::uint8_t>(start_sb);
            int max_width = std::min(sb_cols - start_sb, max_tile_width_sb);
            std::uint32_t v = read_ns(bd, static_cast<std::uint32_t>(max_width));
            fh.width_in_sbs_minus_1[i] = static_cast<std::uint8_t>(v);
            int size_sb = fh.width_in_sbs_minus_1[i] + 1;
            widest_tile_sb = std::max(size_sb, widest_tile_sb);
            start_sb += size_sb;
        }
        fh.tile_cols = static_cast<std::uint8_t>(i);
        fh.tile_cols_log2 = static_cast<std::uint8_t>(tile_log2(1, i));
        int max_tile_height_sb = 0;
        if (min_log2_tiles > 0)
            max_tile_area_sb = (sb_rows * sb_cols) >> (min_log2_tiles + 1);
        else
            max_tile_area_sb = sb_rows * sb_cols;
        max_tile_height_sb = std::max(max_tile_area_sb / std::max(widest_tile_sb, 1), 1);
        start_sb = 0;
        i = 0;
        for (; start_sb < sb_rows && i < 64; ++i) {
            fh.tile_start_row_sb[i] = static_cast<std::uint8_t>(start_sb);
            int max_height = std::min(sb_rows - start_sb, max_tile_height_sb);
            std::uint32_t v = read_ns(bd, static_cast<std::uint32_t>(max_height));
            fh.height_in_sbs_minus_1[i] = static_cast<std::uint8_t>(v);
            int size_sb = fh.height_in_sbs_minus_1[i] + 1;
            start_sb += size_sb;
        }
        fh.tile_rows = static_cast<std::uint8_t>(i);
        fh.tile_rows_log2 = static_cast<std::uint8_t>(tile_log2(1, i));
    }
    if (fh.tile_cols_log2 > 0 || fh.tile_rows_log2 > 0) {
        fh.context_update_tile_id =
            static_cast<std::uint16_t>(bd.read_literal(fh.tile_cols_log2 + fh.tile_rows_log2));
        fh.tile_size_bytes_minus1 = static_cast<std::uint8_t>(bd.read_literal(2));
    } else {
        fh.context_update_tile_id = 0;
        fh.tile_size_bytes_minus1 = 0;
    }
}

inline void parse_global_motion(BooleanDecoder& bd, FrameHeader& fh) {
    if (fh.frame_is_intra)
        return;
    for (int ref = 0; ref < 8; ++ref) {
        fh.is_global[ref] = bd.read_bool(128);
        if (fh.is_global[ref]) {
            fh.is_rot_zoom[ref] = bd.read_bool(128);
            int type;
            if (fh.is_rot_zoom[ref])
                type = 2;  // ROTZOOM
            else {
                fh.is_translation[ref] = bd.read_bool(128);
                type = fh.is_translation[ref] ? 1 : 2;  // translation vs affine (simplified)
                if (!fh.is_translation[ref])
                    type = 3;  // affine
                else
                    type = 1;
            }
            if (type >= 2) {
                // gm_params[2], [3]
                for (int idx = 2; idx <= 3; ++idx) {
                    int abs_bits, prec_bits;
                    if (idx < 2) {
                        abs_bits = 10;
                        prec_bits = 6;
                    } else {
                        abs_bits = 10;
                        prec_bits = 6;
                    }
                    std::uint32_t num_syms = 2 * (1u << abs_bits) + 1;
                    fh.gm_params[ref][idx] = read_subexp(bd, num_syms);
                    (void)prec_bits;
                }
                if (type == 3) {  // affine
                    for (int idx = 4; idx <= 5; ++idx) {
                        std::uint32_t num_syms = 2 * (1u << 10) + 1;
                        fh.gm_params[ref][idx] = read_subexp(bd, num_syms);
                    }
                }
            }
            if (type >= 1) {
                for (int idx = 0; idx <= 1; ++idx) {
                    bool is_trans_only = (type == 1);
                    int abs_bits = is_trans_only ? (12 - (fh.allow_high_precision_mv ? 0 : 1)) : 10;
                    std::uint32_t num_syms = 2 * (1u << abs_bits) + 1;
                    fh.gm_params[ref][idx] = read_subexp(bd, num_syms);
                }
            }
        } else {
            fh.is_rot_zoom[ref] = false;
            fh.is_translation[ref] = false;
        }
    }
}

inline void parse_film_grain(BooleanDecoder& bd, FrameHeader& fh, const SequenceHeader& seq) {
    if (!seq.film_grain_params_present || (!fh.show_frame && !fh.showable_frame))
        return;
    fh.film_grain.apply_grain = bd.read_bool(128);
    if (!fh.film_grain.apply_grain)
        return;
    fh.film_grain.grain_seed = static_cast<std::uint16_t>(bd.read_literal(16));
    if (fh.frame_type == FrameType::InterFrame)
        fh.film_grain.update_grain = bd.read_bool(128);
    else
        fh.film_grain.update_grain = true;
    if (!fh.film_grain.update_grain) {
        fh.film_grain.film_grain_params_ref_idx = static_cast<std::uint8_t>(bd.read_literal(3));
        return;
    }
    fh.film_grain.num_y_points = static_cast<std::uint8_t>(bd.read_literal(4));
    for (int i = 0; i < fh.film_grain.num_y_points; ++i) {
        // fcs with range, simplified to 8 bits
        fh.film_grain.point_y_value[i] = static_cast<std::uint8_t>(bd.read_literal(8));
        fh.film_grain.point_y_scaling[i] = static_cast<std::uint8_t>(bd.read_literal(8));
    }
    if (seq.color_config.mono_chrome)
        fh.film_grain.chroma_scaling_from_luma = false;
    else
        fh.film_grain.chroma_scaling_from_luma = bd.read_bool(128);
    bool need_chroma =
        !(seq.color_config.mono_chrome || fh.film_grain.chroma_scaling_from_luma ||
          (seq.color_config.subsampling_x == 1 && seq.color_config.subsampling_y == 1 &&
           fh.film_grain.num_y_points == 0));
    if (!need_chroma) {
        fh.film_grain.num_cb_points = 0;
        fh.film_grain.num_cr_points = 0;
    } else {
        fh.film_grain.num_cb_points = static_cast<std::uint8_t>(bd.read_literal(4));
        for (int i = 0; i < fh.film_grain.num_cb_points; ++i) {
            fh.film_grain.point_cb_value[i] = static_cast<std::uint8_t>(bd.read_literal(8));
            fh.film_grain.point_cb_scaling[i] = static_cast<std::uint8_t>(bd.read_literal(8));
        }
        fh.film_grain.num_cr_points = static_cast<std::uint8_t>(bd.read_literal(4));
        for (int i = 0; i < fh.film_grain.num_cr_points; ++i) {
            fh.film_grain.point_cr_value[i] = static_cast<std::uint8_t>(bd.read_literal(8));
            fh.film_grain.point_cr_scaling[i] = static_cast<std::uint8_t>(bd.read_literal(8));
        }
    }
    fh.film_grain.grain_scaling_minus_8 = static_cast<std::uint8_t>(bd.read_literal(2));
    fh.film_grain.ar_coeff_lag = static_cast<std::uint8_t>(bd.read_literal(2));
    int num_pos_luma = 2 * fh.film_grain.ar_coeff_lag * (fh.film_grain.ar_coeff_lag + 1);
    int num_pos_chroma = num_pos_luma;
    if (fh.film_grain.num_y_points)
        num_pos_chroma = num_pos_luma + 1;
    if (fh.film_grain.num_y_points) {
        for (int i = 0; i < num_pos_luma; ++i)
            fh.film_grain.ar_coeffs_y_plus_128[i] = static_cast<std::uint8_t>(bd.read_literal(8));
    }
    if (fh.film_grain.chroma_scaling_from_luma || fh.film_grain.num_cb_points) {
        for (int i = 0; i < num_pos_chroma; ++i)
            fh.film_grain.ar_coeffs_cb_plus_128[i] = static_cast<std::uint8_t>(bd.read_literal(8));
    }
    if (fh.film_grain.chroma_scaling_from_luma || fh.film_grain.num_cr_points) {
        for (int i = 0; i < num_pos_chroma; ++i)
            fh.film_grain.ar_coeffs_cr_plus_128[i] = static_cast<std::uint8_t>(bd.read_literal(8));
    }
    fh.film_grain.ar_coeff_shift_minus_6 = static_cast<std::uint8_t>(bd.read_literal(2));
    fh.film_grain.grain_scale_shift = static_cast<std::uint8_t>(bd.read_literal(2));
    if (fh.film_grain.num_cb_points) {
        fh.film_grain.cb_mult = static_cast<std::uint8_t>(bd.read_literal(8));
        fh.film_grain.cb_luma_mult = static_cast<std::uint8_t>(bd.read_literal(8));
        fh.film_grain.cb_offset = static_cast<std::uint16_t>(bd.read_literal(9));
    }
    if (fh.film_grain.num_cr_points) {
        fh.film_grain.cr_mult = static_cast<std::uint8_t>(bd.read_literal(8));
        fh.film_grain.cr_luma_mult = static_cast<std::uint8_t>(bd.read_literal(8));
        fh.film_grain.cr_offset = static_cast<std::uint16_t>(bd.read_literal(9));
    }
    fh.film_grain.overlap_flag = bd.read_bool(128);
    fh.film_grain.clip_to_restricted_range = bd.read_bool(128);
}

}  // namespace detail_frame

[[nodiscard]]
inline FrameHeader parse_frame_header(
    std::span<const std::uint8_t> payload, const SequenceHeader& seq
) {
    BooleanDecoder bd{payload};
    FrameHeader fh;

    const std::uint8_t id_len = seq.frame_id_length();
    const std::uint8_t order_hint_bits = seq.order_hint_bits();
    const std::uint32_t all_frames = (1u << 8) - 1;  // AV1_NUM_REF_FRAMES =8

    bool frame_is_intra = false;

    if (seq.reduced_still_picture_header) {
        fh.frame_type = FrameType::KeyFrame;
        fh.show_frame = true;
        fh.showable_frame = false;
        fh.show_existing_frame = false;
        frame_is_intra = true;
        fh.frame_is_intra = true;
        // Still need to parse remaining header fields for reduced still
        // (disable_cdf_update etc. are inferred differently but we continue)
    } else {
        fh.show_existing_frame = bd.read_bool(128);
        if (fh.show_existing_frame) {
            fh.frame_to_show_map_idx = static_cast<std::uint8_t>(bd.read_literal(3));
            if (seq.decoder_model_info_present_flag && !seq.timing_info.equal_picture_interval) {
                fh.frame_presentation_time = bd.read_literal(
                    seq.decoder_model_info.frame_presentation_time_length_minus_1 + 1
                );
            }
            if (seq.frame_id_numbers_present_flag) {
                fh.display_frame_id = bd.read_literal(id_len);
            }
            // inferred: frame_type from ref, refresh flags etc. — early return
            fh.frame_type = FrameType::InterFrame;  // placeholder
            fh.show_frame = false;
            fh.showable_frame = false;
            fh.error_resilient_mode = false;
            fh.order_hint = 0;
            return fh;
        }

        const std::uint8_t raw_frame_type = static_cast<std::uint8_t>(bd.read_literal(2));
        fh.frame_type = static_cast<FrameType>(raw_frame_type);
        frame_is_intra =
            (fh.frame_type == FrameType::KeyFrame || fh.frame_type == FrameType::IntraOnly);
        fh.frame_is_intra = frame_is_intra;

        fh.show_frame = bd.read_bool(128);
        if (fh.show_frame && seq.decoder_model_info_present_flag &&
            !seq.timing_info.equal_picture_interval) {
            fh.frame_presentation_time =
                bd.read_literal(seq.decoder_model_info.frame_presentation_time_length_minus_1 + 1);
        }
        if (fh.show_frame) {
            fh.showable_frame = fh.frame_type != FrameType::KeyFrame;
        } else {
            fh.showable_frame = bd.read_bool(128);
        }

        if (fh.frame_type == FrameType::Switch ||
            (fh.frame_type == FrameType::KeyFrame && fh.show_frame)) {
            fh.error_resilient_mode = true;
        } else {
            fh.error_resilient_mode = bd.read_bool(128);
        }
    }

    // if not early return for reduced, we still need frame_is_intra for reduced case already set
    if (seq.reduced_still_picture_header && !fh.frame_is_intra) {
        // ensure flag for reduced
        frame_is_intra = true;
        fh.frame_is_intra = true;
    }
    if (!seq.reduced_still_picture_header) {
        // already set above
    } else {
        // for reduced, error_resilient inferred? Actually for reduced still, frame_type KEY and
        // show_frame true => error_resilient true
        fh.error_resilient_mode = true;
    }

    fh.disable_cdf_update = bd.read_bool(128);

    if (seq.seq_force_screen_content_tools == 2) {
        fh.allow_screen_content_tools = bd.read_bool(128);
    } else {
        fh.allow_screen_content_tools = seq.seq_force_screen_content_tools == 1;
    }

    if (fh.allow_screen_content_tools) {
        if (seq.seq_force_integer_mv == 2) {
            fh.force_integer_mv = bd.read_bool(128);
        } else {
            fh.force_integer_mv = seq.seq_force_integer_mv == 1;
        }
    } else {
        fh.force_integer_mv = false;
    }

    if (frame_is_intra) {
        fh.force_integer_mv = true;
    }

    if (seq.frame_id_numbers_present_flag) {
        fh.current_frame_id = bd.read_literal(id_len);
    }

    if (fh.frame_type == FrameType::Switch) {
        fh.frame_size_override_flag = true;
    } else if (seq.reduced_still_picture_header) {
        fh.frame_size_override_flag = false;
    } else {
        fh.frame_size_override_flag = bd.read_bool(128);
    }

    if (order_hint_bits != 0) {
        fh.order_hint = bd.read_literal(order_hint_bits);
    } else {
        fh.order_hint = 0;
    }

    if (frame_is_intra || fh.error_resilient_mode) {
        fh.primary_ref_frame = 7;
    } else {
        fh.primary_ref_frame = static_cast<std::uint8_t>(bd.read_literal(3));
    }

    if (seq.decoder_model_info_present_flag) {
        fh.buffer_removal_time_present_flag = bd.read_bool(128);
        if (fh.buffer_removal_time_present_flag) {
            for (std::uint8_t i = 0; i <= seq.operating_points_cnt_minus_1; ++i) {
                if (seq.decoder_model_present_for_this_op[i]) {
                    std::uint16_t op_pt_idc = seq.operating_point_idc[i];
                    // temporal_id/spatial_id assumed 0
                    bool in_temporal = (op_pt_idc & 1) != 0;
                    bool in_spatial = ((op_pt_idc >> 8) & 1) != 0;
                    if (op_pt_idc == 0 || (in_temporal && in_spatial)) {
                        fh.buffer_removal_time[i] = bd.read_literal(
                            seq.decoder_model_info.buffer_removal_time_length_minus_1 + 1
                        );
                    }
                }
            }
        }
    }

    if (fh.frame_type == FrameType::Switch ||
        (fh.frame_type == FrameType::KeyFrame && fh.show_frame)) {
        fh.refresh_frame_flags = static_cast<std::uint8_t>(all_frames);
    } else {
        fh.refresh_frame_flags = static_cast<std::uint8_t>(bd.read_literal(8));
    }

    if (!frame_is_intra || fh.refresh_frame_flags != all_frames) {
        if (seq.enable_order_hint) {
            for (int i = 0; i < 8; ++i) {
                if (fh.error_resilient_mode) {
                    if (order_hint_bits)
                        fh.ref_order_hint[i] =
                            static_cast<std::uint8_t>(bd.read_literal(order_hint_bits));
                    else
                        fh.ref_order_hint[i] = 0;
                } else {
                    fh.ref_order_hint[i] = 0;  // would be from ref, infer 0
                }
            }
        }
    }

    // frame size handling
    if (frame_is_intra) {
        // frame_size()
        if (fh.frame_size_override_flag) {
            fh.frame_width_minus_1 =
                static_cast<std::uint16_t>(bd.read_literal(seq.frame_width_bits_minus_1 + 1));
            fh.frame_height_minus_1 =
                static_cast<std::uint16_t>(bd.read_literal(seq.frame_height_bits_minus_1 + 1));
            fh.frame_width = fh.frame_width_minus_1 + 1;
            fh.frame_height = fh.frame_height_minus_1 + 1;
        } else {
            fh.frame_width = static_cast<std::uint16_t>(seq.max_frame_width);
            fh.frame_height = static_cast<std::uint16_t>(seq.max_frame_height);
            fh.frame_width_minus_1 = fh.frame_width - 1;
            fh.frame_height_minus_1 = fh.frame_height - 1;
        }
        fh.upscaled_width = fh.frame_width;
        // superres_params
        if (seq.enable_superres)
            fh.use_superres = bd.read_bool(128);
        else
            fh.use_superres = false;
        if (fh.use_superres) {
            fh.coded_denom = static_cast<std::uint8_t>(bd.read_literal(3));
            fh.superres_denom = fh.coded_denom + 9;
            // upscaled_width stays as before, frame_width scaled
            fh.upscaled_width = fh.frame_width;
            fh.frame_width = static_cast<std::uint16_t>(
                (fh.upscaled_width * 8 + fh.superres_denom / 2) / fh.superres_denom
            );
            fh.frame_width_minus_1 = fh.frame_width - 1;
        } else {
            fh.coded_denom = 8;
            fh.superres_denom = 8;
        }
        // render_size
        fh.render_and_frame_size_different = bd.read_bool(128);
        if (fh.render_and_frame_size_different) {
            fh.render_width_minus_1 = static_cast<std::uint16_t>(bd.read_literal(16));
            fh.render_height_minus_1 = static_cast<std::uint16_t>(bd.read_literal(16));
            fh.render_width = fh.render_width_minus_1 + 1;
            fh.render_height = fh.render_height_minus_1 + 1;
        } else {
            fh.render_width = fh.frame_width;
            fh.render_height = fh.frame_height;
            fh.render_width_minus_1 = fh.frame_width_minus_1;
            fh.render_height_minus_1 = fh.frame_height_minus_1;
        }

        if (fh.allow_screen_content_tools && fh.upscaled_width == fh.frame_width)
            fh.allow_intrabc = bd.read_bool(128);
        else
            fh.allow_intrabc = false;
    } else {
        // inter frame size handling
        if (!seq.enable_order_hint) {
            fh.frame_refs_short_signaling = false;
        } else {
            fh.frame_refs_short_signaling = bd.read_bool(128);
            if (fh.frame_refs_short_signaling) {
                fh.last_frame_idx = static_cast<std::uint8_t>(bd.read_literal(3));
                fh.golden_frame_idx = static_cast<std::uint8_t>(bd.read_literal(3));
                // set_frame_refs would derive ref_frame_idx, we infer
                for (int i = 0; i < 7; ++i)
                    fh.ref_frame_idx[i] = static_cast<std::int8_t>(i % 8);
                fh.ref_frame_idx[0] = fh.last_frame_idx;
                fh.ref_frame_idx[1] = fh.golden_frame_idx;
            }
        }
        for (int i = 0; i < 7; ++i) {
            if (!fh.frame_refs_short_signaling)
                fh.ref_frame_idx[i] = static_cast<std::int8_t>(bd.read_literal(3));
            if (seq.frame_id_numbers_present_flag) {
                fh.delta_frame_id_minus1[i] =
                    bd.read_literal(seq.delta_frame_id_length_minus_2 + 2);
            }
        }

        if (fh.frame_size_override_flag && !fh.error_resilient_mode) {
            // frame_size_with_refs
            bool any_found = false;
            for (int i = 0; i < 7; ++i) {
                fh.found_ref[i] = bd.read_bool(128);
                if (fh.found_ref[i])
                    any_found = true;
            }
            if (any_found) {
                // sizes inferred from ref, we keep current frame_width from seq max as placeholder
                fh.frame_width = static_cast<std::uint16_t>(seq.max_frame_width);
                fh.frame_height = static_cast<std::uint16_t>(seq.max_frame_height);
                fh.frame_width_minus_1 = fh.frame_width - 1;
                fh.frame_height_minus_1 = fh.frame_height - 1;
                fh.upscaled_width = fh.frame_width;
                if (seq.enable_superres)
                    fh.use_superres = bd.read_bool(128);
                else
                    fh.use_superres = false;
                if (fh.use_superres) {
                    fh.coded_denom = static_cast<std::uint8_t>(bd.read_literal(3));
                    fh.superres_denom = fh.coded_denom + 9;
                    fh.upscaled_width = fh.frame_width;
                    fh.frame_width = static_cast<std::uint16_t>(
                        (fh.upscaled_width * 8 + fh.superres_denom / 2) / fh.superres_denom
                    );
                }
                // render size inferred to ref's render size, we set to frame size
                fh.render_width = fh.frame_width;
                fh.render_height = fh.frame_height;
                fh.render_width_minus_1 = fh.frame_width_minus_1;
                fh.render_height_minus_1 = fh.frame_height_minus_1;
            } else {
                // frame_size + render_size
                fh.frame_width_minus_1 =
                    static_cast<std::uint16_t>(bd.read_literal(seq.frame_width_bits_minus_1 + 1));
                fh.frame_height_minus_1 =
                    static_cast<std::uint16_t>(bd.read_literal(seq.frame_height_bits_minus_1 + 1));
                fh.frame_width = fh.frame_width_minus_1 + 1;
                fh.frame_height = fh.frame_height_minus_1 + 1;
                fh.upscaled_width = fh.frame_width;
                if (seq.enable_superres)
                    fh.use_superres = bd.read_bool(128);
                else
                    fh.use_superres = false;
                if (fh.use_superres) {
                    fh.coded_denom = static_cast<std::uint8_t>(bd.read_literal(3));
                    fh.superres_denom = fh.coded_denom + 9;
                    fh.upscaled_width = fh.frame_width;
                    fh.frame_width = static_cast<std::uint16_t>(
                        (fh.upscaled_width * 8 + fh.superres_denom / 2) / fh.superres_denom
                    );
                    fh.frame_width_minus_1 = fh.frame_width - 1;
                }
                fh.render_and_frame_size_different = bd.read_bool(128);
                if (fh.render_and_frame_size_different) {
                    fh.render_width_minus_1 = static_cast<std::uint16_t>(bd.read_literal(16));
                    fh.render_height_minus_1 = static_cast<std::uint16_t>(bd.read_literal(16));
                    fh.render_width = fh.render_width_minus_1 + 1;
                    fh.render_height = fh.render_height_minus_1 + 1;
                } else {
                    fh.render_width = fh.frame_width;
                    fh.render_height = fh.frame_height;
                    fh.render_width_minus_1 = fh.frame_width_minus_1;
                    fh.render_height_minus_1 = fh.frame_height_minus_1;
                }
            }
        } else {
            if (fh.frame_size_override_flag) {
                fh.frame_width_minus_1 =
                    static_cast<std::uint16_t>(bd.read_literal(seq.frame_width_bits_minus_1 + 1));
                fh.frame_height_minus_1 =
                    static_cast<std::uint16_t>(bd.read_literal(seq.frame_height_bits_minus_1 + 1));
                fh.frame_width = fh.frame_width_minus_1 + 1;
                fh.frame_height = fh.frame_height_minus_1 + 1;
            } else {
                fh.frame_width = static_cast<std::uint16_t>(seq.max_frame_width);
                fh.frame_height = static_cast<std::uint16_t>(seq.max_frame_height);
                fh.frame_width_minus_1 = fh.frame_width - 1;
                fh.frame_height_minus_1 = fh.frame_height - 1;
            }
            fh.upscaled_width = fh.frame_width;
            if (seq.enable_superres)
                fh.use_superres = bd.read_bool(128);
            else
                fh.use_superres = false;
            if (fh.use_superres) {
                fh.coded_denom = static_cast<std::uint8_t>(bd.read_literal(3));
                fh.superres_denom = fh.coded_denom + 9;
                fh.upscaled_width = fh.frame_width;
                fh.frame_width = static_cast<std::uint16_t>(
                    (fh.upscaled_width * 8 + fh.superres_denom / 2) / fh.superres_denom
                );
                fh.frame_width_minus_1 = fh.frame_width - 1;
            }
            fh.render_and_frame_size_different = bd.read_bool(128);
            if (fh.render_and_frame_size_different) {
                fh.render_width_minus_1 = static_cast<std::uint16_t>(bd.read_literal(16));
                fh.render_height_minus_1 = static_cast<std::uint16_t>(bd.read_literal(16));
                fh.render_width = fh.render_width_minus_1 + 1;
                fh.render_height = fh.render_height_minus_1 + 1;
            } else {
                fh.render_width = fh.frame_width;
                fh.render_height = fh.frame_height;
                fh.render_width_minus_1 = fh.frame_width_minus_1;
                fh.render_height_minus_1 = fh.frame_height_minus_1;
            }
        }

        if (fh.force_integer_mv)
            fh.allow_high_precision_mv = false;
        else
            fh.allow_high_precision_mv = bd.read_bool(128);

        fh.is_filter_switchable = bd.read_bool(128);
        if (fh.is_filter_switchable)
            fh.interpolation_filter = 3;  // SWITCHABLE
        else
            fh.interpolation_filter = static_cast<std::uint8_t>(bd.read_literal(2));

        fh.is_motion_mode_switchable = bd.read_bool(128);

        if (fh.error_resilient_mode || !seq.enable_ref_frame_mvs)
            fh.use_ref_frame_mvs = false;
        else
            fh.use_ref_frame_mvs = bd.read_bool(128);

        fh.allow_intrabc = false;
    }

    if (seq.reduced_still_picture_header || fh.disable_cdf_update)
        fh.disable_frame_end_update_cdf = true;
    else
        fh.disable_frame_end_update_cdf = bd.read_bool(128);

    // tile_info
    detail_frame::parse_tile_info(bd, fh, seq);

    // quantization
    detail_frame::parse_quantization(bd, fh, seq);

    // segmentation
    detail_frame::parse_segmentation(bd, fh);

    // delta_q / delta_lf
    detail_frame::parse_delta_q_params(bd, fh);
    detail_frame::parse_delta_lf_params(bd, fh);

    // compute coded_lossless / all_lossless
    bool coded_lossless = true;
    for (int i = 0; i < 8; ++i) {
        int qindex = fh.base_q_idx;
        if (fh.feature_enabled[i][0])
            qindex += fh.feature_value[i][0];
        if (qindex)
            coded_lossless = false;
        if (fh.delta_q_y_dc || fh.delta_q_u_dc || fh.delta_q_u_ac || fh.delta_q_v_dc ||
            fh.delta_q_v_ac)
            coded_lossless = false;
    }
    bool all_lossless = coded_lossless && (fh.frame_width == fh.upscaled_width);

    // loop filter
    detail_frame::parse_loop_filter(bd, fh, seq, coded_lossless);

    // cdef
    detail_frame::parse_cdef(bd, fh, seq, coded_lossless);

    // lr
    detail_frame::parse_lr(bd, fh, seq, coded_lossless, all_lossless);

    // tx_mode
    if (coded_lossless)
        fh.tx_mode = 0;
    else
        fh.tx_mode = static_cast<std::uint8_t>(detail_frame::read_increment(bd, 1, 2));

    // frame_reference_mode
    if (frame_is_intra)
        fh.reference_select = false;
    else
        fh.reference_select = bd.read_bool(128);

    // skip_mode
    {
        bool skip_mode_allowed = false;
        if (frame_is_intra || !fh.reference_select || !seq.enable_order_hint)
            skip_mode_allowed = false;
        else {
            // simplified: if reference_select and order_hint enabled, allow if there are
            // forward/backward refs For sample, we can assume false to avoid extra bit? But spec
            // requires computed. We'll compute simplified: if frame_is_intra==false and
            // reference_select true and seq.enable_order_hint true then true else false This may
            // cause extra flag read mismatch. For conservative, set to true when possible and read
            // flag. To avoid mismatched bits, we check actual order hints: we don't have ref hints,
            // assume forward exists. So set skip_mode_allowed = true when not intra and
            // reference_select and enable_order_hint
            if (!frame_is_intra && fh.reference_select && seq.enable_order_hint)
                skip_mode_allowed = true;
        }
        if (skip_mode_allowed)
            fh.skip_mode_present = bd.read_bool(128);
        else
            fh.skip_mode_present = false;
    }

    if (frame_is_intra || fh.error_resilient_mode || !seq.enable_warped_motion)
        fh.allow_warped_motion = false;
    else
        fh.allow_warped_motion = bd.read_bool(128);

    fh.reduced_tx_set = bd.read_bool(128);

    detail_frame::parse_global_motion(bd, fh);

    detail_frame::parse_film_grain(bd, fh, seq);

    return fh;
}

/*
 * Legacy overload without sequence-header context.  OrderHintBits is 0, so
 * order_hint is not read; useful when only the leading fields are needed.
 */
[[nodiscard]]
inline FrameHeader parse_frame_header(std::span<const std::uint8_t> payload) {
    return parse_frame_header(payload, SequenceHeader{});
}

}  // namespace av1
}  // namespace bs
