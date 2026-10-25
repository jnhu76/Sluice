#!/usr/bin/env bash
# #580 research reproduction entry point. The PR branch carries zero src/
# diff; every research binary is rebuilt from the frozen base plus the
# archived gate patch inside a throwaway git worktree.
#
#   verify (default)  worktree + patch + gate check + builds + provenance receipt
#   replay            verify + oracle RED/GREEN replay + probe/cache smoke runs
#
# Never touches the caller's checkout or docs/research results; all replay
# output stays under <dest>/build-research/. Historical receipts and rebuilt
# binaries carry different hashes by construction and are never written over
# the archived evidence.
#
# Flags: --mode verify|replay  --dest DIR  --keep  --receipt-out FILE
set -euo pipefail

BASE_SHA=46157fd1f4a1a4afec22d8834c86462b245b576b
PATCH_REL=docs/research/uring-router-580/patches/research-gates-full.patch
PATCH_SHA256=3bb61fa211e62721e13b0020cf7a9a67009195d2c1b5375330edcdd4e8fdd581
BENCH_REL=bench/research/uring-router-580

MODE=verify
DEST=
KEEP=0
RECEIPT_OUT=
while [ $# -gt 0 ]; do
    case "$1" in
        --mode) MODE="${2:?}"; shift 2 ;;
        --dest) DEST="${2:?}"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        --receipt-out) RECEIPT_OUT="${2:?}"; shift 2 ;;
        *) echo "unknown flag: $1" >&2; exit 64 ;;
    esac
done
case "$MODE" in verify|replay) ;; *) echo "--mode must be verify or replay" >&2; exit 64 ;; esac

REPO_ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
if ! git -C "$REPO_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "not a git repository: $REPO_ROOT" >&2; exit 2
fi
if ! git -C "$REPO_ROOT" cat-file -e "$BASE_SHA^{commit}" 2>/dev/null; then
    echo "frozen base $BASE_SHA not present; fetch the repository history" >&2; exit 2
fi

PATCH_FILE="$REPO_ROOT/$PATCH_REL"
if [ ! -f "$PATCH_FILE" ]; then
    echo "missing $PATCH_REL — run this script from the research branch of PR #592" >&2; exit 2
fi
GOT_SHA=$(sha256sum "$PATCH_FILE" | cut -d' ' -f1)
if [ "$GOT_SHA" != "$PATCH_SHA256" ]; then
    echo "patch identity mismatch: expected $PATCH_SHA256, got $GOT_SHA" >&2
    echo "the archived gate patch changed; update PATCH_SHA256 with owner review" >&2; exit 2
fi

if [ -z "$DEST" ]; then
    DEST=$(mktemp -d /tmp/sluice580-research.XXXXXX)
    MADE_TMP_DEST=1
else
    MADE_TMP_DEST=0
fi
if [ -e "$DEST" ] && [ "$(ls -A "$DEST" 2>/dev/null)" ]; then
    echo "dest exists and is not empty: $DEST" >&2; exit 2
fi

WORKTREE_OWNED=0
cleanup() {
    if [ "$WORKTREE_OWNED" = 1 ] && [ "$KEEP" = 0 ]; then
        git -C "$REPO_ROOT" worktree remove --force "$DEST" >/dev/null 2>&1 || rm -rf "$DEST"
    elif [ "$MADE_TMP_DEST" = 1 ] && [ "$KEEP" = 0 ]; then
        rm -rf "$DEST"
    fi
}
trap cleanup EXIT

git -C "$REPO_ROOT" worktree add --detach "$DEST" "$BASE_SHA" >/dev/null
WORKTREE_OWNED=1
mkdir -p "$DEST/bench/research"
cp -r "$REPO_ROOT/$BENCH_REL" "$DEST/$BENCH_REL"

cd "$DEST"
git apply "$PATCH_FILE"

GATE_HITS=$(grep -c 'defined(SLUICE_URING_LOOKUP_INSTRUMENT)' src/async/uring_internal.hpp src/async/uring_completion.cpp src/async/uring_transport.cpp | awk -F: '{s+=$2} END{print s}')
if [ "$GATE_HITS" -ne 17 ]; then
    echo "instrument gate sites drifted after apply (found $GATE_HITS, expected 17)" >&2; exit 2
fi
for gate in SLUICE_580_WE2_REVERSE_CANCEL_SCAN SLUICE_580_MUTANT_FALSE_MISS \
            SLUICE_580_MUTANT_SLOT_ONLY SLUICE_580_CLFLUSH_ROUTER; do
    grep -q "defined($gate)" src/async/uring_transport.cpp \
        || { echo "gate $gate missing after apply" >&2; exit 2; }
done
if ! git apply -R --check "$PATCH_FILE"; then
    echo "patch does not verify as reversible on the applied tree" >&2; exit 2
fi

mkdir -p build-research/logs
B="$BENCH_REL"
run_logged() {
    local log="build-research/logs/$1.log"; shift
    echo "== $* -> $log"
    if ! "$@" >"$log" 2>&1; then
        local keep="$REPO_ROOT/build-research/replay-archive/failed-$(date -u +%Y%m%dT%H%M%SZ)"
        mkdir -p "$keep"
        cp "$log" "$keep/" 2>/dev/null || true
        echo "step failed; log preserved at $keep/$1.log" >&2
        exit 1
    fi
}
run_logged build-arms    bash "$B/build_arms.sh"
run_logged build-probe   bash "$B/build_probe.sh"
run_logged build-cache   bash "$B/build_cache.sh"
run_logged build-oracle  bash "$B/run_oracle_variants.sh" --phase build

REPLAY_RESULT=NOT_RUN
if [ "$MODE" = replay ]; then
    mkdir -p build-research/replay
    run_logged replay-oracle bash "$B/run_oracle_variants.sh" --phase run \
        --out-dir build-research/replay
    run_logged replay-probe  bash "$B/replay_smoke.sh" probe  build-research/replay
    run_logged replay-cache  bash "$B/replay_smoke.sh" cache  build-research/replay
    REPLAY_RESULT=PASS
fi

python3 - "$MODE" "$BASE_SHA" "$PATCH_SHA256" "$GATE_HITS" "$REPLAY_RESULT" \
        "$RECEIPT_OUT" <<'PY'
import hashlib, json, os, subprocess, sys, time
mode, base, patch_sha, gates, replay, receipt_out = sys.argv[1:]
root = os.getcwd()
bins = {}
for name in sorted(os.listdir("build-research")):
    p = os.path.join("build-research", name)
    if os.path.isfile(p) and not name.endswith((".log", ".json", ".stamp")):
        with open(p, "rb") as fh:
            bins[name] = hashlib.sha256(fh.read()).hexdigest()
receipt = {
    "what": "sluice #580 research worktree receipt (rebuilt, not historical)",
    "mode": mode, "base_sha": base, "patch_sha256": patch_sha,
    "instrument_gate_sites": int(gates),
    "gates": ["SLUICE_URING_LOOKUP_INSTRUMENT", "SLUICE_580_WE2_REVERSE_CANCEL_SCAN",
              "SLUICE_580_MUTANT_FALSE_MISS", "SLUICE_580_MUTANT_SLOT_ONLY",
              "SLUICE_580_CLFLUSH_ROUTER"],
    "compiler": subprocess.run(["g++", "--version"], capture_output=True,
                               text=True).stdout.splitlines()[0],
    "binaries": bins, "replay": replay, "timestamp_utc": time.strftime(
        "%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
}
path = os.path.join("build-research", "prepared-receipt.json")
with open(path, "w") as fh:
    json.dump(receipt, fh, indent=1)
    fh.write("\n")
if receipt_out:
    with open(receipt_out, "w") as fh:
        json.dump(receipt, fh, indent=1)
        fh.write("\n")
print(json.dumps({"receipt": path, "binaries": len(bins), "mode": mode}))
PY

if [ "$KEEP" = 1 ]; then
    echo "worktree retained at $DEST (remove with: git -C $REPO_ROOT worktree remove --force $DEST)"
else
    STAMP=$(date -u +%Y%m%dT%H%M%SZ)
    KEEP_DIR="$REPO_ROOT/build-research/replay-archive/$STAMP"
    mkdir -p "$KEEP_DIR"
    cp "$DEST/build-research/prepared-receipt.json" "$KEEP_DIR/" 2>/dev/null || true
    cp -r "$DEST/build-research/replay" "$KEEP_DIR/" 2>/dev/null || true
    cp -r "$DEST/build-research/logs" "$KEEP_DIR/" 2>/dev/null || true
    echo "receipts preserved at $KEEP_DIR (worktree itself removed)"
fi
