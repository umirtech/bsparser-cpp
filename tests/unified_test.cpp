// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * ---------------------------------------------------------------------------
 * Unified public API test
 * ---------------------------------------------------------------------------
 *
 * Exercises:
 *
 *     bs::Codec
 *     bs::State  (opaque)
 *     bs::create_state()
 *     bs::parse()
 *
 * for both codec paths and verifies that the opaque State auto-manages the
 * parameter sets.
 *
 * The dispatcher callbacks are plain function pointers, so they cannot capture.
 * A process-wide pointer to the active State is used instead.
 */

#include "bsparser.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

std::unique_ptr<bs::State> g_state;

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);

    if (!in) {
        std::cerr << "cannot open " << path << "\n";
        std::exit(1);
    }

    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
    );
}

int test_hevc() {
    using namespace bs;

    const auto bytes = read_file("tests/fuzz/corpus/stream.hevc");

    std::span<const std::uint8_t> data{bytes.data(), bytes.size()};

    g_state = create_state(Codec::Hevc);

    BsNalHandlers handlers{};

    handlers.slice = [](const NalUnit&) {
        /*
         * Resolve slice dependencies through the opaque
         * State instead of a user-owned manager.
         */
        auto* sets = g_state->hevc_sets();

        if (sets == nullptr) {
            std::cerr << "HEVC: null state sets\n";
            std::exit(1);
        }

        /*
         * The sample uses PPS id 0; the State should have
         * it after the auto-store pass.
         */
        const auto resolved = sets->resolve_pps(0);

        if (!resolved.valid()) {
            std::cerr << "HEVC: could not resolve PPS 0\n";
            std::exit(1);
        }
    };

    const std::size_t parsed = parse(*g_state, data, NalFramingMode::AnnexB, handlers);

    (void)parsed;

    std::cout << "[unified hevc] parsed=" << parsed << " vps=" << g_state->hevc_sets()->vps_count()
              << " sps=" << g_state->hevc_sets()->sps_count()
              << " pps=" << g_state->hevc_sets()->pps_count() << "\n";

    if (g_state->hevc_sets()->vps_count() == 0 || g_state->hevc_sets()->sps_count() == 0 ||
        g_state->hevc_sets()->pps_count() == 0) {
        std::cerr << "HEVC: missing parameter sets\n";
        return 1;
    }

    return 0;
}

int test_avc() {
    using namespace bs;

    const auto bytes = read_file("tests/fuzz/corpus/avc_main.h264");

    std::span<const std::uint8_t> data{bytes.data(), bytes.size()};

    g_state = create_state(Codec::Avc);

    avc::NalHandlers handlers{};

    handlers.slice = [](const avc::NalUnit&) {
        auto* sets = g_state->avc_sets();

        if (sets == nullptr) {
            std::cerr << "AVC: null state sets\n";
            std::exit(1);
        }

        const auto resolved = sets->resolve(0);

        if (!resolved.valid()) {
            std::cerr << "AVC: could not resolve PPS 0\n";
            std::exit(1);
        }
    };

    const std::size_t parsed = parse(*g_state, data, NalFramingMode::AnnexB, handlers);

    std::cout << "[unified avc] parsed=" << parsed << " sps=" << g_state->avc_sets()->sps_count()
              << " pps=" << g_state->avc_sets()->pps_count() << "\n";

    if (g_state->avc_sets()->sps_count() == 0 || g_state->avc_sets()->pps_count() == 0) {
        std::cerr << "AVC: missing parameter sets\n";
        return 1;
    }

    return 0;
}

int test_codec_mismatch() {
    using namespace bs;

    g_state = create_state(Codec::Avc);

    BsNalHandlers handlers{};

    const std::vector<std::uint8_t> empty;

    std::span<const std::uint8_t> data{empty.data(), empty.size()};

    try {
        (void)parse(*g_state, data, NalFramingMode::AnnexB, handlers);

        std::cerr << "codec mismatch was not detected\n";
        return 1;

    } catch (const std::exception&) {
        /* expected */
        return 0;
    }
}

int test_multiple_states() {
    using namespace bs;

    /*
     * Create several independent states of mixed codecs and use
     * them concurrently.  Each State owns its own parameter-set
     * manager; there is no shared parser state, so multiple
     * states can coexist and be driven independently (including
     * from different threads).
     */
    auto hevc_a = create_state(Codec::Hevc);
    auto hevc_b = create_state(Codec::Hevc);
    auto avc_a = create_state(Codec::Avc);

    if (hevc_a->codec() != Codec::Hevc || hevc_b->codec() != Codec::Hevc ||
        avc_a->codec() != Codec::Avc) {
        std::cerr << "multiple states: bad codec\n";
        return 1;
    }

    /*
     * The HEVC states start empty and stay isolated from one
     * another: feeding one must not affect the other.
     */
    if (hevc_a->hevc_sets()->sps_count() != 0 || hevc_b->hevc_sets()->sps_count() != 0 ||
        avc_a->avc_sets()->sps_count() != 0) {
        std::cerr << "multiple states: non-empty at start\n";
        return 1;
    }

    /*
     * A State is move-only: it cannot be copied, but as many
     * independent instances as needed can be created.
     */
    auto moved = std::move(hevc_b);

    if (moved->codec() != Codec::Hevc) {
        std::cerr << "multiple states: move broke codec\n";
        return 1;
    }

    std::cout << "[unified multi] hevc_a=" << static_cast<void*>(hevc_a.get())
              << " avc_a=" << static_cast<void*>(avc_a.get()) << "\n";

    /*
     * Reusing one State across independent streams leaves stale
     * parameter sets behind.  clear() must reset the store so a
     * later stream starts clean (no stale-ID collision).
     */
    const auto hevc_bytes = read_file("tests/fuzz/corpus/stream.hevc");

    std::span<const std::uint8_t> hevc_data{hevc_bytes.data(), hevc_bytes.size()};

    BsNalHandlers noop{};

    (void)parse(*hevc_a, hevc_data, NalFramingMode::AnnexB, noop);

    if (hevc_a->hevc_sets()->sps_count() == 0) {
        std::cerr << "multi: first pass stored nothing\n";
        return 1;
    }

    hevc_a->clear();

    if (hevc_a->hevc_sets()->sps_count() != 0 || hevc_a->hevc_sets()->pps_count() != 0 ||
        hevc_a->hevc_sets()->vps_count() != 0) {
        std::cerr << "multi: clear() did not reset store\n";
        return 1;
    }

    return 0;
}

}  // namespace

int main() {
    if (test_hevc() != 0) {
        return 1;
    }

    if (test_avc() != 0) {
        return 1;
    }

    if (test_codec_mismatch() != 0) {
        return 1;
    }

    if (test_multiple_states() != 0) {
        return 1;
    }

    std::cout << "[unified] all tests passed\n";
    return 0;
}
