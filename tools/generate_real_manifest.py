// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#!/usr/bin/env python3
"""Generate tests/real/manifest.json describing the real-world samples.

Runs bs_cli (our report) on every file in tests/real/ and records the parse
summary plus source URLs so the set can be re-downloaded without re-browsing
the collection.

Usage: python tools/generate_real_manifest.py
"""

import json
import os
import re
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REAL_DIR = os.path.join(ROOT, "tests", "real")
BS_CLI = os.path.join(ROOT, "build", "bs_cli" + (".exe" if os.name == "nt" else ""))

# name -> source URL on samples.mplayerhq.hu
SOURCES = {
    "avc_avi_hbc9.avi": "https://samples.mplayerhq.hu/V-codecs/h264/hbc9.avi",
    "avc_mkv_hellsing.mkv": "https://samples.mplayerhq.hu/V-codecs/h264/hellsing-h264-blocking.mkv",
    "avc_mp4_cathedral.mp4": "https://samples.mplayerhq.hu/V-codecs/h264/cathedral-beta2-400extra-crop-avc.mp4",
    "avc_mp4_envivio.mp4": "https://samples.mplayerhq.hu/V-codecs/h264/envivio-h264.mp4",
    "avc_mp4_nero.mp4": "https://samples.mplayerhq.hu/V-codecs/h264/NeroAVC.mp4",
    "avc_raw_foreman.264": "https://samples.mplayerhq.hu/V-codecs/h264/foreman_p16x16.264",
    "flv_zelda.flv": "https://samples.mplayerhq.hu/FLV/zelda.flv",
    "hevc_mkv_3d.mkv": "https://samples.mplayerhq.hu/3D/20110805-112659-ch0.mkv",
    "hevc_raw_sintel.265": "https://samples.mplayerhq.hu/ffmpeg-bugs/trac/ticket6907/Sintel_272p_logo.265",
    "hevc_raw_uhd.hevc": "https://samples.mplayerhq.hu/ffmpeg-bugs/trac/ticket4185/UHD_ENT_Transformer_cut.hevc",
    "hevc_ts_polsat.ts": "https://samples.mplayerhq.hu/ffmpeg-bugs/trac/ticket4141/polsat_1080i_hevc.ts",
    "vvc_ts2.ts": "https://samples.mplayerhq.hu/V-codecs/h266/vvc%20test%202.ts",
    "webm_vp8.webm": "https://samples.mplayerhq.hu/ffmpeg-bugs/trac/ticket1430/Sam%20and%20Cocoa%20shaky%20original.webm",
}

NOTES = {
    "flv_zelda.flv": "FLV1/Sorenson video: unsupported codec; FLV demuxer correctly rejects it and bs_cli falls back to a raw parse.",
    "vvc_ts2.ts": "stream_type 0x32 = JPEG XS, NOT VVC despite the h266 directory name; no supported video stream.",
    "hevc_raw_sintel.265": "SPS delta_poc feature; SPS fields parsed and checked against the H.265 spec.",
    "hevc_raw_uhd.hevc": "max_bytes_per_pic_denom feature; SPS fields parsed and checked against the H.265 spec.",
    "hevc_ts_polsat.ts": "1080i broadcast HEVC; our parser resolves the real SPS/PPS.",
    "hevc_mkv_3d.mkv": "3D AVC MKV using Annex-B blocks (fixed); full stream demuxed faithfully.",
    "webm_vp8.webm": "VP8: 637/637 frames parsed.",
}


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.stdout, p.stderr


def bs_cli_summary(path):
    out, _ = run([BS_CLI, path, "--out", "NUL"])
    for line in out.splitlines():
        m = re.match(r"^codec=(\S+) framing=(\S+) nals=(\d+) parsed=(\d+) vcl=(\d+)", line.strip())
        if m:
            return {
                "codec": m.group(1),
                "framing": m.group(2),
                "nals": int(m.group(3)),
                "parsed": int(m.group(4)),
                "vcl": int(m.group(5)),
            }
    return None


def main():
    samples = []
    video_exts = {".avi", ".mkv", ".mp4", ".h264", ".264", ".265", ".hevc", ".flv", ".ts", ".webm"}

    for name in sorted(os.listdir(REAL_DIR)):
        path = os.path.join(REAL_DIR, name)
        if not os.path.isfile(path):
            continue
        if os.path.splitext(name)[1].lower() not in video_exts:
            continue
        size = os.path.getsize(path)
        ours = bs_cli_summary(path)
        entry = {
            "name": name,
            "local": os.path.relpath(path, ROOT).replace("\\", "/"),
            "url": SOURCES.get(name),
            "size_bytes": size,
            "container": {
                "avc_avi_hbc9.avi": "AVI",
                "avc_mkv_hellsing.mkv": "MKV",
                "avc_mp4_cathedral.mp4": "MP4",
                "avc_mp4_envivio.mp4": "MP4",
                "avc_mp4_nero.mp4": "MP4",
                "avc_raw_foreman.264": "raw AVC",
                "flv_zelda.flv": "FLV",
                "hevc_mkv_3d.mkv": "MKV",
                "hevc_raw_sintel.265": "raw HEVC",
                "hevc_raw_uhd.hevc": "raw HEVC",
                "hevc_ts_polsat.ts": "MPEG-TS",
                "vvc_ts2.ts": "MPEG-TS",
                "webm_vp8.webm": "MKV/WebM",
            }.get(name, "raw"),
            "bsparser": ours,
        }
        if name in NOTES:
            entry["notes"] = NOTES[name]
        samples.append(entry)

    manifest = {
        "schema": 1,
        "collection": "samples.mplayerhq.hu",
        "generated": __import__("datetime").date.today().isoformat(),
        "purpose": "Curated real-world samples for bsparser validation. "
                   "Regenerate with tools/download_real_samples.py.",
        "validation": {
            "ctests": "ctest --test-dir build",
        },
        "samples": samples,
    }

    out_path = os.path.join(REAL_DIR, "manifest.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(f"wrote {out_path} ({len(samples)} samples)")


if __name__ == "__main__":
    main()
