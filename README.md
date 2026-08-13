# bsparser

A fast, header-only **C++20** bitstream syntax parser for **H.265/HEVC** and
**H.264/AVC**, with a stable **C API** for non-C++20 consumers and FFI. It
parses the container/NAL layer and every syntax field (VPS/SPS/PPS, slice
headers, SEI) — it does **not** decode pixels.

- **Zero-copy**: all NALs and parameter sets are views into your input buffer.
- **Two APIs, one library**: `#include "bsparser.h"` picks the C++20 templates
  for C++20 compilers, or the compiled C library otherwise.
- **Auto parameter-set management**: a `bs::State` stores VPS/SPS/PPS as they
  appear, so slice handlers resolve dependencies for you.
- **Verified**: accuracy is checked field-by-field against `ffmpeg` output.

---

## Build

Requires CMake ≥ 3.20 and a C++20 compiler (GCC, Clang, MSVC).

```sh
cmake -B build
cmake --build build
ctest --test-dir build        # run the test suite
```

Useful CMake options:

| Option | Default | Meaning |
|---|---|---|
| `BS_ENABLE_TRACE` | `OFF` | Compile in verbose parser trace logging |
| `BS_CAPI_SHARED` | `OFF` | Build the C API as a shared library |
| `BS_ENABLE_FUZZING` | `ON` | Build the fuzz harnesses |
| `BS_ENABLE_SANITIZERS` | `ON` | ASan/UBSan on the standalone fuzz driver |

---

## Quick start (C++20)

```cpp
#include <bsparser.hpp>
#include <cstdio>
#include <vector>

int main()
{
    // read a whole Annex-B file into memory
    std::vector<std::uint8_t> data = /* your bytes */;

    auto state = bs::create_state(bs::Codec::Hevc);

    bs::HevcParsedHandlers h{};
    h.sps = [](const bs::SequenceParameterSet& sps) {
        std::printf("SPS: %ux%u\n", sps.pic_width_in_luma_samples,
                                    sps.pic_height_in_luma_samples);
    };
    h.slice = [](const bs::SliceSegmentHeader& sh) {
        std::printf("slice: type=%d qp_delta=%d\n",
                    static_cast<int>(sh.slice_type), sh.slice_qp_delta);
    };

    bs::parse(*state, data, bs::NalFramingMode::AnnexB, h);
}
```

`bs::parse` returns the number of NALs parsed (ignore it with `(void)` if you
don't need it) and is overloaded on the handler type — `BsNalHandlers` (raw NAL
views), `HevcParsedHandlers` / `AvcParsedHandlers` (typed structs), or a
`bs::StructReport` (value snapshot). Parameter sets are stored in the `State`
automatically; call `state->clear()` when reusing a `State` across independent
streams.

---

## Quick start (C)

Link `bs_capi` (static or shared) and use the C API:

```c
#include <bsparser.h>

static void on_sps(void* ctx, const BsHevcSequenceParameterSet* sps) {
    /* sps->pic_width_in_luma_samples, ... */
}

int main(void) {
    BsState* st = bs_state_create(BS_CODEC_HEVC);
    BsHevcHandlers h = {0};
    h.sps = on_sps;
    bs_parse_hevc(st, data, size, BS_FRAMING_ANNEX_B, 4, &h);
    bs_state_destroy(st);
    return 0;
}
```

The C API is a plain C ABI (opaque handle, integer enums, `size_t`, no STL or
exceptions); errors surface via `bs_get_last_error()`.

---

## CLI

`bs_cli` reads a stream and exports a structured report:

```sh
./build/bs_cli input.hevc                     # JSON to stdout (auto codec)
./build/bs_cli input.h264 --codec avc --out report.html
./build/bs_cli input.hevc --format length --length-size 4
```

Output formats: JSON or a self-contained HTML viewer.

---

## Performance

Slice-header parsing uses a zero-allocation sequential bit reader and skips
building the full EBSP→RBSP map that parameter-set parsing requires. On a 5 MB
HEVC stream:

```
                    before         after
  typed-slice       25.3 ms        17.3 ms     −32%
  typed-full        31.9 ms        24.9 ms     −22%
  c-api-full        39.1 ms        32.5 ms     −17%
```

See [docs/architecture.md](docs/architecture.md) for the full design and the
reasoning behind the two-reader architecture.

---

## Project layout

```
bsparser.h            umbrella header (auto-selects C or C++20 API)
bsparser.hpp          unified C++20 API (bs::Codec / bs::State / bs::parse)
bitstream/            bit readers (map-based + zero-alloc sequential)
parser/               framing, NAL dispatch, VPS/SPS/PPS/slice/SEI parsers
syntax/               immutable parsed models
capi/                 C public API (stable ABI, compiled into bs_capi)
cli/                  bs_cli: JSON / HTML report exporter
tools/                dev/verification tools (bench, verify_c, …)
tests/                unit, integration and fuzz tests
docs/architecture.md  architecture documentation
```
