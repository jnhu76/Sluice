# DP-0 compiler inventory (R1) — frozen toolchain and build config

Recorded from the actual machine and build outputs, not from project docs.
Honesty note: this study ran on the **WSL2 workstation** as instructed; the
Fedora 44 / GCC 16.2.1 host referenced in earlier task correspondence was not
used. All conclusions are statements about this toolchain.

## Host and toolchain

| item | value |
|---|---|
| kernel | Linux 6.18.54.1-microsoft-standard-WSL2 x86_64 |
| compiler | `/usr/bin/g++` (Ubuntu 15.2.0-16ubuntu1) 15.2.0 |
| build system | xmake (`xmake f -m release -o build-ring --liburing=y`) |
| liburing | system `liburing.so.2` (Ubuntu), via `SLUICE_HAS_LIBURING` |
| baseline | Sluice master `be28f275` (frozen after PR #569 merged; task's original baseline `859e4b56…` superseded per the task's own re-check rule) |

## Production library compile command (verbatim, arm `base`, `sluice_async`)

```
/usr/bin/g++ -c -m64 -fvisibility=hidden -fvisibility-inlines-hidden
  -Wall -Werror -O3 -std=c++20
  -I<tree>/include
  -DSLUICE_LIBURING_PROFILE=on -DSLUICE_HAS_LIBURING -DNDEBUG
  -o build-ring/.objs/sluice_async/linux/x86_64/release/src/async/detail/request_core.cpp.o
  src/async/detail/request_core.cpp
```

- **No LTO** in the production config. This is load-bearing for the audit:
  cross-TU dead-store elimination does not happen (verified experimentally,
  see gimple-sra-dse.md §2), so library-internal dead copies are physically
  executed in production.
- `sluice_core` (e.g. `blocking_file.cpp`) uses the same flags minus the
  uring defines.
- `SLUICE_ASYNC_INTERNAL_TESTING` is **not** defined for production objects;
  it appears only in test/bench build configs (`build-bench-*`).
- Object files carry DWARF from the distro default; `objdump -drwC` and
  `--disassemble=` were used for the machine-code inventory.

## Dump capture (exact commands)

GIMPLE dumps were captured per object with GCC 15's `-dumpdir`/`-dumpbase`
(no `-fdump-dir` exists; `-fopt-info-sra`/`-fopt-info-dse` are rejected —
only `-fopt-info-all` works):

```
/usr/bin/g++ -c -O3 -std=c++20 -I<tree>/include -DNDEBUG \
  -fdump-tree-optimized -fdump-ipa-modref -fopt-info-all \
  -dumpdir=<outdir>/ -dumpbase <name>.cpp \
  -o <outdir>/<name>.o src/async/detail/<name>.cpp
```

Session artifacts (full dumps and objdumps) live under `/tmp/dp0/dumps/` and
`/tmp/dp0/artifacts/` for this session; the load-bearing passages are quoted
verbatim in `results/receipts/assembly-excerpts.md` and
[gimple-sra-dse.md](gimple-sra-dse.md). Regeneration:
`results/receipts/build_arm.sh` + the command above.

## Pass inventory relevant to this audit (verified, not assumed)

Dump suffixes were read from actual dump files on this compiler build:
early ESRA `039t.esra`, early DSE `045t.dse1`, local modref `055t.modref1`,
IPA-SRA `088i.sra` (suffix `i`), main SRA `138t.sra`, DSE instances
`125t.dse2`/`158t.dse3`/`213t.dse5`, store-merging `220t.store-merging`,
final GIMPLE `270t.optimized`. Under `-flto`, `lto1` runs a WPA pass then
LTRANS partitions that re-run the same-numbered GIMPLE pipeline
(`*.ltrans0.ltrans.270t.optimized`) — unused here since production has no
LTO.

## Audit scope boundary

This inventory freezes the config under which all R1/R2/R4 statements were
made. A different toolchain (notably one with `-flto`, or a GCC with
stronger cross-TU modref summary propagation) could change which dead stores
survive; the mechanism conclusions in this study cite the exact passes and
machine instructions observed under the config above.
