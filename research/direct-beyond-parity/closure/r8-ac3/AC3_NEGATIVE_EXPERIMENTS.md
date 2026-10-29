# AC-3 Negative Experiments & NOT_RUN Register — R8 E5 session (#607)

Negative results are results. This file records every cell that failed a
prediction, showed a regression, or could not be executed, with the exact
raw evidence.

## 1. Negative / regression findings `[RUN]` (new this session)

### N-1 — btrfs chunk-size regression at 4 KiB (OQ-8)
`raw/ac3-sweep-btrfs.txt`, chunk=4096, ops=16384, pairs=2 (ABBA):
- pair 1: cp1 3708.6 ns/op vs cp2 **12724.2 ns/op** (cfr ~3.4× slower)
- pair 2: cp1 3328.5 ns/op vs cp2 **6917.8 ns/op** (cfr ~2.1× slower)
Prediction class "kernel transfer is never slower than the user loop"
fails on btrfs at 4 KiB. Attribution hypothesis (unverified, no fs-internal
trace): per-call remap attempt + fallback path inside btrfs; the magnitude
makes per-call engine admission/alignment policy a design input, not a
tuning detail.

### N-2 — btrfs no resolvable win at 16 KiB (OQ-8)
`raw/ac3-sweep-btrfs.txt`, chunk=16384: pair 1 cfr faster (7009.9 vs
7710.0), pair 2 cfr slower (7075.5 vs 5882.7) — mixed, no claim at this
caliber. The btrfs win boundary sits between 16 KiB and 64 KiB on this
host; it is fs- and configuration-specific (compress=zstd:1 disclosed).

### N-3 — btrfs misaligned cfr cliff (~80×)
`raw/ac3-accel-attribution.txt`: 64 MiB, 64 KiB chunks, off=1 (misalign):
- btrfs misaligned: 1316–1452 ms (1286580–1419741 ns/chunk)
- btrfs aligned (same probe): 17.7–24.3 ms
- tmpfs misaligned: 18.2–18.4 ms (no cliff)
- tmpfs aligned: 23.0–23.9 ms
Misaligned cfr **succeeds silently** while being ~80× slower than aligned
and ~80× slower than a plain pread+pwrite loop on the same fs. Silent
acceptance of unaligned ranges would be a performance cliff, not "no win".

### N-4 — archived P-M3-1 CPU prediction stands falsified (transcribed)
P3 already recorded CPU −6.9% on tmpfs vs the −30% prediction
(`STRUCTURAL_IO_MECHANISM_STUDY.md` §3.3). Not re-measured this session
(falsified predictions are recorded, not chased); carried here so the
negative register of the family is complete in the closure set.

### N-5 — calibration limit of the acceleration probe (disclosed)
`ac3-accel.cpp` absolute values are not comparable with ladder values
(64 MiB warm pass inside the probe, overwrite of existing extents, no cpu
pinning; e.g. tmpfs aligned 22.5–23.5 µs/chunk vs ladder cp2 15.4). Only
the within-probe contrasts (aligned vs misaligned, reflink ceiling) are
used as evidence. Recorded so nobody promotes these absolutes later.

## 2. NOT_RUN_WITH_REASON (environment-gated; Owner/environment follow-ups)

| Cell | Attempt evidence | Gap if adopted |
|------|------------------|----------------|
| ENOSPC mid-copy | `raw/ac3-notrun-attempts.txt`: unprivileged mount denied under SELinux Enforcing (`unshare -Ur` + `mount -t tmpfs -o size=1m` → rc 32); no libfuse3 (`pkg-config fuse3` missing); filling 32 GiB tmpfs is not a safe injection | error table cannot claim ENOSPC shape; expect EFBIG-class (partial effect + errno) but formally unverified |
| EROFS | no uid-1000-writable read-only fs; remount,ro needs CAP_SYS_ADMIN | read-only destination admission row unpinned (expected EBADF-class by access, unverified) |
| EOPNOTSUPP | `/boot/efi` (vfat) and `/boot` (ext4) not writable (EACCES recorded); no NFS/9p; every writable fs supports cfr | typed-unsupported row for non-supporting fs rests on man text + the EXDEV analog, not injection |
| mid-file kernel short return | not deterministically inducible; loop-and-advance handles it by construction | descriptive only |
| EFBIG destination on btrfs *without* rlimit (true fs limit) | same mount-privilege gap | rlimit injection stands in as the write-failure class representative |

## 3. Explicit non-claims

- All timing this session is coarse (effect ≫ noise); no ns-level claim is
  made; none of these numbers may be quoted as formal-lane results.
- btrfs magnitudes (including the new cliff and sweep numbers) are specific
  to btrfs + compress=zstd:1 on this kernel; not portable to other
  filesystems.
- The concurrency cell is descriptive (one trial per cell per fs, coarse
  interleaving control); it demonstrates behavior classes and closes the
  never-executed disclosure-4 obligation, it does not establish
  completeness over all interleavings.
