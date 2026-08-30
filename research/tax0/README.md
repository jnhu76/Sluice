# TAX-0 — Hot-path data-structure / synchronization topology audit

Research material for issues #227 (roadmap), #249 (paper ledger), #250
(TAX-0 audit). TAX-0A is a READ-ONLY static audit plus native
measurement-readiness check — no production code was modified and no
benchmark sweep was run. Frozen 2026-08-30 after human adversarial review
of #251 (freeze corrections FC-1..FC-7: report §18 / JSON `freeze` block);
this freeze commit is the preregistration baseline for TAX-0B/C — the
hypotheses here were registered BEFORE any causal experiment.

- `TAX0-A-HOTPATH-TOPOLOGY-AUDIT.md` — human report
- `tax0a-hotpath-topology.json` — machine-readable census (every entry
  carries `"status": "FACT"` or `"status": "HYPOTHESIS"`)

Output placement note: the task template suggested
`docs/results/performance-attribution/`, but that directory's own README
restricts it to *runner-produced* benchmark evidence ("Never
hand-created") and is validated by `scripts/bench/perf-evidence-validate.py`.
A hand-collected static census is research material, not a perf-evidence
artifact, so it lives here under `research/` (same convention as
`research/rx1/`).
