// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#include "bsparser.hpp"
#include "utils/MotionBufferPool.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ReadyStats {
    int pictures = 0;
    int ok = 0;
    int direct_cells = 0;
    int inter_cells = 0;
    bool pointer_ok = true;
    std::vector<std::uint8_t> statuses;
    struct Field {
        std::uint32_t width4x4 = 0;
        std::uint32_t height4x4 = 0;
        std::vector<bs::avc::MvCell> cells;
    };
    std::vector<Field> p_fields;
};

struct SinkContext {
    bs::utils::MotionBufferPool* pool = nullptr;
    ReadyStats stats{};
};

bs::avc::MotionVectorBuffer acquire_thunk(
        void* context, const bs::avc::MotionVectorBufferRequest& request) {
    return static_cast<SinkContext*>(context)->pool->acquire(request);
}

void ready(void* context, const bs::avc::MotionVectorBufferView& view) {
    auto* stats = &static_cast<SinkContext*>(context)->stats;
    ++stats->pictures;

    if (view.header == nullptr || view.data == nullptr ||
        view.header != static_cast<const void*>(view.data) ||
        !bs::avc::motionVectorHeaderValid(*view.header)) {
        stats->pointer_ok = false;
        return;
    }

    if (view.header->status == static_cast<std::uint8_t>(bs::avc::MotionVectorStatus::Ok)) {
        ++stats->ok;
    }
    stats->statuses.push_back(view.header->status);

    const auto cells = bs::avc::motionCells(view);
    if (cells.size() != view.header->cell_count) {
        stats->pointer_ok = false;
        return;
    }

    if (view.header->status == static_cast<std::uint8_t>(bs::avc::MotionVectorStatus::Ok) &&
        (view.header->flags & bs::avc::MvPictureIntra) == 0 &&
        view.header->num_ref_idx[1] == 0) {
        ReadyStats::Field field;
        field.width4x4 = view.header->width4x4;
        field.height4x4 = view.header->height4x4;
        field.cells.assign(cells.begin(), cells.end());
        stats->p_fields.push_back(std::move(field));
    }

    const auto* base = static_cast<const std::uint8_t*>(view.data);
    const auto* cell_bytes = reinterpret_cast<const std::uint8_t*>(cells.data());
    if (cell_bytes < base || cell_bytes + cells.size() * sizeof(bs::avc::MvCell) >
                                     base + view.bytes) {
        stats->pointer_ok = false;
    }

    for (const bs::avc::MvCell& cell : cells) {
        if (cell.flags & bs::avc::MvDirect) ++stats->direct_cells;
        if (!(cell.flags & bs::avc::MvSkip) &&
            (cell.flags & (bs::avc::MvList0 | bs::avc::MvList1))) {
            ++stats->inter_cells;
        }
    }
}

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(1);
    }
    return std::vector<std::uint8_t>(
            std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct ReferenceField {
    std::uint32_t width4x4 = 0;
    std::uint32_t height4x4 = 0;
    std::vector<std::array<std::int16_t, 2>> mv;
};

std::vector<ReferenceField> read_mv_reference(const char* path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(1);
    }
    std::vector<ReferenceField> fields;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream header(line);
        std::string kind;
        header >> kind;
        if (kind != "picture") continue;
        ReferenceField field;
        int index = 0;
        header >> index >> field.width4x4 >> field.height4x4;
        field.mv.resize(static_cast<std::size_t>(field.width4x4) * field.height4x4);
        for (auto& mv : field.mv) {
            if (!std::getline(in, line)) {
                std::fprintf(stderr, "truncated motion-vector reference %s\n", path);
                std::exit(1);
            }
            std::istringstream entry(line);
            int x = 0;
            int y = 0;
            int dx = 0;
            int dy = 0;
            entry >> x >> y >> dx >> dy;
            mv = {static_cast<std::int16_t>(dx), static_cast<std::int16_t>(dy)};
        }
        fields.push_back(std::move(field));
    }
    return fields;
}

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 7) {
        std::fprintf(stderr,
                     "usage: avc_mv_test <static.h264> <b_frames.h264> <static.mv.txt> "
                     "<main.h264> <main.mv.txt> <hd.h264>\n");
        return 1;
    }

    const auto static_stream = read_file(argv[1]);
    const auto b_stream = read_file(argv[2]);

    /*
     * Disabled path: no sink, no slice-data parsing and no extractor.
     */
    {
        auto state = bs::create_state(bs::Codec::Avc);
        bs::AvcParsedHandlers handlers{};
        (void)bs::parse(*state, std::span<const std::uint8_t>(static_stream.data(),
                                                             static_stream.size()),
                        bs::NalFramingMode::AnnexB, handlers, 4);
        expect(state->avc_motion_vectors() == nullptr,
               "disabled motion-vector path leaves the extractor uncreated");
    }

    /*
     * Zero-copy static stream: every P picture is a skipped frame.
     */
    {
        bs::utils::MotionBufferPool pool({2, 8, 4u << 20, 64});
        SinkContext context{&pool, {}};

        auto state = bs::create_state(bs::Codec::Avc);
        bs::AvcParsedHandlers handlers{};
        handlers.motion_vectors.ctx = &context;
        handlers.motion_vectors.acquire = &acquire_thunk;
        handlers.motion_vectors.ready = &ready;
        handlers.motion_vectors.options.emit_intra = true;
        (void)bs::parse(*state, std::span<const std::uint8_t>(static_stream.data(),
                                                             static_stream.size()),
                        bs::NalFramingMode::AnnexB, handlers, 4);
        expect(context.stats.pointer_ok, "zero-copy buffer and cell views point into consumer memory");
        expect(context.stats.pictures > 0, "static stream produced pictures");
        expect(context.stats.ok == context.stats.pictures, "static stream pictures are fully parsed");
        expect(context.stats.inter_cells == 0, "static stream has no inter motion");

        std::ifstream reference(argv[3]);
        expect(static_cast<bool>(reference), "FFmpeg motion-vector reference opens");
        int reference_frames = 0;
        bool reference_zero = true;
        std::string line;
        while (std::getline(reference, line)) {
            if (line.rfind("frame ", 0) == 0) {
                ++reference_frames;
            }
            const auto mv = line.find("mv=");
            if (mv != std::string::npos && line.compare(mv + 3, 3, "0,0") != 0) {
                reference_zero = false;
            }
        }
        expect(reference_frames == context.stats.pictures,
               "static picture count matches the FFmpeg reference");
        expect(reference_zero, "static FFmpeg reference has zero motion only");
        expect(state->avc_motion_vectors() != nullptr, "extractor exists when the sink is set");
        expect(state->avc_motion_vectors()->status() == bs::avc::MotionVectorStatus::Ok,
               "static stream status is Ok");

        state->clear();
        expect(pool.stats().buffersInUse == 0, "clear releases reference buffers");
    }

    /*
     * B-frame stream: exercise direct-mode cells and the zero-copy lease
     * retention for reference pictures.
     */
    {
        bs::utils::MotionBufferPool pool({4, 16, 8u << 20, 64});
        SinkContext context{&pool, {}};

        auto state = bs::create_state(bs::Codec::Avc);
        bs::AvcParsedHandlers handlers{};
        handlers.motion_vectors.ctx = &context;
        handlers.motion_vectors.acquire = &acquire_thunk;
        handlers.motion_vectors.ready = &ready;
        handlers.motion_vectors.options.emit_intra = true;
        (void)bs::parse(*state, std::span<const std::uint8_t>(b_stream.data(), b_stream.size()),
                        bs::NalFramingMode::AnnexB, handlers, 4);

        expect(context.stats.pointer_ok, "B stream views point into consumer memory");
        expect(context.stats.pictures > 0, "B stream produced pictures");
        expect(context.stats.ok > 0, "at least one B-stream picture is fully parsed");
        expect(context.stats.direct_cells > 0, "B stream produces direct-mode cells");
        expect(pool.stats().buffersInUse > 0,
               "reference pictures retain consumer buffers in the DPB");

        state->clear();
        pool.trimAll();
        expect(pool.stats().buffersInUse == 0, "clearing the DPB releases all leases");
    }

    /*
     * Residual-heavy CABAC stream: every picture must parse completely, and
     * the P-picture motion field must match the FFmpeg reference.
     */
    {
        bs::utils::MotionBufferPool pool({2, 16, 16u << 20, 64});
        SinkContext context{&pool, {}};

        auto state = bs::create_state(bs::Codec::Avc);
        bs::AvcParsedHandlers handlers{};
        handlers.motion_vectors.ctx = &context;
        handlers.motion_vectors.acquire = &acquire_thunk;
        handlers.motion_vectors.ready = &ready;
        handlers.motion_vectors.options.emit_intra = true;
        const auto main_stream = read_file(argv[4]);
        (void)bs::parse(*state, std::span<const std::uint8_t>(main_stream.data(), main_stream.size()),
                        bs::NalFramingMode::AnnexB, handlers, 4);

        expect(context.stats.pointer_ok, "main stream views point into consumer memory");
        expect(context.stats.pictures > 0, "main stream produced pictures");
        expect(context.stats.statuses.size() ==
                       static_cast<std::size_t>(context.stats.pictures),
               "main stream delivered every picture once");

        bool truncated = false;
        for (std::uint8_t status : context.stats.statuses) {
            if (status == static_cast<std::uint8_t>(bs::avc::MotionVectorStatus::Truncated)) {
                truncated = true;
            }
        }
        expect(!truncated, "main stream has no truncated pictures");
        expect(context.stats.ok == context.stats.pictures,
               "main stream parses every picture completely");

        const auto reference = read_mv_reference(argv[5]);
        expect(reference.size() >= 3, "main motion-vector reference has P pictures");
        expect(context.stats.p_fields.size() >= reference.size(),
               "main stream exposes the referenced P pictures");
        for (std::size_t i = 0; i < reference.size() && i < context.stats.p_fields.size(); ++i) {
            const auto& actual = context.stats.p_fields[i];
            const auto& expected = reference[i];
            expect(actual.width4x4 == expected.width4x4 &&
                           actual.height4x4 == expected.height4x4,
                   "main reference picture dimensions match");
            expect(actual.cells.size() == expected.mv.size(),
                   "main reference picture cell count matches");
            if (actual.cells.size() != expected.mv.size()) continue;
            bool match = true;
            for (std::size_t c = 0; c < expected.mv.size(); ++c) {
                if (actual.cells[c].mv[0][0] != expected.mv[c][0] ||
                    actual.cells[c].mv[0][1] != expected.mv[c][1]) {
                    match = false;
                    break;
                }
            }
            expect(match, "main P-picture list-0 motion matches FFmpeg");
        }

        state->clear();
        pool.trimAll();
        expect(pool.stats().buffersInUse == 0, "main stream clear releases buffers");
    }

    /*
     * HD CABAC stream: 3600 macroblocks per picture, with transform-8x8,
     * intra macroblocks and multi-reference B slices.  It must not truncate.
     */
    {
        bs::utils::MotionBufferPool pool({2, 16, 32u << 20, 64});
        SinkContext context{&pool, {}};

        auto state = bs::create_state(bs::Codec::Avc);
        bs::AvcParsedHandlers handlers{};
        handlers.motion_vectors.ctx = &context;
        handlers.motion_vectors.acquire = &acquire_thunk;
        handlers.motion_vectors.ready = &ready;
        handlers.motion_vectors.options.emit_intra = true;
        const auto hd_stream = read_file(argv[6]);
        (void)bs::parse(*state, std::span<const std::uint8_t>(hd_stream.data(), hd_stream.size()),
                        bs::NalFramingMode::AnnexB, handlers, 4);

        expect(context.stats.pictures > 0, "HD stream produced pictures");
        bool truncated = false;
        for (std::uint8_t status : context.stats.statuses) {
            if (status == static_cast<std::uint8_t>(bs::avc::MotionVectorStatus::Truncated)) {
                truncated = true;
            }
        }
        expect(!truncated, "HD stream has no truncated pictures");
        expect(context.stats.ok == context.stats.pictures,
               "HD stream parses every picture completely");

        state->clear();
        pool.trimAll();
        expect(pool.stats().buffersInUse == 0, "HD stream clear releases buffers");
    }

    /*
     * POC projection helper.
     */
    {
        bs::avc::MotionVectorBufferHeader header{};
        header.poc = 10;
        header.ref_poc[0][0] = 0;
        header.ref_poc[0][1] = 8;
        header.ref_poc[1][0] = 20;
        bs::avc::MvCell cell{};
        cell.ref_idx[0] = 0;
        cell.mv[0][0] = 8;  // 2 pixels to the reference at POC 0
        cell.mv[0][1] = 0;
        const auto sample = bs::avc::motionTo(cell, 20, header);
        expect(sample.valid, "motionTo produces a valid sample");
        expect(std::abs(sample.x) > 0.0f, "motionTo scales toward the target POC");
    }

    if (failures != 0) {
        std::fprintf(stderr, "%d motion-vector failure(s)\n", failures);
        return 1;
    }

    std::puts("avc motion vectors: ok");
    return 0;
}
