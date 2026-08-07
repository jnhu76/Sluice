# Phase C2b Identity / Generation / Cancel-Winner Mutation Evidence

This document records the RED-validity evidence (Issue #68 §13) for the Phase C2b
test suite (rows 3–8: state-transition matrix, generation/stale-key rejection,
cancel-winner semantics, exactly-one terminal, binding identity, and the
publication boundary). Every C2b test group must be proven able to FAIL on
deliberately nonconforming code — green-only tests are not proof.

Method chosen: **local uncommitted mutation** (the §13-accepted alternative).
Constructing a test-only nonconforming fixture for classes A/C/D/E would have
required duplicating substantial `RequestArena` internals, so each defect class
was instead proven by a single-point temporary mutation of the real production
logic, a focused filtered test run, and an immediate revert.

All commands ran on branch `test/phase-c2b-generation-stale-cancel-matrix` at
HEAD `e857eb3` (master base `37298f0`). Toolchain: **Clang 21.1.8
(6ubuntu1)**, xmake **v3.0.9**, `xmake f -m debug --toolchain=clang -y`.

## Method

For each mutant A–G:

1. apply ONE single-point mutation to the production logic;
2. rebuild the affected test target (`xmake build -j 6 <target>`);
3. run exactly the detector case
   (`SLUICE_TEST_FILTER=<case> xmake run <target>`);
4. record the expected case, the actual failing case, the command, and the exit
   code;
5. revert the mutation;
6. after all mutants, confirm `git diff` is EMPTY (no mutation entered a
   commit) and re-run all four affected targets to ALL TESTS PASSED.

Exit-code note: `xmake run` reports 255 when the child fails. Harness assertion
failures exit 1 (`failed(1)`); arena fail-fast invariants call `std::terminate`
(`failed(-1)`). Both are recorded as 255 below because that is what the
recording command (`echo EXIT=$?` after `xmake run`) observed.

## Mutation matrix

| Mutant | Deliberate defect (§13 class) | Mutation applied | Expected failing case | Actual failing case / failure mode | Command | Exit |
| --- | --- | --- | --- | --- | --- | --- |
| A | stale terminal delivered to a reused generation | `RequestArena::record_terminal` (`include/sluice/async/detail/request_arena.hpp`): replace `validate_(h)` with an index-only slot lookup that ignores the handle generation | `arena_stale_handle_leaves_live_occupant_untouched` | same — the stale `record_terminal` lands on the generation+1 occupant; the case's rejection check fails and the skipped cleanup trips the arena fail-fast (`std::terminate`) | `SLUICE_TEST_FILTER=arena_stale_handle_leaves_live_occupant_untouched xmake run request_lifecycle_scheme_b_test` | 255 |
| B | cancel intent rewrites an ordinary success into canceled | `ThreadPoolBackend::cancel(Completion<std::size_t>&)` (`src/async/threadpool_backend.cpp`): also tally `canceled_ops` on `intent_recorded` | `tp_running_cancel_intent_does_not_tally` | same — harness assertion `intent_recorded must NOT tally canceled_ops` (`tests/threadpool_backend_scheme_b_race_test.cpp:597`) fails; real success still wins verbatim but the counter is corrupted | `SLUICE_TEST_FILTER=tp_running_cancel_intent_does_not_tally xmake run threadpool_backend_scheme_b_race_test` | 255 |
| C | second terminal overwrites the first winner | `RequestArena::record_terminal`: when `terminal_.stored`, overwrite the stored result before returning false | `exactly_one_terminal_winner` | same — reap publishes the loser's `backend_error` instead of the winner's 42 bytes; the published-result check fails and the skipped release trips the arena fail-fast | `SLUICE_TEST_FILTER=exactly_one_terminal_winner xmake run request_lifecycle_scheme_b_test` | 255 |
| D | second terminal re-enters the ready ring | `RequestArena::cancel`: the `already_terminal` branch additionally calls `push_ready_locked_` | `tp_canceled_ops_tallied_only_on_terminal_won` | same — the late (already-bound) second cancel re-pushes a slot that is still the ring tail; the ready-ring invariant guard fail-fasts (`std::terminate`) exactly as designed | `SLUICE_TEST_FILTER=tp_canceled_ops_tallied_only_on_terminal_won xmake run threadpool_backend_scheme_b_race_test` | 255 |
| E | reap by slot index instead of terminal-winner order | `RequestArena::push_ready_locked_`: insert the ring entry in ascending slot-index order instead of appending at the tail | `arena_reap_preserves_terminal_winner_order` | same — reap delivers slot 0 before the earlier winner slot 1; the delivery-order check (`tests/request_arena_test.cpp:291`) fails | `SLUICE_TEST_FILTER=arena_reap_preserves_terminal_winner_order xmake run request_arena_test` | 255 |
| F | publication binding delivers a result to the wrong Completion | `RequestArena::reap`: defer publication, then swap the stored results of the first two reaped slots before invoking the bindings (each result lands on the other slot's Completion) | `fake_binding_identity_and_publication_boundary` | same — misbound `ca` receives the canceled terminal; harness assertion `ca.result().has_value()` (`tests/backend_scheme_b_race_test.cpp:185`) fails | `SLUICE_TEST_FILTER=fake_binding_identity_and_publication_boundary xmake run backend_scheme_b_race_test` | 255 |
| G | worker publishes the Completion before poll/reap | `RequestArena::record_terminal`: invoke the slot's publication binding immediately after storing the terminal (before any reap) | `tp_publication_boundary_reap_gates_ready` | same — the Completion is ready the instant the worker records the terminal; the case's `!c.ready()` publication-boundary check fails and the skipped reap trips the Completion/cleanup fail-fast | `SLUICE_TEST_FILTER=tp_publication_boundary_reap_gates_ready xmake run threadpool_backend_scheme_b_race_test` | 255 |

## Revert verification

After mutant G was reverted:

- `grep -rn "VALIDITY MUTATION" include/ src/ tests/` → zero matches;
- `git status --short` and `git diff --stat` → empty working tree (no mutation
  entered any commit);
- re-run of all four affected targets (Clang Debug) → `ALL TESTS PASSED`:
  `request_arena_test`, `request_lifecycle_scheme_b_test`,
  `backend_scheme_b_race_test`, `threadpool_backend_scheme_b_race_test`.

## Conformance-control pairing

Each mutant's detector case has a conforming twin that passes on the real
(unmutated) code, so the suite is not merely "fails on broken code" but asserts
the positive contract:

- A ↔ `arena_stale_handle_leaves_live_occupant_untouched`,
  `fake_stale_generation_event_harmless`,
  `tp_stale_generation_event_harmless` (all green);
- B ↔ `tp_running_cancel_intent_does_not_tally` and
  `tp_running_cancel_intent_real_result_verbatim` (green);
- C ↔ `exactly_one_terminal_winner` and
  `tp_cancel_races_worker_terminal_exactly_one` (green);
- D ↔ `tp_canceled_ops_tallied_only_on_terminal_won` and
  `fake_cancel_disposition_counts_exactly_once` (green);
- E ↔ `arena_reap_preserves_terminal_winner_order` (green);
- F ↔ `fake_binding_identity_and_publication_boundary` (green);
- G ↔ `tp_publication_boundary_reap_gates_ready` and
  `tp_terminal_publication_after_bookkeeping` (green).
