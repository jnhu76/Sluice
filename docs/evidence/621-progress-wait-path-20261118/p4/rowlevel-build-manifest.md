# #621 P4 row-level DWARF build manifest

- BASE: `5d94f233967c3aef993eb19ba9e3252c532b0951` (`git rev-parse HEAD`; tracked tree clean before and after this session)
- Date: 2026-11-18
- Host: linux 7.3.3-200.fc44.x86_64 x86_64
- Toolchain: gcc (GCC) 16.2.1 20261029 (Red Hat 16.2.1-3); xmake v3.1.2+HEAD.5d21e9d; liburing 2.13 (`pkg-config --modversion liburing`)
- Purpose: W3 row-level attribution only. These binaries are **not** for A/B or timing comparison.
- Build root: `/tmp/621-p4-build/` (new, outside the repo). All evidence logs are under `docs/evidence/621-progress-wait-path-20261118/p4/logs/`.

## 1. Mode determination: `releasedbg` is not wired in this project

`xmake show -l rules` lists `mode.releasedbg`, and `xmake f -m releasedbg ...` exits 0. However `xmake.lua:3` registers only `mode.debug`, `mode.release` and `mode.valgrind`:

```lua
add_rules("mode.debug", "mode.release", "mode.valgrind")
```

so the built-in `mode.releasedbg` rule never attaches to any target. The verbose compile line from that experiment (`logs/build-ring-releasedbg.log:4`) is:

```
/usr/lib64/ccache/g++ -c -m64 -Wall -Werror -std=c++20 -I/home/jnhu/Source/Sluice/bench -I/home/jnhu/Source/Sluice/include -DSLUICE_LIBURING_PROFILE=on -DSLUICE_HAS_LIBURING -o /tmp/621-p4-build/ring/.objs/bench_w02_pipeline/linux/x86_64/releasedbg/bench/bench_w02_pipeline.cpp.o bench/bench_w02_pipeline.cpp
```

There is **no `-O3`, no `-g`, no `-DNDEBUG`**; the mode name alone does not produce a debug build here. This was confirmed before choosing the alternative, and no product file was changed.

Chosen mode: `-m release` plus explicit `-g` (the alternative named in the task), with the release strip policy disabled:

```
-m release --cxflags=-g --ldflags=-g --policies=build.release.strip:n
```

The strip policy is required: with plain `-m release`, xmake's release rule adds `-s` at link (evidence `logs/build-ring-release-g.log:41,43`), and the resulting file was reported by `file` as `stripped` with no `.debug_*` sections. With `build.release.strip:n` the link line has no `-s` (`logs/build-ring-release-g-nostrip.log:41,43`).

## 2. Build directories and commands

All directories were created fresh under `/tmp/621-p4-build/`; the in-repo `build/`, `build-bench-noring/` and `build-bench-ring/` directories were not used or modified.

| Build dir | Config command | Build command | Exit |
|---|---|---|---|
| `/tmp/621-p4-build/ring` | `xmake f -P /home/jnhu/Source/Sluice -m release -o /tmp/621-p4-build/ring --liburing=y --cxflags=-g --ldflags=-g --policies=build.release.strip:n -y` | `xmake build -P /home/jnhu/Source/Sluice -y -v bench_w02_pipeline bench_w03_external_loop` | 0 |
| `/tmp/621-p4-build/noring` | `xmake f -P /home/jnhu/Source/Sluice -m release -o /tmp/621-p4-build/noring --liburing=n --cxflags=-g --ldflags=-g --policies=build.release.strip:n -y` | `xmake build -P /home/jnhu/Source/Sluice -y -v bench_w02_pipeline bench_w03_external_loop` | 0 |
| `/tmp/621-p4-build/copy` | `xmake f -P /home/jnhu/Source/Sluice -m release -o /tmp/621-p4-build/copy --liburing=y --cxflags=-g --ldflags=-g --policies=build.release.strip:n -y` | `xmake build -P /home/jnhu/Source/Sluice -y -v sluice-copy` | 0 |

Config logs: `logs/config-ring-release-g-nostrip.log`, `logs/config-noring-release-g-nostrip.log`, `logs/config-copy-release-g-nostrip.log`. Build logs: `logs/build-ring-release-g-nostrip.log`, `logs/build-noring-release-g-nostrip.log`, `logs/build-copy-release-g-nostrip.log`.

Targets for reference: `bench_w02_pipeline` (`xmake/bench.lua:18`), `bench_w03_external_loop` (`xmake/bench.lua:26`), `sluice-copy` (`xmake/apps.lua:19`). The liburing switch is `xmake/uring.lua:36`; `SLUICE_LIBURING_PROFILE` is defined at `xmake/uring.lua:85`.

## 3. Actual flags (from the verbose build logs)

Compile line, ring (`logs/build-ring-release-g-nostrip.log:7`):

```
/usr/lib64/ccache/g++ -c -m64 -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Werror -O3 -std=c++20 -I/home/jnhu/Source/Sluice/bench -I/home/jnhu/Source/Sluice/include -DSLUICE_LIBURING_PROFILE=on -DSLUICE_HAS_LIBURING -DNDEBUG -g -o .../bench_w02_pipeline.cpp.o bench/bench_w02_pipeline.cpp
```

Compile line, noring: same flags with `-DSLUICE_LIBURING_PROFILE=off` and **no** `-DSLUICE_HAS_LIBURING`.

Link line, ring (`logs/build-ring-release-g-nostrip.log:41`):

```
/usr/lib64/ccache/g++ -o .../bench_w02_pipeline ... -m64 -L... -lsluice_async -luring -lsluice_core -g
```

Link line, noring (`logs/build-noring-release-g-nostrip.log:59`): same without `-luring`. Link line, copy (`logs/build-copy-release-g-nostrip.log:47`): `-lsluice_async -luring -lsluice_core -g`.

Summary:

- optimization: `-O3` (same `optimize=fastest` as the P1 `-m release` build)
- asserts: `-DNDEBUG` (same as P1 release)
- debug: `-g` at compile and link; no `-s` at link (`build.release.strip:n`)
- visibility: `-fvisibility=hidden -fvisibility-inlines-hidden` (from release-mode `symbols=hidden`, as in P1 release)
- ring profile: `SLUICE_LIBURING_PROFILE=on`, `SLUICE_HAS_LIBURING`, `-luring`; noring: `SLUICE_LIBURING_PROFILE=off`, no `SLUICE_HAS_LIBURING`, no `-luring`

The DWARF itself embeds the flags (`DW_AT_producer`): `-g -O3 -std=c++20 -fvisibility=hidden -fvisibility-inlines-hidden` (`logs/dwarf-producer-addr2line.log`).

## 4. Binaries, sha256, BuildID

| Profile | Binary | Bytes | BuildID (sha1) | sha256 |
|---|---|---|---|---|
| ring | `/tmp/621-p4-build/ring/linux/x86_64/release/bench_w02_pipeline` | 3811512 | `0dcc8b3a3302e5e356db0edfe538fed4954f0d5d` | `e65a5766c08af7657ba2fe4d7957b2ba9d8e875fe29fc0b69edca316ca2eb9b8` |
| ring | `/tmp/621-p4-build/ring/linux/x86_64/release/bench_w03_external_loop` | 3444656 | `5fd4410c904635f02ebe964f9b0232097542cb93` | `5bc33f861c1a7c6f6bfd498bb6a46af1dd2a7815b332aaae02b91d9d6b26ece4` |
| noring | `/tmp/621-p4-build/noring/linux/x86_64/release/bench_w02_pipeline` | 3104352 | `4f206d78c987b84134f207cd33f98e20b16de1af` | `9f282a81d0bda6c5bda29467a02f406a1e8b6ef995ae102ab1f8de6bfd52e080` |
| noring | `/tmp/621-p4-build/noring/linux/x86_64/release/bench_w03_external_loop` | 2737472 | `32e995e365b8ee729be0cc549dbfe5757471a5ef` | `2289999fd80ed21cb0211311ac2a4140fb203cac62e69537c7f6a6dd93034357` |
| copy | `/tmp/621-p4-build/copy/linux/x86_64/release/sluice-copy` | 1982176 | `9b02d231149fc1cabef4a610aa35e7dd4850d01b` | `959f8090aaeabf863208e9697a9fc0e76d53cc422843026f7d89d604ee593c54` |

Full assertion output: `logs/verify-dwarf-link-sha.log`.

## 5. DWARF assertions (ran)

- `file` reports each of the five binaries as `with debug_info, not stripped`.
- `readelf -S` shows `.debug_info`, `.debug_abbrev`, `.debug_line` and `.debug_line_str` in all five binaries (e.g. ring W-02 `.debug_info` at `00036b98`, `.debug_line` at `001fa32f`).
- `addr2line -e <ring W-02> -f -C $(nm ... | awk '$3=="main"{print $1}')` resolves `main` to `/home/jnhu/Source/Sluice/bench/bench_w02_pipeline.cpp:605`; ring W-03 resolves to `bench_w03_external_loop.cpp:609`; `sluice-copy` resolves `main` to `apps/sluice-copy/main.cpp:45`.
- Library code resolves too, e.g. `sluice::File::open` -> `src/file_resource.cpp:68` and `sluice::async::RequestScope::submit_sync_data` -> `src/async/request_scope.cpp:189` (`logs/dwarf-library-symbols.log`).
- `readelf --debug-dump=decodedline` emits line-table rows (e.g. `main.cpp:37` at `0x402180`).

Contrast: the P1 release binaries are `stripped` and contain no `.debug_*` sections (`logs/p1-release-contrast.log`).

## 6. Link-profile assertions (ran)

`readelf -d` `NEEDED` (`logs/verify-dwarf-link-sha.log`):

- ring `bench_w02_pipeline` and `bench_w03_external_loop`: `liburing.so.2` present.
- noring `bench_w02_pipeline` and `bench_w03_external_loop`: `liburing.so.2` absent.
- `sluice-copy` (liburing=y): `liburing.so.2` present.

All also depend on `libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libc.so.6`.

## 7. Code-generation difference vs the P1 release binaries

P1 artifacts were still present under `/tmp/621-p1-build/`; their sha256 matches `p1/build-manifest.md:44-50` exactly (`logs/text-compare-vs-p1.log`).

For all five pairs, `.text` has the same VMA and size, and the `.text`, `.rodata` and `.data` section bytes are identical (`objcopy --dump-section` + `cmp`):

| Pair | `.text` VMA / size | `.text` sha256 |
|---|---|---|
| ring W-02 | `0x400940` / `0x28d30` | `436f8e21fc3cffe1d79755c20ad8da0841c6ecad19b378608b54ffa6affbf4df` |
| ring W-03 | `0x400940` / `0x23b30` | `efbe1dd6e8afa4ed0c6c0780b16b2a0741ecf1da31a9004002acff33f9bc2b6c` |
| noring W-02 | `0x4008c0` / `0x22390` | `43f7f875741642bfef1a9646dfd995739a557dc59c8e7a4cbdbdd2b43b3ad261` |
| noring W-03 | `0x4008c0` / `0x1d130` | `4ac68b7b4a7f2c73e8215753bb15fc8d6e10424eb2180657adbafb227b4c1a96` |
| copy | `0x400780` / `0x108f0` | `4b3ae86a637bc17168e22cf6a40d9c928fb197006108898fb45c1d037e1dba6e` |

Interpretation:

- `-g` did not change the optimized machine code for this toolchain and input: the allocatable code/data sections are byte-identical to the stripped P1 release artifacts, and `.text` load addresses are unchanged.
- The remaining differences are non-alloc `.debug_*` sections, the `.symtab` symbol table, the ELF BuildID (GNU ld hashes the debug sections too) and the file size (2-3.8 MB vs ~110-225 KB stripped).
- This equality is evidence for this build only, not a general guarantee that `-g` never perturbs layout. The binary is **attribution-only**: do not use it for A/B or timing comparisons.

## 8. Smoke runs (non-formal; not timing evidence)

All bench smokes used `warmup=1`, `measured=1`, the ring DWARF binary, and case filters that match real cases (`bench/bench_w02_pipeline.cpp:460-470` constructs `-w<workers>` into threadpool ids; `bench/bench_w03_external_loop.cpp:473` constructs the W-03 id).

W-02 (`logs/smoke-w02-w03.log`):

```
SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=1 \
SLUICE_BENCH_CASE_FILTER='=W02-threadpool-4k-d16-w4' \
SLUICE_BENCH_JSONL=/tmp/621-p4-smoke/w02.jsonl \
SLUICE_BENCH_TMP_DIR=/tmp/621-p4-smoke \
/tmp/621-p4-build/ring/linux/x86_64/release/bench_w02_pipeline
```

`W02_EXIT=0`; CAL-oracle self-test passed; one warmup and one measured record for `W02-threadpool-4k-d16-w4`, both `ok=true`, `ops=20000`, `bytes=81920000`; `failing repetitions=0`.

W-03 (`logs/smoke-w02-w03.log`):

```
SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=1 \
SLUICE_BENCH_CASE_FILTER='=W03-uring-4k-d4' \
SLUICE_BENCH_JSONL=/tmp/621-p4-smoke/w03.jsonl \
SLUICE_BENCH_TMP_DIR=/tmp/621-p4-smoke \
/tmp/621-p4-build/ring/linux/x86_64/release/bench_w03_external_loop
```

`W03_EXIT=0`; CAL-oracle self-test passed; one warmup and one measured `W03-uring-4k-d4` record, both `ok=true`, `ops=20000`, notifications/passes 8666 and 8925; `failing repetitions=0`.

sluice-copy (`logs/smoke-sluice-copy.log`), source 1 MiB from `dd if=/dev/urandom bs=4096 count=256`:

```
/tmp/621-p4-build/copy/linux/x86_64/release/sluice-copy \
  --buffer-size 4096 --pipeline-depth 4 --workers 1 \
  /tmp/621-p4-smoke/src-1m.bin /tmp/621-p4-smoke/dst-1m.bin
```

`COPY_EXIT=0`; output `copied 1048576 bytes (read_ops=260 write_ops=256 short_writes=0 sync=none pipeline_depth=4 atomic=on)`; source and destination sha256 both `fc2b9cc95b513d0e4980474b1b516f8bdb9ad5fbd1c4d6902a95c04789db2a34`; `cmp` passed.

These smoke numbers must not enter performance conclusions.

## 9. `.xmake` and `compile_commands.json` hygiene

- Before the first command, `.xmake` was copied to `/tmp/621-p4-xmake-backup/` and `compile_commands.json` to `/tmp/621-p4-repo-backup/`.
- The pre-existing project-pointer drift was present again (`.xmake/linux/x86_64/cache/project` points at `/home/jnhu/sluice-e5-elim`), so every xmake command used `-P /home/jnhu/Source/Sluice`. The restore hash of `.xmake/linux/x86_64/xmake.conf` is `2a234c8022e128c2e38a9dcc89bbafc6e2eb397718863a0607eb4510dd4b82fc`, identical to the pre-session value recorded in `p1/build-manifest.md:20`.
- The compile-commands autoupdate plugin rewrote the ignored root `compile_commands.json` during each build; it was restored from backup.
- Final restore verification (`logs/restore-xmake-compile-commands.log`): `diff -r` clean (`XMAKE_RESTORE_DIFF_CLEAN`); `compile_commands.json` sha256 back to `91cb53cd829c87c49afac9eebf458b89c113386b343887ad66baf4af505866f1`; `git status` shows only the three pre-existing untracked evidence directories.

## 10. Risks and deviations

1. `-m releasedbg` is accepted but inert because `mode.releasedbg` is not attached in `xmake.lua`; the build uses `-m release --cxflags=-g --ldflags=-g --policies=build.release.strip:n` instead. No product file was modified to make `releasedbg` work.
2. Without `build.release.strip:n`, release links with `-s` and discards the DWARF. The final binaries were verified to have DWARF after disabling the policy.
3. `--cxflags=-g` / `--ldflags=-g` are project-wide config flags; they applied to all TUs of the three targets (library and application), as the verbose logs show.
4. ccache was active. Build directories were fresh, but the compiler cache was warm; this matches the repository's existing configuration and does not change the linked code, but a fully cold compile was not forced.
5. Build directories live under `/tmp/621-p4-build/` and are disposable; they are not repository artifacts.
6. The binaries are unstripped and notationally different from P1 release artifacts (BuildID, file size). They must not be used for A/B or timing comparisons.
7. `docs/evidence/621-round2-R1/` and `docs/evidence/e5-fullpath-machine-work/` were not read, modified, moved or committed.
