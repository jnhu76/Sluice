# F0 Consumer Graph & Migration DAG (#454, parent #402)

- **Status**: `CONSUMER_MIGRATION_DAG_DEFINED` candidate. Revision 2 resolves Round-1 human-review findings P2-2 (X-10 wording) and P2-3 (X-18 out of F4 prerequisites); record: `f0-role-profile-census.md` §4.1.
- **Baseline**: `989d5c3fa5fa803c05ac87ebfe6c99db9d87564b`, root v1-r3 (provenance: `f0-role-profile-census.md` §0)
- **Rule**: this file only builds the graph. It migrates nothing, retires nothing, and does not re-interpret adopted E1/E2/host semantics. Edge kinds are typed; absence of an edge type is recorded, not assumed.

## 1. Edge vocabulary (`EDGE_KIND`)

| Kind | Meaning |
|---|---|
| `CALL` | invokes the surface at runtime |
| `COMPILE` | names the surface in a signature/template/friend declaration |
| `TYPE_LAYOUT` | embeds the surface by value / inherits it (layout exposure) |
| `TRANSITIVE_INCLUDE` | pulls the surface in via another header |
| `NAME_ONLY` | mentions without compile dependency (docs aside) |
| `DOC_CLAIM` | a current document asserts behavior/authority of the surface |
| `TEST` | test-only consumer (evidence edge) |
| `APP_EXAMPLE` | shipped application consumer |
| `FORMAL` | formal model claims correspondence |
| `BUILD_PACKAGE` | build rule membership/flag propagation |

## 2. Mandatory cross-family edges (F0-C deep checks, #454 §7)

| # | Edge (CONSUMER → OLD_SURFACE) | KIND | OBSERVABLE_DEPENDENCY / evidence |
|---|---|---|---|
| X-01 | legacy host world → `Completion<T>` | COMPILE + CALL + TYPE_LAYOUT | consumer set (include-count vs type-use vs app-use are different numbers): 15 test files include `completion.hpp`; **25 test files name the type**; **all 4 apps construct/settle `Completion` objects in task code** (`apps/sluice-copy/copy_task.cpp:37-38,55`, `apps/sluice-hash/hash_task.cpp:43`, `apps/sluice-grep/grep_task.cpp:45`, `apps/sluice-tail/tail_task.cpp:112-233`); `Batch::Slot` holds `Completion` by value; the compat result carrier is baked into every legacy spelling of submission — retiring it test-only would break all four app builds |
| X-02 | `Completion<T>` (R-07 release path) + `AsyncBackend::identity_of` → `RequestArena` | COMPILE + CALL | `completion.hpp` includes `detail/request_arena.hpp`; release may still route `release_completed_binding` when the legacy binding is installed, and `request_handle.cpp:8-42` derives a `RequestHandle` from arena fields when no core binding is installed — the arena cannot be removed before both paths are re-owned |
| X-03 | `StackfulIoHost` (public header) → `Fiber`/`fiber_ctx` | TRANSITIVE_INCLUDE + TYPE_LAYOUT (`TaskSlot::fiber` by value; `fiber_ctx::Context driver_ctx_`; `fiber_ctx::Switch*` param) | **public-header layout dependency**: F3 internalization must re-spell this header first (PIMPL/opaque slot) or every `stackful_io_host.hpp` consumer breaks. This exposure is a first-class OPTIONAL_CANDIDATE profile row (census H-31) and, with the `CancelToken` signature dependency `IoTaskContext::token()` (census H-30), part of the clean-room optional-host package closure until this fix lands |
| X-04 | `ApplicationRuntime` (public header) → `Scheduler`/`SchedulerWakeHandle` | TRANSITIVE_INCLUDE + TYPE_LAYOUT (`unique_ptr<Scheduler>`, `SchedulerWakeHandle` by value) | same class of exposure for the runtime world |
| X-05 | `await_op_helpers` / `RuntimeTaskContext` → `Scheduler`/runtime | TRANSITIVE_INCLUDE (`await_op_helpers.hpp` → `application_runtime.hpp` → `scheduler.hpp` → `fiber*.hpp`) | helper family cannot outlive the runtime world; migration order: helpers before runtime |
| X-06 | `FileReader`/`FileWriter` → canonical `File` semantics | **NO EDGE (negative finding)** | they define open/close/move/sync independently on raw `fd_` (`src/file.cpp`); they share the *oracle* (`detail/file_semantics.hpp`) but not the *resource authority*; additionally they hold the repo's **only native vectored implementation** (`read_vec/write_vec` via `readv`/`writev`) and the only native `Writer::write_all_vec` — flagged as second-authority candidate + expressiveness gap, decision owned by the audit phase |
| X-07 | `IoContext`/`BlockingIoContext` factory → `FileReader`/`FileWriter` | COMPILE + CALL | the factory is the only production CALL consumer of L-01; apps do **not** include `sluice/file.hpp` (GREP: 0 app hits) |
| X-08 | apps → legacy/host APIs | APP_EXAMPLE | all 4 apps include `application_runtime/task_result/await_op_helpers/async_io_context/threadpool_backend/file_resource/error/result`; `async/file.hpp` is included by **sluice-grep, sluice-hash, sluice-tail** (3 apps); `blocking/file.hpp` by **sluice-tail only**; sluice-copy includes neither; sluice-copy additionally includes **`detail/posix_retry.hpp`** (`safe_output.cpp:3`, X-13); no app includes `completion.hpp`, `sluice/file.hpp`, or scheduler headers directly |
| X-09 | apps → ApplicationRuntime → Scheduler/Fiber/Group | TRANSITIVE_INCLUDE (+ CALL via `task_result.hpp::run_task_to_result`) | the four apps are the anchor consumers of the multi-worker runtime; retiring Scheduler before re-homing apps is unsound |
| X-10 | docs → retired/compat vocabulary | DOC_CLAIM | `README.md`/`README.zh-CN.md` name caller-owned `Completion`, `AsyncIoContext`, Scheduler/Fiber as *retained baseline* (correctly labeled, keep); ADR-0002 (superseded) is the only doc describing `BlockingIoPool`/`MemoryIoContext` semantics; ledger rows own the current vocabulary. **No current document makes a normative source/ABI compatibility promise** (policy §1.5/§1.6) — but current **non-normative product/docs claims do exist** and are real DOC_CLAIM evidence that must migrate with their surfaces: the README retained-baseline framing itself, and the app READMEs' ApplicationRuntime-lifecycle and "installed/public headers only" claims (X-18). |
| X-11 | formal → implementation surface | FORMAL | active models map to `RequestCore`/observer/progress/shutdown machinery only (`RequestCore.tla`, `ObserverCore.tla`, `ProgressSource.tla`, `ShutdownCore.tla`); none maps to `Completion`, `Scheduler`, or the legacy stream world (formal map §8) |
| X-12 | `SLUICE_HAS_LIBURING` → public ODR surface | BUILD_PACKAGE | public define + public link on `sluice_async`; every consumer TU must see the same guarded layout (census §1.5) — any package rule must carry it |
| X-13 | `apps/sluice-copy/safe_output.cpp` → `sluice/detail/posix_retry.hpp` | TRANSITIVE_INCLUDE (direct) | app consuming an INTERNAL header directly — recording only; future home is a canonical spelling or the app's own helper |
| X-14 | tests → `SLUICE_ASYNC_INTERNAL_TESTING` + `src/async` include dir (14 test-target declarations) | BUILD_PACKAGE + TEST | seam access is load-bearing for current evidence; internalizing any seam surface requires seam migration in the same slice |
| X-15 | `Scheduler` → core progress/ready protocol | COMPILE (`ReadyRoutingSink : detail::SynchronousReadySink`) | the runtime consumes the canonical ready-routing contract; it adds no second progress authority (C1/C2 evidence) |
| X-16 | `Scheduler`/`RuntimeTaskContext`/`Fiber` → `Completion<T>` (await machinery) | COMPILE + CALL | `scheduler.hpp:4` includes `completion.hpp`; public `await_completion_size/void(Completion&)` + `cancel_waiter(Completion&)` (`scheduler.hpp:153-163`), implemented in `scheduler_park_wake.cpp:415-504`; `RuntimeTaskContext::await_completion/cancel_waiter` (`application_runtime.hpp:42-46`) forward to them (`application_runtime.cpp:55-73`); `Fiber` carries `CompletionWaitOutcome` (`fiber.hpp:22,59-68`); the await helpers (`await_op_helpers.cpp:8`) and the apps' main loops run through this path — the Completion-retirement chain and the Scheduler-retirement chain **intersect here** and must state their relative order |
| X-17 | `Group`/`AsyncMutex`/wait vocabulary → `Fiber` (complete type) | COMPILE + TYPE use | `group.hpp:4` includes `fiber.hpp`; `evented_fibers_` = `vector<unique_ptr<Fiber>>` (`group.hpp:129`), `make_unique<Fiber>()` + `init_fiber` (`:147,157`); `async_mutex.hpp:6,45` includes `fiber.hpp`, holds `Fiber* owner_`; `wait_node.hpp`/`detail/select_port.hpp` name `Fiber*` (covered via the Scheduler node). Consequence: **F3's tail step (`fiber*.hpp` become internal) is unreachable until Group retires** — X-03's "only by-value embedding" claim stays true, but complete-type compile edges block internalization |
| X-18 | app READMEs → host/Completion machinery | DOC_CLAIM | `apps/sluice-copy/README.md:4-6,247-259` documents `RuntimeTaskContext::await_completion` + "strict Completion lifetime discipline" and references a nonexistent `docs/history/implementation-plans/...` path; `apps/sluice-{hash,grep,tail}/README.md` claim "ApplicationRuntime lifecycle usable" and "installed/public headers only" ("installed" is currently inaccurate — census FA-1). These DOC_CLAIM edges must migrate with the surfaces they describe |

## 3. Consumer edges by family (summary, from the include graph)

Full per-header counts are regenerable via `scripts/f0_surface_inventory.py`. Anchors:

- `async_io_context.hpp` (hub): 4 apps, 18 test files, 2 src TUs, 9 headers — every migration touches it.
- `application_runtime.hpp`: 4 apps, 1 test, 1 src, 3 headers (incl. `task_result.hpp`, `await_op_helpers.hpp`, `async/file.hpp`).
- `threadpool_backend.hpp`: 6 app TUs (all four apps), 19 test files.
- `completion.hpp`: 15 direct-include test files; 25 type-using test files; 4 app CALL/TYPE_LAYOUT consumers; Scheduler/RuntimeTaskContext await machinery (X-16).
- `request.hpp` (canonical `Request<T>`): 5 direct test files + every hub consumer transitively.
- `sluice/file.hpp` (legacy): 2 src-core TUs, 6 test files (4 of them named ledger oracles). 0 apps.
- dormant wrappers: condition/semaphore/async_queue 0 direct includes; async_mutex 1 (condition.hpp); **async_rwlock 10 src TUs (mechanical)**.

## 4. F0-D — Replacement / adoption graph

`MIGRATION_STATE` uses: `ADOPTED` (ledger VERIFIED or ADOPTED ADR), `RECORD` (named evidence record exists; ledger aggregate row open or human review pending), `PARTIAL` (replacement exists and is canonical for new paths; legacy spellings still live), `PROPOSED`, `HISTORICAL` (nothing to do).

| OLD_ROLE | ADOPTED_REPLACEMENT | REPLACEMENT_SHA (adoption evidence) | ROOT_AUTHORITY | EXPRESSIVENESS_DIFFERENCE | MIGRATION_STATE |
|---|---|---|---|---|---|
| Backend waiter vocabulary (`WaiterToken`, `RoutingLease`, `BackendWaitSource`) | context-owned `ProgressSource` + `BackendProgressPort`; OBS registration protocol | C1-E removed legacy waiters; C2-A cutover (ledger C1/C2 records, VERIFIED) | ARCH-01, PROG-01..03, OBS-01/02 | backend loses waiter-domain authority; gains narrow signal + one-shot registration only | **ALREADY_MIGRATED** (names no longer exist; census §2.3) |
| Backend-owned request bookkeeping | context-owned `RequestCore` (acceptance/terminal/publication/reclaim) | B1-A/B1-C cutovers; B1-D seam collapse (records; aggregate ledger rows open) | REQ-01..06, ARCH-02 | core owns arbitration + public binding; backend only executes | RECORD/PARTIAL (arena retained solely as Completion release + identity fallback — X-02) |
| Caller `Completion<T>` as result authority | public `Request<T>` binding over core | B2 record (implemented; ledger HANDLE row open, final review pending) | ARCH-02, HANDLE-01..03, MIG-02 | move-only unique responsibility vs caller-owned storage; observation adds `EffectReport` | PARTIAL: canonical for new paths; compat spellings + backends' publish-into-Completion + the await machinery (X-16) still live |
| `RequestHandle`-style identity only | `RequestId` split (MIG-02-required capability, R-02) + `RequestHandle` spelling whose disposition is **UNDECIDED, F2-owned** (retain \| fold into `RequestId`/context lookup \| internalize — D-17) | implemented in B2/E1 records (RECORD, not VERIFIED) | REQ-01, MIG-02 | copied identity without result authority | RECORD (separation implemented; adoption state tracks D-02's identity-fallback precondition; final spelling decision deferred to F2) |
| busy-poll helpers | explicit Request/progress/scoped paths | MIG-02 row | INV-01, PROG | unbounded drive-spin vs bounded progress passes | **LIVE, covered by D-15**: `op_helpers` `one_step` spin loop exists today (`op_helpers.cpp:18-24,84-90`) — the MIG-02 row is not historical |
| `AsyncIoContext` vs root `IoContext` name | root `IoContext` maps to `AsyncIoContext` until F-phase rename | ADR-0004 §1 (ADOPTED) | ARCH-02, SHUT | naming only | PARTIAL (rename is an F-phase obligation, owned by #402) |
| General runtime (`ApplicationRuntime`/`Scheduler`/`Fiber`/`Group`) | narrow single-owner `StackfulIoHost` | D2 record (`IMPLEMENTED_UNVERIFIED`), ADR-0003 (**PROPOSED**) | HOST-01/02/03, MIG-02, PROD-02 | single-owner vs multi-worker; scheduler primitives demoted to internal substrate | PROPOSED/RECORD — **not adopted**; the narrow adapter is the OPTIONAL_CANDIDATE (PROD-02 optional build profile) while the multi-worker runtime is root-**DEFERRED** (PROD-02); W-04 gate pending human review |
| `Completion`-based await conveniences | host conveniences over `Request`-typed awaits | D2 (`read_exact`/`write_all` with `Result<CompositionOutcome>` on `StackfulIoHost`) | HOST-03, SEM-05 | every-exit settlement typing | PARTIAL (StackfulIoHost spelling proposed; `await_op_helpers`/`async/file.hpp`/`Scheduler::await_completion` still Completion-spelled) |
| Legacy `FileReader/FileWriter`/`IoContext` factory | canonical `File` + `blocking::` | A2 rows VERIFIED; the shared-oracle row A1 is `IMPLEMENTED_UNVERIFIED` (effect/durability halves remain reference-model) | SEM-01..07, LIFE, W-01 | shared oracle + single resource authority vs per-class fd ownership; **gap: no canonical vectored op** (X-06) | PARTIAL: canonical side complete for non-vectored ops; legacy side un-retired (X-06/X-07) |
| Experimental uring write path | canonical `UringAsyncBackend` | B1-C/E1 records | BACKEND, LIFE-03 | raw-fd path batch vs request-contract backend | replacement adopted; experimental surface orphaned (FA-2), disposition D-14 |

## 5. Consumer migration DAG

Nodes are consumer groups; an edge A→B means "B must settle before A can proceed". `🔒` marks a phase-prerequisite from the census exposure facts.

```text
Retire Completion<T> (D-01):
  [4 apps: task-code Completion members + await helpers] ─┐
  [tests using Completion (25 type users / 15 includers)] ─┤
  [Batch (1 test)] ────────────────────────────────────────┤
  [op_helpers → Request-typed + spin-loop removal (D-15)] ─┤
  [await_op_helpers / async/file.hpp re-typing (H-06/07)] ─┼─→ [drop Completion& overloads on
  [🔒 X-16: Scheduler await_completion/cancel_waiter +      │    AsyncIoContext / RuntimeTaskContext]
     RuntimeTaskContext + Fiber CompletionWaitOutcome] ────┤              │
  [ApplicationRuntime internal submits (H-08)] ────────────┤              ▼
  [backend publish path (H-02, both backends)] ────────────┘   [remove completion.hpp]
                                                     → [remove RequestArena/request_slot/
                                                        submit_transaction (D-02/D-03)]
  (relative order vs the Scheduler chain: the X-16 node may be satisfied either by
   re-typing the await machinery in place or by the runtime world retiring first;
   both orders converge, but the node itself is unavoidable)

Retire Scheduler/runtime (D-family around H-08/H-10):
  [4 apps off ApplicationRuntime/task_result] → [await helpers re-typed] →
  [ApplicationRuntime removal] → [Group/Future/WaitPolicy (H-13..15) removal] →
  [Scheduler + wait/select/timer/queue machinery internalize-or-retire]
  (runtime sync primitives H-17/H-18/H-22 retire with or before it)

Internalize Fiber (F3):
  [🔒 stackful_io_host.hpp boundary fix (X-03/D-16) — drops the OPTIONAL_CANDIDATE
     exposure rows H-30/H-31 from the clean-room host package closure] →
  [🔒 application_runtime.hpp boundary fix (X-04)]   →
  [🔒 X-17: Group retired AND AsyncMutex demoted (D-06) AND
     wait/select vocab internalized with Scheduler]  →
  [scheduler.hpp de-fiberization or internalization] → [fiber.hpp/fiber_ctx.hpp become internal]

Retire FileReader/FileWriter (D-09/D-10):
  [decide second-authority question (X-06)] →
  [decide vectored-IO capability: root amendment or accepted degradation] →
  [IoContext factory removal (X-07)] →
  [6 test files migrate to canonical File/blocking — 4 are named ledger oracles
   (semantic_range_test, request_core_ownership/public_request oracles,
   shutdown_lifecycle_test): re-run and re-record the affected ledger rows
   in the same slice] →
  [remove sluice/file.hpp legacy classes]
```

### Answers required by #454 §13

1. **If Completion is retired, who migrates first?** All four apps (their task code holds `Completion` members — X-01), the 25 type-using test files, and the internal producers: `op_helpers` (incl. spin-loop removal), `await_op_helpers`, `async/file.hpp`, `Batch`, `ApplicationRuntime` submit paths, the `Scheduler::await_completion`/`cancel_waiter` machinery and `Fiber::CompletionWaitOutcome` (X-16), and both backends' publish path; the arena release + `identity_of` fallback paths (X-02) must be re-owned before `detail/request_arena.hpp` can follow. `Request<T>` is the adopted target (`RequestScope` already shows the no-Completion pattern); `RequestHandle`'s final home is **UNDECIDED and F2-owned** (D-17) — its identity-fallback dependency (X-02) remains a migration dependency regardless of the spelling decision.
2. **If Scheduler is retired, must helpers/result family migrate first?** Yes: `await_op_helpers`/`async/file.hpp`/`task_result.hpp` and the four apps anchor the runtime (X-05/X-09); result family is independent (canonical `Request<T>` never touches Scheduler) — the dependency is helper→runtime, not result→runtime. The X-16 node is the point where the two chains intersect.
3. **If Fiber is internalized, must the StackfulIoHost header be fixed first?** It is first among equals: `stackful_io_host.hpp` is the only public header embedding `Fiber` **by value** (X-03) and `application_runtime.hpp` is the second boundary (X-04), but internalization is not reachable until **Group retires** and the `Fiber`-naming runtime vocabulary (`async_mutex`, wait/select internals) is demoted/internalized (X-17). All three `🔒` nodes precede the tail step.
4. **If FileReader/FileWriter retire, who must migrate first?** `IoContext`/`BlockingIoContext` (their only production caller), then the 6 test files — with ledger re-recording for the 4 oracle tests (X-07 + evidence edge); no app migrates (X-07/X-08). Two decisions precede all of it: the second-authority question (X-06) and the vectored-IO capability decision, because it determines whether any behavior is preserved via delegation, re-implemented canonically, or intentionally dropped.

## 6. Migration summary matrices

| Field | Value |
|---|---|
| CROSS_FAMILY_MIGRATION_EDGES | 18 recorded (X-01..X-18), of which 1 is a negative finding (X-06, incl. the vectored-IO expressiveness gap) and 1 is a build/ODR edge (X-12) |
| F2_PREREQUISITES (Completion/result contraction) | X-01, X-02, X-16, X-18 (host/Completion DOC_CLAIMs); census exposure: compat overloads on canonical hub |
| F3_PREREQUISITES (Fiber/internal runtime) | X-03, X-04, X-05, X-09, X-14, X-17, X-18 (ApplicationRuntime-lifecycle DOC_CLAIMs) |
| F4_PREREQUISITES (legacy resource world) | X-06, X-07, X-13; D-11/D-12 undecided rows (X-18 is a host/Completion documentation edge, not a native-resource dependency — it rides the F2/F3 documentation-migration path above) |

`UNRESOLVED` inputs this DAG depends on (tracked, not decided here): ADR-0003 adoption (host profile), D2 human review, `RequestHandle` final home + identity-fallback ownership, `BlockingIoPool`/`wal`/`copy` workload admission (PROD-03), vectored-IO capability decision, external-use decisions per policy §3.
