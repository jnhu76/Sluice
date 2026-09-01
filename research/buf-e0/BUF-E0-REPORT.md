# BUF-E0 final report — Phase 2 buffer truth (#263)

```text
VERDICT: STEADY-STATE ALIGNMENT EFFECT MEASURED — user-buffer address
         alignment causally isolated; exact alignment threshold and
         kernel micro-mechanism unresolved. With BUF-F01 eager-init
         resolved as FIRST-TOUCH COST SHIFT / NOT MATERIAL and BUF-F02
         NOT PROVEN.
CONFIDENCE:
  HIGH:   alignment is a causal performance variable in the tested
          WSL2 regime (three sessions, dedicated b1a isolation arm);
          BUF-F01 eager initialization is not material (fault
          conservation + amplifier + arena probe double-bound).
  MEDIUM: magnitudes and generalization beyond this host/regime
          (all absolute numbers WSL2-bound).
  UNRESOLVED: minimum effective alignment (only natural-unaligned vs
          4096-byte alignment was compared); exact kernel/uaccess
          micro-mechanism; standalone WRITE effect; why depth 8
          eliminates the end-to-end benefit.
ENVIRONMENT: WSL2 (kernel 6.18.33.2-microsoft-standard-WSL2, AMD Ryzen 7
         5800H, 8 CPUs, 16 GiB, ext4 on virtual block device, warm page
         cache) — QUALIFIED_BUT_VIRTUALIZED; all absolute numbers
         ENVIRONMENT-LIMITED; same-host causal comparison only. No
         native-NVMe/NUMA/TLB claims.

BASE: 2a8dd7995a606882f1bd42ca264aebc80ce1726b (origin/master)
HEAD: final commit of research/buf-e0-buffer-truth (sessions recorded
      clean-tree states in their environment.json)
BRANCH: research/buf-e0-buffer-truth
PR: #264 (adversarial claim-hygiene closure applied; see the EVIDENCE
    TAXONOMY section for the claim boundaries)
EXECUTION ISSUE: #263 (milestones BUF-E0A/E0B/E0C/VERDICT posted there;
    a claim-boundary clarification is appended there)

PRODUCTION CODE CHANGED: NO
```

Sessions (immutable, under `results/`):

| session | content | gates |
| --- | --- | --- |
| `bufe0-micro-wsl2-1` | 4 arms x phases A/B/C/D x 12 cells x R7/R14 (384 runs) | 0 errors, 0 regime violations |
| `bufe0-align-wsl2-1` | AMENDMENT 1: b1a alignment diagnostic, phases B/C (192 runs) | 0 errors |
| `bufe0-amp-wsl2-1` | application amplifier, 512 MiB real copies (16 runs) | all hash-verified |
| `bufe0-arena-wsl2-1` | secondary arena-regime probe (16 runs) | 0 errors |

Metric note (AMENDMENT 1): `cycles:u` UNRELIABLE on this host (5/192
negative double-differences); primary pair = in-process wall
(median±MAD, 14 reps) + `instructions:u` double-difference
(R14−R7)/7/ops.

==================================================
CURRENT LIFECYCLE (census: CODE FACT, `buf_e0_census.json`)
==================================================

```text
allocation frequency:     setup-only — all slots built once per copy
                          operation, BEFORE the Runtime starts
slot reuse:               unbounded in-operation; ≈ file_bytes /
                          (buffer_size × depth) chunk cycles per slot
                          (512 MiB @ 1 MiB×d8 → 64 reuses/slot;
                          @ d1 → 512)
buffer overwrite:         full chunks are fully overwritten by pread
                          ([0,N)); EOF tail overwrites [0,filled) only
                          and the write side never reads past filled —
                          every zero-initialized byte a full chunk
                          carries is dead data after its first read
steady-state allocation:  none in the copy task loops; backend has
                          fixed workers + bounded dispatch storage
capacity/depth:           capacity == active in-flight read depth
                          (all depth reads outstanding in steady state;
                          single outstanding write)
resize/growth:            none (fixed buffer at construction;
                          slots pointer-vector is reserve(depth))
```

==================================================
BUF-E0 RESULTS (representative cells; full data in summary.csv)
==================================================

PHASE A — allocation → ready (ns/buffer, slots=8)

```text
          b0 vector   b1 uninit   b2 mmap    b3 aligned
4 KiB       5231        3322       1423        3282
64 KiB     26928        3549       1383        3157
1 MiB     445061        4191       1557        4589
```

dominant mechanism: b0 cost = eager zero-init faulting fresh pages
(~0.4 ns/byte + 1.5-1.7 µs/page); b1/b3 cost = one mmap syscall +
chunk metadata (≈ constant per object); b2 = one mmap syscall.

--------------------------------------------------

PHASE B — allocation → first useful I/O (ns/buffer total; faults conserved)

```text
4 KiB (all slot counts):  b0 6288-7359 ±130-371   b1 6518-7725 ±130-496
                          → no material separation at any slot count
                            relative to observed run-to-run dispersion
                            (medians inside each other's MAD; this is
                            not a formal hypothesis test)
64 KiB s8:                b0 48111±1770  b1 49514±4107  b2 28848±2422
                          b3 33100±3571 → b0 ≈ b1
1 MiB s8:                 b0 865694±33085  b1 773273±11616  b2 555591±26290
                          b3 525501±15248 → b1 ~11% < b0; b2/b3 ~35-39% <
                          b0
faults (all cells):       b0 == b1 == b3 totals (2/17/257 per buffer at
                          4K/64K/1M); only LOCATION differs — b0 pays in
                          the alloc span, b1/b3 in the first-I/O span
                          (b2 one fewer: no malloc chunk header)
```

cost disappeared or shifted: SHIFTED, precisely. b0's eager init
prefaults pages at construction (Phase D: b0 first-touch 5-30 ns/page on
resident pages vs 1.4-2.2 µs/page real demand faults for b1/b2/b3). At
4K/64K the shift nets to zero; at 1M it nets to ~10% for b1 (the b2/b3
advantage here is mostly the ALIGNMENT effect below, not initialization).

--------------------------------------------------

PHASE C — prefaulted steady-state (ns/op, identical prefault protocol)

```text
              b0        b1        b2        b3       material b0/b1 vs b2/b3
4 KiB  s1   1654      1640       669       633      ~2.6x
4 KiB  s128 1798      1958       968       881      ~2.0x
64 KiB s1   20808     20915      4725      4144     ~4.7x
64 KiB s128 25620     26727      9141     10859     ~2.5x
1 MiB  s1   392142    420283    113169    120092    ~3.4x
1 MiB  s128 421573    445115    204578    204404    ~2.1x
```

material difference: YES — but NOT along the preregistered
representation axis. b0 == b1 everywhere (ownership/initialization
representation does not change steady state). The difference tracks
buffer ALIGNMENT: b2/b3 are 4096-byte aligned; b0/b1 pointers sit at
+16 (glibc chunk offset).

CAUSAL ISOLATION (AMENDMENT 1, session bufe0-align-wsl2-1): arm b1a =
b1's exact allocation mechanism with the pointer rounded up to 4096-byte
alignment. b1a recovers essentially the entire gap (64K s1: 24198 →
4975 vs b2 4542; 1M s1: 437015 → 124752 vs b2 120551; 10 of 12 cells
full recovery). This isolates user-buffer address alignment as the
dominant causal variable at the userspace boundary, achievable within
owned-allocation semantics (posix_memalign / over-allocate+align). It
is a per-op steady-state cost — NOT cost shifting, NOT vector-vs-mmap.

Mechanism boundary: the campaign compared a natural unaligned pointer
against a 4096-byte-aligned pointer and measured a material change. It
did NOT identify which kernel/uaccess copy-path branch causes the
difference, and it did NOT sweep intermediate alignments — 4096-byte
alignment was the tested effective alignment; the minimum effective
alignment threshold remains unknown.

==================================================
SCALING
==================================================

```text
bytes:        b0 alloc ~linear in bytes (0.41-0.43 ns/byte across
              4K-1M); b1/b2/b3 ~constant per object (allocator-object
              bound)
pages:        faults scale exactly with pages (b0: 2/17/257 per buffer
              at 4K/64K/1M — metadata page + init-touched pages)
slot count:   no pathological growth: Phase A per-buffer flat across
              1/8/32/128; Phase C per-op flat within size (one noisy
              cell: 1M s32 b0 1124 vs 480 at s8 — dispersion noted, not
              load-bearing for any conclusion)
allocator     b1/b3 construct ~constant per object regardless of slot
objects:      count; b2 same (mmap syscall bound). The formal session's
              per-buffer construction differences do NOT grow with the
              number of slots.
```

==================================================
APPLICATION AMPLIFIER (512 MiB real copy, production engine vs replicas)
==================================================

```text
depth 1 (production CLI default):
  engine-b0 (production):      486.4 ms/copy
  replica-b0 (vector):         467.0 ms/copy   (fidelity: engine≈replica)
  replica-b1 (uninitialized):  472.0 ms/copy   → NO benefit vs b0
  replica-b3 (4096-aligned):   260.4 ms/copy   → 1.8x faster

depth 8:
  engine-b0: 234.8 ms   replica-b0: 226.9 ms   replica-b1: 257.3 ms
  replica-b3: 231.5 ms  → no material difference at d8 on this host

direction boundary: the alignment effect is directly measured on the
  READ path (standalone microbench) and survives this real READ+WRITE
  copy amplifier at depth 1. WRITE was NOT independently benchmarked;
  do not read an independent WRITE per-op magnitude out of the
  aggregate amplifier result.

verdict: the F01 (uninitialized construction) microbench saving does
not survive the realistic lifecycle (construction is 0.02-1.9% of one
copy and amortizes over census reuse). The alignment effect is
REGIME-SPECIFIC: material and amplified at the production default
depth 1, absent at depth 8 (mechanism of the disappearance unresolved).
Consistency cross-check (not an independent causal proof): applying the
observed READ per-op alignment magnitude to the full copy operation
count — per-op penalty × (512 reads + 512 writes) ≈ 207 ms — predicts
an end-to-end delta close to the observed replica-b0 − replica-b3 d1
gap (207 ms). Because WRITE was not independently benchmarked, this is
supporting consistency evidence, not a mechanism confirmation. Per
prereg §22 the amplifier governs.
```

==================================================
FINDINGS
==================================================

BUF-F01 eager initialization (vector value-init):

```text
CODE FACT:  std::vector<std::byte>(N) eagerly zero-initializes all N
            bytes at construction (copy_task.cpp:74), touching every
            page; full-chunk reads overwrite all of it afterwards.
COST VERDICT: FIRST-TOUCH COST SHIFT — NOT MATERIAL. The construction
            cost is real (Phase A) but Phase B shows it reappears as
            first-I/O faults in every alternative arm (fault totals
            conserved); totals are equal at 4K/64K, ~10% for b1 at 1M
            in a conservative fresh-page regime; the arena probe shows
            a long-running process shrinks it to pure memset
            (60-760 ns/buffer); the amplifier shows zero
            application-level benefit (replica-b1 == replica-b0 at both
            depths). DO NOT REFACTOR for this.
```

BUF-F02 per-slot ownership/storage policy:

```text
CODE FACT:  each PipelineSlot owns one fixed buffer, constructed once
            per copy operation, reused unboundedly within it; no
            resize/growth; no steady-state allocation.
COST VERDICT: NOT PROVEN. Per-buffer lifecycle cost does not grow with
            slot count (scaling section); steady state is identical
            across ownership representations once alignment is
            controlled (b0==b1; b3 achieves the fast steady state AS an
            ordinary owned per-slot allocation — no pool, no lease, no
            shared storage). "难以未来注册 fixed buffer" remains a
            capability question (Gate B), not performance evidence.
```

NEW (within BUF-E0's storage-representation scope; causally isolated):

```text
BUFFER ADDRESS ALIGNMENT: production slot buffers are never page-aligned
  (glibc chunk pointers at +16; arena-carved small buffers worse).
  Changing only the exposed pointer alignment — within the same
  allocation family and ownership semantics — removes most of the
  measured per-op steady-state gap (b1a). Direct evidence: READ
  standalone, 2-4.7x per-op in the microbench; end-to-end 1.8x at the
  production-default pipeline depth 1, with NO material effect at depth
  8 on this host. Recoverable by pointer alignment alone (b1a) and by
  posix_memalign (b3) within per-slot owned semantics. 4096-byte
  alignment was the tested effective alignment; the minimum effective
  threshold and the exact kernel/uaccess micro-mechanism are unresolved.
  This is the ONE measured, material, non-shifted storage cost BUF-E0
  found.
```

==================================================
NEGATIVE RESULTS
==================================================

- Uninitialized construction (B1) does NOT improve total-to-first-I/O at
  4K/64K (cost shift only) and does not survive the amplifier anywhere.
- mmap-as-representation (B2) has no steady-state advantage over an
  aligned owned allocation (B3 ≈ B2); its Phase A/B wins are alignment +
  syscall-count effects, not mapping semantics.
- No slot-count amplification of any lifecycle cost (Outcome C not
  observed).
- WRITE regime: not measured (NOT PRIMARY FOR BUF-F01 — no
  fill-then-write initialization semantics in the copy lifecycle); the
  amplifier's real writes cross-check the alignment arithmetic but no
  standalone write matrix is claimed.
- One unexplained residual: b1a at 1M×8 recovered only ~half the gap
  (align session); one noisy Phase C cell (1M s32 b0). Neither is
  load-bearing.
- At amplifier depth 8 the alignment advantage vanished on this host;
  mechanism unattributed, recorded as an open observation.

==================================================
EVIDENCE TAXONOMY
==================================================

```text
DIRECTLY MEASURED
  - READ steady-state alignment effect (formal + align sessions,
    2-4.7x per-op; per-cell data in summary.csv)
  - d1 READ+WRITE end-to-end amplifier effect (1.8x, 512 MiB copies,
    hash-verified)
  - d8 null end-to-end result (no material effect at depth 8)
  - eager-init cost/fault shift (fault totals conserved; Phase D
    first-touch rates; arena probe bounds)

CAUSALLY ISOLATED
  - buffer address alignment as the dominant variable separating the
    b0/b1 group from b2/b3, via B1 → B1a (same allocation family and
    ownership semantics; only exposed pointer alignment changes; most
    of the observed gap disappears)

NOT DIRECTLY MEASURED
  - standalone WRITE alignment effect (no standalone write matrix)
  - minimum useful alignment threshold (only natural-unaligned vs
    4096-byte was compared; no 64/128/256/512/1024/2048 sweep)
  - exact kernel/uaccess implementation mechanism (which copy-path
    branch causes the difference)

INFERRED / CONSISTENCY CHECK (supporting, not independent proof)
  - d1 per-op × op-count arithmetic (READ per-op magnitude applied to
    512 reads + 512 writes ≈ 207 ms ≈ observed 207 ms d1 gap; WRITE
    magnitude not independently established)
```

==================================================
PHASE 3 GATE
==================================================

```text
PHASE 3 AUTHORIZED: YES — as the ALIGN-E0 research experiment only.
  Phase 3 is authorized as a research experiment, NOT as a production
  alignment change. No production representation change follows from
  Phase 2 evidence alone.

WHY (the single measured reason): Gate A — the alignment steady-state
copy cost is repeatable (three sessions), material (2-4.7x per-op
READ; 1.8x end-to-end at the production-default depth 1), causally
isolated at the userspace boundary (b1a; the exact kernel/uaccess
micro-mechanism is NOT established by this campaign), and NOT merely
shifted (per-op steady-state cost, present in the current production
representation on the measured READ path and the d1 READ+WRITE
amplifier).

If YES, exact scope implied by the evidence — ALIGN-E0: measure the
alignment threshold and regime crossover:

  alignment:  natural / 64 B / 128 B / 256 B / 512 B / 1 KiB / 2 KiB /
              4 KiB
  × I/O size:  4 KiB / 64 KiB / 1 MiB
  × depth:     1 / 2 / 4 / 8 / 16 / 32
  × direction: READ and WRITE (standalone matrices for both)
  × environment: native Linux MANDATORY before any production
              authorization (WSL2 magnitudes are ENVIRONMENT-LIMITED)

  Optional extension to separate cache-line alignment / generic address
  alignment / page-boundary phase effects: a page-relative offset sweep
  (page+0, +16, +32, +64, ...).

  ALIGN-E0 stays within the current per-slot ownership model (research
  variant of the copy app). It is explicitly NOT a
  BufferStorage/BufferPool/BufferLease framework, NOT registered or
  fixed buffers, and NOT a public buffer API redesign: BUF-F02
  (ownership/pooling) is NOT PROVEN, and B3 achieves the measured
  effect as an ordinary owned allocation. A pool/lease redesign has no
  performance justification from BUF-E0. (Gate B — alignment/
  registration lifetime for future fixed-buffer experiments — may ALSO
  favor an explicit storage boundary, but that is a capability argument
  to be made in Phase 3, not evidence from this phase.)

Keep current std::vector<std::byte> representation until that
experiment is reviewed: YES (Phase 2 changes nothing; production code
changed: NO).
```

==================================================
LIMITATIONS
==================================================

- WSL2 virtualized environment; absolute magnitudes (fault cost ~1.5-2
  µs/page, alignment multipliers) are host-bound. The alignment effect
  is expected to be a real causal variable on native Linux, but both
  its magnitude there and the responsible kernel/uaccess micro-
  mechanism are UNMEASURED — Phase 3 (ALIGN-E0) must re-quantify on a
  native host before any production change.
- No alignment-threshold sweep: only natural-unaligned vs 4096-byte
  alignment was compared. Nothing in this campaign establishes that
  PAGE_SIZE alignment is necessary, optimal, or a threshold.
- No standalone WRITE matrix: WRITE alignment magnitude is not
  independently measured (see EVIDENCE TAXONOMY).
- cycles:u unreliable (virtualized counter; AMENDMENT 1); instruction
  attribution inherits the 1/192 anomalous cell.
- Warm page-cache regime throughout (a memory/lifecycle campaign, not a
  storage-device study); no cold-device, NAND, or FTL claims.
- Phase A/B absolute construction costs for small sizes are in the
  pinned fresh-page regime (conservative upper bound; arena probe
  provides the lower bound); cross-arm deltas are regime-clean.
- The align session had one ~11-second concurrent compile (recorded in
  its notes; Phase C cross-arm ratios stable vs the formal session; its
  Phase B numbers are not used as primary evidence).
- Amplifier: two cells (1M×d1, 1M×d8), workers=1, sync=none — the
  production CLI defaults, not a matrix.
- in-loop word-sum verification is conservative constant overhead
  (identical across arms); strong FNV verification outside timed spans.

==================================================
FINAL STATUS
==================================================

```text
PR #264: adversarial claim-hygiene closure applied (this revision):
  findings preserved, micro-mechanism/threshold/universality claims
  downgraded to the evidence boundary (see EVIDENCE TAXONOMY).
MERGED: see PR #264 state at merge time
```
