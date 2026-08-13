/*
 * ===========================================================================
 * bsparser — C public API implementation
 * ===========================================================================
 *
 * Bridges the C ABI to the C++20 unified API (bs::State / bs::parse).  The
 * core remains header-only; this is the single translation unit that gets
 * compiled into the static/shared library, giving downstream C / older-
 * toolchain consumers a stable, versionable ABI.  Structured data is exchanged
 * through plain C structs (no JSON / serialisation).
 */

#include "bs_capi.h"

#include <bsparser.hpp>

#include <capi/bs_structs_conv.hpp>
#include <capi/bs_structs_free.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>


using bs::capi::bs_conv;
using bs::capi::bs_free_BsHevcVideoParameterSet;
using bs::capi::bs_free_BsHevcSequenceParameterSet;
using bs::capi::bs_free_BsHevcPictureParameterSet;
using bs::capi::bs_free_BsHevcSliceSegmentHeader;
using bs::capi::bs_free_BsAvcSequenceParameterSet;
using bs::capi::bs_free_BsAvcPictureParameterSet;
using bs::capi::bs_free_BsAvcSliceHeader;


namespace {

/*
 * Thread-local bridge state.  bs::parse dispatches through plain function
 * pointers (no capture), so the C callback set is stashed here for the
 * duration of one synchronous bs_parse() call.  Thread-local => reentrant.
 */
thread_local const BsNalHandlers* g_handlers = nullptr;
thread_local const unsigned char* g_data_start = nullptr;

/*
 * Report collection state, used by bs_parse_report().
 */
thread_local std::vector<BsNalEntry> g_entries;
thread_local bs::Codec g_collector_codec = bs::Codec::Hevc;


[[nodiscard]]
inline bs::Codec
to_codec(
    BsCodec c)
{
    return static_cast<bs::Codec>(
        static_cast<int>(c) & 0x01); // AUTO collapses to HEVC here; the real
                                     // auto-probe is in bs_parse_report.
}


[[nodiscard]]
inline bs::NalFramingMode
to_mode(
    BsFramingMode m)
{
    return (m == BS_FRAMING_LENGTH_PREFIXED)
               ? bs::NalFramingMode::LengthPrefixed
               : bs::NalFramingMode::AnnexB;
}


/*
 * Probe the first NAL to decide HEVC vs AVC (mirrors cli auto-detect).
 */
[[nodiscard]]
inline bs::Codec
detect_codec(
    const unsigned char* data,
    std::size_t size)
{
    std::size_t i = 0;

    if (size >= 4 &&
        data[0] == 0x00 && data[1] == 0x00 &&
        data[2] == 0x00 && data[3] == 0x01) {
        i = 4;
    } else if (size >= 3 &&
               data[0] == 0x00 && data[1] == 0x00 &&
               data[2] == 0x01) {
        i = 3;
    }

    if (i >= size) {
        return bs::Codec::Hevc;
    }

    const unsigned hevc_type =
        (static_cast<unsigned>(data[i]) >> 1) & 0x3F;
    const unsigned avc_type =
        static_cast<unsigned>(data[i]) & 0x1F;

    if (hevc_type == 32 || hevc_type == 33 ||
        hevc_type == 34) {
        return bs::Codec::Hevc;
    }

    if (avc_type >= 1 && avc_type <= 21) {
        return bs::Codec::Avc;
    }

    return bs::Codec::Hevc;
}


/*
 * Static NAL type names (returned by BsNalEntry.nal_type_name).
 */
[[nodiscard]]
const char* hevc_type_name(
    int t)
{
    switch (t) {
    case 32: return "VPS_NUT";
    case 33: return "SPS_NUT";
    case 34: return "PPS_NUT";
    case 35: return "PREFIX_SEI_NUT";
    case 36: return "SUFFIX_SEI_NUT";
    case 37: return "AUD_NUT";
    case 38: return "EOS_NUT";
    case 39: return "EOB_NUT";
    case 40: return "FD_NUT";
    default:
        if (t >= 0 && t <= 31) {
            return "VCL";
        }
        return "NAL";
    }
}


[[nodiscard]]
const char* avc_type_name(
    int t)
{
    switch (t) {
    case 1:  return "SliceNonIdr";
    case 2:  return "SliceDataPartitionA";
    case 3:  return "SliceDataPartitionB";
    case 4:  return "SliceDataPartitionC";
    case 5:  return "SliceIdr";
    case 6:  return "SEI";
    case 7:  return "SPS";
    case 8:  return "PPS";
    case 9:  return "AUD";
    case 10: return "EndOfSequence";
    case 11: return "EndOfStream";
    case 12: return "FillerData";
    case 13: return "SpsExtension";
    case 14: return "PrefixNal";
    case 15: return "SubsetSps";
    case 19: return "AuxCodedPicture";
    case 20: return "SliceSvcExtension";
    case 21: return "SliceMvcExtension";
    case 22: return "SliceAvc3dExtension";
    default:
        if ((t >= 1 && t <= 5) || t == 19 || t == 20 ||
            t == 21 || t == 22) {
            return "VCL";
        }
        return "NAL";
    }
}


/*
 * Last-error string (per thread).
 */
thread_local std::string g_last_error;

inline void
set_error(
    const char* msg)
{
    g_last_error = msg ? msg : "unknown error";
}


/*
 * Fill a BsNalUnit from a HEVC NalUnit view.  payload points into the
 * caller's buffer; offset is its position within that buffer.
 */
inline void
fill_hevc(
    const bs::NalUnit& nal,
    BsNalUnit& out,
    const unsigned char* base)
{
    auto p = nal.payload_bytes();
    out.nal_unit_type =
        static_cast<int>(
            static_cast<unsigned>(nal.type()));
    out.nuh_layer_id = nal.layer_id();
    out.nuh_temporal_id_plus1 = nal.temporal_id();
    out.forbidden_zero_bit =
        nal.header.forbidden_zero_bit ? 1 : 0;
    out.is_vcl = nal.is_vcl() ? 1 : 0;
    out.payload = p.data();
    out.payload_size = p.size();
    out.offset =
        static_cast<size_t>(
            p.data() - base);
}


/*
 * Fill a BsNalUnit from an AVC NalUnit view.
 */
inline void
fill_avc(
    const bs::avc::NalUnit& nal,
    BsNalUnit& out,
    const unsigned char* base)
{
    auto p = nal.payload_bytes();
    out.nal_unit_type =
        static_cast<int>(
            static_cast<unsigned>(nal.type()));
    out.nuh_layer_id = 0;
    out.nuh_temporal_id_plus1 = 0;
    out.forbidden_zero_bit =
        nal.header.forbidden_zero_bit ? 1 : 0;
    out.is_vcl = nal.is_vcl() ? 1 : 0;
    out.payload = p.data();
    out.payload_size = p.size();
    out.offset =
        static_cast<size_t>(
            p.data() - base);
}


/*
 * HEVC adaptors: build a BsNalUnit view and forward to the user callback.
 */
void hevc_vps(const bs::NalUnit& nal)
{
    const BsNalHandlers* h = g_handlers;
    if (h && h->vps) {
        BsNalUnit n{};
        fill_hevc(nal, n, g_data_start);
        h->vps(h->ctx, &n);
    }
}

void hevc_sps(const bs::NalUnit& nal)
{
    const BsNalHandlers* h = g_handlers;
    if (h && h->sps) {
        BsNalUnit n{};
        fill_hevc(nal, n, g_data_start);
        h->sps(h->ctx, &n);
    }
}

void hevc_pps(const bs::NalUnit& nal)
{
    const BsNalHandlers* h = g_handlers;
    if (h && h->pps) {
        BsNalUnit n{};
        fill_hevc(nal, n, g_data_start);
        h->pps(h->ctx, &n);
    }
}

void hevc_sei(const bs::NalUnit& nal)
{
    const BsNalHandlers* h = g_handlers;
    if (h && h->sei) {
        BsNalUnit n{};
        fill_hevc(nal, n, g_data_start);
        h->sei(h->ctx, &n);
    }
}

void hevc_slice(const bs::NalUnit& nal)
{
    const BsNalHandlers* h = g_handlers;
    if (h && h->slice) {
        BsNalUnit n{};
        fill_hevc(nal, n, g_data_start);
        h->slice(h->ctx, &n);
    }
}

void hevc_unsupported(const bs::NalUnit& nal)
{
    const BsNalHandlers* h = g_handlers;
    if (h && h->unsupported) {
        BsNalUnit n{};
        fill_hevc(nal, n, g_data_start);
        h->unsupported(h->ctx, &n);
    }
}


/*
 * AVC adaptors.  (AVC has no VPS, so there is no avc_vps adaptor.)
 */
void avc_sps(const bs::avc::NalUnit& nal)
{
    const BsNalHandlers* h = g_handlers;
    if (h && h->sps) {
        BsNalUnit n{};
        fill_avc(nal, n, g_data_start);
        h->sps(h->ctx, &n);
    }
}

void avc_pps(const bs::avc::NalUnit& nal)
{
    const BsNalHandlers* h = g_handlers;
    if (h && h->pps) {
        BsNalUnit n{};
        fill_avc(nal, n, g_data_start);
        h->pps(h->ctx, &n);
    }
}

void avc_sei(const bs::avc::NalUnit& nal)
{
    const BsNalHandlers* h = g_handlers;
    if (h && h->sei) {
        BsNalUnit n{};
        fill_avc(nal, n, g_data_start);
        h->sei(h->ctx, &n);
    }
}

void avc_slice(const bs::avc::NalUnit& nal)
{
    const BsNalHandlers* h = g_handlers;
    if (h && h->slice) {
        BsNalUnit n{};
        fill_avc(nal, n, g_data_start);
        h->slice(h->ctx, &n);
    }
}

void avc_unsupported(const bs::avc::NalUnit& nal)
{
    const BsNalHandlers* h = g_handlers;
    if (h && h->unsupported) {
        BsNalUnit n{};
        fill_avc(nal, n, g_data_start);
        h->unsupported(h->ctx, &n);
    }
}


/*
 * Single collector callback used by bs_parse_report() for every NAL type.
 */
void collect(
    void*,
    const BsNalUnit* n)
{
    BsNalEntry e{};
    e.index = g_entries.size();
    e.offset = n->offset;
    e.nal_unit_type = n->nal_unit_type;
    e.is_vcl = n->is_vcl;
    e.size = n->payload_size;
    e.nal_type_name =
        (g_collector_codec == bs::Codec::Hevc)
            ? hevc_type_name(n->nal_unit_type)
            : avc_type_name(n->nal_unit_type);
    g_entries.push_back(e);
}


/*
 * Typed parameter-set delivery.  The C++ HevcParsedHandlers / AvcParsedHandlers
 * dispatch through plain function pointers (no capture), so the C typed handler
 * set and the report collector live in thread-local state for the duration of
 * one synchronous call.  Reentrant across threads, like the raw-NAL path.
 */
thread_local const BsHevcHandlers* g_hevc_h = nullptr;
thread_local const BsAvcHandlers* g_avc_h = nullptr;
thread_local std::vector<BsStructEntry> g_struct_entries;


/*
 * HEVC typed callback adaptors: convert the C++ struct into its C mirror,
 * hand it to the user callback (valid only for the call), then free it.
 */
void hevc_typed_vps(const bs::VideoParameterSet& src)
{
    const BsHevcHandlers* h = g_hevc_h;
    if (h && h->vps) {
        auto* dst = new BsHevcVideoParameterSet{};
        bs_conv(src, *dst);
        h->vps(h->ctx, dst);
        bs_free_BsHevcVideoParameterSet(dst);
        delete dst;
    }
}

void hevc_typed_sps(const bs::SequenceParameterSet& src)
{
    const BsHevcHandlers* h = g_hevc_h;
    if (h && h->sps) {
        auto* dst = new BsHevcSequenceParameterSet{};
        bs_conv(src, *dst);
        h->sps(h->ctx, dst);
        bs_free_BsHevcSequenceParameterSet(dst);
        delete dst;
    }
}

void hevc_typed_pps(const bs::PictureParameterSet& src)
{
    const BsHevcHandlers* h = g_hevc_h;
    if (h && h->pps) {
        auto* dst = new BsHevcPictureParameterSet{};
        bs_conv(src, *dst);
        h->pps(h->ctx, dst);
        bs_free_BsHevcPictureParameterSet(dst);
        delete dst;
    }
}


/*
 * AVC typed callback adaptors.
 */
void avc_typed_sps(const bs::avc::SequenceParameterSet& src)
{
    const BsAvcHandlers* h = g_avc_h;
    if (h && h->sps) {
        auto* dst = new BsAvcSequenceParameterSet{};
        bs_conv(src, *dst);
        h->sps(h->ctx, dst);
        bs_free_BsAvcSequenceParameterSet(dst);
        delete dst;
    }
}

void avc_typed_pps(const bs::avc::PictureParameterSet& src)
{
    const BsAvcHandlers* h = g_avc_h;
    if (h && h->pps) {
        auto* dst = new BsAvcPictureParameterSet{};
        bs_conv(src, *dst);
        h->pps(h->ctx, dst);
        bs_free_BsAvcPictureParameterSet(dst);
        delete dst;
    }
}


/*
 * SEI adaptors: iterate the parsed messages and forward each with its raw
 * payload type and bytes.  Payload bytes point into the input buffer and are
 * only valid for the duration of this call.
 */
void hevc_typed_sei(const bs::ParsedSei& sei)
{
    const BsHevcHandlers* h = g_hevc_h;
    if (h && h->sei) {
        for (const auto& m : sei.view.messages) {
            h->sei(h->ctx,
                   m.payload_type,
                   reinterpret_cast<const unsigned char*>(
                       m.payload.data()),
                   m.payload.size());
        }
    }
}

void avc_typed_sei(const bs::avc::ParsedSei& sei)
{
    const BsAvcHandlers* h = g_avc_h;
    if (h && h->sei) {
        for (const auto& m : sei.messages) {
            h->sei(h->ctx,
                   m.payload_type,
                   reinterpret_cast<const unsigned char*>(
                       m.payload.data()),
                   m.payload.size());
        }
    }
}


void hevc_typed_slice(const bs::SliceSegmentHeader& src)
{
    const BsHevcHandlers* h = g_hevc_h;
    if (h && h->slice) {
        auto* dst = new BsHevcSliceSegmentHeader{};
        bs_conv(src, *dst);
        h->slice(h->ctx, dst);
        bs_free_BsHevcSliceSegmentHeader(dst);
        delete dst;
    }
}

void avc_typed_slice(const bs::avc::SliceHeader& src)
{
    const BsAvcHandlers* h = g_avc_h;
    if (h && h->slice) {
        auto* dst = new BsAvcSliceHeader{};
        bs_conv(src, *dst);
        h->slice(h->ctx, dst);
        bs_free_BsAvcSliceHeader(dst);
        delete dst;
    }
}


/*
 * Report-collection adaptors: convert and keep ownership in g_struct_entries
 * (freed later by bs_struct_report_destroy).
 */
void hevc_report_vps(const bs::VideoParameterSet& src)
{
    auto* dst = new BsHevcVideoParameterSet{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_HEVC_VPS, dst});
}

void hevc_report_sps(const bs::SequenceParameterSet& src)
{
    auto* dst = new BsHevcSequenceParameterSet{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_HEVC_SPS, dst});
}

void hevc_report_pps(const bs::PictureParameterSet& src)
{
    auto* dst = new BsHevcPictureParameterSet{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_HEVC_PPS, dst});
}

void avc_report_sps(const bs::avc::SequenceParameterSet& src)
{
    auto* dst = new BsAvcSequenceParameterSet{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_AVC_SPS, dst});
}

void avc_report_pps(const bs::avc::PictureParameterSet& src)
{
    auto* dst = new BsAvcPictureParameterSet{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_AVC_PPS, dst});
}


inline void
free_struct_entry(
    const BsStructEntry& e)
{
    switch (e.kind) {

    case BS_STRUCT_HEVC_VPS:
        bs_free_BsHevcVideoParameterSet(
            const_cast<BsHevcVideoParameterSet*>(
                static_cast<const BsHevcVideoParameterSet*>(e.data)));
        delete const_cast<BsHevcVideoParameterSet*>(
            static_cast<const BsHevcVideoParameterSet*>(e.data));
        break;

    case BS_STRUCT_HEVC_SPS:
        bs_free_BsHevcSequenceParameterSet(
            const_cast<BsHevcSequenceParameterSet*>(
                static_cast<const BsHevcSequenceParameterSet*>(e.data)));
        delete const_cast<BsHevcSequenceParameterSet*>(
            static_cast<const BsHevcSequenceParameterSet*>(e.data));
        break;

    case BS_STRUCT_HEVC_PPS:
        bs_free_BsHevcPictureParameterSet(
            const_cast<BsHevcPictureParameterSet*>(
                static_cast<const BsHevcPictureParameterSet*>(e.data)));
        delete const_cast<BsHevcPictureParameterSet*>(
            static_cast<const BsHevcPictureParameterSet*>(e.data));
        break;

    case BS_STRUCT_AVC_SPS:
        bs_free_BsAvcSequenceParameterSet(
            const_cast<BsAvcSequenceParameterSet*>(
                static_cast<const BsAvcSequenceParameterSet*>(e.data)));
        delete const_cast<BsAvcSequenceParameterSet*>(
            static_cast<const BsAvcSequenceParameterSet*>(e.data));
        break;

    case BS_STRUCT_AVC_PPS:
        bs_free_BsAvcPictureParameterSet(
            const_cast<BsAvcPictureParameterSet*>(
                static_cast<const BsAvcPictureParameterSet*>(e.data)));
        delete const_cast<BsAvcPictureParameterSet*>(
            static_cast<const BsAvcPictureParameterSet*>(e.data));
        break;
    }
}

} // namespace


extern "C" {

BsState* bs_state_create(BsCodec codec)
{
    if (codec == BS_CODEC_AUTO) {
        /*
         * The state has no buffer to probe, so an explicit codec is
         * required.  Use bs_parse_report with a NULL state for the
         * auto-detect path.
         */
        set_error(
            "bs_state_create: explicit codec required "
            "(HEVC/AVC); use bs_parse_report with a NULL "
            "state for auto-detect");
        return nullptr;
    }

    try {
        return reinterpret_cast<BsState*>(
            new bs::State(to_codec(codec)));

    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}


void bs_state_destroy(BsState* state)
{
    delete reinterpret_cast<bs::State*>(state);
}


void bs_state_clear(BsState* state)
{
    if (state == nullptr) {
        return;
    }
    reinterpret_cast<bs::State*>(state)->clear();
}


long bs_parse(
    BsState* state,
    const unsigned char* data,
    size_t size,
    BsFramingMode mode,
    unsigned length_size,
    const BsNalHandlers* handlers)
{
    if (state == nullptr || data == nullptr ||
        handlers == nullptr) {
        set_error("bs_parse: null argument");
        return -1;
    }

    try {
        bs::State* s =
            reinterpret_cast<bs::State*>(state);

        std::span<const std::uint8_t> span(
            data, size);

        g_handlers = handlers;
        g_data_start = data;

        const auto cpp_mode = to_mode(mode);
        long result = -1;

        if (s->codec() == bs::Codec::Hevc) {

            bs::BsNalHandlers h{};
            h.vps = hevc_vps;
            h.sps = hevc_sps;
            h.pps = hevc_pps;
            h.prefix_sei = hevc_sei;
            h.suffix_sei = hevc_sei;
            h.slice = hevc_slice;
            h.unsupported = hevc_unsupported;

            const std::size_t n =
                bs::parse(*s, span, cpp_mode, h, length_size);
            result = static_cast<long>(n);

        } else {

            bs::avc::NalHandlers h{};
            h.sps = avc_sps;
            h.pps = avc_pps;
            h.sei = avc_sei;
            h.slice = avc_slice;
            h.unsupported = avc_unsupported;

            const std::size_t n =
                bs::parse(*s, span, cpp_mode, h, length_size);
            result = static_cast<long>(n);
        }

        g_handlers = nullptr;
        g_data_start = nullptr;

        return result;

    } catch (const std::exception& e) {
        g_handlers = nullptr;
        g_data_start = nullptr;
        set_error(e.what());
        return -1;
    }
}


BsReport* bs_parse_report(
    BsState* state,
    const unsigned char* data,
    size_t size,
    BsFramingMode mode,
    unsigned length_size)
{
    if (data == nullptr) {
        set_error("bs_parse_report: null argument");
        return nullptr;
    }

    try {
        /*
         * With a state, use its codec.  With a NULL state, probe the
         * first NAL to auto-detect (the AUTO path).
         */
        const bs::Codec codec =
            (state != nullptr)
                ? reinterpret_cast<bs::State*>(state)
                      ->codec()
                : detect_codec(data, size);

        std::unique_ptr<bs::State> owned;
        bs::State* s = nullptr;

        if (state != nullptr) {
            s = reinterpret_cast<bs::State*>(state);
        } else {
            owned = bs::create_state(codec);
            s = owned.get();
        }

        std::span<const std::uint8_t> span(
            data, size);

        g_entries.clear();
        g_collector_codec = codec;
        g_data_start = data;

        /*
         * The C++ adaptors (hevc_ and avc_ functions) forward each NAL to
         * g_handlers.  Point g_handlers at a BsNalHandlers whose every slot
         * is the collector, so every NAL is captured into g_entries.
         */
        BsNalHandlers h{};
        h.ctx = nullptr;
        h.vps = &collect;
        h.sps = &collect;
        h.pps = &collect;
        h.sei = &collect;
        h.slice = &collect;
        h.unsupported = &collect;

        g_handlers = &h;

        const auto cpp_mode = to_mode(mode);

        if (codec == bs::Codec::Hevc) {
            bs::BsNalHandlers ch{};
            ch.vps = hevc_vps;
            ch.sps = hevc_sps;
            ch.pps = hevc_pps;
            ch.prefix_sei = hevc_sei;
            ch.suffix_sei = hevc_sei;
            ch.slice = hevc_slice;
            ch.unsupported = hevc_unsupported;
            (void)bs::parse(*s, span, cpp_mode, ch, length_size);
        } else {
            bs::avc::NalHandlers ch{};
            ch.sps = avc_sps;
            ch.pps = avc_pps;
            ch.sei = avc_sei;
            ch.slice = avc_slice;
            ch.unsupported = avc_unsupported;
            (void)bs::parse(*s, span, cpp_mode, ch, length_size);
        }

        g_handlers = nullptr;
        g_data_start = nullptr;

        const std::size_t count = g_entries.size();
        BsNalEntry* nals = new BsNalEntry[count];
        std::size_t vcl = 0;

        for (std::size_t i = 0; i < count; ++i) {
            nals[i] = g_entries[i];
            if (nals[i].is_vcl) {
                ++vcl;
            }
        }

        BsReport* report = new BsReport{};
        report->codec =
            (codec == bs::Codec::Hevc)
                ? BS_CODEC_HEVC
                : BS_CODEC_AVC;
        report->nal_count = count;
        report->vcl_count = vcl;
        report->nals = nals;

        return report;

    } catch (const std::exception& e) {
        g_data_start = nullptr;
        set_error(e.what());
        return nullptr;
    }
}


void bs_report_destroy(BsReport* report)
{
    if (report == nullptr) {
        return;
    }
    delete[] report->nals;
    delete report;
}


long bs_parse_hevc(
    BsState* state,
    const unsigned char* data,
    size_t size,
    BsFramingMode mode,
    unsigned length_size,
    const BsHevcHandlers* handlers)
{
    if (state == nullptr || data == nullptr ||
        handlers == nullptr) {
        set_error("bs_parse_hevc: null argument");
        return -1;
    }

    try {
        bs::State* s =
            reinterpret_cast<bs::State*>(state);

        if (s->codec() != bs::Codec::Hevc) {
            set_error("bs_parse_hevc: state is not an HEVC state");
            return -1;
        }

        std::span<const std::uint8_t> span(
            data, size);

        g_hevc_h = handlers;

        bs::HevcParsedHandlers h{};
        h.vps = hevc_typed_vps;
        h.sps = hevc_typed_sps;
        h.pps = hevc_typed_pps;
        h.sei = hevc_typed_sei;
        h.slice = hevc_typed_slice;

        const std::size_t n =
            bs::parse(*s, span, to_mode(mode), h, length_size);

        g_hevc_h = nullptr;

        return static_cast<long>(n);

    } catch (const std::exception& e) {
        g_hevc_h = nullptr;
        set_error(e.what());
        return -1;
    }
}


long bs_parse_avc(
    BsState* state,
    const unsigned char* data,
    size_t size,
    BsFramingMode mode,
    unsigned length_size,
    const BsAvcHandlers* handlers)
{
    if (state == nullptr || data == nullptr ||
        handlers == nullptr) {
        set_error("bs_parse_avc: null argument");
        return -1;
    }

    try {
        bs::State* s =
            reinterpret_cast<bs::State*>(state);

        if (s->codec() != bs::Codec::Avc) {
            set_error("bs_parse_avc: state is not an AVC state");
            return -1;
        }

        std::span<const std::uint8_t> span(
            data, size);

        g_avc_h = handlers;

        bs::AvcParsedHandlers h{};
        h.sps = avc_typed_sps;
        h.pps = avc_typed_pps;
        h.sei = avc_typed_sei;
        h.slice = avc_typed_slice;

        const std::size_t n =
            bs::parse(*s, span, to_mode(mode), h, length_size);

        g_avc_h = nullptr;

        return static_cast<long>(n);

    } catch (const std::exception& e) {
        g_avc_h = nullptr;
        set_error(e.what());
        return -1;
    }
}


BsStructReport* bs_parse_struct_report(
    BsState* state,
    const unsigned char* data,
    size_t size,
    BsFramingMode mode,
    unsigned length_size)
{
    if (data == nullptr) {
        set_error("bs_parse_struct_report: null argument");
        return nullptr;
    }

    try {
        const bs::Codec codec =
            (state != nullptr)
                ? reinterpret_cast<bs::State*>(state)
                      ->codec()
                : detect_codec(data, size);

        std::unique_ptr<bs::State> owned;
        bs::State* s = nullptr;

        if (state != nullptr) {
            s = reinterpret_cast<bs::State*>(state);
        } else {
            owned = bs::create_state(codec);
            s = owned.get();
        }

        std::span<const std::uint8_t> span(
            data, size);

        g_struct_entries.clear();

        bs::HevcParsedHandlers hh{};
        bs::AvcParsedHandlers ah{};

        if (codec == bs::Codec::Hevc) {
            hh.vps = hevc_report_vps;
            hh.sps = hevc_report_sps;
            hh.pps = hevc_report_pps;
        } else {
            ah.sps = avc_report_sps;
            ah.pps = avc_report_pps;
        }

        if (codec == bs::Codec::Hevc) {
            (void)bs::parse(*s, span, to_mode(mode), hh, length_size);
        } else {
            (void)bs::parse(*s, span, to_mode(mode), ah, length_size);
        }

        const std::size_t count = g_struct_entries.size();
        BsStructEntry* entries = new BsStructEntry[count];
        for (std::size_t i = 0; i < count; ++i) {
            entries[i] = g_struct_entries[i];
        }
        g_struct_entries.clear();

        BsStructReport* report = new BsStructReport{};
        report->codec =
            (codec == bs::Codec::Hevc)
                ? BS_CODEC_HEVC
                : BS_CODEC_AVC;
        report->count = count;
        report->entries = entries;

        return report;

    } catch (const std::exception& e) {
        /* free anything collected so far */
        for (const BsStructEntry& e2 : g_struct_entries) {
            free_struct_entry(e2);
        }
        g_struct_entries.clear();
        set_error(e.what());
        return nullptr;
    }
}


void bs_struct_report_destroy(BsStructReport* report)
{
    if (report == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < report->count; ++i) {
        free_struct_entry(report->entries[i]);
    }
    delete[] report->entries;
    delete report;
}


const char* bs_get_last_error(void)
{
    return g_last_error.c_str();
}

} // extern "C"
