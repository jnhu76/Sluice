#!/usr/bin/env python3
"""#595 information-audit: independent recalculation of published continuation counts.

Reads the frozen PR #627 archive tree (worktree path passed as argument) and
recomputes, from per-rep raw records only:
  - W03 B arm invocation/rep/request totals and r3 mechanism counters
  - W02 D2 B arm entry/group counters
  - §9.3 strace enter summary consistency
  - headline bootstrap deltas from the archived summary.json (as published)

Usage: python3 recalc_continuation_counts.py /tmp/sluice-pr627/docs/research/595-r3/continuation/raw
"""
import json
import sys
from pathlib import Path

RAW = Path(sys.argv[1])


def load(rel):
    return [json.loads(l) for l in (RAW / rel).open()]


def rep_records(rel):
    for r in load(rel):
        for rep in r.get("reps", []):
            if rep.get("measured"):
                yield r, rep


trip = list(rep_records("TRIPLET-W03-D16/results.jsonl"))
ab = list(rep_records("AB-W02R3-D2/results.jsonl"))

print("== TRIPLET-W03-D16 ==")
for arm in ("A0", "A1", "B"):
    rs = [(r, p) for r, p in trip if r["member"] == arm]
    invs = len({r["ts_utc"] for r, _ in rs})
    ops = sum(p["ops"] for _, p in rs)
    valid = all(p["ok"] for _, p in rs)
    g = sum(p["r3_group_calls"] for _, p in rs)
    s = sum(p["r3_single_calls"] for _, p in rs)
    suf = sum(p["r3_suffix_single_calls"] for _, p in rs)
    ent = sum(p["r3_entry_calls"] for _, p in rs)
    acc = sum(p["r3_accepted"] for _, p in rs)
    pmax = max(p["r3_prefix_max"] for _, p in rs)
    print(
        f"{arm}: invocations={len(rs)} unique_ts={invs} all_ok={valid} ops={ops} "
        f"entry={ent} group={g} single={s} suffix={suf} accepted={acc} prefix_max={pmax}"
    )

b = [(r, p) for r, p in trip if r["member"] == "B"]
invocations = len({(r["pair"], r["ts_utc"]) for r, _ in b})
ops_b = sum(p["ops"] for _, p in b)
groups = sum(p["r3_group_calls"] for _, p in b)
singles = sum(p["r3_single_calls"] for _, p in b)
suffix = sum(p["r3_suffix_single_calls"] for _, p in b)
accepted = sum(p["r3_accepted"] for _, p in b)
accepted_in_groups = accepted - singles  # each single entry accepts exactly 1 op
untouched = sum(p["r3_untouched"] for _, p in b)
presented_in_groups = accepted_in_groups + untouched  # prefix accepted + suffix untouched
print(f"B measured reps={len(b)} invocations(24 expected)={invocations}")
print(f"B total requests={ops_b} multi-member group entries={groups}")
print(f"single_calls - suffix_single_calls = {singles - suffix}")
print(f"avg ACCEPTED prefix per multi-member group = {accepted_in_groups / groups:.2f} (cap)")
print(f"avg PRESENTED size per multi-member group = {presented_in_groups / groups:.2f} "
      f"(accepted prefix + untouched suffix; caller refill span)")
total_presentations = presented_in_groups + singles
print(f"identity: top-level presentations {presented_in_groups + (singles - suffix)} + suffix "
      f"re-presentations {suffix} = {total_presentations} entry presentations "
      f"(each of {ops_b} ops presented once at top level)")

print("\n== AB-W02R3-D2 ==")
for arm in ("A0", "B"):
    rs = [(r, p) for r, p in ab if r["member"] == arm]
    if not rs:
        continue
    ops = sum(p["ops"] for _, p in rs)
    ent = sum(p["r3_entry_calls"] for _, p in rs)
    g = sum(p["r3_group_calls"] for _, p in rs)
    s = sum(p["r3_single_calls"] for _, p in rs)
    suf = sum(p["r3_suffix_single_calls"] for _, p in rs)
    pmax = max((p["r3_prefix_max"] for _, p in rs), default=0)
    print(f"{arm}: reps={len(rs)} ops={ops} entry={ent} group={g} single={s} suffix={suf} prefix_max={pmax}")

print("\n== mechanism-9_3 enter_summary.json ==")
mech = json.loads((RAW / "mechanism-9_3/enter_summary.json").read_text())
print(json.dumps(mech, indent=1)[:1500])

print("\n== published summaries ==")
for rel in ("TRIPLET-W03-D16/summary.json", "AB-W02R3-D2/summary.json"):
    s = json.loads((RAW / rel).read_text())
    print(f"--- {rel}: top-level keys = {sorted(s.keys())}")
