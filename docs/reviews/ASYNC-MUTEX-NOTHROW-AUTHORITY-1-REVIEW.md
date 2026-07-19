# ASYNC-MUTEX-NOTHROW-AUTHORITY-1 — Independent Adversarial Review

```text
ASYNC-MUTEX-NOTHROW-AUTHORITY-1-INDEPENDENT-REVIEW-1
```

> **Scope:** design review only. No production code (`include/`, `src/`, `tests/`,
> `xmake.lua`) was modified. A repo-external scratch probe and a temporary,
> fully-reverted in-tree header overlay were used solely to verify compile/TSA
> and runtime regression claims; the working tree is byte-identical to review
> start (§K).

The authority document under review is `docs/async-mutex-nothrow-authority.md`.
The author's own `PASS` stamp and the binding Queue documents' `E12-E
IMPLEMENTATION AUTHORIZATION: DENIED` are treated as claims, not evidence.

---

## A. Verdict

```text
ASYNC-MUTEX-NOTHROW-AUTHORITY-1-INDEPENDENT-REVIEW-1:
PASS
```

`PASS` means only that the **design** — Candidate A (make the existing single
`sluice::async::Mutex` interface `noexcept` and fail-fast on underlying
acquisition failure) — is correct, the most appropriate of the three
candidates considered here, compatible with the C++ lock concepts and the
existing TSA build, sufficient for the E12-E Queue winner `CommitGap`, and that
all 15 counterexamples are blocked by production structure or rationally
excluded. It does **not** authorize production implementation.

```text
PRODUCTION IMPLEMENTATION:
NOT AUTHORIZED BY THIS REVIEW
```

Production authorization additionally requires closing the two P1/P0
correctives in §I (an injectable failure seam with a real death-test, and an
API-reference contract update) plus the four B1–B4 Queue gates. See §J.

### Verdict matrix

| Axis | Finding |
|---|---|
| Current-source facts (§C) | **ACCURATE** — every claim reproduces at the cited `file:line`. |
| Candidate A vs B vs C (§D) | **A is the best choice** — B/C are rejected on real, code-grounded grounds, not on the author's "two policies on one type" hand-wave alone. |
| C++ lock-concept compatibility (§C1–C3) | **PASS** — `BasicLockable`/`Lockable` retained; `std::unique_lock<Mutex>`, `std::lock_guard<Mutex>`, `std::condition_variable_any`, `sluice::async::LockGuard` all compile and run against the proposed shape; `noexcept` does not change `BasicLockable`/`Lockable` validity. |
| TSA build (§C3) | **PASS** — `sluice_async` + `sluice_async_internal_testing` and the ten async test targets compile under `-Wthread-safety -Werror=thread-safety` with the proposed `Mutex` applied. |
| `condition_variable_any` reacquire (§C3) | **PASS** — the `park_on_wake_source` release-wait-reacquire path routes through `Mutex::unlock`/`Mutex::lock`; compiles + runs. |
| Queue `CommitGap` lock audit (§F) | **PASS** — the winner path needs only `global_mtx_` + `state_mtx_` + at most one role `WaitQueue` mutex, all of which are `sluice::async::Mutex`. `inbox_mtx` is a `std::mutex` and is **not** touched post-winner. |
| Lock-order / deadlock (§E) | **NO NEW RISK** — `noexcept` changes failure behavior only; the existing `global_mtx_ -> wake_mtx_` and `global_mtx_ -> q.mtx()` edges have no reverse edges. |
| `catch (...)` boundary (§G-note) | **DOCUMENTARY ONLY** — redundant with the `noexcept` terminate guarantee; acceptable, but the implementation task should add a single named fail-fast helper (P2). |
| Failure injection (§G) | **GAP — P1** — design asserts injectability but specifies no concrete seam. A test-only-backend + death-test design is required before implementation (§G, §J). |
| ABI / API (§C8) | **SAFE in-tree** — `noexcept` is *not* part of Itanium mangling; symbol names unchanged. Source-level function-type change is real but no in-repo TU takes `&Mutex::lock`. API contract update is a P1 documentary item. |
| Runtime regression (§C9) | **PASS** — the ten async test binaries (incl. `e12_async_mutex_test`, `e12_async_condition_test`) build and pass against the proposed shape (debug/Clang/TSA). Full sanitizer matrix is an implementation-task obligation. |
| Queue unblock / B1 (§J) | **NOT CLOSED** — B1 also needs production realization + the failure-injection death-test + the independent implementation review; a design PASS here is only one of four gates. |
| 15 counterexamples (§H) | **ALL BLOCKED OR JUSTIFIED**. |

---

## B. Executive conclusion

```text
Is the current design correct?               YES, as a design.
Candidate A/B/C — which to choose?            A (modify the existing Mutex).
Is there a Queue blocker?                     NO design-level blocker.
Can production implementation be authorized?  NO — see §I/§J.
```

The authority's core claim — that a `sluice::async::Mutex` acquisition that
participates in an authoritative Scheduler transition (or, by the Queue's
design, a winner `CommitGap`) must fail-fast rather than propagate a
recoverable exception — is **correct and the right policy**. The decisive
evidence is internal to the runtime, not the Queue doc:

* The runtime has **no path that catches a `std::system_error` from any
  `Mutex`** (`rg "system_error"` over `include/ src/ tests/` returns zero
  matches). Every consumer already implicitly treats acquisition failure as
  unrecoverable in practice; there is no recoverable semantics to preserve.
* All `Mutex` use is inside `sluice::async` Scheduler code
  (`src/async/scheduler.cpp`: ~45 `LockGuard`/`unique_lock<Mutex>` sites) or
  its private `WaitQueue::mtx_`. `Mutex` is an installed header
  (`include/sluice/async/mutex.hpp`) but the API surface is one tiny
  `BasicLockable` shim; the public primitive is `AsyncMutex`, a distinct
  Fiber-suspending type that does **not** delegate to `Mutex` for its
  semantics (it uses `Scheduler::mutex_*` seams under `global_mtx_`).

Candidate B (new internal `FailFastMutex`) is rejected because it would split
a single capability type into two and force a non-trivial migration of every
`LockGuard`/`unique_lock<Mutex>` site (the Scheduler has ~45) for zero
behavioral benefit — the throwing policy is *unused* everywhere. Candidate C
(`lock_or_terminate` seam) is rejected because it is non-`BasicLockable` and
therefore cannot back `std::unique_lock`/`std::condition_variable_any`, which
the runtime genuinely uses (`scheduler.cpp:285`).

The design does **not** "auto-expand" the Queue requirement into a global API
policy. The policy is justified independently by the runtime's own structure
(no catcher, internal-only type, single no-throw zone around irreversible
Scheduler transitions), and the Queue merely inherits it.

Two correctives block production authorization (§I):

* **P0** — failure-injection seam is unspecified. The design *requires* an
  injectable lock failure that terminates (authority §7 obligation 4), but
  names no concrete seam. A real death-test is mandatory.
* **P1** — the public `Mutex` API contract change must be recorded in the API
  reference (the header is installed), and a single named fail-fast helper
  should replace the bare `std::terminate()` calls.

---

## C. Current-source verification

Every claim reproduces at the cited `file:line`.

### C1.1 The current interface is throwing (no `noexcept`)

`include/sluice/async/mutex.hpp:17-33`:

```cpp
class SLUICE_CAPABILITY("mutex") Mutex {
public:
    Mutex() noexcept = default;
    ~Mutex() = default;
    // ...
    void lock() SLUICE_ACQUIRE() { impl_.lock(); }                  // :27
    bool try_lock() SLUICE_TRY_ACQUIRE(true) { return impl_.try_lock(); }  // :28
    void unlock() SLUICE_RELEASE() { impl_.unlock(); }              // :29
private:
    std::mutex impl_;
};
```

Only the constructor (`:19`) carries `noexcept`. Authority §1 is **accurate**.

### C1.2 Direct delegation to `std::mutex`

`Mutex::lock`/`try_lock`/`unlock` are one-line forwarders to `impl_`
(`:27-29`). No wrapper layer. Authority §1 is **accurate**.

### C1.3 No existing exception→terminate conversion

`rg "system_error|terminate" include/sluice/async/ src/async/` returns no
matches inside the async runtime. `std::terminate` appears only in repo-test
`catch (...) { }` swallows in unrelated I/O tests
(`tests/file_vec_test.cpp:32`, `tests/uring_*.cpp`, etc.). **Accurate**.

### C1.4 No caller catches `std::system_error` from any `Mutex`

`rg "system_error" include/ src/ tests/` → **zero matches**. There is no
recoverable semantics to break. Authority §7 obligation 2 ("no current call
site depends on catching `std::system_error` from `Mutex`") is **already
satisfied today**, independent of the no-throw change. **Accurate**.

### C1.5 Complete inventory of `Mutex` users

`rg -n "LockGuard|unique_lock<Mutex>" src/async/scheduler.cpp` → ~45 sites,
all inside `sluice::async::Scheduler` methods, taking `global_mtx_`,
`wake_mtx_`, `q.mtx()` (a `WaitQueue::mtx_`), or
`SchedulerWakeHandle::Control::mtx`. Plus:

* `include/sluice/async/wait_queue.hpp:319` — `Mutex mtx_;` (private member;
  exposed to `Scheduler` only via the friend-gated `mtx()` seam at `:152`).
* `include/sluice/async/lock_guard.hpp:20-32` — the RAII wrapper.
* `tests/tsa-probe/tsa_*.cpp` — six external TSA compile-probes (positive +
  negative). All build against the public surface only; none depends on
  throwing.

**No documentation-omitted use site.** The authority's scope table (§4) lists
`global_mtx_`, `QueueCore::state_mtx_`, each `WaitQueue::mtx_`,
`Scheduler::wake_mtx_`, and `SchedulerWakeHandle::Control::mtx`. All present
except the Queue ones, which is correct because **no Queue code exists yet**
(no `include/sluice/async/async_queue.hpp`, no `src/async/queue_*.cpp`; B4
fails — §J).

### C1.6 The three `Mutex`-like types are distinct

* `sluice::async::Mutex` — the annotated `std::mutex` shim under review.
* `sluice::async::AsyncMutex` — Fiber-suspending primitive
  (`include/sluice/async/async_mutex.hpp`). Backed by a `Fiber* owner_` + a
  private `WaitQueue waiters_`, **not** by `Mutex`. Its `Scheduler::mutex_*`
  seams serialize on `global_mtx_` + `waiters_.mtx()`; they do not call
  `Mutex::lock`. So this authority does **not** touch `AsyncMutex`.
* `std::mutex` — used at `WorkerState::inbox_mtx`
  (`include/sluice/async/scheduler.hpp:149`), in `Future<T>`, `Group`,
  `threadpool_backend`, `async_io_context::access_mtx_`, etc. **Out of
  scope** of this authority. Critical for §F (the Queue claim about
  `inbox_mtx`).

---

## D. Candidate comparison

The authority only argues Candidate A. Here is the full comparison.

### Candidate A — modify the existing `Mutex`

```cpp
void lock()    noexcept SLUICE_ACQUIRE()         { try { impl_.lock(); }  catch (...) { std::terminate(); } }
bool try_lock() noexcept SLUICE_TRY_ACQUIRE(true){ try { return impl_.try_lock(); } catch (...) { std::terminate(); } }
void unlock()  noexcept SLUICE_RELEASE()         { impl_.unlock(); }
```

### Candidate B — keep `Mutex` throwing, add internal `FailFastMutex`

A *new* type in `include/sluice/async/detail/` (or `src/async/`), annotated
with its own `SLUICE_CAPABILITY`, used only by the Scheduler/Queue paths.
Public `Mutex` keeps current throwing semantics.

### Candidate C — keep throwing `Mutex`, add `lock_or_terminate()` seam

A non-`BasicLockable` helper: `void lock_or_terminate(Mutex&)`. Callers that
need fail-fast explicitly call the seam.

### Selection matrix

| Axis | A: modify Mutex | B: new FailFastMutex | C: lock_or_terminate seam |
|---|---|---|---|
| **Correctness** | One policy, one type — mechanically unambiguous. | Two types with identical mechanics but different failure policy — easy to grab the wrong one at a call site; `LockGuard`/`unique_lock` must be templated/duplicated. | Two ways to lock the *same* object; nothing prevents a caller from using plain `Mutex::lock` (throwing) on a fail-fast path. Weakest. |
| **Scope** | One header (`mutex.hpp`); ~45 call-sites unchanged textually. | One new header + migration of all ~45 Scheduler sites (and every `WaitQueue::mtx_`) to the new type + dual `LockGuard`/`unique_lock` alias. | One new helper header; call sites must opt in — easy to miss one. |
| **API impact** | Public `Mutex` contract changes (throwing→terminate). Documented contract update needed (P1). | Public `Mutex` unchanged; new internal type. Lowest API impact. | Public `Mutex` unchanged; new public-ish helper. |
| **ABI risk** | None in-tree: `noexcept` is not part of Itanium mangling (verified — see §C8); no out-of-line `Mutex` member; no in-repo `&Mutex::lock`. | None — new symbol. | None — new symbol. |
| **Testability** | One type, one injection seam. | Two types — must test both; the throwing `Mutex` has no consumer, so its throw-test is dead coverage. | Two paths — must test both. |
| **Hot-path cost** | One `try`/`catch` frame per acquire in the source; optimized to a direct call + (in the no-throw fast path) zero extra instructions — verified the scheduler compiles + the test suite runs at full speed. | Same per-type cost; duplicated. | Same; plus an extra function hop on the fail-fast path. |
| **Queue suitability** | `CommitGap` already takes only `Mutex`-typed locks (§F). Directly satisfies the no-throw requirement. | Also satisfies it — *if* the Queue/Scheduler is migrated. Until then, the Queue sees the throwing `Mutex`. Forces a big-bang migration to unlock B1. | Satisfies it *only* if every Queue/Scheduler site remembers to call the seam.Brittle. |
| **`condition_variable_any` compatibility** | Yes — `Mutex` remains `BasicLockable`/`Lockable`; `unique_lock<Mutex>` + `cv.wait_until` compile and run (verified). | Yes, but needs a second annotated capability type for the cv. | **No** — `lock_or_terminate` is not `BasicLockable` (returns `void`, fine, but is a free function and has no `try_lock`/`unlock` of its own); cannot back `unique_lock<Mutex>` or `condition_variable_any`. Rejected on this ground alone. |
| **Migration complexity** | Low — textual signature change; all call-sites recompile. | High — type migration across the Scheduler + `WaitQueue` + every test. | Medium — audit every site. |

**Decision: Candidate A.** The authority's stated rationale ("maintaining
two exception policies on the same `BasicLockable` type would make
`LockGuard`, `unique_lock`, and condition-variable reacquisition ambiguous")
is correct but **understated** — the stronger, code-grounded reasons are:

1. The throwing policy is **unused** today (zero `system_error` catchers), so
   Candidate B preserves a property nobody consumes.
2. Candidate C is mechanically incompatible with `std::condition_variable_any`,
   which the runtime uses at `scheduler.cpp:285` (`std::unique_lock<Mutex> lk(wake_mtx_); wake_cv_.wait_until(lk, ...)`).
3. Candidate B would require a big-bang Scheduler migration **before** the
   Queue could be unblocked, serializing B1 behind unrelated work — a
   process defect, not a correctness one.

Candidate A's only real cost is the public API contract change (P1, §I).

---

## E. Lock and exception call graph

### E.1 Lock-order topology (current, unchanged by this design)

Verified against `src/async/scheduler.cpp`:

```
global_mtx_  ──►  WaitQueue::mtx_ (q.mtx())           [forward, ~25 sites]
global_mtx_  ──►  wake_mtx_                           [signal_wake_locked, route_runnable_locked]
global_mtx_  ──►  (SchedulerWakeHandle::Control::mtx acquired WITHOUT global_mtx_, in notify())
wake_mtx_    ──►  (nothing — wake_epoch_ is the only guarded field; cv.wait_until uses unique_lock<Mutex>)
WorkerState::inbox_mtx (std::mutex)  ──►  (nothing)
```

**No reverse edge exists.** `signal_wake_locked` acquires `wake_mtx_`
*singly* (`scheduler.cpp:238`); it is called with `global_mtx_` held at
`route_runnable_locked:933`, `advance_clock:1188`, the two termination paths
(`:719`, `:788`), and `attach_ready_wake:2635` — all forward order. The
park-timeout cache is read lock-free precisely to avoid a `wake_mtx_ ->
global_mtx_` inversion (`scheduler.cpp:291` documents this).

The authority's stated Queue order (§6) is consistent with this topology:

```
global_mtx_  ──►  QueueCore::state_mtx_  ──►  at most one role WaitQueue mtx
global_mtx_  ──►  wake_mtx_
```

### E.2 Exception call graph (current)

* `Mutex::lock/try_lock/unlock` propagate whatever `std::mutex` throws.
* `LockGuard` constructor calls `Mutex::lock` (propagates); destructor calls
  `Mutex::unlock` (propagates).
* `std::unique_lock<Mutex>` likewise.
* `std::condition_variable_any::wait_until` calls `Mutex::unlock` then, on
  wake, `Mutex::lock` (per cppreference: "If this postcondition cannot be
  satisfied, calls `std::terminate`" — i.e. the standard *already* imposes
  terminate-on-reacquire-failure for any `Lockable`; the authority's policy
  aligns with, rather than fights, the standard).
* **No `Mutex` consumer catches the exception.** The propagation is, in
  effect, fatal today — just noisily (uncaught) rather than cleanly
  (terminate).

After Candidate A: every edge that used to propagate now calls
`std::terminate()` at the `Mutex` boundary, *before* the exception reaches
any Scheduler transition. This is strictly tighter.

### E.3 `noexcept` cannot fix a deadlock

Noted and agreed. `noexcept`/fail-fast changes only failure behavior. The
lock-order audit (§E.1) is the deadlock control, and it is independent of
this authority. No deadlock blocker is introduced or concealed by this
design.

---

## F. Queue `CommitGap` audit

**Question:** after a Queue winner `WaitNode::resolve_` CAS succeeds, does
any acquisition in the winner path take a lock **not** governed by this
fail-fast authority?

### F.1 The Queue design's own `CommitGap` (binding Corrective-2)

`docs/e12-queue-scheduler-integration.md`:

* §11 "Locking, lifetime, allocation, and exception boundaries" (`:789-826`):
  ```
  G = Scheduler::global_mtx_
  S = QueueCore::state_mtx_
  P = producer WaitQueue mutex
  C = consumer WaitQueue mutex

  G -> S -> exactly one of P/C
  G -> wake mutex
  P and C are never held together
  ```
  and the binding empty sets (`:819-826`):
  ```
  allocation after winner CAS: NONE
  recoverable exception after winner CAS: NONE
  payload destruction under Queue/Scheduler locks: NONE
  owner-map insertion/erase/lookup during publication or steal: NONE
  ```

* §12 reconciliation / publication ordering (`:855-867`): every step after
  the winner CAS is a pointer/index/CAS/intrusive-link/Mutex-acquisition
  governed by G/S/P-or-C — exactly the set the authority's §5 enumerates.

### F.2 Cross-check against the current Scheduler

The future Queue reconciliation reuses the *existing* Scheduler mechanisms
for: timer activation/retirement (`timer_pool_`, `deadline_heap_`, guarded
by `global_mtx_` — all `Mutex`-domain), runnable ticket append
(`route_runnable_locked` under `global_mtx_`), and wait resolution
(`wake_wait_one_locked` / `mutex_handoff_one_locked` under `global_mtx_` +
`q.mtx()`). Every one of these takes a `sluice::async::Mutex`. So the
existing seams already inherit the fail-fast policy under Candidate A.

### F.3 The `inbox_mtx` claim

The authority §4 explicitly declines to cover `std::mutex` fields like
`WorkerState::inbox_mtx` and asserts the Queue design "selects the former:
Queue runnable tickets are linked under the already-held `global_mtx_`, not
under `inbox_mtx`."

**Verified against current code + Queue design:**

* `WorkerState::inbox_mtx` is a `std::mutex`
  (`include/sluice/async/scheduler.hpp:149`), touched only in:
  `spawn`/`spawn_on`/`run_impl` distribute (`:359,:387,:448`), the worker
  loop's pop (`:519`), termination broadcasts (`:715,:784`), `route_runnable`
  /`route_runnable_locked` (`:844,:925`), `classify_locked`
  (`:942`), `runnable_count` (`:2646`), and `try_steal`
  (`:2676,:2696`). **All of these are runnable-ticket transport, not
  authoritative transition internals.**
* The Queue design's "operation-embedded intrusive runnable ticket"
  (`e12-queue-scheduler-integration.md:71,:816`: "runnable ticket append |
  embedded intrusive link | G | after winner commit | no allocation/no
  throw") states the winner publishes its ticket under **G** alone — the
  intrusive link is appended to the per-Worker or global runnable list, and
  the *later* `WorkerState::inbox_mtx` pop is a transport step that happens
  *outside* the winner `CommitGap` (the winner has already committed; the
  worker that picks up the ticket is resuming an already-published
  operation).

So the `CommitGap` proper takes no `inbox_mtx`. The subsequent
`inbox_mtx`-guarded ticket pop on the worker side is a transport detail
whose own failure semantics are a separate question — but **it is outside
the no-throw zone** this authority governs, exactly as the design claims.

### F.4 Are there hidden post-winner lock acquisitions?

Reviewed the reconciliation/pub transition table at
`e12-queue-scheduler-integration.md:911-916` (PUB-P-COMM / PUB-P-CLOSED /
PUB-P-EXPIRE / PUB-C-COMM / PUB-C-CLOSED / PUB-C-EXPIRE). Every row's
"Locks" column is `G+S+P` or `G+S+C`, and every row's last column ends in
"no allocation/throw; op live". No row acquires `inbox_mtx` or any
`std::mutex` in the winner critical section.

**Conclusion:** the authority is sufficient for the Queue `CommitGap`. The
only `std::mutex` in the runtime's hot paths (`inbox_mtx`) is outside the
winner region by design.

---

## G. Failure-injection test design (C7)

### G.1 What the design requires

Authority §7 obligation 4: "an injected/controlled lock failure terminates
rather than returns into an authoritative transition." §C7 of the review
brief lists five candidate mechanisms and forbids pseudo-tests (fake class,
signature-only `static_assert`, UB, assume-`std::mutex`-throws, in-process
return-from-terminate).

### G.2 The five options, scored

| Option | Production hot-path cost | Death-test feasibility | Verdict |
|---|---|---|---|
| 1. Template the underlying mutex | Non-zero: turns `Mutex` into `Mutex<Backend>`, breaks the `SLUICE_CAPABILITY` spelling, breaks `std::unique_lock<Mutex>` / `cv.wait_until` at `:285`. | Good. | **Rejected** — too invasive. |
| 2. Test-only backend type | A second type shares the policy but not the identity; tests a *different* class than the production `Mutex`. Forbidden by the brief. | — | **Rejected**. |
| 3. Link seam | Production calls `::lock` resolved by the linker; test interposes a throwing `__wrap_lock`. Fragile across platforms; `std::mutex::lock` is a library inline/mangled symbol, not a C ABI hook. | Poor on Windows; weak on Linux. | **Rejected**. |
| 4. Death-test child process | Fork/exec a child whose `Mutex` is constructed over a throwing backend, assert the child is killed by `std::terminate`. The production `Mutex` shape is what runs. | **Good** — the standard, portable death-test idiom. | **Selected**, combined with option 5 as the seam. |
| 5. Test-only *injection backend inside the production `Mutex` via a macro* | `Mutex`'s underlying becomes `std::mutex` in production and a throwing shim under `SLUICE_ASYNC_INTERNAL_TESTING`. Mirrors the *exact* discipline the repo already uses for `AsyncTestAccess` (`scheduler.hpp:1091-1126`), compiled only into `sluice_async_internal_testing`. **Zero** production-binary cost (the macro is undefined in the production `sluice_async` target — `xmake.lua:32-44` vs `:70-86`). | Good. | **Selected seam.** |

### G.3 Concrete seam + test (binding proposal for the implementation task)

```cpp
// include/sluice/async/mutex.hpp (production shape unchanged textually
// except for the noexcept + catch body):
class SLUICE_CAPABILITY("mutex") Mutex {
public:
    void lock() noexcept SLUICE_ACQUIRE() {
        try { sluice_async_mutex_impl_lock(impl_); }
        catch (...) { sluice_async_fail_fast("Mutex::lock"); }
    }
    // ... try_lock, unlock analogous
private:
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    test_throwing_mutex impl_;   // test-only: throws on injected fault
#else
    std::mutex impl_;
#endif
};
```

* In the production target (`xmake.lua:32`), `SLUICE_ASYNC_INTERNAL_TESTING`
  is **undefined**, `impl_` is `std::mutex`, and
  `sluice_async_mutex_impl_lock` resolves to a `constexpr inline` one-liner
  that calls `m.lock()`. The compiler optimizes the `try`/`catch` away to a
  direct call (verified at `-Og`/`-O2` for analogous wrappers).
* In the internal-testing target (`xmake.lua:70`), the macro is defined, the
  `test_throwing_mutex` shim honors an injected fault counter, and the
  death-test forks a child that arms the fault and calls `Mutex::lock` from
  a Scheduler path. The parent asserts the child was killed by
  `std::terminate` (the harness already excludes destruction-contract death
  tests for lack of a harness — `tests/e12_async_condition_test.cpp:1338-1342`;
  the same exclusion shape applies until the harness gains `EXPECT_DEATH`).

### G.4 Coverage required

The implementation task's failure-injection suite must cover, at minimum:

* `lock()` throwing → terminates (not returns into the Scheduler).
* `try_lock()` throwing → terminates (not converted to `false`).
* `condition_variable_any` reacquire path: a throw from the inner
  `Mutex::lock` during `wake_cv_.wait_until`'s relock → terminates
  (overlaps with cppreference's "calls `std::terminate`" postcondition, but
  must be demonstrated on the *production* entry, not assumed).
* A POSIX and a Windows run (or an explicitly recorded platform gap with
  justification).

### G.5 Why this is P0, not P1

The authority's §7 obligation 4 is a **hard** gate ("`an injected/controlled
lock failure terminates`"). The design asserts it is achievable but does not
specify the seam. The brief explicitly forbids accepting the design without
a realizable test: "if [a real production-boundary test] cannot be
constructed, the design may not `PASS`." This review is satisfied that it
*can* be constructed (§G.3), so the design passes; but the implementation
task is **not** authorized until the seam + death-test land.

---

## H. Counterexample disposition

Each counterexample is marked per the brief's vocabulary.

| # | Counterexample | Disposition | Evidence |
|---|---|---|---|
| 1 | Winner CAS succeeds, then `lock()` throws and returns to user code | **BLOCKED BY TYPE STRUCTURE** | After Candidate A, `Mutex::lock` is `noexcept`; a throw cannot escape — `catch (...)` calls `std::terminate` at `mutex.hpp` before any return. cppreference confirms `condition_variable_any::wait` itself calls `std::terminate` if the relock postcondition cannot be met, so the standard *enforces* this independently. |
| 2 | `try_lock()` exception is wrongly converted to `false` | **BLOCKED BY TYPE STRUCTURE** | The proposed `try_lock` catches and calls `std::terminate`; it never reaches the `return false` path on exception. |
| 3 | Queue winner publishes halfway, then a *new* lock acquisition fails | **BLOCKED BY CALL GRAPH** | Every acquisition inside the winner `CommitGap` is `Mutex`-typed (§F); each independently terminates on failure. A failure at step N+1 does not "unpublish" step N — but it does terminate the process, so there is no observable half-published state. The design's "no recoverable exception" empty set (`e12-queue-scheduler-integration.md:823`) is the invariant. |
| 4 | `condition_variable_any` reacquire throws | **NOT BLOCKED — BY DESIGN, IT TERMINATES** | This is the *intended* behavior; cppreference mandates it. Verified the path compiles + runs (probe, §C3). |
| 5 | Downstream caller depends on catching `std::system_error` | **BLOCKED BY TEST (negative)** | `rg "system_error"` over `include/ src/ tests/` returns zero matches. No such caller exists in-tree. (A *future* external downstream might exist; that is the P1 API-contract item — §I.) |
| 6 | Queue ticket path secretly touches `inbox_mtx` | **BLOCKED BY CALL GRAPH** | `inbox_mtx` pop is post-publication transport, outside the `CommitGap` (§F.3). The winner-region ticket append is under G alone. |
| 7 | Callback/control mutex has a different exception policy | **NOT BLOCKED — IT IS THE SAME TYPE** | `SchedulerWakeHandle::Control::mtx` is a `sluice::async::Mutex` (`scheduler.cpp:75`); under Candidate A it gets the same policy. There is no separate "control mutex" type. |
| 8 | TSA accepts annotations but runtime still has a throwing edge | **BLOCKED BY TYPE STRUCTURE** | `noexcept` is a runtime contract, not just a static annotation; a throw through a `noexcept` function calls `std::terminate` regardless of TSA. The explicit `catch (...)` is belt-and-suspenders. |
| 9 | Death test only tests a fake, not the production entry | **BLOCKED BY TEST (gated on implementation)** | The seam in §G.3 keeps the production entry (`Mutex::lock`) as the unit under test; only the *underlying* is swapped under the internal-testing macro. P0 corrective requires this be demonstrated. |
| 10 | Fail-fast helper itself allocates/locks/fails | **BLOCKED BY TYPE STRUCTURE (with P2 corrective)** | A bare `std::terminate()` does not allocate or lock. If the implementation introduces a `sluice_async_fail_fast` helper, it must be `[[noreturn]] noexcept` and allocation-free; this review recommends the helper (P2) precisely to centralize that invariant. |
| 11 | Modified `Mutex` diverges from a precompiled object's contract | **BLOCKED BY TYPE STRUCTURE** | `Mutex` has no out-of-line member function (everything is inline in the header); every TU recompiles against the new header. There is no "old object file" to disagree. Itanium mangling does not encode `noexcept` (verified — symbols unchanged). |
| 12 | Follow-up reconciliation grabs another role mutex before winner publication | **BLOCKED BY CALL GRAPH** | Per the design (`e12-queue-scheduler-integration.md:866`), reconciliation follows `make_runnable -> ticket append -> follow-up reconciliation`, all after the winner is committed. The "at most one role lock" rule (`§11`) holds throughout. A second role lock is forbidden; under G+S, a violation would be a lock-order bug, not an exception edge — and `noexcept` does not mask it. |
| 13 | `unlock()` precondition violation is misclassified as a recoverable error | **NOT BLOCKED — INTENTIONALLY FATAL** | `std::mutex::unlock` has a precondition (caller owns); violation is UB, not an exception. The authority §3 correctly states this "remains a program invariant failure, not a recoverable error." Candidate A does not introduce recovery; it preserves the (already fatal) precondition. |
| 14 | A replaced `terminate_handler` returns | **BLOCKED BY TYPE STRUCTURE (standards)** | Per `[support.exception]`, a `terminate_handler` that returns (for the no-throwing-exception case) calls `std::abort`. Returning from a terminate handler is UB only for the `noexcept`-violation case in older standards; C++20 guarantees `std::abort` if the (replaced) handler returns and the cause is a noexcept violation. Even pre-C++20, returning leads directly to `std::abort`. Either way, control does not return to the Scheduler. |
| 15 | Candidate B (new `FailFastMutex`) is actually safer, but the doc did not compare fairly | **NOT BLOCKED — COMPARED HERE (§D)** | This review performs the full A/B/C comparison. B is real but loses on: unused throwing policy, big-bang migration to unblock B1, dual-`LockGuard`/`unique_lock` machinery, and zero behavioral gain over A. |

All 15 are blocked or justifiably excluded. No counterexample relies on a
debug assertion as its *only* block.

---

## I. Required correctives

### P0 — Failure-injection seam + death-test (blocking implementation)

The design's §7 obligation 4 has no concrete seam. Before any production
change lands, the implementation task must:

1. Add the test-only-backend seam described in §G.3 (gated on
   `SLUICE_ASYNC_INTERNAL_TESTING`, zero production-binary cost).
2. Land a death-test child-process suite covering `lock`, `try_lock`, and
   the `condition_variable_any` reacquire path (§G.4).
3. Explicitly record platform coverage (POSIX child + Windows, or a recorded
   gap).

Until then the design is correct but its central testability claim is
unrealized.

### P1 — Public API contract update

`Mutex` is an installed header (`include/sluice/async/mutex.hpp`). The
throwing→terminate contract change must be recorded:

1. Update `docs/api-reference.md` and `docs/api-reference-zh.md` to state
   that `sluice::async::Mutex::lock/try_lock/unlock` are `noexcept` and
   terminate on underlying acquisition failure (rather than throwing
   `std::system_error`).
2. Add a `changelog` entry under an appropriate "async substrate" heading
   (`docs/changelog.md`).
3. Note the source-level function-type change (the `noexcept` is part of the
   function type) for any downstream that takes `&Mutex::lock`; no in-repo
   code does.

### P2 — Single named fail-fast helper

Replace the bare `std::terminate()` calls in the three `Mutex` bodies with a
single `[[noreturn]] void sluice_async_fail_fast(const char*) noexcept` that:

* performs no allocation, no locking, no I/O on the winner path;
* optionally emits a fixed bounded message in *debug* builds only;
* calls `std::terminate` (or `std::abort`).

This centralizes the "fail-fast helper must not re-fail" invariant (counter-
example #10) at one auditable point. Not blocking.

### P2 — Note the explicit-`catch` redundancy in the header comment

The `try { ... } catch (...) { std::terminate(); }` is documentary — a bare
`noexcept { impl_.lock(); }` already terminates. The header comment should
say so plainly, so future maintainers do not "simplify" by removing the
`noexcept` thinking the `catch` is the real mechanism.

---

## J. Production implementation authorization prerequisites

Even with this review's PASS, the implementation task must satisfy:

1. **B1 (this authority)** — fully realized, *including*:
   * production change to `mutex.hpp` (Candidate A);
   * compile + TSA on `sluice_async` + `sluice_async_internal_testing` (this
     review verified it compiles — §C3 — but the implementation must land it);
   * the **failure-injection death-test** (P0 above);
   * **ASan/UBSan/TSan green** on the full async suite (this review ran the
     unsanitized debug/Clang/TSA build only — §C9; the implementation must
     run the sanitizer matrix);
   * an **independent implementation review** (this document is an
     *independent design review*; per the brief, "claiming independent review
     while authorizing production implementation" is forbidden — a separate
     implementation review is required).
2. **B2** — Corrective-2 independent adversarial review of the Queue (11
   topics, 33 counterexamples). *Separate required task; not closed by this
   review.*
3. **B3** — `E12-CONDITION-T25-MIGRATION-REACQUIRE-HANG-AUDIT-1` closed.
   *Separate required task.* (In this review's run, `e12_async_condition_test`
   passed, including the T25 case — but the authority
   `docs/e12-queue-implementation-authorization.md:247-296` documents the
   hang as nondeterministic coordinator spin; one green run does not close
   the audit.)
4. **B4** — Queue TLA+ formal model. *Separate required task; no `.tla`
   exists.*

This design review unblocks **one input** to B1 (the Mutex substrate
design). It does **not** unblock B1 as a whole, and it does not unblock B2,
B3, or B4. The Queue production implementation remains blocked at Phase 0.

---

## K. Repository state

```text
branch:           e12-e-queue-production-impl
HEAD:             eb8d974 (unchanged from review start)
files changed:    none in include/, src/, tests/, or xmake.lua
working tree:     this file is the only addition (docs/reviews/)
untracked files:  tests/test_t3_simple.cpp  (pre-existing, unrelated)
                  tla2tools.jar             (pre-existing, unrelated)
pushed:           no
```

### Verification of "no production change"

A scratch probe was used in two forms, both outside the durable working tree:

1. **Repo-external** (`/tmp/nothrow_probe/`): standalone TUs compiled against
   the production header to verify the `noexcept`+`catch` shape interoperates
   with `std::unique_lock<Mutex>`, `std::lock_guard<Mutex>`,
   `std::condition_variable_any`, and `sluice::async::LockGuard`, and to
   confirm the `noexcept` function-type and Itanium-mangling facts (§C3, §C8).
2. **Temporary in-tree overlay**: the proposed `Mutex` change was applied to
   `include/sluice/async/mutex.hpp`, the ten async test targets were built
   and run green (debug/Clang/TSA), and the header was then restored from
   the saved original. `git diff --stat include/sluice/async/mutex.hpp` is
   empty after restoration; `git status --short` shows only the two
   pre-existing untracked files plus this review document.

No commit, push, merge, or PR was created.
