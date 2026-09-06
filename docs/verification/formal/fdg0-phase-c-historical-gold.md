# FDG-0 Phase C — Historical Gold Corpus

> Owner doc for FDG-0 Phase C (issue #298, "Phase C only — Historical Gold
> Corpus"). Established 2026-09-06 at master `b6e24e67` (PR #303 merge),
> branch safety/fdg0-phase-c-historical-gold; C0 preregistration commit
> `704dc65b` (inventory + frozen gold) precedes C1 harness `01b71530`
> precedes C2 results. The gold file was not modified after C0.
>
> **Verdict (preregistered logic, task book §39):**
> **METHOD_RECALL_NOT_EARNED.** 3 of 12 non-AMBIGUOUS positive expected
> claims were missed (silent NO = 0; every miss occurred on a case that
> still failed closed UNKNOWN at case level), and one PRECISE facet route
> omitted a gold-required target (HEURISTIC-correlated). Per the permanent
> rule, the misses are the result; the resolver was not tuned after the
> gold freeze.

## What Phase C tested

The frozen method stack — Xmake Build Truth (#302) → SCIP structural
recovery (#300) → explicit claim anchors → Formal Facets (#303) — was run,
unmodified, against 23 real historical Sluice changes whose gold labels
were frozen from diffs/tests/issue history BEFORE any resolver execution
(commit C0 `704dc65b`). Every scored case materialized REAL base/head
worktrees from git; the build world, compile_commands, Build Manifest, and
SCIP graph were regenerated inside the historical HEAD worktree
(`SLUICE_FDGC_ANALYSIS_ROOT` seam; the authority artifacts stayed in the
current repository; the current graph was never reused — checksum-verified).

Corpus: 1290-commit candidate inventory (deterministic categories), 24
frozen cases = 23 scored (12 non-AMBIGUOUS positive across F01/F03/F04/
F06/F08, 2 AMBIGUOUS, 9 negative strata) + 1 reported OUT_OF_EPOCH
(C-024, 2026-07-02 pre-`sluice_async` world). 10 subsystems; history span
2026-07-07..2026-08-29. Frozen config: default xmake (with-liburing=false)
for every case — no variant shopping.

## Results (frozen corpus, 23 reconstructed / 23 required)

| Metric | File-only (A) | Explicit-only (B, depth 0) | Method C (Build+SCIP+Facets) |
|---|---|---|---|
| scored positive claim labels | 12 | 12 | 12 |
| claim recall | **83.33% (10/12)** | 41.67% (5/12) | 75.00% (9/12) |
| silent NO | 2 | 1 | **0** |
| expected claim misses | 2 | 7 | 3 |
| DIRECT | 0 | 3 | 3 |
| STRUCTURAL | 0 | 0 | 4 |
| COARSE | 10 | 0 | 0 |
| UNKNOWN | 0 | 2 | 2 |
| extra claim flags | 40 | 21 | 27 |
| review noise (extra/all surfaced) | 0.80 | 0.81 | 0.75 |
| negative-case surfaced claims | 18 | 10 | 14 |

Every miss is listed explicitly — none is averaged away:

| case | commit | missed claim | gold | mechanism (from recorded result) |
|---|---|---|---|---|
| C-001 | 422036cd (07-08) | F08 | HIGH lost-wake window fix | scheduler.cpp hunks attributed to `await_completion_*`/`await_ready_flag` symbols; attributed hunks get no coarse fallback; no depth-2 monotonic path reaches the (resolved) wake anchors. Case still UNKNOWN (F04 unresolved-anchor demotion), but F08 absent. File-only (A) catches this one via file binding. |
| C-007 | 96e3d66e (08-13) | F03 | MEDIUM frozen winner outcome | winner-outcome symbols live in the scheduler wait domain; no depth-2 path reaches the arena/backend terminal anchors; scheduler.cpp is not in F03's file bindings. Both A and C miss. |
| C-012 | 869be913 (08-11) | F08 | MEDIUM uring cancel-path poison wake | scip-clang 0.4.0 (no enclosing ranges) folds all hunks into the nearest-preceding definition `available` — the wrong start symbol — so traversal starts nowhere useful; F08 absent (F03 surfaced UNKNOWN via the gated uring anchor). HEURISTIC-correlated. Both A and C miss. |

Baselines agree with the misses where they must: A (file-only) misses
exactly C-007 and C-012 (their changed files are not bound to the missed
claims); B (explicit-only) finds only DIRECT-anchor edits. File-only had
the best raw recall on this corpus but the worst noise (40 extra flags)
and its recall depends on manifest bindings, not evidence.

## Facet-level safety (§27; only facet-gold cases scored)

| case | gold required | resolver scope | PRECISE targets | missing required | safety |
|---|---|---|---|---|---|
| C-005 | f1-wait-record | CONSERVATIVE_ALL | 2 (parent 2) | 0 | safe |
| C-006 | cancel-token-epoch | PRECISE | 2 (parent 3) | 0 | safe (33% reduction) |
| C-009 | spawn-wake-epoch | PRECISE | 5 (parent 6) | 0 | safe |
| C-010 | e9-park-wake | PRECISE | 3 (parent 6) | 0 | safe (50% reduction) |
| C-011 | e12-rwlock-scheduler-liveness | **PRECISE** | 3 (parent 6) | **1** | **SAFETY FAILURE** |

**C-011 (7afb9378, #161):** the diff's triggering anchor was
`park-boundary` (DIRECT); Phase-B routing selects the triggering anchor's
facets only, so revalidation narrowed to {e9-park-wake, e9-trace-conformance,
e8-suspend-switch} and dropped `e12-rwlock-scheduler-liveness` — the
downstream liveness face this very commit formalized (it created that
suite's TLA+ model). PRECISE narrowing produced a potential false negative
on real history; the route's paths carry HEURISTIC attribution. §41 is
answered: **HEURISTIC PRECISE did exhibit unsafe narrowing on real
history.** CONSERVATIVE_ALL (C-005) was safe, as designed.

## HEURISTIC boundary (§28/§41)

- positive recoveries involving HEURISTIC attribution: **9 of 9** — with
  scip-clang 0.4.0 emitting no enclosing ranges, every reference edge (and
  most start attributions) is nearest-preceding by construction; no
  recovery on this corpus was free of it.
- PRECISE facet routes involving HEURISTIC: 4 of 4.
- misses correlated with HEURISTIC attribution: **1 of 3** (C-012 — wrong
  start-symbol fold; the other two misses are structural-reach limits).
- unsafe facet narrowing correlated with HEURISTIC: **1** (C-011).
- negative-noise corroboration: comment-only commits (C-015, C-016) and a
  mechanical enum rename (C-022) surface STRUCTURAL/DIRECT claims purely
  through nearest-preceding folds — heuristic edges add real review noise.

## Conservative behavior (§7/§17 applicability)

PARTIALLY_APPLICABLE epochs (pre-08-16 worlds) behaved conservatively
exactly as predicted: missing anchor symbols fail closed via
UNRESOLVED_ANCHOR demotion (claims surface as UNKNOWN, never NO); the
empty-diff formal-only commit (C-023) and the build/config-only commit
(C-018) answered NO_FORMAL_IMPACT only after the Build-Truth precondition;
the unrelated sync-io fix (C-019) failed closed UNKNOWN with zero claims.
No OUT_OF_EPOCH or AMBIGUOUS case entered the primary denominator
(C-024 skipped by frozen applicability; C-013/C-014 executed and reported,
never scored).

## Replay identity (§37, per-case records in results.json)

H1 base/head worktrees distinct — yes (all 23); H2 historical
compile_commands regenerated — yes; H3 Build Manifest head/build ids belong
to the historical world — yes; H4 SCIP graph head historical — yes; H5
resolver result from the historical graph — yes; H6 current graph not
reused — yes (main-graph checksum unchanged across the run); H7 worktrees
removed — yes (`git worktree list` clean, no leaked case worktrees); H8
result identity matches frozen case base/head — yes.

## Answering Phase C's governing question (§46)

> Does the whole stack survive contact with real repository history?

**Not unconditionally.** The stack never lies (silent NO = 0; every
uncertainty stayed fail-closed; conservative UNKNOWN behavior worked in
every partial epoch), but on this frozen corpus it missed 3 of 12 expected
positive claims — one from structural-reach limits (C-001), one from
authority that genuinely does not span the scheduler wait domain (C-007,
MEDIUM), one from HEURISTIC start-symbol folding (C-012) — and one PRECISE
facet route narrowed away a gold-required target (C-011). File-only
mapping achieved higher raw recall at higher noise; explicit-only mapping
achieves neither recall nor usable precision. FACETS_EARNED (Phase B,
synthetic specimens) does not transfer unconditionally to real history.

## Mechanical-gate registration (disclosed)

The frozen candidate inventory (docs/results/formal/fdg0-phase-c-candidates.json)
records, per historical commit, the files that commit changed — including paths
that predate the layout migration (the removed docs/spec tree, the flat
pre-migration spec/tla layout). Those
strings are frozen corpus data about past states, not live authority references,
so the file is registered in scripts/formal/verify.py's `CORPUS_DATA_FILES`
old-path-scan exclusion (the same class of exclusion as docs/history/). Only the
inventory artifact is excluded; the gold, results, and this doc remain fully
scanned.

## Residuals

1. The three misses and the C-011 narrowing are OPEN EVIDENCE. Fixing them
   requires authority/algorithm work (traversal policy, attribution
   quality, facet routing scope) that Phase C forbids. No anchor, facet,
   depth, or resolver semantics were changed after the gold freeze.
2. scip-clang 0.4.0's missing enclosing ranges make HEURISTIC edges
   unavoidable; the recorded unsafe narrowing is a direct consequence.
3. Corpus boundary: 12 positive labels is a small denominator; single
   additional misses move recall by 8.3 points. The verdict is
   corpus-relative and preregistered, not a general recall estimate.
4. Build config was frozen to the default world; liburing-gated behavior
   is uniformly a declared config-gated gap.
5. Baseline A/B were computed on the same frozen worlds; B shares C's
   conservative machinery at depth 0.

## Promotion status

Research evidence only. NOT production formal gating authority. Phase C
does NOT authorize facet-narrowed CI, pre-push narrowing, auto-clear of
formal impact, or MODEL_UPDATE_NOT_REQUIRED dispositions. STOP: AST-SF and
enforcement are not started; do not merge automatically; return for
adversarial human review.
