# ADR: Completion Publication Authority

**Status:** Accepted
**Authority:** Corrective alignment with AC-3, AC-5, AC-10, AC-13, AC-14
**Scope:** Completion publication authority hardening (Explicit I/O Phase 1)
**Findings addressed:** P0-03, P1-01, P1-03
**Findings NOT resolved:** P0-02, P1-06, P1-07, P1-10

**Proposed future partial supersession:** If
[ADR-explicit-io-request-contract](ADR-explicit-io-request-contract.md) is accepted, its private
`idle -> binding -> outstanding` protocol supersedes only the direct claim transition and
pre-accept rollback details in Sections 2.2, 5, 9, and 10 of this ADR. This ADR's private/protected
access boundary, backend-only authority, reap-only publication, fail-fast invalid transitions, and
idle/ready destruction permissions remain authoritative. While the request-contract ADR is
Proposed, the sections below continue to describe the accepted, implemented contract from PR #61.

---

## 1. Context

PR #60 established the architecture governance baseline. The audit identified
P0-03: `Completion<T>` exposes `mark_outstanding()` and `complete_with()` as
public methods, allowing any application code to forge publication transitions.
Additionally, P1-03: `SyncBackend::cancel()` calls `complete_with()` directly,
bypassing the designated reap authority (poll/wait_one).

This ADR defines the authority model and state machine corrections.

---

## 2. Authority Separation

### 2.1 Caller-accessible operations

Ordinary application code MAY:

```text
query idle/outstanding/ready state
read the terminal result (once ready)
reset: ready → idle (reuse)
```

### 2.2 Backend/system authority (structurally forbidden to callers)

Only code holding backend capability MAY:

```text
claim:    idle → outstanding
publish:  outstanding → ready (via reap path)
```

### 2.3 Enforcement mechanism

Authority is enforced by C++ access control:

- `mark_outstanding()` and `complete_with()` become **private** members of
  `Completion<T>`.
- `AsyncBackend` is granted `friend` access.
- `AsyncBackend` exposes **protected static** helpers (`try_claim`, `publish`,
  `rollback_claim_before_accept`) that derived backends use.
- Ordinary **non-backend** application code (a plain caller that does NOT
  subclass `AsyncBackend`) cannot obtain publication capability: the mutators
  are private, the helpers are protected, and no other type/token/constructor
  grants access.
- The threat model is therefore "accidental or casual publication forgery by
  callers is structurally impossible". It is NOT "malicious code that
  deliberately derives `AsyncBackend` cannot publish" — see §3: deriving
  `AsyncBackend` IS the sanctioned way to enter the backend-author role.
- A comment saying "backend-only" is NOT an authority boundary.
- `assert()` is NOT the sole protection; Release builds also detect invalid
  transitions via fail-fast (std::terminate through detail::fail_fast).

---

## 3. Public Backend Extension Point

`AsyncBackend` remains a public installed header extension point. Tests and
applications may subclass it.

**Trusted backend-author threat model.** Deriving `AsyncBackend` is the
sanctioned mechanism for obtaining publication capability. A derived backend
inherits the protected `try_claim()` / `publish()` /
`rollback_claim_before_accept()` helpers and, through them, can claim
Completions and publish terminal results. This is the deliberate injection
seam that decouples L1 from how completions are produced; it is NOT a
capability-isolation boundary against code that deliberately subclassed
`AsyncBackend`. The header documents this: a subclass is a trusted
backend-author, and misuse is handled by the backend-conformance contract, not
by pretending the capability is unobtainable.

Transitional decision (DIV-13 update):

- Public backend subclasses do NOT directly access Completion private state.
- They use the inherited protected `try_claim()` / `publish()` /
  `rollback_claim_before_accept()` helpers.
- They MUST follow the unified claim/publish contract.
- A backend conformance suite is a follow-up requirement.

This ADR does NOT internalize `AsyncBackend` or remove backend injection.

---

## 4. Public Completion State

Caller-observable semantic states remain:

```text
idle         — available for submission
outstanding  — accepted by a backend, awaiting terminal result
ready        — terminal result available via result()
```

Internal transient states (not exposed publicly):

```text
publishing   — CAS won, result under construction (publication transient)
resetting    — reset CAS won, prior result being cleared (caller-lifecycle transient)
```

`publishing` makes the winner's storage write exclusive. `resetting` prevents a
new claim from observing `idle` before prior-result cleanup (storage_ /
reap_seq_) is complete. Neither transient is a public lifecycle state; query
methods report both as not-idle/not-ready (`publishing` as outstanding,
`resetting` as not-outstanding).

### Value-type contract (Completion<T>)

`Completion<T>` is an asynchronous terminal-publication cell. Its stored value
type T MUST support the operations the reap/result path performs. These are
compile-enforced by `static_assert` on the template:

```text
is_nothrow_default_constructible_v<T>  (idle storage is value-initialized)
is_copy_constructible_v<T>             (result() returns the stored result by value)
is_nothrow_move_assignable_v<T>        (publish_from_reap assigns the value in)
is_nothrow_destructible_v<T>           (reset tears storage down, noexcept)
```

`result()` returns the stored result BY VALUE — it copies it out; it does NOT
move it out (the Completion keeps its copy until `reset()`). The copy trait is
deliberately NOT a nothrow trait: `result()` is not `noexcept`, so a throwing
copy propagates to the caller like any ordinary return. The `noexcept` reap/
reset path is covered by the default-construction, move-assignment, and
destruction traits.

`Completion<void>` carries no value, so these traits do not apply; its
bool/IoError publication path is noexcept by construction. A type that fails a
trait fails to instantiate `Completion<T>` at compile time (negative-compile
gate: `NEG_THROWING_COMPLETION_VALUE`): a throwing T must never escape the
`noexcept` reap/reset boundary via `std::terminate`, and a non-copyable T
cannot satisfy `result()`'s by-value return.

---

## 5. State Transitions

```text
idle
  └─ backend try_claim (CAS) ─→ outstanding

outstanding
  └─ reap publish (CAS → publishing; build result; release store) ─→ ready

ready
  └─ caller reset (CAS → resetting; clear result; release store) ─→ idle
```

### Forbidden transitions (fail-fast in Debug AND Release)

```text
idle → ready              (cannot skip outstanding)
ready → outstanding       (cannot re-claim without reset)
outstanding → idle        (caller reset skips ready; reset on outstanding = violation)
publishing → reset        (cannot reset mid-publication)
double publish            (exactly-once invariant)
double claim              (exactly-once invariant)
reset on resetting        (reset authority already held)
```

### Reset semantics

`reset()` remains caller-accessible but is state-checked, and routes the
`ready → idle` reuse through an internal `resetting` transient:

```text
reset from ready       → resetting → idle (success; cleanup completes before idle is published)
reset from idle        → no-op (idempotent; defensive first-iteration reset)
reset from outstanding → fail-fast (contract violation)
reset from publishing  → fail-fast (contract violation)
reset from resetting   → fail-fast (reset authority already held)
```

The `resetting` transient is structural, not advisory: reset CASes
`ready → resetting`, clears storage_/reap_seq_, then release-stores `idle`. A
new `try_claim` can only observe `idle` AFTER cleanup has happened, so a fresh
operation can never observe a half-cleaned prior result. `try_claim` itself does
NOT clear storage — cleanup belongs to the reset authority, and re-clearing on
claim would race a backend that has already published `outstanding`.

The `idle → no-op` is a deliberate caller-lifecycle decision, registered in
AC-13 (amended). `op_helpers` `one_step` / `sync_step` reset before their
first submit, when the Completion is already idle; fail-fast on idle would
break that legitimate reuse pattern. An earlier draft of this ADR treated idle
reset as a fail-fast; this ADR and the implementation choose the idempotent
no-op instead. It is NOT a death-test requirement.

Concurrency boundary: claim-vs-claim and publish-vs-publish are atomically
arbitrated (CAS single-winner). reset-vs-new-claim is structurally serialized
through the `resetting` transient. The caller MUST NOT race `result()` or
object destruction with reset/publication — those are caller-lifecycle errors.

### Destruction semantics

```text
destroy idle           → allowed
destroy ready          → allowed
destroy outstanding    → fail-fast (Debug AND Release)
destroy publishing     → fail-fast (Debug AND Release)
destroy resetting      → fail-fast (Debug AND Release)
```

The destructor does NOT attempt implicit cancel or drain.

---

## 6. Concurrent Claim

Two different `AsyncIoContext` instances or backends submitting the same
Completion concurrently:

```text
exactly one claim succeeds (CAS winner)
all other claims fail synchronously with invalid_state
```

Implementation: atomic compare-and-exchange on the state word.

```cpp
bool try_claim_for_backend() noexcept {
    State expected = State::idle;
    return state_.compare_exchange_strong(
        expected, State::outstanding,
        std::memory_order::acq_rel,
        std::memory_order::acquire);
}
```

The claim does NOT clear storage_/reap_seq_: cleanup belongs to the reset
authority (§5). By the time `idle` is observable, a previous reset has already
cleared the result, so re-clearing on claim would both duplicate that work and
race a backend that has already published `outstanding`. The old pattern
(load → assert idle → store outstanding) is removed.

---

## 7. Publication

Terminal result publication satisfies:

```text
exactly once
single winner
release publication
result fully constructed before ready becomes observable
```

The transient `publishing` state makes the winner's storage write exclusive:

```cpp
void publish_from_reap(Result<T>&& result) noexcept {
    // Single-winner CAS: outstanding → publishing. A concurrent publisher
    // (or a publish from idle/ready) fails the CAS and fail-fasts — it can
    // never race a half-built storage_/reap_seq_.
    State expected = State::outstanding;
    if (!state_.compare_exchange_strong(
            expected, State::publishing,
            std::memory_order::acq_rel,
            std::memory_order::acquire)) {
        detail::completion_authority_fail_fast();
    }
    storage_.set(std::move(result));   // noexcept: enforced by Completion<T> traits
    reap_seq_ = detail::next_reap_seq();
    state_.store(State::ready, std::memory_order::release);
}
```

`publish_from_reap` and `Storage::set` take `Result<T>&&` (not by value) to
avoid a redundant move-construction of the Result into the parameter; the
`noexcept` is justified by the `Completion<T>` value-type traits (§4).

Readers use matching acquire semantics (already present in `ready()` /
`result()`). Query methods report the transient `publishing` state as
`outstanding` (an op that is neither idle nor ready).

Publication failure (publish CAS `outstanding → publishing` loses) is an
internal contract violation and triggers fail-fast. It cannot be silently
ignored.

---

## 8. Reap Authority

Completion enters `ready` ONLY through the designated reap path:

```text
poll() / wait_one() → backend drain → publish helper
```

Backend workers, cancel handlers, and timer callbacks do NOT directly publish
Completions.

### SyncBackend cancel correction

Previous behavior (P1-03):

```text
cancel() → complete_with(canceled) directly
```

Corrected behavior:

```text
cancel() → mark entry as cancelled in backend tracking
         → entry remains in backend-owned list
         → poll()/wait_one() drains entry
         → reap path publishes canceled result
```

Requirements:

- cancel does NOT directly make the Completion ready
- outstanding count decrements only at reap time
- cancel is idempotent (multiple calls = one cancel intent)
- cancel after terminal (already reaped) is a no-op
- no double publication between poll and cancel

---

## 9. AsyncBackend Protected Helpers

```cpp
class AsyncBackend {
protected:
    template <class T>
    static bool try_claim(Completion<T>& c) noexcept {
        return c.try_claim_for_backend();
    }

    template <class T>
    static void publish(Completion<T>& c, Result<T>&& result) noexcept {
        c.publish_from_reap(std::move(result));
    }

    // Backend-only: undo a claim that won but was never accepted into backend
    // tracking (no register/enqueue/dispatch; submit has not returned success).
    // Call ONLY immediately after this backend's own successful try_claim(),
    // before any tracking step — e.g. io_uring SQE acquisition failure.
    template <class T>
    static void rollback_claim_before_accept(Completion<T>& c) noexcept {
        c.rollback_claim_before_accept();
    }
};
```

Derived backends use:

```cpp
// In submit_*: claim BEFORE any fallible acquisition step.
if (!try_claim(c))
    return make_unexpected<void>(IoError{IoError::Code::invalid_state});
io_uring_sqe* sqe = get_sqe();
if (sqe == nullptr) {
    rollback_claim_before_accept(c);  // claim was never accepted → undo it
    return backend_error;
}
// ... prep SQE, then register (track the op).

// In poll()/wait_one() reap:
publish(c, std::move(result));
```

---

## 10. Transactional Submit (partial)

Claim is transactional with respect to every fallible step that precedes
backend tracking:

```text
validate → try_claim (CAS) → get_sqe → [get_sqe failure] rollback_claim_before_accept
                          → prep → register_op
```

The null-SQE claim-rollback gap is CLOSED: the io_uring backend claims BEFORE
acquiring the SQE, so the loser of a concurrent claim never acquires an SQE,
and a failed (null) SQE acquisition after a won claim rolls the claim back
instead of leaving an outstanding, untracked Completion. This closes ONLY the
null-SQE branch — an SQE that has already been prepared still runs in the
background if a later step fails (the register_op allocation window below).

P0-02 REMAINS: `register_op` container allocation can throw after the SQE is
prepared, and ThreadPoolBackend's worker spawn can throw after claim (that
path converts spawn failure into a terminal error publication, so no orphaned
Completion). The full RequestSlot transactionalization is deferred to the
RequestSlot PR; this ADR does NOT claim P0-02 is resolved.

For simple test backends where safe local rollback is trivial, a local
transaction may be applied. Production backend systematic transactionalization
is deferred to the RequestSlot PR.

---

## 11. What This ADR Does NOT Change

```text
Scheduler waiter map
Runtime await API
Scheduler O(N) Completion scan
Batch reap_seq mechanism
ThreadPoolBackend persistent worker pool
backend→Scheduler wake
multi-waiter support
public submit_* return type
RequestKey / RequestSlot / generation
identity-bearing reap
```

These are follow-up work items. Discovering them during implementation only
generates a TODO comment, not code.

---

## 12. Test Strategy

1. **Negative-compile gate:** ordinary non-backend code cannot call the
   publication mutators or `reap_seq()` (including the rollback helper); AND a
   value type violating the `Completion<T>` noexcept value-type contract cannot
   instantiate `Completion<T>` (`NEG_THROWING_COMPLETION_VALUE`).
2. **Lifecycle test:** idle → claim → outstanding → publish → ready →
   (resetting) → reset → idle.
3. **Invalid reset death test:** reset outstanding → fail-fast; idle reset is
   an idempotent no-op (control case, NOT a death requirement).
4. **Outstanding destruction death test:** destroy outstanding/publishing/
   resetting → fail-fast.
5. **Concurrent claim race:** two threads claim the same Completion
   (barrier-released); exactly one wins.
6. **Concurrent publication death test:** two threads publish one outstanding
   Completion (barrier-released); the losing publisher fail-fasts, proving the
   single-winner CAS (no concurrent storage write).
7. **Double publication (sequential):** second publish on a ready Completion → fail-fast.
8. **SyncBackend cancel conformance:** cancel defers to poll; exactly-once.
9. **Backend regression:** all existing backends compile and pass with new API.

---

## 13. Next Step

```text
feat(async): add bounded RequestKey / RequestSlot reference lifecycle
```

This follow-up resolves P0-02 (transactional admission), P1-06 (generation/ABA),
and P1-07 (identity-bearing reap) as a unified request lifecycle design.
