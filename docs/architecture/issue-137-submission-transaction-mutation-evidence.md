# Issue #137 — Submission Transaction Mutation Evidence

**Purpose:** RED-validity evidence for the centralized submission transaction
(`detail::submit_transaction` in
`include/sluice/async/detail/submit_transaction.hpp`). The shared ladder is the
ONE pre-accept authority for every in-repo explicit-I/O backend; a single-point
defect here would affect ALL four backends simultaneously. Each mutation below
is planted at ONE site in the shared ladder (or in the Uring policy's Stage-0
hook, which is part of the same authority chain), proven RED by a focused
detector test, reverted, and confirmed GREEN.

**Method:** local uncommitted mutation (Phase C2b §13-accepted). For each
mutant:

1. apply ONE single-point mutation;
2. rebuild the affected test target;
3. run exactly the detector case;
4. record the expected failing case, actual failure mode, exit code;
5. revert;
6. after all mutants: confirm no residue and re-run the full suite.

Toolchain: **Clang (Linux Debug)**, xmake, `--with-liburing=true` (M4 requires
real uring path).

## Mutation matrix

| Mutant | Deliberate defect | Mutation site | Expected failing case | Actual failing case / failure mode | Exit |
| --- | --- | --- | --- | --- | --- |
| M1 | prepare-failure rollback omitted (slot leaks) | `submit_transaction` L126-130: remove `arena.rollback_reserved_or_prepared(h)` from the injected-prepare-failure path | `tp_c2d_prepare_failure_injection_slot_rollback` | same — the injected prepare rejection leaks the candidate slot; the post-submit `arena_slot_in_use == 0` assertion fails and the destructor's arena invariant guard calls `std::terminate` | -1 (terminate) |
| M2 | LP moved before commit (Completion outstanding before slot committed) | `submit_transaction` L155-189: swap order — pause, install_binding, commit_binding BEFORE `arena.commit(h)`, with the C2d commit-injection seam placed after the LP | `tp_c2d_commit_failure_injection_rollback_binding_before_accept` | same — the injected commit failure tries `rollback_binding` on an already-outstanding Completion; the Completion lifecycle guard calls `std::terminate` (rollback_binding on outstanding is a checked contract violation) | -1 (terminate) |
| M3 | CAS loser skips own slot rollback (slot leaks) | `submit_transaction` L155-157: remove `arena.rollback_reserved_or_prepared(h)` from the `begin_binding` loser path | `tp_c2d_cas_loss_rejection_zero_side_effects` | same — a double-submit into an outstanding Completion loses the CAS, but the candidate slot is not rolled back; `arena_slot_in_use` overreports and the destructor's arena invariant guard calls `std::terminate` | -1 (terminate) |
| M4 | Uring poison precedence swapped (admission_closed before fatal_error) | `UringAsyncBackend::SubmitPolicy::stage0_precheck()` in `uring_backend.hpp` L467-472: swap order so `admission_closed_` check precedes `fatal_error_` check | `uring_c2e_poison_close_keeps_class_c` | same — after poison + close_admission, a submit returns `invalid_state` (admission_closed wins) instead of the required `backend_error` (poison verbatim, D4-M5); the assertion `r.error().code == IoError::Code::backend_error` fails | 1 (harness fail) |

## Single-site authority proof

The shared ladder is the ONE pre-accept authority for every in-repo backend.
Mutations M1–M3 are planted at a single site in
`detail::submit_transaction` and affect ALL four backends simultaneously —
this is the centralization dividend: a defect at one site is detectable
through ANY backend's test, and a fix at one site repairs ALL backends.
M4 is planted in the Uring policy's Stage-0 hook (the only policy-specific
admission authority), which is the single divergence from the shared ladder
that carries its own precedence rule (D4-M5).

## Revert verification

After all 4 mutations were reverted:

- `grep -rn "VALIDITY MUTATION" include/ src/ tests/` → zero matches;
- `git diff` on the two mutated files shows no residue beyond the intended
  S1–S4 migration diff;
- full real-liburing Debug suite → **189/189 PASSED**.

## Relationship to existing mutation evidence

This supplement complements
`docs/verification/phase-c2b-identity-mutation-evidence.md` (rows A–G:
RequestArena identity, terminal winner, cancel semantics, publication
boundary). The C2b evidence proves the ARENA authority; this document proves
the SUBMISSION LADDER authority that sits above it. Together they cover the
full pre-accept chain from Stage 0 through the acceptance LP.
