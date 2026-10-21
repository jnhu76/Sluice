# Review B — Performance Claim Validity and Minimality (issue #564)

Reviewer: independent general-purpose agent (background), 2026-10-21, on
branch `perf/uring-owed-count-skip` at `4edb957d` (+ docs commits).
Adversarial brief: attack the measurement methodology and mechanism
minimality; read-only.

## VERDICT: PERFORMANCE_JUSTIFIED

The perf claim is honest, the mechanism passes the Justification Gate as a
derived summary, every published median reproduces from the raw data, the
negative workloads are clean, and the win's capacity-scaling signature
matches the removed mechanism exactly. Findings are non-blocking; two were
addressed in `1fb4c9c2` before the formal E5 run.

## 1. Minimality and the Mechanism Justification Gate — PASS

Gate answers for `owed_count_` (`include/sluice/async/detail/uring_backend.hpp:294`):
- Fact summarized: `owed_count_ == Σ_i [delivery_[i].event_owed]`.
  Authority owning the fact: the `delivery_` record flags under `dispatch_mtx_`.
- Why materialized: both consumers need an O(1) per-pass verdict — the sweep
  entry guard (`src/async/uring_backend.cpp:648-652`) and `poll_progress()`'s
  `immediate_work_remains` (`src/async/uring_backend.cpp:312-313`). Deriving
  at use time is precisely the O(capacity) cost being removed.
- Classification: derived summary, non-authoritative. The authoritative scan
  survives at `src/async/uring_backend.cpp:346` (destructor) and `:369`
  (`internal_work_retired()`) — BACKEND-04 honored as declared.
- Replaces: the unconditional capacity-wide sweep in `poll()` and the O(C)
  residue scan in `poll_progress()`. One mechanism, two consumers; a sticky
  boolean cannot decide sweep re-entry, and an owed FIFO deque would change
  delivery order (the sweep delivers in slot-index order) and would allocate
  — violating the declared constraints. The counter is the minimal mechanism
  that preserves exact base delivery order.
- Removal condition: implied by the ledger/README §8 — if the zero-op owed
  workload or sweep shape changes. Recorded.
- Counter integrity: every mutation of `owed_count_`/`event_owed` is paired
  inside one `dispatch_mtx_` critical section (increment
  `src/async/uring_backend.cpp:568-571`; decrement `:662-664`). All sites
  inventoried: `event_owed = true` has exactly one site, `event_owed = false`
  has two (both counter-consistent — the submit reset at `:498` only executes
  after the `:494` fail-fast proved the flag was clear).

Dead/redundant/defensive branch audit — none found. The `if (owed_present)`
guard is the optimization itself. The mutant-gated wrappers compile to a
load+compare in production. The submit-reset fail-fast (`:494-496`) is
defensive for a state unreachable in pristine code, but it is a root-required
contract diagnostic (AGENTS.md explicitly preserves these) and it is
load-bearing: it converts the one drift class that could silently
desynchronize the derived summary into loud termination, and mutant M4 proves
the tripwire fires (receipt exit 134).

Hidden costs examined and bounded:
- "Submit path now takes dispatch_mtx_ twice" — FALSE. Master already held
  the lock at that block (`git show master:src/async/uring_backend.cpp` lines
  ~486-491); the diff adds only a branch inside the existing critical
  section. Same for `publish_zero_op_inline`.
- Extra mutex acquire in `poll()`'s snapshot: when K=0 it replaces `capacity_`
  acquires; when K>0 it adds exactly one. Empirically bounded by the K>0
  zero-burst rows (no regression; §3).
- Cache/layout: one 8-byte member, read once per pass under the lock already
  held for `delivery_` work. The K>0 rows bound any effect at ≈0.
- Binary-level confirmation: `nm` symbol diff of the two campaign binaries is
  exactly one symbol (`UringAsyncBackend::poll()` becomes out-of-line in
  h2b); the inactive `request_core.cpp` mutant seam produces zero codegen
  delta.

## 2. A/B Campaign Methodology — PASS, with findings

Arm identity (verified independently):
- Worktrees: `/tmp/sluice-h2b-ab/base` at `c8970702` (clean, detached) and
  `/tmp/sluice-h2b-ab/h2b` at `4edb957d`. A full `diff -rq` of the two trees
  shows source differences in exactly the branch's six files;
  `bench/native-linux-m2/pipeline_ab.cpp` is byte-identical across arms
  (single shared harness source; per-arm difference is only the linked
  library).
- Campaign binaries: zero ASAN symbols (`nm`/`nm -D` = 0 hits in all four),
  identical NEEDED libs, symbol tables differ by one symbol. The measured
  artifacts are clean and minimally different.

FINDING B1 (stale provenance, low): the README §6.1 claim
"`sanitizers = ""` (verified in both worktree configs)" was true at build
time but the surviving base worktree config now reads `sanitizers = "asan"`
(mutated after the campaign by the §5 sanitizer incident). The campaign
script records no hashes, so the surviving artifacts cannot themselves
substantiate the sanitizer claim; binary inspection compensates. The E5
script should restore the `arm-identity` hash step.
→ ADDRESSED in `1fb4c9c2`: `run_e5_ab.frozen.sh` records per-arm binary and
library SHA-256 and refuses a sanitizer-tainted config; README §6.1 carries
the provenance note.

Paired design: 330 timed rows = 11 configs × 15 reps × 2 arms, zero
unpaired. Rotation balanced; within-pair position effect measured directly
(median ratio differs by 0.1–2.3 pp between base-first and h2b-first
subgroups — small but real, exactly why rotation was required; adequate).

Fixture reuse: single 64 MiB read-only fixture, page-cached, opened fresh per
phase, zero writes during measurement; byte oracle in a separate `oracle`
phase, campaign aborts on oracle failure. All 270 oracle rows ok;
`bytes == requests × block` on every one; `completed_ops == requests`, no
errors, 0 allocations both arms.

Metric sufficiency: `cpu_ns_per_op` is `CLOCK_PROCESS_CPUTIME_ID`
(`bench/bench_common.hpp:44`) — the right primary metric for eliminating
mutex-guarded scans, with `wall_s` agreeing to <0.1 pp at every median.
Hardware counters genuinely unavailable (perf_event_paranoid=2; the empty
`results/perf/stat_*.txt` attempts recorded) and correctly deferred to E5.
Sufficient for a provisional gate; attribution-grade evidence needs the E5
counters.

Higher `poll_calls` on h2b — verified expected: in the structurally bounded
zero-burst configs poll counts are identical across arms (2501/2501,
314/314); in the data configs they rise because each poll got cheaper and the
driver iterates more per fixed 20000-op workload. Outcome of the mechanism,
not a confound.

FINDING B2 (minor): regression criterion is median-only with min/max, no
dispersion/sign rule.
→ ADDRESSED in `1fb4c9c2`: the E5 gate states median ≤ 1.03 per config AND a
paired sign test (≤ 10/15 pairs above 1.03) in advance.

Median verification (recomputed from raw `ab.jsonl` with throwaway scripts in
/tmp): every number in `ab-summary.json` reproduces exactly. 870/870 rows ok.

Internal validity (strongest evidence in the record): dose-response at
4 KiB/d=1 — base cpu_ns/op median grows 8572 → 9228 → 10843 → 16949 from
C=16→1024 while h2b stays nearly flat (8392 → 8511 → 8777 → 9725). The
treated arm is capacity-independent to within the residual poll cost; the
removed O(C) term is precisely what the measurement isolates.

## 3. Negative Performance Workload — PASS

- K>0 zero burst (block=0, capacity=1024): d=8 median 0.9361, h2b faster in
  15/15 pairs; d=64 median 0.9783, h2b faster in 11/15, 4 reps above parity
  (max 1.141) at 5–7 ms run lengths — noise-scale, not systematic. The
  harness makes every poll a true K>0 case, so this genuinely prices the
  added snapshot+increment/decrement: baseline kept, slightly improved.
- 1 MiB C=64 d=1 (median 1.0019, range 0.590–1.215): neutral is the correct
  reading. Sign test 7/15; base's own wall spread is 1.87× with only 500
  requests; extreme ratios are unpaired single-arm excursions; poll counts
  identical. Reported with the caveat, not hidden.

## 4. Claim Structure Honesty — PASS

`status=PROVISIONAL_WSL2_NOT_E5` in script and provenance; README §6.1 same
label plus limitations; formal E5 acceptance NOT_RUN in README §6.2 and the
ledger PROG-01 row (owner column "owner (E5 run)"), matching the M2
precedent. Mutant receipts genuine and matching (142/1/1/134/134), including
the honest M1 detail (in-test `alarm(120)` watchdog).

FINDING B3 (low): the E5 protocol was partially prose.
→ ADDRESSED in `1fb4c9c2`: frozen as `run_e5_ab.frozen.sh`.

FINDING B4 (low): `h2b-production.diff` omitted
`src/async/detail/request_core.cpp`.
→ ADDRESSED in `1fb4c9c2`: diff regenerated with all four production files
(169 lines).

## 5. Summary of Findings

| # | Severity | Finding | Disposition |
|---|---|---|---|
| B1 | Low | Campaign provenance cannot retroactively substantiate sanitizer cleanliness; no hashes recorded | Addressed: E5 script hash step + refusal guard; binary inspection recorded |
| B2 | Low | Median-only regression criterion | Addressed: E5 acceptance rule fixed in advance (median + sign test) |
| B3 | Low | E5 protocol partially prose | Addressed: frozen runnable script |
| B4 | Low | Production diff record incomplete | Addressed: diff regenerated (4 files) |

No minimality violation, no dead or defensive-for-unreachable branch, no
hidden cost the negative workloads do not bound, and no evidence of arm bias,
fixture effects, or summary fabrication.
