#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bs {

/*
 * H.265 SEI NAL units:
 *
 *     PREFIX_SEI_NUT = 39
 *     SUFFIX_SEI_NUT = 40
 *
 * The SEI RBSP contains zero or more:
 *
 *     sei_message()
 *
 * Each message has:
 *
 *     payloadType
 *     payloadSize
 *     sei_payload(payloadType, payloadSize)
 *
 * payloadType and payloadSize use the 0xFF extension
 * mechanism defined by H.265 7.3.5.
 */

/*
 * -----------------------------------------------------------
 * Known H.265 SEI payload types
 * -----------------------------------------------------------
 *
 * These are the commonly defined H.265 SEI payload types.
 *
 * Unknown/reserved values must still be preserved, so the
 * enum is never used as the storage type for the actual
 * payloadType.
 */
enum class SeiPayloadType : std::uint32_t {

    /*
     * Annex D / common H.265 SEI messages.
     */
    BufferingPeriod = 0,

    PictureTiming = 1,

    PanScanRect = 2,

    FillerPayload = 3,

    UserDataRegistered = 4,

    UserDataUnregistered = 5,

    RecoveryPoint = 6,

    SceneInfo = 9,

    FullFrameSnapshot = 15,

    ProgressiveRefinementSegmentStart = 16,

    ProgressiveRefinementSegmentEnd = 17,

    MotionConstrainedSliceGroupSet = 13,

    FilmGrainCharacteristics = 19,

    PostFilterHint = 22,

    ToneMappingInfo = 23,

    FramePackingArrangement = 45,

    DisplayOrientation = 47,

    GreenMetadata = 56,

    MasteringDisplayColourVolume = 137,

    ContentLightLevelInfo = 144,

    AlternativeTransferCharacteristics = 147,

    AmbientViewingEnvironment = 148,

    ContentColourVolume = 149
};

/*
 * Convert raw payloadType to the known enum when possible.
 *
 * Unknown values are represented by Unknown.
 */
enum class KnownSeiPayloadType : std::uint8_t {
    Unknown = 0,

    BufferingPeriod,
    PictureTiming,
    PanScanRect,
    FillerPayload,
    UserDataRegistered,
    UserDataUnregistered,
    RecoveryPoint,
    SceneInfo,
    FullFrameSnapshot,
    ProgressiveRefinementSegmentStart,
    ProgressiveRefinementSegmentEnd,
    MotionConstrainedSliceGroupSet,
    FilmGrainCharacteristics,
    PostFilterHint,
    ToneMappingInfo,
    FramePackingArrangement,
    DisplayOrientation,
    GreenMetadata,
    MasteringDisplayColourVolume,
    ContentLightLevelInfo,
    AlternativeTransferCharacteristics,
    AmbientViewingEnvironment,
    ContentColourVolume
};

/*
 * -----------------------------------------------------------
 * SEI message location
 * -----------------------------------------------------------
 */

enum class SeiNalUnitKind : std::uint8_t { Prefix, Suffix };

/*
 * -----------------------------------------------------------
 * Generic SEI payload view
 * -----------------------------------------------------------
 *
 * This is the important zero-copy object.
 *
 * payload points directly into the RBSP backing span.
 *
 * No payload bytes are copied.
 */
struct SeiMessageView {
    /*
     * Raw H.265 payloadType.
     *
     * Do NOT use SeiPayloadType here because future/reserved
     * values are legal to encounter.
     */
    std::uint32_t payload_type = 0;

    /*
     * Raw payload size in bytes.
     */
    std::uint32_t payload_size = 0;

    /*
     * Zero-copy payload.
     *
     * This points into the original RBSP buffer.
     */
    std::span<const std::byte> payload{};

    /*
     * -------------------------------------------------------
     * Helpers
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr bool empty() const noexcept {
        return payload.empty();
    }

    [[nodiscard]]
    constexpr std::size_t size() const noexcept {
        return payload.size();
    }

    [[nodiscard]]
    constexpr bool size_matches() const noexcept {
        return payload.size() == static_cast<std::size_t>(payload_size);
    }

    [[nodiscard]]
    constexpr KnownSeiPayloadType known_type() const noexcept {
        switch (payload_type) {
            case 0:
                return KnownSeiPayloadType::BufferingPeriod;

            case 1:
                return KnownSeiPayloadType::PictureTiming;

            case 2:
                return KnownSeiPayloadType::PanScanRect;

            case 3:
                return KnownSeiPayloadType::FillerPayload;

            case 4:
                return KnownSeiPayloadType::UserDataRegistered;

            case 5:
                return KnownSeiPayloadType::UserDataUnregistered;

            case 6:
                return KnownSeiPayloadType::RecoveryPoint;

            case 9:
                return KnownSeiPayloadType::SceneInfo;

            case 13:
                return KnownSeiPayloadType::MotionConstrainedSliceGroupSet;

            case 15:
                return KnownSeiPayloadType::FullFrameSnapshot;

            case 16:
                return KnownSeiPayloadType::ProgressiveRefinementSegmentStart;

            case 17:
                return KnownSeiPayloadType::ProgressiveRefinementSegmentEnd;

            case 19:
                return KnownSeiPayloadType::FilmGrainCharacteristics;

            case 22:
                return KnownSeiPayloadType::PostFilterHint;

            case 23:
                return KnownSeiPayloadType::ToneMappingInfo;

            case 45:
                return KnownSeiPayloadType::FramePackingArrangement;

            case 47:
                return KnownSeiPayloadType::DisplayOrientation;

            case 56:
                return KnownSeiPayloadType::GreenMetadata;

            case 137:
                return KnownSeiPayloadType::MasteringDisplayColourVolume;

            case 144:
                return KnownSeiPayloadType::ContentLightLevelInfo;

            case 147:
                return KnownSeiPayloadType::AlternativeTransferCharacteristics;

            case 148:
                return KnownSeiPayloadType::AmbientViewingEnvironment;

            case 149:
                return KnownSeiPayloadType::ContentColourVolume;

            default:
                return KnownSeiPayloadType::Unknown;
        }
    }

    [[nodiscard]]
    constexpr bool is_type(SeiPayloadType type) const noexcept {
        return payload_type == static_cast<std::uint32_t>(type);
    }

    [[nodiscard]]
    constexpr bool is_user_data_registered() const noexcept {
        return is_type(SeiPayloadType::UserDataRegistered);
    }

    [[nodiscard]]
    constexpr bool is_user_data_unregistered() const noexcept {
        return is_type(SeiPayloadType::UserDataUnregistered);
    }

    [[nodiscard]]
    constexpr bool is_mastering_display() const noexcept {
        return is_type(SeiPayloadType::MasteringDisplayColourVolume);
    }

    [[nodiscard]]
    constexpr bool is_content_light_level() const noexcept {
        return is_type(SeiPayloadType::ContentLightLevelInfo);
    }

    [[nodiscard]]
    constexpr bool is_picture_timing() const noexcept {
        return is_type(SeiPayloadType::PictureTiming);
    }

    [[nodiscard]]
    constexpr bool is_buffering_period() const noexcept {
        return is_type(SeiPayloadType::BufferingPeriod);
    }

    [[nodiscard]]
    constexpr bool is_recovery_point() const noexcept {
        return is_type(SeiPayloadType::RecoveryPoint);
    }
};

/*
 * -----------------------------------------------------------
 * SEI RBSP
 * -----------------------------------------------------------
 *
 * A single SEI NAL unit can contain multiple messages.
 */
struct SeiRbspView {
    SeiNalUnitKind nal_unit_kind = SeiNalUnitKind::Prefix;

    /*
     * Messages point directly into the RBSP storage.
     *
     * The vector itself owns only the descriptors, never
     * the payload bytes.
     */
    std::vector<SeiMessageView> messages{};

    [[nodiscard]]
    bool empty() const noexcept {
        return messages.empty();
    }

    [[nodiscard]]
    std::size_t size() const noexcept {
        return messages.size();
    }
};

/*
 * -----------------------------------------------------------
 * User-data-unregistered SEI
 * -----------------------------------------------------------
 *
 * payloadType = 5
 *
 * Syntax:
 *
 *     uuid_iso_iec_11578[16]
 *     user_data_payload_byte[]
 *
 * The payload bytes are kept as a span.
 */
struct UserDataUnregisteredView {
    std::array<std::byte, 16> uuid{};

    std::span<const std::byte> user_data_payload{};

    [[nodiscard]]
    constexpr bool valid() const noexcept {
        return true;
    }
};

/*
 * -----------------------------------------------------------
 * User-data-registered-by-T.35 SEI
 * -----------------------------------------------------------
 *
 * payloadType = 4
 *
 * The exact payload syntax depends on:
 *
 *     itu_t_t35_country_code
 *
 * and potentially:
 *
 *     itu_t_t35_country_code_extension_byte
 *
 * Therefore the generic payload remains a span.
 */
struct UserDataRegisteredView {
    std::uint8_t country_code = 0;

    bool country_code_extension_present = false;

    std::uint8_t country_code_extension_byte = 0;

    std::span<const std::byte> payload{};
};

/*
 * -----------------------------------------------------------
 * Mastering display colour volume
 * -----------------------------------------------------------
 *
 * This is commonly encountered in HDR10 HEVC streams.
 *
 * The payload uses 16-bit unsigned integer fields.
 *
 * Values are retained in their signaled integer units.
 */
struct MasteringDisplayColourVolume {
    struct Chromaticity {
        std::uint16_t x = 0;
        std::uint16_t y = 0;
    };

    Chromaticity display_primaries[3]{};

    Chromaticity white_point{};

    std::uint32_t max_display_mastering_luminance = 0;

    std::uint32_t min_display_mastering_luminance = 0;
};

/*
 * -----------------------------------------------------------
 * Content light level information
 * -----------------------------------------------------------
 *
 * payloadType = 144
 */
struct ContentLightLevelInfo {
    std::uint16_t max_content_light_level = 0;

    std::uint16_t max_pic_average_light_level = 0;
};

/*
 * -----------------------------------------------------------
 * Recovery point SEI
 * -----------------------------------------------------------
 *
 * payloadType = 6
 */
struct RecoveryPoint {
    /*
     * recovery_poc_cnt
     *
     * signed Exp-Golomb.
     */
    std::int32_t recovery_poc_cnt = 0;

    bool exact_match_flag = false;

    bool broken_link_flag = false;

    /*
     * changing_slice_group_idc
     *
     * 2 bits.
     */
    std::uint8_t changing_slice_group_idc = 0;
};

/*
 * -----------------------------------------------------------
 * Frame packing arrangement
 * -----------------------------------------------------------
 *
 * payloadType = 45
 */
struct FramePackingArrangement {
    std::uint32_t fp_arrangement_id = 0;

    bool fp_arrangement_cancel_flag = false;

    std::uint8_t fp_arrangement_type = 0;

    bool fp_quincunx_sampling_flag = false;

    std::uint8_t fp_content_interpretation_type = 0;

    bool fp_spatial_flipping_flag = false;

    bool fp_frame0_flipped_flag = false;

    bool fp_field_views_flag = false;

    bool fp_current_frame_is_frame0_flag = false;

    bool fp_frame0_self_contained_flag = false;

    bool fp_frame1_self_contained_flag = false;

    std::uint8_t fp_frame0_grid_position_x = 0;

    std::uint8_t fp_frame0_grid_position_y = 0;

    std::uint8_t fp_frame1_grid_position_x = 0;

    std::uint8_t fp_frame1_grid_position_y = 0;

    std::uint8_t fp_arrangement_reserved_byte = 0;

    bool fp_arrangement_persistence_flag = false;

    bool fp_upsampled_aspect_ratio_flag = false;
};

/*
 * -----------------------------------------------------------
 * Alternative transfer characteristics
 * -----------------------------------------------------------
 *
 * payloadType = 147
 */
struct AlternativeTransferCharacteristics {
    std::uint8_t preferred_transfer_characteristics = 0;
};

/*
 * -----------------------------------------------------------
 * Ambient viewing environment
 * -----------------------------------------------------------
 *
 * payloadType = 148
 */
struct AmbientViewingEnvironment {
    std::uint32_t ambient_illuminance = 0;

    std::uint16_t ambient_light_x = 0;

    std::uint16_t ambient_light_y = 0;
};

/*
 * -----------------------------------------------------------
 * Generic SEI message classification
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool is_prefix_sei_type(std::uint32_t payload_type) noexcept {
    /*
     * This function intentionally does not attempt to classify
     * every SEI message from payloadType alone.
     *
     * Whether a payload is permitted in prefix/suffix form
     * is defined by the individual SEI semantics.
     */
    return payload_type != 0xFFFFFFFFu;
}

/*
 * -----------------------------------------------------------
 * Payload type/size decoding helpers
 * -----------------------------------------------------------
 *
 * H.265 represents payloadType as:
 *
 *     while(next_bits(8) == 0xFF)
 *         payloadType += 255;
 *
 *     payloadType += last_payload_type_byte;
 *
 * Likewise payloadSize.
 *
 * These helpers operate on already-separated RBSP bytes.
 */

/*
 * Result of decoding one FF-extended integer.
 */
struct SeiExtendedValue {
    std::uint32_t value = 0;

    /*
     * Number of bytes consumed.
     */
    std::size_t bytes_consumed = 0;

    /*
     * false means the input ended before the terminating
     * byte was encountered.
     */
    bool complete = false;
};

/*
 * Decode a payloadType/payloadSize value from bytes.
 *
 * Example:
 *
 *     05
 *
 * gives:
 *
 *     value = 5
 *     bytes_consumed = 1
 *
 * Example:
 *
 *     FF FF 05
 *
 * gives:
 *
 *     value = 515
 *     bytes_consumed = 3
 */
[[nodiscard]]
inline SeiExtendedValue decode_sei_extended_value(std::span<const std::byte> data) {
    SeiExtendedValue result{};

    for (const std::byte byte : data) {
        const auto value = std::to_integer<std::uint8_t>(byte);

        ++result.bytes_consumed;

        if (value == 0xFFu) {
            /*
             * Avoid uint32 overflow if a malformed stream
             * contains an unreasonable number of FF bytes.
             */
            if (result.value > 0xFFFFFFFFu - 255u) {
                result.value = 0xFFFFFFFFu;

            } else {
                result.value += 255u;
            }

            continue;
        }

        /*
         * Final byte.
         */
        if (result.value > 0xFFFFFFFFu - static_cast<std::uint32_t>(value)) {
            result.value = 0xFFFFFFFFu;

        } else {
            result.value += value;
        }

        result.complete = true;
        return result;
    }

    return result;
}

/*
 * -----------------------------------------------------------
 * SEI message header parsing
 * -----------------------------------------------------------
 */

/*
 * Header information for one SEI message.
 *
 * The payload itself remains a span.
 */
struct SeiMessageHeader {
    std::uint32_t payload_type = 0;

    std::uint32_t payload_size = 0;

    std::size_t header_size = 0;
};

/*
 * Parse only payloadType + payloadSize.
 *
 * This function does NOT consume the payload.
 */
[[nodiscard]]
inline bool parse_sei_message_header(std::span<const std::byte> data, SeiMessageHeader& header) {
    header = {};

    /*
     * payloadType
     */
    const auto type = decode_sei_extended_value(data);

    if (!type.complete) {
        return false;
    }

    header.payload_type = type.value;

    std::size_t offset = type.bytes_consumed;

    /*
     * payloadSize
     */
    const auto size = decode_sei_extended_value(data.subspan(offset));

    if (!size.complete) {
        return false;
    }

    header.payload_size = size.value;

    header.header_size = offset + size.bytes_consumed;

    return true;
}

/*
 * -----------------------------------------------------------
 * Zero-copy SEI message creation
 * -----------------------------------------------------------
 */

/*
 * Parse one complete SEI message from an RBSP span.
 *
 * On success:
 *
 *     message.payload
 *
 * points directly into `data`.
 *
 * `consumed` contains the total number of bytes consumed.
 */
[[nodiscard]]
inline bool parse_sei_message(
    std::span<const std::byte> data, SeiMessageView& message, std::size_t& consumed
) {
    message = {};
    consumed = 0;

    SeiMessageHeader header{};

    if (!parse_sei_message_header(data, header)) {
        return false;
    }

    const std::size_t payload_begin = header.header_size;

    const std::size_t payload_size = static_cast<std::size_t>(header.payload_size);

    /*
     * Check before subspan.
     */
    if (payload_begin > data.size()) {
        return false;
    }

    if (payload_size > data.size() - payload_begin) {
        return false;
    }

    message.payload_type = header.payload_type;

    message.payload_size = header.payload_size;

    message.payload = data.subspan(payload_begin, payload_size);

    consumed = payload_begin + payload_size;

    return true;
}

/*
 * -----------------------------------------------------------
 * Parse an entire SEI RBSP
 * -----------------------------------------------------------
 *
 * The final rbsp_trailing_bits() are not treated as another
 * SEI message.
 *
 * Therefore the parser should stop when fewer than one full
 * byte remains or when the remaining bits represent the RBSP
 * trailing pattern.
 *
 * This byte-oriented helper intentionally leaves the precise
 * trailing-bit validation to the RBSP parser.
 */
[[nodiscard]]
inline bool parse_sei_rbsp(
    std::span<const std::byte> rbsp, SeiNalUnitKind nal_kind, SeiRbspView& result
) {
    result = {};
    result.nal_unit_kind = nal_kind;

    std::size_t offset = 0;

    while (offset < rbsp.size()) {
        /*
         * A standalone 0x80 at the end is the common
         * byte-aligned representation of:
         *
         *     rbsp_stop_one_bit
         *     rbsp_alignment_zero_bit[]
         *
         * Don't interpret it as a payloadType.
         *
         * Exact validation belongs in RbspBitstreamReader.
         */
        if (rbsp.size() - offset == 1 && std::to_integer<std::uint8_t>(rbsp[offset]) == 0x80u) {
            break;
        }

        SeiMessageView message{};

        std::size_t consumed = 0;

        if (!parse_sei_message(rbsp.subspan(offset), message, consumed)) {
            return false;
        }

        if (consumed == 0) {
            return false;
        }

        result.messages.push_back(message);

        offset += consumed;
    }

    return true;
}

/*
 * -----------------------------------------------------------
 * Typed payload views
 * -----------------------------------------------------------
 */

/*
 * User data unregistered:
 *
 *     UUID + remaining bytes.
 *
 * Zero-copy payload portion.
 */
[[nodiscard]]
inline bool parse_user_data_unregistered(
    const SeiMessageView& message, UserDataUnregisteredView& result
) {
    if (!message.is_user_data_unregistered()) {
        return false;
    }

    if (message.payload.size() < 16) {
        return false;
    }

    result = {};

    for (std::size_t i = 0; i < 16; ++i) {
        result.uuid[i] = message.payload[i];
    }

    result.user_data_payload = message.payload.subspan(16);

    return true;
}

/*
 * -----------------------------------------------------------
 * Typed fixed-byte payload helpers
 * -----------------------------------------------------------
 */

/*
 * Parse mastering display colour volume.
 *
 * This is deliberately byte-oriented because the payload is
 * byte aligned and contains fixed-width unsigned fields.
 *
 * Big-endian fields:
 *
 *     3 x (x,y) primary coordinates
 *     white point x/y
 *     max luminance
 *     min luminance
 */
[[nodiscard]]
inline bool parse_mastering_display_colour_volume(
    const SeiMessageView& message, MasteringDisplayColourVolume& result
) {
    if (!message.is_mastering_display()) {
        return false;
    }

    /*
     * 3 primaries * 4 bytes
     * + white point 4 bytes
     * + 2 * 4 byte luminance
     *
     * = 24 bytes.
     */
    if (message.payload.size() < 24) {
        return false;
    }

    const auto read_u16 = [&](std::size_t offset) -> std::uint16_t {
        const auto hi = std::to_integer<std::uint8_t>(message.payload[offset]);

        const auto lo = std::to_integer<std::uint8_t>(message.payload[offset + 1]);

        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(hi) << 8) | static_cast<std::uint16_t>(lo)
        );
    };

    const auto read_u32 = [&](std::size_t offset) -> std::uint32_t {
        const auto b0 = std::to_integer<std::uint8_t>(message.payload[offset]);

        const auto b1 = std::to_integer<std::uint8_t>(message.payload[offset + 1]);

        const auto b2 = std::to_integer<std::uint8_t>(message.payload[offset + 2]);

        const auto b3 = std::to_integer<std::uint8_t>(message.payload[offset + 3]);

        return (static_cast<std::uint32_t>(b0) << 24) | (static_cast<std::uint32_t>(b1) << 16) |
               (static_cast<std::uint32_t>(b2) << 8) | static_cast<std::uint32_t>(b3);
    };

    result = {};

    std::size_t offset = 0;

    for (auto& primary : result.display_primaries) {
        primary.x = read_u16(offset);
        offset += 2;

        primary.y = read_u16(offset);
        offset += 2;
    }

    result.white_point.x = read_u16(offset);
    offset += 2;

    result.white_point.y = read_u16(offset);
    offset += 2;

    result.max_display_mastering_luminance = read_u32(offset);
    offset += 4;

    result.min_display_mastering_luminance = read_u32(offset);

    return true;
}

/*
 * Parse Content Light Level Information.
 */
[[nodiscard]]
inline bool parse_content_light_level_info(
    const SeiMessageView& message, ContentLightLevelInfo& result
) {
    if (!message.is_content_light_level()) {
        return false;
    }

    if (message.payload.size() < 4) {
        return false;
    }

    const auto read_u16 = [&](std::size_t offset) -> std::uint16_t {
        const auto hi = std::to_integer<std::uint8_t>(message.payload[offset]);

        const auto lo = std::to_integer<std::uint8_t>(message.payload[offset + 1]);

        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(hi) << 8) | static_cast<std::uint16_t>(lo)
        );
    };

    result.max_content_light_level = read_u16(0);

    result.max_pic_average_light_level = read_u16(2);

    return true;
}

/*
 * -----------------------------------------------------------
 * Message lookup
 * -----------------------------------------------------------
 */

/*
 * Find the first message of a specified payload type.
 */
[[nodiscard]]
inline const SeiMessageView* find_sei_message(
    const SeiRbspView& sei, std::uint32_t payload_type
) noexcept {
    for (const auto& message : sei.messages) {
        if (message.payload_type == payload_type) {
            return &message;
        }
    }

    return nullptr;
}

/*
 * Find all messages of a specified payload type.
 *
 * The caller supplies the output vector.
 *
 * Only descriptors are copied; payload bytes remain
 * zero-copy.
 */
inline void find_sei_messages(
    const SeiRbspView& sei, std::uint32_t payload_type, std::vector<SeiMessageView>& result
) {
    result.clear();

    for (const auto& message : sei.messages) {
        if (message.payload_type == payload_type) {
            result.push_back(message);
        }
    }
}

}  // namespace bs