# BUF-E0 final report — Phase 2 buffer truth (#263)

```text
VERDICT: STEADY-STATE STORAGE EFFECT MEASURED (page alignment of the I/O
         buffer; mechanism attributed) — with BUF-F01 eager-init resolved
         as FIRST-TOUCH COST SHIFT / NOT MATERIAL and BUF-F02 NOT PROVEN
CONFIDENCE: high for the alignment mechanism (three sessions, dedicated
         b1a isolation arm, amplifier arithmetic cross-check); high for
         F01 immateriality (fault conservation + amplifier + arena probe
         double-bound); medium for absolute magnitudes (WSL2)
ENVIRONMENT: WSL2 (kernel 6.18.33.2-microsoft-standard-WSL2, AMD Ryzen 7
         5800H, 8 CPUs, 16 GiB, ext4 on virtual block device, warm page
         cache) — QUALIFIED_BUT_VIRTUALIZED; all absolute numbers
         ENVIRONMENT-LIMITED; same-host causal comparison only. No
         native-NVMe/NUMA/TLB claims.

BASE: 2a8dd7995a606882f1bd42ca264aebc80ce1726b (origin/master)
HEAD: (final commit of this branch; sessions recorded clean-tree states)
BRANCH: research/buf-e0-buffer-truth
DRAFT PR: (opened after this report; DO NOT MERGE)
EXECUTION ISSUE: #263 (milestones BUF-E0A/E0B/E0C/VERDICT posted there)

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
                          → statistically indistinguishable at every
                            slot count (medians inside each other's MAD)
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
buffer ALIGNMENT: b2/b3 are page-aligned; b0/b1 pointers sit at +16
(glibc chunk offset).

MECHANISM (AMENDMENT 1, session bufe0-align-wsl2-1): arm b1a = b1's
exact allocation mechanism with the pointer rounded up to page
alignment. b1a recovers essentially the entire gap (64K s1:
24198 → 4975 vs b2 4542; 1M s1: 437015 → 124752 vs b2 120551; 10 of 12
cells full recovery). A page-aligned I/O buffer lets the kernel copy
path work in full-page units; a +16 offset defeats it for every page of
every op. This is a per-op steady-state cost — NOT cost shifting, NOT
vector-vs-mmap, and achievable within owned-allocation semantics
(posix_memalign / over-allocate+align).

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
  replica-b3 (page-aligned):   260.4 ms/copy   → 1.8x faster

depth 8:
  engine-b0: 234.8 ms   replica-b0: 226.9 ms   replica-b1: 257.3 ms
  replica-b3: 231.5 ms  → no material difference at d8 on this host

verdict: the F01 (uninitialized construction) microbench saving does
not survive the realistic lifecycle (construction is 0.02-1.9% of one
copy and amortizes over census reuse). The alignment effect AMPLIFIES
at the production default depth. Arithmetic cross-check: per-op
alignment penalty × (512 reads + 512 writes) ≈ 207 ms ≈ observed
replica-b0 − replica-b3 gap at d1 (207 ms). Per prereg §22 the
amplifier governs.
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

NEW (within BUF-E0's storage-representation scope, mechanism-attributed):

```text
BUFFER PAGE ALIGNMENT: production slot buffers are never page-aligned
  (glibc chunk pointers at +16; arena-carved small buffers worse), and
  the kernel copy path penalizes that on EVERY read-into and
  write-from op: 2-4.7x per-op in the microbench, 1.8x end-to-end at
  the production-default pipeline depth, recoverable by pointer
  alignment alone (b1a) and by posix_memalign (b3) within per-slot
  owned semantics. This is the ONE measured, material, non-shifted
  storage cost BUF-E0 found.
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
PHASE 3 GATE
==================================================

```text
PHASE 3 AUTHORIZED: YES

WHY (the single measured reason): Gate A — the page-alignment
steady-state copy cost is repeatable (three sessions), material (2-4.7x
per-op; 1.8x end-to-end at the production-default depth 1), mechanism-
attributed (b1a isolation; kernel full-page copy path), and NOT merely
shifted (per-op steady-state cost, present in the current production
representation on every read and write).

If YES, exact scope implied by the evidence:
  Phase 3 should be a MINIMAL ALIGNMENT experiment within the current
  per-slot ownership model (e.g. posix_memalign / over-allocate+align
  slot storage in a research variant of the copy app), NOT a
  BufferStorage/BufferPool/BufferLease framework: BUF-F02 (ownership/
  pooling) is NOT PROVEN, and B3 achieves the measured effect as an
  ordinary owned allocation. A pool/lease redesign has no performance
  justification from BUF-E0. (Gate B — alignment/registration lifetime
  for future fixed-buffer experiments — may ALSO favor an explicit
  storage boundary, but that is a capability argument to be made in
  Phase 3, not evidence from this phase.)

Keep current std::vector<std::byte> representation until that
experiment is reviewed: YES (Phase 2 changes nothing; production code
changed: NO).
```

==================================================
LIMITATIONS
==================================================

- WSL2 virtualized environment; absolute magnitudes (fault cost ~1.5-2
  µs/page, alignment multipliers) are host-bound. The alignment
  mechanism (kernel full-page copy fast path) is expected to exist on
  native Linux but its magnitude there is UNMEASURED — Phase 3 must
  re-quantify on a native host before any production change.
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
Draft PR: HUMAN REVIEW READY
MERGED: NO
```
