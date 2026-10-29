#!/usr/bin/env bash
# P5 A+B mutation-kill gate over the union seam set (P5_INTERACTION_PLAN.md
# §1 seam-union measurement, §2 R2 target union, §4 S0). Seams: the four
# compose seams carried by both factors, P2C1's two step seams, P2C2's
# reads-suite and rules-suite legs. For each row: BASE build -> killer suite
# GREEN; mutant target build -> the binary fails with the exact named
# assertion (exit 1 + "FAIL: <killer>"; crash/signal/timeout is not a kill);
# restored BASE binary rerun -> GREEN. The detail rule suite must stay GREEN
# under a compose mutant define. Exit 0 only if every row and the control
# pass. Records the tree head and timestamps every archive.
set -uo pipefail

WT=/home/jnhu/Source/Sluice-direct-compose-stack-readpreval-writeexit
RAW=/home/jnhu/Source/Sluice/research/direct-beyond-parity/p5-raw/P5AB
XMAKE=${XMAKE:-xmake}
BIN=$WT/build/linux/x86_64/release

cd "$WT" || { echo "mutants: cannot cd $WT" >&2; exit 1; }
mkdir -p "$RAW"

STAMP=$(date +%Y%m%dT%H%M%S)
RUNLOG="$RAW/mutants-$STAMP"
mkdir -p "$RUNLOG"
git rev-parse HEAD >"$RUNLOG/mutants-head.txt" 2>&1 || true

KILLER_FAULT=direct_composition_fault_test
KILLER_READS=direct_composition_test
KILLER_SHORTIO=semantic_short_io_reference_test

# seam|mutant-target|base-suite|named-killer-assertion
SEAMS=(
  "SLUICE_COMPOSE_MUTANT_ADVANCE_BY_REQUESTED|blocking_file_mut_advance_by_requested|$KILLER_FAULT|short_reads_report_eof_before_full_with_the_prefix"
  "SLUICE_COMPOSE_MUTANT_ADVANCE_BY_REQUESTED|compose_mut_advance_by_requested_reads|$KILLER_READS|read_exact_reports_eof_before_full_with_the_prefix"
  "SLUICE_COMPOSE_MUTANT_RETRY_ZERO_PROGRESS|blocking_file_mut_retry_zero_progress|$KILLER_FAULT|short_reads_report_eof_before_full_with_the_prefix"
  "SLUICE_COMPOSE_MUTANT_RETRY_ZERO_PROGRESS|blocking_file_mut_short_io_retry_zero_progress|$KILLER_SHORTIO|read_exact_accumulates_a_short_prefix_then_reports_eof"
  "SLUICE_COMPOSE_MUTANT_FAILURE_ACCOUNTED|blocking_file_mut_failure_accounted|$KILLER_FAULT|error_after_confirmed_prefix_keeps_the_prefix"
  "SLUICE_COMPOSE_MUTANT_SKIP_IMPOSSIBLE_COUNT|blocking_file_mut_skip_impossible_count|$KILLER_FAULT|impossible_count_stops_immediately"
  "SLUICE_DIRECT_MUTANT_STEP_REPORTS_FULL_COUNT|blocking_file_mut_step_reports_full_count|$KILLER_FAULT|short_reads_report_eof_before_full_with_the_prefix"
  "SLUICE_DIRECT_MUTANT_STEP_SKIPS_EINTR_RETRY|blocking_file_mut_step_skips_eintr_retry|$KILLER_FAULT|primitive_retries_eintr_without_a_completed_count"
)

run_base_green() {
    local base="$1"
    if ! $XMAKE build -y "$base" >>"$RUNLOG/mutants-build.log" 2>&1; then
        echo "mutants: BASE build of $base FAILED" >&2
        return 1
    fi
    timeout 120 "$BIN/$base" >/dev/null 2>"$RUNLOG/mutants-$base-base.err"
    local rc=$?
    if [[ $rc -ne 0 ]]; then
        echo "mutants: BASE run of $base not GREEN (exit $rc)" >&2
        return 1
    fi
    return 0
}

kill_seam() {
    local seam="$1" target="$2" base="$3" killer="$4"
    local err="$RUNLOG/mutants-$target.err"

    if ! run_base_green "$base"; then
        return 1
    fi
    echo "  BASE GREEN ($base)"

    if ! $XMAKE build -r -y "$target" >>"$RUNLOG/mutants-build.log" 2>&1; then
        echo "mutants: $seam mutant build FAILED (build failure is not a kill)" >&2
        return 1
    fi
    timeout 120 "$BIN/$target" >/dev/null 2>"$err"
    local rc=$?
    if [[ $rc -eq 124 ]]; then
        echo "mutants: $seam/$target mutant TIMED OUT (timeout is not a kill)" >&2
        return 1
    fi
    if [[ $rc -ne 1 ]]; then
        echo "mutants: $seam/$target mutant exit $rc (crash/unexpected status is not a kill)" >&2
        return 1
    fi
    if ! grep -q "^FAIL: $killer\$" "$err"; then
        echo "mutants: $seam/$target mutant did not fail on the named assertion '$killer'" >&2
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

rules_green_under_mutant() {
    if ! $XMAKE build -y compose_mut_rules_stay_green_short_io \
        >>"$RUNLOG/mutants-build.log" 2>&1; then
        echo "mutants: rules-control build FAILED" >&2
        return 1
    fi
    timeout 120 "$BIN/compose_mut_rules_stay_green_short_io" \
        >/dev/null 2>"$RUNLOG/mutants-rules-control.err"
}

if ! $XMAKE f -c -m release --liburing=n -y >"$RUNLOG/mutants-config.log" 2>&1; then
    echo "mutants: release configure FAILED" >&2
    exit 1
fi

overall=0
for spec in "${SEAMS[@]}"; do
    IFS='|' read -r seam target base killer <<<"$spec"
    echo "=== seam $seam / $target ==="
    if ! kill_seam "$seam" "$target" "$base" "$killer"; then
        overall=1
    fi
done

if [[ "$overall" == "0" ]]; then
    if ! rules_green_under_mutant; then
        echo "mutants: detail rule suite not GREEN under a mutant define" >&2
        overall=1
    else
        echo "detail rule suite GREEN under mutant define (compose_mut_rules_stay_green_short_io)"
    fi
fi

if [[ "$overall" == "0" ]]; then
    echo "mutants: all seams killed by named assertions (record: $RUNLOG)"
    exit 0
fi
echo "mutants: FAILED" >&2
exit 1
