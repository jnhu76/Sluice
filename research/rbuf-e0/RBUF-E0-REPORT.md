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
2M   x d4   2134.4   2087.6   INFEASIBLE (8 MiB registered-iovec request
                                          ENOMEM under observed 8 MiB
                                          soft RLIMIT_MEMLOCK)
4M   x d2   2121.5   2101.6   INFEASIBLE (same resource boundary)
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
(frozen rule). Scope: HOST-LOCAL, tested feasible regime, frozen rule.
NOT MATERIAL is NOT "no effect exists": the supported statement is that
**no robust material steady-state advantage for registered/fixed buffers
was established in the tested feasible Host-0 regime.** Secondary
observation: the sign of the (sub-material) effect flips across cells —
registration is not a uniform win OR loss here; it is noise-adjacent
everywhere on this host.

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

Capability boundary (OBSERVED, errno recorded): an 8 MiB
registered-iovec request (`2M x d4`, `4M x d2`) was infeasible under the
observed 8 MiB soft RLIMIT_MEMLOCK configuration and failed with ENOMEM.
Those U2 cells are REGISTRATION-INFEASIBLE on Host-0 as-configured and were
excluded BEFORE measurement per prereg §5. No ulimit/sysctl was adjusted.
The kernel's exact accounting/pinning overhead beyond the requested iovec
lengths was NOT observed, so this is recorded as a HOST-LOCAL RESOURCE
CAPABILITY BOUNDARY (requested size vs observed soft limit) — not as a
claim that "registration exceeds memlock", and not a protocol, Linux, or
fixed-buffer limit. The request itself (8 MiB) equaled the soft limit; the
failure shows exactly-at-limit registration does not succeed in this
configuration. Pinning grows with in-flight bytes and ends at this wall.

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
setup+teardown fraction (U2, field setup_plus_teardown_fraction):
89.3% (H=1) -> 66.6% (H=4) -> 34.9% (H=16) -> 7.9% (H=64)
```

What the fraction measures (renamed during adversarial-review remediation;
the formula is unchanged): `setup_plus_teardown_fraction` = median(setup_ns
+ teardown_ns) / median(end-to-end span). The ~89% at H=1 is the share of
the measured end-to-end SPAN occupied by the setup+teardown REGION — it is
NOT a registration cost. The dominant anomaly inside that region is the
arm-independent filesystem/dirty-page/close teardown stall documented under
LIFECYCLE (2.5-14.4 s depending on dirty page-cache debt left by
predecessor runs). The registration lifecycle itself is ~1.086 ms register
+ ~65 us unregister (4 MiB / 2-iovec treatment) — about four orders of
magnitude below the seconds-scale region noise. No statement in this report
may be read as "registration occupies 89%", "registration dominates H1",
or "registration setup is 89%".

CROSSOVER: **AMORTIZATION CROSSOVER NOT LOCATED IN TESTED RANGE**

Authorized claim (evidence-exact): the tested reuse horizons establish no
robust end-to-end amortization crossover under the frozen materiality rule.
The end-to-end amortization measurement is under-resolved for a ~1.15 ms
registration lifecycle cost because seconds-scale filesystem/dirty-page
teardown variation dominates the non-steady region — the noise-level U1/U2
end-to-end difference flips direction between horizons (1.05, 0.94, 1.23,
0.85) with no MAD separation at any horizon. Since RBUF-E0 also establishes
no robust steady-state registration benefit under its frozen rule, this
campaign provides NO EVIDENCE-BACKED amortization case for production
promotion. That is "no evidence-backed benefit", NOT "true benefit is
zero".

Decomposition (context, same session, clean transfer spans only): the
registration lifecycle (~1.15 ms) is ~0.25% of a SINGLE 0.46 s transfer.
A robust steady-state benefit above that scale would amortize registration
within the first transfer (H=1); the frozen steady-state rule establishes
no such robust benefit, and the end-to-end measurement cannot resolve the
millisecond-scale lifecycle against the seconds-scale teardown noise.

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

Remediation re-audit (adversarial review, post-measurement): mechanically
re-verified from raw runs.jsonl by
`scripts/check_rbuf_e0_analysis.py` — all 112 steady runs and all 56
amortization runs gate-clean; in every U1-vs-U2 comparison group (4 steady
cells + all 4 amortization horizons) both arms are identical on
align_remainder (0), slot_stride (== chunk), ring_entries
requested/actual, chunks_per_transfer, read/write/CQE counts and byte
totals, and only U2 carries registered_buffers/registered_bytes (plus the
fixed opcode selection). Isolation holds.

Adversarial self-review (prereg-mandated): none of the ten review questions
surfaced a confound (storage primitive/size/alignment/stride gated equal;
allocation once per process in both arms; setup charged in lifecycle and
amortization accounting; same useful bytes and hashes gated per run; same
ring geometry recorded per run; zero gate errors so no selective run
removal; zero canceled/teardown anomalies in all 218 runs).

==================================================
POST-MEASUREMENT PROBE VALIDATION (adversarial-review remediation)
==================================================

The capability probe in `bench/rbuf_e0_bench.cpp` originally submitted
READ_FIXED and WRITE_FIXED together in one io_uring_submit, so probe
results could depend on kernel-side op ordering (an ordering race for
future foreign kernels / ARM hosts; NOT a formal-performance-evidence
problem — the formal --run path never used the probe). Remediation: the
probe is now strictly serial (submit READ_FIXED -> wait CQE -> validate
length + buffer content -> submit WRITE_FIXED -> wait CQE -> validate ->
pread destination -> validate content). No IOSQE_IO_LINK, no chaining.

```text
formal benchmark rerun:            NO (forbidden by remediation scope)
formal raw evidence modified:      NO (hash audit below)
formal --run C++ path changed:     NO (diff confined to run_probe() +
                                    its header comment)
re-run performed:                  capability probe ONLY
probe session:                     results/rbuf-e0-probe-native-2 (new,
                                   post-measurement validation artifact;
                                   NOT part of any formal session)
probe binary:                      rebuilt bench, sha256
                                   9494dd48baaf3cf64feb1b035e71fa72e5cb
                                   9b5c826548c0a019bd29989dfd49
                                   (formal sessions recorded the frozen
                                   7649fceb... binary in their immutable
                                   environment.json — unchanged)
probe result:                      uring_queue_init PASS, register_buffers
                                   PASS, READ_FIXED PASS (res=4096),
                                   read content PASS, WRITE_FIXED PASS
                                   (res=4096), destination content PASS,
                                   unregister_buffers PASS,
                                   write_submitted_after_read_cqe=true,
                                   capable=true, exit 0
memlock re-observation:            soft = 8388608 (8 MiB), feasible/
                                   infeasible U2 cells identical to the
                                   original probe session
regression guard:                  scripts/check_rbuf_e0_probe_order.py
                                   (source-structural + executed-probe)
                                   PASS
```

Immutable-evidence hash audit (pre- and post-regeneration, sha256 of
raw/runs.jsonl, raw/perf.csv, manifest.json, gates.json for all three
formal sessions): UNCHANGED — Q0 50 runs, steady 112 runs, amortization
56 runs, 218 formal runs total, gate errors 0. Derived artifacts
(analysis.json, summary.csv/json, plots) were regenerated from the
untouched raw evidence only; the sole analysis change is the field rename
`setup_fraction` -> `setup_plus_teardown_fraction` (values identical).

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
FINAL — RBUF-E0 HOST-0 RESULT
==================================================

```text
Q0:                   QUALIFIED (50/50; does NOT close #262 — #262
                      did not reproduce in this restricted
                      single-worker direct-liburing regime; #262
                      remains OPEN)

STEADY STATE:         REGISTERED BUFFER STEADY-STATE NOT MATERIAL
                      (frozen rule; NOT MATERIAL != NO EFFECT EXISTS)

PRIMARY 2M x d2:      nominal +4.05% median U2 advantage, but fails
                      the frozen 1.5x MAD separation; not material

NEIGHBOR CONSISTENCY: NO

LIFECYCLE:            ~1.086 ms registration / ~65 us unregistration
                      for the 4 MiB / 2-iovec treatment

RESOURCE:             8 MiB registration request infeasible under the
                      observed 8 MiB soft RLIMIT_MEMLOCK configuration
                      (ENOMEM); Host-0 configuration/capability
                      boundary, NOT a protocol or Linux limit

AMORTIZATION:         CROSSOVER NOT LOCATED IN TESTED RANGE
MEASUREMENT QUALITY:  end-to-end amortization under-resolved for the
                      ~1.15 ms registration lifecycle — seconds-scale
                      filesystem/dirty-page teardown noise dominates
                      the non-steady region

PRODUCTION PROMOTION: NO (registered-buffer production integration:
                      NO; public API: NO; runtime policy: NO; RBUF-P1:
                      NOT JUSTIFIED)

CLAIM:                HOST-LOCAL ONLY
```

A negative result is a completed result: RBUF-E0 answered its frozen
question for Host-0 and closes as a negative research campaign. #262,
#270 and #259 remain OPEN and unaffected.
