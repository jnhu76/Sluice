# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/2.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
(tag-only: release versions are annotated git tags on `master`; pre-tag work
accumulates under `[Unreleased]`).

> **Release-note workflow:** draft the section for a release from the commit
> history and curate it by hand. Entries are **curated for humans, not dumped
> from git** — review, merge duplicates, and reword the draft before
> committing it. The previous record (v0.1.0 era) and the retired
> `scripts/changelog/generate.py` drafting helper live in Git history.

## [Unreleased]

## [0.0.2] - 2026-10-19

**v2 re-architecture (breaking).** This is the first release line on the
v2 Architecture and Contract (`docs/explicit-io-contract.md`, revision
v2-r1). Owner decision D3 (root §23.4) authorizes these breaking changes
with **no v1 compatibility obligations**: there is no compatibility layer,
forwarding wrapper, type alias, or dual public API for v1 users. Consumers
of the retired surfaces may exist outside this repository; this repository
cannot enumerate them. Pin a v1-era tag (for example `v0.0.1`) or migrate
per the guidance below. This disclosure is a D3 release-documentation
obligation, not a rollback lever.

### Added

- **Direct file I/O with no runtime.** `sluice/file_resource.hpp` owns the
  native resource (`File`, open flags, `FileInfo`, identity comparison);
  `sluice/blocking/file.hpp` provides the completed-return blocking
  operations — explicit-offset `read_at`/`write_at`, current-offset
  `read`/`write`, `sync_data`/`sync_all`, `file_info`, `size`, `resize` —
  and the short-I/O compositions `read_exact*`/`write_all*`. Direct execution
  needs no context, worker or request table, and `sluice_core` links and
  runs standalone.

- **Request execution with public identity.** An `AsyncIoContext` accepts
  six operations per outstanding request (`submit_read`, `submit_write`,
  `submit_sync_data`, `submit_sync_all`, `submit_file_info`,
  `submit_size`) and returns a move-only `Request<T>` carrying a public
  `RequestId` (context identity, slot, generation). Publication,
  observation and consumption of a result are separate events:
  `ready()` and the non-consuming `try_result()` never consume,
  `take_result()` consumes once, and `discard()` releases the public
  binding; slot reclamation follows its own borrow-retirement rules.
  Observer, progress and shutdown vocabulary are public enums and types
  on the installed headers — no `detail::` vocabulary is needed to use
  them.

- **Explicit backend selection.** Contexts are created only through the
  shipped factories `make_threadpool_context(ThreadPoolConfig)` and
  `make_uring_context(UringConfig)`; the io_uring factory reports an
  explicit `not_supported` outcome where the build has no liburing.
  Backend selection stays an explicit caller choice; user-authored
  backends are not a supported configuration.

- **Bounded progress and external event loops.** `poll_progress()` returns
  a `ProgressPass`: a bounded pass reporting completed work, whether
  accepted work remains, and honest owed-delivery residue. A context has
  one fixed progress owner (`claim_progress_owner()`), and each backend
  exposes a pollable progress-notification handle that an external
  `poll(2)` loop can integrate — the handle is never a second drain
  authority.

- **Request observers.** Per-request terminal-notification observers
  (`attach_observer`) with generation-armed at-most-once delivery,
  bounded non-throwing hooks, observer cancellation, and a public
  `delivery_retired()` query; retirement happens at the delivery owner
  after the hook returns.

- **Shutdown with retained results.** `shutdown(ShutdownPolicy)` blocks,
  drains execution and reports a `ShutdownOutcome`. Unconsumed results
  are retained after execution closes — shutdown does not drop accepted
  work — and destroying the context requires every public `Request`
  binding to be gone.

- **`RequestScope` for bounded pipelines.** The owned-driver scope in
  `sluice/async/request_scope.hpp` reserves tracking before acceptance,
  submits through the context, holds the accepted requests, and settles
  every slot on normal, early-exit and exception paths.

### Changed

- **Four reference applications on the v2 surfaces.** `sluice-copy`,
  `sluice-tail`, `sluice-grep` and `sluice-hash` are retained and now
  build on the v2 execution forms (request path, `RequestScope`, or
  direct blocking I/O) with bounded work and explicit progress. CLI
  note: `--workers` remains only on `sluice-copy` (its thread-pool
  workers); `sluice-tail`, `sluice-grep` and `sluice-hash` reject the
  flag — they execute on the caller thread or an app-owned driver.

- **Request handles carry public identity; rebuild required.** `Request<T>`
  stores the public `RequestId` (context identity + slot + generation) and
  grew from 40 to 48 bytes with no added allocation. The ABI is not stable
  across this boundary: dependent programs must recompile against the v2
  headers.

- **Package shape and build profiles.** `sluice_core` (blocking default)
  and `sluice_async` (opt-in request runtime) install through an explicit
  header allowlist, and the liburing-enabled and liburing-free profiles
  are verified as separate package identities
  (`docs/evidence/v2-release-acceptance-20261017/`).

- **Tested environment range (first v2 release).** Validated on Linux
  x86_64 (Fedora 44, kernel 7.2.9) with GCC 16.2.1 (primary; also carries
  the TSan runs), clang 22.1.8 (ASan+UBSan runs), xmake 3.0.9 and system
  liburing 2.13, across debug/release builds, liburing-disabled and
  liburing-enabled configurations, ASan+UBSan and TSan sanitizer runs, the
  ThreadPool and io_uring backends, and the four reference applications.
  The full matrix with per-cell commands and exit codes is recorded in
  `docs/evidence/v2-release-acceptance-20261017/`; configurations outside
  that record are untested by this release.

- **Repository hygiene contraction** — stage-labeled test/target/mutant
  names renamed to behavior-describing names; closed-campaign verification
  scripts, the retired formal corpora and dated-only evidence records moved
  to Git history; the dead lefthook pre-push configuration and the closed
  M-R disposition guard workflow removed. No product behavior, public API
  or supported-surface change; details in
  `docs/evidence/repo-hygiene-contraction-20261017/`.

- **Changelog restructured** — the changelog now lives at the repository
  root (`CHANGELOG.md`) in Keep a Changelog 2.0.0 format, and the version
  history starts at the `v0.0.1` reference baseline. The pre-v0.0.1 record
  (the v0.1.0 era) moved to Git history with a Historical banner;
  `scripts/changelog/generate.py` (with `--self-test`) was added then and
  has since been retired.

### Removed

- **v1 synchronous core surface.** The v0.0.1-era headers
  `sluice/{file,reader,writer,buffer,buffered_readable,copy,copy_strategy,iovec,limit,observed,fault,sync,wal}.hpp`,
  `sluice/io_context.hpp` (`BlockingIoContext`),
  `sluice/memory_io_context.hpp`, `sluice/blocking_io_pool.hpp`, the
  sync/fake async backends and `sluice/experimental/uring_*.hpp` no
  longer exist. Use `File` plus `sluice/blocking/file.hpp` for direct I/O
  or the request path above; otherwise pin `v0.0.1`.

- **v1 cooperative async runtime.** `Scheduler`, the stackful `Fiber`
  await model, `Completion`/`RequestHandle`/`Batch`, the
  Event/Mutex/Semaphore/Condition/Queue/Group/Future/Select/wait
  families, `application_runtime`, and the timer/wait vocabulary are
  removed together with their exclusive consumers. Bounded pipelines use
  `RequestScope`; waiting is explicit progress ownership; otherwise pin
  `v0.0.1`.

- **Stackful host bundle (W-04) hard-removed (D1).** The Stackful I/O Host,
  fiber context switching and the cooperative Cancel kit — the installed
  headers `sluice/async/{stackful_io_host,fiber,fiber_ctx,cancel}.hpp`,
  their implementation, and their exclusive tests and consumers — are
  physically deleted. Nothing of the bundle is retained in-tree, installable
  or shipped. A host requires a fresh root amendment (§16), not resurrection
  of ADR-0003. Migration: bounded-scope work stays on W-02
  (`RequestScope`) with explicit caller-owned execution; dependents that
  need the host itself must pin a v1 tag.

- **Install surface contracted 25 → 11 headers (D2).** The eleven installed
  public headers are `sluice/{effect,error,file_resource,measurement,result}.hpp`,
  `sluice/blocking/file.hpp`, and
  `sluice/async/{async_io_context,request,request_scope,threadpool_backend,uring_backend}.hpp`
  (the 25-header figure is the v2 campaign's pre-contraction interim
  baseline; v0.0.1 shipped the earlier, larger v1 surface). Everything else
  that installed at that interim is internalized or deleted and no longer
  ships: the seven `sluice/async/detail/*` seam headers
  (`request_key`, `ready_sink`, `reference_ready_sink`, `observer_protocol`,
  `fail_fast`, `request_core`, `progress_source`), the three
  `sluice/detail/*` core headers (`file_semantics`, `posix_retry`,
  `uring_submit`), and the four host headers above. Programs including any
  of them must move to the public API or pin a v1 tag.

- **Public backend-extension seam closed (BACKEND-01).** `AsyncBackend` is
  no longer a public extension protocol: the backend classes, the seam, and
  the test hooks are internal and uninstalled; `AsyncIoContext` can no
  longer be constructed from an injected backend object. Backend selection
  is explicit through the shipped factories only:
  `make_threadpool_context(ThreadPoolConfig)` and
  `make_uring_context(UringConfig)` — the latter reports an explicit
  `not_supported` outcome where liburing is unavailable. Shipping a third
  backend against the public surface is not supported.

## [0.0.1] - 2026-08-26

`v0.0.1` is the explicit-I/O reference baseline (`a38df5e`), frozen as the
pre-six-domain-refactor product surface. It accumulates everything since the
v0.1.0-era record: the complete synchronous core and async runtime, the
backend set, the application track, the post-freeze correctness work, and
the documentation reorientation. Full detail per area: the archived
changelog record in Git history.

### Added

- **File-tools application track** — `sluice-copy` (safe output: temp file +
  atomic rename + directory durability), `sluice-tail` (bounded backward
  last-N scan + follow mode with SIGINT cancellation), `sluice-grep`
  (bounded streaming literal search), `sluice-hash` (bounded streaming
  SHA-256).
- **Backend close/drain/destruction semantics (C2e)** — production
  admission-close on `ThreadPoolBackend` and `FakeAsyncBackend`, the
  commit/accept admission transaction, and the shared close/drain
  conformance suite.
- **Group evented admission exception safety (P2-01)** — transactional
  Fiber/stack/Future bookkeeping reservation; allocation failure leaves no
  partial task record.

### Changed

- **Documentation information-architecture reorientation** — root
  `README.md` / `README.zh-CN.md` rewritten for library users; `docs/README.md`
  became the contributor/agent hub; API reference canonicalized under
  `docs/reference/`; legacy namespaces moved to their semantics-owned homes
  under `docs/`; the E16 application-runtime design historicalized.
- **Post-freeze correctness work** — Phase C2e close/drain, publish/reap
  race closures, and the #227 baseline campaign (mechanical cleanup,
  release-status correction, tag-only version verdict).

### Fixed

- **Status drift and stale references** — README/ADR index no longer claim
  unimplementation that had landed; doc-path references in public headers,
  sources, benches, and tests repaired to the moved locations; the stale
  duplicate TLA+ spec document deleted.

### Security

- No security entries for this release.