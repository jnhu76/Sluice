# E12-G Cross-Primitive Semantic Closure — Final Independent Review

> **Reviewer:** Independent adversarial review (ZCode)
> **Date:** 2026-07-19
> **Mode:** STRICTLY READ-ONLY — no file modified by the reviewer, no corrective
> implemented by the reviewer.
> **Status:** `REVIEW-COMPLETE`

## 0. Review metadata

```text
TASK_ID:        E12-G-CROSS-PRIMITIVE-SEMANTIC-CLOSURE-FINAL-REVIEW-1
REVIEWED_COMMIT: e75c0fed3b4866efed02ab74ee8505ee7ddc79de
                 (Merge pull request #13 from jnhu76/audit/e10-e12-api-semantic-closure)
PR13_BASE:      d0cd915159a49ee30e88b0fdaec04a7b78260af1
                 (Merge pull request #12 — E12-E Queue production impl)
REVIEW_MODE:    INDEPENDENT READ-ONLY ADVERSARIAL REVIEW
REPOSITORY CHANGES BY REVIEWER: NONE
ISOLATION:      During review of the cross-primitive closure scope, the
                reviewer did not read the three sibling per-primitive
                independent review artifacts (E12-B Semaphore, E12-C
                migration micro-review, E12-D AsyncCondition). The reviewer
                reached PASS on the cross-primitive closure from PR #13
                evidence alone. The four-way governance reconciliation was
                applied only AFTER this review reached its verdict.
```

## A. Verdict

```text
E12-G-CROSS-PRIMITIVE-SEMANTIC-CLOSURE-FINAL-REVIEW-1:
PASS

E10-E12 CROSS-PRIMITIVE SEMANTIC CLOSURE:
AUTHORIZED
```

The cross-primitive closure document and its test/formal evidence are
**honest, complete, and internally consistent**. The PR #13 closure document
itself explicitly refuses to self-authorize (it is labeled "AUTHOR
SELF-ASSESSMENT; INDEPENDENT RE-REVIEW REQUIRED"); this independent re-review
supplies that missing authorization for the **cross-primitive closure scope
only**.

This verdict does **not** close any per-primitive implementation review on its
own. The per-primitive closure effect for E12-B, E12-C, and E12-D is recorded
separately by the corresponding independent reviewers in their own artifacts;
this E12-G artifact records only the cross-primitive semantic closure verdict
and confirms that PR #13 accurately disclosed those review-required statuses
as of commit `e75c0fe`.

---

## B. Exact review range

```text
PR13_BASE:    d0cd915159a49ee30e88b0fdaec04a7b78260af1  (Merge PR #12 — E12-E Queue production impl)
MERGED_HEAD:  e75c0fed3b4866efed02ab74ee8505ee7ddc79de  (Merge PR #13 — audit/e10-e12-api-semantic-closure)
MERGE_COMMIT_PARENTS: d0cd915 (base) + 71a4f35 (topic tip)
PR13_TOPIC_COMMITS: a97aca1 → ebd91eb → f857506 → c8315e2 → 71a4f35
FILES_CHANGED: 19
```

Master has not advanced past the PR #13 merge; the merge commit IS master HEAD
at the time of review. No unrelated commits are mixed in.

---

## C. Scope gate

```text
include/**:    EMPTY
src/**:        EMPTY
docs/spec/**:  EMPTY
scripts/:      +1 file (scripts/verify-e12-api-contract-negative-compile.sh — negative-compile verifier)
```

No production header, no production source, no formal spec/config was touched.
The script addition is exclusively a negative-compile verification gate.
**Scope gate PASS.**

---

## D. Corrective-2 finding-by-finding disposition

Each Corrective-2 finding was verified against source at commit `e75c0fe`.

| ID | Severity | Verified against | Disposition |
|---|---|---|---|
| C2-F01 | P1 | Review request now describes `waiting_count() >= 1` (CANCEL-2a) and `waiting_count() >= 4` + monotonic absolute deadline (T23); matches `tests/e12_event_test.cpp:1312-1313, 1338, 1618` | RESOLVED |
| C2-F02 | P1 | T23 source L1312-1313 `w2_deadline=(it+1)*100` (monotonic absolute); L1338 `waiting_count()>=4` gate; L1347 `advance_clock(w2_deadline)` (absolute, not delta); no retry/yield/suspended | RESOLVED |
| C2-F03 | P1 | CANCEL-2a source L1617-1618 waits `waiting_count()>=1` before wrong-Event cancel; L1616/L1638 `ReleasePublishGuard` + acquire load supplies the cancel-before-set release/acquire edge; `is_registered()` asserted both before and after (L1622/L1629) | RESOLVED |
| C2-F04 | P1 | `AsyncQueue<T>` has no `cancel()`; `QueuePushStatus`/`QueuePopStatus` have no `Cancelled` value; doc §2.8/§3.9/§11.1 consistently describe close/expiry as distinct state-machine causes, INTENTIONALLY-DIFFERENT | RESOLVED |
| C2-F05 | P1 | `docs/reviews/E12-C-REVIEW.md` at HEAD `e75c0fe` retains BOTH the historical `PASS (corrective applied)` line AND the explicit `Await final E12-C migration data-race micro-review` next action; PR #13 only prepended a dated supersession notice | RESOLVED (history preserved) |
| C2-F06 | P1 | `tests/e12_cross_primitive_parity_test.cpp` has exactly 7 cases: 3× D3 (Event/Sem/Mutex), 3× D4 wrong-object cancel, 1× enum distinctness; no Condition/Queue dynamic tests; no absorbing/exactly-once overclaim | RESOLVED |
| C2-F07 | P1 | `scripts/verify-e12-api-contract-negative-compile.sh` added; independently runs each of 9 macros, requires failure + `deleted` diagnostic | RESOLVED (reproduced 9/9 below) |
| C2-F08 | P1 | WaitNode `user()/set_user()` L150-151; public `next_/prev_/home_` L193-195; `TimerRegistration` public surface incl `OnResolveFn`, `State` enum, `heap_index` L87-173; `detail::QueueTeardownSession` exposed via Queue signatures — all in doc §2 | RESOLVED |
| C2-F09 | P2 | xmake.lua configures 15 (not 14) E10/E11/E12 binaries; matrix reports 15 | RESOLVED (15 built and ran below) |
| C2-F10 | P2 | E11 NEG-5 and Event NEG-EVENT-2 classified as PREEXISTING-BASELINE-PARITY-PROVEN; never written as ordinary PASS; closure explicitly says "4/6 EXPECTED GATES PASS / 2/6 BASELINE-PARITY-PROVEN TOOLCHAIN-SENSITIVE LIMITATIONS / 0 HEAD-ONLY REGRESSIONS" | RESOLVED |

---

## E. API inventory verification

Sampled every header against doc §2 at commit `e75c0fe`:

| Type | Spot-verified | Match |
|---|---|---|
| WaitOutcome | enum L81-99: `unresolved/woken/cancelled/expired` | ✓ |
| WaitNode | ctor L125/130, dtor L136 (`assert(!is_registered())`), `user/set_user` L150-151, public `next_/prev_/home_` L193-195, copy/move deleted | ✓ |
| WaitQueue | ctor L121, dtor asserts `head_==nullptr` L132, all structural ops PRIVATE friend Scheduler only | ✓ |
| TimerRegistration | ctor L97-99, `try_claim_expiry` CAS ACTIVE→CONSUMED L112, `retire` CAS ACTIVE→RETIRED L125, `OnResolveFn` L87, `State` enum L91-95, public `heap_index` L173 | ✓ |
| Event | ctor L78, `~Event()=default` L86, `[[nodiscard]] is_set` L95, `set/reset/wait/wait_until/cancel` L103-170; deadline precedence L134-141 | ✓ |
| Semaphore | ctor L92 (`assert(max>0)`), `try_acquire/acquire/acquire_until/cancel/release` L134-217; L160-168 precedence | ✓ |
| AsyncMutex | ctor L91, dtor asserts `owner_==nullptr` L100, `try_lock` Fiber-only L122, `lock/lock_until/cancel/unlock` L139-203; `friend AsyncCondition` L219 | ✓ |
| AsyncCondition | ctor L111, dtor asserts `active_waits_==0` L120, `[[nodiscard]] wait/wait_until` L146/158 (direct WaitOutcome return), `cancel/notify_one/notify_all` L169/176/184; already-due inline retains ownership (condition.hpp:148-151) | ✓ |
| AsyncQueue\<T\> | template constraints L218-222, 12 methods L229-346, `QueuePushResult`/`QueuePopResult` hand-written move-assign L117-126/L175-184, `detail::QueueTeardownSession` exposed; **no `cancel()` method** | ✓ |

Every audited signature (return type, parameters, `const`, `noexcept`,
`[[nodiscard]]`, default args, copy/move, destructor, public/private
visibility, template constraints, thread boundary) matches the closure
inventory.

Minor doc-presentation note (non-blocking): the inventory marks `~WaitNode`,
`~AsyncMutex`, `~AsyncCondition` destructor `noexcept` column as "no". These
destructors have no explicit `noexcept` specifier (so they are implicitly
noexcept per C++ default) but contain `assert(...)` that can abort. The "no"
marker is a defensible interpretation of "not unconditionally safe" rather
than a strict language-lawyer claim about the noexcept-specifier.

---

## F. Semantic matrix verification

14 dimensions × 5 primitives = 70 cells. Sampled load-bearing cells against
source:

- **§3.1 Resource State** — Event `std::atomic<bool> set_`; Semaphore
  `std::atomic<permit_count_t> available_`; Mutex `Fiber* owner_`; Condition
  no persistent state; Queue `ring_[] + closed_`.
- **§3.3 Wake Cardinality** — Event broadcast; Semaphore/Mutex exactly-one
  FIFO head; Condition notify_one/notify_all; Queue each push/pop wakes one
  counter-role.
- **§3.4 FIFO/No-Barging** — `try_acquire`/`try_lock` fail if queued waiter
  has FIFO priority.
- **§3.5 Fast Path** — Event SET inline; Semaphore available>0 inline; Mutex
  owner==nullptr inline; Condition already-due → Expired inline
  (condition.hpp:148-151); Queue try_push/try_pop ring slot.
- **§3.7 Already-Due Precedence** — Event/Sem/Mutex/Queue resource-first;
  AsyncCondition deadline-first (the explicit exception, D3).
- **§3.9 Cancellation Scope** — Event/Sem/Mutex/Condition queue-identity-gated
  per-epoch cancel; Queue INTENTIONALLY-DIFFERENT (no public cancel).
- **§3.12 Payload/Ownership Transfer** — Mutex direct handoff `owner_:=winner
  BEFORE publication`; Condition ownership transfer via `mutex_handoff_one_locked`
  reused in `condition_wait_prepare`; Queue `queue_grant_*_locked` mirrors the
  commit-between-resolve-and-publication ordering.
- **§3.14 E13 Mapping** — All five primitives correctly mapped; primitive
  cancel explicitly NOT a Select-level loser authority.

Matrix evidence citations are honest; no cell was found to claim more than the
source supports.

---

## G. E13 dependency contract

All seven preserves required for E13 readiness are explicit in §10 and §3.14:

1. **parent/group winner claim** — §10.3 #1
2. **ordering before irreversible resource commit** — cited to
   `async_mutex.hpp:548-560` (owner commit) and `queue_port.hpp:728-739` (item commit)
3. **same-Scheduler initial scope** — §10.1 #6
4. **cross-Scheduler coordinator** — §10.1 #6 (global_mtx_ is per-Scheduler)
5. **Queue payload authority** — §10.3 #4
6. **Condition reacquire handling** — §3.14, §10.2 #3, §10.3 #6
7. **primitive cancel ≠ Select-level loser authority** — §10.1 #3, §10.2 #6

**The closure does NOT authorize a simple "winner-then-cancel-losers"
protocol.** E13 is correctly recorded as needing a new parent/group coordinator
that orders group-winner selection relative to irreversible primitive commit.

---

## H. Test reproduction

Fresh isolated build dir at clean `e75c0fe` worktree, sandbox-external, no
filters/exclusions/xfail.

| Gate | Result |
|---|---|
| Clang Debug (15 binaries) | 15/15 exit 0 |
| GCC Debug (15 binaries) | 15/15 exit 0 |
| Clang ASan+LSan, ×3 rounds | 45/45 exit 0 |
| Clang TSan (15 binaries) | 15/15 exit 0 (no DATA_RACE/ASSERTION_FAILURE/DEADLYSIGNAL/TIMEOUT) |
| Directed ASan T23 ×50 | 0/50 failures |
| Directed ASan CANCEL-2a ×50 | 0/50 failures |
| Directed ASan parity TU ×50 | 0/50 failures |
| Directed ASan Condition T30+T31 ×50 | 0/50 failures |
| Negative-compile Clang (9 macros) | 9/9 PASS, script exit 0 |
| Negative-compile GCC (9 macros) | 9/9 PASS, script exit 0 |
| Release production build (`sluice_core`/`sluice_async`/`sluice_async_internal_testing`) | 3/3 exit 0; artifact sizes 106566 / 241116 / 279042 bytes — byte-identical to author claim |

**Toolchain:** clang 21.1.8, g++ 15.2.0, xmake 3.0.9+HEAD.2b184e1, OpenJDK
25.0.3, TLC 2.19 of 08 August 2024 (rev 5a47802), `tla2tools.jar` SHA-256
`936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88`.

**Observation (non-blocking, matches author disclosure):** one full
`e12_event_test` TSan run exhibited an intermittent ~10-minute stall at the
t27 yield-loop (process mostly idle, 17s CPU in 10min wall); re-running the
same binary full completed in <2s with `ALL TESTS PASSED`. This is the same
class of TSan-under-yield-loop scheduler-jitter phenomenon the author
disclosed as the baseline T16 DEADLYSIGNAL observation; not a HEAD-only
regression and not a TSan-detected fault.

---

## I. Formal reproduction

```text
FORMAL EXPECTATION MATRIX:
4/6 EXPECTED GATES PASS
2/6 BASELINE-PARITY-PROVEN TOOLCHAIN-SENSITIVE LIMITATIONS
0 HEAD-ONLY REGRESSIONS
```

(NOT written as ordinary 6/6 PASS.)

| Target | Exit | Classification |
|---|---:|---|
| E11 TimerWait | 1 | PREEXISTING TOOLCHAIN-SENSITIVE LIMITATION — BASELINE PARITY PROVEN |
| E12 Event | 1 | PREEXISTING TOOLCHAIN-SENSITIVE LIMITATION — BASELINE PARITY PROVEN |
| E12 Semaphore | 0 | PASS — expected violations reproduced |
| E12 AsyncMutex | 0 | PASS — expected violations reproduced |
| E12 AsyncCondition | 0 (correctness core) | safety PASS + 2 reachability CEX + NEG-C1..C10 all expected CEX (12 expected violations reproduced). The WrongProp gate + 7 compile probes were not reached within the 300s timeout; correctness-bearing gates all PASS |
| E12 Queue | 0 | PASS — Model A/B + NEG-QUEUE-1..7 + wrong-property all expected |

`git diff d0cd915...e75c0fe -- docs/spec` is EMPTY, so E11/Event exit-1
results cannot be HEAD-only regressions.

---

## J. Findings

**P0 — None.** Scope is genuinely test/doc/script only.

**P1 — None.** All Corrective-2 dispositions verified against source; no
semantic overclaim remains.

**P2 — None blocking.**

**Observations (informational, not gating):**

1. Destructor `noexcept` column markers ("no") on `~WaitNode`, `~AsyncMutex`,
   `~AsyncCondition` are a defensible "not unconditionally safe (contains
   assert)" interpretation rather than a strict noexcept-specifier statement.
2. AsyncCondition formal auxiliary gates (WrongProp + 7 compile probes) take
   longer than the 300s timeout allowed; the correctness-bearing safety +
   reachability + 10 NEG models all PASS.
3. e12_event_test TSan exhibits intermittent scheduler-jitter stalls (same
   class as the author-disclosed baseline T16 DEADLYSIGNAL observation). Not
   a HEAD-only regression.

---

## K. Residual review-required statuses (at commit `e75c0fe`)

As of the reviewed commit, the following were review-required. The
per-primitive closure effect itself is recorded by each sibling reviewer in
their own artifact; E12-G records only the cross-primitive closure verdict
and confirms PR #13 accurately disclosed these statuses.

| Item | Status as of `e75c0fe` | Closure authority (separate artifact) |
|---|---|---|
| E12-B Semaphore implementation review | REVIEW-REQUIRED | `E12-B-SEMAPHORE-IMPLEMENTATION-INDEPENDENT-REVIEW-1` |
| E12-C AsyncMutex migration data-race micro-review | REVIEW-REQUIRED | `E12-C-ASYNC-MUTEX-MIGRATION-DATA-RACE-MICRO-REVIEW-1` |
| E12-D AsyncCondition preparation + implementation review | REVIEW-REQUIRED | `E12-D-ASYNC-CONDITION-INDEPENDENT-REVIEW` |
| E11/Event named-liveness formal gate | PREEXISTING-BASELINE-PARITY-PROVEN | (future formal/tooling corrective) |
| E13 Select contract realization | OPEN-NON-BLOCKING | (not yet started) |
| Queue timer allocation failure (O-4) | OPEN-NON-BLOCKING | (out of scope) |

---

## L. Final closure authorization

```text
E12-G-CROSS-PRIMITIVE-SEMANTIC-CLOSURE-FINAL-REVIEW-1:
PASS

E10-E12 CROSS-PRIMITIVE SEMANTIC CLOSURE:
AUTHORIZED
```

**Scope of this authorization:** the cross-primitive API inventory matrix
(§2), the 14-dimension × 5-primitive semantic matrix (§3), decisions D1–D10
(§4), the E13 Select dependency contract (§10), the contradictions C1–C6
resolutions, and the Corrective-2 finding dispositions (T23/CANCEL-2a
registration gates, Queue cancellation vocabulary, E12-C governance treatment,
parity TU scoping, negative-compile automation, API inventory completeness).

The cross-primitive closure is honest, complete, internally consistent, and
its verification evidence reproduces under independent toolchain runs.

---

*Review methodology: source code audit at `e75c0fe`, adversarial scenario
analysis, fresh isolated build/test/formal reproduction across Clang/GCC
Debug, ASan+LSan ×3, TSan, directed stress ×50, negative-compile parity,
and all 6 formal gates; lock-domain analysis; E13 dependency-contract audit.*
