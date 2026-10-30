# research-576 — ThreadPool Zero-Owed Delivery Scan

Issue: jnhu76/Sluice#576. Status: CLOSED, `NO_CHANGE_JUSTIFIED`.

## Research Question

Every ThreadPool delivery pass sweeps all C slots even when zero events are
owed (K=0). Would materializing exact derived state (an owed-event count) to
skip K=0 scans pay for its own maintenance tax on the workloads Sluice
actually supports?

## Structure

- **R0 freeze**: environment and binary identities pinned before any
  measurement; baseline master left untouched throughout.
- **R1 state DAG** (the study's correctness backbone): complete
  writer/reader enumeration of the `event_owed` flag — writers W0 (init),
  W1 (submit reset), W2 (publish zero-op set), W3 (pass clear); readers
  S1–S4; reachability proofs including that W1-reset-over-owed-flag is
  unreachable via the zero-op `reclaimable_` control-ref pin; no
  double-counting per event; slot-reuse safety; shutdown authority
  preserved.
- **R2 instrumented census**: an instrumented arm adds census counters as
  weak symbols (`sluice576_*`), so the same harness binary links pristine
  (timing) or instrumented (census). Standalone harness with modes
  `pipeline | zeroburst | mixed | idlepass | mkfixture`; C∈{16,64,256,1024,4096}
  × D∈{1,4,8,16,64} × W∈{1,2,4,8} matrices, perf (paranoid=2) attribution,
  and ABAB×8 cross-capacity paired envelope controls (C16 vs C1024).
- **R3 pre-registered admission gate**: a candidate may proceed only if the
  affected workload is supported, the projected effect is ≥3%, and the
  result differs materially from the #541 adjudication.

## Code References

- Baseline: `master` @ `46157fd1f4a1a4afec22d8834c86462b245b576b`
  (on GitHub).
- Instrumented arm: `research/576-instrumented` @
  `bbc4cb579a0684da9e32b3e86518cee07d2d319a` — two commits on the base
  (census counters + exit dump), **E5-local only, never pushed**; the ref
  has since been deleted. The exact instrumentation survives only as
  `code/s576-instrument.patch` in this archive (and as a full-history git
  bundle in E5 `~/sluice-576-archive/git/`).
- `code/r576_harness.cpp` — the standalone research harness; exists in no
  git history. Original study workspace (reports, matrices, runners,
  measured binaries) remains on E5 at `~/sluice-576-study/` and in the
  interrupted archive staging `~/sluice-576-archive/`.

## Conclusions

`NO_CHANGE_JUSTIFIED` (R3 stop; R4–R8 intentionally `NOT_RUN`;
`PRODUCT_CHANGE = NO`, `PUBLIC_API_CHANGE = NO`).

- K=0 scan cost is real and reproduced: full C-slot sweep ≈ **1.38 ns/slot**
  (idle pass 255/311/562/1640/5822 ns at C=16/64/256/1024/4096); ~6.5
  instr/slot/pass; cost law unchanged vs #541.
- At every supported shape (benches bind C=D; default C=64): passes/op × C
  = 1–12 slot-inspections/op ⇒ predicted benefit **≈0.05–0.3% of wall** —
  below this host's ±3% resolution.
- Material exposure exists only at C≥1024 with D≤16 (legal, but exercised
  by no reference workload or documented configuration). Best corner
  (C1024/D8/W4): scan ≈350 cycles/op ≈ **2.6% wall** < the 3% goal.
- Kill observation: where owed events exist, P(K=0 at pass entry)≈0 — the
  skip never fires, so the count is a pure maintenance tax; K=0 passes are
  precisely the pipeline case, where the benefit bound is <0.3%.
- Cross-capacity envelope: total capacity effect −9.3%/−18.3% at D8W4
  (protocol-dependent), sign-unstable at D16W4; the corner effect is
  majority non-scan footprint cost across delivery/prepared-ops/core slots.
- Measured on E5 (Xeon E5-2666 v3, 10C/20T, Fedora 44, kernel 7.2.11);
  short-phase cross-config resolution on this host is worse than ±3%
  (20–33% spread; D16 median sign flip between 20k/100k protocols) — a
  negative result that is itself part of the verdict.

## Next Steps

Re-evaluate only if a genuinely supported C≫D workload appears *and* its
scan cost is shown to be a material share. Per the R3 conditional record,
BOUND-02 capacity guidance is the correct remedy for such workloads — not a
K=0 skip. The open mechanism question worth measuring first is the zero-op
control-ref pin's backpressure-polling behavior (pass batches reach ~C owed
events/pass), which dominates corner costs more than the scan itself.
