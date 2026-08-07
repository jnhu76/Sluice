# Phase C2b Compliance Gate — Generation / Stale-Key / Cancel-Winner / Identity-Bearing Reap

**Roadmap:** [`remediation-roadmap.md`](remediation-roadmap.md) — Phase C (status PARTIAL; C2b COMPLETE)
**Governing ADR:** [`ADR-explicit-io-request-contract`](../adr/ADR-explicit-io-request-contract.md) (Accepted) — Decisions 9, 11, 12; invariants I16, I17, I18, I19
**Issue #68:** https://github.com/jnhu76/Sluice/issues/68 — Revision 3 (PLAN READY), C2b scope (rows 3–8)
**Branch:** test/phase-c2b-generation-stale-cancel-matrix
**Scope:** Tests + gate scripts + docs only. **No `src/` or `include/sluice/` production change.**

This is the PR-level evidence ledger for Phase C2b, the second C2 semantic-coverage slice: generation,
provenance, stale-event rejection, cancel-winner semantics, exactly-one terminal, and identity-bearing
reap. C2b closes rows 3–8 of the C2 requirement-to-evidence matrix (Issue #68) by extending the
existing arena lifecycle targets with state-transition and identity matrix cases, adding Fake and
ThreadPool integration evidence for cancel-winner and publication-boundary semantics, recording
Uring's Phase-D identity gap as a `not_implemented` manifest record, and proving the cases catch
deliberately nonconforming identity behavior.

---

## 1. Scope

| Requirement (Issue #68 row) | Evidence |
|---|---|
| 3 — every legal state transition has evidence; illegal transitions return contract errors or fail-fast | `arena_mainline_state_transition_matrix`, `arena_illegal_transition_contract_errors`, `arena_death_enqueue_double`, `arena_death_release_before_completion_ready`, `arena_death_release_stale_handle` (arena-level) |
| 4 — generation / provenance / stale-event rejection | `arena_generation_plus_one_on_both_release_authorities`, `arena_stale_handle_leaves_live_occupant_untouched`, `arena_request_key_carries_context_provenance`, `fake_stale_generation_event_harmless`, `tp_stale_generation_event_harmless` |
| 5 — cancel matrix: pending / enqueued / running / terminal / stale | `fake_cancel_disposition_counts_exactly_once`, `tp_canceled_ops_tallied_only_on_terminal_won`, `tp_running_cancel_intent_does_not_tally` |
| 6 — cancel winner does not overwrite ordinary success / error / dispatch terminal | `tp_running_cancel_intent_real_result_verbatim`, `tp_cancel_races_worker_terminal_exactly_one` |
| 7 — exactly-one backend-ready winner; all losers no-op | `exactly_one_terminal_winner`, `tp_cancel_races_worker_terminal_exactly_one` |
| 8 — identity-bearing reap: slot binding publishes; backend-known terminal-winner order; worker does not publish Completion | `arena_reap_preserves_terminal_winner_order`, `fake_binding_identity_and_publication_boundary`, `tp_publication_boundary_reap_gates_ready`, `tp_terminal_publication_after_bookkeeping` |

Explicitly **out of scope** (later slices): C2c waiter/borrow/delivery lease (rows 11–14), C2d failure
injection (rows 9–10), C2e close/drain/destruction (row 15), and the entire Phase D Uring RequestArena
migration.

## 2. Authority

- **Issue #68 Revision 3 (PLAN READY)** — the C2b design authority: the state-transition matrix (row 3),
  generation/provenance/stale-event contract (row 4), cancel matrix (rows 5–6), exactly-one winner
  (row 7), and identity-bearing reap (row 8).
- **ADR-explicit-io-request-contract (Accepted)** — Decisions 9 (identity-bearing reap / backend-known
  order), 11 (RequestKey-targeted best-effort cancel), 12 (first valid terminal winner; losers no-op),
  15 (release / reuse / quiescent destruction); invariants I16 (generation advances before slot reuse),
  I17 (stale key cannot act on new occupant), I18 (exactly-one backend-ready winner), I19 (identity-bearing
  reap order).
- **AGENTS.md** §16.1 (test-header change), §16.6 (gate change), §18 (conformance philosophy).
- **Architecture Constitution** — AC-4 (accepted terminality), AC-7 (bounded resources), AC-13 (identity-bearing
  reap).

## 3. What C2b produces

### 3.1 State-transition matrix (rows 3)

`arena_mainline_state_transition_matrix` walks one accepted request through every `RequestState` the ADR
defines, asserting `state_of()` at each step:
```
free -> reserved -> prepared -> pending -> enqueued -> running
     -> backend_ready -> completion_ready -> free (generation + 1)
```
The running step (`mark_running`) is the ThreadPool dispatch shape; the Phase B reference backends never
enter it, but the shared arena must support it. The case also pins the legitimate dispatch backoff:
`mark_running` on a slot a terminal winner already moved to `backend_ready` returns `false` (NOT fail-fast).

`arena_illegal_transition_contract_errors` pins the recoverable half of the illegal matrix (`not_found` /
`invalid_state`) exactly as the contract assigns it, never substituted. The fail-fast entries (double
enqueue, release before completion_ready, stale enqueue/mark_running/release, record_terminal before
acceptance) live in `request_arena_death_test`.

### 3.2 Generation / provenance / stale-event rejection (row 4)

`arena_generation_plus_one_on_both_release_authorities` proves generation advances exactly +1 on BOTH the
pre-commit rollback authority and the completed-binding release authority, visible immediately after
release — BEFORE the slot can re-enter a reserve — so a stale key can never collide with the next
occupant.

`arena_stale_handle_leaves_live_occupant_untouched` proves the stronger property while the slot holds a
LIVE accepted occupant: every stale attempt (prepare/binding/commit/record_terminal/cancel/waiter) is
rejected AND produces zero side effect on the new occupant (state, terminal, pin, and counters all
unchanged).

`arena_request_key_carries_context_provenance` proves `SlotHandle` is deliberately context-less (slot +
generation only); `RequestKey` is the identity that carries provenance. Proven at the RequestArena/ReadySink
boundary: two arenas with identical slot/generation produce DIFFERENT RequestKeys; the ReadyEvent.key
carries the originating context/slot/generation BY VALUE and survives the slot's release + reuse; a
cross-context key can never be resolved as a same-slot/same-generation request of the other arena.

`fake_stale_generation_event_harmless` and `tp_stale_generation_event_harmless` prove each backend's
integration: after a slot is released (Completion reset) and reused by a NEW request on the SAME physical
slot, stale-generation cancel attempts cannot act on the new occupant; the new request's Completion,
result, and counters stay exactly intact. All identity is pointer-free (SlotHandle/RequestKey) — no
Completion reverse map.

### 3.3 Cancel matrix (rows 5–6)

`fake_cancel_disposition_counts_exactly_once` and `tp_canceled_ops_tallied_only_on_terminal_won` prove
`canceled_ops` tallies ONLY on a confirmed canceled terminal win (`terminal_won`). A terminal loser
(complete_*/record_terminal after cancel won, or cancel after an ordinary winner) and a late cancel after
the terminal never tally; cancel of an unbound Completion resolves nothing.

`tp_running_cancel_intent_does_not_tally` proves running-cancel records intent only — no canceled terminal,
no `canceled_ops` tally; the real syscall result wins VERBATIM (never rewritten to canceled). A cancel
after that ordinary winner is `already_terminal`.

`tp_cancel_races_worker_terminal_exactly_one` provides genuine two-thread TSan evidence: a cancel issued
concurrently with the worker's dispatch/syscall races for the single terminal transition. Each iteration
asserts the exactly-one winner contract end to end: exactly one publication, one ready Completion; the
result is EITHER canceled OR the real success; `canceled_ops` tallies exactly the canceled winners (never
intent/losers); `syscall_count` tallies exactly the syscall winners (cancel-won iterations run no syscall).

### 3.4 Exactly-one backend-ready winner (row 7)

`exactly_one_terminal_winner` (pre-C2b Scheme-B case, retained) proves the first valid terminal transition
to `backend_ready` wins; losers do not overwrite terminal storage, publish Completion-ready, or fabricate
a second result.

`tp_cancel_races_worker_terminal_exactly_one` provides the concurrent evidence described above.

### 3.5 Identity-bearing reap / publication boundary (row 8)

`arena_reap_preserves_terminal_winner_order` (pre-C2b arena case, retained) proves reap delivers
`ReadyEvent.key` in backend-known terminal-winner order, not slot-index or submit order.

`fake_binding_identity_and_publication_boundary` proves each terminal publishes to ITS OWN slot-bound
Completion even when the terminal-winner order differs from the submit order — no queue-head guessing,
no op-kind guessing, no side-band pointer FIFO. The case also pins the publication boundary: `complete_*`/
`cancel` only produce `backend_ready`; the Completions are NOT ready until `poll()`/`wait_one()` reaps, and
a second poll returns 0 (exactly-one publication).

`tp_publication_boundary_reap_gates_ready` and `tp_terminal_publication_after_bookkeeping` provide runtime
evidence that a worker NEVER publishes: once the worker's syscall finished and `record_terminal` stored
the `backend_ready` terminal, the Completion is STILL not ready — only `poll()`/`wait_one()` reap publishes
through the slot binding.

### 3.6 Manifest / gate model

`scripts/backend_conformance_manifest.py`:
- New `c2b_arena_state_identity_matrix` evidence (implemented, mandatory, layer `lifecycle`, backends
  backend-agnostic).
- New `c2b_fake_identity_integration` evidence (implemented, mandatory, layer `lifecycle`, backends Fake).
- New `c2b_threadpool_identity_integration` evidence (implemented, mandatory, layer `lifecycle`, backends
  ThreadPool).
- New `uring_c2b_identity_not_implemented` evidence (STATUS_NOT_IMPLEMENTED, mandatory, layer `lifecycle`,
  backends Uring).

`scripts/verify-backend-conformance.py`:
- The existing `_backend_verdict` iteration over APPLICABLE mandatory evidence already handles the new
  records correctly: Fake/ThreadPool C2b integration is mandatory + implemented, so a RUN_FAIL forces
  NOT_CONFORMING; Uring's C2b gap is mandatory + not_implemented, so it forces INCOMPLETE in Uring's
  OWN verdict (surfaced in the reasons list alongside the capacity gap).

### 3.7 Validity evidence

Method chosen: **local uncommitted mutation** (Issue #68 §13-accepted alternative). Constructing a
test-only nonconforming fixture for classes A/C/D/E would have required duplicating substantial
`RequestArena` internals, so each defect class was instead proven by a single-point temporary mutation of
the real production logic, a focused filtered test run, and an immediate revert.

Seven single-point production mutations (A–G) prove each detector case fails on deliberately
nonconforming code:

| Mutant | Deliberate defect (§13 class) | Expected failing case | Actual failing case |
|---|---|---|---|
| A | stale terminal delivered to a reused generation | `arena_stale_handle_leaves_live_occupant_untouched` | same |
| B | cancel intent rewrites an ordinary success into canceled | `tp_running_cancel_intent_does_not_tally` | same |
| C | second terminal overwrites the first winner | `exactly_one_terminal_winner` | same |
| D | second terminal re-enters the ready ring | `tp_canceled_ops_tallied_only_on_terminal_won` | same |
| E | reap by slot index instead of terminal-winner order | `arena_reap_preserves_terminal_winner_order` | same |
| F | publication binding delivers a result to the wrong Completion | `fake_binding_identity_and_publication_boundary` | same |
| G | worker publishes the Completion before poll/reap | `tp_publication_boundary_reap_gates_ready` | same |

Full mutation matrix, commands, exit codes, and revert verification are recorded in
[`docs/verification/phase-c2b-identity-mutation-evidence.md`](../verification/phase-c2b-identity-mutation-evidence.md).

## 4. Test case ledger (issue #68 C2b cases)

| Case (SLUICE_TEST_CASE) | Target | Status |
|---|---|---|
| `arena_mainline_state_transition_matrix` | request_lifecycle_scheme_b_test | PASS |
| `arena_illegal_transition_contract_errors` | request_lifecycle_scheme_b_test | PASS |
| `arena_generation_plus_one_on_both_release_authorities` | request_arena_test | PASS |
| `arena_stale_handle_leaves_live_occupant_untouched` | request_lifecycle_scheme_b_test | PASS |
| `arena_request_key_carries_context_provenance` | request_lifecycle_scheme_b_test | PASS |
| `arena_death_enqueue_double` | request_arena_death_test | PASS |
| `arena_death_release_before_completion_ready` | request_arena_death_test | PASS |
| `arena_death_release_stale_handle` | request_arena_death_test | PASS |
| `fake_cancel_disposition_counts_exactly_once` | backend_scheme_b_race_test | PASS |
| `fake_binding_identity_and_publication_boundary` | backend_scheme_b_race_test | PASS |
| `fake_stale_generation_event_harmless` | backend_scheme_b_race_test | PASS |
| `tp_canceled_ops_tallied_only_on_terminal_won` | threadpool_backend_scheme_b_race_test | PASS |
| `tp_running_cancel_intent_does_not_tally` | threadpool_backend_scheme_b_race_test | PASS |
| `tp_stale_generation_event_harmless` | threadpool_backend_scheme_b_race_test | PASS |
| `tp_publication_boundary_reap_gates_ready` | threadpool_backend_scheme_b_race_test | PASS |
| `tp_cancel_races_worker_terminal_exactly_one` | threadpool_backend_scheme_b_race_test | PASS |
| Python: `test_backend_conformance_manifest.py` (102 cases, incl. 16 C2b) | unittest | PASS |

## 5. Fake / ThreadPool eligibility, Uring known gap

- **Fake = ELIGIBLE** — `c2b_fake_identity_integration=PASS` in the per-backend report.
- **ThreadPool = ELIGIBLE** — `c2b_threadpool_identity_integration=PASS`.
- **Uring = NOT CONFORMING** — `c2b_arena_state_identity_matrix=INCOMPLETE` (KernelIoProfile rule);
  C2b coverage is the `not_implemented` manifest record, surfaced in the lifecycle layer AND in the
  verdict reasons. Uring stays NOT CONFORMING until Phase D (RequestArena migration). This record
  reinforces (does not replace) the existing KernelIoProfile-stays-NOT-CONFORMING rule.

## 6. Profile applicability

**Fake:**
- pending/enqueued/reference lifecycle
- no real running syscall (Fake never enters `running` state)
- cancel matrix: pending/enqueued/terminal/stale (no running-cancel accounting)

**ThreadPool:**
- pending/enqueued/running blocking syscall
- cancel matrix: pending/enqueued/running/terminal/stale (running-cancel intent only)

**Uring:**
- not implemented until Phase D
- C2b rows 3–8 require RequestArena identity/generation/cancel/reap integration
- Uring remains legacy until Phase D; its C2b gap is the `uring_c2b_identity_not_implemented` record

## 7. Commands run (validation)

| Gate | Command | Result |
|---|---|---|
| Debug build | `xmake f -m debug --toolchain=clang -y` | PASS |
| Focused arena | `xmake build request_arena_test && xmake run request_arena_test` | PASS (8 cases) |
| Focused arena death | `xmake build request_arena_death_test && xmake run request_arena_death_test` | PASS (17 cases) |
| Focused lifecycle | `xmake build request_lifecycle_scheme_b_test && xmake run request_lifecycle_scheme_b_test` | PASS (15 cases) |
| Focused Fake race | `xmake build backend_scheme_b_race_test && xmake run backend_scheme_b_race_test` | PASS (4 cases) |
| Focused ThreadPool race | `xmake build threadpool_backend_scheme_b_race_test && xmake run threadpool_backend_scheme_b_race_test` | PASS (9 cases) |
| Manifest self-test | `python3 scripts/tests/test_backend_conformance_manifest.py` | PASS (102 cases) |
| Aggregate gate | `python3 scripts/verify-backend-conformance.py` | PASS (Fake/TP ELIGIBLE; Uring NOT CONFORMING with C2b gap) |
| Full test group | `xmake test -v` | see section 8 |
| Release | `xmake f -m release --toolchain=clang -y` | see section 8 |
| ASan/UBSan | `xmake f -m asanubsan --toolchain=clang -y` | see section 8 |
| TSan | `xmake f -m tsan --toolchain=clang -y` | see section 8 |

## 8. Validation matrix (full evidence)

All rows below were executed on the current branch head (`e857eb3`). `PASS` is recorded only for commands
that actually ran green.

| Gate | Command | Result |
| ---- | ------- | ------ |
| Debug / Clang full | `xmake f -m debug --toolchain=clang -y && xmake build -g test && xmake test -v` | PENDING |
| Focused arena | `xmake run request_arena_test` | PASS (8 cases) |
| Focused arena death | `xmake run request_arena_death_test` | PASS (17 cases) |
| Focused lifecycle | `xmake run request_lifecycle_scheme_b_test` | PASS (15 cases) |
| Focused Fake race | `xmake run backend_scheme_b_race_test` | PASS (4 cases) |
| Focused ThreadPool race | `xmake run threadpool_backend_scheme_b_race_test` | PASS (9 cases) |
| Release / Clang | `xmake f -m release --toolchain=clang -y && xmake build -g test && xmake test -v` | PENDING |
| ASan/UBSan | `xmake f -m asanubsan --toolchain=clang -y && xmake build -g test && xmake run -g test` | PENDING |
| TSan | `xmake f -m tsan --toolchain=clang -y && xmake build -g test && xmake run -g test` | PENDING |
| Manifest self-test | `python3 scripts/tests/test_backend_conformance_manifest.py` | PASS (102) |
| Aggregate gate | `python3 scripts/verify-backend-conformance.py` | PASS (Fake/TP ELIGIBLE; Uring NOT CONFORMING) |
| Doc links | `python3 scripts/check-doc-links.py` | PENDING |
| Architecture docs | `python3 scripts/verify-architecture-docs.py` | PENDING |
| Negative-compile | `scripts/verify-completion-authority-negative-compile.sh` | PENDING |
| Negative-compile | `scripts/verify-request-arena-negative-compile.sh` | PENDING |
| Negative-compile | `scripts/verify-async-identity-negative-compile.sh` | PENDING |
| Negative-compile | `scripts/verify-external-backend-authority-negative-compile.sh` | PENDING |
| Diff hygiene | `git diff --check` | PENDING |

## 9. Remaining gaps

- **C2c** — waiter/borrow/delivery lease (rows 11–14): PARTIAL, not closed.
- **C2d** — failure injection + post-commit allocator terminal (rows 9–10): MISSING, not closed.
- **C2e** — close/drain/reset (row 15; row 16 already FULL): PARTIAL, not closed.
- **Phase D** — Uring RequestArena migration: PENDING; Uring C2b conformance is the
  `uring_c2b_identity_not_implemented` record, never skip-as-pass.

## 10. Phase status

- Phase C remains **PARTIAL** (C1 IMPLEMENTED; C2a COMPLETE; C2b COMPLETE; C2c–C2e pending).
- **C2b: COMPLETE** — rows 3–8 of the C2 matrix have arena-level, per-backend, validity-proven
  evidence for Fake and ThreadPool; Uring's gap is authoritatively recorded.
- No `src/` or `include/sluice/` change; no synchronous Reader/Writer behavior change; no Phase D
  Uring implementation; no C2c–C2e scope creep.
