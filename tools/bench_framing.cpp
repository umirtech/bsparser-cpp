/* SIMD vs scalar Annex-B start-code scan throughput test.
 * Compile: clang++ -O3 -std=c++17 -DNDEBUG tools/bench_framing.cpp -o build/bench_framing.exe
 */
#include <emmintrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include <chrono>

static std::vector<std::uint8_t> make_stream(std::size_t size) {
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> dist(0, 255);
    std::vector<std::uint8_t> buf;
    buf.reserve(size + 16);
    while (buf.size() < size) {
        buf.push_back(0x00);
        buf.push_back(0x00);
        buf.push_back(0x00);
        buf.push_back(0x01);
        std::size_t body = 20u + (std::size_t)(rng() % 380);
        for (std::size_t k = 0; k < body && buf.size() < size; ++k)
            buf.push_back((std::uint8_t)dist(rng));
    }
    return buf;
}

static std::size_t scalar_count(const std::uint8_t* d, std::size_t n) {
    std::size_t c = 0;
    for (std::size_t i = 0; i + 3 <= n; ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            ++c;
            i += 2;
        }
    }
    return c;
}

static std::size_t simd_find(const std::uint8_t* d, std::size_t n, std::size_t from) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i one = _mm_set1_epi8(1);
    std::size_t i = from;
    while (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i*)(d + i));
        int z = _mm_movemask_epi8(_mm_cmpeq_epi8(v, zero));
        if (z == 0) {
            i += 16;
            continue;
        }
        int o = _mm_movemask_epi8(_mm_cmpeq_epi8(v, one));
        /* result bit k set => "00 00 01" starts at buffer pos i + k */
        unsigned r = (unsigned)(z & (z >> 1) & (o >> 2)) & 0x3FFFu;
        if (r) {
            unsigned k = (unsigned)__builtin_ctz(r);
            std::size_t start = i + k;
            if (start >= 1 && d[start - 1] == 0 && start - 1 >= from)
                return start - 1;
            return start;
        }
        /* recover start codes spanning the chunk boundary (k = 14,15) */
        if (i + 16 < n && d[i + 14] == 0 && d[i + 15] == 0 && d[i + 16] == 1) {
            std::size_t start = i + 14;
            if (start - 1 >= from && d[start - 1] == 0)
                return start - 1;
            return start;
        }
        if (i + 17 < n && d[i + 15] == 0 && d[i + 16] == 0 && d[i + 17] == 1) {
            std::size_t start = i + 15;
            if (start - 1 >= from && d[start - 1] == 0)
                return start - 1;
            return start;
        }
        i += 16;
    }
    for (; i + 3 <= n; ++i)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
            return i;
    return n;
}

static std::size_t simd_count(const std::uint8_t* d, std::size_t n) {
    std::size_t c = 0, p = 0;
    while (p < n) {
        std::size_t sc = simd_find(d, n, p);
        if (sc >= n)
            break;
        ++c;
        p = sc + 3;
    }
    return c;
}

int main() {
    auto buf = make_stream(5 * 1024 * 1024);
    const std::size_t iters = 200;
    double bytes = (double)buf.size() * iters;
    const std::uint8_t* d = buf.data();
    std::size_t n = buf.size();

    auto t0 = std::chrono::steady_clock::now();
    std::size_t sc = 0;
    for (std::size_t k = 0; k < iters; ++k)
        sc += scalar_count(d, n);
    auto t1 = std::chrono::steady_clock::now();
    double us_s = std::chrono::duration<double, std::micro>(t1 - t0).count();

    auto t2 = std::chrono::steady_clock::now();
    std::size_t sm = 0;
    for (std::size_t k = 0; k < iters; ++k)
        sm += simd_count(d, n);
    auto t3 = std::chrono::steady_clock::now();
    double us_v = std::chrono::duration<double, std::micro>(t3 - t2).count();

    printf("stream=%zu bytes\n", buf.size());
    printf(
        "scalar : %8.1f us/iter  %7.1f MB/s  (nals=%zu)\n",
        us_s / iters,
        bytes / (us_s * 1e-6) / 1e6,
        sc / iters
    );
    printf(
        "simd   : %8.1f us/iter  %7.1f MB/s  (nals=%zu)\n",
        us_v / iters,
        bytes / (us_v * 1e-6) / 1e6,
        sm / iters
    );
    printf("speedup: %.1fx\n", us_s / us_v);
    return 0;
}
