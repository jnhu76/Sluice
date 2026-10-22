# MW-HW-1 raw evidence (archived)

Lightweight raw outputs backing the claims in the MW-HW-1 documents. The
heavy artifacts (harness binaries, 64 MiB fixture, the 64 MiB cycles:u
profile `perf-base-4k-d1-c1024.data`) stay external — hashes below; the
archived files are everything needed to recompute the published statistics.

## Contents

- `ab/rows.jsonl` — 216 timed rows plus oracle/latency rows; file order =
  run order; produced by `ab/run_ab_mwhw1.sh` (9 paired rotated reps ×
  6 configs × 4 arms).
- `ab/run_ab_mwhw1.sh` — paired rotated A/B runner (taskset -c 2, tmpfs).
- `ab/arm-identity.txt`, `baseline-identity.txt`, `env-tools.txt` — arm
  binary/library hashes and tool versions.
- `ab/analysis-rep-paired.txt` — authoritative rep-index paired reduction
  (performance.md §1).
- `ab/analysis.txt` — earlier rank-paired reduction, retained as the
  cross-check only (medians agree on every config).
- `analysis/analyze_ab.py` — regenerates `ab/analysis-rep-paired.txt`
  byte-for-byte from `ab/rows.jsonl`.
- `time-v-paired.txt`, `time-v-paired-analysis.txt` — 7-pair interleaved
  `/usr/bin/time -v` rerun (user/system CPU split, syscall-census.md §3).
- `analysis/analyze_time_v.py` — regenerates
  `time-v-paired-analysis.txt` byte-for-byte from `time-v-paired.txt`.
- `strace-*.txt` — syscall censuses (syscall-census.md §1/§2).
  `strace-b0-d1-c1024.txt` is the retracted zero-op attempt: process
  startup only, the harness rejected `--block=0`.
- `perf-base-g.data`, `perf-e1-g.data` — cycles:u profiles backing the
  symbol attribution (syscall-census.md §4).
- `c-sweep-d1.txt` — capacity sweep (performance.md §2).
- `test-gates.txt`, `e1-mutant-receipts.txt`, `mut-*.txt` — correctness
  gates and mutant receipts (performance.md §3).

`time-v-paired*` and `ab/analysis-rep-paired.txt` postdate the round's
freeze-time scratch manifest; this directory's `SHA256SUMS.txt` covers the
complete archived set.

## Verify

```bash
sha256sum -c SHA256SUMS.txt
python3 analysis/analyze_ab.py ab/rows.jsonl | diff - ab/analysis-rep-paired.txt
python3 analysis/analyze_time_v.py time-v-paired.txt | diff - time-v-paired-analysis.txt
```

## External-only artifacts (not archived; SHA-256 at round time)

```
c570cae83eda14d44cebeeef6977a02e534482bbab44ad7a3918c743b4058895  bin/pipeline_ab_base (= _tp)
ae8c8341f0961d79c2950d341ba48b316772f263a5867af719e33ba7def727a6  bin/pipeline_ab_base_g
964d3da2ed73c4e0804384d342560ab68b6b9355ebd75cefea7128a0f431510b  bin/pipeline_ab_e1
c8cb6053dbf1275c5bc00520249ac363d220b3f6d3a868b0791d886beb77f92a  bin/pipeline_ab_e1_g
f2e38b80164f23a90873dee8f41e68dc9a1818e86cd8c9d40c55b5ab01f57c9f  bin/pipeline_ab_e2
981117024ec5512f0489529744ae72617b5a5dd59c061eef15ef167d4c97f093  bin/pipeline_ab_e3
1cdf83d6f6d97d740b0f6e0a10077f2ccde01172631885147705335f532744e6  fixture/data.bin
b485462633bb7f4a97d2b823aa7d1a591439cba8525b6b00993accc1eb7eb6a3  perf-base-4k-d1-c1024.data
```

The round's working copies live in the disposable scratch directory
`/tmp/mw-hw-1-out/`; the files in this directory are the authoritative
copies.
