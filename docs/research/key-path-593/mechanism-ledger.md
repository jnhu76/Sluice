# Mechanism ledger

Per-logical-operation frequency, state, lock, byte and cycle accounting for the
frozen base `bdfd0d8d`. One logical operation = one 4 KiB positional read unless
the case name says `64k`. Two binaries are used and never mixed:

- **Full binary** — performance authority (wall, CPU, cycles, p50/p99).
- **Observe binary** — same source + test-only counters
  (`bench/research/key-path-593/patches/observe-instrumentation.diff`), runtime
  gated by `SLUICE_OBSERVE`; **frequency/attribution only**.

## 1. Instrumentation overhead

Interleaved A/B/A, 5 rounds × 7 measured reps, stats on, `taskset -c 2`.

| Case | full CPU µs/op | observe-off | Δ | observe-on | Δ |
|---|---:|---:|---:|---:|---:|
| W02-uring-4k-d16 | 2.9995 | 3.0160 | +0.56 % | 3.4374 | **+14.65 %** |
| W02-uring-4k-d1 | 5.8825 | 5.9571 | +1.30 % | 6.0759 | **+3.31 %** |
| W03-uring-4k-d16 | 3.1573 | 3.2024 | +1.48 % | 3.6386 | **+15.35 %** |

Disabled-branch cost is inside the host A/A band. Enabled counters cost
+3–15 % CPU/op, so **no cost claim in this repository is derived from the
observe binary**. Cadence is perturbed ±20 %; park incidence is not stable
under observation, so park counts are taken from the full binary.

## 2. Full-binary CPU anatomy

`perf stat` with transient `sudo` for kernel-space events, floor-subtracted,
60,000 ops per arm. `k` = kernel, `u` = user (not "thousands").

| Case | kernel cycles/op | user cycles/op | total | kernel share | IPC kernel | IPC user |
|---|---:|---:|---:|---:|---:|---:|
| W02-uring-4k-d1 | 18,754 | 3,996 | 22,750 | **82.6 %** | 0.954 | 1.477 |
| W02-uring-4k-d16 | 8,293 | 2,572 | 10,865 | **76.3 %** | 0.839 | 1.588 |
| W03-uring-4k-d16 | 8,385 | 2,868 | 11,253 | **74.5 %** | 0.715 | 1.143 |

Thread split of all cycles (`perf record`, W-02 d16): **main thread
58.4–66.3 %, kernel io-wq worker `iou-wrk-*` 32.5–40.4 %**. The two archived 0/1
records disagree by 8 percentage points (Phase C attribution run: 66.29/32.52;
K2 arm: 58.43/40.35), so the share is quoted as a range. Top symbols (Phase C
run): `rep_movs_alternative` 12.0 % (worker; 14.2 % in the K2 run), syscall
entry/exit ≈10 %, `__io_uring_submit` 5.7 %, `pthread_mutex_lock` 2.25 % +
`pthread_mutex_unlock` 1.15 % (**all** Sluice mutex traffic),
`__memcmp_avx2_movbe` 2.2 % (bench byte oracle, not Sluice),
`__memmove_avx_unaligned_erms` 1.35 %.

The mutex share is the load-bearing number for K1/K3/K4-a: 3.4 % of total
cycles ≈ **370 cycles/op** across all mutex sites, against the campaign
4.79 % materiality band (provenance in
[measurement-review.md](measurement-review.md) §4).

## 3. Syscalls per logical op

Full binary, strace census (whole run), plus undistorted tracepoints.

| Case | `io_uring_enter` | futex | `epoll_wait` | `read(eventfd)` | `poll(2)` | total (tracepoint) |
|---|---:|---:|---:|---:|---:|---:|
| W02-uring-4k-d16 | **1.00005** | 0.000004 | 0 | 0.00004 | **0** | 1.13 |
| W03-uring-4k-d16 | **1.00005** | 2.13\* | 0.063 | 0.125 | 0 | 1.605 |

\* the strace futex figure is inflated by tracer distortion (10× slowdown makes
producer and driver contend more); the undistorted tracepoint count gives
1.605 syscalls/op total. `perf sched timehist` during an untraced W-03 run
records 232 context switches per 60,000 ops (0.004/op): under normal speed the
two threads rarely block each other.

## 4. Mechanism frequency per logical op

Observe census, 540,000 ops/arm (d16-saturate 86,400; 64 KiB 54,000; see §8).

| Mechanism | W02 d1 | W02 d4 | W02 d16 | W02 d64 | W02 sat | W02 64k | W03 d4 | W03 d16 | W03 d64 | tp d16 w4 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| CQEs reaped | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | — |
| `io_uring_enter` (submit) | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | — |
| SQEs submitted / enter | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | — |
| `io_uring_sq_ready` at submit | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | — |
| CQEs / `peek_batch_cqe` | 1.00 | 3.47 | 11.76 | 28.88 | 8.12 | 13.99 | 3.19 | 9.59 | 11.83 | — |
| progress passes | 1.34 | 0.42 | 0.127 | 0.030 | 0.185 | 0.143 | 0.314 | 0.104 | 0.070 | 0.098 |
| productive passes (completed > 0) | 1.00 | 0.288 | 0.085 | 0.020 | 0.123 | 0.072 | 0.314 | 0.104 | 0.070 | 0.074 |
| empty passes | 0.34 | 0.137 | 0.042 | 0.010 | 0.061 | 0.072 | ≈0 | ≈0 | ≈0 | 0.024 |
| parks `poll(2)`\*\* | 0.34 | 0.137 | 0.042 | 0.010 | 0.061 | 0.071 | 0 | 0 | 0 | 0.024 |
| eventfd drain reads | 0.68 | 0.27 | 0.084 | 0.020 | 0.12 | 0.14 | 0.63 | 0.21 | 0.14 | 0.048 |
| user `signal_ready_progress` | **0** | 0 | 0 | 0 | 0 | 0 | ≈0 | ≈0 | ≈0 | **1.0** |
| kernel eventfd notifications | — | — | 0 | — | — | — | 0.31 | 0.084 | 0.067 | — |
| `dispatch_mtx_` acquisitions | 7.4 | 3.7 | 2.5 | 2.1 | 2.7 | 2.6 | 3.6 | 2.5 | 2.3 | — |
| `RequestCore::mutex_` acquisitions | 14.7 | 12.8 | 12.4 | 12.1 | 12.6 | 12.4 | 12.7 | 12.3 | 12.2 | 12.3 |
| `access_mtx_` acquisitions | 5.0 | 2.2 | 1.3 | 1.1 | 1.5 | 1.3 | 1.63 | 1.21 | 1.14 | — |
| router lookups / CQE | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | — |
| router probes / lookup | 1.0 | 2.5 | 8.5 | 32.1 | 8.5 | 8.5 | 2.5 | 8.5 | 32.4 | — |
| ledger append / pop | 1 / 1 | 1 / 1 | 1 / 1 | 1 / 1 | 1 / 1 | 1 / 1 | 1 / 1 | 1 / 1 | 1 / 1 | 1 / 1 |
| `service_pending_controls` entries | 2.34 | 1.42 | 1.13 | 1.03 | 1.18 | 1.14 | 1.31 | 1.10 | 1.07 | — |
| actual control scans | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | — |
| overflow `io_uring_get_events` | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | — |
| ledger reconciles | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | — |
| `Request::ready()` core lookups | 2.0 | 1.29 | 1.09 | 1.02 | 1.12 | 1.07 | 1.04 | 1.03 | 1.01 | 1.07 |

Wait/park and user-signal rows are the dormant-wait evidence: W-02 pays zero
park syscalls; W-03's kernel eventfd aggregation yields 0.084 notifications/op
at d16 (≈12 operations per wake) and `empty_wakes = 0.0000` in every uring arm.
The threadpool contrast is exactly 1.0 user-space signal/op — a different wake
economy, not a defect.

\*\* **The `parks` and `empty passes` rows in this table are observe-binary
values and are perturbed by instrumentation.** The full binary records **0
parks** for W-02 (24 eventfd reads and 0 `poll(2)` in 540,000 ops), while the
observe build reports 0.042/op at d16. The `parks` row must not be read as the
W-02 park rate. The W-03 zero rows are consistent between the two binaries.

The `productive passes` row is `completed > 0` — a **return-value** criterion,
not the obligation-based definition brief §6 requires. It over-counts
productive passes (a pass that serves only control/retry/publication/owed work
is counted empty) and under-counts empty ones. In these arms there were no such
obligations, so the split coincides with the load's only progress source; the
label is a census artifact, not a semantic definition.

## 5. Fixed per-operation chains

**Submit chain** (identical at every D, uring):

```
ctx entry
  -> [access_mtx_] validate public preconditions
  -> [core mutex] reserve
  -> prepared_ops_[slot] write
  -> [dispatch_mtx_] delivery_ init
  -> [core mutex] accept
  -> [dispatch_mtx_] dispatch push, dispatch_one (claim_execution, SQE prep, router install,
     ledger append, remove_exact), submit_transport (1 enter), ledger pop
```

**Completion chain** (identical at every D):

```
handle_one_cqe [dispatch_mtx_]
  -> offer_terminal [core]
  -> release_execution [core]
  -> publication push [dispatch_mtx_]
  -> publication pop [dispatch_mtx_] -> publish_one
  -> begin_publication [core], kind read (unlocked, pinned), complete_publication [core]
  -> claim_observer_delivery [core] -> on_ready (lock-free, empty sink) -> retire_observer_delivery [core]
  -> take -> consume_public_result [core] -> try_reclaim_
```

Core mutex per op = reserve 1 + accept 1 + claim_execution 1 + offer_terminal 1
+ release_execution 1 + begin_publication 1 + complete_publication 1 +
claim_observer 1 + retire_observer 1 + consume 1 = **10 fixed**, plus
`outstanding_summary` (1 + passes/op), `health_failed` (passes/op) and
ready-lookups (above). Measurement: 12.1–12.8 acquisitions/op, consistent.

## 6. Bytes and state

| Item | Per op | Lifetime |
|---|---|---|
| Payload read | 4,096 B (65,536 B in the 64k case), byte-oracle verified | per request |
| `PreparedUringOp` | 48 B write | submit → terminal finalize (the completion side classifies byte-op/cancel from it) |
| `DeliveryRecord` | kind/event_owed/owed_key | submit → delivery |
| `RequestDescriptor` | 24 B | accept → publication payload |
| Slot state | ~176 B, ~10 fields | reserve → consume; generation advances on reuse |
| `RouterEntry` | 32 B | dispatch → CQE route/retirement |
| `TransportLedger::Entry` | 40 B | append → consumed reconciliation |
| SQE | 64 B | prep → kernel |
| Approximate structural writes | ~250–300 B/op | — |

The four slot-indexed arrays (`prepared_ops_`, `delivery_`, router, ledger)
encode overlapping *fields* but different *lifetimes*. No block is removable on
field-similarity grounds; the mapping checked each reader.

## 7. Derived views

| Quantity | Formula | Value (W-02 d16) |
|---|---|---|
| Kernel share | kernel / total cycles | 76.3 % |
| All-mutex share | lock+unlock symbols / total cycles | 3.4 % ≈ 370 cycles/op |
| Acquisitions/op | dispatch 2.5 + core 12.4 + access 1.3 (+ progress) | ≈16–18 |
| Cycles per acquisition (uncontended) | 370 / ≈18 | ≈20 cycles |
| CQEs/peek | 1 / (peek calls/op) | 11.76 |
| Ops per kernel wake (W-03 d16) | 1 / notifications/op | ≈12 |
| Locks per CQE (per-op = per-CQE, one CQE/op) | dispatch 2.5 + core 12.4 + access 1.3 | ≈16.2 |

The per-CQE lock count equals the per-op count in every measured form because
exactly one CQE is reaped per logical operation (census `cqe_seen = 1`). For the
completion segment alone it is 2 (`dispatch_mtx_`) + 7 (`core`) per CQE; the
remainder is submit-side and pass-amortized. Submit-side locks are constant
(6/op) at D=1/4/16/64; only the pass-amortized and reap-side counts vary with
D, which is why the per-op totals in §4 fall from 7.4 to 2.1 (`dispatch_mtx_`)
and from 14.7 to 12.1 (`core`) as D rises.

## 8. Denominators and corrections

- Census denominator: 540,000 ops = 9 records × 3 phase passes × 20,000;
  d16-saturate 86,400 (3,200 requests = `kSaturationRounds` × D); 64 KiB d16
  54,000 (2,000 requests per the frozen case list). Every ratio uses the
  record's actual `request_count`.
- **Correction (measured this campaign, arithmetic recheck).** An earlier draft
  read the `k`/`u` column headers as "thousands". They mean kernel/user, as the
  adjacent `IPC k` / `IPC u` columns show. Recomputing from the raw logs:
  `(604,791,795 − 107,235,627) / 60,000 = 8,292.6` kernel cycles/op and
  `(171,478,269 − 17,133,918) / 60,000 = 2,572.4` user cycles/op, matching the
  table. `3.4 % × 10,865 = 369` cycles/op, consistent with "~370 cycles/op".
  There is no 1000× inconsistency; the headers were ambiguous and are now
  fixed. This changes no verdict (both readings are below the threshold).
- The W-03 CPU placement is single-CPU (`cpu/wall = 1.0004`, all threads
  including `iou-wrk` observed on cpu2). An earlier cross-case mixing of
  W02-d1 (5.887 µs/op) with W03-d16 (3.247 µs/op) is retracted; it was not a
  placement contradiction.
- A census summary header in the K4-b A/B directory carries a stale "K2-a
  census medians" banner; the values are K4-b's and match its README. One pair
  runner carries the same stale banner. See
  [measurement-review.md](measurement-review.md) §9.3–§9.4.
