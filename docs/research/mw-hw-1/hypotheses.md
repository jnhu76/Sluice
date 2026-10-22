# MW-HW-1 hypotheses, counterfactuals and candidate ranking

Charter order applied per candidate: DELETE → COALESCE → DEFER → BATCH →
SIMPLIFY → SPECIALIZE → KEEP. A (work removable) / B (semantics preserved) /
C (end-to-end gain) are separated throughout.

## TOP-1 — E1: skip empty pending-control sweeps (derived count)

- **Obligation**: sticky cancellation intents created under SQE pressure must
  be retried by later passes (`service_pending_controls_locked_`).
- **Existing physical work**: full `[scan C]` router walk, 2× per request
  (submit + pass), at P=0 in every non-cancelling workload. RouterEntry
  24 B × 2C per op; C=1024: 48 KiB, ~768 cache lines.
- **Counterfactual (COALESCE / derived summary)**: 8-byte
  `pending_control_count_` maintained under `dispatch_mtx_` at the audited
  transition set (set at the SQE-exhausted sticky site; clear at
  already-prepared, issued, and router-retire sites; idempotent on
  re-cancel). Skip the scan at 0. The flags stay authoritative; the count is
  never consulted by settlement/destruction (`internal_work_retired` and the
  destructor keep their own checks). Exactly the H2b pattern (BACKEND-04).
- **A (removable)**: proven by census + perf (41% of user cycles at
  C=1024/D=1) and the paired A/B (0.851 / 0.738 at C=1024).
- **B (semantics)**: 9 deterministic uring suites PASS incl. W-03 external
  loop and the sticky-intant conformance scenario; new parity oracle (4
  scenarios) PASS; both mutants CAUGHT. Increment-site completeness is the
  residual risk: the audited transition set is closed under
  grep-diffable inspection (all `control_pending` writes live in
  uring_transport.cpp under one lock), and the parity oracle + missing-
  increment mutant pin it. Remaining gate before product: `SEMANTIC_PROOF_
  REQUIRED` items — a reviewer-hardened enumeration of every future
  `control_pending` writer, plus W-03 A/B on native Linux.
- **C (end-to-end)**: −15% wall at 4k/D1/C1024, −26% at 4k/D8/C1024, −8% at
  64k/D8/C1024 on WSL2; whole CI below 1.0 at C=1024 configs.
- **Falsifier**: C=16 (0.984 — no gain where the scan is small) and 1 MiB
  (0.957, overlapping 1.0 — I/O-dominated). A P>0 burst (SQE pressure +
  cancel storm) pays one extra counter update per transition; the parity
  suite covers the transitions but no P>0 *timing* row exists (workload not
  constructible in this harness).
- **Recommendation**: PROPOSE_UPSTREAM (native-Linux A/B + W-03 A/B required
  first; per-PR gate per AGENTS.md).

## TOP-2 — E2 falsifier: dispatch-queue mediation is NOT worth eliminating

- **Hypothesis**: push→immediate-drain→`remove_exact` is a removable
  mediator.
- **Result**: A-falsified — the mediation costs ~25–35 instructions/op at
  D=1 (~0.09% of user instructions). Measured wall ratio 0.994–1.043 across
  all configs (no win). Removing it requires restructuring
  `dispatch_one_locked`'s queue-membership invariant (`remove_exact`-after-
  claim fail-fast) and re-solving accepted-but-undispatched stranding under
  mid-dispatch poison (the queue is what poison's drain recovers).
- **Decision**: NO_CHANGE_JUSTIFIED. The queue is load-bearing (SQE-pressure
  FIFO and poison recovery), not redundant mediation.

## TOP-3 — E3 falsifier: publication-drain lock pairs are NOT worth eliminating standalone

- **Hypothesis**: (pass_bound+1) dispatch-mutex pairs per pass can collapse
  to one (ThreadPool's snapshot shape).
- **Result**: measured 0.974–1.010 (median ≈1.00) — 1–2 uncontended mutex
  pairs/op is below WSL2 noise. The patch is semantically clean (same set,
  order, pass boundary; drive-guard prevents re-entrant use of the snapshot
  buffer) but earns nothing standalone.
- **Decision**: NO_CHANGE_JUSTIFIED as a perf change. Keep the shape in
  inventory for any future poll() restructure where it removes code rather
  than adds a buffer.

## NO_CHANGE_JUSTIFIED summary (charter §6 requirement)

1. **E2 dispatch mediation** — measured ≈1.00; restructuring hazards.
2. **E3 publication lock pairs** — measured ≈1.00; below noise standalone.
3. **Submit batching/deferral (H-K3)** — census shows 1.000 enter/op at every
   sliding-window depth: there is never a backlog to batch. Below-1/op needs
   submit+wait combined enters or SQPOLL — contract/config change, not work
   elimination. Floor for this shape.
4. **Kernel eventfd signal per CQE** — kernel-side, unattributable on WSL2
   (KERNEL_ATTRIBUTION_UNAVAILABLE); lazy registration would change W-03
   wake semantics. No falsifiable candidate here.
5. **Core-mutex coalescing / scope free list / router direct index** —
   respectively: each lock guards a distinct published transition (merging
   spans sink callbacks or duplicates core state); H3 refuted (M1 +15–21%
   FIFO regression); H1 refuted for product (domain narrowing,
   M2 MICROBENCH_ONLY). No new evidence changes these.

## Follow-up inventory (not this round's candidates)

- **ThreadPool owed-count (W7)**: ThreadPool still pays 2×`[scan C]` per pass
  with no derived count (H2b was uring-only). Same candidate class as E1;
  E1's evidence makes this a low-risk sibling. Needs its own parity oracle
  (different lock domain: work_mtx_).
- **W-03 external-loop A/B** on E1 (and H2b) — the standing M2 gap; requires
  a W-03-capable harness (eventfd + poll(2) host loop), owner decision.
- Native-Linux re-run of the E1 matrix on E5 (or successor) before any
  product claim.
