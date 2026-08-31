# TAX-0 / T0-U-ROUTER — Router fix-candidate shootout and fix selection

Research material for issues #227 (roadmap), #250 (TAX-0 campaign),
#255 (fix-selection gate). Downstream of #254 (EXP-U0, merged: router
scan causally attributed at ~100% of the capacity slope). This document
is the §30 fix-selection report for the candidate shootout; it selects a
winner under the §25 mechanical rules and hands off to human review. It
does NOT ship any production change: the winner remains a
`SLUICE_ASYNC_INTERNAL_TESTING`-guarded research mode until a separately
approved production PR.

- **Base master:** `9bbe3a243cc3c87f1f1ce2450a43fc3b5c5eedfa`
- **Branch:** `research/tax0-router-fix-shootout`
- **Draft PR:** `research(perf): benchmark Uring router-resolution fix candidates`
- **SHOOTOUT-FREEZE SHA:** `PENDING (filled at §18 freeze)`
- **Final head SHA:** `PENDING (filled after official campaign)`
- **Canonical raw evidence:** `PENDING (paths filled after official campaign)`
- **Validator result:** `PENDING`
- **VERDICT:** `PENDING (exactly one allowed §31 form)`

**PRODUCTION BEHAVIOR CHANGED: NO** (to be re-affirmed with evidence at
handoff). **PRODUCTION FIX IMPLEMENTED: NO.**

---

## 1. Scope and authorization

Task T0-U-ROUTER under #255, executing the campaign flow fixed by
#250: causal attribution (EXP-U0, done) → prior art → semantic candidate
admission → candidate implementation freeze → benchmark shootout →
robust winner → STOP → human adversarial review → separate production
PR. This report covers up to STOP. It closes nothing: #255 and
T0-U-ROUTER stay open, and #250 is NOT moved to FIX SELECTED here (§33
of the task; proposed ledger transition in §21).

In scope: the UringAsyncBackend router cookie→router-index resolution
path (`find_live_router_cookie_`) and its O(C) linear scan, the single
mechanism EXP-U0 causally attributed. Out of scope: every other
EXP-U1/EXP-0/EXP-2/EXP-3 experiment (not started), any production
implementation of the winner, any change to shipped semantics.

## 2. Frozen prior-art summary

Full per-system analysis: `TAX0-ROUTER-PRIOR-ART.md` (frozen in the same
freeze commit as this report; written BEFORE candidate implementation).
Three families exist in the surveyed systems (liburing, fio, Tokio,
tokio-uring, Monoio, Glommio, Seastar, Boost.Asio):

1. **Raw pointer as user_data** (fio `io_u*`, Seastar
   `kernel_completion*`, Asio `io_queue*`): O(1) resolve, but bakes a
   heap address into the kernel ABI — conflicts with Sluice's
   quiescent-destruction contract, fixed bounded metadata, and the
   integer `user_data` discipline already shipped.
2. **Recycled slab index** (Tokio slab idx, tokio-uring slab with
   `u64::MAX` cancel sentinel, Monoio slab + reserved range, Glommio
   `FreeList` `SourceId`): O(1) resolve, identity reused on release.
   Sluice retires router entries at REAP time while the arena slot is
   released on a later path; a plain recycled index can be re-issued
   while a stale CQE for the previous occupant is still in flight →
   stale CQE resolves to the NEW request (ABA). Every surveyed
   recycled-index system accepts exactly this aliasing window as its
   contract; Sluice's contract forbids it.
3. **No-wrap unique key** (Sluice only): `[1, 2^63-1]` never-reused
   op cookie, high bit = control tag; stale CQEs resolve to nothing by
   construction. The cost is the O(C) scan — the +5.99 instr/op/C tax
   EXP-U0 attributed.

Comparison contract (not "X does Y therefore Sluice should"): **no
surveyed system linearly scans**; the two O(1) families each pay in a
currency Sluice has already declined (address identity / aliasing
window). A third shape — bounded fixed-capacity cookie→index table
keeping the no-wrap cookie — preserves Sluice semantics while removing
the scan; it is admitted as R3 below.

## 3. Candidate admission table (frozen before implementation)

| candidate | mechanism | semantic authority | stale CQE | bounded memory | steady-state alloc | worst case | admitted? |
| --- | --- | --- | --- | --- | --- | --- | --- |
| R0 | forward scan over `router_`, high-index LIFO placement (production baseline) | cookie identity in `RouterEntry`; arena re-validates slot+generation | resolves to nothing (scan never matches retired cookie) | `router_` C entries, preallocated | none | O(C) per CQE | ADMITTED (baseline) |
| R1 | reverse scan (high→low index) | identical to R0 — only traversal direction changes | identical to R0 | identical to R0 | none | O(C) per CQE (live set at low indices ⇒ ~D) | ADMITTED (U0 ablation, formalized) |
| R2 | descending free-list seed ⇒ live entries park at LOW indices, forward scan unchanged | identical to R0 — placement dual of R1, predicate untouched | identical to R0 | identical to R0 | none | O(C) per CQE (live set ~D) | ADMITTED |
| R3 | fixed-capacity open-addressed cookie→index table (linear probing, backward-shift deletion, no tombstones), no-wrap cookie kept as key | cookie identity unchanged; table is a derived index of `router_`, never an authority (every hit re-validated against `router_[idx]` in-use + cookie match) | table retires entries exactly at reap (`retire_router_entry_`); stale cookie ⇒ `kMiss` ⇒ resolves to nothing | `next-pow2(≥2C)` × 16 B entries, preallocated at construction | none | O(1) expected; degenerate all-collision cluster bounded by table size | ADMITTED |
| R4a | plain reusable index as user_data | index IS identity | REJECTED: re-issued index while stale CQE in flight resolves the OLD completion to the NEW request (ABA) — direct violation of the no-aliasing contract | — | — | — | REJECTED pre-benchmark |
| R4b | raw pointer as user_data | address IS identity | rejected: dangling resolution depends on lifetime discipline Sluice does not own (quiescent destruction, fixed metadata) | — | — | — | REJECTED pre-benchmark |
| R4c | packed index+generation in 64-bit user_data | packed identity | rejected: fitting both fields in 64 bits forces the generation width below the no-wrap 64-bit op-cookie contract; generation wrap reintroduces ABA | — | — | — | REJECTED pre-benchmark |

Rejection reasons are the "exact reason" required by the task: all three
R4 shapes weaken the stale-CQE / no-wrap identity contract that EXP-U0's
causal claim rests on. Per §5 of the task they were audited and rejected
BEFORE any benchmark, so the shootout measures only contract-preserving
candidates.

## 4. Semantic authority preservation (per admitted candidate)

Invariants that must survive in EVERY candidate (task §4), and how each
candidate preserves them:

- **No-wrap cookie:** untouched in R1/R2 (scan/placement only); R3 keeps
  the cookie as the table key — `next_cookie_` allocation path is never
  modified by any seam (`set_router_fix_mode_for_test` explicitly never
  touches it).
- **Stale CQE never resolves to a new request:** R1/R2 by scan
  equivalence (retired cookies never match); R3 because erase happens
  inside `retire_router_entry_` — the single reap-timed retirement
  authority — and a lookup miss is exactly the R0 not-found path, plus a
  belt-and-suspenders re-validation of `router_[idx].in_use && cookie`
  on every hit.
- **Router/arena generation coherence:** no candidate touches slot
  handles, generations, or the arena; R3 stores only router indices and
  re-validates through `router_` before use.
- **Cancel/control semantics:** control tag (high bit) and transport
  cookies resolve through the same (single) resolution function in all
  modes; cancel ordering tests in §8 cover intent/enqueued/running.
- **Terminal/reap authority:** `retire_router_entry_` remains the only
  retirement path in all modes (R3's erase is delegated to it, not
  parallel to it).
- **Bounded capacity / no new steady-state allocation:** R3's table is
  sized once at construction from `request_capacity`; insert/lookup/
  erase allocate nothing. Enqueue path allocation behavior is unchanged
  in all modes (table insert is in dispatch, but is allocation-free —
  witness `steady_allocations_per_op: 0` in every Layer-A row).

R4 rejections above are the contract-weakening audit required by the
task (§5): a candidate that only wins by weakening stale-CQE identity is
not a fix.

## 5. Implementation freeze

All candidates live behind `SLUICE_ASYNC_INTERNAL_TESTING` in
non-installed seam headers (`src/async/uring_test_seams.hpp`) plus
guarded branches in `src/async/uring_backend.cpp` and the installed
header's guarded seam declarations. Mode selection via
`set_router_fix_mode_for_test`, only at quiescence (fail-fast
otherwise). Production default (`production_baseline`) is byte-for-byte
the pre-campaign path. Files frozen at the freeze commit:

- `include/sluice/async/uring_backend.hpp` — guarded seam declarations
- `src/async/uring_test_seams.hpp` — `RouterCookieTableForTest` + mode
  control bodies
- `src/async/uring_backend.cpp` — guarded dispatch/retire/lookup branches
- `tests/uring_router_fix_equivalence_test.cpp` — semantic gates (§8)
- `tests/uring_router_fix_death_test.cpp` — fail-fast gates (§8)
- `bench/tax0router_micro_bench.cpp` — Layer A (§9)
- `bench/tax0router_shootout_bench.cpp` — Layer B (§10)
- `xmake/tests/async_internal.lua`, `xmake/tests/death.lua` — test wiring
- `xmake/benchmarks.lua` — bench target wiring
- `scripts/bench/perf-attribution.py` — runner commands
- `scripts/bench/tax0router-validate.py` — §12 validator

Exact blob SHAs: see the freeze commit; this section is closed at
freeze and not edited afterward.

## 6. Correctness gates (frozen before measurement)

Per-candidate §19 gate matrix — equivalence suite
(`uring_router_fix_equivalence_test`) plus death cases
(`uring_router_fix_death_test`, fork/exec children, exit 86 =
fail-fast). Status at freeze: **ALL GREEN** (local run recorded in the
freeze commit message; raw output under `docs/results/performance-attribution/tax0router-freeze-*.txt`).

| gate | R0 | R1 | R2 | R3 |
| --- | --- | --- | --- | --- |
| mode-equivalence matrix C∈{1,2,4,8,32}×K≤5, 2 rounds (reuse+generation) | PASS | PASS | PASS | PASS |
| hit/miss probe accounting per mode | PASS | PASS | PASS | PASS |
| stale/unknown/control-tagged/distant-cookie miss | PASS | PASS | PASS | PASS |
| post-drain retirement misses | PASS | PASS | PASS | PASS |
| cancel + control CQE + transport path on real pipe I/O | PASS | PASS | PASS | PASS |
| stale CQE reinject dropped | PASS | PASS | PASS | PASS |
| placement witness (R2 low-index; R0/R1 high-index; R3 table) | PASS | PASS | PASS | PASS |
| full-occupancy C live set, switch-back at quiescence | PASS | PASS | PASS | PASS |
| duplicate insert fail-fast | n/a | n/a | n/a | PASS (death) |
| erase absent cookie fail-fast | n/a | n/a | n/a | PASS (death) |
| cookie 0 (outside domain) insert/erase fail-fast; lookup(0) miss | n/a | n/a | n/a | PASS |
| forced hash-collision cluster insert/lookup/erase | n/a | n/a | n/a | PASS |
| table full at capacity bound | n/a | n/a | n/a | PASS |
| stale miss after erase (retired cookie never resolves) | n/a | n/a | n/a | PASS |
| non-quiescent mode switch fail-fast | PASS (death) | PASS (death) | PASS (death) | PASS (death) |

## 7. Environment

`PENDING (full record after official campaign: kernel, CPU model +
pinning verification, memory, governor, toolchain, liburing version,
filesystem mounts)`

## 8. Layer A methodology — router lifecycle microbench

`PENDING (frozen parameters + work-size justification)`

## 9. Layer B methodology — real io_uring end-to-end shootout

`PENDING (frozen parameters, WRITE fairness, filesystems)`

## 10. Randomization and blocking

`PENDING (seed 0x52545253, blocked randomized rounds, validator
recomputation statement)`

## 11. Raw evidence index

`PENDING (every JSON/txt path produced by the runner)`

## 12. Validator result

`PENDING (per-artifact validation + mutation self-test statement)`

## 13. Layer A results — per-geometry normalized vs R0

`PENDING (tables)`

## 14. Layer A aggregate — geometric means

`PENDING (overall + split GMs)`

## 15. Layer B results — per-cell envelope vs same-session R0

`PENDING (tables incl. guardrail columns)`

## 16. Layer B aggregate — GMs, split GMs (fs/op), median/best/worst

`PENDING (tables)`

## 17. Guardrail disqualification check (§23)

`PENDING (any cell > +5% instructions vs R0 or any cycles regression ⇒
disqualified; list per candidate)`

## 18. Practical-tie analysis (§24)

`PENDING (both GMs within 2% AND no >2pp worst-regression advantage ⇒
tie; resolve by simplicity order R1 < R2 < R3)`

## 19. Winner selection under §25 mechanical rules

`PENDING (lowest GM instructions among guardrail-passing candidates;
cycles as tie-break; no opaque composite)`

## 20. Verdict

`PENDING — exactly one of the allowed §31 forms.`

## 21. Handoff and proposed ledger transition

`PENDING (SHAs, PR URL, tables, RAW EVIDENCE PATHS, VALIDATOR RESULT,
verdict; PRODUCTION FIX IMPLEMENTED: NO; #255/#250 left open with a
PROPOSED (not applied) transition text for human review; do-not-start
list for EXP-U1/EXP-0/EXP-2/EXP-3)`
