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

## Round 3 — the F-10 dependency-identity correction

Dispatched after the external finding `F10_PROBE_REAL_BUILD_DEPENDENCY_DIVERGENCE`
(P1, #520 scope): the round-1 probe selected header/library by candidate scan
while production resolves through the toolchain. Reviewed content: RED at
`5ab32ae7`, fix at `9e04d3b5`, evidence/docs at `780703a7`. The reviewer
re-ran the identity oracle against its own worktrees (RED reproduces at
`5ab32ae7` with `F10_PROBE_PRODUCTION_IDENTITY_DIVERGENCE`; GREEN reproduces
at `9e04d3b5` with `F10_IDENTITY_BINDING_REGRESSION_PASS`), re-derived the
linker precedence (`-L` → gcc-configured dirs → `LIBRARY_PATH`, confirmed
including the surprising case that a `LIBRARY_PATH` copy loses to the gcc
multilib dirs), built its own adversarial environments (deficient `CPATH`
header vs working `CPLUS_INCLUDE_PATH` copy; working stub with binding;
symbol-less `LIBRARY_PATH` .so; fresh-process build under a bound stub), and
checked the diff scope, the re-baselined negative layers, the matrix, the
B-01 v7 capture and the installed probes against the raw artifacts.

| Point | Verdict |
|---|---|
| A. RED honesty (deterministic counterexample, precedence claims) | PASS-WITH-NOTES |
| B. Mechanism (toolchain traces, pin equality, conditional binding) | PASS |
| C. No over-rejection (compatible version-different installs accepted) | PASS |
| D. Doc corrections (C_INCLUDE_PATH role, superseded prefix claims) | PASS-WITH-NOTES |
| E. RED/GREEN oracle independence (executed for real, both heads) | PASS |
| F. Regression constraints (cycle/negatives/matrix/B-01/probes, #519/#521 untouched) | PASS |
| G. Evidence integrity (heads, no contradictions) | PASS-WITH-NOTES |
| H. Mechanism justification (config keys, no speculative framework) | PASS |

Verdict: **READY-AFTER-FIXES** — all six human-required actions satisfied
with independently reproduced evidence. Findings:

- F-A1 (NON-BLOCKER, must fix before merge): the precedence study's claim
  "env paths rank ahead of `-I`" (and the propagated "the pin loses to
  `CPATH`" reason for the deficient-CPATH negative) is false on this host —
  the `-I` directory wins for gcc and clang, angle and quote forms; recorded
  as measured evidence in five locations, and `precedence-study.log` was a
  hand-written summary despite the PROVENANCE claim of captured output.
- F-D2 (NON-BLOCKER): "actual mixing refuses at configure time" wording —
  the mechanism deliberately has no prefix-equality refusal; only
  unverifiable identity (unresolvable header/library, pin disagreement)
  refuses.
- F-D3 (NON-BLOCKER): the Reproduction block still pointed the re-run gates
  at the `cdb2ca0f` era.
- F-B1 (NON-BLOCKER): re-running the identical configure command in the same
  build dir under a changed environment skips the option check (xmake option
  cache), leaving the previous probe's persisted identity — internally
  consistent and loudly fail-loud if invalidated, but undisclosed.
- F-H2 (NON-BLOCKER): with binding disabled, a consumer adding its own `-I`
  with a different `liburing.h` could shadow the resolved header at its own
  compile — inherent toolchain semantics, no construction in this repo hits
  it, undisclosed.

Disposition (commit `e5f3af8e`): the author independently re-measured and
confirmed F-A1 (`-I` beats `CPATH`; search-list order agrees with
resolution; CPATH before CPLUS_INCLUDE_PATH; C_INCLUDE_PATH invisible to
C++; linker order unchanged), re-captured `precedence-study.log` as real
command output, corrected all five propagation sites, re-derived the
deficient-CPATH negative's validity from the corrected order (the old scan's
first candidate was the `CPATH` dir itself, so probe and production compiled
the same deficient file), and folded in F-D2/F-D3/F-B1/F-H2. No executable
statement changed; the identity regression was re-run green against a fresh
worktree at `e5f3af8e` (`F10_IDENTITY_BINDING_REGRESSION_PASS`), recorded in
`f10-correction/green/PROVENANCE.md`.

## Round 4 — final evidence-integrity re-check (fixture packaging)

Dispatched after GitHub CI run 38561055545 failed the "C++ comment authority
guard" job on the committed stub copies of upstream liburing.h (the
third-party comment "* Check issue #291 for more information.", rule
`issue-pr-reference`; the other three jobs passed). Correction commit
`81fd2563`: the loose stub-good/stub-bad include trees are replaced by
`fixtures.tar.gz` + per-file `SHA256SUMS`, with the packaging provenance in
`green/PROVENANCE.md` and a docstring evidence-index paragraph in
`scripts/verify_liburing_identity_binding.py`.

Reviewer verification, all PASS: the archive restores byte-exactly (12/12
hashes OK; SHA256SUMS covers exactly the 12 removed files; stub-bad differs
from stub-good only in the removed `io_uring_get_probe_ring` declaration);
the upstream comment is present verbatim in the archived bytes and every
archived file is byte-identical to the pre-change blobs (packaging, not
scrubbing); the scripts diff is the 6-line docstring only, scanner and its
28 self-tests unmodified and passing, xmake/src/include/tests/apps
untouched; the new PROVENANCE/README/docstring claims are accurate (source
= the host's traced liburing 2.13 headers, restore commands reproduce,
the script never reads the archive); GitHub API confirms run 38569275374
at exactly `81fd2563` with all four jobs success and the PR head unchanged.
Verdict: **CLEAN — no blockers.** One out-of-scope observation, no action:
`fakelib/liburing.so` remains a committed loose binary run artifact (not a
scannable C++ source; intentionally retained as raw run evidence).
