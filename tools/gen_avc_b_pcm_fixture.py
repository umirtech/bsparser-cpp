#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mohd Umeer
# SPDX-License-Identifier: BSD-2-Clause
"""Generate a B-slice I_PCM fixture.

x264 disables B frames in lossless mode, so no off-the-shelf encoder produces
a CABAC/CAVLC B slice containing I_PCM.  This synthetic 32x32 CAVLC stream
covers the B-slice branch of the I_PCM detection:

  frame 0 (IDR, I)   four I_16x16 macroblocks
  frame 1 (P)        four skipped macroblocks
  frame 2 (B)        B_L0_16x16, I_PCM with luma+chroma samples, two B_SKIPs
  frame 3 (P)        four skipped macroblocks

The 4:2:0 I_PCM macroblock carries 256 luma samples followed by two 8x8
chroma planes, exercising the ChromaArrayType 1 sample geometry in a B slice.
"""

import sys


class Bits:
    def __init__(self):
        self.bits = []

    def bit(self, value):
        self.bits.append(1 if value else 0)

    def u(self, value, count):
        for i in range(count - 1, -1, -1):
            self.bit((value >> i) & 1)

    def ue(self, value):
        value += 1
        n = value.bit_length() - 1
        self.bits.extend([0] * n)
        self.u(value, n + 1)

    def se(self, value):
        self.ue(-2 * value if value <= 0 else 2 * value - 1)

    def align_zero(self):
        while len(self.bits) % 8:
            self.bit(0)

    def rbsp_trailing(self):
        self.bit(1)
        while len(self.bits) % 8:
            self.bit(0)

    def payload(self):
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            byte = 0
            for b in self.bits[i:i + 8]:
                byte = (byte << 1) | b
            out.append(byte)
        return bytes(out)


def escape(rbsp):
    out = bytearray()
    zeros = 0
    for b in rbsp:
        if zeros >= 2 and b <= 3:
            out.append(3)
            zeros = 0
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def nal(nal_type, ref_idc, rbsp):
    return b"\x00\x00\x00\x01" + bytes([(ref_idc << 5) | nal_type]) + escape(rbsp)


def sps():
    b = Bits()
    b.u(66, 8)          # profile_idc: Baseline
    b.u(0, 8)
    b.u(30, 8)          # level_idc
    b.ue(0)             # seq_parameter_set_id
    b.ue(0)             # log2_max_frame_num_minus4 -> 4 bits
    b.ue(2)             # pic_order_cnt_type
    b.ue(2)             # max_num_ref_frames
    b.bit(0)            # gaps_in_frame_num_value_allowed_flag
    b.ue(1)             # pic_width_in_mbs_minus1 -> 2 MBs
    b.ue(1)             # pic_height_in_map_units_minus1 -> 2 MBs
    b.bit(1)            # frame_mbs_only_flag
    b.bit(1)            # direct_8x8_inference_flag
    b.bit(0)            # frame_cropping_flag
    b.bit(0)            # vui_parameters_present_flag
    b.rbsp_trailing()
    return b.payload()


def pps():
    b = Bits()
    b.ue(0)             # pic_parameter_set_id
    b.ue(0)             # seq_parameter_set_id
    b.bit(0)            # entropy_coding_mode_flag: CAVLC
    b.bit(0)            # bottom_field_pic_order_in_frame_present_flag
    b.ue(0)             # num_slice_groups_minus1
    b.ue(0)             # num_ref_idx_l0_default_active_minus1
    b.ue(0)             # num_ref_idx_l1_default_active_minus1
    b.bit(0)            # weighted_pred_flag
    b.u(0, 2)           # weighted_bipred_idc
    b.se(0)             # pic_init_qp_minus26
    b.se(0)             # pic_init_qs_minus26
    b.se(0)             # chroma_qp_index_offset
    b.bit(0)            # deblocking_filter_control_present_flag
    b.bit(0)            # constrained_intra_pred_flag
    b.bit(0)            # redundant_pic_cnt_present_flag
    b.rbsp_trailing()
    return b.payload()


def slice_header(slice_type, frame_num, idr, ref_idc, direct_spatial=1):
    b = Bits()
    b.ue(0)             # first_mb_in_slice
    b.ue(slice_type)
    b.ue(0)             # pic_parameter_set_id
    b.u(frame_num, 4)
    if idr:
        b.ue(0)         # idr_pic_id
    if slice_type == 1:
        b.bit(direct_spatial)  # direct_spatial_mv_pred_flag
    if slice_type in (0, 1, 3, 4):
        b.bit(0)        # num_ref_idx_active_override_flag
        b.bit(0)        # ref_pic_list_modification_flag_l0
        if slice_type == 1:
            b.bit(0)    # ref_pic_list_modification_flag_l1
    if ref_idc != 0:
        if idr:
            b.bit(0)    # no_output_of_prior_pics_flag
            b.bit(0)    # long_term_reference_flag
        else:
            b.bit(0)    # adaptive_ref_pic_marking_mode_flag
    b.se(0)             # slice_qp_delta
    return b


def i_slice(frame_num):
    b = slice_header(2, frame_num, True, 3)
    for _ in range(4):
        b.ue(3)         # I_16x16, DC prediction, luma CBP 0
        b.ue(0)         # intra_chroma_pred_mode
        b.se(0)         # mb_qp_delta
        b.bit(1)        # luma DC coeff_token: TotalCoeff 0
    b.rbsp_trailing()
    return nal(5, 3, b.payload())


def p_slice_skipped(frame_num):
    b = slice_header(0, frame_num, False, 2)
    b.ue(4)             # mb_skip_run: every macroblock is skipped
    b.rbsp_trailing()
    return nal(1, 2, b.payload())


def b_slice(frame_num):
    # Non-reference B picture: no dec_ref_pic_marking().
    b = slice_header(1, frame_num, False, 0)
    # Macroblock 0: B_L0_16x16 with MVD (1, -1) and no residual.
    b.ue(0)             # mb_skip_run
    b.ue(1)             # mb_type: B_L0_16x16
    b.se(1)             # mvd_l0[0][0]
    b.se(-1)            # mvd_l0[0][1]
    b.ue(0)             # coded_block_pattern
    # Macroblock 1: I_PCM with 256 luma + 2 * 8 * 8 chroma samples.
    b.ue(0)             # mb_skip_run
    b.ue(48)            # mb_type: I_PCM in a B slice
    b.align_zero()
    for i in range(256):
        b.u((i * 5 + 11) & 0xFF, 8)
    for i in range(128):
        b.u((i * 3 + 7) & 0xFF, 8)
    # Macroblocks 2 and 3: B_SKIP (direct).
    b.ue(2)             # mb_skip_run
    b.rbsp_trailing()
    return nal(1, 0, b.payload())


def main(path):
    data = bytearray()
    data += nal(7, 3, sps())
    data += nal(8, 3, pps())
    data += i_slice(0)
    data += p_slice_skipped(1)
    data += b_slice(2)
    data += p_slice_skipped(3)
    with open(path, "wb") as f:
        f.write(data)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "avc_b_pcm_cavlc.h264")
