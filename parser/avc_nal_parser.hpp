#pragma once

#include "avc_nal_unit_parser.hpp"
#include "nal_framer.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace bs {
namespace avc {

/*
 * -----------------------------------------------------------
 * Top-level AVC NAL dispatch
 * -----------------------------------------------------------
 *
 * Reuses the codec-agnostic framing from bs (Annex-B and
 * length-prefixed iterators) and routes each NAL by its
 * 1-byte AVC header.
 */

class NalParseError : public std::runtime_error {
   public:
    explicit NalParseError(const char* message) : std::runtime_error(message) {}

    explicit NalParseError(const std::string& message) : std::runtime_error(message) {}
};

enum class NalParseResult : std::uint8_t { Parsed, Ignored, Unsupported };

struct NalHandlers {
    void (*sps)(const NalUnit&) = nullptr;

    void (*pps)(const NalUnit&) = nullptr;

    void (*sei)(const NalUnit&) = nullptr;

    void (*slice)(const NalUnit&) = nullptr;

    void (*unsupported)(const NalUnit&) = nullptr;
};

/*
 * -----------------------------------------------------------
 * Dispatch one already-parsed NAL
 * -----------------------------------------------------------
 */

inline NalParseResult dispatch_nal(const NalUnit& nal, const NalHandlers& handlers) {
    switch (nal.type()) {
        case NalUnitType::Sps:

            if (handlers.sps == nullptr) {
                return NalParseResult::Unsupported;
            }

            handlers.sps(nal);
            return NalParseResult::Parsed;

        case NalUnitType::Pps:

            if (handlers.pps == nullptr) {
                return NalParseResult::Unsupported;
            }

            handlers.pps(nal);
            return NalParseResult::Parsed;

        case NalUnitType::Sei:

            if (handlers.sei == nullptr) {
                return NalParseResult::Unsupported;
            }

            handlers.sei(nal);
            return NalParseResult::Parsed;

        default:

            if (nal.is_vcl()) {
                if (handlers.slice == nullptr) {
                    return NalParseResult::Unsupported;
                }

                handlers.slice(nal);
                return NalParseResult::Parsed;
            }

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
    std::span<const std::uint8_t> bytes, const NalHandlers& handlers
) {
    return dispatch_nal(parse_nal_unit(bytes), handlers);
}

/*
 * -----------------------------------------------------------
 * Dispatch a complete framed stream
 * -----------------------------------------------------------
 */

template <typename Framer>
inline std::size_t dispatch_framed_nals(Framer& framer, const NalHandlers& handlers) {
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

inline std::size_t dispatch_annex_b(
    std::span<const std::uint8_t> data, const NalHandlers& handlers
) {
    AnnexBNalIterator framer{data};

    return dispatch_framed_nals(framer, handlers);
}

inline std::size_t dispatch_length_prefixed(
    std::span<const std::uint8_t> data, unsigned length_size, const NalHandlers& handlers
) {
    LengthPrefixedNalIterator framer{data, length_size};

    return dispatch_framed_nals(framer, handlers);
}

inline std::size_t dispatch_nals(
    std::span<const std::uint8_t> data,
    NalFramingMode mode,
    const NalHandlers& handlers,
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

    throw NalParseError("AVC dispatcher: unsupported framing mode");
}

}  // namespace avc
}  // namespace bs