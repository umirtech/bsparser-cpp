// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#include "utils/MotionBufferPool.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

}  // namespace

int main() {
    bs::utils::MotionBufferPool pool({2, 4, 4u << 20, 64});

    bs::avc::MotionVectorBufferRequest request{};
    request.width4x4 = 8;
    request.height4x4 = 8;
    request.cell_count = 64;
    request.cell_stride = sizeof(bs::avc::MvCell);
    request.required_bytes = sizeof(bs::avc::MotionVectorBufferHeader) + 64 * sizeof(bs::avc::MvCell);
    request.alignment = 64;
    request.poc = 4;
    request.frame_num = 2;

    auto buffer = pool.acquire(request);
    expect(buffer.data != nullptr, "buffer acquired");
    expect(buffer.capacity >= request.required_bytes, "buffer capacity is large enough");
    expect(static_cast<bool>(buffer.lease), "buffer carries a lease");
    expect(reinterpret_cast<std::uintptr_t>(buffer.data) % 64 == 0, "buffer is 64-byte aligned");

    auto* header = static_cast<bs::avc::MotionVectorBufferHeader*>(buffer.data);
    *header = bs::avc::MotionVectorBufferHeader{};
    header->magic = bs::avc::kMotionVectorBufferMagic;
    header->version = bs::avc::kMotionVectorBufferVersion;
    header->header_bytes = sizeof(*header);
    header->total_bytes = static_cast<std::uint32_t>(request.required_bytes);
    header->cell_count = request.cell_count;
    header->cell_stride = sizeof(bs::avc::MvCell);
    header->cells_offset = sizeof(*header);
    header->width4x4 = request.width4x4;
    header->height4x4 = request.height4x4;
    header->poc = request.poc;

    bs::avc::MotionVectorBufferView view{};
    view.header = header;
    view.data = buffer.data;
    view.bytes = request.required_bytes;
    view.lease = buffer.lease;

    const auto cells = bs::avc::motionCells(view);
    expect(cells.size() == request.cell_count, "cell view has the requested count");
    expect(reinterpret_cast<const void*>(cells.data()) ==
                   static_cast<const void*>(static_cast<const std::uint8_t*>(buffer.data) +
                                            header->cells_offset),
           "cell view points into the consumer buffer");

    auto copy = buffer.lease;
    auto stats = pool.stats();
    expect(stats.buffersInUse == 1, "one buffer is in use");
    expect(stats.bytesInUse >= request.required_bytes, "bytes are accounted");

    buffer.lease.reset();
    stats = pool.stats();
    expect(stats.buffersInUse == 1, "consumer copy keeps the buffer alive");

    copy.reset();
    view.lease.reset();
    stats = pool.stats();
    expect(stats.buffersInUse == 0, "last lease releases the buffer");

    auto again = pool.acquire(request);
    expect(again.data != nullptr, "buffer is reused");
    again.lease.reset();
    expect(pool.stats().buffersInUse == 0, "reused buffer is released");

    /*
     * Version-2 per-slice table: the same ref_idx resolves to a different
     * POC in each slice of a multi-slice picture.
     */
    {
        const std::size_t table_offset = sizeof(bs::avc::MotionVectorBufferHeader);
        const std::size_t cells_offset =
                (table_offset + 2 * sizeof(bs::avc::MotionSliceReference) + 63u) & ~std::size_t{63u};
        bs::avc::MotionVectorBufferRequest slice_request{};
        slice_request.width4x4 = 8;
        slice_request.height4x4 = 8;
        slice_request.cell_count = 64;
        slice_request.cell_stride = sizeof(bs::avc::MvCell);
        slice_request.required_bytes = cells_offset + 64 * sizeof(bs::avc::MvCell);
        slice_request.alignment = 64;

        auto slice_buffer = pool.acquire(slice_request);
        expect(slice_buffer.data != nullptr, "slice-table buffer acquired");
        if (slice_buffer.data != nullptr) {
            auto* slice_header =
                    static_cast<bs::avc::MotionVectorBufferHeader*>(slice_buffer.data);
            *slice_header = bs::avc::MotionVectorBufferHeader{};
            slice_header->magic = bs::avc::kMotionVectorBufferMagic;
            slice_header->version = bs::avc::kMotionVectorBufferVersion;
            slice_header->header_bytes = sizeof(*slice_header);
            slice_header->total_bytes = static_cast<std::uint32_t>(slice_request.required_bytes);
            slice_header->poc = 8;
            slice_header->mb_width = 2;
            slice_header->mb_height = 2;
            slice_header->width4x4 = 8;
            slice_header->height4x4 = 8;
            slice_header->cell_count = 64;
            slice_header->cell_stride = sizeof(bs::avc::MvCell);
            slice_header->cells_offset = static_cast<std::uint32_t>(cells_offset);
            slice_header->slice_ref_count = 2;
            slice_header->slice_ref_capacity = 2;
            slice_header->slice_ref_stride = sizeof(bs::avc::MotionSliceReference);
            slice_header->slice_refs_offset = static_cast<std::uint32_t>(table_offset);

            auto* slices = reinterpret_cast<bs::avc::MotionSliceReference*>(
                    static_cast<std::uint8_t*>(slice_buffer.data) + table_offset);
            slices[0].mb_addr_begin = 0;
            slices[0].mb_addr_end = 2;
            slices[0].num_ref_idx[0] = 2;
            slices[0].ref_poc[0][0] = 4;
            slices[0].ref_poc[0][1] = 6;
            slices[1].mb_addr_begin = 2;
            slices[1].mb_addr_end = 4;
            slices[1].num_ref_idx[0] = 1;
            slices[1].ref_poc[0][0] = 6;

            auto* cells = reinterpret_cast<bs::avc::MvCell*>(
                    static_cast<std::uint8_t*>(slice_buffer.data) + cells_offset);
            for (int i = 0; i < 64; ++i) {
                cells[i] = bs::avc::MvCell{};
                cells[i].flags = bs::avc::MvUnavailable;
            }
            cells[0].ref_idx[0] = 0;
            cells[0].flags = bs::avc::MvList0;
            cells[4 * 8].ref_idx[0] = 0;
            cells[4 * 8].flags = bs::avc::MvList0;

            bs::avc::MotionVectorBufferView view{};
            view.header = slice_header;
            view.data = slice_buffer.data;
            view.bytes = slice_request.required_bytes;
            view.lease = slice_buffer.lease;

            expect(bs::avc::motionVectorHeaderValid(*slice_header),
                   "version-3 slice-table header is valid");
            expect(bs::avc::motionSliceReferences(view).size() == 2,
                   "slice table is exposed to consumers");
            expect(bs::avc::motionRefPocAt(view, 0, 0, 0, 0) == 4,
                   "slice 0 maps ref_idx 0 to POC 4");
            expect(bs::avc::motionRefPocAt(view, 0, 4, 0, 0) == 6,
                   "slice 1 maps ref_idx 0 to POC 6");
            expect(bs::avc::motionTo(view, 0, 0, 6).valid,
                   "per-slice motionTo resolves the correct table");
            slice_buffer.lease.reset();
        }
    }

    pool.trimAll();

    /*
     * Version-3 exact macroblock map.  The range-only fallback would resolve
     * every macroblock of this picture to slice 0; the map must override it.
     * The picture is MBAFF, whose CurrMbAddr is 2 * pair_index + parity
     * rather than raster order.
     */
    {
        constexpr std::uint32_t kMbWidth = 2;
        constexpr std::uint32_t kMbHeight = 2;
        const std::size_t table_offset = sizeof(bs::avc::MotionVectorBufferHeader);
        const std::size_t map_offset =
                (table_offset + 2 * sizeof(bs::avc::MotionSliceReference) + 3u) &
                ~std::size_t{3u};
        const std::size_t cells_offset =
                (map_offset + 4 * sizeof(bs::avc::MotionSliceRun) + 63u) &
                ~std::size_t{63u};
        const std::size_t cell_count = 64;
        bs::avc::MotionVectorBufferRequest map_request{};
        map_request.width4x4 = 8;
        map_request.height4x4 = 8;
        map_request.cell_count = static_cast<std::uint32_t>(cell_count);
        map_request.cell_stride = sizeof(bs::avc::MvCell);
        map_request.required_bytes = cells_offset + cell_count * sizeof(bs::avc::MvCell);
        map_request.alignment = 64;

        auto map_buffer = pool.acquire(map_request);
        expect(map_buffer.data != nullptr, "macroblock-map buffer acquired");
        if (map_buffer.data != nullptr) {
            auto* map_header =
                    static_cast<bs::avc::MotionVectorBufferHeader*>(map_buffer.data);
            *map_header = bs::avc::MotionVectorBufferHeader{};
            map_header->magic = bs::avc::kMotionVectorBufferMagic;
            map_header->version = bs::avc::kMotionVectorBufferVersion;
            map_header->header_bytes = sizeof(*map_header);
            map_header->total_bytes =
                    static_cast<std::uint32_t>(map_request.required_bytes);
            map_header->flags = bs::avc::MvPictureMbaff;
            map_header->poc = 6;
            map_header->mb_width = kMbWidth;
            map_header->mb_height = kMbHeight;
            map_header->width4x4 = 8;
            map_header->height4x4 = 8;
            map_header->cell_count = static_cast<std::uint32_t>(cell_count);
            map_header->cell_stride = sizeof(bs::avc::MvCell);
            map_header->cells_offset = static_cast<std::uint32_t>(cells_offset);
            map_header->slice_ref_count = 2;
            map_header->slice_ref_capacity = 2;
            map_header->slice_ref_stride = sizeof(bs::avc::MotionSliceReference);
            map_header->slice_refs_offset = static_cast<std::uint32_t>(table_offset);
            map_header->slice_map_count = 4;
            map_header->slice_map_capacity = 4;
            map_header->slice_map_stride = sizeof(bs::avc::MotionSliceRun);
            map_header->slice_map_offset = static_cast<std::uint32_t>(map_offset);
            map_header->slice_map_coverage = kMbWidth * kMbHeight;

            auto* map_slices = reinterpret_cast<bs::avc::MotionSliceReference*>(
                    static_cast<std::uint8_t*>(map_buffer.data) + table_offset);
            map_slices[0].ref_poc[0][0] = 4;
            map_slices[0].num_ref_idx[0] = 1;
            map_slices[0].mb_addr_begin = 0;
            map_slices[0].mb_addr_end = 4;
            map_slices[1].ref_poc[0][0] = 2;
            map_slices[1].num_ref_idx[0] = 1;
            map_slices[1].mb_addr_begin = 4;
            map_slices[1].mb_addr_end = 4;

            auto* runs = reinterpret_cast<bs::avc::MotionSliceRun*>(
                    static_cast<std::uint8_t*>(map_buffer.data) + map_offset);
            for (std::uint32_t i = 0; i < 4; ++i) {
                runs[i].mb_addr = i;
                runs[i].slice_index = i & 1u;
            }

            auto* cells = reinterpret_cast<bs::avc::MvCell*>(
                    static_cast<std::uint8_t*>(map_buffer.data) + cells_offset);
            for (std::size_t i = 0; i < cell_count; ++i) {
                cells[i] = bs::avc::MvCell{};
                cells[i].flags = bs::avc::MvList0;
                cells[i].ref_idx[0] = 0;
            }
            /* Bottom-left cell: nonzero motion exposes the selected POC. */
            cells[4 * 8].mv[0][0] = 4;
            cells[4 * 8].mv[0][1] = -8;

            bs::avc::MotionVectorBufferView map_view{};
            map_view.header = map_header;
            map_view.data = map_buffer.data;
            map_view.bytes = map_request.required_bytes;
            map_view.lease = map_buffer.lease;

            expect(bs::avc::motionVectorHeaderValid(*map_header),
                   "version-3 macroblock-map header is valid");
            const auto exposed_runs = bs::avc::motionSliceRuns(map_view);
            expect(exposed_runs.size() == 4,
                   "macroblock-map runs are exposed to consumers");
            expect(bs::avc::motionMbAddressAt(*map_header, 0, 0) == 0,
                   "MBAFF top-left macroblock address");
            expect(bs::avc::motionMbAddressAt(*map_header, 0, 4) == 1,
                   "MBAFF bottom-left macroblock address");
            expect(bs::avc::motionMbAddressAt(*map_header, 4, 0) == 2,
                   "MBAFF top-right macroblock address");
            expect(bs::avc::motionMbAddressAt(*map_header, 4, 4) == 3,
                   "MBAFF bottom-right macroblock address");
            expect(bs::avc::motionRefPocAt(map_view, 0, 0, 0, 0) == 4,
                   "macroblock map selects slice 0 for address 0");
            expect(bs::avc::motionRefPocAt(map_view, 0, 4, 0, 0) == 2,
                   "macroblock map selects slice 1 for the MBAFF bottom row");
            expect(bs::avc::motionRefPocAt(map_view, 4, 4, 0, 0) == 2,
                   "macroblock map selects slice 1 for address 3");
            /*
             * Project picture POC 6 onto POC 4.  The map selects slice 1
             * (reference POC 2), so the scale is (4-6)/(2-6) = 0.5.  A
             * first-slice fallback would select POC 4 and scale by 1.0.
             */
            const bs::avc::MotionSample projected =
                    bs::avc::motionTo(map_view, 0, 4, 4);
            expect(projected.valid &&
                           std::fabs(projected.x - 0.5f) < 1e-6f &&
                           std::fabs(projected.y + 1.0f) < 1e-6f,
                   "per-cell motionTo uses the macroblock map, not the first slice");
            map_buffer.lease.reset();
        }
    }

    pool.trimAll();
    if (failures != 0) {
        std::fprintf(stderr, "%d motion-buffer failure(s)\n", failures);
        return 1;
    }

    std::puts("motion buffer: ok");
    return 0;
}
