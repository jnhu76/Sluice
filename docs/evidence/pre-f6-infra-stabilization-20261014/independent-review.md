# Independent adversarial review record

Reviewer: an independent agent with full read access to the repository,
the issue tracker and the raw artifacts, instructed to treat the author
as wrong until the evidence convinced it otherwise. It inspected the
commits, the build files, the issue texts (via the GitHub API), the
evidence tree and the F5 prior slice, re-derived object hashes and
symbol lists from the raw captures, and enumerated the full branch
delta. Read-only; it modified nothing.

## Round 1 — eight review points

| Point | Verdict |
|---|---|
| 1. Object reuse eliminated (#520, both directions, all target classes) | PASS-WITH-NOTES |
| 2. Fault injection fails at its own layer, no tautology | PASS-WITH-NOTES |
| 3. Kernel-vs-build separation (no configure-time kernel probing; no silent fallback) | PASS |
| 4. Public macro / ODR integrity (SLUICE_HAS_LIBURING propagation; no ODR surface for the new define) | PASS-WITH-NOTES |
| 5. #519: both mutants killed at runtime, suppression scoped | PASS-WITH-NOTES |
| 6. #521: coverage not traded for green; F5 files untouched | PASS-WITH-NOTES |
| 7. Mechanism justification (no speculative frameworks, no downgrades) | PASS-WITH-NOTES |
| 8. Evidence integrity (same content head; no internal contradictions) | PASS-WITH-NOTES |

Verdict: **READY-AFTER-FIXES**. One blocker and nine non-blockers.

- F-1 (BLOCKER): the RED narrative misdescribed its own artifacts — the
  captures show that no switch in either direction recompiles anything
  (the n1 stub archive persists through both y phases; `b5f4a300…` are
  the y1-created ring seam objects), while the record claimed ring
  objects built at y1 and ring content left under n.
- F-2…F-9: unrecorded killer exit codes; missing command/head identity
  on some logs; a wrong seam-symbol count; the mixed-prefix guard
  compares only top-level directory components (and `liburing.so.3` was
  speculative); the driver's `/usr/lib64` injector assumption and dead
  helper; issue-number citation drift and "conservative" for
  "overcounted"; untracked probe binaries; the fix's switch-specific
  scope unrecorded.

Dispositions: commit `d21c2f2d` (corrected RED narrative bound to the
artifacts, `killers-exit-codes.log`, three `PROVENANCE.md` files, exact
symbol counts, limitation notes, removal of the dead helper, the
speculative soname candidate and the stray binaries) and `54c4886a`
(ledger Head-row scope statement).

## Round 2 — re-verification of the dispositions

The same reviewer re-checked every disposition against the artifacts
(not the prose): the corrected RED section matches the raw logs, nm
dumps and identity files point by point; it re-executed both mutant
binaries live (134 / 1) and re-derived the symbol counts (3 seam + 6
strong overrides in debug; 0 seam and 20 runtime operator symbols under
TSan, identical in a seam-free suite); it enumerated HEAD's entire
non-docs delta vs the gate content `cdb2ca0f` — exactly the two
disclosed no-op hygiene removals. One wording nit was found and fixed
(`red/PROVENANCE.md`, consumer link vs target definition).

Verdict: **READY** at HEAD `54c4886a`.
