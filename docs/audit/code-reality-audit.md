# Code-First Architecture Reality Audit — SLUICE-CODE-FIRST-ARCHITECTURE-REALITY-AUDIT-1

- **Type**: READ-ONLY CODE AUDIT / DOCS-ONLY OUTPUT. No production code, tests, build files, or ADRs were modified by this audit.
- **PROOF_ROOT**: `7f5a3f59515911fabc3c41a3846b0bad3bef1688` (== `origin/master` at audit start; worktree clean).
- **Audit date**: 2026-09-13.
- **CODE_REALITY_FREEZE sha256**: `2c4e43a39ce0bc9dc48c934cd8a2b745bc443e3291b643d9e8876fd1abc0c5d5` (frozen before any ADR/roadmap/architecture-doc reading; embedded verbatim in §5–§16).

---

## 1. Executive verdict

The retained C++ implementation is **substantially conformant** to the frozen Explicit File architecture (ADR-0002): the canonical `File` spine, the three-axis open contract, the §5.5 access-legality matrix, the durability operation vocabulary, the two-level API discipline (no execution disguise), blocking-first-class execution, mechanism-only backends, and the explicit async correctness machinery (admission / capacity / identity / terminalization / publication / cancellation / deadline / wait-wake) are all present and behave as the ADR requires at every initiation surface I verified.

The material findings fall into five clusters:

1. **Three adjudication-pending ambiguities (AMBIGUOUS_AUTHORITY)** where code and ADR text do not collide head-on but the ADR under-specifies the regime the code implements: **DC-13** — at the explicit surface, arena-capacity/admission-closed rejection (`would_block`/`invalid_state`) preempts the frozen precedence outcomes of the zero-length and offset-validation rules, diverging from the blocking/evented surfaces in the saturated regime; **DC-29** — the evented `await_*` surface straddles ADR §6.1 (common API must not force `Completion` FSM management on ordinary callers) and §7.3 (await-style operations), and on wait-layer error leaves the caller holding an outstanding `Completion`; **DC-30** — the two in-tree read-composition families disagree on premature-EOF (error vs partial success), and nothing classifies `fill` against the ADR's exact/all vocabulary.
2. **Unbacked public surface** — `include/sluice/experimental/*.hpp` are public headers whose only TUs (`src/experimental/*.cpp`) are compiled into **no target**; consuming them cannot link (DC-22, SUS-7).
3. **Zero-consumer mechanism families** — the entire async sync-primitive family (`Event`, `AsyncMutex`, `Semaphore`, `AsyncCondition`, `AsyncRwLock`, `AsyncQueue`, `select`) plus `Batch`, `op_helpers`, `Scheduler` drain mode are scheduler-supported, public, and have **zero real consumers and zero tests** (SUS-5/6, DC-27/25). The legacy streaming world (`Reader`/`Writer`/`IoContext` family/`wal`/`buffer`/`observed`/`fault`/`BlockingIoPool`) is likewise zero-consumer; its disposition is explicitly tracked by the separate legacy-surface process (#355), so this audit records the fact and does not re-adjudicate.
4. **No formal assets exist anywhere in the tree** — no `.tla`, no model checker harness, no verification scripts. The concurrency cores that most deserve models (park/wake epoch protocol, request-slot + completion state machines, wait routing, uring cancel deferred-terminal protocol, teardown barrier) are guarded only by fail-fast asserts and hand tests (Phase D triage: 5 model candidates admitted, 4 claims rejected as lower-cost evidence suffices, 1 deferred).
5. **Verification blind spots inside live machinery** — park/wake protocol and the teardown residual-wait check have no test or assert coverage (SUS-4, SUS-12, SUS-14).

No P0 correctness defect was found. Adversarial lifetime review constructed no use-after-free interleaving in any supported topology (§33). One caller-contract risk window remains open per ADR §7 territory (not a violation): `NativeFileRef` fd borrow during outstanding operations, whose worst variant is fd **recycling** — silent corruption rather than EBADF (SUS-3, PLAN-6). The originally suspected external-wake `Control{alive}` window (SUS-15) was **re-graded safe by construction** under review (§33, D-2).

## 2. Scope, method, and proof discipline

- **Phase A (code-only)**: repository census, module table, root object cards, authority matrix, Figures A–I, state/thread/lifetime maps, suspicious facts — all derived from `include/`, `src/`, `apps/`, `tests/`, `xmake*` only. Per the campaign contamination rule, **no** `docs/adr/*`, `docs/roadmap/*`, `docs/architecture.md`, README architecture claims, architecture issues, or historical review reports were read before `CODE_REALITY_FREEZE` was written and hashed.
- **Phase B (docs)**: after the freeze, `docs/mission.md`, `docs/adr/0001-*`, `docs/adr/0002-*` (+ views companion), `docs/architecture.md` were read in full and reduced to the semantic contract table (§18).
- **Phase C**: per-contract diff with the fixed verdict vocabulary (§19), gap classes A–E (§20).
- **Phase D**: formal-method triage, model cards, FORMAL MATRIX, assets census, evidence pyramid (§21–§25).
- **Corrective plan**: PLAN cards + dependency DAG + **PROPOSED issues only** — no implementation issues were opened (§26–§28).
- **Adversarial reviews A–D** ran in fresh contexts against this report and the repository; all material findings were re-verified against code by the auditor before being incorporated (§29–§33).
- Verification commands used throughout: `git`, `rg`, direct file reads; no build was modified. A link-level check confirmed production directories are byte-identical between the architecture-snapshot baseline (`253fabe7`) and PROOF_ROOT (`7f5a3f59515911fabc3c41a3846b0bad3bef1688`) — the delta is docs/CI only (`git diff --stat 253fabe7..7f5a3f59 -- src include xmake apps tests` is empty), so `docs/architecture.md`'s code claims were checked against the same tree state.

## 3. Output location decision

The campaign default output path `docs/architecture/code-reality-audit.md` assumes a `docs/architecture/` directory; **no such directory exists** in the tree. The existing audit report directory is `docs/audit/` (contains `io-architecture-gap-1.md`). Per the "do not build new dir hierarchies" rule, this report lives at **`docs/audit/code-reality-audit.md`**.

## 4. Freeze record and corrigenda

- Frozen artifact: `/tmp/sluice-audit/code-reality-freeze.md` at audit time; embedded **verbatim and unmodified** as §5–§16 below (its internal `FZ-*` numbering is preserved so the hash relationship stays auditable).
- Freeze protocol: errors in frozen content are corrected only by corrigendum in this section; the frozen text is never edited. Corrigenda COR-2..COR-7 originate from adversarial reviews B/C/D (§31–§33) and COR-8..COR-14 from review A (§30); every load-bearing one was independently re-verified against the code by the auditor before acceptance.

- **COR-1** (census correction, code-only evidence, discovered post-freeze): the freeze's FZ-1 census line "Test binaries: 17 unconditional + 2 liburing-gated + 4 app-consumption" miscounts. Authoritative count from `xmake/tests.lua`: **16 single-file test targets + 4 app-consumption targets = 20 default-registered binaries, plus 2 liburing-gated targets**. No other FZ-1 fact changes.
- **COR-2** (ownership correction, from review D / verified): the `RequestArena` is a **member of each backend** (`detail::RequestArena arena_` in `include/sluice/async/threadpool_backend.hpp` / `uring_backend.hpp`), **not** owned by `AsyncIoContext`, whose members are only `backend_` (unique_ptr), `stats_`, `access_mtx_` (`include/sluice/async/async_io_context.hpp:267-271`). RC-3 ("MUTABLE STATE: … RequestArena"), RC-6 ("AsyncIoContext owns arena; backends borrow"), the FZ-2 AsyncIoContext row's state column, and FZ-7's mini-diagram are corrected accordingly. Destruction is safe **by construction**, independent of `close_resources` ordering: each backend's destructor joins its workers in the destructor *body* (`src/async/threadpool_backend.cpp:117-132`) before any member is destroyed, and `arena_` (first-declared member) is destroyed last — arena writers are dead before the arena dies. The "arena borrow" hazard question is void.
- **COR-3** (state-ownership correction, from review D / verified): the fallback wait maps `waiting_size_`, `waiting_void_`, `waiting_ready_` (plus `waiting_waitq_count_`) are **Scheduler** members guarded by `global_mtx_` (`include/sluice/async/scheduler.hpp:534-539`), not AsyncIoContext state. FZ-9's row and RC-3 are corrected. SUS-4's fix target is therefore `~Scheduler` (assert the maps empty beside the existing `wait_record_live_count_` check), **not** `~AsyncIoContext`; PLAN-3 is reworded. The gap itself stands as a checked-invariant gap (S3), not a UAF: reaching destruction with live entries requires a suspended fiber, which supported runtime topologies cannot produce (drain/join blocks, `~Group` fail-fasts first).
- **COR-4** (lock-scope correction, from review C / verified): in `AsyncIoContext::submit_*`, the `fd < 0` check and `access_legality()` run on the caller's thread **before** `access_mtx_` is acquired (`src/async/async_io_context.cpp:96-102`). RC-3/FZ-9's "serializes all submissions + legality checks" is corrected to: `access_mtx_` serializes backend submissions; legality is evaluated outside the lock on the by-value `NativeFileRef` copy — sound because it is a pure function of value fields immutable after op construction. No multi-writer window exists (review C's adjudication of its own finding; no model warranted — F-6 stays at tests).
- **COR-5** (park-bound correction, from review C / verified): the 2 ms park backstop applies **only** when a deadline exists or bounded backend observation is active; with `earliest == kNoDeadline && !bounded_backend_observation` the Scheduler-domain park is **unbounded** (`src/async/scheduler_park_wake.cpp:258-296`). Figure F's "bound: kParkBackstop = 2ms" is corrected to "bound: 2 ms only with deadline or bounded backend observation; otherwise unbounded park". The 1 ms figure is a test-clock mode (`kTestParkPoll`) reachable only through `AsyncTestAccess` under `SLUICE_ASYNC_INTERNAL_TESTING`, which nothing currently compiles. MC-1 is scoped accordingly (§22) or it would be vacuous.
- **COR-6** (join-site correction, from review D / verified): `~Scheduler` joins no threads. Worker threads are joined inside `run_impl` when each `run_live` returns (`src/async/scheduler.cpp:294-297`); the driver thread is joined in `ApplicationRuntime::join()`/destructor **before** `close_resources()` (`src/async/application_runtime.cpp:129-131, 347-355`). FZ-11's step-2 mechanism is corrected; the group→sched→io_ctx barrier holds via those joins plus backend-destructor worker joins (COR-2).
- **COR-7** (Group-gate correction, from review D / verified): RC-9/FZ-11 presented "clears only when all futures ready" as the Group safety gate. The operative guarantee is the **run-join**: `Group::await`/`~Group` clear only after `run_live` returned, which joins all workers, hence after all fibers departed their stacks. `Future::complete_with` publishes readiness (and calls the wake) from inside the fiber entry *before* `make_done()` and the final context switch (`include/sluice/async/group.hpp:150-155`, `src/async/scheduler.cpp:25-34`), so future-ready ≠ stack-departed; a clearer observing readiness in that window would free a live stack. The interleaving is not constructible in supported topologies (run-join closes it) but would become live for a Group awaited/destroyed from a thread concurrent with a fiber-hosting `run_live` — a configuration the scheduler already cannot support (unsynchronized per-invocation fields; `src/async/scheduler.cpp:202-207, 579-580`). Recorded as latent, documented for MC-4.
- **COR-8** (census count corrections, from review A / auditor re-verified): FZ-1/FZ-2/SUS-16 miscount in four places. Authoritative: `sluice_core` compiles **13** top-level TUs (not 15; the 15 figure erroneously included the 2 uncompiled experimental TUs); `sluice_async` compiles **31** TUs (not 26), including the 3 test-seam TUs; the `src/async/scheduler*.cpp` family is **10 files = 9 production + 1 test seam** (not 7); the fail-fast catalog defines **37** functions (not 31). Re-verified: `git ls-files 'src/*.cpp'` = 46 = 13 + 31 + 2; `rg -c '^\[\[noreturn\]\] void' src/async/fail_fast.cpp` = 37.
- **COR-9** (ApplicationRuntime destruction path, from review A / verified): RC-5/FZ-11 claimed the destructor invokes `close_resources()`. It does not: `~ApplicationRuntime` (`src/async/application_runtime.cpp:119-132`) fail-fasts via `group_lifetime_fail_fast()` unless state ∈ {Constructed, StartFailed, Stopped}, then joins `driver_thread_`. `close_resources()` (lines 546-570; order group→sched→io_ctx) is invoked only from `join()` (355), `shutdown()` (380, via the driver path), and `start()` failure paths (144/169/193). The teardown **order** as frozen is correct; the **trigger** is corrected.
- **COR-10** (interop entry points, from review A / verified): RC-1/RC-2/Figure B mischaracterized two entry points. (a) `File(int fd, FileAccess)` is **private** with no friends (`include/sluice/file_resource.hpp:51-52`) — an internal implementation detail of `File::open`, not a reachable interop entry. (b) `NativeFileRef` values are **created by callers at op construction** (implicit `File` conversion at `ReadOp`/`WriteOp` build sites, e.g. `src/async/file.cpp:35`, `apps/sluice-copy/copy_task.cpp:65/76`); `AsyncIoContext` only *consumes* `op.file` at the legality gate. The public raw-fd interop boundary is `NativeFileRef(int, FileAccess)` alone, as DC-14 already stated.
- **COR-11** (Group execution paths, from review A / verified): FZ-10's "Group threaded path … own thread run_live(1)" conflated the two paths. The threaded path (`sched_ == nullptr`) spawns one `std::thread` per `async()` executing the caller's function directly — no scheduler, no `run_live` (`include/sluice/async/group.hpp:85-111`). `run_live(1, &group_stop_predicate, …)` is the **evented** path, run on the awaiting caller's thread from `Group::await()` (`src/async/group.cpp:39`). The zero-consumer status of the threaded path stands (only ApplicationRuntime constructs Group, always evented).
- **COR-12** (consumer-column corrections, from review A / verified): five FZ-2/RC rows carried wrong consumer sets. (a) `blocking::file`: consumers = sluice-tail (app) + **7 test binaries** (`blocking_file_{read,write,state,sync_data,sync_all,sequential}_test` + `file_access_precedence_test`), not "sluice-tail only". (b) Scheduler, Group, and wait/cancel plumbing rows falsely listed "tests": no test includes `sluice/async/scheduler.hpp` or `sluice/async/group.hpp`, and no test uses `CancelToken`/`RequestHandle`/`WaitPolicy` (sweeps empty) — tests reach this machinery only indirectly via `ApplicationRuntime`/`run_task_to_result`. (c) `AsyncIoContext` consumers = **all four apps** via runtime (tail included). (d) `ThreadPoolBackend` has **no default-backend status**: the `AsyncIoContext` ctor requires a backend argument and `RuntimeBuilder::build()` rejects a missing one; all four apps construct `ThreadPoolBackend` directly. (e) `detail/io_validation` is also consumed by legacy `src/file.cpp` (`checked_posix_offset` at lines 193/243/439/492), which is compiled into `sluice_core`.
- **COR-13** (header-only types, from review A / verified): FZ-2's implementation column listed non-existent TUs: `Completion<T>` lives in `include/sluice/async/completion.hpp` and is header-only (no `src/async/completion.*` exists; no TU defines its members); `Future`/`TaskResult` are likewise header-only with no TU.
- **COR-14** (SUS-14 wording, from review A / verified): the timer-heap/`earliest_active_deadline_` agreement has **no dynamic invariant check at all** — not "(asserts only)": `src/async/scheduler_timer.cpp` contains only `static_assert`s on type traits (202/205) and an unrelated E12 assert (157); the atomic store is at line 326. The substance of SUS-14 (unchecked invariant, S3) stands with corrected wording.

---
# CODE_REALITY_FREEZE — SLUICE-CODE-FIRST-ARCHITECTURE-REALITY-AUDIT-1

- PROOF_ROOT: `7f5a3f59515911fabc3c41a3846b0bad3bef1688` (== `origin/master` at audit start; worktree clean)
- Frozen at: 2026-09-13, before any reading of `docs/adr/*`, `docs/roadmap/*`, `docs/architecture.md`, README architecture claims, architecture issues, or historical review reports.
- Every claim below has a CODE BASIS (file path + symbol). No ADR, issue, PR, or deleted-document input was used.
- Diagram legend (fixed for all figures): `CALL` = synchronous invocation; `OWNS` = lifetime/ownership; `BORROWS` = temporary reference without ownership; `SUBMITS` = hands an operation to an executor; `PUBLISHES` = makes a result visible (exactly-once terminal transition); `WAKES` = ends a suspension; `CANCELS` = requests abandonment of an operation; `LOWERS` = translates an abstract operation into OS primitives (syscall / io_uring SQE).

---

## FZ-1. Repository census

| Item | Count / value |
|---|---|
| Tracked files | 231 |
| C++ LOC (src/ + include/ + apps/ + tests/) | ≈ 33,146 |
| Formal assets (*.tla, *.cfg, verify-*.sh, formal/, models/) | **NONE anywhere in tree** |
| Production TUs (compiled) | src top-level 15 in `sluice_core`; src/async 26 in `sluice_async` |
| Production TUs present but compiled into NO target | `src/experimental/uring_io_context.cpp`, `src/experimental/uring_write_batch.cpp` (2) |
| Public headers | `include/sluice/*.hpp` (core), `include/sluice/async/*.hpp`, `include/sluice/async/detail/*.hpp`, `include/sluice/blocking/file.hpp`, `include/sluice/experimental/*.hpp` (2, public but unbacked by any compiled TU) |
| Apps | 4 (`sluice-copy`, `sluice-hash`, `sluice-grep`, `sluice-tail`), each a multi-file target |
| Test binaries | 17 unconditional + 2 liburing-gated (`uring_public_consumer_probe`, `uring_backend_smoke_test`) + 4 app-consumption (compile app engine sources into test binaries) |
| Test framework | none — hand-rolled check macros + `main()` per binary |
| Build system | xmake; root `xmake.lua` + `xmake/{libraries,apps,tests,helpers}.lua` |
| Test-seam TUs | `src/async/{mutex_test_seam,queue_test_seam,scheduler_fe2_test_seam}.cpp` — compiled into `sluice_async` but entirely `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)` guarded |
| Untracked-but-present | `src/async/fdg0_unowned/` (empty directory, not in git) |

CODE BASIS: `git ls-files | wc -l`; `find`/`rg --files`; `xmake/libraries.lua` (`add_files(R.."src/*.cpp")` single-level glob for `sluice_core`, `src/async/*.cpp` for `sluice_async`); `xmake/tests.lua`; `xmake/apps.lua`.

---

## FZ-2. Module table

| Module | Public surface | Implementation | Direct dependencies | Real consumers (apps+tests) | State owned | Execution owned |
|---|---|---|---|---|---|---|
| `file_resource` | `File`, `FileOpen`, `FileAccess` | `src/file_resource.cpp` | POSIX open/close, `error`, `result` | all 4 apps, blocking layer, async await layer, `AsyncIoContext` | fd + access mode pair | none (caller's thread) |
| `blocking::file` | free fns `read_at/write_at/read/write/sync_data/sync_all/size/resize` | `src/blocking_file.cpp` | `File` | sluice-tail only | none | caller's thread (blocking syscalls) |
| legacy `file.hpp` | `FileReader`, `FileWriter` | `src/file.cpp` | `error`, `measurement` stats pointers | **zero** (only `io_context.cpp` constructs via factory) | fd + stats | caller's thread |
| `io_context` (factory family) | `IoContext`, `BlockingIoContext`, `OpenReader/WriterOptions` | `src/io_context.cpp` | legacy Reader/Writer | **zero** | per-impl | `BlockingIoContext` none |
| `memory_io_context` | `MemoryIoContext` | header-only | `io_context`, `fault` | **zero** | byte-store map | none |
| streaming Reader/Writer | `Reader`, `Writer`, `copy`, `stream_to`, `copy_strategy`, `limit` | `src/reader.cpp`, `src/writer.cpp`, `src/copy.cpp`, `src/copy_strategy.cpp` | legacy file, io_context | **zero** | per-object | caller's thread |
| `wal` | `Wal`... | `src/wal.cpp` | legacy Writer | **zero** | log state | caller's thread |
| `buffer` | `Buffer`, `BufferedReadable` | `src/buffer.cpp` | Reader | **zero** | buffer | none |
| `observed` | observed wrappers | `src/observed.cpp` | io abstractions | **zero** | observers | none |
| `fault` | fault-injection hooks | `src/fault.cpp` | — | header-chained only (`memory_io_context.hpp` includes it) | none | none |
| `blocking_io_pool` | `BlockingIoPool`, `Task<T>` | `src/blocking_io_pool.cpp` | thread + bounded queue | **zero** | worker threads, queue | own worker threads |
| `detail/io_validation` | `checked_posix_offset`, `uring_chunk_length`, `retry_uring_wait_on_eintr` | header | `<limits>`, POSIX types | uring backend, blocking layer | none | none |
| `AsyncIoContext` | `AsyncIoContext`, `NativeFileRef`, op structs, `submit_read/write/sync_data/sync_all`, `wait_one`, `outstanding`, stats | `src/async/async_io_context.cpp` | backends, `RequestArena`, `File` | apps (hash, copy, grep via runtime), many tests | `access_mtx_` serialization, arena, backend refs, fallback wait maps | none (delegates) |
| `ThreadPoolBackend` | class (public header) | `src/async/threadpool_backend.cpp` | `RequestArena`, POSIX pread/pwrite/fsync | AsyncIoContext (default backend), tests | worker threads, bounded dispatch queue, running set | own worker threads |
| `UringAsyncBackend` | class (public header, `#if SLUICE_HAS_LIBURING` guarded core) | `src/async/uring_backend.cpp` | `RequestArena`, liburing, eventfd, poll(2) | AsyncIoContext (opt-in), 2 gated tests | ring, router table, transport ledger, control state | caller submits; kernel completes |
| `Scheduler` | `Scheduler`, fiber spawn/run API | `src/async/scheduler*.cpp` (7 TUs) | `fiber_ctx`, wait queues, timers | ApplicationRuntime, Group, tests | global mutex + wait registry + wake epochs + timer heap + run queues | M:N worker threads (work stealing) |
| `ApplicationRuntime` | `ApplicationRuntime`, task submit, `run_task_to_result` | `src/async/application_runtime.cpp` | Scheduler, Group, AsyncIoContext | hash, copy, grep, tail apps; tests | State/CloseState/DriverState, epochs, counters | driver thread(s) |
| await ops layer | `await_read_at`, `await_write_at`, `await_sync_data/all` | `src/async/file.cpp`, `await_op_helpers.cpp` | File, AsyncIoContext, Completion | all async apps | none (checks + delegates) | scheduler fiber |
| `op_helpers` | poll-loop read/write_all etc. | `src/async/op_helpers.cpp` | AsyncIoContext | **zero** | none | caller fiber |
| `Batch` | `Batch`, `BatchOp`, `BatchResult` | `src/async/batch.cpp` | AsyncIoContext, Completion | **zero** (only experimental headers) | slots | none |
| sync primitives | `Event`, `AsyncMutex`/`Mutex`, `Semaphore`, `AsyncCondition`, `AsyncRwLock`, `lock_guard`, `AsyncQueue`, `select` | `src/async/scheduler_{event,mutex,semaphore,condition,rwlock,queue}.cpp`, `select*.cpp`, `queue_port.cpp` | Scheduler wait queues + timers | **zero** (name-collision false positive only: `MatchEvent` in grep app) | per-primitive wait queues | scheduler |
| `Group` | fiber group join/teardown | `src/async/group.cpp` | Scheduler | ApplicationRuntime, tests | futures/fibers/stacks lists | evented: scheduler; threaded: own thread |
| `Completion` | `Completion<T>`, reap_seq | `src/async/completion.hpp` (+ TU parts) | `fail_fast` | await layer, Batch, apps | caller-owned slot + state machine | none |
| `RequestArena` | detail header | `src/async/detail/request_arena.hpp` | `request_slot`, `request_key`, `ready_sink` | AsyncIoContext + both backends | slot array, generations, ready ring | none |
| `wait/cancel plumbing` | `RequestHandle`, `CancelToken`, `WaitPolicy` | `src/async/{request_handle,cancel,wait_policy}.cpp` | AsyncIoContext, Scheduler | apps (grep cancel), tests | handle → slot binding | none |
| `Future/TaskResult` | `Future<T>`, `TaskResult` | headers + TUs | Group/Scheduler | hash/copy apps | result slot | none |
| `fiber_ctx` | context switch API | `src/async/fiber_ctx.cpp` (x86-64 asm) | — | Scheduler, Group | per-fiber stack+ctx | none (primitive) |
| `fail_fast` | terminate catalog (31 functions) | `src/async/fail_fast.cpp` | — | all async modules | none | none |
| experimental uring | 2 public headers | 2 TUs **compiled nowhere** | uring backend internals | **zero** | — | — |

CODE BASIS: per-file reads listed in the audit log; consumer sets cross-checked by direct `#include <sluice...>` census of `apps/` and `tests/` plus `rg -l` symbol sweeps (false positives manually excluded, e.g. `MatchEvent`).

---

## FZ-3. Root object cards

### RC-1 `File` (canonical file resource)
- CREATED BY: `File::open(path, FileOpen)` (`include/sluice/file_resource.hpp`, `src/file_resource.cpp`); interop ctor `File(int fd, FileAccess)` is the explicit raw-fd entry point.
- OWNED BY: application / owning scope (move-only).
- DESTROYED BY: destructor or `close()` — POSIX `close(fd)`; idempotent via fd sentinel (-1).
- MUTABLE STATE: fd, access mode (immutable after open).
- BORROWERS: `blocking::` free fns (`const File&`), await ops layer (`const File&`), `AsyncIoContext` (via `NativeFileRef`).
- LIFETIME BOUNDARY: borrower must not outlive the File; `NativeFileRef` copies {fd, access} by value at submission.
- CROSS-THREAD ACCESS: fd is a plain int; File itself is not synchronized — read-only sharing assumed.
- AUTHORITY: **A** (owns file identity + access contract).
- MECHANISM ONLY: POSIX open/close.

### RC-2 `NativeFileRef`
- CREATED BY: `AsyncIoContext` submission entry (from `File` or explicit {fd, access}).
- STATE: {int fd, FileAccess access} value pair.
- AUTHORITY: **I** (carries the access contract alongside fd) + **M** (raw handle).
- ROLE: mechanism-level reference; the interop boundary where access legality is evaluated (`access_legality()` in `src/async/async_io_context.cpp`).

### RC-3 `AsyncIoContext`
- CREATED BY: application (constructor taking backend + scheduler handles).
- OWNED BY: `ApplicationRuntime` in apps; standalone in tests.
- DESTROYED BY: destructor — fail-fast `async_context_outstanding_fail_fast()` if `outstanding() != 0`.
- MUTABLE STATE: `access_mtx_` (serializes all submissions + legality checks), `RequestArena`, backend reference, fallback wait maps (`waiting_size_`, `waiting_void_`, `waiting_ready_`), stats.
- BORROWERS: await ops layer, backends (arena), scheduler (wait sources).
- LIFETIME BOUNDARY: all Completions and RequestHandles must be terminal/destroyed before destruction.
- CROSS-THREAD ACCESS: submissions from any fiber under `access_mtx_`; completions land from backend threads into the arena.
- AUTHORITY: **A** for I/O admission + access legality + backend serialization (the "ADR-0002 §5.5" gate is cited in code comments here).
- MECHANISM: none — pure coordination.

### RC-4 `Scheduler`
- CREATED BY: `ApplicationRuntime` (apps) or tests.
- DESTROYED BY: destructor — asserts `wait_record_live_count_ == 0`.
- MUTABLE STATE: `global_mtx_` (single global mutex — the one-lock design), run queues (local + steal + pending_spawn_), wait registry, `wake_mtx_` + `wake_epoch_`/`observed_epoch_` + idle dance counters, timer min-heap + `earliest_active_deadline_`, fallback wait maps, counters.
- BORROWERS: every async primitive (Event/Mutex/… call `Scheduler` methods), `Group`, `ApplicationRuntime`.
- LIFETIME BOUNDARY: no live fibers/waiters/timers may outlive it (assert partially covers this — see S3).
- CROSS-THREAD ACCESS: worker threads under `global_mtx_`; external producers via `SchedulerWakeHandle::Control{alive, scheduler}` shared state.
- AUTHORITY: **A** for fiber/wait/timer liveness and wait routing; **M** for context switching.
- MECHANISM: x86-64 stack switching (`fiber_ctx`), work stealing, park/wake.

### RC-5 `ApplicationRuntime`
- CREATED BY / OWNED BY: application `main()`.
- DESTROYED BY: destructor → `close_resources()` in fixed order **group → sched → io_ctx** (teardown barrier).
- MUTABLE STATE: `State` machine, `CloseState`, `DriverState`, `control_epoch` (observed-epoch driver loop), admitted/terminal task counts, driver thread handle.
- BORROWERS: application submits tasks; await ops reach its io_ctx.
- LIFETIME BOUNDARY: tasks must reach terminal before close completes; teardown barrier blocks until drained.
- CROSS-THREAD ACCESS: driver thread + worker threads + external `request_stop`.
- AUTHORITY: **A** for task-set admission/terminal accounting, lifecycle transitions, teardown ordering.

### RC-6 `RequestArena` (detail)
- CREATED BY: `AsyncIoContext` (owns arena; backends borrow).
- DESTROYED BY: with its owner — fail-fast if any slot not `free`.
- MUTABLE STATE: slot array, per-slot generation counters (ABA defense), state machine `free→reserved→prepared→pending→enqueued→running→backend_ready→completion_ready→free`, ready ring, `enqueue_in_flight_pin`.
- AUTHORITY: **A** for request identity (slot+generation); **M** for ready-ring publication.

### RC-7 `Completion<T>` / `Completion<void>`
- CREATED BY: caller (stack or member); passed by reference into submit.
- DESTROYED BY: caller — **only after terminal** (binding/outstanding destruction → fail-fast).
- MUTABLE STATE: state machine `idle→binding→outstanding→publishing→ready→resetting`; result storage; `reap_seq` ordering.
- AUTHORITY: **A** for exactly-once terminal publication into caller-owned storage; publication ordering via `reap_seq`.

### RC-8 backends (`ThreadPoolBackend`, `UringAsyncBackend`)
- CREATED BY: application/runtime, passed to `AsyncIoContext`.
- DESTROYED BY: destructor — fail-fast on non-quiescent destruction (`threadpool_non_quiescent_destruction_fail_fast` / `uring_non_quiescent_destruction_fail_fast`).
- MUTABLE STATE: threadpool — worker threads, bounded dispatch queue (overflow = terminate), running set; uring — ring fds, router table (cookie → slot), transport ledger, control SQE state, poison/recovery state.
- AUTHORITY: **M only** — `submit_*` are private end-to-end; access legality is enforced before backends become reachable; they lower ops to syscalls/SQEs and publish terminals through the arena.
- MECHANISM: pread/pwrite/fdatasync/fsync on worker threads; io_uring SQE/CQE with user_data cookies, `IORING_FSYNC_DATASYNC`, cancel via `prep_cancel64` + control cookie (CONTROL_TAG bit 63), eventfd + poll(2) bounded wait, poison-and-recover on transport failure.

### RC-9 `Group`
- CREATED BY: `ApplicationRuntime` (owns) / tests.
- DESTROYED BY: teardown — clears futures/fibers/stacks **only when all ready**; otherwise fail-fast (`group_lifetime_fail_fast`).
- AUTHORITY: **A** for fiber-set join semantics + stack reclamation ordering.

### RC-10 `SchedulerWakeHandle` / `Control`
- STATE: shared `Control{alive, scheduler}` + `wake_epoch_`.
- AUTHORITY: **A** for external wake admission (alive flag gates producer validity).

CODE BASIS: card fields extracted from the cited headers/sources, e.g. `include/sluice/file_resource.hpp`, `include/sluice/async/async_io_context.hpp`, `src/async/application_runtime.cpp` (close_resources order), `src/async/scheduler.cpp:104` (dtor assert), `src/async/detail/request_arena.hpp`, `src/async/uring_backend.cpp` (fail-fast + cancel cookies).

---

## FZ-4. Authority matrix (I = information, A = authority, M = mechanism)

| Concern | File | blocking:: | await ops layer | AsyncIoContext | Scheduler | ApplicationRuntime | RequestArena | Completion | Backends |
|---|---|---|---|---|---|---|---|---|---|
| File identity (fd) | **A** | M | — | I | — | — | — | — | M |
| Access contract | **A** (owns) | A (enforces) | A (enforces, 2nd time) | **A** (final gate) | — | — | — | — | — (trusts gate) |
| Blocking semantics | — | **A** | — | — | — | — | — | — | M |
| I/O admission (submit legality) | — | — | I | **A** | — | — | M | — | — |
| Request identity | — | — | — | I | — | — | **A** | — | I |
| Terminal publication (exactly-once) | — | — | I | — | — | — | M (ring) | **A** | M (records) |
| Wait/wake liveness | — | — | — | I (fallback maps) | **A** | — | — | — | — |
| Timer/deadline authority | — | — | — | — | **A** | — | — | — | — |
| Fiber lifecycle | — | — | — | — | **A** (with Group) | I | — | — | — |
| Task-set admission/terminal accounting | — | — | — | — | — | **A** | — | — | — |
| Teardown ordering | — | — | — | — | M | **A** | — | — | M (quiesce) |
| Syscall lowering | — | M | — | — | — | — | — | — | **M** |
| Context switch lowering | — | — | — | — | **M** (fiber_ctx) | — | — | — | — |

Notable authority facts (pre-ADR, code-only):
1. Access legality is enforced **twice** on the async path: await layer (`src/async/file.cpp`) then `AsyncIoContext::access_legality` under `access_mtx_`.
2. `sync_data`/`sync_all` are **not** access-gated at either layer (only `is_open` checked on await layer; no legality check in `access_legality` path for syncs).
3. Backends are reachable **only** through `AsyncIoContext` (`submit_*` private) — no path bypasses the legality gate.
4. Terminal publication authority is split: Completion owns the state machine; RequestArena owns reap ordering; backends only record.

---

## FZ-5. Figure A — whole system (code reality)

```
                        +------------------------------+
                        |        Application           |
                        |  (apps/: copy hash grep tail)|
                        +------+----------------+------+
                    CALL       |                 | CALL
                               v                 v
                  +------------+-----+   +-------+----------+
                  | ApplicationRuntime|   | blocking::file  |
                  | State/Close/Driver|--| (free fns)       |
                  | control_epoch     | | +----------------+
                  +---+----------+---+   |      | LOWERS
              OWNS   |          | OWNS  |      v
                     v          v       |  pread/pwrite/fsync
              +----------+  +-----------+   (caller thread)
              | Scheduler|  |AsyncIoCtx |
              | (M:N)    |  | access_mtx|
              +----+-----+  +-----+-----+
                   | WAKES/CANCELS     | SUBMITS (private submit_*)
                   |                   v
                   |         +---------+------------------+
                   |         | RequestArena (identity/gen)|
                   |         +---------+------------------+
                   |                   |
                   |         +---------v---------+
                   |         | Backend (M only)  |
                   |         | ThreadPool / Uring|
                   |         +---+-----------+---+
                   |             |LOWERS      |PUBLISHES
                   v             v            v
        +----------+--+   syscalls/SQEs   Completion<T>
        |Fibers/WaitQ |                    (caller-owned,
        |Timer heap   |                     exactly-once)
        +-------------+
```

CODE BASIS: aggregate of module table + root cards; every edge appears as a concrete call/include in the cited files.

---

## FZ-6. Subsystem figures B–I

### Figure B — File / I/O surface
```
File::open ──OWNS──> {fd, FileAccess}
   │ CALL                       ┌────────────────────────────┐
   ├──> blocking::read_at/... ──► POSIX syscalls (LOWERS)     │ caller thread
   ├──> await_read_at/...  ─────┐                            │
   │      (checks File.access)  │ CALL                       │
   └──> File(int, FileAccess) ──┴─> AsyncIoContext.submit_*  │
                                       │ builds NativeFileRef{fd,access}
                                       │ access_legality() gate
                                       v
                                  Backend.submit_* ──LOWERS──> pread / io_uring SQE
legacy branch (zero consumers):
IoContext ──CALL──> FileReader/FileWriter ──LOWERS──> syscalls
```
CODE BASIS: `src/file_resource.cpp`, `src/blocking_file.cpp`, `src/async/file.cpp`, `src/async/async_io_context.cpp` (access_legality), `src/io_context.cpp`.

### Figure C — async request lifecycle (happy path)
```
caller fiber                     AsyncIoContext              Arena/Backend
    │ submit_read(op, completion)     │                          │
    ├─────────CALL───────────────────►|                          │
    │                                 │ access_legality (gate)   │
    │                                 │ arena.reserve ──► slot: free→reserved→prepared→pending
    │                                 │ backend.submit_* ──► enqueued→running
    │ fiber suspends (Scheduler)      │                          │
    │                                 │◄─────PUBLISHES──────────┤ syscall/CQE done
    │                                 │ slot: backend_ready→completion_ready
    │                                 │ ready ring + WaiterToken routing
    │◄─────────WAKES──────────────────┤                          │
    │ completion: outstanding→publishing→ready (exactly-once, reap_seq order)
    │ result(); completion reset →ready→resetting→idle; slot →free
```
CODE BASIS: `src/async/detail/request_arena.hpp` (state machine), `src/async/completion.hpp`, `src/async/async_io_context.cpp` (submit/wait_one), `src/async/detail/ready_sink.hpp`.

### Figure D — task/fiber lifecycle
```
ApplicationRuntime.submit(f)
  └─ OWNS task wrapper (set_current_fiber_tag, terminal_count_++ epilogue)
      └─ Scheduler: Fiber created→runnable
           ├─ pending_spawn_ ─► owner inbox (route_runnable_locked)
           ├─ runnable→running (worker pop / steal)
           ├─ running→waiting (commit_suspend_locked + context_switch)
           │      ▲                                     │
           │      └──────────WAKES──────────────────────┘
           └─ running→done: make_done(); context_switch_final(fiber→sched)
                (never returns to fiber stack; retire epilogue re-pushes local_runnable)
```
CODE BASIS: `src/async/scheduler.cpp` (fiber_entry_bridge, classify, route, try_steal, retire epilogue), `src/async/fiber_ctx.cpp`, `src/async/application_runtime.cpp`.

### Figure E — cancellation paths
```
CancelToken/RequestHandle.cancel
  ├─ threadpool: backend.cancel → resolve_completion scan → remove_exact + arena.cancel
  │     terminal_won? ──► signal (ready progress)
  └─ uring: cancel_handle_
        ├─ terminal_won → publish
        └─ intent_recorded → issue_running_cancel: prep_cancel64 + CONTROL cookie (bit63)
              control CQE → deferred terminal if control outstanding
Scheduler-side: cancel_waiter → lease-based WaitRecord free + fiber cancel
Distinction enforced in code: waiter cancel ≠ request cancel ≠ physical cancel
```
CODE BASIS: `src/async/cancel.cpp`, `src/async/threadpool_backend.cpp` (cancel), `src/async/uring_backend.cpp` (cancel_handle_, CONTROL_TAG), `src/async/scheduler_park_wake.cpp` (cancel_waiter).

### Figure F — park / wake protocol
```
worker (no work):
  classify → quiescent ─► idle dance (dance_epoch_++)
  park_on_wake_source:
    refuse if: unguarded_progress_pending │ idle mismatch │ dance epoch mismatch
    commit: observed_epoch_ = wake_epoch_ (under wake_mtx_)
    park_pred: epoch changed ∨ terminate ∨ inbox non-empty
    bound: kParkBackstop = 2ms (test clock: 1ms; earliest-deadline bound)
  external producer: SchedulerWakeHandle.notify ─► Control.alive check ─► signal_wake_locked
    (wake_epoch_++; interrupt backend waiters if backend_wait_active_)
backend split-wait: BackendWaitToken progress/control generations
```
CODE BASIS: `src/async/scheduler_park_wake.cpp` (park_on_wake_source, signal_wake_locked, kParkBackstop), `include/sluice/async/async_io_context.hpp` (BackendWaitToken).

### Figure G — timer / deadline machinery
```
await_*_deadline ─► prepare_ordinary_deadline_locked (TimerRegistration: active)
    ─► publish (heap insert; earliest_active_deadline_ atomic recompute)
expiry: pump_deadlines_locked (heap pop → consume claim CAS → expire_wait)
    └─ special path: rwlock_timer_expire_reconcile; queue timer on_resolve
       (queue_port association decrement)
retire: retire_timer_for_node_locked (linear scan) ─► active→retired
consume: active→consumed (exactly-once claim)
clock: monotonic_now = steady_clock ms, or test clock injection
```
CODE BASIS: `src/async/scheduler_timer.cpp`, `include/sluice/async/timer_registration.hpp`.

### Figure H — backend boundary
```
AsyncIoContext (only entry; submit_* private on AsyncBackend)
   │ SUBMITS
   v
AsyncBackend (abstract, mechanism-only)
   ├─ ThreadPoolBackend: BoundedDispatchQueue (overflow⇒terminate)
   │     workers: pop→mark_running→pread/pwrite/fdatasync/fsync→record_terminal→signal
   └─ UringAsyncBackend:
         dispatch_one_locked: get_sqe (transport flush retry) → prep (fsync uses
           IORING_FSYNC_DATASYNC for sync_data) → cookie=user_data → router entry
           → transport ledger (physical position) → mark_running → remove_exact (fail-fast)
         reap: peek_batch 32 → handle_one_cqe (control cookie vs op cookie)
         bounded wait: UringWaitSource = eventfd(control fd) + poll(2)(ring fd)
         failure: poison_and_recover_locked (Class-A recovery)
   both: PUBLISH via RequestArena; destruction fail-fast unless quiescent
```
CODE BASIS: `src/async/threadpool_backend.cpp`, `src/async/uring_backend.cpp`, `include/sluice/async/async_io_context.hpp` (private submit_*).

### Figure I — build / feature topology
```
xmake root ── modes: debug/release/valgrind; sanitizers asan/tsan/ubsan/asanubsan
options: hardened; liburing (default FALSE)
targets:
  sluice_core  = src/*.cpp           (single-level glob ⇒ no async, no experimental)
  sluice_async = src/async/*.cpp     (opt-in target, default off)
  apps (4)     = core + async, public headers only
  tests (17+2 gated+4 app-consumption)
liburing ON  ⇒ add_defines(SLUICE_HAS_LIBURING, public) + add_links(uring, public)
                (public usage requirement — ODR discipline; Clang TSA flags scoped)
liburing OFF ⇒ UringAsyncBackend compiles as honest stub
NOT COMPILED ANYWHERE: src/experimental/*.cpp (2 TUs) — yet public headers exist
```
CODE BASIS: `xmake.lua`, `xmake/libraries.lua`, `xmake/apps.lua`, `xmake/tests.lua`, `xmake/helpers.lua`.

---

## FZ-7. Module mini-diagrams (selection of load-bearing ones)

**AsyncIoContext internals**
```
submit_*(op, completion) ──access_mtx_──► access_legality() ─► arena.reserve()
  ─► backend.submit_*(slot, op) ─► return
wait_one(): UringWaitSource/BackendWaitSource split-wait loop
  (pin control_baseline; progress generation advance)
outstanding(): arena live count
fallback waits (backends w/o register_waiter): waiting_size_/void_/ready_ maps
~AsyncIoContext: outstanding()==0 else fail-fast
```

**Scheduler core loop**
```
worker_loop: pop local ─► else steal (skip suspend_switch_pending; fiber_owner_==victim)
  ─► else classify_locked_impl (mw_s1 runnable/running, mw_s2 outstanding, mw_s3 waits,
        quiescent) ─► mw_s2 admission: election lowest worker id
  ─► Backend park (ctx_.wait_one) / Scheduler park / idle dance
run_live(stop_predicate): drives until predicate or no live work
route_runnable_locked: resets global_terminate_, owner-inbox routing, admission reset
drain_routed_completion_waits_locked: applies routed ReadyEvents
ReadyRoutingSink::on_ready: token identity/generation/state validation → delivered list
```

**ApplicationRuntime driver**
```
for(;;){ sched_->run_live(worker_count_, stop_predicate_trampoline, this);
        ── stop_predicate = fatal │ driver_exit │ (admission_closed ∧ task_set_terminal)
        drain check: task_set_terminal ∧ io_ctx_->outstanding()==0 ∧ (Stopping│Draining) }
close_resources(): group ─► sched ─► io_ctx   (teardown barrier)
submit(): wrapper sets fiber tag; terminal_count_++ epilogue
```

CODE BASIS: `src/async/scheduler.cpp`, `src/async/application_runtime.cpp`, `src/async/async_io_context.cpp`.

---

## FZ-8. Data flow vs control flow

**Data flow** (bytes): File fd → (blocking: syscalls on caller thread | async: backend syscall/SQE writes into caller-provided buffer referenced by op) → Completion carries Result (size_t or void or IoError) → await layer returns to fiber. Apps then hash/copy/match/print. No library-internal buffering on the async path (buffer.cpp streaming layer is zero-consumer).

**Control flow**: application → ApplicationRuntime.submit → Scheduler spawn → fiber runs task → await op suspends fiber (Scheduler commit_suspend + context_switch) → backend completes → RequestArena ready ring → ReadyRoutingSink → Scheduler WAKES owner worker → fiber resumes → terminal accounting epilogue → driver stop predicate eventually true → close barrier.

CODE BASIS: same as Figures C/D; apps sources for the byte consumers.

---

## FZ-9. State ownership table

| State | Owner | Guarded by | Cross-thread? |
|---|---|---|---|
| fd + access | File | — (immutable post-open) | read-only share |
| access legality decision | AsyncIoContext | `access_mtx_` | yes (any fiber) |
| request slot state + generation | RequestArena | arena mutex | yes (backend threads publish) |
| ready ring | RequestArena | arena mutex | yes |
| completion state machine | Completion (caller-owned) | arena/state machine + fail-fast | backend records; fiber reads |
| scheduler run queues / wait registry / timers | Scheduler | `global_mtx_` (+ `wait_registry_mtx_`, `wake_mtx_` for epochs) | yes (workers + producers) |
| wake/observed epochs, idle dance | Scheduler | `wake_mtx_` | yes |
| earliest_active_deadline_ | Scheduler | atomic | yes |
| fallback wait maps (size/void/ready) | AsyncIoContext | ctx mutex | yes |
| backend queue/router/ledger | backend | backend mutex | yes (workers / completion thread) |
| runtime State/CloseState/DriverState, control_epoch, counts | ApplicationRuntime | its mutex + epochs | yes (driver + external stop) |
| Event.set_, WaitQueue lists | primitive + Scheduler | `global_mtx_` (+ q.mtx()) | yes |
| TimerRegistration CAS state | TimerRegistration | lock-free CAS claims | yes |

---

## FZ-10. Thread / execution map

| Execution context | Created by | Runs | Blocking primitives used |
|---|---|---|---|
| Application thread | app | `ApplicationRuntime` driver loop, submit, join | run_live, close barrier |
| Scheduler worker threads (N, M:N) | Scheduler | fiber execution, steal, park | park_on_wake_source (futex-like via wait source; 2ms backstop) |
| Single-worker inline mode | Scheduler | run_impl with 1 worker on caller thread | same |
| ThreadPoolBackend workers (M) | backend | blocking syscalls pread/pwrite/fdatasync/fsync | condition_variable (work_cv) |
| uring completion path | caller/worker via wait_one | reap CQEs | io_uring submit_and_wait / poll(2)+eventfd |
| Group threaded path (zero-consumer) | Group | own thread run_live(1) | — |
| BlockingIoPool workers (zero-consumer) | pool | arbitrary blocking Task | condition_variable |

Synchronization inventory: one scheduler global mutex (+ wake/registry mutexes), one access_mtx in AsyncIoContext, per-backend mutex, per-WaitQueue mtx, atomics for epochs/deadlines/set flags/CAS timer claims. No lock-order document exists in code; empirical order observed: global_mtx_ → q.mtx() (event paths), access_mtx_ → backend → arena.

---

## FZ-11. Lifetime graph (destruction order)

```
ApplicationRuntime dtor
 └─ close_resources()
     1. Group:        wait all futures ready → clear futures, fibers, stacks
                      (fail-fast otherwise: group_lifetime_fail_fast)
     2. Scheduler:    assert wait_record_live_count_==0; join workers
     3. AsyncIoContext: assert outstanding()==0 (fail-fast); backend borrow ends
Backend dtors: fail-fast unless quiescent (threadpool / uring variants)
Completion dtor: legal only in idle/ready/reset terminal states
  (binding/outstanding destruction ⇒ completion_binding_destruction_fail_fast)
RequestArena: all slots free else request_arena_destruction_fail_fast
Fiber stack: freed by Group only after done; final context_switch never returns
```
CODE BASIS: `src/async/application_runtime.cpp` (close_resources), `src/async/group.cpp`, `src/async/scheduler.cpp:104`, `src/async/fail_fast.cpp` (catalog).

---

## FZ-12. Suspicious architecture facts (S1–S7)

Category definitions used (fixed for this audit):
- **S1 duplicated/ambiguous authority** — same responsibility enforced in ≥2 places with unclear primacy.
- **S2 lifetime hazard candidate** — ownership/borrow window that may admit UAF/dangle under adversarial interleaving.
- **S3 unchecked invariant** — invariant asserted in some paths but not machine-checked everywhere it must hold.
- **S4 dead mechanism** — public surface with zero real consumers.
- **S5 build/export mismatch** — files/headers vs compiled targets disagree.
- **S6 unmeasured complexity/perf hazard** — algorithmic or structural cost with no measurement gate.
- **S7 test coverage gap** — behavior exists, is load-bearing, and has no test.

| ID | Category | Fact | CODE BASIS |
|---|---|---|---|
| SUS-1 | S1 | Access legality enforced twice on async path (await layer + AsyncIoContext gate). Both live; primacy unstated in code. | `src/async/file.cpp`, `src/async/async_io_context.cpp` |
| SUS-2 | S1 | `sync_data`/`sync_all` not access-gated at any layer (read_only fd can sync). Consistent at both layers — possibly intentional (POSIX permits), but no code contract states it. | same |
| SUS-3 | S2 | `NativeFileRef` copies {fd, access} by value at submit; if caller closes File while op outstanding, backend lowers a closed fd (no borrow tracking). Mitigated only by AsyncIoContext outstanding fail-fast at destruction time, not per-op. | `include/sluice/async/async_io_context.hpp` |
| SUS-4 | S3 | `~Scheduler` asserts `wait_record_live_count_==0` but does NOT assert `waiting_size_/waiting_void_/waiting_ready_` fallback maps empty (they live on AsyncIoContext side; no cross-object check at teardown). | `src/async/scheduler.cpp:104`, `src/async/async_io_context.cpp` |
| SUS-5 | S4 | Entire sync-primitive family public + scheduler-supported, zero consumers and zero tests: `Event`, `AsyncMutex`/`Mutex`, `Semaphore`, `AsyncCondition`, `AsyncRwLock`, `lock_guard`, `AsyncQueue`, `select`. | include/sluice/async/*.hpp; consumer sweep (only false positive: grep app `MatchEvent`) |
| SUS-6 | S4 | Zero-consumer core surfaces: `BlockingIoPool`, `IoContext`/`BlockingIoContext`/`MemoryIoContext`, `FileReader`/`FileWriter`, streaming `Reader`/`Writer`/copy/stream_to/copy_strategy/limit, `wal`, `buffer`/`BufferedReadable`, `observed`, `fault`, `Batch`, `op_helpers`, Group threaded path, `Scheduler::run()`/`run_until_idle()` drain mode. | module table census |
| SUS-7 | S5 | `src/experimental/*.cpp` (2 TUs) compiled into NO target; their headers are public under `include/sluice/experimental/`. Build and export surface disagree. | `xmake/libraries.lua` glob; `ls src/experimental` |
| SUS-8 | S5 | `sluice_async` is an opt-in target (`set_default(false)`); nothing in-tree forces its compilation in default configure — apps depend on it so CI-via-apps covers it, but a core-only build silently skips all async code. | `xmake/libraries.lua` |
| SUS-9 | S6 | Uring router cookie→slot lookup is reverse linear scan O(outstanding) per CQE in production path. | `src/async/uring_backend.cpp` (router table lookup) |
| SUS-10 | S6 | `RequestArena::resolve_completion` linear scan per cancel / register_waiter. | `src/async/detail/request_arena.hpp` |
| SUS-11 | S7 | No tests for Event/Select/AsyncQueue/mutex/semaphore/condition/rwlock/Batch/Group public API. | `xmake/tests.lua` census |
| SUS-12 | S7 | Scheduler park/wake epoch protocol (refusal conditions, dance epochs, backstop) has no dedicated test binary. | tests census |
| SUS-13 | S1 | Terminal publication authority split across Completion (state machine), RequestArena (reap order), backend (record) — three objects must agree; agreement enforced by fail-fasts, not by type structure. | completion.hpp, request_arena.hpp, backends |
| SUS-14 | S3 | `earliest_active_deadline_` atomic + heap recompute protocol has no invariant check tying heap minimum to the atomic in release builds (asserts only). | `src/async/scheduler_timer.cpp` |
| SUS-15 | S2 | `SchedulerWakeHandle::Control{alive, scheduler}` raw shared state: producer threads race `alive` check vs scheduler destruction; ordering guaranteed only by wake protocol discipline. | scheduler_park_wake.cpp |
| SUS-16 | S5 | 31-function fail-fast catalog compiled into production `sluice_async` (terminate-only) — surface/name discipline is manual; no single registry ties each to its invariant. | `src/async/fail_fast.cpp` |
| SUS-17 | S4 | Test-seam TUs compiled into production target (guarded by `SLUICE_ASYNC_INTERNAL_TESTING`); guard macro is build-global, not per-TU-namespaced. | `src/async/*_test_seam.cpp` |

---

CODE REALITY IS FROZEN at PROOF_ROOT `7f5a3f59515911fabc3c41a3846b0bad3bef1688`.
From this point: ADR/mission/architecture documents may be read for Phase B comparison; nothing in this freeze may be edited — errors are handled by append-only corrigendum in the final report.
---

# Phase B — ADR-intended architecture (read after CODE_REALITY_FREEZE)

## 17. ADR-intended architecture, reduced to the same figure categories

Sources: `docs/mission.md`, `docs/adr/0001-explicit-io-design-doctrine.md`, `docs/adr/0002-explicit-file-api-architecture.md` (+ `0002-*-views.md`), `docs/architecture.md` (current-code snapshot, non-normative). ADR-0002 §1 mermaid graph is the normative architecture; the views companion adds the semantic spine and the boundary-discipline view. Reduced to this audit's figure vocabulary:

```
Application
  │ CALL
  v
File Resource (identity / ownership / lifetime / access)     [ADR-0002 §2]
  ├─ Resource Lifecycle: open(3 axes) / close                 [§3]
  ├─ Observable File State: size / resize / minimal metadata  [§4]
  ├─ Canonical Operations: seq read/write, pos read/write,
  │     sync_data / sync_all   (vectored = evidence-gated)    [§5]
  └─ Composed Operations: exact / all / copy                  [§11]
        │ two API levels (§6)
        ├─ Common Logical API  (initiate + logical wait → Result)
        └─ Explicit Operation API (Operation + Completion,
             multiple outstanding / cancellation / identity)
             │ execution must stay explicit (§6.3: no disguise)
             v
Replaceable Execution (§8):
  Blocking (first-class, shortest path, no async machinery)
  ThreadPool (blocking syscall offload; workers/QD = policy)
  io_uring (native async; honest refusal when absent)
Side bands (must NOT define semantics): capabilities (direct I/O,
space reservation), hints (advice), backend policy (registered
resources, polling)                                        [§10]
Async correctness facts stay explicit (§9): admission, capacity,
identity/generation, terminalization, publication, cancellation,
deadline, wait/wake, buffer lifetime, reuse — with
  backend terminalization != public terminal completion publication
Lifetime law (§7): reuse authority only from caller-observed
public terminal completion publication.
```

Normative inequalities that govern the diff: `backend capability != semantic authority`; `execution policy != semantic core`; `hint != authority`; `build artifact != semantic world`; `backend terminalization != public publication`.

## 18. Semantic contract table

| CID | ADR § | Claim | Must | Must not |
|---|---|---|---|---|
| CT-01 | 0002 §2.1 | File is the canonical resource root | public operations express file resources through File(-derived) semantics carrying identity+lifetime+access | express canonical resource as bare `fd + raw pointer + length + offset` |
| CT-02 | 0002 §2.1 | `native_handle()` is interop mechanism | remain an explicit escape hatch | become the canonical public resource API |
| CT-03 | 0002 §2.2 | access direction ≠ resource identity | access be part of the File contract | independent reader/writer resource-identity types claim survival right |
| CT-04 | 0002 §3.1 | open = 3 independent axes + combination legality | read_only/write_only/read_write × open_existing/create_if_missing/create_new × preserve/truncate; `read_only+truncate` illegal; `create_new` fails when target exists; truncate caller-visible | hide truncate in defaults; auto-truncate on plain open_existing |
| CT-05 | 0002 §3.2 | close = explicit, once, error-reportable | release exactly once; post-close object inert; `close()` returns Result; dtor best-effort | double-close a possibly recycled handle; dtor pretend to report errors |
| CT-06 | 0002 §3.2 | close does not drain/cancel/pin outstanding ops | caller observe public terminal completion before close | runtime gain extra serialization/lifetime authority from this rule |
| CT-07 | 0002 §4 | size/resize = observable state; minimal metadata | `size` LEGAL all access; `resize` = semantic mutation; only correctness-proven metadata in Core | build a general Metadata framework |
| CT-08 | 0002 §5 | canonical op vocabulary | seq read/write, pos read/write, sync_data/sync_all shared across executions | vectored auto-entering canonical vocabulary (evidence-gated only) |
| CT-09 | 0002 §5.1 | sequential ≠ positional | each keep its own observable contract | simulate positional via hidden seek+read/write |
| CT-10 | 0002 §5.2 | shared short-I/O/EOF contract across executions; composition rules | zero-length → success 0; partial read → success; read 0 on non-empty = EOF success; write zero-progress = error exit only for compositions; no infinite spin; errors via Result | backends redefine zero/partial/EOF; compositions spin forever |
| CT-11 | 0002 §5.4.1 | SyncData guarantee = completed-before-sync boundary | cover writes whose completion happens-before this submission | cover merely-submitted-before writes |
| CT-12 | 0002 §5.4.2 | durability anchored by completion happens-before; supersession rule | ordering decides membership; supersession decides recoverable state | promise snapshot preservation; rely on queue/FIFO policy |
| CT-13 | 0002 §5.4.3 | metadata boundary | retrieval-required state (incl. file size) covered | exclude retrieval-required size; decide bare-resize durability |
| CT-14 | 0002 §5.4.4 | SyncAll = SyncData + broader file metadata | per-actor happens-before boundaries for metadata | promise cross-actor global ordering |
| CT-15 | 0002 §5.4.5 | no directory-entry durability | filename/rename/reachability excluded | leak directory semantics into file ops |
| CT-16 | 0002 §5.4.6 | success/failure/cancel semantics | success = guarantee established (subject to supersession); failure ≠ nothing durable; cancel grants no durability fact | infer "all non-durable" from failure |
| CT-17 | 0002 §5.4.7 | no fabricated durability success | honest backends only report established guarantees | conforming execution fabricate success |
| CT-18 | 0002 §5.5 | access-legality matrix | read: LEGAL/ILLEGAL/LEGAL; write: ILLEGAL/LEGAL/LEGAL; size: all LEGAL; resize: ILLEGAL/LEGAL/LEGAL; sync_data/sync_all: all LEGAL | backend become legality authority |
| CT-19 | 0002 §5.5 | illegal initiation rejected before any OS/backend effect, as `invalid_argument`, on ALL initiation surfaces (blocking common, evented File-facing, explicit File-derived submission) | same legality + same error category everywhere | delegate rejection to OS errors |
| CT-20 | 0002 §5.5 | admission precedence: 1 closed→`invalid_state`; 2 legality→`invalid_argument`; 3 zero-length→success 0; 4 offset/size validation→`invalid_argument` | wrong-access zero-length still `invalid_argument` | reorder checks |
| CT-21 | 0002 §5.5 | File-derived explicit op reference retains the access fact; raw native-handle interop reference is the explicit, non-canonical exception | `NativeFileRef`-equivalent carry access or executable provenance | access-erasing representation at File→op conversion; interop exception spreading |
| CT-22 | 0002 §6.1 | common logical API exists for the canonical ops; ordinary callers not forced to manage arena/generation/Completion FSM/handles | initiate + logically wait → Result; evented form may suspend internally without leaking bookkeeping | force ordinary callers to manage request machinery |
| CT-23 | 0002 §6.2 | explicit operation API for outstanding authority needs; canonical resource reference points to File semantics | internal lowering to fd allowed | bare fd own public semantic authority |
| CT-24 | 0002 §6.3 | common and explicit API must not disguise each other | blocking-ness / outstanding-ness / cancellation / lifetime / bounded consumption stay visible at the boundary | silent `file.read()` guessing execution |
| CT-25 | 0002 §7.1 | common op: File + buffer alive until call returns | evented suspend/resume hide extra bookkeeping | leak lifetime bookkeeping to ordinary callers |
| CT-26 | 0002 §7.2 | explicit outstanding: caller keeps File + buffer alive until public terminal completion observed | obligation documented and enforceable-by-contract | runtime introduce pins/registries without earned evidence |
| CT-27 | 0002 §7.3 | reuse authority only from caller-observed public terminal completion publication | keep the 4-way distinction: waiter cancellation ≠ backend request cancellation ≠ physical completion ≠ public publication; await-style return ends obligation only when terminal observed | waiter wake / cancel request / syscall completion grant reuse authority |
| CT-28 | 0002 §8.1 | blocking = first-class, shortest legal path | direct syscall; no Completion/RequestArena/Scheduler/Fiber on this path | tax low-concurrency callers with async machinery |
| CT-29 | 0002 §8.2 | threadpool config = policy; named bounds where caller-visible | worker count / dispatch / queue depth stay policy; saturation-visible bounds get named | promote all perf parameters to semantics |
| CT-30 | 0002 §8.3 | io_uring = execution backend, not semantic authority | opcode existence ≠ public API | kernel opcode drive public surface |
| CT-31 | 0002 §8.4 | build artifacts ≠ semantic worlds | sluice_core/sluice_async may persist as targets | targets reinterpreted as two long-term semantic worlds |
| CT-32 | 0002 §9 | async correctness facts stay explicit | admission, capacity, identity/generation, terminalization, publication, cancellation, deadline, wait/wake, buffer lifetime, reuse present as facts; backend terminalization ≠ public publication | machinery claim automatic survival right |
| CT-33 | 0002 §10 | capabilities/constraints/hints earn authority individually | direct I/O as capability+validity constraint (no silent buffered fallback); advice as hint only; registered resources/polling as backend capability/policy | generic CapabilitySet/IoOptions/planner framework |
| CT-34 | 0002 §11 | composition = local, explicitly granted transformation boundary | copy choose lowering freely beneath its boundary | mechanisms become public primitives; generic capability framework |
| CT-35 | 0002 §12–14 | every public/Core concept categorizable; verdict vocabulary KEEP/CONVERGE/ADD_MINIMAL/DELETE/RESEARCH/OUT_OF_SCOPE; zero consumers alone ≠ DELETE | disposition audits follow §14 burden of proof | `TECHNICAL_DEBT` as a verdict |

---

# Phase C — reality × ADR diff

## 19. DIFF cards

Verdict vocabulary (fixed): `MATCH` / `CODE_DRIFT` / `DOC_DRIFT` / `AMBIGUOUS_AUTHORITY` / `UNIMPLEMENTED_CONTRACT` / `EXTRA_MECHANISM` / `CORRECTNESS_RISK` / `FORMAL_GAP` / `OUT_OF_SCOPE`.

| ID | CT | Verdict | Evidence (source path :: symbol → call path) |
|---|---|---|---|
| DC-01 | CT-01/02 | **MATCH** | `include/sluice/file_resource.hpp` :: `File` — move-only, owns fd + `FileAccess`; all canonical ops take `File`/`const File&`; `native_handle()` used by apps only at classified interop points (fstat, mkstemp dst) |
| DC-02 | CT-03 | **MATCH** (presence sanctioned as §13/§14-undecided) | `include/sluice/file.hpp` :: `FileReader`/`FileWriter` retained, zero real consumers (only `src/io_context.cpp` factory); disposition tracked by legacy-surface process (#355); zero-consumer fact recorded as SUS-6 |
| DC-03 | CT-04 | **MATCH** | `src/file_resource.cpp` :: `File::open` — `read_only+truncate` → `invalid_argument` pre-OS; `create_new` → `O_CREAT\|O_EXCL`; axes map 1:1 to `FileOpen` |
| DC-04 | CT-05 | **MATCH** | `src/file_resource.cpp` :: `close()` — `std::exchange(fd_,-1)` guarantees single release + inert post-close; error → `Result`; dtor `(void)close()` best-effort |
| DC-05 | CT-06 | **MATCH** | no drain/cancel/pin mechanism on `File::close`; backend destructors fail-fast independently (COR-2: safety is by backend-destructor construction, not close authority) |
| DC-06 | CT-07 | **MATCH** | `src/blocking_file.cpp` :: `size` (fstat, LEGAL all access — only `is_open` check), `resize` (ftruncate, `read_only` → `invalid_argument`); no metadata framework exists |
| DC-07 | CT-08 | **MATCH (blocking full; evented positional+sync only)** | blocking: `read`/`write` (::read/::write, kernel shared offset), `read_at`/`write_at` (pread/pwrite), `sync_data` (fdatasync), `sync_all` (fsync); evented: `await_read_at`/`await_write_at`/`await_sync_data`/`await_sync_all` |
| DC-08 | CT-08/22 | **UNIMPLEMENTED_CONTRACT (low severity)** | evented common-API sequential `read`/`write` absent (`include/sluice/async/file.hpp` has no `await_read`/`await_write`); apps use positional with caller-managed offsets; no consumer demands sequential evented form today |
| DC-09 | CT-09 | **MATCH** | positional via pread/pwrite (op-owned offset); sequential via ::read/::write with explicit comment "the kernel owns and atomically advances the shared offset" (`src/blocking_file.cpp:67`); no hidden seek simulation |
| DC-10 | CT-10 | **MATCH (observable result), mechanism corrected per review B** | observable zero-length outcome = success 0 with no data I/O on every surface. Mechanism differs by surface: blocking (`src/blocking_file.cpp:23,49`) and evented (`src/async/file.cpp:15,29`) short-circuit pre-I/O; the **explicit** surface does **not** short-circuit — backend validators' `len==0 → {}` (`src/async/threadpool_backend.cpp:140`, `src/async/uring_backend.cpp:281/295`) is a validation *pass*; `submit_transaction.hpp:30-39` reserves the slot first, then validates, then the op executes a real zero-length lowering (`::pread(fd, buf, 0, 0)`; uring SQE with `native_length=0`, offset normalized, `threadpool_backend.hpp:251-258`, `uring_backend.hpp:359-375`). Observable conformance holds (success, progress 0, no EOF semantics, no data transfer), but a zero-length explicit submit **consumes bounded request capacity** and under saturation returns `would_block` — see DC-13 |
| DC-11 | CT-11..17 | **MATCH** | durability lowerings: fdatasync/fsync (threadpool + blocking), `IORING_FSYNC_DATASYNC` vs plain fsync (`src/async/uring_backend.cpp:601/604`) — exactly the ADR's possible-lowerings list; ordering/supersession are caller-obligation contracts with no contradicting runtime mechanism; honest uring stub refuses without liburing (lines 38-51, no fabricated success); `async_sync_admission_test` = 4 named tests |
| DC-12 | CT-18/19 | **MATCH** | `src/async/async_io_context.cpp` :: `access_legality(file, write_side)` — read illegal iff `write_only`, write illegal iff `read_only`, sync ungated (LEGAL all) — matrix rows exactly; enforced at **all three** initiation surfaces: blocking (`src/blocking_file.cpp:20,46,72,92`), evented (`src/async/file.cpp:12,26`), explicit submission (`src/async/async_io_context.cpp:99,112`); backends never see access facts (mechanism-only) |
| DC-13 | CT-20 | **AMBIGUOUS_AUTHORITY (strict reading: CODE_DRIFT)** — capacity/admission-closed preemption at the explicit surface | the four-way relative order holds everywhere (blocking `src/blocking_file.cpp:15-30` exact; evented `src/async/file.cpp:9-17` = 1-2-3 with rule 4 deferred to the backend validator; explicit `src/async/async_io_context.cpp:96-101` = 1-2 with rules 3/4 in the backend validator). But at the explicit surface rules 3/4 execute **after** `arena.reserve()` (`submit_transaction.hpp:30-39`) and after uring `stage0_precheck`; `reserve()` returns `would_block` on saturation and `invalid_state` when admission is closed (`request_arena.hpp:126-134`). Under saturation a zero-length request therefore returns `would_block` (not the frozen success-0) and an unrepresentable-offset request can return `would_block` (not `invalid_argument`), while blocking/evented surfaces return the frozen outcomes unconditionally — a cross-surface divergence precisely in the collision regime §5.5 freezes ("同一规则覆盖所有 initiation surface…同一错误类别"). The ADR never ranks the named capacity bound (§8.2/§9) against the §5.5 precedence list, so this is adjudication-pending rather than outright drift. Tests never exercise the saturated regime (F-6 note) |
| DC-14 | CT-21 | **MATCH** | `NativeFileRef{int fd, FileAccess}` retains the access fact; implicit `NativeFileRef(const File&)` copies handle+access at op construction; bare `int` cannot construct an op — the interop exception is spelled out (`dst: NativeFileRef{int, declared access}` in sluice-copy) |
| DC-15 | CT-24 | **MATCH** | `blocking::*` = explicit blocking; `await_*` = explicit evented (takes `RuntimeTaskContext`); no execution-guessing universal method exists. (CT-22's caller-cost question is adjudicated separately in DC-29.) |
| DC-16 | CT-23 | **MATCH** | explicit ops `ReadOp`/`WriteOp`/`SyncDataOp`/`SyncAllOp` + `submit_*` + `Completion<T>`; `AsyncBackend::submit_*` private — only `AsyncIoContext` can enter backends |
| DC-17 | CT-25/26/27 | **MATCH** | common ops return after logical wait; explicit path documents caller-borne lifetime; the 4-way cancellation distinction is encoded (waiter cancel via `cancel_waiter` lease path; request cancel via backend `cancel_handle_`/`resolve_completion`; physical completion ≠ `Completion` publication — arena ready-ring + `reap_seq` publication ordering separate); await-style obligation end only on observed terminal (`await_take` waits `ready()`; on wait-layer error the Completion stays outstanding and the obligation continues — exactly §7.3's rule) |
| DC-18 | CT-28 | **MATCH** | blocking path = direct syscalls in `src/blocking_file.cpp`; zero includes of async runtime; shortest legal path confirmed |
| DC-19 | CT-29 | **MATCH** | `RequestArena(context, request_capacity)` — named bound; `reserve()` failure → `capacity_rejections_` + caller-visible `would_block`/`invalid_state` rejection (`request_arena.hpp:126-160`); worker count/queue depth remain backend policy. Observation: `BoundedDispatchQueue` overflow chooses fail-fast terminate, not admission rejection — an allowed policy choice, not a named-bound violation |
| DC-20 | CT-30 | **MATCH** | uring backend mechanism-only; `liburing` option default off → honest stub; no public API derived from opcodes |
| DC-21 | CT-31 | **MATCH** | two build targets persist; canonical File semantics span both (File in core; await layer in async consuming the same File) |
| DC-22 | CT-35 / repo hygiene | **EXTRA_MECHANISM + CORRECTNESS_RISK** | `src/experimental/uring_io_context.cpp`, `src/experimental/uring_write_batch.cpp` match no target (`xmake/libraries.lua` globs are `src/*.cpp` and `src/async/*.cpp`), yet `include/sluice/experimental/uring_io_context.hpp` + `uring_write_batch.hpp` are public — consuming them cannot link; build/export disagree (SUS-7) |
| DC-23 | CT-32 | **MATCH** | all §9 facts present as explicit machinery: admission (`access_mtx_` + capacity), request identity/generation (arena slot FSM + generations), terminalization vs publication split (backend record → arena ready ring → `Completion` publish), cancellation, deadline (timer heap), wait/wake (epoch protocol), buffer lifetime (caller), reuse (ready()-observed) |
| DC-24 | CT-33 | **MATCH** | no generic capability/options/planner surface exists; direct I/O, advice, registered resources, polling absent from public semantics (backend-local only where at all) |
| DC-25 | CT-34 | **MATCH** | copy implemented as application-level pipeline (thin local mechanism in `apps/sluice-copy/copy_task.cpp`), no capability framework; `copy.cpp` streaming composition retained only in zero-consumer legacy surface |
| DC-26 | CT-08 (vectored) | **MATCH (evidence-gated status respected)** | vectored exists only on legacy `FileReader`/`FileWriter` (`read_vec`/`write_vec`, chunked by IOV_MAX); no vectored on canonical File ops; disposition = KEEP pending legacy audit per the separate vectored decision doc referenced by `docs/architecture.md` §4 |
| DC-27 | doctrine (0001 §6) | **EXTRA_MECHANISM** | async sync-primitive family (`Event`, `AsyncMutex`/`Mutex`, `Semaphore`, `AsyncCondition`, `AsyncRwLock`, `lock_guard`, `AsyncQueue`, `select`) — public, scheduler-integrated, **zero consumers, zero tests** (SUS-5; apparent matches are substrings like `await_op_helpers`); `Batch`, `op_helpers`, `Scheduler::run()`/`run_until_idle()` drain mode same status (SUS-6) |
| DC-28 | CT-14 (metadata difference) | **MATCH** | `sync_data` → fdatasync / `IORING_FSYNC_DATASYNC`; `sync_all` → fsync / plain uring fsync — mechanism difference exactly tracks the metadata-coverage difference |
| DC-29 | CT-22 | **AMBIGUOUS_AUTHORITY** — which regime does `await_*` belong to? | every evented entry point requires a caller-supplied `Completion<T>&` (`include/sluice/async/file.hpp`); real apps declare per-op Completions (`apps/sluice-hash/hash_task.cpp:43`, `apps/sluice-copy/copy_task.cpp:37-38,55`); on wait-layer error/cancellation `await_take` returns without reset (`src/async/await_op_helpers.cpp:5-12`), leaving the ordinary caller holding an **outstanding** Completion with §7.2/§7.3 obligations — the "Completion FSM" conceptual cost §6.1 says ordinary applications should not be forced to manage. ADR §7.3 sanctions await-style operations, so the design is defensible **if** `await_*` is classified as §7.3 await-style surface rather than the §6.1 common API — but then DC-08's framing shifts (the missing sequential forms are missing from the *common* layer, which has no evented representative at all). The ADR does not force the classification; the code does not declare it. Adjudication-pending |
| DC-30 | CT-10 (compositions) | **AMBIGUOUS_AUTHORITY** — in-tree composition families disagree on premature EOF | `src/async/op_helpers.cpp` :: `read_all` returns `IoError{eof}` when a non-empty read returns 0 before the buffer is filled (lines 38-40); `src/async/await_op_helpers.cpp` :: `await_read_fill` (41-61; the variant sluice-copy actually uses, `copy_task.cpp:108`) returns the partial `filled` as **success** on the same event; write zero-progress exits use `invalid_state` (`write_all`) vs `backend_error` (`await_write_exact`) — error-code divergence is permitted (§5.2.3 fixes no codes), but ADR §5.2.1 makes premature-EOF-an-error a property of `exact`/`all` compositions specifically, and nothing in code or docs classifies `fill` against that vocabulary. If `fill` is an exact/all composition it contradicts the frozen rule; if it is a distinct composition type, that type is undeclared |

### 19.1 Resolutions of pre-freeze suspicions against the ADR

- **SUS-1 (dual access enforcement)** → defense-in-depth sanctioned: CT-19 requires the *same observable result* on every surface, not a single enforcement point; the await-layer check and the admission gate agree. **MATCH**, recorded as an observation, not drift.
- **SUS-2 (sync not access-gated)** → **mandated** by CT-18 (sync row LEGAL for all access; ADR explains why: durability coverage is anchored by completion happens-before, independent of the initiating handle's write capability). **MATCH**.
- **SUS-3 (fd borrow window)** → caller-contract territory per CT-26/27 (ADR §7.2/§7.3 explicitly decline to define the observable result of violation and decline runtime pinning). Not a violation; retained as the one genuine correctness-risk window of this audit. Worst variant is fd **recycling**: close File during an outstanding op, a new open reuses the fd number, and the outstanding pread/pwrite/SQE operates on the wrong file — silent corruption, not EBADF (review D). Same-class undocumented caller-contract surface: `waiting_ready_` keys are raw `const std::atomic<bool>*` (`scheduler.hpp:536-537`); destroying the flag while awaited is a dangling-key load. Both belong in PLAN-6's documentation note.
- **SUS-8 (opt-in sluice_async)** → CT-31 makes build shape non-semantic; apps/tests link the target so in-tree coverage exists. Observation only.
- **SUS-15 (external-wake Control race)** → **re-graded: safe by construction** (review D, verified). `SchedulerWakeHandle::notify()` holds `Control::mtx` across both the `alive`/`scheduler` check *and* the `control_->scheduler->notify_external_wake()` call (`scheduler_park_wake.cpp:22-46`); `~Scheduler` clears `{alive=false, scheduler=nullptr}` under the same mutex in the destructor body (`scheduler.cpp:74-81`; fields `SLUICE_GUARDED_BY(mtx)`), which strictly precedes member destruction; no lock inversion exists. Any notify observing `alive==true` completes its Scheduler-member access before the destructor can clear; after the clear, notify returns false without touching the Scheduler. No TOCTOU window. Downgraded to a regression-test note (PLAN-5).

Additional observation (review B, not an ADR violation — not one of the four frozen checks): the null-buffer+positive-length guard exists only in the backend validators; the blocking surface with null `data()` + positive length reaches `::pread`/`::write` and returns an OS `EFAULT`. The explicit surface returns `invalid_argument` for the same input. Recorded as a cross-surface input-validation asymmetry outside the frozen precedence list.

## 20. Gap classification (A–E)

| Class | Definition | Items |
|---|---|---|
| **A** — documentation-only gap | doc must change, code is right | none found (`docs/architecture.md` code claims verified accurate at PROOF_ROOT) |
| **B** — local implementation gap | fixable inside one module | B1: unbacked experimental headers/TUs (DC-22); B2: release-build invariant checks for timer-heap/earliest-deadline agreement (SUS-14); B3: `~Scheduler` residual-wait-map asserts (`waiting_size_`/`waiting_void_`/`waiting_ready_`/`waiting_waitq_count_` beside the existing `wait_record_live_count_` check — owner corrected per COR-3) |
| **C** — cross-module architecture gap | needs coordination or adjudication across modules | C1: dead sync-primitive family disposition (DC-27); C2: legacy zero-consumer world disposition (tracked externally by #355); C3: evented sequential ops absent (DC-08, low); C4: DC-13 capacity-vs-precedence adjudication; C5: DC-29 await-regime classification; C6: DC-30 composition-EOF classification |
| **D** — concurrent-correctness gap | concurrency window needing stress/formal evidence | D1: park/wake epoch protocol unverified (SUS-12, COR-5 unbounded variant); D3: fd borrow + fd recycling during outstanding ops (SUS-3, caller-contract; D2 "Control race" resolved safe-by-construction — only a regression test remains, filed under B); D4: wait-routing generation protocol unverified |
| **E** — formal-model candidate | mechanism whose guarantees merit a model | E1: park/wake epochs incl. backend split-wait domain; E2: request-slot FSM + generations + Completion CAS lifecycle; E3: wait routing; E5: teardown barrier ordering; E6: uring cancel deferred-terminal protocol (see §21–§23) |

---

# Phase D — formal-method triage

## 21. Triage method and TLA+ admission test

Per-claim question: *what is the minimal adequate evidence tool?* A claim is admitted to TLA+ modeling if it passes admission tests T1–T5 and T7:

- **T1** — the guarantee is a state-machine safety/liveness property, not a data-value property;
- **T2** — the interesting states are enumerable at small scale (bound workers/requests/slots);
- **T3** — the property is statable without modeling the OS/kernel (interface abstraction exists);
- **T4** — violations are interleaving-dependent (testing alone cannot exhaust them);
- **T5** — a refinement mapping from model actions to C++ transitions can be written (named functions/methods);
- **T7** — a non-vacuity witness can be constructed (a mutant/protocol variation the model must catch).

**T6** (tool support in-tree) is **not** an admission gate: nothing in-tree provides it today (§24), so every admitted model carries a budgeted bootstrap cost (toolchain choice + pinning). Marking T6 "✗ bootstrap" therefore does not block admission; it prices it.

### Admission results

| Claim | T1 | T2 | T3 | T4 | T5 | T6 | T7 | Verdict |
|---|---|---|---|---|---|---|---|---|
| Park/wake epoch protocol (no lost wake; bounded park where bounded; refusal conditions) | ✓ | ✓ | ✓ | ✓ | ✓ (park_on_wake_source/signal_wake_locked) | bootstrap | constructible | **ADMIT (E1)** |
| Backend split-wait (BackendWaitToken progress/control generations; interrupt path) | ✓ | ✓ | ✓ | ✓ | ✓ (consume_committed_wait/wait_for_change/interrupt_backend_waiters) | bootstrap | constructible | **ADMIT as E1's second park domain** (review C: unbacked sub-claim otherwise) |
| Request-slot FSM + generations + Completion CAS lifecycle (no double-terminal; no ABA reuse; exactly-once publication across the three lock domains) | ✓ | ✓ | ✓ | ✓ | ✓ (arena transitions; completion.hpp CAS paths) | bootstrap | constructible | **ADMIT (E2)** |
| Wait routing (WaiterToken/lease: no stale delivery, no lost ready) | ✓ | ✓ | ✓ | ✓ | ✓ (on_ready/route_runnable) | bootstrap | constructible | **ADMIT (E3)** |
| Teardown barrier ordering (group→sched→io_ctx + drain conditions + join topology) | ✓ | ✓ | ✓ | ✓ | ✓ (close_resources, run_impl joins, backend dtor joins) | bootstrap | constructible | **ADMIT (E5, small model)** |
| Uring cancel deferred-terminal protocol (op-CQE vs control-CQE race) | ✓ | ✓ | ✓ (kernel abstracted as nondeterministic CQE order) | ✓ | ✓ (cancel_handle_/issue_running_cancel/route.deferred_terminal_stored) | bootstrap | constructible | **ADMIT (E6)** — review C: this is distinct from the rejected poison-and-recover row and passes admission |
| Access-legality precedence | ✗ (deterministic function; COR-4 confirms single-writer evaluation) | — | — | ✗ | — | — | — | **REJECT → table tests** (exist: file_access_precedence_test, 40 cases) |
| Durability ordering §5.4.2 | ✗ (caller-obligation calculus; no runtime mechanism to model) | — | — | ✗ | — | — | — | **REJECT → doc + caller-pattern tests** |
| uring transport ledger / poison-and-recover | ✓ | ✗ (kernel-coupled state space) | ✗ | ✓ | partial | bootstrap | — | **REJECT → fault-injection stress tests** (none in tree — L3 gap, §25) |
| TimerRegistration CAS states (active/retired/consumed) | ✓ | ✓ | ✓ | ✗ (all transitions under `global_mtx_`; review C verified) | — | — | — | **REJECT → tests suffice** (T4 fails) |
| M:N work-stealing + admission election | ✓ | ✗ at full scale | ✓ | ✓ | ✓ | bootstrap | — | **DEFER** — model only the election/terminate lattice if E1 finds defects |

## 22. Model cards (NO TLA+ EXISTS YET — these are admission records, not models)

| MC | Target | Scope sketch | Model entities | Properties | Refinement anchors (C++) | Vacuity/mutation defense |
|---|---|---|---|---|---|---|
| MC-1 (E1) | Scheduler park/wake, **two park domains** | workers ≤2; wake epochs unbounded counter; inbox 0/1; terminate flag; dance epoch; **Scheduler-domain park in two variants: unbounded (no deadline, no bounded backend observation — COR-5) and deadline/backstop-bounded; Backend-domain park with progress/control generations and `interrupt_backend_waiters` as a distinct wake mechanism** | Worker, EpochState, Inbox, BackendWaitGen | **Safety**: no committed park misses a prior wake (epoch monotone); refusal conditions never park with pending progress; backend park not left sleeping after progress generation advance. **Liveness**: wake ⇒ eventually unpark (weakly-fair wake handling), **stated per park variant** — the unbounded variant must satisfy it without any timeout action | `park_on_wake_source` (both variants), `signal_wake_locked`, `commit observed_epoch_`, `consume_committed_wait`/`wait_for_change`, `interrupt_backend_waiters`, `backend_wait_active_` | mutants: drop epoch compare; park before progress check; stale dance epoch accepted; drop interrupt on backend waiters; **adding an always-on backstop to the model (must NOT rescue the dropped-epoch mutant — else the model is vacuous per review C-1)** |
| MC-2 (E2+E4) | RequestArena slots **+ Completion CAS lifecycle** | slots ≤3; generations wrap at 2; one backend completer; one canceller; **Completion state machine idle→binding→outstanding→publishing→ready→resetting driven from three lock domains: submit-side claims (`begin_binding_for_backend`/`try_claim_for_backend`), arena-mutex publish (`publish_from_reap`), lock-free caller `reset()`/destructor** | Slot, Gen, Completion, ReadyRing | **Safety**: exactly-once terminal per (slot,gen) across submit/cancel/reap/reset; no publish on free slot; reap order = ring order; no ABA (stale gen rejected); no reset-vs-late-terminal race (reset CAS wins or terminal wins, never both) | arena state transitions, `validate_`, `resolve_completion`, `enqueue_in_flight_pin`, `reap_seq`, `completion.hpp` CAS/release paths (`rollback_claim_before_accept`, `rollback_binding_before_accept`, dtor release) | mutants: skip generation check; double-publish; publish-before-backend_ready; reset during publishing |
| MC-3 (E3) | Wait routing | 1 waiter fiber, 1 routing lease, token generations ≤2, one waker | WaiterToken, Lease, ReadyEvent | **Safety**: stale-token ready dropped ≠ delivered; no ready both dropped and delivered; delivered ⇒ fiber resumable exactly once | `ReadyRoutingSink::on_ready`, `route_runnable_locked`, `drain_routed_completion_waits_locked` | mutants: accept stale token; drop fresh token; double-resume |
| MC-4 (E5) | Teardown barrier + join topology | states of runtime/sched/backend, outstanding counter, driver predicate, worker joins | Runtime, Sched, Backend, Outstanding | **Safety**: teardown order group→sched→io_ctx respected; no destroy with outstanding>0 or live wait records; **workers joined (run_impl) before scheduler members destroyed; backend workers joined in backend dtor body before arena destruction (COR-2/6)**; routing sink detached before backend can still signal. **Liveness**: quiescent input ⇒ teardown terminates | `close_resources`, drain condition, `stop_predicate_fn`, `run_impl` worker joins, `ApplicationRuntime::join`, backend dtor join bodies | mutants: reorder close; drop outstanding check; drop wait_record assert; free stacks before run-join returns (must reproduce COR-7's latent hazard only in unsupported topologies, not supported ones) |
| MC-5 (E6) | Uring cancel deferred-terminal | one op CQE, one control CQE (cancel), kernel chooses order; cancel requested before/after terminal | OpRoute, ControlRoute, CancelIntent | **Safety**: exactly one terminal published whatever the CQE order; terminal never lost when cancel loses the race; deferred terminal flushed when control CQE arrives | `cancel_handle_` (arena disposition → intent_recorded → issue_running_cancel), `prep_cancel64` + CONTROL_TAG cookie, `route.deferred_terminal_stored` reconciliation (`uring_backend.cpp:743-765, 1042-1076, 1216-1227, 1338-1377`) | mutants: publish on control CQE without checking op terminal; drop deferred terminal; double-publish when both arrive |

Fairness assumptions to state explicitly in any future model: worker wake handling is weakly fair; backend completion delivery is fair (no infinite withholding); timer clock advances; the kernel eventually returns every submitted SQE's CQE (for MC-5). These are **environment assumptions**, not proven facts.

### MODEL ASSUMPTION → CODE WITNESS map (required before any model is trusted)

| Assumption | Code witness |
|---|---|
| `wake_epoch_` monotone under `wake_mtx_` | `signal_wake_locked` increments; park commits observed under same mutex (`scheduler_park_wake.cpp`) |
| stale slot operations rejected on every path | `RequestArena::validate_` returns graceful `not_found`/`invalid_state` for stale handles (`request_arena.hpp:624-647`); `enqueue`/`mark_running` additionally fail-fast (corrected per review C-6: most stale paths are graceful, not fail-fast) |
| ready-ring pushes maintain ring invariants under arena mutex | push-side invariant guard `request_arena_ready_ring_invariant_fail_fast` (`request_arena.hpp:649-660`); pop-under-mutex + `completion_ready` transition carry exactly-once consumption (no dedicated check — a property the model or a possible B-class assert must supply, review C-6) |
| waiter lease invalidation before record free | `cancel_waiter` lease-based free path under `wait_registry_mtx_` |
| driver re-entry decisions observe `control_epoch_`; in-run stop decisions read only atomic snapshots | `stop_predicate_fn` reads atomic snapshots (`application_runtime.cpp:527-532`); epoch governs re-entry/drain-wait (481-525) (corrected per review C-6) |
| park boundedness is conditional | backstop applies only with deadline or bounded backend observation; otherwise unbounded park (COR-5, `scheduler_park_wake.cpp:258-296`) |
| memory-ordering discipline | `wake_epoch_`/`observed_epoch` under `wake_mtx_`; `global_terminate_` atomic; inbox under its mutex; predicate inputs are either mutex-guarded or atomic-acquire (`scheduler_park_wake.cpp:212-219`) (added per review C-5) |
| wake-producer completeness | every work-creating path signals: `route_runnable_locked` always calls `signal_wake_locked` (`scheduler.cpp:935`); `attach_ready_wake` (`scheduler_park_wake.cpp:897`); timer pump `advance_clock` signals (`scheduler_timer.cpp:54`) (added per review C-5) |
| idle-dance discipline | `dance_epoch_`/`idle_workers_` reset discipline around admission (`scheduler.cpp:906-910`) (added per review C-5) |

## 23. FORMAL MATRIX

| # | Guarantee | Current evidence level | Target | Minimal adequate tool | Priority |
|---|---|---|---|---|---|
| F-1 | No lost wake across park commit (both park domains) | L0 (code reading) + fail-fasts | L4 | TLA+/TLC (MC-1) | **P1** — concurrency core, zero tests |
| F-2 | Request identity: exactly-once terminal, ABA-safe, publication-safe across lock domains | L1 (explicit_file_ref_test, 31 checks, zero-concurrency) | L4 | TLA+/TLC (MC-2) | **P1** |
| F-3 | Wait routing stale-drop correctness | L0 | L4 | TLA+/TLC (MC-3) | **P2** |
| F-4 | Completion reap ordering | L1 (single-threaded tests) | L4 | merged into MC-2 | P2 |
| F-5 | Teardown barrier + join topology ordering | L0 + asserts | L4 | TLA+/TLC (MC-4) | P3 |
| F-6 | Access-legality precedence (incl. unsaturated regime) | L1 (file_access_precedence_test, 40 cases) | L1 (adequate) | table tests — **no model**; note: the **saturated regime** (DC-13 capacity preemption) is untested and awaiting ADR adjudication, not modeling | done + adjudication note |
| F-7 | Durability coverage anchoring | L2 (async_sync_admission_test, 4 tests) + ADR text | L2 | doc + caller-pattern tests — **no model** | done |
| F-8 | uring poison-and-recover | L0 | L3 | fault-injection stress test | P3 (tooling absent) |
| F-9 | Short-I/O/EOF cross-execution parity | L1 (file_read/write + blocking variants) | L1 | tests | done |
| F-10 | Backend split-wait: progress generation visible to parked worker | L0 | L4 | merged into MC-1 (second park domain) | P1 (with F-1) |
| F-11 | Uring cancel: exactly-once terminal under CQE racing | L1 (uring_backend_smoke_test, 14 checks, liburing-gated) | L4 | TLA+/TLC (MC-5) | **P2** |

## 24. Formal assets census

**NONE.** No `*.tla`, `*.cfg`, model-checker configs, or verification scripts exist anywhere in the tracked tree (verified by `rg --files` sweeps at freeze time). Historical formal campaigns are not present (clean-room reset deleted them; AGENTS.md ranks formal-model rebuild as step 7, after tests/docs). Consequence: every FORMAL_MATRIX row above is at its floor evidence level; T6 bootstrapping (toolchain choice and pinning discipline) is a real cost each admitted model must budget.

## 25. Evidence pyramid (current state)

| Level | Meaning | State at PROOF_ROOT |
|---|---|---|
| L0 | code reading / review | this audit (complete pass over all production TUs) + four adversarial reviews |
| L1 | contract/unit tests | 20 default test binaries (File/open/read/write/sync contracts, access precedence ×40 cases, io validation, explicit ref, app consumption) |
| L2 | integration / app-level | 4 app-consumption tests + 4 real apps as consumers |
| L3 | concurrency stress / sanitizers / fault injection | sanitizer *modes* exist in xmake (asan/tsan/ubsan); **no scheduled stress harness, no fault-injection tests in tree** (test-seam TUs exist but nothing compiles with `SLUICE_ASYNC_INTERNAL_TESTING` today; the only multi-thread test is a 2-thread kernel shared-offset check in `blocking_file_sequential_test.cpp`) |
| L4 | formal model + exhaustive checking | **absent** (§24) |
| L5 | verified refinement / proof | absent |

---

# Corrective plan (PROPOSED — nothing implemented, no issues opened)

## 26. PLAN cards

| PLAN | Class | Content | Size | Risk |
|---|---|---|---|---|
| PLAN-1 | B1 | Resolve experimental build/export mismatch: either compile the two experimental TUs under an explicitly gated target matching their public headers, or remove the public headers until backed. Human decision required (the experimental surface may be deliberately parked). | S | low |
| PLAN-2 | C1 | Dead-mechanism disposition round for the sync-primitive family + `Batch` + `op_helpers` + drain mode: produce the §14-verdict inventory (KEEP/CONVERGE/DELETE/RESEARCH per symbol family) with the zero-consumer + zero-test facts from SUS-5/6. Routes through the existing legacy-surface disposition process; ADR §14 burden of proof applies (zero consumers alone ≠ DELETE). | M | low (docs/decision) |
| PLAN-3 | B3 | Add teardown residual assertions in **`~Scheduler`** (owner corrected per COR-3): assert `waiting_size_`/`waiting_void_`/`waiting_ready_` empty and `waiting_waitq_count_ == 0` beside the existing `wait_record_live_count_` check (`scheduler.cpp:97-111`). | S | low |
| PLAN-4 | D1/D4 | Concurrency verification round: MC-1 (park/wake, both domains) and MC-3 (wait routing) TLA+ models with mutation kills, then a scheduled stress harness reusing the existing (currently dormant) test seams. Unlocks L3→L4 for the scheduler core. | L | medium |
| PLAN-4b | E6 | MC-5 (uring cancel deferred-terminal) model — can ride the same toolchain bootstrap as PLAN-4. | M | medium |
| PLAN-5 | B (downgraded from D2) | SUS-15 resolved safe-by-construction (§19.1): add a regression test pinning the `Control::mtx`-spanning check-and-use in `notify` and the destructor-body clear, so a future refactor cannot silently split the critical section. No hardening needed. | S | low |
| PLAN-6 | D3 | Document the fd-borrow caller contract at `NativeFileRef` (SUS-3): one comment at the interop site stating the CT-26 obligation, **naming the fd-recycling variant** (silent corruption, not EBADF) and the `waiting_ready_` raw-flag-key obligation, per the repo's comment policy (non-obvious Why only). | S | low |
| PLAN-7 | S6 | Measure before touching: uring router reverse-linear cookie scan and `RequestArena::resolve_completion` linear scan are *suspected* hotspots only; profile-first per AGENTS.md — no optimization task exists until measured. | M | n/a (measurement) |
| PLAN-8 | S7 | Test rebuild for whichever surfaces survive PLAN-2 disposition (park/wake protocol tests named first: F-1 gap). | M | low |
| PLAN-9 | C4/C5/C6 | ADR adjudication note (human decision): DC-13 (does the named capacity bound legitimately preempt §5.5 rules 3/4 at the explicit surface?), DC-29 (is `await_*` the §6.1 common API or a §7.3 await-style surface?), DC-30 (is `fill` an exact/all composition?). Each may resolve as doc clarification or small code alignment. | S per item | low |

## 27. Dependency DAG

```
PLAN-1 (experimental) ────────────────────────────── independent
PLAN-3 (teardown assert) ──┐
PLAN-5 (wake regression) ──┼──> PLAN-4 (models MC-1/MC-3 + stress) ──> PLAN-8 (test rebuild)
PLAN-6 (borrow doc) ───────┘          │
                                      └──> PLAN-4b (MC-5 rides the bootstrap)
PLAN-2 (disposition round) ──────────────────────────────────────────┘
PLAN-9 (adjudications) ──> may spawn small code/doc correctives after human decision
PLAN-7 (profiling) ───────────────────────────────────────────────── independent
```

Ordering rationale: teardown/wake-regression/borrow facts feed model assumptions; disposition (PLAN-2) gates which surfaces deserve rebuilt tests; PLAN-4b shares PLAN-4's toolchain cost; profiling is orthogonal and must not gate anything.

## 28. PROPOSED issues (NOT created — awaiting human decision)

1. **[PROPOSED] experimental/ public headers unbacked by any compiled target** — PLAN-1; evidence DC-22/SUS-7; files enumerated.
2. **[PROPOSED] Sync-primitive family + Batch + op_helpers + drain mode: zero consumers, zero tests — disposition round** — PLAN-2; evidence SUS-5/6/DC-27; must follow ADR §14.
3. **[PROPOSED] Teardown residual-wait assertion gap in ~Scheduler** — PLAN-3; evidence SUS-4 + COR-3.
4. **[PROPOSED] Park/wake (both domains) + wait-routing formal models and stress harness (MC-1/MC-3)** — PLAN-4; evidence F-1/F-3/F-10 at L0 floor, SUS-12, COR-5.
5. **[PROPOSED] NativeFileRef fd-borrow caller-contract note (incl. fd recycling + waiting_ready_ keys)** — PLAN-6; evidence SUS-3/DC-17/§19.1.
6. **[PROPOSED] ADR adjudications: capacity-vs-precedence, await-regime, composition-EOF** — PLAN-9; evidence DC-13/DC-29/DC-30.
7. **[PROPOSED] Perf measurement task (router scan / resolve_completion)** — PLAN-7; only after a measured hotspot exists, per repo policy — framed as "measure", not "optimize".

Deliberately **not** proposed: any change to legacy zero-consumer surfaces (externally tracked by #355), any ADR edit (no DOC_DRIFT found), any implementation slice from the A–P list (out of campaign scope).

---

# Adversarial reviews

## 29. Protocol

Four fresh-context reviews were dispatched against a draft of this report plus the repository at PROOF_ROOT, each with a single attack assignment and no access to the others' output. All material findings were independently re-verified by the auditor against code before acceptance (verification commands recorded in the audit log; the load-bearing ones — arena ownership, waiting-map ownership, reserve-before-validate order, composition EOF divergence, park unboundedness, test-table counts 4/40/40 — are reproduced in the corrigenda and cards they amended). The frozen Phase A content (§5–§16) was not edited; reviewer challenges to it are answered by corrigendum (COR-2..COR-7).

Reviewer A's first dispatch failed on an API rate limit and was re-dispatched after the others completed.

## 30. Review A — code-reality attack

**Verdict: FAIL (draft) — 12 findings (0×P1, 6×P2, 6×P3). All accepted after auditor re-verification; frozen content corrected by corrigenda COR-8..COR-14.**

- **A-1 (P2)** census TU/function counts wrong in four places: sluice_core compiles **13** top-level TUs (the frozen "15" had erroneously included the 2 uncompiled experimental TUs); sluice_async compiles **31** TUs including the 3 test seams (not 26); `scheduler*.cpp` = 10 files = 9 production + 1 seam (not 7); fail-fast catalog = **37** functions (not 31). Auditor re-ran the counts (`git ls-files` = 46 = 13+31+2; `rg -c` on fail_fast.cpp = 37). → COR-8.
- **A-2 (P2)** `~ApplicationRuntime` never calls `close_resources()`; it fail-fasts unless state ∈ {Constructed, StartFailed, Stopped} and joins the driver thread. `close_resources()` fires from `join()`/`shutdown()`/`start()`-failure only. Teardown order as frozen is correct; the trigger was not. → COR-9.
- **A-3 (P2)** FZ-10's Group threaded-path row conflated the paths: the threaded path spawns `std::thread`s running caller functions directly (no scheduler, no `run_live`); `run_live(1)` is the evented path on the awaiting caller's thread. Zero-consumer status stands. → COR-11.
- **A-4 (P2)** `blocking::file` consumers = sluice-tail + 7 test binaries, not "sluice-tail only". → COR-12.
- **A-5 (P2)** `File(int, FileAccess)` is **private** — an internal detail of `File::open`, not a reachable interop entry; Figure B's interop edge was wrong. The public interop boundary is `NativeFileRef(int, FileAccess)` alone. → COR-10.
- **A-6 (P2)** systematic false "tests" consumer entries: no test includes `scheduler.hpp`/`group.hpp` or uses `CancelToken`/`RequestHandle`/`WaitPolicy` — tests reach that machinery only indirectly via `ApplicationRuntime`. Material because the consumer sweep is this audit's load-bearing method. → COR-12.
- **A-7 (P3)** `Completion` and `Future`/`TaskResult` are header-only; FZ-2 listed non-existent TUs. → COR-13.
- **A-8 (P3)** `AsyncIoContext` consumers = all four apps via runtime (tail was omitted). → COR-12.
- **A-9 (P3)** no "default backend" exists: the ctor requires a backend, `RuntimeBuilder` rejects a missing one, and all four apps construct `ThreadPoolBackend` directly. → COR-12.
- **A-10 (P3)** `detail/io_validation` is also consumed by legacy `src/file.cpp`. → COR-12.
- **A-11 (P3)** SUS-14's "(asserts only)" overstated: there are no dynamic checks tying the timer heap to `earliest_active_deadline_` at all (only type-trait `static_assert`s). Substance stands. → COR-14.
- **A-12 (P3)** RC-2's creation site: `NativeFileRef` values are created by callers at op construction; `AsyncIoContext` only consumes them at the gate. → COR-10.

Positively confirmed by A (its verification summary): tracked files 231; LOC ≈33,146; untracked empty `fdg0_unowned/`; production dirs byte-identical `253fabe7..7f5a3f59`; experimental TUs unbacked; SUS-5/6/9/10/11/17 as frozen; `Scheduler::run()`/`run_until_idle()` zero callers; access-legality sites and sync-ungated facts; await-layer precedence order; arena slot FSM states; Completion FSM + `reap_seq`; private `submit_*` + friend; `kParkBackstop` 2 ms / `kTestParkPoll` 1 ms; `UringWaitSource` = eventfd + poll(2); `IORING_FSYNC_DATASYNC` split; `CONTROL_TAG` bit 63 + `prep_cancel64`; `peek_batch` 32; mw_s2 lowest-worker-id election; `reserve()`/`capacity_rejections_`; threadpool overflow→terminate; `BlockingIoPool` cv workers; close order group→sched→io_ctx; and all four app usage claims with file:line evidence.

## 31. Review B — ADR-diff attack

**Verdict: FAIL (draft) — 5 findings (0×P1, 3×P2, 2×P3). All accepted after re-verification; report amended as follows.**

- **B-1 (P2)** DC-10 mischaracterized the explicit-surface zero-length path as a pre-I/O short-circuit. Verified: `submit_transaction.hpp:30-39` reserves the slot before `validate`, and zero-length ops execute a real (no-data) lowering. **Amended**: DC-10 rewritten (mechanism per surface; capacity-consumption consequence routed to DC-13).
- **B-2 (P2)** DC-13 overstated precedence uniformity: at the explicit surface, `reserve()` (saturation → `would_block`; admission-closed → `invalid_state`) preempts rules 3/4, diverging from blocking/evented surfaces in the saturated regime. Verified: `request_arena.hpp:126-134` + submit-transaction order. **Amended**: DC-13 downgraded MATCH → **AMBIGUOUS_AUTHORITY** (strict reading CODE_DRIFT), F-6 annotated (saturated regime untested), gap C4 added, PLAN-9 item added.
- **B-3 (P2)** CT-22 was never really diffed; the evented surface forces per-op caller-owned `Completion` and leaves it outstanding on wait-layer error. Verified: `async/file.hpp` signatures, `await_op_helpers.cpp:5-12`, app usage. **Amended**: new **DC-29 AMBIGUOUS_AUTHORITY**, gap C5, PLAN-9 item.
- **B-4 (P3)** composition semantics had zero coverage and the two families diverge on premature EOF (`read_all` → `eof` error vs `await_read_fill` → partial success; write-exit codes `invalid_state` vs `backend_error`). Verified in both sources. **Amended**: new **DC-30 AMBIGUOUS_AUTHORITY**, gap C6, PLAN-9 item.
- **B-5 (P3)** count/uniformity errors: sync-admission tests = 4 (not 5); precedence tests = 40 (not 11 — auditor re-counted the NamedTest table: exactly 40); the null-buffer+len guard is backend-only (blocking reaches EFAULT). **Amended**: counts fixed in DC-11/F-6/F-7; null-buffer asymmetry recorded as an observation in §19.1, not as uniformity evidence.

Verdicts that survived B's attack unchanged: DC-01..DC-09 (minus DC-10's mechanism text), DC-11..DC-19 (minus DC-13's verdict), DC-20..DC-28, the §18 contract rows (CT-01..CT-35 faithful to the ADR), gap classes (no misclassification beyond the amendments above).

## 32. Review C — formalization attack

**Verdict: FAIL (draft) — 8 findings (1×P1, 4×P2, 3×P3). All accepted after re-verification; report amended as follows.**

- **C-1 (P1)** the assumption "backstop bounds park even without wake" is false in code: Scheduler-domain park is **unbounded** when no deadline and no bounded backend observation (`scheduler_park_wake.cpp:258-296`); the 1 ms figure is test-clock-only behind `SLUICE_ASYNC_INTERNAL_TESTING`. An always-on backstop in MC-1 would make the lost-wake mutant unkillable (vacuous model). **Amended**: COR-5; MC-1 scoped to two park variants with the unbounded variant satisfying liveness without a timeout action; test-clock row removed from production assumptions.
- **C-2 (P2)** "split-wait admitted as E1 sub-claim" had no card, no matrix row, and a wake mechanism (`interrupt_backend_waiters`) absent from MC-1's entities. **Amended**: MC-1 extended with the Backend park domain; F-10 added.
- **C-3 (P2)** the uring cancel deferred-terminal protocol (op-CQE vs control-CQE; `route.deferred_terminal_stored`) is admission-passing and was never triaged (distinct from the rejected poison-and-recover row). **Amended**: MC-5 + E6 + F-11 admitted; PLAN-4b added.
- **C-4 (P2)** MC-2 proved only the arena half of the SUS-13 three-object split; the `Completion` CAS lifecycle (three lock domains: `access_mtx_` claims, arena-mutex publish, lock-free caller reset/dtor) was absent. **Amended**: MC-2 extended to model the Completion state machine with cross-lock transitions.
- **C-5 (P2)** missing assumption rows mandated by the table's own purpose: memory-ordering discipline, wake-producer completeness, idle-dance discipline. **Amended**: three rows added with witnesses.
- **C-6 (P3)** three witness rows mischaracterized the cited code (ring guard is push-side; stale paths are graceful via `validate_` not fail-fast; `control_epoch_` governs re-entry, not the in-run stop predicate). **Amended**: rows reworded; the exactly-once-consumption non-check recorded as a candidate B-class assert.
- **C-7 (P3)** internal contradictions: "admitted only if passes all tests" vs ✗-T6 rows; dangling "gap G-8" reference. **Amended**: T6 restated as a priced bootstrap cost, not a gate; G-8 replaced by the L3-gap anchor (§25).
- **C-8 (P3)** RC-3/FZ-9 claimed `access_mtx_` serializes legality checks; they run outside the lock (benign: pure function of by-value fields). **Amended**: COR-4; F-6 stays at tests with the adjudication note.

Positively confirmed by C: timer CAS states correctly not modeled (all under `global_mtx_`, T4 fails); §24/§25 census and pyramid claims accurate (nothing defines `SLUICE_ASYNC_INTERNAL_TESTING`; no fault-injection/stress tests; the single multi-thread test is the kernel shared-offset check).

## 33. Review D — lifetime/UAF attack

**Verdict: FAIL (draft) — 6 findings (0×P1, 3×P2, 3×P3); no use-after-free constructible in any supported topology. All accepted after re-verification; report amended as follows.**

D hunted UAF/dangle interleavings on seven assigned surfaces (fiber stack vs scheduler references; Completion vs backend writes; arena vs slot users; WaitRecord lease/late-wake; external-wake Control; teardown barrier threads; fallback wait maps) and refuted each in supported topologies — the refutations themselves became the safety evidence recorded in the cards.

- **D-1 (P2)** arena ownership inverted in RC-3/RC-6/FZ-2/FZ-7/FZ-11: `RequestArena arena_` is a **backend member**; AsyncIoContext holds only `backend_`/`stats_`/`access_mtx_`. Backend destruction is safe by construction (worker joins in dtor body; arena first-declared = destroyed last). **Amended**: COR-2; the "arena borrow" hazard voided.
- **D-2 (P2)** SUS-15 over-graded: the Control race is closed by construction (notify holds `Control::mtx` across check **and** use; `~Scheduler` clears under the same mutex in the dtor body before member destruction; no inversion). **Amended**: §19.1 re-grade; §1 billing corrected; PLAN-5 downgraded to a regression test.
- **D-3 (P2)** SUS-4/PLAN-3/B3 aimed at the wrong object: the waiting maps are **Scheduler** members under `global_mtx_` (`scheduler.hpp:534-539`). **Amended**: COR-3; PLAN-3 re-targeted to `~Scheduler`; grading stays S3 (checked-invariant gap, not UAF — live entries require a suspended fiber, unreachable in supported topologies).
- **D-4 (P3)** FZ-11 misattributed the join mechanism: `~Scheduler` joins nothing; workers join in `run_impl`, the driver in `ApplicationRuntime::join` before `close_resources`. **Amended**: COR-6; MC-4 anchors extended (run_impl joins, backend dtor joins).
- **D-5 (P3)** RC-9/FZ-11 overstated the Group gate: future-ready publishes **before** stack departure; the operative guarantee is the run-join. Latent only for unsupported concurrent-Group topologies. **Amended**: COR-7; MC-4 mutation list extended.
- **D-6 (P3)** SUS-3 incomplete: worst variant is fd **recycling** (silent corruption), and `waiting_ready_` keys are raw `const std::atomic<bool>*` (same caller-contract class). **Amended**: §19.1 + PLAN-6 extended.

Safety facts D established and the report now carries: resume-mid-suspend closed by `suspend_switch_pending` under `global_mtx_`; late WaitRecord wakes closed by identity+generation+state validation under `wait_registry_mtx_`; Completion never written by backends (publish only in `reap` under arena mutex; reset CAS ordering); uring cancel-vs-terminal serialized by `dispatch_mtx_`; grep's cancel path never destroys an outstanding Completion.

## 34. Final verdict / stop state

**Verdict: `READY_FOR_HUMAN_REVIEW`.**

Architecture conformance: **SUBSTANTIALLY_CONFORMANT** to ADR-0001/ADR-0002 — 30 DIFF cards of which 24 MATCH, 1 UNIMPLEMENTED_CONTRACT (low), 3 AMBIGUOUS_AUTHORITY awaiting adjudication (DC-13 capacity-vs-precedence, DC-29 await-regime, DC-30 composition-EOF), 2 EXTRA_MECHANISM clusters (DC-22 unbacked experimental surface; DC-27 dead sync-primitive family + kin), 0 CODE_DRIFT upheld, 0 DOC_DRIFT, 0 P0 correctness defects, 0 use-after-free constructible in supported topologies (review D's refutations recorded as safety evidence).

Formal posture: **no formal assets exist** (§24); 5 model candidates admitted with scoped cards and mutation defenses (MC-1..MC-5), 3 claims correctly routed to cheaper evidence; evidence pyramid tops out at L2 with an L3 harness gap.

Audit-integrity accounting: CODE_REALITY_FREEZE hashed (`2c4e43a3…`) and embedded verbatim; 14 corrigenda (COR-1..COR-14) record every correction the four adversarial reviews forced — all re-verified by the auditor before acceptance; the frozen text itself was never edited.

Prohibitions honored (read-only audit): no production/test/build/ADR files changed; no bugs fixed (findings frozen as facts); no implementation issues opened (7 PROPOSED issue texts only, §28); no A–P implementation slices started; no performance campaign started; the Draft PR is not merged.

Stop-condition checklist at commit time:

| Condition | State |
|---|---|
| production diff (src/ include/ xmake/ apps/) | **0** |
| test diff (tests/) | **0** |
| build diff (xmake*) | **0** |
| ADR diff (docs/adr/) | **0** |
| report complete §1–§34 | yes |
| only added file | `docs/audit/code-reality-audit.md` |
| branch | `audit/code-reality-architecture-1` |
| commit message | `docs: add code-first architecture reality audit` |
| Draft PR | OPEN, not merged, body states READ-ONLY CODE AUDIT / DOCS-ONLY OUTPUT / NO IMPLEMENTATION CHANGES |
| worktree | clean |
| full report also submitted as | GitHub issue (verbatim, multi-comment if over body limit) |

Human hand-off: the three AMBIGUOUS_AUTHORITY adjudications (PLAN-9), the experimental-surface decision (PLAN-1), and the disposition round (PLAN-2) are the decisions this audit defers to its owner; everything else is execution-ready once accepted.
