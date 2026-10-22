#!/usr/bin/env python3
"""MW-HW-1 paired user/system CPU analysis from /usr/bin/time -v logs.

Input: time-v-paired.txt, interleaved `=== rep=N arm=X ===` blocks produced
by the 7-pair interleaved rerun (base vs e1, same config, taskset -c 2).
Pairs are formed by rep number. Reproduces time-v-paired-analysis.txt.
"""

import re
import statistics
import sys

BLOCK_RE = re.compile(r"^=== rep=(\d+) arm=(\S+) ===")
FIELDS = {
    "user": re.compile(r"^\s*User time \(seconds\): ([0-9.]+)"),
    "system": re.compile(r"^\s*System time \(seconds\): ([0-9.]+)"),
    "invol": re.compile(r"^\s*Involuntary context switches: (\d+)"),
}


def main() -> None:
    path = sys.argv[1] if len(sys.argv) > 1 else "time-v-paired.txt"
    runs = {}
    current = None
    with open(path) as fh:
        for line in fh:
            hit = BLOCK_RE.match(line)
            if hit:
                current = {"user": 0.0, "system": 0.0, "invol": 0}
                runs[(int(hit.group(1)), hit.group(2))] = current
                continue
            if current is None:
                continue
            for name, rex in FIELDS.items():
                hit = rex.match(line)
                if hit:
                    current[name] = float(hit.group(1)) if name != "invol" else int(hit.group(1))

    reps = sorted({rep for rep, _ in runs})
    base = {rep: runs[(rep, "base")] for rep in reps if (rep, "base") in runs}
    e1 = {rep: runs[(rep, "e1")] for rep in reps if (rep, "e1") in runs}
    print(f"parsed: {len(base)} base, {len(e1)} e1")

    def report(name: str, below_label: str) -> None:
        vals = [e1[r][name] / base[r][name] for r in base if r in e1]
        below = sum(1 for v in vals if v < 1.0)
        print(
            f"{name:<7}e1/base ratio: med={statistics.median(vals):.3f} "
            f"range=[{min(vals):.3f},{max(vals):.3f}] {below_label}: {below}/{len(vals)}"
        )

    report("user", "reps<1")
    report("system", "reps<1")
    for label, group in (("base", base), ("e1", e1)):
        print(
            f"{label:<4} user med={statistics.median([g['user'] for g in group.values()]):.3f}s "
            f"system med={statistics.median([g['system'] for g in group.values()]):.3f}s"
        )
    print(
        "invol med: base=%d e1=%d"
        % (
            statistics.median([g["invol"] for g in base.values()]),
            statistics.median([g["invol"] for g in e1.values()]),
        )
    )


if __name__ == "__main__":
    main()
