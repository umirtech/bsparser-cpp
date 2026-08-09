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
