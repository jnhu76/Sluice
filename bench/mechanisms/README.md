# bench/mechanisms — M0/M1 mechanism experiments

Isolated Layer-A experiments for the admitted M0/M1 candidates
([study documents](../../docs/research/state-complexity/)):
H1 io_uring router lookup (scan vs direct `user_data` slot index), H2
owed-event visibility (sweeps vs maintained counter), H3 RequestScope slot
management (first-free scan vs LIFO free list), plus the W-02 harness-only
bookkeeping control.

Each binary self-checks before it times: a reference-model oracle gates
every repetition and a named negative control must be caught (the runner
greps for `CAUGHT` and fails on `ORACLE FAIL` / `SELFCHECK FAIL` /
`oracle divergence`). These are measurement instruments for the study and
for any future owner-approved proposal validation — they are not
registrations of production behavior, link no Sluice library, and are not
built or run by `xmake` / `xmake test` / CI.

Run: `bench/mechanisms/run_mechanisms.sh <output-dir> [reps] [ops-scale]`
(builds with `g++ -std=c++20 -O2 -Wall -Wextra`, pins to CPU 2 by default,
writes JSONL + decision logs + per-arm perf-stat + provenance into the
output directory). Recorded results from the study run live in
`docs/research/state-complexity/results/`.
