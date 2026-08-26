# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/2.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
(tag-only: release versions are annotated git tags on `master`; pre-tag work
accumulates under `[Unreleased]`).

> **Release-note workflow:** draft the section for a release from the commit
> history with `python3 scripts/changelog/generate.py --base <prev-tag>
> --head <commit> --version X.Y.Z --date YYYY-MM-DD`. The script maps
> Conventional-Commits message prefixes to Keep a Changelog categories and
> prints a draft (or inserts it with `--write`). Entries are **curated for
> humans, not dumped from git** — review, merge duplicates, and reword the
> draft before committing it. The previous record (v0.1.0 era) is archived
> at `docs/history/archive/changelog-v0.1.0-era.md`.

## [Unreleased]

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
  with PR #231 (docs/architecture/).

### Changed

- **Changelog restructured** — the changelog now lives at the repository
  root (`CHANGELOG.md`) in Keep a Changelog 2.0.0 format, and the version
  history starts at the `v0.0.1` reference baseline. The pre-v0.0.1 record
  (the v0.1.0 era) moved to
  `docs/history/archive/changelog-v0.1.0-era.md` with a Historical banner.
  Added `scripts/changelog/generate.py` (with `--self-test`) to draft
  release sections from commit history.

## [0.0.1] - 2026-08-26

`v0.0.1` is the explicit-I/O reference baseline (`a38df5e`), frozen as the
pre-six-domain-refactor product surface. It accumulates everything since the
v0.1.0-era record: the complete synchronous core and async runtime, the
backend set, the application track, the post-freeze correctness work, and
the documentation reorientation. Full detail per area:
`docs/history/archive/changelog-v0.1.0-era.md`.

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