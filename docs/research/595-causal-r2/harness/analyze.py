#!/usr/bin/env python3
"""#595 E5 pairing analysis: pair-relative differences, bootstrap CIs, frozen gates.

Direction convention: every "rel diff" is oriented so POSITIVE means B is worse.
  cpu/p95/p99/wall = (B - A) / A
  tput             = (A - B) / A   (positive = B slower)
  ru_secondary     = (B - A) / A   (whole-invocation rusage; secondary view)

Bootstrap: 10,000 percentile resamples over pairs, fixed seed 5950.
Non-inferiority p = P(bootstrap mean >= threshold); Holm across the control family.
"""
import argparse
import json
import os
import random
import sys

BOOT = 10000
SEED = 5950
CONTROL_LIMITS = {"cpu": 0.05, "p95": 0.05, "p99": 0.05, "tput": 0.03}
AA_LIMITS = {"cpu": 0.05, "tput": 0.03, "p99": 0.03}


def load_records(path):
    recs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                recs.append(json.loads(line))
    return recs


def pairs_of(recs, arms):
    by_pair = {}
    for r in recs:
        by_pair.setdefault(r["pair"], {})[r["pos"]] = r
    out = []
    for pair in sorted(by_pair):
        d = by_pair[pair]
        if 0 not in d or 1 not in d:
            continue
        if arms == "AB":
            a = next(r for r in (d[0], d[1]) if r["arm"] == "A")
            b = next(r for r in (d[0], d[1]) if r["arm"] == "B")
        else:
            a, b = d[0], d[1]
        out.append((a, b))
    return out


def _rel_metric(a, b, key):
    x = a["metrics"][key]
    y = b["metrics"][key]
    if x is None or y is None or x == 0:
        return None
    return (y - x) / x


def _rel_tput(a, b):
    x = a["metrics"]["ops_per_sec"]
    y = b["metrics"]["ops_per_sec"]
    if not x or not y:
        return None
    return (x - y) / x


def _rel_ru(a, b):
    if a["ru_total"] <= 0 or b["ru_total"] <= 0:
        return None
    return (b["ru_total"] - a["ru_total"]) / a["ru_total"]


def bootstrap_ci(values):
    n = len(values)
    if n == 0:
        return None
    rng = random.Random(SEED)
    means = []
    for _ in range(BOOT):
        s = 0.0
        for _ in range(n):
            s += values[rng.randrange(n)]
        means.append(s / n)
    means.sort()
    sv = sorted(values)
    med = sv[n // 2] if n % 2 else (sv[n // 2 - 1] + sv[n // 2]) / 2
    lo = means[int(0.025 * BOOT)]
    hi = means[min(int(0.975 * BOOT), BOOT - 1)]
    return {
        "mean": sum(values) / n,
        "median": med,
        "ci_lo": lo,
        "ci_hi": hi,
        "half_width": (hi - lo) / 2,
        "boot_means": means,
        "values": values,
    }


def noninferiority(stat, threshold):
    means = stat["boot_means"]
    ge = sum(1 for m in means if m >= threshold)
    return (ge + 1) / (len(means) + 1)


def summarize(recs, arms):
    pairs = pairs_of(recs, arms)
    raw = {
        "cpu": [_rel_metric(a, b, "cpu_ns_per_op") for a, b in pairs],
        "p95": [_rel_metric(a, b, "p95_ns") for a, b in pairs],
        "p99": [_rel_metric(a, b, "p99_ns") for a, b in pairs],
        "tput": [_rel_tput(a, b) for a, b in pairs],
        "wall": [_rel_metric(a, b, "wall_s") for a, b in pairs],
        "ru_secondary": [_rel_ru(a, b) for a, b in pairs],
    }
    out = {"n_pairs": len(pairs), "metrics": {}}
    for k, vals in raw.items():
        vals = [v for v in vals if v is not None]
        if vals:
            out["metrics"][k] = bootstrap_ci(vals)
    return out


def public_view(s):
    out = {"n_pairs": s["n_pairs"], "metrics": {}}
    for k, m in s["metrics"].items():
        out["metrics"][k] = {kk: vv for kk, vv in m.items()
                             if kk not in ("boot_means", "values")}
    return out


def print_summary(name, s):
    print(f"== {name}: {s['n_pairs']} pairs ==")
    for k, m in s["metrics"].items():
        print(f"  {k:>13}: mean={m['mean']*100:+7.2f}%  median={m['median']*100:+7.2f}%  "
              f"CI95=[{m['ci_lo']*100:+7.2f}%, {m['ci_hi']*100:+7.2f}%]  "
              f"half-width={m['half_width']*100:5.2f}pp")


def check_aa(s):
    ok = True
    for k, th in AA_LIMITS.items():
        m = s["metrics"].get(k)
        good = m is not None and m["half_width"] <= th
        ok = ok and good
        hw = f"{m['half_width']*100:.2f}pp" if m else "missing"
        print(f"  AA GATE {k}: half-width {hw} <= {th*100:.0f}pp -> "
              f"{'PASS' if good else 'FAIL'}")
    print(f"  AA OVERALL: {'PASS' if ok else 'FAIL'}")
    return ok


def check_ab(s):
    cpu = s["metrics"].get("cpu")
    tput = s["metrics"].get("tput")
    p95 = s["metrics"].get("p95")
    p99 = s["metrics"].get("p99")
    if not all((cpu, tput, p95, p99)):
        print("  AB: missing metrics -> FAIL")
        return False
    net = cpu["mean"] <= -0.10 and cpu["ci_hi"] <= -0.05
    print(f"  NET WIN: cpu mean {cpu['mean']*100:+.2f}% (<= -10), "
          f"CI hi {cpu['ci_hi']*100:+.2f}% (<= -5) -> {'PASS' if net else 'FAIL'}")
    prot = tput["mean"] <= 0.03 and p95["mean"] <= 0.05 and p99["mean"] <= 0.05
    print(f"  PERF PROTECTION: tput {tput['mean']*100:+.2f}% (<= +3), "
          f"p95 {p95['mean']*100:+.2f}% / p99 {p99['mean']*100:+.2f}% (<= +5) -> "
          f"{'PASS' if prot else 'FAIL'}")
    return net and prot


def holm(entries, alpha=0.05):
    m = len(entries)
    order = sorted(range(m), key=lambda i: entries[i]["p"])
    stop = False
    for rank, idx in enumerate(order):
        if stop:
            entries[idx]["holm_alpha"] = None
            entries[idx]["holm_reject"] = False
            continue
        adj = alpha / (m - rank)
        entries[idx]["holm_alpha"] = adj
        if entries[idx]["p"] <= adj:
            entries[idx]["holm_reject"] = True
        else:
            entries[idx]["holm_reject"] = False
            stop = True
    return entries


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p1 = sub.add_parser("summary")
    p1.add_argument("--phase-dir", required=True)
    p1.add_argument("--arms", default="AB", choices=["AB", "AA"])
    p1.add_argument("--gate", default="none", choices=["none", "aa", "ab"])
    p1.add_argument("--out", default=None)

    p2 = sub.add_parser("control-family")
    p2.add_argument("--dirs", nargs="+", required=True)
    p2.add_argument("--out", default=None)

    args = ap.parse_args()

    if args.cmd == "summary":
        recs = load_records(os.path.join(args.phase_dir, "results.jsonl"))
        s = summarize(recs, args.arms)
        print_summary(args.phase_dir, s)
        verdict = None
        if args.gate == "aa":
            verdict = {"aa_pass": check_aa(s)}
        elif args.gate == "ab":
            verdict = {"ab_pass": check_ab(s)}
        out = {"phase_dir": args.phase_dir, "arms": args.arms,
               "summary": public_view(s), "verdict": verdict}
        if args.out:
            with open(args.out, "w") as f:
                json.dump(out, f, indent=1)
            print(f"wrote {args.out}")
        return

    entries = []
    tables = {}
    for d in args.dirs:
        recs = load_records(os.path.join(d, "results.jsonl"))
        s = summarize(recs, "AB")
        tables[os.path.basename(d.rstrip("/"))] = public_view(s)
        for k, th in CONTROL_LIMITS.items():
            m = s["metrics"].get(k)
            if m is None:
                continue
            entries.append({
                "shape": os.path.basename(d.rstrip("/")),
                "metric": k,
                "threshold": th,
                "mean": m["mean"],
                "ci_lo": m["ci_lo"],
                "ci_hi": m["ci_hi"],
                "p": noninferiority(m, th),
            })
    holm(entries)
    print("== negative-control family (one-sided non-inferiority, Holm alpha=0.05) ==")
    all_ok = True
    for e in sorted(entries, key=lambda x: (x["shape"], x["metric"])):
        ok = e["holm_reject"] and e["mean"] <= e["threshold"]
        all_ok = all_ok and ok
        print(f"  {e['shape']:<24} {e['metric']:>4} mean={e['mean']*100:+7.2f}% "
              f"ci=[{e['ci_lo']*100:+7.2f},{e['ci_hi']*100:+7.2f}] "
              f"thr={e['threshold']*100:.0f}% p={e['p']:.4f} "
              f"holm_a={e['holm_alpha'] if e['holm_alpha'] is None else round(e['holm_alpha'], 5)} "
              f"-> {'SAFE' if ok else 'FAIL'}")
    print(f"== controls overall: {'PASS' if all_ok else 'FAIL'} ==")
    if args.out:
        with open(args.out, "w") as f:
            json.dump({"entries": entries, "tables": tables,
                       "all_pass": all_ok}, f, indent=1)
        print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
