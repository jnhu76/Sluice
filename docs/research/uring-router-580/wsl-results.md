# R6 — Measurement results: WSL2 exploratory + E5 native

## Protocol

Both hosts ran the same production-shaped A/B protocol (inherited from #575):

- arms fixed at compile time; runner reports wall/user/sys with a full
  byte-pattern + op-count oracle per timed run (0 oracle failures in every
  reported row);
- 15 repetitions per shape with AB/BA order alternation;
- A/A calibration on byte-identical binaries under different paths
  (path-name bias negative control);
- warmup 2-equivalent (`--warmup` fixed per shape: 600 cancel / 2000
  pipeline); identical fixture directory (`/tmp`, tmpfs on both hosts), same
  pinning (`taskset -c 2`);
- reducer (`bench/research/uring-router-580/reduce_ab.py`): paired ratios,
  median with a percentile-bootstrap 95% interval over the 15 pairs (B=10000,
  seed 7). The interval describes the spread of these pairs on this host and
  session; it is not a population confidence guarantee.

## E5 native (primary; the host class #580 names for admission)

A/B ratio = we2/baseline (we2 = reversed by-handle scan direction,
EXPLORATORY_ONLY / NOT_ADMITTED):

| Shape | user_s ratio [95%] | wall_s ratio [95%] | sys_s ratio [95%] |
|---|---:|---:|---:|
| cancel C1024/D64 64 KiB r=0.2 (target shape; S2 probes 993→33) | 0.9867 [0.9195, 1.1094] | 0.9954 [0.9673, 1.0212] | 0.9661 [0.8677, 1.1177] |
| cancel C64/D64 64 KiB r=0.2 | 0.9705 [0.9193, 0.9944] | 0.9888 [0.9834, 0.9963] | 1.0282 [0.9765, 1.0800] |
| pipeline C1024/D64 4 KiB (no-cancel control) | 1.0312 [0.9697, 1.0682] | 0.9986 [0.9753, 1.0136] | 0.9399 [0.8819, 1.0879] |

A/A control (byte-identical binaries, β/α; run order alternates):

| Shape | user_s [95%] | wall_s [95%] |
|---|---:|---:|
| cancel C1024/D64 | 0.9634 [0.9445, 1.0777] | 1.0068 [0.9830, 1.0174] |
| pipeline C1024/D64 | 1.0039 [0.9261, 1.0792] | 0.9956 [0.9899, 0.9999] |

Reading: even on identical binaries the A/A intervals span ±4-8% on user time
(path/run-slot accounting bias), so the cancel-c64 user ratio of 0.9705 [0.919,
0.994] and its wall ratio 0.9888 [0.983, 0.996] — both of whose intervals
exclude 1.0 — are **within the observed A/A bias band** and are not claimed as
effects. (An earlier revision of this table reported the A/A statistic as
first-slot/second-slot due to a reducer normalization bug; Review B caught it
and the reducer now pairs on the binary identity. The A/B tables were and are
arm-normalized and are unaffected.) On the target shape (cancel-c1024), wall
and user both straddle 1.0.
**Conclusion: a 30× mechanism-level probe reduction (993→33 per cancel,
instrumented probe verification) produces no end-to-end effect distinguishable
from the measurement floor on the native host.** The no-cancel control is
neutral as designed.

Work-mix disclosure: the cancel shape's race outcomes vary completed bytes
across runs (E5 spread 7.6%); normalizing user time by verified bytes moves
the cancel-c1024 median 0.9867 → 0.9989, i.e. toward the null — the neutral
conclusion is robust to the work-mix variation. The timed region also includes
the per-read verification and wait work of the harness (the 68% `run_pipeline`
symbol); both are identical across arms, and the shares in this document are
measured against that harness-inclusive denominator.

E5 native perf attribution (`cycles:u`, `-F 1999 -m 64`, symbols resolved
directly by Fedora perf, 1306 samples, worst steady shape C=1024/D=64):

| Symbol | Self share |
|---|---:|
| harness `run_pipeline` (public wait/take spin) | 68.13% |
| `find_live_router_cookie_` | **2.28%** |
| `pthread_mutex_lock` + unlock | ~4.2% |
| `dispatch_one_locked` | 1.96% |
| `submit_read` | 1.93% |

E5 native trigger counts replicate WSL2 structurally (probe counters): 1.00
lookups/op steady at every shape; op-CQE avg probes 8.5 @D16 / ~33 @D64 /
1.0 @D1, independent of C; S2 cancel lookup ~992.9 avg probes @C1024 vs 32.3
@C64; zero natural misses in 100k+ completions. (Widest cell difference
between hosts: pipeline C1024/D64 op-CQE 33.7 WSL vs 32.8 E5.) Raw:
`results/e5/trigger-matrix-e5.jsonl`, `results/e5/ab-pairs-e5.jsonl`,
`results/e5/aa-pairs-e5.jsonl`.

## WSL2 (exploratory; superseded as the primary measurement host mid-round)

Same protocol, gcc 15.2 / liburing 2.14, taskset -c 2:

| Shape | user_s ratio [95%] | wall_s ratio [95%] |
|---|---:|---:|
| cancel C1024/D64 64 KiB r=0.2 | 1.0379 [0.7677, 1.1211] | 0.9990 [0.9758, 1.0565] |
| cancel C64/D64 | 0.9066 [0.8446, 1.0343] | 0.9917 [0.9338, 1.0152] |
| pipeline C1024/D64 | 1.0019 [0.7879, 1.3892] | 1.0138 [0.9956, 1.0435] |

A/A showed the WSL user-s noise floor is very wide (identical binaries:
median 0.81-1.06 with intervals ±20-35%; Hyper-V scheduling) — wall_s is the
only usable metric there, and it is neutral. The E5 campaign above is the
authoritative measurement; WSL rows are retained as the mechanism/counts
cross-check, not as timing evidence.

perf attribution (WSL, symbols mapped through `nm` from raw offsets; 16.2% of
`cycles:u` samples appear at hypervisor/kernel addresses under `:u` and are
excluded): `find_live_router_cookie_` 1.57% (C64/D64 default shape) to 1.69%
(C1024/D64); `issue_running_cancel_locked_` (contains the inlined forward
scan) 0.48% at the 20%-cancel shape.

## Assembly provenance

Archived: `results/disasm_find_live_router_index_{baseline,we2}.txt` per host
toolchain (WSL gcc 15.2.0 from the local run; E5 gcc 16.2.1 from the server).
The two arms differ only in loop direction setup (decrement-from-end vs
increment-from-zero index arithmetic); the match predicate and return values
are instruction-identical. Binary identities in `environment.md` and
`results/manifest.json`. The we2 effect is a real instruction-stream change on
the cancel path, not a code-layout accident — and its measured end-to-end
impact is still below the floor.

## Negative controls retained (per protocol)

- C1024/D1 depth control (trigger matrix): 1.0 probe/op — the scan degenerates
  to a single check at depth 1 regardless of capacity; C16/D16 small-capacity
  control: 8.5 probes/op. No effect anywhere.
- No-cancel pipeline shape: we2 must be neutral — it is (0.999 wall E5).
- A/A path-name bias control both hosts.
- Miss-heavy seam scenario: documents the only true O(C) full sweeps
  (stale-CQE replay), which the healthy kernel path never produces.
