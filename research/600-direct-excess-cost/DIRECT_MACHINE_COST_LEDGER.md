# DIRECT_MACHINE_COST_LEDGER — Sluice #600 Phase I (E2 + E3)

All assembly evidence: `evidence/asm/read_at.O3.txt`, `evidence/asm/read_exact_at.O3.txt`
disassembled from the shipped `sluice_core` object (`-O3 -DNDEBUG`, see DIRECT_COST_DAG).
All timing: `evidence/e3_*.txt`, 16 interleaved AB/BA pairs per cell, taskset cpu2,
performance governor, tmpfs page-cache-resident 64MiB fixture.

## Static instruction ledger — `read_at` success path (legal, nonzero, valid input)

| Region | Instructions | Classification |
|---|---|---|
| entry + File state load (fd, access) | 8 | necessary (public state reads) |
| closed / access / length-0 / range / transfer / null checks | 18 | necessary v2 semantics |
| Precheck struct build→copy→reload→branch | 27 | **excess** (implementation artifact) |
| redundant `checked_posix_offset` sign test (0x220 `test/js`) | 2 | **excess** (D1) |
| arg spills before `call pread` + reloads | 7 | mostly necessary at this call shape |
| `call pread` | 1 | the syscall |
| EINTR test (`test rax; jns`) | 2 | necessary |
| Result stores (value, errno dead-store, has_value) + epilogue | 5 | ABI-necessary shape |
| **Total** | **≈70** | vs ≈42 for matched native wrapper |

Matched native wrapper (A1, hand-written, same checks/order/errors, same TU boundary):
42 instructions including its own 5 spills and same sret store shape — GCC emits the same
Result ABI scaffolding for it. **A2 − A1 static ≈ 28 instructions.**

`read_exact_at` single-step composition: outer precheck call + inlined step validation +
CompositionState stack materialization ≈ AX − A2 static ≈ 45–60 instructions/step.

## GCC elision findings

- COMPILER_ELIDED: the three source-level range conditions collapse to two compares +
  one or/sign test; `precheck_data_op`'s offset bound and transfer bound fuse.
- COMPILER_ELIDED: `Result<off_t>` scaffolding of `checked_posix_offset` folds to a bare
  sign test; no 24-byte temporary materializes for it.
- **NOT elided (D1)**: the sign test itself duplicates the already-proven offset bound —
  survives at `read_at+0x100` (`test %r11,%r11; js`).
- **NOT elided (dominant)**: `Precheck{optional<IoError>, bool}` round-trips through
  memory on every call (merge-tail at `read_at+0x1d0..0x2a0`: two SSE copies + byte
  reloads). SRA does not scalarize it because every rejection edge merges there.

## Measured ladder (median ns/op; pairs 16/16 positive unless noted)

| size | A0 raw pread | A1 matched | A2 sluice read_at | AX read_exact_at | A2−A1 | t | rel |
|---|---|---|---|---|---|---|---|
| 0 B (no syscall) | 525.7* | 10.5 | 23.4 | — | **+12.8** | +148 | +122% |
| 1 B | — | 531.7 | 550.2 | — | **+19.3** | +16.4 | +3.6% |
| 64 B | — | 535.3 | 553.5 | — | **+19.0** | +12.1 | +3.5% |
| 512 B | — | 584.9 | 600.7 | — | **+16.1** | +14.8 | +2.8% |
| 4 KiB | 968.8 | 972.9 | 990.0 | 1017.8 | **+15.8** | +19.5 | +1.6% |
| 64 KiB | — | 9104.9 | 9092.7 | — | +6.2 | −0.66 | n.s. |

*A0 at 0 B still issues `pread(fd,buf,0,off)` — enters the kernel; shown only as the
syscall-entry floor, not a product alternative.

Cross-checks at 4 KiB: A2−A0 = +19.3 ns (t=+12.5, 16/16); AX−A2 = +32.0 ns (t=+13.8,
16/16). A/A calibration (a2 vs a2): pair-delta sd 5.0 ns with a −5.1 ns systematic
first-run-faster order effect, canceled by the ABBA alternation; minimum detectable
effect ≈ 2.5–3 ns at 16 pairs.

Perf user-space instruction deltas (instructions:u, corroborating static counts; absolute
counts inflated by PLT/retpoline/sysret speculation, common to all arms):
1 B: a1−a0 = 76.4, a2−a1 = 60.6, ax−a2 = 172.4; 4 KiB: a1−a0 = 40.0, a2−a1 = 30.9,
ax−a2 = 88.9. Order and magnitude class match the static ledger (a2−a1 ≈ 28).

rusage (4 KiB, uninstrumented): user ≈ 320–390 ns/op, sys ≈ 650–690 ns/op across
A0/A1/A2/AX (tick-granularity noise ±30 ns; wall-clock pairs remain the time oracle);
A3 no-kernel stub: user 8.8 ns/op, sys 0.

Syscalls/op (strace -c, exact): A0 = A1 = A2 = AX = 1.000 per op (37,388 pread64 for
37,384 accounted ops); size-0 Sluice path issues 0 pread64 (fast path, no syscall).
Sluice adds no syscall, no extra memory traffic beyond the Precheck stack round-trip.

## E4 — application attribution (dududu Parquet R1, untouched receipts)

R1 medium fullscan: 57 reads, 181,996,415 bytes (dududu `benchmarks/parquet/results/r1/
summary.txt:18-19`; ~3.2 MiB mean per request). Whole-reader T3 medium ≈ 134–138 ms.
Applying the measured per-call excess (≈ +17 ns × 57 ≈ 0.97 µs) gives ≈ 0.0007% —
arithmetically undetectable in R1, as predicted in #600 review. R1's sign-flipping
whole-reader history is therefore consistent with, and cannot refute, the mechanism-level
excess proven above. A many-small-read application shape is where the mechanism matters.
Adapter confound (not Sluice core): dududu P2 `backend_sluice.cpp:63,75` calls
`file_info` then `size` (which runs `file_info` again) per open → duplicate `fstat`.
That is a dududu adapter defect to fix under a separately named parity-corrected
comparator; R1 receipts unchanged.

## Verdict-relevant summary

- EXCESS_INSTRUCTIONS: ≈28/call vs matched native (read_at), ≈45–60/step (composition).
- EXCESS_CYCLES: ≈16–19 ns/op warm (syscall path), +12.8 ns at the 0 B no-kernel path.
- EXCESS_SYSCALLS: 0.
- VALIDATION_COST (necessary part): 18 instructions — retained by any correct wrapper.
- RESULT_ABI_COST: sret 24-byte return, 3 stores + spills — identical in A1; necessary
  public-interface cost, no Sluice-specific headroom → D3 NO_CHANGE.
- COMPOSITION_COST: +32 ns per fully-satisfiable single step (AX−A2, 4 KiB).
