#pragma once

#include <cstdint>

namespace bs {
namespace vp8 {

/*
 * -----------------------------------------------------------
 * VP8 uncompressed frame header (RFC 6386 §9.1)
 * -----------------------------------------------------------
 *
 * The first three bytes of every frame are the frame_tag:
 *
 *     key_frame_flag (1) · version (3) · show_frame (1) ·
 *     first_part_size (19)
 *
 * Key frames additionally carry a start code (0x9d 0x01 0x2a)
 * followed by the 14-bit frame dimensions.
 *
 * Only the uncompressed header is modelled; the entropy-coded
 * residue is not parsed (same scope as the HEVC/AVC parsers,
 * which parse syntax, not the coded data).
 */
struct FrameHeader {
    bool key_frame = false;

    /*
     * version (3 bits, 0..7).
     */
    std::uint8_t version = 0;

    bool show_frame = false;

    /*
     * first_part_size (19 bits).
     */
    std::uint32_t first_part_size = 0;

    /*
     * True when a key frame carried the expected start code.
     */
    bool start_code_ok = false;

    /*
     * Display size (14 bits each), present on key frames.
     */
    std::uint16_t width = 0;
    std::uint16_t height = 0;

    [[nodiscard]]
    bool valid() const noexcept {
        return !key_frame || start_code_ok;
    }
};

}  // namespace vp8
}  // namespace bs
