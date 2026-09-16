#!/usr/bin/env bash
# FORMAL-CAPABILITY-BOUNDARY-1 TLA+ verification gate (Stage 1V2.2, Event).
#
#   1. TLC on the Event primitive model (`formal/tla/EventCore.tla`, the
#      V2.2 execution-domain machine): TypeOK and the issue-anchored
#      no-wait-before-set safety property must hold.
#   2. TLC on the negative mutant (wait completing inline on a clear flag,
#      Lean `eventMutant`): the safety property MUST be violated, proving
#      the check bites.
#
# `-deadlock` is legitimate here: the fuel bounds (MaxFiber, MaxExt,
# MaxHistory) create terminal parked states by construction.
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

echo "== TLC: EventCore (main model) =="
java -cp "$jar" tlc2.TLC -deadlock -config "$tla/EventCore.cfg" \
    -metadir "$work/main" "$tla/EventCore" > "$work/main.log" 2>&1 || {
    cat "$work/main.log"
    echo "FAIL: main model check failed" >&2
    exit 1
}
grep -q "Model checking completed. No error has been found." "$work/main.log" || {
    cat "$work/main.log"
    echo "FAIL: main model did not complete cleanly" >&2
    exit 1
}
grep -E "Model checking completed|states generated" "$work/main.log"

echo "== TLC: EventCore (negative mutant must fail) =="
set +e
java -cp "$jar" tlc2.TLC -deadlock -config "$tla/EventCoreMutant.cfg" \
    -metadir "$work/mut" "$tla/EventCore" > "$work/mut.log" 2>&1
mut=$?
set -e
if [[ "$mut" -eq 0 ]]; then
    cat "$work/mut.log"
    echo "FAIL: mutant passed -- the invariant check does not bite" >&2
    exit 1
fi
grep -q "Invariant NoWaitBeforeSet is violated" "$work/mut.log" || {
    cat "$work/mut.log"
    echo "FAIL: mutant failed for the wrong reason" >&2
    exit 1
}
grep -E "Invariant NoWaitBeforeSet is violated|states generated" "$work/mut.log"

echo "VERIFY_TLA: PASS"
