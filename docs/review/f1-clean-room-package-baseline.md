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

The manifests record the measured environment per run; the values below are
the ones those runs measured on this machine. Filesystem type is recorded per
location because the scratch prefixes and the source tree do not share one:

```text
TOOLCHAIN   = g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0; clang 21.1.8 present; xmake v2.9.7+20250101
ARCH        = x86_64
KERNEL      = Linux 6.18.33.2-microsoft-standard-WSL2 x86_64; io_uring_disabled = 0 (usable)
FILESYSTEM  = per-point, as measured by `stat -f -c %T`:
              source tree (/home/hoo/Source/sluice) = ext2/ext3;
              scratch install prefix + consumer build dir (under /tmp) = tmpfs
LIBURING    = 2.9 built from source (github.com/axboe/liburing, tag liburing-2.9) into
              /tmp/f1-deps/uring-prefix (liburing.a/.so.2.9) — system liburing absent
              (no pkg-config entry, no /usr/include/liburing.h); sudo unavailable
```

P2 consequences: the liburing-enabled build and its clean-room probes run
against the recorded user-prefix liburing; the manifest records that
environment. liburing-independent profiles (P0/P1/P3) are the default-configuration
evidence and do not touch it.

### 0.3 Provenance model (executable, not declared)

The manifests record three SHA fields, all machine-derived by the verifier at
run start:

```text
PRODUCTION_BASELINE_SHA = git merge-base HEAD origin/master   (a34d96c6… for this branch)
PRODUCTION_DIFF_EMPTY   = no src/ or include/ file differs between the baseline and HEAD
ORIGIN_MASTER_TIP       = git rev-parse origin/master         (context)
VERIFICATION_HEAD       = git rev-parse HEAD                  (the commit built, installed and probed)
VERIFICATION_HEAD_DIRTY = git status --porcelain non-empty at run start
```

A freeze run requires `PRODUCTION_DIFF_EMPTY` and a clean worktree at start
(gate `F1_B_PROVENANCE_VERIFIED`), so `VERIFICATION_HEAD` identifies the exact
content that was built and probed. Because the freeze commit itself can only
add the generated artifacts, reproduction is mechanical rather than
byte-equality of SHAs: at the freeze head,

```sh
python3 scripts/verify_f1_package.py --check-frozen \
    docs/review/f1-package-manifest-<profile>.json \
    --manifest-out /tmp/check.json --archive-baseline-out /tmp/check-baseline.json
```

re-runs the full verification and requires the fresh manifest to equal the
frozen one outside the volatile fields (`GENERATED_UTC`, `VERIFICATION_HEAD`,
`VERIFICATION_HEAD_DIRTY`, `ORIGIN_MASTER_TIP`, and the `file` path inside
`ARCHIVE_BASELINE`), and the archive-baseline sha256 bound in the frozen
manifest to match the fresh baseline byte-for-byte. The archive/object/symbol
baseline content itself is head-independent, so a matching sha256 proves the
shipped artifact content reproduced.

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

Installed-set decision — the full non-experimental public tree (recorded per
adopted F0 policy §2):

- **What is installed**: every header under `include/sluice/**` except
  `include/sluice/experimental/`, including `sluice/detail/` and
  `sluice/async/detail/` (INTERNAL_ONLY rows stay consumer-visible through
  public headers today). The OPTIONAL_CANDIDATE host closure is installed
  complete per H-30/H-31 (`cancel.hpp`, `fiber.hpp`, `fiber_ctx.hpp` included)
  — F1 does not perform the F3 boundary fix.
- **Why more than `CANONICAL + OPTIONAL_CANDIDATE closure`**: F0 established
  there was **no package at all** before F1 (§0.1), so this is a *new physical
  install-surface decision*, not an observation of an existing one. Installing
  a narrower set now would already be a contraction decision made without a
  before-baseline to diff against — exactly the decision F1 exists to enable
  later. F1 therefore freezes the **current** physical exposure: the installed
  set is what the current public closure reaches, including the
  COMPATIBILITY/INTERNAL/TEST_ONLY headers that public headers transitively
  pull in. The frozen manifests are **diff targets, not a compatibility
  promise**: F2/F3 header and target removals are expected to shrink the
  installed set, and each such manifest deletion must be justified by the
  corresponding family disposition, not by backward-compatibility obligations
  to this snapshot.
- **What is not installed**: `include/sluice/experimental/**` (2 headers, L-12)
  is not installed. This is the D-14 "REMOVE or ISOLATE (decide at install-rule
  time)" decision falling due at F1's install-rule time: ISOLATE chosen —
  headers remain in the source tree public include path and their orphaned TUs
  are untouched (FA-2 unchanged); REMOVE is production deletion and stays
  forbidden here.

Usage requirements recorded (validated against clean-room consumers, §4):

```text
sluice_core  : C++20; -I<prefix>/include; no external link requirement
sluice_async : sluice_core + sluice_async archives; same include;
               defines: SLUICE_HAS_LIBURING **iff** the archive was built with --liburing (ODR-critical)
               links:   -luring **iff** built with --liburing; thread link requirement measured in §4
```

## 2. F1-C — Clean-room workloads (results)

Executed by `scripts/verify_f1_package.py` (consumer sources under
`tests/package/`, README with the clean-room rules). Both profiles pass all
gates from the final verifier state (per-run records in the frozen manifests;
`--check-frozen` reproduces them):

```text
F1_B_PROVENANCE_VERIFIED           = PASS  (clean tree, production diff empty vs a34d96c6…)
F1_B_PACKAGE_PREFIX_READY          = PASS  (79 headers, 2 archives)
F1_E_STANDALONE_HEADER_COMPILES    = PASS  (all 79 installed headers, each profile's macro view;
                                            blocking_io_pool_impl.hpp is an anchored _impl fragment, §4)
F1_E_ARCHIVE_BASELINE_RECORDED     = PASS  (2 archives, 50 objects, 1501 defined symbols per profile)
F1_E_ARCHIVE_MATCHES_PROFILE       = PASS  (liburing archive references io_uring + UringConfig ctor;
                                            no-liburing archive does neither — stale-cache guard)
F1_E_ODR_TWO_TU                    = PASS  (same-specialization + macro-sensitive layout discriminator; §4)
P0_W01_CORE_ONLY_CLEAN_ROOM        = PASS  (w01_direct; -lsluice_core only)
P1_w02_pipeline                    = PASS  (W-02 full workload shape)
P1_w03_external_loop_threadpool    = PASS  (application-owned epoll loop)
P2_W03_URING_RUNTIME               = PASS  (real constructed io_uring backend, this kernel)
P1_W03_URING_EXPLICIT_UNAVAILABLE  = PASS  (no-liburing profile: rc=2, explicit outcome)
P2_W04_STRUCTURAL_STOP             = PASS  (liburing profile: structurally ordered H1/H2/H4 arms, below)
P1_W04_STRUCTURAL_ARMS_UNAVAILABLE = PASS  (no-liburing profile: rc=2, measured physical reason, below)
contract_* (8 families)            = PASS  (see f1-requirement-evidence-matrix.md)
NEGATIVE_ASYNC_SYMBOLS_NOT_IN_CORE = PASS  (core-only link of an async user fails)
NEGATIVE_EXPERIMENTAL_HEADERS_NOT_INSTALLED = PASS
NEGATIVE_LINK_WITHOUT_URING_FAILS  = PASS  (P2: undeclared -luring must fail)
NEGATIVE_ODR_DIVERGENT_VIEWS_DETECTED = PASS (P2: opposite macro views yield 48 vs 304 bytes, §4)
CLEAN_ROOM_NO_SOURCE_OR_BUILD_TREE_DEPENDENCY = PASS (per-command audit + include scan)
```

W-03 uring on this machine = RUNTIME_AVAILABLE (WSL2 kernel 6.18,
io_uring_disabled=0, user-prefix liburing 2.9 recorded in the manifest); the
no-liburing profile records the honest UNAVAILABLE outcome.

### 2.1 W-04 structural stop/failure arms

The stop/failure-during-outstanding-I/O arms are built on a measured physical
fact: the threadpool backend services reads with `pread`
(`src/async/threadpool_backend.cpp`), so a non-seekable fd (pipe) fails
immediately with `backend_error` (ESPIPE) and **no externally reachable
suspension exists without liburing**. The arms therefore run over a real
`UringAsyncBackend` context under `SLUICE_HAS_LIBURING`, and the no-liburing
binary reports the explicit unavailable outcome (exit 2) after passing its
profile-independent arms — the w03 pattern.

Every ordering edge in the arms is a real happens-before, not a sleep:

1. the reader task parks on an empty-pipe read (writer open, unbounded);
2. `run()`'s single driver starts the second task only after that suspension,
   and the second task's byte on a sync pipe publishes it;
3. the stop request (or failing task's throw) is issued only after that byte;
4. the writer closes the pipe — the request's real terminal — only after the
   stop call returned (or the throw was announced);
5. the parked read settles only at that EOF, so the task observes the real
   terminal (0-byte read, never `canceled`), the stop flag visible through its
   token, and retires normally.

The deadline (`_for`) arm adds a 50 ms wait parameter: the deadline bounds
only the driver's park window, and the assertions hold whether park-window
expiry or the writer-gated EOF comes first, so no ordering claim rests on
timing. The fine-grained expiry-vs-terminal interleaving stays with the
internal deterministic oracles (matrix §5).

## 3. Package manifest and archive/object/symbol baseline

Frozen machine-readable records (generated by the verifier; the
before-contraction baseline F2–F5 diff against):

- `docs/review/f1-package-manifest-noliburing.json` — P0/P1/P3 profile
  (79 headers, 2 archives, no public defines, no external link requirement).
- `docs/review/f1-package-manifest-liburing.json` — P2 profile (adds the
  `SLUICE_HAS_LIBURING` public define, `-luring` public link, and the
  ODR-critical same-macro-view requirement for every consumer TU).
- `docs/review/f1-archive-baseline-noliburing.json` /
  `docs/review/f1-archive-baseline-liburing.json` — archive/object/symbol
  baseline per profile: per-archive sha256, per-object member sha256 and
  defined symbols, and the union of undefined (external) symbols. Head-independent
  content, bound into the manifest by sha256 (`ARCHIVE_BASELINE` field).

Both manifests record `PRODUCTION_BASELINE_SHA = a34d96c6…` with
`PRODUCTION_DIFF_EMPTY = true` (the F1 diff itself is additive
build/docs/test material only, no production source change), plus the
`VERIFICATION_HEAD` the artifacts were generated from (§0.3). The two
profiles' archives genuinely differ: the liburing `sluice_async.a` defines
the uring transport (79 additional symbols, `io_uring*` externals) that the
no-liburing archive lacks — enforced per run by
`F1_E_ARCHIVE_MATCHES_PROFILE`.

## 4. F1-E — Package/header/link/ODR/config audit

- **Header audit**: every installed header is reachable as `#include <…>`
  from the prefix (all consumers compile with `-I<prefix>/include` only);
  transitive completeness is exercised by the consumer set, which transitively
  pulls `detail/` substrates (the current closure, not the F3/F5 target).
- **Standalone-compile audit** (#402 F1): every installed header must compile
  as the sole include of a TU under the profile's macro view — 79/79 in both
  profiles. `sluice/detail/blocking_io_pool_impl.hpp` is a `*_impl` fragment
  (valid only after its primary header's declarations) and is exempted as a
  recorded class, with the mechanical anchor that a standalone-compiling
  installed header (`blocking_io_pool.hpp`) includes it; an unanchored
  fragment fails the gate.
- **Link audit**: `w01` links `-lsluice_core` only (no async, no uring, no
  pthread needed on glibc 2.4x — measured; recorded per toolchain in the
  manifest); async consumers link `-lsluice_async -lsluice_core` (P2 adds
  `-luring`); the negative probe proves async symbols do not leak into the
  core archive, and the P2 negative proves an undeclared `-luring` fails.
- **Archive/object/symbol audit** (#402 F1): the verifier records per archive
  the sha256, every object member with its own sha256 and defined symbols,
  and the union of undefined externals (2 archives, 50 objects, 1501 defined
  symbols per profile on this toolchain). F2–F5 deletions diff these files.
- **ODR audit** (discriminating, not repeat-evidence): `odr_two_tu` links two
  TUs that observe the SAME template specializations
  (`Result<std::size_t>`, `Request<std::size_t>`, inline members ODR-used in
  both) and the layout of the macro-sensitive `UringAsyncBackend`
  (`sizeof`/`alignof`; every data member of the class sits inside the
  `SLUICE_HAS_LIBURING` guard). The binary fails unless both TUs report
  identical facts under the shared macro view. The probe is demonstrably
  macro-sensitive: `sizeof(UringAsyncBackend)` = 48 without the define and
  304 with it on this toolchain (recorded per profile in the manifests'
  `ODR_DISCRIMINATOR`), and the P2 negative probe
  (`negative_odr_view_*`) compiles two TUs with OPPOSITE macro views and
  deterministically observes the mismatch (exit 1, `odr-divergent-mismatch
  without=48 with=304`) — divergent views are detected and rejected, not
  silently merged. The verifier supplies the define from the profile, never
  hand-copied per consumer. Two further same-flags ODR obligations are
  recorded in the manifests: the fiber sanitizer-variant layout embedded by
  value in installed public headers (consumer TUs must match the archive's
  sanitizer configuration), and the prohibition on consumers defining
  `SLUICE_ASYNC_INTERNAL_TESTING` / `SLUICE_*_MUTANT_*` (installed headers
  carry guarded seam regions; the seam+liburing combination includes an
  uninstalled src/-only header).
- **Config audit**: `contract_backend_availability` + the `w03` uring variant
  prove named-backend selection is explicit in both profiles; the no-liburing
  installed shell is **abstract** from any external TU (the two
  `SLUICE_HAS_LIBURING`-guarded overrides leave pure virtuals unimplemented),
  so unavailability is compile-time — a live-verified refinement of the F0
  census's "construction throws" fact, which describes archive-internal code
  no external TU can reach without the profile (`uring_backend.hpp` guarded
  override block; recorded here as the package-closure truth).
  `F1_E_ARCHIVE_MATCHES_PROFILE` additionally proves per run that the built
  archive carries its profile's macro view (a stale xmake cache build of the
  opposite profile fails the run — found and fixed during this round; §5).
- **Experimental isolation (D-14 decision)**: `include/sluice/experimental/`
  is not installed; headers remain on the source include path; orphaned TUs
  untouched (FA-2 unchanged). This is the ISOLATE arm of the F0 policy's
  "REMOVE or ISOLATE (decide at install-rule time)".

Gate: `F1_E_PACKAGE_DECLARATIONS_MATCH_REALITY` = PASS — declared usage
requirements (manifest) and the flags the clean-room consumers actually
succeeded with are identical, and the negative probes prove undeclared
requirements fail.

## 5. F1-F — Adversarial review record

### 5.1 Round F1-F: three independent reviewers

Three independent reviewers attacked the committed F1 state
(dd5aaaca/95ef2728/284debca): A — clean-room contamination, B — coverage
holes, C — profile/package/config mismatch. Verdicts: A PASS, B PASS, C FAIL;
10 P1 and 14 P2 findings total, all adjudicated below. No P0 (no
retirement-invisible root observation, no broken shipped package).

| Reviewer | Finding | Resolution |
|---|---|---|
| A-1 (P1) | gate env-blind: `CPATH`/`LIBRARY_PATH`-style env could silently supply repo headers/libs to consumer builds | `consumer_env()` whitelist for consumer compiles/links/runs; any include/lib path env var resolving under the repository is a hard failure |
| A-2 (P1) | include scan bypassable (absolute angle include, `..` traversal, macro-quoted, `# include`) | scan hardened: operand must be bare `<...>`, no quote, no leading `/`, no `..` segment, whitespace-normalized |
| A-3 (P1) | P2 link resolved `-luring` via env `LIBRARY_PATH`, invisible to the audited command stream | explicit `-L<uring-prefix>/lib` on every P2 link; runtime still via recorded `LD_LIBRARY_PATH` |
| A-4 (P2) | prefix exemption not anchored; TMPDIR could nest scratch dirs in the repo | `commonpath`-style anchoring for the prefix exemption; scratch dirs checked not to resolve inside the repository; TMPDIR pinned to `/tmp` when unset |
| A-5 (P2) | audit flag list missing `-iquote`/`-imacros` | added |
| A-6/C-7 (P2) | stray untracked default-path manifest | deleted; `--manifest-out` is now required so no non-frozen artifact is produced implicitly |
| A-7 (P2) | README overstated detector scope | README documents the env whitelist and its guarantee |
| B-1 (P1) | W-04 H-claims overclaimed | see Round 2 R-4: the sleep-based arms were replaced by structurally ordered uring-backed arms; the prior "silent-pipe suspension" itself was discovered to never suspend under the threadpool backend (pread/ESPIPE) |
| B-2 (P1) | S8 "refuse-on-live-interest observed" had no external arm | `contract_shutdown` gained the refusal arm: shutdown with a live notification interest is refused (context stays open), completes after the ordered detach |
| B-3 (P1) | V01/V02 rows claimed full PASS without the root's zero-buffer schedule element | `contract_admission` gained zero-buffer invalid-resource and illegal-access arms (precedence before the no-op short circuit); rows upgraded to full PASS |
| B-4 (P2) | hub→compat-carrier include edge (F0 policy §1.3) has no executable detector | recorded: the load-bearing substance (hub's `Completion&` spellings) is compile-baselined by `contract_observer`; the include edge itself rides F2 (the removal of the edge changes no root-visible observation) |
| B-5 (P2) | V18 external arm contained a tautological assertion | matrix wording corrected to the honest form (settlement via fail-fast absence + post-unwind shutdown health) |
| B-6 (P2) | V21 fork-based probing feasible | recorded as an option; kept NOT_APPLICABLE for F1 (the internal fail-fast oracles own it) |
| B-7 (P2) | V04 statistical arm possible | recorded; per-trial disjunction without admission synchronization cannot distinguish the winner, seam evidence stays primary |
| B-8 (P2) | compat-family surfaces are manifest-diff-only baselined | matrix §4 family table now states this explicitly (no root-visible observation by design) |
| B-9 (P2) | RequestHandle runtime behavior unprobed | already recorded (UNDECIDED/F2-owned); unchanged |
| B-10 (P2) | gate-name mismatch in baseline §2 | fixed |
| C-1 (P1) | baseline claimed the thread requirement is "recorded in the manifest" but manifests were silent | manifests carry the toolchain-qualified thread statement (no separate `-pthread` measured on glibc ≥ 2.34; other toolchains may need it) |
| C-2 (P1) | sanitizer-configuration ODR constraint missing from the frozen records | manifests carry the same-sanitizer-config requirement (fiber_ctx layout embedded by value in installed headers) |
| C-3 (P1) | seam macros undeclared although 17 installed headers carry guarded regions | manifests carry the NEVER-DEFINE prohibition for `SLUICE_ASYNC_INTERNAL_TESTING` / `SLUICE_*_MUTANT_*`; baseline §4 states the seam regions |
| C-4 (P2) | PROFILE tier labels unmapped to F0 classes | manifests carry `PROFILE_NOTE` and the README maps the tiers (P3 is the no-liburing profile tier label; OPTIONAL_CANDIDATE closure rides inside P1) |
| C-5 (P2) | manifest SOURCE_SHA is the branch point, not the install-rule commit | superseded by Round 2 R-1 (executable provenance model, §0.3) |
| C-6 (P2) | PROD-02 environment fields partial | manifests record ARCH/KERNEL/per-point FILESYSTEM/LIBURING_VERSION; baseline §0.2 records the same |

### 5.2 Round 2: F1 human review (PR #456 review id 5439314644, 2026-10-07)

Verdict REQUEST_CHANGES / DO NOT MERGE; six P1 findings, one governance P2,
one issue-hygiene item. All adjudicated; the fixes re-ran both profiles from
a clean tree at the final verifier state.

| Finding | Resolution |
|---|---|
| R-1 (P1) manifest provenance self-contradictory: both manifests carried `SOURCE_SHA = GENERATING_COMMIT = 284debca…` while the doc claimed the F0 baseline, and the verifier derived both from `git rev-parse HEAD` | executable provenance model (§0.3): `PRODUCTION_BASELINE_SHA` = merge-base with origin/master, `PRODUCTION_DIFF_EMPTY` gate, `VERIFICATION_HEAD` recorded from a clean worktree at run start; the old fields are gone; manifests are regenerated from the final verifier state and `--check-frozen` proves reproduction at the freeze head |
| R-2 (P1) filesystem evidence contradictory (doc said ext4, manifests measured tmpfs) | per-point recording (§0.2): source tree ext2/ext3, scratch prefix + consumer build dir tmpfs; manifests carry `FILESYSTEM_SOURCE_TREE` / `FILESYSTEM_SCRATCH_PREFIX` / `FILESYSTEM_CONSUMER_BUILD` |
| R-3 (P1) #402 F1 mandatory package audit missing (target/object/archive manifest, canonical entry-header standalone compile, archive/object/symbol baseline) and `SYMBOL_OBJECT_BASELINE` was deferred to F5 | implemented in F1: standalone compile of all 79 installed headers per profile (with the anchored `*_impl` fragment class, §4) and the archive/object/symbol baseline files (§3) bound into the manifests; the F5 deferral is withdrawn — F5 owns final alignment, not first creation |
| R-4 (P1) W04 H2/H4 was a timing-shaped oracle (20/100/250 ms sleeps; `stop_requested()` checked only after joins) | structural rewrite (§2.1): suspension, stop, throw and EOF are ordered by single-driver scheduling plus pipe-byte handoffs; during the rewrite the prior "silent-pipe suspension" was measured to never suspend under the threadpool backend (`pread`/ESPIPE → immediate `backend_error`), so the arms moved to a real uring context under the liburing profile and the no-liburing profile records the explicit measured UNAVAILABLE outcome (exit 2) — no assertion depends on any interleaving timing |
| R-5 (P1) the ODR probe instantiated different specializations per TU and never observed the macro-sensitive surface | discriminator rewrite (§4): both TUs observe the same `Result<std::size_t>`/`Request<std::size_t>` specializations AND `sizeof`/`alignof` of `UringAsyncBackend` (layout entirely inside the `SLUICE_HAS_LIBURING` guard); sensitivity evidenced per profile (48 vs 304 bytes) and by the deterministic divergent-view negative probe that must observe and reject the mismatch |
| R-6 (P1) README reproduction commands omit the now-required `--manifest-out` | README commands pass the two canonical frozen manifest paths and the archive-baseline paths explicitly; `--check-frozen` documented |
| R-7 (P2/governance) the glob installs nearly the whole non-experimental tree (COMPATIBILITY/INTERNAL/TEST_ONLY included) while F0 only mandated the H-30/H-31 closure | §1 now records this explicitly as a new physical install-surface decision: there was no package before F1, so freezing current exposure is the conservative before-baseline; manifests are diff targets, not a compatibility promise; F2/F3 manifest deletions are justified by family dispositions, not by this snapshot |
| R-8 (governance) #402's stale "Current execution authorization" section still described F0 active / F1 blocked | #402 §6 updated to the current state (F0 CLOSED/ADOPTED, F1 ACTIVE-AUTHORIZED) matching the status table |

Found during the R-1..R-6 fix runs (recorded as a new P1 against the prior
harness): the verifier never passed `--liburing=n` explicitly, so a cached
liburing-enabled xmake config silently reused the same archive for both
profiles — the no-liburing "profile" evidence of the prior round was in fact
the uring-built package (archive bytes and symbol sets were byte-identical).
Fix: explicit both-ways switch, env scrubbing of the uring prefix for the
no-liburing build, and the `F1_E_ARCHIVE_MATCHES_PROFILE` gate proving per
run that the built archive carries its profile's macro view (io_uring
externals + `UringConfig` ctor present iff liburing).

## 6. Gate statement

- `F1_A_REALITY_FROZEN` = PASS (§0)
- `F1_B_PROVENANCE_VERIFIED` = PASS (§0.3; clean tree, production diff empty)
- `F1_B_PACKAGE_PREFIX_READY` = PASS (§1, §2)
- `F1_C_EXTERNAL_WORKLOADS_RECORDED` = PASS (§2; both profiles from the final
  verifier state)
- `F1_D_REQUIREMENT_MATRIX_COMPLETE` = PASS-candidate (`f1-requirement-evidence-matrix.md`)
- `F1_E_PACKAGE_DECLARATIONS_MATCH_REALITY` = PASS (§4)
- `F1_F_ADVERSARIAL_REVIEW_COMPLETE` = PASS (§5: rounds 1 and 2, all findings
  adjudicated and re-verified)
- `CLEAN_ROOM_NO_SOURCE_OR_BUILD_TREE_DEPENDENCY` = PASS (§2)
- `PACKAGE_USAGE_REQUIREMENTS_AND_DECLARATIONS_MATCH_BASELINE` = PASS (§4)
- `REQUIREMENT_EVIDENCE_COVERAGE_COMPLETE_FOR_BASELINE` = PASS-candidate
  (matrix §5 records the honest KNOWN_GAP rows with their B/C/D layers)
- `F1_CANONICAL_BASELINE_FROZEN` = PASS (additive baseline frozen from the
  final verifier state at a clean `VERIFICATION_HEAD`; production source diff
  0 vs `PRODUCTION_BASELINE_SHA`; manifests, archive/object/symbol baselines
  and this record are the F2–F5 diff targets; reproduction at the freeze head
  is mechanical via `--check-frozen`)
