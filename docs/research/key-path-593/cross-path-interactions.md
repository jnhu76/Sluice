# Cross-path interactions

Four lanes are not four independent benefit curves. They are one coupling:
**who performs the kernel flush, and who pays for the progress pass.** This
document records the structural edges, the measured evidence, the interaction
verdict, and the measured combination. Percentages from different arms are
never added.

## 1. K2 submit rhythm ↔ K4 SQE batch

**Structural edge.** Both lanes modify the same statement:
`UringAsyncBackend::dispatch_after_accept` calls `submit_transport_locked()`
inline at `src/async/uring_transport.cpp:87`. K2-a deletes the call; K4-b wraps
it in `if (submitter_drives_progress_locked_() && core_->outstanding_summary() > 1)`.
They are not two stackable mechanisms — they are two gates on one mechanism.

**Measured enters/op (corrected against the archived censuses):**

| Form | K2-a (ungated) | K4-b (gated) | ratio |
|---|---:|---:|---:|
| W02-d4 | 0.25007 | 0.50017 | 2.00× |
| W02-d16 | 0.06257 | 0.12577 | 2.01× |
| W02-d64 | 0.01572 | 0.03447 | 2.19× |
| W02-64k-d16 | 0.06317 | 0.13267 | 2.10× |
| W02-d1 | 1.00007 | 1.00007 | 1.00× |
| W03-d4 | 0.25015 | 1.00007 | 4.00× |
| W03-d16 | 0.06257 | 1.00007 | 15.98× |
| W03-d64 | 0.01572 | 1.00007 | 63.60× |

The ≈2× ratio in the same-thread forms is the gate price: K4-b keeps each
window's first submission inline and adds one flush per pass. In W-03 the ratio
is unbounded in practice because the topology predicate keeps **every**
submission inline; that is exactly the regression-avoidance mechanism.

> **Draft correction.** The cross-path synthesis draft listed K2-a W-03 at
> 1.00007. The archived `ab/K2-a/census/census-summary.txt` shows 0.06257 /
> 0.25015 / 0.01572 for W-03 d16/d4/d64. The correct statement is: K2-a defers
> in the split-owner form too (and pays the tail cost), while K4-b's topology
> gate does not defer there. The interaction conclusion (same mechanism, two
> gates) is unchanged; the draft's ratio table was wrong for W-03. See
> [measurement-review.md](measurement-review.md) §9.2.

**Interaction verdict: strong coupling, not independent, not additive.**
Same-thread CPU falls from −32.20 % to −28.07 % (d16), −37.66 % to −35.53 %
(d64), and −26.31 % to −13.06 % (d4) as the gate is tightened. Recovering the
d4 difference would require a third gate (window state), i.e. a new mechanism
that must be built and justified separately.

## 2. K4 CQE batch ↔ K1 lock handoff

**Structural edge.** `reap_cqes` already peeks in batches
(`src/async/uring_completion.cpp:200`) but calls `handle_one_cqe` per CQE, each
taking `dispatch_mtx_` (`:100`); the publication drain takes one lock per entry
(`src/async/uring_backend.cpp:649-652`); `deliver_event` takes two core locks
per event (`:610-614`).

**Measurement.** CQEs/peek = 11.76 (d16), 28.88 (d64): the reap side is already
batched. K1 removes **per-event** locks, not per-batch locks: −2.000
claim/retire − 0.889 pop − 0.209 fold = −3.098 acquisitions/op in W-03 (W-02:
−3.124). The measured end-to-end effect is 2.29 %; the independent bound on the
whole mutex population is the 3.4 % of cycles (~370 cycles/op).

**Interaction.** Two counteracting code-level effects were predicted:

1. K4-b's gate adds one `core_->outstanding_summary()` read (core mutex,
   `src/async/detail/request_core.cpp:93-96`) per submission whose topology
   predicate is true — including the inline D=1 branch — while K1-b removes two
   core acquisitions on the same mutex. The net core-lock reduction in a
   combination could therefore be ~1.0/op rather than 2.0/op.
2. K4-b lowers passes/op slightly (d16 poll/op 0.0515 → 0.0429, wait/op
   −30.48 %), shrinking K1-a's per-pass fold share, while K1's per-event locks
   are a 1-CQE/op invariant and do not shrink.

**Measured combination (both effects together).** See §5. The net result is
that W-02 gains nothing over K4-b (+0.31 % CPU, CI [+0.16,+0.65]), so effect 1
plus 2 is either absent or below the resolution of the design; and the W-03 K1
effect survives (−2.97 % vs K4-b). The predicted "gate eats K1" interference is
**not** observed at the level the design could resolve.

## 3. K1 publication strategy ↔ K3 wakeup

**Structural edge.** K1's publication/deliver path does not call
`signal_ready_progress`. In steady state, uring completions are advertised by
the kernel eventfd registered at `uring_backend.cpp:259-283`; user-space signals
appear only on residue/poison/cancel/zero-op/admission transitions
(`uring_transport.cpp:98-101`).

**Measurement.** K1 arm vs baseline: undistorted futex ≈0.27/op in both arms;
`perf sched` context switches 313 vs 302 (≈0.005/op); W-02 user signals 0/op;
W-03 notifications 0.084/op unchanged. **K1's lock elision does not change the
wake economy.**

**The real coupling is K4's deferral, not K1.** A deferred flush must be
advertised by the existing `unsubmitted_after_dispatch` residue signal
(`uring_transport.cpp:115-122`). In W-02 that is ≈1 user eventfd write per op at
d16 (census), absorbed by the next same-thread pass: polls −16.34 %, waits
−30.48 %, parks do not rise. In W-03 the same signal wakes the driver: the
ungated arm shows polls/notifications +51.6 %/op and p99 +15.70 %; the
depth-gated (pre-topology) arm shows driver passes 0.314 → 0.507/op (+61 %),
eventfd reads 0.628 → 1.013/op (+61 %), user signals 0 → 0.770/op, CPU +34.46 %
at d4.

**Interaction verdict:** K1 is wake-neutral; K4's deferral is the wake-cost
migration source and its sign is set by topology. The K4-b topology predicate
removes the W-03 migration (enters/op 1.00007, poll/notification pair median
+0.00 %).

## 4. K3 driving ↔ K4 kernel enter

**Structural edge.** In W-02 the submitter is the progress owner, and
`wait_one` runs `run_progress_pass_` before parking
(`src/async/async_io_context.cpp:561-684`); the pass-start
`submit_transport_locked()` (`uring_backend.cpp:619-634`) is the flush point.
In W-03 the driver is another thread woken by epoll on the eventfd
(`bench/bench_w03_external_loop.cpp:172-258`), and the flush lands in the
driver's pass.

**Measurement.** W-02 d16: poll/op ≈0.05, zero parks in 540,000 ops (24 eventfd
reads, 0 `poll(2)`). After deferral: enters 0.1258, polls −16.34 %, waits
−30.48 %, no new parks. W-03: every pre-topology deferral variant regresses;
K4-b with the topology predicate returns to baseline (enters 1.00007, CPU
+0.30 % CI crossing zero, p99 +0.38 % CI crossing zero).

**Interaction verdict: the driving topology is the switch for the flush cost.**
Same thread: the flush is absorbed by a pass that would run anyway. Split
owner: the flush becomes an extra pass plus an eventfd round trip, whose fixed
cost (eight non-contended locks, plus epoll/drain) exceeds the removed
submitter enter. K3-b's withdrawal is supported by the same evidence: the
split-owner `access_mtx_` contention has no removable blocking amplification
(futex 0.27/op, context switches 0.005/op), and the real cost is the driver-pass
mechanism.

## 5. Measured composition (K4-b + K1(a+b))

This is the only semantically compatible combination direction (K2-a and K4-b
cannot coexist; the others have no material gain). One merge worktree, one new
measurement — not a sum.

| Comparison | CPU | wall | p50 | p95 | p99 | throughput |
|---|---:|---:|---:|---:|---:|---:|
| W-02 d16 combo vs baseline | −27.87 % [−28.73,−27.30] | −28.04 % | −33.48 % | −39.24 % | −38.99 % | +38.98 % |
| W-02 d64 combo vs baseline | −36.13 % [−36.32,−35.76] | −36.17 % | −41.55 % | −49.68 % | −47.99 % | +56.66 % |
| W-03 d16 combo vs baseline | −2.57 % [−3.27,−2.08] | −2.64 % | −2.38 % | −1.98 % | −3.95 % [−5.49,+0.55] | +2.71 % |
| W-02 d16 combo **vs K4-b** | +0.31 % [+0.16,+0.65] | +0.32 % | +0.22 % | +0.14 % | −0.31 % | −0.32 % |
| W-03 d16 combo **vs K4-b** | −2.97 % [−3.42,−2.24] | −2.99 % | −2.35 % | −1.77 % | −2.00 % [−3.14,−0.89] | +3.08 % |
| A/A W-02 / W-03 | +0.59 % / −0.31 % | +0.62 % / −0.33 % | −0.10 % / −0.35 % | — | −0.42 % / +0.51 % | — |

The combo has **210/210 valid runs**, of which **60 are the increment
comparison** (7 pair directories × 30 runs; 5 baseline comparisons = 150 and 2
increment comparisons = 60). It is not "210 plus 60".

Census: W-02 enters 0.12577 / 0.03447, vctx 4,985 / 1,228; W-03 enters 1.00007,
vctx 12,881 (base 13,337), `instr:u` −8.3 %, `cycles:u` −6.0 %. W-02 `instr:u`
−5.5 %, `cycles:u` −3.5 % vs baseline. No cost-migration signature: context
switches fall in both topologies; W-03 cache-miss:u rises 0.49 → 0.51/op and
LLC-load-misses 0.026 → 0.037/op, neither offsets the effect.

**Interpretation.**

- The W-02 effect belongs to K4-b. K1 adds nothing measurable there; the
  measured increment is +0.31 % (slightly worse, CI excluding zero on the
  positive side). This is a measured non-additivity, not a modelled one.
- The W-03 effect belongs to K1 and survives on the K4-b base (−2.97 % vs
  K4-b, CI [−3.42,−2.24]; 15/15 CPU/wall pairs). The K4-b topology gate keeps
  W-03 inline, so the K1 lock elision remains the only active mechanism.
- The predicted core-lock interference is real in code but not resolvable in
  this design: the W-02 increment is flat, and the W-03 increment is close to
  K1's standalone effect.
- Both are below the 4.79 % gate as *components* of the combination; the
  combination is classified `MECHANISM_GAIN` / `MECHANISM_ONLY_GAIN`.

**What the combination does not fix:** the 64 KiB p99 regression (K1 is
payload-neutral fixed lock work; the combo was not measured at 64 KiB) and the
W-03 d4 +2.54 % (not measured in the combo).

## 6. Merged judgment

```
submit-side enter (K2/K4)
   -> absorbed by the same thread's pass (K3 driving)        => W-02 gain
   -> or migrated to a driver pass + eventfd (K3 driving)    => W-03 CPU/tail regression
   -> changes the CQE batch per pass (K4)                    => changes K1 per-pass share
   -> K1's lock elision is capped by acquisition count (K1)  => small W-03 effect only
```

- K2 and K4 are the same mechanism with different gates.
- K3 is the cost-migration channel of that mechanism.
- K1 is the downstream lock handoff; structurally real, magnitude-capped.
- The composition confirms topology complementarity: the W-02 win is K4-b's
  and the W-03 win is K1's, measured separately in one binary, never summed.

**Systemic gap:** form dependence. The same mechanism is −28 %…−36 % in the
same-thread 4 KiB pipeline, neutral in the split-owner form under the topology
gate, and a p99 regression at 64 KiB. Any product proposal must carry a
three-condition gate (same-thread, outstanding > 1, payload) and a re-measured
combination; K4-b implements two of the three.
