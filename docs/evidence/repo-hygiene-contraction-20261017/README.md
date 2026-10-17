# Repository hygiene contraction — 2026-10-17

Scope: repository naming, script inventory, test/build-graph labels, dated
evidence layout and CI/hook wiring. No production semantics, public API,
request/observer/progress/shutdown design, or release gate changed.

- Base commit: `9845cee1` (master, clean tree, identical to origin/master).
- Records in this directory: this README, `verification-record.md` (commands
  and verdicts executed at the hygiene head), `naming-migration.md`
  (old -> new -> semantic basis -> consumer migration), `script-disposition.md`
  (zero-based script ledger), `test-retention.md` (test/mutant retention and
  kill evidence), `deletion-safety.md` (removal dependency chains).
- Ledger tracking: `docs/roadmap/v2-conformance.md`, section "Dated
  repository-hygiene contraction (2026-10-17)".
- Supersessions introduced here: the retired verification scripts and the
  removed dated records are recoverable from Git history at `9845cee1` and,
  for the v1-campaign records, at `c001bb1e`.
