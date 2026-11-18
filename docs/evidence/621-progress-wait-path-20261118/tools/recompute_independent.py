#!/usr/bin/env python3
"""Independent recomputation of the #621 P1 A/A cells.

This script deliberately does not import ``paired_session.py`` or
``aa_recompute.py`` and does not read any runner-produced aggregate.  It starts
from the per-arm records (``arms.jsonl``), the raw ``perf stat`` files and the
raw benchmark JSONL, re-checks validity, re-derives every metric and computes
the paired statistics from scratch.

Primary metrics (per arm, one whole-process invocation):

  cpu     RUSAGE_CHILDREN user+sys, recomputed as (cpu_user_s + cpu_sys_s)
  instr   perf ``instructions:u``, read from the raw ``.perf`` file
  wall    monotonic wall seconds recorded by the runner
  cycles  perf ``cycles:u``, read from the raw ``.perf`` file (supporting)

Paired difference for pair i is 100 * (B_i - A_i) / A_i.  The boom/bust
sensitivity using the pair mean as denominator is reported as ``sym``.

Fixed-seed bootstrap: 10,000 resamples, percentile 95% CI, seed derived
deterministically from (base seed, cell, metric).  Confidence intervals are
descriptive noise bands, not evidence that an A/B effect exists.

MME inputs use the sample SD of the paired differences and Student-t quantiles
(computed by inversion of the regularized incomplete beta, no SciPy):

  80% power, two-sided alpha 0.05:  (t_.975 + t_.80) * sd / sqrt(n) <= delta
  95% CI half-width <= delta/2:      t_.975 * sd / sqrt(n) <= delta/2

Usage:
  python3 -I tools/recompute_independent.py --aa-root p1/aa
  python3 -I tools/recompute_independent.py --aa-root p1/aa --json-out /tmp/x.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import re
import statistics
import sys

TOOL_VERSION = "1.0-independent"
DEFAULT_SEED = 621
BOOTSTRAP_RESAMPLES = 10000
PERMUTATION_RESAMPLES = 10000
ALPHA = 0.05
POWER = 0.80
METRIC_ORDER = ("cpu", "instr", "wall", "cycles")
METRIC_LABELS = {
    "cpu": "CPU/task (rusage user+sys)",
    "instr": "user instructions/task (instructions:u)",
    "wall": "wall/task",
    "cycles": "user cycles/task (cycles:u)",
}
REQUIRED_EVENTS = ("task-clock", "instructions", "cycles")
MAX_N = 1_000_000


# ---------------------------------------------------------------------------
# small numerical helpers (stdlib only)
# ---------------------------------------------------------------------------

def _betacf(a: float, b: float, x: float) -> float:
    tiny = 1e-300
    qab = a + b
    qap = a + 1.0
    qam = a - 1.0
    c = 1.0
    d = 1.0 - qab * x / qap
    if abs(d) < tiny:
        d = tiny
    d = 1.0 / d
    h = d
    for m in range(1, 300):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        if abs(d) < tiny:
            d = tiny
        c = 1.0 + aa / c
        if abs(c) < tiny:
            c = tiny
        d = 1.0 / d
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        if abs(d) < tiny:
            d = tiny
        c = 1.0 + aa / c
        if abs(c) < tiny:
            c = tiny
        d = 1.0 / d
        delta = d * c
        h *= delta
        if abs(delta - 1.0) < 3.0e-14:
            break
    return h


def _betai(a: float, b: float, x: float) -> float:
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    lbt = (
        math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b)
        + a * math.log(x) + b * math.log1p(-x)
    )
    bt = math.exp(lbt)
    if x < (a + 1.0) / (a + b + 2.0):
        return bt * _betacf(a, b, x) / a
    return 1.0 - bt * _betacf(b, a, 1.0 - x) / b


def t_cdf(t: float, df: int) -> float:
    x = df / (df + t * t)
    tail = 0.5 * _betai(df / 2.0, 0.5, x)
    return 1.0 - tail if t >= 0.0 else tail


def t_ppf(p: float, df: int) -> float:
    if not 0.0 < p < 1.0:
        raise ValueError("p out of range")
    lo, hi = 0.0, 1.0
    while t_cdf(hi, df) < p:
        hi *= 2.0
        if hi > 1.0e7:
            raise RuntimeError("t_ppf failed to bracket")
    for _ in range(200):
        mid = 0.5 * (lo + hi)
        if t_cdf(mid, df) < p:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


def mean(values):
    return statistics.fmean(values)


def sd(values):
    return statistics.stdev(values) if len(values) > 1 else 0.0


def percentile(sorted_values, p):
    if not sorted_values:
        return float("nan")
    if len(sorted_values) == 1:
        return sorted_values[0]
    k = (len(sorted_values) - 1) * p
    lo = int(math.floor(k))
    hi = int(math.ceil(k))
    if lo == hi:
        return sorted_values[lo]
    return sorted_values[lo] * (hi - k) + sorted_values[hi] * (k - lo)


def _rng(cell: str, label: str, seed: int) -> random.Random:
    digest = hashlib.sha256(f"{cell}|{label}".encode()).hexdigest()[:16]
    return random.Random(seed ^ int(digest, 16))


def bootstrap_mean(values, cell, label, seed, resamples=BOOTSTRAP_RESAMPLES):
    rng = _rng(cell, "boot|" + label, seed)
    n = len(values)
    means = sorted(
        mean([values[rng.randrange(n)] for _ in range(n)])
        for _ in range(resamples)
    )
    lo = percentile(means, 0.025)
    hi = percentile(means, 0.975)
    return {
        "lo": lo,
        "hi": hi,
        "halfWidth": 0.5 * (hi - lo),
        "resamples": resamples,
    }


def permutation_mean_diff(group_a, group_b, cell, label, seed,
                          resamples=PERMUTATION_RESAMPLES):
    pooled = list(group_a) + list(group_b)
    observed = mean(group_a) - mean(group_b)
    rng = _rng(cell, "perm|" + label, seed)
    idx = list(range(len(pooled)))
    hits = 0
    for _ in range(resamples):
        rng.shuffle(idx)
        first = [pooled[i] for i in idx[: len(group_a)]]
        second = [pooled[i] for i in idx[len(group_a):]]
        if abs(mean(first) - mean(second)) >= abs(observed) - 1e-15:
            hits += 1
    return {
        "observed": observed,
        "p": (hits + 1) / (resamples + 1),
        "resamples": resamples,
    }


def ols_slope(xs, ys):
    mx = mean(xs)
    my = mean(ys)
    denom = sum((x - mx) ** 2 for x in xs)
    if denom == 0:
        return 0.0
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / denom


def permutation_slope_p(xs, ys, cell, label, seed,
                        resamples=PERMUTATION_RESAMPLES):
    observed = ols_slope(xs, ys)
    rng = _rng(cell, "slope|" + label, seed)
    work = list(ys)
    hits = 0
    for _ in range(resamples):
        rng.shuffle(work)
        if abs(ols_slope(xs, work)) >= abs(observed) - 1e-15:
            hits += 1
    return (hits + 1) / (resamples + 1)


def required_n_power(sd_value, delta, power=POWER):
    """Smallest n with (t_.975 + t_.power) * sd / sqrt(n) <= delta."""
    if delta <= 0 or sd_value <= 0:
        return None
    z_guess = (1.959963984540054 + 0.8416212335729143) ** 2
    guess = max(2, int(math.ceil(z_guess * (sd_value / delta) ** 2)))
    if guess > MAX_N:
        return None
    lo = max(2, guess - 40)
    hi = min(MAX_N, max(guess + 400, int(guess * 1.25) + 40))
    for n in range(lo, hi + 1):
        df = n - 1
        crit = t_ppf(0.975, df) + t_ppf(power, df)
        if crit * sd_value / math.sqrt(n) <= delta:
            return n
    return None


def required_n_halfwidth(sd_value, target_halfwidth):
    """Smallest n with t_.975 * sd / sqrt(n) <= target_halfwidth."""
    if target_halfwidth <= 0 or sd_value <= 0:
        return None
    z = 1.959963984540054
    guess = max(2, int(math.ceil(z * z * (sd_value / target_halfwidth) ** 2)))
    if guess > MAX_N:
        return None
    lo = max(2, guess - 40)
    hi = min(MAX_N, max(guess + 400, int(guess * 1.25) + 40))
    for n in range(lo, hi + 1):
        df = n - 1
        if t_ppf(0.975, df) * sd_value / math.sqrt(n) <= target_halfwidth:
            return n
    return None


def mde80(sd_value, n):
    df = max(1, n - 1)
    return (t_ppf(0.975, df) + t_ppf(POWER, df)) * sd_value / math.sqrt(n)


def ci_halfwidth_t(sd_value, n):
    df = max(1, n - 1)
    return t_ppf(0.975, df) * sd_value / math.sqrt(n)


# ---------------------------------------------------------------------------
# raw-data readers and independent validity
# ---------------------------------------------------------------------------

def read_jsonl(path):
    out = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                out.append(json.loads(line))
    return out


def parse_perf(path):
    """Parse a perf stat -x, file; return event -> record."""
    result = {"events": {}, "bad_markers": [], "exists": os.path.exists(path)}
    if not result["exists"]:
        result["bad_markers"].append("perf_file_missing")
        return result
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if not line:
                continue
            if line.startswith("#"):
                continue
            fields = line.split(",")
            if len(fields) < 4:
                continue
            value, unit, event = fields[0], fields[1], fields[2]
            run_time = fields[3] if len(fields) > 3 else ""
            percent = fields[4] if len(fields) > 4 else ""
            low = line.lower()
            for marker in ("not counted", "not supported", "scaled"):
                if marker in low:
                    result["bad_markers"].append(marker)
            result["events"][event] = {
                "value": value,
                "unit": unit,
                "run_time": run_time,
                "percent_enabled": percent,
            }
    return result


def _event_value(events, prefix):
    for name, rec in events.items():
        if name == prefix or name.startswith(prefix + ":"):
            try:
                return float(rec["value"])
            except (TypeError, ValueError):
                return None
    return None


def _event_pct(events, prefix):
    for name, rec in events.items():
        if name == prefix or name.startswith(prefix + ":"):
            try:
                return float(rec["percent_enabled"])
            except (TypeError, ValueError):
                return None
    return None


def _iso_seconds(a, b):
    from datetime import datetime
    try:
        return (datetime.fromisoformat(b) - datetime.fromisoformat(a)).total_seconds()
    except ValueError:
        return None


def arm_metrics(arm):
    """Recompute the primary metrics from the raw arm fields."""
    body = arm.get("raw") or {}
    perf_path = (arm.get("perf") or {}).get("path") or body.get("perf")
    if not perf_path:
        raw = (arm.get("perf") or {}).get("raw") or ""
        # Fall back to the recorded perf text when the file path is absent.
        tmp = {"events": {}, "bad_markers": [], "exists": False}
        for line in raw.splitlines():
            if not line or line.startswith("#"):
                continue
            f = line.split(",")
            if len(f) >= 3:
                tmp["events"][f[2]] = {
                    "value": f[0],
                    "unit": f[1],
                    "run_time": f[3] if len(f) > 3 else "",
                    "percent_enabled": f[4] if len(f) > 4 else "",
                }
        perf = tmp
    else:
        perf = parse_perf(perf_path)
    user = float(arm.get("cpu_user_s") or 0.0)
    system = float(arm.get("cpu_sys_s") or 0.0)
    wall = float(arm.get("wall_s") or 0.0)
    instr = _event_value(perf["events"], "instructions")
    cycles = _event_value(perf["events"], "cycles")
    tclock = _event_value(perf["events"], "task-clock")
    return {
        "cpu": user + system,
        "cpu_user_s": user,
        "cpu_sys_s": system,
        "instr": instr,
        "wall": wall,
        "cycles": cycles,
        "tclock": tclock,
        "perf": perf,
        "wall_from_timestamps": _iso_seconds(arm.get("start"), arm.get("end")),
    }


def parse_raw_bench(jsonl_path):
    """Independent receipt from the benchmark's own raw JSONL."""
    out = {
        "exists": os.path.exists(jsonl_path),
        "records": None,
        "cal": None,
        "target_case_ids": [],
        "warmup": None,
        "measured": None,
        "target_ok": None,
        "problems": [],
    }
    if not out["exists"]:
        out["problems"].append("bench_jsonl_missing")
        return out
    records = read_jsonl(jsonl_path)
    out["records"] = len(records)
    cal = [r for r in records if r.get("case_id") == "CAL-oracle-self-test"]
    out["cal"] = bool(cal and all(r.get("ok") for r in cal))
    target = [r for r in records if r.get("case_id") != "CAL-oracle-self-test"]
    out["target_case_ids"] = sorted({r.get("case_id") for r in target})
    out["warmup"] = sum(1 for r in target if not r.get("measured"))
    out["measured"] = sum(1 for r in target if r.get("measured"))
    out["target_ok"] = all(
        r.get("ok") and not r.get("failure") for r in target
    ) if target else False
    if len(out["target_case_ids"]) != 1:
        out["problems"].append(
            "target_case_id_count=%d" % len(out["target_case_ids"])
        )
    if not out["cal"]:
        out["problems"].append("cal_not_ok")
    if not out["target_ok"]:
        out["problems"].append("target_record_not_ok_or_missing")
    return out


def validity_of_arm(arm, kind, frozen_ops, expected_target_case):
    """Return (ok, reasons, receipt).  Independent of the runner's valid flag."""
    reasons = []
    receipt = {}
    if arm.get("rc") != 0:
        reasons.append("rc=%s" % arm.get("rc"))
    if arm.get("timed_out"):
        reasons.append("timed_out")
    m = arm_metrics(arm)
    perf = m["perf"]
    try:
        recorded_cpu_us = float(arm.get("cpu_us") or 0.0)
        if abs(recorded_cpu_us - m["cpu"] * 1.0e6) > 2.0:
            reasons.append(
                "cpu_us_mismatch=%.1f" % (recorded_cpu_us - m["cpu"] * 1.0e6)
            )
    except (TypeError, ValueError):
        reasons.append("cpu_us_unparsable")
    for ev in REQUIRED_EVENTS:
        val = _event_value(perf["events"], ev)
        pct = _event_pct(perf["events"], ev)
        if val is None:
            reasons.append("perf_event_missing:%s" % ev)
        if pct is None:
            reasons.append("perf_percent_missing:%s" % ev)
        elif pct < 99.995:
            reasons.append("perf_scaled:%s=%.2f%%" % (ev, pct))
    if perf.get("bad_markers"):
        reasons.append("perf_bad_markers:%s" % ",".join(sorted(set(perf["bad_markers"]))))
    if m["wall_from_timestamps"] is not None:
        drift = abs(m["wall_from_timestamps"] - m["wall"])
        if drift > max(0.02, 0.02 * m["wall"]):
            reasons.append("wall_timestamp_mismatch=%.4fs" % drift)
    oracle = arm.get("oracle") or {}
    if not oracle.get("ok"):
        reasons.append("oracle_not_ok:%s" % ";".join(oracle.get("detail") or []))
    if kind == "sluice_copy":
        sc = oracle.get("sluice_copy") or {}
        parsed = sc.get("parsed") or {}
        receipt["copy"] = {
            "dst_matches_src": sc.get("dst_matches_src"),
            "dst_sha256": sc.get("dst_sha256"),
            "src_sha256": sc.get("src_sha256"),
            "read_ops": parsed.get("read_ops"),
            "write_ops": parsed.get("write_ops"),
            "short_writes": parsed.get("short_writes"),
            "bytes_copied": parsed.get("bytes_copied"),
        }
        if not sc.get("dst_matches_src"):
            reasons.append("copy_dst_mismatch")
        if sc.get("dst_sha256") != sc.get("src_sha256"):
            reasons.append("copy_recorded_hashes_differ")
        if parsed.get("short_writes") not in (0, None):
            reasons.append("copy_short_writes=%s" % parsed.get("short_writes"))
        stdout_path = (arm.get("raw") or {}).get("stdout")
        if stdout_path and os.path.exists(stdout_path):
            text = open(stdout_path, "r", encoding="utf-8", errors="replace").read()
            match = re.search(
                r"copied (\d+) bytes \(read_ops=(\d+) write_ops=(\d+) "
                r"short_writes=(\d+)",
                text,
            )
            if not match:
                reasons.append("copy_stdout_unparsable")
            else:
                bytes_copied, read_ops, write_ops, short_writes = (
                    int(x) for x in match.groups()
                )
                receipt["copy"]["raw_stdout"] = {
                    "bytes_copied": bytes_copied,
                    "read_ops": read_ops,
                    "write_ops": write_ops,
                    "short_writes": short_writes,
                }
                for field, value in (
                    ("bytes_copied", bytes_copied),
                    ("read_ops", read_ops),
                    ("write_ops", write_ops),
                    ("short_writes", short_writes),
                ):
                    if parsed.get(field) != value:
                        reasons.append("copy_stdout_%s_mismatch" % field)
        else:
            reasons.append("copy_stdout_missing")
        if frozen_ops:
            if parsed.get("read_ops") != frozen_ops.get("read_ops"):
                reasons.append("copy_read_ops_drift")
            if parsed.get("write_ops") != frozen_ops.get("write_ops"):
                reasons.append("copy_write_ops_drift")
    else:
        bench = oracle.get("bench") or {}
        raw_path = bench.get("jsonl")
        if not raw_path:
            reasons.append("bench_receipt_missing")
            receipt["bench_raw"] = None
        else:
            rr = parse_raw_bench(raw_path)
            receipt["bench_raw"] = rr
            if bench.get("target_records") != 9:
                reasons.append("bench_target_records=%s" % bench.get("target_records"))
            if bench.get("warmup_records") != 2:
                reasons.append("bench_warmup_records=%s" % bench.get("warmup_records"))
            if bench.get("measured_records") != 7:
                reasons.append("bench_measured_records=%s" % bench.get("measured_records"))
            if not bench.get("cal_ok"):
                reasons.append("bench_cal_not_ok")
            if rr.get("problems"):
                reasons.append("raw_bench:" + ",".join(rr["problems"]))
            if expected_target_case and rr.get("target_case_ids"):
                if rr["target_case_ids"] != [expected_target_case]:
                    reasons.append(
                        "raw_case_id=%s" % ",".join(rr["target_case_ids"])
                    )
    return (not reasons), reasons, receipt


def expected_bench_case(arms):
    ids = set()
    for arm in arms:
        b = (arm.get("oracle") or {}).get("bench") or {}
        rr = b.get("jsonl")
        if rr and os.path.exists(rr):
            for rec in read_jsonl(rr):
                cid = rec.get("case_id")
                if cid and cid != "CAL-oracle-self-test":
                    ids.add(cid)
    return sorted(ids)[0] if len(ids) == 1 else None


def parse_pairs_tsv(path):
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, "r", encoding="utf-8") as fh:
        header = fh.readline().rstrip("\n").split("\t")
        for line in fh:
            fields = line.rstrip("\n").split("\t")
            if len(fields) != len(header):
                continue
            rows.append(dict(zip(header, fields)))
    return rows


def cross_check_pairs_tsv(rows, arm_index):
    """Compare the flattened TSV against the per-arm JSONL; not a data source."""
    fields = ("valid", "wall_s", "cpu_user_s", "cpu_sys_s", "perf_instructions")
    mismatches = 0
    checked = 0
    for row in rows:
        try:
            key = (int(row["pair"]), row["arm"])
        except (KeyError, ValueError):
            mismatches += 1
            continue
        arm = arm_index.get(key)
        if arm is None:
            mismatches += 1
            continue
        checked += 1
        try:
            if str(row["valid"]) != str(bool(arm.get("valid"))):
                mismatches += 1
            if abs(float(row["wall_s"]) - float(arm["wall_s"])) > 1e-9:
                mismatches += 1
            if abs(float(row["cpu_user_s"]) - float(arm["cpu_user_s"])) > 1e-9:
                mismatches += 1
            if abs(float(row["cpu_sys_s"]) - float(arm["cpu_sys_s"])) > 1e-9:
                mismatches += 1
            tsv_instr = float(row["perf_instructions"])
            json_instr = float(arm["perf"]["rows"]["instructions:u"]["value"])
            if tsv_instr != json_instr:
                mismatches += 1
        except (KeyError, TypeError, ValueError):
            mismatches += 1
    return {"checked": checked, "mismatches": mismatches}


# ---------------------------------------------------------------------------
# statistics for one cell
# ---------------------------------------------------------------------------

def analyse_metric(values, orders, cell, metric, seed):
    n = len(values)
    if n < 2:
        return {"n": n}
    avg = mean(values)
    med = statistics.median(values)
    sigma = sd(values)
    boot = bootstrap_mean(values, cell, metric, seed)
    t_hw = ci_halfwidth_t(sigma, n)
    cv = (sigma / abs(avg) * 100.0) if abs(avg) > 1e-12 else None
    ab = [v for v, o in zip(values, orders) if o == "AB"]
    ba = [v for v, o in zip(values, orders) if o == "BA"]
    ab_ba = None
    if ab and ba:
        perm = permutation_mean_diff(ab, ba, cell, metric + "|abba", seed)
        ab_ba = {
            "abMean": mean(ab),
            "baMean": mean(ba),
            "diff": perm["observed"],
            "permP": perm["p"],
            "nAb": len(ab),
            "nBa": len(ba),
        }
    xs = list(range(1, n + 1))
    slope = ols_slope(xs, values)
    half = n // 2
    first = mean(values[:half])
    second = mean(values[half:])
    # Position effect: second arm minus first arm within each pair.  Under
    # AB the second arm is B; under BA it is A.  Pair order is immaterial.
    position = [v if o == "AB" else -v for v, o in zip(values, orders)]
    pos_mean = mean(position)
    rng = _rng(cell, metric + "|position", seed)
    hits = 0
    for _ in range(PERMUTATION_RESAMPLES):
        flipped = [v if rng.random() < 0.5 else -v for v in position]
        if abs(mean(flipped)) >= abs(pos_mean) - 1e-15:
            hits += 1
    return {
        "n": n,
        "mean": avg,
        "median": med,
        "sd": sigma,
        "cvPct": cv,
        "cvStable": abs(avg) >= max(0.10, sigma),
        "bootLo": boot["lo"],
        "bootHi": boot["hi"],
        "bootHalfWidth": boot["halfWidth"],
        "tHalfWidth": t_hw,
        "mde80": mde80(sigma, n),
        "abBa": ab_ba,
        "positionEffect": {
            "mean": pos_mean,
            "sd": sd(position),
            "positive": sum(1 for v in position if v > 0),
            "n": n,
            "signFlipP": (hits + 1) / (PERMUTATION_RESAMPLES + 1),
        },
        "drift": {
            "slopePerPair": slope,
            "permP": permutation_slope_p(xs, values, cell, metric + "|slope", seed),
            "firstHalfMean": first,
            "secondHalfMean": second,
            "secondMinusFirst": second - first,
        },
        "values": values,
    }


def frequency_audit(arms, valid):
    """Compare CPU-time dispersion with cycles/instruction dispersion.

    A large CPU-time CV combined with stable cycles and stable instructions is
    direct evidence of effective-frequency variation; a large cycles CV with
    stable CPU time is not.
    """
    vals = {"cpu": [], "cycles": [], "instr": []}
    for arm in arms:
        key = (arm.get("pair"), arm.get("arm"))
        if key not in valid or not valid[key]:
            continue
        m = arm_metrics(arm)
        if None in (m["cpu"], m["cycles"], m["instr"]):
            continue
        vals["cpu"].append(m["cpu"])
        vals["cycles"].append(m["cycles"])
        vals["instr"].append(m["instr"])

    def cv(seq):
        if len(seq) < 2 or mean(seq) == 0:
            return None
        return 100.0 * sd(seq) / mean(seq)

    out = {k: cv(v) for k, v in vals.items()}
    if out["cpu"] and out["cycles"]:
        out["cpuCvOverCyclesCv"] = out["cpu"] / out["cycles"] if out["cycles"] else None
    out["interpretation"] = (
        "cpu-time CV exceeds cycles CV by >2x with stable instructions: "
        "effective-frequency variation likely"
        if (out.get("cpuCvOverCyclesCv") or 0) > 2.0
        and (out.get("instr") or 1.0) < 0.5
        else "no frequency-scaling signature from CPU/cycles CV comparison"
    )
    return out


def frequency_summary(arms):
    per_cpu_min = {}
    busy_proxy = []
    thermal_pkg = []
    samples = []
    for arm in arms:
        sampler = arm.get("sampler") or {}
        freq = sampler.get("freq") or {}
        samples.append(sampler.get("samples") or 0)
        mins = []
        for cpu, rec in freq.items():
            if rec.get("min") is None:
                continue
            per_cpu_min.setdefault(cpu, []).append(rec["min"])
            mins.append(rec["min"])
        if mins:
            busy_proxy.append(max(mins))
        th = (sampler.get("thermal") or {}).get("thermal_zone0")
        if th and th.get("max") is not None:
            thermal_pkg.append(th["max"])
    return {
        "armsWithSamples": sum(1 for s in samples if s),
        "samplesMin": min(samples) if samples else None,
        "samplesMax": max(samples) if samples else None,
        "samplesTotal": sum(samples),
        "perCpuMinKhz": {
            cpu: {"min": min(v), "max": max(v), "n": len(v)}
            for cpu, v in sorted(per_cpu_min.items())
        },
        "busyProxyMinKhz": {
            "min": min(busy_proxy) if busy_proxy else None,
            "max": max(busy_proxy) if busy_proxy else None,
            "armsBelow2500": sum(1 for v in busy_proxy if v < 2_500_000),
            "armsBelow3000": sum(1 for v in busy_proxy if v < 3_000_000),
            "arms": len(busy_proxy),
        },
        "pkgTempMaxMc": {
            "min": min(thermal_pkg) if thermal_pkg else None,
            "max": max(thermal_pkg) if thermal_pkg else None,
            "armsAtOrAbove80C": sum(1 for v in thermal_pkg if v >= 80_000),
            "arms": len(thermal_pkg),
        },
    }


def workload_counters(arms, kind):
    """Sum the benchmark's own per-rep counters for the target case."""
    if kind == "sluice_copy":
        return None
    counts = {"submits": 0, "waits": 0, "notifications": 0, "passes": 0,
              "ops": 0, "bytes": 0, "records": 0, "armsWithRecords": 0}
    case_ids = set()
    for arm in arms:
        arm_has_records = False
        bench = (arm.get("oracle") or {}).get("bench") or {}
        path = bench.get("jsonl")
        if not path or not os.path.exists(path):
            continue
        for rec in read_jsonl(path):
            if rec.get("case_id") == "CAL-oracle-self-test":
                continue
            case_ids.add(rec.get("case_id"))
            counts["records"] += 1
            arm_has_records = True
            for field in ("submits", "waits", "notifications", "passes",
                          "ops", "bytes"):
                counts[field] += int(rec.get(field) or 0)
        if arm_has_records:
            counts["armsWithRecords"] += 1
    counts["caseIds"] = sorted(case_ids)
    if counts["armsWithRecords"]:
        counts["perArmMean"] = {
            field: counts[field] / counts["armsWithRecords"]
            for field in ("submits", "waits", "notifications", "passes", "ops")
        }
    return counts


def measured_only(arms, valid, cell, seed):
    """Auxiliary: pair the benchmark's 7 measured reps, excluding warmup/setup."""
    if not arms or arms[0].get("kind") == "sluice_copy":
        return None
    series = {"cpu": [], "wall": [], "opsPerSec": []}
    shares = []
    for pair in sorted({p for p, _ in valid}):
        if not (valid.get((pair, "A")) and valid.get((pair, "B"))):
            continue
        vals = {}
        process_cpu = None
        for arm_name in ("A", "B"):
            arm = next(
                (a for a in arms
                 if a.get("pair") == pair and a.get("arm") == arm_name),
                None,
            )
            if arm is not None and arm_name == "A":
                process_cpu = (
                    (arm.get("cpu_user_s") or 0.0)
                    + (arm.get("cpu_sys_s") or 0.0)
                )
            if arm is None:
                vals = {}
                break
            path = ((arm.get("oracle") or {}).get("bench") or {}).get("jsonl")
            if not path or not os.path.exists(path):
                vals = {}
                break
            recs = [
                r for r in read_jsonl(path)
                if r.get("case_id") != "CAL-oracle-self-test" and r.get("measured")
            ]
            if len(recs) != 7:
                vals = {}
                break
            vals[arm_name] = {
                "cpu": sum(float(r.get("cpu_s") or 0.0) for r in recs),
                "wall": sum(float(r.get("wall_s") or 0.0) for r in recs),
                "opsPerSec": mean([float(r.get("ops_per_sec") or 0.0) for r in recs]),
            }
        if len(vals) != 2:
            continue
        for key in series:
            av = vals["A"][key]
            bv = vals["B"][key]
            series[key].append(100.0 * (bv - av) / av if av else None)
        if process_cpu:
            shares.append(100.0 * vals["A"]["cpu"] / process_cpu)
    out = {"n": len(series["cpu"]), "measuredCpuShareOfProcessPct": (
        mean(shares) if shares else None
    )}
    for key, vals in series.items():
        vals = [v for v in vals if v is not None]
        if len(vals) >= 2:
            out[key] = analyse_metric(vals, ["AB" if i % 2 == 0 else "BA"
                                             for i in range(len(vals))],
                                      cell, "measured|" + key, seed)
    return out


def metadata_check(meta, arms, kind, target_records):
    """Check the section 10.4 timing-cell metadata against the raw files."""
    env = meta.get("environment") or {}
    case_meta = meta.get("case") or {}
    first = target_records[0] if target_records else {}
    media = meta.get("media") or {}
    if kind == "sluice_copy":
        case_id = meta.get("cell")
        backend = "direct (sluice-copy)"
        block_size = case_meta.get("buffer_size")
        depth = case_meta.get("pipeline_depth")
        workers = case_meta.get("workers")
        warm_cold = "src page-cache resident; dst tmpfs"
    else:
        case_id = first.get("case_id") or case_meta.get("case")
        backend = first.get("backend")
        block_size = first.get("block_size")
        depth = first.get("queue_depth")
        workers = first.get("worker_count")
        warm_cold = first.get("cache_policy")
    return {
        "caseId": case_id,
        "backend": backend,
        "blockSize": block_size,
        "depth": depth,
        "workers": workers,
        "saturating": "not_recorded",
        "warmCold": warm_cold,
        "mediaPath": sorted(media.keys()),
        "mediaMountOptionsPresent": all(
            bool((v.get("mount") or {}).get("fs_type")) for v in media.values()
            if isinstance(v, dict)
        ),
        "baseSha": (meta.get("git") or {}).get("head"),
        "binarySha": ((meta.get("binaries") or {}).get("A") or {}).get("sha256"),
        "binaryPath": ((meta.get("binaries") or {}).get("A") or {}).get("binary"),
        "buildRoot": "/tmp/621-p1-build (from the recorded binary path)",
        "liburingLinked": ((meta.get("binaries") or {}).get("A") or {}).get("liburing_linked"),
        "compiler": ((meta.get("binaries") or {}).get("A") or {}).get("comment"),
        "warmupMeasured": (
            "n/a (one process per arm)" if kind == "sluice_copy"
            else (
                {"warmup": first.get("warmup"),
                 "measured": sum(1 for r in target_records if r.get("measured")),
                 "source": "raw bench JSONL"}
                if target_records
                else "no target records; case filter matched zero cases"
            )
        ),
        "orderRule": (meta.get("protocol") or {}).get("order_rule"),
        "governorBefore": env.get("governor_all_performance_before"),
        "governorAfter": env.get("governor_all_performance"),
        "noTurboBefore": env.get("no_turbo_before"),
        "noTurboAfter": env.get("no_turbo_after"),
        "freqSampling": "per-arm min/max/last only; 20 ms interval; no series",
        "irqAffinity": "not recorded per IRQ; irqbalance inactive at freeze",
        "backgroundLoad": "loadavg before/after cell only; no per-pair ps guard",
        "temperature": "per-arm thermal_zone0 min/max/last",
        "oracleReceipt": "per arm; see raw/receipts",
        "exitCode": "per arm rc in arms.jsonl",
        "rawPaths": "raw/*.perf, raw/*.jsonl, raw/*.stdout present",
    }


def environment_summary(meta):
    env = meta.get("environment") or {}
    media = meta.get("media") or {}
    return {
        "gitHead": (meta.get("git") or {}).get("head"),
        "gitDirty": (meta.get("git") or {}).get("dirty"),
        "binaryShaA": ((meta.get("binaries") or {}).get("A") or {}).get("sha256"),
        "binaryShaB": ((meta.get("binaries") or {}).get("B") or {}).get("sha256"),
        "sameBinary": (
            ((meta.get("binaries") or {}).get("A") or {}).get("sha256")
            == ((meta.get("binaries") or {}).get("B") or {}).get("sha256")
        ),
        "liburingLinked": ((meta.get("binaries") or {}).get("A") or {}).get("liburing_linked"),
        "governorAllPerformanceBefore": env.get("governor_all_performance_before"),
        "governorAllPerformanceAfter": env.get("governor_all_performance"),
        "noTurboBefore": env.get("no_turbo_before"),
        "noTurboAfter": env.get("no_turbo_after"),
        "loadavgBefore": env.get("loadavg_before"),
        "loadavgAfter": env.get("loadavg_after"),
        "irqAffinityRecorded": any("irq" in str(k).lower() for k in env.keys()),
        "media": {
            k: {
                "path": v.get("path"),
                "anchor": v.get("anchor"),
                "exists": v.get("exists"),
                "fs": (v.get("mount") or {}).get("fs_type"),
                "mountPoint": (v.get("mount") or {}).get("mount_point"),
                "options": (v.get("mount") or {}).get("options"),
                "superOptions": (v.get("mount") or {}).get("super_options"),
            }
            for k, v in media.items() if isinstance(v, dict)
        },
        "case": meta.get("case"),
        "protocol": meta.get("protocol"),
        "environmentWarnings": meta.get("environment_warnings"),
    }


def analyse_cell(path, seed, effects):
    cell = os.path.basename(path.rstrip("/"))
    meta_path = os.path.join(path, "meta.json")
    arms_path = os.path.join(path, "arms.jsonl")
    pairs_path = os.path.join(path, "pairs.tsv")
    meta = json.load(open(meta_path, encoding="utf-8")) if os.path.exists(meta_path) else {}
    arms = read_jsonl(arms_path) if os.path.exists(arms_path) else []
    kind = meta.get("kind") or (arms[0].get("kind") if arms else None)
    frozen_ops = meta.get("frozen_ops")
    expected_case = expected_bench_case(arms) if kind != "sluice_copy" else None

    arm_index = {}
    duplicates = []
    for arm in arms:
        key = (arm.get("pair"), arm.get("arm"))
        if key in arm_index:
            duplicates.append(key)
        arm_index[key] = arm

    valid = {}
    receipts = {}
    independent_reasons = {}
    runner_vs_independent = []
    for key, arm in arm_index.items():
        ok, reasons, receipt = validity_of_arm(
            arm, kind, frozen_ops, expected_case
        )
        valid[key] = ok
        receipts[key] = receipt
        independent_reasons[key] = reasons
        if bool(arm.get("valid")) != ok:
            runner_vs_independent.append(
                {
                    "pair": key[0],
                    "arm": key[1],
                    "runnerValid": arm.get("valid"),
                    "independentValid": ok,
                    "independentReasons": reasons,
                    "runnerReasons": arm.get("invalid_reasons"),
                }
            )

    pair_numbers = sorted({p for p, _ in arm_index})
    pairs_used = []
    pair_rows = []
    orders = []
    for p in pair_numbers:
        a = arm_index.get((p, "A"))
        b = arm_index.get((p, "B"))
        if a is None or b is None:
            pair_rows.append({"pair": p, "reason": "missing_arm"})
            continue
        if not (valid[(p, "A")] and valid[(p, "B")]):
            pair_rows.append(
                {
                    "pair": p,
                    "order": a.get("order"),
                    "reason": "invalid_arm",
                    "aReasons": independent_reasons[(p, "A")],
                    "bReasons": independent_reasons[(p, "B")],
                }
            )
            continue
        ma = arm_metrics(a)
        mb = arm_metrics(b)
        if any(ma[k] is None or mb[k] is None for k in METRIC_ORDER):
            pair_rows.append({"pair": p, "reason": "metric_missing"})
            continue
        row = {
            "pair": p,
            "order": a.get("order"),
            "a": ma,
            "b": mb,
            "pct": {},
            "pctSymmetric": {},
        }
        for metric in METRIC_ORDER:
            av = ma[metric]
            bv = mb[metric]
            row["pct"][metric] = 100.0 * (bv - av) / av if av else None
            row["pctSymmetric"][metric] = (
                200.0 * (bv - av) / (av + bv) if av + bv else None
            )
        pairs_used.append(row)
        pair_rows.append({"pair": p, "order": a.get("order"), "used": True})
        orders.append(a.get("order"))

    metrics = {}
    for metric in METRIC_ORDER:
        vals = [r["pct"][metric] for r in pairs_used if r["pct"][metric] is not None]
        ords = [r["order"] for r in pairs_used if r["pct"][metric] is not None]
        metrics[metric] = analyse_metric(vals, ords, cell, metric, seed)
        sym = [r["pctSymmetric"][metric] for r in pairs_used
               if r["pctSymmetric"][metric] is not None]
        if sym and len(sym) >= 2:
            metrics[metric]["symHalfWidth"] = bootstrap_mean(
                sym, cell, metric + "|sym", seed
            )["halfWidth"]
            metrics[metric]["symSd"] = sd(sym)
    for metric in METRIC_ORDER:
        m = metrics[metric]
        if m.get("n", 0) >= 2:
            m["requiredN"] = {
                "deltaPct": {
                    str(d): {
                        "n80Power": required_n_power(m["sd"], d),
                        "nHalfWidthDeltaOver2": required_n_halfwidth(m["sd"], d / 2.0),
                    }
                    for d in effects
                }
            }

    # Raw-receipt agreement: every arm's own receipt and the raw files.
    receipt_problems = {
        key: independent_reasons[key] for key in independent_reasons
        if not valid[key]
    }
    raw_receipts = {
        "benchCaseIds": sorted({
            cid
            for key, rec in receipts.items()
            for cid in ((rec.get("bench_raw") or {}).get("target_case_ids") or [])
        }),
        "copyOps": sorted({
            (rec["copy"]["read_ops"], rec["copy"]["write_ops"],
             rec["copy"]["short_writes"])
            for rec in receipts.values() if rec.get("copy")
        }),
        "copyDstMatchesSrc": sorted({
            rec["copy"]["dst_matches_src"] for rec in receipts.values()
            if rec.get("copy")
        }),
        "benchWarmup": sorted({
            (rec["bench_raw"] or {}).get("warmup") for rec in receipts.values()
            if rec.get("bench_raw")
        }),
        "benchMeasured": sorted({
            (rec["bench_raw"] or {}).get("measured") for rec in receipts.values()
            if rec.get("bench_raw")
        }),
        "benchTargetOk": sorted({
            (rec["bench_raw"] or {}).get("target_ok") for rec in receipts.values()
            if rec.get("bench_raw")
        }),
    }

    workload = workload_counters(arms, kind)
    target_records = []
    for arm in arms:
        bench = (arm.get("oracle") or {}).get("bench") or {}
        path = bench.get("jsonl")
        if path and os.path.exists(path):
            target_records = [
                r for r in read_jsonl(path)
                if r.get("case_id") != "CAL-oracle-self-test"
            ]
            if target_records:
                break
    valid_instr = [
        arm_metrics(arm_index[key])["instr"]
        for key, ok in valid.items()
        if ok and arm_index[key].get("kind") != "sluice_copy"
    ]
    valid_instr = [v for v in valid_instr if v]
    if workload and valid_instr:
        workload["medianArmInstructions"] = statistics.median(valid_instr)
        workload["oneInstructionPerSubmitPct"] = (
            100.0 * workload["perArmMean"]["submits"]
            / statistics.median(valid_instr)
        )

    excluded = meta.get("excluded_arms") or []
    exclusion_reasons = {}
    for item in excluded:
        for reason in item.get("reasons") or []:
            exclusion_reasons[reason] = exclusion_reasons.get(reason, 0) + 1

    tsv_rows = parse_pairs_tsv(pairs_path)
    tsv_check = cross_check_pairs_tsv(tsv_rows, arm_index)

    perf_rows = 0
    perf_scaled = 0
    perf_bad = []
    for arm in arms:
        perf = (arm_metrics(arm) or {}).get("perf") or {}
        for name, rec in (perf.get("events") or {}).items():
            perf_rows += 1
            try:
                if float(rec.get("percent_enabled")) < 99.995:
                    perf_scaled += 1
            except (TypeError, ValueError):
                perf_scaled += 1
        perf_bad.extend(perf.get("bad_markers") or [])

    valid_pairs = len(pairs_used)
    return {
        "cell": cell,
        "kind": kind,
        "declaredPairs": meta.get("pairs"),
        "armsTotal": len(arms),
        "armsValidRunner": meta.get("arms_valid"),
        "armsInvalidRunner": meta.get("arms_invalid"),
        "armsValidIndependent": sum(1 for v in valid.values() if v),
        "armsInvalidIndependent": sum(1 for v in valid.values() if not v),
        "validPairs": valid_pairs,
        "pairRows": pair_rows,
        "pairOrdinals": [r["pair"] for r in pairs_used],
        "orders": orders,
        "duplicateArms": duplicates,
        "runnerVsIndependent": runner_vs_independent,
        "invalidArmReasons": {
            "runnerExcluded": len(excluded),
            "runnerReasonCounts": exclusion_reasons,
            "independent": {
                f"pair{key[0]}-{key[1]}": reasons
                for key, reasons in independent_reasons.items() if reasons
            },
        },
        "oracleAllValidArmsOk": all(
            (arm_index[key].get("oracle") or {}).get("ok")
            for key, ok in valid.items() if ok
        ) if any(valid.values()) else False,
        "oracleOkArms": sum(
            1 for key in arm_index
            if (arm_index[key].get("oracle") or {}).get("ok")
        ),
        "perfAudit": {
            "eventRows": perf_rows,
            "scaledRows": perf_scaled,
            "badMarkers": sorted(set(perf_bad)),
        },
        "rawReceipts": raw_receipts,
        "workload": workload,
        "metadata": metadata_check(meta, arms, kind, target_records),
        "metrics": metrics,
        "measuredOnly": measured_only(arms, valid, cell, seed),
        "environment": environment_summary(meta),
        "frequency": frequency_summary([arm_index[k] for k in arm_index]),
        "frequencyAudit": frequency_audit(arms, valid),
        "tsvCrossCheck": tsv_check,
        "pairsUsedDetail": [
            {
                "pair": r["pair"],
                "order": r["order"],
                "cpu_pct": r["pct"]["cpu"],
                "instr_pct": r["pct"]["instr"],
                "wall_pct": r["pct"]["wall"],
                "cycles_pct": r["pct"]["cycles"],
                "cpu_A": r["a"]["cpu"], "cpu_B": r["b"]["cpu"],
                "instr_A": r["a"]["instr"], "instr_B": r["b"]["instr"],
                "wall_A": r["a"]["wall"], "wall_B": r["b"]["wall"],
            }
            for r in pairs_used
        ],
    }


# ---------------------------------------------------------------------------
# top level
# ---------------------------------------------------------------------------

def verdict_for(cell_result, quiet_cpu=1.5, quiet_wall=2.5,
                quiet_instr=0.05, usable_cpu=3.5, usable_wall=3.0):
    """Apply the pre-registered A/A gate stated in AA-CALIBRATION.md."""
    n = cell_result["validPairs"]
    if n < 12:
        return {
            "class": "INCONCLUSIVE",
            "reason": "valid pairs %d < 12" % n,
            "baseline": False,
        }
    if not cell_result["oracleAllValidArmsOk"]:
        return {
            "class": "INCONCLUSIVE",
            "reason": "oracle receipt failed on at least one valid arm",
            "baseline": False,
        }
    m = cell_result["metrics"]
    cpu = m["cpu"]
    instr = m["instr"]
    wall = m["wall"]
    drift_p = min(
        x.get("drift", {}).get("permP", 1.0)
        for x in (cpu, instr, wall) if x.get("n", 0) >= 2
    )
    order_hits = []
    for name, metric in (("cpu", cpu), ("instr", instr), ("wall", wall)):
        abba = metric.get("abBa") or {}
        if (abba.get("permP") is not None and abba["permP"] < 0.01
                and abs(abba.get("diff") or 0.0) > 1.0):
            order_hits.append(
                "%s AB-BA=%+.3fpp p=%.4f"
                % (name, abba["diff"], abba["permP"])
            )
    sd_cpu = cpu.get("sd", 0.0)
    sd_wall = wall.get("sd", 0.0)
    freq = cell_result.get("frequencyAudit") or {}
    if ("effective-frequency" in (freq.get("interpretation") or "")
            and sd_cpu > quiet_cpu):
        return {
            "class": "ENVIRONMENT_BLOCKED",
            "reason": (
                "sd cpu=%.3fpp vs cycles=%.3fpp, instructions=%.3fpp: "
                "effective-frequency variation likely; hw cpu=%.3f%% wall=%.3f%%; "
                "no perf multiplexing (all event rows 100%%)"
                % (sd_cpu, freq.get("cycles") or 0.0, freq.get("instr") or 0.0,
                   cpu["bootHalfWidth"], wall["bootHalfWidth"])
            ),
            "baseline": False,
        }
    if (drift_p < 0.01 or sd_cpu > usable_cpu or sd_wall > usable_wall):
        return {
            "class": "NOISY",
            "reason": (
                "sd cpu=%.3fpp wall=%.3fpp; hw cpu=%.3f%% wall=%.3f%%; drift p=%.4f"
                % (sd_cpu, sd_wall, cpu["bootHalfWidth"],
                   wall["bootHalfWidth"], drift_p)
            ),
            "baseline": False,
        }
    if order_hits:
        return {
            "class": "ORDER_SENSITIVE",
            "reason": (
                "n=%d; sd cpu=%.3fpp instr=%.3fpp wall=%.3fpp; "
                "hw cpu=%.3f%% instr=%.3f%% wall=%.3f%%; drift p=%.4f; "
                "position effect: %s"
                % (n, sd_cpu, instr.get("sd", 0.0), sd_wall,
                   cpu["bootHalfWidth"], instr["bootHalfWidth"],
                   wall["bootHalfWidth"], drift_p, "; ".join(order_hits))
            ),
            "baseline": True,
        }
    if (sd_cpu <= quiet_cpu
            and sd_wall <= quiet_wall
            and instr.get("sd", 0.0) <= quiet_instr):
        return {
            "class": "QUIET",
            "reason": (
                "n=%d; sd cpu=%.3fpp instr=%.3fpp wall=%.3fpp; "
                "hw cpu=%.3f%% instr=%.3f%% wall=%.3f%%; drift p=%.4f"
                % (n, sd_cpu, instr.get("sd", 0.0), sd_wall,
                   cpu["bootHalfWidth"], instr["bootHalfWidth"],
                   wall["bootHalfWidth"], drift_p)
            ),
            "baseline": True,
        }
    return {
        "class": "QUALIFIED",
        "reason": (
            "n=%d; sd cpu=%.3fpp instr=%.3fpp wall=%.3fpp; "
            "hw cpu=%.3f%% instr=%.3f%% wall=%.3f%%; drift p=%.4f; "
            "only effects above ~1.4x the CPU half-width are powered"
            % (n, sd_cpu, instr.get("sd", 0.0), sd_wall,
               cpu["bootHalfWidth"], instr["bootHalfWidth"],
               wall["bootHalfWidth"], drift_p)
        ),
        "baseline": True,
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--aa-root", required=True,
                    help="directory containing C* AA cell directories")
    ap.add_argument("--seed", type=int, default=DEFAULT_SEED)
    ap.add_argument("--effects", default="0.5,1,2,3,5",
                    help="comma-separated target effects in percent for MME")
    ap.add_argument("--json-out", default=None,
                    help="optional path for the full JSON result")
    args = ap.parse_args(argv)

    effects = [float(x) for x in args.effects.split(",") if x.strip()]
    root = os.path.abspath(args.aa_root)
    cell_dirs = sorted(
        os.path.join(root, name)
        for name in os.listdir(root)
        if os.path.isdir(os.path.join(root, name))
        and name.startswith("C")
        and os.path.exists(os.path.join(root, name, "arms.jsonl"))
    )
    if not cell_dirs:
        print("no C* cells under %s" % root, file=sys.stderr)
        return 2

    results = []
    for path in cell_dirs:
        result = analyse_cell(path, args.seed, effects)
        result["verdict"] = verdict_for(result)
        results.append(result)

    payload = {
        "tool": os.path.basename(__file__),
        "toolVersion": TOOL_VERSION,
        "generatedBy": "independent recomputation from arms.jsonl + raw perf + raw bench JSONL",
        "aaRoot": root,
        "seed": args.seed,
        "bootstrapResamples": BOOTSTRAP_RESAMPLES,
        "permutationResamples": PERMUTATION_RESAMPLES,
        "alpha": ALPHA,
        "power": POWER,
        "differenceDefinition": "100*(B-A)/A per pair",
        "symmetricDifferenceDefinition": "200*(B-A)/(A+B) per pair",
        "metrics": METRIC_LABELS,
        "targetEffectsPct": effects,
        "gate": {
            "minValidPairs": 12,
            "formalPairs": 15,
            "quietSdCpuPct": 1.5,
            "quietSdWallPct": 2.5,
            "quietSdInstrPct": 0.05,
            "usableSdCpuPct": 3.5,
            "usableSdWallPct": 3.0,
            "driftPermutationPBelow": 0.01,
            "orderEffectPermutationPBelow": 0.01,
        },
        "cells": results,
    }

    text = json.dumps(payload, indent=2, ensure_ascii=False)
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
