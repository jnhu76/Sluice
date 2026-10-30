# #593 — Native critical-path discovery and mechanism elision (E5)

Issue: `jnhu76/Sluice#593` (PocketJS-inspired execution-path slicing), with
#563 (minimal necessary work), #591 (hardware-aware workload benchmark) and
#580 (CQE router lookup, closed `NO_TRIGGER_NO_CHANGE`) as context.

This branch is a **research record intended as an open research PR**. It is not
merged, and the PR was not opened in this run (push/PR creation is a separate
step). `PRODUCT_CHANGE: NONE`. The diff against the frozen base
`bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57` touches only
`docs/research/key-path-593/` and `bench/research/key-path-593/`.
No `src/`, `include/`, spec, test or build file changes are proposed. Every
performance number below was produced by a test-only research arm in an
isolated worktree; nothing here is authorized for merge.

## 1. Question

The campaign asks which mechanisms sit on the real request-completion critical
path, which of them perform no semantic obligation, and whether removing work
beats accelerating it. Four lanes were mapped, attributed, counterfactually
tested and measured natively:

| Lane | Segment |
|---|---|
| K1 | CQE → terminal → publication → observer → `take` |
| K2 | `RequestScope` submit → admission → dispatch → kernel SQE |
| K3 | progress pass → wakeup → bounded retry |
| K4 | SQE/CQE batching and the kernel boundary |

## 2. Frozen base and environment

Everything in this PR is relative to one frozen base and one re-verified host.
The environment was re-observed on the host before the first run; no value is
carried over from earlier campaigns as current fact.

| Item | Value |
|---|---|
| Base SHA | `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57` (master) |
| Host | E5, bare metal, native |
| CPU | Intel Xeon E5-2666 v3 @ 2.90 GHz (Haswell-EP), 1 socket, 10 cores, SMT2, 20 logical CPUs, 1 NUMA node |
| Cache | L1d 32 KiB/core, L2 256 KiB/core, L3 25 MiB shared |
| Kernel | `7.2.11-200.fc44.x86_64` (Fedora 44) |
| Toolchain | gcc 16.2.1, glibc 2.43, liburing 2.13, xmake 3.1.2 |
| Frequency | `schedutil`, min 1200 / max 3500 MHz, turbo enabled; time-varying, never modified |
| Pinning | all measured runs `taskset -c 2`; cpu2 SMT sibling cpu12 **not** isolated |
| Storage | warm tmpfs `/tmp` (page-cache-resident 64 MiB fixture); a btrfs/page-cache arm was run but **device I/O is not measured** |
| io_uring | enabled; capability probe OK (`QUEUE_INIT_OK entries=64`, read/write/fsync/statx/cancel/read_fixed/poll_add/timeout/files_update) |
| PMU | `cycles:u`/`instructions:u`/cache/branch/LLC/dTLB available; `cycles:k` and tracepoints blocked at `perf_event_paranoid=2` and obtained only through transient `sudo` under the Owner authorization recorded in the brief addenda; `perf_event_paranoid=2`, `kptr_restrict=0`, `io_uring_disabled=0` were read identical before and after every privileged block |
| Baseline binaries | w02 `e07a1bd2503e1ff88bd653c520c7ca491dac41249e139a2b3199780efbdbf484`, w03 `80992f4bc366600375bd6846182dff96409e6a3f4ab819e04668236a3f41a3ab` |

No CPU governor, `drop_caches`, sysctl or security setting was changed. The
full freeze is `evidence/environment.md` in the research workspace; the
identity is reproduced in [evidence-index.md](evidence-index.md).

## 3. Workflow executed

`Freeze → four-lane source mapping → native attribution → hypothesis formation
→ counterfactuals → native A/B → cross-path synthesis → independent review →
research PR` (brief §0). Concretely:

1. **Freeze** — worktree at the base SHA, both bench profiles built, binary
   hashes recorded, A/A calibration rounds r1/r2/r3.
2. **Mapping** — full call/synchronization DAG for K1–K4 from source, with
   authority and obligation annotations (`execution-dag.md`).
3. **Attribution** — a separate `SLUICE_OBSERVE` binary (66 counters, per-pass
   trace) for mechanism frequency, plus full-binary `perf stat`/`perf record`/
   `perf sched`, tracepoint syscall counts and `strace`. Instrumentation costs
   +0.56 %/+1.3 %/+1.5 % CPU/op when disabled and +14.65 %/+3.31 %/+15.35 %
   when enabled (W-02 d16 / W-02 d1 / W-03 d16). Frequency comes from the
   observe binary; **cost always comes from the full binary**
   (`mechanism-ledger.md`).
4. **Counterfactuals** — one test-only arm per lane, each with a falsifier
   stated in the lane design before the A/B and a mutation receipt
   (`counterfactuals.md`, [correctness-review.md](correctness-review.md)).
5. **Native A/B** — interleaved AB/BA, 15 pairs per case, 2 warmup + 7
   measured, `SLUICE_BENCH_STATS=1`, `taskset -c 2`, fresh process, single-case
   filter. Every run is validated by the frozen G0 checker (`bench/check_run.py`)
   and the per-repetition 64 MiB byte oracle. Valid runs: K2-a 390/390,
   K4-b 390/390, combo **210/210 (of which 60 are the increment comparison)**.
6. **Synthesis** — [cross-path-interactions.md](cross-path-interactions.md),
   [opportunity-ranking.md](opportunity-ranking.md),
   [decision.md](decision.md).

## 4. Headline findings

**One material mechanism exists, and K2-a and K4-b are two gates on the same
line of code.** `UringAsyncBackend::dispatch_after_accept` performs
`submit_transport_locked()` inline (`src/async/uring_transport.cpp:87`), so
every operation pays exactly one `io_uring_enter` with exactly one SQE at every
depth (1.00007 enters/op, `sq_ready = 1.00` in all census arms; strace
1.00005). Removing or delaying that inline flush is the only change in the
campaign that both removes real work and reproduces end to end.

- **K2-a** deletes the inline flush unconditionally. Same-thread W-02 4 KiB:
  CPU/op −32.20 % (d16), −37.66 % (d64), −26.31 % (d4). But D=1 regresses
  +14.12 % CPU with p50 ×14.1, W-03 d16 p99 regresses +15.70 %, W-03 d4 p50
  +26.42 %, and 64 KiB p99 +6.77 %. It is a measured upper bound on the
  mechanism, **not** a deployable candidate.
- **K4-b** gates the same elision on "the submitter also drives progress" and
  `outstanding_summary() > 1`. It retains the same-thread gain (d16 −28.07 %,
  d64 −35.53 %, d4 −13.06 % CPU/op; p50/p95/p99 improve) and keeps D=1 and all
  W-03 forms on the inline path (enters/op 1.00007 unchanged). The claim is
  **measured at D=4/16/64**; D=2/D=3 were not run and the observed ≈2/D enter
  pattern predicts no enter reduction at D=2. Remaining deviations: 64 KiB p99
  +7.46 % (material, no payload gate yet), W-03 d4 +2.54 % (below the campaign
  4.79 % band and inside the only measured d4 A/A; unattributed), and d1
  −2.50 % (not explainable by the gate; treated as clock/layout).
- **K1(a+b)** genuinely removes 3.098 mutex acquisitions/op (3.209 → 0.111 in
  W-03 d16; W-02 trace `published == reaped` on 70,061 passes, zero violations;
  W-03 has aggregate push/pop parity only) and reproduces −2.29 % CPU
  (−2.31 % wall) in W-03 d16 on 15/15 pairs. The effect is below the campaign
  4.79 % band but above the W-03-d16 session band; the verdict is
  threshold-dependent (see [measurement-review.md](measurement-review.md) §4).
  **No change justified under the campaign band; `INCONCLUSIVE` if it is
  rejected.**
- **K3** is dormant or amortized in the measured domain: W-02 parks zero times
  in 540,000 ops, user-space signals are 0/op, W-03 `empty_wakes = 0.0000`,
  and passes/op fall 1.34 → 0.030 from D=1 to D=64. The K3-a same-lock fusion
  measured −1.00 % (9/15, inside the A/A band) at the largest-signal shape.
  **No change justified.**
- **K4-a** (batch reap lock domain, batch `cq_advance`) was not built: all
  mutex symbols together are 3.4 % of cycles (~370 cycles/op), so the per-CQE
  lock pair is below the band. **The `cqe_seen` khead store is not a mutex and
  is not covered by that bound**, so this is a partial upper bound, not a
  complete proof. **No change justified** on the mutex evidence plus K4-b's
  W-03 result locating the cost in the kernel/driver.

**The combination was measured, and it is not additive.** Merging K4-b + K1(a+b)
into one worktree reproduces the K4-b W-02 effect (−27.87 % d16, −36.13 % d64)
and closes W-03 at −2.57 % against baseline. The increment of the combination
over K4-b alone is **+0.31 %** CPU on W-02 d16 (i.e. no gain, slightly worse)
and **−2.97 %** on W-03 d16. Each component keeps its own topology; the two
percentages are never summed. Under the campaign's "each component
independently crosses 4.79 %" gate there is no qualified combination, so the
paired classification is `MECHANISM_GAIN` (brief §9 vocabulary) and
`MECHANISM_ONLY_GAIN` in the §13 verdict vocabulary. The gate's provenance is
disclosed in [measurement-review.md](measurement-review.md) §4.

**One systemic gap: form dependence.** The sign of the same mechanism is set by
three axes — progress-owner topology (same thread vs split owner), depth (D=1
vs D≥4; D=2/D=3 unmeasured) and payload (4 KiB vs 64 KiB). K4-b implements the
first two gates; a payload gate is not implemented and has no measured arm.
W-03 d4's +2.54 % has no mechanism explanation yet but is sub-threshold and
inside the only measured d4 A/A. The payload regression and the missing
D=2/D=3 points are productization blockers, not correctness problems: both
flush choices are semantically complete.

## 5. Verdicts (brief §13 vocabulary)

| Lane | Candidate | Verdict |
|---|---|---|
| K1 | K1-a pass-snapshot publication drain + K1-b no-observer delivery gate | `NO_CHANGE_JUSTIFIED` under the campaign 4.79 % band; `INCONCLUSIVE` if the band is rejected |
| K2 | K2-a ungated deferred kernel submission | `ELISION_CANDIDATE_FOUND` (paired `END_TO_END_GAIN` in the same-thread forms); ungated arm not deployable; same mechanism as K4-b, supported verdict counted once |
| K3 | K3-a pass-tail same-lock fusion; K3-b drive-gate decoupling | `NO_CHANGE_JUSTIFIED` (both withdrawn) |
| K4 | K4-b topology+depth-gated deferred transport flush | `ELISION_PERF_SUPPORTED` (form-gated, measured D=4/16/64); K4-a `NO_CHANGE_JUSTIFIED` (no arm, partial bound) |
| combo | K4-b + K1(a+b) | `MECHANISM_ONLY_GAIN`; measured non-additive composition, not a candidate |

`ELISION_PERF_SUPPORTED` explicitly does **not** authorize a product change
(brief §13). The productization preconditions that remain open are listed in
[decision.md](decision.md) §4.

## 6. Reading order

| File | Content |
|---|---|
| [README.md](README.md) | scope, environment, findings, verdicts |
| [execution-dag.md](execution-dag.md) | end-to-end execution and synchronization DAG, edge by edge |
| [critical-path-atlas.md](critical-path-atlas.md) | HOT / CRITICAL / WAIT / AVOIDABLE / UNAVOIDABLE / OVERLAPPED / MOVED classification |
| [mechanism-ledger.md](mechanism-ledger.md) | per-operation frequencies, locks, state, bytes, cycles, and the measurement corrections |
| [k1-completion-publication.md](k1-completion-publication.md) | K1 lane report |
| [k2-submit-dispatch.md](k2-submit-dispatch.md) | K2 lane report |
| [k3-progress-wakeup.md](k3-progress-wakeup.md) | K3 lane report |
| [k4-sqe-cqe-batching.md](k4-sqe-cqe-batching.md) | K4 lane report |
| [counterfactuals.md](counterfactuals.md) | competing explanations, arms, falsifiers, outcomes |
| [cross-path-interactions.md](cross-path-interactions.md) | the four coupling channels and the measured composition |
| [opportunity-ranking.md](opportunity-ranking.md) | unified eight-column table and eight-dimension ranking |
| [correctness-review.md](correctness-review.md) | obligations, mutation receipts, what this reviewer re-checked |
| [measurement-review.md](measurement-review.md) | protocol, A/A, PMU boundaries, statistics, defects found |
| [decision.md](decision.md) | final decision, productization preconditions, embedded final block |
| [evidence-index.md](evidence-index.md) | persistent evidence paths, SHA-256, storage disclosure |

Harness and the complete rebuildable patches:
`bench/research/key-path-593/` (see its `README.md`).

## 7. Reproducing

```sh
# fresh worktree at the frozen base
git worktree add /tmp/kp593 bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57
cd /tmp/kp593

# baseline
xmake f -y --mode=release --kind=static --liburing=y -o build-bench-ring -c
xmake -y -j16 bench_w02_pipeline bench_w03_external_loop

# an arm, e.g. K4-b
git apply /path/to/bench/research/key-path-593/patches/k4b-final.diff
xmake -y -j16 bench_w02_pipeline bench_w03_external_loop

# paired run (arm binaries and paths are at the top of the script)
bench/research/key-path-593/harness/pair-run-k4b.sh W02-uring-4k-d16 bench_w02_pipeline 15 elide-vs-full w02_uring_4k_d16
```

The archived scripts carry the absolute paths of the research workspace from
which they were run; they are one-shot experiment provenance, not product
tooling, and are not wired into `xmake` or CI. Rebuilt binaries will not hash
identically to the archived ones; the archived identities are the record of
what was measured.

## 8. Boundaries and evidence preservation

- **tmpfs only.** Page-cache-resident 64 MiB fixture, no device I/O, no NVMe or
  HDD claim, no #578 real-application claim. The wait/park machinery is dormant
  precisely because completion is a memory copy; on a real device the K3 paths
  would activate and the K4 same-thread absorption assumption must be re-tested.
- **One host, one topology, one kernel.** The K4 gain contains kernel-side
  amortization (io-wq scheduling, spinlocks, batched eventfd flush) and a
  same-logical-CPU worker. Multi-socket/NUMA and other kernels must re-measure.
- **PMU scope.** A/B and census sessions are user-space PMU only
  (`perf_event_paranoid=2`). Kernel attribution is inherited from the transient
  `sudo` sessions and is labelled as such where used.
- **No cancellation/control load.** Every census arm has `control_cqe = 0`,
  control scan = 0, reconcile = 0, `get_events = 0`. Deferral under a cancel
  storm is unmeasured; the poison/false-eligibility mutations cover windows,
  not load-level cancellation.
- **Saturate latency not instrumented** (`lat_n = 0`); its p50/p99 are reported
  as "not instrumented", not as zero change.
- **Raw evidence is not in this PR.** 1.4 GB / 6,870 files live at
  `/home/jnhu/sluice-593-research/evidence/` on the E5 host. It is a single
  persistent copy, not replicated and not in CI artifact storage. Paths and
  SHA-256 are recorded for summaries, patches and identities; the per-rep JSONL
  and perf `.data` are **not hash-anchored**. A reader without access to that
  host cannot retrieve the raw data. `EVIDENCE_PRESERVATION: PARTIAL` — see
  [evidence-index.md](evidence-index.md) §7.
- **Preservation update (2026-10-30).** The cross-host preservation state of
  the raw evidence and this record's closeout disposition are recorded in
  [EVIDENCE_PRESERVATION_SUPPLEMENT_20261030.md](EVIDENCE_PRESERVATION_SUPPLEMENT_20261030.md);
  the `EVIDENCE_PRESERVATION: PARTIAL` statement above remains the historical
  status at measurement time.
- The 4.79 % campaign band's provenance and the fact that it is not defined in
  the brief are disclosed in [measurement-review.md](measurement-review.md) §4;
  the K1 and combo verdicts are threshold-dependent and say so.
- One measurement label defect is disclosed and accounted for: a census summary
  header and one pair-runner banner carry a stale "K2-a" label; the K4 pair
  runner's `git_var` path points at the K2-a worktree (same base SHA)
  (`measurement-review.md` §9).

## 9. Authorization

This branch is prepared as an **open research PR record**. It is not merged and
the PR was **not opened in this run** (push and PR creation are a separate
step); `PR_STATUS: OPEN_NOT_MERGED` is the required brief §14 field value and
must hold once the PR is opened. It does not modify production code and does
not create product issues. `OWNER_DECISION: PENDING`.
