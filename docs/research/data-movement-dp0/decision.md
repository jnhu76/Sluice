# DP-0 decision and final report

RESEARCH ONLY. This directory records an audit; nothing here merges product
changes, changes the public API, or amends the v2 root. Per AGENTS.md: do
not merge without explicit instruction.

## Verdict enum (fixed before classification)

1. `ELIMINATED_CONFIRMED` — physically executed data movement removed;
   assembly-exact + deterministic-instrument measurement + correctness oracle.
2. `ELIMINATED_INDICATIVE` — removal proven in assembly; measurements
   consistent but below deterministic resolution on this host.
3. `CONFIRMED_NEUTRAL` — implemented; proven **not** to reduce physical work
   on the hot path (retained only as evidence/oracle carrier).
4. `REJECTED_REGRESSION` — implemented; measured regression.
5. `ANALYTICAL_ONLY_NOT_IMPLEMENTED` — analyzed with concrete numbers/sketch;
   no arm built.
6. `REQUIRES_ROOT_OR_API_CHANGE` — viable design, excluded by root contract.
7. `ALREADY_OPTIMAL` — audited path has no removable movement.

## Per-candidate verdicts

| candidate | verdict | key numbers |
|---|---|---|
| DP-1 publication payload → `nullptr` | **ELIMINATED_CONFIRMED** | zop −27.5 instr/op (15/15, both binary families); spinning cases 0.9–11.9 % across families, 3.2–6.8 % in the artifact-free family; `rep stos` init gone from machine code |
| DP-2 counterfactual A (include_metadata flag, `dp2`) | **CONFIRMED_NEUTRAL** (mildly negative) | zop +12.1 instr/op slower, 0/15 pairs; kept as the runtime-oracle carrier |
| DP-2 counterfactual B (type-split overloads, `dp2b`) | **ELIMINATED_CONFIRMED** | zop −8.8 instr/op (14/15); no `RequestResultData` on non-metadata paths; metadata misrouting is a compile error |
| DP-3A reserve re-init removal (`dp3a`) | **ELIMINATED_CONFIRMED** | zop −16.5 instr/op (15/15, identical in both families); reserve = 1 store |
| DP-3B hot/cold slot split | **ANALYTICAL_ONLY_NOT_IMPLEMENTED** | no implemented arm and no cache/TLB PMU evidence; the earlier "dominated by DP-3C on traffic" claim is withdrawn (DP-3C erratum); footprint benefit unproven at exercised capacities |
| DP-3C don't-write-unread-metadata (producer side) | **ANALYTICAL_ONLY_NOT_IMPLEMENTED** (redefined by erratum) | original premise false: the producer store is already `has_metadata`-gated in source and does not execute on non-metadata ops; redefined as a per-site audit of `Slot::metadata` init/write sites vs `TerminalCandidate::metadata`; `size` ops require metadata (`FileSize`) |
| DP1+DP2B combo | **ELIMINATED_CONFIRMED** (additive) | zop −36.30 instr/op (15/15) = the −36.3 sum of parts, exactly |
| W-01 blocking path | **ALREADY_OPTIMAL** | no control-plane objects; byte-identical control `b3c403c0…` across all six arms |
| Slot.borrow removal | ANALYTICAL_ONLY_NOT_IMPLEMENTED (not a measured arm) | authority-contraction candidate recorded (field-liveness-matrix.md); gate decision pending named-consumer analysis |
| Seastar/tokio-uring owned-buffer patterns | **REQUIRES_ROOT_OR_API_CHANGE** | contradict strictly-borrowed buffers (external-design-comparison.md) |

Projected combined effect of the accepted set (dp1+dp2b+dp3a), per
non-metadata op: `THREE_ARM_PROJECTED_SUM = 52.8 instr/op` (additivity of
measured parts: −36.3 combo + −16.5 dp3a) —
`THREE_ARM_COMBINED_MEASURED = 62.7 instr/op` on the E5 native-Linux
validation host (GCC 16, release, 15/15 pairs; the 52.8 projection was
measured on WSL2/GCC 15 and is superseded by the measured figure on that
host — e5-native-validation.md §1). The ≈ **500 B** figure is a
static-analysis estimate of logical
load/store elimination (payload 216 B, result bridge 152 B, reserve re-init
132 B), not a measured memory-bus or DRAM-traffic quantity. Measured:
the second-order poll-loop
effect (0.9–11.9 % whole-loop instruction reductions across families; 3.2–6.8 % in the artifact-free family). Against
zop's measured base median (3 196.7 instructions/op) the projected direct sum is ≈1.65 % — real physical work, but
not a headline number, and the report does not inflate it.

## Errata (2026-10-23, second review)

An external re-review confirmed the measured arms from the raw JSONL
(dp1 −27.5 15/15; dp2 +12.1 0/15; dp2b −8.8 14/15; dp3a −16.5 15/15;
dp1+dp2b −36.3 15/15; fixedbt reproduces dp1/dp3a) and raised three
corrections, all applied in this revision:

1. **DP-3C source-model error.** The claim that `offer_terminal` writes
   40 B of `FileInfo` per ordinary read/write is false against baseline
   source: the store is gated on `candidate.has_metadata` (default false;
   set only for `file_info`/`size` completions in both backends), and
   zero-ops never reach `offer_terminal`. The sketched
   `descriptor.op == file_info` gate would also have broken `FileSize`
   (size ops require `metadata.size`). DP-3C is redefined as a per-site
   audit of `Slot::metadata` initialization/write sites vs
   `TerminalCandidate::metadata`; no measured figure depended on the old
   premise.
2. **52.8 instr/op is a projected sum.** The only combined arm measured
   in the WSL round is DP-1+DP-2B (−36.3). The ≈500 B figure is a static
   logical load/store estimate, not a memory-traffic measurement. The
   three-arm combo was subsequently measured on the E5 native-Linux host:
   62.7 instr/op (e5-native-validation.md §1).
3. **DP-3B cache-line argument shrunk.** 168 B slots are not 64 B aligned;
   consecutive slot starts drift (168 mod 64 = 40), so slot offset 0x80
   never maps to a fixed physical cache line. Without cache/TLB PMU data or
   implemented DP-3B/DP-3C arms, no cross-workload domination is claimed.

## E5 validation (2026-10-23)

The agreed native-Linux pre-productization stage ran to completion
(e5-native-validation.md; raw evidence under results/raw/e5/ and
results/receipts/e5/). Headlines:

- Three-arm combo measured: 62.7 instr/op removed (zop, 15/15),
  negative control clean; W-02/W-03 and ThreadPool driven; stats-wired
  `poll_calls/op` recorded.
- The claimed second-order poll benefit does not appear on W-02 shapes
  (poll/op unchanged to slightly up) and is real only on
  W03-uring-4k-d4 (−9.5 % poll passes).
- W02-uring-4k-d1 regresses +2.9 % (CI wholly > 1) for the three-arm
  tree. Attribution: dp1 alone improves (0.9821), dp2b alone is neutral
  (0.9919) — the regression is a dp1×dp2b interaction; dp3a adds ≈ +0.5 %
  (noise). The dp2b productization PR must therefore validate on top of
  dp1 at final HEAD, with W02-uring-4k-d1 as the arbitrating shape.
- W-03 pinned runs exposed a stop-protocol race in the
  bench_w03_external_loop harness (driver consumes request_stop's wake
  before `shared.stop` is set and re-parks in epoll_wait(-1) forever;
  join never returns). Investigated to root cause with a causal fix
  (e5-native-validation.md §6); bench fixed in-tree by this branch; the
  library's notification paths audited — no missing-wake path found.
  W-03 numbers are unpinned (recorded deviation).

## The seven final questions

**Q1 — Does DP-1 remove real physical work, and can the compiler do it
alone?** Yes and no. The 120 B payload init + 96 B copy survive at -O3
without LTO (cross-TU DSE keeps every byte; gimple-sra-dse.md probe A). dp1
removes them at source level; measured −27.5 instr/op on the deterministic
instrument, replicated in two binary families, with a consistent 3.2–6.8 %
whole-loop reduction in the artifact-free family on spinning cases. ELIMINATED_CONFIRMED.

**Q2 — Which DP-2 shape is right, and what do the tests actually protect?**
The flag shape (dp2) is instruction-neutral-to-negative (+12.2 instr/op,
0/15) because it keeps the 72 B local struct; the split shape (dp2b)
removes −8.8 instr/op and is compile-time-protected against metadata
misrouting. The conformance suite protects the metadata requirements of
FileInfo/FileSize results: the dp2 mutation fails exactly the 4
`file_info_and_size_match_the_kernel_record` assertions. Ship shape, if
productized: dp2b. dp2 itself: CONFIRMED_NEUTRAL.

**Q3 — Is the slot's initialization actually redundant work?** Half of it.
`reserve()` re-zeroes bytes `release_slot_` just zeroed under the same
mutex — dp3a removes that half (−16.5 instr/op, 15/15, both families).
Release-side zeroing is kept: it is the acquire-side safety re-init
(the same single-site discipline liburing uses with
`io_uring_initialize_sqe`). ELIMINATED_CONFIRMED for the redundant half.

**Q4 — What did the layout study conclude for DP-3B/3C?** (Answer corrected
by the 2026-10-23 erratum.) The original answer repeated a false premise:
the producer's 40 B metadata store does not execute on non-metadata ops —
`offer_terminal` already gates it on `candidate.has_metadata`, set only for
file_info/size completions, and zero-ops never reach `offer_terminal`. What
remains on slot line 2 for non-metadata ops is initialization/zeroing only
(`release_slot_`; the reserve-side twin is dp3a's removed arm), and a
`descriptor.op == file_info` gate would break `FileSize` (size ops require
`metadata.size`). DP-3C is redefined as a per-site audit of
`Slot::metadata` init/write sites; DP-3B's "dominated by DP-3C" claim is
withdrawn — neither has an implemented arm or PMU cache evidence. Both
ANALYTICAL_ONLY_NOT_IMPLEMENTED, with the named gate obligations
(SlotObservation, diagnostics, size-op metadata liveness) recorded.

**Q5 — What is already clean?** W-01 (blocking read): precheck → pread →
Result; byte-identical across all six arms; negative control exact. The
RequestKey/slot-index ownership token matches the tokio-uring/liburing
`user_data` consensus. `user_data`-shaped authority needs no change.

**Q6 — What does the open world say about Sluice's shape?** Every subject
(libuv, asio, tokio-uring, Seastar, liburing) routes only the consumed
datum through completion machinery and re-initializes per-acquire state at
a single site. Sluice's supersized publication snapshot (dp1) and
double-site re-init (dp3a) were the outliers and the audit's productive
findings; its borrowed-buffer contract is stricter than tokio-uring/Seastar
and their owned-buffer patterns are out of scope by root constraint.

**Q7 — What must remain INVALID/unattributed, and what is the honest total?**
No cache, TLB, or memory-hierarchy number in this study is a hardware
measurement (L1-dcache/dTLB events do not exist on this host;
cache-references/cache-misses are INVALID here — hardware-attribution.md).
Wall-time and cycles are context, never verdicts: the self-pair control
proved the frozen thresholds' noise assumption miscalibrated for
poll-spinning cases, so no verdict rests on them. Honest total: the
measured confirmed set is dp1+dp2b (−36.3 measured, additive) plus dp3a
(−16.5 measured on an independent arm); their three-arm sum was projected
at 52.8 on WSL2/GCC 15 and measured at 62.7 instr/op removed on the E5
native-Linux host (e5-native-validation.md §1), and ≈500 B is a
static logical load/store estimate per non-metadata op
(≈1.6 % of the measured zop loop,
direct; 0.9–11.9 % across families including the second-order completion-visibility effect),
with DP-3B/DP-3C/W-03-A/B recorded as not-measured/not-implemented items.

## Recommendation (research disposition — no product action taken)

If productization is later authorized, the evidence supports, in order:
dp1 (smallest, exact), dp2b (with its compile-time oracle), dp3a. Each
would be its own PR re-run through the full conformance matrix per
AGENTS.md; the combo arm's additivity is evidence they are independent.
DP-3C's source model was corrected by the erratum: it needs redefinition
(the per-site audit above) plus a mechanism-justification pass before any
implementation.

## Known gaps (recorded, not hidden)

- W-03 A/B ran unpinned only (pinned runs trip a stop-protocol race in
  the W-03 bench harness — investigated and fixed in-tree by this
  branch, e5-native-validation.md §6); its loop-specific delta is
  measured on unpinned runs and the single-CPU shape remains unmeasured.
- The three-arm combined figure is measured on the E5 native-Linux host
  (62.7 instr/op, e5-native-validation.md §1); it remains unmeasured on
  the WSL host, and the W-02 uring d1 regression carries the dp1×dp2b
  interaction finding into the dp2b productization gate.
- DP-3B/3C are analytical only.
- Instruction-verdict numbers are statements about their measurement
  host (WSL2/GCC 15.2.0 for the arm deltas, native Linux/GCC 16.2.1 for
  E5); the receipts define the reproduction path for any other host.
- The frozen threshold rule proved miscalibrated for spinning cases
  (self-pair control); verdicts rest on the deterministic instrument and
  assembly accounting instead, and this deviation-in-spirit is documented
  rather than patched silently.

BEGIN_SLUICE_DP0_DATA_MOVEMENT
audit: DP-0 compiler-first data-movement audit of Sluice file-I/O paths
baseline: master be28f275 (post PR #569); branch research/dp0-data-movement-audit; RESEARCH ONLY
host: WSL2 Linux 6.18.54.1-microsoft-standard-WSL2, g++ (Ubuntu) 15.2.0, -O3 -std=c++20, no LTO
scope: DP-1 terminal publication, DP-2 result bridge, DP-3 slot init/layout; W-01 blocking control
instruments: GIMPLE/objdump store accounting (exact), zop deterministic microbench (±1 instr),
            w01ctl + identical-binary self-pair controls, 3 interleaved AB/BA matrices
            (29+15+7 configs, 15 pairs each, 0 excluded runs), mutation oracles (runtime+compile-time)
verdicts: DP-1 ELIMINATED_CONFIRMED (-27.5 instr/op zop, 15/15);
          DP-2A CONFIRMED_NEUTRAL (+12.1 instr/op, 0/15); DP-2B ELIMINATED_CONFIRMED (-8.8, 14/15);
          DP-3A ELIMINATED_CONFIRMED (-16.5, 15/15); DP-1+DP-2B combo ELIMINATED_CONFIRMED (-36.30, 15/15);
          DP-3B/DP-3C ANALYTICAL_ONLY_NOT_IMPLEMENTED (2026-10-23 erratum: DP-3C premise corrected —
          producer store already has_metadata-gated; DP-3B domination claim withdrawn);
          W-01 ALREADY_OPTIMAL
THREE_ARM_PROJECTED_SUM = 52.8 instr/op (WSL2/GCC 15);
          THREE_ARM_COMBINED_MEASURED = 62.7 instr/op removed (E5 native Linux, GCC 16, 15/15,
          e5-native-validation.md);
          static logical load/store elimination estimate ~500 B per non-metadata op
          (payload 216 B + result bridge 152 B + reserve re-init 132 B) — not a
          memory-traffic measurement;
          second-order poll-loop reductions 0.9-11.9% across families (3.2-6.8% artifact-free) (above self-pair floor)
E5 stage: native-Linux validation complete (e5-native-validation.md); W02-uring-4k-d1 wall
          arm/base: dp1 alone 0.9821 (improve), dp2b alone 0.9919 (neutral), dp1dp2b 1.0244,
          threearm 1.0292 — d1 regression is a dp1×dp2b interaction, arbitrating shape for the
          dp2b PR's final-HEAD gate; poll/op: W-02 shapes unchanged (claimed poll benefit absent),
          W03-uring-4k-d4 −9.5% poll passes; W-03 pinned stall root-caused to the bench's
          stop-after-request_stop race (bench fixed in-tree; library notification paths audited,
          no missing-wake path); W-03 deltas unpinned (recorded deviation)
correctness: 76/76 tests on all 6 arms incl. combo; 76/76 on all 5 E5 trees; W-01 negative control
          byte-identical b3c403c0...;
          wrong metadata-skip caught at runtime (dp2 shape) and at compile time (dp2b shape)
invalid-marked: L1-dcache-*/dTLB-* unavailable on host; cache-references/cache-misses INVALID;
          wall/cycle medians context only; frozen thresholds demonstrated miscalibrated for
          poll-spinning cases by self-pair control (documented, verdicts do not rest on them)
recommendation: research-only; productization order if authorized: dp1, dp2b, dp3a (each its own PR);
          DP-3C needs the erratum redefinition (per-site audit) plus the mechanism-justification gate
          first; nothing merged here
END_SLUICE_DP0_DATA_MOVEMENT
