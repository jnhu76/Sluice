# RBUF-E0 REPORT — io_uring registered/fixed-buffer steady-state and
# amortization crossover (#272)

Campaign: RBUF-E0, Host-0 bare metal, 2026-09-02.
Preregistration: `RBUF-E0-PREREGISTRATION.md` (FROZEN before formal
measurement; no amendments). Audit: `RBUF-E0-AUDIT.md`.
Instrument: `bench/rbuf_e0_bench.cpp` (research-only direct-liburing
mechanism bench; production code UNTOUCHED — zero production source changes).

```text
HOST: bare metal, x86-64 Xeon E5-2666 v3 (Haswell-EP)
      Fedora 44, kernel 7.1.9-200.fc44.x86_64, btrfs/zstd, SATA SSD
      RLIMIT_MEMLOCK soft = 8 MiB (observed, probe + getrlimit)
      liburing (xrepo, linked into bench): 2.14; system pkg-config: 2.13
      workers = 1 (single submission/completion thread)
      bench sha256: 7649fcebede8ec382845b47e18ecf58973a25bbd3cab9b0e784cd0650105c09f
      fixture sha256: 8a3c4bf01ec3d32c0da34e9ed93a091bfaedfc48e39dad5bcd6c8b1bf548fd53
      (byte-identical to the CHUNK-E0 canonical fixture)

Q0 URING QUALIFICATION (results/rbuf-e0-q0-uring-stability-native-1):
runs: 50/50 valid
unexpected canceled: 0
teardown abort: 0
hash failures: 0
gate errors: 0
VERDICT: Q0 PASS — single-worker direct-liburing path QUALIFIED for RBUF-E0.
This does NOT close #262: #262 did not reproduce in this restricted regime.
```

==================================================
STEADY STATE
==================================================

Session `rbuf-e0-steady-native-1`: 112 runs (7 seeded-interleaved rounds x
16 combos), 0 gate errors. Wall = per-transfer span (1 GiB useful bytes;
throughput in MiB/s). Materiality (frozen): ratio >= 1.03 AND 1.5*MAD
separation.

```text
cell        U0       U1       U2       U1/U2   MAD-sep  MATERIAL
512K x d2   2104.5   2159.2   2121.8   0.9827  no       no
1M   x d2   2073.0   2169.5   2108.2   0.9717  no       no
2M   x d1    827.6    890.7    778.0   0.8735  no       no
2M   x d2   2100.8   2056.6   2140.0   1.0405  no       no   (PRIMARY)
2M   x d4   2134.4   2087.6   INFEASIBLE (registration > memlock)
4M   x d2   2121.5   2101.6   INFEASIBLE (registration > memlock)
```

PRIMARY 2M x d2: U2 median is 4.05% faster, but the U1/U2 distributions
overlap (median(U2)+1.5*MAD(U2) is NOT below median(U1)-1.5*MAD(U1)) ->
not material under the frozen rule.

Direction across cells is MIXED: U2 is slower in 3 of the 4 feasible cells
(2M x d1 shows U2 12.7% slower; the depth-starved cell has the least
overlap to hide submission-path differences) and nominally faster only at
the primary cell, without separation.

instructions/byte (instructions:u, user-space): essentially identical —
ratio U1/U2 = 1.000 at 512K x d2, 1M x d2, 2M x d1; 0.9655 at the primary
(U2 ~3.5% fewer user instructions, same wall overlap). cycles:u was
collected but stays DEMOTED per prereg §8 (no stability probe qualified it
this session).

Neighbor consistency: NOT satisfied (no neighbor is material).

STEADY-STATE VERDICT: **REGISTERED BUFFER STEADY-STATE NOT MATERIAL**
(frozen rule). Secondary observation: the sign of the (sub-material) effect
flips across cells — registration is not a uniform win OR loss here; it is
noise-adjacent everywhere on this host.

==================================================
LIFECYCLE
==================================================

```text
allocation (U1/U2 posix_memalign 4 MiB block):   ~27-30 us  (OBSERVED)
registration (io_uring_register_buffers, 4 MiB): 1.086 ms median (OBSERVED)
unregistration (unregister_buffers):            65.2 us median (OBSERVED)
registered bytes (PRIMARY): 4 MiB = 2 iovecs x 2 MiB (OBSERVED, bench-emitted)
resource accounting: RLIMIT_MEMLOCK soft = 8 MiB (OBSERVED);
  per-process pinned bytes beyond the registered iovec lengths:
  NOT OBSERVABLE (no invented quantities; INFERRED values are forbidden)
```

Capability boundary (OBSERVED, errno recorded): registering exactly 8 MiB
(`2M x d4`, `4M x d2`) fails with ENOMEM under the 8 MiB memlock soft
limit. Those U2 cells are REGISTRATION-INFEASIBLE on Host-0 as-configured
and were excluded BEFORE measurement per prereg §5. No ulimit/sysctl was
adjusted. This boundary is itself a reportable resource cost of the
registration capability: pinning grows with in-flight bytes and ends at the
memlock wall.

Session-level filesystem observation (OBSERVED, mechanism NOT attributed):
in the amortization session, the bench's teardown region (close(dst) +
ring exit) took 2.5-14.4 s depending on the dirty page-cache debt left by
predecessor runs (H up to 64 x 1 GiB buffered writes), vs ~0.26 s in the
steady session (uniform 1 GiB predecessors). This is arm-independent
environment cost; it never enters a steady-state span.

==================================================
AMORTIZATION
==================================================

Session `rbuf-e0-amortization-native-1`: 56 runs (7 rounds x {1,4,16,64} x
{U1,U2}), 2M x d2, seeded-interleaved, 0 gate errors. End-to-end =
setup + sum(transfer spans) + teardown, measured inside the bench process.

```text
H     U1 e2e (s)   U2 e2e (s)   ratio   MAD-sep   material
1     4.5754       4.3620       1.0489  no        no
4     5.7363       6.0764       0.9440  no        no
16    13.3698      10.8649      1.2305  no        no
64    39.9559      47.2087      0.8464  no        no
setup fraction (U2): 89.3% (H=1) -> 66.6% (H=4) -> 34.9% (H=16) ->
7.9% (H=64) — but the numerator/denominator are dominated by the
environmental teardown stall documented above, not by registration.
```

The end-to-end signal is swamped: the registration lifecycle (~1.15 ms total)
is ~4 orders of magnitude below the inter-run filesystem noise (seconds),
and the direction of the (noise-level) U1/U2 difference flips between
horizons. Under the frozen rule no tested horizon is material.

CROSSOVER: **AMORTIZATION CROSSOVER NOT LOCATED IN TESTED RANGE**

Decomposition (context, same session, clean transfer spans only):
registration setup+teardown (~1.15 ms) is ~0.25% of a SINGLE 0.46 s
transfer. Therefore the amortization question collapses onto the
steady-state question: any REAL steady-state benefit larger than ~0.25%
would pay for registration within the first transfer (H=1). Since the
frozen steady-state rule finds no material benefit, there is no benefit for
any reuse horizon to amortize. The negative is a valid closure.

==================================================
CPU
==================================================

```text
instructions/byte (steady, median): U1 == U2 within rounding at 512K x d2,
1M x d2, 2M x d1; U2 ~3.5% fewer at 2M x d2 (0.9655 ratio).
instructions/op: not separately reported — op counts are identical and
gated (2 * ceil(bytes/chunk) per transfer), so instructions/byte is the
same statistic up to the gated constant.
CPU user+sys (getrusage): moves with wall (U2 lower at primary, higher
elsewhere); not separable from noise under the same overlap.
```

==================================================
CAUSAL ISOLATION
==================================================

```text
same allocation primitive:  PASS (U1/U2 single posix_memalign(4096) block)
same allocation size:       PASS (chunk*depth, one block)
same alignment:             PASS (gated: align_remainder == 0, both arms)
same slot count/stride:     PASS (gated: slot_stride == chunk)
same buffer lifetime:       PASS (allocated once, reused across transfers;
                            never resized/re-faulted)
same request order:         PASS (one shared strided slot state machine)
same chunk/depth:           PASS (frozen matrix)
same queue setup:           PASS (ring_entries_requested/actual recorded
                            per run; identical 8/8 for every run, all arms)
same workload:              PASS (fixture sha + dst sha gates per run)
same completion loop:       PASS (single submit/reap code path)
treatment:                  registration + fixed opcode selection ONLY
                            (register_buffers + READ_FIXED/WRITE_FIXED
                            with buf_index = slot; unregister at end)
```

U0 is contextual only (natural heap allocation, unaligned): it shows the
aligned-reuse storage policy itself is worth ~0-3% vs natural allocation
in most cells — an allocation/reuse effect, explicitly NOT a registration
claim.

Adversarial self-review (prereg-mandated): none of the ten review questions
surfaced a confound (storage primitive/size/alignment/stride gated equal;
allocation once per process in both arms; setup charged in lifecycle and
amortization accounting; same useful bytes and hashes gated per run; same
ring geometry recorded per run; zero gate errors so no selective run
removal; zero canceled/teardown anomalies in all 218 runs).

==================================================
CLAIM BOUNDARY
==================================================

```text
HOST-LOCAL ONLY
```

Nothing here generalizes to "Linux fixed buffers", other hosts, other
kernels, other filesystems, other storage, other depths, or multi-worker.
The Host-0 memlock boundary is host configuration, not a protocol constant.

==================================================
PRODUCTION
==================================================

```text
REGISTERED-BUFFER PRODUCTION IMPLEMENTATION: NO
PUBLIC API: NO
RUNTIME POLICY / AUTOTUNER: NO
```

Promotion gate outcome (prereg §12): steady-state material — NO;
neighbor consistency — NO; amortization case — NOT LOCATED;
lifecycle/resource cost — understood (1.086 ms register / 65 us unregister
per 4 MiB; memlock-bounded). An RBUF-P1 production design issue is NOT
justified by this evidence. #262 remains open and unaffected.

==================================================
FINAL
==================================================

```text
STEADY-STATE VERDICT:  REGISTERED BUFFER STEADY-STATE NOT MATERIAL
AMORTIZATION VERDICT:  AMORTIZATION CROSSOVER NOT LOCATED IN TESTED RANGE
                       (registration lifecycle ~1.15 ms per 4 MiB lifecycle
                       is negligible against any single transfer; with no
                       steady-state benefit there is nothing to amortize)

NEXT: adversarial review
```
