#!/usr/bin/env python3
"""Turns the bpftrace syscall counts into syscalls-per-operation tables.

Denominator. The harness runs every recorded repetition three times (timed
throughput pass, correctness-oracle pass, instrumented latency pass), so one
counted run executes (warmup + measured) x 3 x ops operations. That full count
is read back from the run's own JSONL records, so a change in the harness's
warmup or repetition count cannot silently rescale `per op`.

Init cost. Everything the counted process does outside pipeline execution --
process startup, fixture creation, per-repetition prepare, the backend
availability probe -- is measured by tools/measure_init_cost.sh, which runs the
setup path with no pipeline pass. It is reported per case and subtracted in the
all-thread line; the per-role tables divide raw counts by the execution count
and therefore still contain their share of it.

Consistency checks. Data-path syscalls whose per-operation count is known
exactly (one pread64 per read, one write per completion notification) must come
out as the execution count plus a small residual. A wrong denominator fails
these checks loudly instead of skewing every figure: the superseded v1
denominator was 10% short and leaves a residual of 10% of the execution count.

Input:  data/syscalls/<case>.syscalls.txt, <case>.jsonl, <case>.syscalls.err,
        <case>.run.log, and <case>.verify.txt when the hardened capture wrote it
        data/syscalls/init/<case>.init.strace.txt
Output: data/SYSCALLS.md
"""
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
DATA = TOOLS_DIR.parent / "data"
SYSCALLS = DATA / "syscalls"

ID_RE = re.compile(r"#define __NR_(\w+)\s+(\d+)")
CASES = [
    ("w02-tp-4k-d16-w4", "W02-threadpool-4k-d16-w4"),
    ("w02-tp-4k-d16-w1", "W02-threadpool-4k-d16-w1"),
    ("w02-uring-4k-d16", "W02-uring-4k-d16"),
    ("w03-tp-4k-d16-w4", "W03-threadpool-4k-d16-w4"),
    ("w01-read-4k-seq", "W01-read_at-4k-seq"),
]
PASSES_PER_REP = 3
RESIDUAL_TOLERANCE = 0.001  # of the execution count

# Per-execution count that must be exact for the denominator to be right.
EXACT_PER_OP = {
    "w02-tp-4k-d16-w4": {"pread64": 1, "write": 1},
    "w02-tp-4k-d16-w1": {"pread64": 1, "write": 1},
    "w03-tp-4k-d16-w4": {"pread64": 1, "write": 1},
    "w01-read-4k-seq": {"pread64": 1},
    "w02-uring-4k-d16": {},
}


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


def run_records(name):
    return [json.loads(l) for l in (SYSCALLS / f"{name}.jsonl").read_text().splitlines()]


def executions(name):
    """Every recorded repetition of the counted case, warmup included."""
    case = [r for r in run_records(name) if r["case_id"] != "CAL-oracle-self-test"]
    if not case:
        raise SystemExit(f"{name}: no case records in the JSONL")
    tail = [r for r in case if r["case_id"] == case[0]["case_id"]]
    warmup = [r for r in tail if not r["measured"]]
    measured = [r for r in tail if r["measured"]]
    ops = int(sum(r["ops"] for r in tail))
    reps = len(warmup) + len(measured)
    return {
        "case_id": case[0]["case_id"],
        "warmup_reps": len(warmup),
        "measured_reps": len(measured),
        "ops_per_rep": ops // reps if reps else 0,
        "executions": ops * PASSES_PER_REP,
        "failing": [r for r in tail if not r.get("ok", True)],
    }


def init_cost(name):
    """Fixed setup-path syscall count measured with strace, or None."""
    path = SYSCALLS / "init" / f"{name}.init.strace.txt"
    if not path.exists():
        return None
    for line in path.read_text().splitlines():
        if line.startswith("100.00"):
            fields = line.split()
            if len(fields) >= 4 and fields[3].isdigit():
                return int(fields[3])
    return None


def read_counts(name, names):
    counts = defaultdict(lambda: defaultdict(int))
    for line in (SYSCALLS / f"{name}.syscalls.txt").read_text().splitlines():
        m = re.match(r"@syscalls\[(\w+), (\d+)\]: (\d+)", line)
        if not m:
            continue
        role, sid, n = m.group(1), int(m.group(2)), int(m.group(3))
        counts[role][names.get(sid, f"syscall_{sid}")] += n
    return counts


def verification(name, case_id):
    """(fields, source). A live record from the hardened capture wins; otherwise
    the fields the committed artifacts can still prove are reconstructed, with
    the bench exit code marked as not recorded by the original run."""
    path = SYSCALLS / f"{name}.verify.txt"
    if path.exists():
        fields = dict(
            (k.strip(), v.strip())
            for k, _, v in (line.partition("=") for line in path.read_text().splitlines())
            if k.strip()
        )
        return fields, "live"

    err = (SYSCALLS / f"{name}.syscalls.err").read_text()
    log = (SYSCALLS / f"{name}.syscalls.run.log").read_text()
    run = executions(name)
    events = sum(
        int(m.group(3))
        for m in (
            re.match(r"@syscalls\[(\w+), (\d+)\]: (\d+)", line)
            for line in (SYSCALLS / f"{name}.syscalls.txt").read_text().splitlines()
        )
        if m
    )
    attached = len(re.findall(r"^Attached [1-9]", err, re.M))
    verdict = "OK"
    if run["case_id"] != case_id:
        verdict = "CASE_MISMATCH"
    if attached == 0:
        verdict = "PROBE_NOT_ATTACHED"
    if events == 0:
        verdict = "NO_EVENTS"
    if run["failing"]:
        verdict = "FAILING_REPETITIONS"
    return {
        "case_requested": case_id,
        "case_ran": run["case_id"],
        "warmup_reps": str(run["warmup_reps"]),
        "measured_reps": str(run["measured_reps"]),
        "bench_rc": "NOT_RECORDED_IN_ORIGINAL_RUN",
        "probes_attached": str(attached),
        "events_counted": f"{events:,}",
        "completion_marker": (
            "present" if "failing repetitions=0" in log else "absent"
        ),
        "verdict": verdict,
    }, "reconstructed"


def main():
    names = syscall_names()
    out = [
        "# Syscalls per operation (in-kernel counting, bpftrace)",
        "",
        "Method:",
        "",
        "* the harness runs every recorded repetition three times (timed throughput",
        "  pass, correctness-oracle pass, instrumented latency pass), so a counted run",
        "  executes `(warmup + measured) reps x 3 passes x ops` operations;",
        "* the denominator is that full execution count, read from the run's own JSONL",
        "  records rather than assumed from the requested repetition count;",
        "* `init cost` is the fixed startup, fixture and prepare cost measured by",
        "  `tools/measure_init_cost.sh`, which runs the setup path with no pipeline",
        "  pass; it is subtracted in the all-thread line, while the per-role tables",
        "  divide raw counts by the execution count and still contain their share;",
        "* counts come from the bpftrace probe in `tools/count_syscalls.sh`, which",
        "  filters on the benchmark comms, so helper processes are excluded;",
        "* `capture verification` is the record written by `count_syscalls.sh`; where",
        "  the capture predates that script the record is reconstructed from the",
        "  committed artifacts and the bench exit code is marked as not recorded.",
        "",
        "Version note: this is the corrected table. `SYSCALLS.superseded-v1.md` keeps",
        "the first version, which divided by `measured ops x 3` only -- excluding the",
        "warmup repetition -- and so reported every per-operation figure about 10% high.",
        "",
    ]
    failures = []
    for name, case_id in CASES:
        run = executions(name)
        ops = run["executions"]
        init = init_cost(name)
        counts = read_counts(name, names)
        verify, verify_source = verification(name, case_id)

        out.append(f"## {name} — {ops:,} pipeline executions")
        out.append("")
        out.append(
            f"{run['warmup_reps']} warmup + {run['measured_reps']} measured recorded reps"
            f" x {PASSES_PER_REP} passes x {run['ops_per_rep']:,} ops = {ops:,} executions"
        )
        out.append("")
        out.append(
            f"capture verification ({verify_source}): `{verify.get('verdict', 'MISSING')}`"
            f" — case ran `{verify.get('case_ran', '?')}`,"
            f" {verify.get('probes_attached', '?')} probe(s) attached,"
            f" {verify.get('events_counted', '?')} events,"
            f" bench rc {verify.get('bench_rc', '?')}"
        )
        out.append("")
        if verify.get("verdict") != "OK":
            failures.append(f"{name}: capture verdict {verify.get('verdict')}")
        if run["failing"]:
            failures.append(f"{name}: {len(run['failing'])} failing repetitions in the counted run")
        if init is None:
            failures.append(f"{name}: no init-cost measurement (run tools/measure_init_cost.sh)")
            init = 0
        out.append(f"init cost (measured, setup path only): {init:,} syscalls")
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
        grand_total = sum(allsc.values())
        grand_net = grand_total - init
        out.append(
            f"**all threads: {grand_total:,} counted = {grand_total / ops:.3f}/op;"
            f" {grand_net:,} after the measured setup path = {grand_net / ops:.3f}/op**"
        )
        out.append("")

        checks = []
        for sc, expected in EXACT_PER_OP[name].items():
            counted = allsc.get(sc, 0)
            residual = counted - expected * ops
            ok = 0 <= residual <= RESIDUAL_TOLERANCE * ops
            checks.append(
                f"`{sc}`: {counted:,} counted = {expected} x {ops:,} + {residual:,}"
                f" residual ({'OK' if ok else 'FAILED'})"
            )
            if not ok:
                failures.append(
                    f"{name}: {sc} counted {counted:,}, residual {residual:,}"
                    f" exceeds {RESIDUAL_TOLERANCE:.1%} of {ops:,}"
                )
        if checks:
            out.append(
                "consistency checks (residual = setup path and per-repetition prepare;"
                f" a wrong denominator shows up as a residual of ~10% of {ops:,}):"
            )
            out.append("")
            for c in checks:
                out.append(f"* {c}")
            out.append("")

    (DATA / "SYSCALLS.md").write_text("\n".join(out))
    print("wrote", DATA / "SYSCALLS.md")
    for f in failures:
        print("CHECK_FAILED:", f, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
