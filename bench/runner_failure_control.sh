#!/usr/bin/env bash
# Negative control for bench/run_matrix.sh.
#
# The runner's job is to abort when a benchmark does not match the frozen
# preregistration. This script injects the failure modes that matter -- a silent
# zero exit, a non-zero exit, a crash, a lost case, a short repetition set, an
# unexplained failure, and a control run that hides its NOT_RUN set -- and
# requires the runner to reject every one of them. Two passing scenarios prove
# the same shims are accepted when nothing is injected, so each rejection is
# attributable to the injected defect rather than to the shim itself.
#
# The committed evidence is never written: every scenario runs into a temp
# directory, and SLUICE_BENCH_BIN_DIR points the runner at the shims.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

evidence="${SLUICE_BENCH_REFERENCE_DIR:-docs/evidence/g0-403-performance-baseline-20261015/raw}"
runner="$root/bench/run_matrix.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

failures=0

write_shim() {
    local dir="$1"
    mkdir -p "$dir/shim"
    for target in bench_w01_direct bench_w02_pipeline bench_w03_external_loop; do
        cp "$work/shim_body.sh" "$dir/shim/$target"
        chmod +x "$dir/shim/$target"
    done
}

make_fixtures() {
    local dir="$1" stage="$2"
    mkdir -p "$dir/fixtures"
    case "$stage" in
        measured)
            cp "$evidence/w01.jsonl" "$dir/fixtures/bench_w01_direct.jsonl"
            cp "$evidence/w02.jsonl" "$dir/fixtures/bench_w02_pipeline.jsonl"
            cp "$evidence/w03.jsonl" "$dir/fixtures/bench_w03_external_loop.jsonl"
            ;;
        notrun)
            cp "$evidence/w02-noring-notrun.jsonl" "$dir/fixtures/bench_w02_pipeline.jsonl"
            cp "$evidence/w03-noring-notrun.jsonl" "$dir/fixtures/bench_w03_external_loop.jsonl"
            ;;
    esac
}

# expect_reject <label> <scenario-dir> <stage> <NC_MODE> [NC_EXIT]
expect_reject() {
    local label="$1" dir="$2" stage="$3" mode="$4" exit_code="${5:-0}"
    local out="$dir/out"
    local rc=0
    NC_MODE="$mode" NC_EXIT="$exit_code" NC_DIR="$dir" SLUICE_BENCH_BIN_DIR="$dir/shim" \
        "$runner" "$out" 2 7 "$stage" "" >"$dir/runner.log" 2>&1 || rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "FAIL: $label: runner exited 0, so the injected failure was reported as a success"
        failures=$((failures + 1))
        return
    fi
    if ! grep -q "^# FAIL:" "$out/matrix-run.log"; then
        echo "FAIL: $label: runner failed without naming the run it rejected"
        failures=$((failures + 1))
        return
    fi
    echo "PASS: $label: runner rejected the run (exit=$rc)"
}

# expect_accept <label> <scenario-dir> <stage> <NC_MODE> [NC_EXIT]
expect_accept() {
    local label="$1" dir="$2" stage="$3" mode="$4" exit_code="${5:-0}"
    local out="$dir/out"
    local rc=0
    NC_MODE="$mode" NC_EXIT="$exit_code" NC_DIR="$dir" SLUICE_BENCH_BIN_DIR="$dir/shim" \
        "$runner" "$out" 2 7 "$stage" "" >"$dir/runner.log" 2>&1 || rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "FAIL: $label: runner rejected a run that matches the frozen expectation (exit=$rc)"
        sed -n '1,20p' "$out/matrix-run.log" | sed 's/^/    /'
        failures=$((failures + 1))
        return
    fi
    echo "PASS: $label: runner accepted the run"
}

cat >"$work/shim_body.sh" <<'SHIM'
#!/usr/bin/env bash
set -euo pipefail
mode="${NC_MODE:?}"
self="$(basename "$0")"
fixture="$NC_DIR/fixtures/$self.jsonl"
case "$mode" in
    silent)      exit 0 ;;
    silent_fail) exit 3 ;;
    crash)       kill -SEGV $$ ;;
    copy)        cp "$fixture" "${SLUICE_BENCH_JSONL:?}"; exit "${NC_EXIT:-0}" ;;
    drop_record) head -n -1 "$fixture" >"${SLUICE_BENCH_JSONL:?}"; exit 0 ;;
    drop_case)   head -n -9 "$fixture" >"${SLUICE_BENCH_JSONL:?}"; exit 0 ;;
    flip_ok)     python3 "$NC_DIR/flip_ok.py" "$fixture" "${SLUICE_BENCH_JSONL:?}"; exit 0 ;;
    flip_notrun) python3 "$NC_DIR/flip_notrun.py" "$fixture" "${SLUICE_BENCH_JSONL:?}"; exit 1 ;;
    *) echo "unknown NC_MODE $mode" >&2; exit 99 ;;
esac
SHIM

cat >"$work/flip_ok.py" <<'PY'
import json
import pathlib
import sys

src, dst = sys.argv[1], sys.argv[2]
lines = pathlib.Path(src).read_text().splitlines()
for i, line in enumerate(lines):
    record = json.loads(line)
    if record["measured"] and record["ok"] and not record["case_id"].startswith("CAL-"):
        record["ok"] = False
        record["failure"] = "injected negative control: a case failed silently"
        lines[i] = json.dumps(record)
        break
pathlib.Path(dst).write_text("\n".join(lines) + "\n")
PY

cat >"$work/flip_notrun.py" <<'PY'
import json
import pathlib
import sys

src, dst = sys.argv[1], sys.argv[2]
lines = pathlib.Path(src).read_text().splitlines()
for i, line in enumerate(lines):
    record = json.loads(line)
    if not record["ok"] and "NOT_RUN_WITH_REASON" in record.get("failure", ""):
        record["ok"] = True
        lines[i] = json.dumps(record)
        break
pathlib.Path(dst).write_text("\n".join(lines) + "\n")
PY

echo "# scenario P0: the unshimmed no-ring control still passes"
dir="$work/p0"; mkdir -p "$dir"
rc=0
"$runner" "$dir/out" 2 7 notrun "" >"$dir/runner.log" 2>&1 || rc=$?
if [ "$rc" -ne 0 ]; then
    echo "FAIL: P0: the real no-ring control was rejected (exit=$rc)"
    tail -20 "$dir/out/matrix-run.log" | sed 's/^/    /'
    failures=$((failures + 1))
else
    echo "PASS: P0: the real no-ring control is accepted"
fi

for stage in measured notrun; do
    dir="$work/pass-$stage"; mkdir -p "$dir"; write_shim "$dir"; make_fixtures "$dir" "$stage"
    if [ "$stage" = "measured" ]; then
        expect_accept "P1: fixture shims accepted when nothing is injected (measured)" \
            "$dir" measured copy 0
    else
        expect_accept "P2: fixture shims accepted when nothing is injected (notrun)" \
            "$dir" notrun copy 1
    fi
done

for spec in "N1:an empty capture with a zero exit:measured:silent:0" \
            "N2:a non-zero exit with no capture:measured:silent_fail:0" \
            "N3:a crashing benchmark:notrun:crash:0" \
            "N4:a lost repetition:measured:drop_record:0" \
            "N5:a lost case:measured:drop_case:0" \
            "N6:a failure with no reason marker:measured:flip_ok:0" \
            "N7:a control run that claims success:notrun:copy:0" \
            "N8:a control run that hides a NOT_RUN case:notrun:flip_notrun:0"; do
    IFS=: read -r label description stage mode exit_code <<<"$spec"
    dir="$work/$label-$stage"
    mkdir -p "$dir"; write_shim "$dir"; make_fixtures "$dir" "$stage"
    expect_reject "$label: $description" "$dir" "$stage" "$mode" "$exit_code"
done

echo
if [ "$failures" -ne 0 ]; then
    echo "runner failure control: $failures of 11 scenarios did not behave as required"
    exit 1
fi
echo "runner failure control: all 11 scenarios behaved as required"
