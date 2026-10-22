# E1 Native-Linux Validation — CPU Attribution (Gate R6)

Question: of the remaining CPU cost after E1, what is user space vs kernel,
and does the E1 gain come from removing user-space redundant work or from
changing kernel behavior?

Evidence: `perf stat -x,` with `:u` (user-space) counters — every run
**100.00 time-enabled, no multiplexing** (`~/sluice-e1-e5/results/perf-*.txt`);
strace syscall census (`strace-*.txt`); instrumented scan counters
(`mechanism-scans.txt`, research-only MECH trees, never used for timing).

## Mechanism instrumentation (scans/op)

| Config | BASE service calls | BASE scan iterations/op | E1 scans | E1 skip rate |
|---|---:|---:|---:|---:|
| 4096-C16-D1 | 9.37/op | 150/op | 0 | 100% |
| 4096-C64-D1 | 9.30/op | 595/op | 0 | 100% |
| 4096-C256-D1 | 8.94/op | 2,288/op | 0 | 100% |
| 4096-C1024-D1 | 8.46/op | **8,666/op** | 0 | 100% |
| 4096-C1024-D8 | 4.70/op | **4,814/op** | 0 | 100% |
| 4096-C1024-D64 | 4.09/op | 4,192/op | 0 | 100% |
| 65536-C1024-D8 | 5.05/op | 5,167/op | 0 | 100% |
| 1048576-C64-D8 | 5.27/op | 337/op | 0 | 100% |

E1 skips 100% of service calls in every P=0 workload (`skips == calls`,
`scans = 0`, `iters = 0` on all 11 configs); BASE iterates exactly
`calls × capacity`. At the headline config the removed work is
~173M router iterations per 20K-op run.

## perf stat (`:u` events; paranoid=2 ⇒ kernel-excluding HW counts, labeled as such)

| Config | instructions/op base→e1 (ratio) | cycles/op base→e1 (ratio) | branches ratio |
|---|---|---|---|
| 4096-C1024-D1 | 120,665 → 23,233 (**0.193**) | 44,437 → 15,424 (0.347) | 0.248 |
| 4096-C1024-D8 | 70,246 → 16,034 (0.228) | 25,817 → 10,112 (0.392) | 0.278 |
| 4096-C16-D1 (control) | 25,270 → 23,111 (0.915) | 16,122 → 15,868 (0.984) | 0.932 |
| 1048576-C64-D8 (control) | 168,245 → 164,385 (0.977) | 190,524 → 189,705 (0.996) | 0.968 |

Cross-check: the removed instructions at C1024/D1 ≈ 97,432/op ≈ 8,666 scan
iterations × ~11.2 instructions per iteration — the instruction reduction is
accounted for, almost entirely, by the eliminated scan loop. End-to-end CPU
ratio (0.693) is much weaker than the instructions ratio (0.193): the scan
was a large user-space chunk of a wall profile dominated by syscalls and
kernel time, exactly the shape the WSL2 study predicted qualitatively.

## Kernel boundary (strace census, strace time not used as perf time)

| Config | io_uring_enter BASE | io_uring_enter E1 |
|---|---:|---:|
| 4096-C1024-D1 (20K ops, warmup+timed+oracle+latency = 80K ops) | 80,004 | 80,004 |
| 4096-C16-D1 | 80,004 | 80,004 |

Kernel entry counts are byte-identical between arms on both configs
(1 enter/op, plus 4 `eventfd2` at context setup); `futex` traffic ≈ 0
(single-threaded inline loop). Kernel behavior is unchanged by E1; the
entire gain is user-space redundant-work elimination. The rusage split
(rep-paired medians over the 15 pairs, `ab.jsonl`
`utime_ns_per_op`/`stime_ns_per_op`): at C1024/D1 utime ratio **0.458**,
stime ratio **0.858**; at C1024/D8 utime ratio **0.389**, stime ratio
**0.763**. The kernel-charged share drops less than user time (and the
syscall count is unchanged); the residual stime reduction is an indirect
effect of the removed user-space churn and is not further attributed here.

`perf stat` run shape (recorded here because the artifacts themselves do not
carry the command line — Review B P3): each `perf-*.txt` holds the raw
`perf stat -x,` output of ONE `perf stat` process per (config, arm), run
sequentially on pinned CPU 2, all `:u` events, every run reporting 100.00
time-enabled. The recorded totals correspond to exactly one request-count of
operations (e.g. 2,413,309,058 instructions = 20,000 × 120,665 at the
headline config; 84,122,263 = 500 × 168,245 on 1MiB) — i.e. a single
20K/500-op invocation, NOT the 4-phase 80K-op shape used for the strace
census; per-op ratios above are these totals divided by the request count.

## Where the remaining cost lives

At 4KiB/C1024/D8, E1 spends ≈ 10.1K user cycles/op plus ~1 enter/op
(syscall entry/exit + kernel submit/completion work dominates the end-to-end
profile: user task-clock is a minority share). The residual user-side cost is
request/scope plumbing and CQE processing; the kernel-side cost is the
io_uring submit/completion path itself. Neither is addressable by E1; the
next candidates are the E2/E3 MW-HW-1 arms (dispatch and publication
boundary), not the router scan.

Caveats recorded honestly: `perf stat` at paranoid=2 counts user-space
events only (kernel cycles are invisible to the HW counters — the
user/system split comes from rusage); tracefs was unreadable so no
tracepoint-level kernel breakdown; `perf record` was not needed for the
attribution and was not run beyond an availability probe.
