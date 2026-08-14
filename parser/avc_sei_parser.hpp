#pragma once

#include "avc_nal_unit.hpp"
#include "avc_parse_common.hpp"
#include "avc_sei.hpp"
#include "rbsp_bitstream_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace bs {
namespace avc {

/*
 * -----------------------------------------------------------
 * AVC SEI parser (7.3.2.4)
 * -----------------------------------------------------------
 *
 * The NAL payload (EBSP) is read through RbspBitstreamReader,
 * which strips emulation-prevention bytes logically, so the
 * materialized payload bytes are clean RBSP bytes.
 */

[[nodiscard]]
inline ParsedSei parse_sei_nal(const NalUnit& nal) {
    const auto payload = nal.payload_bytes();

    const auto byte_span = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(payload.data()), payload.size()
    );

    RbspBitstreamReader reader(byte_span);

    ParsedSei sei;

    while (reader.more_rbsp_data()) {
        /*
         * payload_type / payload_size are 8-bit fields that
         * may be chained with 0xFF extensions.
         */
        std::uint32_t payload_type = 0;
        std::uint8_t byte = 0;

        do {
            byte = reader.read_u8();
            payload_type += byte;
        } while (byte == 0xFF);

        std::uint32_t payload_size = 0;

        do {
            byte = reader.read_u8();
            payload_size += byte;
        } while (byte == 0xFF);

        if (static_cast<std::uint64_t>(payload_size) > reader.bits_remaining() / 8) {
            throw ParseError("sei: payload size exceeds RBSP");
        }

        SeiMessage message;

        message.payload_type = payload_type;
        message.payload_size = payload_size;

        const std::size_t offset = sei.rbsp_storage.size();

        sei.rbsp_storage.reserve(sei.rbsp_storage.size() + payload_size);

        for (std::uint32_t i = 0; i < payload_size; ++i) {
            sei.rbsp_storage.push_back(reader.read_u8());
        }

        message.payload =
            std::span<const std::uint8_t>(sei.rbsp_storage.data() + offset, payload_size);

        sei.messages.push_back(message);
    }

    reader.read_rbsp_trailing_bits();

    return sei;
}

}  // namespace avc
}  // namespace bs