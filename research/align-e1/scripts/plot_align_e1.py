#!/usr/bin/env python3
"""plot_align_e1.py — derived SVG plots for an ALIGN-E1 sweep session (#268).

Reads results/<session>/summary.csv (+ analysis.json for materiality) and
emits, per depth {1,2,4,8} (prereg B10):
  plots/throughput-d<N>.svg          median MiB/s vs chunk (asymmetric MAD bars)
  plots/instr-per-byte-d<N>.svg      instructions/byte vs chunk
  plots/alignment-ratio-d<N>.svg     replica-natural wall / replica-aligned wall
                                     (baseline 1.0; 1.05 materiality line;
                                     MATERIAL cells marked)

With --causal (E1-C1 strict causal control session, AMENDMENT 2), emits ONLY:
  plots/causal-ratio-d<N>.svg        causal-phase16 wall / causal-aligned64
                                     wall per depth {1,2} (baseline 1.0;
                                     1.05 materiality line)

SVG only (generated artifacts rule). Requires matplotlib.
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
RESULTS = REPO / "research/align-e1/results"
PLOTS = REPO / "research/align-e1/plots"

FILE_BYTES = 134_217_728  # 128 MiB (AMENDMENT 1)
DEPTHS = [1, 2, 4, 8]
MODULES = ["engine", "replica-natural", "replica-aligned"]
MODULE_COLORS = {"engine": "#1f77b4", "replica-natural": "#ff7f0e",
                 "replica-aligned": "#2ca02c"}
CHUNK_LABELS = {4096: "4K", 6144: "6K", 8192: "8K", 12288: "12K",
                16384: "16K", 24576: "24K", 32768: "32K", 49152: "48K",
                65536: "64K", 1048576: "1M"}
CAUSAL_CHUNKS = [c for c in CHUNK_LABELS if c != 1048576]
CAUSAL_DEPTHS = [1, 2]
MATERIAL_RATIO = 1.05


def load_rows(session: str) -> list[dict]:
    p = RESULTS / session / "summary.csv"
    if not p.is_file():
        sys.exit(f"no summary.csv for session {session}")
    with p.open() as f:
        return list(csv.DictReader(f))


def row_index(rows: list[dict]):
    idx = {}
    for r in rows:
        idx[(r["module"], int(r["chunk"]), int(r["depth"]))] = r
    return idx


def xpos(chunk: int) -> float:
    return math.log2(chunk)


def yerr_mibps(r: dict) -> tuple[list[float], list[float]]:
    med = float(r["total_ns_median"])
    madv = float(r["total_ns_mad"])
    lo = FILE_BYTES / (med + madv) * 1e9 if (med + madv) else 0
    hi = FILE_BYTES / (med - madv) * 1e9 if (med - madv) > 0 else 0
    cur = float(r["mibps_median"])
    return [max(cur - lo, 0.0)], [max(hi - cur, 0.0)]


def style_axis(ax, chunks=None):
    shown = list(chunks) if chunks else CHUNK_LABELS
    ax.set_xlabel("chunk size (KiB, log2)")
    ax.grid(True, which="both", ls=":", alpha=0.4)
    ax.set_xticks([xpos(c) for c in shown])
    ax.set_xticklabels([CHUNK_LABELS[c] for c in shown], rotation=45)


def plot_throughput(idx, depth: int, out: Path, session: str):
    fig, ax = plt.subplots(figsize=(8, 4.5))
    for m in MODULES:
        xs, ys, yl, yh = [], [], [], []
        for c in CHUNK_LABELS:
            r = idx.get((m, c, depth))
            if not r:
                continue
            xs.append(xpos(c))
            ys.append(float(r["mibps_median"]))
            lo, hi = yerr_mibps(r)
            yl.append(lo[0])
            yh.append(hi[0])
        ax.errorbar(xs, ys, yerr=[yl, yh], marker="o", ls="-", lw=1.2,
                    ms=4, capsize=2, color=MODULE_COLORS[m], label=m)
    style_axis(ax)
    ax.set_ylabel("median throughput (MiB/s)")
    ax.set_title(f"ALIGN-E1 throughput vs chunk, depth {depth} — {session}")
    ax.legend()
    fig.tight_layout()
    fig.savefig(out / f"throughput-d{depth}.svg")
    plt.close(fig)


def plot_instr_per_byte(idx, depth: int, out: Path, session: str):
    fig, ax = plt.subplots(figsize=(8, 4.5))
    for m in MODULES:
        xs, ys = [], []
        for c in CHUNK_LABELS:
            r = idx.get((m, c, depth))
            if not r:
                continue
            xs.append(xpos(c))
            ys.append(float(r["instructions_per_byte"]))
        ax.plot(xs, ys, marker="o", ls="-", lw=1.2, ms=4,
                color=MODULE_COLORS[m], label=m)
    style_axis(ax)
    ax.set_ylabel("instructions / byte")
    ax.set_title(f"ALIGN-E1 CPU cost vs chunk, depth {depth} — {session}")
    ax.legend()
    fig.tight_layout()
    fig.savefig(out / f"instr-per-byte-d{depth}.svg")
    plt.close(fig)


def plot_alignment_ratio(idx, depth: int, out: Path, session: str):
    fig, ax = plt.subplots(figsize=(8, 4.5))
    xs, ys, marks = [], [], []
    for c in CHUNK_LABELS:
        n = idx.get(("replica-natural", c, depth))
        a = idx.get(("replica-aligned", c, depth))
        if not n or not a or not float(a["total_ns_median"]):
            continue
        xs.append(xpos(c))
        ys.append(float(n["total_ns_median"]) /
                  float(a["total_ns_median"]))
    ax.axhline(1.0, color="black", lw=1, label="1.0 (no separation)")
    ax.axhline(MATERIAL_RATIO, color="red", ls="--", lw=0.9,
               label=f"{MATERIAL_RATIO:.2f} (materiality line)")
    ax.plot(xs, ys, marker="o", ls="-", lw=1.2, ms=5,
            color=MODULE_COLORS["replica-natural"],
            label="natural wall / aligned wall")
    style_axis(ax)
    ax.set_ylabel("median wall ratio (natural / aligned)")
    ax.set_title(f"ALIGN-E1 alignment materiality, depth {depth} — {session}")
    ax.legend()
    fig.tight_layout()
    fig.savefig(out / f"alignment-ratio-d{depth}.svg")
    plt.close(fig)


def plot_causal_ratio(idx, depth: int, out: Path, session: str):
    """E1-C1 (AMENDMENT 2): phase16 wall / aligned64 wall, both arms on the
    SAME posix_memalign backing — the ratio isolates exposed pointer phase."""
    fig, ax = plt.subplots(figsize=(8, 4.5))
    xs, ys = [], []
    for c in CAUSAL_CHUNKS:
        p = idx.get(("causal-phase16", c, depth))
        a = idx.get(("causal-aligned64", c, depth))
        if not p or not a or not float(a["total_ns_median"]):
            continue
        xs.append(xpos(c))
        ys.append(float(p["total_ns_median"]) /
                  float(a["total_ns_median"]))
    ax.axhline(1.0, color="black", lw=1, label="1.0 (no separation)")
    ax.axhline(MATERIAL_RATIO, color="red", ls="--", lw=0.9,
               label=f"{MATERIAL_RATIO:.2f} (materiality line)")
    ax.plot(xs, ys, marker="o", ls="-", lw=1.2, ms=5,
            color=MODULE_COLORS["replica-natural"],
            label="phase16 wall / aligned64 wall")
    style_axis(ax, CAUSAL_CHUNKS)
    ax.set_ylabel("median wall ratio (phase16 / aligned64)")
    ax.set_title(f"ALIGN-E1 E1-C1 causal control, depth {depth} — {session}")
    ax.legend()
    fig.tight_layout()
    fig.savefig(out / f"causal-ratio-d{depth}.svg")
    plt.close(fig)


def clean_svg(path: Path) -> None:
    """Strip trailing whitespace per line (matplotlib SVG path dumps it;
    `git diff --check` requires clean lines)."""
    text = path.read_text()
    fixed = "\n".join(line.rstrip() for line in text.splitlines()) + "\n"
    path.write_text(fixed)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("session", nargs="?", default="aligne1-sweep-native-1")
    ap.add_argument("--causal", action="store_true",
                    help="E1-C1 causal control session: emit ONLY the "
                         "causal-ratio-d{1,2} plots")
    args = ap.parse_args()
    rows = load_rows(args.session)
    idx = row_index(rows)
    out = PLOTS
    out.mkdir(parents=True, exist_ok=True)
    if args.causal:
        for d in CAUSAL_DEPTHS:
            plot_causal_ratio(idx, d, out, args.session)
    else:
        for d in DEPTHS:
            plot_throughput(idx, d, out, args.session)
            plot_instr_per_byte(idx, d, out, args.session)
            plot_alignment_ratio(idx, d, out, args.session)
    for p in out.glob("*.svg"):
        clean_svg(p)
    print(f"plots written to {out}")


if __name__ == "__main__":
    main()