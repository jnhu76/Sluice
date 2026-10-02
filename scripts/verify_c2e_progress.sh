#!/usr/bin/env bash
# C2-E (Issue #397) TLA+ verification gate for the frozen A-D progress
# protocol, re-based on the #444 production corrective pass (the
# transition-bound close_admission signal): formal/tla/ProgressSource.tla.
#
#   Campaign A -- safety / no-lost-wake:
#     1. ProgressSourceSafety.cfg must complete cleanly under TypeOK,
#        InvSafePark (S2/S8), InvControlGenOrder + InvControlSpentNeverParks
#        (S4), InvNoFalseIdle (S5) and InvNoProgressTerminal (the finite
#        image of the post-#444 structural bound: request-driven bumps
#        <= 3C + 2, repeated close a stuttering step).
#   Campaign A' -- mutants (the checks must bite):
#     2. MutParkDrain  (park handshake drains without L4a/P3 revalidation)
#        must violate InvSafePark.
#     3. MutNoProbe    (physical probe blind to CQ state, M-C1 class) must
#        violate InvSafePark.
#     4. MutNoExhaustion (saturated epoch bumps nothing -- the C2-B round-1
#        sticky-bool defect class) must violate InvSafePark.
#   Campaign D -- deadline (S6):
#     5. ProgressSourceDeadline.cfg (caller deadline paths + S6 ghosts)
#        must complete cleanly including InvDeadlineNoCancel.
#   Campaign H -- external host binding (S9):
#     6. ProgressSourceHost.cfg must complete cleanly under InvTornClean
#        and InvInterestMatchesHost.
#   Campaign B -- exhaustion (V24):
#     7. ExhNear (seam-placed first-stage saturation: the exhaustion
#        sequence carries freshness) must complete cleanly.
#     8. ExhTermControl (spent control domain: permanently pending, no
#        ordinary park) must complete cleanly.
#     9. ExhTermProgress (seam-placed progress absorbing pair {Max,Max})
#        MUST VIOLATE InvSafePark. This is the local-safety boundary
#        certificate: the representable internal state stays locally
#        unsafe, while the corrected production transition system cannot
#        reach it (see ReachClose* below and the C2-E records in
#        docs/roadmap/v1-conformance.md). A clean run here means the
#        boundary certificate no longer discriminates and the record must
#        be re-anchored.
#   Campaign C -- liveness (VERIFY-03 conditional):
#    10. ProgressSourceLiveness.cfg (SpecFair: owner fairness, environment
#        completion, transport dispatch permission) must complete cleanly
#        under all five temporal properties.
#   Certificates (the mandated scenarios are reachable, not vacuous):
#    11-14. CertSticky / CertSat / CertSpent / CertHost must each be
#        violated (house must-be-reachable pattern).
#   Campaign R -- post-#444 reachability re-adjudication (see the C2-E
#   re-closure record in docs/roadmap/v1-conformance.md):
#    15. ReachClose (normal Init, #444 semantics: repeated close = no
#        signal) must complete CLEANLY: normal repeated close cannot pump
#        the progress generation toward the absorbing pair.
#    16. ReachClosePark must complete CLEANLY: the pre-fix terminal-pair
#        park counterexample is no longer stageable under production
#        semantics.
#    17. ReachClosePub must complete CLEANLY: the pre-fix
#        production-staged composition (Submit -> repeated CloseAdmission
#        -> {Max,Max} -> WorkerComplete -> stale drain -> park past
#        publication) has disappeared.
#    18. ReachNoFuel (unbounded plain re-acceptance) must complete
#        CLEANLY: InvNoProgressTerminal does not depend on MaxFuel for
#        the request-driven signal sources; exhaustion-close plus the
#        capacity images bound them.
#    19. MutNoExhClose (unbounded re-acceptance + the pass never closes
#        admission on exhaustion) must VIOLATE InvNoProgressTerminal: the
#        exhaustion-close rule is the load-bearing production bound behind
#        the request-source half of the argument.
#   Campaign M -- the restored-pump discriminator (#444 evidence):
#    20. MutRepeatCloseSignals (normal Init; the mutant restores exactly
#        the removed pre-#444 edge: a close on an already-closed
#        admission signals again) must VIOLATE InvNoProgressTerminal:
#        the absorbing pair becomes reachable through the restored pump.
#    21. MutRepeatCloseSignalsPark (same mutant) must VIOLATE
#        InvSafePark: the same defect class as the ExhTermProgress
#        certificate, staged from normal Init through the restored pump.
#
# TLC retrieval is pinned and checksummed exactly as scripts/verify_tla.sh;
# the jar is never committed.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
tla="$repo/formal/tla"

TLA2TOOLS_VERSION="1.7.4"
TLA2TOOLS_SHA256="936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88"
TLA2TOOLS_URL="https://github.com/tlaplus/tlaplus/releases/download/v${TLA2TOOLS_VERSION}/tla2tools.jar"
jar="$tla/tla2tools.jar"

jar_sha() { sha256sum "$1" | cut -d' ' -f1; }

if [[ -n "${SLUICE_TLA2TOOLS_JAR:-}" ]]; then
    if [[ "$(jar_sha "$SLUICE_TLA2TOOLS_JAR")" != "$TLA2TOOLS_SHA256" ]]; then
        echo "SLUICE_TLA2TOOLS_JAR checksum mismatch: refusing to verify" >&2
        exit 1
    fi
    jar="$SLUICE_TLA2TOOLS_JAR"
    echo "== TLC jar (SLUICE_TLA2TOOLS_JAR override, checksum ok): $jar =="
elif [[ -f "$jar" && "$(jar_sha "$jar")" == "$TLA2TOOLS_SHA256" ]]; then
    echo "== TLC jar (cached, checksum ok): v$TLA2TOOLS_VERSION =="
elif [[ -f "$jar" ]]; then
    echo "formal/tla/tla2tools.jar exists but its SHA-256 does not match pinned" >&2
    exit 1
else
    echo "== TLC jar absent: bootstrapping pinned v$TLA2TOOLS_VERSION =="
    curl -fSL --retry 3 -o "$jar" "$TLA2TOOLS_URL" || {
        rm -f "$jar"
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

run_tlc() {
    java -XX:+UseParallelGC -cp "$jar" tlc2.TLC -deadlock \
        -config "$tla/ProgressSource$1.cfg" -metadir "$work/mc-$1" \
        "$tla/ProgressSource.tla" > "$work/$1.log" 2>&1
}

run_clean() {
    local label="$1"
    echo "== TLC: $label =="
    if ! run_tlc "$label"; then
        cat "$work/$label.log"
        echo "FAIL: $label did not complete cleanly" >&2
        exit 1
    fi
    grep -q "Model checking completed. No error has been found." "$work/$label.log" || {
        cat "$work/$label.log"
        echo "FAIL: $label did not report clean completion" >&2
        exit 1
    }
    grep -E "Model checking completed|states generated" "$work/$label.log"
}

run_violate() {
    local label="$1" inv="$2" note="$3"
    echo "== TLC: $label (must violate $inv)${note:+ -- $note} =="
    if run_tlc "$label"; then
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

run_clean Safety
run_violate MutParkDrain InvSafePark "pure-drain park handshake killed"
run_violate MutNoProbe InvSafePark "probe-blind park killed (M-C1 class)"
run_violate MutNoExhaustion InvSafePark "C2-B round-1 saturation defect class killed"
run_clean Deadline
run_clean Host
run_clean ExhNear
run_clean ExhTermControl
run_violate ExhTermProgress InvSafePark \
    "LOCAL-SAFETY BOUNDARY CERTIFICATE: absorbing progress pair {Max,Max} + L4a drain consumes fd freshness -> park past unadvertised obligation (production cannot reach this state; see ReachClose* and docs/roadmap/v1-conformance.md C2-E)"
run_clean Liveness
run_violate CertSticky CertStickyControl "S4 mandated sticky scenario reachable"
run_violate CertSat CertSatFreshness "V24 first-stage saturation freshness reachable"
run_violate CertSpent CertSpentControlReports "spent control domain still reports control"
run_violate CertHost CertHostLifecycle "host unregister-before-detach ordering reachable"
run_clean ReachClose
run_clean ReachClosePark
run_clean ReachClosePub
run_clean ReachNoFuel
run_violate MutNoExhClose InvNoProgressTerminal \
    "load-bearing bound: without exhaustion-closes-admission, unbounded re-acceptance churn reaches the absorbing pair"
run_violate MutRepeatCloseSignals InvNoProgressTerminal \
    "RESTORED-PUMP DISCRIMINATOR: the pre-#444 per-call close signal alone re-reaches the absorbing pair from normal Init -- #444 removed the actual pump"
run_violate MutRepeatCloseSignalsPark InvSafePark \
    "RESTORED-PUMP DISCRIMINATOR: the no-lost-wake composition staged from normal Init through the restored pump"

echo
echo "C2-E ProgressSource campaign: all expected outcomes reproduced."
