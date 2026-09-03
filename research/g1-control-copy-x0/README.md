# G1-Control COPY-X0 — Explicit Copy as a Legal Transformation Boundary

Research/falsification campaign (G1-Control Candidate 2) under roadmap #227 /
#259. Question: does an explicit composed `Copy` boundary grant a useful,
mechanically bounded transformation authority that primitive `read`/`write`
must not have, while preserving the observable contract? Primary mechanism:
`copy_file_range`; `splice` only as a narrowly justified secondary arm.

**Research only. Production code untouched. No C1 authorized by this
campaign's evidence.**

## Layout

```text
COPY-X0-AUDIT.md              as-built copy semantics at master ecd84259
COPY-X0-PREREGISTRATION.md    FROZEN question/arms/fixtures/rules (see FREEZE COMMIT)
COPY-X0-REPORT.md             final report + verdicts (after formal evidence)
campaign.json                 machine-readable campaign record
scripts/                      driver + fail-closed validators + design gate
results/                      immutable session directories (raw JSONL + env)
bench/g1_control_copy_x0_bench.cpp   (repo bench/ root) research-only harness
```

## Status

- [x] Step 0 baseline + governing issues read (#227 #259 #279 PR #280)
- [x] Step 1 audit
- [x] Step 2 design (ladder/fixtures/mutants — inside preregistration)
- [x] Step 3 preregistration frozen (see commit)
- [ ] Step 4 harness + validators
- [ ] Step 5 self-tests / mutants
- [ ] Step 6 formal semantic fixtures
- [ ] Step 7 formal performance matrix
- [ ] Step 8 verdicts derived
- [ ] Step 9 report
- [ ] Step 10 adversarial self-review
- [ ] Step 11 promotion decision + Draft PR
