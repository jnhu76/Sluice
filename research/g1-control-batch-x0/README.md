# BATCH-X0 — G1-Control Candidate 3 (explicit Batch as a bounded control-plane amortization grant)

Research-only falsification campaign under #227 / #221 / #259. It decides
whether an explicit `sluice::async::Batch` semantic boundary grants a
genuinely useful, Sluice-specific right to amortize execution/control
topology while preserving every per-operation correctness authority.

**RESEARCH ONLY. Production code untouched. The current public Batch
contract is unchanged. No C1 is authorized by this campaign.**

Post-review Corrective-1: the evidence-gate/validator findings are closed
(verified-ext4 substrate gate, A/A recomputed from raw rows + explicit
binding, real git ancestry, blocked-gate ordering before promotion,
per-cell enter evidence), the formal performance session
(batch-x0-perf-native-2) is SUPERSEDED for violating the frozen ext4
substrate rule, and the compliant-substrate rerun is BLOCKED — this host
has no usable ext4 filesystem. The semantic result (S9 DIVERGENCE) is
substrate-independent and stands. Terminal verdict: **STOP — NO C1**
(fail-closed). See BATCH-X0-REPORT.md §0.

## Contents

```text
BATCH-X0-AUDIT.md            as-built Batch/AsyncIoContext/backend facts
BATCH-X0-PREREGISTRATION.md  frozen question/arms/fixtures/mutants/verdicts
campaign.json                machine-readable campaign record
scripts/                     runner + validator (added post-freeze)
results/                     formal evidence (added post-freeze)
BATCH-X0-REPORT.md           final report (added post-freeze)
```

## Execution order (frozen in the preregistration §12)

audit → semantic table → arms/mutants design → FREEZE COMMIT → harness →
mutant self-tests → semantic fixtures (incl. the decisive S9 interleaving
witness) → host qualification (A/A gate) → performance matrix → mechanical
verdicts → adversarial self-review → report + Draft PR.

Precedents: #279/PR #280 (C0 fixed-file — STOP) and PR #281 (COPY-X0 —
STOP, NO C1; A/A-gate BLOCKED discipline). The combined lesson under attack:
semantic discipline increasingly supported; runtime architecture expansion
repeatedly NOT earned.
