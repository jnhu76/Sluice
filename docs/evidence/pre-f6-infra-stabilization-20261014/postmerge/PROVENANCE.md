# Post-merge verification — PR #522 on actual master

Owner-authorized merge executed 2026-10-14. Pre-merge drift check: local ==
remote branch == PR head == `7077c4d2dcc4257042e232cc522cbe3fe389c52f`,
`origin/master` still `f99acbc3…`, PR mergeable/clean, GitHub CI run
`38570608942` success 4/4 at exactly that head. Merged with a merge commit:

    MERGE_SHA = 573d616cc1ee79ec7a3b3876351799708237db3a

`git diff 7077c4d2 573d616c` is empty — the merged content is byte-identical
to the locked head. Every gate below was re-executed against the real
post-merge master SHA, each driver against its own fresh detached worktree at
`573d616c`; raw logs are the captured stdout/stderr of the recorded commands.
This is the receipt for the already-reviewed slice, not new work: no source
changed between review and merge.

- `identity/` — `scripts/verify_liburing_identity_binding.py --workdir
  <worktree at 573d616c> --output <fresh>`:
  `F10_IDENTITY_BINDING_REGRESSION_PASS` (deficient CPATH header refused at
  the declaration layer; divergence environment accepted with the printed
  identity equal by realpath to the toolchain resolution; clean print equals
  the traced pair). The regenerated fixture bytes are recorded in
  `fixture-SHA256SUMS` (12 stub headers + fakelib) and not committed loose,
  per the F-10 packaging correction.
- `switch/` — `scripts/verify_liburing_config_switch.py --workdir <worktree>
  --builddir <fresh> --output <fresh>`: `LIBURING_CONFIG_SWITCH_REGRESSION_PASS`
  — same-directory n→y→n→y without cleaning (52/66/52/66, per-switch
  object-hash change, `bind_includes = false` in every y phase) and the four
  negatives at their expected layers. Regenerated stub fixture hashes in
  `stub-fixture-SHA256SUMS`.
- `matrix/` — `matrix/run-matrix.py --repo <worktree> --output <fresh>`:
  `PRE_F6_MATRIX_SIX_PROFILES_PASS` — debug-n 52/52, debug-y 66/66,
  release-n 52/52, release-y 66/66, clang asanubsan-n 52/52, clang tsan-n
  52/52 (`request_scope_test` executed for real).
- `b01-v7/` — the verbatim F5 v7 harness (`reproduce.py`, `persist.py`
  unmodified) run as `--before 14a27a83 --after 573d616c`: harness line
  `F5_B01_V6_CANDIDATE_FRESH_PACKAGE_PASS 14a27a83… 573d616c…`. After-counts
  in both ring profiles: 15 library objects, 13 `libsluice_async.a` members,
  25 installed headers (unchanged surface). `durable/` is the persisted text
  subset of the capture; the execution tree is re-derivable from the recorded
  commands.
- `installed-probes/probes-{n,y}/` — `run-probes.py` against this capture's
  fresh installed prefixes with the 49-header before-prefix controls, both
  macro views: 25 headers standalone-compile, standalone failures 0, ODR
  link+run PASS, `removed_present` 0. Object files and the linked probe
  binary are omitted from the commit (re-derivable from the recorded
  commands).

Not re-run post-merge, with the standing rationale: the #519 mutation
killers and the #521 partition — their surfaces (`src/async/request_scope.cpp`,
`include/sluice/async/request_scope.hpp`, the test sources and the target
recipes) are byte-identical from `cdb2ca0f` through the merged master, where
they were executed and recorded, and the matrix's tsan-n/asanubsan-n profiles
re-executed the full suite at the merge SHA.

GitHub CI on the pre-merge head: run `38569275374` (correction commit
`81fd2563`) and run `38570608942` (final head `7077c4d2`), each success in
all four jobs (Linux debug, Linux release, F1 verifier self-tests, C++
comment authority guard).
