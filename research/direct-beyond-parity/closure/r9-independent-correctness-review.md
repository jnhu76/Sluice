# R9 Independent Correctness Review — §12.1 (transcribed record)

- Scope: E5 Closure workflow R9 §12.1 独立正确性评审 — PR #602 (C1 product
  implementation, head `77e5399a45998dea396699fa2e063c94a665945a`, base
  master) and PR #612 (#608 P2C1 product candidate, head
  `70d48f5eb55a664544b5160cd4675d32150bb21e`, base `sluice-600-direct-c1`):
  latest product implementation, all Mutation Oracles (C1 ×3 + P2C1 ×7), ABI
  and Header Surface, v2-r1 correspondence, all new tests and error paths.
  Performance re-verification (§12.2 serial lane) is out of this audit's
  scope.
- **Verdict: APPROVE.**
- Provenance: this file transcribes the R9 correctness-audit lane result
  delivered 2026-10-29 into the evidence archive, verbatim in substance. The
  lane executed every check listed; the closeout session that committed this
  file did not re-execute them (except where separately marked elsewhere in
  the archive).

## Findings

- **[Blocking: none]**
- [Non-blocking observation 1 — CI coverage gap on PR #612] `gh pr checks 612`
  shows no GitHub Actions checks at all (only CodeRabbit status 'skipping');
  `gh run list --branch sluice-608-p2c1-read-compose` returns empty and
  `gh api .../commits/70d48f5e.../check-runs` total_count=1. All P2C1
  correctness evidence is executor-local plus the auditor's own re-execution
  (which passed). Recommended: the closeout either trigger a CI run on the
  branch or explicitly note that #612 correctness evidence is
  local-execution-based, unlike #602 whose CI is green (6/6 at headSha
  `77e5399a`, run 40363163278).
- [Non-blocking observation 2 — P2C1 performance claims NOT verified in this
  scope] R4 §9 records E5 A/A and paired A/B as NOT_RUN_WITH_REASON (handed to
  the serial measurement lane). The audit is correctness-only per §12.1; PR
  #612 performance claims must remain 'pending serial-lane re-verification'
  until §12.2/serial lane reports — not a correctness defect, but the PR was
  not yet performance-evidenced at its head at audit time. (Closeout note:
  the serial lane has since reported INCONCLUSIVE — see
  `closure/perf-p2c1/SUMMARY.md` and
  `closure/r9-independent-performance-review.md`.)
- [Non-blocking observation 3 — duplicated composition logic classified and
  covered] After P2C1, the SEM-05 rules table (compose_progress/compose_error/
  CompositionState in `include/sluice/detail/file_semantics.hpp:243-286`) is
  consumed only by tests (tests/semantic_short_io_reference_test.cpp:24,61;
  tests/direct_composition_fault_test.cpp:450-522), while production compose
  runs the register-resident loop in `src/blocking_file.cpp:299-334`. Drift
  risk is mitigated by mirrored compile-time mutant hooks on BOTH legs and
  killers on both legs — both RETRY_ZERO_PROGRESS legs re-run RED (fault leg
  via `blocking_file_mut_retry_zero_progress`, rules leg via
  `blocking_file_mut_short_io_retry_zero_progress`) and the fault test pins
  production stops against the oracle
  (`every_stop_reason_is_pinned_against_the_oracle_error_rule`).
  Classification: test-only semantic model + justified oracle duplication; no
  runtime authority added (no new statics/globals/threads in either diff;
  mutant hooks are `#if defined` compile-time, default-off, target-scoped).
- **[Verified finding — THREAD-01 disclosure is accurate and complete]** The
  read compositions (`read_exact_at`/`read_exact`) switch steps to
  `prevalidated_pread_step`/`prevalidated_read_step` (no per-step admission)
  while write compositions keep public `write_at`/`write` with per-step
  admission (`src/blocking_file.cpp:336-395` at `70d48f5e`). In-contract
  equivalence holds: entry admission is unchanged; under caller serialization
  (`docs/explicit-io-contract.md` THREAD-01 row 'File move/close/destruction —
  Caller serialization — No outstanding borrow') file state cannot legally
  change mid-call. Out-of-contract concurrent close surfaces EBADF-class
  backend_error (EBADF absent from `kNativeErrorMappings` in
  `include/sluice/error.hpp:66-88` → backend_error) vs C1's `invalid_state`;
  `File::close` uses `std::exchange(fd_,-1)` then close with no EINTR retry
  (`src/file_resource.cpp:103-112`), so pread on the exchanged fd is
  POSIX-defined EBADF — no UAF (File object lifetime caller-owned in both
  trees), no allocation/leak. The disclosure is present in the ledger SEM-05
  row 'Missing proof or gap' column (diff `77e5399a..70d48f5e`) exactly as
  required by mission §7.4.
- **[Verified finding — `static_cast<off_t>` safety proof holds in both
  trees]** `include/sluice/detail/file_semantics.hpp:18-27` static_asserts
  off_t signed with ≥63 digits and `kMaxNativeOffset = OFF_MAX`;
  `range_is_valid` (:32-39) is overflow-safe (length-1 ≤ kMaxNativeOffset −
  offset). Every remaining cast point is dominated by the execute verdict
  (range validated) or, for the composed step offset entry_offset+confirmed,
  by confirmed < total with the entry-validated range, per the invariant
  comments at `src/blocking_file.cpp:267-268` (C1) and :294-296 (P2C1). The
  deleted `checked_posix_offset`'s rejection arm was dominated-unreachable at
  every old call site; its only unit test was rewritten to the same rule at
  the `precheck_data_op` layer (`tests/semantic_range_test.cpp:89-101`) — a
  rule-level assertion, not implementation mirroring.
- **[Verified finding — admission precedence and error mapping unchanged]**
  `precheck_data_op` (file_semantics.hpp:157-176 at `77e5399a`) orders closed
  → access → complete_empty → range → transfer-cap → execute; `rejection_error`
  maps closed→invalid_state, access/range→invalid_argument (:116-129). All 9
  call sites in blocking_file.cpp dispatch through the verdict (rejection →
  rejection_error; complete_empty → 0/CompositionOutcome{}; then null-buffer
  invalid_argument; zero-length precedes null per old precheck() ordering).
  Composition stop rules match root SEM-05 (`docs/explicit-io-contract.md:459-477`):
  EOF-before-full and write-no-progress stop with accounted effects
  (stalled_outcome leaves remaining=accounted default),
  primitive_error/impossible_count with unknown certainty and preserved error
  (primitive_error_outcome + failure_publish_certainty()=unknown),
  impossible_count → invalid_state — field-for-field equivalent to the C1
  direct_outcome it replaced (CompositionOutcome declaration order confirmed
  at `include/sluice/blocking/file.hpp:42-49`, aggregate-init order matches).
- **[Verified finding — v2-r1 correspondence]** Both ledger edits are
  Status/Evidence/Gap evidence write-backs only (git diff `bdfd0d8d..77e5399a`
  and `77e5399a..70d48f5e` on docs/roadmap/v2-conformance.md touch exactly one
  SEM row each); the root contract text `docs/explicit-io-contract.md` is
  untouched by both PRs. No public API adopted: the only include/ file either
  PR touches is the uninstalled detail header; diff --name-status shows only
  M entries, no new files.
- **[Boundary compliance]** No PR merged (both verified OPEN via gh pr view);
  no push/force-push performed; no new public API adopted; no contract text
  modified; frozen worktrees Sluice-600-base/Sluice-600-wt untouched; no
  benchmark run by this auditor; all failures expected as negative assertions
  — no oracle weakened. Two audit worktrees created
  (`/home/jnhu/Source/Sluice-r9-audit-c1` detached `77e5399a`,
  `/home/jnhu/Source/Sluice-r9-audit-p2c1` detached `70d48f5e`), left in
  place at the audited anchors with clean source trees; no other agent's files
  touched.

## Independently re-checked (executor: R9 auditor)

- PR/branch identity: `gh pr view 602 --json state,headRefOid,baseRefName,mergeable`
  → OPEN/`77e5399a`/master/MERGEABLE; same for #612 →
  OPEN/`70d48f5e`/sluice-600-direct-c1/MERGEABLE; `git ls-remote origin` →
  both remote branch heads match anchors, master=`bdfd0d8d`. No rewriting of
  #602 commits.
- Read every diff hunk of both PRs: `git diff bdfd0d8d..77e5399a` (5 files)
  and `git diff 77e5399a..70d48f5e` (5 files) — src/blocking_file.cpp,
  include/sluice/detail/file_semantics.hpp, tests/semantic_range_test.cpp,
  tests/direct_composition_fault_test.cpp, xmake/tests.lua,
  docs/roadmap/v2-conformance.md.
- Read full source at audited heads: detail/file_semantics.hpp (77e5399a,
  complete), include/sluice/error.hpp:40-110 (errno mapping),
  detail/posix_retry.hpp (retry_on_eintr: retries only result<0 &&
  errno==EINTR), src/file_resource.cpp:95-112 (close semantics),
  src/blocking_file.cpp composition entries at 70d48f5e:336-400,
  CompositionOutcome at include/sluice/blocking/file.hpp:42-49.
- v2-r1 root reading: THREAD-01 matrix (`docs/explicit-io-contract.md:1003-1029`)
  and SEM-05 section (:459-477) — implementation/disclosure correspondence
  verified.
- BASE_GREEN at C1 head `77e5399a` [RUN]: release `xmake f -m release
  --liburing=n -y && xmake -y && xmake test` → 58/58, 0 failed (matches R3 §1
  and CI).
- C1 mutation gate re-executed at `77e5399a` [RUN]: three mutant targets
  built and run → all exit=1 with exactly one FAIL line:
  `read_with_illegal_offset_reports_invalid_argument` /
  `sequential_read_on_write_only_file_rejected_upfront` /
  `resize_overflow_reports_invalid_argument` — identical to the ledger and R3
  §2 kill mapping. Kill-chain validity confirmed: EINVAL absent from
  kNativeErrorMappings (error.hpp:66-88) so any errno fallback lands on
  backend_error, never invalid_argument.
- BASE_GREEN + RESTORED_GREEN at P2C1 head `70d48f5e` [RUN]: release suite →
  58/58 before mutants and again after (RESTORED); `git status --porcelain --
  src/ include/ tests/ xmake/ docs/` empty in both audit worktrees.
- P2C1 seven-mutation gate re-executed at `70d48f5e` [RUN]: 7 targets built
  (exit 0) and run → all seven exit=1 with exactly one named FAIL line each,
  matching R4 §5 one-for-one (see `WSL2_VALIDATION_HANDOFF.md` §5 table).
- C1 admission mutant spot-run on the stacked P2C1 tree [RUN]:
  `blocking_file_mut_empty_before_closed` at `70d48f5e` → exit=1, FAIL
  `sequential_read_on_write_only_file_rejected_upfront` (C1 oracle intact
  under P2C1).
- ABI/Header Surface gate re-executed at `70d48f5e` [RUN]:
  `python3 scripts/verify_v2_package.py --mode release --manifest-out
  /tmp/r9audit_v2_manifest.json --keep-prefix` → all gates PASS (24/24, incl.
  V2_B_PACKAGE_PREFIX_READY, CLEAN_ROOM_*, all P1 contract suites, ODR layout,
  NEGATIVE_V2_REMOVED_HEADERS_UNAVAILABLE, NEGATIVE_V2_SEAM_UNAVAILABLE,
  NEGATIVE_V2_MUTANT_DEFINES_NOT_INSTALLED, NEGATIVE_ASYNC_SYMBOLS_NOT_IN_CORE);
  manifest VERIFICATION_HEAD=`70d48f5e…`, VERIFICATION_HEAD_DIRTY=False,
  HEADERS = exactly the 11 contract headers with
  sluice/detail/file_semantics.hpp absent.
- Public-header blob identity [RUN]: `git ls-tree` →
  include/sluice/blocking/file.hpp = `e62a33c39425de0fc0eb2ea10d34fa33c3177447`
  at bdfd0d8d, 77e5399a and 70d48f5e; only include/ file in either PR diff is
  the uninstalled detail header.
- CI state [RUN]: `gh pr checks 602` → 6/6 pass (run 40363163278, headSha
  `77e5399a`); `gh pr checks 612` → no Actions checks, only CodeRabbit
  'skipping'; `gh run list --branch sluice-608-p2c1-read-compose` → empty.
  NOT run by the auditor: P2C1 performance gates (A/A, paired A/B),
  Debug/ASan/UBSan/asanubsan configurations, and the liburing=y 79/79 suite —
  these remain executor-reported [TRANSCRIBED] from R4 §1/§6; no benchmark
  executed per hard boundary 6.
