# Critical-path atlas

Classification per brief §1. Cost is always from the **full** binary; frequency
is from the observe binary with its +3–15 % overhead disclosed. The measured
domain is warm tmpfs, 4 KiB positional reads, `taskset -c 2`, unless stated.

| Class | Definition used here |
|---|---|
| `HOT_PATH` | executes on every logical operation |
| `CRITICAL_PATH` | removing or delaying it changes end-to-end CPU/wall/latency, proven by a paired A/B |
| `WAIT_PATH` | the mechanism exists to block/wake; its cost is paid only when it runs |
| `AVOIDABLE_WORK` | executes with no pending semantic obligation **and** a legal short path exists |
| `UNAVOIDABLE_SEMANTIC_WORK` | required by a v2 obligation; removal is a semantic break |
| `OVERLAPPED_WORK` | executes concurrently with the measured thread on the same hardware resource |
| `MOVED_WORK` | cost relocated to another thread, phase or resource by a candidate |

## 1. CRITICAL_PATH

| Mechanism | Location | Measured proof |
|---|---|---|
| Kernel submission boundary: `io_uring_submit` → `io_uring_enter` + io-wq dispatch + `rep_movs` copy + CQ eventfd | `src/async/uring_transport.cpp:218-235`; `src/async/uring_completion.cpp:200` | Removing one inline enter per op gives −13 %…−36 % CPU and wall on same-thread 4 KiB (K4-b, D=4/16/64), −26 %…−38 % ungated (K2-a); micro-measured fixed cost ≈1,792.5 core cycles/enter on the valid rounds (1,815 is the TSC run-3 differential; f is 0.91 on TSC but ≈0.37 on core cycles). Kernel share of cycles is 76.3 % (d16) to 82.6 % (d1) |
| Same-thread submit→wait alternation at D=1 | `src/async/async_io_context.cpp:561-684` | Delaying the flush past the pass boundary without a gate regresses D=1 by +14.12 % CPU and p50 ×14.1 — the submit/completion overlap is the critical resource there |
| Cross-thread handoff in the split-owner form | `bench/bench_w03_external_loop.cpp:172-258` | Moving the flush into the driver's pass costs more than the removed submitter syscall: W-03 d4 CPU +34.46 %, p50 +34.97 % in the depth-gated arm; the topology gate in K4-b removes the regression |

**Atlas statement.** The only user-space mechanism proven to be *on* the
critical path is the *frequency* of the kernel submission edge. All other
user-space mechanisms in K1–K4 are hot but either cheap, overlapped, or capped
by an authority that cannot be removed.

## 2. HOT_PATH (executes every op, not proven critical)

| Mechanism | Per-op frequency | Why it is not critical |
|---|---|---|
| Completion chain `handle_one_cqe` → router → `offer_terminal` → `release_execution` → publication push/pop → `begin/complete_publication` → claim/retire observer → `take` | 1 CQE, 1 terminal, 1 publication, 1 take | Whole user-space budget is 2,572 cycles/op at d16 against 8,293 kernel cycles/op; removing 3.1 of its acquisitions changes W-03 by 2.29 % and W-02 by nothing |
| Submit chain validate → `reserve` → prepared write → delivery init → `accept` → `dispatch_one_locked` → SQE prep → router → ledger → `remove_exact` | 1 each per op, identical at every D | Fixed per-op cost; only the enter inside it is material |
| Router lookup | 1 lookup/op, 8.5 probes at d16, 32.4 at d64 | #580 closed `NO_TRIGGER_NO_CHANGE`; ≤2.28 % user cycles at the worst artificial shape |
| `RequestCore::mutex_` fixed transitions | 10/op + ~2/pass | ~370 cycles/op across *all* mutex sites (3.4 % of total cycles) |
| `dispatch_mtx_` | 2 submit-side + ~2.5 completion/pass | Same 3.4 % envelope |
| Ledger append/pop | 1/1, zero reconciles in steady state | Physical accounting obligation (BACKEND-03) |

## 3. WAIT_PATH

| Mechanism | Location | Measured state |
|---|---|---|
| `wait_if_unchanged` park: progress mutex ×4 + `read(eventfd)` + `poll(2)` | `include/sluice/async/detail/progress_source.hpp:108-232` | W-02: **0 parks** — 540,000 ops with 24 eventfd reads and 0 `poll(2)`; every pass finds a completion on tmpfs |
| W-03 driver epoll | `bench/bench_w03_external_loop.cpp:199-207` | 0.063 epoll waits/op (d16); kernel eventfd aggregation ≈12 ops per wake; `empty_wakes = 0.0000` in every uring arm |
| 10 ms bounded retry | `src/async/async_io_context.cpp:22,632-652` | Never fires in steady state; requires injected EAGAIN/EBUSY |

The wait machinery is dormant in the measured domain. This is a property of
warm tmpfs, where a completion is a memory copy. It is **not** evidence that
the park path is cheap on a real device; that domain is unmeasured.

## 4. AVOIDABLE_WORK

| # | Mechanism | Removed per op | End-to-end result | Status |
|---|---|---|---|---|
| ① | One independent `io_uring_enter` per op (inline flush in `dispatch_after_accept`) | 1.00007 → 0.12577 (d16), → 0.03447 (d64), → 0.50017 (d4); 64 KiB → 0.13267; D=1 and W-03 unchanged (gate) | W-02 d16 −28.07 % CPU / −28.08 % wall / p99 −39.97 %; d64 −35.53 %; d4 −13.06 % | **material**; only deployable under the form gate |
| ② | No-observer `claim_observer_delivery` + `retire_observer_delivery` | 2.000 core acquisitions/op | part of K1's −2.29 % W-03; null on W-02 | real but sub-threshold |
| ③ | Per-entry publication pop lock | 0.889 acquisitions/op | part of the same 2.29 % | real but sub-threshold |
| ④ | Pass-tail same-lock repeated acquisitions (backend summary, core health + outstanding, progress health + exhausted) | 3 acquisitions/pass; 4.2/op at D=1 | W-02 d1 −1.00 % (9/15, in band) | real but unmeasurable |
| ⑤ | Per-CQE reap lock pair + `cqe_seen` khead store | 1 lock pair + 1 store/CQE | no arm | upper bound below threshold (all mutex 3.4 %) |
| ⑥ | Redundant `closed`/`access` precondition branch in the backend | 2 predictable branches | no arm | <1 ns/op; not a candidate |

Only ① crosses the campaign 4.79 % materiality band (whose provenance is
disclosed in [measurement-review.md](measurement-review.md) §4).

## 5. UNAVOIDABLE_SEMANTIC_WORK

| Obligation | Root / rule | Mechanism |
|---|---|---|
| Exactly-once terminal choice | REQ-03, HANDLE-01 | `offer_terminal` |
| Borrow retirement before release | LIFE-01 | `release_execution` |
| Observer delivery state machine for armed observers | OBS-02, OBS-03 | `claim_observer_delivery` / `retire_observer_delivery` when armed |
| Publication boundary (release/acquire) | REQ-04 | `begin_publication` / `complete_publication` |
| `delivery_` init before `accept` | cancellation interleaving (see DAG §2) | `uring_backend.cpp:488-500` |
| Reserve/accept admission and rollback | REQ-02 | `reserve`, `accept`, generation |
| Kernel/ledger correspondence, partial submit, poison | BACKEND-03 | ledger append/pop/reconcile |
| CQE identity routing | TERM-01 | router entry (closed by #580) |
| Cancellation, control, poison recovery | CANCEL-01, SHUT | control references, Class-A retirement |
| Bounded pass and honest residue | PROG-01 | entry snapshot, `dispatch_retry_remains` |
| No lost wake | PROG-02 | epoch token + eventfd + probe |
| Shutdown quiescence | SHUT-01/02 | destructor witnesses |
| Host hook never under locks | THREAD-02 | publication queue + lock-free `publish_one` |

Deleting any of these is a semantic break, not an optimization. The K1 result
is the strongest evidence that the *separation* is structural: the arm removed
3.098 acquisitions/op while keeping every obligation, and 70,061 W-02 traced
passes showed `published == reaped` with zero violations (W-03 has aggregate
push/pop parity only).

## 6. OVERLAPPED_WORK

| Work | Evidence | Consequence |
|---|---|---|
| io-wq worker copy chain (`shmem_file_read_iter` → `copy_page_to_iter` → `rep_movs_alternative`) on the same logical CPU as the application thread | worker = 32.5–40.4 % of all cycles, main thread 58.4–66.3 % (two 0/1 W-02 d16 `perf record` runs); `rep_movs_alternative` 12.0–14.2 % of all cycles | User-space cycle savings do not translate 1:1 into wall; the copy worker fills the CPU. This is why the K2/K4 gain is 28 % rather than the 15.8 % enter-path share alone |

`OVERLAPPED_WORK` is not `MOVED_WORK`: the copy is a physical obligation
already scheduled on the same core. It cannot be deleted; it can be
amortized (the batched enter reduces io-wq per-item scheduling too).

## 7. MOVED_WORK (measured migrations)

| Migration | Evidence | Verdict |
|---|---|---|
| K2-a W-03: submitter syscall → driver pass + eventfd | polls/notifications +51.6 %/op, W-03 d16 p50 +10.55 %, p99 +15.70 %, 0/15 p99 improvements | cost migration; arm rejected in that form |
| Depth-gated K4 (pre-topology) W-03 d4: submitter enter → driver pass | driver passes 0.314 → 0.507/op (+61 %), eventfd reads 0.628 → 1.013/op (+61 %), user signal 0 → 0.770/op, CPU +34.46 % | cost migration; topology gate added |
| Depth-gated K4 (pre-topology) W-03 d16: same channel, smaller | passes +25 %, p50 +19.02 %, p99 +48.96 %, CPU −0.38 % | cpu-neutral but tail regression |
| K4-b 64 KiB d16: enter → per-op copy/tail | enters −86.7 % but `cycles:u` +7.9 %, `instr:u` +5.7 %, p99 +7.46 % | payload-dependent migration; needs a payload gate |
| K4-b W-03 d4: enters unchanged | vctx +5.2 %, `cycles:u` −3.5 %, CPU/wall +2.54 %/+2.57 % | **unattributed** small positive deviation, not deferral |
| Combo K4-b+K1(a+b) W-03: none | vctx −3.4 %, ictx −2.4 %, context switches fall; cache-miss:u +3.4 % (0.49 → 0.51/op) | no migration signature; the K1 effect survives |

Migrations 1 and 2 are the reason K2-a is not deployable and K4-b needs a
topology predicate. Migration 4 is the reason a payload gate is still open.

## 8. One-line atlas

> The real critical path is the kernel boundary — one `io_uring_enter`, the
> io-wq dispatch, and the copy on a worker that shares the application's
> logical CPU. The user-space coordination layers are hot, provably partly
> redundant, and far too cheap relative to the kernel share to matter on their
> own. Cost moves along one channel — who owns the progress pass — and the sign
> of every candidate is decided by topology, depth and payload, not by the
> mechanism itself.
