# R6/AC-1 Owner Decision Packet — positional vectored read/write (#605)

Everything below is decision support. **Nothing is adopted** by this PR:
no public API, no product code, no v2-r1 text change (mission §1; #605
"Awaiting Owner"). Each decision lists the options, the measured facts,
and a recommendation with its grounds. The Owner may take or reject any
recommendation.

## Decision 1 (OQ-M1-1) — atomicity-disclosure stance

**Question.** What may the AC-1 contract say about the atomicity of one
vectored call?

**Measured facts** `[RUN]` (`SEMANTICS.md` §3, `raw/r6ac1-atomicity*.txt`):

- A single `preadv` over 1 MiB (8×128 KiB iovs) against a thread that
  continuously rewrites the region produced torn observations (pages from
  two different write epochs inside one call's result) in **0, 1 and 2 of
  5000 iterations** across three runs. Eight separate `pread`s tore 1–3
  times in the same runs. The two forms are the same order of magnitude;
  neither is a snapshot.
- The man-page "guaranteed to read a contiguous block" sentence
  `[TRANSCRIBED]` therefore cannot be read as a cross-page atomicity
  guarantee on this host/kernel.

**Options.**

| Option | Content | Assessment |
|---|---|---|
| A. Inherit SEM-04 verbatim | "Overlapping file writes and concurrent resize have no additional atomicity/snapshot guarantee" — no vectored-specific promise | Consistent with the measurement; smallest text |
| B. A + implementation-fact disclosure | additionally disclose that the vectored form issues one OS call (fewer observation points) with no semantic weight | Equally consistent; adds documentation only |
| C. Promise un-torn whole-extent reads | contract-level atomicity claim | **Contradicted by measurement** (torn observations exist); would violate the red line against promising atomicity beyond Linux's actual guarantee |

**Recommendation.** Option A or B (Owner's taste); C is excluded by
evidence. Whichever is taken, no Sluice test may assert tear-freedom, and
the hazard record stays citable.

## Decision 2 (OQ-M1-2) — iov-fault error-table rows

**Question.** What does the error table say for a faulting iov entry?

**Measured facts** `[RUN]` (`ERROR_MATRIX.md` E9–E11; stable across
repeats and across tmpfs/btrfs):

- Read side: a fault **after** bytes transferred is a **short success**
  (ret 4096, errno 0) — identical on tmpfs and btrfs, wrapper identical
  to raw syscall. A fault with nothing transferable is −1/EFAULT with
  zero effect.
- Write side: **filesystem-dependent.** tmpfs returns short success with
  the prefix landed; btrfs returns −1/EFAULT with **nothing** landed.
  Both stable ×3.

**Options.**

| Option | Content | Assessment |
|---|---|---|
| A. Read-fault = success(count); EFAULT-write = error with **unknown** remainder effects (ERR-02), plus an explicit fs-dependence disclosure | exactly what is measured; no fs is lied about | Recommended |
| B. Normalize EFAULT-write to "nothing written" | simpler text | **Contradicted** by the tmpfs arm (prefix landed); a caller relying on it on tmpfs-class filesystems would be wrong |
| C. Document short-success-on-fault for writes | matches tmpfs | **Contradicted** by the btrfs arm |

**Recommendation.** A. The mapping itself needs no new error code:
EFAULT → `backend_error` (`os_errno` preserved) under the existing
`canonical_error_code` table (`include/sluice/error.hpp:80-88`).

## Decision 3 (OQ-M1-3) — k > `IOV_MAX`: explicit reject vs explicit split

**Question.** A request with more than 1024 buffers: reject with
`not_supported` before acceptance (SEM-04 stricter-limit row), or split
into ⌈k/1024⌉ native `preadv`/`pwritev` calls behind one public result?

**Cost facts** (grounded in archived measurements; derivations shown):

- Value of one saved syscall entry at 4 KiB warm tmpfs, derived from the
  archived P3 M1 deltas (`STRUCTURAL_IO_MECHANISM_STUDY.md` §1.3
  `[TRANSCRIBED]`): k=2 → 511.3/1 = 511.3 ns; k=4 → 1576.0/3 = 525.3 ns;
  k=8 → 3613.6/7 = 516.2 ns. **≈510–525 ns per entry, warm**; cold
  regimes are worth relatively more per entry (M4: R_cold ≈ 0.46 KiB/entry
  vs R_warm ≈ 4.3–4.9 KiB/entry `[TRANSCRIBED]`).
- Split overhead for a k-buffer request: exactly ⌈k/1024⌉ − 1 extra
  kernel entries ≈ (⌈k/1024⌉ − 1) × ~520 ns warm, plus one userspace
  loop iteration per chunk (sub-µs). Example: k = 2048 × 4 KiB → 2
  syscalls instead of 2048; saves ≈ 2046 × 520 ns ≈ **1.06 ms** warm.
  k = 1025 → 2 syscalls instead of 1025; saves ≈ 0.53 ms.
- Reject's cost is what the caller does next: today's only legal fallback
  is k × `read_at` (k syscalls), which measured **wall parity with the
  raw loop** — i.e. for k > 1024 shapes, reject forfeits essentially the
  whole mechanism win (`[TRANSCRIBED]` P3 M1 `avs−av0`: +5.7 ns read /
  −15.4 ns write, sub-MDE `NO_CLAIM`).
- The split win is real only for shapes nobody has admitted: P3 probed
  k ≤ 8; #578 R0 is incomplete and no real workload evidence names a
  k > 1024 shape (`CONDITIONAL_WORKLOAD_WIN` cap).

**Semantic commitments compared.**

| Aspect | Explicit reject | Explicit split |
|---|---|---|
| Contract row | `not_supported` before acceptance — the SEM-04-native answer, one sentence | Declared per-chunk bound + composition semantics inside a primitive signature: EINTR between chunks (retry only without completed count), EOF mid-chunk → short total, error mid-chunk → confirmed = completed chunks + current count, unknown remainder (E11's fs-dependence applies at every chunk boundary) |
| New runtime mechanism | one comparison against a sysconf value | a chunk loop + confirmed-byte accumulation + composition-class stop discipline |
| Observable difference | none beyond the rejection | syscall count ⌈k/1024⌉ instead of k; all other observables preservable (prefix fill is compositional) |
| Perf for k > 1024 | none (caller falls back to k calls) | ≈full win (≈(k − ⌈k/1024⌉) entries × ~520 ns warm) |
| Perf for k ≤ 1024 | identical (both single-call) | identical |
| Test surface | one rejection cell (`r6ac1-iovmax.txt` class) | new split-boundary oracles needed first: EOF at a chunk boundary, error at a chunk boundary, EINTR between chunks — none exist today |
| Contract vocabulary | stays primitive-class | primitive-shaped API with composition-class execution — must be disclosed as such to avoid blurring SEM-05 |

**Options and recommendation.**

- **Recommendation (not auto-adopted): v1 contract = explicit reject**
  (`not_supported`, limit declared from `sysconf(_SC_IOV_MAX)`), with the
  split form recorded as the designated upgrade path that becomes
  eligible only when (a) an admitted workload shows k > 1024 shapes
  (#578-class evidence), and (b) the three split-boundary oracle cells
  are defined and green. Grounds: the repository's smallest-mechanism
  discipline (AGENTS.md), zero admitted demand today, and reject being
  the SEM-04-native row with no new semantic machinery.
- The Owner may equally choose split-in-v1: nothing measured blocks it;
  the price is the composition-disclosure text and the three new oracle
  cells, not any semantic impossibility.
- **Not on the table:** silently accepting k > 1024 and issuing multiple
  calls *without* declaring it (hidden composition), or a silent
  k × `read_at` lowering (§4.2 of `API_DRAFT.md` — observable divergence,
  forbidden as a hidden path).

## Decision 4 — naming and limit surface

- Working names `read_at_vectored` / `write_at_vectored` are placeholders;
  Owner picks final names.
- Limit source: recommend `sysconf(_SC_IOV_MAX)` at admission time
  (measured 1024 on E5, `r6ac1-env.txt`) over a hardcoded constant — the
  limit is a platform fact the contract declares, not a Sluice-owned
  constant.

## Decision 5 — adoption sequencing

Performance opportunity is probe-level and capped
`CONDITIONAL_WORKLOAD_WIN` (`STRUCTURAL_IO_MECHANISM_STUDY.md` §1.4,
#578 R0 open). Recommended order if adopted: resolve Decisions 1–4 →
ADR referencing this packet + the P3 evidence → product slice whose test
set asserts the `E5_ORACLE.md` product obligations through the public
call → ledger update. The win being unreachable without the capability
(P3: today's legal composition is wall-parity) is the adoption
argument; the absence of an admitted workload is the argument for
waiting. Both facts are the Owner's to weigh.

## Red-line compliance statement

This packet makes no atomicity promise beyond Linux's actual guarantee
(Decision 1 Option C is explicitly excluded by measurement); the
split-vs-reject comparison is quantified with a recommendation whose
adoption is left to the Owner; no public API is created by this PR; no
v2-r1 text is modified; all measured claims carry `[RUN]` evidence files,
all transcribed claims cite their archived sources.
