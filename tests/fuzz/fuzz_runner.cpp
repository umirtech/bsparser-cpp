// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * In-process mutational fuzz runner (ASan/UBSan build).
 * Generates truncated / bit-flipped / randomized inputs from the
 * corpus and feeds them to LLVMFuzzerTestOneInput.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t);

namespace {

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    std::vector<std::uint8_t> b;
    if (!f)
        return b;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    b.resize(static_cast<std::size_t>(n));
    std::fread(b.data(), 1, b.size(), f);
    std::fclose(f);
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::vector<std::uint8_t>> corpus;
    for (int i = 1; i < argc; ++i) {
        auto b = read_file(argv[i]);
        if (!b.empty())
            corpus.push_back(std::move(b));
    }
    if (corpus.empty()) {
        std::fprintf(stderr, "no corpus files\n");
        return 1;
    }

    std::mt19937_64 rng(0xC0FFEEu);

    /*
     * Default to a modest iteration count so a casual run does not peg
     * the machine; pass `--iters N` for a deeper sweep (e.g. under ASan).
     */
    long long iters = 50000;
    if (argc > 1 && std::strcmp(argv[1], "--iters") == 0) {
        iters = std::stoll(std::string(argv[2]));
    }

    for (long long iter = 0; iter < iters; ++iter) {
        std::vector<std::uint8_t> input = corpus[rng() % corpus.size()];

        switch (rng() % 5) {
            case 0: /* truncate */
                if (!input.empty())
                    input.resize(rng() % (input.size() + 1));
                break;
            case 1: /* bit flips */
                for (int i = 0; i < 16 && !input.empty(); ++i)
                    input[rng() % input.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
                break;
            case 2: /* random bytes */
                input.clear();
                for (int i = 0, n = static_cast<int>(rng() % 1024); i < n; ++i)
                    input.push_back(static_cast<std::uint8_t>(rng() % 256));
                break;
            case 3: { /* insert random bytes */
                const int n = static_cast<int>(rng() % 64);
                std::vector<std::uint8_t> junk;
                for (int i = 0; i < n; ++i)
                    junk.push_back(static_cast<std::uint8_t>(rng() % 256));
                const std::size_t pos = input.empty() ? 0 : rng() % (input.size() + 1);
                input.insert(
                    input.begin() + static_cast<std::ptrdiff_t>(pos), junk.begin(), junk.end()
                );
                break;
            }
            case 4: /* prefix + truncation */
                if (!input.empty())
                    input.resize(rng() % (input.size() + 1));
                if (input.size() < 8)
                    continue;
                break;
        }

        try {
            LLVMFuzzerTestOneInput(input.data(), input.size());
        } catch (...) {
            /* a fuzz target must never let an exception escape */
        }
    }

    std::fprintf(stderr, "fuzz done: %lld iterations, no crash\n", iters);
    return 0;
}
