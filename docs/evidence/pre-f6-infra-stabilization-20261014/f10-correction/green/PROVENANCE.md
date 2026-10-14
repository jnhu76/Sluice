# F-10 GREEN gates — identity-resolution correction

All gates re-run at the corrected head
`9e04d3b54e8d4e0d7f5bf7cc86bb1622e55ea67b` (branch `codex/pre-f6-infra-520-519-521`),
each driver against its own fresh git worktree of that head. Drivers are the
committed scripts; raw logs are captured stdout/stderr.

- `identity-regression/` — `python3 scripts/verify_liburing_identity_binding.py
  --workdir <worktree at 9e04d3b5> --output <fresh>`: the deficient-CPATH-header
  negative refuses at the declaration layer; the C_INCLUDE_PATH/LIBRARY_PATH
  divergence environment is accepted with the printed identity equal (realpath)
  to what `g++ -E -H` / `-Wl,--trace` resolve; the clean-environment print
  equals the traced pair.
- `switch-regression/` — `python3 scripts/verify_liburing_config_switch.py
  --workdir <worktree> --builddir <fresh> --output <fresh>`: same-directory
  n→y→n→y with no cleaning (52/66/52/66 tests, per-switch object-hash change,
  consumer fingerprint, `bind_includes = false` in every y phase — clean-env
  resolution adds no flags), then the re-baselined negatives: missing header →
  include-resolution layer; bind-mounted empty library files → symbol layer
  (an empty file resolves as a file and ld reads it as an empty linker script,
  so the honest failure is the missing symbol surface); reduced CPATH API →
  declaration layer; CPATH `#include_next` chain → accepted with the printed
  identity naming the stub and `__sluice_liburing_bind_includes = true` recorded
  for consumers.
- `matrix/` — `python3 docs/evidence/pre-f6-infra-stabilization-20261014/matrix/
  run-matrix.py --repo <worktree> --output <fresh>`: six profiles from fresh
  dirs, `PRE_F6_MATRIX_SIX_PROFILES_PASS` (debug/release × n/y 52/66/52/66,
  clang `asanubsan-n` 52/52, clang `tsan-n` 52/52 with `request_scope_test`
  executed for real).
- `b01-v7/` — the verbatim F5 B-01 v7 `reproduce.py` re-run with `--after
  9e04d3b5`: `F5_B01_V6_CANDIDATE_FRESH_PACKAGE_PASS 14a27a83… 9e04d3b5…`;
  after-counts 15 library objects / 13 async archive members / 25 installed
  headers in both ring profiles; `durable/` is the persisted text subset of the
  capture (the execution tree is re-derivable from the recorded commands).
- `installed-probes/probes-{n,y}/` — `run-probes.py` against this capture's
  fresh installed prefixes with the before-prefix controls: 25 installed
  headers, standalone-compile failures 0, ODR link+run PASS,
  `present_removed_headers` 0, control prefixes 49 headers.

Not re-run, with reason: the #519 mutation killers — the mutation surface
(`src/async/request_scope.cpp`, `include/sluice/async/request_scope.hpp`) and
their build recipe are untouched by the identity change, and the matrix's
`tsan-n` profile re-executed `request_scope_test` for real. The #521 seam
partition is untouched (no test-source or `request_scope_target` changes).

Scratch paths under /tmp are ephemeral; `identity.txt` and each driver log
record the exact worktree head, build directory and tool versions.
