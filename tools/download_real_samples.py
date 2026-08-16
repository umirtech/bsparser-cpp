#!/usr/bin/env python3
"""Download the real-world samples described in tests/real/manifest.json.

Files already present and size-matching are skipped, so re-running is
idempotent and does not require re-browsing the samples.mplayerhq.hu
collection.

Downloads retry with exponential backoff and resume partial files (the sample
server is prone to timeouts / connection resets from CI runners).  Returns a
non-zero exit status if any sample could not be fetched after all retries, so
callers can tolerate (or fail on) missing files as they prefer.

Usage:
    python tools/download_real_samples.py          # fetch missing files
    python tools/download_real_samples.py --all     # force re-download
"""

import json
import os
import random
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "tests", "real", "manifest.json")

MAX_ATTEMPTS = 5
BACKOFF_BASE = 2.0
TIMEOUT = 300
CHUNK = 1 << 20


def fetch(url, local, want):
    """Download `url` to `local`, resuming and retrying on failure.

    Returns True on success, False after exhausting all attempts.  `want` is
    the expected size in bytes (0 = unknown); a file of the right size is
    treated as complete.
    """
    for attempt in range(1, MAX_ATTEMPTS + 1):
        if want and os.path.isfile(local) and os.path.getsize(local) == want:
            return True
        try:
            resume = os.path.getsize(local) if os.path.exists(local) else 0
            headers = {"User-Agent": "bsparser-test"}
            if resume:
                headers["Range"] = f"bytes={resume}-"
            req = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
                mode = "ab" if resume and r.status == 206 else "wb"
                if r.status == 200:
                    resume = 0
                with open(local, mode) as out:
                    while True:
                        chunk = r.read(CHUNK)
                        if not chunk:
                            break
                        out.write(chunk)
            if want and os.path.getsize(local) != want:
                raise OSError(
                    f"size mismatch: expected {want}, got {os.path.getsize(local)}")
            return True
        except urllib.error.HTTPError as e:
            if e.code == 416:  # range not satisfiable: local file already longer
                os.remove(local)
                continue
            print(f"  retry {attempt}/{MAX_ATTEMPTS} failed ({e})")
        except Exception as e:  # noqa: BLE001
            print(f"  retry {attempt}/{MAX_ATTEMPTS} failed ({e})")
        if attempt < MAX_ATTEMPTS:
            time.sleep(BACKOFF_BASE ** attempt + random.uniform(0, 1))
    return False


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

    failed = 0
    for s, local in missing:
        os.makedirs(os.path.dirname(local), exist_ok=True)
        print(f"downloading {s['name']} ...")
        if fetch(s["url"], local, s.get("size_bytes", 0)):
            print(f"  ok ({os.path.getsize(local)} bytes)")
        else:
            print(f"  FAILED: could not download after {MAX_ATTEMPTS} attempts")
            failed += 1

    print(f"done: {len(missing)} sample(s) processed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
