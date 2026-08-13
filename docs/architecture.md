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

---

## 1. Layer overview

```
┌──────────────────────────────────────────────────────────────────────────┐
│                     ENTRY POINTS / CLIENTS                                │
│   tests/main.cpp        tests/avc_test.cpp         tests/fuzz/fuzz_*.cpp  │
│   (HEVC demo)           (AVC demo, H.264 streams)  (fuzz harness +        │
│                                                   standalone driver)      │
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
│  BIT READER              bitstream/rbsp_bitstream_reader.hpp  (shared)    │
│                                                                           │
│  Precomputed logical EPB→RBSP map (skips 00 00 03 emulation bytes, O(1))  │
│  read_bit · read_bits(n) · read_ue / read_se · read_u8/u16/u32            │
│  skip_bits · align_to_byte · more_rbsp_data · rbsp_trailing_bits          │
│  bit_position / bits_remaining / byte_aligned · bounds-checked reads      │
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
(later) slice: find_pps(id) → find_sps(pps→sps_id) → parse_slice_segment_header
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
│ avc_slice_parser │                          │ · ref idx active override     │
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
  bitstream/rbsp_bitstream_reader.hpp         shared bit reader (codec-agnostic)
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
 log.hpp                    → self-contained macros (BS_ENABLE_TRACE gate)
```

The two stacks are independent above the shared framing / bit-reader / logging
layers, so an HEVC build does not drag in AVC code and vice-versa.

---

## 6. Directory map

```
bsparser/
├── CMakeLists.txt                project(bsparser); targets bs_test, bs_avc_test,
│                                 bs_fuzz, bs_fuzz_driver; BS_ENABLE_* options
├── bitstream/
│   ├── rbsp_bitstream_reader.hpp   primary bit reader (EPB map, ue/se, bounds)
│   ├── rbsp_reader.hpp             lightweight RbspReader factory used by
│   │                               make_nal_rbsp_reader()
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
├── logging/
│   └── log.hpp                    BS_LOG_* macros, BS_ENABLE_TRACE gate
├── tests/
│   ├── main.cpp                   HEVC demo driver (reads .hevc file at runtime)
│   ├── ext_test.cpp               HEVC extension-field validation tool
│   ├── avc_test.cpp               AVC demo driver (reads .h264 file at runtime)
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