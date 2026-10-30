// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/*
 * H.264 macroblock-layer and slice-data parser for the opt-in motion-vector
 * extractor.
 *
 * Scope of this implementation:
 *   - CABAC and CAVLC entropy coding;
 *   - progressive frame, field and MBAFF pictures;
 *   - P and B slices, all inter partition shapes and sub-partitions;
 *   - 4:2:0, 4:2:2 and 4:4:4 residual syntax, including Intra16x16 DC,
 *     4x4 and 8x8 transform blocks, I_PCM sample skipping (CAVLC and CABAC)
 *     and the 4:2:2 chroma-DC layout;
 *   - monochrome syntax (ChromaArrayType 0): luma-only CBP, residual and
 *     I_PCM paths;
 *   - separate colour planes: each colour_plane_id slice is parsed with the
 *     ChromaArrayType 0 macroblock syntax; the extractor runs one independent
 *     decoder/DPB per plane;
 *   - flexible macroblock ordering (slice groups) for map types 0-6,
 *     including NextMbAddress iteration, mb_skip_run and CABAC
 *     end_of_slice_flag over non-contiguous macroblock addresses;
 *   - intra macroblocks (parsed to advance the bitstream, not reconstructed);
 *   - residual coefficients (parsed to advance, never reconstructed);
 *   - spatial and temporal direct mode;
 *   - reference-list indexing supplied by the caller's MV-DPB.
 *
 * It is intentionally not a pixel decoder: no prediction, transform,
 * deblocking or frame buffer is produced.  The only output is the 4x4 MV grid
 * and the syntax needed to reach the next macroblock.
 */

#include "../syntax/avc_motion_vector.hpp"
#include "../syntax/avc_pps.hpp"
#include "../syntax/avc_slice_header.hpp"
#include "../syntax/avc_sps.hpp"
#include "avc_cabac.hpp"
#include "avc_cavlc.hpp"
#include "avc_cell_simd.hpp"
#include "avc_fmo.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define BS_AVC_MB_ALWAYS_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define BS_AVC_MB_ALWAYS_INLINE __forceinline
#else
#define BS_AVC_MB_ALWAYS_INLINE inline
#endif

namespace bs {
namespace avc {

struct MotionReferencePicture {
    std::int32_t poc = 0;
    const MvCell* cells = nullptr;
    std::uint32_t width4x4 = 0;
    std::uint32_t height4x4 = 0;
    std::uint32_t frame_num = 0;
    bool field_pic = false;
    bool bottom_field = false;
    /* Non-existing frame inferred by the frame_num gap process. */
    bool non_existing = false;
    /*
     * Per-slice reference table of the referenced picture.  Temporal direct
     * mode needs it to translate the co-located block's ref_idx into a POC
     * when the referenced picture itself used multiple slices with different
     * lists.  `field_slice_refs` is used for a frame view synthesized from a
     * PAFF pair: even rows come from the top field and odd rows from the
     * bottom field.  The matching slice_map runs resolve the exact slice of a
     * macroblock for non-contiguous FMO pictures.
     */
    const MotionSliceReference* slice_refs = nullptr;
    std::uint32_t slice_ref_count = 0;
    const MotionSliceRun* slice_map = nullptr;
    std::uint32_t slice_map_count = 0;
    std::uint32_t slice_map_coverage = 0;
    const MotionSliceReference* field_slice_refs[2]{nullptr, nullptr};
    std::uint32_t field_slice_ref_count[2]{0, 0};
    const MotionSliceRun* field_slice_map[2]{nullptr, nullptr};
    std::uint32_t field_slice_map_count[2]{0, 0};
    std::uint32_t field_slice_map_coverage[2]{0, 0};
    bool interleaved_fields = false;
    /* The referenced picture is an MBAFF frame (CurrMbAddr pair addressing). */
    bool mbaff = false;
    /*
     * Reference lists used by the referenced picture.  Kept as the
     * picture-level fallback for callers that do not have cell coordinates.
     */
    std::array<std::int32_t, 32> ref_poc[2]{};
    std::uint8_t num_ref_idx[2]{0, 0};
    std::uint32_t non_existing_mask[2]{0, 0};

    [[nodiscard]] std::int32_t refPoc(int list, int index) const noexcept {
        if (list < 0 || list > 1 || index < 0 || index >= 32 ||
            index >= static_cast<int>(num_ref_idx[list])) {
            return kMotionVectorUnknownPoc;
        }
        return ref_poc[list][static_cast<std::size_t>(index)];
    }

    [[nodiscard]] bool refNonExisting(int list, int index) const noexcept {
        if (list < 0 || list > 1 || index < 0 || index >= 32 ||
            index >= static_cast<int>(num_ref_idx[list])) {
            return false;
        }
        return (non_existing_mask[list] & (1u << static_cast<unsigned>(index))) != 0;
    }

    [[nodiscard]] const MotionSliceReference* sliceAt(
            std::uint32_t mb_addr,
            bool bottom_row = false) const noexcept {
        const MotionSliceReference* table = slice_refs;
        std::uint32_t count = slice_ref_count;
        const MotionSliceRun* map = slice_map;
        std::uint32_t map_count = slice_map_count;
        std::uint32_t coverage = slice_map_coverage;
        if (interleaved_fields) {
            table = field_slice_refs[bottom_row ? 1 : 0];
            count = field_slice_ref_count[bottom_row ? 1 : 0];
            map = field_slice_map[bottom_row ? 1 : 0];
            map_count = field_slice_map_count[bottom_row ? 1 : 0];
            coverage = field_slice_map_coverage[bottom_row ? 1 : 0];
        }
        if (table == nullptr || count == 0) {
            return nullptr;
        }
        if (map != nullptr && map_count > 0 && mb_addr < coverage) {
            std::uint32_t low = 0;
            std::uint32_t high = map_count;
            while (low < high) {
                const std::uint32_t mid = low + (high - low) / 2u;
                if (map[mid].mb_addr <= mb_addr) {
                    low = mid + 1u;
                } else {
                    high = mid;
                }
            }
            if (low == 0) {
                return nullptr;
            }
            const std::uint32_t index = map[low - 1u].slice_index;
            return index != kMotionSliceIndexNone && index < count ? &table[index]
                                                                   : nullptr;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            if (mb_addr >= table[i].mb_addr_begin && mb_addr < table[i].mb_addr_end) {
                return &table[i];
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::uint32_t mbAddressAt(
            std::uint32_t x4,
            std::uint32_t y4) const noexcept {
        const std::uint32_t mb_width = width4x4 / 4u;
        if (mb_width == 0) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        if (interleaved_fields) {
            const std::uint32_t field_y4 = y4 >> 1;
            return (field_y4 >> 2) * mb_width + (x4 >> 2);
        }
        const std::uint32_t frame_y = y4 >> 2;
        if (mbaff) {
            return ((frame_y >> 1) * mb_width + (x4 >> 2)) * 2u + (frame_y & 1u);
        }
        return frame_y * mb_width + (x4 >> 2);
    }

    [[nodiscard]] std::int32_t refPocAt(
            std::uint32_t x4,
            std::uint32_t y4,
            int list,
            int index) const noexcept {
        if (width4x4 / 4u == 0 || list < 0 || list > 1 || index < 0 || index >= 32) {
            return kMotionVectorUnknownPoc;
        }
        const std::uint32_t mb_addr = mbAddressAt(x4, y4);
        if (mb_addr == std::numeric_limits<std::uint32_t>::max()) {
            return kMotionVectorUnknownPoc;
        }
        const MotionSliceReference* slice = sliceAt(mb_addr, (y4 & 1u) != 0);
        if (slice != nullptr) {
            if (index >= static_cast<int>(slice->num_ref_idx[list])) {
                return kMotionVectorUnknownPoc;
            }
            return slice->ref_poc[list][static_cast<std::size_t>(index)];
        }
        return refPoc(list, index);
    }

    [[nodiscard]] bool refNonExistingAt(
            std::uint32_t x4,
            std::uint32_t y4,
            int list,
            int index) const noexcept {
        if (width4x4 / 4u == 0 || list < 0 || list > 1 || index < 0 || index >= 32) {
            return false;
        }
        const std::uint32_t mb_addr = mbAddressAt(x4, y4);
        if (mb_addr == std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        const MotionSliceReference* slice = sliceAt(mb_addr, (y4 & 1u) != 0);
        const std::uint32_t mask =
                slice != nullptr ? slice->non_existing_mask[list]
                                 : non_existing_mask[list];
        return (mask & (1u << static_cast<unsigned>(index))) != 0;
    }
};

namespace detail {

enum class PredMode : std::uint8_t { None, L0, L1, Bi, Direct };

struct Partition {
    std::uint8_t x = 0; // 4x4 units inside the macroblock
    std::uint8_t y = 0;
    std::uint8_t w = 4;
    std::uint8_t h = 4;
    PredMode pred = detail::PredMode::None;
};

struct SubPartition {
    std::uint8_t x = 0;
    std::uint8_t y = 0;
    std::uint8_t w = 2;
    std::uint8_t h = 2;
};

struct RefChoice {
    bool fixed = false;
    int index = 0;
};

/*
 * Per-macroblock parser state, laid out hot-first.
 *
 * The macroblock array is the parser's largest working set: at 1080p a frame
 * holds 8100 entries, so a byte saved here saves 8 KiB per frame and a cache
 * line saved removes that much traffic per macroblock scan.  The layout below
 * is 160 bytes (2.5 cache lines) instead of the previous 396.
 *
 * Notes on the compact representation:
 *   - `ref` is the per-4x4 reference-index cache.  Motion vectors are not
 *     duplicated here: the neighbour predictor reads the dense MvCell output
 *     grid, so a per-MB vector array would have no readers.
 *   - `abs_mvd` stores the saturating absolute value of the decoded MVD.  The
 *     only consumer is the CABAC MVD context derivation, which compares the
 *     accumulated magnitude against 3 and 32 (H.264 9.3.3.1.1.7); clamping at
 *     255 cannot move a value across either threshold, and it avoids a second
 *     128-byte array of signed values.
 *   - `direct4x4` is a 16-bit mask, one bit per luma 4x4 raster index.
 *   - `intra4x4_modes` is intentionally gone: the modes were parsed and stored
 *     but never read.  `parse_intra_modes()` still consumes the syntax.
 *   - `nzc` is kept at 48 entries so 4:2:0, 4:2:2, 4:4:4 and monochrome all
 *     use the same safe layout.  A 4:2:0-only build could shrink it to 24.
 */
struct MbData {
    /* 32 bytes: per-4x4 reference index cache. */
    std::int8_t ref[2][16];
    /* 64 bytes: saturating |mvd| per list/4x4/component for CABAC contexts. */
    std::uint8_t abs_mvd[2][16][2]{};
    /*
     * Non-zero coefficient counts for the residual block layouts:
     *   0..15   plane 0 (luma) 4x4 blocks in raster order
     *   16..31  4:4:4 plane 1, or the two 4:2:2 chroma components
     *   32..47  4:4:4 plane 2
     * 4:2:0 chroma AC uses 16..23 (four blocks per component).
     */
    std::array<std::uint8_t, 48> nzc{};
    std::uint32_t slice_id = 0;
    /* Bit N corresponds to luma 4x4 raster index N (direct mode). */
    std::uint16_t direct4x4 = 0;
    bool available : 1 = false;
    bool intra : 1 = false;
    bool intra16x16 : 1 = false;
    bool is_intra4x4 : 1 = false;
    bool skip : 1 = false;
    bool direct : 1 = false;
    bool transform8x8 : 1 = false;
    /* True for a field picture MB, or for a field-coded MB in MBAFF. */
    bool field_mb : 1 = false;
    std::uint8_t slice_group = 0;
    std::uint8_t cbp = 0;
    std::uint8_t chroma_pred = 0;
    /* Intra16x16 DC presence, one per residual plane (0..2). */
    std::array<std::uint8_t, 3> luma_dc_cbf{0, 0, 0};
    std::uint8_t chroma_dc_cbf[2]{0, 0};

    MbData() {
        for (auto& list : ref) {
            for (auto& r : list) r = -1;
        }
    }

    [[nodiscard]] bool is_direct4x4(int local) const noexcept {
        return ((direct4x4 >> static_cast<unsigned>(local)) & 1u) != 0;
    }

    void set_direct4x4(int local) noexcept {
        direct4x4 |= static_cast<std::uint16_t>(1u << static_cast<unsigned>(local));
    }

    void reset() {
        *this = MbData{};
    }
};

static_assert(sizeof(MbData) <= 170,
              "MbData must stay compact: the per-macroblock array dominates "
              "the parser working set");

struct PTypeInfo {
    std::uint8_t partition_count;
    std::uint8_t shape; // 0=16x16, 1=16x8, 2=8x16
};

inline constexpr PTypeInfo kPTypeInfo[4] = {
        {1, 0}, // P_L0_16x16
        {2, 1}, // P_L0_L0_16x8
        {2, 2}, // P_L0_L0_8x16
        {4, 0}, // P_8x8
};

struct BPartInfo {
    PredMode pred[2];
    std::uint8_t shape; // 0=16x16, 1=16x8, 2=8x16
    std::uint8_t count;
};

inline constexpr BPartInfo kBPartInfo[22] = {
        {{detail::PredMode::Direct, detail::PredMode::None}, 0, 1}, // 0
        {{detail::PredMode::L0, detail::PredMode::None}, 0, 1},     // 1
        {{detail::PredMode::L1, detail::PredMode::None}, 0, 1},     // 2
        {{detail::PredMode::Bi, detail::PredMode::None}, 0, 1},     // 3
        {{detail::PredMode::L0, detail::PredMode::L0}, 1, 2},       // 4
        {{detail::PredMode::L0, detail::PredMode::L0}, 2, 2},       // 5
        {{detail::PredMode::L1, detail::PredMode::L1}, 1, 2},       // 6
        {{detail::PredMode::L1, detail::PredMode::L1}, 2, 2},       // 7
        {{detail::PredMode::L0, detail::PredMode::L1}, 1, 2},       // 8
        {{detail::PredMode::L0, detail::PredMode::L1}, 2, 2},       // 9
        {{detail::PredMode::L1, detail::PredMode::L0}, 1, 2},       // 10
        {{detail::PredMode::L1, detail::PredMode::L0}, 2, 2},       // 11
        {{detail::PredMode::L0, detail::PredMode::Bi}, 1, 2},       // 12
        {{detail::PredMode::L0, detail::PredMode::Bi}, 2, 2},       // 13
        {{detail::PredMode::L1, detail::PredMode::Bi}, 1, 2},       // 14
        {{detail::PredMode::L1, detail::PredMode::Bi}, 2, 2},       // 15
        {{detail::PredMode::Bi, detail::PredMode::L0}, 1, 2},       // 16
        {{detail::PredMode::Bi, detail::PredMode::L0}, 2, 2},       // 17
        {{detail::PredMode::Bi, detail::PredMode::L1}, 1, 2},       // 18
        {{detail::PredMode::Bi, detail::PredMode::L1}, 2, 2},       // 19
        {{detail::PredMode::Bi, detail::PredMode::Bi}, 1, 2},       // 20
        {{detail::PredMode::Bi, detail::PredMode::Bi}, 2, 2},       // 21
};

struct SubTypeInfo {
    std::uint8_t parts;
    std::uint8_t w;
    std::uint8_t h;
};

inline constexpr SubTypeInfo kPSubTypeInfo[4] = {
        {1, 2, 2}, // 8x8
        {2, 2, 1}, // 8x4
        {2, 1, 2}, // 4x8
        {4, 1, 1}, // 4x4
};

inline constexpr SubTypeInfo kBSubTypeInfo[13] = {
        {1, 2, 2}, // 0  B_Direct_8x8
        {1, 2, 2}, // 1  B_L0_8x8
        {1, 2, 2}, // 2  B_L1_8x8
        {1, 2, 2}, // 3  B_Bi_8x8
        {2, 2, 1}, // 4  B_L0_8x4
        {2, 1, 2}, // 5  B_L0_4x8
        {2, 2, 1}, // 6  B_L1_8x4
        {2, 1, 2}, // 7  B_L1_4x8
        {2, 2, 1}, // 8  B_Bi_8x4
        {2, 1, 2}, // 9  B_Bi_4x8
        {4, 1, 1}, // 10 B_L0_4x4
        {4, 1, 1}, // 11 B_L1_4x4
        {4, 1, 1}, // 12 B_Bi_4x4
};

/*
 * Position of sub-partition `p` inside its 8x8 sub-macroblock.
 * 8x8 has one partition, 8x4 partitions stack vertically, 4x8 partitions
 * sit side by side and 4x4 partitions raster inside the 8x8 block.
 */
[[nodiscard]] constexpr std::pair<int, int> sub_part_offset(
        const SubTypeInfo& info, int p) noexcept {
    if (info.w == 1 && info.h == 1) return {p & 1, p >> 1};
    if (info.w == 1) return {p, 0};
    if (info.h == 1) return {0, p};
    return {0, 0};
}

inline constexpr bool b_sub_uses_list(int type, int list) noexcept {
    if (type == 0) return false;
    if (list == 0) {
        return type == 1 || type == 3 || type == 4 || type == 5 || type == 8 ||
               type == 9 || type == 10 || type == 12;
    }
    return type == 2 || type == 3 || type == 6 || type == 7 || type == 8 ||
           type == 9 || type == 11 || type == 12;
}

inline constexpr PredMode b_sub_pred(int type) noexcept {
    if (type == 0) return PredMode::Direct;
    if (type == 1 || type == 4 || type == 5 || type == 10) return PredMode::L0;
    if (type == 2 || type == 6 || type == 7 || type == 11) return PredMode::L1;
    return PredMode::Bi;
}

inline constexpr std::uint8_t kI16CbpTable[6] = {0, 16, 32, 15, 31, 47};

/*
 * Residual block categories.  The numbering follows the semantics of the
 * H.264 ctxBlockCat values, extended for the high-chroma syntax:
 *
 *   1/2/3/6    plane 0 Intra16x16 DC, Intra16x16 AC, 4x4, 8x8
 *   4/5        4:2:0 and 4:2:2 chroma DC and chroma AC
 *   7/8/9/10   4:4:4 plane 1 (same four block kinds)
 *   11/12/13/14 4:4:4 plane 2
 *
 * Category 0 is unused.
 */
enum ResidualCategory : int {
    kCatLumaDc0 = 1,
    kCatLumaAc0 = 2,
    kCatLuma4x4_0 = 3,
    kCatChromaDc = 4,
    kCatChromaAc = 5,
    kCatLuma8x8_0 = 6,
    kCatLumaDc1 = 7,
    kCatLumaAc1 = 8,
    kCatLuma4x4_1 = 9,
    kCatLuma8x8_1 = 10,
    kCatLumaDc2 = 11,
    kCatLumaAc2 = 12,
    kCatLuma4x4_2 = 13,
    kCatLuma8x8_2 = 14,
};

/* ctxIdxOffset bases from H.264 Table 9-5 (ctxBlockCat 0..13). */
inline constexpr std::uint16_t kCbfCtx[15] = {
        0, 85, 89, 93, 97, 101, 1012, 460, 464, 468, 1016, 472, 476, 480, 1020};
inline constexpr std::uint16_t kSigCtx[15] = {
        0, 105, 120, 134, 149, 152, 402, 484, 499, 513, 660, 528, 543, 557, 718};
inline constexpr std::uint16_t kLastCtx[15] = {
        0, 166, 181, 195, 210, 213, 417, 572, 587, 601, 690, 616, 631, 645, 748};
inline constexpr std::uint16_t kLevelCtx[15] = {
        0, 227, 237, 247, 257, 266, 426, 952, 962, 972, 708, 982, 992, 1002, 766};

/*
 * 4:2:2 chroma DC has eight coefficients and a seven-bin significance
 * pattern; the last bin is implied.  Its coeff_abs_level>1 context
 * transition stops at 8 instead of 9 (H.264 9.3.3.1.1.9, Table 9-43).
 */
inline constexpr std::uint8_t kSig422Dc[7] = {0, 0, 1, 1, 2, 2, 2};
inline constexpr std::uint8_t kAbsLevelGt1Ctx422Dc[8] = {5, 5, 5, 5, 6, 7, 8, 8};

[[nodiscard]] constexpr bool is_8x8_residual_cat(int cat) noexcept {
    return cat == kCatLuma8x8_0 || cat == kCatLuma8x8_1 || cat == kCatLuma8x8_2;
}

inline constexpr std::uint8_t kSig8x8[63] = {
        0, 1, 2, 3, 4, 5, 5, 4, 4, 3, 3, 4, 4, 4, 5, 5,
        4, 4, 4, 4, 3, 3, 6, 7, 7, 7, 8, 9, 10, 9, 8, 7,
        7, 6, 11, 12, 13, 11, 6, 7, 8, 9, 14, 10, 9, 8, 6, 11,
        12, 13, 11, 6, 9, 14, 10, 9, 11, 12, 13, 11, 14, 10, 12};

inline constexpr std::uint8_t kLast8x8[63] = {
        0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4,
        5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8};

inline constexpr std::uint8_t kZToRaster[16] = {0, 1, 4, 5, 2, 3, 6, 7,
                                               8, 9, 12, 13, 10, 11, 14, 15};

inline constexpr std::uint8_t kAbsLevel1Ctx[8] = {1, 2, 3, 4, 0, 0, 0, 0};
inline constexpr std::uint8_t kAbsLevelGt1Ctx[8] = {5, 5, 5, 5, 6, 7, 8, 9};
inline constexpr std::uint8_t kAbsLevelTransition[2][8] = {
        {1, 2, 3, 3, 4, 5, 6, 7},
        {4, 4, 4, 4, 5, 6, 7, 7}};

} // namespace detail

/*
 * Parses one slice's macroblock layer into a 4x4 MV grid.
 *
 * `rbsp` is the entire slice RBSP (header included), `slice_data_bit` the
 * exact bit position after the slice header and CABAC alignment.  The caller
 * supplies the reference lists in display order and the output grid.
 */
class SliceDataParser {
   public:
    SliceDataParser(
            const SequenceParameterSet& sps,
            const PictureParameterSet& pps,
            const SliceHeader& header,
            bool idr,
            std::span<const std::uint8_t> rbsp,
            std::size_t slice_data_bit,
            MvCell* cells,
            std::uint32_t width4x4,
            std::uint32_t height4x4,
            std::vector<detail::MbData>& mbs,
            std::uint32_t slice_id,
            const std::vector<MotionReferencePicture>& list0,
            const std::vector<MotionReferencePicture>& list1,
            MotionVectorStatus& status)
        : sps_(sps),
          pps_(pps),
          header_(header),
          idr_(idr),
          width4x4_(width4x4),
          height4x4_(height4x4),
          cells_(cells),
          mbs_(mbs),
          slice_id_(slice_id),
          list0_(list0),
          list1_(list1),
          status_(status),
          chroma_array_type_(sps.separate_colour_plane_flag ? 0 : sps.chroma_format_idc) {
        if (pps_.entropy_coding_mode_flag) {
            cabac_.emplace(rbsp, slice_data_bit);
        } else {
            cavlc_.emplace(rbsp, slice_data_bit);
        }
        mb_width_ = static_cast<std::uint32_t>(sps.pic_width_in_mbs_minus1 + 1);
        const std::uint32_t map_units =
                static_cast<std::uint32_t>(sps.pic_height_in_map_units_minus1 + 1);
        field_pic_ = header.field_pic_flag;
        mbaff_frame_ = !sps.frame_mbs_only_flag &&
                       sps.mb_adaptive_frame_field_flag &&
                       !field_pic_;
        /*
         * A field picture has PicHeightInMbs == PicHeightInMapUnits.  A
         * frame picture has FrameHeightInMbs == PicHeightInMapUnits * 2 when
         * frame_mbs_only_flag is 0 (with or without MBAFF).
         */
        mb_height_ = field_pic_
                             ? map_units
                             : map_units * (sps.frame_mbs_only_flag ? 1u : 2u);
        const std::uint64_t computed_mb_count =
                static_cast<std::uint64_t>(mb_width_) * mb_height_;
        if (computed_mb_count > std::numeric_limits<std::uint32_t>::max() ||
            computed_mb_count != mbs_.size()) {
            mb_count_ = 0;
        } else {
            mb_count_ = static_cast<std::uint32_t>(computed_mb_count);
        }
        slice_qp_ = 26 + pps.pic_init_qp_minus26 + header.slice_qp_delta;
        /*
         * High-bit-depth profiles extend the QP range by QpBdOffsetY;
         * CABAC context initialisation is defined for the full range.
         */
        max_qp_ = 51 + 6 * static_cast<int>(sps.bit_depth_luma_minus8);
        slice_qp_ = std::clamp(slice_qp_, 0, max_qp_);
        last_qp_ = slice_qp_;
        if (cabac_) {
            cabac_->init_contexts(slice_qp_, header.cabac_init_idc, is_i());
        }
    }

    [[nodiscard]] bool parse() {
        if (!prepare_slice_group_map()) {
            return false;
        }

        /*
         * first_mb_in_slice addresses a macroblock pair in MBAFF pictures
         * (H.264 7.3.4: CurrMbAddr = first_mb_in_slice * (1 + MbaffFrameFlag)).
         */
        std::uint32_t mb_addr =
                header_.first_mb_in_slice * (mbaff_frame_ ? 2u : 1u);
        if (mb_addr >= mb_count_) {
            status_ = MotionVectorStatus::Truncated;
            return false;
        }
        if (fmo_) {
            if (mb_addr >= mb_to_slice_group_.size()) {
                status_ = MotionVectorStatus::Truncated;
                return false;
            }
            slice_group_ = mb_to_slice_group_[mb_addr];
        } else {
            slice_group_ = 0;
        }

        if (is_cavlc()) {
            return parse_cavlc(mb_addr);
        }

        try {
            bool first = true;
            bool bottom_skip_pending = false;
            bool bottom_skip = false;
            while (true) {
                if (mb_addr >= mb_count_ || mb_addr >= mbs_.size()) {
                    /*
                     * A single-slice picture may end after its last MB; the
                     * encoder does not need to code an end_of_slice_flag past
                     * the last macroblock of the picture.  With slice groups
                     * a slice can end before the picture does, and then the
                     * terminate bin is mandatory (checked below).
                     */
                    if (!first && mb_addr == mb_count_) break;
                    status_ = MotionVectorStatus::Truncated;
                    return false;
                }

                if (first && is_i()) {
                    // I slices contain no motion.  The caller already emits an
                    // intra grid; no slice-data traversal is needed at all.
                    return true;
                }
                first = false;

                if (mbaff_frame_ && (mb_addr & 1u) == 0 && pair_x(mb_addr) == 0) {
                    predict_pair_field_flag(mb_addr);
                }

                bool skip = false;
                if (!is_i()) {
                    if (mbaff_frame_ && (mb_addr & 1u) != 0 && bottom_skip_pending) {
                        skip = bottom_skip;
                        bottom_skip_pending = false;
                    } else {
                        skip = decode_skip_flag(mb_addr);
                    }
                }

                if (!is_i() && skip) {
                    /*
                     * In an MBAFF frame the bottom MB's skip flag follows the
                     * top MB's skip flag.  Mark the top MB first so the bottom
                     * skip-flag context sees the correct neighbour state.
                     */
                    if (mbaff_frame_ && (mb_addr & 1u) == 0 && mb_addr + 1 < mb_count_) {
                        apply_skip_mb(mb_addr);
                        bottom_skip = decode_skip_flag(mb_addr + 1);
                        bottom_skip_pending = true;
                        if (!bottom_skip) {
                            ensure_pair_field_flag(mb_addr);
                            /*
                             * The pair's field/frame decision was not known
                             * when the top skip MB was first applied.  Re-run
                             * the skip prediction with the correct mode.
                             */
                            if (mbs_[mb_addr].field_mb != pair_field_flag_) {
                                apply_skip_mb(mb_addr);
                            }
                        }
                    } else {
                        apply_skip_mb(mb_addr);
                    }
                } else {
                    if (mbaff_frame_) ensure_pair_field_flag(mb_addr);
                    parse_mb(mb_addr);
                    if (status_ != MotionVectorStatus::Ok) {
                        return false;
                    }
                }

                /*
                 * H.264 7.3.4 keeps MBAFF pairs together: end_of_slice_flag
                 * is only present after the bottom MB of a pair.
                 */
                if (mbaff_frame_ && (mb_addr & 1u) == 0) {
                    ++mb_addr;
                    continue;
                }

                /*
                 * The final macroblock of the picture may omit the terminate
                 * bin (it carries no further decision).  For every earlier
                 * macroblock, including the last macroblock of a slice group,
                 * the bin decides whether the next address belongs to this
                 * slice.
                 */
                if (mb_addr + 1u >= mb_count_) {
                    break;
                }
                if (cabac().decode_terminate()) {
                    break;
                }

                const std::uint32_t next = next_mb_address(mb_addr);
                if (next >= mb_count_) {
                    status_ = MotionVectorStatus::Truncated;
                    return false;
                }
                mb_addr = next;
            }
        } catch (const CabacError&) {
            status_ = MotionVectorStatus::Truncated;
            return false;
        } catch (...) {
            status_ = MotionVectorStatus::Malformed;
            return false;
        }

        return true;
    }

   private:
    static constexpr std::uint32_t kInvalidAddr = 0xFFFFFFFFu;

    [[nodiscard]] bool is_cavlc() const noexcept { return !pps_.entropy_coding_mode_flag; }
    [[nodiscard]] CabacDecoder& cabac() {
        if (!cabac_) throw CabacError("CABAC: decoder not initialised");
        return *cabac_;
    }
    [[nodiscard]] CavlcReader& cavlc() {
        if (!cavlc_) throw CavlcError("CAVLC: reader not initialised");
        return *cavlc_;
    }
    [[nodiscard]] bool is_i() const noexcept { return header_.slice_type == SliceType::I; }
    [[nodiscard]] bool is_b() const noexcept { return header_.slice_type == SliceType::B; }
    [[nodiscard]] bool is_p() const noexcept { return header_.slice_type == SliceType::P; }

    /*
     * Macroblock addressing
     * ---------------------
     * field picture / non-MBAFF frame: raster over picture macroblocks.
     * MBAFF frame: the address is 2 * pair_index + parity, where pair_index
     * rasters over macroblock pairs.  This matches H.264 7.3.4, where the
     * top macroblock of a pair has an even address.
     */
    [[nodiscard]] std::uint32_t pair_index(std::uint32_t addr) const noexcept {
        return addr / 2u;
    }
    [[nodiscard]] std::uint32_t pair_x(std::uint32_t addr) const noexcept {
        return pair_index(addr) % mb_width_;
    }
    [[nodiscard]] std::uint32_t pair_y(std::uint32_t addr) const noexcept {
        return pair_index(addr) / mb_width_;
    }
    [[nodiscard]] std::uint32_t mb_x(std::uint32_t addr) const noexcept {
        return mbaff_frame_ ? pair_x(addr) : addr % mb_width_;
    }
    [[nodiscard]] std::uint32_t mb_y(std::uint32_t addr) const noexcept {
        return mbaff_frame_ ? pair_y(addr) * 2u + (addr & 1u) : addr / mb_width_;
    }
    /* Address of the macroblock containing a 4x4 cell of the output grid. */
    [[nodiscard]] std::uint32_t mb_addr_for_cell(std::uint32_t x4, std::uint32_t y4) const noexcept {
        if (mbaff_frame_) {
            const std::uint32_t px = x4 / 4u;
            const std::uint32_t frame_y = y4 / 4u;
            const std::uint32_t pair = (frame_y / 2u) * mb_width_ + px;
            return pair * 2u + (frame_y & 1u);
        }
        return (y4 / 4u) * mb_width_ + (x4 / 4u);
    }

    /*
     * FMO (H.264 8.2.2/8.2.3).  The map is derived once per slice because
     * slice-group map types 3/4/5 consume the slice header's
     * slice_group_change_cycle.
     */
    [[nodiscard]] bool prepare_slice_group_map() {
        fmo_ = pps_.num_slice_groups_minus1 != 0;
        if (!fmo_) {
            mb_to_slice_group_.clear();
            return true;
        }
        SliceGroupMaps maps;
        std::string error;
        if (!derive_mb_to_slice_group_map(
                    sps_, pps_, header_, mbaff_frame_, mb_count_, maps, &error)) {
            status_ = MotionVectorStatus::Malformed;
            return false;
        }
        mb_to_slice_group_ = std::move(maps.mb_to_group);
        return true;
    }

    [[nodiscard]] std::uint32_t next_mb_address(std::uint32_t addr) const noexcept {
        if (addr + 1u >= mb_count_) {
            return mb_count_;
        }
        if (!fmo_) {
            return addr + 1u;
        }
        return avc::next_mb_address(mb_to_slice_group_, addr, slice_group_);
    }

    [[nodiscard]] detail::MbData* mb_at(std::uint32_t addr) noexcept {
        return addr < mbs_.size() && mbs_[addr].available &&
                               mbs_[addr].slice_id == slice_id_ &&
                               mbs_[addr].slice_group == slice_group_
                       ? &mbs_[addr]
                       : nullptr;
    }
    [[nodiscard]] const detail::MbData* mb_at(std::uint32_t addr) const noexcept {
        return addr < mbs_.size() && mbs_[addr].available &&
                               mbs_[addr].slice_id == slice_id_ &&
                               mbs_[addr].slice_group == slice_group_
                       ? &mbs_[addr]
                       : nullptr;
    }

    /*
     * Neighbour addressing for MBAFF (H.264 6.4.10, 8.4.1.3.1).
     *
     * left: the macroblock in the pair to the left with the same parity.
     * top:  field MB -> the same field's MB in the pair above when that pair
     *       is field-coded, otherwise the frame MB directly above;
     *       frame MB -> the frame MB directly above (which is the other MB of
     *       the same pair for a bottom MB).
     */
    [[nodiscard]] std::uint32_t left_addr(std::uint32_t addr) const noexcept {
        if (mbaff_frame_) {
            return pair_x(addr) > 0 ? addr - 2u : kInvalidAddr;
        }
        return (addr % mb_width_) > 0 ? addr - 1u : kInvalidAddr;
    }

    [[nodiscard]] std::uint32_t top_addr(std::uint32_t addr) const noexcept {
        if (!mbaff_frame_) {
            return (addr / mb_width_) > 0 ? addr - mb_width_ : kInvalidAddr;
        }
        const std::uint32_t py = pair_y(addr);

        const bool field = addr < mbs_.size() && mbs_[addr].field_mb;
        if (field) {
            if (py == 0) return kInvalidAddr;
            const std::uint32_t same = addr - 2u * mb_width_;
            if ((addr & 1u) != 0) return same;
            const detail::MbData* above = mb_at(same);
            return (above != nullptr && above->field_mb) ? same : same + 1u;
        }

        /*
         * A frame-coded bottom macroblock's above neighbour is the top
         * macroblock of the same pair, including in the first pair row.
         */
        if ((addr & 1u) != 0) return addr - 1u;
        if (py == 0) return kInvalidAddr;
        return addr - 2u * mb_width_ + 1u;
    }

    [[nodiscard]] detail::MbData* left_mb(std::uint32_t addr) noexcept {
        const std::uint32_t n = left_addr(addr);
        return n == kInvalidAddr ? nullptr : mb_at(n);
    }
    [[nodiscard]] const detail::MbData* left_mb(std::uint32_t addr) const noexcept {
        const std::uint32_t n = left_addr(addr);
        return n == kInvalidAddr ? nullptr
                                 : const_cast<SliceDataParser*>(this)->mb_at(n);
    }
    [[nodiscard]] detail::MbData* top_mb(std::uint32_t addr) noexcept {
        const std::uint32_t n = top_addr(addr);
        return n == kInvalidAddr ? nullptr : mb_at(n);
    }
    [[nodiscard]] const detail::MbData* top_mb(std::uint32_t addr) const noexcept {
        const std::uint32_t n = top_addr(addr);
        return n == kInvalidAddr ? nullptr
                                 : const_cast<SliceDataParser*>(this)->mb_at(n);
    }

    [[nodiscard]] const MvCell* cell_const(std::uint32_t x4, std::uint32_t y4) const noexcept {
        if (x4 >= width4x4_ || y4 >= height4x4_) return nullptr;
        return &cells_[static_cast<std::size_t>(y4) * width4x4_ + x4];
    }

    /*
     * Absolute MVD cache for the CABAC context derivation.  Values are
     * stored saturated at 255; the context thresholds (3 and 32) are
     * preserved exactly because saturation only affects magnitudes that are
     * already far above both thresholds.
     */
    [[nodiscard]] int abs_mvd_at(
            std::uint32_t x4,
            std::uint32_t y4,
            int list,
            int comp) const noexcept {
        if (x4 >= width4x4_ || y4 >= height4x4_) return 0;
        const std::uint32_t addr = mb_addr_for_cell(x4, y4);
        if (addr >= mbs_.size() || !mbs_[addr].available ||
            mbs_[addr].slice_id != slice_id_) {
            return 0;
        }
        const int local = static_cast<int>((y4 % 4) * 4 + (x4 % 4));
        return mbs_[addr].abs_mvd[list][local][comp];
    }

    static void store_abs_mvd(std::uint8_t& slot, int value) noexcept {
        const int magnitude = value < 0 ? -value : value;
        slot = static_cast<std::uint8_t>(magnitude > 255 ? 255 : magnitude);
    }

    /* Number of active frame reference pictures in one list. */
    [[nodiscard]] std::uint32_t ref_count(int list) const noexcept {
        const auto& list_ref = list == 0 ? list0_ : list1_;
        if (list_ref.empty()) return 0;
        const int active = list == 0 ? header_.num_ref_idx_l0_active_minus1
                                     : header_.num_ref_idx_l1_active_minus1;
        const std::size_t count = std::min<std::size_t>(
                static_cast<std::size_t>(active) + 1u, list_ref.size());
        return static_cast<std::uint32_t>(count);
    }

    /*
     * Number of ref_idx values coded for the macroblock.  A field-coded MB
     * in an MBAFF frame addresses two reference fields per frame reference,
     * so the coded range is doubled (H.264 7.4.5.3, 8.4.2.2.1).
     */
    [[nodiscard]] std::uint32_t syntax_ref_count(int list, bool field_mb) const noexcept {
        /*
         * Number of ref_idx values coded in the slice, from
         * NumRefIdxActive (7.4.3.1).  This is independent of how many
         * reference pictures this parser currently has available.
         */
        const int active = list == 0 ? header_.num_ref_idx_l0_active_minus1
                                     : header_.num_ref_idx_l1_active_minus1;
        const std::uint32_t count = static_cast<std::uint32_t>(active) + 1u;
        return (mbaff_frame_ && field_mb) ? count * 2u : count;
    }

    [[nodiscard]] std::int32_t ref_poc(int list, int ref_idx) const noexcept {
        const auto& list_ref = list == 0 ? list0_ : list1_;
        if (list_ref.empty()) return std::numeric_limits<std::int32_t>::min();
        if (ref_idx < 0 || static_cast<std::size_t>(ref_idx) >= list_ref.size()) {
            return std::numeric_limits<std::int32_t>::min();
        }
        return list_ref[static_cast<std::size_t>(ref_idx)].poc;
    }

    struct NeighMv {
        bool available = false;
        bool list_used = false;
        bool matches = false;
        int ref = -2;
        std::int16_t mv[2] = {0, 0};
    };

    /*
     * Shared tail of neighbour_mv(): validate the owning macroblock once and
     * decode one already-addressed cell.
     */
    [[nodiscard]] BS_AVC_MB_ALWAYS_INLINE NeighMv neighbour_mv_from_cell(
            const MvCell* c,
            std::uint32_t addr,
            int list,
            int ref_idx) const noexcept {
        NeighMv n;
        if (addr >= mbs_.size() || !mbs_[addr].available ||
            mbs_[addr].slice_id != slice_id_) {
            return n;
        }
        if (c->flags & MvUnavailable) return n;
        n.available = true;
        n.ref = -1;
        if (c->flags & MvIntra) return n;
        if ((c->flags & (list == 0 ? MvList0 : MvList1)) == 0) return n;
        n.list_used = true;
        n.ref = c->ref_idx[list];
        n.mv[0] = c->mv[list][0];
        n.mv[1] = c->mv[list][1];
        if (mbaff_frame_ && current_mb_field_) n.mv[1] = static_cast<std::int16_t>(n.mv[1] / 2);
        n.matches = c->ref_idx[list] == ref_idx;
        return n;
    }

    [[nodiscard]] NeighMv neighbour_mv(
            std::uint32_t x4,
            std::uint32_t y4,
            int list,
            int ref_idx) const noexcept {
        /*
         * Progressive pictures keep macroblocks in a plain raster, so the
         * owning macroblock and cell are pure index arithmetic.  The bounds
         * test also guards the direct grid access below.
         */
        if (!mbaff_frame_ && !field_pic_ && x4 < width4x4_ && y4 < height4x4_) {
            const std::uint32_t addr = (y4 >> 2) * mb_width_ + (x4 >> 2);
            return neighbour_mv_from_cell(
                    cells_ + static_cast<std::size_t>(y4) * width4x4_ + x4,
                    addr, list, ref_idx);
        }
        const std::uint32_t addr = mb_addr_for_cell(x4, y4);
        const MvCell* c = cell_const(x4, y4);
        if (c == nullptr) return NeighMv{};
        return neighbour_mv_from_cell(c, addr, list, ref_idx);
    }

    [[nodiscard]] static std::int16_t median3(
            std::int16_t a, std::int16_t b, std::int16_t c) noexcept {
        if (a > b) std::swap(a, b);
        if (b > c) std::swap(b, c);
        if (a > b) std::swap(a, b);
        return b;
    }

    [[nodiscard]] NeighMv diagonal_neighbour(
            std::uint32_t x4,
            std::uint32_t y4,
            int list,
            int ref_idx,
            int part_w4) const noexcept {
        NeighMv c;
        if (y4 > 0 && x4 + static_cast<std::uint32_t>(part_w4) < width4x4_) {
            const std::uint32_t nx = x4 + static_cast<std::uint32_t>(part_w4);
            if (!mbaff_frame_ && !field_pic_) {
                const std::uint32_t ny = y4 - 1;
                const std::uint32_t addr = (ny >> 2) * mb_width_ + (nx >> 2);
                c = neighbour_mv_from_cell(
                        cells_ + static_cast<std::size_t>(ny) * width4x4_ + nx,
                        addr, list, ref_idx);
            } else {
                c = neighbour_mv(nx, y4 - 1, list, ref_idx);
            }
        }
        if (!c.available && x4 > 0 && y4 > 0) {
            if (!mbaff_frame_ && !field_pic_) {
                const std::uint32_t nx = x4 - 1;
                const std::uint32_t ny = y4 - 1;
                const std::uint32_t addr = (ny >> 2) * mb_width_ + (nx >> 2);
                c = neighbour_mv_from_cell(
                        cells_ + static_cast<std::size_t>(ny) * width4x4_ + nx,
                        addr, list, ref_idx);
            } else {
                c = neighbour_mv(x4 - 1, y4 - 1, list, ref_idx);
            }
        }
        return c;
    }

    void predict_mv(
            std::uint32_t x4,
            std::uint32_t y4,
            int part_w4,
            int /*part_h4*/,
            int list,
            int ref_idx,
            std::int16_t out[2]) const {
        NeighMv a;
        NeighMv b;
        if (x4 > 0) a = neighbour_mv(x4 - 1, y4, list, ref_idx);
        if (y4 > 0) b = neighbour_mv(x4, y4 - 1, list, ref_idx);
        const NeighMv c = diagonal_neighbour(x4, y4, list, ref_idx, part_w4);

        const int count = (a.matches ? 1 : 0) + (b.matches ? 1 : 0) + (c.matches ? 1 : 0);
        if (count > 1) {
            out[0] = median3(a.mv[0], b.mv[0], c.mv[0]);
            out[1] = median3(a.mv[1], b.mv[1], c.mv[1]);
            return;
        }
        if (count == 1) {
            const NeighMv& pick = a.matches ? a : (b.matches ? b : c);
            out[0] = pick.mv[0];
            out[1] = pick.mv[1];
            return;
        }
        if (!b.available && !c.available && a.available) {
            out[0] = a.mv[0];
            out[1] = a.mv[1];
            return;
        }
        out[0] = median3(a.mv[0], b.mv[0], c.mv[0]);
        out[1] = median3(a.mv[1], b.mv[1], c.mv[1]);
    }

    void predict_16x8_mv(
            std::uint32_t x4,
            std::uint32_t y4,
            bool top,
            int list,
            int ref_idx,
            std::int16_t out[2]) const {
        if (top) {
            if (y4 > 0) {
                const NeighMv b = neighbour_mv(x4, y4 - 1, list, ref_idx);
                if (b.matches) {
                    out[0] = b.mv[0];
                    out[1] = b.mv[1];
                    return;
                }
            }
        } else if (x4 > 0) {
            const NeighMv a = neighbour_mv(x4 - 1, y4, list, ref_idx);
            if (a.matches) {
                out[0] = a.mv[0];
                out[1] = a.mv[1];
                return;
            }
        }
        predict_mv(x4, y4, 4, 2, list, ref_idx, out);
    }

    void predict_8x16_mv(
            std::uint32_t x4,
            std::uint32_t y4,
            bool left,
            int list,
            int ref_idx,
            std::int16_t out[2]) const {
        if (left) {
            if (x4 > 0) {
                const NeighMv a = neighbour_mv(x4 - 1, y4, list, ref_idx);
                if (a.matches) {
                    out[0] = a.mv[0];
                    out[1] = a.mv[1];
                    return;
                }
            }
        } else {
            const NeighMv c = diagonal_neighbour(x4, y4, list, ref_idx, 2);
            if (c.matches) {
                out[0] = c.mv[0];
                out[1] = c.mv[1];
                return;
            }
        }
        predict_mv(x4, y4, 2, 4, list, ref_idx, out);
    }

    /* H.264 8.4.1.3.2: P_Skip uses the zero-motion shortcut before the
     * regular median predictor. */
    void predict_pskip_mv(std::uint32_t x4, std::uint32_t y4, std::int16_t out[2]) const {
        NeighMv a;
        NeighMv b;
        NeighMv c;
        if (x4 > 0) a = neighbour_mv(x4 - 1, y4, 0, 0);
        if (!a.available) {
            out[0] = out[1] = 0;
            return;
        }
        if (a.list_used && a.matches && a.mv[0] == 0 && a.mv[1] == 0) {
            out[0] = out[1] = 0;
            return;
        }
        if (y4 > 0) b = neighbour_mv(x4, y4 - 1, 0, 0);
        if (!b.available) {
            out[0] = out[1] = 0;
            return;
        }
        if (b.list_used && b.matches && b.mv[0] == 0 && b.mv[1] == 0) {
            out[0] = out[1] = 0;
            return;
        }
        c = diagonal_neighbour(x4, y4, 0, 0, 4);
        const int count = (a.matches ? 1 : 0) + (b.matches ? 1 : 0) + (c.matches ? 1 : 0);
        if (count > 1) {
            out[0] = median3(a.mv[0], b.mv[0], c.mv[0]);
            out[1] = median3(a.mv[1], b.mv[1], c.mv[1]);
            return;
        }
        if (count == 1) {
            const NeighMv& pick = a.matches ? a : (b.matches ? b : c);
            out[0] = pick.mv[0];
            out[1] = pick.mv[1];
            return;
        }
        out[0] = median3(a.mv[0], b.mv[0], c.mv[0]);
        out[1] = median3(a.mv[1], b.mv[1], c.mv[1]);
    }

    void write_partition(
            std::uint32_t mb_addr,
            int px,
            int py,
            int pw,
            int ph,
            detail::PredMode pred,
            detail::RefChoice ref0 = {},
            detail::RefChoice ref1 = {},
            const std::int16_t* pre_mvd0 = nullptr,
            const std::int16_t* pre_mvd1 = nullptr) {
        if (pred == detail::PredMode::None) return;
        if (pred == detail::PredMode::Direct) {
            apply_direct(mb_addr, mbs_[mb_addr]);
            return;
        }

        const std::uint32_t base_x = mb_x(mb_addr) * 4;
        const std::uint32_t base_y = mb_y(mb_addr) * 4;
        const std::uint32_t x4 = base_x + static_cast<std::uint32_t>(px);
        const std::uint32_t y4 = base_y + static_cast<std::uint32_t>(py);

        int ref_idx[2] = {0, 0};
        bool has_list[2] = {false, false};

        // H.264 parses every partition's ref_idx before any mvd, and list 0
        // before list 1.  This loop mirrors that order.
        for (int list = 0; list < 2; ++list) {
            const bool uses = pred == detail::PredMode::Bi ||
                              (pred == detail::PredMode::L0 && list == 0) ||
                              (pred == detail::PredMode::L1 && list == 1);
            if (!uses) continue;

            std::uint32_t count = syntax_ref_count(list, mbs_[mb_addr].field_mb);
            if (count == 0 && list == 1 && is_b()) {
                count = syntax_ref_count(0, mbs_[mb_addr].field_mb);
            }
            if (count == 0) continue;

            const detail::RefChoice& choice = list == 0 ? ref0 : ref1;
            int raw = choice.fixed ? choice.index : 0;
            if (!choice.fixed && count > 1) {
                raw = decode_ref(list, x4, y4);
                if (raw < 0 || static_cast<std::uint32_t>(raw) >= count) raw = 0;
            }
            /* The output grid keeps frame reference indices. */
            const int value = (mbaff_frame_ && mbs_[mb_addr].field_mb) ? raw / 2 : raw;
            ref_idx[list] = value;
            has_list[list] = true;
            fill_ref_cache(mb_addr, list, x4, y4, pw, ph, raw);
        }

        std::int16_t mv_out[2][2] = {{0, 0}, {0, 0}};
        std::int16_t mvd_out[2][2] = {{0, 0}, {0, 0}};
        for (int list = 0; list < 2; ++list) {
            if (!has_list[list]) continue;

            std::int16_t mvp[2];
            if (pw == 4 && ph == 2) {
                predict_16x8_mv(x4, y4, py == 0, list, ref_idx[list], mvp);
            } else if (pw == 2 && ph == 4) {
                predict_8x16_mv(x4, y4, px == 0, list, ref_idx[list], mvp);
            } else {
                predict_mv(x4, y4, pw, ph, list, ref_idx[list], mvp);
            }
            std::int16_t mvd[2] = {0, 0};
            const std::int16_t* pre = list == 0 ? pre_mvd0 : pre_mvd1;
            if (pre != nullptr) {
                mvd[0] = pre[0];
                mvd[1] = pre[1];
            } else {
                decode_mvd(list, x4, y4, mvd);
            }

            const std::int16_t mvx = static_cast<std::int16_t>(mvp[0] + mvd[0]);
            const std::int16_t mvy = static_cast<std::int16_t>(mvp[1] + mvd[1]);
            mv_out[list][0] = mvx;
            mv_out[list][1] = storeMvY(mvy, mbs_[mb_addr].field_mb);
            mvd_out[list][0] = mvd[0];
            mvd_out[list][1] = mvd[1];
        }

        /*
         * Both lists are resolved before the grid is touched, so one
         * rectangle fill writes the whole partition with the right
         * list/bi-prediction flags.
         */
        if (!has_list[0] && !has_list[1]) return;
        const std::uint8_t extra =
                mbs_[mb_addr].field_mb ? static_cast<std::uint8_t>(MvField) : std::uint8_t{0};
        MvCell pattern{};
        if (has_list[0] && has_list[1]) {
            pattern = simd::CellPattern::biPartition(
                              mv_out[0][0], mv_out[0][1],
                              static_cast<std::int8_t>(ref_idx[0]),
                              mv_out[1][0], mv_out[1][1],
                              static_cast<std::int8_t>(ref_idx[1]), extra)
                              .cell;
        } else if (has_list[0]) {
            pattern = simd::CellPattern::l0Partition(
                              mv_out[0][0], mv_out[0][1],
                              static_cast<std::int8_t>(ref_idx[0]), extra)
                              .cell;
        } else {
            pattern = simd::CellPattern::l1Partition(
                              mv_out[1][0], mv_out[1][1],
                              static_cast<std::int8_t>(ref_idx[1]), extra)
                              .cell;
        }
        simd::fillBlock(cells_, width4x4_, x4, y4,
                        static_cast<std::uint32_t>(pw),
                        static_cast<std::uint32_t>(ph), pattern);

        for (int list = 0; list < 2; ++list) {
            if (!has_list[list]) continue;
            for (int yy = 0; yy < ph; ++yy) {
                const int local_row =
                        static_cast<int>((y4 + static_cast<std::uint32_t>(yy)) % 4);
                for (int xx = 0; xx < pw; ++xx) {
                    const int local = local_row * 4 +
                                      static_cast<int>((x4 + static_cast<std::uint32_t>(xx)) % 4);
                    store_abs_mvd(mbs_[mb_addr].abs_mvd[list][local][0], mvd_out[list][0]);
                    store_abs_mvd(mbs_[mb_addr].abs_mvd[list][local][1], mvd_out[list][1]);
                }
            }
        }
    }

    /*
     * Output-grid vertical MV scaling.  MBAFF field MBs are stored in frame
     * line units; field pictures keep their native field-line units.
     */
    [[nodiscard]] std::int16_t storeMvY(std::int16_t y, bool field_mb) const noexcept {
        if (!mbaff_frame_ || !field_mb) return y;
        const int scaled = static_cast<int>(y) * 2;
        return static_cast<std::int16_t>(
                std::clamp(scaled, -32768, 32767));
    }

    void mark_intra_mb(std::uint32_t mb_addr) {
        const std::uint32_t base_x = mb_x(mb_addr) * 4;
        const std::uint32_t base_y = mb_y(mb_addr) * 4;
        const std::uint8_t extra =
                mbs_[mb_addr].field_mb ? static_cast<std::uint8_t>(MvField) : std::uint8_t{0};
        simd::fillBlock(cells_, width4x4_, base_x, base_y, 4, 4,
                        simd::CellPattern::intra(extra).cell);
    }

    // ---- CABAC syntax elements -------------------------------------------

    [[nodiscard]] bool decode_cabac_field_decoding_flag(std::uint32_t mb_addr) {
        /*
         * H.264 Table 9-11: mb_field_decoding_flag uses ctxIdxOffset 70 with
         * two neighbour-dependent increments (see 9.3.3.1.1.5).
         */
        int ctx = 0;
        if (pair_x(mb_addr) > 0 && pair_field_flag_) ++ctx;

        const std::uint32_t pair = pair_index(mb_addr);
        if (pair >= mb_width_) {
            const std::uint32_t above = (pair - mb_width_) * 2u;
            const auto* above_mb = mb_at(above);
            if (above_mb != nullptr && above_mb->field_mb) ++ctx;
        }
        return cabac().decode(cabac().ctx(70 + ctx));
    }

    void read_mb_field_flag(std::uint32_t mb_addr) {
        if (!mbaff_frame_) return;
        pair_field_flag_ = is_cavlc() ? cavlc().read_bit()
                                      : decode_cabac_field_decoding_flag(mb_addr);
        pair_flag_pair_ = pair_index(mb_addr);
    }

    void ensure_pair_field_flag(std::uint32_t mb_addr) {
        if (!mbaff_frame_) return;
        if (pair_flag_pair_ == pair_index(mb_addr)) return;
        read_mb_field_flag(mb_addr);
    }

    /*
     * H.264 7.4.4: when neither MB of a pair carries
     * mb_field_decoding_flag, the value is inferred from the pair to the left
     * in the same slice, otherwise from the pair above, otherwise 0.
     */
    [[nodiscard]] bool infer_pair_field_flag(std::uint32_t mb_addr) const noexcept {
        const std::uint32_t pair = pair_index(mb_addr);
        if (pair_x(mb_addr) > 0) {
            const std::uint32_t left = (pair - 1u) * 2u;
            if (const auto* lm = mb_at(left)) return lm->field_mb;
        }
        if (pair >= mb_width_) {
            const std::uint32_t above = (pair - mb_width_) * 2u;
            if (const auto* am = mb_at(above)) return am->field_mb;
        }
        return false;
    }

    /*
     * H.264 9.3.3.1.1.2 / FFmpeg's predict_field_decoding_flag: at the first
     * pair of a row the CABAC neighbour state is predicted from the pair to
     * the left (last pair of the previous row) or, if that pair belongs to a
     * different slice, from the pair above.  No syntax bit is consumed.
     */
    void predict_pair_field_flag(std::uint32_t mb_addr) {
        if (!mbaff_frame_ || pair_index(mb_addr) == 0) return;
        const std::uint32_t left = mb_addr - 2u;
        if (const auto* lm = mb_at(left)) {
            pair_field_flag_ = lm->field_mb;
            return;
        }
        if (pair_y(mb_addr) > 0) {
            const std::uint32_t above = mb_addr - 2u * mb_width_ + 1u;
            if (const auto* am = mb_at(above)) pair_field_flag_ = am->field_mb;
        }
    }

    [[nodiscard]] bool decode_skip_flag(std::uint32_t mb_addr) {
        int ctx = 11;
        if (mbaff_frame_) {
            /*
             * H.264 9.3.3.1.1.6 / FFmpeg's decode_cabac_mb_skip: the MBAFF
             * neighbour selection for the skip-flag context is not the same
             * as the MV-prediction neighbour selection.  It uses the top MB
             * of the pair for the left candidate and the top MB of the pair
             * above for the top candidate.
             */
            const std::uint32_t pair = pair_index(mb_addr);
            /*
             * FFmpeg uses the slice's current mb_field_decoding_flag, which
             * at a top MB is still the previous pair's decision (the current
             * pair's flag is decoded after the skip flag), and at a bottom MB
             * is the current pair's flag when it was read for the top MB.
             */
            const bool field = pair_field_flag_;

            std::uint32_t left = pair * 2u - 2u;
            if (pair == 0) left = kInvalidAddr;
            if (left != kInvalidAddr && (mb_addr & 1u) != 0) {
                const detail::MbData* lm = mb_at(left);
                if (lm != nullptr && lm->field_mb == field) {
                    left += 2u;
                }
            }

            std::uint32_t top = kInvalidAddr;
            if (pair >= mb_width_) {
                const std::uint32_t above_top = (pair - mb_width_) * 2u;
                if (!field) {
                    top = (mb_addr & 1u) != 0 ? mb_addr - 1u : above_top + 1u;
                } else {
                    /*
                     * Field-coded pair: the candidate above is the bottom MB
                     * of the pair above.  For a top MB, a field-coded
                     * candidate selects the top MB of that pair instead
                     * (H.264 9.3.3.1.1.6 / 8.4.1.3.1).
                     */
                    top = above_top + 1u;
                    if ((mb_addr & 1u) == 0) {
                        const detail::MbData* am = mb_at(top);
                        if (am != nullptr && am->field_mb) top = above_top;
                    }
                }
            }

            const detail::MbData* lm = left == kInvalidAddr ? nullptr : mb_at(left);
            const detail::MbData* tm = top == kInvalidAddr ? nullptr : mb_at(top);
            if (lm != nullptr && !lm->skip) ++ctx;
            if (tm != nullptr && !tm->skip) ++ctx;
        } else {
            const auto* left = left_mb(mb_addr);
            const auto* top = top_mb(mb_addr);
            if (left != nullptr && !left->skip) ++ctx;
            if (top != nullptr && !top->skip) ++ctx;
        }
        if (is_b()) ctx += 13;
        return cabac().decode(cabac().ctx(ctx));
    }

    [[nodiscard]] int decode_intra_mb_type(int ctx_base, bool intra_slice) {
        auto* base = &cabac().ctx(ctx_base);
        if (intra_slice) {
            if (!cabac().decode(base[0])) {
                return 0; // I_NxN
            }
            if (cabac().decode_terminate()) return 25; // I_PCM
            int mb_type = 1;
            mb_type += 12 * (cabac().decode(base[3]) ? 1 : 0);
            if (cabac().decode(base[4])) {
                mb_type += 4;
                if (cabac().decode(base[5])) mb_type += 4;
            }
            mb_type += 2 * (cabac().decode(base[6]) ? 1 : 0);
            mb_type += (cabac().decode(base[7]) ? 1 : 0);
            return mb_type;
        }

        if (!cabac().decode(base[0])) {
            return 0; // I_NxN
        }
        if (cabac().decode_terminate()) return 25; // I_PCM
        int mb_type = 1;
        mb_type += 12 * (cabac().decode(base[1]) ? 1 : 0);
        if (cabac().decode(base[2])) {
            mb_type += 4;
            if (cabac().decode(base[2])) mb_type += 4;
        }
        mb_type += 2 * (cabac().decode(base[3]) ? 1 : 0);
        mb_type += (cabac().decode(base[3]) ? 1 : 0);
        return mb_type;
    }

    [[nodiscard]] int decode_p_mb_type() {
        auto* base = &cabac().ctx(14);
        if (cabac().decode(base[0])) { // intra
            return 5 + decode_intra_mb_type(17, false);
        }
        if (!cabac().decode(base[1])) {
            return 3 * (cabac().decode(base[2]) ? 1 : 0); // 16x16 or 8x8
        }
        return 2 - (cabac().decode(base[3]) ? 1 : 0); // 8x16 or 16x8
    }

    [[nodiscard]] int decode_b_mb_type(std::uint32_t mb_addr) {
        const auto* left = left_mb(mb_addr);
        const auto* top = top_mb(mb_addr);
        int ctx = 0;
        if (left != nullptr && !left->direct) ++ctx;
        if (top != nullptr && !top->direct) ++ctx;
        auto* base = &cabac().ctx(27 + ctx);
        if (!cabac().decode(base[0])) return 0; // direct
        if (!cabac().decode(cabac().ctx(30))) {
            return 1 + (cabac().decode(cabac().ctx(32)) ? 1 : 0);
        }
        int bits = (cabac().decode(cabac().ctx(31)) ? 8 : 0);
        bits += (cabac().decode(cabac().ctx(32)) ? 4 : 0);
        bits += (cabac().decode(cabac().ctx(32)) ? 2 : 0);
        bits += (cabac().decode(cabac().ctx(32)) ? 1 : 0);
        if (bits < 8) return bits + 3;
        if (bits == 13) return 23 + decode_intra_mb_type(32, false);
        if (bits == 14) return 11;
        if (bits == 15) return 22;
        bits = (bits << 1) + (cabac().decode(cabac().ctx(32)) ? 1 : 0);
        return bits - 4;
    }

    [[nodiscard]] int decode_sub_mb_type_p() {
        if (cabac().decode(cabac().ctx(21))) return 0;
        if (!cabac().decode(cabac().ctx(22))) return 1;
        return cabac().decode(cabac().ctx(23)) ? 2 : 3;
    }

    [[nodiscard]] int decode_sub_mb_type_b() {
        if (!cabac().decode(cabac().ctx(36))) return 0;
        if (!cabac().decode(cabac().ctx(37))) {
            return 1 + (cabac().decode(cabac().ctx(39)) ? 1 : 0);
        }
        int type = 3;
        if (cabac().decode(cabac().ctx(38))) {
            if (cabac().decode(cabac().ctx(39))) {
                return 11 + (cabac().decode(cabac().ctx(39)) ? 1 : 0);
            }
            type += 4;
        }
        type += 2 * (cabac().decode(cabac().ctx(39)) ? 1 : 0);
        type += (cabac().decode(cabac().ctx(39)) ? 1 : 0);
        return type;
    }

    [[nodiscard]] int ref_at(int list, std::uint32_t x4, std::uint32_t y4) const {
        if (x4 >= width4x4_ || y4 >= height4x4_) return -1;
        const std::uint32_t addr = mb_addr_for_cell(x4, y4);
        if (addr >= mbs_.size() || !mbs_[addr].available ||
            mbs_[addr].slice_id != slice_id_) {
            return -1;
        }
        const int local = static_cast<int>((y4 % 4) * 4 + (x4 % 4));
        return mbs_[addr].ref[list][local];
    }

    [[nodiscard]] bool direct_at(std::uint32_t x4, std::uint32_t y4) const {
        if (x4 >= width4x4_ || y4 >= height4x4_) return false;
        const std::uint32_t addr = mb_addr_for_cell(x4, y4);
        if (addr >= mbs_.size() || !mbs_[addr].available ||
            mbs_[addr].slice_id != slice_id_) {
            return false;
        }
        const int local = static_cast<int>((y4 % 4) * 4 + (x4 % 4));
        return mbs_[addr].is_direct4x4(local);
    }

    void fill_ref_cache(
            std::uint32_t mb_addr,
            int list,
            std::uint32_t x4,
            std::uint32_t y4,
            int w,
            int h,
            int ref) {
        for (int yy = 0; yy < h; ++yy) {
            for (int xx = 0; xx < w; ++xx) {
                const int local = static_cast<int>((y4 % 4 + static_cast<std::uint32_t>(yy)) * 4 +
                                                   (x4 % 4 + static_cast<std::uint32_t>(xx)));
                mbs_[mb_addr].ref[list][local] = static_cast<std::int8_t>(ref);
            }
        }
    }

    [[nodiscard]] bool field_mb_at_cell(std::uint32_t x4, std::uint32_t y4) const noexcept {
        const std::uint32_t addr = mb_addr_for_cell(x4, y4);
        return addr < mbs_.size() && mbs_[addr].available && mbs_[addr].field_mb;
    }

    [[nodiscard]] int decode_ref(int list, std::uint32_t x4, std::uint32_t y4) {
        const std::uint32_t count = syntax_ref_count(list, current_mb_field_);
        if (is_cavlc()) {
            return count > 1 ? static_cast<int>(cavlc().read_te(count)) : 0;
        }
        const int ref_a = x4 > 0 ? ref_at(list, x4 - 1, y4) : -1;
        const int ref_b = y4 > 0 ? ref_at(list, x4, y4 - 1) : -1;

        /*
         * 9.3.3.1.1.6: a frame MB treats a field neighbour's ref_idx as
         * "zero" when it is 0 or 1 (the two fields of reference frame 0).
         */
        const bool a_field = mbaff_frame_ && x4 > 0 && field_mb_at_cell(x4 - 1, y4);
        const bool b_field = mbaff_frame_ && y4 > 0 && field_mb_at_cell(x4, y4 - 1);
        const int zero_a = (mbaff_frame_ && !current_mb_field_ && a_field) ? 1 : 0;
        const int zero_b = (mbaff_frame_ && !current_mb_field_ && b_field) ? 1 : 0;
        int ctx = 0;
        if (is_b()) {
            if (ref_a > zero_a && !direct_at(x4 - 1, y4)) ctx += 1;
            if (ref_b > zero_b && !direct_at(x4, y4 - 1)) ctx += 2;
        } else {
            if (ref_a > zero_a) ctx += 1;
            if (ref_b > zero_b) ctx += 2;
        }

        int ref = 0;
        while (cabac().decode(cabac().ctx(54 + ctx))) {
            ++ref;
            ctx = (ctx >> 2) + 4;
            if (ref >= static_cast<int>(count) || ref >= 32) return -1;
        }
        return ref;
    }

    void decode_mvd(
            int list,
            std::uint32_t x4,
            std::uint32_t y4,
            std::int16_t out[2]) {
        if (is_cavlc()) {
            out[0] = static_cast<std::int16_t>(cavlc().read_se());
            out[1] = static_cast<std::int16_t>(cavlc().read_se());
            return;
        }
        /*
         * Saturating absolute MVDs cap the sum at 510, so use int rather than
         * the historical int16 accumulator (which could wrap for extreme
         * escape-coded MVDs and invert the context decision).
         */
        int amvd[2] = {0, 0};
        auto abs_mvd = [&](std::uint32_t gx, std::uint32_t gy, int comp) {
            int value = abs_mvd_at(gx, gy, list, comp);
            if (mbaff_frame_ && comp == 1) {
                const std::uint32_t addr = mb_addr_for_cell(gx, gy);
                if (addr < mbs_.size() && mbs_[addr].available) {
                    const bool neighbour_field = mbs_[addr].field_mb;
                    if (!current_mb_field_ && neighbour_field) {
                        value *= 2;  // 9-9
                    } else if (current_mb_field_ && !neighbour_field) {
                        value /= 2;  // 9-10
                    }
                }
            }
            return value;
        };
        if (x4 > 0) {
            amvd[0] += abs_mvd(x4 - 1, y4, 0);
            amvd[1] += abs_mvd(x4 - 1, y4, 1);
        }
        if (y4 > 0) {
            amvd[0] += abs_mvd(x4, y4 - 1, 0);
            amvd[1] += abs_mvd(x4, y4 - 1, 1);
        }
        for (int comp = 0; comp < 2; ++comp) {
            const int ctxbase = 40 + comp * 7;
            int mvd = 0;
            const int ctx_inc = (amvd[comp] >= 3 ? 1 : 0) + (amvd[comp] > 32 ? 1 : 0);
            const bool first = cabac().decode(cabac().ctx(ctxbase + ctx_inc));
            if (first) {
                int value = 1;
                int ctx = ctxbase + 3;
                while (value < 9 && cabac().decode(cabac().ctx(ctx))) {
                    if (value < 4) ++ctx;
                    ++value;
                }
                if (value >= 9) {
                    int k = 3;
                    while (cabac().decode_bypass()) {
                        value += 1 << k;
                        ++k;
                        if (k > 24) break;
                    }
                    while (k-- > 0) {
                        value += (cabac().decode_bypass() ? 1 : 0) << k;
                    }
                }
                mvd = cabac().decode_bypass() ? -value : value;
            }
            out[comp] = static_cast<std::int16_t>(mvd);
        }
    }

    void decode_mvd_cached(
            std::uint32_t mb_addr,
            std::uint32_t x4,
            std::uint32_t y4,
            int w,
            int h,
            int list,
            std::int16_t out[2]) {
        decode_mvd(list, x4, y4, out);
        for (int yy = 0; yy < h; ++yy) {
            for (int xx = 0; xx < w; ++xx) {
                const int local =
                        static_cast<int>((y4 % 4 + static_cast<std::uint32_t>(yy)) * 4 +
                                         (x4 % 4 + static_cast<std::uint32_t>(xx)));
                store_abs_mvd(mbs_[mb_addr].abs_mvd[list][local][0], out[0]);
                store_abs_mvd(mbs_[mb_addr].abs_mvd[list][local][1], out[1]);
            }
        }
    }

    [[nodiscard]] int decode_cbp_luma(std::uint32_t mb_addr) {
        const auto* left = left_mb(mb_addr);
        const auto* top = top_mb(mb_addr);
        const int cbp_a = left != nullptr ? left->cbp : -1;
        const int cbp_b = top != nullptr ? top->cbp : -1;
        int cbp = 0;
        int ctx = (!(cbp_a & 0x02)) + 2 * (!(cbp_b & 0x04));
        cbp += cabac().decode(cabac().ctx(73 + ctx)) ? 1 : 0;
        ctx = (!(cbp & 0x01)) + 2 * (!(cbp_b & 0x08));
        cbp += cabac().decode(cabac().ctx(73 + ctx)) ? 2 : 0;
        ctx = (!(cbp_a & 0x08)) + 2 * (!(cbp & 0x01));
        cbp += cabac().decode(cabac().ctx(73 + ctx)) ? 4 : 0;
        ctx = (!(cbp & 0x04)) + 2 * (!(cbp & 0x02));
        cbp += cabac().decode(cabac().ctx(73 + ctx)) ? 8 : 0;
        return cbp;
    }

    [[nodiscard]] int decode_cbp_chroma(std::uint32_t mb_addr) {
        const auto* left = left_mb(mb_addr);
        const auto* top = top_mb(mb_addr);
        const int cbp_a = left != nullptr ? (left->cbp >> 4) & 3 : 0;
        const int cbp_b = top != nullptr ? (top->cbp >> 4) & 3 : 0;
        int ctx = 0;
        if (cbp_a > 0) ++ctx;
        if (cbp_b > 0) ctx += 2;
        if (!cabac().decode(cabac().ctx(77 + ctx))) return 0;
        ctx = 4;
        if (cbp_a == 2) ++ctx;
        if (cbp_b == 2) ctx += 2;
        return 1 + (cabac().decode(cabac().ctx(77 + ctx)) ? 1 : 0);
    }

    [[nodiscard]] int decode_delta_qp() {
        if (is_cavlc()) {
            const int delta = cavlc().read_se();
            last_delta_qp_ = delta;
            return delta;
        }
        auto* base = &cabac().ctx(60);
        if (!cabac().decode(base[last_delta_qp_ != 0 ? 1 : 0])) {
            last_delta_qp_ = 0;
            return 0;
        }
        int value = 1;
        int ctx = 2;
        while (cabac().decode(base[ctx])) {
            ctx = 3;
            ++value;
            if (value > 2 * max_qp_) break;
        }
        const int delta = (value & 1) ? (value + 1) >> 1 : -((value + 1) >> 1);
        last_delta_qp_ = delta;
        return delta;
    }

    [[nodiscard]] int decode_intra4x4_mode() {
        if (cabac().decode(cabac().ctx(68))) {
            return 2; // predicted mode; value is irrelevant for MV extraction
        }
        int mode = 0;
        mode += cabac().decode(cabac().ctx(69)) ? 1 : 0;
        mode += cabac().decode(cabac().ctx(69)) ? 2 : 0;
        mode += cabac().decode(cabac().ctx(69)) ? 4 : 0;
        return mode;
    }

    [[nodiscard]] int decode_chroma_pred_mode(std::uint32_t mb_addr) {
        const auto* left = left_mb(mb_addr);
        const auto* top = top_mb(mb_addr);
        int ctx = 0;
        if (left != nullptr && left->chroma_pred != 0) ++ctx;
        if (top != nullptr && top->chroma_pred != 0) ++ctx;
        if (!cabac().decode(cabac().ctx(64 + ctx))) return 0;
        if (!cabac().decode(cabac().ctx(64 + 3))) return 1;
        if (!cabac().decode(cabac().ctx(64 + 3))) return 2;
        return 3;
    }

    /*
     * Neighbouring TotalCoeff lookups.  `index` is a raster index into
     * MbData::nzc for the current macroblock's residual layout.  A negative
     * value means the neighbouring block is not available (outside the
     * picture or in a different slice); the CAVLC nC derivation distinguishes
     * "available with zero coefficients" from "not available".
     *
     * MBAFF makes a simple address-based neighbour insufficient: a
     * frame-coded macroblock next to a field-coded pair has its above/left
     * transform blocks split between the two field macroblocks, and a
     * field-coded macroblock samples every second frame line.  Resolve the
     * neighbouring block by its position in the plane's frame grid instead
     * (H.264 6.4.10.4 and the field/frame block-index mapping used by
     * 8.4.1.3.1).
     */
    struct NzcNeighbour {
        std::uint32_t addr = kInvalidAddr;
        int index = -1;
        int value = -1;
    };

    /*
     * Progressive-picture fast path: macroblocks form a plain raster in the
     * sample plane, so the neighbouring block is either inside the current
     * macroblock or in the immediately adjacent one.  No plane arithmetic,
     * no mb_addr_for_cell() and no mb_at() bounds scan are needed.
     */
    [[nodiscard]] NzcNeighbour nzc_neighbour_progressive(
            std::uint32_t mb_addr,
            int nzc_index,
            bool want_left) const noexcept {
        NzcNeighbour result{};
        if (mb_addr >= mbs_.size() || nzc_index < 0) return result;

        const int chroma_format = chroma_array_type_;
        bool chroma = false;
        int comp = 0;
        int bx = 0;
        int by = 0;
        int blocks_x = 4;
        int blocks_y = 4;

        if (chroma_format == 3) {
            if (nzc_index >= 48) return result;
            comp = nzc_index >> 4;
            bx = nzc_index & 3;
            by = (nzc_index & 15) >> 2;
        } else if (nzc_index < 16) {
            bx = nzc_index & 3;
            by = nzc_index >> 2;
        } else if (chroma_format == 1) {
            const int rel = nzc_index - 16;
            if (rel >= 8) return result;
            chroma = true;
            comp = rel >> 2;
            bx = rel & 1;
            by = (rel & 3) >> 1;
            blocks_x = 2;
            blocks_y = 2;
        } else if (chroma_format == 2) {
            const int rel = nzc_index - 16;
            if (rel >= 16) return result;
            chroma = true;
            comp = rel >> 3;
            bx = rel & 1;
            by = (rel & 7) >> 1;
            blocks_x = 2;
            blocks_y = 4;
        } else {
            return result;
        }

        std::uint32_t addr = mb_addr;
        int local_bx = bx;
        int local_by = by;
        if (want_left) {
            if (bx == 0) {
                if (mb_addr % mb_width_ == 0) return result;
                addr = mb_addr - 1u;
                local_bx = blocks_x - 1;
            } else {
                local_bx = bx - 1;
            }
        } else {
            if (by == 0) {
                if (mb_addr < mb_width_) return result;
                addr = mb_addr - mb_width_;
                local_by = blocks_y - 1;
            } else {
                local_by = by - 1;
            }
        }

        if (addr != mb_addr) {
            if (addr >= mbs_.size() || !mbs_[addr].available ||
                mbs_[addr].slice_id != slice_id_ ||
                mbs_[addr].slice_group != slice_group_) {
                return result;
            }
        }

        int neighbour_index = 0;
        if (!chroma) {
            neighbour_index = comp * 16 + local_by * 4 + local_bx;
        } else if (chroma_format == 1) {
            neighbour_index = 16 + comp * 4 + local_by * 2 + local_bx;
        } else {
            neighbour_index = 16 + comp * 8 + local_by * 2 + local_bx;
        }
        result.addr = addr;
        result.index = neighbour_index;
        result.value = mbs_[addr].nzc[static_cast<std::size_t>(neighbour_index)];
        return result;
    }

    [[nodiscard]] NzcNeighbour nzc_neighbour(
            std::uint32_t mb_addr,
            int nzc_index,
            bool want_left) const {
        NzcNeighbour result{};
        if (mb_addr >= mbs_.size() || nzc_index < 0) return result;
        if (!mbaff_frame_ && !field_pic_) {
            return nzc_neighbour_progressive(mb_addr, nzc_index, want_left);
        }

        const int chroma_format = chroma_array_type_;
        bool chroma = false;
        int comp = 0;
        int bx = 0;
        int by = 0;

        if (chroma_format == 3) {
            if (nzc_index >= 48) return result;
            comp = nzc_index / 16;
            bx = nzc_index % 4;
            by = (nzc_index % 16) / 4;
        } else if (nzc_index < 16) {
            bx = nzc_index % 4;
            by = nzc_index / 4;
        } else if (chroma_format == 1) {
            const int rel = nzc_index - 16;
            if (rel >= 8) return result;
            chroma = true;
            comp = rel / 4;
            bx = rel % 2;
            by = (rel % 4) / 2;
        } else if (chroma_format == 2) {
            const int rel = nzc_index - 16;
            if (rel >= 16) return result;
            chroma = true;
            comp = rel / 8;
            bx = rel % 2;
            by = (rel % 8) / 2;
        } else {
            return result;
        }

        int mb_width_samples = 16;
        int mb_height_samples = 16;
        if (chroma && chroma_format == 1) {
            mb_width_samples = 8;
            mb_height_samples = 8;
        } else if (chroma && chroma_format == 2) {
            mb_width_samples = 8;
            mb_height_samples = 16;
        }
        const int plane_width = static_cast<int>(mb_width_) * mb_width_samples;
        const int plane_height = static_cast<int>(mb_height_) * mb_height_samples;

        std::uint32_t addr = kInvalidAddr;
        int local_by = 0;
        int local_bx = 0;

        if (!mbaff_frame_) {
            /*
             * Progressive frames and field pictures keep their macroblocks
             * in a contiguous plane grid; there is no field/frame block
             * interleaving to resolve.
             */
            const int x = static_cast<int>(mb_x(mb_addr)) * mb_width_samples + bx * 4;
            const int y = static_cast<int>(mb_y(mb_addr)) * mb_height_samples + by * 4;
            const int nx = want_left ? x - 1 : x;
            const int ny = want_left ? y : y - 1;
            if (nx < 0 || ny < 0 || nx >= plane_width || ny >= plane_height) {
                return result;
            }
            addr = static_cast<std::uint32_t>(
                    (ny / mb_height_samples) * static_cast<int>(mb_width_) +
                    nx / mb_width_samples);
            local_by = (ny % mb_height_samples) / 4;
            local_bx = (nx % mb_width_samples) / 4;
        } else {
            const int pair_height = chroma_format == 1 && chroma ? 16 : 32;
            const int mb_half_height = pair_height / 2;
            const bool field = mbs_[mb_addr].field_mb;
            const int pair_base =
                    static_cast<int>(mb_y(mb_addr) / 2u) * pair_height;
            const int parity = static_cast<int>(mb_y(mb_addr) & 1u);
            const int x = static_cast<int>(mb_x(mb_addr)) * mb_width_samples + bx * 4;
            const int y = field
                                  ? pair_base + parity + 2 * (by * 4)
                                  : pair_base + parity * mb_half_height + by * 4;

            const int nx = want_left ? x - 1 : x;
            const int ny = want_left ? y : (field ? y - 2 : y - 1);
            if (nx < 0 || ny < 0 || nx >= plane_width || ny >= plane_height) {
                return result;
            }

            const int nx_mb = nx / mb_width_samples;
            const int ny_pair = ny / pair_height;
            const int rem = ny % pair_height;
            const std::uint32_t pair_addr = static_cast<std::uint32_t>(
                    (ny_pair * static_cast<int>(mb_width_) + nx_mb) * 2);
            if (pair_addr + 1u >= mbs_.size()) return result;

            /*
             * Unavailable macroblocks were zero-initialised in the original
             * parser, so treat their field flag as false.  The per-picture
             * clear now only resets `available`, so make the same guarantee
             * explicit here instead of reading stale state.
             */
            const bool pair_field =
                    (mbs_[pair_addr].available && mbs_[pair_addr].field_mb) ||
                    (mbs_[pair_addr + 1u].available && mbs_[pair_addr + 1u].field_mb);
            addr = pair_addr;
            if (pair_field) {
                const int row_parity = rem & 1;
                addr += static_cast<std::uint32_t>(row_parity);
                local_by = ((rem - row_parity) / 2) / 4;
            } else {
                addr += static_cast<std::uint32_t>(rem / mb_half_height);
                local_by = (rem % mb_half_height) / 4;
            }
            local_bx = (nx % mb_width_samples) / 4;
        }
        if (local_by < 0 || local_by > 3 || local_bx < 0 || local_bx > 3) {
            return result;
        }
        if (addr >= mbs_.size() || mb_at(addr) == nullptr) return result;

        int neighbour_index = 0;
        if (!chroma) {
            neighbour_index = comp * 16 + local_by * 4 + local_bx;
        } else if (chroma_format == 1) {
            neighbour_index = 16 + comp * 4 + local_by * 2 + local_bx;
        } else {
            neighbour_index = 16 + comp * 8 + local_by * 2 + local_bx;
        }
        result.addr = addr;
        result.index = neighbour_index;
        result.value = mbs_[addr].nzc[static_cast<std::size_t>(neighbour_index)];
        return result;
    }

    [[nodiscard]] int nzc_left(std::uint32_t mb_addr, int index) const {
        return nzc_neighbour(mb_addr, index, true).value;
    }

    [[nodiscard]] int nzc_top(std::uint32_t mb_addr, int index) const {
        return nzc_neighbour(mb_addr, index, false).value;
    }

    [[nodiscard]] bool decode_cbf(
            CabacDecoder& decoder,
            std::uint32_t mb_addr,
            int cat,
            int nzc_index,
            bool is_dc,
            int plane) {
        const bool intra = mbs_[mb_addr].intra;
        int nza = 0;
        int nzb = 0;
        if (is_dc) {
            const auto* left = left_mb(mb_addr);
            const auto* top = top_mb(mb_addr);
            if (cat == detail::kCatChromaDc) {
                nza = left != nullptr ? left->chroma_dc_cbf[plane] : (intra ? 1 : 0);
                nzb = top != nullptr ? top->chroma_dc_cbf[plane] : (intra ? 1 : 0);
            } else {
                nza = left != nullptr ? left->luma_dc_cbf[plane] : (intra ? 1 : 0);
                nzb = top != nullptr ? top->luma_dc_cbf[plane] : (intra ? 1 : 0);
            }
        } else {
            const NzcNeighbour left = nzc_neighbour(mb_addr, nzc_index, true);
            const NzcNeighbour top = nzc_neighbour(mb_addr, nzc_index, false);
            nza = left.value < 0 ? (intra ? 1 : 0) : (left.value > 0 ? 1 : 0);
            nzb = top.value < 0 ? (intra ? 1 : 0) : (top.value > 0 ? 1 : 0);
            /*
             * H.264 9.3.3.1.1.9: the co-located transform block of a
             * neighbouring macroblock only contributes to an 8x8 block's
             * coded_block_flag context when that neighbouring macroblock
             * also used the 8x8 transform.  A 4x4-coded neighbour (or a
             * neighbour in another slice) contributes zero.
             */
            if (detail::is_8x8_residual_cat(cat)) {
                const int local = nzc_index % 16;
                if (local % 4 == 0) {
                    const auto* lm = left.addr != kInvalidAddr ? mb_at(left.addr) : nullptr;
                    if (lm != nullptr && !lm->transform8x8) nza = 0;
                }
                if (local < 4) {
                    const auto* tm = top.addr != kInvalidAddr ? mb_at(top.addr) : nullptr;
                    if (tm != nullptr && !tm->transform8x8) nzb = 0;
                }
            }
        }
        /*
         * The category base is fixed for the whole block, so index the raw
         * context array once.  kCbfCtx[cat] + 3 stays within the 1024-entry
         * context array for every category (the largest base is 1020).
         */
        CabacContext* ctx_base = &decoder.ctx(detail::kCbfCtx[cat]);
        return decoder.decode(ctx_base[nza + (nzb << 1)]);
    }

    void set_dc_cbf(std::uint32_t mb_addr, int cat, int plane, bool present) {
        if (cat == detail::kCatChromaDc) {
            mbs_[mb_addr].chroma_dc_cbf[plane] = present ? 1 : 0;
        } else {
            mbs_[mb_addr].luma_dc_cbf[plane] = present ? 1 : 0;
        }
    }

    [[nodiscard]] int cavlc_nC(std::uint32_t mb_addr, int nzc_index) const {
        const int nza = nzc_left(mb_addr, nzc_index);
        const int nzb = nzc_top(mb_addr, nzc_index);

        /*
         * H.264 9.2.1: only average when both neighbours are available.
         * With a single available neighbour nC is that neighbour's
         * TotalCoeff; with none it is zero.  Averaging an unavailable
         * neighbour as zero (nA + 1) >> 1 selects the wrong coeff_token
         * table for every left/top edge block.
         */
        if (nza < 0) return nzb < 0 ? 0 : nzb;
        if (nzb < 0) return nza;
        return (nza + nzb + 1) >> 1;
    }

    void parse_residual_cavlc(
            std::uint32_t mb_addr,
            int cat,
            int nzc_index,
            int max_coeff,
            bool is_dc,
            int plane) {
        const bool chroma_dc_420 =
                is_dc && cat == detail::kCatChromaDc && max_coeff == 4;
        const bool chroma_dc_422 =
                is_dc && cat == detail::kCatChromaDc && max_coeff == 8;
        const int nC = (chroma_dc_420 || chroma_dc_422)
                               ? 0
                               : std::max(0, cavlc_nC(mb_addr, nzc_index));
        const std::uint32_t total = cavlc().decode_residual_block(
                static_cast<std::uint32_t>(nC),
                max_coeff,
                chroma_dc_420,
                chroma_dc_422);

        if (is_dc) {
            set_dc_cbf(mb_addr, cat, plane, total > 0);
        } else {
            mbs_[mb_addr].nzc[static_cast<std::size_t>(nzc_index)] =
                    static_cast<std::uint8_t>(total);
        }
    }

    /*
     * Significance and last-significant-coefficient bins.  `sig` and `last`
     * point at the category's context bases, so the per-bin clamp in
     * CabacDecoder::ctx() is paid once per block instead of once per bin.
     * All indices stay inside kCabacContextCount: the largest base is 748
     * (8x8 luma plane 2), plus kLast8x8's maximum of 8.
     */
    template <int kMaxCoeff, bool kChroma422Dc>
    [[nodiscard]] int decode_significance_t(
            CabacDecoder& decoder,
            CabacContext* sig,
            CabacContext* last) {
        int coeff_count = 0;

        if constexpr (kChroma422Dc) {
            /*
             * 4:2:2 chroma DC: seven significance bins share three contexts
             * and the eighth coefficient is implied when the last flag is
             * not set (H.264 9.3.3.1.1.6).
             */
            for (int i = 0; i < 7; ++i) {
                const int offset = detail::kSig422Dc[i];
                if (decoder.decode(sig[offset])) {
                    ++coeff_count;
                    if (decoder.decode(last[offset])) {
                        return coeff_count;
                    }
                }
            }
            return coeff_count + 1;
        } else if constexpr (kMaxCoeff == 64) {
            for (int i = 0; i < 63; ++i) {
                if (decoder.decode(sig[detail::kSig8x8[i]])) {
                    ++coeff_count;
                    if (decoder.decode(last[detail::kLast8x8[i]])) {
                        return coeff_count;
                    }
                }
            }
            return coeff_count + 1;
        } else {
            for (int i = 0; i < kMaxCoeff - 1; ++i) {
                if (decoder.decode(sig[i])) {
                    ++coeff_count;
                    if (decoder.decode(last[i])) {
                        return coeff_count;
                    }
                }
            }
            return coeff_count + 1;
        }
    }

    /* Generic fallback for an unexpected block size; same syntax. */
    [[nodiscard]] int decode_significance_generic(
            CabacDecoder& cabac,
            int max_coeff,
            bool chroma422_dc,
            CabacContext* sig,
            CabacContext* last) {
        int coeff_count = 0;
        if (chroma422_dc) {
            for (int i = 0; i < 7; ++i) {
                const int offset = detail::kSig422Dc[i];
                if (cabac.decode(sig[offset])) {
                    ++coeff_count;
                    if (cabac.decode(last[offset])) {
                        return coeff_count;
                    }
                }
            }
            return coeff_count + 1;
        }
        if (max_coeff == 64) {
            for (int i = 0; i < 63; ++i) {
                if (cabac.decode(sig[detail::kSig8x8[i]])) {
                    ++coeff_count;
                    if (cabac.decode(last[detail::kLast8x8[i]])) {
                        return coeff_count;
                    }
                }
            }
            return coeff_count + 1;
        }
        for (int i = 0; i < max_coeff - 1; ++i) {
            if (cabac.decode(sig[i])) {
                ++coeff_count;
                if (cabac.decode(last[i])) {
                    return coeff_count;
                }
            }
        }
        return coeff_count + 1;
    }

    [[nodiscard]] int decode_significance(
            CabacDecoder& decoder,
            int cat,
            int max_coeff,
            bool chroma422_dc) {
        CabacContext* sig = &decoder.ctx(detail::kSigCtx[cat]);
        CabacContext* last = &decoder.ctx(detail::kLastCtx[cat]);

        if (chroma422_dc) {
            return decode_significance_t<8, true>(decoder, sig, last);
        }
        switch (max_coeff) {
        case 4:
            return decode_significance_t<4, false>(decoder, sig, last);
        case 8:
            return decode_significance_t<8, false>(decoder, sig, last);
        case 15:
            return decode_significance_t<15, false>(decoder, sig, last);
        case 16:
            return decode_significance_t<16, false>(decoder, sig, last);
        case 64:
            return decode_significance_t<64, false>(decoder, sig, last);
        default:
            return decode_significance_generic(
                    decoder, max_coeff, chroma422_dc, sig, last);
        }
    }

    void parse_residual(
            std::uint32_t mb_addr,
            int cat,
            int nzc_index,
            int max_coeff,
            bool is_dc,
            int plane) {
        if (is_cavlc()) {
            parse_residual_cavlc(
                    mb_addr, cat, nzc_index, max_coeff, is_dc, plane);
            return;
        }

        const bool is_8x8 = !is_dc && max_coeff == 64;
        const bool chroma422_dc =
                is_dc && cat == detail::kCatChromaDc && max_coeff == 8;
        CabacDecoder& decoder = cabac();

        if (is_dc) {
            if (!decode_cbf(decoder, mb_addr, cat, nzc_index, true, plane)) {
                set_dc_cbf(mb_addr, cat, plane, false);
                return;
            }
            set_dc_cbf(mb_addr, cat, plane, true);
        } else if (!is_8x8 || chroma_array_type_ == 3) {
            /*
             * A 4:2:0/4:2:2/monochrome luma 8x8 block has no
             * coded_block_flag: its presence is already implied by the CBP
             * bit.  In 4:4:4 the CBP bit is shared by three planes, so each
             * plane's 8x8 block carries its own flag.
             */
            if (!decode_cbf(decoder, mb_addr, cat, nzc_index, false, plane)) {
                if (is_8x8) {
                    static constexpr int kRasterOffsets[4] = {0, 1, 4, 5};
                    for (int offset : kRasterOffsets) {
                        mbs_[mb_addr].nzc[static_cast<std::size_t>(
                                nzc_index + offset)] = 0;
                    }
                } else {
                    mbs_[mb_addr].nzc[static_cast<std::size_t>(nzc_index)] = 0;
                }
                return;
            }
        }

        const std::uint8_t* gt1_table = chroma422_dc
                                                ? detail::kAbsLevelGt1Ctx422Dc
                                                : detail::kAbsLevelGt1Ctx;
        const int coeff_count =
                decode_significance(decoder, cat, max_coeff, chroma422_dc);
        /*
         * The level contexts all share the category base; indexing the raw
         * array avoids a clamped CabacDecoder::ctx() call per coefficient.
         */
        CabacContext* level_ctx = &decoder.ctx(detail::kLevelCtx[cat]);
        int node_ctx = 0;
        for (int i = coeff_count - 1; i >= 0; --i) {
            if (!decoder.decode(level_ctx[detail::kAbsLevel1Ctx[node_ctx]])) {
                node_ctx = detail::kAbsLevelTransition[0][node_ctx];
                (void)decoder.decode_bypass();
            } else {
                int coeff_abs = 2;
                CabacContext* gt1_ctx = level_ctx + gt1_table[node_ctx];
                node_ctx = detail::kAbsLevelTransition[1][node_ctx];
                while (coeff_abs < 15 && decoder.decode(*gt1_ctx)) {
                    ++coeff_abs;
                }
                if (coeff_abs >= 15) {
                    int j = 0;
                    while (decoder.decode_bypass() && j < 23) ++j;
                    while (j-- > 0) {
                        (void)decoder.decode_bypass();
                    }
                }
                (void)decoder.decode_bypass();
            }
        }

        if (is_dc) {
            set_dc_cbf(mb_addr, cat, plane, true);
        } else if (is_8x8) {
            static constexpr int kRasterOffsets[4] = {0, 1, 4, 5};
            for (int offset : kRasterOffsets) {
                mbs_[mb_addr].nzc[static_cast<std::size_t>(nzc_index + offset)] =
                        static_cast<std::uint8_t>(coeff_count);
            }
        } else {
            mbs_[mb_addr].nzc[static_cast<std::size_t>(nzc_index)] =
                    static_cast<std::uint8_t>(coeff_count);
        }
    }

    void parse_luma_plane_residual(
            std::uint32_t mb_addr,
            const detail::MbData& mb,
            int plane) {
        const int base = plane * 16;
        const int dc_cat = plane == 0   ? detail::kCatLumaDc0
                           : plane == 1 ? detail::kCatLumaDc1
                                        : detail::kCatLumaDc2;
        const int ac_cat = plane == 0   ? detail::kCatLumaAc0
                           : plane == 1 ? detail::kCatLumaAc1
                                        : detail::kCatLumaAc2;
        const int l4_cat = plane == 0   ? detail::kCatLuma4x4_0
                           : plane == 1 ? detail::kCatLuma4x4_1
                                        : detail::kCatLuma4x4_2;
        const int l8_cat = plane == 0   ? detail::kCatLuma8x8_0
                           : plane == 1 ? detail::kCatLuma8x8_1
                                        : detail::kCatLuma8x8_2;
        const int luma_cbp = mb.cbp & 15;

        if (mb.intra16x16) {
            /*
             * Intra16x16 always codes a DC block per residual plane, even
             * when the AC CBP is zero.  In 4:4:4 all three planes carry one.
             */
            parse_residual(mb_addr, dc_cat, base, 16, true, plane);
            if (luma_cbp != 0) {
                for (int i8 = 0; i8 < 4; ++i8) {
                    for (int i4 = 0; i4 < 4; ++i4) {
                        const int raster =
                                base + detail::kZToRaster[i8 * 4 + i4];
                        parse_residual(
                                mb_addr, ac_cat, raster, 15, false, plane);
                    }
                }
            }
            return;
        }

        if (mb.transform8x8) {
            for (int i8 = 0; i8 < 4; ++i8) {
                const int raster_base =
                        base + (i8 >> 1) * 8 + (i8 & 1) * 2;
                if ((luma_cbp & (1 << i8)) == 0) {
                    static constexpr int kRasterOffsets[4] = {0, 1, 4, 5};
                    for (int offset : kRasterOffsets) {
                        mbs_[mb_addr].nzc[static_cast<std::size_t>(
                                raster_base + offset)] = 0;
                    }
                    continue;
                }
                if (is_cavlc()) {
                    /*
                     * CAVLC has no 64-coefficient block syntax: the 8x8
                     * transform coefficients are entropy-coded as four 4x4
                     * blocks (H.264 7.3.5.3.2).
                     */
                    for (int i4 = 0; i4 < 4; ++i4) {
                        parse_residual(mb_addr,
                                       l4_cat,
                                       raster_base + detail::kZToRaster[i4],
                                       16,
                                       false,
                                       plane);
                    }
                } else {
                    parse_residual(
                            mb_addr, l8_cat, raster_base, 64, false, plane);
                }
            }
            return;
        }

        for (int i8 = 0; i8 < 4; ++i8) {
            if ((luma_cbp & (1 << i8)) != 0) {
                for (int i4 = 0; i4 < 4; ++i4) {
                    const int raster = base + detail::kZToRaster[i8 * 4 + i4];
                    parse_residual(
                            mb_addr, l4_cat, raster, 16, false, plane);
                }
            } else {
                for (int i4 = 0; i4 < 4; ++i4) {
                    mbs_[mb_addr].nzc[static_cast<std::size_t>(
                            base + detail::kZToRaster[i8 * 4 + i4])] = 0;
                }
            }
        }
    }

    void parse_chroma_residual(
            std::uint32_t mb_addr,
            const detail::MbData& mb) {
        const int chroma_format = chroma_array_type_;
        const int blocks_per_component = chroma_format == 2 ? 8 : 4;
        const int chroma_cbp = (mb.cbp >> 4) & 3;

        if (chroma_cbp != 0) {
            for (int comp = 0; comp < 2; ++comp) {
                const int base = 16 + comp * blocks_per_component;
                parse_residual(mb_addr,
                               detail::kCatChromaDc,
                               base,
                               chroma_format == 2 ? 8 : 4,
                               true,
                               comp);
            }
        }

        if (chroma_cbp == 2) {
            for (int comp = 0; comp < 2; ++comp) {
                const int base = 16 + comp * blocks_per_component;
                for (int i = 0; i < blocks_per_component; ++i) {
                    /*
                     * 4:2:0 has one 8x8 chroma section per component and
                     * 4:2:2 has two stacked sections.  Within each section
                     * the four 4x4 blocks are in z-order, which equals
                     * raster order for the 2x2 section layout.
                     */
                    parse_residual(
                            mb_addr, detail::kCatChromaAc, base + i, 15, false, comp);
                }
            }
        } else {
            for (int i = 16; i < 16 + 2 * blocks_per_component; ++i) {
                mbs_[mb_addr].nzc[static_cast<std::size_t>(i)] = 0;
            }
        }
    }

    void parse_residual_mb(std::uint32_t mb_addr, const detail::MbData& mb) {
        if (chroma_array_type_ == 3) {
            for (int plane = 0; plane < 3; ++plane) {
                parse_luma_plane_residual(mb_addr, mb, plane);
            }
            return;
        }
        parse_luma_plane_residual(mb_addr, mb, 0);
        if (chroma_array_type_ != 0) {
            parse_chroma_residual(mb_addr, mb);
        }
    }

    /*
     * Bit count of an I_PCM macroblock's raw samples.  pcm_sample_luma is
     * 256 samples at BitDepthY; pcm_sample_chroma is two planes of
     * MbWidthC x MbHeightC samples at BitDepthC (H.264 7.3.5).
     */
    [[nodiscard]] std::uint64_t ipcm_sample_bits() const noexcept {
        const std::uint64_t bit_depth_y =
                static_cast<std::uint64_t>(sps_.bit_depth_luma_minus8) + 8u;
        std::uint64_t bits = 256u * bit_depth_y;
        if (chroma_array_type_ == 0) return bits;

        const std::uint64_t bit_depth_c =
                static_cast<std::uint64_t>(sps_.bit_depth_chroma_minus8) + 8u;
        const std::uint64_t mb_width_c = chroma_array_type_ == 3 ? 16u : 8u;
        const std::uint64_t mb_height_c = chroma_array_type_ == 1 ? 8u : 16u;
        bits += 2u * mb_width_c * mb_height_c * bit_depth_c;
        return bits;
    }

    void skip_cavlc_bits(std::uint64_t bits) {
        while (bits >= 32u) {
            (void)cavlc().read_bits(32);
            bits -= 32u;
        }
        if (bits != 0) {
            (void)cavlc().read_bits(static_cast<int>(bits));
        }
    }

    void parse_intra_modes(std::uint32_t mb_addr, detail::MbData& mb) {
        if (is_cavlc()) {
            if (mb.is_intra4x4) {
                /*
                 * For I_NxN the transform_size_8x8_flag precedes the intra
                 * prediction modes.  With the 8x8 transform only four modes
                 * (one per 8x8 block) are coded.
                 */
                if (pps_.transform_8x8_mode_flag) {
                    mb.transform8x8 = cavlc().read_bit();
                }
                const int count = mb.transform8x8 ? 4 : 16;
                for (int i = 0; i < count; ++i) {
                    const bool predicted = cavlc().read_bit();
                    (void)cavlc().read_bits(predicted ? 0 : 3);
                }
            }
            /*
             * 4:4:4 has no separate chroma intra prediction mode: the
             * chroma planes reuse the luma prediction (H.264 7.3.5.1,
             * 7.4.5).  ChromaArrayType 1 and 2 code
             * intra_chroma_pred_mode; monochrome and separate colour planes
             * have no chroma syntax at all.
             */
            if (chroma_array_type_ != 3 && chroma_array_type_ != 0) {
                mb.chroma_pred = static_cast<std::uint8_t>(cavlc().read_ue());
            }
            return;
        }
        if (mb.is_intra4x4) {
            if (pps_.transform_8x8_mode_flag) {
                const auto* left = left_mb(mb_addr);
                const auto* top = top_mb(mb_addr);
                int ctx = 399;
                if (left != nullptr && left->transform8x8) ++ctx;
                if (top != nullptr && top->transform8x8) ++ctx;
                mb.transform8x8 = cabac().decode(cabac().ctx(ctx));
            }
            const int count = mb.transform8x8 ? 4 : 16;
            for (int i = 0; i < count; ++i) {
                (void)decode_intra4x4_mode();
            }
        }
        if (chroma_array_type_ != 3 && chroma_array_type_ != 0) {
            mb.chroma_pred = static_cast<std::uint8_t>(decode_chroma_pred_mode(mb_addr));
        }
    }

    [[nodiscard]] std::int32_t currentFieldPoc(const detail::MbData& mb) const noexcept {
        if (field_pic_) return header_.derived_poc;
        if (!mbaff_frame_ || !mb.field_mb) return header_.derived_poc;
        const bool bottom = (mb_y(static_cast<std::uint32_t>(&mb - mbs_.data())) & 1u) != 0;
        return bottom ? header_.derived_poc_bottom : header_.derived_poc_top;
    }

    [[nodiscard]] int mapColPocToList0(std::int32_t poc) const noexcept {
        for (std::size_t i = 0; i < list0_.size(); ++i) {
            if (list0_[i].poc == poc) return static_cast<int>(i);
        }
        return 0;
    }

    [[nodiscard]] bool col_zero_motion(
            const MotionReferencePicture* col,
            std::uint32_t x4,
            std::uint32_t y4) const noexcept {
        if (col == nullptr || col->cells == nullptr || x4 >= col->width4x4 || y4 >= col->height4x4) {
            return false;
        }
        const MvCell& cc = col->cells[static_cast<std::size_t>(y4) * col->width4x4 + x4];
        if (cc.flags & (MvIntra | MvUnavailable)) return false;
        if (cc.ref_idx[0] == 0 && std::abs(cc.mv[0][0]) <= 1 && std::abs(cc.mv[0][1]) <= 1) {
            return true;
        }
        return cc.ref_idx[0] < 0 && cc.ref_idx[1] == 0 && std::abs(cc.mv[1][0]) <= 1 &&
               std::abs(cc.mv[1][1]) <= 1;
    }

    void apply_direct(
            std::uint32_t mb_addr,
            detail::MbData& mb,
            int region_x = 0,
            int region_y = 0,
            int region_w = 4,
            int region_h = 4) {
        const bool spatial = header_.direct_spatial_mv_pred_flag;
        /*
         * H.264 8.4.1.2.1 uses RefPicList1[0] as the co-located picture
         * (falling back to list 0 for a P slice).  A non-existing frame has no
         * motion grid; degrade to the nearest subsequent available entry,
         * and if none exists the temporal path below emits zero motion.
         */
        const MotionReferencePicture* col = nullptr;
        for (const MotionReferencePicture& candidate : list1_) {
            if (!candidate.non_existing && candidate.cells != nullptr) {
                col = &candidate;
                break;
            }
        }
        if (col == nullptr) {
            for (const MotionReferencePicture& candidate : list0_) {
                if (!candidate.non_existing && candidate.cells != nullptr) {
                    col = &candidate;
                    break;
                }
            }
        }

        const std::uint32_t base_x = mb_x(mb_addr) * 4;
        const std::uint32_t base_y = mb_y(mb_addr) * 4;
        if (spatial && !mbaff_frame_ && !field_pic_) {
            int ref[2] = {0, 0};
            std::int16_t mv[2][2] = {{0, 0}, {0, 0}};
            for (int list = 0; list < 2; ++list) {
                NeighMv a;
                NeighMv b;
                if (base_x > 0) a = neighbour_mv(base_x - 1, base_y, list, 0);
                if (base_y > 0) b = neighbour_mv(base_x, base_y - 1, list, 0);
                const NeighMv c = diagonal_neighbour(base_x, base_y, list, 0, 4);

                const unsigned min_ref = std::min(
                        {static_cast<unsigned>(a.ref), static_cast<unsigned>(b.ref),
                         static_cast<unsigned>(c.ref)});
                if (static_cast<int>(min_ref) >= 0) {
                    const int chosen = static_cast<int>(min_ref);
                    const int count = (a.ref == chosen ? 1 : 0) + (b.ref == chosen ? 1 : 0) +
                                      (c.ref == chosen ? 1 : 0);
                    if (count > 1) {
                        mv[list][0] = median3(a.mv[0], b.mv[0], c.mv[0]);
                        mv[list][1] = median3(a.mv[1], b.mv[1], c.mv[1]);
                    } else {
                        const NeighMv& pick = a.ref == chosen ? a : (b.ref == chosen ? b : c);
                        mv[list][0] = pick.mv[0];
                        mv[list][1] = pick.mv[1];
                    }
                    ref[list] = chosen;
                } else {
                    ref[list] = -1;
                    mv[list][0] = 0;
                    mv[list][1] = 0;
                }
            }
            if (ref[0] < 0 && ref[1] < 0) ref[0] = ref[1] = 0;

            /*
             * The zero-motion shortcut and the chosen ref indices are
             * constant over each 8x8 quadrant, so every quadrant is one
             * pattern fill.
             */
            const std::uint8_t extra = MvDirect;
            for (int qy = region_y; qy < region_y + region_h; qy += 2) {
                const int qh = std::min(2, region_y + region_h - qy);
                for (int qx = region_x; qx < region_x + region_w; qx += 2) {
                    const int qw = std::min(2, region_x + region_w - qx);
                    const bool zero = col_zero_motion(
                            col,
                            base_x + static_cast<std::uint32_t>(((qx & ~1) / 2) * 3),
                            base_y + static_cast<std::uint32_t>(((qy & ~1) / 2) * 3));
                    const std::int16_t mv0x = static_cast<std::int16_t>(
                            (zero && ref[0] == 0) ? 0 : mv[0][0]);
                    const std::int16_t mv0y = static_cast<std::int16_t>(
                            (zero && ref[0] == 0) ? 0 : mv[0][1]);
                    const std::int16_t mv1x = static_cast<std::int16_t>(
                            (zero && ref[1] == 0) ? 0 : mv[1][0]);
                    const std::int16_t mv1y = static_cast<std::int16_t>(
                            (zero && ref[1] == 0) ? 0 : mv[1][1]);
                    MvCell pattern{};
                    if (ref[0] >= 0 && ref[1] >= 0) {
                        pattern = simd::CellPattern::biPartition(
                                          mv0x, mv0y, static_cast<std::int8_t>(ref[0]),
                                          mv1x, mv1y, static_cast<std::int8_t>(ref[1]), extra)
                                          .cell;
                    } else if (ref[0] >= 0) {
                        pattern = simd::CellPattern::l0Partition(
                                          mv0x, mv0y, static_cast<std::int8_t>(ref[0]), extra)
                                          .cell;
                    } else {
                        pattern = simd::CellPattern::l1Partition(
                                          mv1x, mv1y, static_cast<std::int8_t>(ref[1]), extra)
                                          .cell;
                    }
                    simd::fillBlock(
                            cells_, width4x4_, base_x + static_cast<std::uint32_t>(qx),
                            base_y + static_cast<std::uint32_t>(qy),
                            static_cast<std::uint32_t>(qw),
                            static_cast<std::uint32_t>(qh), pattern);
                    for (int yy = 0; yy < qh; ++yy) {
                        for (int xx = 0; xx < qw; ++xx) {
                            const int local = (qy + yy) * 4 + qx + xx;
                            mbs_[mb_addr].ref[0][local] = static_cast<std::int8_t>(ref[0]);
                            mbs_[mb_addr].ref[1][local] = static_cast<std::int8_t>(ref[1]);
                            mbs_[mb_addr].set_direct4x4(local);
                        }
                    }
                }
            }
            return;
        }
        const bool current_field_mb = mb.field_mb;
        const bool current_field_picture = field_pic_;
        const bool bottom_field = field_pic_
                                          ? header_.bottom_field_flag
                                          : ((mb_y(mb_addr) & 1u) != 0);
        const std::int32_t current_poc = currentFieldPoc(mb);
        const std::int32_t ref0_poc = list0_.empty()
                                              ? std::numeric_limits<std::int32_t>::min()
                                              : list0_.front().poc;
        const std::int32_t ref1_poc = list1_.empty()
                                              ? std::numeric_limits<std::int32_t>::min()
                                              : list1_.front().poc;
        const bool have_ref0 = ref_count(0) > 0;
        const bool have_ref1 = ref_count(1) > 0;

        /*
         * Spatial direct in a field picture or an MBAFF picture uses the
         * neighbouring MVP per 4x4 cell.  Keep the scalar row-major order:
         * each cell's predictor reads cells already written above and to the
         * left, so a quadrant-major fill would change the result.
         */
        if (spatial) {
            for (int y = region_y; y < region_y + region_h; ++y) {
                for (int x = region_x; x < region_x + region_w; ++x) {
                    const std::uint32_t gx = base_x + static_cast<std::uint32_t>(x);
                    const std::uint32_t gy = base_y + static_cast<std::uint32_t>(y);
                    mbs_[mb_addr].set_direct4x4(y * 4 + x);
                    /*
                     * The region lies inside this macroblock, which the slice
                     * traversal has already validated, so the grid address is
                     * in range without a per-cell bounds check.
                     */
                    MvCell* c = &cells_[static_cast<std::size_t>(gy) * width4x4_ + gx];
                    std::int16_t mv0[2], mv1[2];
                    predict_mv(gx, gy, 1, 1, 0, 0, mv0);
                    predict_mv(gx, gy, 1, 1, 1, 0, mv1);
                    c->mv[0][0] = mv0[0];
                    c->mv[0][1] = storeMvY(mv0[1], current_field_mb);
                    c->mv[1][0] = mv1[0];
                    c->mv[1][1] = storeMvY(mv1[1], current_field_mb);
                    c->ref_idx[0] = have_ref0 ? 0 : -1;
                    c->ref_idx[1] = have_ref1 ? 0 : -1;
                    c->flags = MvDirect;
                    if (c->ref_idx[0] >= 0) c->flags |= MvList0;
                    if (c->ref_idx[1] >= 0) c->flags |= MvList1;
                    if (current_field_mb) c->flags |= MvField;
                }
            }
            return;
        }

        for (int qy = region_y; qy < region_y + region_h; qy += 2) {
            const int qh = std::min(2, region_y + region_h - qy);
            for (int qx = region_x; qx < region_x + region_w; qx += 2) {
                const int qw = std::min(2, region_x + region_w - qx);
                const std::uint32_t gx = base_x + static_cast<std::uint32_t>(qx);
                const std::uint32_t gy = base_y + static_cast<std::uint32_t>(qy);
                for (int yy = 0; yy < qh; ++yy) {
                    for (int xx = 0; xx < qw; ++xx) {
                        mbs_[mb_addr].set_direct4x4((qy + yy) * 4 + qx + xx);
                    }
                }

                const MvCell* cc = nullptr;
                std::uint32_t cx = 0;
                std::uint32_t cy = 0;
                if (col != nullptr && col->cells != nullptr) {
                    /*
                     * 8.4.1.2.1: a direct 4x4 partition samples the
                     * co-located partition at the representative 4x4 block
                     * of its 8x8 quadrant (luma4x4BlkIdx = 5 * mbPartIdx
                     * when direct_8x8_inference_flag is set), not the
                     * same-parity cell.  Sampling every cell individually
                     * picked up intra co-located cells and produced zero
                     * vectors where the surrounding partition had motion.
                     */
                    cx = (gx & ~3u) + (((gx & 3u) >> 1) * 3u);
                    cy = (gy & ~3u) + (((gy & 3u) >> 1) * 3u);
                    if (current_field_picture && !col->field_pic) {
                        /*
                         * A field picture referencing a frame picture uses
                         * the co-located field: line parity selects the
                         * top/bottom field rows of the frame grid.
                         */
                        cy = cy * 2u + (bottom_field ? 1u : 0u);
                    } else if (!current_field_picture && col->field_pic) {
                        cy = cy / 2u;
                    }
                    if (cx < col->width4x4 && cy < col->height4x4) {
                        cc = &col->cells[static_cast<std::size_t>(cy) * col->width4x4 + cx];
                    }
                }

                if (cc != nullptr && !(cc->flags & MvIntra) && !(cc->flags & MvUnavailable)) {
                    const int col_list = (cc->ref_idx[0] >= 0) ? 0 : 1;
                    if (cc->ref_idx[col_list] >= 0 &&
                        !col->refNonExistingAt(cx, cy, col_list, cc->ref_idx[col_list])) {
                        std::int32_t col_ref_poc =
                                col->refPocAt(cx, cy, col_list, cc->ref_idx[col_list]);
                        if (col_ref_poc == kMotionVectorUnknownPoc) {
                            col_ref_poc = col->poc;
                        }
                        const int ref0 = mapColPocToList0(col_ref_poc);
                        const int ref1 = have_ref1 ? 0 : -1;
                        const std::int32_t pic0_poc =
                                list0_.empty() ? ref0_poc
                                               : list0_[static_cast<std::size_t>(ref0)].poc;
                        /*
                         * H.264 8.4.1.2.3: tb is measured from the
                         * co-located reference picture (pic0) to the current
                         * picture, td from pic0 to RefPicList1[0] (pic1).
                         * Deriving td from the current picture instead made
                         * the scale negative and every temporal-direct vector
                         * wrong whenever RefPicList1[0] was in the future.
                         *
                         * The arithmetic lives in simd::scaleDirectCell()
                         * and is bit-identical to the scalar formula.
                         */
                        simd::DirectScaleParams params;
                        params.current_poc = current_poc;
                        params.col_poc = pic0_poc;
                        params.ref1_poc = ref1_poc;
                        params.ref0 = static_cast<std::int8_t>(ref0);
                        params.ref1 = static_cast<std::int8_t>(ref1);
                        params.current_field_mb = current_field_mb;
                        params.col_field_pic = col->field_pic;
                        params.mbaff_frame = mbaff_frame_;
                        simd::scaleDirectBlock(
                                cells_, width4x4_, gx, gy,
                                static_cast<std::uint32_t>(qw),
                                static_cast<std::uint32_t>(qh), *cc, col_list, params);
                        continue;
                    }
                }

                /*
                 * Temporal direct with an unavailable/intra co-located
                 * partition: 8.4.1.2.3 sets mvCol to zero and refIdxCol to
                 * -1, which maps to list 0 index 0 and a zero vector.
                 */
                MvCell pattern{};
                pattern.ref_idx[0] = have_ref0 ? 0 : -1;
                pattern.ref_idx[1] = have_ref1 ? 0 : -1;
                pattern.flags = MvDirect;
                if (have_ref0) pattern.flags |= MvList0;
                if (have_ref1) pattern.flags |= MvList1;
                if (current_field_mb) pattern.flags |= MvField;
                simd::fillBlock(
                        cells_, width4x4_, gx, gy,
                        static_cast<std::uint32_t>(qw),
                        static_cast<std::uint32_t>(qh), pattern);
            }
        }
    }

    [[nodiscard]] bool parse_cavlc(std::uint32_t mb_addr) {
        try {
            in_cavlc_slice_ = true;
            if (is_i()) {
                return true; // I slices carry no motion syntax
            }

            while (mb_addr < mb_count_ && mb_addr < mbs_.size()) {
                const std::uint32_t skip_run = cavlc().read_ue();
                if (skip_run > mb_count_) {
                    status_ = MotionVectorStatus::Truncated;
                    return false;
                }

                /*
                 * mb_skip_run counts macroblocks in slice-group scan order
                 * (H.264 8.2.3), i.e. each skipped macroblock is reached with
                 * NextMbAddress().  Resolve the address after the run first:
                 * in an MBAFF picture an mb_field_decoding_flag belongs to
                 * the bottom MB that follows the run.
                 */
                std::uint32_t skip_end = mb_addr;
                std::uint32_t stepped = 0;
                while (stepped < skip_run && skip_end < mb_count_) {
                    ++stepped;
                    skip_end = next_mb_address(skip_end);
                }
                if (stepped != skip_run) {
                    status_ = MotionVectorStatus::Truncated;
                    return false;
                }

                /*
                 * When a CAVLC skip run ends on the bottom MB of an MBAFF
                 * pair, mb_field_decoding_flag precedes the coded bottom MB.
                 * Reading it before applying the skipped MBs lets the skipped
                 * top MB inherit the pair's field/frame decision.
                 */
                if (mbaff_frame_ && skip_run > 0 && skip_end < mb_count_ &&
                    (skip_end & 1u) != 0 && cavlc().more_rbsp_data()) {
                    ensure_pair_field_flag(skip_end - 1u);
                }

                for (std::uint32_t i = 0; i < skip_run; ++i) {
                    apply_skip_mb(mb_addr);
                    mb_addr = next_mb_address(mb_addr);
                }
                if (mb_addr >= mb_count_ || mb_addr >= mbs_.size()) break;
                /*
                 * A skip run may end a slice in the middle of the picture.
                 * CAVLC has no end_of_slice_flag, so the only boundary is
                 * the rbsp trailing bits; without this check the parser reads
                 * the next slice's first_mb_in_slice macroblock as if it
                 * belonged to this slice and desynchronises.
                 */
                if (!cavlc().more_rbsp_data()) break;

                if (mbaff_frame_) ensure_pair_field_flag(mb_addr);
                parse_mb(mb_addr);
                if (status_ != MotionVectorStatus::Ok) {
                    return false;
                }
                mb_addr = next_mb_address(mb_addr);
                if (!cavlc().more_rbsp_data()) break;
            }
            return true;
        } catch (const CavlcError&) {
            status_ = MotionVectorStatus::Truncated;
            return false;
        } catch (const std::exception& e) {
            status_ = MotionVectorStatus::Malformed;
            return false;
        } catch (...) {
            status_ = MotionVectorStatus::Malformed;
            return false;
        }
    }

    void apply_skip_mb(std::uint32_t mb_addr) {
        detail::MbData& mb = mbs_[mb_addr];
        mb.reset();
        mb.available = true;
        mb.slice_id = slice_id_;
        mb.slice_group = slice_group_;
        mb.skip = true;
        if (mbaff_frame_ && pair_flag_pair_ != pair_index(mb_addr)) {
            pair_field_flag_ = infer_pair_field_flag(mb_addr);
        }
        mb.field_mb = mbaff_frame_ ? pair_field_flag_ : field_pic_;
        current_mb_field_ = mb.field_mb;
        last_delta_qp_ = 0;
        if (is_b()) {
            mb.direct = true;
            mb.direct4x4 = 0xFFFFu;
            apply_direct(mb_addr, mb);
            return;
        }

        std::int16_t mvp[2];
        predict_pskip_mv(mb_x(mb_addr) * 4, mb_y(mb_addr) * 4, mvp);
        const std::uint32_t base_x = mb_x(mb_addr) * 4;
        const std::uint32_t base_y = mb_y(mb_addr) * 4;
        const std::int16_t mvy = storeMvY(mvp[1], mb.field_mb);
        const std::int8_t ref0 = ref_count(0) > 0 ? 0 : -1;
        const std::uint8_t extra =
                mb.field_mb ? static_cast<std::uint8_t>(MvField) : std::uint8_t{0};
        simd::fillBlock(cells_, width4x4_, base_x, base_y, 4, 4,
                        simd::CellPattern::skip(mvp[0], mvy, ref0, extra).cell);
        std::fill(std::begin(mb.ref[0]), std::end(mb.ref[0]), ref0);
    }

    void parse_mb(std::uint32_t mb_addr) {
        detail::MbData& mb = mbs_[mb_addr];
        mb.reset();
        mb.available = true;
        mb.slice_id = slice_id_;
        mb.slice_group = slice_group_;
        mb.field_mb = mbaff_frame_ ? pair_field_flag_ : field_pic_;
        current_mb_field_ = mb.field_mb;
        transform8x8_allowed_ = pps_.transform_8x8_mode_flag;

        int raw_type = 0;
        if (is_cavlc()) {
            raw_type = static_cast<int>(cavlc().read_ue());
        } else if (is_i()) {
            raw_type = decode_intra_mb_type(3, true);
        } else if (is_p()) {
            raw_type = decode_p_mb_type();
        } else {
            raw_type = decode_b_mb_type(mb_addr);
        }

        const int pcm_type = is_i() ? 25 : (is_p() ? 30 : 48);
        if (raw_type == pcm_type) {
            mb.intra = true;
            /*
             * I_PCM (H.264 7.3.5): pcm_alignment_zero_bit pads to the next
             * byte, then the raw luma/chroma samples follow.  For CABAC the
             * arithmetic engine is re-initialised from the first nine bits
             * after the samples (9.3.1.2); the context model is retained.
             */
            if (is_cavlc()) {
                cavlc().byte_align();
                skip_cavlc_bits(ipcm_sample_bits());
            } else {
                cabac().byte_align();
                cabac().skip_bits(ipcm_sample_bits());
                cabac().restart();
            }
            /*
             * H.264 9.2.1 treats every 4x4 block of an I_PCM macroblock
             * as having TotalCoeff 16 for the neighbours' nC derivation.
             */
            mbs_[mb_addr].nzc.fill(16);
            /*
             * All transform blocks of an I_PCM macroblock are present.
             * CodedBlockPatternLuma = 15 and, when chroma exists,
             * CodedBlockPatternChroma = 2 (H.264 7.4.5, 8.5).  Neighbouring
             * CBP contexts in CABAC depend on these bits.
             */
            mbs_[mb_addr].cbp = static_cast<std::uint8_t>(
                    0x0F | (chroma_array_type_ == 1 || chroma_array_type_ == 2
                                    ? 0x20
                                    : 0x00));
            mbs_[mb_addr].luma_dc_cbf.fill(1);
            mbs_[mb_addr].chroma_dc_cbf[0] = 1;
            mbs_[mb_addr].chroma_dc_cbf[1] = 1;
            /*
             * I_PCM carries no mb_qp_delta; the next macroblock's CABAC
             * delta-QP context therefore starts from zero (9.3.3.1.1.10).
             */
            last_delta_qp_ = 0;
            mark_intra_mb(mb_addr);
            return;
        }

        const bool intra_raw = is_i() || (is_p() && raw_type >= 5) ||
                               (is_b() && raw_type >= 23);
        if (intra_raw) {
            mb.intra = true;
            if (is_i()) raw_type = raw_type; // 0 = I_NxN, 1..24 = I16
            else if (is_p()) raw_type -= 5;
            else raw_type -= 23;

            if (raw_type == 0) {
                mb.is_intra4x4 = true;
                parse_intra_modes(mb_addr, mb);
                /*
                 * I_NxN carries an explicit coded_block_pattern after the
                 * intra modes/chroma prediction (I16x16 derives its CBP
                 * from mb_type instead).
                 */
                if (is_cavlc()) {
                    const std::uint32_t code_num = cavlc().read_ue();
                    /*
                     * ChromaArrayType 0 (monochrome or separate colour
                     * planes) uses the same 16-entry CBP mapping as 4:4:4,
                     * because only the four luma bits are coded.
                     */
                    if (chroma_array_type_ == 3 || chroma_array_type_ == 0) {
                        const std::uint32_t cbp =
                                CavlcReader::cbp_from_code_num_444(code_num, true);
                        if (cbp == 0xFFFFFFFFu) {
                            status_ = MotionVectorStatus::Malformed;
                            return;
                        }
                        mb.cbp = static_cast<std::uint8_t>(cbp);
                    } else {
                        mb.cbp = static_cast<std::uint8_t>(
                                CavlcReader::cbp_from_code_num(code_num, true));
                    }
                } else {
                    int cbp = decode_cbp_luma(mb_addr);
                    if (chroma_array_type_ == 1 || chroma_array_type_ == 2) {
                        cbp |= decode_cbp_chroma(mb_addr) << 4;
                    }
                    mb.cbp = static_cast<std::uint8_t>(cbp);
                }
            } else if (raw_type >= 1 && raw_type <= 24) {
                mb.intra16x16 = true;
                mb.cbp = detail::kI16CbpTable[(raw_type - 1) >> 2];
                /*
                 * ChromaArrayType 3 reuses the four luma CBP bits for all
                 * three planes and ChromaArrayType 0 has no chroma CBP at
                 * all.  Table 7-11 still defines chroma bits for the
                 * Intra16x16 mb_type values, but they are not coded for
                 * these formats; keep only the luma pattern.  Encoders
                 * (x264 High 4:4:4) emit mb_type values whose table entry
                 * has chroma bits set, so rejecting them desynchronises the
                 * slice.
                 */
                if (chroma_array_type_ == 3 || chroma_array_type_ == 0) {
                    mb.cbp &= 0x0F;
                }
                parse_intra_modes(mb_addr, mb);
            } else {
                status_ = MotionVectorStatus::Malformed;
                return;
            }
            mark_intra_mb(mb_addr);
            if (mb.cbp != 0 || mb.intra16x16) {
                (void)decode_delta_qp();
                parse_residual_mb(mb_addr, mbs_[mb_addr]);
            } else {
                last_delta_qp_ = 0;
            }
            return;
        }

        // Inter macroblock.
        if (is_p()) {
            if (raw_type <= 2) {
                const auto& info = detail::kPTypeInfo[raw_type];
                if (info.shape == 0) {
                    write_partition(mb_addr, 0, 0, 4, 4, detail::PredMode::L0);
                } else {
                    const int bx0 = 0;
                    const int by0 = 0;
                    const int bx1 = info.shape == 1 ? 0 : 2;
                    const int by1 = info.shape == 1 ? 2 : 0;
                    const int w = info.shape == 1 ? 4 : 2;
                    const int h = info.shape == 1 ? 2 : 4;

                    detail::RefChoice r0{}, r1{};
                    const std::uint32_t ref_syntax = syntax_ref_count(0, mb.field_mb);
                    if (ref_syntax > 1) {
                        const std::uint32_t x0 = mb_x(mb_addr) * 4 + bx0;
                        const std::uint32_t y0 = mb_y(mb_addr) * 4 + by0;
                        int v = decode_ref(0, x0, y0);
                        if (v < 0 || static_cast<std::uint32_t>(v) >= ref_syntax) v = 0;
                        r0 = {true, v};
                        fill_ref_cache(mb_addr, 0, x0, y0, w, h, v);

                        const std::uint32_t x1 = mb_x(mb_addr) * 4 + bx1;
                        const std::uint32_t y1 = mb_y(mb_addr) * 4 + by1;
                        v = decode_ref(0, x1, y1);
                        if (v < 0 || static_cast<std::uint32_t>(v) >= ref_syntax) v = 0;
                        r1 = {true, v};
                        fill_ref_cache(mb_addr, 0, x1, y1, w, h, v);
                    }
                    write_partition(mb_addr, bx0, by0, w, h, detail::PredMode::L0, r0);
                    write_partition(mb_addr, bx1, by1, w, h, detail::PredMode::L0, r1);
                }
            } else {
                parse_p_8x8(mb_addr, raw_type == 4);
            }
        } else {
            if (raw_type == 0) {
                mb.direct = true;
                transform8x8_allowed_ =
                        transform8x8_allowed_ && sps_.direct_8x8_inference_flag;
                apply_direct(mb_addr, mb);
            } else if (raw_type == 22) {
                parse_b_8x8(mb_addr);
            } else if (raw_type >= 1 && raw_type <= 21) {
                const auto& info = detail::kBPartInfo[raw_type];
                const int bx0 = 0;
                const int by0 = 0;
                const int bx1 = info.shape == 1 ? 0 : 2;
                const int by1 = info.shape == 1 ? 2 : 0;
                const int w = info.shape == 1 ? 4 : 2;
                const int h = info.shape == 1 ? 2 : 4;
                detail::RefChoice refs[2][2]{};
                for (int list = 0; list < 2; ++list) {
                    for (int part = 0; part < info.count; ++part) {
                        const detail::PredMode pm = info.pred[part];
                        const bool uses = pm == detail::PredMode::Bi ||
                                          (pm == detail::PredMode::L0 && list == 0) ||
                                          (pm == detail::PredMode::L1 && list == 1);
                        if (!uses) continue;
                        const std::uint32_t ref_syntax = syntax_ref_count(list, mb.field_mb);
                        if (ref_syntax <= 1) {
                            refs[list][part] = {true, 0};
                            continue;
                        }
                        const int px = part == 0 ? bx0 : bx1;
                        const int py = part == 0 ? by0 : by1;
                        const std::uint32_t x4 = mb_x(mb_addr) * 4 + px;
                        const std::uint32_t y4 = mb_y(mb_addr) * 4 + py;
                        int v = decode_ref(list, x4, y4);
                        if (v < 0 || static_cast<std::uint32_t>(v) >= ref_syntax) v = 0;
                        refs[list][part] = {true, v};
                        fill_ref_cache(mb_addr, list, x4, y4, w, h, v);
                    }
                }
                std::int16_t pre_mvd[2][2][2]{};
                /*
                 * B-slice MVD syntax is list-major: every list 0 MVD for the
                 * partition set precedes list 1 (7.3.5.2).  CABAC contexts
                 * depend on the preceding MVDs, so cache them as they are
                 * decoded.
                 */
                for (int list = 0; list < 2; ++list) {
                    for (int part = 0; part < info.count; ++part) {
                        const detail::PredMode pm = info.pred[part];
                        const bool uses = pm == detail::PredMode::Bi ||
                                          (pm == detail::PredMode::L0 && list == 0) ||
                                          (pm == detail::PredMode::L1 && list == 1);
                        if (!uses) continue;
                        if (is_cavlc()) {
                            pre_mvd[list][part][0] =
                                    static_cast<std::int16_t>(cavlc().read_se());
                            pre_mvd[list][part][1] =
                                    static_cast<std::int16_t>(cavlc().read_se());
                        } else {
                            const int px = part == 0 ? bx0 : bx1;
                            const int py = part == 0 ? by0 : by1;
                            decode_mvd_cached(
                                    mb_addr,
                                    mb_x(mb_addr) * 4 + static_cast<std::uint32_t>(px),
                                    mb_y(mb_addr) * 4 + static_cast<std::uint32_t>(py),
                                    w, h, list, pre_mvd[list][part]);
                        }
                    }
                }
                const bool pre = true;
                if (info.shape == 0) {
                    write_partition(mb_addr, 0, 0, 4, 4, info.pred[0], refs[0][0], refs[1][0],
                                    pre ? pre_mvd[0][0] : nullptr,
                                    pre ? pre_mvd[1][0] : nullptr);
                } else if (info.shape == 1) {
                    write_partition(mb_addr, 0, 0, 4, 2, info.pred[0], refs[0][0], refs[1][0],
                                    pre ? pre_mvd[0][0] : nullptr,
                                    pre ? pre_mvd[1][0] : nullptr);
                    write_partition(mb_addr, 0, 2, 4, 2, info.pred[1], refs[0][1], refs[1][1],
                                    pre ? pre_mvd[0][1] : nullptr,
                                    pre ? pre_mvd[1][1] : nullptr);
                } else {
                    write_partition(mb_addr, 0, 0, 2, 4, info.pred[0], refs[0][0], refs[1][0],
                                    pre ? pre_mvd[0][0] : nullptr,
                                    pre ? pre_mvd[1][0] : nullptr);
                    write_partition(mb_addr, 2, 0, 2, 4, info.pred[1], refs[0][1], refs[1][1],
                                    pre ? pre_mvd[0][1] : nullptr,
                                    pre ? pre_mvd[1][1] : nullptr);
                }
            }
        }

        if (mb.intra16x16) {
            if (mb.cbp != 0) {
                (void)decode_delta_qp();
                parse_residual_mb(mb_addr, mbs_[mb_addr]);
            }
            return;
        }

        int cbp = 0;
        if (is_cavlc()) {
            const std::uint32_t code_num = cavlc().read_ue();
            if (chroma_array_type_ == 3 || chroma_array_type_ == 0) {
                const std::uint32_t parsed =
                        CavlcReader::cbp_from_code_num_444(code_num, mb.is_intra4x4);
                if (parsed == 0xFFFFFFFFu) {
                    status_ = MotionVectorStatus::Malformed;
                    return;
                }
                cbp = static_cast<int>(parsed);
            } else {
                cbp = static_cast<int>(
                        CavlcReader::cbp_from_code_num(code_num, mb.is_intra4x4));
            }
        } else {
            cbp = decode_cbp_luma(mb_addr);
            if (chroma_array_type_ == 1 || chroma_array_type_ == 2) {
                cbp |= decode_cbp_chroma(mb_addr) << 4;
            }
        }
        mbs_[mb_addr].cbp = static_cast<std::uint8_t>(cbp);

        if (transform8x8_allowed_ && (cbp & 15) && !mb.intra) {
            if (is_cavlc()) {
                mbs_[mb_addr].transform8x8 = cavlc().read_bit();
            } else {
                const auto* left = left_mb(mb_addr);
                const auto* top = top_mb(mb_addr);
                int ctx = 399;
                if (left != nullptr && left->transform8x8) ++ctx;
                if (top != nullptr && top->transform8x8) ++ctx;
                mbs_[mb_addr].transform8x8 = cabac().decode(cabac().ctx(ctx));
            }
        }

        if (cbp != 0) {
            (void)decode_delta_qp();
            parse_residual_mb(mb_addr, mbs_[mb_addr]);
        } else {
            last_delta_qp_ = 0;
        }
    }

    void parse_p_8x8(std::uint32_t mb_addr, bool ref0) {
        std::array<int, 4> types{};
        for (int i = 0; i < 4; ++i) {
            types[static_cast<std::size_t>(i)] = is_cavlc()
                                                         ? static_cast<int>(cavlc().read_ue())
                                                         : decode_sub_mb_type_p();
        }
        transform8x8_allowed_ =
                transform8x8_allowed_ &&
                std::all_of(types.begin(), types.end(), [](int type) { return type == 0; });

        std::array<detail::RefChoice, 4> refs{};
        for (int i = 0; i < 4; ++i) {
            const int bx = (i & 1) * 2;
            const int by = (i >> 1) * 2;
            const std::uint32_t ref_syntax = syntax_ref_count(0, mbs_[mb_addr].field_mb);
            if (ref0 || ref_syntax <= 1) {
                refs[static_cast<std::size_t>(i)] = {true, 0};
                continue;
            }
            const std::uint32_t x4 = mb_x(mb_addr) * 4 + bx;
            const std::uint32_t y4 = mb_y(mb_addr) * 4 + by;
            int v = decode_ref(0, x4, y4);
            if (v < 0 || static_cast<std::uint32_t>(v) >= ref_syntax) v = 0;
            refs[static_cast<std::size_t>(i)] = {true, v};
            fill_ref_cache(mb_addr, 0, x4, y4, 2, 2, v);
        }

        for (int i = 0; i < 4; ++i) {
            const auto& info = detail::kPSubTypeInfo[static_cast<std::size_t>(types[static_cast<std::size_t>(i)])];
            const int bx = (i & 1) * 2;
            const int by = (i >> 1) * 2;
            for (int p = 0; p < info.parts; ++p) {
                const auto [ox, oy] = detail::sub_part_offset(info, p);
                const int px = bx + ox;
                const int py = by + oy;
                write_partition(mb_addr, px, py, info.w, info.h, detail::PredMode::L0,
                                refs[static_cast<std::size_t>(i)]);
            }
        }
    }

    void parse_b_8x8(std::uint32_t mb_addr) {
        std::array<int, 4> types{};
        for (int i = 0; i < 4; ++i) {
            types[static_cast<std::size_t>(i)] = is_cavlc()
                                                         ? static_cast<int>(cavlc().read_ue())
                                                         : decode_sub_mb_type_b();
        }
        transform8x8_allowed_ =
                transform8x8_allowed_ &&
                std::all_of(types.begin(), types.end(), [this](int type) {
                    return (type >= 1 && type <= 3) ||
                           (type == 0 && sps_.direct_8x8_inference_flag);
                });

        detail::RefChoice refs[2][4]{};
        for (int list = 0; list < 2; ++list) {
            for (int i = 0; i < 4; ++i) {
                const int type = types[static_cast<std::size_t>(i)];
                if (!detail::b_sub_uses_list(type, list)) {
                    refs[list][i] = {true, 0};
                    continue;
                }
                const std::uint32_t ref_syntax = syntax_ref_count(list, mbs_[mb_addr].field_mb);
                if (ref_syntax <= 1) {
                    refs[list][i] = {true, 0};
                    continue;
                }
                const int bx = (i & 1) * 2;
                const int by = (i >> 1) * 2;
                const std::uint32_t x4 = mb_x(mb_addr) * 4 + bx;
                const std::uint32_t y4 = mb_y(mb_addr) * 4 + by;
                int v = decode_ref(list, x4, y4);
                if (v < 0 || static_cast<std::uint32_t>(v) >= ref_syntax) v = 0;
                refs[list][i] = {true, v};
                fill_ref_cache(mb_addr, list, x4, y4, 2, 2, v);
            }
        }

        std::int16_t pre_mvd[2][4][4][2]{};
        /*
         * Read all B_8x8 MVDs list-major before writing any partition so the
         * syntax order matches H.264 7.3.5.2.  For CABAC the MVD cache is
         * updated as each value is decoded because later contexts depend on
         * the preceding MVDs.
         */
        for (int list = 0; list < 2; ++list) {
            for (int i = 0; i < 4; ++i) {
                const int type = types[static_cast<std::size_t>(i)];
                if (type == 0) continue;
                const detail::PredMode pred = detail::b_sub_pred(type);
                const bool uses = pred == detail::PredMode::Bi ||
                                  (pred == detail::PredMode::L0 && list == 0) ||
                                  (pred == detail::PredMode::L1 && list == 1);
                if (!uses) continue;
                const auto& info =
                        detail::kBSubTypeInfo[static_cast<std::size_t>(type)];
                const int bx = (i & 1) * 2;
                const int by = (i >> 1) * 2;
                for (int p = 0; p < info.parts; ++p) {
                    const auto [ox, oy] = detail::sub_part_offset(info, p);
                    const int px = bx + ox;
                    const int py = by + oy;
                    if (is_cavlc()) {
                        pre_mvd[list][i][p][0] =
                                static_cast<std::int16_t>(cavlc().read_se());
                        pre_mvd[list][i][p][1] =
                                static_cast<std::int16_t>(cavlc().read_se());
                    } else {
                        decode_mvd_cached(
                                mb_addr,
                                mb_x(mb_addr) * 4 + static_cast<std::uint32_t>(px),
                                mb_y(mb_addr) * 4 + static_cast<std::uint32_t>(py),
                                info.w, info.h, list, pre_mvd[list][i][p]);
                    }
                }
            }
        }

        for (int i = 0; i < 4; ++i) {
            const int type = types[static_cast<std::size_t>(i)];
            const int bx = (i & 1) * 2;
            const int by = (i >> 1) * 2;
            if (type == 0) {
                apply_direct(mb_addr, mbs_[mb_addr], bx, by, 2, 2);
                continue;
            }
            const detail::PredMode pred = detail::b_sub_pred(type);
            const auto& info = detail::kBSubTypeInfo[static_cast<std::size_t>(type)];
            for (int p = 0; p < info.parts; ++p) {
                const auto [ox, oy] = detail::sub_part_offset(info, p);
                const int px = bx + ox;
                const int py = by + oy;
                write_partition(mb_addr, px, py, info.w, info.h, pred,
                                refs[0][i], refs[1][i],
                                pre_mvd[0][i][p], pre_mvd[1][i][p]);
            }
        }
    }

    const SequenceParameterSet& sps_;
    const PictureParameterSet& pps_;
    const SliceHeader& header_;
    bool idr_ = false;
    std::uint32_t width4x4_ = 0;
    std::uint32_t height4x4_ = 0;
    std::uint32_t mb_width_ = 0;
    std::uint32_t mb_height_ = 0;
    std::uint32_t mb_count_ = 0;
    bool field_pic_ = false;
    bool mbaff_frame_ = false;
    bool current_mb_field_ = false;
    bool pair_field_flag_ = false;
    std::uint32_t pair_flag_pair_ = kInvalidAddr;
    MvCell* cells_ = nullptr;
    std::vector<detail::MbData>& mbs_;
    std::uint32_t slice_id_ = 0;
    std::vector<std::uint8_t> mb_to_slice_group_{};
    std::uint8_t slice_group_ = 0;
    bool fmo_ = false;
    const std::vector<MotionReferencePicture>& list0_;
    const std::vector<MotionReferencePicture>& list1_;
    MotionVectorStatus& status_;
    std::optional<CabacDecoder> cabac_;
    std::optional<CavlcReader> cavlc_;
    bool in_cavlc_slice_ = false;
    bool transform8x8_allowed_ = false;
    int slice_qp_ = 26;
    int last_qp_ = 26;
    int max_qp_ = 51;
    int last_delta_qp_ = 0;
    /*
     * ChromaArrayType (H.264 7.4.2.1.1): 0 for monochrome and for separate
     * colour planes, otherwise chroma_format_idc.  Every macroblock-layer
     * chroma decision follows this value, not chroma_format_idc, so a
     * separate-colour-plane slice is parsed with monochrome syntax.
     */
    std::uint8_t chroma_array_type_ = 1;
};

} // namespace avc
} // namespace bs
