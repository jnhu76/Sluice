# Issue #553 — M5 signal-before-enqueue mutant: deterministic scheduling coverage

Test-oracle adequacy slice. This is **mutation-coverage debt**, not a
production defect: the M5 reorder exists only in the mutated copy; the
pristine `worker_loop` order (enqueue before signal) makes the erroneous
window unreachable. No production semantic change is made or claimed.

BASE_SHA `5fa0c973fe012c0cb00c4cd3499ecaab9cb49da7` (= `origin/master` =
PR #552 merge; the historical #550/#552 evidence baseline, so the frozen
historical experiments and the current-HEAD verification share one source
identity; local master `7c6dbb3d` is an ancestor of the baseline with no
extra commits).

Branch `test/553-m5-deterministic-oracle` → one PR, held OPEN for owner
merge. Issue #553 stays open until the owner decides.

## M5-1 — Frozen call chain, execution DAG, oracle

### Pristine production order

`ThreadPoolBackend::worker_loop` physical-outcome branch
(`src/async/threadpool_backend.cpp`):

```text
offer_terminal(key, candidate)                      RequestCore terminal handoff
release_execution(key)
{ lock work_mtx_; publication_pending_.push_back(h); }   E: enqueue
signal_ready_progress()                             S: completion signal
```

`signal_ready_progress()` → `BackendProgressPort::signal()` →
`ProgressSource::signal()` (`progress_source.hpp`): one `mtx_` hold covers
the epoch (or exhaustion) advance and `wake_notification_nolock_()`'s
eventfd write. `snapshot()` and the park-side revalidation read the same
epoch under the same mutex.

### Why the token is a publication witness in pristine code

1. Same-thread program order: E is sequenced-before S.
2. `ProgressSource::signal()`'s critical section is ordered against every
   later owner-side epoch read by mutex release/acquire
   (synchronizes-with): an owner that observes the advanced token observes
   the *completed* signal, not an in-flight prefix.
3. Combining 1 and 2: token-visible ⟹ enqueue completed ⟹ the owner's next
   `poll_progress()` pass, which drains `publication_pending_` under
   `work_mtx_`, deterministically finds the obligation.
4. Kernel side: the eventfd write shares the signal's mutex hold. A park
   that revalidates after the signal either sees the epoch change (returns
   before parking) or has already drained under the same lock before the
   signal acquired it — in which case the signal's write leaves persistent
   `POLLIN` readiness that returns from `poll(2)`. Both orders are closed;
   this is PROG-02's "persistent observable readiness or safe race with the
   sleep handshake" for this transition.

### M5 mutant and the minimal erroneous interleaving

M5 swaps the two operations (signal happens-before enqueue). The window
`[signal returns … enqueue completes]` then exists in the mutant's program
order, and nothing orders the owner's pass out of it:

```text
T_worker                                T_owner (driver, wait_one, 8 s bound)
                                        snapshot T0
                                        pass: publication_pending_ empty
                                          (worker still pre-terminal)
                                        wait_if_unchanged(T0)
                                        pause at prerevalidate gate
signal(): mtx_ hold; T0→T1; eventfd write
[mutant window: worker descheduled /
 held here before enqueue]              (test witnesses worker past signal: E1)
                                        (test resumes owner)
                                        lock: T1≠T0 → WakeReason::progress
                                          (returns before the prepark counter)
                                        loop: snapshot T1
                                        pass: queue EMPTY → completed=0
                                        accepted_work_remains → wait_if_unchanged(T1)
                                        lock: T1 unchanged → drain eventfd
                                        prepark counter 1                 E2
                                        probe (threadpool: no override → false)
                                        recheck unchanged → poll(2) parked
[worker resumes] enqueue under work_mtx_     (no subsequent signal)
                                        poll(2) stays unreadable until the
                                        8 s deadline → final pass publishes →
                                        progress(1) with prepark==1
```

Reachability in the mutant: the two statements are adjacent with no
cross-thread ordering other than program order, so any descheduling of the
worker between them (or an owner fast enough to run its empty pass inside
the window) realizes it. Unforced, the window is the worker's remaining
few uncontended instructions versus the owner's gate-wakeup → revalidation
→ snapshot → pass path, which is orders of magnitude longer — hence the
historical 0/20 unforced kills on both hosts (WSL2 record, native record)
while one native run showed the 8.05 s deadline-recovery footprint: the
interleaving *can* occur, the unforced suite just cannot land its assert
inside the window. `NOT_KILLED` is a scheduling-coverage statement, not
equivalence (M5P forced probe killed 5/5 at `prepark==0`).

Why the same false witness is **unreachable in pristine**: for the owner to
cross the park boundary it must revalidate an unchanged token. If the
worker's signal has been observed, the enqueue preceded it (program order),
so the owner's re-run pass was non-empty and returned `progress(1)` before
any park. If the signal has not been observed, the owner may park, but the
later signal writes the eventfd under the same mutex that guards the fused
revalidate+drain, so the park wakes. There is no third case.

### Oracle inventory for the r4 family

| Assertion | Observes | Can product code wrongly satisfy it? |
|---|---|---|
| `worker_signaled` (token witness) | `ProgressSource` epoch pair advanced — production notification authority, not test state | Not without a real signal; producer set in the scenario is exactly the single gated worker |
| `is_progress(result, 1)` | public wait outcome; a pass actually published the gated read | Only by really publishing |
| `prepark == 0` | incremented by production code inside `wait_if_unchanged` **after** the fused unchanged-revalidation + stale drain — a genuine park-boundary crossing with a stale token | Parking is the *symptom* of the lost wake; a mutant that avoids parking must instead publish (caught by the other conjuncts). M5P's observed signature had every other conjunct green and only `prepark=1` red |
| `read.request.ready()` | public Request readiness | Only via real publication |

The assertions depend on no time window; the historical E1 witness is a
bounded spin whose watchdog only *reports* failure. No test/implementation
shared wrong premise: the witness consumes the production token, the
counter is a production-side protocol point, and no assertion inspects the
publication queue's representation (the queue is only read for failure
diagnostics via `publication_pending_size_for_test`).

What the existing suite lacks is not oracle discriminative power but
**schedule control**: no existing seam can hold the worker inside the
mutant-only `[signal … enqueue]` interval (in pristine the interval does
not exist). Enumerated: `worker_outcome_pre_terminal` ends before
`offer_terminal`; `publication_epilogue`/`delivery_claimed` are owner-side
(inside `publish_one`/`deliver_event`); `unclaimed_retirement` is a
different worker branch; context-side gates control the owner only.

## M5-2 — Deterministic admission decision

```text
CANDIDATE_SEAM = worker-side pause gate in worker_loop's physical-outcome
  branch, immediately after signal_ready_progress()
  (WorkerPostCompletionSignalPauseGate), following the existing
  SLUICE_ASYNC_INTERNAL_TESTING gate pattern (10th gate; the same function
  already hosts worker_outcome_pre_terminal)
EXISTING_SEAM_REUSABLE = NO (enumerated above; the mutant-only interval
  cannot be held by any pristine seam placed elsewhere — only a seam placed
  after the signal lands inside the window once M5 swaps the statements)
REQUIRED_INTERLEAVING = worker parks at the gate just past its completion
  signal (E1 = gate paused handshake) → owner released past the
  prerevalidate gate → owner revalidation sees the token change
  (gate release/acquire makes the visibility transitive and deterministic)
  → owner re-run pass: pristine publishes (progress(1), prepark stays 0);
  mutant drains an empty queue and crosses the park boundary
  (E2 = prepark counter ≥1, atomically notified by production code)
EXPECTED_ORACLE = new subtest r4b: worker held past completion signal,
  owner services the publication without parking — prepark==0 ∧
  is_progress(result,1) ∧ request.ready(); under M5 the same subtest is RED
  at exactly the prepark conjunct with a printed causal signature
  (prepark value, publication_pending size == 0 at the witness, driver
  parked)
PRODUCTION_PERTURBATION = none outside SLUICE_ASYNC_INTERNAL_TESTING
  builds (call site compiled out); in testing builds one atomic load per
  worker completion; the gate wait runs outside work_mtx_ and outside
  ProgressSource::mtx_ (call site is after the push_back lock scope and
  after signal() returned)
LIFECYCLE_COST = test-local gate storage; resume + exited handshake before
  context teardown on every path (mirrors GatedWorkerRead); mutant failure
  path wakes the parked owner deterministically via the control interrupt
  instead of waiting out the 8 s bound; no joinable thread, held borrow,
  unconsumed Request or blocked eventfd is left on any path
DETERMINISTIC_ADMISSION = PASS — reuse of the established gate pattern; no
  new production state class; no scheduler framework; no sleep/yield/
  stress as synchronization (E1 and E2 are explicit handshakes; the bounded
  yield loop that watches which of the two occurred only witnesses, and its
  bound only reports harness failure)
```

Mechanism justification gate: the gate is **test-only evidence** (class 5);
it represents no new semantic fact; it is justified by PROG-02's
no-lost-wake schedule coverage for the completion-signal window, which no
existing seam or bounded test can reach (established empirically by the
#550/#552 campaigns on both hosts); it replaces nothing. In pristine builds
it is absent.

Why the swap remains M5: the mutant diff only exchanges the enqueue block
and the signal call; the gate line is pristine, untouched code that the
swap happens to place inside the erroneous window. The mutated semantics —
signal happens-before enqueue — is exactly the historical M5 inversion.

## Results

### WSL2 implementation and causality verification (M5-3)

Environment: WSL2 Ubuntu 26.04, kernel `6.18.54.1-microsoft-standard-WSL2`,
g++ 15.2.0, xmake v3.1.2. Source: branch commit `1612e52b` (seam + r4b).

Mutant builds are one-off `/tmp` copies (`/tmp/sluice-553-mutants/`, direct
g++ recipe from the #550 campaign: `-std=c++20 -O2 -g -pthread
-DSLUICE_ASYNC_INTERNAL_TESTING`), never on the branch. The pristine control
built with the same recipe passes 22/22 (`bin-pristine`, SHA-256
`c0b4c11116a6dc1b82a9437b81b331d6e8c7f91719f56a27e9bc5f7e59e52bb1`),
validating the recipe before any mutant ran.

| Case | Result |
|---|---|
| Pristine / ordinary suite | PASS 22/22 (xmake release×liburing=y binary `9d8545fd…`) |
| Pristine / deterministic control (r4b) | PASS — owner services the publication with `prepark==0` while the worker is held past its signal |
| Pristine / full xmake matrix | 56/56 debug×liburing=n, 74/74 debug×liburing=y, 56/56 release×n, 74/74 release×y (counts identical to the #550/#552 records) |
| Pristine / stability campaigns | 0 fails / 360 sequential, 0/480 concurrent-8, 0/480 concurrent-16 |
| Pristine / TSan (`-O1 -g -fsanitize=thread`) | 5/5 runs, 22/22 subtests, zero warnings |
| M5 / original unforced r4 | **PASS inside every mutant run** — 20/20 M5 runs printed `ok r4_…` before dying at r4b: the unforced window remains unschedulable for the token-witness oracle, matching the historical 0/20 (WSL2) + 0/20 (native) |
| M5 / deterministic controlled schedule (r4b) | **KILLED 20/20**, every run `rc=1`, wall ≈ 0.007 s, identical signature (below) |
| M1 (signal dropped) | KILLED 5/5 at `r4_…` (watchdog path, 18.01 s bounded, `worker completion signal missing` diagnostic) — discrimination preserved |
| M2 (first revalidation dropped) | KILLED 5/5 at `r2_…` — preserved |
| M3 (EAGAIN tolerance removed) | KILLED 5/5, `rc=134`, `write failed with errno=11` after `ok r15_…`, i.e. inside the saturated subtest — preserved |

Deterministic kill signature (identical in all 20 M5 runs):

```text
ok r4_worker_completion_signal_before_final_recheck_is_caught
r4b: owner crossed the park boundary although the worker was already past its
     completion signal: prepark=1 publication_pending=0 token 0/0 -> 1/0
r4b: cleanup publication ok
FAIL: r4b_worker_held_past_completion_signal_is_serviced_without_parking
exit 1, wall ~0.007 s
```

The five kill-evidence requirements:

1. *Executed past signal, before enqueue*: the post-signal gate handshake
   holds the worker after `signal_ready_progress()` returned; at the witness
   the token had advanced (`0/0 -> 1/0`, the signal completed) while
   `publication_pending == 0` (the enqueue had not happened).
2. *Owner inside the target window*: `prepark=1` — the owner crossed the
   park boundary (unchanged-token revalidation + stale drain completed) with
   the queue empty and the spent signal's readiness already drained.
3. *Assertion tied to PROG-02*: the failing conjunct is `prepark == 0`, the
   no-lost-wake obligation that an already-observed completion signal must
   be serviced without an intervening park.
4. *Not an unrelated failure*: named `FAIL:` line from the intended subtest,
   `cleanup publication ok` (fail-path publication and discard completed via
   the deterministic control-interrupt wake, not an 8 s deadline), exit 1 in
   milliseconds — no SIGABRT, no timeout, no teardown symptom.
5. *Reproducible*: 20/20 identical signatures; raw logs, diffs, binaries and
   SHA-256s recorded under `Reproduction`.

Mutant identity (WSL2): M5 diff SHA-256
`1f74a027ce00cde6e3b26a6ac880e93b3716279b19be2c4b3e86bf0f5509639d`
(enqueue block and signal call exchanged; the gate line is untouched
pristine code that the swap places inside the window), binary SHA-256
`7a9d58b950396ee5674183aef80db33cf74b2134359795965e129dddbfb99dd9`.
M1 diff `027bbd4b…`, binary `9a255523…`; M2 diff `790ea5a3…`, binary
`c84fd0eb…`; M3 diff `62436458…`, binary `f2695ec8…`.

Cleanup validation: no leftover processes after any campaign (`pgrep`
clean); the M5 fail path leaves no joinable driver (interrupt-woken and
joined), no held worker (gate resumed, `exited` awaited), no unconsumed
`Request` (final pass publishes, then discard) and no blocked eventfd
(control acknowledged).

## Environment

### WSL2 (implementation host)

| Item | Value |
|---|---|
| Kernel | `6.18.54.1-microsoft-standard-WSL2 x86_64` |
| Compiler | g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0 |
| xmake | v3.1.2+HEAD.5d21e9d |
| Suite size | 22 subtests (21 + r4b) |

