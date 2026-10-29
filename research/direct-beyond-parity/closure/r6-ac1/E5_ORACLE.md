# R6/AC-1 E5 Oracle — what each probe cell distinguishes, and what a product test must assert

The oracle level is the lowest sufficient one: raw syscalls against real
files on two filesystems. These cells are **semantic oracles**, not
implementation mirrors: each names an observable a caller can check without
knowing anything about Sluice internals. If AC-1 is ever adopted, the
deterministic regression set for the new operation should assert exactly
these observables through the public call — nothing about private state,
call sequences or layout.

Run identity: `raw/env-pin.txt` (source sha256
`9e364ff7…cf56a9`, binary sha256 `ccb0334c…62a94` reproduced bit-identical
from source, kernel 7.2.11-200.fc44.x86_64, glibc 2.43). Reproduce with
`probes/run_all.sh`.

## 1. Oracle cells

| Cell (raw file) | Invariant / failure mode it distinguishes | Observed outcome (all `[RUN]`) | Product-oracle obligation if AC-1 is adopted |
|---|---|---|---|
| `r6ac1-env.txt` | Pins the limit surface the contract declares | `sysconf(_SC_IOV_MAX)`=1024, page 4096, glibc 2.43 | The declared limit must come from `sysconf(_SC_IOV_MAX)` at admission time, not a hardcoded 1024 |
| `r6ac1-array-order.txt` | One call moves one contiguous extent into/out of k buffers **in array order**; wrapper ≡ raw syscall | ret 12288; per-buffer slices exact; write twin contiguous-in-order verified; wrapper identical | Public vectored read fills buffers in logical order from `[offset, offset+Σlen)`; write twin lands the same extent (verify by an independent `pread` of the file) |
| `r6ac1-short-read.txt` | Short count = exact prefix across iov boundaries; trailing buffers untouched; EOF-0 success; past-EOF = 0 | ret 6144: buf0 full / buf1 half / buf2 untouched; eof0=0; past_eof=0; straddle ret 8 with trailer untouched | A short count n determines per-buffer fill exactly (buffers are filled in order up to n); bytes beyond n in the last touched buffer and all later buffers are untouched |
| `r6ac1-iovmax.txt` (+btrfs) | The native `iovcnt` limit is exactly `IOV_MAX`; failure is clean and pre-transfer | 1023 OK, 1024 OK, 1025 → EINVAL, 4096 → EINVAL; write twin; state unchanged after rejection | The declared-limit rejection (`not_supported` or split — OQ-M1-3) must happen before acceptance and leave no effects |
| `r6ac1-sum-overflow.txt` | Distinguishes three Σ-regimes: in-range (works), >`MAX_RW_COUNT` (silent clamp = short success), negative-as-`ssize_t` (wild range → EFAULT) | 2 GiB req → 2147479552; `SSIZE_MAX` single → 4096; 2×2⁶² → EFAULT; 1024×1 MiB → 8192 | The library's Σ arithmetic must reject the negative regime before any call (`invalid_argument`) and must treat clamp-regime short counts as ordinary progress — never loop, never pre-truncate silently |
| `r6ac1-null-empty.txt` | `iovcnt=0` is success-0 with no effect (the candidate never reaches this path — SEM-03 no-op); NULL iov with count ≥ 1 is EFAULT | ret 0 ×4 rows incl. write size unchanged; NULL/1 → EFAULT | Zero-total → success 0 with **no OS call** (assert no syscall via the existing native-call seam); NULL-buffer-with-count is a caller precondition, not an admitted input |
| `r6ac1-iov-fault-read.txt` (+btrfs) | Read-side mid-transfer fault is a **short success**, fs-independent | ret 4096, errno 0, buf0 exact, buf2 untouched; tmpfs=btrfs; wrapper=raw | A fault after n confirmed bytes on the read side is success(n) — this is why the result is a plain count and needs no error row |
| `r6ac1-iov-fault-write.txt` (+btrfs, ×3) | **Write-side mid-transfer fault is fs-dependent**: tmpfs short-success vs btrfs EFAULT-with-zero-landed | tmpfs 4096/size 4096/prefix ok; btrfs −1/EFAULT/size 0 — stable | The contract must define EFAULT-write effects as **unknown** (ERR-02); a product test may only assert "either success-with-prefix or error-with-unknown-remainder", never one fs's answer |
| `r6ac1-fsize-partial-write.txt` (+btrfs) | Real partial write via an external limit is a silent clamp, not an error; fully-blocked write is EFBIG+SIGXFSZ | ret 100/no signal/size 100/prefix ok; then −1/EFBIG/SIGXFSZ ×1/size unchanged | Short write = progress (no retry inside the primitive); EFBIG = terminal error with confirmed bytes preserved from the earlier success |
| `r6ac1-eintr.txt` | EINTR is −1/EINTR with **zero effect** (seccomp-injected, deterministic) | ret −1/errno 4 both directions; guards intact; size 0; unfiltered control succeeds | Retry loop may run only when no count was completed; a product test injects EINTR through the existing native-call seam and asserts the buffer is untouched after the reported interruption |
| `r6ac1-cursor.txt` | Positional form never moves the shared cursor, including via `dup` | cursor 100 across preadv/pwritev; dup sees 100; read control advances | Assert cursor invariance through the public cursor operations after every vectored call |
| `r6ac1-nonseekable.txt` | Non-seekable kernel row (ESPIPE) — boundary documentation for the regular-file profile | −1/ESPIPE both directions | Out of admitted profile; no product test needed unless File kind gating changes |
| `r6ac1-atomicity*.txt` | **Negative oracle**: a single `preadv` is not a snapshot against a concurrent buffered writer | torn 0/1/2 vs 1–3 per 5000 (both forms) | No product test may assert tear-freedom; the concurrency disclosure (OQ-M1-1) cites these records |

## 2. How to reproduce

```sh
cd research/direct-beyond-parity/closure/r6-ac1/probes
cc -O2 -Wall -Wextra -D_FILE_OFFSET_BITS=64 -o r6ac1_probe r6ac1_probe.c -lpthread
zsh run_all.sh            # rewrites every file in ../raw/ (tmpfs + btrfs + reps)
```

Scratch dirs: `/tmp/r6-ac1-scratch` (tmpfs) and
`/var/tmp/r6-ac1-scratch-btrfs` (btrfs), both created on demand; contents
are per-cell fixtures that the cells recreate. Every cell is independent;
single cells can be run as `./r6ac1_probe <cell> [scratch_dir]`.

## 3. NOT_RUN_WITH_REASON

- **EINTR with a partially transferred count** — not producible on regular
  files; the kernel returns either a count (success) or −1/EINTR. The
  probe pins the −1/EINTR half and every successful cell pins the count
  half. See `ERROR_MATRIX.md` register.
- **ENOSPC/EDQUOT injection** — needs a size-capped filesystem; no mount
  privileges in this session. Mapped by table only.
- **Sluice public-path oracles** — none exist yet because AC-1 is not an
  adopted API; this directory deliberately adds **no product code and no
  product tests**. The table above is the specification a future
  product-oracle set must satisfy if the Owner adopts the capability.
- **prefaulted-vs-lazily-mapped buffer matrices, additional filesystems,
  io_uring registered-buffer interplay** — out of the ten-item R6 scope;
  not probed, no claim.
