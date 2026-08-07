# Phase C2c Compliance Gate — Waiter / Borrow / Delivery-Lease Conformance

**Roadmap:** [`remediation-roadmap.md`](remediation-roadmap.md) — Phase C (status PARTIAL; C2c COMPLETE)
**Governing ADR:** [`ADR-explicit-io-request-contract`](../adr/ADR-explicit-io-request-contract.md) (Accepted) — Decisions 8, 9, 10; invariants I7, I13, I16, I18
**Issue #68:** https://github.com/jnhu76/Sluice/issues/68 — C2c scope (rows 11–14)
**Branch:** test/phase-c2c-borrow-waiter-delivery-lease
**Scope:** Tests + test-only guarded header seams + gate scripts + docs only. **No production behavior change**: the production `RequestArena` / `RequestSlot` already implements the C2c contract (Phase B); this slice proves it at arena level, at the Fake/ThreadPool integration level, under concurrency, and against deliberate nonconforming mutations.

This is the PR-level evidence ledger for Phase C2c, the third C2 semantic-coverage slice:
**fd/buffer borrow lifetime** (row 11), **single-waiter registration** (row 12a), **waiter-cancel
independence** (row 13), and the **move-only delivery lease** (row 14a). C2c closes these rows for the
RequestArena authority layer and for the migrated Fake / ThreadPool backends, records Uring's Phase-D
gap as a `not_implemented` manifest record, and proves every detector case fails on deliberately
nonconforming code (mutants A–H).

---

## 1. Scope

| Requirement (Issue #68 row) | Evidence |
|---|---|
| 11 — fd/buffer borrow lifetime: commit owns, reap releases; survives every intermediate state and every cancel/wait-cancel path | `c2c_arena_borrow_waiter_lease_matrix` (arena), `c2c_fake_borrow_waiter_integration` (Fake), `c2c_threadpool_borrow_waiter_integration` (ThreadPool) |
| 12a — single-waiter registration + abstract delivery mechanics (C2c scope) | `arena_waiter_registration_state_matrix`, `arena_single_waiter_first_registration_survives`, `arena_register_waiter_vs_terminal_race`; Fake/TP integration cases |
| 12b — real public waiter / RequestHandle / Scheduler registration consumer (**Phase F scope**) | not in C2c — see §3.2; no public waiter API exists (ADR Decision 7/10) |
| 13 — waiter-cancel independence: wait-cancel ≠ I/O cancel; cancel_waiter vs reap exactly-once | `arena_waiter_cancel_removes_only_the_waiter`, `arena_io_cancel_keeps_waiter_registration`, `arena_cancel_waiter_vs_reap_race`; Fake/TP integration cases |
| 14a — abstract move-only delivery lease ownership / exactly-once transfer (C2c scope) | `arena_lease_type_properties`, `arena_lease_transfer_chain_reap_path`, `arena_lease_transfer_chain_wait_cancel_path`, `arena_ready_event_waiter_survives_slot_reuse`, `arena_cancel_waiter_vs_reap_race`; Fake/TP integration cases |
| 14b — real Scheduler routing-record lifetime / real lease acknowledgement (**Phase F scope**) | not in C2c — see §3.2; fake leases prove the transfer mechanics only (ADR Decision 10) |

**Row decomposition.** Like C2b's row 4a/4b split, rows 12 and 14 each conflate two distinct authority
layers, and the Accepted ADR already draws the boundary (Decision 10: "Phase B proves the abstract
transfer and exactly-once rules with fake stable tokens/leases and no Scheduler modification. Phase F
implements and proves actual Scheduler record lifetime, cancellation, drain, and shutdown
integration; Phase B alone is not that evidence."):

- **Row 12a (C2c):** the RequestSlot-level single-waiter registration authority — one registration,
  synchronous `invalid_state` for a second without overwriting the first, registration state matrix
  pinned from the as-built contract, and the abstract delivery mechanics (reap closes registration and
  moves the token/lease out exactly once, racing wait-cancel).
- **Row 12b (Phase F):** the real public waiter / RequestHandle / Scheduler registration consumer that
  will translate slot-level closed-registration into an already-ready observation. C2c adds **no
  public waiter API**.
- **Row 14a (C2c):** the abstract move-only `RoutingLease` ownership — caller → slot → ReadyEvent (or
  cancel_waiter return) with exactly one owner under every race.
- **Row 14b (Phase F):** the real Scheduler routing-record lifetime and lease acknowledgement. C2c
  never claims the fake lease proves real Scheduler lifetime.

Explicitly **out of scope** (later slices): C2d failure injection (rows 9–10), C2e close/drain/
destruction (row 15), and the entire Phase D Uring RequestArena migration.

## 2. Authority

- **Issue #68** — the C2c design authority: borrow-lifetime contract (row 11), single-waiter
  cardinality (row 12), waiter-cancel independence (row 13), delivery-lease ownership (row 14).
- **ADR-explicit-io-request-contract (Accepted)** — Decision 8 (borrow begins at commit, ends only at
  completion-ready publication; none of cancel-requested / waiter-canceled / backend-ready / CQE /
  syscall-return ends it), Decision 9 (reap closes registration, takes any delivery, publishes ready
  in the shared leaf domain; by-value ReadyEvent with no Completion/slot pointer), Decision 10
  (single-waiter registration state machine; wait-cancel removes only the waiter; reap and wait-cancel
  race to move token/lease out exactly once; Phase B proves abstract transfer, Phase F proves real
  Scheduler lifetime); invariants I7 (borrow commit → completion-ready), I13 (single waiter), I16
  (non-escaping by-value delivery), I18 (ready publication after registration closure / borrow end /
  accounting).
- **AGENTS.md** §10.1–10.7 (identity, transaction, terminal winner, reap authority, slot release),
  §15 (test-only controls), §16.1/16.6 (test-header + gate changes), §18 (conformance philosophy),
  §23 (fail-path discipline).
- **Architecture Constitution** — AC-4 (accepted terminality), AC-7 (bounded resources), AC-13
  (identity-bearing reap), AC-15 (non-escaping synchronous delivery).

## 3. What C2c produces

### 3.1 Row 11 — borrow lifecycle (arena matrix)

`arena_borrow_lifecycle_full_matrix` walks one accepted request through the FULL state walk and pins
the borrow flag and the exact fd/address/length at every window:

```text
reserved    borrow snapshot present, inactive
prepare     fd/address/length EXACT, active == false   (prepare must NOT begin the borrow)
commit      active == true                              (linearization point; I7)
pending     active
enqueued    active
running     active
backend_ready (record_terminal)  active                 (terminal known != caller may reuse buffer)
completion_ready (reap)          inactive               (I18: ready observer sees the ended borrow)
release     slot free; no observable borrow
```

`arena_borrow_survives_cancel_and_wait_cancel` proves three cancel paths never end the borrow:
Scheme-B pending cancel (enqueued-no-op + pin ack included), running cancel intent (intent only, real
result later wins verbatim), and wait-cancel. `arena_borrow_rollback_and_stale_protection` proves a
pre-commit rollback never borrows (metadata cleared, generation++) and that a stale-generation handle
reads nullopt and cannot touch a new occupant's borrow metadata.

The borrow is observed through the new generation-validated `borrow_for_test(SlotHandle)` seam
(`SLUICE_ASYNC_INTERNAL_TESTING`-guarded; by-value `BorrowSnapshot` — never a `RequestSlot*` or
`BorrowMetadata&`), so a later slot reuse can never be mistaken for the original request.

### 3.2 Rows 12a/14a — single waiter + lease (arena)

`arena_waiter_registration_state_matrix` pins the registration window from the as-built contract
(which the Accepted ADR's Decision 10 state machine describes — registration happens while the
request is outstanding pre-terminal):

| Slot state | register_waiter result |
|---|---|
| reserved / prepared | `invalid_state` |
| pending / enqueued | success (the pre-terminal registration window) |
| running | `invalid_state` (syscall already executing; no audit divergence — the implementation allows only pending/enqueued and the ADR does not authorize running registration) |
| backend_ready / completion_ready | `invalid_state` (terminal won; registration closed; the Phase F public consumer will translate closed-into-ready) |
| stale (released/reused) | `not_found` (generation validation) |

`arena_single_waiter_first_registration_survives` proves the no-overwrite property the task calls the
C2c false-green trap: the second registration returns `invalid_state` **and** the final reap delivery
carries token A + lease A, never B — checked by token/lease equality on the delivered event, not just
the registration state. After wait-cancel reopens registration, a new waiter B registers cleanly and
is the one delivered (no registration residue).

`arena_lease_type_properties` pins the move-only type: not copy-constructible/assignable,
nothrow-move-constructible/assignable (the nothrow moves are what make the ADR's noexcept by-value
sink delivery safe; the as-built type already has them). `arena_lease_transfer_chain_reap_path` and
`..._wait_cancel_path` prove both transfer chains end-to-end: caller lease moved-from after
registration, slot owns it while registered, reap moves it into the event (slot no longer owns;
second reap produces nothing) or cancel_waiter returns it (ReadyEvent never gets it).

`arena_ready_event_waiter_survives_slot_reuse` proves the by-value property WITH a waiter payload:
inside the sink callback the slot is released AND re-reserved (generation advances) and the captured
event's key/kind/token/lease remain intact.

### 3.3 Rows 13/14a — concurrency (arena)

Both races are driven with `std::barrier`-released threads through the arena's single leaf
slot-lifecycle domain; both assert the exactly-one invariant, not an outcome distribution.

`arena_register_waiter_vs_terminal_race` (32 iterations): exactly two legal outcomes —
register-wins (delivery carries A exactly once; late wait-cancel gets nothing) or terminal-wins
(register `invalid_state`; event has no waiter). Never register-success-with-lost-delivery, never
register-failure-with-stored-waiter.

`arena_cancel_waiter_vs_reap_race` (32 iterations): the C2c centerpiece — a registered waiter's
token/lease is moved out EXACTLY ONCE: cancel-wins (cancel_waiter returns lease A; event has no
waiter) XOR reap-wins (event carries token A + lease A; cancel_waiter `not_found`). The lease
ownership count is exactly one in every iteration; a second attempt after the race delivers nothing.

### 3.4 Row 11–14 backend integration (Fake)

`backend_c2c_waiter_borrow_test` proves the REAL Fake submit path produces the same arena borrow
lifecycle and that the waiter seam routes a real accepted `Completion` through the REAL
`arena_.register_waiter` / `arena_.cancel_waiter` authorities — no side-band waiter map, no
reimplementation of the waiter state machine (the seam resolves `Completion*` → `SlotHandle` via the
arena's own bounded scan, the same identity bridge the public cancel path uses):

- `fake_borrow_waiter_delivery_integration` — submit → borrow fd/addr/len EXACT + active;
  register waiter A; `complete_*` produces ONLY backend_ready (Completion not ready, borrow still
  active, sink silent before poll); poll → Completion ready, borrow ended, sink delivered token A +
  lease 99 exactly once; second poll delivers nothing.
- `fake_wait_cancel_keeps_io` — wait-cancel returns the lease, reopens registration, and the I/O
  stays accepted with its borrow active, no terminal, no canceled tally; the I/O then completes
  normally with no waiter delivered.
- `fake_io_cancel_keeps_waiter` — an I/O cancel that WINS the canceled terminal does not delete the
  waiter registration; reap delivers the canceled result AND the waiter together.
- `fake_stale_waiter_authority_harmless` — after release+reuse, a stale-generation
  register/cancel_waiter handle resolves to `not_found` with zero side effect on the live N+1
  occupant's registration (token B + lease 200 intact and delivered).
- `fake_waiter_seam_unbound_completion_not_found` — the seam on an unbound/released Completion
  resolves nothing (no manufactured waiter state machine).

### 3.5 Row 11–14 backend integration (ThreadPool)

`threadpool_backend_c2c_waiter_borrow_test` uses the existing deterministic pause gates (no
`sleep_for`): `tp_running_borrow_cancel_intent_waiter_survives` (Gate B then Gate C) proves the
RUNNING window borrow is active with the exact submitted fd/address/length, that a waiter registered
while enqueued survives enqueued → running → backend_ready, and that running cancel intent ends
neither the borrow nor the waiter (real result wins verbatim; canceled_ops stays 0).
`tp_backend_ready_borrow_still_active_before_reap` catches the exact window where the worker finished
its syscall and `record_terminal` stored the terminal but no reap ran: the borrow is STILL active and
the Completion is NOT ready — **a worker finishing its syscall is not the borrow lifetime end**; only
reap releases the borrow. `tp_wait_cancel_keeps_io` proves wait-cancel ≠ I/O cancel on the real
backend (the syscall still executes and its real result wins). `tp_io_cancel_keeps_waiter` proves an
enqueued I/O cancel keeps the waiter (canceled result + waiter delivered together; no syscall runs).
`tp_stale_waiter_authority_harmless` proves a stale waiter authority cannot touch a live N+1
occupant on the real backend.

### 3.6 Manifest / gate model

`scripts/backend_conformance_manifest.py`:
- New `c2c_arena_borrow_waiter_lease_matrix` evidence (implemented, mandatory, layer `lifecycle`,
  backends backend-agnostic).
- New `c2c_fake_borrow_waiter_integration` evidence (implemented, mandatory, layer `lifecycle`,
  backends Fake).
- New `c2c_threadpool_borrow_waiter_integration` evidence (implemented, mandatory, layer
  `lifecycle`, backends ThreadPool).
- New `uring_c2c_borrow_waiter_not_implemented` evidence (STATUS_NOT_IMPLEMENTED, mandatory, layer
  `lifecycle`, backends Uring, reason: RequestArena borrow/waiter/lease lifecycle integration must
  wait for Phase D).

`scripts/verify-backend-conformance.py` needs no change: the existing `_backend_verdict` iteration
over APPLICABLE mandatory evidence handles the new records — Fake/ThreadPool C2c integration is
mandatory + implemented, so a RUN_FAIL forces NOT_CONFORMING; Uring's C2c gap is mandatory +
not_implemented, so it forces INCOMPLETE in Uring's OWN verdict (surfaced in the reasons list
alongside the C2a/C2b gaps). A generic arena PASS can never erase Uring's tagged gap.

### 3.7 Validity evidence

Method chosen: **local uncommitted single-point production mutation** (the C2b-precedented
alternative; a test-only nonconforming fixture would require duplicating the RequestArena internals).
Each defect class was proven by a temporary mutation of the real production logic in
`include/sluice/async/detail/request_arena.hpp`, a focused filtered test run, and an immediate
restore verified by `git diff` (no `MUTANT` marker may remain). See
[`docs/verification/phase-c2c-waiter-borrow-mutation-evidence.md`](../verification/phase-c2c-waiter-borrow-mutation-evidence.md)
for the full per-mutant patch/command/exit-code/restore ledger.

| Mutant | Deliberate defect (§13 class) | Expected failing case | Actual failing case |
|---|---|---|---|
| A | borrow begins at prepare (I7 violation) | `arena_borrow_lifecycle_full_matrix` | same |
| B | borrow ends at record_terminal/backend_ready (worker-syscall-end == borrow-end) | `tp_backend_ready_borrow_still_active_before_reap` | same (visible FAILED line: "borrow must still be active with exact metadata at backend_ready") |
| C | second waiter overwrites the first (overwrite bug) | `arena_single_waiter_first_registration_survives` | same |
| D | wait-cancel also cancels the I/O (stolen I/O authority) | `arena_waiter_cancel_removes_only_the_waiter` | same |
| E | cancel_waiter returns the lease but leaves `waiter_delivery_present_` set (duplicate lease) | `arena_cancel_waiter_vs_reap_race` | same |
| F | reap closes registration but drops the lease (lease dropped) | `arena_lease_transfer_chain_reap_path` | same |
| G | registration allowed after terminal (state matrix violation) | `arena_waiter_registration_state_matrix` | same |
| H | stale waiter authority bypasses generation validation | `fake_stale_waiter_authority_harmless` | same |

Every mutant run exited non-zero; arena-level mutants terminate via the destructor fail-fast after the
case's assertion records the violation (the fail-fast fires because the broken invariant leaves the
slot in use at case scope exit — the standard repo mechanism for a failed case), while the
ThreadPool/Fake cases print the FAILED line with the intended message before cleanup. All mutations
were restored and `git diff include/ src/` shows only the guarded test seams.

## 4. Test case ledger

| Case (SLUICE_TEST_CASE) | Target | Status |
|---|---|---|
| `arena_borrow_lifecycle_full_matrix` | request_waiter_borrow_lease_test | PASS |
| `arena_borrow_survives_cancel_and_wait_cancel` | request_waiter_borrow_lease_test | PASS |
| `arena_borrow_rollback_and_stale_protection` | request_waiter_borrow_lease_test | PASS |
| `arena_waiter_registration_state_matrix` | request_waiter_borrow_lease_test | PASS |
| `arena_single_waiter_first_registration_survives` | request_waiter_borrow_lease_test | PASS |
| `arena_waiter_cancel_removes_only_the_waiter` | request_waiter_borrow_lease_test | PASS |
| `arena_io_cancel_keeps_waiter_registration` | request_waiter_borrow_lease_test | PASS |
| `arena_lease_type_properties` | request_waiter_borrow_lease_test | PASS |
| `arena_lease_transfer_chain_reap_path` | request_waiter_borrow_lease_test | PASS |
| `arena_lease_transfer_chain_wait_cancel_path` | request_waiter_borrow_lease_test | PASS |
| `arena_ready_event_waiter_survives_slot_reuse` | request_waiter_borrow_lease_test | PASS |
| `arena_register_waiter_vs_terminal_race` | request_waiter_borrow_lease_test | PASS |
| `arena_cancel_waiter_vs_reap_race` | request_waiter_borrow_lease_test | PASS |
| `fake_borrow_waiter_delivery_integration` | backend_c2c_waiter_borrow_test | PASS |
| `fake_wait_cancel_keeps_io` | backend_c2c_waiter_borrow_test | PASS |
| `fake_io_cancel_keeps_waiter` | backend_c2c_waiter_borrow_test | PASS |
| `fake_stale_waiter_authority_harmless` | backend_c2c_waiter_borrow_test | PASS |
| `fake_waiter_seam_unbound_completion_not_found` | backend_c2c_waiter_borrow_test | PASS |
| `tp_running_borrow_cancel_intent_waiter_survives` | threadpool_backend_c2c_waiter_borrow_test | PASS |
| `tp_backend_ready_borrow_still_active_before_reap` | threadpool_backend_c2c_waiter_borrow_test | PASS |
| `tp_wait_cancel_keeps_io` | threadpool_backend_c2c_waiter_borrow_test | PASS |
| `tp_io_cancel_keeps_waiter` | threadpool_backend_c2c_waiter_borrow_test | PASS |
| `tp_stale_waiter_authority_harmless` | threadpool_backend_c2c_waiter_borrow_test | PASS |
| Python: `test_backend_conformance_manifest.py` (120 cases, incl. 17 C2c) | unittest | PASS |

## 5. Fake / ThreadPool eligibility, Uring known gap

- **Fake = ELIGIBLE** — `c2c_fake_borrow_waiter_integration=PASS` in the per-backend report.
- **ThreadPool = ELIGIBLE** — `c2c_threadpool_borrow_waiter_integration=PASS`.
- **Uring = NOT CONFORMING** — `uring_c2c_borrow_waiter_not_implemented=INCOMPLETE (not_implemented)`
  is the authoritative C2c gap record, surfaced in the lifecycle layer AND in the verdict reasons
  (alongside `uring_capacity_not_implemented` and `uring_c2b_identity_not_implemented`). Uring stays
  NOT CONFORMING until Phase D (RequestArena migration). This record reinforces (does not replace)
  the existing KernelIoProfile-stays-NOT-CONFORMING rule.

## 6. Profile applicability

**Fake:**
- deterministic reference lifecycle; no real running syscall (borrow observed at
  enqueued/backend_ready windows)
- wait-cancel / I/O-cancel independence; sink delivery exactly-once; stale waiter authority

**ThreadPool:**
- real blocking syscall; running window + backend_ready-before-reap window (the
  worker-syscall-end != borrow-end proof)
- running cancel intent vs waiter/borrow; wait-cancel != I/O cancel; enqueued I/O cancel keeps the
  waiter

**Uring:**
- not implemented until Phase D
- C2c rows 11–14 require RequestArena borrow/waiter/lease integration
- Uring's gap is the `uring_c2c_borrow_waiter_not_implemented` record

## 7. Commands run (validation)

| Gate | Command | Result |
|---|---|---|
| Focused arena | `xmake build request_waiter_borrow_lease_test && xmake run request_waiter_borrow_lease_test` | PASS (13 cases) |
| Focused Fake | `xmake build backend_c2c_waiter_borrow_test && xmake run backend_c2c_waiter_borrow_test` | PASS (5 cases) |
| Focused ThreadPool | `xmake build threadpool_backend_c2c_waiter_borrow_test && xmake run threadpool_backend_c2c_waiter_borrow_test` | PASS (5 cases) |
| Stability | 3× repeated runs of the arena + ThreadPool targets | PASS (no flake) |
| Manifest self-test | `python3 scripts/tests/test_backend_conformance_manifest.py` | PASS (120 cases) |
| Aggregate gate | `python3 scripts/verify-backend-conformance.py` | see section 8 |
| RED validity | 8 mutations (A–H), focused filtered runs | all RED; all restored (see §3.7 + mutation ledger) |

## 8. Validation matrix (full evidence)

All rows below were executed on the current branch head. `PASS` is recorded only for commands that
actually ran green.

| Gate | Command | Result |
| ---- | ------- | ------ |
| Debug / Clang full | `xmake f -m debug --toolchain=clang -y && xmake build -g test && xmake test -v` | PASS (146 targets, 0 failed) |
| Focused arena | `xmake run request_waiter_borrow_lease_test` | PASS (13 cases) |
| Focused Fake | `xmake run backend_c2c_waiter_borrow_test` | PASS (5 cases) |
| Focused ThreadPool | `xmake run threadpool_backend_c2c_waiter_borrow_test` | PASS (5 cases) |
| Stability | 3× repeated runs of arena + ThreadPool targets | PASS (no flake) |
| Release / Clang | `xmake f -m release --toolchain=clang -y && xmake build -g test && xmake test -v` | PASS (146 targets, 0 failed) |
| ASan/UBSan | `xmake f -m asanubsan --toolchain=clang -y && xmake build -g test && xmake run -g test` | PASS (exit 0; zero ASan/UBSan reports) |
| TSan | `xmake f -m tsan --toolchain=clang -y && xmake build -g test && xmake run -g test` | PASS (exit 0; zero race reports, incl. the C2c register-vs-terminal and cancel_waiter-vs-reap races) |
| Manifest self-test | `python3 scripts/tests/test_backend_conformance_manifest.py` | PASS (120 cases, incl. 17 C2c) |
| Aggregate gate | `python3 scripts/verify-backend-conformance.py` | PASS (Fake/TP ELIGIBLE with C2c records PASS; Uring NOT CONFORMING with the C2c gap in its reasons) |
| Doc links | `python3 scripts/check-doc-links.py --self-test` + `python3 scripts/check-doc-links.py` | PASS (no broken links, no stale paths) |
| Architecture docs | `python3 scripts/verify-architecture-docs.py` | PASS |
| Negative-compile | `scripts/verify-completion-authority-negative-compile.sh` | PASS (12 cases) |
| Negative-compile | `scripts/verify-request-arena-negative-compile.sh` | PASS (6 cases) |
| Negative-compile | `scripts/verify-async-identity-negative-compile.sh` | PASS (3 cases) |
| Negative-compile | `scripts/verify-external-backend-authority-negative-compile.sh` | PASS (2 cases) |
| Diff hygiene | `git diff --check` | PASS (clean) |

## 9. Remaining gaps

- **C2d** — failure injection + post-commit allocator terminal (rows 9–10): PENDING, not closed.
- **C2e** — close/drain/reset (row 15; row 16 already FULL): PENDING, not closed.
- **Rows 12b/14b** — real public waiter / RequestHandle / Scheduler registration consumer and real
  Scheduler routing-record lifetime + lease acknowledgement: Phase F scope (no public waiter API
  exists; the fake leases prove the abstract transfer mechanics only — ADR Decision 10's own
  boundary).
- **Phase D** — Uring RequestArena migration: PENDING; Uring C2c conformance is the
  `uring_c2c_borrow_waiter_not_implemented` record, never skip-as-pass.
- **Phase G** — backend-ready progress wake bridge: PENDING (out of C2c scope).

## 10. Phase status

- Phase C remains **PARTIAL** (C1 IMPLEMENTED; C2a COMPLETE; C2b COMPLETE; C2c COMPLETE; C2d–C2e
  pending).
- **C2c: COMPLETE** — rows 11, 12a, 13, and 14a of the C2 matrix have arena-level, per-backend,
  concurrency-proven, mutation-valid evidence for Fake and ThreadPool; Uring's gap is authoritatively
  recorded. Rows 12b/14b (real Scheduler waiter consumer / routing-record lifetime) are explicitly
  Phase F scope, not C2c gaps.
- No production behavior change (only `SLUICE_ASYNC_INTERNAL_TESTING`-guarded header seams in
  `include/sluice/`); no synchronous Reader/Writer behavior change; no Phase D Uring implementation;
  no C2d/C2e/Phase F/Phase G scope creep.
