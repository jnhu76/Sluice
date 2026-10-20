# M1 Results — Sluice v2 (master `8fdcd99c`)

Host: WSL2 kernel 6.18.54.1, AMD Ryzen 7 5800H (Zen 3, 4C/8T), 15 GB, gcc
15.2.0, `-std=c++20 -O2 -Wall -Wextra` for mechanism benches, xmake release
for the production binaries. Layer A pinned to CPU 2 with `taskset`; arms
replay identical xorshift sequences; 9 measured repetitions after 2 warmups
unless stated; correctness oracles gate every repetition and every negative
control was required to be CAUGHT before timing counts (enforced by
`run_mechanisms.sh`). Every number below is quoted from the committed
artifacts in `results/`. All values are median wall ns/op (ns/pass for H2);
spread is (max−min)/median over repetitions. WSL2 caveat: absolute values
carry virtualization noise; only deltas with disjoint repetition ranges are
treated as real.

## H1 — io_uring router lookup (Layer A)

`results/h1_decision.txt`. Arms: production reverse scan vs direct
`user_data`-encoded slot index. Encoding: bit 63 control tag, bits
[48..62] router slot (15 bits), bits [0..47] cookie — both arms re-check
the full cookie against the entry, preserving the never-reuse anti-ABA
property. **Domain coupling (proposal constraint, not measured here):**
the candidate narrows two production domains — cookies must fail fast at
2^48 (production: 2^63, `uring_transport.cpp:526-538`) and
`request_capacity` must be bounded to ≤ 32768 at config validation
(`UringConfig` currently admits up to 2^32−1). Both bounds are joint:
slot_bits + cookie_bits + 1 control bit = 64.

| capacity | mix | baseline ns/op | direct ns/op | ratio |
|---|---|---|---|---|
| 8 | lookup | 5.69 | 4.47 | 0.79x |
| 16 | lookup | 6.37 | 4.50 | 0.71x |
| 32 | lookup | 8.26 | 4.68 | 0.57x |
| 64 | lookup | 10.72 | 4.40 | 0.41x |
| 128 | lookup | 16.95 | 4.46 | 0.26x |
| 256 | lookup | 30.27 | 4.98 | 0.16x |
| 1024 | lookup | 102.10 | 4.72 | 0.05x |
| 64 | churn | 42.65 | 15.91 | 0.37x |
| 1024 | churn | 283.80 | 16.74 | 0.06x |
| 64 | miss | 26.33 | 7.46 | 0.28x |
| 1024 | miss | 286.11 | 7.66 | 0.03x |

Crossover: none inside the supported capacity range — the direct index
wins already at R=8 (ratios 0.79x with disjoint repetition ranges); the
scan's cost grows linearly with R exactly as the O(R) model predicts while
the direct index stays flat. Churn/miss absolute costs include
dispatch/retire work common to both arms; the lookup-only delta at R=64 is
≈ 6.3 ns/completion. Controls: the slot-only lookup without the cookie
re-check is CAUGHT by the stale-cookie-after-reuse oracle; the verified
direct arm passes the identical sequence.

**Layer B (in-context):** `perf record` on W02-uring-4k-d64 (711 samples,
2999 Hz, single run, symbols resolved offline —
`results/perf-uring-d64-attribution.txt`,
`results/layerb-provenance.txt`): `find_live_router_cookie_` = **2.01% of
total CPU** (14 samples; Poisson 95% CI ≈ 1.1–3.3%). A zero-cost
replacement therefore caps the uring W-02 end-to-end gain near **2%, CI
1–3%** at this depth — an upper-bound estimate from one profiled run, not
a measured improvement.

## H2 — owed-event visibility (Layer A)

`results/h2_decision.txt`, cap=64 (cap 1024 idle in text below; full
sweep in the artifact). The timed region includes the oracle model's O(C)
checksum walk in all arms equally — absolute ns/pass are inflated, deltas
are the meaningful quantity.

| arm (cap 64) | idle | sparse (1 set) | half | full |
|---|---|---|---|---|
| baseline single-lock sweep (ThreadPool shape) | 129.2 | 143.8 | 608.3 | 1003.3 |
| baseline per-slot lock sweep (uring shape) | 458.0 | 472.3 | 965.2 | 1319.5 |
| counter + snapshot sweep | 87.9 | 125.2 | 609.5 | 1012.5 |

ns/pass; cap 1024 idle: 1503.8 / 6891.6 / 1016.8. At cap 64 idle the
uring shape pays ≈ 370 ns/pass more than the counter shape (disjoint
ranges, spreads ≤ 3%): that delta is the per-slot lock/unlock pairs plus
the always-run O(C) scan, which the counter replaces with an O(1) verdict
and an O(owed) sweep. Crossover: for owed fractions ≳ 50% of capacity all
shapes converge (delivery work dominates); below that the counter wins.
The fresh-event verdict phase keeps the counter arm honest (≈ half the
rounds still scan), so the idle column understates the counter's strict
idle advantage.

Controls: a counter that skips the deliver-side decrement is CAUGHT via
the stale residue verdict; the correct counter arm passes 5000 rounds with
sum+xor delivery checks. Set/clear parity note: production cannot
double-set a slot before delivery — `publish_zero_op_inline` holds a core
control ref that pins the slot against reclaim until the owed event is
delivered — so the oracle does not need a double-set case; this invariant
is part of the proposal's duty list.

**Layer B:** `pthread_mutex_lock` 5.46% (95% CI ≈ 3.9–7.8%) +
`pthread_mutex_unlock` 2.09% (CI ≈ 1.2–3.7%) of total CPU in the uring d64
profile; with ≈ 64 of ~73 lock pairs per op in the owed sweep, the
sweep-specific ceiling is roughly **5% of uring W-02 op CPU at depth 64**
(single run, estimate). 7.22% sits under the unresolved
`_dl_mcount_wrapper` libc symbol — excluded from claims. ThreadPool mutex
time is 3.81% + 2.90% with ~7 pairs/op; its single-lock sweep would save
≈ 40 ns/pass at cap 64 (≈ 1.5% of a 2.5 μs op). **Scope restriction
(BACKEND-04):** a counter may serve only the per-pass `ProgressPass`
residue verdict and wait/park fast paths; `internal_work_retired()` and
both destructors' quiescence checks keep consulting the authoritative
`delivery_` scan (root §15 BACKEND-04: settlement, shutdown and destruction
decisions must consult the authoritative scan, not a derived summary).

## H3 — RequestScope slot management (Layer A)

`results/h3_decision.txt`. Arms share the production resolve scan; they
differ only in reserve (first-free scan vs LIFO free list).

**FIFO pattern (the W-02 default consumption order) — the free list is a
regression at small capacities**, disclosed per the methodology's
regression rule:

| capacity | baseline ns/op | freelist ns/op | delta |
|---|---|---|---|
| 8 | 12.96 | 15.12 | **+17%** (disjoint ranges) |
| 16 | 18.16 | 22.05 | **+21%** (disjoint ranges) |
| 32 | 23.56 | 27.73 | **+18%** (disjoint ranges) |
| 64 | 29.95 | 34.51 | **+15%** (disjoint ranges) |
| 256 | 52.12 | 58.42 | +12% (ranges overlap) |
| 1024 | 59.02 | 63.31 | +7% (spreads ≥ 1.5 — noise) |

Why: under FIFO-oldest consumption the first-free scan is already O(1)
(the freed low slot is reused immediately), while the LIFO free list adds
per-release work and lengthens nothing it pays for. At ρ≈0.5 occupancy
(random take) the two arms are statistically indistinguishable — expected
first-free probes ≈ 2.

**Saturated random pattern (occupancy 100%, one free slot at a time,
random victim) — an adversarial worst case, not the W-02 default shape:**
baseline vs freelist 46.5→38.3 (cap 32, −18%), 66.8→52.9 (cap 64, −21%),
188.4→156.4 (cap 256, −17%), 785.0→557.5 (cap 1024, −29%; disjoint
ranges). The fifo-saturated combination was not measured; under FIFO
saturation the single free slot sits near the array front, so the
baseline scan is expected O(1) there and the free-list win should mostly
vanish.

W-02 harness-only window bookkeeping control: **4.8–5.5 ns/op** at every
depth — the deque push/erase-front + free-slot stack adds no material
cost, and in default (FIFO-oldest) mode the erase is O(1).

**Layer B:** production attribution on W02-threadpool-4k-d64-w4 (680
samples, single run — `results/perf-threadpool-d64-attribution.txt`):
`RequestScope::take` 7.15% (CI ≈ 5.3–9.4%) + `RequestScope::wait_for`
6.02% (CI ≈ 4.3–8.2%) + `RequestScope::submit_read` 2.71% (CI ≈
1.6–4.3%) ≈ **15.9% of in-context CPU (CI ≈ 13–19%)** under the
RequestScope symbols at scope capacity 64. Important framing: this share
is the scope's whole machinery (resolve scan + variant dispatch + core
bridge calls), not a measured scan cost; the scan-specific share is
bounded above by it. The G0-style isolation case is only directionally
informative: d4 (scope 4) 6884 ns/op vs d4-scope64 7083 ns/op (+2.9%) —
sign-consistent with a small scan share but statistically uninformative at
5 reps (spreads 4.9%/16.2%).

## Synthesis

| Mechanism | Semantic equivalence | Original | Candidate | Measured (Layer A) | In-context bound (Layer B, single-run CI) | Decision |
|---|---|---|---|---|---|---|
| H1 router direct index | PASS (oracle + collision control) | O(R)/CQE scan | O(1)+verify | 0.41x at R=64, 0.05x at R=1024, flat in R | ≤ ~2% (1.1–3.3%) of uring W02 CPU (d64) | **PROPOSE_UPSTREAM** (with the 15-bit/48-bit envelope and its config validation + fail-fast obligations) |
| H2 owed counter + snapshot sweep | PASS (delivery sets, residue verdict, negative control) | O(C)/pass (+C lock pairs uring) | O(1) verdict, O(owed) sweep | −370 ns/pass (uring, C=64 idle); converges ≥50% owed; −40 ns/pass ThreadPool | ≤ ~5% uring W02 CPU (d64); ~1.5% ThreadPool | **PROPOSE_UPSTREAM** (uring first; BACKEND-04 scope: pass verdict only, settlement/destruction keep the authoritative scan) |
| H3 scope free list | PASS (behavioral oracle, capacity, negative control) | O(S) first-free + O(S) resolve | O(1) reserve, resolve unchanged | **+15–21% regression** on the W-02 FIFO shape at S≤64; −17–29% only in adversarial saturated-random | scope machinery ≈15.9% (13–19%) of tp W02 CPU, scan share unproven | **REJECT** the free list as a general mechanism (hurts the primary consumption shape); **INVESTIGATE** the scan-specific share (needs a higher-rep `-scope64` isolation protocol) |

Memory overhead of candidates: H1 none (encoding change, plus the two
domain bounds); H2 one 8 B counter per backend (derived summary maintained
in the same critical sections); H3 none (rejected).

## Gain-source classification (charter §9)

- **H1 — algorithmic + microarchitectural**: O(R)→O(1) lookup (algorithmic),
  realized as fewer cache lines walked and fewer unpredictable compare
  branches per CQE (microarchitectural). No state removed or added.
- **H2 — state/abstraction simplification (+ algorithmic)**: an 8 B derived
  summary replaces a full-array visibility scan and, on uring, C lock pairs
  per pass (microarchitectural component). The simplification is bounded by
  BACKEND-04: the summary stays non-authoritative for settlement/destruction.
- **H3 — state/abstraction simplification, rejected on evidence**: the free
  list would have been a pure state-mechanism swap; the measurement showed
  it taxes the primary consumption shape, so lines-of-code or mechanism
  elegance is not a runtime improvement — recorded as REJECT.

## PROPOSE_UPSTREAM sketches (minimal production changes, for owner adjudication)

**H1 — encode the router slot in io_uring `user_data`**
- Minimal change: in `dispatch_one_locked` set
  `user_data = control_tag | router_slot << 48 | cookie48`; in
  `find_live_router_cookie_` decode the slot and re-check the full cookie
  (falling back to nothing — a mismatch is a miss, exactly as today);
  `allocate_cookie_` fail-fast moves from `CONTROL_TAG` to `2^48`;
  `UringConfig` validation gains `request_capacity <= 32768`.
- Affected requirements: REQ-01 (identity/anti-ABA preserved by full-cookie
  re-check + never-reuse), BACKEND-02/03 (profile behavior unchanged),
  BACKEND-04 (mechanism, not contract), BOUND-01 (capacity visibility gains
  an explicit ceiling).
- Next-round matrix: boundary oracles at slot 32767/32768 and cookie near
  2^48; the full `uring_*` deterministic suites (incl. RG5 registered
  mutant re-run per VERIFY-06); native-Linux Layer B re-profile; ledger
  re-basing of G4/G6 rows.

**H2 — owed counter + single-lock snapshot sweep**
- Minimal change: add an 8 B `owed_count_` beside `delivery_`, maintained
  in the same critical sections as every `event_owed` write/clear; the
  uring sweep snapshots under one lock (mirroring the ThreadPool
  `owed_batch_` shape) and skips the scan when the count is zero;
  `poll_progress`'s residue verdict reads the count. `internal_work_retired()`
  and both destructors keep the authoritative `delivery_` scan (BACKEND-04).
- Affected requirements: PROG-01 (V28/V29 honest residue — verdict parity
  is the discriminator), BACKEND-04 (derived-summary rule), THREAD-02
  (same-lock discipline unchanged).
- Next-round matrix: `uring_owed_residue_test` + `threadpool_progress_*`
  suites, the owed-counter parity oracle from `bench/mechanisms/h2`,
  native-Linux W-02 A/B with the interleaved protocol, ledger re-basing of
  G4/G6.

**INVESTIGATE (H3 scan share)**: re-run the G0 `-scope64` isolation cases
with ≥ 15 reps under the frozen-matrix protocol to pin the scan-specific
share before any scope-mechanism proposal.

## Measurement-axis disclosures (charter §8 deviations)

- Threads: 1 only — every mechanism under test is serialized under a single
  mutex in production, so a single-threaded replay under the same lock
  discipline is the faithful model; concurrent arms would measure the mutex,
  not the mechanism. The 4/8-worker dimension is covered by the production
  W-02/W-03 matrix (G0) instead.
- Cache state: structures at these capacities (≤ 32 KB for H1 at R=1024,
  ≤ 32 KB H2 records at C=1024) never leave L2; the >L1/>L2 working-set axis
  is not meaningful for them and was not swept. Layer B profiles carry the
  realistic cache context.
- Access distributions: uniform (xorshift), reuse-heavy (churn), high-miss
  (miss mix), FIFO/sequential (fifo pattern), adversarial collision (stale
  probes); Zipf-like hot-set and burst distributions were not swept — for
  dense ≤1024-slot arrays they vary only the scan start position, which the
  occupancy and pattern axes already bound.

## Evidence boundaries (honesty constraints)

- Layer A→Layer B mapping: mechanism deltas were measured hot-in-cache and
  mutex-uncontended; production shares are per-symbol attribution of the
  real binaries (single profiled run each, Poisson CIs stated), not
  projections of Layer A ratios.
- perf_stat per-arm totals (`results/perf_stat.txt`, 7 solo sections)
  include oracle+warmup phases; they bound cycles/op from above and are
  used only for IPC/cache context, never as per-op claims.
- Arm interleaving: H1 alternates arms per repetition; H2/H3 run arms as
  sequential blocks — their deltas carry disjoint repetition ranges
  instead.
- Latency tails in Layer B W-02 runs show multi-ms WSL2 outliers; tails
  are not used in any decision.
- WSL2-only host; the charter's native-Linux re-run remains owner work for
  any PROPOSE_UPSTREAM acceptance.
- Re-basing obligation: any accepted proposal touching owed-delivery or
  router code re-bases ledger rows G4/G6 (PROG-01 V28/V29, PROG-02 oracle
  suite) and re-runs the RG5 registered mutant per VERIFY-06 before it may
  carry a VERIFIED claim.
- Raw results are committed under `results/` as research evidence for this
  study (charter §11/§12); this is a deliberate exception to the
  performance-methodology rule that perf PRs carry summaries only — that
  rule governs production-change PRs, and this round changes none.
