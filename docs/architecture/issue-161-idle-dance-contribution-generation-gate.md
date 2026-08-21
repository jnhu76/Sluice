# Issue #161 — Idle-Dance Contribution Generation: Compliance Gate

**Change class:** post-freeze evidence-derived correctness fix (Issue #161).
**Baseline master SHA:** `1a6de37` (branch `fix/issue-137-review-corrections`,
tree clean at investigation start).
**Generic gate:** `docs/architecture/design-compliance-gate.md` (this document
covers every Gate 0–4 field for the fix and links back).
**Formal suite:** `spec/tla/e12_rwlock_scheduler_liveness/` +
`scripts/formal/verify-e12-sched-liveness.sh` (manifest id
`e12-rwlock-scheduler-liveness`).

**One-line defect:** a not-last idle dancer's `idle_workers_` contribution
can be erased by an idle reset that bumps no generation — the pop-path
store, the mw_s1 fall-through store (both **unlocked**), or the
route-publication erase in `route_runnable_locked` (G-held but originally
classified non-genuine) — between the dancer's contribution and its park
commit; the R4 backstop (`idle_workers_ > idle_dance_contributed_`, commit
`17907b1`) cannot distinguish the dancer's STALE 1-bit contribution flag
from the eraser's FRESH count, and the eraser's not-last signal (E9-LIFE-8)
can be absorbed by the dancer's still-unarmed park baseline. Both workers
then sleep in the unbounded wake-domain park with `idle < live`, no work,
no waits, no terminate — `run_impl`'s `join` hangs **after every fiber
completed**.

**Split-window model round (this revision):** modeling the C++ erase as the
two distinct steps it performs (`exchange(0)` then the conditional
`dance_epoch_` bump) falsified the original B4 verdict that the
route-publication erase was self-guarded — with the split steps, TLC finds
the M4 stuck shape with the ROUTE erase as the orphaning site
(`E12SchedLivenessB4NoBumpPubErase.cfg`, now a negative gate). The repair
was extended to the third site with the same exchange/bump discipline, and
the refinement argument below was rewritten as the honest dichotomy (the
previous monotonicity argument claimed the commit's `idle_workers_` acquire
load makes its `dance_epoch_` load observe the eraser's later bump — false
under the C++ memory model: the two are distinct atomics with distinct
modification orders, and no acquire on one can be claimed to see a later
release on the other).

---

## Gate 0 — Architecture Classification

```text
Affected capability:    Scheduler idle-dance termination convergence
                        (worker_loop dance + park commit recheck)
Affected layer:         src/async/scheduler.cpp (:550/:582 reset sites,
                        :1035-1093 dance, dance-epoch record),
                        src/async/scheduler_park_wake.cpp (:295-338 commit
                        refusal), include/sluice/async/scheduler.hpp
                        (field contracts + the new generation field)
Classification:         Corrective (evidence-derived; restores the E9-LIFE-8
                        convergence obligation the R4 backstop intended)
Governing ADR:          ADR-execution-model.md §9.4.0 (coordinated
                        termination / idle dance), §9.4.5 (park/wake epoch
                        protocol) — the fix adds a CONTRIBUTION-generation
                        discipline analogous to the wake-epoch/baseline
                        discipline; the ADR's termination contract is
                        unchanged in outcome (Drain runs return)
Conformance map change: no state-machine authority change; one wake-side
                        refusal condition gains a validity term
Constitution rules:     AC-4 (accepted work must be observed to completion —
                        here the OBSERVER-ACCOUNTING for termination), AC-6
                        (state first, then wake; persistent state authority)
```

## Root cause (PROVEN — TLC counterexample, suite M4 gate)

21-state trace (`E12SchedLivenessM4.cfg`, `DrainStuckState` violated; the
`:582` variant under `E12SchedLivenessB4NoBumpMwS1Erase.cfg`):

```text
W0 classifies QUIESCENT while W1 sits in the pop window
  (R2 popped -> invisible; running_fiber_count_ not yet incremented)
W0 DanceContribute not-last: idle 0->1, signal S_x, contributed[W0]=1
W1 :550 pop-path erase:      idle 1->0   [W0's contribution ORPHANED;
                              contributed[W0] stays 1 — stale 1-bit flag]
W1 runs WF (grants R2) and R2 to completion   [ALL WORK DONE]
W1 re-dances not-last (prev=0), signal S_y, parks (baseline >= S_y)
W0's delayed ParkCommit: ProgressPending=FALSE;
  idle_workers_(1) > idle_dance_contributed_(1) is FALSE
  -> arms baseline := current epoch   [ABSORBS S_y]
BOTH PARKED: idle=1 < live=2, terminate=false, no leave enabled
  -> stutter forever; run_impl join hangs with r2_acquired=true
```

This is the exact CI hang state (both workers at `park_wake.cpp:379`
unbounded wait, main in `join` at `scheduler.cpp:457`, no assertion
reached). Lineage: #115 = "parked worker misses a runnable publication →
stranded runnable"; #161 = "work all completed, the dance cannot converge
to terminate" — same persistent-state-before-notify family, different
state; neither proves the other.

## Gate 1 — Ownership and State Machine

No new lifecycle object. The idle dance gains a **contribution identity**:

```text
contribution generation (dance epoch): monotonic atomic counter, advanced
  ONLY by the three GENUINE idle resets (B4 classification, re-proven by the
  split-window model round): the pop-path erase, the mw_s1 fall-through
  erase (both unlocked), and the route-publication erase in
  route_runnable_locked (G-held; reclassified from "self-guarded" — the
  pre-split model's coarser interleaving had masked its trace). The
  remaining reset sites (:958-family recheck, :1065-family reset-continue,
  the run boundary, the Live-resident reset, the MW-S2 sites) are
  self-guarded and never bump. Each bumping site is an exchange(0) that
  advances the generation only when a contribution was actually erased.
dancer record: the generation loaded STRICTLY BEFORE its fetch_add (same
  global_mtx_ critical section).
park commit: refuses while a LIVE contribution (contributed=1) is no
  longer current — the refusing worker signals the wake domain and
  re-loops (existing refusal path, park_wake.cpp:316-318); the re-dance
  sees the eraser's fresh contribution and converges (prev+1 >= live ->
  LAST -> terminate).
loop-top reset (scheduler.cpp:509) invalidates the record together with
  idle_dance_contributed_ — a never-danced worker must not refuse behind a
  generation it never claimed (observer-delegation park stays legitimate).
```

### C++ refinement of the model's atomicity (honest dichotomy)

The TLA model originally fused each erase into one atomic action
(`EraseIdleBumping`: count reset + generation advance). The C++ sites are
`exchange(0)` then a conditional `dance_epoch_.fetch_add(1)` — **two
distinct unlocked RMWs on two distinct atomic objects**, so the split model
(`EraseIdleOnPop -> PopBumpPending -> BumpPopGen` and
`EraseIdleMwS1 -> MwS1BumpPending -> BumpMwS1Gen`) now models the window
between them explicitly, and TLC confirms the window is REACHABLE: the
witness gate `E12SchedLivenessSplitWindow.cfg` violates
`SplitWindowNeverArmed` — a stale contributor's park commit CAN read the
erased count together with the still-current generation and arm a baseline
without the identity term firing.

Three ordering rules are implemented:

1. the dancer RECORDS the generation strictly BEFORE its `fetch_add`;
2. the eraser bumps strictly AFTER its `exchange(0)` (and only when the
   exchange actually erased a contribution);
3. the park commit loads the generation strictly AFTER its `idle_workers_`
   load (the identity term is the LAST disjunct of the refusal).

Rule 3 does NOT — and cannot — make the generation load observe the bump
whenever the idle load observes the erased count: `idle_workers_` and
`dance_epoch_` are distinct atomics with independent modification orders
(cppreference `std::memory_order`: even `seq_cst`'s single total order does
not fuse two writes on two objects into one atomic step — a reader's loads
can still interleave between them). The earlier draft of this section
claimed exactly that visibility ("the refusal fires"), which is false as a
C++ argument; the witness gate exists to keep that claim falsifiable.

The window is nevertheless safe by a two-case split on what the commit
observes (the honest dichotomy):

1. **generation != recorded** (the bump is visible): the identity term
   refuses; the worker signals and re-dances; convergence by the R4
   conservative argument (refuse -> re-loop -> re-dance with a fresh
   identity).
2. **generation == recorded with the count already erased** (the split
   window): the eraser's protocol is INCOMPLETE — it is between its
   exchange and its bump. The only signal the dancer could sleep through
   is the eraser's LATER not-last dance signal, and that signal is emitted
   under `global_mtx_`, while the park commit holds `global_mtx_` across
   its count load, its generation load, and its baseline arming. Observing
   the erased count additionally proves no re-dance contribution is
   G-visible yet, so the signal is sequenced strictly AFTER the arming:
   the cv predicate fires on it, the park is transient, and the dancer
   re-loops. The window reorders who wakes; it cannot rebuild the terminal
   M4 stuck state, whose mechanism needs the absorbed signal to PRECEDE
   the arming.

The model-side proof that the window costs only a transient park is the
repaired-constants SAFETY gate (all three bumps on, split steps live):
`DrainStuckState` still holds. A naive "bump-then-store" eraser paired
with "record-after-add" would instead open a bump-visible/store-not-yet
window in which an orphaned contribution passes the identity check — the
opposite order is load-bearing.

Authority table (unchanged domains; one new field in the existing wake-side
authority):

| Domain | Authority | Fix interaction |
|---|---|---|
| run-domain queues/owners | `global_mtx_` (+ per-inbox `inbox_mtx`) | untouched |
| wake domain | `wake_mtx_` / `wake_epoch_` / `wake_cv_` | untouched (the absorption mechanism it hosts is closed by the refusal, not by epoch changes) |
| idle dance | `global_mtx_`-serialized fetch_add + the three genuine erase sites | the pop path and mw_s1 fall-through (unlocked) and the route-publication erase (G-held) become `exchange(0)` + conditional generation bump (exchange-then-bump; see the refinement rules above) |
| park commit | `global_mtx_` G-section (recheck then arm) | refusal condition gains the generation-validity term (last disjunct); arm unchanged |

## Gate 2 — Resource and Failure Model

No new container, thread, queue, or allocation. One
`std::atomic<std::uint64_t>` on Scheduler + one per-WorkerState atomic
record. Hot-path cost: the three `idle_workers_` reset sites that were
plain `store(0)` become `exchange(0)` + a conditional generation bump (the
route-publication site runs under `global_mtx_`; the two ticketed sites are
unlocked). Same atomic cost class; the all-idle-zero common path stays
generation-stable (`erased == 0` bumps nothing). Park commit gains one
acquire load + compare under G. No failure path.

A/B cost check (repair-cost evidence only, NOT an optimization claim;
`bench/idle_erase_ab_bench.cpp`, Release, Clang, 2026-08-21, WSL2 / Ryzen 7
5800H, 8 logical CPUs): 1024 trivial fibers pre-spawned per round, only
`Scheduler::run(W)` timed, median of 9 rounds per invocation, 8
invocations per tree, interleaved. Trees: MAST `master@06b072e` (all three
sites `store(0)`), HEAD `eb58205` (sites 1+2 exchange, route site
`store(0)`), CAND = this tree (all three sites exchange+bump).

```text
workers   CAND ns/fiber   MAST ns/fiber   HEAD ns/fiber   CAND/MAST   CAND/HEAD
1            88.4            88.6           84.4           -0.3%       +4.7%
2           420.7           411.1          405.3           +2.4%       +3.8%
4           457.9           442.4          428.7           +3.5%       +6.8%
8           774.7           705.4          670.8           +9.8%      +15.5%
```

Single-worker is indistinguishable; 2/4-worker medians sit inside the
same-tree run-to-run noise band (MAST w4 spanned 376–706 ns/fiber across
invocations); the 8-worker median is +10–15% but the per-invocation
min–max bands overlap heavily (both MAST and CAND show ≥2x outliers at
w8 — 8 workers saturate all logical CPUs under WSL2). Verdict: no
reproducible regression outside the environment's noise band; at worst a
small contended-RMW cost at full saturation, on a path costing hundreds
of ns/fiber in the same environment. Reproduce:
`xmake f -m release --toolchain=clang -c -y && xmake build
idle_erase_ab_bench && xmake run idle_erase_ab_bench`.

## Gate 3 — Progress and Wake Model

The refusal is state-first-then-notify exactly like the existing G1/R4
refusals: the refusing worker calls `signal_wake_locked()` before returning
to the loop. Convergence argument (checked by the suite's liveness gates):
after any invalidating erase, every stale contributor refuses at commit and
re-dances; with `live` workers, at most `live` contributions separate the
system from LAST. No polling, no timeout, no periodic wake introduced; the
unbounded park's soundness precondition ("every quiescence-observation path
wakes the domain") is restored rather than weakened. Single-worker runs are
unaffected (prev+1 >= 1 is always LAST; the refusal is unreachable with
live=1). Live-mode MW-S3 resident parks (the R4 damping analysis) are
untouched — the resident reset (`:1027`) is Live-only and out of the Drain
scenario's scope (documented in the suite README).

Lock order: unchanged. The bump at the three erase sites is a lock-free
atomic (the route-publication site inside its existing G-section); the
dancer's record is inside the dance's existing G-section; the commit
check is inside the commit's existing G-section. No edge is added between
`wake_mtx_` and any inbox.

## Gate 4 — Evidence

```text
FORMAL (re-executed 2026-08-21 after the correction round;
        bash scripts/formal/verify-e12-sched-liveness.sh):
  Positive [repaired] safety, EXACT as-built constants .... PASS (incl.
      DrainStuckState). The three genuine invalidation sites bump (pop,
      mw_s1, route-publication); the :958 recheck and :1065 reset-continue
      sites do NOT bump (C++ store(0)); EraseIdleBumping advances the
      generation only when the erase actually erased a contribution
      (idleCount > 0 — the C++ erased != 0).
  Positive [repaired] liveness, same constants ........... PASS
      (4 temporal properties)
  Split-window safety (all-bumps constants, same as the witness) PASS —
      the window costs only a transient park
  B4 self-guarded sites (:958/:1065) .................... PASS with bump
      disabled
  B4 route-publication erase (:1514) ..................... CEX DrainStuckState
      with the bump disabled (RECLASSIFIED genuine by the split model; the
      old "self-guarded" positive gate was removed)
  M1/M2/M3 composition on repaired base .................. PASS (documented
      in-scope closure; M2's verdict is unified as DOCUMENTED PASS across
      module header, cfg, verifier, and README; e9 carries the classes at
      its abstraction)
  M4 as-built safety ..................................... CEX DrainStuckState
      (the 21-state trace above)
  M4 as-built liveness ................................... CEX (fail-closed
      temporal grep — TLC's plural wording "Temporal properties were
      violated." is matched, and a crash/parse failure no longer
      masquerades as a witness)
  M5 grant-without-ticket ................................ CEX liveness
  B4 ticketed sites (pop path, mw_s1) ................... CEX DrainStuckState
      when the bump is disabled
  Split-window witness (all bumps on) ................... CEX SplitWindowNeverArmed —
      the exchange(0)-before-bump window is REACHABLE in the model; the
      split-window safety gate above proves it costs only a transient park

C++ (first fix round — executed 2026-08-21, Clang Debug unless noted;
     commands verbatim):
  [x] baseline Clang Debug full gate (pre-change) — 187/187 PASS
      (xmake build sluice_core; sluice_async; -g test; xmake test -v)
  [x] deterministic reproducer FAILS on pre-fix tree — 5/5 identical:
      xmake run issue161_idle_dance_orphan_test ->
        [issue161] stall evidence: worker 0 park_domain=Scheduler
        [issue161] stall evidence: worker 1 park_domain=Scheduler
        (both last_classify=3/quiescent) FAILED 1 check (progressed);
      fail-closed watchdog + external-wake-handle rescue, zero
      sleep/yield ordering (per-worker seams only)
  [x] reproducer PASSES post-fix — 10/10 Debug + 30/30 ASan runs;
      with the identity term disabled in isolation the rewritten test
      fails 10/10 under ASan (the regression bite is retained);
      async_rwlock_test (incl. T22 rwlock_mw_cancel_and_unlock_on_
      different_workers), issue115, phase_g_backend_progress_wake,
      phase_g_closeout, multi_worker_coord, multi_worker,
      scheduler_progress, external_wake, wake_handle_lifetime,
      scheduler_ready_flag, scheduler_wait, select_multi_worker — all PASS
  [x] Clang Debug full suite post-fix — 188/188 PASS (xmake test -v)
  [x] TSan full suite — PASS, 0 race reports, reproducer included
      (xmake f -m tsan; build -g test; xmake run -g test)
  [x] Clang Release full suite — 188/188 PASS (§16.1; xmake f -m release;
      build sluice_core sluice_async -g test; xmake test -v)
  [x] ASan+UBSan full suite — PASS, 0 sanitizer reports, reproducer
      included (§16.2; xmake f -m asanubsan; build; xmake run -g test)
  [x] mechanical gates — pre-push.sh ALL CHECKS PASSED (working-tree
      mode), check-doc-links PASS, verify-architecture-docs PASS,
      mechanical-facts PASS (incl. seam-production-exclusion + updated
      LOC/test-total rows), assert-hygiene PASS,
      verify-completion-authority-negative-compile 12/12 PASS,
      verify-request-arena-negative-compile 6/6 PASS, git diff --check
      clean; formal verifier re-run post-change: === PASS ===
      (bash scripts/formal/verify-e12-sched-liveness.sh)

C++ (split-window round — the pub-site repair; executed 2026-08-21):
  [x] pub-site deterministic reproducer FAILS with ONLY the
      route-publication bump reverted to store(0) (the two ticketed sites
      keep the fix — the construction isolates the third site):
      xmake run issue161_pub_erase_orphan_test ->
        [issue161-pub] stall evidence: worker 0 park_domain=Scheduler
                        last_classify=2 (mw_s3)
        [issue161-pub] stall evidence: worker 1 park_domain=Scheduler
                        last_classify=3 (quiescent)
        FAILED 1 check (progressed); watchdog + baseline-seam release +
        external-wake rescue converge the run
  [x] reproducer PASSES post-fix — 10/10 Debug runs; deterministic
      barriers are persistent state only (fw_locked flag set after the
      write lock is held, fr FiberState::waiting once queued, seams);
      the per-worker park domain is NOT used as a barrier (it stays
      stale after a wake — an earlier draft's false barrier armed the
      pop seam one pass early and silently pinned the wrong pop)
  [x] issue161_idle_dance_orphan_test (site-1) still PASSES post-fix
  [x] full-suite re-runs for the split-window tree (2026-08-21):
      Clang Debug — PASS 189/189 (xmake test -v); Clang Release —
      PASS 189/189 (§16.1); TSan — PASS 189/189, 0 race reports (§16.3);
      ASan+UBSan — PASS 189/189, 0 sanitizer reports (§16.2);
      negative-compile probes 12/12 + 6/6 PASS; pre-push.sh
      ALL CHECKS PASSED; formal verifier re-run: === PASS ===
```

Reproducer robustness corrective (found by the ASan run of the gate
itself): the initial choreography hardcoded "worker 0 runs F1" — false
under a startup steal (the two workers race the first pop; ASan timing
flipped the roles ~50%). The corrected test DISCOVERS the roles from
F1's `current_worker_id()` (#115 discipline) and every choreography wait
is bounded and fail-closed, so a broken construction reports state and
fails instead of blocking the suite (the broken-role run manifested as
`std::terminate` from unwinding over the joinable runner thread).

## Zig conformance / divergence

Unchanged; no Zig model binds the scheduler idle dance. Corrective fix —
the divergence registry gains no row.

## Exit-path audit

The park/wake exit paths are unchanged (mw_s1 terminate observed, final
park, retire epilogue, all-workers-joined). The new refusal is a re-loop,
not an exit; termination authority remains exclusively the last-idle dance
recheck (`:1040-1047`) and the MW-S2 no-progress path.
