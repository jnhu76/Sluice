# #595 E5 round — build record

Worktree: `/home/jnhu/sluice-595-e5/worktree` (isolated; branch
`research-595-bounded-progress`, never pushed)
Base: `271b81bfc9d64eecc0266fe7066b9040525dd710` (== #622 anchor; master clean)
Research patch: `patch.diff` (sha256
`7be9d0145a8c268a62d9fa90338b7cda95cefc5f8ad2976e4706cb192fb1eaf0`), 6 files:
`src/async/uring_transport.cpp`, `src/async/uring_backend.cpp`,
`include/sluice/async/detail/uring_backend.hpp`, `bench/bench_w02_pipeline.cpp`,
`xmake/tests.lua`, new `tests/uring_595_bounded_progress_test.cpp`.

## Configure / build

```
xmake f -c -y --liburing=y -m release
xmake build bench_w02_pipeline bench_w03_external_loop uring_595_bounded_progress_test
```

Toolchain from `compile_commands.json`: `/usr/lib64/ccache/g++ -m64 -O3 -std=c++20
-Wall -Werror -fvisibility=hidden -fvisibility-inlines-hidden -DNDEBUG`
with `-DSLUICE_HAS_LIBURING` (liburing-devel 2.13-2) and, for the new test,
`-DSLUICE_ASYNC_INTERNAL_TESTING`. Compiler `g++ (GCC) 16.2.1 20260819`,
xmake `3.1.2+HEAD.5d21e9d`.

## Binary SHA256 (release, 2026-11-04 11:39 build)

```
32beabcd891801eb65dab1d6e2df72aa0b89a98a2c16a114d398527cb821589a  bench_w02_pipeline
1e548bc68ca44427ad06a8706689a0bbf45a7d9c339ba795b45b02c71653a642  bench_w03_external_loop
865de828f9b4bc95146caa15101921a28d7298247f8d2130d6d7be56db2b069a  uring_595_bounded_progress_test
```

Both bench programs are one binary serving both arms; the arm is selected at backend
construction from `SLUICE_595_ARM` (absent/`A` = frozen immediate path, `B` = bounded
hold). Same code layout, no binary-layout confound. Any rebuild invalidates these hashes
and requires re-recording them plus a fresh A/A calibration.

## Arm-selector smoke check (strace, not timing)

`strace -f -c -e trace=io_uring_enter`, `W02-uring-4k-d16`, warmup=0 measured=1:
A: 60 004 enters / 20 000 reads; B: 10 885 enters / 20 000 reads. The selector is live;
the mechanism ledger round (Step 4d) will record this properly per shape.

## Case ids used by the round

- W-02 main: `W02-uring-4k-d16`; D1: `W02-uring-4k-d1`; D2/D3 via
  `SLUICE_BENCH_595_DEPTHS=1` → `W02-uring-4k-d2`, `W02-uring-4k-d3`.
- 64 KiB: `W02-uring-64k-d16`; threadpool no-enter control: `W02-threadpool-4k-d16-w4`;
  saturation: `W02-uring-4k-d16-saturate`.
- W-03 split owner: `W03-uring-4k-d4`, `W03-uring-4k-d16`, `W03-uring-4k-d64`
  (threadpool companions `W03-threadpool-4k-d*-w4`).
