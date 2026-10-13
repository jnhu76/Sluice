# F2 authority contraction — execution record

Owner slice: #402 F2 (Request/Result/Identity authority contraction, clean break).
Baseline master: `0ed16588b7101f108aa7282074ba3d568f9d34e9`.
Worktree: independent worktree `/home/jnhu/Source/Sluice-f2`, branch
`f2-clean-authority`. The original `master` worktree was not modified.

## Commit structure

1. `refactor(async)!: key-canonical scheduler await and observer attach` —
   Scheduler `await_request(RequestId)`/`cancel_waiter(RequestId)`;
   `AsyncIoContext::attach_observer(RequestId)`/`cancel_observer(RequestId)`;
   `runtime_waiter_observer_test` migrated; the WaitRecord completion
   back-pointer removed.
2. `refactor(async)!: retire Batch and the busy-poll op helpers` — batch.hpp/.cpp,
   op_helpers.hpp/.cpp deleted; the two carried invariants re-expressed on
   `wait_one` (sticky health verdict through the `health_failure` outcome with
   the claimed owner released) and zero-length submission (settles at
   submission, no driving demand).
3. `refactor(async)!: remove the caller-owned Completion authority and residual
   arena vocabulary` — completion.hpp, request_handle.{hpp,cpp},
   request_arena.hpp, request_slot.hpp, submit_transaction.hpp deleted;
   backend vtable is core-only (`submit_*(Op) -> Result<RequestKey>`);
   Completion-typed context/backend spellings removed; `TerminalResult` in the
   uring chain replaced by `IoOutcome`; `SlotHandle` collapsed into
   `RequestReservation`; `SubmitStage` moved to request_core.hpp (testing);
   16 arena/completion fail-fasts removed; all canonical consumers migrated.

## Requirement mapping (MIG-02 → this slice)

| Root disposition | Executed |
|---|---|
| caller-owned `Completion<T>`: compatibility only, remove as canonical result authority | Removed entirely; `CALLER_OWNED_COMPLETION_CANONICAL_AUTHORITY = 0` |
| `RequestArena`: converge to bounded core | No instance existed; residual vocabulary deleted; `ONE_CANONICAL_SLOT_LIFECYCLE_AUTHORITY` holds in `RequestCore` |
| `RequestHandle`: separate public move-only responsibility from internal copied identity | Handle spelling removed; `Request<T>` is the only move-only settlement responsibility, `RequestId` the only copied identity |
| busy-poll helpers: replace with explicit Request/progress/scoped paths | op_helpers deleted; carried invariants re-expressed on canonical paths |
| Scheduler/Fiber substrate | retained (adopted HOLD); its await family migrated to the canonical identity spelling |

## Consumers migrated

Apps: none needed (sluice-copy already RequestScope/Request-based; other apps
core-only). Scheduler await family, `runtime_waiter_observer_test`,
`threadpool_external_loop_test` (Batch + zero-work + compat cell),
`async_sync_admission_test`, `backend_conformance_e1_test`,
`file_access_precedence_test`, `progress_source_ownership_test`,
`request_core_ownership_test`, `semantic_path_probes` family,
`semantic_reference_case_test`, `shutdown_lifecycle_test`,
`stackful_host_test`, `threadpool/uring core_cutover` and `progress_race`
suites, `pipelined_copy_oracle_support` scripted backend, package consumers
`contract_observer` (RequestId-based attach) and the F4 `retained` probe
(RequestId replaces the Completion instantiation), W-01 probes.

## Remaining F2 compatibility surfaces

None in-tree. External/installed surface break is recorded through the
versioned B-01 comparison in this directory (see `b01-f2-v5/`).
