// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * Malformed-container regression test.
 *
 * These tiny fixtures are hand-crafted to hit specific parser paths that
 * previously produced out-of-bounds spans / reads:
 *   - AVI video chunk with a 4 GiB declared size in a 172-byte file
 *   - MP4 fragmented sample with a near-UINT64_MAX data offset
 *   - MP4 stsz box truncated so its sample-count field is at EOF
 *   - Ogg page header claiming 255 segments with no segment table
 *
 * None of them may crash, throw, or hang the demuxer.  Whether any given
 * file indexes successfully is irrelevant here.
 */
#include <demux/demuxer.hpp>
#include <mux/muxer.hpp>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "FAIL: " #cond " at line " << __LINE__ << "\n";     \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {};
    }
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void test_file(const std::string& path) {
    const auto bytes = read_file(path);
    CHECK(!bytes.empty());
    if (bytes.empty()) {
        return;
    }

    // Must not throw or crash.
    auto demuxer = bs::demux::Demuxer::init(bytes);
    (void)demuxer.container();
    (void)demuxer.metadata();

    for (auto& track : demuxer.tracks()) {
        while (track.hasNext()) {
            const auto sample = track.next();
            if (!sample) {
                break;
            }
            // Touch the payload so an out-of-bounds span is actually read.
            if (!sample.data.empty()) {
                (void)sample.data.front();
                (void)sample.data.back();
            }
        }
    }

    // Truncations at strategic points must also be safe.
    for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{4},
                          std::size_t{8}, bytes.size() / 2,
                          bytes.size() > 0 ? bytes.size() - 1 : 0}) {
        if (n <= bytes.size()) {
            auto d = bs::demux::Demuxer::init(
                std::span<const std::uint8_t>(bytes.data(), n)
            );
            (void)d.ok();
        }
    }

    std::cout << path << ": OK\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bs_malformed_test <file> [file...]\n";
        return 2;
    }

    for (int i = 1; i < argc; ++i) {
        test_file(argv[i]);
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
