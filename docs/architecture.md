# bsparser-cpp — Architecture

bsparser-cpp is a **header-only C++20 bitstream syntax parser** for six video
codecs: **HEVC (H.265), AVC (H.264), VVC (H.266), AV1, VP9 and VP8**. It parses
the container / NAL / OBU layer and the syntax fields — parameter sets, slice
and frame headers, SEI, OBU headers — and derives the presentation-order signal
natively for every codec. It does **not** decode pixels.

## Entry points

There are two public faces, both reachable from the umbrella header
`bsparser.h`:

- **`bsparser.hpp`** — the C++20 header-only API. One header aggregates the
  whole library; every implementation lives in headers and is compiled at the
  call site, so there is no library to link.
- **`capi/bs_capi.h`** — a plain C ABI (opaque handles, integer enums,
  `size_t`, C structs). It is built once into the `bs_capi` static/shared
  library and is meant for C consumers, pre-C++20 toolchains and FFI from
  other languages.

`bsparser.h` selects the right one automatically: C++20 compilers get the
templates unless `BS_USE_C_API` / `BS_FORCE_C_API` is defined; everything else
falls back to the C API. Both run the same parser code — the C API is a thin
bridge over the same core and exposes all six codecs.

## The unified API (`bsparser.hpp`)

The C++ front end is intentionally small:

```cpp
enum class Codec : std::uint8_t { Hevc, Avc, Vvc, Av1, Vp9, Vp8 };

std::unique_ptr<bs::State> state = bs::create_state(bs::Codec::Hevc);
```

- **`bs::Codec`** picks the codec path.
- **`bs::State`** is an opaque, move-only handle. It owns the codec's
  **parameter-set manager** (auto-stored as VPS/SPS/PPS NALs appear, so slice
  handlers can resolve their dependencies) and the codec's **POC tracker**.
  There is no global state — every `State` is independent, and several of
  mixed codecs can coexist. `state->clear()` drops the stored sets and resets
  the POC tracker before reusing the handle on a new stream.
- **`bs::parse(state, data, framing, handlers)`** frames and parses a whole
  buffer. It is overloaded on the handler type (below) and validates that the
  handler matches the `State`'s codec.

Framing is chosen with `bs::NalFramingMode`:
`AnnexB`, `LengthPrefixed`, `Obu` (AV1) and `Ivf` (VP8/VP9).

### Handler flavours

The parsed data reaches the caller in one of three ways:

1. **Raw NAL handlers** (`bs::BsNalHandlers` / `avc::NalHandlers`) — a
   function pointer per NAL type, receiving a zero-copy `NalUnit` view.
2. **Typed parsed handlers** — one struct per codec
   (`HevcParsedHandlers`, `AvcParsedHandlers`, `VvcParsedHandlers`,
   `Av1ParsedHandlers`, `Vp9ParsedHandlers`, `Vp8ParsedHandlers`) with
   callbacks that receive the fully-parsed structs (VPS/SPS/PPS, slice
   headers, SEI, sequence/frame headers). All slots are optional and
   NULL-safe.
3. **`bs::StructReport`** — a value-copied snapshot of every parameter set
   seen during the parse, for one-shot tooling or FFI instead of callbacks.

### Picture order

Every codec's headers carry a presentation-order signal, derived natively
from the spec algorithm:

| Codec | Signal | Field |
|---|---|---|
| HEVC | POC (H.265 §8.3.1) | `SliceSegmentHeader::derived_poc` |
| AVC | POC (H.264 §8.2.1) | `avc::SliceHeader::derived_poc` |
| VVC | POC (H.266 §8.3.1) | `vvc::SliceHeader::derived_poc` + `vvc::PictureHeader::derived_poc` |
| AV1 | `order_hint` (§5.9.2) | `av1::FrameHeader::order_hint`, plus `presentation_order` (decode index) |
| VP9 / VP8 | decode order | `FrameHeader::presentation_order` |

The POC state machines live in `parser/hevc_poc.hpp`, `avc_poc.hpp` and
`vvc_poc.hpp`. They are owned by `State` and updated automatically by the
dispatcher, so `derived_poc` stays correct across chunked `bs::parse()` calls
on the same `State`. `State` exposes the trackers directly (`state->hevc_poc()`,
`state->avc_poc()`, `state->vvc_poc()`) for callers that want to drive the
derivation themselves.

## Codec coverage

| Codec | Framing | Parsed |
|---|---|---|
| HEVC | Annex-B / length-prefixed | VPS/SPS/PPS (incl. RExt/ML/3D/SCC extensions), slice headers, SEI |
| AVC | Annex-B / length-prefixed | SPS/PPS, slice headers, SEI |
| VVC | Annex-B / length-prefixed | DCI/OPI/VPS/SPS/PPS, picture headers, slice headers (incl. embedded PH) |
| AV1 | Annex-B OBU / low-overhead OBU | sequence headers, frame headers |
| VP9 / VP8 | IVF | uncompressed frame headers |

VVC and AV1 parse the header fields needed for syntax and POC (dimensions,
chroma, order hints, POC configuration, …); the deepest RBSP tables (VVC
scaling lists / RPL / ALF-LMCS, full AV1 tile coding) are not modelled.

## How parsing is organised

The library is a strict stack of layers, each with one job, and every layer is
**zero-copy** — all spans point into the caller's buffer:

```
byte stream → framing → unit header → EBSP→RBSP → syntax fields
```

- **Framing** (`parser/nal_framer.hpp`, `obu_framer.hpp`, `ivf_framer.hpp`):
  splits the raw bytes into NAL units / OBUs / frames. Annex-B start codes,
  length-prefixed NALs, AV1 OBUs (Annex-B and low-overhead) and IVF frames.
  Codec-agnostic and shared.
- **Unit layer** (`parser/*_nal_unit_parser.hpp`): unpacks the codec's NAL /
  OBU header (2-byte HEVC/VVC, 1-byte AVC, 1-byte OBU).
- **Bit readers** (`bitstream/`):
  - `rbsp_bitstream_reader.hpp` — map-based RBSP reader (skips `00 00 03`
    emulation bytes via a precomputed map; O(1) random access). Used for
    parameter sets and SEI, which scan backwards or repeatedly.
  - `rbsp_reader.hpp` — zero-allocation sequential reader, used for slice and
    picture headers, which only read forward. This is the performance path.
  - `plain_bit_reader.hpp` — MSB-first reader without emulation-prevention,
    for VP8/VP9.
  - `boolean_decoder.hpp` — AV1's boolean arithmetic decoder.
- **Syntax parsers** (`parser/*.hpp`): one per syntax structure
  (VPS/SPS/PPS/slice/SEI, PH, OBU headers), producing **immutable models**
  (`syntax/*.hpp`) with no internal pointers back into the buffer.

The per-codec dispatch (`parser/hevc_nal_parser.hpp`, `avc_nal_parser.hpp`,
plus the OBU/IVF paths) routes each unit by its type to the right parser and
then to the corresponding handler. Slice headers are parsed in **two passes**:
first the `pps_id` is read from the front of the header, the SPS/PPS chain is
resolved through the `State`, then the full header is parsed with the resolved
parameter sets. This is also where `derived_poc` is stamped, immediately after
the header parse.

## Container demuxing (`demux/`)

A separate layer lets callers feed **muxed files** directly instead of
pre-extracting an elementary stream. `demux::sniff()` detects the container and
`demux::demux()` extracts the first video track into an `ElementaryStream`
(codec, framing, bytes, dimensions):

- MP4/ISO-BMFF (`mp4_demuxer.hpp`) — walks `moov/trak/mdia/stbl` and rebuilds
  a self-contained elementary stream (parameter sets prepended from `avcC` /
  `hvcC` / `vvcC`).
- MPEG-TS (`ts_demuxer.hpp`) — PAT/PMT to the first video stream, PES
  stripped.
- FLV (`flv_demuxer.hpp`), AVI (`avi_demuxer.hpp`), MKV/WebM
  (`mkv_demuxer.hpp`) — extract the video track into Annex-B / OBUs / IVF.
- IVF passes through as the VP8/VP9 elementary stream.

The demux layer is deliberately limited: one video track, no fragmented MP4,
no laced MKV blocks, no audio/subtitles.

## C API (`capi/`)

`capi/bs_capi.cpp` bridges the C ABI to the same `bs::State` / `bs::parse`
core and compiles into `bs_capi` (static by default, shared with
`BS_CAPI_SHARED=ON`). It covers **all six codecs** (HEVC / AVC / VVC / AV1 /
VP9 / VP8). Usage mirrors the C++ API:

```c
BsState* st = bs_state_create(BS_CODEC_HEVC);

BsHevcHandlers h = {0};
h.sps   = on_sps;    /* const BsHevcSequenceParameterSet* */
h.slice = on_slice;  /* const BsHevcSliceSegmentHeader* (incl. derived_poc) */
bs_parse_hevc(st, data, size, BS_FRAMING_ANNEX_B, 4, &h);

/* or a flat report of per-NAL entries */
BsReport* r = bs_parse_report(NULL, data, size, BS_FRAMING_ANNEX_B, 4);
bs_report_destroy(r);
bs_state_destroy(st);
```

The same surface scales to the other codecs: `BsVvcHandlers` / `BsAv1Handlers` /
`BsVp9Handlers` / `BsVp8Handlers` with `bs_parse_vvc` / `bs_parse_av1` /
`bs_parse_vp9` / `bs_parse_vp8` (framing is Annex-B or length-prefixed for
VVC, OBU for AV1, IVF for VP9/VP8). `BsNalHandlers` gained a catch-all `nal`
callback that fires for every unit on every codec, in addition to the typed
VPS/SPS/PPS/SEI/slice slots; `BsFramingMode` carries the matching framing
(`BS_FRAMING_OBU` / `BS_FRAMING_IVF`). Passing a NULL state to
`bs_parse_report` auto-detects the codec (IVF fourcc, VP8/VP9 key-frame
markers, OBU header, or a leading-NAL parameter-set scan for Annex-B
HEVC/AVC/VVC); VCL-only / raw streams may need an explicit codec.

Structured data crosses the ABI as plain C structs — mirror types for the
parsed parameter sets and slice headers, generated by
`tools/gen_c_structs.py` — plus callback sets (`BsHevcHandlers` /
`BsAvcHandlers` / `BsVvcHandlers` / `BsAv1Handlers` / `BsVp9Handlers` /
`BsVp8Handlers`), a per-NAL `BsReport`, and a collected `BsStructReport`
snapshot (whose `BsStructKind` covers every codec's parameter-set kinds).
SEI messages are delivered one at a time via `BsSeiCallback` (payload type +
bytes), since SEI is heterogeneous and view-backed. Errors surface through
`bs_get_last_error()`; no exceptions or STL types cross the boundary.

## CLI (`cli/`)

`bs_cli` reads a stream (or a muxed file, using the demux layer) and exports a
structured report as **JSON** or a self-contained **HTML** viewer with
filtering. Reports show the per-unit syntax summary and, for each codec, the
picture-order values (`derived_poc` / `order_hint` / `presentation_order`).

## Tests

- Per-codec tests (`tests/{hevc,avc,vvc,av1,vp8,vp9}_test.cpp`) parse bundled
  sample streams from `tests/fuzz/corpus/`.
- `tests/poc_test.cpp` verifies picture-order derivation for all six codecs
  against spec-computed expectations.
- `tests/unified_test.cpp` exercises the `Codec`/`State`/`parse` API;
  `tests/capi_test.c` covers the C ABI; `tests/demux_test.cpp` covers all
  container paths.
- `tests/hevc_hdr_test.cpp` / `avc_hdr_test.cpp` and `tools/verify_c.c`
  compare parsed HDR/SPS/VUI/SEI fields against pre-generated expected-value
  reference files (`tests/fuzz/reference/*.txt`).
- `tests/real/` holds real-world samples with a manifest; `tools/validate_real.py`
  runs `bs_cli` over them, and `tools/compare_report.py` compares the reports
  against pre-generated JSON reference files (`tests/real/reference/`).
  References are created once with `tools/compare_report.py --generate`.
- `tests/fuzz/` provides a libFuzzer target and a standalone driver.

## Directory map

```
bsparser.h                umbrella header (selects C or C++20 API)
bsparser.hpp              unified C++20 API (bs::Codec / bs::State / bs::parse)
bitstream/                bit readers (map-based, zero-alloc, plain, boolean)
parser/                   framing, dispatch, syntax parsers, POC trackers,
                          parameter-set managers
syntax/                   immutable parsed models
demux/                    MP4 / TS / FLV / AVI / MKV demuxers
capi/                     C public API (compiled into bs_capi)
cli/                      bs_cli: JSON / HTML report exporter
tools/                    generators, validators, bench, reference tooling
tests/                    unit, integration, fuzz tests + real samples
docs/architecture.md      this document
```

## Design notes

- **Zero-copy throughout** — no layer copies the input; parsed models are
  value types, NAL/OBU views point into the caller's buffer.
- **No global state** — parameter sets live per-`State`, so mixed-codec
  consumers can run side by side without ID collisions.
- **Performance path** — slice/picture headers use the allocation-free
  `RbspReader`; only parameter sets and SEI pay for the RBSP map.
- **Spec-native POC** — the presentation-order signal comes from the spec
  algorithms, not from external tools; the derivation is exercised by
  `poc_test.cpp` and by the reference-based validation.