#!/usr/bin/env python3
"""P2 #621 R1 read-only recompute of archived perf script evidence.

This is an evidence generator, not a profiling driver: it reads the archived
P-*/raw/script-cycles-sym.txt, stat-pmu-r3.txt and stat-tp-uring.txt files and
never runs perf/bench/test.  It exists because the R0 analyzer's
syscall_owner()/first_stage() did not recognize a bare libc __poll frame and
because several R0 inclusive figures added nested symbols.  The original
analyze_p2.py and analysis.json are left byte-identical for evidence integrity;
this script imports their resolver/parser/leaf classifier and supplies the
corrected attribution and aggregation.

Usage (from any directory):
  python3 -I recompute_p2_r1.py [--json-out /tmp/p2-r1.json]

Outputs a JSON document to stdout (and optionally to --json-out).  The
kallsyms/twin paths default to the session paths in /tmp/p2-621; override with
--kallsyms and --twins-dir if the archived session is reconstructed elsewhere.
"""
import argparse
import importlib.util
import json
import math
import os
import re
import subprocess
import sys
from collections import Counter
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
PROFILE_ROOT = os.path.dirname(HERE)
ANALYZER = os.path.join(HERE, "analyze_p2.py")

sys.path.insert(0, HERE)
import analyze_p2 as base  # noqa: E402


def load_base_module():
    if not os.path.exists(ANALYZER):
        raise SystemExit(f"missing archived analyzer: {ANALYZER}")
    return base


# ---------------------------------------------------------------------------
# Corrected poll recognition
# ---------------------------------------------------------------------------

LIBC_POLL_RE = re.compile(r"^__poll(?:@.*)?$")
KERNEL_POLL_RE = re.compile(r"do_poll|__x64_sys_poll|poll_schedule|do_sys_poll")
POLL_BOUNDARY = "poll boundary (libc __poll only)"


def has_libc_poll(frames):
    return any(LIBC_POLL_RE.match(sym) for _ip, _dso, sym in frames)


def has_kernel_poll(frames):
    return any(KERNEL_POLL_RE.search(sym) for _ip, _dso, sym in frames)


def libc_poll_only(frames):
    return has_libc_poll(frames) and not has_kernel_poll(frames)


def syscall_owner_r1(frames):
    """R0 syscall_owner() plus the bare libc __poll boundary."""
    syms = [s for _ip, _dso, s in frames]
    joined = " | ".join(syms)
    if re.search(r"io_uring_enter|io_submit_sqes|io_uring_submit", joined):
        return "submit"
    if re.search(r"pread64|pwrite64|vfs_read|vfs_write|ksys_read|ksys_write", joined):
        return "backend exec (TP)"
    if KERNEL_POLL_RE.search(joined) or any(LIBC_POLL_RE.match(s) for s in syms):
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


def first_stage_r1(frames):
    """R0 first_stage() plus the bare libc __poll frame."""
    for _ip, _dso, sym in frames:
        if LIBC_POLL_RE.match(sym):
            return "progress/wait"
        for name, regs in base.STAGE_RE:
            for reg in regs:
                if reg.search(sym):
                    return name
    return None


def attribute_r1(frames):
    """Corrected attribution without the dedicated poll-boundary row."""
    leaf = frames[0][2]
    if base.GENERIC_SYSCALL_RE.search(leaf):
        return syscall_owner_r1(frames)
    cat = base.leaf_category(leaf)
    if cat in ("payload/FS", "io-wq punt", "bench harness", "backend exec (TP)",
               "startup/thread create"):
        return cat
    stage = first_stage_r1(frames)
    if stage:
        return stage
    return cat or "other"


def attribute_r1_dedicated(frames):
    """R1 stage partition: all libc-__poll-only samples get their own row."""
    if libc_poll_only(frames):
        return POLL_BOUNDARY
    return attribute_r1(frames)


def progress_wait_subclass(comm, frames):
    """Disjoint composition of the R1 progress/wait stage."""
    syms = [s for _ip, _dso, s in frames]
    joined = " | ".join(syms)
    has_poll = has_kernel_poll(frames) or has_libc_poll(frames)
    has_pass = any(re.search(
        r"poll_progress|run_progress_pass_|reap_cqes|handle_one_cqe|"
        r"publish_one|deliver_event|claim_execution|wait_one|wait_for", s)
        for s in syms)
    has_eventfd = "eventfd" in joined
    worker = comm.startswith("iou-wrk")
    if has_poll and has_pass:
        return "poll+progress-pass"
    if has_poll:
        return "app-poll only"
    if has_pass:
        return "progress-pass only"
    if worker and has_eventfd:
        return "worker-eventfd"
    if has_eventfd:
        return "eventfd only"
    return "other wait"


def union_counts(syms, a_pred, b_pred):
    a = any(a_pred(s) for s in syms)
    b = any(b_pred(s) for s in syms)
    return {
        "a": int(a),
        "b": int(b),
        "both": int(a and b),
        "union": int(a or b),
    }


# ---------------------------------------------------------------------------
# A/A frozen-band constants (transcribed, not re-measured)
# ---------------------------------------------------------------------------

# p1/AA-CALIBRATION.md:181-217, 386-402 (P1 cells) and
# p1/AA-CALIBRATION-2.md:441-448 (P2 cells).  Values are percent points.
AA_CELLS = {
    "C1": dict(
        workload="sluice-copy 4k/d4/w1 --sync none --no-atomic",
        binary="db972b19...", matching="P-E", verdict="QUIET",
        source="p1/AA-CALIBRATION.md:24,181-217,386-402",
        metrics={
            "cpu": dict(hw=0.365, sd=0.786, mde80=0.589),
            "instr": dict(hw=0.005, sd=0.012, mde80=0.009),
            "wall": dict(hw=0.708, sd=1.495, mde80=1.120),
            "cycles": dict(hw=1.121, sd=2.346, mde80=1.758),
        }),
    "C6": dict(
        workload="W02-uring-4k-d1", binary="03dbef09...", matching="P-B",
        verdict="ORDER_SENSITIVE (wall)", wall_order_sensitive=True,
        source="p1/AA-CALIBRATION.md:29,386-402; wall effect +1.132pp p=0.0049",
        metrics={
            "cpu": dict(hw=0.471, sd=0.997, mde80=0.747),
            "instr": dict(hw=0.006, sd=0.014, mde80=0.010),
            "wall": dict(hw=0.885, sd=1.885, mde80=1.413),
            "cycles": dict(hw=1.368, sd=2.876, mde80=2.155),
        }),
    "C3b": dict(
        workload="W02-uring-4k-d16", binary="03dbef09...", matching="P-A",
        verdict="ENVIRONMENT_BLOCKED",
        source="p1/AA-CALIBRATION-2.md:441-448",
        metrics={
            "cycles": dict(hw=0.621, sd=1.319, mde80=0.988),
            "cpu": dict(hw=2.597, sd=5.526, mde80=4.141),
        }),
    "C4": dict(
        workload="W03-uring-4k-d16", binary="43054fcd...", matching="P-C",
        verdict="QUALIFIED",
        source="p1/AA-CALIBRATION.md:386-402",
        metrics={
            "cpu": dict(hw=1.402, sd=3.019, mde80=2.262),
            "cycles": dict(hw=3.843, sd=8.123, mde80=6.088),
        }),
    "C2b": dict(
        workload="W02-threadpool-4k-d16-w4", binary="03dbef09...", matching="P-D",
        verdict="NOISY (no usable band)",
        source="p1/AA-CALIBRATION-2.md:19-31,441-448",
        metrics={}),
    "C5b": dict(
        workload="W02-threadpool-4k-d1-w1", binary="03dbef09...",
        matching="not P-D (d16) and not P-B (uring)",
        verdict="QUIET conditional; not a matching cell",
        source="p1/AA-CALIBRATION-2.md:441-448",
        metrics={"cpu": dict(hw=0.458, sd=0.964, mde80=0.723)}),
}


def _betacf(a, b, x):
    tiny = 1e-300
    qab = a + b
    qap = a + 1.0
    qam = a - 1.0
    c = 1.0
    d = 1.0 - qab * x / qap
    if abs(d) < tiny:
        d = tiny
    d = 1.0 / d
    h = d
    for m in range(1, 300):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        if abs(d) < tiny:
            d = tiny
        c = 1.0 + aa / c
        if abs(c) < tiny:
            c = tiny
        d = 1.0 / d
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        if abs(d) < tiny:
            d = tiny
        c = 1.0 + aa / c
        if abs(c) < tiny:
            c = tiny
        d = 1.0 / d
        delta = d * c
        h *= delta
        if abs(delta - 1.0) < 3.0e-14:
            break
    return h


def _betai(a, b, x):
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    lbt = (math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b)
           + a * math.log(x) + b * math.log1p(-x))
    bt = math.exp(lbt)
    if x < (a + 1.0) / (a + b + 2.0):
        return bt * _betacf(a, b, x) / a
    return 1.0 - bt * _betacf(b, a, 1.0 - x) / b


def t_cdf(t, df):
    x = df / (df + t * t)
    tail = 0.5 * _betai(df / 2.0, 0.5, x)
    return 1.0 - tail if t >= 0.0 else tail


def t_ppf(p, df):
    lo, hi = 0.0, 1.0
    while t_cdf(hi, df) < p:
        hi *= 2.0
        if hi > 1.0e7:
            raise RuntimeError("t_ppf failed to bracket")
    for _ in range(200):
        mid = 0.5 * (lo + hi)
        if t_cdf(mid, df) < p:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


def required_n_power(sd, delta, power=0.80):
    for n in range(2, 100000):
        crit = t_ppf(0.975, n - 1) + t_ppf(power, n - 1)
        if crit * sd / math.sqrt(n) <= delta:
            return n
    return None


def required_n_halfwidth(sd, target):
    for n in range(2, 100000):
        if t_ppf(0.975, n - 1) * sd / math.sqrt(n) <= target:
            return n
    return None


# ---------------------------------------------------------------------------
# Raw readers
# ---------------------------------------------------------------------------

def pmu_totals(form_cfg, raw_dir):
    rows = base.parse_stat(os.path.join(raw_dir, "stat-pmu-r3.txt"))
    out = {}
    denom = form_cfg["ops"] if form_cfg["bench"] else form_cfg["chunks"]
    for prefix in ("cycles", "instructions"):
        u = rows.get(prefix + ":u", {}).get("value")
        k = rows.get(prefix + ":k", {}).get("value")
        if u is not None and k is not None:
            total = u + k
            out[prefix] = dict(u=u, k=k, total=total, per_op=total / denom)
    return out


def tp_submit_count(raw_dir):
    rows = base.parse_stat(os.path.join(raw_dir, "stat-tp-uring.txt"))
    row = rows.get("io_uring_submit_req") or rows.get("io_uring:io_uring_submit_req")
    return None if row is None else row["value"]


def objdump_lock_add(twin_path):
    if not os.path.exists(twin_path):
        return dict(error=f"missing twin: {twin_path}")
    cp = subprocess.run(
        ["objdump", "-d", "--start-address=0x424190",
         "--stop-address=0x424230", twin_path],
        capture_output=True, text=True)
    lines = cp.stdout.splitlines()
    hit = next((ln.strip() for ln in lines if "lock addq" in ln), None)
    around = [ln.strip() for ln in lines if "submit_transport_locked" in ln or "lock addq" in ln]
    return dict(twin=twin_path, rc=cp.returncode, lock_add_line=hit, lines=around)


# ---------------------------------------------------------------------------
# Main recompute
# ---------------------------------------------------------------------------

def analyze_form(form, cfg, resolve):
    raw = os.path.join(PROFILE_ROOT, form, "raw")
    path = os.path.join(raw, "script-cycles-sym.txt")
    comm_filter = base.is_bench_comm if cfg["bench"] else base.is_copy_comm
    n = 0
    stage_r0 = Counter()
    stage_minfix = Counter()
    stage_r1 = Counter()
    poll_boundary_from = Counter()
    poll_boundary_eventfd = 0
    poll_boundary_app = 0
    poll_boundary_worker = 0
    comp_r1 = Counter()
    comp_minfix = Counter()
    inclusive = Counter()
    union = {}
    libc_only_n = 0
    app = 0
    app_nobench = 0
    app_bench = 0
    wait_one_n = 0
    poll_no_wait = 0
    four_frame_poll = 0
    claim_leaf = 0
    claim_stage = Counter()
    publication = 0
    w3_family = Counter()
    product_prefix = 0
    driver_thread_leaf = 0
    router_leaf = 0
    router_inclusive = 0

    for comm, frames in base.parse_script(path, resolve, comm_filter):
        n += 1
        syms = [s for _ip, _dso, s in frames]
        joined = " | ".join(syms)
        r0 = base.attribute(frames)
        minfix = attribute_r1(frames)
        r1 = attribute_r1_dedicated(frames)
        stage_r0[r0] += 1
        stage_minfix[minfix] += 1
        stage_r1[r1] += 1

        is_libc_only = libc_poll_only(frames)
        if is_libc_only:
            libc_only_n += 1
            poll_boundary_from[r0] += 1
            if "eventfd" in joined:
                poll_boundary_eventfd += 1
            if comm.startswith("iou-wrk"):
                poll_boundary_worker += 1
            else:
                poll_boundary_app += 1

        if r1 == "progress/wait":
            comp_r1[progress_wait_subclass(comm, frames)] += 1
        if minfix == "progress/wait":
            comp_minfix[progress_wait_subclass(comm, frames)] += 1

        for sym in syms:
            if LIBC_POLL_RE.match(sym):
                inclusive["__poll"] += 1
            if sym.startswith("__pollwait"):
                inclusive["__pollwait"] += 1
            if sym.startswith("__x64_sys_poll"):
                inclusive["__x64_sys_poll"] += 1
            if sym.startswith("do_sys_poll"):
                inclusive["do_sys_poll"] += 1
            if sym.startswith("poll_schedule_timeout"):
                inclusive["poll_schedule_timeout"] += 1
            if sym == "io_eventfd_signal":
                inclusive["io_eventfd_signal"] += 1
            if "eventfd_signal_mask" in sym:
                inclusive["eventfd_signal_mask"] += 1
            if "poll_progress" in sym:
                inclusive["poll_progress"] += 1
            if "wait_one" in sym:
                inclusive["wait_one"] += 1

        for name, (a_hit, b_hit) in (
                ("poll_progress",
                 (any("AsyncIoContext::poll_progress" in s for s in syms),
                  any("UringAsyncBackend::poll_progress" in s for s in syms))),
                ("eventfd_signal",
                 (any(s == "io_eventfd_signal" for s in syms),
                  any("eventfd_signal_mask" in s for s in syms))),
                ("wait_one",
                 (any("AsyncIoContext::wait_one()" in s for s in syms),
                  any("AsyncIoContext::wait_one(" in s
                      and "AsyncIoContext::wait_one()" not in s for s in syms))),
        ):
            rec = union.setdefault(name, {"a": 0, "b": 0, "both": 0, "union": 0})
            rec["a"] += int(a_hit)
            rec["b"] += int(b_hit)
            rec["both"] += int(a_hit and b_hit)
            rec["union"] += int(a_hit or b_hit)

        leaf = frames[0][2]
        if "find_live_router_cookie_" in leaf:
            router_leaf += 1
        if any("find_live_router_cookie_" in s for s in syms):
            router_inclusive += 1
        if leaf.startswith("sluice::") or leaf.startswith("sluice_copy::"):
            product_prefix += 1
        if "driver_thread" in leaf:
            driver_thread_leaf += 1

        if not comm.startswith("iou-wrk"):
            app += 1
            if any("bench_w" in dso or "sluice-copy" in dso
                   for _ip, dso, _sym in frames):
                app_bench += 1
            else:
                app_nobench += 1

        if any("wait_one" in s for s in syms):
            wait_one_n += 1
        if has_libc_poll(frames) and not any("wait_one" in s for s in syms):
            poll_no_wait += 1
        if len(frames) == 4 and has_libc_poll(frames):
            four_frame_poll += 1

        if "claim_execution" in leaf:
            claim_leaf += 1
            claim_stage[r1] += 1
        if r1 == "publication":
            publication += 1
        if r1 in ("admission", "terminal/release", "publication", "observation"):
            w3_family[r1] += 1

    def share(counter):
        return {k: dict(count=v, share=v / n if n else 0)
                for k, v in counter.items()}

    return dict(
        samples=n,
        stage_r0=share(stage_r0),
        stage_minfix=share(stage_minfix),
        stage_r1=share(stage_r1),
        poll_boundary=dict(
            count=libc_only_n,
            share=libc_only_n / n if n else 0,
            from_stage=dict(poll_boundary_from),
            eventfd=poll_boundary_eventfd,
            app=poll_boundary_app,
            worker=poll_boundary_worker,
        ),
        progress_wait_composition_r1=share(comp_r1),
        progress_wait_composition_minfix=share(comp_minfix),
        inclusive={k: dict(count=v, share=v / n if n else 0)
                   for k, v in inclusive.items()},
        unions={k: dict(v, share=v["union"] / n if n else 0)
                for k, v in union.items()},
        app_frames=dict(app=app, no_bench_frame=app_nobench,
                        with_bench_frame=app_bench),
        wait_path=dict(wait_one=wait_one_n,
                       poll_without_wait_one=poll_no_wait,
                       four_frame_poll=four_frame_poll),
        claim_execution=dict(leaf_count=claim_leaf,
                             stages=dict(claim_stage)),
        publication=dict(count=publication,
                         share=publication / n if n else 0),
        w3_family=dict(count=sum(w3_family.values()),
                       share=sum(w3_family.values()) / n if n else 0,
                       by_stage=dict(w3_family)),
        product_leaf_visible=dict(prefix=product_prefix,
                                  driver_thread=driver_thread_leaf),
        m53_router=dict(leaf=router_leaf, leaf_share=router_leaf / n if n else 0,
                        inclusive=router_inclusive,
                        inclusive_share=router_inclusive / n if n else 0),
        pmu=pmu_totals(cfg, raw),
        io_uring_submit_req=tp_submit_count(raw),
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kallsyms", default="/tmp/p2-621/kallsyms.txt")
    ap.add_argument("--twins-dir", default="/tmp/p2-621/symtwin")
    ap.add_argument("--json-out", default=None)
    args = ap.parse_args()

    base.KALLSYMS = args.kallsyms
    base.TWINS = {
        "bench_w02_pipeline": os.path.join(args.twins_dir, "bench_w02_pipeline.sym"),
        "bench_w03_external_loop": os.path.join(args.twins_dir, "bench_w03_external_loop.sym"),
        "sluice-copy": os.path.join(args.twins_dir, "sluice-copy.sym"),
    }
    resolve = base.make_resolver()

    forms = {}
    for form, cfg in base.FORMS.items():
        forms[form] = analyze_form(form, cfg, resolve)

    # M-42: only the three io_uring forms execute submit_flushes_.
    m42 = {
        "mechanism": "src/async/uring_transport.cpp:222 fetch_add",
        "objdump": objdump_lock_add(base.TWINS["bench_w02_pipeline"]),
        "lock_rmw_cycles": [19, 20],
        "forms": {},
    }
    for form, cfg in base.FORMS.items():
        pmu = forms[form]["pmu"]
        cycles = pmu.get("cycles", {}).get("per_op")
        instr = pmu.get("instructions", {}).get("per_op")
        submit = forms[form]["io_uring_submit_req"]
        rec = dict(io_uring_submit_req=submit,
                   cycles_per_op=cycles, instructions_per_op=instr)
        if submit and submit > 1 and cycles and instr:
            rec["cycles_pct_19"] = 19 / cycles * 100
            rec["cycles_pct_20"] = 20 / cycles * 100
            rec["one_instr_pct"] = 1 / instr * 100
        else:
            rec["note"] = "no uring submit in this form; M-42 arithmetic N/A"
        m42["forms"][form] = rec

    aa = {}
    for cell, info in AA_CELLS.items():
        row = dict(info)
        metrics = {}
        for metric, vals in info["metrics"].items():
            m = dict(vals)
            m["n80_delta_1_2"] = required_n_power(vals["sd"], 1.2)
            m["n_half_delta_1_2"] = required_n_halfwidth(vals["sd"], 1.2 / 2.0)
            metrics[metric] = m
        row["metrics"] = metrics
        aa[cell] = row

    out = dict(
        script="p2/profile/analysis/recompute_p2_r1.py",
        generated_utc=datetime.now(timezone.utc).isoformat(),
        analyzer="p2/profile/analysis/analyze_p2.py (sha256 b822ea3a...)",
        inputs=dict(kallsyms=args.kallsyms, twins_dir=args.twins_dir,
                    profile_root=PROFILE_ROOT),
        forms=forms,
        aa=dict(
            note="Values transcribed from p1 A/A reports; not re-measured here.",
            cells=aa,
            matching={
                "P-A": "C3b (W02-uring-4k-d16; ENVIRONMENT_BLOCKED)",
                "P-B": "C6 (W02-uring-4k-d1; cycles/CPU usable, wall ORDER_SENSITIVE)",
                "P-C": "C4 (W03-uring-4k-d16; QUALIFIED)",
                "P-D": "C2b (W02-TP-4k-d16-w4; NOISY, no usable band)",
                "P-E": "C1 (sluice-copy 4k/d4/w1; QUIET)",
            },
        ),
        m42=m42,
    )
    text = json.dumps(out, indent=1, ensure_ascii=False)
    print(text)
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")


if __name__ == "__main__":
    main()
