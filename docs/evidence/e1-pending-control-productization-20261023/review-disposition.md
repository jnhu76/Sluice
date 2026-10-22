# R8 Review Disposition (2026-10-23)

Both reviews ran independently (separate agents, source-first mandates,
"do not trust the author's documents"); full reports:
`review-A-concurrency.md`, `review-B-performance.md`.

## Verdicts

- Review A (semantics/concurrency): **APPROVE** — 0 × P1, 0 × P2.
- Review B (performance/evidence): **APPROVE_WITH_CONDITIONS** — 1 × P2,
  5 × P3.

## P2 (review B, F-1) — fixed

Attribution "instructions 552 804 305 → 410 779 352, ratio 0.743" was a
perf-CSV field misread (time-enabled column, byte-identical on the
instructions and cycles lines; equal to the 0.5528 s / 0.4108 s run
durations). Corrected in `product-ab/README.md`, the ledger G6 row and
the ledger Mechanism-Justification answer 4: attribution now rests on
cycles 668 188 248 → 234 099 483 (0.350), utime 4136.6 → 1598.2 ns/op,
and the strace census (`io_uring_enter` 60003 both arms). Following the
reviewer's caution, the field-1 instruction counts (ratio 0.190) are
NOT substituted: their implied IPC shift (2.76 → 1.50) is inconsistent
with the cycle/rusage ratios, so the run's instruction capture is
recorded as unreliable rather than quoted. No gate arithmetic, pairing,
identity check or honesty admission was affected; no benchmark re-run
required (documentation-only fix).

## P3 dispositions

- A-P3-1 (tsan reproduction trap): documented in `README.md` (R3 row):
  mode switches need explicit `--liburing=y` and the tsan binary lives
  in `build/linux/x86_64/tsan/`, not `release/`.
- A-P3-2 / B-P3-6 (review files forward-referenced): resolved — both
  review files now exist in this directory.
- A-P3-3 (~840 KB committed A/B binaries): kept deliberately; the
  binaries are the hash-pinned provenance artifacts the verdict's
  identity gate checks against.
- A-P3-4 (`xmake test` "nothing to test" under xmake 3.1.2): pre-existing
  repo-wide quirk, out of scope; binaries were run directly.
- B-P3-2 (`e1_sha: null` provenance collapse): stays as disclosed in
  `product-ab/README.md`; load-bearing identity checks (differing
  lib/binary hashes, fixture hash) all verified by both sides.
- B-P3-3 (reducer could silently overwrite an existing `--out`):
  fixed — `e1_product_verdict.py` now refuses any existing `--out`
  path; selftest re-run 16/16 after the change.
- B-P3-4 (duplicated `arm` key in `w03-verify.jsonl` rows): frozen
  campaign data left untouched; JSON last-wins parsing is deterministic
  and the reducer's per-arm counting was verified unaffected (5+5).
- B-P3-5 (cross-campaign `e1_sha` drift seam vs product-ab): verified
  benign by the reviewer — the intervening commits are docs-only;
  `git diff d59a266b..81e7e865 -- src include tests xmake` is empty.
