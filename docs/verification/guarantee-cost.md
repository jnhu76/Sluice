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

## 0. Pilot acceptance scope (rescoped, review #205 P1)

#199 acceptance says *"named guarantee + measured cost + evidence pointer
per mechanism"*. Review (PR #205) correctly rejected that reading: several
row-level costs were "not measured" rather than measured. This pilot
therefore rescopes the acceptance to a **cost vector per mechanism**, being
one of:

- **MEASURED** — timing under the validated overload artifact
  (OVERLOAD-RELEVANT rows);
- **STATIC** — an exact per-operation count of atomic ops (CAS/release
  stores) and/or bytes, resolved from production sources with file:line;
- **PRECEDENT-CITED** — a same-class measurement already accepted by a
  named precedent (the #161 A/B bench), with no new attribution claim.

Explicitly **out of pilot scope** (recorded, not filled): internal timing
decomposition of the accepted-path aggregate (claim/commit/publish/reap/
reset split requires a staged isolating experiment), and a dedicated
park/wake or BlockingIoPool overload timing run. These are follow-up work,
not silently claimed. No row below is both "not measured" and presented as
the measured per-mechanism cost.

The machine-checked claim stays: **in-flight request state remained bounded
by RequestArena capacity under sustained overload, with bounded harness
memory** (§2); "no unbounded growth" is not claimed beyond that scope.

## 1. Mechanism cost vectors

Every row carries a **cost vector**: either a measured timing number
(OVERLOAD-RELEVANT rows, from the validated artifact) or an exact static
per-operation count resolved from production sources (file:line below).

| Mechanism | Guarantee bought (evidence) | Cost class | Cost paid (evidence) |
|---|---|---|---|
| **Completion publication / reap authority** (reap-only publication, slot-bound capability) | exactly-one terminal publication; no worker publishes a Completion (C2b mutants C/F/G: `docs/verification/phase-c2b-identity-mutation-evidence.md`; memory-order tier: #197 weak-memory kernels) | DYNAMIC + STATIC | **static, per full accept→reap→reset cycle** (`include/sluice/async/completion.hpp`): 4 CAS — binding claim `:323`, binding→outstanding commit `:340`, outstanding→publishing winner `:406`, ready→resetting reset claim `:249` — + 2 release stores (ready `:414`, idle `:273`) = **6 atomic ops / op** (all acq_rel/release; orderings are the #197 kernel subjects). Static bytes: `sizeof(Completion<std::size_t>)` = 64 B, `sizeof(Completion<void>)` = 48 B (artifact `static` block). Measured **aggregate** accepted-path submit p50 70 ns under overload ([v6 artifact](../results/performance-attribution/v6-overload-backpressure.json)) — aggregate only, no intra-path decomposition |
| **RequestSlot generation advance before reuse** | a stale key cannot act on a reused slot (C2b mutant A; C2c mutant H; D3-M1/M2) | STATIC | **0 per-op atomic ops**: `free_slot_locked_` (`include/sluice/async/detail/request_arena.hpp:1171`) advances the 64-bit generation as an ordinary load+store **under the arena leaf mutex** (`mutex_`, :1193) — generation is not a slot-level RMW. The leaf domain's serialization IS the arena mutex (1 lock/unlock per reserve/release), already inside the measured aggregate |
| **enqueue-pin acknowledged reap gate** | no reap vs enqueue/cancel race; interrupt-window final reap/poll (C2e mutants M4/M12 detectors `tp_c2e_interrupt_final_reap_closes_ready_race`, `ctx_wait_one_interrupt_final_poll_closes_ready_race`) | STATIC | **0 per-op atomic ops**: `enqueue_in_flight_pin_` is a plain bool (`request_slot.hpp:177`), set/cleared/acquire-checked only under the arena mutex — no separate RMW beyond the mutex itself |
| **terminal-winner arbitration** | exactly one terminal; losers never overwrite (C2b mutants C/D; FE-tier evidence in the mutation docs) | STATIC (winner CAS) + mutex | loser/winner arbitration happens under the arena mutex (slot state ordinary enum, `request_slot.hpp`); the single atomic arbitration point is the publish winner CAS outstanding→publishing (`completion.hpp:387` — counted above). **1 CAS is the only atomic terminal race**; refill-cycle aggregate p50 above |
| **split-wait bridge + wake epoch** (#190/#194/#195 model subjects) | no causeless park return; no lost wake (E9 19-gate suite with non-vacuity witnesses; `scripts/formal/verify-e9-park-wake.sh`) | DYNAMIC (wake-path locked RMW + epoch advance) | same locked-RMW write class as the #161 A/B precedent (`bench/idle_erase_ab_bench.cpp`: store(0) vs exchange(0)+conditional bump on the pop/dance hot path, Release, same-session protocol); **cited precedent, no new attribution** (isolating park/wake experiment is out of pilot scope — see §0) |
| **bounded arena admission** (`would_block` refusal path) | pre-acceptance refusal with zero residue (C2d mutants M12/M13; D2-M7/M11) | **OVERLOAD-RELEVANT (measured)** | measured under sustained overload (§2): refusal p50 40 ns, p99 ≤ 60 ns — **cheaper than the accepted path** (refusal path: 1 arena-mutex lock + free-list empty check + counter++, no CAS, no slot write — `request_arena.hpp:242-253`); refusal share of sustained attempts ≈ 80 % at the operating point |
| **BlockingIoPool bounded dispatch storage** | no unbounded growth (AC-7 class implementation-resource tests) | STATIC (bounded) + RESCOPED | bounded by construction: fixed `worker_count` + `max_queue_depth` (`include/sluice/blocking_io_pool.hpp:47-56`), queue depth is the bounded dispatch storage; overload timing **not separately measured in this pilot** — rescoped to the static bound + the async arena admission proxy (see §0); recorded as an honest gap, not filled |

## 2. Sustained-overload backpressure experiment

Artifact: [`v6-overload-backpressure.json`](../results/performance-attribution/v6-overload-backpressure.json)
(`scripts/bench/perf-attribution.py overload`, kind `overload`, Release).
Instrument: `bench/overload_backpressure_bench.cpp` — FakeAsyncBackend
(deterministic: no worker or disk noise, single thread), so the overloaded
resource is unambiguous per the AGENTS.md §12 resource distinction: the
**RequestArena admission capacity** (not worker count, not queue depth, not
pipeline depth).

Protocol per capacity {16, 64, 256}: cold-fill to the first `would_block`;
then 400 measurement rounds of {32 further attempts (all must refuse —
measured refusal latency) → complete 8 → refill 8 (all must be accepted)};
then a separate **sustained RSS boundedness phase** — 2000 more rounds at
the same fixed capacity with NO latency recording (the sample reservoirs are
already full, so no harness allocation can grow), with RSS sampled across
the whole interval; then drain-all (recovery) and a post-drain admission
probe. Latency samples are stored in fixed-size reservoirs (4096 entries
max per phase), so bench memory is bounded regardless of round count. The
validator enforces the resource-bound distinction fail-closed: refusals ==
rounds×burst+1, refills == rounds×k, high-water == capacity, final in-flight
== 0, post-drain probe accepted — and the RSS plateau: sustained refusals/
refills match their expected counts (the overload was actually maintained)
and the sustained RSS end-minus-start delta is ≤ 256 kB (recomputed from
the series; a growth trend the bounded harness cannot explain fails).

Observed (this host, WSL2 — environment-sensitive, no absolute claims):

| capacity | refuse p50 / p99 (ns) | accept-under-overload p50 / p99 (ns) | high-water | drain (ns) |
|---|---|---|---|---|
| 16 | 40 / 60 | 70 / 90 | 16 | 1 724 |
| 64 | 40 / 50 | 70 / 90 | 64 | 9 439 |
| 256 | 40 / 51 | 70 / 90 | 256 | 98 983 |

- **Backpressure behaves as designed**: the refusal path is a cheap
  synchronous rejection (p50 40 ns — well under half the accepted submit's
  p50 at these sizes; the refusal fires before slot reserve/binding), never
  a drop or a block; the accepted path stays flat across capacities at p50
  while its p99 tail grows mildly (O(capacity) arena scan effects are
  visible in the tail, not the median — consistent with the bounded-scan
  design).
- **Reclamation/recovery**: after the sustained phase, full drain completes
  in µs-to-100 µs at these scales and the post-drain admission probe is
  accepted — the bound that fired was admission capacity and it is fully
  reclaimed (validator-enforced).
- **No unbounded growth (sustained RSS phase)**: the harness's own latency
  storage is bounded (fixed-size reservoirs), and the RSS boundedness phase
  runs at fixed capacity with no further harness allocation. Observed
  sustained RSS delta on this host: **0 kB at every capacity** (the series
  is flat at 5 points over 2000 rounds) — the arena's working set does not
  grow under sustained fixed-capacity overload. The validator requires the
  delta to stay ≤ 256 kB and recomputes it from the recorded series, so a
  growth trend cannot hide behind a summary number. RSS is a coarse signal
  (page granularity, ~4 kB): a sub-page steady-state drift is below its
  resolution and is not claimed away.
- **A/B shape**: accepted-path vs refusal-path latency in the same build
  plus a capacity sweep. No production mutants (prohibited); no internal
  decomposition of the accepted-path aggregate.

### Static probes (production types, from the artifact)

`sizeof(SlotHandle)` 16 B · `sizeof(Completion<std::size_t>)` 64 B ·
`sizeof(Completion<void>)` 48 B · `sizeof(RequestHandle)` 32 B ·
`sizeof(ReadOp)` 32 B.

## 3. Honest open items

- BlockingIoPool dispatch-storage overload timing: rescoped out of pilot
  scope (§0) — the static construction bound
  (`include/sluice/blocking_io_pool.hpp:47-56`) plus the async arena
  admission proxy stand in; a dedicated timing run is follow-up work, not
  claimed.
- Park/wake epoch path: cited via the #161 A/B precedent; a dedicated
  decomposition experiment would be required for any split of the
  accepted-path aggregate among claim/commit/publish/reap/reset (rescoped
  in §0).
- All numbers are single-host (WSL2) Release evidence; the runner embeds the
  environment fingerprint and the binary sha256, and the validator rejects
  hand-typed tables (percentiles are recomputed from raw samples). Static
  counts are resolved against production sources at the artifact revision
  and should be re-verified if the atomic shape of those paths changes.
