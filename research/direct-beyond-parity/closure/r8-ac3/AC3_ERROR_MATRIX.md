# AC-3 Error Matrix — explicit file-to-file copy (`copy_range_at` candidate)

E5 session R8 (#607 / mission §11). All `[RUN]` rows were executed 2026-10-29
on host `E5`, kernel `7.2.11-200.fc44.x86_64`, uid 1000, SELinux Enforcing,
against `fs=a = tmpfs (/tmp/sluice600-ac3)` and
`fs=b = btrfs (/var/tmp/sluice600-ac3, /dev/sda3, compress=zstd:1)`.
Probe: `ac3-sem.cpp` in this directory; raw outputs under `raw/`.

errno names/values are the observed session values. "no partial effect" =
destination size unchanged after the failed attempt.

## 1. Admission (two-descriptor precedence, OQ-3) — `[RUN]`

| # | Condition | cfr observed | Sendfile observed | AC-3 class | Raw |
|---|-----------|--------------|-------------------|------------|-----|
| A1 | src fd = −1, dst valid | `EBADF`(9) | n/a | admission: closed | `ac3-sem-matrix.txt` `prec_src_closed_dst_valid` |
| A2 | src valid, dst fd = −1 | `EBADF`(9) | n/a | admission: closed | `prec_src_valid_dst_closed` |
| A3 | src fd = −1 **and** src offset = −1 | `EBADF`(9) | n/a | closed is evaluated **before** offset range | `prec_src_closed_and_src_neg_off` |
| A4 | src offset = −1, both fds valid | `EOVERFLOW`(75) — **not EINVAL** | n/a | range (see note N1) | `prec_src_neg_off` |
| A5 | dst offset = −1, both fds valid | `EOVERFLOW`(75) | n/a | range (N1) | `prec_dst_neg_off` |
| A6 | dst `O_RDONLY`, valid offsets | `EBADF`(9), no partial effect | n/a | admission: access | `prec_dst_rdonly`, `dest_rdonly_first_attempt` |
| A7 | dst `O_RDONLY` **and** src offset = −1 | `EBADF`(9) | n/a | **dst access precedes src range** (observable partial order) | `prec_dst_rdonly_and_src_neg_off` |
| A8 | both fds closed | `EBADF`(9) | n/a | admission: closed (src-vs-dst order unobservable — same errno) | `prec_both_closed` |
| A9 | dst `O_WRONLY` | ret=4096 ok | n/a | dst needs write access only, not read | `prec_dst_wronly` |
| A10 | `off_in = 2^63−51`, len = 100 (signed overflow of `off_in+len`) | **ret=0, errno=0** — indistinguishable from clean EOF | n/a | semantic hazard — see N2 | `prec_off_len_overflow` |

Notes:

- **N1** The candidate draft table assumed negative offsets → `EINVAL`. On
  this kernel negative offsets surface as `EOVERFLOW`(75). The error table
  must record the observed errno, and the ADR admission layer should treat
  both as one "offset range" class.
- **N2** Offset arithmetic overflow does **not** produce an error: the call
  behaves as past-EOF and returns 0. A Sluice-level admission that does not
  pre-validate offsets cannot distinguish this from a clean EOF stop. The
  ADR proposes explicit offset validation at admission (negative → typed
  range error; `off_in+len` overflow → typed range error) rather than
  passing raw to the engine.

Observed precedence partial order (OQ-3 evidence): `closed | access (EBADF)`
→ `offset range (EOVERFLOW)`. The relative order of src-side vs dst-side
`EBADF` is **unobservable** on Linux (same errno both ways); pinning
"src first" is a contract-text choice, not an engine-derived fact — the ADR
records it as such. All A1–A10 rows identical on tmpfs and btrfs.

## 2. Stop reasons / short copy (OQ-4) — `[RUN]`

| Condition | Observed | AC-3 stop reason | Raw |
|-----------|----------|------------------|-----|
| in-range full chunk | ret=4096, content match | `full` | `eof_exact_full` |
| request crosses source EOF | ret=2048 (short), errno=0, content match on the 2048 bytes | `eof_before_full`, confirmed bytes published exactly once | `eof_partial_short` (+`content_match=1`) |
| offset == size | ret=0, errno untouched | `eof_before_full` when confirmed < len; `full` when confirmed == len | `eof_at_boundary_zero` |
| offset > size | ret=0, errno untouched, off args unadvanced | same | `eof_beyond_zero` |
| last single byte | ret=1 | `full` | `eof_last_byte_full` |
| loop-until-0, 8192 B source, 4 KiB chunks | total=8192 in 3 chunks (4096+4096+0) | loop-and-advance discipline; no replay | `eof_loop_total` |

Short returns at EOF are clean (no errno, no error). A mid-file kernel short
return (ret < len with bytes remaining) could not be induced deterministically
on this host and stays unpinned (descriptive gap, not a blocking one: the
loop-and-advance discipline handles it by construction).

## 3. Destination failure (item 3) — `[RUN]`

| Condition | Observed | Effect certainty | Raw |
|-----------|----------|------------------|-----|
| dst `O_RDONLY` (access) | `EBADF`(9) at first attempt | none — no bytes transferred, dst size 0 | `dest_rdonly_first_attempt`, `dest_rdonly dst_size_after=0` |
| `EFBIG` via `RLIMIT_FSIZE=4096` (SIGXFSZ ignored), 8 KiB source, 4 KiB chunks, both tmpfs and btrfs | attempt 1 → 4096 ok; attempt 2 → **`EFBIG`(27)**; `confirmed_before_fail=4096`, `dst_size_after=4096`, `partial_effect=1` | ERR-02 class confirmed: previously confirmed bytes persist exactly; the failing attempt may be partially applied — publish confirmed bytes, remainder unknown, no rollback | `ac3-efbig-tmpfs.txt`, `ac3-efbig-btrfs.txt` |
| `ENOSPC` | **NOT_RUN_WITH_REASON** | expected same class as EFBIG (mid-copy write failure) but not demonstrated | `ac3-notrun-attempts.txt` |
| read-only filesystem (`EROFS`) | **NOT_RUN_WITH_REASON** | — | `ac3-notrun-attempts.txt` |

## 4. Support / type classes (items 5, 6, 8, 9) — `[RUN]` unless noted

| Condition | cfr observed | sendfile observed | AC-3 class | Raw |
|-----------|--------------|-------------------|------------|-----|
| cross-fs tmpfs→btrfs and btrfs→tmpfs | **`EXDEV`(18)** both directions, **no partial effect** (dst size 0) | ret=4096 ok (works cross-fs) | typed unsupported + caller fallback boundary (OQ-7); errno split vs sendfile must be mapped, not leaked | `crossfs_cfr_*`, `crossfs_sendfile_a_to_b`; re-confirmed by archived-gate re-run `ac3-support-gate-rerun.txt` |
| dst = `/dev/null` (char dev) | `EINVAL`(22) | n/a | non-regular → typed unsupported/admission | `type_dst_devnull` |
| src = `/dev/zero` (char dev) | `EINVAL`(22) | n/a | same | `type_src_devzero` |
| pipe as src / as dst | `EINVAL`(22) both | n/a | same | `type_src_pipe`, `type_dst_pipe` |
| directory fd as src / as dst | **`EISDIR`(21)** both | n/a | same | `type_dst_dirfd`, `type_src_dirfd` |
| dst `O_APPEND` | **`EBADF`(9)**, dst size 0 | **`EINVAL`(22)** | typed unsupported; the cfr/sendfile errno split (9 vs 22) must be mapped in the contract | `oappend_dst_cfr`, `oappend_dst_sendfile`, `oappend_dst dst_size_after=0` |
| fs without cfr support (`EOPNOTSUPP`) | **NOT_RUN_WITH_REASON** — no accessible non-supporting fs (vfat `/boot/efi` and ext4 `/boot` not writable by uid 1000; no NFS/9p; no libfuse3 for a fault-injection fs) | — | open cell; candidates named for Owner/environment | `ac3-notrun-attempts.txt` |

## 5. Same-file and overlapping ranges (item 10) — `[RUN]`

| Condition | Observed | Raw |
|-----------|----------|-----|
| same fd, dst range == src range (exact overlap) | `EINVAL`(22) | `same_exact_overlap` |
| same fd, partial overlap | `EINVAL`(22) | `same_partial_overlap` |
| same fd, adjacent (dst = src+len) | ret=4096, content match | `same_adjacent` |
| same fd, non-overlapping gap | ret=4096, content match | `same_gap` |
| same fd, dst beyond EOF (self-extend) | ret=4096, file extended, dst off arg advanced to 16384 | `same_self_extend` |

All identical on tmpfs and btrfs. Contract form: overlap → admission error
(same-descriptor case), otherwise legal.

## 6. Cursor discipline (item 13) — `[RUN]`

| Form | Observed | Class |
|------|----------|-------|
| explicit offsets, src pos=777 / dst pos=888 | ret=4096; **both positions still 777/888 after**; off args advanced to 5120/5120 | conforming: SEM-04 cursor-untouched | `cursors_explicit_untouched` |
| NULL-offset form (contrast, F4) | src pos 777→4873, dst pos 888→4984 (both consumed) | forbidden as positioned-copy lowering | `cursors_null_offset_consumed` |

## 7. NOT_RUN_WITH_REASON register (Owner/environment follow-ups)

| Cell | Reason | Evidence |
|------|--------|----------|
| `ENOSPC` mid-copy injection | requires a size-capped destination filesystem; unprivileged `mount` denied (SELinux Enforcing): `unshare -Ur` + `mount -t tmpfs -o size=1m` → permission denied (rc 32); no libfuse3 for a userspace fault-injection fs; filling the 32 GiB host tmpfs is not a safe injection | `ac3-notrun-attempts.txt` |
| `EROFS` | no read-only filesystem writable by uid 1000; `mount -o remount,ro` needs CAP_SYS_ADMIN | same |
| `EOPNOTSUPP` | every fs writable by uid 1000 on this host (tmpfs, btrfs) supports cfr; vfat/ext4 candidates not writable; no NFS/9p/FUSE available | same |
| mid-file kernel short return | not deterministically inducible on this host | `ac3-sem-matrix.txt` note |

These remain error-table gaps for the final adopted contract; they are
environment-gated (mount privileges / additional fs types), not
design-gated. The EFBIG row upgrades the P3-session "EFBIG/ENOSPC session-
unverified" register: EFBIG is now session-pinned, and as the observed
representative of the mid-copy write-failure class it constrains the
expected ENOSPC shape (partial effect + errno at the failing attempt).
