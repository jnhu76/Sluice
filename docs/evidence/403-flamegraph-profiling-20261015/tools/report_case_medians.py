#!/usr/bin/env python3
"""Per-case medians of the sampled runs, read from the committed records.

Statistic, applied per case over the run's measured repetitions: median of
`ops_per_sec`, of `cpu_s / ops`, and of the instrumented pass's `lat_p50_ns`
and `lat_p99_ns`. `syscalls/op` is copied from the corrected SYSCALLS.md value
so the two files cannot drift apart.

These come from sampled runs (`perf record` at 4999 Hz, or bpftrace counting),
so they are attribution evidence, not a throughput baseline.

Input:  data/perf/<case>.jsonl, data/SYSCALLS.md
Output: data/CASE-MEDIANS.md
"""
import json
import re
import statistics as st
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
DATA = TOOLS_DIR.parent / "data"

ORDER = [
    "w01-read-4k-seq", "w02-uring-4k-d16", "w02-tp-4k-d16-w4", "w02-tp-4k-d16-w1",
    "w02-tp-4k-d16-w8", "w02-tp-64k-d16-w4", "w02-uring-4k-d64",
    "w03-tp-4k-d16-w4", "w03-uring-4k-d16", "w01-write-4k-seq", "w01-sync-data-4k",
]


def syscalls_per_op():
    """The all-thread figure from the corrected SYSCALLS.md, per case."""
    values = {}
    text = (DATA / "SYSCALLS.md").read_text()
    for block in text.split("\n## ")[1:]:
        name = block.split(" ")[0]
        m = re.search(r"\*\*all threads: .*? = ([\d.]+)/op\*\*", block)
        if m:
            values[name] = float(m.group(1))
    return values


def main():
    per_op = syscalls_per_op()
    out = [
        "# Per-case medians of the sampled runs",
        "",
        "Median over the measured repetitions of each case's committed run:",
        "`ops_per_sec`, `cpu_s / ops`, and the instrumented pass's `lat_p50_ns` /",
        "`lat_p99_ns`. `syscalls/op` is the all-thread figure from the corrected",
        "`SYSCALLS.md`. Sampled runs are attribution evidence, not a throughput",
        "baseline: `perf record` at 4999 Hz and bpftrace both perturb the workload.",
        "",
        "| case | measured reps | throughput | CPU/op | p50 | p99 | syscalls/op |",
        "|---|---|---|---|---|---|---|",
    ]
    for name in ORDER:
        path = DATA / "perf" / f"{name}.jsonl"
        recs = [json.loads(l) for l in path.read_text().splitlines()]
        case = [r for r in recs if r["case_id"] != "CAL-oracle-self-test"]
        measured = [r for r in case if r["measured"]]
        ops = st.median([r["ops"] for r in measured])
        out.append(
            "| {} | {} | {:.0f}k ops/s | {:.2f} µs | {:.2f} µs | {:.2f} µs | {} |".format(
                name, len(measured),
                st.median([r["ops_per_sec"] for r in measured]) / 1000.0,
                st.median([r["cpu_s"] for r in measured]) / ops * 1e6,
                st.median([r["lat_p50_ns"] for r in measured]) / 1000.0,
                st.median([r["lat_p99_ns"] for r in measured]) / 1000.0,
                f"{per_op[name]:.2f}" if name in per_op else "—",
            )
        )
    out.append("")
    (DATA / "CASE-MEDIANS.md").write_text("\n".join(out))
    print("wrote", DATA / "CASE-MEDIANS.md")


if __name__ == "__main__":
    main()
