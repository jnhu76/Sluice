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
