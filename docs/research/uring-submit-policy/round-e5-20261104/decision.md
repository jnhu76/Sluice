# decision.md — frozen gate adjudication and verdict

## 1. Gate-by-gate against the frozen thresholds

Thresholds are the frozen ones ([protocol.md](protocol.md) §6); none was
revised after results were seen.

| Gate | Result | Evidence |
|---|---|---|
| Correctness | **PASS** | 11/11 deterministic oracles green; 4/4 named mutants RED (BASE GREEN → MUTANT RED → RESTORED GREEN); B-mode uring suite 13/13; A-mode suite 79/79 applicable; race 23/23; integration 21/21; asan+ubsan lane green — `bench .../correctness/*.log` |
| Mechanism | **PASS** | enters/op 3.0002 → 0.5630 (−81.2 % ≥ 50 %); ledger reconciles (20,000/20,000 submitted = completed, 0 errors, 0 SQ-full retries). Scope note: total-enter count (submit+wait combined); the ≥ 50 % submit-family reduction survives the worst-case archived-data bound (reanalysis receipt §3); pure-submit split is an observation gap |
| Net win | **PASS** | CPU/op −26.46 % (≤ −10), CI upper −26.02 % (≤ −5); single accounting source so the unfavorable-attribution clause has nothing to shift; secondary whole-invocation rusage −26.76 % agrees in sign |
| Performance protection | **FAIL** | main shape passes (throughput +36 %, p95/p99 −31/−29 %), but **D2** exceeds frozen bounds: p95 +5.69 %, p99 +7.45 % (> 5 %), throughput −3.99 % (> 3 % drop), Holm-rejected, replicated, noise floor 0.5–1.6 pp. D2 CPU (+4.17 %) is within the 5 % CPU bound. W-03 d4 p95 / d64 p99 not-establishable (power, means ≈ 0) — disclosed, not counted as harm |
| Resource | **PASS** | peak RSS +0.00 % (19,764 KiB both arms); no new queue/thread/timer/per-request allocation; no new Core lock (reuses `live_cookies_` atomic + ledger size) |

## 2. Verdict

**`KEEP_IMMEDIATE`** — the policy as specified is not deployable. D3, D16 and
saturation pass every gate and are recorded as E5-scoped research support for
the mechanism; the shallow-depth cell (D2) blocks adoption under the frozen
gates. The immediate path remains the product default.

**A supported mechanism is not a deployable policy.** The positive D3/D16/
saturation results do not outweigh the D2 protection failure: the frozen gate
is conjunctive over the control family, and D2 is exactly the workload regime
(the shallow fallback) a submission policy must not damage.

`PRODUCT = HOLD` regardless of the technical verdict: #604 has not handed off
`READY_FOR_595` (verified 2026-11-04). `PRODUCT_PR = NONE`; the research branch
carries no product change; `MERGED = NO`.

### Frozen verdict block (verbatim, as produced by the round)

```text
BEGIN_SLUICE_URING_SUBMISSION_POLICY
BASE_SHA: 271b81bfc9d64eecc0266fe7066b9040525dd710
E5_HOST_IDENTITY: Xeon E5-2666 v3 (10C/20T, 1 NUMA), kernel 7.2.11-200.fc44, gcc 16.2.1, liburing 2.13-2, native/bare-metal, warm tmpfs, schedutil 1.2-3.5 GHz, perf paranoid=2; taskset -c 2 default, SMT {2,12} and cross {2,8} checks
POLICIES_COMPARED: A·IMMEDIATE (frozen 271b81bf) vs B·BOUNDED_PROGRESS (same binary, SLUICE_595_ARM=B): strict most-recent-driver equality + regular 4 KiB read + kernel-visible un-retired witness (live_cookies_ > ledger size) + hold cap 8; flush at pass start / cap / SQ-full / ineligible arrival; existing control ordering; no new queue/thread/timer/per-request allocation; ring flags unchanged (no SQPOLL).
P1_SHAPE_GATE: PASS on D3/D16/saturation, FAIL at D2. D1 +0.09% CPU / +0.48% p99; D2 +4.17% CPU [+3.53,+4.83], +5.69% p95, +7.45% p99, -3.99% tput (repl +3.05/+4.98/+5.81/-2.95; A/A noise floor 0.53/0.61/1.61/0.53 pp); D3 -6.81% CPU / -11.57% p95 / tput +7.34%; 64 KiB d16 -0.72% CPU; W-03 d4/16/64 ~0 with enters 3.0002 unchanged (owner gate).
P2_FIRST_FLUSH: retained, not eliminated. No kernel-visible un-retired witness (first of a burst / after silence) always flushes inline; the witness is the promptness witness. First-flush elimination (P2) not attempted this round.
P3_NOTIFICATION_COALESCING: not attempted (P3 out of scope). Per-deferral residue signal kept as conservative redundancy under strict owner gating; no coalescing claimed. eventfd registrations unchanged (4-9 per run).
P4_GATE_COST: no RequestCore lock added. Gate reads one relaxed atomic (live_cookies_) plus ledger size under dispatch_mtx_; driver identity is a thread-id pair recorded in poll(). Cost visible only where batching cannot happen: D2's penalty is added waiting (no enter saved), not the gate read; D1/W-03 show ~0.
DEFERRED_LEDGER_ORACLE: 11 deterministic oracles: deferred last item settles without a second submit; cap-8 flush; deferred multi-entry partial submit (rc<pending) prefix reconcile + retry; cancel of a held request; pending control intent serviced by an existing pass; owner/witness/payload gates; deterministic prepark handoff (no sleep); poison with held batch; shutdown with held batch. 4 named mutants BASE GREEN -> MUTANT RED -> RESTORED GREEN (NO_CAP, IGNORES_WITNESS, IGNORES_OWNER, IGNORES_PAYLOAD).
WAKE_RETRY_ORACLE: prepark handoff uses two blocked-pipe kernel witnesses plus a PauseGate; held residue flushed before the gate with ledger==0 and sq_ready==0 at the gate, wake attributed to the held release only; pass-before-park plus the existing 10 ms dispatch retry cadence bound a parked owner. No sleep-based synchronization anywhere.
CANCEL_OVERFLOW_SHUTDOWN: cancel of a held request flushes immediately and settles exactly once; mixed-owner oracle splits immediate vs deferred deterministically; overflow/poison/shutdown/stale-CQE/slot-reuse covered by the unchanged B-mode suite (13/13; race 23/23; integration 21/21); asan+ubsan lane GREEN (oracles + race 23/23).
D1_D2_D3: D1 neutral (PASS). D2 FAILS the frozen CPU/p95/p99/throughput bound; real (tight CI, 0.5-1.6 pp noise floor), replicated, and explained at syscall level: enters 3.0002 -> 3.0002, i.e. one held request per pass with zero amortization. D3 passes and improves (-6.81% CPU, -11.57% p95, throughput +7.34%).
PAYLOAD_TAILS: 64 KiB d16 neutral: CPU -0.72% [-2.03,+0.60], p95 +0.02%, p99 -0.00%, tput -0.82%; the 4 KiB payload gate prevents the K4-b 64 KiB p99 +7.46% regression. No unexplained payload-tail regression accepted.
W02_W03_MIXED_DRIVER: W-02 4 KiB d16, 24 balanced pairs: CPU/op -26.46% [-26.89,-26.02], wall -26.49%, tput +36.07%, p50/p95/p99 -32.13/-30.77/-29.23%; rusage secondary -26.76%; placement cpu8 replicates (-25.16% CPU). W-03 split-owner d4/16/64 within +/-1.6% (safety band), enters identical 3.0002; {2,12} +0.86%, {2,8} +1.58% CPU (placement noise, no enter change).
ENTERS_PER_OP: 3.0002 -> 0.5630 (W-02 4K d16, -81.2%); d3 3.0002 -> 2.0004; saturate 3.0013 -> 0.5759; d2 0%; 64 KiB 0%; W-03 0%. Submitted = completed = 20000, 0 completion errors, 0 SQ-full retries; TransportLedger prefix reconciliation exercised by the partial-submit oracle.
CPU_WALL_LATENCY_AB: CPU/op 2925.8 -> 2151.6 ns; wall 58.5 -> 43.0 ms per 20k-op rep; 341,739 -> 464,952 ops/s; p99 74.0 -> 52.4 us; PMU (3 interleaved reps/arm, 100% enabled): cycles -21.8%, instructions -17.4%, task-clock -26.4%, context-switches -60.0%, migrations/page-faults neutral.
REGRESSION_VERDICT: FAIL on D2 only (real). W-03 d4 p95 and d64 p99 not-establishable due to their own A/A noise floor (3.88 / 7.21 pp; means ~0) — power, not evidenced harm. Main shape, D3, D16, saturation and 64 KiB pass all protection bounds. Peak RSS +0.00%. Unmeasured: per-request submit delay (bounded indirectly by max submit->take latency ~0.1-0.2 ms and by the deterministic prepark oracle; far below the 10 ms parked cadence); kernel CPU outside the process (unrelated kworkers); no #578 application trace.
PRODUCT_API_CHANGE: NONE
RESEARCH_PR: NONE at experiment time (read-only brief; isolated worktree never pushed; archival record: this archive + the #595 writeback comment)
PRODUCT_PR: NONE
MERGED: NO
DECISION: KEEP_IMMEDIATE
END_SLUICE_URING_SUBMISSION_POLICY
```

(Note added at archival time: D1_D2_D3 line's phrase "FAILS the frozen
CPU/p95/p99/throughput bound" is shorthand for the conjunctive control bound;
precisely, D2 CPU is within the 5 % CPU bound while p95/p99/throughput exceed
their bounds — see [results.md §2.1](results.md).)

## 3. What is supported vs what is not

Supported (E5 scope): deferral with owner+payload+witness gating and cap 8
preserves semantics (oracles), cuts total enters 81.2 % on the target shape,
and improves CPU/wall/tails there and at D3/saturation; gates hold exactly on
all non-D2 control shapes.

Not established by this round: deployability (D2 fails), submit-delay
distribution (uninstrumented), pure-submit enter split, real-workload transfer
(tmpfs only), W-03 d4 p95 / d64 p99 non-inferiority (power).

## Next research entry

All four are `FUTURE_HYPOTHESIS / NOT_TESTED` — none may be cited as a result
of this round.

- **RQ1 — Batchability admission.** Can currently-observable state identify
  requests that will actually amortize an enter, so D2 stops deferring? The
  leading candidate is a "second held entry" guard (defer only when the ledger
  already holds ≥ 1): at D=2 the first hold would then stay inline and the
  witness-only hold disappears. Must be re-gated by the full control family.
- **RQ2 — Observability limit.** At D2 and D3 the current observable state is
  identical (witness present, 4 KiB same-owner read); is there a structural
  limit making the two futures indistinguishable from userspace, or does a
  cheap observable (e.g. sq_ready, io-wq pending count) separate them?
- **RQ3 — Submission-delay accounting.** Measure per-deferred-SQE true wait
  time and who owns the final flush (pass, cap, prepark, pressure). Requires
  instrumented deferral timestamps; also enables a pure-submit enter ledger.
- **RQ4 — Real-workload transferability.** Reproduce or refute the effect on
  #604's device filesystems and application traces; tmpfs 100 %-punt behavior
  is a boundary condition of this round (the #622 C4 result shows the punt
  fraction — and therefore the io-wq share of the win — varies by FS).
