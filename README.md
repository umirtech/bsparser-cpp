# bsparser

A fast, header-only **C++20** bitstream syntax parser for **H.265/HEVC**,
**H.264/AVC**, **H.266/VVC**, **AV1**, **VP9** and **VP8**, with a stable
**C API** for non-C++20 consumers and FFI. It parses the container/NAL layer
and syntax fields (parameter sets, slice/frame headers, SEI, OBUs) — it does
**not** decode pixels.

- **Zero-copy**: all NALs, OBUs and frames are views into your input buffer.
- **Two APIs, one library**: `#include "bsparser.h"` picks the C++20 templates
  for C++20 compilers, or the compiled C library otherwise.
- **Auto parameter-set management**: a `bs::State` stores VPS/SPS/PPS as they
  appear, so slice handlers resolve dependencies for you.
- **Verified**: HEVC/AVC accuracy is checked field-by-field against `ffmpeg`;
  VVC/AV1/VP9/VP8 are tested against streams produced by `ffmpeg`
  (`libvvenc`, `libaom-av1`, `libvpx`, `libvpx-vp9`).



<!-- CI:RESULTS:START -->

## CI results

[![CI](https://github.com/umirtech/bsparser-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/umirtech/bsparser-cpp/actions/workflows/ci.yml)

| Platform | Build | Tests (30) | Accuracy |
|---|---|---|---|
| linux-x64 | n/a | n/a | n/a |
| linux-arm64 | PASS | PASS | PASS |
| windows-x64 | n/a | n/a | n/a |
| windows-arm64 | n/a | n/a | n/a |

_Last run: `dfa5161`._

<!-- CI:RESULTS:END -->



## Codec support

| Codec | Framing | Parsed |
|---|---|---|
| H.265/HEVC | Annex-B / length-prefixed | VPS/SPS/PPS, slice headers, SEI |
| H.264/AVC | Annex-B / length-prefixed | SPS/PPS, slice headers, SEI |
| H.266/VVC | Annex-B / length-prefixed | NAL headers, DCI/OPI/VPS/SPS/PPS/PH, slice headers |
| AV1 | Annex-B OBU / low-overhead OBU | sequence + frame headers |
| VP9 | IVF | frame headers |
| VP8 | IVF | frame headers |

## Container demuxing

Feed **muxed files directly** — the `demux/` layer auto-detects the container
and extracts the elementary stream:

| Container | Supported codecs |
|---|---|
| MP4 / ISO-BMFF | H.264, HEVC, VVC, AV1, VP8, VP9 |
| MPEG-TS | H.264, HEVC, VVC |
| FLV | H.264, HEVC, VVC, AV1, VP8, VP9 |
| AVI | H.264, HEVC, VVC, AV1, VP8, VP9 |
| **MKV / WebM** | **H.264, HEVC, VVC, AV1, VP8, VP9** |
| IVF | VP8, VP9 |

```cpp
#include <bsparser.hpp>
#include <demux/demuxer.hpp>

auto es = bs::demux::demux(bytes);        // auto-detect container
if (es.ok) {
    auto state = bs::create_state(es.codec);
    bs::HevcParsedHandlers h{};
    // ...
    bs::parse(*state, es.bytes, es.framing, h);   // es.codec / es.framing chosen
}
```

The CLI does this automatically: `bs_cli input.mp4` demuxes and parses the
video track without any `--codec`/`--format` flags.

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

### Cross-architecture tests (Docker)

The Android NDK cross-builds compile fine but the binaries cannot run on the
CI host, so the common Android ABIs are exercised by running the real test
suite inside a lightweight Alpine container built for each target CPU:

| Docker platform | Maps to |
|---|---|
| `linux/amd64` | Android x86_64 |
| `linux/arm64` | Android arm64-v8a |
| `linux/arm/v7` | Android armeabi-v7a |

The arm/v7 leg also catches 32-bit portability bugs (e.g. `size_t` vs
`uint64_t` width mismatches) that the native amd64/arm64 matrix cannot.

Run all three locally (requires Docker with multi-arch support):

```sh
scripts/test-docker.sh linux/amd64
scripts/test-docker.sh linux/arm64
scripts/test-docker.sh linux/arm/v7
```

The same matrix runs in CI via `.github/workflows/docker-test.yml`.

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

`bs::parse` returns the number of units parsed (ignore it with `(void)` if you
don't need it) and is overloaded on the handler type — `BsNalHandlers` (raw NAL
views), the per-codec `*ParsedHandlers` (typed structs), or a
`bs::StructReport` (value snapshot). Parameter sets are stored in the `State`
automatically; call `state->clear()` when reusing a `State` across independent
streams.

Each codec has its own `State` codec, `*ParsedHandlers` type and framing mode:

```cpp
// VVC (Annex-B)
auto s = bs::create_state(bs::Codec::Vvc);
bs::VvcParsedHandlers h{};
h.pps = [](const bs::vvc::PictureParameterSet& pps) { /* ... */ };
bs::parse(*s, data, bs::NalFramingMode::AnnexB, h);

// AV1 (OBU framing)
auto s = bs::create_state(bs::Codec::Av1);
bs::Av1ParsedHandlers h{};
h.sequence_header = [](const bs::av1::SequenceHeader& sh) { /* ... */ };
bs::parse(*s, data, bs::NalFramingMode::Obu, h);

// VP9 / VP8 (IVF container)
auto s = bs::create_state(bs::Codec::Vp9);
bs::Vp9ParsedHandlers h{};
h.frame_header = [](const bs::vp9::FrameHeader& fh) { /* ... */ };
bs::parse(*s, data, bs::NalFramingMode::Ivf, h);
```

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
