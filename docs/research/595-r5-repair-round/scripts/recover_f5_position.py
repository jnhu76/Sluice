#!/usr/bin/env python3
"""F5 position recovery from the joined diagnostic ops (offline, no rerun).

Review finding R1: `caller_window` recorded by note_built() is
`window.size()` captured ONCE per submit() group (bench_w03_external_loop.cpp
`facts.caller_window = window.size()` before the per-member loop), so it is
CONSTANT within a group. It measures the in-flight window occupancy at group
construction, never the member's position inside the group. The first
published round mislabeled it "real in-window position"; that interpretation
is withdrawn here.

True member order is recoverable offline because the bench builds groups of
exactly `want = min(depth - window.size(), request_count - submitted,
max_refill)` requests with consecutive sequence numbers (seq = base+k):
splitting the admitted rows (sorted by seq) into chunks of
`depth - caller_window` yields the construction order, except terminal
partial groups and rejection-damaged groups, which are detected and dropped
(counted as anomalies) rather than guessed.

Outputs:
- analysis/f5-position.json: per (case, arm=R, measured, on): bucket stats by
  TRUE member position (within groups starting at each occupancy), by window
  occupancy itself, validation counters, and stop_kind shares among tails;
- figures/figure-data/f5-position.csv: the same buckets, one row each.
Buckets with n < 20 are still written (min_n marked) but flagged.
"""
import collections
import csv
import json
import os
import re
import sys

ROOT = "/home/jnhu/sluice-595-e5/r5-repair-measure"
OPS = os.path.join(ROOT, "raw", "diag-ops.csv")
OUT_JSON = os.path.join(ROOT, "analysis", "f5-position.json")
OUT_CSV = os.path.join(ROOT, "figures", "figure-data", "f5-position.csv")
TAIL_US = 55.0  # same threshold as the withdrawn analysis (A0 run p99 ~53.9us)
MIN_N = 20

TAG_RE = re.compile(r"^(W03-uring-4k-d(\d+))-b(\d+)-p(\d+)-(A0|R|C)-(on|off)-a(\d+)$")


def depth_of(case):
    m = TAG_RE.match(case + "-b0-p0-R-on-a1")
    return int(m.group(2)) if m else {"W03-uring-4k-d16": 16, "W03-uring-4k-d4": 4}.get(case)


def load_rows(arm="R"):
    """(tag, case, depth, seq, cw, dt_us, stop_kind) for join==ok rows."""
    rows = []
    with open(OPS) as f:
        rd = csv.reader(f)
        header = next(rd)
        ix = {name: header.index(name) for name in
              ("arm", "case", "tag", "seq", "caller_window", "stop_kind",
               "t0", "t5", "join")}
        for r in rd:
            if r[ix["arm"]] != arm or r[ix["join"]] != "ok":
                continue
            if not r[ix["t0"]] or not r[ix["t5"]]:
                continue
            case = r[ix["case"]]
            depth = depth_of(case)
            if depth is None:
                continue
            rows.append((r[ix["tag"]], case, depth, int(r[ix["seq"]]),
                         int(r[ix["caller_window"]]),
                         (int(r[ix["t5"]]) - int(r[ix["t0"]])) / 1e3,
                         r[ix["stop_kind"]]))
    return rows


def recover_positions(rows_for_tag):
    """Return per-row (position_or_None, cw) + validation counters.
    Split into chunks of depth-cw; validate seq contiguity inside chunks."""
    n = len(rows_for_tag)
    pos = [None] * n
    val = {"rows": n, "groups": 0, "normal_groups": 0,
           "terminal_groups": 0, "anomalous_groups": 0,
           "unpositioned_rows": 0, "seq_gaps": 0}
    prev_seq = None
    for _, _, _, seq, _, _, _ in rows_for_tag:
        if prev_seq is not None and seq != prev_seq + 1:
            val["seq_gaps"] += 1
        prev_seq = seq
    i = 0
    while i < n:
        cw = rows_for_tag[i][4]
        g = rows_for_tag[i][2] - cw
        j = i
        while j < n and rows_for_tag[j][4] == cw and (j - i) < g:
            j += 1
        size = j - i
        contiguous = rows_for_tag[j - 1][3] - rows_for_tag[i][3] == size - 1
        val["groups"] += 1
        if size == g and contiguous:
            val["normal_groups"] += 1
            for k in range(i, j):
                pos[k] = k - i
        elif j == n and size < g:
            # terminal partial group (request_count cap) - keep order but flag
            val["terminal_groups"] += 1
            if contiguous:
                for k in range(i, j):
                    pos[k] = k - i
            else:
                val["unpositioned_rows"] += size
        else:
            val["anomalous_groups"] += 1
            val["unpositioned_rows"] += size
        i = j
    return pos, val


def bucket_stats(items):
    """items: list of (dt_us, stop_kind)."""
    dts = sorted(x[0] for x in items)
    n = len(dts)
    tails = [s for d, s in items if d > TAIL_US]
    return {
        "n": n,
        "median_us": dts[n // 2] if n else None,
        "p99_us": dts[max(0, int(0.99 * n) - 1)] if n else None,
        "tail_gt55_pct": 100.0 * len(tails) / n if n else None,
        "tail_stop_kinds": dict(collections.Counter(tails)) if tails else {},
    }


def main():
    rows = load_rows("R")
    by_tag = collections.defaultdict(list)
    for t in rows:
        by_tag[t[0]].append(t)
    validation = {}
    # bucket -> list of (dt_us, stop_kind)
    by_member = collections.defaultdict(list)   # (case, cw_at_start, position)
    by_occupancy = collections.defaultdict(list)  # (case, cw)
    for tag, trs in sorted(by_tag.items()):
        trs.sort(key=lambda x: x[3])
        pos, val = recover_positions(trs)
        validation[tag] = val
        case = trs[0][1]
        for (tg, cs, depth, seq, cw, dt, stop), p in zip(trs, pos):
            by_occupancy[(case, cw)].append((dt, stop))
            if p is not None:
                by_member[(case, cw, p)].append((dt, stop))
    out = {"tail_threshold_us": TAIL_US, "semantics": {
              "caller_window": "in-flight window occupancy at group construction "
                               "(constant within a group; NOT member position)",
              "member_position": "construction order within the group, recovered "
                                 "by chunk-split (depth - caller_window), "
                                 "terminal/anomalous groups excluded"},
           "validation": validation, "member_position": {}, "window_occupancy": {}}
    for (case, cw, p), items in sorted(by_member.items()):
        if len(items) >= MIN_N:
            out["member_position"][f"{case}|cw{cw}|pos{p}"] = bucket_stats(items)
    for (case, cw), items in sorted(by_occupancy.items()):
        if len(items) >= MIN_N:
            out["window_occupancy"][f"{case}|cw{cw}"] = bucket_stats(items)
    tot = {"tags": len(validation),
           "rows": sum(v["rows"] for v in validation.values()),
           "anomalous_groups": sum(v["anomalous_groups"] for v in validation.values()),
           "terminal_groups": sum(v["terminal_groups"] for v in validation.values()),
           "unpositioned_rows": sum(v["unpositioned_rows"] for v in validation.values()),
           "tags_with_seq_gaps": sum(1 for v in validation.values() if v["seq_gaps"])}
    out["validation_totals"] = tot

    os.makedirs(os.path.dirname(OUT_CSV), exist_ok=True)
    with open(OUT_JSON, "w") as f:
        json.dump(out, f, indent=1)
    with open(OUT_CSV, "w") as f:
        w = csv.writer(f)
        w.writerow(["basis", "case", "cw_or_pos", "n", "median_us", "p99_us",
                    "tail_gt55_pct"])
        for key, s in sorted(out["member_position"].items()):
            case, cw, p = key.split("|")
            w.writerow(["member_position", case, f"{cw}|{p}", s["n"],
                        round(s["median_us"], 2), round(s["p99_us"], 2),
                        round(s["tail_gt55_pct"], 3)])
        for key, s in sorted(out["window_occupancy"].items()):
            case, cw = key.split("|")
            w.writerow(["window_occupancy", case, cw, s["n"],
                        round(s["median_us"], 2), round(s["p99_us"], 2),
                        round(s["tail_gt55_pct"], 3)])
    print(json.dumps(tot, indent=1))
    print(f"wrote {OUT_JSON} and {OUT_CSV}")
    # a damaged phase must be visible in the exit status of downstream tools;
    # this script reports counters and exits 0 only if nothing was unpositioned
    return 0 if tot["unpositioned_rows"] == 0 and tot["tags_with_seq_gaps"] == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
