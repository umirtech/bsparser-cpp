/*
 * ---------------------------------------------------------------------------
 * C API smoke test (compiled as C, links the C++ runtime via bs_capi)
 * ---------------------------------------------------------------------------
 *
 * Demonstrates consuming the library from plain C: create a state, run a
 * callback dispatch over BsNalUnit structs, and request a structured report
 * (auto-detecting the codec).
 */

#include <bsparser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_vcl_count = 0;
static int g_sps_count = 0;

static void on_nal(void* ctx, const BsNalUnit* nal) {
    (void)ctx;

    if (nal->is_vcl) {
        ++g_vcl_count;
    }
}

static void on_sps(void* ctx, const BsNalUnit* nal) {
    (void)ctx;
    (void)nal;

    ++g_sps_count;
}

/* Typed parameter-set callbacks (C mirrors of the parsed structs). */
static int g_hevc_vps = 0, g_hevc_sps = 0, g_hevc_pps = 0;
static int g_avc_sps = 0, g_avc_pps = 0;

static void on_hevc_vps(void* ctx, const BsHevcVideoParameterSet* vps) {
    (void)ctx;
    (void)vps;
    ++g_hevc_vps;
}

static void on_hevc_sps(void* ctx, const BsHevcSequenceParameterSet* sps) {
    (void)ctx;
    (void)sps;
    ++g_hevc_sps;
}

static void on_hevc_pps(void* ctx, const BsHevcPictureParameterSet* pps) {
    (void)ctx;
    (void)pps;
    ++g_hevc_pps;
}

static void on_avc_sps(void* ctx, const BsAvcSequenceParameterSet* sps) {
    (void)ctx;
    (void)sps;
    ++g_avc_sps;
}

static void on_avc_pps(void* ctx, const BsAvcPictureParameterSet* pps) {
    (void)ctx;
    (void)pps;
    ++g_avc_pps;
}

/* SEI messages: delivered per-message with raw payload type + bytes. */
static int g_hevc_sei_msgs = 0;
static unsigned int g_hevc_sei_first_type = 0;

static void on_hevc_sei(
    void* ctx, unsigned int payload_type, const unsigned char* payload, size_t payload_size
) {
    (void)ctx;
    (void)payload;
    (void)payload_size;
    if (g_hevc_sei_msgs == 0) {
        g_hevc_sei_first_type = payload_type;
    }
    ++g_hevc_sei_msgs;
}

/* Slice headers: count + capture slice_type of the first one. */
static int g_hevc_slices = 0;
static int g_avc_slices = 0;

static void on_hevc_slice(void* ctx, const BsHevcSliceSegmentHeader* hdr) {
    (void)ctx;
    (void)hdr;
    ++g_hevc_slices;
}

static void on_avc_slice(void* ctx, const BsAvcSliceHeader* hdr) {
    (void)ctx;
    (void)hdr;
    ++g_avc_slices;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <stream>\n", argv[0]);
        return 1;
    }

    FILE* in = fopen(argv[1], "rb");
    if (in == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }

    fseek(in, 0, SEEK_END);
    long len = ftell(in);
    fseek(in, 0, SEEK_SET);

    if (len < 0) {
        fclose(in);
        return 1;
    }

    unsigned char* buf = (unsigned char*)malloc((size_t)len);
    if (buf == NULL) {
        fclose(in);
        return 1;
    }

    size_t got = fread(buf, 1, (size_t)len, in);
    fclose(in);

    /*
     * Determine the codec up front via the auto-detect report, then build a
     * matching state so the callback dispatch runs against the right codec.
     */
    BsReport* probe = bs_parse_report(NULL, buf, got, BS_FRAMING_ANNEX_B, 4);
    if (probe == NULL) {
        fprintf(stderr, "probe failed: %s\n", bs_get_last_error());
        free(buf);
        return 1;
    }
    const BsCodec codec = probe->codec;
    bs_report_destroy(probe);

    /*
     * Callback dispatch path: callbacks receive a BsNalUnit view (mirrors the
     * C++ NalUnit handlers).
     */
    BsState* state = bs_state_create(codec);
    if (state == NULL) {
        fprintf(stderr, "create failed: %s\n", bs_get_last_error());
        free(buf);
        return 1;
    }

    BsNalHandlers handlers;
    memset(&handlers, 0, sizeof(handlers));
    handlers.ctx = NULL;
    handlers.sps = on_sps;
    handlers.slice = on_nal;

    long parsed = bs_parse(state, buf, got, BS_FRAMING_ANNEX_B, 4, &handlers);

    if (parsed < 0) {
        fprintf(stderr, "parse failed: %s\n", bs_get_last_error());
        bs_state_destroy(state);
        free(buf);
        return 1;
    }

    printf(
        "[c-test] callback dispatch: parsed=%ld vcl=%d sps=%d\n", parsed, g_vcl_count, g_sps_count
    );

    bs_state_destroy(state);

    /*
     * Typed parameter-set callbacks: the struct mirrors are delivered with all
     * nested sub-structs intact.  A state matching the codec is required.
     */
    BsState* tstate = bs_state_create(codec);
    if (codec == BS_CODEC_HEVC) {
        BsHevcHandlers th;
        memset(&th, 0, sizeof(th));
        th.vps = on_hevc_vps;
        th.sps = on_hevc_sps;
        th.pps = on_hevc_pps;
        th.sei = on_hevc_sei;
        th.slice = on_hevc_slice;
        bs_parse_hevc(tstate, buf, got, BS_FRAMING_ANNEX_B, 4, &th);
    } else {
        BsAvcHandlers th;
        memset(&th, 0, sizeof(th));
        th.sps = on_avc_sps;
        th.pps = on_avc_pps;
        th.slice = on_avc_slice;
        bs_parse_avc(tstate, buf, got, BS_FRAMING_ANNEX_B, 4, &th);
    }
    bs_state_destroy(tstate);

    printf(
        "[c-test] typed callbacks: hevc vps=%d sps=%d pps=%d sei_msgs=%d (first type=%u) slices=%d "
        "| avc sps=%d pps=%d slices=%d\n",
        g_hevc_vps,
        g_hevc_sps,
        g_hevc_pps,
        g_hevc_sei_msgs,
        g_hevc_sei_first_type,
        g_hevc_slices,
        g_avc_sps,
        g_avc_pps,
        g_avc_slices
    );

    /*
     * Collected struct report (C-side equivalent of bs::StructReport).
     */
    BsStructReport* sr = bs_parse_struct_report(NULL, buf, got, BS_FRAMING_ANNEX_B, 4);
    if (sr == NULL) {
        fprintf(stderr, "struct report failed: %s\n", bs_get_last_error());
        free(buf);
        return 1;
    }
    printf("[c-test] struct report: codec=%d count=%zu\n", (int)sr->codec, sr->count);

    for (size_t i = 0; i < sr->count; ++i) {
        const BsStructEntry* e = &sr->entries[i];
        switch (e->kind) {
            case BS_STRUCT_HEVC_VPS:
                printf(
                    "  VPS id=%u\n",
                    ((const BsHevcVideoParameterSet*)e->data)->vps_video_parameter_set_id
                );
                break;

            case BS_STRUCT_HEVC_SPS:
                printf(
                    "  HEVC SPS w=%u h=%u\n",
                    ((const BsHevcSequenceParameterSet*)e->data)->pic_width_in_luma_samples,
                    ((const BsHevcSequenceParameterSet*)e->data)->pic_height_in_luma_samples
                );
                break;

            case BS_STRUCT_HEVC_PPS:
                printf(
                    "  HEVC PPS id=%u\n",
                    ((const BsHevcPictureParameterSet*)e->data)->pps_pic_parameter_set_id
                );
                break;

            case BS_STRUCT_AVC_SPS:
                printf(
                    "  AVC SPS id=%u\n",
                    ((const BsAvcSequenceParameterSet*)e->data)->seq_parameter_set_id
                );
                break;

            case BS_STRUCT_AVC_PPS:
                printf(
                    "  AVC PPS id=%u\n",
                    ((const BsAvcPictureParameterSet*)e->data)->pic_parameter_set_id
                );
                break;
        }
    }
    bs_struct_report_destroy(sr);

    /*
     * Structured report path: auto-detect codec via NULL state.  The
     * report is a BsReport of BsNalEntry structs (no JSON).
     */
    BsReport* report = bs_parse_report(NULL, buf, got, BS_FRAMING_ANNEX_B, 4);

    if (report == NULL) {
        fprintf(stderr, "report failed: %s\n", bs_get_last_error());
        free(buf);
        return 1;
    }

    printf(
        "[c-test] report: codec=%d nals=%zu vcl=%zu\n",
        (int)report->codec,
        report->nal_count,
        report->vcl_count
    );

    for (size_t i = 0; i < report->nal_count; ++i) {
        const BsNalEntry* e = &report->nals[i];
        printf(
            "  #%zu off=0x%zx type=%s(%d) vcl=%d size=%zu\n",
            e->index,
            e->offset,
            e->nal_type_name,
            e->nal_unit_type,
            e->is_vcl,
            e->size
        );
    }

    bs_report_destroy(report);

    free(buf);
    return 0;
}
