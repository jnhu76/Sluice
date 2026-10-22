#!/usr/bin/env python3
"""Counterfactual selftest for e1_product_verdict.py (gate R6).

Each case builds a synthetic campaign, runs the reducer's reduction logic on
it, and asserts the expected verdict. The load-bearing case is the pairing
discriminator: inputs whose per-arm marginal distributions are identical but
whose cross-rep correspondence differs must produce the per-rep-paired
median (and the correct gate decision), not the rank-paired median a
``zip(sorted, sorted)`` reducer would compute.
"""

from __future__ import annotations

import copy
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import e1_product_verdict as reducer  # noqa: E402

REPS = 15
W02_CFGS = ["4096-1024-1", "4096-1024-8", "4096-16-1", "1048576-64-8"]
W03_CFGS = ["4096-1024-1", "4096-1024-8", "65536-1024-8"]


def w02_row(rep: int, cfg: str, arm: str, cpu: float) -> dict:
    order = "AB" if rep % 2 == 1 else "BA"
    return {"rep": rep, "order": order, "cfg": cfg, "arm": arm, "phase": "timed",
            "ok": True, "cpu_ns_per_op": cpu, "completion_errors": 0,
            "invalid_state_rejections": 0}


def w03_row(rep: int, cfg: str, arm: str, cpu: float, loop_kind: str = "epoll+eventfd") -> dict:
    order = "AB" if rep % 2 == 1 else "BA"
    return {"rep": rep, "order": order, "cfg": cfg, "arm": arm, "phase": "timed",
            "ok": True, "cpu_ns_per_op": cpu, "loop_kind": loop_kind}


def verify_row(arm: str, ok: bool = True) -> dict:
    return {"arm": arm, "ok": ok, "no_lost_wake": ok}


def seam_row(phase: str, rep: int, arm: str, cpu: float, p_max: int,
             sticky: int = 8) -> dict:
    return {"phase": phase, "rep": rep, "arm": arm, "repeat": 0, "ok": True,
            "cpu_ns": cpu, "p_max": p_max, "sticky_transitions": sticky,
            "parity_violations": 0}


def base_campaigns():
    w02 = []
    for cfg in W02_CFGS:
        for rep in range(1, REPS + 1):
            for phase in ("timed", "oracle", "latency"):
                w02.append(dict(w02_row(rep, cfg, "base", 8000.0), phase=phase))
                w02.append(dict(w02_row(rep, cfg, "e1", 8000.0 * 0.9), phase=phase))
    w03 = []
    for cfg in W03_CFGS:
        for rep in range(1, REPS + 1):
            for phase in ("timed", "oracle"):
                w03.append(dict(w03_row(rep, cfg, "base", 8000.0), phase=phase))
                w03.append(dict(w03_row(rep, cfg, "e1", 8000.0 * 0.9), phase=phase))
    verify = [verify_row("base"), verify_row("base"), verify_row("base"),
              verify_row("e1"), verify_row("e1"), verify_row("e1")]
    seam = []
    for phase in reducer.SEAM_PHASES:
        for rep in range(1, REPS + 1):
            for repeat in (0, 1):
                seam.append(dict(seam_row(phase, rep, "base", 1000.0, 0, 8), repeat=repeat))
                seam.append(dict(seam_row(phase, rep, "e1", 970.0, 8, 8), repeat=repeat))
    return w02, w03, verify, seam


IDENTITY = {
    "lib_async_base_sha256": "a" * 64,
    "lib_async_e1_sha256": "b" * 64,
    "pipeline_base_sha256": "c" * 64,
    "pipeline_e1_sha256": "d" * 64,
    "extloop_base_sha256": "e" * 64,
    "extloop_e1_sha256": "f" * 64,
    "fixture_sha256": "9" * 64,
    "base_sha": "859e4b56",
    "e1_sha": "producthead",
}


def run_reduce(w02, w03, verify, seam, identity=None):
    verdict = reducer.Verdict()
    prov = dict(IDENTITY) if identity is None else identity
    reducer.check_identity(prov, verdict)
    w02_block = reducer.reduce_w02(w02, verdict)
    w03_block = reducer.reduce_w03(w03, verify, verdict)
    seam_block = reducer.reduce_seam(seam, verdict)
    return verdict, w02_block, w03_block, seam_block


CASES = []


def case(name):
    def register(fn):
        CASES.append((name, fn))
        return fn
    return register


@case("healthy synthetic campaign passes with per-rep medians")
def _():
    v, w02, w03, seam = run_reduce(*base_campaigns())
    assert v.ok(), v.issues
    assert w02["4096-1024-1"]["median_paired_ratio"] == 0.9


@case("pairing discriminator: per-rep median used, not rank-paired")
def _():
    w02, w03, verify, seam = base_campaigns()
    # Marginals per arm chosen so every rank-paired ratio is exactly 0.9
    # (a rank-paired reducer would PASS the headline gate), while the
    # per-rep correspondence is a cyclic shift that pushes the median
    # rep-paired ratio to ~1.20 (FAIL). Gate decisions flip; the reducer
    # must follow the rep IDs.
    cfg = "4096-1024-1"
    base_cpu = [8000.0 * (1.06 ** i) for i in range(REPS)]
    e1_cpu = [0.9 * base_cpu[(i + 5) % REPS] for i in range(REPS)]
    rows = [r for r in w02 if r["cfg"] != cfg]
    for rep in range(1, REPS + 1):
        for phase in ("timed", "oracle", "latency"):
            rows.append(dict(w02_row(rep, cfg, "base", base_cpu[rep - 1]), phase=phase))
            rows.append(dict(w02_row(rep, cfg, "e1", e1_cpu[rep - 1]), phase=phase))
    v, w02_block, _, _ = run_reduce(rows, w03, verify, seam)
    assert not v.ok(), "per-rep violation must fail"
    rep_median = sorted(e / b for e, b in zip(e1_cpu, base_cpu))[7]
    rank_median = sorted(e / b for e, b in zip(sorted(e1_cpu), sorted(base_cpu)))[7]
    assert rank_median <= 0.95 < rep_median, (
        "counterfactual must flip the gate between pairings", rep_median, rank_median)
    got = w02_block[cfg]["median_paired_ratio"]
    assert abs(got - round(rep_median, 4)) < 1e-9, (got, rep_median, rank_median)


@case("missing rep row rejected")
def _():
    w02, w03, verify, seam = base_campaigns()
    del w02[3]
    v, *_ = run_reduce(w02, w03, verify, seam)
    assert not v.ok() and any("missing" in i or "has" in i for i in v.issues), v.issues


@case("duplicate row rejected")
def _():
    w02, w03, verify, seam = base_campaigns()
    w02.append(copy.deepcopy(w02[0]))
    v, *_ = run_reduce(w02, w03, verify, seam)
    assert not v.ok() and any("duplicate" in i for i in v.issues), v.issues


@case("oracle failure row rejected")
def _():
    w02, w03, verify, seam = base_campaigns()
    w02[5]["ok"] = False
    v, *_ = run_reduce(w02, w03, verify, seam)
    assert not v.ok(), v.issues


@case("identical arm library hashes rejected")
def _():
    ident = dict(IDENTITY)
    ident["lib_async_e1_sha256"] = ident["lib_async_base_sha256"]
    v, *_ = run_reduce(*base_campaigns(), identity=ident)
    assert not v.ok() and any("same libsluice_async" in i for i in v.issues), v.issues


@case("missing provenance hash rejected")
def _():
    ident = dict(IDENTITY)
    del ident["fixture_sha256"]
    v, *_ = run_reduce(*base_campaigns(), identity=ident)
    assert not v.ok() and any("fixture" in i for i in v.issues), v.issues


@case("seam P>0 claimed without observation rejected")
def _():
    w02, w03, verify, seam = base_campaigns()
    for row in seam:
        if row["arm"] == "e1":
            row["p_max"] = 0
    v, *_ = run_reduce(w02, w03, verify, seam)
    assert not v.ok() and any("would be fabricated" in i for i in v.issues), v.issues


@case("w03 row without eventfd rejected")
def _():
    w02, w03, verify, seam = base_campaigns()
    w03[0]["loop_kind"] = "epoll"
    v, *_ = run_reduce(w02, w03, verify, seam)
    assert not v.ok() and any("epoll+eventfd" in i for i in v.issues), v.issues


@case("w03 verify no_lost_wake=false rejected")
def _():
    w02, w03, verify, seam = base_campaigns()
    verify[2] = verify_row("base", ok=False)
    v, *_ = run_reduce(w02, w03, verify, seam)
    assert not v.ok(), v.issues


@case("ab/ba rotation violation rejected")
def _():
    w02, w03, verify, seam = base_campaigns()
    w02[0]["order"] = "BA"
    v, *_ = run_reduce(w02, w03, verify, seam)
    assert not v.ok() and any("rotation" in i for i in v.issues), v.issues


@case("headline median above 0.95 rejected")
def _():
    w02, w03, verify, seam = base_campaigns()
    for row in w02:
        if row["cfg"] == "4096-1024-1" and row["arm"] == "e1":
            row["cpu_ns_per_op"] = 8000.0 * 0.96
    v, *_ = run_reduce(w02, w03, verify, seam)
    assert not v.ok() and any("4096-1024-1" in i for i in v.issues), v.issues


@case("non-headline median above 1.03 rejected")
def _():
    w02, w03, verify, seam = base_campaigns()
    for row in w02:
        if row["cfg"] == "4096-16-1" and row["arm"] == "e1":
            row["cpu_ns_per_op"] = 8000.0 * 1.04
    v, *_ = run_reduce(w02, w03, verify, seam)
    assert not v.ok() and any("4096-16-1" in i for i in v.issues), v.issues


@case("seam median above 1.05 rejected")
def _():
    w02, w03, verify, seam = base_campaigns()
    for row in seam:
        if row["arm"] == "e1":
            row["cpu_ns"] = 1000.0 * 1.06
    v, *_ = run_reduce(w02, w03, verify, seam)
    assert not v.ok() and any("seam" in i and "1.06" in i for i in v.issues), v.issues


@case("seam parity violation rejected")
def _():
    w02, w03, verify, seam = base_campaigns()
    seam[3]["parity_violations"] = 1
    v, *_ = run_reduce(w02, w03, verify, seam)
    assert not v.ok() and any("parity violation" in i for i in v.issues), v.issues


@case("w03 per-rep pairing gates a rank-pairing pass")
def _():
    # Construct w03 data where rank-paired medians sit below 1.03 but the
    # per-rep medians exceed it for one cfg; the reducer must fail.
    w02, w03, verify, seam = base_campaigns()
    cfg = "65536-1024-8"
    base_cpu = [8000.0 * (1.05 ** i) for i in range(REPS)]
    e1_cpu = [0.97 * base_cpu[(i + 7) % REPS] for i in range(REPS)]
    rows = [r for r in w03 if r["cfg"] != cfg]
    for rep in range(1, REPS + 1):
        for phase in ("timed", "oracle"):
            rows.append(dict(w03_row(rep, cfg, "base", base_cpu[rep - 1]), phase=phase))
            rows.append(dict(w03_row(rep, cfg, "e1", e1_cpu[rep - 1]), phase=phase))
    rep_median = sorted(e / b for e, b in zip(e1_cpu, base_cpu))[7]
    rank_median = sorted(e / b for e, b in zip(sorted(e1_cpu), sorted(base_cpu)))[7]
    assert rank_median < 1.03 < rep_median, (rank_median, rep_median)
    v, _, w03_block, _ = run_reduce(w02, rows, verify, seam)
    assert not v.ok() and any(cfg in i for i in v.issues), v.issues


def main() -> int:
    passed = 0
    for name, fn in CASES:
        try:
            fn()
        except AssertionError as exc:
            print(f"FAIL {name}: {exc}")
            return 1
        passed += 1
        print(f"ok {name}")
    print(f"e1_product_verdict selftest: {passed}/{len(CASES)} cases pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
