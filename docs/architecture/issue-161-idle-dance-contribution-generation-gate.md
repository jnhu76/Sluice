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
can be erased by an **unlocked** idle reset — the pop-path store
(`src/async/scheduler.cpp:550`) or the mw_s1 fall-through store (`:582`) —
between the dancer's contribution and its park commit; the R4 backstop
(`idle_workers_ > idle_dance_contributed_`, commit `17907b1`) cannot
distinguish the dancer's STALE 1-bit contribution flag from the eraser's
FRESH count, and the eraser's not-last signal (E9-LIFE-8) can be absorbed
by the dancer's still-unarmed park baseline. Both workers then sleep in the
unbounded wake-domain park with `idle < live`, no work, no waits, no
terminate — `run_impl`'s `join` hangs **after every fiber completed**.

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
  ONLY by the two UNLOCKED idle resets (:550 pop path, :582 mw_s1
  fall-through) — the sites the B4 model experiments proved are genuine
  invalidation events (:958/:1452/:1065 are self-guarded; see the suite
  README table). Each is an exchange(0) that bumps the generation only when
  a contribution was actually erased.
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

### C++ refinement of the model's atomicity

The TLA model's `EraseIdle` (count reset + generation advance) and
`DanceContribute` (record + count) are single atomic actions; the C++
sites are unlocked RMW pairs that can interleave. The refinement is sound
under the C++ memory model with three ordering rules (all implemented):

1. the dancer RECORDS the generation strictly BEFORE its `fetch_add`;
2. the eraser bumps strictly AFTER its `exchange(0)` (and only when the
   exchange actually erased a contribution);
3. the park commit loads the generation strictly AFTER its `idle_workers_`
   load (the identity term is the LAST disjunct of the refusal).

Monotonicity argument: whenever an eraser's exchange lands after a
dancer's `fetch_add` (the orphaning case), its bump is sequenced after the
exchange, hence after the dancer's earlier record; a commit that observes
the erased count (rule 3 makes the idle load precede the generation load,
and release/acquire on both writes synchronizes the pair) therefore
observes a strictly newer generation than the recorded one — the refusal
fires. Every remaining interleaving (erase between record and fetch_add;
bump observed without the orphaning exchange) can only produce a FALSE
refusal, which converges by the R4 conservative argument (refuse →
re-loop → re-dance with a fresh identity). A naive "bump-then-store"
eraser paired with "record-after-add" would instead open a
bump-visible/store-not-yet window in which an orphaned contribution
passes the identity check — the opposite order is load-bearing.

Authority table (unchanged domains; one new field in the existing wake-side
authority):

| Domain | Authority | Fix interaction |
|---|---|---|
| run-domain queues/owners | `global_mtx_` (+ per-inbox `inbox_mtx`) | untouched |
| wake domain | `wake_mtx_` / `wake_epoch_` / `wake_cv_` | untouched (the absorption mechanism it hosts is closed by the refusal, not by epoch changes) |
| idle dance | `global_mtx_`-serialized fetch_add + the two unlocked erase sites | the two unlocked sites become `exchange(0)` + conditional generation bump (exchange-then-bump; see the refinement rules above) |
| park commit | `global_mtx_` G-section (recheck then arm) | refusal condition gains the generation-validity term (last disjunct); arm unchanged |

## Gate 2 — Resource and Failure Model

No new container, thread, queue, or allocation. One
`std::atomic<std::uint64_t>` on Scheduler + one per-WorkerState atomic
record. Hot-path cost: the two unlocked `idle_workers_.store(0)` sites
become `exchange(0)` + a conditional generation bump (same atomic cost
class; the all-idle-zero common path stays generation-stable). Park commit
gains one acquire load + compare under G. No failure path. No perf claims
(§16.7 N/A).

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

Lock order: unchanged. The bump at `:550`/`:582` is a lock-free atomic;
the dancer's record is inside the dance's existing G-section; the commit
check is inside the commit's existing G-section. No edge is added between
`wake_mtx_` and any inbox.

## Gate 4 — Evidence

```text
FORMAL (executed 2026-08-21; bash scripts/formal/verify-e12-sched-liveness.sh):
  Positive [repaired] safety ................. PASS (incl. DrainStuckState)
  Positive [repaired] liveness ................ PASS (4 temporal properties)
  B4 self-guarded sites (:958/:1452/:1065) ... PASS with bump disabled
  M1/M2/M3 composition on repaired base ....... PASS (documented in-scope
                                                closure; e9 carries the
                                                classes at its abstraction)
  M4 as-built safety .......................... CEX DrainStuckState
                                                (the 21-state trace above)
  M4 as-built liveness ........................ CEX
  M5 grant-without-ticket ..................... CEX liveness
  B4 invalidation sites (:550/:582) ........... CEX DrainStuckState when
                                                the bump is disabled

C++ (executed 2026-08-21, Clang Debug unless noted; commands verbatim):
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
