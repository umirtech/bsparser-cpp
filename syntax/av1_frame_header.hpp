#pragma once

#include "av1_common.hpp"

#include <cstdint>

namespace bs {
namespace av1 {

/*
 * -----------------------------------------------------------
 * AV1 frame header (AV1 §5.7) — leading fields
 * -----------------------------------------------------------
 * Everything is boolean-coded.
 */
struct FrameHeader {

    FrameType frame_type = FrameType::InterFrame;

    bool show_frame = false;

    bool error_resilient_mode = false;

    bool disable_cdf_update = false;

    bool allow_screen_content_tools = false;


    [[nodiscard]]
    bool valid() const noexcept
    {
        return true;
    }
};

} // namespace av1
} // namespace bs
