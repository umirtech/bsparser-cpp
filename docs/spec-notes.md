# Specification references

This project is implemented using the following specifications.  These notes
condense the parts relevant to picture order / presentation order into a
code-mapping reference; the full standards are the authoritative sources.

## Sources

| codec | specification |
|-------|---------------|
| AV1   | [AV1 Bitstream & Decoding Process Specification](https://aomediacodec.github.io/av1-spec/) (© Alliance for Open Media, BSD-3-Clause) |
| AVC   | [ITU-T H.264](https://www.itu.int/rec/T-REC-H.264) (© ITU/ISO, all rights reserved) |
| HEVC  | [ITU-T H.265](https://www.itu.int/rec/T-REC-H.265) (© ITU/ISO, all rights reserved) |
| VVC   | [ITU-T H.266](https://www.itu.int/rec/T-REC-H.266) (© ITU/ISO, all rights reserved) |
| VP8   | [RFC 6386 — VP8 Data Format and Decoding Guide](https://www.rfc-editor.org/rfc/rfc6386) (© IETF Trust) |
| VP9   | [VP9 Bitstream & Decoding Process Specification](https://storage.googleapis.com/downloads.webmproject.org/docs/vp9/vp9-bitstream-specification-v0.7.pdf) (© 2017 Google, all rights reserved) |

The standards texts are copyrighted by their respective owners and are **not**
redistributed in this repository; consult the licenses at the links above.

---

## VVC / H.266 — Picture Order Count

### NAL unit header — §7.3.1.1

    16 bits: forbidden_zero_bit(1) nuh_reserved_zero_bit(1)
             nuh_layer_id(6) nal_unit_type(5) nuh_temporal_id_plus1(3)

`nal_unit_type` VCL values used by POC logic (Table 7-1):

| type | name      | POC role                  |
|------|-----------|---------------------------|
| 0    | TRAIL     | normal                    |
| 2    | RADL      | excluded from prevTid0Pic |
| 3    | RASL      | excluded from prevTid0Pic |
| 7    | IDR_W_RADL| IRAP / CLVSS              |
| 8    | IDR_N_LP  | IRAP / CLVSS              |
| 9    | CRA       | IRAP / CLVSS              |
| 10   | GDR       | CLVSS iff recovery_poc_cnt == 0 |

### SPS POC configuration — §7.3.2.4

    sps_log2_max_pic_order_cnt_lsb_minus4     u(4)   MaxPicOrderCntLsb = 2^(x+4)
    sps_poc_msb_cycle_flag                    u(1)
    if (flag) sps_poc_msb_cycle_len_minus1    ue(v)

`NumExtraPhBits` = count of `sps_extra_ph_bit_present_flag[i] == 1` over
`sps_num_extra_ph_bytes * 8` bits (§7.3.2.4 / (41)); shall be 0 in conforming
streams.

### Picture header — §7.3.2.8 (leading fields)

    ph_gdr_or_irap_pic_flag   u(1)
    ph_non_ref_pic_flag       u(1)
    if (gdr_or_irap) ph_gdr_pic_flag          u(1)
    ph_inter_slice_allowed_flag               u(1)
    if (inter) ph_intra_slice_allowed_flag    u(1)
    ph_pic_parameter_set_id                   ue(v)
    ph_pic_order_cnt_lsb                      u(sps_log2_max_pic_order_cnt_lsb_minus4 + 4)
    if (gdr_pic_flag) ph_recovery_poc_cnt     ue(v)
    for (i < NumExtraPhBits) ph_extra_bit[i]  u(1)
    if (sps_poc_msb_cycle_flag) {
        ph_poc_msb_cycle_present_flag         u(1)
        if (present) ph_poc_msb_cycle_val     u(sps_poc_msb_cycle_len_minus1 + 1)
    }

The PH may be a standalone NAL (`PH_NUT` = 19) or embedded in each slice
header via `sh_picture_header_in_slice_header_flag` (§7.3.4.1).

### Slice header — §7.3.4.1 (leading fields)

    sh_picture_header_in_slice_header_flag  u(1)
    if (flag) picture_header_structure()       <- embedded PH
    if (sps_subpic_info_present_flag) sh_subpic_id  u(sps_subpic_id_len_minus1 + 1)
    [sh_slice_address ...]                     <- omitted for single-tile
    for (i < NumExtraShBits) sh_extra_bit[i]   u(1)
    if (ph_inter_slice_allowed_flag) sh_slice_type  ue(v)

### POC derivation — §8.3.1

    PicOrderCntVal = PicOrderCntMsb + ph_pic_order_cnt_lsb

    prevTid0Pic = previous pic (decode order, same layer) with
                  TemporalId == 0, ph_non_ref_pic_flag == 0, not RASL/RADL

    PicOrderCntMsb:
      - if ph_poc_msb_cycle_val present:  msb_cycle_val * MaxPicOrderCntLsb
      - else if CLVSS picture:            0
      - else (wrap, when prevTid0Pic exists):
          if      poc_lsb <  prevLsb && prevLsb - poc_lsb >= max/2  -> prevMsb + max
          else if poc_lsb >  prevLsb && poc_lsb - prevLsb  >  max/2  -> prevMsb - max
          else                                                       -> prevMsb

    CLVSS = IRAP NAL (7..11) OR (GDR (10) AND ph_recovery_poc_cnt == 0)

Implementation: `parser/vvc_sps_parser.hpp`, `parser/vvc_ph_parser.hpp`,
`parser/vvc_slice_parser.hpp`, `parser/vvc_poc.hpp`, wired in
`dispatch_state_vvc` (bsparser.hpp).  Exposed as `vvc::SliceHeader::derived_poc`
and `vvc::PictureHeader::derived_poc`.

---

## AV1 — presentation order via `order_hint`

AV1 has no POC; presentation order is the `order_hint` field of the frame
header.  It is `f(OrderHintBits)` bits; `OrderHintBits` comes from the
sequence header.  Frames are presented in `order_hint` order (compared
wrap-aware via `get_relative_dist`, §7.8).

### Sequence header — §5.5 (fields needed to reach order_hint_bits_minus_1)

    seq_profile f(3) still_picture f(1) reduced_still_picture_header f(1)
    if (reduced) { seq_level_idx f(5) [+seq_tier f(1)]; frame dims; return }
    timing_info_present_flag f(1)
      if: timing_info() = num_units_in_display_tick f(32) time_scale f(32)
          equal_picture_interval f(1) [num_ticks_per_picture_minus_1 uvlc()]
          decoder_model_info_present_flag f(1)
          if: decoder_model_info() = buffer_delay_length_minus_1 f(5)
              num_units_in_decoding_tick f(32)
              buffer_removal_time_length_minus_1 f(5)
              frame_presentation_time_length_minus_1 f(5)
    initial_display_delay_present_flag f(1)
    operating_points_cnt_minus_1 f(5)
    for each op: operating_point_idc f(12) seq_level_idx f(5) [+seq_tier f(1)]
                 [+decoder_model_present_for_this_op f(1)
                   + operating_parameters_info: decoder/encoder_buffer_delay
                     f(buffer_delay_length_minus_1+1) low_delay_mode_flag f(1)]
                 [+initial_display_delay_minus_1 f(4)]
    frame_width_bits_minus_1 f(4) frame_height_bits_minus_1 f(4)
    max_frame_width_minus_1 / max_frame_height_minus_1  f(n)
    frame_id_numbers_present_flag f(1)
      if: delta_frame_id_length_minus_2 f(4) additional_frame_id_length_minus_1 f(3)
    use_128x128_superblock f(1) enable_filter_intra f(1) enable_intra_edge_filter f(1)
    enable_interintra_compound f(1) enable_masked_compound f(1)
    enable_warped_motion f(1) enable_dual_filter f(1)
    enable_order_hint f(1)
      if: enable_jnt_comp f(1) enable_ref_frame_mvs f(1)
    seq_choose_screen_content_tools f(1)
      if: seq_force_screen_content_tools = SELECT(0)
      else: seq_force_screen_content_tools f(1)          -> 1 forced on / 2 forced off
    if (seq_force_screen_content_tools > 0):
      seq_choose_integer_mv f(1)
        if: seq_force_integer_mv = SELECT(0)
        else: seq_force_integer_mv f(1)
    else: seq_force_integer_mv = SELECT(0)
    if (enable_order_hint): order_hint_bits_minus_1 f(3)  -> OrderHintBits = x+1
    else: OrderHintBits = 0

### Frame header — §5.9.2 (up to order_hint)

    if (reduced_still_picture_header) { KEY, show=1, order_hint=0; return }
    show_existing_frame f(1)
      if (1): frame_to_show_map_idx f(3)
              [+temporal_point_info if dec_model && !equal_picture_interval]
              [+display_frame_id f(idLen) if frame_id_numbers]  ; return (no order_hint)
    frame_type f(2)   ; KEY=0 INTER=1 INTRA_ONLY=2 SWITCH=3
    show_frame f(1)
      if (show_frame && dec_model && !equal_picture_interval): temporal_point_info()
      showable_frame: (show ? frame_type != KEY : f(1))
    error_resilient_mode: SWITCH || (KEY && show) ? 1 : f(1)
    disable_cdf_update f(1)
    allow_screen_content_tools: (seq_force == SELECT ? f(1) : seq_force == 1)
    if (allow_screen_content_tools):
      force_integer_mv: (seq_force_integer == SELECT ? f(1) : seq_force_integer == 1)
    if (FrameIsIntra) force_integer_mv = 1
    if (frame_id_numbers_present_flag): current_frame_id f(idLen)
    frame_size_override_flag: SWITCH ? 1 : (reduced ? 0 : f(1))
    order_hint f(OrderHintBits)

    idLen = additional_frame_id_length_minus_1 + delta_frame_id_length_minus_2 + 3
    temporal_point_info() = frame_presentation_time f(frame_presentation_time_length_minus_1 + 1)

Implementation: `parser/av1_sequence_header_parser.hpp`,
`parser/av1_frame_header_parser.hpp` (context-aware), wired in
`dispatch_state_av1` (bsparser.hpp).  Exposed as `av1::FrameHeader::order_hint`
and `av1::FrameHeader::presentation_order` (decode index).

---

## H.265 / H.264

Already native before this work; see `parser/hevc_poc.hpp` (§8.3.1) and
`parser/avc_poc.hpp` (§8.2.1).  Exposed as `derived_poc` on the slice-header
structs and the C API (`BsHevcSliceSegmentHeader` / `BsAvcSliceHeader`).

## VP9 / VP8

No POC exists in these specs.  Display order of a raw stream is the decode
order, exposed as `presentation_order` on `vp9::FrameHeader` / `vp8::FrameHeader`.
