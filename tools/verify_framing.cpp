// Framing correctness checker: compares the SWAR start-code
// finder against a simple scalar reference over real and random
// buffers. Exits non-zero on mismatch.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

#include <parser/nal_framer.hpp>

using namespace bs;

static std::size_t scalar_find(const std::span<const std::uint8_t>& d, std::size_t from) {
    for (std::size_t i = from; i + 3 <= d.size(); ++i) {
        if (annex_b_start_code_size(d, i) != 0) {
            return i;
        }
    }
    return d.size();
}

template <typename F>
static std::vector<std::pair<std::size_t, std::size_t>> collect(
    F finder, const std::span<const std::uint8_t>& d
) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    std::size_t pos = 0;
    while (pos + 3 <= d.size()) {
        std::size_t start = finder(d, pos);
        if (start == d.size()) {
            break;
        }
        std::size_t prefix = annex_b_start_code_size(d, start);
        if (prefix == 0) {
            prefix = 3;
        }
        std::size_t begin = start + prefix;

        std::size_t next = finder(d, begin);
        std::size_t end = next;
        while (end > begin && d[end - 1] == 0x00) {
            --end;
        }
        if (begin < end) {
            out.push_back({begin, end});
        }
        pos = next;
    }
    return out;
}

int main(int argc, char** argv) {
    std::vector<std::uint8_t> buf;

    if (argc >= 2) {
        FILE* f = std::fopen(argv[1], "rb");
        if (!f) {
            std::cerr << "cannot open " << argv[1] << "\n";
            return 2;
        }
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        buf.resize(static_cast<std::size_t>(n));
        std::fread(buf.data(), 1, buf.size(), f);
        std::fclose(f);
    }

    auto sw = [&](const std::span<const std::uint8_t>& d, std::size_t from) {
        return annex_b_find_start_code(d, from);
    };
    auto sc = [&](const std::span<const std::uint8_t>& d, std::size_t from) {
        return scalar_find(d, from);
    };

    int failures = 0;

    if (!buf.empty()) {
        auto a = collect(sw, buf);
        auto b = collect(sc, buf);
        if (a != b) {
            std::cerr << "REAL FILE mismatch: " << a.size() << " vs " << b.size() << "\n";
            std::size_t m = a.size() < b.size() ? a.size() : b.size();
            for (std::size_t k = 0; k < m; ++k) {
                if (a[k] != b[k]) {
                    std::cerr << "  first diff nal " << k << " sw=[" << a[k].first << ","
                              << a[k].second << "] sc=[" << b[k].first << "," << b[k].second
                              << "]\n";
                    std::size_t at = a[k].first > 8 ? a[k].first - 8 : 0;
                    for (std::size_t p = at; p < at + 16 && p < buf.size(); ++p) {
                        std::cerr << "   buf[" << p << "]=" << std::hex << (int)buf[p] << std::dec
                                  << "\n";
                    }
                    break;
                }
            }
            failures++;
        } else {
            std::cout << "real file: " << a.size() << " nals OK\n";
        }
    }

    std::mt19937 rng(0x1234abcd);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> size_dist(0, 4096);

    for (int trial = 0; trial < 4000; ++trial) {
        std::size_t len = static_cast<std::size_t>(size_dist(rng));
        buf.resize(len);
        for (std::size_t i = 0; i < len; ++i) {
            buf[i] = static_cast<std::uint8_t>(byte_dist(rng));
        }
        auto a = collect(sw, buf);
        auto b = collect(sc, buf);
        if (a != b) {
            std::cerr << "RANDOM mismatch len=" << len << " sw=" << a.size() << " sc=" << b.size()
                      << "\n";
            failures++;
        }
    }

    if (failures == 0) {
        std::cout << "all framing checks passed\n";
        return 0;
    }
    std::cerr << failures << " failures\n";
    return 1;
}
