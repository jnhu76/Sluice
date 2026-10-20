# M0 Benchmark-Harness Bias Audit (W-02 pipeline)

Question: how much of what W-02 measures is Sluice, and how much is the
driver? The concern is directional in both ways: a heavy harness can smear a
real Sluice win, and a light harness can attribute harness savings to Sluice.

## 1. What the harness does per operation (default mode, `bench_w02_pipeline.cpp:322-438`)

| Step | Cost | Whose |
|---|---|---|
| `free_slots` pop/push (`:336,436`) | O(1) vector ops | harness |
| `scope.submit_read` (`:346`) → `RequestScope::reserve_slot_` `[scan S]` → `ctx_.submit_read` | production (scan is `request_scope.cpp:46-55`) | Sluice |
| `window.push_back` (`:356`) | O(1) deque push | harness |
| `scope.wait_for(oldest)` (`:382`) → `resolve_` `[scan S]` + `Request::ready` + `wait_one` pass | production | Sluice |
| `scope.take(oldest)` (`:394`) → `resolve_` `[scan S]` + `consume_public_result` | production | Sluice |
| `window.erase(window.begin()+victim)` (`:437`) | **O(1) in default mode** — `victim` is always 0 (`:395`); deque front erase | harness |
| `verify_pattern` (oracle pass only, `:421`) | excluded from timed phase (`run_one_repetition:523` runs `verify=false`) | harness |

The suspected "deque.erase per completion" cost is therefore **not** on the
primary path: default consumption is strictly oldest-first, and erasing
`begin()` from a `std::deque` is constant time. The O(depth) middle-erase
exists only in the `-anyready` diagnostic mode (`:370-377,397`), which is
diagnostics-gated (`SLUICE_BENCH_DIAGNOSTICS`), never part of the frozen G0
matrix, and excluded from throughput comparisons.

## 2. Residual harness bias in the primary mode

- **Scope resolve scans are production cost**, not harness bias: they run
  inside `RequestScope` (`request_scope.hpp:114-129`). The harness *shape*
  (oldest-first) keeps them short in expectation: freed low-index slots are
  reused first, so the oldest ticket tends to resolve near the array front.
  A consumer that takes results out of order would pay the full O(S) — the
  `-scope64` isolation case (`:640-663`) exists precisely to expose this and
  is re-measured in M1 (Layer B for H3).
- The saturating branch (`:236-317`) uses `vector<Inflight>` + in-order
  settle: no deque at all, no middle erase. Its numbers are a different
  shape (backpressure), correctly labeled `-saturate`.
- Latency instrumentation (`instrument=true`) runs as a *separate* phase
  (`:558`) whose numbers feed only the latency record, never throughput
  (`bench_common.hpp` time_ops separation; W-02 keeps its own
  `now_ns` deltas). No clock call sits in the timed region.

## 3. Harness-only control measurement (M1 Layer B companion)

To bound the harness's own contribution without changing the harness, M1
adds a `harness_only` mode to the isolated mechanism runner: it replays the
identical window bookkeeping (deque push, erase-front, free-slot stack) with
the Sluice calls replaced by no-ops of equal arithmetic shape. This gives an
upper bound on driver cost per op at each depth; it is *not* comparable to
Sluice numbers and is never folded into any A/B ratio.

Verdict for the receipt: **BENCH_HARNESS_BIAS_AUDIT = no material bias in
primary W-02 mode; O(depth) work confined to labeled diagnostic modes;
production scope scans honestly attributed to Sluice (candidate H3).**
