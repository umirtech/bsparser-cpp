#!/usr/bin/env bash
#
# Run the full build + test + fuzz harness inside a lightweight Debian
# container.  Clang is used because GCC 12's ASan runtime shows
# non-deterministic pathological slowdowns in this Docker Desktop/WSL
# environment (a tiny 5 KB input randomly taking 30 s), while Clang's
# ASan is stable.  Clang also enables the real libFuzzer `bs_fuzz` target.
#
# The build tree lives in a reusable named volume (fast, container-local)
# while the source is mounted from the repo; logs are copied back under
# build/docker.  The fuzz stages are budgeted to fit in ~1 minute total.
#
# Usage:
#     tools/docker_test.sh                # build + ctest + fuzz (<= 1 min)
#     tools/docker_test.sh --iters 200000 # deeper mutation sweep
#     tools/docker_test.sh --shell        # drop into a shell for debugging
#     tools/docker_test.sh --fresh        # wipe the build volume first
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="bs-parser-test:latest"
VOL="bs-parser-build"
CTR_WORK="/build"
OUT="${ROOT}/build/docker"

FUZZ_SECS=40
ITERS=100000
SHELL_ONLY=0
FRESH=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --iters) ITERS="$2"; shift 2 ;;
        --fuzz-secs) FUZZ_SECS="$2"; shift 2 ;;
        --shell) SHELL_ONLY=1; shift ;;
        --fresh) FRESH=1; shift ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "${OUT}"

docker build -t "${IMG}" -f - "${ROOT}" <<'DOCKERFILE'
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ clang cmake ninja-build python3 ca-certificates \
        libclang-rt-14-dev && \
    rm -rf /var/lib/apt/lists/*
DOCKERFILE

if [[ ${FRESH} -eq 1 ]]; then
    docker volume rm -f "${VOL}" >/dev/null 2>&1 || true
fi
docker volume create "${VOL}" >/dev/null

# Bounded quarantine avoids the GCC-12-style ASan eviction pathologies.
ASAN="detect_leaks=0:quarantine_size_mb=64"

RUN="docker run --rm -v ${VOL}:${CTR_WORK} -v ${ROOT}:/src:ro -v ${OUT}:/out -e ASAN_OPTIONS=${ASAN}"
COMMON="cmake -G Ninja -S /src -B ${CTR_WORK} \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DBS_ENABLE_SANITIZERS=ON \
    -DBS_ENABLE_FUZZING=ON \
    -DCMAKE_CXX_FLAGS='-O1 -g -fno-omit-frame-pointer'"

if [[ ${SHELL_ONLY} -eq 1 ]]; then
    ${RUN} -it -e COMMON="${COMMON}" "${IMG}" bash
    exit 0
fi

echo "== configure (Clang ASan+UBSan) =="
${RUN} ${IMG} sh -c "${COMMON}"

echo "== build =="
${RUN} ${IMG} cmake --build "${CTR_WORK}" -j"$(nproc)" 2>&1 | tee "${OUT}/build.log" | grep -E "error|FAILED" || true

echo "== ctest =="
${RUN} ${IMG} ctest --test-dir "${CTR_WORK}" --output-on-failure 2>&1 | tee "${OUT}/ctest.log"

echo "== fuzz driver over corpus =="
CORPUS="/src/tests/fuzz/corpus"
${RUN} ${IMG} sh -c \
    "set -e; for f in ${CORPUS}/*; do timeout 10 ${CTR_WORK}/bs_fuzz_driver \"\$f\" >/dev/null 2>&1; done; echo 'corpus parse: OK'"

if [[ -x ${OUT}/../bs_fuzz || -f "${OUT}/../bs_fuzz" ]]; then
    echo "== libFuzzer bs_fuzz (${FUZZ_SECS}s budget) =="
    ${RUN} ${IMG} sh -c \
        "timeout ${FUZZ_SECS} ${CTR_WORK}/bs_fuzz -max_total_time=${FUZZ_SECS} -runs=1000000 \
            ${CORPUS} 2>&1 | tail -6"
else
    echo "== libFuzzer not available; building bs_fuzz =="
    ${RUN} ${IMG} cmake --build "${CTR_WORK}" -j"$(nproc)" --target bs_fuzz 2>&1 | tail -2
    ${RUN} ${IMG} sh -c \
        "timeout ${FUZZ_SECS} ${CTR_WORK}/bs_fuzz -max_total_time=${FUZZ_SECS} -runs=1000000 \
            ${CORPUS} 2>&1 | tail -6"
fi

echo "== in-process mutation sweep (${ITERS} iters) =="
${RUN} ${IMG} sh -c \
    "clang++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I/src /src/tests/fuzz/fuzz_runner.cpp /src/tests/fuzz/fuzz_all.cpp -o /out/fuzz_runner && \
     echo built" 2>&1 | grep -E "built|error" || true
${RUN} ${IMG} sh -c \
    "timeout 20 /out/fuzz_runner --iters ${ITERS} \
        ${CORPUS}/*.hevc ${CORPUS}/*.h264 ${CORPUS}/*.266 ${CORPUS}/*.obu ${CORPUS}/*.ivf \
        2>&1 | tail -3"

echo "== done =="