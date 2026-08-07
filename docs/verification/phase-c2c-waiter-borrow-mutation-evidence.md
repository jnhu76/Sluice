# Phase C2c Waiter / Borrow / Delivery-Lease Mutation Evidence

This document records the RED-validity evidence (Issue #68 §13) for the Phase
C2c test suite (rows 11–14: fd/buffer borrow lifetime, single-waiter
registration, waiter-cancel independence, and the move-only delivery lease).
Every C2c detector case must be proven able to FAIL on deliberately
nonconforming code — green-only tests are not proof.

Review-round correction: the C2c slice was extended with (a) the ADR-corrected
registration window (pending/enqueued/running/backend_ready — the terminal
winner does NOT close registration, only reap does), which redefines mutant G
from "registration allowed after terminal" (now LEGAL) to "registration
allowed after reap"; and (b) the I18 publication-order detector (mutant I),
because a borrow-end-after-publication defect is invisible to post-reap borrow
observations alone.

Method chosen: **local uncommitted mutation** (the §13-accepted alternative,
same as C2b). Constructing a test-only nonconforming fixture for these defect
classes would require duplicating substantial `RequestArena` internals, so
each defect class was instead proven by a single-point temporary mutation of
the real production logic in `include/sluice/async/detail/request_arena.hpp`,
a focused filtered test run, and an immediate restore.

All commands ran on the test/phase-c2c-borrow-waiter-delivery-lease branch
(master base `ad5a057`). Toolchain: **Clang**, xmake, `xmake f -m debug
--toolchain=clang -y`.

## Method

For each mutant A–I:

1. apply ONE single-point mutation to the production logic
   (exact string replacement — the review-round re-run used
   `/tmp/c2c_mutate_review.py`; the mutations are listed in the matrix below);
2. rebuild the affected test target (`xmake build <target>`);
3. run exactly the detector case
   (`SLUICE_TEST_FILTER=<case> xmake run <target>`);
4. record the expected case, the actual failing case, the command, and the
   exit code;
5. restore the file from a pre-mutation snapshot of the CURRENT working tree
   (the C2c guarded seams and the register_waiter window correction — never
   from git, so uncommitted work is never lost);
6. after all mutants, confirm `grep -c MUTANT include/` is zero and `git diff
   include/ src/` shows only the intended C2c changes (guarded seams + the
   register_waiter window correction).

Exit-code note (same as C2b): `xmake run` reports 255 when the child fails.
Harness assertion failures exit non-zero; arena fail-fast invariants call
`std::terminate` (the destructor fail-fast fires when the broken invariant
leaves a slot in use at case scope exit — the standard repo mechanism for a
failed case, after the case's own assertion already recorded the violation).
The ThreadPool/Fake cases print the FAILED line with the intended message
before cleanup.

## Mutation matrix

| Mutant | Deliberate defect (§13 class) | Mutation applied (`request_arena.hpp`) | Expected failing case | Actual failing case / failure mode | Command | Exit |
| --- | --- | --- | --- | --- | --- | --- |
| A | borrow begins at prepare (I7: borrow starts only at commit) | `prepare()`: `s->borrow_.active = true;` instead of `= false` | `arena_borrow_lifecycle_full_matrix` | same — the case's `prepare must NOT begin the borrow` check fires; the skipped cleanup trips the arena destructor fail-fast (`std::terminate`) | `SLUICE_TEST_FILTER=arena_borrow_lifecycle_full_matrix xmake run request_waiter_borrow_lease_test` | 255 |
| B | borrow ends at record_terminal / backend_ready (worker-syscall-end == borrow-end; the C2c false-green trap) | `record_terminal()`: `s->borrow_.active = false;` after the backend_ready transition | `tp_backend_ready_borrow_still_active_before_reap` | same — visible FAILED line: `borrow must still be active with exact metadata at backend_ready` (`threadpool_backend_c2c_waiter_borrow_test.cpp:388`); the case then drains and reports via `SLUICE_FAIL` | `SLUICE_TEST_FILTER=tp_backend_ready_borrow_still_active_before_reap xmake run threadpool_backend_c2c_waiter_borrow_test` | 255 |
| C | second waiter overwrites the first (the no-overwrite false-green trap) | `register_waiter()`: delete the `open_registered -> invalid_state` guard | `arena_single_waiter_first_registration_survives` | same — the second registration now succeeds; the case's `!r2.has_value() && invalid_state` check fires; skipped cleanup trips the destructor fail-fast | `SLUICE_TEST_FILTER=arena_single_waiter_first_registration_survives xmake run request_waiter_borrow_lease_test` | 255 |
| D | wait-cancel also cancels the I/O (stolen I/O authority) | `cancel_waiter()`: additionally store `TerminalResult::err(canceled)` and transition to backend_ready | `arena_waiter_cancel_removes_only_the_waiter` | same — the case's `state must still be enqueued` / `no terminal stored` checks fire; skipped cleanup trips the destructor fail-fast | `SLUICE_TEST_FILTER=arena_waiter_cancel_removes_only_the_waiter xmake run request_waiter_borrow_lease_test` | 255 |
| E | duplicate lease: cancel_waiter returns the lease but leaves `waiter_delivery_present_` set, so reap re-delivers | `cancel_waiter()`: delete `s->waiter_delivery_present_ = false;` | `arena_cancel_waiter_vs_reap_race` | same — every iteration delivers BOTH (cancel returns the lease AND the event carries a lease), violating the XOR ownership invariant `cancel_won != reap_delivered`; skipped cleanup trips the destructor fail-fast | `SLUICE_TEST_FILTER=arena_cancel_waiter_vs_reap_race xmake run request_waiter_borrow_lease_test` | 255 |
| F | lease dropped: reap closes registration but does not move the lease into the ReadyEvent | `reap()`: `if (false && s.waiter_delivery_present_)` (delivery block disabled) | `arena_lease_transfer_chain_reap_path` | same — the case's `sink.deliveries[0].lease_id == 42` check fires; skipped cleanup trips the destructor fail-fast | `SLUICE_TEST_FILTER=arena_lease_transfer_chain_reap_path xmake run request_waiter_borrow_lease_test` | 255 |
| G | registration allowed after reap (completion_ready accepted — the window must end at reap, NOT at terminal: "registration after terminal" is legal per ADR Decision 10) | `register_waiter()`: state guard narrowed to reject only reserved/prepared (completion_ready accepted) | `arena_waiter_registration_state_matrix` | same — registration succeeds on the reaped slot; the case's completion_ready `invalid_state` check fires; skipped cleanup trips the destructor fail-fast | `SLUICE_TEST_FILTER=arena_waiter_registration_state_matrix xmake run request_waiter_borrow_lease_test` | 255 |
| H | stale waiter authority acts on a reused occupant (generation validation bypassed) | `cancel_waiter()`: replace `validate_(h)` with an index-only slot lookup that ignores the handle generation | `fake_stale_waiter_authority_harmless` | same — the stale cancel resolves the LIVE N+1 occupant and takes its lease; the case's `stale cancel_waiter must resolve to not_found` check fires; skipped cleanup trips the destructor fail-fast | `SLUICE_TEST_FILTER=fake_stale_waiter_authority_harmless xmake run backend_c2c_waiter_borrow_test` | 255 |
| I | borrow ends AFTER the Completion-ready publication but still inside reap (I18 order violation — invisible to post-reap borrow observations) | `reap()`: `s.borrow_.active = false;` (with its trace point) moved AFTER `publication_binding_.publish(...)` | `arena_borrow_publication_order` | same — the I18 trace shows publish_seq < borrow_end_seq; the case's `publish_seq > borrow_end_seq` check fires; skipped cleanup trips the destructor fail-fast | `SLUICE_TEST_FILTER=arena_borrow_publication_order xmake run request_waiter_borrow_lease_test` | 255 |

## Revert verification

After mutant I was restored (the last mutant applied):

- `grep -c "MUTANT" include/sluice/async/detail/request_arena.hpp` → 0 (no
  mutation marker remains);
- `git diff --stat include/ src/` → only the guarded seam headers
  (`request_arena.hpp` +54, `reference_ready_sink.hpp` +22,
  `fake_backend.hpp` +86, `threadpool_backend.hpp` +88) plus the
  `register_waiter` window correction (same file); no other production logic
  change;
- re-run of all three affected targets (Clang Debug) → `ALL TESTS PASSED`
  (14 + 6 + 7 cases).

## Conformance-control pairing

Each mutant's detector case has a conforming twin that passes on the real
(unmutated) code:

- A ↔ `arena_borrow_lifecycle_full_matrix`,
  `tp_backend_ready_borrow_still_active_before_reap` (green — commit owns,
  reap releases);
- B ↔ `tp_backend_ready_borrow_still_active_before_reap` and
  `tp_running_borrow_cancel_intent_waiter_survives` (green — backend_ready
  borrow still active);
- C ↔ `arena_single_waiter_first_registration_survives` and
  `arena_waiter_registration_state_matrix` (green — first waiter survives);
- D ↔ `arena_waiter_cancel_removes_only_the_waiter`,
  `fake_wait_cancel_keeps_io`, `tp_wait_cancel_keeps_io` (green — wait-cancel
  never steals I/O authority);
- E ↔ `arena_cancel_waiter_vs_reap_race`,
  `arena_lease_transfer_chain_wait_cancel_path` (green — exactly-one owner);
- F ↔ `arena_lease_transfer_chain_reap_path`,
  `fake_borrow_waiter_delivery_integration` (green — lease delivered);
- G ↔ `arena_waiter_registration_state_matrix` (green — per-state rejection:
  reserved/prepared/completion_ready `invalid_state`; pending/enqueued/running/
  backend_ready success);
- H ↔ `fake_stale_waiter_authority_harmless`,
  `tp_stale_waiter_authority_harmless` (green — stale authority harmless
  against a live N+1 occupant);
- I ↔ `arena_borrow_publication_order` and `arena_borrow_lifecycle_full_matrix`
  (green — borrow ends BEFORE the Completion-ready publication; an acquire
  observer of ready sees the ended borrow).
