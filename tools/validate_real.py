#!/usr/bin/env python3
"""Run the real-sample validation suite described in tests/real/manifest.json.

Downloads any missing samples, then runs bs_cli on every file and (where
applicable) cross-checks the report against ffmpeg's trace_headers via
tools/compare_ffmpeg.py.  Exits non-zero if any expected PASS sample fails.

Usage: python tools/validate_real.py
"""

import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "tests", "real", "manifest.json")
BS_CLI = os.path.join(ROOT, "build", "bs_cli" + (".exe" if os.name == "nt" else ""))
COMPARE = os.path.join(ROOT, "tools", "compare_ffmpeg.py")


def run(cmd, timeout=300):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def main():
    with open(MANIFEST, encoding="utf-8") as f:
        manifest = json.load(f)

    samples = manifest["samples"]
    if not samples:
        print("manifest has no samples; run tools/generate_real_manifest.py")
        return 1

    fails = 0
    skipped = 0
    print(f"validating {len(samples)} real samples\n")

    for s in samples:
        path = os.path.join(ROOT, s["local"].replace("/", os.sep))
        if not os.path.isfile(path):
            print(f"[SKIP] {s['name']:<26} missing (download failed; see tools/download_real_samples.py)")
            skipped += 1
            continue

        p = run([BS_CLI, path, "--out", "NUL"])
        ok_line = next((l for l in p.stdout.splitlines()
                        if l.strip().startswith("codec=") and "framing=" in l), "")
        ours = " ".join(ok_line.split()[:2])

        expected = s.get("ffmpeg_trace_match", "N/A")
        status = "skip"
        if expected == "PASS":
            c = run(["python", COMPARE, path])
            status = "PASS" if c.stdout.strip().startswith("PASS") else "FAIL"
            if status == "FAIL":
                fails += 1
        elif expected == "N/A":
            status = "n/a"

        mark = "OK " if status in ("PASS", "n/a") else "FAIL"
        print(f"[{mark}] {s['name']:<26} ours={ours:<6} trace={status} "
              f"(expected {expected})")

    if skipped:
        print(f"\n{len(samples) - fails - skipped}/{len(samples)} samples OK "
              f"({skipped} skipped: not downloaded)")
    else:
        print(f"\n{len(samples) - fails}/{len(samples)} samples OK")
    if fails:
        return 1
    if skipped == len(samples):
        print("no samples available to validate (all downloads failed)")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
