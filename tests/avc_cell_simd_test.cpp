// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * ---------------------------------------------------------------------------
 * Portable SIMD cell-kernel tests
 * ---------------------------------------------------------------------------
 *
 * Every public kernel is compared byte-for-byte with its scalar reference on
 * randomised patterns and grids, deliberately misaligned buffers, zero-length
 * ranges, every partition width/height used by H.264 (1..4 cells in each
 * axis) and edge coordinates (last row/column of a non-square grid).
 *
 * The temporal-direct helper is additionally compared with an independent
 * transcription of SliceDataParser::apply_direct(), including the POC clamps
 * at +/-128, td == 0 and int16 saturation.
 *
 * The last section is a fixed-size microbenchmark over a 480x270-cell grid
 * (1920x1080 luma, exactly 2,073,600 bytes).
 */

#include "avc_cell_simd.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #cond << "\n"; \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

using bs::avc::MvCell;
namespace simd = bs::avc::simd;

/* -------------------------------------------------------------------------
 * Randomness and byte-exact helpers
 * ---------------------------------------------------------------------- */

struct Rng {
    std::uint64_t state = 0x9E3779B97F4A7C15ull;

    std::uint64_t next() noexcept {
        std::uint64_t x = state;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        state = x;
        return x * 0x2545F4914F6CDD1Dull;
    }

    std::uint32_t below(std::uint32_t n) noexcept {
        return n == 0 ? 0 : static_cast<std::uint32_t>(next() % n);
    }

    std::int32_t range(std::int32_t lo, std::int32_t hi) noexcept {
        return lo + static_cast<std::int32_t>(below(static_cast<std::uint32_t>(hi - lo + 1)));
    }

    bool bit() noexcept { return (next() & 1u) != 0; }
};

MvCell randomCell(Rng& rng) {
    MvCell cell{};
    for (int list = 0; list < 2; ++list) {
        cell.mv[list][0] = static_cast<std::int16_t>(rng.next());
        cell.mv[list][1] = static_cast<std::int16_t>(rng.next());
        cell.ref_idx[list] = static_cast<std::int8_t>(rng.next());
    }
    cell.flags = static_cast<std::uint8_t>(rng.next());
    for (std::uint8_t& byte : cell.reserved) {
        byte = static_cast<std::uint8_t>(rng.next());
    }
    return cell;
}

bool sameBytes(const MvCell& a, const MvCell& b) {
    return std::memcmp(&a, &b, sizeof(MvCell)) == 0;
}

bool sameRegion(const MvCell* a, const MvCell* b, std::size_t count) {
    return count == 0 || std::memcmp(a, b, count * sizeof(MvCell)) == 0;
}

/*
 * Byte arena with a controllable even (but not necessarily 16-byte) offset.
 * The surrounding bytes stay at the 0xA5 sentinel, so an overrun in any
 * kernel is visible even without a sanitizer.
 */
struct Arena {
    std::vector<std::uint64_t> storage;
    std::size_t bytes = 0;

    explicit Arena(std::size_t payload_bytes) {
        bytes = payload_bytes + 64;
        storage.assign((bytes + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t),
                       0xA5A5A5A5A5A5A5A5ull);
    }

    std::uint8_t* data() {
        return reinterpret_cast<std::uint8_t*>(storage.data());
    }

    MvCell* cells(std::size_t offset) {
        return reinterpret_cast<MvCell*>(data() + offset);
    }

    const MvCell* cells(std::size_t offset) const {
        return reinterpret_cast<const MvCell*>(
                reinterpret_cast<const std::uint8_t*>(storage.data()) + offset);
    }
};

/* Even offsets keep MvCell alignment valid while defeating 16-byte alignment. */
constexpr std::size_t kUnalignedOffsets[] = {0, 2, 4, 6, 8, 10, 12, 14};

bool guardIntact(const Arena& arena, std::size_t before, std::size_t after) {
    const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(arena.storage.data());
    for (std::size_t i = 0; i < before; ++i) {
        if (bytes[i] != 0xA5) {
            return false;
        }
    }
    for (std::size_t i = after; i < arena.bytes; ++i) {
        if (bytes[i] != 0xA5) {
            return false;
        }
    }
    return true;
}

std::vector<MvCell> randomGrid(Rng& rng, std::size_t count) {
    std::vector<MvCell> grid(count);
    for (MvCell& cell : grid) {
        cell = randomCell(rng);
    }
    return grid;
}

/* -------------------------------------------------------------------------
 * fillCells / copyCells
 * ---------------------------------------------------------------------- */

void test_fill_cells() {
    Rng rng{0x1111111111111111ull};
    for (int iteration = 0; iteration < 400; ++iteration) {
        const std::size_t count = iteration < 40 ? static_cast<std::size_t>(iteration % 20)
                                                 : static_cast<std::size_t>(rng.below(600));
        const std::size_t offset =
                kUnalignedOffsets[rng.below(sizeof(kUnalignedOffsets) / sizeof(std::size_t))];
        const MvCell pattern = randomCell(rng);

        Arena reference(count * sizeof(MvCell) + 32);
        Arena vectorized(count * sizeof(MvCell) + 32);
        simd::scalar::fillCells(reference.cells(offset), count, pattern);
        simd::fillCells(vectorized.cells(offset), count, pattern);

        CHECK(sameRegion(reference.cells(offset), vectorized.cells(offset), count));
        CHECK(guardIntact(reference, offset, offset + count * sizeof(MvCell)));
        CHECK(guardIntact(vectorized, offset, offset + count * sizeof(MvCell)));
    }

    /* No-op with a null pointer and count 0 must not touch memory. */
    simd::fillCells(nullptr, 0, MvCell{});
    simd::scalar::fillCells(nullptr, 0, MvCell{});
}

void test_copy_cells() {
    Rng rng{0x2222222222222222ull};
    for (int iteration = 0; iteration < 400; ++iteration) {
        const std::size_t count = iteration < 40 ? static_cast<std::size_t>(iteration % 20)
                                                 : static_cast<std::size_t>(rng.below(600));
        const std::size_t src_offset =
                kUnalignedOffsets[rng.below(sizeof(kUnalignedOffsets) / sizeof(std::size_t))];
        const std::size_t dst_offset =
                kUnalignedOffsets[rng.below(sizeof(kUnalignedOffsets) / sizeof(std::size_t))];

        const std::vector<MvCell> source = randomGrid(rng, count);
        Arena reference_src(count * sizeof(MvCell) + 32);
        Arena vectorized_src(count * sizeof(MvCell) + 32);
        Arena reference_dst(count * sizeof(MvCell) + 32);
        Arena vectorized_dst(count * sizeof(MvCell) + 32);
        if (count != 0) {
            std::memcpy(reference_src.cells(src_offset), source.data(), count * sizeof(MvCell));
            std::memcpy(vectorized_src.cells(src_offset), source.data(), count * sizeof(MvCell));
        }
        const MvCell sentinel = randomCell(rng);
        simd::scalar::fillCells(reference_dst.cells(dst_offset), count, sentinel);
        simd::fillCells(vectorized_dst.cells(dst_offset), count, sentinel);

        simd::scalar::copyCells(
                reference_dst.cells(dst_offset), reference_src.cells(src_offset), count);
        simd::copyCells(
                vectorized_dst.cells(dst_offset), vectorized_src.cells(src_offset), count);

        CHECK(sameRegion(reference_dst.cells(dst_offset), vectorized_dst.cells(dst_offset), count));
        CHECK(sameRegion(reference_src.cells(src_offset), vectorized_src.cells(src_offset), count));
        CHECK(guardIntact(vectorized_dst, dst_offset, dst_offset + count * sizeof(MvCell)));
        CHECK(guardIntact(vectorized_src, src_offset, src_offset + count * sizeof(MvCell)));
    }

    simd::copyCells(nullptr, nullptr, 0);
    simd::scalar::copyCells(nullptr, nullptr, 0);
}

/* -------------------------------------------------------------------------
 * fillRows / fillBlock
 * ---------------------------------------------------------------------- */

void check_fill_block_case(
        Rng& rng,
        std::uint32_t stride,
        std::uint32_t rows,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t w,
        std::uint32_t h,
        const MvCell& pattern) {
    const std::size_t count = static_cast<std::size_t>(stride) * rows;
    const std::size_t offset =
            kUnalignedOffsets[rng.below(sizeof(kUnalignedOffsets) / sizeof(std::size_t))];
    const std::vector<MvCell> initial = randomGrid(rng, count);

    Arena reference(count * sizeof(MvCell) + 32);
    Arena vectorized(count * sizeof(MvCell) + 32);
    std::memcpy(reference.cells(offset), initial.data(), count * sizeof(MvCell));
    std::memcpy(vectorized.cells(offset), initial.data(), count * sizeof(MvCell));

    simd::scalar::fillRows(reference.cells(offset), stride, x, y, w, h, pattern);
    simd::fillBlock(vectorized.cells(offset), stride, x, y, w, h, pattern);

    CHECK(sameRegion(reference.cells(offset), vectorized.cells(offset), count));
    CHECK(guardIntact(vectorized, offset, offset + count * sizeof(MvCell)));
}

void test_fill_block() {
    Rng rng{0x3333333333333333ull};
    const std::uint32_t strides[] = {1, 2, 3, 4, 5, 7, 8, 17, 31, 64, 480};

    /* Every partition shape used by H.264 (P/B 16x16 down to 4x4). */
    for (int iteration = 0; iteration < 700; ++iteration) {
        const std::uint32_t stride =
                strides[rng.below(sizeof(strides) / sizeof(strides[0]))];
        const std::uint32_t w = 1 + rng.below(4);
        const std::uint32_t h = 1 + rng.below(4);
        const std::uint32_t rows = 1 + rng.below(24) + h;
        const std::uint32_t x = stride >= w ? rng.below(stride - w + 1) : 0;
        const std::uint32_t y = rng.below(rows - h + 1);
        if (stride < w) {
            continue;
        }
        check_fill_block_case(rng, stride, rows, x, y, w, h, randomCell(rng));
    }

    /* Explicit end-of-grid and full-width fast-path cases. */
    struct Case {
        std::uint32_t stride;
        std::uint32_t rows;
        std::uint32_t x;
        std::uint32_t y;
        std::uint32_t w;
        std::uint32_t h;
    };
    const Case cases[] = {
            {1, 1, 0, 0, 1, 1},
            {4, 4, 0, 0, 4, 4},
            {4, 4, 3, 3, 1, 1},
            {5, 5, 4, 4, 1, 1},
            {8, 6, 0, 0, 8, 6},
            {8, 6, 4, 2, 4, 4},
            {480, 270, 0, 0, 480, 270},
            {480, 270, 476, 266, 4, 4},
            {480, 270, 479, 269, 1, 1},
    };
    for (const Case& c : cases) {
        const MvCell pattern = randomCell(rng);
        check_fill_block_case(rng, c.stride, c.rows, c.x, c.y, c.w, c.h, pattern);

        /* fillRows and fillBlock must agree with each other as well. */
        const std::size_t count = static_cast<std::size_t>(c.stride) * c.rows;
        std::vector<MvCell> a(count, MvCell{});
        std::vector<MvCell> b(count, MvCell{});
        simd::fillRows(a.data(), c.stride, c.x, c.y, c.w, c.h, pattern);
        simd::fillBlock(b.data(), c.stride, c.x, c.y, c.w, c.h, pattern);
        CHECK(sameRegion(a.data(), b.data(), count));
    }

    /* Zero extent must be a no-op even at the end of the grid. */
    std::vector<MvCell> grid(16, randomCell(rng));
    const std::vector<MvCell> before = grid;
    simd::fillBlock(grid.data(), 4, 4, 4, 0, 4, MvCell{});
    simd::fillBlock(grid.data(), 4, 4, 4, 4, 0, MvCell{});
    CHECK(sameRegion(grid.data(), before.data(), grid.size()));
}

/* -------------------------------------------------------------------------
 * CellPattern factories
 * ---------------------------------------------------------------------- */

void test_patterns() {
    {
        const simd::CellPattern p = simd::CellPattern::intra();
        CHECK(p.cell.flags == bs::avc::MvIntra);
        CHECK(p.cell.ref_idx[0] == -1 && p.cell.ref_idx[1] == -1);
        CHECK(p.cell.mv[0][0] == 0 && p.cell.mv[0][1] == 0);
        CHECK(p.cell.mv[1][0] == 0 && p.cell.mv[1][1] == 0);
        for (std::uint8_t byte : p.cell.reserved) {
            CHECK(byte == 0);
        }

        const simd::CellPattern field = simd::CellPattern::intra(bs::avc::MvField);
        CHECK(field.cell.flags == (bs::avc::MvIntra | bs::avc::MvField));
    }
    {
        const simd::CellPattern p = simd::CellPattern::unavailable();
        CHECK(p.cell.flags == bs::avc::MvUnavailable);
        CHECK(p.cell.ref_idx[0] == -1 && p.cell.ref_idx[1] == -1);
        CHECK(p.cell.mv[0][0] == 0 && p.cell.mv[1][1] == 0);
    }
    {
        const simd::CellPattern p = simd::CellPattern::skip(123, -456, 3);
        CHECK(p.cell.mv[0][0] == 123 && p.cell.mv[0][1] == -456);
        CHECK(p.cell.mv[1][0] == 0 && p.cell.mv[1][1] == 0);
        CHECK(p.cell.ref_idx[0] == 3 && p.cell.ref_idx[1] == -1);
        CHECK(p.cell.flags == (bs::avc::MvSkip | bs::avc::MvList0));
        CHECK(p.cell.reserved[0] == 0);

        const simd::CellPattern no_ref = simd::CellPattern::skip(1, 2, -1);
        CHECK(no_ref.cell.flags == (bs::avc::MvSkip | bs::avc::MvUnavailable));
        CHECK(no_ref.cell.ref_idx[0] == -1);
    }
    {
        const simd::CellPattern l0 = simd::CellPattern::l0Partition(7, -8, 2);
        CHECK(l0.cell.mv[0][0] == 7 && l0.cell.mv[0][1] == -8);
        CHECK(l0.cell.mv[1][0] == 0 && l0.cell.mv[1][1] == 0);
        CHECK(l0.cell.ref_idx[0] == 2 && l0.cell.ref_idx[1] == -1);
        CHECK(l0.cell.flags == bs::avc::MvList0);

        const simd::CellPattern l1 = simd::CellPattern::l1Partition(-9, 10, 4);
        CHECK(l1.cell.mv[1][0] == -9 && l1.cell.mv[1][1] == 10);
        CHECK(l1.cell.mv[0][0] == 0 && l1.cell.mv[0][1] == 0);
        CHECK(l1.cell.ref_idx[1] == 4 && l1.cell.ref_idx[0] == -1);
        CHECK(l1.cell.flags == bs::avc::MvList1);

        const simd::CellPattern bi = simd::CellPattern::biPartition(1, 2, 0, -3, -4, 5);
        CHECK(bi.cell.mv[0][0] == 1 && bi.cell.mv[0][1] == 2);
        CHECK(bi.cell.mv[1][0] == -3 && bi.cell.mv[1][1] == -4);
        CHECK(bi.cell.ref_idx[0] == 0 && bi.cell.ref_idx[1] == 5);
        CHECK(bi.cell.flags == (bs::avc::MvList0 | bs::avc::MvList1));
    }
}

/* -------------------------------------------------------------------------
 * Temporal-direct reference
 * ---------------------------------------------------------------------- */

std::int16_t referenceClampMv(std::int64_t value) {
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(value, -32768, 32767));
}

std::int16_t referenceStoreMvY(std::int16_t value, bool field_mb, bool mbaff_frame) {
    if (!mbaff_frame || !field_mb) {
        return value;
    }
    return static_cast<std::int16_t>(std::clamp(static_cast<int>(value) * 2, -32768, 32767));
}

/*
 * Independent transcription of the temporal-direct branch of
 * SliceDataParser::apply_direct() (parser/avc_mb_parser.hpp).  Deliberately
 * does not call any helper from avc_cell_simd.hpp.
 */
MvCell referenceDirectCell(
        const MvCell& co_located,
        int col_list,
        std::int32_t current_poc,
        std::int32_t pic0_poc,
        std::int32_t ref1_poc,
        std::int8_t ref0,
        std::int8_t ref1,
        bool current_field_mb,
        bool col_field_pic,
        bool mbaff_frame) {
    std::int64_t mvx = co_located.mv[col_list][0];
    std::int64_t mvy = co_located.mv[col_list][1];
    if (current_field_mb && !col_field_pic) {
        mvy /= 2;
    } else if (!current_field_mb && col_field_pic) {
        mvy *= 2;
    }

    const std::int64_t td = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(ref1_poc) - pic0_poc, -128, 127);
    const std::int64_t tb = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(current_poc) - pic0_poc, -128, 127);

    std::int64_t mv0x = mvx;
    std::int64_t mv0y = mvy;
    std::int64_t mv1x = 0;
    std::int64_t mv1y = 0;
    if (td != 0) {
        const std::int64_t abs_td = td < 0 ? -td : td;
        const std::int64_t tx = (16384 + abs_td / 2) / td;
        const std::int64_t dist_scale = std::clamp<std::int64_t>(
                (tb * tx + 32) >> 6, -1024, 1023);
        mv0x = (dist_scale * mvx + 128) >> 8;
        mv0y = (dist_scale * mvy + 128) >> 8;
        mv1x = mv0x - mvx;
        mv1y = mv0y - mvy;
    }

    MvCell cell{};
    cell.mv[0][0] = referenceClampMv(mv0x);
    cell.mv[0][1] = referenceStoreMvY(
            referenceClampMv(mv0y), current_field_mb, mbaff_frame);
    cell.mv[1][0] = referenceClampMv(mv1x);
    cell.mv[1][1] = referenceStoreMvY(
            referenceClampMv(mv1y), current_field_mb, mbaff_frame);
    cell.ref_idx[0] = ref0;
    cell.ref_idx[1] = ref1;
    std::uint8_t flags = static_cast<std::uint8_t>(bs::avc::MvDirect | bs::avc::MvList0);
    if (ref1 >= 0) {
        flags = static_cast<std::uint8_t>(flags | bs::avc::MvList1);
    }
    if (current_field_mb) {
        flags = static_cast<std::uint8_t>(flags | bs::avc::MvField);
    }
    cell.flags = flags;
    return cell;
}

void test_scale_direct_cell() {
    Rng rng{0x4444444444444444ull};
    const std::int32_t poc_values[] = {
            -1000, -513, -512, -129, -128, -127, -65, -2, -1, 0,
            1,      2,    63,   64,   127,  128,  129,  255, 256, 512, 513, 1000};

    for (int iteration = 0; iteration < 30000; ++iteration) {
        MvCell co_located = randomCell(rng);
        if (iteration % 7 == 0) {
            /* Extreme int16 vectors exercise the saturation and rounding. */
            const std::int16_t extremes[] = {
                    std::numeric_limits<std::int16_t>::min(),
                    std::numeric_limits<std::int16_t>::max(),
                    0, 1, -1, 2, -2, 255, -255, 256, -256};
            co_located.mv[0][0] =
                    extremes[rng.below(sizeof(extremes) / sizeof(extremes[0]))];
            co_located.mv[0][1] =
                    extremes[rng.below(sizeof(extremes) / sizeof(extremes[0]))];
            co_located.mv[1][0] =
                    extremes[rng.below(sizeof(extremes) / sizeof(extremes[0]))];
            co_located.mv[1][1] =
                    extremes[rng.below(sizeof(extremes) / sizeof(extremes[0]))];
        }

        const std::int32_t current_poc =
                poc_values[rng.below(sizeof(poc_values) / sizeof(poc_values[0]))];
        const std::int32_t pic0_poc =
                poc_values[rng.below(sizeof(poc_values) / sizeof(poc_values[0]))];
        const std::int32_t ref1_poc =
                poc_values[rng.below(sizeof(poc_values) / sizeof(poc_values[0]))];

        const int col_list = rng.bit() ? 0 : 1;
        const std::int8_t ref0 = static_cast<std::int8_t>(rng.below(17));
        const std::int8_t ref1 = rng.bit() ? static_cast<std::int8_t>(rng.below(17)) : -1;
        const bool current_field_mb = rng.bit();
        const bool col_field_pic = rng.bit();
        const bool mbaff_frame = rng.bit();

        simd::DirectScaleParams params;
        params.current_poc = current_poc;
        params.col_poc = pic0_poc;
        params.ref1_poc = ref1_poc;
        params.ref0 = ref0;
        params.ref1 = ref1;
        params.current_field_mb = current_field_mb;
        params.col_field_pic = col_field_pic;
        params.mbaff_frame = mbaff_frame;

        const MvCell expected = referenceDirectCell(
                co_located, col_list, current_poc, pic0_poc, ref1_poc,
                ref0, ref1, current_field_mb, col_field_pic, mbaff_frame);
        const MvCell actual = simd::scaleDirectCell(co_located, col_list, params);
        CHECK(sameBytes(expected, actual));
        CHECK(sameBytes(actual, simd::scalar::scaleDirectCell(co_located, col_list, params)));

        /* The convenience overload must select the same list as apply_direct. */
        const int auto_list = co_located.ref_idx[0] >= 0 ? 0 : 1;
        const MvCell auto_expected = referenceDirectCell(
                co_located, auto_list, current_poc, pic0_poc, ref1_poc,
                ref0, ref1, current_field_mb, col_field_pic, mbaff_frame);
        CHECK(sameBytes(auto_expected, simd::scaleDirectCell(co_located, params)));
    }

    /* td == 0 leaves the co-located vector unscaled and list 1 zero. */
    MvCell co_located{};
    co_located.mv[0][0] = 400;
    co_located.mv[0][1] = -200;
    simd::DirectScaleParams params;
    params.current_poc = 10;
    params.col_poc = 4;
    params.ref1_poc = 4;
    params.ref0 = 0;
    params.ref1 = 0;
    const MvCell td_zero = simd::scaleDirectCell(co_located, 0, params);
    CHECK(td_zero.mv[0][0] == 400 && td_zero.mv[0][1] == -200);
    CHECK(td_zero.mv[1][0] == 0 && td_zero.mv[1][1] == 0);
}

void test_scale_direct_block() {
    Rng rng{0x5555555555555555ull};
    for (int iteration = 0; iteration < 300; ++iteration) {
        const std::uint32_t stride = 4 + rng.below(60);
        const std::uint32_t w = 1 + rng.below(4);
        const std::uint32_t h = 1 + rng.below(4);
        const std::uint32_t rows = 4 + rng.below(20);
        const std::uint32_t x = rng.below(stride - w + 1);
        const std::uint32_t y = rng.below(rows - h + 1);
        const std::size_t count = static_cast<std::size_t>(stride) * rows;
        const std::size_t offset =
                kUnalignedOffsets[rng.below(sizeof(kUnalignedOffsets) / sizeof(std::size_t))];

        const std::vector<MvCell> initial = randomGrid(rng, count);
        Arena reference(count * sizeof(MvCell) + 32);
        Arena vectorized(count * sizeof(MvCell) + 32);
        std::memcpy(reference.cells(offset), initial.data(), count * sizeof(MvCell));
        std::memcpy(vectorized.cells(offset), initial.data(), count * sizeof(MvCell));

        const MvCell co_located = randomCell(rng);
        const int col_list = co_located.ref_idx[0] >= 0 ? 0 : 1;

        simd::DirectScaleParams params;
        params.current_poc = rng.range(-300, 300);
        params.col_poc = rng.range(-300, 300);
        params.ref1_poc = rng.range(-300, 300);
        params.ref0 = static_cast<std::int8_t>(rng.below(8));
        params.ref1 = rng.bit() ? static_cast<std::int8_t>(rng.below(8)) : -1;
        params.current_field_mb = rng.bit();
        params.col_field_pic = rng.bit();
        params.mbaff_frame = rng.bit();

        simd::scalar::scaleDirectBlock(
                reference.cells(offset), stride, x, y, w, h, co_located, col_list, params);
        simd::scaleDirectBlock(
                vectorized.cells(offset), stride, x, y, w, h, co_located, col_list, params);

        CHECK(sameRegion(reference.cells(offset), vectorized.cells(offset), count));
        CHECK(guardIntact(vectorized, offset, offset + count * sizeof(MvCell)));

        /* Every written cell must equal the scalar temporal-direct result. */
        const MvCell expected = simd::scaleDirectCell(co_located, col_list, params);
        for (std::uint32_t row = 0; row < h; ++row) {
            for (std::uint32_t column = 0; column < w; ++column) {
                CHECK(sameBytes(
                        expected,
                        vectorized.cells(offset)[static_cast<std::size_t>(y + row) * stride +
                                                 x + column]));
            }
        }

        /* The overload without an explicit list uses apply_direct's choice. */
        std::vector<MvCell> auto_grid(count, MvCell{});
        simd::scaleDirectBlock(
                auto_grid.data(), stride, x, y, w, h, co_located, params);
        const MvCell auto_expected = simd::scaleDirectCell(co_located, params);
        CHECK(sameBytes(
                auto_expected,
                auto_grid[static_cast<std::size_t>(y) * stride + x]));
    }
}

/* -------------------------------------------------------------------------
 * Direct backend-kernel tests
 * -------------------------------------------------------------------------
 *
 * The public API picks one backend at run time, so on an AVX2 machine the
 * SSE2 kernels would otherwise never execute.  Exercise each compiled kernel
 * against the scalar reference directly, in addition to the dispatch tests
 * above.  This also keeps the one-cell fast path from hiding a broken SIMD
 * tail.
 */

using FillCellsFn = void (*)(MvCell*, std::size_t, const MvCell&);
using FillRowsFn = void (*)(
        MvCell*,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        const MvCell&);
using CopyCellsFn = void (*)(MvCell*, const MvCell*, std::size_t);

[[maybe_unused]] void test_kernel_set(
        const char* label,
        FillCellsFn fill_cells,
        FillRowsFn fill_rows,
        CopyCellsFn copy_cells) {
    Rng rng{0x7777777777777777ull};
    for (int iteration = 0; iteration < 200; ++iteration) {
        /* fillCells: every tail shape, including unaligned destinations. */
        const std::size_t count = static_cast<std::size_t>(rng.below(400));
        const std::size_t offset =
                kUnalignedOffsets[rng.below(sizeof(kUnalignedOffsets) / sizeof(std::size_t))];
        const MvCell pattern = randomCell(rng);
        Arena reference(count * sizeof(MvCell) + 32);
        Arena vectorized(count * sizeof(MvCell) + 32);
        simd::scalar::fillCells(reference.cells(offset), count, pattern);
        fill_cells(vectorized.cells(offset), count, pattern);
        CHECK(sameRegion(reference.cells(offset), vectorized.cells(offset), count));
        CHECK(guardIntact(vectorized, offset, offset + count * sizeof(MvCell)));

        /* fillRows: all 1..4 widths/heights and a non-square stride. */
        const std::uint32_t stride = 1 + rng.below(48);
        const std::uint32_t w = 1 + rng.below(4);
        const std::uint32_t h = 1 + rng.below(4);
        const std::uint32_t rows = h + rng.below(20);
        const std::uint32_t x = stride >= w ? rng.below(stride - w + 1) : 0;
        const std::uint32_t y = rng.below(rows - h + 1);
        if (stride >= w) {
            const std::size_t cells = static_cast<std::size_t>(stride) * rows;
            const std::vector<MvCell> initial = randomGrid(rng, cells);
            Arena rows_reference(cells * sizeof(MvCell) + 32);
            Arena rows_vectorized(cells * sizeof(MvCell) + 32);
            std::memcpy(rows_reference.cells(offset), initial.data(), cells * sizeof(MvCell));
            std::memcpy(rows_vectorized.cells(offset), initial.data(), cells * sizeof(MvCell));
            simd::scalar::fillRows(
                    rows_reference.cells(offset), stride, x, y, w, h, pattern);
            fill_rows(rows_vectorized.cells(offset), stride, x, y, w, h, pattern);
            CHECK(sameRegion(
                    rows_reference.cells(offset), rows_vectorized.cells(offset), cells));
            CHECK(guardIntact(
                    rows_vectorized, offset, offset + cells * sizeof(MvCell)));
        }

        /* copyCells with independent source/destination buffers. */
        const std::size_t copy_count = static_cast<std::size_t>(rng.below(400));
        const std::vector<MvCell> source = randomGrid(rng, copy_count);
        const std::size_t src_offset =
                kUnalignedOffsets[rng.below(sizeof(kUnalignedOffsets) / sizeof(std::size_t))];
        const std::size_t dst_offset =
                kUnalignedOffsets[rng.below(sizeof(kUnalignedOffsets) / sizeof(std::size_t))];
        Arena copy_reference_src(copy_count * sizeof(MvCell) + 32);
        Arena copy_vectorized_src(copy_count * sizeof(MvCell) + 32);
        Arena copy_reference_dst(copy_count * sizeof(MvCell) + 32);
        Arena copy_vectorized_dst(copy_count * sizeof(MvCell) + 32);
        if (copy_count != 0) {
            std::memcpy(
                    copy_reference_src.cells(src_offset), source.data(),
                    copy_count * sizeof(MvCell));
            std::memcpy(
                    copy_vectorized_src.cells(src_offset), source.data(),
                    copy_count * sizeof(MvCell));
        }
        simd::scalar::copyCells(
                copy_reference_dst.cells(dst_offset),
                copy_reference_src.cells(src_offset), copy_count);
        copy_cells(
                copy_vectorized_dst.cells(dst_offset),
                copy_vectorized_src.cells(src_offset), copy_count);
        CHECK(sameRegion(
                copy_reference_dst.cells(dst_offset),
                copy_vectorized_dst.cells(dst_offset), copy_count));
        CHECK(guardIntact(
                copy_vectorized_dst, dst_offset,
                dst_offset + copy_count * sizeof(MvCell)));
    }

    std::cout << "[kernel] " << label << " matches scalar reference\n";
}

void test_compiled_kernels() {
#if defined(BS_AVC_CELL_SIMD_SSE2)
    test_kernel_set(
            "sse2", &simd::detail::fillCellsSse2, &simd::detail::fillRowsSse2,
            &simd::detail::copyCellsSse2);
#endif
#if defined(BS_AVC_CELL_SIMD_HAVE_AVX2)
    if (simd::detail::activeBackend() == simd::detail::Backend::Avx2) {
        test_kernel_set(
                "avx2", &simd::detail::fillCellsAvx2, &simd::detail::fillRowsAvx2,
                &simd::detail::copyCellsAvx2);
    } else {
        std::cout << "[kernel] avx2 compiled but not supported by this CPU; skipped\n";
    }
#endif
#if defined(BS_AVC_CELL_SIMD_NEON)
    test_kernel_set(
            "neon", &simd::detail::fillCellsNeon, &simd::detail::fillRowsNeon,
            &simd::detail::copyCellsNeon);
#endif
#if !defined(BS_AVC_CELL_SIMD_SSE2) && !defined(BS_AVC_CELL_SIMD_HAVE_AVX2) && \
        !defined(BS_AVC_CELL_SIMD_NEON)
    std::cout << "[kernel] scalar reference only on this target\n";
#endif
}

/* -------------------------------------------------------------------------
 * Microbenchmark
 * ---------------------------------------------------------------------- */

volatile std::uint64_t g_bench_sink = 0;

double bench_seconds(
        const std::string& label,
        std::size_t bytes_per_iteration,
        std::uint32_t iterations,
        const std::function<void()>& body) {
    body();  // warm-up / page-in
    double best = std::numeric_limits<double>::max();
    for (int repetition = 0; repetition < 3; ++repetition) {
        const auto start = std::chrono::steady_clock::now();
        for (std::uint32_t i = 0; i < iterations; ++i) {
            body();
        }
        const auto end = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double>(end - start).count());
    }
    const double gigabytes =
            static_cast<double>(bytes_per_iteration) * iterations / 1.0e9;
    const double gbps = best > 0.0 ? gigabytes / best : 0.0;
    std::cout << "[bench] " << label << " best-of-3, " << iterations << " x "
              << (bytes_per_iteration / 1024) << " KiB: " << best << " s, "
              << gbps << " GB/s\n";
    return gbps;
}

void benchmark() {
    /*
     * 1920x1088 coded grid: 1080 visible lines are padded to 68 macroblock
     * rows, which is the array the parser actually owns (2,088,960 bytes).
     */
    constexpr std::uint32_t kWidth4x4 = 480;   // 1920 luma pixels
    constexpr std::uint32_t kHeight4x4 = 272;  // 1088 coded luma pixels
    constexpr std::size_t kCells =
            static_cast<std::size_t>(kWidth4x4) * kHeight4x4;
    constexpr std::size_t kBytes = kCells * sizeof(MvCell);

    std::vector<MvCell> grid(kCells);
    std::vector<MvCell> source(kCells);
    Rng rng{0x6666666666666666ull};
    const MvCell pattern = randomCell(rng);
    for (MvCell& cell : source) {
        cell = randomCell(rng);
    }

    const std::uint32_t iterations = 400;
    std::cout << "[bench] backend=" << simd::backendName() << " grid=" << kWidth4x4 << "x"
              << kHeight4x4 << " cells=" << kCells << " bytes=" << kBytes << "\n";

    bench_seconds("fillCells", kBytes, iterations, [&]() {
        simd::fillCells(grid.data(), kCells, pattern);
        g_bench_sink += static_cast<std::uint64_t>(grid[kCells / 2].flags);
    });

    bench_seconds("copyCells", kBytes * 2, iterations, [&]() {
        simd::copyCells(grid.data(), source.data(), kCells);
        g_bench_sink += static_cast<std::uint64_t>(grid[kCells / 2].flags);
    });

    bench_seconds("fillBlock 16x16 MBs", kBytes, iterations, [&]() {
        for (std::uint32_t y = 0; y < kHeight4x4; y += 4) {
            for (std::uint32_t x = 0; x < kWidth4x4; x += 4) {
                simd::fillBlock(
                        grid.data(), kWidth4x4, x, y, 4, 4, pattern);
            }
        }
        g_bench_sink += static_cast<std::uint64_t>(grid[kCells / 2].flags);
    });

    /* 4x4 partition (one cell) and 8x8 partition, the other common shapes. */
    bench_seconds("fillBlock 4x4 partitions", kBytes, iterations, [&]() {
        for (std::uint32_t y = 0; y < kHeight4x4; ++y) {
            for (std::uint32_t x = 0; x < kWidth4x4; ++x) {
                simd::fillBlock(grid.data(), kWidth4x4, x, y, 1, 1, pattern);
            }
        }
        g_bench_sink += static_cast<std::uint64_t>(grid[kCells / 2].flags);
    });
}

}  // namespace

int main() {
    std::cout << "avc cell SIMD kernels: backend=" << simd::backendName()
              << " sizeof(MvCell)=" << sizeof(MvCell)
              << " alignof(MvCell)=" << alignof(MvCell) << "\n";

    test_patterns();
    test_fill_cells();
    test_copy_cells();
    test_fill_block();
    test_scale_direct_cell();
    test_scale_direct_block();
    test_compiled_kernels();
    benchmark();

    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "all avc cell SIMD tests passed\n";
    return 0;
}
