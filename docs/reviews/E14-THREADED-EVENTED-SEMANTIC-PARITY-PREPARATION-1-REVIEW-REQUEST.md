# Review Request — E14-THREADED-EVENTED-SEMANTIC-PARITY-PREPARATION-1

```text
TASK:    E14-THREADED-EVENTED-SEMANTIC-PARITY-PREPARATION-1
MODE:    AS-BUILT PRODUCTION-FIRST AUDIT + IMPLEMENTATION PREPARATION
AUTHORIZATION REQUESTED: independent review of the preparation document
                         (NOT implementation authorization)
REVISION UNDER REVIEW: rev-3+1
```

**Author:** preparation-phase audit agent.
**Date:** 2026-07-26 (rev-3+1).
**Audited HEAD:** `master` at `4ffff76` (Merge PR #28 — E12-G terminal audit).
**Working tree at audit start:** clean.
**Document under review:** [`docs/e14-threaded-evented-parity-preparation.md`](../e14-threaded-evented-parity-preparation.md) (rev-3).

> **Revision history of this review request.**
> The rev-1 request framed F2 as a single destructor-drain finding, F1 as an
> open F1-A-vs-F1-B contract choice, F5 as a "does `SLUICE_HAS_EVENTED`
> exist?" question, and listed three blocking human semantic decisions. That
> framing is **superseded**. This rev-3+1 request reflects the corrections
> applied through rev-2, rev-3, and rev-3+1 in response to four prior
> independent reviews (`E14-PREPARATION-REVIEW: FAIL`,
> `E14-PREPARATION-REV2-REVIEW: FAIL`,
> `E14-PREPARATION-REV3-REVIEW: FAIL` on artifact-integrity grounds, and
> `E14-PREPARATION-REV3-REVIEW: PASS-WITH-OBSERVATIONS` whose O1–O3 fixes
> are applied in this rev-3+1 request). The deltas a reviewer most needs to
> re-check are summarized in §5. Per the rev-3+1 PASS-WITH-OBSERVATIONS
> outcome, a fresh full re-review is NOT required after O1–O3 — only a
> grep/diff sanity check that they landed.

---

## 1. What is being requested

Independent adversarial review of the E14 preparation document (rev-3).
**No production code, test code, formal-model code, or commit/push/PR is
authorized by this request.** Review of the preparation document only.

Implementation authorization is DENIED by this preparation. Implementation
requires (a) this review signing off the preparation, AND (b) the four
blocking human decisions in §22 of the preparation — **D-E14-1** (F1
wake mechanism), **D-E14-F2a** (Evented Group destructor teardown
policy), **D-E14-2** (F5 admission mechanism), and **D-E14-3** (size()
semantics) — being resolved by accepted authority. (D-E14-F2a was
promoted to a numbered decision ID in rev-3+1 per observation O2; it is
NOT a renumbering of D-E14-2 / D-E14-3.)

---

## 2. Reviewer instructions

Per `async-runtime-construction-method.md` M9, **independent review reads
production first**. Do not begin by trusting this document's narrative.
Recommended order:

```text
1. production authority inventory (include/sluice/async/, src/async/)
2. blocking/wake topology (scheduler.cpp:run_impl / park_on_wake_source)
3. state topology (group.cpp Group::await + ~Group; future.hpp complete_with)
4. the traces in §14 of the preparation (esp. T9, T11, T12 rev-3)
5. the findings in §16 of the preparation (F1, F2a, F2b, F3, F4, F5a/F5b)
6. this document's claims vs production
```

For each claimed finding, verify:

- The cited `file:line` actually contains what the preparation says it does.
- The adversarial trace is REACHABLE in production (not hypothetical).
- The classification is correct under the §5 taxonomy.
- The proposed correction is the smallest that restores an already-accepted
  contract (not a redesign).
- The formal-model impact (§18) is correctly characterized.

### 2.1 Artifact-integrity check (load-bearing for this round)

A prior round FAILED on a physically truncated upload (`B-REV3-1`: the
reviewer received a copy ending mid-§19.2 at `F3 is`, 2549 lines / 131
fences / no trailing newline, while the on-disk file was complete). Before
substantive review, the reviewer MUST confirm the artifact is complete.
Exact `wc -l` / `wc -c` / fence counts are NOT hard-coded here, because
they drift with each edit and were the source of the prior mismatch;
instead verify STRUCTURAL completeness and the externally-captured digest.
(Counting fences with a literal triple-backtick pattern inside a fenced
block is itself ambiguous in Markdown, so the check is phrased without
embedding that literal.)

```text
# 1. fence balance: count of triple-backtick lines must be EVEN
# 2. tail must show the Revision ledger (§24, incl. §24.2 rev-3 and
#    §24.3 artifact-integrity proof) and the "## Cross-links" section
tail -n 80 <preparation>.md
# 3. the §23 verdict must be present
grep -c 'E14-PREPARATION: NOT-READY' <preparation>.md   # >= 1
# 4. digest: compare to the value recorded in the rev-3+1 upload
#    completion report (captured externally because writing it into the
#    file changes the file's own digest)
sha256sum <preparation>.md
```

If the received artifact does NOT match (truncated, fence-imbalanced, or
missing §20–§24 + Cross-links), return
`E14-PREPARATION-REV3-REVIEW: FAIL` on artifact-integrity grounds alone,
as the prior round did. Do not review a partial copy.

---

## 3. What was audited

The preparation inspects, with `file:line` evidence:

- The full mandatory reading set (task §4): AGENTS.md, ADR-execution-model,
  ADR-async-io-model, async-runtime-plan, async-runtime-construction-method,
  api-reference, e10-e12-api-semantic-closure, e12-cross-primitive-terminal-
  audit, e13-select-production-architecture/test-plan/p7-rollback-closeout.
- The production source inventory (task §5): all `include/sluice/async/*.hpp`
  and `src/async/*.cpp` for Future, Group, Scheduler, primitives, Select,
  Completion, AsyncIoContext, cancel.
- The test inventory: future/evented_future/group/evented_group/evented_scheduler
  /scheduler_progress/external_wake/multi_worker/runnable_steal/timer_wait tests
  + E12 primitive tests + E13 Select tests.
- The build system: `xmake.lua` target membership and comments.
- The README and api-reference.

Baseline (run before this audit, evidence in the preparation's completion
reports):

```text
git status --short                  (clean)
git branch --show-current           master
git log -1 --oneline                4ffff76 Merge PR #28
xmake f -m debug --toolchain=clang -y        exit 0
xmake build sluice_core                      exit 0
xmake build sluice_async                     exit 0
xmake build -g test                         exit 0
xmake test -v                              exit 0
                                           100% tests passed, 0 failed of 102
```

---

## 4. Confirmed findings the reviewer must verify (rev-3)

| ID | Class | Severity | One-line summary |
|---|---|---|---|
| F1 | ACCIDENTAL_SEMANTIC_DIVERGENCE | P1 | Evented Group::await returns with pending externally-produced Futures (T9). Contract is FROZEN to await-all; only the wake mechanism (T-WAKE-1..8 / D-E14-1) is open. |
| F2a | ACCIDENTAL_SEMANTIC_DIVERGENCE | P1 | `~Group` has no `sched_` branch and unconditionally awaits every task Future; a pending Evented task Future dereferences null `g_worker`/`current` in `await_ready_flag` on the caller thread, BEFORE any Fiber/stack release (T11 PRIMARY). |
| F2b | ACCIDENTAL_SEMANTIC_DIVERGENCE (conditional) | P2 | Residual `waiting_ready_` registration after Group destruction: the ready-flag KEY (`&Future::ready_`) and `WaitReg.fiber` both dangle; the actual deref is a later `wake_ready_flags_locked()`, NOT the map's own destructor (which only destroys pointer values). |
| F3 | ACCIDENTAL_SEMANTIC_DIVERGENCE | P2 | `init_fiber` failure silently discarded by `Group::async_evented`; RT-F3 must prove the failure is reported BEFORE spawn/enqueue (T13). |
| F4 | ACCIDENTAL_SEMANTIC_DIVERGENCE | P3 | Evented Group `size()` diverges from Threaded after await (T14); depends on the D-E14-3 contract-clarification decision. |
| F5a | ACCIDENTAL_SEMANTIC_DIVERGENCE | P2 | Evented public admission does not enforce `fiber_ctx::supported` (a capability PREDICATE, not yet a gate). Chosen enforcement must keep Threaded compilable/functional; an unscoped `static_assert` in a shared header is FORBIDDEN. |
| F5b | TEST_GAP | P2 | Unsupported-target tests `if constexpr (!supported) return;` and report PASS having proven nothing; RT-F5b must prove the fail/disable contract, not silently early-return. |
| F6 | DOC_DRIFT (multiple) | P2 | Documentation, header comments, build comments, README drift. |

Rejected: R1 (Threaded equivalents of E12 primitives — INTENTIONAL), R2
(Scheduler parking as divergence — out of scope), R3 (AsyncIoContext Fiber
awareness — strategy-neutral L1), R4 (Future resolve_ CAS — incidental
redesign).

---

## 5. Specific things the reviewer should challenge (rev-3 deltas)

These are the rev-2→rev-3 and rev-1→rev-3 corrections a reviewer should
specifically NOT take on trust, because they were the subject of prior
FAILs:

1. **T12 destructor path (rev-3 corrected).** Read `group.cpp:76-92`
   (`~Group`), `evented_wait_policy.hpp:53-58` (borrowed `Scheduler&`),
   `scheduler.cpp:44` (`thread_local WorkerState* g_worker` — NOT a
   Scheduler member), and `scheduler.cpp:1074-1076` (`await_ready_flag`).
   Confirm T12's two-distinct-failure-modes account: (i) the member call
   through a dangling `Scheduler&`, then (ii) inside `await_ready_flag`
   the read of the thread-local `g_worker` (null on the caller thread) and
   the subsequent `ws->current` null-deref. If the document still reads as
   "reads `s.g_worker` out of destroyed Scheduler storage," that is the
   non-blocking wording error to flag.

2. **F2b dereference site (rev-3 corrected).** Confirm the document does
   NOT claim the `waiting_ready_` unordered_map's own destructor
   dereferences `Fiber*` (it only destroys pointer values). Confirm the
   actual dereference site is recorded as a later
   `wake_ready_flags_locked()` (`scheduler.cpp:950-966`) that reads the
   dangling `&Future::ready_` key and then `WaitReg.fiber`.

3. **F1 formal classification (rev-3 corrected to PROVISIONAL).** The
   decision is `PROVISIONAL_NO_FORMAL_CHANGE`, NOT an unconditional
   `NO_FORMAL_CHANGE`. It stays `NO_FORMAL_CHANGE` ONLY IF the chosen
   D-E14-1 mechanism introduces no distinct per-Future/per-registration
   attachment state and refines to existing E9 transitions; it MUST be
   upgraded to `EXTEND_EXISTING_MODEL` otherwise. Confirm the document
   does not pre-judge the absence of new state before D-E14-1 is resolved.

4. **F1 await-all contract (frozen since rev-2).** The await-all contract
   is FROZEN by existing authority (`group.hpp:93-97` + ADR §3); F1-B
   (narrow to in-Scheduler producers) is NOT an available smallest
   correction without a superseding ADR. Confirm D-E14-1 is framed as a
   mechanism decision (answer T-WAKE-1..8), not a contract YES/NO.

5. **F5 gating portability (rev-3 constrained).** Confirm the document
   describes `fiber_ctx::supported` as a capability PREDICATE (not a gate,
   until an admission/build mechanism enforces it) and that the chosen
   enforcement requires the portability proof: Threaded stays
   compilable/functional, Evented is excluded/unavailable/deleted or fails
   cleanly, and a shared-header include does NOT disable Threaded.

6. **Test-ID consistency (rev-3 unified).** Confirm RT-F3 / RT-F5a / RT-F5b
   mean the same thing everywhere (§15.4, §20 P2, §21 matrix):
   - RT-F3 = `init_fiber(false)` reported BEFORE spawn/enqueue;
   - RT-F5a = unsupported-target Evented public admission enforces the
     chosen gate;
   - RT-F5b = unsupported-target CI/test proves fail/disable, not a silent
     early-return.
   The conflicting older definition "RT-F5b = init_fiber failure
   propagation" must be gone.

7. **Artifact integrity.** Per §2.1, compare the received preparation
   artifact's SHA256 with the externally supplied digest, confirm balanced
   fences and a trailing newline, and confirm that §20–§24 plus
   `## Cross-links` are present. Do not hard-code line or fence counts here
   (they drift with each edit and were the source of a prior mismatch).

---

## 6. What the reviewer should NOT do

- Do NOT authorize production implementation in this review. Implementation
  requires the four blocking decisions (D-E14-1, D-E14-F2a, D-E14-2,
  D-E14-3) plus a separate implementation review.
- Do NOT re-litigate CLOSED E12 contracts (E12-G pass) or CLOSED E13
  contracts (P7 closeout). E14 scope is limited to Threaded/Evented parity
  for Future/Group/I/O and the surface-existence of E12/E13 primitives.
- Do NOT propose new abstractions (Executor, Awaitable, etc.) — these are
  forbidden by task §11.
- Do NOT mark a finding CONFIRMED without a reachable production trace
  (task §15).
- Do NOT review a truncated / fence-imbalanced copy. If §20–§24 and
  Cross-links are absent, FAIL on artifact integrity and request a clean
  re-upload (this is the documented cause of the prior round's FAIL).

---

## 7. Expected review outcomes

The reviewer should reach one of:

```text
E14-PREPARATION-REVIEW: PASS
    The preparation document (rev-3) is accurate, the findings are correctly
    classified, the plan is narrow, the F1 formal classification is
    correctly PROVISIONAL, and implementation MAY proceed once the four
    blocking decisions are resolved.

E14-PREPARATION-REVIEW: PASS-WITH-OBSERVATIONS
    The preparation is acceptable but specific corrections are required
    (cite each). Implementation MAY proceed once observations AND the four
    blocking decisions are resolved.

E14-PREPARATION-REVIEW: FAIL
    The preparation contains a material error (false-positive finding,
    missed true-positive, wrong classification, hidden scope expansion,
    formal-model mischaracterization, OR an artifact-integrity defect
    such as truncation / fence imbalance / missing final sections). The
    preparation must be revised and re-reviewed.
```

---

## 8. Cross-links

- Preparation document: [`docs/e14-threaded-evented-parity-preparation.md`](../e14-threaded-evented-parity-preparation.md) (rev-3).
- Construction method (normative for review): `docs/async-runtime-construction-method.md`.
- Authority: `AGENTS.md`, `docs/adr/ADR-execution-model.md`.
- Closed E12-G audit (input): `docs/e12-cross-primitive-terminal-audit.md`.
- Closed E13 production architecture (input): `docs/e13-select-production-architecture.md`.
