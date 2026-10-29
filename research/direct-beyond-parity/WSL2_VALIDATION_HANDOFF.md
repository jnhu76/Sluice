# WSL2 Validation Handoff — Direct Beyond-Parity Product Candidate

- Authority: `MISSION.md` §8.3 (P6.3) — frozen material for the **separate,
  post-mission WSL2 portability workflow**. WSL2 does not participate in
  performance-admission adjudication (`MISSION.md` §0:28-30).
- **Supersedes the 2026-10-28 P6-era draft of this file** (which targeted the
  local-only research candidate `deaf0301`). That draft remains retrievable at
  branch `research/direct-beyond-parity-evidence` commit `292e0734`. The
  validation target is now the actual stacked product candidate frozen below;
  the research candidate's product-relevant content is contained in the
  product chain (`42f5eef9`+`cc8ad2ab` are cherry-picks of `d79244b0`+`31d18b6a`;
  the only production delta beyond them is the audited dead-store shrink
  `b12fb551`).
- Prepared by: E5 closure closeout (R9/release lane), 2026-10-29. Creates
  nothing on GitHub beyond this archive commit; merges nothing.
- **No WSL2 command was run in the producing session.** Every E5 fact below is
  either re-verified by the closeout session (`[RUN-CLOSEOUT]`) or transcribed
  from the named closure record with the lane that executed it
  (`[RUN-R3]`/`[RUN-R4]`/`[RUN-R9]`/`[RUN-PERF]`); `[TRANSCRIBED]` = copied
  from a cited archived record without re-execution.

## 0. Frozen candidate identity

| Field | Value | Evidence |
|---|---|---|
| **Base SHA** | `77e5399a45998dea396699fa2e063c94a665945a` = PR #602 head (C1 product), branch `sluice-600-direct-c1`, remote-verified 2026-10-29 | `[RUN-CLOSEOUT: git ls-remote origin refs/heads/sluice-600-direct-c1]` |
| **Candidate SHA** | `70d48f5eb55a664544b5160cd4675d32150bb21e` = PR #612 head, branch `sluice-608-p2c1-read-compose` | `[RUN-CLOSEOUT: git ls-remote origin refs/heads/sluice-608-p2c1-read-compose]` |
| **PR URL** | https://github.com/jnhu76/Sluice/pull/612 (OPEN, not merged, base = `sluice-600-direct-c1`); stacked on https://github.com/jnhu76/Sluice/pull/602 (OPEN, base `master`) | `[RUN-CLOSEOUT: gh pr view 602/612]` |
| **Stacking** | `git log --oneline 77e5399a..70d48f5e` = exactly 4 commits: `42f5eef9` (test: seams+killers) → `cc8ad2ab` (perf: prevalidated steps) → `b12fb551` (refactor: dead-store shrink) → `70d48f5e` (docs: ledger) | `[RUN-CLOSEOUT]` |
| B0 (context) | master `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57` = remote master, unchanged | `[RUN-CLOSEOUT: git ls-remote origin refs/heads/master]` |

The candidate is stacked **on B1 (`77e5399a`), not on B0**. A WSL2 diff must
state its base; if #602/#612 merge before the WSL2 workflow starts, re-anchor
to the merged trunk and re-derive the delta instead of copying the numbers
below.

## 1. Code delta (Base → Candidate)

`git diff --name-status 77e5399a..70d48f5e` `[RUN-CLOSEOUT]`: **5 files, all
M, +145/−38** — no file added, none deleted.

| File | Δ | Content |
|---|---|---|
| `src/blocking_file.cpp` | +124/−38 | `step_retry_on_eintr`; private `prevalidated_pread_step`/`prevalidated_read_step` (pread/read + EINTR retry + count, entered after the single outer admission); three per-exit-flavor outcome materializers (complete/stalled/primitive_error, aggregate-init form after `b12fb551`); register-resident compose loop; `direct_outcome`/`publish_certainty` deleted (no remaining consumer) |
| `include/sluice/detail/file_semantics.hpp` | +9 | 3 compile-time `SLUICE_COMPOSE_MUTANT_*` seams on the SEM-05 rule table `compose_progress`; detail header is uninstalled (v2-r1 Appendix B SIMPLIFY→internal) |
| `tests/direct_composition_fault_test.cpp` | +27 | named killer `write_all_impossible_count_stops_immediately` |
| `xmake/tests.lua` | +21 | 7 `direct_mutant_target()` entries — target-scoped, `set_default(false)`, no `add_tests`; never run by plain `xmake test` |
| `docs/roadmap/v2-conformance.md` | 1 line | SEM-05 row: live mutation evidence write-back + THREAD-01 out-of-contract disclosure. Contract text untouched. |

## 2. ABI / Header identity

- Public header `include/sluice/blocking/file.hpp` blob is **byte-identical
  across master, Base and Candidate**: blob `e62a33c39425de0fc0eb2ea10d34fa33c3177447`
  at `bdfd0d8d`, `77e5399a`, `70d48f5e` `[RUN-CLOSEOUT: git ls-tree <sha> --
  include/sluice/blocking/file.hpp]`. No public declaration changed; `Result`
  / `CompositionOutcome` layouts untouched.
- Installed SDK surface unchanged: `scripts/verify_v2_package.py` all gates
  PASS (24/24) at `70d48f5e` with manifest `VERIFICATION_HEAD=70d48f5e…`,
  `VERIFICATION_HEAD_DIRTY=False`, HEADERS = exactly the 11 v2-r1 Appendix B
  contract headers, no `detail/` header installed,
  `NEGATIVE_V2_MUTANT_DEFINES_NOT_INSTALLED` PASS `[RUN-R9,
  /home/jnhu/Source/Sluice-r9-audit-p2c1]`.
- The four public primitives `read_at`/`write_at`/`read`/`write` are
  instruction-stream identical Base vs Candidate (byte-equal objdump
  extractions) `[RUN-R4 §7]`; the changed machine surface is exactly the four
  composition primitives `[RUN-PERF §5, re-verified R9 §12.2 F3/F4]`:
  `read_exact_at` 240→174, `write_all_at` 240→211, `read_exact` 208→167,
  `write_all` 208→194 instruction bodies (GCC 16 calibers — re-derive, never
  copy).

## 3. E5 correctness conclusion (frozen)

All at Base+Candidate stack, host E5 (Fedora 44, kernel
`7.2.11-200.fc44.x86_64`, GCC 16.2.1, xmake 3.1.2):

1. Suite matrix at `70d48f5e` `[RUN-R4 §6]`: release/debug/asan/ubsan/asanubsan
   `--liburing=n` → **58/58 ×5**, plus `release --liburing=y` → **79/79**
   (io_uring backend suite). Zero failures; UBSan output grep `runtime error`
   = 0.
2. P2C1 seven-mutation gate `[RUN-R4 §5]`, re-executed by R9 `[RUN-R9]`:
   all 7 exit=1 with exactly one named FAIL line each (table §5 below);
   BASE/RESTORED green; source tree zero-pollution.
3. C1 admission mutants remain lethal on the stacked tree `[RUN-R9]`
   (`blocking_file_mut_empty_before_closed` at `70d48f5e` → exit=1,
   FAIL `sequential_read_on_write_only_file_rejected_upfront`).
4. Installed-SDK consumer probe 16/16 (closed composition → `invalid_state`
   incl. closed-before-empty; access-before-empty; past-EOF prefix +
   `eof_before_full`; zero-length; UINT64_MAX; move/close) `[RUN-R4 §8.2]`.
5. strace syscall oracle `[RUN-R4 §8.3]`: full `read_exact_at` = 1×`pread64`;
   31B/23B = 2 syscalls (data step + EOF step, one syscall each); zero-length
   0 syscalls; closed 0 syscalls; `write_all_at` 1×`pwrite64`. Candidate
   changes no syscall counts vs the B1 oracle.
6. Independent correctness review (R9 §12.1): **APPROVE** — admission
   precedence, range/offset safety, `static_cast<off_t>` domination proof,
   null-buffer, zero-length, closed/access, EINTR mapping completeness,
   composition stops, File lifetime, ABI/v2-r1 correspondence all re-verified
   from source at both heads; every diff hunk read (`closure/r3-c1-review.md`,
   `closure/r4-p2c1.md`; R9 record:
   `closure/r9-independent-correctness-review.md`).
7. CI note: `gh pr checks 612` shows **no GitHub Actions checks** (only
   CodeRabbit "skipping"); the stacked-base PR #602 has CI 6/6 green at
   `77e5399a` (run 40363163278) `[RUN-R9]`. E5 correctness evidence for #612
   is therefore **local-execution-based**; a CI run on the branch is a
   desirable (not blocking) extra when WSL2 or Owner review starts.
8. THREAD-01 boundary (disclosed, not a defect): under caller serialization
   the candidate is behaviorally identical to B1; under a **contract-external**
   concurrent `close` mid-composition, B1 stops with `invalid_state` at the
   next step while the candidate continues into `pread` on the exchanged fd
   (−1) and surfaces a POSIX-defined EBADF-class `backend_error` — no UAF, no
   allocation, caller-owned lifetime in both trees (`src/file_resource.cpp:103-112`).
   Disclosed in ledger SEM-05 row + PR #612 body. If adoption proceeds, this
   lands in the obligation table.

## 4. E5 performance applicability scope (frozen)

- **Current-head serial-lane re-verify (2026-10-29, `closure/perf-p2c1/`)**
  `[RUN-PERF]`: ABBA paired A/B on tmpfs 4 KiB, taskset cpu2, budget ≤24
  pairs/shape, after A/A-first discipline (5 cells ×16 pairs):
  - comp-read-4k (ax seq, 24/24 pairs): median −11.688 ns/op, CI95
    [−20.522, +11.188] **crosses 0** (auditor recompute with correct
    t(0.975,23)=2.069: [−20.060, +10.727] — still crosses 0), |median| <
    MDE′24 15.39 → **INCONCLUSIVE** under session heavy-tail noise
    (sd_AA up to 35.6, ±70–100 ns outliers); direction favorable in both
    16/24-pair views and AB/BA subgroups; PMU −31.0 instr/op independent
    corroboration (never a wall substitute).
  - eofmix (ax tail, 16 pairs): median −12.036, CI95 [−26.389, −7.513]
    excludes 0, ≥ MDE′16 10.40 → **WIN_CONFIRMED**.
  - cursor (ac seq, 16 pairs): median −20.173, CI95 [−35.197, −10.738]
    excludes 0, ≥ MDE′16 18.84 → **WIN_CONFIRMED**.
  - Negative control (non-composed Primitive a2, 16 pairs): PARITY (median
    −0.024, CI crosses 0; total instructions equal within ±0.0004%;
    `read_at` disassembly byte-identical) — the win gate holds.
  - **Overall verdict: INCONCLUSIVE** — the primary claim carrier did not
    reach statistical confirmation in the authorized budget; no regression
    anywhere (all medians ≤0, cycles lower on ax/ac, control parity,
    non-composed primitives machine-identical).
- **Scope limits** `[RUN-PERF + R9 §12.2 F4]`: the P2C1 performance verdict
  covers the three read-composition shapes only. Write compositions
  (`write_all_at`/`write_all`) have changed machine code (§2) but were **not
  wall-measured** in this session; do not generalize the eofmix/cursor wins to
  write compositions. Absolute E5 numbers are **not** WSL2 targets; WSL2 does
  regression-only checks (§7 item 6).
- Historical frozen claims (P2 wall −16.565 CI [−20.806,−7.270]; P4
  fresh-scale WIN ×3 at −19.048/−15.907/−15.130; capped "measurable
  improvement with re-attributed residual") belong to the pre-structure
  research candidate at `deaf0301`/freeze archives; they are reference, not
  current-head evidence. Baseline continuity was re-proven mechanically: fresh
  base build's normalized disassembly multiset is identical to the frozen B1
  archive's (`blocking_file.cpp.o` multiset sha256 `7dd8faeb…` both sides)
  `[RUN-PERF §1, re-verified R9 §12.2]`.

## 5. Mutation Oracle (what must kill on WSL2)

Killers are **named suite assertions** (GREEN → RED(named single-line FAIL,
exit=1) → RESTORED GREEN) — never crash/build-failure/timeout.

C1 admission mutants (carried by Base, keep working):

| Define (target) | Killer |
|---|---|
| `SLUICE_SEM_MUTANT_ADMIT_INVALID_RANGE` | `read_with_illegal_offset_reports_invalid_argument` (`tests/blocking_file_read_test.cpp:122`) |
| `SLUICE_SEM_MUTANT_EMPTY_BEFORE_CLOSED` | `sequential_read_on_write_only_file_rejected_upfront` (`tests/blocking_file_sequential_test.cpp:392`) |
| `SLUICE_SEM_MUTANT_ADMIT_UNREPRESENTABLE_SIZE` | `resize_overflow_reports_invalid_argument` (`tests/blocking_file_state_test.cpp:320`) |

P2C1 composition mutants (candidate seams):

| Define | Killer |
|---|---|
| `SLUICE_COMPOSE_MUTANT_ADVANCE_BY_REQUESTED` | `short_reads_report_eof_before_full_with_the_prefix` |
| `SLUICE_COMPOSE_MUTANT_RETRY_ZERO_PROGRESS` (fault leg) | `short_reads_report_eof_before_full_with_the_prefix` |
| `SLUICE_COMPOSE_MUTANT_RETRY_ZERO_PROGRESS` (rules leg) | `read_exact_accumulates_a_short_prefix_then_reports_eof` |
| `SLUICE_COMPOSE_MUTANT_FAILURE_ACCOUNTED` | `error_after_confirmed_prefix_keeps_the_prefix` |
| `SLUICE_COMPOSE_MUTANT_SKIP_IMPOSSIBLE_COUNT` | `impossible_count_stops_immediately` |
| `SLUICE_DIRECT_MUTANT_STEP_REPORTS_FULL_COUNT` | `short_reads_report_eof_before_full_with_the_prefix` |
| `SLUICE_DIRECT_MUTANT_STEP_SKIPS_EINTR_RETRY` | `primitive_retries_eintr_without_a_completed_count` |

Rules-stay-green control: the SEM-05 rule table (`detail::compose_progress`)
is a **non-authoritative oracle**; a mutation of the rules table must be killed
by the fault-leg suite while the implementation stays green — the rule table
must never become a second authority. Mutant defines are compile-time,
target-scoped, default-off, and must not leak into the installed SDK
(verifier negative gate covers this).

## 6. Independent review status

| Review | Verdict | Record |
|---|---|---|
| C1 product review (R3, §12.1 scope: 10 sections) | APPROVE (C1_PRODUCT_REVIEW) | `closure/r3-c1-review.md` |
| P2C1 productization (R4) | P2C1_PRODUCT_READY (perf handed to serial lane, NOT_RUN_WITH_REASON) | `closure/r4-p2c1.md` |
| Independent correctness review (R9 §12.1) | APPROVE — re-ran release suite ×2, all 10 mutants, verifier 24/24; read every diff hunk + full source at both heads | `closure/r9-independent-correctness-review.md` |
| Independent performance review (R9 §12.2) | APPROVE — all 12 mission checks re-verified from raw samples; 6 non-blocking findings (transcription direction flip F1, analyzer t-table fallback F2, archive-completeness F3, enumeration scope F4, un-archived side numbers F5, A/A asymmetry F6 — none changes a verdict) | `closure/r9-independent-performance-review.md` |

## 7. Platform assumptions to verify on WSL2 (each is an E5 fact, not a portability guarantee)

1. **Syscall surface**: candidate steps use only `pread`/`read` (+EINTR retry)
   and the public `write_at`/`write`; no vectored/cfr/io_uring/alignment
   dependency. WSL2 kernel must provide `pread64`/`pwrite64` with POSIX
   semantics incl. EINTR-before-data (verify; the EINTR retry discipline
   `retry_on_eintr` retries only `result<0 && errno==EINTR`).
2. **`CompositionOutcome` layout** (24B aggregate: confirmed@0, end@8,
   remaining@9, optional@12; libstdc++ optional payload-first) is a GCC/libstdc++
   observation — verify layout independently on the WSL2 toolchain before
   trusting any E5 dead-store reasoning shape.
3. **Static instruction bounds and machine-code shapes are GCC-16 calibers** —
   re-derive from fresh objdump at the WSL2 compiler; check structural
   invariants (one admission per call; one pread/read per step; single-point
   exit materialization; no partial dead store), never copied numbers.
4. **Filesystem identity**: E5 evidence is tmpfs (warm) + btrfs zstd:1; WSL2
   native fs is ext4 — expect different noise floors and different cold-cache
   behavior; fixtures must live on the native fs (never `/mnt/*` 9p/DrvFS for
   timed cells).
5. **Governor/scheduler**: E5 ran schedutil (P5 onward) with elevated session
   noise (sd_AA 19.5–35.6); WSL2 has its own scheduling path — establish a
   fresh A/A noise floor before any paired run.
6. **liburing**: matrix must pass `--liburing=n`; the `--liburing=y` 79/79 leg
   is optional on WSL2 (io_uring availability differs) — if run, it is new
   evidence, not a reproduction.
7. **xmake user-local install** (v3.1.2+HEAD.5d21e9d at user path) and
   relative-`builddir` cwd behavior — reproduce the tool, configure from
   inside the worktree.
8. **glibc error mapping**: `kNativeErrorMappings` assumes glibc errno values;
   WSL2 glibc/MSVCRT interplay must not introduce new errno values on the
   exercised paths.

## 8. WSL2 build & test checklist (ordered)

Work on the frozen identity in §0 (or re-anchored trunk after merges; never
rewrite SHAs).

1. **Matrix**: configure `xmake f -m <mode> --liburing=n -y && xmake -y &&
   xmake test` for `release`, `debug`, `asan`, `asanubsan` → **58/58 each, 0
   failed**; optional `release --liburing=y` → 79/79.
2. **Mutation gate**: build and run all 10 mutant targets (3 C1 + 7 P2C1) →
   each exit=1 with exactly the named FAIL line from §5; then confirm
   `git status --porcelain -- src/ include/ tests/ xmake/ docs/` empty and
   re-run the release suite → RESTORED 58/58.
3. **Package verifier**: `python3 scripts/verify_v2_package.py --mode release
   --manifest-out <tmp>/v2-manifest.json --keep-prefix` → all gates PASS; 11
   contract headers; no `detail/` header; mutant defines not installed.
4. **SDK consumer probe**: compile+link a consumer against the installed
   prefix using only `<sluice/blocking/file.hpp>`; re-check the 16-case
   behavior set (full round-trip, past-EOF prefix + `eof_before_full`,
   zero-length, UINT64_MAX → `invalid_argument`, closed → `invalid_state`,
   access-before-empty, move/close) and `Result<CompositionOutcome>` field
   accounting (`confirmed`/`remaining`/certainty).
5. **strace oracle**: full `read_exact_at` = 1 syscall; 31B/23B = 2; zero
   length = 0; closed = 0 (shape, not timing).
6. **Independent regression check (never absolute perf)**: A/A noise floor
   first (≥16 pairs); then paired ABBA at comp-read-4k / eofmix / cursor +
   negative control a2; adjudicate regressions only; expect different noise
   bands; never compare ns against E5 values in §4.
7. **THREAD-01 documentation**: confirm §3 item 8's disclosed boundary is
   understood and lands in the obligation table if adoption proceeds.
8. **Environment pinning**: write an env-pin per session (kernel, governor
   equivalent, load, fs identity, toolchain, xmake path), mirroring
   `closure/perf-p2c1/ENV-P2C1.txt`; serial quiescent-host discipline.

Boundary: WSL2 must not confirm or rewrite E5 native performance evidence;
its deliverable is correctness/portability/ABI/compiler/regression evidence on
the frozen candidate.
