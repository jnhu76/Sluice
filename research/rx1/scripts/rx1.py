#!/usr/bin/env python3
"""rx1 — RX-1 controlled attribution falsification gate orchestrator (#234).

One command surface for the whole experiment (book §32):

    rx1.py env                     # environment fingerprint
    rx1.py run --phase pilot [...] # calibration runs (NOT formal data)
    rx1.py freeze                  # protocol consistency + hash printout
    rx1.py run --phase formal      # frozen formal matrix (needs protocol)
    rx1.py classify [--phase ...]  # run frozen classifiers + validity
    rx1.py analyze                 # scores, confusion, bootstrap, tax, verdict
    rx1.py self-test               # synthetic classifier + scorer tests

Design constraints (task brief):
  * production code untouched — this script only adds research artifacts;
  * classifiers read ONLY the extract_features() whitelist;
  * formal phase refuses to run without the committed frozen protocol;
  * randomization reproducible from the protocol seed;
  * raw run artifacts are immutable; scoring writes *.scored.json siblings.
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import platform
import random
import shutil
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
RX1 = REPO / "research" / "rx1"
sys.path.insert(0, str(RX1 / "scripts"))

import rx1_classify as rc  # noqa: E402

SCHEMA_VERSION = 1
PROTOCOL_VERSION = 1
PROTOCOL_PATH = RX1 / "rx1_protocol_v1.json"
RESULTS = RX1 / "results"

# ---------------------------------------------------------------------------
# Interventions (one primary constrained resource each; non-target resources
# generous). Static config goes to BOTH classifiers via features; affinity and
# stress parameters are intervention metadata and NEVER classifier features.
# ---------------------------------------------------------------------------

INTERVENTIONS = {
    "I0_CONTROL": dict(depth=16, capacity=64, workers=4, stress=None),
    "I1_APP_PIPELINE_LIMITED": dict(depth=2, capacity=64, workers=4, stress=None),
    "I2_REQUEST_CAPACITY_LIMITED": dict(depth=32, capacity=4, workers=4, stress=None),
    "I3_THREADPOOL_WORKER_LIMITED": dict(depth=32, capacity=64, workers=1, stress=None),
    "I4_CPU_CONTENDED": dict(
        depth=16, capacity=64, workers=4,
        stress=dict(cpus="0,1", count=4, mechanism="taskset+pinned busy loop"),
    ),
}
LABEL_OF = {k: v for k, v in (
    ("I0_CONTROL", "CONTROL"),
    ("I1_APP_PIPELINE_LIMITED", "APP_PIPELINE_LIMITED"),
    ("I2_REQUEST_CAPACITY_LIMITED", "REQUEST_CAPACITY_LIMITED"),
    ("I3_THREADPOOL_WORKER_LIMITED", "THREADPOOL_WORKER_LIMITED"),
    ("I4_CPU_CONTENDED", "CPU_CONTENDED"),
)}
# I5 IO_SERVICE_CONTENDED: DEFERRED — workload files live on tmpfs (no device
# service to contend), iostat is unavailable, and WSL2 virtual storage makes
# device-level ground truth ambiguous. ENVIRONMENT INVALID / DEFERRED.

WORKDIR = Path("/tmp/rx1")
STRESS_PY = "import sys\nwhile True: pass\n"


# ---------------------------------------------------------------------------
# Environment fingerprint
# ---------------------------------------------------------------------------

def sh(cmd: list[str]) -> str:
    import re as _re
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout.strip()
        return _re.sub(r"\x1b\[[0-9;]*m", "", out)
    except Exception:
        return ""


def read_first(path: str) -> str:
    try:
        with open(path) as f:
            return f.read().strip()
    except Exception:
        return ""


def env_fingerprint() -> dict:
    wsl = "microsoft" in read_first("/proc/version").lower()
    cpu_model = ""
    ncpu = os.cpu_count() or 0
    for line in read_first("/proc/cpuinfo").splitlines():
        if line.startswith("model name"):
            cpu_model = line.split(":", 1)[1].strip()
            break
    git_sha = sh(["git", "-C", str(REPO), "rev-parse", "HEAD"])
    dirty = bool(sh(["git", "-C", str(REPO), "status", "--short"]))
    return {
        "git_sha": git_sha,
        "dirty": dirty,
        "kernel": platform.release(),
        "distro": sh(["sh", "-c", ". /etc/os-release && echo $PRETTY_NAME"]),
        "wsl2": wsl,
        "environment_class": "WSL2" if wsl else "NATIVE_LINUX",
        "cpu_model": cpu_model,
        "logical_cpus": ncpu,
        "memory": read_first("/proc/meminfo").splitlines()[0].split(":", 1)[1].strip() if read_first("/proc/meminfo") else "",
        "compiler": sh(["clang", "--version"]).splitlines()[0],
        "xmake": sh(["xmake", "--version"]).splitlines()[0].strip(),
        "perf": sh(["perf", "--version"]),
        "iostat": "unavailable" if not shutil.which("iostat") else sh(["iostat", "--version"]).splitlines()[0],
        "psi": {r: os.path.exists(f"/proc/pressure/{r}") for r in ("cpu", "io", "memory")},
        "perf_event_paranoid": read_first("/proc/sys/kernel/perf_event_paranoid") or "unreadable",
        "cgroup": "v2" if os.path.exists("/sys/fs/cgroup/cgroup.controllers") else "v1/other",
        "workload_fs": sh(["findmnt", "-n", "-o", "FSTYPE", "/tmp"]),
        "workload_mount": sh(["findmnt", "-n", "-o", "SOURCE,OPTIONS", "/tmp"]),
        "cpu_governor": read_first("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor") or "not readable (virtualized)",
        "taskset": bool(shutil.which("taskset")),
        "python": platform.python_version(),
    }


# ---------------------------------------------------------------------------
# Telemetry helpers
# ---------------------------------------------------------------------------

def read_psi() -> dict:
    out = {}
    for res in ("cpu", "io", "memory"):
        d = {}
        try:
            with open(f"/proc/pressure/{res}") as f:
                for line in f:
                    parts = line.split()
                    kind = parts[0]
                    total = int(parts[-1].split("=")[1])
                    d[f"{res}_{kind}_total_us"] = total
        except OSError:
            pass
        out.update(d)
    return out


def psi_delta(before: dict, after: dict) -> dict:
    d = {}
    for k, v in after.items():
        b = before.get(k, v)
        key = k.replace("_total_us", "_delta_us")
        d[key] = v - b if v >= b else 0
    return d


PERF_EVENTS = "task-clock,context-switches,cpu-migrations,cycles,instructions,branches,branch-misses"


def parse_perf(stderr_text: str, ops: int) -> dict:
    """Parse `perf stat -x ';'` output. Absent/zero counters stay None."""
    got = {}
    for line in stderr_text.splitlines():
        fields = line.split(";")
        if len(fields) >= 3:
            val, _unit, event = fields[0], fields[1], fields[2]
            # perf_event_paranoid=2 renames every event with a ":u" suffix
            event = event.split(":")[0]
            if event and not val.startswith("<"):
                try:
                    got[event] = float(val.replace(",", ""))
                except ValueError:
                    pass
    out = {}
    if "task-clock" in got:
        out["task_clock_s"] = got["task-clock"]
    for ev, key in (("cycles", "cycles_per_op"), ("instructions", "instructions_per_op")):
        if ev in got and ops:
            out[key] = got[ev] / ops
    for ev in ("context-switches", "cpu-migrations", "branches", "branch-misses"):
        if ev in got:
            out[ev] = got[ev]
    return out


# ---------------------------------------------------------------------------
# Bench binary resolution
# ---------------------------------------------------------------------------

def find_bench(required_mode: str) -> Path:
    cands = sorted(glob.glob(str(REPO / "build" / "linux" / "x86_64" / "*" / "rx1_workload_bench")))
    for c in cands:
        if f"/{required_mode}/" in c:
            return Path(c)
    raise SystemExit(f"rx1: no {required_mode} build of rx1_workload_bench; run: "
                     f"xmake f -m {required_mode} --toolchain=clang -y && xmake build rx1_workload_bench")


# ---------------------------------------------------------------------------
# One run
# ---------------------------------------------------------------------------

def shape_file(op: str, size: int, total: int) -> Path:
    tag = {4096: "4k", 65536: "64k", 1048576: "1m"}.get(size, f"{size}")
    return WORKDIR / f"rx1_{tag}.bin"


def execute_run(idx: int, shape: dict, iv_name: str, rep: int, observe_ms: int,
                bench: Path, phase: str, seed: int, spec_tax: bool = False) -> dict:
    iv = INTERVENTIONS[iv_name]
    total = shape["total"]
    ops_per_rep = total // shape["size"]
    fpath = shape_file(shape["op"], shape["size"], total)
    WORKDIR.mkdir(parents=True, exist_ok=True)
    cmd = [str(bench), "--op", shape["op"], "--request-size", str(shape["size"]),
           "--total-bytes", str(total), "--app-depth", str(iv["depth"]),
           "--workers", str(iv["workers"]), "--capacity", str(iv["capacity"]),
           "--reps", str(shape.get("reps", 3)), "--warmup", str(shape.get("warmup", 1)),
           "--observe-interval-ms", str(observe_ms), "--file", str(fpath)]
    st = iv.get("stress")
    if st:
        cmd = ["taskset", "-c", st["cpus"]] + cmd

    artifact = {
        "schema_version": SCHEMA_VERSION,
        "phase": phase,
        "protocol_version": PROTOCOL_VERSION,
        "protocol_sha256": protocol_sha256(),
        "git_sha": None,  # filled by the caller (one fingerprint per phase)
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "random_seed": seed,
        "run_index": idx,
        "tax": bool(spec_tax),
        "workload": {
            "op": shape["op"], "request_size": shape["size"], "total_bytes": total,
            "ops_per_rep": ops_per_rep, "reps_internal": shape.get("reps", 3),
            "warmup_internal": shape.get("warmup", 1),
            "pipeline_depth": iv["depth"], "request_capacity": iv["capacity"],
            "configured_workers": iv["workers"],
        },
        "ground_truth_label": LABEL_OF[iv_name],
        "intervention": iv_name,
        "intervention_parameters": {
            "depth": iv["depth"], "capacity": iv["capacity"], "workers": iv["workers"],
            "stress": (dict(st, ran=True, cpus=st["cpus"], count=st["count"]) if st else None),
        },
        "observation_mode": "OBS-OFF" if observe_ms == 0 else ("OBS-LOW" if observe_ms >= 10 else "OBS-HIGH"),
        "sample_interval_ms": observe_ms,
    }

    psi0 = read_psi()
    t0 = time.monotonic()
    stress_procs = []
    if st:
        for _ in range(st["count"]):
            stress_procs.append(subprocess.Popen(
                ["taskset", "-c", st["cpus"], sys.executable, "-c", STRESS_PY],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    perf_err = None
    rc_code, stdout = -1, ""
    try:
        p = subprocess.run(["perf", "stat", "-x", ";", "-e", PERF_EVENTS, "--"] + cmd,
                           capture_output=True, text=True, timeout=300)
        perf_err = p.stderr
        rc_code = p.returncode
        stdout = p.stdout
    except subprocess.TimeoutExpired:
        perf_err = None
    finally:
        for sp in stress_procs:
            sp.terminate()
        for sp in stress_procs:
            try:
                sp.wait(timeout=10)
            except subprocess.TimeoutExpired:
                sp.kill()
    t1 = time.monotonic()
    psi1 = read_psi()
    wall_s = t1 - t0

    artifact["external"] = {
        "wall_s": wall_s,
        "psi": psi_delta(psi0, psi1),
        "perf": parse_perf(perf_err or "", ops_per_rep * shape.get("reps", 3)),
        "perf_raw": (perf_err or "").strip()[-2000:],
    }
    try:
        artifact["bench_json"] = json.loads(stdout)
    except json.JSONDecodeError:
        artifact["bench_json"] = None
        artifact["correctness_pass"] = False
        artifact["failure"] = f"bench exit {rc_code}, unparseable stdout"
        return artifact
    artifact["correctness_pass"] = (rc_code == 0 and artifact["bench_json"].get("all_reps_ok") is True)
    artifact["sample_count"] = artifact["bench_json"].get("sluice_obs", {}).get("sample_count", 0)
    return artifact


def protocol_sha256() -> str | None:
    if not PROTOCOL_PATH.exists():
        return None
    return hashlib.sha256(PROTOCOL_PATH.read_bytes()).hexdigest()


# ---------------------------------------------------------------------------
# Matrix construction (seeded, reproducible)
# ---------------------------------------------------------------------------

def precreate_files(bench: Path, shapes: list[dict]):
    """One unrecorded read pass per distinct file geometry: creates the
    deterministic pattern and warms the page cache so no measured run pays
    first-touch cost (which also pollutes the PSI window of the first run).
    """
    seen = set()
    for sh in shapes:
        key = (sh["size"], sh["total"])
        if key in seen:
            continue
        seen.add(key)
        fpath = shape_file("read", sh["size"], sh["total"])
        cmd = [str(bench), "--op", "read", "--request-size", str(sh["size"]),
               "--total-bytes", str(sh["total"]), "--app-depth", "4",
               "--workers", "2", "--capacity", "16", "--reps", "1",
               "--warmup", "0", "--observe-interval-ms", "0",
               "--no-latency", "--file", str(fpath)]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if p.returncode != 0:
            raise SystemExit(f"rx1: file pre-creation failed for {fpath}: {p.stderr[-500:]}")


def build_matrix(phase: str, protocol: dict | None) -> list[dict]:
    """Returns ordered list of run specs (cell order randomized per block)."""
    if phase == "pilot":
        seed = 0x51A1
        shapes = [
            dict(op="read", size=65536, total=512 << 20, reps=6, warmup=1),
        ]
        ivs = list(INTERVENTIONS)
        reps = 3
        rng = random.Random(seed)
        runs = []
        idx = 0
        for sh in shapes:
            cells = [(iv, r) for iv in ivs for r in range(reps)]
            rng.shuffle(cells)
            for iv, r in cells:
                runs.append(dict(idx=idx, shape=sh, iv=iv, rep=r, observe_ms=10))
                idx += 1
        # interval probes: OBS-HIGH on control + worker-limited
        for iv in ("I0_CONTROL", "I3_THREADPOOL_WORKER_LIMITED"):
            for r in range(2):
                runs.append(dict(idx=idx, shape=shapes[0], iv=iv, rep=r, observe_ms=1))
                idx += 1
        # observation-tax probes (pilot-scale)
        for mode_ms in (0, 10, 1):
            for r in range(3):
                runs.append(dict(idx=idx, shape=shapes[0], iv="I0_CONTROL", rep=r, observe_ms=mode_ms, tax=True))
                idx += 1
        return runs
    # formal: everything from the protocol
    assert protocol is not None, "formal phase requires the frozen protocol"
    seed = protocol["randomization_seed"]
    rng = random.Random(seed)
    runs = []
    idx = 0
    for sh in protocol["formal_matrix"]["shapes"]:
        cells = [(iv, r) for iv in protocol["formal_matrix"]["interventions"]
                 for r in range(protocol["formal_matrix"]["reps_per_cell"])]
        rng.shuffle(cells)
        for iv, r in cells:
            runs.append(dict(idx=idx, shape=dict(
                op=sh["op"], size=sh["request_size"], total=sh["total_bytes"],
                reps=protocol["run_policy"]["internal_reps"],
                warmup=protocol["run_policy"]["internal_warmup"]),
                iv=iv, rep=r,
                observe_ms=protocol["sampling"]["obs_low_ms"]))
            idx += 1
    for sh in protocol["observability_tax"]["shapes"]:
        modes = [protocol["sampling"]["obs_off_ms"], protocol["sampling"]["obs_low_ms"],
                 protocol["sampling"]["obs_high_ms"]]
        cells = [(m, r) for m in modes for r in range(protocol["observability_tax"]["reps_per_mode"])]
        rng.shuffle(cells)
        for m, r in cells:
            runs.append(dict(idx=idx, shape=dict(
                op=sh["op"], size=sh["request_size"], total=sh["total_bytes"],
                reps=protocol["run_policy"]["internal_reps"],
                warmup=protocol["run_policy"]["internal_warmup"]),
                iv="I0_CONTROL", rep=r, observe_ms=m, tax=True))
            idx += 1
    return runs


def cmd_run(args):
    protocol = None
    if args.phase == "formal":
        if not PROTOCOL_PATH.exists():
            raise SystemExit("rx1: formal phase requires the frozen protocol "
                             "(research/rx1/rx1_protocol_v1.json); pilot first, then freeze")
        protocol = json.loads(PROTOCOL_PATH.read_text())
    bench = find_bench("release")
    runs = build_matrix(args.phase, protocol)
    if args.only:
        want = set(args.only.split(","))
        runs = [r for r in runs if r["iv"] in want]
    print("rx1: pre-creating/warming workload files (unrecorded)")
    precreate_files(bench, [r["shape"] for r in runs])
    outdir = RESULTS / args.phase
    outdir.mkdir(parents=True, exist_ok=True)
    fp = env_fingerprint()
    print(f"rx1: phase={args.phase} runs={len(runs)} bench={bench}")
    print(f"rx1: git={fp['git_sha']} dirty={fp['dirty']}")
    t_start = time.time()
    for spec in runs:
        art = execute_run(spec["idx"], spec["shape"], spec["iv"], spec["rep"],
                          spec["observe_ms"], bench, args.phase,
                          protocol["randomization_seed"] if protocol else 0x51A1,
                          spec_tax=spec.get("tax", False))
        art["git_sha"] = fp["git_sha"]
        art["dirty"] = fp["dirty"]
        art["build_mode"] = "release"
        art["environment"] = fp
        name = f"run_{spec['idx']:04d}.json"
        (outdir / name).write_text(json.dumps(art, indent=1) + "\n")
        thr = (art.get("bench_json") or {}).get("outcome", {}).get("throughput_mbs_median", -1)
        ok = art.get("correctness_pass")
        print(f"  [{spec['idx']:04d}] {spec['iv']:32s} obs={art['observation_mode']:8s} "
              f"thr={thr:8.1f} MB/s ok={ok} ({time.time()-t_start:.0f}s elapsed)")
    print(f"rx1: wrote {len(runs)} artifacts to {outdir}")


# ---------------------------------------------------------------------------
# classify + validity
# ---------------------------------------------------------------------------

def load_runs(phase: str) -> list[dict]:
    outdir = RESULTS / phase
    arts = []
    for p in sorted(outdir.glob("run_*.json")):
        if p.name.endswith(".scored.json"):
            continue
        arts.append(json.loads(p.read_text()))
    return arts


def cmd_classify(args):
    T = rc.THRESHOLDS_V1
    if PROTOCOL_PATH.exists():
        proto = json.loads(PROTOCOL_PATH.read_text())
        if proto.get("thresholds") and args.phase == "formal":
            if proto["thresholds"] != T:
                raise SystemExit("rx1: protocol thresholds differ from classifier v1 — freeze violation")
    arts = load_runs(args.phase)
    scored = 0
    for a in arts:
        if not a.get("bench_json"):
            a["classifier_C_prediction"] = None
            a["classifier_E_prediction"] = None
            a["ground_truth_valid"] = False
            a["invalid_reason"] = a.get("failure", "no bench json")
        else:
            f = rc.extract_features(a)
            a["features"] = f
            a["classifier_C_prediction"] = rc.classify_c(f, T)
            a["classifier_E_prediction"] = rc.classify_e(f, T)
            if a["observation_mode"] == "OBS-OFF":
                # tax runs are not attribution samples
                a["ground_truth_valid"] = False
                a["invalid_reason"] = "OBS-OFF tax run (not an attribution sample)"
            else:
                valid, reason = rc.ground_truth_valid(a)
                a["ground_truth_valid"] = valid
                a["invalid_reason"] = reason
        src = RESULTS / args.phase / f"run_{a['run_index']:04d}.json"
        dst = RESULTS / args.phase / f"run_{a['run_index']:04d}.scored.json"
        dst.write_text(json.dumps(a, indent=1) + "\n")
        scored += 1
    print(f"rx1: scored {scored} runs ({args.phase})")


# ---------------------------------------------------------------------------
# analyze
# ---------------------------------------------------------------------------

def confusion_md(title: str, m: dict, labels) -> str:
    lines = [f"### {title}", "", "true\\pred | " + " | ".join(labels),
             "--- | " + " | ".join("---" for _ in labels)]
    for t in labels:
        row = [str(m.get(t, {}).get(p, 0)) for p in labels]
        lines.append(f"{t} | " + " | ".join(row))
    return "\n".join(lines) + "\n"


def cmd_analyze(args):
    arts = []
    for p in sorted((RESULTS / "formal").glob("run_*.scored.json")):
        arts.append(json.loads(p.read_text()))
    if not arts:
        raise SystemExit("rx1: no scored formal runs; run classify --phase formal first")
    # Attribution matrix: the 240 non-tax runs ONLY. The 48 observation-tax
    # runs are a separate matrix and never enter the attribution denominator.
    attribution_runs = [a for a in arts if not a.get("tax") and a.get("bench_json")]
    tax_runs_all = [a for a in arts if a.get("tax")]
    attr = [a for a in attribution_runs if a.get("ground_truth_valid")]
    invalid = [a for a in attribution_runs if not a.get("ground_truth_valid")]
    pairs_c = [(a["classifier_C_prediction"], a["ground_truth_label"]) for a in attr]
    pairs_e = [(a["classifier_E_prediction"], a["ground_truth_label"]) for a in attr]
    acc_c = rc.accuracy(pairs_c)
    acc_e = rc.accuracy(pairs_e)
    delta, lo, hi = rc.paired_bootstrap_delta(pairs_c, pairs_e)
    per_c, mf1_c = rc.per_class(pairs_c)
    per_e, mf1_e = rc.per_class(pairs_e)
    trans = rc.paired_transitions(pairs_c, pairs_e)
    unk_c = sum(1 for p, _ in pairs_c if p == "UNKNOWN") / len(pairs_c) if pairs_c else 0
    unk_e = sum(1 for p, _ in pairs_e if p == "UNKNOWN") / len(pairs_e) if pairs_e else 0
    wrong_c = sum(1 for p, t in pairs_c if p != t and p != "UNKNOWN") / len(pairs_c) if pairs_c else 0
    wrong_e = sum(1 for p, t in pairs_e if p != t and p != "UNKNOWN") / len(pairs_e) if pairs_e else 0
    conf_c = rc.confusion(pairs_c)
    conf_e = rc.confusion(pairs_e)

    # Post-hoc robustness: paired bootstrap with the workload-shape x
    # intervention cell as resampling unit (disagreements cluster by cell).
    cell_of = []
    cells = {}
    for i, a in enumerate(attr):
        w = a["workload"]
        key = (w["op"], w["request_size"], a["intervention"])
        cells.setdefault(key, []).append(i)
    cell_list = list(cells.values())
    rb_delta, rb_lo, rb_hi = rc.paired_block_bootstrap_delta(pairs_c, pairs_e, cell_list)
    robustness = {
        "label": "ROBUSTNESS ANALYSIS — NOT PRIMARY PREREGISTERED SCORE",
        "method": "paired block bootstrap; resampling unit = workload-shape x intervention cell",
        "n_cells": len(cell_list),
        "seed": 1702,
        "delta_accuracy": rb_delta,
        "delta_ci95": [rb_lo, rb_hi],
        "note": "does not replace or modify the frozen run-level primary result",
    }

    # ---- observability tax (paired per shape, see tax_analysis) ----
    tax = tax_analysis(arts)

    analysis = {
        "n_scored": len(arts),
        "n_attribution_runs": len(attribution_runs),
        "n_attribution_valid": len(attr),
        "n_attribution_invalid": len(invalid),
        "n_observation_tax_runs": len(tax_runs_all),
        "attribution_denominator_note": (
            "valid/total counts the 240 attribution-matrix runs only; the 48 "
            "observation-tax runs are a separate matrix and never enter the "
            "attribution denominator"),
        "invalid_reasons": {a["run_index"]: a.get("invalid_reason") for a in invalid},
        "accuracy_c": acc_c,
        "accuracy_e": acc_e,
        "delta_accuracy": delta,
        "delta_ci95": [lo, hi],
        "macro_f1_c": mf1_c,
        "macro_f1_e": mf1_e,
        "unknown_rate_c": unk_c,
        "unknown_rate_e": unk_e,
        "wrong_cause_rate_c": wrong_c,
        "wrong_cause_rate_e": wrong_e,
        "per_class_c": per_c,
        "per_class_e": per_e,
        "transitions": trans,
        "confusion_c": conf_c,
        "confusion_e": conf_e,
        "robustness_block_bootstrap": robustness,
        "observability_tax": tax,
        "verdict_inputs": {
            "delta_accuracy_pp": delta * 100,
            "ci_excludes_zero_positive": lo > 0,
            "wrong_cause_increase": wrong_e > wrong_c,
        },
    }
    # verdict gate (book §26 defaults; unchanged from the frozen protocol)
    v = analysis["verdict_inputs"]
    low_tax = tax.get("aggregate", {}).get("low", {}).get("throughput_tax_pct_median_of_shapes", 99)
    if (v["delta_accuracy_pp"] >= 15 and v["ci_excludes_zero_positive"]
            and not v["wrong_cause_increase"] and low_tax <= 2):
        verdict = "SUPPORTED ENOUGH TO DEEPEN"
    elif delta > 0:
        verdict = "PROMISING BUT INSUFFICIENT"
    elif wrong_e > wrong_c or delta <= 0:
        verdict = "NOT SUPPORTED / STOP EXPANSION"
    else:
        verdict = "ENGINEERING VALUE ONLY"
    analysis["verdict_gate"] = verdict
    outdir = RESULTS / "analysis"
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / "analysis.json").write_text(json.dumps(analysis, indent=1) + "\n")

    labels = rc.CLASSIFIER_LABELS
    md = ["# RX-1 analysis tables (machine source: analysis.json)", ""]
    md.append(f"- attribution-matrix runs: {len(attribution_runs)}; valid: {len(attr)}; "
              f"invalid: {len(invalid)}; observation-tax runs: {len(tax_runs_all)} "
              f"(separate matrix, excluded from the attribution denominator)")
    md.append(f"- accuracy C = {acc_c:.4f}, E = {acc_e:.4f}, Δ = {delta*100:+.2f} pp "
              f"[95% CI {lo*100:+.2f}, {hi*100:+.2f}] (preregistered run-level paired bootstrap)")
    md.append(f"- ROBUSTNESS ANALYSIS — NOT PRIMARY PREREGISTERED SCORE: cell-level "
              f"paired block bootstrap (unit = workload-shape × intervention, "
              f"{len(cell_list)} cells): Δ = {rb_delta*100:+.2f} pp "
              f"[95% CI {rb_lo*100:+.2f}, {rb_hi*100:+.2f}]")
    md.append(f"- macro-F1 C = {mf1_c:.4f}, E = {mf1_e:.4f}")
    md.append(f"- UNKNOWN rate C = {unk_c:.3f}, E = {unk_e:.3f}; wrong-cause C = {wrong_c:.3f}, E = {wrong_e:.3f}")
    md.append("")
    md.append(confusion_md("C confusion (rows = ground truth)", conf_c, labels))
    md.append(confusion_md("E confusion (rows = ground truth)", conf_e, labels))
    md.append("### Paired transitions C→E\n")
    md.append("| transition | count |")
    md.append("|---|---|")
    for k, n in trans.items():
        md.append(f"| {k} | {n} |")
    md.append("\n### Per-class recall C vs E\n")
    md.append("| class | recall C | recall E |")
    md.append("|---|---|---|")
    for L in labels:
        rc_ = per_c.get(L, {}).get("recall", 0.0)
        re_ = per_e.get(L, {}).get("recall", 0.0)
        md.append(f"| {L} | {rc_:.3f} | {re_:.3f} |")
    md.append("\n### Observability tax (paired per shape vs same-shape OBS-OFF)\n")
    md.append("| shape | mode | n | throughput MB/s (median) | thr tax % | p99 µs (median) | p99 tax % |")
    md.append("|---|---|---|---|---|---|---|")
    for shape, row in tax.get("per_shape", {}).items():
        for mode in ("off", "low", "high"):
            t = row.get(mode)
            if not t:
                continue
            tax_txt = f"{t['throughput_tax_pct']:+.1f}" if "throughput_tax_pct" in t else "baseline"
            p99_tax = f"{t['p99_tax_pct']:+.1f}" if "p99_tax_pct" in t else "baseline"
            md.append(f"| {shape} | {mode} | {t['n']} | {t['throughput_mbs_median']:.1f} | {tax_txt} | "
                      f"{t['p99_ns_median']/1000:.0f} | {p99_tax} |")
    agg = tax.get("aggregate", {})
    md.append("")
    md.append(f"- aggregate (median of per-shape normalized effects): OBS-LOW throughput tax "
              f"{agg.get('low', {}).get('throughput_tax_pct_median_of_shapes', float('nan')):+.1f}%, "
              f"OBS-HIGH {agg.get('high', {}).get('throughput_tax_pct_median_of_shapes', float('nan')):+.1f}%")
    md.append(f"- {tax['conclusion']}")
    md.append("")
    md.append(f"**Verdict gate: {verdict}**")
    text = "\n".join(md).rstrip() + "\n"
    (outdir / "tables.md").write_text(text)
    print(json.dumps({k: analysis[k] for k in ("n_attribution_runs", "n_attribution_valid",
                                               "accuracy_c", "accuracy_e", "delta_accuracy",
                                               "delta_ci95", "verdict_gate")}, indent=1))
    print(f"robustness block bootstrap: {robustness['delta_accuracy']*100:+.2f} pp {robustness['delta_ci95']}")
    print(f"rx1: wrote {outdir}/analysis.json and tables.md")


def tax_analysis(arts: list[dict]) -> dict:
    """Observability tax from the dedicated tax block, PAIRED PER WORKLOAD
    SHAPE: each OBS-LOW / OBS-HIGH measurement is normalized against the
    same-shape OBS-OFF baseline first; normalized effects are aggregated
    afterward. Shapes with different throughput scales are never pooled
    into one aggregate percentage. Raw formal artifacts are not modified.
    """
    by = {}  # (op, request_size, mode) -> rows
    for a in arts:
        if not a.get("tax") or a.get("intervention") != "I0_CONTROL" or not a.get("bench_json"):
            continue
        w = a["workload"]
        mode = {"OBS-OFF": "off", "OBS-LOW": "low", "OBS-HIGH": "high"}[a["observation_mode"]]
        b = a["bench_json"]
        f = rc.extract_features(a)
        by.setdefault((w["op"], w["request_size"], mode), []).append({
            "thr": b["outcome"]["throughput_mbs_median"],
            "p50": b["outcome"]["lat_p50_ns_median"],
            "p99": b["outcome"]["lat_p99_ns_median"],
            "cores": f["cpu_cores_used"],
            "ctxt": f["ctxt_per_kop"],
        })
    shapes = sorted({(k[0], k[1]) for k in by})
    per_shape = {}
    for sh in shapes:
        off = by.get((sh[0], sh[1], "off"), [])
        if not off:
            continue
        med = lambda rows, k: statistics.median([r[k] for r in rows])
        off_thr = med(off, "thr")
        off_p99 = med(off, "p99")
        row = {
            "off": {
                "n": len(off),
                "throughput_mbs_median": off_thr,
                "p99_ns_median": off_p99,
                "cpu_cores_median": med(off, "cores"),
                "ctxt_per_kop_median": med(off, "ctxt"),
                "throughput_samples": [r["thr"] for r in off],
                "p99_samples": [r["p99"] for r in off],
            }
        }
        for mode in ("low", "high"):
            rs = by.get((sh[0], sh[1], mode), [])
            if not rs:
                continue
            m_thr, m_p99 = med(rs, "thr"), med(rs, "p99")
            row[mode] = {
                "n": len(rs),
                "throughput_mbs_median": m_thr,
                "throughput_tax_pct": (off_thr - m_thr) / off_thr * 100 if off_thr else float("nan"),
                "p99_ns_median": m_p99,
                "p99_tax_pct": (m_p99 - off_p99) / off_p99 * 100 if off_p99 else float("nan"),
                "cpu_cores_median": med(rs, "cores"),
                "ctxt_per_kop_median": med(rs, "ctxt"),
                "throughput_samples": [r["thr"] for r in rs],
                "p99_samples": [r["p99"] for r in rs],
            }
        per_shape[f"{sh[0]}/{sh[1]}"] = row
    aggregate = {}
    for mode in ("low", "high"):
        thr_taxes = [per_shape[k][mode]["throughput_tax_pct"] for k in per_shape if mode in per_shape[k]]
        p99_taxes = [per_shape[k][mode]["p99_tax_pct"] for k in per_shape if mode in per_shape[k]]
        if thr_taxes:
            aggregate[mode] = {
                "throughput_tax_pct_median_of_shapes": statistics.median(thr_taxes),
                "throughput_tax_pct_per_shape": thr_taxes,
                "p99_tax_pct_median_of_shapes": statistics.median(p99_taxes),
                "p99_tax_pct_per_shape": p99_taxes,
            }
    return {
        "method": ("per-shape pairing against the same-shape OBS-OFF median; "
                   "aggregate = median of per-shape normalized effects"),
        "per_shape": per_shape,
        "aggregate": aggregate,
        "conclusion": ("No reproducible or monotonic observation-tax signal was "
                       "established at the current sample size."),
    }


# ---------------------------------------------------------------------------
# freeze
# ---------------------------------------------------------------------------

def cmd_freeze(args):
    if not PROTOCOL_PATH.exists():
        raise SystemExit("rx1: protocol file missing")
    proto = json.loads(PROTOCOL_PATH.read_text())
    required = ["label_set", "feature_definitions", "sampling", "interventions",
                "thresholds", "rule_precedence", "unknown_policy",
                "formal_matrix", "observability_tax", "run_policy",
                "randomization_seed", "warmup_policy", "invalid_rules",
                "primary_metrics", "verdict_thresholds", "protocol_version"]
    missing = [k for k in required if k not in proto]
    if missing:
        raise SystemExit(f"rx1: protocol missing keys: {missing}")
    if proto["thresholds"] != rc.THRESHOLDS_V1:
        raise SystemExit("rx1: protocol thresholds != rx1_classify.THRESHOLDS_V1")
    if proto["label_set"] != rc.CLASSIFIER_LABELS:
        raise SystemExit("rx1: protocol label set != classifier label set")
    sha = hashlib.sha256(PROTOCOL_PATH.read_bytes()).hexdigest()
    print("RX1 FORMAL PROTOCOL FROZEN")
    print(f"PROTOCOL_SHA256={sha}")
    print(f"protocol_version={proto['protocol_version']}")
    print("commit the protocol BEFORE running: rx1.py run --phase formal")


# ---------------------------------------------------------------------------
# self-test (book §42)
# ---------------------------------------------------------------------------

def cmd_self_test(args):

    ok = [0]
    def check(name, cond):
        ok[0] += 1 if cond else 0
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}")
        if not cond:
            raise SystemExit(f"self-test failed: {name}")

    synth_path = RX1 / "synth" / "synth_cases.json"
    cases = json.loads(synth_path.read_text())["cases"]

    print("rx1 self-test: synthetic classifier cases")
    for c in cases:
        f = c["features"]
        got_c = rc.classify_c(f)
        got_e = rc.classify_e(f)
        check(f"{c['name']}: C={c['expected_c']}", got_c == c["expected_c"])
        check(f"{c['name']}: E={c['expected_e']}", got_e == c["expected_e"])

    print("rx1 self-test: scorer pipeline (tiny known dataset)")
    pairs_c = [("CONTROL", "CONTROL"), ("APP_PIPELINE_LIMITED", "APP_PIPELINE_LIMITED"),
               ("UNKNOWN", "REQUEST_CAPACITY_LIMITED"), ("CPU_CONTENDED", "CPU_CONTENDED"),
               ("CPU_CONTENDED", "CONTROL"), ("UNKNOWN", "UNKNOWN")]
    # true labels for the last pair: UNKNOWN is not a ground truth class;
    # use THREADPOOL_WORKER_LIMITED as truth for that slot instead.
    pairs_c = [("CONTROL", "CONTROL"), ("APP_PIPELINE_LIMITED", "APP_PIPELINE_LIMITED"),
               ("UNKNOWN", "REQUEST_CAPACITY_LIMITED"), ("CPU_CONTENDED", "CPU_CONTENDED"),
               ("CPU_CONTENDED", "CONTROL"), ("UNKNOWN", "THREADPOOL_WORKER_LIMITED")]
    pairs_e = [("CONTROL", "CONTROL"), ("APP_PIPELINE_LIMITED", "APP_PIPELINE_LIMITED"),
               ("REQUEST_CAPACITY_LIMITED", "REQUEST_CAPACITY_LIMITED"), ("CPU_CONTENDED", "CPU_CONTENDED"),
               ("CPU_CONTENDED", "CONTROL"), ("THREADPOOL_WORKER_LIMITED", "THREADPOOL_WORKER_LIMITED")]
    check("accuracy C = 3/6", abs(rc.accuracy(pairs_c) - 0.5) < 1e-12)
    check("accuracy E = 5/6", abs(rc.accuracy(pairs_e) - 5 / 6) < 1e-12)
    d, lo, hi = rc.paired_bootstrap_delta(pairs_c, pairs_e, iters=2000, seed=7)
    check("bootstrap delta = 1/3", abs(d - 1 / 3) < 1e-12)
    check("bootstrap CI ordered", lo <= d <= hi)
    t = rc.paired_transitions(pairs_c, pairs_e)
    check("unknown->right = 2 (slots 3 and 6)", t["unknown_to_right"] == 2 and t["wrong_to_right"] == 0)
    check("right->right = 3", t["right_to_right"] == 3)
    conf = rc.confusion(pairs_c)
    check("confusion cpu/control = 1", conf["CONTROL"]["CPU_CONTENDED"] == 1)
    per, mf1 = rc.per_class(pairs_e)
    check("precision CPU = 0.5 (1 tp / 2 predictions)", abs(per["CPU_CONTENDED"]["precision"] - 0.5) < 1e-12)
    check("recall CPU = 1.0 (1 tp / 1 truth)", abs(per["CPU_CONTENDED"]["recall"] - 1.0) < 1e-12)

    print("rx1 self-test: validity + schema on synthetic artifacts")
    base_bench = dict(
        op="read", request_size=65536, total_bytes=512 << 20, app_depth=16,
        workers=4, capacity=64, ops_per_rep=8192, reps=3, warmup=1, latency=True,
        file="/tmp/x", observe_interval_ms=20, lifecycle_setup_ns=1, lifecycle_teardown_ns=1,
        measured_window_ns=3_000_000_000,
        outcome=dict(submit_rejections_total=0, throughput_mbs_median=1000.0,
                     lat_p50_ns_median=1000.0, lat_p95_ns_median=2000.0, lat_p99_ns_median=3000.0,
                     user_ns_sum=3e9, sys_ns_sum=1e9),
        os_accounting=dict(ctxt_vol_delta=1000, ctxt_invol_delta=10, sched_wait_ns_delta=5e9,
                           sched_run_ns_delta=4e9, sched_slices_delta=1010, threads_end=6),
        sluice_obs=dict(sample_count=100, window_s=3.0, realized_hz=33.0, overflowed=False,
                        arena_capacity=64, configured_workers=4, slot_in_use_max=16.0,
                        slot_in_use_mean=5.0, outstanding_max=16.0, outstanding_mean=4.0,
                        active_max=3.0, active_mean=1.0, dispatch_occ_max=3.0, dispatch_occ_mean=0.5,
                        frac_slot_at_capacity=0.0, frac_active_at_configured=0.05,
                        frac_dispatch_nonzero=0.1, arena_high_water_final=16,
                        dispatch_high_water_final=8, rejections_initial=0, rejections_final=0,
                        rejections_delta=0),
        reps_out=[], all_reps_ok=True)
    base_run = dict(schema_version=1, phase="pilot", run_index=0,
                    bench_json=dict(base_bench),
                    external=dict(wall_s=3.2, psi={"cpu_some_delta_us": 100.0},
                                  perf={}),
                    workload=dict(app_depth=16, capacity=64, workers=4, op="read",
                                  request_size=65536, total_bytes=512 << 20),
                    ground_truth_label="CONTROL", intervention="I0_CONTROL",
                    intervention_parameters=dict(depth=16, capacity=64, workers=4, stress=None),
                    observation_mode="OBS-LOW", sample_interval_ms=20,
                    correctness_pass=True)
    v, r = rc.ground_truth_valid(base_run)
    check("synthetic CONTROL valid", v)
    run2 = json.loads(json.dumps(base_run))
    run2["ground_truth_label"] = "REQUEST_CAPACITY_LIMITED"
    v2, r2 = rc.ground_truth_valid(run2)
    check("synthetic mislabeled CAP invalid", not v2 and "rejections" in r2)
    run3 = json.loads(json.dumps(base_run))
    run3["bench_json"]["sluice_obs"]["rejections_delta"] = 500
    run3["bench_json"]["sluice_obs"]["arena_high_water_final"] = 64
    run3["bench_json"]["sluice_obs"]["frac_slot_at_capacity"] = 0.9
    run3["bench_json"]["outcome"]["submit_rejections_total"] = 500
    run3["ground_truth_label"] = "REQUEST_CAPACITY_LIMITED"
    run3["workload"]["capacity"] = 4
    run3["workload"]["app_depth"] = 32
    f3 = rc.extract_features(run3)
    check("synthetic CAP features: E classifies CAP", rc.classify_e(f3) == "REQUEST_CAPACITY_LIMITED")
    v3, _ = rc.ground_truth_valid(run3)
    check("synthetic CAP valid", v3)

    print("rx1 self-test: env + telemetry parsers")
    psi = read_psi()
    check("psi cpu some present", "cpu_some_total_us" in psi)
    # perf_event_paranoid=2 renames every event with a ":u" suffix and may
    # emit "<not counted>" rows — this case locks the decoder regression that
    # emptied the perf feature dicts for the v1 formal matrix.
    pf = parse_perf(
        "32537.12;msec;task-clock:u;32537123405;100.00;;\n"
        "6562985309;;cycles:u;32537123405;100.00;;\n"
        "0;;context-switches:u;32537123405;100.00;;\n"
        "<not counted>;;branches:u;32537123405;100.00;;\n", 1000)
    check("perf parse :u-suffixed cycles_per_op",
          abs(pf.get("cycles_per_op", 0) - 6562985.309) < 1e-6)
    check("perf parse :u-suffixed task_clock_s",
          abs(pf.get("task_clock_s", 0) - 32537.12) < 1e-9)
    check("perf parse skips <not counted>", "branches" not in pf)
    print(f"\nrx1 self-test: {ok[0]} checks passed")


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(prog="rx1")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("env")
    p = sub.add_parser("run")
    p.add_argument("--phase", choices=["pilot", "formal"], required=True)
    p.add_argument("--only", help="comma-separated intervention names (pilot iteration)")
    sub.add_parser("freeze")
    p = sub.add_parser("classify")
    p.add_argument("--phase", choices=["pilot", "formal"], default="pilot")
    p = sub.add_parser("analyze")
    p = sub.add_parser("self-test")
    args = ap.parse_args()
    if args.cmd == "env":
        print(json.dumps(env_fingerprint(), indent=1))
    elif args.cmd == "run":
        cmd_run(args)
    elif args.cmd == "freeze":
        cmd_freeze(args)
    elif args.cmd == "classify":
        cmd_classify(args)
    elif args.cmd == "analyze":
        cmd_analyze(args)
    elif args.cmd == "self-test":
        cmd_self_test(args)


if __name__ == "__main__":
    main()
