# F2 Minimal State Ledger — Request/Result/Identity authority contraction

Baseline master: `0ed16588b7101f108aa7282074ba3d568f9d34e9` (F3 closed).
Slice: clean-break removal of the caller-owned Completion authority, the
RequestHandle responsibility mixing, and the residual RequestArena vocabulary.

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
