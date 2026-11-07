#!/usr/bin/env python3 -I
"""Join the user-space event ring with kernel io_uring tracepoints (#595 causal-r2).

Per-request chain rebuilt from:
  user: accept -> sqe_prep(cookie) -> defer|immediate -> flush(batch) ->
        reap(cookie,res) -> published -> take
  kernel (join key user_data==cookie, ring identity ctx==ring create):
        enter -> submit_req -> [qasync] -> complete -> [task_work_run]
Checks: cookie monotonicity/reuse, slot/generation lifecycle, res agreement,
thread hand-off (submit tid vs complete tid), coverage per link, and event loss.
"""
import struct
import sys
import collections
import json

REC = struct.Struct("<QQQQQIiIIBBH4x")
EVT = {1: "accept", 2: "sqe_prep", 3: "defer", 4: "immediate", 5: "flush", 6: "reap",
       7: "published", 8: "take"}


def load_user(path):
    data = open(path, "rb").read()
    rows = []
    for i in range(len(data) // REC.size):
        (seq, ts, cookie, gen, aux, tid, fd, slot, phase, evt, cpu, flags) = \
            REC.unpack_from(data, i * REC.size)
        rows.append(dict(seq=seq, ts=ts, cookie=cookie, gen=gen, aux=aux, tid=tid,
                         fd=fd, slot=slot, phase=phase, evt=EVT.get(evt, evt), cpu=cpu,
                         flags=flags))
    return rows


def load_kernel(path):
    rows = []
    for line in open(path):
        parts = line.split()
        if len(parts) < 3 or parts[0] != "S595K" or parts[1] == "BEGIN" or parts[1] == "END":
            continue
        ts = int(parts[1])
        ev = parts[2]
        d = dict(ts=ts, ev=ev)
        for kv in parts[3:]:
            if "=" in kv:
                k, _, v = kv.partition("=")
                d[k] = v
        rows.append(d)
    return rows


def analyze(label, user, kernel):
    out = {"label": label}
    # --- ring identity: kernel create events; fd <-> ctx mapping per phase
    creates = [k for k in kernel if k["ev"] == "create"]
    # user ring_fd values seen per phase
    fd_by_phase = collections.defaultdict(set)
    for u in user:
        fd_by_phase[u["phase"]].add(u["fd"])
    out["kernel_creates"] = [
        {k: e.get(k) for k in ("fd", "ctx", "sq_entries", "cq_entries", "ts")} for e in creates]
    out["user_fd_by_phase"] = {p: sorted(v) for p, v in sorted(fd_by_phase.items())}

    # --- per-phase user chains
    stats = collections.Counter()
    issues = []
    per_phase = {}
    for phase in sorted(fd_by_phase):
        ev = [u for u in user if u["phase"] == phase]
        by_evt = collections.defaultdict(list)
        for u in ev:
            by_evt[u["evt"]].append(u)
        acc = by_evt["accept"]
        sqp = by_evt["sqe_prep"]
        dfi = by_evt["defer"] + by_evt["immediate"]
        reaps = by_evt["reap"]
        pubs = by_evt["published"]
        takes = by_evt["take"]
        flushes = by_evt["flush"]
        stats["accept"] += len(acc)
        stats["sqe_prep"] += len(sqp)
        stats["defer"] += len(by_evt["defer"])
        stats["immediate"] += len(by_evt["immediate"])
        stats["flush"] += len(flushes)
        stats["reap"] += len(reaps)
        stats["published"] += len(pubs)
        stats["take"] += len(takes)

        # cookie monotonic per phase, no reuse
        cookies = [r["cookie"] for r in sqp]
        if cookies != sorted(cookies):
            issues.append(f"phase{phase}: sqe_prep cookies not monotonic")
        if len(cookies) != len(set(cookies)):
            issues.append(f"phase{phase}: cookie REUSED in sqe_prep")
        # slot/gen uniqueness per generation: same (slot,gen) exactly one accept
        keys = [(a["slot"], a["gen"]) for a in acc]
        if len(keys) != len(set(keys)):
            issues.append(f"phase{phase}: duplicate (slot,gen) accept")
        # sqe_prep (slot,gen) must match an accept (slot,gen)
        acc_keys = set(keys)
        for s in sqp:
            if (s["slot"], s["gen"]) not in acc_keys:
                issues.append(f"phase{phase}: sqe_prep without accept (slot={s['slot']},gen={s['gen']})")
        # reap cookies must match sqe_prep cookies
        sqp_cookies = set(cookies)
        reap_cookies = [r["cookie"] for r in reaps]
        ctrl_reaps = [r for r in reaps if r["flags"] & 1]
        for c in reap_cookies:
            if c not in sqp_cookies and not (c >> 63):
                issues.append(f"phase{phase}: reap cookie {c} without sqe_prep")
        if len(reap_cookies) != len(set(reap_cookies)):
            issues.append(f"phase{phase}: reap cookie duplicated")
        if ctrl_reaps:
            stats["reap_control"] += len(ctrl_reaps)
        # published/take (slot,gen) must have been accepted
        for p in pubs:
            if (p["slot"], p["gen"]) not in acc_keys:
                issues.append(f"phase{phase}: published without accept slot={p['slot']}")
        pub_keys = [(p["slot"], p["gen"]) for p in pubs]
        if len(pub_keys) != len(set(pub_keys)):
            issues.append(f"phase{phase}: published (slot,gen) duplicated")
        take_keys = [(t["slot"], t["gen"]) for t in takes]
        if len(take_keys) != len(set(take_keys)):
            issues.append(f"phase{phase}: take (slot,gen) duplicated")
        # slot reuse requires generation increment
        slot_gens = collections.defaultdict(list)
        for a in acc:
            slot_gens[a["slot"]].append(a["gen"])
        for slot, gens in slot_gens.items():
            if len(gens) != len(set(gens)):
                issues.append(f"phase{phase}: slot {slot} reused with SAME generation")
        # zero-len ops (no ring events): accept count vs sqe count expected equal here
        per_phase[phase] = dict(accept=len(acc), sqe_prep=len(sqp), reap=len(reaps),
                                published=len(pubs), take=len(takes), flush=len(flushes),
                                defer=len(by_evt["defer"]), immediate=len(by_evt["immediate"]),
                                slots=len(slot_gens))

    out["user_phase_summary"] = per_phase

    # --- kernel per-phase (time windows from creates)
    wins = []
    for i, c in enumerate(creates):
        start = int(c["ts"])
        end = int(creates[i + 1]["ts"]) if i + 1 < len(creates) else None
        wins.append((start, end, c))
    kstats = collections.Counter()
    for k in kernel:
        kstats[k["ev"]] += 1
    out["kernel_event_counts"] = dict(kstats)

    # submit vs complete pairing by user_data per ctx
    ctx_of_create = {c["ctx"]: c for c in creates}
    submits = [k for k in kernel if k["ev"] == "submit_req"]
    completes = [k for k in kernel if k["ev"] == "complete"]
    comp_by_ud = collections.defaultdict(list)
    for c in completes:
        comp_by_ud[(c.get("ctx"), c.get("user_data"))].append(c)
    unmatched_sub = []
    for s in submits:
        lst = comp_by_ud.get((s.get("ctx"), s.get("user_data")))
        if not lst:
            unmatched_sub.append(s.get("user_data"))
    out["kernel_submit_without_complete"] = len(unmatched_sub)
    out["kernel_submit_without_complete_sample"] = unmatched_sub[:8]

    # thread hand-off: complete tid vs submit tid and vs bench pid
    tid_of_complete = collections.Counter(c.get("tid") for c in completes)
    out["complete_tid_histogram"] = dict(tid_of_complete.most_common(8))
    submit_tids = collections.Counter(s.get("tid") for s in submits)
    out["submit_tid_histogram"] = dict(submit_tids.most_common(8))
    bench_tids = set(submit_tids)
    io_worker_completes = sum(v for t, v in tid_of_complete.items() if t not in bench_tids)
    out["completes_by_io_worker"] = io_worker_completes
    out["completes_by_submit_thread"] = sum(v for t, v in tid_of_complete.items() if t in bench_tids)

    # qasync coverage
    qasync = [k for k in kernel if k["ev"] == "qasync"]
    qa_ud = set((q.get("ctx"), q.get("user_data")) for q in qasync)
    out["qasync_unique_user_data"] = len(qa_ud)
    out["submits_total"] = len(submits)

    # defer (kernel) events, if any
    kdefer = [k for k in kernel if k["ev"] == "defer"]
    out["kernel_defer_events"] = len(kdefer)

    # --- cross-layer join: per (phase) cookie == user_data, ctx == create for that window
    # map each kernel submit to a user phase by ts window
    def phase_of_ts(ts):
        for i, (s, e, c) in enumerate(wins):
            if ts >= s and (e is None or ts < e):
                return i
        return None
    joined = 0
    joined_sample = []
    sqp_by_cookie_phase = {}
    for u in user:
        if u["evt"] == "sqe_prep":
            sqp_by_cookie_phase[(u["phase"], u["cookie"])] = u
    mismatched_res = 0
    for s in submits:
        ph = phase_of_ts(int(s["ts"]))
        ud = int(s["user_data"])
        key = (ph, ud)
        if key in sqp_by_cookie_phase:
            joined += 1
            cands = comp_by_ud.get((s.get("ctx"), s.get("user_data")))
            if cands:
                comp = cands[0]
                # find user reap with same cookie near this complete
                for u in user:
                    if u["evt"] == "reap" and u["cookie"] == ud and u["phase"] == ph:
                        if (int(comp["res"]) if str(comp["res"]).lstrip("-").isdigit() else 0) != \
                                int(u["aux"]):
                            mismatched_res += 1
                        break
    out["cross_join_sqe_prep_to_submit_req"] = joined
    out["res_mismatch_reap_vs_complete"] = mismatched_res

    # user_data=0 (probe statx) vs op cookies
    probe_ud = [s for s in submits if int(s["user_data"]) == 0]
    out["kernel_user_data_zero_submits"] = len(probe_ud)

    out["issues"] = issues[:40]
    out["issue_count"] = len(issues)
    out["user_stats"] = dict(stats)
    return out


def main():
    base = "/home/jnhu/sluice-595-e5/causal-r2/evidence/traces/xlayer"
    result = {}
    for label in sys.argv[1:]:
        user = load_user(f"{base}/{label}/ev.bin")
        kernel = load_kernel(f"{base}/{label}/kernel.txt")
        result[label] = analyze(label, user, kernel)
    print(json.dumps(result, indent=1, sort_keys=False))


if __name__ == "__main__":
    main()
