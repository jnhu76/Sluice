# Post-Freeze Structural Hygiene Final Report (R0 + R1)

**Verdict: READY_FOR_APPLICATION_DESIGN**

**Date:** 2026-08-16
**Baseline SHA:** `d9184de` (master, merge of PR #109 — Phase G foundation freeze)
**Head after this pass:** this report's commit (R1 split + docs)
**Governing docs:** [structural-audit.md](structural-audit.md) (R0, committed
`aaad4a2`), [zig-std-io-alignment.md](zig-std-io-alignment.md),
[../architecture/foundation-freeze.md](../architecture/foundation-freeze.md)

---

## 1. Scope delivered

| Item | Status |
|---|---|
| R0 LOC inventory + 12-dimension audit + risk matrix | done, committed `aaad4a2` |
| Zig `std.Io` structural concept map (read from source via context7) | done, committed, link-fix amended |
| Divergence classification (ALIGN_NOW / VALID_CPP_DIFFERENCE / DEFER_TO_APPLICATION_EVIDENCE) | done — SD-A1 executed; SD-1..SD-5 kept with rationale; PF-1..PF-3 deferred |
| R1 surgical refactor: `scheduler.cpp` god-TU split | done, 9 TUs; moved bodies byte-identical (certificate), cross-TU glue separately verified (§2) |
| First application workload design | done — [../history/application-designs/first-workload.md](../history/application-designs/first-workload.md) (`sluice-pipeline`) |
| Full test matrix Debug + Release | done, both green (§5) |

## 2. R1 refactor — what changed

`src/async/scheduler.cpp` (5864 lines, ten concepts) → concept TUs, matching
the repository's established `select_event.cpp` / `select_timer.cpp` /
`queue_port.cpp` pattern:

| File | Lines | Domain |
|---|---|---|
| `src/async/scheduler_park_wake.cpp` | 1309 | park/wake, R1-R4 protocol, interrupt bridge |
| `src/async/scheduler_timer.cpp` | 609 | deadline heap, clock, test-clock |
| `src/async/scheduler_event.cpp` | 395 | SchedulerEvent wake targets |
| `src/async/scheduler_semaphore.cpp` | 314 | semaphore waits |
| `src/async/scheduler_mutex.cpp` | 342 | AsyncMutex waits |
| `src/async/scheduler_rwlock.cpp` | 685 | rwlock waits, ForgedRwWaitCtx |
| `src/async/scheduler_condition.cpp` | 267 | condition waits |
| `src/async/scheduler_queue.cpp` | 591 | runnable queue, fiber routing |
| `src/async/scheduler_internal.hpp` | 71 | non-installed: `g_worker` TLS (inline), `SchedulerWakeHandle::Control` |
| `src/async/scheduler.cpp` | 2146 | kept: ctor/dtor, worker loop, steal, spawn/run, classification |

Line counts in this table are enforced by `scripts/gates/mechanical-facts.py`
(LOC claims must equal `wc -l`), so the inventory cannot silently drift.
Post-freeze corrective deltas update the count here with their attribution:
`scheduler_park_wake.cpp` 1113 → 1144 (2026-08-17, Issue #116 — test-only
`SLUICE_ASYNC_INTERNAL_TESTING` forensics extension of
`dump_park_forensics_for_test`; production park/wake behavior unchanged);
`scheduler_park_wake.cpp` 1144 → 1153 and `scheduler.cpp` 1952 → 1975
(2026-08-17, Issue #115 — test-only post-baseline park seam in the
internal-testing variant + the spawn/spawn_on runnable-publication wake
signal, a post-freeze evidence-derived correctness fix; see
`docs/history/issues/issue-115-runnable-publication-wake.md`);
`scheduler.cpp` 1975 → 1986 and `scheduler_park_wake.cpp` 1153 → 1155
(2026-08-17, Issue #115 review round 2 — G1 park-commit refusal priority:
runnable tickets refuse unconditionally, the observer exemption covers only
backend work / resident waits; header contract + park comment refreshed;
same investigation doc §6a);
`scheduler.cpp` 1986 → 1991 (2026-08-20, Issue #139 — code-adjacent
navigation comment above `ReadyRoutingSink::on_ready` pointing to the
request-lifecycle walkthrough doc `docs/architecture/async-request-lifecycle.md`;
comment-only, no behavior change);
`scheduler.cpp` 1991 → 2085 and `scheduler_park_wake.cpp` 1155 → 1193
(2026-08-21, Issue #161 — the contribution-identity repair: the three
idle-count erase sites become `exchange(0)` + a conditional generation
bump, the park-commit identity-refusal term, and the per-worker test
seams; see
`docs/architecture/issue-161-idle-dance-contribution-generation-gate.md`);
`scheduler.cpp` 2085 → 2065 (2026-08-22, Issue #170 — removal of the inert
worker-inbox notification surface: the never-populated `WorkerState::inbox`
deque, the no-consumer `inbox_cv`, all 9 inert notifies, and the three
notify-only terminate loops; comments reworded to the wake-domain authority.
No scheduling/wake/lock-order semantic change — see issue #170 Comment A);
`scheduler_rwlock.cpp` 667 → 677, `scheduler_mutex.cpp` 343 → 345,
`scheduler_semaphore.cpp` 315 → 317, `scheduler_timer.cpp` 504 → 506,
`scheduler_condition.cpp` 264 → 265 (2026-08-21, Issue #162 Phase 7 —
audit CPP-001/CPP-002: rwlock publication owner lookups unified onto the
I47-F1 authoritative `owner_for_fiber_locked` (fail-fast, no `g_worker`
fallback), and the dead `make_runnable()` calls on the current RUNNING
fiber removed from every inline admission-resolution path (no-op from
running; comments now state the no-publication invariant). No reachable
behavior change; see
`docs/history/closeout/e12-cross-primitive-terminal-audit.md` §11.7);
`scheduler_park_wake.cpp` 1193 → 1293 and `scheduler.cpp` 2065 → 2122
(2026-08-24, Issue #196 — test-only `SLUICE_ASYNC_INTERNAL_TESTING` E9
trace-conformance recorder call sites (wake publication / park commit /
entered / returned / refused / wake-cause markers), plus the two
duplicated cv-predicate lambdas in `park_on_wake_source` unified into one
`park_pred` (behavior-identical; the guarded entry-evaluation uses it to
observe the immediate-return boundary). Production park/wake behavior
unchanged — the release library carries zero trace symbols; see
`docs/verification/formal/e9-trace-conformance.md`);
`scheduler_queue.cpp` 503 → 499 (2026-08-26, Issue #227 Phase 0 — removal of
a dangling empty `// ====...====` banner comment left over from the R1
split; comment-only, no behavior change);
`scheduler.cpp` 2122 → 2128 (2026-08-26, Issue #229 — the internal-testing
`AsyncTestAccess::active_deadline_count` accessor gains a `global_mtx_`
locked snapshot (TSan data race repair: the coordinator fiber polled the
unlocked counter while workers mutated it); test-only vocabulary, production
behavior/layout unchanged — see
`docs/architecture/issue-229-deadline-test-seam-lock-gate.md`);
`scheduler_timer.cpp` 509 → 536, `scheduler_queue.cpp` 499 → 515,
`scheduler_rwlock.cpp` 674 → 677, `scheduler_mutex.cpp` 344 → 340,
`scheduler_semaphore.cpp` 316 → 312, `scheduler_event.cpp` 397 → 393,
`scheduler_condition.cpp` 264 → 263 (2026-08-27, AC-2b — ordinary deadline
lifecycle authority: eight of the ten ordinary arming sites and every inline
consume/retire transition now route through `arm_ordinary_deadline_locked` /
`consume_ordinary_deadline_locked` / `retire_ordinary_deadline_locked`
(declared in `include/sluice/async/scheduler.hpp`); the two remaining arming
sites are Queue push/pop, which stay intentionally LOCAL — their historical
interleave of `++active_queue_timers_` with the ACTIVE-count/cache publication
is observable through the lock-free earliest-deadline cache that parked
workers read without `global_mtx_`, so that order — including those two
call sites' direct `++active_deadline_count_` arming increments — is
preserved verbatim — while Queue
consume/retire transitions DO route through the authority; the raw-pointer
arm form preserves register_test_deadline_locked's null-node seam contract;
cache-recompute timing and on_resolve hook firing stay at each call site;
Select timers untouched; behavior-preserving authority compression).
`scheduler_park_wake.cpp` 1293 → 1309, `scheduler_queue.cpp` 515 → 513,
`scheduler_rwlock.cpp` 677 → 675, `scheduler_mutex.cpp` 340 → 338,
`scheduler_semaphore.cpp` 312 → 310, `scheduler_event.cpp` 393 → 391,
`scheduler_condition.cpp` 263 → 261 (2026-08-27, Issue #227 AC-2c-b /
[issue #237 context](https://github.com/jnhu76/Sluice/issues/237) — six
primitive-gated cancellation paths now reuse the
private `cancel_primitive_wait_locked` authority for exact-queue membership,
the Cancelled terminal winner/unlink, and AC-2b timer retirement.
`waiting_waitq_count_` retirement deliberately stays at each call site: the
exactly-once wait-epoch retirement obligation is semantic, but that concrete
counter is current stackful Scheduler bookkeeping (MW classification), per the
AC-2a/#237 frontend-neutrality separation (review-corrected). Event,
Semaphore, AsyncMutex, AsyncCondition, Queue, and AsyncRwLock retain their
local Fiber publication and primitive-specific counter/reconcile policy;
generic `Scheduler::cancel_wait`, Select, Completion-waiter, task, and I/O
cancellation remain local and unchanged. This adds one out-of-line helper
call and no new allocation, lock, traversal, or callback. AC-2b timer
retirement still fires any already-installed Queue timer-accounting hook.
There is no public API, ABI, or object-layout change; therefore public API
reference documentation is unaffected.)
`scheduler_queue.cpp` 513 → 582 (2026-08-28, Q-LIV-1 — blocking/timed Queue
admit inline-success paths now reconcile the opposite-role FIFO head via the
existing `queue_grant_consumer_locked` / `queue_grant_producer_locked`
authority (the reconcile `try_push`/`try_pop` FastPush/FastPopCommit always
performed); the grant runs after the admitting role mutex is released, under
`global_mtx_` + `state_mtx_` only — the same lock shape as the fast paths, the
two role mutexes still never held together — and the inline path returns
before the suspend switch because the admitting fiber never suspended.
Queue liveness repair, NOT a structural refactor: role FIFO, ring order,
lease semantics, close semantics, and AC-2b local Queue timer arming are
unchanged; the DST-PV-1 known-drift witness is flipped into the post-fix
regression. No new allocation, mutex object, or lock-order edge; each inline
commit adds one opposite-role mutex acquisition (inside the grant, the
opposite role's `WaitQueue::mtx_`, under the same `global_mtx_` +
`state_mtx_` shape as the fast paths) plus an O(1) FIFO head/empty check —
one grant call added on each of the four inline-commit paths.)
`scheduler_timer.cpp` 536 → 596, `scheduler_queue.cpp` 582 → 591,
`scheduler_mutex.cpp` 338 → 342, `scheduler_semaphore.cpp` 310 → 314,
`scheduler_event.cpp` 391 → 395, `scheduler_condition.cpp` 261 → 267,
`scheduler_rwlock.cpp` 675 → 685 (2026-08-28, R2 review round 2 P1 —
allocation-atomic timed admission: the ordinary arming authority is split
into a MAY-THROW `prepare_ordinary_deadline_locked` (deadline-heap slot
reserve with checked max_size guard + `timer_pool_` node allocation, the
select.cpp step-(5) reserve pattern) and a noexcept
`publish_ordinary_deadline_locked` (hook install + ACTIVE count + heap push
within the reserved capacity + earliest-deadline cache recompute); every
ordinary timed admission now performs ALL of its allocations BEFORE
`register_wait_locked`, so an escaping `bad_alloc` leaves the node Detached
and every counter untouched. Queue's two timed admits keep their LOCAL
publish sequence verbatim (AC-2b corrective order preserved: hooks →
`active_queue_timers_` → ACTIVE count → heap → cache) and share only the
prepare phase. `arm_ordinary_deadline_locked` remains as the composed form
for the test-only hook. No public API, ABI, or object-layout change; no new
lock, lock-order edge, or wake-path change; the admission tail after
registration is now provably allocation-free. `push_until`'s value-carrying
lease has no rejection status in the result vocabulary, so its allocation
failure surfaces through the pre-existing non-empty-lease fail-fast boundary
(Debug AND Release) — death-pinned by
`queue_lifecycle_death_push_until_alloc_fail_fast`; the failure-free
`pop_until`/generic/primitive paths are catchable and regression-pinned by
`od_alloc_a1_generic_admission_atomic`, `od_alloc_a2_event_admission_atomic`,
and `queue_alloc_pop_admission_atomic`.)
`scheduler_timer.cpp` 596 → 609, `scheduler.cpp` 2141 → 2146
(`AsyncTestAccess::deadline_heap_capacity` observation seam for the growth
regression) (2026-08-28, R2 review round 3 P1 — the
prepare-phase heap reserve now preserves vector's geometric growth: the
growth allocation fires ONLY when the heap is exactly full and takes the
next doubling step, checked against max_size, instead of round 2's
unconditional size+1 reserve, which walked capacity up one slot per
admission (O(N) reallocations, O(N^2) element moves reaching N concurrent
deadlines). The R2-ALLOC contract is unchanged — every allocation still
precedes `register_wait_locked`, the publish tail stays noexcept, and
`reserve()` keeps the strong guarantee on throw. Growth curve
regression-pinned by `od_alloc_a3_heap_growth_stays_geometric`.)

**Proof boundary (review-corrected wording):** this is a behavior-preserving
structural split, NOT pure code motion. Two proofs cover two different
surfaces:

- *Moved implementation bodies — byte-identical:* the split script asserted
  21 ranges reassemble the original 5864 lines byte-exactly;
  `verify_split.py` certified 15/15 checks — each new TU body equals its
  concatenated HEAD segments; `scheduler.cpp` keeps its segments. No
  function, statement, ordering, or guard was rewritten.
- *New cross-TU glue — NOT covered by the motion certificate, verified
  separately:* the uniform include block (where the #113 `SLUCE_` typo
  lived), the `inline thread_local` linkage form of `g_worker`, the now
  shared `SchedulerWakeHandle::Control` definition, and the file-header
  comments. `g_worker`'s cross-TU identity and per-thread isolation are
  directly verified by `tests/scheduler_tls_identity_test.cpp`; identifier
  near-misses in new text are caught mechanically by
  `scripts/gates/mechanical-facts.py`.

No new abstraction, no virtual interface, no public API change;
`scheduler_internal.hpp` is not installed.

**One defect injected and repaired during this pass** (recorded for the
audit trail, see §6): the uniform include block in the 8 new TUs initially
misspelled the test seam macro (`SLUCE_…` instead of `SLUICE_…`), which made
only the `sluice_async_internal_testing` build fail (`'sluice_async_test' has
not been declared`) while production targets stayed green. Found by direct
source inspection after the input-level debugging protocol proved the failure
deterministic; fixed as a one-character repair per file; both gates re-run
green afterwards.

## 3. Zig alignment outcome

Structural axis (file-per-concept) is now aligned where it was fixable
without touching frozen semantics (SD-A1). Everything else is either a valid
C++/Sluice difference with recorded rationale (SD-1..SD-5) or deferred to
application evidence (PF-1 scheduler.hpp split, PF-2 process I/O, PF-3
`sleep_for`/`Timeout` convenience). No Zig mechanism was imported; no frozen
divergence (DIV-01..DIV-13) was re-litigated.

## 4. Freeze verification — hard-stop conditions honored

- No Phase G frozen invariant changed: behavioral surfaces enumerated in
  `foundation-freeze.md` untouched (split is intra-`sluice_async`
  implementation motion).
- No scheduler/cancellation/backend redesign: state machines, lock domains,
  wake protocol, and all `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)` seam
  bodies are byte-identical to `d9184de`.
- No new public API: `include/sluice/` unchanged; `scheduler_internal.hpp`
  lives in `src/` and is not installed.
- No LOC-driven split of a coherent implementation: every other large file
  audited KEEP with rationale (audit §5).

## 5. Test matrix (actual commands and results)

| Gate | Command | Result |
|---|---|---|
| Baseline before split | full Clang Debug at `d9184de` (recorded pre-task) | 167/167 pass |
| Debug build | `xmake f -m debug --toolchain=clang --with-liburing=true -y` then `xmake build sluice_core` / `xmake build sluice_async` / `xmake build sluice_async_internal_testing` (one target per command) | all green |
| Debug tests | `xmake build -g test && xmake test -v` | **167/167 pass** |
| Release build | `xmake f -m release --toolchain=clang --with-liburing=true -y` then the same three single-target builds | all green |
| Release tests | `xmake build -g test && xmake test -v` | **167/167 pass** |
| Docs gates | `check-doc-links.py`, `verify-architecture-docs.py`, `git diff --check` | PASS / OK / clean |
| Skipped | TSan/ASan/UBSan, real-liburing functional run, formal TLC, negative-compile scripts | not a §16.3/§16.2 change class (no concurrency/ownership semantics touched — pure motion); re-run when the split's successor changes land |

Debug configuration restored after the Release leg.

## 6. Fault post-mortem: the "phantom compile failure"

During R1 verification the `sluice_async_internal_testing` target failed with
`'sluice_async_test' has not been declared` in the new TUs. The failure
survived reboots, tmpfs copies, ccache bypass, both clang and gcc, and
appeared to flip between TUs — which drove a long environment-level hunt
(execve argv/envp capture, frozen `.ii` 20/20 determinism, wrapper
interposition). The determinism result was the turning point, exactly as the
input-first debugging protocol predicted: a deterministic failure means the
trigger is in the **input** (source + argv), not the spawn environment.
Direct inspection then found the `SLUCE_` typo above. The "flipping between
TUs" was output ordering under `-j2` (all eight TUs were broken; whichever
error surfaced last in the tail looked like "the" failure). Production
targets were green because both guard spellings evaluate false without the
define. Evidence preserved under `~/repro_artifacts/` (execve traces, env
diffs, frozen `.ii`); no hardware fault was ever established (EDAC clean;
machine halts were unrelated auto-suspend, since masked).

## 7. Application readiness

The foundation is frozen, audited, structurally aligned where cheap, and
green on the full Debug+Release matrix. The recommended first workload is
**`sluice-pipeline`** (bounded parallel file pipeline):
streaming reads → bounded queue → parallel transform stage → writes, with
deadlines, cancellation, and graceful shutdown exercised end-to-end. It
stays inside the approved scope (file I/O only, DIV-08; no networking) and
is designed to generate the seam evidence that PF-1..PF-3 triggers require.
See [../history/application-designs/first-workload.md](../history/application-designs/first-workload.md).

## 8. Review reconciliation (PR #114 review round)

The review requested seven items before merge; all are addressed here.

| Review item | Resolution |
|---|---|
| CI red (BLOCKER) | Root-caused: pre-existing park-before-spawn stranding window (issue #115), NOT a split regression — moved bodies are byte-identical to `d9184de`. Proven by construction with the park-forensics ledger (reproducer hangs pre-fix, `timeout` exit 124). `runnable_steal_test` now closes the window deterministically (f0 holds its worker until the victim queue actually holds the stealable fiber); validated 50/50 focused, 5/5 single-CPU pinned, 3/3 full binary. |
| 73 vs 72 LOC claim | Fixed to 72 (`wc -l`); the table is now machine-enforced by the mechanical gate. |
| "pure code motion" overstated | Replaced by the §2 proof boundary: moved bodies byte-identical (certificate) vs new cross-TU glue (separately verified). |
| Motion certificate does not cover new glue | `scripts/gates/mechanical-facts.py` mechanically checks new-text classes (identifier near-miss — the exact #113 defect class; its first repository scan caught the pre-existing `SLUCE_` comment in `threadpool_backend.cpp`, now fixed). Wired into `scripts/gates/pre-push.sh` and CI "Documentation verification". |
| `g_worker` cross-TU identity | Directly verified by `tests/scheduler_tls_identity_test.cpp` via the guarded `AsyncTestAccess::tls_worker_probe()` (read in the `scheduler.cpp` TU, written in the test TU; per-thread isolation asserted). |
| Zig alignment pin | `zig-std-io-alignment.md` §1 now pins upstream Codeberg master `99e540dc39ba45365eaa82db0459a0d7acc251eb` (2026-08-16), keeps the `89e0881f` (2026-08-11) lineage, and notes the GitHub mirror is frozen since 2025-11-27. |
| Audit layout vs as-built | `structural-audit.md` §6 annotated with the as-built delta (`scheduler_queue.cpp`, `scheduler_internal.hpp`) and snapshot markers (`at d9184de`) so its historical LOC claims verify against the pinned commit, not the moving tree. |

Review-round gate matrix (2026-08-16, all actually executed):

| Gate | Result |
|---|---|
| Clang Debug: 3 libs + `-g test` + `xmake test -v` | **168/168 pass** (was 167; +1 = `scheduler_tls_identity_test`) |
| Clang Release: same targets and tests | **168/168 pass** |
| `runnable_steal_test` focused case | 50/50; single-CPU `taskset` pin 5/5; full binary 3/3 |
| Negative-compile probes (completion-authority, request-arena) | pass / pass |
| `check-doc-links.py`, `verify-architecture-docs.py`, `git diff --check` | PASS / OK / clean |
| `mechanical-facts.py` main + `--self-test` | OK / all detectors fired |

Release configuration restored to Debug afterwards. TSan not re-run: this
round changes a test's gating, a guarded test-only header seam, docs, and
scripts — no production concurrency semantics moved (the scheduler bodies
remain byte-identical to `d9184de`; see §2 proof boundary).

## 9. STOP

**Foundation remains frozen. No Phase H.** This pass changed implementation
file organization only; it authorizes nothing beyond application design.
The next repository work is the `sluice-pipeline` workload design and its
friction log — not scheduler, backend, cancellation, or public-API changes.
PF-1..PF-3 are recorded triggers, not approved work.
