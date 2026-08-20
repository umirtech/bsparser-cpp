// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * ---------------------------------------------------------------------------
 * C API smoke test (compiled as C, links the C++ runtime via bs_capi)
 * ---------------------------------------------------------------------------
 *
 * Demonstrates consuming the library from plain C: create a state, run a
 * callback dispatch over BsNalUnit structs, and request a structured report
 * (auto-detecting the codec).  Works for every codec (HEVC/AVC/VVC/AV1/
 * VP9/VP8); the framing mode is chosen from the probed codec.
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

/* VVC typed callbacks. */
static int g_vvc_dci = 0, g_vvc_opi = 0, g_vvc_vps = 0, g_vvc_sps = 0;
static int g_vvc_pps = 0, g_vvc_ph = 0, g_vvc_slices = 0;

static void on_vvc_dci(void* ctx, const BsVvcDci* dci) {
    (void)ctx;
    (void)dci;
    ++g_vvc_dci;
}

static void on_vvc_opi(void* ctx, const BsVvcOpi* opi) {
    (void)ctx;
    (void)opi;
    ++g_vvc_opi;
}

static void on_vvc_vps(void* ctx, const BsVvcVideoParameterSet* vps) {
    (void)ctx;
    (void)vps;
    ++g_vvc_vps;
}

static void on_vvc_sps(void* ctx, const BsVvcSequenceParameterSet* sps) {
    (void)ctx;
    (void)sps;
    ++g_vvc_sps;
}

static void on_vvc_pps(void* ctx, const BsVvcPictureParameterSet* pps) {
    (void)ctx;
    (void)pps;
    ++g_vvc_pps;
}

static void on_vvc_ph(void* ctx, const BsVvcPictureHeader* ph) {
    (void)ctx;
    (void)ph;
    ++g_vvc_ph;
}

static void on_vvc_slice(void* ctx, const BsVvcSliceHeader* hdr) {
    (void)ctx;
    (void)hdr;
    ++g_vvc_slices;
}

/* AV1 / VP9 / VP8 typed callbacks. */
static int g_av1_sh = 0, g_av1_fh = 0;
static int g_vp9_fh = 0, g_vp8_fh = 0;

static void on_av1_sequence_header(void* ctx, const BsAv1SequenceHeader* sh) {
    (void)ctx;
    (void)sh;
    ++g_av1_sh;
}

static void on_av1_frame_header(void* ctx, const BsAv1FrameHeader* fh) {
    (void)ctx;
    (void)fh;
    ++g_av1_fh;
}

static void on_vp9_frame_header(void* ctx, const BsVp9FrameHeader* fh) {
    (void)ctx;
    (void)fh;
    ++g_vp9_fh;
}

static void on_vp8_frame_header(void* ctx, const BsVp8FrameHeader* fh) {
    (void)ctx;
    (void)fh;
    ++g_vp8_fh;
}

/* Pick the framing the given codec is carried in. */
static BsFramingMode framing_for(BsCodec codec) {
    switch (codec) {
        case BS_CODEC_AV1:
            return BS_FRAMING_OBU;
        case BS_CODEC_VP9:
        case BS_CODEC_VP8:
            return BS_FRAMING_IVF;
        case BS_CODEC_VVC:
        case BS_CODEC_HEVC:
        case BS_CODEC_AVC:
        default:
            return BS_FRAMING_ANNEX_B;
    }
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
     * (Detection sniffs the stream bytes, so the Annex-B probe finds the
     * codec even for OBU/IVF inputs.)
     */
    BsReport* probe = bs_parse_report(NULL, buf, got, BS_FRAMING_ANNEX_B, 4);
    if (probe == NULL) {
        fprintf(stderr, "probe failed: %s\n", bs_get_last_error());
        free(buf);
        return 1;
    }
    BsCodec codec = probe->codec;
    bs_report_destroy(probe);

    /* An explicit codec (argv[2]) overrides auto-detection; VCL-only streams
     * are genuinely ambiguous, so deterministic tests pass the codec. */
    if (argc > 2) {
        if (strcmp(argv[2], "hevc") == 0) {
            codec = BS_CODEC_HEVC;
        } else if (strcmp(argv[2], "avc") == 0) {
            codec = BS_CODEC_AVC;
        } else if (strcmp(argv[2], "vvc") == 0) {
            codec = BS_CODEC_VVC;
        } else if (strcmp(argv[2], "av1") == 0) {
            codec = BS_CODEC_AV1;
        } else if (strcmp(argv[2], "vp9") == 0) {
            codec = BS_CODEC_VP9;
        } else if (strcmp(argv[2], "vp8") == 0) {
            codec = BS_CODEC_VP8;
        }
    }

    const BsFramingMode fm = framing_for(codec);

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

    long parsed = bs_parse(state, buf, got, fm, 4, &handlers);

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
    switch (codec) {
        case BS_CODEC_HEVC: {
            BsHevcHandlers th;
            memset(&th, 0, sizeof(th));
            th.vps = on_hevc_vps;
            th.sps = on_hevc_sps;
            th.pps = on_hevc_pps;
            th.sei = on_hevc_sei;
            th.slice = on_hevc_slice;
            bs_parse_hevc(tstate, buf, got, fm, 4, &th);
            break;
        }

        case BS_CODEC_AVC: {
            BsAvcHandlers th;
            memset(&th, 0, sizeof(th));
            th.sps = on_avc_sps;
            th.pps = on_avc_pps;
            th.slice = on_avc_slice;
            bs_parse_avc(tstate, buf, got, fm, 4, &th);
            break;
        }

        case BS_CODEC_VVC: {
            BsVvcHandlers th;
            memset(&th, 0, sizeof(th));
            th.dci = on_vvc_dci;
            th.opi = on_vvc_opi;
            th.vps = on_vvc_vps;
            th.sps = on_vvc_sps;
            th.pps = on_vvc_pps;
            th.ph = on_vvc_ph;
            th.slice = on_vvc_slice;
            bs_parse_vvc(tstate, buf, got, fm, 4, &th);
            break;
        }

        case BS_CODEC_AV1: {
            BsAv1Handlers th;
            memset(&th, 0, sizeof(th));
            th.sequence_header = on_av1_sequence_header;
            th.frame_header = on_av1_frame_header;
            bs_parse_av1(tstate, buf, got, fm, &th);
            break;
        }

        case BS_CODEC_VP9: {
            BsVp9Handlers th;
            memset(&th, 0, sizeof(th));
            th.frame_header = on_vp9_frame_header;
            bs_parse_vp9(tstate, buf, got, fm, &th);
            break;
        }

        case BS_CODEC_VP8: {
            BsVp8Handlers th;
            memset(&th, 0, sizeof(th));
            th.frame_header = on_vp8_frame_header;
            bs_parse_vp8(tstate, buf, got, fm, &th);
            break;
        }

        default:
            break;
    }
    bs_state_destroy(tstate);

    printf(
        "[c-test] typed callbacks: hevc vps=%d sps=%d pps=%d sei_msgs=%d (first type=%u) slices=%d "
        "| avc sps=%d pps=%d slices=%d | vvc dci=%d opi=%d vps=%d sps=%d pps=%d ph=%d slices=%d "
        "| av1 sh=%d fh=%d | vp9 fh=%d | vp8 fh=%d\n",
        g_hevc_vps,
        g_hevc_sps,
        g_hevc_pps,
        g_hevc_sei_msgs,
        g_hevc_sei_first_type,
        g_hevc_slices,
        g_avc_sps,
        g_avc_pps,
        g_avc_slices,
        g_vvc_dci,
        g_vvc_opi,
        g_vvc_vps,
        g_vvc_sps,
        g_vvc_pps,
        g_vvc_ph,
        g_vvc_slices,
        g_av1_sh,
        g_av1_fh,
        g_vp9_fh,
        g_vp8_fh
    );

    /*
     * Collected struct report (C-side equivalent of bs::StructReport).  The
     * state pins the codec (auto-detect cannot disambiguate VCL-only / raw
     * OBU streams).
     */
    BsState* rstate = bs_state_create(codec);
    if (rstate == NULL) {
        fprintf(stderr, "create failed: %s\n", bs_get_last_error());
        free(buf);
        return 1;
    }

    BsStructReport* sr = bs_parse_struct_report(rstate, buf, got, fm, 4);
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

            case BS_STRUCT_VVC_SPS:
                printf(
                    "  VVC SPS id=%u ctu=%u\n",
                    ((const BsVvcSequenceParameterSet*)e->data)->sps_id,
                    ((const BsVvcSequenceParameterSet*)e->data)->log2_ctu_size_minus5 + 5
                );
                break;

            case BS_STRUCT_VVC_PPS:
                printf("  VVC PPS id=%u\n", ((const BsVvcPictureParameterSet*)e->data)->pps_id);
                break;

            case BS_STRUCT_AV1_SEQUENCE_HEADER:
                printf(
                    "  AV1 SPS profile=%u w=%u h=%u\n",
                    ((const BsAv1SequenceHeader*)e->data)->seq_profile,
                    ((const BsAv1SequenceHeader*)e->data)->max_frame_width,
                    ((const BsAv1SequenceHeader*)e->data)->max_frame_height
                );
                break;

            case BS_STRUCT_VP9_FRAME_HEADER:
                printf(
                    "  VP9 FH profile=%u w=%u h=%u\n",
                    ((const BsVp9FrameHeader*)e->data)->profile,
                    ((const BsVp9FrameHeader*)e->data)->width,
                    ((const BsVp9FrameHeader*)e->data)->height
                );
                break;

            case BS_STRUCT_VP8_FRAME_HEADER:
                printf(
                    "  VP8 FH key=%u w=%u h=%u\n",
                    ((const BsVp8FrameHeader*)e->data)->key_frame,
                    ((const BsVp8FrameHeader*)e->data)->width,
                    ((const BsVp8FrameHeader*)e->data)->height
                );
                break;

            default:
                printf("  kind=%d\n", (int)e->kind);
                break;
        }
    }
    bs_struct_report_destroy(sr);

    /*
     * Structured report path: the codec-pinned state selects the NAL-type
     * names; passing a NULL state instead would auto-detect.  The report is a
     * BsReport of BsNalEntry structs (no JSON).
     */
    BsReport* report = bs_parse_report(rstate, buf, got, fm, 4);

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

    bs_state_destroy(rstate);

    free(buf);
    return 0;
}