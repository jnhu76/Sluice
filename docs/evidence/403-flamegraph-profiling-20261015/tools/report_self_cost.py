#!/usr/bin/env python3
"""Builds the flame-graph SVGs and the leaf self-cost summary.

Input:  data/folded/<case>.folded.gz and <case>.tid.folded.gz
Output: svg/<case>.svg, svg/<case>.threads.svg, data/SUMMARY.md

The first SVG aggregates every thread; the second keeps one root frame per
thread so a pipeline's submitter, workers and kernel io-wq threads can be
compared side by side.
"""
import gzip
import subprocess
import tempfile
import sys
from collections import Counter
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
EVIDENCE_DIR = TOOLS_DIR.parent
DATA = EVIDENCE_DIR / "data"
SVG = EVIDENCE_DIR / "svg"
FLAMEGRAPH_DIR = Path(sys.argv[1]) if len(sys.argv) > 1 else TOOLS_DIR / "flamegraph"

TITLES = {
    "w02-tp-4k-d16-w4": "W-02 ThreadPool 4 KiB depth=16 workers=4 (20000 requests)",
    "w02-uring-4k-d16": "W-02 io_uring 4 KiB depth=16 (20000 requests)",
    "w02-tp-4k-d16-w1": "W-02 ThreadPool 4 KiB depth=16 workers=1 (20000 requests)",
    "w02-tp-4k-d16-w8": "W-02 ThreadPool 4 KiB depth=16 workers=8 (20000 requests)",
    "w02-uring-4k-d64": "W-02 io_uring 4 KiB depth=64 (20000 requests)",
    "w02-tp-64k-d16-w4": "W-02 ThreadPool 64 KiB depth=16 workers=4 (2000 requests)",
    "w03-tp-4k-d16-w4": "W-03 external epoll loop, ThreadPool 4 KiB depth=16 workers=4",
    "w03-uring-4k-d16": "W-03 external epoll loop, io_uring 4 KiB depth=16",
    "w01-read-4k-seq": "W-01 direct sequential read_at 4 KiB (100000 ops)",
    "w01-write-4k-seq": "W-01 direct sequential write_at 4 KiB (100000 ops)",
    "w01-sync-data-4k": "W-01 direct 4 KiB write + sync_data barrier, tmpfs (200 ops)",
}


def read_folded(path: Path):
    with gzip.open(path, "rt") as handle:
        for line in handle:
            stack, _, count = line.rstrip("\n").rpartition(" ")
            if count.isdigit():
                yield stack.split(";"), int(count)


def flamegraph(folded: Path, out: Path, title: str, script: Path):
    with tempfile.NamedTemporaryFile("w", suffix=".folded") as plain:
        with gzip.open(folded, "rt") as source:
            plain.write(source.read())
        plain.flush()
        with out.open("w") as sink:
            subprocess.run(
                ["perl", str(script), "--title", title, "--width", "1600", "--height", "18",
                 "--fontsize", "11", "--minwidth", "0.3", "--countname", "cycles",
                 "--nametype", "Function:", "--colors", "hot", plain.name],
                stdout=sink, check=True)


def main():
    script = FLAMEGRAPH_DIR / "flamegraph.pl"
    if not script.is_file():
        raise SystemExit(f"missing {script}; clone the FlameGraph repository there "
                         f"or pass its directory as the first argument")
    SVG.mkdir(exist_ok=True)
    report = ["# Flame-graph self-cost summary", "",
              "Cycle-weighted self cost per case, from the perf captures in `data/perf/`.",
              "Each recorded repetition runs the workload three times (timed throughput",
              "pass, correctness-oracle pass, instrumented latency pass), so the profile",
              "covers all three.", ""]
    for folded in sorted(DATA.glob("folded/*.folded.gz")):
        if ".tid." in folded.name:
            continue
        name = folded.name.replace(".folded.gz", "")
        total = 0
        leaves = Counter()
        roots = Counter()
        for frames, count in read_folded(folded):
            total += count
            leaves[frames[-1]] += count
            roots[frames[0]] += count
        title = TITLES.get(name, name)
        flamegraph(folded, SVG / f"{name}.svg", title, script)
        tid = DATA / "folded" / f"{name}.tid.folded.gz"
        if tid.exists():
            flamegraph(tid, SVG / f"{name}.threads.svg", title + " — per thread", script)
        report += [f"## {name}", "", f"total cycles in profile: {total:,}", "",
                   "threads (root frame share):"]
        report += [f"- {r}: {100.0 * c / total:.1f}%" for r, c in roots.most_common(8)]
        report += ["", "top self cost (leaf frame share):"]
        report += [f"- {leaf}: {100.0 * c / total:.2f}%" for leaf, c in leaves.most_common(20)]
        report.append("")
    (DATA / "SUMMARY.md").write_text("\n".join(report))
    print("wrote", SVG, "and", DATA / "SUMMARY.md")


if __name__ == "__main__":
    main()
