# Phase M final closeout

Machine-readable twin: [`m-phase-final-closeout.json`](m-phase-final-closeout.json).
This document reports the state of Phase M at the end of the execution window.
It is a report, not a contract: every claim below names the artifact that
carries it.

The tier-1 half of the M-R disposition is PR #468 (merged `9d1b5cc0`) plus the
residual migration in PR #471; the isolation half is PR #469 (merged
`667e17f8`), corrected by #471. No single PR declares the family gate closed by
itself. SHA fields: `verified_code_sha = 7f934e0f` (the head the evidence batch
verified), `closeout_merge_sha = ffb5afa8` (the merge of this closeout's PR
#470); the projection revision below carries its own merge SHA in the #458
follow-up.

## Headline

**PHASE_M = NO** — Phase M is not closed. One family (M-R) reached its gate arm
in this window; the other five families are blocked on human authority
decisions that the Decision Packet asked for and that nobody has answered (D2,
D3a, D3b, D4; M-A3 additionally needs the package decision D-13, which the
packet's four questions did not cover — the addendum on #458 now asks it; and
M-A2 needs D-12 once D3a/D3b resolve the vectored dependency — a further
addendum now records that conditional question so M-A2 cannot be silently
skipped). `F2/F3/F4_PRODUCTION_RETIREMENT = NOT_EXECUTED`;
`F1_HISTORICAL_BASELINE = IMMUTABLE` (both frozen profiles reproduce, manifests
untouched); `M_FAMILY_DISPOSITIONS = COMPLETE` as a classification (all seven
families carry a disposition; six have a downstream owner and an exit
condition, M-A4 is closed with no owner because no action is owed) — it does
not assert the decisions are done, which is tracked separately as
`M_DECISIONS_COMPLETE = NO`; `M_REQUIRED_EVIDENCE = PASS`, scoped as
`M_EXECUTED_SCOPE_EVIDENCE = PASS` (this window's executed scope only, not a
conformance verdict for undecided families);
`M_CONSUMER_MIGRATION_OR_ISOLATION = VERIFIED` for M-R after the Wave-5 review
found and PR #471 migrated the last two un-dispositioned consumers — and now
recomputable per consumer: every one of the 75 census M-R records has an
explicit final disposition in
[`m-r-consumer-disposition-final.json`](m-r-consumer-disposition-final.json),
checked mechanically by `scripts/verify_mr_disposition.py` in CI.

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
D-13 addendum and the conditional D-12 addendum on the same issue.

The M-R family's downstream owner is F2 (the Completion carrier retirement);
the twelve tier-2 units carry their own owners — T2-01 F3, T2-08 F2+F3, T2-10
F4, T2-11 F2 — as recorded per item in the tier-2 record.

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
6. **Per-consumer final-state projection.** All 75 census M-R records now carry
   an explicit disposition (MIGRATED / POLICY_ISOLATED / COMPAT_TEST_RETAINED /
   NOT_A_CONSUMER / DOWNSTREAM_MECHANISM_RETAINED) with subscope splits for the
   compound rows, in
   [`m-r-consumer-disposition-final.json`](m-r-consumer-disposition-final.json);
   `scripts/verify_mr_disposition.py` recomputes it against the census and the
   tier-2 record (coverage, exclusivity, field discipline, bidirectional
   census_rows membership) and runs in CI as the `mr-disposition-guard`
   workflow. The #459 Option A provenance is re-anchored there and in the
   tier-2 record: the standing authority is #458 §3.1/§6/§7; Option A was the
   execution-scope packaging of it, and the directive sentence its ruling
   comment cited is no longer cited as authority.

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
master's own binary and is not attributable to this phase; it is recorded as an
**open harness issue (H-1, owner F5/harness)** together with the load
sensitivity above (H-2) — the green re-run of the affected CI job does not
retire either.

A third, post-merge review covered `e23cc62d..ffb5afa8` and returned
`REQUEST_REVISIONS` with two P1s — the #459 Option A ruling's
authorization-source paragraph leaned on the Execution Lead directive's
conditional sentence instead of the standing #458 §3.1/§6 authority, and no
independently recomputable M0-census-to-final-state projection existed for the
75 M-R records — plus five P2s (the single `final_master_sha` field, the breadth
of the global PASS fields, D-12 missing from the packet, a summary line that
assigned every tier-2 exit condition to F2, and the CI-hang framing). This
revision implements the reviewer's minimal fix path: existing-authorization
traceability instead of a new ruling, the disposition overlay plus its
mechanical validator (item 6 above), the sha-field split, the scope-limiting
goal-state fields, the conditional D-12 addendum and the tier-2 owner
correction. No production path was touched; narrow re-review of this delta is
pending.

A second, fresh independent review (Wave 5) attacked this closeout itself and
returned `REQUEST_REVISIONS`: two P1s — the two un-dispositioned consumers
above, which made the `M_CONSUMER_MIGRATION_OR_ISOLATION` claim untrue for
them — plus accuracy findings (a stale "eleven units" count, four more
un-dispositioned census rows, the packet/D-13 mapping, T2-11's misattributed
row, T2-02's unbacked mutant-kill wording, stale evidence citations, the
mislabeled UTC timestamps, and the check-run count). The P1s and the
record-scoped findings are fixed by PR #471; the remaining findings are fixed
in this revision. The narrow re-review of the revised closeout returned
`APPROVE` at head `4d09d3b5` with no blocking items (five non-blocking accuracy
notes, applied); the verdict and its scope are recorded in the JSON
`goal_states.M_FINAL_INDEPENDENT_REVIEW`.

## Evidence

| Evidence | Result | Artifact |
|----------|--------|----------|
| noliburing release, full suite | 50/50 targets pass | `/tmp/m-residual-noliburing-release.log` |
| noliburing debug, full suite | 50/50 pass | `/tmp/m-residual-noliburing-debug.log` |
| liburing release (liburing 2.9 prefix), full suite | 64/64 pass | `/tmp/m-residual-liburing-release.log` |
| ASan+UBSan, migrated suites, both profiles | clean | `/tmp/m-residual-asan-<suite>.log` |
| F1 frozen reproduction, both profiles | all gates PASS | `/tmp/m-residual-f1-noliburing-gates.log`, `/tmp/m-residual-f1-liburing-gates.log` (manifest outputs `/tmp/m-residual-f1-*.json`) |
| Comment-authority guard + F1 provenance self-tests | OK / 10 tests OK | — |
| uring cutover mutation builds (8 named) | all killed; baseline 21/21 | `/tmp/m-residual/uring_cutover_mut_*.log` |
| B-02 fault injection mutant | oracle killed the mutant | PR #468 body |
| Four apps, both profiles | build/link + smoke, byte-identical copy | PR #468 body |
| Pinned stress after the T2-12 carve-out | 0 failures / 30 runs | PR #468 body, T2-12 evidence |
| M-R disposition overlay (75 census rows, 82 subscopes) + validator | total, exclusive, bidirectionally consistent; negative-tested (8 corruption modes all caught) | `docs/review/m-r-consumer-disposition-final.json`, `scripts/verify_mr_disposition.py`, CI `mr-disposition-guard` |

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

1. The rulings D2, D3a, D3b and D4 from the Decision Packet, plus the D-13
   package decision and the conditional D-12 workload-admittance question from
   their addenda, then the F3/F4 execution arms they authorize.
2. M-R's PRs (#468, #469, #471) merged after independent review, with the
   family gate row backfilled on #458 (done; see the #458 comment).
3. The Wave-5 fresh independent review verdict on this closeout (recorded in
   the JSON).
