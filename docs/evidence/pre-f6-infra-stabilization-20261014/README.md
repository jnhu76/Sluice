# Pre-F6 infrastructure stabilization — #520, #519, #521

Post-F5 build/test infrastructure slice on branch
`codex/pre-f6-infra-520-519-521` (commits `5d611b28` → `7f9d43c7` →
`cdb2ca0f`, then the F-10 correction `70bf3e15` → `9e04d3b5`), baseline
`f99acbc3`. Status: **CANDIDATE — Ready-for-Review**; no merge authorization
is claimed (PR #518's authorization is not transferable). The F-10
correction (review round 2, P1, #520 scope) re-heads the liburing
dependency-identity mechanism at `9e04d3b5`; every F-10 gate ran at that
content.

- #520: a same-directory `--liburing` switch recompiled nothing in either
  direction — the n→y switch kept the stub archive under `--liburing=y`
  (xmake's depend check is blind to tail-appended flags), and the consumer
  link failure was the loud symptom. Fixed with an unconditional public
  `SLUICE_LIBURING_PROFILE=on|off` identity define plus configure-time
  capability detection with per-layer fault-injection negatives. Review
  round 2 (F-10) then replaced the probe's candidate scan with the
  toolchain's own include/link traces so the probe and `sluice_async` use
  the same resolved dependency identity.
- #519: the M4 mutation target failed to compile under Clang
  (`-Wunused-private-field` via `-Werror`); fixed with a target-local,
  Clang-tools-only suppression; both mutants still die by their recorded
  named killers.
- #521: the request-scope zero-allocation oracle (strong global
  operator new/delete) collided with the TSan runtime, so TSan excluded
  the whole target. The seam now compiles only outside TSan; TSan runs
  the other 24 cases for real.

Contents:

- `execution-record.md` — every gate bound to SHAs, commands and raw
  evidence, including the RED captures and the notes/limitations.
- `red/` — RED evidence at `f99acbc3`: the same-dir switch capture
  (per-phase object hashes showing that no switch in either direction
  recompiles, the stub archive retained under `--liburing=y`, and the
  consumer link failure) and the Clang mutation-target build failures.
- `switch-regression/` — the green n→y→n→y same-dir cycle plus the four
  fault-injection negatives at `cdb2ca0f` (`scripts/verify_liburing_config_switch.py`).
- `matrix/` — six-profile matrix runner and captured results
  (debug/release × n/y, clang asanubsan-n, clang tsan-n; tsan-n executes
  `request_scope_test`, expected 52).
- `mutation-killers/` — both mutant build/run records at `cdb2ca0f` with
  the backtraces naming the killing cases, including the recorded-killer
  isolation run for M4.
- `request-scope-tsan/` — 25/25 non-TSan vs 24/24 TSan case counts and
  the seam-symbol absence, bound to the matrix binaries.
- `b01-v7-capture/` — new versioned dual-profile package capture
  (F5 v6 harness verbatim, `--before 14a27a83 --after cdb2ca0f`):
  `reproduce.py`, `persist.py`, and `durable/` (928 text artifacts);
  counts [32,30,49] → [15,13,25] in both ring profiles. The execution
  tree is re-derivable; it is not committed.
- `installed-probes/` — standalone compile of every installed header and
  the cross-TU ODR/link/run probe in both macro views, with the
  49-header before prefix as control.
- `f10-correction/` — the review-round-2 dependency-identity correction:
  `red/` (the divergence counterexample, precedence study and RED capture
  at `5ab32ae7`) and `green/` (identity regression, re-baselined switch
  negatives, six-profile matrix, B-01 v7 capture and installed probes all
  re-run at `9e04d3b5`; `green/PROVENANCE.md` binds every driver to its
  worktree head and command). The recorded stub fixture bytes are archived
  (`green/identity-regression/fixtures.tar.gz` + `SHA256SUMS`), not
  committed as loose headers.

F5's evidence directory (`f5-final-package-contraction-20261013/`) is
immutable history: nothing there was modified.
