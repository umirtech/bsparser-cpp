# bsparser C++ port

`bsparser` is a dependency-free C++17 library and command-line inspector for
raw video elementary streams. It is a native port of the JavaScript project's
stream-oriented parsing model, with bounds-checked bit parsing and a compact,
typed public API.

Supported inputs:

- VP8 and VP9 complete frames
- AV1 raw OBU streams (including sequence and frame headers)
- AVC/H.264, HEVC/H.265, and VVC/H.266 Annex-B streams
- IVF containers containing VP8, VP9, AV1, AVC, HEVC, or VVC frames

The parser reports NAL/OBU/frame boundaries and core syntax metadata (types,
keyframe status, IDs, dimensions where present, profiles, and timestamps).
It deliberately stops before entropy-coded slice/tile payloads: this is a
bitstream inspector, not a decoder.

## Build

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

## Command line

```sh
./build/bsparse avc clip.264
./build/bsparse hevc clip.265
./build/bsparse av1 clip.obu
./build/bsparse ivf clip.ivf
```

Each output line is a JSON object, so it is easy to pipe into `jq` or ingest
from another tool.

## Library API

### 1. Stream boundary detection (recommended)

`UnitScanner` accepts arbitrary network/file chunks and returns only complete
units. This is the easiest way to locate NAL/OBU/frame boundaries without
having to retain a partial start code or AV1 size field yourself.

```cpp
#include <bsparser/bsparser.hpp>

bsparser::UnitScanner scanner(bsparser::Codec::HEVC);

for (auto& chunk : network_chunks) {
  for (const bsparser::Unit& unit : scanner.feed(chunk.data(), chunk.size())) {
    // unit.offset: absolute source position
    // unit.bytes: complete NAL payload (start code excluded)
    // unit.type, unit.keyframe, and unit.frame_start are already populated
    auto metadata = bsparser::parse_unit(bsparser::Codec::HEVC, unit);
  }
}
for (const bsparser::Unit& unit : scanner.finish()) {
  auto metadata = bsparser::parse_unit(bsparser::Codec::HEVC, unit);
}
```

For AVC, `frame_start` is detected from `first_mb_in_slice == 0`; for HEVC it
uses `first_slice_segment_in_pic_flag`; for AV1, frame and frame-header OBUs
are marked. VVC picture-header NALs are marked. A raw concatenation of VP8 or
VP9 has no framing syntax, so each call to `feed()` is one caller-supplied
complete frame.

### 2. Parse an individual complete unit

If your transport has already separated frames/NALs/OBUs, call `parse_unit`.
For AVC, HEVC, and VVC, pass a NAL payload without the Annex-B start code.

```cpp
std::vector<uint8_t> nal_payload = read_one_nal();
auto headers = bsparser::parse_unit(bsparser::Codec::AVC, nal_payload, file_offset);
```

### 3. Metadata-only streaming wrapper

```cpp
#include <bsparser/bsparser.hpp>

bsparser::StreamParser parser(bsparser::Codec::AVC);
auto headers = parser.feed(chunk.data(), chunk.size());
auto last_headers = parser.finish();  // call once at EOF
```

`StreamParser` emits parsed metadata directly. Use `UnitScanner` when the
application also needs the original unit bytes or frame-boundary signals.

`IvfParser` accepts arbitrary chunk sizes and detects the codec from the IVF
FourCC.

## Realtime performance

The Annex-B start-code search uses a runtime-selected implementation:

- AVX2 on supported x86/x86-64 CPUs; SSE2 otherwise
- NEON on `arm64-v8a` Android devices
- Runtime-checked NEON on `armeabi-v7a`; scalar fallback on older ARMv7 CPUs
- a safe scalar fallback everywhere else

No global `-mavx2` or `-mfpu=neon` flag is required for the primary library
source. The CMake build compiles ARMv7 NEON into an isolated source file, so
the same APK works on ARMv7 devices without NEON. Inspect the selected path
with:

```cpp
std::cout << bsparser::scan_backend_name(bsparser::active_scan_backend());
```

For a timeline clip already held in one contiguous buffer, use the zero-copy
scanner to get every Annex-B boundary without creating `Unit` objects:

```cpp
auto offsets = bsparser::find_annexb_start_codes(data, size);
```

`UnitScanner` avoids a `std::vector::erase()` for every NAL; it consumes via a
cursor and only compacts its internal buffer occasionally. For best throughput,
feed reasonably sized chunks (for example 64 KiB or larger) and parse only the
parameter sets and frames needed by the visible timeline range.

| Android ABI | Fast boundary-search path |
| --- | --- |
| `armeabi-v7a` | NEON when CPU-supported; scalar otherwise |
| `arm64-v8a` | NEON |
| `x86` | SSE2; AVX2 when CPU-supported |
| `x86_64` | SSE2; AVX2 when CPU-supported |
