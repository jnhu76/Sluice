# Phase M final closeout

Machine-readable twin: [`m-phase-final-closeout.json`](m-phase-final-closeout.json).
This document reports the state of Phase M at the end of the execution window.
It is a report, not a contract: every claim below names the artifact that
carries it.

The tier-1 half of the M-R disposition is PR #468 (merged `9d1b5cc0`); the
isolation half is PR #469 (merged `667e17f8`). Neither declares the family gate
closed by itself. Final master SHA at the time of writing: `667e17f8`.

## Headline

**PHASE_M = NO** — Phase M is not closed. One family (M-R) reached its gate
arm in this window; the other six families are blocked on four human authority
decisions that the Decision Packet asked for and that nobody has answered.
`F2/F3/F4_PRODUCTION_RETIREMENT = NOT_EXECUTED`;
`F1_HISTORICAL_BASELINE = IMMUTABLE` (both frozen profiles reproduce, manifests
untouched); `M_FAMILY_DISPOSITIONS = COMPLETE`;
`M_REQUIRED_EVIDENCE = PASS`; `M_CONSUMER_MIGRATION_OR_ISOLATION = VERIFIED`
for M-R.

## Family dispositions

| family | gate | status | owner | blocker |
|--------|------|--------|-------|---------|
| M-R | RETAINED_CONSUMERS_MIGRATED_OR_EXPLICITLY_ISOLATED | PASS (PRs #468/#469 merged; independent review APPROVE on both) | F2 | — |
| M-H | REPLACEMENT_ADOPTED | BLOCKED_AUTHORITY_DECISION | F3 | D2 host replacement (B-04) |
| M-F | VECTORED+SECOND-AUTHORITY_DECIDED | BLOCKED_AUTHORITY_DECISION | F4 | D3a B-07 vectored / D3b D-09(1) |
| M-A1 | PROD-03_ADMITTANCE_DECIDED | BLOCKED_AUTHORITY_DECISION | F4 | D4 BlockingIoPool admittance |
| M-A2 | PROD-03_ADMITTANCE_DECIDED (bound to M-F) | BLOCKED_UPSTREAM | F4 | M-F ruling not taken |
| M-A3 | PACKAGE_DECISION_RECORDED | BLOCKED_AUTHORITY_DECISION | F4 | D-13 relocate-vs-internalize |
| M-A4 | none (isolated by F1) | CLOSED_NO_ACTION | none | — |

Decisions still owed: see the Decision Packet on #458 (comment 6055397430).

## What M actually delivered

1. **Tier-1 carrier migration (#466 → PR #468).** Reachable context-face test
   consumers retyped onto the adopted six `submit_*(op) -> Result<Request<T>>`
   spellings: threadpool and uring progress-race, threadpool and uring
   external-loop, uring registration-lifecycle, and two sync-admission
   rejection consumers. Five zero-symbol `<sluice/file.hpp>` includes and four
   now-unused `completion.hpp` includes dropped. No `src/` or `include/` change
   at all, so no F2/F3/F4 retirement and no F1 re-freeze was triggered.
2. **B-02 closed (#466).** `SLUICE_COPY_INTERNAL_TESTING` is now defined by
   exactly one target and the `DirFsyncScript` EINTR oracle is reachable and
   discriminating: dropping `retry_on_eintr` around the directory fsync kills
   `directory_fsync_eintr_is_retried_to_success`.
3. **Tier-2 isolation record (#467 → PR #469).** Eleven isolated consumer
   units, each with authority, reason, surviving behaviour, exact scope, owner,
   downstream gating effect, exit condition and evidence; plus the COPY-B/TAIL
   surviving-behaviour obligations.
4. **A finding handed to F2.** The mid-publication-epilogue visibility of a
   cancelled entry exists only on the compat face (`Completion` turns ready at
   `record.publish`; the `Request` face observes only at
   `complete_publication`). Both F1 publication oracles assert that boundary,
   so their sub-assertions keep the compat spelling until F2 expresses the same
   boundary on the adopted face.

## Review history

An independent reviewer (fresh context, adversarial brief) verified the tier-1
diff at `888ccf2e` and returned `REQUEST_REVISIONS` with two P1s — a
cross-thread `Request` handoff data race, and a drain guard declared before the
bindings it tracked — plus three P2/P3s. Both P1s and all P2/P3s were fixed on
the branch (commits `89827e50`, `7f5b113b`) and the fixes re-verified: pinned
`taskset -c 0` stress went from 2-3 failures / 25 runs to 0 / 30, and CI run
37761311720 is green in all five checks.

The same review surfaced one honest residual: under artificial 8x
oversubscription these race oracles flip slightly more often than master's
(6/160 vs 1/160). Instrumentation shows the failing sub-assertion is the
unchanged `prepark` precondition (`progress=1 completed=1 prepark=1 ready=1`),
i.e. the migration did not change what the wait observed; the oracle was left
untouched because relaxing it under load would weaken what it proves. A
separate pre-existing hang (`backend_conformance_e1_threadpool_test` under load
inside `pre_execution_cancel_wins_with_known_zero_effect`) is reproducible on
master's own binary and is not attributable to this phase.

## Evidence

| Evidence | Result | Artifact |
|----------|--------|----------|
| noliburing release, full suite | 50/50 targets pass | `/tmp/m-ev-noliburing-release-test.log` |
| noliburing debug, full suite | 50/50 pass | `/tmp/m-ev-noliburing-debug-test.log` |
| liburing release (liburing 2.9 prefix), full suite | 64/64 pass | `/tmp/m-ev-liburing-release-test.log` |
| ASan+UBSan noliburing / liburing migrated suites | 50/50 ; 7/7 | `/tmp/m-ev-asan-test.log` |
| F1 frozen reproduction, both profiles | all gates PASS at the tier-1 head | `/tmp/f1-check-noliburing.json`, `/tmp/f1-check-liburing.json` |
| Comment-authority guard + F1 provenance self-tests | OK / 10 tests OK | — |
| B-02 fault injection mutant | oracle killed the mutant | PR #468 body |
| Four apps, both profiles | build/link + smoke, byte-identical copy | PR #468 body |

This closes B-02 (seam wired) and B-03 on this host (liburing 2.9 reconstructed
at the frozen prefix; liburing-profile build, link, suite and frozen
reproduction all pass). B-01 was not triggered and its reproduction is proven.
B-04/B-05/B-06/B-07 stay open with their owners.

## Prohibitions check

No consumer was deleted to shrink counts; no multi-worker→single-owner
conversion; no F2/F3/F4 retirement; ADR-0003 not adopted; no canonical vectored
I/O introduced; zero-consumer evidence was never used as a safety proof; the
frozen manifests were not edited (reproduction proves it); test counts and green
CI were never substituted for semantic evidence (mutant kills and the boundary
finding are recorded); every `POLICY_ISOLATED` item has an owner, a necessity
and an exit condition; and Phase M was not declared closed before its
obligations were met.

## What would close Phase M

1. The four rulings (D2, D3a, D3b, D4) from the Decision Packet, then the
   F3/F4 execution arms they authorize.
2. M-R's two PRs merged after independent review, with the family gate row
   backfilled on #458.
3. The Wave-5 fresh independent review verdict on this closeout.
