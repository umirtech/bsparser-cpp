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
 *   bs::Codec                      select a codec path
 *                                    (Hevc / Avc / Vvc / Av1 / Vp9 / Vp8)
 *   bs::State                      opaque parser state (parameter-set store
 *                                    + per-codec POC tracker)
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
 *   * bs::*ParsedHandlers -- typed callbacks per codec (Hevc / Avc / Vvc /
 *     Av1 / Vp9 / Vp8) that receive the fully-parsed structs as they are
 *     parsed, with all nested sub-structs intact.
 *   * bs::StructReport -- instead of callbacks, a value-copied snapshot of
 *     every parameter set seen during the parse, retrievable afterwards.
 *
 * The user selects the codec path through the bs::Codec enum and supplies an
 * opaque bs::State.  The State auto-manages parameter sets (HEVC/AVC: VPS/
 * SPS/PPS; VVC: DCI/OPI/VPS/SPS/PPS/PH; AV1: the sequence header) and the
 * per-codec POC trackers: as parameter-set NALs are encountered they are
 * parsed and stored internally, so slice handlers can resolve their
 * dependencies through the State instead of maintaining their own managers.
 * Slice / picture / frame headers carry the presentation-order signal
 * natively (derived_poc / order_hint / presentation_order).
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
#include <parser/avc_poc.hpp>
#include <parser/av1_frame_header_parser.hpp>
#include <parser/av1_obu_parser.hpp>
#include <parser/av1_sequence_header_parser.hpp>
#include <parser/hevc_nal_parser.hpp>
#include <parser/hevc_parameter_set_manager.hpp>
#include <parser/hevc_pps_parser.hpp>
#include <parser/hevc_sei_parser.hpp>
#include <parser/hevc_slice_parser.hpp>
#include <parser/hevc_sps_parser.hpp>
#include <parser/hevc_vps_parser.hpp>
#include <parser/hevc_poc.hpp>
#include <parser/ivf_framer.hpp>
#include <parser/nal_framer.hpp>
#include <parser/obu_framer.hpp>
#include <parser/vp8_frame_header_parser.hpp>
#include <parser/vp9_frame_header_parser.hpp>
#include <parser/vvc_dci_parser.hpp>
#include <parser/vvc_opi_parser.hpp>
#include <parser/vvc_nal_unit_parser.hpp>
#include <parser/vvc_parameter_set_manager.hpp>
#include <parser/vvc_ph_parser.hpp>
#include <parser/vvc_pps_parser.hpp>
#include <parser/vvc_slice_parser.hpp>
#include <parser/vvc_sps_parser.hpp>
#include <parser/vvc_vps_parser.hpp>
#include <parser/vvc_poc.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
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
enum class Codec : std::uint8_t { Hevc, Avc, Vvc, Av1, Vp9, Vp8 };

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

/*
 * VVC typed handlers: one callback per parameter-set-like NAL
 * plus slice headers.
 */
struct VvcParsedHandlers {
    void (*dci)(const vvc::Dci&) = nullptr;
    void (*opi)(const vvc::Opi&) = nullptr;
    void (*vps)(const vvc::VideoParameterSet&) = nullptr;
    void (*sps)(const vvc::SequenceParameterSet&) = nullptr;
    void (*pps)(const vvc::PictureParameterSet&) = nullptr;
    void (*ph)(const vvc::PictureHeader&) = nullptr;
    void (*slice)(const vvc::SliceHeader&) = nullptr;
};

/*
 * AV1 typed handlers.
 */
struct Av1ParsedHandlers {
    void (*sequence_header)(const av1::SequenceHeader&) = nullptr;
    void (*frame_header)(const av1::FrameHeader&) = nullptr;
};

/*
 * VP9 / VP8 typed handlers (frame headers only).
 */
struct Vp9ParsedHandlers {
    void (*frame_header)(const vp9::FrameHeader&) = nullptr;
};

struct Vp8ParsedHandlers {
    void (*frame_header)(const vp8::FrameHeader&) = nullptr;
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
    explicit StateImpl(Codec codec) : codec_(codec) {
        switch (codec) {
            case Codec::Hevc:
                hevc_ = std::make_unique<ParameterSetManager>();
                break;

            case Codec::Avc:
                avc_ = std::make_unique<avc::ParameterSetManager>();
                break;

            case Codec::Vvc:
                vvc_ = std::make_unique<vvc::ParameterSetManager>();
                break;

            case Codec::Av1:
            case Codec::Vp9:
            case Codec::Vp8:
                break;
        }
    }

    [[nodiscard]]
    Codec codec() const noexcept {
        return codec_;
    }

    /*
     * Drop every stored parameter set.  Used when one State is
     * reused across independent streams to avoid stale-ID
     * collisions between them.
     */
    void clear() noexcept {
        switch (codec_) {
            case Codec::Hevc:
                hevc_->clear();
                hevc_poc_.reset();
                break;

            case Codec::Avc:
                avc_->clear();
                avc_poc_.reset();
                break;

            case Codec::Vvc:
                vvc_->clear();
                vvc_poc_.reset();
                break;

            case Codec::Av1:
            case Codec::Vp9:
            case Codec::Vp8:
                break;
        }
    }

    [[nodiscard]]
    ParameterSetManager& hevc() noexcept {
        return *hevc_;
    }

    [[nodiscard]]
    avc::ParameterSetManager& avc() noexcept {
        return *avc_;
    }

    [[nodiscard]]
    vvc::ParameterSetManager& vvc() noexcept {
        return *vvc_;
    }

    [[nodiscard]]
    HevcPocState& hevc_poc() noexcept {
        return hevc_poc_;
    }

    [[nodiscard]]
    avc::PocState& avc_poc() noexcept {
        return avc_poc_;
    }

    [[nodiscard]]
    vvc::PocState& vvc_poc() noexcept {
        return vvc_poc_;
    }

    /*
     * Last-seen AV1 sequence header (provides OrderHintBits and the
     * screen-content / integer-MV selection for frame headers).
     */
    [[nodiscard]]
    av1::SequenceHeader& av1_seq() noexcept {
        return av1_seq_;
    }

   private:
    Codec codec_;

    std::unique_ptr<ParameterSetManager> hevc_{};

    std::unique_ptr<avc::ParameterSetManager> avc_{};

    std::unique_ptr<vvc::ParameterSetManager> vvc_{};

    /*
     * Per-codec POC trackers (H.265 §8.3.1 / H.264 §8.2.1 / H.266 §8.3.1).
     * Held here so POC state survives across chunked bs::parse() calls on
     * the same State.
     */
    HevcPocState hevc_poc_{};

    avc::PocState avc_poc_{};

    vvc::PocState vvc_poc_{};

    av1::SequenceHeader av1_seq_{};
};

/*
 * Convert a span of bytes-like storage into a span of std::byte, which is
 * what the RBSP reader consumes.
 */
[[nodiscard]]
inline std::span<const std::byte> to_byte_span(std::span<const std::uint8_t> data) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size());
}

/*
 * Parse + store one HEVC parameter-set NAL into the manager.
 *
 * Failures are swallowed: a malformed parameter set must not abort the
 * whole stream dispatch, and the user's handler is still free to attempt its
 * own parse.
 */
inline void store_hevc_parameter_set(
    ParameterSetManager& manager,
    const NalUnit& nal,
    const HevcParsedHandlers& ph = {},
    StructReport* report = nullptr
) {
    try {
        switch (nal.type()) {
            case NalUnitType::VPS_NUT: {
                RbspBitstreamReader reader(to_byte_span(nal.payload_bytes()));
                VideoParameterSet vps = parse_video_parameter_set(reader);
                (void)manager.store_vps(vps);
                if (ph.vps)
                    ph.vps(vps);
                if (report)
                    report->hevc_vps.push_back(vps);
                break;
            }

            case NalUnitType::SPS_NUT: {
                RbspBitstreamReader reader(to_byte_span(nal.payload_bytes()));
                SequenceParameterSet sps = parse_sequence_parameter_set(reader);
                (void)manager.store_sps(sps);
                if (ph.sps)
                    ph.sps(sps);
                if (report)
                    report->hevc_sps.push_back(sps);
                break;
            }

            case NalUnitType::PPS_NUT: {
                RbspBitstreamReader reader(to_byte_span(nal.payload_bytes()));
                PictureParameterSet pps = parse_picture_parameter_set(reader);
                (void)manager.store_pps(pps);
                if (ph.pps)
                    ph.pps(pps);
                if (report)
                    report->hevc_pps.push_back(pps);
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
inline void store_avc_parameter_set(
    avc::ParameterSetManager& manager,
    const avc::NalUnit& nal,
    const AvcParsedHandlers& ph = {},
    StructReport* report = nullptr
) {
    try {
        switch (nal.type()) {
            case avc::NalUnitType::Sps: {
                RbspBitstreamReader reader(to_byte_span(nal.payload_bytes()));
                avc::SequenceParameterSet sps = avc::parse_sequence_parameter_set(reader);
                (void)manager.store_sps(sps);
                if (ph.sps)
                    ph.sps(sps);
                if (report)
                    report->avc_sps.push_back(sps);
                break;
            }

            case avc::NalUnitType::Pps: {
                RbspBitstreamReader reader(to_byte_span(nal.payload_bytes()));
                avc::PictureParameterSet pps = avc::parse_picture_parameter_set(reader);
                (void)manager.store_pps(pps);
                if (ph.pps)
                    ph.pps(pps);
                if (report)
                    report->avc_pps.push_back(pps);
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
inline std::size_t dispatch_state_hevc(
    StateImpl& impl,
    Framer& framer,
    const BsNalHandlers& handlers,
    const HevcParsedHandlers& ph = {},
    StructReport* report = nullptr
) {
    std::size_t parsed_count = 0;

    ParameterSetManager& manager = impl.hevc();

    while (framer.valid()) {
        const auto bytes = framer.nal();

        try {
            NalUnit nal = parse_nal_unit(bytes);

            store_hevc_parameter_set(manager, nal, ph, report);

            if (ph.sei) {
                const auto t = nal.type();
                if (t == NalUnitType::PREFIX_SEI_NUT || t == NalUnitType::SUFFIX_SEI_NUT) {
                    try {
                        ParsedSei sei = parse_sei_nal(nal);
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
                    const std::uint8_t nut = static_cast<std::uint8_t>(nal.nal_type());

                    RbspReader r1(nal.payload_bytes());
                    (void)r1.read_bit();
                    if (is_irap_nal_unit(nut)) {
                        (void)r1.read_bit();
                    }
                    const std::uint32_t pps_id = r1.read_ue();

                    const auto resolved = manager.resolve_pps(static_cast<std::uint8_t>(pps_id));

                    if (resolved.pps != nullptr && resolved.sps != nullptr) {
                        RbspReader r2(nal.payload_bytes());
                        SliceSegmentHeader hdr = parse_slice_segment_header(
                            r2, *resolved.sps, *resolved.pps, nut, nal.temporal_id()
                        );
                        /*
                         * Derive the presentation-order POC (H.265 §8.3.1)
                         * from the slice POC LSB and refresh NumPocTotalCurr,
                         * which is built relative to the picture POC.
                         */
                        hdr.derived_poc = impl.hevc_poc().derive(
                            nut,
                            nal.temporal_id(),
                            hdr.slice_pic_order_cnt_lsb,
                            resolved.sps->max_pic_order_cnt_lsb()
                        );
                        derive_num_poc_total_curr(*resolved.sps, hdr);
                        ph.slice(hdr);
                    }
                } catch (...) {
                    /* malformed slice: skip */
                }
            }

            if (dispatch_nal(nal, handlers) == NalParseResult::Parsed) {
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
inline std::size_t dispatch_state_avc(
    StateImpl& impl,
    Framer& framer,
    const avc::NalHandlers& handlers,
    const AvcParsedHandlers& ph = {},
    StructReport* report = nullptr
) {
    std::size_t parsed_count = 0;

    avc::ParameterSetManager& manager = impl.avc();

    while (framer.valid()) {
        const auto bytes = framer.nal();

        try {
            avc::NalUnit nal = avc::parse_nal_unit(bytes);

            store_avc_parameter_set(manager, nal, ph, report);

            if (ph.sei) {
                if (nal.type() == avc::NalUnitType::Sei) {
                    try {
                        avc::ParsedSei sei = avc::parse_sei_nal(nal);
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
                if (t == avc::NalUnitType::SliceNonIdr || t == avc::NalUnitType::SliceIdr) {
                    try {
                        RbspReader r1(nal.payload_bytes());
                        (void)r1.read_ue();
                        (void)r1.read_ue();
                        const std::uint32_t pps_id = r1.read_ue();

                        const auto resolved = manager.resolve(static_cast<std::uint8_t>(pps_id));

                        if (resolved.pps != nullptr && resolved.sps != nullptr) {
                            RbspReader r2(nal.payload_bytes());
                            avc::SliceHeader hdr = avc::parse_slice_header(
                                r2,
                                *resolved.sps,
                                *resolved.pps,
                                t,
                                static_cast<std::uint8_t>(nal.header.nal_ref_idc)
                            );
                            /*
                             * Derive the presentation-order POC (H.264 §8.2.1).
                             */
                            hdr.derived_poc = impl.avc_poc().derive(
                                hdr,
                                *resolved.sps,
                                t == avc::NalUnitType::SliceIdr,
                                static_cast<std::uint8_t>(nal.header.nal_ref_idc)
                            );
                            ph.slice(hdr);
                        }
                    } catch (...) {
                        /* malformed slice: skip */
                    }
                }
            }

            if (avc::dispatch_nal(nal, handlers) == avc::NalParseResult::Parsed) {
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
 * VVC framing + dispatch with automatic parameter-set storage.
 */
template <typename Framer>
inline std::size_t dispatch_state_vvc(
    StateImpl& impl, Framer& framer, const VvcParsedHandlers& ph
) {
    std::size_t parsed_count = 0;

    vvc::ParameterSetManager& manager = impl.vvc();

    while (framer.valid()) {
        const auto bytes = framer.nal();

        try {
            vvc::NalUnit nal = vvc::parse_nal_unit(bytes);

            switch (nal.type()) {
                case vvc::NalUnitType::OpiNut: {
                    RbspReader r(nal.payload_bytes());
                    vvc::Opi opi = vvc::parse_opi(r);
                    manager.store_opi(opi);
                    if (ph.opi)
                        ph.opi(opi);
                    break;
                }

                case vvc::NalUnitType::DciNut: {
                    RbspReader r(nal.payload_bytes());
                    vvc::Dci dci = vvc::parse_dci(r);
                    manager.store_dci(dci);
                    if (ph.dci)
                        ph.dci(dci);
                    break;
                }

                case vvc::NalUnitType::VpsNut: {
                    RbspReader r(nal.payload_bytes());
                    vvc::VideoParameterSet vps = vvc::parse_vps(r);
                    manager.store_vps(vps);
                    if (ph.vps)
                        ph.vps(vps);
                    break;
                }

                case vvc::NalUnitType::SpsNut: {
                    RbspReader r(nal.payload_bytes());
                    vvc::SequenceParameterSet sps = vvc::parse_sps(r);
                    manager.store_sps(sps);
                    if (ph.sps)
                        ph.sps(sps);
                    break;
                }

                case vvc::NalUnitType::PpsNut: {
                    RbspReader r(nal.payload_bytes());
                    vvc::PictureParameterSet pps = vvc::parse_pps(r);
                    manager.store_pps(pps);
                    if (ph.pps)
                        ph.pps(pps);
                    break;
                }

                case vvc::NalUnitType::PhNut: {
                    /*
                     * The POC LSB width comes from the SPS referenced by the
                     * PH's pps_id, which is decoded inside the header.  Two
                     * passes: read pps_id, resolve the SPS, then re-read the
                     * full PH with the SPS-derived POC configuration.
                     */
                    RbspReader r1(nal.payload_bytes());
                    vvc::PictureHeader lead = vvc::parse_ph(r1);

                    const auto resolved = manager.resolve(static_cast<std::uint8_t>(lead.pps_id));

                    if (resolved.sps != nullptr) {
                        RbspReader r2(nal.payload_bytes());
                        vvc::PictureHeader phdr = vvc::parse_ph(r2, resolved.sps, resolved.pps);
                        manager.store_ph(phdr);
                        if (ph.ph)
                            ph.ph(phdr);
                    } else {
                        manager.store_ph(lead);
                        if (ph.ph)
                            ph.ph(lead);
                    }
                    break;
                }

                default:
                    break;
            }

            if (ph.slice && nal.is_vcl()) {
                try {
                    /*
                     * The picture header may be embedded in the slice header
                     * (sh_picture_header_in_slice_header_flag), in which case
                     * the POC LSB width needs the SPS referenced by the
                     * embedded PH's pps_id.  Two passes like the HEVC/AVC
                     * slices: read pps_id, resolve the SPS, re-parse.
                     */
                    RbspReader r1(nal.payload_bytes());
                    vvc::SliceHeader lead = vvc::parse_slice_header(
                        r1, nullptr, nullptr, nullptr, static_cast<int>(nal.nal_type())
                    );

                    const vvc::PictureHeader* stored_ph = manager.ph();

                    const std::uint32_t pps_id =
                        lead.picture_header_in_slice_header_flag
                            ? static_cast<std::uint32_t>(lead.pps_id)
                            : (stored_ph != nullptr ? stored_ph->pps_id : 0u);

                    const auto resolved = manager.resolve(static_cast<std::uint8_t>(pps_id));

                    if (resolved.sps != nullptr) {
                        RbspReader r2(nal.payload_bytes());
                        vvc::SliceHeader hdr = vvc::parse_slice_header(
                            r2,
                            resolved.sps,
                            resolved.pps,
                            stored_ph,
                            static_cast<int>(nal.nal_type())
                        );

                        if (!hdr.picture_header_in_slice_header_flag && stored_ph != nullptr) {
                            hdr.ph = *stored_ph;
                        }

                        /*
                         * Derive the presentation-order POC (H.266 §8.3.1)
                         * from the picture header and this slice's NAL type.
                         */
                        if (hdr.ph.poc_lsb_bits != 0) {
                            hdr.derived_poc =
                                impl.vvc_poc().derive(nal.nal_type(), nal.temporal_id(), hdr.ph);
                        }
                        ph.slice(hdr);
                    }
                } catch (...) {
                    /* malformed slice: skip */
                }
            }

            ++parsed_count;

        } catch (...) {
            /* skip a NAL that cannot even be framed/parsed */
        }

        framer.next();
    }

    return parsed_count;
}

/*
 * AV1 OBU framing + dispatch.
 */
template <typename Framer>
inline std::size_t dispatch_state_av1(
    StateImpl& impl, Framer& framer, const Av1ParsedHandlers& ph
) {
    std::size_t parsed_count = 0;
    std::size_t frame_count = 0;

    while (framer.valid()) {
        const auto bytes = framer.obu();

        try {
            av1::Obu obu = av1::parse_obu(bytes);

            switch (static_cast<int>(obu.type())) {
                case static_cast<int>(av1::ObuType::SequenceHeader): {
                    av1::SequenceHeader sh = av1::parse_sequence_header(obu.payload_bytes());
                    impl.av1_seq() = sh;
                    if (ph.sequence_header) {
                        ph.sequence_header(sh);
                    }
                    break;
                }

                case static_cast<int>(av1::ObuType::FrameHeader):
                case static_cast<int>(av1::ObuType::RedundantFrameHeader):
                case static_cast<int>(av1::ObuType::Frame): {
                    if (ph.frame_header) {
                        av1::FrameHeader fh =
                            av1::parse_frame_header(obu.payload_bytes(), impl.av1_seq());
                        fh.presentation_order = static_cast<std::int32_t>(frame_count);
                        ++frame_count;
                        ph.frame_header(fh);
                    }
                    break;
                }

                default:
                    break;
            }

            ++parsed_count;

        } catch (...) {
            /* skip a malformed OBU */
        }

        framer.next();
    }

    return parsed_count;
}

/*
 * VP9 / VP8 IVF framing + dispatch.
 */
template <typename Framer, typename Header>
inline std::size_t dispatch_state_vp_frame(
    Framer& framer, Header (*parse)(std::span<const std::uint8_t>), void (*handler)(const Header&)
) {
    std::size_t parsed_count = 0;

    while (framer.valid()) {
        const auto bytes = framer.frame();

        try {
            Header header = parse(bytes);

            /*
             * No POC in VP8/VP9: the display order of a raw stream is the
             * decode order, exposed as presentation_order.
             */
            header.presentation_order = static_cast<std::int32_t>(parsed_count);

            if (handler != nullptr) {
                handler(header);
            }

            ++parsed_count;

        } catch (...) {
            /* skip a malformed frame */
        }

        framer.next();
    }

    return parsed_count;
}

}  // namespace detail

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
    explicit State(Codec codec) : impl_(std::make_unique<detail::StateImpl>(codec)) {}

    State(State&&) noexcept = default;

    State& operator=(State&&) noexcept = default;

    ~State() = default;

    /*
     * Which codec path this state was created for.
     */
    [[nodiscard]]
    Codec codec() const noexcept {
        return impl_->codec();
    }

    /*
     * Drop all stored parameter sets.  Call this before parsing
     * a new, independent stream into the same State so that
     * parameter-set IDs from the previous stream cannot collide
     * with (or shadow) those of the new one.
     */
    void clear() noexcept {
        impl_->clear();
    }

    /*
     * HEVC parameter-set manager, or nullptr when this is an AVC state.
     */
    [[nodiscard]]
    ParameterSetManager* hevc_sets() noexcept {
        return impl_->codec() == Codec::Hevc ? &impl_->hevc() : nullptr;
    }

    [[nodiscard]]
    const ParameterSetManager* hevc_sets() const noexcept {
        return impl_->codec() == Codec::Hevc ? &impl_->hevc() : nullptr;
    }

    /*
     * AVC parameter-set manager, or nullptr when this is a HEVC state.
     */
    [[nodiscard]]
    avc::ParameterSetManager* avc_sets() noexcept {
        return impl_->codec() == Codec::Avc ? &impl_->avc() : nullptr;
    }

    [[nodiscard]]
    const avc::ParameterSetManager* avc_sets() const noexcept {
        return impl_->codec() == Codec::Avc ? &impl_->avc() : nullptr;
    }

    /*
     * VVC parameter-set manager, or nullptr when this is not a
     * VVC state.
     */
    [[nodiscard]]
    vvc::ParameterSetManager* vvc_sets() noexcept {
        return impl_->codec() == Codec::Vvc ? &impl_->vvc() : nullptr;
    }

    [[nodiscard]]
    const vvc::ParameterSetManager* vvc_sets() const noexcept {
        return impl_->codec() == Codec::Vvc ? &impl_->vvc() : nullptr;
    }

    /*
     * Per-codec POC trackers (H.265 §8.3.1 / H.264 §8.2.1 / H.266 §8.3.1).
     * These are updated automatically by the unified parse() dispatcher; they
     * are exposed so callers can also drive the derivation themselves (e.g.
     * CLI report builders that re-parse slice headers).
     */
    [[nodiscard]]
    HevcPocState* hevc_poc() noexcept {
        return impl_->codec() == Codec::Hevc ? &impl_->hevc_poc() : nullptr;
    }

    [[nodiscard]]
    avc::PocState* avc_poc() noexcept {
        return impl_->codec() == Codec::Avc ? &impl_->avc_poc() : nullptr;
    }

    [[nodiscard]]
    vvc::PocState* vvc_poc() noexcept {
        return impl_->codec() == Codec::Vvc ? &impl_->vvc_poc() : nullptr;
    }

   private:
    std::unique_ptr<detail::StateImpl> impl_;

    /*
     * Allow the unified parse() overloads to reach the implementation.
     */
    friend std::size_t parse(
        State&, std::span<const std::uint8_t>, NalFramingMode, const BsNalHandlers&, unsigned
    );

    friend std::size_t parse(
        State&, std::span<const std::uint8_t>, NalFramingMode, const avc::NalHandlers&, unsigned
    );

    friend std::size_t parse(
        State&, std::span<const std::uint8_t>, NalFramingMode, const HevcParsedHandlers&, unsigned
    );

    friend std::size_t parse(
        State&, std::span<const std::uint8_t>, NalFramingMode, const AvcParsedHandlers&, unsigned
    );

    friend std::size_t parse(
        State&, std::span<const std::uint8_t>, NalFramingMode, StructReport&, unsigned
    );

    friend std::size_t parse(
        State&, std::span<const std::uint8_t>, NalFramingMode, const VvcParsedHandlers&, unsigned
    );

    friend std::size_t parse(
        State&, std::span<const std::uint8_t>, NalFramingMode, const Av1ParsedHandlers&, unsigned
    );

    friend std::size_t parse(
        State&, std::span<const std::uint8_t>, NalFramingMode, const Vp9ParsedHandlers&, unsigned
    );

    friend std::size_t parse(
        State&, std::span<const std::uint8_t>, NalFramingMode, const Vp8ParsedHandlers&, unsigned
    );
};

/*
 * ---------------------------------------------------------------------------
 * State factory
 * ---------------------------------------------------------------------------
 */
[[nodiscard]]
inline std::unique_ptr<State> create_state(Codec codec) {
    return std::make_unique<State>(codec);
}

/*
 * ===========================================================================
 * Unified parse — HEVC
 * ===========================================================================
 */
[[nodiscard]]
inline std::size_t parse(
    State& state,
    std::span<const std::uint8_t> data,
    NalFramingMode mode,
    const BsNalHandlers& handlers,
    unsigned length_size = 4
) {
    if (state.codec() != Codec::Hevc) {
        throw BsNalParseError("bs::parse: state is not an HEVC state");
    }

    switch (mode) {
        case NalFramingMode::AnnexB: {
            AnnexBNalIterator framer{data};
            return detail::dispatch_state_hevc(*state.impl_, framer, handlers);
        }

        case NalFramingMode::LengthPrefixed: {
            LengthPrefixedNalIterator framer{data, length_size};
            return detail::dispatch_state_hevc(*state.impl_, framer, handlers);
        }

        default:
            break;
    }

    throw BsNalParseError("bs::parse: unsupported framing mode");
}

/*
 * ===========================================================================
 * Unified parse — AVC
 * ===========================================================================
 */
[[nodiscard]]
inline std::size_t parse(
    State& state,
    std::span<const std::uint8_t> data,
    NalFramingMode mode,
    const avc::NalHandlers& handlers,
    unsigned length_size = 4
) {
    if (state.codec() != Codec::Avc) {
        throw avc::NalParseError("bs::parse: state is not an AVC state");
    }

    switch (mode) {
        case NalFramingMode::AnnexB: {
            AnnexBNalIterator framer{data};
            return detail::dispatch_state_avc(*state.impl_, framer, handlers);
        }

        case NalFramingMode::LengthPrefixed: {
            LengthPrefixedNalIterator framer{data, length_size};
            return detail::dispatch_state_avc(*state.impl_, framer, handlers);
        }

        default:
            break;
    }

    throw avc::NalParseError("bs::parse: unsupported framing mode");
}

/*
 * ===========================================================================
 * Unified parse — typed parameter-set handlers (HEVC)
 * ===========================================================================
 */
[[nodiscard]]
inline std::size_t parse(
    State& state,
    std::span<const std::uint8_t> data,
    NalFramingMode mode,
    const HevcParsedHandlers& handlers,
    unsigned length_size = 4
) {
    if (state.codec() != Codec::Hevc) {
        throw BsNalParseError("bs::parse: state is not an HEVC state");
    }

    BsNalHandlers raw{};
    switch (mode) {
        case NalFramingMode::AnnexB: {
            AnnexBNalIterator framer{data};
            return detail::dispatch_state_hevc(*state.impl_, framer, raw, handlers);
        }

        case NalFramingMode::LengthPrefixed: {
            LengthPrefixedNalIterator framer{data, length_size};
            return detail::dispatch_state_hevc(*state.impl_, framer, raw, handlers);
        }

        default:
            break;
    }

    throw BsNalParseError("bs::parse: unsupported framing mode");
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
inline std::size_t parse(
    State& state,
    std::span<const std::uint8_t> data,
    NalFramingMode mode,
    StructReport& report,
    unsigned length_size = 4
) {
    switch (mode) {
        case NalFramingMode::AnnexB: {
            AnnexBNalIterator framer{data};
            if (state.codec() == Codec::Hevc) {
                return detail::dispatch_state_hevc(
                    *state.impl_, framer, BsNalHandlers{}, HevcParsedHandlers{}, &report
                );
            }
            return detail::dispatch_state_avc(
                *state.impl_, framer, avc::NalHandlers{}, AvcParsedHandlers{}, &report
            );
        }

        case NalFramingMode::LengthPrefixed: {
            LengthPrefixedNalIterator framer{data, length_size};
            if (state.codec() == Codec::Hevc) {
                return detail::dispatch_state_hevc(
                    *state.impl_, framer, BsNalHandlers{}, HevcParsedHandlers{}, &report
                );
            }
            return detail::dispatch_state_avc(
                *state.impl_, framer, avc::NalHandlers{}, AvcParsedHandlers{}, &report
            );
        }

        default:
            break;
    }

    if (state.codec() == Codec::Hevc) {
        throw BsNalParseError("bs::parse: unsupported framing mode");
    }
    throw avc::NalParseError("bs::parse: unsupported framing mode");
}

/*
 * ===========================================================================
 * Unified parse — typed parameter-set handlers (AVC)
 * ===========================================================================
 */
[[nodiscard]]
inline std::size_t parse(
    State& state,
    std::span<const std::uint8_t> data,
    NalFramingMode mode,
    const AvcParsedHandlers& handlers,
    unsigned length_size = 4
) {
    if (state.codec() != Codec::Avc) {
        throw avc::NalParseError("bs::parse: state is not an AVC state");
    }

    avc::NalHandlers raw{};
    switch (mode) {
        case NalFramingMode::AnnexB: {
            AnnexBNalIterator framer{data};
            return detail::dispatch_state_avc(*state.impl_, framer, raw, handlers);
        }

        case NalFramingMode::LengthPrefixed: {
            LengthPrefixedNalIterator framer{data, length_size};
            return detail::dispatch_state_avc(*state.impl_, framer, raw, handlers);
        }

        default:
            break;
    }

    throw avc::NalParseError("bs::parse: unsupported framing mode");
}

/*
 * ===========================================================================
 * Unified parse — VVC
 * ===========================================================================
 */
[[nodiscard]]
inline std::size_t parse(
    State& state,
    std::span<const std::uint8_t> data,
    NalFramingMode mode,
    const VvcParsedHandlers& handlers,
    unsigned length_size = 4
) {
    if (state.codec() != Codec::Vvc) {
        throw BsNalParseError("bs::parse: state is not a VVC state");
    }

    switch (mode) {
        case NalFramingMode::AnnexB: {
            AnnexBNalIterator framer{data};
            return detail::dispatch_state_vvc(*state.impl_, framer, handlers);
        }

        case NalFramingMode::LengthPrefixed: {
            LengthPrefixedNalIterator framer{data, length_size};
            return detail::dispatch_state_vvc(*state.impl_, framer, handlers);
        }

        default:
            break;
    }

    throw BsNalParseError("bs::parse: unsupported framing mode");
}

/*
 * ===========================================================================
 * Unified parse — AV1
 * ===========================================================================
 */
[[nodiscard]]
inline std::size_t parse(
    State& state,
    std::span<const std::uint8_t> data,
    NalFramingMode mode,
    const Av1ParsedHandlers& handlers,
    unsigned length_size = 4
) {
    (void)length_size;

    if (state.codec() != Codec::Av1) {
        throw BsNalParseError("bs::parse: state is not an AV1 state");
    }

    switch (mode) {
        case NalFramingMode::Obu: {
            av1::ObuFramer framer{data};
            return detail::dispatch_state_av1(*state.impl_, framer, handlers);
        }

        default:
            break;
    }

    throw BsNalParseError("bs::parse: unsupported framing mode");
}

/*
 * ===========================================================================
 * Unified parse — VP9
 * ===========================================================================
 */
[[nodiscard]]
inline std::size_t parse(
    State& state,
    std::span<const std::uint8_t> data,
    NalFramingMode mode,
    const Vp9ParsedHandlers& handlers,
    unsigned length_size = 4
) {
    (void)length_size;

    if (state.codec() != Codec::Vp9) {
        throw BsNalParseError("bs::parse: state is not a VP9 state");
    }

    switch (mode) {
        case NalFramingMode::Ivf: {
            IvfFramer framer{data};
            return detail::dispatch_state_vp_frame(
                framer, vp9::parse_frame_header, handlers.frame_header
            );
        }

        default:
            break;
    }

    throw BsNalParseError("bs::parse: unsupported framing mode");
}

/*
 * ===========================================================================
 * Unified parse — VP8
 * ===========================================================================
 */
[[nodiscard]]
inline std::size_t parse(
    State& state,
    std::span<const std::uint8_t> data,
    NalFramingMode mode,
    const Vp8ParsedHandlers& handlers,
    unsigned length_size = 4
) {
    (void)length_size;

    if (state.codec() != Codec::Vp8) {
        throw BsNalParseError("bs::parse: state is not a VP8 state");
    }

    switch (mode) {
        case NalFramingMode::Ivf: {
            IvfFramer framer{data};
            return detail::dispatch_state_vp_frame(
                framer, vp8::parse_frame_header, handlers.frame_header
            );
        }

        default:
            break;
    }

    throw BsNalParseError("bs::parse: unsupported framing mode");
}

}  // namespace bs
