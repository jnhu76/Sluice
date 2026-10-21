# H1 results: io_uring router direct index

Decision: **MICROBENCH_ONLY — do not productize**.
The mechanism makes the router lookup itself 6-17× cheaper at high capacity, but
the end-to-end production-path effect is at most 3.7% and does not meet the 5%
materiality threshold. The candidate also narrows two product domains
(`request_capacity` to 32768 and the cookie horizon to `2^48`), which requires an
owner/root decision unrelated to the measured gain.

## 1. Mechanism and protocol admission

The complete identity dataflow matrix is in `h1-identity-matrix.md`. It covers
the operation SQE, `RouterEntry.cookie`, both transport-ledger entry kinds, the
`io_uring_prep_cancel64` target, the control SQE/CQE, the operation CQE,
consumed-control lookup, poison/Class-A recovery, stale completions, slot reuse,
cookie exhaustion, shutdown and test seams. Every consumer decodes the slot and
then re-checks the full identity, so a slot hit alone is never accepted.

Seven research-only controls pass:

1. identity round-trip and the `slot=32767, cookie=2^48-1` envelope;
2. capacity 32768 accepted, 32769 rejected;
3. forged slot dropped, correct identity completes;
4. retired-slot identity dropped after reuse, new identity completes;
5. wrong control target does not release the live control reference;
6. control-tagged CQE without submitted control state terminates;
7. cookie exhaustion terminates before a wrapped cookie can alias.

The baseline full suite passes 74/74; the H1 worktree passes 75/75, including
`uring_identity_envelope_test`.

## 2. Layer A: mechanism microbenchmark (E5)

Median ns/op, 15 repetitions, rotated arms; all oracle checks PASS and negative
controls CAUGHT. `direct` is the decode-and-verify lookup; `scan` is the baseline
reverse scan.

| Capacity | Lookup scan | Lookup direct | Ratio | Churn scan | Churn direct | Ratio | Miss scan | Miss direct | Ratio |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 8 | 9.77 | 7.96 | 0.82× | 30.95 | 20.41 | 0.66× | 21.12 | 14.48 | 0.69× |
| 16 | 10.66 | 7.88 | 0.74× | 36.45 | 20.86 | 0.57× | 23.47 | 14.34 | 0.61× |
| 32 | 12.63 | 7.86 | 0.62× | 44.71 | 20.97 | 0.47× | 30.64 | 14.42 | 0.47× |
| 64 | 16.68 | 7.90 | 0.47× | 62.54 | 20.95 | 0.33× | 41.27 | 14.33 | 0.35× |
| 128 | 23.30 | 7.90 | 0.34× | 104.07 | 20.92 | 0.20× | 59.68 | 14.32 | 0.24× |
| 256 | 36.33 | 7.88 | 0.22× | 190.80 | 20.95 | 0.11× | 96.85 | 14.34 | 0.15× |
| 1024 | 122.90 | 7.85 | 0.06× | 481.89 | 20.97 | 0.04× | 338.09 | 14.33 | 0.04× |

The direct lookup is flat at 7.85-7.96 ns/op from capacity 8 to 1024, while the
scan grows roughly linearly. This establishes the mechanism, not a production
gain. Negative control: an unverified direct lookup accepts a stale hit after
slot reuse and is CAUGHT by the oracle. Positive control: the verified lookup on
the same sequence PASSES.

## 3. Layer C: real production A/B

Paired CPU-time ratio versus baseline; median with 95% bootstrap CI; lower is
better. Primary campaign (15 reps) and the idle-window replication (15 reps)
are shown separately.

| Workload | Primary | Replication |
|---|---:|---:|
| 4 KiB, C=16, d=1 | 0.990 [0.972, 1.000] | — |
| 4 KiB, C=16, d=4 | 0.997 [0.984, 1.006] | — |
| 4 KiB, C=16, d=8 | 1.003 [0.994, 1.013] | — |
| 4 KiB, C=64, d=1 | 0.988 [0.971, 0.995] | 0.986 [0.980, 0.991] |
| 4 KiB, C=64, d=4 | 0.987 [0.978, 0.996] | — |
| 4 KiB, C=64, d=8 | 1.001 [0.983, 1.011] | 0.981 [0.968, 0.999] |
| 4 KiB, C=256, d=1 | 0.992 [0.981, 0.994] | — |
| 4 KiB, C=256, d=4 | 0.988 [0.980, 0.993] | — |
| 4 KiB, C=256, d=8 | 0.996 [0.965, 1.005] | — |
| 4 KiB, C=1024, d=1 | 0.967 [0.952, 0.984] | 0.963 [0.943, 0.978] |
| 4 KiB, C=1024, d=4 | 0.977 [0.969, 0.988] | 0.977 [0.974, 0.992] |
| 4 KiB, C=1024, d=8 | 0.982 [0.968, 1.001] | 0.963 [0.928, 0.986] |
| 64 KiB, C=64, d=1 | 0.995 [0.986, 0.999] | — |
| 64 KiB, C=64, d=8 | 0.998 [0.994, 1.002] | 1.005 [0.994, 1.013] |
| 64 KiB, C=1024, d=1 | 0.982 [0.977, 0.996] | 0.988 [0.979, 0.998] |
| 64 KiB, C=1024, d=8 | 0.992 [0.987, 1.009] | — |
| 1 MiB, C=64, d=1 | 0.995 [0.990, 1.001] | — |
| 1 MiB, C=64, d=8 | 1.003 [0.992, 1.005] | — |
| 1 MiB, C=1024, d=1 | 0.988 [0.981, 1.001] | — |
| 1 MiB, C=1024, d=8 | 0.994 [0.987, 0.999] | — |

The largest observed effect is 3.7% (4 KiB, C=1024, d=1 or d=8 in the
replication). Every configuration's CPU ratio is above 0.95, so every effect is
below the 5% materiality threshold. The 1 MiB configurations are essentially
neutral because the per-operation copy/I/O work dominates.

All 15 repetitions in both campaigns passed the byte oracle, the op-count check
and the zero-timed-allocation check. There are no oracle-failing timing rows.

## 4. Layer B: why the end-to-end gain is small

At 4 KiB, C=1024, d=1, `perf record` self-symbol shares:

| Symbol | Base | H1 |
|---|---:|---:|
| `pthread_mutex_lock` + unlock variants | ~51.4% | ~54.7% |
| `UringAsyncBackend::poll()` self | 20.18% | 16.92% |
| `service_pending_controls_locked_` | 8.81% | 8.07% |
| `dispatch_after_accept` | 7.50% | 8.62% |
| `poll_progress` | 4.42% | 4.09% |

H1 reduces the scan-attributed `poll()` self share by about 3.3 percentage points,
but the dominant cost is the H2 per-slot mutex sweep, which H1 does not touch.
H1's lookup is a small fraction of the path once the lock traffic and dispatch
work are counted. The perf profile therefore corroborates the A/B result; it is
not used to claim a wall-clock speedup.

Full details: `cpu-attribution.md`.

## 5. Why this is not productized

1. **Materiality.** The maximum end-to-end CPU improvement is 3.7%, below the 5%
   threshold, and only at the largest capacity. At C=16-64 the effect is 0-1.4%.
2. **Semantic cost.** The packed identity requires `request_capacity <= 32768`
   and a `2^48` cookie horizon with fail-fast termination. Both are visible
   product-domain changes; the v2 root does not authorize them for an
   optimization of this size.
3. **Risk surface.** Every consumer of the identity must decode and re-verify.
   The matrix shows the edges, and the controls pass, but the change touches
   cancellation and poison/recovery, which are safety-critical.
4. **No production patch is requested.** `h1.patch` remains research-only and is
   not applied to product `src/` or `include/`.

Revisit only if a future workload makes router lookup a demonstrated hotspot at
scale, and only after the owner decides the capacity/cookie-domain narrowing.
