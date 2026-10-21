# M2 native-Linux A/B harness

Research harness for the M2 campaign. It is not registered with xmake, is not
part of the product, and is not a general benchmark.

## Files

| File | Role |
|---|---|
| `pipeline_ab.cpp` | The workload: `RequestScope::submit_read` over a 64 MiB tmpfs fixture, bounded window, warmup/timed/oracle/latency phases, process CPU and allocation counters, JSONL output |
| `run.sh` | Builds the harness against each arm's `libsluice_async.a`/`libsluice_core.a`, runs the rotated 20-config matrix, records arm/binary/library SHA-256 and load/frequency, then validates |
| `analyze.py` | Validates the JSONL and computes paired ratios and bootstrap CIs |
| `validate-results.sh` | Independent gate: one timed row per (config, arm, rep), oracle PASS, op count match, zero timed allocations |

## Requirements

- Linux with liburing 2.13+, a C++20 compiler, `taskset`, `perf` (optional).
- Five built worktrees: `base`, `h1`, `h2a`, `h2b`, `h2ab`, each configured with
  `xmake f -m release --liburing=y -y` and built with `xmake build -g bench`.
- A tmpfs or otherwise warm path for the fixture.

The candidate patches are not applied by this harness. Apply them to separate
worktrees first; see `docs/research/native-linux-m2/patches/README.md`.

## Run

```bash
bash run.sh <worktree-root> <output-dir> [reps] [cpu]
```

Defaults: 15 repetitions, CPU 2. The runner:

1. records provenance, per-arm git SHA/dirty paths, binary/library SHA-256;
2. creates the fixture if needed;
3. for each repetition, rotates configuration and arm order;
4. runs each arm with `taskset -c "$CPU"` and appends JSONL;
5. records `scaling_cur_freq` and `loadavg` around every run;
6. calls `validate-results.sh`.

A non-zero timed allocation, a failed oracle, a missing row or a failed
validation gate makes the campaign fail. Never relax those gates to obtain a
timing row.

## Analyze an existing campaign

```bash
bash validate-results.sh path/to/ab.jsonl 15
python3 analyze.py path/to/ab.jsonl --reps 15 --json-out path/to/summary.json
```

`analyze.py` reports, per configuration and arm, the median paired ratio versus
`base` and a 95% percentile bootstrap CI over the repetitions (10,000
resamples, fixed seed). It does not use CV thresholds as an equivalence proof.

## Workload notes

- The fixture is a `fill_pattern` file. Every repetition verifies the bytes read
  against the pattern; the fixture is created once per campaign.
- The default matrix is 4 KiB/64 KiB/1 MiB, capacities 16/64/256/1024, window
  depth 1/4/8. Request counts are 20,000/2,000/500.
- The fixture is on tmpfs in the reported E5 campaign, so the workload is
  cache-warm and measures the async path, not storage.
- The harness is compiled identically for every arm with
  `-std=c++20 -O3 -DNDEBUG`; only the linked Sluice libraries differ.
- `perf record` needs a reduced ring (`-m 64` on E5) because the default perf
  ring and the io_uring ring share the locked-memory budget. This is a harness
  environment note, not a measurement parameter.
