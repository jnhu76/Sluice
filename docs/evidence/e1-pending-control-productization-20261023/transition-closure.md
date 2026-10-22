# E1 Productization — State-Transition Closure (Gate R2)

Rebuilt from production source at product HEAD (branch
`perf/e1-pending-control-zero-scan`, base `859e4b56`), not from the research
patch. Every `control_pending` and `pending_control_count_` write site is
enumerated with its reachability condition, pre/post state, lock domain and
distinguishing oracle.

## Governing invariants

- **INV-1 (parity)** —
  `pending_control_count_ == #{route | route.in_use && route.control_pending}`.
- **INV-2 (control-state exclusivity)** —
  `route.control_state != none ⟹ route.control_pending == false`.
- **INV-3 (single lock domain)** — every write to `control_pending` or
  `pending_control_count_` executes while `dispatch_mtx_` is held.
- **INV-4 (non-authoritative summary)** — the only production reader of
  `pending_control_count_` is the zero-count gate in
  `service_pending_controls_locked_()` (`uring_transport.cpp:700-701`).
  Settlement, `internal_work_retired()`, `retire_execution_resources()`, the
  destructor and RequestCore consult authoritative state only (verified by
  grep at product HEAD: the identifier appears nowhere else in `src/`).

## Write sites

All sites are in `src/async/uring_transport.cpp` on the product HEAD.
`S` abbreviates `RouterEntry::ControlState`.

| # | Site (line) | Containing function | Reachable when | Pre → Post (`control_pending`) | Count effect | Lock domain | Oracle |
|---|---|---|---|---|---|---|---|
| W1 | `:629-632` | `issue_running_cancel_locked_` | public `cancel` on a route whose control is already prepared/submitted (repeated/coalesced cancel) | true → false | −1 (guarded) | `dispatch_mtx_` via `cancel_key` | `repeated_cancel_oracle` (coalesced requested ×3, count stays 1) |
| W2 | `:654-665` (set arm) | `issue_running_cancel_locked_` | SQE unavailable after the in-function retry submit (`sqe == nullptr` twice) and sticky intent retained | false → true | +1, exactly once per edge (the `!route.control_pending` guard makes repeated cancel idempotent) | `dispatch_mtx_` via `cancel_key` or `service_pending_controls_locked_` | `sticky_lifecycle_oracle` parity checkpoint; `concurrent_cancel_storm` |
| W3 | `:666-669` (clear arm) | `issue_running_cancel_locked_` | same exhaustion path under `SLUICE_BACKEND_CONTRACT_MUTANT_STICKY_INTENT_DROPPED` only | true → false | −1 | `dispatch_mtx_` | existing backend-contract suite's sticky-intent-drop mutant (unchanged) |
| W4 | `:682-686` | `issue_running_cancel_locked_` | an SQE was obtained; the control is prepared | true → false | −1 (guarded) | `dispatch_mtx_` | `sticky_lifecycle_oracle` "count did not retire with the issued intent" |
| W5 | `:573-578` | `retire_router_entry_` | a route holding a sticky intent is retired (original CQE first, poison Class-A recovery, stranded-completion settlement) | true → false | −1 | `dispatch_mtx_` via `handle_one_cqe`/`finalize_operation_terminal_`/`poison_and_recover_locked` | `retire_and_slot_reuse_oracle`; `poison_with_pending_intent` |

Retire callers (all under `dispatch_mtx_`): `uring_completion.cpp:95`
(original CQE, control already none), `uring_completion.cpp:137,140` (control
CQE with terminal delivered), `uring_transport.cpp:440` (poison Class-A),
`uring_transport.cpp:517` (mutant-gated stranded settlement, not in
production builds).

## Transition matrix against the task's checklist

| Required transition | Product site | Status |
|---|---|---|
| SQE exhausted + sticky intent: false→true, +1 | W2 | INV-1 holds; +1 fires only on the false→true edge |
| Repeated cancel does not re-increment | W1 + W2 guard | second cancel either coalesces at W1 (control live) or re-enters W2 with `control_pending` already true → no increment |
| SQE available, control prepared: true→false, −1 | W4 | decrement only when the flag was set |
| Original CQE completes first | W5 via `finalize_operation_terminal_` | intent dies with the route; count synchronized |
| Control CQE arrives | flag already false at W4; `handle_one_cqe` clears `control_state` only | no double decrement by construction |
| Router retire | W5 | count retires with the route on every retire path |
| Router slot reuse | `entry = RouterEntry{}` at `:579` reinitializes the flag; retire pre-decremented | new entry starts false, count unchanged (0 contribution) |
| Poison/recovery | W5 via `:440`; the derived summary never gates recovery | poison convergence and settlement keep the authoritative scans |
| Shutdown | no new dependency: `internal_work_retired()` and the shutdown driver never read the count | `shutdown_after_pending_intent_settles`, `poison_with_pending_intent` |

## Invariant proofs

**INV-2** (control-state exclusivity): `control_pending` becomes true only at
W2, which is textually dominated by the `control_state != none` early return
at `:627-633`. `control_state` leaves `none` only at the prepared transition
(`:680`), whose block W4 executes in the same critical section; the flag is
therefore false whenever `control_state` is `prepared` or `submitted`, and the
`prepared → submitted` (`mark_consumed_control_locked_`,
`uring_transport.cpp:294`) and `submitted → none` (`uring_completion.cpp:117`,
poison recovery `uring_transport.cpp:430,492`) transitions never write
`control_pending`. Consequently **W1's clear is defensive**: in any
INV-1-consistent execution the guarded decrement at W1 never fires. It is
retained (a) to preserve master's exact clearing semantics at this site and
(b) so that a hypothetical INV-2 violation would fail toward parity, not away
from it. Its unreachability is a control-flow proof, not a test-coverage
claim.

**INV-1** (parity): by induction over the five sites. The flag has no other
writers (grep at product HEAD: `control_pending` occurrences in `src/` are
exactly the sites above plus the scan predicates at `:707` and the test
snapshot). Each true↔false edge adjusts the count exactly once inside the
same critical section that flips the flag; slot reinitialization produces a
false flag with no count change because W5 already restored the count on
retirement. Readers outside the lock do not exist (INV-4).

**INV-3**: callers of `issue_running_cancel_locked_` are `cancel_key`
(`:581` lock) and `service_pending_controls_locked_` (locked callers:
`dispatch_after_accept` `:49`, `poll`); `retire_router_entry_` callers are
enumerated above, each inside a `dispatch_mtx_` critical section.

## Concurrency-correct test snapshot

`pending_control_snapshot_for_test()` (`src/async/uring_test_seams.hpp`)
reads `derived_count`, `authoritative_pending`, `live_routes`,
`prepared_controls` and `submitted_controls` inside one `dispatch_mtx_`
critical section. Concurrent parity monitoring uses this snapshot only —
sampling the derived count and the authoritative scan through two separate
locked getters compares two different instants and can misclassify a legal
transition as a parity failure. The snapshot is internal-test-only
(`SLUICE_ASYNC_INTERNAL_TESTING`), not public API.

## Mutation gate (Gate R3)

Registered builds under `xmake/tests.lua`; each must die on its named oracle
(exit 1 with the named message, never an incidental abort/timeout):

| Mutant | Define | Named kill |
|---|---|---|
| missing increment | `SLUICE_URING_MUTANT_PENDING_CONTROL_MISSING_INCREMENT` | parity oracle at `sticky_lifecycle`: derived 0 vs scan 1 |
| missing decrement | `SLUICE_URING_MUTANT_PENDING_CONTROL_MISSING_DECREMENT` | parity oracle at `sticky_lifecycle`: derived 1 vs scan 0 after issue |
| gate always skips | `SLUICE_URING_MUTANT_PENDING_CONTROL_GATE_ALWAYS_SKIPS` | protocol oracle: released pressure did not issue the pending control SQE (parity still holds — this kill isolates liveness, not parity) |
| double increment | `SLUICE_URING_MUTANT_PENDING_CONTROL_DOUBLE_INCREMENT` | parity oracle: derived 2 vs scan 1 under repeated cancel/pressure |
| retire stale count | `SLUICE_URING_MUTANT_PENDING_CONTROL_RETIRE_STALE_COUNT` | parity oracle at `repeated_cancel`/retire: derived 1 vs scan 0 after route retirement |

Slot-reuse identity isolation (task counterfactual 6) is discriminated by the
`retire_and_slot_reuse_oracle` assertions: the reused slot presents a fresh
cookie (`third_cookie != identity`), zero derived count and zero
authoritative pending — an inherited identity or stale count fails the named
assertions deterministically.
