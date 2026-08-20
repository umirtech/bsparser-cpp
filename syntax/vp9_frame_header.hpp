// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstdint>

namespace bs {
namespace vp9 {

/*
 * -----------------------------------------------------------
 * VP9 uncompressed frame header
 * -----------------------------------------------------------
 *
 * Modelled fields follow the VP9 bitstream specification
 * (uncompressed_header):
 *
 *     frame_marker (2, =0b10) · profile (2) ·
 *     show_existing_frame (1)
 *     if !show_existing: frame_type · show_frame ·
 *     error_resilient_mode · frame_size ...
 *
 * Only the uncompressed (header) portion is modelled.
 */
enum class FrameType : std::uint8_t { KeyFrame = 0, InterFrame = 1 };

struct FrameHeader {
    /*
     * frame_marker must equal 0b10.
     */
    std::uint8_t frame_marker = 0;

    /*
     * profile (0..3).
     */
    std::uint8_t profile = 0;

    bool show_existing_frame = false;

    /*
     * frame_to_show_map_idx (3 bits), when show_existing_frame.
     */
    std::uint8_t frame_to_show_map_idx = 0;

    bool show_frame = false;

    bool error_resilient_mode = false;

    FrameType frame_type = FrameType::InterFrame;

    /*
     * intra_only, present on inter frames.
     */
    bool intra_only = false;

    /*
     * Coded width/height (16 bits each). Present for key frames
     * and intra-only frames; for inter frames with references the
     * dimensions are derived from the reference frames, so these
     * remain 0 and frame_size_from_refs is set.
     */
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool frame_size_present = false;

    /*
     * Set for inter frames whose size is copied from a reference
     * frame (frame_size_with_refs), so width/height are absent.
     */
    bool frame_size_from_refs = false;

    /*
     * Decode-order index of this frame, filled by the unified dispatch layer.
     * VP9 has no POC; the display order of a raw stream is the decode order.
     */
    std::int32_t presentation_order = 0;

    [[nodiscard]]
    bool valid() const noexcept {
        return frame_marker == 0b10u;
    }
};

}  // namespace vp9
}  // namespace bs
