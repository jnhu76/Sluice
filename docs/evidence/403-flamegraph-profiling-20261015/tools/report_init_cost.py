#!/usr/bin/env python3
"""Summarises the fixed per-process syscall cost of each counted case.

The harness creates its fixture before any repetition runs, so the runs behind
`data/syscalls/init/` execute the startup and fixture path with no pipeline
pass. What they count does not scale with the operation count, and
report_syscalls.py subtracts it so `per op` describes pipeline execution.

The counts include the `mkdir -p` helper processes the harness spawns through
system(3); the bpftrace census filters by comm and excludes those, so this
figure is a slight over-estimate of the benchmark's own setup cost.

Input:  data/syscalls/init/<case>.init.strace.txt and .init.verify.txt
Output: data/INIT-COST.md
"""
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
DATA = TOOLS_DIR.parent / "data"
INIT = DATA / "syscalls" / "init"


def parse(path):
    """(total, [(syscall, calls)]) from a strace -c summary."""
    rows = []
    total = 0
    for line in path.read_text().splitlines():
        fields = line.split()
        if line.startswith("100.00") and len(fields) >= 5:
            total = int(fields[3])
        elif len(fields) == 5 and fields[3].isdigit():
            rows.append((fields[4], int(fields[3])))
    return total, sorted(rows, key=lambda kv: -kv[1])


def main():
    out = [
        "# Fixed per-process cost (setup path only, strace)",
        "",
        "Each run executes the harness's startup and fixture creation with no",
        "pipeline pass (`G0_WARMUP=0 G0_MEASURED=0`), so these syscalls do not scale",
        "with the operation count. `SYSCALLS.md` subtracts the total in its",
        "all-thread line. The counts include the harness's `mkdir -p` helper",
        "processes, which the bpftrace census excludes, so this is a slight",
        "over-estimate of the benchmark's own setup cost.",
        "",
    ]
    for agg in sorted(INIT.glob("*.init.strace.txt")):
        name = agg.name[:-len(".init.strace.txt")]
        total, rows = parse(agg)
        verify = {}
        verify_path = INIT / f"{name}.init.verify.txt"
        if verify_path.exists():
            verify = dict(
                (k.strip(), v.strip())
                for k, _, v in (l.partition("=") for l in verify_path.read_text().splitlines())
                if k.strip()
            )
        out.append(f"## {name} — {total:,} syscalls")
        out.append("")
        out.append(
            f"verification: `{verify.get('verdict', 'MISSING')}`"
            f" (bench rc {verify.get('bench_rc', '?')},"
            f" repetition lines {verify.get('repetition_lines', '?')})"
        )
        out.append("")
        out.append("| syscall | calls |")
        out.append("|---|---|")
        for syscall, calls in rows[:10]:
            out.append(f"| {syscall} | {calls:,} |")
        out.append("")
    (DATA / "INIT-COST.md").write_text("\n".join(out))
    print("wrote", DATA / "INIT-COST.md")


if __name__ == "__main__":
    main()
