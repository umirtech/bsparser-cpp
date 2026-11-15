# bsparser-cpp

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
- **Spec-driven correctness**: syntax fields are parsed and validated directly
  against the specifications (H.264, H.265, H.266, AV1, VP8/VP9), including
  native picture-order derivation per the spec algorithms.



<!-- CI:RESULTS:START -->

## CI results

[![CI](https://github.com/umirtech/bsparser-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/umirtech/bsparser-cpp/actions/workflows/ci.yml)

<!-- CI:RESULTS:END -->



## Codec support

| Codec | Framing | Parsed |
|---|---|---|
| H.265/HEVC | Annex-B / length-prefixed | VPS/SPS/PPS, slice headers, SEI |
| H.264/AVC | Annex-B / length-prefixed | SPS/PPS, slice headers, SEI |
| H.266/VVC | Annex-B / length-prefixed | DCI/OPI/VPS/SPS/PPS/PH, slice headers (full H.266 §7.3) |
| AV1 | Annex-B OBU / low-overhead OBU | sequence + frame headers (full Annex A) |
| VP9 | IVF / RawFrame | frame headers |
| VP8 | IVF / RawFrame | frame headers |

## Container demuxing

Feed **muxed files directly** — the `demux/` layer auto-detects the container
and indexes zero-copy elementary-stream samples:

| Container | Supported codecs |
|---|---|
| MP4 / ISO-BMFF | H.264, HEVC, VVC, AV1, VP8, VP9 |
| MPEG-TS | H.264, HEVC, VVC |
| FLV | H.264, HEVC, VVC, AV1, VP8, VP9 |
| AVI | H.264, HEVC, VVC, AV1, VP8, VP9 |
| **MKV / WebM** | **H.264, HEVC, VVC, AV1, VP8, VP9** |
| Ogg | VP8, VP9, AV1, HEVC, AVC |
| MPEG-PS | H.264, HEVC |
| IVF | VP8, VP9 |

MP4 includes fragmented (`moof`) files and files whose `moov` atom is at the
end.

### Streaming / zero-copy demuxing

```cpp
#include <bsparser.hpp>
#include <demux/demuxer.hpp>

auto demuxer = bs::demux::Demuxer::init("input.mp4");   // path, fd, or span
for (auto& track : demuxer.tracks()) {
    auto state = bs::create_state(track.codec);
    bs::HevcParsedHandlers h{};
    // ...
    bs::demux::SampleView sample;
    while (track.hasNext() && track.next(sample)) {
        // sample.data is a zero-copy span into the mmap-backed file
        bs::parse(*state, sample.data, sample.framing, h);
    }
}
```

`Track` also supports `seek(sample_index)` and `seek_time(pts)`.  The
`SampleView::mmap_backed` flag tells you whether the span points into mapped
file memory or a reusable internal buffer.  MP4 support includes fragmented
(`moof`) files and files whose `moov` atom is at the end.

The CLI uses the same streaming API, parses samples chunk by chunk, and prints
progress while processing large files.

---

## H.264 motion vectors (opt-in)

The AVC path can optionally parse slice data and expose a dense 4x4 motion
field for motion-analysis consumers.  It is disabled unless both callbacks in
`bs::avc::MotionVectorSink` are installed, so normal header parsing is
unchanged.

```cpp
#include <utils/MotionBufferPool.h>

bs::utils::MotionBufferPool pool;
bs::AvcParsedHandlers handlers{};
handlers.motion_vectors.ctx = &pool;
handlers.motion_vectors.acquire = &bs::utils::MotionBufferPool::acquireThunk;
handlers.motion_vectors.ready = my_ready_callback; // const MotionVectorBufferView&
bs::parse(*state, sample, framing, handlers, length_size);
```

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
| `BS_BUILD_BENCHMARKS` | `OFF` | Build `the AVC benchmark tool` and `hash_avc_mv` (development only) |


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

See [docs/architecture.md](docs/architecture.md) for the full design and architecture.

## Project layout

```
bsparser.h            umbrella header (auto-selects C or C++20 API)
bsparser.hpp          unified C++20 API (bs::Codec / bs::State / bs::parse)
bitstream/            bit readers (map-based + zero-alloc sequential)
utils/                parser-local ObjectPool + zero-copy MotionBufferPool
parser/               framing, NAL dispatch, VPS/SPS/PPS/slice/SEI parsers
syntax/               immutable parsed models
capi/                 C public API (stable ABI, compiled into bs_capi)
cli/                  bs_cli: JSON / HTML report exporter
tools/                dev tools (bench, sample downloader, …)
tests/                unit, integration and fuzz tests
docs/architecture.md  architecture documentation
```


## Contributors

See the [Contributors](https://github.com/umirtech/bsparser-cpp/graphs/contributors)



## AI-Assisted Development

This project was developed with assistance from AI coding agents. All AI-assisted work was performed under the supervision and review of the project author, including architectural decisions, implementation, testing, debugging, and validation.
