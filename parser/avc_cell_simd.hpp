// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/*
 * ===========================================================================
 * Portable SIMD kernels for the AVC 4x4 motion-cell grid
 * ===========================================================================
 *
 * The AVC motion-vector extractor writes one 16-byte MvCell per 4x4 luma
 * block.  Most of that work is unstructured: a macroblock is split into one
 * to sixteen rectangles, each of which receives the same motion vector,
 * reference indices and flags.  These kernels provide the bulk store/copy
 * primitives for that pattern, plus a batched fill for rectangular
 * partitions, so the parser does not need a scalar loop per cell.
 *
 * Backends
 * --------
 *   x86:  SSE2 is the baseline (x86-64, or 32-bit with __SSE2__).  When the
 *         compiler is GCC/Clang the header additionally compiles an AVX2
 *         kernel under __attribute__((target("avx2"))) and selects it once
 *         at run time with __builtin_cpu_supports("avx2").  The header never
 *         requires -march=native and never lets the baseline path contain
 *         AVX instructions.  MSVC uses the compile-time instruction set:
 *         /arch:AVX2 selects AVX2, otherwise SSE2.
 *         ARM: NEON when __ARM_NEON / __aarch64__ / _M_ARM64 is available.
 *   All other targets, and any gap between the SIMD loops, use the scalar
 *   reference in bs::avc::simd::scalar.
 *
 * Defining BS_AVC_CELL_SIMD_DISABLE_AVX2 before including this header
 * suppresses both the compile-time AVX2 kernel and the runtime dispatch.  It
 * exists so a build (or a test) can pin the baseline SSE2/NEON/scalar kernel.
 *
 * Layout
 * ------
 * MvCell is 16 bytes and trivially copyable.  A grid row is a run of MvCell
 * whose first byte is at a 16-byte multiple *relative to the grid base*, but
 * the base itself is only guaranteed to be suitably aligned for MvCell
 * (alignof(MvCell) == 2, the buffer helpers request 64).  Every load and
 * store here is therefore unaligned; no kernel requires 16-byte alignment.
 *
 * Coordinate model
 * ----------------
 * `stride4x4` is the grid width in 4x4 cells, `x`/`y` are cell coordinates
 * of the rectangle origin and `w`/`h` its extent in cells.  A 16x16
 * macroblock is (x, y, 4, 4); 16x8 is (4, 2); 8x16 is (2, 4); 8x8 is (2, 2);
 * 8x4 is (2, 1); 4x8 is (1, 2); 4x4 is (1, 1).  The kernels do not know the
 * grid bounds: the caller guarantees that every addressed cell exists.
 *
 * Threading
 * ---------
 * All functions are reentrant and touch only the supplied range.  The
 * one-time backend probe uses a function-local static, which C++11
 * guarantees is initialised once even under concurrent first use.
 */

#include "../syntax/avc_motion_vector.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

/* -------------------------------------------------------------------------
 * Backend detection
 * ---------------------------------------------------------------------- */

#if defined(__SSE2__) || defined(_M_X64) || \
        (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define BS_AVC_CELL_SIMD_SSE2 1
#endif

#if defined(__AVX2__)
#define BS_AVC_CELL_SIMD_AVX2 1
#endif

#if defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
#define BS_AVC_CELL_SIMD_NEON 1
#endif

#if defined(BS_AVC_CELL_SIMD_SSE2)
#include <emmintrin.h>
#endif

/*
 * AVX2 is available in one of two ways:
 *   - the translation unit was compiled for AVX2 (__AVX2__), so the kernel is
 *     an ordinary inline function;
 *   - the compiler supports per-function target attributes, so the kernel is
 *     compiled for AVX2 while the rest of the header stays at the baseline.
 */
#if !defined(BS_AVC_CELL_SIMD_DISABLE_AVX2)
#if defined(BS_AVC_CELL_SIMD_AVX2)
#define BS_AVC_CELL_SIMD_HAVE_AVX2 1
#elif (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER) && \
        (defined(__x86_64__) || defined(__i386__)) && \
        defined(BS_AVC_CELL_SIMD_SSE2)
#define BS_AVC_CELL_SIMD_HAVE_AVX2 1
#define BS_AVC_CELL_SIMD_HAVE_AVX2_DISPATCH 1
#define BS_AVC_CELL_SIMD_AVX2_TARGET __attribute__((target("avx2")))
#endif
#endif

#if defined(BS_AVC_CELL_SIMD_HAVE_AVX2)
#include <immintrin.h>
#endif

#if defined(BS_AVC_CELL_SIMD_NEON)
#include <arm_neon.h>
#endif

#if !defined(BS_AVC_CELL_SIMD_AVX2_TARGET)
#define BS_AVC_CELL_SIMD_AVX2_TARGET
#endif

namespace bs {
namespace avc {
namespace simd {

static_assert(sizeof(MvCell) == 16, "SIMD kernels assume a 16-byte MvCell");

/* -------------------------------------------------------------------------
 * Cell patterns
 * ---------------------------------------------------------------------- */

/*
 * A CellPattern is the 16-byte value written into one or more cells.  The
 * factories mirror the assignments the parser performs for the corresponding
 * syntax element, including the flags that describe which lists are present.
 * `reserved` is always zero so a filled grid is byte-for-byte reproducible.
 */
struct CellPattern {
    MvCell cell{};

    /* Intra macroblock: zero motion, ref_idx = -1, flags = MvIntra. */
    [[nodiscard]] static CellPattern intra(std::uint8_t extra_flags = 0) noexcept {
        CellPattern pattern;
        pattern.cell.ref_idx[0] = -1;
        pattern.cell.ref_idx[1] = -1;
        pattern.cell.flags = static_cast<std::uint8_t>(MvIntra | extra_flags);
        return pattern;
    }

    /* Not decoded (yet): zero motion, ref_idx = -1, flags = MvUnavailable. */
    [[nodiscard]] static CellPattern unavailable(std::uint8_t extra_flags = 0) noexcept {
        CellPattern pattern;
        pattern.cell.ref_idx[0] = -1;
        pattern.cell.ref_idx[1] = -1;
        pattern.cell.flags = static_cast<std::uint8_t>(MvUnavailable | extra_flags);
        return pattern;
    }

    /*
     * P-slice skipped macroblock (H.264 8.4.1.1) for one MV predictor:
     * list 0 only, ref_idx 0 or -1 when list 0 has no reference.  Matches
     * SliceDataParser::apply_skip_mb.
     */
    [[nodiscard]] static CellPattern skip(
            std::int16_t mv_x,
            std::int16_t mv_y,
            std::int8_t ref0,
            std::uint8_t extra_flags = 0) noexcept {
        CellPattern pattern;
        pattern.cell.mv[0][0] = mv_x;
        pattern.cell.mv[0][1] = mv_y;
        pattern.cell.ref_idx[0] = ref0;
        pattern.cell.ref_idx[1] = -1;
        pattern.cell.flags = static_cast<std::uint8_t>(
                MvSkip | (ref0 >= 0 ? MvList0 : MvUnavailable) | extra_flags);
        return pattern;
    }

    /*
     * One list of a P/B partition.  `list` must be 0 or 1.  The other list
     * keeps ref_idx = -1, which is the state of a freshly decoded cell; a
     * bi-predicted partition must use biPartition() instead.
     */
    [[nodiscard]] static CellPattern listPartition(
            int list,
            std::int16_t mv_x,
            std::int16_t mv_y,
            std::int8_t ref,
            std::uint8_t extra_flags = 0) noexcept {
        CellPattern pattern;
        const std::size_t index = list == 1 ? 1u : 0u;
        pattern.cell.mv[index][0] = mv_x;
        pattern.cell.mv[index][1] = mv_y;
        pattern.cell.ref_idx[0] = -1;
        pattern.cell.ref_idx[1] = -1;
        pattern.cell.ref_idx[index] = ref;
        pattern.cell.flags = static_cast<std::uint8_t>(
                (index == 0 ? MvList0 : MvList1) | extra_flags);
        return pattern;
    }

    [[nodiscard]] static CellPattern l0Partition(
            std::int16_t mv_x,
            std::int16_t mv_y,
            std::int8_t ref0,
            std::uint8_t extra_flags = 0) noexcept {
        return listPartition(0, mv_x, mv_y, ref0, extra_flags);
    }

    [[nodiscard]] static CellPattern l1Partition(
            std::int16_t mv_x,
            std::int16_t mv_y,
            std::int8_t ref1,
            std::uint8_t extra_flags = 0) noexcept {
        return listPartition(1, mv_x, mv_y, ref1, extra_flags);
    }

    /* B-slice bi-predicted partition: both lists carry a vector and a ref. */
    [[nodiscard]] static CellPattern biPartition(
            std::int16_t mv0_x,
            std::int16_t mv0_y,
            std::int8_t ref0,
            std::int16_t mv1_x,
            std::int16_t mv1_y,
            std::int8_t ref1,
            std::uint8_t extra_flags = 0) noexcept {
        CellPattern pattern;
        pattern.cell.mv[0][0] = mv0_x;
        pattern.cell.mv[0][1] = mv0_y;
        pattern.cell.mv[1][0] = mv1_x;
        pattern.cell.mv[1][1] = mv1_y;
        pattern.cell.ref_idx[0] = ref0;
        pattern.cell.ref_idx[1] = ref1;
        pattern.cell.flags = static_cast<std::uint8_t>(
                MvList0 | MvList1 | extra_flags);
        return pattern;
    }
};

/* -------------------------------------------------------------------------
 * Temporal-direct scaling
 * ---------------------------------------------------------------------- */

/*
 * Inputs for the uniform temporal-direct case of H.264 8.4.1.2.3.  The POC
 * values are the ones SliceDataParser::apply_direct() uses:
 *
 *   col_poc   picture order count of the co-located reference picture (pic0)
 *   ref1_poc  POC of RefPicList1[0] (pic1)
 *   ref0      list 0 reference index selected by mapColPocToList0()
 *   ref1      list 1 reference index, or -1 when list 1 is unusable
 *
 * `current_field_mb`, `col_field_pic` and `mbaff_frame` select the field-line
 * normalisation and the MvField flag exactly as the parser does.
 */
struct DirectScaleParams {
    std::int32_t current_poc = 0;
    std::int32_t col_poc = 0;
    std::int32_t ref1_poc = 0;
    std::int8_t ref0 = 0;
    std::int8_t ref1 = -1;
    bool current_field_mb = false;
    bool col_field_pic = false;
    bool mbaff_frame = false;
};

namespace detail {

[[nodiscard]] inline std::int16_t clampMv(std::int64_t value) noexcept {
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(value, -32768, 32767));
}

[[nodiscard]] inline std::int16_t storeMvY(
        std::int16_t value,
        bool field_mb,
        bool mbaff_frame) noexcept {
    if (!mbaff_frame || !field_mb) {
        return value;
    }
    const int scaled = static_cast<int>(value) * 2;
    return static_cast<std::int16_t>(std::clamp(scaled, -32768, 32767));
}

}  // namespace detail

/*
 * Scalar reference for one temporal-direct cell.  This is a transcription of
 * the formula in SliceDataParser::apply_direct(): int64 tb/td, the 16384
 * reciprocal, the +32 rounding, the +128 rounding of the scaled vector, the
 * int16 saturation and the MBAFF frame-line doubling.  No SIMD version of
 * this arithmetic is provided because the rounding/shift sequence is not
 * profitable to vectorise and must stay bit-identical to the scalar path.
 */
[[nodiscard]] inline MvCell scaleDirectCell(
        const MvCell& co_located,
        int col_list,
        const DirectScaleParams& params) noexcept {
    const std::size_t list = col_list == 1 ? 1u : 0u;
    std::int64_t mvx = co_located.mv[list][0];
    std::int64_t mvy = co_located.mv[list][1];
    if (params.current_field_mb && !params.col_field_pic) {
        mvy /= 2;
    } else if (!params.current_field_mb && params.col_field_pic) {
        mvy *= 2;
    }

    const std::int64_t td = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(params.ref1_poc) - params.col_poc, -128, 127);
    const std::int64_t tb = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(params.current_poc) - params.col_poc, -128, 127);

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

    MvCell out{};
    out.mv[0][0] = detail::clampMv(mv0x);
    out.mv[0][1] = detail::storeMvY(
            detail::clampMv(mv0y), params.current_field_mb, params.mbaff_frame);
    out.mv[1][0] = detail::clampMv(mv1x);
    out.mv[1][1] = detail::storeMvY(
            detail::clampMv(mv1y), params.current_field_mb, params.mbaff_frame);
    out.ref_idx[0] = params.ref0;
    out.ref_idx[1] = params.ref1;
    out.flags = static_cast<std::uint8_t>(
            MvDirect | MvList0 | (params.ref1 >= 0 ? MvList1 : 0) |
            (params.current_field_mb ? MvField : 0));
    return out;
}

/* Convenience overload using apply_direct()'s co-located list choice. */
[[nodiscard]] inline MvCell scaleDirectCell(
        const MvCell& co_located,
        const DirectScaleParams& params) noexcept {
    return scaleDirectCell(co_located, co_located.ref_idx[0] >= 0 ? 0 : 1, params);
}

/* -------------------------------------------------------------------------
 * Scalar reference implementation
 * ---------------------------------------------------------------------- */

namespace scalar {

[[nodiscard]] inline MvCell scaleDirectCell(
        const MvCell& co_located,
        int col_list,
        const DirectScaleParams& params) noexcept {
    return bs::avc::simd::scaleDirectCell(co_located, col_list, params);
}

[[nodiscard]] inline MvCell scaleDirectCell(
        const MvCell& co_located,
        const DirectScaleParams& params) noexcept {
    return bs::avc::simd::scaleDirectCell(co_located, params);
}

inline void fillCells(MvCell* dst, std::size_t count, const MvCell& pattern) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        dst[i] = pattern;
    }
}

inline void fillRows(
        MvCell* base,
        std::uint32_t stride4x4,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t w,
        std::uint32_t h,
        const MvCell& pattern) noexcept {
    if (w == 0 || h == 0) {
        return;
    }
    for (std::uint32_t row = 0; row < h; ++row) {
        fillCells(
                base + static_cast<std::size_t>(y + row) * stride4x4 + x, w, pattern);
    }
}

inline void fillBlock(
        MvCell* base,
        std::uint32_t stride4x4,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t w,
        std::uint32_t h,
        const MvCell& pattern) noexcept {
    fillRows(base, stride4x4, x, y, w, h, pattern);
}

inline void copyCells(MvCell* dst, const MvCell* src, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        dst[i] = src[i];
    }
}

inline void scaleDirectBlock(
        MvCell* base,
        std::uint32_t stride4x4,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t w,
        std::uint32_t h,
        const MvCell& co_located,
        int col_list,
        const DirectScaleParams& params) noexcept {
    const MvCell pattern =
            bs::avc::simd::scaleDirectCell(co_located, col_list, params);
    fillBlock(base, stride4x4, x, y, w, h, pattern);
}

inline void scaleDirectBlock(
        MvCell* base,
        std::uint32_t stride4x4,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t w,
        std::uint32_t h,
        const MvCell& co_located,
        const DirectScaleParams& params) noexcept {
    const MvCell pattern = bs::avc::simd::scaleDirectCell(co_located, params);
    fillBlock(base, stride4x4, x, y, w, h, pattern);
}

}  // namespace scalar

/* -------------------------------------------------------------------------
 * Backend probe
 * ---------------------------------------------------------------------- */

namespace detail {

enum class Backend : std::uint8_t {
    Scalar = 0,
    Sse2,
    Avx2,
    Neon,
};

[[nodiscard]] inline Backend detectBackend() noexcept {
#if defined(BS_AVC_CELL_SIMD_HAVE_AVX2_DISPATCH)
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") != 0 ? Backend::Avx2 : Backend::Sse2;
#elif defined(BS_AVC_CELL_SIMD_HAVE_AVX2)
    return Backend::Avx2;
#elif defined(BS_AVC_CELL_SIMD_NEON)
    return Backend::Neon;
#elif defined(BS_AVC_CELL_SIMD_SSE2)
    return Backend::Sse2;
#else
    return Backend::Scalar;
#endif
}

[[nodiscard]] inline Backend activeBackend() noexcept {
    static const Backend backend = detectBackend();
    return backend;
}

}  // namespace detail

[[nodiscard]] inline const char* backendName() noexcept {
    switch (detail::activeBackend()) {
        case detail::Backend::Avx2:
            return "avx2";
        case detail::Backend::Sse2:
            return "sse2";
        case detail::Backend::Neon:
            return "neon";
        case detail::Backend::Scalar:
            break;
    }
    return "scalar";
}

/* -------------------------------------------------------------------------
 * SIMD kernels
 * ---------------------------------------------------------------------- */

namespace detail {

#if defined(BS_AVC_CELL_SIMD_SSE2)
inline void fillCellsSse2(MvCell* dst, std::size_t count, const MvCell& pattern) noexcept {
    const __m128i value =
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(&pattern));
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 0), value);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 1), value);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 2), value);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 3), value);
    }
    for (; i < count; ++i) {
        dst[i] = pattern;
    }
}

inline void fillRowsSse2(
        MvCell* base,
        std::uint32_t stride4x4,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t w,
        std::uint32_t h,
        const MvCell& pattern) noexcept {
    if (w == 0 || h == 0) {
        return;
    }
    MvCell* const first = base + static_cast<std::size_t>(y) * stride4x4 + x;
    if (x == 0 && w == stride4x4) {
        fillCellsSse2(first, static_cast<std::size_t>(w) * h, pattern);
        return;
    }
    for (std::uint32_t row = 0; row < h; ++row) {
        fillCellsSse2(first + static_cast<std::size_t>(row) * stride4x4, w, pattern);
    }
}

inline void copyCellsSse2(MvCell* dst, const MvCell* src, std::size_t count) noexcept {
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 0));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 1));
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 2));
        const __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 3));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 0), a);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 1), b);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 2), c);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 3), d);
    }
    for (; i < count; ++i) {
        dst[i] = src[i];
    }
}
#endif  // BS_AVC_CELL_SIMD_SSE2

#if defined(BS_AVC_CELL_SIMD_HAVE_AVX2)
BS_AVC_CELL_SIMD_AVX2_TARGET
inline void fillCellsAvx2(MvCell* dst, std::size_t count, const MvCell& pattern) noexcept {
    const __m128i half =
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(&pattern));
    const __m256i value = _mm256_set_m128i(half, half);
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i + 0), value);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i + 2), value);
    }
    if (i + 2 <= count) {
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), value);
        i += 2;
    }
    for (; i < count; ++i) {
        dst[i] = pattern;
    }
}

BS_AVC_CELL_SIMD_AVX2_TARGET
inline void fillRowsAvx2(
        MvCell* base,
        std::uint32_t stride4x4,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t w,
        std::uint32_t h,
        const MvCell& pattern) noexcept {
    if (w == 0 || h == 0) {
        return;
    }
    MvCell* const first = base + static_cast<std::size_t>(y) * stride4x4 + x;
    if (x == 0 && w == stride4x4) {
        fillCellsAvx2(first, static_cast<std::size_t>(w) * h, pattern);
        return;
    }
    for (std::uint32_t row = 0; row < h; ++row) {
        fillCellsAvx2(first + static_cast<std::size_t>(row) * stride4x4, w, pattern);
    }
}

BS_AVC_CELL_SIMD_AVX2_TARGET
inline void copyCellsAvx2(MvCell* dst, const MvCell* src, std::size_t count) noexcept {
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const __m256i a =
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i + 0));
        const __m256i b =
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i + 2));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i + 0), a);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i + 2), b);
    }
    if (i + 2 <= count) {
        const __m256i a =
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), a);
        i += 2;
    }
    for (; i < count; ++i) {
        dst[i] = src[i];
    }
}
#endif  // BS_AVC_CELL_SIMD_HAVE_AVX2

#if defined(BS_AVC_CELL_SIMD_NEON)
inline void fillCellsNeon(MvCell* dst, std::size_t count, const MvCell& pattern) noexcept {
    const uint8x16_t value =
            vld1q_u8(reinterpret_cast<const std::uint8_t*>(&pattern));
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        vst1q_u8(reinterpret_cast<std::uint8_t*>(dst + i + 0), value);
        vst1q_u8(reinterpret_cast<std::uint8_t*>(dst + i + 1), value);
        vst1q_u8(reinterpret_cast<std::uint8_t*>(dst + i + 2), value);
        vst1q_u8(reinterpret_cast<std::uint8_t*>(dst + i + 3), value);
    }
    for (; i < count; ++i) {
        dst[i] = pattern;
    }
}

inline void fillRowsNeon(
        MvCell* base,
        std::uint32_t stride4x4,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t w,
        std::uint32_t h,
        const MvCell& pattern) noexcept {
    if (w == 0 || h == 0) {
        return;
    }
    MvCell* const first = base + static_cast<std::size_t>(y) * stride4x4 + x;
    if (x == 0 && w == stride4x4) {
        fillCellsNeon(first, static_cast<std::size_t>(w) * h, pattern);
        return;
    }
    for (std::uint32_t row = 0; row < h; ++row) {
        fillCellsNeon(first + static_cast<std::size_t>(row) * stride4x4, w, pattern);
    }
}

inline void copyCellsNeon(MvCell* dst, const MvCell* src, std::size_t count) noexcept {
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const uint8x16_t a =
                vld1q_u8(reinterpret_cast<const std::uint8_t*>(src + i + 0));
        const uint8x16_t b =
                vld1q_u8(reinterpret_cast<const std::uint8_t*>(src + i + 1));
        const uint8x16_t c =
                vld1q_u8(reinterpret_cast<const std::uint8_t*>(src + i + 2));
        const uint8x16_t d =
                vld1q_u8(reinterpret_cast<const std::uint8_t*>(src + i + 3));
        vst1q_u8(reinterpret_cast<std::uint8_t*>(dst + i + 0), a);
        vst1q_u8(reinterpret_cast<std::uint8_t*>(dst + i + 1), b);
        vst1q_u8(reinterpret_cast<std::uint8_t*>(dst + i + 2), c);
        vst1q_u8(reinterpret_cast<std::uint8_t*>(dst + i + 3), d);
    }
    for (; i < count; ++i) {
        dst[i] = src[i];
    }
}
#endif  // BS_AVC_CELL_SIMD_NEON

}  // namespace detail

/* -------------------------------------------------------------------------
 * Public kernels
 * ---------------------------------------------------------------------- */

/* Fill `count` contiguous cells.  `dst` may be unaligned. */
inline void fillCells(MvCell* dst, std::size_t count, const MvCell& pattern) noexcept {
    if (count == 0) {
        return;
    }
    /* One 16-byte cell is cheaper as a direct store than as a dispatched call. */
    if (count == 1) {
        dst[0] = pattern;
        return;
    }
#if defined(BS_AVC_CELL_SIMD_HAVE_AVX2)
    if (detail::activeBackend() == detail::Backend::Avx2) {
        detail::fillCellsAvx2(dst, count, pattern);
        return;
    }
#endif
#if defined(BS_AVC_CELL_SIMD_SSE2)
    if (detail::activeBackend() == detail::Backend::Sse2) {
        detail::fillCellsSse2(dst, count, pattern);
        return;
    }
#endif
#if defined(BS_AVC_CELL_SIMD_NEON)
    if (detail::activeBackend() == detail::Backend::Neon) {
        detail::fillCellsNeon(dst, count, pattern);
        return;
    }
#endif
    scalar::fillCells(dst, count, pattern);
}

/*
 * Fill h rows of w cells starting at (x, y).  The common full-row case
 * (x == 0 && w == stride4x4) is collapsed into one contiguous run.
 */
inline void fillRows(
        MvCell* base,
        std::uint32_t stride4x4,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t w,
        std::uint32_t h,
        const MvCell& pattern) noexcept {
    if (w == 0 || h == 0) {
        return;
    }
    /*
     * A one-cell-wide partition (4x4, 4x8 and 8x4 blocks) is a column of
     * plain 16-byte stores; going through the SIMD dispatcher costs more than
     * the stores themselves.
     */
    if (w == 1) {
        MvCell* dst = base + static_cast<std::size_t>(y) * stride4x4 + x;
        for (std::uint32_t row = 0; row < h; ++row) {
            *dst = pattern;
            dst += stride4x4;
        }
        return;
    }
#if defined(BS_AVC_CELL_SIMD_HAVE_AVX2)
    if (detail::activeBackend() == detail::Backend::Avx2) {
        detail::fillRowsAvx2(base, stride4x4, x, y, w, h, pattern);
        return;
    }
#endif
#if defined(BS_AVC_CELL_SIMD_SSE2)
    if (detail::activeBackend() == detail::Backend::Sse2) {
        detail::fillRowsSse2(base, stride4x4, x, y, w, h, pattern);
        return;
    }
#endif
#if defined(BS_AVC_CELL_SIMD_NEON)
    if (detail::activeBackend() == detail::Backend::Neon) {
        detail::fillRowsNeon(base, stride4x4, x, y, w, h, pattern);
        return;
    }
#endif
    scalar::fillRows(base, stride4x4, x, y, w, h, pattern);
}

/* Rectangular 4x4-cell partition fill; alias of fillRows. */
inline void fillBlock(
        MvCell* base,
        std::uint32_t stride4x4,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t w,
        std::uint32_t h,
        const MvCell& pattern) noexcept {
    fillRows(base, stride4x4, x, y, w, h, pattern);
}

/* Copy `count` contiguous cells.  Source and destination must not overlap. */
inline void copyCells(MvCell* dst, const MvCell* src, std::size_t count) noexcept {
    if (count == 0) {
        return;
    }
#if defined(BS_AVC_CELL_SIMD_HAVE_AVX2)
    if (detail::activeBackend() == detail::Backend::Avx2) {
        detail::copyCellsAvx2(dst, src, count);
        return;
    }
#endif
#if defined(BS_AVC_CELL_SIMD_SSE2)
    if (detail::activeBackend() == detail::Backend::Sse2) {
        detail::copyCellsSse2(dst, src, count);
        return;
    }
#endif
#if defined(BS_AVC_CELL_SIMD_NEON)
    if (detail::activeBackend() == detail::Backend::Neon) {
        detail::copyCellsNeon(dst, src, count);
        return;
    }
#endif
    scalar::copyCells(dst, src, count);
}

/*
 * Uniform temporal-direct fill.  Use this only when every cell of the
 * rectangle samples the same co-located cell and maps to the same ref0/ref1;
 * in apply_direct() that is one 8x8 quadrant (w <= 2 && h <= 2), because the
 * co-located sample point (cx, cy) changes per quadrant.  The arithmetic is
 * scalar and bit-identical to apply_direct(); the rectangle store is the
 * vectorised fillBlock().
 */
inline void scaleDirectBlock(
        MvCell* base,
        std::uint32_t stride4x4,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t w,
        std::uint32_t h,
        const MvCell& co_located,
        int col_list,
        const DirectScaleParams& params) noexcept {
    fillBlock(
            base, stride4x4, x, y, w, h,
            scaleDirectCell(co_located, col_list, params));
}

inline void scaleDirectBlock(
        MvCell* base,
        std::uint32_t stride4x4,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t w,
        std::uint32_t h,
        const MvCell& co_located,
        const DirectScaleParams& params) noexcept {
    fillBlock(
            base, stride4x4, x, y, w, h,
            scaleDirectCell(co_located, params));
}

}  // namespace simd
}  // namespace avc
}  // namespace bs
