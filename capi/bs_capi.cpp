// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

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
using bs::capi::bs_free_BsAv1FrameHeader;
using bs::capi::bs_free_BsAv1SequenceHeader;
using bs::capi::bs_free_BsAvcPictureParameterSet;
using bs::capi::bs_free_BsAvcSequenceParameterSet;
using bs::capi::bs_free_BsAvcSliceHeader;
using bs::capi::bs_free_BsHevcPictureParameterSet;
using bs::capi::bs_free_BsHevcSequenceParameterSet;
using bs::capi::bs_free_BsHevcSliceSegmentHeader;
using bs::capi::bs_free_BsHevcVideoParameterSet;
using bs::capi::bs_free_BsVp8FrameHeader;
using bs::capi::bs_free_BsVp9FrameHeader;
using bs::capi::bs_free_BsVvcDci;
using bs::capi::bs_free_BsVvcOpi;
using bs::capi::bs_free_BsVvcPictureHeader;
using bs::capi::bs_free_BsVvcPictureParameterSet;
using bs::capi::bs_free_BsVvcSequenceParameterSet;
using bs::capi::bs_free_BsVvcSliceHeader;
using bs::capi::bs_free_BsVvcVideoParameterSet;

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
inline bs::Codec to_codec(BsCodec c) {
    switch (c) {
        case BS_CODEC_AVC:
            return bs::Codec::Avc;
        case BS_CODEC_VVC:
            return bs::Codec::Vvc;
        case BS_CODEC_AV1:
            return bs::Codec::Av1;
        case BS_CODEC_VP9:
            return bs::Codec::Vp9;
        case BS_CODEC_VP8:
            return bs::Codec::Vp8;
        case BS_CODEC_HEVC:
        default:
            /* AUTO collapses to HEVC here; the real auto-probe is in
             * bs_parse_report. */
            return bs::Codec::Hevc;
    }
}

[[nodiscard]]
inline BsCodec to_bs_codec(bs::Codec c) {
    switch (c) {
        case bs::Codec::Avc:
            return BS_CODEC_AVC;
        case bs::Codec::Vvc:
            return BS_CODEC_VVC;
        case bs::Codec::Av1:
            return BS_CODEC_AV1;
        case bs::Codec::Vp9:
            return BS_CODEC_VP9;
        case bs::Codec::Vp8:
            return BS_CODEC_VP8;
        case bs::Codec::Hevc:
        default:
            return BS_CODEC_HEVC;
    }
}

[[nodiscard]]
inline bs::NalFramingMode to_mode(BsFramingMode m) {
    switch (m) {
        case BS_FRAMING_LENGTH_PREFIXED:
            return bs::NalFramingMode::LengthPrefixed;
        case BS_FRAMING_OBU:
            return bs::NalFramingMode::Obu;
        case BS_FRAMING_IVF:
            return bs::NalFramingMode::Ivf;
        case BS_FRAMING_ANNEX_B:
        default:
            return bs::NalFramingMode::AnnexB;
    }
}

/*
 * Framing that carries a given codec by default.  Used by the auto-detect
 * path (NULL state): the caller's framing cannot be known before the codec is
 * detected, so the detected codec selects it.
 */
[[nodiscard]]
inline bs::NalFramingMode default_framing(bs::Codec c) {
    switch (c) {
        case bs::Codec::Av1:
            return bs::NalFramingMode::Obu;
        case bs::Codec::Vp9:
        case bs::Codec::Vp8:
            return bs::NalFramingMode::Ivf;
        case bs::Codec::Hevc:
        case bs::Codec::Avc:
        case bs::Codec::Vvc:
        default:
            return bs::NalFramingMode::AnnexB;
    }
}

/*
 * Probe the first unit to decide the codec (mirrors cli auto-detect).
 */
[[nodiscard]]
inline bs::Codec detect_codec(const unsigned char* data, std::size_t size) {
    if (size < 3) {
        return bs::Codec::Hevc;
    }

    /*
     * IVF container: 'DKIF' magic, fourcc at bytes 8..11 (VP80/VP90/AV01).
     */
    if (size >= 12 && data[0] == 'D' && data[1] == 'K' && data[2] == 'I' && data[3] == 'F') {
        if (data[8] == 'V' && data[9] == 'P') {
            if (data[10] == '8') {
                return bs::Codec::Vp8;
            }
            if (data[10] == '9') {
                return bs::Codec::Vp9;
            }
        }
        if (data[8] == 'A' && data[9] == 'V' && data[10] == '0' && data[11] == '1') {
            return bs::Codec::Av1;
        }
        return bs::Codec::Vp9;
    }

    /*
     * AV1 raw OBU stream.  A stream commonly opens with a sequence-header
     * OBU (0x0A / 0x0B header byte, type 1 with size field) or a
     * temporal-delimiter OBU (0x12 0x00).  The first byte never looks like
     * an Annex-B start code, so this cannot collide with a NAL stream.
     */
    if (data[0] == 0x0A || (data[0] == 0x12 && data[1] == 0x00)) {
        return bs::Codec::Av1;
    }

    /*
     * VP8: every key frame starts with the start code 0x9D 0x01 0x2A.
     */
    if (size >= 3 && data[0] == 0x9D && data[1] == 0x01 && data[2] == 0x2A) {
        return bs::Codec::Vp8;
    }

    /*
     * VP9: every key frame starts with the marker 0x82 0x49 0x83 0x42.
     */
    if (size >= 4 && data[0] == 0x82 && data[1] == 0x49 && data[2] == 0x83 && data[3] == 0x42) {
        return bs::Codec::Vp9;
    }

    /*
     * Annex-B NAL stream (HEVC / AVC / VVC).  The first NAL is not
     * necessarily a parameter set (an encoder may lead with AUD, SEI or a
     * VCL slice), so scan up to 64 leading NALs and count codec-specific
     * types.
     *
     * The three codecs put the type in different header bytes:
     *
     *   HEVC  type = (b0 >> 1) & 0x3F   VPS=32 SPS=33 PPS=34 SEI=35/36
     *   VVC   type = (b1 >> 3) & 0x1F   OPI=12 DCI=13 VPS=14 SPS=15 PPS=16
     *                                   APS=17/18 PH=19 AUD=20 EOS=21
     *                                   EOB=22 SEI=23/24 FD=25
     *   AVC   b0 == 0x67/0x27 (SPS), 0x68/0x28 (PPS)
     *
     * HEVC parameter sets are unambiguous (VVC reserves types 32..34), so
     * they win.  VVC-only types are checked against the second byte, which
     * is tid_plus1 (<= 7) in HEVC base-layer NALs and never aliases a VVC
     * type; AVC parameter-set bytes are excluded from that count.  With no
     * decisive type the first NAL byte decides (an AVC type 1..21 is AVC,
     * otherwise HEVC).
     */
    std::size_t i = 0;
    unsigned vvc_seen = 0;
    unsigned hevc_seen = 0;
    unsigned avc_seen = 0;
    unsigned first_avc_type = 0;
    bool have_first = false;

    for (unsigned nal = 0; i < size && nal < 64; ++nal) {
        if (i + 4 <= size && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x00 &&
            data[i + 3] == 0x01) {
            i += 4;
        } else if (i + 3 <= size && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
            i += 3;
        } else {
            break;
        }

        if (i + 2 > size) {
            break;
        }

        const unsigned b0 = data[i];
        const unsigned b1 = data[i + 1];
        const unsigned hevc_type = (b0 >> 1) & 0x3F;
        const unsigned vvc_type = (b1 >> 3) & 0x1F;

        if (!have_first) {
            have_first = true;
            first_avc_type = b0 & 0x1F;
        }

        /*
         * VVC-only types (base layer): byte0 is fz(1)+rz(1)+layer(6) and is
         * 0x00 for base layer, byte1 holds the type.  The b0 == 0x00 guard
         * prevents AVC payload bytes (byte0 is never 0x00 in a real AVC
         * stream) and HEVC base-layer NALs (byte1 <= 7) from aliasing.
         */
        if (b0 == 0x00 && vvc_type >= 12 && vvc_type <= 25) {
            ++vvc_seen;
        } else if (hevc_type == 32 || hevc_type == 33 || hevc_type == 34 || hevc_type == 35 ||
                   hevc_type == 36) {
            if ((b1 >> 3) == 0 && (b1 & 0x07) != 0) {
                ++hevc_seen;
            }
        } else if (b0 == 0x67 || b0 == 0x27 || b0 == 0x68 || b0 == 0x28) {
            ++avc_seen;
        }

        /* Advance past this NAL. */
        i += 2;
        while (i + 3 <= size && !(data[i] == 0x00 && data[i + 1] == 0x00 &&
                                  (data[i + 2] == 0x01 || data[i + 2] == 0x00))) {
            ++i;
        }
    }

    if (hevc_seen > 0) {
        return bs::Codec::Hevc;
    }
    if (vvc_seen > 0) {
        return bs::Codec::Vvc;
    }
    if (avc_seen > 0) {
        return bs::Codec::Avc;
    }
    if (have_first && first_avc_type >= 1 && first_avc_type <= 21) {
        return bs::Codec::Avc;
    }

    return bs::Codec::Hevc;
}

/*
 * Static NAL type names (returned by BsNalEntry.nal_type_name).
 */
[[nodiscard]]
const char* hevc_type_name(int t) {
    switch (t) {
        case 32:
            return "VPS_NUT";
        case 33:
            return "SPS_NUT";
        case 34:
            return "PPS_NUT";
        case 35:
            return "PREFIX_SEI_NUT";
        case 36:
            return "SUFFIX_SEI_NUT";
        case 37:
            return "AUD_NUT";
        case 38:
            return "EOS_NUT";
        case 39:
            return "EOB_NUT";
        case 40:
            return "FD_NUT";
        default:
            if (t >= 0 && t <= 31) {
                return "VCL";
            }
            return "NAL";
    }
}

[[nodiscard]]
const char* avc_type_name(int t) {
    switch (t) {
        case 1:
            return "SliceNonIdr";
        case 2:
            return "SliceDataPartitionA";
        case 3:
            return "SliceDataPartitionB";
        case 4:
            return "SliceDataPartitionC";
        case 5:
            return "SliceIdr";
        case 6:
            return "SEI";
        case 7:
            return "SPS";
        case 8:
            return "PPS";
        case 9:
            return "AUD";
        case 10:
            return "EndOfSequence";
        case 11:
            return "EndOfStream";
        case 12:
            return "FillerData";
        case 13:
            return "SpsExtension";
        case 14:
            return "PrefixNal";
        case 15:
            return "SubsetSps";
        case 19:
            return "AuxCodedPicture";
        case 20:
            return "SliceSvcExtension";
        case 21:
            return "SliceMvcExtension";
        case 22:
            return "SliceAvc3dExtension";
        default:
            if ((t >= 1 && t <= 5) || t == 19 || t == 20 || t == 21 || t == 22) {
                return "VCL";
            }
            return "NAL";
    }
}

/*
 * Static VVC NAL type names.
 */
[[nodiscard]]
const char* vvc_type_name(int t) {
    switch (t) {
        case 12:
            return "OPI_NUT";
        case 13:
            return "DCI_NUT";
        case 14:
            return "VPS_NUT";
        case 15:
            return "SPS_NUT";
        case 16:
            return "PPS_NUT";
        case 17:
            return "PREFIX_APS_NUT";
        case 18:
            return "SUFFIX_APS_NUT";
        case 19:
            return "PH_NUT";
        case 20:
            return "AUD_NUT";
        case 21:
            return "EOS_NUT";
        case 22:
            return "EOB_NUT";
        case 23:
            return "PREFIX_SEI_NUT";
        case 24:
            return "SUFFIX_SEI_NUT";
        case 25:
            return "FD_NUT";
        default:
            if (t >= 0 && t <= 11) {
                return "VCL";
            }
            return "NAL";
    }
}

/*
 * Static AV1 OBU type names.
 */
[[nodiscard]]
const char* av1_type_name(unsigned type) {
    switch (type) {
        case 1:
            return "SEQUENCE_HEADER";
        case 2:
            return "TEMPORAL_DELIMITER";
        case 3:
            return "FRAME_HEADER";
        case 4:
            return "TILE_GROUP";
        case 5:
            return "METADATA";
        case 6:
            return "FRAME";
        case 7:
            return "REDUNDANT_FRAME_HEADER";
        case 8:
            return "TILE_LIST";
        case 15:
            return "PADDING";
        default:
            return "OBU";
    }
}

/*
 * Last-error string (per thread).
 */
thread_local std::string g_last_error;

inline void set_error(const char* msg) {
    g_last_error = msg ? msg : "unknown error";
}

/*
 * Fill a BsNalUnit from a HEVC NalUnit view.  payload points into the
 * caller's buffer; offset is its position within that buffer.
 */
inline void fill_hevc(const bs::NalUnit& nal, BsNalUnit& out, const unsigned char* base) {
    auto p = nal.payload_bytes();
    out.nal_unit_type = static_cast<int>(static_cast<unsigned>(nal.type()));
    out.nuh_layer_id = nal.layer_id();
    out.nuh_temporal_id_plus1 = static_cast<int>(nal.temporal_id()) + 1;
    out.forbidden_zero_bit = nal.header.forbidden_zero_bit ? 1 : 0;
    out.is_vcl = nal.is_vcl() ? 1 : 0;
    out.payload = p.data();
    out.payload_size = p.size();
    out.offset = static_cast<size_t>(p.data() - base);
}

/*
 * Fill a BsNalUnit from an AVC NalUnit view.
 */
inline void fill_avc(const bs::avc::NalUnit& nal, BsNalUnit& out, const unsigned char* base) {
    auto p = nal.payload_bytes();
    out.nal_unit_type = static_cast<int>(static_cast<unsigned>(nal.type()));
    out.nuh_layer_id = 0;
    out.nuh_temporal_id_plus1 = 0;
    out.forbidden_zero_bit = nal.header.forbidden_zero_bit ? 1 : 0;
    out.is_vcl = nal.is_vcl() ? 1 : 0;
    out.payload = p.data();
    out.payload_size = p.size();
    out.offset = static_cast<size_t>(p.data() - base);
}

/*
 * Deliver one HEVC/AVC NAL to a BsNalHandlers slot (the catch-all `nal` fires
 * for every unit, then the type slot).
 */
inline void emit_hevc(const bs::NalUnit& nal, BsNalCallback slot) {
    const BsNalHandlers* h = g_handlers;
    if (h && slot) {
        BsNalUnit n{};
        fill_hevc(nal, n, g_data_start);
        slot(h->ctx, &n);
    }
}

inline void emit_avc(const bs::avc::NalUnit& nal, BsNalCallback slot) {
    const BsNalHandlers* h = g_handlers;
    if (h && slot) {
        BsNalUnit n{};
        fill_avc(nal, n, g_data_start);
        slot(h->ctx, &n);
    }
}

void hevc_vps(const bs::NalUnit& nal) {
    if (g_handlers) {
        emit_hevc(nal, g_handlers->nal);
        emit_hevc(nal, g_handlers->vps);
    }
}

void hevc_sps(const bs::NalUnit& nal) {
    if (g_handlers) {
        emit_hevc(nal, g_handlers->nal);
        emit_hevc(nal, g_handlers->sps);
    }
}

void hevc_pps(const bs::NalUnit& nal) {
    if (g_handlers) {
        emit_hevc(nal, g_handlers->nal);
        emit_hevc(nal, g_handlers->pps);
    }
}

void hevc_sei(const bs::NalUnit& nal) {
    if (g_handlers) {
        emit_hevc(nal, g_handlers->nal);
        emit_hevc(nal, g_handlers->sei);
    }
}

void hevc_slice(const bs::NalUnit& nal) {
    if (g_handlers) {
        emit_hevc(nal, g_handlers->nal);
        emit_hevc(nal, g_handlers->slice);
    }
}

void hevc_unsupported(const bs::NalUnit& nal) {
    if (g_handlers) {
        emit_hevc(nal, g_handlers->nal);
        emit_hevc(nal, g_handlers->unsupported);
    }
}

/*
 * AVC adaptors.  (AVC has no VPS, so there is no avc_vps adaptor.)
 */
void avc_sps(const bs::avc::NalUnit& nal) {
    if (g_handlers) {
        emit_avc(nal, g_handlers->nal);
        emit_avc(nal, g_handlers->sps);
    }
}

void avc_pps(const bs::avc::NalUnit& nal) {
    if (g_handlers) {
        emit_avc(nal, g_handlers->nal);
        emit_avc(nal, g_handlers->pps);
    }
}

void avc_sei(const bs::avc::NalUnit& nal) {
    if (g_handlers) {
        emit_avc(nal, g_handlers->nal);
        emit_avc(nal, g_handlers->sei);
    }
}

void avc_slice(const bs::avc::NalUnit& nal) {
    if (g_handlers) {
        emit_avc(nal, g_handlers->nal);
        emit_avc(nal, g_handlers->slice);
    }
}

void avc_unsupported(const bs::avc::NalUnit& nal) {
    if (g_handlers) {
        emit_avc(nal, g_handlers->nal);
        emit_avc(nal, g_handlers->unsupported);
    }
}

/*
 * VVC raw-NAL walk.  The C++ core only exposes typed VVC dispatch, so the
 * generic BsNalHandlers path walks the framer directly and maps NAL types to
 * the vps/sps/pps/sei/slice/unsupported slots (the catch-all `nal` fires for
 * every unit).
 */
template <typename Framer>
[[nodiscard]]
inline std::size_t walk_vvc_raw(Framer& framer, const BsNalHandlers* h) {
    std::size_t count = 0;

    while (framer.valid()) {
        const auto span = framer.nal();

        try {
            bs::vvc::NalUnit nal = bs::vvc::parse_nal_unit(span);
            auto p = nal.payload_bytes();

            BsNalUnit n{};
            n.nal_unit_type = static_cast<int>(nal.nal_type());
            n.nuh_layer_id = static_cast<int>(nal.layer_id());
            n.nuh_temporal_id_plus1 = static_cast<int>(nal.temporal_id()) + 1;
            n.forbidden_zero_bit = 0;
            n.is_vcl = nal.is_vcl() ? 1 : 0;
            n.payload = p.data();
            n.payload_size = p.size();
            n.offset = static_cast<size_t>(p.data() - g_data_start);

            if (h && h->nal) {
                h->nal(h->ctx, &n);
            }

            if (nal.is_vcl()) {
                if (h && h->slice) {
                    h->slice(h->ctx, &n);
                }
            } else {
                switch (nal.nal_type()) {
                    case static_cast<std::uint8_t>(bs::vvc::NalUnitType::VpsNut):
                        if (h && h->vps) {
                            h->vps(h->ctx, &n);
                        }
                        break;
                    case static_cast<std::uint8_t>(bs::vvc::NalUnitType::SpsNut):
                        if (h && h->sps) {
                            h->sps(h->ctx, &n);
                        }
                        break;
                    case static_cast<std::uint8_t>(bs::vvc::NalUnitType::PpsNut):
                        if (h && h->pps) {
                            h->pps(h->ctx, &n);
                        }
                        break;
                    case static_cast<std::uint8_t>(bs::vvc::NalUnitType::SeiPrefixNut):
                    case static_cast<std::uint8_t>(bs::vvc::NalUnitType::SeiSuffixNut):
                        if (h && h->sei) {
                            h->sei(h->ctx, &n);
                        }
                        break;
                    default:
                        if (h && h->unsupported) {
                            h->unsupported(h->ctx, &n);
                        }
                        break;
                }
            }

            ++count;

        } catch (...) {
            /* skip a NAL that cannot be framed/parsed */
        }

        framer.next();
    }

    return count;
}

/*
 * AV1 OBU walk for the generic BsNalHandlers path.  Every OBU is delivered
 * through the catch-all `nal` callback (AV1 has no NAL slot mapping).
 */
[[nodiscard]]
inline std::size_t walk_av1_raw(std::span<const std::uint8_t> data, const BsNalHandlers* h) {
    std::size_t count = 0;
    bs::av1::ObuFramer f{data};

    while (f.valid()) {
        const auto span = f.obu();

        try {
            bs::av1::Obu obu = bs::av1::parse_obu(span);
            auto p = obu.payload_bytes();

            BsNalUnit n{};
            n.nal_unit_type = static_cast<int>(obu.type());
            n.nuh_layer_id = 0;
            n.nuh_temporal_id_plus1 = 0;
            n.forbidden_zero_bit = 0;
            n.is_vcl = 0;
            n.payload = p.data();
            n.payload_size = p.size();
            n.offset = static_cast<size_t>(p.data() - g_data_start);

            if (h && h->nal) {
                h->nal(h->ctx, &n);
            }
            if (h && h->unsupported) {
                h->unsupported(h->ctx, &n);
            }

            ++count;

        } catch (...) {
            /* skip a malformed OBU */
        }

        f.next();
    }

    return count;
}

/*
 * VP9 / VP8 IVF frame walk for the generic BsNalHandlers path.  Every frame is
 * delivered through the catch-all `nal` callback.
 */
template <typename Framer>
[[nodiscard]]
inline std::size_t walk_vp_frame_raw(Framer& framer, const BsNalHandlers* h) {
    std::size_t count = 0;

    while (framer.valid()) {
        const auto frame = framer.frame();

        BsNalUnit n{};
        n.nal_unit_type = 0;
        n.nuh_layer_id = 0;
        n.nuh_temporal_id_plus1 = 0;
        n.forbidden_zero_bit = 0;
        n.is_vcl = 0;
        n.payload = frame.data();
        n.payload_size = frame.size();
        n.offset = static_cast<size_t>(frame.data() - g_data_start);

        if (h && h->nal) {
            h->nal(h->ctx, &n);
        }
        if (h && h->unsupported) {
            h->unsupported(h->ctx, &n);
        }

        ++count;
        framer.next();
    }

    return count;
}

/*
 * Single collector callback used by bs_parse_report() for every NAL type.
 */
[[nodiscard]]
inline const char* type_name_for(bs::Codec codec, int type) {
    switch (codec) {
        case bs::Codec::Avc:
            return avc_type_name(type);
        case bs::Codec::Vvc:
            return vvc_type_name(type);
        case bs::Codec::Av1:
            return av1_type_name(static_cast<unsigned>(type));
        case bs::Codec::Vp9:
        case bs::Codec::Vp8:
            return "frame";
        case bs::Codec::Hevc:
        default:
            return hevc_type_name(type);
    }
}

void collect(void*, const BsNalUnit* n) {
    BsNalEntry e{};
    e.index = g_entries.size();
    e.offset = n->offset;
    e.nal_unit_type = n->nal_unit_type;
    e.is_vcl = n->is_vcl;
    e.size = n->payload_size;
    e.nal_type_name = type_name_for(g_collector_codec, n->nal_unit_type);
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
void hevc_typed_vps(const bs::VideoParameterSet& src) {
    const BsHevcHandlers* h = g_hevc_h;
    if (h && h->vps) {
        auto* dst = new BsHevcVideoParameterSet{};
        bs_conv(src, *dst);
        h->vps(h->ctx, dst);
        bs_free_BsHevcVideoParameterSet(dst);
        delete dst;
    }
}

void hevc_typed_sps(const bs::SequenceParameterSet& src) {
    const BsHevcHandlers* h = g_hevc_h;
    if (h && h->sps) {
        auto* dst = new BsHevcSequenceParameterSet{};
        bs_conv(src, *dst);
        h->sps(h->ctx, dst);
        bs_free_BsHevcSequenceParameterSet(dst);
        delete dst;
    }
}

void hevc_typed_pps(const bs::PictureParameterSet& src) {
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
void avc_typed_sps(const bs::avc::SequenceParameterSet& src) {
    const BsAvcHandlers* h = g_avc_h;
    if (h && h->sps) {
        auto* dst = new BsAvcSequenceParameterSet{};
        bs_conv(src, *dst);
        h->sps(h->ctx, dst);
        bs_free_BsAvcSequenceParameterSet(dst);
        delete dst;
    }
}

void avc_typed_pps(const bs::avc::PictureParameterSet& src) {
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
void hevc_typed_sei(const bs::ParsedSei& sei) {
    const BsHevcHandlers* h = g_hevc_h;
    if (h && h->sei) {
        for (const auto& m : sei.view.messages) {
            h->sei(
                h->ctx,
                m.payload_type,
                reinterpret_cast<const unsigned char*>(m.payload.data()),
                m.payload.size()
            );
        }
    }
}

void avc_typed_sei(const bs::avc::ParsedSei& sei) {
    const BsAvcHandlers* h = g_avc_h;
    if (h && h->sei) {
        for (const auto& m : sei.messages) {
            h->sei(
                h->ctx,
                m.payload_type,
                reinterpret_cast<const unsigned char*>(m.payload.data()),
                m.payload.size()
            );
        }
    }
}

void hevc_typed_slice(const bs::SliceSegmentHeader& src) {
    const BsHevcHandlers* h = g_hevc_h;
    if (h && h->slice) {
        auto* dst = new BsHevcSliceSegmentHeader{};
        bs_conv(src, *dst);
        h->slice(h->ctx, dst);
        bs_free_BsHevcSliceSegmentHeader(dst);
        delete dst;
    }
}

void avc_typed_slice(const bs::avc::SliceHeader& src) {
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
 * VVC / AV1 / VP9 / VP8 typed callback adaptors (same ownership contract as
 * the HEVC/AVC ones: the C struct is valid for the callback only).
 */
thread_local const BsVvcHandlers* g_vvc_h = nullptr;
thread_local const BsAv1Handlers* g_av1_h = nullptr;
thread_local const BsVp9Handlers* g_vp9_h = nullptr;
thread_local const BsVp8Handlers* g_vp8_h = nullptr;

void vvc_typed_dci(const bs::vvc::Dci& src) {
    const BsVvcHandlers* h = g_vvc_h;
    if (h && h->dci) {
        auto* dst = new BsVvcDci{};
        bs_conv(src, *dst);
        h->dci(h->ctx, dst);
        bs_free_BsVvcDci(dst);
        delete dst;
    }
}

void vvc_typed_opi(const bs::vvc::Opi& src) {
    const BsVvcHandlers* h = g_vvc_h;
    if (h && h->opi) {
        auto* dst = new BsVvcOpi{};
        bs_conv(src, *dst);
        h->opi(h->ctx, dst);
        bs_free_BsVvcOpi(dst);
        delete dst;
    }
}

void vvc_typed_vps(const bs::vvc::VideoParameterSet& src) {
    const BsVvcHandlers* h = g_vvc_h;
    if (h && h->vps) {
        auto* dst = new BsVvcVideoParameterSet{};
        bs_conv(src, *dst);
        h->vps(h->ctx, dst);
        bs_free_BsVvcVideoParameterSet(dst);
        delete dst;
    }
}

void vvc_typed_sps(const bs::vvc::SequenceParameterSet& src) {
    const BsVvcHandlers* h = g_vvc_h;
    if (h && h->sps) {
        auto* dst = new BsVvcSequenceParameterSet{};
        bs_conv(src, *dst);
        h->sps(h->ctx, dst);
        bs_free_BsVvcSequenceParameterSet(dst);
        delete dst;
    }
}

void vvc_typed_pps(const bs::vvc::PictureParameterSet& src) {
    const BsVvcHandlers* h = g_vvc_h;
    if (h && h->pps) {
        auto* dst = new BsVvcPictureParameterSet{};
        bs_conv(src, *dst);
        h->pps(h->ctx, dst);
        bs_free_BsVvcPictureParameterSet(dst);
        delete dst;
    }
}

void vvc_typed_ph(const bs::vvc::PictureHeader& src) {
    const BsVvcHandlers* h = g_vvc_h;
    if (h && h->ph) {
        auto* dst = new BsVvcPictureHeader{};
        bs_conv(src, *dst);
        h->ph(h->ctx, dst);
        bs_free_BsVvcPictureHeader(dst);
        delete dst;
    }
}

void vvc_typed_slice(const bs::vvc::SliceHeader& src) {
    const BsVvcHandlers* h = g_vvc_h;
    if (h && h->slice) {
        auto* dst = new BsVvcSliceHeader{};
        bs_conv(src, *dst);
        h->slice(h->ctx, dst);
        bs_free_BsVvcSliceHeader(dst);
        delete dst;
    }
}

void av1_typed_sequence_header(const bs::av1::SequenceHeader& src) {
    const BsAv1Handlers* h = g_av1_h;
    if (h && h->sequence_header) {
        auto* dst = new BsAv1SequenceHeader{};
        bs_conv(src, *dst);
        h->sequence_header(h->ctx, dst);
        bs_free_BsAv1SequenceHeader(dst);
        delete dst;
    }
}

void av1_typed_frame_header(const bs::av1::FrameHeader& src) {
    const BsAv1Handlers* h = g_av1_h;
    if (h && h->frame_header) {
        auto* dst = new BsAv1FrameHeader{};
        bs_conv(src, *dst);
        h->frame_header(h->ctx, dst);
        bs_free_BsAv1FrameHeader(dst);
        delete dst;
    }
}

void vp9_typed_frame_header(const bs::vp9::FrameHeader& src) {
    const BsVp9Handlers* h = g_vp9_h;
    if (h && h->frame_header) {
        auto* dst = new BsVp9FrameHeader{};
        bs_conv(src, *dst);
        h->frame_header(h->ctx, dst);
        bs_free_BsVp9FrameHeader(dst);
        delete dst;
    }
}

void vp8_typed_frame_header(const bs::vp8::FrameHeader& src) {
    const BsVp8Handlers* h = g_vp8_h;
    if (h && h->frame_header) {
        auto* dst = new BsVp8FrameHeader{};
        bs_conv(src, *dst);
        h->frame_header(h->ctx, dst);
        bs_free_BsVp8FrameHeader(dst);
        delete dst;
    }
}

/*
 * Report-collection adaptors: convert and keep ownership in g_struct_entries
 * (freed later by bs_struct_report_destroy).
 */
void hevc_report_vps(const bs::VideoParameterSet& src) {
    auto* dst = new BsHevcVideoParameterSet{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_HEVC_VPS, dst});
}

void hevc_report_sps(const bs::SequenceParameterSet& src) {
    auto* dst = new BsHevcSequenceParameterSet{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_HEVC_SPS, dst});
}

void hevc_report_pps(const bs::PictureParameterSet& src) {
    auto* dst = new BsHevcPictureParameterSet{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_HEVC_PPS, dst});
}

void avc_report_sps(const bs::avc::SequenceParameterSet& src) {
    auto* dst = new BsAvcSequenceParameterSet{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_AVC_SPS, dst});
}

void avc_report_pps(const bs::avc::PictureParameterSet& src) {
    auto* dst = new BsAvcPictureParameterSet{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_AVC_PPS, dst});
}

/*
 * VVC / AV1 / VP9 / VP8 report-collection adaptors: convert and keep
 * ownership in g_struct_entries (freed later by bs_struct_report_destroy).
 */
void vvc_report_dci(const bs::vvc::Dci& src) {
    auto* dst = new BsVvcDci{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_VVC_DCI, dst});
}

void vvc_report_opi(const bs::vvc::Opi& src) {
    auto* dst = new BsVvcOpi{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_VVC_OPI, dst});
}

void vvc_report_vps(const bs::vvc::VideoParameterSet& src) {
    auto* dst = new BsVvcVideoParameterSet{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_VVC_VPS, dst});
}

void vvc_report_sps(const bs::vvc::SequenceParameterSet& src) {
    auto* dst = new BsVvcSequenceParameterSet{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_VVC_SPS, dst});
}

void vvc_report_pps(const bs::vvc::PictureParameterSet& src) {
    auto* dst = new BsVvcPictureParameterSet{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_VVC_PPS, dst});
}

void vvc_report_ph(const bs::vvc::PictureHeader& src) {
    auto* dst = new BsVvcPictureHeader{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_VVC_PH, dst});
}

void vvc_report_slice(const bs::vvc::SliceHeader& src) {
    auto* dst = new BsVvcSliceHeader{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_VVC_SLICE, dst});
}

void av1_report_sequence_header(const bs::av1::SequenceHeader& src) {
    auto* dst = new BsAv1SequenceHeader{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_AV1_SEQUENCE_HEADER, dst});
}

void av1_report_frame_header(const bs::av1::FrameHeader& src) {
    auto* dst = new BsAv1FrameHeader{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_AV1_FRAME_HEADER, dst});
}

void vp9_report_frame_header(const bs::vp9::FrameHeader& src) {
    auto* dst = new BsVp9FrameHeader{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_VP9_FRAME_HEADER, dst});
}

void vp8_report_frame_header(const bs::vp8::FrameHeader& src) {
    auto* dst = new BsVp8FrameHeader{};
    bs_conv(src, *dst);
    g_struct_entries.push_back(BsStructEntry{BS_STRUCT_VP8_FRAME_HEADER, dst});
}

inline void free_struct_entry(const BsStructEntry& e) {
    switch (e.kind) {
        case BS_STRUCT_HEVC_VPS:
            bs_free_BsHevcVideoParameterSet(
                const_cast<BsHevcVideoParameterSet*>(
                    static_cast<const BsHevcVideoParameterSet*>(e.data)
                )
            );
            delete const_cast<BsHevcVideoParameterSet*>(
                static_cast<const BsHevcVideoParameterSet*>(e.data)
            );
            break;

        case BS_STRUCT_HEVC_SPS:
            bs_free_BsHevcSequenceParameterSet(
                const_cast<BsHevcSequenceParameterSet*>(
                    static_cast<const BsHevcSequenceParameterSet*>(e.data)
                )
            );
            delete const_cast<BsHevcSequenceParameterSet*>(
                static_cast<const BsHevcSequenceParameterSet*>(e.data)
            );
            break;

        case BS_STRUCT_HEVC_PPS:
            bs_free_BsHevcPictureParameterSet(
                const_cast<BsHevcPictureParameterSet*>(
                    static_cast<const BsHevcPictureParameterSet*>(e.data)
                )
            );
            delete const_cast<BsHevcPictureParameterSet*>(
                static_cast<const BsHevcPictureParameterSet*>(e.data)
            );
            break;

        case BS_STRUCT_AVC_SPS:
            bs_free_BsAvcSequenceParameterSet(
                const_cast<BsAvcSequenceParameterSet*>(
                    static_cast<const BsAvcSequenceParameterSet*>(e.data)
                )
            );
            delete const_cast<BsAvcSequenceParameterSet*>(
                static_cast<const BsAvcSequenceParameterSet*>(e.data)
            );
            break;

        case BS_STRUCT_AVC_PPS:
            bs_free_BsAvcPictureParameterSet(
                const_cast<BsAvcPictureParameterSet*>(
                    static_cast<const BsAvcPictureParameterSet*>(e.data)
                )
            );
            delete const_cast<BsAvcPictureParameterSet*>(
                static_cast<const BsAvcPictureParameterSet*>(e.data)
            );
            break;

        case BS_STRUCT_VVC_DCI:
            bs_free_BsVvcDci(const_cast<BsVvcDci*>(static_cast<const BsVvcDci*>(e.data)));
            delete const_cast<BsVvcDci*>(static_cast<const BsVvcDci*>(e.data));
            break;

        case BS_STRUCT_VVC_OPI:
            bs_free_BsVvcOpi(const_cast<BsVvcOpi*>(static_cast<const BsVvcOpi*>(e.data)));
            delete const_cast<BsVvcOpi*>(static_cast<const BsVvcOpi*>(e.data));
            break;

        case BS_STRUCT_VVC_VPS:
            bs_free_BsVvcVideoParameterSet(
                const_cast<BsVvcVideoParameterSet*>(
                    static_cast<const BsVvcVideoParameterSet*>(e.data)
                )
            );
            delete const_cast<BsVvcVideoParameterSet*>(
                static_cast<const BsVvcVideoParameterSet*>(e.data)
            );
            break;

        case BS_STRUCT_VVC_SPS:
            bs_free_BsVvcSequenceParameterSet(
                const_cast<BsVvcSequenceParameterSet*>(
                    static_cast<const BsVvcSequenceParameterSet*>(e.data)
                )
            );
            delete const_cast<BsVvcSequenceParameterSet*>(
                static_cast<const BsVvcSequenceParameterSet*>(e.data)
            );
            break;

        case BS_STRUCT_VVC_PPS:
            bs_free_BsVvcPictureParameterSet(
                const_cast<BsVvcPictureParameterSet*>(
                    static_cast<const BsVvcPictureParameterSet*>(e.data)
                )
            );
            delete const_cast<BsVvcPictureParameterSet*>(
                static_cast<const BsVvcPictureParameterSet*>(e.data)
            );
            break;

        case BS_STRUCT_VVC_PH:
            bs_free_BsVvcPictureHeader(
                const_cast<BsVvcPictureHeader*>(static_cast<const BsVvcPictureHeader*>(e.data))
            );
            delete const_cast<BsVvcPictureHeader*>(static_cast<const BsVvcPictureHeader*>(e.data));
            break;

        case BS_STRUCT_VVC_SLICE:
            bs_free_BsVvcSliceHeader(
                const_cast<BsVvcSliceHeader*>(static_cast<const BsVvcSliceHeader*>(e.data))
            );
            delete const_cast<BsVvcSliceHeader*>(static_cast<const BsVvcSliceHeader*>(e.data));
            break;

        case BS_STRUCT_AV1_SEQUENCE_HEADER:
            bs_free_BsAv1SequenceHeader(
                const_cast<BsAv1SequenceHeader*>(static_cast<const BsAv1SequenceHeader*>(e.data))
            );
            delete const_cast<BsAv1SequenceHeader*>(
                static_cast<const BsAv1SequenceHeader*>(e.data)
            );
            break;

        case BS_STRUCT_AV1_FRAME_HEADER:
            bs_free_BsAv1FrameHeader(
                const_cast<BsAv1FrameHeader*>(static_cast<const BsAv1FrameHeader*>(e.data))
            );
            delete const_cast<BsAv1FrameHeader*>(static_cast<const BsAv1FrameHeader*>(e.data));
            break;

        case BS_STRUCT_VP9_FRAME_HEADER:
            bs_free_BsVp9FrameHeader(
                const_cast<BsVp9FrameHeader*>(static_cast<const BsVp9FrameHeader*>(e.data))
            );
            delete const_cast<BsVp9FrameHeader*>(static_cast<const BsVp9FrameHeader*>(e.data));
            break;

        case BS_STRUCT_VP8_FRAME_HEADER:
            bs_free_BsVp8FrameHeader(
                const_cast<BsVp8FrameHeader*>(static_cast<const BsVp8FrameHeader*>(e.data))
            );
            delete const_cast<BsVp8FrameHeader*>(static_cast<const BsVp8FrameHeader*>(e.data));
            break;
    }
}

}  // namespace

extern "C" {

BsState* bs_state_create(BsCodec codec) {
    if (codec == BS_CODEC_AUTO) {
        /*
         * The state has no buffer to probe, so an explicit codec is
         * required.  Use bs_parse_report with a NULL state for the
         * auto-detect path.
         */
        set_error(
            "bs_state_create: explicit codec required "
            "(HEVC/AVC/VVC/AV1/VP9/VP8); use bs_parse_report "
            "with a NULL state for auto-detect"
        );
        return nullptr;
    }

    try {
        return reinterpret_cast<BsState*>(new bs::State(to_codec(codec)));

    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

void bs_state_destroy(BsState* state) {
    delete reinterpret_cast<bs::State*>(state);
}

void bs_state_clear(BsState* state) {
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
    const BsNalHandlers* handlers
) {
    if (state == nullptr || data == nullptr || handlers == nullptr) {
        set_error("bs_parse: null argument");
        return -1;
    }

    try {
        bs::State* s = reinterpret_cast<bs::State*>(state);

        std::span<const std::uint8_t> span(data, size);

        g_handlers = handlers;
        g_data_start = data;

        const auto cpp_mode = to_mode(mode);
        long result = -1;

        switch (s->codec()) {
            case bs::Codec::Hevc: {
                bs::BsNalHandlers h{};
                h.vps = hevc_vps;
                h.sps = hevc_sps;
                h.pps = hevc_pps;
                h.prefix_sei = hevc_sei;
                h.suffix_sei = hevc_sei;
                h.slice = hevc_slice;
                h.unsupported = hevc_unsupported;

                const std::size_t n = bs::parse(*s, span, cpp_mode, h, length_size);
                result = static_cast<long>(n);
                break;
            }

            case bs::Codec::Avc: {
                bs::avc::NalHandlers h{};
                h.sps = avc_sps;
                h.pps = avc_pps;
                h.sei = avc_sei;
                h.slice = avc_slice;
                h.unsupported = avc_unsupported;

                const std::size_t n = bs::parse(*s, span, cpp_mode, h, length_size);
                result = static_cast<long>(n);
                break;
            }

            case bs::Codec::Vvc: {
                if (cpp_mode == bs::NalFramingMode::AnnexB) {
                    bs::AnnexBNalIterator framer{span};
                    result = static_cast<long>(walk_vvc_raw(framer, g_handlers));
                } else if (cpp_mode == bs::NalFramingMode::LengthPrefixed) {
                    bs::LengthPrefixedNalIterator framer{span, length_size};
                    result = static_cast<long>(walk_vvc_raw(framer, g_handlers));
                } else {
                    set_error(
                        "bs_parse: unsupported framing mode for VVC (Annex-B or length-prefixed)"
                    );
                    result = -1;
                }
                break;
            }

            case bs::Codec::Av1: {
                if (cpp_mode == bs::NalFramingMode::Obu) {
                    result = static_cast<long>(walk_av1_raw(span, g_handlers));
                } else {
                    set_error("bs_parse: unsupported framing mode for AV1 (OBU)");
                    result = -1;
                }
                break;
            }

            case bs::Codec::Vp9:
            case bs::Codec::Vp8: {
                if (cpp_mode == bs::NalFramingMode::Ivf) {
                    bs::IvfFramer framer{span};
                    result = static_cast<long>(walk_vp_frame_raw(framer, g_handlers));
                } else {
                    set_error("bs_parse: unsupported framing mode for VP9/VP8 (IVF)");
                    result = -1;
                }
                break;
            }
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
    BsState* state, const unsigned char* data, size_t size, BsFramingMode mode, unsigned length_size
) {
    if (data == nullptr) {
        set_error("bs_parse_report: null argument");
        return nullptr;
    }

    try {
        /*
         * With a state, use its codec.  With a NULL state, probe the
         * first NAL to auto-detect (the AUTO path).
         */
        const bs::Codec codec = (state != nullptr) ? reinterpret_cast<bs::State*>(state)->codec()
                                                   : detect_codec(data, size);

        std::unique_ptr<bs::State> owned;
        bs::State* s = nullptr;

        if (state != nullptr) {
            s = reinterpret_cast<bs::State*>(state);
        } else {
            owned = bs::create_state(codec);
            s = owned.get();
        }

        std::span<const std::uint8_t> span(data, size);

        g_entries.clear();
        g_collector_codec = codec;
        g_data_start = data;

        /*
         * The codec adaptors fire the catch-all `nal` callback for every unit,
         * so point only `nal` at the collector — one entry per NAL/OBU/frame.
         */
        BsNalHandlers h{};
        h.ctx = nullptr;
        h.nal = &collect;

        g_handlers = &h;

        /*
         * The auto-detect path (NULL state) selects the framing from the
         * detected codec; an explicit state uses the caller's mode.
         */
        const auto cpp_mode = (state == nullptr) ? default_framing(codec) : to_mode(mode);

        switch (codec) {
            case bs::Codec::Hevc: {
                bs::BsNalHandlers ch{};
                ch.vps = hevc_vps;
                ch.sps = hevc_sps;
                ch.pps = hevc_pps;
                ch.prefix_sei = hevc_sei;
                ch.suffix_sei = hevc_sei;
                ch.slice = hevc_slice;
                ch.unsupported = hevc_unsupported;
                (void)bs::parse(*s, span, cpp_mode, ch, length_size);
                break;
            }

            case bs::Codec::Avc: {
                bs::avc::NalHandlers ch{};
                ch.sps = avc_sps;
                ch.pps = avc_pps;
                ch.sei = avc_sei;
                ch.slice = avc_slice;
                ch.unsupported = avc_unsupported;
                (void)bs::parse(*s, span, cpp_mode, ch, length_size);
                break;
            }

            case bs::Codec::Vvc: {
                if (cpp_mode == bs::NalFramingMode::AnnexB) {
                    bs::AnnexBNalIterator framer{span};
                    (void)walk_vvc_raw(framer, g_handlers);
                } else if (cpp_mode == bs::NalFramingMode::LengthPrefixed) {
                    bs::LengthPrefixedNalIterator framer{span, length_size};
                    (void)walk_vvc_raw(framer, g_handlers);
                } else {
                    set_error("bs_parse_report: unsupported framing mode for VVC");
                }
                break;
            }

            case bs::Codec::Av1: {
                if (cpp_mode == bs::NalFramingMode::Obu) {
                    (void)walk_av1_raw(span, g_handlers);
                } else {
                    set_error("bs_parse_report: unsupported framing mode for AV1 (OBU)");
                }
                break;
            }

            case bs::Codec::Vp9:
            case bs::Codec::Vp8: {
                if (cpp_mode == bs::NalFramingMode::Ivf) {
                    bs::IvfFramer framer{span};
                    (void)walk_vp_frame_raw(framer, g_handlers);
                } else {
                    set_error("bs_parse_report: unsupported framing mode for VP9/VP8 (IVF)");
                }
                break;
            }
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
        report->codec = to_bs_codec(codec);
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

void bs_report_destroy(BsReport* report) {
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
    const BsHevcHandlers* handlers
) {
    if (state == nullptr || data == nullptr || handlers == nullptr) {
        set_error("bs_parse_hevc: null argument");
        return -1;
    }

    try {
        bs::State* s = reinterpret_cast<bs::State*>(state);

        if (s->codec() != bs::Codec::Hevc) {
            set_error("bs_parse_hevc: state is not an HEVC state");
            return -1;
        }

        std::span<const std::uint8_t> span(data, size);

        g_hevc_h = handlers;

        bs::HevcParsedHandlers h{};
        h.vps = hevc_typed_vps;
        h.sps = hevc_typed_sps;
        h.pps = hevc_typed_pps;
        h.sei = hevc_typed_sei;
        h.slice = hevc_typed_slice;

        const std::size_t n = bs::parse(*s, span, to_mode(mode), h, length_size);

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
    const BsAvcHandlers* handlers
) {
    if (state == nullptr || data == nullptr || handlers == nullptr) {
        set_error("bs_parse_avc: null argument");
        return -1;
    }

    try {
        bs::State* s = reinterpret_cast<bs::State*>(state);

        if (s->codec() != bs::Codec::Avc) {
            set_error("bs_parse_avc: state is not an AVC state");
            return -1;
        }

        std::span<const std::uint8_t> span(data, size);

        g_avc_h = handlers;

        bs::AvcParsedHandlers h{};
        h.sps = avc_typed_sps;
        h.pps = avc_typed_pps;
        h.sei = avc_typed_sei;
        h.slice = avc_typed_slice;

        const std::size_t n = bs::parse(*s, span, to_mode(mode), h, length_size);

        g_avc_h = nullptr;

        return static_cast<long>(n);

    } catch (const std::exception& e) {
        g_avc_h = nullptr;
        set_error(e.what());
        return -1;
    }
}

long bs_parse_vvc(
    BsState* state,
    const unsigned char* data,
    size_t size,
    BsFramingMode mode,
    unsigned length_size,
    const BsVvcHandlers* handlers
) {
    if (state == nullptr || data == nullptr || handlers == nullptr) {
        set_error("bs_parse_vvc: null argument");
        return -1;
    }

    try {
        bs::State* s = reinterpret_cast<bs::State*>(state);

        if (s->codec() != bs::Codec::Vvc) {
            set_error("bs_parse_vvc: state is not a VVC state");
            return -1;
        }

        std::span<const std::uint8_t> span(data, size);

        g_vvc_h = handlers;

        bs::VvcParsedHandlers h{};
        h.dci = vvc_typed_dci;
        h.opi = vvc_typed_opi;
        h.vps = vvc_typed_vps;
        h.sps = vvc_typed_sps;
        h.pps = vvc_typed_pps;
        h.ph = vvc_typed_ph;
        h.slice = vvc_typed_slice;

        const std::size_t n = bs::parse(*s, span, to_mode(mode), h, length_size);

        g_vvc_h = nullptr;

        return static_cast<long>(n);

    } catch (const std::exception& e) {
        g_vvc_h = nullptr;
        set_error(e.what());
        return -1;
    }
}

long bs_parse_av1(
    BsState* state,
    const unsigned char* data,
    size_t size,
    BsFramingMode mode,
    const BsAv1Handlers* handlers
) {
    if (state == nullptr || data == nullptr || handlers == nullptr) {
        set_error("bs_parse_av1: null argument");
        return -1;
    }

    try {
        bs::State* s = reinterpret_cast<bs::State*>(state);

        if (s->codec() != bs::Codec::Av1) {
            set_error("bs_parse_av1: state is not an AV1 state");
            return -1;
        }

        std::span<const std::uint8_t> span(data, size);

        g_av1_h = handlers;

        bs::Av1ParsedHandlers h{};
        h.sequence_header = av1_typed_sequence_header;
        h.frame_header = av1_typed_frame_header;

        const std::size_t n = bs::parse(*s, span, to_mode(mode), h, 4);

        g_av1_h = nullptr;

        return static_cast<long>(n);

    } catch (const std::exception& e) {
        g_av1_h = nullptr;
        set_error(e.what());
        return -1;
    }
}

long bs_parse_vp9(
    BsState* state,
    const unsigned char* data,
    size_t size,
    BsFramingMode mode,
    const BsVp9Handlers* handlers
) {
    if (state == nullptr || data == nullptr || handlers == nullptr) {
        set_error("bs_parse_vp9: null argument");
        return -1;
    }

    try {
        bs::State* s = reinterpret_cast<bs::State*>(state);

        if (s->codec() != bs::Codec::Vp9) {
            set_error("bs_parse_vp9: state is not a VP9 state");
            return -1;
        }

        std::span<const std::uint8_t> span(data, size);

        g_vp9_h = handlers;

        bs::Vp9ParsedHandlers h{};
        h.frame_header = vp9_typed_frame_header;

        const std::size_t n = bs::parse(*s, span, to_mode(mode), h, 4);

        g_vp9_h = nullptr;

        return static_cast<long>(n);

    } catch (const std::exception& e) {
        g_vp9_h = nullptr;
        set_error(e.what());
        return -1;
    }
}

long bs_parse_vp8(
    BsState* state,
    const unsigned char* data,
    size_t size,
    BsFramingMode mode,
    const BsVp8Handlers* handlers
) {
    if (state == nullptr || data == nullptr || handlers == nullptr) {
        set_error("bs_parse_vp8: null argument");
        return -1;
    }

    try {
        bs::State* s = reinterpret_cast<bs::State*>(state);

        if (s->codec() != bs::Codec::Vp8) {
            set_error("bs_parse_vp8: state is not a VP8 state");
            return -1;
        }

        std::span<const std::uint8_t> span(data, size);

        g_vp8_h = handlers;

        bs::Vp8ParsedHandlers h{};
        h.frame_header = vp8_typed_frame_header;

        const std::size_t n = bs::parse(*s, span, to_mode(mode), h, 4);

        g_vp8_h = nullptr;

        return static_cast<long>(n);

    } catch (const std::exception& e) {
        g_vp8_h = nullptr;
        set_error(e.what());
        return -1;
    }
}

BsStructReport* bs_parse_struct_report(
    BsState* state, const unsigned char* data, size_t size, BsFramingMode mode, unsigned length_size
) {
    if (data == nullptr) {
        set_error("bs_parse_struct_report: null argument");
        return nullptr;
    }

    try {
        const bs::Codec codec = (state != nullptr) ? reinterpret_cast<bs::State*>(state)->codec()
                                                   : detect_codec(data, size);

        std::unique_ptr<bs::State> owned;
        bs::State* s = nullptr;

        if (state != nullptr) {
            s = reinterpret_cast<bs::State*>(state);
        } else {
            owned = bs::create_state(codec);
            s = owned.get();
        }

        std::span<const std::uint8_t> span(data, size);

        g_struct_entries.clear();

        /*
         * The auto-detect path (NULL state) selects the framing from the
         * detected codec; an explicit state uses the caller's mode.
         */
        const auto cpp_mode = (state == nullptr) ? default_framing(codec) : to_mode(mode);

        switch (codec) {
            case bs::Codec::Hevc: {
                bs::HevcParsedHandlers hh{};
                hh.vps = hevc_report_vps;
                hh.sps = hevc_report_sps;
                hh.pps = hevc_report_pps;
                (void)bs::parse(*s, span, cpp_mode, hh, length_size);
                break;
            }

            case bs::Codec::Avc: {
                bs::AvcParsedHandlers ah{};
                ah.sps = avc_report_sps;
                ah.pps = avc_report_pps;
                (void)bs::parse(*s, span, cpp_mode, ah, length_size);
                break;
            }

            case bs::Codec::Vvc: {
                bs::VvcParsedHandlers vh{};
                vh.dci = vvc_report_dci;
                vh.opi = vvc_report_opi;
                vh.vps = vvc_report_vps;
                vh.sps = vvc_report_sps;
                vh.pps = vvc_report_pps;
                vh.ph = vvc_report_ph;
                vh.slice = vvc_report_slice;
                (void)bs::parse(*s, span, cpp_mode, vh, length_size);
                break;
            }

            case bs::Codec::Av1: {
                bs::Av1ParsedHandlers ah{};
                ah.sequence_header = av1_report_sequence_header;
                ah.frame_header = av1_report_frame_header;
                (void)bs::parse(*s, span, cpp_mode, ah, length_size);
                break;
            }

            case bs::Codec::Vp9: {
                bs::Vp9ParsedHandlers vh{};
                vh.frame_header = vp9_report_frame_header;
                (void)bs::parse(*s, span, cpp_mode, vh, length_size);
                break;
            }

            case bs::Codec::Vp8: {
                bs::Vp8ParsedHandlers vh{};
                vh.frame_header = vp8_report_frame_header;
                (void)bs::parse(*s, span, cpp_mode, vh, length_size);
                break;
            }
        }

        const std::size_t count = g_struct_entries.size();
        BsStructEntry* entries = new BsStructEntry[count];
        for (std::size_t i = 0; i < count; ++i) {
            entries[i] = g_struct_entries[i];
        }
        g_struct_entries.clear();

        BsStructReport* report = new BsStructReport{};
        report->codec = to_bs_codec(codec);
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

void bs_struct_report_destroy(BsStructReport* report) {
    if (report == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < report->count; ++i) {
        free_struct_entry(report->entries[i]);
    }
    delete[] report->entries;
    delete report;
}

const char* bs_get_last_error(void) {
    return g_last_error.c_str();
}

}  // extern "C"
