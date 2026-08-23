#!/usr/bin/env bash
# verify-e9-trace-conformance.sh — #196 V2 trace-conformance formal gate.
#
# Validates the checked-in semantic trace corpus
# (spec/tla/e9_park_wake/traces/) against the PRISTINE same-revision
# as-built model (spec/tla/e9_park_wake/E9ParkWake.tla) via
# scripts/formal/e9_trace_validate.py, which generates an isolated TLC
# replay wrapper per trace (existential realizability — the repository
# model is the authority; no second protocol implementation).
#
#   t1/t2/t3/t4a/t4b/t5 fixtures   -> ACCEPT (real C++ trace shapes; the
#                                     C++ side is tests/e9_trace_conformance_
#                                     test.cpp, whose per-case shape
#                                     assertions are the freshness link)
#   neg_a (causeless return)       -> REJECT by model semantics (LeavePark
#                                     has no enabled disjunct under
#                                     SplitWait=TRUE)
#   neg_b (pre-#185-style unconditional-escape claim on an UN-ARMED
#                                 reference park) -> REJECT by model
#                                     semantics (the faithful escape requires
#                                     observationArmed; #185 was MODEL drift,
#                                     never a C++ defect)
#   malformed_*                    -> REJECT fail-closed (schema / revision)
#   validator --self-test          -> PASS (fail-closed legs + one ACCEPT and
#                                     one REJECT TLC leg: the verdict itself
#                                     is non-vacuous)
#
# --fresh: additionally rebuild + rerun the C++ corpus test bound to the
# CURRENT HEAD and validate every emitted trace (the strongest same-revision
# evidence; requires xmake + the debug config). NOT run by verify.py (the
# formal workflow does not build C++); run it in the CI test phase or by
# hand.
#
# Source-safe: TLC runs in isolated mktemp workspaces.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
traces="$repo/spec/tla/e9_park_wake/traces"

# Resolve jar via shared helper (sets TLA2TOOLS_JAR; checksum-verified).
source "$here/resolve-jar.sh"
export TLA2TOOLS_JAR

if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2; exit 2
fi

rc=0
echo "=== E9 trace-conformance gate (#196) ==="

if python3 "$here/e9_trace_validate.py" --self-test; then
  echo "PASS  validator self-test (fail-closed legs + TLC accept/reject legs)"
else
  echo "FAIL  validator self-test"
  rc=1
fi

for f in "$traces"/*.json; do
  name="$(basename "$f" .json)"
  case "$name" in
    neg_*|malformed_*) exp="reject" ;;
    *)                 exp="accept" ;;
  esac
  if python3 "$here/e9_trace_validate.py" --trace "$f" --expect "$exp" \
      >/dev/null 2>&1; then
    echo "PASS  $name ($exp)"
  else
    echo "FAIL  $name (expected $exp)"
    python3 "$here/e9_trace_validate.py" --trace "$f" --expect "$exp" || true
    rc=1
  fi
done

if [[ "${1:-}" == "--fresh" ]]; then
  echo "--- fresh C++ corpus run (bound to HEAD) ---"
  head="$(git -C "$repo" rev-parse HEAD)"
  out="$(mktemp -d -t e9-fresh.XXXXXX)"
  if xmake build e9_trace_conformance_test >/dev/null 2>&1 \
     && SLUICE_E9_TRACE_OUT="$out" SLUICE_E9_TRACE_REVISION="$head" \
        xmake run e9_trace_conformance_test >/dev/null 2>&1; then
    fresh_rc=0
    for f in "$out"/*.json; do
      if python3 "$here/e9_trace_validate.py" --trace "$f" --expect accept \
          >/dev/null 2>&1; then
        echo "PASS  fresh $(basename "$f" .json) (accept, rev=$head)"
      else
        echo "FAIL  fresh $(basename "$f" .json)"
        python3 "$here/e9_trace_validate.py" --trace "$f" --expect accept \
            || true
        fresh_rc=1
      fi
    done
    rm -rf -- "$out"
    rc=$((rc | fresh_rc))
  else
    echo "FAIL  fresh corpus run (build/run failed)"
    rm -rf -- "$out"
    rc=1
  fi
fi

echo
if [[ "$rc" -eq 0 ]]; then
  echo "=== PASS ==="
  echo "claim: TRACE-CONFORMANT (TESTED EXECUTIONS) — corpus only;"
  echo "       not implementation verification, not all executions."
else
  echo "=== FAIL ==="
fi
exit "$rc"
