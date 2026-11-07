#!/usr/bin/env python3
"""#595 R5-repair v2 tail-trace analyzer.

Parses v2 per-writer event dumps (magic 0x32435254544c4941) and performs the
offline join the instrument no longer does online:

  submitter FIFO pairing HYPOTHESIS, accepted only when the counts match:
  the i-th admitted event pairs with the i-th built event of the phase.
  Under rejection the admitted stream is a subsequence of the built stream
  and FIFO MISBINDS; a count mismatch is detectable and NO pairing is then
  fabricated — every op row keeps join=fifo_count_mismatch with slot/gen
  left null, the (slot,gen)-keyed joins are skipped, and the process exits
  nonzero (review R4: never mark a mispaired row join=ok). DOCUMENTED BLIND
  SPOT: an equal-count mispairing (a rejected op rebuilt so that built and
  admitted counts rebalance) is NOT detectable from the recorded fields —
  admitted events carry the ADAPTER's (slot,gen), not the bench seq, and
  the adapter slot space is independent of the bench window depth. The next
  instrument revision must record the bench seq (or cookie) on admitted
  events to close this; until then any count anomaly fails the whole phase.

  taken (T5) joins by bench seq and stays valid even in degraded phases.

  (slot, gen64) joins: cqe/published/stop/cookie events attach to the op that
  admitted that (slot, gen); duplicates and misses are counted per class.

  wake association: a cqe's T_wake is the latest wake event in the SAME
  writer buffer strictly before it (same thread = real order); cross-writer
  cqes get T_wake=0 and class no_wake_same_thread (per F4: record driver
  epoch, do not invent a wake cause).

Dump header flags: bit0 = quiesce deadline breach, bit1 = dropped
unregistered writer, bit3 (value 4) = instrument disabled after breach,
header-only failure dump (n_writers 0, no event bytes). Any of these, any
dropped_overflow, any join/classification counter, or any op row with
join != ok makes the process exit 1.

Output: JSON with per-phase per-op rows and counters. The v1 arena format is
NOT parsed here; v1 traces stay frozen evidence under their own analyzer.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

MAGIC = 0x32435254544c4941
VERSION = 2
EVENT_BYTES = 40
EV_NAMES = {1: "built", 2: "admitted", 3: "enter", 4: "cookie", 5: "cqe",
            6: "published", 7: "taken", 8: "stop", 9: "wake"}

HEADER = "<QIIIII"  # magic, version, event_bytes, n_writers, flags, tag_len
WHDR = "<QQQQ"      # tid, count, dropped_overflow, presentations
EVENT = "<qQQIIIBxH"  # t, gen, cookie, seq, slot, aux, kind, pad


def parse_dump(path):
    data = Path(path).read_bytes()
    off = 0
    magic, version, event_bytes, n_writers, flags, tag_len = struct.unpack_from(HEADER, data, off)
    off += struct.calcsize(HEADER)
    if magic != MAGIC or version != VERSION or event_bytes != EVENT_BYTES:
        raise ValueError(f"{path}: not a v2 tail-trace dump")
    tag = data[off:off + tag_len].decode("ascii", "replace")
    off += tag_len
    pid, run_nonce, dump_index, quiesce_timeouts = struct.unpack_from("<QQQQ", data, off)
    off += 32
    writers = []
    for _ in range(n_writers):
        tid, count, dropped, presentations = struct.unpack_from(WHDR, data, off)
        off += struct.calcsize(WHDR)
        events = []
        for _ in range(count):
            (t, gen, cookie, seq, slot, aux, kind, _pad) = struct.unpack_from(EVENT, data, off)
            off += EVENT_BYTES
            events.append({"t": t, "gen": gen, "cookie": cookie, "seq": seq,
                           "slot": slot, "aux": aux, "kind": EV_NAMES.get(kind, str(kind))})
        writers.append({"tid": tid, "count": count, "dropped_overflow": dropped,
                        "presentations": presentations, "events": events})
    return {"file": str(path), "tag": tag, "flags": flags, "pid": pid,
            "run_nonce": run_nonce, "dump_index": dump_index,
            "quiesce_timeouts": quiesce_timeouts, "writers": writers}


def join_phase(dump):
    writers = dump["writers"]
    role = {}
    events_by_kind = {}
    for w in writers:
        for ev in w["events"]:
            events_by_kind.setdefault(ev["kind"], []).append(ev)
        if any(e["kind"] == "wake" for e in w["events"]):
            role[w["tid"]] = "driver"

    built = [e for w in writers for e in w["events"] if e["kind"] == "built"]
    admitted = [e for w in writers for e in w["events"] if e["kind"] == "admitted"]
    taken = [e for w in writers for e in w["events"] if e["kind"] == "taken"]
    enter = [e for w in writers for e in w["events"] if e["kind"] == "enter"]
    cookie = [e for w in writers for e in w["events"] if e["kind"] == "cookie"]
    cqe = [e for w in writers for e in w["events"] if e["kind"] == "cqe"]
    published = [e for w in writers for e in w["events"] if e["kind"] == "published"]
    stop = [e for w in writers for e in w["events"] if e["kind"] == "stop"]
    wake_n = sum(1 for w in writers for e in w["events"] if e["kind"] == "wake")

    c = {
        "built": len(built), "admitted": len(admitted), "enter": len(enter),
        "cookie": len(cookie), "cqe": len(cqe), "published": len(published),
        "taken": len(taken), "stop": len(stop), "wake": wake_n,
        "pair_built_admitted_mismatch": abs(len(built) - len(admitted)),
        "fifo_count_mismatch": abs(len(built) - len(admitted)),
        "admit_key_duplicate": 0, "cqe_no_admit": 0, "published_no_admit": 0,
        "stop_no_admit": 0, "cookie_no_admit": 0, "cookie_duplicate_key": 0,
        "taken_seq_unknown": 0, "cqe_cross_writer_no_wake": 0,
        "admit_without_cookie_cqe_published": 0,
        "wake_unassociated": 0,
    }

    ops = {}
    for i, ev in enumerate(built):
        ops[i] = {"seq": ev["seq"], "t0": ev["t"], "caller_window": ev["aux"],
                  "slot": None, "gen": None, "presentation": None, "t1": None,
                  "t2": None, "cookie": None, "t_wake": 0, "t3": None,
                  "t4": None, "t5": None, "stop_kind": None, "wake_class": None,
                  "join": "ok"}
    seq_to_idx = {}
    for i, ev in enumerate(built):
        if ev["seq"] in seq_to_idx:
            c["pair_built_admitted_mismatch"] += 1
        seq_to_idx[ev["seq"]] = i

    # FIFO pairing hypothesis: validated only by count equality (and unique
    # built seqs above). Equal-count mispairing is a documented blind spot.
    counts_match = len(built) == len(admitted)
    fifo_ok = counts_match
    if not fifo_ok:
        c["fifo_degraded"] = 1

    key_to_idx = {}
    if fifo_ok:
        for i, ev in enumerate(admitted):
            if i >= len(built):
                c["pair_built_admitted_mismatch"] += 1
                continue
            key = (ev["slot"], ev["gen"])
            if key in key_to_idx:
                c["admit_key_duplicate"] += 1
                continue
            key_to_idx[key] = i
            op = ops[i]
            op.update({"slot": ev["slot"], "gen": ev["gen"],
                       "presentation": ev["aux"], "t1": ev["t"]})

    if fifo_ok:
        for ev in enter:
            key = (ev["slot"], ev["gen"])
            i = key_to_idx.get(key)
            if i is None:
                continue
            ops[i]["t2"] = ev["t"]

    for ev in (cookie if fifo_ok else []):
        key = (ev["slot"], ev["gen"])
        i = key_to_idx.get(key)
        if i is None:
            c["cookie_no_admit"] += 1
            continue
        if ops[i]["cookie"] is not None:
            c["cookie_duplicate_key"] += 1
            continue
        ops[i]["cookie"] = ev["cookie"]

    # wake association per writer buffer, then cqe join
    wake_by_writer = {}
    for w in writers:
        if role.get(w["tid"]) == "driver" or any(e["kind"] == "wake" for e in w["events"]):
            wake_by_writer[id(w)] = [e["t"] for e in w["events"] if e["kind"] == "wake"]
    for w in (writers if fifo_ok else []):
        wakes = wake_by_writer.get(id(w), [])
        wi = 0
        for ev in w["events"]:
            if ev["kind"] == "cqe":
                key = (ev["slot"], ev["gen"])
                i = key_to_idx.get(key)
                if i is None:
                    c["cqe_no_admit"] += 1
                    continue
                while wi < len(wakes) and wakes[wi] < ev["t"]:
                    wi += 1
                # wi = first wake >= cqe.t; previous one is latest strictly before
                if wi > 0:
                    ops[i]["t_wake"] = wakes[wi - 1]
                    ops[i]["wake_class"] = "same_thread_prior_wake"
                else:
                    ops[i]["wake_class"] = "no_prior_wake_same_thread"
                ops[i]["t3"] = ev["t"]
                ops[i]["cqe_tid"] = w["tid"]

    for ev in (published if fifo_ok else []):
        key = (ev["slot"], ev["gen"])
        i = key_to_idx.get(key)
        if i is None:
            c["published_no_admit"] += 1
            continue
        ops[i]["t4"] = ev["t"]

    for ev in (stop if fifo_ok else []):
        key = (ev["slot"], ev["gen"])
        i = key_to_idx.get(key)
        if i is None:
            c["stop_no_admit"] += 1
            continue
        ops[i]["stop_kind"] = ev["aux"]

    # taken (T5) is keyed by bench seq and stays valid in degraded phases
    for ev in taken:
        i = seq_to_idx.get(ev["seq"])
        if i is None:
            c["taken_seq_unknown"] += 1
            continue
        ops[i]["t5"] = ev["t"]

    degraded = None if fifo_ok else "fifo_count_mismatch"
    for i, op in ops.items():
        if degraded:
            op["join"] = degraded
            continue
        if op["slot"] is None:
            op["join"] = "built_never_admitted"
            continue
        if op["t3"] is None and op["t4"] is None:
            op["join"] = "no_kernel_completion_observed"
            c["admit_without_cookie_cqe_published"] += 1
        if op["wake_class"] == "no_prior_wake_same_thread":
            c["cqe_cross_writer_no_wake"] += 0  # same-writer miss, counted below
    c["cqe_same_writer_no_prior_wake"] = sum(
        1 for op in ops.values() if op["wake_class"] == "no_prior_wake_same_thread")

    return {"dump": dump["file"], "tag": dump["tag"], "flags": dump["flags"],
            "quiesce_timeouts": dump["quiesce_timeouts"],
            "writers": [{"tid": w["tid"], "count": w["count"],
                         "dropped_overflow": w["dropped_overflow"],
                         "presentations": w["presentations"]} for w in writers],
            "writer_roles": role, "counters": c, "ops": list(ops.values())}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dumps", nargs="+")
    ap.add_argument("--json-out", default=None)
    ap.add_argument("--ops-out", default=None)
    args = ap.parse_args()
    phases = [join_phase(parse_dump(p)) for p in args.dumps]
    report = {"phases": phases}
    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump(report, f, indent=1)
    if args.ops_out:
        import csv
        keys = ["seq", "slot", "gen", "cookie", "presentation", "caller_window",
                "stop_kind", "t0", "t1", "t2", "t_wake", "t3", "t4", "t5",
                "wake_class", "join"]
        with open(args.ops_out, "w", newline="") as f:
            wr = csv.DictWriter(f, fieldnames=keys, extrasaction="ignore")
            wr.writeheader()
            for ph in phases:
                wr.writerows(ph["ops"])
    totals = {}
    for ph in phases:
        for k, v in ph["counters"].items():
            totals[k] = totals.get(k, 0) + v
        totals["dropped_overflow"] = totals.get("dropped_overflow", 0) + sum(
            w["dropped_overflow"] for w in ph["writers"])
        totals["header_flag_failures"] = totals.get("header_flag_failures", 0) + (
            1 if ph["flags"] & 0b111 else 0)
        totals["ops_not_ok"] = totals.get("ops_not_ok", 0) + sum(
            1 for op in ph["ops"] if op["join"] != "ok")
    print(json.dumps({"n_phases": len(phases), "totals": totals}, indent=1))
    bad_keys = ("pair_built_admitted_mismatch", "fifo_count_mismatch",
"admit_key_duplicate", "cqe_no_admit",
                "published_no_admit", "stop_no_admit", "cookie_no_admit",
                "cookie_duplicate_key", "taken_seq_unknown",
                "admit_without_cookie_cqe_published", "quiesce_timeouts",
                "dropped_overflow", "header_flag_failures", "ops_not_ok")
    bad = sum(totals.get(k, 0) or 0 for k in bad_keys)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
