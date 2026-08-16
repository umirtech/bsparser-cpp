#!/usr/bin/env python3
"""Download the real-world samples described in tests/real/manifest.json.

Files already present and size-matching are skipped, so re-running is
idempotent and does not require re-browsing the samples.mplayerhq.hu
collection.

Downloads retry with capped exponential backoff, resume partial files and
give each file a wall-clock deadline (the sample server is prone to slow /
dropped transfers from CI runners).  Returns a non-zero exit status if any
sample could not be fetched, so callers can tolerate (or fail on) missing
files as they prefer.

Usage:
    python tools/download_real_samples.py          # fetch missing files
    python tools/download_real_samples.py --all     # force re-download
    python tools/download_real_samples.py --timeout 120 --attempts 20
"""

import http.client
import json
import os
import random
import socket
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "tests", "real", "manifest.json")

MAX_ATTEMPTS = 15      # per-file attempts before giving up
TIMEOUT = 90           # per-read socket timeout (s): a stall is retried via resume
BACKOFF_BASE = 1.5     # exponential backoff base (s)
BACKOFF_MAX = 15.0     # backoff cap (s)
FILE_DEADLINE = 900    # per-file wall-clock deadline (s)
CHUNK = 1 << 19        # 512 KiB read chunks

USER_AGENT = "bsparser-test"


def _candidate_urls(url):
    """Prefer https, but allow an http fallback (plain http is often more
    stable than TLS on the sample server)."""
    yield url
    if url.startswith("https://"):
        yield "http://" + url[len("https://"):]


def _retryable(e):
    """Errors worth an immediate resume+retry (dropped / stalled transfer)."""
    return isinstance(
        e,
        (
            socket.timeout,
            TimeoutError,
            http.client.IncompleteRead,
            ConnectionResetError,
            BrokenPipeError,
            OSError,
            urllib.error.URLError,
            urllib.error.HTTPError,
        ),
    )


def fetch(url, local, want):
    """Download `url` to `local`, resuming and retrying on failure.

    Returns True on success, False after exhausting attempts / the deadline.
    `want` is the expected size in bytes (0 = unknown); a file of the right
    size is treated as complete.
    """
    deadline = time.monotonic() + FILE_DEADLINE
    attempt = 0
    last_err = None

    while True:
        if want and os.path.isfile(local) and os.path.getsize(local) == want:
            return True

        attempt += 1
        if attempt > MAX_ATTEMPTS:
            break
        if time.monotonic() >= deadline:
            print(f"  giving up: per-file deadline ({FILE_DEADLINE}s) reached")
            return False

        try:
            resume = os.path.getsize(local) if os.path.exists(local) else 0
            got = False

            for candidate in _candidate_urls(url):
                try:
                    headers = {"User-Agent": USER_AGENT}
                    if resume:
                        headers["Range"] = f"bytes={resume}-"
                    req = urllib.request.Request(candidate, headers=headers)
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
                    got = True
                    break
                except Exception as e:  # noqa: BLE001  (try the next scheme)
                    last_err = e
                    resume = os.path.getsize(local) if os.path.exists(local) else 0

            if not got:
                raise last_err

            if want and os.path.getsize(local) != want:
                raise OSError(
                    f"size mismatch: expected {want}, got {os.path.getsize(local)}")
            return True

        except urllib.error.HTTPError as e:
            if e.code == 416:  # range not satisfiable: local file already longer
                os.remove(local)
                continue
            last_err = e
            print(f"  attempt {attempt}/{MAX_ATTEMPTS} failed ({e})")
        except Exception as e:  # noqa: BLE001
            last_err = e
            if _retryable(e):
                print(f"  attempt {attempt}/{MAX_ATTEMPTS} failed ({e}) - resuming")
            else:
                print(f"  attempt {attempt}/{MAX_ATTEMPTS} failed ({e})")

        if attempt < MAX_ATTEMPTS:
            sleep_s = min(BACKOFF_BASE ** attempt + random.uniform(0, 1), BACKOFF_MAX)
            time.sleep(sleep_s)

    print(f"  FAILED after {MAX_ATTEMPTS} attempts (last error: {last_err})")
    return False


def main():
    force = "--all" in sys.argv

    # Optional tuning knobs (defaults are CI-proven).
    for arg, dest in (("--timeout", "timeout"), ("--attempts", "attempts")):
        global TIMEOUT, MAX_ATTEMPTS  # noqa: PLW0603
        if arg in sys.argv:
            idx = sys.argv.index(arg)
            value = int(sys.argv[idx + 1])
            if dest == "timeout":
                TIMEOUT = value
            else:
                MAX_ATTEMPTS = value

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
            print(f"  FAILED: could not download {s['name']}")
            failed += 1

    print(f"done: {len(missing)} sample(s) processed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
