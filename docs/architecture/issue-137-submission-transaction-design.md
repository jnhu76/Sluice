# Issue #137 Design — Centralizing the Explicit-I/O Submission Transaction

```text
STATUS: PROPOSED — AWAITING INDEPENDENT REVIEW
IMPLEMENTATION: STILL BLOCKED (issue #137 gate; only an accepted review lifts it)
```

This document is the design deliverable issue #137 demands (transaction
anatomy, authority map, ≥3 compared candidates, failure matrix, adversarial
audit, evaluation criteria, migration plan). It changes no production code.
Baseline: master 2026-08-20. The per-request narrative and the as-built
authority table it builds on live in
[async-request-lifecycle.md](async-request-lifecycle.md) (#139).

---

## 1. Transaction anatomy (STEP A) — what one submission is today

ADR Decision 5 defines the five-stage transaction
(`docs/adr/ADR-explicit-io-request-contract.md`). As built, one submission
consists of:

| # | Step | Arena-side API | Backend-side work |
|---|---|---|---|
| 0 | Stage-0 gate | — | uring only: ring availability, poison (`fatal_error_`), local `admission_closed_`, in-lock |
| 1 | reserve | `RequestArena::reserve` | admission discipline acquisition (none / `admission_mtx_` / `dispatch_mtx_`) |
| 1.5 | descriptor validation | — | `validate_op` (ThreadPool: `SSIZE_MAX` bound; Uring: `UINT_MAX` native bound; Fake/Sync: deferred, DIV-14) |
| 2 | prepare | `RequestArena::prepare` | prepared-op scratch write (`PreparedBlockingOp` / `PreparedUringOp`) |
| 2.5 | publication binding | `install_publication_binding` | publish-thunk selection (size vs void) |
| 3a | Completion claim | `begin_binding` (idle→binding CAS) | — |
| 3b | commit | `RequestArena::commit` (→ pending, pin, accounting, borrow) | — |
| — | deterministic pause | — | commit-vs-LP test seam (C2e/DIV) |
| 3c | accept LP | `install_binding` + `commit_binding` (binding→outstanding) | admission lock released after |
| 4 | enqueue | `RequestArena::enqueue` (or Scheme-B no-op) | backend dispatch linkage: nothing (Sync/Fake) / `work_mtx_` + ring + cv (ThreadPool) / `dispatch_mtx_` + ring + inline SQE drain (Uring) |
| 5 | dispatch | `mark_running` (execution side) | worker / kernel |

The **correctness-critical transaction** is stages 0–3c plus the rollback
ladder — the part that must be atomic ("no half-submitted request") and that
is today copy-pasted **eight times** (2 templates × 4 backends:
`sync_backend.hpp`, `fake_backend.hpp`, `threadpool_backend.cpp`,
`uring_backend.cpp`). Stage 4–5 is **execution ownership** and differs
fundamentally per backend; stage-4 enqueue is already noexcept/no-allocation
by contract, and its linkage mechanism is ADR-sanctioned backend property.

### 1.1 The rollback ladder (identical 8×, the drift surface)

```text
reserve fail          -> return error                      (no slot held)
validate fail         -> rollback_reserved_or_prepared; return error
prepare fail          -> rollback_reserved_or_prepared; return error
binding-install fail  -> rollback_reserved_or_prepared; return error
begin_binding loser   -> rollback_reserved_or_prepared; return error   (own slot only)
commit fail           -> rollback_binding_before_accept(c) FIRST,
                         then rollback_reserved_or_prepared; return error
— after commit_binding: NO failure representation exists (pause/enqueue are
  noexcept; a post-commit backend failure becomes a terminal via the arena,
  never a submit rejection) —
```

The ladder itself has NOT drifted today (the DIV C2b–C2e mutation matrix
constrains the copies), but every other dimension has: admission lock,
Stage-0 checks, validation bounds, scratch shape, pause seams, enqueue
scoping, stats tally policy (see the drift table in
[async-request-lifecycle.md §5](async-request-lifecycle.md)).

---

## 2. Current authority map (STEP B) — who owns what in a submission

| Question | Current owner | Split? |
|---|---|---|
| Who decides accepted? | Backend ladder steps 1–3c; the LP is `commit_binding` | the DECISION rule is the ADR's, but its *execution* is 8 copies |
| Accounting inc/dec | Arena (`commit` / reap / release) | NOT split — already single |
| Rollback obligation | Whoever holds the slot mid-ladder (each backend copy) | SPLIT 8× — the core defect |
| Backend ownership transfer | Stage 4 enqueue / dispatch linkage | deliberately per-backend (correct) |
| Last rollback-eligible point | `commit_binding` (LP) | rule single, copies 8 |
| After which point only completion can terminate | same LP | same |
| How errors cross the boundary | `Result<void>` from submit; terminal results via arena | single |
| shutdown vs submit final ruling | admission check at `reserve` (arena) + Uring Stage-0 local check under `dispatch_mtx_` | SPLIT by design (poison precedence is Uring authority, D4-M5) |

**Correctness transaction logic** (the ladder) is split and must be
centralized. **Backend-specific execution** (stage 4–5, locks, scratch,
poison), **wake/routing authority** (Scheduler; signal_ready_progress), and
**lifecycle/terminal authority** (arena record_terminal/cancel/reap) must
NOT be absorbed — absorbing any of them is a design failure (issue
prohibition; AGENTS.md §4).

---

## 3. Candidate designs (STEP C)

### Option A — `RequestArena::Submission` RAII transaction object

`auto tx = arena.begin_submission(...); tx.prepare(...); tx.bind(...); tx.commit(...); tx.finish();`
with destructor-driven pre-commit rollback.

- Single source of truth: yes (inside the arena).
- Exception safety: good in principle, but the repository's error discipline
  is `Result`-based — there are no exceptions on this path today. The RAII
  destructor covers exceptions that cannot happen while the `Result` ladder
  still needs explicit code, and the **disarm point is exactly the LP**: a
  tx destroyed between `commit` and `commit_binding` must roll back the
  binding; one destroyed after must NOT. Encoding "the destructor changes
  meaning at the LP" is the classic misuse magnet.
- Rollback completeness: yes, but double-execution risk (explicit finish +
  destructor) needs a state flag inside the tx — an extra mini-state-machine
  shadowing the slot state.
- Backend independence: hooks for admission gate/validation/scratch again.
- Authority impact: grows the arena's public surface with an orchestration
  type; submission orchestration is backend-side per AGENTS.md §4 — the
  arena owns slot states, not the submit story.
- Verdict: **rejected** — RAII buys nothing for a no-throw straight-line
  path and adds an LP-disarm subtlety plus a parallel mini-state-machine.

### Option B — `detail::SubmissionTransaction` coordinator layer

An object/function between backend and arena+Completion owning the
orchestration, with backend hooks.

- Single source of truth: yes.
- Authority impact: creates a FIFTH authority domain (slot-lifecycle /
  backend-progress / Completion / Scheduler / **coordinator**). AGENTS.md
  §4.1 forbids slot-lifecycle authority leaking to a parallel orchestrator;
  the coordinator inevitably accumulates queue/wake knowledge (its hooks
  must know when enqueue happens) — attack H territory.
- Hook count historically grows: "prepare→submit→commit/abort" becomes 6+
  hooks with pause seams, stats, poison.
- Verdict: **rejected** — authority regression disguised as dedup.

### Option C — policy/template adapter: `submit_transaction(arena, c, op, policy)`

A single free function template in a detail header owns the LADDER ONLY;
every genuinely backend-specific concern is a named hook on a per-backend
static policy; the backend calls the function inside its own admission
discipline; stage-4 enqueue stays in backend code after the call returns.

- If the policy surface approaches the protocol size, the design fails
  (issue's own criterion). The policy surface below is 12 production-facing
  methods + 1 test-only injection hook — far below the ~8×60-line protocol
  it replaces, and mechanically countable.
- Verdict: **recommended**, with the strict policy interface below.

### Option D — status quo + mechanical drift probe only

Keep 8 copies; add a gate that diffs the ladder copies textually.

- Zero risk, zero improvement: the authority remains split; a probe detects
  drift only after it exists (and pause-seam naming differences already
  defeat textual diffing today).
- Verdict: **rejected** — does not fix the defect (replicated transaction
  authority), only instruments it.

---

## 4. Recommended design (Option C, strict form)

```cpp
// Schematic (structure is near-literal; exact types/names decided at S1).
// Lives in an installed detail header (see §9 item 3).
namespace sluice::async::detail {

// One ladder. The backend holds its admission discipline AROUND this call;
// this function acquires no backend/admission/wake lock directly (arena
// leaf mutex via RequestArena calls is unchanged), performs no wake,
// allocates nothing.
// noexcept boundary: any accidental throw inside — in particular in the
// 3c→return window — terminates (fail-fast), which is exactly the intended
// response to a violated no-throw-after-acceptance contract.
template <class Policy>
Result<void> submit_transaction(RequestArena& arena,
                                 typename Policy::CompletionRef c,
                                 const typename Policy::Op& op,
                                 Policy& policy) noexcept {
    if (auto pre = policy.stage0_precheck(); !pre.has_value()) return pre;   // 0
    auto h = arena.reserve();                                                // 1
    if (!h.has_value()) return unexpected(h.error());
    if (auto v = policy.validate(op); !v.has_value()) {                      // 1.5
        arena.rollback_reserved_or_prepared(h.value());
        return v;
    }
    if (auto p = arena.prepare(h.value(), policy.kind(op), policy.borrow(op));
        !p.has_value()) {
        arena.rollback_reserved_or_prepared(h.value());
        return p;
    }
    policy.write_scratch(h.value(), op);                       // 2   (noexcept)
    if (auto b = arena.install_publication_binding(h.value(), &c, policy.requested_bytes(op),
                                                   policy.publish_thunk());
        !b.has_value()) {
        arena.rollback_reserved_or_prepared(h.value());
        return b;
    }
    if (!policy.begin_binding(c)) {                            // 3a  (CAS; loser rolls back OWN slot)
        arena.rollback_reserved_or_prepared(h.value());
        return unexpected(IoError{.code = IoError::Code::invalid_state});
    }
    if (auto m = arena.commit(h.value())) {                    // 3b
        policy.rollback_binding(c);                            // binding→idle FIRST
        arena.rollback_reserved_or_prepared(h.value());
        return m;
    }
    policy.pause_before_commit_binding();                      //     deterministic seam (noexcept)
    policy.install_and_commit_binding(c, h);                   // 3c  = accept LP
    return {};                                                 // nothing may fail after 3c
}
}  // namespace
```

The backend's `submit_size`/`submit_void` shrink to: acquire admission
discipline → `submit_transaction(...)` → release → `enqueue_after_commit(h)`
(unchanged, backend-owned). The 4× duplicated submit-read/write/sync
forwarders, publish thunks, `resolve_identity_state`, waiter surface stay
per-backend in shape but shrink; forwarding thunks may additionally be
shared via the policy's template parameters — NOT required for acceptance.

### 4.1 The policy surface — 5 hooks + 6 accessors

Two different kinds of policy members, counted separately so the issue's
"surface ≈ protocol size ⇒ failure" criterion is evaluated honestly:

**Policy interface (mechanically countable; any addition needs re-review):**

*4 argument/data adapters (pure data the ladder reads):*
`kind`, `borrow`, `requested_bytes`, `publish_thunk`. Fixed by the
protocol; they carry no control flow and cannot drift into a hook.

*4 Completion-binding adapters (protected AsyncBackend statics):*
`begin_binding`, `install_binding`, `commit_binding`, `rollback_binding`.
Mirror one-to-one onto existing arena/Completion call arguments.

*4 backend-divergence hooks (control-flow decisions):*

| Hook | Sync | Fake | ThreadPool | Uring |
|---|---|---|---|---|
| `stage0_precheck` | trivial ok | trivial ok | trivial ok | ring + poison + local admission_closed, VERBATIM error |
| `validate` | ok (DIV-14 divergence stays explicit) | ok | `SSIZE_MAX` family | `UINT_MAX` native-length family |
| `write_scratch` | none | none | `PreparedBlockingOp` | `PreparedUringOp` (+ native length) |
| `pause_before_commit_binding` | none | `wait_submit_pause_` | `wait_before_commit_binding_pause_` | same + `admission_domain_held` |

*1 test-only injection hook (`SLUICE_ASYNC_INTERNAL_TESTING` only):*
`injected_precommit_stage_failure`.

Combined surface: 12 production-facing methods + 1 test-only injection
hook against the ~8×60-line protocol it replaces.

**Explicitly NOT parameterized (stays backend code):** admission lock
acquisition/release, Stage-4 enqueue and its linkage, `signal_ready_progress`
placement, cancel execution interlock, close_admission wake, stats tally
policy, `mark_running`/dispatch. The shared function contains zero wake
calls and no new lock-domain operations by construction (arena leaf mutex
via RequestArena calls is unchanged from pre-centralization behavior).

### 4.2 What each non-breakable invariant maps to

| Invariant (#137 list) | Mechanism under Option C |
|---|---|
| reserve before prepare / ladder order | one function; order is unreadable-wrong |
| capacity/admission error precedence | `stage0_precheck` (backend) precedes `reserve`; arena's own admission check at reserve — both unchanged |
| descriptor-validation order | fixed position 1.5 in the one ladder |
| Completion idle→binding single winner | unchanged CAS; loser path is the one ladder's rollback of its OWN handle |
| slot binding installation / commit LP / borrow start | arena calls unchanged (single authority already) |
| accepted_outstanding accounting | arena-only (unchanged) |
| rollback before acceptance / none after | structural: no `return` statement exists after 3c in the function |
| no allocation/throw after acceptance | 3c→return contains only noexcept policy calls; a throw there terminates through the `noexcept` boundary (fail-fast), never returns a rejection |
| enqueue pin semantics | arena `enqueue` unchanged; Scheme-B no-op unchanged |
| close_admission linearization | unchanged (arena reserve check + uring Stage-0) |
| backend execution ownership / generation safety / no Completion* identity | untouched by design scope |

---

## 5. Failure matrix (STEP D)

| Failure point | accepted? | accounting | backend owns? | rollback | externally observable result |
|---|---|---|---|---|---|
| arena capacity full (`reserve`) | no | unchanged | no | none needed | `would_block` (sync, submit return) |
| admission closed (arena check / uring Stage-0) | no | unchanged | no | none | `invalid_state` / poison error verbatim |
| malformed descriptor (1.5) | no | unchanged | no | ladder: slot rollback | `invalid_argument` |
| binding CAS loser (3a) | no | unchanged | no | ladder: own-slot rollback | `invalid_state` |
| commit failure (3b) | no | reversed in ladder | no | `rollback_binding_before_accept` THEN slot rollback | error |
| exception inside transaction | no (pre-LP) / never returns (post-LP) | unchanged | — | Result discipline; hooks and the function boundary `noexcept` — an accidental throw terminates (fail-fast), it can never produce a half-submitted request or a rejection-after-accept | process termination = invariant violation surfaced, by design |
| dispatch-ring overflow post-commit (today: ThreadPool fail-fast) | YES | committed | yes | **no rollback after LP** — capacity equation makes this unreachable (ring capacity == request capacity) | fail-fast = invariant violation, by design |
| uring SQE/submit failure post-commit | yes | committed | yes | none — terminal only after ownership proof (D2) | terminal `backend_error` via reap |
| shutdown race vs submit | resolved at 0/1 under admission discipline | — | — | ladder | whichever linearizes first; no half-state |
| cancellation race (pending) | yes | committed, decremented at reap | Scheme-B | enqueue no-op acknowledges pin | `canceled` terminal via reap |
| accepted then execution failure | yes | decremented at reap | released at backend_ready | n/a | real error terminal via reap |

**No half-submitted request:** every pre-LP row ends in the one ladder
(zero residue: Completion idle, no slot, no borrow, accounting unchanged);
every post-LP row is terminal-path only. Under Option C this is a property
of ONE function instead of eight audited copies — the matrix rows do not
change mechanically.

---

## 6. Adversarial audit (STEP E)

Issue Attacks 1–6, then the prompt's A–J (overlapping answers merged):

1. **New pre-commit failure stage appears.** The backend cannot insert an
   unchecked early-return between arena calls — it no longer owns the
   sequence. A new stage must become a policy hook at a fixed ladder
   position; rollback is the ladder's, single authority preserved.
2. **begin_binding CAS loser pollutes another slot.** Impossible and
   unchanged: the ladder rolls back the handle IT reserved; the winner's
   slot is never touched. Mutation probe C2b continues to pin this (one
   mutation site now covers all backends).
3. **close_admission vs submission race.** LP unchanged; admission locks
   are NOT parameters — Fake/ThreadPool keep `admission_mtx_`, Uring keeps
   `dispatch_mtx_` + Stage-0; Sync keeps external `access_mtx_`
   serialization. Nothing that should not be unified is unified.
4. **Uring Stage-0 already poisoned.** `stage0_precheck` is the FIRST ladder
   step and returns the poison error verbatim before `reserve`. The shared
   function never assumes a healthy backend — it cannot: health is a policy
   question.
5. **Admission-lock differences erased.** See 3 — locks live in backend code
   around the call; the shared function is lock-free by construction (leaf
   arena calls only); AGENTS.md §13.1 domains untouched.
6. **Fake/Sync shallow abstraction.** Their policies are explicit ~10-line
   structs of trivial hooks — visible, greppable divergence declarations
   (DIV-14 stays a recorded divergence), not 20 defaulted virtuals.
   A. accounting-then-submit-fail → ladder order (binding rollback before
   slot rollback) is now written once.
   B. partial ownership + exception → no exceptions on the path (Result
   discipline; `noexcept` boundary); pre-LP the ladder is total.
   C. shutdown vs submit → unchanged (attack 3).
   D. "submit failed but backend accepted" → structurally impossible: no
   rejection return exists after 3c in the one function. This is precisely
   the drift the design eliminates.
   E. double rollback → single straight-line ladder, no RAII second path
   (why Option A was rejected).
   F. completion before submit returns → legal today (reap on another
   thread); Scheme-B/terminal_noop path unchanged.
   G. authority lost between accepted and terminal → arena owns it
   throughout; unchanged.
   H. centralization copies Scheduler wake authority → the function contains
   zero wake calls; enqueue/wake placement stays backend code (hook budget
   §4.1).
   I. split-wait liveness broken → wait sources, interrupt bridge, 2ms
   policy untouched.
   J. inconsistent "accepted" definitions → accepted == `commit_binding` LP,
   now literally the same code for every backend; remaining sanctioned
   divergence (validation bounds) is explicit in `validate`.

---

## 7. Evaluation criteria (issue checklist)

| Criterion | Assessment |
|---|---|
| authority locality | ladder: one detail header; arena/Completion/Scheduler authorities untouched |
| exception safety | `noexcept` boundary; Result discipline preserved; no RAII disarm subtlety |
| rollback clarity | one ladder, one order, mechanically total |
| debuggability | one place to breakpoint; per-backend policies remain visible |
| backend-specific divergence visibility | policy structs declare every divergence in-tree |
| compile-time complexity | one template + 4 small policy structs; no CRTP, no virtuals |
| template/code-size cost | net negative (removes ~8×60-line copies) |
| testability | backend-conformance suites run unchanged per backend; C2e pause seams preserved as hooks |
| mutation testing (DIV C2b–C2e) | probes re-point at the shared ladder + each policy; coverage strictly improves (one mutation constrains all backends) |
| close_admission compatibility | unchanged (attacks 3/4) |
| RequestArena ownership preserved | arena API unchanged; no new public type |
| Completion ownership preserved | binding trio called via policy, semantics unchanged |
| Uring poison-path compatibility | Stage-0 hook first, verbatim error (attack 4) |

**Formal models (AGENTS.md §17):** the `request-arena` TLA+ suite models the
arena-side lifecycle (five-stage admission, Scheme-B, terminal-winner,
reap gating) and binds the backend files only for Layer-B EXTERNAL
obligations (WF(Enqueue) at `enqueue_after_commit`, WF(Reap) at progress
call sites). Option C changes NO arena transition, admission rule, queue
bound, or wake rule — no model edit is required. The ladder's own property
("the sequence of leaf calls admits no half-submitted request") becomes a
property of one C++ function and stays test/mutation-bound; a dedicated TLA+
model of the ladder would model the already-modeld leaf's caller — ceremony,
not coverage. Reviewer may overrule.

---

## 8. Migration plan (staged, each independently verifiable)

1. **S1 SyncBackend** (no locks, no validation — smallest policy). Gate:
   Sync backend-conformance suite + C2b isolation run green; diff shows the
   backend's ladder replaced by the shared call.
2. **S2 FakeAsyncBackend** (adds `admission_mtx_` scope + `wait_submit_pause_`
   hook). Gate: Fake conformance + C2e deterministic pause cases green.
3. **S3 ThreadPoolBackend** (adds validate + scratch + SubmitStage injection
   harness re-pointing). Gate: full ThreadPool conformance, scheme-B race
   suite, TSan race classes (§16.3) green.
4. **S4 UringAsyncBackend** (adds Stage-0 + native length + inline drain
   remains outside). Gate: stub AND real-liburing paths (§16.5), D2 noalloc
   suite, D1 poison tests green.

Each stage: DIV mutation probes re-pointed in the same change; external
backends (DIV-07/13) untouched. Before S1 begins, the §8 architecture
compliance gate (`design-compliance-gate.md`) must be completed with all
fields PENDING→PASS after execution (this document supplies the state
machine §1, the lock/atomic authority table §4.1/§6-3/5, the resource model
— no new resources, and the wake model — no wake changes).

---

## 9. Decision (STEP F)

**DESIGN READY — IMPLEMENTATION MAY PROCEED (subject to the issue's
independent-review gate).** Authority boundaries are explicit (§2, §4.1);
the transaction invariant is one straight-line rule (§4.2); rollback is
total and single-authority (§4, §5); wake authority is untouched by
construction; lifecycle/liveness unchanged; the implementation surface per
stage is small and covered by existing conformance suites plus re-pointed
mutation probes (§8).

What independent review should specifically confirm (the points a reviewer
could still reject):

1. the 12+1 policy surface is genuinely sufficient at S3/S4 (no hidden
   divergence — e.g. ThreadPool's `SubmitStage` failure-injection harness
   must be expressible as the test-only injection hook without widening
   the production interface);
2. the policy's `stage0_precheck` position (before `reserve`) preserves
   Uring's poison-vs-admission precedence exactly (D4-M5);
3. whether the shared function should live in an installed detail
   header (submit_transaction.hpp under include/sluice/async/detail/, like
   request_arena.hpp) or a non-installed src/async/ header —
   either is conformant; recommendation: installed detail, since it is
   production code referenced by four production backends.

Per the issue's own gate, this BLOCKED state is lifted only by that review —
not by this document.
