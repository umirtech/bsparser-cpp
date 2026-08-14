#pragma once

#include "av1_frame_header.hpp"

#include <bitstream/boolean_decoder.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace bs {
namespace av1 {

/*
 * -----------------------------------------------------------
 * AV1 frame header parser (AV1 §5.7) — leading fields
 * -----------------------------------------------------------
 * Reads the first few boolean-coded fields of the frame header
 * OBU payload.
 */
[[nodiscard]]
inline FrameHeader
parse_frame_header(
    std::span<const std::uint8_t> payload)
{
    BooleanDecoder bd{payload};

    FrameHeader fh;

    fh.frame_type =
        static_cast<FrameType>(bd.read_literal(2));

    fh.show_frame = bd.read_bool(128);

    fh.error_resilient_mode = bd.read_bool(128);

    fh.disable_cdf_update = bd.read_bool(128);

    fh.allow_screen_content_tools = bd.read_bool(128);

    return fh;
}

} // namespace av1
} // namespace bs
