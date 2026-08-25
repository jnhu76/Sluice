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
  hard-referenced by a gate, script, test, or production comment (path pin),
  or that still carries a current open disposition with no other CURRENT home.
  Not current authority; not movable without the atomic pin set below.
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
| [`issue-110-dequeue-gate-generation-handshake.md`](issue-110-dequeue-gate-generation-handshake.md) | TEST_PROTOCOL_ROOT_CAUSE_FIXED | #110 CLOSED | **CLOSED-HISTORY** | The generation-scoped pause-gate handshake lives in `src/async/threadpool_test_seams.hpp` + the race tests; production impact was NONE | **Zero** exact-path or basename consumers (verified repo-wide). Code comments reference "Issue #110" (`threadpool_test_seams.hpp:11,63,80,118,371,543,602`, `threadpool_backend.hpp:182,209`) and resolve via the GitHub issue, not this path | `docs/history/issues/` | Fixed test-infrastructure defect; no current limitation is carried only by this file. Move is a pure `git mv` + historical banner; no other doc updates required. |
| [`issue-115-runnable-publication-wake.md`](issue-115-runnable-publication-wake.md) | RUNNABLE_WAKE_ROOT_CAUSE_FIXED | #115 CLOSED ("pre-existing, frozen") | **PINNED-EVIDENCE** | Repair is live in production (`spawn()`/`spawn_on()` advance the wake epoch; G1 refusal priority in `unguarded_progress_pending_locked`); gate record at `docs/architecture/issue-115-runnable-publication-wake-gate.md` | `docs/architecture/issue-115-runnable-publication-wake-gate.md:7`; `docs/post-freeze/post-freeze-final-report.md:54` | `docs/history/issues/` (MOVE-WITH-CONSUMERS, deferred) | **Classification blocker.** The disposition "deferred to application evidence" (this file §14; `issue-116-runtime-reentry-liveness.md` §13) and the #111 supersede indication have **no CURRENT home** — not in the gate doc, `docs/known-issues/`, or the roadmap. Moving would leave a current open disposition recorded only in history. Follow-up: give the disposition a CURRENT home before any move. |
| [`issue-116-runtime-reentry-liveness.md`](issue-116-runtime-reentry-liveness.md) | ROOT_CAUSE_PROVEN_AND_FIXED | #116 CLOSED | **PINNED-EVIDENCE** | Repair is live in production (`ApplicationRuntime::driver_main()` re-enters `run_live` while `outstanding() > 0`); gate record at `docs/architecture/issue-116-reentry-liveness-gate.md` | **`scripts/gates/mechanical-facts.py:462` (`TEST_TOTAL_EXTRA_DOCS`) — hard script pin**: the file's `test:default-gate-targets` row is mechanically validated. Also `issue-116-reentry-liveness-gate.md:35,238`; `tests/issue116_interrupt_reevaluation_regression_test.cpp:3`; `xmake/tests/async_internal.lua:40` | `docs/history/issues/` (MOVE-WITH-CONSUMERS, deferred) | Mechanical gate hard-codes the path. A move must atomically update `mechanical-facts.py` `TEST_TOTAL_EXTRA_DOCS` and keep the doc's test-total claim valid (currently 190). |
| [`issue-123-phase-g-closeout-parallel-flake.md`](issue-123-phase-g-closeout-parallel-flake.md) | ROOT_CAUSE_PROVEN_AND_FIXED (test-methodology false failure) | #123 CLOSED | **CLOSED-HISTORY** | The blocking-handshake methodology is current practice in `tests/phase_g_closeout_test.cpp` / `phase_g_closeout_uring_test.cpp` (issue #123 comments); the phase-g design's methodology note (`docs/design/phase-g-backend-progress-wake.md` line 12–19) records the migration | `docs/design/phase-g-backend-progress-wake.md:19`; `docs/investigations/issue-116-runtime-reentry-liveness.md:370` (both docs-only). Code comments reference "issue #123" (`tests/phase_g_closeout_test.cpp`, `phase_g_closeout_uring_test.cpp`, `src/async/scheduler_test_access.hpp:98`) and resolve via the GitHub issue, not this path | `docs/history/issues/` | Fixed test-methodology flake; no current limitation carried only here. Move needs the two docs-only pointer updates above (both in files that stay in place). |

## LIVE investigations

None. All four files above are completed investigations; none is an active
work surface.

## Move slices (for the future Step 5 archive pass)

- **MOVE-NOW** (zero or docs-only consumers; current facts have CURRENT homes):
  - `issue-110-dequeue-gate-generation-handshake.md` → `docs/history/issues/`
  - `issue-123-phase-g-closeout-parallel-flake.md` → `docs/history/issues/`
    (atomic pointer updates: `docs/design/phase-g-backend-progress-wake.md:19`,
    `docs/investigations/issue-116-runtime-reentry-liveness.md:370`)
- **MOVE-WITH-CONSUMERS** (deferred; pin sets recorded above):
  - `issue-115-runnable-publication-wake.md` — **blocked** until the
    deferred-disposition gets a CURRENT home; then update
    `issue-115-runnable-publication-wake-gate.md:7` and
    `docs/post-freeze/post-freeze-final-report.md:54`
  - `issue-116-runtime-reentry-liveness.md` — requires an atomic
    `scripts/gates/mechanical-facts.py` update plus the gate/test/lua
    comment updates listed above
- **KEEP-LIVE**: none.

## Navigation

- **Current architecture** — `docs/architecture/` (classification index:
  `docs/architecture/README.md`)
- **Historical issue records** — `docs/history/issues/`
- **Design documents** — `docs/design/README.md` (phase-record classification:
  see that index)
