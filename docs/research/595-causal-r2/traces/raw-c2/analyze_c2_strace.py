#!/usr/bin/env python3
"""Parse C2 strace block outputs.

Segments each strace file by io_uring_setup (one ring per pass; PMU_ONLY=1 =>
one main pass per rep: warmup=1 + measured=3 => 4 segments; last 3 = measured).
Extracts per-segment syscall mix and io_uring_enter argument/return/duration
distributions, plus eventfd write/read/park counts.
"""
import json
import re
import sys
from pathlib import Path

RAW = Path("/home/jnhu/sluice-595-e5/causal-r2/evidence/traces/raw-c2")

SETUP_RE = re.compile(r"^\d+ (\d+\.\d+) io_uring_setup\((\d+),.*\) = (\d+) <")
ENTER_RE = re.compile(
    r"^\d+ (\d+\.\d+) io_uring_enter\((\d+), (\d+), (\d+), ([^,]*), (NULL|0x[0-9a-f]+), (\d+)\)"
    r" = (-?\d+)(?: <(\d+\.\d+)>)?$"
)
EVENTFD_RE = re.compile(r"^\d+ \d+\.\d+ eventfd2\(.*\) = (\d+) <")
WRITE_RE = re.compile(r"^\d+ (\d+\.\d+) write\((\d+), .*\) = (-?\d+)(?: <(\d+\.\d+)>)?$")
READ_RE = re.compile(r"^\d+ (\d+\.\d+) read\((\d+), .*\) = (-?\d+)(?: <(\d+\.\d+)>)?$")
POLL_RE = re.compile(r"^\d+ (\d+\.\d+) (poll|ppoll)\(.*\) = (-?\d+)(?: <(\d+\.\d+)>)?$")
FUTEX_RE = re.compile(r"^\d+ \d+\.\d+ futex\(")


def parse_file(path):
    segs = []
    cur = None
    with open(path, errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            m = SETUP_RE.match(line)
            if m:
                cur = {
                    "setup_ts": float(m.group(1)),
                    "sq_entries": int(m.group(2)),
                    "ring_fd": int(m.group(3)),
                    "enters": [],
                    "eventfd_fd": None,
                    "writes": [],
                    "reads_eventfd": 0,
                    "polls": [],
                    "futex": 0,
                }
                segs.append(cur)
                continue
            if cur is None:
                continue
            m = ENTER_RE.match(line)
            if m:
                cur["enters"].append(
                    {
                        "ts": float(m.group(1)),
                        "fd": int(m.group(2)),
                        "to_submit": int(m.group(3)),
                        "min_complete": int(m.group(4)),
                        "flags": m.group(5).strip(),
                        "ret": int(m.group(8)),
                        "dur_s": float(m.group(9)) if m.group(9) else None,
                    }
                )
                continue
            m = EVENTFD_RE.match(line)
            if m:
                cur["eventfd_fd"] = int(m.group(1))
                continue
            m = WRITE_RE.match(line)
            if m:
                cur["writes"].append(
                    {
                        "ts": float(m.group(1)),
                        "fd": int(m.group(2)),
                        "ret": int(m.group(3)),
                        "dur_s": float(m.group(4)) if m.group(4) else None,
                    }
                )
                continue
            m = READ_RE.match(line)
            if m and cur["eventfd_fd"] is not None and int(m.group(2)) == cur["eventfd_fd"]:
                cur["reads_eventfd"] += 1
                continue
            m = POLL_RE.match(line)
            if m:
                cur["polls"].append({"ts": float(m.group(1)), "kind": m.group(2),
                                     "ret": int(m.group(3))})
                continue
            if FUTEX_RE.match(line):
                cur["futex"] += 1
    return segs


def q(vals, p):
    if not vals:
        return None
    vals = sorted(vals)
    k = (len(vals) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(vals) - 1)
    return vals[lo] + (vals[hi] - vals[lo]) * (k - lo)


def seg_summary(seg, ops):
    enters = seg["enters"]
    main = [e for e in enters if e["flags"] == "0"]
    probe = [e for e in enters if e["flags"] != "0"]
    durs_ns = [e["dur_s"] * 1e9 for e in main if e["dur_s"] is not None]
    ts = [e["ts"] for e in main]
    gaps_us = [(b - a) * 1e6 for a, b in zip(ts, ts[1:])]
    argtuples = {}
    for e in enters:
        k = (e["to_submit"], e["min_complete"], e["flags"], e["ret"])
        argtuples[k] = argtuples.get(k, 0) + 1
    wwrites = [w for w in seg["writes"] if seg["eventfd_fd"] is not None
               and w["fd"] == seg["eventfd_fd"]]
    wdurs = [w["dur_s"] * 1e9 for w in wwrites if w["dur_s"] is not None]
    return {
        "ring_fd": seg["ring_fd"],
        "sq_entries": seg["sq_entries"],
        "ops": ops,
        "enter_total": len(enters),
        "enter_per_op": len(enters) / ops,
        "submit_only_per_op": len(main) / ops,
        "probe_enters": len(probe),
        "arg_tuples": {str(k): v for k, v in sorted(argtuples.items())},
        "neg_ret": sum(1 for e in enters if e["ret"] < 0),
        "partial_submit": sum(1 for e in enters if 0 < e["ret"] < e["to_submit"]),
        "ret_lt_tosubmit": sum(1 for e in enters if 0 <= e["ret"] < e["to_submit"]),
        "to_submit_hist": {str(e["to_submit"]): sum(1 for x in enters if x["to_submit"] == e["to_submit"]) for e in enters[:1]},
        "dur_ns": {
            "n": len(durs_ns),
            "mean": sum(durs_ns) / len(durs_ns) if durs_ns else None,
            "p50": q(durs_ns, 0.50), "p90": q(durs_ns, 0.90),
            "p95": q(durs_ns, 0.95), "p99": q(durs_ns, 0.99),
            "max": max(durs_ns) if durs_ns else None,
        },
        "gap_us": {
            "n": len(gaps_us),
            "mean": sum(gaps_us) / len(gaps_us) if gaps_us else None,
            "p50": q(gaps_us, 0.50), "p99": q(gaps_us, 0.99),
        },
        "eventfd_writes": len(wwrites),
        "eventfd_writes_per_op": len(wwrites) / ops,
        "eventfd_write_dur_ns": {
            "n": len(wdurs),
            "mean": sum(wdurs) / len(wdurs) if wdurs else None,
            "p50": q(wdurs, 0.50), "p95": q(wdurs, 0.95),
        } if wdurs else None,
        "eventfd_reads_drain": seg["reads_eventfd"],
        "poll_park_count": len(seg["polls"]),
        "futex": seg["futex"],
        "other_writes": len(seg["writes"]) - len(wwrites),
    }


def main():
    out = {}
    for mode in ("on", "off"):
        for arm in ("A", "B"):
            for idx in (1, 2):
                tag = f"c2d2-{mode}-{arm}-{idx}"
                sfile = RAW / f"{tag}.strace.txt"
                jfile = RAW / f"{tag}.jsonl"
                if not sfile.exists():
                    continue
                segs = parse_file(sfile)
                jsonl = [json.loads(x) for x in open(jfile) if x.strip()]
                measured = [r for r in jsonl if r.get("measured")]
                n_measured = len(measured)
                take = segs[-n_measured:] if n_measured else []
                recs = []
                for seg, jr in zip(take, measured):
                    s = seg_summary(seg, int(jr["ops"]))
                    s["rep"] = jr.get("rep")
                    s["bench"] = {
                        "cpu_s": jr.get("cpu_s"), "wall_s": jr.get("wall_s"),
                        "ops": jr.get("ops"), "ops_per_sec": jr.get("ops_per_sec"),
                    }
                    recs.append(s)
                out[tag] = {"n_passes_total": len(segs), "segments": recs,
                            "jsonl_all_reps": len(jsonl)}
    json.dump(out, sys.stdout, indent=1)


if __name__ == "__main__":
    main()
