# Direct Cost Floor — Per-Operation Cost Model (P1 Synthesis)

This document is the P1 synthesis of the per-operation cost model for the
Direct execution form at baseline **B1** (Direct after C1, commit
`77e5399a45998dea396699fa2e063c94a665945a`, frozen archive sha256
`d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0`, re-verified
read-only by every P1 auditor). It merges the six P1 static audits
(`p1-audit/*.md`) with the executed P1 measurement campaign
(`P1_MEASUREMENT_PLAN.md` + `p1-raw/*`). Companion documents:
`DIRECT_HOTPATH_LEDGER.md` (per-hot-path machine work) and
`CANDIDATE_GAP_REGISTER.md` (gap register).

## 0. Evidence legend (P0 convention)

| Tag | Meaning |
|---|---|
| `[RUN]` | Executed during the P1 mission (audit disassembly sessions or the measurement campaign); record archived under `p1-audit/` or `p1-raw/`. |
| `[TRANSCRIBED]` | Number or claim transcribed from the #600 frozen evidence or the P0 record (`P0_BASELINE_AND_OWNERSHIP.md`); not re-measured. |
| `[UNCERTAIN]` | Could not be fully verified; flagged rather than filled with a guess. |
| (no tag) | Static citation of repository code or of a P1 record named inline. |

Machine-count convention (audits, applies to every static count in this
directory): executed instructions on one legal, nonempty, success-path call —
including branches, the syscall `call`, and `ret` — from `objdump -d`/`-dr` of
the frozen archive objects, with comparator arms compiled in-session using the
P0-recorded flags (`g++ -m64 -fvisibility=hidden -Wall -Werror -O3 -std=c++20
-DNDEBUG`, `CCACHE_DISABLE=1`, GCC 16.2.1 20260819). The #600 ledger used a
different counting context (e.g. its recorded N1 ≈42 for `read_at` vs this
mission's standalone recount of the identical source at 40); parity
conclusions are insensitive to ±2 instructions `[UNCERTAIN: #600's exact
counting convention is not recoverable from the ledger text]`.

Ladder identity `[RUN]`: comparator kit committed at `e096b4a1e3b7fb1cbfedfcd5f10e7e6a88050637`
on local-only branch `direct-cost-ladder` (worktree
`/home/jnhu/Source/Sluice-direct-cost-ladder`, one commit over B1 touching
only `research/direct-cost-ladder/`); ladder binary sha256
`0685c31f7585e437910ed671a7025a172de4303e756d5b6e31b2480e1c45812a`; committed
`ladder.cpp` sha256 `f43db18cc42ec93e404e6a2d578264299e278a56c15ea3f3ffd27857b53f6005`
(`p1-raw/ladder-identity.txt`). This closes the "uncommitted probe" pattern
flagged at P0 §4.8 for all *new* arms (write, metadata); the branch remains
local-only by ownership rule and must be pushed/merged by the owner before any
external claim depends on it.

## 1. Session noise floors and MDEs (interpretive key)

All wall verdicts below use the preregistered §5.5 rule with the session's own
A/A floors `[RUN]` (canonical reruns; see `p1-raw/aa-*.txt`):

| Track | sd_AA (ns) | MDE = 2.131·sd_AA/√16 (ns) | #600 reference band |
|---|---|---|---|
| read (warm) | 10.390 | **5.535** | sd_AA≈5.0 / MDE≈2.5–3.0 |
| write (warm) | 29.461 | **15.695** | idem |
| metadata (warm) | 19.176 | **10.216** | idem |
| read (cold, btrfs) | 2658.254 | **1416.185** | none |

Verdict rule: **EXCESS** iff the 95% t-CI of the paired mean excludes 0 AND
|median| ≥ MDE_cell; **PARITY_REACHED** iff |median| < max(MDE_cell, 1% of the
N1 arm median); otherwise **NO_CLAIM** (middle band). Cells completed before
their track's A/A unit existed are classified here retroactively with the
session A/A, as the plan prescribes one session-wide floor per track; each
such cell's own disclosure is preserved in `p1-raw/`. The four canonical A/A
rerun files originally carried raw samples only; the synthesis pass
recomputed the pairing statistics from those samples (consecutive-pair
first−second deltas, sample stdev df=15, MDE=2.131·sd/√16 — all four
reproduce the values in this table exactly) and appended post-hoc analysis
tails to `p1-raw/aa-{read-4k,write-4k,meta-file-info,cold-read-4k}.txt`.
All A/A floors exceed
the #600 ≈5 ns reference band (10.4–29.5 ns, each rerun once per the
preregistered §11.10 rule and still high → disclosed as persistent session
noise, not absorbed by extra reps) — this session's MDEs are therefore wider
than #600's, and sub-MDE point estimates must not be read as zero.

## 2. Per-op path chains (public API → syscall)

Citations are `file:line` at B1; machine-level addresses in the audit docs.

- **read_at** — `include/sluice/blocking/file.hpp:14` → admission
  `src/blocking_file.cpp:78-81` → `admit_data_op` `:25-29` →
  `precheck_data_op` `include/sluice/detail/file_semantics.hpp:157-176`
  (closed :162-163 → access :164-165 via `access_allows` :54-68 → zero-length
  :166-167 → range :171-172 via `range_is_valid` :32-39 → transfer :173-174) →
  verdict dispatch `blocking_file.cpp:82-87` → `retry_on_eintr`
  `include/sluice/detail/posix_retry.hpp:7-15` wrapping `native_pread`
  `blocking_file.cpp:51-58` → `::pread` :57 → Result `:96` /
  `from_errno_value` `include/sluice/error.hpp:66-93`.
- **read** (shared cursor) — `file.hpp:19` → `blocking_file.cpp:120-121`
  (offset ≡ 0; range+transfer fold into one `jle` reusing the zero-length
  `test`) → dispatch :122-127 → `retry_on_eintr` + `native_read` :43-49 →
  `::read` :48 → Result :135. Cursor semantics carried by the native
  open-file-description cursor (SEM-04).
- **write_at** — `file.hpp:16-17` → `blocking_file.cpp:99-118` → same
  `precheck_data_op` chain → dispatch :103-108 → `retry_on_eintr` +
  `native_pwrite` :68-75 → `::pwrite` → Result :114-117.
- **write** (shared cursor) — `file.hpp:21` → `blocking_file.cpp:138-154`
  (offset ≡ 0; whole range/transfer admission costs zero extra arithmetic
  instructions at -O3) → dispatch :140-145 → `native_write` :60-66 →
  `::write` → Result :150-153.
- **read_exact_at** — `file.hpp:51-52` → outer admission
  `blocking_file.cpp:288-296` → `compose<Primitive>` loop :269-283 carrying
  `CompositionState` (`file_semantics.hpp:232-272`) with the **public
  `read_at` re-entered per step** (:297-299) → `::pread` →
  `Result<CompositionOutcome>` via `direct_outcome`/`composition_effect_certainty`
  (`blocking_file.cpp:241-265`, `file_semantics.hpp:338-351`).
- **read_exact** (shared cursor) — `file.hpp:57` → `blocking_file.cpp:318-326`
  → compose :269-283 with public `read` per step (:327-328) → `::read` →
  Result via `direct_outcome`.
- **write_all_at** — `file.hpp:54-55` → `blocking_file.cpp:302-312` → compose
  :313-315→:269-283 with public `write_at` per step (:313-315) →
  `retry_on_eintr` + `native_pwrite` → `::pwrite` → Result via
  `direct_outcome`/`publish_certainty` (:241-265, :236-239).
- **write_all** (shared cursor) — `file.hpp:59` → `blocking_file.cpp:331-339`
  → compose :340-342→:269-283 with public `write` per step (:340-341) →
  `::write` → Result via `direct_outcome`.
- **File::open** — `include/sluice/blocking/file_resource.hpp:75` →
  `src/file_resource.cpp:68-80`: embedded-NUL scan + read_only/truncate
  rejection (`file_semantics.hpp:82-101`) → `open_flags_for` 9-mode constant
  flags (`file_resource.cpp:31-58`) → single `::open(path, O_CLOEXEC|…, 0644)`
  :75 (no EINTR retry, SEM-02) → 24-B sret `Result<File>`.
- **File::close** — `file_resource.hpp:82` → `file_resource.cpp:104-115`:
  closed/moved-from → no-op success (:105-107); ownership consumed
  (`std::exchange(fd_,-1)`) **before** the single terminal
  `close_native(fd)` (:110-113, EINTR → `interrupted`, MUST NOT retry) →
  12-B `Result<void>`.
- **file_info** — `blocking/file.hpp:27` → `blocking_file.cpp:171-191`:
  closed check (access check folds to constant-true, SEM-03 table) → 144-B
  zero-initialized `struct ::stat` (:179) → `retry_on_eintr(::fstat)` :180 →
  S_ISREG/st_size/st_dev/st_ino projection :185-190 → 56-B sret.
- **size** — `blocking/file.hpp:29` → `blocking_file.cpp:193-199`: `file_info`
  fully inlined; only `st_size` consumed (kind/dev/ino dead-code eliminated).
- **resize** — `blocking/file.hpp:31` → `blocking_file.cpp:201-214` →
  `precheck_resize` (`file_semantics.hpp:187-197`: closed → read_only →
  representability sign test :41-43) → `retry_on_eintr(::ftruncate)` :208-209
  → `Result<void>`.
- **sync_data** — `blocking/file.hpp:23` → `blocking_file.cpp:156-169` →
  `precheck_state_op` (`file_semantics.hpp:178-185`; access arm folds to
  constant true) → `retry_on_eintr(::fdatasync)` :164 → `Result<void>`.
- **sync_all** — `blocking/file.hpp:25` → `blocking_file.cpp:216-229` → same
  shape with `::fsync` :224 (metadata increment carried entirely by the
  syscall choice; zero extra userspace instructions vs sync_data).

## 3. Eight-class classification rollup

Rollup of every classification made by the six audits. Per-instruction
evidence and semantic basis live in the audit docs named in the last column.

1. **SEMANTICALLY_REQUIRED** — the dominant class. Admission precedence
   (closed → access → zero-length no-op → range/transfer), EINTR
   retry-without-completed-count, partial-count success, progress-merge stop
   set, errno canonicalization, NUL scan, read_only+truncate rejection,
   ownership-consume-before-close. All oracle-pinned (audit docs cite the
   oracles per item).
2. **KERNEL_FLOOR** — exactly one native call per op on every path that
   transfers or mutates: 1 × pread64/read/pwrite64/write per data op, 1 ×
   open, 1 × close, 1 × fstat, 1 × ftruncate, 1 × fdatasync/fsync; **zero**
   syscalls on zero-length and on userspace rejection (strace-verified
   `[RUN]`, `p1-raw/*.syscalls.txt`, `read-sweep-*` strace arms).
3. **ABI_OR_COMPILER_COST** (carries #600 D3 `NO_CHANGE_JUSTIFIED` — not a
   gap, not re-registered): spill/restore shape across the syscall
   (`posix_retry.hpp:7-15`), 24/33/56-B sret Result scaffolding, the dead
   `os_errno` zero-store on success, `Result<void>` bool stack round-trip
   (3 instructions where 1 would do — byte-identical in the N1 controls),
   +1/+2 cold-block layout `jmp`s (sync_data/sync_all/resize). Every one of
   these is emitted identically by the matched-native comparator arms
   compiled with the same flags.
4. **DUPLICATE_WORK** — per-step re-admission inside the compose loops, the
   only confirmed duplicate class at B1: read_exact_at ≈13 instr/step
   (fd reload + closed + access + or/js transfer re-test + range recompute),
   read_exact ≈9, write_all_at 12 (of 44), write_all 7 (of 34), absent from
   matched-native step loops. No v2-r1 obligation requires it (SEM-03 is
   established once per public call; THREAD-01 serializes file state; the
   invariant is self-documented at `blocking_file.cpp:267-268`).
5. **REPRESENTATION_OVERHEAD** — dead `CompositionState` round-trips: 8
   instructions / 7 memory ops per step in read_exact_at and read_exact
   (incl. a 16-B `movdqa`+`movaps` pair storing to a stack slot no reader
   ever touches, certified dead by full-function reader enumeration), ~10
   per iteration in the write compose loops (incl. the same 16-B dead store).
   Pre-C1 the class was dominant; at B1 it survives only inside composition.
6. **TRANSFORMABLE_WORK** — contract-required output produced in a removable
   form: the double exit materialization of `Result<CompositionOutcome>`
   (~12 memory ops + 2 dead zero-stores vs the matched native's 5 direct
   sret stores); the null-data guards (read side 3 instr + 1 branch at
   `read_at`/`read`; write side 2 instr + 1 branch at
   `write_at`/`write` — guarding a `std::span` preconditions-violating
   input class that is unconstructible without UB); `File::open`'s
   6-constant flag-table jump chain (+13 instr vs an arithmetic comparator
   computing the same 9 flag words); the 144-B `stat` zero-initialization
   in file_info/size (10 instr, also carried by the matched arm).
7. **COMPOSITION_OVERHEAD** — one-step composition deltas vs matched-native
   composition controls: read_exact_at +37 static instr (100 vs 63; +25 per
   additional short step), read_exact +31 (82 vs 51; +19), write_all_at +29
   (101 vs A1 arm; +17 per additional step), write_all +22 (+11). Outer
   admissions are at parity (29v29, 21v21), so the class is exactly the
   loop machinery of classes 4–6.
8. **UNATTRIBUTED** — the impossible-count guards (2 instr/step in each
   compose loop; no v2-r1 requirement names them, but behavior is
   oracle-pinned by `impossible_count_stops_immediately`
   `tests/direct_composition_fault_test.cpp:340`; keep/remove is an owner
   decision under the mechanism-justification gate, not free-running
   excess) and the layout `jmp`s already listed under class 3.

## 4. Cost ladders (N0 / N1 / B1)

N0 = bare-POSIX floor arm (A0/W0/M0), N1 = matched-native arm
(A1/A1x/W1/W1x/M1), B1 = Sluice arm (A2/AX/W2/WX/M2). All wall numbers are
`[RUN]`: uninstrumented, best-of-5 reps, 16 ABBA pairs, `taskset -c 2`,
warm tmpfs fixtures unless marked cold. Δ = paired median in the
**B1−N1** convention (positive = Sluice slower); the 95% CIs on the paired
mean are stated in the same convention (sign-flipped where a raw cell record
used the N1−B1 convention — the 16 archived raw deltas make any convention
recomputable). N0/A3 floors are
single-invocation context arms (descriptive only). PMU deltas
(`perf instructions:u`, whole-process, cross-arm difference is the signal)
and strace syscall counts come from the per-cell diagnostic runs.

### 4.1 Read ladder

| Cell | size | N1 med (ns/op) | B1 med | Δ(B1−N1) med | 95% CI (mean) | Verdict (§5.5) | PMU Δ instr/op |
|---|---|---|---|---|---|---|---|
| read-sweep-0b | 0 B | 4.676 | 7.878 | +3.338 | [+2.584, +3.663] | PARITY (BELOW_NOISE: < MDE 5.535) | +34.2 |
| read-sweep-1b | 1 B | 558.214 | 553.850 | −0.940 | [−12.928, +2.577] | PARITY | +38.2 |
| read-sweep-64b | 64 B | 798.360 | 672.590 | −36.226 | [−236.357, +107.312] | NO_CLAIM (cell contaminated by host transient; sd 322 ns) | +38.2 |
| read-sweep-512b | 512 B | 594.244 | 593.536 | +3.670 | [−5.143, +13.753] | PARITY (< max(5.535, 5.942)) | +38.2 |
| read-sweep-4k | 4 KiB | 704.666 | 711.949 | +6.028 | [−4.135, +12.227] | PARITY (< max(5.535, 7.047)) | +39.7 |
| read-sweep-64k | 64 KiB | 8696.847 | 8455.666 | +100.345 | [−406.124, +1252.871] | NO_CLAIM (> max(5.535, 86.968); CI spans 0; bandwidth noise) | +39.6 |
| read-sweep-1m | 1 MiB | 158341.213 | 158572.391 | +107.833 | [−3183.048, +4561.850] | PARITY (< 1583.4) | +26.6 |
| read-random-4k | 4 KiB LCG | 776.361 | 761.668 | −1.460 | [−20.889, +10.715] | PARITY | +38.0 |
| read-cold-4k | 4 KiB cold | 8672.99 | 8650.21 | −10.876 | [−729.834, +2396.975] | PARITY (< 86.73; MDE_cold 1416) | +38.1 |
| read-cold-64k | 64 KiB cold | 150413.772 | 149193.915 | −2409.683 | [−48526.823, +14301.220] | NO_CLAIM (> max(1416.185, 1504.14); CI spans 0) | +38.7 |

Context floors (single invocation): A0 raw `::pread` 543.161 (0B) … 328591
(1MiB); A3 no-syscall stub 3.1–6.1 ns/op. Sources: `p1-raw/read-sweep-*.txt`
(+ `-pmu.txt`), `read-random-4k*.txt`, `read-cold-*.txt`.

### 4.2 Read composition ladder

| Cell | series | N1 med | B1 med | Δ(B1−N1) | 95% CI | Verdict | PMU Δ instr/op |
|---|---|---|---|---|---|---|---|
| read-composition-4k | A1x vs AX (sequential 4 KiB) | 702.473 | 721.206 | **+18.992** | [+7.788, +24.326] | **EXCESS** (≥ MDE 5.535, CI excludes 0; 13/16 pairs slower) | +118.2 |
| read-eof-mix-4k | A1x vs AX (full/partial/at-EOF rotation) | 710.707 | 722.508 | **+16.129** | [+6.234, +25.061] | **EXCESS** | +123.4 |
| read-eof-mix-4k | A1 vs A2 (single `read_at`, same rotation) | 538.884 | 544.220 | +9.037 | [−3.290, +17.879] | NO_CLAIM (> max(5.535, 5.389); CI spans 0) | +38.0 |

Attribution cross-check `[RUN]` (`p1-raw/read-composition-4k.txt`): the
composition-specific instruction excess (AX−A1x = +118.2/op) is ≈3× the
single-op constant (+38.8/op), and `ax−a2 = +125.4` (Sluice composition
machinery) vs `a1x−a1 = +46.1` (native composition machinery). Syscall
parity: 2.000 pread64/op timed+verify (1.3333/op at the EOF mix, identical
across tiers) — the excess is pure userspace.

### 4.3 Write ladder (NEW — closes P0 §4.13)

| Cell | size | W1 med (ns/op) | W2 med | Δ(B1−N1) med | 95% CI (mean) | Verdict | PMU Δ instr/op |
|---|---|---|---|---|---|---|---|
| write-sweep-0b | 0 B | 2.588 | 4.589 | +2.002 | [+1.958, +2.130] | PARITY (BELOW_NOISE: < MDE 15.695; CI excludes 0 but sub-MDE; 16/16 same sign) | +18.0 |
| write-sweep-1b | 1 B | 587.044 | 588.282 | +2.096 | [−71.029, +94.575] | PARITY by rule; contention-disclosed (pair sd 155 ns; ±2 ns point estimate unresolvable this session) | +19.3 |
| write-sweep-64b | 64 B | 597.073 | 600.356 | +2.281 | [−2.480, +4.207] | PARITY | +19.1 |
| write-sweep-512b | 512 B | 614.401 | 616.564 | +1.764 | [−2.019, +13.382] | PARITY | +19.0 |
| write-sweep-4k | 4 KiB | 764.864 | 767.835 | +0.025 | [−5.572, +12.159] | PARITY | +19.1 |
| write-sweep-64k | 64 KiB | 8756.448 | 8642.444 | −33.775 | [−427.256, +55.685] | PARITY (< max(15.695, 87.56)) | +19.1 |
| write-sweep-1m | 1 MiB | 161123.160 | 159425.535 | −26.606 | [−2099.1, +896.6] | PARITY (< 1611) | +19.0 |
| write-random-4k | 4 KiB LCG | 794.251 | 789.004 | −2.876 | [−17.265, +14.920] | PARITY | +19.0 |
| write-cold-4k | 4 KiB cold | 2959.302 | 3480.796 | −55.430 | [−4411.858, +1654.383] | NO_CLAIM (> 1% = 29.59; no cold-write A/A; CI spans 0) | +19.0 |
| write-composition-4k | W1x vs WX | 781.064 | 790.027 | +11.392 | [−13.205, +26.325] | PARITY at wall (< MDE 15.695) — instruction-caliber gap open (PMU +58.1) | +58.1 |

W0 floors: 466.422 (0B) … 159262 (1MiB). Syscalls/op: exactly 1.000
pwrite64/op at every size ≥1 B and 0.000 at 0 B, identical across W0/W1/W2
`[RUN]` (strace, per-cell `*-pmu.txt`/diagnostics). The write wrapper
instruction delta is a size-independent constant (+18.0…+19.3 across all
seven sizes); at 4 KiB and above it is buried under the ~760 ns–160 µs
syscall floor. Scratch discipline (per cell headers, not uniform):
**Amendment-1 dedicated scratches** for write-sweep-1b/64b/512b/4k/64k/1m
and write-composition-4k (per-cell files under `/tmp/sluice600/`, deleted
after use); **write-sweep-0b** used the shared fixture with no content
interaction (0-B writes touch nothing); **write-random-4k ran before
Amendment 1** (~04:09 vs ~04:38) on the shared
`/tmp/sluice600/write64m.bin` — its attempt-1 collision with a concurrent
sweep cell is documented in `p1-raw/write-random-4k-attempt1-failed.txt`
and its isolation rests on after-the-fact serialization (lsof check before
the single retry), not on a dedicated scratch. Sources:
`p1-raw/write-sweep-*.txt`, `write-random-4k*.txt`, `write-cold-4k*.txt`,
`write-composition-4k*.txt`.

### 4.4 Metadata ladder (NEW — closes P0 §4.13)

| Cell | op pair | M1 med (ns/op) | M2 med | Δ(B1−N1) med | 95% CI | Verdict | PMU Δ instr/op |
|---|---|---|---|---|---|---|---|
| meta-open-close | open+close | 1596.861 | 1603.927 | +11.487 | [−15.501, +32.513] | PARITY (< max(10.216, 15.969)) | +74.0 |
| meta-file-info | file_info | 1013.365 | 1041.133 | +30.312 | [−12.046, +49.833] | NO_CLAIM (> max(10.216, 10.134); CI spans 0; 12/16 pairs Sluice slower) | −2.0 |
| meta-size | size | 517.871 | 510.085 | −6.609 | [−21.921, +1.872] | PARITY (< 10.216; point estimate Sluice faster) | −18.0 |

Syscalls/op identical across tiers: 1×openat+1×close per open-close op;
exactly 1×fstat per file_info/size op; 0 syscalls on closed-file rejection
paths `[RUN]` (`p1-raw/meta-*.syscalls.txt`). The file_info wall middle band
coexists with instruction parity (−2.0/op) — the +30 ns point estimate is not
attributable to userspace instructions and remains unexplained noise-band
structure `[UNCERTAIN]`. M0 floors: open+close 1674.044; raw fstat 1004.730 /
529.063 across two cells (single-sample swing ~2×, disclosed).

### 4.5 Diagnostic cells (no CI claims, §5.8)

- Userspace rejection (closed file): read +2.020, write +1.721, file_info
  −0.627 ns/op vs matched mirrors; all two orders of magnitude below the
  kernel EBADF floors (409/436/479 ns/op); zero syscalls on both tiers.
- EINTR (3 µs itimer, non-stationary): in the B1−N1 convention used
  throughout §4, read median **+79.563** ns (pair range −603.6…+708.975),
  write **+20.647** ns (range −1593.3…+1639.3) — under the itimer storm the
  Sluice arm is the slower side in both cells; descriptive only, no claim
  caliber. The userspace wrapper instruction delta persists under the itimer
  (+38.7 read / +19.5 write), i.e. the retry path adds no measurable wrapper
  cost. strace was non-viable under the itimer (terminated >15 min;
  recorded `NOT_MEASURED_WITH_REASON` in
  `p1-raw/read-eintr-4k.syscalls.txt`).

## 5. Caliber nails (P0 leftovers, now fixed and executed)

1. **Statistics rule (P0 §4.11 — "15-pair rule not formalized")** — pinned in
   `P1_MEASUREMENT_PLAN.md` §5 and executed: 16 ABBA pairs/cell (≥15),
   arm order alternating, `taskset -c 2`; point estimate = median of paired
   deltas; CI = 95% t on the paired mean, t(0.975,15)=2.131·sd/√16; sign
   count as nonparametric corroboration; raw 16 deltas archived so any
   convention is recomputable; MDE_cell = 2.131·sd_AA/√16 with per-track
   session A/A units; EXCESS/PARITY/BELOW_NOISE decision rule as in §1 here;
   one rerun per cell on failure (§5.9) and one post-quiesce rerun per A/A
   unit above 10 ns (§11.10). Session A/A outcomes: all four tracks ran
   above both the 10 ns band and the #600 ≈5 ns reference (§1), with the
   prescribed single rerun each — MDEs are session-wide facts, not
   #600's 2.5–3 ns band.
2. **CPU/op caliber (P0 §4.12)** — operational definition executed:
   uninstrumented wall is the *only* source for statistics; `rusage`
   user/sys ns/op are corroborating only (scheduler-tick granularity, the
   ±30 ns class P0 §4.12 noted — visible in the zero-syscall arms where
   sys≈0/user≈all); PMU (`perf instructions:u`/`cycles:u`) is a diagnostic
   caliber where **absolute values are whole-process approximations and
   cross-arm deltas are the signal**; static disassembly remains the
   authority for absolute instruction counts. A `syscalls/op` caliber was
   added during the campaign (ask-required, *not* preregistered — disclosed
   in every cell): exact strace -c counts (reps=1 minus reps=0 window
   isolation, or fixed-prime subtraction for write arms), counts only,
   never a wall source.
3. **Cold-cache policy (P0 §4.10)** — operational definition fixed and
   validated `[RUN]` (`p1-raw/cold-cache-validation.txt`):
   `sync; echo 3 > /proc/sys/vm/drop_caches` via password-protected sudo
   before **every** invocation; cold cells only on btrfs fixtures
   (`/var/tmp/sluice600-p1/cold64m.bin`); warmup=0, reps=1 single pass;
   write-cold uses a pre-primed scratch with `noprime` so the ladder's
   prime pass cannot refill the cache; tmpfs warm reads are never reported
   as cold. Validation numbers (provenance per the raw record): btrfs warm
   925.420 → **8720.546 after drop** (≈9.4×) → warm again 954.377; the
   8497.615 ns line in the "warm btrfs x2" section is the **first read of
   the freshly created fixture** (cold by construction, before any drop),
   not an after-drop value — the raw file's own verdict cites one cold
   value (~8200, 8.9×); tmpfs control unchanged (1010.792 → 1010.850).
   (The same mislabel of 8498 as an after-drop value appears in
   `P1_MEASUREMENT_PLAN.md` §7; the plan is left unedited as the
   preregistered record and this section is the corrected statement.)
   Scope disclosed: page-cache eviction only — btrfs metadata/allocator
   state, controller cache, shared-root-device noise and dirty-writeback
   timing are not controlled. Fallback if sudo becomes unavailable:
   `NOT_RUN_WITH_REASON`; no fadvise-only or tmpfs substitution (G0's
   effectively-warm cold column is not reused).
4. **Four-configuration correctness matrix** — command (executed at the
   campaign boundary `[RUN]`, `p1-raw/matrix-{release,debug,asan,asanubsan}.txt`):
   ```
   cd /home/jnhu/Source/Sluice-direct-cost-ladder && \
   for m in release debug asan asanubsan; do \
     xmake f -c -m $m --liburing=n -y && xmake -y && xmake test -y \
     | tee /home/jnhu/Source/Sluice/research/direct-beyond-parity/p1-raw/matrix-$m.txt; done
   ```
   Result: **58/58 passed in all four modes** (0 failed). Sanitizer/liburing
   linkage: the campaign cell record asserted a per-mode DT_NEEDED spot-check
   whose output was **not archived at the time**; the synthesis pass re-ran
   the check `[RUN]` on the still-present campaign build tree and archived
   it (`p1-raw/matrix-sanitizer-linkage.txt`): 60/60 executables in `asan`
   link libasan.so.8; 60/60 in `asanubsan` link libasan.so.8 + libubsan.so.1;
   release/debug link no sanitizer runtime; **no binary in any mode links
   liburing**. The planning-session release record was superseded by this
   campaign-boundary rerun at kit HEAD e096b4a1 (disclosed in the cell).
5. **Ladder coverage (P0 §4.13)** — closed: the committed kit adds write
   arms (W0/W1/W2, W1x/WX) and metadata arms (M0/M1/M2 × open-close,
   file_info, size) to the #600 read ladder; the campaign executed all of
   them (§4.3–4.4). Remaining un-laddered public ops: `read`, `write`,
   `read_exact`, `write_all` (shared-cursor forms) and
   resize/sync_data/sync_all (state-change group) — these carry **static**
   parity verdicts only (§6, `DIRECT_HOTPATH_LEDGER.md`).
6. **Comparator identity (P0 §4.8)** — write/metadata comparators now have a
   committed, rebuildable identity (kit commit e096b4a1, hashes in §0).
   The in-`/tmp` static audit comparators (a1_read, a1_write_at, N1-matched
   composition controls, etc.) remain session artifacts per AGENTS.md
   one-shot-probe rule; any future *decision* measurement must use committed
   arms.

## 6. Open attribution items `[UNCERTAIN]`

1. **Static-vs-ladder comparator tension.** Static audits find B1 single ops
   at or below their freshly compiled matched arms (read_at 39 vs 40,
   read 29 vs 35, write_at 39 vs 40, write 29 vs 35), while the ladder PMU
   deltas are a stable positive constant (≈+38 read / +19 write instr/op vs
   the ladder N1 arms) and the 0 B wall cells show +3.3 / +2.0 ns point
   estimates (sub-MDE). The two comparators are different artifacts: the
   audit arms are same-TU standalone compilations of the A1 pattern; the
   ladder N1 arms are in-harness mirrors, and B1 is reached through the
   archive boundary (PLT, no cross-TU inlining). Both facts stand; neither
   instrument contradicts the wall verdicts, and the ~38-instruction delta
   at ~4 IPC is consistent with the ~3 ns sub-MDE point estimates. Not
   resolved further in P1.
2. **Cross-round composition-residual contradiction (P2 must answer
   before claiming a target).** #600 recorded a post-C1/C2 AX−A2 residual of
   +9.7…+9.9 ns at 4 KiB (P0 §3.3 `[TRANSCRIBED]`); this session measured
   the same quantity shape — Sluice composition vs Sluice single op, 4 KiB —
   at ≈+19.0 ns (unpaired context, `p1-raw/read-composition-4k.txt`: AX
   721.206 vs a2 702.210) and +18.992 ns paired against A1x. The ~2× median
   difference is **not** explained by the wider session noise floor
   (sd_AA 10.4 vs #600 ≈5.0 widens the MDE, it does not double a median).
   Candidate explanations not ruled out in P1: comparator-arm differences
   between the rounds, host/kernel state drift between rounds, and residual
   uncontrolled session contention (several cells disclosed concurrent
   sibling runs). Consequence for P2: the composition candidate must
   preregister **which** number it claims to beat, measured against a
   freshly run baseline on the same host/session; the 9.9-vs-19 spread is
   itself part of what a controlled rerun must settle. The static
   decomposition (audit: ≈27 composition instructions ≈ 9.7–9.9 ns under
   #600's ≈0.36 ns/instr band) sits closer to the #600 number; the measured
   +19 ns remains the only in-session, CI-excluding value.
3. **Per-item nanosecond splits inside composition gaps** are static-derived
   (≈2.7–3.7 instr/ns calibrated against the aggregate); only the aggregate
   composition excess is measured (+18.992/+16.129 ns, read).
4. **read-sweep-64b** is contaminated by a host transient (both arms bimodal,
   pair sd 322 ns); §5.9 forbids silent rerun; verdict NO_CLAIM.
5. **file_info wall middle band** (+30.312 ns point, CI spans 0, PMU −2.0
   instr/op) — direction inconsistent with instruction parity; unexplained.
6. **#600 ledger count reconciliation** (42 vs 40) — see §0.
7. **EINTR cells** are non-stationary by construction; no claim caliber.
8. **MISSION P1.1 metric coverage is partial.** MISSION §3 P1.1 asks each
   path to record branches & mispredictions, CPU cycles, and effective vs
   overhead bytes. These documents report branches/calls/loads/stores,
   static instruction counts, and `instructions:u` cross-arm deltas;
   `cycles:u` were collected per plan §6.2 in every `*-pmu.txt` but are
   **not synthesized per-op here** (e.g. `read-composition-4k-pmu.txt`:
   AX ≈8720.5 vs A1x ≈8888.2 cycles/op — AX executes *fewer* cycles than
   A1x yet is wall-slower, consistent with a dependency/IPC effect; ≈4.0
   IPC on the 36.5M-instr context run corroborates the ~4 IPC used in
   §6.1).
   **Branch-misprediction counts were never measured anywhere in the
   campaign** (no `perf branch-misses` runs exist), and effective-vs-
   overhead byte accounting exists only implicitly (payload bytes = size;
   overhead = the instruction/syscall tables above). P2 must add these
   calibers before any MISSION P1.1 compliance claim.
9. **dududu double-fstat confounder** (P0 §4.5) is adapter-side, unmeasured
   here; any application-shape conclusion must fix it in a parity-corrected
   comparator first.

## 7. Sources

- Audits: `p1-audit/positioned-reads.md`, `p1-audit/write-primitives.md`,
  `p1-audit/composition-read.md`, `p1-audit/combined-writes.md`,
  `p1-audit/RESOURCE_METADATA_OPS_AUDIT.md`,
  `p1-audit/STATE_CHANGE_PERSISTENCE_AUDIT.md`.
- Plan + identity: `P1_MEASUREMENT_PLAN.md`, `p1-raw/ladder-identity.txt`,
  `p1-raw/cold-cache-validation.txt`, `p1-raw/smoke-ladder.txt`.
- Raw cell records: `p1-raw/<cell>.txt` (+ `-pmu.txt` / `.syscalls.txt` /
  `-diagnostics.txt` / attempt & window logs) for every cell named in §4.
- Inherited: `P0_BASELINE_AND_OWNERSHIP.md` (§1.3 B1 identity, §3 verified
  eliminations, §4 open questions), #600 evidence `[TRANSCRIBED]`.
