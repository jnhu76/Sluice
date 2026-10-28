# P3 Mechanism Study Plan — Structural I/O Mechanisms (Preregistered)

- Mission: `research/direct-beyond-parity/MISSION.md` §5 (P3 — Beyond-Parity
  Structural I/O Research; families §5.1 M1–M4, gain accounting §5.2, API
  decision §5.3).
- Preregistration date: 2026-10-28. Primary platform: E5 Native Linux.
- **Status of this document: PREREGISTERED.** The four family scopes, research
  questions, semantic/legal frameworks, predictions, probe-arm definitions,
  parameter shapes, cross-arm checksum groups, cells, statistics, count
  calibers, verdict vocabulary and admission caps below are fixed **before**
  any P3 arm, kit commit or measurement runs. Mid-campaign deviations are
  recorded as numbered amendments with trigger/change/reason, never silently
  applied (precedents: `P1_MEASUREMENT_PLAN.md` Amendment 1;
  `P2_CANDIDATE_P2C1.md` §12 re-run discipline).
- Inputs (all read for this plan): `MISSION.md` §5; `P0_BASELINE_AND_OWNERSHIP.md`
  (§1 baselines/ownership, §3 verified eliminations, §4 open questions, §5
  semantic floor); P1 synthesis `DIRECT_COST_FLOOR.md`, `DIRECT_HOTPATH_LEDGER.md`,
  `CANDIDATE_GAP_REGISTER.md`; `P1_MEASUREMENT_PLAN.md` (§3 environment pin,
  §5 statistics, §6 calibers, §7 cold discipline, Amendment 1);
  `P2_EXPERIMENT_PLAN.md` (§2 cross-candidate protocol, §5 deferred table) and
  `P2_CANDIDATE_P2C1.md` / `P2_CANDIDATE_P2C2.md` (verdicts, caps, deferrals);
  `docs/performance/methodology.md` (standing method, tool boundary).

## 0. Standing facts verified for this plan (this session, 2026-10-28)

| Fact | Evidence (command run / document read) |
|---|---|
| #578 R0 is incomplete; issue OPEN | `gh issue view 578 --json number,title,state,updatedAt` → `{"state":"OPEN", …,"updatedAt":"2026-10-26T15:18:37Z"}`; body is a #556 handoff whose R0 scope (fio + DuckDB baseline/admission) is pending. No admitted real-workload evidence exists anywhere in this mission. |
| Host: E5 Xeon E5-2666 v3, **1 NUMA node** | `lscpu` → `NUMA node(s): 1` |
| Kernel 7.2.11-200.fc44.x86_64; root btrfs; /tmp tmpfs | `uname -r`; `findmnt -no FSTYPE /` → btrfs, `/tmp` → tmpfs |
| strace 7.2 available | `strace --version` → 7.2 |
| `copy_file_range` functionally supported same-fs on **both** tmpfs and btrfs (kernel 7.2.11) | Minimal functional probe run this session: explicit-offset 4 KiB copy → `ret=4096`, content MATCH on /tmp and /var/tmp. Incidental semantic fact pinned for §4: with NULL offset arguments the syscall uses (and advances) the file positions — the first, NULL-offset probe variant returned 0 at EOF without error. This grounds the M3 offset-semantics disclosure; the executor re-runs the probe at P3 start and archives it (§4.1). |
| Kit state: branch `direct-cost-ladder` (local-only), HEAD `2364be4a` (P2 twin arms + two-binary pairing), arms a0…m2sz per `README.md`; linked frozen B1 archive `d1e351a5…3007f0` | `git -C /home/jnhu/Source/Sluice-direct-cost-ladder log --oneline -3`; `research/direct-cost-ladder/README.md` arm table |
| Inherited decision floors (P1 §1, session A/A): read warm MDE **5.535 ns**, write warm MDE **15.695 ns**; cold track MDE 1416.185 ns | `DIRECT_COST_FLOOR.md` §1 |
| Inherited cost constants (warm tmpfs, P1 §4.1): read_at 4 KiB N1 704.666 ns/op; 64 KiB 8696.847; 1 B 553.850; cold btrfs 4 KiB 8672.99; marginal byte cost ≈ (8696.847−704.666)/(65536−4096) = **0.128 ns/B**; PMU wrapper constants ≈ +38 (read) / +19 (write) instr/op | `DIRECT_COST_FLOOR.md` §4.1, `DIRECT_HOTPATH_LEDGER.md` §1 |
| Tail latency NOT_SUPPORTABLE at ns-scale effects (per-op sampling clock ≈26 ns > predicted effects) | `P0_BASELINE_AND_OWNERSHIP.md` §4.3 |
| P2 outcomes: P2C1 KEEP (wall −16.565 ns/op composition, capped "measurable improvement with re-attributed residual"; tail-mix subclaim restricted); P2C2 KEEP (instruction caliber); combined measurement assigned to P5; deferred register carried (§12) | `P2_CANDIDATE_P2C1.md` §12, `P2_CANDIDATE_P2C2.md` §9/§10 |

## 1. Scope, ownership and product-implementation ban

1. P3 is **mechanism research only**. It answers: is there a legal I/O
   execution form that completes identical application work with fewer
   syscalls, fewer copies, or fewer processing steps (MISSION §5 goal)?
   It does not productize anything (§10 admission cap).
2. All probe work extends the committed kit on branch `direct-cost-ladder`
   (worktree `/home/jnhu/Source/Sluice-direct-cost-ladder`): local commits,
   **never pushed**, never merged, never inside the frozen worktrees
   (`Sluice-600-base`, `Sluice-600-wt`) or the main workspace (P0 §1.3
   ownership rules; the ask's probe-arm mandate).
3. No production source, public header, test or `xmake` target of the product
   tree changes in P3. Any mechanism that would need a new public capability
   is recorded `REQUIRES_API_DECISION` with a six-element design candidate
   (§11) and waits for the Owner (MISSION §5.3).
4. Raw records: `research/direct-beyond-parity/p3-raw/<family>/<cell>.txt`
   with date/host/command headers (P1 §10 conventions); analysis re-runnable
   via the kit's `analyze_pairs.py`.
5. Environment pin inherits P1 §3 verbatim (E5, kernel 7.2.11, GCC 16.2.1,
   binutils 2.46.1, `scaling_governor=performance`, `perf_event_paranoid=1`,
   `taskset -c 2`, fixtures under `/tmp/sluice600` tmpfs and
   `/var/tmp/sluice600-p3` btrfs).
6. Kit-extension order (narrow commits): (a) functional-support probe tool;
   (b) new arm families (one commit per family: vectored, planner, copy);
   (c) M4 fixture/size-axis support; (d) kit smoke gate over all new families
   × parameter shapes × states before any timed cell (P1 §1 precedent; smoke
   raw → `p3-raw/kit/smoke-ladder.txt`). Kit commit SHA and every new binary
   sha256 recorded in `p3-raw/kit/ladder-identity.txt` before the first timed
   cell.

## 2. Family M1 — Vectored I/O (`readv`/`writev`/`preadv`/`pwritev`)

### 2.1 Semantic/legal framework (three-case discipline, fixed)

The public Direct contract is one span per call (`read_at`/`write_at`, SEM-03
through SEM-05, P0 §5.1). Three cases are kept strictly distinct; conflating
them is a protocol violation, not a finding:

- **Case A — file range contiguous × user buffers scattered.** k destination
  buffers covering one contiguous file extent `[off, off+total)`. Legal today
  only as k `read_at` calls (k admissions, k syscalls). `preadv` expresses
  exactly this shape in one syscall with zero intermediate copies (kernel
  fills the iov directly). Equivalence obligations: partial/short counts map
  to a prefix fill across iov boundaries (the caller composition must
  attribute the short count to the straddling buffer, not re-loop as one
  unit); EOF-zero is success; EINTR retries without a completed count;
  per-call admission precedence is evaluated once for the whole extent.
  **Not expressible through the current public API** → any product form is a
  new-capability design candidate (§11), never a change to single-call
  `read_at` semantics.
- **Case B — file ranges scattered (disjoint offsets).** **No single
  regular-file syscall expresses this.** `preadv` covers exactly one contiguous file
  range split across iov entries. **It is forbidden to issue one plain
  `preadv` over non-contiguous offsets as if it were the same request** —
  that reads bytes the caller did not request, crosses EOF boundaries the
  caller did not cross, and changes error attribution. Legal forms only:
  (i) explicit caller composition (k `pread`s); (ii) M2-style merge+over-read
  under an explicit amplification/consistency contract (§3); (iii) a
  normatively designed new capability (batched positioned reads with
  per-request results) — §11 candidate. P3 documents the non-expressibility
  and measures the opportunity size; it does not fake Case B with Case A.
- **Case C — multiple independent operations (ordering and error
  responsibility).** Each op owns its admission verdict, its result, its
  error, and its EOF/partial semantics. Any batching form must preserve
  per-op result and error attribution, must not reorder side-effecting
  (write) ops across each other unless the caller declares independence, and
  must not fuse error responsibility (one op's `invalid_argument` cannot
  poison or vanish behind another's success). This case exists only as a
  design-candidate shape (§11 template); no probe arm pretends to satisfy it
  by merging.

### 2.2 Research questions (fixed)

- M1-Q1: For Case A at matched total bytes on E5 warm tmpfs, what is the
  syscall-reduction and wall/CPU effect of one `preadv`/`pwritev` versus k
  separate `pread`/`pwrite` calls (k ∈ {2,4,8}, total 4 KiB; k=4, total 64 KiB)?
- M1-Q2: What does the kernel charge for iovec iteration versus per-syscall
  entry (syscall/op, instr/op, wall/op)? Where is the crossover in k and size
  below which vectored stops paying?
- M1-Q3: Does the Case-A equivalence obligation hold at probe level —
  identical destination bytes, identical EOF/short-count behavior under a
  boundary fixture, zero user-space copies, zero extra bytes?
- M1-Q4: What is today's Sluice-side cost of the only legal composition
  (k × `read_at`/`write_at` through the library) relative to the raw k×pread
  form (per-op wrapper constant × k)?
- M1-Q5: For Case B, what is the theoretical ceiling a batched primitive
  could capture (bounded by the k×pread − 1×covering-read spread measured in
  M2), and what contract elements would a legal batched API need (§11)?

### 2.3 Probe arms (kit extension; names, parameter shape, checksum groups)

New trailing token `iovcnt=<k>` (argv position after the existing optional
tokens; default k=4; existing arms ignore it — no existing token position
changes). `size` keeps its meaning: total logical bytes per op-group.

| Arm | Tier | Mechanism | Parameter shape |
|---|---|---|---|
| `av0` | N0-class raw | k separate `::pread` calls, k scattered destination buffers, contiguous extent split at k−1 boundaries (the legal raw composition) | `<size> <ops> <warmup> <reps> <file> [pattern] [iovcnt]` |
| `av1` | N0-class raw | one `::preadv` with k iov entries covering the same extent and destination bytes | same |
| `avs` | B1 library | k separate Sluice `read_at` calls (today's only legal Sluice composition; links frozen B1 archive) | same |
| `wv0` / `wv1` / `wvs` | write twins | k × `::pwrite` / one `::pwritev` / k × Sluice `write_at`, same shapes | same, per-cell scratch |

Cross-arm checksum validation (smoke + every cell): equality group
{`av0`,`av1`,`avs`} produces an identical destination-content checksum
(concatenation of the k destination buffers in logical order, computed by the
verify pass exactly like the existing read arms); write group
{`wv0`,`wv1`,`wvs`} additionally requires `content_ok=1` on the scratch.
EOF-boundary state (fixture tail): the three Case-A arms must produce
identical per-buffer prefix fills (checksum over the effective prefix + exact
prefix-length equality) — this is the M1-Q3 oracle.

### 2.4 Cells (all warm tmpfs, 16 ABBA pairs, taskset -c 2, serial-exclusive)

| id | pairing | shape |
|---|---|---|
| p3-m1-aa-vectored-4k | av0 vs av0 | 4096 1024 1024 5 16 $READ_FIX iovcnt=4 (session A/A; new-track floor) |
| p3-m1-vectored-4k-k2 | av0 vs av1 | 4096 1024 1024 5 16, iovcnt=2 |
| p3-m1-vectored-4k-k4 | av0 vs av1 | 4096 1024 1024 5 16, iovcnt=4 |
| p3-m1-vectored-4k-k8 | av0 vs av1 | 4096 1024 1024 5 16, iovcnt=8 |
| p3-m1-vectored-64k-k4 | av0 vs av1 | 65536 512 512 5 16, iovcnt=4 |
| p3-m1-sluice-gap-4k-k4 | av0 vs avs | 4096 1024 1024 5 16, iovcnt=4 (M1-Q4; PMU + wall) |
| p3-m1-wvectored-4k-k4 | wv0 vs wv1 | Amendment-1 per-cell scratch, iovcnt=4 |
| p3-m1-wsluice-gap-4k-k4 | wv0 vs wvs | Amendment-1 per-cell scratch, iovcnt=4 |
| p3-m1-eof-boundary-4k | av0 vs av1 (tail state) | EOF-straddling extents; oracle cell, descriptive |

Diagnostics per cell: `strace -c` syscall counts (both arms), `perf stat
instructions:u,cycles:u` (both arms), rusage (kit output).

### 2.5 Preregistered predictions (pinned before measurement)

- **PRED-M1-1 (syscall count):** `av1` issues exactly 1.000 `preadv`/op-group
  vs `av0` 4.000 `pread64`/op-group at k=4 (integral count caliber; exact
  repetition across ≥3 diagnostic runs required).
- **PRED-M1-2 (wall):** at 4 KiB/k=4 warm tmpfs, av1 − av0 ≤ **−400 ns/op-group**
  with CI excluding 0 (model: 3 saved syscall entries at the measured ≈550 ns
  small-read floor class, minus iov-iteration cost). Falsified (refutes the
  syscall-entry cost model for this host) if the saving is < 100 ns/op-group.
- **PRED-M1-3 (k-scaling):** the av1−av0 saving grows monotonically in k
  (k=2 < k=4 < k=8 point estimates) at fixed total bytes.
- **PRED-M1-4 (work accounting):** av1 shows extra bytes = 0, user-space
  copies = 0 (structural certification + PMU corroboration); any nonzero
  extra-byte or extra-copy accounting falsifies the "no intermediate copy"
  claim.
- **PRED-M1-5 (Sluice-side gap):** avs − av0 ≈ k × the P1 wrapper constant:
  PMU cross-arm delta ∈ [+120, +200] instr/op-group at k=4 (4 × ≈38 ± slack);
  wall point estimate sub-MDE-class and not claimed as wall.
- **PRED-M1-6 (write side):** wv1 − wv0 ≤ −400 ns/op-group at 4 KiB/k=4;
  syscall 4.000 → 1.000.
- **PRED-M1-7 (Case B):** non-expressibility stands (documented, no arm
  violates it); the measured Case-B ceiling belongs to M2 cells.

Falsifier consequences: PRED-M1-2/6 failing → the mechanism is
`NO_CHANGE_JUSTIFIED` at wall caliber (syscall reduction alone would then be
`COST_MIGRATION_ONLY`); PRED-M1-4 failing → `SEMANTICALLY_NOT_EQUIVALENT` at
probe level (copy added); oracle-cell divergence → cell invalid, family
blocked until the oracle is understood (MISSION §9).

## 3. Family M2 — Request range planning / coalescing

### 3.1 Semantic/legal framework (fixed)

The independent Direct Primitive must **not** implicitly cache, merge or
reorder calls — admission, result and error semantics are per public call
(SEM-03…05; P0 §5.1). Legal forms for range planning, exhaustively:

1. **Explicit caller composition** — the caller (application) plans ranges
   and issues merged reads itself, owning the amplification and consistency
   consequences.
2. **Independent Host policy** — a separate host-side layer outside the
   Direct primitive family (e.g. a future dududu-side planner), again
   explicit.
3. **A normatively designed new capability** — a batched/gathered read
   facility with its own contract (§11 candidate), adopted only via Owner
   decision.

P3 studies the mechanism at probe level only.

### 3.2 Proof obligations (each carries a probe-level oracle)

| Obligation | Probe-level oracle |
|---|---|
| Input request-set identity | The request set R is generated deterministically (seeded LCG over offsets/sizes/gaps); merged and unmerged arms consume byte-identical request streams (generated once, replayed by both arms; recorded digest of the request set in the cell header). |
| Per-caller byte correctness | Per-request destination checksum equals an independently computed serialized reference replay (non-timed reference pass in the same process); equality gate per request, not just per group. |
| Per-item result & error attribution | EOF-boundary traces: each request whose extent crosses EOF receives exactly its prefix with eof semantics per request; injected short-read shapes (fixture-tail pattern) attribute per request. |
| No crossing side-effect/lifetime boundaries | Read-only family: no write arms merge across a sync/durable boundary in P3 scope; write-side planning is out of scope (recorded as a boundary, §12 D-P3-05). |
| Concurrent writes / file changes / cancellation / partial errors | A descriptive divergence cell: a writer process mutates merged extents between merged and unmerged replays; the observable byte difference is recorded as the semantic hazard magnitude (no CI claim). Cancellation/partial-error under batching is a design-candidate contract question (§11), documented, not simulated. |

### 3.3 Probe arms

New trailing tokens `merge=<T>` (over-read threshold in bytes; `merge=0` =
no merging) and `dedup={0,1}`. Trace token `trace=<name>` selects the
request-set generator (`g4k`: k=8 requests of 4 KiB, gaps ~U[0, 8 KiB];
`ov30`: 30% duplicate/overlapping bytes; `eofmix`: extents straddling EOF).

| Arm | Mechanism |
|---|---|
| `rpl0` | unmerged replay: k separate `::pread` per request, per-request dispatch (Baseline) |
| `rplT` | sort+sweep merge of adjacent/overlapping extents with gap ≤ T into one `::pread` per merged extent, then in-memory slice dispatch to per-request destinations |
| `rplD` | overlap/duplicate elimination (`dedup=1`, T=0): coalesced identical/contained ranges read once, dispatched by slicing |

Checksum groups: {`rpl0`, `rplT` for every T, `rplD`} on the same trace must
produce identical per-request checksums (the merge may over-read but never
delivers wrong bytes); group gate evaluated per request.

### 3.4 Cells

| id | pairing | shape |
|---|---|---|
| p3-m2-aa-planner-g4k | rpl0 vs rpl0 | trace=g4k (new-track A/A) |
| p3-m2-merge-sweep-g4k | rpl0 vs rplT, T ∈ {512, 1K, 2K, 4K, 8K, 16K} | warm tmpfs; per-T cell (the T-sweep locates T*) |
| p3-m2-dedup-ov30 | rpl0 vs rplD | trace=ov30 |
| p3-m2-eof-attribution | rpl0 vs rplT (T=4K) | trace=eofmix; per-request oracle cell |
| p3-m2-merge-cold-g4k | rpl0 vs rplT (T ∈ {0, 256, 512, 1K, 4K}) | btrfs cold per P1 §7 discipline |
| p3-m2-concurrent-divergence | rpl0 vs rplT with writer process | descriptive divergence cell, no CI claim |

### 3.5 Preregistered predictions

- **PRED-M2-1 (merge win):** on `g4k` warm tmpfs at T=8 KiB: syscalls/op-group
  8.00 → ≤ 3.00 (integral counts); wall/op-group reduction ≥ 30% with CI
  excluding 0; byte amplification ≤ 2.0× requested.
- **PRED-M2-2 (break-even threshold):** T*(warm tmpfs) ∈ **[1 KiB, 16 KiB]**
  — the smallest T where the T-cell's wall delta is CI-excluding negative.
  Grounding: 0.128 ns/B marginal byte cost vs ≈550–700 ns per saved syscall
  ⇒ ≈5.4 KiB break-even gap.
- **PRED-M2-3 (cold shift):** on cold btrfs, T* ≤ T*_warm/8 (grounding: cold
  ≈2.1 ns/B ⇒ break-even ≤ ≈330 B).
- **PRED-M2-4 (dedup):** on `ov30`: bytes transferred reduced by 30% ± 5pp;
  syscalls/op-group reduced ≥ 25%; wall reduction ≥ 15% (CI-excluding).
- **PRED-M2-5 (dispatch overhead bound):** at equal syscall counts (T=0 with
  sort+sweep still applied, adjacent-only merges), the planner's own
  userspace cost ≤ 100 ns/op-group vs `rpl0`; if the dispatch overhead ≥ 50%
  of the syscall saving at the winning T, the mechanism verdict degrades to
  `COST_MIGRATION_ONLY`.
- **PRED-M2-6 (correctness):** all per-request checksum gates green in every
  cell including eofmix; any mismatch invalidates the cell (P1 §5.9
  precedent) and blocks the family.

## 4. Family M3 — File-to-file kernel transfer (`copy_file_range` / `sendfile`)

### 4.1 Position and mandatory functional-support probe

This is an **explicit file-copy capability study**. It is **not** a
`read_at` replacement and never a way to return file bytes to a user buffer;
kernel-transfer results feed at most a §11 design candidate for an explicit
copy capability. Before any M3 timing cell, the executor runs and archives a
minimal functional-support matrix (`p3-raw/m3/support-matrix.txt`),
committed as a kit tool: for {tmpfs, btrfs} × {`copy_file_range`, `sendfile`}
× {same-fs, cross-fs} × {explicit offsets, NULL offsets} × {in-range,
past-EOF}: supported? returned count? content match? Today's session probe
already establishes the same-fs explicit-offset cells (ret=4096, MATCH on
both filesystems) and the NULL-offset position-advance/EOF-zero behavior;
the archived execution-time run is the gate. Any unsupported combination →
that cell records `NOT_RUN_WITH_REASON` — no substitution.

### 4.2 Seven mandatory disclosures (each mapped to its evidence source)

| Disclosure | Evidence in P3 |
|---|---|
| 1. File type & filesystem support | §4.1 support matrix (regular files only; pipes/sockets and non-regular types recorded out of scope for this family) |
| 2. Short-transfer behavior | Probe cells with chunk sizes 4 KiB/64 KiB against a 64 MiB fixture; short counts observed, loop-and-advance handling recorded (both syscalls may return < count without error) |
| 3. Offset & error semantics | Explicit-offset loops (offsets advanced by returned count); NULL-offset position semantics from §4.1; error mapping (EXDEV/EINVAL/EFBIG/ENOSPC classes) recorded from the support matrix and error-injection cells (write to a size-capped scratch) |
| 4. Concurrent-modification consistency | Descriptive divergence cell: writer mutates source during copy; merged view vs user-loop re-read view recorded (no CI claim); contract implications go to the §11 candidate |
| 5. Partial-transfer responsibility | Who owns the remainder after a short/error return — the loop must publish confirmed bytes exactly once; the §11 candidate must define the copy outcome shape (confirmed bytes / unknown effect mirroring ERR-02 discipline) |
| 6. Fallback path | Cross-fs and unsupported combinations: recorded from §4.1; a product candidate must define its fallback (user-space loop) and never silently fall back (today: `NOT_RUN_WITH_REASON` cells) |
| 7. Real CPU/memory-traffic reduction | §5 accounting table: CPU/byte (rusage), instr/byte (PMU), syscalls/byte (strace), user-space copies (structural: 2/byte for loops — kernel→user, user→kernel; 0/byte for kernel transfer), wall/byte |

### 4.3 Probe arms

| Arm | Mechanism |
|---|---|
| `cp1` | native user-space loop: `::pread` 64 KiB chunk → `::pwrite` (N1-class Baseline mechanism) |
| `cp1s` | Sluice loop: `read_at` + `write_at` per chunk (today's legal Sluice composition — the §5.2 Baseline) |
| `cp2` | `copy_file_range` loop, 64 KiB chunks, explicit offsets |
| `cp3` | `sendfile` loop (file→file), 64 KiB chunks, explicit out-offset |

Parameter shape: `<chunk> <nchunks> <warmup> <reps> <src> <dst-scratch>`;
destination is an Amendment-1-style per-cell scratch; content gate: full-file
checksum equality across {`cp1`,`cp1s`,`cp2`,`cp3`} + source (`content_ok=1`
class).

### 4.4 Cells and predictions

Cells: `p3-m3-aa-copy` (cp1 vs cp1, new-track A/A); `p3-m3-copy-tmpfs`
(cp1 vs cp2, cp1 vs cp3, cp1s vs cp1 — tmpfs→tmpfs 64 MiB);
`p3-m3-copy-btrfs` (same on /var/tmp btrfs); `p3-m3-copy-crossfs` (both
directions; `NOT_RUN_WITH_REASON` per unsupported combination);
`p3-m3-short-error-cells` (descriptive: size-capped destination scratch).

- **PRED-M3-1:** cp2 vs cp1 on tmpfs: rusage CPU (user+sys) per byte −30% or
  better; PMU instructions/byte reduced ≥ 2×; syscalls/byte 2→1 class
  (pread+pwrite → one cfr).
- **PRED-M3-2:** wall/byte improvement ≥ 10% on tmpfs (band; the primary
  claim is CPU/bytes, wall is recorded — tmpfs page moves are cheap and the
  wall gain may be small; a wall regression ≥ 5% with a CPU win is recorded
  as-is and the verdict stays CPU-scoped).
- **PRED-M3-3:** btrfs→btrfs warm: CPU/byte −20% or better; wall within
  [−5%, +30%].
- **PRED-M3-4:** |cp3 − cp2| wall/byte within 2× the copy-track MDE′ (same
  splice class) or the difference attributed.
- **PRED-M3-5:** cp1s − cp1 ≈ per-chunk wrapper constants (≈+38/+19
  instr/chunk class; wall sub-MDE per chunk, aggregate over 1024 chunks may
  resolve — reported at whatever caliber resolves, no extrapolation).
- **PRED-M3-6:** kernel-internal byte movement is **not directly observable**
  without kernel tracing — kernel work is attributed via syscall
  identity/counts + CPU split and disclosed as attribution (§5), never
  reported as a measured byte count.

## 5. Family M4 — Filesystem / hardware awareness

### 5.1 Scope and prohibitions (fixed)

Axes studied: cache hot/cold (P1 §7 drop discipline only), tmpfs vs real btrfs,
request-size sweep, working set (8 MiB < L3 25 MiB < 64 MiB < 256 MiB), CPU
cache / memory bandwidth (via size×working-set cells + PMU), buffered I/O
costs. **Prohibitions:** no unproven hardware-specific configuration
(no madvise/fadvise policy tuning, no hugepages, no IRQ/queue fiddling);
**no `O_DIRECT`** as an equivalent replacement in any cell (it is not
semantically equivalent — caller-buffer/alignment/cache-semantics contract
differs); NUMA cells → `NOT_RUN_WITH_REASON` (host has 1 NUMA node, §0).
M4 produces environmental constants and the syscall-vs-byte exchange rate R
that parameterizes M2's threshold model; it proposes no mechanism of its own
(any "read bigger blocks" conclusion is filed under M2's evidence).

### 5.2 Cells (reuse existing a0/a1/a2 arms; new fixtures only)

| id | shape |
|---|---|
| p3-m4-size-sweep-{512b,4k,64k,1m} × {tmpfs-warm, btrfs-warm} | a1 vs a2 (existing pairing), fixtures 64 MiB |
| p3-m4-ws-{8m,64m,256m} | a1 vs a2 at 4 KiB seq, working-set axis (256 MiB fixtures under /tmp and /var/tmp/sluice600-p3) |
| p3-m4-cold-{4k,64k} | a1 vs a2, btrfs cold per P1 §7 (sudo drop; `NOT_RUN_WITH_REASON` fallback if sudo unavailable) |
| p3-m4-exchange-rate | derived, not a cell: R(state) = (wall cost of one saved syscall)/(marginal ns/B) from the cells above |

### 5.3 Preregistered predictions

- **PRED-M4-1:** marginal warm-tmpfs byte cost ∈ **[0.08, 0.25] ns/B**
  (grounding: 0.128 from P1 4 KiB→64 KiB slope).
- **PRED-M4-2:** cold/warm ns/B ratio on btrfs ≥ 8× (grounding ≈ 12×).
- **PRED-M4-3:** working set 256 MiB vs 64 MiB at 4 KiB seq warm tmpfs:
  ns/op increase ≤ 15% (memory bandwidth not yet binding at this shape);
  falsified if > 25% with CI excluding 0.
- **PRED-M4-4:** R ordering R_warm_tmpfs > R_warm_btrfs > R_cold_btrfs with
  R_warm_tmpfs ≥ 8 × R_cold_btrfs (feeds PRED-M2-2/3 cross-checks).

## 6. §5.2 structural gain accounting protocol (per mechanism, fixed)

Every mechanism reports the MISSION §5.2 table with **both Baseline and
Candidate sides measured or attributed; one-sided reporting is forbidden**
(a candidate-only number is not a result). Row calibers, pinned:

| Row | Caliber (tool pinned) |
|---|---|
| Logical requests | Arm definition (deterministic request set digest recorded in the cell header) — identical N on both sides by construction, verified by the replay digest |
| Physical syscalls | `strace -c` (strace 7.2, this host) exact counts, whole-trace net counting with reps=1 − reps=0 window isolation or fixed-prime subtraction (P1 §5.2 caliber; the host's rejection of `-e trace=pread` is inherited — P2 §2.5 disclosure). Integral counts must repeat exactly across ≥3 diagnostic runs. |
| Bytes requested / Bytes transferred / Extra bytes | Probe-internal deterministic counters (sum of syscall length arguments; over-read = transferred − requested per M2's T). Never a wall source. |
| User-space copies | Structural certification (call-shape enumeration: caller buffers passed directly vs staging buffers) + PMU instr/op corroboration; reported as certified-structural, not measured-byte-count |
| Kernel work | **Attribution, disclosed as such**: syscall identity × counts + rusage user/sys split + PMU deltas; kernel-internal byte movement is not directly observed (no kernel tracing in P3 scope) |
| CPU/op | rusage user+sys ns/op (corroborating; ±30 ns tick class) + `perf stat -e instructions:u,cycles:u` (diagnostic; cross-arm deltas are the signal) — never a wall source (P1 §6) |
| Wall/op | Uninstrumented runs only (the sole wall source): 16 ABBA pairs, taskset -c 2, best-of-reps kit caliber |
| Tail latency | Only when the cell's expected effect ≥ 10× the ≈26 ns per-op sampling overhead **and** the arm logs per-op samples (M3 copy cells, M2 op-groups qualify; ns-scale M1/M4 cells do not). Otherwise the row records `NOT_SUPPORTABLE_WITH_REASON` (P0 §4.3 precedent; not silently omitted) |
| Correctness | The cell's oracle result: checksum equality groups, per-request checksums (M2), content_ok (write/copy), EOF-boundary prefix equality (M1/M2), support-matrix gates (M3) |

A mechanism's §5.2 table with reduced physical syscalls but unchanged
(attributed) total CPU/work is not a win — see §8 `COST_MIGRATION_ONLY`.

## 7. Statistics and count calibers (inherited + pinned)

1. **Wall statistics inherit `P1_MEASUREMENT_PLAN.md` §5 and the P2 additions
   verbatim:** A/A first per session and track (and per binary where two
   binaries are compared); interleaved ABBA; **16 pairs/cell (≥15)**; arm
   order alternating per pair; one process invocation per arm sample;
   `taskset -c 2`; point estimate = median of paired deltas; primary CI =
   95% t on the paired mean, t(0.975,15)=2.131·sd/√16; sign count as
   corroboration; raw 16 deltas archived; run-integrity gates before any
   number is read (exit 0, no VERIFY_FAIL, checksum equality, content_ok,
   fixture sha unchanged, ladder identity recorded).
2. **Claim rule:** a wall claim requires CI excluding 0 **and** |median| ≥
   MDE_track. Pinned floors inherited from P1: read warm **5.535 ns**, write
   warm **15.695 ns**, cold 1416.185 ns. New P3 tracks (vectored, planner,
   copy) have no pinned prior: their fresh A/A yields MDE′ =
   2.131·sd_AA′/√16, which is the only floor for that track (disclosed as
   such). For the inherited tracks, MDE and MDE′ are dual-reported and any
   claim that would flip under MDE′ is downgraded to NO_CLAIM (P2 §2.4
   addition 1, one-way rule).
3. **Serial exclusivity:** all cells strictly serial on a quiescent host; no
   concurrent cells/builds/load; `lsof` isolation before every write/copy
   cell.
4. **Amendment 1 scratch discipline** for every warm tmpfs write/copy cell:
   per-cell scratch, `mkfixture` immediately before, deleted after; cold
   write/copy cells on btrfs keep the pre-primed `noprime` protocol.
5. **Cold discipline:** P1 §7 verbatim (sudo drop before every invocation;
   btrfs fixtures only; `NOT_RUN_WITH_REASON` fallback, never fadvise/tmpfs
   substitution).
6. **Count claims (syscalls/op, instr/op, bytes):** never from wall runs;
   repetition + counter caliber with pinned tools — `strace -c` 7.2 (window
   isolation as in §6), `perf stat -e instructions:u,cycles:u`
   (perf_event_paranoid=1), probe-internal deterministic byte counters.
   Integral counts (syscalls/op) must be exactly equal across ≥3 diagnostic
   repeats; non-integral ratios (instr/op) repeat within 5% or the spread is
   disclosed. Count numbers carry no CI and never feed wall verdicts.
7. One rerun per failed cell after quiescing (P1 §11.10); persistent noise
   disclosed, never absorbed by extra reps without an amendment.

## 8. Verdict vocabulary (fixed criteria; no post-hoc reinterpretation)

| Verdict | Criterion (all required) |
|---|---|
| `MECHANISM_WIN_EARNED` | §5.2 table complete on both sides; physical work genuinely reduced (syscalls/op and/or bytes and/or user-space copies, count caliber); wall and/or CPU improvement ≥ MDE with CI excluding 0 on at least one warm cell; correctness oracle green; no semantic-equivalence violation at probe level |
| `CONDITIONAL_WORKLOAD_WIN` | Mechanism-level evidence holds (as above or instruction-caliber per P2C2 precedent) **but no admitted real workload exists** — the automatic P3 cap per §9; scope, shape and boundary conditions stated |
| `COST_MIGRATION_ONLY` | **Mandatory** whenever the measured "benefit" equals cost migrated to the kernel, another thread, or the caller: syscall count reduced but attributed total CPU/work unchanged; or work moved into planner/dispatch overhead; or bytes moved but not reduced. Recorded even when a wall number improved |
| `SEMANTICALLY_NOT_EQUIVALENT` | The mechanism cannot express the required observable semantics without a contract change (Case-B-as-Case-A, merged over-read under concurrent writers, batched error-attribution fusion); recorded with the exact divergent observable |
| `NO_CHANGE_JUSTIFIED` | No measurable gain at any caliber (sub-MDE wall, no count-caliber reduction), or gain smaller than the mechanism-justification cost (AGENTS.md gate) |
| `REQUIRES_API_DECISION` | Expressing the opportunity needs a new public capability; six-element design candidate filed per §11; Owner decides; no SDK extension from microbenchmark speed alone (MISSION §5.3) |

A family may carry a mechanism verdict **and** the §9 admission cap
simultaneously (e.g. M1 Case A: `MECHANISM_WIN_EARNED` at probe level +
`REQUIRES_API_DECISION` for productization + overall record capped at
`CONDITIONAL_WORKLOAD_WIN`). Verdicts are per mechanism × workload-shape; no
single cell generalizes (MISSION §11).

## 9. Real-workload admission (#578 R0)

`#578` R0 (fio + DuckDB baseline/admission) is **incomplete**: issue OPEN,
verified this session (§0). Consequences, fixed:

1. **P3 performs no product implementation.** All research stays at
   probe/suite level: kit arms on the local `direct-cost-ladder` branch,
   local commits, no pushes, no PRs, no master changes.
2. **No P3 conclusion claims real-workload benefit.** Every mechanism
   conclusion without workload evidence is capped at
   `CONDITIONAL_WORKLOAD_WIN` at best (§8); the cap is stated in the record,
   not implied.
3. Any mechanism needing a new public capability records
   `REQUIRES_API_DECISION` and proceeds only via §11.
4. If #578 R0 admission lands during P3 (it has not), the cap may be revisited
   **only** by an amendment naming the admitted workload evidence — never
   retroactively reinterpreted.

## 10. P3 execution outputs (named for the executing phase)

`STRUCTURAL_IO_MECHANISM_STUDY.md` (per-family findings, §5.2 tables,
verdicts), `IO_WORK_REDUCTION_LEDGER.md` (machine-work deltas per mechanism),
`API_CAPABILITY_CANDIDATES.md` (if any §11 candidates are filed) — per
MISSION §5 outputs. Raw evidence under `p3-raw/` as pinned in §1.4.

## 11. API / architecture decision path (MISSION §5.3, six elements)

If (and only if) a P3 mechanism shows a real opportunity that current public
APIs cannot legally express, the design candidate filed contains:

1. **New Operation Contract** — the operation's name, inputs, outputs,
   admission precedence, stop set, effect-certainty and result shape.
2. **Relation to v2-r1** — which existing requirement IDs it extends,
   intersects or leaves untouched (e.g. a `read_at`-vectored form vs SEM-03
   per-call admission; a copy capability vs SEM-05/ERR-02 outcome shapes);
   no existing contract text is rewritten by the candidate.
3. **Performance opportunity requiring it** — the P3 §5.2 numbers (which
   workload shape, which cell, which caliber).
4. **Semantics / errors / lifecycle / resource boundaries / portability
   design** — Case-C per-op error attribution, EOF/partial semantics,
   buffer lifetime rules, kernel-fallback strategy (M3 disclosure 6),
   cross-platform behavior (incl. WSL2 follow-up), resource ownership.
5. **Design Issue / ADR Candidate** — filed as such (issue/ADR draft),
   referencing the evidence.
6. **Awaiting Owner** — explicit wait state; nothing auto-adopts; no merged
   PR (MISSION §1).

## 12. Deferred register (carried from P2 + new P3 deferrals)

Carried verbatim from `P2_EXPERIMENT_PLAN.md` §5 (reasons ledger-cited there
and in `P2_CANDIDATE_P2C1.md` §10):

| ID | Reason (short) |
|---|---|
| G-READAT-01 (+ read twin) | Null-data guard delete-or-anchor-in-root is an Owner semantics decision under the mechanism-justification gate; comparator carries the same check; no beyond-parity gain available |
| G-WRITEAT-02 | Same Owner gate as G-READAT-01; ≪ MDE |
| G-READAT-02 | Record-only: EINTR re-entry re-spill, error-path only, no admitted workload frequency |
| G-META-01 | No executable admissibility oracle (no msan mode; ASan/UBSan cannot detect uninitialized reads); static-only until an uninit oracle exists |
| G-OPEN-01 | Measured PARITY (kernel floor dominates); static ablation only if ever re-opened |
| G-WRITEAT-01 | RESOLVED by P1 (retained for traceability) |

New P3 deferrals (allowed by this plan with reasons):

| ID | Reason |
|---|---|
| D-P3-01 | M1 Case-B single-syscall scattered-offset execution (io_uring batched submits / registered buffers): outside Direct's W-01 independence boundary (no IoContext/worker in Direct) and no admitted workload; only the §11 design-candidate path can revisit it |
| D-P3-02 | M3 cross-filesystem and `O_DIRECT`-interplay cells beyond the §4.1 support matrix: `O_DIRECT` is excluded by §5.1 prohibitions (not an equivalent replacement); cross-fs cells run only where the support matrix says supported, else `NOT_RUN_WITH_REASON` |
| D-P3-03 | M4 cache-policy tuning (fadvise/madvise/THP): forbidden unproven hardware-specific configuration (§5.1); revisit only with an admitted workload and a mechanism-justification answer |
| D-P3-04 | Write-side / mutation-side range planning (merge of writes, write combining): side-effect and durability boundaries (sync coverage, SEM-06) make every merge semantically loaded; read-only planning is the only P3 scope — write-side planning would be a new design candidate, not a probe |
| D-P3-05 | P2C1+P2C2 combined measurement: assigned to the P5 interaction matrix by `P2_CANDIDATE_P2C1.md` §12.6; not P3 scope |

## 13. Limitations (preregistered)

1. All P3 evidence is probe-level on synthetic request shapes; no claimed
   transfer to any application workload (§9 cap).
2. Kernel-internal work is attributed, not directly measured (§6 row).
3. New P3 tracks lack prior pinned MDEs; their floors are session-facts
   dual-reported per §7.2 — cross-track comparisons of wall magnitudes are
   caliber-limited and disclosed as such.
4. Tail latency stays `NOT_SUPPORTABLE_WITH_REASON` wherever effects are
   ns-scale (P0 §4.3); only µs-scale cells (M2 op-groups, M3 copies) may
   report tails, and only with per-op samples.
5. Cold cells carry the P1 §7 scope disclosure (page-cache eviction only;
   shared root device; btrfs metadata/allocator state uncontrolled).
6. Concurrent-writer cells are descriptive divergence records, not claims;
   the consistency contract question belongs to §11 candidates.
7. E5-native-Linux only; no cross-host comparability (methodology rule).

## Amendment log

None. Deviations during execution are appended here with trigger, change and
reason — never applied silently.
