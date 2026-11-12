# Final contract repair and independent verification — 2026-11-12

Closing round for PR #634 (branch `product/621-threadpool-owed-count-gate`,
base `f2c42535`). Scope: exact counter parity oracle, statistical wording
repair, independent semantic re-review, full verification gate re-run. The
production mechanism (the `owed_count_` gate) is unchanged this round; the
measured A/B evidence is unchanged.

## 1. Exact parity oracle (repair)

Before this round `owed_scan_for_test()` returned only the boolean
`any_event_owed_locked_()` verdict, so the oracle could compare the counter
against a test expectation and a zero/nonzero scan but never directly
verified the full counting invariant
`owed_count_ == #{record | record.event_owed}`.

- Added test-only `ThreadPoolBackend::owed_scan_count_for_test()`
  (`SLUICE_ASYNC_INTERNAL_TESTING` only): takes `work_mtx_`, counts the
  records with `event_owed` set, writes nothing. No production-path cost in
  non-test builds; no new hot-path scan.
- The oracle `counter_matches_authority` now asserts
  `owed_count_for_test() == owed_scan_count_for_test() == expected` **and**
  `owed_scan_for_test() == (expected != 0)`, keeping the tie to the
  production existence scan used by settlement. Cross-interface reads stay
  at quiescent checkpoints only (inline zero-op submits, the owed-sweep
  pause gate, or joined producers).
- The three contracts stay separately observable: counter precision
  (count==scan-count), obligation existence (production boolean scan), sink
  exactly-once delivery (delivery counts asserted independently).

## 2. Statistical wording repair

The round's frozen protocol pre-registered the negative-control cell and its
directional expectation, but **no non-inferiority margin** (the house
convention with explicit margins, e.g. uring-submit-policy round-e5's ≤5%
bounds, was not applied to this round). "Non-inferior, as pre-registered"
in the README, REPORT §4/§5/§6, the ledger row and the CHANGELOG entry
overstated the claim. All five locations plus the PR body now read: **no
statistically resolved regression in the measured negative-control cell**,
with the explicit caveat that the CI upper bound (+1.77%) cannot exclude a
CPU regression that small. Observations, CIs, sample sizes and methods are
unchanged. The off-repo original at `/home/jnhu/sluice-621-round/REPORT.md`
keeps its original wording as the dated historical record.

## 3. Independent semantic re-review findings

Re-derived from the product diff (not from the round's reports):

- **Counter invariant**: exhaustive `event_owed` writer list is three sites
  — submit-path record re-init (`false`, safe only on reclaimable slots),
  `publish_zero_op_inline` (`true` + increment), the gated sweep
  (`false` + decrement). Counter and record mutations share `work_mtx_`; no
  unsynchronized reader (all test accessors lock). No double increment,
  double decrement or underflow path outside the registered mutants.
- **Slot lifetime**: owed event acquires its control reference before the
  flag is set and releases it only after `deliver_event` in the sweep;
  `reclaimable_` requires `control_refs == 0`, and slots re-enter
  `free_slots_` only through `try_reclaim_`, so a submit can never re-init a
  record with a pending owed event. `event_owed ⇒ control_refs ≥ 1` and
  `control_refs > 0 ⇒ !reclaimable_` hold on every reachable path
  (generation, publication inflight, cancel, shutdown inspected).
- **Progress/liveness**: the gate cannot strand an owed event with a
  correct counter — every mid-pass producer increments under `work_mtx_`,
  the epilogue verdict re-locks and reads the same counter, and
  `signal_ready_progress()` fires on every owed publication. Settlement and
  teardown still consult `any_event_owed_locked_()`, so a hypothetical
  low-drifted counter parks settlement (liveness failure) rather than
  silently dropping delivery.
- **Authority separation**: no admission, settlement, publication,
  retirement or wake decision reads the counter; no new public API, lock
  domain or cross-backend abstraction was introduced. No blocking: the two
  new test seams are test-only pause-gate plumbing identical in shape to
  the existing seams.

No correctness defect found; no production code change required this round.

## 4. Verification re-run (this round, same host/toolchain)

- Parity suite: 8/8 PASS in all four matrix cells and under TSAN and ASAN.
- Mutants: all four killed by the runtime oracle at the strengthened
  oracle — see `mutant-receipts.txt` (re-verification section) for exit
  codes, failure sites, watchdog behavior and the gdb confirmation of the
  missing-increment settlement deadlock.
- Configuration matrix: release/liburing=y 80/80, release/liburing=n 59/59,
  debug/liburing=y 80/80, debug/liburing=n 59/59 — 0 failures.
- Sanitizers: TSAN on the five concurrency suites (parity, race, external
  loop, pass bound, notification suppression) and ASAN on parity + race —
  clean.
- Performance evidence: not re-run — the production hot path is unchanged
  by this round (test-only seams and documentation only), so the measured
  A/B conclusions and their scope limits carry over unchanged.

## 5. Manifest

`MANIFEST.sha256` regenerated for the files changed by this round; raw
A/A, A/B and perf-stat artifacts are unchanged from the original round.
