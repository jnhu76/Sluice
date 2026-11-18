#!/usr/bin/env python3
"""P2 #621 profile analysis: perf stat/record/script aggregation.

Reads only the raw evidence tree; writes /tmp/p2-621/analysis.json and .txt.
Symbols: kernel from /tmp/p2-621/kallsyms.txt, user from verified relinked twins.
No third-party imports.
"""
import bisect
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict

ROOT = "/home/jnhu/Source/Sluice/docs/evidence/621-progress-wait-path-20261118/p2/profile"
KALLSYMS = "/tmp/p2-621/kallsyms.txt"
TWINS = {
    "bench_w02_pipeline": "/tmp/p2-621/symtwin/bench_w02_pipeline.sym",
    "bench_w03_external_loop": "/tmp/p2-621/symtwin/bench_w03_external_loop.sym",
    "sluice-copy": "/tmp/p2-621/symtwin/sluice-copy.sym",
}
LIBC = "/usr/lib64/libc.so.6"
LIBURING = "/usr/lib64/liburing.so.2"

FORMS = {
    "P-A-w02-uring-4k-d16": dict(binary="bench_w02_pipeline", bench=True, ops=540000, chunks=540000),
    "P-B-w02-uring-4k-d1": dict(binary="bench_w02_pipeline", bench=True, ops=540000, chunks=540000),
    "P-C-w03-uring-4k-d16": dict(binary="bench_w03_external_loop", bench=True, ops=540000, chunks=540000),
    "P-D-w02-tp-4k-d16-w4": dict(binary="bench_w02_pipeline", bench=True, ops=540000, chunks=540000),
    "P-E-sluice-copy-4k-d4-w1": dict(binary="sluice-copy", bench=False, ops=131076, chunks=65536),
}

# Ordered stage patterns. First match wins.
STAGES = [
    ("progress/wait", [r"ProgressSource", r"wait_if_unchanged", r"run_progress_pass_",
                       r"poll_progress", r"AsyncIoContext::wait_one", r"AsyncIoContext::poll\(",
                       r"wait_for", r"drain_notification", r"wake_notification",
                       r"observe_pending_control", r"acknowledge_progress", r"epoll_wait",
                       r"do_epoll_wait", r"eventfd", r"__x64_sys_poll", r"do_sys_poll",
                       r"__x64_sys_epoll_wait", r"io_eventfd_signal", r"eventfd_signal",
                       r"eventfd_write", r"io_eventfd_put"]),
    ("observation", [r"RequestScope::take", r"take_result", r"consume_public_result",
                     r"RequestBridge::consume", r"RequestObservation", r"RequestScope::lookup",
                     r"RequestBridge::observe", r"RequestBridge::ready"]),
    ("admission", [r"RequestScope::submit_read", r"AsyncIoContext::submit_read",
                   r"UringAsyncBackend::submit_read", r"ThreadPoolBackend::submit_read",
                   r"RequestCore::reserve", r"RequestCore::accept",
                   r"submit_validate", r"validate_submission"]),
    ("dispatch", [r"dispatch_after_accept", r"dispatch_one_locked", r"dispatch_",
                  r"get_sqe", r"io_uring_get_sqe", r"prepare_sqe", r"submit_transport_locked",
                  r"UringAsyncBackend::dispatch", r"ThreadPoolBackend::dispatch",
                  r"__io_prep_rw", r"io_rw_init_file", r"io_prep_rw"]),
    ("submit", [r"io_uring_enter", r"io_submit_sqes",
                r"__io_submit_flush_completions", r"io_uring_submit"]),
    ("reap/CQE", [r"reap_cqes", r"handle_one_cqe", r"peek_batch_cqe", r"find_live_router",
                  r"io_get_cqe", r"io_cqe_cache_refill", r"io_cqring", r"io_uring_cqe_seen",
                  r"io_cq_unlock_post", r"uring_completion", r"io_req_complete_post",
                  r"io_req_task_complete", r"io_req_task_work_add"]),
    ("terminal/release", [r"finalize_operation_terminal_", r"offer_terminal",
                          r"release_execution", r"RequestCore::offer_terminal",
                          r"RequestCore::release_execution"]),
    ("publication", [r"publish_one", r"begin_publication", r"complete_publication",
                     r"deliver_event", r"claim_execution", r"retire",
                     r"publish_zero_op", r"RequestCore::begin_publication",
                     r"RequestCore::complete_publication", r"RequestCore::claim_execution"]),
]

PHYS = [
    ("payload/FS", [r"rep_movs", r"__memcpy", r"memcpy", r"copy_user", r"copy_page",
                    r"clear_page", r"clear_highpages", r"filemap_", r"shmem_", r"xas_",
                    r"folio", r"page_cache", r"iov_iter", r"vfs_read", r"vfs_write",
                    r"ksys_read", r"ksys_write", r"__io_read", r"io_read",
                    r"simple_read", r"generic_file", r"read_iter", r"write_iter"]),
    ("io-wq punt", [r"io_wq", r"io_worker", r"io_workers", r"tctx_task_work_run",
                    r"task_work_run", r"io_prep_async_work", r"io_queue_async_work",
                    r"io_req_normal_work_add", r"io_wq_enqueue", r"io_wq_put_work",
                    r"task_work_add", r"io_req_task_work_add"]),
    ("bench harness", [r"bench::", r"run_pipeline_phase", r"verify_pattern",
                       r"run_external_phase", r"print_record", r"emit_record",
                       r"write_fixture", r"rotation_order", r"compute_stats", r"now_ns",
                       r"__memcmp_avx2", r"__memcmp", r"memcmp", r"__introsort_loop", r"std::sort"]),
    ("startup/thread create", [r"clone3", r"kernel_clone", r"__do_sys_clone3", r"__x64_sys_clone"]),
    ("backend exec (TP)", [r"ThreadPoolBackend::worker_loop", r"worker_loop",
                           r"__x64_sys_pread64", r"__x64_sys_pwrite64", r"ksys_pread64",
                           r"ksys_pwrite64"]),
    ("kernel syscall/FS overhead", [r"selinux_", r"security_", r"cap_", r"fput", r"fget",
                                     r"fdget", r"get_mem_cgroup", r"__kmalloc", r"kmem_cache",
                                     r"__list_del_entry", r"prandom_", r"__alloc", r"__free", r"__list_add_valid",
                                     r"rmqueue", r"get_page_from_freelist", r"lru_add",
                                     r"__mod_memcg", r"try_charge", r"__next_zones",
                                     r"rw_verify_area", r"avc_lookup", r"__dquot",
                                     r"__perf_event_task_sched"]),
    ("shared sync", [r"pthread_mutex", r"__GI___pthread_mutex", r"lll_lock", r"mutex",
                     r"_raw_spin_lock", r"native_queued_spin_lock", r"__mutex", r"futex",
                     r"osq_lock", r"rwsem", r"spin_lock", r"__smp_call", r"smp_call_function",
                     r"csd_lock"]),
    ("scheduler/IRQ", [r"__schedule", r"finish_task_switch", r"try_to_wake_up", r"ttwu_",
                       r"psi_", r"cpuacct", r"native_write_msr", r"asm_sysvec", r"irqentry",
                       r"error_entry", r"get_signal", r"do_signal", r"handle_signal",
                       r"exit_to_user_mode", r"syscall_exit_to_user_mode", r"swapgs_restore",
                       r"irqentry_exit", r"asm_common_interrupt", r"common_interrupt",
                       r"update_curr", r"update_load_avg", r"__update_load_avg", r"dequeue_entity",
                       r"enqueue_entity", r"set_next_task_fair", r"put_prev_task", r"pick_next_task",
                       r"__switch_to", r"switch_to", r"task_tick", r"update_se", r"dequeue_task",
                       r"enqueue_task", r"newidle_balance", r"load_balance", r"check_preempt",
                       r"resched_curr", r"sched_slice", r"sched_vslice", r"place_entity",
                       r"sched_clock", r"native_sched_clock", r"sysvec_", r"irq_",
                       r"default_send_IPI", r"__flush_smp_call", r"rcu_",
                       r"pick_eevdf", r"pick_task_fair", r"avg_vruntime", r"reweight_",
                       r"update_rq_clock", r"entity_lag", r"vruntime", r"dequeue_hierarchy",
                       r"__calc_smp_shares", r"calc_concur_shares", r"prepare_task_switch",
                       r"ktime_get", r"record_times", r"switch_fpu", r"os_xsave",
                       r"restore_fpregs", r"sched_balance", r"wake_up_q", r"wake_q_add",
                       r"call_function_single_prep_ipi", r"__perf_event_task_sched"]),
]

STAGE_RE = [(n, [re.compile(p) for p in ps]) for n, ps in STAGES]
PHYS_RE = [(n, [re.compile(p) for p in ps]) for n, ps in PHYS]


def load_nm(path, dynamic=False):
    args = ["nm", "-n", "-C"] + (["-D"] if dynamic else []) + [path]
    out = subprocess.run(args, capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) != 3:
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        if parts[1] == "U" or addr == 0:
            continue
        syms.append((addr, parts[2].rstrip()))
    syms.sort()
    return [s[0] for s in syms], syms


def load_kallsyms():
    syms = []
    with open(KALLSYMS, errors="replace") as f:
        for line in f:
            parts = line.split(None, 2)
            if len(parts) != 3:
                continue
            try:
                addr = int(parts[0], 16)
            except ValueError:
                continue
            if addr == 0:
                continue
            syms.append((addr, parts[2].rstrip()))
    syms.sort()
    return [s[0] for s in syms], syms


def make_resolver():
    maps = {name: load_nm(path) for name, path in TWINS.items()}
    kmap = load_kallsyms()
    libc = load_nm(LIBC, dynamic=True)
    liburing = load_nm(LIBURING, dynamic=True)
    other_cache = {}

    def nearest(tbl, ip):
        addrs, syms = tbl
        i = bisect.bisect_right(addrs, ip) - 1
        if i < 0:
            return None
        return syms[i][1]

    def resolve(ip, dso, symfield):
        if dso == "[kernel.kallsyms]":
            return nearest(kmap, ip) or "[kernel]"
        if "bench_w02_pipeline" in dso:
            return nearest(maps["bench_w02_pipeline"], ip) or f"bench_w02+0x{ip:x}"
        if "bench_w03_external_loop" in dso:
            return nearest(maps["bench_w03_external_loop"], ip) or f"bench_w03+0x{ip:x}"
        if "sluice-copy" in dso:
            return nearest(maps["sluice-copy"], ip) or f"sluice-copy+0x{ip:x}"
        if "libc.so" in dso:
            s = symfield.strip()
            if s and s != "[unknown]":
                return s
            return nearest(libc, ip) or f"libc+0x{ip:x}"
        if "liburing" in dso:
            s = symfield.strip()
            if s and s != "[unknown]":
                return s
            return nearest(liburing, ip) or f"liburing+0x{ip:x}"
        s = symfield.strip()
        if s and s != "[unknown]":
            return s
        if dso not in other_cache:
            other_cache[dso] = load_nm(dso, dynamic=True) if os.path.exists(dso) else ([], [])
        return nearest(other_cache[dso], ip) or dso

    return resolve


FRAME_RE = re.compile(r"^\s+([0-9a-fA-F]+)\s+(.*?)\s+\((.*)\)\s*$")


def parse_script(path, resolve, comm_filter):
    """Yield (comm, [(ip, dso, symbol), ...]) blocks."""
    comm = None
    frames = []
    with open(path, errors="replace") as f:
        for line in f:
            if not line.strip():
                if comm is not None and frames:
                    if comm_filter(comm):
                        yield comm, frames
                comm, frames = None, []
                continue
            if line[0] not in "\t ":
                if comm is not None and frames and comm_filter(comm):
                    yield comm, frames
                comm = line.split()[0] if line.split() else ""
                frames = []
                continue
            m = FRAME_RE.match(line.rstrip("\n"))
            if not m:
                continue
            ip = int(m.group(1), 16)
            rest = m.group(2)
            dso = m.group(3)
            frames.append((ip, dso, resolve(ip, dso, rest.strip())))
    if comm is not None and frames and comm_filter(comm):
        yield comm, frames


def leaf_category(sym):
    for name, regs in PHYS_RE:
        for r in regs:
            if r.search(sym):
                return name
    return None


def first_stage(frames):
    for _ip, _dso, sym in frames:
        for name, regs in STAGE_RE:
            for r in regs:
                if r.search(sym):
                    return name
    return None


GENERIC_SYSCALL_RE = re.compile(
    r"entry_SYSCALL|entry_SYSRETQ|syscall_return_via_sysret|do_syscall_64|"
    r"swapgs_restore|exit_to_user_mode|syscall_exit_to_user_mode|"
    r"irqentry_exit|asm_common_interrupt|common_interrupt")


def syscall_owner(frames):
    syms = [s for _i, _d, s in frames]
    joined = " | ".join(syms)
    if re.search(r"io_uring_enter|io_submit_sqes|io_uring_submit", joined):
        return "submit"
    if re.search(r"pread64|pwrite64|vfs_read|vfs_write|ksys_read|ksys_write", joined):
        return "backend exec (TP)"
    if re.search(r"do_poll|__x64_sys_poll|poll_schedule|do_sys_poll", joined):
        return "progress/wait"
    if re.search(r"epoll|do_epoll_wait", joined):
        return "progress/wait"
    if re.search(r"eventfd", joined):
        return "progress/wait"
    if re.search(r"futex", joined):
        return "shared sync"
    if re.search(r"clone3|__x64_sys_clone", joined):
        return "startup/thread create"
    return "syscall (unattributed)"


def attribute(frames):
    leaf = frames[0][2]
    if GENERIC_SYSCALL_RE.search(leaf):
        return syscall_owner(frames)
    cat = leaf_category(leaf)
    if cat in ("payload/FS", "io-wq punt", "bench harness", "backend exec (TP)",
               "startup/thread create"):
        return cat
    st = first_stage(frames)
    if st:
        return st
    return cat or "other"


def parse_stat(path):
    rows = {}
    if not os.path.exists(path):
        return rows
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            p = line.split(",")
            if len(p) < 5:
                continue
            try:
                value = float(p[0])
            except ValueError:
                continue
            event = p[2]
            if "%" in p[3]:
                stddev = p[3]
                runtime = float(p[4]) if p[4] else None
                percent = p[5] if len(p) > 5 else ""
            else:
                stddev = None
                runtime = float(p[3]) if p[3] else None
                percent = p[4] if len(p) > 4 else ""
            rows[event] = dict(value=value, stddev=stddev, runtime_ns=runtime,
                               percent=percent, raw=line)
    return rows


def parse_jsonl(path):
    if not os.path.exists(path):
        return None
    recs = []
    with open(path, errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                recs.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    measured = [r for r in recs if r.get("measured") and r.get("case_id") != "CAL-oracle-self-test"]
    if not measured:
        return dict(records=len(recs), measured=0)
    walls = [r["wall_s"] for r in measured if r.get("wall_s", 0) > 0]
    opss = [r["ops_per_sec"] for r in measured if r.get("ops_per_sec", 0) > 0]
    cpus = [r["cpu_s"] for r in measured]
    fails = [r for r in recs if r.get("measured") and not r.get("ok", True)]
    return dict(records=len(recs), measured=len(measured),
                wall_mean=sum(walls) / len(walls) if walls else None,
                wall_sd=(sum((x - sum(walls) / len(walls)) ** 2 for x in walls) / len(walls)) ** 0.5 if len(walls) > 1 else None,
                ops_per_sec_mean=sum(opss) / len(opss) if opss else None,
                cpu_s_mean=sum(cpus) / len(cpus), failures=len(fails))


def header_duration(path):
    if not os.path.exists(path):
        return None
    with open(path, errors="replace") as f:
        for line in f:
            m = re.search(r"sample duration\s*:\s*([0-9.]+)\s*ms", line)
            if m:
                return float(m.group(1))
    return None


def is_bench_comm(c):
    return c.startswith("bench_w02") or c.startswith("bench_w03") or c.startswith("iou-wrk")


def is_copy_comm(c):
    return c.startswith("sluice-copy")


def analyze_form(form, cfg, resolve, out_txt):
    d = os.path.join(ROOT, form, "raw")
    res = dict(form=form, config=cfg)
    # PMU
    pmu = {}
    for src in ("stat-pmu-r3", "stat-mem-r3"):
        for ev, row in parse_stat(os.path.join(d, src + ".txt")).items():
            row = dict(row)
            row["source"] = src
            if cfg["bench"]:
                row["per_op"] = row["value"] / cfg["ops"]
                if ev == "task-clock":
                    row["per_op_us"] = row["value"] * 1000.0 / cfg["ops"]
            else:
                row["per_chunk"] = row["value"] / cfg["chunks"]
                if ev == "task-clock":
                    row["per_chunk_us"] = row["value"] * 1000.0 / cfg["chunks"]
            pmu[ev] = row
    res["pmu"] = pmu
    # tracepoints
    tps = {}
    for src in ("stat-tp-uring", "stat-tp-sched-syscall", "stat-tp-block-io", "stat-tp-epoll"):
        for ev, row in parse_stat(os.path.join(d, src + ".txt")).items():
            row = dict(row)
            row["source"] = src
            row["per_op"] = row["value"] / (cfg["ops"] if cfg["bench"] else cfg["chunks"])
            tps[ev] = row
    res["tracepoints"] = tps
    # uprobes
    ups = {}
    for src in ("stat-uprobe-1", "stat-uprobe-2", "stat-uprobe-waitpath"):
        p = os.path.join(d, src + ".txt")
        if not os.path.exists(p):
            continue
        stat = parse_stat(p)
        # window ops: full run (7 measured repetitions -> 540000 ops over 3 phases)
        # or short run (1 measured repetition -> 60000 ops over 3 phases).
        window = cfg["ops"] if cfg["bench"] else cfg["chunks"]
        j = parse_jsonl(os.path.join(d, src.replace("stat-", "jsonl-") + ".jsonl"))
        if cfg["bench"] and j and j.get("measured"):
            window = 540000 if j["measured"] >= 7 else 60000
        for ev, row in stat.items():
            row = dict(row)
            row["source"] = src
            row["window_ops"] = window
            row["per_op"] = row["value"] / window if window else None
            ups[ev.split(":")[-1]] = row
    res["uprobes"] = ups
    # overhead baselines
    base_ms = None
    for src in ("stat-pmu-r3", "stat-mem-r3"):
        rows = parse_stat(os.path.join(d, src + ".txt"))
        if rows:
            rts = [r["runtime_ns"] for r in rows.values() if r["runtime_ns"]]
            if rts:
                base_ms = max(rts) / 1e6 / (3 if "-r3" in src else 1)
                break
    overhead = {"baseline_process_ms": base_ms, "schemes": {}}
    if base_ms:
        for src in ("stat-tp-uring", "stat-tp-sched-syscall", "stat-tp-block-io", "stat-tp-epoll",
                    "stat-uprobe-1", "stat-uprobe-2", "stat-uprobe-waitpath"):
            rows = parse_stat(os.path.join(d, src + ".txt"))
            if not rows:
                continue
            rts = [r["runtime_ns"] for r in rows.values() if r["runtime_ns"]]
            if rts:
                ms = max(rts) / 1e6
                overhead["schemes"][src] = dict(ms=ms, ratio=ms / base_ms)
        for rec in ("record-cycles", "record-tp-uring", "record-tp-sched-syscall",
                    "record-tp", "record-offcpu"):
            ms = header_duration(os.path.join(d, "header-" + rec + ".txt"))
            if ms:
                overhead["schemes"][rec] = dict(ms=ms, ratio=ms / base_ms)
        # measured timed-phase wall ratios
        jbase = parse_jsonl(os.path.join(d, "jsonl-pmu-r3.jsonl"))
        if jbase and jbase.get("wall_mean"):
            overhead["baseline_measured_wall_ms"] = jbase["wall_mean"] * 1e3
            for scheme in ("tp-uring", "tp-sched-syscall", "tp-block-io",
                           "record-cycles", "record-tp-uring", "record-tp-sched-syscall",
                           "uprobe-1", "uprobe-2", "uprobe-waitpath"):
                j = parse_jsonl(os.path.join(d, "jsonl-" + scheme + ".jsonl"))
                if j and j.get("wall_mean"):
                    overhead["schemes"].setdefault(scheme, {})["measured_wall_ms"] = j["wall_mean"] * 1e3
                    overhead["schemes"][scheme]["measured_wall_ratio"] = j["wall_mean"] * 1e3 / (jbase["wall_mean"] * 1e3)
    res["overhead"] = overhead
    # cycles record
    cyc_path = os.path.join(d, "script-cycles-sym.txt")
    cyc = dict(samples=0, leaf=Counter(), inclusive=Counter(), stage_self=Counter(),
               stage_inc=Counter(), categories=Counter(), dso=Counter(),
               single_frame=0, no_frames=0, user_kernel_stage=defaultdict(Counter))
    cfilter = is_bench_comm if cfg["bench"] else is_copy_comm
    if os.path.exists(cyc_path):
        for comm, frames in parse_script(cyc_path, resolve, cfilter):
            cyc["samples"] += 1
            if len(frames) == 1:
                cyc["single_frame"] += 1
            leaf_sym = frames[0][2]
            dso = frames[0][1]
            kind = "kernel" if dso == "[kernel.kallsyms]" else ("bench" if "bench_w" in dso or "sluice-copy" in dso else "lib")
            cyc["dso"][kind] += 1
            cyc["leaf"][(leaf_sym, kind)] += 1
            for _ip, _dso, s in frames:
                cyc["inclusive"][s] += 1
            stage = attribute(frames)
            cyc["stage_self"][stage] += 1
            cyc["categories"][stage] += 1
            cyc["user_kernel_stage"][stage][kind] += 1
            seen = set()
            for _ip, _dso, s in frames:
                for name, regs in STAGE_RE:
                    if name in seen:
                        continue
                    if any(r.search(s) for r in regs):
                        cyc["stage_inc"][name] += 1
                        seen.add(name)
                        break
    res["cycles"] = dict(
        samples=cyc["samples"], single_frame=cyc["single_frame"],
        leaf_top=[dict(symbol=s, kind=k, count=c, share=c / cyc["samples"] if cyc["samples"] else 0)
                  for (s, k), c in cyc["leaf"].most_common(30)],
        inclusive_top=[dict(symbol=s, count=c, share=c / cyc["samples"] if cyc["samples"] else 0)
                       for s, c in cyc["inclusive"].most_common(30)],
        stage_self={k: dict(count=v, share=v / cyc["samples"] if cyc["samples"] else 0)
                    for k, v in cyc["stage_self"].most_common()},
        stage_inclusive={k: dict(count=v, share=v / cyc["samples"] if cyc["samples"] else 0)
                         for k, v in cyc["stage_inc"].most_common()},
        user_kernel={k: dict(v) for k, v in cyc["user_kernel_stage"].items()},
    )
    # off-CPU
    off_path = os.path.join(d, "script-offcpu-sym.txt")
    off = dict(samples=0, primitive=Counter(), user_site=Counter(), pair=Counter(),
               stage=Counter(), leaf=Counter(), prim_comm=defaultdict(Counter))
    if os.path.exists(off_path):
        for comm, frames in parse_script(off_path, resolve, cfilter):
            off["samples"] += 1
            leaf = frames[0][2]
            off["leaf"][leaf] += 1
            joined = " | ".join(s for _i, _d, s in frames)
            comm_class = "iou-wrk" if comm.startswith("iou-wrk") else "app"
            if re.search(r"io_cqring_wait", joined):
                prim = "io_uring cqring wait"
            elif re.search(r"futex", joined):
                prim = "futex wait"
            elif re.search(r"do_poll|poll_schedule|__x64_sys_poll|do_sys_poll", joined):
                prim = "poll wait"
            elif re.search(r"epoll|do_epoll_wait", joined):
                prim = "epoll wait"
            elif re.search(r"eventfd", joined):
                prim = "eventfd wait"
            elif re.search(r"io_wq_worker", joined):
                prim = "io-wq worker idle"
            elif re.search(r"kernel_clone|wait_for_completion", joined):
                prim = "thread create wait"
            elif re.search(r"mutex|rwsem|semaphore|pthread_cond", joined):
                prim = "mutex/condvar wait"
            elif re.search(r"schedule_timeout|schedule_hrtimeout", joined):
                prim = "timeout sleep"
            elif re.search(r"exit_to_user_mode_loop", joined):
                prim = "scheduler preempt/return"
            else:
                prim = "other"
            off["primitive"][prim] += 1
            off["prim_comm"][prim][comm_class] += 1
            site = None
            for _ip, dso, s in frames[1:]:
                if "bench_w" in dso or "sluice-copy" in dso:
                    site = s
                    break
            if site is None:
                for _ip, dso, s in frames[1:]:
                    if "pthread_cond" in s or "pthread_mutex" in s or "futex" in s:
                        site = s
                        break
            site = site or "[kernel-only stack]"
            off["user_site"][site] += 1
            off["pair"][(prim, comm_class, site)] += 1
            off["stage"][attribute(frames)] += 1
    res["offcpu"] = dict(
        samples=off["samples"],
        primitive={k: dict(count=v, share=v / off["samples"] if off["samples"] else 0,
                           comm=dict(off["prim_comm"][k]))
                    for k, v in off["primitive"].most_common()},
        user_site=[dict(symbol=s, count=c, share=c / off["samples"] if off["samples"] else 0) for s, c in off["user_site"].most_common(20)],
        pair=[dict(primitive=p, comm=c, site=s, count=n) for (p, c, s), n in off["pair"].most_common(25)],
        stage={k: dict(count=v) for k, v in off["stage"].most_common()},
        leaf_top=[dict(symbol=s, count=c) for s, c in off["leaf"].most_common(20)],
    )
    # JSONL inventories
    res["jsonl"] = {}
    for name in sorted(os.listdir(d)):
        if name.startswith("jsonl-") and name.endswith(".jsonl"):
            j = parse_jsonl(os.path.join(d, name))
            if j:
                res["jsonl"][name] = j
    # text
    out_txt.write(f"\n===== {form} =====\n")
    out_txt.write(f"PMU per op/chunk ({'540000 ops' if cfg['bench'] else '65536 chunks'}):\n")
    for ev, row in pmu.items():
        per = row.get("per_op", row.get("per_chunk"))
        out_txt.write(f"  {ev:22s} {row['value']:>16.1f}  per={per:10.3f}  {row.get('stddev') or ''}  {row['percent']}\n")
    out_txt.write("Tracepoints per op:\n")
    for ev, row in tps.items():
        out_txt.write(f"  {ev:45s} {row['value']:>12.0f}  per={row['per_op']:.5f}\n")
    out_txt.write("Uprobes per op (window):\n")
    for ev, row in ups.items():
        out_txt.write(f"  {ev:20s} {row['value']:>12.0f}  window={row['window_ops']}  per={row['per_op']:.5f}\n")
    out_txt.write("Overhead (process ms, ratio):\n")
    for k, v in overhead["schemes"].items():
        out_txt.write(f"  {k:24s} {v.get('ms', v.get('measured_wall_ms', 0)):10.1f}ms ratio={v.get('ratio', v.get('measured_wall_ratio', 0)):.3f}\n")
    out_txt.write(f"Cycles samples={cyc['samples']} single_frame={cyc['single_frame']} dso={dict(cyc['dso'])}\n")
    out_txt.write("Top leaf:\n")
    for s, k, c in [(x['symbol'], x['kind'], x['count']) for x in res['cycles']['leaf_top'][:20]]:
        out_txt.write(f"  {100.0*c/cyc['samples']:6.2f}% {k:6s} {s}\n")
    out_txt.write("Stage self:\n")
    for k, v in res["cycles"]["stage_self"].items():
        out_txt.write(f"  {100.0*v['share']:6.2f}% {k}\n")
    out_txt.write("Off-CPU samples=%d primitives=%s\n" % (off["samples"], dict(off["primitive"])))
    for x in res["offcpu"]["pair"][:12]:
        out_txt.write(f"  {x['count']:8d} {x['primitive']:14s} {x['site']}\n")
    return res


def main():
    resolve = make_resolver()
    allres = {}
    txt_path = "/tmp/p2-621/analysis.txt"
    with open(txt_path, "w") as out_txt:
        for form, cfg in FORMS.items():
            allres[form] = analyze_form(form, cfg, resolve, out_txt)
    with open("/tmp/p2-621/analysis.json", "w") as f:
        json.dump(allres, f, indent=1, default=str)
    print("wrote /tmp/p2-621/analysis.json and", txt_path)


if __name__ == "__main__":
    main()
