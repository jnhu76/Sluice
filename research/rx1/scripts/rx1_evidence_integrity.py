#!/usr/bin/env python3
"""rx1_evidence_integrity — evidence-integrity checks for the RX-1 package.

Three read-only checks over the committed raw artifacts (raw run_*.json are
never modified; scoring state is only read, never written here):

  invariance   Recompute features / C+E predictions / validity from the raw
               artifacts with the CURRENT code and compare against the
               on-disk .scored.json siblings. Proves a decoder repair (e.g.
               the task-clock unit fix) is prediction- and validity-invariant
               and characterizes exactly which feature keys moved.

  sensitivity  SPEC-CONSISTENCY SENSITIVITY — POST-HOC, NOT PRIMARY.
               The frozen protocol's textual rule_precedence says the E APP
               and E CONTROL rules inherit ALL corresponding C external
               predicates ("same external conditions" / "same"); the frozen
               executable classifier omits depth_over_capacity <= 0.10 from
               _rule_app_e and the PSI-quiet predicate from _rule_control_e.
               This check re-evaluates the preserved formal artifacts under
               the LITERAL protocol semantics and reports prediction diffs,
               accuracy / delta, and whether the preregistered verdict would
               change. Primary results remain those of the executable
               classifier frozen at 45a993d.

  manifest     Re-hash every raw artifact listed in
               results/ARTIFACT_MANIFEST.json plus the frozen protocol;
               exit non-zero on any mismatch.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

RX1 = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RX1 / "scripts"))

import rx1  # noqa: E402
import rx1_classify as rc  # noqa: E402

FREEZE_COMMIT = "45a993d"
FREEZE_CLASSIFIER_BLOB = "c1e4fce0b6a95e3893d01d68d41a32691c041c3b"


# ---------------------------------------------------------------------------
# invariance
# ---------------------------------------------------------------------------

def recompute(a: dict):
    """Features + predictions + validity recomputed exactly like cmd_classify."""
    if not a.get("bench_json"):
        return None
    f = rc.extract_features(a)
    if a["observation_mode"] == "OBS-OFF":
        valid = (False, "OBS-OFF tax run (not an attribution sample)")
    else:
        valid = rc.ground_truth_valid(a)
    return f, rc.classify_c(f), rc.classify_e(f), valid


def cmd_invariance(args) -> int:
    failed = False
    for phase in ("pilot", "formal"):
        raws = rx1.load_runs(phase)
        n = pc = pe = pv = 0
        feat_changed: dict[str, int] = {}
        for a in raws:
            scored_path = rx1.RESULTS / phase / f"run_{a['run_index']:04d}.scored.json"
            if not scored_path.exists():
                print(f"  [{phase}] MISSING scored sibling {scored_path.name}")
                failed = True
                continue
            s = json.loads(scored_path.read_text())
            if not a.get("bench_json"):
                continue
            got = recompute(a)
            if got is None:
                continue
            f, c, e, (valid, _reason) = got
            n += 1
            for k in set(f) | set(s.get("features", {})):
                if f.get(k) != s.get("features", {}).get(k):
                    feat_changed[k] = feat_changed.get(k, 0) + 1
            if c != s.get("classifier_C_prediction"):
                pc += 1
                print(f"  [{phase}] C pred diff run {a['run_index']}: "
                      f"{s.get('classifier_C_prediction')} -> {c}")
            if e != s.get("classifier_E_prediction"):
                pe += 1
                print(f"  [{phase}] E pred diff run {a['run_index']}: "
                      f"{s.get('classifier_E_prediction')} -> {e}")
            if valid != s.get("ground_truth_valid"):
                pv += 1
                print(f"  [{phase}] validity diff run {a['run_index']}: "
                      f"{s.get('ground_truth_valid')} -> {valid}")
        status = "OK" if (pc == 0 and pe == 0 and pv == 0) else "FAIL"
        print(f"[{phase}] rechecked {n} scored runs: C-diff={pc} E-diff={pe} "
              f"validity-diff={pv} [{status}]")
        if feat_changed:
            for k, cnt in sorted(feat_changed.items(), key=lambda kv: -kv[1]):
                print(f"  feature moved: {k} on {cnt}/{n} runs "
                      f"(value-level only; predictions unaffected per diffs above)")
        if pc or pe or pv:
            failed = True
    return 1 if failed else 0


# ---------------------------------------------------------------------------
# SPEC-CONSISTENCY SENSITIVITY — POST-HOC, NOT PRIMARY
# ---------------------------------------------------------------------------

def _rule_app_e_literal(f, T):
    # Protocol rule 5 literal: "E = same external conditions AND ..." —
    # "same external conditions" = C's full conjunction, INCLUDING
    # depth_over_capacity <= 0.10 (the frozen executable omits it).
    fired = (f["cpu_cores_used"] <= T["app_cores_used_max"]
             and f["depth_over_capacity"] <= T["app_depth_over_capacity_max"]
             and f["sluice_slot_occ_mean"] <= T["e_app_slot_occ_mean_max"]
             and f["sluice_frac_active_at_configured"] <= T["e_app_frac_active_max"]
             and f["sluice_rejections_delta"] == 0)
    return fired, 1.0


def _rule_control_e_literal(f, T):
    # Protocol rule 6 literal: "E = same AND ..." — "same" = C's full
    # conjunction, INCLUDING the PSI-quiet predicate (the frozen executable
    # omits it). Static note: CPU precedes CONTROL and fires exactly when
    # psi > threshold, so for any run reaching the CONTROL branch the added
    # conjunct is already true — this rule is executable-equivalent there.
    fired = (f["cpu_cores_used"] >= T["control_cores_used_min"]
             and f["psi_cpu_some_us_per_s"] <= T["psi_cpu_some_us_per_s"]
             and f["sluice_frac_active_at_configured"] < T["e_control_frac_active_max"]
             and f["sluice_frac_slot_at_capacity"] < T["frac_slot_at_capacity_sat"]
             and f["sluice_rejections_delta"] == 0)
    return fired, 1.0


def classify_e_literal(f: dict, T: dict | None = None) -> str:
    """rc._classify with ONLY the E APP / E CONTROL predicates replaced by
    their literal-spec forms. Precedence, UNKNOWN policy, the C side, and the
    E capacity / worker / CPU / IO gates are the frozen executable ones."""
    T = T or rc.THRESHOLDS_V1
    cpu = rc._rule_cpu(f, T)
    cap = rc._rule_capacity_e(f, T)
    wrk = rc._rule_worker_e(f, T)
    app = _rule_app_e_literal(f, T)
    ctl = _rule_control_e_literal(f, T)
    io = rc._rule_io(f, T)
    if not any(r[0] for r in (cpu, cap, wrk, app, ctl, io)):
        return "UNKNOWN"
    if cpu[0]:
        return "CPU_CONTENDED"
    if cap[0] and wrk[0]:
        return "UNKNOWN"
    if cap[0]:
        return "REQUEST_CAPACITY_LIMITED"
    if wrk[0]:
        return "THREADPOOL_WORKER_LIMITED"
    if app[0]:
        return "APP_PIPELINE_LIMITED"
    if ctl[0]:
        return "CONTROL"
    if io[0]:
        return "IO_SERVICE_CONTENDED"
    return "UNKNOWN"


def cmd_sensitivity(args) -> int:
    raws = rx1.load_runs("formal")
    attribution = [a for a in raws if not a.get("tax") and a.get("bench_json")]

    rows = []
    scored_mismatch = 0
    for a in attribution:
        got = recompute(a)
        if got is None:
            continue
        f, c, e_exec, (valid, _reason) = got
        s = json.loads((rx1.RESULTS / "formal" / f"run_{a['run_index']:04d}.scored.json").read_text())
        if c != s.get("classifier_C_prediction") or e_exec != s.get("classifier_E_prediction"):
            scored_mismatch += 1
        rows.append(dict(a=a, f=f, c=c, e_exec=e_exec, e_lit=classify_e_literal(f), valid=valid))

    diff_all = [r for r in rows if r["e_exec"] != r["e_lit"]]
    vrows = [r for r in rows if r["valid"]]
    diff_valid = [r for r in vrows if r["e_exec"] != r["e_lit"]]

    pairs_c = [(r["c"], r["a"]["ground_truth_label"]) for r in vrows]
    pairs_lit = [(r["e_lit"], r["a"]["ground_truth_label"]) for r in vrows]
    acc_c = rc.accuracy(pairs_c)
    acc_lit = rc.accuracy(pairs_lit)
    delta, lo, hi = rc.paired_bootstrap_delta(pairs_c, pairs_lit)
    wrong_c = sum(1 for p, t in pairs_c if p != t and p != "UNKNOWN") / len(pairs_c)
    wrong_lit = sum(1 for p, t in pairs_lit if p != t and p != "UNKNOWN") / len(pairs_lit)

    primary = json.loads((rx1.RESULTS / "analysis" / "analysis.json").read_text())
    low_tax = primary["observability_tax"]["aggregate"]["low"]["throughput_tax_pct_median_of_shapes"]
    if (delta * 100 >= 15 and lo > 0 and not (wrong_lit > wrong_c) and low_tax <= 2):
        verdict_lit = "SUPPORTED ENOUGH TO DEEPEN"
    elif delta > 0:
        verdict_lit = "PROMISING BUT INSUFFICIENT"
    elif wrong_lit > wrong_c or delta <= 0:
        verdict_lit = "NOT SUPPORTED / STOP EXPANSION"
    else:
        verdict_lit = "ENGINEERING VALUE ONLY"

    out = {
        "label": "SPEC-CONSISTENCY SENSITIVITY — POST-HOC, NOT PRIMARY",
        "why": ("frozen protocol rule_precedence text says E APP / E CONTROL "
                "inherit ALL corresponding C external predicates; the frozen "
                "executable classifier omits depth_over_capacity<=0.10 "
                "(_rule_app_e) and PSI-quiet (_rule_control_e). Discrepancy "
                "predates formal execution (both texts at freeze commit "
                f"{FREEZE_COMMIT}, classifier git blob {FREEZE_CLASSIFIER_BLOB}); "
                "it is a protocol-to-executable consistency defect, not "
                "post-hoc tuning. Primary results remain the executable "
                "classifier's."),
        "n_attribution_runs": len(rows),
        "e_prediction_diff_all": len(diff_all),
        "e_prediction_diff_all_runs": [
            {"run_index": r["a"]["run_index"], "truth": r["a"]["ground_truth_label"],
             "executable_e": r["e_exec"], "literal_e": r["e_lit"],
             "fired_rules": rc.fired_rules(r["f"])} for r in diff_all],
        "n_valid": len(vrows),
        "e_prediction_diff_valid": len(diff_valid),
        "scored_mismatch_vs_stored": scored_mismatch,
        "accuracy_c_literal_baseline": acc_c,
        "accuracy_e_literal": acc_lit,
        "delta_accuracy_literal": delta,
        "delta_ci95_literal": [lo, hi],
        "wrong_cause_rate_c": wrong_c,
        "wrong_cause_rate_e_literal": wrong_lit,
        "primary_verdict": primary["verdict_gate"],
        "verdict_gate_literal": verdict_lit,
        "verdict_unchanged": verdict_lit == primary["verdict_gate"],
        "static_argument": ("literal E CONTROL is executable-equivalent on "
                            "any run reaching the CONTROL branch (CPU rule "
                            "precedes it and fires exactly when the added "
                            "PSI conjunct would fail); literal E APP is "
                            "strictly tighter, so only executable-APP "
                            "predictions with depth_over_capacity>0.10 can "
                            "move — none exist in this matrix (I1 depth=2, "
                            "capacity=64 gives 0.031)"),
    }
    (rx1.RESULTS / "analysis" / "spec_sensitivity.json").write_text(json.dumps(out, indent=1) + "\n")
    print(json.dumps({k: out[k] for k in (
        "n_attribution_runs", "e_prediction_diff_all", "n_valid",
        "e_prediction_diff_valid", "scored_mismatch_vs_stored",
        "accuracy_c_literal_baseline", "accuracy_e_literal",
        "delta_accuracy_literal", "primary_verdict", "verdict_gate_literal",
        "verdict_unchanged")}, indent=1))
    print(f"rx1: wrote {rx1.RESULTS / 'analysis' / 'spec_sensitivity.json'}")
    return 0 if scored_mismatch == 0 else 1


# ---------------------------------------------------------------------------
# manifest
# ---------------------------------------------------------------------------

def cmd_manifest(args) -> int:
    mpath = rx1.RESULTS / "ARTIFACT_MANIFEST.json"
    m = json.loads(mpath.read_text())
    failed = []
    for e in m["artifacts"]:
        p = rx1.REPO / e["path"]
        if not p.exists():
            failed.append(f"missing {e['path']}")
            continue
        h = hashlib.sha256(p.read_bytes()).hexdigest()
        if h != e["sha256"]:
            failed.append(f"hash mismatch {e['path']}")
    proto = hashlib.sha256(rx1.PROTOCOL_PATH.read_bytes()).hexdigest()
    if proto != m["protocol_sha256"]:
        failed.append("protocol hash mismatch")
    if failed:
        for x in failed:
            print(f"MANIFEST FAIL: {x}")
        return 1
    print(f"manifest OK: {len(m['artifacts'])} raw artifacts + frozen protocol verified")
    return 0


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(prog="rx1_evidence_integrity")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("invariance", help="current-code recompute vs stored .scored.json")
    sub.add_parser("sensitivity", help="literal-spec E rules over formal artifacts (POST-HOC)")
    sub.add_parser("manifest", help="re-hash ARTIFACT_MANIFEST.json")
    args = ap.parse_args()
    if args.cmd == "invariance":
        raise SystemExit(cmd_invariance(args))
    if args.cmd == "sensitivity":
        raise SystemExit(cmd_sensitivity(args))
    if args.cmd == "manifest":
        raise SystemExit(cmd_manifest(args))


if __name__ == "__main__":
    main()
