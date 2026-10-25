# #580 — Full-domain CQE router lookup research — RESEARCH ARCHIVE

Issue: jnhu76/Sluice#580 — **CLOSED** (parent #563). This PR (#592) is a
**research archive**: `PRODUCT_CHANGE: NONE`, zero `src/`/`include/`/spec/CI
diff against master `46157fd1`. The compile-time-gated research blocks
(lookup instrumentation, WE-2 arm, FALSE_MISS/SLOT_ONLY mutants, R9 clflush
gate) live in `patches/research-gates-full.patch` — generated as
`git diff 46157fd1 2c00c36a -- src/async/`, admitted by base→apply→reverse
round-trip in a fresh worktree, and proven codegen-neutral without their
defines (byte-identical objects for both patched translation units, see
`results/manifest.json`).

## Reading order

| File | Content |
|---|---|
| `archive-index.md` | evidence map: stage → evidence → script → identity → verdict, with evidence grades |
| `environment.md` | R0 freeze (both hosts), toolchains, io_uring capability, host-switch record |
| `source-dag.md` | R1: full call DAG, six scan sites S1-S6, identity matrix, complexity ledger, work-elimination findings WE-1/WE-2 |
| `trigger.md` | R2: measured lookup frequencies/probes, CPU attribution, gate decision `NO_TRIGGER_NO_CHANGE` for index candidates |
| `design-alternatives.md` | R3: gate-blocked records for Candidates A/B; WE-1/WE-2 carried as analysis |
| `lifecycle-cost.md` | R4: full lifecycle cost table + break-even + the five protocol questions |
| `correctness-oracles.md` | R5: oracle scenarios, mutant ledger, M7 kill, slot-only unkillability analysis |
| `measurement-protocol.md` | R6 protocol as executed |
| `wsl-results.md` | R6 results: E5 native (primary) + WSL2 (replication) A/B, A/A, attribution, assembly provenance |
| `reviews.md` | R7: independent Review A (semantics) and Review B (measurement) verdicts; R9 same-session + delta-review records |
| `decision.md` | R8: final verdicts and the three closing questions; R9 scope addendum (delta-reviewed) |
| `cache-locality-audit.md` | R9-A: audit of the five cache-locality claims in the R0-R8 evidence |
| `cache-experiment.md` | R9-B/C: cache-sensitivity design, interference workload, matrix, PMU plan |
| `cache-results.md` | R9: probe invariants, paired net-wall vs A/A floors, clflush decomposition, PMU shares, decision gate |
| `results/` | raw JSONL, oracle/conformance logs, disassembly, PMU data, manifest with SHA-256s |
| `patches/` | `research-gates-full.patch` (complete, R9-inclusive); `instrumentation-and-arm-gates.patch` (R0-R8-era subset, superseded, retained for provenance) |

Harness: `bench/research/uring-router-580/` (probe, A/B runner, oracle,
builders, reducers). All scripts are research-only; nothing here enters
product builds.

## Reproducing

```sh
# verify: isolated worktree at the frozen base + patch + builds + receipt
bench/research/uring-router-580/prepare-research-worktree.sh --mode verify

# replay: additionally runs the oracle RED/GREEN suite and probe/cache smokes
bench/research/uring-router-580/prepare-research-worktree.sh --mode replay
```

Both modes work entirely inside a throwaway git worktree; the caller's
checkout and the archived `results/` evidence are never modified. Build
scripts fail closed (exit 2) when run on an unpatched tree. Rebuilt binaries
hash differently from the historical ones by construction; both identities
are recorded (`results/manifest.json` for history, the generated
`prepared-receipt.json` for the rebuild).

## Headline result (current, delta-reviewed)

CQE reverse router lookup is **not a material avoidable cost** under the full
v2 contract within the measured domain (D ≤ 512, interleaved working sets
≤ 16 MiB at ≤ 4 KiB/op, W-02 single-owner and W-03-replica execution forms,
WSL Zen 3 + E5 Haswell): 1 lookup/op, O(live/2) probes independent of
capacity, 3-5 cycles/probe warm, invariant under cache pressure. In R0-R8
shapes the lookup is 1.6-2.3% of user cycles at the worst artificial shape
(~0.4% of total CPU); R9 measured 5.07% (WSL) / 5.8-9.8% (E5,
condition-dependent) harness-inclusive only at the non-standard D=512 warm
shape, ≈0.21-0.50 µs/op absolute. The only capacity-scaled scan (the forward
by-handle cancel lookup, ~993 probes/cancel at C=1024) was eliminated 30× by
a scan-direction flip (WE-2, EXPLORATORY_ONLY) whose measured end-to-end
effect is indistinguishable from the A/A noise floor on the native host.

Verdicts: `NO_TRIGGER_NO_CHANGE` (issue, closed), `HOT_PATH_ROBUST` within
the measured cache domain, `PRODUCT_CHANGE: NONE`. R9 delta review: APPROVE.

Reopen conditions (all measured, none present today): a real application
form (#578) with sustained live depth in the hundreds **together with**
per-lookup L2+ eviction or a measured prototype win; revisit order
WE-2 → WE-1 → Candidate A. The clflush regime (forced eviction before every
lookup: +26%/+108% wall on E5, ≈90% of which is the flush instruction at
~30 ns per flushed line; genuine cold refill ≈ +8% E5 / +29% WSL by
capacity-gradient differencing) is synthetic and has no applicant.
