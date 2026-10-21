# H2 results: owed-event visibility

Decision: **H2b — MECHANISM_IMPROVEMENT_CONFIRMED** on the CPU-bound, high-capacity
configurations that motivated H2. H2a also improves the path and is the better
choice at 1 MiB/C=64/d=1, where H2b is reliably worse; on the decisive 4 KiB and
64 KiB configurations H2b is better. The combined H2a+H2b arm is usually worse
than H2b and adds no repeatable gain. The minimal product candidate is H2b alone.
M2 does not create a product PR.

## 1. What the baseline actually does

`UringAsyncBackend::poll()` scans all `C` delivery slots every pass. For each
slot it takes and releases `dispatch_mtx_` (one lock pair per slot), checks
`event_owed`, clears it, and collects the owed key. `event_owed` is set only by
`publish_zero_op_inline`, i.e. by zero-length operations. The W-02 read workload
never calls that path, so the sweep is pure per-pass overhead: `C` lock pairs
and `C` slot inspections with no event to deliver. This is the overhead H2
targets; it is not a correctness mechanism.

Corrected complexity (the M1 report's `O(owed)` phrasing was too strong):

| Arm | Empty (no owed event) | Non-empty (`K > 0`) |
|---|---|---|
| baseline uring | `O(C)` scan + `C` lock pairs | `O(C)` scan + `C` lock pairs + `O(K)` delivery |
| H2a single-lock snapshot | `O(C)` scan + 1 lock pair | `O(C)` scan + 1 lock pair + `O(K)` delivery |
| H2b derived count | `O(1)` skip | `O(C)` scan + `C` lock pairs + `O(K)` delivery |

H2b is the W-02 win because `K` is normally zero. It is not an `O(K)` owed list;
a real list would need new index state and its own protocol matrix.

BACKEND-04 boundary: `owed_count_` is a derived summary used by `poll()` and
`poll_progress()` only. `~UringAsyncBackend()` and `internal_work_retired()`
continue to call the authoritative `any_event_owed_locked_()` scan. The counter
is never the authority for settlement, shutdown or destruction.

## 2. Layer A: mechanism microbenchmark (E5)

Median ns/pass, 15 repetitions, rotated arms; all oracle checks PASS. The timed
region includes a shared `O(C)` oracle checksum, so absolute values are inflated;
the deltas are the signal. `single-lock` is the H2a shape, `per-slot` is the
current uring shape, and `counter+snapshot` is the **H2ab shape** (one lock for
the whole scan plus a counter), not production H2b.

| Capacity | Mix | Single-lock (H2a) | Per-slot (baseline) | Counter+snapshot (H2ab shape) |
|---:|---|---:|---:|---:|
| 64 | idle | 205.9 | 601.9 | 166.6 |
| 64 | half | 1029.3 | 1493.5 | 1055.7 |
| 64 | full | 1688.7 | 2080.1 | 1754.9 |
| 256 | idle | 617.2 | 2221.4 | 473.2 |
| 256 | half | 3890.2 | 5754.2 | 3997.3 |
| 256 | full | 6514.3 | 8153.0 | 6809.7 |
| 1024 | idle | 2290.9 | 8749.3 | 1735.0 |
| 1024 | half | 15632.9 | 22919.5 | 15981.2 |
| 1024 | full | 26928.1 | 32342.2 | 27865.1 |

At idle, H2a removes the lock-pair cost and the counter+snapshot arm additionally
skips the scan; at half/full the three arms converge because delivery work
dominates. The table does not model production H2b's non-empty behavior:
production H2b keeps the baseline per-slot lock when `owed_count_ != 0`, so its
non-empty cost is the per-slot baseline, not the single-lock snapshot. The
production A/B below measures the real H2b. Negative control: a counter without
the deliver-side decrement is CAUGHT via the stale residue verdict. Positive
control: the correct counter passes 5000 rounds with sum+xor delivery checks.

## 3. Layer C: real production A/B

Paired CPU-time ratio versus baseline; median with 95% bootstrap CI; lower is
better. Primary and idle-window replication are separate columns.

| Workload | H2a primary | H2a replication | H2b primary | H2b replication | H2ab primary | H2ab replication |
|---|---:|---:|---:|---:|---:|---:|
| 4 KiB, C=16, d=1 | 0.970 | — | 0.953 | — | 0.960 | — |
| 4 KiB, C=16, d=4 | 0.986 | — | 0.956 | — | 0.979 | — |
| 4 KiB, C=16, d=8 | 1.000 | — | 0.960 | — | 0.978 | — |
| 4 KiB, C=64, d=1 | 0.919 | 0.920 | 0.879 | 0.884 | 0.894 | 0.903 |
| 4 KiB, C=64, d=4 | 0.953 | — | 0.906 | — | 0.925 | — |
| 4 KiB, C=64, d=8 | 0.971 | 0.963 | 0.914 | 0.914 | 0.947 | 0.946 |
| 4 KiB, C=256, d=1 | 0.806 | — | 0.731 | — | 0.743 | — |
| 4 KiB, C=256, d=4 | 0.873 | — | 0.807 | — | 0.819 | — |
| 4 KiB, C=256, d=8 | 0.882 | — | 0.831 | — | 0.854 | — |
| 4 KiB, C=1024, d=1 | 0.589 | 0.592 | 0.487 | 0.478 | 0.484 | 0.477 |
| 4 KiB, C=1024, d=4 | 0.765 | 0.741 | 0.676 | 0.678 | 0.699 | 0.683 |
| 4 KiB, C=1024, d=8 | 0.812 | 0.798 | 0.751 | 0.740 | 0.756 | 0.744 |
| 64 KiB, C=64, d=1 | 0.946 | — | 0.930 | — | 0.944 | — |
| 64 KiB, C=64, d=8 | 0.998 | 1.001 | 0.984 | 0.988 | 0.997 | 0.995 |
| 64 KiB, C=1024, d=1 | 0.741 | 0.747 | 0.640 | 0.648 | 0.643 | 0.647 |
| 64 KiB, C=1024, d=8 | 0.893 | — | 0.864 | — | 0.870 | — |
| 1 MiB, C=64, d=1 | 0.979 | — | 0.991 | — | 0.994 | — |
| 1 MiB, C=64, d=8 | 1.000 | — | 1.002 | — | 0.999 | — |
| 1 MiB, C=1024, d=1 | 0.912 | — | 0.905 | — | 0.910 | — |
| 1 MiB, C=1024, d=8 | 0.982 | — | 0.976 | — | 0.977 | — |

Primary CIs are in `results/ab-15/validation.txt`; replication CIs are in
`results/ab-clean/validation.txt`. Every replication CI is wholly below 1.0 for
H2b at C>=64 with 4 KiB and at C=1024 with 64 KiB. The largest single effect is
H2b at 4 KiB, C=1024, d=1: **0.478 [0.474, 0.497]** in the clean replication and
0.487 [0.471, 0.492] in the primary campaign.

Observations:

- At C=16 the effect is small (H2b 0.953-0.960) and was measured only in the
  primary campaign.
- At C=64 the effect is 8.6-12% (4 KiB) and 1.2-7% (64 KiB); H2b beats H2a on
  the 4 KiB configurations. On 64 KiB/C=64/d=8 the primary direct ratio is
  below 1 but the replication CI crosses 1, so that sub-case is not confirmed.
- At C=256 the effect is 17-27% and H2b's extra skip over H2a is 5-8 points.
- At C=1024 the effect is 24-52%; H2b's extra skip is 6-10 points.
- At 1 MiB the per-operation I/O/copy work dominates. H2a is actually better at
  C=64/d=1 (direct H2b/H2a = 1.0123, 15/15 repetitions); at C=64/d=8 the two are
  neutral; at C=1024 the difference is small.
- H2ab is **not** indistinguishable from H2b. At 4 KiB/C=64/d=8 the direct
  H2ab/H2b ratio is 1.0376 [1.0175, 1.0468] primary and 1.0403 [1.0184, 1.0454]
  replication; at 4 KiB/C=64/d=1 and d=4 it is also above 1. At
  4 KiB/C=1024/d=1 the sign flips between campaigns (1.0137 primary,
  0.9841 replication). There is no repeatable gain over H2b, and the combined
  arm adds the snapshot buffer.

### Direct paired contrasts

These bypass the baseline and test H2b against H2a, and H2ab against H2b, on the
same repetitions:

| Contrast | Campaign | Median ratio [95% CI] | Reps above 1 |
|---|---|---:|---:|
| H2b / H2a, 4 KiB C=64 d=8 | primary | 0.9404 [0.9224, 0.9555] | 0/15 |
| H2b / H2a, 4 KiB C=1024 d=1 | primary | 0.8106 [0.7997, 0.8361] | 0/15 |
| H2b / H2a, 4 KiB C=1024 d=1 | replication | 0.8243 [0.8193, 0.8361] | 0/15 |
| H2b / H2a, 64 KiB C=64 d=8 | primary | 0.9855 [0.9834, 0.9921] | 2/15 |
| H2b / H2a, 64 KiB C=64 d=8 | replication | 0.9834 [0.9736, 1.0035] | 6/15 |
| H2b / H2a, 1 MiB C=64 d=1 | primary | 1.0123 [1.0093, 1.0157] | 15/15 |
| H2b / H2a, 1 MiB C=1024 d=1 | primary | 0.9932 [0.9902, 1.0027] | 6/15 |
| H2ab / H2b, 4 KiB C=64 d=8 | primary | 1.0376 [1.0175, 1.0468] | 14/15 |
| H2ab / H2b, 4 KiB C=64 d=8 | replication | 1.0403 [1.0184, 1.0454] | 13/15 |
| H2ab / H2b, 4 KiB C=1024 d=1 | primary | 1.0137 [1.0085, 1.0202] | 11/15 |
| H2ab / H2b, 4 KiB C=1024 d=1 | replication | 0.9841 [0.9654, 0.9958] | 3/15 |

All 15 repetitions in both campaigns passed the byte oracle, the op-count check
and the zero-timed-allocation check; there are no oracle-failing timing rows.

## 4. Layer B: where the time goes

At 4 KiB, C=1024, d=1, 5 repetitions, unmultiplexed `perf stat`:

| Arm | cycles/op | instructions/op | user s | sys s |
|---|---:|---:|---:|---:|
| base | 41.30 k | 116.46 k | 2.47 | 1.11 |
| H2a | 16.13 k | 45.41 k | 1.02 | 1.12 |
| H2b | 11.22 k | 30.62 k | 0.74 | 1.05 |

The baseline retires about 86 k more instructions per operation than H2b; at
C=1024 and depth 1 that is the per-pass sweep paid once per operation. H2b cuts
user CPU by 70% and leaves system time essentially unchanged (1.107 s -> 1.049 s,
about 5% lower): the gain is in the user-space owed sweep, not in kernel
submission.

`perf record` self-symbol shares:

| Symbol | Base | H2a | H2b |
|---|---:|---:|---:|
| mutex lock + unlock variants | ~51.4% | ~4.6% | ~7.5% |
| `UringAsyncBackend::poll()` self | 20.18% | 17.89% | 0.84% |
| `poll_progress()` | 4.42% | 13.29% | not in top 18 |
| `service_pending_controls_locked_()` | 8.81% | 22.68% | 32.50% |
| `dispatch_after_accept()` | 7.50% | 20.28% | 28.46% |

In H2b, `poll_progress` disappears from the profile (the derived counter returns
early) and the remaining time is real submission, control and dispatch work.
H2a's single-lock scan shows up as the 13.29% `poll_progress` self share; that is
the work H2b skips entirely. Shares are not wall-clock speedups; the paired A/B
is the performance claim.

Full method, sample counts and limits: `cpu-attribution.md`.

## 5. Correctness evidence

- `xmake test`: H2a 74/74, H2b 75/75, H2ab 75/75; baseline 74/74. The extra test
  is `uring_owed_counter_parity_test`.
- Parity controls: zero-op tracking, data-op immunity, slot-reuse parity, and
  producer/poller concurrency with exactly-once delivery (one producer thread).
  All pass.
- The mutant `uring_owed_counter_parity_mut_no_decrement` fails
  `counter_tracks_zero_op_owed_events`, so the test is not a tautology.
- `owed_count_` is incremented only on `false -> true` of `event_owed` in
  `publish_zero_op_inline`, decremented on snapshot consumption, and also on
  submit reset. The submit-reset decrement is a defensive branch that is
  unreachable in a consistent state: `event_owed` holds a core control
  reference, and a slot cannot be re-reserved while that reference is held.
  A product proposal must not silently normalize this state: it has to either
  justify the transition as legal with an independent test that reaches it,
  or replace the branch with an invariant-fail (fail-fast) whose
  unreachability is established by a negative oracle. The research patch
  keeps the silent branch only because the timing result does not depend on
  it. The H2ab arm
  additionally asserts `owed_bound == expected` and terminates on a parity loss.
- Destructor and `internal_work_retired()` use `any_event_owed_locked_()`, not
  the derived count. BACKEND-04 is preserved.
- No public API change; `UringConfig` is unchanged.

## 6. Minimal product proposal (not applied)

If the owner decides to productize, the smallest change is **H2b only**:

1. Add `std::size_t owed_count_` and `owed_visible_locked_()` to the backend.
2. Increment on the `false -> true` transition in `publish_zero_op_inline`;
   decrement on submit reset and on snapshot consumption.
3. Snapshot the count under `dispatch_mtx_`; skip the scan when zero.
4. Keep `any_event_owed_locked_()` in `~UringAsyncBackend()` and
   `internal_work_retired()`.
5. Add the parity test and the no-decrement mutant target.

Validation checklist before any product merge:

- the submit-reset branch disposition: either a fail-fast invariant whose
  unreachability a negative oracle establishes, or a justified-and-tested
  legal transition — silent normalization is not acceptable;
- **W-03 admission**: the external-event-loop notification/acknowledge path
  under the derived count (`W03_AB = NOT_RUN` in M2; a W-02 gain does not
  certify W-03 wakeup/liveness);
- full deterministic suite plus the parity test and mutant kill, with
  zero-length I/O, concurrent submission, cancellation and shutdown called
  out;
- counter parity under every set/clear path and concurrent producers;
- BACKEND-04 audit: teardown still authoritative;
- paired A/B at C=64/256/1024, 4 KiB and 64 KiB, 15 valid pairs, oracle and
  zero-timed-allocation checks;
- perf confirmation that mutex share drops and system time remains essentially
  unchanged;
- v2 Ledger update with exact evidence.

M2 deliberately does not create that product PR. The repository root and the
owner decide whether the change is worth productizing. H2b is a win for
small-block, high-capacity, CPU-bound work; it is the wrong choice for
1 MiB/C=64/d=1, where H2a is better, and neither mechanism matters for
I/O-dominated large-block work.

## 7. Limitations and open questions

- H2a's shared `owed_batch_` is safe only because THREAD-01 restricts `poll()` to
  the fixed progress owner. H2a is not selected; a future concurrent/recursive
  poll design would need its own protocol matrix.
- The H2b test seams read `owed_count_` and call `any_event_owed_locked_()`
  without taking `dispatch_mtx_`. Current tests call them only when quiescent;
  the `_locked_` name is a convention, not an enforced precondition.
- Production H2b still scans `C` slots with the baseline per-slot locks when
  `owed_count_ != 0`. A true owed-event list would need a new index, new state
  and new concurrency evidence; it is not proposed here.
- The submit-reset decrement is defensive and currently unreachable; keep only
  if a product reviewer can name the interleaving that requires it.
