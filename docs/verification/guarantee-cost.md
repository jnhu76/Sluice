# Guarantee-cost vectors (#199, child of #163 V6)

Every load-bearing correctness mechanism records BOTH columns — the
**guarantee bought** and the **cost paid** — with an evidence pointer for
each. Methodology: [`performance-engineering.md`](performance-engineering.md);
measured evidence is Release-only, runner JSON under
`docs/results/performance-attribution/`, structurally validated by
`scripts/bench/perf-evidence-validate.py` (kind `overload`).

**Wording rule inherited from the methodology (§7)**: an aggregate measured
cost proves the cost exists; it does **not** decompose it among internal
mechanisms. No row below claims internal attribution without a decomposition
experiment.

## 1. Mechanism cost vectors

| Mechanism | Guarantee bought (evidence) | Cost class | Cost paid (evidence) |
|---|---|---|---|
| **Completion publication / reap authority** (reap-only publication, slot-bound capability) | exactly-one terminal publication; no worker publishes a Completion (C2b mutants C/F/G: `docs/verification/phase-c2b-identity-mutation-evidence.md`; memory-order tier: #197 weak-memory kernels) | DYNAMIC + STATIC | per accepted op: claim CAS + commit CAS + publish CAS + release store + seq_cst reap RMW (orderings are the #197 kernel subjects, not decomposed here); measured **aggregate** accepted-path submit p50 70–80 ns under overload ([v6 artifact](../results/performance-attribution/v6-overload-backpressure.json)); static: `sizeof(Completion<std::size_t>)` = 64 B, `sizeof(Completion<void>)` = 48 B (artifact `static` block) |
| **RequestSlot generation advance before reuse** | a stale key cannot act on a reused slot (C2b mutant A; C2c mutant H; D3-M1/M2) | STATIC (+ release-path RMW) | generation lives inside the slot (generation advances fused into slot release); the release path is inside the measured refill/accept aggregate — **no separate attribution claim** (decomposition experiment not run) |
| **enqueue-pin acknowledged reap gate** | no reap vs enqueue/cancel race; interrupt-window final reap/poll (C2e mutants M4/M12 detectors `tp_c2e_interrupt_final_reap_closes_ready_race`, `ctx_wait_one_interrupt_final_poll_closes_ready_race`) | DYNAMIC (fused) | folded into the reap path inside the measured aggregate; **not separately measurable at this layer** (recorded honestly, not guessed) |
| **terminal-winner arbitration** | exactly one terminal; losers never overwrite (C2b mutants C/D; FE-tier evidence in the mutation docs) | DYNAMIC (one CAS per terminal) | inside the measured complete+poll+reset refill cycle aggregate (refill accept p50 above); **no decomposition claim** |
| **split-wait bridge + wake epoch** (#190/#194/#195 model subjects) | no causeless park return; no lost wake (E9 19-gate suite with non-vacuity witnesses; `scripts/formal/verify-e9-park-wake.sh`) | DYNAMIC (wake-path locked RMW + epoch advance) | same locked-RMW write class as the #161 A/B precedent (`bench/idle_erase_ab_bench.cpp`: store(0) vs exchange(0)+conditional bump on the pop/dance hot path, Release, same-session protocol); **cited precedent, no new attribution** |
| **bounded arena admission** (`would_block` refusal path) | pre-acceptance refusal with zero residue (C2d mutants M12/M13; D2-M7/M11) | **OVERLOAD-RELEVANT (measured)** | measured under sustained overload (§2): refusal p50 40–50 ns, p99 ≤ 80 ns — **cheaper than the accepted path** (the refusal fires before slot reserve/binding); refusal share of sustained attempts ≈ 80 % at the operating point |
| **BlockingIoPool bounded dispatch storage** | no unbounded growth (AC-7 class implementation-resource tests) | OVERLOAD-RELEVANT | boundedness proven by tests; overload cost **not separately measured in this pilot** — the async arena admission path is the measured proxy; recorded as an honest gap, not filled |

## 2. Sustained-overload backpressure experiment

Artifact: [`v6-overload-backpressure.json`](../results/performance-attribution/v6-overload-backpressure.json)
(`scripts/bench/perf-attribution.py overload`, kind `overload`, Release).
Instrument: `bench/overload_backpressure_bench.cpp` — FakeAsyncBackend
(deterministic: no worker or disk noise, single thread), so the overloaded
resource is unambiguous per the AGENTS.md §12 resource distinction: the
**RequestArena admission capacity** (not worker count, not queue depth, not
pipeline depth).

Protocol per capacity {16, 64, 256}: cold-fill to the first `would_block`;
then 400 rounds of {32 further attempts (all must refuse — measured refusal
latency) → complete 8 → refill 8 (all must be accepted)}; then drain-all
(recovery) and a post-drain admission probe. The validator enforces the
resource-bound distinction fail-closed: refusals == rounds×burst+1, refills
== rounds×k, high-water == capacity, final in-flight == 0, post-drain probe
accepted.

Observed (this host, WSL2 — environment-sensitive, no absolute claims):

| capacity | refuse p50 / p99 (ns) | accept-under-overload p50 / p99 (ns) | high-water | drain (ns) |
|---|---|---|---|---|
| 16 | 50 / 71 | 80 / 110 | 16 | 2 265 |
| 64 | 50 / 80 | 70 / 131 | 64 | 10 662 |
| 256 | 40 / 80 | 70 / 150 | 256 | 109 672 |

- **Backpressure behaves as designed**: the refusal path is a cheap
  synchronous rejection (~2/3 of the accepted submit's p50 at these sizes),
  never a drop or a block; the accepted path stays flat across capacities at
  p50 while its p99 tail grows mildly (O(capacity) arena scan effects are
  visible in the tail, not the median — consistent with the bounded-scan
  design).
- **Reclamation/recovery**: after the sustained phase, full drain completes
  in µs-to-100 µs at these scales and the post-drain admission probe is
  accepted — the bound that fired was admission capacity and it is fully
  reclaimed (validator-enforced).
- **No unbounded growth**: RSS series recorded per 50 rounds (10 points per
  capacity); observed drift over the run is ~+350 kB, which includes the
  bench's own growing sample buffers — the series is recorded so a real
  growth trend cannot be hidden by a summary number.
- **A/B shape**: accepted-path vs refusal-path latency in the same build
  plus a capacity sweep. No production mutants (prohibited); no internal
  decomposition of the accepted-path aggregate.

### Static probes (production types, from the artifact)

`sizeof(SlotHandle)` 16 B · `sizeof(Completion<std::size_t>)` 64 B ·
`sizeof(Completion<void>)` 48 B · `sizeof(RequestHandle)` 32 B ·
`sizeof(ReadOp)` 32 B.

## 3. Honest open items

- BlockingIoPool dispatch-storage overload: not measured (async arena
  admission is the measured proxy; same refusal contract class).
- Park/wake epoch path: cost class cited via the #161 A/B precedent; a
  dedicated decomposition experiment would be required for any split of the
  accepted-path aggregate among claim/commit/publish/reap/reset.
- All numbers are single-host (WSL2) Release evidence; the runner embeds the
  environment fingerprint and the binary sha256, and the validator rejects
  hand-typed tables (percentiles are recomputed from raw samples).
