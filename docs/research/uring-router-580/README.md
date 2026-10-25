# #580 — Full-domain CQE router lookup research (WSL2 + E5 round)

Issue: jnhu76/Sluice#580 (parent #563). Branch `research/uring-router-580-wsl`
at master `46157fd1`. Research-only: no production code, spec, or CI change is
requested by this PR; the `src/` diff consists solely of compile-time-gated
instrumentation/arm/mutant blocks proven codegen-neutral without their defines
(byte-identical objects for both patched translation units, see
`results/manifest.json`).

## Reading order

| File | Content |
|---|---|
| `environment.md` | R0 freeze (both hosts), toolchains, io_uring capability, host-switch record |
| `source-dag.md` | R1: full call DAG, six scan sites S1-S6, identity matrix, complexity ledger, work-elimination findings WE-1/WE-2 |
| `trigger.md` | R2: measured lookup frequencies/probes, CPU attribution, gate decision `NO_TRIGGER_NO_CHANGE` for index candidates |
| `design-alternatives.md` | R3: gate-blocked records for Candidates A/B; WE-1/WE-2 carried as analysis |
| `lifecycle-cost.md` | R4: full lifecycle cost table + break-even + the five protocol questions |
| `correctness-oracles.md` | R5: oracle scenarios, mutant ledger, M7 kill, slot-only unkillability analysis |
| `measurement-protocol.md` | R6 protocol as executed |
| `wsl-results.md` | R6 results: E5 native (primary) + WSL2 (replication) A/B, A/A, attribution, assembly provenance |
| `reviews.md` | R7: independent Review A (semantics) and Review B (measurement) verdicts |
| `decision.md` | R8: final verdicts and the three closing questions; R9 scope addendum |
| `cache-locality-audit.md` | R9-A: audit of the five cache-locality claims in the R0-R8 evidence |
| `cache-experiment.md` | R9-B/C: cache-sensitivity design, interference workload, matrix, PMU plan |
| `cache-results.md` | R9: probe invariants, paired net-wall vs A/A floors, clflush bound, PMU shares, decision gate |
| `results/` | raw JSONL, oracle/conformance logs, disassembly, manifest with SHA-256s |
| `patches/` | archived gated source patch (instrumentation + arm/mutant gates) |

Harness: `bench/research/uring-router-580/` (probe, A/B runner, oracle,
builders, reducer). All scripts are research-only with named consumers in this
campaign; nothing here enters product builds.

## Headline result

CQE reverse router lookup is **not a material avoidable cost** under the full
v2 contract at any measured configuration: 1 lookup/op, O(live/2) probes
independent of capacity, 1.6-2.3% of user-space cycles at the worst artificial
shape, ~0.4% of total CPU. The only capacity-scaled scan (the forward by-handle
cancel lookup, ~993 probes/cancel at C=1024) was eliminated 30× by a
scan-direction flip whose measured end-to-end effect is indistinguishable from
the A/A noise floor on the native host. Verdict: `NO_TRIGGER_NO_CHANGE`
(issue-level); no product change proposed.
