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
 * Codec selection.  BS_CODEC_AUTO probes the first NAL of the stream (only
 * meaningful for bs_parse_report, which accepts a NULL state).
 */
typedef enum { BS_CODEC_HEVC = 0, BS_CODEC_AVC = 1, BS_CODEC_AUTO = 2 } BsCodec;

/*
 * NAL framing / container.
 */
typedef enum { BS_FRAMING_ANNEX_B = 0, BS_FRAMING_LENGTH_PREFIXED = 1 } BsFramingMode;

/*
 * Opaque parser state.  Created/destroyed by the library; never inspected
 * by the caller.
 */
typedef struct BsState BsState;

/*
 * Non-owning view of one NAL unit, passed to every callback.  This mirrors
 * bs::NalUnit / bs::avc::NalUnit: the payload points directly into the input
 * buffer and is only valid for the duration of the callback (zero-copy).
 *
 *   nal_unit_type         codec NAL type id
 *   nuh_layer_id          HEVC layer id (0 for AVC)
 *   nuh_temporal_id_plus1 HEVC temporal id + 1 (0 for AVC)
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
 */
typedef struct BsNalHandlers {
    void* ctx;
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
 * Create / destroy the opaque state.  An explicit codec (HEVC/AVC) is required;
 * use bs_parse_report with a NULL state for the auto-detect path.
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
 * A NULL state auto-detects the codec from the first NAL (the AUTO path);
 * an explicit state fixes the codec.
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
 * parsed parameter-set structs (VPS/SPS/PPS) with all nested sub-structs
 * intact, exactly mirroring bs::HevcParsedHandlers / bs::AvcParsedHandlers.
 *
 * Each typed callback receives an owned C struct (BsHevc* / BsAvc*) that is
 * valid only for the duration of that callback.  Copy any fields you need to
 * keep; the library frees the struct (and its heap buffers) once the callback
 * returns.  `ctx` is passed verbatim to every callback.  Any NULL callback
 * slot is skipped.
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
 * Collected, value-copied snapshot of every parameter set seen during a parse
 * (the C-side equivalent of bs::StructReport).  `entries` points at an array
 * of `count` items; each `data` pointer addresses a heap-allocated BsHevc* /
 * BsAvc* struct owned by this report.  Free the whole report with
 * bs_struct_report_destroy — do not free individual entries yourself.
 */
typedef enum {
    BS_STRUCT_HEVC_VPS,
    BS_STRUCT_HEVC_SPS,
    BS_STRUCT_HEVC_PPS,
    BS_STRUCT_AVC_SPS,
    BS_STRUCT_AVC_PPS
} BsStructKind;

typedef struct BsStructEntry {
    BsStructKind kind;
    const void* data; /* a heap-allocated BsHevc* / BsAvc* struct */
} BsStructEntry;

typedef struct BsStructReport {
    BsCodec codec;
    size_t count;
    BsStructEntry* entries;
} BsStructReport;

/*
 * Parse a buffer and collect a StructReport instead of invoking callbacks.
 * Returns NULL on error.
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
