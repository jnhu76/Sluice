# F5 — Final package & public surface contraction (#402)

Execution baseline: `14a27a83821a94e6fc5a9684b48fcd296d7de891` (F2 final exit
master). Candidate branch: `f5-final-package`, implementation head
`a23a94cdfbfa90f6ad6aff2d1b6706c305a4245c`. Status: **CANDIDATE — owner
decision pending**; nothing here is merged or authorized.

Contents:

- `closure-manifest.md` / `closure-manifest.json` — F5-A live package closure,
  per-surface dispositions and the #475 A/B/C trigger assessment at the
  baseline.
- `installed-surface-risk-disposition.md` — the per-surface breakage record,
  negative-probe inventory, rollback units and the requested owner decision.
- `execution-record.md` — the execution log binding every F5 gate to exact
  SHAs, commands and raw evidence, including the three pre-existing environment
  findings.
- `b01-v6-candidate/` — B-01 v6 four-configuration clean-room package capture:
  `reproduce.py` (the harness that produced
  `F5_B01_V6_CANDIDATE_FRESH_PACKAGE_PASS`), `persist.py`, `summary.json`,
  `commands.json`, `inventory.lua`, `raw-manifest.json`,
  `raw-package-evidence.tar.gz` (928 text artefacts) and
  `excluded-artifacts.json`.
- `installed-probes/` — `run-probes.py` with per-profile results under
  `probes-n/` and `probes-y/`: every installed header compiled standalone in
  both macro views, the cross-TU ODR/link probe, removed-header absence with
  the before-prefix control.
- `matrix/` — `run-matrix.py` plus the captured six profiles, their effective
  `xmake.conf`, toolchain cache and `toolchain.log`; `matrix.json`.
- `known-preexisting/` — the pre-existing environment findings: the Clang
  `xmake -a` mutation-target build failure at base and head, and the captured
  shared-build-directory ring-profile reuse failure.
- `w04-negative-support-audit.md` — the W-04 conditional-N/A support-claim
  audit.
- `independent-review.md` — the adversarial review record for the final
  candidate head.

Prior-phase receipts (F2/F3/F4) are immutable history; nothing in this
directory rewrites them.
