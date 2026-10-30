#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mohd Umeer
# SPDX-License-Identifier: BSD-2-Clause
"""Generate the synthetic AVC DPB-marking fixtures used by avc_mv_dpb_test.

x264 does not emit MMCO/long-term-reference syntax, so the marking fixtures
are written directly from the H.264 syntax.  Macroblocks are all skipped (or
intra), which keeps the streams tiny while still exercising:

    avc_mmco_cavlc.h264          sliding window, MMCO 1-6, IDR marking,
                                 frame_num wrap and long-term list ordering;
    avc_field_reorder_cavlc.h264 PAFF field lists, field/frame list
                                 reordering, field and pair long-term marking;
    avc_field_mmco_cavlc.h264    MMCO 1/2/3/4/6 applied to individual PAFF
                                 fields and to a field pair;
    avc_field_b_cavlc.h264       B field lists and the identical-list
                                 first-two-entry swap;
    avc_frame_num_wrap_cavlc.h264 FrameNumWrap/PicNum arithmetic across a
                                 log2_max_frame_num wrap;
    avc_frame_num_gap_cavlc.h264 progressive frame_num gap synthesis,
                                 non-existing reference slots and a B list
                                 that selects the non-existing frame;
    avc_frame_num_gap_field_cavlc.h264
                                 the gap process across a PAFF field pair;
    avc_frame_num_gap_poc0_cavlc.h264
                                 pic_order_cnt_type 0: unknown non-existing
                                 POC and omission from B lists;
    avc_multislice_refs_cavlc.h264
                                 two slices with different reordering and
                                 active reference counts;
    avc_no_output_prior_cavlc.h264
                                 IDR no_output_of_prior_pics_flag deferred
                                 delivery suppression.

Usage:
    python3 tools/gen_avc_mmco_fixtures.py <output-directory>
"""

import os
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


def sps(frame_mbs_only, poc_type, log2_frame_num, log2_poc_lsb, max_refs,
        gaps_allowed=0):
    b = Bits()
    b.u(66, 8)          # profile_idc: baseline
    b.u(0, 8)           # constraint flags + reserved
    b.u(30, 8)          # level_idc
    b.ue(0)             # seq_parameter_set_id
    b.ue(log2_frame_num - 4)
    b.ue(poc_type)
    if poc_type == 0:
        b.ue(log2_poc_lsb - 4)
    b.ue(max_refs)
    b.bit(gaps_allowed)
    b.ue(1)             # pic_width_in_mbs_minus1  -> 2 MBs wide
    b.ue(1)             # pic_height_in_map_units_minus1 -> 2 map units
    b.bit(frame_mbs_only)
    if not frame_mbs_only:
        b.bit(0)        # mb_adaptive_frame_field_flag: PAFF, not MBAFF
    b.bit(1)            # direct_8x8_inference_flag
    b.bit(0)            # frame_cropping_flag
    b.bit(0)            # vui_parameters_present_flag
    b.rbsp_trailing()
    return b.payload()


def pps(default_active0=0):
    b = Bits()
    b.ue(0)             # pic_parameter_set_id
    b.ue(0)             # seq_parameter_set_id
    b.bit(0)            # entropy_coding_mode_flag (CAVLC)
    b.bit(0)            # bottom_field_pic_order_in_frame_present_flag
    b.ue(0)             # num_slice_groups_minus1
    b.ue(default_active0)  # num_ref_idx_l0_default_active_minus1
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


def mmco(b, operation):
    b.ue(operation["op"])
    if operation["op"] in (1, 3):
        b.ue(operation["difference_minus1"])
    if operation["op"] == 2:
        b.ue(operation["long_term_pic_num"])
    if operation["op"] in (3, 6):
        b.ue(operation["long_term_frame_idx"])
    if operation["op"] == 4:
        b.ue(operation["max_long_term_frame_idx_plus1"])


def slice_header(frame_mbs_only, slice_type, frame_num, log2_frame_num,
                 poc_type, log2_poc_lsb,
                 field=False, bottom=False, idr=False, pic_order_cnt_lsb=0,
                 nal_ref_idc=2,
                 active_override=None, reorder_l0=None, mmco_ops=None,
                 long_term_reference=False, no_output=False, pps_bottom=False,
                 first_mb=0, reorder_l1=None):
    b = Bits()
    b.ue(first_mb)      # first_mb_in_slice
    b.ue(slice_type)
    b.ue(0)             # pic_parameter_set_id
    b.u(frame_num, log2_frame_num)
    if not frame_mbs_only:
        b.bit(1 if field else 0)
        if field:
            b.bit(bottom)
    if idr:
        b.ue(0)         # idr_pic_id
    if poc_type == 0:
        b.u(pic_order_cnt_lsb, log2_poc_lsb)
        if pps_bottom and not field:
            b.se(0)     # delta_pic_order_cnt_bottom
    if slice_type == 1:
        b.bit(1)        # direct_spatial_mv_pred_flag
    if slice_type in (0, 1, 3, 4):
        if active_override is None:
            b.bit(0)
        else:
            b.bit(1)
            b.ue(active_override[0])
            if slice_type == 1:
                b.ue(active_override[1])
        if reorder_l0 is None:
            b.bit(0)
        else:
            b.bit(1)
            for op in reorder_l0:
                b.ue(op[0])
                if op[0] in (0, 1):
                    b.ue(op[1])
                elif op[0] == 2:
                    b.ue(op[1])
            b.ue(3)
        if slice_type == 1:
            if reorder_l1 is None:
                b.bit(0)    # ref_pic_list_modification_flag_l1
            else:
                b.bit(1)
                for op in reorder_l1:
                    b.ue(op[0])
                    if op[0] in (0, 1):
                        b.ue(op[1])
                    elif op[0] == 2:
                        b.ue(op[1])
                b.ue(3)
    if nal_ref_idc != 0:
        if idr:
            b.bit(no_output)
            b.bit(long_term_reference)
        elif mmco_ops is None:
            b.bit(0)    # adaptive_ref_pic_marking_mode_flag
        else:
            b.bit(1)
            for op in mmco_ops:
                mmco(b, op)
            b.ue(0)     # end of MMCO commands
    b.se(0)             # slice_qp_delta
    return b


def idr_slice(frame_mbs_only, poc_type, log2_frame_num, log2_poc_lsb,
              field=False, bottom=False, long_term=False, no_output=False):
    b = slice_header(frame_mbs_only, 2, 0, log2_frame_num, poc_type,
                     log2_poc_lsb, field=field, bottom=bottom, idr=True,
                     nal_ref_idc=3, long_term_reference=long_term,
                     no_output=no_output)
    b.rbsp_trailing()
    return nal(5, 3, b.payload())


def p_slice(frame_mbs_only, poc_type, log2_frame_num, log2_poc_lsb, frame_num,
            field=False, bottom=False, pic_order_cnt_lsb=0,
            active_override=None, reorder_l0=None, mmco_ops=None,
            mb_count=4, first_mb=0):
    b = slice_header(frame_mbs_only, 0, frame_num, log2_frame_num, poc_type,
                     log2_poc_lsb, field=field, bottom=bottom,
                     pic_order_cnt_lsb=pic_order_cnt_lsb,
                     active_override=active_override, reorder_l0=reorder_l0,
                     mmco_ops=mmco_ops, first_mb=first_mb)
    b.ue(mb_count)      # mb_skip_run: every MB is skipped
    b.rbsp_trailing()
    return nal(1, 2, b.payload())


def b_slice(frame_mbs_only, poc_type, log2_frame_num, log2_poc_lsb, frame_num,
            field=False, bottom=False, pic_order_cnt_lsb=0, active_override=None,
            reorder_l0=None, reorder_l1=None, mb_count=4, first_mb=0):
    b = slice_header(frame_mbs_only, 1, frame_num, log2_frame_num, poc_type,
                     log2_poc_lsb, field=field, bottom=bottom,
                     pic_order_cnt_lsb=pic_order_cnt_lsb,
                     nal_ref_idc=0, active_override=active_override,
                     reorder_l0=reorder_l0, reorder_l1=reorder_l1,
                     first_mb=first_mb)
    b.ue(mb_count)      # mb_skip_run: every MB is direct/skipped
    b.rbsp_trailing()
    return nal(1, 0, b.payload())


def write_progressive(path):
    log2_frame_num = 4
    log2_poc_lsb = 4
    data = bytearray()
    data += nal(7, 3, sps(1, 2, log2_frame_num, log2_poc_lsb, 6))
    data += nal(8, 3, pps(6))

    def p(frame_num, ops=None, active=None):
        nonlocal data
        data += p_slice(1, 2, log2_frame_num, log2_poc_lsb, frame_num,
                        active_override=active, mmco_ops=ops)

    data += idr_slice(1, 2, log2_frame_num, log2_poc_lsb,
                      long_term=False, no_output=False)
    # P1: assign long-term index 1 to the IDR (MMCO 4 sets the limit first).
    p(1, [{"op": 4, "max_long_term_frame_idx_plus1": 2},
          {"op": 3, "difference_minus1": 0, "long_term_frame_idx": 1}])
    p(2)
    # P3: remove the long-term IDR (MMCO 2).
    p(3, [{"op": 2, "long_term_pic_num": 1}])
    # P4: mark the current picture long-term index 0 (MMCO 6).
    p(4, [{"op": 6, "long_term_frame_idx": 0}])
    p(5)
    # P6: clear all references (MMCO 5).
    p(6, [{"op": 5}])
    p(7)
    # Second IDR: long-term IDR, clears the DPB and sets MaxLongTermFrameIdx=0.
    data += idr_slice(1, 2, log2_frame_num, log2_poc_lsb,
                      long_term=True, no_output=False)
    p(1)
    p(2)
    # P3: remove the oldest short-term picture (MMCO 1).
    p(3, [{"op": 1, "difference_minus1": 0}])
    # P4: remove the long-term IDR (MMCO 2).
    p(4, [{"op": 2, "long_term_pic_num": 0}])
    # P5: assign long-term index 2 to P3 (MMCO 4 + 3).
    p(5, [{"op": 4, "max_long_term_frame_idx_plus1": 3},
          {"op": 3, "difference_minus1": 1, "long_term_frame_idx": 2}])
    # P6: reassign long-term index 2 to P4.  The previous holder (P3) lies
    # earlier in the DPB, exercising conflict removal while a later target
    # pointer is live.
    p(6, [{"op": 3, "difference_minus1": 1, "long_term_frame_idx": 2}])
    # P7: lower MaxLongTermFrameIdx to 0, dropping long-term index 2 (MMCO 4).
    p(7, [{"op": 4, "max_long_term_frame_idx_plus1": 1}])
    p(8)
    # P9: the sliding window drops the oldest short-term picture.
    p(9, active=(5, 0))

    with open(path, "wb") as out:
        out.write(data)


def write_field_reorder(path):
    log2_frame_num = 4
    log2_poc_lsb = 4
    data = bytearray()
    data += nal(7, 3, sps(0, 0, log2_frame_num, log2_poc_lsb, 6))
    data += nal(8, 3, pps(6))

    def p_field(frame_num, bottom, lsb, active=None, reorder=None, ops=None):
        nonlocal data
        data += p_slice(0, 0, log2_frame_num, log2_poc_lsb, frame_num,
                        field=True, bottom=bottom, pic_order_cnt_lsb=lsb,
                        active_override=active, reorder_l0=reorder,
                        mmco_ops=ops)

    def p_frame(frame_num, lsb, active=None, ops=None):
        nonlocal data
        data += p_slice(0, 0, log2_frame_num, log2_poc_lsb, frame_num,
                        pic_order_cnt_lsb=lsb, active_override=active,
                        mmco_ops=ops, mb_count=8)

    data += idr_slice(0, 0, log2_frame_num, log2_poc_lsb, field=True, bottom=False)
    data += idr_slice(0, 0, log2_frame_num, log2_poc_lsb, field=True, bottom=True)
    p_field(1, False, 4)
    p_field(1, True, 6)

    # P top frame 2: move the opposite-parity P bottom field (PicNum 2,
    # POC 6) to index 0, then assign it long-term index 1 with MMCO 4 + 3.
    p_field(2, False, 8, active=(1, 0),
            reorder=[(0, 2)],
            ops=[{"op": 4, "max_long_term_frame_idx_plus1": 2},
                 {"op": 3, "difference_minus1": 2, "long_term_frame_idx": 1}])
    p_field(2, True, 10)

    # P top frame 3: move long-term field index 1 (opposite-parity
    # LongTermPicNum 2, POC 6) to index 0.
    p_field(3, False, 14, active=(0, 0),
            reorder=[(2, 2)])
    p_field(3, True, 16)

    # P frame 4: move the P frame-2 field pair to long-term index 0.  The
    # reference list still contains ST frames and is followed by the LT pair.
    p_frame(4, 18, active=(2, 0),
            ops=[{"op": 3, "difference_minus1": 1, "long_term_frame_idx": 0}])
    # P frame 5 and 6: LT pair stays at the end of the frame list; the sliding
    # window then drops the IDR frame.
    p_frame(5, 20)
    p_frame(6, 22, active=(3, 0))

    with open(path, "wb") as out:
        out.write(data)


def write_field_mmco(path):
    log2_frame_num = 4
    log2_poc_lsb = 4
    data = bytearray()
    data += nal(7, 3, sps(0, 0, log2_frame_num, log2_poc_lsb, 4))
    data += nal(8, 3, pps(6))

    def p_field(frame_num, bottom, lsb, reorder=None, ops=None):
        nonlocal data
        data += p_slice(0, 0, log2_frame_num, log2_poc_lsb, frame_num,
                        field=True, bottom=bottom, pic_order_cnt_lsb=lsb,
                        reorder_l0=reorder, mmco_ops=ops)

    def p_frame(frame_num, lsb, active=None):
        nonlocal data
        data += p_slice(0, 0, log2_frame_num, log2_poc_lsb, frame_num,
                        pic_order_cnt_lsb=lsb, active_override=active,
                        mb_count=8)

    data += idr_slice(0, 0, log2_frame_num, log2_poc_lsb, field=True, bottom=False)
    data += idr_slice(0, 0, log2_frame_num, log2_poc_lsb, field=True, bottom=True)
    p_field(1, False, 4)
    p_field(1, True, 6)

    # P top frame 2: assign the opposite-parity P bottom field long-term
    # index 0 (MMCO 4 + 3) and remove the same-parity P top field with MMCO 1.
    p_field(2, False, 8,
            ops=[{"op": 4, "max_long_term_frame_idx_plus1": 1},
                 {"op": 3, "difference_minus1": 2, "long_term_frame_idx": 0},
                 {"op": 1, "difference_minus1": 1}])
    p_field(2, True, 10)

    # P top frame 3: remove the long-term P bottom field (opposite-parity
    # LongTermPicNum 0) with MMCO 2, then make the current field long-term.
    p_field(3, False, 12,
            ops=[{"op": 4, "max_long_term_frame_idx_plus1": 2},
                 {"op": 2, "long_term_pic_num": 0},
                 {"op": 6, "long_term_frame_idx": 1}])
    p_field(3, True, 14,
            ops=[{"op": 6, "long_term_frame_idx": 1}])

    # Frame list: short-term ST frames first, then the LT field pair.
    p_frame(4, 16, active=(2, 0))

    with open(path, "wb") as out:
        out.write(data)


def write_wrap(path):
    log2_frame_num = 4
    log2_poc_lsb = 4
    data = bytearray()
    data += nal(7, 3, sps(1, 2, log2_frame_num, log2_poc_lsb, 2))
    data += nal(8, 3, pps(2))
    data += idr_slice(1, 2, log2_frame_num, log2_poc_lsb)
    for frame_num in range(1, 16):
        data += p_slice(1, 2, log2_frame_num, log2_poc_lsb, frame_num)
    # frame_num wraps 15 -> 0.  Reorder the older fn14 (PicNum -2) in front of
    # fn15 (PicNum -1), then remove fn15 with MMCO 1 (picNumX = -1).
    data += p_slice(1, 2, log2_frame_num, log2_poc_lsb, 0,
                   active_override=(1, 0), reorder_l0=[(1, 13)],
                   mmco_ops=[{"op": 1, "difference_minus1": 0}])
    # The new fn1 must see fn0 (POC 32) and fn14 (POC 28); the sliding window
    # then drops fn14 and adds the new fn1.
    data += p_slice(1, 2, log2_frame_num, log2_poc_lsb, 1,
                   active_override=(1, 0))
    with open(path, "wb") as out:
        out.write(data)


def write_field_b(path):
    log2_frame_num = 4
    log2_poc_lsb = 4
    data = bytearray()
    data += nal(7, 3, sps(0, 0, log2_frame_num, log2_poc_lsb, 4))
    data += nal(8, 3, pps(6))
    data += idr_slice(0, 0, log2_frame_num, log2_poc_lsb, field=True, bottom=False)
    data += idr_slice(0, 0, log2_frame_num, log2_poc_lsb, field=True, bottom=True)
    data += p_slice(0, 0, log2_frame_num, log2_poc_lsb, 1,
                    field=True, bottom=False, pic_order_cnt_lsb=4)
    data += p_slice(0, 0, log2_frame_num, log2_poc_lsb, 1,
                    field=True, bottom=True, pic_order_cnt_lsb=6)
    # B top field: both lists contain the P pair and IDR pair; identical
    # lists are resolved by swapping the first two entries of list 1.
    data += b_slice(0, 0, log2_frame_num, log2_poc_lsb, 2,
                    field=True, bottom=False, pic_order_cnt_lsb=8,
                    active_override=(6, 6))
    with open(path, "wb") as out:
        out.write(data)


def write_gap(path):
    log2_frame_num = 4
    log2_poc_lsb = 4
    data = bytearray()
    # gaps_in_frame_num_value_allowed_flag = 1, pic_order_cnt_type = 2 so the
    # non-existing frame's POC is inferable (8.2.1.3 / 8.2.5.2).
    data += nal(7, 3, sps(1, 2, log2_frame_num, log2_poc_lsb, 4, gaps_allowed=1))
    data += nal(8, 3, pps(6))
    data += idr_slice(1, 2, log2_frame_num, log2_poc_lsb)
    data += p_slice(1, 2, log2_frame_num, log2_poc_lsb, 1)
    # A non-reference B advances to frame_num 3.  The gap process synthesizes
    # non-existing frame 2 (POC 4).  Its list 1 keeps that frame in the
    # colocated slot; the extractor must not use fabricated motion for it.
    data += b_slice(1, 2, log2_frame_num, log2_poc_lsb, 3,
                    active_override=(1, 1), reorder_l0=[(0, 1)],
                    reorder_l1=[(0, 0)])
    # P3 is a reference picture after the same frame_num; PrevRefFrameNum is
    # now the synthesized fn2, so no duplicate gap frame is created.
    data += p_slice(1, 2, log2_frame_num, log2_poc_lsb, 3,
                    active_override=(1, 0))
    data += p_slice(1, 2, log2_frame_num, log2_poc_lsb, 4,
                    active_override=(2, 0))
    # Two consecutive missing frame numbers (5 and 6) exercise the 8.2.5.2
    # loop and sliding-window eviction of earlier synthetic frames.
    data += p_slice(1, 2, log2_frame_num, log2_poc_lsb, 7,
                    active_override=(3, 0))
    with open(path, "wb") as out:
        out.write(data)


def write_gap_field(path):
    log2_frame_num = 4
    log2_poc_lsb = 4
    data = bytearray()
    data += nal(7, 3, sps(0, 2, log2_frame_num, log2_poc_lsb, 6, gaps_allowed=1))
    data += nal(8, 3, pps(6))
    data += idr_slice(0, 2, log2_frame_num, log2_poc_lsb, field=True, bottom=False)
    data += idr_slice(0, 2, log2_frame_num, log2_poc_lsb, field=True, bottom=True)
    data += p_slice(0, 2, log2_frame_num, log2_poc_lsb, 1,
                    field=True, bottom=False)
    data += p_slice(0, 2, log2_frame_num, log2_poc_lsb, 1,
                    field=True, bottom=True)
    # The first field of frame 3 triggers the gap; the synthesized frame has
    # both fields non-existing and appears in both field lists.
    data += p_slice(0, 2, log2_frame_num, log2_poc_lsb, 3,
                    field=True, bottom=False, active_override=(5, 0))
    data += p_slice(0, 2, log2_frame_num, log2_poc_lsb, 3,
                    field=True, bottom=True, active_override=(5, 0))
    with open(path, "wb") as out:
        out.write(data)


def write_multislice_refs(path):
    log2_frame_num = 4
    log2_poc_lsb = 4
    data = bytearray()
    data += nal(7, 3, sps(1, 2, log2_frame_num, log2_poc_lsb, 6))
    data += nal(8, 3, pps(6))
    data += idr_slice(1, 2, log2_frame_num, log2_poc_lsb)
    data += p_slice(1, 2, log2_frame_num, log2_poc_lsb, 1)
    data += p_slice(1, 2, log2_frame_num, log2_poc_lsb, 2)
    data += p_slice(1, 2, log2_frame_num, log2_poc_lsb, 3)
    # Picture 4 has two slices covering the four macroblocks of the 2x2
    # picture.  Slice 0 reorders fn2 ahead of fn3 and activates two entries;
    # slice 1 has no reordering and only one active entry.  The picture-level
    # convenience list is the first slice; the per-slice table preserves both.
    data += p_slice(1, 2, log2_frame_num, log2_poc_lsb, 4,
                    active_override=(1, 0), reorder_l0=[(0, 1)],
                    first_mb=0, mb_count=2)
    data += p_slice(1, 2, log2_frame_num, log2_poc_lsb, 4,
                    active_override=(0, 0), first_mb=2, mb_count=2)
    with open(path, "wb") as out:
        out.write(data)


def write_no_output_prior(path):
    log2_frame_num = 4
    log2_poc_lsb = 4
    data = bytearray()
    data += nal(7, 3, sps(1, 2, log2_frame_num, log2_poc_lsb, 4))
    data += nal(8, 3, pps(6))
    data += idr_slice(1, 2, log2_frame_num, log2_poc_lsb, no_output=False)
    data += p_slice(1, 2, log2_frame_num, log2_poc_lsb, 1)
    data += p_slice(1, 2, log2_frame_num, log2_poc_lsb, 2)
    # This IDR suppresses the three pictures above.
    data += idr_slice(1, 2, log2_frame_num, log2_poc_lsb, no_output=True)
    data += p_slice(1, 2, log2_frame_num, log2_poc_lsb, 1)
    # An IDR with flag 0 flushes the queued IDR1/P1 pair before it.
    data += idr_slice(1, 2, log2_frame_num, log2_poc_lsb, no_output=False)
    data += p_slice(1, 2, log2_frame_num, log2_poc_lsb, 1)
    with open(path, "wb") as out:
        out.write(data)


def write_gap_poc0(path):
    log2_frame_num = 4
    log2_poc_lsb = 4
    data = bytearray()
    # pic_order_cnt_type 0: 8.2.5.2 does not infer a POC for the non-existing
    # frame, and B lists must omit it (8.2.4.2.3/8.2.4.2.4).
    data += nal(7, 3, sps(1, 0, log2_frame_num, log2_poc_lsb, 4, gaps_allowed=1))
    data += nal(8, 3, pps(6))
    data += idr_slice(1, 0, log2_frame_num, log2_poc_lsb)
    data += p_slice(1, 0, log2_frame_num, log2_poc_lsb, 1,
                    pic_order_cnt_lsb=2)
    # Non-reference B fn3 triggers the gap; its B lists omit fn2.
    data += b_slice(1, 0, log2_frame_num, log2_poc_lsb, 3,
                    pic_order_cnt_lsb=5, active_override=(1, 1))
    # P3 keeps the non-existing frame in its P list (unknown sentinel), and
    # its skipped cells select the empty slot.
    data += p_slice(1, 0, log2_frame_num, log2_poc_lsb, 3,
                    pic_order_cnt_lsb=6, active_override=(2, 0))
    data += p_slice(1, 0, log2_frame_num, log2_poc_lsb, 4,
                    pic_order_cnt_lsb=8, active_override=(2, 0))
    with open(path, "wb") as out:
        out.write(data)


def main(directory):
    os.makedirs(directory, exist_ok=True)
    write_progressive(os.path.join(directory, "avc_mmco_cavlc.h264"))
    write_field_reorder(os.path.join(directory, "avc_field_reorder_cavlc.h264"))
    write_field_mmco(os.path.join(directory, "avc_field_mmco_cavlc.h264"))
    write_field_b(os.path.join(directory, "avc_field_b_cavlc.h264"))
    write_wrap(os.path.join(directory, "avc_frame_num_wrap_cavlc.h264"))
    write_gap(os.path.join(directory, "avc_frame_num_gap_cavlc.h264"))
    write_gap_field(os.path.join(directory, "avc_frame_num_gap_field_cavlc.h264"))
    write_gap_poc0(os.path.join(directory, "avc_frame_num_gap_poc0_cavlc.h264"))
    write_multislice_refs(os.path.join(directory, "avc_multislice_refs_cavlc.h264"))
    write_no_output_prior(os.path.join(directory, "avc_no_output_prior_cavlc.h264"))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
