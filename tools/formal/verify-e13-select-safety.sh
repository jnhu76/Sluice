#!/usr/bin/env bash
# Reproducible PR #18 gate for the layered E13 Select formal safety suite.
# Builds on PR #17 (verify-e13-select-core.sh) by adding:
#   - the full named layered safety invariants (Contract/Central/Adapter)
#   - focused negative models (Contract NEG-C1..C9, Central NEG-S1..S7,
#     Event/Timer/Accounting NEG-E1..E6 + NEG-T1..T5 + NEG-A1..A4,
#     Multi-group NEG-MG1) plus the FAULT="None" restoration checks
#   - the per-law non-vacuity witness matrix (including the corrective-1
#     frozen-winner / frozen-snapshot / widened-accounting witnesses)
#   - the bounded two-group non-interference model + its three independent
#     reach witnesses (shared-Event, mixed Event+Timer, rollback-vs-complete)
#     plus two registration-split rollback reachability witnesses
#   - widened-domain refinement checks
#   - widened-accounting A1/A2 no-TypeOK double-check (counter pushed to 2
#     must trip the at-most-once law WITHOUT tripping EventTimerTypeOK)
#
# Source-safe: every TLC run happens in an isolated mktemp workspace with a
# defensive cleanup trap.  The script never deletes anything outside the
# mktemp root it created.
#
# Exit code: 0 if every gate passed its expectation, 1 otherwise.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
spec="$repo/docs/spec/e13_select"
jar="${TLA2TOOLS_JAR:-$repo/tla2tools.jar}"
workers="${TLC_WORKERS:-1}"

if [[ ! -f "$jar" ]]; then
  echo "error: tla2tools.jar not found at $jar" >&2
  echo "  set TLA2TOOLS_JAR=/path/to/tla2tools.jar" >&2
  exit 2
fi
if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2
  exit 2
fi

outroot="$(mktemp -d -t e13-select-safety.XXXXXX)"
workdir="$outroot/work"
cleanup() {
  if [[ -n "$outroot" ]] \
     && [[ "$outroot" == /tmp/e13-select-safety.* \
           || "$outroot" == "${TMPDIR:-/tmp}"/e13-select-safety.* ]]; then
    rm -rf -- "$outroot"
  fi
}
trap cleanup EXIT

mkdir -p "$workdir"
cp "$spec"/*.tla "$workdir/"
cp "$spec"/*.cfg "$workdir/"

cd "$workdir"

run_tlc() {
  local model="$1" cfg="$2" tag="$3"
  local metadir="$outroot/$tag.meta"
  mkdir -p "$metadir"
  java -XX:+UseParallelGC -cp "$jar" tlc2.TLC -nowarning \
       -workers "$workers" -metadir "$metadir" -config "$cfg" "$model" \
       >"$outroot/$tag.out" 2>&1 || true
}

launched() { grep -q '^Starting\.\.\.' "$1"; }
passed()   { grep -q 'Model checking completed. No error has been found' "$1"; }
deadlocked() { grep -qiE 'Deadlock reached|is deadlocked' "$1"; }
named_violation() { grep -Eq "Invariant $2 is violated" "$1"; }
property_violation() { grep -Eq "Temporal property .* is violated" "$1"; }
# PR #18 corrective-1: A1/A2 widened-accounting double-check helper.  The
# target at-most-once invariant must be violated AND the TLC output must NOT
# report an EventTimerTypeOK violation -- this confirms the focused fault
# isolates exactly the law it claims to break, not a typing precondition.
# The TypeOK domain was widened to 0..2 specifically so a fault can push a
# counter to 2 without tripping TypeOK.
no_typeok_violation() {
  ! grep -Eq "Invariant EventTimerTypeOK is violated|EventTimerTypeOK is.*[Vv]iolated" "$1"
}

metrics() {
  local file="$1"
  local states depth runtime
  states="$(grep 'states generated' "$file" | tail -1 | sed 's/^[[:space:]]*//' || true)"
  depth="$(grep 'The depth of the complete state graph search is' "$file" | tail -1 | sed 's/^[[:space:]]*//' || true)"
  runtime="$(grep '^Finished in' "$file" | tail -1 | sed 's/ at .*//' || true)"
  printf '%s; %s; %s' "$states" "$depth" "$runtime"
}

# expect_pass:   cfg must launch, not deadlock, and reach "completed, no error".
# expect_reach:  cfg must launch, not deadlock, NOT pass, and report the named
#                Invariant as violated (this is the witness that the premise
#                of an inverse-reachability / non-vacuity check is reachable).
# expect_negative: cfg must launch, not deadlock, NOT pass, and report the
#                named Invariant as violated (this is the focused fault
#                breaking exactly its target law).
# expect_restored: cfg must launch, not deadlock, and reach "completed, no
#                error" -- the FAULT="None" restoration check proving the
#                canonical model still passes once the fault is disabled.

expect_pass() {
  local label="$1" model="$2" cfg="$3" tag="$4"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out" || deadlocked "$out" || ! passed "$out"; then
    echo "FAIL  $label (expected PASS)"
    tail -25 "$out"
    return 1
  fi
  echo "PASS  $label  ($(metrics "$out"))"
}

expect_reach() {
  local label="$1" property="$2" cfg="$3" tag="$4"
  local model="${5:-E13Select}"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out" || deadlocked "$out" || passed "$out" \
     || ! named_violation "$out" "$property"; then
    echo "FAIL  $label (expected reach witness: $property violated)"
    tail -25 "$out"
    return 1
  fi
  echo "REACH $label  ($property violated; $(metrics "$out"))"
}

expect_negative() {
  local label="$1" property="$2" cfg="$3" tag="$4"
  local model="$5"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out" || deadlocked "$out" || passed "$out" \
     || ! named_violation "$out" "$property"; then
    echo "FAIL  $label (expected negative: $property violated by the fault)"
    tail -25 "$out"
    return 1
  fi
  echo "NEG   $label  ($property violated; $(metrics "$out"))"
}

expect_restored() {
  local label="$1" cfg="$2" tag="$3"
  local model="$4"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out" || deadlocked "$out" || ! passed "$out"; then
    echo "FAIL  $label (expected restored PASS with FAULT=\"None\")"
    tail -25 "$out"
    return 1
  fi
  echo "RESTORE $label  (FAULT=\"None\" PASS; $(metrics "$out"))"
}

# expect_negative_no_typeok: like expect_negative, but additionally requires
# that the TLC output contains NO EventTimerTypeOK violation.  Used for the
# widened-accounting NEG-A1/A2 gates so a counter pushed to 2 trips exactly
# the at-most-once law and not a typing precondition.
expect_negative_no_typeok() {
  local label="$1" property="$2" cfg="$3" tag="$4"
  local model="$5"
  local out="$outroot/$tag.out"
  run_tlc "$model" "$cfg" "$tag"
  if ! launched "$out" || deadlocked "$out" || passed "$out" \
     || ! named_violation "$out" "$property"; then
    echo "FAIL  $label (expected negative: $property violated by the fault)"
    tail -25 "$out"
    return 1
  fi
  if ! no_typeok_violation "$out"; then
    echo "FAIL  $label (target violated but EventTimerTypeOK ALSO violated -- fault does not isolate the law)"
    tail -25 "$out"
    return 1
  fi
  echo "NEG   $label  ($property violated, TypeOK intact; $(metrics "$out"))"
}

rc=0
echo "=== E13 Select layered formal SAFETY (PR #18; workers=$workers) ==="
echo

echo "--- Layered safety aggregates (P) ---"
expect_pass "Contract safety H1-H5+I (2-arm)" \
  E13SelectContract E13SelectContract.safety.cfg p_contract_safety || rc=1
expect_pass "Contract safety H1-H5+I (3-arm)" \
  E13SelectContract E13SelectContract.safety3.cfg p_contract_safety3 || rc=1
expect_pass "Central safety J (2-arm)" \
  E13SelectCentralClaim E13SelectCentralClaim.safety.cfg p_central_safety || rc=1
expect_pass "Central safety J (3-arm admission tie)" \
  E13SelectCentralClaim E13SelectCentralClaim.safety3sim.cfg p_central_safety3sim || rc=1
expect_pass "Central safety J (4-arm tie-break)" \
  E13SelectCentralClaim E13SelectCentralClaim.safety4.cfg p_central_safety4 || rc=1
expect_pass "Adapter safety K+L+M+N (2-arm)" \
  E13SelectEventTimer E13SelectEventTimer.safety.cfg p_adapter_safety || rc=1
expect_pass "Adapter safety K+L+M+N (3-arm mix)" \
  E13SelectEventTimer E13SelectEventTimer.safety3mix.cfg p_adapter_safety3mix || rc=1
echo

echo "--- Multi-group bounded non-interference (O) ---"
expect_pass "Two-group MGSafetyInv (shared Event)" \
  E13SelectMultiGroup E13SelectMultiGroup.cfg p_mg_safety || rc=1
# PR #18 corrective-1: split the single combined reach witness into three
# independent cfgs so each arm of the multi-group non-interference property
# is exercised in isolation (shared-Event, mixed Event+Timer, rollback-vs-
# complete).  Plus two non-vacuity witnesses for the new registration-split
# rollback reachability (arm installed but not finished; pre-finish rollback).
expect_reach "Two-group shared-Event double completion" \
  NotMG_ReachSharedEventBothGroupsComplete \
  E13SelectMultiGroup.reach_shared_event.cfg \
  r_mg_shared_event E13SelectMultiGroup || rc=1
expect_reach "Two-group mixed Event+Timer completion" \
  NotMG_ReachMixedEventTimer \
  E13SelectMultiGroup.reach_mixed_event_timer.cfg \
  r_mg_mixed_event_timer E13SelectMultiGroup || rc=1
expect_reach "Two-group one rollback other complete" \
  NotMG_ReachOneRollbackOtherComplete \
  E13SelectMultiGroup.reach_rollback_vs_complete.cfg \
  r_mg_rollback_vs_complete E13SelectMultiGroup || rc=1
expect_reach "Multi-group arm installed not finished" \
  NotMG_ReachArmInstalledNotFinished \
  E13SelectMultiGroup.nv_installed_not_finished.cfg \
  nv_mg_installed_not_finished E13SelectMultiGroup || rc=1
expect_reach "Multi-group pre-finish rollback" \
  NotMG_ReachPreFinishRollback \
  E13SelectMultiGroup.nv_pre_finish_rollback.cfg \
  nv_mg_pre_finish_rollback E13SelectMultiGroup || rc=1
echo

echo "--- Widened-domain refinement (X) ---"
expect_pass "Central -> Contract refinement (3-arm admission tie)" \
  E13SelectCentralClaim E13SelectCentralClaim.refine3.cfg x_central_refine3 || rc=1
# The 2-arm adapter -> Central refinement is already covered by PR #17
# (verify-e13-select-core.sh, E13SelectEventTimer.cfg).  Wider adapter
# refinement PROPERTY checks (3-mix, 4-mix) blow up past the 5-minute TLC
# budget; the 3-arm adapter domain is instead exercised by AdapterSafetyInv
# in E13SelectEventTimer.safety3mix.cfg.
echo

echo "--- Contract negative models (R: NEG-C1..C9) ---"
expect_negative "NEG-C1 commit before linearization" \
  C_InvCommitRequiresWinnerLinearization E13SelectContractNeg.C1.cfg \
  neg_c1 E13SelectContractNeg || rc=1
expect_negative "NEG-C2 second committed winner" \
  C_InvAtMostOneCommittedWinner E13SelectContractNeg.C2.cfg \
  neg_c2 E13SelectContractNeg || rc=1
expect_negative "NEG-C3 loser publishes result" \
  C_InvLoserNeverPublishesResult E13SelectContractNeg.C3.cfg \
  neg_c3 E13SelectContractNeg || rc=1
expect_negative "NEG-C4 complete with open authority" \
  C_InvCompletionRequiresAllAuthorityClosed E13SelectContractNeg.C4.cfg \
  neg_c4 E13SelectContractNeg || rc=1
expect_negative "NEG-C5 loser reservation not released" \
  C_InvLoserReservationReleased E13SelectContractNeg.C5.cfg \
  neg_c5 E13SelectContractNeg || rc=1
expect_negative "NEG-C6 rollback requires Running caller" \
  C_InvRegistrationRollbackRequiresRunningCaller E13SelectContractNeg.C6.cfg \
  neg_c6 E13SelectContractNeg || rc=1
expect_negative "NEG-C7 Aborted caller Waiting" \
  C_InvAbortedCallerNeverWaiting E13SelectContractNeg.C7.cfg \
  neg_c7 E13SelectContractNeg || rc=1
expect_negative "NEG-C8 Destroy without Consumed or valid Abort" \
  C_InvDestroyRequiresConsumedOrValidAbort E13SelectContractNeg.C8.cfg \
  neg_c8 E13SelectContractNeg || rc=1
# PR #18 corrective-1: NEG-C9 isolates the frozen-winner identity stability
# law.  A legal linearization has stamped linearized_winner = A; the fault
# flips the live winner to a different registered arm B (no WinnerCommitted)
# and leaves the frozen history unchanged.
expect_negative "NEG-C9 winner identity flip after linearization" \
  C_InvWinnerIdentityStableAfterLinearization E13SelectContractNeg.C9.cfg \
  neg_c9 E13SelectContractNeg || rc=1
expect_restored "Contract restore (FAULT=None)" \
  E13SelectContractNeg.restore.cfg restore_contract E13SelectContractNeg || rc=1
echo

echo "--- Central Claim negative models (S: NEG-S1..S7) ---"
expect_negative "NEG-S1 claim a non-offered arm" \
  S_InvClaimRequiresOfferedArm E13SelectCentralClaimNeg.S1.cfg \
  neg_s1 E13SelectCentralClaimNeg || rc=1
expect_negative "NEG-S2 snapshot mutated after claim" \
  S_InvClaimSnapshotImmutableAfterClaim E13SelectCentralClaimNeg.S2.cfg \
  neg_s2 E13SelectCentralClaimNeg || rc=1
expect_negative "NEG-S3 second successful claim" \
  S_InvAtMostOneSuccessfulClaim E13SelectCentralClaimNeg.S3.cfg \
  neg_s3 E13SelectCentralClaimNeg || rc=1
expect_negative "NEG-S4 commit without winner class" \
  S_InvClaimBeforeAdapterCommit E13SelectCentralClaimNeg.S4.cfg \
  neg_s4 E13SelectCentralClaimNeg || rc=1
expect_negative "NEG-S5 admission tie not lowest-index" \
  S_InvAdmissionTieUsesLowestIndex E13SelectCentralClaimNeg.S5.cfg \
  neg_s5 E13SelectCentralClaimNeg || rc=1
expect_negative "NEG-S6 snapshot does not contain winner" \
  S_InvClaimSnapshotContainsWinner E13SelectCentralClaimNeg.S6.cfg \
  neg_s6 E13SelectCentralClaimNeg || rc=1
# PR #18 corrective-1: NEG-S7 isolates the strict frozen-snapshot
# immutability law in its hardest case (addition, not removal).  Requires
# a 3-arm cfg so a winner + snapshot-internal candidate + snapshot-external
# registered arm can coexist.
expect_negative "NEG-S7 snapshot adds member after claim (3-arm)" \
  S_InvClaimSnapshotImmutableAfterClaim E13SelectCentralClaimNeg.S7.cfg \
  neg_s7 E13SelectCentralClaimNeg || rc=1
expect_restored "Central restore (FAULT=None)" \
  E13SelectCentralClaimNeg.restore.cfg restore_central E13SelectCentralClaimNeg || rc=1
echo

echo "--- Event/Timer/Accounting negative models (E/T/A: NEG-E1..E6, T1..T5, A1..A4) ---"
expect_negative "NEG-E1 candidate bypasses scan path" \
  E_InvNoReadinessBypassesPermittedEventPath E13SelectEventTimerNeg.E1.cfg \
  neg_e1 E13SelectEventTimerNeg || rc=1
expect_negative "NEG-E2 scan adds arm of other Event" \
  E_InvBroadcastScansOnlyTargetEventArms E13SelectEventTimerNeg.E2.cfg \
  neg_e2 E13SelectEventTimerNeg || rc=1
expect_negative "NEG-E3 Scanning forges + commits winner" \
  E_InvPhaseOneDoesNotCommit E13SelectEventTimerNeg.E3.cfg \
  neg_e3 E13SelectEventTimerNeg || rc=1
expect_negative "NEG-E4 Select consumes Event Set" \
  E_InvEventPersistentSetNotConsumed E13SelectEventTimerNeg.E4.cfg \
  neg_e4 E13SelectEventTimerNeg || rc=1
expect_negative "NEG-E5 Scanning publishes result" \
  E_InvPhaseOneDoesNotPublish E13SelectEventTimerNeg.E5.cfg \
  neg_e5 E13SelectEventTimerNeg || rc=1
expect_negative "NEG-E6 Timer publishes via Event broadcast" \
  E_InvOnlyWinnerPublishes E13SelectEventTimerNeg.E6.cfg \
  neg_e6 E13SelectEventTimerNeg || rc=1
expect_negative "NEG-T1 Retired registration derefs" \
  T_InvRetiredRegistrationNeverDereferences E13SelectEventTimerNeg.T1.cfg \
  neg_t1 E13SelectEventTimerNeg || rc=1
expect_negative "NEG-T2 Consumed without winner" \
  T_InvConsumeRequiresWinner E13SelectEventTimerNeg.T2.cfg \
  neg_t2 E13SelectEventTimerNeg || rc=1
expect_negative "NEG-T3 Retire non-loser" \
  T_InvRetireRequiresLoserOrRollback E13SelectEventTimerNeg.T3.cfg \
  neg_t3 E13SelectEventTimerNeg || rc=1
expect_negative "NEG-T4 pump authority Retired" \
  T_InvActiveStableDuringPumpAuthority E13SelectEventTimerNeg.T4.cfg \
  neg_t4 E13SelectEventTimerNeg || rc=1
expect_negative "NEG-T5 stale-pump publishes" \
  T_InvStalePumpNeverPublishes E13SelectEventTimerNeg.T5.cfg \
  neg_t5 E13SelectEventTimerNeg || rc=1
expect_negative "NEG-A1 waiting closes twice" \
  A_InvWaitAccountClosesAtMostOnce E13SelectEventTimerNeg.A1.cfg \
  neg_a1 E13SelectEventTimerNeg || rc=1
expect_negative "NEG-A2 timer closes twice" \
  A_InvTimerAccountClosesAtMostOnce E13SelectEventTimerNeg.A2.cfg \
  neg_a2 E13SelectEventTimerNeg || rc=1
# PR #18 corrective-1 double-check: A1/A2 must isolate the at-most-once law
# without tripping EventTimerTypeOK, since the counter domain was widened to
# 0..2 precisely so a focused fault can push a counter to 2 without breaking
# the typing precondition.  Repeat A1/A2 with the no-TypeOK double-check.
expect_negative_no_typeok "NEG-A1 widowed (no TypeOK violation)" \
  A_InvWaitAccountClosesAtMostOnce E13SelectEventTimerNeg.A1.cfg \
  neg_a1_no_typeok E13SelectEventTimerNeg || rc=1
expect_negative_no_typeok "NEG-A2 widowed (no TypeOK violation)" \
  A_InvTimerAccountClosesAtMostOnce E13SelectEventTimerNeg.A2.cfg \
  neg_a2_no_typeok E13SelectEventTimerNeg || rc=1
expect_negative "NEG-A3 Completed with open accounting" \
  A_InvCompletedHasNoOpenAccounting E13SelectEventTimerNeg.A3.cfg \
  neg_a3 E13SelectEventTimerNeg || rc=1
expect_negative "NEG-A4 accounting underflow" \
  A_InvNoAccountingUnderflow E13SelectEventTimerNeg.A4.cfg \
  neg_a4 E13SelectEventTimerNeg || rc=1
expect_restored "Adapter restore (FAULT=None)" \
  E13SelectEventTimerNeg.restore.cfg restore_adapter E13SelectEventTimerNeg || rc=1
echo

echo "--- Multi-group negative models (MG: NEG-MG1) ---"
# PR #18 corrective-1: NEG-MG1 isolates the transition-level cross-group
# non-interference that the per-action UNCHANGED audit establishes
# structurally.  Group g0's RollbackArm spuriously mutates group g1's
# authority_open while group g1 is at a terminal phase.
expect_negative "NEG-MG1 cross-group authority mutation during rollback" \
  MG_InvAuthorityClosureDoesNotCrossGroups E13SelectMultiGroupNeg.MG1.cfg \
  neg_mg1 E13SelectMultiGroupNeg || rc=1
expect_restored "Multi-group restore (FAULT=None)" \
  E13SelectMultiGroupNeg.restore.cfg restore_mg E13SelectMultiGroupNeg || rc=1
echo

echo "--- Per-law non-vacuity witnesses (W) ---"
expect_reach "Contract committed winner exists" \
  NotReachContractCommittedWinner E13SelectContract.nv_commitwinner.cfg \
  w_contract_commitwinner E13SelectContract || rc=1
expect_reach "Contract classified loser exists" \
  NotReachContractLoserExists E13SelectContract.nv_loser.cfg \
  w_contract_loser E13SelectContract || rc=1
expect_reach "Contract arm published" \
  NotReachContractArmPublished E13SelectContract.nv_armpub.cfg \
  w_contract_armpub E13SelectContract || rc=1
expect_reach "Contract Completed phase" \
  NotReachContractCompleted E13SelectContract.nv_completed.cfg \
  w_contract_completed E13SelectContract || rc=1
expect_reach "Contract Destroyed phase" \
  NotReachContractDestroyed E13SelectContract.nv_destroyed.cfg \
  w_contract_destroyed E13SelectContract || rc=1
expect_reach "Contract Aborted phase" \
  NotReachContractAborted E13SelectContract.nv_aborted.cfg \
  w_contract_aborted E13SelectContract || rc=1
expect_reach "Contract Rollback phase" \
  NotReachContractRollback E13SelectContract.nv_rollback.cfg \
  w_contract_rollback E13SelectContract || rc=1
# PR #18 corrective-1 (P1-4) non-vacuity witness for the frozen-winner
# identity stability law: linearized but not yet committed.
expect_reach "Contract winner linearized not committed" \
  NotReachContractWinnerLinearizedNotCommitted \
  E13SelectContract.nv_lin_not_committed.cfg \
  w_contract_lin_not_committed E13SelectContract || rc=1
expect_reach "Central claimed offer" \
  NotReachCentralClaimed E13SelectCentralClaim.central_claimed.cfg \
  w_central_claimed E13SelectCentralClaim || rc=1
expect_reach "Central claim candidates latched" \
  NotReachCentralClaimCandidatesLatched E13SelectCentralClaim.central_candidates.cfg \
  w_central_candidates E13SelectCentralClaim || rc=1
expect_reach "Central admission tie" \
  NotReachCentralAdmissionTie E13SelectCentralClaim.central_admission_tie.cfg \
  w_central_admission_tie E13SelectCentralClaim || rc=1
expect_reach "Central Winner+Loser classified" \
  NotReachCentralWinnerClassified E13SelectCentralClaim.central_winner_classified.cfg \
  w_central_winner_classified E13SelectCentralClaim || rc=1
# PR #18 corrective-1 (P1-3) non-vacuity witnesses for the frozen claim
# snapshot laws: stamp happened, and the multi-candidate non-trivial case.
expect_reach "Central frozen snapshot valid" \
  NotReachCentralFrozenSnapshotValid \
  E13SelectCentralClaim.nv_frozen_snapshot.cfg \
  w_central_frozen_snapshot E13SelectCentralClaim || rc=1
expect_reach "Central multi-candidate frozen snapshot" \
  NotReachCentralMultiCandidateSnapshot \
  E13SelectCentralClaim.nv_multi_candidate_snapshot.cfg \
  w_central_multi_candidate_snapshot E13SelectCentralClaim || rc=1
expect_reach "Adapter wait account closed" \
  NotReachAdapterWaitAccountClosed E13SelectEventTimer.adapter_wait_closed.cfg \
  w_adapter_wait_closed E13SelectEventTimer || rc=1
expect_reach "Adapter timer account closed" \
  NotReachAdapterTimerAccountClosed E13SelectEventTimer.adapter_timer_closed.cfg \
  w_adapter_timer_closed E13SelectEventTimer || rc=1
expect_reach "Adapter Retired timer exists" \
  NotReachAdapterRetiredTimerExists E13SelectEventTimer.adapter_retired.cfg \
  w_adapter_retired E13SelectEventTimer || rc=1
expect_reach "Adapter Consumed timer exists" \
  NotReachAdapterConsumedTimerExists E13SelectEventTimer.adapter_consumed.cfg \
  w_adapter_consumed E13SelectEventTimer || rc=1
expect_reach "Adapter commit_step recorded" \
  NotReachAdapterCommitStepRecorded E13SelectEventTimer.adapter_commit_step.cfg \
  w_adapter_commit_step E13SelectEventTimer || rc=1
expect_reach "Adapter publication_step recorded" \
  NotReachAdapterPublicationStepRecorded E13SelectEventTimer.adapter_publication_step.cfg \
  w_adapter_publication_step E13SelectEventTimer || rc=1
# PR #18 corrective-1 non-vacuity witness for the widened accounting at-most-
# once laws: at least one arm has closed its account (counter == 1, the value
# the legal transition produces).  After TypeOK widening to 0..2, this
# confirms the laws bite on the genuinely reachable counter value.
expect_reach "Adapter account close count == 1 (widened)" \
  NotReachAdapterAccountCountOne \
  E13SelectEventTimer.adapter_account_count_one.cfg \
  w_adapter_account_count_one E13SelectEventTimer || rc=1
echo

echo "--- PR #17 regression: causal reachability R1-R12 ---"
expect_reach "R1 Event admission-ready inline" NotReach_R1 \
  E13Select.scene_inline.cfg r1 || rc=1
expect_reach "R5 Event winner + Timer loser" NotReach_R5 \
  E13Select.scene_2mix.cfg r5 || rc=1
expect_reach "R7 claim snapshot tie-break" NotReach_R7 \
  E13Select.scene_4mix.cfg r7 || rc=1
expect_reach "R9 partial-registration rollback" NotReach_R9 \
  E13Select.scene_rollback.cfg r9 || rc=1
expect_reach "R10 stale TimerRegistration skip" NotReach_R10 \
  E13Select.scene_staletimer.cfg r10 || rc=1
echo

echo
grep -m1 '^TLC2 Version' "$outroot/p_contract_safety.out" || true
if [[ "$rc" -eq 0 ]]; then
  echo "=== PASS: E13 Select layered formal SAFETY suite (PR #18) ==="
else
  echo "=== FAIL: one or more E13 formal SAFETY gates failed ==="
fi
exit "$rc"
