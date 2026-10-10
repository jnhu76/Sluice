# M-A1 BlockingIoPool disposition (#462, Phase M closeout wave 2)

> **Decision overlay:** [owner-selected Clean Minimal V1](m-phase-clean-minimal-v1-owner-direction.md) chooses **P2 final retirement** for v1 with **P3 compatibility-only isolation solely as the bounded transition**. The P3 proposal below remains dated decision input, not the final release disposition. No immediate header/implementation removal is authorized.

- **Status**: DISPOSITION_INPUT_COMPLETE — PENDING_OWNER_DECISION on the
  PROD-03 admittance ruling (candidate P3 below). No source, header, archive
  member or manifest entry is deleted, added or edited by this record.
- **Baseline**: live master `94b5cfcdace6ee15c75c909dae09665eda2d1a26`.

## 1. The two PROD-03 channels, evaluated against live evidence

**Q1 accepted workload? NO.** W-01 explicitly requires direct execution
"without allocating an `IoContext`, worker, request table, or progress
source" (root `:164`). W-03's concurrent world is the ThreadPoolBackend +
pollable progress source (root `:172`) — the canonical request world, not a
generic `std::function` pool. No ledger row, no formal model, no MIG-02 row
names BlockingIoPool (the 17-row MIG-02 table `:918-934` has none).

**Q2 named correctness/resource obligation? NO.** The pool performs **zero
I/O** (grep over `src/blocking_io_pool.cpp`: no read/write/open/close/fsync
syscall; it is fixed workers + bounded `std::deque` + mutex/cv —
`blocking_io_pool.cpp:38-179`). Nothing in root SEM/LIFE/ERR or the adopted
ADRs obligates a second task pool. Its history confirms origin-as-benchmark:
introduced for the W1-W4 bench matrix (`1918701e`), promoted to production
with tests (`c83a0acd`), tests deleted in the repository-context cleanup
(`5f62b55b`, 2026-09-07) — **zero in-tree consumers since** (re-verified this
wave: the census grep returns only its own three files; apps 0 / tests 0).

Therefore no channel for admission (P1) is satisfied. P1 would additionally
require a root amendment (GOV-04) and a boundary record: `Task<T>` is a
task-world storage cell that "must never be promoted to request-result truth
without a root amendment" (F0 policy §1.8); the concrete difference from
ThreadPoolBackend is the `std::function` admission + blocking `get()` cell
vs the `RequestCore`/`Completion<T>`/`RequestKey` request world
(`include/sluice/async/threadpool_backend.hpp:30-80` vs
`include/sluice/blocking_io_pool.hpp:22-92`).

## 2. Candidate disposition: P3 — not admitted; COMPATIBILITY_ONLY isolation with explicit sunset

**PENDING_OWNER_DECISION.** Evidence-ordered rationale:

- Removal (P2) now is **not** supported: zero consumers alone "neither
  preserves nor deletes" (root `:129`); both headers are F1 frozen installed
  surface (manifests `:81/:86`, 79-header set) and `blocking_io_pool.cpp.o`
  sits byte-identical in both frozen archives (sha256 `8f0dfa11…f85e8`,
  26064 bytes); external use is unknown (F0 §3 legacy stream world =
  `unknown`; "No row may treat IN_TREE_CONSUMERS = 0 as 'no consumers'").
  P2's precondition — the §1.7(5) explicit external-use decision — is a
  #402 human-review obligation that has not been taken.
- Indefinite COMPAT without sunset is equally prohibited (policy §1.7).
- Hence: record the surface **COMPATIBILITY_ONLY, not admitted to canonical
  v1**, sunset-bound, with these exit conditions:

| field | value |
|---|---|
| DISPOSITION | P3 candidate: not admitted to canonical v1; isolated compatibility surface; no physical change |
| AUTHORITY | PROD-03 root:127/:129 (no channel satisfied); F0 D-11 (CANDIDATE UNDECIDED → this record supplies the workload/capability audit); census L-05 COMPAT; policy §1.7 |
| CONSUMERS | in-tree 0 (live re-verified); external unknown (U-row below) |
| PRESERVED_BEHAVIOR | installed headers, symbols (`make_blocking_io_pool`, `BlockingIoPool::shutdown/pool_stats`, static `current_blocking_io_pool`), archive membership — all unchanged; any external program keeps compiling/linking |
| EVIDENCE | this record; manifests + archive baselines (unchanged); git history 1918701e → c83a0acd → 5f62b55b |
| OWNER | F4 (disposition execution), F5 (package alignment) |
| MAINTENANCE COST | 479 lines (96 header + 159 impl header + 224 src), zero tests since 5f62b55b, zero consumers; cost is frozen-surface carry, not active maintenance risk |
| EXIT_TRIGGER (retire) | (i) U-external-use human decision (policy §1.7(5)) lands as breakage-acceptance or deprecation window; then (ii) #402 F4/F5 gates authorize the removal batch: headers 79→77 (fragment exception `_impl` dies with it → re-run the standalone gate over the remaining 77), archive 13→12 objects, B-01 same-slice re-freeze |
| EXIT_TRIGGER (admit) | a future accepted workload would require the GOV-04 root amendment + boundary record + oracle — none exists |
| DOWNSTREAM_F_GATE | F4 `SINGLE_CANONICAL_NATIVE_RESOURCE_AUTHORITY` / `RESOURCE_CONSUMERS_MIGRATED_OR_ISOLATED`; F5 package-role consistency |

## 3. Constraint obligations recorded with this disposition

1. **External-use registration**: F0 census §3 covers the pool only via the
   L-01..L-09 family row; this record asks the owner to add a dedicated U-row
   (or to affirm the family-row coverage) when ruling — the same shape #475
   required for U-01/U-08.
2. **c-x10-004 banner recheck — performed this wave** (its trigger condition
   is exactly this disposition): `docs/adr/0002-explicit-file-api-architecture.md:3-15`
   carries the HISTORICAL/SUPERSEDED banner stating the sole normative
   authority is the root and that in-body "normative/frozen/authority"
   language is time-scoped; the banner covers the BlockingIoPool semantic
   passages (incl. the `:1070` execution-owner question). No edit needed.
3. **DOC_CLAIM scan**: current docs mention BlockingIoPool only in
   ADR-0002 (superseded), F0/F1/M0 audit artifacts and this record — no
   unqualified canonical claim exists; the F4 execution batch must re-run the
   scan and file it.
4. **Count-drift correction**: F0 D-11/L-05 record "src 1 + tests"; the tests
   half has been false since `5f62b55b` — recorded here per the census §7
   drift rule; no F0 artifact is edited (they are adopted history).

## 4. What closes M-A1

The owner rules the PROD-03 question (adopt P3, or select P1/P2 with their
channels' preconditions). On adoption, D-11 flips UNDECIDED→decided, the
M-A1 family gate `PROD-03_ADMITTANCE_DECIDED` backfills on #458, and the
disposition row above becomes the F4/F5 input. Nothing in Phase M executes
physically.
