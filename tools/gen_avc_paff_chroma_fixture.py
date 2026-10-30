#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mohd Umeer
# SPDX-License-Identifier: BSD-2-Clause
"""Generate high-chroma PAFF field-picture fixtures.

x264 has no field-picture (PAFF) encoder, so this synthetic stream is written
from the H.264 syntax.  It covers the combination that the x264/ffmpeg MBAFF
fixtures cannot: a 4:2:2 or 4:4:4 field picture whose P slice contains an
I_PCM macroblock.

  frame 0 top field (I, IDR)    four I_PCM macroblocks
  frame 0 bottom field (I, IDR) four I_PCM macroblocks
  frame 1 top field (P)         I_PCM followed by three skipped macroblocks
  frame 1 bottom field (P)      four skipped macroblocks
  frame 2 frame picture (P)     eight skipped macroblocks

The I_PCM macroblock in the P field exercises the high-chroma raw-sample
geometry (MbWidthC x MbHeightC per component) in a field-sized grid.

Usage:
    tools/gen_avc_paff_chroma_fixture.py <output.h264> <422|444> <8|10>
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
    for byte in rbsp:
        if zeros >= 2 and byte <= 3:
            out.append(3)
            zeros = 0
        out.append(byte)
        zeros = zeros + 1 if byte == 0 else 0
    return bytes(out)


def nal(nal_type, ref_idc, rbsp):
    return b"\x00\x00\x00\x01" + bytes([(ref_idc << 5) | nal_type]) + escape(rbsp)


def writer(spec):
    """Return the I_PCM sample writer for the requested format."""
    chroma, depth = spec
    bit_depth = 10 if depth == 10 else 8
    if chroma == "422":
        components = ((16, 16), (8, 16), (8, 16))
    else:
        components = ((16, 16), (16, 16), (16, 16))

    def samples(b):
        value = 0
        for width, height in components:
            for _ in range(width * height):
                b.u(value & ((1 << bit_depth) - 1), bit_depth)
                value += 37

    return samples


def sps(spec):
    chroma, depth = spec
    bit_depth = 10 if depth == 10 else 8
    b = Bits()
    b.u(122 if chroma == "422" else 244, 8)
    b.u(0, 8)           # constraint flags
    b.u(30, 8)          # level_idc
    b.ue(0)             # seq_parameter_set_id
    b.ue(2 if chroma == "422" else 3)
    if chroma == "444":
        b.bit(0)        # separate_colour_plane_flag
    b.ue(bit_depth - 8) # bit_depth_luma_minus8
    b.ue(bit_depth - 8) # bit_depth_chroma_minus8
    b.bit(0)            # qpprime_y_zero_transform_bypass_flag
    b.bit(0)            # seq_scaling_matrix_present_flag
    b.ue(0)             # log2_max_frame_num_minus4 -> 4 bits
    b.ue(2)             # pic_order_cnt_type
    b.ue(2)             # max_num_ref_frames
    b.bit(0)            # gaps_in_frame_num_value_allowed_flag
    b.ue(1)             # pic_width_in_mbs_minus1 -> 2 MBs
    b.ue(1)             # pic_height_in_map_units_minus1 -> 2 MBs
    b.bit(0)            # frame_mbs_only_flag
    b.bit(0)            # mb_adaptive_frame_field_flag -> PAFF
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


def slice_header(slice_type, frame_num, field, bottom, idr):
    b = Bits()
    b.ue(0)             # first_mb_in_slice
    b.ue(slice_type)
    b.ue(0)             # pic_parameter_set_id
    b.u(frame_num, 4)   # log2_max_frame_num = 4
    b.bit(field)
    if field:
        b.bit(bottom)
    if idr:
        b.ue(0)         # idr_pic_id
    # pic_order_cnt_type == 2 carries no POC syntax here.
    if slice_type in (0, 1, 3, 4):
        b.bit(0)        # num_ref_idx_active_override_flag
        b.bit(0)        # ref_pic_list_modification_flag_l0
    if idr:
        b.bit(0)        # no_output_of_prior_pics_flag
        b.bit(0)        # long_term_reference_flag
    else:
        b.bit(0)        # adaptive_ref_pic_marking_mode_flag
    b.se(0)             # slice_qp_delta
    return b


def ipcm_mb(b, samples, mb_type):
    b.ue(mb_type)
    b.align_zero()
    samples(b)


def idr_field(bottom, spec):
    samples = writer(spec)
    b = slice_header(2, 0, True, bottom, True)
    for _ in range(4):
        ipcm_mb(b, samples, 25)
    b.rbsp_trailing()
    return nal(5, 3, b.payload())


def p_field(frame_num, bottom, spec, coded):
    samples = writer(spec)
    b = slice_header(0, frame_num, True, bottom, False)
    if coded:
        b.ue(0)         # mb_skip_run
        ipcm_mb(b, samples, 30)
        b.ue(3)         # mb_skip_run for the remaining three macroblocks
    else:
        b.ue(4)         # every macroblock is skipped
    b.rbsp_trailing()
    return nal(1, 2, b.payload())


def p_frame(frame_num):
    b = slice_header(0, frame_num, False, False, False)
    b.ue(8)             # every macroblock is skipped
    b.rbsp_trailing()
    return nal(1, 2, b.payload())


def main(path, chroma, depth):
    spec = (chroma, int(depth))
    data = bytearray()
    data += nal(7, 3, sps(spec))
    data += nal(8, 3, pps())
    data += idr_field(False, spec)
    data += idr_field(True, spec)
    data += p_field(1, False, spec, True)
    data += p_field(1, True, spec, False)
    data += p_frame(2)
    with open(path, "wb") as output:
        output.write(data)


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[2] not in ("422", "444") or sys.argv[3] not in ("8", "10"):
        sys.stderr.write(
            "usage: gen_avc_paff_chroma_fixture.py <output.h264> <422|444> <8|10>\n")
        raise SystemExit(2)
    main(sys.argv[1], sys.argv[2], sys.argv[3])
