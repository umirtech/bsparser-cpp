/*
 * ---------------------------------------------------------------------------
 * HEVC parser fuzz target
 * ---------------------------------------------------------------------------
 *
 * This file contains the shared fuzz entry point:
 *
 *     LLVMFuzzerTestOneInput
 *
 * It is compiled two ways:
 *
 *   Clang + libFuzzer (primary):
 *       -fsanitize=fuzzer,address,undefined
 *       (libFuzzer provides main(); see the bs_fuzz CMake target)
 *
 *   Any compiler + standalone driver (secondary, e.g. GCC):
 *       link with fuzz_driver.cpp, which reads corpus files from
 *       argv and forwards each one here.
 *
 * The harness treats the input as an Annex-B HEVC stream and drives every
 * syntax parser (VPS, SPS, PPS, SEI, slice header) through the shared
 * ParameterSetManager so realistic slice parsing is exercised.
 *
 * A second pass treats the same bytes as a single raw NAL unit so that
 * inputs without start codes still reach the bit-level parsers.
 *
 * Malformed input is expected to be rejected by throwing; those exceptions
 * are caught here so fuzzing continues.  Memory-safety bugs surface through
 * ASan/UBSan regardless of exception handling.
 */

#include "nal_framer.hpp"
#include "hevc_nal_unit_parser.hpp"
#include "hevc_parameter_set_manager.hpp"
#include "hevc_pps_parser.hpp"
#include "rbsp_bitstream_reader.hpp"
#include "hevc_sei_parser.hpp"
#include "hevc_slice_parser.hpp"
#include "hevc_sps_parser.hpp"
#include "hevc_vps_parser.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace {

[[nodiscard]]
std::span<const std::byte> as_bytes(std::span<const std::uint8_t> input) noexcept {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(input.data()), input.size()
    );
}

/*
 * Per-input parser state.
 */
struct FuzzState {
    bs::ParameterSetManager parameter_sets;

    /*
     * Track which PPS slots are populated so slice parsing only
     * retries against PPS that actually exist in this input.
     */
    std::array<bool, bs::kMaxPpsCount> pps_seen{};
};

void parse_slice(FuzzState& state, const bs::NalUnit& nal) {
    const auto payload = nal.payload_bytes();

    const auto span = as_bytes(payload);

    const auto nal_type = static_cast<std::uint8_t>(nal.nal_type());

    const auto temporal_id = nal.header.temporal_id();

    /*
     * The slice header encodes its PPS id inside the header, so we try
     * every PPS known from this input.  Bounded by the number of stored
     * PPS (at most kMaxPpsCount); mismatches reject quickly by throwing.
     */
    for (std::size_t pps_id = 0; pps_id < state.pps_seen.size(); ++pps_id) {
        if (!state.pps_seen[pps_id]) {
            continue;
        }

        const auto* pps = state.parameter_sets.find_pps(static_cast<std::uint8_t>(pps_id));

        if (pps == nullptr) {
            continue;
        }

        const auto* sps = state.parameter_sets.find_sps(pps->pps_seq_parameter_set_id);

        if (sps == nullptr) {
            continue;
        }

        /*
         * Fresh reader per attempt: the header parse is replayable.
         */
        bs::RbspBitstreamReader reader(span);

        (void)bs::parse_slice_segment_header(reader, *sps, *pps, nal_type, temporal_id);
    }
}

void handle_nal(FuzzState& state, const bs::NalUnit& nal) {
    const auto payload = nal.payload_bytes();

    const auto span = as_bytes(payload);

    switch (nal.type()) {
        case bs::NalUnitType::VPS_NUT: {
            bs::RbspBitstreamReader reader(span);
            auto vps = bs::parse_video_parameter_set(reader);
            (void)state.parameter_sets.store_vps(std::move(vps));
            break;
        }

        case bs::NalUnitType::SPS_NUT: {
            bs::RbspBitstreamReader reader(span);
            auto sps = bs::parse_sequence_parameter_set(reader);
            (void)state.parameter_sets.store_sps(std::move(sps));
            break;
        }

        case bs::NalUnitType::PPS_NUT: {
            bs::RbspBitstreamReader reader(span);
            auto pps = bs::parse_picture_parameter_set(reader);
            const auto id = pps.pps_pic_parameter_set_id;
            if (state.parameter_sets.store_pps(std::move(pps))) {
                state.pps_seen[static_cast<std::size_t>(id)] = true;
            }
            break;
        }

        case bs::NalUnitType::PREFIX_SEI_NUT:
        case bs::NalUnitType::SUFFIX_SEI_NUT: {
            std::vector<std::byte> storage;
            bs::SeiRbspView view;
            (void)bs::parse_sei_nal(nal, storage, view);
            break;
        }

        default:
            if (nal.is_vcl()) {
                parse_slice(state, nal);
            }
            break;
    }
}

void run_input(std::span<const std::uint8_t> input) {
    /*
     * Pass 1: Annex-B framing with full parameter-set state.
     */
    {
        FuzzState state;

        bs::AnnexBNalIterator framer{input};

        while (framer.valid()) {
            const auto bytes = framer.nal();

            try {
                const bs::NalUnit nal = bs::parse_nal_unit(bytes);

                handle_nal(state, nal);
            } catch (...) {
                /* malformed NAL: skip it and keep going */
            }

            framer.next();
        }
    }

    /*
     * Pass 2: treat the entire input as one raw NAL unit.
     *
     * This reaches the bit-level parsers even when the input contains
     * no Annex-B start codes at all.
     */
    {
        FuzzState state;

        bs::NalUnit nal;

        if (bs::try_parse_nal_unit(input, nal)) {
            try {
                handle_nal(state, nal);
            } catch (...) {
                /* malformed payload: skip */
            }
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);

    try {
        run_input(input);
    } catch (...) {
        /*
         * Rejection is signaled with exceptions throughout this parser.
         * Swallow them so a malformed input is not treated as a crash.
         */
    }

    return 0;
}