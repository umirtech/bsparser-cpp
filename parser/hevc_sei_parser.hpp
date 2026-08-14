#pragma once

#include "hevc_sei.hpp"
#include "hevc_nal_unit.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace bs {

/*
 * -----------------------------------------------------------
 * SEI parser errors
 * -----------------------------------------------------------
 */

class SeiParseError : public std::runtime_error {
   public:
    explicit SeiParseError(const char* message) : std::runtime_error(message) {}
};

/*
 * -----------------------------------------------------------
 * RBSP-span parser
 * -----------------------------------------------------------
 *
 * This is the preferred zero-copy entry point.
 *
 * `rbsp` MUST already be RBSP data, not EBSP data.
 *
 * All SeiMessageView::payload spans point directly into
 * `rbsp`.
 */

[[nodiscard]]
inline bool parse_sei_rbsp_view(
    std::span<const std::byte> rbsp, SeiNalUnitKind nal_kind, SeiRbspView& result
) {
    return parse_sei_rbsp(rbsp, nal_kind, result);
}

/*
 * -----------------------------------------------------------
 * Payload type helper
 * -----------------------------------------------------------
 *
 * Convert the NAL type into the semantic SEI location.
 */

[[nodiscard]]
constexpr SeiNalUnitKind sei_nal_kind_from_type(NalUnitType type) {
    switch (type) {
        case NalUnitType::PREFIX_SEI_NUT:
            return SeiNalUnitKind::Prefix;

        case NalUnitType::SUFFIX_SEI_NUT:
            return SeiNalUnitKind::Suffix;

        default:
            throw SeiParseError(
                "SEI parser: NAL is not a PREFIX_SEI_NUT or "
                "SUFFIX_SEI_NUT"
            );
    }
}

/*
 * -----------------------------------------------------------
 * NAL type validation
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool is_sei_nal(NalUnitType type) noexcept {
    return type == NalUnitType::PREFIX_SEI_NUT || type == NalUnitType::SUFFIX_SEI_NUT;
}

[[nodiscard]]
constexpr bool is_sei_nal(const NalUnit& nal) noexcept {
    return is_sei_nal(nal.type());
}

/*
 * -----------------------------------------------------------
 * RBSP byte extraction
 * -----------------------------------------------------------
 *
 * Convert EBSP -> RBSP.
 *
 * This is intentionally a separate operation because
 * SeiMessageView requires contiguous spans.
 *
 * If emulation-prevention bytes exist, the resulting RBSP
 * storage must outlive the SeiRbspView.
 */

inline void materialize_rbsp(std::span<const std::uint8_t> ebsp, std::vector<std::byte>& rbsp) {
    rbsp.clear();
    rbsp.reserve(ebsp.size());

    for (std::size_t i = 0; i < ebsp.size(); ++i) {
        /*
         * Remove emulation-prevention byte 0x03 from:
         *
         *     00 00 03 00
         *     00 00 03 01
         *     00 00 03 02
         *     00 00 03 03
         */
        if (i >= 2 && ebsp[i - 2] == 0x00 && ebsp[i - 1] == 0x00 && ebsp[i] == 0x03 &&
            i + 1 < ebsp.size() && ebsp[i + 1] <= 0x03) {
            continue;
        }

        rbsp.push_back(static_cast<std::byte>(ebsp[i]));
    }
}

/*
 * -----------------------------------------------------------
 * Parse an SEI NAL with caller-owned scratch storage
 * -----------------------------------------------------------
 *
 * IMPORTANT:
 *
 * `rbsp_storage` must remain alive while `result.messages`
 * are being used.
 *
 * The payload spans point into rbsp_storage.
 */

[[nodiscard]]
inline bool parse_sei_nal(
    const NalUnit& nal, std::vector<std::byte>& rbsp_storage, SeiRbspView& result
) {
    if (!is_sei_nal(nal)) {
        return false;
    }

    const auto kind = sei_nal_kind_from_type(nal.type());

    materialize_rbsp(nal.payload_bytes(), rbsp_storage);

    return parse_sei_rbsp(
        std::span<const std::byte>{rbsp_storage.data(), rbsp_storage.size()}, kind, result
    );
}

/*
 * -----------------------------------------------------------
 * Convenience owning parse result
 * -----------------------------------------------------------
 *
 * This object owns the materialized RBSP so that all
 * SeiMessageView payload spans remain valid.
 */

struct ParsedSei {
    std::vector<std::byte> rbsp_storage{};

    SeiRbspView view{};

    [[nodiscard]]
    bool empty() const noexcept {
        return view.empty();
    }

    [[nodiscard]]
    std::size_t size() const noexcept {
        return view.size();
    }

    [[nodiscard]]
    const SeiMessageView* find(std::uint32_t payload_type) const noexcept {
        return find_sei_message(view, payload_type);
    }
};

/*
 * -----------------------------------------------------------
 * Owning SEI NAL parser
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline ParsedSei parse_sei_nal(const NalUnit& nal) {
    ParsedSei result{};

    if (!parse_sei_nal(nal, result.rbsp_storage, result.view)) {
        throw SeiParseError("SEI parser: invalid SEI NAL unit");
    }

    return result;
}

/*
 * -----------------------------------------------------------
 * Message header parsing
 * -----------------------------------------------------------
 *
 * Exposed for applications that want to inspect a message
 * stream incrementally.
 */

[[nodiscard]]
inline bool parse_sei_message_header(
    std::span<const std::byte> data,
    std::uint32_t& payload_type,
    std::uint32_t& payload_size,
    std::size_t& header_size
) {
    SeiMessageHeader header{};

    if (!parse_sei_message_header(data, header)) {
        payload_type = 0;
        payload_size = 0;
        header_size = 0;
        return false;
    }

    payload_type = header.payload_type;
    payload_size = header.payload_size;
    header_size = header.header_size;

    return true;
}

/*
 * -----------------------------------------------------------
 * Message iteration
 * -----------------------------------------------------------
 */

template <typename Callback>
inline void for_each_sei_message(const SeiRbspView& sei, Callback&& callback) {
    for (const auto& message : sei.messages) {
        callback(message);
    }
}

/*
 * -----------------------------------------------------------
 * Typed payload helpers
 * -----------------------------------------------------------
 *
 * These simply expose the typed parsing helpers already
 * defined by hevc_sei.hpp.
 */

/*
 * User-data-unregistered.
 */
[[nodiscard]]
inline UserDataUnregisteredView user_data_unregistered(const SeiMessageView& message) {
    UserDataUnregisteredView result{};

    if (!parse_user_data_unregistered(message, result)) {
        throw SeiParseError("SEI: invalid user_data_unregistered payload");
    }

    return result;
}

/*
 * Mastering display colour volume.
 */
[[nodiscard]]
inline MasteringDisplayColourVolume mastering_display_colour_volume(const SeiMessageView& message) {
    MasteringDisplayColourVolume result{};

    if (!parse_mastering_display_colour_volume(message, result)) {
        throw SeiParseError("SEI: invalid mastering display colour volume");
    }

    return result;
}

/*
 * Content light level information.
 */
[[nodiscard]]
inline ContentLightLevelInfo content_light_level_info(const SeiMessageView& message) {
    ContentLightLevelInfo result{};

    if (!parse_content_light_level_info(message, result)) {
        throw SeiParseError("SEI: invalid content light level information");
    }

    return result;
}

/*
 * -----------------------------------------------------------
 * Find typed SEI payload
 * -----------------------------------------------------------
 */

template <typename T>
[[nodiscard]]
const T* find_typed_sei(const SeiRbspView&, std::uint32_t) {
    /*
     * Intentionally unsupported generic form.
     *
     * Typed SEI structures have different parsing rules and
     * should be handled explicitly.
     */
    return nullptr;
}

/*
 * -----------------------------------------------------------
 * Common HDR convenience functions
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline bool get_mastering_display_colour_volume(
    const SeiRbspView& sei, MasteringDisplayColourVolume& result
) {
    const auto* message = find_sei_message(
        sei, static_cast<std::uint32_t>(SeiPayloadType::MasteringDisplayColourVolume)
    );

    if (message == nullptr) {
        return false;
    }

    return parse_mastering_display_colour_volume(*message, result);
}

[[nodiscard]]
inline bool get_content_light_level_info(const SeiRbspView& sei, ContentLightLevelInfo& result) {
    const auto* message =
        find_sei_message(sei, static_cast<std::uint32_t>(SeiPayloadType::ContentLightLevelInfo));

    if (message == nullptr) {
        return false;
    }

    return parse_content_light_level_info(*message, result);
}

[[nodiscard]]
inline bool get_user_data_unregistered(const SeiRbspView& sei, UserDataUnregisteredView& result) {
    const auto* message =
        find_sei_message(sei, static_cast<std::uint32_t>(SeiPayloadType::UserDataUnregistered));

    if (message == nullptr) {
        return false;
    }

    return parse_user_data_unregistered(*message, result);
}

/*
 * -----------------------------------------------------------
 * SEI validation
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline bool validate_sei(const SeiRbspView& sei) noexcept {
    for (const auto& message : sei.messages) {
        if (!message.size_matches()) {
            return false;
        }

        /*
         * The span must never exceed the signaled size.
         */
        if (message.payload.size() > static_cast<std::size_t>(message.payload_size)) {
            return false;
        }
    }

    return true;
}

/*
 * -----------------------------------------------------------
 * SEI message counts
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline std::size_t count_sei_messages(const SeiRbspView& sei, std::uint32_t payload_type) noexcept {
    std::size_t count = 0;

    for (const auto& message : sei.messages) {
        if (message.payload_type == payload_type) {
            ++count;
        }
    }

    return count;
}

/*
 * -----------------------------------------------------------
 * Prefix/suffix convenience
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool is_prefix_sei(const SeiRbspView& sei) noexcept {
    return sei.nal_unit_kind == SeiNalUnitKind::Prefix;
}

[[nodiscard]]
constexpr bool is_suffix_sei(const SeiRbspView& sei) noexcept {
    return sei.nal_unit_kind == SeiNalUnitKind::Suffix;
}

}  // namespace bs