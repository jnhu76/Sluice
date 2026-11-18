# #621 P1 build and baseline receipt

- BASE: `5d94f233967c3aef993eb19ba9e3252c532b0951` (`git rev-parse HEAD`, clean tracked tree)
- Date: 2026-11-18
- Host: linux 7.3.3-200.fc44.x86_64 x86_64
- Toolchain: gcc (GCC) 16.2.1 20261029 (Red Hat 16.2.1-3); xmake v3.1.2+HEAD.5d21e9d; liburing 2.13 (`pkg-config --modversion liburing`; rpms `liburing-2.13-2.fc44.x86_64`, `liburing-devel-2.13-2.fc44.x86_64`)
- ccache: enabled (`ccache = true` in the inherited xmake config). "Clean build" here means a fresh, never-before-used build directory; the compiler cache was not disabled. `bench/build_profiles.sh` likewise does not disable it.
- Build root: `/tmp/621-p1-build/` (new; outside the repo). The in-repo `build/`, `build-bench-noring/`, `build-bench-ring/` directories were not used and their mtimes are unchanged (2026-11-12 21:41, 2026-11-18 01:23).

## Pre-existing `.xmake` state and restore

Before any command, `.xmake` was copied to `/tmp/621-p1-xmake-backup/`. The snapshot contained a pre-existing project-pointer drift: `.xmake/linux/x86_64/cache/project` held `projectdir = "/home/jnhu/sluice-e5-elim"` and `.xmake/linux/x86_64/cache/history` ended with `xmake -P /home/jnhu/sluice-e5-elim f -m release --liburing=y -y`. A plain `xmake` invocation in the repo therefore warned that the directory's `xmake.lua` was shadowed and failed (`Invalid option: --liburing=n`). Every command below therefore passed `-P /home/jnhu/Source/Sluice` explicitly.

At the end, `.xmake` was restored from the backup and verified:

```
$ diff -r /tmp/621-p1-xmake-backup .xmake
XMAKE_RESTORE_DIFF_CLEAN
$ sha256sum .xmake/linux/x86_64/xmake.conf
2a234c8022e128c2e38a9dcc89bbafc6e2eb397718863a0607eb4510dd4b82fc  .xmake/linux/x86_64/xmake.conf
```

The pre-restore hash of the same file was identical (`2a234c80...4b82fc`). No configuration drift from this session remains. The restored pointer to `/home/jnhu/sluice-e5-elim` is pre-existing and will shadow plain `xmake` again; future sessions must use `-P /home/jnhu/Source/Sluice`.

## Build directories and configuration

All four directories were created fresh under `/tmp/621-p1-build/`; none existed before this session. Config commands (logs under `docs/evidence/621-progress-wait-path-20261118/p1/logs/`):

| Build dir | Profile | Config command | Config exit | Build command | Build exit |
|---|---|---|---|---|---|
| `/tmp/621-p1-build/noring` | release, liburing=n | `xmake f -P /home/jnhu/Source/Sluice -m release -o /tmp/621-p1-build/noring --liburing=n -y` | 0 | `xmake build -P /home/jnhu/Source/Sluice -y bench_w01_direct bench_w02_pipeline bench_w03_external_loop` | 0 |
| `/tmp/621-p1-build/ring` | release, liburing=y | `xmake f -P /home/jnhu/Source/Sluice -m release -o /tmp/621-p1-build/ring --liburing=y -y` | 0 | `xmake build -P /home/jnhu/Source/Sluice -y bench_w01_direct bench_w02_pipeline bench_w03_external_loop` | 0 |
| `/tmp/621-p1-build/copy` | release, liburing=y | `xmake f -P /home/jnhu/Source/Sluice -m release -o /tmp/621-p1-build/copy --liburing=y -y` | 0 | `xmake build -P /home/jnhu/Source/Sluice -y sluice-copy` | 0 |
| `/tmp/621-p1-build/tests` | release, liburing=y | `xmake f -P /home/jnhu/Source/Sluice -m release -o /tmp/621-p1-build/tests --liburing=y -y` | 0 | `xmake test -P /home/jnhu/Source/Sluice -y` | `TEST_EXIT=0` |

Common inherited settings (observed in `.xmake/linux/x86_64/xmake.conf` while configured): `mode=release`, `plat=linux`, `arch=x86_64`, `kind=static`, `hardened=false`, `ccache=true`, `sanitizers=""`. `liburing` was `true` only for `ring`, `copy`, `tests`, and `false` for `noring`. No build directory was reused across a `--liburing` switch.

Logs: `logs/build-noring.log`, `logs/build-ring.log`, `logs/build-sluice-copy.log`, `logs/test-release-liburing.log`.

## Binaries and sha256

| Profile | Binary | sha256 |
|---|---|---|
| noring | `/tmp/621-p1-build/noring/linux/x86_64/release/bench_w01_direct` | `4d4a948ac27d8b309212f072208b69c466a5c4f235a0609cb93834e9aa746e9b` |
| noring | `/tmp/621-p1-build/noring/linux/x86_64/release/bench_w02_pipeline` | `edb76b108f01dd5c325a6ec5aab4319533d1a256b6e62af958ea09994793b426` |
| noring | `/tmp/621-p1-build/noring/linux/x86_64/release/bench_w03_external_loop` | `6c1d2a8faa73166949b0ebc1513b39d274f398eb9a4febe00e34a62c0bbee941` |
| ring | `/tmp/621-p1-build/ring/linux/x86_64/release/bench_w01_direct` | `4d4a948ac27d8b309212f072208b69c466a5c4f235a0609cb93834e9aa746e9b` |
| ring | `/tmp/621-p1-build/ring/linux/x86_64/release/bench_w02_pipeline` | `03dbef098d41e6d6a5280a46d46758ca7267326925ae95d1156ad8d8c3c6a896` |
| ring | `/tmp/621-p1-build/ring/linux/x86_64/release/bench_w03_external_loop` | `43054fcd59f7000553d34a2f5b2909ff7ae6dae78910d4f379a8e8b0129937c1` |
| copy | `/tmp/621-p1-build/copy/linux/x86_64/release/sluice-copy` | `db972b1946fcf2b282135ebdf600263b03492215b08846e867d856c6213fa0be` |

## Link-profile assertions (`readelf -d`, `logs/readelf-assertions.log`)

`NEEDED` for ring `bench_w02_pipeline` and `bench_w03_external_loop` includes `liburing.so.2`; for the noring builds it does not. `bench_w01_direct` has no `liburing.so` dependency in either profile.

```
ok: ring bench_w02_pipeline links liburing
ok: noring bench_w02_pipeline does not link liburing
ok: ring bench_w03_external_loop links liburing
ok: noring bench_w03_external_loop does not link liburing
ok: bench_w02_pipeline differs across profiles
ok: bench_w03_external_loop differs across profiles
ok: bench_w01_direct identical across profiles
```

`sluice-copy` (release, liburing=y) also has `NEEDED liburing.so.2` (`logs/sluice-copy-readelf.log`).

## Full release+liburing=y test suite

Command: `xmake test -P /home/jnhu/Source/Sluice -y` in `/tmp/621-p1-build/tests` after the release+liburing=y config above.

Actual summary line from `logs/test-release-liburing.log`:

```
100% tests passed, 0 test(s) failed out of 81, spent 4.012s
TEST_EXIT=0
```

- `testTotalN = 81` (parsed from `out of 81`, not from the percentage)
- failures: none; no test case name needed to be recorded
- The suite includes the #621-affected targets (`progress_source_drain_semantic_oracle_test`, `threadpool_owed_counter_parity_test`, `threadpool_progress_race_test`, `threadpool_progress_pass_bound_test`, `progress_notification_suppression_test`, `app_copy_pipeline_oracle_test`).

## Smoke runs (non-formal; not timing evidence)

All smoke runs used `warmup=1`, `measured=1` and the release+liburing=y ring binary.

### W-02 with the literal asked filter

```
SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=1 \
SLUICE_BENCH_CASE_FILTER='=W02-threadpool-4k-d16' \
SLUICE_BENCH_JSONL=/tmp/621-p1-smoke/smoke.jsonl \
SLUICE_BENCH_TMP_DIR=/tmp/621-p1-smoke \
/tmp/621-p1-build/ring/linux/x86_64/release/bench_w02_pipeline
SMOKE_EXIT=0
```

Result: CAL-oracle self-test passed; `W-02 complete, failing repetitions=0`. However, the exact filter `=W02-threadpool-4k-d16` selects **zero** benchmark cases: `bench_w02_pipeline.cpp:460-470` appends `-w<workers>` to every threadpool case id, so the frozen case is `W02-threadpool-4k-d16-w4`. The JSONL (`/tmp/621-p1-smoke/smoke.jsonl`) contains only the `CAL-oracle-self-test` record. This run proves the entry starts and the CAL oracle passes, but it does not exercise a benchmark case.

### W-02 with the actual frozen case id (added to close the gap above)

```
SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=1 \
SLUICE_BENCH_CASE_FILTER='=W02-threadpool-4k-d16-w4' \
SLUICE_BENCH_JSONL=/tmp/621-p1-smoke/smoke-w4.jsonl \
SLUICE_BENCH_TMP_DIR=/tmp/621-p1-smoke \
/tmp/621-p1-build/ring/linux/x86_64/release/bench_w02_pipeline
SMOKE_EXIT=0
```

Result: CAL oracle passed; one warmup and one measured record for `W02-threadpool-4k-d16-w4`, both `ok=true`, `ops=20000`, `bytes=81920000`; `failing repetitions=0`. Log: `logs/smoke-w02-case-w4.log`.

### W-03

```
SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=1 \
SLUICE_BENCH_CASE_FILTER='=W03-uring-4k-d4' \
SLUICE_BENCH_JSONL=/tmp/621-p1-smoke/w03.jsonl \
SLUICE_BENCH_TMP_DIR=/tmp/621-p1-smoke \
/tmp/621-p1-build/ring/linux/x86_64/release/bench_w03_external_loop
SMOKE_EXIT=0
```

Result: CAL oracle passed; one warmup and one measured `W03-uring-4k-d4` record, both `ok=true`, `ops=20000`, `notifications/passes > 0` (io_uring progress notification path active); `failing repetitions=0`. Log: `logs/smoke-w03.log`.

### sluice-copy

Source: 1 MiB (`dd if=/dev/urandom bs=4096 count=256`, sha256 `00b0f54618b481189181276d5b498ac4349eb20de46af581df93e33197b8b88b`).

```
/tmp/621-p1-build/copy/linux/x86_64/release/sluice-copy \
  --buffer-size 4096 --pipeline-depth 4 --workers 1 \
  /tmp/621-p1-smoke/src-1m.bin /tmp/621-p1-smoke/dst-1m.bin
SMOKE_EXIT=0
copied 1048576 bytes (read_ops=260 write_ops=256 short_writes=0 sync=none pipeline_depth=4 atomic=on)
```

Destination sha256 equals source sha256; `cmp` passed. Log: `logs/smoke-sluice-copy.log`.

## Deviations and risks

1. **W-02 literal filter selects no case.** As requested, the literal filter `=W02-threadpool-4k-d16` was run; it matched zero benchmark cases because threadpool case ids carry `-w<workers>` (`bench/bench_w02_pipeline.cpp:460-470`). Only the CAL oracle executed. An additional run with `=W02-threadpool-4k-d16-w4` was made and is the actual W-02 case-level smoke evidence. Formal P1 runs must use `W02-threadpool-4k-d16-w4` (or `-w1`/`-w8`) if the intended cell is the frozen threadpool 4k/d16 case.
2. **Pre-existing `.xmake` project-pointer drift.** A plain `xmake` in the repo targets `/home/jnhu/sluice-e5-elim`; all commands used `-P /home/jnhu/Source/Sluice`. `.xmake` was restored byte-for-byte at the end.
3. **ccache was active.** Builds used fresh directories but a warm compiler cache; this matches the repository's existing configuration and `bench/build_profiles.sh`, and does not change linked binaries, but a fully cold compile was not forced.
4. **Build directories live under `/tmp/621-p1-build/`.** They are not in-repo and not covered by the repo's `build/`/`build-bench-*` ignore rules; they are disposable. The in-repo `build/` and `build-bench-*` artifacts were not read, rebuilt, or modified.
5. **Smoke runs are not formal measurements.** No governor, IRQ affinity, frequency, or A/A checks were performed; the numbers in the smoke JSONL must not enter performance conclusions.
6. `docs/evidence/621-round2-R1/` and `docs/evidence/e5-fullpath-machine-work/` were not read, modified, moved, or committed.
