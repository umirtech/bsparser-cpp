#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bs {
namespace avc {

/*
 * One SEI message (7.3.2.4).
 *
 * payload references materialized RBSP storage owned by the
 * ParsedSei that produced it.
 */
struct SeiMessage {
    std::uint32_t payload_type = 0;
    std::uint32_t payload_size = 0;
    std::span<const std::uint8_t> payload{};
};

struct ParsedSei {
    /*
     * De-emulation-prevented RBSP storage.
     *
     * Kept alive for as long as the messages (which reference
     * it) are used.
     */
    std::vector<std::uint8_t> rbsp_storage{};

    std::vector<SeiMessage> messages{};
};

}  // namespace avc
}  // namespace bs