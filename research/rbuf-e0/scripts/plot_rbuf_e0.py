#!/usr/bin/env python3
"""plot_rbuf_e0.py — derived SVG plots for RBUF-E0 sessions (#272).

Reads results/<session>/analysis.json (+ summary.json for U0 context where
needed) and emits (SVG only, one plot per file, matplotlib/Agg like
research/chunk-e0):

  plots/steady-wall-ratio-u1-vs-u2.svg       median(U1 wall)/median(U2 wall)
                                             per cell, 1.03 materiality line
  plots/steady-throughput-by-cell.svg        U0/U1/U2 median MiB/s per cell
                                             (context: includes U0, which is
                                             NOT part of any causal claim)
  plots/instructions-per-byte-u1-vs-u2.svg   U1 vs U2 instructions/byte
  plots/amortized-cost-vs-reuse-horizon.svg  U1 vs U2 amortized per-transfer
                                             end-to-end cost vs H (log-x)
  plots/setup-plus-teardown-fraction-vs-reuse-horizon.svg
                                             U2 (setup_ns + teardown_ns) /
                                             end-to-end span fraction vs H
                                             (region dominated by filesystem/
                                             close teardown, NOT registration)
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

REPO = Path(__file__).resolve().parents[3]
RESULTS = REPO / "research/rbuf-e0/results"
PLOTS = REPO / "research/rbuf-e0/plots"

CELL_LABEL = {(524288, 2): "512K×d2", (1048576, 2): "1M×d2",
              (2097152, 1): "2M×d1", (2097152, 2): "2M×d2 (primary)",
              (2097152, 4): "2M×d4", (4194304, 2): "4M×d2"}
ARM_COLORS = {"U0": "#7f7f7f", "U1": "#1f77b4", "U2": "#d62728"}


def load(session: str) -> tuple[dict, list]:
    a = RESULTS / session / "analysis.json"
    s = RESULTS / session / "summary.json"
    if not a.is_file() or not s.is_file():
        sys.exit(f"session {session}: missing analysis.json/summary.json")
    return json.loads(a.read_text()), json.loads(s.read_text())


def cell_labels(cells: list[dict]) -> list[str]:
    return [CELL_LABEL.get((c["chunk"], c["depth"]),
                           f"{c['chunk']//1024}K×d{c['depth']}")
            for c in cells]


def save(fig, name: str) -> None:
    PLOTS.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(PLOTS / name, format="svg")
    plt.close(fig)
    print(f"wrote plots/{name}")


def bar_labels(ax, xs, vals, fmt="{:.3f}"):
    for x, v in zip(xs, vals):
        if v is not None:
            ax.text(x, v, fmt.format(v), ha="center", va="bottom",
                    fontsize=8)


def plot_wall_ratio(analysis: dict) -> None:
    cells = [c for c in analysis["cells"] if c.get("u1_vs_u2")]
    labels, ratios = [], []
    for c in cells:
        m = c["u1_vs_u2"]
        labels.append(CELL_LABEL.get((c["chunk"], c["depth"])))
        ratios.append(m["ratio_u1_over_u2"])
    fig, ax = plt.subplots(figsize=(8, 4.5))
    xs = range(len(labels))
    colors = ["#2ca02c" if c["u1_vs_u2"]["material"] else "#1f77b4"
              for c in cells]
    ax.bar(xs, ratios, color=colors)
    ax.axhline(1.03, color="#d62728", linestyle="--", linewidth=1,
               label="materiality threshold 1.03")
    ax.axhline(1.0, color="black", linewidth=0.8)
    bar_labels(ax, xs, ratios)
    ax.set_xticks(list(xs))
    ax.set_xticklabels(labels)
    ax.set_ylabel("median(U1 wall) / median(U2 wall)")
    ax.set_title("RBUF-E0 steady-state wall ratio, U1 vs U2 "
                 "(>1 means registration is faster; HOST-LOCAL ONLY)")
    ax.legend()
    save(fig, "steady-wall-ratio-u1-vs-u2.svg")


def plot_throughput(analysis: dict) -> None:
    cells = analysis["cells"]
    labels = cell_labels(cells)
    fig, ax = plt.subplots(figsize=(9, 4.5))
    width = 0.27
    for k, arm in enumerate(("U0", "U1", "U2")):
        vals = [(c[arm.lower()] or {}).get("mibps_median")
                for c in cells]
        xs = [i + (k - 1) * width for i in range(len(cells))]
        ax.bar(xs, [v if v else 0 for v in vals], width=width,
               color=ARM_COLORS[arm], label=arm)
        for x, v in zip(xs, vals):
            if v:
                ax.text(x, v, f"{v:.0f}", ha="center", va="bottom",
                        fontsize=7)
    ax.set_xticks(range(len(cells)))
    ax.set_xticklabels(labels)
    ax.set_ylabel("median throughput (MiB/s)")
    ax.set_title("RBUF-E0 steady-state throughput by cell "
                 "(U0 is contextual only; causal claim = U1 vs U2)")
    ax.legend()
    save(fig, "steady-throughput-by-cell.svg")


def plot_instructions(analysis: dict) -> None:
    cells = [c for c in analysis["cells"] if c.get("u1_vs_u2")]
    labels, vals = [], []
    for c in cells:
        u1 = c["u1"]["instructions_per_byte_median"]
        u2 = c["u2"]["instructions_per_byte_median"]
        labels.append(CELL_LABEL.get((c["chunk"], c["depth"])))
        vals.append((u1, u2))
    fig, ax = plt.subplots(figsize=(8, 4.5))
    xs = range(len(labels))
    v1 = [v[0] for v in vals]
    v2 = [v[1] for v in vals]
    ax.bar([x - 0.2 for x in xs], v1, width=0.4, color=ARM_COLORS["U1"],
           label="U1 ordinary")
    ax.bar([x + 0.2 for x in xs], v2, width=0.4, color=ARM_COLORS["U2"],
           label="U2 registered/fixed")
    ax.set_xticks(list(xs))
    ax.set_xticklabels(labels)
    ax.set_ylabel("instructions:u per useful byte")
    ax.set_title("RBUF-E0 steady-state user-space instructions per byte")
    ax.legend()
    save(fig, "instructions-per-byte-u1-vs-u2.svg")


def plot_amortized(analysis: dict) -> None:
    hz = [h for h in analysis["amortization"]["horizons"]
          if h.get("u1") and h.get("u2")]
    if not hz:
        print("skip plots/amortized-cost-vs-reuse-horizon.svg "
              "(no amortization data in this session)")
        return
    hs = [h["horizon_transfers"] for h in hz]
    u1 = [h["u1"]["amortized_per_transfer_ns_median"] / 1e9 for h in hz]
    u2 = [h["u2"]["amortized_per_transfer_ns_median"] / 1e9 for h in hz]
    fig, ax = plt.subplots(figsize=(8, 4.5))
    ax.plot(hs, u1, "o-", color=ARM_COLORS["U1"],
            label="U1 ordinary (reused buffers)")
    ax.plot(hs, u2, "s-", color=ARM_COLORS["U2"],
            label="U2 registered/fixed")
    cov = analysis["amortization"].get("crossover_horizon")
    if cov:
        ax.axvline(cov, color="#2ca02c", linestyle="--", linewidth=1,
                   label=f"AMORTIZATION CROSSOVER @ H={cov}")
    ax.set_xscale("log", base=2)
    ax.set_xticks(hs)
    ax.set_xticklabels([str(h) for h in hs])
    ax.set_xlabel("reuse horizon H (1 GiB transfers per lifecycle)")
    ax.set_ylabel("amortized end-to-end cost per transfer (s)")
    ax.set_title("RBUF-E0 amortized cost vs reuse horizon (2M×d2, "
                 "setup+teardown included)")
    ax.legend()
    save(fig, "amortized-cost-vs-reuse-horizon.svg")


def plot_setup_fraction(analysis: dict) -> None:
    hz = [h for h in analysis["amortization"]["horizons"] if h.get("u2")]
    if not hz:
        print("skip plots/setup-plus-teardown-fraction-vs-reuse-horizon.svg "
              "(no U2 amortization data in this session)")
        return
    hs = [h["horizon_transfers"] for h in hz]
    frac = [h["u2"]["setup_plus_teardown_fraction"] for h in hz]
    fig, ax = plt.subplots(figsize=(8, 4.5))
    ax.plot(hs, frac, "s-", color=ARM_COLORS["U2"])
    for h, f in zip(hs, frac):
        ax.annotate(f"{f*100:.2f}%", (h, f), textcoords="offset points",
                    xytext=(0, 6), ha="center", fontsize=8)
    ax.set_xscale("log", base=2)
    ax.set_xticks(hs)
    ax.set_xticklabels([str(h) for h in hs])
    ax.set_xlabel("reuse horizon H")
    ax.set_ylabel("median(setup_ns + teardown_ns) / end-to-end span")
    ax.set_title("RBUF-E0 U2 setup+teardown REGION fraction of the measured "
                 "end-to-end span vs reuse horizon (2M×d2; region dominated "
                 "by filesystem/close teardown, not registration)")
    save(fig, "setup-plus-teardown-fraction-vs-reuse-horizon.svg")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("session")
    args = ap.parse_args()
    analysis, _summary = load(args.session)
    if analysis.get("steady_state_verdict") is None:
        sys.exit("analysis.json lacks RBUF-E0 verdict fields")
    has_cells = any(c.get("u1_vs_u2") for c in analysis["cells"])
    if has_cells:
        plot_wall_ratio(analysis)
        plot_throughput(analysis)
        plot_instructions(analysis)
    else:
        print("skip steady plots (no per-cell U1/U2 data in this session)")
    plot_amortized(analysis)
    plot_setup_fraction(analysis)


if __name__ == "__main__":
    main()
