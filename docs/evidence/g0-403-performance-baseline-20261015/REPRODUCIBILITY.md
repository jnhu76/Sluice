# Reproducibility and evidence integrity

```text
[SLUICE-G0-403]
BASE_SHA = dcc46266709e98d760b3be2957d18c09e1e55952
STAGE = C — integrity record (revision 2)
```

## Two evidence revisions

| Revision | Files | Status |
|---|---|---|
| v1 | `raw-v1-oracle-degenerate/*.jsonl` | The run published in comment B. Its content oracle was degenerate (the pattern table was identically zero), so its correctness claim was vacuous and its cold-case numbers were reading zero pages. **Retained only because comment B cites its hashes**; they still match byte-for-byte. |
| v2 | `raw/*.jsonl` | Authoritative. Non-degenerate oracle, oracle self-test gate, corrected write-case and buffer-aliasing harness bugs, 7 measured repetitions per case. |

## Byte-identical reproduction of the measured binaries

Measured binaries were rebuilt from committed sources with the recorded flags and
their sha256 match the values written into `matrix-run.log` before the runs.

```text
v2 (authoritative, source = BENCHMARK_HEAD_SHA recorded in COMMIT.txt):
  noring bench_w01_direct         ab2314f986b5a37cfe3e6fe8017c38ee42cd581aa3a1f3f3de35e170c7da67a4
  noring bench_w02_pipeline       c1923010df7b66b84350fcad0d091f3afa8914ba7ff32119e3f96f3c2bd2f97b
  noring bench_w03_external_loop  3bfe189a3d473d681945bb8c78d2335887d9414c7c53362dffe259549843f0d5
  ring   bench_w01_direct         ab2314f986b5a37cfe3e6fe8017c38ee42cd581aa3a1f3f3de35e170c7da67a4
  ring   bench_w02_pipeline       60225bc75d13860e90940664dbae7d874da1b545909e37834941b81568f9cd90
  ring   bench_w03_external_loop  38b730f791036403ad0f0bc0ed335a7f423be1e712778b6d6790c246f522d4a8

v1 (published in comment B, reproduced from commit 8c98b417):
  noring bench_w01_direct         9ec8915ea53e44f1e6305822735ca505c4cf61b864335954ab97d27b9a0a69fd
  ring   bench_w02_pipeline       dcacf5a19c233dd0a27d94a8e59ebb07c7b014a3e5065c9a534974848ec2f971
  ring   bench_w03_external_loop  1bcfa3f8b725f87071f26b8c030cc2fa00e162f5fb70d2784219287dcf6477cd
```

Reproduction recipe (per binary; `bench/build_profiles.sh release` produces the
archives first):

```bash
# no-ring W-01
gcc -c -m64 -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Werror -O3 \
    -std=c++20 -Ibench -Iinclude -DSLUICE_LIBURING_PROFILE=off -DNDEBUG \
    -o w01.o bench/bench_w01_direct.cpp
g++ -o w01 w01.o -m64 -Lbuild-bench-noring/linux/x86_64/release -s -lsluice_core

# ring W-02 / W-03
gcc -c -m64 -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Werror -O3 \
    -std=c++20 -Ibench -Iinclude -DSLUICE_LIBURING_PROFILE=on \
    -DSLUICE_HAS_LIBURING -DNDEBUG -o w02.o bench/bench_w02_pipeline.cpp
g++ -o w02 w02.o -m64 -Lbuild-bench-ring/linux/x86_64/release -s \
    -lsluice_async -luring -lsluice_core
```

Profile identity is asserted from dynamic dependencies, not from directory names:
only the ring builds depend on `liburing.so.2`, and `bench_w01_direct` is
byte-identical across profiles because W-01 has no async dependency.

## Provenance of each committed raw file

The authoritative v2 files are named canonically (`raw/w01.jsonl`, `w02.jsonl`,
`w03.jsonl`, `w02-diagnostics.jsonl`) while the run log records them with a `v2`
suffix, because the runner accepts a suffix to keep revisions apart. **The
committed files were renamed after their runs; renaming does not change content,
and the sha256 list in `COMMIT.txt` identifies exactly the bytes committed.** File
mtimes equal the end of the corresponding run block in `matrix-run.log`.

Because the log is append-only across stages, it contains more invocations than
the six committed evidence files. What the extras are:

| Log entry | Status |
|---|---|
| v2 primary block (17:47:30-17:50:17), outputs `w01v2`/`w02v2`/`w03v2` | **Authoritative** for `raw/w01.jsonl`, `raw/w02.jsonl`, `raw/w03.jsonl` |
| `w02-diagnostics` at 17:59 (172 records, exit 0) | **Authoritative** for `raw/w02-diagnostics.jsonl` |
| no-ring controls in the same v2 block | **Authoritative** for the two `*-noring-notrun.jsonl` files |
| an earlier diagnostics run at 17:44 (exit 1) | **Superseded and deleted**: it contained the buffer-aliasing failures described in `MEASUREMENT.md` section 0. Its output is not committed. |
| an earlier v2 build (binaries `bc238cca` / `161cacb4`) | **Superseded** by the authoritative build (`c1923010` / `60225bc7`) after the free-slot fix; only the authoritative binaries' outputs are committed. |
| three no-op invocations at 17:58:39 / 17:58:47 | Runner stage-gating bug; they executed no case and produced no data. Fixed; the diagnostics stage no longer re-runs W-01. |

The v2 runs were executed on a **dirty working tree** (`git_status_dirty=16..22`
in the log). That tree's benchmark sources are identical to commit
`BENCHMARK_HEAD_SHA` — verified by `git diff --stat <sha>..HEAD -- bench xmake`
being empty — and all six measured binaries reproduce byte-identically from those
sources, which is the property that matters for the evidence.

## Case selection

`G0_CASE_FILTER` selects cases (substring, or `=ID` for an exact match);
`G0_DIAGNOSTICS=1` adds the post-hoc diagnostics (`-core64`, `-anyready`). Both are
opt-in and default-off, so a default rebuild reproduces the frozen primary case
list exactly — verified by set equality against the recorded case ids.

## Known limits of the harness that produced the recorded evidence

* The `-cold` W-01 cases evict in `prepare()` only, so their **instrumented
  latency pass is warm-cache** while their throughput phase is attempted-cold.
  Annotated in `MEASUREMENT.md` section 2; not re-measured.
* The lock probe counts only interposed `pthread_mutex_lock` calls, so its
  acquisitions-per-operation figures are a **floor**.
* `strace` and `perf` perturb the run; only counts, ratios and per-operation
  counters from them are used, and the profiling build's absolute throughput is
  never compared with the baseline binary's.

## Production code

`git diff BASE_SHA..HEAD -- src include` is empty. The only build-system change is
`includes("xmake/bench.lua")` in `xmake.lua` plus the new `xmake/bench.lua`; no
existing target, flag or source file was modified. Six existing suites (99 named
cases) were re-run green with the harness present
(`logs/existing-tests-unaffected.txt`).
