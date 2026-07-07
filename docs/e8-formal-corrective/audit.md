# E8-FORMAL-CORRECTIVE — Production Authority Audit

Scope: verify the accepted production protocol (§0 of the corrective brief)
against current HEAD `44b9b7f`, then determine whether the ADR / TLA+ /
refinement-map authority model matches as-built production.

Expected production C++ diff: **NONE** (this is a corrective closure of the
formal/docs authority model, not a runtime repair).

## Baseline (frozen)

```
HEAD                      44b9b7f474f99b957cfb77b9a9774584d550f978
E8 production commit      44b9b7f  async: transfer runnable Fiber ownership on steal (E8-B/C)
E8 formal/docs commit     133bb97  spec: E8-0 ownership audit + ownership-transfer TLA+ model + ADR §9.3
git status --short        (clean except untracked .mimocode/)
git diff --name-only      (empty)
```

Documents audited:
- `docs/adr/ADR-execution-model.md` §9.3 (lines 802-1020)
- `docs/spec/e8_ownership_transfer/E8OwnershipTransfer.tla`
- `docs/spec/e8_ownership_transfer/E8OwnershipTransfer.cfg`
- `docs/spec/e8_ownership_transfer/E8OwnershipTransferBuggyOwner.tla`
- `docs/spec/e8_ownership_transfer/E8OwnershipTransferBuggyOwner.cfg`
- `docs/spec/e8_ownership_transfer/README.md`

Production audited:
- `src/async/scheduler.cpp`
- `include/sluice/async/scheduler.hpp`
- `include/sluice/async/fiber.hpp`

## Authority table (as-built production)

| Fiber phase | authoritative runtime fact | production representation | writer | reader |
| ----------- | -------------------------- | ------------------------- | ------ | ------ |
| Runnable    | runnable ownership + runnable ticket location | `fiber_owner_[F]` + `WorkerState::local_runnable` ticket | `spawn` (sched.cpp:73), `spawn_on` (:99), `run()` distribute (:147), `try_steal` (:665) | `try_steal` eligibility check (:656-657); wake routing does NOT read this |
| Running     | current execution Worker | `g_worker` (TLS, sched.cpp:28) / `WorkerState::current` (run_next_on, :430) | `run_next_on` sets `ws->current` (:430); `worker_loop`/thread sets `g_worker` (:167,:178) | `await_*` reads `g_worker` to capture `WaitReg.owner` (:563,:577,:591); `fiber_entry_bridge` (:40) |
| Waiting     | wait-epoch resume owner | `WaitReg.owner` in `waiting_size_/void_/ready_` maps (sched.hpp:135-138) | `await_*` stores `WaitReg{me, ws}` with `ws = g_worker` (:568,:582,:596) | `wake_ready_completions_locked` (:461,:478), `wake_ready_flags_locked` (:496) |
| Done        | detached | `FiberState::done` (absorbing); no owner/ticket/reg required | `fiber_entry_bridge` → `make_done()` (fiber.cpp) | none |

## A1 — Meaning of `fiber_owner_[F]`

`fiber_owner_[F]` is the **runnable ownership record**: it records which
Worker's `local_runnable` queue currently holds the runnable ticket for `F`.
It is read ONLY by:
- `try_steal` (sched.cpp:656-657) — to verify the victim still owns the
  stealable ticket (`oit->second != victim` ⇒ skip).
- `owner_of` / `owner_id_of` (sched.hpp:120-128) — TEST/DEBUG diagnostic only,
  explicitly not a runtime-policy input.

It is NOT read by any wake path. Evidence (grep of all `fiber_owner_` reads):
```
include/sluice/async/scheduler.hpp:122   (owner_of diagnostic)
src/async/scheduler.cpp:656              (try_steal eligibility)
src/async/scheduler.cpp:657              (try_steal eligibility)
```
The header comment (scheduler.hpp:206-215) states it is "updated by suspend"
and "the wake path reads this." **Both claims are false as-built** (see A3/A4).

So the answer is: **runnable ownership / steal-consistency record** — NOT
"global lifetime owner," NOT "current execution owner," and NOT the wake-
routing authority.

## A2 — Running authority

Which Worker executes `F` is proven by `g_worker` (TLS, sched.cpp:28) and
`WorkerState::current` (set in `run_next_on`, sched.cpp:430). `run_next_on`
sets `ws->current = fiber` (:430), runs the context switch (:436), and on
return clears it (:438). Inside a Fiber body, `await_*` reads `g_worker`
(:563,:577,:591) and `ws->current` (:564,:578,:592) to identify the executing
Worker and Fiber. This is genuine Worker-local state — one Worker per OS thread.

## A3 — Value stored in `WaitReg.owner`

`WaitReg.owner = g_worker` captured at suspend time. Evidence per await path:

`await_completion_size` (sched.cpp:562-574):
```
WorkerState* ws = g_worker;        // :563  <-- the current executor
Fiber* me = ws->current;           // :564
...
waiting_size_[...] = {me, ws};     // :568  <-- WaitReg.owner = ws = g_worker
```

`await_completion_void` (sched.cpp:576-588): identical pattern
(:577,:578,:582).

`await_ready_flag` (sched.cpp:590-608): identical pattern
(:591,:592,:596).

`WaitReg.owner` is therefore **g_worker at suspend time** — i.e. the Worker
that is *about to* suspend the Fiber (the current executor). It is NEVER
`fiber_owner_[F]` (no `await_*` writes `fiber_owner_`), NEVER a "historical"
owner, and NEVER the initial owner if a steal intervened. It is captured
fresh at each suspend.

## A4 — Wake authority (route destination)

For every wake path, the route destination is determined by `it->second.owner`
(=`WaitReg.owner`), NOT `fiber_owner_`:

`wake_ready_completions_locked` (sched.cpp:453-489):
```
WorkerState* owner = it->second.owner;   // :461 (size), :478 (void)
...
route_runnable_locked(f, owner);          // :467, :482
```

`wake_ready_flags_locked` (sched.cpp:491-507):
```
WorkerState* owner = it->second.owner;   // :496
...
route_runnable_locked(f, owner);          // :499
```

`route_runnable_locked` (sched.cpp:509-532) pushes `f` to `owner->local_runnable`
(:527). There is NO read of `fiber_owner_` anywhere in the wake/route path.
Grep confirms: zero `fiber_owner_` references in lines 442-532.

**Conclusion: wake routes by `WaitReg.owner`, the captured wait-epoch resume
owner. Wake does NOT read `fiber_owner_` as its primary routing authority.**
This matches the §0 "accepted production evidence" exactly.

## A5 — Owner-record consistency

| claimed relation | status | evidence |
| --- | --- | --- |
| Running: `fiber_owner_[F] == g_worker` | **structurally implied on the happy path, NOT checked** | `fiber_owner_` is set at spawn to the worker that receives the ticket (:73) and not mutated until steal. The worker that pops and runs `F` is the owner of the queue the ticket sat on. So at Running entry they agree. But there is no assertion, and `await_*` does not refresh `fiber_owner_` to `g_worker`. The agreement holds only because the pop path consumes the owner-local ticket (the ticket was on `owner[f]`'s queue). |
| Waiting: `fiber_owner_[F] == WaitReg.owner` | **NOT explicitly asserted; structurally implied iff no steal, but can be STALE after steal+resume** | `WaitReg.owner = g_worker` at suspend. If `F` was stolen, `fiber_owner_[F]` was updated at steal to the thief (:665), the thief pops and runs `F` (so `g_worker` = thief at suspend), thus `WaitReg.owner` = thief = `fiber_owner_[F]`. They agree. BUT: `fiber_owner_` is the *runnable* owner record; once `F` is Waiting, `fiber_owner_[F]` is stale (it still names the last worker that owned a runnable ticket for `F`) and is not refreshed. It happens to equal `WaitReg.owner` because the lifecycle is linear, but nothing enforces it. |

So: **not explicitly asserted**; **structurally implied** on the linear
runnable→running→waiting path; **not checked**; and `fiber_owner_`'s meaning
in Waiting is "last runnable owner" (a stale-but-convenient fact), not the
routing authority.

## A6 — One owner or multiple state-indexed authorities?

**MODEL-STATE-INDEXED.** Production uses distinct representations across
phases:

- **Runnable ownership**: `fiber_owner_[F]` (mutated by steal) + the ticket's
  `local_runnable` location. This is the steal-consistency record.
- **Running authority**: `g_worker` / `WorkerState::current`. Execution context.
- **Waiting resume authority**: `WaitReg.owner` (captured `g_worker` at
  suspend). This is what wake reads.

These are **three different production representations** that agree at
lifecycle transition boundaries (they must, for wake to route correctly) but
are not a single field. The current TLA+ model collapses them into one
`owner[f]` variable that `WakeReady` reads directly — which is the refinement
ambiguity. Production wake reads `WaitReg.owner`; the model's `WakeReady`
reads `owner[f]` and thereby implicitly claims wake reads the mutable owner
record. **That is the drift.**

## Verdict of the audit

**Production is CORRECT** and matches the §0 accepted evidence. The ADR §9.3
(esp. §9.3.5 last paragraph and §9.3.6), the TLA+ model (single `owner[f]`
read by `WakeReady`), the refinement-map row for `WakeReady` (claims "owner
read from current owner record"), and the scheduler.hpp comment block
(lines 206-215) all drift from production by describing wake routing as if
`fiber_owner_` is the routing authority. As-built, `WaitReg.owner` is.

Expected production C++ diff: **NONE**. No runtime change is needed. The
corrective work is confined to the formal model, ADR, refinement map, the
misleading scheduler.hpp comment, and reproducible verification.
