# F1 Clean-Room Package Baseline (#402 Phase F1)

- **Status**: in progress. Gates recorded per phase; nothing here authorizes deletion or a supported-profile claim.
- **Execution**: F1 branch `feat/402-f1-clean-room-baseline`; umbrella #402; F0 adopted via #454 / PR #455.
- **Normative root**: `docs/explicit-io-v1-final-decision.md` v1-r3. Authority order: root > adopted ADR (ADR-0004) > #402 phase policy > adopted F0 evidence/policy > ledger/test/model/source evidence > implementation accident.
- **F1 modification boundary**: additive install/package rules, export/config metadata, clean-room external consumers, manifests, verification scripts, F1 docs. No public API removed/renamed/internalized; no consumer migrated; no semantic change; no F0 UNDECIDED disposition resolved (`RequestHandle` stays F2-owned UNDECIDED; ADR-0003 stays PROPOSED).

## 0. F1-A — Reality re-freeze

All facts below were re-verified at the F1 branch point; nothing is carried over
from a prompt or an older record without live evidence.

```text
LIVE_MASTER_SHA   = a34d96c64f5ac8647b7d0f57c5511e68301b0f0e   (git rev-parse origin/master, 2026-10-07)
ROOT_REVISION     = v1-r3                                       (root header table; implementation baseline field unchanged)
ISSUE_402_STATE   = OPEN — umbrella; F0 gate CLOSED/ADOPTED recorded (comment 6032220967 + status table edit)
ISSUE_454_STATE   = CLOSED / completed (closed 2026-10-07 after adoption record)
PR_455_STATE      = MERGED at 2026-10-07T06:11:38Z
PR_455_MERGE_SHA  = a34d96c64f5ac8647b7d0f57c5511e68301b0f0e (== LIVE_MASTER_SHA)
F0_FINAL_REVIEW   = "F0 Round-3 human review — FINAL APPROVE", PR #455 comment at 2026-10-07T06:07:37Z,
                    verified against head 7ed62f2208c19182ffa1c07f3782b079c33bc353
F0_ADOPTED_ON_MASTER = yes — all three files live on master at a34d96c6:
                    docs/review/f0-role-profile-census.md
                    docs/review/f0-package-profile-policy.md
                    docs/review/f0-consumer-migration-dag.md
E_REQUIRED_CLOSURE_ADOPTED = PASS (ADOPTED_SHA eb7bf055ad4fce5540de0f876609ab38c45e385b, #402 §0)
F0_ADOPTED_SHA    = a34d96c64f5ac8647b7d0f57c5511e68301b0f0e
F1 authorization  = kill gate (§2 of the F1 brief) all four conditions live-verified
                    → F1_PRODUCTION_PREPARATION = AUTHORIZED
```

### 0.1 Packaging reality re-verified at a34d96c6

Re-ran the census's install/package scan against the live tree (not the F0
baseline SHA):

- `grep -ri "install|package|set_prefixdir|set_targetdir" xmake.lua xmake/*.lua`
  → **no install/export/package rule exists**; the only hits are the public
  `uring` link (`xmake/libraries.lua:39`) and comments. `F1 starts from
  NO_PACKAGE_BASELINE` — confirmed live, matching census §1.2 / ledger row
  "Installed-header / package surface = NOT_ASSESSED".
- Targets confirmed live (`xmake/libraries.lua`): `sluice_core` static, default,
  public includedir `include/`, links nothing external; `sluice_async` static,
  opt-in (`set_default(false)`), deps `sluice_core`, public includedir
  `include/`, `SLUICE_HAS_LIBURING` public define + `uring` public link only
  under `--liburing` (the single config point; ODR-critical, DAG X-12).
- Header inventory (census §1.3, 81 headers: 21 core + 1 blocking + 36 async +
  17 async/detail + 4 detail + 2 experimental) accepted as the F0 record; F1
  re-verifies the *installed* subset mechanically at install time (§3 manifest)
  rather than re-running the whole census.
- Load-bearing F0 closure facts F1 must preserve (policy §1.3/§2, DAG X-03/X-12):
  no umbrella header; `request.hpp`/`async_io_context.hpp`/backends transitively
  expose `detail/` substrates; the OPTIONAL_CANDIDATE host closure carries
  `cancel.hpp` + `fiber.hpp` + `fiber_ctx.hpp` until F3 fixes the H-09 boundary
  (rows H-30/H-31); `SLUICE_HAS_LIBURING` is an ODR-critical public define.

### 0.2 Environment facts (recorded for reproduction)

```text
TOOLCHAIN   = g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0; clang 21.1.8 present; xmake v2.9.7+20250101
ARCH        = x86_64
KERNEL      = Linux 6.18.33.2-microsoft-standard-WSL2 x86_64; io_uring_disabled = 0 (usable)
FILESYSTEM  = ext4 (the scratch prefixes and all probe working files)
LIBURING    = 2.9 built from source (github.com/axboe/liburing, tag liburing-2.9) into
              /tmp/f1-deps/uring-prefix (liburing.a/.so.2.9) — system liburing absent
              (no pkg-config entry, no /usr/include/liburing.h); sudo unavailable
PROVENANCE  = manifests record SOURCE_SHA (branch point a34d96c6…) and
              GENERATING_COMMIT (the F1 commit carrying the install rules and
              verifier); the F1 diff touches no production source
```

P2 consequences: the liburing-enabled build and its clean-room probes run
against the recorded user-prefix liburing; the manifest records that
environment. liburing-independent profiles (P0/P1/P3) are the default-configuration
evidence and do not touch it.

## 1. F1-B — Additive package prefix

Design (mechanism justification per AGENTS.md):

- `WHY_NEEDED`: the baseline must exist before any contraction so F2–F5 compare
  against a real installed package, and ledger row "Installed-header / package
  surface" (NOT_ASSESSED) gets its first executable assessment.
- `WHY_EXISTING_MECHANISM_INSUFFICIENT`: no install rule exists at all (§0.1);
  the existing `direct_w01_consumer_probe` builds inside the source/build tree
  and is not a package boundary.
- `STATE_COST`: two `add_headerfiles` declarations on the existing library
  targets (native xmake install mechanism; no second build system), one
  verification script, and standalone consumer sources. No runtime state, no
  new public API, no new authority.
- `MAINTENANCE_OWNER`: #402 Phase F1 (this PR); F5 owns final alignment.

Installed-set decisions (recorded per adopted F0 policy §2):

- Every header required to compile the current public closure is installed:
  `include/sluice/**` including `sluice/detail/` and `sluice/async/detail/`
  (INTERNAL_ONLY rows stay consumer-visible through public headers today; F1
  snapshots reality, it does not repair boundaries — that is F3/F5).
- `include/sluice/experimental/**` (2 headers, L-12) is **not installed**.
  This is the D-14 "REMOVE or ISOLATE (decide at install-rule time)" decision
  falling due at F1's install-rule time: ISOLATE chosen — headers remain in the
  source tree public include path and their orphaned TUs are untouched
  (FA-2 unchanged); REMOVE is production deletion and stays forbidden here.
- The OPTIONAL_CANDIDATE host closure is installed complete per H-30/H-31
  (`cancel.hpp`, `fiber.hpp`, `fiber_ctx.hpp` included) — F1 does not perform
  the F3 boundary fix.

Usage requirements recorded (validated against clean-room consumers, §4):

```text
sluice_core  : C++20; -I<prefix>/include; no external link requirement
sluice_async : sluice_core + sluice_async archives; same include;
               defines: SLUICE_HAS_LIBURING **iff** the archive was built with --liburing (ODR-critical)
               links:   -luring **iff** built with --liburing; thread link requirement measured in §4
```

## 2. F1-C — Clean-room workloads (results)

Executed by `scripts/verify_f1_package.py` (consumer sources under
`tests/package/`, README with the clean-room rules). Six full runs recorded
(3 × no-liburing profile, 3 × P2-liburing profile), **all gates PASS in all
six runs**:

```text
P0_W01_CORE_ONLY_CLEAN_ROOM        = PASS   (w01_direct; -lsluice_core only)
P1_w02_pipeline                    = PASS   (W-02 full workload shape)
P1_w03_external_loop_threadpool    = PASS   (application-owned epoll loop)
P1_w04_stackful_candidate          = PASS   (OPTIONAL_CANDIDATE evidence only)
P2_W03_URING_RUNTIME               = PASS   (real constructed io_uring backend, this kernel)
P1_W03_URING_EXPLICIT_UNAVAILABLE  = PASS   (no-liburing profile: rc=2, explicit outcome)
contract_* (8 families)            = PASS   (see f1-requirement-evidence-matrix.md)
F1_E_ODR_TWO_TU                    = PASS   (two TUs, one macro view)
NEGATIVE_ASYNC_SYMBOLS_NOT_IN_CORE = PASS   (core-only link of an async user fails)
NEGATIVE_EXPERIMENTAL_HEADERS_NOT_INSTALLED = PASS
NEGATIVE_LINK_WITHOUT_URING_FAILS  = PASS   (P2: undeclared -luring must fail)
CLEAN_ROOM_NO_SOURCE_OR_BUILD_TREE_DEPENDENCY = PASS (per-command audit + include scan)
```

W-03 uring on this machine = RUNTIME_AVAILABLE (WSL2 kernel 6.18,
io_uring_disabled=0, user-prefix liburing 2.9 recorded in the manifest); the
no-liburing profile records the honest UNAVAILABLE outcome.

## 3. Package manifest

Frozen machine-readable manifests (generated by the verifier; the
before-contraction baseline F2–F5 diff against):

- `docs/review/f1-package-manifest-noliburing.json` — P0/P1/P3 profile
  (79 headers, 2 archives, no public defines, no external link requirement).
- `docs/review/f1-package-manifest-liburing.json` — P2 profile (adds the
  `SLUICE_HAS_LIBURING` public define, `-luring` public link, and the
  ODR-critical same-macro-view requirement for every consumer TU).

Both manifests record `SOURCE_SHA = a34d96c6…` (the F0-adopted baseline; the
F1 diff itself is additive build/docs/test material only, no production
source change).

## 4. F1-E — Package/header/link/ODR/config audit

- **Header audit**: every installed header is reachable as `#include <…>`
  from the prefix (all consumers compile with `-I<prefix>/include` only);
  transitive completeness is exercised by the consumer set, which transitively
  pulls `detail/` substrates (the current closure, not the F3/F5 target).
- **Link audit**: `w01` links `-lsluice_core` only (no async, no uring, no
  pthread needed on glibc 2.4x — measured; recorded per toolchain in the
  manifest); async consumers link `-lsluice_async -lsluice_core` (P2 adds
  `-luring`); the negative probe proves async symbols do not leak into the
  core archive, and the P2 negative proves an undeclared `-luring` fails.
- **ODR audit**: `odr_two_tu` links two TUs instantiating `Request<T>`/
  `Result<T>` under one shared macro view, in both profiles; the
  `SLUICE_HAS_LIBURING` ODR rule (DAG X-12) is enforced by the verifier
  supplying the define from the manifest rather than per-consumer flags. Two
  further same-flags ODR obligations are recorded in the manifests: the fiber
  sanitizer-variant layout embedded by value in installed public headers
  (consumer TUs must match the archive's sanitizer configuration), and the
  prohibition on consumers defining `SLUICE_ASYNC_INTERNAL_TESTING` /
  `SLUICE_*_MUTANT_*` (installed headers carry guarded seam regions; the
  seam+liburing combination includes an uninstalled src/-only header).
- **Config audit**: `contract_backend_availability` + the `w03` uring variant
  prove named-backend selection is explicit in both profiles; the no-liburing
  installed shell is **abstract** from any external TU (the two
  `SLUICE_HAS_LIBURING`-guarded overrides leave pure virtuals unimplemented),
  so unavailability is compile-time — a live-verified refinement of the F0
  census's "construction throws" fact, which describes archive-internal code
  no external TU can reach without the profile (`uring_backend.hpp` guarded
  override block; recorded here as the package-closure truth).
- **Experimental isolation (D-14 decision)**: `include/sluice/experimental/`
  is not installed; headers remain on the source include path; orphaned TUs
  untouched (FA-2 unchanged). This is the ISOLATE arm of the F0 policy's
  "REMOVE or ISOLATE (decide at install-rule time)".

Gate: `F1_E_PACKAGE_DECLARATIONS_MATCH_REALITY` = PASS — declared usage
requirements (manifest) and the flags the clean-room consumers actually
succeeded with are identical, and the negative probes prove undeclared
requirements fail.

## 5. F1-F — Adversarial review record

Three independent reviewers attacked the committed F1 state
(dd5aaaca/95ef2728/284debca): A — clean-room contamination, B — requirement
coverage holes, C — profile/package/config mismatch. Verdicts: A PASS,
B PASS, C FAIL; 10 P1 and 14 P2 findings total, all adjudicated below. No P0
(no retirement-invisible root observation, no broken shipped package).

| Reviewer | Finding | Resolution |
|---|---|---|
| A-1 (P1) | gate env-blind: `CPATH`/`LIBRARY_PATH`-style env could silently supply repo headers/libs to consumer builds | `consumer_env()` whitelist for consumer compiles/links/runs; any include/lib path env var resolving under the repository is a hard failure |
| A-2 (P1) | include scan bypassable (absolute angle include, `..` traversal, macro-quoted, `# include`) | scan hardened: operand must be bare `<...>`, no quote, no leading `/`, no `..` segment, whitespace-normalized |
| A-3 (P1) | P2 link resolved `-luring` via env `LIBRARY_PATH`, invisible to the audited command stream | explicit `-L<uring-prefix>/lib` on every P2 link; runtime still via recorded `LD_LIBRARY_PATH` |
| A-4 (P2) | prefix exemption not anchored; TMPDIR could nest scratch dirs in the repo | `commonpath`-style anchoring for the prefix exemption; scratch dirs checked not to resolve inside the repository; TMPDIR pinned to `/tmp` when unset |
| A-5 (P2) | audit flag list missing `-iquote`/`-imacros` | added |
| A-6/C-7 (P2) | stray untracked default-path manifest | deleted; `--manifest-out` is now required so no non-frozen artifact is produced implicitly |
| A-7 (P2) | README overstated detector scope | README documents the env whitelist and its guarantee |
| B-1 (P1) | W-04 H-claims overclaimed: H4 had no external arm; H1's failing task was rejected pre-acceptance with `run()` error discarded; H2's task finished before the stop landed | w04 rewritten: H1 arm now fails via a task exception while the sibling's silent-pipe read is outstanding (run() error asserted, sibling settlement asserted); the H2/H4 arm parks a task on a silent-pipe read, lands stop mid-suspension, lets the await deadline expire and asserts the real EOF terminal arrives without cancellation (FIFO is outside the regular-file profile — the in-repo shutdown-oracle suspension pattern; recorded in the matrix) |
| B-2 (P1) | S8 "refuse-on-live-interest observed" had no external arm | `contract_shutdown` gained the refusal arm: shutdown with a live notification interest is refused (context stays open), completes after the ordered detach |
| B-3 (P1) | V01/V02 rows claimed full PASS without the root's zero-buffer schedule element | `contract_admission` gained zero-buffer invalid-resource and illegal-access arms (precedence before the no-op short circuit); rows upgraded to full PASS |
| B-4 (P2) | hub→compat-carrier include edge (F0 policy §1.3) has no executable detector | recorded: the load-bearing substance (hub's `Completion&` spellings) is compile-baselined by `contract_observer`; the include edge itself rides F2 (the removal of the edge changes no root-visible observation) |
| B-5 (P2) | V18 external arm contained a tautological assertion | matrix wording corrected to the honest form (settlement via fail-fast absence + post-unwind shutdown health) |
| B-6 (P2) | V21 fork-based probing feasible | recorded as an option; kept NOT_APPLICABLE for F1 (the internal fail-fast oracles own it) |
| B-7 (P2) | V04 statistical arm possible | recorded; per-trial disjunction without admission synchronization cannot distinguish the winner, seam evidence stays primary |
| B-8 (P2) | compat-family surfaces are manifest-diff-only baselined | matrix §4 family table now states this explicitly (no root-visible observation by design) |
| B-9 (P2) | RequestHandle runtime behavior unprobed | already recorded (UNDECIDED/F2-owned); unchanged |
| B-10 (P2) | gate-name mismatch in baseline §2 | fixed |
| C-1 (P1) | baseline claimed the thread requirement is "recorded in the manifest" but manifests were silent | manifests now carry the toolchain-qualified thread statement (no separate `-pthread` measured on glibc ≥ 2.34; other toolchains may need it) |
| C-2 (P1) | sanitizer-configuration ODR constraint missing from the frozen records | manifests now carry the same-sanitizer-config requirement (fiber_ctx layout embedded by value in installed headers) |
| C-3 (P1) | seam macros undeclared although 17 installed headers carry guarded regions | manifests now carry the NEVER-DEFINE prohibition for `SLUICE_ASYNC_INTERNAL_TESTING` / `SLUICE_*_MUTANT_*`; baseline §4 states the seam regions |
| C-4 (P2) | PROFILE tier labels unmapped to F0 classes | manifests carry `PROFILE_NOTE` and the README maps the tiers (P3 is the no-liburing profile tier label; OPTIONAL_CANDIDATE closure rides inside P1) |
| C-5 (P2) | manifest SOURCE_SHA is the branch point, not the install-rule commit | manifests now record both `SOURCE_SHA` (content baseline a34d96c6…, production diff empty) and `GENERATING_COMMIT` |
| C-6 (P2) | PROD-02 environment fields partial | manifests record ARCH/KERNEL/FILESYSTEM/LIBURING_VERSION; baseline §0.2 records the same plus filesystem |
| C-7 | (same as A-6) | resolved above |

Post-fix state: the hardened harness re-ran both profiles end-to-end with all
gates PASS (the strengthened w04/contract_admission/contract_shutdown arms
included), and the manifests were re-frozen with the new declaration fields.

## 6. Gate statement

- `F1_A_REALITY_FROZEN` = PASS (§0)
- `F1_B_PACKAGE_PREFIX_READY` = PASS (§1, §2)
- `F1_C_EXTERNAL_WORKLOADS_RECORDED` = PASS (§2; 6/6 stable runs)
- `F1_D_REQUIREMENT_MATRIX_COMPLETE` = PASS-candidate (`f1-requirement-evidence-matrix.md`)
- `F1_E_PACKAGE_DECLARATIONS_MATCH_REALITY` = PASS (§4)
- `F1_F_ADVERSARIAL_REVIEW_COMPLETE` = PASS (§5: three reviewers, all findings
  adjudicated; P1s fixed and re-verified, P2s resolved or recorded)
- `CLEAN_ROOM_NO_SOURCE_OR_BUILD_TREE_DEPENDENCY` = PASS (§2)
- `PACKAGE_USAGE_REQUIREMENTS_AND_DECLARATIONS_MATCH_BASELINE` = PASS (§4)
- `REQUIREMENT_EVIDENCE_COVERAGE_COMPLETE_FOR_BASELINE` = PASS-candidate
  (matrix §5 records the honest KNOWN_GAP rows with their B/C/D layers)
- `F1_CANONICAL_BASELINE_FROZEN` = PASS (additive baseline frozen at
  GENERATING_COMMIT; production source diff 0; F2–F5 diff targets are the two
  frozen manifests and this record)
