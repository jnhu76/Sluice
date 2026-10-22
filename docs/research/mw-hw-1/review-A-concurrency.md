# MW-HW-1 Review A — Semantic / Concurrency (independent)

Reviewer: isolated adversarial agent; scope E1 (primary) + E2/E3 (secondary).
Baseline `8e2aa5ab`, candidate `9af4d46a` (later `eda0409c` adds the review's
P3 scenario). The reviewer independently re-ran the parity oracle (4/4 at the
time) and both mutants (both killed).

## Findings (reviewer's own words condensed; severities theirs)

1. Transition-set completeness — PROVEN SOUND. Complete enumeration of
   `control_pending` writes and RouterEntry resets; every count transition is
   paired with the flag transition inside the same `dispatch_mtx_` critical
   section. Supporting invariant proven from code:
   `control_state != none ⟹ control_pending == false`; hence
   `pending_control_count_ == #{in_use ∧ control_pending}` at every lock
   release, and the zero-skip is semantically identical to master's empty
   scan. Fresh free-list slots always enter with the flag false, so
   `dispatch_one_locked`'s reset can never destroy an uncounted intent.
2. Lost sticky cancellation — PROVEN SOUND. The skip can hide an intent only
   if flag=true ∧ count=0, excluded by the invariant. Races checked: public
   re-cancel while pending (legal: RequestCore reports `requested` on every
   call while execution is claimed; the no-double-increment guard holds);
   cancel racing CQE finalization; cancel racing poison/retirement;
   concurrent cancel from another thread (all transitions under one mutex).
3. Underflow/overflow — PROVEN SOUND. Decrements guarded by flag==true,
   increments by flag==false; double-retire impossible (fail-fast on
   !in_use). Failure asymmetry noted: too-large count ⇒ scan still runs
   (master behavior, safe); too-low count is exactly the missing-increment
   mutant, killed by the oracle.
4. Lock discipline — PROVEN SOUND. Both service callers, all
   issue/retire callers, and both test seams hold `dispatch_mtx_`.
5. Shutdown/settlement — PROVEN SOUND. `internal_work_retired()` and the
   destructor never read the count or the flag; a residual nonzero count
   implies a live route ⇒ `live_cookies_ != 0` ⇒ identical fail-fast.
   BACKEND-04 respected.
6. W-03 external loop — PROVEN SOUND. `poll_progress` verdicts do not
   involve `control_pending`; retry advertising for a sticky intent is
   carried by `dispatch_retry_remains_locked_` (SQE pressure ⇒ non-empty
   ledger) plus `cancel_key`'s signal.
7. ABA/generation — PROVEN SOUND. Transitions keyed on the entry's current
   flag under one mutex hold; retirement clears before free-list return;
   recycled slots start flag-false.
8. [P3] Parity-test gaps: no *public* double-cancel scenario; no
   poison-with-pending scenario (same decrement path, structurally gated by
   fatal_error_); no multi-thread cancel test (mutex argument; same evidence
   level as the merged owed_count_ precedent). → **Addressed this round**:
   `repeated_cancel_does_not_double_count` added (5/5 pass, commit
   `eda0409c`); the other two remain productization-slice conditions.
9. [P3] Mutant-kill attribution: missing-increment is killed by the
   nonterminal-responsibility fail-fast during the early-return unwind, not
   by the parity assertion print. → Recorded in performance.md §3.
10. [P3] Defensive guard at the `control_state != none` clear site guards an
    unreachable state in production builds (mirrors master's unconditional
    no-op write). → Recorded as a mechanism-justification note for the
    productization slice.
11. [P3] E2 audit: semantically equivalent in every traced interleaving;
    reservation-undo is master's own trick inside one critical section; the
    direct path cannot newly poison; NO_CHANGE stands on cost, not
    correctness.
12. [P3] E3 audit: the shared member `publish_batch_` is safe only because
    the root contract outlaws concurrent/recursive drive (§13) and
    `drive_entry_admitted_` enforces it; master tolerated both locally.
    NO_CHANGE stands; if ever revived, the buffer must not be a shared
    member.

## Verdict

**E1: ACCEPT** for research status (no failing interleaving constructed;
oracle and mutants independently re-run and killed). P3 items are
evidence-hygiene conditions for the productization slice, one of which was
closed in-round. E2/E3: no blocking semantic defect; both correctly remain
NO_CHANGE.
