# Sluice #621 Round Report — ThreadPool owed-count gate (application path: sluice-copy)

Date: 2026-11-12. Verdict summary: **MECHANISM: GO · END_TO_END (this consumer): GO** — see §5.

## 1. Repository reality

| Item | Value |
|---|---|
| BASE_SHA (A) | `f2c425351d8bf14563a6f2d4972ea97ccf915cfd` (verified == origin/master at execution time; clean tree) |
| B commit | `de9b452a` on branch `research/621-tp-owed-count` in worktree `/home/jnhu/Source/Sluice-exp-f2c42535`; diff `b_owd_count.diff` sha256 `c2187b02fbab26ca…` (5 files, +496/−12) |
| Binaries (release, xmake `mode.release`) | A `7df5c8571310a…`, B `fb5123a1161a8…` (clean-verified reproducible; a mid-round contamination was caught by hash cross-check and rebuilt clean — see note below) |
| Host | Intel Xeon E5-2666 v3 (10C/20T, 1 socket), Fedora 44, kernel `7.2.11-200.fc44.x86_64`, 62 GiB RAM; governor `performance` during measurement, restored `schedutil` after; affinity `taskset -c 2,3` (two distinct physical cores; SMT pairs are 2,12 / 3,13) |
| Toolchain | gcc (Fedora 44), xmake, liburing capability on for tests; perf with sudo (perf_event_paranoid=2) |
| Filesystem | btrfs `/home` (device FS, warm page cache); source 256 MiB sha256 `1a6a11ed3b953a87…` |
| Consumer | `sluice-copy` (apps/sluice-copy) — the repo's real executable application with an output oracle; it uses **RequestScope + ThreadPoolBackend only** (`copy_task.cpp:299`) |
| Owning issues | #621 (this round), #604 (application gains), #632 (buffer handoff — sluice-copy is its H0 negative control) |
| Historical evidence | PR #618 six eliminations (product baseline); key-path-593 K1 lane (uring publication drain, NO_CHANGE_JUSTIFIED under 4.79% band); uring `owed_count_` already production (`uring_backend.hpp:304`); no prior veto covers the ThreadPool owed scan |

Contamination note: the first "A" build was made while B edits were uncommitted in the worktree
(git checkout carried them over). Detected via binary hash (equaled B), fixed by committing B
first, `xmake clean --all`, and rebuilding A from pristine `f2c42535` (hash-stable across two
clean rebuilds). All reported numbers use the corrected binaries.

## 2. Cost ledger and candidate selection

Equivalent application task (frozen before measurement): copy 256 MiB (or 64 MiB in profiling)
`src → dst` on btrfs, warm cache, `--workers 1 --sync none --no-atomic`, fixed buffer/depth per
cell; completion = byte-identical dst (`cmp`), app-reported `read_ops/write_ops` exactly the
measured deterministic counts (65540/65536 @4 KiB d4; 4097/4096 @64 KiB d1), exit 0.
Durability endpoint: `sync none` (app policy) — identical across arms; no partial-effect or
cancel divergence reachable in this task shape.

Per-request path (sluice-copy, ThreadPool, workers=1), with file:line + symbol and per-task
frequency (task = 131,076 requests @4 KiB d4):

| Step | Location | Responsibility | Frequency | Direct evidence |
|---|---|---|---|---|
| App submit read/write | `copy_task.cpp:66-88` submit_slot_read/write | semantic (task DAG) | 131,076 | source |
| Context admission | `async_io_context.cpp:249-269` submit_read (access_mtx_) | semantic (admission, THREAD-01) | 131,076 | source + profile (mutex family) |
| Backend validate/reserve/accept | `threadpool_backend.cpp:212-297` submit_request; `request_core.cpp:165-210` | semantic + physical (slot, borrow) | 131,076 | source |
| delivery-record init + dispatch push | `threadpool_backend.cpp:261-267, 299-332` (2× work_mtx_) | physical (queue handoff) | 131,076 | source; part of ~9% mutex blob |
| Worker dequeue/claim | `threadpool_backend.cpp:383-410` worker_loop | physical (execution claim) | 131,076 | source |
| Syscall pread/pwrite | `threadpool_backend.cpp:478-541` run_syscall | **required physical** | 131,076 | profile: kernel syscall-entry family ~10-14% |
| Worker terminal/execution-release/publication push | `threadpool_backend.cpp:441-463`; `request_core.cpp:346-409` | semantic (terminal, exactly-once) | 131,076 | source |
| Progress signal | `threadpool_backend.cpp:603` → `progress_source.hpp:252` signal (+eventfd write) | physical notification | 131,076 | source |
| Driver pass: batch pop + **owed sweep** | `threadpool_backend.cpp:556-576` poll_progress | **candidate: sweep is O(request_capacity=64) with zero owed events on this path** | 131,076 passes | perf: operator[] 2.88% + size() 1.58% + part of poll_progress self 2.08% |
| Driver pass: **any_event_owed scan** | `threadpool_backend.cpp:596-597, 824-831` | **candidate: second O(64) scan per pass** | 131,076 | perf: any_event_owed_locked_ 1.35% |
| Publication + observer + sink | `threadpool_backend.cpp:353-381` publish_one/deliver_event; core begin/complete/claim/retire | semantic (REQ-04/OBS-02) | 131,076 | source; core mutex family |
| Park/wake | `progress_source.hpp:104-232` wait_if_unchanged (poll(2), eventfd drain) | physical notification | ~131k parks | profile: __poll under wait_if_unchanged |
| Result take + slot reclaim | `request_scope.hpp:131-143` take_; `request_core.cpp:323-344,580-631` | semantic (generation, exactly-once reclaim) | 131,076 | source |

Ranked candidates (max 3 per protocol):

1. **α (selected): gate the two per-pass O(request_capacity) delivery-record scans behind a
   `work_mtx_`-owned derived owed count** — a mechanism that is **already production on the
   uring backend** (`owed_count_`, `uring_backend.hpp:304`, gates drain at
   `uring_backend.cpp:661` and hot residue verdict at `:313` via `owed_visible_locked_`).
   ThreadPool alone kept the unconditional scans. Measured: ~5.8–7.9% of samples (scan family)
   at 4 KiB/d4, ~3% at 64 KiB/d1; second-order benefit: shorter `work_mtx_` critical section
   (contended unlocks visible in A's profile).
2. β: merge the two `work_mtx_` acquisitions in `submit_request` (delivery init + dispatch push).
   Est. ~0.5% (1 of ~20 acquisitions/op); lock-domain reasoning burden; not selected.
3. γ: merge the triple `access_mtx_` acquisition at `wait_one` entry. Context-owned, est. <0.5%;
   not selected.

Rejected entry points with reasons: uring submit amortization (#595 paused; strict re-entry),
uring `offer_terminal→release_execution` fusion (#621 P2 gate; #622 E5 shows reap side is not
the top lever), ThreadPool thread-handoff restructuring (prompt gate: handoff elimination needs
its own exposure proof; α touches no handoff, thread topology identical), sluice-copy app-level
copy elimination (no copy exists — reads write directly into app-owned slot buffers;
`copy_task.cpp:67-69, 79-81`).

Single hypothesis: on the ThreadPool driver thread, the two O(64) delivery scans per progress
pass are avoidable machine work when no owed events exist (the entire normal-path regime —
only zero-length ops create owed events, `threadpool_backend.cpp:347`).

A deletes nothing; B deletes (per pass, when `owed_count_==0`): the 64-iteration collection
sweep (threadpool_backend.cpp:567-575) and the 64-iteration `any_event_owed_locked_` scan
behind `immediate_work_remains` (:596-597). B adds: one counter increment per zero-op
(rare), one decrement per collected owed event, one load per pass. Responsibilities retained:
the delivery records remain the authority; settlement paths (`internal_work_retired` :836,
`retire_execution_resources` :866) keep the authoritative scan — mirroring uring exactly.

Falsifiers: (i) parity oracle (counter vs scan vs sink deliveries) fails in new suite or any
existing test; (ii) paired A/B CPU/op improvement not distinguishable from the A/A band;
(iii) B's profile still shows the scan family. None fired (§3, §4).

## 3. Semantic evidence

Change surface: `ThreadPoolBackend` only (backend-local execution policy under Architecture A);
RequestCore, ProgressSource, uring backend untouched. Public API unchanged.

Contract mapping and linearization points (all counter mutations are inside the same
`work_mtx_` critical sections that mutate the paired `event_owed` flag — before/after are
identical lock domains):

- **Creation**: `publish_zero_op_inline` — `{event_owed=true, owed_key=id, ++owed_count_}` in
  one CS (`threadpool_backend.cpp:344-352`). Control-ref (`acquire_control` :335) still pins the
  slot before the flag is set.
- **Collection**: `poll_progress` batch CS — `{event_owed=false, --owed_count_}` per record
  (`:566-580`); gate `owed_count_!=0` read inside the same CS. Pass bound unchanged
  (`pass_bound` at entry; owed collection still bounded by capacity_). PROG-01 preserved.
- **Residue verdict**: `immediate_work_remains` reads `owed_visible_locked_()` under `work_mtx_`
  post-pass (`:597-601`) — same lock, same position as the old scan. PROG-01/03 preserved; a
  zero-op arriving mid-pass (producer CS) is visible to the verdict exactly as before.
- **Settlement**: `internal_work_retired` / `retire_execution_resources` keep the authoritative
  `any_event_owed_locked_` scan (mirrors uring `:346,369`). LIFE/teardown verdicts unchanged.
- **Borrow/slot safety**: owed event ⇒ `control_refs ≥ 1` ⇒ `reclaimable_` false
  (`request_core.cpp:580-590`) ⇒ the core cannot re-free a slot whose owed event is uncollected
  ⇒ submit-time record re-init can never wipe an uncollected owed event (the invariant that
  keeps the counter and records in lockstep). Generation/REQ-02..06, OBS-02, CANCEL paths
  untouched (cancel never writes `event_owed`; grep-verified exhaustive writer list).
- Mechanism-justification classification: `owed_count_` is a **derived summary** (owner:
  `work_mtx_` domain; invariant: equals the count of `event_owed` records), materialized to make
  the common-case pass O(1); replaces the unconditional scans on hot-path verdicts only;
  replaces no authority. Identical classification to production uring `owed_count_` and the
  key-path-593 K1 lane's description of it ("the existing derived counter").

Most dangerous interleaving (old vs new identical): a zero-length submit racing a driver pass
between its sweep and its residue verdict. Pinned deterministically by the new
`owed_during_pass_is_counted_and_reported` case using a test-only pause gate
(`OwedSweepEpiloguePauseGate`, zero production code when `SLUICE_ASYNC_INTERNAL_TESTING`
undefined).

Oracle evidence:

- New `tests/threadpool_owed_counter_parity_test.cpp` (8 cases): counter/exact-count-scan/
  existence-scan/sink parity at quiescent checkpoints (the oracle asserts
  `owed_count_for_test() == owed_scan_count_for_test() == expected` plus the boolean
  production scan; strengthened in the final-repair round), zero-op tracking, data-op
  indifference, full-capacity exactly-once, slot-reuse-before-delivery (control-ref
  pinning), cancel of already-terminal zero-op, during-pass owed counting + residue
  verdict, 500-op concurrent producer exactly-once. **BASE GREEN** (all 8).
- Named mutants (builds registered in `xmake/tests.lua`, manual per house convention), each
  **RED** (non-green exit), **RESTORED GREEN** after revert:
  - `threadpool_owed_mut_missing_increment` (`SLUICE_THREADPOOL_MUTANT_OWED_COUNT_MISSING_INCREMENT`):
    parity violated at submit; settlement teardown deadlocks (owed events invisible to the gated
    sweep) — killed by SIGALRM watchdog; identical exposure shape to the uring mutant.
  - `threadpool_owed_mut_missing_decrement` → RED at `counter_tracks_zero_op_reads` (post-delivery parity).
  - `threadpool_owed_mut_empty_verdict_always` → RED at `owed_during_pass_is_counted_and_reported` (residue verdict).
  - `threadpool_owed_mut_reclaim_ignoring_control_refs` (shared core mutant) → RED via fail-fast (control-ref pinning is load-bearing for the counter invariant).
- Existing suites: `threadpool_progress_race_test`, `threadpool_progress_pass_bound_test`,
  `threadpool_external_loop_test`, `progress_notification_suppression_test` PASS.
- Full configuration matrix: release/liburing=y **80/80**, release/liburing=n **59/59**,
  debug/liburing=y **80/80**, debug/liburing=n **59/59** — 0 failures.
- Sanitizers: TSAN and ASAN clean on `threadpool_owed_counter_parity_test` and
  `threadpool_progress_race_test` (plus external_loop/pass_bound/suppression under TSAN).
- Workload oracle independence: per-run `cmp` byte identity + deterministic op counts + exit 0;
  oracle power verified by flipping one byte in an output (rejected, `ORACLE_REJECTS_corrupt_ok`).

Out-of-scope matrix cells (with reason): multi-worker shapes (mechanism is driver-thread-local;
workers never touch delivery records — unchanged code path); zero-op-heavy production workloads
(correctness covered by parity suite; perf of the owed>0 branch intentionally unmeasured — the
scan still runs there); uring cells (backend untouched; its counter already exists).

## 4. Results table

Protocol (frozen before formal runs): 24 interleaved pairs per set (even A→B, odd B→A),
one task per run, `sync` + settle before every run (normalizes btrfs dirty-page state; without
it, unrelated writeback stalls leak into wall time — observed and designed out), source kept
page-cache-warm, `taskset -c 2,3`, governor `performance`. Primary metric: process rusage
CPU (user+sys) per completed 256 MiB copy. Statistics: per-pair relative difference
(Y−X)/X, bootstrap 95% CI, 10,000 percentile resamples, seed 5950 (#595 convention).
CPU accounting: **PARTIAL_CPU_ACCOUNTING** — scope is the process (app thread + I/O worker
thread via rusage); excluded: IRQ/softirq, kernel writeback/flush threads (io-wq N/A —
ThreadPool pread/pwrite execute in-process). The excluded components perform byte-identical
work in both arms, so the paired difference isolates user-space coordination.

| Set | n (valid/24) | CPU mean A | CPU mean B | CPU reldiff | 95% CI | wall reldiff [CI] |
|---|---|---|---|---|---|---|
| A/A 4 KiB d4 (calibration) | 24 | 1.471 s | 1.462 s | −0.58% | [−1.18, +0.03]% | −0.33% [−1.28, +0.64]% |
| **A/B 4 KiB d4 (main)** | 24 | 1.467 s | 1.385 s | **−5.58%** | **[−6.45, −4.71]%** | −6.53% [−7.55, −5.53]% |
| A/B 64 KiB d1 (negative control) | 24 | 0.173 s | 0.173 s | +0.10% | [−1.56, +1.77]% | −0.69% [−1.96, +0.60]% |

Every run passed the oracle (exit 0, byte identity, exact op counts, 268,435,456 bytes);
zero exclusions; no oracle failures.

Tails (per-arm empirical percentiles): main cell CPU p50/p95/p99: A 1.470/1.498/1.500 →
B 1.390/1.418/1.435 (all improved); wall p99 A 1.088 → B 1.018. Negative control: identical
distributions. Negative control behaves as pre-registered: at 16× fewer requests per byte the
scan work is below the noise band and the cell shows no statistically resolved regression (the
round's frozen protocol pre-registered the cell and its directional expectation, not a
non-inferiority margin; the CI upper bound +1.77% cannot exclude a regression that small).

Mechanism metrics (profiler runs, separated from clean timing):
- User-space instructions per task: A ≈ 898.5 M (901.9/883.8/909.8) → B ≈ 700.0 M
  (700.9/694.1/705.1), **−22.1%** (≈6,854 → ≈5,342 instr/request; consistent with
  2 × 64-record scans + loop scaffolding per pass). task-clock −7.5%, context-switches 0 in
  both (no topology change).
- B's clean profile: the scan family (`DeliveryRecord::operator[]`, `vector::size()`,
  `any_event_owed_locked_`) — 5.8% of A's samples — **disappears below the 0.7% threshold**;
  remaining top symbols are the physical syscall path and the (untouched) mutex machinery.
- A's hotspot evidence (clean baseline, independent profile, 2 shapes): scan family
  2.88+1.58+1.35 ≈ 5.8% (+poll_progress self) at 4 KiB d4; ≈3% at 64 KiB d1; mutex
  machinery ≈9–11% (next candidate, not this round).

Reproduction entry points:
- Binaries: `/home/jnhu/sluice-621-round/bin/sluice-copy.{A,B}` (sha256 in `MANIFEST.sha256`).
- Runner: `/home/jnhu/sluice-621-round/run_paired.sh <A> <B> <label> 24 4096 4 65540 65536`.
- Analysis: `/home/jnhu/sluice-621-round/analyze_paired.py results/*.tsv`.
- Raw data: `results/{aa_4k_d4,ab_4k_d4,ab_64k_d1}.tsv`, perf data
  `results/perf_4k_sym.data` (A), `results/perf_B_4k.data` (B); diff `b_owd_count.diff`.
- Tests: `xmake build threadpool_owed_counter_parity_test && xmake run …`; mutants via the
  four `threadpool_owed_mut_*` targets.

## 5. Decision

- **MECHANISM: GO.** The hypothesis named an avoidable cost (two O(request_capacity) scans per
  progress pass on the driver thread, zero owed events on the entire normal path), a single
  deletable intervention (derived owed count gating both scans; production-proven on uring),
  and it was confirmed causally: paired CPU −5.58% [−6.45, −4.71] vs an A/A band of ±1.2%,
  −22% user instructions, and the scan symbols vanishing from B's profile. Correctness gates
  all green (parity oracle, 4 mutant receipts, 278-run config matrix, TSAN/ASAN).
- **END_TO_END (this consumer, this host): GO.** For the first time in this campaign the
  measured cell is a real executable application (sluice-copy) — not a synthetic W-02/W-03
  shape — and the improvement is in the application's own task metric (CPU and wall per
  completed, oracle-verified 256 MiB copy), with a pre-registered negative control showing
  no statistically resolved regression where the mechanism shouldn't act. Scope limits: btrfs + this Xeon host +
  workers=1 + warm cache; wall-time claims require the sync-discipline described above.
  This is a workload-local win, not a library-wide claim; no cell regressed.
- Residual gaps (honest): multi-worker and zero-op-heavy shapes unmeasured (mechanism-neutral
  by construction; correctness covered); ext4/xfs and other hosts untested — a different FS
  changes syscall mix, not the deleted user-space work; kernel-side CPU (writeback) not in
  rusage — identical across arms, so it cannot explain the paired delta.

Three first-round questions:
1. **What work did the equivalent task stop doing?** Per progress pass on the driver thread,
   two full 64-record sweeps of the `delivery_` table (collection sweep + any-owed verdict)
   that provably found nothing on this path (only zero-length requests create owed events);
   ≈1,512 user instructions/request, ≈5.6% of task CPU at 4 KiB buffers, plus the
   `work_mtx_` hold-time relief that comes with the shorter critical section.
2. **Which correctness duties are still guaranteed, and by what?** Delivery obligations are
   still owned by the `delivery_` records + core control refs (owed event pins its slot:
   `reclaimable_` requires `control_refs==0`); exactly-once settlement, generation checks,
   pass bounds, no-lost-wake parking and teardown quiescence are all unchanged (settlement
   keeps the authoritative scan); the counter is a non-authoritative summary whose invariant
   is enforced by the parity suite and four named mutants.
3. **Under what conditions do the gain and any regression hold?** Gain holds where
   requests-per-byte is high (small buffers) and owed events are absent (all non-zero-length
   work); it fades below noise as buffer size grows (negative control at 64 KiB d1: +0.10%
   [−1.56, +1.77], i.e., no regression). No measured cell regressed; the owed>0 branch keeps
   the full scan so zero-op-heavy workloads pay only the counter maintenance (unmeasured,
   bounded by one increment per zero-op).

## 6. Product handoff

The full candidate diff is `/home/jnhu/sluice-621-round/b_owd_count.diff` (= branch
`research/621-tp-owed-count` @ `de9b452a`, 5 files, +496/−12 — of which 396 lines are the new
parity test and 30 are xmake wiring; production change is ~62 lines in three files, including
mutant seams that compile to nothing in product builds).

Affected conformance obligations (for the PR description): PROG-01 (bounded pass, honest
residue), PROG-02/03 (park/no-lost-wake — signal path untouched), REQ-04/05 (publication/
observation unchanged), LIFE-*/teardown (settlement keeps authoritative scan), THREAD-01/02
(no host hooks under locks; no new lock domains). Ledger: add one row under the progress/
backend obligation group citing this report.

Suggested owning-issue writeback draft (for #621/#604, not posted — no GitHub write
authorized this round):

> ThreadPool per-pass owed-event scans gated behind a work_mtx_-owned derived count
> (production mirror of uring owed_count_). Real-consumer evidence: sluice-copy 256 MiB,
> 4 KiB/d4/w1, btrfs, 24 interleaved AB/BA pairs — CPU/task −5.58% [−6.45, −4.71]%,
> wall −6.53% [−7.55, −5.53]%, instructions −22.1%, A/A band ±1.2%; negative control
> (64 KiB/d1) no statistically resolved regression (CI upper +1.77%).
> Parity suite (8 cases) + 4 mutant receipts + 278-run config
> matrix + TSAN/ASAN green. Branch research/621-tp-owed-count @ de9b452a (BASE f2c42535).

Product acceptance gaps before merge: independent review of the two-counter-invariant comment
accuracy, ledger row, and (per GOV-02) the reviewer's own re-run of the parity suite and one
mutant on their host; nothing else is believed missing.
