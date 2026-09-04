# G1-Control COPY-X0 — Explicit Copy as a Legal Transformation Boundary

Research/falsification campaign (G1-Control Candidate 2) under roadmap #227 /
#259. Question: does an explicit composed `Copy` boundary grant a useful,
mechanically bounded transformation authority that primitive `read`/`write`
must not have, while preserving the observable contract? Primary mechanism:
`copy_file_range`; `splice` never opened (goal §8 conditions not triggered).

**RESEARCH ONLY. PRODUCTION CODE UNTOUCHED. NO C1 AUTHORIZED BY THIS
CAMPAIGN'S EVIDENCE.**

## Final verdicts (`results/campaign-verdicts.json`, mechanically derived)

```text
CAPABILITY:              BLOCKED (measurement infrastructure — 11 disclosed
                         formal attempts failed the frozen A/A calibration
                         gate on this WSL2 host)
SEMANTIC-EQUIVALENCE:    EQUIVALENT FOR DECLARED COPY CONTRACT
TRANSFORMATION-BOUNDARY: LEGAL TRANSFORMATION BOUNDARY SUPPORTED
MINIMALITY:              LOCAL COPY BRANCH SUFFICIENT; FRAMEWORK NOT EARNED
G1-CONTROL:              NOT ESTABLISHED (capability gate unmeasured)
PROMOTION:               STOP — NO C1
```

## Layout

```text
COPY-X0-AUDIT.md              as-built copy semantics at master ecd84259
COPY-X0-PREREGISTRATION.md    FROZEN at 715c7711 (+ Amendments 1-3, §16)
COPY-X0-REPORT.md             final report + adversarial 15-question review
campaign.json                 machine-readable campaign record
scripts/                      driver + fail-closed validator + design gate
results/                      immutable sessions (verdicts + supersession
                              records; work/ dirs are not evidence)
bench/g1_control_copy_x0_bench.cpp   research-only four-arm harness
```

## Reproduce

```sh
xmake f -m release --toolchain=clang -y && xmake build g1_control_copy_x0_bench
python3 research/g1-control-copy-x0/scripts/run_copy_x0.py qualify \
  --session <name> --tmp-root <tmpfs-dir> --ext-root <ext4-dir>
python3 research/g1-control-copy-x0/scripts/validate_copy_x0.py --self-test
python3 research/g1-control-copy-x0/scripts/validate_copy_x0.py --session \
  research/g1-control-copy-x0/results/<name>
```

## Status — campaign CLOSED (STOP — NO C1)

- [x] audit, prereg freeze, harness, self-tests/mutants
- [x] formal semantic session (VALID, host-local)
- [x] formal perf attempts ×11 (all superseded-degraded; disclosed)
- [x] verdicts mechanically derived; adversarial self-review answered
- [x] reopen condition recorded (measurement window passing the frozen A/A
      gate; frozen §9 rule then applies unchanged)
