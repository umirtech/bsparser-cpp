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

The consumer buffer is filled in place; reference pictures retain the lease
until they leave the MV-DPB.  See `syntax/avc_motion_vector.hpp` for the POD
buffer layout and `docs/architecture.md` for the supported subset.

Current support matrix:

- CABAC: 4:2:0, 4:2:2 and 4:4:4 progressive frame/field pictures, MBAFF
  frame pictures, spatial/temporal direct mode, multi-slice picture
  accumulation, zero-copy output, and I_PCM macroblocks (raw-sample skip plus
  arithmetic-engine re-initialisation) at 8 and 10 bits;
- CAVLC: slice data, residual-heavy P/B streams, field flags, MBAFF pairs,
  high-chroma (4:2:2/4:4:4) MBAFF at 8 and 10 bits, I_PCM sample skipping for
  4:2:0/4:2:2/4:4:4, static/skip paths, B-pyramid + multiple active
  references and multi-slice pictures;
- monochrome (`chroma_format_idc = 0`): luma-only CBP, residual, intra and
  I_PCM syntax, CABAC and CAVLC; output cells carry `MvPictureMonochrome`;
- separate colour planes (`separate_colour_plane_flag = 1`): each
  `colour_plane_id` is decoded with ChromaArrayType 0 macroblock syntax in its
  own MV-DPB and delivered as its own zero-copy grid.  The three buffers of an
  access unit carry the same POC/frame_num, `plane_count = 3`,
  `colour_plane_id = 0/1/2` and `MvPictureSeparateColourPlanes`; reference
  lists, marking and frame_num/POC bookkeeping are independent per plane;
- slice groups / FMO (`num_slice_groups_minus1 > 0`): map types 0-6
  (interleaved, dispersed, foreground, box-out, raster, wipe and explicit),
  `NextMbAddress` iteration, slice-group boundaries, CAVLC `mb_skip_run` and
  CABAC `end_of_slice_flag` over non-contiguous macroblock addresses.
  Macroblock availability is limited to the current slice and slice group;
- 4:2:2 residual syntax: 8-coefficient chroma DC blocks, 8 chroma AC blocks
  per component, CABAC and CAVLC;
- 4:4:4 residual syntax: three luma-like planes with per-plane
  Intra16x16 DC/AC, 4x4 and 8x8 transforms, the shared 4-bit CBP and the
  8x8 coded-block-flag context rules;
- field pictures (`frame_mbs_only_flag = 0`, `field_pic_flag = 1`) produce a
  field-sized grid; MBAFF frame pictures produce a frame-sized grid with
  `MvPictureMbaff`, and field-coded macroblocks carry `MvField` and
  frame-normalised vertical MVs;
- reference-picture management: sliding-window marking, adaptive MMCO
  operations 1-6 (`max_long_term_frame_idx_plus1`, long-term assignment,
  clear-all), IDR marking including `long_term_reference_flag`, and
  `no_output_of_prior_pics_flag` via a deferred-ready queue (default
  `defer_ready = true`) so suppressed pictures are never delivered;
- `ref_pic_list_reordering` for frame and field lists, including short-term
  `PicNum`/`FrameNumWrap` wrap arithmetic, long-term `LongTermPicNum`
  ordering, field-parity interleaving and the B-list duplicate swap;
- long-term references are kept after short-term entries in the order given
  by `LongTermFrameIdx`; PAFF field pairs count as one DPB frame and expose
  both field entries to field lists;
- weighted prediction does not affect MV syntax.

**Experimental feature.**  AVC motion-vector extraction is opt-in and
experimental: the `MotionVectorSink` API, the versioned buffer layout, and the
generated fixtures may change in future releases.  Treat extracted grids as
best-effort and re-validate against your own reference when upgrading the
module.

Residual-heavy streams that cannot yet be traversed report
`MotionVectorStatus::Truncated` so the caller can fall back instead of using a
partial field.  All colour formats, monochrome, separate colour planes and
slice groups (map types 0-6) are supported.

H.264 data partitioning (NAL unit types 2-4) is the one AVC slice syntax this
extractor does not reconstruct: the three partitions of a slice must be
combined before a macroblock grid exists.  A stream that uses it reports
`MotionVectorStatus::Unsupported` and delivers no partial grid.  SVC/MVC/3D
extension slices are likewise outside the AVC extractor's input contract; the
AVC-compatible base view of an MVC stream is parsed normally.

Performance (indicative)
------------------------
The extractor is opt-in: with no `MotionVectorSink` callbacks installed,
`bs::parse()` only walks NAL framing and parameter sets (~0.01 ms per 720p
frame).  With the sink installed, single-threaded measurements on an AMD
Ryzen 5 3600X with GCC 15.2 Release (`-O3`) over 4:2:0 streams give:

- 1280x720 CABAC crf 23: ~1.6 ms/frame (~615 fps)
- 1280x720 CAVLC crf 23: ~0.9 ms/frame (~1120 fps)
- 1920x1080 CABAC crf 23: ~3.5 ms/frame (~290 fps)
- 3840x2160 CABAC crf 23: ~14.5 ms/frame (~69 fps)

For reference, single-thread FFmpeg full decode of the same files on this
machine was ~2.0 / 4.0 / 17.1 ms per frame at 720p / 1080p / 4K.  The
extractor is now at or ahead of that while performing motion-vector
extraction only.

The hot path uses a table-driven CAVLC decoder, a 64-bit CABAC reservoir with
clz-based renormalisation, a 160-byte `MbData` layout, and SSE2/AVX2/NEON cell
fill/copy kernels (scalar fallback included).  SSE2 and AVX2 were executed on
x86-64; the NEON path is implemented and compile-checked but was not run on an
ARM device in this environment.  Deferred delivery is zero-copy:
cells are written once into the leased consumer buffer, so immediate and
deferred cost the same CPU time.  The deferred queue still needs one buffer
per queued picture until the next IDR decides
`no_output_of_prior_pics_flag`: roughly 120 MB at 1080p and 475 MB at 4K for
the benchmark streams, versus 10 MB and 40 MB immediate.  Set
`max_deferred_pictures` to bound the queue, or use immediate delivery when the
flag can be ignored.

Field/MBAFF and DPB-marking test fixtures:

- `tests/fuzz/corpus/avc_mbaff_cabac.h264` — x264 MBAFF CABAC, 15 frames;
- `tests/fuzz/corpus/avc_mbaff_cavlc.h264` — x264 MBAFF CAVLC, 5 frames;
- `tests/fuzz/corpus/avc_mbaff_{422,444}_cavlc*.h264` — 64x64 x264 CAVLC
  high-chroma MBAFF, 8 and 10 bits, with a mix of field-coded and frame-coded
  macroblock pairs (the `_frame` variants contain frame-coded pairs only and
  have FFmpeg-derived `.mv.txt` references).  Generated from true interlaced
  content so x264 actually selects field-coded pairs:
  `ffmpeg -f lavfi -i "gradients=size=64x64:rate=50:speed=0.4" -frames:v 24
  -vf "tinterlace=interleave_top" -r 25 -pix_fmt yuv422p -c:v libx264
  -profile:v high422 -x264-params
  "cabac=0:interlaced=1:bframes=2:b-pyramid=normal:keyint=12:min-keyint=1:ref=2:8x8dct=1:weightp=2:weightb=1:slices=1:scenecut=0"
  -f h264 avc_mbaff_422_cavlc.h264`
  (`yuv444p`/`high444` for 4:4:4; `yuv422p10le`/`yuv444p10le` with
  `8x8dct=0` for the 10-bit files).  The frame-only variants use
  `testsrc2=size=64x64:rate=25`, 12 frames and the same `cabac=0`/
  `interlaced=1` options; their `.mv.txt` files are produced with
  `the external reference generator <stream> > blocks.txt` followed by
  `tools/gen_avc_mv_grid.py blocks.txt 16 16`;
- `tests/fuzz/corpus/avc_paff_cavlc.h264` — synthetic PAFF CAVLC (x264 has no
  field-picture encoder), regenerated with
  `python3 tools/gen_avc_paff_fixture.py <output>`;
- `tests/fuzz/corpus/avc_mv_multiref_b.h264` — x264 High profile, 128x96,
  60 frames, 6 references, 4 B frames, B-pyramid; generated with
  `ffmpeg -f lavfi -i "testsrc2=size=128x96:rate=30:duration=2" -c:v libx264
  -profile:v high -pix_fmt yuv420p -refs 6 -bf 4 -b-pyramid normal -g 30
  -keyint_min 30 -sc_threshold 0
  -x264-params "ref=6:bframes=4:b-pyramid=normal:weightp=0:8x8dct=1"
  -f h264 avc_mv_multiref_b.h264`, with
  `tests/fuzz/reference/avc_mv_multiref_b.txt` produced by
  `tools/the external reference generator` + `tools/gen_avc_mv_grid.py`;
- `tests/fuzz/corpus/avc_mmco_cavlc.h264`,
  `tests/fuzz/corpus/avc_field_reorder_cavlc.h264`,
  `tests/fuzz/corpus/avc_field_mmco_cavlc.h264`,
  `tests/fuzz/corpus/avc_field_b_cavlc.h264`,
  `tests/fuzz/corpus/avc_frame_num_wrap_cavlc.h264` and
  `tests/fuzz/corpus/avc_frame_num_gap_cavlc.h264` — synthetic CAVLC streams
  covering MMCO 1-6 in frame and field pictures, IDR long-term marking,
  PAFF field/pair list reordering, B-field list ordering, `FrameNumWrap`
  across a `log2_max_frame_num` wrap, frame_num gaps (progressive, field-pair
  and POC0), per-slice reference lists and no-output-prior IDR suppression.
  Regenerate all of them with
  `python3 tools/gen_avc_mmco_fixtures.py tests/fuzz/corpus`.

`gaps_in_frame_num_value_allowed_flag` synthesizes non-existing reference
frames (8.2.5.2), including field-pair gaps; their cells are flagged
`MvRefNonExisting` and B-list omission follows the spec when POC is
undefined.  Multi-slice pictures carry a versioned per-slice reference table
(`MotionSliceReference`) plus an exact macroblock-to-slice map
(`MotionSliceRun`) so each cell resolves its own slice's `ref_idx`/POC
mapping, including non-contiguous FMO slice groups.  Use
`motionTo(view, x4, y4, targetPoc)` for exact per-slice projection.

High-chroma, monochrome and I_PCM fixtures:

- `avc_residual_422_cabac.h264`, `avc_residual_422_cavlc.h264` — 4:2:2
  high422, 8 frames, B frames, CABAC/CAVLC;
- `avc_residual_444_cabac.h264`, `avc_residual_444_cavlc.h264` — 4:4:4
  high444, 8 frames, B frames, CABAC/CAVLC;
- `avc_residual_444_cavlc_8x8.h264` — 4:4:4 CAVLC with 8x8 transform;
- `avc_residual_444_10bit_cabac.h264` — 64x64 10-bit 4:4:4 CABAC with B
  frames, exercising the high-bit-depth CABAC residual path;
- `avc_residual_444_10bit_qp60_cabac.h264` — the same at QP 60, above the
  8-bit QP ceiling, exercising the extended CABAC QP range;
- `avc_residual_422_pcm_cavlc.h264`, `avc_residual_444_pcm_cavlc.h264` —
  lossless CAVLC streams whose P pictures are entirely I_PCM, exercising the
  per-format I_PCM sample geometry;
- `avc_residual_422_pcm10_cavlc.h264`,
  `avc_residual_444_pcm10_cavlc.h264` — 16x16 10-bit I_PCM CAVLC streams,
  exercising the bit-packed I_PCM sample skip;
- `avc_residual_444_pcm_cabac.h264` — a lossless CABAC stream whose P pictures
  are I_PCM;
- `avc_pcm_cabac_{420,422,444}.h264` and
  `avc_pcm_cabac_{420,422,444}_10bit.h264` — 64x64 lossless CABAC streams
  with mixed I_PCM/inter macroblocks at 8 and 10 bits.  They are the primary
  coverage for CABAC I_PCM alignment/re-initialisation; each has an
  FFmpeg-derived `.mv.txt` reference;
- `avc_pcm_cavlc_420.h264` — the same source as a lossless 4:2:0 CAVLC
  stream with mixed I_PCM/inter macroblocks;
- `avc_mono_cabac.h264`, `avc_mono_cavlc.h264`, `avc_mono_10bit_cabac.h264` —
  monochrome motion/residual streams;
- `avc_mono_pcm_cabac.h264`, `avc_mono_pcm_cavlc.h264` — monochrome lossless
  streams with mixed I_PCM/inter macroblocks, exercising the luma-only
  ChromaArrayType 0 sample count;
- `avc_sep_planes_cavlc.h264` — synthetic CAVLC separate-colour-plane stream
  (`tools/gen_avc_sep_planes_fixture.py`), three planes in each of three
  access units, with different motion per plane and one I_PCM macroblock;
- `avc_b_pcm_cavlc.h264` — synthetic CAVLC B slice carrying an I_PCM
  macroblock with 4:2:0 chroma samples (`tools/gen_avc_b_pcm_fixture.py`);
  x264 disables B frames in lossless mode, so this branch has no natural
  encoder fixture;
- `avc_fmo_map{0..6}_{cavlc,cabac}.h264` — synthetic FMO streams
  (`tools/gen_avc_fmo_fixture.py`), one file per map type and entropy mode,
  each with an IDR picture and a P picture split across all slice groups.
  The `.fmo.txt` sidecars list the CAVLC coded macroblock addresses/MVDs.
- `avc_fmo_refs_map{0..6}_{cavlc,cabac}.h264` — the same slice-group maps
  with two reference P pictures and a target P picture whose slices use
  different list reordering and active counts, so the same `ref_idx` resolves
  to a different POC in each group.  The sidecars list the expected
  per-group POCs and the `mb_addr`-to-group map;
- `avc_fmo_mixed_i_p_map0_cavlc.h264` — one picture whose first slice group
  is P and second is I, proving that a later I slice marks only its own
  macroblocks intra and does not erase the P group decoded before it.

The motion references beside them (`tests/fuzz/reference/*.mv.txt`) are
FFmpeg-derived per-4x4 list-0 fields for the P pictures.  They can be
regenerated with `tools/the external reference generator.cpp` followed by
`tools/gen_avc_mv_grid.py`; the grid converter takes an optional
`<width4x4> <height4x4>` for streams whose intra/I_PCM cells export no
motion vectors.  The synthetic separate-plane and FMO fixtures are
regenerated with:

```sh
python3 tools/gen_avc_sep_planes_fixture.py tests/fuzz/corpus/avc_sep_planes_cavlc.h264
python3 tools/gen_avc_fmo_fixture.py tests/fuzz/corpus
```

The FMO generator writes both entropy modes for every map type, the
per-slice-reference variant, and a `.fmo.txt` sidecar beside each stream.  The mixed-PCM fixtures were generated
with:

```sh
ffmpeg -f lavfi -i "testsrc2=size=64x64:rate=25:duration=0.2" \
       -f lavfi -i "color=black:size=32x64:rate=25:duration=0.2,noise=alls=80:allf=t" \
       -filter_complex "[0:v][1:v]overlay=32:0" -pix_fmt <fmt> \
       -c:v libx264 -qp 0 -profile:v high444 \
       -x264-params "keyint=5:min-keyint=1:scenecut=0:bframes=2:cabac=1" \
       -f h264 <output>
```

where `<fmt>` is one of `yuv420p`, `yuv422p`, `yuv444p`, `yuv420p10le`,
`yuv422p10le`, `yuv444p10le`.  The monochrome fixtures use `gray`/`gray10le`
(or the same overlay without the colour planes) and `cabac=0` for CAVLC.

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

## Benchmarking

`the AVC benchmark tool` and `hash_avc_mv` are opt-in development targets. They are not
built by default and are not registered with ctest.

```sh
cmake -S . -B build-bench -DBS_BUILD_BENCHMARKS=ON
cmake --build build-bench -j
```

`the AVC benchmark tool` measures header/slice parsing, deferred MV delivery and
immediate MV delivery over the same Annex-B H.264 input:

```sh
./build-bench/the AVC benchmark tool <stream.h264> [iterations] [frames] [width] [height] [mode]
```

`mode` is `all` (default), `headers`, `deferred`, `immediate`, or the numeric
aliases `0`/`1`/`2`. The reported `MMB/s` is million macroblocks per second.

Generate representative streams with FFmpeg (only needed for benchmarking,
not for building or testing):

```sh
ffmpeg -y -f lavfi -i testsrc2=size=1280x720:rate=30:duration=5 \
    -c:v libx264 -preset medium -crf 23 -pix_fmt yuv420p -g 30 \
    -f h264 bench_720p.h264

ffmpeg -y -f lavfi -i testsrc2=size=1920x1080:rate=30:duration=5 \
    -c:v libx264 -preset medium -crf 23 -pix_fmt yuv420p -g 30 \
    -f h264 bench_1080p.h264

ffmpeg -y -f lavfi -i testsrc2=size=3840x2160:rate=30:duration=2 \
    -c:v libx264 -preset ultrafast -crf 23 -pix_fmt yuv420p -g 30 \
    -f h264 bench_4k.h264
```

Example runs:

```sh
./build-bench/the AVC benchmark tool bench_720p.h264 15 150 1280 720
./build-bench/the AVC benchmark tool bench_1080p.h264 5 150 1920 1080 2
./build-bench/the AVC benchmark tool bench_4k.h264 3 60 3840 2160 mv-deferred
```

### Deterministic hashing

`hash_avc_mv` folds every delivered MV picture (header fields, dense 4x4 cell
grid, per-slice reference tables and slice runs) into an FNV-1a 64-bit hash and
prints one fixed-width line per input:

```sh
./build-bench/hash_avc_mv bench_720p.h264 bench_1080p.h264 > hashes.txt

# Re-run after a parser change and compare.
./build-bench/hash_avc_mv bench_720p.h264 bench_1080p.h264 > hashes-new.txt
diff -u hashes.txt hashes-new.txt
```

Identical lines mean bit-identical MV output. Hashes are comparable between
builds on the same architecture and byte order.

---

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
