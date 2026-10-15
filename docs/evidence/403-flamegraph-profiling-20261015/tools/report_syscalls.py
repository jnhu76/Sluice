#!/usr/bin/env python3
"""Turns the bpftrace syscall counts into syscalls-per-operation tables.

The benchmark harness runs each recorded repetition three times (timed
throughput pass, correctness-oracle pass, instrumented latency pass), so the
denominator is measured_ops x 3: `per op` is the syscall cost of ONE pipeline
execution.

Input:  data/syscalls/<case>.syscalls.txt (bpftrace output)
        data/syscalls/<case>.jsonl (records of the counting run)
Output: data/SYSCALLS.md
"""
import json
import re
from collections import defaultdict
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
DATA = TOOLS_DIR.parent / "data"

ID_RE = re.compile(r"#define __NR_(\w+)\s+(\d+)")
CASES = [
    ("w02-tp-4k-d16-w4", 10), ("w02-tp-4k-d16-w1", 10), ("w02-uring-4k-d16", 10),
    ("w03-tp-4k-d16-w4", 10), ("w01-read-4k-seq", 10),
]
PASSES_PER_REP = 3


def syscall_names():
    names = {}
    for path in ("/usr/include/asm/unistd_64.h", "/usr/include/asm-generic/unistd.h"):
        try:
            text = Path(path).read_text()
        except OSError:
            continue
        for name, num in ID_RE.findall(text):
            names.setdefault(int(num), name)
    return names


def measured_ops(name: str, reps: int) -> int:
    records = [json.loads(l) for l in (DATA / "syscalls" / f"{name}.jsonl").read_text().splitlines()]
    case = records[-1]["case_id"]
    tail = [r for r in records if r["case_id"] == case][-(reps + 1):]
    return sum(r["ops"] for r in tail if r["measured"])


def main():
    names = syscall_names()
    out = ["# Syscalls per operation (in-kernel counting, bpftrace)", "",
           "Counts cover the whole process run (fixture setup included) and are",
           "normalised to one pipeline execution (measured ops x 3 passes).", ""]
    for name, reps in CASES:
        ops = measured_ops(name, reps) * PASSES_PER_REP
        counts = defaultdict(lambda: defaultdict(int))
        for line in (DATA / "syscalls" / f"{name}.syscalls.txt").read_text().splitlines():
            m = re.match(r"@syscalls\[(\w+), (\d+)\]: (\d+)", line)
            if not m:
                continue
            role, sid, n = m.group(1), int(m.group(2)), int(m.group(3))
            counts[role][names.get(sid, f"syscall_{sid}")] += n
        out.append(f"## {name} — {ops:,} pipeline executions (measured ops x 3 passes)")
        out.append("")
        for role in sorted(counts, key=lambda r: -sum(counts[r].values())):
            total = sum(counts[role].values())
            out.append(f"### {role} — {total:,} syscalls ({total / ops:.2f}/op)")
            out.append("")
            out.append("| syscall | count | per op |")
            out.append("|---|---|---|")
            for sc, n in sorted(counts[role].items(), key=lambda kv: -kv[1])[:12]:
                out.append(f"| {sc} | {n:,} | {n / ops:.4f} |")
            out.append("")
        allsc = defaultdict(int)
        for role in counts:
            for sc, n in counts[role].items():
                allsc[sc] += n
        total = sum(allsc.values())
        out.append(f"**all threads: {total:,} syscalls = {total / ops:.2f}/op**")
        out.append("")
    (DATA / "SYSCALLS.md").write_text("\n".join(out))
    print("wrote", DATA / "SYSCALLS.md")


if __name__ == "__main__":
    main()
