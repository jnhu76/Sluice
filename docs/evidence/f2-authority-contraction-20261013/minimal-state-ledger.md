# F2 Minimal State Ledger — Request/Result/Identity authority contraction

Baseline master: `0ed16588b7101f108aa7282074ba3d568f9d34e9` (F3 and F4 closed).
Slice: clean-break removal of the caller-owned Completion authority, the
RequestHandle responsibility mixing, and the residual RequestArena vocabulary.
Implementation head: the branch head recorded in `gates.json` in this directory.
Measured deltas in the installed package: 56 → 49 installed headers, 35 → 32
library objects (2 core + 33 → 30 async), 33 → 30 `libsluice_async.a` members,
417 previously defined async symbols removed and 57 added (debug, no-ring,
`nm -C --defined-only`).

## Removed state and mechanisms (no surviving semantic fact)

| Removed mechanism | Why it had no independent authority |
|---|---|
| `Completion<T>::state_` (idle/binding/outstanding/publishing/ready/resetting) | A second settlement state machine over the same slot. `RequestCore` already owns acceptance, terminal arbitration, publication and public binding; the Completion state only mirrored core facts for a caller-owned destination. |
| `Completion<T>::storage_` (+ `Completion<void>` error cell) | Duplicate result storage. The canonical result lives in the slot until `consume_public_result`; `Request<T>::try_result/take_result` are the only public readers. |
| `Completion<T>::release_arena_/bound_slot_` | Arena-side binding path. No `RequestArena` instance existed at baseline; pure residual vocabulary. |
| `Completion<T>::release_core_/core_key_` | Duplicate of the `Request<T>` binding responsibility over the same `release_public_binding` edge. |
| `Completion<T>::reap_seq_` + `detail::next_reap_seq()` global counter | Order bookkeeping with no canonical contract (submit order is not publish order); died with Batch. |
| `RequestHandle` (context/slot/generation + valid) | Second public identity spelling. `RequestId` (copied identity, `lookup`/`cancel`) already covers every legal use. |
| `RequestHandleState` + `resolve_identity_state` + `request_state` + `supports_request_identity` | Mapping layer from `PublicLookup` to a second enum; `RequestReadiness` + `lookup` is the one authority. |
| `RequestArena` whole state machine (8 request states, ready ring, enqueue pin, submit seq, capacity counters, publication binding, borrow flag) | Dead at baseline: no instantiation. Its obligations (acceptance pin, borrow window, publication ordering, generation bump) are owned by `RequestCore` slots. |
| `RequestSlot`/`TerminalResult`(arena)/`CompletionBinding`/`BorrowMetadata`/`EnqueueOutcome`/arena `CancelDisposition` | Arena vocabulary only. `TerminalResult` in the uring backend is replaced by `IoOutcome` (canonical terminal carrier); `SlotHandle` collapses into `RequestReservation`. |
| `submit_transaction.hpp` reserve/prepare/commit template | Unreachable template over the dead arena; the live acceptance transaction is `RequestCore::reserve/accept/rollback`. `SubmitStage` (test-injection vocabulary) moves to `request_core.hpp`. |
| `Batch` (`slots_`, per-slot `Completion` pair, ready/popped bits, `reap_seq` harvest) | Aggregator over the removed authority; no canonical contract depends on its completion-order harvest (its only consumer re-expressed on `wait_one`). |
| `op_helpers` busy-poll (`while (!c.ready()) ctx.poll()`) | Idle spin existed only to drive caller-owned Completions; the canonical wait/drive family is `wait_one`/`poll_progress`/Request readiness. |
| `DeliveryRecord{completion, publish}` (both backends) + `publish_*_ready` thunks | Caller-destination plumbing inside the publication window. Publication is core-only; the record keeps only `kind`, `event_owed`, `owed_key` (delivery accounting that pairs with a core control ref). |
| 16 arena/completion fail-fasts | Diagnostics of the removed state machines. |
| `WaitRecord::completion` back-pointer (Scheduler) | Never matched on; the wait registry is keyed by `RequestKey`. |

## Retained state and its justification

| Retained state | Sole authority | Why it must exist |
|---|---|---|
| `RequestCore::Slot` fields (phase/generation/accepted/binding_live/terminal_chosen/published/publication_inflight/execution_claimed/cancel_intent/observer_phase/execution_refs/control_refs/outcome…) | `RequestCore` | The canonical acceptance/terminal/publication/reclaim state machine; each field guards a legal interleaving exercised by the protocol suite. |
| `Request<T>` `{core_, backend_, key_}` binding | `Request<T>` | The single public settlement responsibility; release edge is `discard_public_result`. |
| `RequestId` copy | caller | Permits lookup/cancel from a copied identity; never owns settlement. |
| backend `DeliveryRecord{kind, event_owed, owed_key}` | backend | Physical event accounting: `event_owed` pairs with one core control ref so a published-but-undelivered ready event cannot be dropped (no-lost-wake). |
| backend dispatch rings (`BoundedHandleRing`/`publication_pending_`) keyed by `RequestReservation` | backend | Physical dispatch/publication queues for kernel/threadpool execution; no result payload, no settlement. |
| `WaitRecord{request_key, state, fiber, owner}` (Scheduler) | Scheduler | Fiber-wake obligation keyed by slot+generation; delivery claim/retirement rides the core observer protocol. |
| `SubmitStage` enum (testing-only) | test injection | Names the pre-commit injection points of the canonical acceptance transaction. |

## Public-surface deltas (intentional breaking changes)

- Removed public headers: `sluice/async/completion.hpp`, `sluice/async/request_handle.hpp`,
  `sluice/async/batch.hpp`, `sluice/async/op_helpers.hpp`,
  `sluice/async/detail/request_arena.hpp`, `sluice/async/detail/request_slot.hpp`,
  `sluice/async/detail/submit_transaction.hpp`.
- Removed APIs: `AsyncIoContext::submit_*(Op, Completion&)`, `submit_*_request`,
  `cancel(Completion&)`, Completion-typed `attach_observer`/`cancel_observer`,
  `request_state(RequestHandle)`; `AsyncBackend::cancel(Completion&)`,
  `identity_of`, `resolve_identity_state`, `supports_request_identity`.
- Canonical replacements: `submit_*(Op) -> Request<T>`, `cancel(RequestId)`,
  `attach_observer(RequestId)`, `cancel_observer(RequestId)`, `lookup(RequestId)`,
  `Scheduler::await_request(RequestId)`, `Scheduler::cancel_waiter(RequestId)`.
- Removed compiled objects: `src/async/batch.cpp`, `src/async/op_helpers.cpp`,
  `src/async/request_handle.cpp`.

## Authority-contraction audit

Authority that moved in this slice: public settlement from caller-owned
`Completion<T>` to `RequestCore` + `Request<T>`; copied identity from
`RequestHandle` to `RequestId` + `AsyncIoContext::lookup`; slot lifecycle from
the arena template family to `RequestCore::Slot`; waiting from `Batch` and the
busy-poll helpers to `wait_one` / `RequestScope` / explicit progress.

State, guards, counters and capability representations that became redundant
*as a consequence* of that move, and are therefore deleted:

| Redundant after the move | Why it can no longer disagree with anything |
|---|---|
| `Completion<T>::state_` (six states), `storage_`, `bound_slot_`/`release_arena_`, `release_core_`/`core_key_` | Private mirror of core acceptance/terminal/publication/binding facts for a second destination that no longer exists |
| `Completion<T>::reap_seq_` + `detail::next_reap_seq()` | Order bookkeeping for the removed `Batch` harvest; submit order was never a publish-order contract |
| `RequestHandleState`, `resolve_identity_state`, `request_state`, `supports_request_identity`, `identity_of`, the `AsyncBackend::supports_request_identity` vtable slot | A second mapping from `PublicLookup` to an enum; `RequestReadiness` and `lookup` are the one authority |
| Whole `RequestArena` state set (8 request states, ready ring, enqueue pin, submit sequence, capacity counters, publication binding, borrow flag), `RequestSlot`/`SlotState` | Never instantiated at the baseline commit; every obligation it named is owned by `RequestCore::Slot` |
| `submit_transaction` reserve/prepare/commit template | Unreachable template over the dead arena; the live transaction is `RequestCore::reserve/accept/rollback` |
| `Batch::slots_`, per-slot `Completion` pair, ready/popped bits, harvest, `BatchResult`, `BatchResultOrigin` | Aggregator over the removed authority; its only consumer is re-expressed on `wait_one` |
| `op_helpers` busy-poll loops | Idle spin over caller-owned Completions; the canonical wait family is `wait_one`/`poll_progress`/request readiness |
| `WaitRecord::completion`, delivery-record `completion`/`publish` fields, `publish_*_thunk` instantiations | Caller-destination plumbing inside the publication window; publication is core-only |
| 16 arena/completion fail-fasts | Diagnostics of the removed state machines |
| `RequestCore::release_public_binding` (retired during this slice's review) | A second release edge over the same `binding_live` state: it accepted a release while `publication_inflight`, which is the removed caller-owned `Completion`'s cross-thread visibility rule. Only `discard_public_result` is reachable from the public settlement responsibility, and the slot reclamation predicate still requires zero execution and control references. |

Audited as apparently redundant but **retained**, because each protects a real
legal interleaving, lifetime boundary or physical obligation:

| Retained mechanism | The obligation that makes it independent |
|---|---|
| `execution_refs` next to `control_refs` | They retire on different edges: the borrow touch (physical execution finished) versus the delivery event (ready event owed to the host). Collapsing them would let a reclaimed slot still own an owed event. The `ObserverCore` and `ShutdownCore` models carry this separation. |
| `publication_inflight` next to `published` | The acquire/release window in which the result must stay invisible; `try_result()` reports pending while it is set. A single `published` bit would expose a partially written payload. |
| `binding_live` | The public settlement responsibility outlives publication until consume/discard; `published` alone cannot say whether the caller still owns it. |
| `cancel_intent` | Records that a cancellation was requested on the slot before a terminal was chosen. It arbitrates nothing (`offer_terminal` and the reclaim predicate never read it), and its only readers are the protocol seam (`observe_slot`) and the protocol/cutover suites, so it is protocol-observability state rather than an independent authority. Retirement candidate once that seam retires. |
| Backend `event_owed`/`owed_key` | The backend's physical accounting for one owed ready event paired with one control ref (no lost wake). Not derivable from core state, which does not observe the kernel/worker event. |
| io_uring `fatal_error_` and `RequestCore::health_failed_` | Two independent authorities (ring poison, core health verdict) composed at the pass level; neither derives from the other and each has a distinct producer. The ThreadPool backend has no poison state of its own: it reports health only through the core verdict. |
| `Scheduler` wait registry keyed by `RequestKey` | A fibre-wake obligation that is not derivable from request state: a request can be terminal while its waiter has not been armed or resumed. |
| Notification epoch/exhaustion pair | Freshness authority for wakes; the acknowledgement drains the fd, and the epoch is what revalidates a stale readiness (`ProgressSource` model). |
| `claim_execution`/`acquire_execution`/`release_execution` | The borrow-touch accounting itself: the accepting transaction takes the first execution reference and the executor side takes and retires further ones, with retirement required before publication is eligible. The extra-acquire entry point has no production caller (production acquires inside `accept`), but it models a second executor on the same slot for the protocol and cutover suites, so it is protocol-test surface rather than dead vocabulary. |
| `SubmitStage` (testing only) | Names the pre-commit injection points of the canonical acceptance transaction; test instrumentation, not runtime state. |

Added in this slice: no new runtime state, flag, counter, token, lease,
capability probe or defensive guard. The only new declarations are the canonical
replacements for removed spellings (`RequestObservation`, `Request::key_for_test`
under the testing macro, `RequestId`-based observer/scheduler entry points).

Evidence binding: this ledger describes the branch head recorded in `gates.json`
in this directory; the package counts above come from the B-01 v5 capture in
`b01-v5-candidate/`.
