#pragma once

#include <cstdint>

namespace bs {
namespace av1 {

/*
 * -----------------------------------------------------------
 * AV1 sequence header (AV1 §5.5)
 * -----------------------------------------------------------
 * Leading fields only. Everything is boolean-coded.
 */
struct SequenceHeader {
    std::uint8_t seq_profile = 0;

    bool still_picture = false;

    bool reduced_still_picture_header = false;

    /*
     * Max frame dimensions. Present in reduced-still-picture
     * headers; otherwise not parsed here.
     */
    std::uint32_t max_frame_width = 0;
    std::uint32_t max_frame_height = 0;
    bool dimensions_present = false;

    [[nodiscard]]
    bool valid() const noexcept {
        return seq_profile <= 7;
    }
};

}  // namespace av1
}  // namespace bs
