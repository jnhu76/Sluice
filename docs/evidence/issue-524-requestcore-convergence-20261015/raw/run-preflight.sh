#!/usr/bin/env bash
# #524 R5 fast preflight: every RequestCore *violate* gate on the converged
# model, with the verify_tla.sh verdict semantics (exact named kill, never a
# bare exit code).  The full-space clean runs (reference safety, reference
# liveness, and the two subsumption witnesses) are deliberately NOT here:
# they are the R6 complete runs.
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
tla="$root/formal/tla"
jar="$tla/tla2tools.jar"
work="${1:?usage: run-preflight.sh <workdir>}"
mkdir -p "$work"

queue="$work/queue.tsv"
: > "$queue"
enqueue() { printf '%s\t%s\t%s\n' "$1" "$2" "$3" >> "$queue"; }

# label, cfg, expected (invariant-name regex | "TEMPORAL")
enqueue rcore-mut-ignore-close              RequestCoreMutIgnoreClose.cfg              InvNoAcceptAfterClose
enqueue rcore-mut-ignore-health             RequestCoreMutIgnoreHealth.cfg             InvNoAcceptAfterHealth
enqueue rcore-mut-rollback-residue          RequestCoreMutRollbackResidue.cfg          InvUnoccupiedClean
enqueue rcore-mut-terminal-overwrite        RequestCoreMutTerminalOverwrite.cfg        InvSingleWinner
enqueue rcore-mut-cancel-as-physical        RequestCoreMutCancelAsPhysical.cfg         InvCancelWinRequiresUnclaimed
enqueue rcore-mut-publish-while-e           RequestCoreMutPublishWhileE.cfg            InvNoPubWhileExec
enqueue rcore-mut-gen-wrap                  RequestCoreMutGenWrap.cfg                  InvAcceptedRepresented
enqueue rcore-mut-reclaim-ignores-ctl       RequestCoreMutReclaimIgnoresCtl.cfg        InvReclaimConditions
enqueue rcore-mut-retire-last-exec          RequestCoreMutRetireLastExec.cfg           InvExecHeldUntilTerminal
enqueue rcore-mut-exec-after-terminal       RequestCoreMutExecAfterTerminal.cfg        "InvNoExecRevival|InvNoPubWhileExec"
enqueue rcore-mut-stale-event               RequestCoreMutStaleEvent.cfg               InvTerminalMatchesGeneration
enqueue rcore-mut-double-decrement          RequestCoreMutDoubleDecrement.cfg          TypeOK
enqueue rcore-mut-consume-keeps-bind        RequestCoreMutConsumeKeepsBind.cfg         InvReleasedNotLive
enqueue rcore-mut-consume-unpublished       RequestCoreMutConsumeUnpublished.cfg       InvConsumeAfterPublish
enqueue rcore-mut-discard-keeps-bind        RequestCoreMutDiscardKeepsBind.cfg         InvReleasedNotLive
enqueue rcore-mut-discard-inflight          RequestCoreMutDiscardInflight.cfg          InvDiscardAfterPublish
enqueue rcore-mut-strand-post-accept        RequestCoreMutStrandPostAccept.cfg         TEMPORAL
enqueue rcore-mut-lazy-reclaim              RequestCoreMutLazyReclaim.cfg              TEMPORAL
enqueue rcore-mut-ctl-after-settled         RequestCoreMutCtlAfterSettled.cfg          TEMPORAL

one() {
    local label="$1" cfg="$2" expect="$3"
    local dir="$work/$label"
    mkdir -p "$dir"
    java -Xmx4g -cp "$jar" tlc2.TLC -workers 4 -deadlock \
        -config "$tla/$cfg" -metadir "$dir/mc" "$tla/RequestCore.tla" \
        > "$dir/$label.log" 2>&1
    local rc=$?
    local verdict
    if [[ "$rc" -eq 0 ]]; then
        verdict="FAIL passed-without-violation"
    elif [[ "$expect" == "TEMPORAL" ]]; then
        if grep -q "Temporal properties were violated" "$dir/$label.log"; then
            verdict="PASS temporal-violation"
        else
            verdict="FAIL wrong-reason"
        fi
    else
        if grep -qE "Invariant ($expect) is violated" "$dir/$label.log"; then
            verdict="PASS $(grep -oE "Invariant ($expect) is violated" "$dir/$label.log" | head -1)"
        else
            verdict="FAIL wrong-reason"
        fi
    fi
    printf '%s\t%s\n' "$label" "$verdict" >> "$work/results.tsv"
}
export -f one
export root tla jar work

printf 'label\texpected\tverdict\n' > "$work/results.tsv"
ls "$work" >/dev/null
while IFS=$'\t' read -r label cfg expect; do
    one "$label" "$cfg" "$expect" &
    while [[ "$(jobs -rp | wc -l)" -ge 4 ]]; do wait -n || true; done
done < "$queue"
wait

# re-check every kill against the script's semantics and emit the summary
python3 - "$work" <<'PY'
import sys, re
work = sys.argv[1]
rows = [l.split("\t") for l in open(f"{work}/results.tsv").read().splitlines()[1:] if l]
bad = [r for r in rows if not r[1].startswith("PASS")]
print(f"preflight gates: {len(rows)} run, {len(rows)-len(bad)} PASS, {len(bad)} FAIL")
for label, verdict in rows:
    log = f"{work}/{label}/{label}.log"
    tail = [l for l in open(log).read().splitlines() if "states generated" in l or "Model checking completed" in l]
    print(f"  {label:38s} {verdict:42s} {tail[-1].strip() if tail else ''}")
if bad:
    print("FAILED GATES:", [r[0] for r in bad]); sys.exit(1)
PY
