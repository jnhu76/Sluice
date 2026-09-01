#!/usr/bin/env python3
"""plot_chunk_e0.py — derived SVG plots for a CHUNK-E0 H0 sweep session (#270).

Reads results/<session>/summary.csv (+ analysis.json for the frozen
sweet-spot rules) and emits (prereg §10/§12/§14, SVG only):

  plots/throughput-vs-chunk-d<N>.svg          median MiB/s vs chunk per
                                              depth, with asymmetric MAD
                                              bars; marks TESTED_RANGE_PEAK,
                                              95% point, PLATEAU_ENTRY and
                                              KNEE only when the frozen
                                              algorithms actually located
                                              them.
  plots/instructions-per-byte-vs-chunk-d<N>.svg  instructions/byte vs chunk
  plots/throughput-vs-inflight.svg            median MiB/s vs in-flight
                                              bytes (chunk x depth) across
                                              all cells, sized/marked per
                                              depth; Pareto-frontier cells
                                              highlighted.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

REPO = Path(__file__).resolve().parents[3]
RESULTS = REPO / "research/chunk-e0/results"
PLOTS = REPO / "research/chunk-e0/plots"

FILE_BYTES = 1_073_741_824  # 1 GiB
DEPTHS = [1, 2, 4, 8]
DEPTH_COLORS = {1: "#1f77b4", 2: "#ff7f0e", 4: "#2ca02c", 8: "#d62728"}

CHUNK_LABELS = {16384: "16K", 32768: "32K", 65536: "64K", 98304: "96K",
                131072: "128K", 196608: "192K", 262144: "256K",
                393216: "384K", 524288: "512K", 786432: "768K",
                1048576: "1M", 1572864: "1.5M", 2097152: "2M",
                3145728: "3M", 4194304: "4M"}


def load_rows(session: str) -> list[dict]:
    p = RESULTS / session / "summary.csv"
    if not p.is_file():
        sys.exit(f"no summary.csv for session {session}")
    with p.open() as f:
        return list(csv.DictReader(f))


def row_index(rows: list[dict]):
    idx = {}
    for r in rows:
        idx[(int(r["chunk"]), int(r["depth"]))] = r
    return idx


def load_analysis(session: str) -> dict:
    p = RESULTS / session / "analysis.json"
    if not p.is_file():
        sys.exit(f"no analysis.json for session {session}")
    return json.loads(p.read_text())


def xpos(chunk: int) -> float:
    return math.log2(chunk)


def yerr_mibps(r: dict) -> tuple[list[float], list[float]]:
    """Asymmetric ±MAD throughput bounds around the median, all in MiB/s.
    matplotlib takes (below, above) magnitudes: below = slower
    (median+mad), above = faster (median-mad)."""
    med = float(r["total_ns_median"])
    madv = float(r["total_ns_mad"])
    cur = float(r["mibps_median"])
    slow = FILE_BYTES / (med + madv) * 1e9 / (1 << 20) \
        if (med + madv) else cur
    fast = FILE_BYTES / (med - madv) * 1e9 / (1 << 20) \
        if (med - madv) > 0 else cur
    return [max(cur - slow, 0.0)], [max(fast - cur, 0.0)]


def style_axis(ax):
    ax.set_xlabel("chunk size (KiB, log2)")
    ax.grid(True, which="both", ls=":", alpha=0.4)
    ax.set_xticks([xpos(c) for c in CHUNK_LABELS])
    ax.set_xticklabels([CHUNK_LABELS[c] for c in CHUNK_LABELS],
                       rotation=45)


def plot_throughput(idx, depth: int, out: Path, session: str,
                    analysis: dict) -> None:
    ss = analysis["sweet_spots"][str(depth)]
    fig, ax = plt.subplots(figsize=(8, 4.5))
    xs, ys, yl, yh = [], [], [], []
    for c in CHUNK_LABELS:
        r = idx.get((c, depth))
        if not r:
            continue
        xs.append(xpos(c))
        ys.append(float(r["mibps_median"]))
        lo, hi = yerr_mibps(r)
        yl.append(lo[0])
        yh.append(hi[0])
    ax.errorbar(xs, ys, yerr=[yl, yh], marker="o", ls="-", lw=1.2,
                ms=4, capsize=2, color=DEPTH_COLORS[depth])
    # Frozen-algorithm markers — drawn ONLY when the algorithm located them.
    if ss["tested_range_peak_chunk"] is not None:
        pc = ss["tested_range_peak_chunk"]
        ax.scatter([xpos(pc)], [ss["tested_range_peak_mibps"]], marker="*",
                   s=140, color="black", zorder=5, label="tested peak")
    if ss["p95_point_chunk"] is not None:
        pc = ss["p95_point_chunk"]
        ax.axvline(xpos(pc), color="green", ls="--", lw=0.9,
                   label=f"95% point ({CHUNK_LABELS[pc]})")
    if ss["plateau_entry_chunk"] is not None:
        pc = ss["plateau_entry_chunk"]
        ax.axvline(xpos(pc), color="purple", ls="-.", lw=1.1,
                   label=f"plateau entry ({CHUNK_LABELS[pc]})")
    if ss["knee_chunk"] is not None and ss["knee_label"] == "KNEE":
        ax.axvline(xpos(ss["knee_chunk"]), color="red", ls=":", lw=1.2,
                   label=f"knee ({CHUNK_LABELS[ss['knee_chunk']]})")
    style_axis(ax)
    ax.set_ylabel("median throughput (MiB/s)")
    ax.set_title(f"CHUNK-E0 H0 throughput vs chunk, depth {depth} — {session}")
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out / f"throughput-vs-chunk-d{depth}.svg")
    plt.close(fig)


def plot_instr_per_byte(idx, depth: int, out: Path, session: str) -> None:
    fig, ax = plt.subplots(figsize=(8, 4.5))
    xs, ys = [], []
    for c in CHUNK_LABELS:
        r = idx.get((c, depth))
        if not r:
            continue
        xs.append(xpos(c))
        ys.append(float(r["instructions_per_byte"]))
    ax.plot(xs, ys, marker="o", ls="-", lw=1.2, ms=4,
            color=DEPTH_COLORS[depth])
    style_axis(ax)
    ax.set_ylabel("instructions / byte")
    ax.set_title(f"CHUNK-E0 H0 CPU cost vs chunk, depth {depth} — {session}")
    fig.tight_layout()
    fig.savefig(out / f"instructions-per-byte-vs-chunk-d{depth}.svg")
    plt.close(fig)


def plot_throughput_inflight(idx, out: Path, session: str,
                             analysis: dict) -> None:
    """Throughput vs in-flight bytes (chunk x depth), all depths on one
    canvas; Pareto-frontier cells highlighted (prereg §11)."""
    frontier = {(f["chunk"], f["depth"]) for f in
                analysis["pareto"]["frontier"]}
    fig, ax = plt.subplots(figsize=(8, 4.5))
    for depth in DEPTHS:
        xs, ys = [], []
        for c in CHUNK_LABELS:
            r = idx.get((c, depth))
            if not r:
                continue
            xs.append(float(r["in_flight_bytes"]) / (1 << 20))
            ys.append(float(r["mibps_median"]))
        ax.plot(xs, ys, marker="o", ls="-", lw=1.0, ms=3.5,
                color=DEPTH_COLORS[depth], label=f"depth {depth}")
    # Pareto markers on top.
    px = [f["in_flight_bytes"] / (1 << 20) for f in analysis["pareto"]
          ["frontier"]]
    py = [f["mibps_median"] for f in analysis["pareto"]["frontier"]]
    ax.scatter(px, py, marker="D", s=60, facecolors="none",
               edgecolors="black", zorder=5, label="Pareto frontier")
    ax.set_xscale("log", base=2)
    ax.set_xlabel("in-flight bytes (MiB, chunk x depth, log2)")
    ax.set_ylabel("median throughput (MiB/s)")
    ax.set_title(f"CHUNK-E0 H0 throughput vs in-flight bytes — {session}")
    ax.grid(True, which="both", ls=":", alpha=0.4)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out / "throughput-vs-inflight.svg")
    plt.close(fig)


def clean_svg(path: Path) -> None:
    """Strip trailing whitespace per line (matplotlib SVG path dumps it;
    `git diff --check` requires clean lines)."""
    text = path.read_text()
    fixed = "\n".join(line.rstrip() for line in text.splitlines()) + "\n"
    path.write_text(fixed)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("session", nargs="?", default="chunk-e0-h0-sweep-native-1")
    args = ap.parse_args()
    rows = load_rows(args.session)
    idx = row_index(rows)
    analysis = load_analysis(args.session)
    out = PLOTS
    out.mkdir(parents=True, exist_ok=True)
    for d in DEPTHS:
        plot_throughput(idx, d, out, args.session, analysis)
        plot_instr_per_byte(idx, d, out, args.session)
    plot_throughput_inflight(idx, out, args.session, analysis)
    for p in out.glob("*.svg"):
        clean_svg(p)
    print(f"plots written to {out}")


if __name__ == "__main__":
    main()
