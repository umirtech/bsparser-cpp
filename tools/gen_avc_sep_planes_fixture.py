#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mohd Umeer
# SPDX-License-Identifier: BSD-2-Clause
"""Generate the synthetic separate-colour-plane fixture.

x264 cannot emit separate_colour_plane_flag=1, so this fixture is written
directly from the H.264 syntax.  It is a 32x32 High 4:4:4 CAVLC stream with
three colour planes per access unit, each plane carried in its own slice NAL
and tagged by colour_plane_id.

Every access unit contains all three planes and each plane has deliberately
different motion, so a consumer must keep three independent MV-DPBs and
deliver three grids:

  frame 0 (IDR)   all planes: four I_16x16 macroblocks
  frame 1 (P)     plane 0: P_L0_16x16 MVD (2,-2), one I_PCM macroblock
                            (ChromaArrayType 0, so only 256 luma samples),
                            then two skips
                  plane 1: P_L0_16x16 MVD (1,1), then three skips
                  plane 2: two skips, P_L0_16x16 MVD (-3,2), one skip
  frame 2 (P)     plane 0: four skips
                  plane 1: three skips, P_L0_16x16 MVD (-2,3)
                  plane 2: four skips

All three plane slices of an access unit carry the same frame_num and POC.
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
        code = -2 * value if value <= 0 else 2 * value - 1
        self.ue(code)

    def align_zero(self):
        while len(self.bits) % 8:
            self.bit(0)

    def rbsp_trailing(self):
        self.bit(1)
        self.align_zero()

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
    b.u(244, 8)         # profile_idc: High 4:4:4 Predictive
    b.u(0, 8)           # constraint flags + reserved
    b.u(30, 8)          # level_idc
    b.ue(0)             # seq_parameter_set_id
    b.ue(3)             # chroma_format_idc: 4:4:4
    b.bit(1)            # separate_colour_plane_flag
    b.ue(0)             # bit_depth_luma_minus8
    b.ue(0)             # bit_depth_chroma_minus8
    b.bit(0)            # qpprime_y_zero_transform_bypass_flag
    b.bit(0)            # seq_scaling_matrix_present_flag
    b.ue(0)             # log2_max_frame_num_minus4 -> 4 bits
    b.ue(2)             # pic_order_cnt_type
    b.ue(1)             # max_num_ref_frames
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


def slice_header(slice_type, plane, frame_num, idr):
    b = Bits()
    b.ue(0)             # first_mb_in_slice
    b.ue(slice_type)
    b.ue(0)             # pic_parameter_set_id
    b.u(plane, 2)       # colour_plane_id
    b.u(frame_num, 4)   # log2_max_frame_num = 4
    if idr:
        b.ue(0)         # idr_pic_id
    # pic_order_cnt_type == 2 carries no POC syntax.
    if slice_type in (0, 1, 3, 4):
        b.bit(0)        # num_ref_idx_active_override_flag
        b.bit(0)        # ref_pic_list_modification_flag_l0
        if slice_type == 1:
            b.bit(0)    # ref_pic_list_modification_flag_l1
    if idr:
        b.bit(0)        # no_output_of_prior_pics_flag
        b.bit(0)        # long_term_reference_flag
    else:
        b.bit(0)        # adaptive_ref_pic_marking_mode_flag
    b.se(0)             # slice_qp_delta
    return b


def i16_mb(b):
    """I_16x16, DC prediction, no residual (ChromaArrayType 0)."""
    b.ue(1)             # I_16x16, pred mode 0, luma CBP 0
    b.se(0)             # mb_qp_delta
    b.bit(1)            # luma DC coeff_token: TotalCoeff 0


def i_slice(plane, frame_num, idr):
    b = slice_header(2, plane, frame_num, idr)
    for _ in range(4):
        i16_mb(b)
    b.rbsp_trailing()
    return nal(5 if idr else 1, 3 if idr else 2, b.payload())


def p_slice(plane, frame_num, skip_before, dx, dy, skip_after):
    b = slice_header(0, plane, frame_num, False)
    b.ue(skip_before)
    if dx is not None:
        b.ue(0)         # mb_type: P_L0_16x16
        b.se(dx)
        b.se(dy)
        b.ue(0)         # coded_block_pattern
        if skip_after > 0:
            b.ue(skip_after)
    b.rbsp_trailing()
    return nal(1, 2, b.payload())


def p_slice_i_pcm(plane, frame_num):
    b = slice_header(0, plane, frame_num, False)
    # Macroblock 0: P_L0_16x16 with MVD (2, -2) and no residual.
    b.ue(0)
    b.ue(0)
    b.se(2)
    b.se(-2)
    b.ue(0)
    # Macroblock 1: I_PCM.  ChromaArrayType is 0, so only 256 luma samples.
    b.ue(0)             # mb_skip_run
    b.ue(30)            # mb_type: I_PCM in a P slice
    b.align_zero()
    for i in range(256):
        b.u((i * 7 + 3) & 0xFF, 8)
    # Macroblocks 2 and 3 are skipped.
    b.ue(2)
    b.rbsp_trailing()
    return nal(1, 2, b.payload())


def main(path):
    data = bytearray()
    data += nal(7, 3, sps())
    data += nal(8, 3, pps())

    for plane in range(3):
        data += i_slice(plane, 0, True)

    data += p_slice_i_pcm(0, 1)
    data += p_slice(1, 1, 0, 1, 1, 3)
    data += p_slice(2, 1, 2, -3, 2, 1)

    data += p_slice(0, 2, 4, None, None, 0)
    data += p_slice(1, 2, 3, -2, 3, 0)
    data += p_slice(2, 2, 4, None, None, 0)

    with open(path, "wb") as f:
        f.write(data)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "avc_sep_planes_cavlc.h264")
