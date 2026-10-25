# Opportunity ranking

Ranking follows the eight dimensions in brief §13 and is based only on measured
evidence. Percentages are paired variant−baseline medians from the full binary;
negative is better. **Columns are independent measurements and must not be
summed.**

## 1. Unified research table

| Candidate | Path | Mechanism frequency | Avoidable work | CPU impact | Wall impact | Correctness risk | Decision |
|---|---|---|---|---|---|---|---|
| **K4-b** topology+depth-gated deferred flush | K4 submission→kernel boundary (crosses K2/K3) | `io_uring_enter` 1.00007 → **0.12577**/op (d16, −87.42 %), →0.03447 (d64, −96.55 %), →0.50017 (d4, −49.99 %), 64 KiB →0.13267 (−86.74 %); d1 and all W-03 forms 1.00007 (gate keeps inline); poll/op −16.34 %, wait/op −30.48 % | one independent enter per op and its fixed cost (micro upper bound ≈1,792.5 core cycles/enter on the valid rounds; the earlier 1,815/f≈0.91 pairing is retracted). Kernel-side amortization is **inherited K2 evidence** (worker −33.0 % on a 0/1 record), not measured on K4-b | W-02 d16 **−28.07 %** [−28.69,−27.34], d64 −35.53 %, d4 −13.06 %, d1 −2.50 %, sat −43.06 %; W-03 d16 +0.30 % (CI crosses 0), W-03 d4 **+2.54 %** (below the 4.79 % band, inside d4 A/A); 64 KiB −2.99 % | W-02 d16 −28.08 %, d64 −35.53 %, d4 −13.10 %; p50/p95/p99 −34.08/−39.27/−39.97 % (d16); W-03 d16 +0.29 %, d4 +2.57 %; 64 KiB −2.97 % with p99 +7.46 % | derived topology summary (2×`thread::id`+bool, non-authoritative); one `outstanding_summary()` core read and one `signal_ready_progress()` per gated/deferred submit; both flush choices complete; gate is a performance heuristic; 6 mutants RED→GREEN; 14 targets BASE GREEN + 7 key oracles; 390/390 runs valid; **D=2/D=3 unmeasured** | **`ELISION_PERF_SUPPORTED`** (form-gated, measured D=4/16/64) / `END_TO_END_GAIN`; productization preconditions open |
| **K2-a** ungated deferred submission | K2 submit→admission→dispatch (mechanism at the K4 boundary) | 1.00007 → **0.06257**/op (d16, −93.7 %), 0.01572 (d64), 0.25007 (d4), 0.06317 (64k); d1 1.00007; W-03 0.06257/0.25015/0.01572 (d16/d4/d64); poll/op −23.71 %, wait/op −38.42 % | same enter, plus the "first submission of the window" that K4-b keeps inline | W-02 d16 **−32.20 %** [−32.63,−31.62], d64 −37.66 %, d4 −26.31 %, **d1 +14.12 %** [+11.75,+14.58]; W-03 d16 −11.17 %, d4 −1.32 % (CI crosses 0), d64 −12.27 %; 64 KiB −3.05 %; sat −47.48 % | W-02 d16 −32.24 %, d1 +14.06 %; p50/p95/p99 −35.91/−46.66/−45.14 % (d16); W-03 d16 −11.22 % but p99 +15.70 %; 64 KiB −3.08 % with p99 +6.77 % | no new state (minimal patch), but **no gate**: D=1 p50 ×14.1, W-03 d16 p99 +15.70 %, W-03 d4 p50 +26.42 %, 64 KiB p99 +6.77 %; 5 mutation logs; 13 targets GREEN; 1 u2b re-pin + #585 flush re-pin; 390/390 valid | **`ELISION_CANDIDATE_FOUND`** (paired `END_TO_END_GAIN`); ungated arm not deployable, **not a product candidate**; same mechanism as K4-b, supported verdict counted once under K4 |
| **K1(a+b)** merged publication snapshot + no-observer delivery gate | K1 completion→publication→observer | completion-segment mutex acquisitions/op 3.209 → **0.111** (W-03 d16), 3.254 → 0.130 (W-02 d16), **−3.098 / −3.124**; W-02 trace `published == reaped` on 70,061 passes, 0 violations; W-03 aggregate push/pop parity only | 2.000 claim/retire core locks; 0.889 per-entry pop lock; 0.209 pass-boundary fold. Bound: all-mutex 3.4 % of cycles (~370 cycles/op), not the circular 3.1×22.8 ns | W-03 d16 **−2.29 %** (15/15, range −4.86…−0.99); W-02 d16 −0.43 % (null) | W-03 d16 −2.31 %; p50/p95/p99 −1.95/−1.54/−2.23 %; W-02 d16 −0.38 % but p95 +0.82 %, p99 **+3.07 %** | K1-b changes the installed detail header return shape (Owner review required); sink delivery is a separate obligation (two tests killed an over-elided build); 9 tests GREEN; no single-factor split; **no K1 mutant**; owed-scan lock hold unexercised | **`NO_CHANGE_JUSTIFIED`** under the campaign 4.79 % band; **INCONCLUSIVE** if the band is rejected (effect 15/15 and above the session band); revive only after a K4 product change |
| **Combo** K4-b + K1(a+b) | composition | W-02 enters 0.12577/0.03447; W-03 enters 1.00007; W-03 `instr:u` −8.3 %, `cycles:u` −6.0 % | K4-b's enter + K1's locks, each in its own topology | vs baseline: W-02 d16 −27.87 %, d64 −36.13 %, W-03 d16 −2.57 %; **vs K4-b**: W-02 **+0.31 %** (no gain), W-03 **−2.97 %** | vs baseline: −28.04 % / −36.17 % / −2.64 %; p50/p95/p99 −33.48/−39.24/−38.99 % (W-02 d16); vs K4-b: +0.32 % / −2.99 % | both component risks; one textual merge conflict (member insertion) resolved verbatim and archived; 16 suite targets GREEN; all six K4-b mutants still RED; **210/210 runs, of which 60/60 increment** | **`MECHANISM_ONLY_GAIN`** (`MECHANISM_GAIN` in campaign vocabulary); measured non-additive composition, not a candidate |
| **K3-a** pass-tail same-lock fusion | K3 progress pass tail | empty-pass mutex 8 → 5; d1 −4.2 acquisitions/op (passes/op 1.405) | three repeated same-lock acquisitions per pass | W-02 d1 **−1.00 %** [−3.78,+1.89] (9/15); d4 +0.54 % | d1 −0.98 %, d4 +0.52 %; p50 −1.97 % / −2.73 %; p95 −1.50 % / +3.54 %; p99 −1.43 % / +1.04 % | requires a new `dispatch_state_version_` atomic + 8 bump points; the unguarded version was killed by two payload oracles; mutation RED→GREEN; 11 tests GREEN, zero test changes | **`NO_CHANGE_JUSTIFIED`** |
| **K4-a** batch reap lock domain + batch `cq_advance` | K4 reap side | one `dispatch_mtx_` pair + one `cqe_seen` per CQE; CQEs/peek 11.76 (d16), 28.88 (d64) | per-CQE lock pair and khead store | **not measured** (no arm) | **not measured** (no arm) | no arm; lock scope ×batch ≤32 could lengthen W-03 submitter waits; **the 3.4 % mutex bound does not cover the `cqe_seen` store** | **`NO_CHANGE_JUSTIFIED`** (no arm; partial upper bound only) |
| **K3-b** drive-gate / `access_mtx_` decoupling | K3 driver gate × K2 acceptance | two `access_mtx_` per public drive call; cross-thread in W-03 | submitter `io_uring_enter` vs driver pass entry mutual exclusion | **not measured** | **not measured** | no arm; changes THREAD-01 driver ownership/acceptance serialization | **`NO_CHANGE_JUSTIFIED`** (premise refuted: futex 0.27/op, ctx 0.004–0.005/op) |

Closed and not listed as candidates (to prevent "one optimisation per lane" misreading):

- K2-A closed/access double check — branch-only, <1 ns/op.
- K2-B delivery-init merge — illegal (cancel can push a publication between
  accept and init; the reader would see the previous generation's kind).
- K2-C prepared/descriptor/borrow re-encoding — different lifetimes; BorrowFacts
  has no reader today but is a root-named obligation (open question).
- #580 router lookup — `NO_TRIGGER_NO_CHANGE`, not reopened.

## 2. Eight-dimension ranking

Numbers are ranks within each dimension (1 = best). "No arm" is stated, never
guessed.

| Dimension | K4-b | K2-a | K1(a+b) | K3-a | K4-a | K3-b |
|---|---|---|---|---|---|---|
| **D1 removes real work** | ② −87.4 %…−96.6 % enters (d16/d64), −50 % d4 | ① −93.7 %…−98.4 %, −75 % d4; same line, removes more | ③ 3.098 acquisitions/op | ④ 3 locks/pass | ⑤ upper bound only | ⑥ upper bound only |
| **D2 measured critical path** | ② same-thread submit→wait; removal changes CPU/wall by −28 % | ① same path, more removal, −32 %…−38 % | ③ W-03 −2.29 % (15/15) | ④ −1.00 % (9/15) — not proven critical | ⑤ no arm | ⑥ no arm |
| **D3 full-lifecycle CPU/op** | ② −28.07 %/−35.53 %/−13.06 % (d16/d64/d4) | ① −32.20 %/−37.66 %/−26.31 %; **+14.12 % d1** | ③ −2.29 % W-03, −0.43 % W-02 | ④ −1.00 % d1 | ⑤ not measured | ⑥ not measured |
| **D4 real wall/latency** | ② wall −28.08 %, p50 −34.08 %, p99 −39.97 % (d16); d64 p99 −48.49 %; 64 KiB p99 **+7.46 %** | ① wall −32.24 %, p99 −45.14 %; **D=1 p50 ×14.1**, W-03 p99 +15.70 %, W-03 d4 p50 +26.42 %, 64 KiB p99 +6.77 % | ③ W-03 wall −2.31 %, p50 −1.95 %, p99 −2.23 %; W-02 p99 +3.07 % | ④ d1 wall −0.98 %; d4 +0.52 % | ⑤ not measured | ⑥ not measured |
| **D5 new authority/complexity** (less is better) | ② 3 derived fields + one core read per gated submission + one `signal_ready_progress()` per deferred submit; non-authoritative | ① no new state, but needs an external gate to deploy | ③ K1-a adds two preallocated batch stores, `OwedDelivery`, and a persistent `observer_armed` bit (derived, bounded by `request_capacity`); K1-b changes an installed detail header | ⑤ new version atomic + 8 bump sites | ④ split `_locked` + preallocated batch; no arm | ⑥ split THREAD-01 gate/acceptance lock; no arm |
| **D6 added contention/coherence** (less is better) | ② vctx/ictx −72.3 %/−72.6 %; poll/wait down; LLC miss +0.007/op; W-03 unchanged | ③ vctx/ictx −78.4 %/−77.4 %; LLC 0.005 → 0.079/op; **W-03 polls/notifications +51.6 %** | ① removes 3.1 cross-thread acquisitions; futex/ctx unchanged | ④ 3 locks/pass saved, version read only | ⑤ no arm | ⑥ no arm |
| **D7 regressions in other standard workloads** (fewer is better) | ③ 64 KiB p99 **+7.46 %** (material); W-03 d4 +2.54 % (below band, inside d4 A/A, unattributed); d1 −2.50 % CI [−3.54,−1.45] (unexplained; cannot be a mechanism gain); W-03 d16 in band; threadpool +1.19 % is a **null control**, not a mechanism regression (the diff does not touch ThreadPoolBackend) | ④ four known: D=1 +14.12 %/p50 ×14.1, W-03 d16 tail +15.70 %, W-03 d4 p50 +26.42 %, 64 KiB p99 +6.77 % | ① two forms measured: W-03 non-regressing (15/15), W-02 CPU null but p99 **+3.07 %**; 64 KiB/saturate/cancel not measured | ② d1/d4 in band; coverage narrow | ⑤ not measured | ⑥ not measured; premise refuted |
| **D8 cross-host validation value** (higher is better) | ① highest: the mechanism is consistent with kernel-side amortization (inherited K2 evidence) and same-CPU worker serialization; must re-test on other cores/topologies | ② high: upper-bound form of the same mechanism; its regressions are also host-sensitive | ③ medium: 22.8 ns/acquisition is a cache-line cost; multi-socket may raise it but the count bounds the effect | ④ low-medium | ⑤ low | ⑥ low; premise already refuted |

### Final order and one-line justification

1. **K4-b** — the only candidate that removes measured critical-path work,
   improves CPU/wall/p99 materially, and gates the known regressions out. It is
   the only mechanism worth a productization pre-review. D1/D2/D3/D4/D6 are
   top-two; D5 adds derived state; D7's 64 KiB regression is explicit and
   gateable, while W-03 d4 and d1 are sub-threshold/unexplained.
2. **K2-a** — the ungated upper bound. Higher D1–D4 ranks, materially worse
   D5/D6/D7 (D=1 pure regression, W-03 tail migration, 64 KiB tail regression)
   and no gate. Its verdict is `ELISION_CANDIDATE_FOUND`; it prices the gate but
   is not deployed.
3. **K1(a+b)** — structurally real (−3.098 acquisitions/op, 70,061 W-02 passes
   zero violations) and directionally reproducible (W-03 15/15), but 2.29 % is
   below the campaign 4.79 % band; the independent bound is the all-mutex 3.4 %
   share. Threshold-dependent (`INCONCLUSIVE` if the band is rejected). Its
   measured survival on a K4-b base is the useful result; revival is
   conditional.
4. **Combo** — not a candidate: measured composition with no additive W-02 gain
   and a sub-threshold W-03 gain. It answers the interaction question, nothing
   more.
5. **K3-a** — largest signal −1.00 % (median inside the A/A median/range, range
   wider) and requires a new consistency mechanism; mechanism argument and
   measurement close together.
6. **K4-a** — no arm; the mutex bound is 3.4 %, but the `cqe_seen` store is not
   covered by that bound. K4-b's W-03 result locates the real cost in the
   kernel/driver, not in lock micro-work.
7. **K3-b** — no arm; premise refuted by three independent measurements.

## 3. Brief §13 questions, answered

**What did each lane find?**

- **K1:** the full completion→publication→observer lock/queue DAG, and two
  structurally removable handoffs (no-observer claim/retire, per-entry pop
  lock). Real and obligation-preserving; the effect (2.29 % W-03) is below the
  campaign 4.79 % band but above the session band.
- **K2:** the submit side's only material cost — exactly one `io_uring_enter`
  per op caused by the inline flush — and its end-to-end value (−32.20 % d16)
  together with the measured price of every gate. The arm is ungated, so the
  lane verdict is `ELISION_CANDIDATE_FOUND`; K4-b supplies the gate.
- **K3:** the wake economy is healthy: the machinery is dormant on warm tmpfs
  (0 parks, 0 user signals, 0 empty wakes) and amortized 1/D; the pass-tail
  locks are real but worth ~1 %.
- **K4:** the submit/reap asymmetry (CQE side already batched 11.8–28.9 per
  peek; SQE side never batched), and the only material elision
  (−13 %…−36 % CPU when the form gate holds).

**Which mechanisms cannot be bypassed?** Exactly-once terminal, borrow
retirement, observer state machine for armed observers, publication
begin/complete, delivery-init before accept, reserve/accept admission and
rollback, ledger append/pop/reconcile, router entry, cancel/control/poison
recovery, bounded pass, no-lost-wake, shutdown quiescence.

**Which are provably redundant?** No-observer claim/retire (2.0/op), per-entry
publication pop lock (0.889/op), pass-tail same-lock repeats (3/pass), per-CQE
reap lock pair (no arm), closed/access double check (<1 ns/op). Only the
per-op kernel enter is both provable and material.

**Which are locally expensive but not on the critical path?** All mutex
acquisitions (~370 cycles/op, 3.4 %); router probes (32/CQE at d64, ≤2.28 % user
cycles); the four slot-indexed arrays; and the io-wq `rep_movs` copy (12.0 % of
all cycles) — the last is a physical obligation and is *overlapped*, not
avoidable.

**Which optimisations moved cost elsewhere?** K2-a W-03 (submitter enter →
driver pass + eventfd: notifications +51.6 %, p99 +15.70 %); the pre-topology
depth gate in W-03 (passes +61 %, CPU +34.46 %); K4-b 64 KiB (enter → copy/tail:
`cycles:u` +7.9 %, p99 +7.46 %). The primary K4-b shape shows no migration.

**Is there a new systemic gap?** Yes — form dependence (topology × depth ×
payload). K4-b implements the first two gates; payload is not gated and has no
arm. Two secondary gaps: the mixed-driver predicate is a bounded approximation,
and the W-03 d4 +2.54 % is unattributed.

**What does the next round need?** (1) E5 depth: payload-gated variant, W-03 d4
attribution, cancel/control load, saturate latency instrumentation, mixed-driver
load, and the #585 flush-semantics redefinition for any product form. (2) A
second native host (multi-socket/NUMA) because both the kernel-side amortization
and the per-acquisition cost are host-sensitive. (3) #578 real-application
validation, because tmpfs results cannot be extrapolated to device I/O.
