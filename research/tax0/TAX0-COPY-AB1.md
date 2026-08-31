# TAX-0 COPY-AB-1 — Application-Level Copy A/B (R0 vs R1 router)

- Experiment: COPY-AB-1 (task brief; design authority:
  `research/tax0/TAX0-COPY-AB1-DESIGN.md`)
- Base SHA: `c4f0b4aacaa55c5c204287b4d7798533af49535b`
- COPY-AB1-FREEZE SHA: `d428cef` (DRAFT PR #257)
- FINAL HEAD: recorded in the PR after the evidence commit
- Bench: `bench/tax0_copy_ab_bench` (real sluice-copy engine + injected
  backend); runner `scripts/bench/tax0-copy-ab1-run.py`; validator
  `scripts/bench/tax0-copy-ab1-validate.py`
- Host: Fedora 44, kernel 7.1.9, Xeon E5-2666 v3, clang 22.1.8 Release,
  liburing 2.13, `taskset 0,2,4,6` (verified non-SMT physical cores),
  schedutil governor (fingerprint in every artifact).

# 1 Verdict

**COPY-AB-1 PASS — BENEFIT ONLY IN CAPACITY-SKEWED REGIMES.**

The primary metric (instructions / copied byte) improves materially exactly
where EXP-U0's causal model predicts — when `request_capacity` overprovisions
the pipeline's active depth — and the benefit vanishes at near-capacity
sizing. Cycles improve consistently but clear the frozen materiality rule
only in the most skewed cells. Wall throughput is NOT material. Production
R1 is NOT implemented by this task.

# 2 Question

Does the #256-selected R1 reverse-scan router candidate materially improve a
REAL Sluice file-copy workload — the existing `sluice-copy` bounded pipeline
on the REAL `UringAsyncBackend` — when `request_capacity C` exceeds the
actual active I/O depth? Primary: instructions / copied byte. Secondary:
cycles / copied byte. Wall throughput is reported and separately classified.

# 3 Existing sluice-copy architecture

Verified from code at the base SHA (full trace in the design doc):

- The production CLI path (`run_pipelined_copy`) constructs
  `ThreadPoolBackend` and forwards to `run_pipelined_copy_with_backend()` —
  the SAME copy task body (`PipelinedCopyTask::run_body`) the CLI uses.
- `pipeline_depth` P = read-ahead slot count; up to P reads outstanding; at
  most ONE write outstanding (strictly ascending offsets). Code trace +
  smoke: peak of concurrently ACCEPTED backend requests = P.
- `UringConfig` exposes `request_capacity` and `queue_depth` independently;
  C < Q and C > Q are both legal by contract (validation rejects only
  C=0, oversized C, Q=0).
- The R0/R1 modes exist only under `SLUICE_ASYNC_INTERNAL_TESTING`
  (#256 research seam); the production router is untouched.

# 4 Why backend injection is a fair A/B

The research bench calls the public
`run_pipelined_copy_with_backend(...)` — the audited application entry —
with a caller-constructed backend as the ONLY variable: identical copy
algorithm, buffers, worker count (`workers=1`, the CLI default), Runtime
bridge (`run_task_to_result`), deterministic source bytes, and process
placement. R0 = `production_baseline`, R1 = `reverse_scan`, installed via
the #256 seam on the quiescent fresh backend before any runtime drives it.
The bench links `sluice_async_internal_testing`; the production build
compiles none of the seam. ThreadPool control uses `ThreadPoolBackend()`
default construction — the exact production object — and is never mixed
into the R0/R1 aggregates.

# 5 A/A noise calibration

Both labels execute the IDENTICAL `production_baseline` mode; per-round
permutation over (label, cell) entries closes positional bias. 11 paired
reps × 3 cells × {tmpfs, btrfs} (66 rows per session).

| metric  | tmpfs p90 \|d\| | btrfs p90 \|d\| | FROZEN envelope |
|---------|-----------------|-----------------|-----------------|
| instr   | 0.0022          | 0.0000          | 0.002212        |
| cycles  | 0.1168          | 0.1464          | 0.146368        |
| wall    | 0.3987          | 0.4114          | 0.411414        |

FROZEN materiality rule (committed at the freeze, before any R0/R1 run):
metric m improves MATERIALLY iff
`median_log2(R1/R0) <= -max(2 × envelope[m], log2(1/0.98))`:

- instructions: ratio ≤ 0.980 (2% floor dominates)
- cycles: ratio ≤ 0.8164 (2×noise dominates)
- wall: ratio ≤ 0.5653 (2×noise dominates)

The §5 pre-registered 1 GiB amendment was NOT invoked: 1 MiB × 256 MiB
cells proved counter-stable (instr p90 |d| ≈ 0.0000 both sessions).

# 6 Frozen matrix

- File: 256 MiB deterministic source (splitmix64 master block, seed
  `0xE1E1E1E121212121`); sha256 bound per row; Q = 64 everywhere;
  workers 1.
- Cells: {4 KiB, 64 KiB, 1 MiB} × {P=1, 8, 32} × {C_near = P+1, 128, 512}
  × {tmpfs, btrfs} = 27 cells per session; labels {r0, r1}; 9
  blocked-randomized rounds; seed `0x434F5059`. 486 measured rows per
  session (972 total).
- ThreadPool control: B=4 KiB, P=8, both filesystems, 9 rounds each.
- C_near = P+1 justification: code trace (Phase 2 reaps one read before
  submitting the write; peak accepted = P) + smoke (C=P accepted; C<P is
  outside the envelope and hits the pre-existing Phase-3 drain defect —
  design doc §7.1 finding, recorded, NOT fixed here).

# 7 Correctness / same-work

Every measured row proves `bytes_copied == 268435456`, `write_ops == chunks`,
`short_writes == 0`, `chunks ≤ read_ops ≤ chunks + P` (the validator
fail-closes on all of these), and after EVERY measured process an unmeasured
`cmp` full byte comparison of destination vs source passed. No `cmp` failure
occurred in any session. uring rows additionally prove `real_uring == true`
(`SLUICE_HAS_LIBURING` compiled + `available()` verified before handover;
stub results are structurally impossible). VALIDATOR: PASS on all six
official artifacts; self-test 25/25 planted mutations fail closed.

# 8 Raw application results

Artifacts (recomputed-and-verified by the validator):
`tax0-copy-ab1-tmpfs.json`, `tax0-copy-ab1-btrfs.json` (486 rows each),
`tax0-copy-ab1-aa-*.json` (66 rows each), `tax0-copy-ab1-control-*.json`
(9 rows each). read_ops = chunks + 1..P at the EOF window (bounded, as
derived); write_ops == chunks exactly; short_writes == 0 everywhere.

# 9 instructions/byte (PRIMARY)

Overall geometric-mean R1/R0 ratio: **0.8982 (tmpfs) / 0.8886 (btrfs)** —
a ~10–11% instruction reduction, MATERIAL under the frozen rule (≤ 0.980).

- C_near (C=P+1): GM **1.0024 / 1.0078** — no benefit (9/9 cells inside
  noise or marginal regression).
- C=128: GM **0.9381 / 0.9330** — material (9/9 cells IMP across both fs).
- C=512: GM **0.7707 / 0.7462** — large material gains (9/9 cells IMP).
- Material instr improvements: **18/27 cells per filesystem — exactly the
  18 capacity-skewed cells; 0 of 9 near-capacity cells**.
- Paired blocked effect: median log2 −0.0825 (tmpfs) / −0.0806 (btrfs),
  GM ratio 0.898 / 0.889.

# 10 cycles/byte (SECONDARY)

GM ratio **0.9224 (tmpfs) / 0.9200 (btrfs)** — a consistent ~8% reduction
that does NOT clear the frozen cycles rule (≤ 0.8164) at GM level. In the
most skewed small-buffer cells it does: 4 KiB/64 KiB at C=512 reach
0.74–0.79 (material IMP); C=128 and all 1 MiB cells stay inside noise.
Near-capacity cells: exactly 1.00.

# 11 wall throughput

GM ratio **0.9770 (tmpfs) / 0.9927 (btrfs)** — NOT material (rule:
≤ 0.5653). Directionally positive on tmpfs (best skewed cells 0.79–0.85:
4 KiB P=8/P=32 C=512), essentially unchanged on btrfs. Copy throughput on
this warm-cache matrix is dominated by other work (memory traffic /
filesystem path), not by the router tax.

# 12 capacity-ratio effect

The causal prediction is confirmed with a clean dose-response shape
(instr GM per capacity stratum, both fs):

| C stratum | C / active depth | tmpfs instr GM | btrfs instr GM |
|-----------|------------------|----------------|----------------|
| C = P+1   | ~1               | 1.0024         | 1.0078         |
| C = 128   | 4–128            | 0.9381         | 0.9330         |
| C = 512   | 16–512           | 0.7707         | 0.7462         |

Benefit grows monotonically with C skew and disappears at near-capacity
sizing — the application-level signature of the EXP-U0 capacity tax.

# 13 buffer-size effect

instr GM: 4 KiB **0.8752 / 0.8713**, 64 KiB **0.8939 / 0.8758**,
1 MiB **0.9262 / 0.9195** (tmpfs/btrfs). The router tax is per-I/O-op, so
its per-byte weight shrinks as B grows — exactly the pre-registered
prediction (§26 of the design; motivation-only, confirmed as a trend).

# 14 pipeline-depth effect

instr GM: P=1 **0.9011 / 0.8878**, P=8 **0.8881 / 0.8750**, P=32
**0.9055 / 0.9032**. Weak on aggregate — because P interacts with the C
strata (P=32 forces C_near=33, which is noise) — the near-capacity cells
dominate the P=32 stratum's average. The capacity strata (§12), not P alone,
carry the effect.

# 15 filesystem-strata consistency

tmpfs and btrfs agree on every conclusion: same 18/27 material instr cells,
same monotone C-stratum ordering, same buffer dilution, same null wall
result. These are tested-strata consistency checks on ONE host, NOT a
universal-robustness claim.

# 16 ThreadPool control

`ThreadPoolBackend()` (no router, no capacity parameter), B=4 KiB P=8,
9 rounds per fs: instr/byte median 3.078 (tmpfs, rel. sd 0.08%) /
3.097 (btrfs, 0.30%). The harness itself exhibits no capacity-dependent
router effect — it cannot fake one: the production backend has no C
parameter at all.

# 17 What improved

- instructions / copied byte: MATERIAL overall (GM −10.2% tmpfs,
  −11.1% btrfs), and in every capacity-skewed cell (18/18 across both fs).
- Largest single cell: 4 KiB, P=8, C=512 → instr ratio **0.683 / 0.686**
  (~31% fewer instructions per byte).
- cycles / byte: consistent directional reduction (GM −7.8/−8.0%);
  materially improved in the most skewed cells (small buffers × C=512).
- Wall (secondary): tmpfs skewed cells improve up to 15% (0.85), but below
  the frozen materiality bar.

# 18 What did NOT improve

- Near-capacity cells (C = P+1): zero benefit — 1.002–1.008 GM; one cell
  (4 KiB, P=32, C=33) shows a marginal ~2.1–2.5% regression on both fs
  (reverse scan pays a small cost when live entries cluster at low router
  indices — the known R1 near-capacity property from #256).
- cycles / byte at GM level and in C=128/1 MiB strata: inside the frozen
  noise envelope.
- Wall throughput on btrfs: unchanged (0.9927). Copy wall is dominated by
  other work in this warm-cache matrix.

# 19 What is NOT proven

- Nothing about production: R1 is NOT implemented on the production path;
  the CLI default backend remains ThreadPoolBackend; #250/#255 remain open
  under their own governance. Landing implications are a decision for the
  router-fix review, gated on its own compliance work.
- No claim beyond the tested matrix (this host/kernel, tmpfs + warm btrfs,
  these geometries). The optional real-storage sensitivity group was
  SKIPPED (no suitable dedicated persistent-storage setup isolated for this
  campaign) — device-latency-dominated wall behavior is unmeasured.
- The near-capacity marginal regression (~2%) is small enough that its
  practical significance is untested against differently shaped workloads.
- Formal/whole-program claims: none (benchmarks are not proofs).
- The out-of-envelope Phase-3 drain defect (design doc §7.1) was recorded,
  not fixed; it does not touch the frozen envelope (C ≥ P+1 never rejects)
  but it is real and needs its own authorized fix task.

# 20 Production-landing implications

- The R1 candidate's benefit is REAL at the application level, and its
  scope is precisely characterized: it scales with `C / active_depth` and
  vanishes when the arena is sized near the pipeline's actual concurrency.
  This upgrades the #256 conclusion from "microbenchmark-level win" to
  "application-level win, scope-bounded".
- A landing decision should therefore pair the router change with capacity
  SIZING guidance: with C near the real active depth, R1 ≈ R0 (no harm);
  with overprovisioned C, R1 recovers most of the tax. The one measured
  cost is the ~2% near-capacity corner at deep pipelines.
- Copy wall throughput is NOT a landing argument on this evidence.
- Post-landing verification must reuse this A/B harness (it is validated,
  mutation-tested, and reproducible from the frozen seed).
