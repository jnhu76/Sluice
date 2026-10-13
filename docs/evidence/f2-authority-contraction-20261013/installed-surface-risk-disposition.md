# F2 installed-surface risk and disposition record — #402

Status: technical candidate; human owner decision **PENDING**. External use
UNKNOWN for every row.

Measured implementation head: the branch head recorded in `gates.json` in this directory
(branch `f2-clean-authority`, draft PR #516). Execution baseline:
`0ed16588b7101f108aa7282074ba3d568f9d34e9` (F3 and F4 closed).
Draft PR #516 remains unmerged: the clean-break direction is not by itself the
separate approval the root requires for an external public/installed API break.

Authority: root v1-r4 MIG-01/MIG-02/MIG-03, PROD-03, GOV-02. F0 §1.7
deprecation procedure. Prior adopted dispositions this slice executes:
MIG-02 rows for `Completion<T>`, `RequestArena`, `RequestHandle` and the
busy-poll helpers. F0 §1.7 principle applied here: historical pre-v1 published
interfaces require deliberate-deprecation disclosure, and unknown external use
is not zero use.

## What breaks

Seven installed headers disappear and three compiled objects leave
`libsluice_async.a`. The removal is a source and a relink break for any external
consumer of the removed entry points; an already linked static executable keeps
its copied code and does not change behaviour.

Installed-surface delta, measured in the B-01 v5 clean-room capture
(`b01-v5-candidate/summary.json`, both ring profiles):

| Metric | Before (`0ed16588`) | After (F2 head) |
|---|---|---|
| Installed headers | 56 | 49 |
| Library objects | 35 (2 core + 33 async) | 32 (2 core + 30 async) |
| `libsluice_async.a` members | 33 | 30 |

Removed archive members, by name: `request_handle.cpp.o`, `op_helpers.cpp.o`,
`batch.cpp.o`. Exported-definition delta on `libsluice_async.a`
(debug, no-ring profile, `nm -C --defined-only`): 417 previously defined symbols
disappear and 57 new definitions appear. The removed set is dominated by
instantiations of the retired authority (`Completion<T>`, `Result<Completion<T>>`,
`Result<RequestHandle>`, `Batch::Slot`), not by shared infrastructure; the added
set is the canonical replacement surface (`submit_request<T>`,
`attach_observer(RequestId)`, `cancel_observer(RequestId)`, `await_request`,
`cancel_waiter(RequestId)`).

## Per-surface rows

| Surface | Header or object | Retired API | Migration or no equivalent | Proposed policy | Owner |
|---|---|---|---|---|---|
| F2-COM | `sluice/async/completion.hpp` | `Completion<T>`, `Completion<void>` error cell, the six-state settlement machine, caller-owned result storage, `submit_*(Op, Completion&)`, `submit_*_request`, `cancel(Completion&)`, `begin_binding`/`install_core_binding`/`commit_binding`, `next_reap_seq` | `submit_*(Op) -> Result<Request<T>>`; readiness through `Request::ready()/try_result()/take_result()`, release through `Request::discard()`, cancel through `Request::cancel()` or `AsyncIoContext::cancel(RequestId)` | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F2-HND | `sluice/async/request_handle.hpp` | `RequestHandle`, `RequestHandleState`, `request_state(RequestHandle)`, `resolve_identity_state`, `supports_request_identity`, `identity_of` | `RequestId` plus `AsyncIoContext::lookup(RequestId) -> RequestReadiness`; `Request<T>::id()` | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F2-BAT | `sluice/async/batch.hpp` | `Batch`, `BatchResult`, `BatchResultOrigin`, per-slot `Completion` pairs, ready/popped bits, `reap_seq` harvest, `await_one` | `wait_one`, `poll_progress`, `RequestScope`, per-request readiness; submit order was never a publish-order contract | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F2-OPH | `sluice/async/op_helpers.hpp` | `read_all`, `write_all`, `sync_data_all`, `sync_all_all` (busy-poll drivers over caller-owned Completions) | explicit submit plus `wait_one`/`RequestScope` loops; the two carried invariants (sticky health verdict through the `health_failure` outcome, zero-length submission settles at submission) are preserved on the canonical path | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F2-ARN | `sluice/async/detail/request_arena.hpp` | `RequestArena` whole state machine (8 request states, ready ring, enqueue pin, submit sequence, capacity counters, publication binding, borrow flag) | none needed: no instantiation existed at the baseline commit; its obligations are owned by `RequestCore` slots | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F2-SLT | `sluice/async/detail/request_slot.hpp` | `RequestSlot`, `SlotState`, arena slot vocabulary | `RequestCore::SlotPhase` and `RequestReservation{SlotIndex, Generation}` | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F2-TRX | `sluice/async/detail/submit_transaction.hpp` | reserve/prepare/commit transaction template over the dead arena | `RequestCore::reserve/accept/rollback`; the `SubmitStage` test-injection vocabulary moves to `detail/request_core.hpp` under `SLUICE_ASYNC_INTERNAL_TESTING` | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F2-OBJ | `src/async/request_handle.cpp`, `src/async/batch.cpp`, `src/async/op_helpers.cpp` | three compiled objects and their archive members | none: their headers are removed as well | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F2-SCH | `sluice/async/scheduler.hpp` | `Scheduler::await_completion_size`, `await_completion_void`, `cancel_waiter(Completion<T>&)` | `await_request(RequestId)`, `cancel_waiter(RequestId)` | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F2-OBS | `sluice/async/async_io_context.hpp` | `attach_observer(Completion<T>&)`, `cancel_observer(Completion<T>&)` | `attach_observer(RequestId)`, `cancel_observer(RequestId)` | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F2-BOP | `sluice/async/batch.hpp` | `BatchOp` and its `Kind`/payload members, `Batch::add`, `Batch::next`, `Batch::pending_count` | none: the aggregator is retired; per-request readiness plus `wait_one`/`RequestScope` | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F2-FF | `sluice/async/detail/fail_fast.hpp` | the 16 retired `[[noreturn]]` diagnostics (3 completion, 1 slot, 12 arena) | none: they described the removed state machines. The retained diagnostics, including `request_nonterminal_release_fail_fast` and `request_binding_invariant_fail_fast`, are unchanged | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F2-RLB | `sluice/async/detail/request_core.hpp` | `RequestCore::release_public_binding` (retired during review) | `RequestCore::discard_public_result`, the one release edge the public settlement responsibility uses | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |

## Negative probes binding the break

Five probe sources are written against the removed entry points. Each compiles
against the **before-headers** (placed first on the include search path so every
nested include resolves inside the old set) and then links against the **F2
library**; the link step is the assertion
(`b01-v5-candidate/raw-package-evidence.tar.gz`, `after-evidence-*/old-entry-*.log`):

| Probe | Before (`0ed16588`) | After (F2 head) | Undefined symbol |
|---|---|---|---|
| `completion-submit` | compiles, links, runs | compiles, link fails | `AsyncIoContext::submit_read(ReadOp, Completion<unsigned long>&)` |
| `completion-submit-request` | compiles, links, runs | compiles, link fails | `AsyncIoContext::submit_read_request(ReadOp, Completion<unsigned long>&)` |
| `identity-state` | compiles, links, runs | compiles, link fails | `AsyncIoContext::request_state(RequestHandle const&) const` |
| `batch-await` | compiles, links, runs | compiles, link fails | `Batch::await_one(AsyncIoContext&)` |
| `op-helpers` | compiles, links, runs | compiles, link fails | `read_all(AsyncIoContext&, NativeFileRef const&, std::span<std::byte>, unsigned long)` |

No compatibility shim, deprecated alias or forwarding wrapper is added: a shim
would keep the retired authority compiled into the library and would let a
consumer obtain a settlement object this slice deliberately removed.

## Retained consumers

W-01, W-02, W-03 and the retained Scheduler/Fiber/host/Future closure compile,
link and run from the installed prefix in both ring profiles
(`b01-v5-candidate`, consumer rows). Every retained public header's dependency
closure loses only removed headers and gains none, so no retained consumer
acquires a dependency on the contraction.

## What this record does not claim

It does not claim zero external users, it does not grant
`SAFE_TO_UNINSTALL`/`SAFE_TO_DELETE_FROM_TREE`, and it does not authorise the
merge. Under the root's MIG-02 negative-scope boundary this is a deliberate
break of a pre-v1 published surface: it requires the explicit owner
external-use/deprecation decision before the slice merges, and it cannot be
waived by the negative-scope exception because `Completion`/`RequestHandle`
holdings were an admitted v1 workload surface, not an unadmitted capability.
