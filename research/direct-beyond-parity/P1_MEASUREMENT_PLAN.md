# P1 Measurement Plan — Direct Cost Floor (Preregistered)

- Mission: `research/direct-beyond-parity/MISSION.md` §3 (P1 — Complete Direct
  Cost Floor).
- Preregistration date: 2026-10-27. Primary platform: E5 Native Linux.
- Authority and inputs: `P0_BASELINE_AND_OWNERSHIP.md` §1 (baseline and
  ownership rules), §3 (#600 ladder and pairing method, inherited calibers),
  §4.10–4.13 (open questions this plan closes), §5 (semantic floor every
  comparator must preserve); `docs/performance/methodology.md` (standing
  method); comparator template `probe-snapshot-600/` (#600 kit).
- Status of this document: **preregistered**. Cell definitions, pairing
  counts, statistics, calibers and gates below are fixed before the P1
  measurement campaign runs. Any deviation discovered mid-campaign is
  recorded as an amendment with reason, never silently applied.

## 0. What this plan closes from P0

| P0 open item | Closed by |
|---|---|
| §4.13 E5 ladder covers read path only | §1–§2: committed ladder extended to `write_at`/`write_all_at` and `open+close`/`file_info`/`size` with N0/N1/B1 tiers |
| §4.8 comparator arms must have committed, rebuildable identity | §1: kit committed on local branch `direct-cost-ladder` |
| §4.10 cold-cache control has no standard procedure | §7: operational definition fixed and validated on this host |
| §4.11 15-pair rule not formalized | §5: preregistered statistics with #600 precedent |
| §4.12 CPU/op caliber not pinned | §6: dual rusage + perf caliber with separation rule |
| §3.6 four-config matrix commands not captured | §8: exact matrix commands pinned (release leg executed) |

## 1. Ladder identity and ownership compliance

- Branch: `direct-cost-ladder` (local only, never pushed), cut from B1
  `77e5399a45998dea396699fa2e063c94a665945a` (PR #602 head). Kit commit:
  `e096b4a1e3b7fb1cbfedfcd5f10e7e6a88050637`
  ("research(direct): committed cost ladder for write and metadata paths";
  exactly one commit over B1, touching only `research/direct-cost-ladder/`).
- Worktree: `/home/jnhu/Source/Sluice-direct-cost-ladder`. Not the main
  workspace; not either frozen worktree.
- Linked library: frozen B1 archive
  `/home/jnhu/Source/Sluice-600-wt/build/linux/x86_64/release/libsluice_core.a`,
  sha256 `d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0`,
  re-verified this session and enforced by `build.sh`:
  `sha256sum /home/jnhu/Source/Sluice-600-wt/build/linux/x86_64/release/libsluice_core.a`
  → the value above. The frozen worktree stays at `77e5399a`, untouched
  (P0 §1.3 rule 2: read-only linking only).
- Build (exact, from inside the kit directory):
  `./build.sh [output]` =
  `CCACHE_DISABLE=1 g++ -std=c++20 -O3 -DNDEBUG -fvisibility=hidden -Wall -Werror -I/home/jnhu/Source/Sluice-600-wt/include ladder.cpp /home/jnhu/Source/Sluice-600-wt/build/linux/x86_64/release/libsluice_core.a -o ./ladder`
  (GCC 16.2.1 20260819, binutils 2.46.1; recorded binary sha256
  `0685c31f7585e437910ed671a7025a172de4303e756d5b6e31b2480e1c45812a`).
- Read-arm lineage: `A0/A1/A2/A3/AX` preserve the #600 ladder behavior
  (template sha256
  `d879b2685b14539efa6552b9918776a401ee5633339df4db1e58c70c252fe884`),
  including best-of-reps wall caliber, checksum gate, and rusage fields.
  `mkfixture.cpp`/`analyze_pairs.py` are byte-exact #600 snapshot copies.
- Smoke verification performed before this plan was finalized: all families
  × patterns (seq/rand/tail) × states (normal/closed/eintr) build, run, and
  produce identical checksums (and, for write cells, identical
  `content_hash`) inside every required equality group — raw record
  `p1-raw/smoke-ladder.txt` (120 arm lines + group audit:
  ALL GROUPS IDENTICAL). One defect was found and fixed during smoke
  (composition stop paths dropped `confirmed_bytes`; would have zeroed every
  eof/partial stop) — the fix is in the committed kit, not patched afterhand.

## 2. Comparator tiers and the semantics floor

Every family is measured at three tiers (MISSION P1.3):

- **N0 bare syscall floor**: `A0_raw_pread` (`::pread`), `W0_raw_pwrite`,
  `M0_raw_open_close` (`::open`+`::close` with the flags Sluice ends up
  passing), `M0_raw_fstat`. A mechanism floor, not a semantics-carrying
  replacement (P0 §1.4).
- **N1 hand-matched native**: `A1`, `W1`, `A1x`, `W1x`, `M1oc`, `M1fi`,
  `M1sz`. Public Sluice headers only (no `detail/` includes), separate TU
  boundary, `Result<T>` ABI, mirroring the B1 public path check order exactly
  (sources mirrored: `src/blocking_file.cpp`, `src/file_resource.cpp` at
  `77e5399a`):
  closed → access → zero length (no OS call) → range/transfer limit → null
  buffer; EINTR retried only without a completed count, never replaying
  confirmed bytes; partial counts are success without looping; read EOF-zero
  is success; `read_exact`/`write_all` stop conditions
  (complete / eof_before_full / write_no_progress / primitive_error /
  impossible-count) with effect certainty accounted/unknown exactly as
  `direct_outcome` publishes and the confirmed prefix preserved through
  stops; `open` prechecks embedded NUL then read_only+truncate, maps flags
  (O_CLOEXEC, existence, truncate) with mode 0644 and no EINTR retry;
  `close` consumes ownership on the first attempt (also on EINTR/error) and a
  consumed close is a no-op; `file_info` rejects closed with invalid_state,
  retries `fstat` on EINTR, maps kind/size/identity; `size` delegates to
  `file_info` (structural mirror).
- **B1 Sluice arm**: `A2`, `AX`, `W2`, `WX`, `M2oc`, `M2fi`, `M2sz`, linked
  against the frozen archive.
- **Test-only diagnostic**: `A3_nosyscall_stub` (attribution only, never a
  performance oracle; excluded from checksum equality groups).

The N1 arms mirror semantics **on the probe's legal domain** (open file of
the right access, in-range offsets, non-null buffers). The frozen scenario
tables and fault-injection suites (P0 §5.1–§5.3) remain the authority for
out-of-domain behavior; this ladder measures cost, not semantic coverage.

Diagnostic states (never throughput substitutes):

- `closed` — every op runs against a closed File/fd; userspace arms measure
  the pure rejection path (invalid_state), raw arms document the kernel
  EBADF floor.
- `eintr` — 3 µs SIGALRM itimer installed without SA_RESTART so pread/pwrite
  surface EINTR; measures retry-handling cost. Delivery timing is
  non-stationary; descriptive statistics only.

## 3. Environment pin (frozen for every P1 cell)

- Host: E5 (Xeon E5-2666 v3, 10C/20T, 1 NUMA node), Fedora 44, kernel
  7.2.11-200.fc44.x86_64, `scaling_governor=performance`,
  `perf_event_paranoid=1` (perf `instructions:u` usable unprivileged —
  validated this session), GCC 16.2.1, binutils 2.46.1.
- CPU pinning: every timed invocation under `taskset -c 2` (#600 precedent).
- Filesystems: `/tmp` tmpfs (32G) for warm cells; `/var/tmp` on btrfs
  `/dev/sda3` (root device) for cold cells. tmpfs warm reads never represent
  cold or device-backed behavior (P0 §4.10 rule).
- Fixtures (64 MiB each, deterministic):
  - `$READ_FIX=/tmp/sluice600/data64m.bin` (tmpfs read fixture, sha256
    `bc020fd15a622e081793b0fb8ff2383838e27f6d1cb2135f204aad8383b2993a`)
  - `$WRITE_FIX=/tmp/sluice600/write64m.bin` (tmpfs write scratch)
  - `$COLD_FIX=/var/tmp/sluice600-p1/cold64m.bin` (btrfs read fixture)
  - `$COLDW_FIX=/var/tmp/sluice600-p1/coldwrite64m.bin` (btrfs write scratch,
    pre-primed to the uniform block pattern once at setup)
- Setup commands (exact, run once before the campaign):

```sh
KIT=/home/jnhu/Source/Sluice-direct-cost-ladder/research/direct-cost-ladder
LADDER=$KIT/ladder
PAIRED=$KIT/paired.sh
READ_FIX=/tmp/sluice600/data64m.bin
WRITE_FIX=/tmp/sluice600/write64m.bin
COLD_FIX=/var/tmp/sluice600-p1/cold64m.bin
COLDW_FIX=/var/tmp/sluice600-p1/coldwrite64m.bin
mkdir -p /tmp/sluice600 /var/tmp/sluice600-p1
$KIT/mkfixture $READ_FIX 67108864
$KIT/mkfixture $WRITE_FIX 67108864
$KIT/mkfixture $COLD_FIX 67108864
$KIT/mkfixture $COLDW_FIX 67108864
taskset -c 2 $LADDER w0 4096 64 0 1 $COLDW_FIX   # one-time uniform priming (cold-write cells then use noprime)
```

## 4. Load matrix (preregistered cells)

Common shape: paired series = `$PAIRED $LADDER <armA> <armB> <size> <ops>
<warmup> <reps> <pairs> <file> [pattern] [seed] [state] [prime]` with pairs=16
(§5); context arms (floor/diagnostic) run unpaired in the same cell record.
Ops table: sizes 0–512 B → ops=4096, warmup=4096; 4 KiB → 1024/1024; 64 KiB
→ 512/512; 1 MiB → 256/128; all with reps=5 (best-of caliber, #600 shape).
Primary pairing is N1 vs B1 (excess measurement); the floor and composition
pairings are recorded per cell as listed.

### 4.1 read track (tmpfs, warm)

| id | pairing | command (after §3 setup) | context arms |
|---|---|---|---|
| read-sweep-0b | A1 vs A2 | `$PAIRED $LADDER a1 a2 0 4096 4096 5 16 $READ_FIX` | a0, a3 |
| read-sweep-1b | A1 vs A2 | `$PAIRED $LADDER a1 a2 1 4096 4096 5 16 $READ_FIX` | a0, a3 |
| read-sweep-64b | A1 vs A2 | `$PAIRED $LADDER a1 a2 64 4096 4096 5 16 $READ_FIX` | a0, a3 |
| read-sweep-512b | A1 vs A2 | `$PAIRED $LADDER a1 a2 512 4096 4096 5 16 $READ_FIX` | a0, a3 |
| read-sweep-4k | A1 vs A2 | `$PAIRED $LADDER a1 a2 4096 1024 1024 5 16 $READ_FIX` | a0, a3 |
| read-sweep-64k | A1 vs A2 | `$PAIRED $LADDER a1 a2 65536 512 512 5 16 $READ_FIX` | a0, a3 |
| read-sweep-1m | A1 vs A2 | `$PAIRED $LADDER a1 a2 1048576 256 128 5 16 $READ_FIX` | a0, a3 |
| read-composition-4k | A1x vs AX | `$PAIRED $LADDER a1x ax 4096 1024 1024 5 16 $READ_FIX` | a1, a2 (same run, context) |
| read-random-4k | A1 vs A2 | `$PAIRED $LADDER a1 a2 4096 1024 1024 5 16 $READ_FIX rand 42` | a0 |
| read-eof-mix-4k | A1 vs A2, plus A1x vs AX | `$PAIRED $LADDER a1 a2 4096 1024 1024 5 16 $READ_FIX tail` then `$PAIRED $LADDER a1x ax 4096 1024 1024 5 16 $READ_FIX tail` | a0 |

Context-arm command shape (recorded into the same cell file, unpaired):
`for arm in a0 a3; do taskset -c 2 $LADDER $arm <size> <ops> <warmup> 5 $READ_FIX [pattern]; done`

### 4.2 write track (tmpfs scratch, warm; content_ok=1 gate)

| id | pairing | command | context arms |
|---|---|---|---|
| write-sweep-0b | W1 vs W2 | `$PAIRED $LADDER w1 w2 0 4096 4096 5 16 $WRITE_FIX` | w0 |
| write-sweep-1b | W1 vs W2 | `$PAIRED $LADDER w1 w2 1 4096 4096 5 16 $WRITE_FIX` | w0 |
| write-sweep-64b | W1 vs W2 | `$PAIRED $LADDER w1 w2 64 4096 4096 5 16 $WRITE_FIX` | w0 |
| write-sweep-512b | W1 vs W2 | `$PAIRED $LADDER w1 w2 512 4096 4096 5 16 $WRITE_FIX` | w0 |
| write-sweep-4k | W1 vs W2 | `$PAIRED $LADDER w1 w2 4096 1024 1024 5 16 $WRITE_FIX` | w0 |
| write-sweep-64k | W1 vs W2 | `$PAIRED $LADDER w1 w2 65536 512 512 5 16 $WRITE_FIX` | w0 |
| write-sweep-1m | W1 vs W2 | `$PAIRED $LADDER w1 w2 1048576 256 128 5 16 $WRITE_FIX` | w0 |
| write-composition-4k | W1x vs WX | `$PAIRED $LADDER w1x wx 4096 1024 1024 5 16 $WRITE_FIX` | w1, w2 |
| write-random-4k | W1 vs W2 | `$PAIRED $LADDER w1 w2 4096 1024 1024 5 16 $WRITE_FIX rand 42` | w0 |

### 4.3 metadata track (tmpfs read fixture, warm)

| id | pairing | command | context arms |
|---|---|---|---|
| meta-open-close | M1oc vs M2oc | `$PAIRED $LADDER m1oc m2oc 0 2048 512 5 16 $READ_FIX` | m0oc |
| meta-file-info | M1fi vs M2fi | `$PAIRED $LADDER m1fi m2fi 0 16384 2048 5 16 $READ_FIX` | m0fi |
| meta-size | M1sz vs M2sz | `$PAIRED $LADDER m1sz m2sz 0 16384 2048 5 16 $READ_FIX` | m0fi (shared floor) |

### 4.4 edge/diagnostic track (descriptive only; never throughput)

| id | pairing | command | notes |
|---|---|---|---|
| read-reject-closed-4k | A1 vs A2 | `$PAIRED $LADDER a1 a2 4096 4096 1024 3 16 $READ_FIX seq 0 closed` | a0 context = kernel EBADF floor; userspace reject = invalid_state |
| write-reject-closed-4k | W1 vs W2 | `$PAIRED $LADDER w1 w2 4096 4096 1024 3 16 $WRITE_FIX seq 0 closed` | w0 context |
| meta-reject-closed | M1fi vs M2fi | `$PAIRED $LADDER m1fi m2fi 0 4096 1024 3 16 $READ_FIX seq 0 closed` | m0fi context |
| read-eintr-4k | A1 vs A2 | `$PAIRED $LADDER a1 a2 4096 2048 256 2 16 $READ_FIX seq 0 eintr` | itimer; no CI claims (§5.8) |
| write-eintr-4k | W1 vs W2 | `$PAIRED $LADDER w1 w2 4096 2048 256 2 16 $WRITE_FIX seq 0 eintr` | itimer; no CI claims |

### 4.5 cache track (btrfs, cold — see §7 for the drop operation)

| id | pairing | command | notes |
|---|---|---|---|
| read-cold-4k | A1 vs A2 | ABBA loop below, size 4096, ops 16384, warmup 0, reps 1 | drop before every invocation |
| read-cold-64k | A1 vs A2 | same loop with size 65536, ops 1024 | |
| write-cold-4k | W1 vs W2 | same loop against `$COLDW_FIX`, size 4096, ops 16384, plus trailing `normal noprime` args | pre-primed scratch (§3); drop nullifies priming only because prime is skipped |
| aa-cold-read-4k | A1 vs A1 | same loop, both arms a1 | cold A/A noise |

Cold ABBA loop (exact; the credential is entered interactively via sudo's
stdin prompt — never embedded in files, commands or logs):

```sh
drop() { sudo -S -p '' sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'; }
run_cold() { # armA armB size ops file [extra...]
  local a=$1 b=$2 sz=$3 ops=$4 f=$5; shift 5
  for p in $(seq 0 15); do
    if [ $((p % 2)) -eq 0 ]; then first=$a second=$b; else first=$b second=$a; fi
    for arm in $first $second; do
      drop
      taskset -c 2 $LADDER $arm $sz $ops 0 1 $f "$@"
    done
  done
}
run_cold a1 a2 4096 16384 $COLD_FIX                 > p1-raw/read-cold-4k.txt
run_cold a1 a2 65536 1024 $COLD_FIX                 > p1-raw/read-cold-64k.txt
run_cold w1 w2 4096 16384 $COLDW_FIX seq 0 normal noprime > p1-raw/write-cold-4k.txt
run_cold a1 a1 4096 16384 $COLD_FIX                 > p1-raw/aa-cold-read-4k.txt
```

Analysis: `python3 $KIT/analyze_pairs.py <armA> <armB> < p1-raw/<cell>.txt`.
Cold cells additionally report the median of the 16 single-pass ns/op values
per arm (reps=1 → no best-of).

### 4.6 A/A noise cells (warm, one per track per session)

| id | command |
|---|---|
| aa-read-4k | `$PAIRED $LADDER a1 a1 4096 1024 1024 5 16 $READ_FIX` |
| aa-write-4k | `$PAIRED $LADDER w1 w1 4096 1024 1024 5 16 $WRITE_FIX` |
| aa-meta-file-info | `$PAIRED $LADDER m2fi m2fi 0 16384 2048 5 16 $READ_FIX` |

## 5. Preregistered statistics

1. **Design.** Every quantitative cell uses interleaved ABBA pairing: 16
   pairs (≥15 required by the mission brief; 16 pinned to match the #600 E3
   precedent of 16 ABBA pairs/cell, P0 §3.1/§3.2), arm order alternating per
   pair, one process invocation per arm sample, all invocations pinned to
   cpu2. The ABBA alternation cancels first-run/order bias by design (#600
   measured a −5.1 ns systematic first-run-faster effect and canceled it this
   way; no additional numeric order correction is applied. Residual order
   effect is checked by comparing AB vs BA subgroup medians and reported).
2. **A/A noise baseline.** Per session and track, the §4.6 A/A cells yield
   the pair-delta sd (sd_AA). This is the session noise floor; #600's
   reference values on this host: sd_AA ≈ 5.0 ns, minimum detectable effect
   ≈ 2.5–3 ns at 16 pairs. A cell whose |median delta| < MDE (defined below)
   is classified BELOW_NOISE and no excess/parity claim is made from it
   (handling precedent: #600's 64 KiB +6.2 ns n.s. cell; P0 §3.3's
   within-noise caliber note).
3. **Point estimate.** Median of the 16 paired deltas (armA − armB as named
   in the cell), reported in ns/op together with mean and sd (the analyzer's
   output fields). For cold cells additionally the per-arm median of the 16
   single-pass values.
4. **CI.** Primary: 95% t-based CI on the mean paired delta,
   mean ± t(0.975, df=15)=2.131 × sd_deltas/√16, matching the #600
   paired-t reporting caliber. Corroboration (not primary): sign count
   (positive deltas / 16). The raw 16 deltas are always archived in p1-raw
   so any convention can be recomputed (this closes the P0 §3.2 caliber
   uncertainty for P1 numbers).
5. **MDE gate.** MDE_cell = 2.131 × sd_AA(track) / √16. Claim rule:
   - *Excess* (B1 arm slower than N1): CI excludes 0 AND |median| ≥ MDE.
   - *PARITY_REACHED* (MISSION P1 output vocabulary): |median| <
     max(MDE, 1% of the N1 arm median).
6. **Multiple cells.** 26 planned quantitative cells are interpreted per
   cell with CIs; no family-wise correction is applied (consistent with #600
   practice; disclosed here deliberately).
7. **Wall source.** Wall ns/op comes exclusively from uninstrumented
   invocations (§6 rule 3). PMU runs never feed wall statistics.
8. **Diagnostic cells** (closed/eintr, §4.4): medians and ranges only; no
   t/CI/sign claims — the eintr state is non-stationary by construction and
   closed cells measure different mechanisms per tier.
9. **Run-integrity gate before reading numbers** (methodology step 2): every
   invocation must exit 0 with no VERIFY_FAIL; checksum equality must hold
   within the cell's equality group (read group {a0,a1,a2,a1x,ax} minus a3;
   write group {w0,w1,w2,w1x,wx} with content_ok=1; metadata groups
   {m0oc,m1oc,m2oc}, {m0fi,m1fi,m2fi}, {m1sz,m2sz}; in closed state the
   userspace group {a1,a2,a1x,ax,w1,w2,w1x,wx,m1fi,m2fi,m1sz,m2sz} while
   raw arms document the floor separately). One failing invocation
   invalidates the whole cell series (rerun the cell, do not drop samples).

## 6. CPU/op caliber (pinned)

Every cell records two calibers, always from separate runs:

1. **rusage user/sys ns/op** — printed by the ladder itself
   (`user_ns_per_op`, `sys_ns_per_op`). Window = warmup + timed reps (+ the
   read-verify pass for read arms, matching #600; the write readback runs
   outside the window). Granularity caveat: rusage samples at scheduler
   ticks; at #600-scale op counts this is the ±30 ns/op-class signal P0
   §4.12 described — user/sys are corroborating evidence, wall is primary.
2. **perf instructions:u (and cycles:u)** — PMU diagnostic run per cell and
   arm (never the wall source), unprivileged (perf_event_paranoid=1,
   validated):

   ```sh
   taskset -c 2 perf stat -e instructions:u,cycles:u \
       $LADDER <arm> 4096 4096 0 1 $READ_FIX
   ```

   Reported as instructions per op with the fixed-process-overhead caveat:
   the counter covers the whole process, so absolute instructions/op are
   approximate; **cross-arm instruction deltas are the signal** (shared
   harness overhead cancels). Static disassembly remains the authority for
   absolute instruction counts (#600 precedent: 70 → 40 success-path
   instructions at -O3).
3. **Separation rule.** A wall/throughput number may cite only
   uninstrumented runs; PMU numbers appear only in a separate PMU section
   and are never differenced as wall time (P0 §4.12 / methodology step 3:
   profiling answers where cost lives, never how much a change improves).

## 7. Cold-cache operational definition (validated this session)

- Operation: `sync` + `echo 3 > /proc/sys/vm/drop_caches`, executed via
  password-protected sudo (`sudo -S -p '' sh -c 'sync; echo 3 >
  /proc/sys/vm/drop_caches'`; the operator's credential is entered at the
  prompt — it is not stored in any file, command line, or log).
- Validation (raw: `p1-raw/cold-cache-validation.txt`, 2026-10-27): 64 MiB
  btrfs file, 4 KiB `A0` reads: warm ≈ 925–1010 ns/op → immediately after
  drop ≈ 8498/8720 ns/op (8–9× slower) → warm again ≈ 954 ns/op. tmpfs
  control unchanged across the same drops (1010.8 → 1010.9 ns/op):
  `drop_caches` cools page-cache files on btrfs and does **not** cool tmpfs
  (shmem) pages. Therefore: cold cells run only on btrfs fixtures; a tmpfs
  read is never reported as cold (P0 §4.10 rule).
- Scope of the control, disclosed: the drop evicts the page cache; btrfs
  metadata/allocator state, on-disk controller cache (if any), and
  concurrent root-device I/O are not controlled. Cold-write cells
  additionally depend on dirty-page writeback timing (single pass, median
  over 16 invocations, `noprime` precondition per §4.5).
- NOT_RUN_WITH_REASON fallback: if sudo becomes unavailable in a later
  session, the cold cells are marked `NOT_RUN_WITH_REASON: drop_caches
  unavailable` in that session's records; they are never substituted with
  fadvise-only or tmpfs approximations (G0's fadvise(DONTNEED)-only cold
  column was effectively warm — P0 §4.10 — and is not reused).

## 8. Four-config correctness matrix (commands pinned; P0 §3.6 residual)

Run inside a worktree of the tree under test (for the P1 floor that is
`/home/jnhu/Source/Sluice-direct-cost-ladder`; later candidates use their own
fresh worktree — never the frozen worktrees, never the main workspace):

```sh
cd /home/jnhu/Source/Sluice-direct-cost-ladder
for m in release debug asan asanubsan; do
  xmake f -c -m $m --liburing=n -y
  xmake -y
  xmake test -y 2>&1 | tee /home/jnhu/Source/Sluice/research/direct-beyond-parity/p1-raw/matrix-$m.txt
done
```

- `--liburing=n` is pinned for **all four** configurations: it matches the
  frozen library identity (both #600 libraries and the B1 archive were built
  with `--liburing=n`), and it keeps the 58-test suite shape identical to the
  release leg executed this session (58/58 passed — raw:
  `p1-raw/matrix-release.txt`). The release leg was executed during planning;
  debug/asan/asanubsan legs are prescribed here and run at the campaign
  boundary (each must show 58/58 before any candidate comparison is
  admitted).
- A `--liburing=y` pass is added only when a candidate under test changes
  uring-transport code (not in P1 scope), and requires the liburing dev
  package present.
- `xmake f -c` (clean reconfigure) between modes avoids any stale-profile
  object reuse; the build-affecting binary is the user-local
  `/home/jnhu/.local/bin/xmake` v3.1.2+HEAD.5d21e9d (P0 §1.5 caveat).

## 9. Per-run oracle gates

A cell sample is valid only if (checked before any number is read):

1. exit status 0 and no `VERIFY_FAIL` on stderr;
2. checksum equality inside the cell's equality group (§5.9);
3. write cells: `content_ok=1`;
4. fixture identity: read fixture sha256 unchanged after the session
   (`sha256sum $READ_FIX` before/after campaign);
5. ladder identity: `./ladder` binary is the §1 build (build.sh re-verified
   archive hash at build time; binary sha256 recorded in p1-raw).

## 10. Raw output conventions

- All raw series go to `research/direct-beyond-parity/p1-raw/<cell-id>.txt`
  (this session's planning evidence already there: `ladder-identity.txt`,
  `smoke-ladder.txt`, `cold-cache-validation.txt`, `matrix-release.txt`).
- Every file carries a header line with date, host, and the exact producing
  command.
- Analysis is always re-runnable: `python3 $KIT/analyze_pairs.py <armA>
  <armB> < p1-raw/<cell>.txt`.

## 11. Limitations and caveats

1. N1 arms are handwritten mirrors on the probe's legal domain; the frozen
   semantic tables (P0 §5.1) remain the out-of-domain authority. A later
   candidate changing public semantics invalidates the mirror and requires
   regenerating N1 arms before any comparison.
2. rusage granularity makes user/sys per-op coarse at small op counts; they
   corroborate, never lead, a claim.
3. perf counters cover the whole process; absolute instructions/op are
   approximate — deltas are the signal; static disassembly stays the
   authority for absolute counts.
4. Cold control evicts page cache only (§7 scope); the root device is shared
   (btrfs system disk) — cold cells disclose this and are run on a quiescent
   host.
5. EINTR cells are itimer-driven and non-stationary; descriptive only.
6. Metadata open/close cells exercise tmpfs path resolution at depth 2;
   deeper dentry pressure is not covered.
7. Composition cells measure the one-primitive-completes case at 4 KiB;
   multi-step composition is exercised only via the tail cells' EOF boundary.
8. All claims are E5-native-Linux only; no cross-host comparability
   (methodology rule); #600/WSL2 numbers are shape references, not targets.
9. The kit branch is local-only by ownership rule; its identity is the
   commit SHA recorded here. If the branch is ever lost, the kit is
   recoverable from this document's build command plus the archived sources
   in p1-raw (ladder.cpp hash `f43db18cc42ec93e404e6a2d578264299e278a56c15ea3f3ffd27857b53f6005`).
10. Ops/pairs were chosen from the #600 caliber; if a cell's A/A noise
    exceeds the #600 band (>10 ns sd_AA) the cell is rerun once after
    quiescing; persistent noise is disclosed, never absorbed by more reps
    without an amendment.

## Amendment 1 (2026-10-27, run owner, escalation dwfq-ad7f7a55-1): per-cell warm write scratch

- **Trigger (evidence):** `write-sweep-1b` attempt 1 and `write-random-4k`
  attempt 1 (both archived void per §5.9: `p1-raw/write-sweep-1b-attempt1.txt`,
  `p1-raw/write-random-4k-attempt1-failed.txt`) failed their `content_ok`
  gates because concurrently running warm write cells shared
  `$WRITE_FIX=/tmp/sluice600/write64m.bin` as scratch and overwrote each
  other's prime patterns (scratch scan: foreign ladder block pattern at
  phase `offset%4096`, size-4096 provenance; `ps` caught both cells'
  `paired.sh` on the same file). The preregistration fixed cell commands but
  not write-cell scheduling — a plan gap, closed here.
- **Change:** every **warm tmpfs write cell** (all of §4.2 using `$WRITE_FIX`:
  `write-sweep-*`, `write-composition-4k`, `aa-write-4k`, `write-eintr-4k`,
  `write-reject-*`, and `write-random-4k` wherever it is executed) uses a
  **per-cell scratch path** `/tmp/sluice600/write-<cellid>.bin` instead of the
  shared `$WRITE_FIX`: create it with `$KIT/mkfixture <path> 67108864`
  immediately before the cell, run the cell's registered command with the
  path substituted for `$WRITE_FIX`, and delete the file after the cell
  completes. No other command token changes.
- **Why this is isolation, not a variable change:** each ladder invocation
  primes the whole scratch and verifies on return, so per-invocation
  behavior on a fresh vs reused tmpfs file is identical (same size, same
  deterministic pattern, same prime-then-verify). Sizes, ops, pairs, arms,
  patterns, seeds, and gates are untouched.
- **Cold-write cells (`$COLDW_FIX`) are unchanged:** their protocol is
  overwrite-of-pre-primed-file (allocation vs overwrite is semantically
  different on btrfs); they already run track-serialized and must remain so.
- **Retries:** the two voided attempts above are retried under this
  amendment (retry budget per standing rule), with the amendment cited in
  the cell record.
