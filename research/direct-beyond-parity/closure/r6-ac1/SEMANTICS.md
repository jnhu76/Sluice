# R6/AC-1 Semantic Table — positional vectored read/write (`preadv`/`pwritev`)

- Issue: Sluice #605 (AC-1, M1 Case A). This table pins the **accurate
  semantic boundary** of the candidate capability on E5, measured by the
  independent probe `probes/r6ac1_probe.c` (no Sluice headers, raw
  `SYS_preadv`/`SYS_pwritev` unless a row says wrapper).
- Legend per campaign convention: `[RUN]` = executed in this session (raw
  file cited); `[TRANSCRIBED]` = copied from an archived record, source
  cited; `[UNCERTAIN]` = not pinned here.
- Host: E5, kernel 7.2.11-200.fc44.x86_64, glibc 2.43, `IOV_MAX` 1024,
  page 4096 (`raw/env-pin.txt`, `raw/r6ac1-env.txt`) `[RUN]`.

## 1. Kernel ground truth (each row = one probe cell)

| # | Semantic row | Measured kernel behavior on E5 | Evidence `[RUN]` |
|---|---|---|---|
| S1 | One call = one contiguous extent, buffers filled/written in array order | `preadv` 3×4096 @0 on a 12288 B pattern file: ret 12288, each buffer exactly matches its file-offset slice; `pwritev` 3×2048 @1024 lands one contiguous in-order extent (content verified by `pread`); glibc wrapper identical | `r6ac1-array-order.txt` |
| S2 | Short count = prefix fill across iov boundaries, trailing buffers untouched | 6144 B file, request 3×4096: ret 6144; buf0 full (0–4095), buf1 first 2048 B (4096–6143), buf2 fully untouched; 8 B-before-EOF straddle: ret 8, trailer untouched | `r6ac1-short-read.txt` |
| S3 | EOF is success 0 | ret 0 at EOF and wholly past EOF, errno 0; no error is raised | `r6ac1-short-read.txt` (`eof0_*`, `past_eof_*`) |
| S4 | Zero-length iov entries are skipped, no shift of order | `[0, 4096, 0]` → ret 4096, only the middle buffer filled; all-zero entries (k=3) → ret 0, success | `r6ac1-short-read.txt` (`mixed_zero_len_*`), `r6ac1-null-empty.txt` (`read_all_zero_len_*`) |
| S5 | `iovcnt = 0` → success 0, no bytes, no effect | ret 0 errno 0 with `iov = NULL` and with a valid pointer (read and write; write leaves size unchanged) | `r6ac1-null-empty.txt` |
| S6 | `iovcnt` boundary is exactly `IOV_MAX` = 1024 | k=1023 OK (1023), k=1024 OK (1024), k=1025 → `-1/EINVAL(22)`, k=4096 → `-1/EINVAL`; write twin identical; post-state unchanged (file size 1024 after the failed k=1025 attempt) | `r6ac1-iovmax.txt`, identical on btrfs |
| S7 | Positive Σlen above the kernel per-call cap is **silently clamped**, surfacing as a short success | Single 2 GiB entry on a sparse 4 GiB tmpfs file: requested 2147483648 → ret **2147479552** = `INT_MAX & ~PAGE_MASK` (kernel `MAX_RW_COUNT`); single `SSIZE_MAX` entry → ret 4096 (file-bounded short success, no error); 1024×1 MiB positive sum → short success, no error | `r6ac1-sum-overflow.txt` |
| S8 | Σlen that is negative as `ssize_t` (2×2⁶²) is rejected before transfer | ret −1 **EFAULT(14)** on this kernel — the overflow makes the user range wild, so it arrives as an access-range fault, not the man page's `EINVAL` | `r6ac1-sum-overflow.txt` (`overflow_negative_*`) |
| S9 | `iov = NULL` with `iovcnt ≥ 1` | ret −1 EFAULT(14) | `r6ac1-null-empty.txt` |
| S10 | Mid-transfer iov fault, **read**: short success, not an error | `[valid 4096, PROT_NONE 4096, valid 4096]` on 12288 B file → **ret 4096**, errno 0, buf0 fully and correctly filled, buf2 untouched; identical on tmpfs and btrfs; glibc wrapper identical | `r6ac1-iov-fault-read.txt`, `r6ac1-iov-fault-read-btrfs.txt` |
| S11 | Mid-transfer iov fault, **write**: outcome is **filesystem-dependent** | tmpfs: ret 4096, errno 0, valid prefix landed (size 4096, content verified). btrfs: ret −1 **EFAULT(14)**, size **0** — nothing landed. Both stable across 3 runs each | `r6ac1-iov-fault-write.txt`, `r6ac1-iov-fault-write-btrfs.txt`, `-btrfs-rep2/3.txt`; tmpfs reps in-session |
| S12 | Fault with zero bytes transferable → clean EFAULT, no partial effects | First-entry fault: ret −1 EFAULT, all guard bytes intact (read); single PROT_NONE source: ret −1 EFAULT, size 0 (write) | `r6ac1-iov-fault-read.txt` (`first_fault_*`, `single_fault_*`), `r6ac1-iov-fault-write.txt` (`single_fault_*`) |
| S13 | Partial write by external limit is a **silent clamp** (no signal, no error); a fully-blocked write raises SIGXFSZ + `EFBIG` | `RLIMIT_FSIZE` = 100: `pwritev` 3×4096 → ret 100, errno 0, SIGXFSZ count 0, size 100, prefix verified; second attempt at the limit → ret −1 EFBIG(27), SIGXFSZ count 1, size unchanged | `r6ac1-fsize-partial-write.txt`, identical on btrfs |
| S14 | EINTR (seccomp-injected `SECCOMP_RET_ERRNO`) → −1/EINTR with zero effect | ret −1 errno 4 both directions; destination guard bytes intact; write leaves size 0; unfiltered parent control succeeds | `r6ac1-eintr.txt` |
| S15 | Shared cursor is not moved, including through `dup` | cursor 100 before/after `preadv` and `pwritev`; `dup` fd sees 100; control `read` advances | `r6ac1-cursor.txt` |
| S16 | Negative offset and `INT64_MAX` offset → EINVAL | ret −1 EINVAL(22) both | `r6ac1-cursor.txt` |
| S17 | Non-seekable fd → ESPIPE | pipe: ret −1 ESPIPE(29) both directions | `r6ac1-nonseekable.txt` |
| S18 | Whole-extent atomicity vs a concurrent buffered writer: **not observable** — both forms tear at the same low rate | 1 MiB region, 8×128 KiB, writer flipping epoch tags every ~50 µs, 5000 iterations × 3 runs: `preadv` torn 0/1/2, 8×`pread` torn 2/2/1. A single `preadv` does **not** provide a cross-page snapshot on this host | `r6ac1-atomicity.txt`, `-rep2/3.txt` (descriptive, no rate claim) |

## 2. Mapping to the v2 contract (no v2 text is changed)

| v2 anchor | Mapping for the AC-1 candidate (draft, awaiting Owner) |
|---|---|
| SEM-03 admission precedence | One verdict per call, evaluated once over Σlen: closed → `invalid_state`; access illegality → `invalid_argument`; Σlen = 0 (or zero buffers) → logical no-op, success 0, **no OS call** (S4/S5 make the kernel's own zero-total behavior unreachable, as SEM-03 step 3 requires); else checked `offset` and `offset + Σlen − 1` (no overflow, same arithmetic class as `range_is_valid` in `include/sluice/detail/file_semantics.hpp:37-43`); declared `IOV_MAX` limit → SEM-04 stricter-limit branch **before acceptance** (S6). |
| SEM-04 position | Holds natively: cursor untouched incl. through `dup` (S15); hidden `lseek`+`readv` lowering remains forbidden. Checked extent arithmetic over the whole extent; a stricter declared execution limit (`IOV_MAX`) is `not_supported` before acceptance; a primitive-level short count is legal and must never be looped away. |
| SEM-05 short/EOF | S2/S3 match the table rows exactly: short count = successful progress, prefix derivable in array order from the single total; nonempty read returning 0 = EOF success; no loop-to-full. |
| SEM-05 EINTR | S14: EINTR arrives as −1/EINTR with zero effect; the library may retry only when the attempt reports no completed count (existing `retry_on_eintr` discipline, `src/blocking_file.cpp:100-102`). "EINTR after a partial transfer" is not producible on regular files (NOT_RUN register, `E5_ORACLE.md` §3). |
| ERR-02 effects | S10/S11/S12 pin the effect rows: a mid-transfer read fault is a short success (confirmed bytes exact); a write that returns −1 after a mid-transfer fault has **filesystem-dependent** confirmed bytes — the result must report effect uncertainty for the remainder, never an invented count. |
| SEM-06 durability | Unchanged: temporal coverage over confirmed writes; gather form does not alter sync semantics. |
| THREAD-01 | Per-call rules carry over; no cross-call order promised. The OQ-M1-1 disclosure duty is resolved by measurement in §3 below. |
| W-01 independence | Direct blocking form, no context, no request path — unchanged. |

## 3. Atomicity stance (OQ-M1-1) — what may and may not be said

The man page sentence "readv is guaranteed to read a contiguous block of
data from the file" `[TRANSCRIBED: man 2 readv, Linux man-pages, session
copy archived in M1_ANALYSIS §0]` describes the kernel's single-call shape,
**not** a cross-page snapshot guarantee. Measured on E5 `[RUN, S18]`: a
single `preadv` over 1 MiB against a concurrently rewriting thread
produced torn observations (mixed epoch tags) at the same order of
frequency as eight separate `pread`s (0–2 vs 1–2 in 5000 iterations).

Consequences for the candidate text:

1. The contract must **not** promise un-torn whole-extent reads. It
   inherits SEM-04's "no additional atomicity/snapshot guarantee" verbatim.
2. The kernel's single-syscall shape (fewer observation points than k
   separate calls) may be disclosed as an implementation fact with no
   semantic weight — the same class of disclosure as the existing
   non-vectored primitive.
3. No Sluice test may assert tear-freedom of the vectored form against a
   concurrent writer; the hazard record (`r6ac1-atomicity*.txt`) is the
   negative evidence a reviewer can cite.

## 4. The kernel clamp is part of the semantic boundary

S7 means a single `preadv` — like today's `pread`-backed `read_at` — does
not transfer requests above ≈2 GiB (`MAX_RW_COUNT = INT_MAX & ~PAGE_MASK`
on x86-64) in one call; the excess silently becomes a short count. The
candidate must treat this exactly like SEM-05 already does for the
non-vectored primitive (short count is legal progress; the library must
not loop and must not pre-truncate the request silently). No new decision
is required here; the disclosure belongs in the capability's documentation
row, not in admission rejection.
