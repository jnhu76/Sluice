# M2 protocol: native-Linux H1/H2 validation

Status: protocol executed; results in `h1-results.md`, `h2-results.md`,
`cpu-attribution.md` and `end-to-end-ab.md`.
Date: 2026-10-21.

## 1. Scope

M2 independently validates two mechanisms proposed by the M0/M1 study on a
native-Linux host:

- **H1** — encode the router slot in the io_uring operation identity so CQE
  routing is a direct index instead of a reverse scan.
- **H2a** — replace the uring owed-event sweep's per-slot lock/unlock pairs with
  one bounded snapshot under a single lock.
- **H2b** — maintain a derived owed-event count and skip the owed scan when the
  count is zero.

M2 does not change product `src/` or `include/`, does not change CI, and does
not merge either research branch. Its PR is research-only and stays open.

Workload scope: the Layer C campaign exercises the W-02 synchronous-wait
shape only (`submit_read` → `wait_for` → `take`). **W03_AB = NOT_RUN** — the
notification/acknowledge path and host-owned external event loops were not
driven, so all confirmed conclusions are W-02-scoped and W-03 admission
remains a productization gate (`h2-results.md` §6).

## 2. Dependency state at execution time

| Item | State |
|---|---|
| `master` | `8fdcd99c5f9a42dadfe5aa0c095bf96c756aeb01` |
| PR #560 | **OPEN**, head `31446946c25de65ae93711071e52d94d2a8867c2`, branch `research/m0m1-state-complexity` |
| Issue #559 | OPEN |
| #560 owner review | `COMMENTED` / **REVISE** at the same head: P1 (H1 identity dataflow incomplete) and P2 (H2 conditional complexity misstated) |
| M2 source baseline | `8fdcd99c` (master); the A/B worktrees are unmodified source plus one research patch |
| M2 delivery branch | stacked on `31446946` so the PR depends on #560 without modifying it |

The #560 review's P1 and P2 are addressed *inside M2* by
`h1-identity-matrix.md` and by the H2a/H2b split and parity test. They are not
silently applied to #560: #560 remains at its frozen head and stays open. The
M2 campaign itself was requested separately by the owner; it is not presented as
the #560 correction.

Post-campaign closeout: the P1/P2 corrections were backfilled into #560 at
`2e998fba` with a review disposition, and #560 was merged; this PR was then
retargeted to `master` so its diff is M2-only. The owner review of this PR
(review `5599795090`) and its dispositions are recorded in `review.md`.

## 3. Corrected mechanism statements

### H1

Baseline: the operation SQE `user_data`, `RouterEntry.cookie`,
`TransportLedger::Entry.cookie`, the `io_uring_prep_cancel64` target, the control
SQE/CQE payload, consumed-control lookup, and poison/Class-A recovery all carry
the same flat cookie. `find_live_router_cookie_` reverse-scans live router
entries and compares the full 64-bit value.

Candidate: identity = `(router_slot << 48) | cookie48`, with bit 63 reserved for
the control tag. Every consumer decodes the slot and then re-checks the full
identity plus the entry's `in_use`/handle/control state. The complete matrix is
`h1-identity-matrix.md`. The design deliberately spends domain width:

- `request_capacity` becomes `[1, 32768]`;
- the cookie horizon becomes `2^48 - 1`, with fail-fast termination before wrap.

These are product-domain changes, not free implementation details. M2 measures
the gain but does not authorize the narrowing.

### H2

Baseline `poll()` scans all `C` delivery slots every pass. For each slot it takes
and releases `dispatch_mtx_` (a per-slot lock pair), checks `event_owed`, clears
it, and collects the owed key. In the W-02 read workload `event_owed` is set only
by `publish_zero_op_inline` (zero-length operations), which normal reads never
call. The sweep is therefore pure per-pass overhead in this workload: `C` lock
pairs and `C` slot inspections with nothing to deliver.

H2a replaces the per-slot locking with one bounded snapshot: one lock, one scan,
preallocated `owed_batch_`, then delivery outside the lock. It is still `O(C)`
per pass, but with one lock pair and no allocation.

H2b adds a derived `owed_count_`. `poll_progress()` uses it only to decide
whether immediate work remains; `poll()` snapshots it under the same lock and
skips the entire scan when it is zero. When non-zero, H2b still scans `C` slots
with the baseline per-slot locks. It is therefore:

- `O(1)` when the count is zero (the W-02 case);
- `O(C)` scan plus `O(K)` delivery when `K > 0` owed events exist.

H2b is explicitly **not** an `O(K)` owed list. Its counter is a derived summary,
not settlement/shutdown/destruction authority: the destructor and
`internal_work_retired()` keep the authoritative `any_event_owed_locked_()`
scan. `poll_progress()`'s early return is a progress-pass optimization. This is
the BACKEND-04 boundary.

## 4. Frozen inputs

| Input | Value |
|---|---|
| Source baseline | `8fdcd99c5f9a42dadfe5aa0c095bf96c756aeb01` |
| `h1.patch` SHA-256 | `c9d6e86ec2abc13394d969d82be287779b9f958be26bc783ad92889187cca999` |
| `h2a.patch` SHA-256 | `f31f4e0b8618089d0e1906160608b329656269ced7744988033e840fecc94d8a` |
| `h2b.patch` SHA-256 | `e36da40f00aae9003f4719e0973eab433d2bed042e805e1de16c8e66682b4c0e` |
| `h2ab.patch` SHA-256 | `65fef1ef67a9aaf816aba740303f76cd67289fb60fa2247bd1a98656c37ea91c` |
| Harness source | `pipeline_ab.cpp`, SHA-256 `6aa9e30c2b98d53c08a8a0843083b49a80b88f9a6e979a3c9737d60c32645223` |
| Primary runner | `run_ab.sh` (frozen before the campaign), SHA-256 `23066988580eb845f348e3e7ddc545d22694db0e814726673a9a8b242eacffd4`, retained at `results/ab-15/run_ab.frozen.sh` |
| Replication runner | `replicate.sh`, retained at `results/ab-clean/replicate.sh` |
| Committed runner | `run.sh`, superset with provenance and self-validation; measurement loop unchanged |
| Compiler | g++ 16.2.1, `-std=c++20 -O3 -DNDEBUG` |
| Link | `libsluice_async.a`, `libsluice_core.a`, `-luring -lpthread` |
| Fixture | 64 MiB `fill_pattern` file on tmpfs |

Per-arm binary and static-library SHA-256 values are in
`results/ab-15/arm-identity.txt` and `results/ab-clean/arm-identity.txt`.

## 5. Correctness gate (before any timing claim)

Each arm's worktree ran the existing deterministic suite (`xmake test`) on E5:

| Arm | Tests | Result |
|---|---|---|
| base | 74 | 74 passed |
| h1 | 75 | 75 passed |
| h2a | 74 | 74 passed |
| h2b | 75 | 75 passed |
| h2ab | 75 | 75 passed |

The suites include RequestCore/ThreadPool/io_uring conformance, cancellation,
owed delivery, observer delivery, progress notification, shutdown, request
lifetime, borrow retirement and mutation gates. No sleep, retry inflation or
weakened assertion was introduced.

Research-only negative controls:

- H1 identity envelope test: seven controls (round-trip/envelope, capacity
  boundary, forged slot, retired-slot reuse, wrong control target, control CQE
  without state, cookie exhaustion). All pass. A slot-only lookup mutant
  (`entry.in_use && entry.cookie == identity` reduced to `entry.in_use`) fails
  exactly `stale_cookie_cannot_reach_a_reused_slot` (20/21), so the integration
  test distinguishes the full-identity re-check from a slot-only lookup.
- H2 parity test: zero-op tracking, data-op immunity, slot-reuse parity,
  concurrent-producer exactly-once. The compile-time
  `uring_owed_counter_parity_mut_no_decrement` mutant fails the first control
  (`FAIL: counter_tracks_zero_op_owed_events`), so the test distinguishes the
  mechanism from a no-decrement implementation.

Any semantic failure would have produced `CORRECTNESS_REJECTED` and barred that
arm from performance admission. None occurred.

## 6. Measurement protocol

Three independent layers, no layer substituted for another:

1. **Layer A (mechanism microbenchmark).** `bench/mechanisms/run_mechanisms.sh`
   on E5: capacities 8-1024, lookup/churn/miss, owed idle/sparse/half/full,
   H3 control. 15 repetitions, warmup, interleaved/rotated arms, independent
   oracle per arm; negative controls must be caught; oracle-failing arms produce
   no performance result. Used to establish mechanism-level direction and
   scaling, not production speedup.
2. **Layer B (production-path perf).** `perf stat` with unmultiplexed
   user-space event groups and `perf record -m 64` (default ring starves
   io_uring's memlock) on the real `RequestScope` pipeline. Samples map to
   executable/build-id/source symbols; kernel symbols are unavailable
   (`paranoid=2`). Perf shares are never converted into wall-clock speedup
   claims.
3. **Layer C (real production A/B).** The same research harness compiled against
   each arm's static library, driven through `RequestScope` over the real
   `UringAsyncBackend`. Workload matrix: 4 KiB/64 KiB/1 MiB, capacities
   16/64/256/1024, window depth 1/4/8. Paired within each repetition; arm order
   rotated by repetition; `taskset -c 2`; warm tmpfs fixture; oracle in every
   repetition; zero timed allocations enforced.

Primary campaign: 20 configurations × 5 arms × 15 repetitions (4500 accepted
rows). Idle-window replication: 7 decisive configurations × 5 arms × 15
repetitions (1575 accepted rows). Both passed the independent validator with
zero oracle failures and zero timed allocations.

No absolute ns/op is compared between E5 and WSL2. The WSL2 M1 evidence is used
only for sign/magnitude context, with its own measurement caveats.

## 7. Decision rule

Applied per mechanism, using the paired CPU-time ratio (the primary quantity)
and the wall ratio/p99 as supporting observations:

- `CORRECTNESS_REJECTED`: any correctness or oracle failure.
- `PROTOCOL_DESIGN_REQUIRED`: the mechanism cannot preserve the v2 identity
  semantics without an owner/root decision.
- `MECHANISM_IMPROVEMENT_CONFIRMED`: median paired ratio ≤ 0.95, its 95%
  bootstrap CI wholly below 1.0, the effect reproduced in both campaigns, and
  the perf profile attributing the gain to the intended mechanism.
- `MICROBENCH_ONLY`: a repeatable mechanism-level gain that does not meet the
  end-to-end materiality threshold.
- `PERFORMANCE_NOT_DISTINGUISHED`: no repeatable material end-to-end effect.
- `KEEP_BASELINE`: no gain, or a gain not worth its semantic/mechanism cost.

The 5% materiality threshold is an operational choice for M2, not a
pre-registered industry constant. It is stated so readers can re-adjudicate with
a different threshold. H2b additionally had to beat H2a repeatably; otherwise
the simpler H2a would be preferred.

## 8. Threats to validity

- Perf is user-space only; kernel submission/completion time cannot be
  symbol-attributed. The A/B wall and CPU ratios remain valid, but perf shares
  are not a wall-clock decomposition.
- The primary campaign ran with background load (median loadavg 2.48,
  including the owner's `parquet_bench`); the replication ran at median 1.35.
  The paired/rotated design and the replication bound, but do not eliminate,
  this threat.
- CPU 2's SMT sibling (CPU 12) was not cpuset-isolated.
- The fixture is tmpfs-backed, so the workload is cache-warm and does not test
  block-device behavior.
- The psuedo-`O(1)` H2b skip depends on the derived counter being exact. The
  parity test and the retained authoritative teardown scan are the evidence;
  a future owed-event list would need its own protocol matrix.
