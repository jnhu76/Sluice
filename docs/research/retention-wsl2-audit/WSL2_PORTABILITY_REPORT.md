# WSL2 Portability Report (retention-wsl2-audit)

- Date: 2026-10-30 (audit executed 2026-10-29/30, Asia/Shanghai).
- Subject: `master` = `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`
  (= `origin/master` = P2C1 #612 squash) and C1 #602 at
  `43cb8029ca60b572c91a9aa9dccb17d4300e2f88`.
- Role: WSL2 portability narrative for the branch-retention audit campaign —
  baseline freeze (B0), platform/compiler assumption audits (B1), the
  correctness matrix (B2), mutation receipts (B3), and the dual independent
  re-review with the performance round (B4).
- Discipline: audit and non-destructive verification only. No product code was
  modified; no PR merged/closed; no branch/worktree/evidence deleted; no
  force push. All builds ran inside dedicated `/tmp/sluice-val-*` and
  `/tmp/sluice-perf-*` worktrees; the main workspace was never built in.
- Companion documents: `wsl2-baseline-freeze.md` (B0 raw),
  `WSL2_COMMAND_MATRIX.md` (scout), `WSL2_CORRECTNESS_MATRIX.md` (B2 rollup),
  `WSL2_MUTATION_RECEIPTS.md` (B3 rollup),
  `WSL2_PERFORMANCE_OBSERVATIONS.md` + `receipts/perf-observations.log` (B4
  performance), `correctness-review-20261030.md` (B4 correctness re-review),
  `c1-platform-compiler-audit.md` and
  `p2c1-platform-compiler-assumptions-audit.md` (B1).

## B0 — Baseline identity

All values were collected live on this host and frozen with raw outputs in
`receipts/01-host-kernel.txt` … `receipts/06-merge-shas.txt`; the full freeze
record is `wsl2-baseline-freeze.md`.

| Item | Value |
| --- | --- |
| Host | `Hu`, Ubuntu 26.04.1 LTS (resolute) |
| Kernel | `Linux 6.18.54.1-microsoft-standard-WSL2 #1 SMP PREEMPT_DYNAMIC Tue Oct 6 19:13:07 UTC 2026 x86_64 GNU/Linux` |
| Toolchain | gcc/g++ 15.2.0 (Ubuntu 15.2.0-16ubuntu1); clang 21.1.8 (6ubuntu1); glibc 2.43; xmake v3.1.2+HEAD.5d21e9d (same xmake version as E5; compilers are NOT the E5 GCC 16.2.1) |
| Filesystem | repo `/home/hoo/Projects/Sluice` on ext4 (`/dev/sdd`, WSL2 native root — not `/mnt/*`, not 9p/drvfs); `/tmp` tmpfs (`receipts/03-filesystem.txt`) |
| liburing | 2.14 (Ubuntu `liburing-dev`/`liburing2` 2.14-1); `/proc/sys/kernel/io_uring_disabled` = 0 (`receipts/04-liburing.txt`) |
| SDK headers | `include/sluice/` top level = 8 entries (dirs `async/`, `blocking/`, `detail/` + 5 headers); recursive `.hpp` total = 26 vs the "~11" expectation — both counts recorded as-is; the delta reflects subdirectory organization, not missing files (`receipts/05-sdk-headers.txt`) |
| C1 identity | `43cb8029ca60b572c91a9aa9dccb17d4300e2f88` — GitHub **squash** (single parent `ac995641…`), ancestor of `origin/master`; no merge commit exists for #602 (latest merge is `3df10dc4` / #583) (`receipts/06-merge-shas.txt`) |
| P2C1 identity | `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` — squash (single parent `4b962abb…`); it **is** `origin/master` HEAD itself, matching the script-confirmed expectation |
| Workspace | clean apart from the untracked audit report directory `docs/research/retention-wsl2-audit/` |
| binaryIdentity | `pending-release-build` — instructed placeholder; the Release verifier's sha256 must backfill it before any release-artifact claim |

No toolchain blocker: gcc, g++, clang, ldd and xmake all present; no NONE
entries were needed.

## B1 — Platform and compiler assumption audits (C1 + P2C1)

Two independent read-only audits covered the two recent production slices on
this toolchain. Both reports live in this directory; raw outputs in
`receipts/07-c1-toolchain.txt` … `receipts/17-top-edge-einval-platform.txt`.

**C1 audit** (`c1-platform-compiler-audit.md`): 0 high, 0 medium, 2 low. No
UB (signed overflow / strict aliasing / uninitialized reads / OOB), no
value-losing implicit `off_t`/`ssize_t` narrowing, no semantics that depend on
a specific optimizer. The proof chain (execute verdict ⇒ `offset+length ≤
off_t::max` ⇒ value-preserving cast; composition-step offsets are subsets of
the validated range) is built from unsigned arithmetic plus entry admission,
and is compiler-independent. Verification actually executed: 9 suites GCC
15.2 release all green (`receipts/08-c1-gcc152-suite-runs.txt`), 8 suites
ASan+UBSan green (`receipts/11-c1-gcc152-asanubsan-runs.txt`), 8 suites +
10/10 mutants killed under clang 21.1.8
(`receipts/10-c1-clang21-suite-and-mutants.txt`) and 10/10 under GCC 15.2
(`receipts/09-c1-gcc152-mutant-kills.txt`), zero warnings under
`-Wconversion`/`-Wsign-conversion` on both compilers
(`receipts/12-c1-wconversion-scan.txt`,
`receipts/13-conversion-warnings.txt`), LP64 `off_t`/`ssize_t` identity probes
(`receipts/07-c1-typesize.cpp.txt`), and GCC 15.2 `-O3` disassembly
(`receipts/13-c1-blocking-file-gcc152-O3.dis`,
`receipts/14-c1-gcc152-instruction-counts.txt`). Not executed (disclosed):
E5 gcc 16.2.1 native replay (host unavailable; archived ledger cited), the
liburing=y matrix, and -O0 suites (covered by the archived merge evidence or
non-essential).

**P2C1 audit** (`p2c1-platform-compiler-assumptions-audit.md`): no
high-severity findings. The four exit flavors of the new prevalidated compose
loop map field-for-field onto the retained `compose_progress`/`compose_error`
oracle; the `off_t` cast safety is backed by entry admission
(`range_is_valid` bounds `offset+confirmed ≤ INT64_MAX`, probed to a
full-width offset, `receipts/15-boundary-probe-and-clang.txt`); EINTR retry
and errno classification preserved; no UB under ASan+UBSan; no conversion
warnings on either compiler; all seven composition mutants killed by their
named assertions; semantics pass at -O0 (`receipts/18-debug-o0-suites.txt`),
so nothing depends on optimization. Register residency (%r14) and the
one-materialization-per-exit pattern hold qualitatively on GCC 15.2 -O3; no
correctness claim rests on E5 codegen.

Low findings across both audits (all `verified`, none contract-internal):

1. **THREAD-01 fd-reuse flavor** — the ledger's SEM-05 disclosure accurately
   covers the EBADF-vs-`invalid_state` delta but omits the fd-reuse flavor:
   under a THREAD-01-violating mid-composition close plus descriptor reuse, a
   prevalidated read step can silently read a different file's data and
   report success. P2C1 widened the window from per-step to whole-composition;
   the race class is pre-existing and contract-external; recommendation is a
   one-sentence ledger addition.
2. **E5 machine-code evidence is not transferable** — full-function
   instruction counts on WSL2 (GCC 15.2: `read_at`=111, `read_exact_at`=163;
   clang 21.1.8: 102/138) vs the E5 exact-path caliber (≈40) differ by both
   compiler and counting caliber. Purely a performance-evidence scope issue;
   **any WSL2 re-adjudication of C1 performance must re-measure, never cite
   E5 counts.**
3. **Kernel top-edge EINVAL** — the top-edge request that SEM-04 admission
   legally accepts (`offset+length-1 == INT64_MAX`) is rejected by this
   kernel (6.18 WSL2) with EINVAL (not EOF) on tmpfs and ext4; Sluice surfaces
   it correctly as `primitive_error`/`backend_error`/`os_errno=EINVAL` with
   `confirmed=0`, identical to the C1 baseline — a platform envelope fact,
   not a P2C1 regression (`receipts/17-top-edge-einval-platform.txt`; mainline
   provenance of the kernel check UNKNOWN).
4. **Test-seam-only truncation** — `native_pread`/`native_pwrite` script path
   casts `off_t` to `long` (would truncate on ILP32+LFS); unreachable on the
   audited x86_64 platform; production steps pass `off_t` directly.
5. **Citation inaccuracy in the C1 audit report** (found by the correctness
   re-review, F5): `c1-platform-compiler-audit.md:118-120` attributes an
   errno-mapping jump table (`movslq … ; jmp *%rsi`) to `receipts/13`, but
   that pattern appears 0 times in the 1,517-line receipt (grep); `read_at`'s
   errno→Code mapping actually compiled to a direct compare chain. The
   finding's conclusion (E5 counts non-transferable) is unaffected —
   `receipts/14`'s own disclaimer and the 111-vs-40 caliber gap carry it —
   but the sentence should be corrected to the actual instruction shape or
   re-anchored to an archived disassembly.

## B2 — Correctness matrix (summary)

Full configuration × result table with per-row commands, Direct
Primitive/Composition attribution and receipt links:
[WSL2_CORRECTNESS_MATRIX.md](WSL2_CORRECTNESS_MATRIX.md). Summary:

| Cell | Result | Status |
| --- | --- | --- |
| release (`--liburing=n`) | 58/58, 0 failed; Direct Primitive 9/9, Composition 4/4 | ADAPTED |
| debug | 58/58; Direct 9/9, Composition 4/4; `-O0` suites 15/15 + 9/9 | ADAPTED |
| ASan | 58/58, zero ASan/LSan reports; sanitizer effectiveness independently evidenced | PASS |
| UBSan | 58/58, `runtime error` count 0 | PASS |
| ASan+UBSan | 58/58; 387/387 compile commands carry both sanitizers; zero reports | PASS |
| liburing on | 79/79 (capability probe ok; 21/21 uring-only targets; new evidence on this host, not an E5 reproduction) | ADAPTED |
| liburing off | 58/58 (honest stub: 0×`SLUICE_HAS_LIBURING`, profile `off` ×37) | ADAPTED |
| SDK package verifier | exit 0, 24/24 gates (11 contracted headers, 422 defined symbols, OR/mutant-define negative probes); manifest head/dirty verified | ADAPTED |
| Independent consumer | 16/16 PASS against the installed prefix only, with link/include independence controls | ADAPTED |
| mutation cells (C1, P2C1-a, P2C1-b) | RESTORED GREEN 58/58 after every mutant | ADAPTED/PASS |

No cell recorded a failure; no assertion was weakened anywhere. The
`ADAPTED` label records executed, documented, coverage-complete command
deviations (worktree-path substitution, xmake v3.1.2 test-selector syntax,
clean-build preamble); it never indicates a weakened check.

## B3 — Mutation receipts (summary)

Full per-mutant BASE GREEN / MUTANT RED / RESTORED GREEN record:
[WSL2_MUTATION_RECEIPTS.md](WSL2_MUTATION_RECEIPTS.md). Verdict: **10/10
valid kills (C1 3/3, P2C1 7/7)** on GCC 15.2 release `--liburing=n` at
`84e1b7d5`. Every kill is a semantic assertion failure (exit=1, exactly one
`FAIL:` line naming the designated killer); crash/signal/build-failure/timeout
were never accepted as kills. Injection is compile-time (mutant targets), so
RESTORED = clean tree + full matrix 58/58, executed after every mutant. The
rules-stay-green control held (P2C1-3: rules-table mutation killed by its own
oracle while the production implementation stayed 58/58). Cross-checks: the
correctness re-review independently re-killed 2/10 (C1-1, P2C1-5), and the C1
platform audit killed all 10 under both GCC 15.2 and clang 21.1.8 — no kill
depends on one compiler's codegen.

## B4 — Performance observations and dual re-review

### Performance round

Method and raw numbers: `WSL2_PERFORMANCE_OBSERVATIONS.md`, raw log
`receipts/perf-observations.log`. The archived ladder kit
(`research/direct-beyond-parity/closure/perf-p2c1/`) was reused unchanged —
`ladder.cpp` sha256 byte-identical to the kit's own `ENV-P2C1.txt` record, and
the 64 MiB fixture sha256 equals the E5 reference (`bc020fd1…2993a`). Only
instrument/fixture identity was carried across platforms; **no wall-clock
number was ever compared against E5.**

- A/A noise floor: read arms clean (eofmix MDE′ ≈ 13 ns, `read_exact` ≈ 26 ns,
  sequential 4 KiB ≈ 101 ns); the write arm demonstrated a false-positive mode
  (CI excluding zero on identical binaries, replicating away on rerun), so a
  CI-excluding write cell is only accepted if it replicates.
- A/B: 31 paired ABBA series (7 dedicated 4 KiB cells incl. tie-breaker +
  24 size-sweep cells over `read_exact_at`/`read_exact`/`write_all_at`
  /`write_all` × 0 B/1 B/64 B/512 B/4 KiB/64 KiB), 32 samples each,
  order-aware analysis: **every cell parity**; the negative control
  (`read_at`, non-composed) stayed parity as preregistered; the single
  flagged write cell (`write_all` @4 KiB sweep, Δ −45.3 ns) failed
  replication across two independent same-parameter series and sits below
  the write-arm MDE′ — resolved as noise. All 31 cells had the content
  oracle green on every sample of both sides.
- User/kernel split is qualitative (getrusage tick granularity); all ops are
  syscall-dominated on both sides with the same composition. PMU:
  `PARTIALLY_OBSERVABLE_USER_ONLY` (`perf_event_paranoid = 2` blocks
  kernel-inclusive counting); no verdict rests on PMU counts. CPU frequency
  state is unobservable on this guest (no cpufreq sysfs) — recorded UNKNOWN.
- Verdict recorded by the round: `PORTABILITY_PASS_PERF_DIFFERENT` — WSL2
  portability gates pass; none of the archived E5 per-cell win verdicts
  replicated as distinguishable under this host's session noise. The token is
  defined here (the observations report uses it undefined): *PERF_DIFFERENT =
  the E5 kit's per-cell win verdicts did not replicate as distinguishable
  under WSL2 session noise; no E5 wall numbers were compared.*

### Correctness re-review (verdict: OK, ABI_CONSISTENT)

Independent reviewer, first-party execution only
(`correctness-review-20261030.md`): re-ran release 58/58 and ASan+UBSan 58/58
in its own worktree; killer suites direct-run green (15/15, 9/9); re-killed
P2C1-5 and C1-1 with clean trees; verified SEM-01..05/ERR-01/02/THREAD-01
against source; ABI:

- public installed headers: `git diff 43cb8029^..HEAD` over the installed
  header set = **0 lines** (changes confined to the uninstalled
  `detail/file_semantics.hpp`, `src/blocking_file.cpp`, `xmake/tests.lua`);
- archive global defined-symbol sets (same toolchain, release):
  `libsluice_core.a` 22 symbols IDENTICAL, `libsluice_async.a` 233 symbols
  IDENTICAL, pre-C1 `ac995641` vs HEAD `84e1b7d5`;
- installed-face corroboration from the verifier receipt (read, not re-run):
  24/24 gates + 16/16 consumer.

Five low findings (F1–F5), all verified; F5 is the C1-report citation
inaccuracy listed under B1.

### Performance re-review (verdict: CONCERNS)

Independent performance reviewer
(`WSL2_PERFORMANCE_OBSERVATIONS.md` cross-examination against its receipts):

- **HIGH — A/B baseline mislabeled.** The report's central attribution claim
  is false: `WSL2_PERFORMANCE_OBSERVATIONS.md:21` and
  `receipts/perf-observations.log:4` call baseline `77e5399a` a "stacked base
  (pre-#602/#612)", and §1/§11 claim the measured production delta is
  "C1 (#602) + P2C1 (#612)". In fact `77e5399a` is the **PR #602 branch head
  whose production content already IS C1** (its pre-squash parent `4c3e698d`
  is byte-identical to squash `43cb8029` on `src/`+`include/`). The A/B
  therefore measured **only the P2C1 (#612) increment** on top of C1; C1's
  WSL2 wall-clock effect was never isolated. Mitigating: the measured pair
  exactly replicates the archived E5 kit's structure (E5 SUMMARY §0 pairs PR
  #612 head `70d48f5e` vs PR #602 head `77e5399a`), so the experiment is the
  right like-for-like cross-platform comparison — only the label is wrong.
  Fix is a label correction in the report (baseline = PR #602 head,
  production-identical to master's C1; measured delta = P2C1 increment), not
  a re-run. Also record that `77e5399a` is NOT an ancestor of `origin/master`
  (merge-base `bdfd0d8d`; reachable only via
  `origin/backup/pr-612-original-head-70d48f5e`).
- Low: read-arm "parity" needs the "indistinguishable ≠ proven equal"
  qualifier the report applies only to write arms — WSL2 MDE′ is 13 ns
  (eofmix) / 26 ns (cursor), and the archived E5 kit's confirmed wins on the
  same instrument (−12.04 ns eofmix, −20.17 ns cursor) sit **below** those
  MDE′; WSL2's non-detection is a statistical-power fact, not a contradiction
  of E5.
- Low: the verdict token `PORTABILITY_PASS_PERF_DIFFERENT` is undefined in
  the report (single occurrence, :234); defined in this report above.
- Low: the oracle-smoke sentence overstates the receipt (15 smokes: 10 arms
  on the candidate binary, only the 5 read arms on the base binary; no
  standalone base write smoke). Actual base write-arm oracle coverage is
  intact via in-series enforcement (`paired2.sh` runs under
  `set -euo pipefail`; e.g. the ab-wx-4k series shows 32/32 `content_ok=1`
  lines across both sides). Sentence should be corrected to match the
  receipt.
- Low: negative-control discipline partially replicated — the WSL2 round ran
  the control as A/B (parity, satisfying the E5 gating rule) but no A/A of
  the control arm, so the control's own WSL2 noise floor was never
  established; immaterial to the gate that passed, but the A/A floor table
  should note the omission.

The performance re-reviewer did not adjudicate ABI (correctness reviewer's
jurisdiction); no ABI claim was made or relied on by the performance report.

## Conclusions

1. **Portability verdict: PASS (correctness), with performance explicitly
   out of E5 scope.** On this WSL2 host the C1+P2C1 slices pass the full
   correctness matrix (release/debug/ASan/UBSan/ASan+UBSan all 58/58,
   liburing y 79/79 + n 58/58, SDK verifier 24/24, consumer 16/16), kill all
   10 mutants, show ABI_CONSISTENT (0-line public-header delta; 22+233
   archive symbols IDENTICAL pre-C1↔HEAD), and contain no compiler-dependent
   semantics (green at -O0; 10/10 kills on two compilers).
2. **WSL2 does not re-adjudicate E5 performance.** No E5 wall number was
   compared, no E5 instruction count carries weight here (different compiler,
   different caliber — B1 finding 2), and the round's own verdict
   (`PORTABILITY_PASS_PERF_DIFFERENT`) only records that the E5 kit's
   per-cell win verdicts did not replicate as distinguishable under WSL2
   session noise. Any future WSL2 performance claim about C1 must be a fresh
   measurement.
3. **Even the WSL2-local attribution is narrower than labeled.** Per the
   high-severity re-review finding, the measured A/B isolates the **P2C1
   increment only**; C1's WSL2 wall-clock effect was never isolated. This is
   a report-label defect (the experiment pair itself mirrors the archived E5
   kit's pairing), to be fixed by a label correction, not a re-run.
4. **Known behavioral deltas are contract-external and disclosed** (THREAD-01
   race flavor; kernel top-edge EINVAL platform envelope), and one report
   citation inaccuracy (F5) needs an erratum. None affects contract-internal
   behavior.

## Limitations and not-run items (explicit)

- `binaryIdentity` remains `pending-release-build` until the Release
  verifier's sha256 is backfilled into receipts.
- E5 gcc 16.2.1 replay: impossible from this host; archived ledger cited with
  its own caliber disclaimers.
- SDK verifier liburing profile gate (`V2_P2_URING_RUNTIME`), strace oracle,
  `--sanitizers=`/`--hardened` legs: not run (see
  `WSL2_CORRECTNESS_MATRIX.md` §Not run).
- 8/10 mutants not re-run by the re-reviewer (taken from receipts;
  disclosed); 2/10 re-killed independently.
- PMU kernel-inclusive counters blocked (`perf_event_paranoid=2`); CPU
  frequency state UNKNOWN on this guest; getrusage split qualitative; the
  in-tree bench cross-check is position-bias dominated (±5%) and was not used
  as evidence.
- io_uring/request-path compositions are outside the audited slices' scope;
  the async archive symbol-set identity supports (does not prove) that scope.
- Raw E5 evidence preservation is a separate, unresolved workstream
  (`E5_RAW_EVIDENCE_PRESERVATION_PLAN.md`, status `E5_RAW_ARCHIVE_PENDING`);
  it does not affect this portability verdict.
