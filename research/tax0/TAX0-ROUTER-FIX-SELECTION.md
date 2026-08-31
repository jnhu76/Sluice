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
  — https://github.com/jnhu76/Sluice/pull/256
- **SHOOTOUT-FREEZE SHA:** `d45f620` (candidate implementations, admission
  table, prior-art survey, correctness gates, both bench layers, runner,
  and validator frozen BEFORE any official measurement)
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
- `tests/uring_router_fix_equivalence_test.cpp` — semantic gates (§6)
- `tests/uring_router_fix_death_test.cpp` — fail-fast gates (§6)
- `bench/tax0router_micro_bench.cpp` — Layer A (§8)
- `bench/tax0router_shootout_bench.cpp` — Layer B (§9)
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

Identical for Layer A and Layer B (same host, same session class):

| fact | value |
| --- | --- |
| host CPU | Intel(R) Xeon(R) CPU E5-2666 v3 @ 2.90GHz, 1 socket, 10 cores / 20 threads |
| pinning | `taskset -c 0,2,4,6` — VERIFIED distinct physical cores (cpu0→core 0, cpu2→core 2, cpu4→core 4, cpu6→core 9 via `/proc/cpuinfo`; CPUs 10–19 are SMT siblings, never pinned) |
| kernel | Linux 7.1.9-200.fc44.x86_64 |
| governor | schedutil |
| memory | 62 GiB |
| filesystems | `/tmp` = tmpfs; `/home` = btrfs (`/dev/sda3[/home]`), warm |
| liburing | 2.13 (real — Layer B requires it) |
| toolchain | clang 22.1.8 (Fedora 22.1.8-4.fc44), xmake Release config |

## 8. Layer A methodology — router lifecycle microbench

Binary `tax0router_micro_bench` (frozen at d45f620), driven by
`perf-attribution.py tax0routermicro`: one `perf stat` process per rep,
user-mode `instructions`/`cycles` counters, pinned to CPUs 0,2,4,6.

- **Honesty scope:** each micro-op mirrors ONLY the router-install slice
  of `dispatch_one_locked` (free-list pop in the mode's physical order,
  no-wrap cookie allocation, `RouterEntry` install, R3 table insert);
  lookups run through the REAL production `find_live_router_cookie_`
  (mode-dispatched) and retires through the REAL production
  `retire_router_entry_` (including R3's table erase). No kernel I/O,
  no ring, no arena interaction: this layer isolates the router
  representation's lifecycle cost; Layer B supplies end-to-end
  attribution.
- **Identical logical trace per candidate:** window = D installs → D
  lookups in completion order → D retires. The trace is a pure function
  of (pattern, seed, windows, D, C) and asserted identical across
  candidates (per-row witness: `ops == lifecycle_ops`, lookup hits ==
  ops, misses == 0, `steady_allocations_per_op == 0`).
- **Patterns:** P0 = LIFO/reverse-order completion (worst case for R0);
  P1 = per-window Fisher–Yates permutation from the campaign seed; P2 =
  full occupancy, runs only on square cells (D == C).
- **Frozen parameters:** windows 20000, reps 9 (blocked randomized
  rounds), geometries D:C = 8:8, 8:32, 8:128, 8:512, 32:32, 32:128,
  32:512, 128:128, 128:512 (the task's optional 128-cells WERE run),
  seed 0x52545253. Measured runs: 4 candidates × (2 patterns × 9 cells
  + 1 pattern × 3 square cells) × 9 reps = 756.
- Work size fixed pre-freeze from smoke runs; not adjusted after any
  official measurement (§18 discipline).

## 9. Layer B methodology — real io_uring end-to-end shootout

Binary `tax0router_shootout_bench` (frozen at d45f620), driven by
`perf-attribution.py tax0routershootout`: real io_uring via liburing
2.13, 4 KiB requests, Q == D enforced per cell (frozen primary matrix),
candidate mode installed via `set_router_fix_mode_for_test` before
runtime start.

- **Four sessions (fs × op):** read/tmpfs, write/tmpfs, read/btrfs,
  write/btrfs; candidates interleaved inside each session.
- **Frozen work size:** 128 MiB per process per cell (4 KiB × 32768
  ops), warmup 2 unmeasured full rounds, 9 measured rounds (blocked
  randomized), per-candidate process isolation.
- **WRITE fairness:** the write arm refills its buffer from the master
  pattern before every submit (no zero-page shortcut), verifies the
  written file after all reps, and never fsyncs (page-cache semantics
  for every candidate alike).
- **Witness gates per row (fail-closed):** op lookups == ops, hits ==
  ops, misses == 0, control/transport contamination == 0,
  `matched_router_index_max` < capacity, R3 `table_insert_calls ==
  table_erase_calls == ops`, non-R3 table calls == 0.
- Workload file deterministic, bench-generated when absent; same file
  for every cell within a session; separate files for the two
  filesystems and the two op arms.

## 10. Randomization and blocking

Both layers: blocked randomized rounds generated by
`random.Random(0x52545253).sample(sorted(cells))` per round — a pure
function of (cells, reps, seed), predeclared at freeze. The validator
RECOMPUTES the order from the recorded seed and requires exact equality
with the recorded per-round order AND the physical row sequence (fail
closed). Candidates are interleaved inside each round, so drift lands
between rounds, not between candidates.

## 11. Raw evidence index

All artifacts runner-produced, one file each, under
`docs/results/performance-attribution/`:

- `tax0router-fix-micro.json` — Layer A official (kind
  `tax0routermicro`, 756 rows). SUPERSEDES an earlier same-day run of
  identical frozen parameters whose `execution_order.cells` summary
  mislabeled the candidate field (runner unpack bug — capacity written
  into the candidate slot; `rounds`, `rows`, and all measurements were
  correct). Rather than hand-editing a runner-produced artifact, the
  tooling was fixed and the campaign re-run with unchanged parameters;
  the superseding artifact is the official one.
- `tax0router-fix-shootout-read-tmpfs.json` / `-write-tmpfs.json` /
  `-read-btrfs.json` / `-write-btrfs.json` — Layer B official (kind
  `tax0routershootout`, 252 rows each = 4 candidates × 7 cells × 9
  reps).

Post-freeze tooling fixes (both validator-facing, none touching
measurement parameters or winner rules; scope documented here):
(a) the micro `execution_order.cells` unpack bug above; (b)
`tax0router-validate.py` micro parser could not parse ANY 3-field cell
string and its self-test never exercised `validate_micro` (now covered:
4 micro mutations + multi-session aggregate + duplicate-session
fail-closed); (c) `campaign_aggregate` flagged the by-design shared
(cand, D, C) grid across the four fs×op sessions as duplicate cells —
rewritten to key by (cand, D, C, fs, op); (d) `perf-evidence-validate.py`
kind allowlist learned the two new kinds (structural checks only;
semantic authority stays in `tax0router-validate.py`).

## 12. Validator result

`scripts/bench/tax0router-validate.py --micro <Layer A> --shootout
<all four Layer B artifacts>`: **VALIDATION PASSED**. It recomputes the
seed-derived execution order (exact match required vs recorded rounds
and row sequence), per-cell medians, normalization vs same-session r0,
GMs, envelopes, guardrail/tie classification, and the §25 selection —
recorded `derived` blocks must match the recomputation. Per-row
semantic witnesses are enforced (misses == 0, control/transport == 0,
R3 table accounting, non-R3 zero table traffic, zero steady-state
allocation, real_uring + Q == D). Self-test: 7 shootout + 4 micro
mutations all rejected, multi-session aggregate sanity, duplicate-session
fail-closed. Additionally `perf-evidence-validate.py`: 22/22 artifacts
pass (structural), self-test OK.

## 13. Layer A results — per-cell normalized instructions vs R0

Micro (router lifecycle isolated). Full per-cell table (instruction
ratios; cycles in parentheses r1/r2/r3):

| cell | r1 | r2 | r3 |
| --- | --- | --- | --- |
| P0 D=8,C=8 | 0.993 (1.031) | 0.993 (1.002) | 1.208 (1.130) |
| P0 D=8,C=32 | 0.676 (0.836) | 0.676 (0.829) | 0.823 (0.923) |
| P0 D=8,C=128 | 0.297 (0.357) | 0.297 (0.354) | 0.361 (0.396) |
| P0 D=8,C=512 | 0.092 (0.155) | 0.092 (0.152) | 0.111 (0.170) |
| P0 D=32,C=32 | 0.994 (1.015) | 0.994 (1.003) | 0.918 (0.951) |
| P0 D=32,C=128 | 0.361 (0.281) | 0.361 (0.276) | 0.328 (0.257) |
| P0 D=32,C=512 | 0.102 (0.134) | 0.102 (0.132) | 0.092 (0.123) |
| P0 D=128,C=128 | 0.997 (1.002) | 0.997 (1.002) | 0.400 (0.522) |
| P0 D=128,C=512 | 0.231 (0.225) | 0.231 (0.224) | 0.093 (0.118) |
| P1 (same 9 cells) | ≈P0 | ≈P0 | ≈P0 |
| P2 D=8,C=8 | 0.993 (1.015) | 0.993 (0.996) | 1.208 (1.117) |
| P2 D=32,C=32 | 0.994 (1.021) | 0.994 (1.004) | 0.918 (0.956) |
| P2 D=128,C=128 | 0.997 (1.002) | 0.997 (1.003) | 0.400 (0.520) |

Reading: r1 ≡ r2 to three decimals everywhere (they are placement
duals — identical scan lengths by construction). Ratios scale with the
live-set fraction D/C: no gain at D == C (full array is live; any scan
is equally long), ~10× at C/D = 64. r3 is flat O(1) — fastest at large
C, but at small capacity its table maintenance (insert+erase probes)
EXCEEDS the scan it replaces (1.21× at C=8): on the isolated slice, R3
only wins once C is large enough for the scan to dominate maintenance.

## 14. Layer A aggregate — geometric means

Normalized GM vs same-session R0 over all 21 (cell, pattern) points:

| candidate | gm_instr | gm_cycles |
| --- | --- | --- |
| r1 | 0.4371 | 0.5050 |
| r2 | 0.4371 | 0.4963 |
| r3 | 0.3723 | 0.4203 |

On the isolated slice R3 looks best — but the slice is exactly where
the scan dominates; §15–§16 show this does NOT transfer end-to-end.

## 15. Layer B results — per-cell medians vs same-session R0

Median instr/op of R0 and instruction ratios per candidate (each value
over 9 blocked-randomized reps; four sessions):

| session | cell | r0 instr/op | r1 | r2 | r3 |
| --- | --- | --- | --- | --- | --- |
| read/tmpfs | D=8,C=8 | 4837.6 | 0.9999 | 0.9993 | 1.0143 |
| read/tmpfs | D=8,C=32 | 4979.0 | 0.9709 | 0.9712 | 0.9852 |
| read/tmpfs | D=8,C=128 | 5558.3 | 0.8702 | 0.8697 | 0.8825 |
| read/tmpfs | D=8,C=512 | 7856.6 | 0.6161 | 0.6162 | 0.6243 |
| read/tmpfs | D=32,C=32 | 4417.0 | 0.9995 | 0.9997 | 0.9945 |
| read/tmpfs | D=32,C=128 | 4993.4 | 0.8842 | 0.8842 | 0.8799 |
| read/tmpfs | D=32,C=512 | 7297.1 | 0.6052 | 0.6052 | 0.6022 |
| write/tmpfs | D=8,C=8 | 5257.7 | 0.9994 | 0.9996 | 1.0141 |
| write/tmpfs | D=8,C=32 | 5399.6 | 0.9757 | 0.9738 | 0.9868 |
| write/tmpfs | D=8,C=128 | 5977.2 | 0.8800 | 0.8789 | 0.8914 |
| write/tmpfs | D=8,C=512 | 8279.2 | 0.6346 | 0.6356 | 0.6433 |
| write/tmpfs | D=32,C=32 | 4840.4 | 0.9997 | 0.9996 | 0.9950 |
| write/tmpfs | D=32,C=128 | 5416.6 | 0.8932 | 0.8933 | 0.8892 |
| write/tmpfs | D=32,C=512 | 7721.1 | 0.6268 | 0.6267 | 0.6239 |
| read/btrfs | D=8,C=8 | 4454.0 | 0.9996 | 0.9996 | 1.0162 |
| read/btrfs | D=8,C=32 | 4598.0 | 0.9682 | 0.9682 | 0.9834 |
| read/btrfs | D=8,C=128 | 5174.2 | 0.8605 | 0.8605 | 0.8739 |
| read/btrfs | D=8,C=512 | 7478.8 | 0.5954 | 0.5954 | 0.6047 |
| read/btrfs | D=32,C=32 | 4295.3 | 0.9995 | 0.9995 | 0.9962 |
| read/btrfs | D=32,C=128 | 4871.5 | 0.8814 | 0.8814 | 0.8784 |
| read/btrfs | D=32,C=512 | 7176.1 | 0.5984 | 0.5984 | 0.5964 |
| write/btrfs | D=8,C=8 | 5749.2 | 1.0016 | 0.9972 | 1.0149 |
| write/btrfs | D=8,C=32 | 5910.0 | 0.9758 | 0.9693 | 0.9859 |
| write/btrfs | D=8,C=128 | 6465.1 | 0.8898 | 0.8896 | 0.8999 |
| write/btrfs | D=8,C=512 | 8700.1 | 0.6619 | 0.6620 | 0.6681 |
| write/btrfs | D=32,C=32 | 4909.2 | 1.0158 | 1.0046 | 1.0066 |
| write/btrfs | D=32,C=128 | 5446.6 | 0.9026 | 0.9022 | 0.9007 |
| write/btrfs | D=32,C=512 | 7724.4 | 0.6413 | 0.6352 | 0.6333 |

The end-to-end slope mirrors EXP-U0's causal claim: gain grows with
capacity at fixed depth (≈0% at D==C, ~35–40% of TOTAL instructions at
C=512), is op-independent (read ≈ write), and filesystem-independent
(tmpfs ≈ btrfs).

## 16. Layer B aggregate — envelope, split GMs, median/best/worst

Campaign envelope vs same-session R0 across all 4 sessions (28
session-cell ratios per candidate; §25 production-selection authority):

| candidate | gm_instr | gm_cycles | worst_cell_instr | worst_cell_cycles | guardrail |
| --- | --- | --- | --- | --- | --- |
| r1 | 0.8397 | 0.9093 | 1.0158 | 1.0417 | PASS |
| r2 | 0.8387 | 0.9072 | 1.0046 | 1.0454 | PASS |
| r3 | 0.8443 | 0.9023 | 1.0162 | 1.0325 | PASS |

Split GMs (instructions): tmpfs r1 0.8385 / r2 0.8383 / r3 0.8432;
btrfs r1 0.8409 / r2 0.8390 / r3 0.8454. By op: read r1 0.8293 / r2
0.8293 / r3 0.8349; write r1 0.8502 / r2 0.8482 / r3 0.8538.

Per-candidate distribution over the 28 session-cells (instr): median
r1 0.8915 / r2 0.8914 / r3 0.8957; best ≈0.595–0.596 (C=512 cells);
worst 1.0046–1.0163 (D==C cells). Cycles: median r1 0.9429 / r2 0.9388
/ r3 0.9292; worst 1.0325–1.0454.

## 17. Guardrail disqualification check (§23)

Rule: any cell > +5% instructions vs R0, or any cycles regression
beyond +5%, disqualifies. NO candidate is disqualified: worst
instruction cell is r3 read/btrfs D=8,C=8 at +1.62% (r1 +1.58%, r2
+0.46%); worst cycles cell is r2 write/tmpfs at +4.54% (within the 5%
guardrail; r1 +4.17%, r3 +3.25%). All three candidates: guardrail
PASS.

## 18. Practical-tie analysis (§24)

Rule: candidates whose GMs are both within 2% of the GM leader AND with
no >2pp worst-regression disadvantage form a practical tie, resolved by
the simplicity order R1 < R2 < R3. GM leader on instructions is r2
(0.8387): r1 trails by 0.10pp, r3 by 0.56pp — both inside the 2% band;
cycles spread 0.9023–0.9093 (0.77pp) — inside the band; worst-cell
instruction spread 1.0046–1.0163 (1.2pp) — inside 2pp. PRACTICAL TIE:
{r1, r2, r3} → simplest candidate r1.

Interpretation (not part of the mechanical rule): end-to-end, the
router slice is small enough that O(1)-vs-O(D) lookup differences do
not separate the candidates; what all three remove is the CAPACITY
term (the C-proportional part), which is why the C=512 cells converge
and the D==C cells show no difference. R3's table maintenance (paid on
dispatch and reap, which R1/R2 never touch) cancels its lookup gain at
exactly the cells where Layer A showed R3 winning in isolation.

## 19. Winner selection under §25 mechanical rules

Applied by the frozen validator to the official artifacts:
eligible = {r1, r2, r3} (guardrail). No candidate leads another by ≥2%
on instruction GM ⇒ no outright selection; practical-tie set = {r1,
r2, r3}; simplicity minimum ⇒ **R1 SELECTED**. No opaque composite
score used: instructions GM + guardrail + tie band + simplicity order
only, with cycles GM recorded alongside.

## 20. Verdict

**ROUTER SHOOTOUT PASS — PRACTICAL TIE, SIMPLEST CANDIDATE SELECTED
(R1).**

R1 (reverse scan of the existing `router_` array — the traversal
one-line dual of the production predicate, first evidenced by EXP-U0)
is the selected fix candidate: it delivers the entire measurable
end-to-end benefit (GM instructions 0.8397, GM cycles 0.9093 vs
same-session R0; +35–40% instruction reduction at C=512 cells; zero
regression beyond noise at D==C), with no new state, no new capacity
consumer, and no structural change. R2 delivers the same numbers with
a placement change (reseeded free list); R3 adds bounded structural
state for an end-to-end gain indistinguishable from the tie band.

## 21. Handoff and proposed ledger transition

- Base master: `9bbe3a243cc3c87f1f1ce2450a43fc3b5c5eedfa`
- SHOOTOUT-FREEZE: `d45f620` (admission, candidates, gates, harnesses,
  runner, validator — frozen before any official measurement)
- Post-freeze tooling fixes: recorded in §11, committed on this branch
  (validator/runner only; measurement parameters and winner rules
  untouched)
- Final head: see the branch tip of `research/tax0-router-fix-shootout`
  (also recorded in the closing commit of PR #256)
- Draft PR: https://github.com/jnhu76/Sluice/pull/256
  `research(perf): benchmark Uring router-resolution fix candidates`
- RAW EVIDENCE PATHS (§11): `docs/results/performance-attribution/`
  `tax0router-fix-micro.json`,
  `tax0router-fix-shootout-{read,write}-{tmpfs,btrfs}.json`
- VALIDATOR RESULT (§12): VALIDATION PASSED (tax0router-validate.py on
  all five artifacts; perf-evidence-validate.py 22/22 structural)
- VERDICT: ROUTER SHOOTOUT PASS — PRACTICAL TIE, SIMPLEST CANDIDATE
  SELECTED (R1)

**PRODUCTION BEHAVIOR CHANGED: NO.** All candidate modes live behind
`SLUICE_ASYNC_INTERNAL_TESTING`; the production default path is
byte-identical to master `9bbe3a2` (mechanically enforced by the
mechanical-facts seam-exclusion detector; equivalence matrix proves
mode-independent observable semantics).

**PRODUCTION FIX IMPLEMENTED: NO.** R1 ships nothing. Implementation is
a separate production PR, to be authored only after human adversarial
review of this report.

**Issue discipline (not executed, by task §33/§34):** #255 and
T0-U-ROUTER remain OPEN; #250 is NOT transitioned to FIX SELECTED by
this campaign; EXP-U1 / EXP-0 / EXP-2 / EXP-3 are NOT started.

Proposed #250 ledger transition (PROPOSAL ONLY — for the human reviewer
to apply after adversarial review of this report):

> T0-U-ROUTER: SHOOTOUT COMPLETE — FIX SELECTED (R1, reverse scan),
> pending production implementation. Evidence: PR #256,
> `research/tax0/TAX0-ROUTER-FIX-SELECTION.md` (freeze d45f620),
> artifacts `tax0router-fix-*` under
> `docs/results/performance-attribution/`, validator PASS. Next step:
> human adversarial review → separate production PR (do NOT close
> #255 until the production fix lands).
