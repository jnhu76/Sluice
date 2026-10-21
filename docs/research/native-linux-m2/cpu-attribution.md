# Production-path CPU attribution (E5, user-space perf)

Scope: attribute the measured A/B effect to source-level work on the real
`RequestScope → UringAsyncBackend` path. This layer explains the A/B; it is not
an independent speedup claim and its shares are never converted into wall-clock
percentages.

## 1. Method

- Host: E5, Xeon E5-2666 v3, Fedora 44, kernel 7.2.9, g++ 16.2.1, `-O3 -DNDEBUG`.
- Binaries: the exact A/B binaries (`pipeline_ab_{base,h1,h2a,h2b}`), SHA-256 in
  `results/ab-15/arm-identity.txt`.
- `perf stat`, 5 repetitions per arm/config, interleaved; 4 hardware events
  (`cycles:u,instructions:u,branches:u,branch-misses:u`) with `task-clock:u`,
  and a separate 2-event cache group. Both groups fit the 4 programmable
  counters, so the counts are not multiplexed.
- `perf record -e cycles:u -F 997 -m 64 -g --call-graph dwarf,16384`.
  `-m 64` is required in this environment: with the default perf ring,
  `io_uring_queue_init()` fails with `-ENOMEM`, and a minimal
  `io_uring_queue_init(1)` program reproduces the failure under the default
  ring and succeeds with `-m 1`. The recorded limits are
  `perf_event_mlock_kb=516` and `RLIMIT_MEMLOCK=8192 KiB`; the exact accounting
  interaction is consistent with a shared locked-memory budget but was not
  proven, so this is reported as an empirical workaround.
- Pinned with `taskset -c 2`. CPU 2's SMT sibling is CPU 12; no cpuset
  isolation. `loadavg(1)` was 0.22-0.94 across the stat and record windows.
- Perf record covers all four harness phases (warmup, timed, oracle, latency),
  each with the same request count. Shares describe the whole process and are
  compared only between arms.

## 2. Counter totals

4 KiB block, capacity 1024, window depth 1, 50,000 requests per phase
(200,000 operations total); medians of 5 repetitions:

| Arm | task-clock | cycles/op | instructions/op | IPC | branch-miss | cache-miss | user s | sys s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| base | 3.57 s | 41.30 k | 116.46 k | 2.82 | 0.056% | 0.057% | 2.468 | 1.107 |
| h1 | 3.43 s | 39.22 k | 116.72 k | 2.98 | 0.050% | n/a | 2.346 | 1.088 |
| h2a | 2.13 s | 16.13 k | 45.41 k | 2.82 | 0.116% | 0.049% | 1.017 | 1.117 |
| h2b | 1.79 s | 11.22 k | 30.62 k | 2.73 | 0.175% | 0.119% | 0.735 | 1.049 |

H1's gain is about 2.1 k cycles/op (5%) with essentially unchanged
instructions/op. In this depth-1 workload only one operation is live, so the
baseline reverse scan inspects mostly `in_use == false` entries and is short;
the H1 gain is microarchitectural (fewer dependent loads/branch work), not an
instruction-count reduction. This also explains why H1's production effect is
much smaller than its isolated Layer A lookup ratio.

Branches per operation and cache references are dominated by the four phases
(including the oracle's `memcmp`); they are not used as mechanism evidence. The
instruction and cycle reductions are the signal:

- baseline → H2a: -59.5 k instructions/op (-51%) and -25.2 k cycles/op;
- baseline → H2b: -85.8 k instructions/op (-74%) and -30.1 k cycles/op;
- H2b removes about 86 k instructions/op, the per-pass `C=1024` sweep.

The user/system split is diagnostic. At C=1024/d=1 the baseline is 2.468 s user
and 1.107 s system; H2b is 0.735 s user and 1.049 s system. H2b removes
user-space sweep work but leaves kernel submission/completion time essentially
unchanged. After H2b, system time is 59% of the total at this configuration, so
the remaining wall-time floor is the kernel path, not another user-space loop.

At 4 KiB, C=64, d=8 the baseline is 0.258 s user + 0.549 s system (0.807 s);
H2b is 0.251 s + 0.500 s (0.751 s). The user-space sweep is already small
relative to kernel work, which is why the end-to-end gain at C=64 is only a few
percent.

## 3. Symbol attribution

`perf record` self shares, 4 KiB/C=1024/d=1:

| Symbol | Base | H1 | H2a | H2b |
|---|---:|---:|---:|---:|
| `pthread_mutex_lock` + unlock variants | 51.4% | 54.7% | 5.3% | 8.0% |
| `UringAsyncBackend::poll()` self | 20.18% | 16.92% | 17.89% | 0.84% |
| `poll_progress()` | 4.42% | 4.09% | 13.29% | 0.51% |
| `service_pending_controls_locked_()` | 8.81% | 8.07% | 22.68% | 32.50% |
| `dispatch_after_accept()` | 7.50% | 8.62% | 20.28% | 28.46% |
| `dispatch_one_locked()` | 0.42% | 0.38% | 1.51% | 2.57% |
| `finalize_operation_terminal_()` | 0.33% | 0.56% | 1.40% | 1.62% |
| `publish_one()` | 0.28% | n/r | 0.47% | 1.07% |
| `reap_cqes()` | 0.22% | n/r | 0.52% | 0.95% |

`n/r` = not in the top-18 rows of that profile.
Sample counts: base 3600, H1 3397, H2a 2112, H2b 1780; lost samples: 0 for all
four. The H2b profile is therefore coarse; only top-level shares are used. The
H2b `poll()` self share (0.84%, plus a small `poll .part.0`) shows that the sweep
is gone. H2a's single-lock scan appears as `poll_progress` at 13.29%; H2b
reduces it to 0.51%.

`service_pending_controls_locked_()` and `dispatch_after_accept()` are real
submission-side work. Their share rises after H2 because the denominator shrinks,
not because their absolute cost grows. They are the next plausible optimization
targets, but M2 did not test a mechanism for them.

## 4. Source mapping

The sampled symbols map to the following current sources:

| Symbol | Source |
|---|---|
| `UringAsyncBackend::poll` | `src/async/uring_backend.cpp` (owed sweep and publication) |
| `UringAsyncBackend::poll_progress` | `src/async/uring_backend.cpp` (pass verdict) |
| `service_pending_controls_locked_` | `src/async/uring_backend.cpp` |
| `dispatch_after_accept`, `dispatch_one_locked` | `src/async/uring_backend.cpp`, `src/async/uring_transport.cpp` |
| `publish_one`, `publish_zero_op_inline` | `src/async/uring_backend.cpp` |
| `finalize_operation_terminal_`, `reap_cqes` | `src/async/uring_completion.cpp` |
| `account_transport_result_locked` | `src/async/uring_transport.cpp` |
| `pthread_mutex_*`, `memcmp`, `memcpy` | glibc / libc |
| `io_uring_*` | liburing 2.13 |

Build IDs from `perf buildid-list`:

```
386ffd85bf766c79edf956863264f2f896e15b68  pipeline_ab_base
1d7447fd76e9fad15da5542f6246c3938b856804  /usr/lib64/liburing.so.2.13
85075e2ae2e13352a1413d41c327dacf6461526c  /usr/lib64/libc.so.6
19730e8a5f72329cd19c291b10a0ee4275294ed5  /usr/lib64/libstdc++.so.6.0.36
```

## 5. Limits

- **No kernel attribution.** `perf_event_paranoid=2` and `dmesg_restrict=1`
  restrict events to user space and hide kernel symbols/tracepoints. The
  `sys` time above comes from `perf stat`'s process accounting, not from kernel
  samples. No kernel CPU is labelled `[unknown]` or otherwise guessed.
- **Call-graph fidelity.** `dwarf,16384` unwinding on an `-O3` binary without
  `-g` is approximate. Individual unresolved addresses are below 2%, but the
  total unresolved share in the H2b profile is 3.1% (0xa8c alone is 1.64%).
  They are left unresolved rather than assigned to a function.
- **Phase mixing.** Perf record includes warmup, timed, oracle and latency
  phases. The oracle's `memcmp` contributes to `__memcmp_avx2_movbe`; the
  comparison between arms is still apples-to-apples.
- **Sampling resolution.** 1K-3K samples at 997 Hz over 2-4 s. This supports the
  top-level attribution above, not fine-grained line-level claims.
- **No wall-clock inference.** The perf shares explain *where* cycles go. The
  performance claim comes from the paired A/B in `end-to-end-ab.md`.
- **SMT.** CPU 12 was not isolated; background work could share the physical
  core. The paired design and the low load during these runs bound this.
