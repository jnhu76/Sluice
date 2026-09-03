# G1-CONTROL-C0 PREREGISTRATION — fixed-file resource identity and
# specialization falsification (#279)

Status: **FROZEN** before formal measurement. Only additive amendments are
permitted after formal measurement begins (each logged in §16 with a reason
and a timestamp). Governing issue: #279. Audit:
`research/g1-control-c0/G1-CONTROL-C0-AUDIT.md`. Governing roadmap: #227
(G1-Control gate), #259 (data/control plane roadmap). Related negative
evidence: RBUF-E0 #272/PR #273 (registered BUFFER steady-state NOT MATERIAL —
fixed BUFFER is a different mechanism; see AUDIT §2).

This is RESEARCH ONLY. No production implementation, no public API change,
no production default change, and no runtime autotuner is authorized or
delivered by this campaign. C1 (FixedFileBinding prototype) is explicitly
out of scope even if all stop-gates pass; C0 outputs evidence + adjudication
only.

The campaign is a **falsification** campaign. The default hypotheses to
attack are:

```text
H-0  fixed-file registration has no material steady-state per-op cost
     difference vs ordinary fd under equivalent semantics
H-1  ordinary process-fd reuse changes the wrong-target identity shape
H-2  L0 frozen binding is sufficient for the current Sluice target workload;
     generation and per-request live-use are NOT required
H-3  the candidate authority split is at least PARTIALLY_SUPPORTED:
     Linux owns physical kernel-resource lifetime of bound requests; Sluice
     owns logical binding meaning (and can do so with L0/L1 discipline)
```

The experiment is designed to falsify H-2/H-3 and to measure H-0/H-1
honestly. A null/negative outcome for fixed-file performance is a valid,
successful falsification result.

## 1. Host (frozen class; facts re-captured per session into environment.json)

```text
Host-0: bare metal, x86-64 Xeon E5-2666 v3 (Haswell-EP, 10c/20t)
Fedora 44, kernel 7.1.9-200.fc44.x86_64, SATA SSD on /dev/sda3
RAM 62 GiB; /home = btrfs (compress=zstd:1, ssd, space_cache=v2);
/tmp = tmpfs (32 GiB); both TESTED FILESYSTEMS ARE PRESENT (no BLOCKED fs)
RLIMIT_MEMLOCK soft = 8 MiB (observed 2026-09-03; file registration does not
  pin memory, so the memlock boundary is NOT expected to constrain F1 —
  recorded, not relied on)
perf_event_paranoid = 2 -> ONLY userspace events qualify (instructions:u,
  cycles:u, task-clock:u). Kernel-space events NOT QUALIFIED.
io_uring_disabled = 0 (io_uring enabled); io_uring_group = -1
governor schedutil; intel_pstate no_turbo = 0; NUMA nodes = 1
```

All conclusions are **HOST-LOCAL ONLY** (RBUF-E0 convention). No
"Linux fixed files save X on all x86" claims are permitted.

## 2. Build (frozen)

```text
xmake f -m release --toolchain=clang --with-liburing=true -y
xmake build g1_control_c0_bench
bench: bench/g1_control_c0_bench.cpp (research-only direct-liburing
  mechanism bench; production code UNTOUCHED)
liburing: xrepo-pinned 2.14 (bench links the xrepo package)
bench sha256 recorded at freeze and per session in environment.json
```

## 3. Primary metric and perf event semantics (frozen, fail-closed)

```text
PRIMARY:   steady-state wall per operation (wall/op, ns)
           = measured transfer span / number of data-path ops in the span
           (spans are contiguous; lifecycle regions are separate, §8)

WHY wall/op is the ONLY metric that can capture the mechanism delta:
  the fixed-vs-ordinary difference is a KERNEL-SIDE file lookup
  (AUDIT §3/§4). perf_event_paranoid = 2 pins every perf event to :u
  (verified 2026-09-03: `perf stat -e cycles` yields cycles:u). Kernel
  lookup cost is INVISIBLE to instructions:u / cycles:u / task-clock:u.
  Therefore perf:u MUST NOT be presented as evidence of kernel lookup
  savings, and wall/op is the primary claim metric (it includes syscall +
  kernel submission + lookup + I/O + completion).

SECONDARY (report-only, userspace-only, each labeled):
  instructions:u / op      (userspace instruction count in the process,
                             NOT a kernel-lookup metric)
  cycles:u / op            (report only)
  task-clock:u / op        (report only)
  context switches         NOT QUALIFIED (attribution unreliable)

REGISTRATION COST (separate regions, never inside the measured span):
  register_ns, unregister_ns (F1 arms only) — reported, not amortized into
  steady state (§8)
```

## 4. Arms (exact semantics; the bench is the single implementation)

One shared engine: identical ring setup (entries = max(8, 2*depth),
flags = 0), identical buffers (single posix_memalign(4096) block, one
slot per concurrency unit at chunk strides), identical per-op scheduling
state machine, identical file geometry, identical submit/reap loop. The
ONLY F0→F1 delta is: file lookup mechanism.

```text
F0    ordinary fd: io_uring_prep_read/write with the real process fd;
      no IOSQE_FIXED_FILE. No registration anywhere.

F1    fixed file, L0 FROZEN binding: io_uring_register_files(ring,
      {fd}, 1) ONCE after ring init (slot 0); every op uses
      io_uring_prep_read/write with sqe->fd = 0 AND
      sqe->flags |= IOSQE_FIXED_FILE; io_uring_unregister_files ONCE at
      the end of the measured lifecycle (after all CQEs reaped).

F0-T  ordinary fd under the MATCHED threaded-process condition (§5)
F1-T  fixed file (identical to F1) under the SAME matched threaded-process
      condition
```

Confounders explicitly excluded (identical across arms by construction):
buffer, alignment, queue depth, batching, SQE preparation, completion
policy, ring flags, file layout, op count, warmup, syscall batching,
thread pinning. Causal fields are emitted by the bench and gated by the
driver (§10): `align_remainder == 0`, `slot_stride == chunk`,
`registered_files == 1` for F1/F1-T, identical `ring_entries`, identical
fixture sha.

IMPORTANT opcode fact (AUDIT §2/§5): fixed FILE I/O is `IORING_OP_READ/
WRITE` + `IOSQE_FIXED_FILE` (sqe->fd = slot). `IORING_OP_READ_FIXED/
WRITE_FIXED` is the fixed BUFFER mechanism (RBUF-E0's object) and is NOT
used here.

## 5. Threaded-process condition (frozen; matched between F0-T and F1-T)

Mechanism basis (AUDIT §3): `fget()` takes no files lock and its
RCU + atomic-refcount lookup is shape-identical for 1 vs N threads; the
documented multithread cost concern is file-ref mode switching
(percpu→atomic f_ref when a file is referenced from multiple threads) and
shared-fdtable effects, NOT the lookup itself. The threaded arms therefore
exercise exactly that: a process that has created threads sharing the file
table, with the file referenced from more than one thread.

```text
K = 4 frozen worker threads, spawned at setup (before the measured span):
  each thread opens the measured file(s) (src for READ, dst for WRITE),
  performs ONE 4 KiB read (READ cells) / write (WRITE cells) to establish
  a second file reference (exercising the file-ref mode), then parks on a
  condition variable until the main thread's transfer completes; teardown
  signals and joins. NO further I/O, NO fd churn, NO lock contention
  manufacture, NO sleep-based sequencing in the data path (condvar park +
  explicit signal).

Why this shape: it is the minimal competent "matched threaded-process
condition" — the process has multiple threads sharing `files_struct` and
the file is referenced from more than one thread — without feeding the
fixed-file arms a pathological open/close or lock-contention workload.

The untreaded arms (F0/F1) run in a single-threaded process shape. The
threaded arms are EXPLORATORY: no primary verdict depends on them; they
answer "does the documented multithread file-ref concern change the
F0-vs-F1 delta?"
```

## 6. Matrix (frozen; preregistered shrink from the Issue #279 initial
# range, decided from host facts BEFORE any formal measurement)

```text
operation: READ, WRITE
size x depth:
   4 KiB x {1, 8, 32}   (lookup-sensitive family: small I/O, per-op cost
                         visible; depth 32 maximizes lookup share)
   64 KiB x {1}         (mid: I/O share larger)
   2 MiB x {1}          (CONTROL: I/O-dominated, expected null)
filesystem: tmpfs (primary, /tmp) + btrfs (regime control, /home)
process shape: single-thread {F0, F1} + matched threaded {F0-T, F1-T}
```

Shrink rationale (recorded BEFORE results): depth is only varied for 4 KiB
because the lookup-cost question is a small-I/O question; 64 KiB / 2 MiB
are single-depth regime/control cells. Filesystem × shape complete the
regime map without re-scanning a CHUNK-E0-style surface.

Formal runs: 5 cells (per op) x 2 ops x 2 fs x 4 arms x R=7 rounds =
**560 runs**. Each run is one full pass: 4 KiB cells move 512 MiB
(131072 ops), 64 KiB / 2 MiB cells move 1 GiB (16384 / 512 ops).

No cell may be deleted after results are seen. Infeasible cells are
reported as `INFEASIBLE` / `BLOCKED` with the reason; the bench's own
registration/lifecycle failure class exits 4 (RBUF-E0 convention) and is
recorded, never aggregated over.

## 7. Workload (frozen)

```text
READ cell:  one pass of ops reading file[0..file_bytes) at size-byte
            offsets, depth-concurrent. src file is generated once per
            (fs, size) with the deterministic splitmix64 tile pattern
            (AUDIT/RBUF-E0 generator, seed 0xE1E1E1E121212121); src sha256
            recorded at generate time.
WRITE cell: one pass of ops writing the deterministic per-offset tile
            pattern to file[0..file_bytes) (dst opened O_TRUNC in setup,
            before the measured span). dst content depends only on
            (file_bytes, size) — every byte is written exactly once from a
            pattern function of its offset, independent of depth and arm —
            so the expected dst sha256 is a per-cell CONSTANT, computed at
            generate time and frozen in the manifest.
sync: none (buffered; no fsync/fdatasync anywhere; no durability claim)
```

## 8. Measured regions (frozen; never mixed)

```text
SETUP     (outside span): open files, allocate aligned block, init ring,
          spawn threads (threaded arms), register files (F1/F1-T),
          recorded as setup_ns
STEADY    measured span: the single transfer pass, transfer_ns
LIFECYCLE (outside span, separate numbers): register_ns, unregister_ns
TEARDOWN  (outside span): join threads, unregister, close, free,
          recorded as teardown_ns
```

Registration setup is NEVER inside a transfer span. No e2e amortization
number is produced unless the analysis explicitly decomposes it
(per RBUF-E0: registration cost is reported separately, never blended).

## 9. Same-work gates (fail-closed per run; any failure = INVALID run)

```text
bench exit 0; bench JSON ok:true
op count == ceil(file_bytes / size) exactly (READ: read_ops; WRITE:
write_ops)
every CQE res == requested length (short I/O recorded, never retried)
zero canceled terminals, zero error terminals, zero short I/O
state-machine validation on every CQE (slot/concurrency-unit/opcode/len)
no in-flight op at span end
READ: bytes_read == file_bytes; src file untouched; content spot-check
      (bench preads first + last 4 KiB after the span and compares to the
      tile) — both arms read the SAME src file at the SAME offsets
WRITE: bytes_written == file_bytes; dst size == file_bytes (driver);
      dst sha256 == frozen per-cell expected constant (driver, per run)
causal fields: align_remainder == 0 AND slot_stride == chunk
F1/F1-T extra: registered_files == 1; registration/unregistration success
      (exit 4 = lifecycle/capability failure class, recorded, never
      aggregated over)
threaded arms extra: K threads spawned, each performed its one I/O, all
      joined (emitted counters, driver-gated)
```

## 10. Driver-level gates (fail-closed)

```text
perf instructions:u present and > 0 (per run)
arm balance: every round contains every arm for every cell (seeded
  blocked-interleaved, RBUF-E0 run_plan)
no duplicate/missing run ids; exact cell accounting in analysis
unexpected exit -> gate error
```

## 11. Repetitions and ordering (frozen)

```text
R = 7 seeded blocked-interleaved rounds (RBUF-E0 run_plan)
SEED = 0xE1E1E1E121212121; run ids r<round>-NNNN in shuffle position
combos = (op, size, depth, fs, arm); 80 combos per round; no arm/cell is
ever measured in contiguous blocks
```

## 12. Phase Q0 — io_uring stability qualification (hard precondition)

Before any formal performance measurement: 30 runs of F0 at 4 KiB x d8 on
tmpfs, READ, full §9/§10 gates.

```text
Q0 PASS: 30/30 valid, 0 unexpected canceled, 0 teardown abort, 0 gate
         errors -> single-worker direct-liburing path QUALIFIED
Q0 FAIL: any unexpected canceled / teardown abort / semantic mismatch ->
         C0-PERF STOPPED; evidence goes to #262 (still OPEN); no
         performance claims
```

Q0 PASS does NOT close #262 (RBUF-E0 convention).

## 13. Materiality and decision rules (frozen; no tuning after formal
# measurement starts)

Per cell, per direction, on `wall/op` medians over the 7 rounds:

```text
ratio = median(F0 wall/op) / median(F1 wall/op)   (F1 faster if > 1)
MATERIAL := ratio >= 1.03 AND median(F1) + 1.5*MAD(F1)
                            < median(F0) - 1.5*MAD(F0)
REGRESSION := ratio <= 1/1.03 AND median(F0) + 1.5*MAD(F0)
                            < median(F1) - 1.5*MAD(F1)
```

```text
PRIMARY CELLS: the 4 KiB family on tmpfs, READ and WRITE independently
NEIGHBOR CONSISTENCY: a stable fixed-file effect requires the primary
  direction to hold on >= 1 neighbor cell in the same direction
  (neighbors: the other 4 KiB depths; 64 KiB; the same cell on btrfs).
  Otherwise: ISOLATED CELL ONLY.
```

Threaded arms: same rule applied to F0-T vs F1-T; they are exploratory
and cannot by themselves carry a verdict.

Secondary (userspace-only) metrics are REPORTED per cell with the
`USERSpace-only, NOT kernel-lookup evidence` label; they never decide a
verdict.

### 13.1 C0-PERF verdict vocabulary (one per campaign)

```text
FIXED-FILE PERFORMANCE BENEFIT ESTABLISHED     (primary direction material
                                                + neighbor support)
REGIME-LOCAL BENEFIT ESTABLISHED               (material in an isolated
                                                primary/regime cell only)
FIXED-FILE PERFORMANCE REGRESSION              (REGRESSION material +
                                                neighbor support)
FIXED-FILE PERFORMANCE BENEFIT NOT ESTABLISHED (no material effect)
BLOCKED / INVALID
```

## 14. FILE-ID-E0 — deterministic identity witness (not a benchmark)

Two files with clearly distinct deterministic markers (pattern `A` and
pattern `B`, both splitmix64-derived with distinct seeds). Both arms use
`dup2`-forced fd reuse — NO sleep, NO probabilistic `open()==N` loop.

### 14.1 Ordinary-fd arm

```text
open A -> fd N (record N)
open B -> fd M
close(N)
dup2(M, N)   // N now refers deterministically to B's file object
read via stale N (ordinary, no fixed flag) -> must return B's marker
```

Witness shape: same numeric fd N, different file object, stale logical
identity, wrong-target operation. Expected: ORDINARY-FD WRONG-TARGET
REPRODUCED.

### 14.2 Fixed-file L0 arm

```text
open A -> fd N
register N into fixed slot S (io_uring_register_files)
close(N)
open B -> fd M; dup2(M, N)   // process fd N now refers to B
submit IOSQE_FIXED_FILE read with fd = S (slot)
-> must return A's marker (frozen binding survived process-fd reuse)
then io_uring_register_files_update(S <- B); submit fixed read with
fd = S -> must return B's marker (replacement is honored going forward)
```

Expected: FIXED L0 BINDING PRESERVED TARGET. The witness proves:
`ordinary process fd reuse != fixed-table binding identity change`
(AUDIT §6). It does NOT prove the slot is an eternal identity — only that
the frozen binding survives ordinary process-fd reuse.

### 14.3 Validation→submission→binding window (AUDIT §6, boundaries A/D)

```text
BOUNDARY A (deterministic, userspace-controlled): register A into slot S;
prepare a fixed-read SQE on slot S but DO NOT submit; update slot S <- B;
submit the prepared SQE; result must be B's marker (the kernel binds at
issue time from the CURRENT table; AUDIT §6 M-D). PROVES the
validation->submission->binding window is real and not closed by the
kernel for unbound requests.

BOUNDARY D (deterministic): register A into slot S; submit fixed read on
slot S; reap CQE (binds A, holds node ref); update slot S <- B; the
completed op returned A's marker (kernel request-side retention; the
update cannot yank an already-bound resource).

BOUNDARY B/C (submitted-but-not-issued / consumed-but-not-bound): NOT
DETERMINISTICALLY OBSERVABLE from userspace (AUDIT §6) — recorded as such,
with the bounded source-based conclusion.
```

### 14.4 FILE-ID-E0 verdict vocabulary

```text
ORDINARY-FD WRONG-TARGET REPRODUCED
FIXED L0 BINDING PRESERVED TARGET
FIXED L0 BINDING DID NOT PRESERVE TARGET
BOUNDARY-A WINDOW CONFIRMED (userspace-prepared SQE binds post-update)
BOUNDARY-D RETENTION CONFIRMED (bound request keeps old target)
BLOCKED / INVALID
```

No vague PASS. This experiment is deterministic: each arm has a single
possible correct outcome; a deviation is INVALID.

## 15. C0-MINIMALITY (frozen questions; analysis-only, from workload +
# mechanism + witness evidence)

```text
Q1  Does the CURRENT Sluice target workload require runtime fixed-slot
    replacement?  (from current architecture, APIs, roadmap, intended
    capability use — AUDIT §1: zero fixed-file support, DIV-09 deferred,
    no roadmap item)
    answers: REQUIRED / USEFUL-BUT-NOT-REQUIRED / NO CURRENT REQUIREMENT /
    UNKNOWN

Q2  L0 falsification: is there a current target workload that cannot be
    served by register -> run -> quiesce -> unregister without runtime
    replace?  If no: L0 SUFFICIENT / GENERATION NOT EARNED.

Q3  If dynamic replacement were required, is L1 (quiescent replacement)
    expressible with EXISTING lifecycle authority (request capacity,
    drain, reap — "no accepted request referencing the slot is
    outstanding")?  If yes: L1 SUFFICIENT / GENERATION NOT EARNED.

Q4  Is a per-request live-use counter earned?  Only if: Sluice logical
    validation says A, a LEGAL concurrent replace A->B exists, the same
    accepted request binds B, AND L0 cannot avoid it AND L1 discipline
    cannot avoid it AND existing request lifecycle authority cannot
    express the exclusion.  Otherwise: LIVE-USE / LEASE NOT EARNED.

Q5  Is generation (L2) earned?  Only if the required API permits a stale
    logical handle (slot, generation) to survive replacement.  If the API
    can specify "replacement invalidates all old handles AND replacement
    only under quiescence": GENERATION NOT EARNED.

Q6  Competent baseline: what does raw liburing + a thin (20-line)
    quiescence discipline achieve that Sluice's explicit binding does
    not?  If nothing material: NO STRONG SLUICE-SPECIFIC CONTROL VALUE.

Q7  Hot-path control tax (mechanism budget, per candidate L0/L1/L2):
    steady-state extra registry lookup / lock / allocation / refcount /
    generation check — each must be named or marked ABSENT.
```

Verdict vocabulary (per issue #279):

```text
L0 SUFFICIENT | L1 REQUIRED / L1 SUFFICIENT | L2 GENERATION EARNED |
GENERATION NOT EARNED | PER-REQUEST LIVE-USE EARNED |
PER-REQUEST LIVE-USE NOT EARNED | UNRESOLVED
```

## 16. Amendments

```text
(none — to be appended ONLY as additive, timestamped entries after freeze)
```

AMENDMENT-1 (2026-09-03, Corrective-1 P1-1 — threaded-condition mismatch):
session g1-control-c0-native-1 did NOT execute the frozen §5 threaded
condition. The bench spawned the K=4 workers, let each perform its one
setup I/O, and JOINED them BEFORE the measured span (spawn -> I/O ->
join -> span), instead of parking them across the span (§5: park until
the main thread's transfer completes). The native-1 threaded subset
(F0-T/F1-T) is SUPERSEDED for every threaded claim; it is retained
byte-identical as historical evidence. Corrective-1 re-executes exactly
the affected cells (2 ops x 5 cells x 2 fs x 2 threaded arms x R=7 =
280 runs) in session g1-control-c0-native-2-threaded-corrective with a
deterministic mutex/condvar park-release gate (no sleeps/yields: workers
ready before the span, released only after it; emitted gate fields
threads_ready / threads_released / thread_gate_ready /
thread_gate_release_after_transfer, driver- and validator-gated), same
seeded-interleaved plan, same frozen thresholds and materiality.
Single-thread arms (F0/F1) are unaffected; their native-1 evidence
stands.

AMENDMENT-2 (2026-09-03, Corrective-1 P1-2 — BOUNDARY-D claim withdrawn):
the executed §14.3 BOUNDARY-D topology submits the fixed read, REAPS its
CQE, and only then updates the slot. The request is complete before the
update, so the executed step cannot witness in-flight retention. The
executed claim "BOUNDARY-D RETENTION CONFIRMED" is WITHDRAWN; the step is
reclassified as a POST-COMPLETION UPDATE CONTROL (non-overlap; not
load-bearing). Already-bound-resource retention remains SOURCE-SUPPORTED,
VERSION-BOUND (AUDIT §6: request-side node->refs retention,
io_req_put_rsrc_nodes, fput at refs==0). No replacement overlap
experiment is added.

AMENDMENT-3 (2026-09-03, Corrective-1 — substrate-label disclosure):
environment.json of g1-control-c0-native-1 records BOTH §6 filesystem
labels ("tmpfs", "btrfs") resolving to the SAME btrfs substrate
(/home, zstd:1, page-cache): the "tmpfs (primary, /tmp)" intent was not
met by the executed session. The F0-vs-F1 causal comparison is unaffected
(identical substrate within every arm pair; the only delta remains the
file-lookup mechanism) and the substrate-share bias runs AGAINST F1
(harder materiality), so the executed verdicts are not endangered by
this defect. What is withdrawn: REGIME language — "tmpfs vs btrfs regime
control" did not vary the regime; cross-label agreement is
same-substrate replication. Corrective-1 keeps the frozen directory
labels for run-id continuity, records the resolved substrate per session
(manifest substrate_fstypes), and executes the corrective threaded cells
on the SAME substrate as native-1 for comparability.

AMENDMENT-4 (2026-09-03, Corrective-1 — interpretation split, §14.3
BOUNDARY-A): the executed witness proves only that a
prepared-but-unsubmitted SQE observed the POST-update binding
(PRE-SUBMISSION FIXED BINDING NOT FROZEN). That binding occurs at the
kernel issue path (io_assign_file / io_file_get_fixed) is
SOURCE-SUPPORTED, VERSION-BOUND — an inference from source, not an
executed fact. Boundaries B/C remain NOT DETERMINISTICALLY OBSERVABLE
from userspace.
