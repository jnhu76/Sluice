# DP-1 — terminal publication payload (candidate analysis)

## Claim under test

The inline publication path materializes a 120-byte `PublicationPayload`
(stack init + 12-move fill from slot and arguments) whose only consumed field
is `.id` (24 B) — which the publisher already holds — so the payload need
never be constructed: pass `nullptr`.

## Mechanism (base → dp1)

- Base: backends (`uring_backend.cpp`, `threadpool_backend.cpp`) build the
  payload (`mov $0xf,%ecx; rep stos` = 120 B + field stores), call
  `begin_publication(key, &payload)`, and read `payload.id`;
  `deliver_event(payload.id, kind)`-shaped call sites re-derive the key from
  the payload.
- dp1: `begin_publication(key, nullptr)` / `begin_publication(id, nullptr)`;
  the 4 call sites that dereferenced `.id` now use the in-scope key directly.
  `begin_publication`'s body keeps the `out != nullptr` guard, so the
  snapshot capability is retained for any future caller; today none exists
  (field-liveness-matrix.md residual fact 2).

## Static evidence

- Machine code: base `publish_zero_op_inline` contains the 15-qword `rep stos`
  init and the 12-move `begin_publication` copy block executes per op;
  dp1/dp1dp2b machines have **no** payload init and the copy block is
  null-skipped (`results/receipts/assembly-excerpts.md` §A).
- Compiler behavior: with no LTO, cross-TU DSE cannot remove either half
  (gimple-sra-dse.md §2, probe A case 1) — the removal had to be a source
  change, and was.

## Dynamic evidence

- zop (deterministic): −27.5 instructions/op, 15/15 pairs (main matrix;
  identical −0.860 % in the fixedbt matrix — a two-family replication).
- Poll-spinning cases (fixedbt, artifact-free for dp1): cons4k +6.18 %
  (12/3), thr64 +6.78 % (11/4), write4k +6.18 % (10/5), finfo +4.40 %
  (11/4), constp +3.22 % (13/2) — a consistent second-order effect (earlier
  completion visibility → fewer consumer poll iterations), far above the
  self-pair variance floor.
- w01ctl: 0.000 % both families (exact negative control).

## Correctness

- 76/76 conformance+unit tests (both uring and threadpool backends, plus the
  app-level oracles) on the dp1 and dp1dp2b arms.
- W-01 binary byte-identical across arms.
- The publication protocol (begin/complete/deliver under the core mutex) is
  unchanged; only the out-copy is skipped. `begin_publication`'s return
  status semantics are untouched.

## Research classification

**ELIMINATED_CONFIRMED** (see decision.md for the enum): the dead payload is
never constructed; the effect is exact on the deterministic instrument,
replicated across two binary families, and consistent (second-order) on
spinning cases.

## Productization notes (for any future PR, not part of this research)

- Public API unchanged; `PublicationPayload` stays a public type.
- A product patch should include the same 4 call-site rewrites and the
  same rationale; the research arm diff is 16 lines across 2 files
  (`results/receipts/` harness receipts reference the arm worktrees).
- Mechanism-justification gate: the removed states are a derived snapshot
  (non-authoritative); no authority moves.
