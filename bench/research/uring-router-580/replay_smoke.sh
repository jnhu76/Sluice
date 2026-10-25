#!/usr/bin/env bash
# #580 replay smoke (research-only, deterministic). Two scenarios:
#   probe OUT_DIR  one instrumented lookup_probe run; asserts the archived
#                  trigger invariants still hold: ok, 1 lookup per install,
#                  zero natural misses at the op_cqe site.
#   cache OUT_DIR  one warm run each of the cache baseline and the clflush
#                  arm; asserts both complete and emit a timed JSON record.
# Evidence-quality replays use the full matrix runners, not this smoke.
set -euo pipefail
cd "$(dirname "$0")/../../.."

WHAT="${1:?usage: replay_smoke.sh probe|cache OUT_DIR}"
OUT_DIR="${2:?usage: replay_smoke.sh probe|cache OUT_DIR}"
mkdir -p "$OUT_DIR"

check_json() { python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$1"; }

case "$WHAT" in
probe)
    BIN=build-research/lookup_probe
    [ -x "$BIN" ] || { echo "missing $BIN (build via prepare-research-worktree.sh)" >&2; exit 2; }
    LOG="$OUT_DIR/trigger-smoke.json"
    taskset -c 2 "$BIN" --scenario pipeline --capacity 64 --depth 16 \
        --ops 2000 --payload 4096 --fixture-dir /tmp >"$LOG"
    check_json "$LOG"
    python3 - "$LOG" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["ok"] is True, f"probe reported failure: {d.get('failure')}"
assert d["installs"] > 0, "no router installs recorded"
assert d["sites"]["op_cqe"]["misses"] == 0, "natural op_cqe misses appeared"
ratio = d["sites"]["op_cqe"]["attempts"] / d["installs"]
assert 0.9 <= ratio <= 1.1, f"lookups/install drifted from 1.0: {ratio:.3f}"
print(f"probe smoke OK: lookups/install={ratio:.3f}, misses=0, avg_probes={d['sites']['op_cqe']['avg_probes']:.1f}")
PY
    ;;
cache)
    for arm in cache_ab cache_ab_flush; do
        BIN="build-research/$arm"
        [ -x "$BIN" ] || { echo "missing $BIN (build via prepare-research-worktree.sh)" >&2; exit 2; }
        LOG="$OUT_DIR/cache-smoke-$arm.json"
        timeout 300 taskset -c 2 "$BIN" --scenario pipeline --depth 64 \
            --ops 2000 --warmup 200 --pin-mode single --cpu-a 2 \
            --label "smoke_$arm" >"$LOG"
        check_json "$LOG"
        python3 - "$LOG" "$arm" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["ok"] is True, f"{sys.argv[2]}: run reported failure: {d.get('failure')}"
assert d["completed_ops"] == 2000, f"{sys.argv[2]}: completed {d['completed_ops']} of 2000"
assert not d.get("warmup_failure"), f"{sys.argv[2]}: warmup failure: {d['warmup_failure']}"
assert d["wall_s"] > 0, f"{sys.argv[2]}: no timing record"
assert d["ops"] == 2000, f"{sys.argv[2]}: ops mismatch {d['ops']}"
print(f"{sys.argv[2]} smoke OK: wall_s={d['wall_s']:.3f} ok=true completed=2000 warmup-clean")
PY
    done
    ;;
*)
    echo "unknown scenario: $WHAT" >&2; exit 64 ;;
esac
