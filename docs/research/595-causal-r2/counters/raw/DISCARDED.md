# DISCARDED raw blocks — D1 substring-filter contamination

Date: 2026-11-04 (causal-r2 phase B). These directories are retained for
transparency but are **not used** in any analysis.

## Cause

`bench::case_selected()` (`bench/bench_w02_pipeline.cpp` uses
`bench_common.hpp:500-509`) treats `SLUICE_BENCH_CASE_FILTER` as a substring
unless it starts with `=`. The first D1 blocks used
`SLUICE_BENCH_CASE_FILTER=W02-uring-4k-d1`, which also matches
`W02-uring-4k-d16` (the filter is a prefix of the d16 case id). Each invocation
therefore ran both cases, and the source-counter dump contained one phase per
case; summing them produced artificial `sqes_deferred` / `actual_enters` /
`flushes` values (e.g. 0.876 deferred/op at "D1").

Direct evidence of contamination: `strace -f -c -e trace=io_uring_enter` on the
same binary/config counted 129,610 enters for the d1-filter run, consistent
with two cases × three passes × 20k ops (plus startup), not one case.

## Discarded directories

- `raw/pmu-d1-ab/`
- `raw/cnt-on-full-d1/`
- `raw/cnt-off-full-d1/`
- `raw/split-d1/`

## Authoritative replacements (exact filter `=W02-uring-4k-d1`)

- `raw/pmu-d1x-ab/`
- `raw/cnt-on-full-d1x/`
- `raw/cnt-off-full-d1x/`
- `raw/split-d1x/`

These use `case_selected`'s exact-match branch (`filter[0] == '='`) and contain
exactly one case per invocation. D1 conclusions in `cost_table.md` use only the
`*-d1x` blocks.
