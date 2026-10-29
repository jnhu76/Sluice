# ADR Candidate — Explicit offset-addressed file-to-file copy capability (AC-3, `copy_range_at`)

Status: CANDIDATE — Owner decision required. Nothing here adopts a public
API, amends v2-r1, or merges anything. Supersedes the draft text in
`API_CAPABILITY_CANDIDATES.md` §Candidate AC-3 §5 by completing its error
table, pinning its admission order, and adding chunk/alignment policy
evidence (R8 #607, mission §11).

## Context

Sluice's legal composition for file-to-file transfer is a `read_at`+`write_at`
loop (F7). P3 (`STRUCTURAL_IO_MECHANISM_STUDY.md` §3) proved the
kernel-transfer path (`copy_file_range`, explicit offsets) is structurally
unreachable from that composition and earned a probe-level
`MECHANISM_WIN_EARNED` on tmpfs (+10.4% wall) and btrfs (+62% wall, −54.9%
CPU; reflink-attribution caveat), capped `CONDITIONAL_WORKLOAD_WIN`. Forms
F1–F4 (cfr/sendfile as positioned-read implementation or hidden-cursor
lowering) are `SEMANTICALLY_NOT_EQUIVALENT` and stay forbidden. The open
owner questions OQ-2..OQ-8 from the candidate are now evidenced per below.

## Decision requested

Add one composition-class operation (working name `copy_range_at`,
naming = Owner):

```
copy_range_at(src: File&, src_off, dst: File&, dst_off, len)
  → confirmed_bytes, stop ∈ {full, eof_before_full, error}, effect_certainty (ERR-02)
```

Engine: explicit-offset `copy_file_range` where the support matrix admits.
Never a `read_at` replacement; never returns file bytes to a user buffer;
never a silent fallback; both shared cursors untouched (SEM-04).

## Contract terms pinned by this session's evidence

1. **Admission (OQ-3).** Two-descriptor admission is evaluated as one
   operation with the observed precedence:
   `closed/access (EBADF)` → `offset range (EOVERFLOW)` → engine call.
   Sluice additionally pre-validates offsets at admission: negative offsets
   and `off+len` overflow become typed admission errors **before** the
   syscall, because the engine silently maps both to past-EOF behavior
   (`ret=0`, error-table N1/N2) that is indistinguishable from a clean EOF
   stop. The relative order of src- vs dst-side `EBADF` is unobservable on
   Linux (identical errno); the contract may pin "source first" as a text
   choice, disclosed as such, not as an engine-derived fact.
2. **Zero-length** remains a logical no-op before range checks (SEM-03
   class, unchanged from the candidate sketch).
3. **Stop set (OQ-4).** `full` (confirmed == len), `eof_before_full`
   (engine returned short-at-EOF or 0 with confirmed < len; errno
   untouched), `error` (errno row of the matrix). Short engine returns are
   legal and loop-and-advance with confirmed bytes published exactly once.
4. **Effect certainty (ERR-02).** On a failed attempt the previously
   confirmed bytes persist (session: EFBIG at exactly `confirmed ==
   dst_size`), the failing chunk may be partially applied, remainder is
   unknown; no rollback, never report 0 for unknown.
5. **Fallback (OQ-5) — caller-explicit, never silent.** `EXDEV`
   (cross-type-fs, this kernel, both directions), `EOPNOTSUPP` (per man;
   uninjectable on this host — disclosed), non-regular descriptors
   (`EINVAL`/`EISDIR`), and `O_APPEND` destinations (cfr `EBADF`, sendfile
   `EINVAL` — the errno split is mapped to one typed unsupported outcome,
   not leaked) surface as typed unsupported results. A user-space
   `pread`+`pwrite` loop remains available as a **separate explicit
   operation the caller composes**; demonstrated in
   `ac3-sem-matrix.txt` (`crossfs_explicit_fallback`: typed EXDEV with dst
   empty, then caller loop, match=1).
6. **sendfile (OQ-2) = comparator only.** It works cross-fs where cfr is
   EXDEV and reaches parity on tmpfs, but it unconditionally advances the
   out_fd cursor (session: 1024→5120; re-confirmed in the archived support
   matrix) — disqualified for a cursor-untouched contract.
7. **Cross-fs (OQ-7).** EXDEV on this kernel both directions; the boundary
   is part of the opportunity statement (typed unsupported + caller
   choice), not a defect to hide.
8. **Error table (OQ-6).** See `AC3_ERROR_MATRIX.md`. EFBIG now
   session-pinned (`[RUN]`, both fs). ENOSPC/EROFS/EOPNOTSUPP remain
   `NOT_RUN_WITH_REASON` (environment-gated; attempt evidence in
   `ac3-notrun-attempts.txt`) — the table is final only for the classes
   injected; adoption text must carry the three open rows.
9. **Concurrency (THREAD-01 / disclosure-4 — now executed).** Descriptive
   cell (`ac3-conc-tmpfs.txt`, `ac3-conc-btrfs.txt`): under concurrent
   source truncation both cfr and the user loop stopped cleanly at the new
   EOF (`confirmed = truncation point`, ret=0); under concurrent overwrite
   both produced torn mixtures (disclosed byte counts); an append beyond
   the requested window never entered confirmed bytes. **No divergence
   between cfr and the user loop was observed in any exercised
   interleaving; neither engine provides atomicity or snapshot.** Contract
   text: concurrent modification leaves transferred *contents* unspecified;
   stop-reason and confirmed-bytes accounting remain well-defined.
10. **Chunk/alignment policy (OQ-8) — new evidence, must not be omitted.**
    64 MiB / varied chunk, coarse caliber (`ac3-sweep-*.txt`):
    - tmpfs: cfr faster at every probed chunk (4 KiB → 1 MiB).
    - btrfs (compress=zstd:1): **regression ~2–3.4× at 4 KiB**, mixed/parity
      at 16 KiB, large win at 64 KiB (~2.6×), 256 KiB (~4.4–5.9×), 1 MiB
      (~6.4×).
    - Alignment cliff: misaligned (off=1) cfr on btrfs costs
      **~1300–1420 µs/chunk vs ~6–18 µs/chunk aligned (~80×)**; tmpfs shows
      no alignment effect. Misaligned calls still *succeed* silently.
    Consequence: the engine must either restrict admission (range alignment
    as an admission input where the fs accelerates remap) or document the
    cliff and leave engine/fallback choice explicit to the caller. Default
    chunk 64 KiB matches the probed evidence; **btrfs magnitudes must not
    be generalized to other filesystems** (OQ-1 caveat stands, now with a
    direct aligned/misaligned + `cp --reflink=always`-ceiling contrast:
    metadata remap 0.00 s ≪ aligned cfr ≪ userspace copy for 64 MiB).

## Scope and lifecycle

Regular files only; one contiguous range per call; two File borrows; no
user buffer, no staging allocation, no new runtime state, no global
mechanism (mechanism-justification: the operation owns no new semantic
authority — it exposes a kernel capability with admission + effect
discipline; nothing existing is replaced; no removal conditions arise).
glibc ≥ 2.27 wrapper emulates pre-4.5 kernels as a user-space copy — must
be disclosed if a supported configuration relies on it (portability note
from the candidate, unchanged). Sparse-source hole-expansion caveat (man
NOTES) unchanged.

## Validation performed on E5 (this session)

- Fifteen-item checklist: items 1,2,3(EFBIG leg),4,5,8,9,10,11,12,13,14,15
  `[RUN]`; item 3 (ENOSPC/EROFS legs) and 6 (EOPNOTSUPP)
  `NOT_RUN_WITH_REASON` with recorded attempt evidence.
- Comparator re-run with the pinned P3 kit (`ladder-4455d98d`, sha
  `5a53a9ad…` = `p3-raw/env-pin.txt:11`): direction and magnitude class of
  the archived cells reproduced (tmpfs ~+14–17% wall, btrfs ~+60%),
  content gates green, syscall class 2→1/chunk re-confirmed by coarse
  strace (`ac3-syscall-counts.txt`).
- Composition-gap re-run: Sluice `read_at`+`write_at` (cp1s) vs raw loop
  parity within coarse caliber — kernel transfer remains reachable only by
  the new capability.
- Archived support-matrix gate re-run with the pinned `m3-support` binary
  (sha `0f8a97f6…` = `env-pin.txt:12`): `SUPPORT_GATE_PASS` on this
  session's scratch dirs.

Caliber disclosure: all timing this session is coarse-grained (effect
sizes ≫ noise; medians of 2 ABBA pairs unless noted), executed outside the
formal exclusive benchmark lane; no ns-level claims are made and none are
needed for the design decisions above. Formal adoption-time benchmarking
remains a separate obligation.

## Consequences

- Owner decisions enabled: OQ-2 (sendfile comparator-only — evidenced),
  OQ-3 (precedence + offset pre-validation — evidenced), OQ-4 (stop
  shape — evidenced), OQ-5 (caller-explicit fallback — demonstrated),
  OQ-6 (error table final except 3 disclosed rows), OQ-7 (EXDEV boundary —
  re-confirmed), OQ-8 (chunk/alignment policy — new evidence, decision
  needed: admission-restrict vs document-and-leave-choice-to-caller).
- Remaining before any adoption: ENOSPC/EROFS/EOPNOTSUPP injection on an
  environment with mount privileges or richer fs types; WSL2 support-matrix
  follow-up named in the candidate; formal-lane performance confirmation
  at adoption time.
