#pragma once

/*
 * ===========================================================================
 * bsparser — unified public-facing header
 * ===========================================================================
 *
 * This single header aggregates the whole library and exposes one unified
 * API on top of the existing per-codec internals.
 *
 * The library is header-only: every implementation lives in headers and is
 * instantiated at the call site.  There is no translation unit to compile or
 * link against.
 *
 * Unified entry points
 * ---------------------
 *
 *   bs::Codec                      select a codec path (Hevc / Avc)
 *   bs::State                      opaque parser state
 *   bs::create_state(Codec)        construct an opaque state
 *   bs::parse(state, data, mode,
 *             handlers[, length_size])
 *                                  frame + parse a stream
 *
 * The unified parse entry point comes in three flavours that share the same
 * framing/dispatch core:
 *
 *   * bs::BsNalHandlers / avc::NalHandlers -- the original raw-NAL callback
 *     (receives each NAL unit as a bs::NalUnit / avc::NalUnit).
 *   * bs::HevcParsedHandlers / bs::AvcParsedHandlers -- typed callbacks that
 *     receive the fully-parsed parameter-set structs (VPS/SPS/PPS) as they
 *     are parsed, with all nested sub-structs intact.
 *   * bs::StructReport -- instead of callbacks, a value-copied snapshot of
 *     every parameter set seen during the parse, retrievable afterwards.
 *
 * The user selects the codec path through the bs::Codec enum and supplies an
 * opaque bs::State.  The State auto-manages parameter sets (SPS/PPS, and VPS
 * for HEVC): as parameter-set NALs are encountered they are parsed and stored
 * internally, so slice handlers can resolve their dependencies through the
 * State instead of maintaining their own managers.
 *
 * The previous per-codec API (bs::dispatch_nals / bs::avc::dispatch_nals and
 * the explicit syntax parsers) remains fully available and unchanged.
 */

#include <bitstream/rbsp_bitstream_reader.hpp>
#include <bitstream/rbsp_reader.hpp>
#include <logging/log.hpp>
#include <parser/avc_nal_parser.hpp>
#include <parser/avc_parameter_set_manager.hpp>
#include <parser/avc_pps_parser.hpp>
#include <parser/avc_sei_parser.hpp>
#include <parser/avc_slice_parser.hpp>
#include <parser/avc_sps_parser.hpp>
#include <parser/hevc_nal_parser.hpp>
#include <parser/hevc_parameter_set_manager.hpp>
#include <parser/hevc_pps_parser.hpp>
#include <parser/hevc_sei_parser.hpp>
#include <parser/hevc_slice_parser.hpp>
#include <parser/hevc_sps_parser.hpp>
#include <parser/hevc_vps_parser.hpp>
#include <parser/nal_framer.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>


namespace bs {

/*
 * ===========================================================================
 * Codec selection
 * ===========================================================================
 *
 * The user decides which codec path the unified parser takes by passing one
 * of these values when constructing the State and again when calling
 * bs::parse (the handler type already disambiguates the codec; the enum keeps
 * the selection explicit and lets the library validate it).
 */
enum class Codec : std::uint8_t {
    Hevc,
    Avc
};


/*
 * ===========================================================================
 * Typed access to fully-parsed parameter-set structs
 * ===========================================================================
 *
 * The unified parser already parses every VPS/SPS/PPS NAL into its rich,
 * nested C++ struct (see the syntax headers).  By default those structs are
 * only stored internally for slice/SEI resolution, but they can also be
 * handed to the caller two ways:
 *
 *   * bs::HevcParsedHandlers / bs::AvcParsedHandlers -- a set of function
 *                            pointers, one per parsed parameter-set type,
 *                            invoked as each NAL is parsed.  Mirrors the C
 *                            API's typed callbacks.
 *   * bs::StructReport    -- a collected, value-copied snapshot of every
 *                            parameter set seen during a parse, retrievable
 *                            after the call instead of via callbacks.
 *
 * Both are optional and NULL-safe: passing an empty handler set (or omitting
 * it) leaves the original behaviour untouched.
 */
struct HevcParsedHandlers {
    void (*vps)(const VideoParameterSet&) = nullptr;
    void (*sps)(const SequenceParameterSet&) = nullptr;
    void (*pps)(const PictureParameterSet&) = nullptr;
    void (*sei)(const ParsedSei&) = nullptr;
    void (*slice)(const SliceSegmentHeader&) = nullptr;
};


struct AvcParsedHandlers {
    void (*sps)(const avc::SequenceParameterSet&) = nullptr;
    void (*pps)(const avc::PictureParameterSet&) = nullptr;
    void (*sei)(const avc::ParsedSei&) = nullptr;
    void (*slice)(const avc::SliceHeader&) = nullptr;
};


struct StructReport {
    std::vector<VideoParameterSet> hevc_vps;
    std::vector<SequenceParameterSet> hevc_sps;
    std::vector<PictureParameterSet> hevc_pps;
    std::vector<avc::SequenceParameterSet> avc_sps;
    std::vector<avc::PictureParameterSet> avc_pps;
};


namespace detail {

/*
 * ---------------------------------------------------------------------------
 * Opaque state implementation
 * ---------------------------------------------------------------------------
 *
 * This is the only place where codec-specific state is held.  It is defined
 * here (rather than in a .cpp) so the library stays header-only, but it is
 * deliberately hidden behind bs::State: consumers never name StateImpl and
 * never see the per-codec parameter-set managers directly.
 */
class StateImpl {
public:

    explicit StateImpl(Codec codec)
        : codec_(codec)
    {
        if (codec == Codec::Hevc) {
            hevc_ =
                std::make_unique<ParameterSetManager>();
        } else {
            avc_ =
                std::make_unique<avc::ParameterSetManager>();
        }
    }


    [[nodiscard]]
    Codec codec() const noexcept
    {
        return codec_;
    }


    /*
     * Drop every stored parameter set.  Used when one State is
     * reused across independent streams to avoid stale-ID
     * collisions between them.
     */
    void clear() noexcept
    {
        if (codec_ == Codec::Hevc) {
            hevc_->clear();
        } else {
            avc_->clear();
        }
    }


    [[nodiscard]]
    ParameterSetManager& hevc() noexcept
    {
        return *hevc_;
    }


    [[nodiscard]]
    avc::ParameterSetManager& avc() noexcept
    {
        return *avc_;
    }


private:

    Codec codec_;

    std::unique_ptr<ParameterSetManager> hevc_{};

    std::unique_ptr<avc::ParameterSetManager> avc_{};
};


/*
 * Convert a span of bytes-like storage into a span of std::byte, which is
 * what the RBSP reader consumes.
 */
[[nodiscard]]
inline std::span<const std::byte>
to_byte_span(
    std::span<const std::uint8_t> data) noexcept
{
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(data.data()),
        data.size());
}


/*
 * Parse + store one HEVC parameter-set NAL into the manager.
 *
 * Failures are swallowed: a malformed parameter set must not abort the
 * whole stream dispatch, and the user's handler is still free to attempt its
 * own parse.
 */
inline void
store_hevc_parameter_set(
    ParameterSetManager& manager,
    const NalUnit& nal,
    const HevcParsedHandlers& ph = {},
    StructReport* report = nullptr)
{
    try {

        switch (nal.type()) {

        case NalUnitType::VPS_NUT: {
            RbspBitstreamReader reader(
                to_byte_span(nal.payload_bytes()));
            VideoParameterSet vps =
                parse_video_parameter_set(reader);
            (void)manager.store_vps(vps);
            if (ph.vps) ph.vps(vps);
            if (report) report->hevc_vps.push_back(vps);
            break;
        }

        case NalUnitType::SPS_NUT: {
            RbspBitstreamReader reader(
                to_byte_span(nal.payload_bytes()));
            SequenceParameterSet sps =
                parse_sequence_parameter_set(reader);
            (void)manager.store_sps(sps);
            if (ph.sps) ph.sps(sps);
            if (report) report->hevc_sps.push_back(sps);
            break;
        }

        case NalUnitType::PPS_NUT: {
            RbspBitstreamReader reader(
                to_byte_span(nal.payload_bytes()));
            PictureParameterSet pps =
                parse_picture_parameter_set(reader);
            (void)manager.store_pps(pps);
            if (ph.pps) ph.pps(pps);
            if (report) report->hevc_pps.push_back(pps);
            break;
        }

        default:
            break;
        }

    } catch (...) {
        /* leave the manager untouched on parse failure */
    }
}


/*
 * Parse + store one AVC parameter-set NAL into the manager.
 */
inline void
store_avc_parameter_set(
    avc::ParameterSetManager& manager,
    const avc::NalUnit& nal,
    const AvcParsedHandlers& ph = {},
    StructReport* report = nullptr)
{
    try {

        switch (nal.type()) {

        case avc::NalUnitType::Sps: {
            RbspBitstreamReader reader(
                to_byte_span(nal.payload_bytes()));
            avc::SequenceParameterSet sps =
                avc::parse_sequence_parameter_set(reader);
            (void)manager.store_sps(sps);
            if (ph.sps) ph.sps(sps);
            if (report) report->avc_sps.push_back(sps);
            break;
        }

        case avc::NalUnitType::Pps: {
            RbspBitstreamReader reader(
                to_byte_span(nal.payload_bytes()));
            avc::PictureParameterSet pps =
                avc::parse_picture_parameter_set(reader);
            (void)manager.store_pps(pps);
            if (ph.pps) ph.pps(pps);
            if (report) report->avc_pps.push_back(pps);
            break;
        }

        default:
            break;
        }

    } catch (...) {
        /* leave the manager untouched on parse failure */
    }
}


/*
 * HEVC framing + dispatch with automatic parameter-set storage.
 */
template <typename Framer>
inline std::size_t
dispatch_state_hevc(
    StateImpl& impl,
    Framer& framer,
    const BsNalHandlers& handlers,
    const HevcParsedHandlers& ph = {},
    StructReport* report = nullptr)
{
    std::size_t parsed_count = 0;

    ParameterSetManager& manager =
        impl.hevc();

    while (framer.valid()) {

        const auto bytes =
            framer.nal();

        try {

            NalUnit nal =
                parse_nal_unit(bytes);

            store_hevc_parameter_set(
                manager,
                nal,
                ph,
                report);

            if (ph.sei) {
                const auto t = nal.type();
                if (t == NalUnitType::PREFIX_SEI_NUT ||
                    t == NalUnitType::SUFFIX_SEI_NUT) {
                    try {
                        ParsedSei sei =
                            parse_sei_nal(nal);
                        ph.sei(sei);
                    } catch (...) {
                        /* malformed SEI: skip */
                    }
                }
            }

            /*
             * Slice headers require the SPS/PPS resolved via the
             * slice's pps_id before they can be parsed.  Because
             * pps_id is encoded inside the header itself, do a
             * two-pass parse: read just the pps_id from the front
             * of the RBSP, resolve it, then re-parse the full
             * header with the resolved parameter sets.
             */
            if (ph.slice && nal.is_vcl()) {
                try {
                    const std::uint8_t nut =
                        static_cast<std::uint8_t>(
                            nal.nal_type());

                    RbspReader r1(nal.payload_bytes());
                    (void)r1.read_bit();
                    if (is_irap_nal_unit(nut)) {
                        (void)r1.read_bit();
                    }
                    const std::uint32_t pps_id =
                        r1.read_ue();

                    const auto resolved =
                        manager.resolve_pps(
                            static_cast<std::uint8_t>(
                                pps_id));

                    if (resolved.pps != nullptr &&
                        resolved.sps != nullptr) {
                        RbspReader r2(nal.payload_bytes());
                        SliceSegmentHeader hdr =
                            parse_slice_segment_header(
                                r2,
                                *resolved.sps,
                                *resolved.pps,
                                nut,
                                nal.temporal_id());
                        ph.slice(hdr);
                    }
                } catch (...) {
                    /* malformed slice: skip */
                }
            }

            if (dispatch_nal(
                    nal,
                    handlers) ==
                NalParseResult::Parsed) {

                ++parsed_count;
            }

        } catch (...) {
            /* skip a NAL that cannot even be framed/parsed */
        }

        framer.next();
    }

    return parsed_count;
}


/*
 * AVC framing + dispatch with automatic parameter-set storage.
 */
template <typename Framer>
inline std::size_t
dispatch_state_avc(
    StateImpl& impl,
    Framer& framer,
    const avc::NalHandlers& handlers,
    const AvcParsedHandlers& ph = {},
    StructReport* report = nullptr)
{
    std::size_t parsed_count = 0;

    avc::ParameterSetManager& manager =
        impl.avc();

    while (framer.valid()) {

        const auto bytes =
            framer.nal();

        try {

            avc::NalUnit nal =
                avc::parse_nal_unit(bytes);

            store_avc_parameter_set(
                manager,
                nal,
                ph,
                report);

            if (ph.sei) {
                if (nal.type() == avc::NalUnitType::Sei) {
                    try {
                        avc::ParsedSei sei =
                            avc::parse_sei_nal(nal);
                        ph.sei(sei);
                    } catch (...) {
                        /* malformed SEI: skip */
                    }
                }
            }

            /*
             * AVC slice headers are two-pass as well: read the
             * pps_id from the front of the RBSP, resolve it, then
             * re-parse the full header.
             */
            if (ph.slice) {
                const auto t = nal.type();
                if (t == avc::NalUnitType::SliceNonIdr ||
                    t == avc::NalUnitType::SliceIdr) {
                    try {
                        RbspReader r1(nal.payload_bytes());
                        (void)r1.read_ue();
                        (void)r1.read_ue();
                        const std::uint32_t pps_id =
                            r1.read_ue();

                        const auto resolved =
                            manager.resolve(
                                static_cast<std::uint8_t>(
                                    pps_id));

                        if (resolved.pps != nullptr &&
                            resolved.sps != nullptr) {
                            RbspReader r2(nal.payload_bytes());
                            avc::SliceHeader hdr =
                                avc::parse_slice_header(
                                    r2,
                                    *resolved.sps,
                                    *resolved.pps,
                                    t,
                                    static_cast<std::uint8_t>(
                                        nal.header
                                            .nal_ref_idc));
                            ph.slice(hdr);
                        }
                    } catch (...) {
                        /* malformed slice: skip */
                    }
                }
            }

            if (avc::dispatch_nal(
                    nal,
                    handlers) ==
                avc::NalParseResult::Parsed) {

                ++parsed_count;
            }

        } catch (...) {
            /* skip a NAL that cannot even be framed/parsed */
        }

        framer.next();
    }

    return parsed_count;
}

} // namespace detail


/*
 * ===========================================================================
 * Opaque State
 * ===========================================================================
 *
 * The user owns a State for the lifetime of a parsing session but never
 * inspects its contents directly.  Parameter sets are retrieved through the
 * typed accessors below, which return nullptr when the State was created for
 * the other codec.
 */
class State {
public:

    explicit State(Codec codec)
        : impl_(std::make_unique<detail::StateImpl>(codec))
    {
    }


    State(State&&) noexcept = default;

    State& operator=(State&&) noexcept = default;

    ~State() = default;


    /*
     * Which codec path this state was created for.
     */
    [[nodiscard]]
    Codec codec() const noexcept
    {
        return impl_->codec();
    }


    /*
     * Drop all stored parameter sets.  Call this before parsing
     * a new, independent stream into the same State so that
     * parameter-set IDs from the previous stream cannot collide
     * with (or shadow) those of the new one.
     */
    void clear() noexcept
    {
        impl_->clear();
    }


    /*
     * HEVC parameter-set manager, or nullptr when this is an AVC state.
     */
    [[nodiscard]]
    ParameterSetManager* hevc_sets() noexcept
    {
        return
            impl_->codec() == Codec::Hevc
                ? &impl_->hevc()
                : nullptr;
    }


    [[nodiscard]]
    const ParameterSetManager* hevc_sets() const noexcept
    {
        return
            impl_->codec() == Codec::Hevc
                ? &impl_->hevc()
                : nullptr;
    }


    /*
     * AVC parameter-set manager, or nullptr when this is a HEVC state.
     */
    [[nodiscard]]
    avc::ParameterSetManager* avc_sets() noexcept
    {
        return
            impl_->codec() == Codec::Avc
                ? &impl_->avc()
                : nullptr;
    }


    [[nodiscard]]
    const avc::ParameterSetManager* avc_sets() const noexcept
    {
        return
            impl_->codec() == Codec::Avc
                ? &impl_->avc()
                : nullptr;
    }


private:

    std::unique_ptr<detail::StateImpl> impl_;


    /*
     * Allow the unified parse() overloads to reach the implementation.
     */
    friend std::size_t
    parse(
        State&,
        std::span<const std::uint8_t>,
        NalFramingMode,
        const BsNalHandlers&,
        unsigned);

    friend std::size_t
    parse(
        State&,
        std::span<const std::uint8_t>,
        NalFramingMode,
        const avc::NalHandlers&,
        unsigned);

    friend std::size_t
    parse(
        State&,
        std::span<const std::uint8_t>,
        NalFramingMode,
        const HevcParsedHandlers&,
        unsigned);

    friend std::size_t
    parse(
        State&,
        std::span<const std::uint8_t>,
        NalFramingMode,
        const AvcParsedHandlers&,
        unsigned);

    friend std::size_t
    parse(
        State&,
        std::span<const std::uint8_t>,
        NalFramingMode,
        StructReport&,
        unsigned);
};


/*
 * ---------------------------------------------------------------------------
 * State factory
 * ---------------------------------------------------------------------------
 */
[[nodiscard]]
inline std::unique_ptr<State>
create_state(
    Codec codec)
{
    return std::make_unique<State>(codec);
}


/*
 * ===========================================================================
 * Unified parse — HEVC
 * ===========================================================================
 */
[[nodiscard]]
inline std::size_t
parse(
    State& state,
    std::span<const std::uint8_t> data,
    NalFramingMode mode,
    const BsNalHandlers& handlers,
    unsigned length_size = 4)
{
    if (state.codec() != Codec::Hevc) {
        throw BsNalParseError(
            "bs::parse: state is not an HEVC state");
    }

    switch (mode) {

    case NalFramingMode::AnnexB: {
        AnnexBNalIterator framer{data};
        return detail::dispatch_state_hevc(
            *state.impl_,
            framer,
            handlers);
    }

    case NalFramingMode::LengthPrefixed: {
        LengthPrefixedNalIterator framer{
            data,
            length_size
        };
        return detail::dispatch_state_hevc(
            *state.impl_,
            framer,
            handlers);
    }
    }

    throw BsNalParseError(
        "bs::parse: unsupported framing mode");
}


/*
 * ===========================================================================
 * Unified parse — AVC
 * ===========================================================================
 */
[[nodiscard]]
inline std::size_t
parse(
    State& state,
    std::span<const std::uint8_t> data,
    NalFramingMode mode,
    const avc::NalHandlers& handlers,
    unsigned length_size = 4)
{
    if (state.codec() != Codec::Avc) {
        throw avc::NalParseError(
            "bs::parse: state is not an AVC state");
    }

    switch (mode) {

    case NalFramingMode::AnnexB: {
        AnnexBNalIterator framer{data};
        return detail::dispatch_state_avc(
            *state.impl_,
            framer,
            handlers);
    }

    case NalFramingMode::LengthPrefixed: {
        LengthPrefixedNalIterator framer{
            data,
            length_size
        };
        return detail::dispatch_state_avc(
            *state.impl_,
            framer,
            handlers);
    }
    }

    throw avc::NalParseError(
        "bs::parse: unsupported framing mode");
}


/*
 * ===========================================================================
 * Unified parse — typed parameter-set handlers (HEVC)
 * ===========================================================================
 */
[[nodiscard]]
inline std::size_t
parse(
    State& state,
    std::span<const std::uint8_t> data,
    NalFramingMode mode,
    const HevcParsedHandlers& handlers,
    unsigned length_size = 4)
{
    if (state.codec() != Codec::Hevc) {
        throw BsNalParseError(
            "bs::parse: state is not an HEVC state");
    }

    BsNalHandlers raw{};
    switch (mode) {

    case NalFramingMode::AnnexB: {
        AnnexBNalIterator framer{data};
        return detail::dispatch_state_hevc(
            *state.impl_,
            framer,
            raw,
            handlers);
    }

    case NalFramingMode::LengthPrefixed: {
        LengthPrefixedNalIterator framer{
            data,
            length_size
        };
        return detail::dispatch_state_hevc(
            *state.impl_,
            framer,
            raw,
            handlers);
    }
    }

    throw BsNalParseError(
        "bs::parse: unsupported framing mode");
}


/*
 * ===========================================================================
 * Unified parse — collected parameter-set report
 * ===========================================================================
 *
 * Fills `report` with a value-copied snapshot of every parameter set seen
 * during the parse.  Works for either codec; the State's codec selects the
 * dispatch path.
 */
[[nodiscard]]
inline std::size_t
parse(
    State& state,
    std::span<const std::uint8_t> data,
    NalFramingMode mode,
    StructReport& report,
    unsigned length_size = 4)
{
    switch (mode) {

    case NalFramingMode::AnnexB: {
        AnnexBNalIterator framer{data};
        if (state.codec() == Codec::Hevc) {
            return detail::dispatch_state_hevc(
                *state.impl_,
                framer,
                BsNalHandlers{},
                HevcParsedHandlers{},
                &report);
        }
        return detail::dispatch_state_avc(
            *state.impl_,
            framer,
            avc::NalHandlers{},
            AvcParsedHandlers{},
            &report);
    }

    case NalFramingMode::LengthPrefixed: {
        LengthPrefixedNalIterator framer{
            data,
            length_size
        };
        if (state.codec() == Codec::Hevc) {
            return detail::dispatch_state_hevc(
                *state.impl_,
                framer,
                BsNalHandlers{},
                HevcParsedHandlers{},
                &report);
        }
        return detail::dispatch_state_avc(
            *state.impl_,
            framer,
            avc::NalHandlers{},
            AvcParsedHandlers{},
            &report);
    }
    }

    if (state.codec() == Codec::Hevc) {
        throw BsNalParseError(
            "bs::parse: unsupported framing mode");
    }
    throw avc::NalParseError(
        "bs::parse: unsupported framing mode");
}


/*
 * ===========================================================================
 * Unified parse — typed parameter-set handlers (AVC)
 * ===========================================================================
 */
[[nodiscard]]
inline std::size_t
parse(
    State& state,
    std::span<const std::uint8_t> data,
    NalFramingMode mode,
    const AvcParsedHandlers& handlers,
    unsigned length_size = 4)
{
    if (state.codec() != Codec::Avc) {
        throw avc::NalParseError(
            "bs::parse: state is not an AVC state");
    }

    avc::NalHandlers raw{};
    switch (mode) {

    case NalFramingMode::AnnexB: {
        AnnexBNalIterator framer{data};
        return detail::dispatch_state_avc(
            *state.impl_,
            framer,
            raw,
            handlers);
    }

    case NalFramingMode::LengthPrefixed: {
        LengthPrefixedNalIterator framer{
            data,
            length_size
        };
        return detail::dispatch_state_avc(
            *state.impl_,
            framer,
            raw,
            handlers);
    }
    }

    throw avc::NalParseError(
        "bs::parse: unsupported framing mode");
}

} // namespace bs
