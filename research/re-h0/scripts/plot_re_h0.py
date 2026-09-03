#!/usr/bin/env python3
"""plot_re_h0.py — derived SVG plots for RE-H0 sessions (#277).

Reads results/<session>/summary.json (+ analysis.json where present) and
emits SVG plots under research/re-h0/plots/ (preregistration P7/P11;
SVG only, readable, no 3D surfaces):

  z-ladder-instructions-per-op.svg   instr/op by arm, per (fs, cell, op)
  z-ladder-cost-ratios.svg           frozen decomposition ratios
  z-ladder-throughput.svg            median wall throughput by arm
  re2-envelope-instructions.svg      RE-2 pair instr ratios across cells
  re2-envelope-throughput.svg        RE-2 median throughput across cells
  threadpool-vs-uring-regimes.svg    Sluice L2 vs Sluice Z2 mechanism view
"""

import argparse
import json
import pathlib
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def savefig_clean(fig, path):
    """matplotlib writes trailing spaces inside <path d="..."> lines; the
    repo gate (git diff --check) rejects them. Strip per-line trailing ws
    without touching rendering."""
    fig.savefig(path)
    p = pathlib.Path(path)
    text = p.read_text()
    p.write_text("".join(line.rstrip() + "\n" for line in
                         text.splitlines()))

REPO = pathlib.Path(__file__).resolve().parents[3]
RESULTS = REPO / "research/re-h0/results"
PLOTS = REPO / "research/re-h0/plots"

ARMS_Z = ["z1", "z1b", "z1bw", "z2", "z3"]
ARMS_E1 = ["L0", "L1", "L2"]


def load(session):
    rows = json.loads((RESULTS / session / "summary.json").read_text())
    analysis_path = RESULTS / session / "analysis.json"
    analysis = (json.loads(analysis_path.read_text())
                if analysis_path.exists() else {"blocks": []})
    return rows, analysis


def med(values):
    s = sorted(values)
    return s[len(s) // 2]


def combo_key(r):
    return (r["fs"], r.get("cell"), r["op"])


def throughput_mibs(r):
    w = med(r["wall_ns_per_op_samples"])
    if w <= 0:
        return 0.0
    ops_per_s = 1e9 / w
    return ops_per_s * r["request_size"] / (1024 * 1024)


def z_ladder_plots(rows, outdir):
    fam = [r for r in rows if r["family"] == "re1u" and r["ok"]]
    blocks = sorted({combo_key(r) for r in fam})
    if not blocks:
        return
    # instructions per op
    fig, ax = plt.subplots(figsize=(1.8 * len(blocks) + 2, 4.5))
    width = 0.16
    for i, arm in enumerate(ARMS_Z):
        xs, ys = [], []
        for j, key in enumerate(blocks):
            sel = [r for r in fam if combo_key(r) == key and r["arm"] == arm]
            if sel:
                est = sel[0]["instr_u_per_op_estimates"]
                xs.append(j + (i - 2) * width)
                ys.append(med(est))
        ax.bar([x + width / 2 for x in xs], ys, width=width, label=arm)
    ax.set_xticks(range(len(blocks)))
    ax.set_xticklabels([f"{k[0]}\n{k[1]} {k[2]}" for k in blocks], fontsize=7)
    ax.set_ylabel("instructions/op (user, double-difference)")
    ax.set_yscale("log")
    ax.set_title("RE-1U Z-ladder: instructions per op (Host-0)")
    ax.legend(fontsize=7)
    fig.tight_layout()
    savefig_clean(fig, outdir / "z-ladder-instructions-per-op.svg")
    plt.close(fig)

    # throughput (median wall)
    fig, ax = plt.subplots(figsize=(1.8 * len(blocks) + 2, 4.5))
    for i, arm in enumerate(ARMS_Z):
        xs, ys = [], []
        for j, key in enumerate(blocks):
            sel = [r for r in fam if combo_key(r) == key and r["arm"] == arm]
            if sel:
                xs.append(j + (i - 2) * width)
                ys.append(throughput_mibs(sel[0]))
        ax.bar([x + width / 2 for x in xs], ys, width=width, label=arm)
    ax.set_xticks(range(len(blocks)))
    ax.set_xticklabels([f"{k[0]}\n{k[1]} {k[2]}" for k in blocks], fontsize=7)
    ax.set_ylabel("median throughput (MiB/s)")
    ax.set_yscale("log")
    ax.set_title("RE-1U Z-ladder: throughput (Host-0)")
    ax.legend(fontsize=7)
    fig.tight_layout()
    savefig_clean(fig, outdir / "z-ladder-throughput.svg")
    plt.close(fig)


def ratio_plot(analysis, outdir):
    blocks = [b for b in analysis.get("blocks", []) if "case" in b]
    if not blocks:
        return
    names = [("C_sem", "Z1b/Z1"), ("T_backend", "Z2/Z1b"),
             ("C_cont", "Z1bw/Z1b"), ("T_runtime", "Z3/Z1bw")]
    labels = [f"{b['block']['fs']}\n{b['block'].get('cell')} "
              f"{b['block']['op']}" for b in blocks]
    x = range(len(blocks))
    width = 0.2
    fig, ax = plt.subplots(figsize=(1.9 * len(blocks) + 2, 4.5))
    for i, (name, lbl) in enumerate(names):
        ys = [b[name]["ratio"] for b in blocks]
        ax.bar([xx + (i - 1.5) * width for xx in x], ys, width=width,
               label=lbl)
        for xx, y, b in zip(x, ys, blocks):
            v = b[name]["verdict"]
            color = {"MATERIAL_TAX": "red", "PARITY": "green",
                     "GRAY": "orange"}[v]
            ax.text(xx + (i - 1.5) * width, y + 0.03, v[0], fontsize=6,
                    ha="center", color=color)
    ax.axhline(1.05, ls="--", lw=0.7, color="green")
    ax.axhline(1.10, ls="--", lw=0.7, color="red")
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, fontsize=7)
    ax.set_ylabel("median wall ratio (cand/base)")
    ax.set_title("RE-1U frozen decomposition ratios "
                 "(P=parity, M=material, G=gray)")
    ax.legend(fontsize=7)
    fig.tight_layout()
    savefig_clean(fig, outdir / "z-ladder-cost-ratios.svg")
    plt.close(fig)


def re2_plots(rows, outdir):
    fam = [r for r in rows if r["family"] in ("re2u", "re2p") and r["ok"]]
    if not fam:
        return
    cells = sorted({r["cell"] for r in fam},
                   key=lambda c: ({"4Kd1": 0, "4Kd8": 1, "64Kd2": 2,
                                   "2Md1": 3, "2Md2": 4}.get(c, 9), c))
    ops = ["read", "write"]
    # instructions ratio per family/op across cells (btrfs only primary
    # view is not assumed: plot both fs)
    for metric, fname, ylab in (
            ("instr", "re2-envelope-instructions.svg",
             "instr ratio cand/base (log)"),
            ("wall", "re2-envelope-throughput.svg",
             "median wall ratio cand/base")):
        fig, axes = plt.subplots(1, 2, figsize=(11, 4.2), sharey=True)
        for ax, op in zip(axes, ops):
            width = 0.35
            for i, (fam_name, label) in enumerate(
                    (("re2u", "Z2/Z1b (uring)"), ("re2p", "L2/L1 (pool)"))):
                xs, ys = [], []
                for j, cell in enumerate(cells):
                    sel = [r for r in fam if r["family"] == fam_name
                           and r["op"] == op and r["cell"] == cell
                           and r["fs"] == "btrfs"]
                    if len(sel) != 2:
                        continue
                    cand = next(r for r in sel
                                if r["arm"] == ("z2" if fam_name == "re2u"
                                                else "L2"))
                    base = next(r for r in sel
                                if r["arm"] == ("z1b" if fam_name == "re2u"
                                                else "L1"))
                    if metric == "instr":
                        c = med(cand["instr_u_per_op_estimates"])
                        b = med(base["instr_u_per_op_estimates"])
                    else:
                        c = med(cand["wall_ns_per_op_samples"])
                        b = med(base["wall_ns_per_op_samples"])
                    xs.append(j + (i - 0.5) * width)
                    ys.append(c / b)
                ax.bar([x + width / 2 for x in xs], ys, width=width,
                       label=label)
            ax.axhline(1.05, ls="--", lw=0.7, color="green")
            ax.axhline(1.10, ls="--", lw=0.7, color="red")
            ax.set_xticks(range(len(cells)))
            ax.set_xticklabels(cells, fontsize=7)
            ax.set_title(op, fontsize=9)
            ax.legend(fontsize=7)
        axes[0].set_ylabel(ylab)
        fig.suptitle("RE-2 envelope: Sluice vs its own floor "
                     "(btrfs primary)", fontsize=10)
        fig.tight_layout()
        savefig_clean(fig, outdir / fname)
        plt.close(fig)


def mechanism_plot(rows, outdir):
    fam = [r for r in rows if r["family"] in ("re2u", "re2p") and r["ok"]]
    z2 = {(r["cell"], r["op"]): r for r in fam
          if r["family"] == "re2u" and r["arm"] == "z2" and r["fs"] == "btrfs"}
    l2 = {(r["cell"], r["op"]): r for r in fam
          if r["family"] == "re2p" and r["arm"] == "L2" and r["fs"] == "btrfs"}
    keys = sorted(set(z2) & set(l2),
                  key=lambda k: ({"4Kd1": 0, "4Kd8": 1, "64Kd2": 2,
                                  "2Md1": 3, "2Md2": 4}.get(k[0], 9), k[1]))
    if not keys:
        return
    fig, ax = plt.subplots(figsize=(8, 4.5))
    width = 0.35
    for i, (sel, label) in enumerate(((z2, "Sluice Uring (Z2)"),
                                      (l2, "Sluice ThreadPool (L2)"))):
        xs, ys = [], []
        for j, k in enumerate(keys):
            xs.append(j + (i - 0.5) * width)
            ys.append(throughput_mibs(sel[k]))
        ax.bar([x + width / 2 for x in xs], ys, width=width, label=label)
    ax.set_xticks(range(len(keys)))
    ax.set_xticklabels([f"{k[0]} {k[1]}" for k in keys], fontsize=7)
    ax.set_ylabel("median throughput (MiB/s)")
    ax.set_yscale("log")
    ax.set_title("Mechanism comparison, Sluice backends (btrfs) — "
                 "NOT an abstraction-tax measure")
    ax.legend(fontsize=8)
    fig.tight_layout()
    savefig_clean(fig, outdir / "threadpool-vs-uring-regimes.svg")
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--session", action="append", required=True)
    args = ap.parse_args()
    PLOTS.mkdir(parents=True, exist_ok=True)
    all_rows, all_analysis = [], {"blocks": []}
    for s in args.session:
        rows, analysis = load(s)
        all_rows.extend(rows)
        all_analysis["blocks"].extend(analysis.get("blocks", []))
    z_ladder_plots(all_rows, PLOTS)
    ratio_plot(all_analysis, PLOTS)
    re2_plots(all_rows, PLOTS)
    mechanism_plot(all_rows, PLOTS)
    print(f"plots -> {PLOTS}")


if __name__ == "__main__":
    sys.exit(main())
