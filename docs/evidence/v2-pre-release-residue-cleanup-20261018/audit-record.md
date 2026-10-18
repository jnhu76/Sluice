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
| `tests/shutdown_lifecycle_test.cpp:94` | `(the release-job flake, #483)` | issue number dropped, hazard text kept (review fix) |
| `.github/CONTRIBUTING.md` tests section | "The minimum Linux Clang Debug baseline is documented in `AGENTS.md`" — no such baseline exists in AGENTS.md (dead v1-era reference) | replaced with the actual AGENTS.md testing discipline (review fix) |

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
- `formal/tla/**` campaign-era provenance comments (C2-E/E2/F2/F3/…) and
  the model-gate script `scripts/verify_progress_source_model.sh` (`#444` /
  C2-B / C2-E verdict-catalogue anchors): the models are the living
  VERIFY-06 evidence base; the comments map modeled transitions and gate
  verdicts to their origin. Comment-only edits would still change model /
  gate sources and trigger a model re-run obligation for zero contract
  benefit.
- `tests/semantic_reference_case_test.cpp` `owner #400` DEFERRED markers:
  live disposition pointers naming the owner of the deferred request-side
  representation evidence, printed by the test itself.
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
  The recorded manifests were produced by two sequential verifier runs and
  pin two different heads (fields read from the JSON, not inferred):

  ```text
  MANIFEST_N_HEAD = 5128013502378fa40f0b83ea302221e70ae7b7e6
  MANIFEST_Y_HEAD = 8b78a2ce20c4fd0d216eab2c532aae411f708ef6
  MANIFEST_N_DIRTY = False
  MANIFEST_Y_DIRTY = False
  MANIFEST_N_GATES = 24/24 PASS
  MANIFEST_Y_GATES = 24/24 PASS
  V2_PACKAGE_HEADERS = 11 (both profiles, equal to the libraries.lua allowlist)
  ```

  The noliburing run verified the tree at `51280135` (clean; its outputs
  are the two noliburing files). The first liburing attempt, still at
  `51280135`, failed the `V2_B_PROVENANCE_RECORDED` gate — the untracked
  noliburing outputs made the tree dirty as non-output dirt — and was
  rerun after those outputs were committed in `8b78a2ce`; the failed
  intermediate manifest is visible in the `8b78a2ce` tree and the passing
  rerun (clean at `8b78a2ce`) replaced it in `bccd8131`. Both heads are
  Markdown-only follow-ons of the battery head `8c2170bb` (`8c2170bb..51280135`
  changes `docs/README.md`, the ledger and this record only), so the
  verified C++/build content is identical to the battery head. From each
  manifest head to the final PR head `0cc89df0` the changed files are
  Markdown/JSON under `docs/` and `.github/` plus one comment-only source
  edit (`tests/shutdown_lifecycle_test.cpp` in `861e1f26`, the `#483`
  citation drop), so no executable token, target, oracle or package-surface
  change separates a verification head from the final head and the 24/24
  receipts remain valid there; the final-head token check below covers the
  whole branch.
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

## CI expectation and observed verdicts

The PR (#549) touches `tests/**`, `xmake/**` and root/docs Markdown.
`.github/workflows/ci.yml` triggered on the `tests/**` and `xmake/**` path
filters and every job passed on the PR head:

| Job | Verdict |
|---|---|
| C++ comment authority guard (self-tests + scan) | PASS |
| Package verifier surface self-tests | PASS |
| Linux debug build + tests | PASS |
| Linux release build + tests | PASS |

## Review passes (in-session)

Two adversarial review passes were run over the branch (architecture /
C++ / concurrency, and documentation / release accuracy). The originally
planned independent-context reviewer agents could not be launched: the
hosting account's subagent quota was exhausted at review time (reset
2026-10-19 00:51). Both passes were therefore executed in-session as
separate structured checklists against the branch tree — they are review
passes, not independent-context reviews — and the PR is left
OPEN for owner review; the deviation is disclosed here rather than
papered over. A separate-context agent review was later obtained and is
recorded in the next section.

Reviewer A (architecture) — **PASS_WITH_NOTES**:

- Diff-class proof: comment-stripped (whitespace-normalized) sources are
  token-identical to master for all non-Markdown files modified at the
  review-time head — eight; the ninth (`tests/shutdown_lifecycle_test.cpp`)
  arrives with the review fix below and is covered by the final-head
  token check in the reconciliation section. Root spec, AGENTS.md and
  CHANGELOG.md byte-identical; no target, identifier, oracle,
  assertion, `add_tests` registration or public-surface change.
- Reworded-comment correspondence re-derived from source: the shipped
  backends' executor handoff is `RequestCore::claim_execution`
  (`threadpool_backend.cpp:405`, `uring_transport.cpp:113/152`); the
  shutdown fail-fast/poison oracles live in `shutdown_lifecycle_test.cpp`
  (poison convergence plus the forked SIGABRT death matrix); the uring f1
  oracle uses `detail::make_context` + submit test hooks (internal seam).
- Package boundary unchanged (allowlist token-identical; 24/24 verifier
  gates both profiles).
- Finding (fixed): `tests/shutdown_lifecycle_test.cpp:94` still carried an
  issue-number citation (`#483`) in the deadline-budget comment — dropped
  in commit `861e1f26`; both shutdown suites re-run green (10 TP / 17
  uring scenarios PASS).
- Residue sweep: remaining letter-code/issue references in active files
  are the justified keeps enumerated above (RG5 oracle vocabulary, model
  provenance, G0 evidence linkage, `owner #400` disposition pointers).

Reviewer B (documentation / release) — **PASS_WITH_NOTES**:

- `--workers` claims verified empirically on the built release binaries:
  grep rejects with exit 2, hash with exit 1, tail with exit 1 (exact
  recorded messages); copy accepts 1..64, `--workers 0`/`65` are usage
  exit 1, `--workers 2` copies byte-identically with `atomic=on` and no
  leftover temp file.
- README/diagram: no stale `HOST`/`IoContext` node remains; the evidence
  link resolves; "configurations outside that record are untested" matches
  the ledger/CHANGELOG disclosure; EN/ZH make equivalent claims.
- docs/README evidence-dir list matches `docs/evidence/` exactly
  (9 = 9); ledger diff is append-only (0 removed lines); CHANGELOG
  byte-identical; 11 changed Markdown files, 0 broken relative links.
- Finding (fixed): `.github/CONTRIBUTING.md` claimed "the minimum Linux
  Clang Debug baseline is documented in `AGENTS.md`" — no such baseline
  exists there (dead v1-era reference); replaced with the actual AGENTS.md
  testing discipline.
- Remaining convergence phrasing in the READMEs is the allowed kind:
  "open gaps" names the ledger's live gap column, and the v1 Phase M/F
  sentence is explicit completed-history framing.

No P0/P1 findings; both P2-class findings were fixed directly and
re-verified. The final review heads include those fixes (`861e1f26` and
the CONTRIBUTING fix commit).

## Independent review (separate-context agent, 2026-10-18)

A separate-context reviewer agent (fresh context; read-only adversarial
instructions; no access to the author's working conclusions beyond the
repository itself) reviewed the PR at head `0cc89df0`. It derived its own
evidence: full PR diff and per-commit file lists, its own comment-stripped
token comparison, manifest JSON fields, the allowlist enumeration, CLI
parser reads, link checks, `gh` CI state, and SHA ancestry. Verdict:
**REVISE** — the diff class, package boundary, manifest genuineness,
interval equivalence, append-only ledger and documentation claims all
survived its counterexample attempts; the required revisions were two
sentences inside this record.

Findings and dispositions:

| # | Severity | Finding | Disposition |
|---|---|---|---|
| F1 | P1 | This record's sentence "The recorded manifests next to this record were produced at `8b78a2ce`" was false for the noliburing manifest (its own `VERIFICATION_HEAD` is `51280135`, and `8b78a2ce` contains it); the parenthetical "intervening commits add evidence files only" also glossed two Markdown follow-ons | Fixed in this reconciliation commit (provenance block above) |
| F2 | P2 | The failed first liburing verifier run (`V2_B_PROVENANCE_RECORDED` false at `51280135`, non-output dirt) was discoverable only from Git history; the record never disclosed it | Fixed in this reconciliation commit (provenance block above) |
| F3 | P3 | The section title "Independent review" overstated two in-session passes | Fixed: retitled "Review passes (in-session)"; this section now holds the agent review |
| F4 | P3 | Reviewer A's "all eight non-Markdown files" was stale at the final head (nine) | Fixed above (eight at the review head, ninth identified) |
| F5 | P3 | PR body said "CI jobs are the same three" while four check entries report | PR body clarified (three workflow jobs; the build job is a debug/release matrix) |

The reviewer's stated final condition: after the two-sentence provenance
amendment (and optionally the P3 tweaks) the PR is a clean PASS.

Post-amendment confirmation round (2026-10-18, same separate-context
agent, working-tree amendment at HEAD `0cc89df0`): items 1–4 above
confirmed item-by-item against the reviewer's own git/JSON evidence
(per-manifest heads and the failed-run reconstruction, the retitle, the
eight/ninth recount, and every number in the reconciliation block,
including the correct 9 commits where the reviewer's round-1 count of 10
was its own miscount, acknowledged by it). The round raised two new
findings on the amendment itself: NEW-1 (P2 — the F5 cell above asserted
a PR-body clarification that had not yet been performed) and NEW-2 (P3 —
this section's closing sentence promised a confirmation record "below"
that did not exist). Fixes applied before this commit was finalized: the
PR body was edited (REST PATCH, 2026-10-18T14:41:41Z) to state the three
workflow jobs and the debug/release matrix, making the F5 disposition
true as written; and this paragraph replaces the dangling pointer. A
closing confirmation round over exactly these two fixes follows; its
outcome is recorded below, along with the single post-review addition
that proved necessary: the CI-trigger correction in the reconciliation
section, forced by an observed run that post-dated the closed review.

Closing-round outcome (2026-10-18): both fixes re-verified — the live PR
body shows the matrix heading at `updated_at` 2026-10-18T14:41:41Z with
the old string gone, and the F5 cell and the replaced paragraph are true
as written; three fresh counterexample attempts failed; no new
inaccuracy. Verdict, quoted verbatim from the reviewer's closing round:
"PASS — at 0cc89df0 with this working-tree amendment committed, PR #549
is approved: every executable-preservation, provenance and documentation
claim has survived three independent adversarial verification rounds, all
identified inaccuracies are fixed and re-verified true as written, and no
P0/P1/P2 finding remains open." Beyond the post-review CI-trigger
correction in the reconciliation section, this closing-round paragraph is
the only content added after that round.

## Final-head reconciliation (2026-10-18)

Counts at head `0cc89df0` (from `git diff --name-status` and `gh pr diff`,
cross-checked against the agent reviewer's independent recount):

```text
CHANGED_FILES = 24 (19 modified, 5 added, 0 deleted)
NON_MARKDOWN_MODIFIED = 9 (7 C++ tests, 2 xmake Lua)
CPP_FILES = 7   XMAKE_FILES = 2   ADDED_JSON = 4
CHANGED_MARKDOWN = 11 (10 modified + this record, added)
COMMITS_AT_0cc89df0 = 9 (the provenance reconciliation commit makes it
                    10; the CI-trigger correction below makes it 11)
```

Equivalence at the final head, re-derived after all review fixes:
comment-stripped (C++ `//`/`/* */`, Lua `--`/`--[[ ]]`) and
whitespace-normalized sources are token-identical to master `2171d37f`
for all nine non-Markdown files; `include/`, `src/`, `scripts/`,
`formal/`, `bench/` and the untouched xmake files have zero diff; the
installed-header allowlist is unchanged and resolves to the same 11
headers the manifests record. Therefore production source, build graph,
test oracles, verifier logic and package surface are all unchanged from
master, and the local battery at `8c2170bb` plus the two manifest runs
remain valid evidence at the final head modulo the disclosed comment-only
delta.

CI: all four check entries passed at `0cc89df0` (C++ comment authority
guard, Package verifier surface self-tests, Linux debug build + tests,
Linux release build + tests; the workflow defines three jobs, the build
job being a debug/release matrix). The reconciliation commit `e79d380c`
(touching only this record) triggered a fresh run as well — run
39137354129, same four entries passing — because the `ci.yml`
`pull_request` paths filter evaluates the cumulative PR diff, which
contains the `tests/**` and `xmake/**` changes; a docs-only push to this
PR re-runs CI after all. An earlier sentence in the `e79d380c` version of
this paragraph asserted the opposite (no trigger; argument resting on
equivalence alone); the observed run corrected it, and any further
evidence-only push re-runs the same green matrix by the same mechanism.

## Release-gate impact

RG1–RG7 evidence is untouched: no gate's owner file, test target, oracle
name or evidence record changed. The boundary gates' live re-verification
(both package profiles at the branch head) reproduces the RG2/RG3 substance
at this head. Release acceptance for v0.0.2 remains the recorded
`v2-release-acceptance-20261017/` matrix; this round introduces no source
change that could affect it (comment/Markdown-only diff).
