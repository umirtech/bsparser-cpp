#!/usr/bin/env python3
"""Download the real-world samples described in tests/real/manifest.json.

Files already present and size-matching are skipped, so re-running is
idempotent and does not require re-browsing the samples.mplayerhq.hu
collection.

Usage:
    python tools/download_real_samples.py          # fetch missing files
    python tools/download_real_samples.py --all     # force re-download
"""

import json
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "tests", "real", "manifest.json")


def main():
    force = "--all" in sys.argv

    with open(MANIFEST, encoding="utf-8") as f:
        manifest = json.load(f)

    samples = manifest["samples"]
    missing = []
    for s in samples:
        if not s.get("url"):
            continue
        local = os.path.join(ROOT, s["local"].replace("/", os.sep))
        want = s.get("size_bytes", 0)
        if not force and os.path.isfile(local) and (want == 0 or os.path.getsize(local) == want):
            continue
        missing.append((s, local))

    if not missing:
        print("all samples present and size-verified")
        return 0

    for s, local in missing:
        os.makedirs(os.path.dirname(local), exist_ok=True)
        print(f"downloading {s['name']} ...")
        try:
            req = urllib.request.Request(s["url"], headers={"User-Agent": "bsparser-test"})
            with urllib.request.urlopen(req, timeout=120) as r, open(local, "wb") as out:
                out.write(r.read())
        except Exception as e:  # noqa: BLE001
            print(f"  FAILED: {e}")
            continue

        got = os.path.getsize(local)
        if want and got != want:
            print(f"  size mismatch: expected {want}, got {got}")

    print(f"done: {len(missing)} sample(s) processed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
