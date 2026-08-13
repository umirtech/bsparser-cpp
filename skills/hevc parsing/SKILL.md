Absolutely. For an H.265/HEVC parser, I’d make `SKILLS.md` much more than a list of syntax elements—it should encode the **mental model, parsing rules, invariants, error handling, implementation architecture, and debugging methodology** needed to work safely on raw HEVC bitstreams.

# H.265 / HEVC Bitstream Parsing — Engineering Skill

## Purpose

This skill defines how to correctly analyze, implement, debug, extend, and validate an H.265 / HEVC bitstream parser in C++20.

The parser may operate at several layers:

1. Byte-stream / Annex B extraction
2. NAL unit parsing
3. RBSP extraction
4. Bit-level syntax parsing
5. Parameter-set parsing
6. Slice-header parsing
7. SEI parsing
8. Picture / reference-picture state derivation
9. Profile, level, tier, VUI, and HRD interpretation
10. Structural validation and debugging

The primary objective is **bitstream correctness**.

A parser must never assume that a field is byte-aligned unless the HEVC syntax explicitly guarantees alignment.

---

# 1. Core Mental Model

An HEVC elementary stream is not simply:

```text
bytes -> fields
```

The actual structure is:

```text
byte stream
    |
    +-- Annex B start-code / length framing
    |
    v
NAL unit
    |
    +-- NAL header
    |
    +-- EBSP
          |
          +-- emulation-prevention removal
          |
          v
        RBSP
          |
          +-- bit-level syntax
          |
          +-- rbsp_trailing_bits
```

For a parser, these are separate concepts and must remain separate in code.

Recommended abstraction:

```text
Input bytes
    ↓
NAL extractor
    ↓
NAL header
    ↓
EBSP → RBSP
    ↓
BitReader
    ↓
Syntax parser
    ↓
Parsed structures
    ↓
Derived state / validation
```

Do not mix Annex B handling, emulation-prevention removal, and syntax parsing into one function.

---

# 2. HEVC NAL Unit Structure

An HEVC NAL unit has:

```text
NAL unit
+-------------------------+
| 2-byte NAL header       |
+-------------------------+
| RBSP payload / EBSP     |
+-------------------------+
```

The two-byte NAL header contains:

```text
forbidden_zero_bit       1 bit
nal_unit_type            6 bits
nuh_layer_id              6 bits
nuh_temporal_id_plus1    3 bits
```

Total:

```text
1 + 6 + 6 + 3 = 16 bits
```

The NAL header must be parsed independently of the RBSP payload.

Important:

```text
nuh_temporal_id_plus1 != temporal_id
```

The actual temporal identifier is:

```text
TemporalId = nuh_temporal_id_plus1 - 1
```

A value of zero for `nuh_temporal_id_plus1` is invalid.

The parser should validate:

```cpp
forbidden_zero_bit == 0
nuh_temporal_id_plus1 >= 1
```

---

# 3. Important HEVC NAL Unit Types

Common NAL unit types:

```text
0  TRAIL_N
1  TRAIL_R

2  TSA_N
3  TSA_R

4  STSA_N
5  STSA_R

6  RADL_N
7  RADL_R

8  RASL_N
9  RASL_R

16 BLA_W_LP
17 BLA_W_RADL
18 BLA_N_LP

19 IDR_W_RADL
20 IDR_N_LP

21 CRA_NUT

32 VPS_NUT
33 SPS_NUT
34 PPS_NUT

35 AUD_NUT
36 EOS_NUT
37 EOB_NUT

38 FD_NUT
39 PREFIX_SEI_NUT
40 SUFFIX_SEI_NUT
```

Do not hard-code only:

```text
32 = VPS
33 = SPS
34 = PPS
```

A robust parser should preserve unknown/reserved NAL types instead of crashing.

For debugging, always print:

```text
NAL type
NAL name
layer ID
temporal ID
payload size
```

Example:

```text
NAL type = 33
SPS
layer_id = 0
temporal_id = 0
payload_bytes = 27
```

---

# 4. Annex B Parsing

Annex B streams typically contain:

```text
00 00 01
```

or:

```text
00 00 00 01
```

start codes.

The parser must detect both:

```text
0x000001
0x00000001
```

Do not assume every NAL uses the four-byte form.

Typical stream:

```text
00 00 00 01 [NAL]
00 00 01    [NAL]
00 00 01    [NAL]
```

The start code is framing and is not part of the NAL payload.

Correct conceptual operation:

```text
find start code
    ↓
start of NAL = byte immediately after start code
    ↓
find next start code
    ↓
NAL bytes = current NAL start ... before next start code
```

Trailing zero bytes before a start code require care.

Do not accidentally feed the start-code bytes into the NAL header parser.

---

# 5. Length-Prefixed Streams

Not every HEVC stream is Annex B.

Containers commonly use length-prefixed NAL units.

Example:

```text
[length][NAL]
[length][NAL]
[length][NAL]
```

The parser architecture should therefore separate:

```cpp
AnnexBExtractor
LengthPrefixedExtractor
NalUnit
```

from:

```cpp
NalParser
```

The NAL parser should not care how the NAL was framed.

---

# 6. EBSP and RBSP

HEVC uses emulation-prevention bytes.

The encoded payload is generally:

```text
RBSP
  ↓
emulation prevention
  ↓
EBSP
```

To parse syntax, the EBSP must be converted to RBSP.

The relevant pattern is associated with:

```text
00 00 03
```

The `03` is an emulation-prevention byte and is removed when reconstructing the RBSP.

Example:

```text
00 00 03 01
```

becomes:

```text
00 00 01
```

The parser must not blindly remove every `03`.

The removal rule depends on the preceding bytes and position.

A common implementation rule is:

```cpp
if (zero_count >= 2 && byte == 0x03) {
    skip byte;
    zero_count = 0;
}
```

but implementation must still follow the exact HEVC byte-stream/RBSP rules rather than treating arbitrary `03` bytes as removable.

Keep:

```cpp
ebsp
rbsp
```

as separate representations.

This makes debugging dramatically easier.

---

# 7. RBSP Trailing Bits

Most RBSP syntax ends with:

```text
rbsp_stop_one_bit = 1
rbsp_alignment_zero_bit*
```

Conceptually:

```text
... syntax bits
1
0
0
0
...
```

The stop bit is not another syntax field.

A parser must distinguish:

```text
actual syntax
```

from:

```text
rbsp_trailing_bits
```

Never consume trailing bits as if they belonged to the final syntax element.

---

# 8. BitReader Requirements

The `BitReader` is the foundation of the entire parser.

It should support at minimum:

```cpp
uint32_t readBit();
uint32_t readBits(unsigned n);
uint32_t peekBits(unsigned n);
void skipBits(unsigned n);

uint32_t readUE();
int32_t readSE();

bool byteAligned() const;
size_t bitsRemaining() const;
size_t bitPosition() const;
```

Recommended additional operations:

```cpp
bool canRead(size_t n);
uint64_t peekBits64(unsigned n);
void alignToByte();
```

The reader must never silently read beyond the RBSP.

Prefer:

```cpp
Expected<T, ParseError>
```

or equivalent error propagation over undefined behavior.

---

# 9. Bit Ordering

HEVC syntax is MSB-first.

For:

```text
10110110
```

the first bit read is:

```text
1
```

then:

```text
0
```

then:

```text
1
```

Do not implement the reader as LSB-first.

A basic operation:

```cpp
uint32_t readBits(unsigned n)
{
    uint32_t result = 0;

    for (unsigned i = 0; i < n; ++i) {
        result = (result << 1) | readBit();
    }

    return result;
}
```

An optimized implementation may use a bit cache, but correctness comes first.

---

# 10. Unsigned Exp-Golomb — `ue(v)`

HEVC uses unsigned Exp-Golomb syntax extensively.

For a codeword:

```text
0001001
```

count leading zeros:

```text
000
```

Then:

```text
leadingZeroBits = 3
suffix = 001
```

Value:

```text
(1 << leadingZeroBits) - 1 + suffix
```

Therefore:

```text
codeNum = 4 + 1 = 5
```

General algorithm:

```cpp
uint32_t readUE()
{
    unsigned leadingZeros = 0;

    while (readBit() == 0) {
        ++leadingZeros;
    }

    if (leadingZeros == 0)
        return 0;

    uint32_t suffix = readBits(leadingZeros);

    return ((1u << leadingZeros) - 1u) + suffix;
}
```

However, production code must guard against excessive leading zeros and integer overflow.

Do not use:

```cpp
1u << leadingZeros
```

without checking the width.

---

# 11. Signed Exp-Golomb — `se(v)`

Signed mapping:

```text
codeNum = 0 -> 0
codeNum = 1 -> +1
codeNum = 2 -> -1
codeNum = 3 -> +2
codeNum = 4 -> -2
...
```

Equivalent:

```cpp
int32_t readSE()
{
    uint32_t codeNum = readUE();

    if (codeNum & 1)
        return static_cast<int32_t>((codeNum + 1) / 2);

    return -static_cast<int32_t>(codeNum / 2);
}
```

Again, overflow must be handled explicitly.

---

# 12. `u(n)` Syntax

A syntax element such as:

```text
u(4)
```

means:

```text
read exactly 4 bits
```

It is not Exp-Golomb.

Examples:

```text
u(1)
u(2)
u(3)
u(4)
u(6)
u(8)
```

Always distinguish:

```text
u(n)
ue(v)
se(v)
```

---

# 13. Fixed-Width Dynamic Syntax

Some syntax fields have widths derived from previously parsed values.

Example:

```text
u(v)
```

where the width is computed from another syntax value.

The implementation should calculate the width explicitly:

```cpp
unsigned width = ...;
uint32_t value = br.readBits(width);
```

Never accidentally treat dynamic-width syntax as `ue(v)`.

---

# 14. `ceil(log2())` and `CeilLog2`

HEVC syntax frequently requires:

```text
CeilLog2(x)
```

Do not use floating-point `log2()` for this.

Use integer arithmetic.

For:

```text
x > 0
```

the mathematical result is:

```text
ceil(log2(x))
```

A safe implementation:

```cpp
unsigned ceilLog2(uint32_t x)
{
    if (x <= 1)
        return 0;

    unsigned n = 0;
    --x;

    while (x) {
        x >>= 1;
        ++n;
    }

    return n;
}
```

This matters for syntax such as:

```text
slice_pic_parameter_set_id
pic_parameter_set_id
```

and other bounded identifiers.

---

# 15. `more_rbsp_data()`

A parser cannot simply ask:

```cpp
bitsRemaining() > 0
```

to determine whether more syntax data exists.

The remaining bits may consist only of:

```text
rbsp_stop_one_bit
rbsp_alignment_zero_bit
```

Therefore a conceptual helper:

```cpp
bool moreRbspData()
```

must detect whether the remaining bits contain syntax data rather than only the trailing-bit pattern.

This is especially important for:

```text
SEI
VUI
HRD
scaling_list_data
```

and other variable-length syntax structures.

---

# 16. `rbsp_trailing_bits()`

Typical implementation:

```cpp
void rbspTrailingBits(BitReader& br)
{
    br.readBit(); // rbsp_stop_one_bit

    while (!br.byteAligned())
        br.readBit(); // rbsp_alignment_zero_bit
}
```

Validation should verify:

```text
stop bit == 1
alignment bits == 0
```

unless the surrounding syntax specifies another alignment mechanism.

---

# 17. NAL Parsing Architecture

Recommended C++20 design:

```text
HevcBitstream
    |
    +-- NalExtractor
    |
    +-- NalUnit
           |
           +-- NalHeader
           |
           +-- Ebsp
           +-- Rbsp
                    |
                    +-- BitReader
```

Then syntax-specific parsers:

```text
VpsParser
SpsParser
PpsParser
SliceHeaderParser
SeiParser
AudParser
```

Avoid one giant:

```cpp
parseEverything()
```

function.

---

# 18. VPS

NAL type:

```text
32
```

The Video Parameter Set describes information shared across layers and temporal sub-layers.

Important fields include:

```text
vps_video_parameter_set_id
vps_base_layer_internal_flag
vps_base_layer_available_flag
vps_max_layers_minus1
vps_max_sub_layers_minus1
vps_temporal_id_nesting_flag
profile_tier_level()
vps_sub_layer_ordering_info_present_flag
vps_max_dec_pic_buffering_minus1[]
vps_max_num_reorder_pics[]
vps_max_latency_increase_plus1[]
vps_max_layer_id
vps_num_layer_sets_minus1
...
```

Do not parse VPS fields in isolation.

Several arrays have lengths derived from:

```text
vps_max_sub_layers_minus1
```

and:

```text
vps_sub_layer_ordering_info_present_flag
```

Be careful about the conditional loop start.

---

# 19. Profile, Tier and Level

`profile_tier_level()` is reused in VPS and SPS contexts.

It contains:

```text
general_profile_space
general_tier_flag
general_profile_idc
general_profile_compatibility_flags
general_progressive_source_flag
general_interlaced_source_flag
general_non_packed_constraint_flag
general_frame_only_constraint_flag
...
general_level_idc
```

There can also be sub-layer profile/level information.

Do not assume:

```text
general_level_idc
```

is always present at the same location without first parsing the associated flags and loop structure.

Create a reusable parser:

```cpp
ProfileTierLevel parseProfileTierLevel(
    BitReader& br,
    unsigned maxSubLayersMinus1,
    bool profilePresentFlag
);
```

---

# 20. SPS

NAL type:

```text
33
```

The Sequence Parameter Set is one of the most important structures.

Typical syntax includes:

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
conf_win_left_offset
conf_win_right_offset
conf_win_top_offset
conf_win_bottom_offset

bit_depth_luma_minus8
bit_depth_chroma_minus8

log2_max_pic_order_cnt_lsb_minus4

sps_sub_layer_ordering_info_present_flag
...

log2_min_luma_coding_block_size_minus3
log2_diff_max_min_luma_coding_block_size
log2_min_luma_transform_block_size_minus2
log2_diff_max_min_luma_transform_block_size

max_transform_hierarchy_depth_inter
max_transform_hierarchy_depth_intra

scaling_list_enabled_flag
...

amp_enabled_flag
sample_adaptive_offset_enabled_flag
pcm_enabled_flag

num_short_term_ref_pic_sets
short_term_ref_pic_set()

long_term_ref_pics_present_flag
...

sps_temporal_mvp_enabled_flag
strong_intra_smoothing_enabled_flag

vui_parameters_present_flag
vui_parameters()
```

The exact parsing order must follow the HEVC syntax.

Never rearrange fields because they "logically belong together."

---

# 21. SPS Dimensions

A common debugging trap:

```text
pic_width_in_luma_samples
pic_height_in_luma_samples
```

are not necessarily the final display dimensions.

For example:

```text
pic_width_in_luma_samples = 960
pic_height_in_luma_samples = 400
```

may still be subject to:

```text
conformance_window_flag
```

Therefore distinguish:

```cpp
codedWidth
codedHeight
```

from:

```cpp
displayWidth
displayHeight
```

and calculate the conformance-window crop using the chroma format and associated subsampling factors.

Do not simply subtract offsets as if every chroma format used four-byte or one-byte units.

---

# 22. Chroma Format

Important values:

```text
chroma_format_idc = 0 -> monochrome
chroma_format_idc = 1 -> 4:2:0
chroma_format_idc = 2 -> 4:2:2
chroma_format_idc = 3 -> 4:4:4
```

For:

```text
chroma_format_idc == 3
```

the parser may encounter:

```text
separate_colour_plane_flag
```

Do not assume 4:4:4 automatically means three independently coded planes.

---

# 23. Bit Depth

Syntax:

```text
bit_depth_luma_minus8
bit_depth_chroma_minus8
```

Actual bit depth:

```cpp
bitDepthLuma = bit_depth_luma_minus8 + 8;
bitDepthChroma = bit_depth_chroma_minus8 + 8;
```

Never report the raw `minus8` value as the bit depth.

---

# 24. PPS

NAL type:

```text
34
```

Important PPS syntax includes:

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

diff_cu_qp_delta_depth

pps_cb_qp_offset
pps_cr_qp_offset

pps_slice_chroma_qp_offsets_present_flag
weighted_pred_flag
weighted_bipred_flag

transquant_bypass_enabled_flag
tiles_enabled_flag
entropy_coding_sync_enabled_flag
...

loop_filter_across_tiles_enabled_flag
pps_loop_filter_across_slices_enabled_flag
deblocking_filter_control_present_flag
...

pps_scaling_list_data_present_flag
lists_modification_present_flag
log2_parallel_merge_level_minus2

slice_segment_header_extension_present_flag
```

PPS parsing must preserve all conditional dependencies.

---

# 25. Slice Header Parsing

Slice headers are significantly more complicated than VPS/SPS/PPS because they depend on parameter sets.

A slice parser typically needs:

```text
NAL header
SPS
PPS
slice type
picture order count state
reference-picture state
short-term RPS
long-term references
reference-list modification
weighted prediction
merge candidates
deblocking state
tiles / WPP state
```

Do not parse a slice header without resolving:

```text
slice_pic_parameter_set_id
```

to a known PPS, and then:

```text
pps_seq_parameter_set_id
```

to a known SPS.

---

# 26. Slice Segment Header

Important conditional structure:

```text
first_slice_segment_in_pic_flag
```

If:

```text
first_slice_segment_in_pic_flag == 0
```

additional fields become relevant.

For dependent slice segments:

```text
dependent_slice_segment_flag
```

must be interpreted according to PPS configuration.

The parser must distinguish:

```text
slice segment
```

from:

```text
independent slice
```

and not assume every slice segment contains a complete header.

---

# 27. Slice Type

The field:

```text
slice_type
```

typically maps to:

```text
0 = B
1 = P
2 = I
```

A useful enum:

```cpp
enum class SliceType {
    B = 0,
    P = 1,
    I = 2
};
```

Never reverse the values.

---

# 28. Picture Order Count

POC parsing is one of the most error-prone parts of HEVC.

Relevant fields include:

```text
pic_order_cnt_lsb
short_term_ref_pic_set_sps_flag
short_term_ref_pic_set()
num_long_term_sps
num_long_term_pics
lt_idx_sps[]
poc_lsb_lt[]
used_by_curr_pic_lt_flag[]
delta_poc_msb_present_flag[]
delta_poc_msb_cycle_lt[]
```

Do not treat:

```text
pic_order_cnt_lsb
```

as the final POC.

The full POC depends on previous picture state and:

```text
log2_max_pic_order_cnt_lsb
```

A parser that only reports `pic_order_cnt_lsb` should label it clearly:

```text
POC LSB
```

not:

```text
POC
```

---

# 29. Short-Term Reference Picture Sets

`short_term_ref_pic_set()` is highly conditional.

It may be represented directly or derived from a previous RPS.

Important fields:

```text
inter_ref_pic_set_prediction_flag
delta_idx_minus1
delta_rps_sign
abs_delta_rps_minus1

used_by_curr_pic_flag[]
use_delta_flag[]

num_negative_pics
num_positive_pics

delta_poc_s0_minus1[]
used_by_curr_pic_s0_flag[]

delta_poc_s1_minus1[]
used_by_curr_pic_s1_flag[]
```

The parser should preferably produce a normalized representation:

```cpp
struct ShortTermRps {
    std::vector<int> negativeDeltaPoc;
    std::vector<int> positiveDeltaPoc;

    std::vector<bool> usedByCurrentPicture;
};
```

This makes later POC/reference processing easier.

---

# 30. Long-Term References

Long-term reference syntax depends on SPS and slice state.

Do not allocate fixed arrays unless the specification guarantees the bound.

Validate counts against:

```text
num_long_term_ref_pics_sps
num_long_term_pics
```

Store the actual derived POC information separately from the encoded syntax.

---

# 31. Reference Picture Lists

For P and B slices, reference picture lists may be modified.

Important concepts:

```text
RefPicList0
RefPicList1
```

and:

```text
num_ref_idx_l0_active_minus1
num_ref_idx_l1_active_minus1
```

Do not confuse:

```text
encoded reference index
```

with:

```text
POC of the referenced picture
```

A parser can initially expose both:

```text
L0 index
L0 POC
```

when reference state is available.

---

# 32. Tiles

If:

```text
tiles_enabled_flag == 1
```

the PPS contains tile configuration.

Important fields include:

```text
num_tile_columns_minus1
num_tile_rows_minus1
uniform_spacing_flag
column_width_minus1[]
row_height_minus1[]
loop_filter_across_tiles_enabled_flag
```

The parser should derive actual tile dimensions rather than forcing users to interpret `minus1` syntax fields.

For example:

```cpp
tileColumnWidth[i]
tileRowHeight[i]
```

should contain actual CTB counts.

---

# 33. Wavefront Parallel Processing

WPP is controlled by:

```text
entropy_coding_sync_enabled_flag
```

When enabled, CTB rows can have entropy synchronization points.

Do not interpret WPP as tiles.

They are different mechanisms:

```text
Tiles -> spatial partitioning
WPP   -> entropy synchronization between CTB rows
```

---

# 34. CTB Geometry

Many HEVC dimensions are expressed in Coding Tree Blocks.

Important derived values include:

```text
CtbLog2SizeY
CtbSizeY

PicWidthInCtbsY
PicHeightInCtbsY

PicSizeInCtbsY
```

These are derived from SPS parameters.

Do not use:

```text
pic_width_in_luma_samples / 64
```

unconditionally.

The maximum CTB size is commonly 64, but actual CTB size is determined by SPS syntax.

---

# 35. Coding Block Geometry

Important SPS parameters:

```text
log2_min_luma_coding_block_size_minus3
log2_diff_max_min_luma_coding_block_size
```

Derived:

```text
Log2MinCbSizeY
Log2CtbSizeY
CtbSizeY
```

A parser should expose both encoded and derived forms where useful.

Example:

```cpp
struct CodingTreeGeometry {
    unsigned minCbLog2;
    unsigned ctbLog2;
    unsigned ctbSize;
    unsigned widthInCtbs;
    unsigned heightInCtbs;
};
```

---

# 36. VUI

VUI is optional:

```text
vui_parameters_present_flag
```

Important VUI information can include:

```text
aspect_ratio_info_present_flag
aspect_ratio_idc
sar_width
sar_height

overscan_info_present_flag
overscan_appropriate_flag

video_signal_type_present_flag
video_format
video_full_range_flag
colour_description_present_flag

colour_primaries
transfer_characteristics
matrix_coefficients

chroma_loc_info_present_flag
chroma_sample_loc_type_top_field
chroma_sample_loc_type_bottom_field

neutral_chroma_indication_flag
field_seq_flag
frame_field_info_present_flag

default_display_window_flag
def_disp_win_left_offset
...

timing_info_present_flag
num_units_in_tick
time_scale
poc_proportional_to_timing_flag

hrd_parameters_present_flag
bitstream_restriction_flag
```

Do not confuse:

```text
SPS coded dimensions
```

with:

```text
VUI display properties
```

---

# 37. Aspect Ratio

If:

```text
aspect_ratio_info_present_flag == 1
```

then:

```text
aspect_ratio_idc
```

determines the interpretation.

For extended SAR:

```text
aspect_ratio_idc == 255
```

the stream supplies:

```text
sar_width
sar_height
```

Do not simply display `aspect_ratio_idc` as an aspect ratio.

Convert it to a meaningful:

```text
SAR
DAR
```

only when the necessary dimensions and semantics are available.

---

# 38. Colour Information

If:

```text
colour_description_present_flag == 1
```

then parse:

```text
colour_primaries
transfer_characteristics
matrix_coefficients
```

Also preserve:

```text
video_full_range_flag
```

These values are metadata, not pixel data.

Never infer HDR/SDR solely from one field without considering the complete signaling.

---

# 39. Timing Information

Timing information can include:

```text
num_units_in_tick
time_scale
```

A nominal frame-rate calculation may involve:

```text
time_scale / (2 * num_units_in_tick)
```

depending on the HEVC timing semantics and relevant flags.

Do not label this blindly as the actual displayed FPS.

Use terminology such as:

```text
signaled frame rate
nominal frame rate
timing parameters
```

until all relevant syntax has been interpreted.

---

# 40. HRD

HRD parsing is optional and deeply conditional.

Do not create a simplified parser that assumes:

```text
one sub-layer
one CPB
no sub-picture HRD
```

unless the application explicitly supports only that profile.

Important structures include:

```text
nal_hrd_parameters_present_flag
vcl_hrd_parameters_present_flag
sub_pic_hrd_params_present_flag

bit_rate_scale
cpb_size_scale
cpb_size_du_scale

initial_cpb_removal_delay_length_minus1
au_cpb_removal_delay_length_minus1
dpb_output_delay_length_minus1

fixed_pic_rate_general_flag[]
fixed_pic_rate_within_cvs_flag[]
elemental_duration_in_tc_minus1[]
low_delay_hrd_flag[]
cpb_cnt_minus1[]

bit_rate_value_minus1[]
cpb_size_value_minus1[]
cbr_flag[]
```

Keep HRD parsing isolated from basic SPS parsing.

---

# 41. Scaling Lists

Scaling-list parsing is another area where conditional loops are easy to get wrong.

The parser must account for:

```text
sizeId
matrixId
pred_mode_flag
pred_matrix_id_delta
dc_coef_minus8
scaling_list_delta_coef
```

Do not hard-code only the 4x4 matrix.

HEVC scaling lists cover multiple sizes and prediction modes.

Represent the final result in a normalized matrix structure:

```cpp
struct ScalingList {
    std::array<int, 64> coefficients;
    int dcCoefficient;
};
```

or an equivalent size-aware representation.

---

# 42. PCM

If:

```text
pcm_enabled_flag == 1
```

additional syntax is present.

Important:

```text
pcm_sample_bit_depth_luma_minus1
pcm_sample_bit_depth_chroma_minus1
log2_min_pcm_luma_coding_block_size_minus3
log2_diff_max_min_pcm_luma_coding_block_size
pcm_loop_filter_disabled_flag
```

A syntax parser does not need to decode PCM samples unless pixel decoding is required, but it must correctly skip/read them when parsing the associated coding-tree syntax.

---

# 43. SAO

Sample Adaptive Offset is signaled through SPS/PPS/slice-related state.

At high level:

```text
sample_adaptive_offset_enabled_flag
```

does not mean every slice necessarily contains SAO parameters.

Do not interpret an enable flag as if the feature is always active.

Always follow the conditional syntax chain.

---

# 44. Deblocking Filter

Deblocking configuration can originate in PPS and be overridden at slice level.

Important fields include:

```text
deblocking_filter_control_present_flag
deblocking_filter_override_enabled_flag
pps_deblocking_filter_disabled_flag

slice_deblocking_filter_disabled_flag
slice_beta_offset_div2
slice_tc_offset_div2
```

Maintain both:

```text
PPS defaults
```

and:

```text
slice override
```

rather than overwriting one structure prematurely.

---

# 45. SEI

SEI NAL units:

```text
39 = prefix SEI
40 = suffix SEI
```

SEI is structured as messages.

Each message has:

```text
payloadType
payloadSize
payload
```

Payload type and payload size use repeated `0xFF` extension bytes.

Conceptually:

```cpp
uint32_t readSeiValue()
{
    uint32_t value = 0;
    uint8_t byte;

    do {
        byte = readByte();
        value += byte;
    } while (byte == 0xFF);

    return value;
}
```

The exact SEI parsing must preserve payload boundaries.

Never let a malformed SEI payload consume the next SEI message.

---

# 46. Important SEI Types

A useful parser can support:

```text
buffering_period
pic_timing
recovery_point
user_data_registered_itu_t_t35
user_data_unregistered
mastering_display_colour_volume
content_light_level_info
alternative_transfer_characteristics
```

and retain unknown SEIs as:

```cpp
UnknownSei {
    uint32_t payloadType;
    std::vector<uint8_t> payload;
};
```

Unknown SEI types must not break the stream parser.

---

# 47. `user_data_unregistered`

This SEI contains:

```text
uuid_iso_iec_11578
user_data_payload_byte[]
```

The UUID is 16 bytes.

Do not interpret arbitrary UUID payload as text unless explicitly requested.

Expose:

```text
UUID
payload size
raw payload
optional printable representation
```

---

# 48. Mastering Display Metadata

`mastering_display_colour_volume` commonly carries:

```text
display_primaries_x[3]
display_primaries_y[3]
white_point_x
white_point_y
max_display_mastering_luminance
min_display_mastering_luminance
```

These are coded values with defined scaling.

A parser should preserve both:

```text
raw integer value
```

and, if convenient:

```text
physical/display value
```

Do not discard the raw representation.

---

# 49. Content Light Level

For content light level metadata:

```text
MaxCLL
MaxFALL
```

must be represented with their correct semantics and units.

Do not confuse them with mastering-display luminance.

---

# 50. Access Units

An access unit is a higher-level concept than a NAL unit.

A stream can contain multiple NAL units belonging to one picture/access unit:

```text
AUD
VPS
SPS
PPS
SEI
slice
slice
SEI
...
```

A parser should not assume:

```text
one NAL = one frame
```

For frame-level analysis, maintain:

```cpp
AccessUnit
```

containing:

```cpp
std::vector<NalUnit>
```

and optionally:

```text
POC
IRAP status
random-access status
reference status
```

---

# 51. IRAP Pictures

Important random-access picture classes include:

```text
BLA
IDR
CRA
```

Do not reduce everything to:

```text
isKeyframe
```

Internally preserve the exact IRAP type.

For example:

```cpp
enum class IrapType {
    None,
    BLA_W_LP,
    BLA_W_RADL,
    BLA_N_LP,
    IDR_W_RADL,
    IDR_N_LP,
    CRA
};
```

---

# 52. Parameter Set Storage

Parameter sets are identified by IDs.

Recommended storage:

```cpp
std::unordered_map<uint32_t, VPS> vps;
std::unordered_map<uint32_t, SPS> sps;
std::unordered_map<uint32_t, PPS> pps;
```

But SPS also references VPS, and PPS references SPS.

Therefore validation should include:

```text
PPS -> SPS exists
SPS -> VPS exists
slice -> PPS exists
PPS -> SPS exists
SPS -> VPS exists
```

Do not silently create default parameter sets.

---

# 53. Parameter Set Lifetime

Parameter sets can appear more than once.

A new VPS/SPS/PPS with the same ID may replace previous state.

When replacing parameter sets:

```text
validate ID
parse complete structure
validate dependencies
commit new structure
```

Prefer parse-then-commit rather than modifying active state field-by-field.

This prevents partially parsed parameter sets from becoming visible.

---

# 54. Parser State

Maintain explicit stream state:

```cpp
struct HevcContext {
    std::unordered_map<int, VPS> vps;
    std::unordered_map<int, SPS> sps;
    std::unordered_map<int, PPS> pps;

    PictureState picture;
    ReferencePictureState references;
};
```

Do not hide all state in global variables.

---

# 55. Syntax Structure vs Derived Structure

Keep these separate.

Example:

```cpp
struct SpsSyntax {
    uint32_t pic_width_in_luma_samples;
    uint32_t pic_height_in_luma_samples;
    uint32_t chroma_format_idc;
    uint32_t bit_depth_luma_minus8;
};
```

Then:

```cpp
struct SpsDerived {
    uint32_t width;
    uint32_t height;
    uint32_t bitDepthLuma;
    uint32_t bitDepthChroma;

    uint32_t ctbSize;
    uint32_t widthInCtbs;
    uint32_t heightInCtbs;
};
```

This prevents confusion between:

```text
encoded syntax
```

and:

```text
calculated values
```

---

# 56. Parser API Design

A useful API:

```cpp
class HevcParser {
public:
    ParseResult parseNal(std::span<const uint8_t> nal);
    ParseResult parseAnnexB(std::span<const uint8_t> stream);
};
```

Lower-level components:

```cpp
class NalExtractor;
class BitReader;
class VpsParser;
class SpsParser;
class PpsParser;
class SliceHeaderParser;
class SeiParser;
```

Each parser should consume only the syntax it owns.

---

# 57. Parse Results

Prefer structured errors.

Example:

```cpp
enum class ParseErrorCode {
    None,
    EndOfBitstream,
    InvalidNalHeader,
    InvalidStartCode,
    InvalidExpGolomb,
    MissingParameterSet,
    InvalidSyntax,
    InvalidTrailingBits,
    UnsupportedSyntax,
    Overflow
};
```

Result:

```cpp
struct ParseError {
    ParseErrorCode code;
    size_t bitPosition;
    std::string message;
};
```

A good error should say:

```text
SPS id=0:
failed reading log2_max_pic_order_cnt_lsb_minus4
bit position=173
bits remaining=2
```

rather than:

```text
parse error
```

---

# 58. Bit Position Debugging

Every syntax parser should be able to report:

```text
NAL type
RBSP byte offset
bit offset
syntax element
value
```

Example:

```text
[SPS]
bit=42
sps_video_parameter_set_id = 0

bit=46
sps_max_sub_layers_minus1 = 0

bit=47
sps_temporal_id_nesting_flag = 1
```

This makes comparisons against a reference parser vastly easier.

---

# 59. Syntax Trace Mode

Implement an optional trace mode:

```cpp
ParseTrace trace;
```

Output:

```text
[SPS] sps_video_parameter_set_id = 0
[SPS] sps_max_sub_layers_minus1 = 0
[SPS] sps_temporal_id_nesting_flag = 1
[SPS] profile_space = 0
[SPS] tier_flag = 0
[SPS] profile_idc = 1
...
```

Prefer RAII or scoped tracing:

```cpp
TRACE_FIELD("sps_video_parameter_set_id", value);
```

Do not scatter uncontrolled `std::cout` statements throughout the parser.

---

# 60. Syntax Assertions

When parsing known constrained fields, validate the constraints.

Examples:

```cpp
if (forbidden_zero_bit != 0)
    error(...);

if (nuh_temporal_id_plus1 == 0)
    error(...);

if (chroma_format_idc > 3)
    error(...);
```

Assertions should distinguish:

```text
invalid bitstream
```

from:

```text
programmer bug
```

Do not use `assert()` as the only validation mechanism for untrusted input.

---

# 61. Bounds Checking

Every read operation must be bounded.

Never:

```cpp
buffer[pos++]
```

without ensuring:

```cpp
pos < buffer.size()
```

For bit reads:

```cpp
if (bitsRemaining() < n)
    throw ParseError(...);
```

For Exp-Golomb:

```text
leading-zero count
```

must have an upper bound.

Malformed bitstreams are expected input to a parser.

---

# 62. Integer Overflow

Be careful with:

```text
1 << n
width * height
ctbCountX * ctbCountY
offset calculations
payloadSize
```

Use sufficiently wide types:

```cpp
uint64_t
size_t
```

where appropriate.

Do not cast to `int` merely for convenience.

---

# 63. Never Trust Encoded Counts

Fields such as:

```text
num_short_term_ref_pic_sets
num_long_term_pics
num_tile_columns_minus1
num_tile_rows_minus1
cpb_cnt_minus1
```

must be validated before allocation or iteration.

A malicious bitstream may contain extremely large values.

Set application-level limits where appropriate:

```cpp
ParseLimits {
    maxNalSize;
    maxParameterSets;
    maxSeiPayload;
    maxRpsEntries;
    maxTiles;
};
```

---

# 64. Unknown Syntax

If the specification allows a reserved or unknown value:

```text
preserve it
```

rather than:

```text
abort
```

However, if an unknown value makes it impossible to determine the syntax layout, fail safely.

Distinguish:

```text
unknown but safely skippable
```

from:

```text
unknown and structurally ambiguous
```

---

# 65. Reserved Bits

When the specification says:

```text
reserved_zero_Xbits
```

validate that the bits are zero.

Do not silently ignore them.

This is useful for catching:

```text
bit offset bugs
```

because one wrong earlier read often causes reserved bits to become non-zero.

---

# 66. Byte Alignment

Some syntax requires byte alignment.

Use:

```cpp
br.byteAligned()
```

and:

```cpp
br.alignToByte()
```

only where the syntax explicitly calls for alignment.

Never add alignment "just to make parsing easier."

That is one of the fastest ways to corrupt an HEVC parser.

---

# 67. Do Not Parse by Guessing

Never use heuristics such as:

```cpp
if (remainingBytes == 10)
    assume this is VUI
```

or:

```cpp
if (value looks like 960)
    assume width
```

HEVC syntax is deterministic.

Parsing must follow:

```text
syntax order
conditional flags
loop bounds
previous values
```

exactly.

---

# 68. Common Bitstream Parser Bugs

## Bug 1 — Treating EBSP as RBSP

Symptom:

```text
SPS parses correctly for some streams
but fails for others.
```

Cause:

```text
emulation-prevention bytes were not removed.
```

Fix:

```text
NAL payload
→ EBSP
→ RBSP
→ BitReader
```

---

## Bug 2 — Removing every `0x03`

Wrong:

```cpp
if (byte == 0x03)
    skip();
```

This can corrupt legitimate payload bytes.

Use the correct emulation-prevention rule.

---

## Bug 3 — Reading Exp-Golomb as fixed width

Wrong:

```cpp
readBits(8)
```

for:

```text
ue(v)
```

Fix:

```cpp
readUE()
```

---

## Bug 4 — Off-by-one in `minus1` fields

For:

```text
num_tile_rows_minus1
```

actual count is:

```cpp
numTileRows = num_tile_rows_minus1 + 1;
```

Do not expose the encoded field as the actual count.

---

## Bug 5 — Confusing `minus8`

For:

```text
bit_depth_luma_minus8
```

actual bit depth:

```cpp
+ 8
```

---

## Bug 6 — Confusing POC LSB with POC

```text
pic_order_cnt_lsb
```

is not necessarily the final picture order count.

---

## Bug 7 — Ignoring parameter-set dependencies

A slice cannot be interpreted correctly without its PPS/SPS context.

---

## Bug 8 — Assuming one frame per NAL

A frame/picture may contain multiple slice NAL units.

---

## Bug 9 — Ignoring dependent slices

Not every slice segment has a complete independent header.

---

## Bug 10 — Assuming all fields are byte-aligned

Most HEVC syntax is bit-packed.

---

# 69. Recommended Debugging Workflow

When parsing a new sample:

### Step 1

Extract NAL units.

Print:

```text
NAL index
offset
size
NAL type
layer ID
temporal ID
```

### Step 2

Parse the NAL header.

Verify:

```text
forbidden_zero_bit == 0
temporal_id_plus1 >= 1
```

### Step 3

Convert EBSP to RBSP.

Print:

```text
EBSP size
RBSP size
```

### Step 4

Parse the parameter sets.

Start with:

```text
VPS
SPS
PPS
```

### Step 5

Print every syntax field with bit positions.

### Step 6

Compare derived values:

```text
coded width
coded height
crop
chroma
bit depth
CTB size
```

### Step 7

Only then parse slices.

### Step 8

Validate POC/reference state.

---

# 70. Golden Bitstream Tests

Maintain small test streams for:

```text
VPS only
SPS only
PPS only
single I frame
P frame
B frame
CRA
IDR
tiles
WPP
4:2:0
4:2:2
4:4:4
10-bit
12-bit
VUI
HRD
SEI
long-term references
scaling lists
dependent slices
```

Tests should verify both:

```text
syntax values
```

and:

```text
bit positions
```

---

# 71. Differential Testing

When possible, compare against an independent HEVC parser.

Useful comparisons:

```text
NAL type
VPS ID
SPS ID
PPS ID
width
height
chroma format
bit depth
profile
level
VUI
POC
slice type
reference counts
```

Do not assume the other parser is correct merely because it produces output.

Use disagreements to investigate the syntax path.

---

# 72. Fuzz Testing

HEVC parsers are excellent fuzzing targets.

Fuzz:

```text
NAL header
RBSP
Exp-Golomb values
SEI sizes
parameter-set IDs
slice headers
```

Required properties:

```text
no crash
no infinite loop
no out-of-bounds read
no unbounded allocation
no undefined behavior
```

A malformed stream should produce:

```text
ParseError
```

not:

```text
segmentation fault
```

---

# 73. Infinite Loop Prevention

Every loop over bitstream data should have a progress guarantee.

Dangerous:

```cpp
while (moreRbspData()) {
    ...
}
```

if the body can consume zero bits.

Every parser loop should either:

```text
consume bits
```

or:

```text
terminate
```

For debugging, optionally assert:

```cpp
auto before = br.bitPosition();
parseSomething();
auto after = br.bitPosition();

if (after == before)
    error("parser made no progress");
```

---

# 74. Parsing vs Decoding

A bitstream parser does not necessarily need to implement:

```text
CABAC decoding
inverse transform
motion compensation
deblocking
SAO reconstruction
pixel reconstruction
```

Keep the boundary clear.

A syntax parser may need to understand enough slice syntax to locate or describe coded data without decoding the entire picture.

Recommended architecture:

```text
Syntax Parser
     |
     +-- Parameter Sets
     +-- Slice Headers
     +-- SEI
     +-- Picture State
     |
     v
Optional Decoder
     |
     +-- CABAC
     +-- Transform
     +-- Prediction
     +-- Reconstruction
```

---

# 75. CABAC Boundary

After the slice header, the remaining coded slice data is entropy-coded using CABAC.

A syntax parser should not attempt to interpret CABAC-coded bins as ordinary bitstream syntax.

The boundary is conceptually:

```text
NAL
 ↓
RBSP
 ↓
slice_segment_header()
 ↓
slice_segment_data()
       ↓
      CABAC
```

This distinction is critical.

---

# 76. `cabac_zero_word`

When handling slice RBSP trailing data, be aware of HEVC-specific CABAC termination/padding semantics.

Do not assume that:

```text
last bytes == ordinary rbsp_trailing_bits
```

without considering the surrounding slice syntax.

A parser designed only to extract headers may stop after the slice header and treat the remaining payload as opaque CABAC data.

That is often safer than pretending to parse coded-tree syntax.

---

# 77. Recommended Data Model

A practical high-level model:

```cpp
struct NalHeader {
    uint8_t nalUnitType;
    uint8_t nuhLayerId;
    uint8_t temporalId;
};

struct NalUnit {
    NalHeader header;
    std::vector<uint8_t> ebsp;
    std::vector<uint8_t> rbsp;
};

struct HevcStream {
    std::vector<NalUnit> nals;
    std::unordered_map<int, VPS> vps;
    std::unordered_map<int, SPS> sps;
    std::unordered_map<int, PPS> pps;
};
```

Then extend with:

```cpp
struct AccessUnit;
struct Picture;
struct SliceHeader;
struct SeiMessage;
```

---

# 78. C++20 Guidelines

Use modern C++ facilities where they improve safety.

Prefer:

```cpp
std::span<const uint8_t>
```

for non-owning byte ranges.

Prefer:

```cpp
std::vector<uint8_t>
```

for owned RBSP data.

Prefer:

```cpp
std::optional<T>
```

for optional syntax.

Prefer:

```cpp
enum class
```

for NAL types and slice types.

Prefer:

```cpp
std::expected<T, ParseError>
```

when available in the project's C++20 compatibility layer, or an equivalent project-defined result type.

Avoid:

```cpp
uint8_t*
```

plus manually managed lengths unless necessary.

---

# 79. No Hidden Global Bit State

Bad:

```cpp
static size_t bitPos;
```

Good:

```cpp
class BitReader {
    std::span<const uint8_t> data_;
    size_t bitPos_ = 0;
};
```

Each parser receives an explicit reader or parsing context.

This makes nested parsing and unit tests much easier.

---

# 80. Parser Reentrancy

The parser should ideally be reentrant.

Avoid:

```cpp
global currentSps
global currentPps
global bitPosition
```

Instead:

```cpp
ParseContext context;
```

containing the necessary state.

This allows:

```text
multiple streams
multiple parser instances
parallel parsing
unit testing
```

without state corruption.

---

# 81. Logging Levels

Recommended levels:

```text
ERROR
WARN
INFO
DEBUG
TRACE
```

Example:

```text
INFO:
NAL type = 33 (SPS)

DEBUG:
SPS ID = 0
dimensions = 960 x 400

TRACE:
bit=17 pic_width_in_luma_samples = 960
```

Avoid always printing every syntax element in production.

---

# 82. Human-Friendly Output

A useful summary:

```text
HEVC Stream
-----------
VPS:
  id: 0
  max layers: 1
  max sub-layers: 1

SPS:
  id: 0
  VPS id: 0
  dimensions: 960 x 400
  chroma: 4:2:0
  luma bit depth: 8
  chroma bit depth: 8
  temporal MVP: enabled

PPS:
  id: 0
  SPS id: 0
  tiles: disabled
  WPP: enabled
```

The human-readable summary should be generated from parsed structures, not directly during low-level bit parsing.

---

# 83. Raw + Derived Output

For debugging, expose both:

```text
bit_depth_luma_minus8 = 2
bit depth luma = 10
```

and:

```text
log2_min_luma_coding_block_size_minus3 = 0
minimum CB size = 8
```

This makes it immediately obvious whether an error is in:

```text
bit parsing
```

or:

```text
derived-value calculation
```

---

# 84. Specification-Driven Implementation

When implementing a syntax element, document:

```text
HEVC syntax name
syntax type
condition
loop dependency
derived value
```

Example:

```cpp
// HEVC:
// pic_width_in_luma_samples u(v)
//
// Parsed directly from SPS.
// Actual display width may additionally depend on
// conformance_window_flag and chroma subsampling.
```

This is far safer than comments such as:

```cpp
// width
```

---

# 85. Preserve Specification Names

Prefer variable names close to the specification:

```cpp
uint32_t sps_max_sub_layers_minus1;
uint32_t pic_width_in_luma_samples;
bool conformance_window_flag;
```

Then derive application-friendly names:

```cpp
unsigned maxSubLayers;
unsigned codedWidth;
```

This makes cross-checking against the standard much easier.

---

# 86. Do Not Over-Normalize Too Early

Do not immediately discard the original syntax.

For example, keep:

```cpp
bit_depth_luma_minus8
```

even if you also store:

```cpp
bitDepthLuma
```

The raw syntax is invaluable during debugging.

---

# 87. Parsing Conditional Syntax

Use explicit structure:

```cpp
if (vui_parameters_present_flag) {
    vui = parseVui(br);
}
```

Avoid compact code that hides syntax dependencies:

```cpp
vui = parseVuiIfPossible(br);
```

HEVC parsing is specification-driven; visibility of conditions is valuable.

---

# 88. Parsing Loops

Write loops in the same conceptual form as the specification.

Example:

```cpp
for (unsigned i = 0; i <= vps_max_sub_layers_minus1; ++i) {
    ...
}
```

or, where the syntax has different conditions:

```cpp
unsigned start =
    vps_sub_layer_ordering_info_present_flag
        ? 0
        : vps_max_sub_layers_minus1;

for (unsigned i = start;
     i <= vps_max_sub_layers_minus1;
     ++i) {
    ...
}
```

Do not "simplify" loops until their correctness is established.

---

# 89. Common Off-by-One Patterns

Watch especially for:

```text
minus1
minus2
minus3
minus4
minus8
```

Examples:

```text
num_ref_idx_l0_active_minus1 + 1
log2_max_pic_order_cnt_lsb_minus4 + 4
bit_depth_luma_minus8 + 8
log2_parallel_merge_level_minus2 + 2
```

A useful convention is:

```cpp
// Encoded
uint32_t num_ref_idx_l0_active_minus1;

// Derived
uint32_t numRefIdxL0Active =
    num_ref_idx_l0_active_minus1 + 1;
```

---

# 90. Validation Invariants

Useful invariants include:

```text
forbidden_zero_bit == 0

nuh_temporal_id_plus1 >= 1

chroma_format_idc <= 3

PPS references existing SPS

SPS references existing VPS

slice PPS exists

slice SPS exists

coded width > 0

coded height > 0

bit depth >= 8

max_sub_layers <= 7

array indices remain within syntax-derived bounds
```

Validation should happen as close as practical to the point where the value becomes known.

---

# 91. Error Recovery

For stream inspection tools, consider allowing:

```text
continue after malformed NAL
```

while decoder-oriented applications may choose:

```text
fail current access unit
```

Never continue with corrupted parser state silently.

Use a clear policy:

```cpp
enum class ErrorPolicy {
    Strict,
    RecoverNal,
    RecoverAccessUnit
};
```

---

# 92. Start Code Edge Cases

Test:

```text
00 00 01
00 00 00 01
00 00 00 00 01
```

and streams with:

```text
leading_zero_8bits
trailing_zero_8bits
```

Do not assume start codes are always isolated by arbitrary non-zero bytes.

---

# 93. Empty and Truncated NALs

Handle:

```text
start code + no NAL
```

and:

```text
one-byte NAL
```

and:

```text
truncated NAL header
```

explicitly.

Never read:

```cpp
payload[1]
```

unless the NAL contains at least two header bytes.

---

# 94. Parameter Set Ordering

Real streams may contain:

```text
VPS
SPS
PPS
SEI
slice
```

but streams may also repeat parameter sets.

Do not require a globally fixed order unless the application specifically imposes one.

The parser should process NALs sequentially and maintain state.

---

# 95. Parser Output Contract

A parser should clearly state whether it guarantees:

### Level 1 — Structural parsing

```text
NAL extraction
NAL headers
RBSP
basic parameter sets
```

### Level 2 — Syntax parsing

```text
VPS/SPS/PPS
VUI
SEI
slice headers
RPS
POC
```

### Level 3 — Picture state

```text
access units
reference pictures
POC derivation
IRAP
```

### Level 4 — Full decoding

```text
CABAC
prediction
transform
reconstruction
```

Do not claim Level 4 capabilities when implementing Level 2.

---

# 96. Recommended Implementation Order

Build the parser in this order:

```text
1. BitReader
2. Exp-Golomb
3. RBSP trailing bits
4. EBSP → RBSP
5. NAL header
6. Annex B extraction
7. VPS
8. profile_tier_level
9. SPS
10. PPS
11. VUI
12. SEI
13. slice header
14. POC
15. short-term RPS
16. long-term references
17. reference lists
18. access-unit detection
19. picture state
20. optional CABAC/decoder
```

Do not begin with slice decoding before the foundational bitstream layer is proven.

---

# 97. Unit-Test the BitReader First

Before testing SPS:

```text
test readBit()
test readBits()
test peekBits()
test skipBits()
test readUE()
test readSE()
test byte alignment
test end-of-buffer
test overflow
```

Example Exp-Golomb vectors:

```text
1       -> 0
010     -> 1
011     -> 2
00100   -> 3
00101   -> 4
```

Then signed mappings:

```text
0 -> 0
1 -> +1
2 -> -1
3 -> +2
4 -> -2
```

If these tests are wrong, every higher-level parser result is suspect.

---

# 98. Golden SPS Example

For every real stream, record:

```text
NAL type
SPS ID
VPS ID
max sub-layers
profile
tier
level
chroma format
width
height
bit depth
POC LSB width
CTB size
SAO
AMP
WPP
tiles
VUI
```

A parser regression test should compare the complete expected structure.

---

# 99. Debugging a Wrong Width/Height

If the parser reports:

```text
960 x 400
```

but another tool reports something different, check in this exact order:

```text
1. NAL header
2. EBSP → RBSP
3. SPS bit position
4. sps_video_parameter_set_id
5. sps_max_sub_layers_minus1
6. profile_tier_level()
7. sps_seq_parameter_set_id
8. chroma_format_idc
9. separate_colour_plane_flag
10. pic_width_in_luma_samples
11. pic_height_in_luma_samples
12. conformance_window_flag
13. crop offsets
14. chroma subsampling
15. VUI default display window
```

Do not immediately change the width formula.

Usually the problem is an earlier bit-position error.

---

# 100. Debugging Strategy: Find the First Divergence

When comparing your parser with a known-good parser:

Do not compare only final output.

Compare:

```text
field 1
field 2
field 3
...
```

Find the **first field that differs**.

If:

```text
profile_idc
```

is already wrong, there is no point debugging:

```text
pic_width_in_luma_samples
```

The parser almost certainly lost bit alignment earlier.

---

# 101. Bit Trace Example

Ideal diagnostic output:

```text
[SPS] bit=0   sps_video_parameter_set_id = 0
[SPS] bit=4   sps_max_sub_layers_minus1 = 0
[SPS] bit=7   sps_temporal_id_nesting_flag = 1

[PTL] bit=8   general_profile_space = 0
[PTL] bit=9   general_tier_flag = 0
[PTL] bit=10  general_profile_idc = 1

[SPS] bit=...
      sps_seq_parameter_set_id = 0

[SPS] bit=...
      chroma_format_idc = 1

[SPS] bit=...
      pic_width_in_luma_samples = 960

[SPS] bit=...
      pic_height_in_luma_samples = 400
```

This style makes parser bugs obvious.

---

# 102. Never Mix Parsing and Printing

Bad:

```cpp
std::cout << "width=" << br.readUE();
```

Better:

```cpp
sps.pic_width_in_luma_samples = br.readUE();
```

Then:

```cpp
printSps(sps);
```

This gives tests access to the actual structure and keeps the parser deterministic.

---

# 103. Never Mutate Input Data

The original NAL byte sequence should remain intact.

Do:

```cpp
auto rbsp = removeEmulationPrevention(ebsp);
```

rather than modifying the original NAL buffer.

This makes:

```text
hex dump
RBSP dump
reproduction
```

much easier.

---

# 104. Hex Dump Utility

Provide a diagnostic utility:

```cpp
dumpHex(nal);
dumpHex(rbsp);
```

Include offsets:

```text
0000: 42 01 01 60 00 00 03 00
0008: 00 03 00 00 03 00 99 ...
```

This is extremely useful when investigating emulation-prevention problems.

---

# 105. Parser Security

Treat every HEVC stream as untrusted.

Protect against:

```text
integer overflow
stack exhaustion
heap exhaustion
huge SEI payloads
huge RPS counts
malformed Exp-Golomb
infinite loops
truncated NALs
invalid IDs
invalid references
```

Do not allocate based directly on an unvalidated encoded count.

---

# 106. Performance

For normal video streams, bit parsing is not usually the dominant cost compared with decoding.

Prioritize:

```text
correctness
clarity
safe bounds checking
```

before micro-optimization.

If performance matters:

```text
bit cache
branch reduction
span-based parsing
zero-copy NAL extraction
arena allocation
```

can be considered later.

Never sacrifice bitstream correctness for a small parsing optimization.

---

# 107. Recommended File Structure

A clean C++ project can look like:

```text
hevc/
├── bit_reader.hpp
├── bit_reader.cpp
│
├── rbsp.hpp
├── rbsp.cpp
│
├── nal.hpp
├── nal.cpp
├── nal_extractor.hpp
├── nal_extractor.cpp
│
├── hevc_vps.hpp
├── vps.cpp
├── hevc_sps.hpp
├── sps.cpp
├── hevc_pps.hpp
├── pps.cpp
│
├── hevc_profile_tier_level.hpp
├── profile_tier_level.cpp
│
├── hevc_vui.hpp
├── vui.cpp
├── hevc_hrd.hpp
├── hrd.cpp
│
├── hevc_sei.hpp
├── sei.cpp
│
├── hevc_slice_header.hpp
├── slice_header.cpp
├── rps.hpp
├── rps.cpp
│
├── picture_state.hpp
├── picture_state.cpp
│
├── hevc_parser.hpp
├── hevc_parser.cpp
│
└── tests/
    ├── bit_reader_tests.cpp
    ├── rbsp_tests.cpp
    ├── nal_tests.cpp
    ├── sps_tests.cpp
    ├── pps_tests.cpp
    ├── sei_tests.cpp
    └── slice_tests.cpp
```

---

# 108. Recommended Development Rules

When modifying the parser:

1. Do not change multiple parsing layers at once unless necessary.
2. Preserve existing bit-position diagnostics.
3. Add a regression test for every discovered parsing bug.
4. Keep specification syntax names visible in code.
5. Never "fix" a wrong value by adding a compensating offset without identifying the root cause.
6. Verify the first divergent field.
7. Verify RBSP conversion before debugging syntax.
8. Verify Exp-Golomb before debugging complex structures.
9. Verify parameter-set dependencies before parsing slices.
10. Keep raw syntax separate from derived state.

---

# 109. HEVC Parser Golden Rule

When a value looks wrong:

```text
DO NOT immediately change the formula.
```

First ask:

```text
Did I extract the correct NAL?
Did I parse the NAL header correctly?
Did I remove emulation-prevention bytes correctly?
Am I at the correct RBSP bit position?
Did I consume the previous conditional field?
Did I calculate the loop bound correctly?
Did I parse ue(v) / se(v) correctly?
Did I accidentally align to a byte?
Did I confuse a minus-N field with its derived value?
```

Most HEVC parser bugs are **bit-position bugs disguised as value-calculation bugs**.

---

# 110. Practical Parsing Checklist

Before declaring an HEVC parser correct:

- [ ] Annex B 3-byte start code works.
- [ ] Annex B 4-byte start code works.
- [ ] Length-prefixed NAL extraction is separated from syntax parsing.
- [ ] NAL header is parsed MSB-first.
- [ ] `forbidden_zero_bit` is validated.
- [ ] `nuh_temporal_id_plus1` is validated.
- [ ] EBSP → RBSP conversion is correct.
- [ ] Emulation-prevention bytes are removed correctly.
- [ ] BitReader is bounds checked.
- [ ] `u(n)` works.
- [ ] `ue(v)` works.
- [ ] `se(v)` works.
- [ ] `more_rbsp_data()` is correct.
- [ ] `rbsp_trailing_bits()` is validated.
- [ ] VPS parsing works.
- [ ] Profile/Tier/Level parsing works.
- [ ] SPS parsing works.
- [ ] PPS parsing works.
- [ ] Chroma formats are handled.
- [ ] Conformance window is handled.
- [ ] Bit depth is derived correctly.
- [ ] CTB geometry is derived correctly.
- [ ] VUI parsing works.
- [ ] SEI parsing preserves unknown payloads.
- [ ] Slice headers resolve PPS/SPS dependencies.
- [ ] Slice type is interpreted correctly.
- [ ] POC LSB is distinguished from full POC.
- [ ] Short-term RPS parsing works.
- [ ] Long-term reference syntax is handled.
- [ ] Reference-list modification is handled.
- [ ] Tiles are parsed.
- [ ] WPP is distinguished from tiles.
- [ ] IRAP types are preserved.
- [ ] Access units are distinguished from NAL units.
- [ ] Malformed streams cannot crash the parser.
- [ ] Fuzzing does not produce hangs.
- [ ] Golden bitstreams pass.
- [ ] Bit-position traces are available.
- [ ] Raw syntax and derived values are both available.

---

# 111. Final Engineering Principle

The parser should behave like a direct executable interpretation of the HEVC syntax.

The preferred flow is:

```text
HEVC specification
        ↓
syntax element
        ↓
BitReader operation
        ↓
stored raw value
        ↓
derived value
        ↓
validated semantic structure
```

For example:

```text
bitstream
  ↓
pic_width_in_luma_samples
  ↓
960
  ↓
codedWidth = 960
  ↓
conformance window
  ↓
displayWidth
```

Do not jump directly from:

```text
bytes
```

to:

```text
"width = 960"
```

without preserving the intermediate syntax and derivation.

The strongest HEVC parser is not the one that merely produces correct values on a known sample.

It is the one where, for **every value**, you can answer:

```text
Which NAL contained it?
Which RBSP bit contained it?
Which HEVC syntax element produced it?
What condition caused it to exist?
What previous syntax determined its width?
What derivation produced the final value?
What validation proves it is legal?
```

That is the standard to use when implementing, reviewing, or debugging an H.265/HEVC bitstream parser.