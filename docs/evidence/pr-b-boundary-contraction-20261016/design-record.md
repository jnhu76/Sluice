# PR B Boundary Contraction Design Record

| Field | Value |
|---|---|
| Authority | `docs/explicit-io-contract.md` v2-r1 (D1/D2/D3/D6, §16, Appendix B); `docs/roadmap/v2-conformance.md` G9/RG2/RG3 |
| BASE_SHA | `5c1b14db328f3588ddb5a275d264eb01aac7e188` (PR #544 merge; clean worktree) |
| Baseline toolchain | g++ 16.2.1 (Red Hat), clang 22.1.8, xmake v3.0.9, Linux 7.2.9 fc44 x86_64, liburing present (`/usr/include/liburing.h`, `liburing.so.2`) — both profiles buildable |
| Baseline install surface | 25 installed headers (26 in tree; `context_identity.hpp` already internal) |
| Baseline archives | `sluice_async`: 13 members (async_io_context, cancel, context_identity, fail_fast, fiber, fiber_ctx, request_core, request_scope, stackful_io_host, threadpool_backend, uring_backend, uring_completion, uring_transport); `sluice_core`: 2 (blocking_file, file_resource) |

## 1. Dependency/ownership DAG (facts established at BASE_SHA)

### DAG A — header closure of the installed surface

```
async_io_context.hpp ──> detail/observer_protocol.hpp, detail/progress_source.hpp,
  │                        detail/ready_sink.hpp, detail/request_key.hpp,
  │                        async/request.hpp, error.hpp, file_resource.hpp,
  │                        measurement.hpp, result.hpp
request.hpp ──> detail/fail_fast.hpp, detail/request_core.hpp, error.hpp, result.hpp
threadpool_backend.hpp ──> async_io_context.hpp, detail/reference_ready_sink.hpp,
  │                        detail/request_key.hpp, detail/posix_retry.hpp, error.hpp, result.hpp
uring_backend.hpp ──> async_io_context.hpp, detail/reference_ready_sink.hpp,
                      detail/request_core.hpp, detail/uring_submit.hpp, error.hpp, result.hpp
                      (class layout #if-guarded on SLUICE_HAS_LIBURING; under
                       SLUICE_ASYNC_INTERNAL_TESTING&&liburing it further includes the
                       src-only "uring_test_seams.hpp")
request_scope.hpp ──> async_io_context.hpp, request.hpp, error.hpp, result.hpp
blocking/file.hpp, effect/error/file_resource/measurement/result.hpp ──> std only
```

Public-type leaks through this closure: `AsyncBackend` (public abstract seam class),
`detail::SynchronousReadySink`/`ReadyEvent` (via `set_ready_sink` + transitively reachable
subclassing), `detail::RequestKey`/`ObserverRegistration`/`ObserverCancellation` (observer
API), `detail::RequestCore` (inline `Request<T>` operations), `detail::ProgressSource`
(context test seams + nested `Token`/`PauseGate`), `sluice::detail::IoOutcome` (via
`request.hpp`), macro-sensitive `UringAsyncBackend` layout.

### DAG B — construction/lifetime/teardown (owners)

Named backend construction today: caller `make_unique<ThreadPoolBackend|UringAsyncBackend>`
→ `AsyncIoContext(std::unique_ptr<AsyncBackend>, AsyncStats*)` (public ctor):
`progress_` = `make_unique<ProgressSource>` (eventfd; throws on failure) → backend
`attach_stats` + `attach_progress_port(BackendProgressPort{progress_})` (uring installs
eventfd registration; failure throws) → `allocate_context_identity()` → core =
`make_unique<RequestCore>(identity, backend->slot_capacity())` → `backend_->core_` set.
Member declaration order core_, progress_, backend_ gives destruction order backend →
progress → core on every unwind path. Request<T> holds raw `core_`/`backend_` pointers +
`detail::RequestKey`; publication handoff through the core mutex; progress epochs under
`ProgressSource::mtx_`; admission/drive serialized by `access_mtx_`. Move ctor/assign move
members under source lock, fail-fast on claimed owner handle. Destructor: lock →
fail-fast live progress binding → fail-fast public bindings → close admission →
`drive_settlement_` (must complete, else fail-fast). All of these owners are preserved
unchanged by the contraction; only their *spelling locations* move.

### DAG C — consumer ownership (full census in PR notes; recomputed at BASE_SHA)

- `AsyncBackend` subclassers: 8 internal test backends (Scripted/Throwing/Counting×2/
  Recording/Null×2/DtorSignal/UnwindSignal) + `ThreadPoolBackend`/`UringAsyncBackend`.
  No app subclasses or constructs `AsyncBackend` except sluice-copy's injection parameter
  (`run_*_with_backend(std::unique_ptr<AsyncBackend>)`).
- Host bundle consumers (recomputed, not assumed): `tests/stackful_host_test.cpp`,
  `tests/package/consumers/w04_stackful_candidate.cpp`,
  `tests/package/f4_retirement/probes/retained.cpp` (already stale — includes retired
  headers). No app, no bench, no retained test depends on the bundle; the old claim
  "host consumers = host tests only" was incomplete at the package layer.
- Package consumers construct public backend objects in 13 files; only
  `contract_observer.cpp` uses `detail::` names (custom sink + observer enums).
- `SLUICE_ASYNC_INTERNAL_TESTING` defines: seam target families in `xmake/tests.lua`
  only; apps and bench never define it.

### DAG D — build/package/formal

Install globs `libraries.lua:15-17,44-46` ship 25 headers. Seam test families compile
their own production TUs (never link `sluice_async`). Mutation controls: 72 targets
mapping 60+ `SLUICE_*_MUTANT_*` defines to seams (13 stackful-host mutants die with the
bundle). Package authority today: `scripts/verify_f1_package.py` + frozen 79-header-era
manifests in `docs/review/` (stale against the 25-header tree; never used as v2 proof).
Four living models in `formal/tla/` with correspondence recorded in
`docs/evidence/f6-final-correspondence-20261014/`.

## 2. Headers leaving the install set (exactly 14)

Deleted outright (D1): `sluice/async/cancel.hpp`, `fiber.hpp`, `fiber_ctx.hpp`,
`stackful_io_host.hpp`.
Internalized (remain in tree, uninstalled; library TUs and isolated seam tests include
them via `-I include`): `sluice/async/detail/{fail_fast, observer_protocol,
progress_source, ready_sink, reference_ready_sink, request_core, request_key}.hpp`,
`sluice/detail/{file_semantics, posix_retry, uring_submit}.hpp`.

## 3. Public type leaks and their C++ users — disposition

| Leak | Users | Disposition |
|---|---|---|
| `AsyncBackend` public class | context ctor seam, 8 test backends, 2 backend subclasses | Class definition moves to uninstalled `sluice/async/detail/async_backend.hpp`; public ctor removed; tests keep subclassing via internal header + `detail::make_context` |
| `ThreadPoolBackend`/`UringAsyncBackend` public classes + macro layout | apps/tests/bench/consumers construct | Class definitions move to uninstalled `detail/threadpool_backend.hpp`, `detail/uring_backend.hpp`; installed headers keep only `ThreadPoolConfig`/`UringConfig` + named factories |
| `detail::RequestCore` + `detail::IoOutcome` via `request.hpp` | inline `Request<T>` ops | Out-of-line non-template bridge functions declared in `request.hpp` (public vocabulary signatures only), defined in `src/async/detail/request_core.cpp`; B2 release-edge mutants move with the logic, same kill oracles |
| `detail::ProgressSource` nested types (context test seams) | progress race/loop tests | Removed from installed header; internal `detail/TestAccess` hooks header |
| `set_ready_sink(detail::SynchronousReadySink*)` | 2 internal tests + 1 package consumer | Removed from public API (no public callback framework under v2); internal `TestAccess` hook; package consumer keeps the OBS-01..03 oracle without the sink |
| observer `detail::*` enums/keys in `ObserverAttachment`/`ObserverCancelResult`/`retire_delivery` | 5 test files + 1 package consumer | Public `ObserverStatus`/`ObserverCancelStatus` enums + `RequestId`-based struct/method signatures; library maps 1:1 |

## 4. Retained consumer migration table (summary)

- Apps: sluice-copy switches to `make_threadpool_context` and takes an `AsyncIoContext`
  parameter instead of `std::unique_ptr<AsyncBackend>` at its test seam; other apps
  unaffected (core-only).
- Bench: `make_backend` factories become context factories via the named public
  factories (macro branch kept for choosing whether to run the uring leg).
- Production-lib-linked tests: construct via `make_threadpool_context` /
  `make_uring_context` (+`Result` unwrap); custom-backend tests additionally include
  `detail/async_backend.hpp` and use `detail::make_context`.
- Seam tests: same TU sets and defines; includes move to `detail/*backend.hpp`;
  `AsyncIoContext ctx(std::move(b))` → `detail::make_context(std::move(b), s)`;
  context seams via `detail::TestAccess::*`; `key_for_test()` → `TestAccess::request_key`.
- Package consumers: rewritten against the 11-header surface only (§6).

## 5. At-risk invariants and deterministic discriminators

| Invariant | Discriminator preserved |
|---|---|
| Accepted work never orphaned by post-accept failure (S7, REQ-02) | `request_core_protocol/publication/ownership`, cutover suites, `pipelined_copy_oracle_support` scripted backends |
| Publication only after handoff/retirement (S4, REQ-04) | `request_core_publication_test`, B2 observe/consume mutants |
| Nonterminal release fails fast (HANDLE-02) | `public_request_release_violation_test` + B2 release mutants (moving with the code, same oracles) |
| Cancel arbitration distinct (CANCEL-01) | e1 suites both backends, `RequestCore.tla` |
| Observer at-most-once/retirement (OBS) | cutover observer phases, `contract_observer` package oracle, `ObserverCore.tla` |
| No lost wake / bounded pass / honest residue (PROG) | progress race/loop/pass-bound/owed-residue suites (unchanged mechanism) |
| Context move/teardown order (SHUT) | `shutdown_lifecycle_test` 9 TP + 17 uring scenarios, `ShutdownCore.tla` |
| Public isolation (VERIFY-01 build row) | new v2 clean-room verifier: 11 headers independently compilable, negative probes for all 14 removed/internalized headers and the seam |
| Hot-path allocation-free Requests | `Request<T>` remains a value type; bridge calls are plain out-of-line calls; no per-request allocation added (context construction keeps today's allocation count) |

## 6. Chosen design (B1) and rejected alternatives

Chosen shape — "internal headers stay in tree, uninstalled; installed surface carries
only public vocabulary":

1. `AsyncIoContext` keeps its member layout and move/teardown semantics verbatim; the
   installed header drops all detail includes and gains only forward declarations
   (`AsyncBackend`, `detail::RequestCore`, `detail::ProgressSource`,
   `detail::make_context`, `detail::TestAccess`) — every member of incomplete type is a
   `unique_ptr` whose destroying operations are out-of-line, so no layout or teardown
   change occurs and no extra allocation is introduced.
2. Construction: private ctor + `friend detail::make_context(unique_ptr<AsyncBackend>,
   AsyncStats*)` (internal, always defined); public named factories
   `make_threadpool_context(ThreadPoolConfig, AsyncStats*) -> AsyncIoContext`
   (throws on invalid config/setup, exactly today's behavior) and
   `make_uring_context(UringConfig, AsyncStats*) -> Result<AsyncIoContext>`
   (`not_supported` when the liburing profile is absent; setup failures throw exactly as
   today). No registry, no vtable, no second dispatch authority; UringConfig becomes
   unconditional, removing all public macro layout sensitivity.
3. `Request<T>` stores `RequestId` + two opaque pointers; all core operations route
   through five non-template bridge functions declared with public-only signatures and
   defined in `request_core.cpp`; B2 release-edge mutant seams move with the logic.
4. Observer API re-typed onto public enums + `RequestId`; ready-sink installation and
   all context test seams move to `include/sluice/async/detail/test_access.hpp`
   (installed headers end with zero `SLUICE_ASYNC_INTERNAL_TESTING` blocks).
5. Install rules become a precise 11-header allowlist; internal detail headers remain
   in-tree under `include/sluice/**/detail/` for library TUs and isolated seam tests.

Rejected alternatives:
- PImpl `State` for the context: strictly more mechanism (extra allocation, move-
  semantics rewrite, shared internal state header) for the same boundary property; the
  incomplete-`unique_ptr`-member layout achieves the closure contraction with zero
  lifetime-code churn.
- Keeping `AsyncIoContext(unique_ptr<AsyncBackend>, ...)` public but uninstalled-detail:
  fails D2's "no public backend implementation seam" requirement de jure.
- Enum-dispatch single factory: weaker typing, unifies distinct configs, no smaller.
- Public ready-sink/callback framework: not required by v2 (OBS-01), would add public
  seam vocabulary the root explicitly does not require.
- Deleting internal detail headers from the tree: they are the production seam the
  shipped backends and mutation mirrors compile against; Appendix B keeps them in-tree.

## 7. Expected scope

Production: 4 installed headers rewritten (2 shrunk to selection API), 4 new internal
headers, 1 internal hooks header, `async_io_context.cpp`/`request_core.cpp`/
`threadpool_backend.cpp`/`uring_backend.cpp` updated, `libraries.lua` allowlist, host
bundle deleted (4 headers + 4 TUs + test + xmake block + fail-fast trims), sluice-copy
seam, bench factories. Tests: ~40 files migrated mechanically, oracles unchanged.
Package: new `scripts/verify_v2_package.py` + rewritten consumer set; F1 frozen records
preserved as history. Docs: ledger G9/RG2/RG3 updates.
