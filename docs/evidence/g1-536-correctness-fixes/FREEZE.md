# #536 correctness-fix evidence — frozen configuration

Scope: the correctness fixes applied on top of the frozen G1 adoption set
(C9-A + C1 + C2), on branch `codex/403-g1-native-optimization`.

This directory is additive. It never rewrites, re-derives or supersedes
`docs/evidence/g1-403-native-optimization/`; that directory's raw JSONLs,
reports and receipts stay as recorded at their own SHAs.

## SHAs

| Item | Value |
|---|---|
| `master` base (repo state at mission start) | `10aaf58b` (PR #533 + #534 merged) |
| Baseline freeze (G0) | `d0673a8b` |
| Adopted G1 set (C9-A, C1, C2) | `001512d4`, `4c86d3fe`, `0891be17` |
| PR #536 head before these fixes | `54631b5f` |
| Fix commit 1 (pass bound + preallocated snapshot stores) | `b88750e3` |
| Fix commit 2 (suppression-test temp-file RAII) | `64e3241b` (= HEAD of this slice) |
| Frozen G1 report time | `docs/evidence/g1-403-native-optimization/ab/final/` |

## What changed, in code

`git diff --stat 54631b5f 64e3241b`:

```text
 include/sluice/async/threadpool_backend.hpp      |   8 +-
 src/async/threadpool_backend.cpp                 |  61 +++--
 tests/progress_notification_suppression_test.cpp |  53 +++-
 tests/threadpool_progress_pass_bound_test.cpp    | 316 +++++++++++++++++++++++
 xmake/tests.lua                                  |   2 +
```

The formal assets are byte-identical to the G1 run head:

```text
$ git diff --stat 0891be17 HEAD -- formal/ scripts/verify_tla.sh scripts/verify_formal.sh
(empty)
$ sha256sum formal/tla/RequestCore.tla formal/tla/RequestCore.cfg formal/tla/RequestCoreLive.cfg scripts/verify_tla.sh
4ad6dbe5cd4af19e12a846f06818622e099a37899474206fdd6984e445725aa1  formal/tla/RequestCore.tla
82c31d615fdeb73d947342b2a8990072c738069cfa2e1363455a53314f7f8edf  formal/tla/RequestCore.cfg
062c8c3e6b873a5bfd518856952ceacd942dab2f08bf9e6145c34be4f771f09d  formal/tla/RequestCoreLive.cfg
2e38fabae8a4224e98b34124675c325f5fd8353f4ce9661c9ced7bae3d90a756  scripts/verify_tla.sh
$ sha256sum /home/jnhu/.cache/sluice/formal/tla2tools.jar
936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88
```

All four file hashes equal the #524 correspondence-after table and the jar
hash equals the pinned `936a2620…`; the model text under check is therefore
the same text whose PASS receipts are recorded for #524 and G1.

## Host (E5, native Linux) — unchanged from the G1 admission record

| Item | Value |
|---|---|
| kernel | `7.2.9-201.fc44.x86_64` |
| CPU | Intel Xeon E5-2666 v3, 10 cores / 20 logical CPUs, 1 NUMA node |
| memory | 62 GiB |
| governor / driver | `schedutil` / `intel_pstate`, `no_turbo=0` |
| toolchain | GCC 16.2.1, xmake 3.0.9, liburing 2.13, OpenJDK 25.0.4.1 |
| gate state at run start | idle (load average 0.22 / 0.11 / 0.37; `%Cpu(s) 99.9 id`) |

No sysctl, governor or privilege setting was changed for any run in this
directory. Results are compared only with measurements taken on this host; the
WSL2 G0 v2 figures are never mixed in.

## Binaries (sha256, release profile)

Built with the frozen profile commands (same targets, same flags, separate
output directories so no frozen build directory is overwritten):

```bash
xmake f -m release -o build-bench-fixhead-ring   --liburing=y -y
xmake f -m release -o build-bench-fixhead-noring --liburing=n -y
xmake build -y bench_w01_direct bench_w02_pipeline bench_w03_external_loop   # per target
```

| Binary | frozen B0 (`build-bench-ring`) | frozen arm (`build-bench-armC-ring`) | fixed head (`build-bench-fixhead-ring`) |
|---|---|---|---|
| `bench_w02_pipeline` | `29b2eb93…503cc5` | `b5c687ea…1fed7f` | `b0ae53a6…4935a2` |
| `bench_w03_external_loop` | `1c5533f9…f95e32` | `3a8b18fc…2f294e3a` | `7b3c15cd…814830f0` |
| `bench_w01_direct` | `9cb468d3…60d6f2` | `9cb468d3…60d6f2` | `9cb468d3…60d6f2` |

`bench_w01_direct` is byte-identical across all three: W-01 links `sluice_core`
only, so neither the G1 optimizations nor these fixes can reach it. That
identity is also the session's null-effect control (session 2 reports a paired
ratio of 1.0290 for a case whose two binaries are the same bytes).

Profile linkage was asserted from the binaries, not from directory names:
`bench_w02_pipeline` in `build-bench-fixhead-ring` depends on `liburing.so`
(1 match in `readelf -d`) and in `build-bench-fixhead-noring` does not (0).

`build-bench-ring` still holds the frozen B0 binaries: its three hashes equal
the `b0_*` hashes recorded in
`docs/evidence/g1-403-native-optimization/ab/final/run.log`, so the frozen
baseline was reused rather than rebuilt. `build-bench-armC-ring` still holds
the frozen pre-fix arm binaries (`b5c687ea…`, `3a8b18fc…`), so the incremental
comparison uses the recorded artifact, not a recompilation of it.

## Protocol (frozen from G1, not re-specified here)

`ab_run.sh` from `docs/evidence/g1-403-native-optimization/tools/` — the same
runner, unmodified:

* one measured repetition per side per repetition index, interleaved; even
  repetitions run B0 first, odd repetitions run the arm first;
* warmup = 1, measured = 1 per invocation, per-side JSONL output;
* per-stage validation rejects a repetition whose measured record is missing
  or whose exit code is non-zero (`failures=` counter in each `run.log`);
* each session writes its own `run.log` provenance header (start time, git
  head, dirty-file count, both directories, per-binary hashes, uptime).

Repetition counts are the preregistered G1 ones, reused verbatim:
15 for the high-variance ThreadPool cases (W02 w4, W02 w1, W03 w4,
W02-uring), 7 for saturate/W03-uring/W01.

Spec files:

| File | Cases | Note |
|---|---|---|
| `spec-ring.txt` | the frozen `ab/final/spec.txt`, copied unchanged | ring profile, all 7 G1 cases |
| `spec-noring.txt` | the 4 ThreadPool cases only | no-ring profile: the build carries no io_uring backend, so the uring cases are not applicable and are excluded rather than reported as zero |

## Sessions in this directory

| Session | Left side | Right side | Question |
|---|---|---|---|
| `ab-s1-b0-vs-fixed/` | frozen B0 (`build-bench-ring`) | fixed head `64e3241b` | are the adopted G1 gains retained after the correctness fixes? |
| `ab-s2-prefix-vs-fixed/` | frozen pre-fix arm (`build-bench-armC-ring`, hash `b5c687ea…`) | fixed head `64e3241b` | what did the fixes themselves cost or gain? |
| `ab-s3-noring/` | frozen B0 no-ring (`build-bench-noring`) | fixed head no-ring | same question on the no-ring profile |

Sessions ran one at a time, never concurrently, and never overlapped with a
TLC run. Every raw JSONL, every `run.log`, and the generated `AB.md` are kept
as produced; no failed run, no outlier and no negative control was deleted.
