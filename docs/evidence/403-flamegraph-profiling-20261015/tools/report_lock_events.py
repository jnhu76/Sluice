#!/usr/bin/env python3
"""Aggregates the strace lock/submission traces into directly observed events.

These are events, not cycles: they say which futex operations the workload
issues and to which addresses, and how many submission queue entries each
io_uring_enter carries. They are what a syscall count cannot say -- one
io_uring_enter per operation is compatible with any SQE batch size -- and they
are what the call-path attribution in ATTRIBUTION.md must be checked against.

Input:  data/lock-events/<case>.strace.txt[.gz]
Output: data/LOCK-EVENTS.md
"""
import gzip
import re
from collections import Counter
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
DATA = TOOLS_DIR.parent / "data"
EVENTS = DATA / "lock-events"

FUTEX_RE = re.compile(r"futex\((0x[0-9a-f]+), (FUTEX_[A-Z_]+)")
ENTER_RE = re.compile(r"io_uring_enter\(\d+, (\d+), (\d+)")


def main():
    out = [
        "# Directly observed synchronization and submission events (strace)",
        "",
        "Events, not cycles. `strace` serialises the traced threads, so timing and",
        "therefore contention differ from an uninstrumented run; the identities, the",
        "operation mix and the submission batch sizes are what these traces establish.",
        "Unperturbed per-operation counts are in `SYSCALLS.md`; cycle shares and their",
        "call-path attribution are in `ATTRIBUTION.md`.",
        "",
    ]
    traces = sorted(EVENTS.glob("*.strace.txt.gz")) or sorted(EVENTS.glob("*.strace.txt"))
    for trace in traces:
        name = trace.name.replace(".strace.txt.gz", "").replace(".strace.txt", "")
        opener = gzip.open if trace.suffix == ".gz" else open
        ops = Counter()
        addrs = Counter()
        batches = Counter()
        enters = 0
        for line in opener(trace, "rt").read().splitlines():
            m = FUTEX_RE.search(line)
            if m:
                addr, op = m.group(1), m.group(2)
                ops[op] += 1
                addrs[(op, addr)] += 1
                continue
            m = ENTER_RE.search(line)
            if m:
                enters += 1
                batches[int(m.group(1))] += 1

        out.append(f"## {name}")
        out.append("")
        total_futex = sum(ops.values())
        if total_futex:
            out.append(f"futex calls observed: {total_futex:,}")
            out.append("")
            out.append("| operation | calls | share |")
            out.append("|---|---|---|")
            for op, n in ops.most_common():
                out.append(f"| {op} | {n:,} | {100.0 * n / total_futex:.1f}% |")
            out.append("")
            out.append(f"distinct (operation, address) pairs: {len(addrs):,}")
            out.append("")
            out.append("busiest addresses (each address is one futex word, i.e. one lock or"
                       " one condition-variable waiter word):")
            out.append("")
            out.append("| operation | address | calls |")
            out.append("|---|---|---|")
            for (op, addr), n in addrs.most_common(10):
                out.append(f"| {op} | `{addr}` | {n:,} |")
            out.append("")
        if enters:
            out.append(f"io_uring_enter calls observed: {enters:,}")
            out.append("")
            out.append("| SQEs submitted per enter (`to_submit`) | calls | share |")
            out.append("|---|---|---|")
            for n, c in sorted(batches.items()):
                out.append(f"| {n} | {c:,} | {100.0 * c / enters:.1f}% |")
            out.append("")
        if not total_futex and not enters:
            out.append("no futex or io_uring_enter calls in this trace")
            out.append("")

    (DATA / "LOCK-EVENTS.md").write_text("\n".join(out))
    print("wrote", DATA / "LOCK-EVENTS.md")


if __name__ == "__main__":
    main()
