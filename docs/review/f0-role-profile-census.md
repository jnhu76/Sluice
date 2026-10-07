# F0 Role / Profile Census (#454, parent #402)

- **Status**: `F0_ROLE_PROFILE_CENSUS_COMPLETE` candidate — CANDIDATE DISPOSITIONS ONLY, no deletion authorization
- **Baseline (frozen)**: `MASTER_SHA = 989d5c3fa5fa803c05ac87ebfe6c99db9d87564b` (live master at census start; branch `docs/454-f0-role-profile-census`)
- **Root**: `docs/explicit-io-v1-final-decision.md`, revision `v1-r3` (sole normative authority)
- **Adopted ADRs**: ADR-0004 (ADOPTED, PR #453 at `eb7bf055`). ADR-0003 is **PROPOSED** (D2 ledger row `IMPLEMENTED_UNVERIFIED`, human review pending). ADR-0001/0002 superseded (GOV-03).
- **Issue states at freeze**: #399/#400/#401/#452 CLOSED; #402/#454 OPEN; `E_REQUIRED_CLOSURE_ADOPTED = PASS`
- **Scope**: this file records F0-A (tree/build/install), F0-B (semantic role) and formal-asset classification. Consumer edges and migration ordering live in `f0-consumer-migration-dag.md`; profile/compatibility policy lives in `f0-package-profile-policy.md`.
- **Production source diff**: 0. No production file was modified to produce this census.

## 0. Method and provenance

Every row below has `SOURCE_SHA = 989d5c3fa5fa803c05ac87ebfe6c99db9d87564b`
(the frozen baseline; a per-row column would repeat one value). Fields that
could not be established say `UNKNOWN`; nothing is guessed.

`DISCOVERY_METHOD` values used:

| Code | Meaning |
|---|---|
| `READ` | full read of the header/TU at the baseline SHA |
| `XMAKE` | parsed `xmake.lua` / `xmake/{helpers,libraries,apps,tests}.lua` rule text |
| `GRAPH` | mechanical include-graph extraction over `include/ src/ apps/ tests/` (script: `scripts/f0_surface_inventory.py`) |
| `GREP` | mechanical symbol/reference scan over `include src apps tests docs formal` |
| `LEDGER` | `docs/roadmap/v1-conformance.md` slice records |
| `FORMALMAP` | `docs/roadmap/v1-formal-evidence-map.md` disposition tables |

Census primary key (per #454): `surface_id` × `semantic_role` × `shipped_profile`.
Surfaces are never classified by file name, age, or naming similarity; where a
surface carries more than one role, each role gets its own row
(e.g. `Completion<T>` = compatibility result carrier **and** retained arena
release substrate; `SynchronousReadySink` = routing interface **and**
backend-embedded no-op).

## 1. F0-A — Tree / build / install census

### 1.1 Targets (XMAKE)

| Target | Kind | Default | Members | Notes |
|---|---|---|---|---|
| `sluice_core` | static lib | **default** | glob `src/*.cpp` (non-recursive), 13 TUs | public includedir `include/`; links nothing external |
| `sluice_async` | static lib | opt-in (`set_default(false)`, group `async`) | glob `src/async/*.cpp` (non-recursive), 35 TUs + explicit `src/async/detail/context_identity.cpp`, `src/async/detail/request_core.cpp` = 37 TUs | public includedir `include/`; deps `sluice_core`; clang-only TSA flags; `SLUICE_HAS_LIBURING` + public link `uring` under `--liburing` |
| `sluice-copy/-hash/-grep/-tail` | binaries | opt-in (group `apps`) | `apps/*/​*.cpp` listed explicitly | deps core+async; `apps.lua` states "public headers only" |
| ~70 test/binary targets | binaries | opt-in (group `test`) | one or more `tests/*.cpp` | several families add include dir `src/async` and define `SLUICE_ASYNC_INTERNAL_TESTING` (internal seam access); uring variants add `SLUICE_HAS_LIBURING` |

Resolved canonical source sets (exact, GRAPH over the two globs):

- `sluice_core` = `blocking_file.cpp blocking_io_pool.cpp buffer.cpp copy.cpp copy_strategy.cpp fault.cpp file.cpp file_resource.cpp io_context.cpp observed.cpp reader.cpp wal.cpp writer.cpp`
- `sluice_async` = the 35 `src/async/*.cpp` listed in `libraries.lua`'s globs at the baseline SHA **plus** `detail/context_identity.cpp`, `detail/request_core.cpp`
- **Orphan TUs**: `src/experimental/uring_io_context.cpp`, `src/experimental/uring_write_batch.cpp` are members of **no** target (production or test) at the baseline. Their headers are still on the public include path.
- Four seam TUs compile into the production `sluice_async` archive: `mutex_test_seam.cpp`, `queue_test_seam.cpp`, `scheduler_fe2_test_seam.cpp` (bodies fully `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)`-guarded) and `fail_fast.cpp` (one guarded test region). `GRAPH`+`READ`.

### 1.2 Install / package (XMAKE + LEDGER)

- **No install, export, or package rule exists anywhere** in `xmake.lua` / `xmake/` (GREP; `set_kind`, `add_installrules`, package targets: zero hits).
- Ledger row "Installed-header / package surface" is `NOT_ASSESSED` and states: *"No install or package target exists in this commit, so no installed surface can be audited; the audit belongs to the legacy/retirement slice (#402)."*
- Consequence: **`INSTALLED = NO` for every header in this census.** "Public" below means *reachable from the public include path* (`include/` is added `{public = true}` on both libraries, so every header under `include/` — including `detail/` and `experimental/` — is consumer-visible), not *installed*.

### 1.3 Header inventory (GRAPH)

| Group | Count | Notes |
|---|---:|---|
| `include/sluice/*.hpp` (core namespace) | 24 | canonical File vocabulary, blocking adapter, legacy stream world, vocab carriers, 2 test-double headers |
| `include/sluice/async/*.hpp` | 34 | request/result family, context/host family, runtime world, backends |
| `include/sluice/async/detail/*.hpp` | 19 | core substrate, progress/observer/ready seams, queue/select mechanisms, 2 test seams |
| `include/sluice/detail/*.hpp` | 4 | file semantics oracle, posix retry, uring submit classification, pool impl |
| `include/sluice/experimental/*.hpp` | 2 | uring write batch + io context |
| **Total** | **81** | no umbrella header exists; consumers include per-topic headers |

Headers with **zero in-tree includes** (no `sluice/...` include anywhere in `include/ src/ apps/ tests/`): `async/async_queue.hpp`, `async/condition.hpp`, `async/semaphore.hpp`, `memory_io_context.hpp`. These are shipped public-namespace surfaces with no in-tree consumer; per §8 of the brief this records `IN_TREE_CONSUMERS = 0` and **not** a removal verdict (`UNKNOWN_EXTERNAL_USE` handled in the policy file).

### 1.4 Transitive exposure facts (GRAPH — load-bearing for F2–F4)

| Fact | Evidence |
|---|---|
| `async/completion.hpp` transitively includes `detail/request_arena.hpp`, `detail/request_core.hpp`, `detail/request_slot.hpp`, `detail/fail_fast.hpp` | direct includes `completion.hpp:3-5` |
| `async/async_io_context.hpp` (the canonical context hub) directly includes `completion.hpp`, `detail/observer_protocol.hpp`, `detail/progress_source.hpp`, `detail/ready_sink.hpp`, `detail/request_key.hpp`, `request.hpp`, `request_handle.hpp`, `file_resource.hpp`, `measurement.hpp` | `async_io_context.hpp:3-13`; every async public header that reaches it transitively exposes the whole detail substrate |
| **`async/stackful_io_host.hpp` directly includes `fiber.hpp` + `fiber_ctx.hpp`; private `TaskSlot` holds `Fiber` by value; private members `fiber_ctx::Context driver_ctx_` and `fiber_ctx::Switch*` parameter** | `stackful_io_host.hpp:6-7,181,197,219` — mandatory #454 check: public-header type/layout dependency on Fiber exists today; internalizing Fiber (F3) **must fix this header first** |
| `async/application_runtime.hpp` includes `scheduler.hpp`; private members `std::unique_ptr<Scheduler>`, `SchedulerWakeHandle wake_handle_` (by value) | `application_runtime.hpp:6,182-184` — Scheduler layout is exposed to every ApplicationRuntime consumer |
| `async/scheduler.hpp` includes `fiber.hpp`+`fiber_ctx.hpp`; public nested `WorkerState` embeds `fiber_ctx::Context` by value and `std::atomic<Fiber*>`; public API takes `Fiber&` | scheduler agent census; F3 implication as above |
| `blocking_io_pool.hpp` unconditionally includes `detail/blocking_io_pool_impl.hpp` (template impl leak, namespaced detail) | `blocking_io_pool.hpp:96` |
| Nearly every async public header transitively reaches `sluice/detail/file_semantics.hpp` (the semantic oracle) | GRAPH closure over `async_io_context.hpp` |
| `WaiterToken` / `RoutingLease` **do not exist** anywhere in `include/` or `src/` | GREP, zero hits — MIG-02's backend-vocabulary removal (C1-E) already happened; recorded `ALREADY_MIGRATED`, no new deletion task |

### 1.5 Configuration macros (READ + XMAKE)

| Macro | Kind | Where | Consumers must see it? |
|---|---|---|---|
| `SLUICE_HAS_LIBURING` | build profile | defined public on `sluice_async` with `--liburing`; guards the whole uring backend body in `uring_backend.hpp` (stub throws at construction without it) | **Yes — ODR-critical**, public define + public link propagate from the single config point |
| `SLUICE_ASYNC_INTERNAL_TESTING` | test seam | defined by ~15 test target families, which also add include dir `src/async`; opens test accessors in `async_io_context.hpp`, `application_runtime.hpp`, `scheduler.hpp`, `stackful_io_host.hpp`, `request_arena.hpp`, `progress_source.hpp`, `task_result.hpp`, seam TUs | No — must never reach production builds |
| `SLUICE_*_MUTANT_*` (~40 names: `SLUICE_B1C_MUT_*`, `SLUICE_B2_MUT_*`, `SLUICE_D1_MUT_*`, `SLUICE_E1_MUT_*`, `SLUICE_E2_MUT_*`, `SLUICE_STACKFUL_HOST_MUT_*`, `SLUICE_SHUTDOWN_URING`, `SLUICE_E1_CONFORMANCE_URING`, …) | mutation-test seams | test targets only; several are referenced **inside public headers** (`request.hpp`, `request_scope.hpp`, `stackful_io_host.hpp`) to compile named broken variants | No — mutant builds are evidence artifacts |
| `SLUICE_FIBER_ASAN_ENABLED` / `SLUICE_FIBER_TSAN_ENABLED` | sanitizer autodetect | `fiber_ctx.hpp` only (struct layout varies with sanitizers) | Derived, not a user switch |
| `EWOULDBLOCK != EAGAIN` guard | POSIX | `error.hpp` | n/a |

### 1.6 Findings (F0-A)

| ID | Severity | Finding |
|---|---|---|
| FA-1 | P2 | No install/package target exists, so "installed vs not installed" is currently uniform (`NO`); the PACKAGE_PROFILE_POLICY (companion file) still classifies per-profile because a future install rule is an F-phase decision, not a present fact. |
| FA-2 | P2 | `src/experimental/*.cpp` (2 TUs) compile into no target; their headers remain on the public include path. Experimental surface is header-shipped but not linked — exactly the "physically present ≠ supported" split the policy must record. |
| FA-3 | P2 | Seam TUs (`*_test_seam.cpp`, guarded) and test seam regions in public headers live inside the production archive/header set; the mechanism is macro-gated and named, but it means the shipped archive contains test-only code paths reachable only under a define production never sets. |
| FA-4 | P3 | `libraries.lua` header comment names a `bench_common` production library that no rule declares; comment-only staleness. |
| FA-5 | P3 | Four public headers have zero in-tree includes (`async_queue`, `condition`, `semaphore`, `memory_io_context`); three of them are thin shells over live `Scheduler::` machinery (their behavior lives in `scheduler_*.cpp`), one is an incoherent test double. |

No P0/P1. All findings are recorded, not repaired (F0 forbids production changes).

## 2. F0-B — Semantic role census

Legend for compact columns:

- `PROFILE`: `CANONICAL` (adopted v1 surface), `OPT_SUPPORTED` (shipped optional profile; W-04 gate per PROD-02), `COMPAT` (compatibility/dormant retained surface, no promise attached), `EXPERIMENTAL`, `INTERNAL` (detail mechanism; still consumer-visible via the public include path), `TEST_ONLY` (public-tree test doubles).
- Authority codes (which authority the surface **owns**; `·` = none): `R`esult/settlement, `O`wnership (native/stack/memory), `A`ccess (provenance/legality), `L`ifetime (borrow/binding/reclaim), `P`rogress (driver/notification), `I`dentity/routing, `S`emantic-oracle (shared meaning).
- `STATUS`: `SUPPORTED` (ledger-evidenced v1), `SUPPORTED_OPT`, `SHIPPED` (present, no v1 claim), `DORMANT` (zero in-tree consumers), `PROPOSED` (ADR pending), `HISTORICAL`, `TEST_ONLY`.
- Consumers: `src`/`tst`/`app`/`hdr` counts from the include graph (`hdr` = only reached via other headers); `none` = zero includes anywhere.

### 2.1 Vocabulary / shared-semantics family (root SEM/ERR basis)

| ID | Surface | Path | Kind | Profile | Auth | Role | Status | Consumers | Root IDs |
|---|---|---|---|---|---|---|---|---|---|
| V-01 | `sluice::Result<T>` / `Result<void>` / `make_unexpected` | `sluice/result.hpp` | header-only template | CANONICAL | R | result value carrier for the whole repo (expected-like, fixed `IoError` error type) | SUPPORTED | src 3 · tst 19 · app 6 | ERR-01/02 |
| V-02 | `sluice::IoError` + `Code` (11 canonical codes) + errno mapping | `sluice/error.hpp` | value + inline mapping | CANONICAL | S | canonical error-domain authority; no compat spellings exist | SUPPORTED | src 7 · tst 2 · app 11 | ERR-01 |
| V-03 | `sluice::EffectReport`, `EffectCertainty` | `sluice/effect.hpp` | value | CANONICAL | S | data-effect currency shared by blocking + request worlds (aliased `IoEffect` in detail) | SUPPORTED | tst 1 (via others) | ERR-02 |
| V-04 | `sluice::File`, `FileOpen/Access/Existence/InitialContents`, `FileKind/Identity/Info/Size`, `IdentityMatch` | `sluice/file_resource.hpp` | class + vocab | CANONICAL | O, A | **canonical native-resource + access authority** (RAII fd owner; open/close/move per SEM-02; `native_handle()` borrow per LIFE-02) | SUPPORTED | src 3+7 · tst 37 · app 9 | SEM-02/03/07, LIFE-01/02 |
| V-05 | `sluice::blocking::{read,write,read_at,write_at,sync_data,sync_all,file_info,size,resize,read_exact(_at),write_all(_at)}`, `CompositionOutcome/End` | `sluice/blocking/file.hpp` | free functions | CANONICAL | A, S | direct-invocation adapter over canonical `File`; shares the semantic oracle, no RequestCore | SUPPORTED (A1/A2) | src 1 · tst 17 · app 1 | SEM-01..06, INV-01, W-01 |
| V-06 | `sluice::detail::file_semantics` (precheck/classify/compose/IoOutcome/covers) | `sluice/detail/file_semantics.hpp` | constexpr oracle | INTERNAL | S | **the shared semantic oracle**: open legality, precedence, short-IO/EOF, composition, effect certainty, durability coverage | SUPPORTED (A1/A2) | src 10 · tst 8 | SEM-03..06, ERR-02 |
| V-07 | `sluice::detail::posix_retry` (`retry_on_eintr`) | `sluice/detail/posix_retry.hpp` | inline template | INTERNAL | · | syscall EINTR retry mechanism | SHIPPED | src 3 · app **1** · tst 0 | SEM-05 (EINTR rule) |
| V-08 | `sluice::detail::uring_submit` classification | `sluice/detail/uring_submit.hpp` | inline | INTERNAL | · | submit-outcome classification vocabulary (uring) | SHIPPED | src 2 · tst 1 | BACKEND-03 |
| V-09 | `sluice::detail::blocking_io_pool_impl` | `sluice/detail/blocking_io_pool_impl.hpp` | template impl | INTERNAL | · | impl completion for `blocking_io_pool.hpp`, unconditionally included by it | SHIPPED | hdr (via P-02) | — |

Note: `sluice/file.hpp` is **not** in this family — it is the legacy stream world (L-01 below). The file-name/role inversion is recorded exactly because the census key forbids path-based classification.

### 2.2 Request / result / identity family (root REQ/HANDLE)

| ID | Surface | Path | Kind | Profile | Auth | Role | Status | Consumers | Root IDs |
|---|---|---|---|---|---|---|---|---|---|
| R-01 | `sluice::async::Request<T>` | `sluice/async/request.hpp` | move-only class | CANONICAL | R, L | **unique public result/settlement binding** (nonblocking try/take, always-on nonterminal-release fail-fast per HANDLE-02) | SUPPORTED (B2) | tst 5 direct + via hub | ARCH-02, HANDLE-01..03 |
| R-02 | `sluice::async::RequestId` | `sluice/async/request.hpp` | value | CANONICAL | I | copied identity = lookup/cancel token while context lives (REQ-01) | SUPPORTED | via R-01 | REQ-01 |
| R-03 | `sluice::async::RequestObservation<T>`, `RequestReadiness` | `sluice/async/request.hpp` | value | CANONICAL | R | observation view (readiness + result + `EffectReport`); not an authority | SUPPORTED | via R-01 | ERR-02, HANDLE-01 |
| R-04 | `sluice::async::CancelDisposition` (5 values incl. `physical_interruption_unsupported`) | `sluice/async/request.hpp` | enum | CANONICAL | S | cancel-disposition vocabulary (CANCEL-01) | SUPPORTED (E1) | via R-01 | CANCEL-01 |
| R-05 | `sluice::async::RequestHandle` + `RequestHandleState` | `sluice/async/request_handle.hpp` | value | CANONICAL | I | **copied identity without result authority** (backend-resolved state probe); MIG-02 separation of public responsibility from copied identity | SHIPPED | src 1 · tst 2 | REQ-01, MIG-02 |
| R-06 | `sluice::async::Completion<T>` — compatibility role | `sluice/async/completion.hpp` | class | **COMPAT** | R | caller-owned result storage; MIG-02: *"Compatibility only; remove as canonical result authority"*; 15 test consumers, 0 app | SHIPPED (retained) | tst 15 | MIG-02 |
| R-07 | `Completion<T>` — internal release-substrate role | same | class privates | INTERNAL | L | non-public friend surface: `install_core_binding_for_backend` / `release_public_binding` / `publish_from_reap`; the backend publishes **through** it; release path still routes via `RequestArena` when the legacy binding is installed | SHIPPED (retained) | src (friends) | REQ-05, MIG-02 |
| R-08 | `sluice::async::Batch` | `sluice/async/batch.hpp` | class | COMPAT | R | ordered batching facade storing private `Completion` cells; exposes `BatchResult` (plain `Result`), never `Request`/`RequestHandle` | SHIPPED (near-dormant: 1 test) | tst 1 | MIG-02 (compat family) |
| R-09 | `detail::RequestKey/ContextIdentity/SlotIndex/Generation`, `detail::context_identity` allocation | `detail/request_key.hpp`, `detail/context_identity.{hpp,cpp}` | value | INTERNAL | I | **identity domain authority** (context,slot,generation; exhaustion closes admission before wrap) | SUPPORTED (B1-A) | src 4 · tst 1 | REQ-01 |
| R-10 | `detail::RequestCore` | `detail/request_core.hpp/.cpp` | class | INTERNAL | R, L, I | **acceptance / terminal arbitration / publication / public-binding / observer-episode / reclaim authority**, owned by `AsyncIoContext` | SUPPORTED (B1-B/C/D, B2, C1, E1, E2) | src 4 · tst 13 | REQ-01..06, ARCH-01/02 |
| R-11 | `detail::RequestArena` + `detail::RequestSlot` + `SlotHandle` | `detail/request_arena.hpp`, `detail/request_slot.hpp` | inline classes | INTERNAL | R, L | **legacy substrate** (reserve→…→reap pipeline with own publication binding + ready ring). Still live in exactly two roles: `Completion`'s legacy release path (`release_completed_binding`) and the test-gated `submit_transaction` choreography | SHIPPED (legacy) | src 1 · tst (via R-06/07) | MIG-02 "RequestArena → converge to bounded core" |
| R-12 | `detail::submit_transaction<Policy>` | `detail/submit_transaction.hpp` | template | INTERNAL (TEST_ONLY today) | · | staged submit orchestration over the **arena** (not the core); included only under `SLUICE_ASYNC_INTERNAL_TESTING` | TEST_ONLY | src 0 · tst (seam) | — |
| R-13 | `detail::observer_protocol` (5 enum families) | `detail/observer_protocol.hpp` | enums | INTERNAL | S | observer-episode protocol vocabulary (OBS-01/02 outcomes) | SUPPORTED (C1) | hdr (via hub) | OBS-01/02 |
| R-14 | `detail::RequestState/TerminalResult/BorrowMetadata/CompletionBinding` (slot vocabulary) | `detail/request_slot.hpp` | vocab | INTERNAL | L | per-slot lifecycle vocabulary of the **arena** world | SHIPPED (legacy) | tst 1 | — |

Key authority statement: the repo currently has **one** canonical result/settlement authority (`Request<T>` over `RequestCore`) and **one** retained compatibility result carrier (`Completion<T>`) whose release path still partially routes through the legacy `RequestArena`. No third result authority exists (`Future<T>` owns only its own cell in the dormant Group world; `TaskResultSlot` is an app-facing mailbox, not a request result).

### 2.3 Progress / observer / ready-routing family (root PROG/OBS)

| ID | Surface | Path | Kind | Profile | Auth | Role | Status | Consumers | Root IDs |
|---|---|---|---|---|---|---|---|---|---|
| P-01 | `detail::ProgressSource` (+`Token`, `WakeReason`, `PhysicalProbe`) | `detail/progress_source.hpp` | class (inline methods) | INTERNAL | P | **context-owned progress authority**: eventfd + saturating (progress,control) epoch pairs, observe→acknowledge control retirement, park handshake, fd lend/retire | SUPPORTED (C2-A..E) | hdr (via hub) | PROG-01..04, ARCH-01 |
| P-02 | `detail::BackendProgressPort` | same | narrowed view | INTERNAL | P | backend-facing capability: `signal()` + one-shot kernel notification registration grant; backend can never wait/drain/rebind | SUPPORTED (C2-A) | src 2 (backends) | PROG-01/03 |
| P-03 | `detail::SynchronousReadySink` + `ReadyEvent` + `OperationKind` | `detail/ready_sink.hpp` | interface | INTERNAL | I | ready-notification routing contract (terminal-ready event → sink); `OperationKind` is the domain-wide op-kind enum | SUPPORTED | hdr + src (backends, scheduler) | OBS-02, ARCH-01 |
| P-04 | `detail::ReferenceReadySink` | `detail/reference_ready_sink.hpp` | no-op sink | INTERNAL | · | null-object sink embedded in **both production backends** (production discards; test seam counts) | SHIPPED | src 2 | — |
| P-05 | context observer API (`attach_observer/cancel_observer/retire_delivery` on `AsyncIoContext`, `ObserverAttachment/ObserverCancelResult`) | `sluice/async/async_io_context.hpp` | methods | CANONICAL | L | observer attach/cancel/retire entries over the core's episode protocol (Completion-based spellings today) | SUPPORTED (C1) | tst 13 (via hub) | OBS-01..04 |
| P-06 | `detail::ProgressOwner` (driving-authority handle) | `sluice/async/async_io_context.hpp` | class | CANONICAL | P | release handle for the context's **fixed** progress owner (no live transfer, PROG-01) | SUPPORTED (C2-D) | src 2 · tst · app (via hub) | PROG-01, SHUT-01 |

Not found (recorded `ALREADY_MIGRATED`, per #454 §6): `WaiterToken`, `RoutingLease`, `BackendWaitSource` — removed by C1-E/C2-A; no deletion task remains.

### 2.4 Context / host / runtime family (root INV/HOST/SHUT)

| ID | Surface | Path | Kind | Profile | Auth | Role | Status | Consumers | Root IDs |
|---|---|---|---|---|---|---|---|---|---|
| H-01 | `sluice::async::AsyncIoContext` | `sluice/async/async_io_context.hpp` | class | CANONICAL | I, L, P | **context authority**: identity, budgets/admission/health, owns {RequestCore, ProgressSource, AsyncBackend}; shutdown state machine driver (ADR-0004 §1: root's `IoContext` name maps here until the F-phase rename) | SUPPORTED (B1, C2, E1, E2) | src 2 · tst 18 · app 4 | ARCH-02, SHUT-01..04 |
| H-02 | `sluice::async::AsyncBackend` | same | abstract class | INTERNAL (mechanism seam) | A | backend interface: submit/poll/cancel/retirement seam; holds core_/progress_port_/routing_sink_ installed by the context; forbidden to mention Scheduler/Fiber vocabulary (ARCH-01) — verified clean | SUPPORTED | src 2 backends | BACKEND-01..03 |
| H-03 | `ReadOp/WriteOp/SyncDataOp/SyncAllOp/FileInfoOp/SizeOp`, `NativeFileRef` | same | value structs | CANONICAL | A | operation descriptors + **borrow/provenance representation** (`fd`,`access`); MIG-02: internal borrow model, "no second ownership model" | SUPPORTED | everywhere (hub) | LIFE-01/02, MIG-02 |
| H-04 | `ShutdownPolicy`, `ShutdownOutcome`, `close_admission/request_stop/shutdown` | same | enum/methods | CANONICAL | L | shutdown contract spelling (ADR-0004 §1 public spelling) | SUPPORTED (E2) | tst 18 · app 0 | SHUT-01..03 |
| H-05 | `op_helpers`: `read_all/write_all/sync_data_all/sync_all_all` | `sluice/async/op_helpers.hpp` | free functions | COMPAT | R | completed-return helpers over context + internal `Completion` cells; MIG-02: *"runtime completed-return helpers — retain only with every-exit settlement guarantees"* | SHIPPED | src 1 · tst 1 | MIG-02 |
| H-06 | `await_op_helpers`: `await_take/await_drain/await_read_once/await_read_fill/await_write_exact` | `sluice/async/await_op_helpers.hpp` | free functions | OPT_SUPPORTED | R | W-04 host conveniences over `RuntimeTaskContext` (suspend → acquire publication → consume); D2 evidence | SHIPPED (W-04 pending human review) | src 2 · tst 2 · app 4 | HOST-03, W-04 |
| H-07 | `async/file.hpp`: `await_read_at/await_write_at/await_sync_data/await_sync_all` | `sluice/async/file.hpp` | free functions | OPT_SUPPORTED | R | File-taking variants of the await conveniences (canonical `File&` + `RuntimeTaskContext` + `Completion`) | SHIPPED | src 1 · tst 9 · app 3 | HOST-03, SEM-01 |
| H-08 | `RuntimeTaskContext`, `RuntimeBuilder`, `ApplicationRuntime`, `RuntimeTaskFn` | `sluice/async/application_runtime.hpp` | classes | OPT_SUPPORTED | L | **multi-worker host runtime**: driver thread + Scheduler + root Group; owns io_ctx/sched/root_group; task lifecycle state machine | SHIPPED (D2 `IMPLEMENTED_UNVERIFIED`) | src 1 · tst 1 · app 4 | HOST-01, MIG-02 |
| H-09 | `StackfulIoHost`, `StackfulHostConfig`, `IoTaskContext` | `sluice/async/stackful_io_host.hpp` | classes | OPT_SUPPORTED | L, P | **narrow single-owner stackful host** (D2 SUPPORT_W04): claims progress owner in `run()`, bounded tasks/stacks, stop token, `_for` bounded parks; **fiber type/layout exposure** (§1.4) | PROPOSED (ADR-0003) | src 1 · tst 1 | HOST-01/02/03, W-04, PROD-02 |
| H-10 | `Scheduler` + `SchedulerWakeHandle` + `RunMode` + `WorkerState` | `sluice/async/scheduler.hpp` | class (781 lines) | OPT_SUPPORTED | L, P | scheduling domain of the stackful host: workers, wait registry (keyed by `detail::RequestKey`), timer heap, admission for every runtime primitive; routes `ReadyEvent` to fibers via `ReadyRoutingSink` (core-protocol consumer) | SHIPPED | src 18 · tst 1 | HOST-01, MIG-02 |
| H-11 | `Fiber`, `FiberState`, `CompletionWaitOutcome` | `sluice/async/fiber.hpp` | class | OPT_SUPPORTED | O | stackful execution **handle** (register context + cancel token; does **not** own its stack — Group/ApplicationRuntime own stack bytes) | SHIPPED | src 1(+12 via scheduler) · hdr 5 | HOST-01, MIG-02 |
| H-12 | `fiber_ctx::{Context,Switch,Entry,context_switch,…}` | `sluice/async/fiber_ctx.hpp` | ABI + asm | OPT_SUPPORTED | O | x86_64-Linux context-switch mechanism; sanitizer-variant layout | SHIPPED | src 12 · hdr 3 | — |
| H-13 | `Group` | `sluice/async/group.hpp` | class | OPT_SUPPORTED | O, R | fork-join over threads or evented fibers; owns stack bytes + `Future` cells + group `CancelToken`; retained as ApplicationRuntime's root group (HOST-01 "bounded structured lifetime required by retained adapter") | SHIPPED | src 2 · hdr 1 | HOST-01, MIG-02 |
| H-14 | `Future<T>` | `sluice/async/future.hpp` | template | COMPAT | R | one-shot result cell over `WaitPolicy`; **only consumer is `Group`** | SHIPPED (Group-only) | hdr 1 | MIG-02 "Group/Future/WaitPolicy" |
| H-15 | `WaitPolicy`, `ThreadedWaitPolicy`, `EventedWaitPolicy` | `sluice/async/wait_policy.hpp`, `evented_wait_policy.hpp` | interface | COMPAT | · | blocking strategy seam for `Future` (threaded vs scheduler-evented) | SHIPPED (Future-only) | hdr 2 | MIG-02 |
| H-16 | `CancelToken/CancelState/CancelGuard/CancelProtection/check_cancel` | `sluice/async/cancel.hpp` | latch vocab | CANONICAL (runtime-facing) | L | cooperative cancellation latch + epoch acknowledge protocol; used by Future/Fiber/Group **and** `StackfulIoHost::stop_token_` | SHIPPED | src 3 · tst · hdr | CANCEL-01/02 (runtime half) |
| H-17 | `Event` | `sluice/async/event.hpp` | class | COMPAT | L | runtime event flag + select arm; live in scheduler/select paths, wrapper class consumed only via scheduler | SHIPPED | src 12 | MIG-02 dormant-primitives family |
| H-18 | `select()/SelectResult/EventSelectCase/TimerSelectCase`, `select_fwd` concept | `sluice/async/select.hpp`, `select_fwd.hpp` | variadic template | COMPAT | L | runtime select over {event,timer} arms; arbitration in Scheduler | SHIPPED | src 10 | MIG-02 dormant-primitives family |
| H-19 | `WaitNode/WaitResume/ActorId/WaitOutcome`, `WaitQueue` | `sluice/async/wait_node.hpp`, `wait_queue.hpp` | intrusive vocab | INTERNAL (mechanism) | L | scheduler wait-list machinery; mutations friended to Scheduler only | SHIPPED | hdr only (via scheduler) | — |
| H-20 | `TimerRegistration`, `deadline_tick_t` | `sluice/async/timer_registration.hpp` | vocab | INTERNAL (mechanism) | L | deadline-heap arm protocol node | SHIPPED | hdr only | — |
| H-21 | `Mutex`, `LockGuard`, `thread_annotations` | `sluice/async/mutex.hpp`, `lock_guard.hpp`, `thread_annotations.hpp` | wrapper + macros | INTERNAL (mechanism) | · | host-internal `std::mutex` wrapper + Clang TSA shims | SHIPPED | hdr + src | — |
| H-22 | `AsyncMutex`, `AsyncCondition`, `Semaphore`, `AsyncRwLock`, `AsyncQueue<T>` | `sluice/async/{async_mutex,condition,semaphore,async_rwlock,async_queue}.hpp` | wrapper templates | COMPAT (**DORMANT**) | L | thin public shells over live `Scheduler::{mutex,condition,sem,rwlock,queue}_*` machinery; **zero in-tree includes** for condition/semaphore/async_queue (async_mutex/async_rwlock reached only via condition/queue headers) | DORMANT | none | MIG-02 *"public dormant synchronization primitives — outside v1"* |
| H-23 | `detail::queue_item`, `detail::queue_port`, `queue_test_seam` | `detail/queue_*.hpp` | mechanism | INTERNAL (runtime world) | L | bounded MPMC channel mechanics under the dormant `AsyncQueue<T>` shell; scheduler-driven | SHIPPED | src 2 | — |
| H-24 | `detail::select_port`, `detail::select_registration` | `detail/select_*.hpp` | mechanism | INTERNAL (runtime world) | L | select arbitration records + timer-heap unification | SHIPPED | src 13 · hdr | — |
| H-25 | `detail::fail_fast` (~49 named `[[noreturn]]` invariants) | `detail/fail_fast.hpp/.cpp` | vocabulary | INTERNAL | S | domain-wide invariant-violation diagnostics; names cover **both** substrate generations (arena + core) | SUPPORTED | src 23 | HANDLE-02, SHUT-04 (always-on diagnostics) |

### 2.5 Resource / stream world (`sluice_core`, root SEM/LIFE + MIG-02)

| ID | Surface | Path | Kind | Profile | Auth | Role | Status | Consumers | Root IDs |
|---|---|---|---|---|---|---|---|---|---|
| L-01 | `sluice::FileReader`, `sluice::FileWriter` | `sluice/file.hpp` (name!) | classes | COMPAT | **O** | **independent native-fd owners**: each holds raw `fd_`, own `close()` (first-attempt-consumes), own sync via `SyncableWriter`; no dependency on canonical `File` for ownership — candidate **second native-resource authority** pending the consumer audit (their semantics partially mirror SEM-02 close rules but are independently defined) | SHIPPED (legacy) | src 2 · tst 6 | MIG-02 *"legacy Reader/Writer/FileReader/FileWriter — retire from canonical surface after audit"* |
| L-02 | `sluice::Reader`, `sluice::Writer` | `sluice/reader.hpp`, `writer.hpp` | abstract | COMPAT | · | pure stream interfaces, own nothing (no fd, no close); embed some composition algorithm (`read_exact`, `stream_to`) | SHIPPED | src 5 · app 0 · hdr many | MIG-02 |
| L-03 | `sluice::BufferedReader/Writer`, `BufferedReadable` | `sluice/buffer.hpp`, `buffered_readable.hpp` | decorators | COMPAT | · | buffering decorators over borrowed Reader/Writer (borrowed scratch span; dtor asserts clean flush) — fully delegate resource semantics, **not** a second resource authority | SHIPPED | src 1 | MIG-02 |
| L-04 | `sluice::IoContext`, `BlockingIoContext`, `OpenReaderOptions/WriterOptions` | `sluice/io_context.hpp` | factory | COMPAT | · | abstract factory producing `unique_ptr<Reader|Writer>` by path; owns no fd, no results, no progress. **Name-collision hazard** with root `IoContext` (owned by `sluice::async::AsyncIoContext` per ADR-0004 §1) | SHIPPED (legacy) | src 1 · tst (via L-01) | MIG-02 |
| L-05 | `sluice::BlockingIoPool`, `Task<T>`, `PoolStats` | `sluice/blocking_io_pool.hpp` (+impl) | pool | COMPAT | O, R | self-contained worker pool owning threads + admission; `Task<T>` owns a result cell (move-only `get()`) | SHIPPED | src 1 · tst | MIG-02 legacy-wrapper family |
| L-06 | `sluice::wal::{write_record,read_record,WalWriter}` | `sluice/wal.hpp` | protocol | COMPAT | S | record framing + written→flushed→durable LSN vocabulary over **borrowed** `Writer` + optional `SyncableWriter`; owns no fd; delegates durability decision | SHIPPED | src 1 · tst 1 | — |
| L-07 | `sluice::copy_all` (5 overloads), `CopyStrategy/Options/Decision`, `CopyLimit`, `IoSlice/ConstIoSlice` | `sluice/copy.hpp`, `copy_strategy.hpp`, `limit.hpp`, `iovec.hpp` | free fns + vocab | COMPAT | · | copy algorithm + policy/outcome vocabulary over borrowed Reader/Writer | SHIPPED | src 3 · tst | — |
| L-08 | `sluice::SyncableWriter` | `sluice/sync.hpp` | interface | COMPAT | S | durability capability interface (`sync_data`/`sync_all`) separate from `Writer::flush` | SHIPPED | src 2 | SEM-06 (spelling ancestor) |
| L-09 | 7 stats structs (`SyscallStats`…`AsyncStats`), `ReaderStats/WriterStats`, `ObservedReader/Writer` | `sluice/measurement.hpp`, `sluice/observed.hpp` | counters | COMPAT | · | passive caller-owned instrumentation (three split homes: measurement/observed/PoolStats) | SHIPPED | src 3 · tst · app | — |
| L-10 | `FaultReader/FaultWriter/FaultPlan`, `MemoryReader/MemoryWriter` | `sluice/fault.hpp` | doubles | TEST_ONLY | · | in-memory doubles + fault injection shipped in the public tree | TEST_ONLY | src 1 · tst | — |
| L-11 | `sluice::MemoryIoContext` | `sluice/memory_io_context.hpp` | double | TEST_ONLY | · | in-memory `IoContext` factory; **writer side does not round-trip the store** (recorded defect, not repaired in F0) | TEST_ONLY, **DORMANT** (zero in-tree includes) | none | — |
| L-12 | `sluice::experimental::UringWriteBatch`, `UringIoContext`, `UringWriteResult` | `sluice/experimental/*.hpp` | classes | EXPERIMENTAL | O | raw-fd/path-based uring write loop; no `File`, no request identity, no provider-affinity/registered-buffer surface; **TUs orphaned from every target** (FA-2) | EXPERIMENTAL | src-orphan 2 | LIFE-03 constraint, MIG-02 experimental row |

Per the brief's BufferedReader example: `BufferedReader{Reader&}`/decorators (L-03) fully delegate resource semantics and are **not** counted as second authorities; `FileReader/FileWriter` (L-01) **are** flagged because they independently define open/close/move/sync behavior on a raw `fd_` without going through `sluice::File` — the consumer/audit question for F1 is whether that independence carries any retained consumer obligation.

### 2.6 Formal asset classification (FORMALMAP + READ)

Three axes per #454 §14: Retention / current-claim eligibility / scope.

| Asset set | Retention | Current-claim eligibility | Scope |
|---|---|---|---|
| Lean V2 family (11 modules, 16.8k lines: Calc/Condition/Driver/Event/Judge/Mutex/Queue/RwLock/Select/Sem/Vacuity) | ADAPT_PENDING / HISTORICAL | `NOT_CURRENT_CLOSURE_EVIDENCE` | conceptual/protocol over the **runtime world** (old sync primitives); no v1-r3 identity/observer/progress/shutdown vocabulary |
| TLA+ baseline 8 (`EventCore SemCore MutexCore CondCore RwCore QueueCore SelectCore DriverCore`) | HISTORICAL ×3, ADAPT ×4 (`SemCore CondCore QueueCore SelectCore`), `DriverCore` = RETIRE_FROM_V1_EVIDENCE | `NOT_CURRENT_CLOSURE_EVIDENCE` (safety-only, fuel-gated, no fairness — the map's §4 proof) | internal-substrate/protocol of the runtime world |
| `formal/tla/RequestCore.tla` + cfgs | RETAIN | `ACTIVE_IMPLEMENTATION_EVIDENCE` (B1-1; reused by E1 record unchanged) | protocol, safety + conditional liveness; maps to `RequestCore::{cancel,offer_terminal,…}` |
| `formal/tla/ObserverCore.tla` + cfgs | RETAIN | `ACTIVE_IMPLEMENTATION_EVIDENCE` (C1-H closure) | protocol (OBS), safety + conditional liveness; five C++ mutation seams mirrored |
| `formal/tla/ProgressSource.tla` + cfgs | RETAIN | `ACTIVE_IMPLEMENTATION_EVIDENCE` (C2-E re-closure incl. V24 adjudication) | protocol (PROG), safety + conditional liveness |
| `formal/tla/ShutdownCore.tla` + cfgs | RETAIN | `ACTIVE_IMPLEMENTATION_EVIDENCE` (E2; 171-run gate PASS) | protocol (SHUT), safety + reachability certs + conditional liveness with negative controls |
| `scripts/verify_formal.sh` / `verify_tla.sh` / lean toolchain | RETAIN | evidence tooling | — |
| `research/RESULTS.md` | RETAIN | `ACTIVE_ABSTRACT_SUPPORT` at most (self-described rationale) | conceptual |
| `docs/archive/**` (mission, ADR-0001/0002, old conformance, architecture snapshot) | HISTORICAL | `NOT_CURRENT_CLOSURE_EVIDENCE` (GOV-03 supersession) | conceptual/historical |
| `visuals/sluice-v1/` (+`review/ui-review.md`) | RETAIN | `ACTIVE_ABSTRACT_SUPPORT` for the target diagram only | conceptual (derived, non-normative) |

Forbidden inferences held both ways (recorded, per #454 §14): no asset is deleted for naming an old class; no historical PASS counts as current closure.

### 2.7 Evidence / promise surfaces (READ)

| Surface | Claim class |
|---|---|
| `README.md` / `README.zh-CN.md` | current canonical product claims, correctly subordinated to the root; explicitly labels caller-owned `Completion` APIs, `AsyncIoContext`, backends, runtime machinery as **retained baseline**, not yet target-satisfying |
| `docs/README.md` | navigation map; states only the root is normative |
| `docs/roadmap/v1-conformance.md` | implementation/evidence ledger (GOV-02) |
| `docs/roadmap/v1-formal-evidence-map.md` | formal governance (non-normative) |
| `docs/adr/0003` (PROPOSED) / `docs/adr/0004` (ADOPTED) | derived decisions |
| `docs/review/e2-a-reality-freeze.md` | FROZEN E2-stage reality census (structural predecessor of this file, narrower scope: shutdown slice) |
| `docs/roadmap/b1-1-request-core-evidence.md` | slice evidence record (B1-1) |
| `apps/*/README.md` | app-level usage docs; apps are public-header consumers |
| `review/ui-review.md` | visualization-slice evidence |
| `AGENTS.md`, `CHANGELOG.md` | working rules / history |

## 3. Summary matrices (#454 §19)

### Profile summary

| Field | Value |
|---|---|
| CANONICAL_PROFILE_SURFACES | 23 rows: V-01..V-05, R-01..R-05, H-01..H-04 (+P-05/P-06 spellings), L-… none |
| OPTIONAL_SUPPORTED_SURFACES | 5 rows: H-06..H-11 (await helpers, async/file helpers, ApplicationRuntime family, StackfulIoHost) |
| COMPATIBILITY_ONLY_SURFACES | 14 rows: R-06, R-08, H-05, H-14, H-15, H-17, H-18, H-22 (5 dormant wrappers), L-01..L-09 |
| EXPERIMENTAL_SURFACES | 1 row (2 headers): L-12 |
| INTERNAL_ONLY_SURFACES | 21 rows: V-06..V-09, R-09..R-14, P-01..P-04, H-02, H-19..H-21, H-23..H-25 |
| HISTORICAL_RESEARCH_SURFACES | 4 asset sets (§2.6): Lean family, baseline TLA 8, docs/archive, research/RESULTS |

### Authority summary

| Field | Holders (rows) |
|---|---|
| RESULT_AUTHORITIES | R-01 `Request<T>` (canonical, unique); R-06/R-07 `Completion<T>` (compat carrier + release substrate); R-11 `RequestArena` (legacy substrate, release path only) |
| NATIVE_RESOURCE_AUTHORITIES | V-04 `sluice::File` (canonical); L-01 `FileReader/FileWriter` (independent fd owners — flagged); L-05 `BlockingIoPool` (threads/queue, not file); L-12 experimental batch (raw fd, orphaned) |
| PROGRESS_AUTHORITIES | P-01 `ProgressSource` (unique, context-owned); P-02 `BackendProgressPort` (narrowed capability, no independent truth) |
| HOST_ROUTING_AUTHORITIES | P-03 `SynchronousReadySink`/`ReadyEvent` routing contract; H-01 `AsyncIoContext` (installs routing); H-10 `Scheduler` (runtime-world routing only) |
| SETTLEMENT_AUTHORITIES | R-10 `RequestCore` (terminal arbitration/publication); H-01 (shutdown/settlement driver per ADR-0004); H-09 `StackfulIoHost` (task-region settlement, host-scoped) |
| LIFETIME_AUTHORITIES | V-04 `File` (borrow/close per LIFE); H-01 (bindings/destroyable); R-01 (public binding); P-06 `ProgressOwner` (drive domain); H-16 `CancelToken` (runtime stop latch); H-13 `Group` (stack bytes) |

### Uncertainty

| Field | Value |
|---|---|
| UNKNOWN_ROWS | 0 rows carry UNKNOWN in a load-bearing field; per-field UNKNOWNs: none remaining after the three sub-censuses (consumer-side unknowns are external-use flags, tracked in the policy file) |
| UNKNOWN_EXTERNAL_USE_ROWS | tracked per family in `f0-package-profile-policy.md` §4 (no repo compatibility policy exists, so every COMPAT/EXPERIMENTAL/DORMANT row has `UNKNOWN_EXTERNAL_USE = unknown`) |
| AMBIGUOUS_ROLE_ROWS | 2: `Completion<T>` (compat carrier vs internal release substrate — split into R-06/R-07 rather than resolved), `RequestHandle` (public vocabulary vs future internal-only token — MIG-02 separation is adopted, final home not yet decided) |

## 4. Gate statement

- `F0_A_TREE_BUILD_INSTALL` = recorded (§1)
- `F0_B_SEMANTIC_ROLE` = recorded (§2, 68 surface rows across 6 families + formal/evidence assets)
- Production deletions / authority retirements performed by F0: **0**
- Candidate dispositions and policies: see companion files; nothing here authorizes removal.
