#!/usr/bin/env bash
# FORMAL-CAPABILITY-BOUNDARY-1 verification gate.
#
# Runs the complete formal verification for the frozen Lean artifact set:
#   1. lake build must succeed,
#   2. no sorry/admit may appear in any proof file,
#   3. every exported theorem must depend only on the standard logic
#      principles (propext, Classical.choice, Quot.sound).
#
# Usage: scripts/verify_formal.sh   (from the repository root)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
formal="$root/formal"

if [[ ! -d "$formal" ]]; then
    echo "formal/ missing: nothing to verify" >&2
    exit 1
fi

export PATH="$HOME/.elan/bin:$PATH"

echo "== lake build =="
cd "$formal"
lake build

echo "== sorry/admit scan =="
# `admit` is also the name of the PrimLTS2 entry-critical-section field in the
# V2 calculus, so the scan excludes the field occurrences (`admit :`, `admit :=`,
# `.admit`, `` `admit` ``) and still catches genuine sorry/admit tactic usage.
if grep -rn -E '\bsorry\b|\badmit\b' Sluice/ | grep -vE 'admit *(:=|:)|\.admit\b|`admit`'; then
    echo "FAIL: sorry/admit found in proof files" >&2
    exit 1
fi
echo "clean"

echo "== axiom audit =="
cat > .lake/axiom-audit.lean <<'EOF'
import Sluice.Formal.VacuityV2
import Sluice.Formal.EventV2
import Sluice.Formal.SemV2
import Sluice.Formal.MutexV2
import Sluice.Formal.ConditionV2
import Sluice.Formal.RwLockV2
import Sluice.Formal.QueueV2
import Sluice.Formal.SelectV2
import Sluice.Formal.DriverV2
open Sluice.Formal
#print axioms Sluice.Formal.tracesEnc_shadow_false
#print axioms Sluice.Formal.probeEnc_possesses
#print axioms Sluice.Formal.seqOK_not_shadow
#print axioms Sluice.Formal.echoRed
#print axioms Sluice.Formal.echo_reducibleTo
#print axioms Sluice.Formal.echo_reducible
#print axioms Sluice.Formal.echo_not_irreducible
#print axioms Sluice.Formal.not_reducible_of_separated
#print axioms Sluice.Formal.obligations_preserved_of_reduction
#print axioms Sluice.Formal.domPrim_possesses
#print axioms Sluice.Formal.domEnc_possesses
#print axioms Sluice.Formal.encLie_overProduces
#print axioms Sluice.Formal.extIssue_inv
#print axioms Sluice.Formal.eventPrim_guarantees
#print axioms Sluice.Formal.semPrimOf_guarantees
#print axioms Sluice.Formal.semBalance_mirror
#print axioms Sluice.Formal.semRuns_split
#print axioms Sluice.Formal.semStep_take_credit
#print axioms Sluice.Formal.semPrim_possesses_window
#print axioms Sluice.Formal.semPrim_possesses_fiberWindow
#print axioms Sluice.Formal.semPrim_possesses_handoff
#print axioms Sluice.Formal.semPrim_possesses_extRel
#print axioms Sluice.Formal.semPrimOf_possesses_initialTake
#print axioms Sluice.Formal.semPrimOf_possesses_ceiling
#print axioms Sluice.Formal.handoff_tight
#print axioms Sluice.Formal.initialTake_tight
#print axioms Sluice.Formal.ceiling_tight
#print axioms Sluice.Formal.seqOK_window
#print axioms Sluice.Formal.seqOK_fiberWindow
#print axioms Sluice.Formal.seqOK_extRel
#print axioms Sluice.Formal.seqOK_handoff
#print axioms Sluice.Formal.seqOK_initialTake
#print axioms Sluice.Formal.seqOK_ceiling
#print axioms Sluice.Formal.semMutant_not_guarantees
#print axioms Sluice.Formal.eventPrim_possesses_drain
#print axioms Sluice.Formal.eventPrim_possesses_ext
#print axioms Sluice.Formal.encChained_underProduces
#print axioms Sluice.Formal.enc_no_ext_cap_under
#print axioms Sluice.Formal.event_capability_defer
#print axioms Sluice.Formal.eventMutant_not_guarantees
#print axioms Sluice.Formal.eventStep_preserved
#print axioms Sluice.Formal.mutex_mirror
#print axioms Sluice.Formal.mutexRuns_split
#print axioms Sluice.Formal.mutexStep_grant_credit
#print axioms Sluice.Formal.mutexPrim_guarantees
#print axioms Sluice.Formal.mutex_silent_prefix
#print axioms Sluice.Formal.mutex_not_unlockIssue
#print axioms Sluice.Formal.mutex_over_produces
#print axioms Sluice.Formal.mutex_irreducible
#print axioms Sluice.Formal.mutex_possesses_grant
#print axioms Sluice.Formal.mutex_possesses_cancel
#print axioms Sluice.Formal.mutexMutant_not_guarantees
#print axioms Sluice.Formal.encoding_class_inhabited
#print axioms Sluice.Formal.cond_encoding_class_inhabited
#print axioms Sluice.Formal.rw_encoding_class_inhabited
#print axioms Sluice.Formal.rw_irreducible
#print axioms Sluice.Formal.rw_over_produces
#print axioms Sluice.Formal.rw_not_wunlockIssue
#print axioms Sluice.Formal.seqOK_wunlockIssue
#print axioms Sluice.Formal.rw_silent_prefix
#print axioms Sluice.Formal.rw_exclusion
#print axioms Sluice.Formal.rw_exclusion_holds
#print axioms Sluice.Formal.rwGrantHead_preserves
#print axioms Sluice.Formal.rwRun_preserves
#print axioms Sluice.Formal.rwPark_preserves
#print axioms Sluice.Formal.rwFinish_preserves
#print axioms Sluice.Formal.rwExtRun_preserves
#print axioms Sluice.Formal.rwFinish_consumed
#print axioms Sluice.Formal.rwFinishW_consumed
#print axioms Sluice.Formal.rwConsume_mem
#print axioms Sluice.Formal.rwConsume_len
#print axioms Sluice.Formal.rwGrantHead_claims_head_writer
#print axioms Sluice.Formal.rwGrantHead_batch_drains
#print axioms Sluice.Formal.rwGrantHead_batch_stops_at_writer
#print axioms Sluice.Formal.rwGrantHead_writer_blocked_by_readers
#print axioms Sluice.Formal.rwGrantHead_blocked_by_writer
#print axioms Sluice.Formal.rwUnlockRead_pays
#print axioms Sluice.Formal.rwUnlockRead_zero_gate
#print axioms Sluice.Formal.rwAdmit_unchanged
#print axioms Sluice.Formal.rwAdmit_wunlock_owner_gated
#print axioms Sluice.Formal.rw_battery_inline_read
#print axioms Sluice.Formal.rw_battery_writer_handoff
#print axioms Sluice.Formal.rw_battery_batch
#print axioms Sluice.Formal.rw_battery_cancel
#print axioms Sluice.Formal.rwM1_exclusion_break
#print axioms Sluice.Formal.rwM2_unpaid
#print axioms Sluice.Formal.rwM2_handoff_refuted
#print axioms Sluice.Formal.rwM3_undergrants
#print axioms Sluice.Formal.rwM4_nonowner_admitted
#print axioms Sluice.Formal.cond_silent_prefix
#print axioms Sluice.Formal.cond_not_waitIssue
#print axioms Sluice.Formal.cond_over_produces
#print axioms Sluice.Formal.cond_irreducible
#print axioms Sluice.Formal.cond_mirror
#print axioms Sluice.Formal.cond_battery_fast_path
#print axioms Sluice.Formal.cond_battery_cancel
#print axioms Sluice.Formal.cond_battery_broadcast
#print axioms Sluice.Formal.condRun_notifyAll_drains
#print axioms Sluice.Formal.condExtRun_notifyAll_drains
#print axioms Sluice.Formal.condWakeBacked
#print axioms Sluice.Formal.condConsume_mem
#print axioms Sluice.Formal.condFinish_consumed
#print axioms Sluice.Formal.condFinish_takes_owner
#print axioms Sluice.Formal.condPark_frees_slot
#print axioms Sluice.Formal.condM1_spurious
#print axioms Sluice.Formal.condM2_drains_refuted
#print axioms Sluice.Formal.condM2_overclaims
#print axioms Sluice.Formal.condM3_reacquire_skipped
#print axioms Sluice.Formal.condM4_keeps_slot
#print axioms Sluice.Formal.condM4_lost_wakeup
#print axioms Sluice.Formal.qDrainC_drop
#print axioms Sluice.Formal.qLen_commit
#print axioms Sluice.Formal.qLen_tail
#print axioms Sluice.Formal.qLedger_push
#print axioms Sluice.Formal.qLedger_pop
#print axioms Sluice.Formal.qLedger_pop_grant
#print axioms Sluice.Formal.qRun_result_agrees
#print axioms Sluice.Formal.qExtRun_result_agrees
#print axioms Sluice.Formal.qFinish_result_agrees
#print axioms Sluice.Formal.qAdmit_unchanged
#print axioms Sluice.Formal.qGrantConsumer_preserves
#print axioms Sluice.Formal.qGrantProducer_preserves
#print axioms Sluice.Formal.qRun_preserves
#print axioms Sluice.Formal.qPark_preserves
#print axioms Sluice.Formal.qFinish_preserves
#print axioms Sluice.Formal.qExtRun_preserves
#print axioms Sluice.Formal.q_safe
#print axioms Sluice.Formal.q_safe_holds
#print axioms Sluice.Formal.q_capacity_holds
#print axioms Sluice.Formal.q_fifo_holds
#print axioms Sluice.Formal.q_drained_holds
#print axioms Sluice.Formal.qRun_closed_inert
#print axioms Sluice.Formal.qExtRun_closed_inert
#print axioms Sluice.Formal.qPark_inert
#print axioms Sluice.Formal.qFinish_inert
#print axioms Sluice.Formal.q_closed_no_commit
#print axioms Sluice.Formal.q_battery_inline
#print axioms Sluice.Formal.q_battery_handoff_pc
#print axioms Sluice.Formal.q_battery_handoff_cp
#print axioms Sluice.Formal.q_battery_close_buffered
#print axioms Sluice.Formal.q_battery_close_consumer
#print axioms Sluice.Formal.q_battery_close_producer
#print axioms Sluice.Formal.seqOK_extPush
#print axioms Sluice.Formal.q_not_extPush
#print axioms Sluice.Formal.enc_extPush
#print axioms Sluice.Formal.q_over_produces_of_extCap
#print axioms Sluice.Formal.seqOK_extTryPush
#print axioms Sluice.Formal.q_possesses_extTryPush
#print axioms Sluice.Formal.extCap_true_of_extTryPushIssue
#print axioms Sluice.Formal.q_under_produces_of_no_extCap
#print axioms Sluice.Formal.q_capability_defer
#print axioms Sluice.Formal.q_encoding_class_inhabited
#print axioms Sluice.Formal.encNoTry_under
#print axioms Sluice.Formal.encExtPush_over
#print axioms Sluice.Formal.qM1_breaks
#print axioms Sluice.Formal.qM2_fifo_break
#print axioms Sluice.Formal.qM3_handoff_refuted
#print axioms Sluice.Formal.qM4_wrong_result
#print axioms Sluice.Formal.qM5_handoff_refuted
#print axioms Sluice.Formal.selRun_result_agrees
#print axioms Sluice.Formal.selExtRun_result_agrees
#print axioms Sluice.Formal.selFinish_result_agrees
#print axioms Sluice.Formal.selResolveEv_preserves
#print axioms Sluice.Formal.selSetSection_preserves
#print axioms Sluice.Formal.selRun_preserves
#print axioms Sluice.Formal.selExtRun_preserves
#print axioms Sluice.Formal.selPark_preserves
#print axioms Sluice.Formal.selFinish_preserves
#print axioms Sluice.Formal.selExpire_preserves
#print axioms Sluice.Formal.sel_safe
#print axioms Sluice.Formal.sel_safe_holds
#print axioms Sluice.Formal.sel_battery_inlineEv
#print axioms Sluice.Formal.sel_battery_inlineTim
#print axioms Sluice.Formal.sel_battery_handoffEv
#print axioms Sluice.Formal.sel_battery_handoffTim
#print axioms Sluice.Formal.sel_battery_prioEv
#print axioms Sluice.Formal.sel_battery_prioTim
#print axioms Sluice.Formal.sel_battery_resetBlind
#print axioms Sluice.Formal.sel_battery_fiberSet
#print axioms Sluice.Formal.sel_battery_rearm
#print axioms Sluice.Formal.sel_battery_noSpurious
#print axioms Sluice.Formal.seqOK_extSel
#print axioms Sluice.Formal.sel_not_extSel
#print axioms Sluice.Formal.enc_extSel
#print axioms Sluice.Formal.sel_over_produces_of_extCap
#print axioms Sluice.Formal.seqOK_extSet
#print axioms Sluice.Formal.sel_possesses_extSet
#print axioms Sluice.Formal.extCap_true_of_extSetIssue
#print axioms Sluice.Formal.sel_under_produces_of_no_extCap
#print axioms Sluice.Formal.sel_capability_defer
#print axioms Sluice.Formal.sel_encoding_class_inhabited
#print axioms Sluice.Formal.encNoSel_under
#print axioms Sluice.Formal.encExtSel_over
#print axioms Sluice.Formal.selSafe_done
#print axioms Sluice.Formal.selM1_breaks
#print axioms Sluice.Formal.selM2_wrong_result
#print axioms Sluice.Formal.selM3_breaks
#print axioms Sluice.Formal.selM4_removes
#print axioms Sluice.Formal.selM5_wrong_arm
#print axioms Sluice.Formal.runRuns_cons
#print axioms Sluice.Formal.run_safe
#print axioms Sluice.Formal.runSafe_runInit
#print axioms Sluice.Formal.run_safe_holds
#print axioms Sluice.Formal.runApply_preserves
#print axioms Sluice.Formal.runEffectIn_preserves
#print axioms Sluice.Formal.runEffectOut_preserves
#print axioms Sluice.Formal.runSpawnDone_preserves
#print axioms Sluice.Formal.runDispatch_preserves
#print axioms Sluice.Formal.runWorkDone_preserves
#print axioms Sluice.Formal.runDrainEnter_preserves
#print axioms Sluice.Formal.runDrainReturn_preserves
#print axioms Sluice.Formal.battery_canonical
#print axioms Sluice.Formal.seqOK_b1
#print axioms Sluice.Formal.battery_fifo
#print axioms Sluice.Formal.seqOK_b2
#print axioms Sluice.Formal.battery_midDrain
#print axioms Sluice.Formal.seqOK_b3
#print axioms Sluice.Formal.battery_staleWindow
#print axioms Sluice.Formal.seqOK_b4
#print axioms Sluice.Formal.battery_seqDrains
#print axioms Sluice.Formal.seqOK_b5
#print axioms Sluice.Formal.battery_empty
#print axioms Sluice.Formal.seqOK_b6
#print axioms Sluice.Formal.run_not_extWork
#print axioms Sluice.Formal.enc_extWork
#print axioms Sluice.Formal.run_over_produces_of_extCap
#print axioms Sluice.Formal.drainCap_true_of_drainIssue
#print axioms Sluice.Formal.run_under_produces_of_no_extCap
#print axioms Sluice.Formal.run_capability_defer
#print axioms Sluice.Formal.run_encoding_class_inhabited
#print axioms Sluice.Formal.encNoDrain_under
#print axioms Sluice.Formal.encExtWork_over
#print axioms Sluice.Formal.silent_run_nil
#print axioms Sluice.Formal.silent_run_some
#print axioms Sluice.Formal.silent_run_none
#print axioms Sluice.Formal.runM1_breaks
#print axioms Sluice.Formal.runM2_breaks
#print axioms Sluice.Formal.runM3_wrong_result
#print axioms Sluice.Formal.runM4_removes
#print axioms Sluice.Formal.runM5_unbacked
EOF
lake env lean .lake/axiom-audit.lean | tee .lake/axiom-audit.out
rm -f .lake/axiom-audit.lean

# Every axiom list must contain only the standard logic principles
# (propext, Classical.choice, Quot.sound); 'sorryAx' would appear here if
# any proof body contained a sorry.
allowed='^\[(propext|Classical\.choice|Quot\.sound)(, (propext|Classical\.choice|Quot\.sound))*\]$'
bad=$(grep -oE '\[[^]]*\]' .lake/axiom-audit.out | grep -vcE "$allowed" || true)
if [[ "$bad" -ne 0 ]]; then
    echo "FAIL: axiom list outside the allowed set" >&2
    exit 1
fi
if grep -q 'sorryAx' .lake/axiom-audit.out; then
    echo "FAIL: sorryAx dependency detected" >&2
    exit 1
fi
rm -f .lake/axiom-audit.out

echo "VERIFY_FORMAL: PASS"
