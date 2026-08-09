# bsparser C++ Architecture Reference

## One-screen summary

`bsparser` turns raw elementary-stream bytes into `Unit` boundaries, then
turns individual units into lightweight `Header` metadata.

```text
input chunks → UnitScanner → Unit(s) → parse_unit() → Header(s)
                    │
                    └─ SIMD Annex-B start-code search
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
  size_t start_code_size; // 0 for AV1/VP; 3 or 4 for Annex-B
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
| AVC | Annex-B NAL | VCL with `first_mb_in_slice == 0` | IDR NAL (type 5) |
| HEVC | Annex-B NAL | VCL with `first_slice_segment_in_pic_flag` | NAL types 16–21 |
| VVC | Annex-B NAL | Picture header NAL (type 19) | NAL types 7–10 |
| AV1 | OBU with size field | Frame/header OBU (3/6) | Parsed frame type when present |
| VP8 | Caller-provided frame | Always | Frame tag |
| VP9 | Caller-provided frame | Always | Parsed frame type |

These are indexing hints, not a full decoder-level access-unit implementation.
Do not claim exact display-order or decode-order semantics without adding the
required codec state.

## Scanner internals and invariants

`UnitScanner` owns `pending_`. `pending_begin_` advances through consumed
bytes; `pending_offset_` is the absolute offset corresponding to that cursor.
`compact_pending()` moves data only after substantial consumption (64 KiB and
at least half the vector), avoiding quadratic behavior on NAL-heavy streams.

When editing this code:

- Advance `pending_offset_` by exactly the bytes advanced in `pending_begin_`.
- Preserve up to 3 trailing bytes when searching a chunk without a start code;
  `00 00 01` can cross chunk boundaries.
- AV1 may have an extension byte and a variable-length LEB128 size. Do not
  emit until both header and payload are complete.
- `finish()` must flush only a complete final Annex-B NAL and must reject a
  truncated AV1 OBU.

## Performance model

The hot path is `find_start_code_fast()` in `cpp/bsparser.cpp`, not syntax
field extraction. It selects once per process:

| ABI/CPU | Backend |
| --- | --- |
| `arm64-v8a` | NEON |
| `armeabi-v7a` with `HWCAP_NEON` | Isolated ARMv7 NEON source |
| Other `armeabi-v7a` | Scalar |
| x86/x86_64 with AVX2 | AVX2 |
| x86/x86_64 with SSE2 | SSE2 |
| Other | Scalar |

AVX2/SSE2 use function target attributes, so no global ISA option is needed.
ARMv7 NEON is in `simd_neon_armv7.cpp` and gets `-mfpu=neon` only for that
translation unit. CMake enables it only for `ANDROID_ABI=armeabi-v7a` and
defines `BSPARSER_ARMV7_NEON_DISPATCH` for the dispatcher.

Do not add AVX/NEON instructions to generic parser code. Add an isolated
implementation plus runtime feature detection instead.

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

# ARMv7: include the isolated NEON implementation and dispatch define
& $NDK_CLANG --target=armv7a-linux-androideabi28 -std=c++17 -Wall -Wextra -Wpedantic `
  -DBSPARSER_ARMV7_NEON_DISPATCH=1 -mfpu=neon -I cpp\include `
  cpp\bsparser.cpp cpp\simd_neon_armv7.cpp cpp\tests.cpp -o bsparser_tests_armv7
```

The produced files are Android executables, not Windows executables. Push one
to a device/emulator and run it with `adb shell` to execute assertions.

## Parser coverage vs. JavaScript reference

The original browser parser is `java-script/parser.js` (~223 KB) and has more
complete syntax coverage. The C++ port focuses on practical timeline data:
unit boundaries, NAL/OBU type, keyframe classification, profile/IDs, and
dimensions where inexpensive to read.

When porting behavior, use the JavaScript file as the syntax reference but do
not copy its global-header state or browser-specific UI behavior. Preserve the
C++ API’s safe exceptions and streaming semantics.
