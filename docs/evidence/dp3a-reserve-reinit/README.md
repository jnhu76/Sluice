# DP-3A redundant reserve-time slot reset — compiler proof and oracle evidence

Baseline: master `4221c7e3` (the merge-base of this PR; includes DP-1 via
#572 and the #571 bench fixes). The reserve change is one independent hunk of
`src/async/detail/request_core.cpp` and is revertible on its own. Compiler:
WSL2 `g++ (Ubuntu) 15.2.0`, release `-O3`, `-std=c++20`, NDEBUG, no LTO.

Reproduction:

    g++ -std=c++20 -O3 -DNDEBUG -I include -I src/async \
        -S src/async/detail/request_core.cpp -o out.s
    # extract RequestCore::reserve() between its label and .size directive

## Findings

- BASE `reserve()` re-stores the whole cleared field set of the popped slot:
  seven 16-byte `movups` zero stores (offsets 40/49/72/96/112/128/144) plus
  eight scalar stores (`movq $0` at 16/28/88/160, `movb $0` at 24, `movl $-1`
  at 72, `movb $1` at 128) — the re-initialization the free-list invariant
  makes redundant.
- The dp3a `reserve()` keeps exactly one slot store: `movb $1, (%rax)`
  (`phase = SlotPhase::reserved`). Every other slot-field store is gone; the
  remaining stores build the returned `ReserveAttempt` in the result buffer.
- No new allocation, no hidden temporary of comparable size; the function
  body shrinks 105 → 90 lines (including the `.cold` section).
- On the E5 release binaries (GCC 16.2.1) the same conclusion holds in the
  final linked code: the ARM pop path has the single `movb $0x1,(%rax)` slot
  store and no zeroing stores; see `reserve.e5.objdump.txt`. The direct
  incremental A/B and full native gate on that baseline are recorded in
  `e5-final-ab/README.md`.

## Initialization-closure oracle

`tests/request_core_init_closure_test.cpp` asserts the invariant the removal
relies on: every slot on the free list is indistinguishable from a
value-initialized slot (modulo generation) — across construction, first
reserve, accept, rollback, physical/metadata/cancel-won/zero-op terminals,
publication pin, observer claim/retirement, release, generation advance,
generation exhaustion (saturated generation retires; exhausted core refuses),
retired slots never resurfacing, and closed-admission/poison refusals.

- It passes unchanged on this arm (reserve no longer re-initializes) and on
  the BASE tree with only the test seam (SlotObservation metadata exposure)
  applied — it is an invariant test, not an implementation mirror.
- All writers of `free_slots_` were audited: the constructor (value-
  initialized slots) and `release_slot_` (full clear before push). No path
  bypasses release.

## Named mutants (killed by the closure oracle; manual execution only)

| mutant | observed kill |
|---|---|
| `RELEASE_SKIPS_OUTCOME_CLEAR` | 28 named assertion failures (stale outcome leaks into re-acquire) |
| `RELEASE_SKIPS_METADATA_CLEAR` | metadata assertion of the closure oracle (SlotObservation metadata) |
| `RELEASE_SKIPS_OBSERVER_CLEAR` | stale `queued` observer phase blocks reclaim, named failure |
| `ROLLBACK_WITHOUT_RELEASE` | "rolled-back slot is clean", "the reservation identity is dead", "rolled-back slot returns to service" (then teardown fail-fast) |
| `EXHAUSTED_SLOT_REUSED` | 9 named failures — the saturated slot re-enters service and the exhausted status never reports |
| `CONSTRUCTOR_DIRTY_SLOTS` | 8 failures — dirty constructed slots are caught at the first-reserve/accept assertions |

A `RELEASE_SKIPS_CONTROL_REFS_CLEAR` seam was reviewed and removed: the skip
is structurally unobservable through any legal protocol path (`reclaimable_`
gates the free-list push on `control_refs == 0`), so it cannot register a
distinguishing failure and stayed dead in the guard set. The unconditional
release-side clear is retained. Binding-lifetime violations are owned by the
existing `public_request_release_violation_test` (green on this arm).

## Instruction effect

Research measurement (WSL2/GCC 15, zop deterministic instrument, 15/15
pairs): −16.5 instructions/op; the final-candidate E5 native A/B (GCC 16,
master baseline, 15/15 pairs) measured −20.9 instructions/op for this
mechanism alone, with the d1 wall ratio at 1.00142 [0.99349, 1.00539] —
see `e5-final-ab/README.md`.
