# IO Work-Reduction Ledger (P3, MISSION §5.2)

- One §5.2 accounting table per mechanism, **both Baseline and Candidate
  sides measured or attributed** (one-sided reporting forbidden —
  `P3_MECHANISM_STUDY_PLAN.md` §6). Row calibers are the plan's pinned ones:
  syscalls = `strace -c` integral/window counts (≥3 exact repeats); bytes =
  probe-internal deterministic counters; user-space copies = structural
  certification + PMU corroboration; kernel work = **attribution only**
  (syscall identity × counts + rusage split + PMU deltas; no kernel tracing
  in P3 scope — never a measured byte count); CPU = rusage user+sys (±30 ns
  tick class) with PMU deltas as the cross-arm signal; wall = uninstrumented
  16-pair ABBA medians; correctness = the cell's oracle.
- All numbers `[RUN]` in the P3 execution session of 2026-10-28 on host E5
  (kit HEAD `4455d98d…`, ladder sha256 `5a53a9ad…`, `p3-raw/env-pin.txt`);
  per-mechanism narrative, falsified predictions and verdicts in
  `STRUCTURAL_IO_MECHANISM_STUDY.md`; exchange-rate constants from M4 in §9.
  Post-session independent review: several single-arm values were
  mis-transcribed from the raw archives (wrong field, smoke-gate request
  set, single-sample ratios); all have been re-aligned to the archived
  analysis blocks — every paired Δ and CI re-verified exact (correction
  record: study §5, item 8).
- Sign convention: Δwall = Baseline − Candidate (positive = candidate
  faster). MDE′ per track (fresh A/A, plan §7.2): vectored 32.2 ns, planner
  warm 42.2 ns, planner cold 16 544 ns, copy 122.9 ns.
- Host-quiescence caveat (disclosed, unresolved): `p3-raw/env-pin.txt:10`
  records `concurrent_measure_procs=3` at pin time and `:18` records
  `measure_proc_check: 1 matching measurement processes (expect 0)` —
  observations conflicting with the plan §7.3 quiescent-host condition that
  were neither explained nor adjudicated in-session (see study header).
- Unmeasured or unmeasurable fields carry `NOT_RUN_WITH_REASON` /
  `NOT_SUPPORTABLE_WITH_REASON` — never blank, never extrapolated.
- Tail-latency row, all mechanisms: **`NOT_SUPPORTABLE_WITH_REASON`** — kit
  arms record best-of-reps `ns_per_op` only, no per-op samples (P0 §4.3
  precedent; plan §6 row); expected effects are ns–µs scale against a ≈26 ns
  per-op sampling overhead.

## 1. M1-VR — Case-A vectored read (k×pread → 1×preadv), 4 KiB / k=4 warm tmpfs

Cell `p3-m1-vectored-4k-k4`; raw `p3-raw/m1/p3-m1-vectored-4k-k4.txt`,
`-syscalls.txt`, `-pmu.txt`.

| Item | Baseline (av0: 4 × `pread64`) | Candidate (av1: 1 × `preadv`) |
|---|---|---|
| Logical requests | 1 op-group (request shape pinned by arm definition; equality group {av0,av1,avs} single-valued) | identical (same digest `0xd5e13fde91da8f83`, `fill_len_hash` equal) |
| Physical syscalls | 4.000 `pread64`/op-group | 1.000 `preadv`/op-group |
| Bytes requested | 4096 B | 4096 B |
| Bytes transferred | 4096 B | 4096 B |
| Extra bytes / over-read | 0 | 0 |
| User-space copies | 0 (kernel fills caller buffers directly) | 0 — certified structural (caller buffers are the iov entries; no staging shape exists), PMU non-contradicting (Δinstr −345/group) |
| Kernel work (attributed) | 4 entries + per-entry fixed paths | 1 entry + iov iteration; rusage sys split 1372→~296-997 ns band; split is attribution, not measurement |
| CPU/op (rusage u+s median) | 4097 ns/op-group | 2510 ns/op-group |
| Wall/op | 2451.6 ns | 876.7 ns |
| Δwall | — | **+1576.0 ± 21.7 ns**, CI ≠ 0, 16/16, ≥ MDE′ 32.2 |
| Tail latency | NOT_SUPPORTABLE_WITH_REASON (kit caliber) | NOT_SUPPORTABLE_WITH_REASON |
| Correctness | content_hash / fill_len_hash / checksum green | identical digests — green |

Scaling rows `[RUN]`: k=2 Δwall +511.3 ± 12.4 (2.000 → 1.000 syscalls);
k=8 +3613.6 ± 38.3 (8.000 → 1.000); 64 KiB/k=4 +1389.2 ± 76.9 (4.000 →
1.000; bytes 65 536/65 536/0). EOF oracle cell (`p3-m1-eof-boundary-4k.txt`):
per-buffer prefix lengths and effective-prefix checksums exactly equal
(descriptive, Δwall +1469.7 ± 18.7).

## 2. M1-VW — Case-A vectored write (k×pwrite → 1×pwritev), 4 KiB / k=4 warm tmpfs

Cell `p3-m1-wvectored-4k-k4` (Amendment-1 per-cell scratch); raw
`p3-raw/m1/p3-m1-wvectored-4k-k4.txt`, `-syscalls.txt`, `-pmu.txt`.

| Item | Baseline (wv0: 4 × `pwrite64`) | Candidate (wv1: 1 × `pwritev`) |
|---|---|---|
| Logical requests | 1 op-group | identical |
| Physical syscalls | 4.000 `pwrite64`/op-group | 1.000 `pwritev`/op-group |
| Bytes requested / transferred / extra | 4096 / 4096 / 0 | 4096 / 4096 / 0 |
| User-space copies | 0 (gather direct from caller buffers) | 0 — structural certification + PMU (Δinstr −207/group) |
| Kernel work (attributed) | 4 entries + fixed paths | 1 entry + iov iteration (attribution) |
| CPU/op | 2673 ns/op-group | 952 ns/op-group |
| Wall/op | 2578.2 ns | 877.7 ns |
| Δwall | — | **+1699.3 ± 9.3 ns**, CI ≠ 0, 16/16 |
| Tail latency | NOT_SUPPORTABLE_WITH_REASON | NOT_SUPPORTABLE_WITH_REASON |
| Correctness | `content_ok=1`, scratch content digest green | identical |

64 KiB write twin: `NOT_RUN_WITH_REASON` — not in the preregistered cell set
(OQ-M1-8; absence is a scope statement, not a negative result).

## 3. M1-GAP — today's only legal Sluice composition vs raw (informational parity row)

Cells `p3-m1-sluice-gap-4k-k4` / `p3-m1-wsluice-gap-4k-k4`; raw
`p3-raw/m1/p3-m1-sluice-gap-4k-k4.txt` (+`-pmu.txt` auxiliary a0/a1/a2),
`p3-m1-wsluice-gap-4k-k4.txt`.

| Item | Baseline (raw k×pread / k×pwrite) | Candidate (Sluice k×read_at / k×write_at) |
|---|---|---|
| Logical requests | 1 op-group | identical |
| Physical syscalls | 4.000 / 4.000 per op-group | 4.000 / 4.000 (unchanged — no mechanism) |
| Bytes req / xfer / extra | 4096 / 4096 / 0 | 4096 / 4096 / 0 |
| User-space copies | 0 | 0 |
| CPU/op | read 4035 ns | read 4029 ns class (sub-tick) |
| Wall/op | read 2417.6 / write 2578.8 | read 2429.5 / write 2594.7 |
| Δwall | read +5.7 ns; write −15.4 ns — both sub-MDE → **NO_CLAIM** | |
| PMU Δinstr/op-group | — | read +263 (above the [+120,+200] band; attributed: av0 is the bare N0 arm, auxiliary a1−a0=+52.0 / a2−a1=+44.0 per op); write +154 (above its own [+55,+100] band — open discrepancy, same caliber caveat, no auxiliary run) |
| Tail latency | NOT_SUPPORTABLE_WITH_REASON | NOT_SUPPORTABLE_WITH_REASON |
| Correctness | digests green | identical |

No product candidate; the row exists to pin that the vectored win is not
reachable by wrapper work (see study §5.2).

## 4. M2-MRG — threshold merge at T (rpl0 → rplt), trace g4k warm tmpfs

Cells `p3-m2-merge-sweep-g4k-{512,1024,2048,4096,8192,16384}`; raw
`p3-raw/m2/p3-m2-merge-sweep-g4k-*.txt` (+`-syscalls.txt`, `-pmu.txt` per T).
Primary row = T\* = 4 KiB (the only CI-excluding win).

| Item | Baseline (rpl0: 8 × `pread64`) | Candidate (rplt, T=4096) |
|---|---|---|
| Logical requests | 8 requests/op-group, req_digest `0x721f35c2d480f218` (identical every cell/repeat) | identical digest |
| Physical syscalls | 8.000 `pread64`/op-group | 4.548/op-group (strace window; kit merged-group counter 4 per realized group) |
| Bytes requested | 32 768 B | 32 768 B |
| Bytes transferred (issued) | 32 768 B | 39 829 B |
| Extra bytes / over-read | 0 | **+7061 B (1.215×)** — gap bytes covered by merged extents (timed cell's own `issued_per_group`; the kit smoke gate's ops=64 request set must not be quoted here — earlier drafts did and are corrected) |
| User-space copies | 0 (kernel→caller direct) | +1 userspace memcpy per covered request of its requested length + one kernel→staging transfer per merged extent (structural certification, PRED-M2-A shape) |
| Kernel work (attributed) | 8 entries | 4.548 entries + amplified bytes at 0.117 ns/B class (attribution) |
| CPU/op | 22 818 ns/op-group | 22 488 ns/op-group |
| Wall/op | 8599.9 ns | 8281.8 ns |
| Δwall | — | **+327.2 ± 35.2 ns**, CI ≠ 0, 16/16, ≥ MDE′ 42.2 ⇒ T\* = 4 KiB |
| Tail latency | NOT_SUPPORTABLE_WITH_REASON | NOT_SUPPORTABLE_WITH_REASON |
| Correctness | per-request digests green | req/ref/outcome digests green in every cell (PRED-M2-6) |

Full sweep Δwall (rpl0−rplt) `[RUN]`: T=512 −329.9 ± 36.4 (7.601 syscalls,
+103 B); T=1K −156.7 ± 37.9 (7.156, +444 B); T=2K +151.6 ± 182.8 CI∋0 →
NO_CLAIM (6.257, +1821 B); T=8K **−722.5 ± 49.6** (1.000, +28 828 B =
1.880×); T=16K **−752.0 ± 42.5** (1.000, +28 828 B). PRED-M2-1 wall clause
falsified; the two count clauses hold.

Cold btrfs row: `NOT_RUN_WITH_REASON`-equivalent at claim level — cells were
run (`p3-raw/m2/p3-m2-merge-cold-g4k-*.txt`) but every Δ (T=0: +1072 ±
10 707; 256: −329 ± 15 015; 512: +330 ± 9227; 1K: −561 ± 10 102; 4K: +762 ±
6711) is below the cold MDE′ 16 544 ns → `NO_CLAIM` at all T; PRED-M2-3
`NOT_RESOLVED`. Count caliber cold-invariant (8.000 → 4.548 @ 4K, extra
1.215×).

**PRED-M2-5 trigger evaluation (verdict-bearing, study §2.3 for the full
calculation):** dispatch overhead at equal counts −495.2 ± 41.7 ns (CI ≠ 0,
§6) vs the winning-T syscall saving — 151% of the net +327.2 ns wall
saving, 60% of the additive-gross 822.4 ns; both evidence-internal readings
exceed the preregistered 50% threshold ⇒ the threshold-merge mechanism
verdict is degraded to `COST_MIGRATION_ONLY` (plan §8, mandatory, recorded
despite the T\* wall win). The < 50% reading requires pricing the 3.452
saved entries at the preregistration grounding constant (≈550–700 ns/entry),
which the cells refute (measured gross ≈ 238 ns per saved entry); it is
recorded here because the earlier published "≈23%, not reached" figure
relied on it.

## 5. M2-DDP — intra-set dedup (rpl0 → rpld), trace ov30 warm tmpfs

Cell `p3-m2-dedup-ov30`; raw `p3-raw/m2/p3-m2-dedup-ov30.txt`,
`-syscalls.txt`, `-pmu.txt`.

| Item | Baseline (rpl0) | Candidate (rpld, dedup=1 T=0) |
|---|---|---|
| Logical requests | 8 requests/op-group, req_digest `0x791ce099c10536ad` | identical digest |
| Physical syscalls | 8.000 `pread64`/op-group | 3.999/op-group (strace window; kit disjoint-union-component counter 3 ≤ k) |
| Bytes requested | 32 768 B | 32 768 B |
| Bytes transferred (issued) | 32 768 B | 21 507 B |
| Extra bytes | 0 | **−11 261 B (−34.4%)** — union never over-reads (issued = requested − duplicated) |
| User-space copies | 0 | +1 memcpy per covered request (dispatch slicing; structural) |
| Kernel work (attributed) | 8 entries, 32 768 B | 4 entries, 21 507 B — genuinely fewer attributed transfers |
| CPU/op | 21 509 ns/op-group | 19 584 ns/op-group |
| Wall/op | 7078.4 ns | 4925.3 ns |
| Δwall | — | **+2146.6 ± 53.0 ns**, CI ≠ 0, 16/16 |
| Tail latency | NOT_SUPPORTABLE_WITH_REASON | NOT_SUPPORTABLE_WITH_REASON |
| Correctness | per-request digests green | all per-request digest gates green |

EOF-attribution oracle (eofmix @ T=4K, `p3-m2-eof-attribution.txt`):
per-request outcome/ref/req digests triple-equal; 8.000 → 1.000 syscalls;
wall 4311.1 → 1208.5 (descriptive). Concurrent-writer divergence
(`p3-m2-concurrent-divergence.txt`, descriptive, no CI): under equal file
state both forms byte-identical (each matches its own reference); merged arm
sampled 7503 B/op-group of unrequested bytes invisible to the caller —
contract question for AC-2.

## 6. M2-PLN — planner own userspace cost at equal syscall counts (T=0)

Cell `p3-m2-planner-overhead-g4k`; raw `p3-raw/m2/p3-m2-planner-overhead-g4k.txt`,
`-pmu.txt`.

| Item | Baseline (rpl0) | Candidate-side measurement (rplt, T=0: sort+sweep + staging dispatch, adjacent-only) |
|---|---|---|
| Logical requests | 8/op-group, digest pinned | identical |
| Physical syscalls | 8.000 | 8.000 (7 kit-counter adjacent merges that still stage+slice; equal-count condition) |
| Bytes req / issued / extra | 32 768 / 32 768 / 0 | 32 768 / 32 768 / 0 |
| User-space copies | 0 | full requested bytes staged + slice-dispatched (structural) |
| CPU/op | 22 761 ns | 23 179 ns |
| Wall/op | 8556.2 ns | 9059.9 ns |
| Δwall | — | **−495.2 ± 41.7 ns** (CI ≠ 0, 0/16); Δinstr +8053/group (rplt 1 162 684 from this cell's `-pmu.txt`, three identical runs ÷1024 ops; rpl0 1 154 631 is the T=4K cell's rpl0 PMU arm — same arm/trace/digest, cross-cell reuse disclosed; this cell's own `-pmu.txt` archived only the rplt arm) |
| Tail latency | NOT_SUPPORTABLE_WITH_REASON | NOT_SUPPORTABLE_WITH_REASON |
| Correctness | digests green | digests green |

PRED-M2-5's ≤ 100 ns bound falsified (≈5×); the same measurement **fires the
preregistered `COST_MIGRATION_ONLY` trigger** — dispatch overhead 495.2 ns =
151% of the winning-T net saving (+327.2 ns) and 60% of the additive-gross
saving (822.4 ns), both ≥ the 50% threshold (full calculation and the
rejected model-constant reading: study §2.3; the earlier "≈23%, not reached"
figure priced the 3.452 saved entries at a grounding constant the cells
refute). Consequence: the M2-MRG threshold-merge verdict degrades to
`COST_MIGRATION_ONLY` (§13). The
bookkeeping-vs-dispatch decomposition arm (M2-OQ1) was
`NOT_RUN_WITH_REASON` — requires a plan amendment (direct-fill or plan-only
arm), not exercised.

## 7. M3-CFR-T — `copy_file_range` vs user loop, tmpfs→tmpfs, 64 KiB × 1024 chunks

Cell `p3-m3-copy-tmpfs-cfr`; raw `p3-raw/m3/p3-m3-copy-tmpfs-cfr.txt`,
`-syscalls.txt`, `-pmu.txt`.

| Item | Baseline (cp1: `pread`+`pwrite` loop) | Candidate (cp2: cfr loop, explicit offsets) |
|---|---|---|
| Logical requests | 1 chunk/call, 1024 chunks, full-file digest pinned | identical |
| Physical syscalls | 2.000/chunk kit counter (`pread64`+`pwrite64`); strace window 2.125/chunk (pwrite64 1.000 exact + pread64 1.125 verify-read residual; three repeats bit-identical 1154/578) | 1.000 `cfr`/chunk exact in both calibers |
| Bytes requested | 65 536 B/chunk | 65 536 B/chunk |
| Bytes transferred (ABI crossings) | 131 072 B/chunk (read + write) | 65 536 B/chunk |
| Extra bytes | 0 | 0 |
| User-space copies | 2 traversals/byte (kernel→user staging, user→kernel) | **0** — no user buffer exists in the call shape (structural certification) |
| Kernel work (attributed) | 2 entries + 2 ABI crossings/byte | 1 entry, in-kernel move (attribution only; never a measured kernel byte count — P-M3-9) |
| CPU/chunk (rusage u+s) | 21 579 ns | 20 092 ns (**−6.9%** — misses P-M3-1's −30%, prediction falsified and recorded) |
| Wall/chunk | 19 676.0 ns | 17 626.1 ns |
| Δwall | — | **+2054.2 ± 66.4 ns (+10.4%)**, CI ≠ 0, ≥ MDE′ 122.9 (P-M3-2's ≥10% holds) |
| PMU instr/chunk | 994 899 | 994 833 (Δ −66 — ≥2× instr/byte target `NOT_RUN_WITH_REASON` at this caliber: whole-process PMU cannot resolve it; the copy-elimination row is carried by structural certification) |
| Tail latency | NOT_SUPPORTABLE_WITH_REASON | NOT_SUPPORTABLE_WITH_REASON |
| Correctness | full-file content equality group {cp1,cp1s,cp2,cp3} + source, `content_ok=1` | identical |

## 8. M3-CFR-B — `copy_file_range` vs user loop, btrfs→btrfs (same fs)

Cell `p3-m3-copy-btrfs-cfr`; raw `p3-raw/m3/p3-m3-copy-btrfs-cfr.txt` +
diagnostics.

| Item | Baseline (cp1) | Candidate (cp2) |
|---|---|---|
| Logical requests | identical | identical |
| Physical syscalls | 2.000/chunk kit counter (strace window 2.125, identical repeats) | 1.000/chunk |
| Bytes req / transferred / extra | 65 536 / 131 072 / 0 | 65 536 / 65 536 / 0 |
| User-space copies | 2/byte | 0 (structural) |
| Kernel work (attributed) | 2 entries + 2 crossings/byte | 1 entry; reflink-class acceleration possibly engaged (man NOTES) — attribution caveat, magnitude not extrapolable |
| CPU/chunk | 16 815 ns | 7581 ns (**−54.9%**, ≥ −20% holds) |
| Wall/chunk | 15 343.2 ns | 5773.4 ns |
| Δwall | — | **+9508.3 ± 114.5 ns (+62.0%)** — exceeds P-M3-4's band upper [+−5%, +30%], recorded as-is |
| Tail latency | NOT_SUPPORTABLE_WITH_REASON | NOT_SUPPORTABLE_WITH_REASON |
| Correctness | content equality green | identical |

## 9. M3-SFL — `sendfile` comparator rows

Raw `p3-raw/m3/p3-m3-copy-tmpfs-sendfile.txt`, `p3-m3-copy-crossfs-sendfile-{b2t,t2b}.txt`,
`p3-m3-copy-btrfs-sendfile.txt`. sendfile is a comparator, not a candidate
engine (always advances the out_fd shared cursor — session-measured 1024→
5120; SEM-04 conflict, study §3.1).

| Pairing | Baseline wall | cp3 wall | Δwall | Disposition |
|---|---|---|---|---|
| tmpfs same-fs | 19 756.8 | 17 634.0 (1.000 sendfile/chunk) | **+2123.1 ± 102.0** | CI ≠ 0; \|cp3−cp2\| = 7.9 ns (medians 17 634.0 vs 17 626.1) < 2×MDE′ (same splice class, P-M3-5) |
| cross-fs b2t | 17 098.0 | 14 902.5 | **+2202.6 ± 51.7** | CI ≠ 0 |
| cross-fs t2b | — | — | −163.2 ± 208.6 | CI includes 0 → NO_CLAIM |
| btrfs same-fs | 15 415.4 | 14 972.5 | +765.7 median (mean +493.9, mean CI [75, 913]) | marginal mean CI + paired-delta sd 786 ns (arm sds 1099.9 / 932.0; cfr cell 215); \|cp3−cp2\| = 9199 ns (medians 14 972.5 vs 5773.4) ≫ 2×MDE′ → attributed (out-cursor mechanism + variance), **no claim** |

Correctness: content equality group green in every run cell. Tail latency:
NOT_SUPPORTABLE_WITH_REASON. cfr cross-fs: `NOT_RUN_WITH_REASON` — EXDEV
(ret=−1 errno 18, support matrix, both directions).

## 10. M3-GAP — today's legal Sluice copy composition vs raw loop (informational parity row)

Cells `p3-m3-copy-{tmpfs,btrfs}-sluicegap`; raw
`p3-raw/m3/p3-m3-copy-tmpfs-sluicegap.txt`, `-btrfs-`.

| Item | Baseline (cp1 raw loop) | Candidate (cp1s Sluice read_at+write_at loop) |
|---|---|---|
| Logical requests | 1024 × 64 KiB chunks | identical |
| Physical syscalls | 2.000/chunk kit counter (strace window 2.125, identical repeats) | 2.000/chunk kit counter (admission is userspace; strace window 2.125, identical repeats) |
| Bytes req / transferred / extra | 65 536 / 131 072 / 0 per chunk | identical |
| User-space copies | 2/byte | 2/byte |
| CPU/chunk | tmpfs 21 670 ns | tmpfs 21 653 ns |
| Wall/chunk | tmpfs 19 777.6 / btrfs 15 275.8 | tmpfs 19 644.7 / btrfs 15 232.6 |
| Δwall | tmpfs −36.6 ns; btrfs −103.1 ns — both sub-MDE′ → **NO_CLAIM** | |
| PMU Δinstr/chunk | — | +85 (cp1s 994 984 from this cell's `-pmu.txt`; cp1 994 899 from the cfr cell's PMU arm, cross-cell reuse disclosed; inside P-M3-6's [−50%, +150%] band around ≈+57 wrapper class) |
| Tail latency | NOT_SUPPORTABLE_WITH_REASON | NOT_SUPPORTABLE_WITH_REASON |
| Correctness | group green, source `content_ok=1` | identical |

Short-at-EOF descriptive cell (`p3-raw/m3/p3-m3-short-error-cells.txt`): all
four arms publish exactly 2 097 152 confirmed bytes = source length;
strictly monotonic offset advance, no replay of confirmed ranges. Shape
deviation disclosed: executed as 64 KiB chunk × ops=40 against a 2 MiB
source (source-EOF construction) — not the preregistered "4 KiB/64 KiB vs
64 MiB fixture" evidence shape; no numbered plan amendment was recorded
(study §3.3, plan amendment pending Owner action).

## 11. M3 error-injection and boundary rows

- M3 concurrent-modification divergence cell (plan §4.2 disclosure 4):
  `NOT_RUN_WITH_REASON` — no archived cell exists (M2's divergence record is
  not M3 evidence); the descriptive disclosure was neither run nor
  registered at execution time, and is now recorded here and in study §3.3.
  The candidate's concurrent-modification disclosure obligation (AC-3 §4)
  is therefore **not** evidence-backed.
- cfr cross-fs (both directions): `NOT_RUN_WITH_REASON` — EXDEV pinned by
  the support matrix (`p3-raw/m3/support-matrix.txt`: ret=−1 errno=18); no
  substitution.
- ENOSPC/EFBIG injection sub-cells: `NOT_RUN_WITH_REASON` — the session's
  sudo grant (perf/PMU/strace + planned drop_caches) cannot construct a
  size-constrained destination filesystem (loop-mount/quota require mount
  privileges); EXDEV/EINVAL/EBADF classes are pinned by the support matrix;
  EFBIG/ENOSPC/EOPNOTSUPP remain man-page-based `[TRANSCRIBED]` until
  injected.
- NUMA axis: `NOT_RUN_WITH_REASON` — single-node host (`lscpu`,
  `p3-raw/env-pin.txt:7`).
- `O_DIRECT` interplay cells: `NOT_RUN_WITH_REASON` — excluded by plan §5.1
  as a non-equivalent access mode (D-P3-02).

## 12. M4 — environmental constants (no §5.2 mechanism row)

M4 proposes no mechanism; no Baseline/Candidate pair exists by design
(`p3-families/M4_ANALYSIS.md` §1.1, study §4). Constants consumed by the
rows above `[RUN]` (`p3-raw/m4/`): warm-tmpfs marginal 0.117 ns/B (4k→64k)
and 0.154 ns/B (64k→1m); warm-btrfs 0.081 / 0.123; cold/warm btrfs ratio
13.6× @4 KiB, 27.9× @64 KiB (a1 wall medians; the previously published
23.4×/29.3× were single-sample, dimension-mixed and are retracted — study
§4.3); working-set axis 788.2 / 965.1 / 929.0 ns/op
(8/64/256 MiB; a1 wall medians — the previously published 420.1/677.6/635.8
were single-sample `sys_ns_per_op` mis-parses and are retracted — study
§4.3); exchange rate R_warm ≈ 4.3–4.9 KiB vs R_cold ≈ 0.46 KiB per
saved syscall. NUMA: `NOT_RUN_WITH_REASON` (1 node). Write-side environment
axes: `NOT_RUN_WITH_REASON` — not M4 cells by plan §5.2 (OQ-9); any
write-side R needs an amendment. Syscall-count axis archived for the 4 KiB
tmpfs cell (`p3-m4-size-sweep-4k-tmpfs-syscalls.txt`); remaining axes share
the same single-span arm shape and are not separately archived.

## 13. Verdict column (summary)

| Mechanism | Verdict |
|---|---|
| M1-VR / M1-VW (Case A vectored) | `MECHANISM_WIN_EARNED` (probe) + `REQUIRES_API_DECISION`; family capped `CONDITIONAL_WORKLOAD_WIN` |
| M1-GAP | `NO_CHANGE_JUSTIFIED` at wall caliber (sub-MDE; instruction-caliber wrapper constants confirmed, band miss attributed) |
| M2-MRG (threshold merge) | `COST_MIGRATION_ONLY` — preregistered PRED-M2-5 trigger fires at T\* = 4 KiB (dispatch overhead −495.2 ns = 151% of the net / 60% of the additive-gross winning-T syscall saving, both ≥ 50%; plan §8 mandatory, recorded despite the CI-excluding wall win +327.2 ns at T\*); PRED-M2-1 wall clause falsified at T ≥ 8 KiB; cold `NOT_RESOLVED` |
| M2-DDP (dedup) | `MECHANISM_WIN_EARNED` (probe) + `REQUIRES_API_DECISION`; capped `CONDITIONAL_WORKLOAD_WIN` |
| M2-PLN (planner overhead) | PRED-M2-5 falsified (+495.2 ns ≫ 100 ns); the same measurement fires the `COST_MIGRATION_ONLY` trigger degrading M2-MRG (see above) |
| M3-CFR-T (tmpfs) | `MECHANISM_WIN_EARNED` (probe: wall +10.4% CI ≠ 0 ≥ MDE′; syscalls 2.000 → 1.000 count caliber; user-space copies 2 → 0 structural; P-M3-3's CPU −30% prediction falsified at −6.9% — recorded as a falsified prediction, not a verdict criterion) + `REQUIRES_API_DECISION` |
| M3-CFR-B (btrfs) | `MECHANISM_WIN_EARNED` (probe; reflink caveat; out-of-band-upper wall recorded as-is); capped `CONDITIONAL_WORKLOAD_WIN` |
| M3-SFL (sendfile) | comparator only; per-cell dispositions in §9 |
| M3-GAP | `NO_CHANGE_JUSTIFIED` at wall caliber (sub-MDE) |
| M4 | `NO_CHANGE_JUSTIFIED` — no mechanism proposed; constants delivered |

No row's benefit is claimed for any real workload (#578 R0 incomplete —
plan §9 cap). No syscall-count reduction is reported as a win without an
attributed CPU/wall/work counterpart on the same row.
