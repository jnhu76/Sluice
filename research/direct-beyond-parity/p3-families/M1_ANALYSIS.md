# M1 — Vectored I/O Family Analysis (Preregistered, Pre-Measurement)

- Family: M1 — Vectored I/O (`readv` / `writev` / `preadv` / `pwritev`,
  with `preadv2` / `pwritev2` noted where their extra flags matter).
- Mission input: `research/direct-beyond-parity/MISSION.md` §5.1 M1
  (three-case discipline, lines 457–481 of the mission file read this
  session).
- Plan input: `research/direct-beyond-parity/P3_MECHANISM_STUDY_PLAN.md` §2
  (family M1, fixed semantic/legal framework §2.1, research questions §2.2,
  arms §2.3, cells §2.4, pinned predictions §2.5), plus §6 accounting rows,
  §7 statistics, §8 verdicts, §9 admission cap, §11 six-element candidate
  path, §12 deferral D-P3-01.
- Semantic floor input: `research/direct-beyond-parity/P0_BASELINE_AND_OWNERSHIP.md`
  §5 (must-preserve semantics and oracles; contract anchors SEM-02…SEM-07,
  ERR-01/02, THREAD-01, W-01).
- **Status: PREREGISTERED BEFORE ANY M1 MEASUREMENT.** Verified this
  session: the probe kit on branch `direct-cost-ladder` (HEAD `2364be4a`)
  has **no vectored arm yet** — its arm table stops at
  `a0…m2sz` with no `av*`/`wv*` entries
  (`/home/jnhu/Source/Sluice-direct-cost-ladder/research/direct-cost-ladder/README.md:45-69`).
  No M1 probe number exists anywhere in this repository at the time of
  writing; every prediction below is therefore pinned before observation.
- Scope note: this document is the M1 family analyst's deliverable. M2's
  five proof obligations, M3's seven disclosures and M4's perception matrix
  belong to their own family documents; M1 references M2 only where
  M1-Q5 requires the Case-B ceiling bound.

## 0. Verified environment and semantic facts (this session, 2026-10-28)

| Fact | Evidence (command run / file read) |
|---|---|
| Kernel `7.2.11-200.fc44.x86_64`; glibc 2.43; GCC 16.2.1 | `uname -r`; `ldd --version` → "GNU libc 2.43"; `gcc --version` → 16.2.1 20260819 |
| `IOV_MAX = 1024` on this host | `getconf IOV_MAX` → `1024` |
| `preadv`/`pwritev` fill iov entries **in array order**, completely filling `iov[0]` before `iov[1]`; short fills leave trailing buffers untouched | `man 2 readv` (Linux man-pages 6.13, 2025-01-04), DESCRIPTION |
| `readv()` "is guaranteed to read a **contiguous block** of data from the file, regardless of read operations performed in other threads or processes" — i.e. one `preadv` reads exactly one contiguous file extent, atomically relative to same-description readers | `man 2 readv`, DESCRIPTION (atomicity paragraph) |
| `preadv`/`pwritev` do **not** change the file offset; fd must be seekable | `man 2 readv`, preadv/pwritev section |
| Short transfer (< requested) on vectored calls is **success**, not error; return 0 on a read means EOF | `man 2 readv` RETURN VALUE; `man 2 pread` RETURN VALUE ("a return of zero indicates end of file") |
| Kernel-side vectored rejections: `EINVAL` if Σ`iov_len` overflows `ssize_t`; `EINVAL` if `iovcnt` < 0 or > permitted maximum (1024 here); `EOPNOTSUPP` for unknown `preadv2` flags | `man 2 readv` ERRORS + NOTES ("On modern Linux systems, the limit is 1024") |
| glibc wrappers for `readv`/`writev`/`preadv`/`pwritev` are **direct syscalls** since glibc 2.20 (the historical temporary-buffer fallback for low `IOV_MAX` kernels is dead code on this host) | `man 2 readv`, "Historical C library/kernel differences"; host glibc 2.43 |
| `preadv`/`pwritev` since Linux 2.6.30 / glibc 2.10; `preadv2`/`pwritev2` since Linux 4.6; `RWF_*` flags are **per call**, not per iov entry | `man 2 readv` VERSIONS + preadv2 flags list |
| `preadv2` `offset = -1` uses the current file offset (unlike `preadv`, where offset is always explicit) | `man 2 readv` preadv2 section |
| Sluice public data ops are **one span per call** — no vectored form exists | `include/sluice/blocking/file.hpp:14-21` (`read_at`/`write_at`/`read`/`write` all take a single `std::span`) |
| Contract anchors: SEM-03 precedence at `docs/explicit-io-contract.md:398`; SEM-04 at `:437`; SEM-05 at `:459`; SEM-06 at `:483` | Read this session |
| P1 grounding constants: read wrapper ≈ **+38.0…+39.7 instr/op** (`DIRECT_COST_FLOOR.md:220-228`), write wrapper ≈ **+19.0…+19.3 instr/op** (`DIRECT_COST_FLOOR.md:254-260`); 1 B read floor 553.850 ns/op; 4 KiB read_at N1 704.666 ns/op; warm marginal byte cost ≈ 0.128 ns/B (plan §0 derivation) | `research/direct-beyond-parity/DIRECT_COST_FLOOR.md` §4.1 tables |
| B1 identity: commit `77e5399a4599…` (PR #602 head), library sha256 `d1e351a5…3007f0` | `P0_BASELINE_AND_OWNERSHIP.md:69,105,144` |

## 1. Mechanism forms in this family

| Form | File-offset relation | User-buffer relation | Cursor | Notes |
|---|---|---|---|---|
| `preadv(fd, iov, k, off)` / `pwritev(...)` | one contiguous extent `[off, off+Σlen)` | k scattered buffers, filled/written in array order | not moved | the Case-A primitive |
| `readv` / `writev` | one contiguous extent at the shared cursor | k scattered buffers | advanced by the transfer | cursor-relative variant; would map to a vectored `read`/`write`, not `read_at`; no P3 cell covers it (open question OQ-M1-4) |
| `preadv2` / `pwritev2` (+`RWF_*`) | same as `preadv`, or current offset when `offset=-1` | same | not moved (unless `offset=-1`) | flags are whole-call; `RWF_HIPRI` requires `O_DIRECT` (prohibited, plan §5.1); `RWF_NOWAIT`'s EAGAIN-with-zero-bytes shape does not map onto SEM-05 (open question OQ-M1-5) |
| one plain `preadv` spanning the **gaps** between scattered requested offsets | contiguous extent **larger than the request set** | scattered | — | **forbidden shape**, see Case B |

## 2. Legal framework against the v2-r1 public contract

The public Direct contract is **one span per call** with per-call admission
(SEM-03 steps 1–7, `docs/explicit-io-contract.md:398-435`), positional
semantics that never touch the shared cursor (SEM-04, `:437-457`), and
short-I/O/EOF rules where a primitive does not loop (SEM-05, `:459-481`).
The P0 §5 oracle floor (closed/access/zero-length/offset-overflow
precedence, EINTR, partial, EOF, confirmed bytes, effect certainty,
THREAD-01 concurrency, W-01 independence) applies to every mechanism form
below. The three cases are kept strictly distinct; conflating them is a
protocol violation, not a finding (plan §2.1).

### 2.1 Case A — file range contiguous × user buffers scattered

**Shape.** k destination buffers, in logical order, covering exactly one
contiguous file extent `[off, off+total)` split at k−1 boundaries.
`total = Σ len_i`.

**What the kernel actually does (verified, §0).** One `preadv` transfers
the whole extent in one syscall: it is guaranteed to read a contiguous
block, fills `iov[0]` completely before `iov[1]`, returns one total count,
treats a short total as success, returns 0 only at EOF for a nonempty
request, and never moves the shared cursor. The kernel writes directly
into the caller's buffers — there is no intermediate staging copy in the
kernel or in glibc 2.43 (wrapper is a direct syscall, §0). "Zero
intermediate copies" is thus a **structural property of the call shape**,
to be certified by call-shape enumeration and corroborated by PMU
(plan §6 row "User-space copies"), not assumed.

**Legality verdict: LEGAL mechanism, NOT EXPRESSIBLE in the current public
API.** Today the only legal Sluice composition is k separate `read_at`
calls (k admissions, k syscalls). `preadv` expresses the same shape in one
syscall. Because `read_at` takes a single span
(`include/sluice/blocking/file.hpp:14-17`), no current public operation can
carry k buffers; therefore any product form is a **new-capability design
candidate** (§4) — the single-call `read_at` semantics are untouchable
(mission §5.1 M1: "如果现有 API 无法表达这种合法组合，提交独立接口设计候选，不可改变现有单次
read_at 的语义").

**Equivalence obligations a Case-A candidate must discharge** (each maps to
a P0 §5 oracle row and to the M1-Q3 probe oracle):

1. **Admission precedence, evaluated once for the whole extent.** Closed →
   `invalid_state` before access check; access illegality →
   `invalid_argument` (SEM-03 table). A vectored form must not evaluate
   admission k times with k different observable outcomes; it is one public
   call with one verdict.
2. **Total-zero is a logical no-op.** k buffers whose lengths sum to zero
   (including zero buffers) → success 0, skip range checks, **no OS call**
   (SEM-03 step 3; oracle `zero_length_requests_make_no_native_call`,
   P0 §5.1). The kernel's own behavior for a zero-total `preadv` is thereby
   irrelevant and must never be reached.
3. **Range validation over the whole extent with checked arithmetic.**
   Validate `off` and `off + total − 1` without overflow (SEM-04);
   native-invalid → `invalid_argument`. The kernel adds two vectored
   rejections that must be mapped, not leaked: Σ`iov_len` overflowing
   `ssize_t` (`EINVAL`) and `iovcnt > 1024` (`EINVAL`) — per SEM-04 these
   are either native-invalid range (`invalid_argument`) or a stricter
   declared limit (`not_supported`), decided **before** acceptance
   (OQ-M1-3).
4. **Short count = prefix fill across iov boundaries.** A partial total
   fills `iov[0..j]` completely plus a prefix of `iov[j+1]`, in array
   order. Per-buffer fill is derivable from the single total, so the
   result shape stays a primitive count (SEM-05 row "0 < n ≤ requested"),
   not a per-iov vector.
5. **EOF is success.** Nonempty request returning 0 at the observation
   point is EOF, not error (SEM-05); under a quiescent fixture the
   per-buffer prefix picture is identical between k×`pread` and 1×`preadv`
   — this identity is exactly the M1-Q3 oracle cell (`p3-m1-eof-boundary-4k`).
6. **EINTR retries only without a completed count** (SEM-05; ERR-01;
   oracle `primitive_retries_eintr_without_a_completed_count`, P0 §5.1).
   One `preadv` EINTR with no bytes transferred retries the whole extent;
   it must never replay confirmed bytes.
7. **Cursor untouched.** `preadv`/`pwritev` do not move the shared cursor
   (§0) — the SEM-04 positional rule holds natively. A lowering through
   `lseek`+`readv` would violate SEM-04's "hidden seek-plus-read/write is
   not a conforming lowering" and is forbidden (SNE-3 below).
8. **Confirmed bytes / effect certainty** follow the primitive-level ERR-02
   discipline unchanged: a partial total before an error is confirmed
   progress; an uncounted attempt reports unknown effect, never 0.
9. **Concurrency.** THREAD-01's rules (caller serializes
   move/close/destroy; no cross-call order promised) are per-call rules and
   carry over verbatim. One caveat is **stronger than** the baseline: the
   man-page atomicity guarantee means a single `preadv` reads a contiguous
   block un-torn relative to other readers of the same open file
   description, while k separate `pread`s have no group atomicity. This is
   a semantic *superset* of what SEM-04 promises ("no additional
   atomicity/snapshot guarantee"); the probe cells are serial-exclusive so
   they cannot measure this difference, and the §4 candidate must take an
   explicit disclosure stance (OQ-M1-1).

**Write twin.** `pwritev` gathers k source buffers into one contiguous
extent. Short write = progress; zero progress is primitive-level success
(composition must stop, SEM-05); SEM-06 sync coverage is temporal
(confirmed writes happen-before sync initiation) and is unaffected by
gather form. Same admission/no-op/overflow obligations as the read side.

### 2.2 Case B — file ranges scattered (disjoint offsets)

**Kernel fact (verified, §0):** no regular-file syscall reads multiple
disjoint offsets in one call. `preadv` covers exactly **one** contiguous
range split across iov entries — the man page's atomicity sentence is the
definitive statement of this.

**FORBIDDEN: issuing one plain `preadv` over non-contiguous offsets as if
it were the same request.** The only way to "use one preadv" for scattered
offsets is a **covering** read whose extent spans the gaps. That shape
fails equivalence in three independent, individually sufficient ways:

1. **It reads bytes the caller did not request** (byte amplification).
   Delivering correctness only by slicing the over-read afterwards is an
   amplification/consistency contract the primitive layer does not have;
   M2's explicit over-read threshold machinery exists precisely because
   this needs its own contract (plan §3.1).
2. **It crosses EOF boundaries the caller did not cross.** In the legal
   composition, a request wholly before EOF succeeds while a request
   starting past EOF observes EOF-0; a covering read collapses both into
   one short total (or one 0) attributed to the whole extent. Per-request
   EOF observation — an SEM-05 observable — is destroyed.
3. **It changes error attribution.** The kernel reports one `errno` per
   call. An error located in a gap region (or a fault on a page only the
   over-read touches) poisons requests whose own bytes were never
   implicated; conversely a per-request rejection class (range
   invalidity on one request) loses its per-op identity inside one call.

**Legal forms, exhaustively (plan §2.1 Case B):** (i) explicit caller
composition — k separate `pread`s (probe arm `av0` is exactly this for the
contiguous sub-case); (ii) M2-style merge + over-read under an explicit
amplification/consistency contract — M2 family territory; (iii) a
normatively designed new capability (batched positioned reads with
per-request results) — §4 candidate C. P3 documents the non-expressibility
and measures the opportunity size (M1-Q5, bounded by M2's k×pread −
1×covering-read spread); it does not fake Case B with Case A.

### 2.3 Case C — multiple independent operations (ordering and error responsibility)

**Obligations that must survive any batching:** each op owns its admission
verdict, its result/confirmed bytes, its error identity and precedence,
and its EOF/partial semantics (SEM-03/SEM-05 per call). Side-effecting
(write) ops must not be reordered across each other unless the caller
declares independence; error responsibility must not fuse — one op's
`invalid_argument` can neither poison nor vanish behind another's success.

**Kernel facts that bound what is possible (verified, §0):** vectored
syscalls return exactly **one count and one errno per call**; `RWF_*`
flags are per call, not per iov entry. Therefore a single `preadv`/
`pwritev` **cannot** express Case C, full stop. A legal batch form must be
a new capability whose *public* result is a per-op vector; its *internal*
execution may still be multiple syscalls (API-level batching), or an
io_uring-registered-buffer path — the latter is deferred by the plan
(D-P3-01: outside Direct's W-01 independence boundary, no admitted
workload) and cannot be revisited except through the §11 candidate path.

**Ordering.** Sluice promises no cross-call serialization order (SEM-04);
callers needing order establish it. A batch executes its member ops in
some order, thereby *creating* an order that did not exist. That is legal
only if the capability contract declares it — the caller either asserts
independence or accepts the declared execution order. Undeclared ordering
of independent writes is an SNE candidate (SNE-2).

This case exists only as a design-candidate shape (§4 candidate C); no
probe arm pretends to satisfy it by merging (plan §2.1).

### 2.4 SEMANTICALLY_NOT_EQUIVALENT register (illegal forms, with reasons)

| ID | Form | Divergent observable |
|---|---|---|
| SNE-1 | Case-B-as-Case-A: one covering `preadv` for scattered requested offsets | unrequested bytes transferred; per-request EOF observation collapsed to one short total; gap-region errors attributed to clean requests |
| SNE-2 | Batched independent ops behind one result/errno (error-responsibility fusion; undeclared write reordering) | one op's rejection or failure disappears into another's outcome; write order created without caller declaration |
| SNE-3 | Hidden `lseek` + `readv`/`writev` lowering for a positional vectored op | shared cursor moved (SEM-04 forbids hidden seek-plus-read/write); cursor is observable through `read`/`write` and native duplication (oracle `shared_cursor_is_shared_through_native_duplication`, P0 §5.1) |
| SNE-4 | `O_DIRECT`-based or `RWF_HIPRI`-based "equivalent" vectored cells | caller-buffer, alignment and cache-semantics contract differs (plan §5.1 prohibition, family-wide) |

## 3. Preregistered predictions and probe criteria (pinned before measurement)

Predictions PRED-M1-1…7 are the campaign-pinned set from plan §2.5,
restated here with their falsifiable forms and cell mapping; PRED-M1-5w is
this family document's addition (still pre-measurement). Wall claims obey
plan §7: 16 ABBA pairs, `taskset -c 2`, CI excluding 0 **and** |median| ≥
the new vectored track's MDE′ from `p3-m1-aa-vectored-4k`; count claims
never come from wall runs.

### 3.1 Predictions

- **PRED-M1-1 (syscall count, integral caliber).** At k=4: `av1` issues
  exactly **1.000 `preadv`/op-group** vs `av0` exactly **4.000
  `pread64`/op-group** (`strace -c`, window isolation, ≥3 diagnostic runs
  with identical counts). Write twin: `wv1` 1.000 `pwritev` vs `wv0`
  4.000 `pwrite64`. *Falsifiable form:* any other integral count, or
  non-repetition across runs, falsifies. *Cells:* all
  `p3-m1-vectored-*`, `p3-m1-wvectored-*`.
- **PRED-M1-2 (wall, Case A read, 4 KiB / k=4 warm tmpfs).** `av1 − av0 ≤
  −400 ns/op-group` with CI excluding 0 and ≥ MDE′.
  *Falsifiable form:* saving < 100 ns/op-group refutes the syscall-entry
  cost model for this host and caps the mechanism at
  `NO_CHANGE_JUSTIFIED` (wall) / `COST_MIGRATION_ONLY` (count-only).
  *Cell:* `p3-m1-vectored-4k-k4`. *Grounding model:* (k−1) = 3 saved
  syscall entries; the P1 1-B read floor (553.850 ns/op,
  `DIRECT_COST_FLOOR.md:220`) upper-bounds the entry+validation class, so
  the naive ceiling is ≈ 1.65 µs; the pinned −400 ns claim is deliberately
  conservative because part of a real 1 KiB entry cost pipelines with data
  movement and `preadv` adds iov copy-in/iteration. Measured landing in
  [100, 400) ns is a **partial** refutation with the predefined
  consequence above.
- **PRED-M1-3 (k-scaling).** The `av0 − av1` saving grows monotonically in
  k (point estimates k=2 < k=4 < k=8) at fixed 4 KiB total.
  *Falsifiable form:* any point-estimate inversion across the three cells.
  *Cells:* `p3-m1-vectored-4k-k{2,4,8}`. *Model:* saving(k) ≈ (k−1)·E −
  I(k) with E the per-entry saving and I(k) the iov-iteration term;
  monotonicity holds iff I(k) grows slower than linearly with slope < E.
- **PRED-M1-4 (work accounting).** `av1` shows **extra bytes = 0**
  (probe-internal counters: bytes requested = bytes transferred) and
  **user-space copies = 0** (structural certification — caller buffers are
  passed directly as iov entries, no staging array exists in the call
  shape — corroborated by PMU instr/op not contradicting).
  *Falsifiable form:* any nonzero extra-byte or extra-copy accounting
  falsifies "no intermediate copy" and marks the form
  `SEMANTICALLY_NOT_EQUIVALENT` at probe level. *Cells:* all vectored
  cells.
- **PRED-M1-5 (Sluice-side composition gap, read).** `avs − av0` ≈ k ×
  the P1 wrapper constant: PMU cross-arm delta ∈ **[+120, +200]
  instr/op-group** at k=4 (4 × ≈38 ± slack; P1 measured +38.0…+39.7
  instr/op per read_at, `DIRECT_COST_FLOOR.md:220-228`). Wall point
  estimate is sub-claim (pre-declared not a wall claim; instruction
  caliber only). *Falsifiable form:* PMU delta outside the band.
  *Cell:* `p3-m1-sluice-gap-4k-k4`.
- **PRED-M1-5w (Sluice-side composition gap, write; added by this
  document, pre-measurement).** `wvs − wv0` ≈ 4 × ≈19 instr/op-group:
  PMU cross-arm delta ∈ **[+55, +100] instr/op-group** (grounding: P1
  write wrapper +19.0…+19.3 instr/op, `DIRECT_COST_FLOOR.md:254-260`).
  *Falsifiable form:* PMU delta outside the band. *Cell:*
  `p3-m1-wsluice-gap-4k-k4`.
- **PRED-M1-6 (write mechanism).** `wv1 − wv0 ≤ −400 ns/op-group` at
  4 KiB / k=4 with CI excluding 0 and ≥ MDE′; syscall 4.000 → 1.000.
  Same falsifier consequence as PRED-M1-2. *Cell:*
  `p3-m1-wvectored-4k-k4`.
- **PRED-M1-7 (Case B documentary).** Non-expressibility stands: no probe
  arm expresses scattered offsets as one `preadv`; the measured Case-B
  ceiling belongs to M2 cells. *Falsifiable form (of the framework
  itself):* discovery of any regular-file single-syscall scattered-offset
  mechanism on this kernel would reopen Case B — none is known (§0 man
  page: one contiguous block per call).

### 3.2 Research-question → prediction/cell map

| Question (plan §2.2) | Answered by | Probe criterion |
|---|---|---|
| M1-Q1 (syscall-reduction + wall/CPU of 1×preadv vs k×pread) | PRED-M1-1/2/3/6 | integral `strace -c` counts; paired wall deltas ≥ MDE′ with CI≠0; PMU instr/op deltas |
| M1-Q2 (iovec-iteration vs per-syscall-entry charge; crossover) | the k∈{2,4,8} and 64 KiB cells interpreted through the saving(k) = (k−1)·E − I(k) model | crossover criterion: the smallest k at fixed total where the `av0 − av1` delta is CI-excluding; if none of k=2/4/8 qualifies, crossover > 8 at this shape; kernel-side split is **attribution only** (plan §6: no kernel tracing) |
| M1-Q3 (Case-A equivalence at probe level) | `p3-m1-eof-boundary-4k` oracle cell + PRED-M1-4 | identical destination bytes (checksum group {av0,av1,avs}); identical EOF/short-count per-buffer prefix fills (exact prefix-length equality per buffer + checksum over effective prefix); zero user-space copies; zero extra bytes. Any divergence → cell invalid, family blocked (MISSION §9) |
| M1-Q4 (today's Sluice-side cost of the only legal composition) | PRED-M1-5/5w | PMU cross-arm deltas vs the k×wrapper-constant bands |
| M1-Q5 (Case-B ceiling + contract elements of a legal batched API) | M2's `rpl0` vs `rplT` spread (cross-family bound) + §4 candidate C | ceiling = (k×pread wall) − (1×covering-read wall) at the gap structure of M2's traces; qualitative prediction: positive at small gaps on warm tmpfs (M2's ≈5.4 KiB break-even grounding, 0.128 ns/B vs ≈550–700 ns/syscall), shrinking to negative past break-even; recorded as attribution to M2, claimed there |

### 3.3 Structural gain accounting rows this family will report (plan §6)

Both sides measured or attributed; one-sided reporting forbidden. For M1:
logical requests = 1 op-group (identical by construction, request digest in
cell header); physical syscalls = `strace -c` integral; bytes
requested/transferred/extra = probe counters (PRED-M1-4: extra = 0);
user-space copies = structural certification + PMU corroboration;
kernel work = attribution (syscall identity × counts, rusage split, PMU
deltas — never a measured kernel byte count); CPU/op = rusage + PMU;
wall/op = uninstrumented 16-pair ABBA; tail latency =
`NOT_SUPPORTABLE_WITH_REASON` (ns-scale effects, P0 §4.3 precedent);
correctness = checksum groups + EOF prefix equality + content_ok (write).

### 3.4 Verdict expectation (criteria fixed, plan §8; no post-hoc reinterpretation)

Case A may earn `MECHANISM_WIN_EARNED` at probe level **and**
`REQUIRES_API_DECISION` for productization simultaneously; with #578 R0
incomplete (plan §0, §9) the overall record is capped at
`CONDITIONAL_WORKLOAD_WIN` at best and the cap is stated, not implied.
Case B/C illegal forms are `SEMANTICALLY_NOT_EQUIVALENT` per §2.4. A
PRED-M1-2/6 failure reduces the mechanism to `COST_MIGRATION_ONLY`
(syscall count moved, no wall/CPU win); a PRED-M1-4 failure is
`SEMANTICALLY_NOT_EQUIVALENT` at probe level.

## 4. New public capability (apiCandidateNeeded = true) — six-element design points

**apiCandidateNeeded = true.** Case A is a real, legal mechanism shape
that the current one-span-per-call public API cannot express
(`include/sluice/blocking/file.hpp:14-21`); productizing it requires a new
public operation. Per MISSION §5.3 / plan §11, the candidate filed carries:

1. **New Operation Contract (sketch; final naming and shapes are Owner
   decisions).** A positional vectored primitive, e.g.
   `read_at_vectored(file, offset, buffers)` where `buffers` is a
   logical-ordered span of destination spans (write twin gathers source
   spans). Admission precedence mirrors SEM-03 exactly once per call:
   closed → `invalid_state`; access illegality → `invalid_argument`;
   Σlength = 0 (or zero buffers) → logical no-op, no OS call, range checks
   skipped; otherwise checked `offset + total` (first and last addressed
   byte, no overflow) → `invalid_argument`; a declared iov-count limit →
   `not_supported` before acceptance (SEM-04 stricter-limit rule; the
   1024 host limit and the split-vs-reject decision are OQ-M1-3). Result
   shape: **one primitive count** (total confirmed bytes; per-buffer
   prefix fill derivable in array order — no per-iov result vector, which
   is what keeps this Case A and not Case C). Stop set / effect certainty:
   primitive-level ERR-02 (short count is success; EOF-0 is success; error
   retains native detail and effect uncertainty; EINTR retried only
   without a completed count).
2. **Relation to v2-r1.** Extends the SEM-03/04/05 shapes with a vectored
   form; **no existing contract text is rewritten**; single-call
   `read_at`/`write_at` semantics are untouched; SEM-06 durability is
   unaffected (temporal coverage over confirmed writes); THREAD-01
   per-call rules carry over; W-01 independence is preserved (direct
   blocking form, no context). The stronger man-page atomicity of one
   `preadv` relative to k separate reads needs an explicit disclosure
   stance (OQ-M1-1) — the candidate may neither promise group-atomicity
   semantics it does not control nor silently inherit them.
3. **Performance opportunity requiring it.** The P3 cells
   `p3-m1-vectored-4k-k{2,4,8}`, `p3-m1-vectored-64k-k4`,
   `p3-m1-wvectored-4k-k4` (mechanism spread) and
   `p3-m1-sluice-gap-*`/`wsluice-gap` (today's cost of the only legal
   composition), at the calibers of §3. Numbers attach post-measurement;
   no SDK extension follows from a microbenchmark speed alone (MISSION
   §5.3).
4. **Semantics / errors / lifecycle / resource / portability design.**
   Buffer lifetime is synchronous-caller-owned (direct blocking call —
   buffers need only outlive the call); resource ownership unchanged
   (File borrow per existing direct ops). Kernel availability: `preadv`/
   `pwritev` since Linux 2.6.30 / glibc 2.10; glibc ≥ 2.20 wrappers are
   direct syscalls (host glibc 2.43 — the historical low-IOV_MAX fallback
   copy path cannot trigger). Portability: `readv`/`writev` are
   POSIX.1-2008 but `preadv`/`pwritev` are BSD/Linux, and the SEM-04
   ban on hidden seek forbids lowering vectored-positional to
   `lseek`+`readv` — non-Linux profiles (WSL2 follow-up, MISSION §8.3)
   need their own native vectored primitive or an explicit
   not-supported/looping disclosure; never a silent fallback. Error table:
   EINTR per SEM-05; native EINVAL classes mapped per §2.1 item 3;
   mid-transfer EFAULT/partial interplay is OQ-M1-2 and must be pinned
   before the candidate's error table is final.
5. **Design Issue / ADR Candidate.** Filed as a design candidate issue/ADR
   draft referencing this document and the P3 evidence (not as code).
6. **Awaiting Owner.** Explicit wait state; nothing auto-adopts; no merged
   PR (MISSION §1).

Separately, the **Case-B/C batched API** (per-request results and error
attribution, declared independence/ordering, explicit amplification
disclosure for any over-read, io_uring-registered-buffer execution) is a
**distinct** candidate (candidate C) whose ceiling evidence comes from
M2's cells; it is not a vectored form of `read_at` and must not be
presented as one. Its execution-mechanism half is D-P3-01-deferred.

## 5. Open questions (recorded, not guessed)

- **OQ-M1-1 (concurrent-mutation atomicity divergence).** One `preadv`
  reads a contiguous block un-torn relative to same-description readers;
  k separate `pread`s do not. Probe cells are serial-exclusive, so M1-Q3
  equivalence is established only under quiescence. What stance
  (disclose-and-allow / disclaim / forbid reliance) the §4 candidate takes
  is an Owner contract decision; no P3 cell can measure it beyond M2's
  descriptive divergence record.
- **OQ-M1-2 (mid-transfer fault semantics).** Whether a `preadv` that
  faults after fully filling some iov entries returns the partial count or
  `-1`/EFAULT is not pinned by the man page (§0). This affects the
  candidate's error table. The current cell set has no iov-fault
  injection arm; resolving it requires an added fault-injection cell via
  a numbered plan amendment — not guessed here.
- **OQ-M1-3 (k > IOV_MAX boundary).** Host limit 1024 (`getconf IOV_MAX`).
  Splitting a larger k into multiple `preadv`s preserves observable
  semantics (prefix-fill is compositional) but changes the syscall-count
  story; rejecting with `not_supported` is SEM-04-legal. Owner decision
  for the candidate; the probe cells never exceed k=8.
- **OQ-M1-4 (sequential vectored forms).** `readv`/`writev` (cursor-
  relative) have no cell and no candidate sketch beyond §1; if wanted,
  they are a separate design decision (shared-cursor advance semantics
  would be inherited from `read`/`write`).
- **OQ-M1-5 (preadv2 flags).** `RWF_HIPRI` requires `O_DIRECT`
  (prohibited, SNE-4); `RWF_NOWAIT`'s zero-byte EAGAIN shape has no
  SEM-05 mapping; `RWF_ATOMIC`/`RWF_DSYNC`/`RWF_SYNC` are write-durability
  knobs orthogonal to Direct's SEM-06 model. Default: out of family scope.
- **OQ-M1-6 (kernel-side attribution ceiling).** The iovec-iteration cost
  inside the kernel is attributable only through PMU deltas and syscall
  identity (plan §6); M1-Q2's kernel/user split is therefore attribution,
  not measurement — stated so in every table.
- **OQ-M1-7 (linearity of the Sluice-side wrapper model).** PRED-M1-5
  assumes avs−av0 scales linearly in k (k × ≈38 instr); only k=4 is in
  the cell set. If post-measurement suspicion of nonlinearity arises,
  adding k∈{2,8} gap cells requires a plan amendment — never silent
  (plan §0 amendment rule).
- **OQ-M1-8 (64 KiB write twin).** The cell set pairs `wv0` vs `wv1` only
  at 4 KiB/k=4 (`p3-m1-wvectored-4k-k4`); the 64 KiB write shape is
  unmeasured by design. Absence is a scope statement, not a negative
  result.
