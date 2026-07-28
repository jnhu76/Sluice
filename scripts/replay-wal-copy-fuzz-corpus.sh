#!/usr/bin/env bash
# Exact tracked-corpus replay for the WAL + copy_all fuzz targets.
#
# Replays ONLY the corpus files tracked in git (via 'git ls-files'), one seed at
# a time per target, against the currently-built fuzz binaries. This is the
# deterministic regression gate: it does NOT enter open-ended fuzzing mode and
# does NOT rely on libFuzzer's directory-walk '-runs=1' behavior (which does not
# guarantee each committed seed exactly once and may merge/trim).
#
# Uses:
#   - baseline preflight for the mutation proof (scripts/run-wal-copy-mutations.sh)
#   - local deterministic corpus verification
#   - future CI fuzz replay
#
# Requirements:
#   - Clang fuzz binaries already built (xmake build -g fuzz).
#   - Run from a worktree of the repository (uses git ls-files for corpus
#     authority).
#
# Returns non-zero on the first failing seed, printing the target and seed.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "$PROJECT_ROOT"

# Per-target max_len profiles (§19): the three targets have materially different
# resource profiles.
declare -A MAXLEN=(
    [wal_read_record_fuzz]=1048576
    [wal_roundtrip_fuzz]=262144
    [copy_all_fault_fuzz]=8192
)

# target|corpus-rel pairs.
declare -a PAIRS=(
    "wal_read_record_fuzz|fuzz/corpus/wal_read_record"
    "wal_roundtrip_fuzz|fuzz/corpus/wal_roundtrip"
    "copy_all_fault_fuzz|fuzz/corpus/copy_all_fault"
)

# Resolve the directory containing the built binaries for the current xmake
# config. The standard Linux debug path is build/linux/x86_64/debug; if a
# different config is active, fall back to xmake's target directory.
BIN_DIR="${PROJECT_ROOT}/build/linux/x86_64/debug"

if ! command -v clang++ >/dev/null 2>&1; then
    echo "ERROR: clang++ not found on PATH (libFuzzer is Clang-only)." >&2
    exit 1
fi

FAIL=0
TOTAL=0
echo "EXACT TRACKED-CORPUS REPLAY"
echo "Corpus authority: git ls-files (only tracked seeds are replayed)"
echo ""

for pair in "${PAIRS[@]}"; do
    target="${pair%%|*}"
    corpus="${pair#*|}"
    binary="${BIN_DIR}/${target}"
    if [[ ! -x "$binary" ]]; then
        echo "ERROR: binary not found: ${binary} (build with 'xmake build -g fuzz')" >&2
        exit 1
    fi
    # Enumerate tracked seeds only. git ls-files is NUL-separated; convert to
    # newline-separated absolute paths.
    listed=$(git ls-files -z -- "$corpus" | tr '\0' '\n' | sort)
    if [[ -z "$listed" ]]; then
        echo "ERROR: no tracked files under ${corpus}" >&2
        exit 1
    fi

    target_total=0
    target_fail=0
    while IFS= read -r rel; do
        [[ -z "$rel" ]] && continue
        seed="${PROJECT_ROOT}/${rel}"
        target_total=$((target_total + 1))
        set +e
        "$binary" "$seed" -runs=1 -timeout=10 -rss_limit_mb=1024 \
            -max_len="${MAXLEN[$target]}" >/dev/null 2>&1
        rc=$?
        set -e
        if [[ $rc -ne 0 ]]; then
            echo "FAIL ${target} seed=${rel} rc=${rc}"
            target_fail=$((target_fail + 1))
            FAIL=1
        fi
    done <<< "$listed"
    TOTAL=$((TOTAL + target_total))
    if [[ $target_fail -eq 0 ]]; then
        echo "PASS ${target}: ${target_total} tracked seeds"
    else
        echo "FAIL ${target}: ${target_fail}/${target_total} seeds failed"
    fi
done

echo ""
if [[ $FAIL -ne 0 ]]; then
    echo "VERDICT: FAIL (${TOTAL} seeds checked)"
    exit 1
fi
echo "VERDICT: PASS (${TOTAL} tracked seeds replayed exactly)"
