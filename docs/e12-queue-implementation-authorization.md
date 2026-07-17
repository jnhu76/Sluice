# E12-E Queue Production Implementation Authorization

> **Decision identity:** `E12-E-QUEUE-PRODUCTION-IMPLEMENTATION-1`
>
> **Phase:** 0 — Implementation authorization gate
>
> **Verdict:**
>
> ```text
> E12-E-QUEUE-IMPLEMENTATION-AUTHORIZATION-1:
> BLOCKED
> ```
>
> No Queue production code, placeholder API, Scheduler seam, or test was
> created. All four preconditions (B1–B4) fail against the current binding
> authority, and the binding documents themselves independently declare
> `E12-E IMPLEMENTATION AUTHORIZATION: DENIED`. This file records the
> investigation only, as permitted by section B5 of the implementation task
> when the verdict is `BLOCKED`.

## Investigation method

Each of the four preconditions was investigated independently against the
repository at branch `e12-e-queue-production-impl`, HEAD `c6efa13`. Every
claim below is anchored to a `file:line` in the current tree. No production
code was modified and no test was run as part of this authorization check;
the gate is documentary and the evidence is static.

The binding authorities consulted:

```text
docs/e12-queue.md
docs/e12-queue-state-machine.md
docs/e12-queue-scheduler-integration.md
docs/e12-sync-primitives-plan.md
docs/async-mutex-nothrow-authority.md
docs/async-runtime-plan.md
```

Only the Corrective-2 sections of the three Queue documents are binding
(`docs/e12-queue.md:173` onward is explicitly `NON-BINDING HISTORICAL
ANALYSIS`).

---

## B1. Mutex no-throw substrate — FAILS

**Required:** `ASYNC-MUTEX-NOTHROW-AUTHORITY-1` must be independently
implemented in production, with `Mutex::lock/unlock` complying with the
no-throw / fail-fast authority, and the corresponding compile, TSA, and
runtime tests passing.

**Finding:** The authority is **design-only**. Its own header states
(`docs/async-mutex-nothrow-authority.md:3-12`):

```text
Design status:  PASS — INDEPENDENT REVIEW REQUIRED
Production status: NOT IMPLEMENTED
... E12-E Queue depends on this decision and remains
implementation-denied until this decision has passed an independent review
and its production realization has been separately authorized.
```

and its closing block (`docs/async-mutex-nothrow-authority.md:161-164`):

```text
ASYNC-MUTEX-NOTHROW-AUTHORITY-1 IMPLEMENTATION: UNAUTHORIZED
E12-E IMPLEMENTATION AUTHORIZATION: DENIED
```

**Production code is the pre-authority shape, unchanged.**
`include/sluice/async/mutex.hpp:27-29`:

```cpp
void lock() SLUICE_ACQUIRE() { impl_.lock(); }
bool try_lock() SLUICE_TRY_ACQUIRE(true) { return impl_.try_lock(); }
void unlock() SLUICE_RELEASE() { impl_.unlock(); }
```

This is byte-for-byte the "Current-source fact" the authority documents as
the state to be replaced (`docs/async-mutex-nothrow-authority.md:14-22`).
There is:

- no `noexcept` on `lock()` / `try_lock()` / `unlock()` (only the ctor at
  `mutex.hpp:19` carries it);
- no `try { ... } catch (...) { std::terminate(); }` fail-fast wrapper —
  `std::terminate` does not appear in `include/sluice/async/mutex.hpp` or
  anywhere under `src/async/`;
- no test of injected/controlled lock failure terminating rather than
  returning. `tests/e12_async_mutex_test.cpp` (1416 lines) covers the
  Fiber-suspending `AsyncMutex` (E12-C) behavior; a grep for `noexcept`,
  `terminate`, `nothrow`, `system_error`, `ASYNC-MUTEX-NOTHROW` returns
  zero hits. `tests/e12_async_mutex_authority_probe.cpp` is a negative
  compile probe for a *different* authority (`F-MTX-SEAM-1`).

**PR #11 landed only documentation.** `git show --stat c6efa13` shows PR #11
touched exactly two files, both non-production:

```text
docs/async-mutex-nothrow-authority.md   (+164)
scripts/run-e12-tlc-all.sh              (+113)
```

Zero `.hpp`, zero `.cpp`. The six verification obligations in
`docs/async-mutex-nothrow-authority.md:147-158` (TSA compile, no
`std::system_error` catch dependency, condition-variable reacquire
well-formedness, injected-failure termination, ASan/UBSan/TSan green,
independent adversarial review) are explicitly pending.

**TSA annotations exist but are orthogonal.** `mutex.hpp:17,27-29` and
`include/sluice/async/lock_guard.hpp:20-23` carry `SLUICE_CAPABILITY`,
`SLUICE_ACQUIRE`, `SLUICE_TRY_ACQUIRE`, `SLUICE_RELEASE`,
`SLUICE_SCOPED_CAPABILITY`; enforced via `-Wthread-safety` /
`-Werror=thread-safety` on the `sluice_async` target (`xmake.lua:50-51`).
These predate the nothrow authority and coexist with the still-throwing
lock body. They are not the no-throw substrate.

**Conclusion B1:** Not satisfied. The Scheduler consumes `Mutex` via
`global_mtx_` / `wake_mtx_` / `LockGuard` at many sites in
`src/async/scheduler.cpp` (e.g. lines 356, 378, 442, 526, 559 and ~15
more); until `ASYNC-MUTEX-NOTHROW-AUTHORITY-1` is independently reviewed,
implemented, and proven at runtime, the Queue cannot treat
`Mutex::lock/unlock` as no-throw or fail-fast.

---

## B2. Corrective-2 independent adversarial review — FAILS

**Required:** A genuine independent (non-author) adversarial review of
Corrective-2 covering one-shot lease unforgery, failed-push original
payload, ring lease uniqueness, active-victim ticket stealing, owner-slot
lifetime, PREPARED timer list/destruction order, teardown uniqueness,
`active_port_calls_` interpretation, 19/19 canonical transitions, 6/6
publication transitions, and the 33 counterexamples blocked by production
structure (not debug assertions).

**Finding:** No such review exists. The canonical home for independent
reviews, `docs/reviews/`, contains only:

```text
docs/reviews/E12-C-REVIEW.md                          (E12-C AsyncMutex)
docs/reviews/E12-D-CONDITION-PREPARATION-AUDIT-1.md   (E12-D AsyncCondition)
docs/reviews/e12-d-condition-cpp-implementation-plan-1.md (E12-D)
docs/reviews/024S-sync-runtime-merge-readiness.md     (sync runtime)
```

There is no `E12-E-REVIEW.md` or any E12-E entry, in any commit
(`git log --all --name-only -- "docs/reviews/"`). The "Corrective-2"
mentions in `E12-C-REVIEW.md` refer to
`E12-C-MIGRATION-EVIDENCE-CORRECTIVE-2`, an AsyncMutex test-evidence
corrective — a name collision, not the Queue Corrective-2.

**The only E12-E Queue reviewers of record are author self-assessment and
two doc-nit bots.** Every commit touching `docs/e12-queue*.md` is authored
by `jnhu <me@hoooo.org>` (GitHub `jnhu76`), the Corrective-2 author. PR #10
("docs(async): E12-E Corrective-2 integration architecture", `dc690f8`)
received exactly two bot reviews, both `state: COMMENTED`,
`authorAssociation: NONE`:

- `gemini-code-assist` — one comment suggesting a phrasing change
  ("complete with a closed outcome"); the bot announced its own sunset on
  2026-07-17.
- `coderabbitai` — four comments, all documentation nits (code-fence
  language tag at line 737; stale status rows at lines 1000-1005 and 1703;
  close-vs-producer-commit race clarification at lines 195-221); profile
  `CHILL`; a separate "Review limit reached" message confirms no full pass.

Neither bot engaged any of the 11 required topics. A grep for "CodeRabbit"
or "Gemini" inside the three Corrective-2 docs returns zero matches — the
docs never cite these tools as having reviewed their invariants.

**Coverage of the 11 required topics by an independent party: 0/11.** Each
topic is argued only in self-authored design prose:

| # | Required topic | Self-audit location | Independent coverage |
|---|---|---|---|
| 1 | one-shot `QueueItemLease` unforgery | `e12-queue-scheduler-integration.md:172-199,219,950-957` | none |
| 2 | failed push returns original unique payload | `e12-queue.md:40,62-72,956-957` | none |
| 3 | ring never has multiple leases of same control | `e12-queue.md:82-86,955` | none |
| 4 | active victim ticket stealability | `e12-queue.md:44,960-917`; *and* flagged unresolved via T25 (`e12-queue.md:144-154`) | none |
| 5 | owner-slot address lifetime | `e12-queue.md:45,963-964` | none |
| 6 | PREPARED timer list iterator / destruction order | `e12-queue-scheduler-integration.md:564-633,968-977,374-378` | none |
| 7 | teardown session uniqueness / irreversibility | `e12-queue.md:46,97-102,977-982` | none |
| 8 | `active_port_calls_` is not full typed-call lifetime | `e12-queue.md:47,543,982` | none |
| 9 | 19/19 canonical transitions | self-stamped at `e12-queue.md:111-112`, `e12-queue-state-machine.md:461-467` ("AUTHOR SELF-ASSESSMENT") | none |
| 10 | 6/6 publication transitions | same self-stamp locations | none |
| 11 | 33 counterexamples blocked by structure, not debug asserts | `e12-queue-scheduler-integration.md:944-986`; the table itself says `:946` "Each disposition is a design claim, not production evidence" and `:984-986` "Production proof still requires implementation, tests, and independent review; none is authorized here" | none |

**The binding documents themselves concede the gap.** Direct quotes:

- `docs/e12-queue.md:6` — `Status: PASS — AUTHOR SELF-ASSESSMENT —
  INDEPENDENT REVIEW REQUIRED`
- `docs/e12-queue.md:162` — "Corrective-2 needs a fresh independent
  adversarial review"
- `docs/e12-queue.md:168` — `E12-E IMPLEMENTATION AUTHORIZATION: DENIED`
- `docs/e12-queue-scheduler-integration.md:6,19,30-31,984-986` — same
  `AUTHOR SELF-ASSESSMENT` / `INDEPENDENT ADVERSARIAL REVIEW REQUIRED` /
  "none is authorized here"
- `docs/e12-queue-state-machine.md:458-472` —
  `E12-E-QUEUE-STATE-MACHINE-DESIGN-CORRECTIVE-2: PASS — AUTHOR
  SELF-ASSESSMENT` / `INDEPENDENT ADVERSARIAL REVIEW REQUIRED` /
  `E12-E IMPLEMENTATION AUTHORIZATION: DENIED`

**The five "review findings" commits are not independent review.**
`9ce2577` ("fix Corrective-2 critical defects from review") and `9c7573b`
("fix review findings from CodeRabbit and Gemini") were both authored by
`jnhu`; the former's "review" is the author's own self-audit (no external
reviewer named), the latter applied the two bots' doc nits.
`e79f088` ("fix TLC runner review defects") is a six-line shell fix to
`scripts/run-e12-tlc-all.sh` unrelated to Queue design.

**Conclusion B2:** Not satisfied. Per the authority rule "author
self-review is not independent review," Corrective-2 has no independent
adversarial review. The required review must be performed by a context
distinct from the Corrective-2 author and must cover all 11 topics with
production-structure (not debug-assertion) reasoning for the 33
counterexamples.

---

## B3. Condition T25 migration-reacquire-hang audit — FAILS

**Required:** `E12-CONDITION-T25-MIGRATION-REACQUIRE-HANG-AUDIT-1` must be
closed with reproducible original hang evidence, an identified root cause,
a fix or clear proof of unrelatedness to the production Scheduler, the
complete Condition runtime suite passing, and no hidden timeouts, skipped
tests, or weakened assertions.

**Finding:** The audit is **open**. The binding authorities state
uniformly (`docs/e12-sync-primitives-plan.md:69-76`, `:1736`,
`:1784-1786`; `docs/e12-queue.md:144-154`):

```text
E12-CONDITION-T25-MIGRATION-REACQUIRE-HANG-AUDIT-1:
SEPARATE REQUIRED TASK

Condition build: PASS
Condition runtime suite: INCOMPLETE
T25 migration/reacquire: HANG OBSERVED
```

and `docs/e12-queue.md:152-154`: "The T25 hang neither proves nor
disproves active-victim Queue ticket stealing, but it must close
independently before Queue implementation." A repo-wide search for
`T25.*(CLOSED|PASS|RESOLVED|FIXED)` returns zero matches.

**Reproducible evidence exists; no fix exists.** The hang is reproduced by
`e12_cond_t25_migration_condition_reacquire` at
`tests/e12_async_condition_test.cpp:1348-1429` (case body 1355-1429), a
real two-worker migration test using `sched.run_live(2)` with an E8 steal.
`git log -- tests/e12_async_condition_test.cpp include/sluice/async/condition.hpp
src/async/scheduler.cpp` shows the last commit touching any of them is
`aeb9255` (the original E12-D landing). No corrective commit exists.

**Root cause (identified by analogy, never applied):** the T25 coordinator
spins unbounded:

```text
tests/e12_async_condition_test.cpp:1404-1406
    while (!blocker_running.load(...)) { std::this_thread::yield(); }
tests/e12_async_condition_test.cpp:1415-1417
    while (!a_unlocked.load(...)) { std::this_thread::yield(); }
```

When the intended steal/migration interleaving does not occur, `a_unlocked`
is never set and the process hangs. The sibling Mutex migration test
`e12_mtx_t19_real_migration_lock_own_unlock` went through four correctives
to eliminate exactly this class of hang (`docs/e12-async-mutex.md:1262-1306`
§17.3.1): bounded coordinator waits via `bounded_wait`/`bounded_pred`, a
`release_for_drain()` failure-bound guard, and an `f_idle` fiber keeping
the steal target busy.

**The Condition test ships those helpers but never calls them.**
`tests/e12_async_condition_test.cpp:89-105` defines `bounded_wait` and
`bounded_pred` (both `[[maybe_unused]]`); grep finds zero call sites in
the file. The Mutex test calls `bounded_wait` six times. This is precisely
why the authority labels the Condition suite `RUNTIME SUITE INCOMPLETE`
while the Mutex suite is `IMPLEMENTATION-1 COMPLETE`.

**Not unrelated to the production Scheduler.** The path under test is the
production path: `Scheduler::try_steal` (`src/async/scheduler.cpp:533-543`),
`run_live` (`src/async/scheduler.cpp:403-...`), and the production reacquire
`AsyncCondition::wait` -> `mutex_.lock(reacquire_node)` at
`include/sluice/async/condition.hpp:223-254` (reacquire at line 250).

**No hidden timeouts / skipped tests / weakened assertions mask the hang.**
The reverse is true: the hang is caused by *unbounded* waits. The only
test exclusions are honestly disclosed destruction-contract death tests
(`tests/e12_async_condition_test.cpp:1338-1342`), excluded for lack of a
death-test harness and verified by ASan/debug builds instead. The T25
assertions themselves (`:1421-1428`) are strong; they are simply
unreachable because the preceding spin hangs.

**Conclusion B3:** Not satisfied. The audit is `SEPARATE REQUIRED TASK` /
`HANG OBSERVED`, the runtime suite is `INCOMPLETE`, no fix has landed
since `aeb9255`, and the hang is on the production Scheduler path.

---

## B4. Queue formal model normalization — FAILS

**Required:** A Queue formal model covering Corrective-2 Model A (bounded
MPMC FIFO: bounded MPMC FIFO, producer/consumer waiters, FIFO/no-barging,
lease ownership uniqueness, buffer bounds) and Model B (Open→Closed
monotonicity: Open→Closed monotonicity, close rejects producers, buffer
drain after close, Closed+empty consumers, close-vs-push/pop races),
reflecting one-shot lease/control location, no Permit owner, no direct
handoff, active-victim stealing, irreversible teardown, 19 canonical and
6 publication transitions — with a positive TLC gate, named invariants, a
corresponding negative model, a wrong-property gate, a verify script, and
repeatable commands with exit codes. Old model elements (cancellation
outcome, Permit, active-owner steal veto, reusable item identity) must be
absent.

**Finding:** **No Queue formal model exists in any form.** Every other E12
primitive has a TLA+ spec under `docs/spec/`:

```text
docs/spec/e10_waitnode/
docs/spec/e11_timer_wait/
docs/spec/e12_async_condition/
docs/spec/e12_async_mutex/
docs/spec/e12_event/
docs/spec/e12_semaphore/
docs/spec/e7_multiworker_progress/
docs/spec/e7_publication/
docs/spec/e8_ownership_transfer/
docs/spec/e9_park_wake/
```

`find . -iname "*queue*.tla"` (excluding vendored `node_modules`) returns
zero hits. `grep -rln "QueueItemLease|QueueTeardown|one-shot lease|
active.victim|teardown.*session" --include=*.tla --include=*.cfg` returns
zero hits. There is no `scripts/verify-e12-queue-formal.sh` and no Queue
entries in `scripts/run-e12-tlc-all.sh` (whose header at line 2 scopes it
to "E12-D AsyncCondition"). The `tla2tools.jar` at the repo root has no
Queue spec to check.

**The binding authority disclaims formal coverage.** Direct quotes:

- `docs/e12-queue.md:141-142` — "Corrective-2 modifies no TLA+ artifact.
  Formal status is not updated and no formal PASS is claimed."
- `docs/e12-queue-state-machine.md:421-430` — "Existing Queue models have
  not been normalized to the one-shot lease/control location, active-victim
  stealing, or teardown lifecycle. Therefore: `FORMAL/TLA CORRECTIVE-2
  STATUS: NOT UPDATED` / `FORMAL PASS: NOT CLAIMED`."
- `docs/e12-queue.md:164-165` — gate 4 explicitly defers: "later formal
  normalization must preserve the one-shot lease and corrected
  steal/teardown semantics."
- `docs/e12-queue.md:168` / `docs/e12-queue-state-machine.md:472` —
  `E12-E IMPLEMENTATION AUTHORIZATION: DENIED`.

The "19/19 canonical" and "6/6 publication" claims in
`docs/e12-queue-state-machine.md:461-467` are author self-assessment prose
inside a Markdown document, not the output of any TLC run against a `.tla`
spec. The candidate invariants at `docs/e12-queue.md:1381-1414` are
Phase-5 planning prose, not formalized.

**Coverage matrix (every required element):**

| Required element | Status |
|---|---|
| Model A — bounded MPMC FIFO | NOT COVERED (no `.tla`) |
| Model A — producer/consumer waiters | NOT COVERED |
| Model A — FIFO / no-barging | NOT COVERED (prose only) |
| Model A — lease ownership uniqueness | NOT COVERED (prose only) |
| Model A — buffer bounds | NOT COVERED (prose only) |
| Model B — Open→Closed monotonicity | NOT COVERED (prose only) |
| Model B — close rejects producers | NOT COVERED |
| Model B — buffer drain after close | NOT COVERED |
| Model B — Closed+empty consumers | NOT COVERED |
| Model B — close-vs-push/pop races | NOT COVERED (prose only) |
| positive TLC gate | absent |
| named invariants | absent |
| negative model | absent |
| wrong-property gate | absent |
| verify script | absent |

**Conclusion B4:** Not satisfied. A Queue TLA+ spec, invariants, negative
models, wrong-property gate, and `scripts/verify-e12-queue-formal.sh` must
all be authored before this gate can pass. The precedent shape is
`scripts/verify-e12-async-condition-formal.sh` (positive TLC + named
invariant + NEG-C1..C10 + wrong-property gate, exit 0 on all-as-expected).

---

## Authorization conclusion

```text
E12-E-QUEUE-IMPLEMENTATION-AUTHORIZATION-1:
BLOCKED
```

All four preconditions fail. In accordance with section B5 of the
implementation task, under `BLOCKED` only investigation reports, necessary
precondition-dependency fixes, and their tests are permitted. None of the
following was created:

- no Queue production implementation skeleton;
- no empty `AsyncQueue<T>` API or placeholder Scheduler seam;
- no fake or speculative Queue test;
- no modification to `include/sluice/async/async_queue.hpp`,
  `include/sluice/async/detail/queue_*.hpp`, `src/async/queue_*.cpp`, or
  `src/async/scheduler.cpp` for Queue purposes;
- no modification to any Queue design document or formal model.

The blocking preconditions and their owners (each is a separate required
task per the binding authority):

1. **B1 — Mutex no-throw substrate.** Author, independently review, and
   implement `ASYNC-MUTEX-NOTHROW-AUTHORITY-1` in production
   (`include/sluice/async/mutex.hpp:27-29`); satisfy the six obligations
   at `docs/async-mutex-nothrow-authority.md:147-158`; land a runtime test
   of injected-failure termination.
2. **B2 — Corrective-2 independent adversarial review.** Commission a
   review by a context distinct from the Corrective-2 author, covering all
   11 required topics, with production-structure reasoning for the 33
   counterexamples. Deposit the artifact at `docs/reviews/E12-E-REVIEW.md`
   (or equivalent) following the precedent of `docs/reviews/E12-C-REVIEW.md`.
3. **B3 — Condition T25 hang audit.** Close
   `E12-CONDITION-T25-MIGRATION-REACQUIRE-HANG-AUDIT-1`: port the Mutex
   T19 corrective pattern (bounded coordinator waits, `release_for_drain`,
   `f_idle`) to `tests/e12_async_condition_test.cpp:1348-1429`, drive the
   Condition runtime suite to green, and update the verdict blocks in
   `docs/e12-sync-primitives-plan.md:69-76` and `docs/e12-queue.md:144-154`.
4. **B4 — Queue formal model.** Author the Queue TLA+ spec(s) for Models A
   and B under `docs/spec/e12_queue/`, encode the Corrective-2 shape
   (one-shot lease, no Permit, no direct handoff, active-victim stealing,
   irreversible teardown, 19 canonical + 6 publication transitions), define
   named invariants, build the matching negative and wrong-property models,
   and add `scripts/verify-e12-queue-formal.sh` plus a Queue entry in
   `scripts/run-e12-tlc-all.sh` with repeatable commands and exit codes.

Until all four are independently satisfied and recorded, the Queue
production implementation task remains blocked at Phase 0 and no
production Queue code may be written.

---

## Repository state at end of investigation

```text
branch:           e12-e-queue-production-impl
HEAD:             c6efa13 (unchanged from investigation start)
working tree:     dirty (this file is the only addition)
untracked files:  docs/e12-queue-implementation-authorization.md
                  tests/test_t3_simple.cpp  (pre-existing, unrelated)
                  tla2tools.jar             (pre-existing, unrelated)
pushed:           no
```

No commit has been made. The branch was created from `master` at `c6efa13`
for this investigation; only this authorization report is staged for
commit, pending user direction.
