#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mohd Umeer
# SPDX-License-Identifier: BSD-2-Clause
"""Generate synthetic H.264 slice-group (FMO) fixtures.

x264 does not expose FMO, so the streams are written directly from the
H.264 syntax.  Each map-type fixture is a 4x4-macroblock (64x64) stream with
an IDR picture followed by one P picture.  The P picture is split into one
slice per slice group, which forces the parser to walk non-contiguous
macroblock addresses with NextMbAddress().

A second variant (`avc_fmo_refs_map*`) adds two reference P pictures before
the target P picture and gives each slice group a different reordering /
active-count combination.  The same ref_idx therefore maps to different POCs
per group, and the sidecar records the expected per-group list and the
macroblock-to-group map.  Those fixtures pin the exact per-cell slice
resolution for non-contiguous groups.

`avc_fmo_mixed_i_p_map0_cavlc.h264` mixes slice types inside one picture: a P
slice group is decoded first and an I slice group second.  A whole-grid mark
for the I slice would erase the P motion, so the fixture pins per-slice intra
marking.

Two entropy modes are emitted:

  cavlc  each group's first macroblock is P_L0_16x16 with a known MVD and
         the rest of the group is an mb_skip_run.  The sidecar .fmo.txt
         lists the coded macroblock addresses and MVDs.
  cabac  every macroblock is a skipped P macroblock.  The arithmetic coder
         is the spec's informative encoder, so end_of_slice_flag is present
         after every macroblock and must be consumed at each slice-group
         boundary.

The generator derives the same slice-group maps as the parser (8.2.2) to
choose slice starts and CAVLC skip-run lengths.  It is a test tool, not a
decoder.

Usage:
    python3 tools/gen_avc_fmo_fixture.py [output-directory]
"""

from __future__ import annotations

import math
import re
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Bit / NAL helpers
# ---------------------------------------------------------------------------


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
        while len(self.bits) % 8:
            self.bit(1)

    def align_zero(self):
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


def ceil_log2(value):
    return 0 if value <= 1 else (value - 1).bit_length()


# ---------------------------------------------------------------------------
# Slice-group maps (H.264 8.2.2)
# ---------------------------------------------------------------------------


def build_unit_map(spec, width, height, cycle):
    n = width * height
    groups = spec["groups"]
    kind = spec["type"]
    out = [0] * n

    if kind == 0:
        i = 0
        while i < n:
            for group in range(groups):
                if i >= n:
                    break
                run = spec["run"][group]
                for j in range(run + 1):
                    if i + j < n:
                        out[i + j] = group
                i += run + 1
    elif kind == 1:
        for i in range(n):
            x = i % width
            y = i // width
            out[i] = (x + (y * groups) // 2) % groups
    elif kind == 2:
        out = [groups - 1] * n
        for group in range(groups - 2, -1, -1):
            tl, br = spec["rect"][group]
            y0, x0 = divmod(tl, width)
            y1, x1 = divmod(br, width)
            for y in range(y0, y1 + 1):
                for x in range(x0, x1 + 1):
                    out[y * width + x] = group
    elif kind in (3, 4, 5):
        assert groups == 2
        rate = spec["rate"]
        map_units0 = min(rate * cycle, n)
        direction = spec["dir"]
        if kind == 3:
            out = [1] * n
            dir_v = 1 if direction else 0
            x = (width - dir_v) // 2
            y = (height - dir_v) // 2
            left = right = x
            top = bottom = y
            x_dir = dir_v - 1
            y_dir = dir_v
            filled = 0
            guard = 0
            while filled < map_units0 and guard < n * 4 + 16:
                line = y * width + x
                if 0 <= line < n and out[line] == 1:
                    out[line] = 0
                    filled += 1
                if x_dir == -1 and x == left:
                    left = max(left - 1, 0)
                    x = left
                    x_dir, y_dir = 0, 2 * dir_v - 1
                elif x_dir == 1 and x == right:
                    right = min(right + 1, width - 1)
                    x = right
                    x_dir, y_dir = 0, 1 - 2 * dir_v
                elif y_dir == -1 and y == top:
                    top = max(top - 1, 0)
                    y = top
                    x_dir, y_dir = 1 - 2 * dir_v, 0
                elif y_dir == 1 and y == bottom:
                    bottom = min(bottom + 1, height - 1)
                    y = bottom
                    x_dir, y_dir = 2 * dir_v - 1, 0
                else:
                    x += x_dir
                    y += y_dir
                guard += 1
            assert filled == map_units0
        elif kind == 4:
            upper = (n - map_units0) if direction else map_units0
            out = [1 if i >= upper else 0 for i in range(n)]
            if direction:
                out = [1 - v for v in out]
        else:
            upper = (n - map_units0) if direction else map_units0
            out = [0] * n
            k = 0
            for column in range(width):
                for row in range(height):
                    out[row * width + column] = 0 if k < upper else 1
                    k += 1
            if direction:
                out = [1 - v for v in out]
    elif kind == 6:
        assert len(spec["ids"]) == n
        out = list(spec["ids"])
    else:
        raise ValueError(kind)
    return out


FMO_SPECS = {
    0: {"type": 0, "groups": 2, "run": [1, 1]},
    1: {"type": 1, "groups": 3},
    2: {"type": 2, "groups": 3, "rect": [(0, 5), (10, 15)]},
    3: {"type": 3, "groups": 2, "dir": 0, "rate": 4},
    4: {"type": 4, "groups": 2, "dir": 0, "rate": 4},
    5: {"type": 5, "groups": 2, "dir": 1, "rate": 4},
    6: {"type": 6, "groups": 3, "ids": [i % 3 for i in range(16)]},
}


# ---------------------------------------------------------------------------
# CABAC encoder (H.264 9.3.4, informative encoder)
# ---------------------------------------------------------------------------


def load_cabac_tables():
    table_path = Path(__file__).resolve().parent.parent / "parser" / "avc_cabac_tables.hpp"
    text = table_path.read_text(encoding="utf-8")

    def rows(name, columns):
        match = re.search(
            rf"{name}\[64\]\[{columns}\]\s*=\s*\{{(.*?)\n\}};", text, re.S
        )
        if not match:
            raise RuntimeError(f"cannot find {name} in {table_path}")
        values = [int(v) for v in re.findall(r"\d+", match.group(1))]
        return [values[i:i + columns] for i in range(0, len(values), columns)]

    init_match = re.search(
        r"kCabacContextInit\[1024\]\[4\]\[2\]\s*=\s*\{(.*?)\n\};", text, re.S
    )
    if not init_match:
        raise RuntimeError(f"cannot find kCabacContextInit in {table_path}")
    init_values = [
        (int(a), int(b))
        for a, b in re.findall(r"\{\s*(-?\d+)\s*,\s*(-?\d+)\s*\}", init_match.group(1))
    ]
    init = [init_values[i:i + 4] for i in range(0, len(init_values), 4)]
    return rows("kCabacRangeLps", 4), rows("kCabacStateTrans", 2), init


RANGE_LPS, STATE_TRANS, CONTEXT_INIT = load_cabac_tables()


class CabacContext:
    def __init__(self, ctx_idx, model, qp):
        m, n = CONTEXT_INIT[ctx_idx][model]
        pre = ((m * qp) >> 4) + n
        pre = min(max(pre, 1), 126)
        if pre <= 63:
            self.state = 63 - pre
            self.mps = 0
        else:
            self.state = pre - 64
            self.mps = 1


class CabacEncoder:
    def __init__(self):
        self.low = 0
        self.range = 510
        self.first = True
        self.outstanding = 0
        self.bits = []

    def _put_bit(self, bit):
        if self.first:
            self.first = False
        else:
            self.bits.append(bit)
        while self.outstanding:
            self.bits.append(1 - bit)
            self.outstanding -= 1

    def _put_bits(self, value, count):
        for i in range(count - 1, -1, -1):
            self._put_bit((value >> i) & 1)

    def _renorm(self):
        while self.range < 256:
            if self.low >= 512:
                self.low -= 512
                self._put_bit(1)
            elif self.low < 256:
                self._put_bit(0)
            else:
                self.low -= 256
                self.outstanding += 1
            self.range <<= 1
            self.low <<= 1

    def decision(self, ctx, bin_value):
        q = (self.range >> 6) & 3
        lps = RANGE_LPS[ctx.state][q]
        self.range -= lps
        if bin_value != ctx.mps:
            self.low += self.range
            self.range = lps
            if ctx.state == 0:
                ctx.mps ^= 1
            ctx.state = STATE_TRANS[ctx.state][0]
        else:
            ctx.state = STATE_TRANS[ctx.state][1]
        self._renorm()

    def terminate(self, bin_value):
        self.range -= 2
        if bin_value:
            self.low += self.range
            self.range = 2
            self._renorm()
            self._put_bit((self.low >> 9) & 1)
            self._put_bits(((self.low >> 7) & 3) | 1, 2)
        else:
            self._renorm()

    def payload(self):
        while len(self.bits) % 8:
            self.bits.append(0)
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            byte = 0
            for b in self.bits[i:i + 8]:
                byte = (byte << 1) | b
            out.append(byte)
        return bytes(out)


# ---------------------------------------------------------------------------
# Parameter sets and slices
# ---------------------------------------------------------------------------


def sps(width=4, height=4, max_refs=2):
    b = Bits()
    b.u(77, 8)          # profile_idc: Main (CABAC capable)
    b.u(0, 8)           # constraint flags
    b.u(30, 8)          # level_idc
    b.ue(0)             # seq_parameter_set_id
    b.ue(0)             # log2_max_frame_num_minus4 -> 4 bits
    b.ue(2)             # pic_order_cnt_type
    b.ue(max_refs)      # max_num_ref_frames
    b.bit(0)            # gaps_in_frame_num_value_allowed_flag
    b.ue(width - 1)     # pic_width_in_mbs_minus1
    b.ue(height - 1)    # pic_height_in_map_units_minus1
    b.bit(1)            # frame_mbs_only_flag
    b.bit(1)            # direct_8x8_inference_flag
    b.bit(0)            # frame_cropping_flag
    b.bit(0)            # vui_parameters_present_flag
    b.bit(1)            # rbsp_stop_one_bit
    b.align_zero()
    return b.payload()


def pps(spec, cabac0):
    b = Bits()
    b.ue(0)             # pic_parameter_set_id
    b.ue(0)             # seq_parameter_set_id
    b.bit(cabac0)       # entropy_coding_mode_flag
    b.bit(0)            # bottom_field_pic_order_in_frame_present_flag
    b.ue(spec["groups"] - 1)
    b.ue(spec["type"])
    if spec["type"] == 0:
        for run in spec["run"]:
            b.ue(run)
    elif spec["type"] == 1:
        pass
    elif spec["type"] == 2:
        for tl, br in spec["rect"]:
            b.ue(tl)
            b.ue(br)
    elif spec["type"] in (3, 4, 5):
        b.bit(spec["dir"])
        b.ue(spec["rate"] - 1)
    elif spec["type"] == 6:
        b.ue(len(spec["ids"]) - 1)
        bits = ceil_log2(spec["groups"])
        for group in spec["ids"]:
            b.u(group, bits)
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
    b.bit(1)            # rbsp_stop_one_bit
    b.align_zero()
    return b.payload()


def slice_header(spec, slice_type, first_mb, frame_num, idr, cabac0, cycle,
                 active_l0=None, reorder_l0=None):
    b = Bits()
    b.ue(first_mb)
    b.ue(slice_type)
    b.ue(0)             # pic_parameter_set_id
    b.u(frame_num, 4)   # log2_max_frame_num = 4
    if idr:
        b.ue(0)         # idr_pic_id
    if slice_type in (0, 1, 3, 4):
        if active_l0 is None:
            b.bit(0)    # num_ref_idx_active_override_flag
        else:
            b.bit(1)
            b.ue(active_l0 - 1)
        if reorder_l0 is None:
            b.bit(0)    # ref_pic_list_modification_flag_l0
        else:
            b.bit(1)
            for op in reorder_l0:
                b.ue(op[0])
                if op[0] in (0, 1):
                    b.ue(op[1])
            b.ue(3)
        if slice_type == 1:
            b.bit(0)    # ref_pic_list_modification_flag_l1
    if idr:
        b.bit(0)        # no_output_of_prior_pics_flag
        b.bit(0)        # long_term_reference_flag
    else:
        b.bit(0)        # adaptive_ref_pic_marking_mode_flag
    if cabac0 and slice_type not in (2, 4):
        b.ue(0)         # cabac_init_idc
    b.se(0)             # slice_qp_delta
    if spec["type"] in (3, 4, 5):
        b.u(cycle, ceil_log2(spec["rate"]))
    if cabac0:
        b.align_one()
    return b


def idr_slice(spec, cabac0):
    b = slice_header(spec, 2, 0, 0, True, cabac0, 1)
    if cabac0:
        # The extractor does not traverse I-slice data; a valid 9-bit
        # arithmetic preload is all the syntax parser needs.
        b.bits.extend([1] + [0] * 31)
    else:
        for _ in range(16):
            b.ue(1)     # I_16x16, DC prediction, no luma CBP
            b.se(0)     # mb_qp_delta
            b.bit(1)    # luma DC coeff_token: TotalCoeff 0
        b.bit(1)        # rbsp_stop_one_bit
        b.align_zero()
    return nal(5, 3, b.payload())


def cavlc_i_slice(spec, addrs, frame_num):
    """I slice for a slice group (the extractor deliberately skips I data)."""
    b = slice_header(spec, 2, addrs[0], frame_num, False, False, 1)
    for _ in addrs:
        b.ue(1)     # I_16x16, DC prediction, no coded block pattern
        b.se(0)     # mb_qp_delta
        b.bit(1)    # luma DC coeff_token: TotalCoeff 0
        b.bit(1)    # Cb DC coeff_token: TotalCoeff 0
        b.bit(1)    # Cr DC coeff_token: TotalCoeff 0
    b.bit(1)        # rbsp_stop_one_bit
    b.align_zero()
    return nal(1, 2, b.payload())


def cavlc_p_slice(spec, group, addrs, frame_num, active_l0=None,
                  reorder_l0=None):
    b = slice_header(spec, 0, addrs[0], frame_num, False, False, 1,
                     active_l0=active_l0, reorder_l0=reorder_l0)
    dx, dy = group_mvd(group)
    b.ue(0)             # mb_skip_run before the first coded MB
    b.ue(0)             # P_L0_16x16
    if active_l0 is not None and active_l0 > 1:
        # ref_idx_l0 = 0 uses te(v); te(2) is one inverted bit.
        if active_l0 == 2:
            b.bit(1)
        else:
            b.ue(0)
    b.se(dx)
    b.se(dy)
    b.ue(0)             # coded_block_pattern = 0
    b.ue(len(addrs) - 1)  # remaining macroblocks in this slice group
    b.bit(1)            # rbsp_stop_one_bit
    b.align_zero()
    return nal(1, 2, b.payload())


def cabac_p_slice(spec, addrs, frame_num, active_l0=None, reorder_l0=None):
    b = slice_header(spec, 0, addrs[0], frame_num, False, True, 1,
                     active_l0=active_l0, reorder_l0=reorder_l0)
    enc = CabacEncoder()
    ctx = CabacContext(11, 1, 26)  # mb_skip_flag, cabac_init_idc 0, QP 26
    for index in range(len(addrs)):
        enc.decision(ctx, 1)       # mb_skip_flag
        enc.terminate(1 if index + 1 == len(addrs) else 0)
    body = enc.payload()
    # Append the arithmetic codeword bits after the byte-aligned header.
    for byte in body:
        b.u(byte, 8)
    return nal(1, 2, b.payload())


def cavlc_skip_slice(spec, addrs, frame_num, active_l0=None, reorder_l0=None):
    b = slice_header(spec, 0, addrs[0], frame_num, False, False, 1,
                     active_l0=active_l0, reorder_l0=reorder_l0)
    b.ue(len(addrs))    # whole slice group is skipped
    b.bit(1)            # rbsp_stop_one_bit
    b.align_zero()
    return nal(1, 2, b.payload())


def group_mvd(group):
    return ((group * 3) % 7) - 3, ((group * 5 + 1) % 9) - 4


def make_stream(map_type, cabac0):
    spec = FMO_SPECS[map_type]
    width = height = 4
    cycle = spec.get("cycle", 1)
    unit_map = build_unit_map(spec, width, height, cycle)
    groups = {}
    for addr, group in enumerate(unit_map):
        groups.setdefault(group, []).append(addr)
    order = sorted(groups, key=lambda g: (0 not in groups[g], g))

    data = bytearray()
    data += nal(7, 3, sps(width, height))
    data += nal(8, 3, pps(spec, cabac0))
    data += idr_slice(spec, cabac0)

    reference_lines = [
        "# avc fmo reference",
        f"map_type {map_type}",
        f"entropy {'cabac' if cabac0 else 'cavlc'}",
        f"groups {len(groups)}",
    ]

    for group in order:
        addrs = groups[group]
        if cabac0:
            data += cabac_p_slice(spec, addrs, 1)
        else:
            data += cavlc_p_slice(spec, group, addrs, 1)
            dx, dy = group_mvd(group)
            reference_lines.append(f"coded {addrs[0]} {dx} {dy}")

    return bytes(data), "\n".join(reference_lines) + "\n"


def make_mixed_i_p_stream():
    """One P picture whose first slice group is P and second is I.

    H.264 permits mixed slice types in a picture.  The I slice must only mark
    its own macroblocks intra; marking the whole grid would erase motion from
    the P group that was decoded before it.
    """
    spec = FMO_SPECS[0]
    width = height = 4
    unit_map = build_unit_map(spec, width, height, 1)
    groups = {}
    for addr, group in enumerate(unit_map):
        groups.setdefault(group, []).append(addr)

    data = bytearray()
    data += nal(7, 3, sps(width, height))
    data += nal(8, 3, pps(spec, False))
    data += idr_slice(spec, False)
    data += cavlc_p_slice(spec, 0, groups[0], 1)
    data += cavlc_i_slice(spec, groups[1], 1)
    return bytes(data)


def refs_group_plan(group):
    """Active count and reordering for one slice group of the target P picture.

    List construction for P frame_num 3 starts as [fn2(POC 4), fn1(POC 2), ...].
    Group 0 activates one entry, group 1 reorders fn1 to the front and activates
    two entries, and any later group keeps the default two-entry order.
    """
    if group == 0:
        return 1, None, [4]
    if group == 1:
        return 2, [(0, 1)], [2, 4]
    return 2, None, [4, 2]


def make_refs_stream(map_type, cabac0):
    """Fixture with per-slice-group reference reordering and active counts.

    Two reference P pictures (fn1 POC 2, fn2 POC 4) precede a target P picture
    (fn3 POC 6) whose slices deliberately disagree: the same ref_idx 0 maps to
    POC 4 in group 0 and POC 2 in group 1.  The picture-level convenience table
    is the first slice, so a consumer that falls back to it reports POC 4 for
    every macroblock.  The exact slice map must report POC 2 for group 1.
    """
    spec = FMO_SPECS[map_type]
    width = height = 4
    cycle = spec.get("cycle", 1)
    unit_map = build_unit_map(spec, width, height, cycle)
    groups = {}
    for addr, group in enumerate(unit_map):
        groups.setdefault(group, []).append(addr)
    order = sorted(groups, key=lambda g: (0 not in groups[g], g))

    data = bytearray()
    data += nal(7, 3, sps(width, height, max_refs=4))
    data += nal(8, 3, pps(spec, cabac0))
    data += idr_slice(spec, cabac0)

    # Two reference P pictures; every slice group is coded as a skipped slice.
    for frame_num in (1, 2):
        for group in order:
            addrs = groups[group]
            if cabac0:
                data += cabac_p_slice(spec, addrs, frame_num)
            else:
                data += cavlc_skip_slice(spec, addrs, frame_num)

    reference_lines = [
        "# avc fmo per-slice reference mapping",
        f"map_type {map_type}",
        f"entropy {'cabac' if cabac0 else 'cavlc'}",
        f"groups {len(groups)}",
        "refs 2",
        # Projection target: different from the picture POC (6) and from
        # every list entry, so motionTo really scales per slice.
        "project_poc 2",
    ]
    for group in order:
        active, _, pocs = refs_group_plan(group)
        reference_lines.append(
            f"group {group} active {active} poc " + " ".join(str(p) for p in pocs)
        )
    for addr, group in enumerate(unit_map):
        reference_lines.append(f"mb_group {addr} {group}")

    # Target P picture: one slice per group with group-specific list state.
    for group in order:
        addrs = groups[group]
        active, reorder, _ = refs_group_plan(group)
        if cabac0:
            data += cabac_p_slice(spec, addrs, 3, active_l0=active,
                                  reorder_l0=reorder)
        else:
            data += cavlc_p_slice(spec, group, addrs, 3, active_l0=active,
                                  reorder_l0=reorder)
            dx, dy = group_mvd(group)
            reference_lines.append(f"coded {addrs[0]} {dx} {dy}")

    return bytes(data), "\n".join(reference_lines) + "\n"


def main(out_dir):
    directory = Path(out_dir)
    directory.mkdir(parents=True, exist_ok=True)
    for map_type in sorted(FMO_SPECS):
        for cabac0, suffix in ((False, "cavlc"), (True, "cabac")):
            stream, reference = make_stream(map_type, cabac0)
            name = f"avc_fmo_map{map_type}_{suffix}.h264"
            (directory / name).write_bytes(stream)
            (directory / (name + ".fmo.txt")).write_text(reference, encoding="ascii")
            print(name)

            stream, reference = make_refs_stream(map_type, cabac0)
            name = f"avc_fmo_refs_map{map_type}_{suffix}.h264"
            (directory / name).write_bytes(stream)
            (directory / (name + ".fmo.txt")).write_text(reference, encoding="ascii")
            print(name)

    name = "avc_fmo_mixed_i_p_map0_cavlc.h264"
    (directory / name).write_bytes(make_mixed_i_p_stream())
    print(name)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "tests/fuzz/corpus")
