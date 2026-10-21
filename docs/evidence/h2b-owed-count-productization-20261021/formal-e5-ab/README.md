# Formal E5 A/B acceptance run — PASS

Executed 2026-10-22 on the native Linux E5 host by the owner-delegated
session, using the r2 runner exactly as committed at the measured candidate
HEAD (no post-run edits to the runner or reducer; their SHA-256 values are
recorded in `provenance.txt` and match the committed files).

- Command: `run_e5_ab.frozen.sh <base-tree> <h2b-tree> c8970702… eb465c4f… ~/h2b-e5-results 2`
  (base = `c8970702aad4fa74d4053d00ae941409e709ce67`, frozen pre-PR master;
  candidate = `eb465c4f96f1d010f8cbed0b5a94bb7d6f6f0778`, PR branch HEAD)
- Host: `E5`, Fedora 44, kernel 7.2.9-201.fc44.x86_64, Intel Xeon E5-2666 v3
  @ 2.90 GHz, 20 cores, CPU 2 pinned, g++ 16.2.1, liburing 2.13
- Arms: both rebuilt from an empty `build/` and a cleared xmake config;
  `worktree_sha` matched the pins; `libsluice_async.a` differs between arms
  and `libsluice_core.a` is byte-identical, consistent with the diff being
  release-code of the async library only (`arm-identity.txt`)
- Dataset: 870/870 rows (2 arms x 15 reps x 9 data configs x {timed, oracle,
  latency} + 2 zero configs x timed), byte oracle green on every repetition
- Verdict: **PASS** (`verdict.json`, 0 reasons; process exit 0)

Median `cpu_ns_per_op` ratios h2b/base (15 paired reps per config):

| config (block,depth,capacity) | median | pairs above 1.03 |
|---|---|---|
| 4096, 1, 1024 (headline) | **0.495** | 0/15 |
| 65536, 1, 1024 | 0.638 | 0/15 |
| 4096, 8, 1024 | 0.776 | 0/15 |
| 4096, 1, 256 | 0.755 | 0/15 |
| 4096, 1, 64 | 0.904 | 0/15 |
| 4096, 1, 16 | 0.956 | 0/15 |
| 65536, 1, 64 | 0.957 | 0/15 |
| 4096, 8, 16 | 0.988 | 1/15 |
| 1048576, 1, 64 (I/O control) | 0.994 | 0/15 |
| 0 (K>0 zero burst), 64, 1024 | 0.990 | 0/15 |
| 0 (K>0 zero burst), 8, 1024 | 0.956 | 0/15 |

Dose-response matches the removed O(capacity) scan: the win grows with
capacity (0.956 at C=16 → 0.495 at C=1024), C=16 is near-neutral, the 1 MiB
I/O-dominated control is neutral, and the K>0 zero-op burst (negative
workload for the delivery path) keeps its baseline.

Hardware-counter attribution (`perf_stat_status=AVAILABLE`; user-only
counters on this kernel) on the headline config, pair 1: task-clock
1410.91 ms (base) vs 703.62 ms (h2b) = 0.499, instructions 9.30 G vs
2.43 G, cycles 3.31 G vs 0.86 G — the candidate executes ~4x fewer
instructions on this shape, consistent with the sweep removal rather than
with timer or measurement artifacts (`perf_*.txt`).

Provenance chain: `runner_sha256=01159c75…`, `verdict_sha256=174e1114…` —
identical to the runner audited by the independent round-2 runner review
(RUNNER_ACCEPTED) and to the committed files at the candidate HEAD.
