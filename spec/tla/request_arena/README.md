# request_arena — RequestArena / RequestSlot explicit-I/O request lifecycle

Protocol: **explicit-I/O accepted-request lifecycle** — the slot state machine
every migrated async backend runs on (`free → reserved → prepared → pending →
enqueued → running → backend_ready → completion_ready → free` with generation
increment), per ADR-explicit-io-request-contract (Accepted) and
AGENTS.md §3.2.

This suite closes the manifest's former `request-arena-lifecycle` ACCEPTED
FORMAL-DEBT gap with the smallest model that captures the load-bearing races
(AGENTS.md §7): Scheme-B enqueue/cancel arbitration, terminal-winner
exactly-once, running-cancel intent vs verbatim ordinary results, reap-only
Completion publication gated on the acknowledged enqueue pin, generation
advance before reuse, and borrow-through-reap.

C++ authority (one atomic TLA action = one leaf slot-lifecycle mutex critical
section; TLA interleaving = mutex acquisition order):

- `include/sluice/async/detail/request_arena.hpp`
- `include/sluice/async/detail/request_slot.hpp`

## Authority layering (PR #125 review P1)

The model separates two authorities and never blurs them:

- **Layer A — leaf safety (proven here).** Everything the arena leaf itself
  enforces under its one mutex: admission staging, Scheme-B arbitration via
  the arena's own `cancel()` entry, terminal exactly-once, generation
  advance before reuse, pin/reap gating, borrow window, quiescent destroy.
- **Layer B — external obligations (assumed here, owned by other code).**
  - *Progress*: `WF(Enqueue)` is the **backend submit path's obligation**
    (`ThreadPoolBackend::enqueue_after_commit` — mandatory noexcept
    post-commit step); `WF(Reap)` is the **backend/runtime progress loop's
    obligation** (`ThreadPoolBackend::poll`/`wait_one`,
    `UringAsyncBackend` reaper paths call `arena_.reap`). The arena itself
    cannot make anyone invoke enqueue or reap — the liveness properties are
    CONDITIONAL on these obligations, and every cfg sets
    `CHECK_DEADLOCK FALSE` because terminal states legitimately have no
    enabled action: **this suite does not prove deadlock-freedom**.
  - *Decision-11 provenance*: `RecordCanceledConfirmed` models the
    **backend obligation** to call `record_canceled` only after a CONFIRMED
    interruption. The C++ leaf `record_canceled(h)` is just
    `record_terminal(err(canceled))` — it validates handle generation, slot
    state, and exactly-once, but performs NO cancel-intent or provenance
    check, and **no production backend currently calls it** (tests simulate
    the confirming backend). `InvCanceledTerminalSource` is therefore an
    environment-contract invariant: "IF callers honor the obligation THEN
    no intent-only running cancel yields a canceled terminal" — the leaf
    does not enforce the discipline, and NEG-RA-6 pins the ill-behaved
    caller.

A compositional RequestArena + backend-progress refinement is recorded as
debt in `docs/verification/formal/cpp-model-coverage.md` rather than folded
into this leaf model.

## Gates

| Gate | cfg | Expect |
| ---- | --- | ------ |
| Positive safety (18 invariants) | `RequestArena.cfg` | PASS |
| Liveness (WF Enqueue + WF Reap) | `RequestArenaLiveness.cfg` | PASS |
| NEG-RA-1 second terminal winner | `RequestArenaFaultDoubleTerminal.cfg` | `InvNoDoubleTerminal` violated |
| NEG-RA-2 stale-generation cancel (causal old key) | `RequestArenaFaultStaleCancel.cfg` | `InvTerminalRequiresAccepted` violated |
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
| `record_canceled` — **backend obligation, not leaf-enforced; no production caller today** (leaf checks only state/exactly-once; see Authority layering) | `RecordCanceledConfirmed` |
| `cancel` pending/enqueued (terminal_won) | `CancelPendingOrEnqueued` |
| `cancel` running (`intent_recorded`, Decision 11) | `CancelRunningIntent` |
| `register_waiter` / `cancel_waiter` | `RegisterWaiter` / `CancelWaiter` |
| `reap` (review C3 leaf-domain publication) | `Reap` |
| `release_completed_binding` | `ReleaseCompleted` |
| `close_admission` | `CloseAdmission` |
| destructor quiescence (AC-13) | `Destroy` + guards + `InvDestroyQuiescent` |
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
| State machine + illegal transitions | `tests/request_lifecycle_scheme_b_test.cpp` (`arena_mainline_state_transition_matrix`, `arena_illegal_transition_contract_errors`); `tests/request_arena_test.cpp` (supporting: `arena_capacity_bounded`, `arena_stale_key_rejected`, `arena_accounting_tracks_slot_in_use_vs_accepted_outstanding`, `arena_borrow_lifecycle`) |
| Stale identity (W4, NEG-RA-2/4) | `tests/request_lifecycle_scheme_b_test.cpp` (`generation_reuse_stale_attempts`, `arena_stale_handle_leaves_live_occupant_untouched`), `tests/request_arena_death_test.cpp` |
| Running-cancel intent vs verbatim (W3, NEG-RA-6) | `tests/request_arena_cancel_intent_test.cpp` |
| Waiter exactly-once delivery (W5) | `tests/request_lifecycle_scheme_b_test.cpp` (`reap_wins_lease_over_wait_cancel`, `waiter_registration_cardinality`); `tests/request_waiter_borrow_lease_test.cpp` (supporting register-vs-reap / cancel_waiter-vs-reap) |
| Reap-only publication / acquire ordering | `tests/request_lifecycle_scheme_b_test.cpp` (`acquire_observer_of_ready_sees_all_effects`); `tests/threadpool_backend_reap_test.cpp` (supporting) |
| Close-admission vs in-flight submission | `close_admission_rejects_new_but_existing_reapable`, `close_admission_gates_reserve_not_inflight_prepared_slot` |
| Cross-backend Scheme-B races | `tests/threadpool_backend_scheme_b_race_test.cpp`, `tests/backend_scheme_b_race_test.cpp` |

## Scope and residuals

- **One slot (capacity 1).** `would_block` admission is exercised
  structurally (a busy slot admits nothing). Multi-slot interference and
  ready-ring FIFO **order across slots** (ADR Decision 9's backend-known
  order) remain executable-evidence scope (`request_arena_test`,
  `reference_backend_arena_lifecycle_test`).
- `MaxGen = 2` bounds the state space for TLC; it is not a protocol rule.
  `Reserve` admits only generations 0 and 1 (`gen < MaxGen`), so the model
  explores exactly two occupant generations with one full release/reuse
  cycle; generation 2 is reached only as the terminal free state after the
  second release and admits no further `Reserve`.
- The backend admission transaction around commit (Completion
  `idle → binding → outstanding`, AGENTS.md §3.3) is a different protocol and
  is NOT modeled here.
- Fairness: `WF(Enqueue)` = backend submit-path obligation,
  `WF(Reap)` = backend/runtime progress-loop obligation — **external
  Layer-B assumptions, NOT guarantees of the arena leaf** (see Authority
  layering). Backend/syscall termination is likewise an environment
  assumption, same boundary as the blocking-io-pool suite. `CHECK_DEADLOCK
  FALSE` in every cfg: this suite does not claim deadlock-freedom.

## Reproduce

```sh
python3 scripts/formal/verify.py suite request-arena
# or directly:
bash scripts/formal/verify-request-arena.sh
```
