// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * Micro-benchmark for the unified parser.
 *
 * Compares the cost of each layer of the unified parse:
 *   1. raw-only    - original BsNalHandlers dispatch (framing + NAL unit)
 *   2. typed-ps    - VPS/SPS/PPS typed callbacks only
 *   3. typed-full  - VPS/SPS/PPS + slice + SEI typed callbacks
 *   4. c-api       - bs_parse_hevc / bs_parse_avc (full typed path, C ABI)
 *
 * Usage: bench <file.hevc|.h264> <hevc|avc> [iterations]
 */
#include <bsparser.hpp>
#include <capi/bs_capi.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <parser/nal_framer.hpp>

static std::vector<std::uint8_t> read_file(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(1);
    }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<std::uint8_t> buf(static_cast<size_t>(n));
    std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    return buf;
}

template <typename T>
static double avg(const std::vector<T>& v) {
    double s = 0;
    for (auto x : v)
        s += static_cast<double>(x);
    return s / static_cast<double>(v.size());
}

static const char* codec_str = "?";

static std::uint64_t bench_raw(const std::vector<std::uint8_t>& buf, std::size_t iters) {
    std::uint64_t nals = 0;
    volatile std::uint64_t sink = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iters; ++i) {
        auto st = bs::create_state(bs::Codec::Hevc);
        bs::BsNalHandlers h{};
        (void)h;
        bs::parse(*st, buf, bs::NalFramingMode::AnnexB, h);
        nals += 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    std::printf(
        "[%s] raw-only      : %8.1f us/iter  %7.1f MB/s\n",
        codec_str,
        us / iters,
        (buf.size() * iters) / (us * 1e-6) / 1e6
    );
    return nals;
}

static std::uint64_t bench_typed_ps(const std::vector<std::uint8_t>& buf, std::size_t iters) {
    std::uint64_t nals = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iters; ++i) {
        auto st = bs::create_state(bs::Codec::Hevc);
        bs::HevcParsedHandlers h{};
        h.vps = [](const bs::VideoParameterSet&) {};
        h.sps = [](const bs::SequenceParameterSet&) {};
        h.pps = [](const bs::PictureParameterSet&) {};
        bs::parse(*st, buf, bs::NalFramingMode::AnnexB, h);
        nals += 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    std::printf(
        "[%s] typed-ps      : %8.1f us/iter  %7.1f MB/s\n",
        codec_str,
        us / iters,
        (buf.size() * iters) / (us * 1e-6) / 1e6
    );
    return nals;
}

static std::uint64_t bench_typed_slice(const std::vector<std::uint8_t>& buf, std::size_t iters) {
    std::uint64_t nals = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iters; ++i) {
        auto st = bs::create_state(bs::Codec::Hevc);
        bs::HevcParsedHandlers h{};
        h.vps = [](const bs::VideoParameterSet&) {};
        h.sps = [](const bs::SequenceParameterSet&) {};
        h.pps = [](const bs::PictureParameterSet&) {};
        h.slice = [](const bs::SliceSegmentHeader&) {};
        bs::parse(*st, buf, bs::NalFramingMode::AnnexB, h);
        nals += 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    std::printf(
        "[%s] typed-slice   : %8.1f us/iter  %7.1f MB/s\n",
        codec_str,
        us / iters,
        (buf.size() * iters) / (us * 1e-6) / 1e6
    );
    return nals;
}

static std::uint64_t bench_typed_sei(const std::vector<std::uint8_t>& buf, std::size_t iters) {
    std::uint64_t nals = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iters; ++i) {
        auto st = bs::create_state(bs::Codec::Hevc);
        bs::HevcParsedHandlers h{};
        h.vps = [](const bs::VideoParameterSet&) {};
        h.sps = [](const bs::SequenceParameterSet&) {};
        h.pps = [](const bs::PictureParameterSet&) {};
        h.sei = [](const bs::ParsedSei&) {};
        bs::parse(*st, buf, bs::NalFramingMode::AnnexB, h);
        nals += 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    std::printf(
        "[%s] typed-sei     : %8.1f us/iter  %7.1f MB/s\n",
        codec_str,
        us / iters,
        (buf.size() * iters) / (us * 1e-6) / 1e6
    );
    return nals;
}

static std::uint64_t bench_typed_full(const std::vector<std::uint8_t>& buf, std::size_t iters) {
    std::uint64_t nals = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iters; ++i) {
        auto st = bs::create_state(bs::Codec::Hevc);
        bs::HevcParsedHandlers h{};
        h.vps = [](const bs::VideoParameterSet&) {};
        h.sps = [](const bs::SequenceParameterSet&) {};
        h.pps = [](const bs::PictureParameterSet&) {};
        h.sei = [](const bs::ParsedSei&) {};
        h.slice = [](const bs::SliceSegmentHeader&) {};
        bs::parse(*st, buf, bs::NalFramingMode::AnnexB, h);
        nals += 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    std::printf(
        "[%s] typed-full    : %8.1f us/iter  %7.1f MB/s\n",
        codec_str,
        us / iters,
        (buf.size() * iters) / (us * 1e-6) / 1e6
    );
    return nals;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <file> <hevc|avc> [iters]\n", argv[0]);
        return 1;
    }
    const char* path = argv[1];
    const char* codec = argv[2];
    std::size_t iters = (argc > 3) ? std::atoll(argv[3]) : 2000;
    codec_str = codec;

    auto buf = read_file(path);
    std::printf("file=%s size=%zu iters=%zu\n", path, buf.size(), iters);

    if (std::strcmp(codec, "hevc") == 0) {
        bench_raw(buf, iters);
        bench_typed_ps(buf, iters);
        bench_typed_slice(buf, iters);
        bench_typed_sei(buf, iters);
        bench_typed_full(buf, iters);
    } else if (std::strcmp(codec, "avc") == 0) {
        /* AVC variants reuse the same scenario shapes. */
        auto avc_raw = [&](std::size_t n) {
            auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n; ++i) {
                auto st = bs::create_state(bs::Codec::Avc);
                bs::avc::NalHandlers h{};
                bs::parse(*st, buf, bs::NalFramingMode::AnnexB, h);
            }
            auto t1 = std::chrono::steady_clock::now();
            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            std::printf(
                "[%s] raw-only      : %8.1f us/iter  %7.1f MB/s\n",
                codec_str,
                us / n,
                (buf.size() * n) / (us * 1e-6) / 1e6
            );
        };
        auto avc_ps = [&](std::size_t n) {
            auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n; ++i) {
                auto st = bs::create_state(bs::Codec::Avc);
                bs::AvcParsedHandlers h{};
                h.sps = [](const bs::avc::SequenceParameterSet&) {};
                h.pps = [](const bs::avc::PictureParameterSet&) {};
                bs::parse(*st, buf, bs::NalFramingMode::AnnexB, h);
            }
            auto t1 = std::chrono::steady_clock::now();
            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            std::printf(
                "[%s] typed-ps      : %8.1f us/iter  %7.1f MB/s\n",
                codec_str,
                us / n,
                (buf.size() * n) / (us * 1e-6) / 1e6
            );
        };
        auto avc_full = [&](std::size_t n) {
            auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < n; ++i) {
                auto st = bs::create_state(bs::Codec::Avc);
                bs::AvcParsedHandlers h{};
                h.sps = [](const bs::avc::SequenceParameterSet&) {};
                h.pps = [](const bs::avc::PictureParameterSet&) {};
                h.sei = [](const bs::avc::ParsedSei&) {};
                h.slice = [](const bs::avc::SliceHeader&) {};
                bs::parse(*st, buf, bs::NalFramingMode::AnnexB, h);
            }
            auto t1 = std::chrono::steady_clock::now();
            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            std::printf(
                "[%s] typed-full    : %8.1f us/iter  %7.1f MB/s\n",
                codec_str,
                us / n,
                (buf.size() * n) / (us * 1e-6) / 1e6
            );
        };
        avc_raw(iters);
        avc_ps(iters);
        avc_full(iters);
    } else {
        std::printf("unknown codec %s\n", codec);
        return 1;
    }

    /* C API path (full typed). */
    auto st = bs_state_create(std::strcmp(codec, "hevc") == 0 ? BS_CODEC_HEVC : BS_CODEC_AVC);
    BsHevcHandlers hh{};
    hh.vps = [](void*, const BsHevcVideoParameterSet*) {};
    hh.sps = [](void*, const BsHevcSequenceParameterSet*) {};
    hh.pps = [](void*, const BsHevcPictureParameterSet*) {};
    hh.sei = [](void*, unsigned int, const unsigned char*, size_t) {};
    hh.slice = [](void*, const BsHevcSliceSegmentHeader*) {};
    BsAvcHandlers ah{};
    ah.sps = [](void*, const BsAvcSequenceParameterSet*) {};
    ah.pps = [](void*, const BsAvcPictureParameterSet*) {};
    ah.sei = [](void*, unsigned int, const unsigned char*, size_t) {};
    ah.slice = [](void*, const BsAvcSliceHeader*) {};

    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iters; ++i) {
        if (std::strcmp(codec, "hevc") == 0) {
            bs_parse_hevc(st, buf.data(), buf.size(), BS_FRAMING_ANNEX_B, 4, &hh);
        } else {
            bs_parse_avc(st, buf.data(), buf.size(), BS_FRAMING_ANNEX_B, 4, &ah);
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    std::printf(
        "[%s] c-api-full    : %8.1f us/iter  %7.1f MB/s\n",
        codec_str,
        us / iters,
        (buf.size() * iters) / (us * 1e-6) / 1e6
    );
    bs_state_destroy(st);

    return 0;
}
