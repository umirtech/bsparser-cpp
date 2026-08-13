#pragma once

#include "hevc_nal_unit_header.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace bs {

/*
 * H.265 / HEVC NAL unit
 *
 * A NAL unit consists conceptually of:
 *
 *     nal_unit_header()
 *     rbsp_byte[]
 *
 * This structure is intentionally non-owning.
 *
 * The payload is represented by:
 *
 *     std::span<const std::uint8_t>
 *
 * so parsing can operate directly over the caller's buffer.
 *
 * Lifetime requirement:
 *
 *     The memory referenced by payload must remain alive
 *     while this NalUnit is being used.
 */


/*
 * -----------------------------------------------------------
 * NAL payload view
 * -----------------------------------------------------------
 */

struct NalPayloadView {

    /*
     * Bytes following the two-byte NAL header.
     *
     * This is still EBSP data if the NAL came directly from
     * an Annex-B / length-prefixed bitstream.
     *
     * Emulation-prevention bytes have NOT been removed here.
     */
    std::span<const std::uint8_t> bytes{};


    [[nodiscard]]
    constexpr const std::uint8_t*
    data() const noexcept
    {
        return bytes.data();
    }


    [[nodiscard]]
    constexpr std::size_t
    size() const noexcept
    {
        return bytes.size();
    }


    [[nodiscard]]
    constexpr bool
    empty() const noexcept
    {
        return bytes.empty();
    }


    [[nodiscard]]
    constexpr const std::uint8_t&
    operator[](std::size_t index) const noexcept
    {
        return bytes[index];
    }


    [[nodiscard]]
    constexpr auto
    begin() const noexcept
    {
        return bytes.begin();
    }


    [[nodiscard]]
    constexpr auto
    end() const noexcept
    {
        return bytes.end();
    }
};


/*
 * -----------------------------------------------------------
 * Complete NAL unit view
 * -----------------------------------------------------------
 */

struct NalUnit {

    /*
     * Decoded two-byte NAL header.
     */
    NalUnitHeader header{};

    /*
     * Non-owning payload view.
     *
     * This is EBSP payload, not yet converted to RBSP.
     */
    NalPayloadView payload{};


    /*
     * -------------------------------------------------------
     * Header access
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr NalUnitType
    type() const noexcept
    {
        return header.nal_unit_type;
    }


    [[nodiscard]]
    constexpr std::uint8_t
    nal_type() const noexcept
    {
        return header.nal_type();
    }


    [[nodiscard]]
    constexpr std::uint8_t
    layer_id() const noexcept
    {
        return header.nuh_layer_id;
    }


    [[nodiscard]]
    constexpr std::uint8_t
    temporal_id() const noexcept
    {
        return header.temporal_id();
    }


    /*
     * -------------------------------------------------------
     * Classification
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr bool
    is_vcl() const noexcept
    {
        return is_vcl_nal_unit(
            header.nal_unit_type);
    }


    [[nodiscard]]
    constexpr bool
    is_irap() const noexcept
    {
        return is_irap_nal_unit(
            header.nal_unit_type);
    }


    [[nodiscard]]
    constexpr bool
    is_idr() const noexcept
    {
        return is_idr_nal_unit(
            header.nal_unit_type);
    }


    [[nodiscard]]
    constexpr bool
    is_cra() const noexcept
    {
        return is_cra_nal_unit(
            header.nal_unit_type);
    }


    [[nodiscard]]
    constexpr bool
    is_reference_picture() const noexcept
    {
        return is_reference_vcl_nal_unit(
            header.nal_unit_type);
    }


    [[nodiscard]]
    constexpr bool
    is_parameter_set() const noexcept
    {
        return bs::is_parameter_set(header);
    }


    [[nodiscard]]
    constexpr bool
    is_sei() const noexcept
    {
        return is_sei_nal_unit(
            header.nal_unit_type);
    }


    /*
     * -------------------------------------------------------
     * Payload access
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr std::span<const std::uint8_t>
    payload_bytes() const noexcept
    {
        return payload.bytes;
    }


    [[nodiscard]]
    constexpr std::size_t
    payload_size() const noexcept
    {
        return payload.size();
    }


    [[nodiscard]]
    constexpr bool
    has_payload() const noexcept
    {
        return !payload.empty();
    }


    /*
     * -------------------------------------------------------
     * Validation
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr bool
    valid() const noexcept
    {
        return header.valid();
    }
};


/*
 * -----------------------------------------------------------
 * Construction helpers
 * -----------------------------------------------------------
 */


/*
 * Construct a zero-copy NAL unit view.
 */
[[nodiscard]]
constexpr NalUnit
make_nal_unit(
    const NalUnitHeader& header,
    std::span<const std::uint8_t> payload) noexcept
{
    return NalUnit{
        header,
        NalPayloadView{payload}
    };
}


/*
 * Convenience overload for a pointer + size.
 */
[[nodiscard]]
constexpr NalUnit
make_nal_unit(
    const NalUnitHeader& header,
    const std::uint8_t* data,
    std::size_t size) noexcept
{
    return make_nal_unit(
        header,
        std::span<const std::uint8_t>{
            data,
            size
        });
}


/*
 * -----------------------------------------------------------
 * NAL type predicates
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool
is_vps(
    const NalUnit& nal) noexcept
{
    return nal.type() == NalUnitType::VPS_NUT;
}


[[nodiscard]]
constexpr bool
is_sps(
    const NalUnit& nal) noexcept
{
    return nal.type() == NalUnitType::SPS_NUT;
}


[[nodiscard]]
constexpr bool
is_pps(
    const NalUnit& nal) noexcept
{
    return nal.type() == NalUnitType::PPS_NUT;
}


[[nodiscard]]
constexpr bool
is_prefix_sei(
    const NalUnit& nal) noexcept
{
    return nal.type() ==
        NalUnitType::PREFIX_SEI_NUT;
}


[[nodiscard]]
constexpr bool
is_suffix_sei(
    const NalUnit& nal) noexcept
{
    return nal.type() ==
        NalUnitType::SUFFIX_SEI_NUT;
}


/*
 * -----------------------------------------------------------
 * RBSP payload note
 * -----------------------------------------------------------
 *
 * The payload stored above is EBSP.
 *
 * HEVC uses emulation-prevention bytes:
 *
 *     00 00 03 xx
 *
 * where the 03 byte must be removed when constructing RBSP.
 *
 * We deliberately do NOT perform that transformation here.
 *
 * The next layer will provide a zero-copy/read-through RBSP
 * view backed by the original span where possible.
 *
 * This keeps:
 *
 *     NAL framing
 *         ↓
 *     EBSP view
 *         ↓
 *     RBSP bit reader
 *
 * as separate responsibilities.
 */


/*
 * -----------------------------------------------------------
 * Basic NAL size helper
 * -----------------------------------------------------------
 *
 * A complete NAL unit has:
 *
 *     2 bytes header
 *     payload bytes
 *
 * This helper intentionally does not account for:
 *
 *     Annex-B start codes
 *     length prefixes
 *
 * because those belong to the container/framing layer.
 */

[[nodiscard]]
constexpr std::size_t
nal_unit_size(
    const NalUnit& nal) noexcept
{
    return 2 + nal.payload_size();
}

} // namespace bs