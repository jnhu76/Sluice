# PHASE3_VALIDATION — Sluice #600 (E5 Native Linux)

BASE_SHA: bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57
CANDIDATE_SHA: ea2fc4fca1a6 (branch sluice-600-direct-research, commit "perf(direct):
dispatch admission verdicts without Precheck materialization (#600 C1)")
Library identities (sha256, deterministically archived):
  baseline  489104b5819cdaca251471d5e0b10c20051071bca88dcb7bfc198f1336c99c5a
  candidate d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0
The same two hashes were reproduced through dududu's clean-room SDK install path
(/tmp/sluice600/dudu-{base,cand}-root/sluice-sdk/noliburing), tying the application
pairing below to the exact bytes measured by the ladder.

## V1 Correctness (candidate)

| gate | result |
|---|---|
| xmake test, release | 58/58 passed |
| xmake test, debug | 58/58 passed |
| xmake test, asan | 58/58 passed |
| xmake test, asan+ubsan | 58/58 passed |
| mutation oracle (C1) | 3 named mutants RED with named kills, restored GREEN |
| package verifier (installed SDK, clean-room consumers) | all gates PASS |
| W-01 bench (full case set, byte oracles) | 0 failing repetitions |
| C++ comment authority check | pass |

Mutation kills: SLUICE_SEM_MUTANT_ADMIT_INVALID_RANGE →
`read_with_illegal_offset_reports_invalid_argument`;
SLUICE_SEM_MUTANT_EMPTY_BEFORE_CLOSED →
`sequential_read_on_write_only_file_rejected_upfront` (precedence arm; the closed+empty
arm `sequential_zero_length_read_after_close_reports_invalid_state` is the same
precedence rule); SLUICE_SEM_MUTANT_ADMIT_UNREPRESENTABLE_SIZE →
`resize_overflow_reports_invalid_argument`. C2 research mutants (step full-count,
step EINTR skip) were killed by `short_reads_report_eof_before_full_with_the_prefix`
and `primitive_retries_eintr_without_a_completed_count` before C2 was reverted.

## V2 Direct-op performance (frozen baseline vs candidate, ABBA 16 pairs/cell)

| cell | baseline median | candidate median | delta | t | sig |
|---|---|---|---|---|---|
| read_at 0 B (no syscall) | 23.4 ns | 10.5 ns | −13.3 ns (−57%) | −179.8 | 16/16 |
| read_at 1 B | 531.7 ns | 514.6 ns | −16.4 ns (−3.0%) | −10.0 | 16/16 |
| read_at 64 B | 535.3 ns | 519.4 ns | −15.8 ns (−2.9%) | −11.4 | 16/16 |
| read_at 512 B | 584.9 ns | 568.7 ns | −16.2 ns (−2.7%) | −10.1 | 16/16 |
| read_at 4 KiB | 978.7 ns | 963.2 ns | −16.6 ns (−1.7%) | −12.4 | 16/16 |
| read_at 64 KiB | 9092.7 ns | 9089.5 ns | −3.2 ns | −0.03 | n.s. (expected) |
| read_exact_at 4 KiB | 1018.3 ns | 984.0 ns | −33.8 ns (−3.3%) | −22.4 | 16/16 |

Independent confirmation of the first 4-KiB series in a second session: −14.2 ns
median (t=−10.2, 16/16). Syscalls/op identical (1.000, strace-exact, both libs);
zero-length path issues no syscall. Tail latency: per-op sampling clock overhead
(~26 ns, W-01 CAL) exceeds the effect size, so per-op tails are not a supportable
statistic for this delta; block-mean pairs above are the reported evidence.
Instrumented views: perf instructions/cycles (Phase I ledger), rusage user/sys,
strace counts; uninstrumented wall-clock pairs are the timing oracle.

## V3 Application transfer (dududu Parquet R1, medium, frozen dataset+trace)

Dataset sha256 6116e69c…, trace: 57 requests / 181,996,415 B (matches the R1 record
exactly). ABBA 16 pairs, taskset cpu2, warm cache:

| series | base median | cand median | delta(cand−base) | t |
|---|---|---|---|---|
| Replay/sluice (P2 I/O-only) | 240.649 ms | 240.760 ms | −82.5 µs mean | −0.20 |
| Replay/native (P1 control, no Sluice code) | 240.375 ms | 240.615 ms | +536.0 µs mean | +2.15 |
| T3_fullscan/p2_sluice (whole reader) | 361.498 ms | 360.548 ms | −31.9 µs mean | −0.07 |
| T3_fullscan/p1_native (control) | 362.857 ms | 362.506 ms | −429.1 µs mean | −0.42 |

Preregistered arithmetic: 57 calls × ~16 ns ≈ 0.9 µs on a 240 ms replay (0.0004%) —
undetectable, and the P1 control arms (identical non-Sluice code in both builds)
swing ±0.2–0.5 ms, bounding the harness noise far above the possible effect.
**No application-level transfer for the R1 shape, and no regression.** A
many-small-reads application shape is where the per-call mechanism gain lands.

## C2 record (stopped variant)

Composition pre-validated step primitive: implemented, assembly-proven work
elimination (compose loop calls only pread/`__errno_location`; out-of-line precheck
clone gone), fault-injection mutants killed, but zero measurable wall-clock gain
over C1 (remaining AX−A2: +9.9 ns → +9.7 ns, within noise) → admission rule
("only when repeated validation is measurable") not satisfied post-C1 → reverted.
Patch preserved: evidence/c1c2_full_worktree.patch.

## D3 record (no change)

`Result<size_t>`'s sret + spills + errno-store shape is emitted identically for the
hand-written matched native control (A1): it is the necessary cost of the public
interface, not Sluice-specific excess. NO_CHANGE_JUSTIFIED.

## V4 verdict inputs

- EXCESS_MACHINE_WORK_PROVEN: Phase I (+12.8…+19.3 ns/call, 16/16, t=10–148).
- EXCESS_MACHINE_WORK_ELIDED: C1 removes it (−13.3…−16.6 ns, 16/16, t=−10…−180;
  success-path instructions 70→40; assembly archived).
- COMPILER_ALREADY_ELIDED: recorded for the fused range conditions and the folded
  Result<off_t> scaffolding (they were never part of the excess).
- MECHANISM_ONLY_GAIN: for the R1 Parquet application shape (0.0004% arithmetic,
  V3 null with noise-bounded controls). For direct small-read shapes the gain is
  1.4–3.0% per warm op on this E5.
- Platform scope: E5 Native Linux only; no claim for other hardware/OS/workloads.
