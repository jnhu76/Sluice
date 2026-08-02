# ADR: Completion Publication Authority

**Status:** Accepted
**Authority:** Corrective alignment with AC-5, AC-13, AC-14
**Scope:** Completion publication authority hardening (Explicit I/O Phase 1)
**Findings addressed:** P0-03, P1-03, partial P1-01
**Findings NOT resolved:** P0-02, P1-06, P1-07, P1-10

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
- `AsyncBackend` exposes **protected static** helpers (`try_claim`, `publish`)
  that derived backends use.
- No public type, token, or constructor allows ordinary code to obtain
  publication capability.
- A comment saying "backend-only" is NOT an authority boundary.
- `assert()` is NOT the sole protection; Release builds also detect invalid
  transitions via fail-fast (std::terminate through detail::fail_fast).

---

## 3. Public Backend Extension Point

`AsyncBackend` remains a public installed header extension point. Tests and
applications may subclass it.

Transitional decision (DIV-13 update):

- Public backend subclasses do NOT directly access Completion private state.
- They use the inherited protected `try_claim()` / `publish()` helpers.
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
publishing   — CAS won, result under construction (atomic transient)
```

The transient state is an implementation detail of the atomic publish sequence
and does NOT create a new public lifecycle burden.

---

## 5. State Transitions

```text
idle
  └─ backend try_claim (CAS) ─→ outstanding

outstanding
  └─ reap publish (CAS + release store) ─→ ready

ready
  └─ caller reset ─→ idle
```

### Forbidden transitions (fail-fast in Debug AND Release)

```text
idle → ready              (cannot skip outstanding)
ready → outstanding       (cannot re-claim without reset)
outstanding → idle        (cannot skip ready; reset on outstanding = violation)
publishing → reset        (cannot reset mid-publication)
double publish            (exactly-once invariant)
double claim              (exactly-once invariant)
```

### Reset semantics

`reset()` remains caller-accessible but is state-checked:

```text
reset from ready       → idle (success)
reset from idle        → fail-fast (contract violation)
reset from outstanding → fail-fast (contract violation)
```

### Destruction semantics

```text
destroy idle           → allowed
destroy ready          → allowed
destroy outstanding    → fail-fast (Debug AND Release)
destroy publishing     → fail-fast (Debug AND Release)
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

The old pattern (load → assert idle → store outstanding) is removed.

---

## 7. Publication

Terminal result publication satisfies:

```text
exactly once
single winner
release publication
result fully constructed before ready becomes observable
```

Implementation structure:

```cpp
void publish_from_reap(Result<T>&& result) noexcept {
    // Precondition: state is outstanding (checked, fail-fast)
    storage_.set(std::move(result));
    reap_seq_ = detail::next_reap_seq();
    state_.store(State::ready, std::memory_order::release);
}
```

Readers use matching acquire semantics (already present in `ready()` /
`result()`).

Publication failure (state != outstanding) is an internal contract violation
and triggers fail-fast. It cannot be silently ignored.

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
};
```

Derived backends use:

```cpp
// In submit_*:
if (!try_claim(c))
    return make_unexpected<void>(IoError{IoError::Code::invalid_state});

// In poll()/wait_one() reap:
publish(c, std::move(result));
```

---

## 10. Transactional Submit Gap (NOT resolved)

This ADR hardens publication authority. It does NOT resolve P0-02
(transactional admission). The sequence:

```text
try_claim(c)  →  container allocation  →  allocation failure
```

remains a known gap in production backends. This ADR:

- Preserves existing external semantics for this gap.
- Marks the gap with explicit TODO comments.
- Does NOT claim P0-02 is resolved.
- Does NOT expand into a full RequestSlot implementation.

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

1. **Negative-compile gate:** ordinary code cannot call publication mutators.
2. **Lifecycle test:** idle → claim → outstanding → publish → ready → reset → idle.
3. **Invalid reset death test:** reset idle/outstanding → fail-fast.
4. **Outstanding destruction death test:** destroy outstanding → fail-fast.
5. **Double-claim race (TSan):** two threads claim same Completion; exactly one wins.
6. **Double publication:** second publish → fail-fast.
7. **SyncBackend cancel conformance:** cancel defers to poll; exactly-once.
8. **Backend regression:** all existing backends compile and pass with new API.

---

## 13. Next Step

```text
feat(async): add bounded RequestKey / RequestSlot reference lifecycle
```

This follow-up resolves P0-02 (transactional admission), P1-06 (generation/ABA),
and P1-07 (identity-bearing reap) as a unified request lifecycle design.
