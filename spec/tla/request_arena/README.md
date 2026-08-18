# request_arena — RequestArena / RequestSlot explicit-I/O request lifecycle

Protocol: **explicit-I/O accepted-request lifecycle** — the slot state machine
every migrated async backend runs on (`free → reserved → prepared → pending →
enqueued → running → backend_ready → completion_ready → free` with generation
increment), per ADR-explicit-io-request-contract (Accepted) and
AGENTS.md §10.

This suite closes the manifest's former `request-arena-lifecycle` ACCEPTED
FORMAL-DEBT gap with the smallest model that captures the load-bearing races
(AGENTS.md §17): Scheme-B enqueue/cancel arbitration, terminal-winner
exactly-once, running-cancel intent vs verbatim ordinary results, reap-only
Completion publication gated on the acknowledged enqueue pin, generation
advance before reuse, and borrow-through-reap.

C++ authority (one atomic TLA action = one leaf slot-lifecycle mutex critical
section; TLA interleaving = mutex acquisition order):

- `include/sluice/async/detail/request_arena.hpp`
- `include/sluice/async/detail/request_slot.hpp`

## Gates

| Gate | cfg | Expect |
| ---- | --- | ------ |
| Positive safety (17 invariants) | `RequestArena.cfg` | PASS |
| Liveness (WF Enqueue + WF Reap) | `RequestArenaLiveness.cfg` | PASS |
| NEG-RA-1 second terminal winner | `RequestArenaFaultDoubleTerminal.cfg` | `InvNoDoubleTerminal` violated |
| NEG-RA-2 stale-generation cancel | `RequestArenaFaultStaleCancel.cfg` | `InvTerminalRequiresAccepted` violated |
| NEG-RA-3 publish without reap | `RequestArenaFaultDirectPublish.cfg` | `InvPublishedCompleteness` violated |
| NEG-RA-4 no generation increment | `RequestArenaFaultNoGenIncrement.cfg` | `InvGenAdvanceOnFree` violated |
| NEG-RA-5 reap ignores live pin | `RequestArenaFaultReapIgnoresPin.cfg` | `InvNoPinnedPublication` violated |
| NEG-RA-6 running cancel stores terminal | `RequestArenaFaultRunningCancelStores.cfg` | `InvCanceledTerminalSource` violated |
| Wrong-property controls ×2 | `RequestArenaWrongProp1/2.cfg` | PASS (faults do NOT trip unrelated laws) |
| Witnesses W1–W5 | `RequestArenaSceneW1..W5.cfg` | `NotReach_*` violated (scenes reachable) |

Negative models use the e13 FAULT-constant pattern: each `Fault_*` action is
enabled only under its `FAULT` value and performs exactly the buggy
transition; each fault cfg asserts ONLY its named target invariant, so the
verifier's named check is exact. `FAULT = "None"` (the positive cfg) doubles
as the restore gate.

## Concrete ↔ abstract mapping

| C++ | TLA |
| --- | --- |
| `RequestState` (`request_slot.hpp:48`) | `state` |
| `Generation generation_` + release `++` | `gen`, `InvGenAdvanceOnFree` |
| `SlotHandle{slot, generation}` staleness (`validate_`) | generation-indexed ghosts; `FaultStaleCancel` |
| `enqueue_in_flight_pin_` (I19) | `pin`, `InvPinPhase`, `InvNoPinnedPublication` |
| `reserve/prepare/install_publication_binding/commit` | `Reserve/Prepare/InstallBinding/Commit` |
| `rollback_reserved_or_prepared` | `RollbackPreCommit` |
| `enqueue` (Scheme-B outcomes, ADR Decision 5) | `Enqueue` (enqueued / terminal_noop pin ack) |
| `mark_running` | `MarkRunning` |
| `record_terminal` (verbatim winner) | `RecordTerminal` |
| `record_canceled` (confirmed interruption only) | `RecordCanceledConfirmed` |
| `cancel` pending/enqueued (terminal_won) | `CancelPendingOrEnqueued` |
| `cancel` running (`intent_recorded`, Decision 11) | `CancelRunningIntent` |
| `register_waiter` / `cancel_waiter` | `RegisterWaiter` / `CancelWaiter` |
| `reap` (review C3 leaf-domain publication) | `Reap` |
| `release_completed_binding` | `ReleaseCompleted` |
| `close_admission` | `CloseAdmission` |
| destructor quiescence (AC-13) | `Destroy` + guards |
| `slot_in_use_` / `accepted_outstanding_` (P1-05) | `slotInUse` / `acceptedOutstanding` |
| `backend_ready_count_` + ready-ring linkage | `onRing` |
| `BorrowMetadata::active` (I7/I18) | `borrowActive`, `InvBorrowWindow` |
| `publication_binding_.installed()` (review C2) | `bindingInstalled`, `InvReapRequiresBinding` |
| terminal canceled bit | `terminalCanceled` + `cancelSource` ghost |

## C++ regression bridge

Every load-bearing model property maps to existing executable regressions
(a TLA counterexample must be reproducible as a C++ scenario):

| Model property | C++ test |
| -------------- | -------- |
| Scheme-B arbitration (W1/W2), terminal winner, pin/noop | `tests/request_lifecycle_scheme_b_test.cpp` (`pending_cancel_wins_before_enqueue_then_enqueue_noop`, `exactly_one_terminal_winner`, `concurrent_submit_cancel_enqueue`) |
| State machine + illegal transitions | `tests/request_arena_test.cpp`, scheme-b `arena_mainline_state_transition_matrix`, `arena_illegal_transition_contract_errors` |
| Stale identity (W4, NEG-RA-2/4) | `generation_reuse_stale_attempts`, `arena_stale_handle_leaves_live_occupant_untouched`, `tests/request_arena_death_test.cpp` |
| Running-cancel intent vs verbatim (W3, NEG-RA-6) | `tests/request_arena_cancel_intent_test.cpp` |
| Waiter exactly-once delivery (W5) | `tests/request_waiter_borrow_lease_test.cpp` (`reap_wins_lease_over_wait_cancel`, `waiter_registration_cardinality`) |
| Reap-only publication / acquire ordering | `acquire_observer_of_ready_sees_all_effects`, `tests/threadpool_backend_reap_test.cpp` |
| Close-admission vs in-flight submission | `close_admission_rejects_new_but_existing_reapable`, `close_admission_gates_reserve_not_inflight_prepared_slot` |
| Cross-backend Scheme-B races | `tests/threadpool_backend_scheme_b_race_test.cpp`, `tests/backend_scheme_b_race_test.cpp` |

## Scope and residuals

- **One slot (capacity 1).** `would_block` admission is exercised
  structurally (a busy slot admits nothing). Multi-slot interference and
  ready-ring FIFO **order across slots** (ADR Decision 9's backend-known
  order) remain executable-evidence scope (`request_arena_test`,
  `reference_backend_arena_lifecycle_test`).
- `MaxGen = 2` bounds the state space for TLC (three occupant generations);
  it is not a protocol rule.
- The backend admission transaction around commit (Completion
  `idle → binding → outstanding`, AGENTS.md §4.3) is a different protocol and
  is NOT modeled here.
- Fairness: only `WF(Enqueue)` (mandatory noexcept post-commit step) and
  `WF(Reap)` (level-triggered wait/progress paths). Backend/syscall
  termination is an environment assumption, same boundary as the
  blocking-io-pool suite.

## Reproduce

```sh
python3 scripts/formal/verify.py suite request-arena
# or directly:
bash scripts/formal/verify-request-arena.sh
```
