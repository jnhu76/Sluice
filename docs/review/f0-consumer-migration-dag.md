# F0 Consumer Graph & Migration DAG (#454, parent #402)

- **Status**: `CONSUMER_MIGRATION_DAG_DEFINED` candidate
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
| X-01 | legacy host world (`ApplicationRuntime`, `RuntimeTaskContext`, `await_op_helpers`, `async/file.hpp`, `op_helpers`, `Batch`, all `submit_*(&, Completion&)` overloads, backend publish path, 15 test files) → `Completion<T>` | COMPILE + CALL + TYPE_LAYOUT (`Batch::Slot` holds `Completion` by value) | compat result carrier is baked into every legacy spelling of submission; removing it without migration breaks compile of host helpers and backends |
| X-02 | `Completion<T>` (R-07 release path) → `RequestArena` | COMPILE + CALL | `completion.hpp` includes `detail/request_arena.hpp`; release may still route `release_completed_binding` when the legacy binding is installed — the arena cannot be removed before this path is |
| X-03 | `StackfulIoHost` (public header) → `Fiber`/`fiber_ctx` | TRANSITIVE_INCLUDE + TYPE_LAYOUT (`TaskSlot::fiber` by value; `fiber_ctx::Context driver_ctx_`; `fiber_ctx::Switch*` param) | **public-header layout dependency**: F3 internalization must re-spell this header first (PIMPL/opaque slot) or every `stackful_io_host.hpp` consumer breaks |
| X-04 | `ApplicationRuntime` (public header) → `Scheduler`/`SchedulerWakeHandle` | TRANSITIVE_INCLUDE + TYPE_LAYOUT (`unique_ptr<Scheduler>`, `SchedulerWakeHandle` by value) | same class of exposure for the runtime world |
| X-05 | `await_op_helpers` / `RuntimeTaskContext` → `Scheduler`/runtime | TRANSITIVE_INCLUDE (`await_op_helpers.hpp` → `application_runtime.hpp` → `scheduler.hpp` → `fiber*.hpp`) | helper family cannot outlive the runtime world; migration order: helpers before runtime |
| X-06 | `FileReader`/`FileWriter` → canonical `File` semantics | **NO EDGE (negative finding)** | they define open/close/move/sync independently on raw `fd_` (`src/file.cpp`); they share the *oracle* (`detail/file_semantics.hpp`) but not the *resource authority*; flagged as second-authority candidate, decision owned by the audit phase |
| X-07 | `IoContext`/`BlockingIoContext` factory → `FileReader`/`FileWriter` | COMPILE + CALL | the factory is the only production CALL consumer of L-01; apps do **not** include `sluice/file.hpp` (GREP: 0 app hits) |
| X-08 | apps → legacy/host APIs | APP_EXAMPLE | all 4 apps include `application_runtime/task_result/await_op_helpers/async_io_context/threadpool_backend/file_resource/error/result`; sluice-copy additionally `async/file.hpp` + `blocking/file.hpp` + **`detail/posix_retry.hpp`** (app→detail edge, X-13); no app includes `completion.hpp`, `sluice/file.hpp`, scheduler headers directly |
| X-09 | apps → ApplicationRuntime → Scheduler/Fiber/Group | TRANSITIVE_INCLUDE (+ CALL via `task_result.hpp::run_task_to_result`) | the four apps are the anchor consumers of the multi-worker runtime; retiring Scheduler before re-homing apps is unsound |
| X-10 | docs → retired/compat vocabulary | DOC_CLAIM | `README.md`/`README.zh-CN.md` name caller-owned `Completion`, `AsyncIoContext`, Scheduler/Fiber as *retained baseline* (correctly labeled, keep); ADR-0002 (superseded) is the only doc describing `BlockingIoPool`/`MemoryIoContext` semantics; ledger rows own the current vocabulary. No current doc promises the legacy surface as product. |
| X-11 | formal → implementation surface | FORMAL | active models map to `RequestCore`/observer/progress/shutdown machinery only (`RequestCore.tla`, `ObserverCore.tla`, `ProgressSource.tla`, `ShutdownCore.tla`); none maps to `Completion`, `Scheduler`, or the legacy stream world (formal map §8) |
| X-12 | `SLUICE_HAS_LIBURING` → public ODR surface | BUILD_PACKAGE | public define + public link on `sluice_async`; every consumer TU must see the same guarded layout (census §1.5) — any package rule must carry it |
| X-13 | `apps/sluice-copy/safe_output.cpp` → `sluice/detail/posix_retry.hpp` | TRANSITIVE_INCLUDE (direct) | app consuming an INTERNAL header directly — recording only; future home is a canonical spelling or the app's own helper |
| X-14 | tests → `SLUICE_ASYNC_INTERNAL_TESTING` + `src/async` include dir (~15 target families) | BUILD_PACKAGE + TEST | seam access is load-bearing for current evidence; internalizing any seam surface requires seam migration in the same slice |
| X-15 | `Scheduler` → core progress/ready protocol | COMPILE (`ReadyRoutingSink : detail::SynchronousReadySink`) | the runtime consumes the canonical ready-routing contract; it adds no second progress authority (C1/C2 evidence) |

## 3. Consumer edges by family (summary, from the include graph)

Full per-header counts are regenerable via `scripts/f0_surface_inventory.py`. Anchors:

- `async_io_context.hpp` (hub): 4 apps, 18 test files, 2 src TUs, 9 headers — every migration touches it.
- `application_runtime.hpp`: 4 apps, 1 test, 1 src, 3 headers (incl. `task_result.hpp`, `await_op_helpers.hpp`, `async/file.hpp`).
- `threadpool_backend.hpp`: 6 app TUs (all four apps), 19 test files.
- `completion.hpp`: 15 test files (+ hub transitively). 0 apps, 0 direct src (friends use it via the hub).
- `request.hpp` (canonical `Request<T>`): 5 direct test files + every hub consumer transitively.
- `sluice/file.hpp` (legacy): 2 src-core TUs, 6 test files. 0 apps.
- dormant wrappers: 0 direct consumers (3 headers) / header-only (2).

## 4. F0-D — Replacement / adoption graph

`MIGRATION_STATE` uses: `ADOPTED` (ledger VERIFIED or ADOPTED ADR), `IMPLEMENTED_PENDING` (ledger `IMPLEMENTED_UNVERIFIED`), `PARTIAL` (replacement exists and is canonical for new paths; legacy spellings still live), `PROPOSED`, `HISTORICAL` (nothing to do).

| OLD_ROLE | ADOPTED_REPLACEMENT | REPLACEMENT_SHA (adoption evidence) | ROOT_AUTHORITY | EXPRESSIVENESS_DIFFERENCE | MIGRATION_STATE |
|---|---|---|---|---|---|
| Backend waiter vocabulary (`WaiterToken`, `RoutingLease`, `BackendWaitSource`) | context-owned `ProgressSource` + `BackendProgressPort`; OBS registration protocol | C1-E removed legacy waiters; C2-A cutover (ledger C1/C2 records, VERIFIED) | ARCH-01, PROG-01..03, OBS-01/02 | backend loses waiter-domain authority; gains narrow signal + one-shot registration only | **ALREADY_MIGRATED** (names no longer exist; census §2.3) |
| Backend-owned request bookkeeping | context-owned `RequestCore` (acceptance/terminal/publication/reclaim) | B1-A/B1-C cutovers; B1-D seam collapse (VERIFIED evidence chain) | REQ-01..06, ARCH-02 | core owns arbitration + public binding; backend only executes | ADOPTED (arena retained solely as Completion release substrate — X-02) |
| Caller `Completion<T>` as result authority | public `Request<T>` binding over core | B2 record (VERIFIED) | ARCH-02, HANDLE-01..03, MIG-02 | move-only unique responsibility vs caller-owned storage; observation adds `EffectReport` | PARTIAL: canonical for new paths; compat spellings + backends' publish-into-Completion still live |
| `RequestHandle`-style identity only | `RequestHandle` (kept) + `RequestId` split | B2/E1 | REQ-01, MIG-02 | copied identity without result authority | ADOPTED (spelling kept by design) |
| busy-poll helpers | explicit Request/progress/scoped paths | MIG-02 row | INV-01, PROG | — | **HISTORICAL: no busy-poll helper exists at the baseline** (GREP); MIG-02 row retained as guard, no task |
| `AsyncIoContext` vs root `IoContext` name | root `IoContext` maps to `AsyncIoContext` until F-phase rename | ADR-0004 §1 (ADOPTED) | ARCH-02, SHUT | naming only | PARTIAL (rename is an F-phase obligation, owned by #402) |
| General runtime (`ApplicationRuntime`/`Scheduler`/`Fiber`/`Group`) | narrow single-owner `StackfulIoHost` | D2 record (`IMPLEMENTED_UNVERIFIED`), ADR-0003 (**PROPOSED**) | HOST-01/02/03, MIG-02 | single-owner vs multi-worker; scheduler primitives demoted to internal substrate | PROPOSED/IMPLEMENTED_PENDING — **not adopted**; both hosts shipped, W-04 gate pending human review |
| `Completion`-based await conveniences | host conveniences over `Request`-typed awaits | D2 (`read_exact`/`write_all` with `Result<CompositionOutcome>`) | HOST-03, SEM-05 | every-exit settlement typing | PARTIAL (StackfulIoHost spelling adopted-pending; `await_op_helpers`/`async/file.hpp` still Completion-spelled) |
| Legacy `FileReader/FileWriter`/`IoContext` factory | canonical `File` + `blocking::` | A1/A2 (VERIFIED) | SEM-01..07, LIFE, W-01 | shared oracle + single resource authority vs per-class fd ownership | PARTIAL: canonical side complete; legacy side un-retired (X-06/X-07) |
| Experimental uring write path | canonical `UringAsyncBackend` | B1-C/E1 | BACKEND, LIFE-03 | raw-fd path batch vs request-contract backend | replacement adopted; experimental surface orphaned (FA-2), disposition D-14 |

## 5. Consumer migration DAG

Nodes are consumer groups; an edge A→B means "B must settle before A can proceed". `🔒` marks a phase-prerequisite from the census exposure facts.

```text
Retire Completion<T> (D-01):
  [tests using Completion (15)] ──────────────┐
  [Batch (1 test)] ───────────────────────────┤
  [op_helpers → Request-typed helpers (H-05)] ├─→ [drop Completion& overloads on
  [await_op_helpers / async/file.hpp (H-06/07)]│    AsyncIoContext / RuntimeTaskContext]
  [ApplicationRuntime internal submits (H-08)]─┤              │
  [backend publish path (H-02, both backends)]─┘              ▼
  [🔒 X-02: arena release path rerouted to core]   →   [remove completion.hpp]
                                                     → [remove RequestArena/request_slot/
                                                        submit_transaction (D-02/D-03)]

Retire Scheduler/runtime (D-family around H-08/H-10):
  [4 apps off ApplicationRuntime/task_result] → [await helpers re-typed] →
  [ApplicationRuntime removal] → [Group/Future/WaitPolicy (H-13..15) removal] →
  [Scheduler + wait/select/timer/queue machinery internalize-or-retire]
  (runtime sync primitives H-17/H-18/H-22 retire with or before it)

Internalize Fiber (F3):
  [🔒 stackful_io_host.hpp boundary fix (X-03/D-16)] →
  [🔒 application_runtime.hpp boundary fix (X-04)]   →
  [scheduler.hpp de-fiberization or internalization] → [fiber.hpp/fiber_ctx.hpp become internal]

Retire FileReader/FileWriter (D-09/D-10):
  [decide second-authority question (X-06)] → [IoContext factory removal (X-07)] →
  [6 test files migrate to canonical File/blocking] → [remove sluice/file.hpp legacy classes]
```

### Answers required by #454 §13

1. **If Completion is retired, who migrates first?** The 15 test files and the internal producers: `op_helpers`, `await_op_helpers`, `async/file.hpp`, `ApplicationRuntime` submit paths, `Batch`, and both backends' publish path; the arena release path (X-02) must be rerouted before `detail/request_arena.hpp` can follow. `Request<T>`/`RequestHandle` are the adopted targets; `RequestScope` already shows the no-Completion pattern.
2. **If Scheduler is retired, must helpers/result family migrate first?** Yes: `await_op_helpers`/`async/file.hpp`/`task_result.hpp` and the four apps anchor the runtime (X-05/X-09); result family is independent (canonical `Request<T>` never touches Scheduler) — the dependency is helper→runtime, not result→runtime.
3. **If Fiber is internalized, must the StackfulIoHost header be fixed first?** Yes — it is the only public header embedding `Fiber` by value and `fiber_ctx::Context` in private members (X-03); `application_runtime.hpp` (X-04) is the second boundary. Both must be de-laid-out before `fiber*.hpp` can leave the consumer-visible tree.
4. **If FileReader/FileWriter retire, who must migrate first?** `IoContext`/`BlockingIoContext` (their only production caller), then the 6 test files; no app migrates (X-07/X-08). The second-authority question (X-06) is decided before any of it, because it determines whether any behavior is preserved via delegation or simply removed.

## 6. Migration summary matrices

| Field | Value |
|---|---|
| CROSS_FAMILY_MIGRATION_EDGES | 15 recorded (X-01..X-15), of which 2 are negative findings (X-06) and 1 is a build/ODR edge (X-12) |
| F2_PREREQUISITES (Completion/result contraction) | X-01, X-02; census exposure: compat overloads on canonical hub |
| F3_PREREQUISITES (Fiber/internal runtime) | X-03, X-04, X-05, X-09, X-14 |
| F4_PREREQUISITES (legacy resource world) | X-06, X-07, X-13; D-11/D-12 undecided rows |

`UNRESOLVED` inputs this DAG depends on (tracked, not decided here): ADR-0003 adoption (host profile), D2 human review, `RequestHandle` final home, `BlockingIoPool`/`wal`/`copy` workload admission (PROD-03), external-use decisions per policy §3.
