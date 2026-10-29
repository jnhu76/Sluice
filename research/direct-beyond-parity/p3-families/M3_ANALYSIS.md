# M3 Family Analysis — File-to-File Kernel Transfer (`copy_file_range` / `sendfile`)

- Family: P3 mechanism family M3 (MISSION.md §5, "M3 — File-to-File Kernel
  Transfer", `research/direct-beyond-parity/MISSION.md:509-524`).
- Preregistered plan of record: `P3_MECHANISM_STUDY_PLAN.md` §4 (lines 276-345,
  position + support matrix §4.1, disclosures §4.2, arms §4.3, cells/predictions
  §4.4), §5.2 accounting §6, statistics §7, verdicts §8, admission cap §9,
  API path §11.
- Semantic floor: `P0_BASELINE_AND_OWNERSHIP.md` §5 (SEM-03…07 / ERR-02
  obligation table with named oracles, lines 634-727).
- Contract anchors verified in the v2-r1 root this session:
  `docs/explicit-io-contract.md:437` (SEM-04), `:459` (SEM-05), `:562` (ERR-02).
- **Status: PREREGISTERED.** This document fixes the legal frame, the seven
  disclosures, the qualitative predictions and the API-candidate shape
  **before any M3 timing cell runs**. The only probes executed for it are the
  mandated functional-support matrix and two error-class probes (semantic
  facts, listed in §1.2); no timing, count or PMU number has been observed.
  Timing/count/copy predictions in §4 are falsifiable commitments, not
  findings.

## 1. Inputs and session-verified facts

### 1.1 Documents read

| Input | What it pinned for M3 |
|---|---|
| MISSION.md §5 M3 (lines 509-524) | Family = explicit file-copy capability study; the seven disclosure topics; "not a `read_at` replacement" |
| P3_MECHANISM_STUDY_PLAN.md §0 | Host facts (kernel 7.2.11, /tmp tmpfs, /var/tmp btrfs, 1 NUMA node, strace 7.2), prior session probe, P1 cost constants |
| P3_MECHANISM_STUDY_PLAN.md §4 | Support-matrix gate, disclosure table, arms cp1/cp1s/cp2/cp3, cells, PRED-M3-1…6 |
| P3_MECHANISM_STUDY_PLAN.md §6-§9 | Accounting calibers, statistics, verdict vocabulary, real-workload cap |
| P0_BASELINE_AND_OWNERSHIP.md §5 | Must-preserve semantics floor (SEM-03/04/05/07, ERR-02) with oracle names |
| Local man-pages (this session) | `man 2 copy_file_range` (man-pages 6.13, 2024-11-17) and `man 2 sendfile` — quoted in §2/§3 with the exact behavior each claim rests on |
| DIRECT_COST_FLOOR.md §4.1 (spot-check) | N1 4 KiB read 704.666 ns, 64 KiB 8696.847 ns (`DIRECT_COST_FLOOR.md:223-224`); wrapper constants +38/+19 instr/op and 0.128 ns/B marginal cost as pinned in plan §0 |

### 1.2 Probes executed this session (semantic facts only)

Environment verified: `uname -r` → `7.2.11-200.fc44.x86_64`; `findmnt -no
FSTYPE /tmp` → tmpfs; `/var/tmp` → btrfs; `gcc --version` → 16.2.1.

**Probe A — functional support matrix** (mandated by the ask and plan §4.1;
one-shot collector at `/tmp/m3_support_probe.c` per AGENTS.md hygiene — the
collector stays out of the tree, the evidence is quoted here; the executor
re-runs and archives the gate file `p3-raw/m3/support-matrix.txt` at P3 start):

Command: `gcc -O2 -Wall -Wextra -o /tmp/m3_support_probe
/tmp/m3_support_probe.c && /tmp/m3_support_probe`. Full matrix
{tmpfs,btrfs}² × {cfr,sendfile} × {explicit,NULL offsets} × {in-range,
past-EOF/at-EOF}, 4 KiB copy at offset 1024 of an 8 KiB source. Observed:

| Cell class | Result (kernel 7.2.11-200.fc44.x86_64) |
|---|---|
| same-fs cfr, explicit offsets, in-range | tmpfs→tmpfs **ret=4096, content MATCH**; btrfs→btrfs **ret=4096, content MATCH** — the ask's gate cells pass |
| same-fs cfr, NULL offsets, in-range | ret=4096, MATCH; shared positions used and advanced (in 1024→5120, out 1024→5120) |
| same-fs cfr, explicit offsets, past-EOF source | **ret=0, no error**; `off` arguments not advanced; positions untouched |
| same-fs cfr, NULL offsets, parked exactly at EOF | **ret=0, no error**; positions stay at EOF — the "returns 0 at EOF" fact |
| cfr, explicit mode, in-range | shared file positions **unchanged** (in 0→0, out 0→0); kernel writes back advanced `off_in`/`off_out` values (1024→5120) |
| cross-fs cfr (tmpfs↔btrfs, both directions, all offset modes) | **ret=−1, errno=18 EXDEV** every time |
| sendfile, all fs pairs incl. cross-fs, in-range | ret=4096, MATCH in **all four** src/dst combinations (splice-based; no same-fs restriction observed) |
| sendfile, non-NULL in-offset | in_fd position untouched (0→0), but **out_fd position advances** (1024→5120) — out_fd's own cursor is the implicit out-offset |
| sendfile, past-EOF / at-EOF source | ret=0, no error |

**Probe B — error classes** (`/tmp/m3_err_probe.c`, same build/run pattern):

| Error shape | Observed | Man-page anchor |
|---|---|---|
| cfr, same fd, truly overlapping ranges (in [0,4096) vs out [1024,5120)) | ret=−1, **EINVAL** | `man 2 copy_file_range` ERRORS "fd_in and fd_out refer to the same file and the source and target ranges overlap" |
| cfr, same fd, adjacent ranges (in [0,4096) vs out [4096,8192)) | **ret=4096 success** | adjacency is not overlap; legal |
| cfr, `O_APPEND` destination | ret=−1, **EBADF** (errno 9) | man ERRORS "The O_APPEND flag is set … fd_out" |
| sendfile, `O_APPEND` destination | ret=−1, **EINVAL** (errno 22) | `man 2 sendfile` ERRORS "out_fd has the O_APPEND flag set" |

EFBIG/ENOSPC/EOPNOTSUPP classes are **not probed this session** — they belong
to the executor's error-injection cells (plan §4.2 disclosure 3) and the
archive gate; the analysis below uses man-page text only for them and marks
them session-unverified.

## 2. Legal framework — mechanism forms vs the v2-r1 public contract

The Direct public contract is user-buffer I/O on one descriptor per call
(SEM-03 admission → SEM-04 range/position → SEM-05 outcome → ERR-02 progress),
plus metadata ops. M3 mechanisms move bytes **fd→fd inside the kernel**; their
ABI has no user byte buffer at all (`ssize_t copy_file_range(int fd_in, off_t
*off_in, int fd_out, off_t *off_out, size_t size, unsigned int flags)` — man
SYNOPSIS). Every legal-boundary question reduces to: which Sluice-visible
operation could this syscall serve, and does that operation exist?

### 2.1 Form-by-form adjudication

| # | Mechanism form | Verdict class | Reasoning |
|---|---|---|---|
| F1 | cfr/sendfile as an internal implementation of `read_at` / `read` (returning file bytes to a caller buffer) | **SEMANTICALLY_NOT_EQUIVALENT — impossible at ABI level** | Neither syscall can deposit bytes in a user buffer; there is no output-buffer parameter. Any "implementation" that ends with bytes in the caller's span is just a read (kernel→user copy still happens); nothing was eliminated. The ask and plan §4.1 pin this as the family's defining boundary. |
| F2 | cfr/sendfile as an internal implementation of `write_at` / `write` (taking user bytes to a file) | **SEMANTICALLY_NOT_EQUIVALENT — impossible at ABI level** | Symmetric: no input-buffer parameter; the source is an fd. User bytes cannot enter the transfer. |
| F3 | Hidden `lseek(out_fd)` + `sendfile` to emulate an explicit out-offset | **SEMANTICALLY_NOT_EQUIVALENT — non-conforming lowering** | sendfile has no out-offset argument (man SYNOPSIS: one `off_t *offset` for in_fd); the destination position is out_fd's shared cursor, always advanced when seekable ("If it's seekable, then sendfile() changes the file offset appropriately"). SEM-04 (`docs/explicit-io-contract.md:437`): a positional operation does not read or modify the shared cursor, and "hidden seek-plus-read/write is not a conforming lowering" — the same class covers hidden seek+sendfile, with the added hazard that the cursor movement is externally observable (session probe A: out 1024→5120) and racy under THREAD-01 concurrency. |
| F4 | NULL-offset cfr/sendfile loops presented as positioned (explicit-range) copy | **SEMANTICALLY_NOT_EQUIVALENT for the positioned form** | Session probe A: NULL-offset variants read **and advance both** shared cursors (in 1024→5120, out 1024→5120). A cursor-based copy is a different operation class (like sequential `read`/`write`), legal only as its own named shared-cursor contract — a design-candidate question, never a substitute for offset-addressed semantics. |
| F5 | cfr with explicit offsets as the kernel-transfer engine of a **new, explicitly named copy capability** (file→file, offset-addressed) | **Legal — REQUIRES_API_DECISION** | cfr with non-NULL `off_in`/`off_out` changes neither file position (probe A: 0→0) — SEM-04-clean; flags must be 0; regular files only. But v2-r1 has no public file-copy operation, so this is a new capability (MISSION §5.3; plan §11). |
| F6 | sendfile(file→file) inside that same new capability | **Conditionally legal — design constraint** | Works on all four fs pairs (probe A) and takes an explicit in-offset without moving in_fd's position; but it always advances the out_fd shared cursor (F3). A capability whose contract promises untouched cursors cannot use sendfile on the destination without either changing the promise or restoring the cursor (racy). cfr-explicit is the only cursor-clean kernel-transfer engine. |
| F7 | Today's legal Sluice composition: `read_at` + `write_at` per chunk through a user staging buffer (arm `cp1s`) | **Legal — the Baseline** | Pure composition of existing public ops; the §5.2 accounting Baseline (plan §4.3). Pays 2 user-space traversals/byte + 2 syscalls/chunk + Sluice admission wrappers. |
| F8 | Raw native loop `::pread`+`::pwrite` (arm `cp1`) | Legal comparator (N1-class) | The mechanism floor for the same observable work. |

**Non-goals confirmed illegal/not-applicable for this family** (recorded so
nobody re-litigates): returning file bytes without a user-buffer copy;
crossing into M1/M2 territory (scatter/gather, range planning) — M3 only ever
moves one contiguous range fd→fd.

### 2.2 Error-class table (what a candidate must disclose; session-verified rows marked)

| Native errno | When (man + session) | Candidate-facing class |
|---|---|---|
| EXDEV (18) | cfr across filesystems of different types (session: tmpfs↔btrfs, all modes). Since-5.19 wording: cross-fs only when same type and both support it. sendfile unaffected (probe A). | `not_supported`-with-reason at the mechanism level; triggers the **explicit** fallback decision (disclosure 6) — never a silent user-space fallback |
| EINVAL (22) | cfr: flags≠0; same-file overlapping ranges (session-verified); sendfile: O_APPEND out_fd (session-verified); non-regular fds | `invalid_argument` admission class (mirrors SEM-04 native-invalid) |
| EBADF (9) | cfr: O_APPEND out_fd (session-verified; note the errno differs from sendfile's EINVAL — must be mapped, not leaked raw if the contract distinguishes) | admission/access class |
| EFBIG / ENOSPC | dest beyond max size / no space (man; **not session-verified** — executor's size-capped-scratch cells) | terminal write-side error; ERR-02 unknown-remainder discipline applies (bytes confirmed so far are the prefix; the failing chunk's effect may be partially applied — no rollback) |
| EOPNOTSUPP | filesystem lacks the operation (man, since 5.19) | same handling family as EXDEV |
| EINTR | interruption without a completed count | SEM-05 retry discipline: retry only with no completed count, never replay confirmed bytes |

## 3. The seven mandatory disclosures, each mapped to evidence

1. **File type & filesystem support.** Gate = support matrix (plan §4.1),
   archived by the executor at P3 start as `p3-raw/m3/support-matrix.txt`.
   Session evidence (§1.2 probe A): cfr same-fs works on both tmpfs and
   btrfs; cfr cross-type-fs = EXDEV; sendfile works on all four pairs.
   Scope: **regular files only** (man EINVAL: "Either fd_in or fd_out is not
   a regular file"; EISDIR for directories); pipes/sockets and non-regular
   types are out of family scope. Sparse sources: cfr "may expand any holes"
   (man NOTES) — recorded as a content-shape caveat for the candidate.
2. **Short-transfer behavior.** Both syscalls may return less than requested
   without error (man cfr RETURN VALUE: "This could be less than the size
   originally requested"; man sendfile: "a successful call … may write fewer
   bytes than requested"). Handling shape: loop-and-advance — advance both
   offsets (or positions) by the returned count, publish it as confirmed
   progress, continue. Evidence: executor cells `p3-m3-short-error-cells`
   (4 KiB/64 KiB chunks vs 64 MiB fixture; plan §4.2) record observed short
   counts. Not yet measured — prediction P-M3-B2 pins the qualitative rule.
3. **Offset & error semantics.** Session-verified (probe A/B): explicit-offset
   cfr does not move shared cursors and writes back advanced off values only
   on progress; past-EOF source = ret 0 success (not error); NULL-offset
   variants consume and advance shared positions; same-file overlap = EINVAL;
   O_APPEND dst = EBADF (cfr) / EINVAL (sendfile). Error classes and their
   candidate mapping: §2.2. EFBIG/ENOSPC/EOPNOTSUPP pending executor cells.
4. **Concurrent-modification consistency.** No atomicity or snapshot across
   concurrent writers of source or destination — kernel transfer is not a
   locked transaction, same standing as THREAD-01/SEM-04 ("no additional
   atomicity/snapshot guarantee", `docs/explicit-io-contract.md:455-456`).
   Evidence: descriptive divergence cell (writer mutates source mid-copy;
   plan §4.2 row 4) — recorded, no CI claim. Contract implications go to the
   §5 candidate, not to probe interpretation.
5. **Partial-transfer responsibility.** After any short or error return, the
   capability must publish confirmed bytes exactly once, mirroring ERR-02
   (`docs/explicit-io-contract.md:562`): a returned count is exact progress;
   an OS error after confirmed prefix keeps the prefix and marks the
   remainder **unknown** (a failed cfr attempt may have partially written the
   destination — "no trustworthy count" ⇒ unknown, never zero, never
   rollback). This is the confirmed-bytes outcome shape the candidate must
   define (see §5 element 1) — a design-candidate question, pinned before
   measurement.
6. **Fallback path.** Cross-fs/unsupported combos (EXDEV/EOPNOTSUPP, and any
   cell the support matrix rejects) record `NOT_RUN_WITH_REASON` — no
   substitution (plan §4.1). A product candidate must surface the
   unsupported condition to the caller and, if it offers a user-space loop
   fallback, make that an **explicit, observable choice** — never silent
   (plan §4.2 row 6; §5 element 4).
7. **Real CPU/memory-traffic reduction.** The §5.2 accounting table with both
   sides measured/attributed (plan §6): CPU/byte (rusage), instr/byte (PMU),
   syscalls/byte (`strace -c`), user-space copies (structural certification:
   2/byte for user-space loops — kernel→user then user→kernel, each byte
   crosses the ABI twice; 0/byte for kernel transfer — no user buffer exists
   in the call shape), wall/byte (uninstrumented ABBA). Kernel-internal byte
   movement is **attributed, not measured** (PRED-M3-6): syscall identity ×
   counts + rusage split + PMU deltas; no kernel tracing in P3 scope.

## 4. Preregistered predictions and probe criteria (pinned before any timing cell)

Each prediction states its falsifiable form. Grounding constants are the
plan-§0 pinned P1 facts (704.666 ns 4 KiB read N1; 0.128 ns/B marginal;
+38/+19 instr/op wrapper constants) — cost-model inputs, not M3 measurements.

| ID | Prediction (qualitative/structural where marked S) | Falsifiable form |
|---|---|---|
| P-M3-1 (S) | User-space copy structure: `cp1`/`cp1s` perform **2 user-space traversals per byte** (pread kernel→user staging + pwrite user→kernel); `cp2`/`cp3` perform **0** (no user buffer in the syscall shape). | Structural certification by call-shape enumeration (plan §6 row) + PMU instr/byte corroboration: cp1 instr/byte ≫ cp2 instr/byte at equal bytes; any certified staging-buffer bypass in cp1/cp1s or user-buffer appearance in cp2/cp3 falsifies the row. |
| P-M3-2 (S) | Syscalls/chunk (64 KiB): cp1 = 2 (pread+pwrite); cp1s = 2 (admission is user-space); cp2 = 1 (cfr); cp3 = 1 (sendfile, explicit in-offset, out advanced by cursor) + **0** lseek when the loop relies on the out cursor, else +1 lseek/chunk if repositioning. | `strace -c` integral counts, exactly equal across ≥3 diagnostic runs (plan §7.6). Any third syscall per chunk (unexpected lseek/madvise) falsifies the loop shape. |
| P-M3-3 | CPU/byte: cp2 vs cp1 on warm tmpfs rusage CPU (user+sys) reduced **≥30%**; PMU instructions/byte reduced **≥2×** (plan PRED-M3-1; answers M3-Q1's thresholds). | Paired diagnostic arms, ≥3 runs, ratios repeat within 5% (plan §7.6). Falling short of either threshold refutes the copy-elimination cost model on this host. |
| P-M3-4 | Wall/byte on tmpfs improves ≥10% (band); on warm btrfs CPU/byte −20% or better, wall within [−5%, +30%] (plan PRED-M3-2/3). A wall regression ≥5% with a CPU win is recorded as-is and the verdict stays CPU-scoped (answers M3-Q2's recording rule). | 16 ABBA pairs, CI excluding 0 and \|median\| ≥ copy-track MDE′ (fresh A/A floor, plan §7.2). CI-including-0 ⇒ NO_CLAIM at wall caliber; the CPU-scoped record stands or falls on P-M3-3 alone. |
| P-M3-5 | \|cp3 − cp2\| wall/byte within 2× the copy-track MDE′ (same splice class); if outside, the difference must be attributed (kernel path: sendfile=splice vs cfr=fs copy path) (plan PRED-M3-4; M3-Q3). | ABBA deltas vs 2·MDE′; an unattributable exceedance falsifies the "same splice class" grouping. |
| P-M3-6 | Sluice composition gap: cp1s − cp1 ≈ wrapper constants (≈+38 read /+19 write instr per chunk ⇒ ≈+57 instr/chunk aggregate class); wall sub-MDE per chunk; aggregate over 1024 chunks reported only at the caliber that resolves (plan PRED-M3-5; M3-Q5). | PMU cross-arm delta within [−50%, +150%] of the predicted ≈+57 instr/chunk class; a delta outside that band (e.g. ≫, indicating hidden per-chunk work) refutes the wrapper-constant model for the copy loop. |
| P-M3-7 (S) | Offset/EOF semantics reproduce the session matrix exactly at archive time: explicit past-EOF ⇒ ret 0 success, off args unadvanced; NULL at EOF ⇒ ret 0; EXDEV cross-type-fs; overlap EINVAL; O_APPEND EBADF/EINVAL. | Executor's archived `support-matrix.txt` must match §1.2 row-for-row; any divergence invalidates the gate and blocks family timing (plan §4.1 "the archived execution-time run is the gate"). |
| P-M3-8 (S) | Short counts never carry an error; every observed short count is handled by loop-and-advance with confirmed-bytes publication; no short count is retried as a replay. | Executor short/error cells: strace-visible syscall sequence strictly advance-offsets monotonic; any replay of an already-confirmed range falsifies the loop discipline (M3-Q4's observed-behavior half). |
| P-M3-9 (S) | Kernel work attribution only: kernel-internal byte movement is never reported as a measured count (plan PRED-M3-6). | Any M3 output claiming a measured kernel byte count without kernel tracing falsifies the accounting discipline. |
| P-M3-10 (S) | btrfs same-fs cfr may engage fs copy acceleration (man NOTES: reflink/server-side-copy). If engaged, cp2 CPU/byte on btrfs may be far below the copy-elimination model — this must be **disclosed as an attribution caveat**, not claimed as general mechanism gain. | Data-independent timing or near-zero sys-time signature across differing payloads on btrfs indicates acceleration; absent such signature, prediction of acceleration not engaged. Recorded descriptive either way (open question OQ-1). |

**Problem-list mapping.** M3-Q1 → P-M3-3; M3-Q2 → P-M3-4; M3-Q3 → P-M3-5;
M3-Q4 → P-M3-7 + P-M3-8 + the §5 outcome shape; M3-Q5 → P-M3-6.

## 5. API candidate needed — yes (`apiCandidateNeeded = true`)

The measured opportunity (if P-M3-3/4 hold) is reachable **only** through a
new public operation: v2-r1's public surface has no fd→fd copy; `read_at` +
`write_at` (F7) cannot reach the kernel-transfer path without eliminating the
user-buffer semantics that define them (F1/F2). Verdict path: mechanism
verdict from the cells + `REQUIRES_API_DECISION` for productization; overall
record capped at `CONDITIONAL_WORKLOAD_WIN` (no admitted #578 R0 workload,
plan §9). Six-element design points (MISSION §5.3; plan §11):

1. **New Operation Contract.** Working name `copy_range_at` (Owner decides):
   inputs source File& + src offset, dest File& + dest offset, length;
   output a composition-class result: confirmed bytes + stop set {full,
   eof-before-full (source exhausted), error} + EffectCertainty mirroring
   ERR-02 (unknown remainder after an uncounted failed attempt; no rollback).
   Zero-length = logical no-op before range checks (SEM-03 step-3 class).
   Explicit-offset only in v1 of the candidate — the NULL-offset cursor form
   is a separate decision (F4).
2. **Relation to v2-r1.** Extends SEM-05/ERR-02 outcome discipline to a
   two-descriptor operation; SEM-04 applies verbatim (positional ⇒ cursors
   untouched ⇒ cfr-explicit engine, not sendfile, per F6); no existing
   SEM-03…07 text is rewritten; admission precedence must be defined for the
   **pair** of files (closed > access > range evaluated on which descriptor
   first — an ordering the candidate must pin; today's contract has a
   one-descriptor table only).
3. **Performance opportunity requiring it.** The P3 §5.2 table for cells
   `p3-m3-copy-tmpfs` / `-btrfs` / `-crossfs` (CPU/byte, instr/byte,
   syscalls/byte, structural copies 2→0/byte, wall/byte), plus the cp1s−cp1
   composition gap (P-M3-6) that today's legal form pays.
4. **Semantics/errors/lifecycle/resource/portability design.** Error mapping
   per §2.2 with EXDEV/EOPNOTSUPP → explicit unsupported outcome + caller-
   visible fallback choice (disclosure 6); EINTR retry without completed
   count; short-count loop-and-advance publishing confirmed bytes
   (disclosure 5); concurrency = THREAD-01 class, no extra atomicity
   (disclosure 4); resources: two File borrows, no user buffer, no staging
   allocation; portability: cfr is Linux-and-glibc-specific (glibc 2.27
   wrapper emulates pre-4.5 kernels — emulation is a user-space copy and
   must be disclosed), sendfile "should not be used in portable programs"
   (man STANDARDS/PORTABILITY) — WSL2 follow-up named in the candidate;
   sparse-hole expansion caveat (man NOTES).
5. **Design Issue / ADR Candidate.** Filed as such, referencing this document
   and the cell evidence; no contract text changes inside the candidate.
6. **Awaiting Owner.** Explicit wait state; nothing auto-adopts; no merged PR.

## 6. Open questions (unresolved — recorded, not guessed)

- **OQ-1.** Does btrfs engage reflink-class acceleration for same-fs cfr on
  this host, and how would the candidate detect and disclose it (timing
  signature vs FIEMAP-style extent inspection is out of P3 scope)? Affects
  whether the btrfs CPU win generalizes to non-accelerating filesystems.
- **OQ-2.** Is sendfile eligible at all under a cursor-untouched contract
  (F6), given it always advances the out_fd shared cursor and dup does not
  escape the shared open-file-description position? Or is cfr-explicit the
  sole sanctioned engine, making sendfile a comparator-only arm?
- **OQ-3.** Two-descriptor admission precedence: which file's closed/access/
  range verdict wins when both would reject? No v2-r1 anchor exists; Owner
  semantics decision (§5 element 2).
- **OQ-4.** Should source-EOF-before-full keep the SEM-05 `eof_before_full`
  name/shape, and how do destination-side short/error stop reasons compose
  with it (dest is side-effecting; source is not)? Design decision for the
  candidate.
- **OQ-5.** Fallback surface shape: typed unsupported outcome with a separate
  explicit `copy_range_fallback`-style loop, or a documented error the
  application composes itself? (Disclosure 6; mechanism-justification gate
  applies to any runtime capability probe.)
- **OQ-6.** EFBIG/ENOSPC/EOPNOTSUPP observed behavior on this host is
  session-unverified (§2.2); the executor's error-injection cells must close
  this before the candidate's error table is final.
- **OQ-7.** Cross-fs sendfile works (probe A) but cross-fs cfr is EXDEV: if
  the candidate is cfr-only (OQ-2), cross-fs copies always take the
  fallback — is that boundary acceptable, or does it argue for a documented
  sendfile-based cross-fs mode with cursor disclosure?
- **OQ-8.** Chunk-size policy for a product loop (plan pins 64 KiB for
  probes): systematic short-count regimes (e.g. memory pressure, fs limits)
  were not characterized; P-M3-8 pins the handling rule, not the
  distribution.

## 7. Verdict vocabulary applicability (from plan §8, M3-specific reading)

- `MECHANISM_WIN_EARNED` requires the §5.2 table complete on both sides +
  count-caliber work reduction + wall/CPU ≥ MDE′ on a warm cell + oracle
  green. For M3 the correctness oracle is the checksum-equality group
  {cp1, cp1s, cp2, cp3} + source (`content_ok=1` class, plan §4.3) **and**
  the support-matrix gate (P-M3-7).
- `COST_MIGRATION_ONLY` is mandatory if syscall reduction (2→1/chunk) lands
  without attributed CPU/work reduction — e.g. if the kernel path re-copies
  pages such that total CPU is unchanged. The CPU-scoped verdict (P-M3-4)
  exists precisely so a wall/CPU regression cannot be hidden behind
  syscall-count wins.
- `SEMANTICALLY_NOT_EQUIVALENT` is pre-assigned to forms F1-F4 (§2.1).
- `REQUIRES_API_DECISION` is the family's productization gate (§5); with no
  admitted real workload the overall cap is `CONDITIONAL_WORKLOAD_WIN` at
  best (plan §9).
- Unsupported combos: `NOT_RUN_WITH_REASON`, never substituted (plan §4.1).
