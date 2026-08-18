#ifndef BS_CAPI_H
#define BS_CAPI_H

/*
 * ===========================================================================
 * bsparser — C public API
 * ===========================================================================
 *
 * A stable C ABI over the C++20 header-only core.  Compile this layer into a
 * STATIC or SHARED library and consume it from C, or from any toolchain /
 * language that cannot build the C++20 templates directly.
 *
 * Rules of the ABI:
 *
 *   * Everything is plain C: opaque handles, integer enums, `size_t`, and
 *     `char*` strings (only the last-error message).  No C++ types,
 *     exceptions, or STL cross the boundary.
 *   * Structured data is delivered through plain C structs (see BsNalUnit and
 *     BsReport) rather than serialised strings, so usage mirrors the C++
 *     handler-based API: callbacks receive a BsNalUnit view, and bs_parse_report
 *     returns a BsReport of BsNalEntry structs.
 *   * Resources returned by the API (BsState*, BsReport*) are freed by the
 *     matching API call (bs_state_destroy / bs_report_destroy).
 *   * On error, bs_parse returns -1 and bs_parse_report returns NULL; the
 *     human-readable message is retrieved with bs_get_last_error().
 */

#include <stddef.h>

/*
 * The C mirror of the parsed parameter-set structs (BsHevc* / BsAvc*).  These
 * are the types delivered by the typed callbacks and the struct report below.
 * All heap buffers they point at (vectors/arrays) are owned by the library and
 * freed per the rules documented with each delivery mechanism.
 *
 * Included outside the extern "C" block; in C++ it pulls <cstddef>/<cstdint>
 * (and <type_traits> for the STL) so the first C++ header in this TU is valid.
 */
#ifdef __cplusplus
#include <cstddef>
#include <cstdint>
#include <type_traits>
#endif
#include "bs_structs.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Codec selection.  BS_CODEC_AUTO probes the first unit of the stream (only
 * meaningful for bs_parse_report, which accepts a NULL state).
 */
typedef enum {
    BS_CODEC_HEVC = 0,
    BS_CODEC_AVC = 1,
    BS_CODEC_AUTO = 2,
    BS_CODEC_VVC = 3,
    BS_CODEC_AV1 = 4,
    BS_CODEC_VP9 = 5,
    BS_CODEC_VP8 = 6
} BsCodec;

/*
 * NAL / OBU / frame framing.
 */
typedef enum {
    BS_FRAMING_ANNEX_B = 0,
    BS_FRAMING_LENGTH_PREFIXED = 1,
    BS_FRAMING_OBU = 2,
    BS_FRAMING_IVF = 3
} BsFramingMode;

/*
 * Opaque parser state.  Created/destroyed by the library; never inspected
 * by the caller.
 */
typedef struct BsState BsState;

/*
 * Non-owning view of one NAL unit / OBU / frame, passed to every callback.
 * This mirrors bs::NalUnit / bs::avc::NalUnit: the payload points directly
 * into the input buffer and is only valid for the duration of the callback
 * (zero-copy).
 *
 *   nal_unit_type         codec NAL type id (HEVC/AVC/VVC), OBU type (AV1),
 *                         0 for VP9/VP8 frames
 *   nuh_layer_id          HEVC/VVC layer id (0 for AVC)
 *   nuh_temporal_id_plus1 HEVC/VVC temporal id + 1 (0 for AVC/AV1/VP9/VP8)
 *   forbidden_zero_bit    0/1
 *   is_vcl                1 if this is a VCL (slice) NAL, else 0
 *   payload               EBSP bytes (emulation-prevention still present)
 *   payload_size           number of bytes in payload
 *   offset                 byte offset of payload within the input buffer
 */
typedef struct BsNalUnit {
    int nal_unit_type;
    int nuh_layer_id;
    int nuh_temporal_id_plus1;
    int forbidden_zero_bit;
    int is_vcl;
    const unsigned char* payload;
    size_t payload_size;
    size_t offset;
} BsNalUnit;

/*
 * Per-NAL callback invoked by bs_parse.  Mirrors the C++ handler callbacks,
 * which receive a const NalUnit&.
 */
typedef void (*BsNalCallback)(void* ctx, const BsNalUnit* nal);

/*
 * Callback set for bs_parse.  Any member may be NULL; NULL callbacks are
 * simply not invoked.  `ctx` is passed verbatim to every callback.
 *
 * `nal` is a catch-all: it fires for every NAL / OBU / frame, regardless of
 * type, on every codec (HEVC/AVC deliver it in addition to the typed slots;
 * VVC maps to vps/sps/pps/sei/slice/unsupported by NAL type; AV1/VP9/VP8
 * deliver every unit through `nal`).
 */
typedef struct BsNalHandlers {
    void* ctx;
    BsNalCallback nal; /* every unit, all codecs */
    BsNalCallback vps; /* HEVC only; NULL-safe for AVC */
    BsNalCallback sps;
    BsNalCallback pps;
    BsNalCallback sei; /* HEVC prefix/suffix SEI and AVC SEI */
    BsNalCallback slice;
    BsNalCallback unsupported;
} BsNalHandlers;

/*
 * One NAL unit in a parsed report (mirrors bs::cli::NalEntry).  nal_type_name
 * points at a static string owned by the library — do not free it.
 */
typedef struct BsNalEntry {
    size_t index;
    size_t offset;
    int nal_unit_type;
    const char* nal_type_name;
    int is_vcl;
    size_t size;
} BsNalEntry;

/*
 * Whole-stream report returned by bs_parse_report.  `nals` points at an array
 * of `nal_count` entries; free the whole report with bs_report_destroy.
 */
typedef struct BsReport {
    BsCodec codec;
    size_t nal_count;
    size_t vcl_count;
    BsNalEntry* nals;
} BsReport;

/*
 * Create / destroy the opaque state.  An explicit codec is required; use
 * bs_parse_report with a NULL state for the auto-detect path.
 */
BsState* bs_state_create(BsCodec codec);
void bs_state_destroy(BsState* state);

/*
 * Drop all stored parameter sets (use before parsing a new, independent
 * stream into the same state).
 */
void bs_state_clear(BsState* state);

/*
 * Frame + parse a buffer, dispatching each NAL to the matching callback.
 *
 * Returns the number of NALs parsed, or -1 on error.
 */
long bs_parse(
    BsState* state,
    const unsigned char* data,
    size_t size,
    BsFramingMode mode,
    unsigned length_size,
    const BsNalHandlers* handlers
);

/*
 * Parse a buffer and return a structured report (an array of BsNalEntry).
 * The caller owns the report and must release it with bs_report_destroy().
 *
 * A NULL state auto-detects the codec from the stream and selects the framing
 * for it (Annex-B for HEVC/AVC/VVC, OBU for AV1, IVF for VP9/VP8); an
 * explicit state fixes both codec and framing.
 *
 * Returns NULL on error.
 */
BsReport* bs_parse_report(
    BsState* state, const unsigned char* data, size_t size, BsFramingMode mode, unsigned length_size
);

/*
 * Release a report previously returned by bs_parse_report.
 */
void bs_report_destroy(BsReport* report);

/*
 * ===========================================================================
 * Typed parameter-set delivery
 * ===========================================================================
 *
 * In addition to the raw-NAL callbacks above, the C API can deliver the fully
 * parsed parameter-set structs (VPS/SPS/PPS, VVC DCI/OPI/PH, AV1 sequence and
 * frame headers, VP9/VP8 frame headers) with all nested sub-structs intact,
 * exactly mirroring bs::HevcParsedHandlers, bs::AvcParsedHandlers,
 * bs::VvcParsedHandlers, bs::Av1ParsedHandlers, bs::Vp9ParsedHandlers and
 * bs::Vp8ParsedHandlers.
 *
 * Each typed callback receives an owned C struct (BsHevc* / BsAvc* /
 * BsVvc* / BsAv1* / BsVp9* / BsVp8*) that is valid only for the duration of
 * that callback.  Copy any fields you need to keep; the library frees the
 * struct (and its heap buffers) once the callback returns.  `ctx` is passed
 * verbatim to every callback.  Any NULL callback slot is skipped.
 */
typedef void (*BsHevcVpsCallback)(void* ctx, const BsHevcVideoParameterSet* vps);
typedef void (*BsHevcSpsCallback)(void* ctx, const BsHevcSequenceParameterSet* sps);
typedef void (*BsHevcPpsCallback)(void* ctx, const BsHevcPictureParameterSet* pps);
typedef void (*BsAvcSpsCallback)(void* ctx, const BsAvcSequenceParameterSet* sps);
typedef void (*BsAvcPpsCallback)(void* ctx, const BsAvcPictureParameterSet* pps);

/*
 * SEI delivery.  Unlike the parameter sets, a SEI NAL carries zero or more
 * messages of heterogeneous payload types; the C struct mirror would be a
 * view into the RBSP buffer, so instead each message is delivered on its own
 * with its raw payload type and bytes (payload points into the input buffer,
 * valid only for the duration of the callback).
 */
typedef void (*BsSeiCallback)(
    void* ctx, unsigned int payload_type, const unsigned char* payload, size_t payload_size
);

/*
 * Slice headers: delivered as the fully-parsed mirror struct (owned by the
 * library, valid only for the duration of the call).  Like the parameter
 * sets, the library resolves the referenced SPS/PPS from its manager before
 * parsing and skips the slice if resolution fails.
 */
typedef void (*BsHevcSliceCallback)(void* ctx, const BsHevcSliceSegmentHeader* hdr);
typedef void (*BsAvcSliceCallback)(void* ctx, const BsAvcSliceHeader* hdr);

typedef struct BsHevcHandlers {
    void* ctx;
    BsHevcVpsCallback vps;
    BsHevcSpsCallback sps;
    BsHevcPpsCallback pps;
    BsSeiCallback sei;
    BsHevcSliceCallback slice;
} BsHevcHandlers;

typedef struct BsAvcHandlers {
    void* ctx;
    BsAvcSpsCallback sps;
    BsAvcPpsCallback pps;
    BsSeiCallback sei;
    BsAvcSliceCallback slice;
} BsAvcHandlers;

/*
 * Frame + parse a buffer, dispatching each parsed parameter set to the
 * matching typed callback.  Returns the number of NALs parsed, or -1 on error.
 */
long bs_parse_hevc(
    BsState* state,
    const unsigned char* data,
    size_t size,
    BsFramingMode mode,
    unsigned length_size,
    const BsHevcHandlers* handlers
);

long bs_parse_avc(
    BsState* state,
    const unsigned char* data,
    size_t size,
    BsFramingMode mode,
    unsigned length_size,
    const BsAvcHandlers* handlers
);

/*
 * Typed VVC delivery.  `mode` must be Annex-B or length-prefixed.
 */
typedef void (*BsVvcDciCallback)(void* ctx, const BsVvcDci* dci);
typedef void (*BsVvcOpiCallback)(void* ctx, const BsVvcOpi* opi);
typedef void (*BsVvcVpsCallback)(void* ctx, const BsVvcVideoParameterSet* vps);
typedef void (*BsVvcSpsCallback)(void* ctx, const BsVvcSequenceParameterSet* sps);
typedef void (*BsVvcPpsCallback)(void* ctx, const BsVvcPictureParameterSet* pps);
typedef void (*BsVvcPhCallback)(void* ctx, const BsVvcPictureHeader* ph);
typedef void (*BsVvcSliceCallback)(void* ctx, const BsVvcSliceHeader* hdr);

typedef struct BsVvcHandlers {
    void* ctx;
    BsVvcDciCallback dci;
    BsVvcOpiCallback opi;
    BsVvcVpsCallback vps;
    BsVvcSpsCallback sps;
    BsVvcPpsCallback pps;
    BsVvcPhCallback ph;
    BsVvcSliceCallback slice;
} BsVvcHandlers;

/*
 * Typed AV1 delivery.  `mode` must be OBU.
 */
typedef void (*BsAv1SequenceHeaderCallback)(void* ctx, const BsAv1SequenceHeader* sh);
typedef void (*BsAv1FrameHeaderCallback)(void* ctx, const BsAv1FrameHeader* fh);

typedef struct BsAv1Handlers {
    void* ctx;
    BsAv1SequenceHeaderCallback sequence_header;
    BsAv1FrameHeaderCallback frame_header;
} BsAv1Handlers;

/*
 * Typed VP9 / VP8 delivery.  `mode` must be IVF.
 */
typedef void (*BsVp9FrameHeaderCallback)(void* ctx, const BsVp9FrameHeader* fh);
typedef void (*BsVp8FrameHeaderCallback)(void* ctx, const BsVp8FrameHeader* fh);

typedef struct BsVp9Handlers {
    void* ctx;
    BsVp9FrameHeaderCallback frame_header;
} BsVp9Handlers;

typedef struct BsVp8Handlers {
    void* ctx;
    BsVp8FrameHeaderCallback frame_header;
} BsVp8Handlers;

/*
 * Frame + parse a buffer, dispatching each parsed parameter set / header to
 * the matching typed callback.  Returns the number of units parsed, or -1 on
 * error.
 */
long bs_parse_vvc(
    BsState* state,
    const unsigned char* data,
    size_t size,
    BsFramingMode mode,
    unsigned length_size,
    const BsVvcHandlers* handlers
);

long bs_parse_av1(
    BsState* state,
    const unsigned char* data,
    size_t size,
    BsFramingMode mode,
    const BsAv1Handlers* handlers
);

long bs_parse_vp9(
    BsState* state,
    const unsigned char* data,
    size_t size,
    BsFramingMode mode,
    const BsVp9Handlers* handlers
);

long bs_parse_vp8(
    BsState* state,
    const unsigned char* data,
    size_t size,
    BsFramingMode mode,
    const BsVp8Handlers* handlers
);

/*
 * Collected, value-copied snapshot of every parameter set seen during a parse
 * (the C-side equivalent of bs::StructReport).  `entries` points at an array
 * of `count` items; each `data` pointer addresses a heap-allocated
 * BsHevc* / BsAvc* / BsVvc* / BsAv1* / BsVp9* / BsVp8* struct owned by this
 * report.  Free the whole report with bs_struct_report_destroy — do not free
 * individual entries yourself.
 */
typedef enum {
    BS_STRUCT_HEVC_VPS,
    BS_STRUCT_HEVC_SPS,
    BS_STRUCT_HEVC_PPS,
    BS_STRUCT_AVC_SPS,
    BS_STRUCT_AVC_PPS,
    BS_STRUCT_VVC_DCI,
    BS_STRUCT_VVC_OPI,
    BS_STRUCT_VVC_VPS,
    BS_STRUCT_VVC_SPS,
    BS_STRUCT_VVC_PPS,
    BS_STRUCT_VVC_PH,
    BS_STRUCT_VVC_SLICE,
    BS_STRUCT_AV1_SEQUENCE_HEADER,
    BS_STRUCT_AV1_FRAME_HEADER,
    BS_STRUCT_VP9_FRAME_HEADER,
    BS_STRUCT_VP8_FRAME_HEADER
} BsStructKind;

typedef struct BsStructEntry {
    BsStructKind kind;
    const void* data; /* a heap-allocated BsHevc* / BsAvc* / BsVvc* /
                         BsAv1* / BsVp9* / BsVp8* struct */
} BsStructEntry;

typedef struct BsStructReport {
    BsCodec codec;
    size_t count;
    BsStructEntry* entries;
} BsStructReport;

/*
 * Parse a buffer and collect a StructReport instead of invoking callbacks.
 * Framing follows the codec as with bs_parse_report (NULL state auto-detects
 * both codec and framing).  Returns NULL on error.
 */
BsStructReport* bs_parse_struct_report(
    BsState* state, const unsigned char* data, size_t size, BsFramingMode mode, unsigned length_size
);

/*
 * Release a report previously returned by bs_parse_struct_report.
 */
void bs_struct_report_destroy(BsStructReport* report);

/*
 * Last error message (valid until the next API call on the same thread).
 */
const char* bs_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* BS_CAPI_H */
