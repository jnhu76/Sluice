#!/usr/bin/env python3
"""Admission-driven offline verification of the frozen r5-repair numbers.

Repairs review finding R6 (evidence validation). Differences vs the v1
checker (withdrawn, see CORRECTIONS.md R6):

- Admission is driven by the official per-invocation records
  (raw/<cell>/main/results.jsonl): validity, seg, block, position, arm,
  attempt, binary identity, arm readback, measured/warmup record counts.
  Per-rep jsonl files are associated through each record's `jsonl` path and
  re-parsed with the frozen case_id filter — never globbed.
- Per-invocation metrics are recomputed with the frozen harness definition
  (measure3.py: cpu_ns_per_op = 1e9*sum(cpu_s)/sum(ops) over the 5 measured
  reps; p50/p95/p99 = mean of each rep's percentile; never pooled
  per-request percentiles), then aggregated as analyze_round.py does
  (mean of per-block invocation values, blocks kept aligned).
- Duplicate valid invocations for one (block, position, arm) slot are a
  failure (the frozen protocol admits exactly one; analyze_round's load_seg
  would silently overwrite them).
- Precision claims state exactly what was compared: recomputed arm means vs
  the frozen analysis values in analysis/round-analysis.json (full float
  precision) and vs the displayed tables (display rounding). Bootstrap CIs,
  Holm p-values and gate thresholds are NOT recomputed.
- F5 checks assert the exact recovered position sets (d16/cw0 = 0..15,
  d16/cw8 = 0..7, d4/cw0 = 0..3), per-bucket denominators, integer tail
  counts reconciled with the stored percentages, and the validation totals
  (24 instrument-on R tags x 20000 rows = 480000), then re-run the frozen
  recovery script offline against raw/diag-ops.csv and require the derived
  summary to be reproduced exactly.
- Second-round repairs: the frozen comparison derives its expected
  (cell, arm, metric) set from the verified cells and fails if any expected
  comparison was not performed (an empty round-analysis.json no longer
  passes); F5 bucket denominators must close against the recovery totals
  under the recovery script's inclusion/exclusion rules (positioned = rows -
  unpositioned; written buckets n >= 20; per-case residual bounded by the
  sub-MIN_N drop), so a summary with clean validation counters but
  non-closing position buckets is rejected.

Exit 0 = every check passes; each failure prints a receipt line and the
exit code is nonzero.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict

DEFAULT_ROOT = "/home/jnhu/sluice-595-e5/r5-repair-measure"

CELL_DIRS = {
    "MAIN-W03-D16": ("main-w03-d16", "W03-uring-4k-d16", "w03"),
    "PROT-W03-D4": ("prot-w03-d4", "W03-uring-4k-d4", "w03"),
    "PROT-W03-D2SERIAL": ("prot-w03-d2serial", "W03-uring-4k-d2-r3d2serial", "w03"),
    "PROT-W02-D1": ("prot-w02-d1", "W02-uring-4k-d1", "w02"),
    "PROT-W02-D2": ("prot-w02-d2", "W02-uring-4k-d2", "w02"),
}
ARMS = ("A0", "R", "C")
# Frozen binary identities. These are read from measure3.py's BINS table at
# run time (single source, no hand transcription); when the timed binaries
# still exist under the root they are additionally re-hashed and must match.
BIN_PATHS = {
    "w03_original": "worktree-original/build/linux/x86_64/release/bench_w03_external_loop",
    "w02_original": "worktree-original/build/linux/x86_64/release/bench_w02_pipeline",
    "w03_cleanup": "worktree-cleanup/build/linux/x86_64/release/bench_w03_external_loop",
    "w02_cleanup": "worktree-cleanup/build/linux/x86_64/release/bench_w02_pipeline",
}


def frozen_bins(root):
    """Evaluate measure3.py's BINS assignment (dict of (os.path.join, sha))."""
    import ast
    src = os.path.join(root, "harness", "measure3.py")
    text = open(src).read()
    tree = ast.parse(text)
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == "BINS" for t in node.targets):
            seg = ast.get_source_segment(text, node.value)
            return eval(seg, {"__builtins__": {}}, {"os": os, "ROOT": root})
    raise RuntimeError(f"BINS not found in {src}")
MEASURED_REPS = 5
WARMUP_REPS = 1
OPS_PER_REP = 20000
BLOCKS = 24

F5_EXPECT_SETS = {
    "W03-uring-4k-d16|cw0|": set(range(16)),
    "W03-uring-4k-d16|cw8|": set(range(8)),
    "W03-uring-4k-d4|cw0|": set(range(4)),
}
# recover_f5_position.py writes a bucket only when its sample count reaches
# MIN_N; the verifier re-asserts this on written buckets and uses it to bound
# the rows a summary may legitimately leave out of the position buckets.
F5_MIN_N = 20


class Report:
    def __init__(self):
        self.fails = 0
        self.checks = 0
        self.lines = []

    def check(self, ok, label, detail=""):
        self.checks += 1
        tag = "PASS" if ok else "FAIL"
        if not ok:
            self.fails += 1
        line = f"{tag} {label}" + (f" :: {detail}" if detail else "")
        self.lines.append(line)
        print(line)
        return ok

    @property
    def ok(self):
        return self.fails == 0


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def load_records(root, cell):
    cdir, case, _kind = CELL_DIRS[cell]
    path = os.path.join(root, "raw", cdir, "main", "results.jsonl")
    if not os.path.exists(path):
        return None, path, f"missing {path}"
    recs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                recs.append(json.loads(line))
    return recs, path, None


def admit_invocations(recs, cell, rep, bins, root):
    """Frozen admission: one valid invocation per (block, position, arm)."""
    cdir, case, kind = CELL_DIRS[cell]
    slots = defaultdict(list)
    for r in recs:
        slots[(r.get("block"), r.get("position"), r.get("arm"))].append(r)
    admitted = {}
    problems = []
    for slot, group in sorted(slots.items(), key=lambda kv: str(kv[0])):
        valid = [r for r in group if r.get("valid") is True and r.get("seg") == "main"]
        if not valid:
            problems.append(f"slot {slot}: no valid invocation (attempts: "
                            f"{[r.get('invalid_reasons') for r in group]})")
            continue
        if len(valid) > 1:
            # The frozen protocol (run_slot) retries an invalid invocation and
            # keeps the first valid one; two valid records for one slot is an
            # identity conflict with no tie-break — refuse rather than pick.
            problems.append(f"slot {slot}: {len(valid)} valid invocations, "
                            "selection not unique under frozen protocol")
            continue
        r = valid[0]
        block, position, arm = slot
        if arm not in ARMS:
            problems.append(f"slot {slot}: unknown arm")
            continue
        want_bin = ("w03_" if kind == "w03" else "w02_") + (
            "original" if arm in ("A0", "R") else "cleanup")
        want_arm_env = "A0" if arm == "A0" else "R"
        if r.get("binary") != want_bin:
            problems.append(f"slot {slot}: binary {r.get('binary')} != {want_bin}")
        want_sha = (bins.get(want_bin) or (None, None))[1]
        if want_sha is None:
            problems.append(f"slot {slot}: frozen BINS has no sha for {want_bin}")
        elif r.get("binary_sha256") != want_sha:
            problems.append(f"slot {slot}: binary sha {r.get('binary_sha256')} != "
                            f"frozen {want_sha}")
        else:
            binpath = os.path.join(root, BIN_PATHS[want_bin])
            if os.path.exists(binpath):
                actual = sha256_file(binpath)
                if actual != want_sha:
                    problems.append(f"slot {slot}: on-disk binary sha {actual} != "
                                    f"frozen {want_sha}")
        if (r.get("readback") or {}).get("arm") != want_arm_env:
            problems.append(f"slot {slot}: readback arm "
                            f"{(r.get('readback') or {}).get('arm')} != {want_arm_env}")
        if r.get("exit") != 0:
            problems.append(f"slot {slot}: exit={r.get('exit')}")
        if r.get("measured_records") != MEASURED_REPS:
            problems.append(f"slot {slot}: measured_records={r.get('measured_records')}")
        if r.get("warmup_records") != WARMUP_REPS:
            problems.append(f"slot {slot}: warmup_records={r.get('warmup_records')}")
        admitted[(block, position, arm)] = r
    # block/arm completeness
    blocks = {(b, p) for (b, p, a) in admitted}
    if blocks != {(b, p) for b in range(BLOCKS) for p in range(3)}:
        problems.append(f"block/position coverage {len(blocks)}/72 incomplete")
    for arm in ARMS:
        n = sum(1 for k in admitted if k[2] == arm)
        if n != BLOCKS:
            problems.append(f"arm {arm}: {n}/{BLOCKS} invocations")
    return admitted, problems


def reparse_invocation(rec, case):
    """Frozen reparse of the per-rep file (measure3.parse_jsonl + metrics)."""
    path = rec.get("jsonl")
    problems = []
    if not path or not os.path.exists(path):
        return None, [f"missing rep file {path}"]
    measured = []
    non_measured = 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            if r.get("case_id") != case:
                continue
            if r.get("measured"):
                measured.append(r)
            else:
                non_measured += 1
    if len(measured) != MEASURED_REPS:
        problems.append(f"{os.path.basename(path)}: {len(measured)} measured reps")
    if non_measured != WARMUP_REPS:
        problems.append(f"{os.path.basename(path)}: {non_measured} non-measured "
                        f"case-matched records (want {WARMUP_REPS})")
    for r in measured:
        if r.get("ops") != OPS_PER_REP:
            problems.append(f"{os.path.basename(path)}: rep ops={r.get('ops')}")
        if not r.get("ok"):
            problems.append(f"{os.path.basename(path)}: rep not ok")
    if problems:
        return None, problems
    ops_tot = sum(r["ops"] for r in measured)
    cpu_tot = sum(r["cpu_s"] for r in measured)
    wall_tot = sum(r["wall_s"] for r in measured)
    n = len(measured)
    metrics = {
        "cpu": 1e9 * cpu_tot / ops_tot,
        "tput": ops_tot / wall_tot,
        "p50": sum(r["lat_p50_ns"] for r in measured) / n,
        "p95": sum(r["lat_p95_ns"] for r in measured) / n,
        "p99": sum(r["lat_p99_ns"] for r in measured) / n,
        "ops": ops_tot,
        "cpu_s": cpu_tot,
    }
    # cross-check against the harness-embedded metrics (same definition)
    m = rec.get("metrics") or {}
    for k, v in metrics.items():
        if k not in ("ops", "cpu_s"):
            hv = m.get({"cpu": "cpu_ns_per_op", "tput": "ops_per_sec"}.get(k, k + "_ns"))
            if hv is None:
                problems.append(f"{os.path.basename(path)}: harness metric {k} missing")
            elif abs(v - hv) > 1e-9 * max(abs(v), abs(hv), 1e-12):
                problems.append(f"{os.path.basename(path)}: {k} recompute {v} vs "
                                f"harness {hv}")
    return metrics, problems


def verify_cells(root, rep, bins):
    per_cell = {}
    for cell in CELL_DIRS:
        cdir, case, _kind = CELL_DIRS[cell]
        recs, path, err = load_records(root, cell)
        if err:
            rep.check(False, f"{cell} results.jsonl", err)
            per_cell[cell] = None
            continue
        admitted, problems = admit_invocations(recs, cell, rep, bins, root)
        for p in problems:
            rep.check(False, f"{cell} admission", p)
        if problems or not admitted:
            per_cell[cell] = None
            continue
        by_block = defaultdict(dict)
        for (block, position, arm), rec in admitted.items():
            metrics, mprobs = reparse_invocation(rec, case)
            for p in mprobs:
                rep.check(False, f"{cell} b{block:02d} p{position} {arm} reparse", p)
            if metrics is None:
                per_cell[cell] = None
                continue
            by_block[block][arm] = metrics
        if len(by_block) != BLOCKS or any(set(v) != set(ARMS) for v in by_block.values()):
            rep.check(False, f"{cell} alignment", "block/arm table incomplete")
            per_cell[cell] = None
            continue
        arm_vals = {a: {k: [by_block[b][a][k] for b in sorted(by_block)]
                        for k in ("cpu", "tput", "p50", "p95", "p99")} for a in ARMS}
        # Flat-mean equivalence: with constant per-rep ops the flat mean over
        # all reps equals the mean of per-invocation ratios exactly (both are
        # (1/120) * sum(cpu_i)/20000 when ops is constant); assert it so the
        # equivalence is demonstrated, not assumed.
        for a in ARMS:
            flat = []
            for (block, position, arm), rec in admitted.items():
                if arm != a:
                    continue
                with open(rec["jsonl"]) as f:
                    for line in f:
                        r = json.loads(line)
                        if r.get("case_id") == case and r.get("measured"):
                            flat.append(r["cpu_s"] / r["ops"])
            rep.check(abs(sum(flat) / len(flat) * 1e9 - sum(arm_vals[a]["cpu"]) / BLOCKS)
                      <= 1e-6,
                      f"{cell} {a} flat-mean equivalence",
                      f"flat={sum(flat)/len(flat)*1e9:.4f} per-inv-mean="
                      f"{sum(arm_vals[a]['cpu'])/BLOCKS:.4f} ns/op (equal because "
                      "every rep carries the same ops; not a reason to skip "
                      "admission/completeness checks)")
        per_cell[cell] = {
            "arm_means": {a: {k: sum(v) / len(v) for k, v in arm_vals[a].items()}
                          for a in ARMS},
            "per_block": arm_vals,
            "results_sha256": sha256_file(path),
            "n_invocations": len(admitted),
        }
    return per_cell


def compare_frozen(root, per_cell, rep):
    """Compare recomputed arm means against the frozen analysis values.

    The expected comparison set is derived from the cells that were actually
    verified: every (cell, arm, metric) must be compared exactly once. A
    frozen file that is empty, truncated, or missing entries therefore FAILS
    coverage instead of passing vacuously (second-round review counterexample:
    round-analysis.json = [] used to print a unconditional success line).
    """
    ra_path = os.path.join(root, "analysis", "round-analysis.json")
    if not os.path.exists(ra_path):
        rep.check(False, "round-analysis.json", f"missing {ra_path}")
        return
    ra = json.load(open(ra_path))
    key_map = {"cpu": "cpu", "tput": "tput", "p50": "p50", "p95": "p95", "p99": "p99"}
    verified = {c for c, v in per_cell.items() if v is not None}
    expected = {(cell, arm, metric) for cell in verified
                for arm in ARMS for metric in key_map}
    compared = set()
    worst = 0.0
    if not isinstance(ra, list) or not ra:
        rep.check(False, "round-analysis.json",
                  f"not a non-empty list (type {type(ra).__name__}, "
                  f"len {len(ra) if isinstance(ra, list) else 'n/a'})")
        ra = []
    for entry in ra:
        if (not isinstance(entry, dict) or "cell" not in entry
                or not isinstance(entry.get("abs"), dict)):
            rep.check(False, "round-analysis.json entry",
                      f"malformed entry: {json.dumps(entry)[:160]}")
            continue
        cell = entry["cell"]
        if cell not in per_cell or per_cell[cell] is None:
            rep.check(False, f"{cell} frozen compare", "no recomputed values")
            continue
        for arm in ARMS:
            for metric, fk in key_map.items():
                try:
                    frozen = entry["abs"][arm][fk][0]
                except (KeyError, TypeError):
                    rep.check(False, f"{cell} {arm} {metric}",
                              "missing in frozen abs")
                    continue
                got = per_cell[cell]["arm_means"][arm][metric]
                compared.add((cell, arm, metric))
                rel = abs(got - frozen) / max(abs(frozen), 1e-12)
                worst = max(worst, rel)
                if rel > 1e-9:
                    rep.check(False, f"{cell} {arm} {metric}",
                              f"recompute {got} vs frozen {frozen} (rel {rel:.2e})")
    missing = expected - compared
    if missing:
        shown = ", ".join("/".join(m) for m in sorted(missing)[:6])
        rep.check(False, "frozen compare coverage",
                  f"{len(missing)} of {len(expected)} expected "
                  f"(cell,arm,metric) comparisons not performed"
                  f"{'; e.g. ' + shown if shown else ''}")
    else:
        rep.check(True, "frozen arm means match at raw precision",
                  f"{len(compared)}/{len(expected)} expected comparisons "
                  f"performed over cells {sorted(verified)}; achieved max "
                  f"relative difference {worst:.2e}; CIs/Holm/gates NOT "
                  f"recomputed")


def p99_diff_table(per_cell, rep):
    cell = "MAIN-W03-D16"
    if per_cell.get(cell) is None:
        rep.check(False, "p99 diff table", "MAIN cell not verified")
        return {}
    out = {}
    pb = per_cell[cell]["per_block"]
    means = per_cell[cell]["arm_means"]
    for x, y in (("C", "A0"), ("R", "A0")):
        run_level_us = (means[x]["p99"] - means[y]["p99"]) / 1000.0
        effects = [(a - b) / b for a, b in zip(pb[x]["p99"], pb[y]["p99"])]
        out[f"{x}-{y}"] = {
            "run_level_mean_diff_us": run_level_us,
            "per_block_relative_effect_mean": sum(effects) / len(effects),
            "pooled_per_request_p99": "UNAVAILABLE (main runs store per-rep "
                                      "percentiles only, not request latencies)",
        }
    rep.check(True, "p99 difference calibers emitted (distinct, not interchangeable)",
              json.dumps(out, indent=1))
    return out


def f5_summary_checks(f5, rep):
    """Pure assertions on a derived f5-position summary dict."""
    mp = f5.get("member_position") or {}

    prefixes = {k.rsplit("|pos", 1)[0] for k in mp}
    rep.check(prefixes <= {p.rstrip("|") for p in F5_EXPECT_SETS},
              "F5 bucket families restricted", f"got {sorted(prefixes)}")
    for prefix, want in F5_EXPECT_SETS.items():
        got = {int(k.split("|pos")[1]) for k in mp if k.startswith(prefix)}
        rep.check(got == want, f"F5 {prefix} exact position set",
                  f"got {sorted(got)} want {sorted(want)}")
        for k in (k for k in mp if k.startswith(prefix)):
            n = mp[k]["n"]
            pct = mp[k]["tail_gt55_pct"]
            tail = round(pct * n / 100.0)
            # stored pct was computed as 100*tail/n in float64: the identity
            # must hold to float round-trip error, not to a counting tolerance
            ok = (0 <= tail <= n
                  and abs(100.0 * tail / n - pct) <= 1e-6 + 1e-9 * abs(pct))
            rep.check(ok, f"F5 {k} integer tail count",
                      f"n={n} pct={pct:.4f} -> tail={tail}")

    vt = f5.get("validation_totals") or {}
    tags = f5.get("validation") or {}
    rep.check(vt.get("tags") == len(tags) == 24, "F5 tag count",
              f"totals {vt.get('tags')} / entries {len(tags)}")
    rep.check(vt.get("rows") == sum(v["rows"] for v in tags.values()) == 480000,
              "F5 rows reconciliation",
              "480000 = 24 instrument-on R diag tags x 20000 ops "
              "(12 x W03-uring-4k-d16 + 12 x W03-uring-4k-d4)")
    per_case = defaultdict(int)
    for t, v in tags.items():
        per_case[t.rsplit("-b", 1)[0]] += v["rows"]
    for case, rows in per_case.items():
        rep.check(rows == 240000, f"F5 {case} rows", f"{rows}")
    on_off = defaultdict(int)
    for t in tags:
        on_off[t.split("-")[-2]] += 1
    rep.check(set(on_off) == {"on"},
              "F5 pools instrument-on phases only", f"{dict(on_off)}")
    rep.check(vt.get("anomalous_groups") == 0 and vt.get("unpositioned_rows") == 0
              and vt.get("tags_with_seq_gaps") == 0,
              "F5 recovery validation clean (kept PERTURBED status)")

    # Closure under the frozen recovery script's inclusion/exclusion rules
    # (recover_f5_position.py, read from its code — note its own
    # "terminal/anomalous groups excluded" semantics string is imprecise:
    # CONTIGUOUS terminal groups are positioned, only non-contiguous terminal
    # and anomalous groups become unpositioned_rows). Consequences:
    #   positioned rows = rows - unpositioned_rows  (exactly, per tag)
    #   buckets are written only when n >= MIN_N (= 20)
    # so per case:  sum(bucket n) = positioned - rows_dropped_by_min_n
    # with 0 <= rows_dropped_by_min_n <= 19 * depth*(depth+1)/2 possible
    # (case, cw, pos) keys. A summary whose bucket denominators do not close
    # against the recovery totals within that bound is rejected (second-round
    # review counterexample: sums 216800 vs 480000 with clean validation
    # counters used to pass).
    for k, v in mp.items():
        rep.check(v["n"] >= F5_MIN_N, f"F5 {k} bucket n >= MIN_N",
                  f"n={v['n']} MIN_N={F5_MIN_N}")
    for t, v in tags.items():
        rep.check(v["groups"] == v["normal_groups"] + v["terminal_groups"]
                  + v["anomalous_groups"],
                  f"F5 {t} group arithmetic",
                  f"groups={v['groups']} vs {v['normal_groups']}+"
                  f"{v['terminal_groups']}+{v['anomalous_groups']}")
        rep.check((v["rows"] > 0) == (v["groups"] > 0),
                  f"F5 {t} rows/groups consistency",
                  f"rows={v['rows']} groups={v['groups']}")
        rep.check(0 <= v["unpositioned_rows"] <= v["rows"],
                  f"F5 {t} unpositioned bound",
                  f"unpositioned={v['unpositioned_rows']} rows={v['rows']}")
    positioned = defaultdict(int)
    for t, v in tags.items():
        positioned[t.rsplit("-b", 1)[0]] += v["rows"] - v["unpositioned_rows"]
    for case, pos_rows in sorted(positioned.items()):
        suffix = case.rsplit("-d", 1)[-1]
        depth = int(suffix) if suffix.isdigit() else None
        bucket_sum = sum(v["n"] for k, v in mp.items()
                         if k.split("|")[0] == case)
        residual = pos_rows - bucket_sum
        if depth is None:
            rep.check(False, f"F5 {case} closure", f"unparseable depth {case!r}")
            continue
        bound = (F5_MIN_N - 1) * depth * (depth + 1) // 2
        rep.check(0 <= residual <= bound,
                  f"F5 {case} bucket denominators close against recovery totals",
                  f"positioned={pos_rows} bucket_sum={bucket_sum} residual="
                  f"{residual} (sub-MIN_N drop bound {bound})")


def f5_checks(root, rep):
    f5_path = os.path.join(root, "analysis", "f5-position.json")
    if not os.path.exists(f5_path):
        rep.check(False, "F5", f"missing {f5_path}")
        return
    f5 = json.load(open(f5_path))
    f5_summary_checks(f5, rep)

    # Independent offline rerun of the frozen recovery script on raw input.
    src = os.path.join(root, "scripts", "recover_f5_position.py")
    ops = os.path.join(root, "raw", "diag-ops.csv")
    if not (os.path.exists(src) and os.path.exists(ops)):
        rep.check(False, "F5 rerun", "frozen script or raw diag-ops.csv missing")
        return
    with tempfile.TemporaryDirectory(prefix="f5-rerun-") as td:
        copy = os.path.join(td, "recover_f5_position.py")
        text = open(src).read()
        text = text.replace('OUT_JSON = os.path.join(ROOT, "analysis", "f5-position.json")',
                            f'OUT_JSON = {os.path.join(td, "f5.json")!r}')
        text = text.replace('OUT_CSV = os.path.join(ROOT, "figures", "figure-data", '
                            '"f5-position.csv")',
                            f'OUT_CSV = {os.path.join(td, "f5.csv")!r}')
        if ("OUT_JSON = os.path.join(ROOT" in text
                or "OUT_CSV = os.path.join(ROOT" in text):
            # both replacements must have landed; refuse to run anything that
            # could write into the frozen evidence tree
            rep.check(False, "F5 rerun sandboxing", "output path patch failed")
            return
        with open(copy, "w") as f:
            f.write(text)
        proc = subprocess.run([sys.executable, copy], capture_output=True, text=True)
        rep.check(proc.returncode == 0, "F5 frozen recovery rerun exit",
                  (proc.stderr or proc.stdout).strip()[-200:])
        out_path = os.path.join(td, "f5.json")
        if proc.returncode == 0 and os.path.exists(out_path):
            rerun = json.load(open(out_path))
            same = rerun == f5
            rep.check(same, "F5 rerun reproduces frozen derived summary exactly",
                      "derived-summary integrity only; per-request identity vs raw "
                      "is inherited from the frozen recovery assumptions "
                      "(depth-cw chunk split, contiguous seq, terminal/anomaly drops)")


def old_checker_defect_demo(rep):
    """Expose the withdrawn v1 logic's blind spots on a synthetic fixture.

    The v1 checker (a) globbed every jsonl and admitted any measured-looking
    record (so a duplicate/invalid invocation was silently double-counted)
    and (b) only checked positions that were PRESENT in the F5 summary (so a
    summary retaining positions 0 and 8 alone passed the front/back check).
    Both are reproduced here against a minimal fixture; the v2 checks above
    must reject the same fixture.
    """
    with tempfile.TemporaryDirectory(prefix="old-defect-") as td:
        # (a) duplicate invocation double-counted by flat glob mean
        cell_dir = os.path.join(td, "raw", "mini", "main")
        os.makedirs(os.path.join(cell_dir, "jsonl"))
        fixture_bins = {"w03_original": ("unused-path",
                                         "7bf83eb93fa589628649931a1a5cdba6a"
                                         "9e9375083e8ced38e09a73c455b9da5")}

        def write_inv(tag, cpu_s, valid=True, measured=True):
            j = os.path.join(cell_dir, "jsonl", f"{tag}.jsonl")
            recs = [{"case_id": "CASE", "measured": False, "ops": 20000,
                     "ok": True, "cpu_s": 0.01, "wall_s": 0.01,
                     "lat_p50_ns": 1.0, "lat_p95_ns": 2.0, "lat_p99_ns": 3.0}]
            if measured:
                recs.append({"case_id": "CASE", "measured": True, "ops": 20000,
                             "ok": True, "cpu_s": cpu_s, "wall_s": 0.05,
                             "lat_p50_ns": 1.0, "lat_p95_ns": 2.0, "lat_p99_ns": 3.0})
            with open(j, "w") as f:
                for r in recs:
                    f.write(json.dumps(r) + "\n")
            entry = {"cell": "MINI", "case": "CASE", "seg": "main", "block": 0,
                     "position": 0, "arm": "A0", "binary": "w03_original",
                     "binary_sha256": fixture_bins["w03_original"][1], "attempt": 1,
                     "exit": 0, "measured_records": 1 if measured else 0,
                     "warmup_records": 1, "valid": valid, "jsonl": j,
                     "readback": {"arm": "A0"},
                     "metrics": {"cpu_ns_per_op": 1e9 * cpu_s / 20000,
                                 "ops_per_sec": 20000 / 0.05,
                                 "p50_ns": 1.0, "p95_ns": 2.0, "p99_ns": 3.0,
                                 "wall_s": 0.05, "ops": 20000, "cpu_s": cpu_s,
                                 "stats_max_outstanding": 1}}
            with open(os.path.join(cell_dir, "results.jsonl"), "a") as f:
                f.write(json.dumps(entry) + "\n")
            return j
        j1 = write_inv("main-b00-p0-A0-a1", 0.02)   # valid, 1000 ns/op
        j2 = write_inv("main-b00-p0-A0-a1-dup", 0.10)  # second "measured-looking" file
        # v1 glob logic: mean over every measured-looking record
        vals = []
        for j in (j1, j2):
            for line in open(j):
                r = json.loads(line)
                if r.get("measured") and r.get("ops", 0) >= 20000 and r.get("cpu_s", 0) > 0:
                    vals.append(r["cpu_s"] / r["ops"] * 1e9)
        old_mean = sum(vals) / len(vals)
        rep.check(abs(old_mean - 3000.0) < 1e-6,
                  "old-v1 glob logic demonstrably double-counts the duplicate",
                  f"flat mean {old_mean:.1f} ns/op counts both files; true value "
                  "is single-admission 1000.0")
        recs = [json.loads(l) for l in open(os.path.join(cell_dir, "results.jsonl"))]
        # the fixture's on-disk binaries do not exist; root="." avoids the
        # re-hash step, which is exercised on the real root above
        admitted, problems = admit_invocations(recs, "MAIN-W03-D16", rep,
                                               fixture_bins, ".")
        rep.check(len(problems) >= 1 and problems[0].startswith("slot (0, 0, 'A0')"),
                  "v2 admission rejects the same fixture",
                  f"{problems[:1]}")

        # (b) F5 positions-only check passes a truncated summary
        pos = {0: 1.17, 8: 9.10}
        front = [pos[i] for i in range(0, 8) if i in pos]
        back = [pos[i] for i in range(8, 16) if i in pos]
        v1_ok = max(front) < 1.5 and min(back) > 8.5
        got = set(pos)
        rep.check(v1_ok and got != F5_EXPECT_SETS["W03-uring-4k-d16|cw0|"],
                  "old-v1 F5 logic passes a two-position summary",
                  "front/back band check alone cannot detect missing positions; "
                  "v2 requires the exact set 0..15")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--json", help="optional machine-readable output path")
    args = ap.parse_args(argv)
    rep = Report()
    print(f"input root: {args.root}")
    try:
        bins = frozen_bins(args.root)
    except (OSError, RuntimeError) as e:
        print(f"FAIL frozen BINS :: {e}")
        return 1
    per_cell = verify_cells(args.root, rep, bins)
    compare_frozen(args.root, per_cell, rep)
    diffs = p99_diff_table(per_cell, rep)
    f5_checks(args.root, rep)
    old_checker_defect_demo(rep)
    summary = {
        "checks": rep.checks,
        "failures": rep.fails,
        "cells": {c: (v or {}).get("results_sha256") for c, v in per_cell.items()},
        "p99_diffs": diffs,
        "precision_claim": "recomputed per-invocation metrics (frozen harness "
                           "definition) aggregated as analyze_round (mean over 24 "
                           "blocks); arm means match analysis/round-analysis.json at "
                           "raw float precision (max rel diff reported); bootstrap "
                           "CIs, Holm corrections and gate thresholds NOT recomputed",
    }
    if args.json:
        with open(args.json, "w") as f:
            json.dump(summary, f, indent=1)
        print(f"wrote {args.json}")
    print(f"== {rep.checks - rep.fails}/{rep.checks} checks pass ==")
    return 0 if rep.ok else 1


if __name__ == "__main__":
    sys.exit(main())
