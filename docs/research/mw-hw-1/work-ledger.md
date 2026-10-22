# MW-HW-1 work-elimination ledger (userspace redundancy census)

Cardinalities: C = `request_capacity` (slots, router, delivery), D = live
depth, S = scope capacity (= D in the harness), L = ledger length, K = owed
events, P = pending cancel controls, Q = queries per request/pass.
Frequencies are per request unless marked /pass.

## Census table (current master `8e2aa5ab`, uring W-02)

| # | Work | Where (file:line) | Cardinality | Frequency | Classification |
|---|---|---|---|---|---|
| W1 | Full router scan for pending controls | `uring_transport.cpp:666` `service_pending_controls_locked_` | `[scan C]` | **2/op** (submit via `dispatch_after_accept:88`, every pass via `poll:622`) | hot; P=0 in every non-cancelling workload ⇒ pure waste |
| W2 | Dispatch-queue mediation push→immediate drain→remove | `uring_transport.cpp:72,201` | O(1) push + `[scan D+shift]` remove | 1/op each | hot but tiny at low D (~25-35 instr/op) |
| W3 | Reverse router lookup per CQE | `uring_transport.cpp:551` | `[scan C]` worst | 1/CQE | hot; ~1 probe at D=1 (LIFO reuse clusters live entries at top); grows with D (H1 refuted for product; domain narrowing) |
| W4 | Publication drain lock pairs | `uring_backend.cpp:630-647` | (pass_bound+1) dispatch-mutex pairs/pass | ~2/op at D=1 | warm; ThreadPool already uses single-lock snapshot shape |
| W5 | Per-op core-mutex acquisitions | `request_core.cpp` (each op = own lock) | ~12/op (reserve, accept, claim, offer, release_exec, begin/complete_pub, claim/retire_obs, consume, outstanding, health) | 12/op | semantic floor candidates individually; each guards a distinct phase transition |
| W6 | Scope first-free/resolve scans | `request_scope.cpp:46`, `request_scope.hpp:114` | `[scan S]` | 2-3/op | hot at S=1 trivial; H3 free-list REJECTED (M1) |
| W7 | ThreadPool owed scans without count | `threadpool_backend.cpp:567-572,599` | `[scan C]` ×2/pass | 2/pass | warm; H2b was uring-only — same candidate class as E1, unmeasured here |
| W8 | Clock reads in wait path | `async_io_context.cpp:589`, `request_scope.hpp:152`, `progress_source.hpp:112` | O(1) | 2-3/op | vdso 4.2% of user cycles; bounded-wait deadline math even when no park happens |
| W9 | `delivery_` owed sweep | `uring_backend.cpp:653-676` | `[scan C]` only when K>0 | 0 in data-op workload | eliminated by H2b (merged); zero-op census confirms 0 syscalls |
| W10 | Settlement/occupancy/poison scans | `async_io_context.cpp:457+`, `uring_transport.cpp:337+` | `[scan N]`, `[scan L×C]` | per-context teardown / failure only | cold — correctly off the per-request path |

## H-K1 — pending control scan (W1): confirmed

`service_pending_controls_locked_` scans all C router entries looking for
`in_use && control_state==none && control_pending`. `control_pending` becomes
true ONLY in `issue_running_cancel_locked_` (645) when a running cancel could
not get an SQE even after a flush attempt AND sticky intent is retained —
SQE pressure + cancellation, absent from every non-cancelling workload. All
set/clear/reset paths audited: set at 645; cleared at 623 (control already
prepared), 658 (control issued); entries reset in `retire_router_entry_`
(559) and initialized in `dispatch_one_locked` (191). Sticky semantics are
retry-based: the scan IS the retry mechanism, so the scan cannot be deleted —
only skipped at P=0 (exactly the H2b shape, BACKEND-04 derived summary).
Measured cost at C=1024: 41.4% of user-space cycles (perf attribution),
1.3–1.8 µs/op.

## H-K2 — submission mediation (W2 + submit path): measured negligible

From acceptance to SQE: validate → core reserve → prepared store → delivery
init (lock) → core accept → dispatch_after_accept (lock): push → drain
(get_sqe → claim → prep → router → ledger → remove_exact) → submit (enter) →
pending-control scan → retry check. Lock counts: access ×1, dispatch ×2,
core ×3 — no repeated locking of the same state beyond the two dispatch-mutex
sections (delivery init and dispatch), which guard different invariants.
The push→drain→remove round trip costs ~25–35 instructions at D=1; the E2
falsifier (direct dispatch bypassing the queue) measured wall ratio
0.99–1.04 (no win, within noise) and carries real restructuring hazards
(`remove_exact`-after-claim is a fail-fast invariant; a failed direct attempt
after accept can strand an accepted reservation if poison intervenes — the
queue is what poison's drain recovers). Verdict: mediation is cheap and
load-bearing. KEEP.

## H-K3 — kernel boundary floor: 1 enter/op is the floor for this shape

Census: exactly 1 `io_uring_enter` per op at every depth; park/poll/read
eventfd cold; zero-op is 0 syscalls. System CPU (72% of total at 4k/D1/C1024
on base) = enter + inline tmpfs read + CQE post + eventfd signal + WSL2
scheduler accounting (~1 involuntary switch/op, WSL2 artifact). No batchable
SQE backlog exists in the sliding-window steady state, so DEFER/BATCH of
submits cannot reduce enter/op below 1 without changing the wait shape
(submit+wait combined enter) or SQPOLL — both are contract/config changes,
not work elimination. Verdict: semantic/physical floor for W-02-on-tmpfs;
`KERNEL_ATTRIBUTION_UNAVAILABLE` for finer splits.

## Non-candidates audited (negative controls)

- **W3 router direct index (H1)**: remains refuted for product (domain
  narrowing 2^48/≤32768, M2 `MICROBENCH_ONLY`); no new evidence.
- **W4 publication single-lock snapshot (E3 falsifier)**: measured 0.97–1.01
  at 4 KiB (median ≈1.00 at D=1, 0.974 at D=64/C=64 with overlapping ranges).
  Saves 1–2 uncontended mutex pairs/op — below WSL2 noise. NO_CHANGE as a
  standalone perf change; could ride along with a future poll() restructure
  for symmetry with ThreadPool.
- **W5 core-mutex coalescing**: each acquisition guards a distinct published
  phase transition with its own ABA/permission check; merging them creates a
  wider critical section spanning sink callbacks (forbidden: on_ready never
  under lock) or duplicates core state machines. Not pursued.
- **W6 scope free list (H3)**: refuted in M1 (+15–21% on FIFO shape).
- **W8 clock-read elision**: ~4% of user cycles lives in vdso clock reads
  driven by the harness's bounded `wait_for` deadline; Sluice-side elision
  (skip deadline math when unbounded) saves <0.5% end-to-end. NO_CHANGE.
- **Eventfd per-COE kernel signal when nobody parks**: kernel-side cost,
  unattributable on WSL2; suppressing it (lazy registration) changes W-03
  wake semantics. INVESTIGATE on native Linux only if kernel attribution
  shows a material share. Not a candidate here.
