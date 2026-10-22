# E1 Native-Linux Validation — Identity (Gate R0)

Master was re-fetched at round start: remote `origin/master` = local master =
`381756370980f75e4beaf47310db4b8b9a99053d` (the SHA frozen by the protocol;
no drift, no intervening production change).

## Arms

| Arm | Worktree | SHA | Content |
|---|---|---|---|
| BASE | `~/sluice-e1-e5/base` | `381756370980f75e4beaf47310db4b8b9a99053d` | master as merged (includes PR #565 H2b owed-count productization and PR #566 MW-HW-1 archive) |
| E1 | `~/sluice-e1-e5/candidate` | `2caead59f9317665510af72968439f1252bc5bb9` | BASE + exactly `docs/research/mw-hw-1/patches/e1-pending-control-skip.patch`, committed on isolated branch `research/native-linux-e1-pending-control-skip` |

- `E1_PATCH_SHA256` = `ea217da4252a885f73889ba28b569f848bf050f8913b61fa69fa8bd1b385695f`
- Patch scope verified: only `include/sluice/async/detail/uring_backend.hpp`,
  `src/async/uring_test_seams.hpp`, `src/async/uring_transport.cpp`,
  `xmake/tests.lua`, new `tests/uring_pending_control_parity_test.cpp`.
  No H1/E2/E3/eventfd/ThreadPool owed-count content is mixed in; E1 fully
  inherits H2b (H2b merged in master lineage at `8e2aa5ab`).
- Both worktrees clean (`git status --porcelain` empty; enforced again by the
  runner at campaign start).

## Builds

Both arms: clean `rm -rf build`, then
`xmake f -m release --liburing=y` and per-target `xmake build sluice_core`
then `xmake build sluice_async` (`sluice_async` is `set_default(false)`;
building it by name is mandatory).

| Artifact | SHA256 |
|---|---|
| BASE `libsluice_async.a` | `76e61047e18e5672bcd1e358f0199e08a006ca2678e5ecbd872d8e3898199afa` |
| E1 `libsluice_async.a` | `bb6ccd6ccc6c9ed06e231e966e5f21129e86126db9e8e6b3b5eb021b3e4fd351` |
| BASE `libsluice_core.a` | `489104b5819cdaca251471d5e0b10c20051071bca88dcb7bfc198f1336c99c5a` |
| E1 `libsluice_core.a` | `489104b5819cdaca251471d5e0b10c20051071bca88dcb7bfc198f1336c99c5a` |

Async libs DIFFER (E1 touches only the async layer), core libs are byte-identical
(E1 touches no core file). The BASE async hash reproduces the H2b E5 receipt
(`76e61047…`), so the two rounds' BASE arms are the same binary artifact.

Harness binaries (`-std=c++20 -O3 -DNDEBUG`, linked per arm):

| Binary | SHA256 |
|---|---|
| `~/sluice-e1-e5/results/pipeline_ab_e1_base` | `4266b54d5a2a557d5c0c523126b1d4acdd1f24b23df67c1cbbc4e747418560d4` |
| `~/sluice-e1-e5/results/pipeline_ab_e1_candidate` | `72e9807b31f5c3ae0ab43bd8bb535f97b18abac72153c656abc2c337c29f2b30` |

Same shape for `external_loop_ab_e1_{base,candidate}` and
`pending_control_pressure_{base,candidate}` plus the seam-build
`pending_control_pressure_probe` (E1-only, `SLUICE_ASYNC_INTERNAL_TESTING`).

Correction (Review B P2-3): `results-w03/arm-identity.txt` and
`results-p5/arm-identity.txt` are stale verbatim copies of the W-02 file;
their pipeline-binary hashes are NOT receipts for those campaigns. The
W-03/P5 runners verify worktree SHA pins and clean trees at start and record
runner/harness-source SHAs in their provenance files, but do not hash the
harness binaries themselves. The hashes below were recorded at review time
from the binaries as they stand on disk (mtimes equal to the campaign start
timestamps); they are a post-hoc record, not a runner-enforced pin:

| Binary (post-hoc record) | SHA256 |
|---|---|
| `results-w03/external_loop_ab_e1_base` | `4ed27a8b41345d0ac9aeb1a93ce657b51084f2a97f721ce32f24c3daa16ffefd` |
| `results-w03/external_loop_ab_e1_candidate` | `c320153d4e1b29027f1404278b0b358967434c5f032707d0ebdeffb38a64c487` |
| `results-p5/pending_control_pressure_base` | `dcecb9b66579a81fabd96efe4c53e2d306a7f2a901418b87f4f1b3ecd4f73248` |
| `results-p5/pending_control_pressure_candidate` | `7ec82cbca1f5817c8e16ac8d8dfab9cfca0c9c02cd673b0261d4781dcafdf7e7` |

## Frozen framework (research-only)

| File | SHA256 (at formal rounds) |
|---|---|
| `scripts/run_e1_e5.sh` | `2de6822577f193285f3312ce09f10043968686b37a444c55a16c94dab121176e` |
| `scripts/e1_verdict.py` | `420e573a148d25bb4118406673d3e9f418371a0fdf007f33d95bd81e3dd3da0e` |
| `scripts/e1_verdict_selftest.py` | `cbdaaa7789ae85bedbf7538341d7f7b84e493cd21841ae8773a416554463e8da` |
| `scripts/pipeline_ab_e1.cpp` | `9fb07bc33d0f89e22bad91230621cc7988953d5ec7f3abe55d62b5455b3eae72` |
| `scripts/bench_common.hpp` | `0d3f63a5a56e85cf7a01b4cccf791056f3b0bcdd1fa385450bd5dec6bf0c0e84` |

W-03 and P>0 runner SHAs are recorded in their own provenance files
(`results-w03/provenance-w03.txt`, `results-p5/provenance-p5.txt`). The
`run_e1_e5.sh` SHA above matches both the file on disk and the campaign's
own `results/provenance.txt` `runner_sha256` recorded at campaign start
(Review B P2-2: an earlier draft of this table carried a stale pre-campaign
draft hash, `b3063d60…`). The
reducer selftest (14 counterfactual cases) passed 14/14 before the formal
verdict round.

## Campaign receipts

| Campaign | Raw evidence | sha256 | Status |
|---|---|---|---|
| W-02 (15 reps × 11 cfg × 2 arms) | `results/ab.jsonl` (990 rows) | in `results/provenance.txt` (`ab_jsonl_sha256`) | COMPLETE |
| W-03 (15 reps × 3 cfg × 2 arms + 5+5 verify rows) | `results-w03/w03.jsonl`, `w03-verify.jsonl`, combined `w03-combined.jsonl` (`9ed02432…`) | in `results-w03/provenance-w03.txt` | COMPLETE |
| P>0 (15 reps × 2 arms + probe) | `results-p5/p5.jsonl`, `p5-probe.jsonl` | in `results-p5/provenance-p5.txt` | COMPLETE |
