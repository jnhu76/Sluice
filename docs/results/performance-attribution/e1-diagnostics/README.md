# E1 Round-1 Diagnostics (non-canonical)

Supplementary diagnostic captures for the E1 abstraction-tax round
(#221 G0). These are NOT canonical evidence artifacts: they were captured
on a dirty-diagnostic basis (separate invocations after the canonical
runs) and are excluded from `perf-evidence-validate.py`'s artifact glob by
living in this subdirectory with non-JSON extensions. The canonical
evidence is the four `e1-round1-*.json` artifacts in the parent directory.

Host/tooling: same host and build as the canonical artifacts (WSL2, Ryzen
7 5800H, Release clang, tmpfs); `perf 7.0.12` under root with
`--call-graph dwarf` (the Release build omits frame pointers, so the
default FP unwinder breaks user stacks); `bpftrace v0.25.0`
(unprivileged BPF disabled on this host — probes ran under sudo).

## Flame graphs (CPU, 199 Hz, one cell)

`e1-round1-flame-L1-4k-d1-read.svg` / `e1-round1-flame-L2-4k-d1-read.svg`
(+ `.folded` stack listings): L1_pool vs L2_sluice on the tax-heavy cell
(4 KiB, depth 1, workers 4, READ, 1 GiB, 2 reps).

Coordination-class observation only (methodology §6 — profiles are clues,
not decomposition): both ladders' samples are dominated by kernel-side
waiting/waking (`futex` leaf ≈ 12–15% of samples, plus scheduler and
cross-CPU IPI frames); the data path itself (`copy_page_to_iter` /
`_copy_to_iter`) is the other visible class. PMU-disable MSR frames are
perf's own overhead. No internal attribution of the L2−L1 increment is
claimed from these captures.

## bpftrace counts (system-call and scheduler accounting)

One 1-rep invocation per ladder on the same cell (262,144 logical ops;
`-c` child accounting includes ambient system noise):

| counter | L1_pool | L2_sluice |
|---|---:|---:|
| `sys_enter_pread64` | 262,742 | 262,818 |
| `sched_switch` | 1,076,083 | 1,077,188 |
| `sched_migrate_task` | 6,009 | 6,317 |

Reading (bounded by this one cell): both ladders issue the exact expected
`pread64` count (same work at the syscall level); both pay ≈ 4.1 thread
switches per op on this host — the large L1−L0 pool tax here is a
wake/handoff phenomenon, not extra syscalls — and L2 adds only ≈ +0.4%
switches over L1, consistent with the small L2−L1 increment at this cell
in the canonical artifacts.
