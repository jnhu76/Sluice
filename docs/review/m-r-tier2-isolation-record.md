# M-R tier-2 consumer isolation record (#467)

Machine-readable source of record: [`m-r-tier2-isolation-record.json`](m-r-tier2-isolation-record.json).
The tier-1 half of the M-R disposition is PR #468 (merged as `9d1b5cc0`); this
record supplies the isolation half. Neither half declares the family gate
closed by itself.
This file is a summary; it does not create a contract. The M consumer inventory
itself stays in [`m-consumer-edges.json`](m-consumer-edges.json) (scan snapshot at
its own `BASE_SHA`) — this record states what the M window did with each
isolated consumer and what the closing arm owes.

## Authority

- #459 M-R RULING Option A (comment 6055510469): tier-1 = reachable context-face
  test consumers retyped to the six `submit_*/Request<T>` spellings,
  `attach_observer` tests exempted, five zero-symbol includes deleted, B-02
  wired; tier-2 = X-16 explicit policy isolation pending F3; Batch retyping
  rides the F2 B-01 re-freeze batch; `identity_of` fallback stays dormant.
- #458 §3.1/§6/§7 (adopted M1 decomposition) and #402 gate arm
  `RETAINED_CONSUMERS_MIGRATED_OR_EXPLICITLY_ISOLATED`.
- root MIG-02: caller-owned `Completion` is Compatibility only; no M window
  authorizes retirement.

No prohibition was crossed: no Completion deletion, no F2/F3/F4 contraction, no
multi-worker→single-owner conversion, no unadopted canon, no ADR-0003 adoption,
no F1 manifest edit.

## Isolated items

| ID | Isolated unit | Census rows | Owner | Exit condition |
|----|---------------|-------------|-------|----------------|
| T2-01 | X-16 await/waiter runtime surface | c-x16-001..010, 015..019, c-x01-040 | F3 | An F3 arm closes the node (re-typed await over `Request<T>` or runtime retirement) + waiter/shutdown/file oracles re-proven + B-04 host decision |
| T2-02 | Core-cutover publication/observer oracles | c-x01-023, c-x01-026, c-x14-005 | F2 | Publication-seam contraction with an equivalent adopted-boundary oracle keeping every mutant kill |
| T2-03 | Shutdown settlement oracle on caller-held carriers | c-x01-022 | F2 | F2 contraction of the observer registration/cancel hinge with the shutdown oracles re-proven |
| T2-04 | RequestHandle identity + release-path oracle | c-x01-019, c-x02-002/003, B-05 | F2 | D-17 decided + identity oracle proving the core path suffices |
| T2-05 | Compat-spelling pins and the forced backend seam | c-x01-020, c-x01-030 | F2 | `AsyncBackend` seam contraction with the admission oracle re-derived |
| T2-06 | `Batch::Slot` value carriers | c-x01-005, c-x01-006 | F2 | Batch re-typing with the B-01 same-slice re-freeze |
| T2-07 | `identity_of` arena-field fallback (dormant) | c-x02-002..006, B-05 | F2 | F2 identity contraction |
| T2-08 | App task carriers and await chains | c-x01-001..004, c-x16-011..014 | F2 + F3 | F2/F3 arm closure + app consumption suites + COPY-B/TAIL obligations |
| T2-09 | F1 publication-epilogue compat sub-spelling | c-x01-025, c-x01-028 | F2 | Adopted-face oracle for the mid-epilogue boundary |
| T2-10 | File-surface test carriers | c-x01-037/038, c-x06-006/008 | F4 | F4 File contraction with semantic oracles re-run |
| T2-11 | Installed-package consumer probes under the F1 freeze | c-x01-901, c-x16-015, B-01 | F2 + harness | Contraction slice re-freezing both manifests together with the probes |
| T2-12 | Cross-thread submission-observation carve-outs | c-x01-024 | F2 | An adopted cross-thread observation spelling, or explicit re-shaping of the two oracles with the §24 acceptance shape re-proved |

Each item's reason, surviving behaviour, exact scope, downstream gating effect
and evidence are in the JSON record. None of them is counted as migrated; the
M-R family's migration claim covers only the tier-1 set delivered by #466.

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
| noliburing release suite (50/50) | `/tmp/m-ev-noliburing-release-test.log` |
| noliburing debug suite (50/50) | `/tmp/m-ev-noliburing-debug-test.log` |
| liburing release suite, liburing 2.9 prefix (64/64) | `/tmp/m-ev-liburing-release-test.log` |
| ASan+UBSan noliburing (50/50) | `/tmp/m-ev-asan-test.log` |
| ASan+UBSan liburing, migrated suites (7/7) | `/tmp/m-ev-asan-<suite>.log` |
| F1 frozen reproduction, both profiles, all gates PASS | `/tmp/f1-check-noliburing.json`, `/tmp/f1-check-liburing.json` |
| Pinned stress, cross-thread carve-out (0/30; 2-3/25 before) | `taskset -c 0` loops on `threadpool_external_loop_test` |
| CI release job that caught the regression (PR #468) | `gh run view 37758034238 --job 113247343965` |
| Tier-1 delivery | PR #468 (merged `9d1b5cc0`, reviewed head `7f5b113b`) |
