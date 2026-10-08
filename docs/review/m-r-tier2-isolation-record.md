# M-R tier-2 consumer isolation record (#467)

Machine-readable source of record: [`m-r-tier2-isolation-record.json`](m-r-tier2-isolation-record.json).
The tier-1 half of the M-R disposition is PR #468 (merged as `9d1b5cc0`) plus
the residual migration in PR #471 (head `dad773c2`); this record supplies the
isolation half. Neither half declares the family gate closed by itself.
This file is a summary; it does not create a contract. The M consumer inventory
itself stays in [`m-consumer-edges.json`](m-consumer-edges.json) (scan snapshot at
its own `BASE_SHA`) — this record states what the M window did with each
isolated consumer and what the closing arm owes.

## Authority

- #458 §3.1 migrationStrategy + §6 X-16 ruling + §7 child-issue charter
  (adopted M1 decomposition, R2/R3-reviewed) is the standing authority:
  tier-1 migrates the reachable context-face test consumers onto the six
  `submit_*/Request<T>` spellings with the `attach_observer` three-test
  exemption (§3.1 blockers 5) and the five zero-symbol include deletions,
  wires B-02, defers Batch to the F2 B-01 re-freeze batch (blockers 6) and
  keeps the `identity_of` fallback dormant (blockers 2); tier-2 = X-16
  explicit policy isolation pending F3 (§6).
- #459 M-R RULING Option A (comment 6055510469) is the execution-scope
  selection record that packages that standing authority, not an independent
  human ruling: Option B contradicted §6 clause 2 and §3.1 blockers 6, and
  Option C required unadopted faces, so Option A was the only packaging
  consistent with #458. Its authorization-source paragraph cited the
  Execution Lead directive's conditional sentence plus the #458 orchestration
  comment (6055084231, which disclaims ruling status); that sentence is
  prioritized execution advice and is no longer cited as authority (provenance
  correction recorded on #459 and in the projection artifact below).
- #402 gate arm `RETAINED_CONSUMERS_MIGRATED_OR_EXPLICITLY_ISOLATED`.
- root MIG-02: caller-owned `Completion` is Compatibility only; no M window
  authorizes retirement.

No prohibition was crossed: no Completion deletion, no F2/F3/F4 contraction, no
multi-worker→single-owner conversion, no unadopted canon, no ADR-0003 adoption,
no F1 manifest edit.

## Final consumer-state projection

Every one of the 75 census M-R records now has an explicit final disposition in
[`m-r-consumer-disposition-final.json`](m-r-consumer-disposition-final.json)
(MIGRATED / POLICY_ISOLATED / COMPAT_TEST_RETAINED / NOT_A_CONSUMER /
DOWNSTREAM_MECHANISM_RETAINED, with subscope splits for the compound rows).
`scripts/verify_mr_disposition.py` recomputes the projection against this
record and the census (coverage, exclusivity, field discipline, bidirectional
census_rows membership) and runs in CI as the `mr-disposition-guard` workflow.
The PR #471 residual pair is recorded there as census rows of the M-F and aux
families, so the M-R delivery claim is recomputable end to end.

## Isolated items

| ID | Isolated unit | Census rows | Owner | Exit condition |
|----|---------------|-------------|-------|----------------|
| T2-01 | X-16 await/waiter runtime surface | c-x16-001..010, 015..019, c-x01-034/035, c-x01-040, c-x04-008, c-x05-001/002/010 | F3 | An F3 arm closes the node (re-typed await over `Request<T>` or runtime retirement) + waiter/shutdown/file oracles re-proven + B-04 host decision |
| T2-02 | Core-cutover publication/observer oracles | c-x01-023, c-x01-026, c-x14-005 | F2 | Publication-seam contraction with an equivalent adopted-boundary oracle keeping every mutant kill |
| T2-03 | Shutdown settlement oracle on caller-held carriers | c-x01-022 | F2 | F2 contraction of the observer registration/cancel hinge with the shutdown oracles re-proven |
| T2-04 | RequestHandle identity + release-path oracle | c-x01-019, c-x02-002/003, B-05 | F2 | D-17 decided + identity oracle proving the core path suffices |
| T2-05 | Compat-spelling pins and the forced backend seam | c-x01-014/015, c-x01-020, c-x01-030, c-x01-039 | F2 | `AsyncBackend` seam contraction with the admission oracle re-derived and the E1 conformance matrix re-run |
| T2-06 | `Batch::Slot` value carriers | c-x01-005, c-x01-006, c-x01-024 (batch subscope) | F2 | Batch re-typing with the B-01 same-slice re-freeze |
| T2-07 | `identity_of` arena-field fallback (dormant) | c-x02-002..006, B-05 | F2 | F2 identity contraction |
| T2-08 | App task carriers and await chains | c-x01-001..004, c-x16-011..014 | F2 + F3 | F2/F3 arm closure + app consumption suites + COPY-B/TAIL obligations |
| T2-09 | F1 publication-epilogue compat sub-spelling | c-x01-025, c-x01-028 | F2 | Adopted-face oracle for the mid-epilogue boundary |
| T2-10 | File-surface test carriers | c-x01-037/038, c-x06-004..008 | F4 | F4 File contraction with semantic oracles re-run |
| T2-11 | Installed-package consumer probes under the F1 freeze | c-x01-901, B-01 | F2 | Contraction slice re-freezing both manifests together with the probes |
| T2-12 | Cross-thread submission-observation carve-outs | c-x01-024 | F2 | An adopted cross-thread observation spelling, or explicit re-shaping of the two oracles with the §24 acceptance shape re-proved |

Each item's reason, surviving behaviour, exact scope, downstream gating effect
and evidence are in the JSON record. None of them is counted as migrated; the
M-R family's migration claim covers the tier-1 sets delivered by #466 and #471
(the residual pair found by the Wave-5 review).

## COPY-B / TAIL surviving-behaviour obligations

- **OB-1** — copy pipeline Version B keeps bounded buffer reuse, pipeline depth,
  worker count, short-write handling and its reported counters.
- **OB-2** — atomic output keeps temp-file → rename → directory fsync, with
  EINTR retried through the shared POSIX retry authority and a real error
  reported at `dir_sync`.
- **OB-3** — tail keeps `-n` semantics over its backward single-byte and window
  reads.

Evidence for all three: the four app consumption suites in every profile plus
the app smoke runs, and the B-02 directory-fsync oracle for OB-2.

## Evidence index

| Evidence | Artifact |
|----------|----------|
| noliburing release suite (50/50) | `/tmp/m-residual-noliburing-release.log` |
| noliburing debug suite (50/50) | `/tmp/m-residual-noliburing-debug.log` |
| liburing release suite, liburing 2.9 prefix (64/64) | `/tmp/m-residual-liburing-release.log` |
| ASan+UBSan, migrated suites, both profiles | `/tmp/m-residual-asan-<suite>.log` |
| F1 frozen reproduction, both profiles, all gates PASS | `/tmp/m-residual-f1-noliburing.json`, `/tmp/m-residual-f1-liburing.json` |
| uring cutover mutation builds re-run (8 killed) | `/tmp/m-residual/uring_cutover_mut_*.log` |
| Pinned stress, cross-thread carve-out (0/30; 2-3/25 before) | `taskset -c 0` loops on `threadpool_external_loop_test` |
| CI release job that caught the regression (PR #468) | `gh run view 37758034238 --job 113247343965` |
| Tier-1 delivery | PR #468 (merged `9d1b5cc0`, reviewed head `7f5b113b`) |
| Residual tier-1 delivery | PR #471 (head `dad773c2`, merged `7f934e0f`) |

Earlier evidence logs from the #467/#468 rounds (`/tmp/m-ev-*.log`,
`/tmp/f1-check-*.json`) are retained in the JSON record's per-item evidence
lists; the table above is the state at the current verified head.
