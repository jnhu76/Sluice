# Results

This directory stores committed validation and benchmark evidence.

- [`performance-attribution/`](performance-attribution/) — canonical
  machine-readable performance-evidence artifacts (runner-produced JSON,
  never hand-created). Structural validation:
  `python3 scripts/bench/perf-evidence-validate.py` (also runs in the
  pre-push/CI gate).

Local throwaway run outputs do not belong here; canonical evidence records
its own git SHA, environment fingerprint, raw samples, and provenance note
(see `docs/verification/performance-engineering.md` §13).
