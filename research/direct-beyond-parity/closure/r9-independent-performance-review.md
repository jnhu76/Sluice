# R9 Independent Performance Review — §12.2 (transcribed record)

- Scope: E5 Closure workflow R9 §12.2 独立性能评审 — audit entry point is the
  full P2C1 perf=INCONCLUSIVE evidence (`closure/perf-p2c1/`: SUMMARY.md,
  ENV-P2C1.txt, 9 raw sample files, 3 PMU diagnostics, 6 disassembly identity
  files, hash-pinned runner/analyzer/fixture tools), plus a spot-check of the
  R5 R0 native baseline (`closure/r5-native/`, incl. fio/DuckDB raw samples)
  for A/A and workload attribution. All 12 mission §12.2 checks were
  verified: E5 Host Identity, Baseline/Candidate SHA, Library Hash, A/A, ABBA
  pairing, MDE, raw samples, negative control, Wall/CPU/Instruction
  separation, actual syscall count, application attribution boundary,
  regression status. Every performance conclusion is anchored to a named
  workload. Zero writes to the repo, zero merges, zero pushes; frozen
  worktrees read-only.
- **Verdict: APPROVE** (all six findings below are non-blocking; none changes
  a verdict).
- Provenance: this file transcribes the R9 performance-audit lane result
  delivered 2026-10-29 into the evidence archive, verbatim in substance.

## Findings

- **F1 (minor, transcription error, does not affect the verdict)**:
  SUMMARY.md §4 (`closure/perf-p2c1/SUMMARY.md:114-115`) calls the
  +91.3/+57.7/+45.5 outliers "in the cand-favorable direction" and −73.1/−35.4
  "in the other" — the direction labels are flipped. The statistic is
  cand−base (negative favors the candidate); positive outliers actually favor
  the baseline and raise the candidate median. The INCONCLUSIVE adjudication
  is unaffected (two-sided outliers inflate sd either way).
- **F2 (minor, analyzer defect, does not affect the verdict)**: SUMMARY §4/§6
  claim the 24-pair cell used t(0.975,23)=2.069, but analyze2-p5.py:28-31,42's
  T975 table stops at df=20; df=23 falls through to the `.get(n-1, 2.131)`
  default, so the archived CI95 [−20.522,+11.188] actually used 2.131.
  Recomputed with the correct t=2.069: [−20.060,+10.727] — still crosses 0,
  and |median| 11.688 < MDE′24=15.39 holds under both t values; INCONCLUSIVE
  unchanged. eofmix/cursor use df=15 (table value 2.131, correct) —
  unaffected.
- **F3 (evidence completeness, claim re-verified and holds)**: SUMMARY §5
  claims write_at and file_info disassembly identity, but perf-p2c1/ archived
  only the read_at/read/read_exact/read_exact_at machid pairs. The auditor
  re-extracted objects from both fresh archives and re-verified: read_at
  126=126, write_at 126=126, read 115=115, write 115=115, file_info 114=114
  instruction bodies byte-identical — the five-primitive identity claim is
  CONFIRMED, but at archive time that sub-claim was TRANSCRIBED without raw
  files behind it.
- **F4 (enumeration incomplete / scope statement, consistent with the mission
  but must be explicit)**: SUMMARY §5 lists the changed surface as
  read_exact_at (240→174) and read_exact (208→167), omitting the re-verified
  write_all_at (240→211) and write_all (208→194). No write-side composition
  wall cell was measured this session (mission §7.5 requires only three read
  shapes + negative control — compliant), but the P2C1 performance verdict
  scope is strictly the three read shapes; the eofmix/cursor WIN_CONFIRMED
  must not be generalized to write compositions. Also §5's "composition
  function −27.5% instructions" refers only to read_exact_at — wording can be
  misread as all composition functions.
- **F5 (minor, non-rearchivable transcriptions)**: SUMMARY §5's "ax
  uninstrumented ≈ 950 ns/op vs 1268 under perf" and §2's "loadavg 0.17 at
  end" have no raw record in the archived files (pmu-ax-diagnostic.txt holds
  only PMU counters; ENV-P2C1.txt only the start loadavg 0.23). The ≈950 side
  is consistent with the A/B baseline-side medians 936–957; both numbers are
  [UNCERTAIN]/[TRANSCRIBED] in nature but were not labeled as such in the
  files.
- **F6 (low, observation)**: A/A coverage asymmetric — the read-4k shape has
  both cand×cand and base×base A/A; eofmix/cursor/neg have cand-side A/A
  only. The machinery (paired2.sh + analyzer) is binary-independent and
  disclosed; read-4k is null on both sides — acceptable, recorded.

## Independently re-checked (executor: R9 auditor)

- **E5 Host Identity**: `hostname`=E5, `uname -r`=7.2.11-200.fc44.x86_64,
  /proc/cpuinfo model name=Intel Xeon E5-2666 v3 @ 2.90GHz — three-way match
  with ENV-P2C1.txt (perf_event_paranoid=2, schedutil, loadavg 0.23) and
  `E5_CLOSURE_SNAPSHOT.md` §1.
- **Baseline/Candidate SHA**: base worktree rev-parse = `77e5399a…` (= task
  B1/PR602 anchor); head worktree = `70d48f5e…`; merge-base = base (genuinely
  stacked); `git log --oneline 77e5399a..70d48f5e` = exactly the 4 claimed
  commits; `git show --stat 70d48f5e` docs-only (1 ledger line), so R4's
  binary evidence at `b12fb551` covers the measured head; both worktrees
  porcelain-clean.
- **Remote identity chain**: `git ls-remote origin` = sluice-600-direct-c1@
  `77e5399a` (unmoved), sluice-608-p2c1-read-compose@`70d48f5e`; `gh pr view`
  612=OPEN/ready/base=sluice-600-direct-c1/head=`70d48f5e`, 602=OPEN/head=
  `77e5399a`, dududu PR 7=OPEN. Zero writes, zero merges that round.
- **Library Hash**: re-hashed both libsluice_core.a = `e65a252f…`(base) /
  `a536bc6a…`(head), ladder-base=`89084b1d…`, ladder-cand=`3af44bf9…`,
  ladder.cpp=`fa26ac36…` — match ENV-P2C1.txt/SUMMARY §1; frozen B1 archive
  re-hash = `d1e351a5…` matches.
- **Base-side machine continuity**: extracted blocking_file.cpp.o from the
  fresh base archive and the frozen B1 archive; `objdump -d | strip addresses
  | sort | uniq -c | sha256sum` identical on both sides (`7dd8faeb…`) — the
  normalized-disassembly-multiset claim independently reproduced.
- **Nine public primitives machine identity** (objdump -d
  --disassemble=<mangled> per symbol from both archives):
  read_at/write_at/read/write/file_info all IDENTICAL; read_exact_at 240→174,
  write_all_at 240→211, read_exact 208→167, write_all 208→194 all DIFF — the
  changed surface is exactly the composition primitives.
- **A/A independent recomputation** (auditor-written order-aware recompute
  script aligned with paired2.sh + analyze2-p5.py): all 5 cells' sd_AA/median/
  CI/pos match SUMMARY §3 (35.601/+4.873; 28.275/+0.841; 19.519/+2.851;
  35.383/+4.463; 20.234/+1.606); MDE′ = 2.131·sd/√16 recomputed =
  18.97/15.06/10.40/18.85/10.78 — match.
- **A/B independent recomputation**: ab-read 24 pairs median=−11.688 /
  mean=−4.667 / sd=36.449 / pos=7/24 / AB/BA=−6.429/−15.828 reproduced; 16-pair
  view median=−11.486 CI=[−20.604,+18.218] reproduced; ab-eofmix median=
  −12.036 CI=[−26.389,−7.513] excludes 0; ab-cursor median=−20.173
  CI=[−35.197,−10.738] excludes 0; ab-neg median=−0.024 pos=8/16 CI crosses 0
  — all reproduced; MDE′24=2.069·36.449/√24=15.39 and |−11.688|<15.39 vs
  |−12.036|≥10.40, |−20.173|≥18.84 — all three cell adjudications follow the
  pre-declared rules.
- **Raw samples**: 9 raw files present, each with embedded `# cmd` line,
  runner/analyzer consistent with the #600 frozen kit; sample counts
  32/32/32/32/32 (AA) + 48/32/32/32 (AB) as claimed; per-cell checksum single
  values (seq=25165824, tail=12595200) verified by script; fixture
  /tmp/sluice600/data64m.bin sha256=`bc020fd1…2993a` matches the pinned #600
  identity.
- **ABBA pairing**: paired2.sh even pairs A-first, odd pairs B-first, statistic
  always ladderA−ladderB in emission order — verified by reading the script;
  consistent with analyze2-p5.py --order flip logic; the limitation that
  per-line binary attribution is defined only by emission order (both binaries
  print identical arm names) is explicitly disclosed in the script header
  (inherent to the design; not re-derivable from archived samples).
- **Negative control**: a2 wall PARITY recomputed; read_at disassembly
  byte-identical (126=126, diff=0) re-verified from archives; pmu-a2-
  diagnostic.txt three-round total instructions base [37263989, 37263801,
  37263885] vs cand [37263839, 37264091, 37263824], half-range/mean =
  ±0.00039% matching the claimed ±0.0004%; PMU ax Δ=−31.0 and ac Δ=−26.0
  instr/op recomputed — the win's negative-control gate is intact.
- **Wall/CPU/Instruction separation**: ladder.cpp:1360-1375 ns_per_op = timed
  loop best-of-5 wall (excludes warmup/verify); ladder.cpp:1717-1770 user/sys
  = whole-run rusage; warmup=1024 denominator (1+5+1)×1024=7168 recomputed;
  auditor's rusage cross-check shows user/sys splits dominated by accounting
  jitter (e.g. ab-neg wall parity while user +73 / sys −109), supporting the
  side-evidence-only discipline.
- **Actual syscall count**: evidence lives in `closure/r4-p2c1.md` §8.3 [RUN]
  (strace -f -e trace=pread64,pwrite64): full read_exact_at=1×pread64;
  31B/23B=2 syscalls (one per step); zero-length 0; closed 0; write_all_at
  1×pwrite64 — same shape as the R3 B1 baseline oracle; the candidate changes
  no syscall counts (perf-p2c1 session itself carried no syscall counter;
  cross-file reference acceptable since 70d48f5e differs from the evidence
  head only by docs).
- **Application attribution boundary**: both ladder binaries from the same
  ladder.cpp (hash-pinned) + unchanged public headers (git diff 77e5399a
  70d48f5e -- include/sluice/blocking/ empty; ladder.cpp includes public
  headers only) → identical harness codegen; the measured difference can only
  come from the linked library delta; tmpfs fixture, taskset -c 2, serial,
  timed window excludes warmup/verify — attribution boundary holds.
- **Regression status**: all four A/B cell medians ≤0; ax/ac PMU cycles also
  lower; a2 parity; five non-composed primitives machine-identical — no
  regression on the measured surface; write compositions changed machine code
  but were not wall-re-verified this session (not required by the mission) —
  recorded as out-of-scope-unclaimed.
- **R5 spot-check**: 30 raw aa-w1b run files independently recomputed:
  mean=35.887 / median=35.641 / sd=1.602 / CV=4.46% / min–max 33.0–39.0 /
  adjacent-pair spread mean 4.90% max 13.86% — bit-matches
  02-duckdb-oracle-aa.md (29 pairs); pair counts 29/29/15=73 confirmed;
  04-analysis.md's every performance conclusion names workload and host; the
  "no Direct-I/O opportunity" claim is explicitly scoped "on this host, for
  this workload" — no un-scoped generalization found.
