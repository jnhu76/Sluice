# ALIGN-E0 preregistration (FROZEN)

Frozen BEFORE the first formal ALIGN-E0 measurement session. After freeze,
the arms, primary metrics, matrices, direction definitions, same-work
contract, materiality rules, promotion gate, and amplifier cells below must
not change. Design errors discovered later require a dated `AMENDMENT`
section appended to this file recording: what changed, why, when discovered,
whether data already existed, and whether old/new data remain comparable.
The original text never disappears.

Execution authority: issue #265 (roadmap #259; Phase 2 #263 CLOSED as
completed; BUF-E0 evidence #264 merged at 312ede5). Subject: the
kernel/userspace I/O buffer address geometry (alignment and page-relative
phase) of ordinary `pread`/`pwrite` into/from user buffers, in the
prefaulted steady-state reuse regime.

> ALIGN-E0 is a research-only experiment. It does not authorize a production
> buffer abstraction or a production alignment change. Its purpose is to
> locate the minimum effective alignment and the workload regimes where
> alignment materially changes I/O cost.

Governing rule: OBSERVED EFFECT → ALIGNMENT THRESHOLD → SIZE/DEPTH/DIRECTION
CROSSOVER → NATIVE REPLICATION → ONLY THEN a production alignment decision.
"4096 worked → make all buffers page aligned" and "alignment helps d1 →
alignment universally faster" are both prohibited.

## 1. Experimental arms (exposed buffer address is the ONLY variable)

All arms provide exactly N writable/readable contiguous bytes, backed by the
SAME allocation mechanism. Core principle: keep backing
allocation/ownership identical; change only the exposed pointer alignment.

Backing allocation (identical for every arm): one over-allocated owned block
per process

```
posix_memalign(&base, 4096, N + 4096)
```

(one block, never touched before the prefault protocol; teardown `free(base)`).

| Arm | Exposed pointer | Meaning |
| --- | --- | --- |
| **A0** | `base + 16` | natural production-like pointer: 16-byte aligned, page offset 16 (glibc arena chunk-user-pointer shape). NOT deliberately aligned. |
| **A1** | `round_up(base, 64)` | 64-byte aligned, page offset 0 |
| **A2** | `round_up(base, 128)` | 128-byte aligned, page offset 0 |
| **A3** | `round_up(base, 256)` | 256-byte aligned, page offset 0 |
| **A4** | `round_up(base, 512)` | 512-byte aligned, page offset 0 |
| **A5** | `round_up(base, 1024)` | 1024-byte aligned, page offset 0 |
| **A6** | `round_up(base, 2048)` | 2048-byte aligned, page offset 0 |
| **A7** | `round_up(base, 4096)` | 4096-byte aligned, page offset 0 |

Because `base` is page-aligned, the ladder arms A1–A7 all sit at page offset
0 and differ only in divisibility. A0 sits at page offset 16. This is
deliberate: the primary ladder holds page-relative phase constant while
varying alignment (§9 of the plan). If "same nominal alignment but different
page-relative position behaves differently" is observed, the PAGE-OFFSET-E0
diagnostic (§7) is the follow-up, not a pre-expanded matrix.

Recorded per arm/session: exact allocation call, teardown call, page size
(4096), exposed pointer alignment, exposed pointer page offset. No allocator
framework, no pooling, no registration, no O_DIRECT.

Note on A0 fidelity: a true "natural malloc" pointer would flip between
arena (16-aligned, arbitrary page offset) and mmap (page-aligned) with size
under default glibc behavior. The plan forbids re-mixing allocator behavior
into the ladder, so A0 is the fixed `base + 16` shape. The page-relative
phase dimension of true arena pointers is covered by PAGE-OFFSET-E0 if
triggered.

## 2. Direction matrix (READ and WRITE are independent verdicts)

### READ

`pread(fd, exposed, N, off)` — kernel → userspace buffer. BUF-E0 observed an
alignment effect on WSL2 (must native-reproduce); READ is the primary
direction.

### WRITE

`pwrite(fd, exposed, N, off)` — userspace buffer → kernel. MUST be measured
independently. It is forbidden to back WRITE out of a READ+WRITE amplifier.
WRITE gets its own verdict.

READ and WRITE are benchmarked in separate processes/modes with identical
protocols. Both must satisfy the same-work contract (§4).

## 3. Size matrix (frozen minimum; no pre-expansion)

```
4096 (4 KiB)
8192 (8 KiB)
16384 (16 KiB)
65536 (64 KiB)
1048576 (1 MiB)
```

8 KiB and 16 KiB are added over BUF-E0's {4K,64K,1M} because the goal is
crossover localization. If a transition is observed between two neighbors
(e.g. 8K no effect / 16K effect), a follow-up neighborhood {12K, 24K, 32K}
may be appended ONLY via AMENDMENT after the formal session closes — never
pre-expanded.

## 4. Same-work contract (fail-closed, every formal run)

Every arm/cell must be byte- and work-identical:

- same file, same fd mode (O_RDONLY for READ, O_WRONLY for WRITE)
- same offsets (deterministic per cell; slot/stream `i` at rotating offsets
  within the working window; byte-identical across arms)
- same op count, same requested bytes (exactly N per op), same completed
  bytes (every op must complete N; short I/O = semantic failure, exit 3)
- same file size (256 MiB), same cache regime (warm page cache, §8)
- same depth, same worker count, same buffer reuse count
- same content validation, same error policy

WRITE: source bytes byte-identical across arms (deterministic master-block
generator, TAX-0-line family — seed `0xE1E1E1E1_21212121`, 4 KiB splitmix64
master block). Every pwrite returns exactly N; any short write fails closed.

READ: every read returns exactly N; in-loop cheap verification is a mixed
64-bit word sum that must equal the expected sum at that offset (identical
across arms); full FNV-1a hash of every read's N bytes is verified OUTSIDE
timed spans; a per-run hash-set gate requires identical results across arms
of the same cell.

WRITE integrity: after each WRITE run, the runner verifies the target file's
hash equals the expected deterministic hash (runner-side, outside timed
spans). If the target file is shared across arms, each arm writes the same
bytes at the same offsets, so the final file must equal the deterministic
content exactly.

Same-work failure → the cell is invalid (FAIL CLOSED), the process exits 3,
and the session is invalid.

## 5. Depth matrix and worker topology

Depth matrix (minimum):

```
1, 2, 4, 8, 16, 32
```

Worker topology: `workers = 1` PRIMARY (matches the BUF-E0 amplifier
interpretability).

Mechanism fact recorded up front: with plain synchronous `pread`/`pwrite`
there is no concurrency to hide copy latency, so in the synchronous
microbench `--depth N` means a back-to-back batch of N syscalls per rep
window (per-op metric). The plan's d1→d8 question is about whether per-op
copy cost survives when the copy latency is OVERLAPPED; overlap requires
real concurrency. Therefore:

- Synchronous microbench (`workers=1`): measures per-op cost vs alignment ×
  size × direction; depth is a batching diagnostic.
- Depth-crossover mode (SECONDARY TOPOLOGY DIAGNOSTIC): `workers = depth`,
  each worker thread owns its own buffers and issues synchronous
  syscalls in a barrier-synchronized steady-state loop — real in-flight
  depth equals worker count. This is the mechanism that answers "does the
  per-op wall benefit survive overlap". It is explicitly labeled
  `SECONDARY TOPOLOGY DIAGNOSTIC`, never merged into the primary map.
- The application amplifier (§9) carries the final d1/d8 application-level
  adjudication with the realistic copy pipeline.

Do NOT expand to alignment × size × depth × direction × workers up front.

## 6. Lifecycle regime and residency

Primary regime: prefaulted steady-state reuse. The object of study is the
per-I/O alignment effect, NOT allocation/first-touch.

- All arms enter the SAME residency first: explicit prefault protocol = one
  byte write per 4096-byte page across the exposed buffer's pages (identical
  code path for all arms; pages are the same backing set).
- Timed loop: NO allocation, NO initialization, NO manual prefault, NO
  buffer recreation. Same buffer reused every rep (production reuse
  semantics).
- Warm-up: one untimed warm sweep before formal reps (cache regime intent:
  warm page cache — this is a memory/uaccess-copy campaign, not a storage
  device study).

## 7. PAGE-OFFSET-E0 diagnostic (gated, NOT pre-expanded)

Trigger: only if the primary ladder (or depth/size analysis) shows that
"same nominal alignment but different page-relative position behaves
differently" — i.e. the data cannot be described by "alignment" alone.

Design (if triggered, frozen here):

- Page-aligned backing region (as §1).
- Exposed user buffer starts at `page_base + offset` for offsets
  `{0, 16, 32, 64, 128, 256, 512, 1024, 2048}` (N large enough that
  `page_base + offset + N` stays within the owned block).
- Same protocol/metrics as the main ladder, READ and WRITE at the
  then-current primary size(s).
- Purpose: distinguish cache-line boundary vs power-of-two alignment vs
  page-relative phase vs full-page coincidence. If offset-0 fast and
  offset-64 slow while a 64-byte-aligned pointer elsewhere is fast, the
  final report must NOT use "alignment" as the single descriptor.

## 8. Environment and cache regime

- Data file: 256 MiB, deterministic TAX-0-line bytes (4 KiB splitmix64
  master block, seed `0xE1E1E1E1_21212121`), generated once per session
  environment. READ target: this file. WRITE target: a separate 256 MiB file
  filled with the same deterministic bytes, rewritten identically per run.
- Primary regime: WARM page cache (untimed warm sweep before formal reps).
- Environment classification: WSL2 = `QUALIFIED_BUT_VIRTUALIZED`,
  `ENVIRONMENT-LIMITED` (same-host causal comparison only; no native
  NVMe/NUMA/TLB/device claims). NATIVE LINUX is the mandatory primary
  environment for any final Phase-3 verdict; WSL2 is HARNESS DEVELOPMENT
  ONLY. If no native host is reachable, the final verdict is
  `ENVIRONMENT-BLOCKED` with `PRODUCTION ALIGNMENT CHANGE AUTHORIZED: NO`.
- Recorded per session: filesystem, file size, RAM, cache-warm protocol,
  kernel, CPU, page size, compiler, libc (environment.json).

## 9. Application amplifier (after the microbench matrix)

Realistic copy amplifier, research-only replica of the production copy
algorithm (production engine untouched; compared against it as external
fidelity reference):

- Copy size: 512 MiB source → destination, full copy, READ + WRITE.
- Depth: `{1, 2, 4, 8, 16}`.
- Alignment arms: DO NOT carry all 8 arms. Select:
  - natural baseline (A0)
  - best threshold candidate from the microbench ladder (the minimum tested
    alignment with most/full benefit)
  - 4096 historical reference (A7)
  - If `best == 4096`, keep only {natural, 4096}.
- Same-work: `bytes_copied == file size`, read_ops/write_ops bounds, hash
  verification (post-exit dst hash == src hash, runner-side), fail-closed.
- Measured: per-op wall/instructions where reliable; full copy span;
  alignment-relative deltas. Production engine run as fidelity reference in
  the same session.
- Verdict rule: if the microbench winner disappears under the realistic copy
  lifecycle, the final verdict FOLLOWS THE AMPLIFIER (and vice versa the
  microbench map is reported as the per-op truth with the amplifier as the
  application-level boundary).

## 10. Metrics (frozen)

Primary:

```
wall/op           (in-process steady_clock spans, median+MAD over reps)
instructions/op   (perf stat, R7/R14 double-difference, TAX-0-line
                   normalization: (total(R14) - total(R7)) / 7 / ops_per_rep)
cycles/op         (perf stat; ONLY if reliable on the host — BUF-E0 recorded
                   cycles:u as UNRELIABLE on WSL2 (virtualized non-monotonic
                   counter); on WSL2 cycles is demoted and the quantitative
                   pair is instructions:u + in-process wall)
```

Secondary (best-effort, may be `UNAVAILABLE/UNRELIABLE` without penalty):
minor faults (in-process `getrusage` delta), context switches, syscalls/op,
CPU time/op. PMU cache/TLB events (branches, L1D/LLC/dTLB) are added ONLY
when a causal story requires them and the events are reliable on the host;
never automatically because the word "alignment" appears.

Repetitions: formal sessions use R7/R14 process pairs per cell×arm; ≥7
useful in-process reps reported as median + MAD (+IQR) with min/max. Never
best-run vs best-run.

Denominators pinned: per-op (wall/instructions/cycles per syscall).

## 11. Environment claims discipline

- WSL2 → `QUALIFIED_BUT_VIRTUALIZED`; absolute numbers `ENVIRONMENT-LIMITED`;
  valid for same-host causal comparison and harness validation only.
- No NAND/FTL/SSD-erase/device-write-amplification claims. No NUMA/TLB
  generalization. No claim that WSL2 findings reproduce on native Linux
  until native measurement exists.

## 12. Materiality rules (no arbitrary thresholds)

No pre-registered percentage (5%/10%) decides productionization. Materiality
weighs:

```
effect vs dispersion (median ± MAD separation)
neighboring-cell consistency
absolute CPU cost (instructions/op, wall/op)
application amplifier outcome
breadth of regime (how many size×depth×direction cells show the effect)
complexity of implementation
```

A statistically stable few-ns per-op difference is NOT automatically
architecture-worthy. Instructions-only savings with no wall benefit are
reported as `ALIGNMENT CPU TAX ONLY — WALL BENEFIT OVERLAPPED`, not as a
wall win.

## 13. Production authorization gate (ALL 8 required for YES)

1. native Linux reproduced
2. same-work valid
3. threshold reasonably localized
4. READ/WRITE boundary understood
5. size/depth regime mapped
6. application amplifier survives
7. d1/d8 discrepancy adjudicated enough not to misstate scope
8. implementation can remain minimal

Even YES authorizes only `minimal aligned per-slot storage` — never
BufferPool, BufferStorage framework, registered buffers. A minimal resource
boundary candidate is raised only if the current ownership representation
genuinely cannot express the earned alignment policy cleanly; if
`posix_memalign + RAII wrapper` suffices, `BufferStorage framework NOT
justified`. Not implemented in this phase regardless.

## 14. Final verdict vocabulary (exactly one primary term)

```
ALIGNMENT EFFECT NOT REPRODUCED ON NATIVE
ALIGNMENT EFFECT REPRODUCED — THRESHOLD UNRESOLVED
ALIGNMENT THRESHOLD LOCATED — REGIME-SPECIFIC
READ-ONLY ALIGNMENT EFFECT
READ+WRITE ALIGNMENT EFFECT
ALIGNMENT CPU TAX ONLY — WALL BENEFIT OVERLAPPED
ALIGNMENT BENEFIT BROAD ENOUGH FOR PRODUCTION CANDIDATE
MIXED — NEED TARGETED FOLLOW-UP
ENVIRONMENT-BLOCKED
```

then `PRODUCTION ALIGNMENT CHANGE AUTHORIZED: YES/NO` (default NO).

Phase-3 success does NOT require optimization. "64 B already enough", "4096
required only for large shallow READ", "WRITE has no alignment effect",
"alignment saves instructions but not wall at depth ≥ 4", "WSL2 effect does
not reproduce on native", and "ALIGNMENT EFFECT NOT REPRODUCED ON NATIVE"
are all successes.

## 15. Evidence taxonomy

Every final-report claim is classified into exactly one of:

```
DIRECTLY MEASURED
CAUSALLY ISOLATED
PROFILE/SOURCE SUPPORT
INFERRED
UNRESOLVED
```

Exact kernel/uaccess micro-mechanism (copy_to_user / copy_from_user /
iov_iter / copy_user* branch) may be investigated via source inspection and
perf profile, but source inspection NEVER upgrades a claim to
CAUSALLY PROVEN. If not attributed: `MICRO-MECHANISM UNRESOLVED` is an
acceptable, successful outcome. Kernel investigation is a means of
explanation only; the alignment verdict stands on the A/B data.

## 16. Sessions and immutability

Formal sessions live under `research/align-e0/results/<session-id>/`:
`environment.json` (timestamp, git HEAD/branch/dirty, binary sha256, system,
tools), `manifest.json` (protocol + cells), `commands.md`, `notes.md`,
`raw/` (per-run bench JSON + perf output), `summary.csv`/`summary.json`.
Written once; never edited after the session closes. Re-runs = new session
id. Session id convention: `aligne0-<micro|amp>-wsl2-<n>` (WSL2) or
`aligne0-<micro|amp>-native-<n>` (native Linux).

## 17. Production guard

`PRODUCTION CODE CHANGED: NO`. All arms are research-only bench code. If the
harness proves unable to express the intended lifecycle, that is a reported
BLOCKER, not a production change. One Draft research PR only; DO NOT MERGE;
stop for adversarial review after the verdict. Even a YES verdict does not
put production code on this PR — a separate issue/PR would be opened.

---

## AMENDMENTs

(none yet — append-only from here)
