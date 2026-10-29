# AC-3 E5 Evidence Index — R8 closure session (#607)

Session date: 2026-10-29. Host `E5`, kernel `7.2.11-200.fc44.x86_64`,
20 CPUs, uid 1000, SELinux Enforcing, gcc 16.2.1.
Filesystems under test: `fs=a` tmpfs (`/tmp/sluice600-ac3`, 32 GiB,
nr_inodes=1048576, usrquota), `fs=b` btrfs (`/var/tmp/sluice600-ac3`,
`/dev/sda3` subvol, rw,relatime,**compress=zstd:1**,ssd,discard=async,
space_cache=v2).

## Tool identity

| Tool | sha256 | Pin |
|------|--------|-----|
| `ladder-4455d98d` (P3 kit, reused read-only from `Sluice-direct-cost-ladder` worktree) | `5a53a9ad1ea00a9a26dee98543ef473ca15dcdfda86c2aa647b59cca4ec05507` | matches `p3-raw/env-pin.txt:11` |
| `m3-support` (archived gate tool, re-run) | `0f8a97f6947e7a5de88e864e2a23e2c6b526fdd73320c8a92160142479d30501` | matches `p3-raw/env-pin.txt:12` |
| `ac3-sem` built from `ac3-sem.cpp` (this dir) | session-built, g++ -O2 -std=c++20 | this session |
| `ac3-accel` built from `ac3-accel.cpp` (this dir) | session-built, g++ -O2 -std=c++20 | this session |

The plain `ladder` binary in that worktree hashes `894503…` (later rebuild,
NOT pinned) — the pinned `ladder-4455d98d` was used for every paired run.

Fixtures: `data64m.bin` 67108864 B, sha256
`a416b1529a17e09fd75602196f574be783934aaba295703ed8a558d0f7d844ce`,
identical on both fs (deterministic LCG pattern; the P3 fixture
`bc020fd1…` no longer existed on disk — this session used its own).

## Raw evidence → claim map

| Raw file | Command (summary) | Supports |
|----------|-------------------|----------|
| `raw/ac3-sem-matrix.txt` | `ac3-sem sem /tmp/sluice600-ac3 /var/tmp/sluice600-ac3` | checklist 1 (admission A1–A10), 2/4 (EOF stop + short), 3 (dst access failure), 5 (EXDEV both directions), 8 (non-regular EINVAL/EISDIR), 9 (O_APPEND EBADF/EINVAL split), 10 (overlap/adjacent/gap/self-extend), 13 (cursor untouched; NULL-offset contrast), 14 (typed EXDEV + caller-explicit fallback loop, match=1) — `[RUN]` |
| `raw/ac3-efbig-tmpfs.txt`, `raw/ac3-efbig-btrfs.txt` | `ac3-sem efbig <dir>` (RLIMIT_FSIZE=4096, SIGXFSZ ignored) | checklist 3 EFBIG leg: errno 27, confirmed 4096, `partial_effect=1` — `[RUN]` |
| `raw/ac3-conc-tmpfs.txt`, `raw/ac3-conc-btrfs.txt` | `ac3-sem conc <dir> 16777216` (64 KiB chunks, writer thread acts once dst ≥ ¼ total) | checklist 11 disclosure-4 cell: truncate→clean stop both engines; overwrite→torn mixture counts disclosed (old 4194304 + new 16392 bytes, both engines); append-beyond-window excluded from confirmed — `[RUN]`, descriptive |
| `raw/ac3-cmp-tmpfs-cp1cp2.txt`, `raw/ac3-cmp-btrfs-cp1cp2.txt` | `paired.sh ./ladder-4455d98d cp1 cp2 65536 1024 2 5 2 <fix> dst=<dir>/cmp.bin` (ABBA, taskset cpu2) | checklist 12: kernel copy vs userspace loop, archived class reproduced (tmpfs cp1≈17.6–18.5 µs vs cp2≈15.4 µs; btrfs cp1≈15.8–16.1 µs vs cp2≈6.2 µs), `content_ok=1` all arms, syscalls/chunk 2 vs 1 (kit counter) — `[RUN]`, coarse |
| `raw/ac3-gap-tmpfs-cp1scp1.txt`, `raw/ac3-gap-btrfs-cp1scp1.txt` | `paired.sh ./ladder-4455d98d cp1s cp1 65536 1024 2 5 2 …` | Sluice composition parity (cp1s medians 18347/15239 vs cp1 17618/15362 ns/chunk) — kernel transfer unreachable from today's composition — `[RUN]`, coarse |
| `raw/ac3-sweep-tmpfs.txt`, `raw/ac3-sweep-btrfs.txt` | same paired form, chunk ∈ {4 KiB, 16 KiB, 256 KiB, 1 MiB}, ops=64 MiB/chunk, pairs 2 (64 KiB anchor in the cmp files) | checklist 15: tmpfs cfr faster at all sizes; btrfs regression ~2–3.4× at 4 KiB, mixed at 16 KiB, win ≥64 KiB — `[RUN]`, coarse |
| `raw/ac3-accel-attribution.txt` | `ac3-accel … align|misalign` ×3 + `cp --reflink=always/never` timed | fs-acceleration review: btrfs aligned 17.7–24.3 ms vs misaligned 1316–1452 ms per 64 MiB (~80×); tmpfs no cliff; `cp --reflink=always` 0.00 s (metadata ceiling) vs `--reflink=never` 0.05 s (userspace) — `[RUN]`, coarse; calibration caveat N-5 |
| `raw/ac3-syscall-counts.txt` | `strace -c -f` around one cp1 / cp2 rep, tmpfs | coarse count corroboration: cp1 1025 pwrite64 + 1155 pread64; cp2 1025 copy_file_range + 130 pread64 (verify residual, P3 §6 caliber) — `[RUN]` |
| `raw/ac3-support-gate-rerun.txt` | `./m3-support /tmp/sluice600-ac3 /var/tmp/sluice600-ac3` | archived support gate reproduces on this session's dirs: `SUPPORT_GATE_PASS`, EXDEV 18, O_APPEND EBADF 9 / sendfile EINVAL 22, overlap EINVAL, NULL-offset EOF behavior — `[RUN]` |
| `raw/ac3-notrun-attempts.txt` | touch /boot{,/efi}, `unshare -Ur` mount, `pkg-config fuse3` | NOT_RUN_WITH_REASON attempt evidence for EOPNOTSUPP / ENOSPC / EROFS |

## Status labels used

- `[RUN]` — executed this session, raw output archived above.
- `[TRANSCRIBED]` — carried from archived P3 evidence and quoted with
  pointer (e.g. P-M3-1 falsification; archived cfr/sendfile cells; P3
  wall/CPU figures in the ADR context section).
- `NOT_RUN_WITH_REASON` — ENOSPC, EROFS, EOPNOTSUPP, mid-file short
  return, true-fs-limit EFBIG (see `AC3_NEGATIVE_EXPERIMENTS.md` §2).
- `[UNCERTAIN]` — btrfs 16 KiB row (mixed pairs, no claim) and all
  absolute `ac3-accel` values outside their within-probe contrasts.

## Calibration / exclusivity disclosure

Runs are coarse-grained by instruction (mission §11: effect ≫ noise, no
ns-level claims, no competition with the formal serial benchmark lane).
Load at run time: load average 0.55–0.78 on 20 CPUs; paired cells pin
cpu2 via `paired.sh` for within-session comparability with the archived
method; total paired-run wall time is seconds. These numbers must not be
promoted to formal benchmark status at adoption; formal confirmation on
the exclusive lane remains an adoption-time obligation.
