# R5 — Correctness oracle and named mutants

Oracle: `bench/research/uring-router-580/identity_oracle.cpp` — deterministic
scenarios over the real backend (real kernel + the existing internal-testing
seam for stale/duplicate/wrong-target CQE injection). Every wait is bounded;
every RED must come from a named assertion (assertion text is flushed before
any downstream contract fail-fast).

## Scenario list (23 assertions)

1. **s1 exact identity, stale replay**: an op completes through its exact
   cookie; replaying that cookie afterwards (success and error variants) is a
   silent miss that leaves no residue.
2. **s2 slot reuse**: a retired cookie cannot attach to the successor route
   that reused the router slot; the successor completes with verified bytes
   and retires exactly once. (H1 negative-control 4 equivalent.)
3. **s3 duplicate CQE**: a second completion for the same cookie after
   retirement produces no second publication.
4. **s4 exact control routing**: a running cancel converges through its
   control CQE; a control CQE aimed at a **wrong (non-live) target** is
   ignored (`live_control_sqes` unchanged); the terminal is a legal kernel
   outcome; control released and route retired. The terminal race (real CQE
   vs cancel) is not asserted — the kernel owns that order; identity is
   asserted through control accounting.
5. **s5 settled-generation cancel**: canceling an already-settled request
   must not touch the live successor on the same slot; no control reference
   leaks.
6. **s6 user_data 0** is dropped; the real completion still delivers.

## Results (E5 native, logs in `results/oracle_*.log`)

| Variant | Result |
|---|---|
| baseline | **ALL GREEN 23/23** |
| we2 (scan-direction flip) | **ALL GREEN 23/23** — behavioral equivalence on every scenario |
| we2 conformance suite (E5 + WSL) | 20/20 `backend_contract_conformance_uring`; 21/21 `uring_request_core_integration` incl. `real_kernel_slot_reuse`, `stale_cookie_cannot_reach_a_reused_slot`; 8/8 smoke — both arms |
| `SLUICE_580_MUTANT_FALSE_MISS` (M7 family: a live CQE silently discarded) | **RED by named assertion**: `s2 successor completed within the poll budget`, `s2 successor unaffected…`, `s2 successor retired exactly once` all FAIL; the subsequent destructor fail-fast on the leaked nonterminal request is the production contract firing on the damaged state, downstream of the kill |
| restored baseline (rebuilt, rerun after the mutant) | **ALL GREEN 23/23** — BASE GREEN → MUTANT RED → RESTORED GREEN complete |
| `SLUICE_580_MUTANT_SLOT_ONLY` (M3/M6 family: generation dropped from the by-handle match) | **GREEN under all legal inputs — by construction, documented below** |

## Why the slot-only mutant is unkillable by legal inputs (and why that is the
safety argument for the direction flip)

`find_live_router_index_` has exactly two callers, both under `dispatch_mtx_`:

1. `cancel_key` calls it only when `core_->cancel(key)` returned `requested` —
   i.e. the core has just confirmed that exact (slot, generation) is running;
   under the same lock, its router route cannot disappear between the
   confirmation and the lookup.
2. `service_pending_controls_locked_` passes `route.handle` read from the
   route's own entry — exact by construction.

Additionally at most one live router entry per slot exists at any time, held
by **two pinning mechanisms** (Review A's verification):

1. when no control is live at terminal time, the router entry retires under
   the same `dispatch_mtx_` hold that queues publication, so the entry is gone
   before the publication that frees the slot for its next generation;
2. when a cancel control is live (the cancel-issued → op terminal →
   publication → control-CQE interleaving), retirement waits for the control
   CQE: the slot cannot be re-reserved because `reclaimable_` requires
   `control_refs == 0`, and the `release_control` that unpins it runs inside
   the same lock hold, immediately before `retire_router_entry_`.

A reservation also dispatches at most once (queue discipline +
`claim_execution`), and a (slot, generation) pair can never recur
(`release_slot_` bumps the generation before re-adding the slot). Therefore
the scan predicate `in_use && slot== && generation==` has a unique live match
in every reachable state, and **no legal execution — indeed no reachable
state — can distinguish a forward scan from a reversed scan**; the two arms
are observationally equivalent, which the mutant's GREEN result and the 20/20 +
21/21 suites corroborate. The we2 disassembly retains both the 32-bit slot and
64-bit generation compares. The miss branch remains as a defensive no-op for
impossible states, unchanged by the direction flip.

## Protocol mutant ledger

| Mutant | Status |
|---|---|
| M1 hash/fingerprint trusted without exact cookie verify | index-candidate-specific; no candidate built (gate). Baseline's exact-64-bit-equality rule at every consumer is asserted by s1/s2/s4 and the conformance suite. |
| M2 skip index erase on retirement | index-candidate-specific; not built. Baseline retirement zeroing is asserted by s1/s2/s3 residue checks. |
| M3 reused router slot accepted for stale cookie | exercised by s2 (GREEN baseline/we2); slot-only variant analyzed above |
| M4 control cookie routed as original operation | control-tag separation asserted by s4 (wrong-target ignored, exact-target releases); control-without-submitted-state termination is covered by the master conformance suite on both arms |
| M5 poison recovery leaves stale index entry | index-candidate-specific; not built. Baseline Class-A identity verification (cookie+handle before any recovery retirement) is unchanged by we2 and covered by the conformance suite's poison scenarios |
| M6 incorrect generation match | slot-only mutant built and analyzed (above); s5 covers the settled-generation path |
| M7 index false miss silently discards a live CQE | **built and killed** (`FALSE_MISS` variant, RED by three named assertions, restored green) |
