// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#!/usr/bin/env python3
"""Compare bsparser's report fields against pre-generated reference files.

For each input file:
  1. Run bs_cli --json to get bsparser's parsed fields.
  2. Load the expected results from a reference file (JSON) produced once by
     `--generate`.
  3. Compare the parameter-set and slice fields that both expose.

The reference files hold the expected values; the comparison itself never
invokes any external tool.

Usage:
  python tools/compare_report.py <file...>             # compare vs references
  python tools/compare_report.py --generate <file...>  # create reference files
"""

import json
import os
import re
import subprocess
import sys

BS_CLI = os.path.join("build", "bs_cli" + (".exe" if os.name == "nt" else ""))
MAX_SLICES = 40

# ---------------------------------------------------------------------------
# reference parsing (the trace is parsed once at generation time; the
# expected results are stored as a JSON reference file)
# ---------------------------------------------------------------------------

SECTION_HEADERS = {
    "Sequence Parameter Set", "Picture Parameter Set", "Video Parameter Set",
    "Slice", "Access Unit Delimiter", "End Of Sequence", "End Of Stream",
    "SEI", "Slice Segment Header", "Decoding Capability Information",
    "Picture Header", "OPS", "DCI", "Operating Point Information",
}

ELEMENT_RE = re.compile(r"^(\d+)\s+(\S+)\s+([01x ]+?)\s*=\s*(-?\d+)\s*$")


def parse_trace(text):
    """Return a list of {'kind':..., 'fields':{name:int}} per NAL."""
    groups = []
    current = None
    for raw in text.splitlines():
        line = raw.strip()
        if "trace_headers" not in line:
            continue
        content = line.split("] ", 1)[-1].strip() if "] " in line else line
        if content.startswith("nal_unit_type:"):
            continue
        if content.startswith("Extradata"):
            current = {"kind": "Extradata", "fields": {}}
            groups.append(current)
            continue
        # A section header starts a new NAL.
        if content in SECTION_HEADERS or any(
            content.startswith(h) for h in SECTION_HEADERS
        ):
            current = {"kind": content, "fields": {}}
            groups.append(current)
            continue
        if current is None:
            continue
        m = ELEMENT_RE.match(content)
        if m:
            name = m.group(2)
            try:
                value = int(m.group(4))
            except ValueError:
                continue
            current["fields"][name] = value
    return groups


def prune_groups(groups, max_slices=MAX_SLICES):
    """Keep the first group of each non-slice kind plus the first N slices.

    The comparison only touches the first SPS group and the first N slice
    groups, so a compact reference is sufficient.
    """
    by_kind = {}
    for g in groups:
        if g["kind"].startswith("Slice"):
            continue
        if g["kind"] not in by_kind:
            by_kind[g["kind"]] = g
    slices = [g for g in groups if g["kind"].startswith("Slice")][:max_slices]
    return list(by_kind.values()) + slices


def reference_path(path):
    return os.path.join(
        os.path.dirname(os.path.abspath(path)),
        "reference",
        os.path.basename(path) + ".json",
    )


def generate_reference(path):
    """Run the external trace once and store the expected results as JSON."""
    groups = parse_trace(run_external_trace(path))
    report = load_report(path)
    ref = {
        "codec": report.get("codec") if report else None,
        "slice_count": sum(1 for g in groups if g["kind"].startswith("Slice")),
        "groups": prune_groups(groups),
    }
    ref_file = reference_path(path)
    os.makedirs(os.path.dirname(ref_file), exist_ok=True)
    with open(ref_file, "w", encoding="utf-8") as f:
        json.dump(ref, f, indent=1)
        f.write("\n")
    print(f"wrote {ref_file} (slice_count={ref['slice_count']}, "
          f"groups={len(ref['groups'])})")


def load_reference(path):
    ref_file = reference_path(path)
    if not os.path.isfile(ref_file):
        return None
    with open(ref_file, encoding="utf-8") as f:
        return json.load(f)


def run_external_trace(path, timeout=600):
    cmd = ["ffmpeg", "-v", "trace", "-i", path, "-c:v", "copy",
           "-bsf:v", "trace_headers", "-f", "null", "-"]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.stderr


# ---------------------------------------------------------------------------
# bsparser report parsing
# ---------------------------------------------------------------------------


def load_report(path):
    p = subprocess.run(
        [BS_CLI, path, "--json"],
        capture_output=True,
        text=True,
        timeout=300,
    )
    out = p.stdout
    # bs_cli prints "container=..." first and a "codec=..." summary after;
    # the JSON document itself spans the first '{' to the last '}'.
    start = out.find("{")
    end = out.rfind("}")
    if start < 0 or end < 0 or end <= start:
        return None
    return json.loads(out[start:end + 1])


# ---------------------------------------------------------------------------
# Comparison helpers
# ---------------------------------------------------------------------------

def cmp(name, ours, theirs):
    """Return (ok, msg). ours/theirs may be str-or-int."""
    if ours is None or theirs is None:
        return True, None  # field absent on one side: skip
    try:
        o = int(ours)
        t = int(theirs)
    except (ValueError, TypeError):
        return True, None
    if o == t:
        return True, None
    return False, f"{name}: ours={o} ref={t}"


def add(checks, name, ours, theirs, transform=None):
    if ours is None or theirs is None:
        return
    if transform:
        theirs = transform(theirs)
    ok, msg = cmp(name, ours, theirs)
    if not ok:
        checks.append(msg)


SLICE_MAP = {"P": 0, "B": 1, "I": 2, "SP": 3, "SI": 4}  # AVC
HEVC_SLICE_MAP = {"B": 0, "P": 1, "I": 2}


def avc_display_size(f):
    """Display dimensions from the raw SPS fields (H.264 7.4.2.1.1)."""
    w_mbs = f.get("pic_width_in_mbs_minus1", 0) + 1
    h_map = f.get("pic_height_in_map_units_minus1", 0) + 1
    mbs_only = f.get("frame_mbs_only_flag", 1)
    chroma = f.get("chroma_format_idc", 1)
    w = w_mbs * 16
    h = (2 - mbs_only) * h_map * 16
    if f.get("frame_cropping_flag"):
        sub_w = 1 if chroma == 0 else (2 if chroma in (1, 2) else 1)
        cu_x = 1 if chroma == 0 else sub_w
        cu_y = (2 - mbs_only) if chroma == 0 else 2 * (2 - mbs_only)
        w -= cu_x * (f.get("frame_crop_left_offset", 0) + f.get("frame_crop_right_offset", 0))
        h -= cu_y * (f.get("frame_crop_top_offset", 0) + f.get("frame_crop_bottom_offset", 0))
    return w, h


def compare_avc(report, ref, checks, max_slices=MAX_SLICES):
    sps_ours = next((e for e in report["nals"] if e["type"] == "SPS"), None)
    sps_ff = first_group(ref["groups"], "Sequence Parameter Set")
    if sps_ours and sps_ff:
        f = sps_ff["fields"]
        o = sps_ours["fields"]
        add(checks, "SPS profile_idc", o.get("profile_idc"), f.get("profile_idc"))
        add(checks, "SPS level_idc", o.get("level_idc"), f.get("level_idc"))
        add(checks, "SPS chroma_format_idc", o.get("chroma_format_idc"), f.get("chroma_format_idc"))
        add(checks, "SPS log2_max_frame_num", o.get("log2_max_frame_num_minus4"), f.get("log2_max_frame_num_minus4"))
        add(checks, "SPS pic_order_cnt_type", o.get("pic_order_cnt_type"), f.get("pic_order_cnt_type"))
        add(checks, "SPS log2_max_poc_lsb", o.get("log2_max_pic_order_cnt_lsb_minus4"), f.get("log2_max_pic_order_cnt_lsb_minus4"))
        add(checks, "SPS max_num_ref_frames", o.get("max_num_ref_frames"), f.get("max_num_ref_frames"))
        if any("frame_crop" in k for k in f):
            dw, dh = avc_display_size(f)
            add(checks, "SPS width", o.get("width"), dw)
            add(checks, "SPS height", o.get("height"), dh)
        else:
            add(checks, "SPS width", o.get("width"), f.get("pic_width_in_mbs_minus1"),
                transform=lambda v: (v + 1) * 16)
            add(checks, "SPS height", o.get("height"), f.get("pic_height_in_map_units_minus1"),
                transform=lambda v: (v + 1) * 16)
        add(checks, "SPS frame_mbs_only", o.get("frame_mbs_only"), f.get("frame_mbs_only_flag"))
        add(checks, "SPS direct_8x8", o.get("direct_8x8_inference"), f.get("direct_8x8_inference_flag"))

    ours_slices = [e for e in report["nals"] if e["vcl"]]
    ff_slices = [g for g in ref["groups"] if g["kind"].startswith("Slice")]
    ff_slice_count = ref.get("slice_count", len(ff_slices))
    n = min(len(ours_slices), len(ff_slices), max_slices)
    for i in range(n):
        o = ours_slices[i]["fields"]
        f = ff_slices[i]["fields"]
        tag = f"slice[{i}]"
        # the raw ue(v) slice_type (0-9); base type = value % 5.
        if o.get("slice_type") in SLICE_MAP and f.get("slice_type") is not None:
            if SLICE_MAP[o["slice_type"]] != (f["slice_type"] % 5):
                checks.append(f"{tag} slice_type: ours={o['slice_type']}({SLICE_MAP.get(o['slice_type'])}) ref={f['slice_type']}")
        add(checks, tag + " frame_num", o.get("frame_num"), f.get("frame_num"))
        add(checks, tag + " poc_lsb", o.get("pic_order_cnt_lsb"), f.get("pic_order_cnt_lsb"))
        add(checks, tag + " qp_delta", o.get("slice_qp_delta"), f.get("slice_qp_delta"))
        add(checks, tag + " first_mb", o.get("first_mb"), f.get("first_mb_in_slice"))
    if len(ours_slices) != ff_slice_count and ff_slice_count > 0:
        checks.append(f"slice count: ours={len(ours_slices)} ref={ff_slice_count}")


def compare_hevc(report, ref, checks, max_slices=MAX_SLICES):
    sps_ours = next((e for e in report["nals"] if e["type"] == "SPS_NUT"), None)
    sps_ff = first_group(ref["groups"], "Sequence Parameter Set")
    if sps_ours and sps_ff:
        f = sps_ff["fields"]
        o = sps_ours["fields"]
        add(checks, "SPS profile_idc", o.get("profile_idc"), f.get("general_profile_idc"))
        add(checks, "SPS level_idc", o.get("level_idc"), f.get("general_level_idc"))
        add(checks, "SPS width", o.get("width"), f.get("pic_width_in_luma_samples"))
        add(checks, "SPS height", o.get("height"), f.get("pic_height_in_luma_samples"))
        add(checks, "SPS chroma_format", o.get("chroma_format"), f.get("chroma_format_idc"))
        add(checks, "SPS bit_depth_luma", o.get("bit_depth_luma"), f.get("bit_depth_luma_minus8"),
            transform=lambda v: v + 8)
        add(checks, "SPS log2_max_poc_lsb", o.get("log2_max_poc_lsb"), f.get("log2_max_pic_order_cnt_lsb_minus4"),
            transform=lambda v: v + 4)
        add(checks, "SPS max_sub_layers", o.get("max_sub_layers"), f.get("sps_max_sub_layers_minus1"),
            transform=lambda v: v + 1)

    ours_slices = [e for e in report["nals"] if e["vcl"]]
    ff_slices = [g for g in ref["groups"] if g["kind"].startswith("Slice")]
    ff_slice_count = ref.get("slice_count", len(ff_slices))
    n = min(len(ours_slices), len(ff_slices), max_slices)
    for i in range(n):
        o = ours_slices[i]["fields"]
        f = ff_slices[i]["fields"]
        tag = f"slice[{i}]"
        # HEVC slice_type ue(v): 0=B, 1=P, 2=I.
        if o.get("slice_type") in HEVC_SLICE_MAP and f.get("slice_type") is not None:
            if HEVC_SLICE_MAP[o["slice_type"]] != f["slice_type"]:
                checks.append(f"{tag} slice_type: ours={o['slice_type']}({HEVC_SLICE_MAP.get(o['slice_type'])}) ref={f['slice_type']}")
        add(checks, tag + " poc_lsb", o.get("poc_lsb"), f.get("slice_pic_order_cnt_lsb"))
        add(checks, tag + " qp_delta", o.get("slice_qp_delta"), f.get("slice_qp_delta"))
        add(checks, tag + " first_slice", o.get("first_slice"), f.get("first_slice_segment_in_pic_flag"))
        add(checks, tag + " dep_slice", o.get("dependent_slice"), f.get("dependent_slice_segment_flag"))
    if len(ours_slices) != ff_slice_count and ff_slice_count > 0:
        checks.append(f"slice count: ours={len(ours_slices)} ref={ff_slice_count}")


def first_group(groups, kind_prefix):
    for g in groups:
        if g["kind"].startswith(kind_prefix):
            return g
    return None


def main():
    args = sys.argv[1:]
    generate = False
    if args and args[0] == "--generate":
        generate = True
        args = args[1:]
    if not args:
        print("usage: python tools/compare_report.py [--generate] <file...>")
        return 1

    for path in args:
        report = load_report(path)
        if report is None:
            print(f"=== {path}: bs_cli produced no JSON")
            continue
        if generate:
            try:
                generate_reference(path)
            except subprocess.TimeoutExpired:
                print(f"=== {path}: reference generation timed out")
            continue
        ref = load_reference(path)
        if ref is None:
            print(f"=== {path}: no reference file; run with --generate first")
            continue
        codec = report.get("codec")
        checks = []
        if codec == "AVC":
            compare_avc(report, ref, checks)
        elif codec == "HEVC":
            compare_hevc(report, ref, checks)
        else:
            print(f"=== {path}: codec {codec} not compared")
            continue
        if not checks:
            print(f"PASS  {path} ({codec}): all compared fields match")
        else:
            print(f"FAIL  {path} ({codec}): {len(checks)} mismatch(es)")
            for c in checks[:12]:
                print(f"        {c}")
            if len(checks) > 12:
                print(f"        ... and {len(checks) - 12} more")
    return 0


if __name__ == "__main__":
    sys.exit(main())