#!/usr/bin/env python3
"""Attributes folded perf stacks into "where does the time go" buckets.

Three kinds of evidence appear in this file and are kept apart:

* directly sampled self cost -- the leaf frame is what the CPU was executing
  when the sample fired. Every share here is of this kind.
* call-path attribution -- a kernel leaf is charged to the syscall that entered
  the kernel, found by walking down the stack for a glibc syscall wrapper. This
  is an inference from the stack, and it assumes the frame-pointer unwind is
  complete. Each case reports how often the wrapper sat directly above the
  kernel frames (`direct`), how often it was only found after skipping frames
  this map does not know (`gap`, with the skipped frames named), and how often
  no wrapper was found at all (`no-wrapper`). Kernel cycles whose stack starts
  at an interrupt or exception entry (`interrupt`) are not syscall time and are
  reported as their own bucket.
* directly observed events -- the syscall census in data/SYSCALLS.md comes from
  in-kernel counting, a different instrument; it counts events, not cycles.

Kernel-only threads (io_uring's io-wq workers) are reported separately, because
their cost is real CPU time the submitting process does not pay directly.

Symbol names are normalised before lookup: the `@@GLIBC_*` version suffix is
dropped and glibc's `__GI_` internal alias prefix is stripped, so
`__GI___libc_write` and `__libc_write` are the same symbol. Charging kernel
cycles to a frame that is not a syscall wrapper (a Sluice function, `memcmp`,
`std::chrono`) is never done: those stacks are reported as unresolved instead,
because the unwind, not the code, is what failed there.

Input:  data/folded/*.folded.gz and *.tid.folded.gz
Output: data/ATTRIBUTION.md
"""
import gzip
from collections import Counter
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
DATA = TOOLS_DIR.parent / "data"

# Keys are normalised symbol names (see norm()).
SYSCALL_OF = {
    "__libc_pread": "pread",
    "__libc_pwrite": "pwrite",
    "__libc_write": "write",
    "__libc_read": "read",
    "read": "read",
    "write": "write",
    "__libc_open64": "open",
    "__libc_open": "open",
    "__libc_close": "close",
    "close": "close",
    "__libc_fsync": "fsync",
    "__libc_fdatasync": "fdatasync",
    "fdatasync": "fdatasync",
    "eventfd_write": "eventfd write",
    "eventfd_read": "eventfd read",
    "__libc_epoll_pwait": "epoll_wait",
    "epoll_wait": "epoll_wait",
    "__poll": "poll",
    "poll": "poll",
    "io_uring_enter": "io_uring_enter",
    "io_uring_submit": "io_uring_enter",
    "__io_uring_submit": "io_uring_enter",
    "io_uring_submit_and_wait": "io_uring_enter",
    "io_uring_queue_init": "io_uring setup",
    "__io_uring_queue_init_params": "io_uring setup",
    "io_uring_mmap": "io_uring setup",
    "io_uring_register_eventfd": "io_uring_register",
    "io_uring_queue_exit": "io_uring teardown",
    "pthread_cond_wait": "futex_wait (condvar)",
    "__futex_abstimed_wait_cancelable64": "futex_wait (timed)",
    "__lll_lock_wait": "futex_wait (mutex)",
    "__lll_lock_wake": "futex_wake (mutex)",
    "pthread_cond_signal": "futex_wake (condvar)",
    "pthread_cond_broadcast": "futex_wake (condvar)",
    "pthread_mutex_lock": "mutex_lock (userspace)",
    "__pthread_mutex_unlock_usercnt": "mutex_unlock (userspace)",
    "__pthread_mutex_cond_lock": "mutex_cond_lock (userspace)",
    "clock_gettime": "clock_gettime",
    "__libc_nanosleep": "nanosleep",
    "clock_nanosleep": "nanosleep",
    "__clone3": "clone",
    "clone3": "clone",
    "__madvise": "madvise",
    "__mmap64": "mmap",
    "mmap": "mmap",
    "__munmap": "munmap",
    "__execve": "execve",
}

GENERIC = {
    "__internal_syscall_cancel",
    "__syscall_cancel",
    "__syscall_cancel_arch",
    "__syscall_cancel_arch_start",
    "__syscall_cancel_arch_end",
    "syscall",
}

SYSCALL_ENTRY = ("entry_SYSCALL_64", "do_syscall_64", "entry_SYSRETQ",
                 "syscall_return_via_sysret")
INTERRUPT_ENTRY = ("asm_sysvec", "asm_exc", "__irqentry_text", "__softirqentry_text",
                   "apic_timer_interrupt", "error_entry", "irq_entries_start")

# The completion notification is a write(2) to the progress eventfd; the caller
# chain in the same stack is what identifies it as the notification rather than
# a stdout write.
CALLER_LABELS = [
    ("sluice::async::detail::ProgressSource::wake_notification_nolock_",
     "write", "eventfd write (completion notification)"),
    ("sluice::async::detail::ProgressSource::drain_notification_nolock_",
     "read", "eventfd read (notification drain)"),
]


def norm(frame: str) -> str:
    """Symbol name without version suffix or glibc's internal alias prefix."""
    name = frame.split("<")[0].split("(")[0].strip()
    name = name.split("@@")[0]
    if name.startswith("__GI_"):
        name = name[len("__GI_"):]
    return name


def is_kernel(frame: str) -> bool:
    return frame.endswith("_[k]")


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


def refine(detail: str, frames) -> str:
    for caller, syscall, label in CALLER_LABELS:
        if detail == syscall and any(caller in f for f in frames):
            return label
    return detail


def classify(frames):
    """(bucket, detail, confidence) for the cost carried by this stack's leaf."""
    leaf = frames[-1]
    if is_kernel(leaf):
        user_frames = [f for f in frames if not is_kernel(f)]
        # a kernel-only thread has just its comm name in the stack
        if len(user_frames) <= 1:
            return "kernel (io-wq worker)", leaf[:-4], "direct"
        entering = None
        matched = None
        for f in reversed(frames):
            if is_kernel(f):
                continue
            if norm(f) in GENERIC:
                continue
            if entering is None:
                entering = f
            if norm(f) in SYSCALL_OF:
                matched = f
                break
        if matched is not None:
            detail = refine(SYSCALL_OF[norm(matched)], frames)
            if matched == entering:
                return "kernel", detail, "direct"
            return "kernel", detail, "gap (skipped " + norm(entering) + ")"
        kernel_frames = [f for f in frames if is_kernel(f)]
        bottom = kernel_frames[0]
        if any(bottom.startswith(p) for p in INTERRUPT_ENTRY):
            return "kernel (interrupt/exception)", bottom[:-4], "interrupt"
        return ("kernel (syscall, wrapper unresolved)",
                norm(entering) if entering else "unattributed", "no-wrapper")

    name = norm(leaf)
    if "sluice::" in leaf:
        return "sluice", name, "direct"
    if name.startswith(("__memcmp", "__memmove", "__memcpy", "memcpy", "memcmp")):
        return "libc mem*", name, "direct"
    if name.startswith(("__pthread", "pthread_", "__gthread", "__libc_pthread",
                        "__lll_", "__futex")):
        return "libc pthread", name, "direct"
    if name.startswith(("std::", "__gnu_cxx::")):
        return "c++ runtime", name, "direct"
    if name.startswith(("bench::", "main", "_start", "operator", "precheck", "verify",
                        "retry_on_eintr", "fill_pattern", "time_ops", "run_case")):
        return "bench harness", name, "direct"
    if name.startswith("__libc_"):
        return "libc wrapper self", name, "direct"
    if leaf == "[unknown]":
        return "unresolved", leaf, "direct"
    return "other", name, "direct"


def iter_stacks(path: Path):
    with gzip.open(path, "rt") as handle:
        for line in handle:
            stack, _, count = line.rstrip("\n").rpartition(" ")
            if count.isdigit():
                yield stack.split(";"), int(count)


def table(out, folded: Path, roles: bool):
    total = 0
    kernel_leaf_total = 0
    buckets = Counter()
    detail = Counter()
    confidence = Counter()
    gaps = Counter()
    by_role = {}
    for frames, count in iter_stacks(folded):
        b, d, c = classify(frames)
        total += count
        if is_kernel(frames[-1]):
            kernel_leaf_total += count
            confidence[c.split(" ")[0]] += count
            if c.startswith("gap"):
                gaps[(c[len("gap (skipped "):-1], d)] += count
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
    out.append(
        f"call-path confidence, as a share of the {kernel_leaf_total / 1e9:.1f} G cycles"
        f" with a kernel leaf ({100.0 * kernel_leaf_total / total:.1f}% of the case): "
        + ", ".join(f"{k} {100.0 * v / kernel_leaf_total:.2f}%"
                    for k, v in sorted(confidence.items(), key=lambda kv: -kv[1]))
    )
    out.append("")
    if gaps:
        out.append("kernel cycles charged only after skipping an unrecognised frame"
                   " (share of kernel-leaf cycles):")
        out.append("")
        for (skipped, charged), c in gaps.most_common(5):
            out.append(f"* {100.0 * c / kernel_leaf_total:.2f}% — entering frame"
                       f" `{skipped}`, charged to `{charged}`")
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
    out = [
        "# Where the time goes (cycle-weighted self cost)",
        "",
        "Evidence classes: every share is **directly sampled** leaf self cost (what the",
        "CPU was executing when the sample fired). Kernel cycles are additionally",
        "**charged by call path** to the syscall that entered the kernel; that charge is",
        "an inference from the stack, not an observation, and the per-case confidence",
        "line below says how often the wrapper was found directly above the kernel",
        "frames and how often frames had to be skipped. Kernel cycles with no syscall",
        "entering frame are reported as `kernel (interrupt/exception)` or",
        "`kernel (syscall, wrapper unresolved)` and are never charged to a Sluice",
        "function. Syscall **event counts** are a separate instrument and live in",
        "`SYSCALLS.md`.",
        "",
        "Shares are of all cycles captured for the case, across all threads.",
        "",
    ]
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
