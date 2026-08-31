# TAX-0 COPY-AB-1 — Application-Level Copy A/B Design (frozen before measurement)

- Experiment ID: `COPY-AB-1`
- Task: does the #256-selected R1 reverse-scan router candidate materially
  improve a REAL Sluice file-copy workload?
- Base authority: `c4f0b4aacaa55c5c204287b4d7798533af49535b` (master, clean).
- Branch: `research/tax0-copy-ab1`.
- Prior evidence: #254 EXP-U0 (capacity-tax causal attribution), #256 router
  shootout (R1 selected; GM instr 0.8397 / cycles 0.9093 over its op matrix —
  MOTIVATION ONLY, not a preregistered effect size).
- Status: DESIGN — every threshold/matrix below is frozen BEFORE any official
  R0-vs-R1 run. A/A calibration refines only the noise envelope and the
  repetition count, never the winner rule.

## 1. Authority trace (verified from code at the base SHA)

The real copy path is:

```
sluice_copy::run_pipelined_copy_with_backend()   apps/sluice-copy/copy_task.cpp:418
  → argument + resource-limit validation          (copy_task.cpp:428-440)
  → pipeline slot allocation (P slots)            (copy_task.cpp:446-456)
  → PipelinedCopyTask::run_body()                 (copy_task.cpp:208-397)
      ctx.submit_read/submit_write                RuntimeTaskContext
  → run_task_to_result<CopyStats>(workers, backend, task)   (copy_task.cpp:467)
      → ApplicationRuntime + AsyncIoContext       include/sluice/async/task_result.hpp
        → injected AsyncBackend
          → UringAsyncBackend (research injection)
            → find_live_router_cookie_()          src/async/uring_backend.cpp:1118
            → CQE → RequestArena::reap → Completion publication
      → copy pipeline resumes
```

Verified facts (each checked against the base-SHA source):

1. `run_pipelined_copy_with_backend()` IS the normal app path: the production
   `run_pipelined_copy()` (copy_task.cpp:409-416) constructs
   `ThreadPoolBackend()` and forwards to the same function with the same task
   body. The injected variant runs the IDENTICAL algorithm — only the backend
   object differs. (Also matches the task brief: the normal CLI constructs
   ThreadPoolBackend.)
2. `pipeline_depth` P = read-ahead slot count; up to P reads outstanding; at
   most ONE write outstanding at a time; writes are submitted in strictly
   ascending offset order (copy_task.hpp:87-101, copy_task.cpp:272-309).
   Code-traced peak of concurrently ACCEPTED backend requests = P: Phase 2
   always reaps one read (P-1 outstanding) before submitting the write
   (P-1+1 = P); slot recycle replaces the written slot's read (stays P).
   Phase 1 submits up to P reads. Verified again by smoke (C=P accepted with
   zero `would_block` rejections); frozen C_near keeps a +1 margin.
3. No extra copy implementation is required: the research bench calls the
   public `run_pipelined_copy_with_backend()` with a caller-constructed
   backend; `copy_task.cpp` is NOT modified.
4. `UringConfig` exposes `request_capacity` and `queue_depth` independently
   (include/sluice/async/uring_backend.hpp:97-100); validation rejects only
   `request_capacity == 0`, `> slot_index_max`, or `queue_depth == 0`
   (uring_backend.cpp:494-496) — C < Q and C > Q are both legal by contract.
5. The #256 R0/R1 router modes exist ONLY under
   `SLUICE_ASYNC_INTERNAL_TESTING`: `RouterFixModeForTest::production_baseline
   | reverse_scan` switched via `set_router_fix_mode_for_test()` (fresh-backend
   operation; backend must be quiescent). The production build compiles the
   untouched forward scan (uring_backend.cpp:1193-1199).
6. Sync policy: primary campaign uses `SyncPolicy::none` — durability is a
   separate question and must not contaminate the router measurement.

## 2. Question / claim under test

H-COPY-1: for the existing sluice-copy bounded pipeline on the REAL
UringAsyncBackend, replacing R0 forward router scanning with R1 reverse
scanning reduces application CPU cost when `request_capacity C >> actual
active I/O depth`.

- Primary metric: `instructions / copied byte` (user-mode `perf stat`).
- Secondary: `cycles / copied byte` (same rule).
- Application result, separately classified: `wall_ns / byte`, MiB/s.
- The #256 GM numbers are motivation only; no effect size is preregistered.

## 3. Experiment principle — single-variable A/B

Same application, same source bytes, same destination semantics, same copy
algorithm, same backend TYPE and config, same scheduler/runtime shape, same
buffer size, same pipeline depth, same filesystem, same process placement,
same worker count. ONLY DIFFERENCE: R0 forward router scan vs R1 reverse
router scan, selected through the existing #256 research seam before the
runtime starts. No production code change; no CLI behavior change; no copy
algorithm change.

## 4. Research bench (`tax0_copy_ab_bench`)

New research-only target `bench/tax0_copy_ab_bench.cpp`, wired in
`xmake/benchmarks.lua` exactly like `tax0router_shootout_bench`:
deps `{sluice_core, sluice_async_internal_testing}` plus the app's
`copy_task.cpp` (the research instrument links the REAL app copy engine).

- CLI: `--backend uring-r0|uring-r1|threadpool`, `--buffer-size`,
  `--pipeline-depth`, `--request-capacity`, `--queue-depth`, `--workers`,
  `--src`, `--dst`, `--expected-bytes`, `--reps` (official runner uses 1).
- uring modes: `UringAsyncBackend(UringConfig{C, Q})`; fail closed (exit 3)
  unless `SLUICE_HAS_LIBURING` compiled AND `available() == true`.
  `set_router_fix_mode_for_test()` is applied BEFORE the runtime drives the
  backend (quiescent, single-threaded moment after construction).
- threadpool mode: `ThreadPoolBackend()` default construction (the exact
  production backend object); router witness recorded as not-applicable.
- The measured copy = the whole `run_pipelined_copy_with_backend()` call
  (build/start/submit/wait/drain/join — the necessary Runtime/backend
  execution). Source generation, destination verification, JSON emission,
  cleanup all live OUTSIDE it (and outside the perf-measured process, see §6).
- Same-work witness per rep: `bytes_copied == expected file size`;
  `read_ops ∈ [ceil(N/B), ceil(N/B) + P]` (beyond-EOF zero reads are legal
  copy-algorithm work in the EOF window, bounded by P slots);
  `write_ops == ceil(N/B)` and `short_writes == 0` expected on
  tmpfs/warm-btrfs (recorded; validator fail-closes on violations);
  router witness: exactly `read_ops + write_ops` operation-cookie lookups,
  all hits, zero control/transport lookups (no-cancel copy).
- One process per measured candidate run; `--reps 1` per process.

## 5. Matrix (frozen BEFORE official runs)

Notation B (buffer), P (pipeline depth), C (request capacity), Q (queue depth).
`Q = 64` for ALL primary cells (kernel ring depth held constant so C is the
only capacity variable; legal for every C below by §1.4 and smoke).

- File: 256 MiB source, deterministic pseudo-random bytes (splitmix64, seed
  `0xE1E1E1E121212121` — the SAME generator as #254/#256 benches), generated
  once per session OUTSIDE measurement, sha256 recorded and bound into rows.
- Buffers: 4 KiB, 64 KiB, 1 MiB. Depths: P = 1, 8, 32.
- Capacities per P: `C_near = P + 1` (code-traced peak accepted = P; smoke
  verifies C = P is accepted, C_near keeps the smallest safe margin),
  `C_mid = 128`, `C_high = 512`.
- Cell count: 2 fs × 3 B × 3 P × 3 C = 54 cells; candidates R0/R1 → 108 cell
  arms; 9 blocked-randomized rounds, seed `0x434F5059`.
- Sensitivity amendment (pre-registered): if 1 MiB × 256 MiB proves too few
  ops for stable counters during calibration/smoke, ONLY that buffer group is
  re-run at 1 GiB source, frozen BEFORE any R0-vs-R1 comparison. Decision is
  made on counter stability, never on R1-vs-R0 direction.
- ThreadPool control: B = 4 KiB, P = 8, both fs groups, same 9 rounds; never
  mixed into the R0/R1 GM.

## 6. Measurement protocol

- Reuse the TAX-0 host discipline: one process per run under
  `perf stat -x,` user-mode events `instructions:u cycles:u` (required,
  fail-closed) + optional branches/branch-misses/cache-misses;
  `taskset -c 0,2,4,6` (verified non-SMT physical cores 0,2,4,6 on this host,
  re-verified this session via `lscpu -e`).
- Content verification: after EVERY measured process the runner runs an
  UNMEASURED full byte comparison (`cmp`) of destination vs source; a
  mismatch invalidates the row and fails the campaign closed. Method recorded
  as `cmp` full byte comparison. Source sha256 bound per row.
- Destination is `open(O_WRONLY|O_CREAT|O_TRUNC)` by the bench process — the
  engine-level fd copy path, NOT the CLI temp+rename lifecycle (the question
  is the copy pipeline, not rename/fsync metadata).
- Cache discipline: tmpfs group = CPU microscope; btrfs group = WARM page
  cache control (warmed deterministically by the A/A + warmup passes before
  official rounds; policy recorded; no drop_caches).
- Warmup: 1 unmeasured round over all cells before official rounds.
- Unmeasured generator/verifier helper: `tax0_copy_ab_bench --generate` /
  `--verify-hash` modes (never invoked inside a perf-measured region).

## 7. A/A calibration (measured; frozen BEFORE any R0-vs-R1 run)

Two labels `R0-A` / `R0-B` running the IDENTICAL `production_baseline` mode,
interleaved as if real candidates (per-round permutation over the
(label, cell) entries — candidate order within a cell is randomized, closing
positional bias):

- Sessions: tmpfs + warm btrfs, cells (4 KiB, P8, C32), (4 KiB, P8, C512),
  (1 MiB, P8, C512), 11 paired repetitions each (66 measured rows per
  session). Artifacts: `tax0-copy-ab1-aa-tmpfs.json`,
  `tax0-copy-ab1-aa-btrfs.json`.
- Noise envelope (log2 paired-ratio |d| p90, max over cells):

  | metric  | tmpfs p90 | btrfs p90 | FROZEN envelope (max) |
  |---------|-----------|-----------|-----------------------|
  | instr   | 0.0022    | 0.0000    | 0.002212              |
  | cycles  | 0.1168    | 0.1464    | 0.146368              |
  | wall    | 0.3987    | 0.4114    | 0.411414              |

- FROZEN materiality rule: R1/R0 is a MATERIAL improvement for metric m iff
  `median_log2(ratio) <= -max(2 × NOISE[m], log2(1/0.98))`:
  - instructions: threshold log2 0.029146 → ratio ≤ 0.980 (the 2% floor
    dominates; A/A instr noise is negligible).
  - cycles: threshold log2 0.292735 → ratio ≤ 0.8164 (2×noise dominates —
    a ≥18.4% cycles/byte reduction is required).
  - wall: threshold log2 0.822828 → ratio ≤ 0.5653 (2×noise dominates).
    Wall is never required for a CPU-benefit PASS; it is separately
    classified with this envelope.
- Pre-registered §5 sensitivity amendment decision: NOT invoked. The
  1 MiB × 256 MiB cells proved counter-stable (instr p90 |d| ≈ 0.0000 in
  both sessions; 1 MiB total ≈ 51M instructions per measured process). The
  campaign stays at the frozen 256 MiB file size for ALL buffer groups.

## 7.1 Out-of-envelope finding (recorded, NOT fixed in this task)

While smoke-testing the capacity boundary (C < P, i.e. deliberately outside
the frozen envelope), the copy fails cleanly with `would_block` in some
geometries but TERMINATES the process in others
(`uring_non_quiescent_destruction_fail_fast` from `~UringAsyncBackend`
during `ApplicationRuntime::join → close_resources`).

Root cause (verified with debug builds + gdb, base SHA c4f0b4a):
`copy_task.cpp` Phase 3 error-path drain skips completions whose state is
READY (published by the reap path but not yet consumed by the task):
`Completion::outstanding()` returns false for `State::ready`
(completion.hpp:509-511), so `if (s->read_c.outstanding())` misses them.
On fast filesystems with small buffers, reads complete and publish BEFORE
the task reaches Phase 3; those completions are never taken/reset, their
arena slots stay `completion_ready`, and the backend destructor correctly
fail-fasts on the non-quiescent destroy (AGENTS.md §14). Reproducer:
`--backend uring-r0 --buffer-size 4096 --pipeline-depth 8
--request-capacity 7` on a 16 MiB tmpfs source (10/10 aborts; B=1 MiB
10/10 clean `would_block`, because 1 MiB reads are still outstanding —
not yet published — when Phase 3 starts).

Classification: TRUE POSITIVE, application-level, ERROR PATH ONLY (the
success path consumes every completion via await_take/await_drain; the
clean would_block return is also reachable). Backend-agnostic (the same
skip would occur with any bounded backend; the uring backend's fail-fast
is the correct report of the app's contract violation). NOT in the
experiment envelope: every frozen cell uses C ≥ P+1 where no submit is
rejected. Left for a separate authorized fix task; not worked around in
the research bench (which never enters the failing configuration).

## 8. Analysis plan (frozen)

- Per exact cell (fs × B × P × C): normalized ratios of per-arm medians
  (instr/byte, cycles/byte, wall_ns/byte). 1.0 = same, <1 = improvement.
- Blocked paired design preserved: per round × cell log-ratios
  `d = log2(R1/R0)`; report paired median effect and geometric mean; clustered
  (round-level) bootstrap CI if implemented.
- Aggregates: overall GM plus tested-strata consistency checks by fs, B, P, C
  (never claimed as universal robustness).
- Verdicts (exactly one): PASS — APPLICATION CPU BENEFIT MATERIAL /
  PASS — CPU BENEFIT, WALL BENEFIT NOT MATERIAL / PASS — BENEFIT ONLY IN
  CAPACITY-SKEWED REGIMES / NOT SUPPORTED — NO MATERIAL APPLICATION BENEFIT /
  PARTIAL / BLOCKED. No invented positive verdict.

## 9. Machine-readable artifacts

`docs/results/performance-attribution/tax0-copy-ab1-aa-tmpfs.json`,
`tax0-copy-ab1-aa-btrfs.json` (A/A), `tax0-copy-ab1-tmpfs.json`,
`tax0-copy-ab1-btrfs.json` (primary campaign, one session per fs), and
`tax0-copy-ab1-control-tmpfs.json`, `tax0-copy-ab1-control-btrfs.json`
(ThreadPool control, separate label — never mixed into the R0/R1 GM).
Every row binds:
git SHA, binary sha256, dirty paths, environment fingerprint + environment_id,
candidate, filesystem label, B/P/C/Q, file size, source sha256, bytes copied,
read/write ops, short writes, round, execution order index, instructions,
cycles, wall/user/sys ns, same-work witness, content verification method +
result, real_uring, router witness (lookups/iterations).

## 10. Validator

`scripts/bench/tax0-copy-ab1-validate.py` with an EXTERNAL frozen manifest
(the frozen matrix + thresholds live in the validator/manifest, not in the
artifact). Recomputes medians, normalized ratios, GMs, paired effects, and the
materiality classification from raw rows; validates candidate/fs/B/P/C/Q/file
size/reps/seed/source identity/same-work/content verification/real-uring.
Mutation tests (drop row, drop cell+resync, drop candidate, relabel candidate,
change file size/source hash/bytes, false verification, real_uring=false,
tamper instructions/normalized ratio/GM/order/A-A threshold) must all fail
closed.

## 11. Freeze discipline

Design → bench → smoke (real ring + C_near + threadpool) → A/A → freeze
thresholds + matrix → commit → push → DRAFT PR → COPY-AB1-FREEZE SHA → only
then official measurements. Any post-freeze harness semantics change
invalidates affected evidence; refreeze and rerun.

## 12. Non-goals (hard boundaries)

No production R1; no CLI default change; no copy algorithm change; no
production router path change; no EXP-U1; #250/#255 stay governed elsewhere.
The ThreadPool control exists to prove the harness cannot fake a
capacity-dependent router effect, not to crown a backend winner.
