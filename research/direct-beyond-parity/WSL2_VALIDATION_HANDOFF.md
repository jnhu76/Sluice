# WSL2 Validation Handoff — Direct Beyond-Parity Product Candidate

- Authority: `MISSION.md` §8.3 (P6.3, MISSION.md:866-890) — frozen material for
  the **separate, post-mission WSL2 portability workflow**. WSL2 does not
  participate in performance-admission adjudication for this mission
  (`MISSION.md` §0, MISSION.md:28-30).
- Prepared by: P6 closeout session, 2026-10-28. This document is a draft
  consolidation record; it creates nothing on GitHub and merges nothing.
- Verification legend: `[RUN-THIS-SESSION]` = command executed by the P6
  session that wrote this file (commands cited inline); `[TRANSCRIBED]` =
  transcribed from the frozen mission documents cited inline, not re-executed
  here.
- Validation target (per `INTEGRATION_AND_INDEPENDENT_REVIEW.md` §6.1, the
  preregistered P5 fallback): **A alone = P2C1** (read-composition with
  prevalidated register-resident steps). The stacked branch and P2C2 are
  frozen secondary identities, not validation targets (§2 below).

## 1. BASE_SHA

Two anchors, both required reading before any WSL2 comparison:

| Code | Role | Identity | Notes |
|---|---|---|---|
| B0 | master at freeze | `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57` | `[RUN-THIS-SESSION: git rev-parse master]` → same SHA (2026-10-28). The repository's public trunk; C1 is NOT in master. |
| B1 | comparison BASE used by every P2/P5 gate | `77e5399a45998dea396699fa2e063c94a665945a` = PR #602 head, branch `sluice-600-direct-c1` | B0 + two commits: `4c3e698d` (C1 product) + `77e5399a` (docs-only ledger). Frozen worktree `/home/jnhu/Source/Sluice-600-wt`; release archive sha256 `d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0` `[TRANSCRIBED: P0_BASELINE_AND_OWNERSHIP.md §1.2-§1.3; p5-raw/identity.txt read this session]`. |

The candidate is stacked **on B1**, not on B0 (`git log master..<candidate>`
includes `4c3e698d`, `77e5399a` `[RUN-THIS-SESSION]`). A WSL2 run must state
which base it diffs against; the frozen gates all used B1. If PR #602 merges
before the WSL2 workflow starts, re-anchor per
`INTEGRATION_AND_INDEPENDENT_REVIEW.md` §6.2 item 2 before diffing.

## 2. CANDIDATE_SHA

| Arm | HEAD | Branch | Local-only? | Release archive sha256 |
|---|---|---|---|---|
| **A = P2C1 (validation target)** | `deaf03014173e13633a43f4db405c9a9e9860f23` | `direct-read-compose-prevalidated-steps` | yes — `git branch -r --contains deaf0301` → empty `[RUN-THIS-SESSION]` | `07d0680307c71cc412dfd021fd47c3098de57b580283df189f66dcdc2052049e` |
| B = P2C2 (secondary, frozen) | `476bd9ce9780594163350228e066a86b554c157d` | `direct-write-compose-exit-staging` | yes `[RUN-THIS-SESSION]` | `672510281abadba1cbda0a0e89f15e1dbb2c69f5a15bceb306b70196b420a807` |
| A+B stacked (secondary, NOT admitted) | `4ab6ef170ff939001cc1459445b77c21df9e7d86` | `direct-compose-stack-readpreval-writeexit` | yes `[RUN-THIS-SESSION]` | byte-identical to A's (`07d06803…2049e`) |

Archive and worktree identities `[TRANSCRIBED: p5-raw/identity.txt, read in
full this session]`. Stacking verdict: **NOT STACK_ADMITTED** (S1 write-side
INTERFERENCE_STATIC 39>37; S2 STACK_LOSS ×3; S3 FAIL margin +0.037) — recorded
integration candidate is **A alone** per the preregistered fallback
(`INTEGRATION_AND_INDEPENDENT_REVIEW.md` §6.1 `[TRANSCRIBED]`). A's commit
chain over B1: `d79244b0` (seams + killers) → `31d18b6a` (mechanism) →
`5180e27a` (gate scripts) → `deaf0301` (ANSI-parse fix)
(`P2_CANDIDATE_P2C1.md` §1 `[TRANSCRIBED]`).

## 3. ABI / Header differences (A vs B1)

Cumulative `git diff --numstat 77e5399a..deaf0301` `[TRANSCRIBED:
P2_CANDIDATE_P2C1.md` §1]`: 6 files, +301/−37.

| File | Δ | Content |
|---|---|---|
| `include/sluice/detail/file_semantics.hpp` | +9/−0 | Three dormant `SLUICE_COMPOSE_MUTANT_*` seam guards on `detail::compose_progress`; zero BASE code change on this path |
| `src/blocking_file.cpp` | +91/−37 | Six compose seam guards; `step_retry_on_eintr`; private `prevalidated_pread_step` / `prevalidated_read_step` (pread/read + EINTR retry + count, entered after one outer admission); per-exit-flavor outcome materializers (complete / stalled / primitive_error) feeding the sret directly; register-resident compose loop. `direct_outcome` and `publish_certainty` deleted (no remaining consumer) |
| `tests/direct_composition_fault_test.cpp` | +27/−0 | New named killer `write_all_impossible_count_stops_immediately` |
| `xmake/tests.lua` | +21/−0 | Seven `direct_mutant_target()` entries for the seam set |
| `research/direct-beyond-parity/p2-scripts/{P2C1-matrix.sh,P2C1-mutants.sh}` | +50/+103 | Gate scripts |

**No public-header or ABI change** (`INTEGRATION_AND_INDEPENDENT_REVIEW.md`
§6.3 `[TRANSCRIBED]`): the four public ops `read_at`/`read`/`write_at`/`write`
have instruction streams **identical across BASE and candidate** (sha-equal
object streams; `P2_CANDIDATE_P2C1.md` §2.2), the `Result<size_t>` /
`Result<CompositionOutcome>` ABI is untouched, W-01/INV-01 direct independence
is preserved (no new include, context or runtime dependency). Adoption of A is
an internal implementation change under the existing v2-r1 contract.

## 4. Test checklist (what must be green on WSL2)

1. **Four-configuration matrix, 58/58 each**: modes `release`, `debug`,
   `asan`, `asanubsan` (repo-defined in `xmake.lua`), configured
   `--liburing=n`. Gate script: `research/direct-beyond-parity/p2-scripts/P2C1-matrix.sh`
   (exit 0 at `deaf0301`); re-executed form at the stacked HEAD:
   `research/direct-beyond-parity/p5-scripts/p5-combined-matrix.sh` (sha256
   `3efd53b4…fdce25`, four legs 0 failed, `P5_INTERACTION_OUTCOME.md` §13
   `[TRANSCRIBED]`).
2. **Mutation kill gate**: `p2-scripts/P2C1-mutants.sh` (7/7 named-assertion
   kills, GREEN→RED→RESTORED); `p5-scripts/p5-combined-mutants.sh` (sha256
   `c805f416…993d8`; 8 seam legs + rules control) `[TRANSCRIBED: P5 §13]`.
3. **Key oracle suites** (full obligation table in
   `P0_BASELINE_AND_OWNERSHIP.md` §5.1): `direct_composition_fault_test`
   (fault-injection; incl. the new write-side impossible-count killer),
   `file_access_precedence_test` (36), `semantic_validation_precedence_test`
   (direct 21/21), `semantic_range_test`, `direct_composition_test`,
   `direct_cursor_position_test`, `blocking_file_{read,write,state,sequential}_test`,
   `file_open_contract_test`, `file_close_semantics_test`,
   `semantic_effect_outcome_test`, `direct_w01_consumer_probe` (W-01
   independence static asserts).
4. Command-reproducibility caveat carried from P0 (`P0_BASELINE_AND_OWNERSHIP.md`
   §3.6): the #600-era matrix runs did not pin exact per-mode command lines;
   the P2/P5 gate scripts above are the pinned form — run the scripts, not
   reconstructed commands.

## 5. Proven semantic invariants (must survive on WSL2 unchanged)

Contract anchors are v2-r1 requirement IDs in `docs/explicit-io-contract.md`;
the oracle table with per-obligation named assertions is
`P0_BASELINE_AND_OWNERSHIP.md` §5.1 `[TRANSCRIBED]`. Summary of the floor:

- Closed-file precedence (`invalid_state` before access/range); access-mode
  legality table; offset/length/overflow never wraps (SEM-03/04).
- Zero-length = logical no-op skipping offset/range checks, no OS call;
  null buffer → empty-span no-op (SEM-03 step 3).
- EINTR retried only without a completed count; never replays confirmed
  bytes (SEM-05/ERR-01).
- Partial read/write is success; primitive does not loop; EOF read-0 is
  success; `read_exact*` keeps the prefix on `eof_before_full` (SEM-05).
- No-progress stop; confirmed-bytes accounting; effect certainty —
  uncounted attempts report unknown, never fabricated 0 (ERR-02).
- Open legality (36 scenarios incl. NUL rejection, close-on-exec, 0644);
  close consumes ownership on first native attempt incl. EINTR (SEM-02).
- Shared open-file-description cursor; positional ops never move it (SEM-04/07).
- Composition stop conditions full/eof/zero-progress/error with the shared
  rejection precedence (SEM-05/SEM-03).
- `file_info`/`size` observation-time semantics; resize zero-fill/discard,
  cursor unmoved; sync coverage of completed writes (SEM-06/07).
- THREAD-01 caller-serialization boundary; W-01/INV-01 direct independence.

P2C1-specific proofs: §3 of `P2_CANDIDATE_P2C1.md` maps every obligation to
its oracle; the four public primitives are machine-identical (§2.2); the
compose loop keeps exactly one admission per call and re-tests nothing per
step that the outer admission already proved.

**Disclosed boundary to carry (not a defect; adoption-time obligation-table
entry):** under a contract-external THREAD-01 violation (concurrent `close`
during an in-flight composition), B1's per-step re-validation would stop with
`invalid_state` at the next step, while A's prevalidated steps continue into
`pread` on the closed descriptor and surface a backend/EBADF-class primitive
error. Out-of-contract either way; recorded by the independent reviewers and
to be written into the conformance-ledger obligation row at adoption
(`INTEGRATION_AND_INDEPENDENT_REVIEW.md` §6.2 item 3, `P5_INTERACTION_OUTCOME.md`
§12.1 `[TRANSCRIBED]`).

## 6. Mutation Oracle

- **Seam set (candidate)**: three `SLUICE_COMPOSE_MUTANT_*` guards on
  `detail::compose_progress` (`include/sluice/detail/file_semantics.hpp`) +
  six guards in `src/blocking_file.cpp`; the P5 union form drives the 6-define
  seam set as 8 legs + 1 rules-control leg. Killers must be **named suite
  assertions** (GREEN→RED(named)→RESTORED GREEN) — never crash, build failure
  or timeout.
- **Rules-stay-green control**: the control leg (retargeted to the
  production-only compose seam at `4ab6ef17`, Amendment 1) must stay GREEN
  under `FAILURE_ACCOUNTED` while the implementation legs go RED — proving the
  non-authoritative SEM-05 rule table (`detail::compose_progress`) is an
  oracle, not a second authority (`P5_INTERACTION_OUTCOME.md` §2/§13
  `[TRANSCRIBED]`).
- **BASE-side admission mutants** (carried by B1, keep working): 
  `SLUICE_SEM_MUTANT_EMPTY_BEFORE_CLOSED`, `SLUICE_SEM_MUTANT_ADMIT_INVALID_RANGE`
  (in `precheck_data_op`), `SLUICE_SEM_MUTANT_ADMIT_UNREPRESENTABLE_SIZE` (in
  `precheck_resize`); xmake targets via `direct_mutant_target()`
  (`P0_BASELINE_AND_OWNERSHIP.md` §5.3 `[TRANSCRIBED]`).
- Gate scripts + sha256: see §4 item 2.

## 7. Platform assumptions behind the E5 evidence

- Host: Intel Xeon E5-2666 v3 (10C/20T, L3 25 MiB, single NUMA node), Fedora
  44, kernel `7.2.11-200.fc44.x86_64`, glibc `2.43-9.fc44`; `/tmp` tmpfs;
  root fs btrfs+zstd; governor `performance` in P0/P2 sessions, `schedutil`
  pinned in P5 (`p5-raw/env-pin-2026-10-28.txt`); `perf_event_paranoid` 1
  (P0) / 2 (P5); measurement cores via `taskset` (cpu2); 16 ABBA pairs per
  cell; quiescent-host discipline per `P2_CANDIDATE_P2C1.md` §12.1
  `[TRANSCRIBED: P0 §1.5; P5 header]`.
- Fixture: 64 MiB warm-tmpfs read fixture, sha256
  `bc020fd15a622e081793b0fb8ff2383838e27f6d1cb2135f204aad8383b2993a`;
  deterministically regenerable via
  `research/direct-beyond-parity/probe-snapshot-600/mkfixture.cpp` (fixed LCG
  pattern, no seed) `[TRANSCRIBED: P0 §1.4]`.
- NUMA axis: single-node host — any NUMA statement is `NOT_RUN_WITH_REASON`
  (`IO_WORK_REDUCTION_LEDGER.md` §13 `[TRANSCRIBED]`).
- **Absolute E5 numbers are not WSL2 targets** (MISSION §8.3: WSL2 checks
  correctness, portability, ABI, compiler difference and *independent
  regression*; it must not confirm or rewrite E5 native performance
  evidence).

## 8. Compiler conditions

- Frozen E5 toolchain: GCC 16.2.1 20260819 (`gcc-16.2.1-2.fc44`), flags
  `-m64 -fvisibility=hidden -Wall -Werror -O3 -std=c++20 -DNDEBUG`;
  binutils ar/ld 2.46.1 with deterministic archives (`ar -D`); ccache wrapper
  first on PATH — clean rebuilds ran `CCACHE_DISABLE=1`; xmake
  `v3.1.2+HEAD.5d21e9d` **user-local** (reproducing the build environment
  means reproducing that xmake at that path, not `dnf install xmake`);
  relative-`builddir` cwd caveat — always configure from inside the worktree
  `[TRANSCRIBED: P0 §1.5]`.
- Clang 22.1.8 present on E5 but **no Clang build was ever frozen** — a
  WSL2 Clang leg is *new evidence*, not a reproduction. Recommended: primary
  WSL2 leg on the distro GCC with the same flags; optional secondary Clang
  leg reported separately.
- The static instruction bounds (e.g. `read_exact_at` 68≤75 / arc 25≤26,
  `read_exact` 55≤60 / 21≤21; public-op ±0) are **GCC-16 calibers**
  (`P2_CANDIDATE_P2C1.md` §2.1/§12.4 `[TRANSCRIBED]`). On another compiler,
  re-derive bounds from fresh objdump transcripts; never copy the numbers.

## 9. E5 applicability scope (what the candidate is and is not claimed to be)

- A's admitted claim, capped at **"measurable improvement with re-attributed
  residual"** (`P2_CANDIDATE_P2C1.md` §12.4): P2 wall −16.565 ns/op
  (95% CI [−20.806, −7.270]) on the composition shape vs B1; P4 fresh-scale
  `WIN_CONFIRMED` ×3 on composition cells only (comp-read-4k −19.048
  CI[−21.376,−10.723]; eofmix −15.907 CI[−17.130,−9.639]; cursor −15.130
  CI[−22.305,−4.849]; all ≥ fresh MDE′ 12.414; PHYSICAL_WORK_DELTA = 0;
  PMU −62/−69/−52 instr/op; negative controls PARITY)
  (`REAL_WORKLOAD_COMPETITION.md` §1 `[TRANSCRIBED]`).
- The win does **not** transfer to: public primitive ops, metadata, random
  offsets, 1 MiB streaming, cold btrfs (7 W01 cells NO_CLAIM); the Parquet
  real-workload trace (PARITY at workload scale, transfer NO_CLAIM); the tail
  (P99 `NOT_SUPPORTABLE_WITH_REASON` everywhere). Kit-shape evidence, single
  E5 host, no cross-host/cross-kernel comparibility claim
  (`REAL_WORKLOAD_COMPETITION.md` §5.1 `[TRANSCRIBED]`).
- B (P2C2) has **no wall claim** (instruction caliber only); the stack is
  not admitted. Neither is a WSL2 validation target.

## 10. WSL2 focus items (correctness, portability, ABI, compiler, regression)

1. **Matrix + mutation**: run §4 items 1–3 on the WSL2 GCC; all 58/58 legs
   green; every seam killed by its named assertion; rules-control GREEN.
2. **ABI check**: link a consumer against the candidate archive; verify
   `Result<CompositionOutcome>` layout and the four public op symbols are
   unchanged vs BASE (object/symbol comparison at the WSL2 toolchain; the
   sha-equal-streams proof is GCC-16-specific and must be re-derived).
3. **Static gates re-derived, not copied**: objdump the compose loops at the
   WSL2 compiler; check the *structural* invariants (0 re-tests in step
   loops; exactly 1 pread/read per step; single-point exit materialization;
   `movaps` dead-store count 0; back-edge guards present) rather than the
   numeric bounds.
4. **Portability surface**: A uses only `pread`/`read` + EINTR retry — no
   vectored/cfr/io_uring dependency, no kernel-version-sensitive syscall, no
   alignment assumption. Build both `--liburing=n` and the default config.
   Fixtures on the WSL2 native filesystem (ext4 in the VM), never `/mnt/*`
   (9p/DrvFS) for timing cells; tmpfs for warm cells; document cache control.
5. **THREAD-01 boundary documentation**: confirm the §5 disclosed
   concurrent-close difference is understood and lands in the obligation
   table if adoption proceeds.
6. **Independent regression check (not absolute performance)**: A/A noise
   floor, then paired ABBA A/B at the W01 composition shapes (comp-read /
   eofmix / cursor) and the negative-control cells (negread-1b/4k). Adjudicate
   regressions only; expect different noise bands and first-pair outliers
   (P5 §11 class); never compare absolute ns against E5.
7. **Environment pinning**: record an env-pin file (governor, load, matching
   processes) per session, mirroring `p5-raw/env-pin-2026-10-28.txt`;
   quiescent serial discipline per `P2_CANDIDATE_P2C1.md` §12.1.
8. **Known disclosed items to watch, not fix silently**: persistent session
   A/A noise above the historical 10 ns band (P5 §11); write-track first-pair
   outlier +140…+204 ns class (P5 §11); eofmix residual direction (+10.228
   EXCESS at P2, re-adjudicated WIN at P4 fresh scale — direction instability
   is a recorded property, cap maintained).
