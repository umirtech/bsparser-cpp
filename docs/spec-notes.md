# Specification references

This project is implemented using the following specifications.
## Sources

| codec | specification |
|-------|---------------|
| AV1   | [AV1 Bitstream & Decoding Process Specification](https://aomediacodec.github.io/av1-spec/) |
| AVC   | [ITU-T H.264](https://www.itu.int/rec/T-REC-H.264) |
| HEVC  | [ITU-T H.265](https://www.itu.int/rec/T-REC-H.265) |
| VVC   | [ITU-T H.266](https://www.itu.int/rec/T-REC-H.266) |
| VP8   | [RFC 6386 — VP8 Data Format and Decoding Guide](https://www.rfc-editor.org/rfc/rfc6386) |
| VP9   | [VP9 Bitstream & Decoding Process Specification](https://storage.googleapis.com/downloads.webmproject.org/docs/vp9/vp9-bitstream-specification-v0.7.pdf) |

## AVC high-chroma residual traversal

The motion-vector extractor traverses residual syntax only to reach the next
macroblock.  The 4:2:2 and 4:4:4 paths follow:

- ITU-T H.264 7.3.5 (residual syntax, `ctxBlockCat` 0..13),
- 6.4.10.4 (MBAFF neighbour block derivation; a frame macroblock adjacent to
  a field-coded pair selects the top/bottom field macroblock per 4x4 block
  row, and a field macroblock samples every second frame line),
- 9.2.1 (`nC` derivation for CAVLC),
- 9.3.3.1.1.6 (4:2:2 chroma-DC significance),
- 9.3.3.1.1.9 (`coded_block_flag` contexts, including the rule that an 8x8
  block's neighbouring macroblock only contributes when it also used the 8x8
  transform),
- Tables 9-5 and 9-12..9-45 (CABAC `ctxIdxOffset` and initialisation).

For 4:4:4 the three planes are coded luma-like; the 4-bit
`coded_block_pattern` is shared by all planes and each 8x8 block codes its own
`coded_block_flag`.  For 4:2:2 the chroma DC block has eight coefficients and
the chroma AC blocks are arranged as two 8x8 sections per component.

## AVC I_PCM, monochrome and separate colour planes

- 7.3.5 (`macroblock_layer`, `pcm_alignment_zero_bit`, `pcm_sample_luma`,
  `pcm_sample_chroma`): the PCM sample count is 256 luma samples plus
  `2 * MbWidthC * MbHeightC` chroma samples at their respective bit depths;
  ChromaArrayType 0 (monochrome or separate colour planes) has no chroma
  samples.
- 9.3.1.2: after an I_PCM macroblock the arithmetic decoding engine is
  re-initialised with a new 9-bit offset and `range = 510`; the CABAC context
  model is not re-initialised.
- 9.3.3.1.1.10: an I_PCM macroblock has no `mb_qp_delta`, so the next
  macroblock's delta-QP context starts from zero.
- 9.2.1: all 4x4 blocks of an I_PCM macroblock contribute TotalCoeff 16 to
  neighbour derivations; its CodedBlockPatternLuma is 15 and
  CodedBlockPatternChroma is 2 (ChromaArrayType 1/2).
- 7.4.2.1.1 (ChromaArrayType), 7.3.5.2 (monochrome 16-entry CBP mapping) and
  7.3.3.1 (`colour_plane_id` position: after `pic_parameter_set_id`, before
  `frame_num`).
- 8.2.4 / 7.4.1.2: with `separate_colour_plane_flag = 1` each colour plane is
  coded as an independent picture; the extractor keeps one independent MV-DPB
  and grid per plane and delivers all three with the same POC/frame_num.
- 8.2.2 (map types 0-6), 8.2.2.8 (map-unit to macroblock conversion) and
  8.2.3 (`NextMbAddress`): slice-group maps are rebuilt per slice because
  types 3/4/5 consume `slice_group_change_cycle`; macroblock iteration,
  CAVLC `mb_skip_run` and CABAC `end_of_slice_flag` follow the resulting
  non-contiguous addresses.  The delivered buffer records the exact
  `CurrMbAddr -> slice` mapping as `MotionSliceRun` runs (version 3), so a
  per-cell reference lookup never falls back to the first slice for a
  non-contiguous group.
- 7.3.4 (`first_mb_in_slice`, `CurrMbAddr`): the motion slice map and the
  public cell-to-macroblock helper use raster addresses for frame/field
  pictures and `2 * pair_index + parity` for MBAFF frame pictures.
- 6.4.7 (macroblock availability): a neighbour is unavailable across a slice
  boundary; because every macroblock of a slice belongs to one slice group,
  the extractor records and checks the slice-group identity as well.
- 7.3.2 / 7.3.4 (data partitioning, NAL unit types 2-4): partitions A/B/C
  carry complementary parts of one slice and cannot be reconstructed
  independently.  The extractor reports `MotionVectorStatus::Unsupported`
  instead of emitting a grid with missing macroblocks.

AVC motion-vector extraction is **experimental**: the opt-in sink API, the
versioned buffer layout and the generated fixtures may change in future
releases.  Treat extracted grids as best-effort and re-validate against your
own reference when upgrading the module.
