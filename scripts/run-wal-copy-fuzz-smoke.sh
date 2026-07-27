#!/usr/bin/env bash
# Smoke runner for the WAL + copy_all fuzz targets.
#
#   1. Requires Clang (libFuzzer is Clang-only).
#   2. Configures a dedicated instrumented Clang debug build.
#   3. Builds all three fuzz targets (and the instrumented sluice_core_fuzz).
#   4. Replays the committed seed corpus deterministically (-runs=1).
#   5. Runs a bounded smoke session for each target.
#   6. Prints exact seed + artifact directories.
#   7. Returns non-zero on any crash or timeout.
#
# Corpus replay comes first so a known-good baseline is established before any
# random mutation. Artifacts go under fuzz/artifacts/<target>/ (gitignored).
set -euo pipefail

# --- Locate the project root (the directory containing this script's parent). \
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

# --- Recommended default smoke settings (resource-bounded). ---
TIMEOUT=2
RSS_LIMIT_MB=1024
MAX_LEN=1048576
SMOKE_RUNS=120
SMOKE_SECONDS=30

ARTIFACTS_DIR="${PROJECT_ROOT}/fuzz/artifacts"
CORPUS_WAL_RAW="${PROJECT_ROOT}/fuzz/corpus/wal_read_record"
CORPUS_WAL_RTP="${PROJECT_ROOT}/fuzz/corpus/wal_roundtrip"
CORPUS_COPY="${PROJECT_ROOT}/fuzz/corpus/copy_all_fault"
DICT="${PROJECT_ROOT}/fuzz/dictionaries/wal_record.dict"

# --- Require Clang. ---
if ! command -v clang++ >/dev/null 2>&1; then
    echo "ERROR: clang++ not found on PATH (libFuzzer is Clang-only)." >&2
    exit 1
fi
TOOLCHAIN="clang"
echo "FUZZ TOOLCHAIN: $(clang++ --version | head -1)"

# --- Configure + build. ---
echo "Configuring: xmake f -m debug --toolchain=${TOOLCHAIN} -y"
xmake f -m debug --toolchain="${TOOLCHAIN}" -y
echo "Building fuzz group..."
xmake build -g fuzz

# --- Common libFuzzer flags (forwarded after `--`). ---
COMMON_OPTS=(
    "-timeout=${TIMEOUT}"
    "-rss_limit_mb=${RSS_LIMIT_MB}"
    "-max_len=${MAX_LEN}"
)

run_target() {
    local name="$1"
    local corpus="$2"
    local extra_opts=("${@:3}")
    local binary="${PROJECT_ROOT}/build/linux/x86_64/debug/${name}"
    local artifact_dir="${ARTIFACTS_DIR}/${name}"
    mkdir -p "${artifact_dir}"

    if [[ ! -x "${binary}" ]]; then
        echo "ERROR: binary not found: ${binary}" >&2
        exit 1
    fi

    echo ""
    echo "=== ${name}: corpus replay ==="
    # -runs=1 replays each corpus entry exactly once (deterministic baseline).
    "${binary}" "${corpus}" -runs=1 "${COMMON_OPTS[@]}" "${extra_opts[@]}"

    echo ""
    echo "=== ${name}: bounded smoke ==="
    # A short random session seeded from the corpus. -runs / -max_total_time
    # bound it so the smoke is fast and CI-stable.
    "${binary}" "${corpus}" \
        "${COMMON_OPTS[@]}" "${extra_opts[@]}" \
        -max_total_time="${SMOKE_SECONDS}" \
        -runs="${SMOKE_RUNS}" \
        -artifact_prefix="${artifact_dir}/" \
        -dict="${DICT}"
}

echo ""
echo "SEED CORPORA:"
echo "  wal_read_record:   $(ls "${CORPUS_WAL_RAW}" | wc -l) files"
echo "  wal_roundtrip:     $(ls "${CORPUS_WAL_RTP}" | wc -l) files"
echo "  copy_all_fault:    $(ls "${CORPUS_COPY}" | wc -l) files"
echo "ARTIFACT DIR: ${ARTIFACTS_DIR}"

run_target wal_read_record_fuzz "${CORPUS_WAL_RAW}"
run_target wal_roundtrip_fuzz "${CORPUS_WAL_RTP}"
run_target copy_all_fault_fuzz "${CORPUS_COPY}"

echo ""
echo "=== SMOKE PASSED: all targets replayed and fuzzed without crashing ==="
