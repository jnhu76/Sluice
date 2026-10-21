#!/usr/bin/env python3
"""Fail-closed acceptance reducer for the E5 H2b A/B campaign.

Consumes the raw ab.jsonl rows and the arm-identity receipt, recomputes the
pre-declared acceptance rule from scratch, and is the only authority that may
record PASS/FAIL. Every structural or numeric anomaly — missing rows,
duplicated rows, unknown configurations, failed byte oracles, illegal
measurements, mismatched arm identity, a performance regression — fails
closed to a nonzero exit with a machine-readable verdict.json.

--selftest runs the counterfactual fixtures below and must be re-run
whenever this reducer changes; a reducer that cannot reject its own
corruption classes cannot certify anything.
"""

import argparse
import json
import os
import re
import statistics
import sys
import tempfile

REPS = 15
RATIO_LIMIT = 1.03
MAX_PAIRS_ABOVE = 10

# (block, depth, capacity) — data configs run timed/oracle/latency phases,
# zero configs (block == 0) run the single timed phase.
DATA_CONFIGS = [
    (4096, 1, 16),
    (4096, 1, 64),
    (4096, 1, 256),
    (4096, 1, 1024),
    (4096, 8, 1024),
    (4096, 8, 16),
    (65536, 1, 1024),
    (65536, 1, 64),
    (1048576, 1, 64),
]
ZERO_CONFIGS = [
    (0, 64, 1024),
    (0, 8, 1024),
]
CONFIG_PHASES = {cfg: {"timed", "oracle", "latency"} for cfg in DATA_CONFIGS}
CONFIG_PHASES.update({cfg: {"timed"} for cfg in ZERO_CONFIGS})
ARMS = ("base", "h2b")


class Reject(Exception):
    pass


def config_key(row):
    return (row["block"], row["depth"], row["capacity"])


def check_row(row, reasons, where):
    def bad(msg):
        reasons.append(f"{where}: {msg}")

    for field in ("arm", "rep", "block", "depth", "capacity", "requests", "phase",
                  "ok", "wall_s", "cpu_s", "cpu_ns_per_op", "ops", "completed_ops",
                  "completion_errors", "invalid_state"):
        if field not in row:
            bad(f"missing field {field}")
            return
    if row["arm"] not in ARMS:
        bad(f"unknown arm {row['arm']!r}")
        return
    cfg = config_key(row)
    if cfg not in CONFIG_PHASES:
        bad(f"unknown config {cfg}")
        return
    if row["phase"] not in CONFIG_PHASES[cfg]:
        bad(f"illegal phase {row['phase']!r} for config {cfg}")
        return
    if not isinstance(row["rep"], int) or not 1 <= row["rep"] <= REPS:
        bad(f"rep out of range: {row['rep']!r}")
        return
    if row["ok"] is not True:
        bad(f"oracle gate failed: ok={row['ok']!r} failure={row.get('failure')!r}")
    if row["requests"] <= 0 or row["ops"] != row["requests"]:
        bad(f"ops {row['ops']} != requests {row['requests']}")
    if row["completed_ops"] != row["requests"]:
        bad(f"completed_ops {row['completed_ops']} != requests {row['requests']}")
    if row["completion_errors"] != 0 or row["invalid_state"] != 0:
        bad(f"completion_errors={row['completion_errors']} "
            f"invalid_state={row['invalid_state']}")
    if not row["wall_s"] > 0 or not row["cpu_s"] > 0 or not row["cpu_ns_per_op"] > 0:
        bad(f"illegal measurement wall_s={row['wall_s']} cpu_s={row['cpu_s']} "
            f"cpu_ns_per_op={row['cpu_ns_per_op']}")


def load_rows(path, reasons):
    rows = []
    with open(path, "r", encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as exc:
                reasons.append(f"line {lineno}: unparseable JSON ({exc})")
    return rows


def expected_cells():
    cells = set()
    for cfg, phases in CONFIG_PHASES.items():
        for arm in ARMS:
            for rep in range(1, REPS + 1):
                for phase in phases:
                    cells.add((arm, rep, cfg, phase))
    return cells


def audit_data(path):
    reasons = []
    rows = load_rows(path, reasons)
    seen = {}
    for idx, row in enumerate(rows):
        where = f"row {idx + 1}"
        check_row(row, reasons, where)
        if row["arm"] in ARMS and isinstance(row.get("rep"), int) and \
                config_key(row) in CONFIG_PHASES and \
                isinstance(row.get("phase"), str):
            key = (row["arm"], row["rep"], config_key(row), row["phase"])
            if key in seen:
                reasons.append(f"{where}: duplicate cell {key} (first at row "
                               f"{seen[key] + 1})")
            else:
                seen[key] = idx
    missing = expected_cells() - set(seen)
    for cell in sorted(missing):
        reasons.append(f"missing cell arm={cell[0]} rep={cell[1]} "
                       f"config={cell[2]} phase={cell[3]}")
    verdicts = {}
    if not missing:
        for cfg in CONFIG_PHASES:
            ratios = []
            for rep in range(1, REPS + 1):
                base = rows[seen[("base", rep, cfg, "timed")]]
                h2b = rows[seen[("h2b", rep, cfg, "timed")]]
                ratios.append(h2b["cpu_ns_per_op"] / base["cpu_ns_per_op"])
            above = sum(r > RATIO_LIMIT for r in ratios)
            median = statistics.median(ratios)
            verdicts[str(cfg)] = {
                "median_ratio": round(median, 6),
                "pairs_above_limit": above,
                "ratios": [round(r, 6) for r in ratios],
                "pass": median <= RATIO_LIMIT and above <= MAX_PAIRS_ABOVE,
            }
    return reasons, verdicts, len(rows)


def parse_identity(path):
    arms = {}
    current = None
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line.startswith("arm="):
                current = line[4:]
                arms[current] = {}
            elif "=" in line and current is not None:
                key, _, value = line.partition("=")
                arms[current][key] = value
    return arms


def audit_identity(path, expected_shas, reasons):
    if not os.path.exists(path):
        reasons.append(f"arm identity receipt missing: {path}")
        return {}
    arms = parse_identity(path)
    for arm in ARMS:
        if arm not in arms:
            reasons.append(f"arm identity receipt lacks arm {arm}")
            continue
        rec = arms[arm]
        want = expected_shas.get(arm, "")
        got = rec.get("worktree_sha", "")
        if want != got:
            reasons.append(f"arm {arm} worktree_sha {got} != expected {want}")
        for field in ("binary_sha256", "zero_binary_sha256", "lib_async_sha256",
                      "lib_core_sha256"):
            if not re.fullmatch(r"[0-9a-f]{64}", rec.get(field, "")):
                reasons.append(f"arm {arm} {field} missing or malformed")
    if ARMS[0] in arms and ARMS[1] in arms:
        if arms[ARMS[0]].get("lib_async_sha256") == arms[ARMS[1]].get("lib_async_sha256"):
            reasons.append("both arms link an identical libsluice_async.a; "
                           "the measurement is not an A/B")
    return arms


def audit(jsonl_path, identity_path, expected_shas):
    reasons, verdicts, nrows = audit_data(jsonl_path)
    arms = audit_identity(identity_path, expected_shas, reasons)
    overall = "PASS" if not reasons and verdicts and \
        all(v["pass"] for v in verdicts.values()) else "FAIL"
    return {
        "overall": overall,
        "rule": {
            "reps": REPS,
            "median_ratio_limit": RATIO_LIMIT,
            "max_pairs_above_limit": MAX_PAIRS_ABOVE,
            "note": "one-sided binomial tail P(X>=11|n=15,p=0.5)=0.059; the "
                    "pair threshold is an engineering bound, not an exact 5% "
                    "test",
        },
        "rows_checked": nrows,
        "arms": {arm: arms.get(arm, {}) for arm in ARMS},
        "configs": verdicts,
        "reasons": reasons[:50],
        "reason_count": len(reasons),
    }


# --- counterfactual fixtures -------------------------------------------------

def synthetic_rows(h2b_scale=1.0):
    rows = []
    value = 1000.0
    for rep in range(1, REPS + 1):
        for cfg, phases in CONFIG_PHASES.items():
            block, depth, capacity = cfg
            requests = 20000
            for arm in ARMS:
                scale = h2b_scale if arm == "h2b" else 1.0
                for phase in sorted(phases):
                    ns = value + rep + sum(cfg) * (0.01 if phase == "timed" else 0)
                    rows.append({
                        "arm": arm, "rep": rep, "block": block, "depth": depth,
                        "capacity": capacity, "requests": requests,
                        "phase": phase, "ok": True, "wall_s": 0.1,
                        "cpu_s": ns * requests / 1e9,
                        "cpu_ns_per_op": ns * scale, "ops": requests,
                        "completed_ops": requests, "completion_errors": 0,
                        "invalid_state": 0, "failure": "",
                    })
    return rows


def write_jsonl(rows, handle):
    for row in rows:
        handle.write(json.dumps(row) + "\n")
    handle.flush()


def selftest():
    def run(rows, identity=None):
        with tempfile.TemporaryDirectory() as tmp:
            jsonl = os.path.join(tmp, "ab.jsonl")
            with open(jsonl, "w", encoding="utf-8") as fh:
                write_jsonl(rows, fh)
            ident = identity or os.path.join(tmp, "arm-identity.txt")
            if identity is None:
                with open(ident, "w", encoding="utf-8") as fh:
                    for arm, sha in (("base", "a" * 40), ("h2b", "b" * 40)):
                        fh.write(f"arm={arm}\nworktree_sha={sha}\n"
                                 "binary_sha256=" + "0" * 64 + "\n"
                                 "zero_binary_sha256=" + "0" * 64 + "\n"
                                 "lib_async_sha256=" +
                                 ("1" * 64 if arm == "base" else "2" * 64) + "\n"
                                 "lib_core_sha256=" + "0" * 64 + "\n")
            return audit(jsonl, ident, {"base": "a" * 40, "h2b": "b" * 40})

    def expect(name, result, want_overall, want_reason=None):
        ok = result["overall"] == want_overall
        if ok and want_reason is not None:
            ok = any(want_reason in r for r in result["reasons"])
        print(f"{'ok' if ok else 'FAIL'} {name} -> {result['overall']}")
        if not ok:
            print(json.dumps(result["reasons"][:5], indent=1))
        return ok

    good = all([
        expect("clean_dataset_passes", run(synthetic_rows()), "PASS"),
        expect("regression_fails", run(synthetic_rows(h2b_scale=1.2)), "FAIL"),
    ])
    cases = []
    rows = synthetic_rows()
    cases.append(("missing_row_fails", rows[:-1], None, "missing cell"))
    cases.append(("duplicate_row_fails", rows + [rows[0]], None, "duplicate cell"))
    oracle_row = dict(rows[0], ok=False, failure="byte oracle mismatch")
    cases.append(("failed_oracle_fails", [oracle_row] + rows[1:], None,
                  "oracle gate failed"))
    illegal_row = dict(rows[0], cpu_ns_per_op=0.0)
    cases.append(("illegal_measurement_fails", [illegal_row] + rows[1:], None,
                  "illegal measurement"))
    unknown_row = dict(rows[0], capacity=999)
    cases.append(("unknown_config_fails", [unknown_row] + rows[1:], None,
                  "unknown config"))
    for name, mutated, ident, reason in cases:
        good &= expect(name, run(mutated, ident), "FAIL", reason)

    same_arm = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False)
    same_arm.write("arm=base\nworktree_sha=" + "a" * 40 + "\n"
                   "binary_sha256=" + "0" * 64 + "\n"
                   "zero_binary_sha256=" + "0" * 64 + "\n"
                   "lib_async_sha256=" + "1" * 64 + "\n"
                   "lib_core_sha256=" + "0" * 64 + "\n"
                   "arm=h2b\nworktree_sha=" + "a" * 40 + "\n"
                   "binary_sha256=" + "0" * 64 + "\n"
                   "zero_binary_sha256=" + "0" * 64 + "\n"
                   "lib_async_sha256=" + "1" * 64 + "\n"
                   "lib_core_sha256=" + "0" * 64 + "\n")
    same_arm.close()
    try:
        good &= expect("same_arm_identity_fails",
                       run(synthetic_rows(), same_arm.name), "FAIL",
                       "not an A/B")
        good &= expect("sha_mismatch_fails",
                       audit_via_expected_override(synthetic_rows()),
                       "FAIL", "worktree_sha")
    finally:
        os.unlink(same_arm.name)

    print("reducer selftest:", "PASS" if good else "FAIL")
    return 0 if good else 1


def audit_via_expected_override(rows):
    with tempfile.TemporaryDirectory() as tmp:
        jsonl = os.path.join(tmp, "ab.jsonl")
        ident = os.path.join(tmp, "arm-identity.txt")
        with open(jsonl, "w", encoding="utf-8") as fh:
            write_jsonl(rows, fh)
        with open(ident, "w", encoding="utf-8") as fh:
            for arm in ARMS:
                fh.write(f"arm={arm}\nworktree_sha={'a' * 40}\n"
                         "binary_sha256=" + "0" * 64 + "\n"
                         "zero_binary_sha256=" + "0" * 64 + "\n"
                         "lib_async_sha256=" + "1" * 64 + "\n"
                         "lib_core_sha256=" + "0" * 64 + "\n")
        return audit(jsonl, ident,
                     {"base": "a" * 40, "h2b": "c" * 40})


def main(argv):
    if "--selftest" in argv:
        return selftest()
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("jsonl")
    ap.add_argument("identity")
    ap.add_argument("--expect-sha", action="append", default=[],
                    metavar="ARM=SHA")
    ap.add_argument("--out", default=None)
    args = ap.parse_args(argv)
    expected = {}
    for item in args.expect_sha:
        arm, _, sha = item.partition("=")
        expected[arm] = sha
    result = audit(args.jsonl, args.identity, expected)
    out_path = args.out or os.path.join(os.path.dirname(args.jsonl),
                                        "verdict.json")
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(result, fh, indent=1, sort_keys=True)
        fh.write("\n")
    print(f"verdict: {result['overall']} ({result['rows_checked']} rows, "
          f"{result['reason_count']} reasons) -> {out_path}")
    for cfg, v in sorted(result["configs"].items()):
        print(f"  {cfg}: median={v['median_ratio']} "
              f"above={v['pairs_above_limit']}/{REPS} "
              f"{'pass' if v['pass'] else 'FAIL'}")
    for reason in result["reasons"][:10]:
        print(f"  reason: {reason}")
    return 0 if result["overall"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
