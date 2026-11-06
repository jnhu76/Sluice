# #595 R5 Phase A — R4_CLEANUP C1/C2 equivalence and regression evidence

Role: mechanical-fix implementer. Date: 2026-11-06/07 (UTC). Host: native E5
(verified in `r5/analysis/env-context.md` §1). Branch `research-595-r5`:

- baseline `f3b25329bc8a64a201961c426a58adacfb8fdf08` (R4_ORIGINAL)
- C1 `c55165a2` — bounded policy-side eligible-prefix scan
- C2 `b191bb89` — pass-fact signal renamed to owner-pass activity

Patches: `cleanup-c1.patch` (sha256 4ecbe2b6526e7e533c67c4eee08ee39f97430037
7533436e89b1222618373865), `cleanup-c2.patch` (sha256 39e9cf4e75e6d78122b6b502
b95cfa6df4d11890351e2f40e3506dbacfb10bf6).

No timed/performance comparison was run in this subtask, so the
MEASUREMENT.lock was not taken (probed free once before builds). All runs are
functional/counting runs. r4/ was only read; the main repo master stayed
clean at `271b81bf` throughout.

## Method

1. Build: `xmake f -y -m release --liburing=y --builddir=/home/jnhu/sluice-595
   -e5/r5/build` in the r5 worktree, then `xmake build -y <targets>` per
   stage. Binaries land in `r5/build/linux/x86_64/release/`; the worktree has
   no build products (`.xmake/`, `build/` are gitignored; porcelain clean).
   Where xmake's incremental tracking missed a header change (observed once
   for `bench_w03_external_loop`), stage binaries were force-rebuilt with
   `xmake build -r` and verified distinct by sha256 before any A/B claim:
   baseline `7bf83eb9…`, C1 `26c0756f…`.
2. Deterministic transport-event oracle (the ask's 18-scenario R4 harness):
   `r4/evidence/correctness/uring_ready_reuse_correctness_test.cpp`
   (sha256 `5c10be9e…`, matches `r4/evidence/correctness/RECEIPT.md` line 25)
   compiled outside the repo in `/tmp/sluice-r5-eq/` with the exact target
   flags from `xmake/tests.lua` (`-std=c++20 -Wall -Werror -O3`,
   `-DSLICE_ASYNC_INTERNAL_TESTING -DSLICE_HAS_LIBURING -DNDEBUG`, includes
   `include`, `src/async`, `bench`), linked against the xmake-built seam
   objects plus `libsluice_core.a` and `-luring`. Run once in full and once
   per scenario (`--only=`, 18 names) at each stage; rc recorded per
   scenario; full outputs diffed byte-wise.
   - C2 name adaptation: the archived source references the renamed fields,
     so the C2-stage copy (`harness_c2names.cpp`, sha256 `58e9b6a5…`) differs
     from the archived source ONLY by the renamed identifiers/row string and
     one corrected assertion label; the full diff is
     `raw-diag/equiv/c2/harness_c2name_adaptation.diff` (9 hunks, all
     mechanical renames; no condition changed).
3. In-repo suites at each stage: `r4_arm_smoke` (asserts exact decision
   shapes, presentation counts and trace records per arm),
   `uring_ready_batch_deterministic_test` (26 `--only` scenarios),
   `uring_ready_batch_research_test`, `uring_ready_batch_fault_test`,
   plus concurrency/cancel/shutdown regressions
   `uring_progress_race_test`, `uring_external_loop_test`,
   `uring_request_core_integration_test`, `shutdown_lifecycle_test`,
   `shutdown_lifecycle_uring_test`, `submit_concurrency_uring_test`.
   Outputs captured under `raw-diag/equiv/{baseline,c1,c2}/` and diffed.
4. W03 real-kernel smoke per arm (`SLUICE_595_R3_ARM={R,S,SNI,SNP,Q2,Q8,
   SHADOW,B}`, case `=W03-uring-4k-d16-r3d16cap`, warmup 0 / measured 1,
   stats off) — rc and per-arm counters captured at every stage.

## Results

### Deterministic oracle (the designated equivalence instrument)

| Stage | 18 scenarios rc=0 | full output vs baseline |
|---|---|---|
| baseline f3b25329 | 18/18 | — |
| C1 c55165a2 | 18/18 | byte-identical |
| C2 b191bb89 (adapted harness copy) | 18/18 | byte-identical |

The oracle asserts the scripted/real-ring transport event sequences
themselves (enter calls, SQE counts and opcodes, accepted identities,
untouched boundaries, stops, errors, notifications, control/cancel
interleavings), so identical per-scenario rc with zero FAIL lines is the
per-event identity the ask requires.

### In-repo suites

| Suite | baseline | C1 | C2 |
|---|---|---|---|
| r4_arm_smoke | rc=0, 57/57 PASS | rc=0, byte-identical | rc=0, 57/57 PASS; only the 8 intentionally corrected labels differ (diff recorded in `raw-diag/equiv/c2/r4_arm_smoke.out` vs c1) |
| uring_ready_batch_deterministic_test | rc=0 | rc=0, byte-identical | rc=0, byte-identical |
| uring_ready_batch_research_test | rc=0 | rc=0, byte-identical | rc=0, byte-identical |
| uring_ready_batch_fault_test | rc=0 | rc=0, byte-identical | rc=0, byte-identical |
| shutdown_lifecycle_test | rc=0 | rc=0 (binary not rebuilt: no bench include; one diagnostic poll-count line varies run to run on the same binary — proven by rerun) | rc=0 |
| uring_progress_race_test | rc=0 | rc=0, byte-identical | rc=0 |
| uring_external_loop_test | rc=0 | rc=0, byte-identical | rc=0 |
| uring_request_core_integration_test | rc=0 | rc=0 (one INFO line varies: real-kernel cancel terminal "success (8 bytes)" vs "canonical=1 native=0"; binary contains no bench code) | rc=0 |
| shutdown_lifecycle_uring_test | rc=0 | rc=0, byte-identical | rc=0 |
| submit_concurrency_uring_test | rc=0 | rc=0, byte-identical | rc=0 |

### W03 r3d16cap per arm

All arms rc=0 (`failing repetitions=0`) at all three stages. The
presentation/pressure counters of this real-kernel harness are NOT an
equivalence oracle: an A/B repeat study (5 interleaved reps per binary per
arm, `raw-diag/equiv/c1/w03_ab_repeat_study.log`) shows same-binary
run-to-run spread, e.g. S baseline 20→28 presentations and pressure 12→25
across reps, R 9→10; the R4 archived original-code smoke itself recorded a
third R value (presentations=11, `r4/logs/w03_smoke_R.log`). Distributions
overlap between baseline and C1/C1 binaries; counters are reported as
informational only, consistent with the ask's "real-kernel scheduling is not
required to be bit-identical".

## Constraints honored

- No change to quantum, suffix singles, refill, completion order, or any
  predicate evaluation; no eligibility caching across re-entries or
  NativeFileRef/borrow lifetimes; the bounded scan reads a strict subset of
  the members the full scan read (`op.file` never touched).
- `DecisionRecord.eligible_prefix` keeps its full-prefix meaning (trace-only)
  and the `r4_*` record/counter schema including `r4_pressure_decisions` is
  untouched, so historical R4 evidence joins keep their keys; arm tokens
  SNI/SNP unchanged.
- Main repo `master` clean at `271b81bf`; r4 tree read-only; worktree
  porcelain clean after both commits; no push, no PR, no labels.
