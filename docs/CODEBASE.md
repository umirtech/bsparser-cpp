# bsparser C++ Architecture Reference

## One-screen summary

`bsparser` turns raw elementary-stream bytes into `Unit` boundaries, then
turns individual units into lightweight `Header` metadata.

```text
input chunks → UnitScanner → Unit(s) → parse_unit() → Header(s)
```

Use `IvfParser` only when the input is an IVF container. Use `StreamParser`
when only parsed headers are wanted. For a video editor, prefer
`UnitScanner`: it exposes offsets, bytes, keyframe hints, and frame starts.

## Public API decision table

| Need | API | Notes |
| --- | --- | --- |
| Split streaming chunks into units | `UnitScanner::feed/finish` | Stateful; preferred realtime API |
| Parse one complete known unit | `parse_unit(codec, bytes, offset)` | AVC/HEVC/VVC exclude Annex-B prefix |
| Parse a scanner result | `parse_unit(codec, Unit)` | Retains scanner offset |
| Scan a contiguous Annex-B clip | `find_annexb_start_codes(data, size)` | Zero-copy offsets only |
| IVF chunks and timestamps | `IvfParser::feed` | Codec comes from FourCC |
| Headers only, legacy wrapper | `StreamParser` | Less control than `UnitScanner` |

### Core types

```cpp
enum class Codec { VP8, VP9, AV1, AVC, HEVC, VVC, Unknown };
enum class UnitKind { Frame, Obu, NalUnit };

struct Unit {
  UnitKind kind;
  uint64_t offset;       // source byte position of bytes[0]
  size_t start_code_size; // 0 for AV1/VP; always 3 for legacy Annex-B
  uint8_t type;          // NAL or OBU type
  bool frame_start;
  bool keyframe;
  std::vector<uint8_t> bytes;
};
```

`Header.fields` is intentionally string-valued to make CLI JSON emission
simple and stable. Do not change field names casually; downstream code may
index them.

## Boundary semantics

| Codec | Unit | `frame_start` | `keyframe` |
| --- | --- | --- | --- |
| AVC | Annex-B NAL | Never | IDR NAL (type 5) |
| HEVC | Annex-B NAL | VCL with `first_slice_segment_in_pic_flag` | NAL types 16–21 |
| VVC | Annex-B NAL | Picture header NAL (type 19) | NAL types 7–10 |
| AV1 | OBU with size field | Never | Parsed frame type when present |
| VP8 | Caller-provided frame | Never | Frame tag |
| VP9 | Caller-provided frame | Never | Parsed frame type |

HEVC uses the JavaScript reference rule of treating only NAL types 19 and 20
as keyframes; VVC never reports a keyframe.

These follow the bundled JavaScript parser and intentionally do not expose
access-unit/frame-start detection.

## Scanner internals and invariants

`UnitScanner` owns `pending_`. `pending_begin_` advances through consumed
bytes; `pending_offset_` is the absolute offset corresponding to that cursor.
`compact_pending()` moves data only after substantial consumption (64 KiB and
at least half the vector), avoiding quadratic behavior on NAL-heavy streams.

When editing this code:

- Advance `pending_offset_` by exactly the bytes advanced in `pending_begin_`.
- Preserve up to 3 trailing bytes when searching a chunk without a start code;
  `00 00 01` can cross chunk boundaries. Treat a 4-byte prefix as a 3-byte
  marker beginning at its second zero, matching the JavaScript parser.
- AV1 may have an extension byte and a variable-length LEB128 size. Do not
  emit until both header and payload are complete.
- `finish()` must flush only a complete final Annex-B NAL and must reject a
  truncated AV1 OBU.


## Build and verification

### CMake

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

### Android NDK direct compile

Replace `NDK_CLANG` with the NDK `clang++.exe` path.

```powershell
# x86_64
& $NDK_CLANG --target=x86_64-linux-android28 -std=c++17 -Wall -Wextra -Wpedantic `
  -I cpp\include cpp\bsparser.cpp cpp\tests.cpp -o bsparser_tests
```
