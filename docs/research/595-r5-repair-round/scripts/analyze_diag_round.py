#!/usr/bin/env python3
"""#595 R5-repair diagnostic aggregation.

From raw/diag-*/results.jsonl + raw/diag-dumps/<tag>/: produces
- analysis/diag-perturbation.json: per shape, per arm X in {R, C}:
    (X-A0)_on - (X-A0)_off on p99 (percentage points), bootstrap CI,
    within_window flag (CI inside ±3pp);
- raw/diag-ops.csv: joined v2 ops for on-mode R runs (T0..T5, presentation,
  caller_window, stop_kind, wake_class) for F5.
Diagnostic-only; never mixed with clean cells.
"""
import glob
import json
import os
import random
import subprocess
import sys

ROOT = "/home/jnhu/sluice-595-e5/r5-repair-measure"
RAW = os.path.join(ROOT, "raw")
DUMPS = os.path.join(RAW, "diag-dumps")
BOOT = 10000
SEED = 5950
WINDOW_PP = 3.0


def paired_boot(eff):
    rng = random.Random(SEED)
    n = len(eff)
    means = sorted(sum(eff[rng.randrange(n)] for _ in range(n)) / n for _ in range(BOOT))
    n_ = len(means)
    return means[min(n_ - 1, int(0.025 * n_))], means[min(n_ - 1, int(0.975 * n_))]


def main():
    shapes = ("W03-uring-4k-d16", "W03-uring-4k-d4")
    pert = {}
    for case in shapes:
        seg = f"diag-{case}"
        path = os.path.join(RAW, seg, "results.jsonl")
        if not os.path.exists(path):
            continue
        recs = [json.loads(l) for l in open(path)]
        recs = [r for r in recs if r.get("valid")]
        by_block = {}
        for r in recs:
            by_block.setdefault(r["block"], {})[(r["arm"], r["mode"])] = r
        per_arm = {}
        for arm in ("R", "C"):
            eff_on, eff_off, inter = [], [], []
            for b in sorted(by_block):
                d = by_block[b]
                if not all(k in d for k in (("A0", "on"), ("A0", "off"),
                                            (arm, "on"), (arm, "off"))):
                    continue
                on = (d[(arm, "on")]["metrics"]["p99_ns"] - d[("A0", "on")]["metrics"]["p99_ns"]) \
                    / d[("A0", "on")]["metrics"]["p99_ns"]
                off = (d[(arm, "off")]["metrics"]["p99_ns"] - d[("A0", "off")]["metrics"]["p99_ns"]) \
                    / d[("A0", "off")]["metrics"]["p99_ns"]
                eff_on.append(on)
                eff_off.append(off)
                inter.append(on - off)
            if not inter:
                continue
            point = sum(inter) / len(inter)
            lo, hi = paired_boot(inter)
            per_arm[arm] = {
                "n_blocks": len(inter),
                "effect_on_pct": 100 * sum(eff_on) / len(eff_on),
                "effect_off_pct": 100 * sum(eff_off) / len(eff_off),
                "interaction_pp": 100 * point,
                "ci95_pp": [100 * lo, 100 * hi],
                "within_window": (100 * lo >= -WINDOW_PP and 100 * hi <= WINDOW_PP),
            }
        pert[case] = per_arm
    os.makedirs(os.path.join(ROOT, "analysis"), exist_ok=True)
    with open(os.path.join(ROOT, "analysis", "diag-perturbation.json"), "w") as f:
        json.dump(pert, f, indent=1)
    print(json.dumps(pert, indent=1))

    # joined ops for F5: on-mode runs of every arm, from v2 dumps
    rows = []
    out = open(os.path.join(RAW, "diag-ops.csv"), "w")
    out.write("arm,case,tag,seq,slot,gen,cookie,presentation,caller_window,"
              "stop_kind,t0,t1,t2,t_wake,t3,t4,t5,wake_class,join\n")
    import re
    tag_re = re.compile(r"^(W03-uring-4k-d\d+)-b(\d+)-p(\d+)-(A0|R|C)-(on|off)-a(\d+)$")
    for dtag in sorted(os.listdir(DUMPS)) if os.path.isdir(DUMPS) else []:
        mobj = tag_re.match(dtag)
        if not mobj or mobj.group(5) != "on":
            continue
        arm = mobj.group(4)
        case = mobj.group(1)
        dumps = sorted(glob.glob(os.path.join(DUMPS, dtag, "*.bin")))
        if not dumps:
            continue
        # each process emits one dump per instrumented pass: tail-000 is the
        # warmup rep's, the last is the single measured rep's - keep only the
        # measured one
        dumps = dumps[-1:]
        r = subprocess.run([sys.executable,
                            os.path.join(ROOT, "scripts", "analyze_tail_trace_v2.py"),
                            "--ops-out", "/tmp/diag-ops-one.csv"] + dumps,
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"analyze failed for {dtag}: {r.stderr[:200]}")
            continue
        with open("/tmp/diag-ops-one.csv") as f:
            for i, line in enumerate(f):
                if i == 0:
                    continue
                out.write(f"{arm},{case},{dtag},{line}")
                rows.append(1)
    out.close()
    print(f"diag-ops rows: {len(rows)}")


if __name__ == "__main__":
    main()
