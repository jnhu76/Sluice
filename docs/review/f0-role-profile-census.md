# F0 Role / Profile Census (#454, parent #402)

- **Status**: `F0_ROLE_PROFILE_CENSUS_COMPLETE` candidate — CANDIDATE DISPOSITIONS ONLY, no deletion authorization. Round-1 human review (`F0_HUMAN_REVIEW = REQUEST_CHANGES`, PR #455 comment 6030469392): revision 2 resolves P1-1/P1-2/P2-1/P2-2/P2-3 (§4.1). Round-2 review (`F0_HUMAN_REVIEW_ROUND_2 = REQUEST_CHANGES`, comment 6031614724): `COMPATIBILITY_DISPOSITION_DEFINED` + `CONSUMER_MIGRATION_DAG_DEFINED` = PASS(candidate); revision 3 closes the residual census/policy P1 (OPTIONAL_CANDIDATE public closure, §4.2). Gates stay candidate pending Round-3 review.
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
surface carries more than one role — or one role whose public exposure rides a
different profile — each role × profile gets its own row
(e.g. `Completion<T>` = compatibility result carrier **and** retained arena
release substrate; `SynchronousReadySink` = routing interface **and**
backend-embedded no-op; `CancelToken`/`Fiber`/`fiber_ctx` split into
mechanism, legacy-runtime exposure, and OPTIONAL_CANDIDATE public-closure
exposure rows).

Two standing caveats on mechanical evidence:

1. The `GRAPH` closure **ignores `#if` guards**: an edge that exists only under
   `SLUICE_ASYNC_INTERNAL_TESTING` (e.g. `threadpool_backend.hpp` /
   `uring_backend.hpp` → `detail/submit_transaction.hpp`) is reported as if
   unconditional; §1.4 marks such edges where they matter.
2. Include-based consumer counts undercount *type users*: a file can use
   `Completion<T>` through another header without including `completion.hpp`.
   Consumer columns therefore distinguish direct includes from known type
   users where the adversarial review measured both.

## 1. F0-A — Tree / build / install census

### 1.1 Targets (XMAKE)

| Target | Kind | Default | Members | Notes |
|---|---|---|---|---|
| `sluice_core` | static lib | **default** | glob `src/*.cpp` (non-recursive), 13 TUs | public includedir `include/`; links nothing external |
| `sluice_async` | static lib | opt-in (`set_default(false)`, group `async`) | glob `src/async/*.cpp` (non-recursive), 35 TUs + explicit `src/async/detail/context_identity.cpp`, `src/async/detail/request_core.cpp` = 37 TUs | public includedir `include/`; deps `sluice_core`; clang-only TSA flags; `SLUICE_HAS_LIBURING` + public link `uring` under `--liburing` |
| `sluice-copy/-hash/-grep/-tail` | binaries | opt-in (group `apps`) | `apps/*/*.cpp` listed explicitly | deps core+async; `apps.lua` states "public headers only" |
| 131 test/app binaries total | binaries | opt-in | 63 `add_tests`-registered targets + 64 manually-run mutant targets (group `test`) + 4 apps | several test families add include dir `src/async` and define `SLUICE_ASYNC_INTERNAL_TESTING` (internal seam access); uring variants add `SLUICE_HAS_LIBURING`; two `direct_seam_target` families additionally define `SLUICE_FILE_INTERNAL_TESTING` |

Resolved canonical source sets (exact, GRAPH over the two globs):

- `sluice_core` = `blocking_file.cpp blocking_io_pool.cpp buffer.cpp copy.cpp copy_strategy.cpp fault.cpp file.cpp file_resource.cpp io_context.cpp observed.cpp reader.cpp wal.cpp writer.cpp`
- `sluice_async` = the 35 `src/async/*.cpp` listed in `libraries.lua`'s globs at the baseline SHA **plus** `detail/context_identity.cpp`, `detail/request_core.cpp`
- **Orphan TUs**: `src/experimental/uring_io_context.cpp`, `src/experimental/uring_write_batch.cpp` are members of **no** target (production or test) at the baseline. Their headers are still on the public include path.
- Four seam TUs compile into the production `sluice_async` archive: `mutex_test_seam.cpp`, `queue_test_seam.cpp`, `scheduler_fe2_test_seam.cpp` and `fail_fast.cpp` (one guarded region). Under the production configuration their test content preprocesses out entirely (`mutex_test_seam.cpp:1`, `queue_test_seam.cpp:1`, `scheduler_fe2_test_seam.cpp:3`, `fail_fast.cpp:247`); they contribute zero test symbols to the shipped archive and recompile with content only in test binaries that define the seam macro. `GRAPH`+`READ`.

### 1.2 Install / package (XMAKE + LEDGER)

- **No install, export, or package rule exists anywhere** in `xmake.lua` / `xmake/` (GREP; `add_installrules`, `set_kind("package")`, `utils.install*`: zero hits).
- Ledger row "Installed-header / package surface" is `NOT_ASSESSED` and states: *"No install or package target exists in this commit, so no installed surface can be audited; the audit belongs to the legacy/retirement slice (#402)."*
- Consequence: **`INSTALLED = NO` for every header in this census.** "Public" below means *reachable from the public include path* (`include/` is added `{public = true}` on both libraries, so every header under `include/` — including `detail/` and `experimental/` — is consumer-visible), not *installed*.
- No production target adds `src/` to its include path; the `src/async` include dir appears only on test targets (together with the seam define), and apps add only their own source dir.

### 1.3 Header inventory (GRAPH)

| Group | Count | Notes |
|---|---:|---|
| `include/sluice/*.hpp` (core namespace) | 21 | canonical File vocabulary + carriers + legacy stream world + test doubles |
| `include/sluice/blocking/*.hpp` | 1 | `blocking/file.hpp` (direct adapter) |
| `include/sluice/async/*.hpp` | 36 | request/result family, context/host family, runtime world, backends |
| `include/sluice/async/detail/*.hpp` | 17 | core substrate, progress/observer/ready seams, queue/select mechanisms, 2 test seams |
| `include/sluice/detail/*.hpp` | 4 | file semantics oracle, posix retry, uring submit classification, pool impl |
| `include/sluice/experimental/*.hpp` | 2 | uring write batch + io context |
| **Total** | **81** | no umbrella header exists; consumers include per-topic headers |

Headers with **zero in-tree includes** (no `sluice/...` include anywhere in `include/ src/ apps/ tests/`): `async/async_queue.hpp`, `async/condition.hpp`, `async/semaphore.hpp`, `memory_io_context.hpp`. These are shipped public-namespace surfaces with no in-tree consumer; per §8 of the brief this records `IN_TREE_CONSUMERS = 0` and **not** a removal verdict (`UNKNOWN_EXTERNAL_USE` handled in the policy file). Note the distinction from `async/async_rwlock.hpp`, whose **wrapper class** has no caller but whose **header** is directly included by 10 production scheduler TUs (§2.4 H-22).

### 1.4 Transitive exposure facts (GRAPH + READ — load-bearing for F2–F4)

| Fact | Evidence |
|---|---|
| `async/completion.hpp` directly includes `detail/request_arena.hpp`, `detail/request_core.hpp` (and `detail/request_slot.hpp` arrives transitively via the arena) | `completion.hpp:3-5` |
| `async/request.hpp` (the **canonical** result carrier) directly includes `detail/request_core.hpp` + `detail/fail_fast.hpp` | `request.hpp:3-4` |
| `async/async_io_context.hpp` (the canonical context hub) directly includes `completion.hpp`, `detail/observer_protocol.hpp`, `detail/progress_source.hpp`, `detail/ready_sink.hpp`, `detail/request_key.hpp`, `request.hpp`, `request_handle.hpp`, `file_resource.hpp`, `measurement.hpp` | `async_io_context.hpp:3-13`; every async public header that reaches it transitively exposes the whole detail substrate |
| **`async/stackful_io_host.hpp` directly includes `fiber.hpp` + `fiber_ctx.hpp`; private `TaskSlot` holds `Fiber` by value; private members `fiber_ctx::Context driver_ctx_` and `fiber_ctx::Switch*` parameter** | `stackful_io_host.hpp:6-7,181,197,219` — mandatory #454 check: public-header type/layout dependency on Fiber exists today; internalizing Fiber (F3) **must fix this header first** |
| `async/application_runtime.hpp` includes `scheduler.hpp`; private members `std::unique_ptr<Scheduler>`, `SchedulerWakeHandle wake_handle_` (by value) | `application_runtime.hpp:6,182-184` — Scheduler layout is exposed to every ApplicationRuntime consumer |
| `async/scheduler.hpp` includes `fiber.hpp`+`fiber_ctx.hpp`; public nested `WorkerState` embeds `fiber_ctx::Context` by value and `std::atomic<Fiber*>`; public API takes `Fiber&` | scheduler census; F3 implication as above |
| `async/mutex.hpp` unconditionally includes `detail/mutex_test_seam.hpp` — the test-seam **header** is on the production transitive closure of `mutex/lock_guard/wait_queue/scheduler/event/select/group/async_rwlock` (the seam *declarations* are themselves macro-guarded inside the header) | `mutex.hpp:4`; `detail/mutex_test_seam.hpp:10` |
| Other direct public→detail edges: `event.hpp` → `detail/select_port.hpp`+`fail_fast`; `async_rwlock.hpp` → `fail_fast`; `threadpool_backend.hpp` → `detail/{reference_ready_sink,request_key,request_slot}.hpp` + `sluice/detail/posix_retry.hpp` (the `submit_transaction.hpp` edge is seam-gated); `uring_backend.hpp` → `detail/{reference_ready_sink,request_core}.hpp` + `sluice/detail/uring_submit.hpp` | header include blocks (GRAPH; guards noted per caveat 1) |
| `blocking_io_pool.hpp` unconditionally includes `detail/blocking_io_pool_impl.hpp` (template impl leak, namespaced detail) | `blocking_io_pool.hpp:96` |
| 23 of 36 async public headers transitively reach `sluice/detail/file_semantics.hpp` (the semantic oracle) — most, not all | GRAPH closure |
| `WaiterToken` / `RoutingLease` **do not exist** anywhere in `include/` or `src/` | GREP, zero hits — MIG-02's backend-vocabulary removal (C1-E) already happened; recorded `ALREADY_MIGRATED`, no new deletion task |

### 1.5 Configuration macros (READ + XMAKE)

| Macro | Kind | Where | Consumers must see it? |
|---|---|---|---|
| `SLUICE_HAS_LIBURING` | build profile | defined public on `sluice_async` with `--liburing`; in `uring_backend.hpp` it guards the liburing-specific **fragments** (UringConfig, statx/io_uring forwards, UringRingState, slot_capacity, test hooks, and the entire private implementation), while the `UringAsyncBackend` class shell compiles in both profiles **with different member sets** — that member-surface difference is what makes the define ODR-critical; without it construction throws (`uring_backend.cpp:36-40`, `available_` false) | **Yes — ODR-critical**, public define + public link propagate from the single config point |
| `SLUICE_ASYNC_INTERNAL_TESTING` | test seam | defined by 14 test-target declarations (11 generator families + 2 inline targets + shared helper use); also add include dir `src/async`. Opens test regions in **17 headers/TUs**: public `async_io_context.hpp`, `application_runtime.hpp`, `scheduler.hpp`, `stackful_io_host.hpp`, `group.hpp`, `mutex.hpp`, `select.hpp`, `threadpool_backend.hpp`, `uring_backend.hpp`, `task_result.hpp`; detail `request_arena.hpp`, `request_core.hpp`, `progress_source.hpp`, `reference_ready_sink.hpp`, `submit_transaction.hpp`, `mutex_test_seam.hpp`, `queue_test_seam.hpp`; plus the seam TUs of §1.1 | No — must never reach production builds |
| `SLUICE_FILE_INTERNAL_TESTING` | test seam (core world) | defined by 2 test targets (`direct_seam_target`); gates native-call injection regions in `src/file_resource.cpp`, `src/file.cpp`, `src/blocking_file.cpp` via `src/file_test_seams.hpp`; the seam header lives in `src/`, **not** on the public include path — cleaner isolation than the async seam | No |
| `SLUICE_*_MUTANT_*` (~40 names: `SLUICE_B1C_MUTANT_*`, `SLUICE_B2_MUTANT_*`, `SLUICE_C1_MUTANT_*`, `SLUICE_C2E_MUTANT_*`, `SLUICE_D1_MUTANT_*`, `SLUICE_E1_MUTANT_*`, `SLUICE_E2_MUTANT_*`, `SLUICE_STACKFUL_HOST_MUTANT_*`, plus scenario switches `SLUICE_SHUTDOWN_URING`, `SLUICE_E1_CONFORMANCE_URING`, …) | mutation-test seams | test targets only; several are referenced **inside public headers** (`request.hpp`, `request_scope.hpp`, `stackful_io_host.hpp`) to compile named broken variants | No — mutant builds are evidence artifacts |
| `SLUICE_FIBER_ASAN_ENABLED` / `SLUICE_FIBER_TSAN_ENABLED` | sanitizer autodetect | macro text confined to `fiber_ctx.hpp` (+ its TU), but the **variant layout propagates**: `scheduler.hpp` `WorkerState.sched_ctx` and `stackful_io_host.hpp` `driver_ctx_` embed `fiber_ctx::Context` by value, so those public headers' layouts are sanitizer-configuration-dependent (same same-flags ODR discipline as `SLUICE_HAS_LIBURING`) | Derived, not a user switch |
| `EWOULDBLOCK != EAGAIN` guard | POSIX | `error.hpp` | n/a |

No `NDEBUG`/`_FORTIFY`-dependent layout was found in `include/` (GREP); hardening flags are link/flag-level only (census §1.1).

### 1.6 Findings (F0-A)

| ID | Severity | Finding |
|---|---|---|
| FA-1 | P2 | No install/package target exists, so "installed vs not installed" is currently uniform (`NO`); the PACKAGE_PROFILE_POLICY (companion file) still classifies per-profile because a future install rule is an F-phase decision, not a present fact. Note: the four app READMEs claim "installed/public headers only"; "installed" is currently inaccurate wording (no install rule) — recorded as a DOC_CLAIM correction in the DAG file. |
| FA-2 | P2 | `src/experimental/*.cpp` (2 TUs) compile into no target; their headers remain on the public include path. Experimental surface is header-shipped but not linked — exactly the "physically present ≠ supported" split the policy must record. |
| FA-3 | P3 | Four seam TUs whose entire test content compiles out under the production configuration remain members of the `sluice_async` source set; the seam exists only in test binaries that recompile the TUs with `SLUICE_ASYNC_INTERNAL_TESTING`. The seam-bearing **header** set (17 headers, §1.5) is the exposure fact that matters. |
| FA-4 | P3 | `xmake.lua:61` sub-file map comment names a `bench_common` production library that no rule declares; comment-only staleness (the comment is in `xmake.lua`, not `libraries.lua` itself). |
| FA-5 | P3 | Four public headers have zero in-tree includes (`async_queue`, `condition`, `semaphore`, `memory_io_context`); three of them are thin shells over live `Scheduler::` machinery (their behavior lives in `scheduler_*.cpp`), one is an incoherent test double. |

No P0/P1. All findings are recorded, not repaired (F0 forbids production changes).

## 2. F0-B — Semantic role census

Legend for compact columns:

- `PROFILE`: `CANONICAL` (adopted v1 surface), `OPTIONAL_CANDIDATE` (implemented optional-profile candidate; support claim gated on W-04 + ADR-0003 adoption — PROD-02 makes only the single-owner stackful adapter optional and marks the multi-worker task host DEFERRED; **exposure rows riding H-09's public closure inherit its gate**), `COMPAT` (compatibility/dormant retained surface, no promise attached; includes the root-DEFERRED multi-worker runtime world), `EXPERIMENTAL`, `INTERNAL` (detail/host mechanism; still consumer-visible via the public include path), `UNDECIDED` (capability recorded; public-spelling disposition owned by a later phase), `TEST_ONLY` (public-tree test doubles).
- Authority codes (which authority the surface **owns**; `·` = none): `R`esult/settlement, `O`wnership (native/stack/memory), `A`ccess (provenance/legality), `L`ifetime (borrow/binding/reclaim), `P`rogress (driver/notification), `I`dentity/routing, `S`emantic-oracle (shared meaning).
- `STATUS` (evidence state): `VERIFIED` (ledger row VERIFIED), `RECORD` (named evidence record exists; ledger aggregate row open or human review pending), `SHIPPED` (present, no v1 claim), `DORMANT` (zero consumer), `PROPOSED` (ADR pending), `TEST_ONLY`, `HISTORICAL`.
- Consumers: `src`/`tst`/`app`/`hdr` counts from the include graph (`hdr` = only reached via other headers).

### 2.1 Vocabulary / shared-semantics family (root SEM/ERR basis)

| ID | Surface | Path | Kind | Profile | Auth | Role | Status | Consumers | Root IDs |
|---|---|---|---|---|---|---|---|---|---|
| V-01 | `sluice::Result<T>` / `Result<void>` / `make_unexpected` | `sluice/result.hpp` | header-only template | CANONICAL | R | result value carrier for the whole repo (expected-like, fixed `IoError` error type) | VERIFIED (A2 rows use it; carrier itself unassessed) | src 3 · tst 19 · app 6 | ERR-01/02 |
| V-02 | `sluice::IoError` + `Code` (11 canonical codes) + errno mapping | `sluice/error.hpp` | value + inline mapping | CANONICAL | S | canonical error-domain authority; no compat spellings exist | VERIFIED (A1/A2 error-table evidence) | src 7 · tst 2 · app 11 | ERR-01 |
| V-03 | `sluice::EffectReport`, `EffectCertainty` | `sluice/effect.hpp` | value | CANONICAL | S | data-effect currency shared by blocking + request worlds (aliased `IoEffect` in detail) | RECORD (E1) | tst 1 (via others) | ERR-02 |
| V-04 | `sluice::File`, `FileOpen/Access/Existence/InitialContents`, `FileKind/Identity/Info/Size`, `IdentityMatch` | `sluice/file_resource.hpp` | class + vocab | CANONICAL | O, A | **canonical native-resource + access authority** (RAII fd owner; open/close/move per SEM-02; `native_handle()` borrow per LIFE-02) | VERIFIED (A2) | src-core 1 · tst 37 · app 9 | SEM-02/03/07, LIFE-01/02 |
| V-05 | `sluice::blocking::{read,write,read_at,write_at,sync_data,sync_all,file_info,size,resize,read_exact(_at),write_all(_at)}`, `CompositionOutcome/End` | `sluice/blocking/file.hpp` | free functions | CANONICAL | A, S | direct-invocation adapter over canonical `File`; shares the semantic oracle, no RequestCore | VERIFIED (A2) | src 1 · tst 17 · app 1 | SEM-01..06, INV-01, W-01 |
| V-06 | `sluice::detail::file_semantics` (precheck/classify/compose/IoOutcome/covers) | `sluice/detail/file_semantics.hpp` | constexpr oracle | INTERNAL | S | **the shared semantic oracle**: open legality, precedence, short-IO/EOF, composition, effect certainty, durability coverage | RECORD (A2 direct rows VERIFIED; A1 oracle row `IMPLEMENTED_UNVERIFIED` — effect/durability halves remain reference-model) | src 10 · tst 8 | SEM-03..06, ERR-02 |
| V-07 | `sluice::detail::posix_retry` (`retry_on_eintr`) | `sluice/detail/posix_retry.hpp` | inline template | INTERNAL | · | syscall EINTR retry mechanism | SHIPPED | src 3 · app **1** · tst 0 | SEM-05 (EINTR rule) |
| V-08 | `sluice::detail::uring_submit` classification | `sluice/detail/uring_submit.hpp` | inline | INTERNAL | · | submit-outcome classification vocabulary (uring) | SHIPPED | src 2 · tst 1 | BACKEND-03 |
| V-09 | `sluice::detail::blocking_io_pool_impl` | `sluice/detail/blocking_io_pool_impl.hpp` | template impl | INTERNAL | · | impl completion for `blocking_io_pool.hpp`, unconditionally included by it | SHIPPED | hdr (via L-05) | — |

Note: `sluice/file.hpp` is **not** in this family — it is the legacy stream world (L-01 below). The file-name/role inversion is recorded exactly because the census key forbids path-based classification.

### 2.2 Request / result / identity family (root REQ/HANDLE)

| ID | Surface | Path | Kind | Profile | Auth | Role | Status | Consumers | Root IDs |
|---|---|---|---|---|---|---|---|---|---|
| R-01 | `sluice::async::Request<T>` | `sluice/async/request.hpp` | move-only class | CANONICAL | R, L | **unique public result/settlement binding** (nonblocking try/take, always-on nonterminal-release fail-fast per HANDLE-02) | RECORD (B2 record; ledger HANDLE row open) | tst 5 direct + via hub | ARCH-02, HANDLE-01..03 |
| R-02 | `sluice::async::RequestId` | `sluice/async/request.hpp` | value | CANONICAL | I | copied identity = lookup/cancel token while context lives (REQ-01) | RECORD (B2) | via R-01 | REQ-01 |
| R-03 | `sluice::async::RequestObservation<T>`, `RequestReadiness` | `sluice/async/request.hpp` | value | CANONICAL | R | observation view (readiness + result + `EffectReport`); not an authority | RECORD (B2/E1) | via R-01 | ERR-02, HANDLE-01 |
| R-04 | `sluice::async::CancelDisposition` (5 values incl. `physical_interruption_unsupported`) | `sluice/async/request.hpp` | enum | CANONICAL | S | cancel-disposition vocabulary (CANCEL-01) | RECORD (E1 `IMPLEMENTED_UNVERIFIED`, human review pending) | via R-01 | CANCEL-01 |
| R-05 | `sluice::async::RequestHandle` + `RequestHandleState` | `sluice/async/request_handle.hpp` | value | UNDECIDED (spelling) | I | the copied-identity/state-probe **capability** is MIG-02-required (public responsibility ≠ copied identity); the `RequestHandle` **spelling** is a second public identity/state-probe vocabulary beside `RequestId` (R-02), still bound to Completion-era request APIs — its disposition (retain | fold into `RequestId`/context lookup | internalize) is **UNDECIDED and owned by F2** per #402; F0 records capability and dependencies only | RECORD (implemented; identity fallback via arena binding — DAG X-02) | src 1 · tst 2 | REQ-01, MIG-02 |
| R-06 | `sluice::async::Completion<T>` — compatibility role | `sluice/async/completion.hpp` | class | **COMPAT** | R | caller-owned result storage; MIG-02: *"Compatibility only; remove as canonical result authority"*; consumer set: 15 test files include the header, **25 test files name the type**, and **all 4 apps construct/settle `Completion` objects in their task code** (via `await_op_helpers` include + direct members) | SHIPPED (retained) | tst 25 · app 4 (+ producers in §2.4) | MIG-02 |
| R-07 | `Completion<T>` — internal release-substrate role | same | class privates | INTERNAL | L | non-public friend surface: `install_core_binding_for_backend` / `release_public_binding` / `publish_from_reap`; the backend publishes **through** it; release path still routes via `RequestArena` when the legacy binding is installed | SHIPPED (retained) | src (friends) | REQ-05, MIG-02 |
| R-08 | `sluice::async::Batch` | `sluice/async/batch.hpp` | class | COMPAT | R | ordered batching facade storing private `Completion` cells; exposes `BatchResult` (plain `Result`), never `Request`/`RequestHandle` | SHIPPED (near-dormant: 1 test) | tst 1 | MIG-02 (compat family) |
| R-09 | `detail::RequestKey/ContextIdentity/SlotIndex/Generation`, `detail::context_identity` allocation | `detail/request_key.hpp`, `detail/context_identity.{hpp,cpp}` | value | INTERNAL | I | **identity domain authority** (context,slot,generation; exhaustion closes admission before wrap) | RECORD (B1-A) | src-async 2 · tst 1 (each header) | REQ-01 |
| R-10 | `detail::RequestCore` | `detail/request_core.hpp/.cpp` | class | INTERNAL | R, L, I | **acceptance / terminal arbitration / publication / public-binding / observer-episode / reclaim authority**, owned by `AsyncIoContext` | RECORD (B1-B/C/D + B2 + C1 + E1 + E2 records; aggregate ledger rows open) | src 4 · tst 13 | REQ-01..06, ARCH-01/02 |
| R-11 | `detail::RequestArena` + `detail::RequestSlot` + `SlotHandle` | `detail/request_arena.hpp`, `detail/request_slot.hpp` | inline classes | INTERNAL | R, L | **legacy substrate** (reserve→…→reap pipeline with own publication binding + ready ring). Production-**unarmed**: no `RequestArena` instance exists in `src/`; the compile-time-reachable roles are `Completion`'s legacy release path (`release_completed_binding`, armed only by the legacy `install_binding` helper) and the seam-gated `submit_transaction` choreography; the **identity fallback** `AsyncBackend::identity_of` also reads arena fields when no core binding is installed (DAG X-02) | SHIPPED (legacy) | completion.hpp · request_handle.cpp · submit_transaction.hpp · 2 test probes | MIG-02 "RequestArena → converge to bounded core" |
| R-12 | `detail::submit_transaction<Policy>` | `detail/submit_transaction.hpp` | template | INTERNAL (TEST_ONLY today) | · | staged submit orchestration over the **arena** (not the core); included only under `SLUICE_ASYNC_INTERNAL_TESTING` | TEST_ONLY | src 0 · tst (seam) | — |
| R-13 | `detail::observer_protocol` (5 enum families) | `detail/observer_protocol.hpp` | enums | INTERNAL | S | observer-episode protocol vocabulary (OBS-01/02 outcomes) | VERIFIED (C1) | hdr (via hub) | OBS-01/02 |
| R-14 | `detail::RequestState/TerminalResult/BorrowMetadata/CompletionBinding` (slot vocabulary) | `detail/request_slot.hpp` | vocab | INTERNAL | L | per-slot lifecycle vocabulary of the **arena** world | SHIPPED (legacy) | tst 1 | — |
| R-15 | `sluice::async::RequestScope`, `ScopeTicket<T>`, `ScopeCleanupPolicy`, `ScopeWaitStatus` | `sluice/async/request_scope.hpp` | class + template ticket | CANONICAL | R, L | **W-02 owned-driver scope** (HOST-02): reserves tracking before acceptance, owns accepted `Request<T>`s, settles on every exit incl. exceptions; claims the context's `ProgressOwner` for its lifetime | VERIFIED (D1) | src-async 1 · tst 2 (+ via hub) | HOST-02, W-02, LIFE |

Key authority statement: the repo currently has **one** canonical request result/settlement authority (`Request<T>` over `RequestCore`, scoped by R-15) and **one** retained compatibility result carrier (`Completion<T>`) whose release path still partially routes through the legacy `RequestArena`. Task-world result cells that are **not** request-settlement authorities are recorded separately: `Future<T>` (H-14, Group-only), `TaskResultSlot<T>` (H-26, app bridge), `Task<T>` (L-05, pool), `Batch`'s private cells (R-08).

### 2.3 Progress / observer / ready-routing family (root PROG/OBS)

| ID | Surface | Path | Kind | Profile | Auth | Role | Status | Consumers | Root IDs |
|---|---|---|---|---|---|---|---|---|---|
| P-01 | `detail::ProgressSource` (+`Token`, `WakeReason`, `PhysicalProbe`) | `detail/progress_source.hpp` | class (inline methods) | INTERNAL | P | **context-owned progress authority**: eventfd + saturating (progress,control) epoch pairs, observe→acknowledge control retirement, park handshake, fd lend/retire | VERIFIED (C2-A..E) | hdr (via hub) | PROG-01..04, ARCH-01 |
| P-02 | `detail::BackendProgressPort` | same | narrowed view | INTERNAL | P | backend-facing capability: `signal()` + one-shot kernel notification registration grant; backend can never wait/drain/rebind | VERIFIED (C2-A) | src 2 (backends) | PROG-01/03 |
| P-03 | `detail::SynchronousReadySink` + `ReadyEvent` + `OperationKind` | `detail/ready_sink.hpp` | interface | INTERNAL | I | ready-notification routing contract (terminal-ready event → sink); `OperationKind` is the domain-wide op-kind enum | VERIFIED (C1/C2) | hdr + src (backends, scheduler) | OBS-02, ARCH-01 |
| P-04 | `detail::ReferenceReadySink` | `detail/reference_ready_sink.hpp` | no-op sink | INTERNAL | · | null-object sink embedded in **both production backends** (production discards; test seam counts) | SHIPPED | src 2 | — |
| P-05 | context observer API (`attach_observer/cancel_observer/retire_delivery` on `AsyncIoContext`, `ObserverAttachment/ObserverCancelResult`) | `sluice/async/async_io_context.hpp` | methods | CANONICAL | L | observer attach/cancel/retire entries over the core's episode protocol (Completion-based spellings today) | VERIFIED (C1) | tst 13 (via hub) | OBS-01..04 |
| P-06 | `ProgressOwner` (driving-authority handle) | `sluice/async/async_io_context.hpp` | class | CANONICAL | P | release handle for the context's **fixed** progress owner (no live transfer, PROG-01) | VERIFIED (C2-D) | src 2 · tst · app (via hub) | PROG-01, SHUT-01 |

Not found (recorded `ALREADY_MIGRATED`, per #454 §6): `WaiterToken`, `RoutingLease`, `BackendWaitSource` — removed by C1-E/C2-A; no deletion task remains.

### 2.4 Context / host / runtime family (root INV/HOST/SHUT)

| ID | Surface | Path | Kind | Profile | Auth | Role | Status | Consumers | Root IDs |
|---|---|---|---|---|---|---|---|---|---|
| H-01 | `sluice::async::AsyncIoContext` | `sluice/async/async_io_context.hpp` | class | CANONICAL | I, L, P | **context authority**: identity, budgets/admission/health, owns {RequestCore, ProgressSource, AsyncBackend}; shutdown state machine driver (ADR-0004 §1: root's `IoContext` name maps here until the F-phase rename) | VERIFIED (C2/E2) + RECORD (E1) | src 2 · tst 18 · app 4 | ARCH-02, SHUT-01..04 |
| H-02 | `sluice::async::AsyncBackend` | same | abstract class | INTERNAL (mechanism seam) | A | backend interface: submit/poll/cancel/retirement seam; holds core_/progress_port_/routing_sink_ installed by the context; forbidden to mention Scheduler/Fiber vocabulary (ARCH-01) — verified clean | RECORD | src 2 backends | BACKEND-01..03 |
| H-03 | `ReadOp/WriteOp/SyncDataOp/SyncAllOp/FileInfoOp/SizeOp`, `NativeFileRef` | same | value structs | CANONICAL | A | operation descriptors + **borrow/provenance representation** (`fd`,`access`); MIG-02: internal borrow model, "no second ownership model" | VERIFIED (via slice records) | everywhere (hub) | LIFE-01/02, MIG-02 |
| H-04 | `ShutdownPolicy`, `ShutdownOutcome`, `close_admission/request_stop/shutdown` | same | enum/methods | CANONICAL | L | shutdown contract spelling (ADR-0004 §1 public spelling) | VERIFIED (E2) | tst 18 · app 0 | SHUT-01..03 |
| H-05 | `op_helpers`: `read_all/write_all/sync_data_all/sync_all_all` | `sluice/async/op_helpers.hpp` | free functions | COMPAT | R | completed-return helpers over context + internal `Completion` cells — **contain a `one_step` busy-spin drive loop** (`op_helpers.cpp:18-24,84-90`: `while (!c.ready()) (void)ctx.poll();`), making them the live instance of MIG-02's "busy-poll helpers" row as well | SHIPPED | src 1 · tst 1 | MIG-02 (two rows) |
| H-06 | `await_op_helpers`: `await_take/await_drain/await_read_once/await_read_fill/await_write_exact` | `sluice/async/await_op_helpers.hpp` | free functions | COMPAT (DEFERRED) | R | Completion-spelled conveniences over `RuntimeTaskContext` — conveniences of the multi-worker runtime (root-**DEFERRED** per PROD-02), cannot outlive that world (DAG X-05); the Request-typed W-04 conveniences belong to the OPTIONAL_CANDIDATE host spelling (DAG §4) | RECORD (D2 pending human review) | src 2 · tst 2 · app 4 | HOST-03, W-04, PROD-02 |
| H-07 | `async/file.hpp`: `await_read_at/await_write_at/await_sync_data/await_sync_all` | `sluice/async/file.hpp` | free functions | COMPAT (DEFERRED) | R | File-taking variants of the await conveniences over `RuntimeTaskContext` — same DEFERRED runtime world as H-06 (DAG X-05) | RECORD (D2) | src 1 · tst 9 · app 3 | HOST-03, SEM-01, PROD-02 |
| H-08 | `RuntimeTaskContext`, `RuntimeBuilder`, `ApplicationRuntime`, `RuntimeTaskFn` | `sluice/async/application_runtime.hpp` | classes | COMPAT (DEFERRED) | L, P | **multi-worker host runtime** — root-**DEFERRED** configuration per PROD-02 (not optional-supported): driver thread + Scheduler + root Group; owns io_ctx/sched/root_group; task lifecycle state machine; `await_completion/cancel_waiter` are Completion-spelled (DAG X-16) | RECORD (D2 `IMPLEMENTED_UNVERIFIED`) | src 1 · tst 1 · app 4 | HOST-01, MIG-02, PROD-02 |
| H-09 | `StackfulIoHost`, `StackfulHostConfig`, `IoTaskContext` | `sluice/async/stackful_io_host.hpp` | classes | OPTIONAL_CANDIDATE | L, P | **narrow single-owner stackful adapter** — the only optional-profile configuration PROD-02 admits; support claim gated on W-04 pass + ADR-0003 adoption (until then candidate, not supported product): claims progress owner in `run()`, bounded tasks/stacks, stop token, `_for` bounded parks; **fiber type/layout exposure** (§1.4) | PROPOSED (ADR-0003; D2 row `IMPLEMENTED_UNVERIFIED`) | src 1 · tst 1 | HOST-01/02/03, W-04, PROD-02 |
| H-10 | `Scheduler` + `SchedulerWakeHandle` + `RunMode` + `WorkerState` | `sluice/async/scheduler.hpp` | class (781 lines) | COMPAT (DEFERRED) | L, P | scheduling domain of the root-DEFERRED multi-worker runtime: workers, wait registry (keyed by `detail::RequestKey`), timer heap, admission for every runtime primitive; routes `ReadyEvent` to fibers via `ReadyRoutingSink` (core-protocol consumer); **Completion-spelled await/cancel surface** (`await_completion_*`, `cancel_waiter`) | RECORD | src 18 · tst 1 | HOST-01, MIG-02, PROD-02 |
| H-11 | `Fiber`, `FiberState`, `CompletionWaitOutcome` | `sluice/async/fiber.hpp` | class | INTERNAL (host substrate mechanism) | O | stackful execution **mechanism substrate** shared by the host layer (register context + cancel token + completion-wait outcome; does **not** own its stack — Group/ApplicationRuntime own stack bytes); the public-header **exposure** of this mechanism to the legacy runtime world is a separate surface (H-29) | SHIPPED | src 1(+12 via scheduler) · hdr 5 (group/async_mutex/wait vocab — DAG X-17) | HOST-01, MIG-02 |
| H-12 | `fiber_ctx::{Context,Switch,Entry,context_switch,…}` | `sluice/async/fiber_ctx.hpp` | ABI + asm | INTERNAL (host substrate mechanism) | O | x86_64-Linux context-switch mechanism; sanitizer-variant layout (propagates into scheduler/stackful headers, §1.5); public-header exposure recorded as H-29 | SHIPPED | src 12 · hdr 3 | — |
| H-13 | `Group` | `sluice/async/group.hpp` | class | COMPAT (DEFERRED) | O, R | fork-join over threads or evented fibers — part of the root-DEFERRED multi-worker runtime (PROD-02); owns stack bytes + `Future` cells + group `CancelToken`; **complete-type dependency on `Fiber`** (`unique_ptr<Fiber>` members, `init_fiber`) — F3 prerequisite (DAG X-17); retained as ApplicationRuntime's root group (HOST-01) | SHIPPED | src 2 · hdr 1 | HOST-01, MIG-02, PROD-02 |
| H-14 | `Future<T>` | `sluice/async/future.hpp` | template | COMPAT | R | one-shot result cell over `WaitPolicy`; **only consumer is `Group`** | SHIPPED (Group-only) | hdr 1 | MIG-02 "Group/Future/WaitPolicy" |
| H-15 | `WaitPolicy`, `ThreadedWaitPolicy`, `EventedWaitPolicy` | `sluice/async/wait_policy.hpp`, `evented_wait_policy.hpp` | interface | COMPAT | · | blocking strategy seam for `Future` (threaded vs scheduler-evented) | SHIPPED (Future-only) | hdr 2 | MIG-02 |
| H-16 | `CancelToken/CancelState/CancelGuard/CancelProtection/check_cancel` | `sluice/async/cancel.hpp` | latch vocab | COMPAT (DEFERRED-world vocabulary exposure) | L | cooperative cancellation latch + epoch acknowledge protocol — the **legacy multi-worker runtime exposure** role (Future/Fiber/Group consumers); the same physical surface's **current H-09 public-signature dependency** role is a separate row (H-30, OPTIONAL_CANDIDATE exposure); **not** a request-cancel surface (CANCEL-01 lives in `Request::cancel`) | SHIPPED | hdr 5 · src-async 1 (+ host tests) | HOST-01, MIG-02 runtime family |
| H-17 | `Event` | `sluice/async/event.hpp` | class | COMPAT | L | runtime event flag + select arm; live in scheduler/select paths, wrapper class consumed only via scheduler | SHIPPED | src 12 | MIG-02 dormant-primitives family |
| H-18 | `select()/SelectResult/EventSelectCase/TimerSelectCase`, `select_fwd` concept | `sluice/async/select.hpp`, `select_fwd.hpp` | variadic template | COMPAT | L | runtime select over {event,timer} arms; arbitration in Scheduler | SHIPPED | src 10 | MIG-02 dormant-primitives family |
| H-19 | `WaitNode/WaitResume/ActorId/WaitOutcome`, `WaitQueue` | `sluice/async/wait_node.hpp`, `wait_queue.hpp` | intrusive vocab | INTERNAL (mechanism) | L | scheduler wait-list machinery; mutations friended to Scheduler only; name `Fiber*` (DAG X-17) | SHIPPED | hdr only (via scheduler) | — |
| H-20 | `TimerRegistration`, `deadline_tick_t` | `sluice/async/timer_registration.hpp` | vocab | INTERNAL (mechanism) | L | deadline-heap arm protocol node | SHIPPED | hdr only | — |
| H-21 | `Mutex`, `LockGuard`, `thread_annotations` | `sluice/async/mutex.hpp`, `lock_guard.hpp`, `thread_annotations.hpp` | wrapper + macros | INTERNAL (mechanism) | · | host-internal `std::mutex` wrapper + Clang TSA shims | SHIPPED | hdr + src | — |
| H-22 | `AsyncMutex`, `AsyncCondition`, `Semaphore`, `AsyncRwLock`, `AsyncQueue<T>` | `sluice/async/{async_mutex,condition,semaphore,async_rwlock,async_queue}.hpp` | wrapper templates | COMPAT (**wrapper-dormant**) | L | thin public shells over live `Scheduler::{mutex,condition,sem,rwlock,queue}_*` machinery. Consumer split: `condition/semaphore/async_queue` headers have **zero** in-tree includes; `async_mutex.hpp` is included once (by `condition.hpp`); **`async_rwlock.hpp` is directly included by 10 production scheduler TUs** (its out-of-line methods live there) — the *class* has no caller, the *header* is mechanically load-bearing | SHIPPED | per-header (at left) | MIG-02 *"public dormant synchronization primitives — outside v1"* |
| H-23 | `detail::queue_item`, `detail::queue_port`, `queue_test_seam` | `detail/queue_*.hpp` | mechanism | INTERNAL (runtime world) | L | bounded MPMC channel mechanics under the dormant `AsyncQueue<T>` shell; scheduler-driven | SHIPPED | src 2 | — |
| H-24 | `detail::select_port`, `detail::select_registration` | `detail/select_*.hpp` | mechanism | INTERNAL (runtime world) | L | select arbitration records + timer-heap unification | SHIPPED | src 13 · hdr | — |
| H-25 | `detail::fail_fast` (~49 named `[[noreturn]]` invariants) | `detail/fail_fast.hpp/.cpp` | vocabulary | INTERNAL | S | domain-wide invariant-violation diagnostics; names cover **both** substrate generations (arena + core) | RECORD | src 23 | HANDLE-02, SHUT-04 (always-on diagnostics) |
| H-26 | `TaskResultSlot<T>`, `translate_task_exception<T>`, `run_task_to_result` | `sluice/async/task_result.hpp` | header-only templates | COMPAT (DEFERRED; rides H-08) | R, S | app-facing blocking bridge into the root-DEFERRED multi-worker runtime: one-shot task-result mailbox (first publish wins) + exception→`IoError` translation + inline ApplicationRuntime lifecycle driver; **not** a request-settlement authority | SHIPPED | app 4 · tst 8 | HOST-01, MIG-02 completed-return family |
| H-27 | `ThreadPoolBackend` (+`ThreadPoolConfig`) | `sluice/async/threadpool_backend.hpp` | concrete backend | CANONICAL (required profile spelling) | O, A | required ThreadPool request backend: owns worker threads + dispatch/publication rings + delivery records; publishes through the context-owned core; public class that apps construct directly for explicit backend selection | RECORD (E1) | src-async 2 · tst 19 · app 6 TUs | BACKEND-01..03, PROD-02 |
| H-28 | `UringAsyncBackend` (+`UringConfig` when guarded) | `sluice/async/uring_backend.hpp` | concrete backend | CANONICAL (required supported profile; availability-gated) | O, A | io_uring request backend: ring transport, cookie ledger/router, statx metadata buffers, eventfd registration, poison/recover; body fragments guarded by `SLUICE_HAS_LIBURING` (stub throws at construction) | RECORD (E1; real-kernel evidence recorded) | src-async 3 · tst 16 | BACKEND-01..03, PROD-02 |
| H-29 | public `fiber.hpp`/`fiber_ctx.hpp` spellings (legacy-runtime exposure) | `sluice/async/fiber.hpp`, `sluice/async/fiber_ctx.hpp` | public headers | COMPAT (DEFERRED-world exposure) | · | the H-11/H-12 mechanism as **public-header exposure to the legacy multi-worker runtime**: `Scheduler`/`Group`/`AsyncMutex`/wait vocabulary name `Fiber` in public headers (complete-type edge X-17; layout edge X-04 via `application_runtime.hpp`) and the sanitizer-variant layout propagates into `scheduler.hpp` (§1.5); F3 internalizes these spellings only after the 🔒 prerequisites (DAG §5). The separate **current H-09 compile/layout exposure** is row H-31 | SHIPPED | src 12 · hdr 5 | MIG-02, PROD-02 |
| H-30 | `CancelToken` as H-09 public-signature dependency | `sluice/async/cancel.hpp` (via `stackful_io_host.hpp`) | public signature + private member | OPTIONAL_CANDIDATE (exposure; inherits the H-09 gate) | · | `stackful_io_host.hpp:4` publicly includes `cancel.hpp`; `IoTaskContext::token()` returns `const CancelToken&` (`stackful_io_host.hpp:39`); private `StackfulIoHost::stop_token_` member (`:221`). Until F3 re-homes the stop/latch vocabulary, a clean-room OPTIONAL_CANDIDATE host package must carry `cancel.hpp`; the retention spelling is not automatically canonical | SHIPPED | via H-09 (src 1 · tst 1) | HOST-01, W-04, PROD-02 |
| H-31 | `Fiber`/`fiber_ctx` as H-09 public compile/layout exposure | `sluice/async/fiber.hpp`, `sluice/async/fiber_ctx.hpp` (via `stackful_io_host.hpp`) | public headers (compile + TYPE_LAYOUT) | OPTIONAL_CANDIDATE (exposure; inherits the H-09 gate) | · | `stackful_io_host.hpp:6-7` directly includes `fiber.hpp`+`fiber_ctx.hpp`; private `TaskSlot::fiber` embeds `Fiber` **by value** (`:181`), `fiber_ctx::Context driver_ctx_` (`:219`), `fiber_ctx::Switch*` bridge parameter (`:197`) — every TU compiling the public header needs both headers, and the sanitizer-variant layout propagates into this public header (§1.5, X-03). Until the F3 boundary fix (X-03/D-16), a clean-room OPTIONAL_CANDIDATE host package must carry `fiber.hpp`/`fiber_ctx.hpp` even though the mechanism's target disposition is KEEP_INTERNAL | SHIPPED | via H-09 (src 1 · tst 1) | HOST-01, W-04, PROD-02 |

### 2.5 Resource / stream world (`sluice_core`, root SEM/LIFE + MIG-02)

| ID | Surface | Path | Kind | Profile | Auth | Role | Status | Consumers | Root IDs |
|---|---|---|---|---|---|---|---|---|---|
| L-01 | `sluice::FileReader`, `sluice::FileWriter` | `sluice/file.hpp` (name!) | classes | COMPAT | **O** | **independent native-fd owners**: each holds raw `fd_`, own `close()` (first-attempt-consumes), own sync via `SyncableWriter`, and **true vectored ops** (`read_vec/read_vec_at/write_vec/write_vec_at` via `readv/writev`, the repo's only native vectored implementation — DAG §4 expressiveness note); no dependency on canonical `File` for ownership — candidate **second native-resource authority** pending the consumer audit | SHIPPED (legacy) | src 2 · tst 6 | MIG-02 *"legacy Reader/Writer/FileReader/FileWriter — retire from canonical surface after audit"* |
| L-02 | `sluice::Reader`, `sluice::Writer` | `sluice/reader.hpp`, `writer.hpp` | abstract | COMPAT | · | pure stream interfaces, own nothing (no fd, no close); embed some composition algorithm (`read_exact`, `stream_to`, `write_all_vec`) | SHIPPED | src-core 3 · hdr many | MIG-02 |
| L-03 | `sluice::BufferedReader/Writer`, `BufferedReadable` | `sluice/buffer.hpp`, `buffered_readable.hpp` | decorators | COMPAT | · | buffering decorators over borrowed Reader/Writer (borrowed scratch span; dtor asserts clean flush) — fully delegate resource semantics, **not** a second resource authority | SHIPPED | src 1 | MIG-02 |
| L-04 | `sluice::IoContext`, `BlockingIoContext`, `OpenReaderOptions/WriterOptions` | `sluice/io_context.hpp` | factory | COMPAT | · | abstract factory producing `unique_ptr<Reader|Writer>` by path; owns no fd, no results, no progress. **Name-collision hazard** with root `IoContext` (owned by `sluice::async::AsyncIoContext` per ADR-0004 §1) | SHIPPED (legacy) | src 1 · tst (via L-01) | MIG-02 |
| L-05 | `sluice::BlockingIoPool`, `Task<T>`, `PoolStats` | `sluice/blocking_io_pool.hpp` (+impl) | pool | COMPAT | O, R | self-contained worker pool owning threads + admission; `Task<T>` owns a result cell (move-only `get()`) | SHIPPED | src 1 · tst | MIG-02 legacy-wrapper family |
| L-06 | `sluice::wal::{write_record,write_record_vec,read_record,WalWriter}` | `sluice/wal.hpp` | protocol | COMPAT | S | record framing + written→flushed→durable LSN vocabulary over **borrowed** `Writer` + optional `SyncableWriter`; owns no fd; delegates durability decision; `write_record_vec` reaches the only native `write_all_vec` (L-01) | SHIPPED | src 1 · tst 1 | — |
| L-07 | `sluice::copy_all` (5 overloads), `CopyStrategy/Options/Decision`, `CopyLimit`, `IoSlice/ConstIoSlice` | `sluice/copy.hpp`, `copy_strategy.hpp`, `limit.hpp`, `iovec.hpp` | free fns + vocab | COMPAT | · | copy algorithm + policy/outcome vocabulary over borrowed Reader/Writer | SHIPPED | src 3 · tst | — |
| L-08 | `sluice::SyncableWriter` | `sluice/sync.hpp` | interface | COMPAT | S | durability capability interface (`sync_data`/`sync_all`) separate from `Writer::flush` | SHIPPED | src 2 | SEM-06 (spelling ancestor) |
| L-09 | 7 stats structs (`SyscallStats`…`AsyncStats`), `ReaderStats/WriterStats`, `ObservedReader/Writer` | `sluice/measurement.hpp`, `sluice/observed.hpp` | counters | COMPAT | · | passive caller-owned instrumentation (three split homes: measurement/observed/PoolStats) | SHIPPED | src 3 · tst · app | — |
| L-10 | `FaultReader/FaultWriter/FaultPlan`, `MemoryReader/MemoryWriter` | `sluice/fault.hpp` | doubles | TEST_ONLY | · | in-memory doubles + fault injection shipped in the public tree | TEST_ONLY | src 1 · tst | — |
| L-11 | `sluice::MemoryIoContext` | `sluice/memory_io_context.hpp` | double | TEST_ONLY | · | in-memory `IoContext` factory; **writer side does not round-trip the store** (recorded defect, not repaired in F0) | TEST_ONLY, **DORMANT** (zero in-tree includes) | none | — |
| L-12 | `sluice::experimental::UringWriteBatch`, `UringIoContext`, `UringWriteResult` | `sluice/experimental/*.hpp` | classes | EXPERIMENTAL | O | raw-fd/path-based uring write loop; no `File`, no request identity, no provider-affinity/registered-buffer surface; **TUs orphaned from every target** (FA-2) | EXPERIMENTAL | src-orphan 2 | LIFE-03 constraint, MIG-02 experimental row |

Per the brief's BufferedReader example: `BufferedReader{Reader&}`/decorators (L-03) fully delegate resource semantics and are **not** counted as second authorities; `FileReader/FileWriter` (L-01) **are** flagged because they independently define open/close/move/sync/vectored behavior on a raw `fd_` without going through `sluice::File` — the consumer/audit question for F1 is whether that independence carries any retained consumer obligation.

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
| `apps/*/README.md` | app-level usage docs; make current DOC_CLAIM edges on `RuntimeTaskContext::await_completion`, "strict Completion lifetime discipline", "ApplicationRuntime lifecycle usable", and an inaccurate "installed/public headers only" phrase (DAG X-18) |
| `review/ui-review.md` | visualization-slice evidence |
| `AGENTS.md`, `CHANGELOG.md` | working rules / history |

## 3. Summary matrices (#454 §19)

### Profile summary (73 surface rows; ID sets are the source of truth, not the counts)

| Field | Rows |
|---|---|
| CANONICAL_PROFILE_SURFACES (17) | V-01..V-05, R-01..R-04, R-15, H-01, H-03, H-04, H-27, H-28, P-05, P-06 |
| OPTIONAL_CANDIDATE_SURFACES (3; all gated on W-04 + ADR-0003 adoption) | H-09 + its public-closure exposure rows H-30 (`CancelToken` signature dependency), H-31 (`Fiber`/`fiber_ctx` compile/layout exposure) — the exposure rows inherit the host gate |
| COMPATIBILITY_ONLY_SURFACES (25) | R-06, R-08, H-05, H-06, H-07, H-08, H-10, H-13, H-14, H-15, H-16, H-17, H-18, H-22, H-26, H-29, L-01..L-09 (the host-family members are the root-DEFERRED multi-worker runtime world per PROD-02) |
| EXPERIMENTAL_SURFACES (1) | L-12 |
| INTERNAL_ONLY_SURFACES (23) | V-06..V-09, R-07, R-09..R-11, R-13, R-14, P-01..P-04, H-02, H-11, H-12, H-19..H-21, H-23..H-25 |
| UNDECIDED_SPELLING_SURFACES (1; F2-owned) | R-05 |
| TEST_ONLY_SURFACES (3) | L-10, L-11, R-12 |
| HISTORICAL_RESEARCH_SURFACES | 4 asset sets (§2.6): Lean family, baseline TLA 8, docs/archive, research/RESULTS |

### Authority summary

| Field | Holders (rows) |
|---|---|
| RESULT_AUTHORITIES — request world | R-01 `Request<T>` (canonical, unique); R-06/R-07 `Completion<T>` (compat carrier + release substrate); R-11 `RequestArena` (legacy substrate, production-unarmed release path + identity fallback) |
| RESULT STORAGE CELLS — task/pool world (not request-settlement authorities) | H-14 `Future<T>`, H-26 `TaskResultSlot<T>`, L-05 `Task<T>`, R-08 `Batch` private cells |
| NATIVE_RESOURCE_AUTHORITIES | V-04 `sluice::File` (canonical); L-01 `FileReader/FileWriter` (independent fd owners — flagged); L-05 `BlockingIoPool` (threads/queue, not file); L-12 experimental batch (raw fd, orphaned) |
| PROGRESS_AUTHORITIES | P-01 `ProgressSource` (unique, context-owned); P-02 `BackendProgressPort` (narrowed capability, no independent truth) |
| HOST_ROUTING_AUTHORITIES | P-03 `SynchronousReadySink`/`ReadyEvent` routing contract; H-01 `AsyncIoContext` (installs routing); H-10 `Scheduler` (runtime-world routing only) |
| SETTLEMENT_AUTHORITIES | R-10 `RequestCore` (terminal arbitration/publication); R-15 `RequestScope` (owned-driver settlement, W-02); H-01 (shutdown/settlement driver per ADR-0004); H-09 `StackfulIoHost` (task-region settlement, host-scoped) |
| LIFETIME_AUTHORITIES | V-04 `File` (borrow/close per LIFE); H-01 (bindings/destroyable); R-01 (public binding); R-15 (scope-owned requests); P-06 `ProgressOwner` (drive domain); H-16 `CancelToken` (host stop latch); H-13 `Group` (stack bytes) |

### Uncertainty

| Field | Value |
|---|---|
| UNKNOWN_ROWS | 0 rows carry UNKNOWN in a load-bearing field; per-field UNKNOWNs: none remaining after the three sub-censuses (consumer-side unknowns are external-use flags, tracked in the policy file) |
| UNKNOWN_EXTERNAL_USE_ROWS | tracked per family in `f0-package-profile-policy.md` §3 (no repo compatibility policy exists, so every COMPAT/EXPERIMENTAL/DORMANT row has `UNKNOWN_EXTERNAL_USE = unknown`) |
| AMBIGUOUS_ROLE_ROWS | 2: `Completion<T>` (compat carrier vs internal release substrate — split into R-06/R-07 rather than resolved), `RequestHandle` (public vocabulary vs future internal-only token — MIG-02 separation implemented, adoption state RECORD; spelling disposition **UNDECIDED**, owned by F2 per #402 — R-05/D-17 record capability and dependencies only) |

## 4. Adversarial review record (F0-F)

Three independent reviewers (A: authority duplication; B: exposure/package; C: consumer/migration) reviewed commit `92da4301` and returned `REQUEST_CHANGES` with P0=0, P1=8, P2=14, P3=8. All findings were adjudicated against the tree and incorporated in this revision:

| Finding | Resolution |
|---|---|
| A-1/B-1 (missing `RequestScope` row) | added R-15; matrices/policy updated |
| A-2/B-1 (missing `task_result.hpp` row) | added H-26; RESULT storage-cells line added to §3 |
| A-3 (CancelToken misclassified CANONICAL) | H-16 moved out of CANONICAL; Root IDs corrected (revision 2 note: H-16 is now `COMPAT` — root-DEFERRED-world vocabulary, §2.4/§4.1 P1-1) |
| A-4/B-3 (header group counts wrong) | §1.3 recount: 21/1/36/17/4/2 = 81 |
| A-5 (authority lists incomplete, counts unverifiable) | §3 rebuilt as explicit ID sets; task-world cells recorded |
| A-6 (vectored-IO expressiveness gap in D-09) | L-01 role + DAG §4 record the readv/writev gap and the `write_all_vec` dependency |
| A-7/B-8 (FA-4 wrong file) | → `xmake.lua:61` |
| A-8 (RequestArena "live" overstated) | R-11 reworded production-unarmed, test-reachable |
| B-1 (missing concrete backend rows) | added H-27/H-28 |
| B-2 (`AsyncRwLock` falsely "zero consumers") | H-22 consumer split corrected; policy D-05/D-06 updated |
| B-4 (missed public→detail edges) | §1.4 rows added (request.hpp, mutex.hpp seam-header edge, event/async_rwlock, both backends) |
| B-5 (macro site list incomplete) | §1.5 `SLUICE_ASYNC_INTERNAL_TESTING` → 14 target declarations / 17 headers |
| B-6 (liburing guard description) | §1.5 reworded: fragments guarded, class shell differs per profile (the ODR fact) |
| B-7/C-11 (numeric drift) | target count 131; mutant spellings `SLUICE_*_MUTANT_*` incl. C1/C2E families; "23 of 36" file_semantics reach; consumer counts corrected (V-04, H-16, R-09, L-02) |
| B-9 (FA-3 overstated) | reworded: test content compiles out under production config |
| B-10 (missing core-world seam macro) | `SLUICE_FILE_INTERNAL_TESTING` row added |
| B-11 (sanitizer layout propagation) | §1.5 fiber-sanitizer row extended |
| C-1 (Completion consumers understated) | R-06/X-01/policy §3 updated: 25 test files name the type, 4 apps are CALL+TYPE_LAYOUT consumers |
| C-2 (Scheduler Completion-await machinery missing) | new DAG edge X-16; H-08/H-10 roles note Completion-spelled await/cancel |
| C-3 (Group→Fiber edge missing; F3 unreachable) | new DAG edge X-17; F3 tail gated on Group retirement |
| C-4 (A1 status overstated) | V-06 STATUS + DAG §4 cite A1 `IMPLEMENTED_UNVERIFIED` (reference-model halves) |
| C-5 (B2/RequestHandle adoption states) | STATUS vocabulary introduced (VERIFIED vs RECORD); R-01/R-05/R-10/R-04 + DAG §4 corrected |
| C-6 (X-08 app-include evidence wrong) | DAG X-08 fixed: `async/file.hpp` in grep/hash/tail; `blocking/file.hpp` in tail only |
| C-7 (busy-poll row falsely HISTORICAL) | H-05 role + DAG §4: `op_helpers` `one_step` spin loop is the live MIG-02 busy-poll instance, covered by D-15 |
| C-8 (app-README DOC_CLAIM edges) | new DAG edge X-18; census §2.7 app-README row annotated |
| C-9 (identity_of arena fallback) | R-11 + DAG X-02/D-02 record the `RequestHandle` derivation fallback |
| C-10 (evidence-vehicle test migration) | DAG D-09 chain gains the ledger re-record obligation |

Post-review verdicts are recorded per reviewer in the F0 final report; residual P2/P3 items are resolved in place or recorded as findings (FA-*, C-7 defect note) with owners assigned to later phases. No P0 and no unresolved P1 remains.

## 4.1 Round-1 human review (PR #455 comment 6030469392) — resolution record

Human review verdict: `F0_HUMAN_REVIEW = REQUEST_CHANGES` (P0=0, P1=2, P2=3, P3=0; all four exit gates REQUEST_CHANGES; F1 = BLOCKED). All five findings are addressed in this revision; the change set stayed within docs/policy/evidence and the production source diff remains **0**:

| Finding | Resolution |
|---|---|
| P1-1 — optional single-owner host conflated with legacy multi-worker runtime as OPT_SUPPORTED | Reclassified per root PROD-02 ("Single-owner stackful I/O adapter = Optional build profile; multi-worker task host = Deferred"): H-09 (`StackfulIoHost`/`IoTaskContext`) → `OPTIONAL_CANDIDATE` (support claim gated on W-04 pass + ADR-0003 adoption; ADR-0003 stays PROPOSED — not adopted here); H-06, H-07, H-08, H-10, H-13, H-16, H-26 → `COMPAT` (root-DEFERRED multi-worker runtime world); `Fiber`/`fiber_ctx` split by role×profile into INTERNAL mechanism rows (H-11/H-12) plus one legacy-runtime public-exposure row (H-29, COMPAT). Profile counts/matrix (§3), policy §1.1/§1.4/§2/§3 and disposition table D-16 updated. |
| P1-2 — `RequestHandle` prematurely frozen as KEEP_PUBLIC/CANONICAL | Capability and spelling separated: R-05 profile → `UNDECIDED (spelling)` — the copied-identity/state-probe capability stays MIG-02-required, while the `RequestHandle` spelling's disposition (retain \| fold into `RequestId`/context lookup \| internalize) is recorded as **UNDECIDED, owned by F2** per #402; D-17 candidate changed from KEEP_PUBLIC to UNDECIDED (F2-owned); X-02 arena fallback retained as migration dependency. CANONICAL set drops to 17; `UNDECIDED_SPELLING_SURFACES` (1) added to §3. |
| P2-1 — PR-head CI not green | Run `37565411352` (head `404af1c1`): attempt 1 — release PASS, debug FAIL, single failure `backend_conformance_e1_threadpool_test` 180s timeout (48/49). Attempt 2 (same SHA, same configuration, rerun 2026-10-07): **all jobs PASS**, debug job 1m13s. Attribution: the PR diff contains no production file, test file or build rule (`docs/` + `scripts/` only), so the built test binary is byte-identical to master's; the same binary that timed out at 180s passed on rerun. Classification: runner-execution timeout, not reproducible, not F0-introduced. No production defect is asserted either way; the E1 threadpool test's 180s ceiling on loaded runners is recorded here as an observation for the CI owner, with no code change made (F0 forbids production/test edits). |
| P2-2 — X-10 contradicted X-18 on current-doc claims | X-10 reworded (DAG §2): no **normative source/ABI compatibility promise** exists (policy §1.5/§1.6), while current **non-normative product/docs claims do exist** — README's retained-baseline framing and the app READMEs' ApplicationRuntime-lifecycle and "installed/public headers only" claims (X-18) are real DOC_CLAIM/product-surface evidence that must migrate with their surfaces. |
| P2-3 — F4 prerequisite summary mis-bound X-18 | X-18 removed from `F4_PREREQUISITES` (it is a host/Completion DOC_CLAIM edge, not a native-resource dependency); F4 = X-06, X-07, X-13 + the D-11/D-12 undecided rows; X-18 assigned to the F2/F3 documentation-migration path (DAG §6). |

## 4.2 Round-2 human review (PR #455 comment 6031614724) — resolution record

Human review verdict: `F0_HUMAN_REVIEW_ROUND_2 = REQUEST_CHANGES` (P0=0, P1=1, P2=1). Round-1 items P1-2/P2-1/P2-2/P2-3 = CLOSED; P1-1 = PARTIALLY CLOSED (semantic split correct, public closure under-classified). This revision closes the residual:

| Finding | Resolution |
|---|---|
| Residual P1 — OPTIONAL_CANDIDATE public compile/API closure under-classified | Applied the primary-key rule (same physical surface, different role × profile ⇒ separate rows) to H-09's real public closure, verified against `stackful_io_host.hpp` at the baseline: new **H-30** (`CancelToken` as H-09 public-signature dependency — `stackful_io_host.hpp:4` publicly includes `cancel.hpp`, `IoTaskContext::token() → const CancelToken&` at `:39`, private `stop_token_` member at `:221`) and new **H-31** (`Fiber`/`fiber_ctx` as H-09 public compile/layout exposure — `:6-7` includes both headers, `TaskSlot::fiber` embeds `Fiber` by value at `:181`, `fiber_ctx::Context driver_ctx_` at `:219`, sanitizer layout propagation §1.5/X-03). **H-29** rewritten to carry only the legacy-runtime exposure (its `stackful_io_host.hpp` prose moved to H-31); **H-16** scoped to the legacy-runtime vocabulary role with a cross-ref to H-30. `OPTIONAL_CANDIDATE_SURFACES` = H-09, H-30, H-31 (exposure rows inherit the H-09 gate); row total 71 → 73. Package rule recorded in policy §2: **until F3 repairs the H-09 header boundary, a clean-room OPTIONAL_CANDIDATE host package must carry every header/type required to compile that public header (`fiber.hpp`/`fiber_ctx.hpp`/`cancel.hpp` among them), even where the underlying mechanism's target disposition is KEEP_INTERNAL.** DAG X-03 and the F3 chain updated to cite H-30/H-31. ADR-0003 stays PROPOSED; no production change. |
| P2 — stale PR body metadata | PR #455 body refreshed to the current row count/profile distribution and review-round state. |
| Round-3 CI observation (same test, recurring — escalated record) | `backend_conformance_e1_threadpool_test` wall-clock stalls, occurrences: (1) head `404af1c1` run `37565411352` attempt 1, **debug**, 180s timeout — attempt 2 all PASS, whole debug job 1m13s; (2) head `bd5f4d5b` run `37577464233` attempt 1, **release**, 180.004s timeout — attempt 2 all PASS, test itself **0.016s**; (3) head `82d7d239` run `37578167444` attempt 1, **debug**, 180.005s timeout — rerun outcome recorded in the round-3 handoff. Evidence chain: every F0 head differs from master by `docs/`+`scripts/` only, so the built test binary is **input-identical in all occurrences** (including master's own build); the fail-fast stderr lines visible in failing runs also appear in passing runs (expected negative-scenario harness output); **no assertion failure was ever emitted** — the failure mode is always the invocation stalling to the observed 180s ceiling. Tally in the observation window: 4 ms-scale passes vs 3 stalls over seven invocations of the same binary; config-independent (debug ×2, release ×1); every rerun passed. Classification: **pre-existing intermittent stall of the test invocation, not F0-introduced** (F0 has zero code delta) — no longer classed as rare; recorded as a real CI-robustness finding. Configuration fact for the owner: the test is registered bare (`add_tests(name)`, `xmake/tests.lua:604`) without an explicit `run_timeout`, unlike sibling heavy tests (120000 ms), so a stall converts to a job failure at the ceiling; a diagnosis task (stack dump on hang, serial execution, or explicit timeout policy) is assigned to the CI/test owner in a later phase. F0 makes no production/test change. |

## 5. Gate statement

- `F0_A_TREE_BUILD_INSTALL` = recorded (§1)
- `F0_B_SEMANTIC_ROLE` = recorded (§2, 73 surface rows across 6 families + formal/evidence assets)
- Production deletions / authority retirements performed by F0: **0**
- Candidate dispositions and policies: see companion files; nothing here authorizes removal.
- Round-1 human review = REQUEST_CHANGES (comment 6030469392) — resolved by revision 2 (§4.1). Round-2 human review = REQUEST_CHANGES (comment 6031614724): `COMPATIBILITY_DISPOSITION_DEFINED` and `CONSUMER_MIGRATION_DAG_DEFINED` = PASS(candidate); the residual census/policy P1 (OPTIONAL_CANDIDATE public closure) is resolved by revision 3 (§4.2). Gates remain **candidate** until Round-3 human review; F1 stays BLOCKED pending that verdict.
