#!/usr/bin/env python3
"""W3 row-level attribution for the frozen #621 p4 DWARF profile session.

Reads only the archived perf.data-derived text under p4/profile/*/raw/.
Resolves sampled user IPs with addr2line against the p4 DWARF binaries,
parses perf script --inline frames, and aggregates:

  self             leaf enclosing function == candidate (candidate's own code)
  callsite_extra   candidate appears in the leaf or immediate-caller inline
                   chain, leaf is another symbol (mainly libc mutex/syscall)
  owner_total      self + callsite_extra
  inclusive        candidate appears anywhere in the call chain

The stage container uses the p2 R1 classifier (P2-COST-MAP-R1.md), including
the libc __poll boundary row, so the candidate rows can be compared with the
stage shares without conflating the two.

No product code, test, model or build input is read or written here.
"""
import argparse
import collections
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROFILE = os.path.dirname(HERE)
REPO = "/home/jnhu/Source/Sluice"

CASES = {
    "P-D": {
        "dir": "P-D-w02-tp-4k-d16-w4",
        "binary": "/tmp/621-p4-build/ring/linux/x86_64/release/bench_w02_pipeline",
        "comm_prefix": "bench_w02",
        "kind": "bench",
        "case_id": "W02-threadpool-4k-d16-w4",
        "runs": ["run-01", "run-02"],
        "ops_per_run": 540000,
    },
    "P-C": {
        "dir": "P-C-w03-uring-4k-d16",
        "binary": "/tmp/621-p4-build/ring/linux/x86_64/release/bench_w03_external_loop",
        "comm_prefix": "bench_w03",
        "kind": "bench",
        "case_id": "W03-uring-4k-d16",
        "runs": ["run-01", "run-02"],
        "ops_per_run": 540000,
    },
    "P-E": {
        "dir": "P-E-sluice-copy-4k-d4-w1",
        "binary": "/tmp/621-p4-build/copy/linux/x86_64/release/sluice-copy",
        "comm_prefix": "sluice-copy",
        "kind": "copy",
        "case_id": "sluice-copy-4k-d4-w1",
        "runs": ["run-01", "run-02"],
        "chunks_per_run": 5 * 65536,
        "requests_per_run": 5 * 131076,
    },
    "P-F": {
        "dir": "P-F-w02-uring-4k-d64",
        "binary": "/tmp/621-p4-build/ring/linux/x86_64/release/bench_w02_pipeline",
        "comm_prefix": "bench_w02",
        "kind": "bench",
        "case_id": "W02-uring-4k-d64",
        "runs": ["run-01", "run-02"],
        "ops_per_run": 540000,
        "control": {
            "label": "d16-control",
            "case_id": "W02-uring-4k-d16",
            "runs": ["control-d16-run-01"],
            "ops_per_run": 540000,
        },
    },
}

# key -> (display name, pattern, family)
CANDIDATES = [
    ("reserve", "RequestCore::reserve", "RequestCore::reserve(", "admission"),
    ("accept", "RequestCore::accept", "RequestCore::accept(", "admission"),
    ("offer_terminal", "RequestCore::offer_terminal", "RequestCore::offer_terminal(", "terminal"),
    ("claim_execution", "RequestCore::claim_execution", "RequestCore::claim_execution(", "execution"),
    ("acquire_execution", "RequestCore::acquire_execution", "RequestCore::acquire_execution(", "execution"),
    ("release_execution", "RequestCore::release_execution", "RequestCore::release_execution(", "terminal"),
    ("release_control", "RequestCore::release_control", "RequestCore::release_control(", "terminal"),
    ("begin_publication", "RequestCore::begin_publication", "RequestCore::begin_publication(", "publication"),
    ("complete_publication", "RequestCore::complete_publication", "RequestCore::complete_publication(", "publication"),
    ("observe_public_result", "RequestCore::observe_public_result", "RequestCore::observe_public_result(", "observer"),
    ("consume_public_result", "RequestCore::consume_public_result", "RequestCore::consume_public_result(", "observer"),
    ("register_observer", "RequestCore::register_observer", "RequestCore::register_observer(", "observer"),
    ("claim_observer_delivery", "RequestCore::claim_observer_delivery", "RequestCore::claim_observer_delivery(", "observer"),
    ("retire_observer_delivery", "RequestCore::retire_observer_delivery", "RequestCore::retire_observer_delivery(", "observer"),
    ("tp_publish_one", "ThreadPoolBackend::publish_one", "ThreadPoolBackend::publish_one(", "publication"),
    ("tp_deliver_event", "ThreadPoolBackend::deliver_event", "ThreadPoolBackend::deliver_event(", "publication"),
    ("tp_poll_progress", "ThreadPoolBackend::poll_progress", "ThreadPoolBackend::poll_progress(", "progress"),
    ("tp_worker_loop", "ThreadPoolBackend::worker_loop", "ThreadPoolBackend::worker_loop(", "backend"),
    ("tp_run_syscall", "ThreadPoolBackend::run_syscall", "ThreadPoolBackend::run_syscall(", "backend"),
    ("ur_publish_one", "UringAsyncBackend::publish_one", "UringAsyncBackend::publish_one(", "publication"),
    ("ur_deliver_event", "UringAsyncBackend::deliver_event", "UringAsyncBackend::deliver_event(", "publication"),
    ("ur_poll_progress", "UringAsyncBackend::poll_progress", "UringAsyncBackend::poll_progress(", "progress"),
    ("ur_poll", "UringAsyncBackend::poll", "UringAsyncBackend::poll(", "progress"),
    ("ur_reap_cqes", "UringAsyncBackend::reap_cqes", "UringAsyncBackend::reap_cqes(", "reap"),
    ("ur_handle_cqe", "UringAsyncBackend::handle_one_cqe", "UringAsyncBackend::handle_one_cqe(", "reap"),
    ("ur_find_cookie", "UringAsyncBackend::find_live_router_cookie_", "UringAsyncBackend::find_live_router_cookie_(", "reap"),
    ("ur_find_index", "UringAsyncBackend::find_live_router_index_", "UringAsyncBackend::find_live_router_index_(", "reap"),
    ("ur_finalize", "UringAsyncBackend::finalize_operation_terminal_", "UringAsyncBackend::finalize_operation_terminal_(", "terminal"),
    ("ur_dispatch_one", "UringAsyncBackend::dispatch_one_locked", "UringAsyncBackend::dispatch_one_locked(", "dispatch"),
    ("ur_submit_transport", "UringAsyncBackend::submit_transport_locked", "UringAsyncBackend::submit_transport_locked(", "submit"),
    ("ctx_wait_one", "AsyncIoContext::wait_one", "AsyncIoContext::wait_one(", "progress"),
    ("ctx_run_pass", "AsyncIoContext::run_progress_pass_", "AsyncIoContext::run_progress_pass_(", "progress"),
    ("ctx_poll_progress", "AsyncIoContext::poll_progress", "AsyncIoContext::poll_progress(", "progress"),
    ("progress_wait_unchanged", "ProgressSource::wait_if_unchanged", "ProgressSource::wait_if_unchanged", "progress"),
    ("scope_wait_for", "RequestScope::wait_for", "RequestScope::wait_for(", "progress"),
    ("scope_wait_slot", "RequestScope::wait_for_slot_", "RequestScope::wait_for_slot_", "progress"),
    ("scope_take", "RequestScope::take", "RequestScope::take(", "observer"),
    ("request_take_result", "Request::take_result", "take_result(", "observer"),
    ("bridge_consume", "RequestBridge::consume", "RequestBridge::consume(", "observer"),
    ("bridge_observe", "RequestBridge::observe", "RequestBridge::observe(", "observer"),
    ("copy_wait_take", "PipelinedCopyTask::wait_take", "wait_take<", "progress"),
    ("copy_run_body", "PipelinedCopyTask::run_body", "run_body(", "copy-main"),
    ("copy_run_context", "sluice_copy::run_pipelined_copy_with_context", "run_pipelined_copy_with_context(", "copy-main"),
    ("bench_driver_thread", "driver_thread (W-03 driver)", "driver_thread(", "driver"),
    ("bench_external_phase", "run_external_phase (W-03 submitter)", "run_external_phase(", "submitter"),
    ("bench_pipeline_phase", "run_pipeline_phase (W-02 app)", "run_pipeline_phase(", "app"),
]

CAND = {k: (name, pat, fam) for k, name, pat, fam in CANDIDATES}

# p2 R1 stage classifier, copied from
# docs/evidence/621-progress-wait-path-20261118/p2/profile/analysis/analyze_p2.py
# and recompute_p2_r1.py; this keeps the stage container comparable.
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
GENERIC_SYSCALL_RE = re.compile(
    r"entry_SYSCALL|entry_SYSRETQ|syscall_return_via_sysret|do_syscall_64|"
    r"swapgs_restore|exit_to_user_mode|syscall_exit_to_user_mode|"
    r"irqentry_exit|asm_common_interrupt|common_interrupt")
LIBC_POLL_RE = re.compile(r"^__poll(?:@.*)?$")
KERNEL_POLL_RE = re.compile(r"do_poll|__x64_sys_poll|poll_schedule|do_sys_poll")

FRAME_RE = re.compile(r"^\s+([0-9a-fA-F]+)\s+(.+?)\s+\(([^()]*)\)\s*$")
HDR_RE = re.compile(r"^(\S+)\s+(\d+)(?:/(\d+))?\s+[0-9.]+:\s+(\d+)\s+")


def strip_offset(s):
    return re.sub(r"\+0x[0-9a-fA-F]+$", "", s).strip()


def parse_script(path):
    samples = []
    comm = None
    tid = None
    period = 0
    frames = []

    def flush():
        nonlocal comm, tid, period, frames
        if comm is not None and frames:
            samples.append((comm, tid, period, frames))
        comm = None
        tid = None
        period = 0
        frames = []

    with open(path, errors="replace") as f:
        for line in f:
            if not line.strip():
                flush()
                continue
            if line[0] not in "\t ":
                flush()
                m = HDR_RE.match(line)
                if m:
                    comm = m.group(1)
                    tid = int(m.group(3) if m.group(3) else m.group(2))
                    period = int(m.group(4))
                else:
                    comm = line.split()[0] if line.split() else None
                    tid = -1
                    period = 0
                frames = []
                continue
            m = FRAME_RE.match(line.rstrip("\n"))
            if not m:
                continue
            frames.append((int(m.group(1), 16), strip_offset(m.group(2)),
                           m.group(3), m.group(3) == "inlined"))
    flush()
    return samples


def addr2line(binary, ips, cache):
    todo = sorted(set(ips) - set(cache))
    for i in range(0, len(todo), 4000):
        chunk = todo[i:i + 4000]
        inp = "".join("0x%x\n" % x for x in chunk)
        p = subprocess.run(["addr2line", "-e", binary, "-f", "-C", "-i", "-a"],
                           input=inp, text=True, capture_output=True)
        cur = None
        lines = []

        def flush():
            nonlocal cur, lines
            if cur is not None:
                chain = []
                for j in range(0, len(lines) - 1, 2):
                    chain.append((lines[j], lines[j + 1]))
                if len(lines) % 2 == 1:
                    chain.append((lines[-1], "??:0"))
                cache[cur] = chain
            cur = None
            lines = []

        for line in p.stdout.splitlines():
            if line.startswith("0x"):
                try:
                    a = int(line, 16)
                except ValueError:
                    continue
                flush()
                cur = a
            elif cur is not None:
                lines.append(line)
        flush()
    return cache


def groups(frames):
    out = []
    for f in frames:
        if not out or out[-1][0][0] != f[0]:
            out.append([f])
        else:
            out[-1].append(f)
    return out


def norm_loc(loc):
    loc = re.sub(r"\s+\(discriminator \d+\)$", "", loc)
    prefix = REPO + "/"
    if loc.startswith(prefix):
        return loc[len(prefix):]
    return loc


def leaf_category(sym):
    for name, regs in PHYS_RE:
        for r in regs:
            if r.search(sym):
                return name
    return None


def first_stage(names):
    for sym in names:
        for name, regs in STAGE_RE:
            for r in regs:
                if r.search(sym):
                    return name
    return None


def has_libc_poll(names):
    return any(LIBC_POLL_RE.match(s) for s in names)


def has_kernel_poll(names):
    return any(KERNEL_POLL_RE.search(s) for s in names)


def syscall_owner(names):
    joined = " | ".join(names)
    if re.search(r"io_uring_enter|io_submit_sqes|io_uring_submit", joined):
        return "submit"
    if re.search(r"pread64|pwrite64|vfs_read|vfs_write|ksys_read|ksys_write", joined):
        return "backend exec (TP)"
    if has_kernel_poll(names) or has_libc_poll(names):
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


def attribute_r1(names):
    leaf = names[0]
    if GENERIC_SYSCALL_RE.search(leaf):
        return syscall_owner(names)
    cat = leaf_category(leaf)
    if cat in ("payload/FS", "io-wq punt", "bench harness", "backend exec (TP)",
               "startup/thread create"):
        return cat
    stage = first_stage(names)
    if stage:
        return stage
    return cat or "other"


def attribute_r1_dedicated(names):
    if has_libc_poll(names) and not has_kernel_poll(names):
        return "poll boundary (libc __poll only)"
    return attribute_r1(names)


def role_from_names(names):
    joined = " | ".join(names)
    if "ThreadPoolBackend::worker_loop(" in joined or "worker_loop(" in joined:
        return "worker"
    if "driver_thread(" in joined:
        return "driver"
    if ("run_external_phase(" in joined or "run_pipeline_phase(" in joined or
            "run_pipelined_copy_with_context(" in joined or "run_body(" in joined or
            "wait_take<" in joined):
        return "main"
    return "unknown"


def pct(n, d):
    return 0.0 if not d else 100.0 * n / d


def top(counter, n=12):
    return [{"key": k, "count": v} for k, v in counter.most_common(n)]


def analyze_case(label, cfg, with_control=False):
    raw = os.path.join(PROFILE, cfg["dir"], "raw")
    binaries = {}
    runs = list(cfg["runs"])
    if with_control and cfg.get("control"):
        runs += cfg["control"]["runs"]
    samples = []
    per_run = {}
    comms = collections.Counter()
    for run in runs:
        path = os.path.join(raw, "script-inline-%s.txt" % run)
        rs = parse_script(path)
        per_run[run] = len(rs)
        for s in rs:
            comms[s[0]] += 1
            samples.append((run, s[0], s[1], s[2], s[3]))
    app = [s for s in samples if s[1].startswith(cfg["comm_prefix"])]
    iou = [s for s in samples if s[1].startswith("iou-wrk")]
    other = [s for s in samples if not s[1].startswith(cfg["comm_prefix"])
             and not s[1].startswith("iou-wrk")]
    total_app_period = sum(s[3] for s in app)
    total_all_period = sum(s[3] for s in app) + sum(s[3] for s in iou)
    # Resolve every user IP that occurs in app samples.
    cache = {}
    ipset = set()
    for run, comm, tid, period, frames in app:
        for ip, sym, dso, inl in frames:
            if dso == cfg["binary"]:
                ipset.add(ip)
    addr2line(cfg["binary"], ipset, cache)
    unresolved = len(ipset - set(cache))

    # First pass: per-sample names, roles, stage, candidate counts.
    cand_stats = {k: collections.Counter() for k in CAND}
    cand_self_lines = {k: collections.Counter() for k in CAND}
    cand_self_inner = {k: collections.Counter() for k in CAND}
    cand_inline_lines = {k: collections.Counter() for k in CAND}
    cand_call_lines = {k: collections.Counter() for k in CAND}
    cand_call_leaf = {k: collections.Counter() for k in CAND}
    cand_roles = {k: collections.Counter() for k in CAND}
    stage_app = collections.Counter()
    stage_all = collections.Counter()
    leaf_counter = collections.Counter()
    no_frames = 0
    single_group = 0
    depth_hist = collections.Counter()
    tid_votes = collections.defaultdict(collections.Counter)
    sample_rows = []

    for run, comm, tid, period, frames in samples:
        names = [f[1] for f in frames]
        if not names:
            if comm.startswith(cfg["comm_prefix"]):
                no_frames += 1
            stage_all["other"] += 1
            continue
        gs = groups(frames)
        gnames = []
        for g in gs:
            ns = {f[1] for f in g}
            ip = g[0][0]
            if g[0][2] == cfg["binary"] and ip in cache:
                ns |= {fn for fn, _loc in cache[ip]}
            gnames.append(ns)
        allnames = set()
        for ns in gnames:
            allnames |= ns
        real0 = next((f for f in gs[0] if not f[3]), None)
        leafsym = real0[1] if real0 else gs[0][0][1]
        near = set()
        for gi in (0, 1):
            if gi < len(gnames):
                near |= gnames[gi]
        role = role_from_names(names)
        if role != "unknown":
            tid_votes[tid][role] += 1
        stage = attribute_r1_dedicated(names)
        stage_all[stage] += 1
        is_app = comm.startswith(cfg["comm_prefix"])
        if is_app:
            stage_app[stage] += 1
            leaf_counter[leafsym] += 1
            if len(gs) == 1:
                single_group += 1
            depth_hist[min(len(gs), 20)] += 1
        sample_rows.append((run, comm, tid, period, frames, gs, gnames, near,
                            allnames, leafsym, role, is_app))

    tid_role = {}
    for tid, votes in tid_votes.items():
        total = sum(votes.values())
        role, n = votes.most_common(1)[0]
        if total >= 3 and n / total >= 0.8:
            tid_role[tid] = role

    # Second pass: candidate attribution on app samples.
    family_union = collections.defaultdict(collections.Counter)
    family_keys = collections.defaultdict(list)
    for key, (disp, pat, fam) in CAND.items():
        family_keys[fam].append(key)
    for (run, comm, tid, period, frames, gs, gnames, near, allnames, leafsym,
         role, is_app) in sample_rows:
        if not is_app:
            continue
        if role == "unknown":
            role = tid_role.get(tid, "unknown")
        real0 = next((f for f in gs[0] if not f[3]), None)
        leaf_is_binary = bool(real0 is not None and real0[2] == cfg["binary"])
        owned_families = set()
        incl_families = set()
        for key, (disp, pat, fam) in CAND.items():
            is_self = pat in leafsym
            in_group0 = any(pat in n for n in gnames[0])
            in_group1 = len(gnames) > 1 and any(pat in n for n in gnames[1])
            if is_self:
                owned_families.add(fam)
                cand_stats[key]["self"] += 1
                cand_stats[key]["self_cycles"] += period
                cand_roles[key][role] += 1
                if real0 is not None and real0[2] == cfg["binary"] and real0[0] in cache:
                    ch = cache[real0[0]]
                    if ch:
                        cand_self_lines[key][norm_loc(ch[-1][1])] += 1
                        cand_self_inner[key][norm_loc(ch[0][1]) + " [" + ch[0][0] + "]"] += 1
                    else:
                        cand_self_lines[key]["??:0"] += 1
            elif leaf_is_binary and in_group0:
                owned_families.add(fam)
                cand_stats[key]["inlined"] += 1
                cand_stats[key]["inlined_cycles"] += period
                cand_roles[key][role] += 1
                ip = gs[0][0][0]
                if ip in cache:
                    ent = next(((fn, l) for fn, l in cache[ip] if pat in fn), None)
                    if ent:
                        cand_inline_lines[key][norm_loc(ent[1])] += 1
            elif (not leaf_is_binary) and in_group1:
                owned_families.add(fam)
                cand_stats[key]["call"] += 1
                cand_stats[key]["call_cycles"] += period
                cand_roles[key][role] += 1
                cand_call_leaf[key][leafsym] += 1
                loc = None
                ip = gs[1][0][0]
                if ip in cache:
                    ent = next(((fn, l) for fn, l in cache[ip] if pat in fn), None)
                    if ent:
                        loc = norm_loc(ent[1])
                if loc is None:
                    ent2 = next((s for _ip, s, _d, _i in gs[1] if pat in s), None)
                    if ent2:
                        loc = "(symbol-only) " + ent2
                if loc:
                    cand_call_lines[key][loc] += 1
            if any(pat in n for n in allnames):
                incl_families.add(fam)
                cand_stats[key]["inc"] += 1
                cand_stats[key]["inc_cycles"] += period
        for fam in owned_families:
            family_union[fam]["owner"] += 1
            family_union[fam]["owner_cycles"] += period
        for fam in incl_families:
            family_union[fam]["inclusive"] += 1
            family_union[fam]["inclusive_cycles"] += period

    n_app = len(app)
    n_all = len(app) + len(iou)
    if cfg["kind"] == "bench":
        units = len(cfg["runs"]) * cfg["ops_per_run"]
        unit = "op"
        units_requests = units
        unit_requests = "op"
    else:
        units = len(cfg["runs"]) * cfg["chunks_per_run"]
        unit = "chunk"
        units_requests = len(cfg["runs"]) * cfg["requests_per_run"]
        unit_requests = "request"
    candidates = {}
    for key, (disp, pat, fam) in CAND.items():
        s = cand_stats[key]
        candidates[key] = {
            "display": disp,
            "family": fam,
            "pattern": pat,
            "self": s["self"],
            "self_pct_app": pct(s["self"], n_app),
            "self_pct_all": pct(s["self"], n_all),
            "inlined_self": s["inlined"],
            "inlined_self_pct_app": pct(s["inlined"], n_app),
            "callsite_extra": s["call"],
            "callsite_extra_pct_app": pct(s["call"], n_app),
            "callsite_extra_pct_all": pct(s["call"], n_all),
            "owner_total": s["self"] + s["inlined"] + s["call"],
            "owner_total_pct_app": pct(s["self"] + s["inlined"] + s["call"], n_app),
            "owner_total_pct_all": pct(s["self"] + s["inlined"] + s["call"], n_all),
            "inclusive": s["inc"],
            "inclusive_pct_app": pct(s["inc"], n_app),
            "inclusive_pct_all": pct(s["inc"], n_all),
            "self_cycles_per_unit": s["self_cycles"] / units if units else 0.0,
            "callsite_cycles_per_unit": s["call_cycles"] / units if units else 0.0,
            "owner_cycles_per_unit": (s["self_cycles"] + s["inlined_cycles"] +
                                      s["call_cycles"]) / units if units else 0.0,
            "inclusive_cycles_per_unit": s["inc_cycles"] / units if units else 0.0,
            "self_cycles_per_request": s["self_cycles"] / units_requests if units_requests else 0.0,
            "owner_cycles_per_request": (s["self_cycles"] + s["inlined_cycles"] +
                                         s["call_cycles"]) / units_requests if units_requests else 0.0,
            "self_pct_period_app": pct(s["self_cycles"], total_app_period),
            "callsite_pct_period_app": pct(s["call_cycles"], total_app_period),
            "owner_pct_period_app": pct(s["self_cycles"] + s["inlined_cycles"] +
                                        s["call_cycles"], total_app_period),
            "inclusive_pct_period_app": pct(s["inc_cycles"], total_app_period),
            "unit": unit,
            "self_lines": top(cand_self_lines[key]),
            "self_inner": top(cand_self_inner[key]),
            "inlined_lines": top(cand_inline_lines[key]),
            "callsite_lines": top(cand_call_lines[key]),
            "callsite_leaf_symbols": top(cand_call_leaf[key]),
            "roles": dict(cand_roles[key]),
        }

    families = {}
    for fam, counter in family_union.items():
        families[fam] = {
            "owner": counter["owner"],
            "owner_pct_app": pct(counter["owner"], n_app),
            "owner_pct_all": pct(counter["owner"], n_all),
            "owner_cycles_per_unit": counter["owner_cycles"] / units if units else 0.0,
            "inclusive": counter["inclusive"],
            "inclusive_pct_app": pct(counter["inclusive"], n_app),
            "inclusive_cycles_per_unit": counter["inclusive_cycles"] / units if units else 0.0,
        }

    # Stage percentages for both denominators.
    stage_rows = {}
    for stage in sorted(set(stage_app) | set(stage_all)):
        stage_rows[stage] = {
            "app": stage_app[stage],
            "app_pct": pct(stage_app[stage], n_app),
            "all": stage_all[stage],
            "all_pct": pct(stage_all[stage], n_all),
        }

    result = {
        "label": label,
        "case_id": cfg["case_id"],
        "binary": cfg["binary"],
        "comm_prefix": cfg["comm_prefix"],
        "runs": runs,
        "samples_total": len(samples),
        "samples_app": n_app,
        "samples_iou_wrk": len(iou),
        "samples_other_comm": len(other),
        "total_app_period": total_app_period,
        "total_all_period": total_all_period,
        "unit": unit,
        "units": units,
        "comm_counts": dict(comms),
        "per_run_samples": per_run,
        "binary_ips": len(ipset),
        "binary_ips_unresolved": unresolved,
        "app_no_frames": no_frames,
        "app_single_group": single_group,
        "app_depth_hist": dict(depth_hist),
        "leaf_top": top(leaf_counter, 30),
        "stages": stage_rows,
        "families": families,
        "candidates": candidates,
        "tid_roles": {str(k): v for k, v in tid_role.items()},
    }
    return result


def fmt_lines(lines, denom, n=5):
    out = []
    for row in lines[:n]:
        out.append("%s %.2f%% (%d)" % (row["key"], pct(row["count"], denom), row["count"]))
    return "; ".join(out) if out else "-"


def render(case, out):
    n = case["samples_app"]
    na = case["samples_app"] + case["samples_iou_wrk"]
    out.append("### %s  (%s)" % (case["label"], case["case_id"]))
    out.append("")
    out.append("- app samples %d, iou-wrk %d, all %d; binary IPs %d (unresolved %d); "
               "no-frame app %d; single-group app %d; period est %.1f cyc/%s app, "
               "%.1f cyc/%s all"
               % (n, case["samples_iou_wrk"], na, case["binary_ips"],
                  case["binary_ips_unresolved"], case["app_no_frames"],
                  case["app_single_group"],
                  case["total_app_period"] / case["units"], case["unit"],
                  case["total_all_period"] / case["units"], case["unit"]))
    out.append("- comms: %s" % ", ".join("%s=%d" % (k, v) for k, v in
                                        sorted(case["comm_counts"].items(), key=lambda x: -x[1])))
    out.append("")
    out.append("| candidate | self app%% | inlined-self app%% | callsite app%% | owner app%% | owner all%% | inclusive app%% | self cyc/%s | owner cyc/%s |"
               % (case["unit"], case["unit"]))
    out.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for key, c in case["candidates"].items():
        if c["inclusive"] == 0:
            continue
        out.append("| %s | %.2f%% (%d) | %.2f%% (%d) | %.2f%% (%d) | %.2f%% | %.2f%% | %.2f%% | %.1f | %.1f |"
                   % (c["display"], c["self_pct_app"], c["self"],
                      c["inlined_self_pct_app"], c["inlined_self"],
                      c["callsite_extra_pct_app"], c["callsite_extra"],
                      c["owner_total_pct_app"], c["owner_total_pct_all"],
                      c["inclusive_pct_app"], c["self_cycles_per_unit"],
                      c["owner_cycles_per_unit"]))
    out.append("")
    out.append("Candidate-family union (each sample counted once per family, app denominator):")
    out.append("")
    out.append("| family | owner app%% | owner cyc/%s | inclusive app%% |" % case["unit"])
    out.append("|---|---:|---:|---:|")
    for fam, row in sorted(case["families"].items(), key=lambda x: -x[1]["owner"]):
        out.append("| %s | %.2f%% (%d) | %.1f | %.2f%% |"
                   % (fam, row["owner_pct_app"], row["owner"],
                      row["owner_cycles_per_unit"], row["inclusive_pct_app"]))
    out.append("")
    out.append("Stage container (p2 R1 classifier, app / all):")
    out.append("")
    out.append("| stage | app% | all% |")
    out.append("|---|---:|---:|")
    for stage, row in sorted(case["stages"].items(), key=lambda x: -x[1]["app"]):
        out.append("| %s | %.2f%% (%d) | %.2f%% (%d) |"
                   % (stage, row["app_pct"], row["app"], row["all_pct"], row["all"]))
    out.append("")
    interesting = ["reserve", "accept", "offer_terminal", "claim_execution",
                   "release_execution", "begin_publication", "complete_publication",
                   "consume_public_result", "tp_publish_one", "tp_deliver_event",
                   "ur_publish_one", "ur_deliver_event", "ctx_wait_one", "ctx_run_pass",
                   "scope_wait_for", "scope_take", "copy_wait_take", "copy_run_context",
                   "bench_driver_thread", "bench_external_phase", "ur_find_cookie",
                   "ur_handle_cqe", "ur_reap_cqes"]
    for key in interesting:
        c = case["candidates"].get(key)
        if c is None or c["inclusive"] == 0:
            continue
        out.append("- **%s**: self lines: %s" % (c["display"],
                                                 fmt_lines(c["self_lines"], n, 4)))
        if c["inlined_self"]:
            out.append("  - inlined-body lines: %s" % fmt_lines(c["inlined_lines"], n, 4))
        if c["callsite_extra"]:
            out.append("  - callsite lines: %s" % fmt_lines(c["callsite_lines"], n, 4))
            out.append("  - callsite leaf: %s" % fmt_lines(c["callsite_leaf_symbols"], n, 4))
        if c["self_inner"]:
            out.append("  - self inner (actual sampled function:line): %s"
                       % fmt_lines(c["self_inner"], n, 3))
        out.append("  - roles: %s" % c["roles"])
    out.append("")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json-out", default=os.path.join(HERE, "analysis-w3.json"))
    ap.add_argument("--txt-out", default=os.path.join(HERE, "analysis-w3.txt"))
    args = ap.parse_args()
    results = {}
    for label in ("P-D", "P-C", "P-E", "P-F"):
        results[label] = analyze_case(label, CASES[label])
    control = None
    if CASES["P-F"].get("control"):
        control = analyze_case("P-F-d16-control", {
            "dir": CASES["P-F"]["dir"],
            "binary": CASES["P-F"]["binary"],
            "comm_prefix": CASES["P-F"]["comm_prefix"],
            "kind": "bench",
            "case_id": CASES["P-F"]["control"]["case_id"],
            "runs": CASES["P-F"]["control"]["runs"],
            "ops_per_run": CASES["P-F"]["control"]["ops_per_run"],
        })
    doc = {
        "generated": "2026-11-18",
        "profile_root": PROFILE,
        "denominators": {
            "P-D": {"app": results["P-D"]["samples_app"], "all": results["P-D"]["samples_app"] + results["P-D"]["samples_iou_wrk"]},
            "P-C": {"app": results["P-C"]["samples_app"], "all": results["P-C"]["samples_app"] + results["P-C"]["samples_iou_wrk"]},
            "P-E": {"app": results["P-E"]["samples_app"], "all": results["P-E"]["samples_app"] + results["P-E"]["samples_iou_wrk"]},
            "P-F": {"app": results["P-F"]["samples_app"], "all": results["P-F"]["samples_app"] + results["P-F"]["samples_iou_wrk"]},
        },
        "cases": results,
        "d16_control": control,
    }
    with open(args.json_out, "w") as f:
        json.dump(doc, f, indent=1, sort_keys=True)
    lines = []
    lines.append("# W3 row-level analysis (text rendering)")
    lines.append("")
    for label in ("P-D", "P-C", "P-E", "P-F"):
        render(results[label], lines)
    if control:
        render(control, lines)
    with open(args.txt_out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", args.json_out, args.txt_out)


if __name__ == "__main__":
    main()
