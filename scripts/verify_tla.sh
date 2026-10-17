#!/usr/bin/env bash
# TLA+ verification gate for the models that mirror C++ mutation seams:
# RequestCore, ObserverCore and ShutdownCore (formal/tla/). ProgressSource is
# gated separately by scripts/verify_progress_source_model.sh because its
# configurations pin their own verdict catalogues.
#
# The retired synchronization-primitive corpus (Event/Semaphore/Mutex/
# Condition/RwLock/Queue/Select/Driver models and their coverage/mutant
# configurations) is dated v1-campaign evidence in Git history; it carries no
# required v2 obligation and is no longer executed by this gate.
#
# Every expected failure is checked against its specific invariant line,
# never against a bare exit code.
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

# Parallel execution: the run_* helpers enqueue jobs; drain() launches them
# through a pool of concurrent TLC JVMs and aggregates the verdicts. The
# product SLUICE_TLA_JOBS x SLUICE_TLA_WORKERS should roughly match the host
# core count (inter-model parallelism x intra-model worker threads). Raise
# SLUICE_TLA_WORKERS when a single large model dominates (the rcore liveness
# stage); lower it when several large models would otherwise run concurrently
# and exhaust memory or metadir disk. SLUICE_TLA_JAVA_OPTS is passed to every
# JVM (e.g. -Xmx).
SLUICE_TLA_JOBS="${SLUICE_TLA_JOBS:-$(($(nproc 2>/dev/null || echo 4) / 2))}"
SLUICE_TLA_WORKERS="${SLUICE_TLA_WORKERS:-2}"
case "$SLUICE_TLA_JOBS" in ''|*[!0-9]*|0) SLUICE_TLA_JOBS=1;; esac
case "$SLUICE_TLA_WORKERS" in ''|*[!0-9]*|0) SLUICE_TLA_WORKERS=1;; esac

queue="$work/queue.tsv"
: > "$queue"

enqueue() {
    printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" >> "$queue"
}

tlc() {
    java ${SLUICE_TLA_JAVA_OPTS:-} -cp "$jar" tlc2.TLC \
        -workers "$SLUICE_TLA_WORKERS" -deadlock -config "$tla/$2" \
        -metadir "$work/mc-$1" "$tla/$3" > "$work/$1.log" 2>&1
}

pass_verdict() { echo "PASS" > "$work/$1.verdict"; }
fail_verdict() { printf 'FAIL %s\n' "$2" > "$work/$1.verdict"; }

worker() {
    local kind="$1" label="$2" cfg="$3" module="$4" inv="$5"
    local rc=0
    tlc "$label" "$cfg" "$module" || rc=$?
    case "$kind" in
    clean)
        if [[ "$rc" -ne 0 ]] \
            || ! grep -q "Model checking completed. No error has been found." "$work/$label.log"
        then
            fail_verdict "$label" "did not complete cleanly"
            return
        fi
        ;;
    violate)
        if [[ "$rc" -eq 0 ]]; then
            fail_verdict "$label" "passed -- $inv does not bite"
            return
        fi
        if ! grep -qE "Invariant ($inv) is violated" "$work/$label.log"; then
            fail_verdict "$label" "failed for the wrong reason (expected: $inv)"
            return
        fi
        ;;
    live_clean)
        if [[ "$rc" -ne 0 ]] \
            || ! grep -q "Model checking completed. No error has been found." "$work/$label.log" \
            || ! grep -q "Finished checking temporal properties" "$work/$label.log"
        then
            fail_verdict "$label" "did not complete cleanly with temporal properties"
            return
        fi
        ;;
    temporal_violate)
        if [[ "$rc" -eq 0 ]]; then
            fail_verdict "$label" "passed -- the liveness obligation does not bite"
            return
        fi
        if ! grep -q "Temporal properties were violated" "$work/$label.log"; then
            fail_verdict "$label" "failed for the wrong reason (expected: temporal violation)"
            return
        fi
        ;;
    esac
    pass_verdict "$label"
}

drain() {
    local failed=0 kind label cfg module inv
    while IFS=$'\t' read -r kind label cfg module inv; do
        while [[ "$(jobs -rp | wc -l)" -ge "$SLUICE_TLA_JOBS" ]]; do
            wait -n || true
        done
        worker "$kind" "$label" "$cfg" "$module" "$inv" &
    done < "$queue"
    wait
    while IFS=$'\t' read -r kind label cfg module inv; do
        if [[ "$(cat "$work/$label.verdict")" == PASS ]]; then
            grep -hE "Model checking completed|Temporal properties were violated|Invariant .* is violated" "$work/$label.log" | head -1
            grep -h "states generated" "$work/$label.log" | tail -1
        else
            echo "FAIL: $label -- $(cat "$work/$label.verdict")" >&2
            cat "$work/$label.log"
            failed=1
        fi
    done < "$queue"
    if [[ "$failed" -ne 0 ]]; then
        echo "VERIFY_TLA: FAIL" >&2
        exit 1
    fi
}

# run_clean <label> <cfg> <module>: TLC must complete with no error.
run_clean() {
    local label="$1" cfg="$2" module="$3"
    echo "== TLC: $label =="
    enqueue clean "$label" "$cfg" "$module" ""
}

# run_violate <label> <cfg> <module> <invariant>: TLC must fail with that
# exact invariant violated -- a pass, or a failure for any other reason,
# aborts the gate.
run_violate() {
    local label="$1" cfg="$2" module="$3" inv="$4"
    echo "== TLC: $label (must violate $inv) =="
    enqueue violate "$label" "$cfg" "$module" "$inv"
}

# run_violate_any <label> <cfg> <module> "<inv1>|<inv2>": TLC must fail with
# one of the named invariants violated.  Used for MutOverflowFull, whose
# ceiling breakage is caught by both TypeOK (type domain) and InvCapacity
# (the ceiling literal); either report is the intended kill.
run_violate_any() {
    local label="$1" cfg="$2" module="$3" invs="$4"
    echo "== TLC: $label (must violate one of: $invs) =="
    enqueue violate "$label" "$cfg" "$module" "$invs"
}

# run_live_clean <label> <cfg> <module>: TLC must complete cleanly with the
# temporal PROPERTYs of the cfg checked (liveness-carrying spec).
run_live_clean() {
    local label="$1" cfg="$2" module="$3"
    echo "== TLC: $label (temporal properties) =="
    enqueue live_clean "$label" "$cfg" "$module" ""
}

# run_temporal_violate <label> <cfg> <module>: TLC must fail because a
# temporal property (liveness obligation) is violated.
run_temporal_violate() {
    local label="$1" cfg="$2" module="$3"
    echo "== TLC: $label (must violate a temporal property) =="
    enqueue temporal_violate "$label" "$cfg" "$module" ""
}

#   RequestCore (issue #394 campaign record), the first liveness-carrying model.
#    28. Safety: the full fact set (admission close race, single acceptance,
#        single admissible winner, generation matching, cancel-admissibility,
#        publication gated on exec retirement, release invalidation, release
#        implies publication complete, the four-pin reclaim predicate,
#        health-gated acceptance, the E-chain capability rules) must
#        complete cleanly.
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
#    32. Subsumption witnesses (2): historical fault shapes whose enabling
#        state the current protocol makes unreachable, so their cfgs must
#        complete CLEANLY and a violation means the enabling state came
#        back.  MutReadyBeforePayload (publication may begin with no
#        terminal chosen) is subsumed by the B1-1 Corrective-1 E-chain rule
#        (exec = 0 with no terminal is unreachable).  MutReclaimIgnoresPub
#        (reclaim drops the published requirement) is subsumed by #524's
#        convergence: with the retired inflight-tolerant release flavor
#        gone, every release edge requires publication complete, so
#        InvBindImpliesPublished makes the dropped guard redundant -- and
#        that witness is the tripwire for an inflight-tolerant release
#        path reappearing.
#    33. B2 (#395) public release-edge mutants: each release edge carries its
#        own mutant.  MutConsumeKeepsBind (take_result forgets the binding
#        release) and MutDiscardKeepsBind (discard/destruction forgets it)
#        must each die on InvReleasedNotLive from their own edge;
#        MutConsumeUnpublished (consume runs before publication completes)
#        must die on InvConsumeAfterPublish and MutDiscardInflight (public
#        discard/destruction accepts an inflight publication) on
#        InvDiscardAfterPublish -- the per-identity pubDoneIds and
#        discarded[] facts are what make "was published" expressible,
#        because both edges promote stage past 2 in the same step they
#        record the consumption/discard.  #524 removed ReleaseBind, the
#        pre-F2 Completion release flavor, which was the only inflight-
#        tolerant release edge; MutReleaseResolvable died with it.
echo "== RequestCore:  safety =="
run_clean rcore-safety RequestCore.cfg RequestCore

echo "== RequestCore:  conditional liveness =="
run_live_clean rcore-live RequestCoreLive.cfg RequestCore

echo "== RequestCore:  safety mutants (ACTIVE MUTATION: each must violate its named property) =="
run_violate rcore-mut-ignore-close RequestCoreMutIgnoreClose.cfg RequestCore InvNoAcceptAfterClose
run_violate rcore-mut-ignore-health RequestCoreMutIgnoreHealth.cfg RequestCore InvNoAcceptAfterHealth
run_violate rcore-mut-rollback-residue RequestCoreMutRollbackResidue.cfg RequestCore InvUnoccupiedClean
run_violate rcore-mut-terminal-overwrite RequestCoreMutTerminalOverwrite.cfg RequestCore InvSingleWinner
run_violate rcore-mut-cancel-as-physical RequestCoreMutCancelAsPhysical.cfg RequestCore InvCancelWinRequiresUnclaimed
run_violate rcore-mut-publish-while-e RequestCoreMutPublishWhileE.cfg RequestCore InvNoPubWhileExec
run_violate rcore-mut-gen-wrap RequestCoreMutGenWrap.cfg RequestCore InvAcceptedRepresented
run_violate rcore-mut-reclaim-ignores-ctl RequestCoreMutReclaimIgnoresCtl.cfg RequestCore InvReclaimConditions
run_violate rcore-mut-retire-last-exec RequestCoreMutRetireLastExec.cfg RequestCore InvExecHeldUntilTerminal
run_violate_any rcore-mut-exec-after-terminal RequestCoreMutExecAfterTerminal.cfg RequestCore "InvNoExecRevival|InvNoPubWhileExec"
run_violate rcore-mut-stale-event RequestCoreMutStaleEvent.cfg RequestCore InvTerminalMatchesGeneration
run_violate rcore-mut-double-decrement RequestCoreMutDoubleDecrement.cfg RequestCore TypeOK

echo "== RequestCore:  liveness mutants (ACTIVE MUTATION: each must violate a temporal property) =="
run_temporal_violate rcore-mut-strand-post-accept RequestCoreMutStrandPostAccept.cfg RequestCore
run_temporal_violate rcore-mut-lazy-reclaim RequestCoreMutLazyReclaim.cfg RequestCore
run_temporal_violate rcore-mut-ctl-after-settled RequestCoreMutCtlAfterSettled.cfg RequestCore

echo "== RequestCore: SUBSUMPTION WITNESSES (enabling state made unreachable; each must remain clean) =="
run_clean rcore-mut-ready-before-payload RequestCoreMutReadyBeforePayload.cfg RequestCore
run_clean rcore-mut-reclaim-ignores-pub RequestCoreMutReclaimIgnoresPub.cfg RequestCore

echo "== Stage B2: RequestCore public release-edge mutants (ACTIVE MUTATION: each must violate its named invariant) =="
run_violate rcore-mut-consume-keeps-bind RequestCoreMutConsumeKeepsBind.cfg RequestCore InvReleasedNotLive
run_violate rcore-mut-consume-unpublished RequestCoreMutConsumeUnpublished.cfg RequestCore InvConsumeAfterPublish
run_violate rcore-mut-discard-keeps-bind RequestCoreMutDiscardKeepsBind.cfg RequestCore InvReleasedNotLive
run_violate rcore-mut-discard-inflight RequestCoreMutDiscardInflight.cfg RequestCore InvDiscardAfterPublish

#   ObserverCore (issues #396/#433 campaign record), the host-neutral observer delivery
#   protocol model derived from the merged implementation.
#    34. Safety: the full fact set (reset discipline, observer-cancel
#        isolation, attach/publishation atomicity in both directions, the
#        phase/delivery agreement, retired-finality, at-most-once claim and
#        retirement per registration episode, claim-behind-publication,
#        cancel-ack != delivery retirement, the episode-ledger observer
#        pin, host wait-record retention) must complete cleanly.
#    35. Liveness: PROPERTY L0 (accepted ~> published -- settlement needs
#        no observer), L1 (armed + publication ~> retired), L2 (waiting +
#        publication ~> woken) and L4 (refused attach ~> published) under
#        the declared WF_ driver/host service conjuncts.
#    36. C++ mutation-seam mirrors (the SLUICE_C1_MUTANT_* defines):
#        MutAttachIgnoresPublication dies on InvArmedRequiresUnpublished;
#        MutAttachTreatsTerminalAsPublished on
#        InvAlreadyTerminalRequiresPublication; MutDeliveryClaimUnbounded
#        on InvAtMostOnceDelivery; MutCancelDuringDeliveryReturns on
#        InvRetireOwnership; MutPublicationSkipsQueue on the temporal
#        properties (armed + published never retires/wakes).
#    37. Model-only floor mutants: MutObsCancelCancelsOp dies on
#        InvCancelIsolation (S1); MutReclaimIgnoresObserver on
#        InvObserverPin (the episode pin); MutNoDeliveryWake on the
#        temporal properties (the routed delivery is never drained).
#    38. Race/scenario coverage: the nine InvCov* witness certificates
#        are negated conjunctions; each MUST be violated, certifying the
#        scenario is reachable -- attach-vs-publication both directions,
#        terminal-window attach, cancel-vs-claim both directions,
#        retirement-after-delivery, observer-free settlement (S4), the
#        delivery pin past binding release (OBS-03), and registration
#        renewal after cancellation.
echo "== ObserverCore: ObserverCore safety =="
run_clean obs-safety ObserverCore.cfg ObserverCore

echo "== ObserverCore: ObserverCore conditional liveness =="
run_live_clean obs-live ObserverCoreLive.cfg ObserverCore

echo "== ObserverCore: ObserverCore C++ mutation-seam mirrors (each must violate its named invariant) =="
run_violate obs-mut-attach-ignores-pub ObserverCoreMutAttachIgnoresPublication.cfg ObserverCore InvArmedRequiresUnpublished
run_violate obs-mut-attach-terminal-as-pub ObserverCoreMutAttachTreatsTerminalAsPublished.cfg ObserverCore InvAlreadyTerminalRequiresPublication
run_violate obs-mut-claim-unbounded ObserverCoreMutDeliveryClaimUnbounded.cfg ObserverCore InvAtMostOnceDelivery
run_violate obs-mut-cancel-during-delivery ObserverCoreMutCancelDuringDeliveryReturns.cfg ObserverCore InvRetireOwnership
run_temporal_violate obs-mut-publication-skips-queue ObserverCoreMutPublicationSkipsQueue.cfg ObserverCore

echo "== ObserverCore: ObserverCore floor mutants (each must violate its named invariant or property) =="
run_violate obs-mut-obs-cancel-cancels-op ObserverCoreMutObsCancelCancelsOp.cfg ObserverCore InvCancelIsolation
run_violate obs-mut-reclaim-ignores-observer ObserverCoreMutReclaimIgnoresObserver.cfg ObserverCore InvObserverPin
run_temporal_violate obs-mut-no-delivery-wake ObserverCoreMutNoDeliveryWake.cfg ObserverCore

echo "== ObserverCore: race/scenario coverage (each witness must be reachable) =="
run_violate obs-cov-attach-rode-pub ObserverCoreCovAttachRodePublication.cfg ObserverCore InvCovAttachRodePublication
run_violate obs-cov-already-terminal ObserverCoreCovAlreadyTerminalFastPath.cfg ObserverCore InvCovAlreadyTerminalFastPath
run_violate obs-cov-window-attach ObserverCoreCovWindowAttach.cfg ObserverCore InvCovWindowAttach
run_violate obs-cov-cancel-lost-to-claim ObserverCoreCovCancelLostToClaim.cfg ObserverCore InvCovCancelLostToClaim
run_violate obs-cov-queued-suppression ObserverCoreCovQueuedSuppression.cfg ObserverCore InvCovQueuedSuppression
run_violate obs-cov-delivery-retired ObserverCoreCovDeliveryRetired.cfg ObserverCore InvCovDeliveryRetired
run_violate obs-cov-no-observer-settlement ObserverCoreCovNoObserverSettlement.cfg ObserverCore InvCovNoObserverSettlement
run_violate obs-cov-pinned-past-release ObserverCoreCovPinnedPastRelease.cfg ObserverCore InvCovPinnedPastRelease
run_violate obs-cov-rearm ObserverCoreCovReArm.cfg ObserverCore InvCovReArm

echo "== ShutdownCore: ShutdownCore safety =="
run_clean shutdown-core-safety ShutdownCore.cfg ShutdownCore

echo "== ShutdownCore: ShutdownCore C++ mutation-seam mirrors (each must violate its named invariant) =="
run_violate shutdown-core-mut-m1-destroy-ignores-closure ShutdownCoreMutM1DestroyIgnoresClosure.cfg ShutdownCore InvDestroyRequiresExecutionClosed
run_violate shutdown-core-mut-m6-snapshot-before-close ShutdownCoreMutM6SnapshotBeforeClose.cfg ShutdownCore InvNoAcceptAfterAdmissionClose
run_violate shutdown-core-mut-m7-close-without-convergence ShutdownCoreMutM7CloseWithoutConvergence.cfg ShutdownCore InvExecutionClosedNoBorrowTouchingExecution
run_violate shutdown-core-mut-m8-retire-fd-early ShutdownCoreMutM8RetireFdEarly.cfg ShutdownCore InvNotificationRetireRequiresNoDeliveryRefs

echo "== ShutdownCore: ShutdownCore contract mutants (a clean run is the kill: the certified state becomes unreachable) =="
run_clean shutdown-core-mut-m4-require-consumption ShutdownCoreMutM4RequireConsumption.cfg ShutdownCore

echo "== ShutdownCore: ShutdownCore reachability certificates (each must be reachable) =="
run_violate shutdown-core-cov-published-survives ShutdownCoreCovPublishedSurvivesExecutionClose.cfg ShutdownCore InvCovPublishedSurvivesExecutionClose
run_violate shutdown-core-cov-unconsumed-converges ShutdownCoreCovUnconsumedResultConverges.cfg ShutdownCore InvCovUnconsumedResultConverges
run_violate shutdown-core-cov-internal-pin ShutdownCoreCovInternalPinThenCleanClose.cfg ShutdownCore InvCovInternalPinThenCleanClose
run_violate shutdown-core-cov-cancel-policy ShutdownCoreCovCancelPolicyCompletes.cfg ShutdownCore InvCovCancelPolicyCompletes

echo "== ShutdownCore: ShutdownCore conditional liveness (the SHUT-03 convergence statement) =="
run_live_clean shutdown-core-live ShutdownCoreLive.cfg ShutdownCore
run_temporal_violate shutdown-core-live-ops-nofair ShutdownCoreLiveOpsNoFair.cfg ShutdownCore
run_temporal_violate shutdown-core-live-close-nofair ShutdownCoreLiveCloseNoFair.cfg ShutdownCore

drain

echo "VERIFY_TLA: PASS"
