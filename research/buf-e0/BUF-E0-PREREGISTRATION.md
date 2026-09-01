# BUF-E0 preregistration (FROZEN)

Frozen BEFORE the first formal BUF-E0 measurement session. After freeze, the
arms, primary metrics, matrix, phase definitions, same-work contract, and
promotion criteria below must not change. Design errors discovered later
require a dated `AMENDMENT` section appended to this file recording: what
changed, why, when discovered, whether data already existed, and whether
old/new data remain comparable. The original text never disappears.

Execution authority: issue #263 (roadmap #259; Phase 1 #250 frozen).
Subject: the production per-slot pipeline buffer lifecycle
(`apps/sluice-copy/copy_task.cpp` `PipelineSlot`), per
`BUF-E0-BUFFER-LIFECYCLE.md` / `buf_e0_census.json`.

## 1. Experimental arms (storage representation is the ONLY variable)

All arms provide exactly N writable contiguous bytes to the application
(bench) layer, with equal usable capacity.

| Arm | Mechanism (exact) | Init behavior |
| --- | --- | --- |
| **B0** | `std::vector<std::byte> buffer(N)` — the production representation (reference/semantic baseline) | eager value-init (zero) of all N bytes at construction |
| **B1** | `std::make_unique_for_overwrite<std::byte[]>(N)` (clang 21.1.8 / C++20; verified available) | default-initialization: no eager zero-init |
| **B2** | `mmap(nullptr, N, PROT_READ\|PROT_WRITE, MAP_PRIVATE\|MAP_ANONYMOUS, -1, 0)`; teardown `munmap` | demand-paged zero page COW; no MAP_POPULATE |
| **B3** | `posix_memalign(&p, alignment, N)` with `alignment = 4096` (page size); teardown `free` | no eager init beyond allocator's own metadata |

Recorded per arm/session: exact allocation call, teardown call, page size
(4096), mapping/block size, and (B3) alignment. No allocator framework, no
pooling, no registration — benchmark-only variants.

Arms may differ in allocator metadata overhead; that difference is part of
the object under study.

## 2. Lifecycle phases (three primary + one diagnostic)

### Phase A — allocation → ready

Per rep: construct `slots` fresh buffers of N bytes (the cell's slot count);
stop the clock when construction returns (buffer ready to pass to I/O);
teardown all buffers; repeat. Measures: allocation + arm-specific
initialization + whatever page touching that initialization causes.

### Phase B — allocation → first useful I/O

Per rep: construct `slots` fresh buffers; NO manual prefault; for each slot
perform ONE real `pread(fd, buf, N, off_i)` (first useful I/O — the
production first-read pattern); stop the clock per span (construction span
and first-I/O span recorded separately and summed); verify useful bytes
(hash) OUTSIDE timed spans; teardown. Reported: allocation span, first-I/O
span, total-to-first-useful-I/O.

Primary comparison: `B1 total-to-first-useful-I/O` vs `B0
total-to-first-useful-I/O` (NOT B1-allocation vs B0-allocation). If B1 only
moves page-fault cost into the read span, the verdict records
`FIRST-TOUCH COST SHIFT — NO NET WIN` semantics.

### Phase C — prefaulted steady-state reuse

All arms enter an IDENTICAL residency state first: explicit prefault
protocol = one byte write per 4096-byte page across every buffer (identical
code path for all arms). Then measurement loop: `reps × slots` `pread`s of
N bytes each at rotating offsets within the cell's working window, hash
verification of every read INSIDE the loop (identical across arms — noted as
conservative constant overhead), no allocation or initialization inside the
timed region, same buffer reuse each rep (production reuse semantics).

Primary question: with lifecycle costs removed, does storage representation
still change steady-state I/O?

### Phase D — memory-only first-touch diagnostic (NOT application headline)

Per rep: construct fresh buffers; then deterministic first touch = one byte
write per page (sequential); teardown. Separates VM/page-fault cost from I/O
cost. Diagnostic only; never replaces Phase B and is never quoted as
application performance.

## 3. Matrix (frozen minimum; no pre-expansion)

- Sizes N: `4096` (4 KiB), `65536` (64 KiB), `1048576` (1 MiB) — interpretable
  relative to TAX-0 copy-research granularity.
- Slots: `1, 8, 32, 128`.
- Memory guard: skip (and record `SKIPPED — MEMORY BUDGET`) any cell whose
  `N × slots` exceeds 512 MiB or pushes process RSS above 2 GiB. Expected:
  zero skips (max cell = 1 MiB × 128 = 128 MiB).
- Phases: A, B, C (primary) + D (diagnostic) for every cell × arm.
- Regime: READ primary (production hypothesis: read overwrites
  vector-initialized bytes — census Q3). WRITE is
  `NOT PRIMARY FOR BUF-F01` and is not measured in the formal matrix (no
  production "fill-then-write" initialization-overwrite semantics in the
  copy lifecycle).
- No neighboring-cell expansion before analysis; expansion only via AMENDMENT
  after a threshold is observed.

## 4. Same-work contract (fail-closed, every formal run)

- Data file: generated once per session environment by the TAX-0-line
  generator (4 KiB splitmix64 master block, seed
  `0xE1E1E1E1_21212121`, identical bytes across the campaign).
- File: 256 MiB under `build/bufe0-data` (ext4, WSL2 virtual block device),
  warm page-cache regime (one untimed warm sweep before formal reps; intent:
  warm — this is a memory/lifecycle campaign, not a storage-device study).
- Offsets: deterministic per cell (slot i at `i*N` in Phase B; rotation
  within window `slots*N*K` in Phase C); byte-identical across arms.
- Bytes requested: exactly N per read; every read must return N (offsets are
  interior to the file; short read = semantic failure).
- Verification: FNV-1a hash over each read's N bytes must equal the expected
  hash of the generator bytes at that offset (Phase B: outside timed spans;
  Phase C: inside the loop, identical across arms).
- Per-run gate: total bytes read, read count, and hash set must be identical
  across arms of the same cell/phase; violation fails the process (exit 3)
  and the session is invalid.
- Phase C steady-state reuse count: `K = clamp(256 MiB / (slots*N), 1, 16)`
  per cell (documented denominator; identical across arms).

## 5. Fresh-page regime for Phase A/B/D (allocator pinning — HARNESS MECHANISM FACT)

Problem pinned here: within one process, `free`+`malloc` of the same size can
return the same still-resident block, so eager-init pages do not re-fault and
uninitialized arms inherit resident pages — an allocator-reuse artifact that
would distort fault attribution in BOTH directions.

Regime: at bench startup (before any buffer work) Phase A/B/D processes call
`mallopt(M_MMAP_THRESHOLD, 4096)` (and pin `M_TRIM_THRESHOLD`/`M_TOP_PAD`
defaults by explicit set). Effect: every B0/B1/B3 buffer construction at our
sizes is served by a fresh `mmap` (never-touched pages) and teardown `munmap`
— deterministic fresh-page lifecycle per rep, identical allocator mechanism
across arms.

Consequences recorded up front:
- B0's per-rep fault+zero cost is the COLD-START truth (process start → slots
  touch fresh pages). Production small-buffer arena reuse could only REDUCE
  B0's fault count, so this regime is conservative AGAINST the B0-fine story
  (generous to the B1 hypothesis) — the falsification-friendly direction.
- Absolute per-buffer construction costs for small N are in the
  pinned-mmap regime and are NOT the arena-regime absolute; cross-arm deltas
  (same mechanism, only init differs) remain clean.
- Phase C is unaffected (no per-rep allocation; buffers held for the phase).
- Secondary exploratory probe (labeled `arena-regime`, not primary):
  default-malloc Phase A for {4K, 64K} × slots 8, all arms, quantifying how
  much arena reuse amortizes faults in-process.

Rejected alternatives (recorded): per-rep fresh process (startup noise
dominates on WSL2); `madvise(MADV_DONTNEED)` on allocator blocks (corrupts
glibc heap metadata).

## 6. Metrics (frozen)

Primary:

| Phase | Metrics | Source |
| --- | --- | --- |
| A, B, D | wall ns/buffer (in-process spans), cycles/buffer + instructions/buffer (perf, double-difference), minor faults/buffer (in-process `getrusage(RUSAGE_SELF).ru_minflt` delta; perf `minor-faults` secondary) | bench JSON + `perf stat` |
| C | wall ns/op, cycles/op, instructions/op | same |

- Perf wrapper: `perf stat -x, -e instructions:u,cycles:u` per process
  (R7/R14 pair); marginal per-op = `(total(R14) − total(R7)) / 7 / ops`
  (double-difference, removes process-fixed overhead) — the TAX-0-line
  normalization.
- Secondary (best-effort, may be `UNAVAILABLE/UNRELIABLE` without penalty):
  major faults (`ru_majflt`), RSS/resident delta (`/proc/self/statm` around
  allocation), peak RSS (`ru_maxrss`), `dTLB-load-misses`, `LLC-load-misses`,
  context switches. On WSL2 PMU unreliability: record the fact, no TLB/cache
  stories from unreliable counters.
- Repetitions: formal sessions use R7/R14 process pairs per cell×arm×phase;
  in-process per-rep spans (≥7 useful reps) reported as median + MAD (+IQR)
  with min/max diagnostics. Never best-run vs best-run.
- Denominators pinned per phase: A/D = per buffer and per page; B = per
  first-useful-I/O (per slot) and total-to-first-useful per buffer; C = per
  steady-state op.

## 7. Environment claims discipline

- WSL2 → classification `QUALIFIED_BUT_VIRTUALIZED`; all absolute numbers
  `ENVIRONMENT-LIMITED`; valid for allocation / initialization /
  fault-shifting / same-host causal comparison only.
- Filesystem recorded per session (ext4 on virtual block device; warm cache
  intent). No NAND/FTL/SSD-erase/device-write-amplification claims. No
  NUMA/TLB generalization.

## 8. Application amplifier (after the microbench matrix)

Research-only replica of the production copy lifecycle (verbatim
`PipelinedCopyTask` algorithm; production code untouched), plus the REAL
production engine as external consistency reference:

- arms: `engine-b0` (production `run_pipelined_copy_with_backend`),
  `replica-b0` (replica, vector slots), `replica-b1` (replica, B1 storage).
- cells: {1 MiB × d1, 1 MiB × d8} × {arm}, 512 MiB source file, R7/R14.
- measured: slot-construction span (replica arms), full engine call span
  (all arms: Runtime build/start/submit/wait/drain/join + copy), perf
  double-difference instructions; same-work: `bytes_copied == file size`,
  read_ops/write_ops bounds, post-exit dst hash == src hash (runner-side).
- The amplifier arm pairing is pinned NOW as B0 vs B1. If (and only if)
  Phase A/B show a different arm materially better than B1 on
  total-to-first-useful-I/O, an AMENDMENT swaps `replica-b1` for that arm
  with the recorded reason; otherwise no swap.
- Verdict rule: if the microbench winner disappears under the realistic slot
  lifecycle, the final verdict FOLLOWS THE AMPLIFIER.

## 9. Required causal questions (Q1–Q8 of #263, answered in the report)

Q1 eager-init construction cost · Q2 cost shift to first touch/I/O · Q3 best
arm total-to-first-useful-I/O · Q4 steady-state representation effect · Q5
scaling (bytes/pages/slots/allocator objects) · Q6 reuse amortization · Q7
per-slot ownership steady-state penalty (BUF-F02 key) · Q8 keep
`std::vector<std::byte>` (YES allowed).

## 10. Materiality and verdicts (no arbitrary thresholds)

No pre-registered percentage/ns thresholds. Materiality weighs: effect vs
run-to-run dispersion (median±MAD separation), neighboring-cell consistency,
absolute CPU/time cost, realistic reuse count (census Q2), end-to-end
lifecycle share (amplifier). A statistically stable few-ns setup difference
is NOT automatically architecture-worthy.

Final verdict — exactly one primary term from:

```
NO MATERIAL BUFFER TAX FOUND
EAGER-INITIALIZATION LIFECYCLE TAX ONLY
FIRST-TOUCH COST SHIFT — NO NET WIN
PER-SLOT OWNERSHIP COST MEASURED
STEADY-STATE STORAGE EFFECT MEASURED
MIXED — NEED TARGETED CAUSAL FOLLOW-UP
ENVIRONMENT-BLOCKED
```

then `PHASE 3 AUTHORIZED: YES/NO`. Phase 3 (resource boundary / minimal
BufferStorage experiment) requires Gate A (repeatable, material,
mechanism-attributed, not merely shifted cost) or Gate B (a named upcoming
capability — stable storage identity / alignment / registration lifetime /
external ownership / pool reuse authority — provably impossible to express
fairly under the current ownership model). "vector feels inflexible" and
"future fixed-buffer convenience" are not evidence. If NO: keep current
production `std::vector<std::byte>` representation (YES expected answer
shape allowed).

## 11. Sessions and immutability

Formal sessions live under `research/buf-e0/results/<session-id>/` with
`environment.json` (timestamp, git HEAD/branch/dirty, binary sha256, system,
tools), `manifest.json` (protocol + cells), `commands.md`, `notes.md`,
`raw/` (per-run bench JSON + perf output), `summary.csv`/`summary.json`.
Written once; never edited after the session closes. Re-runs = new session
id. New session id convention: `bufe0-<phase|amp>-wsl2-<n>`.

## 12. Production guard

`PRODUCTION CODE CHANGED: NO`. All arms are research-only bench code. If the
harness proves unable to express the production lifecycle, that is a reported
BLOCKER, not a production change. Draft research PR only; DO NOT MERGE; stop
for adversarial review after the verdict.

---

## AMENDMENT 1 — 2026-09-01, after formal session bufe0-micro-wsl2-1

What changed:

1. **Diagnostic arm b1a added** (B1 + page alignment: allocate
   `make_unique_for_overwrite<std::byte[]>(N + 4096)`, expose the pointer
   rounded up to 4096; N usable bytes; teardown frees the original
   allocation). Measured in a separate follow-up session (phases B and C,
   identical protocol/matrix/runner) as arm `b1a` alongside `b1/b2/b3`.

2. **Amplifier alternative arm extended** per §8's own swap rule: Phase B
   formal data showed arms materially better than B1 on
   total-to-first-useful-I/O at 1 MiB (B2/B3 ~25-30% lower), so the
   amplifier runs `replica-b3` (posix_memalign page-aligned storage, the
   owned/glibc-family representation closest to production semantics)
   alongside the originally pinned `replica-b1`. Both run in the same
   amplifier session; the §8 pairing (B0 vs B1) remains present.

3. **cycles:u demoted to UNRELIABLE on this host** (metric-availability
   note, not a design change): 5/192 cells show negative R7/R14
   double-differences (virtualized cycle counter non-monotonicity).
   `instructions:u` (1/192 anomalous) and in-process wall spans remain the
   quantitative pair. Recorded per the environment-availability rules.

Why: Phase C of the formal session showed the Outcome-D pattern —
B0/B1 (glibc chunk pointer at +16, not page-aligned) are 2.5-5x slower
than B2/B3 (page-aligned) in prefaulted steady-state reads, consistently
across sizes and slot counts. Prereg §Outcome-D requires proving the
mechanism (alignment? allocator placement? TLB? cache? mapping?) before
interpreting; b1a isolates page alignment with the allocation family held
constant (B1's exact mechanism + pointer shift only).

When discovered: after closing formal session bufe0-micro-wsl2-1 (all
gates green), during first report generation.

Whether data already existed: yes — the 4-arm formal session is complete
and unchanged. b1a/b3-amplifier data did not exist when the pattern was
found.

Comparability: the original 4-arm cells are not re-measured or replaced;
b1a is an added arm under the identical protocol (same runner, same
R7/R14 normalization, same same-work gates, same data file), so cross-arm
comparison b1 vs b1a is same-session-family and directly interpretable.
The amplifier comparison happens wholly within its own session.
