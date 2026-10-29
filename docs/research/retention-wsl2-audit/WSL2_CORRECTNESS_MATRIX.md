# WSL2 Correctness Matrix (retention-wsl2-audit)

Configuration × result matrix for the WSL2 branch-retention audit round.
Every row was executed on this host during the round against
`84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` (= `origin/master` = P2C1 #612;
C1 #602 at `43cb8029ca60b572c91a9aa9dccb17d4300e2f88`), each cell in its own
detached `/tmp/sluice-val-*` verification worktree. The main workspace was
never built in; no product code was modified; no worktree was removed by the
audit.

Command shapes come from the scout matrix `WSL2_COMMAND_MATRIX.md` (E5-frozen
command sequences re-anchored to this host). Raw output for each cell is in
the linked receipt under `receipts/`; this document is the consolidated rollup.

Common environment for all cells: WSL2 kernel
`6.18.54.1-microsoft-standard-WSL2`, Ubuntu 26.04.1 LTS, gcc 15.2.0
(Ubuntu 15.2.0-16ubuntu1), xmake v3.1.2+HEAD.5d21e9d, 8 CPUs
(`receipts/01-host-kernel.txt`, `receipts/02-toolchain.txt`). Repo on ext4
(native root, not `/mnt/*`), `/tmp` on tmpfs (`receipts/03-filesystem.txt`).

## Status vocabulary

- **PASS** — cell executed and green with only the per-unit worktree-path
  substitution recorded (scout commands say `cd /tmp/sluice-val-release`;
  each unit was assigned its own worktree).
- **ADAPTED** — cell executed and green, and the receipt records at least one
  further executed deviation from the commanded sequence. Every recorded
  adaptation is coverage-complete and assertion-preserving; none relaxed a
  check.

Honesty note: three receipts self-label their outcome `PASS`
(`release.md` Result section, `c1-mutation.md`, `p2c1-mutation-a.md`) while
this rollup records them `ADAPTED` because those receipts document executed
command deviations (test-selector syntax, clean-build preamble). The
underlying evidence is identical either way: 0 failures, adaptations
documented, no assertion weakened. The rollup labels are the ones carried
into the audit's structured result.

## Consolidated result table

| # | Cell | Command essentials (each `cd <worktree>` first) | Result | Direct Primitive suites | Composition suites | Status | Receipt |
|---|------|--------------------------------------------------|--------|--------------------------|--------------------|--------|---------|
| 1 | release | `xmake f -c -m release --liburing=n -y && xmake -y && xmake test -y` | **58/58, 0 failed** (`100% tests passed, 0 test(s) failed out of 58, spent 4.159s`); build ok 1.667s | 9/9 (`xmake test -y '*/blocking_file_read_test' …`, spent 0.096s) | 4/4 (`xmake test -y '*/direct_composition_test' …`, spent 0.005s) | ADAPTED | [receipts/release.md](receipts/release.md) |
| 2 | debug | `xmake f -c -m debug --liburing=n -y && xmake -y && xmake test -y` | **58/58, 0 failed** (`… out of 58, spent 3.994s`); build ok 1.557s | 9/9 (`xmake test -y 'blocking_file_read_test/*' …`, spent 0.153s) | 4/4 (`xmake test -y 'direct_composition_test/*' …`, spent 0.044s); plus `-O0` supplemental: fault suite 15/15, short-io 9/9 | ADAPTED | [receipts/debug.md](receipts/debug.md), [receipts/18-debug-o0-suites.txt](receipts/18-debug-o0-suites.txt) |
| 3 | ASan | `xmake f -c -m asan --liburing=n -y && xmake -y && xmake test -y` | **58/58, 0 failed** (spent 4.103s); 0 ASan/LSan reports (grep zero hits) | included in the 58 (C1 killer suites green) | included in the 58 (composition/fault suites green) | PASS | [receipts/asan.md](receipts/asan.md) |
| 4 | UBSan | `xmake f -c -m ubsan --liburing=n -y && xmake -y && xmake test -v 2>&1 \| tee ubsan.log` | **58/58, 0 failed** (spent 4.007s); `grep -c 'runtime error'` = **0** | included in the 58 | included in the 58 | PASS | [receipts/ubsan.md](receipts/ubsan.md) |
| 5 | ASan+UBSan | `xmake f -c -m asanubsan --liburing=n -y && xmake -y && xmake test -v \| tee asanubsan.log` | **58/58, 0 failed** (spent 4.142s); 387/387 compile commands carry both `-fsanitize=address` and `-fsanitize=undefined`; `runtime error` = 0, AddressSanitizer reports = 0 | included in the 58 | included in the 58 | PASS | [receipts/asan-ubsan.md](receipts/asan-ubsan.md) |
| 6 | liburing **on** | `xmake f -c -m release --liburing=y -y && xmake -y && xmake test -y` | **79/79, 0 failed** (spent 3.993s); capability probe resolved real `/usr/include/liburing.h` + `liburing.so`; all 21 uring-only targets passed individually, incl. `uring_backend_smoke_test` (real submissions) and `uring_public_consumer_probe` | unchanged (58 base targets green) | unchanged; the 21 extra are uring backend/consumer targets | ADAPTED | [receipts/liburing.md](receipts/liburing.md) |
| 7 | liburing **off** | `xmake f -c -m release --liburing=n -y && xmake -y && xmake test -y` (same worktree, immediately after leg 6) | **58/58, 0 failed** (spent 4.019s); no capability probe at configure; `SLUICE_HAS_LIBURING` occurrences in compile_commands.json = 0; `-DSLUICE_LIBURING_PROFILE=off` × 37 (honest stub) | 9/9 equivalent within the 58 | 4/4 equivalent within the 58 | ADAPTED | [receipts/liburing.md](receipts/liburing.md) |
| 8 | SDK package verifier | `python3 scripts/verify_v2_package.py --mode release --manifest-out /tmp/sluice-val-sdk/v2-manifest.json --keep-prefix` | **exit 0, 24/24 gates PASS** (run 3; wall 29.9s); manifest: `VERIFICATION_HEAD=84e1b7d5…`, `VERIFICATION_HEAD_DIRTY=False`, 11 headers (exact contracted list), 422 defined symbols, ODR macro-independent, negative seam/mutant-define probes PASS | verifier P0/P1 clean-room consumers green (core-only link) | SEM-05-adjacent consumer families inside the gate set | ADAPTED | [receipts/sdk.md](receipts/sdk.md) |
| 9 | Independent consumer probe | self-written 16-case probe; `g++ -std=c++20 -I"$PFX"/include … -lsluice_core`; sole include `<sluice/blocking/file.hpp>`; prefix `/tmp/sluice-v2-prefix-0vjpa23t` from run 3 | **16/16 PASS, exit 0**; controls: `ldd` shows system libs only; link without `-lsluice_core` fails (74 undefined refs); compile without `-I` fails | round-trip / past-EOF / zero-length / UINT64_MAX-offset cases (primitives via compositions) | `eof_before_full` confirmed-23 + remaining-accounted, closed → `invalid_state`, write-only read → `invalid_argument`, move-then-close, `CompositionOutcome` accounting | ADAPTED | [receipts/sdk.md](receipts/sdk.md) (Part 2) |
| 10 | c1-mutation (supplementary) | release `--liburing=n`; three mutant targets built and run, then restored | 3/3 valid kills; every RESTORED check **58/58** | killers live in Direct Primitive suites (`blocking_file_{read,sequential,state}_test`) | — | ADAPTED | [receipts/c1-mutation.md](receipts/c1-mutation.md); detail in [WSL2_MUTATION_RECEIPTS.md](WSL2_MUTATION_RECEIPTS.md) |
| 11 | p2c1-mutation-a (supplementary) | release `--liburing=n`; four mutant cycles | 4/4 valid kills; RESTORED **58/58** after each | — | killers live in Composition suites (`direct_composition_fault_test`, `semantic_short_io_reference_test`) | ADAPTED | [receipts/p2c1-mutation-a.md](receipts/p2c1-mutation-a.md); detail in [WSL2_MUTATION_RECEIPTS.md](WSL2_MUTATION_RECEIPTS.md) |
| 12 | p2c1-mutation-b (supplementary) | release `--liburing=n`; three mutant cycles off one shared base run | 3/3 valid kills; RESTORED **58/58** after each | — | killers in `direct_composition_fault_test` (base run 15/15, exit 0) | PASS | [receipts/p2c1-mutation-b.md](receipts/p2c1-mutation-b.md); detail in [WSL2_MUTATION_RECEIPTS.md](WSL2_MUTATION_RECEIPTS.md) |

Rollup totals by cell: release 58/58 · debug 58/58 · asan 58/58 · ubsan 58/58 ·
asan-ubsan 58/58 · liburing 137/137 (79+58) · sdk 40/40 (24 gates + 16 consumer
cases). **No cell recorded a failure.** Mutation-cell `58/58` figures are the
RESTORED-GREEN full-matrix runs; mutant RED/RESTORED evidence per mutant is in
[WSL2_MUTATION_RECEIPTS.md](WSL2_MUTATION_RECEIPTS.md).

## Direct Primitive vs Composition attribution

The two groupings are audit constructs (handoff §2 primitive/composition
definitions plus the C1/SEM-05 killer homes); the repo registers no such
target groups (`WSL2_COMMAND_MATRIX.md` §7 gap 7). `xmake test` name
filtering is the mechanism; xmake v3.1.2 rejects bare target names
(`nothing to test`, verified twice), so the working quoted pattern forms
`*/name` (release cell) or `name/*` (debug cell) were used — both verified to
select exactly the commanded suites.

- **Direct Primitive suites** (public primitives `read_at`/`write_at`/`read`
  /`write` and the admission face; 9 targets): `blocking_file_read_test`,
  `blocking_file_write_test`, `blocking_file_state_test`,
  `blocking_file_sequential_test`, `direct_cursor_position_test`,
  `file_access_precedence_test`, `file_close_semantics_test`
  (`SLUICE_FILE_INTERNAL_TESTING` seam build, `xmake/tests.lua:60-74`),
  `semantic_range_test`, `semantic_validation_precedence_test`.
  The three C1 admission mutants are killed inside this group.
  Executed green in release (9/9, spent 0.096s) and debug (9/9, spent 0.153s);
  present and green inside every full-matrix cell.
- **Composition suites** (`read_exact_at`/`write_all_at`/`read_exact`
  /`write_all`, SEM-05 stop-reason oracle and Effect Certainty accounting;
  4 targets): `direct_composition_test`, `direct_composition_fault_test`
  (composition stop-reason oracle + write-side impossible-count script; home
  of the P2C1 killers), `semantic_short_io_reference_test` (SEM-05 rules-table
  oracle), `semantic_effect_outcome_test`.
  Executed green in release (4/4, spent 0.005s) and debug (4/4, spent 0.044s);
  present and green inside every full-matrix cell.

## Per-cell evidence notes (adaptations and independent-effectiveness proof)

1. **release** — deviations recorded by the receipt: (a) test-selector syntax
   `*/name` (bare names produce `nothing to test` on xmake v3.1.2+HEAD,
   verified twice); (b) `xmake clean -a && rm -rf build` preamble so cmd-01 is
   a from-clean build (leftover partial artifacts from an interrupted earlier
   session; `git status --porcelain` stayed empty). Binary identity frozen:
   `libsluice_core.a` sha256 `d41685d2…70d338`, `libsluice_async.a`
   `e456008e…73b4`, plus 16 test/app binaries and the 11 installed SDK
   headers (`receipts/release.md` cmd-04). The receipt closes `status = PASS`
   with no weakening; the rollup records ADAPTED per the vocabulary above.
2. **debug** — deviations: dedicated worktree `/tmp/sluice-val-debug`;
   direct/composition subsets run in debug mode (the matrix's subset examples
   said release; mode registration is identical per `xmake/tests.lua`);
   selector form `name/*` with zsh quoting. `-O0` supplemental proof that no
   semantic depends on optimization: `direct_composition_fault_test` 15/15,
   `semantic_short_io_reference_test` 9/9
   (`receipts/18-debug-o0-suites.txt`).
3. **ASan** — effectiveness independently evidenced, not assumed: actual
   compile line carries `-fsanitize=address`;
   `nm …direct_composition_test` shows `U __asan_init` + 21 asan runtime
   symbols; zero `AddressSanitizer`/`LeakSanitizer`/`deadly signal`/`SEGV`
   hits in the log.
4. **UBSan** — `grep -c 'runtime error' ubsan.log` → 0 (r3 §1 criterion).
   Non-failure stderr inside the log (deliberate contract-violation self-tests
   in watchdog/shutdown lifecycle suites, e.g. `terminate called without an
   active exception` at ubsan.log:656-778 and one withheld-completion TIMEOUT
   dump inside `shutdown_lifecycle_test`) is disclosed in the receipt; the
   corresponding xmake targets are all `passed`.
5. **ASan+UBSan** — both sanitizer policies verified on every compile command
   (387/387) and both runtimes linked into test binaries (`nm` grep = 11).
   Zero reports under both sanitizers.
6. **liburing on/off** — single worktree, two sequential legs. The `y` leg is
   **new evidence on this host** (first local y-leg run in this audit), not an
   E5 reproduction: `io_uring_disabled=0` on this kernel, capability probe
   resolved the real header/library, and the 21 `has_config("liburing")`-only
   targets registered and passed 21/21. The `n` leg proves the honest stub:
   zero `SLUICE_HAS_LIBURING`, profile macro pinned `off`. No test skipped,
   no assertion weakened in either leg.
7. **SDK verifier** — runs 1–2 failed exactly one gate,
   `V2_B_PROVENANCE_RECORDED`, because the executor's own log file sat inside
   the worktree (self-inflicted harness dirt; logs preserved at
   `/tmp/sluice-val-sdk-consumer/verifier-run{1,2}.log`). Run 3 (clean, log
   outside the worktree) is the authoritative run: exit 0, 24/24 gates,
   manifest read-back confirms head/dirty/headers/libraries/compiler. This
   run-1/2 → run-3 sequence is disclosed, not hidden: the gate that failed is
   a provenance gate about worktree dirt, not a product gate.
8. **Independent consumer probe** — the R3 21-case / R4 16-case probe sources
   were never archived (E5 `/tmp` only), so this probe is a **fresh
   equivalent**, not a reproduction. First run 13/16; all three failures were
   probe-authoring errors (sequential-write-then-read round-trip in cases
   01/14; a 63-vs-64-byte span arithmetic error in case 16), verified against
   the implementation and corrected toward the true installed semantics —
   case 16's correction adds `complete()` + `confirmed_bytes==64`, a strictly
   stronger assertion. Independence controls (ldd, link-negative,
   include-negative) are recorded in the receipt.
9–12. **Mutation cells** — full BASE GREEN / MUTANT RED / RESTORED GREEN
   evidence per mutant, including the rules-stay-green control (P2C1-3) and
   the shared-base-run structure of p2c1-mutation-b:
   see [WSL2_MUTATION_RECEIPTS.md](WSL2_MUTATION_RECEIPTS.md).

## Not run / out of matrix this round

- SDK verifier liburing profile (`--liburing`, gate `V2_P2_URING_RUNTIME`):
  not part of the named release command; not run (`receipts/sdk.md` UNKNOWN
  section).
- strace syscall oracle (`WSL2_COMMAND_MATRIX.md` §2.7): driver source never
  archived; command shape recorded, not executed this round.
- `--sanitizers=` combined option and `--hardened` release variant: exist in
  `xmake.lua` but are not members of the frozen E5 matrix; no unverifiable
  expectation invented (`WSL2_COMMAND_MATRIX.md` §7 gap 8).
- Cross-machine reproducibility of the frozen release artifact hashes: not
  tested (same host only; `receipts/release.md` cmd-04 note).
