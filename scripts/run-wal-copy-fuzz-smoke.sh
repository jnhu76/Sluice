#!/usr/bin/env bash
# Smoke runner for the WAL + copy_all fuzz targets.
#
# Bounded PR-smoke mode with one clear policy: EXACT tracked-corpus replay first
# (every committed seed, deterministically), THEN a short fixed-run random
# session per target. There is no claim of "long fuzzing" here; long-running
# campaigns are a separate, scheduled activity (see the overnight campaign
# policy in docs/verification/wal-copy-fuzz-mutation-evidence.md).
#
#   1. Requires Clang (libFuzzer is Clang-only).
#   2. Configures a dedicated instrumented Clang debug build.
#   3. Builds all three fuzz targets (and the instrumented sluice_core_fuzz).
#   4. Replays the committed seed corpus EXACTLY via
#      scripts/replay-wal-copy-fuzz-corpus.sh (git ls-files, one seed at a time).
#   5. Runs a SHORT bounded random session per target with a target-specific
#      -max_len (the three targets have materially different resource profiles).
#   6. Prints exact seed + artifact directories.
#   7. Returns non-zero on any crash or timeout.
#
# Artifacts go under fuzz/artifacts/<target>/ (gitignored).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

# --- Recommended default smoke settings (resource-bounded). ---
TIMEOUT=2
RSS_LIMIT_MB=1024
SMOKE_RUNS=1000

ARTIFACTS_DIR="${PROJECT_ROOT}/fuzz/artifacts"
DICT="${PROJECT_ROOT}/fuzz/dictionaries/wal_record.dict"

# Per-target max_len profiles (§19). The three targets have materially different
# resource profiles: the WAL decoder handles up to 1 MiB streams; the round-trip
# target allocates two encodings plus a decode, so a smaller cap is appropriate;
# the copy target's payload is config bytes plus the semantic source, bounded by
# the documented 8 KiB profile.
declare -A MAXLEN=(
    [wal_read_record_fuzz]=1048576
    [wal_roundtrip_fuzz]=262144
    [copy_all_fault_fuzz]=8192
)

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

# --- Exact tracked-corpus replay first (deterministic baseline). ---
echo ""
echo "=== EXACT TRACKED-CORPUS REPLAY ==="
bash "${SCRIPT_DIR}/replay-wal-copy-fuzz-corpus.sh"

# --- Common libFuzzer flags for the bounded random session. ---
run_smoke() {
    local name="$1"
    local binary="${PROJECT_ROOT}/build/linux/x86_64/debug/${name}"
    local corpus="${PROJECT_ROOT}/fuzz/corpus/${name#wal_}"
    # The copy target's directory name differs from the binary suffix.
    if [[ "$name" == "copy_all_fault_fuzz" ]]; then
        corpus="${PROJECT_ROOT}/fuzz/corpus/copy_all_fault"
    elif [[ "$name" == "wal_read_record_fuzz" ]]; then
        corpus="${PROJECT_ROOT}/fuzz/corpus/wal_read_record"
    elif [[ "$name" == "wal_roundtrip_fuzz" ]]; then
        corpus="${PROJECT_ROOT}/fuzz/corpus/wal_roundtrip"
    fi
    local artifact_dir="${ARTIFACTS_DIR}/${name}"
    mkdir -p "${artifact_dir}"

    if [[ ! -x "${binary}" ]]; then
        echo "ERROR: binary not found: ${binary}" >&2
        exit 1
    fi

    echo ""
    echo "=== ${name}: bounded smoke (runs=${SMOKE_RUNS}, max_len=${MAXLEN[$name]}) ==="
    # A short random session seeded from the corpus. -runs bounds it so the
    # smoke is fast and CI-stable; the target-specific -max_len bounds input
    # size per the documented resource profile.
    "${binary}" "${corpus}" \
        "-timeout=${TIMEOUT}" \
        "-rss_limit_mb=${RSS_LIMIT_MB}" \
        "-max_len=${MAXLEN[$name]}" \
        -runs="${SMOKE_RUNS}" \
        -artifact_prefix="${artifact_dir}/" \
        -dict="${DICT}"
}

CORPUS_WAL_RAW="${PROJECT_ROOT}/fuzz/corpus/wal_read_record"
CORPUS_WAL_RTP="${PROJECT_ROOT}/fuzz/corpus/wal_roundtrip"
CORPUS_COPY="${PROJECT_ROOT}/fuzz/corpus/copy_all_fault"

echo ""
echo "SEED CORPORA (tracked):"
echo "  wal_read_record:   $(git ls-files fuzz/corpus/wal_read_record | wc -l) files"
echo "  wal_roundtrip:     $(git ls-files fuzz/corpus/wal_roundtrip | wc -l) files"
echo "  copy_all_fault:    $(git ls-files fuzz/corpus/copy_all_fault | wc -l) files"
echo "ARTIFACT DIR: ${ARTIFACTS_DIR}"

run_smoke wal_read_record_fuzz
run_smoke wal_roundtrip_fuzz
run_smoke copy_all_fault_fuzz

echo ""
echo "=== SMOKE PASSED: corpus replayed exactly and bounded smoke ran without crashing ==="
