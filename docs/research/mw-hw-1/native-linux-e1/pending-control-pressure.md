# E1 Native-Linux Validation — P>0 Pending-Control Pressure (Gate R5)

Status: **`P_POSITIVE_COST = NOT_MEASURED`** (real-kernel sticky states could
not be reproduced on this stack) — with the real cancel-path A/B measured and
neutral, and full sticky-path semantic coverage from the deterministic suites.

## What was attempted

Evidence-status note (Review A P2): the probe runs below were first made
during round exploration and their raw lines were not archived; the citable
artifacts are the **formal campaign probe** (`results-p5/p5-probe.jsonl`) and
the **archived supplement** (`results-p5/p5-probe-supplement.jsonl`, recorded
during review remediation with the same frozen probe binary and a
byte-identically recreated fixture) described under "Archived probe evidence".

1. **Pipe-cancel storm** (`C32/Q8/W16`, reads on a writer-less pipe — every
   op genuinely kernel-pending, every settle via real cancel; round-log
   runs): all cancels took the *prepared → submitted* control path. The
   sticky path never engaged.
2. **CQ-congestion storm** (`C32/Q8/W16`, then `W24`; round-log runs):
   instant-completing tmpfs reads accumulate faster than they are reaped so
   `io_uring_submit` turns EBUSY — the intended way to jam the SQ ring and
   force `get_sqe` failures on the control path. The round-log monitor
   observed real control SQEs in kernel (`control_sqe_observations` up to 42)
   with `p_positive_samples = 0` and `parity_violations = 0` throughout.

## Archived probe evidence (citable)

- **Formal campaign probe** — `p5-probe.jsonl`, one run at the formal shape
  (`--requests=1000 --window=24`): `p_max=0`, `p_positive_samples=0`,
  `parity_violations=0`, `samples=2`. The sample count is itself evidence
  about the workload: all 1000 instant-completing ops settle faster than the
  monitor thread can yield-sample.
- **Post-review supplement** — `p5-probe-supplement.jsonl`
  (sha256 in `provenance-p5-supplement.txt`; frozen probe binary
  `87c99ff2…`, fixture recreated byte-identical `1cdf83d6…`, pinned CPU 2,
  2026-10-22T11:13Z): 10 runs at `--requests=20000` plus 2 replays of the
  formal shape (`--requests=1000`, which confirm the formal shape starves the
  monitor: `samples=0` on both). Totals across the 20K-request runs:
  **1,731 monitor samples, `p_positive_samples = 0`, `parity_violations = 0`,
  `p_max = 0`**, with `control_sqe_observations = 6` — the monitor did catch
  live control-path SQEs in kernel, and in those same samples the derived
  count was still 0, consistent with clear-on-prepare.

## Why sticky states do not occur here (mechanism, with evidence)

liburing 2.13's `io_uring_submit` internally drains completions when the CQ
is congested, so the application-visible SQ ring does not stay saturated
under Sluice's eager per-accept submission: `submit_transport_locked()`
consumes each SQE essentially immediately, and by the time a cancel needs a
control SQE, a slot is free. The sticky branch (`sqe == nullptr` after a
retry) therefore requires an interleave this stack does not produce in these
workloads. On the archived probes: formal campaign probe (0 sticky
observations, 0 parity violations) plus the post-review supplement (12 runs,
1,731 monitor samples, 0 sticky observations, 0 parity violations —
`results-p5/p5-probe-supplement.jsonl`); earlier round-log probe runs were
directionally identical but are not archived. The protocol's alternative —
forcing SQE pressure
with the switchable-submit test hook — is a fake kernel and is valid only
for semantic coverage (R1), never as performance evidence, so it was not
used here.

## What WAS measured (real kernel, cancel-heavy)

Formal campaign `run_e1_p5.sh`, 15 interleaved AB/BA pairs, congestion
storm (`file-cancel-congestion C32-Q8-W24`, 2000 cancel cycles per rep):

| Metric | BASE | E1 |
|---|---|---|
| median cpu_ns_per_op | 4872.2 | ratio **0.9907** (9/15 pairs < 1) |
| invariant (parity asserted per row) | ok | ok |
| cancel latency p50/p95 (instrumented phase) | recorded per row | recorded per row |

The 0.9907 ratio is the preregistered per-rep pairing (E1 rep *i* vs BASE rep
*i*). The frozen reducer emits no P5 ratio at all — it short-circuits to
`NOT_MEASURED` on `p_events_total == 0` before any median is computed — so
this number exists only in this document, recomputable from
`results-p5/p5.jsonl` (Review B P2-1: do not cross-read it against the
reducer's rank-paired W-03 values). This is P=0 cost evidence only: no row of
this campaign contains an actual sticky state.

No regression at the ≤1.05 preregistered line; E1's counter maintenance on
the paths this workload exercises (clear-on-prepare, clear-on-retire) is
measurably free.

## Consequence for the verdict

Per the frozen protocol: sticky P>0 real-kernel cost is `NOT_MEASURED`, so
the round cannot issue a full productization `NATIVE_GO` — the overall
verdict is `CONDITIONAL` regardless of the W-02/W-03 gains. What P>0 risk
remains is bounded by evidence, not measurement: the counter's cost model is
one mutex-held integer update at four sites; the WSL2 round measured the
sticky path at ratio ≈ 1.0–1.05; and the deterministic suites prove the
sticky path itself stays correct. Productization should carry a P>0
follow-up measurement obligation on a stack/kernel where sticky states are
reachable (or after a deliberately induced, protocol-legal backlog
scenario), rather than treating this round as covering it.
