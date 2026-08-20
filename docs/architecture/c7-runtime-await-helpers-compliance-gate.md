# C7 Runtime Await Helpers + Task-Result Bridge: Compliance Gate

**Change class:** application-pattern extraction into public async API (#135 C7,
PR agent/runtime-await-operation-helpers). Adds `await_op_helpers.hpp`,
`task_result.hpp` (public async API + a synchronization primitive), and the
P1 submit-exception boundary fix (review issue #150).
**Baseline master SHA:** `6d40ce6` (post-#149).
**Generic gate:** `docs/architecture/design-compliance-gate.md` — this document
covers every Gate 0–4 field for the change and links back.
**Governing docs:** AGENTS.md §9/§13/§14, `docs/architecture/failure-model.md`.

---

## Gate 0 — Architecture Classification

New: public await-style operation coordinators (thin protocol composition over
the existing per-op submit/await_completion contract — no new backend, request,
or Completion semantics), and the task-result bridge. `TaskResultSlot` is a new
public synchronization primitive (mutex + condition variable, app-owned).
`run_task_to_result` owns a complete explicit Runtime lifecycle
(build → start → submit → wait → request_stop → drain → join) with no implicit
ownership, cancellation, or drain. No AC-N rule is violated: the bridge is a
caller of existing authorities (ApplicationRuntime, Group, Completion), never a
new terminal-winner, wake, or reap authority.

`TaskResultSlot` logical state machine:

```text
Idle --first publish--> Published --first take--> Consumed
        later publishes: dropped (first wins)
        second take: std::bad_optional_access (deterministic contract-
        violation surface; never a silent moved-from value)
```

## Gate 1 — Ownership, Locks, and State Machine

`TaskResultSlot::mtx_` is a LEAF domain: held only to empose/observe the
optional and the done flag. While holding it the code calls no Scheduler, no
Runtime, no Group, no user code, performs no I/O, and allocates nothing
(enforced by the `T` nothrow-move-constructible static_assert — `publish` is
noexcept). No new lock-order edges: `mtx_` is never held across any other lock.
The bridge itself holds no locks; it sequences Runtime lifecycle calls from one
non-Runtime thread.

## Gate 2 — Resource and Failure Model

Capacity: one terminal outcome per slot (bounded by construction, no growth
dimension). `AwaitOpTally` is two counters. The bridge creates one Runtime,
one root task, and drives it to terminal — no container grows with cumulative
operations.

Failure model (failure-model.md classes):

- op errors / EOF / short transfers: T1/T7 — typed `Result` propagation with
  positional retry; zero-progress write is an invalid-state report (await
  variant: `backend_error` — deliberate bilaterally documented divergence);
- task-body exception: translated to the published outcome (T1-shaped typed
  result) — the Group boundary would otherwise swallow it and hang the caller;
- build/start throw, submit throw (P2-02 rollback-and-rethrow, e.g.
  `bad_alloc`): netted to `translate_task_exception` after best-effort
  `shutdown()` — the P1 fix; an escaping exception would otherwise unwind
  into `~ApplicationRuntime` while Running and fail fast
  (`group_lifetime_fail_fast`), i.e. process termination instead of a typed
  result. Deterministically regression-tested (`task_result_submit_throw_test`,
  internal-testing injection seam; production compiles the seam out);
- drain/join error: returned as the bridge result after best-effort shutdown
  (see Gate 4 rationale).

## Gate 3 — Progress and Wake Model

Persistent state: `done_` under `mtx_`; predicate `cv_.wait(lk, [this]{ return done_; })`.
Producer: task worker sets `done_` under the mutex, then `notify_all` —
publish-state is committed BEFORE the notify, so the commit-to-sleep race is
closed by the standard condition-variable protocol (no lost wake; no
undeclared polling anywhere in the bridge). The bridge's blocking wait
(`wait_and_take`) runs on a non-Runtime thread; the Runtime driver keeps
reaping I/O concurrently (same rule as the application slots it replaces).

## Gate 4 — Shutdown and Evidence

No slot-owned shutdown: `TaskResultSlot` has no background work, threads, or
I/O; its destructor is trivially safe in any logical state (the outcome is a
value). Runtime lifecycle shutdown is owned entirely by the bridge:
request_stop → drain → join, with best-effort `shutdown()` on every error leg.
drain/join precedence (deliberate, preserves the only sound audited
application semantics): a teardown failure after the task published is
RETURNED, never swallowed — `~ApplicationRuntime` fail-fasts in any state
other than Constructed/StartFailed/Stopped, so the audited `(void)drain();
(void)join();` application pattern could only reach its "return the task
result" line on the success path (on a real failure it terminated at scope
exit); returning the lifecycle error is the only sound observable. On the
success path the published task result is returned verbatim.

Evidence: `runtime_await_helpers_test` (helper semantics, both backends),
`task_result_submit_throw_test` (P1 deterministic regression + inert-seam
control), the four migrated application test suites, Debug + Release full
gates. Actual results recorded in the PR description at the final head SHA
(never pre-filled).
