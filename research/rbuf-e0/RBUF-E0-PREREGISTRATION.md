# RBUF-E0 PREREGISTRATION — io_uring registered/fixed-buffer steady-state
# and amortization crossover (#272)

Status: **FROZEN** before formal measurement. Only additive amendments are
permitted after formal measurement begins (each logged in §13 with a reason
and a timestamp). Governing issue: #272. Audit:
`research/rbuf-e0/RBUF-E0-AUDIT.md`. Governing branches: #259 (roadmap),
#262 (uring correctness — NOT closed by this campaign), #270 (chunk
baseline), PR #271 (Host-0 reference).

This is RESEARCH ONLY. No production implementation, no public API change,
no production default change, and no runtime autotuner is authorized or
delivered by this campaign.

## 1. Host (frozen class; facts re-captured per session into environment.json)

```text
Host-0: bare metal, x86-64 Xeon E5-2666 v3 (Haswell-EP)
Fedora 44, kernel 7.1.9, btrfs/zstd, SATA SSD
workers = 1 (single submission/completion thread inside the bench)
RLIMIT_MEMLOCK soft = 8 MiB (observed 2026-09-02; re-observed per session)
```

All conclusions are **HOST-LOCAL ONLY**. No "Linux fixed buffers save X" or
"x86 fixed-buffer improvement" claims are permitted.

## 2. Build (frozen)

```text
xmake f -m release --toolchain=clang --with-liburing=true -y
xmake build rbuf_e0_bench
bench: bench/rbuf_e0_bench.cpp (research-only direct-liburing mechanism bench)
liburing: xrepo-pinned 2.14 for the bench target (system pkg-config reports
2.13; the bench links the xrepo package, recorded via bench binary sha256)
bench sha256 at freeze: 7649fcebede8ec382845b47e18ecf58973a25bbd3cab9b0e784cd0650105c09f
production code: UNTOUCHED (zero production source changes in this campaign)
```

## 3. Arms (exact semantics; the bench is the single implementation)

```text
U0  ordinary-natural reference
    per-slot std::vector<std::byte> heap buffers (natural policy, no
    explicit alignment), ordinary IORING_OP_READ/WRITE.
    CONTEXT ONLY — never part of a causal registration claim.

U1  causal ordinary control
    ONE posix_memalign(4096, chunk*depth) block; depth slots of `chunk`
    bytes at chunk strides; ordinary READ/WRITE; buffers allocated ONCE and
    reused across all transfers of the run.

U2  registered/fixed treatment
    byte-identical storage construction as U1, plus
    io_uring_register_buffers(one iovec per slot) issued ONCE after ring
    init; io_uring_prep_read_fixed/write_fixed with buf_index = slot;
    io_uring_unregister_buffers ONCE at lifecycle end (after all CQEs are
    reaped, before storage free).
```

Shared by all arms (single code path): ring entries = max(8, 2*depth) with
flags=0, identical fixture, identical file offsets, identical strided slot
state machine (slot s handles chunks s, s+depth, ...; read chunk -> write
chunk -> next), identical submit/reap loop (batch prepare ->
io_uring_submit -> wait_cqe + peek drain), identical process lifecycle.
**The ONLY U1->U2 delta is registration + fixed opcode selection.**

## 4. Workload (frozen)

```text
buffered READ + WRITE copy, 1 GiB useful bytes per transfer
fixture: CHUNK-E0 canonical splitmix64 tile, seed 0xE1E1E1E121212121
src sha256 = 8a3c4bf01ec3d32c0da34e9ed93a091bfaedfc48e39dad5bcd6c8b1bf548fd53
           (byte-identical to research/chunk-e0 sessions — verified at
           generate time)
sync policy: none (page-cache buffered; no fsync/fdatasync anywhere)
dst: rewritten in place from offset 0 on every transfer (no re-open);
     final dst sha256 must equal src sha256 (gate)
no cache dropping, no ionice/cgroup isolation, no governor changes
```

## 5. Chunk/depth matrix (frozen; NOT a re-scan of CHUNK-E0)

```text
cells: 512K x d2, 1M x d2, 2M x d1, 2M x d2 (PRIMARY), 2M x d4, 4M x d2
depth 8 is excluded (off the Host-0 frozen Pareto frontier in CHUNK-E0 H0;
#262 lifecycle uncertainty; not needed to answer registration causality)
```

### REGISTRATION-INFEASIBLE cells (resource capability boundary)

With RLIMIT_MEMLOCK soft = 8 MiB (observed), the U2 registration for
`2M x d4` and `4M x d2` (= 8 MiB, exactly at the limit) fails with ENOMEM
(behaviorally verified 2026-09-02, errno recorded). Frozen handling: the
driver marks these cells U2-INFEASIBLE **before** measurement from the
probe's observed limit; U2 is not attempted there; U0/U1 still run (context).
These cells are reported as an observed resource capability boundary — NOT
gate errors, NOT anomalies, and NOT silently dropped. If the observed memlock
limit at session time differs, feasibility is recomputed from the fresh
observation by the same rule (`registered_bytes < memlock_soft`).

## 6. Reuse horizons (frozen)

```text
H1 = 1 transfer, H4 = 4, H16 = 16, H64 = 64
cell: 2M x d2 (PRIMARY)
U1: allocate once, reuse ordinary buffers across H transfers, destroy
U2: allocate once, register once, reuse fixed buffers across H transfers,
    unregister once, destroy
every transfer moves the same useful bytes (1 GiB)
no smoke-based reduction — H64 runtime accepted (~80 s/run)
```

## 7. Repetitions and ordering (frozen)

```text
R = 7 seeded blocked-interleaved rounds per experiment
each round shuffles ALL combos with random.Random(SEED + round),
SEED = 0xE1E1E1E121212121; run ids r<round>-NNNN in shuffle position
steady-state combos: cells x arms (U2 minus infeasible cells)
amortization combos: {1,4,16,64} x {U1,U2}
no arm/cell is ever measured in contiguous blocks
```

Formal run counts: steady 7 x (6 cells x 2 arms + 4 feasible U2) = 7 x 16 =
112 runs; amortization 7 x 8 = 56 runs; Q0 50 runs (below).

## 8. Metrics (frozen)

```text
PRIMARY steady-state: wall = per-transfer span ns (bench transfer_ns[0])
  (throughput MiB/s derived; CHUNK-E0 convention: useful bytes / span)
LIFECYCLE (separate regions, never inside a transfer span):
  alloc_ns, register_ns (U2), unregister_ns (U2), setup_ns, teardown_ns
AMORTIZATION: end_to_end(H) = setup + sum(transfer spans) + teardown
  amortized_per_transfer = end_to_end / H; setup_fraction
SECONDARY: instructions:u / byte (perf stat, user-space), cycles:u (report
  only; DEMOTED unless the session probe shows no negative consecutive
  per-op double-difference), CPU user+sys (getrusage), peak RSS, minor
  faults, context-switch metrics NOT collected (unreliable)
REGISTERED-MEMORY ACCOUNTING: registered_bytes = chunk*depth (OBSERVED,
bench-emitted), RLIMIT_MEMLOCK soft/hard (OBSERVED), VmLck NOT OBSERVABLE
per-process reliably -> any pinned-bytes quantity beyond the registered
iovec lengths is INFERRED and forbidden in quantitative claims
```

## 9. Same-work gates (fail-closed per run; any failure = INVALID run)

```text
bench exit 0; bench JSON ok:true
cqe_count == read_ops + write_ops == 2 * transfers * ceil(bytes/chunk)
every CQE res == requested length (short I/O recorded, never retried)
zero canceled terminals, zero error terminals, zero short I/O
state-machine validation on every CQE (slot/opcode/length)
no in-flight op at transfer end
bytes_read == bytes_written == transfers * file size
dst sha256 == src sha256 (driver-side, per run)
perf instructions:u present and > 0 (driver-side)
U1/U2 causal fields: align_remainder == 0 AND slot_stride == chunk
U2 extra: registered_buffers == depth AND registered_bytes == chunk*depth;
   registration/unregistration success (bench exit 4 = capability/lifecycle
   failure class, recorded, never aggregated over)
```

## 10. Decision rules (frozen; no tuning after formal measurement starts)

### 10.1 Steady-state materiality (U1 vs U2, per cell, on wall medians)

```text
ratio = median(U1 wall) / median(U2 wall)
MATERIAL  := ratio >= 1.03  AND  median(U2) + 1.5*MAD(U2)
                             <  median(U1) - 1.5*MAD(U1)
```

Throughput and instructions/byte ratios are REPORTED; the primary verdict is
decided by the frozen wall rule only.

### 10.2 Neighbor consistency

```text
stable registration regime requires:
  PRIMARY 2M x d2 material
  AND >= 1 neighbor cell material in the same direction
      (neighbors: 1M x d2, 2M x d1, 2M x d4*, 4M x d2*;  * = U2-infeasible
       under §5 and usable as neighbors only if feasibility changes)
otherwise: ISOLATED CELL ONLY
```

### 10.3 Amortization crossover (primary cell, end-to-end)

```text
per horizon H: MATERIAL(H) := same 1.03 + 1.5*MAD rule applied to
               median(U2 end_to_end(H)) vs median(U1 end_to_end(H))
CROSSOVER := smallest tested H with MATERIAL(H)
if no tested H qualifies:
  AMORTIZATION CROSSOVER NOT LOCATED IN TESTED RANGE (never extrapolate)
if U2 end-to-end never even reaches parity:
  REGISTRATION NEVER RECOVERS SETUP COST IN TESTED RANGE
```

### 10.4 U0 interpretation

U0 vs U1 may illustrate allocation/reuse/storage policy effects. It NEVER
enters a causal registration statement. Causal registration statements come
from U1 vs U2 only.

## 11. Phase Q0 — io_uring stability qualification (hard precondition)

Before any formal performance measurement: 50 runs of U1 at 2M x d2,
1 GiB copy, workers=1, full §9 gates.

```text
Q0 PASS: 50/50 valid, 0 unexpected canceled, 0 teardown abort,
         0 corruption  -> single-worker uring path QUALIFIED for RBUF-E0
Q0 FAIL: any unexpected canceled / teardown abort / semantic mismatch /
         hash mismatch -> RBUF-E0 STOPPED; #262 becomes blocking;
         full reproduction evidence goes to #262; no performance claims
```

Q0 PASS does NOT close #262; it only records that #262 did not reproduce in
this restricted regime (single worker, no cancel API, direct liburing).

## 12. Verdict vocabulary (final primary verdicts, one each)

```text
steady-state: REGISTERED BUFFER STEADY-STATE MATERIAL
            | REGISTERED BUFFER REGIME-SPECIFIC
            | REGISTERED BUFFER STEADY-STATE NOT MATERIAL
            | REGISTERED BUFFER MIXED / UNSTABLE
            | URING QUALIFICATION FAILED — #262 BLOCKING
amortization: AMORTIZATION CROSSOVER LOCATED @ H=<...>
            | AMORTIZATION CROSSOVER NOT LOCATED IN TESTED RANGE
            | REGISTRATION NEVER RECOVERS SETUP COST IN TESTED RANGE
production:   REGISTERED-BUFFER PRODUCTION IMPLEMENTATION: NO
              PUBLIC API: NO;  RUNTIME POLICY: NO
```

Promotion gate (out of scope here): a future RBUF-P1 design issue requires
steady-state material + neighbor consistency + an amortization case +
understood lifecycle/resource cost. A single-host win NEVER promotes.

## 13. Amendments

```text
(none — to be appended ONLY as additive, timestamped entries after freeze)
```
