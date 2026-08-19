# Failure-Response Model (T1–T7)

Status: ACTIVE policy (post-audit remediation, issue #135 / finding C8, tracking
issue #144 for the historical backlog).

Authority chain for this document:

1. `AGENTS.md` §9.2 (binding rules for the `assert()` family).
2. This document (taxonomy and mechanical decision rules).
3. `scripts/gates/assert-hygiene.py` + `scripts/gates/assert-hygiene.allowlist`
   (changed-lines enforcement, wired into `scripts/gates/pre-push.sh` and CI).

This document does not change any Accepted ADR. It classifies failure responses
and names the mechanism each class permits, so that "which mechanism do I use
here" has a mechanical answer instead of habit or local precedent.

---

## 1. Problem this model solves

The repository historically mixes several failure mechanisms with overlapping
and sometimes contradictory roles:

- `Result<T>` / `IoError` propagation (the core error channel);
- synchronous admission rejections (`would_block`, `no_space`);
- the named fail-fast family in `include/sluice/async/detail/fail_fast.hpp`
  (`[[noreturn]] noexcept`, active in Debug **and** Release, death-tested);
- bare `assert()` (~155 sites in `include/` and `src/` at the time of writing);
- scattered `std::terminate` / `std::abort` calls;
- a few `IoError::invalid_state` typed fallbacks paired with Debug asserts.

The risk is not the count — it is that the choice of mechanism is not derivable
from the failure's nature. A bare `assert()` protecting a request-lifecycle
invariant is a Debug-only guard that vanishes in Release and in every downstream
build that defines `NDEBUG`; a `Result` returned for an impossible internal
state invites the caller to "handle" corruption. Two defects of this shape are
fixed in the remediation that introduced this document (the uring
`try_wait_token_for_test` / `wait_epoch_changed_for_test` asserts, which fire
when the ring has no wait source — an environment-availability condition, not a
programmer error — and the `select.hpp` `index()` Release path, which had a
Debug assert without its typed fallback).

## 2. Taxonomy

| Class | Nature | Who can cause it | Permitted mechanisms |
|---|---|---|---|
| T1 | Expected operation failure | environment | `Result<T>` / `IoError`, raw error fidelity |
| T2 | Admission / capacity refusal | bounded resource state | synchronous reportable rejection before acceptance |
| T3 | Caller contract violation (public API misuse) | caller | typed fallback where one exists; otherwise named fail-fast (Debug+Release) |
| T4 | Internal invariant violation | implementation bug | named fail-fast (Debug+Release), forensic context, never continue |
| T5 | Introspection / test-seam unavailability | environment or test sequence | typed observable result (`nullopt`, `false`, error `Result`) |
| T6 | Lifetime / shutdown contract violation | caller | named per-authority fail-fast (Debug+Release); never hidden cleanup |
| T7 | Kernel boundary condition | kernel | repository retry authority + raw errno preservation |

### T1 — Expected operation failure

The operation executed and the environment answered no: an I/O error, EOF, a
short transfer, a missing file. A correct caller can sensibly react at runtime
(translate, retry by policy, report to a user).

Rules:

- MUST surface as `Result<T>` carrying `IoError` with raw OS error information
  preserved (AGENTS.md §9).
- MUST NOT assert, terminate, or abort.
- Destructors and `[[noreturn]]` paths have no `Result` channel: a T1 that
  would otherwise be reported there is a design smell — the operation belongs
  on a path that can report it (AGENTS.md §9: destructors must not invent
  unreportable I/O success or hide unreportable I/O failure).

### T2 — Admission / capacity refusal

A bounded resource is full and the request has NOT been accepted (AGENTS.md
§12): request arena exhausted, dispatch queue full, kernel queue at depth.

Rules:

- MUST return a synchronous, reportable rejection (`would_block`, `no_space`)
  before acceptance, leaving the Completion idle and no borrow behind
  (AGENTS.md §10.2).
- MUST NOT terminate; capacity pressure is a designed operating condition.
- After commit/accept, the same physical condition is no longer T2 — it must
  not retroactively reject (post-accept liveness is a design obligation, not a
  caller error).

### T3 — Caller contract violation (public API misuse)

The caller broke a documented precondition of a public API: awaiting an idle
Completion, double-submitting into an outstanding Completion, resetting a
bound Completion, driving a context from the wrong thread domain. There is no
valid runtime recovery; the fix is at the call site.

Rules:

- Preferred shape: a typed, documented fallback when a return channel exists —
  the Completion L9 pattern: Debug asserts as a tripwire, Release returns
  `IoError::invalid_state` (or the documented typed result). Release behavior
  must be correct WITHOUT the assert.
- When no return channel exists (destructors, `[[noreturn]]` internal paths):
  a NAMED fail-fast from `detail::fail_fast.hpp`, active in Debug AND Release,
  death-tested. Naming is per authority so the failure site is attributable
  (e.g. `completion_authority_fail_fast`, `group_lifetime_fail_fast`).
- A bare `assert()` alone is NEVER a complete T3 response: it vanishes under
  downstream `NDEBUG`.

### T4 — Internal invariant violation

The implementation observed a state its own protocol proves impossible:
terminal-winner arbitration corrupted, a current-generation key failing
validation, a ready-ring structural inconsistency, a double release. This is an
implementation bug by definition; no caller input can legitimately produce it.

Rules:

- MUST NOT continue: no fabricated result, no silent skip, no "recovery".
- MUST be a named fail-fast (Debug+Release) with the authority named in the
  function name; see the `request_arena_*_fail_fast` family for the pattern.
- MUST NOT be expressed as a public `IoError`: there is nothing a caller can
  correctly do with "the arena state machine is corrupt".

### T5 — Introspection / test-seam unavailability

A query or `SLUICE_ASYNC_INTERNAL_TESTING` seam is asked to observe something
the current environment or test sequence cannot provide: the ring was never
constructed (no wait source), the staged operation is not (yet) visible, the
clock has no armed timer. This is an INPUT to the test, not a programmer error.

Rules:

- MUST return a typed observable result the test asserts on:
  `std::optional` (`std::nullopt`), `bool`, or an error `Result`.
- MUST NOT assert: internal-testing binaries are also built in Release
  configurations, and a stub / resource-unavailable environment is legitimate
  (a stub-only build must never present itself as real-path evidence, but it
  must also not die when a seam is asked for a resource the stub does not
  have).
- Blocking wait helpers (e.g. `wait_for_*` seams) fail closed through their
  bounded protocol (deadline / generation handshake), not through termination.

### T6 — Lifetime / shutdown contract violation

Ownership ends in a state the contract forbids: destroying an `AsyncIoContext`
with outstanding Completions, destroying a backend with accepted work,
destroying a held async primitive, destroying a Scheduler with live waiters
(AGENTS.md §14). Quiescent teardown MUST succeed silently; the violation is the
non-quiescent state, not the destruction itself.

Rules:

- Named per-authority fail-fast (Debug+Release):
  `async_context_outstanding_fail_fast`, `threadpool_non_quiescent_destruction_fail_fast`,
  `uring_non_quiescent_destruction_fail_fast`, `scheduler_wait_registry_nonempty_fail_fast`,
  `request_arena_destruction_fail_fast`.
- NEVER hidden cleanup: no implicit cancel-all, drain, unlock, or synthesized
  results on the destruction path.
- The async synchronization primitives (AsyncMutex / AsyncRwLock / Event /
  Condition / WaitQueue) are being unified onto this rule by a dedicated ADR
  and PR (tracked in #135 phase 6); until then their destructor asserts are
  grandfathered inventory (issue #144), not precedent.

### T7 — Kernel boundary condition

Not a "failure" of the model — normalization rules at the syscall edge:
`EINTR` (retry through the repository retry authority; never duplicate ad-hoc
retry loops), partial reads/writes (`read_some`/`write_some` may be short;
exact/all helpers loop), errno fidelity (preserve the raw error in `IoError`).

Rules:

- `EINTR` on a retryable blocking syscall is retried by the designated helper;
  a caller-visible `interrupted` surfaces only where the contract says so.
- Short transfers are looped by callers/coordinators, never treated as errors
  by the primitive.
- Zero progress on a non-empty write is an invalid backend state (T4), not a
  T7 condition and not an infinite-retry loop (AGENTS.md §9).
- Tracked defects of this class live in #142 (EINTR-001) and #143 (ERR-001);
  #141 (FILEOP-001) is a T1 fidelity defect.

## 3. Mechanical decision rules

Apply in order; the first match wins.

1. Is the condition kernel-boundary noise (`EINTR`, partial transfer, signal
   restart)? → **T7**: normalize (retry / loop / preserve errno).
2. Did the environment execute (or refuse after accepting) real work with a
   caller-relevant outcome? → **T1**: `Result` / `IoError`.
3. Is a bounded resource refusing BEFORE acceptance? → **T2**: synchronous
   rejection, no side effects.
4. Can the condition occur when every caller is CORRECT (environment or test
   sequence alone produces it)? → **T5** if it is an introspection/seam query:
   typed `nullopt`/`false`/error; → **T1** otherwise.
5. Did the CALLER violate a documented public precondition? → **T3**: typed
   fallback if a channel exists, else named fail-fast. Bare `assert()` never
   suffices.
6. Is ownership ending in a forbidden state? → **T6**: named per-authority
   fail-fast, no hidden cleanup.
7. Otherwise the implementation observed an impossible state → **T4**: named
   fail-fast, never continue, never fabricate.

Cross-checks that catch misclassification:

- If you cannot name WHO must fix the condition (caller? implementer? nobody —
  it is an input), the classification is wrong.
- If a Release build would behave differently from Debug in a way a test can
  observe, an `assert()` is doing enforcement work it must not do.
- If a death test cannot observe the failure (it only fires under a Debug
  assert), the mechanism is not enforceable and the classification is wrong.

## 4. The `assert()` family and `NDEBUG`

- The Release build defines `NDEBUG`; a public header's `assert()` also
  compiles away for every downstream consumer that defines it. **`NDEBUG` is
  not semantic authority**: an invariant that matters only inside `assert()`
  is not enforced in Release and not enforced for consumers at all.
- `assert()` is a Debug-only DIAGNOSTIC. It is legal only as a redundant
  tripwire on top of a complete mechanism of classes T1–T7 — never as the
  mechanism itself.
- `static_assert` is compile-time, `NDEBUG`-independent, and unaffected by
  this policy.
- This policy restricts the `assert()` family only. It does not restrict
  ordinary freestanding headers (`<cstdint>`, `<cstddef>`, ...).

## 5. Allowlisted assert categories

A NEW `assert(`, `#include <cassert>`, or `#include <assert.h>` line in
`include/` or `src/` must be added to
`scripts/gates/assert-hygiene.allowlist` naming one of these categories:

1. **Completion L9 pattern** — Debug tripwire layered on a Release path that
   already returns the documented typed error; Release behavior is correct
   without the assert.
2. **Pure diagnostics** — the assert observes an invariant proven elsewhere
   and its failure changes no returned value or state transition.
3. **Internal-testing preconditions** — a precondition inside a
   `SLUICE_ASYNC_INTERNAL_TESTING`-guarded seam that the test author fully
   controls, where a violation means the test itself is broken (NOT an
   environment-availability condition, which is T5 and must be typed).

The allowlist entry format and enforcement are defined in the allowlist file
header comment.

## 6. Gate enforcement

- `scripts/gates/assert-hygiene.py --self-test` — plants each violation shape
  and requires every detector to fire (fail-closed against a broken checker).
- `scripts/gates/assert-hygiene.py [-- <git-diff-args>...]` — scans ADDED lines
  of the given diff (pushed ranges in hook mode; staged + working tree when
  invoked manually) in `include/` and `src/` for the forbidden patterns and
  fails on unallowlisted additions.
- Wired as a gate in `scripts/gates/pre-push.sh`, which CI re-runs via the
  "Repository mechanical gates" step. Fail-closed: no `|| true`, first failure
  wins, reproduction command printed.
- **Grandfathered**: pre-existing sites are not flagged (changed-lines only).
  The backlog of historical sites is tracked in issue #144; cleaning them is
  deliberate, reviewed work — never a drive-by bulk rewrite.

## 7. Historical inventory (context, not precedent)

At policy adoption (master `d7ee077`):

- ~155 bare `assert(` sites in `include/` + `src/` (excluding `static_assert`);
- named fail-fast family: ~25 entries in
  `include/sluice/async/detail/fail_fast.hpp`, all
  Debug+Release, death-tested;
- scattered `std::terminate` / `std::abort` sites outside the named family;
- one established Completion L9 typed-fallback pair.

Existing code is evidence of history, not automatic architectural precedent
(AGENTS.md §3): each grandfathered site is reclassified only through reviewed
change (issue #144 and the per-phase PRs of the #135 remediation).
