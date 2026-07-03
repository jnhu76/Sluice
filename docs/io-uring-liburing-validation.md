# liburing validation runbook

**Status: SLUICE-CORE-014C.** How to run the experimental io_uring path on a
machine with liburing. The spike (013) is build-gated: without liburing the
normal build is unaffected and the uring path is a clean stub.

## 1. Environment requirements

- Linux kernel ≥ 5.1 (io_uring was merged in 5.1).
- liburing headers + library (development package).
- xmake + a C++20 compiler (same as the normal build).
- A writable temp directory (the tests/bench write temp files).

## 2. Installing liburing headers/library

Debian/Ubuntu:

```bash
sudo apt-get install liburing-dev
```

Fedora/RHEL:

```bash
sudo dnf install liburing-devel
```

From source (if a package is unavailable):

```bash
git clone https://github.com/axboe/liburing
cd liburing
./configure && make -j && sudo make install
```

Verify:

```bash
pkg-config --modversion liburing   # or: ls /usr/include/liburing/io_uring.h
```

## 3. xmake configuration

The project's build option (see `xmake.lua`, the `option("with-liburing")` block):

```bash
xmake f -m debug --with-liburing=true
```

Without `--with-liburing`, the experimental targets compile as unsupported stubs
and the rest of the project is unchanged.

## 4. Building experimental targets

```bash
xmake f -m debug --with-liburing=true
xmake build sluice_core
xmake build -g test
xmake build -g examples
xmake build sluice_experimental_uring   # the experimental lib
```

## 5. Running uring tests

```bash
xmake test
```

The uring tests (`uring_write_batch_test`, `uring_io_context_test`,
`uring_stats_test`) select their real-path branch automatically when
`SLUICE_HAS_LIBURING` is defined.

## 6. Running uring example

```bash
xmake run experimental_uring_write
```

## 7. Running uring bench

```bash
xmake f -m release --with-liburing=true
xmake build -g bench
xmake run uring_write_bench
```

## 8. Expected success output

`experimental_uring_write`:

```text
experimental_uring_write: wrote N bytes via experimental uring path
  submitted_ops=... completed_ops=... bytes_completed=... errors=0
  (experimental only; not a production backend; no durability claim)
```

`uring_write_bench` emits CSV rows where the `uring_write_batch` mode has real
(non-zero) submitted/completed op counts (not the `..._SKIPPED_NO_LIBURING` row).

## 9. Expected skip output when unavailable

`experimental_uring_write`:

```text
experimental_uring_write: SKIPPED (liburing unavailable)
```

`uring_write_bench` emits:

```text
uring_write,uring_write_batch_SKIPPED_NO_LIBURING,<bytes>,0,0,0,0,0,0,0
```

## 10. How to record results

Save under `docs/results/` with the date and host info (see 014D):

```text
docs/results/liburing-validation-YYYY-MM-DD.md
```

Record OS/kernel, filesystem, compiler, liburing version, commit hash, test
result, example result, bench output, and any caveats. **Do not generalize**
— results are local, workload-specific observations.

## 11. Common failure modes

- `io_uring_setup` returns `-EPERM` / `-ENOSYS`: kernel too old, or seccomp/AppArmor
  blocks io_uring (common in containers). The tests should report an error, not
  crash; if they crash, that is a bug to fix.
- `xmake` cannot find liburing despite installing: re-run `xmake f -m debug
  --with-liburing=true` (xmake caches config); check `pkg-config --cflags liburing`.
- Link errors referencing `io_uring_*`: the experimental lib wasn't rebuilt with
  the flag — `xmake build sluice_experimental_uring` then rebuild dependents.
