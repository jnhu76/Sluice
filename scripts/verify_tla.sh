#!/usr/bin/env bash
# FORMAL-CAPABILITY-BOUNDARY-1 TLA+ verification gate.
#
#   Stage 1V2.2 (Event):
#     1. TLC on the Event primitive model (`formal/tla/EventCore.tla`, the
#        V2.2 execution-domain machine): TypeOK and the issue-anchored
#        no-wait-before-set safety property must hold.
#     2. TLC on the negative mutant (wait completing inline on a clear flag,
#        Lean `eventMutant`): the safety property MUST be violated, proving
#        the check bites.
#
#   Stage 2V2.3 (Semaphore, `formal/tla/SemCore.tla`):
#     3. Safety: the constructor-domain matrix (initial in {0,1} x
#        max in {1,2}) must complete cleanly under all nine safety
#        invariants (TypeOK .. InvCompDiscipline plus InvReleaseResult).
#     4. Witness: the V2.3 return-window witness (InvWitness) must be
#        reachable in the correct model -- the check MUST be violated.
#     5. Coverage: the 13 scenario certificates are negated conjunctions;
#        each MUST be violated, which certifies the scenario is reachable.
#     6. Safety mutants: each mutant switch must be killed by its expected
#        invariant (a clean pass, or a failure for any other reason, fails
#        the gate).  The release result semantics are gated separately:
#        MutOverflowFull (ceiling overflow, result/effect consistent) must
#        die on the capacity invariants, while MutWrongFullResult (ceiling
#        refusal returns true) and MutWrongGrantResult (grant returns
#        false) must die exactly on InvReleaseResult -- the independent
#        binding between the observable release(bool) and the effect
#        branch that actually ran.
#     7. Fused-return mutant (FiberEffect and FiberDone fused into one
#        step, the pre-V2.3 shape): must complete CLEANLY -- all nine
#        safety invariants including InvReleaseResult.  The witness
#        becomes unreachable there -- that unreachability is the
#        separation certificate for the V2.3 split.
#
# Every expected failure is checked against its specific invariant line,
# never against a bare exit code.
#
#   Stage 4V2.3 (Condition, `formal/tla/CondCore.tla`):
#     8. Safety: the three boot configurations (primInit-faithful, the
#        slot-holder start, the parked-waiter start) must complete
#        cleanly under all seven safety invariants.
#     9. Coverage: the six scenario certificates are negated
#        conjunctions; each MUST be violated (the witness is reachable).
#    10. Safety mutants: MutSpurious (M1, the finish ignores the record)
#        and MutNoTake (M3, the reacquire skips the slot take) must die
#        on their discipline invariants; MutParkHoldsOwn (M4's release
#        skipped) must die on InvQueueOwnership (the slot holder parked).
#    11. Under-production witness separations: MutDrainOne (M2,
#        notify_all readies only the head) and MutParkHolds (M4) must
#        complete CLEANLY with the broadcast/fast-path witness
#        invariants asserted -- the GOOD witnesses are unreachable there
#        (the coverage cfgs certify they ARE reachable in the correct
#        model).
#
#   Stage 5V2.3 (RwLock, `formal/tla/RwCore.tla`):
#    12. Safety: the boot configurations (primInit-faithful, two queued
#        readers, held write + queued writer) must complete cleanly
#        under all eight safety invariants.
#    13. Coverage: the seven scenario certificates are negated
#        conjunctions; each MUST be violated (the witness is reachable).
#    14. Safety mutants: MutGrantWrite (M1, the writer claim ignores the
#        reader count) must die on InvExclusion; MutNoPay (M2,
#        unlock_read skips the decrement) on InvLedger; MutOwnerSkip
#        (M4, the unlock_write owner gate dropped) on InvWriterOwned.
#    15. Under-production witness separation: MutBatchOne (M3, the
#        reader batch grants only the head) must complete CLEANLY with
#        full safety plus NotBatchWitness -- the batch witness is
#        unreachable there (the coverage cfg certifies it IS reachable
#        in the correct model).
#
# `-deadlock` is legitimate here: the fuel bounds (MaxHistory and friends)
# create terminal parked states by construction.
#
# Usage: scripts/verify_tla.sh   (from the repository root)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tla="$root/formal/tla"
jar="$tla/tla2tools.jar"

if [[ ! -f "$jar" ]]; then
    echo "tla2tools.jar missing: cannot verify" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# run_clean <label> <cfg> <module>: TLC must complete with no error.
run_clean() {
    local label="$1" cfg="$2" module="$3"
    echo "== TLC: $label =="
    java -cp "$jar" tlc2.TLC -deadlock -config "$tla/$cfg" \
        -metadir "$work/mc-$label" "$tla/$module" > "$work/$label.log" 2>&1 || {
        cat "$work/$label.log"
        echo "FAIL: $label did not complete cleanly" >&2
        exit 1
    }
    grep -q "Model checking completed. No error has been found." "$work/$label.log" || {
        cat "$work/$label.log"
        echo "FAIL: $label did not report clean completion" >&2
        exit 1
    }
    grep -E "Model checking completed|states generated" "$work/$label.log"
}

# run_violate <label> <cfg> <module> <invariant>: TLC must fail with that
# exact invariant violated -- a pass, or a failure for any other reason,
# aborts the gate.
run_violate() {
    local label="$1" cfg="$2" module="$3" inv="$4"
    echo "== TLC: $label (must violate $inv) =="
    set +e
    java -cp "$jar" tlc2.TLC -deadlock -config "$tla/$cfg" \
        -metadir "$work/mc-$label" "$tla/$module" > "$work/$label.log" 2>&1
    local rc=$?
    set -e
    if [[ "$rc" -eq 0 ]]; then
        cat "$work/$label.log"
        echo "FAIL: $label passed -- $inv does not bite" >&2
        exit 1
    fi
    grep -q "Invariant $inv is violated" "$work/$label.log" || {
        cat "$work/$label.log"
        echo "FAIL: $label failed for the wrong reason (expected: $inv)" >&2
        exit 1
    }
    grep -E "Invariant $inv is violated|states generated" "$work/$label.log"
}

# run_violate_any <label> <cfg> <module> "<inv1>|<inv2>": TLC must fail with
# one of the named invariants violated.  Used for MutOverflowFull, whose
# ceiling breakage is caught by both TypeOK (type domain) and InvCapacity
# (the ceiling literal); either report is the intended kill.
run_violate_any() {
    local label="$1" cfg="$2" module="$3" invs="$4"
    echo "== TLC: $label (must violate one of: $invs) =="
    set +e
    java -cp "$jar" tlc2.TLC -deadlock -config "$tla/$cfg" \
        -metadir "$work/mc-$label" "$tla/$module" > "$work/$label.log" 2>&1
    local rc=$?
    set -e
    if [[ "$rc" -eq 0 ]]; then
        cat "$work/$label.log"
        echo "FAIL: $label passed -- none of ($invs) bites" >&2
        exit 1
    fi
    grep -qE "Invariant ($invs) is violated" "$work/$label.log" || {
        cat "$work/$label.log"
        echo "FAIL: $label failed for the wrong reason (expected one of: $invs)" >&2
        exit 1
    }
    grep -E "Invariant .* is violated|states generated" "$work/$label.log"
}

echo "== Stage 1V2.2: Event =="
run_clean event-main EventCore.cfg EventCore
run_violate event-mutant EventCoreMutant.cfg EventCore NoWaitBeforeSet

echo "== Stage 2V2.3: SemCore safety matrix =="
for cfg in SemCore SemCoreI1 SemCoreM2 SemCoreI1M2; do
    run_clean "sem-safety-$cfg" "$cfg.cfg" SemCore
done

echo "== Stage 2V2.3: V2.3 seam witness (must be reachable) =="
run_violate sem-witness SemCoreCovWitness.cfg SemCore InvWitness

echo "== Stage 2V2.3: scenario coverage (each must be reachable) =="
run_violate sem-cov-a1 SemCoreCovA1.cfg SemCore InvCovW1
run_violate sem-cov-a2 SemCoreCovA2.cfg SemCore InvCovW2
run_violate sem-cov-a3 SemCoreCovA3.cfg SemCore InvCovQ
run_violate sem-cov-b1 SemCoreCovB1.cfg SemCore InvCovW1
run_violate sem-cov-b2 SemCoreCovB2.cfg SemCore InvCovInitial
run_violate sem-cov-b3 SemCoreCovB3.cfg SemCore InvCovQ
run_violate sem-cov-c1 SemCoreCovC1.cfg SemCore InvCovMax2
run_violate sem-cov-c2 SemCoreCovC2.cfg SemCore InvCovW2
run_violate sem-cov-c3 SemCoreCovC3.cfg SemCore InvCovReorder
run_violate sem-cov-d1 SemCoreCovD1.cfg SemCore InvCovW1
run_violate sem-cov-d2 SemCoreCovD2.cfg SemCore InvCovInitial
run_violate sem-cov-d3 SemCoreCovD3.cfg SemCore InvCovMax2State
run_violate sem-cov-d4 SemCoreCovD4.cfg SemCore InvCovQ1

echo "== Stage 2V2.3: safety mutants (each must die on its expected invariant) =="
run_violate sem-mut-create-permit SemCoreMutCreatePermit.cfg SemCore InvPermitPool
run_violate sem-mut-lose-permit SemCoreMutLosePermit.cfg SemCore InvPermitPool
run_violate sem-mut-double-consume SemCoreMutDoubleConsume.cfg SemCore InvPermitPool
run_violate sem-mut-fifo-bypass SemCoreMutFifoBypass.cfg SemCore InvFifo

echo "== Stage 2V2.3: release result-semantics mutants =="
run_violate_any sem-mut-overflow-full SemCoreMutOverflowFull.cfg SemCore "TypeOK|InvCapacity"
run_violate sem-mut-wrong-full-result SemCoreMutWrongFullResult.cfg SemCore InvReleaseResult
run_violate sem-mut-wrong-grant-result SemCoreMutWrongGrantResult.cfg SemCore InvReleaseResult

echo "== Stage 2V2.3: fused-return mutant (witness must become unreachable) =="
run_clean sem-mut-fused-return SemCoreMutFusedReturn.cfg SemCore

echo "== Stage 3V2.3: MutexCore safety matrix =="
run_clean mutex-main MutexCore.cfg MutexCore

echo "== Stage 3V2.3: V2.3 seam witness (must be reachable) =="
run_violate mutex-witness MutexCoreWitness.cfg MutexCore InvWitness

echo "== Stage 3V2.3: scenario coverage (each must be reachable) =="
run_violate mutex-cov-w1 MutexCoreCovW1.cfg MutexCore InvCovW1
run_violate mutex-cov-q MutexCoreCovQ.cfg MutexCore InvCovQ
run_violate mutex-cov-tryboth MutexCoreCovTryBoth.cfg MutexCore InvCovTryBoth
run_violate mutex-cov-cancelchain MutexCoreCovCancelChain.cfg MutexCore InvCovCancelChain
run_violate mutex-cov-cancelmiss MutexCoreCovCancelMiss.cfg MutexCore InvCovCancelMiss
run_violate mutex-cov-extwindow MutexCoreCovExtWindow.cfg MutexCore InvCovExtWindow

echo "== Stage 3V2.3: safety mutants (each must die on its expected invariant) =="
run_violate mutex-mut-grant-held MutexCoreMutGrantHeld.cfg MutexCore InvBalance
run_violate mutex-mut-handoff-owner MutexCoreMutHandoffOwner.cfg MutexCore InvRecordOwner
run_violate mutex-mut-cancel-true MutexCoreMutCancelTrue.cfg MutexCore InvBalance

echo "== Stage 3V2.3: fused-return mutant (witness must become unreachable) =="
run_clean mutex-mut-fused-return MutexCoreMutFusedReturn.cfg MutexCore

echo "== Stage 4V2.3: CondCore safety matrix =="
for cfg in CondCore CondCoreBoot CondCoreBootWait3; do
    run_clean "cond-safety-$cfg" "$cfg.cfg" CondCore
done

echo "== Stage 4V2.3: scenario coverage (each must be reachable) =="
run_violate cond-cov-fastpath CondCoreCovFastPath.cfg CondCore InvCovFastPath
run_violate cond-cov-broadcast CondCoreCovBroadcast.cfg CondCore InvCovBroadcast
run_violate cond-cov-cancel CondCoreCovCancel.cfg CondCore InvCovCancel
run_violate cond-cov-handoff CondCoreCovHandoff.cfg CondCore InvCovHandoff
run_violate cond-cov-notify-empty CondCoreCovNotifyEmpty.cfg CondCore InvCovNotifyEmpty
run_violate cond-cov-cancel-miss CondCoreCovCancelMiss.cfg CondCore InvCovCancelMiss

echo "== Stage 4V2.3: safety mutants (each must die on its expected invariant) =="
run_violate cond-mut-spurious CondCoreMutSpurious.cfg CondCore InvFinishBacked
run_violate cond-mut-no-take CondCoreMutNoTake.cfg CondCore InvFinishOwned
run_violate cond-mut-park-holds-own CondCoreMutParkHoldsOwn.cfg CondCore InvQueueOwnership

echo "== Stage 4V2.3: under-production witness separations (must hold cleanly) =="
run_clean cond-mut-drain-one CondCoreMutDrainOne.cfg CondCore
run_clean cond-mut-park-holds CondCoreMutParkHolds.cfg CondCore

echo "== Stage 5V2.3: RwCore safety matrix =="
for cfg in RwCore RwCoreRq2 RwCoreWq1; do
    run_clean "rw-safety-$cfg" "$cfg.cfg" RwCore
done

echo "== Stage 5V2.3: scenario coverage (each must be reachable) =="
run_violate rw-cov-inline-read RwCoreCovInlineRead.cfg RwCore InvCovInlineRead
run_violate rw-cov-inline-write RwCoreCovInlineWrite.cfg RwCore InvCovInlineWrite
run_violate rw-cov-writer-claim RwCoreCovWriterClaim.cfg RwCore InvCovWriterClaim
run_violate rw-cov-batch RwCoreCovBatch.cfg RwCore InvCovBatch
run_violate rw-cov-cancel RwCoreCovCancel.cfg RwCore InvCovCancel
run_violate rw-cov-try-fail RwCoreCovTryFail.cfg RwCore InvCovTryFail
run_violate rw-cov-cancel-miss RwCoreCovCancelMiss.cfg RwCore InvCovCancelMiss

echo "== Stage 5V2.3: safety mutants (each must die on its expected invariant) =="
run_violate rw-mut-grant-write RwCoreMutGrantWrite.cfg RwCore InvExclusion
run_violate rw-mut-no-pay RwCoreMutNoPay.cfg RwCore InvLedger
run_violate rw-mut-owner-skip RwCoreMutOwnerSkip.cfg RwCore InvWriterOwned

echo "== Stage 5V2.3: under-production witness separation (must hold cleanly) =="
run_clean rw-mut-batch-one RwCoreMutBatchOne.cfg RwCore

echo "VERIFY_TLA: PASS"
