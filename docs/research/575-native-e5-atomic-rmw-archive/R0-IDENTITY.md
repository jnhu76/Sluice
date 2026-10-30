# R0 — Repository and Baseline Freeze

## Verdict

`NO_EARLY_STOP` — both target operations still exist in the current
authoritative master and are not compiler-eliminated a priori. Campaign
proceeds.

## Execution environment (verified 2026-10-24, all commands run directly on the host)

| Item | Value |
|---|---|
| Host | `E5` (static hostname), chassis `desktop`, model JUXIESHI X99-RS9 V5.0 |
| Native status | Native bare metal (no hypervisor below this OS; `Virtualization: VT-x` is a CPU capability only) |
| OS | Fedora Linux 44 (KDE Plasma Desktop Edition), kernel `7.2.11-200.fc44.x86_64` (PREEMPT_DYNAMIC) |
| CPU | Intel Xeon E5-2666 v3 @ 2.90GHz, 1 socket × 10 cores × 2 threads = 20 CPUs, Haswell microarchitecture, 1 NUMA node |
| Memory | 62 GiB RAM + 8 GiB swap (0 B swap used), 58 GiB available |
| Compiler | GCC 16.2.1 20260819 (Red Hat 16.2.1-2) via `/usr/lib64/ccache/g++` (ccache symlink; identity verified `g++ --version`) |
| xmake | v3.1.2+HEAD.5d21e9d |
| liburing | 2.13 (`/usr/lib64/liburing.so.2.13`, `pkg-config --modversion liburing` = 2.13) |
| perf | 7.2.11-200.fc44 (matches kernel), `perf_event_paranoid = 2` |
| Governor | `schedutil` (cpu0 + policy0), frequency range 1200–3500 MHz |
| tmpfs | `/dev/shm`, 32 GiB, 1% used — used for warm-cache benchmark files |
| Uptime/activity | Machine booted ~16:00 local; single user `jnhu` (this session, SSH from 192.168.31.244) plus a KDE session; no competing benchmark jobs; load average ≈ 0.6 falling at campaign start |

### PMU / counter capability (measured, not assumed)

- User-space hardware counters work: `instructions:u`, `cycles:u`,
  `branch-instructions:u`, `L1-dcache-loads:u`, `LLC-loads:u`,
  `dTLB-load-misses:u` all counted on a probe binary.
- `perf_event_paranoid = 2` means **kernel-side attribution is unavailable**
  (`context-switches:u` reads 0; kernel-resident events cannot be counted).
  Context-switch / syscall-side evidence is therefore taken from
  `getrusage(RUSAGE_SELF)` (`ru_nvcsw`, `ru_nivcsw`, `ru_utime`, `ru_stime`)
  reported by the benchmark harness itself, plus `/proc/<pid>/status`
  `voluntary_ctxt_switches` where useful. Kernel-side syscall census is
  recorded as `UNAVAILABLE` if a kernel-only view is required.
- Haswell general + `mem_load_uops_*` events are listed by `perf list`.
  Multiplexing observed on some event groups (probe showed 19–81% duty);
  per-metric multiplex fractions are recorded with each measurement, and any
  metric whose enabling fraction is < 90% is marked invalid for causal claims.
- The WSL2 GCC 15 preliminary compiler-IR step is `NOT_RUN_BY_OWNER_OVERRIDE`;
  its intended compiler-IR/assembly verification is executed on E5 GCC 16.2.1
  in R2.

## Repository identity

- Authoritative master at campaign start: `3df10dc44d84799cccb18d29f20149d2983137ec`
  (`origin/master`, merge of PR #583 — the v0.0.3 release).
- The issue was written against `5a75e3033ac7e525e1e4a04095a2edc4f76ed07d`,
  which is an ancestor of the new master. The 4 newer commits touch only
  `CHANGELOG.md`, `docs/` and `tests/external_loop_stop_order_test.cpp` —
  `git diff 5a75e303..origin/master -- src/async include/sluice/async` is
  empty, so the target code is identical between both SHAs.
- Prior campaign changes verified present in the baseline: H2b (owed-count /
  delivery-scan skip), E1, DP-1 and DP-3A (reserve-reinit) are all in history
  (`git log` evidence: `5956624d`, `1fb4c9c2`, `d3418718`, `dc33575d`, …).
- Target counters still present (grep over tracked sources):
  - `syscall_count_`: `src/async/threadpool_backend.cpp`,
    `include/sluice/async/detail/threadpool_backend.hpp`,
    `src/async/threadpool_test_seams.hpp`, and three test files.
  - `submit_flushes_`: `src/async/uring_transport.cpp`,
    `include/sluice/async/detail/uring_backend.hpp`,
    `src/async/uring_test_seams.hpp`, and two test files.
- Owner checkout `/home/jnhu/Source/Sluice` is untouched (clean tree at
  `5a75e303`, behind origin by the 4 release commits; left exactly as found).
- Research worktree: `/tmp/sluice-575/base`, branch
  `research/575-native-e5-atomic-rmw`, HEAD `3df10dc4`. All builds,
  patches and evidence live under this worktree; evidence documents are
  committed on the research branch only.

## Build configuration (frozen)

- `xmake f -m release --liburing=y` on Fedora/GCC 16.2.1.
- Production TU flags (from `compile_commands.json`, e.g. the `sluice_async`
  member `src/async/threadpool_backend.cpp`):
  `-c -m64 -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Werror -O3 -std=c++20 -I include -D SLUICE_LIBURING_PROFILE=on -D SLUICE_HAS_LIBURING -D NDEBUG`
  (async library TUs additionally `-I src/async`). No LTO, no PGO, no
  sanitizer in release mode.
- Internal seam tests compile their own copy of the backend TUs with
  `-D SLUICE_ASYNC_INTERNAL_TESTING` (plus `SLUICE_HAS_LIBURING` where
  applicable) and never link the production `sluice_async` objects — the
  production build and the seam build never meet in one binary. This is the
  ODR-isolation property R3 relies on.
- Benchmark targets: `bench_w01_direct`, `bench_w02_pipeline`,
  `bench_w03_external_loop` (see `xmake/bench.lua`).

## Baseline binary identity (Release, first build)

```
f578f0476ed585c4f2bc92243b608620a6c628b153f8133538d83fce49b1740a  bench_w01_direct
7ea9df984f15991db0da9d321ad1fe0289c9cf662bc421fb4c62f68034558b12  bench_w02_pipeline
052befc2b56109d7908e5109c97af6981617d0f62afd3867df5584f9269f16e0  bench_w03_external_loop
f1419abe0c594bd513d10dc35495a43f59082571542780897219b75180cd3c7a  libsluice_async.a
```

(All under `build/linux/x86_64/release/`; regeneration:
`cd /tmp/sluice-575/base && xmake f -m release --liburing=y && xmake build bench_w01_direct bench_w02_pipeline bench_w03_external_loop`.)

## Experiment boundaries

- Allowed: isolated worktree `/tmp/sluice-575/base`, experimental patches,
  deterministic tests, native benchmarks, evidence documents on the research
  branch, research PR kept OPEN, evidence comments on #575/#563.
- Not allowed: master changes, product PR merge, public API/contract changes,
  CI changes, new runtime observability subsystem, silent optimization
  adoption, system configuration changes (governor, kernel, SMT, perf
  permissions), deleting prior experiment artifacts, killing unrelated
  processes.
- Historical issues #538/#541 and PRs #565/#569/#572/#573 are treated as
  provenance; their campaigns are not rerun. Their relevant conclusions are
  cited where they bear on #575 (notably: none of them certified or eliminated
  these two atomic RMWs).
