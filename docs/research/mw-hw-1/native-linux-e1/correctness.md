# E1 Native-Linux Validation — Correctness (Gate R1)

Semantic gate: **PASS**. All deterministic suites green on the E1 arm, all
mutants killed, the named-oracle requirement met, and all six protocol-required
adversarial scenarios exercised.

## Source-level state machine review

All `control_pending` transitions live under `dispatch_mtx_`. The E1 patch
touches exactly four sites (candidate `src/async/uring_transport.cpp`):

1. `issue_running_cancel_locked_` — control already prepared/submitted:
   clear + decrement (guarded by `if (route.control_pending)`).
2. `issue_running_cancel_locked_` — SQE unavailable: sticky set + increment
   only when previously clear (idempotent under repeated cancel);
   `sticky_intent_retained()==false` (mutant-only) clears + decrements.
3. `issue_running_cancel_locked_` — control SQE prepared: clear + decrement
   (the sticky intent retires *into* the issued control).
4. `retire_router_entry_` — route dies with a pending intent: clear +
   decrement before the slot returns to the free list.

The counter is a derived summary: the authoritative scan survives verbatim in
`pending_control_scan_for_test()` and the parity oracle compares the two.
`service_pending_controls_locked_()` keeps its fatal-error check *before* the
zero-count gate, so poison semantics are unchanged.

## Stock deterministic suites (E1 arm, all rc=0)

```
uring_backend_smoke_test            uring_progress_race_test
uring_external_loop_test            uring_registration_lifecycle_test
uring_owed_residue_test             uring_owed_counter_parity_test
uring_pending_control_parity_test   uring_request_core_integration_test
uring_submit_boundary_test          request_core_ownership_uring_test
progress_source_ownership_uring_test backend_contract_conformance_uring_test
```
(raw outputs: `~/sluice-e1-e5/results/r1-out-*.txt`, summary
`r1-suite-results.txt`)

## Mutant kills

| Mutant | Stock-suite kill | Named-oracle kill |
|---|---|---|
| `uring_pending_control_mut_missing_increment` | rc=134 (destructor abort) | **yes** — `FAIL parity oracle: pending_control_count_ diverged from the router scan after the sticky set (missing increment)`, rc=1 |
| `uring_pending_control_mut_missing_decrement` | rc=1 `FAIL: sqe_pressure_cancel_sets_and_keeps_pending` | **yes** — `FAIL parity oracle: count did not retire with the issued intent (missing decrement)`, rc=1 |
| `backend_contract_conformance_mut_sticky_intent_dropped` | rc=134 | (contract suite owns this mutant; not an E1-counter mutant) |
| `uring_owed_mut_reclaim_ignoring_control_refs` | rc=134 | (owed-count mutant; still killed on the E1 arm) |
| `uring_core_integration_mut_reclaim_before_control` | rc=134 | (early-retire/control-ref mutant; still killed on the E1 arm) |

The protocol's named-oracle requirement for missing increment/decrement is
met by `scripts/e1_mutant_oracle_test.cpp` (research-only): it reports
through `fail()+_Exit` so no request destructor can preempt the named
verdict, and adds the protocol oracle "a sticky intent under SQE pressure
must be issued once pressure releases". Healthy build passes; both mutants
die with their named parity message. (Raw: `~/sluice-e1-e5/results/`,
mutant-oracle build variants recorded in the round log.)

## Adversarial scenarios (`scripts/e1_adversarial_test.cpp`, all PASS)

1. **Concurrent cancel storm** — 8 latch-gated threads cancel one running
   request while a monitor thread continuously checks parity; disposition
   all `requested`; count stays exactly 1; intent survives 3 pressure
   passes; issues after release; settles through control+`-ECANCELED`+read
   CQEs; parity at every checkpoint.
2. **Poison with pending intent** — sticky intent set, then a real `-EINVAL`
   submit-hook poison; parity holds through poison and shutdown; count
   returns to 0 as routes retire; both requests reach terminal states.
3. **Shutdown after pending intent settles** — sticky intent issued and
   settled, then `shutdown(drain)` converges; no stale count; terminal
   responsibility intact.
4. **Slot reuse after retirement** (in the mutant-oracle file) — a fresh op
   on the freed slot starts with clean control state; count and scan stay 0.
5. **CQE-ordering races** — control-first and original-CQE-first orderings
   exercised (stock parity suite + adversarial + mutant oracle).
6. **Wedged-transport shutdown probe** — `cancel_then_drain` under a
   permanently wedged transport does not converge, identically on BASE and
   E1 (probe lines recorded on both arms). Pre-existing drain semantics,
   not an E1 regression; outside the contract's reachable states (a real
   submit returns errors rather than wedging).

## P>0 semantic evidence

The seam probe (`results-p5/p5-probe.jsonl`) and the adversarial suites show
`pending_control_count_ == router scan` (parity) at every observed point;
parity violations: 0 across all runs. Real-kernel sticky observation is
covered in `pending-control-pressure.md` (NOT observed on this stack).
