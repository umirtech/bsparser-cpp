// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "nal_framer.hpp"
#include "hevc_nal_unit_parser.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace bs {

/*
 * -----------------------------------------------------------
 * Top-level HEVC NAL dispatch
 * -----------------------------------------------------------
 *
 * Pipeline:
 *
 *     encoded input
 *          |
 *          v
 *      NalUnit
 *          |
 *          v
 *      NAL type
 *          |
 *    +-----+------+-------+-------+
 *    |     |      |       |       |
 *   VPS   SPS    PPS     SEI    VCL
 *
 * The dispatcher does not own any encoded data.
 *
 * All payloads remain backed by the original std::span.
 */

/*
 * -----------------------------------------------------------
 * Dispatch error
 * -----------------------------------------------------------
 */

class BsNalParseError : public std::runtime_error {
   public:
    explicit BsNalParseError(const char* message) : std::runtime_error(message) {}

    explicit BsNalParseError(const std::string& message) : std::runtime_error(message) {}
};

/*
 * -----------------------------------------------------------
 * Dispatch result
 * -----------------------------------------------------------
 */

enum class NalParseResult : std::uint8_t { Parsed, Ignored, Unsupported };

/*
 * -----------------------------------------------------------
 * Dispatcher callbacks
 * -----------------------------------------------------------
 *
 * A callback receives:
 *
 *     const NalUnit&
 *
 * and therefore has access to:
 *
 *     nal.header
 *     nal.payload_bytes()
 *
 * The callback is responsible for constructing its own
 * RbspReader:
 *
 *     auto reader = make_nal_rbsp_reader(nal);
 *
 * This keeps the dispatcher independent of parser state.
 */

struct BsNalHandlers {
    /*
     * VPS_NUT
     */
    void (*vps)(const NalUnit&) = nullptr;

    /*
     * SPS_NUT
     */
    void (*sps)(const NalUnit&) = nullptr;

    /*
     * PPS_NUT
     */
    void (*pps)(const NalUnit&) = nullptr;

    /*
     * PREFIX_SEI_NUT
     */
    void (*prefix_sei)(const NalUnit&) = nullptr;

    /*
     * SUFFIX_SEI_NUT
     */
    void (*suffix_sei)(const NalUnit&) = nullptr;

    /*
     * VCL NAL units.
     */
    void (*slice)(const NalUnit&) = nullptr;

    /*
     * NAL types which are valid but not currently parsed.
     *
     * Examples:
     *
     *     AUD
     *     EOS
     *     EOB
     *     filler
     */
    void (*unsupported)(const NalUnit&) = nullptr;
};

/*
 * -----------------------------------------------------------
 * Classification
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool is_parameter_set_type(NalUnitType type) noexcept {
    return type == NalUnitType::VPS_NUT || type == NalUnitType::SPS_NUT ||
           type == NalUnitType::PPS_NUT;
}

/*
 * -----------------------------------------------------------
 * Dispatch one already-parsed NAL
 * -----------------------------------------------------------
 */

inline NalParseResult dispatch_nal(const NalUnit& nal, const BsNalHandlers& handlers) {
    if (!validate_nal_unit(nal)) {
        throw BsNalParseError("HEVC NAL dispatcher: invalid NAL header");
    }

    switch (nal.type()) {
            /*
             * -------------------------------------------------------
             * VPS
             * -------------------------------------------------------
             */

        case NalUnitType::VPS_NUT:

            if (handlers.vps == nullptr) {
                return NalParseResult::Unsupported;
            }

            handlers.vps(nal);

            return NalParseResult::Parsed;

            /*
             * -------------------------------------------------------
             * SPS
             * -------------------------------------------------------
             */

        case NalUnitType::SPS_NUT:

            if (handlers.sps == nullptr) {
                return NalParseResult::Unsupported;
            }

            handlers.sps(nal);
            return NalParseResult::Parsed;

            /*
             * -------------------------------------------------------
             * PPS
             * -------------------------------------------------------
             */

        case NalUnitType::PPS_NUT:

            if (handlers.pps == nullptr) {
                return NalParseResult::Unsupported;
            }

            handlers.pps(nal);
            return NalParseResult::Parsed;

            /*
             * -------------------------------------------------------
             * Prefix SEI
             * -------------------------------------------------------
             */

        case NalUnitType::PREFIX_SEI_NUT:

            if (handlers.prefix_sei == nullptr) {
                return NalParseResult::Unsupported;
            }

            handlers.prefix_sei(nal);
            return NalParseResult::Parsed;

            /*
             * -------------------------------------------------------
             * Suffix SEI
             * -------------------------------------------------------
             */

        case NalUnitType::SUFFIX_SEI_NUT:

            if (handlers.suffix_sei == nullptr) {
                return NalParseResult::Unsupported;
            }

            handlers.suffix_sei(nal);
            return NalParseResult::Parsed;

            /*
             * -------------------------------------------------------
             * VCL / slice
             * -------------------------------------------------------
             */

        default:

            if (nal.is_vcl()) {
                if (handlers.slice == nullptr) {
                    return NalParseResult::Unsupported;
                }

                handlers.slice(nal);
                return NalParseResult::Parsed;
            }

            /*
             * Valid HEVC non-VCL NAL which isn't handled yet.
             */
            if (handlers.unsupported != nullptr) {
                handlers.unsupported(nal);
            }

            return NalParseResult::Ignored;
    }
}

/*
 * -----------------------------------------------------------
 * Parse and dispatch one raw NAL span
 * -----------------------------------------------------------
 */

inline NalParseResult parse_and_dispatch_nal(
    std::span<const std::uint8_t> bytes, const BsNalHandlers& handlers
) {
    const NalUnit nal = parse_nal_unit(bytes);

    return dispatch_nal(nal, handlers);
}

/*
 * -----------------------------------------------------------
 * Dispatch a complete framed stream
 * -----------------------------------------------------------
 */

template <typename Framer>
inline std::size_t dispatch_framed_nals(Framer& framer, const BsNalHandlers& handlers) {
    std::size_t parsed_count = 0;

    while (framer.valid()) {
        const auto bytes = framer.nal();

        const auto result = parse_and_dispatch_nal(bytes, handlers);

        if (result == NalParseResult::Parsed) {
            ++parsed_count;
        }

        framer.next();
    }

    return parsed_count;
}

/*
 * -----------------------------------------------------------
 * Annex-B convenience dispatcher
 * -----------------------------------------------------------
 */

inline std::size_t dispatch_annex_b(
    std::span<const std::uint8_t> data, const BsNalHandlers& handlers
) {
    AnnexBNalIterator framer{data};

    return dispatch_framed_nals(framer, handlers);
}

/*
 * -----------------------------------------------------------
 * Length-prefixed convenience dispatcher
 * -----------------------------------------------------------
 */

inline std::size_t dispatch_length_prefixed(
    std::span<const std::uint8_t> data, unsigned length_size, const BsNalHandlers& handlers
) {
    LengthPrefixedNalIterator framer{data, length_size};

    return dispatch_framed_nals(framer, handlers);
}

/*
 * -----------------------------------------------------------
 * Unified dispatcher
 * -----------------------------------------------------------
 */

inline std::size_t dispatch_nals(
    std::span<const std::uint8_t> data,
    NalFramingMode mode,
    const BsNalHandlers& handlers,
    unsigned length_size = 4
) {
    switch (mode) {
        case NalFramingMode::AnnexB:
            return dispatch_annex_b(data, handlers);

        case NalFramingMode::LengthPrefixed:
            return dispatch_length_prefixed(data, length_size, handlers);

        default:
            break;
    }

    throw BsNalParseError("HEVC dispatcher: unsupported framing mode");
}

/*
 * -----------------------------------------------------------
 * Reader-oriented callback helper
 * -----------------------------------------------------------
 *
 * This is useful for wiring the existing syntax parsers
 * without changing them.
 *
 * Example:
 *
 *     BsNalHandlers handlers;
 *
 *     handlers.sps =
 *         [](const NalUnit& nal) {
 *
 *             auto reader =
 *                 make_nal_rbsp_reader(nal);
 *
 *             auto sps =
 *                 parse_sps(reader);
 *
 *             ...
 *         };
 */

template <typename Parser, typename Consumer>
inline void parse_nal_with(const NalUnit& nal, Parser&& parser, Consumer&& consumer) {
    auto reader = make_nal_rbsp_reader(nal);

    auto value = std::forward<Parser>(parser)(reader);

    std::forward<Consumer>(consumer)(std::move(value));
}

/*
 * -----------------------------------------------------------
 * Generic callback factory
 * -----------------------------------------------------------
 *
 * This allows:
 *
 *     auto handlers =
 *         make_hevc_handlers(
 *             parse_vps,
 *             parse_sps,
 *             parse_pps,
 *             parse_prefix_sei,
 *             parse_suffix_sei,
 *             parse_slice);
 *
 * However, because the existing parser functions may have
 * different signatures, explicit lambdas are generally the
 * safer integration point.
 */

}  // namespace bs