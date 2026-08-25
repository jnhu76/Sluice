# Investigations — Classification Index

This directory holds root-cause investigations and their repair records. The
docs/README.md directory map describes it as "What is being diagnosed right
now?"; in practice it now also holds **completed** investigations whose records
are still referenced by compliance gates, production comments, or mechanical
gates. This index separates the two so a reader can tell which investigation
is still an open work surface and which is a closed record awaiting archive
(issue #167 Step 5b classification, baseline `master@6d8ebf1`, 2026-08-25).

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

| File | Self-declared status | GitHub issue | Class | Current facts' CURRENT home | Pins / consumers | Move candidate | Blocker / rationale |
|------|----------------------|--------------|-------|-----------------------------|------------------|----------------|---------------------|
| [`issue-115-runnable-publication-wake.md`](issue-115-runnable-publication-wake.md) | RUNNABLE_WAKE_ROOT_CAUSE_FIXED | #115 CLOSED ("pre-existing, frozen") | **PINNED-EVIDENCE** | Repair is live in production (`spawn()`/`spawn_on()` advance the wake epoch; G1 refusal priority in `unguarded_progress_pending_locked`); gate record at `docs/architecture/issue-115-runnable-publication-wake-gate.md` | `docs/architecture/issue-115-runnable-publication-wake-gate.md:7`; `docs/post-freeze/post-freeze-final-report.md:54` | `docs/history/issues/` (MOVE-WITH-CONSUMERS, deferred) | **Classification blocker.** The disposition "deferred to application evidence" (this file §14; `issue-116-runtime-reentry-liveness.md` §13) and the #111 supersede indication have **no CURRENT home** — not in the gate doc, `docs/known-issues/`, or the roadmap. Moving would leave a current open disposition recorded only in history. Follow-up: give the disposition a CURRENT home before any move. |
| [`issue-116-runtime-reentry-liveness.md`](issue-116-runtime-reentry-liveness.md) | ROOT_CAUSE_PROVEN_AND_FIXED | #116 CLOSED | **PINNED-EVIDENCE** | Repair is live in production (`ApplicationRuntime::driver_main()` re-enters `run_live` while `outstanding() > 0`); gate record at `docs/architecture/issue-116-reentry-liveness-gate.md` | **`scripts/gates/mechanical-facts.py:462` (`TEST_TOTAL_EXTRA_DOCS`) — hard script pin**: the file's `test:default-gate-targets` row is mechanically validated. Also `issue-116-reentry-liveness-gate.md:35,238`; `tests/issue116_interrupt_reevaluation_regression_test.cpp:3`; `xmake/tests/async_internal.lua:40` | `docs/history/issues/` (MOVE-WITH-CONSUMERS, deferred) | Mechanical gate hard-codes the path. A move must atomically update `mechanical-facts.py` `TEST_TOTAL_EXTRA_DOCS` and keep the doc's test-total claim valid (currently 190). |

## LIVE investigations

None. The two files above (issue-115, issue-116) are completed investigations;
none is an active work surface. issue-110 and issue-123 were archived in Step
5c (2026-08-25) — see "Completed archive moves" below.

## Move slices (issue #167 Step 5)

- **MOVE-NOW** (executed 2026-08-25, Step 5c — see "Completed archive moves"):
  - `issue-110-dequeue-gate-generation-handshake.md` → `docs/history/issues/`
  - `issue-123-phase-g-closeout-parallel-flake.md` → `docs/history/issues/`
- **MOVE-WITH-CONSUMERS** (deferred; pin sets recorded above):
  - `issue-115-runnable-publication-wake.md` — **blocked** until the
    deferred-disposition gets a CURRENT home; then update
    `issue-115-runnable-publication-wake-gate.md:7` and
    `docs/post-freeze/post-freeze-final-report.md:54`
  - `issue-116-runtime-reentry-liveness.md` — requires an atomic
    `scripts/gates/mechanical-facts.py` update plus the gate/test/lua
    comment updates listed above
- **KEEP-LIVE**: none.

## Completed archive moves (2026-08-25, issue #167 Step 5c)

- [`issue-110-dequeue-gate-generation-handshake.md`](../history/issues/issue-110-dequeue-gate-generation-handshake.md)
  — zero path consumers; pure `git mv` + historical banner.
- [`issue-123-phase-g-closeout-parallel-flake.md`](../history/issues/issue-123-phase-g-closeout-parallel-flake.md)
  — relocatable consumers updated atomically:
  `docs/design/phase-g-backend-progress-wake.md:19`,
  `docs/investigations/issue-116-runtime-reentry-liveness.md:370`.
- Old paths registered in `KNOWN_MOVED` (`scripts/check-doc-links.py`); any new
  reference to them fails the docs gate.

## Navigation

- **Current architecture** — `docs/architecture/` (classification index:
  `docs/architecture/README.md`)
- **Historical issue records** — `docs/history/issues/`
- **Design documents** — `docs/design/README.md` (phase-record classification:
  see that index)
