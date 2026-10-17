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

**v2 contract adoption (breaking).** Sluice v2 is the first release line on
the v2 Architecture and Contract (`docs/explicit-io-contract.md`, revision
v2-r1). Owner decision D3 (root §23.4) authorizes these breaking changes
with **no v1 compatibility obligations**: there is no compatibility layer,
forwarding wrapper, type alias, or dual public API for v1 users. Consumers
of the retired surfaces may exist outside this repository; this repository
cannot enumerate them. Pin a v1-era tag (for example `v0.0.1`) or migrate
per the guidance below. This disclosure is a D3 release-documentation
obligation, not a rollback lever.

### Removed

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
  `sluice/async/{async_io_context,request,request_scope,threadpool_backend,uring_backend}.hpp`.
  Everything else that previously installed is internalized or deleted and
  no longer ships: the seven `sluice/async/detail/*` seam headers
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

### Changed

- **Request handles carry public identity; rebuild required.** `Request<T>`
  stores the public `RequestId` (context identity + slot + generation) and
  grew from 40 to 48 bytes with no added allocation. The ABI is not stable
  across this boundary: dependent programs must recompile against the v2
  headers.
- **Observer and shutdown vocabulary is public.** Progress observation and
  shutdown outcome reporting use public enums on the installed headers
  instead of `detail::` vocabulary; consumers matching on the internal
  enumerators must rename.
- **Tested environment range (first v2 release).** Validated on Linux
  x86_64 (Fedora 44, kernel 7.2.9) with GCC 16.2.1, xmake 3.0.9 and system
  liburing 2.13, across debug/release builds, liburing-disabled and
  liburing-enabled configurations, ASan+UBSan and TSan sanitizer runs, the
  ThreadPool and io_uring backends, and the four reference applications.
  The full matrix with per-cell commands and exit codes is recorded in
  `docs/evidence/v2-release-acceptance-20261017/`; configurations outside
  that record are untested by this release.

### Changed

- **Repository hygiene contraction** — stage-labeled test/target/mutant
  names renamed to behavior-describing names; closed-campaign verification
  scripts, the retired formal corpora and dated-only evidence records moved
  to Git history; the dead lefthook pre-push configuration and the closed
  M-R disposition guard workflow removed. No product behavior, public API
  or supported-surface change; details in
  `docs/evidence/repo-hygiene-contraction-20261017/`.

### Fixed

- **TSan data race in the timer test-observation seam (#229, PR #231)** —
  `Scheduler::AsyncTestAccess::active_deadline_count()` read the
  `global_mtx_`-guarded `active_deadline_count_` counter unlocked, racing
  with production deadline registration/retire paths while a live
  coordinator fiber polled the seam
  (`timer_new_earlier_deadline_becomes_earliest`). The accessor now takes
  `global_mtx_` and returns a synchronized snapshot, following the
  `earliest_active_deadline` / `waiting_select_count` locked-seam precedent.
  Test-only internal-testing vocabulary; production behavior, layout, and
  public API unchanged. The compliance gate record for the repair lands
  with PR #231 (record in Git history).

### Changed

- **Changelog restructured** — the changelog now lives at the repository
  root (`CHANGELOG.md`) in Keep a Changelog 2.0.0 format, and the version
  history starts at the `v0.0.1` reference baseline. The pre-v0.0.1 record
  (the v0.1.0 era) moved to Git history with a Historical banner;
  `scripts/changelog/generate.py` (with `--self-test`) was added then and
  has since been retired.

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