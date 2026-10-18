# v0.0.2 pre-release residue and naming audit

## Scope and baseline

| Field | Value |
|---|---|
| Base | master `2171d37f430febc826a25997568823ca1a377e4a` (= origin/master, clean; matches the task's REFERENCE_MASTER) |
| Branch | `chore/v2-pre-release-residue-cleanup` |
| Date | 2026-10-18 |
| Diff class | Comments in build/test files + Markdown only; no production C++, no target/identifier/oracle/assertion change, no root-spec change |
| Out of scope and untouched | root specification, frozen evidence records, `docs/archive/**`, Git history, CHANGELOG entries, the v0.0.2 tag and GitHub release |

Goal statuses:

```text
ACTIVE_ARCHITECTURE = V2_ONLY                  (confirmed, no v1 runtime residue found)
LEGACY_RUNTIME_RESIDUE = NONE_UNJUSTIFIED      (none found on any active surface)
STAGE_NAMING_DEBT = NONE_UNJUSTIFIED           (residual codes are justified keeps, below)
README_CURRENT_STATE = ACCURATE                (fixed in this round)
RELEASE_GATE_EVIDENCE = PRESERVED_OR_REVALIDATED (no gate evidence touched; boundary re-verified live)
```

## Inventory method

Tracked-file sweep (`git ls-files`, 1080 files) over `include/ src/ apps/
tests/ bench/ formal/ xmake{,.lua} scripts/ .github/ .claude/` plus the root
Markdown, `docs/` active pages and `research/`, cross-checked against the
xmake target graph, the installed-header allowlist and the frozen evidence
citations. Keywords (case/identifier variants, word boundaries):
`v1, legacy, old, retired, deprecated, compat, migration, Scheduler, Fiber,
Completion, ApplicationRuntime, BlockingIoContext, RequestArena,
RequestHandle, WaiterToken, RoutingLease, stackful_io_host, fiber_ctx,
Phase, Stage, F0–F6, B1/B2, C1/C2, E1/E2, G0–G2, R0/R1, candidate,
experiment`. Scanner-output files under `docs/evidence/**` (logs, digests,
argv) were excluded as frozen artifacts. Historical trees
(`docs/archive/**`, supersession banners, dated evidence) were classified,
not edited.

## Findings and classification

Class A (retired production residue): **none**. `src/` and `include/` carry
no retired-component references; the host bundle headers are absent from the
tree; `AsyncBackend` appears in installed headers only as private seams
(private constructor + `friend detail::ContextFactory`/`TestAccess`), and
the clean-room verifier's pinned-diagnostic seam negatives pass.

Class B (unused build/test/tool residue): **none**. Every xmake target and
script found has a current consumer; no orphaned targets or dead tooling.

Class C/D (stage-name residue / stale current documentation) — fixed:

| Location (base SHA) | Was | Action |
|---|---|---|
| `xmake/tests.lua` §semantic group | `A1 shared File semantic oracle` | prefix dropped |
| `xmake/tests.lua` §direct group | `A2 direct closure` | reworded to direct-execution-surface description |
| `xmake/tests.lua` §app group | `A2 app-consumption tests` | prefix dropped |
| `xmake/tests.lua` error-domain mutants | `#526 named mutation builds` | `Named mutation builds` |
| `xmake/tests.lua` file-move group | `#527 (F6-F13) File-owner moves…` | campaign prefix dropped |
| `xmake/tests.lua` submit-concurrency group | `#527 (F6-F14) concurrent submissions…` | campaign prefix dropped |
| `xmake/tests.lua` request-scope TSan note | `#521: TSan replaces…` | issue prefix dropped |
| `xmake/libraries.lua` arflags note | `Reproducible archives (#489)` … `F1 frozen archive baselines` | issue prefix and F1 dropped |
| `tests/support/pipelined_copy_oracle_support.hpp:6` | `exactly like a v1 request backend` | `exactly like the shipped request backends` |
| `tests/uring_progress_race_test.cpp:1238` | `Kept on the compat spellings` | `Kept on the internal seam spellings` |
| `tests/threadpool_external_loop_test.cpp:1031` | `C2-D stop/settle/detach` | `Stop/settle/detach ordering` |
| `tests/progress_source_ownership_test.cpp:61` | `The C2-C convergence removed…` | `No poll-only readiness-fd lend exists…` |
| `tests/threadpool_progress_race_test.cpp:608` | `(the sticky outcome taxonomy is C2-D scope)` | parenthetical deleted |
| `tests/package/consumers/contract_shutdown.cpp:3` | `internal shutdown oracles in the F1 evidence matrix` | `internal shutdown lifecycle oracles` (the F1 matrix left the tree in the 2026-10-17 hygiene round) |
| `README.md` / `README.zh-CN.md` | convergence-era phrasing: "open items", "Current implementation and convergence", "a successful build is not evidence that every v2 target configuration has been implemented", adoption-era zh scaffolding sentence | ledger bullet now names the release-gate status; section retitled "Current implementation"; build note now points at the recorded release-acceptance matrix and the untested-configuration disclosure |
| `README.md` diagram | `HOST["Progress owner / Host"]`, `IoContext` node labels | node id `PROGRESS_OWNER["Progress owner (application side)"]`; labels read `AsyncIoContext` (the installed public type); all edges updated; Mermaid remains informative, root wins |
| `README.zh-CN.md` | `显式 IoContext` | `显式 AsyncIoContext` |
| `docs/README.md` | "becomes the sole normative root when its adopting PR merges" (future tense), "any remaining implementation items" | past tense; ledger bullet names recorded evidence and release-gate status |
| `CONTEXT.md` | "including the open items and the release gate" | "including the release-gate status" |
| `apps/sluice-grep/README.md` | `approved removal for F3 Step 3 on 2026-10-11`; `retired with the F2 contraction` | date kept, stage codes dropped; `retired with the v1 surfaces` |
| `apps/sluice-hash/README.md` | `approved removal for F3 Step 3 on 2026-10-11` | date kept, stage code dropped |
| `apps/sluice-copy/README.md` | `retired by F2/F5` | `retired with the v1 surfaces` |
| `.github/CONTRIBUTING.md` | v1-era area list (`Completion publication`, `scheduler wake/progress`, `synchronization primitives`) and dead `AGENTS.md §4` reference | v2 vocabulary; references AGENTS.md's engineering order/architecture discipline by name |
| `.github/pull_request_template.md` | v1-era examples (`RequestKey/RequestSlot`, `Completion publication`, `Scheduler wake/progress`, `Runtime ownership`); `Completion/publication authority` checklist line | v2 vocabulary; `Terminal/publication authority` |

Class E/F/G (provenance / domain terminology / stable evidence IDs) —
justified keeps, verified against current citations:

- Oracle ID vocabulary `r1_..r15_` (ThreadPool), `k1_..k15_/u1_/u2_/u2b/f1_/f2_`
  (io_uring) and their `R1:`/`F1 oracle:` comment prefixes: cited verbatim by
  the frozen RG5 record
  `docs/evidence/v2-release-acceptance-20261017/prog02-settlement-notification-oracle.md`
  (coverage classes 1–15); renaming would orphan frozen release-gate
  evidence. The `M-C1` class name matches `formal/tla/ProgressSource.tla`'s
  mutant annotations.
- Root requirement-ID prefixes in test names (`v01_..v03_`, `v15_..v17_`,
  `v18a..c_`, `v19_`, `v27_`, `w01_/w02_/w03_`) and mutant families
  (`SLUICE_ERR01_*`, `SLUICE_THREAD01_*`, `SLUICE_LIFE_*`): VERIFY-04
  scenario and W-workload IDs with current contract meaning (kept by the
  2026-10-17 hygiene round's rule).
- `formal/tla/**` campaign-era provenance comments (C2-E/E2/F2/F3/…): the
  models are the living VERIFY-06 evidence base; the comments map modeled
  transitions to their origin. Comment-only edits would still change model
  sources and trigger a model re-run obligation for zero contract benefit.
- ADR-0004 filename and `M1..M12` mutation numbers: stable ADR identity
  cited by ledger G7 and `scripts/verify_tla.sh`.
- `docs/roadmap/b1-1-request-core-evidence.md`: dated slice record under its
  2026-10-15 supersession banner; cited with exact line numbers by the
  frozen `issue-524-requestcore-convergence-20261015/` records.
- `bench/*` `G0 (#403)` references and benchmark banner strings: provenance
  linkage to the frozen preregistered measurement matrix
  (`docs/evidence/g0-403-performance-baseline-20261015/`); the printed
  banner strings are hash-pinned by `bench/verify_hashes.py`, so changing
  them would break the frozen evidence.
- `scripts/verify_v2_package.py` manifest `FORMAT` strings
  (`v2-package-manifest-v1`, `v2-archive-baseline-v1`): stable evidence
  format identifiers read/written by the standing verifier.
- Domain vocabulary: `SlotPhase`, observer `retired`/retirement, transport
  "retired ledger entries" (OBS-02/REQ-05/BACKEND-03 terms), `SafeCommitStage`
  (atomic-output commit stages), backend `SubmitStage` fault-injection
  stages, test-local measurement phases ("Phase 1/2 validates…" in
  `app_copy_pipeline_oracle_test.cpp`, external-loop handoff phases).
- `README.md`'s labeled historical links (archive architecture snapshot,
  research conclusions) and the app READMEs' `--workers` migration sections:
  the task's own allowed v1 categories (compatibility-break and migration
  notes, clearly labeled historical entries). `--workers` claims re-verified
  against the parsers: copy accepts 1..64 (CLI-capped, usage exit 1);
  tail/grep/hash reject all spellings before file open with their recorded
  exit codes (1/2/1).
- `sluice-copy` "Version A/B/C" mode labels: stable app vocabulary that
  appears in the binary's own help/usage strings, which the CLI oracles
  assert byte-exactly; renaming would change CLI-visible output or desync
  the README from the binary. Kept (each already carries a descriptive
  gloss).
- `.github/workflows/open-code-review.yml` "legacy machinery" prompt line:
  instructs the reviewer to distinguish historical evidence from current
  claims; `.claude/skills/*` "legacy" hits concern skill-file mechanics, not
  project v1. GitHub Action `@v1` / endpoint `/v1/` strings are external
  version identifiers.

Considered and rejected: extending
`scripts/check_cpp_comment_authority.py` to flag letter-coded comment
prefixes generally. No standing duty names it; the existing high-confidence
rules were tuned to avoid false positives, and this round's residual class
is justified-keep vocabulary that such a rule would flag wrongly. No new
mechanism added.

## Negative boundary evidence

Baseline (`2171d37f`, clean tree) and branch head (`8c2170bb`, clean tree):

- `python3 scripts/test_verify_v2_package.py` — OK (both points).
- `python3 scripts/verify_v2_package.py` both profiles — 24/24 gates PASS
  each, exit 0; installed surface exactly the 11 contracted headers;
  `NEGATIVE_V2_REMOVED_HEADERS_UNAVAILABLE` (deleted host headers absent
  from tree and prefix), `NEGATIVE_V2_SEAM_UNAVAILABLE` (seam construction
  from installed headers fails with pinned diagnostics),
  `NEGATIVE_ASYNC_SYMBOLS_NOT_IN_CORE`, ODR macro-independence all PASS.
  The recorded manifests next to this record were produced at `8b78a2ce`
  (a documentation-only follow-on of the battery head `8c2170bb`: the
  intervening commits add evidence files only, so the verified C++/build
  content is identical).
- `python3 scripts/check_cpp_comment_authority.py .` — clean (branch head).

## Validation battery (branch head `8c2170bb`)

Local host: WSL2, kernel 6.18.54.1-microsoft-standard-WSL2, xmake
2.9.7+20261005, GCC 15.2.0 (xmake-reported), liburing 2.13 dev present.
Toolchain versions differ from the recorded release matrix (Fedora 44 /
xmake 3.0.9 / GCC 16.2.1 / clang 22.1.8); this battery is regression
evidence for this round's diff, not a re-statement of the RG6 matrix.

| Check | Result |
|---|---|
| `git diff master...HEAD --check` | clean |
| debug build + `xmake test` (liburing=n) | 56/56 passed |
| debug build + `xmake test` (liburing=y) | 74/74 passed |
| release build + `xmake test` (liburing=y) | 74/74 passed |
| release build + `xmake test` (liburing=n) | 56/56 passed |
| package verifier noliburing (branch head) | 24/24 gates PASS, exit 0 |
| package verifier liburing (branch head) | 24/24 gates PASS, exit 0 |
| comment-authority scan | clean |
| verifier self-tests | OK |
| markdown links in changed files | all resolve |
| `--workers` claims vs parsers (copy/tail/grep/hash) | consistent |

No sanitizer cells were run: the diff is comments and Markdown with no
executable-semantics change (the touched C++ files are comment-only edits
verified by inspection of `git diff master...HEAD`), and the deterministic
suites above cover the compiled artifacts of every touched file. No TLA+
re-run: no model source or modeled transition changed.

## CI expectation

The PR touches `tests/**`, `xmake/**`, `scripts/**` (none) and root/docs
Markdown. `.github/workflows/ci.yml` triggers on the `tests/**` and
`xmake/**` path filters, so all three jobs (comment-authority guard,
package-verifier self-tests, debug+release builds with `xmake test`) are
expected to run on the PR; the record will be updated with the observed CI
verdicts. Local equivalents of all three jobs pass as recorded above.

## Release-gate impact

RG1–RG7 evidence is untouched: no gate's owner file, test target, oracle
name or evidence record changed. The boundary gates' live re-verification
(both package profiles at the branch head) reproduces the RG2/RG3 substance
at this head. Release acceptance for v0.0.2 remains the recorded
`v2-release-acceptance-20261017/` matrix; this round introduces no source
change that could affect it (comment/Markdown-only diff).
