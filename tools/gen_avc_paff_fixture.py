#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mohd Umeer
# SPDX-License-Identifier: BSD-2-Clause
"""Generate the small synthetic PAFF fixture used by avc_mv_field_test.

x264 does not implement PAFF (field-coded) output, so this fixture is written
directly from the H.264 syntax.  It is deliberately tiny and uses a CAVLC
P-slice whose macroblocks are all skipped; the motion-vector extractor's field
geometry, field POC/DPB and field flags are what the test exercises.
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

    def align_one(self):
        self.bit(1)
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
    b.u(66, 8)          # profile_idc: baseline
    b.u(0, 8)           # constraint flags + reserved
    b.u(30, 8)          # level_idc
    b.ue(0)             # seq_parameter_set_id
    b.ue(0)             # log2_max_frame_num_minus4
    b.ue(2)             # pic_order_cnt_type
    b.ue(2)             # max_num_ref_frames (IDR pair + P pair)
    b.bit(0)            # gaps_in_frame_num_value_allowed_flag
    b.ue(1)             # pic_width_in_mbs_minus1  -> 2 MBs wide
    b.ue(1)             # pic_height_in_map_units_minus1 -> 2 map units
    b.bit(0)            # frame_mbs_only_flag
    b.bit(0)            # mb_adaptive_frame_field_flag
    b.bit(1)            # direct_8x8_inference_flag
    b.bit(0)            # frame_cropping_flag
    b.bit(0)            # vui_parameters_present_flag
    b.rbsp_trailing()
    return b.payload()


def pps():
    b = Bits()
    b.ue(0)             # pic_parameter_set_id
    b.ue(0)             # seq_parameter_set_id
    b.bit(0)            # entropy_coding_mode_flag (CAVLC)
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
        if slice_type == 1:
            b.bit(0)    # ref_pic_list_modification_flag_l1
    if idr:
        b.bit(0)        # no_output_of_prior_pics_flag
        b.bit(0)        # long_term_reference_flag
    else:
        b.bit(0)        # adaptive_ref_pic_marking_mode_flag
    b.se(0)             # slice_qp_delta
    return b


def idr_field(bottom):
    b = slice_header(2, 0, True, bottom, True)
    # The extractor emits an intra grid for I slices without traversing the
    # macroblock layer; trailing bits keep the NAL well-formed.
    b.rbsp_trailing()
    return nal(5, 3, b.payload())


def p_slice(frame_num, field, bottom, mb_count, coded=False):
    b = slice_header(0, frame_num, field, bottom, False)
    if not coded:
        b.ue(mb_count)      # mb_skip_run: every MB is skipped
    else:
        # One P_L0_16x16 macroblock with a small MVD and no residual,
        # followed by a skip run for the remaining macroblocks.
        b.ue(0)             # mb_skip_run
        b.ue(0)             # mb_type: P_L0_16x16
        b.se(2)             # mvd_l0[0][0]
        b.se(-2)            # mvd_l0[0][1]
        b.ue(0)             # coded_block_pattern
        if mb_count > 1:
            b.ue(mb_count - 1)
    b.rbsp_trailing()
    return nal(1, 2, b.payload())


def main(path):
    data = bytearray()
    data += nal(7, 3, sps())
    data += nal(8, 3, pps())
    data += idr_field(False)
    data += idr_field(True)
    # Two P fields (2x2 MBs) and one P frame (2x4 MBs).  The top field
    # carries one coded P_L0_16x16 macroblock.
    data += p_slice(1, True, False, 4, coded=True)
    data += p_slice(1, True, True, 4)
    data += p_slice(2, False, False, 8)
    with open(path, "wb") as f:
        f.write(data)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "avc_paff_cavlc.h264")
