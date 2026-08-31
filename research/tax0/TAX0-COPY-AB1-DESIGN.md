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
  production backend object); no router dimension exists on that backend,
  so no router witness applies.
- The measured copy = the whole `run_pipelined_copy_with_backend()` call
  (build/start/submit/wait/drain/join — the necessary Runtime/backend
  execution). Source generation, destination verification, JSON emission,
  cleanup all live OUTSIDE it (and outside the perf-measured process, see §6).
- Same-work witness per rep: `bytes_copied == expected file size`;
  `read_ops ∈ [ceil(N/B), ceil(N/B) + P]` (beyond-EOF zero reads are legal
  copy-algorithm work in the EOF window, bounded by P slots);
  `write_ops == ceil(N/B)` and `short_writes == 0` expected on
  tmpfs/warm-btrfs (recorded; validator fail-closes on violations).
  Router causality is INHERITED from EXP-U0/#254 and the #256 router
  shootout; COPY-AB-1 is an APPLICATION-EFFECT validation and does NOT
  independently record structural router-iteration witnesses (lookup
  counts / iterations are not fields of the official COPY-AB-1 artifacts).
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
- Noise envelope (log2 paired-ratio |d| p90, max over cells; the values
  below are the freeze-era RECORDED pooled statistics — superseded by the
  corrected max-per-cell nearest-rank recomputation in §13):

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
rejected. Registered as issue #258 for a separate authorized fix task; not
worked around in the research bench (which never enters the failing
configuration).

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
result, real_uring. (Router-iteration witnesses are NOT recorded — see §4;
router causality is inherited from EXP-U0/#254 and #256.)

## 10. Validator

`scripts/bench/tax0-copy-ab1-validate.py` with an EXTERNAL frozen manifest
(the frozen matrix + thresholds live in the validator/manifest, not in the
artifact). Recomputes per-cell medians, normalized ratios, GMs, the PAIRED
per-round effects, and the A/A noise envelope from raw rows; classifies
materiality from the recomputed per-cell PAIRED MEDIAN against the frozen
thresholds (ratio-of-arm-medians is descriptive only); pins the recorded
freeze-era paired fields for tamper evidence; validates candidate/fs/
B/P/C/Q/file size/reps/seed/source identity/same-work/content verification/
real-uring, and that each declared `fs_label` matches the filesystem type
actually recorded for the input/output paths. The top-level invocation is a
SIX-ARTIFACT CAMPAIGN SEAL: exactly one tmpfs + one btrfs for A/A, campaign
and control, plus cross-artifact consistency (source bytes, benchmark
binary, seed, matrix constants across the two filesystem sessions).
Mutation tests (per-artifact: drop row, drop cell+resync, drop candidate,
relabel candidate, change file size/source hash/bytes, false verification,
real_uring=false, tamper instructions, tamper one round's R1 with only the
descriptive section re-embedded, tamper normalized ratio/GM/order/A-A
threshold; campaign-seal: drop/duplicate each of the six artifacts,
unknown filesystem, wrong kind, wrong experiment, cross-filesystem source
or binary mismatch, paired-row swap, A/A envelope tamper) must all fail
closed, plus a fixture proving the validator classifies with the PAIRED
median where ratio-of-medians would disagree. (As-frozen rule; implemented
as written by the §13 evidence corrective.)

## 11. Freeze discipline

Design → bench → smoke (real ring + C_near + threadpool) → A/A → freeze
thresholds + matrix → commit → push → DRAFT PR → COPY-AB1-FREEZE SHA → only
then official measurements. Any post-freeze harness semantics change
invalidates affected evidence; refreeze and rerun.

## 12. Non-goals (hard boundaries)

No production R1; no CLI default change; no copy algorithm change; no
production router path change; no EXP-U1; #250/#255 stay governed elsewhere.
The ThreadPool control exists to show the copy harness is stable under a
backend with no Uring router/request_capacity dimension; it is a
harness-stability control, NOT an experimental manipulation of C.

## 13. Evidence corrective (2026-08-31, post-evidence review)

A corrective review of the EVIDENCE ADJUDICATION (not of the measurements)
found that the freeze-era implementation did not compute this design's
as-frozen statistics correctly. The historical sequence is preserved here;
nothing above is rewritten except pointers to this section.

What the original prose CORRECTLY preregistered (unchanged):

- the decision statistic is the PAIRED per-round log-ratio median per cell
  (§8: "report paired median effect");
- the A/A envelope is the max per-cell p90 of |log2 paired ratio|
  (§7: "max over cells");
- the materiality rule `median_log2 <= -max(2 × NOISE[m], log2(1/0.98))`.

What the implementation got wrong (8 findings, fixed in the corrective):

1. Cell materiality was classified from the RATIO-OF-ARM-MEDIANS
   (`log2(median(R1)/median(R0))` — a descriptive statistic) instead of
   the paired median. Root cause: the validator reused the descriptive
   ratio section as the classification input. Fix: the validator
   recomputes per-cell paired effects from raw rows and classifies with
   the paired median; a dedicated fixture proves the two rules can
   disagree and that the paired rule wins.
2. The A/A envelope POOLED all 33 pairs per session before taking p90
   instead of taking max over the three per-cell p90 values. Fix:
   `corrected_envelope()` recomputes per-cell nearest-rank p90 and takes
   the max over cells, then over both official sessions; the frozen
   manifest constants must regenerate from the official A/A raw rows at
   every validation.
3. The p90 order statistic used `int(0.9*n)-1` (biased low) instead of
   the nearest-rank `ceil(0.9*n)-1`. Fix: nearest-rank everywhere the
   envelope or per-cell p90 is computed.
4. The runner's per-cell `paired_effects` aggregate keyed its accumulator
   by (round, cell) but emitted under a round-less cell name, so each
   round overwrote the previous one (recorded A/A per_cell entries all
   carry n_pairs=1). Fix: the helper now collects ALL paired rounds per
   cell. Tooling-only: the corrupt recorded `per_cell` fields are ignored
   (never authoritative) and the raw artifacts stay byte-identical.
5. Validation accepted each artifact independently; the six-artifact
   campaign was not sealed top-level. Fix: the validator requires exactly
   one tmpfs + one btrfs for A/A, campaign and control (exact kind and
   experiment id, no duplicate paths/filesystems), plus cross-artifact
   consistency (source sha256, benchmark binary sha256, seed, file_bytes,
   workers, queue_depth, candidate labels across the two filesystem
   sessions) and fs_label-vs-recorded-filesystem-type checks.
6. The design claimed structural router witnesses (lookup counts, all
   hits, zero control/transport lookups) that the official artifacts do
   NOT record. Fix: docs now state COPY-AB-1 inherits router causality
   from EXP-U0/#254 and the #256 shootout; its role is application-effect
   validation, not a second causal-attribution experiment. No measurement
   was rerun to add fields.
7. The ThreadPool control conclusion ("no capacity-dependent effect
   observed") overstated a session with no C manipulation. Corrected
   wording: the control shows the same copy harness is stable under a
   backend with no Uring router/request_capacity dimension; it is a
   harness-stability control, not an experimental manipulation of C.
8. A/A provenance is a pre-freeze DIRTY research tree (git sha c4f0b4a,
   dirty=true; dirty paths were the research bench/runner/validator and
   artifact output; the benchmark binary sha256 is recorded in the
   artifacts; the production libraries were unchanged). This is a
   provenance LIMITATION and must not be described as a clean-tree
   freeze. Process lesson for future experiments:
   TOOL-FREEZE → A/A CALIBRATION → THRESHOLD-FREEZE → OFFICIAL CAMPAIGN.

Corrective recomputation (from the UNCHANGED raw rows):

- Corrected A/A envelope (max per-cell p90, nearest-rank):
  instr 0.002030 (tmpfs cell 4 KiB/P8/C32; btrfs max 0.0000116),
  cycles 0.145216 (btrfs cell 1 MiB/P8/C512; tmpfs max 0.115284),
  wall 0.398224 (tmpfs cell 4 KiB/P8/C32; btrfs max 0.367874).
  Corrected thresholds (log2): instr 0.029146 (ratio ≤ 0.9800; 2% floor
  dominates), cycles 0.290432 (ratio ≤ 0.8177), wall 0.796448
  (ratio ≤ 0.5758).
- Corrected per-cell classification: instructions 18/27 material
  improvements per filesystem — exactly the 18 capacity-skewed cells
  (C=128: 9/9, C=512: 9/9, C=P+1: 0/9), cycles 6/27 (the
  {4 KiB, 64 KiB} × C=512 cells), wall 0/27, on BOTH filesystems.
  btrfs additionally classifies two near-capacity cells as MATERIAL
  REGRESSIONS under the paired rule (4 KiB/P32/C33 ratio 1.0250,
  64 KiB/P32/C33 ratio 1.0238 vs the 0.9800 bar); on tmpfs the same
  geometry is +1.8%, inside the band.
- RAW DATA RERUN: NO. OFFICIAL ARTIFACTS CHANGED: NO (sha256 verified
  byte-identical before and after the corrective; see the report's
  Evidence Corrective section for the hash record). The headline verdict
  SURVIVES the corrected rule.
