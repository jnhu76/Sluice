# R8 — AC-3 (#607) E5 Semantic Completion — Session Summary

Date: 2026-10-29 · Host `E5` · kernel `7.2.11-200.fc44.x86_64` · uid 1000 ·
SELinux Enforcing · fs under test: tmpfs (`/tmp/sluice600-ac3`) and btrfs
(`/var/tmp/sluice600-ac3`, compress=zstd:1).

**Verdict: `DESIGN_READY_FOR_OWNER`** — every checklist item has either a
`[RUN]` result or a `NOT_RUN_WITH_REASON` with recorded attempt evidence;
no public API adopted; nothing merged; v2-r1 untouched.

Artifacts: `r8-ac3/AC3_ADR_CANDIDATE.md` (decision request + pinned terms),
`r8-ac3/AC3_ERROR_MATRIX.md` (error table), `r8-ac3/AC3_NEGATIVE_EXPERIMENTS.md`
(regressions + NOT_RUN register), `r8-ac3/AC3_E5_EVIDENCE.md` (tool hashes,
fixtures, raw→claim map, calibration disclosure), `r8-ac3/ac3-sem.cpp`,
`r8-ac3/ac3-accel.cpp` (probes), `r8-ac3/raw/*` (12 raw outputs).

## Fifteen-item coverage

| # | Item | Result |
|---|------|--------|
| 1 | two-fd admission precedence | `[RUN]` — observed partial order: closed/access (`EBADF`) → offset range (`EOVERFLOW`); src-vs-dst `EBADF` order unobservable (same errno) → text choice, disclosed. **Negative offsets surface as EOVERFLOW(75), not EINVAL** — candidate draft table corrected. |
| 2 | source EOF stop reason | `[RUN]` — ret=0 with errno untouched at/beyond EOF; short-at-EOF clean (ret=2048, content match); stop set {full, eof_before_full, error} implementable. |
| 3 | destination failure | `[RUN]` access (`EBADF`, no partial effect) + **EFBIG now injected** (RLIMIT_FSIZE; errno 27, confirmed==dst_size==4096, `partial_effect=1`, both fs — upgrades the P3 NOT_RUN row). ENOSPC/EROFS `NOT_RUN_WITH_REASON`. |
| 4 | short copy | `[RUN]` — clean short at EOF; loop-and-advance total exact (8192 in 3 chunks). Mid-file kernel short return not inducible (descriptive gap). |
| 5 | EXDEV | `[RUN]` — errno 18 both directions, dst left empty; sendfile cross-fs works (comparator contrast). |
| 6 | EOPNOTSUPP | `NOT_RUN_WITH_REASON` — no writable non-supporting fs on host (vfat/ext4 EACCES; no NFS/9p/FUSE); candidates named. |
| 7 | EFBIG / ENOSPC | EFBIG `[RUN]`; ENOSPC `NOT_RUN_WITH_REASON` (unprivileged mount denied under SELinux; no libfuse3; unsafe to fill host tmpfs). |
| 8 | unsupported file types | `[RUN]` — `/dev/null`, `/dev/zero`, pipes → `EINVAL`(22); directory fds → `EISDIR`(21). |
| 9 | destination O_APPEND | `[RUN]` — cfr `EBADF`(9) vs sendfile `EINVAL`(22); errno split must be mapped to one typed outcome; no bytes written. |
| 10 | same-file / overlap | `[RUN]` — exact & partial overlap `EINVAL`; adjacent, gap, self-extend legal with content match. |
| 11 | concurrent modification | `[RUN]` (descriptive) — **closes the P3 disclosure-4 obligation**: truncate → both engines stop cleanly at new EOF; overwrite → torn mixture disclosed (old 4194304 + new 16392 B, identical class both engines, both fs); append beyond window never in confirmed. No divergence cfr-vs-user-loop observed; neither engine atomic. |
| 12 | kernel vs userspace comparator | `[RUN]` (coarse, pinned P3 kit `ladder-4455d98d` sha-matched to `env-pin.txt:11`) — archived class reproduced: tmpfs cfr +14–17% wall, btrfs ~+60%; syscalls 2→1/chunk (kit + coarse strace); cp1s (Sluice) parity with raw loop. |
| 13 | file offsets | `[RUN]` — explicit form leaves both cursors untouched (777/888 preserved); NULL-offset consumes both (F4 boundary). |
| 14 | fallback caller-explicit | `[RUN]` — demonstrated: typed EXDEV with dst empty, then caller-chosen pread+pwrite loop, match=1; ADR pins never-silent-fallback. |
| 15 | chunk-size benefit/regression | `[RUN]` — tmpfs: cfr faster 4 KiB→1 MiB. **btrfs: regression ~2–3.4× at 4 KiB, mixed at 16 KiB, win ~2.6×/4.4–5.9×/6.4× at 64 KiB/256 KiB/1 MiB.** |

## Filesystem-acceleration review (required special item)

- **Alignment cliff (new):** misaligned (off=1) cfr on btrfs =
  1316–1452 ms per 64 MiB vs 17.7–24.3 ms aligned (**~80×**), while
  tmpfs shows no alignment effect. Misaligned calls succeed silently —
  a performance cliff hidden behind a successful return.
- **Reflink ceiling:** `cp --reflink=always` = 0.00 s (metadata remap) ≪
  aligned cfr ≪ `cp --reflink=never` 0.05 s (userspace loop) for the same
  64 MiB on btrfs. tmpfs has no acceleration layer (cfr ≈ memory-copy
  floor).
- Kernel-transfer vs user-space decomposition: btrfs cp2 sys-time
  ~8.3 µs/chunk vs cp1 ~16.4 (remap halves kernel cost); tmpfs sys-time
  ~17–21 µs both (memory copy floor); user-side traversals 2/byte → 0
  structural.
- **btrfs magnitudes are not portable** — now backed by direct evidence
  (alignment/cliff/sweep), not just the archived caveat.

## Design consequences for the Owner packet (OQ-2..OQ-8)

- OQ-3: pin `closed/access → range` order; add Sluice-level offset
  pre-validation (negative and `off+len` overflow are silently mapped to
  past-EOF `ret=0` by the engine — semantic hazard).
- OQ-4: stop set implementable as specified; zero-length no-op unchanged.
- OQ-5: typed unsupported + caller-explicit user loop; demonstrated.
- OQ-2: sendfile comparator-only (cursor advance re-confirmed).
- OQ-6: error table final for all injected classes; 3 rows stay open
  (ENOSPC, EROFS, EOPNOTSUPP) — environment-gated, listed as
  Owner/environment tasks.
- OQ-7: EXDEV boundary confirmed both directions.
- OQ-8: chunk/alignment policy is a real decision — recommend 64 KiB
  default + explicit alignment policy (admission restriction or documented
  cliff with caller-owned engine choice); never generalize btrfs numbers.

## Caliber and boundaries honored

All timing is coarse (effect ≫ noise; medians of 2 ABBA pairs; total
paired-run wall time seconds; no ns-level claims; outside the formal
exclusive benchmark lane — flagged as non-promotable in
`AC3_E5_EVIDENCE.md`). No PR created or merged; no public API adopted; no
v2-r1 edit; frozen worktrees untouched (P3 kit executed read-only from its
worktree); error injection limited to session-local scratch dirs and
process rlimits; destructive system configuration: none.
