# Investigations — Classification Index

This directory held the repository's root-cause investigations and their
repair records (the docs/README.md directory map describes it as "What is
being diagnosed right now?"). All four completed investigations were archived
under `docs/history/issues/` by issue #167 Step 5 (2026-08-25) — see
"Completed archive moves" below. The classification vocabulary from Step 5b
(baseline `master@6d8ebf1`) is preserved below for provenance.

## Classes

- **LIVE** — the issue is still being investigated; the file is the ongoing
  work surface.
- **PINNED-EVIDENCE** — a completed investigation whose record is still
  hard-referenced by a **mechanical pin** (a script, CI step, or operational
  verification anchor that hard-codes the path) or a **current code/verification
  authority** that explicitly depends on the location, or that still carries a
  current open disposition with no other CURRENT home. A relocatable prose or
  source/test comment link is **not** a pin. Not current authority; not movable
  without the atomic pin set below.
- **CLOSED-HISTORY** — the work is fixed/closed; current facts have a CURRENT
  home (architecture/ADR/reference/test); the file has provenance/forensic
  value only and is an archive-move candidate.

Classification rules: GitHub issue open/closed is NOT the decision.
`docs/history/` never carries new current authority. Forensic/provenance
content is never deleted. See the architecture index
(`docs/architecture/README.md`) for the same policy applied to
`docs/architecture/`.

## Inventory (issue #167 Step 5b)

The Step 5b classification table classified all four investigations. All four
are now archived (see "Completed archive moves"): issue-110 / issue-123
(CLOSED-HISTORY, Step 5c), issue-115 (PINNED-EVIDENCE → adjudicated
**superseded**, Step 5d), issue-116 (PINNED-EVIDENCE, Step 5e1 — the hard
script pin `TEST_TOTAL_EXTRA_DOCS` was updated atomically). The directory
currently holds no investigation files.

## LIVE investigations

None — no active investigation remains; all four completed investigations are
archived under `docs/history/issues/`.

## Move slices (issue #167 Step 5) — all executed

- issue-110, issue-123 (Step 5c) → `docs/history/issues/`
- issue-115 (Step 5d, deferred disposition adjudicated **superseded**) →
  `docs/history/issues/`
- issue-116 (Step 5e1, atomic `TEST_TOTAL_EXTRA_DOCS` plus gate/test/lua
  comment updates) → `docs/history/issues/`
- **KEEP-LIVE**: none.

## Completed archive moves (2026-08-25, issue #167 Step 5c / 5d / 5e1)

- [`issue-110-dequeue-gate-generation-handshake.md`](../history/issues/issue-110-dequeue-gate-generation-handshake.md)
  — zero path consumers; pure `git mv` + historical banner.
- [`issue-123-phase-g-closeout-parallel-flake.md`](../history/issues/issue-123-phase-g-closeout-parallel-flake.md)
  — relocatable consumers updated atomically:
  `docs/history/implementation-plans/phase-g-backend-progress-wake.md:19`,
  `docs/history/issues/issue-116-runtime-reentry-liveness.md:370`.
- [`issue-115-runnable-publication-wake.md`](../history/issues/issue-115-runnable-publication-wake.md)
  — deferred disposition adjudicated **superseded** (Phase D); relocatable
  consumers updated atomically: `issue-115-runnable-publication-wake-gate.md:7`,
  `docs/post-freeze/post-freeze-final-report.md:54`. The historical banner
  carries the adjudication.
- [`issue-116-runtime-reentry-liveness.md`](../history/issues/issue-116-runtime-reentry-liveness.md)
  — mechanical pin `TEST_TOTAL_EXTRA_DOCS` updated atomically in
  `scripts/gates/mechanical-facts.py`; consumers updated:
  `issue-116-reentry-liveness-gate.md:35,238`,
  `tests/issue116_interrupt_reevaluation_regression_test.cpp:3`,
  `xmake/tests/async_internal.lua:40`. Banner notes the stale §13 #115
  cross-reference (predates the #115 fix).
- Old paths registered in `KNOWN_MOVED` (`scripts/check-doc-links.py`); any new
  reference to them fails the docs gate.

## Navigation

- **Current architecture** — `docs/architecture/` (classification index:
  `docs/architecture/README.md`)
- **Historical issue records** — `docs/history/issues/`
- **Design documents** — `docs/design/README.md` (phase-record classification:
  see that index)
