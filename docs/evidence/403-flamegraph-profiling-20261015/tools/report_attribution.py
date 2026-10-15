#!/usr/bin/env python3
"""Attributes folded perf stacks into "where does the time go" buckets.

Every folded line carries its cost on the leaf frame. A kernel leaf is charged to
the operation that entered the kernel, found by scanning the stack for a known
syscall entry (glibc's generic cancellation wrappers are skipped). Kernel-only
threads (io_uring's io-wq workers) are reported separately, because their cost is
real CPU time the submitting process does not pay directly.

Input:  data/folded/*.folded.gz and *.tid.folded.gz
Output: data/ATTRIBUTION.md
"""
import gzip
from collections import Counter
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
DATA = TOOLS_DIR.parent / "data"

SYSCALL_OF = {
    "__libc_pread": "pread",
    "__libc_pwrite": "pwrite",
    "__libc_write": "write",
    "__libc_read": "read",
    "__libc_open64": "open",
    "__libc_open": "open",
    "__libc_close": "close",
    "__libc_fsync": "fsync",
    "__libc_fdatasync": "fdatasync",
    "eventfd_write": "eventfd_write",
    "eventfd_read": "eventfd_read",
    "__libc_epoll_pwait": "epoll_wait",
    "epoll_wait": "epoll_wait",
    "io_uring_enter": "io_uring_enter",
    "io_uring_submit": "io_uring_enter",
    "__io_uring_submit": "io_uring_enter",
    "io_uring_submit_and_wait": "io_uring_enter",
    "io_uring_queue_init": "io_uring_setup",
    "io_uring_register_eventfd": "io_uring_register",
    "pthread_cond_wait@@GLIBC_2.3.2": "futex_wait (condvar)",
    "__GI___futex_abstimed_wait_cancelable64": "futex_wait (timed)",
    "__GI___lll_lock_wait": "futex_wait (mutex)",
    "__GI___lll_lock_wake": "futex_wake (mutex)",
    "pthread_cond_signal@@GLIBC_2.3.2": "futex_wake (condvar)",
    "pthread_cond_broadcast@@GLIBC_2.3.2": "futex_wake (condvar)",
    "pthread_mutex_lock@@GLIBC_2.2.5": "mutex_lock (userspace)",
    "__GI___pthread_mutex_unlock_usercnt": "mutex_unlock (userspace)",
    "clock_gettime@@GLIBC_2.17": "clock_gettime",
    "__libc_nanosleep": "nanosleep",
    "clock_nanosleep@@GLIBC_2.17": "nanosleep",
    "__GI___clone3": "clone (thread create)",
    "clone3": "clone (thread create)",
}

GENERIC = {
    "__internal_syscall_cancel",
    "__syscall_cancel",
    "__syscall_cancel_arch",
    "__syscall_cancel_arch_start",
    "__syscall_cancel_arch_end",
    "syscall",
}


def base_name(frame: str) -> str:
    return frame.split("<")[0].split("(")[0].strip()


def role_of(frames) -> str:
    root = frames[0]
    if root.startswith("iou-wrk"):
        return "io-wq kernel worker"
    if root in ("sh", "mkdir", "perf"):
        return "harness system()"
    for f in frames:
        if "worker_loop" in f or "execute_native_thread_routine" in f:
            return "worker thread"
    return "main (submit + consume)"


def classify(frames):
    """(bucket, detail) for the cost carried by this stack's leaf frame."""
    leaf = frames[-1]
    if leaf.endswith("_[k]"):
        for f in reversed(frames):
            if f.endswith("_[k]"):
                continue
            if base_name(f) in SYSCALL_OF:
                return "kernel", SYSCALL_OF[base_name(f)]
        user_frames = [f for f in frames if not f.endswith("_[k]")]
        # a kernel-only thread has just its comm name in the stack
        if len(user_frames) <= 1:
            return "kernel (io-wq worker)", leaf[:-4]
        for f in reversed(frames):
            if f.endswith("_[k]") or base_name(f) in GENERIC:
                continue
            if f is frames[0]:
                break
            return "kernel", "syscall machinery via " + base_name(f)
        return "kernel", "syscall machinery (unattributed)"

    if "sluice::" in leaf:
        return "sluice", base_name(leaf)
    if leaf.startswith(("__memcmp", "__memmove", "__memcpy", "memcpy", "memcmp")):
        return "libc mem*", leaf
    if leaf.startswith(("__GI___pthread", "pthread_", "__gthread", "__libc_pthread")):
        return "libc pthread", leaf
    if leaf.startswith(("std::", "__gnu_cxx::")):
        return "c++ runtime", base_name(leaf)
    if leaf.startswith(("bench::", "main", "_start", "operator", "precheck", "verify",
                        "retry_on_eintr", "fill_pattern", "time_ops", "run_case")):
        return "bench harness", base_name(leaf)
    if leaf.startswith("__libc_"):
        return "libc wrapper self", base_name(leaf)
    if leaf == "[unknown]":
        return "unresolved", leaf
    return "other", base_name(leaf)


def iter_stacks(path: Path):
    with gzip.open(path, "rt") as handle:
        for line in handle:
            stack, _, count = line.rstrip("\n").rpartition(" ")
            if count.isdigit():
                yield stack.split(";"), int(count)


def table(out, folded: Path, roles: bool):
    total = 0
    buckets = Counter()
    detail = Counter()
    by_role = {}
    for frames, count in iter_stacks(folded):
        b, d = classify(frames)
        total += count
        buckets[b] += count
        detail[(b, d)] += count
        if roles:
            by_role.setdefault(role_of(frames), Counter())[b] += count
    out.append(f"## {folded.name.replace('.tid.folded.gz', '').replace('.folded.gz', '')}"
               f"  (total {total / 1e9:.1f} G cycles)")
    out.append("")
    out.append("| bucket | share |")
    out.append("|---|---|")
    for b, c in buckets.most_common():
        out.append(f"| {b} | {100.0 * c / total:.1f}% |")
    out.append("")
    out.append("| bucket | detail | share |")
    out.append("|---|---|---|")
    for (b, d), c in detail.most_common(16):
        out.append(f"| {b} | {d} | {100.0 * c / total:.2f}% |")
    out.append("")
    if roles:
        for r, counter in sorted(by_role.items(), key=lambda kv: -sum(kv[1].values())):
            rt = sum(counter.values())
            if rt / total < 0.005:
                continue
            top = ", ".join(f"{b} {100.0 * c / rt:.0f}%" for b, c in counter.most_common(5))
            out.append(f"- **{r}** — {100.0 * rt / total:.1f}% of all cycles: {top}")
        out.append("")


def main():
    out = ["# Where the time goes (cycle-weighted self cost)", "",
           "Kernel cost is charged to the operation that entered the kernel.",
           "Shares are of all cycles captured for the case, across all threads.", ""]
    for folded in sorted(DATA.glob("folded/*.folded.gz")):
        if ".tid." in folded.name:
            continue
        table(out, folded, roles=False)
    out.append("# Per-thread-role split")
    out.append("")
    for folded in sorted(DATA.glob("folded/*.tid.folded.gz")):
        table(out, folded, roles=True)
    (DATA / "ATTRIBUTION.md").write_text("\n".join(out))
    print("wrote", DATA / "ATTRIBUTION.md")


if __name__ == "__main__":
    main()
