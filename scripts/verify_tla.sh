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
#   Stage 6V2.3 (Queue, `formal/tla/QueueCore.tla`):
#    16. Safety: the boot configurations (primInit-faithful, the full
#        ring with a parked producer, the parked consumer) must complete
#        cleanly under all nine safety invariants (capacity, the
#        commit/delivery ledger, drained consumers, no-commit-after-close,
#        result agreement, queue discipline, completion discipline).
#    17. Coverage: the ten scenario certificates (inline push, inline
#        pop, producer blocking, consumer blocking, the p->c and c->p
#        handoffs, close over a buffered item, close draining a parked
#        producer, close draining a parked consumer, capacity>1 FIFO)
#        are negated conjunctions; each MUST be violated (the witness is
#        reachable).
#    18. Mutants: MutCloseCommit (the closed gate dropped from push)
#        must die on InvNoCommitClosed; MutFifo (pop delivers the second
#        ring item) on InvLog; MutWrongResult (try_pop returns itemA on
#        an empty closed queue) on InvResultAgree.  MutNoConsumerGrant
#        and MutNoProducerGrant must complete CLEANLY with NotHandoffCP
#        / NotHandoffPC -- the handoff witnesses are unreachable there
#        (the coverage cfgs certify they ARE reachable in the correct
#        model); these two are TRACE-REMOVAL kills, not safety kills.
#
#   Stage 7V2.3 (Select, `formal/tla/SelectCore.tla`):
#    19. Safety: the boot configurations (primInit-faithful, the armed
#        select start, the armed + timer-due start) must complete
#        cleanly under all seven safety invariants (armed quiet,
#        phase/winner agreement with source-read ghosts, the one-record
#        completion window, group ownership, result agreement,
#        completion discipline).
#    20. Coverage: the eight scenario certificates (inline event win,
#        inline timer win, the set-section hand-off, the timer-pump
#        hand-off, both-ready priority resolved to each arm, reset
#        blindness, the level-semantics re-arm) are negated
#        conjunctions; each MUST be violated (the witness is
#        reachable).
#    21. Mutants: MutReResolve (the done-group gate dropped from the
#        set section) must die on InvWindow; MutFinishLie (the resumed
#        select delivers wonEv regardless of the consumed record) and
#        MutM5 (the scan order flipped) on InvResultAgree; MutParkReady
#        (the ready-source gate dropped from the park) on
#        InvQuietArmed.
#    22. Trace-removal witness separation: MutNoTimer (the timer pump
#        omitted) must complete CLEANLY with full safety plus
#        NotHandoffTim -- the timer-handoff witness is unreachable
#        there (the coverage cfg certifies it IS reachable in the
#        correct model); a TRACE-REMOVAL kill, not a safety kill.
#
#   Stage 8V2.3 (Driver, `formal/tla/DriverCore.tla`):
#    23. Safety: the two boot configurations (runInit-faithful, the
#        loaded mid-run start with the drain in flight over a queued
#        task) must complete cleanly under all eight safety invariants
#        (task conservation, id bounds, worker occupancy, the
#        idle-backlog shape, terminate quiescence, the spawn-record
#        shape, completion discipline).
#    24. Coverage: the seven scenario certificates (the empty drain,
#        two tasks through the backlog and the worker, the mid-run
#        spawn section minting onto the backlog, the entry flush, the
#        stale-classify window, the post-terminate unclaimed spawn, the
#        task issue) are negated conjunctions; each MUST be violated
#        (the witness is reachable).
#    25. Mutants: MutQuiescence (the drain-return quiescence gates
#        dropped) must die on InvTermQuiet; MutDuplicateDispatch (the
#        worker-free gate dropped from dispatch) on InvConservation;
#        MutDrainResult (the drain returns rDone) on InvDrainResult.
#    26. Trace-removal witness separation: MutSilentDispatch (the
#        task's issue observation dropped) must complete CLEANLY with
#        full state safety plus NotWorkIssue -- no step emits a `work`
#        issue there (the coverage cfg certifies the issue IS reachable
#        in the correct model); a TRACE-REMOVAL kill, not a safety
#        kill.  The completion discipline is excluded from that
#        configuration: the removed issue orphans the completion, and
#        that break IS the removal.
#    27. Result-binding separation: MutUnbackedMint (the spawn section
#        fixes the result without minting the task) must be VIOLATED on
#        NotUnbacked -- the unbacked four-observation trace becomes
#        reachable there while the state stays safe; the correct
#        model's unreachability of that trace is the Lean separation.
#
# `-deadlock` is legitimate here: the fuel bounds (MaxHistory and friends)
# create terminal parked states by construction.
#
# Usage: scripts/verify_tla.sh   (from the repository root)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tla="$root/formal/tla"
jar="$tla/tla2tools.jar"

# TLC retrieval is pinned and checksummed (issue #391, formal evidence reset):
# the jar is never committed (gitignored); a clean clone bootstraps the exact
# pinned stable release and verifies its SHA-256 before any model runs.
# SLUICE_TLA2TOOLS_JAR points at a pre-provisioned jar (CI cache, offline
# mirror); it bypasses the download but still enforces the pinned SHA-256.
TLA2TOOLS_VERSION="1.7.4"
TLA2TOOLS_SHA256="936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88"
TLA2TOOLS_URL="https://github.com/tlaplus/tlaplus/releases/download/v${TLA2TOOLS_VERSION}/tla2tools.jar"

jar_sha() {
    sha256sum "$1" 2>/dev/null | cut -d' ' -f1
}

if [[ -n "${SLUICE_TLA2TOOLS_JAR:-}" ]]; then
    if [[ ! -f "$SLUICE_TLA2TOOLS_JAR" ]]; then
        echo "SLUICE_TLA2TOOLS_JAR=$SLUICE_TLA2TOOLS_JAR not found: cannot verify" >&2
        exit 1
    fi
    if [[ "$(jar_sha "$SLUICE_TLA2TOOLS_JAR")" != "$TLA2TOOLS_SHA256" ]]; then
        echo "SLUICE_TLA2TOOLS_JAR=$SLUICE_TLA2TOOLS_JAR SHA-256 mismatch: refusing to verify" >&2
        echo "expected: $TLA2TOOLS_SHA256" >&2
        exit 1
    fi
    jar="$SLUICE_TLA2TOOLS_JAR"
    echo "== TLC jar (SLUICE_TLA2TOOLS_JAR override, checksum ok): $jar =="
elif [[ -f "$jar" && "$(jar_sha "$jar")" == "$TLA2TOOLS_SHA256" ]]; then
    echo "== TLC jar (cached, checksum ok): v$TLA2TOOLS_VERSION =="
elif [[ -f "$jar" ]]; then
    echo "formal/tla/tla2tools.jar exists but its SHA-256 does not match pinned" >&2
    echo "v$TLA2TOOLS_VERSION ($TLA2TOOLS_SHA256); delete it, or point SLUICE_TLA2TOOLS_JAR" >&2
    echo "at a known jar: cannot verify against an untrusted binary" >&2
    exit 1
else
    echo "== TLC jar absent: bootstrapping pinned v$TLA2TOOLS_VERSION =="
    curl -fSL --retry 3 -o "$jar" "$TLA2TOOLS_URL" || {
        rm -f "$jar"
        echo "download of tla2tools v$TLA2TOOLS_VERSION failed: cannot verify" >&2
        echo "needs: java, curl and network; or pre-provision SLUICE_TLA2TOOLS_JAR" >&2
        exit 1
    }
    if [[ "$(jar_sha "$jar")" != "$TLA2TOOLS_SHA256" ]]; then
        rm -f "$jar"
        echo "tla2tools SHA-256 mismatch: refusing to verify" >&2
        exit 1
    fi
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

# run_live_clean <label> <cfg> <module>: TLC must complete cleanly with the
# temporal PROPERTYs of the cfg checked (liveness-carrying spec).
run_live_clean() {
    local label="$1" cfg="$2" module="$3"
    echo "== TLC: $label (temporal properties) =="
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
    grep -q "Finished checking temporal properties" "$work/$label.log" || {
        cat "$work/$label.log"
        echo "FAIL: $label never checked its temporal properties" >&2
        exit 1
    }
    grep -E "Model checking completed|states generated" "$work/$label.log"
}

# run_temporal_violate <label> <cfg> <module>: TLC must fail because a
# temporal property (liveness obligation) is violated.
run_temporal_violate() {
    local label="$1" cfg="$2" module="$3"
    echo "== TLC: $label (must violate a temporal property) =="
    set +e
    java -cp "$jar" tlc2.TLC -deadlock -config "$tla/$cfg" \
        -metadir "$work/mc-$label" "$tla/$module" > "$work/$label.log" 2>&1
    local rc=$?
    set -e
    if [[ "$rc" -eq 0 ]]; then
        cat "$work/$label.log"
        echo "FAIL: $label passed -- the liveness obligation does not bite" >&2
        exit 1
    fi
    grep -q "Temporal properties were violated" "$work/$label.log" || {
        cat "$work/$label.log"
        echo "FAIL: $label failed for the wrong reason (expected: temporal violation)" >&2
        exit 1
    }
    grep -E "Temporal properties were violated|states generated" "$work/$label.log"
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

echo "== Stage 6V2.3: QueueCore safety matrix =="
for cfg in QueueCore QueueCoreFull2 QueueCoreEmptyC; do
    run_clean "queue-safety-$cfg" "$cfg.cfg" QueueCore
done

echo "== Stage 6V2.3: scenario coverage (each must be reachable) =="
run_violate queue-cov-inline-push QueueCoreCovInlinePush.cfg QueueCore InvCovInlinePush
run_violate queue-cov-inline-pop QueueCoreCovInlinePop.cfg QueueCore InvCovInlinePop
run_violate queue-cov-push-park QueueCoreCovPushPark.cfg QueueCore InvCovPushPark
run_violate queue-cov-pop-park QueueCoreCovPopPark.cfg QueueCore InvCovPopPark
run_violate queue-cov-handoff-pc QueueCoreCovHandoffPC.cfg QueueCore InvCovHandoffPC
run_violate queue-cov-handoff-cp QueueCoreCovHandoffCP.cfg QueueCore InvCovHandoffCP
run_violate queue-cov-close-buffered QueueCoreCovCloseBuffered.cfg QueueCore InvCovCloseBuffered
run_violate queue-cov-close-pushpark QueueCoreCovClosePushPark.cfg QueueCore InvCovClosePushPark
run_violate queue-cov-close-poppark QueueCoreCovClosePopPark.cfg QueueCore InvCovClosePopPark
run_violate queue-cov-fifo-multi QueueCoreCovFifoMulti.cfg QueueCore InvCovFifoMulti

echo "== Stage 6V2.3: safety mutants (each must die on its expected invariant) =="
run_violate queue-mut-close-commit QueueCoreMutCloseCommit.cfg QueueCore InvNoCommitClosed
run_violate queue-mut-fifo QueueCoreMutFifo.cfg QueueCore InvLog
run_violate queue-mut-wrong-result QueueCoreMutWrongResult.cfg QueueCore InvResultAgree

echo "== Stage 6V2.3: trace-removal witness separations (must hold cleanly) =="
run_clean queue-mut-no-consumer-grant QueueCoreMutNoConsumerGrant.cfg QueueCore
run_clean queue-mut-no-producer-grant QueueCoreMutNoProducerGrant.cfg QueueCore

echo "== Stage 7V2.3: SelectCore safety matrix =="
for cfg in SelectCore SelectCoreArmed SelectCoreDue; do
    run_clean "select-safety-$cfg" "$cfg.cfg" SelectCore
done

echo "== Stage 7V2.3: scenario coverage (each must be reachable) =="
run_violate select-cov-inline-ev SelectCoreCovInlineEv.cfg SelectCore InvCovInlineEv
run_violate select-cov-inline-tim SelectCoreCovInlineTim.cfg SelectCore InvCovInlineTim
run_violate select-cov-handoff-ev SelectCoreCovHandoffEv.cfg SelectCore InvCovHandoffEv
run_violate select-cov-handoff-tim SelectCoreCovHandoffTim.cfg SelectCore InvCovHandoffTim
run_violate select-cov-prio-ev SelectCoreCovPrioEv.cfg SelectCore InvCovPrioEv
run_violate select-cov-prio-tim SelectCoreCovPrioTim.cfg SelectCore InvCovPrioTim
run_violate select-cov-reset-blind SelectCoreCovResetBlind.cfg SelectCore InvCovResetBlind
run_violate select-cov-rearm SelectCoreCovRearm.cfg SelectCore InvCovRearm

echo "== Stage 7V2.3: safety + result mutants (each must die on its expected invariant) =="
run_violate select-mut-reresolve SelectCoreMutReResolve.cfg SelectCore InvWindow
run_violate select-mut-finish-lie SelectCoreMutFinishLie.cfg SelectCore InvResultAgree
run_violate select-mut-park-ready SelectCoreMutParkReady.cfg SelectCore InvQuietArmed
run_violate select-mut-scan-flip SelectCoreMutScanFlip.cfg SelectCore InvResultAgree

echo "== Stage 7V2.3: trace-removal witness separation (must hold cleanly) =="
run_clean select-mut-no-timer SelectCoreMutNoTimer.cfg SelectCore

echo "== Stage 8V2.3: DriverCore safety matrix =="
for cfg in DriverCore DriverCoreLoaded; do
    run_clean "driver-safety-$cfg" "$cfg.cfg" DriverCore
done

echo "== Stage 8V2.3: scenario coverage (each must be reachable) =="
run_violate driver-cov-empty-drain DriverCoreCovEmptyDrain.cfg DriverCore InvCovEmptyDrain
run_violate driver-cov-fifo-two DriverCoreCovFifoTwo.cfg DriverCore InvCovFifoTwo
run_violate driver-cov-effect-in DriverCoreCovEffectIn.cfg DriverCore InvCovEffectIn
run_violate driver-cov-flush DriverCoreCovFlush.cfg DriverCore InvCovFlush
run_violate driver-cov-stale DriverCoreCovStale.cfg DriverCore InvCovStale
run_violate driver-cov-post-term DriverCoreCovPostTerm.cfg DriverCore InvCovPostTerm
run_violate driver-cov-dispatch DriverCoreCovDispatch.cfg DriverCore InvCovDispatch

echo "== Stage 8V2.3: safety + result mutants (each must die on its expected invariant) =="
run_violate driver-mut-quiescence DriverCoreMutQuiescence.cfg DriverCore InvTermQuiet
run_violate driver-mut-duplicate-dispatch DriverCoreMutDuplicateDispatch.cfg DriverCore InvConservation
run_violate driver-mut-drain-result DriverCoreMutDrainResult.cfg DriverCore InvDrainResult

echo "== Stage 8V2.3: trace-removal witness separation (must hold cleanly) =="
run_clean driver-mut-silent-dispatch DriverCoreMutSilentDispatch.cfg DriverCore

echo "== Stage 8V2.3: result-binding separation (must be reachable there) =="
run_violate driver-mut-unbacked-mint DriverCoreMutUnbackedMint.cfg DriverCore NotUnbacked

#   Stage B1-1 (#394): RequestCore, the first liveness-carrying model.
#    28. Safety: the full fact set (admission close race, single acceptance,
#        single admissible winner, generation matching, cancel-admissibility,
#        publication gated on exec retirement, release invalidation, the
#        four-pin reclaim predicate, health-gated acceptance, the E-chain
#        capability rules) must complete cleanly.
#    29. Liveness: PROPERTY L1 (accepted ~> published) and L5 (reclaimable ~>
#        reclaimed) under the declared WF_ fairness conjuncts, by full
#        request identity.
#    30. Safety mutants: each switch must die on its named invariant.
#        MutGenWrap dies first on InvAcceptedRepresented (the wrapped slot
#        strands the accepted identity); InvSingleAcceptance kills the same
#        mutant when checked alone (recorded in the B1-1 evidence note).
#        MutExecAfterTerminal may present as either InvNoExecRevival or
#        InvNoPubWhileExec depending on which violating state BFS reaches
#        first, so both kills are the intended one.
#    31. Liveness mutants: MutStrandPostAccept must violate L1 (a recorded
#        intent swallowing the physical outcome strands the request);
#        MutLazyReclaim must violate L5 (reclaim gated on an unrelated
#        submit wake); MutCtlAfterSettled must violate L5 (re-acquiring
#        control on settled work keeps Reclaimable non-monotone and starves
#        the reclaim service through intermittent enabling).
#    32. Separation witness: MutReadyBeforePayload (publication may begin
#        with no terminal chosen) must complete CLEANLY since the B1-1
#        Corrective-1 E-chain rule: exec = 0 with no terminal is now
#        unreachable, so the fault is subsumed one layer earlier and the
#        mutant's unkillability is that subsumption's certificate.
#    33. B2 (#395) public consumption mutants: MutConsumeKeepsBind
#        (take_result forgets the binding release) must die on
#        InvReleasedNotLive; MutConsumeUnpublished (consume runs before
#        publication completes) must die on InvConsumeAfterPublish -- the
#        per-identity pubDoneIds fact is what makes "was published"
#        expressible, because consume promotes stage past 2 in the same
#        step it records consumption.  MutDiscardInflight (public
#        discard/destruction accepts an inflight publication) must die on
#        InvDiscardAfterPublish -- the per-identity discarded[] fact is
#        what makes "was publicly discarded" expressible, because discard
#        promotes stage past 2 in the same step; ReleaseBind stays
#        deliberately inflight-tolerant as the compatibility flavor.
echo "== Stage B1-1: RequestCore safety =="
run_clean rcore-safety RequestCore.cfg RequestCore

echo "== Stage B1-1: RequestCore conditional liveness =="
run_live_clean rcore-live RequestCoreLive.cfg RequestCore

echo "== Stage B1-1: RequestCore safety mutants (ACTIVE MUTATION: each must violate its named property) =="
run_violate rcore-mut-ignore-close RequestCoreMutIgnoreClose.cfg RequestCore InvNoAcceptAfterClose
run_violate rcore-mut-ignore-health RequestCoreMutIgnoreHealth.cfg RequestCore InvNoAcceptAfterHealth
run_violate rcore-mut-rollback-residue RequestCoreMutRollbackResidue.cfg RequestCore InvUnoccupiedClean
run_violate rcore-mut-terminal-overwrite RequestCoreMutTerminalOverwrite.cfg RequestCore InvSingleWinner
run_violate rcore-mut-cancel-as-physical RequestCoreMutCancelAsPhysical.cfg RequestCore InvCancelWinRequiresUnclaimed
run_violate rcore-mut-publish-while-e RequestCoreMutPublishWhileE.cfg RequestCore InvNoPubWhileExec
run_violate rcore-mut-release-resolvable RequestCoreMutReleaseResolvable.cfg RequestCore InvReleasedNotLive
run_violate rcore-mut-gen-wrap RequestCoreMutGenWrap.cfg RequestCore InvAcceptedRepresented
run_violate rcore-mut-reclaim-ignores-pub RequestCoreMutReclaimIgnoresPub.cfg RequestCore InvReclaimConditions
run_violate rcore-mut-reclaim-ignores-ctl RequestCoreMutReclaimIgnoresCtl.cfg RequestCore InvReclaimConditions
run_violate rcore-mut-retire-last-exec RequestCoreMutRetireLastExec.cfg RequestCore InvExecHeldUntilTerminal
run_violate_any rcore-mut-exec-after-terminal RequestCoreMutExecAfterTerminal.cfg RequestCore "InvNoExecRevival|InvNoPubWhileExec"
run_violate rcore-mut-stale-event RequestCoreMutStaleEvent.cfg RequestCore InvTerminalMatchesGeneration
run_violate rcore-mut-double-decrement RequestCoreMutDoubleDecrement.cfg RequestCore TypeOK

echo "== Stage B1-1: RequestCore liveness mutants (ACTIVE MUTATION: each must violate a temporal property) =="
run_temporal_violate rcore-mut-strand-post-accept RequestCoreMutStrandPostAccept.cfg RequestCore
run_temporal_violate rcore-mut-lazy-reclaim RequestCoreMutLazyReclaim.cfg RequestCore
run_temporal_violate rcore-mut-ctl-after-settled RequestCoreMutCtlAfterSettled.cfg RequestCore

echo "== Stage B1-1: SUBSUMPTION WITNESS MutReadyBeforePayload (enabling state made unreachable; must remain clean) =="
run_clean rcore-mut-ready-before-payload RequestCoreMutReadyBeforePayload.cfg RequestCore

echo "== Stage B2: RequestCore public consumption mutants (ACTIVE MUTATION: each must violate its named invariant) =="
run_violate rcore-mut-consume-keeps-bind RequestCoreMutConsumeKeepsBind.cfg RequestCore InvReleasedNotLive
run_violate rcore-mut-consume-unpublished RequestCoreMutConsumeUnpublished.cfg RequestCore InvConsumeAfterPublish
run_violate rcore-mut-discard-inflight RequestCoreMutDiscardInflight.cfg RequestCore InvDiscardAfterPublish

echo "VERIFY_TLA: PASS"
