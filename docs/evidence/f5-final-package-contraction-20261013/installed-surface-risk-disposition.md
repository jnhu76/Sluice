# F5 installed-surface risk and disposition record — #402

Status: **PENDING owner decision.** Every row below is a prepared, validated
breaking change on the candidate branch. External use is UNKNOWN for every row
and is not zero (F0 §1.5: the historical v0.0.1 publication used a
deliberate-deprecation convention; unknown external use is not zero use).

Candidate head: this directory's evidence records the exact implementation
head. Execution baseline: `14a27a83821a94e6fc5a9684b48fcd296d7de891`
(F2 final exit master).

Authority: root v1-r4 MIG-01 Phase F / MIG-02 (ApplicationRuntime/Scheduler/Fiber
row: legacy multi-worker runtime is not a required v1 product; Group/Future/
WaitPolicy row: only the bounded structured lifetime required by a retained
adapter — none remains; public dormant synchronization primitives row: outside
v1; experimental public headers row: isolated or removed). HOST-01: public
runtime-aware sync primitives are outside v1. F0 §1.7 deprecation procedure
applied per surface. #475 adopted A/B/C installation policies and their
T1–T4 triggers / E1–E5 exit gates are the recorded installation dispositions
this slice re-examines under T2/T3; the physical step stays gated on the
owner's T4/E1 external-use decision, which is exactly this packet.

## What breaks

Twenty-three installed headers disappear from the package and the tree, one
library-private header leaves the package only, seventeen compiled objects
leave `libsluice_async.a`, and the never-installed pre-v1 experimental tree
leaves the repository. The removal is a source break (headers gone) and a
relink break (archive members gone) for any external consumer of the removed
entry points; an already linked static executable keeps its copied code and
does not change behavior.

Installed-surface delta, measured by the B-01 v6 clean-room capture
(`b01-v6-candidate/summary.json`, identical in both ring profiles):

| Metric | Before (`14a27a83`) | After (F5 head) |
|---|---|---|
| Installed headers | 49 | 25 |
| Library objects | 32 (2 core + 30 async) | 15 (2 core + 13 async) |
| `libsluice_async.a` members | 30 | 13 |

## Why each family can exit now (the #475 re-examination)

* **Family A** (`condition.hpp`, `semaphore.hpp`, `async_queue.hpp`): zero
  in-tree consumers at the baseline (reverse-include empty). T1: no
  external-use signal observed through 2026-10-13; UNKNOWN, not zero. T2: this
  slice is the package mutation and re-records the disposition. T3: F2/F3/F4
  final dispositions exist and the four-app retirement is complete, so
  scheduler.hpp's installed-includer landscape no longer depends on
  `application_runtime.hpp`. T4: this packet.
* **Family B** (`future.hpp`, `wait_policy.hpp`): the #475 necessary condition
  — `group.hpp` and `evented_wait_policy.hpp` leaving the installed closure —
  was met at the F3 part-C v4 cut. No independent surviving dependency,
  implementation or consumer remains: `wait_policy.cpp` defined only
  `default_wait_policy()`, and `future.hpp`'s last in-tree reference was F4's
  historical retained probe, which is pinned to its own SHA.
* **Family C** (ten headers) plus the scheduler substrate: the retained
  legacy Scheduler had no remaining consumer outside the removal family after
  the four-app retirement; the canonical archive objects reference none of it
  (per-member undefined-symbol audit, `closure-manifest.json`); the W-04
  candidate host uses only `Fiber`/`FiberCtx`, which stay installed. A and C
  exit together in one separable dependency family, as #475 recorded.
* **D-13 test seams** (`detail/mutex_test_seam.hpp`, `detail/queue_test_seam.hpp`,
  `src/async/scheduler_fe2_test_seam.cpp`): zero defined symbols in both ring
  profiles; test-only infrastructure that rode the `src/async/*.cpp` glob into
  the production archive.
* **M-A4 experimental tree** (`include/sluice/experimental/`, `src/experimental/`):
  zero consumers, built by no target, never installed; the final-tree
  disposition owed to F5 is removal.

## Per-surface rows

| Surface | Header or object | Retired API | Migration or no equivalent | Policy | Owner |
|---|---|---|---|---|---|
| F5-A-COND | `sluice/async/condition.hpp` | `AsyncCondition` wait/notify over `AsyncMutex` | no equivalent: runtime-aware public sync is outside v1 (HOST-01); `std::condition_variable` remains the application-side tool | ACCEPT_BREAKAGE_NOW (proposed) | PENDING |
| F5-A-SEM | `sluice/async/semaphore.hpp` | `Semaphore` acquire/release | no equivalent (HOST-01) | ACCEPT_BREAKAGE_NOW (proposed) | PENDING |
| F5-A-QUE | `sluice/async/async_queue.hpp` | `AsyncQueue<T>` bounded MPMC port + push/pop result types | no equivalent (HOST-01); applications own their queues | ACCEPT_BREAKAGE_NOW (proposed) | PENDING |
| F5-B-FUT | `sluice/async/future.hpp` | `Future<T>` completed-return cell, `complete_with`/`await` | canonical `Request<T>` + `RequestScope`; the retained adapter that needed Future no longer exists | ACCEPT_BREAKAGE_NOW (proposed) | PENDING |
| F5-B-WPOL | `sluice/async/wait_policy.hpp`, `src/async/wait_policy.cpp` | `WaitPolicy`, `default_wait_policy()` | no equivalent: the evented-wait admission it served left with `evented_wait_policy.hpp` | ACCEPT_BREAKAGE_NOW (proposed) | PENDING |
| F5-C-SEL | `sluice/async/select.hpp`, `select_fwd.hpp`, `detail/select_port.hpp`, `detail/select_registration.hpp`, `src/async/select*.cpp` | `select(Scheduler&, cases...)`, `SelectResult`, event/timer select cases | no equivalent: multi-source suspension was scheduler runtime surface (MIG-02 runtime row) | ACCEPT_BREAKAGE_NOW (proposed) | PENDING |
| F5-C-SYNC | `sluice/async/{wait_queue,wait_node,timer_registration,mutex,lock_guard,thread_annotations}.hpp` | `WaitQueue`, `WaitNode`, `WaitResume`, `TimerRegistration`, `Mutex`, `LockGuard`, TSA annotations | internal scheduler substrate with no remaining consumer (MIG-02: retain internal substrate only when needed) | ACCEPT_BREAKAGE_NOW (proposed) | PENDING |
| F5-C-EVT | `sluice/async/event.hpp` | runtime-aware `Event` set/reset/wait | no equivalent (HOST-01); application eventfds/futures remain | ACCEPT_BREAKAGE_NOW (proposed) | PENDING |
| F5-S-AMTX | `sluice/async/async_mutex.hpp`, `async_rwlock.hpp` | `AsyncMutex`, `AsyncRwLock` scheduler-driven locks | no equivalent (HOST-01) | ACCEPT_BREAKAGE_NOW (proposed) | PENDING |
| F5-S-SCHED | `sluice/async/scheduler.hpp`, `src/async/scheduler*.cpp` (10 TUs) | the whole legacy multi-worker `Scheduler` runtime (fiber scheduling, park/wake, wait registry, timer pump) | no equivalent by design: the multi-worker runtime is not a v1 product (MIG-02); the OPTIONAL_CANDIDATE `StackfulIoHost` covers the optional single-owner W-04 profile and does not depend on it | ACCEPT_BREAKAGE_NOW (proposed) | PENDING |
| F5-C-PORT | `sluice/async/detail/{queue_item,queue_port}.hpp`, `src/async/queue_port.cpp` | `QueuePort`, `QueueItemFactory` opaque queue substrate | no consumer survives Family A | ACCEPT_BREAKAGE_NOW (proposed) | PENDING |
| F5-D13 | `sluice/async/detail/{mutex_test_seam,queue_test_seam}.hpp`, `src/async/{mutex_test_seam,queue_test_seam,scheduler_fe2_test_seam}.cpp` | test seam hooks and their zero-symbol archive members | test-only infrastructure internalized by removal; the canonical `RequestCore` observation seams remain under `SLUICE_ASYNC_INTERNAL_TESTING` in `detail/request_core.hpp` | ACCEPT_BREAKAGE_NOW (proposed) | PENDING |
| F5-FF | `sluice/async/detail/fail_fast.hpp` | 19 retired `[[noreturn]]` diagnostics (select/scheduler/async-mutex/rwlock/condition/wait-queue/evented families) plus `evented_admission_check` and its helper (21 entities) | none: they described the removed state machines; the 15 retained diagnostics (context, request, scope, host, threadpool, uring, identity) are unchanged | ACCEPT_BREAKAGE_NOW (proposed) | PENDING |
| F5-TST | `tests/runtime_waiter_observer_test.cpp` + target | the legacy-runtime waiter/observer seam test | its subject is retired; RequestCore observer protocol coverage lives in `request_core_protocol_test`/`request_core_publication_test`/`public_request_test` | ACCEPT_BREAKAGE_NOW (proposed) | PENDING |
| F5-IDNT | `sluice/async/detail/context_identity.hpp` (de-install only) | installed availability of the library-private substrate header | no API change: the header stays in the tree; no installed header includes it, so no external consumer can lose compilation through the canonical closure | ACCEPT_BREAKAGE_NOW (proposed) | PENDING |
| F5-EXP | `include/sluice/experimental/`, `src/experimental/` | pre-v1 experimental uring IoContext/write-batch (never installed, never built) | canonical `uring_backend.hpp` is the supported surface | ACCEPT_BREAKAGE_NOW (proposed) | PENDING |
| F5-W04FIB | `sluice/async/fiber.hpp` (retained host closure) | Scheduler-era surface with zero retained users: `friend class Scheduler;`, `CompletionWaitOutcome`, `set_entry`, `cancel_token`, `cancel_state`, `execution_tag`, `completion_wait_outcome` and their setters/members | none needed by the host: `stackful_io_host.cpp` uses Fiber's core entry/resume surface only | RETAIN_WITH_HOST (recorded residual) — removing it is a further per-surface break on the pending W-04 host | PENDING (owner W-04 host disposition) |

No compatibility shim, deprecated alias, forwarding wrapper or substitute
runtime is added: any of those would keep the retired authority compiled into
the package, which is exactly what this slice removes.

## Negative probes binding the break

Ten probe sources are written against the removed entry points
(`b01-v6-candidate/*/old-entry-*.log`): scheduler-lifecycle, semaphore-acquire,
condition-notify, queue-push, future-await, wait-policy, async-mutex-lock,
rwlock-write, event-set, select-await. Each compiles against the
**before-headers** (placed first on the include search path) and links against
the **F5 library**: at the baseline every probe compiles, links and (for the
constructor-real probes) runs; at the F5 head every link fails with
`undefined reference` naming the removed archive symbol. The twenty-three
removed headers additionally fail standalone compilation with
`No such file or directory` in the after prefix, with before-prefix presence
recorded as the control.

## #475 exit gates E1–E5

| Gate | Defined as | Status in this candidate |
|---|---|---|
| E1 | explicit owner decision accepting unknown external use/breakage, or evidence of a completed bounded deprecation window | **PENDING** — this packet is the request; no deprecation window was run for these surfaces (F0 §1.5: unknown use is not zero use) |
| E2 | exact adopted non-admission / `replacement=N/A` disposition referencing root authority | **SATISFIED (recorded)**: every row above carries its `replacement=N/A` or equivalent form and cites MIG-02/HOST-01 (see the per-surface table's Migration column) |
| E3 | correct phase sequencing and authorization | **SATISFIED (sequencing) / PENDING (authorization)**: F2→F3→F4 closed before F5; this slice is the F5 candidate and physical removal awaits the owner authorization above |
| E4 | consumer, build, evidence and claim dependency audit | **SATISFIED (recorded)**: reverse-include audit (`closure-manifest.json`), archive/object/symbol audit, docs/support-claim audit (`w04-negative-support-audit.md`), and the residual register (`closure-manifest.md` §Residuals) |
| E5 | versioned B-01 dual-profile freeze, clean-room probes, negative controls and rollback evidence | **SATISFIED (recorded)**: B-01 v6 four-configuration capture with before-prefix controls, installed probes both macro views, and the four rollback commits below |

## Rollback units

Each family is an independently revertible commit on the candidate branch:
`f4904d5a` (Family A), `f5b4f6e4` (Family B), `0524114c` (scheduler/C/seams
closure), `549bad34` (install-surface + experimental contraction).

## Owner decision requested

```ini
DECISION_TYPE = F5_SCOPE_BOUND_INSTALLED_BREAK
BASE_MASTER_SHA = 14a27a83821a94e6fc5a9684b48fcd296d7de891
CANDIDATE_HEAD = a23a94cdfbfa90f6ad6aff2d1b6706c305a4245c

FAMILY_A_PROPOSED_DISPOSITION = ACCEPT_BREAKAGE_NOW (remove from tree and package)
FAMILY_B_PROPOSED_DISPOSITION = ACCEPT_BREAKAGE_NOW (remove from tree and package)
FAMILY_C_PROPOSED_DISPOSITION = ACCEPT_BREAKAGE_NOW (remove the closure with A)

EXACT_HEADERS_TO_REMOVE = 23 installed headers removed from the tree, 1 header
  de-installed from the package only (list in closure-manifest.json
  installed_headers.COMPAT_INSTALLED_NONCANONICAL_REMOVE_CANDIDATE plus
  detail/context_identity.hpp)
EXACT_OBJECTS_TO_REMOVE = 17 archive members (14 legacy runtime TUs + 3 D-13
  seam TUs; one object and member each; list in closure-manifest.json
  archive_members)
EXACT_SYMBOL_FAMILIES_TO_REMOVE = Scheduler / AsyncMutex / AsyncRwLock /
  AsyncCondition / Semaphore / AsyncQueue / Future / WaitPolicy /
  default_wait_policy / Event / SelectResult+select / WaitQueue+WaitNode /
  TimerRegistration / Mutex+LockGuard / QueuePort+QueueItemFactory / the
  scheduler/select/condition/wait-queue/evented fail-fast diagnostics (asserted
  absent from the after archives in b01-v6-candidate/reproduce.py)

UNKNOWN_EXTERNAL_USE = UNKNOWN — accepted-breakage risk requested, not zero
SOURCE_RELINK_BREAKAGE = accepted-breakage risk requested (source and relink)
NO_SHIMS = YES

RETAINED_CONSUMER_EVIDENCE = W-01/W-02/W-03 consumers and 8 contract families
  link and run from the installed prefix in both ring profiles (B-01 v6
  consumer rows)
NEGATIVE_PACKAGE_PROBES = 23 removed-header compile negatives + 10 old-entry
  relink negatives with before-prefix positive controls (raw-package-evidence
  tar); installed-probes removed-header absence with before-prefix control
B01_VERSIONED_CAPTURE = v6 (F5_B01_V6_CANDIDATE_FRESH_PACKAGE_PASS,
  32/30/49 -> 15/13/25 both profiles)
ROLLBACK_UNIT = f4904d5a (A), f5b4f6e4 (B), 0524114c (C + seams),
  549bad34 (install surface + experimental), each independently revertible
INDEPENDENT_TECHNICAL_VERDICT = see independent-review.md (final verdict line)

OWNER_APPROVAL = PENDING
MERGE_AUTHORIZATION = PENDING
```

Approving this packet approves every row above as a breaking change, accepts
unknown external source/relink breakage for exactly these surfaces, and
authorizes merge of the reviewed candidate once SHA consistency, independent
technical acceptance and CI are green. It is not a blanket approval for future
public-interface removals, and it does not by itself claim W-04 support or v1
release readiness.
