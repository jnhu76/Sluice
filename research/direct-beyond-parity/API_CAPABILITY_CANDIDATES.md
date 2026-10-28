# API Capability Candidates (P3, MISSION §5.3)

- Filed because P3 produced mechanisms whose opportunity **cannot be legally
  expressed by the current public API** (families M1/M2/M3 carry
  `apiCandidateNeeded = true` and `REQUIRES_API_DECISION` verdicts;
  `STRUCTURAL_IO_MECHANISM_STUDY.md` §0).
- These are **design candidates, not adopted contracts**. No existing v2-r1
  text is rewritten by any candidate; no SDK extension follows from
  microbenchmark speed alone (MISSION §5.3); nothing auto-adopts and no PR
  merges itself (MISSION §1). Each candidate is in the explicit
  **Awaiting Owner** state (element 6).
- Evidence base: `STRUCTURAL_IO_MECHANISM_STUDY.md` (findings and verdicts),
  `IO_WORK_REDUCTION_LEDGER.md` (§5.2 tables), family preregistrations
  `p3-families/M1_ANALYSIS.md` §4, `p3-families/M2_ANALYSIS.md` §4,
  `p3-families/M3_ANALYSIS.md` §5, raw records under `p3-raw/`. All
  performance numbers are probe-level, E5-session, warm-cell evidence capped
  at `CONDITIONAL_WORKLOAD_WIN` (#578 R0 incomplete — plan §9).
- Working names below are placeholders; final naming, shapes and limits are
  Owner decisions.

## Candidate AC-1 — Positional vectored read/write (M1 Case A)

One public call moving one **contiguous file extent** into/out of **k
caller buffers in logical order** — the shape `preadv`/`pwritev` expresses
in one syscall with zero intermediate copies.

### 1. New Operation Contract (sketch)

- Shape class: `read_at_vectored(file, offset, span<span<byte>> buffers) ->
  primitive count` and the write twin `write_at_vectored(file, offset,
  span<span<const byte>> sources)`; total = Σ lengths.
- Admission precedence, evaluated **once per call** mirroring SEM-03:
  closed → `invalid_state`; access illegality → `invalid_argument`;
  total = 0 (or zero buffers) → logical no-op, success 0, **no OS call**,
  range checks skipped; otherwise checked `offset + total − 1` (first/last
  addressed byte, no overflow) → `invalid_argument`; a declared iov-count
  limit (host `IOV_MAX` = 1024, `getconf`, session-verified) → either split
  into multiple syscalls or reject `not_supported` **before acceptance**
  (SEM-04 stricter-limit rule — Owner decision, OQ-M1-3).
- Result shape: **one primitive count** (total confirmed bytes). Per-buffer
  prefix fill is derivable in array order; no per-iov result vector (that is
  what keeps this Case A, not Case C).
- Stop set / effect certainty: primitive-level SEM-05/ERR-02 — short count
  is success; EOF-0 is success; EINTR retried only without a completed
  count; error retains native detail and effect uncertainty; confirmed
  bytes published exactly once.

### 2. Relation to v2-r1

Extends the SEM-03/04/05 per-call shapes with a vectored form; **no existing
contract text is rewritten**; single-call `read_at`/`write_at` semantics are
untouched (MISSION §5.1 M1: "如果现有 API 无法表达这种合法组合，提交独立接口设计候选，不可改变现有单次
read_at 的语义"). SEM-04 positional rule holds natively (no cursor
movement); SEM-06 durability unaffected (temporal coverage over confirmed
writes); THREAD-01 per-call rules carry over; W-01 independence preserved
(direct blocking form, no context). **Stronger-than-baseline disclosure
duty:** one `preadv` reads a contiguous block un-torn relative to
same-description readers (man 2 readv) — a semantic *superset* of SEM-04's
"no additional atomicity" language; the candidate must neither promise it
as contract nor silently inherit it (OQ-M1-1, Owner stance decision).

### 3. Performance opportunity requiring it

`IO_WORK_REDUCTION_LEDGER.md` §1–§2 `[RUN]`: at 4 KiB/k=4 warm tmpfs,
4.000 → 1.000 syscalls/op-group with zero extra bytes and zero user-space
copies; Δwall +1576.0 ± 21.7 ns (read) and +1699.3 ± 9.3 ns (write), CI ≠ 0,
≥ MDE′; monotone in k (+511/+1576/+3614 ns at k=2/4/8); 64 KiB/k=4
+1389.2 ± 76.9 ns. Today's only legal form (k × `read_at`) is wall-parity
with the raw loop (M1-GAP row), so the win is unreachable without this
capability. Scope: contiguous-extent shapes only — scattered offsets are
AC-2 territory, never a covering read.

### 4. Semantics / errors / lifecycle / resource boundaries / portability

- Buffers: synchronous-caller-owned, must outlive the call (direct blocking
  form); resources unchanged (File borrow).
- Error table: EINTR per SEM-05; native EINVAL classes (Σ`iov_len`
  overflow; iovcnt > limit) mapped per SEM-04 as native-invalid
  `invalid_argument` or declared-limit `not_supported` — decided before
  acceptance. Mid-transfer EFAULT (partial iov fill then fault) is **not
  pinned** by the man page: the candidate's error table is not final until
  an iov-fault injection cell exists (OQ-M1-2; requires a numbered plan
  amendment — no guessing).
- Portability: `preadv`/`pwritev` are Linux/BSD (not POSIX); the SEM-04 ban
  on hidden seek forbids lowering to `lseek`+`readv`; non-Linux profiles
  (WSL2 follow-up, MISSION §8.3) need a native vectored primitive or an
  explicit not-supported disclosure — never a silent fallback. glibc ≥ 2.20
  wrappers are direct syscalls (host 2.43); the historical low-IOV_MAX
  fallback copy path cannot trigger here.
- Out of scope: cursor-relative `readv`/`writev` forms (OQ-M1-4);
  `preadv2` flag surface (OQ-M1-5).

### 5. Design Issue / ADR Candidate (draft text)

> **Title:** Add positional vectored read/write to the Direct blocking
> family (SEM-03/04/05 vectored extension).
>
> **Proposal:** Add `read_at_vectored` / `write_at_vectored` as primitive-
> class direct operations transferring one contiguous file extent between k
> caller buffers in one admission and (where the platform provides a
> vectored syscall) one OS call. Admission precedence, no-op, range and
> result semantics mirror SEM-03/04/05 exactly, evaluated once per call;
> result is a single primitive count. Kernel map: `preadv`/`pwritev`
> (Linux/BSD); platforms without a positional vectored primitive report
> `not_supported` rather than lowering through the shared cursor.
>
> **Evidence:** P3 probe cells `p3-m1-vectored-*` / `p3-m1-wvectored-*`
> (syscall 4.000→1.000; Δwall +1576/+1699 ns per op-group at 4 KiB/k=4,
> CI-excluding; zero extra bytes; zero intermediate copies certified;
> EOF/short-count oracle equal per-buffer). Capped
> `CONDITIONAL_WORKLOAD_WIN` — no admitted real workload (#578 R0 open).
>
> **Owner decisions required:** atomicity-disclosure stance (OQ-M1-1);
> iov-fault error-table row (OQ-M1-2, needs injection evidence); k >
> IOV_MAX split-vs-reject (OQ-M1-3); naming and limit surface.

### 6. Awaiting Owner

Explicit wait state. Nothing auto-adopts; any PR stays OPEN; adoption
requires the Owner to accept the contract (and resolve the listed decisions)
per MISSION §1/§5.3.

## Candidate AC-2 — Gathered positioned read with per-request results (M1 Case B/C + M2)

One public call accepting a **set of independent positioned read requests**
at unrelated offsets, returning **per-request results**, with an explicit,
caller-visible execution policy (merge threshold T, dedup on/off). This is
the only in-SDK form that can capture M2's planning mechanisms and M1's
Case-B/C shape; it is **not** a vectored form of `read_at` and must not be
presented as one.

### 1. New Operation Contract (sketch)

- Shape class: `read_gather(file, span<ReadRequest const>, policy) ->
  per-request results`, ReadRequest = {offset, length, destination span};
  policy declares merge threshold T (default 0 = no merging) and dedup
  (default on) — the policy is **caller-visible, never hidden state**.
- Per-member admission follows SEM-03 precedence **per member** (a
  zero-length member is a logical no-op skipping offset/range checks —
  mirroring the primitive oracle
  `blocking_read_at_empty_buffer_ignores_unrepresentable_offset`);
  results are per member: {count, EOF-at-observed-position, admission
  rejection, OS error with native detail retained}; effect certainty per
  member under ERR-02.
- The executor is **primitive-class**: no loop-to-full (short counts
  surface per member; a wholly-past-EOF member reports count-0 EOF success;
  a straddling member gets exactly its prefix — reconstructed per request,
  never a group outcome).
- The planner is a pure function of (request set, policy): **stateless per
  call; no cross-call memoization** (intra-set dedup legal, cross-call
  caching remains illegal — M2-OQ6).
- Execution order: issued in sorted-extent order when merging; both orders
  are legal (SEM-04 promises no cross-call order) and the contract declares
  which is used.

### 2. Relation to v2-r1

Extends SEM-03/04/05 *per-call* semantics to a *per-member* discipline for
this one operation; SEM-04 position rules unchanged (positional reads only,
cursor untouched, checked extent arithmetic that never wraps); SEM-06
untouched (read-only); THREAD-01 unchanged (the operation is one call);
ERR-02 carried per member (`include/sluice/blocking/file.hpp:40-49`
EffectCertainty shape); W-01/INV-01: directly callable without any context
— no IoContext/request-table dependency (`docs/explicit-io-contract.md:225`,
`:309`). No existing text rewritten; `read_at` stays exactly as today. The
negative space is binding: the seven illegal forms of
`p3-families/M2_ANALYSIS.md` §2.5 (library-internal merge/memoization,
delivered over-read, error fusion, EOF mis-attribution, request-set
mutation, write-side merging, loop-to-full) define what this contract must
never become.

### 3. Performance opportunity requiring it

`IO_WORK_REDUCTION_LEDGER.md` §4–§6 `[RUN]`: (a) **dedup (ov30 trace)**:
bytes issued −34.4% (32 768 → 21 507 B/op-group, union never over-reads),
syscalls −50.0% (8.000 → 3.999), Δwall +2146.6 ± 53.0 ns (+30.3%), all
per-request digest gates green — a genuine physical-work reduction;
(b) **threshold merge**: CI-excluding wall win only at T\* = 4 KiB on g4k
warm tmpfs (+327.2 ± 35.2 ns, 8.000 → 4.548 syscalls, 1.215× amplification),
with CI-excluding regressions at T ≥ 8 KiB (1.880× amplification overtakes
the saved entries — PRED-M2-1 wall clause falsified) and at T ≤ 1 KiB; the
mechanism verdict is `COST_MIGRATION_ONLY` — the preregistered PRED-M2-5
trigger fires (planner/dispatch overhead 495.2 ns at equal syscall counts =
151% of the net / 60% of the additive-gross winning-T saving, both ≥ the
50% threshold; study §2.3); cold
btrfs unresolved (sub-MDE at every T). The capability's default policy must
therefore ship T=0/dedup-on; any nonzero T is a bound, host- and
trace-dependent tuning decision whose dispatch overhead must be charged
against the claimed saving, not a default. Planner own cost measured
+495.2 ± 41.7 ns/op-group at equal syscall counts (PRED-M2-5 falsified) —
a first-class cost the
contract's economics must disclose. Today none of this is expressible
in-SDK: the only legal forms are caller-side composition or host-side
policy outside the SDK.

### 4. Semantics / errors / lifecycle / resource boundaries / portability

- **Amplification contract:** issued bytes ≤ requested + k·T, declared and
  bounded; delivered bytes exactly per request (probe-verified by the
  per-request reference-replay oracle in every cell).
- **EOF reconstruction per member** (PRED-M2-D probe-verified on eofmix);
  **error attribution per member**: a merged syscall's OS error maps to the
  members whose extents intersect the failed transfer, uncounted attempts
  flagged unknown-effect (ERR-02) — this clause is where batching-time
  cancellation/partial-error semantics get defined (M2 Obligation 5's open
  contract question).
- **Observation-point disclosure:** a merged extent is one kernel
  observation; no atomicity/snapshot promise (consistent with SEM-04's
  existing no-snapshot language). The divergence record
  (`p3-raw/m2/p3-m2-concurrent-divergence.txt`) shows both forms
  byte-identical under equal file state while the merged form sampled 7503
  B/op-group of unrequested bytes — the consistency window is part of the
  contract text, not a footnote.
- **Buffer lifetime:** destination spans borrowed for the call's duration
  (same as today's `std::span` arguments); planner holds no state after
  return.
- **Fallback:** the T=0/dedup-off policy *is* the plain per-member pread
  loop — no silent fallback path exists to hide.
- **Portability:** pure POSIX `pread` composition; any internal `preadv`
  dispatch optimization is Case-A-shaped and invisible. Write-side planning
  is explicitly out of scope (D-P3-04: sync-coverage/side-effect boundaries
  make every write merge semantically loaded).
- **Unprobed admission shapes** (M2-OQ5): zero-length members with
  unrepresentable offsets, per-member admission rejections inside a batch,
  injected mid-batch OS errors — oracles must be defined before any product
  change; the candidate's error table is not final without them.

### 5. Design Issue / ADR Candidate (draft text)

> **Title:** Add a gathered positioned-read capability with per-request
> results and explicit planning policy (read-only; M2/M1-Case-B/C).
>
> **Proposal:** Add a single public operation accepting a span of
> independent positioned read requests and returning per-request outcomes
> (count, EOF, admission verdict, OS error, effect certainty). Execution
> policy (merge threshold T, default 0; intra-set dedup, default on) is an
> explicit argument. The operation is stateless per call; it never memoizes
> across calls, never delivers over-read bytes, never fuses errors, never
> loops to satisfy exact counts, and never touches the shared cursor.
> Merged execution reads gap bytes bounded by the declared threshold and
> discloses the single-observation consistency window.
>
> **Evidence:** P3 cells `p3-m2-dedup-ov30` (bytes −34.4%, syscalls −50%,
> wall +30.3%, CI-excluding, all per-request oracles green);
> `p3-m2-merge-sweep-g4k-*` (T\* = 4 KiB window; regressions outside —
> threshold is a bound, not a default; mechanism verdict
> `COST_MIGRATION_ONLY` via the PRED-M2-5 trigger);
> `p3-m2-eof-attribution` (per-request
> EOF reconstruction equal to unmerged); `p3-m2-planner-overhead-g4k`
> (+495.2 ns own cost at equal counts — fires the trigger);
> `p3-m2-concurrent-divergence`
> (descriptive hazard record). Capped `CONDITIONAL_WORKLOAD_WIN` (#578 R0
> open). io_uring-style scattered-offset single-syscall execution stays
> deferred (D-P3-01).
>
> **Owner decisions required:** default policy constants; consistency-window
> text; batch admission-shape oracles (M2-OQ5); naming; whether the
> capability ships at all given the narrow merge window.

### 6. Awaiting Owner

Explicit wait state. Nothing auto-adopts; any PR stays OPEN.

## Candidate AC-3 — Explicit file-to-file copy (M3, cfr-explicit engine)

One public operation copying a byte range **fd→fd inside the kernel**,
explicitly offset-addressed on both descriptors — an explicit copy
capability, never a `read_at` replacement and never returning file bytes to
a user buffer (forms F1–F4 of `p3-families/M3_ANALYSIS.md` §2.1 are
`SEMANTICALLY_NOT_EQUIVALENT` and stay forbidden).

### 1. New Operation Contract (sketch)

- Working name `copy_range_at` (Owner decides): inputs source File& +
  src offset, dest File& + dest offset, length; output a composition-class
  result: confirmed bytes + stop set {full, eof-before-full (source
  exhausted), error} + EffectCertainty mirroring ERR-02 (unknown remainder
  after an uncounted failed attempt; a failed chunk may be partially
  applied — no rollback, never report 0 for unknown).
- Zero-length = logical no-op before range checks (SEM-03 step-3 class).
- Explicit-offset only in v1; the NULL-offset cursor form is a separate
  decision (F4).
- Two-descriptor admission precedence (closed > access > range, evaluated
  on which descriptor first) must be pinned by the contract — v2-r1 has a
  one-descriptor table only (OQ-3).
- Short transfers are legal (both syscalls may return < count without
  error): loop-and-advance, confirmed bytes published exactly once
  (probe-verified discipline, P-M3-8).

### 2. Relation to v2-r1

Extends SEM-05/ERR-02 outcome discipline to a two-descriptor operation;
SEM-04 applies verbatim (positional ⇒ both cursors untouched ⇒
explicit-offset `copy_file_range` engine, not `sendfile`, which always
advances the out cursor — session-measured 1024→5120; OQ-2 Owner decision
on whether sendfile is comparator-only); SEM-03/06/07 texts untouched; no
existing contract rewritten.

### 3. Performance opportunity requiring it

`IO_WORK_REDUCTION_LEDGER.md` §7–§8 `[RUN]`: tmpfs 64 KiB×1024 chunks —
2.000 → 1.000 syscalls/chunk (count caliber), user-space traversals
2/byte → 0/byte
(structural), CPU −6.9%, wall +2054.2 ± 66.4 ns/chunk (+10.4%, CI ≠ 0);
btrfs same-fs — CPU −54.9%, wall +9508.3 ± 114.5 ns/chunk (+62.0%,
out-of-band-upper recorded as-is; reflink-acceleration attribution caveat —
magnitude not portable to non-accelerating filesystems). Today's legal
composition (`read_at`+`write_at` loop, cp1s) is wall-parity with the raw
loop (M3-GAP row) and structurally cannot reach the kernel-transfer path
without eliminating its user-buffer semantics. Cross-fs cfr is EXDEV
(support matrix) — the fallback boundary is part of the opportunity
statement, not a defect.

### 4. Semantics / errors / lifecycle / resource boundaries / portability

- **Error mapping** (session-verified rows from
  `p3-raw/m3/support-matrix.txt`): EXDEV (cross-type fs) and EOPNOTSUPP →
  explicit unsupported outcome + caller-visible fallback choice (disclosure
  6 — typed unsupported result with a separate explicit user-space loop, or
  a documented error the application composes itself; OQ-5 Owner decision;
  never a silent fallback); EINVAL (flags/overlap/non-regular) and EBADF
  (cfr `O_APPEND` dest; sendfile uses EINVAL — the errno split must be
  mapped, not leaked) → admission classes; EINTR per SEM-05. EFBIG/ENOSPC
  are man-page-based only (`NOT_RUN_WITH_REASON`: the session's sudo scope
  could not build a size-capped destination filesystem) — **the candidate's
  error table is not final until injected** (OQ-6).
- **Lifecycle/resources:** two File borrows, no user buffer, no staging
  allocation, no new runtime state.
- **Concurrency:** THREAD-01 class, no extra atomicity/snapshot
  (concurrent-modification disclosure: `NOT_RUN_WITH_REASON` — the
  descriptive divergence cell pinned by plan §4.2 disclosure 4 was never
  executed and no M3 evidence exists; the disclosure obligation remains
  open and must be closed before adoption).
- **Portability:** `copy_file_range` is Linux-specific (glibc ≥ 2.27
  wrapper emulates pre-4.5 kernels **as a user-space copy** — must be
  disclosed if relied upon); `sendfile` is non-portable by its own man page;
  WSL2 follow-up named in the candidate; sparse-source hole-expansion
  caveat (man NOTES). Chunk-size policy beyond the probed 64 KiB is
  uncharacterized (OQ-8).
- **Scope:** regular files only; one contiguous range per call.

### 5. Design Issue / ADR Candidate (draft text)

> **Title:** Add an explicit offset-addressed file-to-file copy capability
> (kernel-transfer engine, cursor-untouched).
>
> **Proposal:** Add `copy_range_at(src, src_off, dst, dst_off, len)` as a
> composition-class direct operation: confirmed bytes, stop set
> {full, eof-before-full, error}, ERR-02 effect certainty, zero-length
> no-op, two-descriptor admission precedence (ordering to be pinned).
> Engine: explicit-offset `copy_file_range` where the support matrix
> admits; unsupported combinations (EXDEV/EOPNOTSUPP, cross-type fs, non-
> regular files, `O_APPEND` destinations) surface as typed unsupported
> outcomes with an explicit, caller-chosen fallback — never silent.
> `sendfile` remains ineligible as an engine under the cursor-untouched
> contract (always advances out_fd).
>
> **Evidence:** P3 cells `p3-m3-copy-{tmpfs,btrfs}-cfr` (syscalls 2→1/chunk;
> user-space copies 2→0/byte structural; CPU −6.9%/−54.9%; wall +10.4%/
> +62.0% CI-excluding; content-equality oracle green; support-matrix gate
> re-run PASS); `p3-m3-copy-*-sendfile` (comparator rows); short-at-EOF
> descriptive cell (loop-and-advance, exactly-once confirmed bytes; executed
> in a deviating source-EOF shape — study §3.3). Capped
> `CONDITIONAL_WORKLOAD_WIN` (#578 R0 open); btrfs magnitude carries the
> reflink caveat.
>
> **Owner decisions required:** two-descriptor admission precedence (OQ-3);
> source-EOF stop-reason shape and dest-side composition (OQ-4); fallback
> surface (OQ-5); sendfile eligibility (OQ-2); cross-fs boundary
> acceptability (OQ-7); error-table completion after EFBIG/ENOSPC injection
> (OQ-6); naming.

### 6. Awaiting Owner

Explicit wait state. Nothing auto-adopts; any PR stays OPEN.

## Not filed (with reasons)

- **M4** — `apiCandidateNeeded = false`: no operation, no state, no
  observable; constants only (`p3-families/M4_ANALYSIS.md` §4). Any future
  read-shape advisory would be M2's/AC-2's filing, not M4's.
- **io_uring batched scattered-offset execution** — D-P3-01: outside
  Direct's W-01 independence boundary (no IoContext/worker in Direct) and
  no admitted workload; revisitable only through this §11/§5.3 path with a
  mechanism-justification answer.
- **Write-side range planning** — D-P3-04: side-effect/durability
  boundaries (sync coverage, SEM-06) make every write merge semantically
  loaded; a separate design candidate, not an extension of AC-2.
- **Sequential (`readv`/`writev`) vectored forms, `preadv2` flag surface**
  — OQ-M1-4/OQ-M1-5: no cells, no candidate sketches; independent design
  decisions if ever wanted.
