# DP-2 — result bridge materialization (candidate analysis, two counterfactuals)

## Claim under test

`Request<T>::try_result/take_result` always populate a 72-byte
`RequestResultData` (default-init + 32 B IoOutcome + 40 B FileInfo copied
from the slot), then re-copy the consumed fields into `Result<T>`. For
`size_t`-carried results the 40 B metadata and (part of) the local struct
are unread — the Required Fields Matrix
(field-liveness-matrix.md) says exactly which bytes each of the six result
forms needs.

## Counterfactual A — dp2 (runtime flag)

4-arg overloads `observe/consume(core, id, &data, include_metadata)` with
`include_metadata` a compile-time constant per `T`
(`k_result_uses_metadata_ = is_same<T,FileInfo> || is_same<T,FileSize>`).

- Assembly: flag-guarded metadata copy (`test %cl,%cl; cmovne` + `test
  %rdx,%rdx; je` around the 0x80/0x90/0xa0 moves — `results/receipts/
  assembly-excerpts.md` §D). The 72 B local `RequestResultData` still exists
  for every T.
- **Measured (zop, deterministic): −12.1 instr/op i.e. the arm is SLOWER by
  ~12 instructions, 0/15 pairs** — the saved metadata copy (≈3 instructions)
  does not pay for the flag plumbing, and the local struct remains.
- Verdict: instruction-neutral-to-negative as a product change. Retained in
  the study as the matrix's executable counterfactual and as the arm that
  provides the **runtime mutation oracle**.

- Oracle (M1): forcing the classification to "never metadata" compiles and
  fails exactly the metadata assertions — 4 × `file_info_and_size_match_the_kernel_record`
  in both backend conformance binaries; restore → 76/76
  (`results/receipts/m1-dp2-mutation-failures.txt`). This proves the tests
  protect the ● entries of the matrix.

## Counterfactual B — dp2b (type-directed overload split)

Split overloads `observe/consume(core, id, bool* succeeded, IoError* error,
EffectReport* effect)` for non-metadata T: no `RequestResultData` is
declared at all on those paths; metadata types keep the original overload.

- Assembly: the split functions contain **zero** 0x80/0x90/0xa0 accesses,
  and non-metadata instantiations contain no `RequestResultData` init
  (§D of the receipt).
- **Measured (zop): −8.8 instructions/op, 14/15 pairs** — matches the
  prediction (72 B init + metadata copies − split-call overhead).
- Oracle (M1): misrouting `FileInfo` to the split path is a **compile error**
  in every consumer TU (`request.hpp:229: cannot convert 'const uint64_t' to
  'sluice::FileKind'`; `results/receipts/m1-dp2b-mutation-summary.txt`) —
  stronger than a runtime catch; the type system enforces the matrix.
- w01ctl: exactly +1 instruction/iteration (0/15 pairs) — a bench-TU
  register-allocation artifact of recompiling the same source against the
  modified header, fully accounted (`results/receipts/assembly-excerpts.md`
  §E). Not a library change: `blocking_file.o` is byte-identical across arms.

## Combo additivity

`dp1dp2b` (both patches; disjoint files): zop −36.30 instr/op (15/15) vs
dp1+dp2b predicted sum −36.3 — additive exactly
(`results/analysis.md`). 76/76 tests; `bench_w01_direct` byte-identical.

## Research classification

- dp2 (counterfactual A): **CONFIRMED_NEUTRAL** as a product change
  (no physical-work reduction; kept only as oracle carrier and matrix
  executable form).
- dp2b (counterfactual B): **ELIMINATED_CONFIRMED** for `size_t`-carried
  results (dead local + dead metadata copies never constructed; deterministic
  measurement + type-level oracle).

## Productization notes

- Public API unchanged: the split overloads are internal
  (`RequestBridge::observe/consume` are detail headers); `Request<T>`'s
  public semantics are untouched — conformance suites pass unchanged.
- The mechanism justification is clean: the removed states are derived
  snapshots; no authority moves; the metadata path is bit-identical to base
  for FileInfo/FileSize (assembly §D).
- Risk note for review: `EffectReport` replaces `RequestResultData.effect`
  on the split path — the conformance suite's effect-reporting assertions
  pass unchanged on all arms, which is the evidence that the two carriers
  are semantically equivalent for non-metadata results.
