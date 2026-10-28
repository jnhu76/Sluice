# P4 Workload Competition Plan — Real Workloads (Preregistered)

- Mission: `research/direct-beyond-parity/MISSION.md` §6 (P4 — Real Workload
  Performance Competition; layering §6.1/P4.1, workloads §6.2/P4.2, statistics
  §6.3/P4.3, decision rows §6.4/P4.4).
- Preregistration date: 2026-10-28. Primary platform: E5 Native Linux.
- **Status of this document: PREREGISTERED.** Workload admissions (§1), the
  Layer A/B/C mappings (§2), every cell, arm identity, pairing count,
  statistic, caliber, correctness gate and verdict rule below are fixed
  **before** any P4 measurement runs. Mid-campaign deviations become numbered
  amendments with trigger/change/reason — never silent (precedents:
  `P1_MEASUREMENT_PLAN.md` Amendment 1; `P2_CANDIDATE_P2C1.md` §12; P3 plan
  Amendment log discipline).
- Inputs read for this plan: `MISSION.md` §6/§11;
  `P0_BASELINE_AND_OWNERSHIP.md` (§1 baselines/ownership, §3 #600 inherited
  results incl. R1 Parquet, §4 open questions, §5 oracle floor);
  `P1_MEASUREMENT_PLAN.md` (§3 environment pin, §4 cell roster, §5 statistics,
  §6 calibers, §7 cold discipline, §8 matrix commands, Amendment 1);
  `P2_CANDIDATE_P2C1.md` / `P2_CANDIDATE_P2C2.md` (KEEP identities, gates,
  caps, §12 closure); `P3_MECHANISM_STUDY_PLAN.md` (§7 statistics, §8 verdict
  vocabulary, §9 admission cap); `STRUCTURAL_IO_MECHANISM_STUDY.md` (verdicts,
  boundaries); `API_CAPABILITY_CANDIDATES.md` (AC-1/AC-2/AC-3); dududu R1
  protocol `benchmarks/parquet/protocol.md` + frozen receipts
  `build/parquet-results/`.

## 0. Candidate set (fixed)

| Candidate | Identity | Evidence status |
|---|---|---|
| P2C1 — read-composition prevalidated register-resident steps | branch `direct-read-compose-prevalidated-steps` (local-only, never pushed; verified 0 remote refs this session), HEAD `deaf03014173e13633a43f4db405c9a9e9860f23`, release archive sha256 `07d0680307c71cc412dfd021fd47c3098de57b580283df189f66dcdc2052049e` | KEEP (P2C1 §12.5): wall −16.565 ns/op @ read_exact_at 4 KiB seq, CI-excluding; claim capped "measurable improvement with re-attributed residual"; tail-mix subclaim restricted |
| P2C2 — write-composition exit staging | branch `direct-write-compose-exit-staging` (local-only; 0 remote refs), HEAD `476bd9ce9780594163350228e066a86b554c157d`, release archive sha256 `672510281abadba1cbda0a0e89f15e1dbb2c69f5a15bceb306b70196b420a807` | KEEP — instruction caliber only (P2C2 §9); no wall claim |
| BASELINE for all W01/PARQUET comparisons | frozen B1 archive `d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0` at `/home/jnhu/Source/Sluice-600-wt` (worktree HEAD `77e5399a`, clean) | P0 §1.3 anchor; re-hashed this session (§1.1) |

Rules fixed now:

1. **Single factor only.** Each cell compares B1 vs exactly one KEEP
   candidate. The P2C1+P2C2 combination is **forbidden in P4** — it is the P5
   interaction matrix's assignment (`P2_CANDIDATE_P2C1.md` §12.6;
   `P3_MECHANISM_STUDY_PLAN.md` D-P3-05). No A+B cell exists in this plan.
2. **Identity gate before any measurement:** every candidate archive's
   sha256 must equal the P2-recorded value (checked this session, §1.1). Hash
   equality carries the P2 four-configuration 58/58 matrix and mutant-gate
   evidence forward. If any hash differs at campaign time, that candidate is
   measured only after a full `p2-scripts/P2C1-matrix.sh`-equivalent matrix
   re-run (exact commands: P1 §8) exits 0.
3. **No new mechanism enters P4 unpreregistered.** P3 structural mechanisms
   participate only as PARQUET experimental arms (§4), filed as
   workload-opportunity evidence for AC-1/AC-2/AC-3 — never as Sluice product
   claims (`API_CAPABILITY_CANDIDATES.md` status: Awaiting Owner).

## 1. Asset inventory (read-only, executed 2026-10-28 before this plan)

Every check below was run in this session on E5; command and result are the
admission evidence. Legend as P0 §0: `[RUN]` executed this session.

### 1.1 W01 — direct-cost-ladder suite: ADMITTED

- Kit worktree `/home/jnhu/Source/Sluice-direct-cost-ladder`, branch
  `direct-cost-ladder` (local-only; `git branch -r --list origin/direct-cost-ladder`
  → 0 refs `[RUN]`), HEAD `4455d98d9823ec138c904be4c63a1d5a84903e97` `[RUN]`
  (`git log --oneline -1`) — identical to the P3 execution identity
  (`STRUCTURAL_IO_MECHANISM_STUDY.md` header). **P4's measurement binaries
  are built from the kit source commit `2364be4a` (the P2 kit source,
  extracted by scratch `git archive` — see Amendment 1), not from the HEAD
  tree**; P4 uses no P3 arm. Untracked build products only
  (`git status --porcelain` → `?? ladder*`, `?? m3-support`, `?? mkfixture`);
  no tracked modifications.
- Binaries present and hash-verified `[RUN]` (`sha256sum`), **all built from
  the one common kit source commit `2364be4a` (P2 kit source; Amendment 1)**:
  `ladder` = `8945036746d03389e6f44df7a6b26ae5599f2f1bddda8825da28ebd11dc010e8`
  (byte-exact reproduction of the P2 session's B1-linked binary — the same
  hash recorded in `p2-raw/kit/ladder-identity.txt` and every
  `p2-raw/p2c1/*` cell header), `ladder-c1` =
  `8305d22ca4ff74db0017a24fd7867b85f4a48ec767fbfc5d49563402ce870381`
  (= P2C1 §12.3 record), `ladder-c2` =
  `8bb6473b0f2455c2b092d94cc55ccaec20fe72ff40103bd9fb5c9151c7740065`.
  The P3-era `ladder` build (`5a53a9ad…`, kit source `4455d98d`) is preserved
  untracked as `ladder-4455d98d` and is NOT used by any P4 cross-binary cell
  (Amendment 1 records the asymmetry it caused).
- **Runnable, proven this session** `[RUN]`: warm fixture regenerated with the
  committed `./mkfixture /tmp/sluice600/data64m.bin 67108864` → sha256
  `bc020fd15a622e081793b0fb8ff2383838e27f6d1cb2135f204aad8383b2993a` (=
  pinned value; the tmpfs fixture had vanished on reboot — anticipated by P0
  §1.4 — and regenerates byte-identically, so fixture identity stays closed).
  Post-Amendment-1 smoke on the restored common-source set: `taskset -c 2
  ./ladder {a0,a1,a2,a1x,ax,a1c,ac} 4096 256 64 1 <fix>` and the same seven
  arms on `./ladder-c1` all exit 0 with identical checksum 1310720 on both
  binaries; `./ladder-c2 {w1,wx} …` on a fresh Amendment-1-style scratch →
  checksum 1310720 with `content_ok=1`. Cold btrfs fixtures persist:
  `/var/tmp/sluice600-p1/{cold64m,coldwrite64m}.bin` `[RUN]` (`ls`).
- Frozen B1 archive re-hashed `[RUN]`:
  `/home/jnhu/Source/Sluice-600-wt/build/linux/x86_64/release/libsluice_core.a`
  → `d1e351a5…3007f0` (matches P0 §1.3); worktree HEAD `77e5399a`, status
  clean. P2C1 worktree HEAD `deaf0301` + archive `07d06803…2049e` (matches
  P2C1 header); P2C2 worktree HEAD `476bd9ce` + archive `67251028…20a807`
  (matches P2C2 header) — all `[RUN]` (`git rev-parse`, `sha256sum`).
- Pairing harness present `[RUN]`: `paired.sh` (single-binary ABBA) and
  `paired2.sh` (two-binary ABBA: same arm selector on candidate vs baseline
  builds — the cross-binary P2 caliber; `analyze_pairs.py` for statistics,
  order-aware for two-binary series per `paired2.sh` header).
- **Verdict: W01 ADMITTED.** Suite, baseline archive, both candidate
  binaries, fixtures and harnesses all exist and were exercised this session.

### 1.2 PARQUET — frozen Reader Trace + three arms: ADMITTED

- Harness: dududu checkout `/home/jnhu/Source/dududu`, branch
  `r1-parquet-sluice-bench` (dududu PR #3 = MERGED `[RUN]`:
  `gh pr view 3 --repo jnhu76/dududu` → state MERGED 2026-10-21), working
  tree clean `[RUN]`. Protocol:
  `/home/jnhu/Source/dududu/benchmarks/parquet/protocol.md` defines the three
  arms — P0 arrow native (`arrow::io::ReadableFile`), P1 native wrapper
  (same `AdapterFile`, POSIX `pread`), P2 sluice direct (same adapter,
  `sluice::blocking::read_at`; TU contains no `pread` symbol).
- Frozen Reader Trace `[RUN]` (`sha256sum`):
  `build/parquet-results/trace-medium-t3.jsonl` =
  `a059e58fbc8771d78b23cd74afc3e1e7d26a09e230165b474dfc5b5cf1b53724` and
  `trace-medium-t3-p2.jsonl` =
  `4c67562626853ec75a9d48cb9626f283342db07c968bb8b7d893dd08a966950d` — both
  equal the frozen `run-manifest.txt` TRACE lines. Dataset
  `build/parquet-data/medium-none.parquet` re-hashed `[RUN]` →
  `6116e69cf20a89b7ca8a23e4d4dbd7d3759225a68e379d4776b3727bf3c05af6`
  (181,966,794 B) — **this closes the P0 §3.5 `[UNCERTAIN at the file
  level]` dataset-identity gap**: the file on disk matches the trace's
  self-declared digest.
- Frozen R1 receipts (citable, immutable):
  `build/parquet-results/{summary.txt,measure-pass.json,warmup-pass.json,run-manifest.txt}`.
  Run manifest pins `DUDUDU_SHA=ed053cb…`, `SLUICE_SHA=8fdcd99c…`
  (noliburing; the staged `libsluice_core.a` sha256 `489104b5…c5a` equals the
  B0 frozen library hash, P0 §1.2 — i.e. R1's P2 arm was measured at the
  B0-era library), kernel `7.2.9-201.fc44.x86_64` (frozen
  `run-manifest.txt:9`; host now 7.2.11 — drift
  disclosed in §6; no cross-kernel comparability claim).
- Tools runnable today `[RUN]`: the three binaries re-hash to the manifest
  (`parquet_bench e24a6f5b…`, `parquet_trace 408a97b1…`,
  `parquet_fixture b4750ec7…`), and
  `taskset -c 2 ./build/cmake-parquet/experiments/parquet/parquet_bench
  --medium=build/parquet-data/medium-none.parquet
  --trace=build/parquet-results/trace-medium-t3.jsonl
  --benchmark_filter='Replay/.*' --benchmark_repetitions=1` ran clean:
  `Replay/native/medium` 234.6 ms, `Replay/sluice/medium` 232.7 ms,
  `replay_bytes=181.996M replay_requests=57` — inside the frozen R1 band
  (native 234.4 / sluice 234.8 ms). The three-arm T1/T2/T3 benchmarks are
  registered in the same binary (bench_main.cpp).
- **Trace structure, measured this session** `[RUN]` (python over the frozen
  trace; recomputed and byte-coverage-verified at Amendment 1): 57 requests,
  sizes 33,277–16,030,016 B, median 1,390,820 B, total requested
  181,996,415 B; sorted extents (adjacent-touching merged) form **1
  contiguous segment [4, 181966794)** with **exactly one 4-byte hole:
  bytes [0, 4) are never requested**, and **29,625 B of overlap (0.016%)**
  (union 181,966,790 + overlap 29,625 = 181,996,415 ✓); no exact duplicate
  (offset,size) pairs; back-to-back adjacency is the dominant structure
  (e.g. [4,1550847) then [1550847,3057951)). An earlier draft of this
  section claimed "1 contiguous union (zero gaps)" — the zero-gap clause was
  false (the [0,4) hole existed in the draft's own union arithmetic);
  corrected by Amendment 1 with a direct point-coverage query (bytes
  1550843–1550846 are covered by request [4,1550847); a suspected second
  hole there was disproved). Consequences preregistered in §4.3
  (dedup/merge/vectored opportunity at this trace shape).
- Adapter path fact `[RUN]` (read `experiments/parquet/src/backend_sluice.cpp`):
  P2's byte-moving primitive is `sluice::blocking::read_at` — the public
  primitive. P2C1/P2C2 modify composition internals only and their public
  four-op instruction streams are machine-identical to B1 (P2C1 §2.2, P2C2
  §2.2), so neither candidate can change the Parquet adapter path; B1 changes
  `read_at` only at ≤4 KiB (its 64 KiB delta was n.s., P0 §3.2) while this
  trace's reads are ≥33 KiB. The open-time `file_info`+`size` pair
  (`backend_sluice.cpp`, the P0 §4.5 confounder) is symmetric across P1/P2
  arms by protocol design ("Same early rejection as P1's fstat") and is
  disclosed, not corrected, in P4.
- **Verdict: PARQUET ADMITTED.** Trace, dataset, harness, binaries and
  receipts all present, hash-matched and exercised this session. Historical
  R1 numbers are quoted only; nothing in `build/parquet-results/` is edited.

### 1.3 DUCKDB and fio — R0 admission: BLOCKED

- `gh issue view 578` `[RUN]`: state **OPEN**, updatedAt
  2026-10-26T15:18:37Z, title "research(app-io): minimal fio + DuckDB native
  baseline and async host admission R0". Body: a #556 handoff whose R0 items
  1–4 (freeze SHAs/host A-A baseline; seam analysis; `DUCKDB_FIO_R0_PREREG`
  deliverable) are the finite scope — explicitly "**not an assertion that the
  study was completed**".
- Comments `[RUN]`: exactly one (2026-10-26T15:18:36Z) — the #600 handoff
  note. No R0 execution, no prereg deliverable, no A/A study exists.
- Local state `[RUN]`: `fio --version` → not installed; DuckDB binary not on
  host; dududu DuckDB submodule uninitialized (`git submodule status` →
  `-069cc9f9…`); `experiments/duckdb/{common,native,native-wrapper,
  sluice-direct,sluice-threadpool,sluice-uring}` contain README scaffolding
  only (`find -type f` → 6 READMEs; no runner, no oracle).
- **Verdict: DUCKDB + fio BLOCKED.** MISSION §6.2 forbids treating the
  unfinished D1–D4 as existing evidence; #578 R0 has zero completion. P4
  records the blocked row and does not fabricate arms. Revisit path: only an
  amendment naming landed #578 R0 admission evidence (P3 plan §9.4 rule).

### 1.4 HF_SMALL_IO — high-frequency small I/O: BLOCKED

- Admission requires a documented real-application source with the eight
  MISSION §6.2 fields (request cause, input distribution, offset/size, file
  type, sequence dependency, data correctness, original application
  scenario, legal request-combination support).
- Searched evidence `[RUN]`: `P0_BASELINE_AND_OWNERSHIP.md` §4.4 records "No
  admitted high-frequency small-I/O workload exists yet" (from #600
  PHASE3_VALIDATION V3); #578 carries no workload record; dududu #4
  `[RUN]` (`gh issue view 4 --repo jnhu76/dududu`) is a state/indexing-cost
  audit question, not an application I/O source; R1 Parquet is 57 large reads
  (median 1.39 MB — the opposite shape); the G0 W-01 matrix is synthetic.
- **Verdict: HF_SMALL_IO BLOCKED** — no admissible source exists. The
  high-frequency-small-read *shape* is still covered inside W01 by its small
  cells (§3.2), which is kit evidence, not an application-workload claim.
  Revisit path: file the eight-field record + admission before any cell runs.

## 2. Comparator layers per workload (fixed)

MISSION §6.1's three layers, bound to concrete arms:

- **Layer A — mechanism floor (N0):** W01 kit arms `a0`/`w0`/`m0oc`/`m0fi`
  (bare `pread`/`pwrite`/`open+close`/`fstat`). PARQUET: the P1-native replay
  (validation-thin `pread` loop) is the closest runnable floor; a rawer
  no-validation pread loop exists only as a diagnostic if a cell needs it.
  Layer A bounds physical cost; it is never treated as a semantics-carrying
  fair replacement (P0 §1.4).
- **Layer B — semantic parity (N1):** W01 kit arms `a1`/`w1`/`a1x`/`w1x`/
  `a1c`/`w1c`/`m1oc`/`m1fi`/`m1sz` (hand-matched mirrors of the B1 public
  path, P1 §2). PARQUET: the P2 sluice-direct arm (full v2-r1 semantics
  through `read_at`) vs P1 (adapter parity control) — the R1 arm structure.
  Layer B adjudicates Goal A (zero-cost abstraction).
- **Layer C — optimized native competitor:** the native side may use the same
  legal kernel mechanisms Sluice could use — `preadv`/`pwritev` multi-buffer,
  `copy_file_range` file-to-file, gather/dedup planning — with the same input
  information and permitted execution strategies (MISSION §6.1: compare both
  sides' best acceptable implementations). **Selecting a deliberately
  low-quality POSIX loop to manufacture a Sluice advantage is forbidden.**
  - **W01 Layer C statement (fixed): on the single-span shapes the best legal
    native execution is the single `pread`/`pwrite` form Layer B already
    uses** — no additional legal kernel mechanism exists at one-span-one-
    buffer shape (P3: M1 Case A needs k ≥ 2 caller buffers; M2 planning needs
    a request set; M3 needs a second fd). Layer C therefore **collapses onto
    Layer B for all W01 cells** and every W01 row records this as a
    structural fact, not an omission. The Layer C distinction is exercised
    where request structure exists: the PARQUET strategy arms (§4.4).
  - **PARQUET Layer C:** (i) Arrow's own request planner
    (`pre_buffer=true`) — a native planning baseline the R1 protocol
    deliberately disabled; (ii) a native synthetic gather arm (sort/merge/
    dedup + `preadv` where a contiguous multi-buffer extent exists) over the
    frozen trace; (iii) `copy_file_range` — **NOT_APPLICABLE_SHAPE**: a Parquet
    reader returns bytes to user buffers; no file-to-file transfer exists in
    this workload. Recorded as a row with reason, never silently dropped.

## 3. Workload W01 — cells (preregistered)

### 3.1 Session preconditions (order fixed)

1. Environment pin re-recorded (P1 §3): E5, kernel **7.2.11** (current;
   R1-era was 7.2.9), GCC 16.2.1, `scaling_governor=performance`,
   `perf_event_paranoid=1`, `/tmp` tmpfs, `/var/tmp` btrfs.
2. Fixtures: `$READ_FIX=/tmp/sluice600/data64m.bin` regenerated by
   `mkfixture` and hash-pinned (`bc020fd1…b2993a`); per-cell warm write
   scratches per Amendment 1 (`/tmp/sluice600/write-<cellid>.bin`, mkfixture
   before, deleted after); cold fixtures = persistent
   `/var/tmp/sluice600-p1/{cold64m,coldwrite64m}.bin`.
3. **Identity gate:** `sha256sum` of the three linked archives
   (B1 `d1e351a5…`, P2C1 `07d06803…`, P2C2 `67251028…`) and the three kit
   binaries — expected: `ladder` `89450367…010e8`, `ladder-c1`
   `8305d22c…0381`, `ladder-c2` `8bb6473b…0065` — recorded in
   `p4-raw/identity.txt`; mismatch ⇒ stop (§0 rule 2). **Common-kit-source
   clause (Amendment 1):** all three binaries must descend from the one kit
   source commit `2364be4a` (the P2 cross-binary fairness discipline,
   `P2_CANDIDATE_P2C1.md` §2.2: "both sides of every cross-binary cell build
   from the same kit source (common-mode harness TU)"); the kit's `ladder`
   binary must never be a different-source build (the P3-era
   `ladder-4455d98d` = `5a53a9ad…` is excluded from every P4 cell). Any
   future kit-source change requires rebuilding **all three** binaries and a
   numbered amendment before any cross-binary cell runs.
4. **Kit smoke gate** over every arm × pattern × state used below, before any
   timed cell (P1 §1 precedent; raw → `p4-raw/kit-smoke.txt`): all equality
   groups identical (`content_ok=1` for write groups).
5. **A/A floors first**, serially exclusive, per binary and track:
   `w01-aa-read-4k-b1`, `w01-aa-read-4k-c1` (`paired.sh <bin> a1 a1 4096
   1024 1024 5 16 $READ_FIX`), `w01-aa-write-4k-b1`, `w01-aa-write-4k-c2`
   (`w1 w1`, per-cell scratch). Fresh MDE′ dual-reported with the pinned
   floors (read 5.535 ns, write 15.695 ns, cold 1416.185 ns); one-way rule:
   a claim that flips under MDE′ downgrades to NO_CLAIM (P2 §2.4 addition 1).

### 3.2 Shape-class coverage — representative cells

Mission §6.2 W-01 shape roster → representative unit each (16 ABBA pairs,
`taskset -c 2`, warm tmpfs unless stated). Full P1's ~35-unit sweep is **not**
re-run; the seven claimed shapes must each be covered by at least one cell.
Cross-binary cells use `paired2.sh <ladderA> <ladderB> <arm> …` (A = candidate,
B = B1; per-pair statistic candidate − baseline). Layer A/B context arms are
recorded unpaired into the same cell file (P1 §4 convention).

| # | Shape (MISSION §6.2) | Cell id | Arms / command core | Granularity rationale (why this unit represents the shape) |
|---|---|---|---|---|
| 1 | High-frequency small reads | `w01-hf-1b` | `paired2.sh ladder-c1 ladder a2 1 4096 4096 5 16 $READ_FIX`; context a0/a1 | 1 B, 4096 ops: the fixed-path extreme — highest call frequency, zero payload; where #600 measured −16.4 ns/op and per-call wrapper cost dominates all byte cost |
| 1b | (same shape, block carrier) | `w01-hf-4k` | `paired2.sh ladder-c1 ladder a2 4096 1024 1024 5 16 $READ_FIX` | 4 KiB: the C1/P2C1 effect band and the classic small-block unit; doubles as P2C1 negative control (public op machine-identical) |
| 2 | Sequential | `w01-seq-1m` | `paired2.sh ladder-c1 ladder a2 1048576 256 128 5 16 $READ_FIX` | 1 MiB sequential: amortization boundary where per-call costs vanish into byte cost (P1: 64 KiB already n.s.); guards against claiming small-cell wins transfer to large streaming |
| 3 | Random | `w01-rand-4k` | `paired2.sh ladder-c1 ladder a2 4096 1024 1024 5 16 $READ_FIX rand 42` | Random 4 KiB offsets: same per-call path without access locality — separates wrapper cost from cache-locality effects (seed pinned for replayability) |
| 4 | Read/Write | `w01-write-4k` | `paired2.sh ladder-c2 ladder w2 4096 1024 1024 5 16 <per-cell scratch>` | Write primitive at 4 KiB (Amendment-1 scratch, `content_ok=1` gate): the write-side band where P2C2's family lives; P2C2 negative control (public op untouched) |
| 5 | Composition | `w01-comp-read-4k` | per-binary `paired.sh <bin> a1x ax 4096 1024 1024 5 16 $READ_FIX` (both binaries) **and** cross-binary `paired2.sh ladder-c1 ladder ax 4096 1024 1024 5 16 $READ_FIX` | P2C1's benefit unit (`read_exact_at` 4 KiB seq): the exact shape whose wall delta (−16.565 ns, CI-excluding) is the KEEP claim being load-tested |
| 5b | (multi-step/EOF boundary) | `w01-comp-eofmix-4k` | same as 5 with pattern `tail` | Tail rotation (full/half/at-EOF): exercises per-extra-step arcs and EOF exits — the shape of P2C1's capped residual (+10.228 ns); mandatory because the KEEP claim is capped here |
| 5c | (cursor twin, optional-but-listed) | `w01-comp-cursor-4k` | `ac`/`a1c` forms of 5 | Shared-cursor composition twin: first P2C1 twin measured in P2; listed so the P4 record can cite workload coverage of the cursor form; may be waived only by amendment |
| 6 | Metadata-heavy | `w01-meta-oc` | `paired2.sh ladder-c1 ladder m2oc 0 2048 512 5 16 $READ_FIX`; context m0oc/m1oc | open+close cycles at maximum syscall density — the resource-lifecycle metadata shape |
| 6b | (observation metadata) | `w01-meta-fi` | `paired2.sh ladder-c1 ladder m2fi 0 16384 2048 5 16 $READ_FIX`; context m0fi/m1fi | `file_info` at 16384 ops: the stat-observation shape (P2 candidates touch no metadata path — expectation PARITY, machine-identical symbols per P2C2 §2.3) |
| 7 | Cold/Warm | `w01-cold-4k` | cold ABBA loop (P1 §4.5 exact `run_cold` shape) `a2` cross-binary, btrfs `$COLD_FIX`, drop before every invocation, reps=1, ops=16384; plus `w01-aa-cold` (a1 a1) | Cold btrfs 4 KiB: the controlled cold state (P1 §7 discipline); warm is the declared state of every tmpfs cell above. Preregistered expectation NO_CLAIM (cold MDE 1416 ns ≫ any candidate effect) — the row exists to bound the claim, not to win |

Negative controls (mandatory, MISSION §6.4): `w01-neg-1b` and `w01-neg-4k`
(`paired.sh ladder-c1 a1 a2 …`), `w01-negw-4k` (`w1 w2` @ ladder-c2) —
public-op PARITY expectation; any EXCESS routes to the compliant-rerun
disposition (P2C1 §8/§12.1 precedent), never deletion of samples.

P99 note for W01: every W01 candidate effect (−16.6 ns class) is below
10 × the ≈26 ns per-op sampling overhead ⇒ `P99_DELTA =
NOT_SUPPORTABLE_WITH_REASON` on all W01 rows (P0 §4.3; P3 plan §6 row) —
recorded in the row, never silently omitted.

### 3.3 W01 candidate mapping

- P2C1 cells: `w01-comp-read-4k` (primary), `w01-comp-eofmix-4k` (capped-claim
  carrier), `w01-hf-1b`/`w01-hf-4k`/`w01-seq-1m`/`w01-rand-4k`/`w01-meta-*`
  (coverage + negative controls), `w01-cold-4k` (boundary).
- P2C2 cells: `w01-write-4k` (negative control), plus one benefit-class cell
  `w01-wcomp-4k` (`w1x`/`wx` per-binary + `paired2.sh ladder-c2 ladder wx …`,
  Amendment-1 scratch) — instruction-caliber candidate; preregistered wall
  expectation NO_CLAIM (P2C2 §6.2: −8.812 ns sub-MDE with CI spanning 0).
- B1-only context cells (no candidate): none required beyond the Layer A/B
  context arms recorded per cell.

## 4. Workload PARQUET — cells (preregistered)

### 4.1 Arm identity discipline (fixed)

- The frozen R1 artifacts (`build/parquet-results/*`, dataset, traces) are
  **read-only inputs**: quoted, never edited (MISSION §6.2 "历史 R1 不得修改").
- P4 measures new arms by re-staging the Sluice SDK per arm from the frozen
  archives into fresh SDK prefixes (`build/sluice-sdk/p4-b1`,
  `p4-p2c1`, `p4-p2c2`), rebuilding the three tools per the R1 protocol step
  1 (build verification, untimed), and writing a **new** P4 run manifest
  (R1 format: per-arm `SLUICE_SHA`/archive sha256, binary/dataset/trace
  digests, kernel, date) into `p4-raw/`. Frozen worktrees are linked
  read-only (P0 §1.3 rule 2). The dududu branch is not modified; tool-side
  changes (a new strategy arm's TU) live on a new local dududu research
  branch cut from `r1-parquet-sluice-bench`, never pushed, per the R1
  ownership pattern.
- Correctness admission before any number: `ctest` labels `parquet`
  (`parquet.contract`, `parquet.oracle`, `parquet.trace`) green per arm
  build (R1 protocol hard gate); the three arms' oracle digests must match.

### 4.2 Cells (16 ABBA pairs where paired; GB-protocol arms descriptive)

| id | pairing | shape | claim caliber |
|---|---|---|---|
| `pq-aa-replay-native` | P1-native replay vs itself (process-level ABBA) | new-track A/A floor | MDE′ for the track |
| `pq-aa-replay-sluice` | P2-sluice@B1 replay vs itself | A/A | floor |
| `pq-r1-transfer-b1` | P2-sluice@B1-SDK vs P1-native, interleaved ABBA (process invocations alternate; R1's unpaired GB caveat does not apply to this cell) | 57-request trace replay, warm | **Goal-A transfer check at real shape**: preregistered expectation NO_CLAIM — per-call wrapper ≈ +44 instr/44 ns-class vs ≈4.2 ms/read median (trace analysis §1.2) |
| `pq-neg-p2c1` | P2-sluice@P2C1-SDK vs P2-sluice@B1-SDK, ABBA | same trace | **Workload-scale negative control**: adapter uses `read_at`; P2C1's public-op stream is machine-identical to B1 ⇒ predicted PARITY; any resolvable delta is an environment alarm, not a candidate effect |
| `pq-t3-context-b1` | full R1 three-arm protocol (`run-bench.sh` shape, 10 reps) at B1 SDK | T1/T2/T3 × {p0,p1,p2} × {small,medium} | **descriptive context only** — R1 protocol discloses its samples are unpaired; no CI claim (deviation (a)); recorded for continuity with frozen R1 receipts |
| `pq-strategy-arrowprebuf` | P0 arm with Arrow default `pre_buffer=true` vs P0 (frozen config) | whole-scan decode | **NEW APPLICATION STRATEGY** (marked in every row): Arrow's own coalescing planner = native best-planning Layer C baseline; answers "what does planning buy" on the native side |
| `pq-strategy-gather-native` | native synthetic gather arm: sort/merge/dedup + `preadv` (contiguous multi-buffer extents) replaying the frozen trace | trace replay | **NEW APPLICATION STRATEGY / Layer C**: native side's best acceptable implementation using the same legal kernel mechanisms (AC-2/AC-1 shapes) |
| `pq-strategy-gather-sluice` | same planning policy through Sluice's **current public API** (k × `read_at` composition; app-side staging/slicing where the plan requires it) | trace replay | **NEW APPLICATION STRATEGY**: Sluice side's best acceptable implementation under the current one-span API; compared head-to-head with `pq-strategy-gather-native` (both sides' best, MISSION §6.1) |
| `pq-cfr` | — | — | **NOT_APPLICABLE_SHAPE**: no file-to-file transfer in a Parquet reader; AC-3's workload opportunity must come from a copy-shaped workload, which no admitted workload provides (§1.3/§1.4) |

Strategy-arm contract marks (fixed): any arm that changes Reader Request
Planning (merge threshold, dedup, buffer splitting, pre-buffering) **is
explicitly labeled a new application strategy, not a backend effect under an
identical request sequence** (MISSION §6.2). Its request set differs from the
frozen trace by declaration; the per-request byte oracle (each original
request's bytes delivered exactly, verified against the serialized reference
replay — R1 validate discipline + M2 per-request digest precedent) still
gates correctness. These arms produce **workload-opportunity evidence for
AC-1/AC-2/AC-3 only**; nothing in their rows is a Sluice product claim.

### 4.3 Preregistered structural expectations (from §1.2 trace analysis)

- Dedup: overlap = 29,625 B of 181,996,415 B (0.016%) ⇒ dedup arm expected
  NO_CLAIM (far below any plausibly resolvable wall floor).
- Merge/syscall-elimination: the single contiguous segment means full merge
  takes 57 → 1 syscalls — **≤56 saved entries ≈ 13–40 µs** vs ≈230–240
  ms/pass (≤0.017%) ⇒ expected NO_CLAIM at ABBA caliber.
- Staging-copy strategies: a plan that stages the union (181,966,790 B, the
  [4,181966794) segment) then slices adds ≈0.12 ns/B × 181.97 MB ≈ 22
  ms-class cost ⇒ expected REGRESSION; the `preadv` multi-buffer form avoids
  the staging copy but requires buffer re-planning (new app strategy,
  marked).
- These are **predictions, pinned before measurement** (P3 §8 discipline): a
  falsified expectation is recorded as falsified; a confirmed negative
  **closes AC-1/AC-2/AC-3's real-trace opportunity question honestly** —
  "no workload opportunity at the only admitted real trace" is a valid,
  valuable P4 outcome and feeds the Owner decision on the AC candidates.

### 4.4 PARQUET Layer A/B/C row mapping

- Layer A: P1-native replay (`pread` floor). Layer B: P2-sluice@B1 (semantic
  parity, Goal A). Layer C: the three strategy arms (§4.2) + NOT_APPLICABLE
  cfr row. Historical R1 P0/P1/P2 numbers are quoted as the frozen baseline
  context in every PARQUET section header, with their identity
  (SLUICE_SHA=8fdcd99c, kernel 7.2.9-201) stated — never re-labeled.

## 5. Blocked workloads (rows recorded, not fabricated)

- `DUCKDB` — BLOCKED (§1.3 evidence). Row appears in §7 output as
  `BLOCKED: #578 R0 incomplete (issue OPEN, one handoff comment, R0 items
  1–4 unexecuted; fio absent; duckdb submodule uninitialized; scaffolding
  READMEs only)`. The mission's D1–D4 are future work, not evidence.
- `HF_SMALL_IO` — BLOCKED (§1.4 evidence). Row appears as `BLOCKED: no
  documented real-application source (eight-field record absent; P0 §4.4;
  no candidate record in #578/#600/dududu #4)`.
- Neither blocked workload may be unblocked mid-campaign except by numbered
  amendment naming the new admission evidence.

## 6. Statistics discipline (inherited, pinned)

Verbatim inheritance from `P1_MEASUREMENT_PLAN.md` §5–§7 with the P2/P3
additions, applied to every P4 cell:

1. **A/A first** per session, track and binary (§3.1 item 5; §4.2 A/A cells);
   sd_AA is the session noise floor; fresh MDE′ = 2.131·sd_AA/√16
   dual-reported with pinned inherited floors; one-way downgrade rule.
2. **16 ABBA pairs/cell** (≥15 mission minimum), arm order alternating per
   pair, one process invocation per arm sample, `taskset -c 2`, strictly
   serial cells on a quiescent host (P2 §2.4 serial exclusivity; load and
   concurrent-process survey recorded per session — the P3 session's
   unresolved `concurrent_measure_procs=3` pin blemish is the cautionary
   precedent: P4's pin record must show 0 matching measurement processes
   before a session's cells count).
3. **Point estimate** = median of paired deltas; **primary CI** = 95% t on
   the paired mean (t(0.975,15)=2.131·sd/√16); sign count corroboration;
   AB vs BA subgroup medians recorded (order-bias disclosure); all 16 raw
   deltas archived.
4. **Claim rule:** wall claim requires CI excluding 0 AND |median| ≥
   MDE_track; BELOW_NOISE/NO_CLAIM otherwise. No family-wise correction
   across cells (disclosed, P1 §5.6 precedent).
5. **Wall source:** uninstrumented runs only. PMU (`perf stat -e
   instructions:u,cycles:u`) and rusage appear in separate diagnostic
   sections, never differenced as wall (P1 §6 separation rule). CPU_DELTA
   rows carry rusage caliber with PMU corroboration marked.
6. **Amendment 1 scratch discipline** for every warm tmpfs write/copy cell;
   **cold discipline** per P1 §7 (sudo drop, btrfs only,
   `NOT_RUN_WITH_REASON` fallback, never fadvise/tmpfs substitution).
7. One quiesce rerun per failed cell; persistent noise disclosed, never
   absorbed by extra reps without amendment.
8. **Environment drift disclosure:** kernel 7.2.9-201 (R1 receipts,
   `run-manifest.txt:9`) → 7.2.11 (P4 sessions); no cross-kernel
   comparability claimed; P4 PARQUET numbers are fresh measurements, not
   deltas against frozen R1 values.
9. **Parquet GB-protocol arms** (`pq-t3-context-b1`) are descriptive only
   (unpaired repetitions — R1 protocol deviation (a)); CI-bearing claims
   come exclusively from the ABBA-paired replay/strategy cells.

## 7. §6.4 decision rows — format, coverage and verdict rules (fixed)

### 7.1 Row format (one row per candidate × workload cell)

```
WORKLOAD / BASELINE / CANDIDATE / CPU_DELTA / WALL_DELTA / P99_DELTA /
PHYSICAL_WORK_DELTA / CORRECTNESS / UNCERTAINTY / VERDICT
```

- WORKLOAD: cell id + shape class (W01 shape roster #1–#7; PARQUET
  trace-replay / strategy).
- BASELINE / CANDIDATE: arm selector + binary identity (archive sha256
  prefix; for PARQUET arms the SDK prefix + manifest reference).
- CPU_DELTA: rusage user+sys ns/op (corroborating caliber) with PMU
  instr/op delta cross-referenced to the cell's PMU section.
- WALL_DELTA: median paired delta (ns/op or ms/pass), 95% CI, MDE/MDE′
  comparison — uninstrumented runs only.
- P99_DELTA: measured only where (i) the cell's expected effect ≥ 10 × the
  ≈26 ns per-op sampling overhead and (ii) the harness logs per-op samples
  (µs/ms-scale PARQUET replay arms may qualify if per-request logging is
  added on the research branch — itself an amendment-noted change); else
  `NOT_SUPPORTABLE_WITH_REASON` (all W01 rows, §3.2 note).
- PHYSICAL_WORK_DELTA: syscalls/op (strace -c window-isolated integral
  counts, ≥3 exact repeats), bytes requested/issued/extra, user-space copies
  (structural certification), count caliber — never a wall source.
- CORRECTNESS: the cell's oracle result (checksum-equality group,
  `content_ok=1`, per-request digest vs reference replay, `ctest parquet`
  admission, kit smoke gate).
- UNCERTAINTY: sd_AA, MDE′ dual-report, sign count, AB/BA subgroups, caveats.
- VERDICT: from §7.2, nothing else.

### 7.2 Verdict vocabulary (per cell; no post-hoc reinterpretation)

`WIN_CONFIRMED` (CI excludes 0 AND |median| ≥ MDE, favorable direction) /
`PARITY` (Goal-A cells: |median| < max(MDE, 1% of N1 median) — P1 §5.5) /
`REGRESSION` (CI excludes 0, unfavorable) / `NO_CLAIM` (sub-MDE or CI
spanning 0) / `NOT_RUN_WITH_REASON` / `NOT_SUPPORTABLE_WITH_REASON` /
`NOT_APPLICABLE_SHAPE` / `BLOCKED`.

### 7.3 Adjudication rules (fixed)

1. **Local verdicts are retained, never merged.** A candidate that wins at
   high-frequency small I/O and shows NO_CLAIM at large-block Parquet gets
   two separate recorded verdicts; neither cancels the other (MISSION §6.4).
   No single cell's best value may be generalized into a whole-Sluice
   performance conclusion (MISSION §11).
2. **Tail-for-throughput trades may not be hidden.** If any cell shows a
   wall/throughput win together with a resolvable latency-distribution
   worsening, the row records both and the verdict is adjudicated against
   that cell's pre-declared use-case acceptance; a cell with no declared
   latency acceptance cannot claim a win that depends on a tail regression.
3. **Negative controls gate the wins.** A candidate's WIN_CONFIRMED row
   stands only if its public-op negative controls return PARITY (or route
   through the compliant-rerun disposition, P2C1 §12.1 precedent, with the
   original record preserved).
4. **Predictions are binding.** §4.3 expectations and §3 preregistered
   expectations are falsifiable: outcomes are recorded as
   confirmed/falsified; no threshold is re-derived after seeing data.
5. **Correctness precedes performance** (MISSION §9): any oracle/ctest/smoke
   failure stops that candidate's measurement until fixed; numbers from a
   red correctness state are void.
6. **Composition forbidden:** no P2C1+P2C2 stacked cell exists in P4 (P5
   owns it); independent gains are never added.
7. **Blocked workloads stay blocked** except by amendment naming admission
   evidence (§5).

### 7.4 Minimum coverage (fixed)

Every candidate's §7 table must include, at minimum: (a) its benefit-shape
cell(s); (b) at least one negative-control cell; (c) at least one
not-applicable or boundary row (W01: `w01-cold-4k` NO_CLAIM expectation or
`w01-seq-1m`; PARQUET: `pq-cfr` NOT_APPLICABLE_SHAPE and the
`pq-strategy-*` expected-NO_CLAIM rows); (d) the blocked-workload rows
(§5). A candidate with fewer rows than this minimum has not completed P4.

## 8. Raw recording conventions

- All raw series → `research/direct-beyond-parity/p4-raw/<cell-id>.txt`
  (plus `p4-raw/kit-smoke.txt`, `p4-raw/identity.txt`, per-session
  `p4-raw/env-pin-<date>.txt` with load/concurrent-process survey).
- Every file carries a header: date, host, kernel, exact producing command.
- Analysis re-runnable: `python3 $KIT/analyze_pairs.py <armA> <armB> <
  p4-raw/<cell>.txt` (order-aware for two-binary series, per `paired2.sh`).
- PARQUET session manifests in R1 format per arm; frozen R1 artifacts never
  edited (new files only).

## 9. Limitations (preregistered)

1. W01 remains kit-shape evidence (synthetic ladder on deterministic
   fixtures) — it is the W-01 workload the mission names, not an application
   workload; application-shape claims rest on PARQUET only.
2. Layer C collapses to Layer B on W01 single-span shapes (§2) — W01 can
   adjudicate Goal A but cannot, by structure, show beyond-parity vs a
   better native mechanism; that question lives in the PARQUET strategy arms.
3. PARQUET strategy arms are new-application-strategy evidence for AC-1/2/3,
   capped by the P3 §9 admission regime; they are not product claims and not
   comparable to frozen R1 numbers (kernel drift, different request plans by
   declaration).
4. P99 supportable only in µs/ms-scale arms with per-op logging; all other
   rows carry NOT_SUPPORTABLE_WITH_REASON.
5. DuckDB/fio and HF_SMALL_IO blocked (§5) — P4's real-workload coverage is
   exactly W01 + PARQUET; conclusions must not imply broader coverage.
6. E5-native-Linux only; single host; no cross-host comparability
   (methodology rule).

## Amendment log

### Amendment 1 (2026-10-28, pre-measurement, from independent review)

- **Trigger 1 (blocking, kit-source asymmetry in cross-binary cells).** The
  review established — and this session re-verified `[RUN]` — that the kit's
  `ladder.cpp` changed **+852/−31** between `2364be4a` (P2 kit source,
  2026-10-27T21:44) and `4455d98d` (P3 kit source, 2026-10-28T01:43)
  (`git diff --numstat 2364be4a 4455d98d -- research/direct-cost-ladder/ladder.cpp`),
  and that the on-disk binaries were split across those sources: `ladder`
  67784 B built 2026-10-28 01:45 (source `4455d98d`, sha256 `5a53a9ad…`) vs
  `ladder-c1`/`ladder-c2` 41552 B each built 2026-10-27 22:12 (source
  `2364be4a`, `8305d22c…`/`8bb6473b…`). The original §1.1 presented these as
  one verified set and did not disclose the asymmetry or the P2 session's own
  B1-ladder identity (`89450367…010e8`, `p2-raw/kit/ladder-identity.txt`,
  every `p2-raw/p2c1/*` cell header, P2C1 §12.3). This violated the binding
  P2 discipline that both sides of every cross-binary cell build from the
  same kit source (common-mode harness TU, `P2_CANDIDATE_P2C1.md` §2.2) —
  exactly the class of systematic behind P2's recorded +9–10 ns false
  positive (P2C1 §8/§12.2) — and would have affected every `paired2.sh`
  cell, including the primary claim carrier `w01-comp-read-4k`.
- **Change 1.** `ladder` rebuilt from kit source `2364be4a` (scratch
  `git archive 2364be4a` export at `/tmp/p4-kit-2364be4a`, one-off per
  AGENTS rules; same `build.sh` flags, frozen B1 archive) `[RUN]` → sha256
  `8945036746d03389e6f44df7a6b26ae5599f2f1bddda8825da28ebd11dc010e8`,
  **byte-exact the P2 session's B1-linked binary**; installed as the kit
  `ladder`. `ladder-c1`/`ladder-c2` unchanged (already `2364be4a`-built, the
  literal P2-measured artifacts). The P3-era build is preserved untracked as
  `ladder-4455d98d` (`5a53a9ad…`) and excluded from every P4 cell. §1.1 and
  the §3.1 identity gate now pin the three expected binary hashes and add
  the **common-kit-source clause**: all cross-binary cells use binaries from
  the one kit source commit `2364be4a`; any future kit-source change
  requires rebuilding all three binaries plus a numbered amendment.
  Rationale for choosing `2364be4a` over rebuilding everything at
  `4455d98d`: P4 uses no P3 arm; `2364be4a` preserves the exact binary
  identities the P2 KEEP claims were measured with (zero new harness
  variables), whereas a `4455d98d` rebuild would have replaced the
  P2-measured c1/c2 binaries with never-measured ones. Post-fix smoke re-run
  `[RUN]`: seven read arms × {ladder, ladder-c1} identical checksum 1310720;
  `ladder-c2` w1/wx `content_ok=1`. The §0 rule 2 archive sha256s (the P2
  evidence carriers) are unaffected by this amendment.
- **Trigger 2 (evidence-statement defect).** The original §1.2 claimed the
  frozen trace's "sorted extents merge into 1 contiguous union (zero gaps)".
  The zero-gap clause was false — the draft's own union arithmetic
  (181,966,790 B vs file size 181,966,794 B) already implied uncovered
  bytes. Recomputed `[RUN]` with a direct point-coverage query: exactly
  **one 4-byte hole, [0,4)**; the covered set is the single segment
  [4,181966794). The review additionally suspected a second hole at
  [1550843,1550847); **disproved by the query** — those bytes are covered by
  request [4,1550847) (offset 4 + requested 1550843), and the next request
  starts exactly at 1550847 ([1550847,3057951)).
- **Change 2.** §1.2 rewritten to the verified geometry (1 segment, one
  4-byte hole, overlap 29,625 B, union+overlap arithmetic closure); §4.3
  restated with exact figures. Predictions unchanged in substance: full
  merge remains 57 → 1 syscalls (**≤56 saved entries**, still inside the
  preregistered bound), dedup 29,625 B (0.016%), staging-copy ≈22 ms class.
- **Trigger 3 (transcription defect).** §1.2 quoted the frozen R1 kernel as
  `7.2.9-200.fc44.x86_64`; the frozen `run-manifest.txt:9` actually reads
  `KERNEL=7.2.9-201.fc44.x86_64` `[RUN]` (`grep -n KERNEL`; protocol.md:187
  agrees). **Change 3:** all citations corrected to `7.2.9-201` (§1.2, §4.4,
  §6 item 8). No prediction depends on the patch level.
- Nothing else changes: workload admissions, cell definitions, pairing
  counts, statistics, calibers, correctness gates and verdict rules are as
  originally preregistered.
