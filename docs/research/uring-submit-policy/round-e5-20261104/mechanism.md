# mechanism.md — arm B as built, its guarantees, and why D2 fails

Recovered from the research patch
(`bench/research/uring-submit-policy/round-e5-20261104/patch.diff`, sha256
`7be9d0145a8c268a62d9fa90338b7cda95cefc5f8ad2976e4706cb192fb1eaf0`) against
base `271b81bf`, not from the report. The patch touches 6 files:
`src/async/uring_transport.cpp`, `src/async/uring_backend.cpp`,
`include/sluice/async/detail/uring_backend.hpp`, `bench/bench_w02_pipeline.cpp`
(D2/D3 case selector only), `xmake/tests.lua` (595 + 4 mutant targets), and
new `tests/uring_595_bounded_progress_test.cpp`.

## 1. Arm selection

`UringAsyncBackend` reads `SLUICE_595_ARM` once at construction; `B` sets
`bounded_progress_ = true`. Absent or anything else = frozen A path. Two
members are added to the backend object: `bool progress_driver_seen_` and
`std::thread::id progress_driver_recent_{}` (plus the constant
`kDeferCap = 8` and the test seam `defer_cap_for_test()`). That is the entire
new state: no queue, no thread, no timer, no per-request allocation, no ring
flag change (no SQPOLL), no RequestCore lock.

## 2. The deferral predicate

In `dispatch_after_accept` (transport layer), the inline
`submit_transport_locked()` flush is replaced by a deferral decision.
`defer_transport_flush_locked_()` defers **only** when all four hold:

1. **Most-recent-driver equality** — the submitting thread is the recorded
   most recent progress driver. `poll()` records
   `progress_driver_seen_ = true; progress_driver_recent_ =
   std::this_thread::get_id()` under the dispatch mutex at the top of its
   critical section. Strict single thread-id equality; a second submitter
   thread or a stale driver forces the immediate path.
2. **Payload gate** — the prepared op is a regular 4 KiB read
   (`kind == read && native_length == 4096`). 64 KiB, writes, fsync, and every
   control op are ineligible.
3. **Kernel-visible un-retired witness** — `live_cookies_ > ledger.size()`:
   at least one previously accepted request is still visible to the kernel
   (submitted, un-retired). No witness = first submission of a burst / after
   silence → flush inline. The existing `live_cookies_` atomic is reused; no
   new counter, no lock.
4. **Cap** — fewer than 8 SQEs already held (`kDeferCap = 8`).

Held SQEs sit in the existing `TransportLedger` (append/pop/reconcile against
the kernel SQ head — no new structure). Flush happens at:

- the **next progress pass** — `poll()`'s existing
  `submit_transport_locked()` call, unchanged in position and ordering;
- the **cap** (8th deferral attempt flushes instead of holding);
- **SQ-full / recovery** — the existing eager flush on partial submit
  (`rc < pending`, TransportLedger prefix reconciliation, then retry);
- **any ineligible arrival** (control/cancel SQE, wrong payload, wrong owner) —
  the ineligible item itself takes the immediate path; the residue rides the
  next pass or the pre-park handoff;
- **pre-park handoff** — a parking owner flushes held residue before parking
  (deterministically exercised by oracle 9); the existing 10 ms dispatch retry
  cadence additionally bounds a parked owner.

Control/cancel SQEs keep the **existing** ordering. An earlier draft added a
cancel-time flush and a `pending_control_count_` gate; both were removed as
redundant (`service_pending_controls_locked_` runs identically in both arms and
its SQ-full recovery flushes eagerly). No first-flush elimination (P2), no
notification coalescing (P3): a per-deferral residue signal is kept as
conservative redundancy under the strict owner gate.

## 3. Progress and correctness guarantees

Oracles (11, deterministic, no sleep-based ordering —
`tests/uring_595_bounded_progress_test.cpp`, B-mode only by design):

| # | Oracle | Guarantees |
|---|---|---|
| 1 | last deferred item completes without a second submit | no lost-wakeup liveness hole |
| 2 | cap-8 flush held batch inline | bounded hold, batch actually submits |
| 3 | deferred multi-entry partial submit (`rc < pending`) reconciles prefix and retries | TransportLedger vs kernel SQ head agreement under partial flush |
| 4 | cancel of a held request flushes immediately, settles exactly once | cancel semantics preserved for held work |
| 5 | pending control intent serviced by an existing pass | no new control ordering introduced |
| 6 | owner gate splits immediate from deferred | most-recent-driver authority |
| 7 | witness gate keeps sequential submissions immediate | P2 first-flush behavior retained |
| 8 | payload gate keeps large reads immediate | payload containment |
| 9 | deterministic pre-park handoff (pause gate + two blocked-pipe kernel witnesses; ledger==0 and sq_ready==0 at the gate) | a parking owner cannot sit on held work |
| 10 | poison with held batch retires through failure | error propagation for held work |
| 11 | shutdown with held batch settles | teardown obligation |

Mutation receipts (BASE GREEN → NAMED MUTANT RED → RESTORED GREEN, each mutant
kills exactly its named gate; `correctness/mutant-receipts.log`):
`SLUICE_595_MUTANT_DEFER_NO_CAP`, `..._IGNORES_WITNESS`, `..._IGNORES_OWNER`,
`..._IGNORES_PAYLOAD`.

Broader suites unchanged and green in B mode: uring suite 13/13, A-mode suite
79/79 applicable, race suite 23/23, integration 21/21, asan+ubsan lane green
(`correctness/*.log`).

## 4. Why D2 fails (the measured boundary)

The mechanism amortizes `io_uring_enter` over *held* SQEs. What a depth-D
pipeline actually holds per progress pass is bounded by the arrival pattern:

| shape | enters/op A | enters/op B | held per pass (implied) | outcome |
|---|---|---|---|---|
| d1 | 3.0002 | 3.0002 | 0 | neutral (owner/witness gates close it) |
| **d2** | **3.0002** | **3.0002** | **1, with no batch mate** | **holds 1, saves 0, pays delay** |
| d3 | 3.0002 | 2.0004 | 2 | saves 1 enter/pass, net win |
| d16 | 3.0002 | 0.5630 | ~14.4 | saves ~2.4 enters/pass, large win |
| saturate | 3.0013 | 0.5759 | deep | CPU −35.7 % |

At D=2 exactly one request is in flight beyond the one being submitted, so the
witness gate admits the hold but there is never a second SQE to batch with:
the flush still happens on the next pass with a batch of one. Zero enter is
saved (3.0002 → 3.0002, strace) while the single held SQE waits one pass —
measured as CPU +4.17 %, p95 +5.69 %, p99 +7.45 %, throughput −3.99 %
(replicated +3.05/+4.98/+5.81/−2.95 %). The gate predicate *cannot distinguish*
this future from a D≥3 future using its current observable state — the same
witness condition is satisfied — which is the structural limit recorded as
RQ2 in [decision.md](decision.md). A depth/batchability guard (e.g. require a
second held entry before deferring) is the obvious candidate fix and is RQ1;
it was explicitly out of scope this round and was **not** tested or tuned post
hoc.

This is exactly the "cannot reduce enters at D=2" case the pre-registered P1
shape gate was designed to expose; the gate did its job.
