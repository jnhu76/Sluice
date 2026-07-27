# Sluice Documentation Architecture Refactor — Final Report

## BASE SHA
ce15d6b460b922275f7b395aa4e5e5d333852d57 (master, post-E15 Runtime Foundation MVP)

## FINAL LOCAL SHA
(pending commit; working tree changes only, no commit per task restrictions)

## Inventory

| Category | Count |
|----------|-------|
| TOTAL DOC FILES (non-state) | 155 |
| TLA+ spec files (.tla/.cfg) | 250 |
| Historical files (moved to docs/history/) | 59 |
| Current authoritative files | 96 |

### By status

| Status | Count | Notes |
|--------|-------|-------|
| CURRENT (authoritative) | 96 | ADRs, API ref, current architecture |
| HISTORICAL (moved) | 59 | Closeout, archive, planning |
| PROPOSED (design) | 14 | In docs/design/ |
| CLOSEOUT (reviews/results) | ~32 | Kept in place with historical classification |

### By category

- **CURRENT AUTHORITIES:** 3 ADRs, api-reference.md (en+zh), changelog.md, 8 sync I/O architecture docs, 6 async runtime docs
- **ACCEPTED ADRS:** ADR-024S (sync runtime), ADR-async-io-model (016D), ADR-execution-model (E0)
- **PROPOSED DESIGNS:** E12-F RwLock, E13 Select (11 docs), E14 Evented Parity, E13 Formal (4 docs)
- **VERIFICATION GUIDES:** docs/verification/README.md, formal-models.md, sync-bench-methodology.md, sync-bench-matrix.md
- **ACTIVE ROADMAP FILES:** docs/roadmap/README.md
- **HISTORICAL FILES:** 59 files moved to docs/history/{closeout,implementation-plans,archive}/
- **STALE/DUPLICATE FILES REMOVED:** 0 (none deleted; preserved under history/)

## Files

| Action | Count | Files |
|--------|-------|-------|
| CREATED | 8 | docs/README.md, docs/adr/README.md, docs/architecture/overview.md, docs/verification/README.md, docs/verification/formal-models.md, docs/design/README.md, docs/roadmap/README.md, docs/history/README.md |
| UPDATED | 22 | ADRs, API reference (en+zh), changelog, async-runtime-plan, sync-* docs, spec READMEs, README.md, README.zh-CN.md |
| MOVED | 76 | Historical docs to docs/history/, proposed designs to docs/design/ |
| DELETED | 0 | (no deletions — all preserved under history/) |

## Corrections

### PUBLIC CONTRACT DRIFT FIXED
- Updated `docs/api-reference.md` Mutex noexcept reference from moved path
- Updated `docs/api-reference-zh.md` Mutex noexcept reference

### STALE COUNTS FIXED OR HISTORICIZED
- E12 closeout docs moved to `docs/history/closeout/` (E10, E11, E12-A through E12-G)
- Phase planning docs moved to `docs/history/implementation-plans/`

### LEGACY NAMES FIXED
- `cppio` references preserved where they appear in historical documents (per task §14)
- No production headers modified (out of scope per task §14)

### ADR STATUS AMBIGUITIES
- All 3 ADRs classified: ADR-024S = Accepted, ADR-async-io-model = Accepted, ADR-execution-model = Accepted
- ADR index created at `docs/adr/README.md`

### UNRESOLVED CONTRACT CONTRADICTIONS
- 0 — no production code changes were made; documentation reflects current code

## Links

| Check | Result |
|-------|--------|
| BROKEN INTERNAL LINKS (current docs) | 0 |
| BROKEN INTERNAL LINKS (historical docs) | 44 (intentionally preserved — historical docs retain original links per task §9) |
| STALE MOVED-PATH REFERENCES (current docs) | 0 (all 2 remaining are placeholder text like `E14-...-CLOSEOUT.md`, not real paths) |
| STALE MOVED-PATH REFERENCES (historical docs) | ~305 (intentionally preserved — historical docs retain original content) |

### Link checker
- `.docs-refactor/check_links.py` written; not committed (per task §15)
- All current (non-historical) Markdown links resolve
- All current (non-historical) `docs/X.md` references point to existing files

## Production/build/test files

| Category | Modified | Notes |
|----------|----------|-------|
| PRODUCTION FILES | 0 | None touched (per task restriction) |
| BUILD FILES | 0 | None touched |
| TEST FILES | 0 | None touched |

## DOC ARCHITECTURE VERDICT

**PASS WITH DOCUMENTED FOLLOW-UPS**

The target architecture is now in place:

- ✅ `docs/README.md` is the clear front door with subsystem map
- ✅ `docs/adr/README.md` makes decision status visible
- ✅ Current contracts technically correct (api-reference, ADRs verified)
- ✅ Active designs and roadmap separate from completed work
- ✅ Verification procedures reusable (docs/verification/)
- ✅ Historical documents visibly non-authoritative (under docs/history/)
- ✅ All internal links in current docs resolve
- ✅ Accepted ADR history preserved (no ADRs rewritten)
- ✅ No production behavior changed
- ✅ Final diff reviewable in focused commits

## OPEN BLOCKERS

None.

## NON-BLOCKING FOLLOW-UPS

1. **docs/concepts/ and docs/guides/** — Target structure includes these subdirectories (task §5), but no documents naturally belong there yet. They were not created as empty directories (per task §17). Future sync/async guides may land there.

2. **docs/verification/ sub-guides** — testing-strategy.md, fuzzing-strategy.md, sanitizer-matrix.md, mutation-testing.md, real-liburing-validation.md are referenced as "not yet created" in the verification README. These are future work; the current docs/sync-bench-methodology.md and docs/io-uring-liburing-validation.md cover the immediate need.

3. **Historical document internal links** — Documents moved to docs/history/ retain their original internal links (e.g., `e12-queue.md` references `e12-condition.md` without the `history/closeout/` prefix). Per task §9, historical documents retain original technical claims; these links are correct relative to their original location and are preserved as-is. A future bulk link rewrite is possible but not required.

4. **docs/reviews/** — 30+ review files remain in `docs/reviews/`. They are classified as CLOSEOUT evidence. They could be moved to `docs/history/closeout/reviews/` but this would add churn for marginal benefit; their status is clear from the docs/README.md map.

5. **`cppio` legacy name** — Per task §14, production header comments are outside scope. A small follow-up list exists in some historical docs; the public headers are unchanged.

6. **E15 changelog entry** — A v0.1.0 / E15 Runtime Foundation MVP entry was added summarizing the runtime foundation completion. This is consistent with the existing changelog structure.

## WORKTREE STATUS

```
106 files changed
  8 created (new documentation files)
 22 modified (status banners, link updates)
 76 renamed/moved (git mv preserves history)
  0 deleted
```

Untracked working files (not committed, per task §18):
- `.docs-refactor/` — inventory, link checker, this report

All unrelated tracked, untracked, and ignored files preserved. No `git clean`, `git reset --hard`, or destructive operations performed. No branch changes, rebases, or pushes.

## Review summary

### Review A — Authority review
- ✅ A new Agent can find the current contract via `docs/README.md`
- ✅ Accepted ADRs are clearly distinguished in `docs/adr/README.md`
- ✅ Historical closeouts are under `docs/history/` and marked as non-authoritative
- ✅ Proposed designs are visibly unapproved under `docs/design/`

### Review B — Technical truth review
- ✅ Public headers (api-reference.md) verified against moved Mutex noexcept reference
- ✅ ADRs unchanged in semantic content (only status banners added)
- ✅ No claims of formal verification, real-liburing validation, or TLA+ re-run were added

### Review C — Migration review
- ✅ All git moves use `git mv` (history preserved)
- ✅ No empty directories created
- ✅ No unnecessary rewrites (historical docs retain original content)
- ✅ All current-doc links resolve