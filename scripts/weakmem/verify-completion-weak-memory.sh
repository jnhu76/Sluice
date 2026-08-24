#!/usr/bin/env bash
# verify-completion-weak-memory.sh — #197 bounded weak-memory evidence gate.
#
# Runs the Completion publication/reset kernels (spec/weakmem/completion-
# publication/) under GenMC (RC11 / SC / TSO / RA+RLX), plus generated
# broken-order mutants that MUST FAIL, and writes a machine-readable artifact.
#
#   kernels  K1 publication, K2 reset/reuse   -> PASS on every model
#   controls N1/N1b/N3 (K1), N2/N4 (K2)       -> FAIL on rc11+ra (gated)
#                                              sc/tso results are recorded
#                                              but NOT gated. Empirically sc
#                                              repairs every control (no
#                                              reordering at all), while tso
#                                              ALSO rejects them: under the
#                                              C++ model semantics GenMC
#                                              implements, a relaxed
#                                              publication leaves the
#                                              non-atomic payload accesses
#                                              racy even on a TSO-shaped
#                                              machine (store buffers do not
#                                              license the race).
#   N4 note: pre-registered in the audit as a PREDICTED-redundant weakening;
#   the checker DISPROVED the prediction (an acquire RMW reading a relaxed
#   store gives no synchronizes-with edge), so it was upgraded to a control.
#   RA1 (A1 begin-binding CAS -> relaxed)      -> PASSES rc11: documented
#                                              redundancy observation
#                                              (acquire reads the initial
#                                              write; release subsumed by the
#                                              later same-thread releases);
#                                              informational, never a control
#
# This is a SEPARATELY-RUN evidence layer (#197 non-goal: no CI wiring until
# the pilot demonstrates value). Reproduce with:
#
#   GENMC_BIN=<path-to-genmc> scripts/weakmem/verify-completion-weak-memory.sh
#   scripts/weakmem/verify-completion-weak-memory.sh --self-test
#
# Fail-closed: exits non-zero on the first hard failure (missing checker,
# compile error, unexpected kernel failure, control that stops failing,
# generator pattern drift), naming the failing leg and the reproduction.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
specdir="$repo/spec/weakmem/completion-publication"
gen="$here/_gen_mutants.py"

MODELS=(rc11 sc tso ra)
FAIL_MODELS=(rc11 ra)     # controls must FAIL here (gated)
PASS_MODELS=(sc tso)      # recorded, not gated (sc repairs; tso rejects too)
RUN_TIMEOUT="${GENMC_RUN_TIMEOUT:-600}"

# ---------------------------------------------------------------------------
# Checker resolution: explicit GENMC_BIN (authoritative — an invalid explicit
# path is a hard error, never silently substituted) > PATH > user-local build.
resolve_genmc() {
  local c="${GENMC_BIN:-}"
  if [[ -n "$c" ]]; then
    if [[ ! -x "$c" ]]; then
      echo "error: GENMC_BIN is set but not executable: $c" >&2
      return 1
    fi
    GENMC="$c"; return 0
  fi
  c="$(command -v genmc 2>/dev/null || true)"
  if [[ -z "$c" || ! -x "$c" ]] && [[ -x "$HOME/tools/genmc-src/build/bin/genmc" ]]; then
    c="$HOME/tools/genmc-src/build/bin/genmc"
  fi
  if [[ -z "$c" || ! -x "$c" ]]; then
    echo "error: genmc not found (set GENMC_BIN, or install on PATH, or" \
         "build at ~/tools/genmc-src — see docs/verification/weak-memory/" \
         "completion-publication-kernel.md)" >&2
    return 1
  fi
  GENMC="$c"
}

# Runs one file under one model. Sets OUT_ / VERDICT_ (PASS|REJECTED|BAD) /
# EXEC_ (complete executions explored, or empty). Classification is by OUTPUT
# CONTENT ONLY: genmc exits non-zero when it finds an error, which is a valid
# REJECTED verdict, not a runner failure. Only a timeout or unclassifiable
# output is a hard failure.
run_one() {
  local file="$1" model="$2" out status
  out="$(mktemp)"
  status=0
  timeout "$RUN_TIMEOUT" "$GENMC" "--$model" "$file" >"$out" 2>&1 || status=$?
  OUT_="$out"
  EXEC_="$(grep -o 'Number of complete executions explored: [0-9]*' "$out" \
           | grep -o '[0-9]*$' || true)"
  if grep -q 'No errors were detected' "$out"; then
    VERDICT_=PASS; return 0
  fi
  if grep -q 'Verification unsuccesful' "$out" \
     || grep -q '^ERROR:' "$out"; then
    VERDICT_=REJECTED; return 0
  fi
  echo "error: unclassifiable genmc output (exit=$status) for" \
       "$(basename "$file") [$model]" >&2
  tail -15 "$out" >&2
  rm -f "$out"; VERDICT_=BAD; EXEC_=""; return 1
}

# Gate one (file, model, expectation). Expectation pass -> PASS required;
# expectation fail -> REJECTED required.
gate_one() {
  local file="$1" model="$2" expect="$3" label="$4"
  if ! run_one "$file" "$model"; then
    echo "FAIL  $label (genmc did not run cleanly) [$model]"; return 1
  fi
  if [[ "$expect" == pass && "$VERDICT_" == PASS ]]; then
    echo "PASS  $label [$model] (${EXEC_:-?} executions)"
    rm -f "$OUT_"; return 0
  fi
  if [[ "$expect" == fail && "$VERDICT_" == REJECTED ]]; then
    echo "REJ   $label [$model] (checker rejected the mutant)"
    rm -f "$OUT_"; return 0
  fi
  echo "FAIL  $label [$model]: expected $expect, got $VERDICT_" \
       "(reproduce: $GENMC --$model $file)"
  rm -f "$OUT_"; return 1
}

gen_mutant() {  # kernel-file mutation-name -> stdout source
  python3 "$gen" "$1" "$2"
}

# ---------------------------------------------------------------------------
# Hidden modes used by --self-test and the artifact writer helper.
case "${1:-}" in
  --resolve-only)
    resolve_genmc && echo "$GENMC" && exit 0
    echo "FAIL  self-test leg: missing checker must fail resolution" >&2; exit 1
    ;;
  --gate-one)
    # usage: --gate-one <file> <model> <pass|fail> <label>
    resolve_genmc
    gate_one "$2" "$3" "$4" "$5" && exit 0
    echo "=== gate-one reported the mismatch above (expected for this leg) ===" >&2
    exit 1
    ;;
esac

# ---------------------------------------------------------------------------
self_test() {
  local rc=0 work
  work="$(mktemp -d -t wm-st.XXXXXX)"
  trap 'rm -rf -- "$work"' RETURN

  echo "=== weak-memory runner self-test ==="
  # Leg 1: a nonexistent checker must fail resolution (fail-closed, not a skip).
  if ( GENMC_BIN=/nonexistent-genmc bash "$0" --resolve-only ) >/dev/null 2>&1; then
    echo "FAIL  leg1: resolution succeeded despite GENMC_BIN=/nonexistent-genmc"; rc=1
  else
    echo "PASS  leg1: missing checker fails closed"
  fi

  # Leg 2: generator pattern drift must fail (mutated kernel copy in a temp
  # dir named after the kernel so the name guard does not fire first).
  mkdir -p "$work/drift"
  sed 's/st\.store(St::ready, std::memory_order_release);/st.store(St::ready, std::memory_order_seq_cst);/' \
    "$specdir/kernel_publication.cpp" > "$work/drift/kernel_publication.cpp"
  if python3 "$gen" "$work/drift/kernel_publication.cpp" \
       N1_pub_ready_store_relaxed >/dev/null 2>&1; then
    echo "FAIL  leg2: generator accepted a drifted kernel"; rc=1
  else
    echo "PASS  leg2: generator pattern drift fails closed"
  fi

  resolve_genmc

  # Leg 3: the kernel gate must flag an input that the checker rejects when
  # a PASS is expected (a control mutant masquerading as a clean kernel).
  gen_mutant "$specdir/kernel_publication.cpp" N1_pub_ready_store_relaxed \
    > "$work/n1.cpp"
  if bash "$0" --gate-one "$work/n1.cpp" rc11 pass "leg3-sabotage" >/dev/null 2>&1; then
    echo "FAIL  leg3: kernel gate accepted a failing input as PASS"; rc=1
  else
    echo "PASS  leg3: unexpectedly-failing kernel is flagged"
  fi

  # Leg 4: the mutant gate must flag a mutant that does NOT fail when a FAIL
  # is expected (RA1 is the documented redundancy weakening: it passes rc11).
  gen_mutant "$specdir/kernel_publication.cpp" RA1_begin_binding_cas_relaxed \
    > "$work/ra1.cpp"
  if bash "$0" --gate-one "$work/ra1.cpp" rc11 fail "leg4-redundancy" >/dev/null 2>&1; then
    echo "FAIL  leg4: mutant gate accepted a non-failing mutant as control"; rc=1
  else
    echo "PASS  leg4: non-failing mutant is flagged"
  fi

  if [[ "$rc" -eq 0 ]]; then echo "=== self-test PASS ==="; else echo "=== self-test FAIL ==="; fi
  return "$rc"
}

[[ "${1:-}" == "--self-test" ]] && { self_test; exit $?; }

# ---------------------------------------------------------------------------
# Main evidence run.
artifact_out="${2:-}"
[[ "${1:-}" == "--artifact" ]] || { echo "usage: $0 [--artifact <json>] | --self-test" >&2; exit 2; }

resolve_genmc
k1="$specdir/kernel_publication.cpp"
k2="$specdir/kernel_reset_reuse.cpp"
for f in "$k1" "$k2" "$gen"; do
  [[ -f "$f" ]] || { echo "error: missing $f" >&2; exit 1; }
done

work="$(mktemp -d -t wm-ev.XXXXXX)"
cleanup() { [[ -n "$work" && -d "$work" ]] && rm -rf -- "$work"; }
trap cleanup EXIT
rows="$work/rows.tsv"; : >"$rows"
rc=0

echo "=== #197 Completion publication weak-memory evidence ==="
echo "checker: $("$GENMC" --version 2>/dev/null | sed -n '2,3p' | tr '\n' ' ')"

declare -A KERNEL_FILE=( [K1]="$k1" [K2]="$k2" )
declare -A KERNEL_ROUNDS=( [K1]=1 [K2]=2 )
for k in K1 K2; do
  f="${KERNEL_FILE[$k]}"
  threads="$(grep -c 'pthread_create(' "$f")"
  budget="$(grep -o 'WAIT_BUDGET = [0-9]*' "$f" | grep -o '[0-9]*$' || echo '')"
  prov="$(grep -o '@ [0-9a-f]\{40\}' "$f" | head -1 | cut -d' ' -f2 || true)"
  for m in "${MODELS[@]}"; do
    if gate_one "$f" "$m" pass "$k clean kernel"; then
      printf 'kernel\t%s\t%s\t%s\t%s\n' "$k" "$m" "$VERDICT_" "${EXEC_:-}" >>"$rows"
    else
      printf 'kernel\t%s\t%s\t%s\t\n' "$k" "$m" FAIL >>"$rows"; rc=1
    fi
  done
  printf 'meta\t%s\tthreads\t%s\n' "$k" "$threads" >>"$rows"
  printf 'meta\t%s\tbudget\t%s\n' "$k" "${budget:-none}" >>"$rows"
  printf 'meta\t%s\trounds\t%s\n' "$k" "${KERNEL_ROUNDS[$k]}" >>"$rows"
  printf 'meta\t%s\tprovenance\t%s\n' "$k" "${prov:-MISSING}" >>"$rows"
done

controls=(N1_pub_ready_store_relaxed N1b_observer_load_relaxed \
          N3_commit_cas_relaxed N2_late_observer_load_relaxed \
          N4_reset_idle_store_relaxed)
declare -A MUT_BASE=( [N1_pub_ready_store_relaxed]=K1 \
                      [N1b_observer_load_relaxed]=K1 \
                      [N3_commit_cas_relaxed]=K1 \
                      [N2_late_observer_load_relaxed]=K2 \
                      [N4_reset_idle_store_relaxed]=K2 )
for name in "${controls[@]}"; do
  base="${MUT_BASE[$name]}"
  mfile="$work/$name.cpp"
  gen_mutant "${KERNEL_FILE[$base]}" "$name" >"$mfile" \
    || { echo "FAIL  control $name: generator refused (pattern drift)"; rc=1; continue; }
  for m in "${FAIL_MODELS[@]}"; do
    if gate_one "$mfile" "$m" fail "control $name"; then
      printf 'control\t%s\t%s\tREJECTED\t%s\n' "$name" "$m" "${EXEC_:-}" >>"$rows"
    else
      printf 'control\t%s\t%s\tNOT-REJECTED\t\n' "$name" "$m" >>"$rows"; rc=1
    fi
  done
  for m in "${PASS_MODELS[@]}"; do
    # Stronger models are recorded, not gated: empirically sc repairs every
    # control and tso rejects them too (C++ race semantics, see header note).
    if run_one "$mfile" "$m"; then
      printf 'control\t%s\t%s\t%s\t%s\n' "$name" "$m" "$VERDICT_" "${EXEC_:-}" >>"$rows"
      rm -f "$OUT_"
    else
      printf 'control\t%s\t%s\tRUN-ERROR\t\n' "$name" "$m" >>"$rows"; rc=1
    fi
  done
done

# Redundancy observation RA1: informational, expected PASS on rc11.
ra1file="$work/RA1.cpp"
if gen_mutant "$k1" RA1_begin_binding_cas_relaxed >"$ra1file"; then
  if run_one "$ra1file" rc11; then
    printf 'redundancy\tRA1_begin_binding_cas_relaxed\trc11\t%s\t%s\n' \
      "$VERDICT_" "${EXEC_:-}" >>"$rows"
    rm -f "$OUT_"
  else
    printf 'redundancy\tRA1_begin_binding_cas_relaxed\trc11\tRUN-ERROR\t\n' >>"$rows"; rc=1
  fi
else
  printf 'redundancy\tRA1_begin_binding_cas_relaxed\trc11\tGEN-ERROR\t\n' >>"$rows"; rc=1
fi

echo
if [[ "$rc" -eq 0 ]]; then
  echo "=== PASS — MEMORY-MODEL-CHECKED (BOUNDED KERNEL) ==="
else
  echo "=== FAIL (see the flagged leg above) ==="
fi

if [[ -n "$artifact_out" ]]; then
  GENMC_PATH="$GENMC" REPO_REV="$(git -C "$repo" rev-parse HEAD)" \
  UNAME_STR="$(uname -sr)" DATE_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  python3 - "$rows" "$artifact_out" <<'PYEOF'
import json, os, sys

rows_path, out_path = sys.argv[1], sys.argv[2]
kernels, controls, redundancy, meta = {}, {}, {}, {}
for line in open(rows_path):
    parts = line.rstrip("\n").split("\t")
    kind = parts[0]
    if kind == "kernel":
        _, k, m, verdict, ex = parts
        kernels.setdefault(k, {"results": {}})["results"][m] = {
            "verdict": verdict, "executions": int(ex) if ex else None}
    elif kind == "control":
        _, name, m, verdict, ex = parts
        controls.setdefault(name, {"results": {}})["results"][m] = {
            "verdict": verdict, "executions": int(ex) if ex else None}
    elif kind == "redundancy":
        _, name, m, verdict, ex = parts
        redundancy[name] = {"model": m, "verdict": verdict,
                            "executions": int(ex) if ex else None}
    elif kind == "meta":
        _, k, field, value = parts
        meta.setdefault(k, {})[field] = value

ver = os.popen(f'"{os.environ["GENMC_PATH"]}" --version').read().splitlines()
version = commit = llvm = ""
for l in ver:
    if "GenMC v" in l:
        version = l.split("GenMC ")[1].split(" ")[0]
        commit = l.split("commit #")[1].rstrip(")") if "commit #" in l else ""
    if "Built with LLVM" in l:
        llvm = l.split("Built with LLVM ")[1].split(" ")[0]

doc = {
    "schema": "sluice-weakmem-completion-publication/1",
    "generated_utc": os.environ["DATE_UTC"],
    "repo_revision": os.environ["REPO_REV"],
    "claim": "MEMORY-MODEL-CHECKED (BOUNDED KERNEL)",
    "tool": {"name": "genmc", "version": version, "commit": commit,
             "llvm": llvm, "binary": os.environ["GENMC_PATH"]},
    "host": {"uname": os.environ["UNAME_STR"]},
    "models": ["rc11", "sc", "tso", "ra"],
    "kernels": [
        {"name": k,
         "file": "spec/weakmem/completion-publication/"
                 + ("kernel_publication.cpp" if k == "K1"
                    else "kernel_reset_reuse.cpp"),
         "provenance_revision": meta[k].get("provenance"),
         "threads": int(meta[k]["threads"]),
         "publication_rounds": int(meta[k]["rounds"]),
         "wait_budget": (int(meta[k]["budget"])
                         if meta[k].get("budget", "none") != "none" else None),
         "results": kernels[k]["results"]}
        for k in ("K1", "K2")],
    "controls": [
        {"name": n,
         "base_kernel": "K1" if n.startswith(("N1", "N3", "RA")) else "K2",
         "gated_must_fail_models": ["rc11", "ra"],
         "recorded_ungated_models": ["sc", "tso"],
         "results": controls[n]["results"]}
        for n in ("N1_pub_ready_store_relaxed", "N1b_observer_load_relaxed",
                  "N3_commit_cas_relaxed", "N2_late_observer_load_relaxed",
                  "N4_reset_idle_store_relaxed")
        if n in controls],
    "redundancy_observations": [redundancy],
    "limitations": [
        "bounded kernel: 2-3 threads, 1-2 publication rounds, wait budget 3",
        "assertion class + RC11 data-race detection only; no liveness claim",
        "not whole-program verification of Sluice",
        "round-2 reap gating uses bounded waits as a stand-in for the arena "
        "backend-ready linkage (documented in the kernel header)",
        "sc/tso control results are recorded ungated: sc repairs every "
        "control; tso rejects them too (a relaxed publication leaves the "
        "non-atomic payload accesses racy under C++ model semantics even "
        "on a TSO-shaped machine)",
    ],
}
with open(out_path, "w") as f:
    json.dump(doc, f, indent=2, ensure_ascii=False)
    f.write("\n")
print(f"artifact written: {out_path}")
PYEOF
fi

exit "$rc"
