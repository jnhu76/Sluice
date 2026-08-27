#!/usr/bin/env python3
"""rx1_classify — frozen RX-1 classifiers, feature extraction, and validity.

RX-1 (#234) compares two attribution systems over the SAME runs:

  C (external-only)      : process/kernel telemetry + workload outcome +
                           universally-exposed static workload config.
  E (external + AC-1a)   : exactly C plus the nine public ThreadPoolBackend
                           resource accessors sampled by the harness observer.

Both classifiers output the same label set and are deliberately interpretable
rule systems (no ML): RX-1 tests information value, not model capacity.

Freeze discipline: THRESHOLDS_V1 + rule precedence below are frozen at
protocol freeze (research/rx1/rx1_protocol_v1.json must carry the same
numbers; rx1.py freeze cross-checks). Post-freeze edits require a protocol
version bump and a full formal rerun.

Classifier input is the feature dict produced by extract_features() — an
explicit whitelist. Ground-truth label, intervention parameters, run index,
affinity/stress metadata and output paths never enter the feature dict.
"""

from __future__ import annotations

import math

# ---------------------------------------------------------------------------
# Label set
# ---------------------------------------------------------------------------

LABELS = [
    "CONTROL",
    "APP_PIPELINE_LIMITED",
    "REQUEST_CAPACITY_LIMITED",
    "THREADPOOL_WORKER_LIMITED",
    "CPU_CONTENDED",
    "IO_SERVICE_CONTENDED",
]
CLASSIFIER_LABELS = LABELS + ["UNKNOWN"]

# ---------------------------------------------------------------------------
# Frozen threshold set (v1). Numbers finalized during pilot; after protocol
# freeze these are immutable for the formal matrix.
# ---------------------------------------------------------------------------

THRESHOLDS_V1 = {
    # --- external (C and E) ---
    # PSI cpu `some` stall increment over the run, microseconds per wall
    # second. Calibrated in pilot: ambient-under-load stays below ~65k;
    # I4 (4 stressors on the same 2 CPUs as the workload) reads 358k+.
    "psi_cpu_some_us_per_s": 250000.0,
    "psi_io_some_us_per_s": 50000.0,
    # --- C rules (external evidence only) ---
    # Caller-observed synchronous would_block rejections: any I/O library
    # surfaces these to its caller (workload outcome, not a Sluice accessor).
    "app_rejections_gt": 0,
    # Aggregate process CPU / wall in "cores worth", divided by configured
    # workers. Pilot: worker-limited (1 worker) reads ~1.5 (worker + driver);
    # balanced control reads ~0.85. Busy-ness only: an external observer
    # cannot see the dispatch backlog that separates "busy" from
    # "bottleneck" — that asymmetry is part of what RX-1 measures.
    "per_worker_util_sat": 1.10,
    # App-limited: the process as a whole consumes few cores AND the offered
    # pipeline is shallow relative to arena capacity. Pilot: app-limited
    # reads ~1.4 cores; balanced control ~3.3.
    "app_cores_used_max": 2.0,
    "app_depth_over_capacity_max": 0.10,
    # Control: healthy compute level with no refusal and no contention.
    "control_cores_used_min": 0.75,
    # --- E rules (AC-1a derived additions) ---
    # Capacity saturation: rejections observed AND arena pinned at capacity
    # in a substantial fraction of samples. Pilot: I2 reads 0.97-1.0.
    "frac_slot_at_capacity_sat": 0.30,
    # Worker saturation: workers at configured count in a substantial
    # fraction of samples AND a persistently deep dispatch queue (mean
    # occupancy; pilot: worker-limited ~28, everything else <= 6 — the
    # frac-nonzero variant was rejected as a stagger artifact at 64 KiB).
    "frac_active_at_configured_sat": 0.55,
    "dispatch_occ_mean_sat": 10.0,
    # App-limited internal signature: arena occupancy and worker activity
    # consistently low, no rejections, shared external low-compute condition.
    "e_app_slot_occ_mean_max": 0.10,
    "e_app_frac_active_max": 0.30,
    # Control internal signature: no saturation of any internal resource
    # (workers not pinned at configured count, arena not pinned at capacity).
    "e_control_frac_active_max": 0.55,
}


# ---------------------------------------------------------------------------
# Feature extraction (whitelist — the ONLY classifier input)
# ---------------------------------------------------------------------------

def extract_features(run: dict) -> dict:
    """Build the classifier feature dict from one run artifact.

    Common (C-legal) features: static workload config, workload outcome,
    process-level OS accounting, PSI deltas, perf counters when available.
    E features: the same dict plus `sluice.*` keys derived from the AC-1a
    observer aggregates. Classifier C ignores `sluice.*` by construction.
    """
    b = run["bench_json"]
    win_s = b["measured_window_ns"] / 1e9
    ops = b["ops_per_rep"] * b["reps"]
    oc = b["outcome"]
    oa = b["os_accounting"]
    psi = run["external"]["psi"]
    f: dict = {}
    # static workload configuration (exposed identically to both classifiers)
    f["op_is_write"] = 1.0 if b["op"] == "write" else 0.0
    f["request_size"] = float(b["request_size"])
    f["app_depth"] = float(b["app_depth"])
    f["capacity"] = float(b["capacity"])
    f["workers"] = float(b["workers"])
    f["depth_over_capacity"] = f["app_depth"] / f["capacity"]
    # workload outcome
    f["throughput_mbs"] = oc["throughput_mbs_median"]
    f["lat_p50_ns"] = oc["lat_p50_ns_median"]
    f["lat_p95_ns"] = oc["lat_p95_ns_median"]
    f["lat_p99_ns"] = oc["lat_p99_ns_median"]
    f["app_submit_rejections"] = float(oc["submit_rejections_total"])
    # process-level CPU (all threads) normalized to cores
    cpu_s = (oc["user_ns_sum"] + oc["sys_ns_sum"]) / 1e9
    f["cpu_cores_used"] = cpu_s / win_s if win_s > 0 else 0.0
    f["per_worker_util"] = f["cpu_cores_used"] / f["workers"]
    f["ctxt_per_kop"] = (oa["ctxt_vol_delta"] + oa["ctxt_invol_delta"]) / ops * 1000.0 if ops else 0.0
    tot_ctxt = oa["ctxt_vol_delta"] + oa["ctxt_invol_delta"]
    f["invol_ctxt_frac"] = oa["ctxt_invol_delta"] / tot_ctxt if tot_ctxt > 0 else 0.0
    rr = oa["sched_run_ns_delta"]
    f["sched_wait_per_run"] = oa["sched_wait_ns_delta"] / rr if rr > 0 else 0.0
    f["sched_slices_per_kop"] = oa["sched_slices_delta"] / ops * 1000.0 if ops else 0.0
    # PSI (system-wide) deltas normalized per wall second, microseconds
    wall_s = run["external"]["wall_s"]
    f["psi_cpu_some_us_per_s"] = psi["cpu_some_delta_us"] / wall_s if wall_s > 0 else 0.0
    f["psi_cpu_full_us_per_s"] = psi.get("cpu_full_delta_us", 0.0) / wall_s if wall_s > 0 else 0.0
    f["psi_io_some_us_per_s"] = psi.get("io_some_delta_us", 0.0) / wall_s if wall_s > 0 else 0.0
    f["psi_io_full_us_per_s"] = psi.get("io_full_delta_us", 0.0) / wall_s if wall_s > 0 else 0.0
    f["psi_mem_some_us_per_s"] = psi.get("memory_some_delta_us", 0.0) / wall_s if wall_s > 0 else 0.0
    # perf stat (optional; absent events recorded as None, treated as 0)
    perf = run["external"].get("perf") or {}
    for key, out in (
        ("cycles_per_op", "cycles_per_op"),
        ("instructions_per_op", "instructions_per_op"),
        ("task_clock_ratio", "task_clock_ratio"),
    ):
        f[out] = perf.get(key) if perf.get(key) is not None else 0.0
    f["perf_available"] = 1.0 if perf.get("cycles_per_op") is not None else 0.0
    # --- E-only: AC-1a observer aggregates ---
    so = b.get("sluice_obs")
    if so and so.get("sample_count", 0) > 0 and so.get("arena_capacity", 0) > 0:
        cap = float(so["arena_capacity"])
        cw = float(so["configured_workers"])
        f["sluice_sample_count"] = float(so["sample_count"])
        f["sluice_arena_capacity"] = cap
        f["sluice_configured_workers"] = cw
        f["sluice_slot_occ_mean"] = so["slot_in_use_mean"] / cap
        f["sluice_frac_slot_at_capacity"] = so["frac_slot_at_capacity"]
        f["sluice_frac_active_at_configured"] = so["frac_active_at_configured"]
        f["sluice_frac_dispatch_nonzero"] = so["frac_dispatch_nonzero"]
        f["sluice_dispatch_occ_mean"] = so["dispatch_occ_mean"]
        f["sluice_active_mean"] = so["active_mean"]
        f["sluice_outstanding_mean"] = so["outstanding_mean"]
        f["sluice_dispatch_occ_max"] = so["dispatch_occ_max"]
        f["sluice_arena_high_water_frac"] = so["arena_high_water_final"] / cap
        f["sluice_dispatch_high_water"] = so["dispatch_high_water_final"]
        f["sluice_rejections_delta"] = float(so["rejections_delta"])
    else:
        # No observer data: E degrades to C on these keys (0 / False).
        f["sluice_sample_count"] = 0.0
        f["sluice_slot_occ_mean"] = 0.0
        f["sluice_frac_slot_at_capacity"] = 0.0
        f["sluice_frac_active_at_configured"] = 0.0
        f["sluice_frac_dispatch_nonzero"] = 0.0
        f["sluice_dispatch_occ_mean"] = 0.0
        f["sluice_active_mean"] = 0.0
        f["sluice_outstanding_mean"] = 0.0
        f["sluice_dispatch_occ_max"] = 0.0
        f["sluice_arena_high_water_frac"] = 0.0
        f["sluice_dispatch_high_water"] = 0.0
        f["sluice_rejections_delta"] = 0.0
    return f


# ---------------------------------------------------------------------------
# Rule predicates. Each returns (fired: bool, strength: float) so the
# dispatcher can implement the UNKNOWN-on-conflict policy.
# ---------------------------------------------------------------------------

def _rule_cpu(f, T):
    return f["psi_cpu_some_us_per_s"] > T["psi_cpu_some_us_per_s"], f["psi_cpu_some_us_per_s"]


def _rule_capacity_c(f, T):
    fired = f["app_submit_rejections"] > T["app_rejections_gt"]
    return fired, f["app_submit_rejections"]


def _rule_capacity_e(f, T):
    fired = (f["sluice_rejections_delta"] > 0
             and f["sluice_frac_slot_at_capacity"] >= T["frac_slot_at_capacity_sat"])
    return fired, f["sluice_rejections_delta"]


def _rule_worker_c(f, T):
    # External proxy: aggregate process CPU divided by the configured worker
    # count. Busy-ness only — an external observer cannot see the dispatch
    # backlog that separates "workers busy" from "workers the bottleneck".
    fired = (f["per_worker_util"] >= T["per_worker_util_sat"]
             and f["psi_cpu_some_us_per_s"] <= T["psi_cpu_some_us_per_s"]
             and f["app_submit_rejections"] <= T["app_rejections_gt"])
    return fired, f["per_worker_util"]


def _rule_worker_e(f, T):
    # Worker saturation = workers pinned at the configured count AND a
    # persistently deep dispatch queue (mean occupancy). Rejections would
    # prove the arena is the constraint and suppress this rule.
    fired = (f["sluice_frac_active_at_configured"] >= T["frac_active_at_configured_sat"]
             and f["sluice_dispatch_occ_mean"] >= T["dispatch_occ_mean_sat"]
             and f["psi_cpu_some_us_per_s"] <= T["psi_cpu_some_us_per_s"]
             and f["sluice_rejections_delta"] == 0)
    return fired, f["sluice_frac_active_at_configured"]


def _rule_app_c(f, T):
    fired = (f["cpu_cores_used"] <= T["app_cores_used_max"]
             and f["depth_over_capacity"] <= T["app_depth_over_capacity_max"]
             and f["app_submit_rejections"] <= T["app_rejections_gt"])
    return fired, 1.0


def _rule_app_e(f, T):
    # The external low-compute condition is shared with C deliberately: at
    # request sizes where the single driving fiber is the limiter, internal
    # emptiness alone must not relabel a balanced control as app-limited.
    fired = (f["cpu_cores_used"] <= T["app_cores_used_max"]
             and f["sluice_slot_occ_mean"] <= T["e_app_slot_occ_mean_max"]
             and f["sluice_frac_active_at_configured"] <= T["e_app_frac_active_max"]
             and f["sluice_rejections_delta"] == 0)
    return fired, 1.0


def _rule_control_c(f, T):
    fired = (f["cpu_cores_used"] >= T["control_cores_used_min"]
             and f["app_submit_rejections"] <= T["app_rejections_gt"]
             and f["psi_cpu_some_us_per_s"] <= T["psi_cpu_some_us_per_s"])
    return fired, 1.0


def _rule_control_e(f, T):
    fired = (f["cpu_cores_used"] >= T["control_cores_used_min"]
             and f["sluice_frac_active_at_configured"] < T["e_control_frac_active_max"]
             and f["sluice_frac_slot_at_capacity"] < T["frac_slot_at_capacity_sat"]
             and f["sluice_rejections_delta"] == 0)
    return fired, 1.0


def _rule_io(f, T):
    return f["psi_io_some_us_per_s"] > T["psi_io_some_us_per_s"], f["psi_io_some_us_per_s"]


def _classify(f: dict, T: dict, use_sluice: bool) -> str:
    cpu = _rule_cpu(f, T)
    cap = _rule_capacity_e(f, T) if use_sluice else _rule_capacity_c(f, T)
    wrk = _rule_worker_e(f, T) if use_sluice else _rule_worker_c(f, T)
    app = _rule_app_e(f, T) if use_sluice else _rule_app_c(f, T)
    ctl = _rule_control_e(f, T) if use_sluice else _rule_control_c(f, T)
    io = _rule_io(f, T)
    if not any(r[0] for r in (cpu, cap, wrk, app, ctl, io)):
        return "UNKNOWN"
    if cpu[0]:
        # CPU contention legitimately produces secondary internal queueing
        # (workers starve, dispatch backs up): CPU outranks internal rules.
        return "CPU_CONTENDED"
    if cap[0] and wrk[0]:
        # Two incompatible internal causes simultaneously strong.
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


def classify_c(f: dict, T: dict | None = None) -> str:
    return _classify(f, T or THRESHOLDS_V1, use_sluice=False)


def classify_e(f: dict, T: dict | None = None) -> str:
    return _classify(f, T or THRESHOLDS_V1, use_sluice=True)


def fired_rules(f: dict, T: dict | None = None) -> dict:
    """Diagnostics: which rules fired (failure-case analysis support)."""
    T = T or THRESHOLDS_V1
    return {
        "cpu": _rule_cpu(f, T)[0],
        "capacity_c": _rule_capacity_c(f, T)[0],
        "capacity_e": _rule_capacity_e(f, T)[0],
        "worker_c": _rule_worker_c(f, T)[0],
        "worker_e": _rule_worker_e(f, T)[0],
        "app_c": _rule_app_c(f, T)[0],
        "app_e": _rule_app_e(f, T)[0],
        "control_c": _rule_control_c(f, T)[0],
        "control_e": _rule_control_e(f, T)[0],
        "io": _rule_io(f, T)[0],
    }


# ---------------------------------------------------------------------------
# Ground-truth validity checks (experiment validity, NOT classifier output).
# May observe everything the harness recorded, including Sluice accessors.
# ---------------------------------------------------------------------------

def ground_truth_valid(run: dict) -> tuple[bool, str]:
    label = run["ground_truth_label"]
    b = run["bench_json"]
    so = b.get("sluice_obs") or {}
    f = extract_features(run)
    w = run["workload"]
    cap = w.get("request_capacity", w.get("capacity"))
    depth = w.get("pipeline_depth", w.get("app_depth"))
    workers = w.get("configured_workers", w.get("workers"))
    if not run.get("correctness_pass", False):
        return False, "correctness failure"
    if b.get("observe_interval_ms", 0) == 0 or so.get("sample_count", 0) == 0:
        return False, "no observer samples (OBS-OFF run cannot be validity-checked)"
    if label == "CONTROL":
        if so.get("rejections_delta", 0) != 0:
            return False, "control run saw capacity rejections"
        if so.get("frac_slot_at_capacity", 0.0) >= 0.30:
            return False, "control run arena persistently at capacity"
        if f["sluice_frac_active_at_configured"] >= 0.55:
            return False, "control run workers pathologically saturated"
        if f["psi_cpu_some_us_per_s"] > 250000.0:
            return False, "control run saw CPU pressure above gate"
        return True, ""
    if label == "APP_PIPELINE_LIMITED":
        if so.get("rejections_delta", 0) != 0:
            return False, "app-limited run saw capacity rejections"
        if f["sluice_frac_slot_at_capacity"] >= 0.30:
            return False, "arena saturated in app-limited run"
        if f["sluice_frac_active_at_configured"] >= 0.55:
            return False, "workers saturated in app-limited run"
        if depth > 4:
            return False, "injected pipeline depth not low"
        return True, ""
    if label == "REQUEST_CAPACITY_LIMITED":
        if so.get("rejections_delta", 0) <= 0:
            return False, "no capacity rejections observed"
        if so.get("arena_high_water_final", 0) != b["capacity"]:
            return False, "arena high-water did not reach capacity"
        if cap >= depth:
            return False, "capacity not below offered depth"
        return True, ""
    if label == "THREADPOOL_WORKER_LIMITED":
        if f["sluice_frac_active_at_configured"] < 0.50:
            return False, "active-worker saturation not observed"
        if f["sluice_frac_dispatch_nonzero"] < 0.25:
            return False, "dispatch queueing not observed"
        if so.get("rejections_delta", 0) > 0 and f["sluice_frac_slot_at_capacity"] >= 0.30:
            return False, "capacity rejection dominates in worker-limited run"
        if workers > depth:
            return False, "workers not fewer than offered depth"
        return True, ""
    if label == "CPU_CONTENDED":
        st = run["intervention_parameters"].get("stress")
        if not st or not st.get("ran"):
            return False, "stressor did not run"
        if f["psi_cpu_some_us_per_s"] <= 10000.0:
            return False, "no CPU competition evidence (PSI below gate)"
        return True, ""
    if label == "IO_SERVICE_CONTENDED":
        return False, "IO_SERVICE_CONTENDED deferred in this environment"
    return False, f"unknown label {label}"


# ---------------------------------------------------------------------------
# Scoring helpers
# ---------------------------------------------------------------------------

def confusion(pairs: list[tuple[str, str]], labels=CLASSIFIER_LABELS) -> dict:
    """pairs: (predicted, true) over VALID runs. Returns row=true matrix."""
    m = {t: {p: 0 for p in labels} for t in labels}
    for pred, true in pairs:
        m.setdefault(true, {}).setdefault(pred, 0)
        m[true][pred] += 1
    return m


def accuracy(pairs: list[tuple[str, str]]) -> float:
    if not pairs:
        return float("nan")
    ok = sum(1 for p, t in pairs if p == t)
    return ok / len(pairs)


def per_class(pairs: list[tuple[str, str]]):
    """precision/recall/f1 per class; UNKNOWN counted as its own class."""
    labels = sorted({t for _, t in pairs} | {p for p, _ in pairs})
    out = {}
    for L in labels:
        tp = sum(1 for p, t in pairs if p == L and t == L)
        fp = sum(1 for p, t in pairs if p == L and t != L)
        fn = sum(1 for p, t in pairs if p != L and t == L)
        prec = tp / (tp + fp) if tp + fp else 0.0
        rec = tp / (tp + fn) if tp + fn else 0.0
        f1 = 2 * prec * rec / (prec + rec) if prec + rec else 0.0
        out[L] = {"precision": prec, "recall": rec, "f1": f1, "tp": tp, "fp": fp, "fn": fn}
    macro_f1 = sum(v["f1"] for v in out.values()) / len(out) if out else 0.0
    return out, macro_f1


def paired_bootstrap_delta(pairs_c, pairs_e, iters=10000, seed=1701):
    """Paired bootstrap for Δaccuracy = acc(E) − acc(C) over matched runs.

    pairs_c/pairs_e are aligned lists (same index = same run). Returns
    (point_estimate, lo95, hi95).
    """
    assert len(pairs_c) == len(pairs_e)
    n = len(pairs_c)
    acc_c = accuracy(pairs_c)
    acc_e = accuracy(pairs_e)
    if n == 0:
        return float("nan"), float("nan"), float("nan")
    import random

    rng = random.Random(seed)
    deltas = []
    for _ in range(iters):
        idx = [rng.randrange(n) for _ in range(n)]
        dc = [pairs_c[i] for i in idx]
        de = [pairs_e[i] for i in idx]
        deltas.append(accuracy(de) - accuracy(dc))
    deltas.sort()

    def pct(p):
        k = min(len(deltas) - 1, max(0, int(round(p * (len(deltas) - 1)))))
        return deltas[k]

    return acc_e - acc_c, pct(0.025), pct(0.975)


def paired_transitions(pairs_c, pairs_e) -> dict:
    """C→E outcome transitions over matched valid runs."""
    t = {
        "wrong_to_right": 0,
        "unknown_to_right": 0,
        "right_to_wrong": 0,
        "right_to_unknown": 0,
        "right_to_right": 0,
        "wrong_to_wrong": 0,
        "unknown_to_unknown": 0,
        "wrong_to_unknown": 0,
        "unknown_to_wrong": 0,
    }
    for (pc, tc), (pe, te) in zip(pairs_c, pairs_e):
        assert tc == te
        c_right, e_right = pc == tc, pe == te
        if not c_right and e_right:
            t["wrong_to_right" if pc != "UNKNOWN" else "unknown_to_right"] += 1
        elif c_right and not e_right:
            t["right_to_unknown" if pe == "UNKNOWN" else "right_to_wrong"] += 1
        elif c_right and e_right:
            t["right_to_right"] += 1
        elif not c_right and not e_right:
            if pc == "UNKNOWN" and pe == "UNKNOWN":
                t["unknown_to_unknown"] += 1
            elif pe == "UNKNOWN":
                t["wrong_to_unknown"] += 1
            elif pc == "UNKNOWN":
                t["unknown_to_wrong"] += 1
            else:
                t["wrong_to_wrong"] += 1
    return t
