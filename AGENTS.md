# Agent Guide — bsparser

## Goal

Maintain a C++17, dependency-free **header-level** raw-video bitstream inspector.
It finds elementary-unit boundaries and extracts lightweight metadata; it is
not a decoder and must not attempt entropy-coded payload parsing.

## Read first

1. `include/bsparser.hpp` — public contract.
2. `docs/CODEBASE.md` — architecture, invariants, and ABI details.
3. `tests/tests.cpp` — minimal behavioral examples.

## Project map

| Path | Purpose |
| --- | --- |
| `src/bsparser.cpp` | Parsing, streaming scanners, SIMD dispatch |
| `include/bsparser.hpp` | Library API |
| `src/main.cpp` | JSON-lines CLI + HTML report generator |
| `tests/tests.cpp` | Assertion-based tests |
| `CMakeLists.txt` | Library/executable/test targets + ARMv7 build rule |
| `reference/bsparser-web/parser.js` | Original reference implementation |

## Non-negotiable behavior

- `UnitScanner` accepts arbitrary chunks and returns **only complete** units.
- Annex-B `Unit.bytes` excludes its 3/4-byte start code; `offset` points at
  the first payload byte.
- AV1 `Unit.bytes` includes the complete OBU header and payload.
- A raw concatenated VP8/VP9 stream has no intrinsic frame delimiter: one
  `feed()` call equals one complete frame.
- Keep all scalar fallbacks. SIMD is an optimization, never a requirement.
- Preserve `UnitScanner`'s cursor-based buffering; do not reintroduce
  `vector.erase()` per NAL/OBU.
- Parsing must bounds-check malformed/truncated input and throw standard
  exceptions instead of reading past the buffer.

## Change checklist

1. Update `bsparser.hpp` and `README.md` for public API changes.
2. Add a focused assertion to `src/tests.cpp` for each boundary/parser bug.
3. Format with the repository `.clang-format`.
4. Compile all relevant Android ABIs when SIMD/build logic changes.


## Scope guidance

Prefer small, explicit syntax readers with named fields. Before porting a
large JavaScript syntax section, confirm it is useful for boundary detection,
timeline metadata, or keyframe indexing.

## Progress Task
# Task: Refactor HEVC Parser for Correct Bitstream Parsing

## Objective

Refactor the existing `bsparser` HEVC implementation so that HEVC Annex-B NAL units, RBSP syntax, VPS, SPS, PPS, and VCL slice headers are parsed according to the HEVC/H.265 bitstream syntax rather than using simplified/guessed field layouts.

The existing parser successfully detects Annex-B NAL boundaries and extracts the HEVC NAL header, but the syntax parser currently loses bitstream synchronization when parsing SPS/PPS/slice headers.

The refactor must produce structurally correct HEVC parsing and must never "guess" syntax fields when the required SPS/PPS state is unavailable.

---

# Existing Implementation

The main implementation is currently in:

```text
bsparser.cpp
bsparser.h
```

Relevant components:

```cpp
BitReader
UnitScanner
StreamParser
find_annexb_start_codes()
parse_unit()
parse_hevc()
rbsp()
skip_profile_tier_level()
```

The parser supports:

```cpp
Codec::HEVC
```

and uses Annex-B start-code scanning.

The public API should be preserved wherever reasonably possible.

---

# Current Observed Failure

The parser currently produces output similar to:

```text
[*] Header Type=VPS [*]
   nal_unit_type=32
   nuh_layer_id=0
   nuh_temporal_id_plus1=1
   vps_video_parameter_set_id=3
   vps_base_layer_available_flag=0
   vps_base_layer_internal_flag=0
   vps_max_layers_minus1=0
   vps_max_sub_layers_minus1=0
   vps_temporal_id_nesting_flag=1

[*] Header Type=HEVC NAL [*]
   nal_unit_type=33
   nuh_layer_id=0
   nuh_temporal_id_plus1=1
   sps_max_sub_layers_minus1=0
   sps_seq_parameter_set_id=6
   ...
   pic_width_in_luma_samples=13
   pic_height_in_luma_samples=4
   bit_depth=14
   log2_max_pic_order_cnt_lsb_minus4=2187
   height=4269770681

[*] Header Type=HEVC NAL [*]
   nal_unit_type=34
   ...
   pps_pic_parameter_set_id=0
   pps_seq_parameter_set_id=0
   num_extra_slice_header_bits=6
   num_ref_idx_l0_default_active_minus1=267

[*] Header Type=IDR_N_LP [*]
   nal_unit_type=20
   first_slice_segment_in_pic_flag=1
   slice_pic_parameter_set_id=16
   slice_type=1
   slice_type_name=P
   poc=174
```

These values demonstrate bitstream synchronization failure.

In particular:

```text
log2_max_pic_order_cnt_lsb_minus4=2187
height=4269770681
num_ref_idx_l0_default_active_minus1=267
slice_pic_parameter_set_id=16
```

are not trustworthy.

Also, HEVC `slice_type` is only:

```text
0 = B
1 = P
2 = I
```

Therefore values such as:

```text
3
5
7
```

must be treated as parser errors, not mapped to "Unknown" and silently accepted.

---

# Important Existing Bugs

## 1. HEVC NAL type name table

The HEVC NAL type mapping must be exactly aligned with the numeric NAL types.

Correct mapping:

```text
0   TRAIL_N
1   TRAIL_R
2   TSA_N
3   TSA_R
4   STSA_N
5   STSA_R
6   RADL_N
7   RADL_R
8   RASL_N
9   RASL_R
10  RSV_VCL_N10
11  RSV_VCL_R11
12  RSV_VCL_N12
13  RSV_VCL_R13
14  RSV_VCL_N14
15  RSV_VCL_R15
16  BLA_W_LP
17  BLA_W_RADL
18  BLA_N_LP
19  IDR_W_RADL
20  IDR_N_LP
21  CRA_NUT
22  RSV_IRAP_VCL22
23  RSV_IRAP_VCL23
24  RSV_VCL24
25  RSV_VCL25
26  RSV_VCL26
27  RSV_VCL27
28  RSV_VCL28
29  RSV_VCL29
30  RSV_VCL30
31  RSV_VCL31
32  VPS_NUT
33  SPS_NUT
34  PPS_NUT
35  AUD_NUT
36  EOS_NUT
37  EOB_NUT
38  FD_NUT
39  PREFIX_SEI_NUT
40  SUFFIX_SEI_NUT
```

Do not shift VPS/SPS/PPS/AUD/SEI names.

---

# 2. HEVC NAL header

Keep the existing 2-byte HEVC NAL header parsing, but validate it.

HEVC NAL header:

```text
forbidden_zero_bit : 1
nal_unit_type      : 6
nuh_layer_id       : 6
nuh_temporal_id+1  : 3
```

Current extraction:

```cpp
uint8_t type = (d[0] >> 1) & 0x3f;
uint8_t layer_id = ((d[0] & 1) << 5) | (d[1] >> 3);
uint8_t tid = d[1] & 7;
```

is acceptable.

Add validation:

```cpp
forbidden_zero_bit == 0
nuh_temporal_id_plus1 != 0
```

Invalid headers should be reported as parse errors rather than silently accepted.

---

# 3. RBSP / EBSP handling

HEVC syntax must be parsed from RBSP, not raw EBSP.

The parser must remove emulation-prevention bytes:

```text
00 00 03 xx
```

becomes:

```text
00 00 xx
```

The implementation must not remove arbitrary `03` bytes.

Only a `03` satisfying the emulation-prevention condition may be removed.

Create or improve a dedicated helper such as:

```cpp
Bytes ebsp_to_rbsp(const uint8_t* data, size_t size);
```

or equivalent.

Do not mix Annex-B start-code removal with RBSP processing.

The processing pipeline must be:

```text
Annex-B stream
    ↓
start code
    ↓
NAL unit
    ↓
2-byte NAL header
    ↓
EBSP payload
    ↓
RBSP
    ↓
BitReader
    ↓
HEVC syntax parser
```

---

# 4. BitReader

Keep the existing MSB-first behavior.

The following must remain correct:

```cpp
u(bits)
s(bits)
ue()
se()
skip()
bits_left()
bit_position()
```

Add strong bounds checking.

`ue()` must reject unreasonable/truncated Exp-Golomb codes instead of producing arbitrary huge values.

Do not silently catch and ignore parse failures.

---

# 5. Do NOT silently swallow HEVC parsing errors

The current implementation contains patterns like:

```cpp
try
{
    ...
}
catch (const std::exception&)
{
}
```

This is unacceptable for the new HEVC parser.

A malformed/truncated HEVC NAL must not appear as successfully parsed output containing partially decoded garbage.

Use one of:

```cpp
throw
```

or an explicit parse-status/error mechanism.

The existing public API can be preserved, but malformed syntax must be distinguishable from successfully parsed syntax.

Do not manufacture values such as:

```text
poc=0
slice_type=Unknown
width=0
```

just because parsing failed.

---

# 6. HEVC parser must maintain SPS/PPS state

This is the most important architectural change.

HEVC VCL slice headers depend on previously parsed SPS/PPS information.

The parser must maintain codec-specific state similar to:

```cpp
struct HevcSps
{
    uint32_t sps_seq_parameter_set_id;
    uint32_t sps_video_parameter_set_id;

    uint32_t max_sub_layers_minus1;

    uint32_t chroma_format_idc;
    bool separate_colour_plane_flag;

    uint32_t pic_width_in_luma_samples;
    uint32_t pic_height_in_luma_samples;

    uint32_t bit_depth_luma_minus8;
    uint32_t bit_depth_chroma_minus8;

    uint32_t log2_max_pic_order_cnt_lsb_minus4;

    // Add additional fields required by slice-header parsing.
};

struct HevcPps
{
    uint32_t pps_pic_parameter_set_id;
    uint32_t pps_seq_parameter_set_id;

    bool dependent_slice_segments_enabled_flag;
    bool output_flag_present_flag;

    uint32_t num_extra_slice_header_bits;

    bool sign_data_hiding_enabled_flag;
    bool cabac_init_present_flag;

    uint32_t num_ref_idx_l0_default_active_minus1;
    uint32_t num_ref_idx_l1_default_active_minus1;

    int32_t init_qp_minus26;

    // Add fields required by slice-header parsing.
};
```

Exact names/types may be adapted to the existing architecture.

Use a parser context:

```cpp
struct HevcContext
{
    std::map<uint32_t, HevcSps> sps;
    std::map<uint32_t, HevcPps> pps;
    ...
};
```

or equivalent.

---

# 7. SPS parsing

Implement the actual HEVC SPS syntax in the correct order.

At minimum correctly parse:

```text
sps_video_parameter_set_id
sps_max_sub_layers_minus1
sps_temporal_id_nesting_flag
profile_tier_level()
sps_seq_parameter_set_id
chroma_format_idc
separate_colour_plane_flag
pic_width_in_luma_samples
pic_height_in_luma_samples
conformance_window_flag
conformance window offsets
bit_depth_luma_minus8
bit_depth_chroma_minus8
log2_max_pic_order_cnt_lsb_minus4
sps_sub_layer_ordering_info_present_flag
sub-layer ordering fields
```

Additional SPS syntax should be parsed as necessary to correctly reach the end of the SPS.

Do not stop parsing after only the fields needed for display.

The parser must consume syntax in the exact specified order.

---

# 8. profile_tier_level()

Implement `profile_tier_level()` according to HEVC syntax.

It must correctly handle:

```text
general_profile_space
general_tier_flag
general_profile_idc
general_profile_compatibility_flags
general_progressive_source_flag
general_interlaced_source_flag
general_non_packed_constraint_flag
general_frame_only_constraint_flag
general_reserved_zero bits
general_level_idc
sub_layer_profile_present_flag[]
sub_layer_level_present_flag[]
reserved_zero_2bits[]
sub-layer profile fields
sub-layer level fields
```

Do not simply skip an arbitrary number of bits without documenting why.

If the implementation intentionally only extracts selected profile fields, it must still consume all syntax correctly.

---

# 9. SPS sanity checks

After parsing SPS, validate at minimum:

```text
sps_max_sub_layers_minus1 <= 6
chroma_format_idc <= 3
bit_depth_luma_minus8 is reasonable
bit_depth_chroma_minus8 is reasonable
log2_max_pic_order_cnt_lsb_minus4 is reasonable
width > 0
height > 0
```

Do not use arbitrary huge values.

For example:

```text
log2_max_pic_order_cnt_lsb_minus4=2187
```

must never be accepted as a valid SPS field.

If a value is outside the HEVC-constrained range, report a syntax error.

---

# 10. PPS parsing

Implement PPS syntax in the correct order.

At minimum correctly parse:

```text
pps_pic_parameter_set_id
pps_seq_parameter_set_id
dependent_slice_segments_enabled_flag
output_flag_present_flag
num_extra_slice_header_bits
sign_data_hiding_enabled_flag
cabac_init_present_flag
num_ref_idx_l0_default_active_minus1
num_ref_idx_l1_default_active_minus1
init_qp_minus26
constrained_intra_pred_flag
transform_skip_enabled_flag
cu_qp_delta_enabled_flag
diff_cu_qp_delta_depth        // conditional
pps_cb_qp_offset
pps_cr_qp_offset
pps_slice_chroma_qp_offsets_present_flag
weighted_pred_flag
weighted_bipred_flag
transquant_bypass_enabled_flag
tiles_enabled_flag
entropy_coding_sync_enabled_flag
```

Important:

If:

```cpp
cu_qp_delta_enabled_flag == 1
```

then:

```text
diff_cu_qp_delta_depth
```

must be parsed before the QP offsets.

Do not skip conditional syntax.

If tiles are enabled, correctly parse the tile syntax rather than blindly continuing.

---

# 11. VCL slice-header parsing

Implement VCL slice-header parsing using the active PPS and SPS.

The parser must:

1. Parse NAL header.
2. Parse:
   ```text
   first_slice_segment_in_pic_flag
   ```
3. For IRAP:
   ```text
   no_output_of_prior_pics_flag
   ```
4. Parse:
   ```text
   slice_pic_parameter_set_id
   ```
5. Look up the referenced PPS.
6. Look up the SPS referenced by that PPS.
7. Continue parsing according to the PPS/SPS configuration.
8. Parse:
   ```text
   slice_type
   ```
9. Validate:
   ```text
   slice_type ∈ {0,1,2}
   ```
10. Parse POC using the SPS-derived width.

Mapping:

```text
0 = B
1 = P
2 = I
```

Values:

```text
3
4
5
6
7
...
```

must be rejected as invalid `slice_type`.

---

# 12. POC parsing

Never hardcode:

```cpp
b.u(8)
```

for HEVC POC.

The width is derived from:

```text
log2_max_pic_order_cnt_lsb_minus4 + 4
```

Therefore:

```cpp
unsigned poc_bits =
    sps.log2_max_pic_order_cnt_lsb_minus4 + 4;

uint32_t poc_lsb = b.u(poc_bits);
```

Only parse POC where the HEVC slice-header syntax requires it.

Do not assign:

```cpp
poc = 0
```

simply because the NAL is an IRAP NAL.

---

# 13. Slice header must respect PPS fields

Fields such as:

```text
dependent_slice_segments_enabled_flag
output_flag_present_flag
num_extra_slice_header_bits
```

change the slice-header syntax.

For example:

```text
dependent_slice_segments_enabled_flag
```

controls whether:

```text
dependent_slice_segment_flag
```

is present.

`num_extra_slice_header_bits` controls additional reserved slice-header bits.

The parser must consume these fields before reaching `slice_type`.

---

# 14. Access-unit awareness

Do not confuse:

```text
NAL unit
```

with:

```text
picture
```

A single HEVC access unit can contain multiple NAL units:

```text
AUD
PREFIX_SEI
PREFIX_SEI
VPS/SPS/PPS
VCL
SUFFIX_SEI
```

A picture may also contain multiple VCL NAL units.

The NAL scanner must remain responsible only for NAL boundaries unless explicit access-unit detection is implemented.

Do not modify the public scanner semantics unnecessarily.

---

# 15. Annex-B scanner correctness

Review:

```cpp
find_start_code_scalar()
scan_annexb()
find_annexb_start_codes()
```

The scanner must correctly support:

```text
00 00 01
00 00 00 01
```

and must work when the input arrives in arbitrary chunks.

Pay particular attention to the fact that the current implementation uses a shared mutable:

```cpp
size_t startCodeSize
```

for multiple searches.

The start-code size of the current delimiter and the next delimiter must not be confused.

Test sequences such as:

```text
00 00 01 NAL
00 00 00 01 NAL
00 00 01 NAL
```

and arbitrary chunk boundaries inside start codes:

```text
00
00 01
```

```text
00 00
01
```

```text
00 00 00
01
```

---

# 16. Unit extraction must not modify NAL payload

The extracted `Unit.bytes` for HEVC must contain:

```text
NAL header + EBSP payload
```

but not:

```text
Annex-B start code
```

RBSP conversion should happen only during syntax parsing.

Do not permanently remove emulation-prevention bytes from `Unit.bytes`.

---

# 17. Offset correctness

Preserve existing offset semantics.

Verify:

```text
Unit.offset
Header.offset
Header.length
Unit.start_code_size
```

against actual input positions.

Tests must include both:

```text
3-byte start code
4-byte start code
```

and mixed streams.

---

# 18. Preserve other codecs

Do not rewrite AVC, VP8, VP9, or AV1 parsing unless a change is directly required by shared infrastructure.

The primary target is:

```cpp
Codec::HEVC
```

VVC code should not be silently changed to use HEVC syntax.

---

# 19. Recommended architecture

Refactor HEVC into smaller functions instead of keeping all syntax inside one large `parse_hevc()`.

Suggested structure:

```cpp
parse_hevc_nal_header()

parse_hevc_vps()

parse_hevc_sps()

parse_hevc_pps()

parse_hevc_slice_header()

parse_hevc_aud()

parse_hevc_sei()
```

Supporting helpers:

```cpp
parse_hevc_profile_tier_level()

ebsp_to_rbsp()

validate_hevc_sps()

validate_hevc_pps()

lookup_hevc_sps()

lookup_hevc_pps()
```

A context/state object should be passed through the parser:

```cpp
HevcContext&
```

rather than using global state.

---

# 20. SEI handling

For:

```text
nal_unit_type=39
```

identify it as:

```text
PREFIX_SEI
```

For:

```text
nal_unit_type=40
```

identify it as:

```text
SUFFIX_SEI
```

Do not attempt to interpret arbitrary SEI payload bits as VPS/SPS/PPS/slice syntax.

At minimum, parse the SEI message structure correctly or expose the NAL as SEI without inventing fields.

---

# 21. AUD handling

For:

```text
nal_unit_type=35
```

parse:

```text
pic_type
```

as 3 bits.

Valid values are:

```text
0
1
2
3
4
5
6
7
```

Do not interpret AUD payload as slice syntax.

---

# 22. Regression tests

Create tests for at least:

## NAL header

Verify:

```text
type 32 → VPS
type 33 → SPS
type 34 → PPS
type 35 → AUD
type 39 → PREFIX_SEI
type 40 → SUFFIX_SEI
```

## Slice types

Verify:

```text
0 → B
1 → P
2 → I
```

and invalid:

```text
3
5
7
```

must fail parsing rather than return `"Unknown"`.

## SPS

Verify that a valid SPS does not produce values such as:

```text
sps_seq_parameter_set_id = 268436479
log2_max_pic_order_cnt_lsb_minus4 = 2187
height = 4269770681
```

## PPS

Verify reasonable values for:

```text
pps_pic_parameter_set_id
pps_seq_parameter_set_id
num_ref_idx_l0_default_active_minus1
num_ref_idx_l1_default_active_minus1
```

and test:

```text
cu_qp_delta_enabled_flag = 0
cu_qp_delta_enabled_flag = 1
```

## VCL

Verify that a VCL NAL referencing PPS ID `X` actually resolves to the stored PPS `X`.

Verify that the PPS references an existing SPS.

Missing references must generate a parse error.

## POC

Create tests with different:

```text
log2_max_pic_order_cnt_lsb_minus4
```

values and verify that POC is read using:

```text
value + 4
```

bits rather than a hardcoded 8 bits.

## Annex-B

Test:

```text
00 00 01
00 00 00 01
```

including mixed prefixes and chunked input.

---

# 23. Required debug instrumentation during development

During implementation, temporarily log:

```text
NAL type
RBSP size
bit position before each major syntax structure
bit position after each major syntax structure
SPS ID
PPS ID
SPS referenced by PPS
slice PPS ID
slice type
POC bit width
POC LSB
```

For SPS specifically:

```text
before profile_tier_level
after profile_tier_level
before sps_seq_parameter_set_id
after sps_seq_parameter_set_id
```

For a stream with:

```text
sps_max_sub_layers_minus1 = 0
```

the bit position immediately before:

```text
sps_seq_parameter_set_id
```

should be:

```text
104
```

because:

```text
4 + 3 + 1 + 96 = 104
```

This diagnostic should be removed or disabled after the parser is validated.

---

# 24. Do not "fix" invalid output with heuristics

Do NOT implement fixes such as:

```cpp
if (slice_type > 2)
    slice_type = 2;
```

or:

```cpp
if (poc > 1000)
    poc = 0;
```

or:

```cpp
if (sps_id > 31)
    sps_id = 0;
```

or:

```cpp
if (width == 0)
    width = previous_width;
```

These hide synchronization bugs.

The parser must consume the correct number of bits and reject malformed input.

---

# 25. Success Criteria

The refactor is complete only when:

### NAL detection

- [ ] 3-byte Annex-B prefixes work.
- [ ] 4-byte Annex-B prefixes work.
- [ ] Mixed prefix lengths work.
- [ ] Chunked input works.
- [ ] NAL payload does not include start codes.

### NAL headers

- [ ] forbidden_zero_bit validated.
- [ ] NAL type correctly extracted.
- [ ] layer ID correctly extracted.
- [ ] temporal ID correctly extracted.
- [ ] HEVC type names 0–40 are correct.

### RBSP

- [ ] Emulation-prevention bytes correctly removed.
- [ ] Raw `Unit.bytes` remains EBSP.
- [ ] BitReader reads RBSP.

### VPS

- [ ] VPS fields parsed in correct order.
- [ ] VPS syntax is consumed correctly.

### SPS

- [ ] PTL parsed/skipped correctly.
- [ ] SPS ID correct.
- [ ] chroma format correct.
- [ ] dimensions correct.
- [ ] bit depth correct.
- [ ] POC LSB width correct.
- [ ] SPS stored in parser context.

### PPS

- [ ] PPS ID correct.
- [ ] SPS reference correct.
- [ ] conditional syntax handled.
- [ ] PPS stored in parser context.

### VCL

- [ ] PPS lookup works.
- [ ] SPS lookup works.
- [ ] slice type is restricted to 0/1/2.
- [ ] POC width comes from SPS.
- [ ] IRAP syntax handled correctly.
- [ ] dependent slice syntax handled.
- [ ] extra slice header bits handled.

### Error handling

- [ ] No silent swallowing of HEVC syntax errors.
- [ ] No guessed values.
- [ ] Invalid references fail clearly.
- [ ] Truncated RBSP fails clearly.

### Regression

- [ ] Existing non-HEVC tests continue to pass.
- [ ] Existing public API remains compatible where practical.
- [ ] The previously observed invalid values no longer occur on the supplied HEVC stream.

---

# Final Implementation Principle

The parser should follow this rule:

```text
Never interpret a field until all syntax elements preceding it
have been consumed according to the HEVC specification.
```

In particular:

```text
VCL slice
    ↓
slice_pic_parameter_set_id
    ↓
PPS lookup
    ↓
SPS lookup
    ↓
PPS/SPS-dependent syntax
    ↓
slice_type
    ↓
POC and remaining slice-header syntax
```

Do not parse HEVC VCL headers as independent self-contained NALs.

The goal is not merely to make the displayed values "look reasonable".

The goal is to maintain **bit-exact syntax synchronization with the HEVC RBSP** throughout the entire NAL.

