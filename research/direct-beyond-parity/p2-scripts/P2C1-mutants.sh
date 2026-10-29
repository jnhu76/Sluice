#!/usr/bin/env bash
# P2C1 mutation-kill gate (P2 plan §2.5). For each seam: BASE build -> named
# killer GREEN; mutant target build -> the killer binary fails with the exact
# named assertion (exit 1 + "FAIL: <killer>"; crash/signal/timeout is not a
# kill); restored BASE binary rerun -> GREEN. Exit 0 only if every seam
# completes GREEN->RED->RESTORED GREEN.
set -uo pipefail

WT=/home/jnhu/Source/Sluice-direct-read-compose-prevalidated-steps
RAW=/home/jnhu/Source/Sluice/research/direct-beyond-parity/p2-raw/P2C1
XMAKE=${XMAKE:-xmake}
BIN=$WT/build/linux/x86_64/release

cd "$WT" || { echo "mutants: cannot cd $WT" >&2; exit 1; }
mkdir -p "$RAW"

KILLER_FAULT=direct_composition_fault_test
KILLER_SHORTIO=semantic_short_io_reference_test

# seam|mutant-target|base-binary|named-killer-assertion
SEAMS=(
  "SLUICE_COMPOSE_MUTANT_ADVANCE_BY_REQUESTED|blocking_file_mut_advance_by_requested|$KILLER_FAULT|short_reads_report_eof_before_full_with_the_prefix"
  "SLUICE_COMPOSE_MUTANT_RETRY_ZERO_PROGRESS|blocking_file_mut_retry_zero_progress|$KILLER_FAULT|short_reads_report_eof_before_full_with_the_prefix"
  "SLUICE_COMPOSE_MUTANT_RETRY_ZERO_PROGRESS|blocking_file_mut_short_io_retry_zero_progress|$KILLER_SHORTIO|read_exact_accumulates_a_short_prefix_then_reports_eof"
  "SLUICE_COMPOSE_MUTANT_FAILURE_ACCOUNTED|blocking_file_mut_failure_accounted|$KILLER_FAULT|error_after_confirmed_prefix_keeps_the_prefix"
  "SLUICE_COMPOSE_MUTANT_SKIP_IMPOSSIBLE_COUNT|blocking_file_mut_skip_impossible_count|$KILLER_FAULT|impossible_count_stops_immediately"
  "SLUICE_DIRECT_MUTANT_STEP_REPORTS_FULL_COUNT|blocking_file_mut_step_reports_full_count|$KILLER_FAULT|short_reads_report_eof_before_full_with_the_prefix"
  "SLUICE_DIRECT_MUTANT_STEP_SKIPS_EINTR_RETRY|blocking_file_mut_step_skips_eintr_retry|$KILLER_FAULT|primitive_retries_eintr_without_a_completed_count"
)

run_base_green() {
    local base="$1" killer="$2"
    if ! $XMAKE build -y "$base" >>"$RAW/mutants-build.log" 2>&1; then
        echo "mutants: BASE build of $base FAILED" >&2
        return 1
    fi
    timeout 120 "$BIN/$base" >/dev/null 2>"$RAW/mutants-$base-base.err"
    local rc=$?
    if [[ $rc -ne 0 ]]; then
        echo "mutants: BASE run of $base not GREEN (exit $rc)" >&2
        return 1
    fi
    return 0
}

kill_seam() {
    local seam="$1" target="$2" base="$3" killer="$4"
    local err="$RAW/mutants-$target.err"

    if ! run_base_green "$base" "$killer"; then
        return 1
    fi

    if ! $XMAKE build -r -y "$target" >>"$RAW/mutants-build.log" 2>&1; then
        echo "mutants: $seam mutant build FAILED (build failure is not a kill)" >&2
        return 1
    fi
    timeout 120 "$BIN/$target" >/dev/null 2>"$err"
    local rc=$?
    if [[ $rc -eq 124 ]]; then
        echo "mutants: $seam mutant TIMED OUT (timeout is not a kill)" >&2
        return 1
    fi
    if [[ $rc -ne 1 ]]; then
        echo "mutants: $seam mutant exit $rc (crash/unexpected status is not a kill)" >&2
        return 1
    fi
    if ! grep -q "^FAIL: $killer\$" "$err"; then
        echo "mutants: $seam mutant did not fail on the named assertion '$killer'" >&2
        grep '^FAIL:' "$err" | head -3 >&2 || true
        return 1
    fi

    timeout 120 "$BIN/$base" >/dev/null 2>/dev/null
    if [[ $? -ne 0 ]]; then
        echo "mutants: $seam RESTORED base run not GREEN" >&2
        return 1
    fi
    echo "$seam via $target: GREEN -> RED(FAIL: $killer) -> RESTORED GREEN"
    return 0
}

if ! $XMAKE f -c -m release --liburing=n -y >"$RAW/mutants-config.log" 2>&1; then
    echo "mutants: release configure FAILED" >&2
    exit 1
fi

git rev-parse HEAD >"$RAW/mutants-head.txt" 2>&1 || true

overall=0
for spec in "${SEAMS[@]}"; do
    IFS='|' read -r seam target base killer <<<"$spec"
    if ! kill_seam "$seam" "$target" "$base" "$killer"; then
        overall=1
    fi
done

if [[ "$overall" == "0" ]]; then
    echo "mutants: all seams killed by named assertions"
    exit 0
fi
echo "mutants: FAILED" >&2
exit 1
