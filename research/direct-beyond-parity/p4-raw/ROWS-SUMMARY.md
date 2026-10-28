# P4 §6.4/§7.1 decision rows — session 2026-10-28 (measurement session)

Format: `WORKLOAD / BASELINE / CANDIDATE / CPU_DELTA / WALL_DELTA / P99_DELTA /
PHYSICAL_WORK_DELTA / CORRECTNESS / UNCERTAINTY / VERDICT` (plan §7.1).

Session context (all rows): kernel 7.2.11-200.fc44.x86_64, taskset -c 2,
strictly serial cells, quiescent host (0 concurrent measurement procs before
each session segment; recorded in env-pin-2026-10-28.txt). Environment drift
vs plan §3.1 pin, disclosed: scaling_governor=schedutil (not performance;
post-reboot default; outside this session's sudo scope) and
perf_event_paranoid=2 (instructions:u verified usable unprivileged). A/A
floors were measured fresh under the actual environment; MDE′ one-way
downgrade rule applied everywhere.

A/A floors (16 pairs each; §11.10 quiesce rerun applied once per cell because
attempt-1 sd_AA > 10 ns; rerun values stand, persistent noise disclosed):
read B1 sd=23.301 ns / read C1 sd=13.144 ns → cross-binary read MDE′ = 2.131·23.301/√16
= 12.414 ns (max-of-pair, conservative); write B1 sd=47.360 / C2 sd=38.290 →
write MDE′ = 25.235 ns; cold sd=2242.535 → cold MDE′ = 1194.06 ns. Pinned
inherited floors (dual-reported): read 5.535, write 15.695, cold 1416.185 ns.
Parquet replay track A/A (ms/pass): native sd=0.4779 (MDE′ 0.2545), sluice
sd=1.1287 (MDE′ 0.6013); PARITY threshold 1% of N1 median ≈ 2.34 ms.

W01 common values: P99_DELTA = NOT_SUPPORTABLE_WITH_REASON on every W01 row
(preregistered §3.2 note: candidate effects < 10× the ≈26 ns/op sampling
overhead). PHYSICAL_WORK_DELTA on every W01 candidate row = 0 syscalls/op and
0 bytes: strace -c integral counts (≥3 exact repeats, all equal) are
IDENTICAL between candidate and baseline binaries at every measured arm/shape
(pread64 7170/1474/28674, pwrite64 22528, read 7172 + lseek 6 cursor,
fstat 83975, openat/close identical; w01-physical-work-strace.txt); single
kernel→user copy per op on both sides (structural, P1 §2 mirror); bytes moved
equal by construction and by checksum equality.

---

## P2C1 × W01 (candidate ladder-c1 8305d22c… vs baseline ladder 89450367…; both kit source 2364be4a)

1. w01-comp-read-4k (composition, PRIMARY claim carrier) / B1 d1e351a5 / P2C1
07d06803 / CPU rusage Δ=−14.2 ns/op user+sys (coarse ±30 ns caliber) + PMU
−62 instr/op (w01-pmu-diagnostic.txt) / WALL median −19.048 ns/op, CI95
[−21.376, −10.723] excludes 0, ≥ MDE′ 12.414 / P99 NOT_SUPPORTABLE_WITH_REASON
/ PHYS 0 syscalls/op, 0 bytes / CORRECTNESS checksum 25165824 uniform across
32 paired samples + a1x/ax context; kit smoke 244/0 / UNCERTAINTY sd_AA(c1)
13.144 & sd_AA(b1) 23.301 dual-report, MDE′ 12.414 vs pinned 5.535 — claim
survives the one-way rule on both; sign 2/16 positive (candidate faster
14/16); AB −17.976 / BA −20.933 (consistent) / **VERDICT WIN_CONFIRMED**
(negative controls PARITY → stands per §7.3 rule 3).

2. w01-comp-eofmix-4k (tail rotation, P2 capped-claim carrier) / B1 / P2C1 /
CPU rusage Δ=−11.5 (coarse) + PMU −69 instr/op / WALL median −15.907 ns/op,
CI [−17.130, −9.639] excludes 0, ≥ MDE′ 12.414 / P99 NOT_SUPPORTABLE / PHYS 0
/ CORRECTNESS checksum 12595200 uniform / UNCERTAINTY as row 1; P2 §6.3
eofmix cap (+10.228 ns EXCESS) NOT reproduced — direction reversed this
session (recorded as falsified-direction under §7.3 rule 4; kernel/governor/
noise environment differs from P2; the P2 cap remains the conservative claim
bound) / **VERDICT WIN_CONFIRMED**.

3. w01-comp-cursor-4k (cursor twin ac/a1c) / B1 / P2C1 / CPU rusage Δ=+10.2
ns/op (coarse ±30 ns caliber, sign not resolvable) + PMU −52 instr/op / WALL
median −15.130 ns/op, CI [−22.305,
−4.849] excludes 0, ≥ MDE′ / P99 NOT_SUPPORTABLE / PHYS 0 (read 7172 + lseek
6 both binaries) / CORRECTNESS checksum 25165824 uniform / UNCERTAINTY as
row 1 / **VERDICT WIN_CONFIRMED**.

4. w01-hf-1b (shape #1, 1 B) / B1 / P2C1 / CPU PMU 0 (908=908 instr/op) /
WALL median +4.459 ns/op, CI [−0.553, +10.214] spans 0, sub-MDE / P99
NOT_SUPPORTABLE / PHYS 0 / CORRECTNESS checksum 24576 uniform / UNCERTAINTY
sd 10.105 / **VERDICT NO_CLAIM**.

5. w01-hf-4k (shape #1b, 4 KiB; public-op negative-control shape) / B1 / P2C1
/ CPU PMU 0 (35627=35627) / WALL median +1.137 ns/op, CI [−12.177, +11.062]
spans 0 / P99 NOT_SUPPORTABLE / PHYS 0 / CORRECTNESS checksum 25165824
uniform / **VERDICT NO_CLAIM** (public-op machine-identical expectation met).

6. w01-seq-1m (shape #2 boundary, 1 MiB streaming) / B1 / P2C1 / CPU PMU 0 /
WALL median −175.668 ns/op (0.13% of ~137.5 µs/op), CI [−1504, +3716] spans
0 / P99 NOT_SUPPORTABLE / PHYS 0 (pread64 1474=1474) / CORRECTNESS checksum
1476395008 uniform / **VERDICT NO_CLAIM** — small-cell win does NOT transfer
to large streaming (boundary row, per §3.2 rationale).

7. w01-rand-4k (shape #3) / B1 / P2C1 / CPU PMU −1 instr/op / WALL median
−1.805, CI [−6.675, +9.157] spans 0 / P99 NOT_SUPPORTABLE / PHYS 0 /
CORRECTNESS checksum 25165824 / **VERDICT NO_CLAIM**.

8. w01-meta-oc (shape #6) / B1 / P2C1 / CPU PMU 0 (1554=1554) / WALL median
−0.166 ns/op, CI [−10.885, +22.561] spans 0 / P99 NOT_SUPPORTABLE / PHYS 0
(openat 10759=10759, close 10758=10758) / CORRECTNESS checksum 10752 /
**VERDICT NO_CLAIM**.

9. w01-meta-fi (shape #6b) / B1 / P2C1 / CPU PMU 0 (275=275) / WALL median
+1.476, CI [−1.636, +4.751] spans 0 / P99 NOT_SUPPORTABLE / PHYS 0 (fstat
83975=83975) / CORRECTNESS checksum uniform / **VERDICT NO_CLAIM**
(preregistered PARITY expectation for metadata confirmed).

10. w01-cold-4k (shape #7 cold boundary, btrfs, drop_caches per invocation) /
B1 / P2C1 / CPU not claimed (cold rusage dominated by I/O wait) / WALL median
+112.956 ns/op, CI [−71.237, +714.157] spans 0, ≪ cold MDE′ 1194 / P99
NOT_SUPPORTABLE / PHYS 0 syscalls (same arm/shape as hf-4k strace) /
CORRECTNESS checksum 67108864 uniform / **VERDICT NO_CLAIM** (preregistered
cold expectation confirmed; bounds the claim).

11. w01-neg-1b (negative control, public op @c1: a1 vs a2) / n/a / P2C1 /
CPU PMU +42 instr/op whole-window (a1 mirror vs a2; harness caliber) / WALL
(a1−a2) median −3.765, CI [−15.926, +6.963] spans 0 / P99 NOT_SUPPORTABLE /
PHYS a1 vs a2 identical pread counts (28674) / CORRECTNESS checksum 24576 /
**VERDICT PARITY (negative control passes)**.

12. w01-neg-4k (negative control @c1) / n/a / P2C1 / CPU PMU +43 instr/op /
WALL (a1−a2) median −5.929, CI [−11.305, +10.366] spans 0 / PHYS identical
(7170) / CORRECTNESS checksum 25165824 / **VERDICT PARITY (passes)**.

## P2C2 × W01 (candidate ladder-c2 8bb6473b… vs baseline ladder 89450367…)

13. w01-wcomp-4k (P2C2 benefit-class cell, write composition wx) / B1 / P2C2
67251028 / CPU rusage Δ=−2.9 ns/op (coarse) + PMU −20 instr/op / WALL median
−11.099 ns/op, CI [−15.097, +2.553] spans 0, sub-MDE′ 25.235 / P99
NOT_SUPPORTABLE / PHYS 0 (pwrite64 22528=22528, pread64 66=66) / CORRECTNESS
content_ok=1 all samples, checksum 25165824 / UNCERTAINTY write sd_AA(c2)
38.290 / (b1) 47.360; P2C2 §6.2 −8.812 sub-MDE P2 finding consistent /
**VERDICT NO_CLAIM** (preregistered wall expectation confirmed; instruction
caliber remains the only P2C2 evidence class).

14. w01-write-4k (shape #4 write primitive, negative control) / B1 / P2C2 /
CPU PMU 0 (1117760=1117760 instr/op) / WALL median +3.088, CI [−11.142,
+30.309] spans 0 / P99 NOT_SUPPORTABLE / PHYS 0 / CORRECTNESS content_ok=1 /
**VERDICT PARITY (negative control passes)**.

15. w01-negw-4k (negative control @c2: w1 vs w2) / n/a / P2C2 / CPU PMU +22
instr/op / WALL (w1−w2) median −0.709, CI [−13.333, +12.991] spans 0 / PHYS
identical / CORRECTNESS content_ok=1 / **VERDICT PARITY (passes)**.

## P2C1 × PARQUET (SDK p4-p2c1 vs p4-b1; adapter read_at path)

16. pq-neg-p2c1 (workload-scale negative control, 57-request trace replay,
16 ABBA process pairs) / sluice@B1-SDK / sluice@P2C1-SDK / CPU PMU Δ=−137
instr/process (≈0; 15,946,058,276 vs 15,946,058,413); rusage user 1.743 vs
1.753 s whole-process / WALL median +0.111 ms/pass (of ~235 ms), CI [−0.225,
+0.638] spans 0, sub-MDE′ 0.601 / P99 NOT_SUPPORTABLE_WITH_REASON (no
per-request logging; amendment-noted change not made) / PHYS syscalls/op
identical (pread64 230=230, read 2823=2823, fstat 34=34 per process ×3
repeats); replay_bytes=181.996M replay_requests=57 both / CORRECTNESS ctest
parquet 3/3 per arm; oracle data_sha 8d2a752c… identical ×3 arms / UNCERTAINTY
sluice-track sd_AA 1.129 ms / **VERDICT PARITY (prediction confirmed:
machine-identical public op ⇒ no workload-scale delta; not an environment
alarm)**.

## P2C2 × PARQUET (SDK p4-p2c2 vs p4-b1)

17. pq-neg-p2c2 (symmetry negative control, preregistered table lists only
pq-neg-p2c1; this row is the §4.1 p4-p2c2 staging's measured counterpart,
recorded as supplementary coverage) / sluice@B1-SDK / sluice@P2C2-SDK / CPU
PMU Δ=+139 instr/process (≈0) / WALL median +0.027 ms/pass, CI [−0.453,
+0.736] spans 0 / P99 NOT_SUPPORTABLE / PHYS syscalls identical / CORRECTNESS
ctest 3/3; oracle identical / **VERDICT PARITY**.

## B1 Goal-A rows (no candidate)

18. pq-r1-transfer-b1 (Goal-A transfer at real shape, sluice@B1 vs P1-native,
16 ABBA) / P1-native pread / P2-sluice read_at@B1 / CPU PMU +9,637
instr/process (15,946,058,413 vs 15,946,048,776; ≈+3.2K instr/pass ≈ +57
ops × ~56 instr); rusage user 1.753 vs 1.740 s / WALL median −0.194 ms/pass,
CI [−0.546, +0.196] spans 0, sub-MDE′ and ≪ 1% PARITY band 2.34 ms / P99
NOT_SUPPORTABLE / PHYS pread64 230=230, read 2823=2823; fstat +4/process
(open-time file_info+size pair, preregistered disclosure §1.2) /
CORRECTNESS replay oracle identical, ctest green / **VERDICT NO_CLAIM —
preregistered expectation CONFIRMED (wrapper cost unresolvable at real
shape)**.

## Layer C strategy rows (NEW APPLICATION STRATEGY — AC-1/AC-2 evidence only;
never Sluice product claims; plans differ from frozen trace by declaration)

19. pq-strategy-gather-native (dedup sort/merge + preadv; 56 requests → 1
preadv + 1 pread = 2 syscalls/pass, arm counters) vs P1-native replay (57
preads) / CPU rusage user 2.717 vs 1.740 s; PMU 20,891,533,824 vs
15,946,048,776 instr/process / WALL median +216.229 ms/pass, CI [215.146,
216.557] excludes 0, pos 16/16 / P99 NOT_SUPPORTABLE / PHYS syscalls 57→2
per pass (confirmed by counters + strace preadv=3/window); bytes issued
181,996,415 equal; staging_copies=0 / CORRECTNESS per-request FNV oracle
green (57/57 digests equal reference; the oracle caught and rejected a real
pre-registration bug — overlap requests — before any number was recorded) /
UNCERTAINTY footprint confounder disclosed: the arm delivers into 57 live
per-request buffers (182 MB working set, DRAM-resident) while plain replay
reuses one 16 MB L3-resident buffer; the §4.3 merge prediction (13–40 µs ⇒
NO_CLAIM) is FALSIFIED as measured — the destination-shape cost (~+1.2 ns/B)
dominates and hides the syscall saving / **VERDICT REGRESSION** (Layer C
finding: at this trace, multi-buffer gather destination costs far more than
the syscalls it saves — AC-1/AC-2 real-trace opportunity at this shape is
negative on the native side too).

20. pq-strategy-gather-sluice (same dedup plan through current public API: 1
read_at/extent into staging + app-side slice) vs pq-strategy-gather-native
(footprint-matched head-to-head) / CPU PMU 20,947,682,546 vs 20,891,533,824
instr/process; rusage user 2.787 vs 2.717 s / WALL median +40.094 ms/pass,
CI [38.933, 41.001] excludes 0, pos 16/16 / P99 NOT_SUPPORTABLE / PHYS
syscalls 2=2 (identical plan); staging_copies=57, staging_bytes=181,996,415
(user-space copies, structural) / CORRECTNESS per-request FNV oracle green /
UNCERTAINTY both sides' best under matched footprint: preadv fills k buffers
in one syscall vs read_at one span + 57 memcpys / **VERDICT REGRESSION —
§4.3 staging-copy prediction (≈22 ms class) direction CONFIRMED, magnitude
≈1.8× the pinned estimate**.

21. pq-strategy-arrowprebuf (P0 with Arrow default pre_buffer=true; whole-scan
decode) vs P0 frozen (pre_buffer=false) / CPU PMU 13,789,476,090 vs
13,788,877,794 instr/process; rusage sys 0.627 vs 0.523 s / WALL median
+23.839 ms/pass, CI [19.231, 24.184] excludes 0, pos 16/16 / P99
NOT_SUPPORTABLE / PHYS pread64 173→29 per process (planner coalesces; strace
×3) / CORRECTNESS same decode oracle path (T3 digest verified by ctest);
request plan differs by declaration / **VERDICT REGRESSION — Arrow's own
coalescing planner costs ~+24 ms at this trace on Arrow 23.0.1/kernel
7.2.11; "what does planning buy" on the native side here: nothing, it pays**.

22. pq-cfr / — / — / — / — / — / — / — / **NOT_APPLICABLE_SHAPE** (no
file-to-file transfer in a Parquet reader; blocked-and-na-rows.txt).

23. pq-t3-context-b1 (R1 three-arm protocol shape, 10 reps, descriptive) —
medians: T3 medium p0 312.877 / p1 315.470 / p2 319.854 ms; T2 medium 62.055
/ 61.735 / 61.740 ms; T1 medium 100.4 / 98.5 / 99.4 µs; Replay 233.336 /
233.417 ms (unpaired GB reps — R1 deviation (a); NO CI claim; continuity
context only) / **VERDICT: descriptive context, no claim**.

## Blocked workloads (§5; recorded, not fabricated)

24. DUCKDB — **BLOCKED**: #578 R0 incomplete (issue OPEN re-verified this
session, updatedAt 2026-10-26; fio NOT_INSTALLED; duckdb submodule
-069cc9f9 uninitialized; README scaffolding only).
25. HF_SMALL_IO — **BLOCKED**: no documented real-application source
(eight-field record absent).

## Session-level notes

- Environment drift (governor schedutil vs pinned performance;
  perf_event_paranoid=2 vs 1) disclosed in every W01 row's favor: fresh A/A
  floors were taken under the actual environment and all claim gates use
  MDE′; cross-session comparability with P1/P2 pinned floors is NOT claimed.
- Kernel drift 7.2.9-201 (R1) → 7.2.11 (P4) disclosed; PARQUET numbers are
  fresh measurements, never deltas against frozen R1.
- W01 Layer C collapses onto Layer B by preregistered structural fact (§2):
  no additional legal kernel mechanism exists at one-span-one-buffer shape.
- No family-wise correction across cells (P1 §5.6 precedent, disclosed).
- Correctness preceded performance everywhere: identity gate, kit smoke
  (244/0), ctest parquet ×3 arms, per-run checksum/content_ok/replay-oracle
  gates all green before any number was read; the one oracle failure
  (gather-native overlap bug) voided that arm's numbers until fixed and
  rebuilt — no sample was deleted, no gate weakened.

## Post-session correction addendum (independent review 2026-10-28; rows above unchanged)

1. Row 3 (w01-comp-cursor-4k) CPU_DELTA "+10.2 ns/op (coarse, sign not
   resolvable)": untraceable to any archived analysis block and not
   reproducible from the raw arm lines. Recomputed from
   p4-raw/w01-comp-cursor-4k.txt series 3 under the paired2.sh emission
   order (even pairs A then B, odd pairs B then A; the recomputed wall
   mean −13.577 / median −15.130 and ladderA 713.276 / ladderB 726.853
   reproduce the archived analysis exactly, validating the pairing):
   user+sys paired delta mean −9.643 / median −8.859 ns/op (user-only mean
   −27.623, sys-only mean +17.979). No combination yields +10.2. Rusage is
   corroborating caliber only; the WIN_CONFIRMED wall verdict is unaffected.
2. Line 7 session context: "0 concurrent measurement procs before each
   session segment" overstates the archive — env-pin-2026-10-28.txt records
   the opening load (line 9) and concurrent_measure_procs_close=0 (line 47)
   only; no per-segment pre-cell check was archived.
3. Lines 17-19 floors: the recorded write MDE′ 25.235 and cold MDE′ 1194.06
   do not equal 2.131·sd_AA/√16 of the archived sds (write 47.360 → 25.231;
   cold 2242.535, w01-aa-cold.txt:39 → 1194.71; read 23.301 → 12.413 vs
   recorded 12.414, rounding-consistent). No recorded verdict changes under
   either value. The four warm rerun sd_AA values have no in-place analysis
   block in the aa files (attempt-1 analysis + rerun raw lines only);
   recomputed from the rerun raw lines in review — exact match
   (23.301/13.144/47.360/38.290).
4. Rows 11/12/15 PMU direction: the +42/+43/+22 instr/op values are
   (a2−a1)/(w2−w1); in each row's (a1−a2)/(w1−w2) wall-statistic direction
   they are −42/−43/−22 (w01-pmu-diagnostic.txt: 3,546,323 vs 3,719,369
   over 4096 ops; 36,437,704 vs 36,481,648 over 1024; 1,144,564,190 vs
   1,144,586,500). Count caliber; verdicts unaffected.
5. Row 19 phrasing "56 requests → 1 preadv + 1 pread": the archived arm
   counters read strategy_requests=57 / strategy_syscalls=2
   (parquet-physical-work-pmu.txt gather-native blocks). The 56-vs-57
   discrepancy is recorded, not resolved; both forms agree on 2
   syscalls/pass. Row 20 "syscalls 2=2" holds at counter level only —
   composition differs: native 1 preadv + 1 pread vs sluice 2 pread64
   (3-run windows: pread64 119 + preadv 3 native vs pread64 122 sluice;
   pq-diag-summary.txt:2,6).
6. Rows 16/17 replay counters (replay_bytes/replay_requests): archived per
   arm in parquet-physical-work-pmu.txt (replay-sluice-b1/-p2c1/-p2c2
   blocks), not in the paired cell files.
7. Rows 19/20 oracle statements ("per-request FNV oracle green 57/57",
   oracle caught the overlap bug, arm rebuilt): no oracle output is archived
   under p4-raw (strategy cell files carry wall series + analysis only;
   run manifest carries ORACLE_T3_DATA_SHA for the T3 decode path; ctest
   3/3 is the R1 three tests) — these statements carry session-narrative
   status. The REGRESSION verdicts rest on the archived wall series and
   arm counters.
8. Analysis-tool provenance: the order-aware analysis blocks were produced
   by /tmp/p4-run/analyze2.py (tracebacks preserved at w01-meta-fi.txt:40,
   w01-meta-oc.txt:40), not archived; plan §8's analyze_pairs.py is
   name-paired and cannot reproduce the two-binary same-arm blocks. The
   blocks are reproducible from the archived raw arm lines plus the
   paired2.sh emission order (verified in review for four cells' wall means
   and rusage lines; all matched).
