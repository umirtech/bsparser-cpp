// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * C-API accuracy check against pre-generated reference files.
 *
 * Parses a stream with the C API and compares the decoded SPS / VUI / SEI
 * fields against a reference file containing the expected values. The
 * reference files live under tests/fuzz/reference as .txt files.
 *
 * Usage:
 *     verify_c <stream> <reference.txt> <hevc|avc>
 *
 * Exit status: 0 = every reference key matched, 1 = mismatch / error.
 */

#include "bs_capi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * strdup is POSIX and spelled _strdup on MSVC; provide a portable copy so
 * the tool builds on every CI platform.
 */
static char* bs_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

/* ---- captured parser output ---- */
static int g_profile_idc = -1;
static int g_level_idc = -1;
static int g_coded_w = -1, g_coded_h = -1;
static int g_disp_w = -1, g_disp_h = -1;
static int g_chroma = -1, g_bd_luma = -1, g_bd_chroma = -1;
static int g_colour_prim = -1, g_transfer = -1, g_matrix = -1, g_full_range = -1;
static int g_vui_colour_present = 0;
static int g_mastering = 0, g_maxcll = 0, g_maxfall = 0;

static int g_codec_hevc = 1;

/* ---- reference store ---- */
#define MAX_REF 64
static char* ref_key[MAX_REF];
static long ref_val[MAX_REF];
static int ref_n = 0;

static void store_ref(const char* key, long val) {
    for (int i = 0; i < ref_n; ++i) {
        if (strcmp(ref_key[i], key) == 0) {
            ref_val[i] = val;
            return;
        }
    }
    if (ref_n < MAX_REF) {
        ref_key[ref_n] = bs_strdup(key);
        ref_val[ref_n] = val;
        ++ref_n;
    }
}

static int load_reference(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot open reference %s\n", path);
        return -1;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#')
            continue;
        char* eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        store_ref(line, strtol(eq + 1, NULL, 10));
    }
    fclose(f);
    return 0;
}

/* ---- HEVC callbacks ---- */
static void on_hevc_sps(void* ctx, const BsHevcSequenceParameterSet* sps) {
    (void)ctx;
    g_profile_idc = sps->profile_tier_level.general_profile_idc;
    g_level_idc = sps->profile_tier_level.general_level_idc;
    g_coded_w = (int)sps->geometry.coded_width;
    g_coded_h = (int)sps->geometry.coded_height;
    g_disp_w = (int)sps->geometry.display_width;
    g_disp_h = (int)sps->geometry.display_height;
    g_chroma = sps->chroma_format;
    g_bd_luma = sps->bit_depth.luma;
    g_bd_chroma = sps->bit_depth.chroma;

    if (sps->vui.video_signal.present && sps->vui.video_signal.colour.present) {
        g_vui_colour_present = 1;
        g_colour_prim = sps->vui.video_signal.colour.colour_primaries;
        g_transfer = sps->vui.video_signal.colour.transfer_characteristics;
        g_matrix = sps->vui.video_signal.colour.matrix_coefficients;
    }
    if (sps->vui.video_signal.present) {
        g_full_range = sps->vui.video_signal.video_full_range_flag;
    }
}

static void on_hevc_sei(
    void* ctx, unsigned int payload_type, const unsigned char* payload, size_t payload_size
) {
    (void)ctx;
    if (payload_type == 137 && payload_size >= 24) {
        g_mastering = 1;
    } else if (payload_type == 144 && payload_size >= 4) {
        g_maxcll = (payload[0] << 8) | payload[1];
        g_maxfall = (payload[2] << 8) | payload[3];
    }
}

/* ---- AVC callbacks ---- */
static void on_avc_sps(void* ctx, const BsAvcSequenceParameterSet* sps) {
    (void)ctx;
    g_profile_idc = sps->profile_idc;
    g_level_idc = sps->level_idc;
    g_chroma = sps->chroma_format_idc;
    g_bd_luma = sps->bit_depth_luma_minus8 + 8;
    g_bd_chroma = sps->bit_depth_chroma_minus8 + 8;

    int mb_w = sps->pic_width_in_mbs_minus1 + 1;
    int mb_h = sps->pic_height_in_map_units_minus1 + 1;
    int coded_w = mb_w * 16;
    int coded_h = mb_h * 16 * (2 - sps->frame_mbs_only_flag);
    g_coded_w = coded_w;
    g_coded_h = coded_h;

    int sub_w = (g_chroma == 1 || g_chroma == 2) ? 2 : 1;
    int sub_h = (g_chroma == 1) ? 2 : 1;
    int crop_l = sps->frame_cropping_flag ? (int)sps->frame_crop_left_offset : 0;
    int crop_r = sps->frame_cropping_flag ? (int)sps->frame_crop_right_offset : 0;
    int crop_t = sps->frame_cropping_flag ? (int)sps->frame_crop_top_offset : 0;
    int crop_b = sps->frame_cropping_flag ? (int)sps->frame_crop_bottom_offset : 0;
    g_disp_w = coded_w - sub_w * (crop_l + crop_r);
    g_disp_h = coded_h - sub_h * (crop_t + crop_b);

    if (sps->vui_parameters_present_flag) {
        if (sps->vui.video_signal_type_present_flag) {
            g_full_range = sps->vui.video_full_range_flag;
        }
        if (sps->vui.colour_description_present_flag) {
            g_vui_colour_present = 1;
            g_colour_prim = sps->vui.colour_primaries;
            g_transfer = sps->vui.transfer_characteristics;
            g_matrix = sps->vui.matrix_coefficients;
        }
    }
}

static void on_avc_sei(
    void* ctx, unsigned int payload_type, const unsigned char* payload, size_t payload_size
) {
    (void)ctx;
    if (payload_type == 137 && payload_size >= 24) {
        g_mastering = 1;
    } else if (payload_type == 144 && payload_size >= 4) {
        g_maxcll = (payload[0] << 8) | payload[1];
        g_maxfall = (payload[2] << 8) | payload[3];
    }
}

/* ---- comparison ---- */
static int g_failures = 0;

static void check_int(const char* key, int parsed) {
    for (int i = 0; i < ref_n; ++i) {
        if (strcmp(ref_key[i], key) == 0) {
            if (parsed != ref_val[i]) {
                printf("  FAIL %s: parser=%d reference=%ld\n", key, parsed, ref_val[i]);
                ++g_failures;
            } else {
                printf("  ok   %s = %d\n", key, parsed);
            }
            return;
        }
    }
}

static int read_file(const char* path, unsigned char** out, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*)malloc(sz ? (size_t)sz : 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out = buf;
    *out_size = rd;
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <stream> <reference.txt> <hevc|avc>\n", argv[0]);
        return 2;
    }

    g_codec_hevc = (strcmp(argv[3], "avc") != 0);

    if (load_reference(argv[2]) != 0)
        return 2;

    unsigned char* data = NULL;
    size_t size = 0;
    if (read_file(argv[1], &data, &size) != 0) {
        fprintf(stderr, "cannot open stream %s\n", argv[1]);
        return 2;
    }

    BsState* state = bs_state_create(g_codec_hevc ? BS_CODEC_HEVC : BS_CODEC_AVC);

    long nals = -1;
    if (g_codec_hevc) {
        BsHevcHandlers h = {0};
        h.sps = on_hevc_sps;
        h.sei = on_hevc_sei;
        nals = bs_parse_hevc(state, data, size, BS_FRAMING_ANNEX_B, 4, &h);
    } else {
        BsAvcHandlers h = {0};
        h.sps = on_avc_sps;
        h.sei = on_avc_sei;
        nals = bs_parse_avc(state, data, size, BS_FRAMING_ANNEX_B, 4, &h);
    }

    if (nals < 0) {
        fprintf(stderr, "parse error: %s\n", bs_get_last_error());
        free(data);
        bs_state_destroy(state);
        return 1;
    }

    printf("stream=%s nals=%ld\n", argv[1], nals);

    check_int("profile_idc", g_profile_idc);
    check_int("level_idc", g_level_idc);
    check_int("coded_width", g_coded_w);
    check_int("coded_height", g_coded_h);
    check_int("display_width", g_disp_w);
    check_int("display_height", g_disp_h);
    check_int("chroma_format_idc", g_chroma);
    check_int("bit_depth_luma", g_bd_luma);
    check_int("bit_depth_chroma", g_bd_chroma);
    if (g_vui_colour_present) {
        check_int("colour_primaries", g_colour_prim);
        check_int("transfer_characteristics", g_transfer);
        check_int("matrix_coefficients", g_matrix);
    }
    check_int("video_full_range_flag", g_full_range);
    check_int("mastering_display_present", g_mastering);
    check_int("max_content_light_level", g_maxcll);
    check_int("max_pic_average_light_level", g_maxfall);

    free(data);
    bs_state_destroy(state);

    if (g_failures == 0) {
        printf("PASS %s (C API)\n", argv[1]);
        return 0;
    }
    printf("FAIL %s (%d mismatch)\n", argv[1], g_failures);
    return 1;
}
