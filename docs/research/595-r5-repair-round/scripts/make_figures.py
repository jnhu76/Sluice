#!/usr/bin/env python3
"""#595 R5-repair figures F1-F5 from recomputable data.

Inputs: analysis JSON (scripts/analyze_round.py --out), per-invocation
results.jsonl, diagnostic ops CSV + perturbation analysis. Outputs: SVG+PNG
per figure plus figures/figure-data/*.csv (one row per plotted value).
No hand-entered numbers: every value comes from the inputs.
"""
import csv
import json
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = "/home/jnhu/sluice-595-e5/r5-repair-measure"
FIG = os.path.join(ROOT, "figures")
DAT = os.path.join(FIG, "figure-data")
os.makedirs(DAT, exist_ok=True)

MAIN = "MAIN-W03-D16"
PROTS = ["PROT-W03-D4", "PROT-W03-D2SERIAL", "PROT-W02-D1", "PROT-W02-D2"]
SHORT = {"MAIN-W03-D16": "W03-d16", "PROT-W03-D4": "W03-d4",
         "PROT-W03-D2SERIAL": "W03-d2ser", "PROT-W02-D1": "W02-d1",
         "PROT-W02-D2": "W02-d2"}


def load():
    with open(os.path.join(ROOT, "analysis", "round-analysis.json")) as f:
        return {r["cell"]: r for r in json.load(f)}


def savefig(fig, name):
    fig.savefig(os.path.join(FIG, name + ".svg"), bbox_inches="tight")
    fig.savefig(os.path.join(FIG, name + ".png"), dpi=150, bbox_inches="tight")
    plt.close(fig)
    print("wrote", name)


def csv_rows(name, header, rows):
    with open(os.path.join(DAT, name + ".csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


def f1(d):
    m = d[MAIN]
    arms = ["A0", "R", "C"]
    labels = ["cpu", "tput", "p95", "p99"]
    titles = ["CPU ns/op (lower better)", "throughput ops/s (higher better)",
              "p95 ns (run-level)", "p99 ns (run-level)"]
    rows = []
    fig, axes = plt.subplots(1, 4, figsize=(16, 4.2))
    for ax, key, title in zip(axes, labels, titles):
        vals = [m["abs"][a][key][0] for a in arms]
        ax.bar(arms, vals, color=["#777", "#4477aa", "#cc8800"])
        ax.set_title(title + f" — {MAIN}")
        for a, v in zip(arms, vals):
            rows.append([key, a, v])
        ax.set_ylabel("ns" if key in ("p95", "p99") else
                      ("ops/s" if key == "tput" else "ns/op"))
    # relative effects + gates
    ax2 = fig.text(0.01, -0.08, "", fontsize=7, family="monospace")
    lines = ["C/A0-1 (point [ci95 lo, hi]; gate) per metric:"]
    for metric, gate in (("cpu", "point<=-10%, 1s95up<=-5%"),
                         ("tput", "1s95lo>=-3%"), ("p95", "1s95up<=+5%"),
                         ("p99", "1s95up<=+5%")):
        c = m["C_vs_A0"][metric]
        bound = c["upper95"] if metric != "tput" else c["lower95"]
        lines.append(f"  {metric:5s} {100*c['point']:+7.2f}% "
                     f"[{100*c['ci95'][0]:+.2f}, {100*c['ci95'][1]:+.2f}] "
                     f"1s95={100*bound:+.2f}%  gate: {gate}")
    ax2.set_text("\n".join(lines))
    for metric in ("cpu", "tput", "p95", "p99"):
        c = m["C_vs_A0"][metric]
        rows.append([f"C_vs_A0_{metric}", "point", c["point"],
                     c["ci95"][0], c["ci95"][1],
                     c["upper95"] if metric != "tput" else c["lower95"]])
    csv_rows("f1-main-absolute", ["metric", "arm", "value"], rows)
    fig.tight_layout()
    savefig(fig, "f1-main-three-arm")


def f2(d):
    rows = []
    cells = [MAIN] + PROTS
    fig, axes = plt.subplots(1, 2, figsize=(13, 4.6), sharey=True)
    for ax, contrast, title in ((axes[0], "C_vs_R", "C−R (cleanup increment)"),
                                (axes[1], "C_vs_A0", "C−A0 (net effect, formal gate)")):
        xs, ys, los, his = [], [], [], []
        for i, cell in enumerate(cells):
            if cell not in d or contrast not in d.get(cell, {}):
                continue
            for j, metric in enumerate(("cpu", "tput", "p95", "p99")):
                c = d[cell][contrast][metric]
                x = i + (j - 1.5) * 0.2
                xs.append(x)
                ys.append(100 * c["point"])
                los.append(100 * c["ci95"][0])
                his.append(100 * c["ci95"][1])
                rows.append([SHORT[cell], contrast, metric, 100 * c["point"],
                             100 * c["ci95"][0], 100 * c["ci95"][1]])
        ax.errorbar(xs, ys, yerr=[[y - l for y, l in zip(ys, los)],
                                  [h - y for y, h in zip(ys, his)]],
                    fmt="o", capsize=3, ms=5)
        ax.axhline(0, color="k", lw=0.8)
        for v, lab, style in ((5, "+5% gate", ":"), (-5, "-5% gate", ":"),
                              (-3, "-3% tput gate", "--")):
            ax.axhline(v, color="r", lw=0.7, ls=style)
            ax.text(0.02, v, lab, fontsize=6, color="r", transform=ax.get_yaxis_transform())
        ax.set_xticks(range(len(cells)))
        ax.set_xticklabels([SHORT[c] for c in cells], fontsize=8)
        ax.set_title(title)
        ax.set_ylabel("% effect")
    csv_rows("f2-cross-shape-effects", ["cell", "contrast", "metric", "point_pct",
                                "ci_lo_pct", "ci_hi_pct"], rows)
    fig.tight_layout()
    savefig(fig, "f2-cross-shape-effects")


def f3(d):
    m = d[MAIN]
    fig, ax = plt.subplots(figsize=(6.5, 5.5))
    rows = []
    # A0 at origin; R and C at (p99 effect, cpu effect)
    ax.axhline(-10, color="r", ls=":", lw=1)
    ax.axvline(5, color="r", ls=":", lw=1)
    ax.add_patch(plt.Rectangle((-100, -100), 105, 90, alpha=0.08, color="green"))
    ax.text(-95, -13, "qualified region (cpu up-bound -5%, p99 up-bound +5%)",
            fontsize=7, color="green")
    for arm, contrast, color in (("R", "R_vs_A0", "#4477aa"),
                                 ("C", "C_vs_A0", "#cc8800")):
        cpu = 100 * m[contrast]["cpu"]["point"]
        p99 = 100 * m[contrast]["p99"]["point"]
        cpu_hi = 100 * m[contrast]["cpu"]["upper95"]
        p99_hi = 100 * m[contrast]["p99"]["upper95"]
        ax.errorbar([p99], [cpu], xerr=[[max(0, p99 - 100 * m[contrast]["p99"]["ci95"][0])],
                                     [max(0, 100 * m[contrast]["p99"]["ci95"][1] - p99)]],
                    yerr=[[max(0, cpu - 100 * m[contrast]["cpu"]["ci95"][0])],
                          [max(0, 100 * m[contrast]["cpu"]["ci95"][1] - cpu)]],
                    fmt="o", capsize=4, color=color, label=f"{arm} vs A0 (point, 95% CI)")
        rows.append([arm, p99, cpu, 100 * m[contrast]["p99"]["ci95"][0],
                     100 * m[contrast]["p99"]["ci95"][1],
                     100 * m[contrast]["cpu"]["ci95"][0],
                     100 * m[contrast]["cpu"]["ci95"][1], p99_hi, cpu_hi])
    ax.plot([0], [0], "ks", label="A0 (origin)")
    ax.set_xlabel("% p99 effect (C or R vs A0)")
    ax.set_ylabel("% CPU/op effect (C or R vs A0)")
    ax.set_title(f"F3 CPU vs p99 — {MAIN}\n(point ≤−10% & 1s95 cpu ≤−5% & 1s95 p99 ≤+5% for the full product gate)")
    ax.legend(fontsize=8)
    csv_rows("f3-cpu-p99-plane", ["arm", "p99_pct", "cpu_pct", "p99_lo", "p99_hi",
                                  "cpu_lo", "cpu_hi", "p99_1s95", "cpu_1s95"], rows)
    fig.tight_layout()
    savefig(fig, "f3-cpu-p99-plane")


def f4(d):
    cell_dir = os.path.join(ROOT, "raw", "main-w03-d16", "main")
    path = os.path.join(cell_dir, "results.jsonl")
    by_block = {}
    with open(path) as f:
        for line in f:
            r = json.loads(line)
            if r.get("valid") and r.get("seg") == "main":
                by_block.setdefault(r["block"], {})[r["arm"]] = r["metrics"]
    rows = []
    fig, axes = plt.subplots(1, 2, figsize=(13, 4.6))
    for ax, metric, key in ((axes[0], "CPU/op effect C−A0 %", "cpu_ns_per_op"),
                            (axes[1], "p99 effect C−A0 %", "p99_ns")):
        xs, ys = [], []
        for b in sorted(by_block):
            m = by_block[b]
            if "A0" not in m or "C" not in m:
                continue
            eff = 100 * (m["C"][key] - m["A0"][key]) / m["A0"][key]
            xs.append(b)
            ys.append(eff)
            rows.append([b, metric, eff])
        ax.plot(xs, ys, "o-", ms=4)
        ax.axhline(0, color="k", lw=0.8)
        ax.set_xlabel("matched block (execution order)")
        ax.set_ylabel(metric)
    csv_rows("f4-block-drift", ["block", "effect", "pct"], rows)
    fig.tight_layout()
    savefig(fig, "f4-block-drift")


def f5():
    ops_path = os.path.join(ROOT, "raw", "diag-ops.csv")
    pert_path = os.path.join(ROOT, "analysis", "diag-perturbation.json")
    if not os.path.exists(ops_path) or not os.path.exists(pert_path):
        print("F5: diagnostic data absent; skipped")
        return
    with open(pert_path) as f:
        pert = json.load(f)
    rows = []
    fig, axes = plt.subplots(1, 2, figsize=(13, 4.6))
    # left: perturbation interaction effects per shape/arm
    ax = axes[0]
    xs, ys, labs = [], [], []
    i = 0
    for shape in sorted(pert):
        for arm in ("R", "C"):
            e = pert[shape].get(arm)
            if not e:
                continue
            xs.append(i)
            ys.append(e["interaction_pp"])
            labs.append(f"{shape}\n{arm}")
            rows.append([shape, arm, e["interaction_pp"], e["ci95_pp"][0], e["ci95_pp"][1],
                         e["within_window"]])
            i += 1
    ax.bar(xs, ys, color=["#4477aa", "#cc8800"] * 4)
    ax.axhline(3, color="r", ls=":")
    ax.axhline(-3, color="r", ls=":")
    ax.set_xticks(xs)
    ax.set_xticklabels(labs, fontsize=7)
    ax.set_ylabel("(X−A0)_on − (X−A0)_off, p99 pp")
    title = "instrument perturbation (±3pp window)"
    if not all(r[5] for r in rows if r):
        title += " — PERTURBED (no formal attribution)"
    ax.set_title(title, color="darkred" if "PERTURBED" in title else "k")
    # right: conditional tail risk by TRUE member position (recovered offline
    # by scripts/recover_f5_position.py — caller_window itself is a per-group
    # constant, the window occupancy at construction, NOT member position).
    ax = axes[1]
    pos_path = os.path.join(ROOT, "analysis", "f5-position.json")
    if os.path.exists(pos_path):
        with open(pos_path) as f:
            posdat = json.load(f)
        series = [
            ("full refill (occupancy 0 at build)", "W03-uring-4k-d16|cw0|pos%d", 16, "#4477aa", "o-"),
            ("half-drain refill (occupancy 8)", "W03-uring-4k-d16|cw8|pos%d", 8, "#cc8800", "s--"),
        ]
        for label, keyf, width, color, style in series:
            xs, ys = [], []
            for p in range(width):
                s = posdat["member_position"].get(keyf % p)
                if s:
                    xs.append(p)
                    ys.append(s["tail_gt55_pct"])
            if xs:
                ax.plot(xs, ys, style, color=color, label=label, ms=4)
                rows.append([f"tailrisk_{label}", len(xs), ys[0], ys[-1]])
        ax.axvline(7.5, color="gray", ls=":", lw=1)
        ax.text(7.5, ax.get_ylim()[1] * 0.95, " presentation quantum=8 boundary",
                fontsize=7, color="gray", va="top")
        ax.set_xlabel("true member position within the built group (recovered)")
        ax.set_ylabel("conditional tail risk P(T0→T5 > 55 µs), %")
        ax.set_ylim(bottom=0)
        ax.legend(fontsize=8)
    ax.set_title("F5 (corrected): tail risk by true member position, d16 R\n"
                 "(lead-level: instrument PERTURBED; NOT a p99 decomposition)")
    csv_rows("f5-diagnostic", ["item", "n_or_arm", "value1", "value2"], rows)
    fig.tight_layout()
    savefig(fig, "f5-diagnostic")


def main():
    d = load()
    f1(d)
    f2(d)
    f3(d)
    f4(d)
    f5()
    print("figures complete")


if __name__ == "__main__":
    main()
