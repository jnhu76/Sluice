# research-594 — E5 Key-Path K1–K4: Counterfactual Mechanism Elision

Issue: jnhu76/Sluice#594 (realizing the #593 method). Status: campaign
complete; `PRODUCT_CHANGE: NONE`; research PR archived; `OWNER_DECISION:
PENDING` on the one supported candidate.

## Research Question

Which mechanisms sit on Sluice's real request-completion critical path,
which of them perform no semantic obligation, and does removing work beat
accelerating it? Four lanes, each mapped, attributed, counterfactually
elided and measured natively on E5.

## Structure

Base `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57` (nine isolated worktrees,
snapshots consistent). Method per lane:

1. Map the mechanism graph (execution DAG).
2. Attribute cost (census counters; perf; strace; Observe arm only).
3. Counterfactual elision arm (patch), paired A/B vs Full
   (15 interleaved pairs per case, bootstrap 95% CI, seed 593).
4. Correctness: three-state mutation receipts
   (BASE GREEN → MUTANT RED → RESTORED GREEN).
5. Negative controls: threadpool null arms (mechanism only touches uring),
   in-gate structural controls, floor probes, stock-list reruns.

Lanes: K1 completion→publication→observer→take; K2 submit→admission→
dispatch→SQE; K3 progress pass→wakeup→retry; K4 SQE/CQE batching at the
kernel boundary; plus the K4-b+K1 combination arm.

## Code References

- All elision/observe patches (14) and the full harness (pair runners,
  census scripts, bootstrap reducers) are **on GitHub master**:
  `bench/research/key-path-593/patches/` and
  `bench/research/key-path-593/harness/` (also branch
  `research/593-key-path`). No code is reproduced in this archive.
- Narrative record: `docs/research/key-path-593/` on GitHub master
  (archived in `9fb4904f`, PR #594): README, decision.md,
  measurement-review.md, correctness-review.md, evidence-index, atlas.
- Raw evidence workspace (pair JSONLs, perf records, snapshots — GB scale)
  remains on E5 at `~/sluice-593-research/evidence/`; not part of this
  knowledge archive.

## Conclusions

| Lane | Verdict |
|---|---|
| K1 | `NO_CHANGE_JUSTIFIED` under the 4.79% campaign band (−2.29% W-03 CPU, 15/15 pairs — below the band, above session A/A); `INCONCLUSIVE` if the band is rejected |
| K2 | `ELISION_CANDIDATE_FOUND` — paired end-to-end gain only in the same-thread form; the ungated arm is not deployable (D=2/D=3 unmeasured) |
| K3 | `NO_CHANGE_JUSTIFIED` — both arms withdrawn (best −1.00%, median inside band) |
| K4 | `ELISION_PERF_SUPPORTED`, form-gated, measured D=4/16/64 (K4-a: `NO_CHANGE_JUSTIFIED`) |
| K4-b+K1 combo | `MECHANISM_ONLY_GAIN` — measured non-additive (W-02 +0.31%, W-03 −2.97% vs K4-b alone); not a candidate |

K2 and K4 gate the same line (`src/async/uring_transport.cpp:87`); the
supported verdict is recorded once, under K4.

**K4-b known regression boundaries**: 64 KiB d16 tail p99 **+7.46%**
(0/15 improved), p95 +7.50%, p50 −2.61%; W-03 d4 CPU +2.54% [+1.32,+4.79];
D=1 −2.50% judged clock/layout noise (unexplained by the gate); D=2/D=3
never measured; threadpool null control w02 +1.19% (CI excludes 0 —
disclosed small positive bias).

**Honest limits from the two reviews**: the 4.79% band is a post-hoc
baseline A/A drift (W03-uring-4k-d4), and its "pre-registered" label was
retracted; K1's and combo's verdicts depend on it. CI methodology is not
uniform across artifacts and p99 conclusions rest on sign counts. K4-b had
no kernel PMU attribution (kernel-side claim inherited from a K2-session
perf record). K1 has **no** dedicated mutation receipt (the only kill was
an over-elided build); `k4_residue_wake_test` is racy (evidence, not
proof); deferred pass-start multi-entry retry is untested. Correctness
review's own words: K4-b is "not sufficient for productization".

## Next Steps

Worth measuring if this line is revived: (1) the K2/K4 same-thread gated
form at D=2/D=3 (the deployability gap); (2) K4-b's deferred
partial-submit/overflow/shutdown paths, which have no test today;
(3) a replacement for the racy residue-wake test before any
productization; (4) re-deriving the K1 verdict with a pre-registered band,
since the current one is post-hoc. The combo result warns that K1-side
mechanisms do not compose additively with K4-b.
