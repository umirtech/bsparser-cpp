#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mohd Umeer
# SPDX-License-Identifier: BSD-2-Clause
"""Generate a CABAC B slice containing an I_PCM macroblock.

x264 disables B frames in lossless mode and never emits I_PCM for ordinary
quantisers, so no off-the-shelf encoder produces this syntax combination.
This tool writes the complete Annex-B stream from the H.264 syntax:

  frame 0 (IDR, I)  four I_16x16 macroblocks, no residual
  frame 1 (P)       four P_SKIP macroblocks
  frame 2 (B)       I_PCM, I_16x16 with residual, two B_SKIP macroblocks

The B slice is 4:2:0 CABAC.  The I_PCM macroblock carries 256 luma samples
and two 8x8 chroma planes.  The following I_16x16 macroblock codes luma DC,
luma AC and one chroma-DC block, so its CABAC contexts consume the CBP,
DC-CBF and non-zero-count state left behind by the I_PCM macroblock.  A
decoder that fails to update that state (or that forgets the arithmetic
engine restart after the raw samples) desynchronises before the B_SKIP
macroblocks.

The CABAC tables are read from parser/avc_cabac_tables.hpp so the generator
and the parser always use the same specification data.

Usage:
    tools/gen_avc_cabac_b_pcm_fixture.py <output.h264>
"""

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
TABLES = ROOT / "parser" / "avc_cabac_tables.hpp"


def load_tables():
    text = TABLES.read_text(encoding="utf-8")

    def body(name):
        start = text.index(name)
        start = text.index("{", start)
        depth = 0
        for pos in range(start, len(text)):
            if text[pos] == "{":
                depth += 1
            elif text[pos] == "}":
                depth -= 1
                if depth == 0:
                    return text[start:pos + 1]
        raise ValueError(f"unterminated table {name}")

    init_rows = [
        (int(a), int(b))
        for a, b in re.findall(r"\{\s*(-?\d+)\s*,\s*(-?\d+)\s*\}", body(
            "kCabacContextInit"))
    ]
    if len(init_rows) != 1024 * 4:
        raise ValueError(f"unexpected context init count {len(init_rows)}")
    init = [init_rows[i * 4:(i + 1) * 4] for i in range(1024)]

    lps_values = [int(v) for v in re.findall(r"-?\d+", body("kCabacRangeLps"))]
    range_lps = [lps_values[i * 4:(i + 1) * 4] for i in range(64)]

    trans_values = [int(v) for v in re.findall(r"-?\d+", body("kCabacStateTrans"))]
    state_trans = [trans_values[i * 2:(i + 1) * 2] for i in range(64)]
    return init, range_lps, state_trans


CONTEXT_INIT, RANGE_LPS, STATE_TRANS = load_tables()


class CabacEncoder:
    """H.264 Annex 9 arithmetic encoder (byte-oriented carry handling)."""

    def __init__(self, model: int, qp: int):
        self.contexts = [[0, 0] for _ in range(1024)]
        for i in range(1024):
            m, n = CONTEXT_INIT[i][model]
            pre = ((m * qp) >> 4) + n
            pre = 1 if pre < 1 else (126 if pre > 126 else pre)
            if pre <= 63:
                self.contexts[i] = [63 - pre, 0]
            else:
                self.contexts[i] = [pre - 64, 1]
        self.filler_bit = 1
        self.out = bytearray()
        self.restart_engine()

    def restart_engine(self):
        self.low = 0
        self.range = 0x01FE
        self.queue = -9
        self.outstanding = 0

    def _put_byte(self):
        if self.queue < 0:
            return
        value = self.low >> (self.queue + 10)
        self.low &= (0x400 << self.queue) - 1
        self.queue -= 8
        if (value & 0xFF) == 0xFF:
            self.outstanding += 1
            return
        carry = value >> 8
        if self.out:
            self.out[-1] = (self.out[-1] + carry) & 0xFF
        elif carry:
            raise ValueError("CABAC carry escaped the slice payload")
        while self.outstanding:
            self.out.append((carry - 1) & 0xFF)
            self.outstanding -= 1
        self.out.append(value & 0xFF)

    def _renorm(self):
        shift = 0
        while self.range < 256:
            self.range <<= 1
            shift += 1
        if shift:
            self.low <<= shift
            self.queue += shift
        self._put_byte()

    def decision(self, ctx: int, bit: int):
        state, mps = self.contexts[ctx]
        lps = RANGE_LPS[state][(self.range >> 6) & 3]
        self.range -= lps
        if bit != mps:
            self.low += self.range
            self.range = lps
            if state == 0:
                mps ^= 1
            self.contexts[ctx] = [STATE_TRANS[state][0], mps]
        else:
            self.contexts[ctx] = [STATE_TRANS[state][1], mps]
        self._renorm()

    def terminate(self):
        self.range -= 2
        self._renorm()

    def bypass(self, bit: int):
        self.low <<= 1
        if bit:
            self.low += self.range
        self.queue += 1
        self._put_byte()

    def flush(self):
        """Terminate the current arithmetic segment and return its bytes."""
        self.low += self.range - 2
        self.low |= 1
        self.low <<= 9
        self.queue += 9
        self._put_byte()
        self._put_byte()
        if self.queue >= 0:
            self.low >>= self.queue
        else:
            self.low <<= -self.queue
        self.low |= self.filler_bit << 10
        self.queue = 0
        self._put_byte()
        while self.outstanding:
            self.out.append(0xFF)
            self.outstanding -= 1
        data = bytes(self.out)
        self.out = bytearray()
        return data


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
        length = value.bit_length() - 1
        self.bits.extend([0] * length)
        self.u(value, length + 1)

    def se(self, value):
        self.ue(-2 * value if value <= 0 else 2 * value - 1)

    def align_one(self):
        while len(self.bits) % 8:
            self.bit(1)

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


def sps():
    b = Bits()
    b.u(77, 8)          # profile_idc: Main
    b.u(0xC0, 8)        # constraint flags: no B slices? keep Main
    b.u(30, 8)          # level_idc
    b.ue(0)             # seq_parameter_set_id
    b.ue(0)             # log2_max_frame_num_minus4 -> 4 bits
    b.ue(0)             # pic_order_cnt_type
    b.ue(2)             # log2_max_pic_order_cnt_lsb_minus4 -> 6 bits
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


def pps(pps_id, cabac):
    b = Bits()
    b.ue(pps_id)        # pic_parameter_set_id
    b.ue(0)             # seq_parameter_set_id
    b.bit(cabac)        # entropy_coding_mode_flag
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


def slice_header(slice_type, frame_num, poc_lsb, idr, ref_idc, pps_id,
                 direct_spatial=1, cabac=True):
    b = Bits()
    b.ue(0)             # first_mb_in_slice
    b.ue(slice_type)
    b.ue(pps_id)
    b.u(frame_num, 4)
    if idr:
        b.ue(0)         # idr_pic_id
    b.u(poc_lsb, 6)     # pic_order_cnt_lsb
    if slice_type == 1:
        b.bit(direct_spatial)
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
    if cabac and slice_type in (0, 1):
        b.ue(0)         # cabac_init_idc
    b.se(0)             # slice_qp_delta
    if cabac:
        b.align_one()   # cabac_alignment_one_bit
    return b


def i_slice():
    """IDR I slice: four I_16x16 macroblocks with no residual (CAVLC)."""

    b = slice_header(2, 0, 0, True, 3, 0, cabac=False)
    for _ in range(4):
        b.ue(3)         # I_16x16, DC prediction, luma CBP 0
        b.ue(0)         # intra_chroma_pred_mode
        b.se(0)         # mb_qp_delta
        b.bit(1)        # luma DC coeff_token: TotalCoeff 0
    b.rbsp_trailing()
    return nal(5, 3, b.payload())


def p_slice():
    """P reference: four P_SKIP macroblocks (CAVLC)."""

    b = slice_header(0, 1, 4, False, 2, 0, cabac=False)
    b.ue(4)             # mb_skip_run
    b.rbsp_trailing()
    return nal(1, 2, b.payload())


def b_slice():
    """B slice: I_PCM, I_16x16 residual, two B_SKIP macroblocks."""

    enc = CabacEncoder(1, 26)

    # Macroblock 0: I_PCM.
    enc.decision(24, 0)              # mb_skip_flag, ctx 11 + 13
    enc.decision(27, 1)              # not B_Direct
    enc.decision(30, 1)
    enc.decision(31, 1)
    enc.decision(32, 1)
    enc.decision(32, 0)
    enc.decision(32, 1)              # mb_type prefix == 13 (intra)
    enc.decision(32, 1)              # not I_NxN
    pcm = enc.flush()                # terminate == 1, then byte alignment
    samples = bytearray()
    samples += bytes((i * 5 + 11) & 0xFF for i in range(256))
    samples += bytes((i * 3 + 7) & 0xFF for i in range(128))
    enc.restart_engine()
    enc.terminate()                  # end_of_slice_flag after the PCM MB

    # Macroblock 1: I_16x16, luma CBP = AC present, chroma CBP = DC only.
    enc.decision(25, 0)              # mb_skip_flag: left PCM is not skipped
    enc.decision(28, 1)              # not B_Direct
    enc.decision(30, 1)
    enc.decision(31, 1)
    enc.decision(32, 1)
    enc.decision(32, 0)
    enc.decision(32, 1)
    enc.decision(32, 1)              # not I_NxN
    enc.terminate()                  # not I_PCM
    enc.decision(33, 1)              # cbp_luma != 0
    enc.decision(34, 1)              # cbp_chroma != 0
    enc.decision(34, 0)              # cbp_chroma == 1
    enc.decision(35, 0)              # intra16x16 prediction mode
    enc.decision(35, 0)
    enc.decision(64, 0)              # intra_chroma_pred_mode == DC (4:2:0)
    enc.decision(60, 0)              # mb_qp_delta == 0

    # Luma DC: left I_PCM contributes luma_dc_cbf = 1, no top neighbour.
    enc.decision(88, 1)
    enc.decision(105, 1)             # significant coeff 0
    enc.decision(166, 1)             # last coeff 0
    enc.decision(228, 0)             # abs level == 1
    enc.bypass(0)                    # positive sign

    # Luma AC: sixteen empty blocks.  The contexts mirror 9.3.3.1.1.9:
    # left-edge blocks see the I_PCM macroblock's non-zero counts, first-row
    # blocks see the unavailable top macroblock as intra (nzb = 1), and the
    # remaining blocks use the already-decoded block above inside this MB.
    left_nzc = [16] * 16
    cur_nzc = [0] * 16

    def nzc_left(index):
        if index % 4 == 0:
            return left_nzc[index + 3]
        return cur_nzc[index - 1]

    def nzc_top(index):
        if index < 4:
            return -1
        return cur_nzc[index - 4]

    for raster in (0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15):
        left = nzc_left(raster)
        top = nzc_top(raster)
        nza = 1 if left < 0 else (1 if left > 0 else 0)
        nzb = 1 if top < 0 else (1 if top > 0 else 0)
        enc.decision(89 + nza + 2 * nzb, 0)

    # Chroma DC: component 0 carries one coefficient, component 1 is empty.
    # Both contexts see the I_PCM chroma_dc_cbf state on the left.
    enc.decision(100, 1)
    enc.decision(149, 1)
    enc.decision(210, 1)
    enc.decision(258, 0)
    enc.bypass(0)
    enc.decision(100, 0)
    enc.terminate()                  # end_of_slice_flag after MB1

    # Macroblock 2: B_SKIP.
    enc.decision(12 + 13, 1)
    enc.terminate()

    # Macroblock 3: B_SKIP (left skipped, top not skipped).
    enc.decision(11 + 1 + 13, 1)

    rbsp = slice_header(1, 2, 2, False, 0, 1).payload() + pcm + bytes(samples) + enc.flush()
    return nal(1, 0, rbsp)


def main(path):
    data = bytearray()
    data += nal(7, 3, sps())
    data += nal(8, 3, pps(0, False))
    data += nal(8, 3, pps(1, True))
    data += i_slice()
    data += p_slice()
    data += b_slice()
    with open(path, "wb") as output:
        output.write(data)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "avc_b_pcm_cabac.h264")
