# Phase M final closeout

Machine-readable twin: [`m-phase-final-closeout.json`](m-phase-final-closeout.json).
This document reports the state of Phase M at the end of the execution window.
It is a report, not a contract: every claim below names the artifact that
carries it.

The tier-1 half of the M-R disposition is PR #468 (merged `9d1b5cc0`) plus the
residual migration in PR #471; the isolation half is PR #469 (merged
`667e17f8`), corrected by #471. No single PR declares the family gate closed by
itself. Final master SHA at the time of writing: `MASTER_AFTER_471`.

## Headline

**PHASE_M = NO** — Phase M is not closed. One family (M-R) reached its gate arm
in this window; the other six families are blocked on human authority decisions
that the Decision Packet asked for and that nobody has answered (D2, D3a, D3b,
D4; M-A3 additionally needs the package decision D-13, which the packet's four
questions did not cover — an addendum on #458 adds it). `F2/F3/F4_PRODUCTION_RETIREMENT = NOT_EXECUTED`;
`F1_HISTORICAL_BASELINE = IMMUTABLE` (both frozen profiles reproduce, manifests
untouched); `M_FAMILY_DISPOSITIONS = COMPLETE` (all seven families carry a
disposition; six have a downstream owner and an exit condition, M-A4 is closed
with no owner because no action is owed); `M_REQUIRED_EVIDENCE = PASS`;
`M_CONSUMER_MIGRATION_OR_ISOLATION = VERIFIED` for M-R after the Wave-5 review
found and PR #471 migrated the last two un-dispositioned consumers.

## Family dispositions

| family | gate | status | owner | blocker |
|--------|------|--------|-------|---------|
| M-R | RETAINED_CONSUMERS_MIGRATED_OR_EXPLICITLY_ISOLATED | PASS (PRs #468/#469/#471 merged; independent review APPROVE on each) | F2 | — |
| M-H | REPLACEMENT_ADOPTED | BLOCKED_AUTHORITY_DECISION | F3 | D2 host replacement (B-04) |
| M-F | VECTORED+SECOND-AUTHORITY_DECIDED | BLOCKED_AUTHORITY_DECISION | F4 | D3a B-07 vectored / D3b D-09(1) |
| M-A1 | PROD-03_ADMITTANCE_DECIDED | BLOCKED_AUTHORITY_DECISION | F4 | D4 BlockingIoPool admittance |
| M-A2 | PROD-03_ADMITTANCE_DECIDED (bound to M-F) | BLOCKED_UPSTREAM | F4 | M-F ruling not taken |
| M-A3 | PACKAGE_DECISION_RECORDED | BLOCKED_AUTHORITY_DECISION | F4 | D-13 relocate-vs-internalize (outside the packet; addendum posted) and the M-F dependency |
| M-A4 | none (isolated by F1) | CLOSED_NO_ACTION | none | — |

Decisions still owed: the Decision Packet on #458 (comment 6055397430) plus the
D-13 addendum on the same issue.

## What M actually delivered

1. **Tier-1 carrier migration (#466 → PR #468).** Reachable context-face test
   consumers retyped onto the adopted six `submit_*(op) -> Result<Request<T>>`
   spellings: threadpool and uring progress-race, threadpool and uring
   external-loop, uring registration-lifecycle, and two sync-admission
   rejection consumers. Five zero-symbol `<sluice/file.hpp>` includes and four
   now-unused `completion.hpp` includes dropped. No `src/` or `include/` change
   at all, so no F2/F3/F4 retirement and no F1 re-freeze was triggered.
2. **Residual tier-1 migration (PR #471).** The Wave-5 review found two
   `AsyncIoContext` consumers that were neither migrated nor recorded
   (`file_access_precedence_test`, c-x01-015, twelve plain-spelling
   submissions; `progress_source_ownership_test`, c-x01-016, six submissions).
   #471 retypes them onto the adopted face, keeps the four `_request` pins and
   the forced backend virtuals as recorded compat evidence, and corrects the
   isolation record's scopes.
3. **B-02 closed (#466).** `SLUICE_COPY_INTERNAL_TESTING` is now defined by
   exactly one target and the `DirFsyncScript` EINTR oracle is reachable and
   discriminating: dropping `retry_on_eintr` around the directory fsync kills
   `directory_fsync_eintr_is_retried_to_success`.
4. **Tier-2 isolation record (#467 → PR #469, corrected by #471).** Twelve
   isolated consumer units, each with authority, reason, surviving behaviour,
   exact scope, owner, downstream gating effect, exit condition and evidence;
   plus the COPY-B/TAIL surviving-behaviour obligations and the T2-12
   cross-thread submission carve-outs.
5. **Two findings handed to F2.** The mid-publication-epilogue visibility of a
   cancelled entry exists only on the compat face (`Completion` turns ready at
   `record.publish`; the `Request` face observes only at
   `complete_publication`), so both F1 publication oracles keep that
   sub-assertion (T2-09). And the compat cell bound inside a submitting call is
   observable across threads before the submitter hands anything back, so two
   cross-thread host-loop oracles keep the compat cell (T2-12).

## Review history

An independent reviewer (fresh context, adversarial brief) verified the tier-1
diff at `888ccf2e` and returned `REQUEST_REVISIONS` with two P1s — a
cross-thread `Request` handoff data race, and a drain guard declared before the
bindings it tracked — plus three P2/P3s. Both P1s and all P2/P3s were fixed on
the branch (commits `89827e50`, `7f5b113b`) and the fixes re-verified: pinned
`taskset -c 0` stress went from 2-3 failures / 25 runs to 0 / 30, and CI run
37761311720 is green in all four CI jobs (release, debug, F1 self-tests,
comment-authority guard; the fifth check-run status is the legacy CodeRabbit
status). Those verdicts were relayed through PR comments by the author; the
repository holds no GitHub review objects for them.

The same review surfaced one honest residual: under artificial 8x
oversubscription these race oracles flip slightly more often than master's
(6/160 vs 1/160). Instrumentation shows the failing sub-assertion is the
unchanged `prepark` precondition (`progress=1 completed=1 prepark=1 ready=1`),
i.e. the migration did not change what the wait observed; the oracle was left
untouched because relaxing it under load would weaken what it proves. A
separate pre-existing hang (`backend_conformance_e1_threadpool_test` under load
inside `pre_execution_cancel_wins_with_known_zero_effect`) is reproducible on
master's own binary and is not attributable to this phase.

A second, fresh independent review (Wave 5) attacked this closeout itself and
returned `REQUEST_REVISIONS`: two P1s — the two un-dispositioned consumers
above, which made the `M_CONSUMER_MIGRATION_OR_ISOLATION` claim untrue for
them — plus accuracy findings (a stale "eleven units" count, four more
un-dispositioned census rows, the packet/D-13 mapping, T2-11's misattributed
row, T2-02's unbacked mutant-kill wording, stale evidence citations, the
mislabeled UTC timestamps, and the check-run count). The P1s and the
record-scoped findings are fixed by PR #471; the remaining findings are fixed
in this revision. The re-review verdict on this revised closeout is recorded in
the JSON `goal_states.M_FINAL_INDEPENDENT_REVIEW`.

## Evidence

| Evidence | Result | Artifact |
|----------|--------|----------|
| noliburing release, full suite | 50/50 targets pass | `/tmp/m-residual-noliburing-release.log` |
| noliburing debug, full suite | 50/50 pass | `/tmp/m-residual-noliburing-debug.log` |
| liburing release (liburing 2.9 prefix), full suite | 64/64 pass | `/tmp/m-residual-liburing-release.log` |
| ASan+UBSan, migrated suites, both profiles | clean | `/tmp/m-residual-asan-<suite>.log` |
| F1 frozen reproduction, both profiles | all gates PASS at `1032317e` | `/tmp/m-residual-f1-noliburing.json`, `/tmp/m-residual-f1-liburing.json` |
| Comment-authority guard + F1 provenance self-tests | OK / 10 tests OK | — |
| uring cutover mutation builds (8 named) | all killed; baseline 21/21 | `/tmp/m-residual/uring_cutover_mut_*.log` |
| B-02 fault injection mutant | oracle killed the mutant | PR #468 body |
| Four apps, both profiles | build/link + smoke, byte-identical copy | PR #468 body |
| Pinned stress after the T2-12 carve-out | 0 failures / 30 runs | PR #468 body, T2-12 evidence |

Earlier logs from the #468/#469 rounds (`/tmp/m-ev-*.log`,
`/tmp/f1-check-*.json`) are retained in the tier-2 record's per-item evidence
lists. This closes B-02 (seam wired) and B-03 on this host (liburing 2.9
reconstructed at the frozen prefix; liburing-profile build, link, suite and
frozen reproduction all pass). B-01 was not triggered and its reproduction is
proven. B-04/B-05/B-06/B-07 stay open with their owners.

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

1. The rulings D2, D3a, D3b and D4 from the Decision Packet plus the D-13
   package decision from its addendum, then the F3/F4 execution arms they
   authorize.
2. M-R's PRs (#468, #469, #471) merged after independent review, with the
   family gate row backfilled on #458 (done; see the #458 comment).
3. The Wave-5 fresh independent review verdict on this closeout (recorded in
   the JSON).
