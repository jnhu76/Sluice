#!/bin/bash
# #595 R3 continuation: independent verifier re-run for the §14.3 flip receipt.
# Runs frozen-SHA correctness binaries (no rebuild), the 3 mutants, the
# isolated-builddir restored-green rebuild, and the frozen regression list,
# all under the shared E5 MEASUREMENT.lock. Read-only w.r.t. the worktree.
set -u
LOCK=/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock
ROOT=/home/jnhu/sluice-595-e5/r3-flip-20261105T1428Z
FROZEN=/home/jnhu/sluice-595-e5/r3/worktree/build/linux/x86_64/release
NEW=/tmp/r3c2-build/linux/x86_64/release
WT=/home/jnhu/sluice-595-e5/r3/worktree
OUT="$ROOT/evidence/correctness"
LOG="$OUT/session.log"

SCENARIOS="context_precheck context_cap_suffix inert_context capacity_boundary \
acceptance_barrier mixed_acceptance shutdown_accepted partial_modes poison_report \
control_stop gate_defer post_poison_single sq_partial_residue d2_positive d2_negative \
d1_dependency d3_budget sq_full_residue partial_submit cancel_ordering prepark_wake \
overflow poison shutdown borrow stale_generation"

REGRESSIONS="uring_ready_batch_research_test uring_ready_batch_fault_test \
uring_submit_boundary_test uring_progress_race_test uring_flush_count_oracle_test \
uring_external_loop_test uring_public_consumer_probe \
uring_request_core_integration_test shutdown_lifecycle_test"

exec 9>"$LOCK"
if ! flock -w 1800 9; then
    echo "BLOCKED_LOCK: could not acquire $LOCK within 1800s" | tee -a "$LOG"
    exit 42
fi
{
    echo "=== VERIFIER SESSION (independent, §14.3) ==="
    echo "acquired_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"

    echo "=== PRE-STATE ==="
    git -C "$WT" rev-parse HEAD
    git -C "$WT" status --porcelain | wc -l
    git -C "$WT" diff HEAD | sha256sum
    sha256sum "$FROZEN/uring_ready_batch_deterministic_test" \
              "$FROZEN/uring_ready_batch_det_mut_includes_not_ready" \
              "$FROZEN/uring_ready_batch_det_mut_residue_dropped" \
              "$FROZEN/uring_ready_batch_det_mut_redispatch_consumed" \
              "$FROZEN/uring_ready_batch_research_test" \
              "$FROZEN/uring_ready_batch_fault_test" \
              "$FROZEN/shutdown_lifecycle_test" \
              "$NEW/uring_ready_batch_deterministic_test" \
              "$NEW/bench_w03_external_loop" \
              "$NEW/bench_w02_pipeline"

    echo "=== BASE GREEN (frozen deterministic binary, aggregate) ==="
    "$FROZEN/uring_ready_batch_deterministic_test"
    echo "base_aggregate_rc=$?"

    echo "=== BASE GREEN (per-scenario, frozen binary) ==="
    fail=0
    for s in $SCENARIOS; do
        "$FROZEN/uring_ready_batch_deterministic_test" --only="$s" >/dev/null 2>&1
        rc=$?
        echo "scenario $s rc=$rc"
        [ $rc -ne 0 ] && fail=1
    done
    echo "all_scenarios_green=$((1-fail))"

    echo "=== MUTANT RED (frozen mutant binaries) ==="
    for m in includes_not_ready residue_dropped redispatch_consumed; do
        "$FROZEN/uring_ready_batch_det_mut_${m}" >/dev/null 2>&1
        echo "mut_${m} rc=$?"
    done

    echo "=== RESTORED GREEN (isolated-builddir rebuild of same source) ==="
    sha256sum "$NEW/uring_ready_batch_deterministic_test"
    "$NEW/uring_ready_batch_deterministic_test"
    echo "restored_green_rc=$?"

    echo "=== EXISTING REGRESSIONS (frozen binaries) ==="
    for t in $REGRESSIONS; do
        "$FROZEN/$t" >/dev/null 2>&1
        echo "$t rc=$?"
    done

    echo "=== POST-STATE ==="
    git -C "$WT" rev-parse HEAD
    git -C "$WT" status --porcelain | wc -l
    git -C "$WT" diff HEAD | sha256sum
    sha256sum "$FROZEN/uring_ready_batch_deterministic_test"
    echo "released_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$LOG" 2>&1
rc=$?
flock -u 9
echo "verification session rc=$rc (log: $LOG)"
tail -5 "$LOG"
