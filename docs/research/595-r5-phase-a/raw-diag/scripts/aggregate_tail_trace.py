#!/usr/bin/env python3
"""#595 R5 Phase A aggregate: interval decomposition per protocol section 1.

Inputs: onmode-trace-analysis.json (per-file tables from analyze_tail_trace.py)
and matrix/manifest.json (per-run lat stats). Produces the pooled (all-request)
percentile tables per arm/shape, run-level p99 means (named distinctly from
pooled p99), the >=50% attribution computation, tail-op membership summaries,
and burst-spread summaries. Output: raw-diag/aggregate-summary.json.
"""
import glob
import json
import os

BASE = "/home/jnhu/sluice-595-e5/r5/raw-diag"


def quantile(sorted_vals, q):
    import math
    if not sorted_vals:
        return None
    raw = q * (len(sorted_vals) - 1)
    idx = min(int(math.floor(raw + 0.5)), len(sorted_vals) - 1)
    return sorted_vals[idx]


def pooled(recs_list, fn):
    vals = []
    for recs in recs_list:
        vals.extend(fn(r) for _, r in recs)
    vals.sort()
    return vals


def main():
    per_file = json.load(open(os.path.join(BASE, "onmode-trace-analysis.json")))["files"]
    manifest = json.load(open(os.path.join(BASE, "matrix", "manifest.json")))

    # group measured dumps by (case, arm)
    groups = {}
    for r in manifest["runs"]:
        if r["mode"] != "on" or r["exit"] != 0 or not r["lat"]:
            continue
        dump = [d for d in r["dumps"] if d.startswith("tail-001-")]
        assert len(dump) == 1, (r["phase"], r["pair"], r["dumps"])
        path = os.path.join(BASE, "matrix", f"{r['phase']}", f"p{r['pair']:02d}-{r['pos']}-{r['arm']}", "dump", dump[0])
        groups.setdefault((r["case"], r["arm"]), []).append(
            {"path": path, "pair": r["pair"], "lat": r["lat"]})

    import struct
    REC = struct.Struct("<IIIIQIIqqqqqqq")

    def load_recs(path):
        with open(path, "rb") as f:
            blob = f.read()
        nrec = struct.unpack_from("<I", blob, 16)[0]
        off = 24 + 8 * 18
        out = []
        for i in range(nrec):
            v = REC.unpack_from(blob, off + i * 88)
            out.append(v)  # seq,slot,gen,pres,cookie,flags,pad,t0..t5
        return out

    summary = {}
    for (case, arm), items in sorted(groups.items()):
        assert len(items) == 6, (case, arm, len(items))
        recsets = [(i, load_recs(it["path"])) for i, it in enumerate(items)]
        full = [[(i, r) for r in recs if r[7] and r[8] and r[9] and r[11] and r[12] and r[13]]
                for i, recs in recsets]
        coverage = {
            "runs": len(items),
            "records_total": sum(len(recs) for _, recs in recsets),
            "records_full_join": sum(len(f) for f in full),
        }
        coverage["join_rate"] = round(coverage["records_full_join"] / coverage["records_total"], 6)

        intervals = {
            "prep_T1-T0": lambda r: r[8] - r[7],
            "submit_T2-T1": lambda r: r[9] - r[8],
            "kernel_wake_T3-T2": lambda r: r[11] - r[9],
            "reap_pub_T4-T3": lambda r: r[12] - r[11],
            "observe_T5-T4": lambda r: r[13] - r[12],
            "total_T5-T0": lambda r: r[13] - r[7],
            "wake_part_Tw-T2": lambda r: r[10] - r[9],
            "reap_part_T3-Tw": lambda r: r[11] - r[10],
        }
        pooled_p = {}
        for name, fn in intervals.items():
            vals = pooled(full, fn)
            pooled_p[name] = {
                "p50": quantile(vals, 0.5), "p95": quantile(vals, 0.95),
                "p99": quantile(vals, 0.99), "max": vals[-1],
                "neg_count": sum(1 for v in vals if v < 0), "n": len(vals),
            }
        # run-level p99 of each interval (mean over the 6 runs), named separately
        runmean_p99 = {}
        for name, fn in intervals.items():
            per_run = []
            for f in full:
                v = sorted(fn(r) for _, r in f)
                per_run.append(quantile(v, 0.99))
            runmean_p99[name] = round(sum(per_run) / len(per_run), 1)

        # tail-op membership (pooled top 1% by total)
        allfull = [r for f in full for _, r in f]
        totals = sorted(allfull, key=lambda r: r[13] - r[7])
        k = max(1, int(len(totals) * 0.01))
        tail = totals[-k:]
        body = totals[:-k]
        from collections import Counter
        STOPS = {0: "plain_single", 1: "input_exhausted", 2: "rejected", 3: "cap",
                 4: "sq_budget", 5: "control", 6: "recovery", 7: "ineligible"}
        iv_fns = {
            "prep": lambda r: r[8] - r[7], "submit": lambda r: r[9] - r[8],
            "kernel": lambda r: r[11] - r[9], "reap": lambda r: r[12] - r[11],
            "observe": lambda r: r[13] - r[12], "total": lambda r: r[13] - r[7],
        }

        def memb(rs):
            stop = Counter(STOPS[r[5] & 7] for r in rs)
            cw = Counter((r[5] >> 3) & 0xFF for r in rs)
            out = {
                "n": len(rs),
                "stop_hist": dict(stop),
                "cap_share": round(stop.get("cap", 0) / len(rs), 4),
                "caller_window_hist": dict(sorted(cw.items())),
            }
            for name, fn in iv_fns.items():
                vals = [fn(r) for r in rs]
                out[f"{name}_mean_ns"] = round(sum(vals) / len(vals), 1)
                out[f"{name}_p99_ns"] = quantile(sorted(vals), 0.99)
            return out
        mid_lo = totals[int(len(totals) * 0.40)]
        mid_hi = totals[int(len(totals) * 0.60)]
        mid = [r for r in totals if mid_lo is not r and mid_hi is not r
               and mid_lo[13] - mid_lo[7] <= r[13] - r[7] <= mid_hi[13] - mid_hi[7]]
        membership = {"tail": memb(tail), "body": memb(body), "mid40_60": memb(mid),
                      "tail_threshold_ns": tail[0][13] - tail[0][7]}

        # burst spread per presentation (max T3 - min T3, presentations with >=2 ops)
        by_pres = {}
        for i, f in enumerate(full):
            for _, r in f:
                by_pres.setdefault((i, r[3]), []).append(r[11])
        spreads = sorted(max(t) - min(t) for t in by_pres.values() if len(t) >= 2)
        size_hist = Counter(len(t) for t in by_pres.values())
        # spread by stop kind of the tail presentations
        summary[f"{case}|{arm}"] = {
            "coverage": coverage,
            "pooled_percentiles_ns": pooled_p,
            "run_level_mean_of_p99_ns": runmean_p99,
            "bench_run_lat": {  # from jsonl (harness instrumented subphase)
                "p50_mean": round(sum(x["lat"]["lat_p50_ns"] for x in items) / 6, 1),
                "p95_mean": round(sum(x["lat"]["lat_p95_ns"] for x in items) / 6, 1),
                "p99_mean": round(sum(x["lat"]["lat_p99_ns"] for x in items) / 6, 1),
                "p99_per_run": [x["lat"]["lat_p99_ns"] for x in sorted(items, key=lambda i: i["pair"])],
            },
            "presentation_size_hist": dict(size_hist),
            "burst_spread_ns": {
                "n": len(spreads), "p50": quantile(spreads, 0.5),
                "p95": quantile(spreads, 0.95), "p99": quantile(spreads, 0.99),
                "max": spreads[-1] if spreads else None,
            },
            "tail_membership": membership,
        }

    # attribution computation (lead-level given the perturbation verdict)
    attr = {}
    for case in ("W03-uring-4k-d16", "W03-uring-4k-d4"):
        a0 = summary[f"{case}|A0"]["pooled_percentiles_ns"]
        rr = summary[f"{case}|R"]["pooled_percentiles_ns"]
        gap = rr["total_T5-T0"]["p99"] - a0["total_T5-T0"]["p99"]
        row = {"total_gap_p99_ns": gap}
        for iv in ("prep_T1-T0", "submit_T2-T1", "kernel_wake_T3-T2", "reap_pub_T4-T3",
                   "observe_T5-T4"):
            d = rr[iv]["p99"] - a0[iv]["p99"]
            row[iv] = {"delta_p99_ns": d, "share_of_gap": round(d / gap, 4) if gap else None}
        attr[case] = row
    summary["attribution_pooled"] = attr

    # on/off perturbation check numbers
    def arm_lat(mode, case, arm):
        xs = [r["lat"] for r in manifest["runs"]
              if r["mode"] == mode and r["case"] == case and r["arm"] == arm and r["lat"]]
        return xs
    onoff = {}
    for case in ("W03-uring-4k-d16", "W03-uring-4k-d4"):
        entry = {}
        for q in ("p50", "p95", "p99"):
            k = f"lat_{q}_ns"
            off_a, off_r = arm_lat("off", case, "A0"), arm_lat("off", case, "R")
            on_a, on_r = arm_lat("on", case, "A0"), arm_lat("on", case, "R")
            if off_a and off_r:
                entry[f"off_pair_delta_{q}_pct"] = round(100 * (off_r[0][k] / off_a[0][k] - 1), 2)
            if on_a and on_r:
                ma = sum(x[k] for x in on_a) / len(on_a)
                mr = sum(x[k] for x in on_r) / len(on_r)
                entry[f"on_ratio_of_means_delta_{q}_pct"] = round(100 * (mr / ma - 1), 2)
                pairs = []
                by_pair = {}
                for r in manifest["runs"]:
                    if r["mode"] == "on" and r["case"] == case and r["lat"]:
                        by_pair.setdefault(r["pair"], {})[r["arm"]] = r["lat"][k]
                for p, d in sorted(by_pair.items()):
                    if "A0" in d and "R" in d:
                        pairs.append(100 * (d["R"] / d["A0"] - 1))
                entry[f"on_paired_delta_{q}_pct_per_pair"] = [round(x, 2) for x in pairs]
                entry[f"on_mean_paired_delta_{q}_pct"] = round(sum(pairs) / len(pairs), 2)
        onoff[case] = entry
    summary["on_off_check"] = onoff
    gap99 = onoff["W03-uring-4k-d16"]
    summary["on_off_check_verdict_d16_p99"] = {
        "off_pair_delta_pct": gap99.get("off_pair_delta_p99_pct"),
        "on_ratio_of_means_delta_pct": gap99.get("on_ratio_of_means_delta_p99_pct"),
        "on_mean_paired_delta_pct": gap99.get("on_mean_paired_delta_p99_pct"),
        "gap_pp_ratio_of_means": round(gap99["on_ratio_of_means_delta_p99_pct"] - gap99["off_pair_delta_p99_pct"], 2),
        "gap_pp_mean_paired": round(gap99["on_mean_paired_delta_p99_pct"] - gap99["off_pair_delta_p99_pct"], 2),
        "window_pp": 3.0,
    }

    with open(os.path.join(BASE, "aggregate-summary.json"), "w") as f:
        json.dump(summary, f, indent=1)
    print(json.dumps(summary["on_off_check_verdict_d16_p99"], indent=1))
    print("attribution d16:", json.dumps(summary["attribution_pooled"]["W03-uring-4k-d16"], indent=1))
    print("attribution d4:", json.dumps(summary["attribution_pooled"]["W03-uring-4k-d4"], indent=1))


if __name__ == "__main__":
    main()
