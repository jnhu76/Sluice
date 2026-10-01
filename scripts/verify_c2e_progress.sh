#!/usr/bin/env bash
# C2-E (Issue #397) TLA+ verification gate for the frozen A-D progress
# protocol: formal/tla/ProgressSource.tla.
#
#   Campaign A -- safety / no-lost-wake:
#     1. ProgressSourceSafety.cfg must complete cleanly under TypeOK,
#        InvSafePark (S2/S8), InvControlGenOrder + InvControlSpentNeverParks
#        (S4), InvNoFalseIdle (S5) and InvNoProgressTerminal (the finite
#        image of the production capacity bound).
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
#        MUST VIOLATE InvSafePark. This is the recorded C2-E formal
#        boundary finding: at the fully saturated progress pair a producer
#        signal bumps nothing, the fused L4a drain can consume its fd
#        freshness, and the owner parks past the unadvertised obligation
#        (see the C2-E record in docs/roadmap/v1-conformance.md). The
#        violation is the expected, recorded outcome; a clean run here
#        means the production protocol changed and the record must be
#        re-anchored.
#   Campaign C -- liveness (VERIFY-03 conditional):
#    10. ProgressSourceLiveness.cfg (SpecFair: owner fairness, environment
#        completion, transport dispatch permission) must complete cleanly
#        under all five temporal properties.
#   Certificates (the mandated scenarios are reachable, not vacuous):
#    11-14. CertSticky / CertSat / CertSpent / CertHost must each be
#        violated (house must-be-reachable pattern).
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
    "RECORDED C2-E FORMAL BOUNDARY DEFECT: absorbing progress pair {Max,Max} + L4a drain consumes fd freshness -> park past unadvertised obligation (see docs/roadmap/v1-conformance.md C2-E)"
run_clean Liveness
run_violate CertSticky CertStickyControl "S4 mandated sticky scenario reachable"
run_violate CertSat CertSatFreshness "V24 first-stage saturation freshness reachable"
run_violate CertSpent CertSpentControlReports "spent control domain still reports control"
run_violate CertHost CertHostLifecycle "host unregister-before-detach ordering reachable"

echo
echo "C2-E ProgressSource campaign: all expected outcomes reproduced."
