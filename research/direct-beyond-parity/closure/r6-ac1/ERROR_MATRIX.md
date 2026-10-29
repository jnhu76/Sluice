# R6/AC-1 Error Matrix — positional vectored read/write on E5

Every "observed" column is a probe measurement from this session
(`probes/r6ac1_probe.c`, raw files under `raw/`, tmpfs unless the row says
btrfs). The "candidate mapping" column is a **draft** for the AC-1
capability and reuses the existing error vocabulary only
(`include/sluice/error.hpp`): `invalid_state`, `invalid_argument`,
`not_supported`, `interrupted`, `backend_error` (+ `os_errno` preserved),
etc. Nothing here changes any existing mapping.

Order of rows follows the proposed admission precedence (SEM-03 steps 1–4,
then declared-limit and execution-time rows).

| # | Condition | Kernel behavior (observed) | Candidate mapping (draft) | Evidence `[RUN]` |
|---|---|---|---|---|
| E1 | Closed File | not reachable in probe (library-level); SEM-03 row | `invalid_state`, before any OS call | SEM-03 table (`docs/explicit-io-contract.md:400-406`) |
| E2 | Access illegality (read on write_only / write on read_only) | not reachable in probe; SEM-03 row | `invalid_argument`, before any OS call | SEM-03 table |
| E3 | Σlen = 0 or zero buffers | kernel: ret 0 success (S4/S5) — but candidate must **not** reach the kernel | success 0, no OS call, range checks skipped | `r6ac1-null-empty.txt`, `r6ac1-short-read.txt` |
| E4 | `offset` or `offset + Σlen − 1` unrepresentable (incl. negative / `INT64_MAX` native offset) | −1/EINVAL(22) (`INT64_MAX` row); negative never reaches kernel in candidate | `invalid_argument` via checked arithmetic before the call (same class as `checked_posix_offset`, `include/sluice/detail/file_semantics.hpp:45-49`) | `r6ac1-cursor.txt` |
| E5 | `iovcnt` > `IOV_MAX` (1024 here) | −1/EINVAL(22) at k=1025, k=4096; k=1024 exact boundary OK; post-state unchanged | declared stricter limit → `not_supported` **before acceptance**, or explicit split — Owner decision OQ-M1-3 (see `OWNER_DECISION_PACKET.md` §3) | `r6ac1-iovmax.txt` (+ btrfs identical) |
| E6 | Σlen negative as `ssize_t` (2×2⁶² entries) | −1/EFAULT(14) — overflow materializes as a wild user range, **not** the man page's EINVAL | `invalid_argument`: the candidate's own checked Σ arithmetic (size_t accumulate with overflow check) rejects before any call — the kernel EFAULT path is unreachable by construction; mapping recorded anyway: EFAULT → `backend_error` (`os_errno=EFAULT`) via existing table | `r6ac1-sum-overflow.txt`; man-page EINVAL row `[TRANSCRIBED: man 2 readv ERRORS]` |
| E7 | Σlen positive > kernel `MAX_RW_COUNT` (≈2 GiB) | **silent clamp**: ret 2147479552 for a 2147483648 request; `SSIZE_MAX` single entry → file-bounded short success | legal SEM-05 short count; no rejection, no library-side truncation; disclosure row only | `r6ac1-sum-overflow.txt` |
| E8 | `iov = NULL`, `iovcnt ≥ 1` | −1/EFAULT(14) | caller precondition (valid memory) — not dynamically detected per SEM-03; documenting the native row for completeness: `backend_error`/EFAULT | `r6ac1-null-empty.txt` |
| E9 | iov entry points at unmapped/PROT_NONE memory, first entry | −1/EFAULT(14), zero effect (guards intact / size 0) | `backend_error` (`os_errno=EFAULT`); effects = none observed, contract reports the primitive error; confirmed bytes 0 because nothing transferred (measured) | `r6ac1-iov-fault-read.txt` (`first_fault_*`, `single_fault_*`), `r6ac1-iov-fault-write.txt` (`single_fault_*`) |
| E10 | iov fault **after** bytes transferred, read | **short success**: ret 4096 for `[valid, PROT_NONE, valid]`; bytes before the fault confirmed, rest untouched; tmpfs = btrfs; wrapper = raw syscall | success(count); this is an ordinary SEM-05 short count, **not an error** — no special mapping needed | `r6ac1-iov-fault-read.txt`, `-btrfs.txt` |
| E11 | iov fault **after** bytes transferred, write — **fs-dependent** | tmpfs: ret 4096, prefix landed (stable ×3). btrfs: ret −1/EFAULT(14), size 0 (stable ×3) | tmpfs shape → success(count); EFAULT shape → `backend_error`/EFAULT with **unknown effect** for the remainder (ERR-02: never report 0 for an uncounted attempt). The contract must not document either outcome as guaranteed across filesystems | `r6ac1-iov-fault-write.txt`, `-btrfs.txt`, `-btrfs-rep2/3.txt` |
| E12 | External size limit (`RLIMIT_FSIZE`) partially blocks a write | silent clamp: ret 100 of 12288, errno 0, **no SIGXFSZ** | legal short count (SEM-05), native `os_errno` path untouched | `r6ac1-fsize-partial-write.txt` (+ btrfs) |
| E13 | External size limit fully blocks a write | −1/EFBIG(27) + SIGXFSZ raised once; size unchanged | `backend_error` (`os_errno=EFBIG`); SIGXFSZ disposition is process state, not library business — document only | `r6ac1-fsize-partial-write.txt` |
| E14 | Interrupted call (seccomp `SECCOMP_RET_ERRNO(EINTR)`) | −1/EINTR(4), zero effect both directions; buffer guards intact; size unchanged | `interrupted` via existing EINTR mapping; library may retry only without a completed count (SEM-05) | `r6ac1-eintr.txt` |
| E15 | Non-seekable descriptor | −1/ESPIPE(29) both directions | out of the regular-file profile; recorded as the kernel row (Sluice `File` opens paths; kind `other` exists in `FileInfo`, so the row is boundary documentation, not an admitted input) | `r6ac1-nonseekable.txt` |
| E16 | No space on device (`ENOSPC`) | **not injected** — requires a size-capped filesystem (mount/quota privileges unavailable, same scope as P3's M3 register) | `no_space` via existing `{ENOSPC, EDQUOT}` mapping — mapping table only, **no measured row** | NOT_RUN_WITH_REASON below |
| E17 | `preadv2` flag surface (`RWF_*`) | out of scope (OQ-M1-5, #605 text) | none — separate decision | not probed by design |

## Native errno → canonical code reference used by the draft

The candidate reuses `canonical_error_code` unchanged
(`include/sluice/error.hpp:66-88`): EINTR → `interrupted`; ENOSPC/EDQUOT →
`no_space`; EACCES/EPERM → `permission_denied`; everything else —
including EFAULT, EFBIG, ESPIPE, EINVAL arriving from the kernel despite
library-side admission — → `backend_error` with `os_errno` preserved.

## NOT_RUN_WITH_REASON register

| Item | Reason | Gap owner |
|---|---|---|
| `ENOSPC`/`EDQUOT` injection during a vectored write | Needs a size-capped destination filesystem (loop-mount/quota); the session has no mount privileges. Candidate's error table row E16 is mapping-based until injected. | Owner/environment follow-up (same class as P3 M3's register) |
| "EINTR with a partially transferred count" | Not producible on regular files by any known kernel path: the syscall returns either a count (success) or −1/EINTR. POSIX requires an interrupted transfer that moved data to return the count `[TRANSCRIBED: man 2 read / POSIX.1-2017 read()]`. The probe pins the −1/EINTR-with-zero-effect half (E14); the count half is pinned by every successful cell. | None — structural, documented |
| btrfs/generic-fs coverage of E10/E11 beyond tmpfs+btrfs | Two filesystems measured; the tmpfs-vs-btrfs **divergence** proves the row is fs-dependent, so no single-fs answer can be promoted to contract. Wider fs matrices only widen the disclosure; they cannot remove it. | Owner disclosure stance (OQ-M1-2) |
| WSL2 / non-Linux profiles | Out of E5 scope by mission (§1: no WSL2 runs this round); portability rows in `API_DRAFT.md` §4 are platform-availability facts, not measurements. | WSL2 handoff |
