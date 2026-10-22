#!/usr/bin/env python3
"""E1 product acceptance reducer (gate R6).

Versioned successor of the frozen research reducer
``docs/research/mw-hw-1/native-linux-e1/scripts/e1_verdict.py``. The
research reducer rank-pairs W-03 medians (``zip(sorted(e1), sorted(base))``),
which is inconsistent with the preregistered per-repetition statistic; this
reducer pairs every campaign (W-02, W-03 and the seam/P>0 bound) strictly by
replication ID, validates rotation, identity, oracle and honesty gates, and
emits a machine-readable verdict.

Exit codes: 0 = formal verdict issued and every gate PASS; 1 = at least one
gate failed or an input was rejected (``issues`` carries the reasons).
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections import defaultdict
from pathlib import Path

SCHEMA = "sluice-e1-product-verdict/1"

HEADLINE_CFGS = ("4096-1024-1", "4096-1024-8")
HEADLINE_MEDIAN_MAX = 0.95
HEADLINE_WIN_MIN = 12
OTHER_MEDIAN_MAX = 1.03
SEAM_MEDIAN_MAX = 1.05
W03_MEDIAN_MAX = 1.03
SEAM_PHASES = ("low_p", "high_p", "repeated_cancel", "high_rotation")


def load_jsonl(path: Path) -> list[dict]:
    rows: list[dict] = []
    with path.open() as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: invalid JSON: {exc}") from exc
    return rows


def median(values: list[float]) -> float:
    ordered = sorted(values)
    n = len(ordered)
    if n == 0:
        raise ValueError("median of empty list")
    mid = n // 2
    if n % 2 == 1:
        return ordered[mid]
    return (ordered[mid - 1] + ordered[mid]) / 2.0


class Verdict:
    def __init__(self) -> None:
        self.issues: list[str] = []
        self.blocks: dict[str, dict] = {}

    def reject(self, reason: str) -> None:
        self.issues.append(reason)

    def ok(self) -> bool:
        return not self.issues


def check_arms_and_cells(rows: list[dict], verdict: Verdict, label: str,
                         phases: tuple[str, ...] = ("timed",),
                         expect_reps: int = 15) -> dict[tuple[str, str, str], dict]:
    """Validate row shape, arms, rotation, duplicates and completeness.

    Returns {(cfg, arm, phase) -> {rep: row}}.
    """
    cells: dict[tuple[str, str, str], dict[int, dict]] = defaultdict(dict)
    for row in rows:
        for field in ("rep", "order", "cfg", "arm", "phase"):
            if field not in row:
                verdict.reject(f"{label}: row missing field {field}: {row}")
                continue
        if row.get("ok") is not True:
            verdict.reject(f"{label}: row reports ok != true: {row}")
        if row.get("phase") not in phases:
            verdict.reject(f"{label}: unknown phase {row.get('phase')}")
            continue
        if row.get("arm") not in ("base", "e1"):
            verdict.reject(f"{label}: unknown arm {row.get('arm')}")
            continue
        key = (row["cfg"], row["arm"], row["phase"])
        rep = int(row["rep"])
        if rep in cells[key]:
            verdict.reject(f"{label}: duplicate row for cfg={row['cfg']} arm={row['arm']} "
                           f"phase={row['phase']} rep={rep}")
            continue
        cells[key][rep] = row
        expected_order = "AB" if rep % 2 == 1 else "BA"
        if row["order"] != expected_order:
            verdict.reject(f"{label}: rep {rep} order {row['order']} violates AB/BA rotation")
    cfgs = {row["cfg"] for row in rows}
    for cfg in cfgs:
        for arm in ("base", "e1"):
            for phase in phases:
                have = set(cells.get((cfg, arm, phase), {}).keys())
                if len(have) != expect_reps:
                    missing = sorted(set(range(1, expect_reps + 1)) - have)
                    verdict.reject(f"{label}: cfg={cfg} arm={arm} phase={phase} has "
                                   f"{len(have)} reps, expected {expect_reps}; missing={missing[:5]}")
    return cells


def paired_ratios(cells: dict, cfg: str, phase: str = "timed") -> list[float]:
    """True per-rep pairing: E1 row of rep i over BASE row of rep i."""
    base = cells[(cfg, "base", phase)]
    e1 = cells[(cfg, "e1", phase)]
    ratios = []
    for rep in sorted(base.keys() & e1.keys()):
        b = float(base[rep]["cpu_ns_per_op"])
        e = float(e1[rep]["cpu_ns_per_op"])
        if not (math.isfinite(b) and b > 0 and math.isfinite(e) and e > 0):
            raise ValueError(f"non-finite cpu_ns_per_op at cfg={cfg} rep={rep}")
        ratios.append(e / b)
    return ratios


def check_identity(provenance: dict[str, str], verdict: Verdict) -> None:
    base_async = provenance.get("lib_async_base_sha256")
    e1_async = provenance.get("lib_async_e1_sha256")
    if not base_async or not e1_async:
        verdict.reject("identity: provenance is missing lib_async hashes")
        return
    if base_async == e1_async:
        verdict.reject("identity: both arms link the same libsluice_async.a (same-arm comparison)")
    for field in ("pipeline_base_sha256", "pipeline_e1_sha256", "fixture_sha256",
                  "extloop_base_sha256", "extloop_e1_sha256"):
        if not provenance.get(field):
            verdict.reject(f"identity: provenance missing {field}")


def read_provenance(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in path.read_text().splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            out[key.strip()] = value.strip()
    return out


def reduce_w02(rows: list[dict], verdict: Verdict) -> dict:
    cells = check_arms_and_cells(rows, verdict, "w02")
    block: dict[str, dict] = {}
    for cfg in sorted({row["cfg"] for row in rows}):
        if verdict.issues:
            break
        try:
            ratios = paired_ratios(cells, cfg)
        except ValueError as exc:
            verdict.reject(f"w02: {exc}")
            break
        med = median(ratios)
        wins = sum(1 for r in ratios if r < 1.0)
        if cfg in HEADLINE_CFGS:
            gate = med <= HEADLINE_MEDIAN_MAX and wins >= HEADLINE_WIN_MIN
        else:
            gate = med <= OTHER_MEDIAN_MAX
        if not gate:
            verdict.reject(f"w02: cfg={cfg} median={med:.4f} wins={wins}/{len(ratios)} "
                           f"fails its gate")
        block[cfg] = {"median_paired_ratio": round(med, 4),
                      "wins_below_one": wins, "pairs": len(ratios),
                      "ratios": [round(r, 4) for r in ratios]}
    return block


def reduce_w03(rows: list[dict], verify_rows: list[dict], verdict: Verdict) -> dict:
    for row in rows:
        if row.get("loop_kind") != "epoll+eventfd":
            verdict.reject(f"w03: row without a real epoll+eventfd loop: loop_kind="
                           f"{row.get('loop_kind')!r}")
    cells = check_arms_and_cells(rows, verdict, "w03")
    block: dict[str, dict] = {}
    for cfg in sorted({row["cfg"] for row in rows}):
        if verdict.issues:
            break
        try:
            ratios = paired_ratios(cells, cfg)
        except ValueError as exc:
            verdict.reject(f"w03: {exc}")
            break
        med = median(ratios)
        if med > W03_MEDIAN_MAX:
            verdict.reject(f"w03: cfg={cfg} median={med:.4f} exceeds {W03_MEDIAN_MAX}")
        block[cfg] = {"median_paired_ratio": round(med, 4),
                      "pairs": len(ratios),
                      "ratios": [round(r, 4) for r in ratios]}
    verify_ok: dict[str, dict] = {}
    per_arm = defaultdict(int)
    for row in verify_rows:
        arm = row.get("arm", "unknown")
        per_arm[arm] += 1
        if row.get("no_lost_wake") is not True:
            verdict.reject(f"w03-verify: row reports no_lost_wake != true: {row}")
        if row.get("ok") is not True:
            verdict.reject(f"w03-verify: row not ok: {row}")
    for arm in ("base", "e1"):
        if per_arm.get(arm, 0) < 3:
            verdict.reject(f"w03-verify: arm {arm} has {per_arm.get(arm, 0)} verify rows, "
                           "need at least 3")
    verify_ok["verify_rows_per_arm"] = dict(per_arm)
    block["_verify"] = verify_ok
    return block


def reduce_seam(rows: list[dict], verdict: Verdict, expect_reps: int = 15,
                repeats: int = 2) -> dict:
    agg: dict[tuple[str, str, int], list[dict]] = defaultdict(list)
    for row in rows:
        if row.get("ok") is not True:
            verdict.reject(f"seam: row not ok: {row}")
            continue
        agg[(row["phase"], row["arm"], int(row["rep"]))].append(row)
    block: dict[str, dict] = {}
    for phase in SEAM_PHASES:
        ratios = []
        p_observed_e1 = 0
        p_observed_base = 0
        for rep in range(1, expect_reps + 1):
            base_rows = agg.get((phase, "base", rep), [])
            e1_rows = agg.get((phase, "e1", rep), [])
            if len(base_rows) != repeats or len(e1_rows) != repeats:
                verdict.reject(f"seam: phase={phase} rep={rep} expected {repeats} repeats per "
                               f"arm, got base={len(base_rows)} e1={len(e1_rows)}")
                continue
            b = sum(float(r["cpu_ns"]) for r in base_rows) / repeats
            e = sum(float(r["cpu_ns"]) for r in e1_rows) / repeats
            ratios.append(e / b)
            if max(int(r["p_max"]) for r in e1_rows) > 0:
                p_observed_e1 += 1
            if sum(int(r["sticky_transitions"]) for r in base_rows) > 0:
                p_observed_base += 1
            for r in e1_rows:
                if int(r["parity_violations"]) != 0:
                    verdict.reject(f"seam: parity violation recorded: {r}")
        if verdict.issues:
            break
        if p_observed_e1 != expect_reps:
            verdict.reject(f"seam: phase={phase} observed P>0 in only {p_observed_e1}/"
                           f"{expect_reps} E1 reps; a P_POSITIVE_SEAM_BOUND verdict without "
                           "observed P>0 would be fabricated")
        if p_observed_base != expect_reps:
            verdict.reject(f"seam: phase={phase} BASE retained intents in only "
                           f"{p_observed_base}/{expect_reps} reps")
        med = median(ratios)
        if med > SEAM_MEDIAN_MAX:
            verdict.reject(f"seam: phase={phase} median={med:.4f} exceeds {SEAM_MEDIAN_MAX}")
        block[phase] = {"median_paired_ratio": round(med, 4),
                        "p_positive_e1_reps": p_observed_e1,
                        "p_positive_base_reps": p_observed_base,
                        "ratios": [round(r, 4) for r in ratios]}
    return block


def run(args: argparse.Namespace) -> int:
    verdict = Verdict()
    provenance = read_provenance(Path(args.provenance))
    check_identity(provenance, verdict)

    w02_rows = load_jsonl(Path(args.w02))
    w03_rows = load_jsonl(Path(args.w03))
    w03_verify = load_jsonl(Path(args.w03_verify))
    seam_rows = load_jsonl(Path(args.seam))

    w02_block = reduce_w02(w02_rows, verdict)
    w03_block = reduce_w03(w03_rows, w03_verify, verdict)
    seam_block = reduce_seam(seam_rows, verdict)

    out = {
        "schema": SCHEMA,
        "pairing": "per-replication-id (E1 rep i over BASE rep i, same cfg/phase)",
        "gates": {
            "w02_headline_median_max": HEADLINE_MEDIAN_MAX,
            "w02_headline_min_wins": HEADLINE_WIN_MIN,
            "w02_other_median_max": OTHER_MEDIAN_MAX,
            "w03_median_max": W03_MEDIAN_MAX,
            "seam_median_max": SEAM_MEDIAN_MAX,
        },
        "identity": {
            "base_sha": provenance.get("base_sha"),
            "e1_sha": provenance.get("e1_sha"),
            "lib_async_base_sha256": provenance.get("lib_async_base_sha256"),
            "lib_async_e1_sha256": provenance.get("lib_async_e1_sha256"),
        },
        "w02": w02_block,
        "w03": w03_block,
        "seam_p_positive_bound": seam_block,
        "p_positive_native": "NOT_MEASURED",
        "issues": verdict.issues,
    }
    out["overall"] = "PASS" if verdict.ok() else "FAIL"

    out_path = Path(args.out)
    for input_path in (args.w02, args.w03, args.w03_verify, args.seam):
        if out_path.resolve() == Path(input_path).resolve():
            print(f"refusing to write verdict over input {input_path}", file=sys.stderr)
            return 1
    out_path.write_text(json.dumps(out, indent=2, sort_keys=False) + "\n")
    print(json.dumps({"overall": out["overall"], "issues": verdict.issues}, indent=2))
    return 0 if verdict.ok() else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--w02", required=True)
    parser.add_argument("--w03", required=True)
    parser.add_argument("--w03-verify", required=True)
    parser.add_argument("--seam", required=True)
    parser.add_argument("--provenance", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    try:
        return run(args)
    except (ValueError, OSError) as exc:
        print(f"rejected: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
