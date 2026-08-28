# DST-PV-1 — Deterministic Schedule-Driver Proof-of-Value: Author Report

- Status: PENDING REVIEW (report kept UNTRACKED per task)
- Campaign: DST-PV-1 (falsifiable proof-of-value for a minimal test-only scheduler
  decision seam)
- Branch: `test/dst-pv1-next-runnable-seam`
- BASE_SHA: `b4db8c2` (master HEAD; lineage verified to contain merged PR #238 and
  PR #240 — `git merge-base --is-ancestor 86b65d8 HEAD` passes)
- Task spec: DST-PV-1 — Deterministic Schedule-Driver Proof-of-Value (§0–§26)

---

## 1. VERDICT

**MINIMAL DETERMINISTIC DRIVER JUSTIFIED.**

The falsifiable question the campaign asked was: *can Sluice expose ONE very small
TEST-ONLY scheduler decision seam — the next-runnable choice — and gain materially
better deterministic schedule exploration/replay?* The answer is YES, on
two independent pieces of value evidence:

- **P1 (discovery):** the seam made a previously non-executable as-built
  liveness/accounting drift executable: `queue_pop_admit`'s inline success path
  does NOT reconcile a parked blocking producer (`dst_t5_v1`). The producer is
  stranded `waiting` at run end; only `close`/`try_pop` resolves it — exactly the
  "live drift specimen" rows 11–14 of the AC-2a wait-authority matrix (#234). The
  existing causal tests cover only the documented reconciler (`try_pop`); none
  could drive the interleaving that exposes the drift, because only the worker's
  runnable-choice controls who runs while the pop holds the ring.
- **P3 (expressibility):** the AC-2d falsification campaign (four Queue
  timed/untimed push/pop admission paths) became expressible as deterministic
  scripts — three ladders (V2a–e, V3a–c) plus the V1 probe — with zero
  coordinator threads, zero sleep-based ordering, zero in-fiber ready-flag
  atomics, and `active_deadline_count == 0` asserted at every ladder end.

The seam is exactly the minimum: one guarded hook in `worker_loop` step 1, one
decison vocabulary (Run plus reuse of existing CompleteIo/AdvanceClock/Cancel
seams), fixed-size script storage, test-assigned small-integer identity (never
`Fiber*` in replay syntax or diagnostics), fail-fast abort on illegal decisions
(no fallback), and zero production symbols/behavior change (verified by `nm` and
the seam-production-exclusion mechanical gate).

The negative evidence is equally explicit: the driver does not search, randomize,
exhaust, or model-check; width is 1 and depth is bounded at 64 steps; it can only
realize interleavings the author writes. No race was found in the AC-2d window
(lock-serialization argument holds for every explored interleaving) — the value
is the executable drift specimen and the reduced choreography, not a race find.

**Actions:** keep this minimum; open a DRAFT PR; DO NOT MERGE; DO NOT update #227.

---

## 2. BASE and lineage

- `BASE_SHA = b4db8c2` ("Add AC2C cancellation authority review author report",
  master HEAD at campaign start).
- Lineage requirement: merged PR #238 (AC-2b ordinary deadline authority) and
  PR #240 (AC-2c cancellation authority, merge `86b65d8`) both present;
  verified with `git merge-base --is-ancestor 86b65d8 HEAD`.
- No known human-owned untracked review reports were added, committed, renamed,
  or removed. The only untracked files created by this campaign are the two new
  test files and this report (the report intentionally stays untracked).

---

## 3. EXISTING DETERMINISM SURFACE (Stage A mechanical re-audit)

What Sluice already had before this campaign, and what it did not have:

| Dimension | Existing control | Authority |
|---|---|---|
| I/O completion control | `FakeAsyncBackend` staged completions (`complete_oldest_with_bytes` → `poll()` via `drain_routed_completion_waits_locked`); `IdleBackend` (never completes) | test backends |
| Logical time control | `advance_clock`, `TimerTestControl` (`enable_test_clock`/`set_clock`/`clock_now`/`active_deadline_count`) | AC-2b |
| Semantic phase seams | 31 `PhaseTag` pause gates (`test_phase`/`test_phase_worker` + controller registry keyed on `Scheduler*`) | C4/#135 |
| Cancellation control | `cancel_wait` (documented winner/terminal semantics) | AC-2c |
| Initial placement | `spawn` / `spawn_on` (initial runnable placement only) | — |
| **Next-runnable choice** | **MISSING** — the actual choice authority is `worker_loop`'s FIFO pop from `ws->local_runnable` under `inbox_mtx` (scheduler.cpp step 1) | **this campaign** |

Conclusion of Stage A: the runnable *set* is controllable (spawn + phase seams +
IO/time/cancel), but the runnable *order* — the only remaining nondeterminism
visible to a single-worker test — belongs to an un-seamed FIFO deque. That is the
minimal missing dimension, and it is the only one this campaign closed.

---

## 4. SLEEP / POLL CENSUS

Census of every sleep/yield/poll site in the async tests (classification per
task: WD = watchdog, allowed; CP = causal proof via wall time, forbidden as
deterministic evidence; OTHER):

- **365 WD** — watchdog-bounded loop caps (death-test 60 s watchdogs, bounded
  park loops in deadline tests, bounded wait-for-convergence loops with caps).
- **1 weak-CP** — `runtime_wait_death_test.cpp:75`: a 100 ms grace period whose
  absence verdict is time-dependent. Inventory item, not rewritten (task forbids
  rewriting the suite).
- **18 OTHER** — yield-only loops, spin-wait helpers in coordinator-thread
  choreography, poll loops tied to backend readiness. None is the primary
  ordering proof of a causality claim in the DST campaign.

The new driver tests introduce **zero** sleep/yield/poll sites: all ordering is
script steps plus semantic state observation (verified by construction — the new
files contain no `sleep_for`/`yield`/poll call).

---

## 5. MISSING DIMENSION (the choice authority)

The worker's step 1 (`src/async/scheduler.cpp`, around line 520) was:

```cpp
Fiber* f = nullptr;
{
    std::lock_guard<std::mutex> lk(ws->inbox_mtx);
    if (!ws->local_runnable.empty()) {
        f = ws->local_runnable.front();
        ws->local_runnable.pop_front();
    }
}
```

A test cannot choose WHICH already-runnable fiber runs next: the FIFO pop is the
choice authority. `spawn_on` controls initial placement only; steal/route are
transport. This is the seam the campaign adds — test-only, single decision point.

---

## 6. CONTINUATION AUTHORITY (7 questions)

Continuation of the DST line requires affirmative answers to all of:

1. **Value:** Is the P1/P3 evidence sufficient to keep the minimum seam in the
   test suite (executable drift specimen + AC-2d ladder expressibility)?
2. **Intrusion:** Is the standing invariant "zero production intrusion — 0
   symbols, compiles out, installed headers untouched, verified by `nm` and the
   seam-production-exclusion gate" accepted for every future use of the seam?
3. **Scope:** Does the next experiment stay inside the single-worker `run(1)`
   inline window, or is a multi-worker decision seam (a materially different
   design) authorized as a new campaign?
4. **AC-2d:** Is the V1 drift specimen to be filed as a tracked finding for the
   Queue authority, with `dst_t5_v1` as its executable regression guard, leaving
   any repair to a later approved slice (NO production Queue change in this
   campaign)?
5. **Choreography:** May `DstScheduleDriver` graduate to a shared maintained test
   utility (its header under `tests/`) so subsequent tests prefer script-step
   choreography over the coordinator-thread idiom, with LOC/target-count docs
   updated accordingly?
6. **Vocabulary:** Are Run / CompleteIo / AdvanceClock / Cancel the complete
   near-term vocabulary (no waiter-order control, no steal control, no run-until
   -predicate) until a concrete test needs a new primitive?
7. **Stop gate:** Is the stop gate binding — *a new experiment that requires
   search/randomness/DPOR, that cannot be expressed within 64 steps on the
   current single-worker seam, or that requires a production behavior change,
   terminates this line*?

---

## 7. THE MINIMAL SEAM

### 7.1 Shape

- **One guarded hook** in `Scheduler::worker_loop` step 1
  (`src/async/scheduler.cpp:522–534`), compiled out unless
  `SLUICE_ASYNC_INTERNAL_TESTING`:
  ```cpp
  if (sluice_async_test::schedule_script_active(*this)) {
      f = sluice_async_test::schedule_script_pick(*this, ws);
  }
  ```
  The hook runs with **no scheduler lock held**; script actions may acquire
  `global_mtx_` or backend state. Script exhaustion returns `nullptr` and the
  unchanged FIFO pop runs (a free run).
- **Identity:** test-assigned small integers (≤ kScheduleMaxFibers = 8),
  rendered as letters `A..H` in replay/diagnostics. Never `Fiber*` as replay
  syntax; the pointer table exists only inside the controller (test-only).
- **Script storage:** fixed arrays — 64 steps, 8 fibers, 8 actions, 64 executed
  records — no allocation after install, per-install lifetime, uninstalled in the
  driver destructor.
- **Fail-fast, no fallback:** an illegal `Run(N)` aborts with a diagnostic
  package: test name, failing step index, requested id, legal runnable set
  (rendered as `Run(A)` ids), logical clock, replay vector prefix, executed
  prefix, and queue depth (`schedule_script_fail` → `std::abort()`).
- **Semantics of steps:**
  - `Run(id)` — remove the bound fiber from `ws->local_runnable` under
    `inbox_mtx` alone (the same lock/critical-section shape as the FIFO pop) and
    return it as the worker's next fiber.
  - `Invoke(action)` — execute a test action, then **terminate the visit**
    (return `nullptr`): actions stage effects (deadline expiry pumping, staged
    completion) that become runnable only after the worker's next drain.
- **Lock discipline:** the new script mutex is a leaf; only the test-thread
  visits it; no production lock is held at entry, and no production code ever
  acquires it (order script-mtx → production locks is therefore the only
  possible order; no cycle).

### 7.2 Vocabulary

Run(X) is the only genuinely new primitive; everything else reuses existing
seams:

| Primitive | Origin |
|---|---|
| `Run(X)` | NEW — deterministic next-runnable choice (single-worker) |
| `CompleteIo(N)` | existing `FakeAsyncBackend` staging + `poll()` |
| `AdvanceClock(T)` / `Clock(T)` | existing `advance_clock` |
| `Cancel(W)` | existing `cancel_wait` |

### 7.3 Replay format

`DstScheduleDriver` records every step as it is appended:
`Run(B), Run(A), Clock(60), Io(4)` → the observed trace is asserted against
semantic output; the replay vector is part of illegal-decision diagnostics.

---

## 8. EXPERIMENT TARGETS

| Target | Driver test | What it proves |
|---|---|---|
| T1 exact runnable selection | `dst_t1_exact_runnable_selection` (20 iterations over BCA/CBA orders) | The seam chooses exactly the scripted runnable; orders are reproducible bit-for-bit |
| T2 replay | `dst_t2_replay` (two schedules, one with post-run cancel) | The same script produces the same semantic trace; different scripts produce different traces |
| T3 illegal decision | `dst_t3_illegal_decision_aborts` (fork/exec death child, 60 s watchdog) | Invalid `Run(N)` → SIGABRT with the full diagnostic package; no fallback |
| T4 cross-domain | `dst_t4_cross_domain_precedence` (DEADLINE × IO × CANCEL) | Genuine cross-domain interaction: expiry-wins vs cancel-wins vs cancel-after-expiry, `active_deadline_count == 0` at end |
| T5 AC-2d falsification | `dst_t5_v1/v2/v3` (below) | The four Queue admission paths under deterministic scheduling |

T4 is the required genuine cross-domain interaction (deadline expiry, staged I/O
completion, and cancellation arbitration in one run); T5 is the required AC-2d
informing experiment.

---

## 9. AC-2d FALSIFICATION RESULT

**Experiment:** use deterministic scheduling to try to falsify the
lock-serialization argument for the four Queue push/pop admission paths whose
final recheck differs (AC-2a matrix rows 11–14, the #234 live drift specimen).

**Interleavings driven (all under script control, zero coordinator threads):**

- `dst_t5_v1` — parked blocking producer vs inline `try_pop` success (cap-1
  ring pre-filled): the inline success path (`queue_pop_admit`) does not run
  `queue_grant_producer_locked`; the parked producer remains `waiting` at run
  end; only `close`/`try_pop` resolves it. Pinned as the **exclusive** as-built
  verdict (a mutant that adds the reconciler call fails the test).
- `dst_t5_v2` — timed producer ladder (`push_until(50)`): (a) expiry pumps
  `P:expired`; (b) `try_pop` reconciler grants `P:committed`; (c) close gives
  `P:closed`; (d) already-due + full ring → `P:expired`; (d2) already-due +
  empty ring → `P:committed` (precedence-1 wins); (e) close-before-admit →
  `P:closed`.
- `dst_t5_v3` — timed consumer ladder (`pop_until(50)`): (a) expiry `C:expired`;
  (b) `try_push` grants `C:item:5`; (c) close + empty ring → `C:closed`.

**Verdict:** **NO RACE FOUND.** In every explored interleaving, admission and
reconciliation that touch the same ring serialize on the QueuePort role mutex
under the documented G→S→role-mtx order; the differing-final-recheck rows are
not reachable as a lost-wake, duplicate-terminal, or stale-generation race.
For the *race* question the recheck comparison is REDUNDANT as-built. However,
the campaign exposed that the QUEUE-only missing terminal-recheck is real as a
**liveness/accounting drift**: the inline pop success path leaves a parked
blocking producer unreconciled (V1). The drift is now an executable deterministic
regression — this is P1, the discovery value.

No production Queue change was made (`src/async/scheduler_queue.cpp` untouched;
`queue_port.cpp` untouched).

---

## 10. BEFORE / AFTER CHOREOGRAPHY

- **Before (existing idiom, e.g. `async_mutex_primitive_test.cpp`):** in-fiber
  `ready_flag` atomics + a coordinator thread + `spin_wait` helpers + cap loops,
  ordering argued through shared flags; a failure leaves only flags and a stack
  trace.
- **After (driver script):** `run(X)`/`invoke(...)` steps + semantic assertions
  on the observed output; a failure carries the replay vector, the failing step,
  the legal runnable set, and the executed prefix — the artifact that
  reproduces the schedule by construction, not by argument.

Measured reduction on the AC-2d ladders: 3 ladders + 1 probe in one test file,
zero threads, zero sleeps, zero atomics in test code, while exercising the same
subsystem states the matrix rows document.

---

## 11. BUGS / DRIFT SPECIMENS FOUND (as-built facts; none fixed)

1. **V1 — stranded parked producer (drift specimen, executable regression):**
   `queue_pop_admit`'s inline success path never runs
   `queue_grant_producer_locked`, so a blocking `push` parked on a full ring is
   not reconciled by the very pop that frees the slot; it stays `waiting` until
   `close`/`try_pop`. Matches the AC-2a matrix's #234 "live drift specimen"
   (terminal-recheck missing in Queue only). Guarded by `dst_t5_v1`.
2. **F-2 — drain-mode run with pending fake I/O plus a registered waitq wait
   never terminates without a script** (recorded per task §25, not fixed):
   MW-S2's no-progress path's `if (external_wake_possible_locked()) continue;`
   loops forever in ~1 ms bounded parks when `waiting_waitq_count_ > 0` (the
   predicate never goes false). Forensics: `/proc` futex wait streams,
   `strace FUTEX_WAIT_BITSET` timeouts, park forensics (domain=SCHEDULER,
   admission=committed, outstanding=1, last_classify=mw_s2), E9 trace showing
   endless `park_committed → park_entered → park_returned(timeout)` cycles with
   `wake_epoch=0`. Exposed during T2 development when the script was accidentally
   not armed; the seam made the cause exactly attributable.

---

## 12. VALUE EVIDENCE / NEGATIVE EVIDENCE

**Value:**

- P1: `dst_t5_v1` — the stranded-producer drift is now a deterministic
  executable fact with a replay artifact (not covered by any existing causal
  test; `async_queue_primitive_test.cpp` P4 covers only the documented
  reconciler).
- P3: the AC-2d ladder campaign (V2a–e/V3a–c) and the cross-domain precedence
  suite (T4) are expressible deterministically; T1 proves 20/20 scripted orders
  reproduce exactly.

**Negative:**

- No random/exhaustive/DPOR/search power: width 1, depth ≤ 64, author-written
  interleavings only. The driver cannot *find* an interleaving; it *realizes*
  one.
- No race found in the AC-2d window (see §9). The seam's value there is the
  drift specimen, not a race catch.
- Single-worker scoped (the `run(1)` inline fast path); multi-worker runnable
  choice remains FIFO and out of scope.
- One weak-CP sleep site (`runtime_wait_death_test.cpp:75`) and the
  absence-after-sleep inventory remain in the suite (not rewritten per task).

---

## 13. PRODUCTION INTRUSION (zero)

- `nm` on the production `sluice_async` archive: **0 `schedule_script` symbols**
  (the hook and all helpers compile out without `SLUICE_ASYNC_INTERNAL_TESTING`).
- Installed public headers (`include/sluice/`) untouched; the only production
  TU touched is `src/async/scheduler.cpp` inside a guarded block.
- `mechanical-facts.py` seam-production-exclusion check: PASS.
- Production `sluice_core` and `sluice_async` build clean under the Clang Debug
  and TSan configurations used for this campaign.
- No behavior change: the guarded block is the only diff outside tests/docs.

---

## 14. TEST / GATE EVIDENCE (all executed, none claimed without running)

| Gate | Command | Result |
|---|---|---|
| Clang Debug full suite | `xmake f -m debug --toolchain=clang -y` + build + `xmake test -v` | ALL TESTS PASSED, 193/193 targets (7.769 s) |
| TSan | `xmake f -m tsan --toolchain=clang -y` + build + `xmake run -g test` | 182 binaries ALL TESTS PASSED; 0 ThreadSanitizer warnings; all 7 DST cases ran under TSan |
| Release | — | NOT REQUIRED (§16.1): no installed public header, no `noexcept`/fail-fast/API contract change; not claimed |
| mechanical-facts | `--self-test` + full | PASS (incl. seam-production-exclusion; LOC rows 192→193 under `test:default-gate-targets` verified) |
| assert-hygiene | `--self-test` + changed-lines | OK — no new assert-family lines |
| doc links | `python3 scripts/check-doc-links.py --self-test` + full | PASS |
| arch docs | `python3 scripts/verify-architecture-docs.py` | OK |
| pre-push | `bash scripts/gates/pre-push.sh` | ALL CHECKS PASSED |
| diff hygiene | `git diff --check` | OK |
| Formal | — | NOT APPLICABLE: no modeled transition/admission/wake/terminal/generation/shutdown rule changed (the runnable *set* is unchanged; only a test may choose the pick order within it) — gap justification recorded in §16 Gate 1 |

---

## 15. MUTATION SENSITIVITY (all reverted; final suite re-run green after reverts)

| Mutant | Intent | Killed by |
|---|---|---|
| M1 FIFO-degrade (ignore Run) | fallback would be silent | `dst_t1` fails (exact selection not enforced) |
| M2 silent-fallback (illegal → FIFO) | no-fallback requirement | `dst_t3` fails (no abort) |
| M3 step-transpose (swap adjacent Run order) | replay fidelity | `dst_t1`/`dst_t2` event-order assertion fails |
| M4 cancel-skip (drop Cancel step) | cross-domain coverage | `dst_t4` cancel-wins trace fails |
| M5 production reconcile-repair (add `queue_grant_producer_locked` to inline pop success) | V1 guards as-built drift, not hypothetical repair | `dst_t5_v1` pinned as-built verdict fails (producer must stay waiting) |

M5 is the load-bearing one: it proves the drift specimen test detects exactly the
repair the matrix documents as missing — the test guards the as-built fact, not
the repaired hypothetical. `scheduler_queue.cpp` was restored byte-identical
(`git diff` = 0 lines after revert).

---

## 16. PHASE-SPECIFIC ARCHITECTURE COMPLIANCE GATE (Gate 0–4)

Referenced authority: `docs/architecture/design-compliance-gate.md` (generic);
this section covers every Gate 0–4 field.

### Gate 0 — Design compliance

- Task authority: DST-PV-1 spec (test-only seam; no production behavior change).
- AGENTS.md §15 (test-only controls): guarded by `SLUICE_ASYNC_INTERNAL_TESTING`;
  lives in `tests/` + a guarded src TU block; no installed-header change; no
  public API; no `*_for_test` unguarded method.
- AC-N: AC-1 (minimal observability — this is observability of runnable order,
  test-side only); AC-2a/2b/2c/2d unaffected (no authority change; AC-2d
  experiment conducted, no production change).
- Non-trigger justification for advisory/cancel/queue-capacity ADRs: no change
  to admission, capacity, terminal winner, wake rules, or shutdown.
- Zig classification: no conformance/divergence impact (no production surface).

### Gate 1 — State machine

- Production: UNCHANGED. The runnable set is untouched; only the pick order may
  be test-chosen. The unchanged FIFO pop remains the authority when no script is
  active or the script is exhausted.
- Test seam states: `installed → active → exhausted | failed(abort)` and
  `uninstalled` (driver dtor). No request/slot/Completion transitions are
  touched; the seam never binds Completion pointers.
- Formal-model gap: no modeled transition/admission/wake/terminal/generation/
  shutdown rule changed → no model update required (§17 of AGENTS); the
  runnable-pick order is transport, not modeled protocol.

### Gate 2 — Lock / atomic authority table

| Domain | Lock | Authority | Notes |
|---|---|---|---|
| runnable removal | `ws->inbox_mtx` (existing, leaf) | `Run` pick uses the identical critical section as the FIFO pop | same shape, same order, no new ordering |
| script state | new script `mtx` (leaf) | only the test thread; no production lock held at entry; production never acquires it | order script-mtx → production locks only; acyclic |
| arena / scheduler | `global_mtx_` (unchanged) | script actions may acquire it; run under script-mtx | no reverse order exists |
| atomics | none added | — | — |

### Gate 3 — Resource-capacity and allocation model

- Fixed arrays: 64 steps / 8 fibers / 8 actions / 64 executed records; zero
  allocation after install; per-install lifetime; uninstalled at driver dtor.
- No interaction with request_capacity, ring depth, worker count, or pipeline
  depth; no long-lived container; no historical growth.
- Illegal-decision path: `std::abort()` — no allocation, no partial state
  publication.

### Gate 4 — Wake / progress model and shutdown

- Wake: no obligation changes. The runnable set is identical; a `Run` pick is the
  same dequeue the worker would otherwise perform, executed inline on the same
  thread (the `run(1)` fast path) — no wake can be lost to the seam.
- Progress: script exhaustion → free run → unchanged FIFO; illegal decision →
  test fail-fast abort, not a production liveness event.
- Shutdown: uninstall in the driver destructor; no scheduler shutdown state
  touched; production destruction semantics unchanged.

### Evidence list

- PENDING (all itemized): → PASS after execution. Commands and results in §14;
  every row was actually run. No `PASS` was pre-filled.

---

## 17. REVIEW ROUND 2 (PR #241 review ID 5043944562) — fixes applied

The reviewer approved the direction and proof-of-value but found 4 P1s + 1 P2,
all confined to the test harness. All fixes are applied on the same branch
(commits `585ba76` + `75d857b`, pushed; PR #241 head now `75d857b`, still
DRAFT) and each is now an executable regression, not just a wording change.

| Finding | Fix | Regression |
|---|---|---|
| P1-1 inactive path took registry + script locks per pop | `schedule_script_active` is now a no-lock fast gate: one thread-local activation pair + a Scheduler identity compare (no registry mutex, no script mutex, no atomic). A script is visible ONLY on its installing thread, so every other worker in every multi-worker run pays one TLS load + compare and stays on the FIFO pop — multi-worker scheduling space is structurally untouched | Code-level: the gate body reads only `t_active_script` + two same-thread flags; `find_controller` remains only in install/uninstall (one call per test) |
| P1-2 action executed while holding the script mutex | Invoke copies its action out under the script mutex, releases it, executes the action with NO script mutex and NO production lock held, then re-acquires to record the step + advance the index | `dst_t6_action_reenters_control_surface`: an action that calls `uninstall_schedule_script` mid-run deadlocks pre-fix and completes post-fix |
| P1-3 the 60 s watchdog was unreachable (blocking read-to-EOF before waitpid) | Parent now runs ONE bounded loop: poll(pipe, remaining budget) + waitpid(WNOHANG) per iteration; EOF, reap, or the deadline ends it; a hung child is SIGKILLed and reported `timed_out` | T3 still passes and the loop is provably bounded (the poll paces even the EOF-with-live-child case; no busy spin) |
| P1-4 fixed capacities had unchecked builder indices | `bind`/`on_action`/`run`/`invoke` validate id < 8 and step_count < 64 at append time and fail loudly (abort with the violation named); the seam's install validates counts too; the pick validates the Invoke payload against registered actions | Two new death children in T3: `dst_t3_driver_bounds` (bind(8) aborts with "DST SCHEDULE DRIVER FAILURE" naming the id) and `dst_t3_unregistered_action` (invoke(0) with no action aborts with the configuration package) |
| P2 V1 framed the drift as a desired contract | V1 is now explicitly a **KNOWN-DRIFT CHARACTERIZATION WITNESS**: it proves the as-built defect exists today; a future Queue repair slice must consciously FLIP or REPLACE the expectation (P:committed from the pop), and a repair is deliberately NOT attempted in this campaign. Existing semantic checks (C got the pre-filled item exactly once; P stranded at run end; close resolves P) are unchanged | V1 comments + assertion messages restated; the M5 mutant (inline-path reconciler added) still kills the witness, proving it detects exactly the repair the matrix documents as missing |

Research discipline on the AC-2d verdict is also tightened: the report now says
"no race found **in the enumerated boundary schedules**, consistent with the
lock-serialization proof" — the 64-step, no-search PV driver never claims
exhaustive interleaving coverage.

Round-2 gate evidence: the full TSan suite re-ran with the fixed harness
(182 binaries, ALL TESTS PASSED, 0 ThreadSanitizer warnings; all 8 DST cases
ran under TSan); Clang Debug suite re-ran (193/193 targets, ALL TESTS PASSED);
mechanical-facts, assert-hygiene, check-doc-links, verify-architecture-docs,
pre-push, git diff --check all PASS. Production intrusion unchanged
(0 `schedule_script` symbols; installed headers untouched).

---

## 18. REVIEW ROUND 3 (PR #241 corrective pass after adversarial re-review) — fixes applied

A second adversarial review (corrective round 3) accepted the direction and
proof-of-value and left exactly one P1 + two P2s, all in the test harness.
Fixes are applied on the same branch (commits `d3be24b` + `c52303d`, pushed;
PR #241 head now `c52303d`, still DRAFT) with executable regressions.

| Finding | Fix | Regression |
|---|---|---|
| P1 Invoke actions could re-enter `install_schedule_script` (re-install / re-arm), letting the OLD script's post-action epilogue advance a NEW script's step counter — silent deterministic-replay corruption | `install_schedule_script` now FAILS LOUDLY when called from inside an Invoke action: a same-thread `t_schedule_invoke_active` flag is set before the action runs and cleared after it returns; the guard aborts with "DST-PV-1 SCHEDULE SCRIPT FAILURE / install/re-arm forbidden inside Invoke", naming the current script and step index. No generations, no nested schedules, no transactional replacement | `dst_t7_reinstall_inside_invoke_aborts` (death child): a script whose Invoke action arms a second complete script must SIGABRT with the named diagnostic — no silent replacement, no skipped B step, no hang |
| P1 (same) pick copied `std::function` on the invoke path (may allocate) | The pick now invokes actions through a STABLE POINTER (`std::function*`) into the controller's fixed action array — no copy, so the pick allocates nothing after install. The pointer is stable because the array is fixed controller storage retained until process teardown, re-install is fail-closed during Invoke, and uninstall never rewrites the array. The zero-allocation claim is now structurally true (kept) | Code-level lifetime proof + the T7 guard makes mid-invoke array rewrite impossible; the uninstall path (T6) is untouched |
| P2 watchdog busy-spin: EOF + live child made poll on the HUP'd pipe return immediately until the deadline (bounded but CPU-burning) | `run_child_captured` (ONE bounded poll+waitpid loop) now switches to a fixed 10 ms pacing interval (`poll(nullptr, 0, 10)`) once EOF is seen while the child still lives. Watchdog pacing is observation-only and never causal (sleep-for-ordering stays forbidden) | `dst_t8_watchdog_hang_after_stderr_close`: a child that closes stderr then hangs must reach the (500 ms test-local) deadline, be SIGKILLed, report `timed_out`, and burn parent CPU far below wall time (getrusage ratio proof — a spin loop would burn ~100% of a core) |
| P2 stale / over-strong PR evidence | Actual GitHub PR #241 body patched (via REST API): DST case count corrected mechanically (10 on POSIX), zero-allocation claim kept only because it is now structurally true, AC-2d wording narrowed to "no race found in the enumerated boundary schedules, consistent with the lock-serialization proof", V1 explicitly KNOWN-DRIFT CHARACTERIZATION WITNESS, T6 contract narrowed to supported non-replacing surfaces (uninstall supported, re-install fail-closed), CodeRabbit status stated accurately (skipped because the PR is a draft) | `gh pr view 241 --json body` confirms the patched text |

T6 semantics are preserved: an Invoke action may still call
`uninstall_schedule_script`; after it returns the epilogue records the
executed step into the OLD controller state and never reactivates (activation
is the TLS pair alone, which uninstall cleared) — verified by the retained
`dst_t6_action_reenters_control_surface`.

Round-3 gate evidence (all executed): Clang Debug full suite 193/193 targets
PASS; TSan full suite 182 binaries ALL TESTS PASSED, 0 ThreadSanitizer
warnings, all 10 DST cases under TSan; Clang Release full suite 193/193
targets PASS; mechanical-facts, assert-hygiene, claim-hygiene,
check-doc-links, verify-architecture-docs, pre-push, git diff --check all
PASS. Production intrusion unchanged: `nm` on the production
`libsluice_async.a` (Debug/TSan/Release) shows 0 `schedule_script` symbols;
installed public headers untouched; Queue production code untouched;
no random/search/seed feature added.

---

## 19. WHAT MUST NOT BE BUILT

Explicit non-goals (the task's stop conditions, recorded as binding):

- NO search / seeds / random schedules / exhaustive enumeration / DPOR /
  model-checking loop around the driver.
- NO production hot-path branch that does not compile away; NO production
  scheduling-policy change; NO continuation API; NO runtime decision seam.
- NO multi-worker runnable-choice control, NO waiter-order control, NO steal
  control, NO priority/real-time semantics.
- NO new primitive beyond Run/CompleteIo/AdvanceClock/Cancel until a concrete
  test requires one.
- NO rewrite of the existing test suite's sleep patterns; NO new wall-clock
  ordering anywhere.
- NO `Completion*` or `Fiber*` as replay syntax; identity stays
  test-assigned small integers.
- DO NOT MERGE the DRAFT PR; DO NOT update #227.

---

## 20. RECOMMENDATION

Keep the minimum seam. Open a DRAFT PR

**"test(async): add minimal deterministic runnable-choice seam"**

whose description states: NOT a DST framework; NOT production scheduling policy;
no random scheduler; no exhaustive search; no continuation API; single-worker
`run(1)` scope; exact proof-of-value (V1 drift specimen + AC-2d ladder);
replay example; production-intrusion proof (0 symbols); sleep/watchdog
distinction (driver adds zero sleeps); AC-2d relevance; and the explicit stop
gate from §19. DO NOT MERGE; DO NOT update #227.