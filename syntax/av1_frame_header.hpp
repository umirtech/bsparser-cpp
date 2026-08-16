#pragma once

#include "av1_common.hpp"

#include <cstdint>

namespace bs {
namespace av1 {

/*
 * -----------------------------------------------------------
 * AV1 frame header (AV1 §5.7) — leading fields
 * -----------------------------------------------------------
 * Everything is boolean-coded.  Parsed through the `order_hint` field, which
 * is the native presentation-order signal (sort frames by it, wrap-aware).
 */
struct FrameHeader {
    FrameType frame_type = FrameType::InterFrame;

    bool show_frame = false;

    bool show_existing_frame = false;

    std::uint8_t frame_to_show_map_idx = 0;

    bool showable_frame = false;

    bool error_resilient_mode = false;

    bool disable_cdf_update = false;

    bool allow_screen_content_tools = false;

    bool force_integer_mv = false;

    /*
     * order_hint (AV1 §5.9.2): the presentation-order signal.  Its width is
     * OrderHintBits from the sequence header; frames are presented in order
     * of order_hint (compared wrap-aware via get_relative_dist).
     */
    std::uint32_t order_hint = 0;

    /*
     * Decode-order index of this frame, filled by the unified dispatch layer.
     */
    std::int32_t presentation_order = 0;

    [[nodiscard]]
    bool valid() const noexcept {
        return true;
    }
};

}  // namespace av1
}  // namespace bs
