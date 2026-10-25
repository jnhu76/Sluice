# Gate E — reducer reproduction receipt (archive acceptance)

Commands (from the repository root of the research branch, 2026-10-25,
WSL host, python 3.14):

```sh
python3 bench/research/uring-router-580/reduce_ab.py \
    docs/research/uring-router-580/results/e5/ab-pairs-e5.jsonl \
    docs/research/uring-router-580/results/e5/aa-pairs-e5.jsonl

python3 bench/research/uring-router-580/reduce_cache.py \
    docs/research/uring-router-580/results/cache-e5.jsonl --json-out /tmp/e5.json

python3 bench/research/uring-router-580/reduce_cache.py \
    docs/research/uring-router-580/results/cache-wsl.jsonl
```

Published figure → reproduced value (all exact matches from the committed
raw JSONL, no adjustments):

| Claim | Published | Reproduced |
|---|---|---|
| R6 E5 A/B `cancel-c1024` wall | 0.9954 [0.9673, 1.0212] | 0.9954 [0.9673, 1.0212] |
| R9 E5 net-wall `pipe_d512_ws16m` | 0.9627 [0.955, 0.970] | 0.9627 [0.955, 0.970] |
| R9 E5 A/A floors (`d64_aa_warm`, `d512_aa_warm`, `d64_aa_16m`) | 1.0011 / 1.0065 / 1.0037 | 1.0011 / 1.0065 / 1.0037 |
| R9 E5 flush `flush_d64` / `flush_d512` wall | 1.2623 / 2.0772 | 1.2623 / 2.0772 |
| R9 WSL flush `flush_d512` wall | 1.2963 | 1.2963 |
| R9 probe invariants (all cells both hosts) | 1.00 lookups/install, 0 misses, ≈D/2 probes | e.g. E5 D512: ratio 1.00, misses 0, avg 257.8 |

Note: the text printer of `reduce_cache.py` shows wall/user/sys only; the
net-wall figures are in its `--json-out` payload under
`pairs/<shape>/ratios/net_wall_s` (this asymmetry predates the archive and
is disclosed here so future readers do not mistake the text table for the
complete metric set).

PMU share figures (5.07% WSL / 5.80–9.81% E5) are not reducer-reproducible;
they derive from the committed `perf stat`/`perf report` text and `.data`
under `results/pmu*/` and were independently recomputed during the R9 delta
review (see `reviews.md`).
