# bsparser — Architecture

A header-only C++20 bitstream syntax parser for **H.265 / HEVC** and **H.264 /
AVC** (no decoding, no pixel output). It is organised as strict layers, each
with one responsibility:

```
 byte stream  →  NAL framing  →  NAL header  →  EBSP→RBSP  →  syntax fields
```

Every layer is zero-copy: all spans reference the original input buffer. The
framing layer, the RBSP bit reader and the logging layer are codec-agnostic
and are shared by both the HEVC and the AVC stack.

The whole library is surfaceable through a single header, `bsparser.h`, which
transparently selects the right implementation for the build: the C++20
header-only templates (`bsparser.hpp`) when a C++20 compiler is available, or
the C public API (`bs_capi.h`, backed by the compiled `bs_capi` library)
otherwise — or whenever `BS_USE_C_API` / `BS_FORCE_C_API` is defined. This is
the recommended entry point for all consumers. The C++20 front end itself is
documented in "Unified public API" below. The previous
`dispatch_nals` / `avc::dispatch_nals` entry points remain available and
unchanged.

> **Header-only is available to C++20 users only, and that is by design.**
> The core implementation is written in C++20 (it uses `std::span`,
> `[[nodiscard]]`, etc.), so a consumer's compiler must support C++20 to
> compile the headers. C and pre-C++20 consumers therefore cannot use the
> library header-only — they must link the prebuilt `bs_capi` static/shared
> library (the C API) instead. There is no way around this: "header-only"
> means *your* compiler compiles the headers, so it must understand C++20.

---

## Unified public API (`bsparser.hpp`)

`bsparser.hpp` is the recommended entry point. It exposes a `Codec` enum to
select the codec path, an opaque `State` that auto-manages parameter sets, and a
single `bs::parse` overload set.

### Codec selection

```cpp
enum class Codec : std::uint8_t { Hevc, Avc };
```

The user picks the codec path with this enum and supplies an opaque `State`.

### Opaque State

```cpp
std::unique_ptr<bs::State> state = bs::create_state(bs::Codec::Hevc);
```

`bs::State` is opaque: it hides `detail::StateImpl`, which owns a single
codec-specific `ParameterSetManager`. There is **no global parameter-set store**
— each `State` is independent, so many `State` instances (even of mixed codecs)
can coexist without ID collisions. A `State` is move-only (copy is deleted).

### Unified parse + automatic parameter-set management

```cpp
std::size_t parsed = bs::parse(
    state,                            // bs::State&
    data,                             // std::span<const std::uint8_t>
    bs::NalFramingMode::AnnexB,
    handlers);                        // bs::BsNalHandlers  (or bs::avc::NalHandlers)
```

`bs::parse` is overloaded on the handler type (`BsNalHandlers` ⇒ HEVC,
`avc::NalHandlers` ⇒ AVC) and validates that the `State`'s codec matches.
Internally it frames the stream and, for every parameter-set NAL (VPS/SPS/PPS
for HEVC; SPS/PPS for AVC), parses and stores the set into the `State`
automatically. Slice handlers therefore resolve their dependencies through the
`State` instead of maintaining their own manager:

```cpp
// inside a slice handler
auto* sets = state.hevc_sets();        // nullptr for an AVC state
auto  resolved = sets->resolve_pps(0); // PPS → SPS → VPS chain
```

`state.hevc_sets()` / `state.avc_sets()` return the codec manager, or `nullptr`
when the `State` was created for the other codec.

### Reusing a State across streams

Reusing one `State` across independent streams keeps stale parameter sets, so
IDs from stream A can shadow stream B. Call `state.clear()` before parsing a new
stream:

```cpp
s->clear();
bs::parse(*s, streamB, bs::NalFramingMode::AnnexB, handlers);
```

### Typed parameter-set callbacks

Beyond the raw-NAL `BsNalHandlers` / `avc::NalHandlers`, `bs::parse` is also
overloaded to deliver the **fully-parsed parameter-set structs** (with all
nested sub-structs intact) through typed function pointers:

```cpp
bs::HevcParsedHandlers h{};
h.sps = [](const bs::SequenceParameterSet& sps) { /* ... */ };
h.pps = [](const bs::PictureParameterSet& pps) { /* ... */ };
h.vps = [](const bs::VideoParameterSet& vps)   { /* ... */ };
h.sei = [](const bs::ParsedSei& sei) {
    for (const auto& m : sei.view.messages) { /* m.payload_type, m.payload */ }
};
h.slice = [](const bs::SliceSegmentHeader& sh) { /* sh.slice_type, sh.slice_qp_delta, ... */ };
bs::parse(*state, data, bs::NalFramingMode::AnnexB, h);

// AVC analogue
bs::AvcParsedHandlers ah{};
ah.sps = /* const avc::SequenceParameterSet& */;
ah.pps = /* const avc::PictureParameterSet& */;
bs::parse(*state, data, bs::NalFramingMode::AnnexB, ah);
```

The handler structs are default-constructed empty and NULL-safe: any unset
slot is simply skipped. They mirror the typed callbacks exposed by the C API
(see below) and are the preferred way to walk parsed parameter sets without
keeping the `State` alive.

### Collected StructReport

When callbacks are inconvenient (e.g. one-shot tooling, or FFI), a single
`bs::StructReport` snapshot can be filled instead:

```cpp
bs::StructReport report{};
bs::parse(*state, data, bs::NalFramingMode::AnnexB, report);
// report.hevc_vps / report.hevc_sps / report.hevc_pps
// report.avc_sps  / report.avc_pps   (value-copied, codec-dependent)
```

The report is codec-agnostic: the `State`'s codec selects which vectors are
populated. Every entry is a copy, so the report outlives the `State`.

### Flow

```
bs::parse(state, bytes, mode, handlers)
   │  overload chosen by handler type (Hevc ⇔ BsNalHandlers)
   ▼
NalFramingMode → AnnexBNalIterator / LengthPrefixedNalIterator   nal_framer.hpp
   ▼
parse_nal_unit(span) → NalUnit / avc::NalUnit
   │
   ├─ store_hevc/avc_parameter_set()  →  State's ParameterSetManager (auto)
   ▼
handlers.vps/sps/pps/sei/slice(nal)        caller-provided callback
   │
   ▼
RbspBitstreamReader(ebsp) → syntax parsers → models
```

### C public API (`capi/`)

For consumers that cannot build the C++20 templates (plain C, older
compilers, or FFI from other languages), `capi/bs_capi.cpp` is a single
translation unit that bridges this ABI to `bs::State` / `bs::parse`. It is
compiled into the `bs_capi` target, which can be built as a **STATIC**
(default) or **SHARED** (`-DBS_CAPI_SHARED=ON`) library. Structured data is
exchanged through plain C structs (no JSON / serialisation), so the usage
mirrors the C++ handler-based API:

```c
BsState* st = bs_state_create(BS_CODEC_HEVC);

/* callback dispatch — each callback receives a BsNalUnit view */
BsNalHandlers h = {0};
h.sps   = on_sps;     /* const BsNalUnit* nal */
h.slice = on_slice;
bs_parse(st, data, size, BS_FRAMING_ANNEX_B, 4, &h);

/* or a structured report as an array of BsNalEntry structs */
BsReport* r = bs_parse_report(NULL, data, size,
                              BS_FRAMING_ANNEX_B, 4); /* auto-detect codec */
for (size_t i = 0; i < r->nal_count; ++i) {          /* iterate r->nals[i] */
    /* r->nals[i].nal_type_name, .offset, .size, .is_vcl, ... */
}
bs_report_destroy(r);
bs_state_destroy(st);
```

#### Typed parameter-set structs

Beyond the raw `BsNalUnit` view, the C API also delivers the fully-parsed
parameter-set structs (the `BsHevc*` / `BsAvc*` mirrors generated by
`tools/gen_c_structs.py`, with all nested sub-structs intact). These mirror
`bs::HevcParsedHandlers` / `bs::AvcParsedHandlers`:

```c
BsHevcHandlers h = {0};
h.sps = on_hevc_sps;   /* const BsHevcSequenceParameterSet* */
h.pps = on_hevc_pps;
bs_parse_hevc(st, data, size, BS_FRAMING_ANNEX_B, 4, &h);

/* or a collected snapshot (C-side bs::StructReport equivalent) */
BsStructReport* r = bs_parse_struct_report(NULL, data, size,
                                           BS_FRAMING_ANNEX_B, 4);
for (size_t i = 0; i < r->count; ++i) {        /* r->entries[i].kind / .data */
    const BsHevcSequenceParameterSet* sps =
        (const BsHevcSequenceParameterSet*)r->entries[i].data;
    /* ... */
}
bs_struct_report_destroy(r);
```

The typed callback receives an owned struct valid only for the call's
duration; the library frees it (and its heap buffers) afterwards. The struct
report owns every entry and is released with `bs_struct_report_destroy`.
Both are generated alongside the mirror: `bs_structs_conv.hpp` (C++→C
conversion) and `bs_structs_free.hpp` (per-struct `bs_free_*` helpers).

**SEI** is delivered differently: a SEI NAL carries zero or more messages of
heterogeneous payload types backed by views into the RBSP buffer, so it is not
mirrored as a single C struct. Instead each message is delivered on its own via
`BsSeiCallback` (`payload_type`, `payload` bytes, `payload_size`). The C++
`sei` handler receives the whole container (`bs::ParsedSei::view.messages` for
HEVC, `bs::avc::ParsedSei::messages` for AVC) and may iterate at leisure.

**Slice headers** (`BsHevcSliceSegmentHeader` / `BsAvcSliceHeader`) are
delivered as the fully-parsed mirror struct for every VCL NAL. Because the
slice's referenced `pps_id` lives inside the header itself, the dispatch does a
two-pass parse: it reads just the `pps_id` from the front of the RBSP, resolves
the SPS/PPS through the state's manager, then re-parses the full header with
the resolved parameter sets. If resolution fails the slice is skipped (no
callback). The C++ `slice` handler receives `const bs::SliceSegmentHeader&` /
`const bs::avc::SliceHeader&`.

Both passes use the lightweight, allocation-free `RbspReader` (see the bit
reader section below): a slice header only ever reads forward, so it never pays
for the `RbspBitstreamReader` logical-map construction that PS/SEI parsing
requires. This removes one full-payload scan + heap allocation per slice.

The ABI is plain C: an opaque `BsState*` handle, integer enums, `size_t`, and
`char*` strings (only the last-error message); no exceptions or STL types
cross the boundary (errors surface via `bs_get_last_error`). `bs_capi.h` is
installed to `include/` and the library to `lib/`.

#### Performance

The C++20 templates are an *implementation detail* of the library: they are
compiled exactly once, into `bs_capi`. C / pre-C++20 consumers never include
them — they only `#include "bsparser.h"` (which selects the C API) and link
the library. There is **no runtime penalty**:

- `bs_parse` hands each NAL to the caller as a `const BsNalUnit*` view that
  points directly into the input buffer — zero-copy, identical to the C++
  `NalUnit` handler path. The only overhead per NAL is one indirect callback
  and a `thread_local` read, negligible next to the bit-level parsing.
- `bs_parse_report` builds a `BsReport` of `BsNalEntry` structs in one pass
  (the `nal_type_name` fields point at static strings), allocating a single
  array that the caller frees with `bs_report_destroy`. No per-NAL string
  building or serialisation.
- The bit reader and syntax parsers execute inside the already-compiled,
  optimized C++20 library, so consumers get the same code regardless of their
  own compiler/standard. (Both paths still auto-store parameter sets, same as
  the C++ API — a framer-only entry can be added if a consumer wants to skip
  that parse cost entirely.)

---

## Performance

The parser is optimised around the two facts that dominate real-world HEVC/AVC
streams:

1. **Most NALs are slices, and slice *headers* are small** — yet the payload a
   slice NAL carries is dominated by slice *data* the syntax parser never
   reads.
2. **The slice-header parser only reads forward** — it never needs random byte
   access, a total bit count, or `more_rbsp_data()`.

The `RbspBitstreamReader`'s constructor builds a full `logical_to_ebsp_` map by
scanning + allocating over the *entire* NAL payload. That map is genuinely
needed only by the parsers that call `more_rbsp_data()` / `find_last_one_bit()`
(backward scans): the VPS/SPS/PPS extension loops and AVC SEI.

Slice-header dispatch therefore uses the **zero-allocation `RbspReader`** (an
inline byte cursor with emulation-prevention skipping) for both passes of the
`pps_id` + full-header parse. Measured on a 5 MB HEVC stream:

```
                    before         after
  typed-slice       25.3 ms        17.3 ms     −32%
  typed-full        31.9 ms        24.9 ms     −22%
  c-api-full        39.1 ms        32.5 ms     −17%
```

The same change was applied to AVC by templating `parse_slice_header` and its
helpers on the reader type (accuracy is verified against `ffmpeg`-generated
references for both codecs).

### Design rule

> Use `RbspReader` for any parser that only reads forward and never calls
> `more_rbsp_data()` / `bits_remaining()`. Use `RbspBitstreamReader` only where
> a backward scan or random byte access is actually required.

---


## 1. Layer overview

```
┌──────────────────────────────────────────────────────────────────────────┐
│  UNIFIED API   bsparser.hpp                                              │
│  enum class Codec { Hevc, Avc };                                         │
│  bs::State (opaque) · bs::create_state() · bs::parse()                   │
│  → selects codec path, auto-manages parameter sets per State             │
└──────────────────────────────────┬───────────────────────────────────────┘
                                    │  raw Annex-B bytes (either codec)
                                    ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                     ENTRY POINTS / CLIENTS                                │
│   tests/main.cpp        tests/avc_test.cpp         tests/fuzz/fuzz_*.cpp  │
│   (HEVC demo)           (AVC demo, H.264 streams)  (fuzz harness +        │
│                                                   standalone driver)      │
│   tests/unified_test.cpp  (exercises the unified Codec/State API)         │
└──────────────────────────────────┬───────────────────────────────────────┘
                                    │  raw Annex-B bytes (either codec)
                                    ▼
┌──────────────────────────────────────────────────────────────────────────┐
│  DISPATCH LAYER                                                           │
│  HEVC:  parser/hevc_nal_parser.hpp        AVC:  parser/avc_nal_parser.hpp   │
│                                                                           │
│  BsNalHandlers (function-pointer callbacks per NAL type)                  │
│  dispatch_nal() · parse_and_dispatch_nal() · dispatch_framed_nals()       │
│  dispatch_annex_b() · dispatch_length_prefixed() · dispatch_nals()        │
└──────────────────────────────────┬───────────────────────────────────────┘
                                   │  one complete NAL span
                                   ▼
┌──────────────────────────────────────────────────────────────────────────┐
│  FRAMING LAYER           parser/nal_framer.hpp  (shared)                  │
│                                                                           │
│  AnnexBNalIterator          length-prefixed iterator                      │
│    detects 00 00 01 /        [length][NAL] with length sizes 1..4         │
│    00 00 00 01 start codes,  read_big_endian_length()                    │
│    strips trailing_zero_8bits                                            │
└──────────────────────────────────┬───────────────────────────────────────┘
                                   │  FramedNalSpan (header + EBSP)
                                   ▼
┌──────────────────────────────────────────────────────────────────────────┐
│  NAL UNIT LAYER                                                            │
│  HEVC: parser/hevc_nal_unit_parser.hpp      AVC: parser/avc_nal_unit_parser.hpp│
│  + syntax/hevc_nal_unit.hpp                  + syntax/avc_nal_unit.hpp         │
│  + syntax/hevc_nal_unit_header.hpp           + syntax/avc_common.hpp           │
│                                                                           │
│  HEVC: 2-byte header                    AVC: 1-byte header                │
│   forbidden_zero_bit ·                    forbidden_zero_bit(1) ·         │
│   nal_unit_type(6) · nuh_layer_id(6) ·    nal_ref_idc(2) ·                │
│   nuh_temporal_id_plus1(3)                nal_unit_type(5)                │
└──────────────────────────────────┬───────────────────────────────────────┘
                                   │  EBSP payload (no copy)
                                   ▼
┌──────────────────────────────────────────────────────────────────────────┐
│  BIT READERS                 bitstream/*.hpp  (shared)                    │
│                                                                           │
│  RbspBitstreamReader (map-based):                                         │
│    precomputed logical EPB→RBSP map (skips 00 00 03, O(1) random access)  │
│    read_bit/bits/ue/se · more_rbsp_data · rbsp_trailing_bits · align      │
│    used where backward scans are needed: VPS/SPS/PPS extension loops, AVC │
│    SEI. Builds the map once in the ctor (scans the whole NAL payload).    │
│                                                                           │
│  RbspReader (zero-alloc sequential):                                      │
│    read_bit/bits/ue/se with an inline byte cursor; NO map, NO allocation  │
│    used for slice-header parsing (forward reads only) — skips the map.    │
└───────────────────────────────┬──────────────────┬───────────────────────┘
                                │                  │
        ┌───────────────────────┴──────────┐       │
        │  SYNTAX PARSERS (see §3)         │       │
        │  HEVC: vps/sps/pps/slice/sei     │       │
        │  AVC:  sps/pps/slice/sei         │       │
        └───────────────┬──────────────────┘       │
                        ▼                          │
        ┌──────────────────────────┐                │
        │  SYNTAX STRUCTS (syntax/ │◄───────────────┘
        │  vps/sps/pps/slice/sei…  │
        │  avc_*.hpp)              │
        └───────────────┬──────────┘
                        ▼
        ┌──────────────────────────────────────────┐
        │  STATE          parser/parameter_set_    │
        │                 manager.hpp              │
        │  HEVC: bounded VPS(16)/SPS(16)/PPS(64)   │
        │  AVC:  parser/avc_parameter_set_manager  │
        │        SPS(32)/PPS(256), slice resolution│
        └──────────────────────────────────────────┘
```

---

## 2. End-to-end data flow

HEVC (`parser/hevc_nal_parser.hpp`):

```
Annex-B byte stream
   │
   ▼
dispatch_annex_b(span, handlers)
   │  while (framer.valid())
   │      parse_and_dispatch_nal(framer.nal(), handlers)
   ▼
AnnexBNalIterator::nal()  →  NAL span             nal_framer.hpp
   ▼
parse_nal_unit(span)  →  NalUnit                  hevc_nal_unit_parser.hpp
   │   header unpacked, payload = span[2:]
   ▼
handlers.vps/sps/pps/sei/slice(nal)               caller-provided callback
   │
   ▼
RbspBitstreamReader(ebsp_span)                    rbsp_bitstream_reader.hpp
   │   EPB removal is logical (map built once in ctor)
   ▼
parse_sequence_parameter_set(reader)  →  SequenceParameterSet
   ▼
parameter_sets.store_sps(std::move(sps))           hevc_parameter_set_manager.hpp
   ▼
(later) slice: find_pps(id) → find_sps(pps→sps_id)
   → parse_slice_segment_header(RbspReader, ...)   rbsp_reader.hpp (no map)
```

AVC (`parser/avc_nal_parser.hpp`) mirrors this, with one key difference: AVC has
**no VPS**, so a slice references a PPS which references an SPS:

```
slice NAL
   │  pic_parameter_set_id read from inside the slice header
   ▼
avc::ParameterSetManager::resolve(pps_id)          avc_parameter_set_manager.hpp
   │  → const PictureParameterSet*  →  const SequenceParameterSet*
   ▼
avc::parse_slice_header(reader, *sps, *pps, nal_type, nal_ref_idc)
```

---

## 3. Syntax parser map

Each row: *parser file → syntax model → notable responsibilities / sub-parsers*.

### 3.1 HEVC

```
┌──────────────────┬──────────────────────────┬──────────────────────────────┐
│ VPS              │ syntax/hevc_vps.hpp           │ vps id · max layers/sub-layers│
│ parser/          │                          │ profile_tier_level()          │
│ hevc_vps_parser.hpp   │                          │ sub-layer ordering · layer    │
│                  │                          │ sets · timing/HRD             │
│                  │                          │ ▶ hevc_hrd_parser.hpp              │
│                  │                          │ ▶ profile_tier_level_parser   │
├──────────────────┼──────────────────────────┼──────────────────────────────┤
│ SPS              │ syntax/hevc_sps.hpp           │ dimensions · chroma · bit     │
│ parser/          │  + sps_range_extension   │ depth · POC · CTB geometry    │
│ hevc_sps_parser.hpp   │  + sps_multilayer_       │ scaling lists · short-term    │
│                  │    extension             │ RPS · long-term refs · VUI    │
│                  │  + sps_3d_extension      │ + RExt/ML/3D/SCC extensions   │
│                  │  + sps_scc_extension     │ ▶ hevc_vui_parser.hpp              │
│                  │                          │ ▶ profile_tier_level_parser   │
│                  │                          │ ▶ hevc_hrd_parser.hpp              │
├──────────────────┼──────────────────────────┼──────────────────────────────┤
│ PPS              │ syntax/hevc_pps.hpp           │ tiles · deblocking · scaling  │
│ parser/          │  + pps_range_extension   │ lists + RExt/ML/3D/SCC        │
│ hevc_pps_parser.hpp   │  + pps_multilayer_       │ extensions (colour-mapping    │
│                  │    extension             │ octants, DLTS/delta_dlt,      │
│                  │  + pps_3d_extension      │ palette predictors)           │
│                  │  + pps_scc_extension     │                              │
├──────────────────┼──────────────────────────┼──────────────────────────────┤
│ Slice header     │ syntax/hevc_slice_header.hpp  │ first_slice · dependent slice │
│ parser/          │                          │ pps id · slice type · POC ·   │
│ hevc_slice_parser.hpp │                          │ RPS · long-term · ref list    │
│                  │                          │ mod · pred weights · entry    │
│                  │                          │ points · slice extension      │
│                  │                          │ ▶ hevc_slice_parser_context.hpp    │
│                  │                          │   (resolved SPS/PPS/VPS)      │
│                  │                          │ ▶ reference_picture_manager  │
│                  │                          │   (ref lists, POC state)      │
│                  │                          │ ▶ hevc_short_term_ref_pic_set.hpp  │
├──────────────────┼──────────────────────────┼──────────────────────────────┤
│ SEI              │ syntax/hevc_sei.hpp           │ payload type/size with 0xFF   │
│ parser/          │                          │ extension · known payloads    │
│ hevc_sei_parser.hpp   │                          │ (user data, mastering display │
│                  │                          │ colour volume, CLL, …) ·      │
│                  │                          │ unknown types preserved       │
└──────────────────┴──────────────────────────┴──────────────────────────────┘
```

### 3.2 AVC

```
┌──────────────────┬──────────────────────────┬──────────────────────────────┐
│ SPS              │ syntax/avc_sps.hpp       │ profile/level · chroma_format │
│ parser/          │                          │ · bit depth · POC type ·      │
│ avc_sps_parser   │                          │ max refs · frame_mbs_only ·   │
│ .hpp             │                          │ MB dimensions + crop helpers  │
│                  │                          │ · scaling lists (defaults)    │
│                  │                          │ · VUI/HRD                     │
│                  │                          │ ▶ avc_vui.hpp                 │
│                  │                          │ ▶ avc_scaling_list.hpp        │
├──────────────────┼──────────────────────────┼──────────────────────────────┤
│ PPS              │ syntax/avc_pps.hpp       │ entropy mode · slice groups    │
│ parser/          │                          │ (map types 0/2/3/4/5/6) ·      │
│ avc_pps_parser   │                          │ num_ref_idx_defaults ·        │
│ .hpp             │                          │ weighted pred · QP init ·      │
│                  │                          │ deblocking · optional 8x8      │
│                  │                          │ transform + scaling section   │
│                  │                          │ (gated by more_rbsp_data)     │
├──────────────────┼──────────────────────────┼──────────────────────────────┤
│ Slice header     │ syntax/avc_slice_header  │ first_mb · slice_type ·       │
│ parser/          │ .hpp                     │ frame_num · POC · direct/spat │
│ avc_slice_parser │   (templated on Reader)  │ · ref idx active override     │
│ .hpp             │                          │ (inferred from PPS defaults   │
│                  │                          │ when clear, 7.4.3.1) · ref    │
│                  │                          │ pic list mod (L0/L1) · pred   │
│                  │                          │ weight table (separate L0/L1) │
│                  │                          │ · dec_ref_pic_marking (MMCO)  │
│                  │                          │ · CABAC init · QP delta ·     │
│                  │                          │ deblocking · SP/SI            │
│                  │                          │ caps: 64 reorder ops, 64 MMCO │
├──────────────────┼──────────────────────────┼──────────────────────────────┤
│ SEI              │ syntax/avc_sei.hpp       │ payload type/size with 0xFF   │
│ parser/          │                          │ extension · messages kept as  │
│ avc_sei_parser   │                          │ (type, size) list             │
│ .hpp             │                          │                              │
└──────────────────┴──────────────────────────┴──────────────────────────────┘
```

Shared sub-parsers:

```
  parser/nal_framer.hpp                       Annex-B / length-prefixed framing
  parser/avc_parse_common.hpp                 ParseError · read_ue_max ·
                                              read_se_bounded · ceil_log2
  parser/avc_nal_unit_parser.hpp              1-byte AVC NAL header
  bitstream/rbsp_bitstream_reader.hpp         map-based bit reader (PS/SEI)
  bitstream/rbsp_reader.hpp                   zero-alloc sequential reader (slices)
```

---

## 4. NAL type dispatch

HEVC (`parser/hevc_nal_parser.hpp`) routes by `nal_unit_type`:

```
                          parse_and_dispatch_nal(span, handlers)
                                        │
                                        ▼
                            ┌─── validate NAL header ───┐
                            │      (throws on bad)      │
                            └────────────┬──────────────┘
                                         ▼
                    switch (nal.type())   (NalUnitType: 0..63)
   ┌───────────┬───────────┬───────────┬───────────┬─────────────┐
   ▼           ▼           ▼           ▼           ▼
 VPS_NUT(32) SPS_NUT(33) PPS_NUT(34) SEI(39/40)  VCL (0..31)
 handlers.vps handlers.sps handlers.pps prefix/suffix handlers.slice
   │           │           │          handlers.       │
   │           │           │          prefix_sei/     │
   │           │           │          suffix_sei      │
   └───────────┴───────┬───┴──────────────────────────┘
                       ▼
             Parsed / Ignored / Unsupported  (NalParseResult)
```

AVC (`parser/avc_nal_parser.hpp`) routes by the 5-bit `nal_unit_type`:

```
   ┌───────────┬───────────┬───────────┬───────────┬─────────────┐
   ▼           ▼           ▼           ▼           ▼
 SPS(7)      PPS(8)      SEI(6)      IDR(5)     VCL (1..4, 9..20)
 handlers.   handlers.   handlers.   handlers.   handlers.slice
 sps         pps         sei         slice
   └───────────┴───────┬───┴─────────────┴──────────┘
                       ▼
             all other types → handlers.unsupported (Ignored)
```

Both codecs reuse the same framing (`dispatch_annex_b` / `dispatch_length_prefixed`)
and the same `NalFramingMode` enum from `parser/nal_framer.hpp`.

---

## 5. Static dependency graph

```
                hevc_nal_parser.hpp                  avc_nal_parser.hpp
                │              │                   │             │
                ▼              ▼                   ▼             ▼
          nal_framer.hpp  hevc_nal_unit_parser.hpp  avc_nal_unit_  avc_sps_parser.hpp
                                          │    parser.hpp   │
                                          ▼                ▼
                          rbsp_bitstream_reader.hpp   avc_parse_common.hpp
                                          ▲
        ┌───────────┬─────────────────────┴──────────┬───────────┐
        ▼           ▼                                 ▼           ▼
 HEVC stack                                    AVC stack
 vps_parser  sps_parser  pps_parser            avc_sps_parser  avc_pps_parser
 sei_parser  slice_parser                     avc_slice_parser  avc_sei_parser
        ▼                                                 │
 hevc_parameter_set_manager.hpp                                ▼
        │                                   avc_parameter_set_manager.hpp
        │                                   (SPS 32 / PPS 256, resolve())
        ▼
  rbsp_bitstream_reader.hpp → <span> <vector> <cstdint> (self-contained)
  rbsp_reader.hpp           → <span> <cstdint> (no allocation; slice headers)
  log.hpp                   → self-contained macros (BS_ENABLE_TRACE gate)
```

The two stacks are independent above the shared framing / bit-reader / logging
layers, so an HEVC build does not drag in AVC code and vice-versa.

`bsparser.hpp` sits at the very top: it `#include`s both stacks plus the shared
layers and adds the unified `Codec` / `State` / `parse` front end. The unified
`parse` reuses each stack's existing framing iterators and `dispatch_nal`, and
adds per-`State` parameter-set auto-storage on top — it does not duplicate the
per-codec dispatch logic.

---

## 5b. Additional codecs (VVC / AV1 / VP9 / VP8)

The unified API was extended to cover VVC, AV1, VP9 and VP8 using the same
layered model as HEVC/AVC: per-codec `syntax/` models + `parser/` parsers,
codec-agnostic framing, a `Codec` enum value, a `*ParsedHandlers` type and a
`bs::parse` overload.

| Codec | Framing (`NalFramingMode`) | Unit layer | Parsed |
|---|---|---|---|
| VVC | Annex-B / length-prefixed (shared) | 2-byte NAL header | DCI/OPI/VPS/SPS/PPS/PH, slice headers |
| AV1 | `Obu` (Annex-B start codes **and** low-overhead ULEB128 sizes) | OBU header | sequence + frame headers |
| VP9 | `Ivf` | IVF frame | uncompressed frame headers |
| VP8 | `Ivf` | IVF frame | uncompressed frame headers |

Key reuse:

- **VVC** reuses the HEVC bit readers verbatim — it keeps the same RBSP
  emulation-prevention scheme (`00 00 03`), so `RbspReader` (slice/PH) and
  `RbspBitstreamReader` (parameter sets) apply unchanged. Only the NAL header
  layout and the syntax parsers are new.
- **AV1** adds two small codec-agnostic components: a **boolean arithmetic
  decoder** (`bitstream/boolean_decoder.hpp`) because AV1 headers are
  entropy-coded, and an **OBU framer** (`parser/obu_framer.hpp`) that handles
  both Annex-B start codes and low-overhead (size-field) OBUs.
- **VP8/VP9** use the plain bit reader (`bitstream/plain_bit_reader.hpp`, no
  emulation prevention) and an **IVF framer** (`parser/ivf_framer.hpp`).

VP8/VP9/AV1 have no SPS/PPS, so their `State` holds no parameter-set manager;
VVC adds a `vvc::ParameterSetManager` (DCI/OPI/VPS/SPS/PPS/PH) alongside the
HEVC/AVC managers.

The new syntax parsers parse the *leading header fields* (IDs, dimensions,
chroma, flags); the full VVC/AV1 RBSP depth (scaling lists, RPL, ALF/LMCS
tables, …) is not yet modelled. Sample streams in `tests/fuzz/corpus/`
(`vvc_sample.266`, `av1_sample.obu`, `vp9_sample.ivf`, `vp8_sample.ivf`) were
generated with `ffmpeg` (`libvvenc`, `libaom-av1`, `libvpx`, `libvpx-vp9`).

---

## 5c. Container demuxing (`demux/`)

A separate **demux layer** lets callers feed *muxed files* directly instead of
pre-extracting elementary streams. It auto-detects the container and extracts
the first video track:

```
container bytes
   │  demux::sniff()  (magic bytes / box scan / TS sync)
   ▼
MP4 │ MPEG-TS │ FLV │ AVI │ IVF          demux/{mp4,ts,flv,avi}_demuxer.hpp
   ▼
ElementaryStream { codec, framing, bytes, width, height }
   ▼
bs::parse(state, es.bytes, es.framing, handlers)   (existing path)
```

- **MP4** (`mp4_demuxer.hpp`): walks `moov→trak→mdia→minf→stbl` (stsd/stsz/
  stsc/stco/co64), reads sample sizes/offsets and reconstructs a self-contained
  elementary stream: avc1/hvc1/hev1/vvc1 → Annex-B (parameter sets prepended
  from avcC/hvcC/vvcC), av01 → OBUs, vp09/vp08 → IVF.
- **MPEG-TS** (`ts_demuxer.hpp`): follows PAT→PMT to the first video stream,
  strips PES headers and concatenates the Annex-B payload.
- **FLV** (`flv_demuxer.hpp`): walks video tags; AVC/HEVC/VVC NALUs → Annex-B,
  AV1 → OBUs, VP8/VP9 → IVF.
- **AVI** (`avi_demuxer.hpp`): reads `hdrl`/`strh`/`strf` + `movi` chunks;
  H.26x chunks (Annex-B or length-prefixed) → Annex-B, VP8/VP9 → IVF, AV1 → OBUs.
- **IVF** passes through as the VP8/VP9 elementary stream.

The CLI calls `demux::sniff` + `demux::demux` automatically, so
`bs_cli input.mp4` works without `--codec`/`--format`. The demux layer is
deliberately limited: single video track, no fragmented MP4 (`moof`), no
MKV/WebM (EBML), no subtitles/audio.

---

## 6. Directory map

```
bsparser/
├── CMakeLists.txt                project(bsparser); targets bs_test, bs_avc_test,
│                                 bs_unified_test, bs_fuzz, bs_fuzz_driver;
│                                 BS_ENABLE_* options; root added to include path
├── bsparser.h                   SINGLE UMBRELLA HEADER (use this) — adapts
│                                 to language (C/C++), standard (C++20 or
│                                 not) and build config (BS_USE_C_API /
│                                 BS_FORCE_C_API) and pulls in either the C
│                                 API (bs_capi.h) or the C++20 templates
│                                 (bsparser.hpp)
├── bsparser.hpp                  UNIFIED PUBLIC HEADER (C++20 templates) —
│                                 aggregates all sub-headers; defines bs::Codec,
│                                 bs::State, bs::create_state(), bs::parse()
├── cli/                          command-line tool
│   ├── bs_cli.cpp                arg parsing, help, codec auto-detect,
│   │                             framing selection, file I/O
│   └── report.hpp                Report/NalEntry model, JSON + HTML
│                                 (self-contained viewer w/ filters) exporters
├── capi/                         C public API (stable ABI)
│   ├── bs_capi.h                 extern "C" header: BsState, BsCodec,
│   │                             BsFramingMode, BsNalUnit, BsNalHandlers,
│   │                             BsNalEntry, BsReport, bs_parse,
│   │                             bs_parse_report, bs_state_*, bs_report_destroy,
│   │                             bs_get_last_error
│   └── bs_capi.cpp               single TU bridging C ABI ↔ bs::State /
│                                 bs::parse; compiled into the bs_capi STATIC
│                                 or SHARED library (structs, no JSON)
├── bitstream/
│   ├── rbsp_bitstream_reader.hpp   map-based bit reader (EPB map, ue/se,
│   │                               bounds, more_rbsp_data) — PS/SEI parsing
│   ├── rbsp_reader.hpp             zero-allocation sequential RbspReader —
│   │                               slice-header parsing (forward reads only)
│   ├── plain_bit_reader.hpp        MSB-first bit reader, no EP — VP8/VP9
│   ├── boolean_decoder.hpp         AV1 boolean arithmetic decoder
│   └── bitstream_reader.hpp
├── parser/
│   ├── hevc_nal_parser.hpp           HEVC NAL dispatch + handler callbacks
│   ├── nal_framer.hpp              Annex-B / length-prefixed framing (shared)
│   ├── hevc_nal_unit_parser.hpp         HEVC NAL header parse → NalUnit
│   ├── hevc_nal_unit_header_parser.hpp  16-bit header unpacking
│   ├── hevc_parameter_set_manager.hpp   HEVC VPS/SPS/PPS storage (id-keyed)
│   ├── hevc_vps_parser.hpp · hevc_sps_parser.hpp · hevc_pps_parser.hpp
│   ├── hevc_slice_parser.hpp            HEVC slice segment header + context helpers
│   ├── hevc_slice_parser_context.hpp    resolved SPS/PPS/VPS bundle for slices
│   ├── hevc_sei_parser.hpp              HEVC SEI message stream
│   ├── hevc_profile_tier_level_parser.hpp · hevc_scaling_list_parser.hpp
│   ├── hevc_short_term_ref_pic_set_parser.hpp · hevc_vui_parser.hpp · hevc_hrd_parser.hpp
│   ├── hevc_reference_picture_manager.hpp  ref pic lists, POC state
│   │
│   ├── avc_nal_parser.hpp          AVC NAL dispatch + handler callbacks
│   ├── avc_nal_unit_parser.hpp     1-byte AVC NAL header parse
│   ├── avc_parse_common.hpp        ParseError · bounded ue/se readers
│   ├── avc_parameter_set_manager.hpp  AVC SPS(32)/PPS(256) + slice resolution
│   ├── avc_sps_parser.hpp · avc_pps_parser.hpp
│   ├── avc_slice_parser.hpp        AVC slice header (7.3.3.1)
│   └── avc_sei_parser.hpp          AVC SEI (type/size with 0xFF extension)
│   │
│   ├── vvc_nal_unit_parser.hpp · vvc_vps/sps/pps/dci/opi/ph/slice_parser.hpp
│   ├── vvc_parameter_set_manager.hpp   VVC DCI/OPI/VPS/SPS/PPS/PH store
│   ├── obu_framer.hpp · av1_obu_parser.hpp · av1_sequence/frame_header_parser.hpp
│   ├── ivf_framer.hpp · vp8_frame_header_parser.hpp · vp9_frame_header_parser.hpp
├── demux/                       container demuxing layer
│   ├── demuxer.hpp              Container sniff() + auto-detect facade
│   ├── mp4_demuxer.hpp          ISO-BMFF (stsd/stsz/stsc/stco → ES)
│   ├── ts_demuxer.hpp           MPEG-TS (PAT/PMT/PES → Annex-B)
│   ├── flv_demuxer.hpp          FLV video tags → ES
│   ├── avi_demuxer.hpp          RIFF/AVI → ES
│   └── stream.hpp               Container + ElementaryStream types
├── syntax/                        immutable parsed models
│   ├── hevc_common.hpp                 shared enums/constants (HEVC)
│   ├── hevc_nal_unit.hpp · hevc_nal_unit_header.hpp
│   ├── hevc_vps.hpp · hevc_sps.hpp · hevc_pps.hpp (incl. all RExt/ML/3D/SCC extensions)
│   ├── hevc_slice_header.hpp · hevc_sei.hpp
│   ├── hevc_profile_tier_level.hpp · hevc_scaling_list.hpp · hevc_short_term_ref_pic_set.hpp
│   ├── hevc_vui.hpp · hevc_hrd.hpp
│   │
│   ├── avc_common.hpp              AVC enums/constants (NalUnitType, caps)
│   ├── avc_nal_unit.hpp · avc_sps.hpp · avc_pps.hpp
│   ├── avc_slice_header.hpp        (ref list mod · MMCO · pred weight table)
│   ├── avc_sei.hpp · avc_vui.hpp · avc_scaling_list.hpp
│   │
│   ├── vvc_common.hpp · vvc_nal_unit.hpp · vvc_dci/opi/vps/sps/pps/ph/slice.hpp
│   ├── av1_common.hpp · av1_obu.hpp · av1_sequence_header.hpp · av1_frame_header.hpp
│   └── vp8_frame_header.hpp · vp9_frame_header.hpp
├── logging/
│   └── log.hpp                    BS_LOG_* macros, BS_ENABLE_TRACE gate
├── tests/
│   ├── main.cpp                   HEVC demo driver (reads .hevc file at runtime)
│   ├── ext_test.cpp               HEVC extension-field validation tool
│   ├── avc_test.cpp               AVC demo driver (reads .h264 file at runtime)
│   ├── unified_test.cpp           unified API: Codec/State, multi-state, clear()
│   └── fuzz/
│       ├── fuzz_hevc.cpp          shared LLVMFuzzerTestOneInput
│       ├── fuzz_driver.cpp        standalone file/stdin runner (GCC etc.)
│       └── corpus/                seeds: stream.hevc, ext_stream.hevc,
│                                  avc_main.h264, avc_high444.h264
└── docs/architecture.md           this document
```

---

## 7. AVC specifics worth remembering

- The AVC NAL header is **1 byte** (`forbidden_zero_bit` 1, `nal_ref_idc` 2,
  `nal_unit_type` 5), versus HEVC's 2 bytes.
- AVC has **no VPS**: PPS id lives in the slice header, SPS id lives in the PPS.
  `ParameterSetManager::resolve(pps_id)` returns both.
- `more_rbsp_data()` is used at the end of the PPS to gate the optional
  `transform_8x8_mode_flag` / scaling / `second_chroma_qp_index_offset` section.
  The shared reader locates the final `1` bit (the `rbsp_stop_one_bit`) rather
  than treating remaining bytes as data.
- When `num_ref_idx_active_override_flag` is 0, `NumRefIdxActive` is inferred
  from the PPS `num_ref_idx_l0/l1_default_active_minus1` fields (H.264 7.4.3.1).
  The `pred_weight_table` for P slices depends on this, so parsing must use the
  inferred value or later fields (MMCO) misalign.
- SEI `payload_size` is a sequence of `0xFF` bytes plus a final byte.

## 8. Key HEVC extension structures (2016–2019 H.265 amendments)

Added to SPS/PPS models and parsers (validated against `ffmpeg trace_headers`):

```
SPS                          PPS
├─ sps_extension             ├─ pps_extension
│  ├─ range_extension_flag   │  ├─ range_extension_flag
│  ├─ multilayer_extension   │  ├─ multilayer_extension   pps_multilayer_extension()
│  ├─ extension_3d_flag      │  │  ├─ poc_reset_info
│  └─ scc_extension_flag     │  │  ├─ infer_scaling_list · ref layer id
│  ├─ 4 reserved bits        │  │  ├─ ref_loc_offsets (scaled/ref/resample)
│  └─ payloads in flag order │  │  └─ colour_mapping_table (octant recursion)
│                            │  ├─ extension_3d_flag → pps_3d_extension()
│  sps_range_extension()     │  │  └─ dlts · delta_dlt per depth layer
│  sps_multilayer_extension()│  └─ scc_extension_flag → pps_scc_extension()
│  sps_3d_extension()        │     ├─ curr_pic_ref · ACT qp offsets
│  sps_scc_extension()       │     └─ palette predictor initializers
│    palette_mode/initializers
```

Parsing order is fixed by the spec: the 4 flags, then the 4 reserved bits, then
each enabled payload in flag order (range → multilayer → 3D → SCC), then
`while (more_rbsp_data()) sps/pps_extension_data_flag`.