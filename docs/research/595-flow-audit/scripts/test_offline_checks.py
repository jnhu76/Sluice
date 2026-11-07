#!/usr/bin/env python3
"""Deterministic tests for the offline verification tools (no server, no timing).

Covers the review-mandated counterexamples (R4 synthetic CPU window test,
R6 admission negatives, R6 F5 completeness negatives, R6 CSV negatives) plus
a structural check of the five mermaid diagrams in FLOW_AUDIT.md. Every
negative case must FAIL the checked component (nonzero/failed report);
every positive case must reproduce a hand-computed value.

Run: python3 -I test_offline_checks.py   (exit 0 = all tests pass)
"""
import csv
import importlib.util
import io
import json
import os
import re
import shutil
import sys
import tempfile
import contextlib

HERE = os.path.dirname(os.path.abspath(__file__))


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


verify = load("verify_inherited_evidence")
cpu = load("cpu_scope_recalc")
ledger = load("ledger_csv")

PASS = 0
FAIL = 0


def case(name, ok, detail=""):
    global PASS, FAIL
    print(("PASS " if ok else "FAIL ") + name + (f" :: {detail}" if detail else ""))
    if ok:
        PASS += 1
    else:
        FAIL += 1


# ---------------------------------------------------------------------------
# Shared mini-root builder: a complete synthetic round (1 block, 3 arms,
# 2 measured reps of 100 ops) with hand-computable values.
# ---------------------------------------------------------------------------
MINI_SHA = "ab" * 32


def build_mini_root(td, n_blocks=1, reps=2, ops=100, attempt_plan=None,
                    dup_valid=False, drop_arm=None, bad_ops=False,
                    bad_measured=False):
    cdir, case_id = "mini-cell", "MINI-CASE"
    cell_dir = os.path.join(td, "raw", cdir, "main")
    os.makedirs(os.path.join(cell_dir, "jsonl"))
    os.makedirs(os.path.join(td, "harness"), exist_ok=True)
    with open(os.path.join(td, "harness", "measure3.py"), "w") as f:
        f.write('BINS = {"w03_original": ("/nonexistent", "%s"),'
                ' "w02_original": ("/x", "%s"),'
                ' "w03_cleanup": ("/x", "%s"),'
                ' "w02_cleanup": ("/x", "%s")}\n'
                % (MINI_SHA, "cd" * 32, "ef" * 32, "01" * 32))
    attempts_log = []
    for b in range(n_blocks):
        for p, arm in enumerate(("A0", "R", "C")):
            if arm == drop_arm and b == n_blocks - 1:
                continue
            plan = (attempt_plan or {}).get((b, arm), [(1, True)])
            for attempt, valid in plan:
                if dup_valid and attempt == 1 and (b, arm) == (0, "A0"):
                    pass  # both writes below stay valid
                tag = f"main-b{b:02d}-p{p}-{arm}-a{attempt}"
                j = os.path.join(cell_dir, "jsonl", tag + ".jsonl")
                rows = [{"case_id": "OTHER-CASE", "measured": False, "ops": 0,
                         "ok": True, "cpu_s": 0.0, "wall_s": 0.0,
                         "lat_p50_ns": 0, "lat_p95_ns": 0, "lat_p99_ns": 0},
                        {"case_id": case_id, "measured": False, "ops": ops,
                         "ok": True, "cpu_s": 0.05, "wall_s": 0.05,
                         "lat_p50_ns": 10, "lat_p95_ns": 20, "lat_p99_ns": 30}]
                n_reps = reps if (valid and not bad_measured) else 1
                for r in range(n_reps):
                    rows.append({"case_id": case_id, "measured": True,
                                 "ops": ops if not bad_ops else ops - 1,
                                 "ok": True,
                                 "cpu_s": 0.001 * (r + 1) if arm == "A0" else 0.002,
                                 "wall_s": 0.01,
                                 "lat_p50_ns": 100, "lat_p95_ns": 200,
                                 "lat_p99_ns": 100 if r == 0 else 300})
                with open(j, "w") as f:
                    for row in rows:
                        f.write(json.dumps(row) + "\n")
                cpu_tot = sum(r["cpu_s"] for r in rows if r["measured"])
                ops_tot = sum(r["ops"] for r in rows if r["measured"])
                p99s = [r["lat_p99_ns"] for r in rows if r["measured"]]
                n_meas = len(p99s)
                bin_key = "w03_cleanup" if arm == "C" else "w03_original"
                readback_arm = "A0" if arm == "A0" else "R"
                entry = {"cell": "MINI", "case": case_id, "seg": "main",
                         "block": b, "position": p, "arm": arm,
                         "binary": bin_key,
                         "binary_sha256": MINI_SHA if bin_key == "w03_original"
                         else "ef" * 32,
                         "attempt": attempt, "exit": 0 if valid else 3,
                         "measured_records": n_meas, "warmup_records": 1,
                         "valid": valid, "jsonl": j,
                         "readback": {"arm": readback_arm},
                         "ru_utime": 0.9, "ru_stime": 0.4, "wall_s": 1.5,
                         "metrics": {"cpu_ns_per_op": 1e9 * cpu_tot / ops_tot,
                                     "ops_per_sec": ops_tot / (0.01 * n_meas),
                                     "p50_ns": 100.0, "p95_ns": 200.0,
                                     "p99_ns": sum(p99s) / n_meas,
                                     "wall_s": 0.01 * n_meas, "ops": ops_tot}}
                with open(os.path.join(cell_dir, "results.jsonl"), "a") as f:
                    f.write(json.dumps(entry) + "\n")
                attempts_log.append((b, arm, attempt, valid))
                if dup_valid and attempt == 1 and (b, arm) == (0, "A0"):
                    # second VALID record for the same slot
                    entry2 = dict(entry)
                    j2 = j.replace("a1", "a1b")
                    shutil.copy(j, j2)
                    entry2["jsonl"] = j2
                    with open(os.path.join(cell_dir, "results.jsonl"), "a") as f:
                        f.write(json.dumps(entry2) + "\n")
                    attempts_log.append((b, arm, 1, "dup-valid"))
    return cdir, case_id


def run_verify_mini(td, cdir, case_id, n_blocks=1, reps=2, ops=100):
    saved = {k: getattr(verify, k) for k in
             ("CELL_DIRS", "BLOCKS", "MEASURED_REPS", "WARMUP_REPS",
              "OPS_PER_REP", "ARMS")}
    verify.CELL_DIRS = {"MINI": (cdir, case_id, "w03")}
    verify.BLOCKS, verify.MEASURED_REPS = n_blocks, reps
    verify.WARMUP_REPS, verify.OPS_PER_REP, verify.ARMS = 1, ops, ("A0", "R", "C")
    rep = verify.Report()
    buf = io.StringIO()
    mini_bins = {"w03_original": ("/x", MINI_SHA), "w03_cleanup": ("/x", "ef" * 32)}
    try:
        with contextlib.redirect_stdout(buf):
            per_cell = verify.verify_cells(td, rep, mini_bins)
            ra_path = os.path.join(td, "analysis", "round-analysis.json")
            if os.path.exists(ra_path):
                verify.compare_frozen(td, per_cell, rep)
    finally:
        for k, v in saved.items():
            setattr(verify, k, v)
    return rep, per_cell


def test_admission_positives():
    with tempfile.TemporaryDirectory() as td:
        cdir, case_id = build_mini_root(td)
        rep, per_cell = run_verify_mini(td, cdir, case_id)
        case("admission: legal mini sample verifies", rep.ok,
             f"{rep.fails} fails: {[l for l in rep.lines if l.startswith('FAIL')][:3]}")
        # hand computation, arm A0: cpu_s 0.001+0.002 over 200 ops
        want_cpu = 1e9 * 0.003 / 200
        want_p99 = (100 + 300) / 2
        got = per_cell["MINI"]["arm_means"]["A0"]
        case("admission: hand-computed A0 cpu mean", abs(got["cpu"] - want_cpu) < 1e-9,
             f"got {got['cpu']} want {want_cpu}")
        case("admission: hand-computed A0 p99 mean",
             abs(got["p99"] - want_p99) < 1e-9, f"got {got['p99']} want {want_p99}")
        # R arm: cpu 0.002+0.002 over 200 ops
        case("admission: hand-computed R cpu mean",
             abs(per_cell["MINI"]["arm_means"]["R"]["cpu"] - 1e9 * 0.004 / 200) < 1e-9)


def test_admission_negatives():
    # N3a: failed attempt then valid attempt -> valid one used, mean unchanged
    with tempfile.TemporaryDirectory() as td:
        cdir, case_id = build_mini_root(
            td, attempt_plan={(0, "A0"): [(1, False), (2, True)]})
        rep, per_cell = run_verify_mini(td, cdir, case_id)
        case("admission: failed attempt 1 + valid attempt 2 -> valid used",
             rep.ok and abs(per_cell["MINI"]["arm_means"]["A0"]["cpu"]
                            - 1e9 * 0.003 / 200) < 1e-9,
             f"fails={rep.fails}")
    # N3b: duplicate valid invocation -> fail
    with tempfile.TemporaryDirectory() as td:
        cdir, case_id = build_mini_root(td, dup_valid=True)
        rep, _ = run_verify_mini(td, cdir, case_id)
        case("admission: duplicate valid invocation -> fail", rep.fails > 0,
             next((l for l in rep.lines if l.startswith("FAIL")), ""))
    # N4a: missing arm on one block -> fail
    with tempfile.TemporaryDirectory() as td:
        cdir, case_id = build_mini_root(td, n_blocks=2, drop_arm="C")
        rep, _ = run_verify_mini(td, cdir, case_id, n_blocks=2)
        case("admission: missing block/arm -> fail", rep.fails > 0)
    # N4b: measured_records != expected -> fail
    with tempfile.TemporaryDirectory() as td:
        cdir, case_id = build_mini_root(td, bad_measured=True)
        rep, _ = run_verify_mini(td, cdir, case_id)
        case("admission: wrong measured-rep count -> fail", rep.fails > 0)
    # N4c: ops mismatch -> fail
    with tempfile.TemporaryDirectory() as td:
        cdir, case_id = build_mini_root(td, bad_ops=True)
        rep, _ = run_verify_mini(td, cdir, case_id)
        case("admission: ops mismatch -> fail", rep.fails > 0)
    # N4d: empty results.jsonl -> explicit failure, not silent success
    with tempfile.TemporaryDirectory() as td:
        cdir, case_id = "mini-cell", "MINI-CASE"
        os.makedirs(os.path.join(td, "raw", cdir, "main"), exist_ok=True)
        open(os.path.join(td, "raw", cdir, "main", "results.jsonl"), "w").close()
        rep, per_cell = run_verify_mini(td, cdir, case_id)
        case("admission: empty input -> fail (never empty success)",
             rep.fails > 0 and per_cell.get("MINI") is None)


def make_f5(tail_pct=1.2, n=1000, rows=480000, d16_positions=None,
            extra_bucket=None, tag_rows=None):
    """Well-formed derived summary: d16 cw0 0..15, d16 cw8 0..7, d4 cw0 0..3,
    24 tags split over the two cases, totals closing."""
    d16_positions = list(range(16)) if d16_positions is None else d16_positions
    mp = {}
    for p in d16_positions:
        mp[f"W03-uring-4k-d16|cw0|pos{p}"] = {
            "n": n, "median_us": 40.0, "p99_us": 60.0,
            "tail_gt55_pct": tail_pct, "tail_stop_kinds": {}}
    for p in range(8):
        mp[f"W03-uring-4k-d16|cw8|pos{p}"] = {
            "n": 100, "median_us": 40.0, "p99_us": 60.0,
            "tail_gt55_pct": 25.0, "tail_stop_kinds": {}}
    for p in range(4):
        mp[f"W03-uring-4k-d4|cw0|pos{p}"] = {
            "n": 50000, "median_us": 15.0, "p99_us": 20.0,
            "tail_gt55_pct": 0.02, "tail_stop_kinds": {}}
    if extra_bucket:
        mp[extra_bucket[0]] = extra_bucket[1]
    tags = {}
    for i in range(12):
        tags[f"W03-uring-4k-d16-b{i:02d}-p{i % 6}-R-on-a1"] = {
            "rows": 20000, "groups": 0, "normal_groups": 0, "terminal_groups": 0,
            "anomalous_groups": 0, "unpositioned_rows": 0, "seq_gaps": 0}
        tags[f"W03-uring-4k-d4-b{i:02d}-p{i % 6}-R-on-a1"] = {
            "rows": 20000, "groups": 0, "normal_groups": 0, "terminal_groups": 0,
            "anomalous_groups": 0, "unpositioned_rows": 0, "seq_gaps": 0}
    if tag_rows:
        k, v = tag_rows
        tags[k]["rows"] = v
    total = sum(t["rows"] for t in tags.values())
    return {
        "tail_threshold_us": 55.0,
        "semantics": {},
        "validation": tags,
        "member_position": mp,
        "window_occupancy": {},
        "validation_totals": {"tags": len(tags), "rows": rows if rows is not None
                              else total, "anomalous_groups": 0,
                              "terminal_groups": 0, "unpositioned_rows": 0,
                              "tags_with_seq_gaps": 0},
    }


def test_f5_negatives():
    # N1: only positions 0 and 8
    rep = verify.Report()
    verify.f5_summary_checks(make_f5(d16_positions=[0, 8]), rep)
    case("F5: two-position summary rejected", rep.fails > 0)
    # N2a: missing one position (0..14)
    rep = verify.Report()
    verify.f5_summary_checks(make_f5(d16_positions=list(range(15))), rep)
    case("F5: missing position rejected", rep.fails > 0)
    # N2b: unexpected extra family (wrong shape/tag)
    rep = verify.Report()
    bad = make_f5(extra_bucket=("W03-uring-4k-d8|cw0|pos0",
                                {"n": 500, "tail_gt55_pct": 5.0}))
    verify.f5_summary_checks(bad, rep)
    case("F5: foreign case bucket rejected", rep.fails > 0)
    # N2c: totals not closing (rows field != sum of tag rows)
    rep = verify.Report()
    f5 = make_f5(rows=480000 - 20000)
    verify.f5_summary_checks(f5, rep)
    case("F5: totals mismatch rejected", rep.fails > 0)
    # N2c2: a single tag whose rows do not close the per-case sum
    rep = verify.Report()
    f5 = make_f5(tag_rows=("W03-uring-4k-d16-b00-p0-R-on-a1", 19000))
    verify.f5_summary_checks(f5, rep)
    case("F5: tag rows not closing per-case totals rejected", rep.fails > 0)
    # N2d: non-integer tail fraction (percentage inconsistent with n)
    rep = verify.Report()
    f5 = make_f5()
    k0 = f"{next(iter(f5['member_position']))}"
    f5["member_position"][k0]["tail_gt55_pct"] = 1.234567
    verify.f5_summary_checks(f5, rep)
    case("F5: inconsistent tail percentage rejected", rep.fails > 0)
    # positive: all three families complete, integer-consistent tails
    rep = verify.Report()
    verify.f5_summary_checks(make_f5(), rep)
    case("F5: complete well-formed summary passes", rep.ok,
         [l for l in rep.lines if l.startswith("FAIL")][:2])


def test_cpu_windows():
    with tempfile.TemporaryDirectory() as td:
        cdir, case_id = build_mini_root(td, reps=2, ops=100)
        # add sampler-style buckets to each record
        path = os.path.join(td, "raw", cdir, "main", "results.jsonl")
        recs = [json.loads(l) for l in open(path)]
        for r in recs:
            r["buckets"] = {
                "available": True, "samples": 20, "max_user_threads": 3,
                "app_ticks": 5, "driver_ticks": 1, "io_wq_ticks": 3,
                "io_wq_in_process": {"99": {"comm": "iou-wrk-98", "ticks": 3}},
                "in_thread_kernel_stime_ticks": 6, "threads": {},
                "residual_ticks_est": 0.5, "residual_bound_ticks": 5.0,
                "ru_total_ticks_est": 9.5, "io_wq": {}, "tick_hz": 100,
            }
        with open(path, "w") as f:
            for r in recs:
                f.write(json.dumps(r) + "\n")
        saved = {k: getattr(cpu, k) for k in
                 ("CASES", "BLOCKS", "ARMS", "MEASURED_REPS", "WARMUP_REPS",
                  "OPS_PER_REP")}
        cpu.CASES = {cdir: case_id}
        cpu.BLOCKS, cpu.ARMS = 1, ("A0", "R", "C")
        cpu.MEASURED_REPS, cpu.WARMUP_REPS, cpu.OPS_PER_REP = 2, 1, 100
        buf, jpath = io.StringIO(), os.path.join(td, "cpu.json")
        try:
            with contextlib.redirect_stdout(buf):
                rc = cpu.main(["--root", td, "--cells", cdir, "--json", jpath])
            out = json.load(open(jpath))
            a0 = out["cells"][cdir]["A0"]
            # primary uses ONLY measured reps: A0 reps 0.001+0.002 over 200 ops;
            # the warmup rep (0.05 s) and the OTHER-CASE row must not enter
            case("cpu: primary excludes warmup/oracle overhead",
                 abs(a0["primary_measured_cpu_ns_per_op_mean"] - 15000.0) < 1e-9,
                 f"got {a0['primary_measured_cpu_ns_per_op_mean']} want 15000.0")
            # whole-invocation rusage stays in seconds and is never divided by ops
            serialized = json.dumps(out)
            fabricated = [k for k in re.findall(r'"([^"]*ns_per_op[^"]*)"',
                                                serialized)
                          if k != "primary_measured_cpu_ns_per_op_mean"
                          and k != "harness_metrics_cpu_ns_per_op"]
            case("cpu: no fabricated ns/op for invocation/tick windows",
                 not fabricated, f"found {fabricated}")
            case("cpu: invocation window kept in seconds",
                 abs(a0["invocation_ru_utime_s_mean"] - 0.9) < 1e-12
                 and abs(a0["invocation_ru_stime_s_mean"] - 0.4) < 1e-12)
            case("cpu: sampled-classified share computed",
                 abs(a0["iowq_share_of_sampled_classified_pct"] - 100 * 3 / 9) < 1e-9)
            case("cpu: exit 0 on complete input", rc == 0)
            # missing required field -> explicit failure path, exit 2
            for r in recs:
                del r["ru_utime"]
                del r["buckets"]
            with open(path, "w") as f:
                for r in recs:
                    f.write(json.dumps(r) + "\n")
            buf2 = io.StringIO()
            with contextlib.redirect_stdout(buf2):
                rc2 = cpu.main(["--root", td, "--cells", cdir])
            case("cpu: missing ru/buckets -> problems + exit 2",
                 rc2 == 2 and "ru_utime missing" in buf2.getvalue()
                 and "buckets missing" in buf2.getvalue(),
                 (buf2.getvalue().strip().splitlines() or [""])[0][:80])
        finally:
            for k, v in saved.items():
                setattr(cpu, k, v)


def test_csv_negatives():
    good = os.path.join(HERE, "..", "WORK_AND_WAIT_LEDGER.csv")
    case("csv: rebuilt ledger passes", not ledger.check(good))
    with tempfile.TemporaryDirectory() as td:
        trunc = os.path.join(td, "trunc.csv")
        lines = open(good).read().splitlines()
        cut = lines[1].split(",")
        open(trunc, "w").write("\n".join(
            [lines[0], ",".join(cut[:-1]), *lines[2:]]) + "\n")
        case("csv: truncated row -> nonzero exit",
             ledger.check(trunc) != [] and ledger.main(["check", trunc]) == 1)
        over = os.path.join(td, "over.csv")
        open(over, "w").write("\n".join(
            [lines[0], lines[1] + ",EXTRA", *lines[2:]]) + "\n")
        case("csv: overlong row -> nonzero exit",
             ledger.check(over) != [] and ledger.main(["check", over]) == 1)
        badgrade = os.path.join(td, "grade.csv")
        g = lines[1].split(",")
        g[8] = "PUBLISHED_VERIFIED"
        open(badgrade, "w").write("\n".join(
            [lines[0], ",".join(g), *lines[2:]]) + "\n")
        case("csv: non-vocabulary evidence grade -> nonzero exit",
             ledger.check(badgrade) != [])


def test_mermaid_structure():
    path = os.path.join(HERE, "..", "FLOW_AUDIT.md")
    text = open(path).read()
    blocks = re.findall(r"```mermaid\n(.*?)```", text, re.S)
    # five diagram groups; 图C is intentionally split into a main
    # stateDiagram plus a legal-branches flowchart (review R5)
    case("mermaid: six blocks (5 groups, 图C split into main+branches)",
         len(blocks) == 6, f"got {len(blocks)}")
    heads = [b.strip().splitlines()[0].split()[0] for b in blocks]
    case("mermaid: known diagram types",
         all(h in ("flowchart", "stateDiagram-v2", "sequenceDiagram") for h in heads),
         str(heads))
    ok_balance = True
    detail = []
    for i, b in enumerate(blocks, 1):
        lines_b = [l.strip() for l in b.strip().splitlines()]
        if heads[i - 1] == "flowchart":
            sub = sum(1 for l in lines_b if l.startswith("subgraph "))
            ends = sum(1 for l in lines_b if l == "end")
            if sub != ends:
                ok_balance = False
                detail.append(f"block {i}: subgraph {sub} vs end {ends}")
        if heads[i - 1] == "stateDiagram-v2":
            notes = sum(1 for l in lines_b if l.startswith("note "))
            endnotes = sum(1 for l in lines_b if l == "end note")
            if notes != endnotes:
                ok_balance = False
                detail.append(f"block {i}: note {notes} vs end note {endnotes}")
    case("mermaid: subgraph/note balance (structural check; render NOT verified)",
         ok_balance, "; ".join(detail))


def main():
    test_admission_positives()
    test_admission_negatives()
    test_f5_negatives()
    test_cpu_windows()
    test_csv_negatives()
    test_mermaid_structure()
    print(f"== {PASS}/{PASS + FAIL} tests pass ==")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
