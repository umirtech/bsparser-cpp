# Agent Guide — bsparser

## Goal

Maintain a C++17, dependency-free **header-level** raw-video bitstream inspector.
It finds elementary-unit boundaries and extracts lightweight metadata; it is
not a decoder and must not attempt entropy-coded payload parsing.

## Read first

1. `cpp/include/bsparser/bsparser.hpp` — public contract.
2. `docs/CODEBASE.md` — architecture, invariants, and ABI details.
3. `cpp/tests.cpp` — minimal behavioral examples.

## Project map

| Path | Purpose |
| --- | --- |
| `cpp/bsparser.cpp` | Parsing, streaming scanners, SIMD dispatch |
| `cpp/simd_neon_armv7.cpp` | Isolated optional ARMv7 NEON implementation |
| `cpp/include/bsparser/bsparser.hpp` | Library API |
| `cpp/main.cpp` | JSON-lines CLI |
| `cpp/tests.cpp` | Assertion-based tests |
| `CMakeLists.txt` | Library/executable/test targets + ARMv7 build rule |
| `java-script/parser.js` | Original reference implementation |

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

1. Update `bsparser.hpp` and `cpp/README.md` for public API changes.
2. Add a focused assertion to `cpp/tests.cpp` for each boundary/parser bug.
3. Format with the repository `.clang-format`.
4. Compile all relevant Android ABIs when SIMD/build logic changes.

## NDK compile examples

Use the Android Studio NDK Clang executable. The exact local path is
environment-specific; see `docs/CODEBASE.md` for commands. Build outputs are
Android ELF executables and require an emulator/device plus `adb` to run.

## Scope guidance

Prefer small, explicit syntax readers with named fields. Before porting a
large JavaScript syntax section, confirm it is useful for boundary detection,
timeline metadata, or keyframe indexing.

## Active migration: JavaScript-reference compatibility

The user has requested that the public `UnitScanner` and `Header` behavior
match `reference/bsparser-web/parser.js`, including its legacy edge cases.
This supersedes the earlier frame-boundary and strict Annex-B semantics above.

- Annex-B uses only the `00 00 01` marker. A four-byte prefix is treated as a
  marker beginning at its second zero, matching the reference's retained-zero
  behavior.
- `Unit.frame_start` is always `false`; do not add access-unit detection.
- Use JavaScript keyframe rules: AVC type 5; HEVC types 19/20; no VVC
  keyframe classification.
- Keep `tests/compare-reference.js` as the compatibility test. It currently
  compares header count and codec-level unit kinds for H.264, H.265, VP8, and
  VP9 fixture files.
- Continue porting metadata readers from `reference/bsparser-web/parser.js`
  into `src/bsparser.cpp` before strengthening comparison to exact fields.
  Match JavaScript field names and type labels rather than inventing C++-only
  names or inferred fields.
- The bundled JavaScript AV1 parser currently crashes on `test-files/av1.ivf`
  because `frame_width_bits_minus_1` is unbound. Decide with the user whether
  to reproduce that failure or repair the JavaScript reference before adding
  AV1 exact-parity assertions.
- Run `ctest --test-dir build -C Debug --output-on-failure` after each change.
