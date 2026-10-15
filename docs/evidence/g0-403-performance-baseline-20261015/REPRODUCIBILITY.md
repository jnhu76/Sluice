# Reproducibility and evidence integrity

```text
[SLUICE-G0-403]
BASE_SHA = dcc46266709e98d760b3be2957d18c09e1e55952
STAGE = C — integrity record
```

## Measured artifacts

The measured binaries' sha256 are recorded in `matrix-run.log` (written by
`bench/run_matrix.sh` before the runs). They were built by
`bench/build_profiles.sh release`, which asserts each binary's profile from its
dynamic dependencies rather than from the directory name:

* `bench_w02_pipeline` and `bench_w03_external_loop` in the ring profile depend
  on `liburing.so.2`; the no-ring profile binaries do not.
* `bench_w01_direct` links `sluice_core` only, so it is byte-identical in both
  profiles — asserted, not assumed.

## Harness delta after the measured run

Two harness capabilities were added *after* the measured matrix, and both are
opt-in so that a default rebuild reproduces the frozen case list exactly:

1. `G0_CASE_FILTER=<substring>` (and `=ID` for an exact match) — case selection
   for targeted profiling runs. Default (unset) runs every case.
2. `G0_DIAGNOSTICS=1` — adds the post-hoc `-core64` capacity-isolation
   diagnostic. Default (unset) omits it.

Verified: with the default environment, the rebuilt harness emits **exactly the
16 `W02-*` case ids** recorded in `raw/w02.jsonl` (set equality checked), so the
frozen matrix is reproducible from this directory's harness.

The timer-instrumented latency pass and the throughput phase semantics are
unchanged by these additions: neither touches the timed regions.

## Production code

`git diff BASE_SHA..EVIDENCE_SHA -- src include` is empty. The only build-system
change is `includes("xmake/bench.lua")` in `xmake.lua` plus the new
`xmake/bench.lua`; no existing target, flag or source file was modified.

## Known defect in the recorded cold-path cases

The run that produced `W01-read_at-64k-seq-cold` and `W01-read_at-1m-seq-cold`
probed the fixture *after* `fadvise(DONTNEED)`, re-warming 4 MiB / 64 MiB of the
file before the timed phase. The harness now probes first and evicts last. The
recorded figures were left untouched (re-measuring would silently replace
baseline evidence) and are disclosed in `MEASUREMENT.md` as conservative.

## Post-hoc attribution runs

The `-core64`, `-scope64`, CPU-pinning, `perf` and `strace` runs are attribution
experiments, not baseline measurements. `-scope64` was preregistered;
`-core64`, pinning, `perf` and `strace` were post-hoc and are labelled as such in
`MEASUREMENT.md`. Throughput figures from the symbol-preserving profiling build
are never compared with the baseline binary's figures.
