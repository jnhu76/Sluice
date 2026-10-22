# MW-HW-1 syscall census and kernel boundary audit (WSL2)

Method: `strace -f -c` on the harness (census only — traced wall time is never
quoted as a result; ptrace roughly doubles per-op cost), `/usr/bin/time -v`
for user/system accounting, `perf stat -e cycles:u,instructions:u` for
user-space counters (paranoid=2), `perf record -e cycles:u` for symbol
attribution. All runs pinned `taskset -c 2`, 4 phases × N requests each
(warmup + timed + oracle + latency), tmpfs fixture.

## 1. io_uring census (backend=uring, 80,000 timed+warmup+oracle+latency ops)

| Config | io_uring_enter | enter/op | poll(2) | read(eventfd) | futex | total |
|---|---:|---:|---:|---:|---:|---:|
| 4 KiB, D=1, C=1024 | 80,004 | **1.000** | 0 | 6 | 2 | 80,261 |
| 4 KiB, D=8, C=1024 | 80,004 | **1.000** | 0 | 6 | 2 | 80,262 |
| 4 KiB, D=64, C=1024 | 80,004 | **1.000** | 0 | 6 | 2 | 80,258 |
| 64 KiB, D=8, C=1024 | 8,004 | **1.000** | 0 | 6 | 2 | 8,264 |
| 0 B (zero-op), D=1 | **0** | **0.000** | 0 | 6 | 2 | 176 |
| E1 arm, 4 KiB, D=1 | 80,004 | **1.000** | 0 | 6 | 2 | 80,261 |

Findings:

1. **Exactly one `io_uring_enter` per operation at every sliding-window
   depth.** The harness refills the window one request per completion, so
   `dispatch_after_accept` always submits exactly one SQE; the initial burst
   of D submits is amortized to nothing over thousands of requests. There is
   no per-op `poll(2)`, no per-op eventfd read, and no futex traffic — the
   park path is cold because warm-tmpfs completions land before/inside the
   first progress pass and the physical probe catches the rest.
2. **Kernel-side per-op work** = submit-path dispatch + the tmpfs read + CQE
   post + eventfd signal (registered) + scheduler accounting. This is the
   `system` share below; function-level kernel attribution is unavailable
   (`KERNEL_ATTRIBUTION_UNAVAILABLE`: paranoid=2, kptr_restrict=1, no
   tracepoints).
3. Zero-op requests never enter the kernel at all: the owed pipeline is pure
   userspace (H2b's counter made the empty sweep O(1); E1 does not touch it).

## 2. ThreadPool census (contrast, 20,000 ops)

| Syscall | calls | per op | share of traced time |
|---|---:|---:|---:|
| pread64 | 20,004 | 1.00 | 12.8% |
| futex | 46,246 | 2.31 | **79.1%** |
| poll | 2,116 | 0.11 | 1.3% |
| eventfd2 | 4 | — | — |

The worker-handoff condvar chain (submit notify → worker wake → completion
signal) costs ~2.3 futex round trips per operation — the structural reason
io_uring (1 enter/op) wins on this host, independent of any Sluice-side work.

## 3. CPU split (time -v, 4 KiB, D=1, C=1024, 80k ops)

| Arm | user s | system s | involuntary ctx switches |
|---|---:|---:|---:|
| base | 0.22 | 0.57 | 79,958 (~1/op) |
| e1 | 0.07–0.10 | 0.65–0.66 | ~79,950 (~1/op) |

E1 removes userspace scan work only; system time and the ~1/op involuntary
switch pattern (WSL2 scheduler behavior on the enter/completion path) are
unchanged. After E1, userspace is ~12–15% of total CPU at this config; the
remaining floor is kernel-side: 1 enter + inline tmpfs read + CQE + eventfd
signal per op.

`perf stat` user-space counters (base, 3 runs): 3.07–3.08 giga-instructions
total ⇒ **38.5k user instructions/op**, IPC 3.13–3.18.

## 4. User-space symbol attribution (perf record cycles:u)

Base arm, 4 KiB/D=1/C=1024 (`perf-base-g.data`, ~3k samples):

| Symbol (resolved via nm) | share |
|---|---:|
| `service_pending_controls_locked_` (outlined copy, called from `poll()`) | 30.0% |
| `dispatch_after_accept` +506..+545 (inlined scan/drain region) | 11.4% |
| `[vdso]` clock_gettime (harness deadline + park math) | 4.2% |
| `dispatch_one_locked` +197 | 1.8% |
| `reap_cqes` +63 | 0.5% |
| kernel `[unknown]` leakage under cycles:u (WSL2 anomaly) | ~5.9% |

E1 arm (`perf-e1-g.data`): `service_pending_controls_locked_` and the
`dispatch_after_accept` scan region vanish from the top symbols; the largest
user symbol becomes `dispatch_one_locked`+197 (2.1%) — SQE preparation, i.e.
real dispatch work. This matches the C-sweep: user wall/op at C=16 is 8.4 µs
vs 9.7 µs at C=1024 (Δ ≈ +1.3 µs/op, the C-dependent component E1 deletes;
paired A/B measured the saving at 1.4–1.8 µs/op).

## 5. Ineffective-interaction verdicts (§5.3 of the charter)

| Question | Verdict | Evidence |
|---|---|---|
| Per-request independent enter? | Yes, exactly 1/op; irreducible below 1 without submit+wait combination or SQPOLL (config/root change, out of scope) — the sliding-window steady state never batches | census, all depths |
| SQEs submittable in batch but split? | No: at most 1 SQE pending per submit call in this shape; batch-flush would not reduce enter/op | census at D=1/8/64 |
| Progress passes re-querying kernel with nothing new? | The pass does NOT enter the kernel in steady state (peek = mmap reads); `has_immediate_physical_work` is 2 ring reads. No avoidable kernel call found in the pass | census (0 extra enters) |
| Excess notification signal/ack? | Kernel writes the eventfd per CQE (registration); in the inline-completion regime nobody consumes it (counter accumulates). Lazy/conditional registration is a W-03-visible contract change (park wakeups) — not proposed; kernel-side cost not attributable on WSL2 | design analysis |
| Wakeup then no useful work? | Not observed: park path is cold; probe-suppressed parks return with work | read=6 total |
| ThreadPool spurious cv wakes? | ~2.3 futex/op is the handoff chain, not spurious; reducing it means changing the worker model | census |
| Calls while queues/CQ known empty? | **Yes — the pending-control scan (E1 target): full router scan at P=0, per submit and per pass. Fixed in E1 arm with identical syscall census** | E1 census identical to base |

## 6. Honesty limits

- strace inflates wall ~2x; counts only.
- The involuntary ~1 ctx-switch/op pattern is a WSL2 scheduler artifact;
  native Linux may not show it. No E5/WSL2 wall comparison is made.
- `cycles:u` includes some `[unknown] [k]` leakage on WSL2 (~6%); those
  samples are excluded from all claims.
- `perf record` covers all four harness phases; shares compare arms under
  identical phase mixes.
