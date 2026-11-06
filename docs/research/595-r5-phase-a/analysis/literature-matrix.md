# #595 R5 Phase A — Literature Mechanism Matrix for the R4 p99 Question

Role: p99 literature-mechanism analyst. Written 2026-11-06 (UTC) on the native
E5 host. This file is analysis only: no measurements were run for it, no
locks were needed, no repository files were modified. Every R4 number below
was read this session from the frozen archive files
`/home/jhu/sluice-595-e5/r5/r4-archive/TAIL-LATENCY.md` (cited T:line) and
`/home/jnhu/sluice-595-e5/r4/RESULTS.md` (cited R:line). Sluice mechanics were
read from the R4_ORIGINAL source at `/home/jnhu/sluice-595-e5/r5/worktree`
(HEAD f3b25329), cited as `path:line`. Paper text was read from local
extracts under `/tmp/r5-papers/` (provenance in §4); ZygOS was read through a
server-side extraction of a course-site mirror because every canonical host
blocked or was unreachable (§4) — it is first-source text, not a paraphrase.

Question addressed: R4's main-gate FAIL is a p99-only phenomenon at W03-d16
(R p99 +21.85%, Q8 +23.33%, S +34.57% vs A0; p50/p95 ≤ +5% for R; T:24).
What do the mechanisms in five µs-scale systems papers predict about *why*
each R4 arm failed, which mechanisms are legal for Sluice, and which of the
failure classes (indiscriminate signal / stale snapshot / action not acting
on the target / too-expensive execution / genuine tradeoff) each arm falls
into?

## 1. Sluice-side ground facts used (all verified this session)

Code facts (worktree f3b25329):

- The R4 arms live in the bench adapter, not the library:
  `bench/r3_ready_batch.hpp`. The decision table (`kTable`,
  r3_ready_batch.hpp:224-246) has rows `quantum_base` (batch, quantum 8),
  `inflight_zero` (active_data == 0 → clamp quantum to 2),
  `completion_pressure` (pressure → clamp to 2), plus immediate/authority
  rows. Presentation length is `min(quantum, eligible_prefix)`
  (r3_ready_batch.hpp:497) — presentation never waits for more ops to
  arrive; the quantum only caps how much of the *already-ready* span is
  submitted per entry.
- `pressure` = `facts.pressure_known && (facts.last_pass_completed > 0 ||
  facts.last_pass_immediate_work)` (r3_ready_batch.hpp:474-476).
- `DecisionFacts` is a snapshot of the most recent owner progress pass,
  "carried to the submit thread by the harness (its existing shared-mutex
  channel); the adapter itself never polls" (r3_ready_batch.hpp:137-140).
- The producer loop runs `poll_progress()` until a pass reports
  `immediate_work_remains == false` and then publishes that terminal pass's
  `completed`/`immediate_work` (bench_w03_external_loop.cpp:230-245); the
  consumer reads the shared snapshot just before `adapter->submit(...)`
  (bench_w03_external_loop.cpp:415-424). Consequence: in the normal path the
  published `last_pass_immediate_work` is always false (loop-break
  condition), so `completion_pressure` reduces to "the owner's previous
  terminal drain pass completed > 0", of unbounded age.
- R's cap behavior: on `ReadyBatchStop::cap` with progress and more input,
  the adapter immediately re-presents the suffix in the same submit call
  (`cap_reuse`, r3_ready_batch.hpp:557-570). A0 is one `submit_read` per op
  (`submit_context_singles`, r3_ready_batch.hpp:412, 595).
- Library side: the entry's own cap is
  `limit = min(ready.size(), ready_batch_cap, sq_budget)`
  (src/async/uring_backend.cpp:531).
- Harness topology: exactly two threads — submitter (parks on a condition
  variable) and driver/progress owner (parks in `epoll_wait`); "neither
  thread busy-polls" (bench_w03_external_loop.cpp:1-2, 329). There is one
  presentation queue (the submitter's `ready` array). The owner's role is
  reaping/progress (`poll_progress`), not executing user reads.

R4 measured facts (archive, cited): R p50 +4.75 / p95 +2.71 / p99 +21.85,
24/24 blocks positive (T:24; R:62,75). Q8 p99 +23.33 (T:24). S p95
+33.68 / p99 +34.57, all-percentile shift (T:24). S counters: 49,962
pressure reads, 49,998 quantum-2 of 50,002 presentations (T:51); R4 gate doc
records completion_pressure branch 29,973/29,998 = 99.92% and quantum-2
99.97%, mean ready 7.92 vs presented 2.00 (R:199-206). S ≈ Q2 (CPU diff
+0.83%) and S−Q8 +7.98% (R:186). SNP: only 6,630/19,172 ≈ 35% presentations
take quantum 2 (via `outstanding()==0`), yet p99 +34.48 ≈ S level vs Q8
+23.33 (T:50-65). d4: R p95 −20.54 / p99 −12.92 (tail improves); d64: R p99
+21.17; d2serial: every arm, every metric +6.47..+14.09 FAIL (T:71-78;
R:116-132). btrfs warm buffered: R p99 −30.43, CPU −54.5%; S p95 +23.13 /
p99 +19.45, p50 +108 (T:95-96). SHADOW−R: CPU +1.02, p95 +1.95, p99 +1.22
(R:182). R−B (suffix singles instead of re-entry): R better by p95 −24.06 /
p99 −12.44 (T:107-109; R:188,394). CPU buckets d16: submitter 38.4-45.7%,
io-wq 31.8-36.1%, driver ≤0.5%, residual 22.0-25.7% (R:212-224). The three
preregistered hypotheses for the R p99 regression: (1) completion-order
coupling (last-ready member of a group waits for the group), (2)
cap-truncation restart cost, (3) io-wq/wakeup interaction (T:112-131).

## 2. The matrix

"Adopted" here means: adopted as the mechanism-level hypothesis/eligibility
design for R5 analysis. It does not authorize new runtime state; every row
says what the action would be and at what obligation. Hedging/duplicate I/O,
busy polling, SQPOLL, extra threads, and deadline-style I/O are not in any
row's adopted set (see §3.4).

| # | Paper mechanism (version, section) | Precondition in the original system | Sluice counterpart / wait in question | Existing Sluice signal | Actually executable action under this round's constraints | New cost / semantic obligation | Falsifiable prediction | Adopt / defer |
|---|---|---|---|---|---|---|---|---|
| 1 | IX: adaptive bounded batching — "we never wait to batch requests and batching only occurs in the presence of congestion; … an upper bound on the number of batched packets" (OSDI'14, §5 design principles) | Batch slices an *existing* backlog at the NIC edge; cap protects cache/tx-starvation; amortizes syscall/transition overhead | R/Q8 presentation = cap 8 over the already-ready span (`min(quantum, eligible_prefix)`, r3_ready_batch.hpp:497). The R4 wait in question is not admission delay — it is whatever the larger submitted group does to completion/reaping of its own last members (hypotheses 1/3, T:118-126) | Presentation-time `ready.size()`, `eligible_prefix` (already computed, r3_ready_batch.hpp:437-446) | Keep bounded-batch presentation (R family) as the baseline action; the literature contradicts making the batch *smaller by default* (rows 5, 10) | None beyond R4's R arm | At d16, ops-per-presentation histograms for R vs A0 should show mean presented ≈ ready (no forced waits); if presentations ever idle-wait for group formation, IX's precondition is violated and hypothesis "waiting to batch" revives — R4 counters (mean ready 7.92 vs presented 2.00 for S; Q8 presents 8) make this refuted already for R/Q8 | **Adopt (keep R's admission rule as the reference)**; it matches IX's rule exactly and its CPU/throughput wins are the established part of R4 |
| 2 | IX: the batch is a run-to-completion *execution* unit — "a batch of packets is first carried through the networking stack and then … processed by the application" without intermediate buffering (§5; ZygOS §4.2 describes this) | Batch boundary = execution boundary; no separate completion-notification coupling per member | In Sluice the group is a *submission* unit only; completions are individual CQEs reaped by the owner pass. Batching changes who completes together and when the owner wakes, not how execution interleaves | Owner pass facts (`last_pass_completed`, pass counts) exist; per-member completion timing does not (R4 notes stripped binaries block ns entry→enter, R:439) | Analysis-side: instrument per-position completion latency inside a presented group (test-only evidence, no runtime authority) on a scratch build | Test-only counters in the bench harness; no public-contract change | If hypothesis 1 (completion-order coupling) holds, last-member wait inside 8-groups should grow with group size at d16 tmpfs and vanish at d4/btrfs (where R's tail *improves*, T:73-78, 95) — matching the sign flips; if last-member wait is flat vs group size, hypothesis 1 is refuted and hypothesis 3 gains weight | **Adopt as the top R5 discriminating measurement**; it separates hypotheses 1/3 without any new mechanism |
| 3 | ZygOS: work-conserving task stealing — idle core takes ready connections from another core's shuffle queue; IPIs force a busy home core to replenish (SOSP'17, §4.2, §4.4, §4.5) | (a) transferable work parked in a queue owned by a *different* executor, (b) an actually idle executor, (c) temporary cross-core imbalance (ZygOS §2.3 Obs 1) | W03/W02 shapes have one presentation queue owned by the only submitter and a second thread whose role is reaping; there is no second queue to steal from and no idle executor class (bench_w03_external_loop.cpp:1-2, 329). d2serial never has >1 op in flight (T1 shadow_outstanding_sum ≡ 0, R:244-246) | None: no counter measures "ready work at executor B while executor A busy" because B does not exist | None legal. Stealing would require new executor threads or a second submission queue — both out of bounds (no new threads/runtimes) | n/a | If a future cell shows ready ops accumulating at the submitter while the owner pass idles (queue imbalance), revisit; current data has no such member | **Defer (precondition absent)** — per the ask, no stealing without evidence of transferable work + idle executor |
| 4 | ZygOS: on tiny tasks IX-with-batching beats the stealing system — "ZYGOS outperforms IX with batching disabled but lags behind IX with adaptive bounded batching" for <2µs memcached tasks (SOSP'17, §6.2); ZygOS §3.3: they *disabled* IX batching in their own µs-benchmarks "as disabling batching noticeably improves tail latency" | Tiny, homogeneous task sizes; batching amortizes transitions; stealing machinery has fixed per-event cost | Sluice tmpfs 4KiB ops are the tiny-homogeneous regime. R4's own data reproduces both literature faces: CPU −19.6%/tput +24.5% (batching pays) AND p99 +21.85 (batching costs tail) at d16; but p99 −12.9 at d4 and −30.4 on btrfs (sign flips, T:73-78, 95) | The depth dimension (d2/d4/d16/d64) already measured | None new: use the depth sweep as the tradeoff map; the action is choosing per-regime conclusions, not a new mechanism | None | ZygOS's SLO-choice discussion (§7) predicts the winner flips with the SLO axis, not only with load; if Sluice's flip point (d4→d16) tracks completion-path speed rather than offered load, the coupling is completion-side, consistent with hypothesis 1/3 over admission-side delay | **Adopt (as interpretation frame)**: batching is the right family for this regime; the question is which coupling it creates, not whether to batch |
| 5 | Shinjuku: quantum preemption of a *running* request — dispatcher timestamps each running request, preempts after 5-15µs "**assuming the queue is not empty**" (NSDI'19, §3.4 SQ policy) | There is a running victim whose interruption frees capacity for a queued beneficiary; preemption point exists at arbitrary execution points (IPI, 298/1212 cycles, §3.2-3.3) | After `io_uring_enter`, SQEs are kernel-owned: Sluice has no userspace preemption point over submitted work. The only boundary is between presentations (before submit). Shrinking quantum (S: 8→2) slices *future* submissions; it interrupts nothing | `outstanding()==0` (read live at presentation, r3_ready_batch.hpp:469-470, 503) and the stale pressure snapshot (row 7) | None that is preemption. The Shinjuku-derived *discipline* is portable: act only when a beneficiary exists — i.e., gate any shrink on "waiting work exists now", which S lacks and SNP inverts | None (it is a predicate change in the bench adapter, analysis-stage) | A shrink decision taken at `outstanding()==0` (SNP) has no beneficiary by definition; SNP's p99 (+34.48 at only 35% shrink share, T:50-65) is the observed cost with zero benefit. Conversely any shrink gated on measured same-item wait (row 7) should fire ≪1% at d16 saturation | **Adopt the precondition discipline; reject batch-shrink-as-preemption** — shrinking SQE count is not preemption (ask's guardrail confirmed by mechanism structure) |
| 6 | Shinjuku/Perséphone: preemption has a floor cost — Shinjuku spends ≤10% worker throughput to preempt every 5µs (NSDI'19, §3.2-3.4); Perséphone: for a 1µs request "even preempting as frequently as every 5µs introduces a 6x slowdown" (SOSP'21 submission §3, Paper #344) | Preemption machinery amortizes only when victim run length ≫ quantum | Sluice's "quantum" (presentation cap) is nearly free (SHADOW read cost: CPU +1.02%, p99 +1.22, R:182; R−B: re-entry cheaper than suffix singles, T:107-109) but buys no interruption. S paid +8.51% CPU *and* +10.52% p99 vs R (R:187) — cost without the mechanism | Counters exist (S−R, S−Q8, R:183-188) | Reject S-family: the quantum action is cheap, but it is not acting on any running work, so its cost is pure overhead paid by every op | None (rejection) | Any future policy whose tail-targeting action is "present fewer now" will pay per-op overhead on the p99 members too; if a variant ever improves p99 at d16 this way alone, this row's reasoning is refuted | **Adopt (rejection rationale for S/Q2 as tail fixes)** |
| 7 | Shenango: congestion signal = *the same item still queued across two consecutive 5µs samples* — Algorithm 1 compares current queue contents to `prev_runq`/`prev_inq`; "the duration of queuing is a more robust signal than the length of a queue" (NSDI'19, §4.1.1) | A sampler running every 5µs on a dedicated busy-spinning core; item identity (head vs previous tail) is tracked | Sluice's `completion_pressure` = "previous owner pass completed > 0" (r3_ready_batch.hpp:474-476) — a *flow-positivity* signal. In a saturated pipeline completions are always > 0, so it fires 99.92% of the time (R:199) and carries no discrimination; it is not equivalent to persistence-across-samples (ask's guardrail confirmed) | The presentation loop already sees the ready span twice in a natural way (untouched suffix across cap re-entries); a same-item wait age can be derived there without new threads or timers | Analysis-stage eligibility predicate: "an op that was eligible at the previous presentation is still unserved now" or item wait time > threshold µs — computed at the existing decision point in the bench adapter | Test-only; if promoted, it is a derived summary (mechanism-justification gate: no new authority) | At d16 saturation, a persistence signal should fire rarely (queues drain within one presentation cycle: mean ready 7.92 presented 2.00 refers to S's clamp; R presents ~8 of ~8). If it fires >50% at d16, there IS a stuck backlog and S's shrink had beneficiaries — refuting row 5's application to S; if it fires ≈never and S still differs from Q8, the signal story is confirmed | **Adopt (as the replacement eligibility hypothesis)** — it is the direct Shenango-corrected version of what S tried to sense |
| 8 | Shenango: the sampler itself — one dedicated busy-spinning IOKernel core polling queue heads every 5µs (NSDI'19, §3.1, §4.1.1); oscillatory add/park per tick is "by design" because 600ns context switches amortize | A spare core + acceptance of oscillation | Sluice: no dedicated core allowed (hard constraint: no dedicated core/new thread/background timer); facts travel only on the owner's existing passes | DecisionFacts channel (bench_w03_external_loop.cpp:154-158, 251-256) | None legal. The closest legal approximation is the stale snapshot already present — which is why the snapshot's age matters (row 9) | n/a | n/a | **Defer (mechanism forbidden by round constraints)**; only the signal *design* transfers (row 7) |
| 9 | (Cross-cutting, from the facts-channel structure; no single paper) Staleness: any signal consumed at decision time must describe decision time | Shenango's two samples are 5µs apart by construction | Sluice's `DecisionFacts` describe the owner's *previous* terminal pass, of unbounded age (r3_ready_batch.hpp:137-140; bench_w03_external_loop.cpp:241-244, 415-424); moreover the published `last_pass_immediate_work` is structurally always false (loop-break condition), so the signal is strictly weaker than designed | Pass counters (`shared.passes`, `last_pass_completed`) exist; snapshot age (presentations since last pass) is derivable | Analysis-stage: record snapshot age per decision (count of presentations since the last facts refresh) — a counter, not a mechanism | Test-only counter | If snapshot age at S's clamp decisions is large/variable and clamping correlates with old facts (not current state), S's failure is partially a staleness artifact; if S's decisions track current outstanding anyway, staleness is immaterial and row 5/7 carry the whole story | **Adopt (measurement first)**: one counter distinguishes stale-snapshot vs indiscriminate-signal contributions to S's failure |
| 10 | NAPI: budget-exhaustion protocol — a poller that exhausts its budget returns exactly `budget` and "the NAPI instance will be serviced/polled again (without the need to be scheduled)"; completion path calls `napi_complete_done()` and only then unmasks IRQ; the exact-budget edge must be reported as `budget-1` or left for the next poll (kernel 6.12 docs, Datapath API / Scheduling and IRQ masking) | Continuation responsibility lives in the infrastructure: the return value *is* the continuation protocol; IRQs stay masked while polling continues | Sluice's counterpart of "return budget" is `ReadyBatchStop::cap` + immediate suffix re-entry (r3_ready_batch.hpp:557-570) — continuation is paid inline (extra presentation boundary per refill), whereas NAPI defers to the next scheduled poll without shrinking anything | `r4_cap_reentries` counter (6,170/invocation for R, T:47) | Keep R's re-entry (it already beat B's suffix singles on every percentile, T:107-109); the NAPI lesson applies to the *drain* end: at empty, NAPI does a bounded number of extra polls (`napi_defer_hard_irqs`) then returns to IRQ — it never shrinks a batch | None | Hypothesis 2 (cap-truncation restart cost) predicts d64 (more cap re-entries) widens p99 spread — observed (T:77-78); if per-cap re-entry cost is constant, restart cost is linear in re-entry count and should show in the entry→enter accounting once instrumented (row 2 build) | **Adopt (interpretation)**: hypothesis 2 remains live and is the cheapest to falsify with row 2's instrumentation |
| 11 | NAPI drain-phase policy: at the empty state, do bounded repolls then fall back to interrupts (`gro_flush_timeout`, `napi_defer_hard_irqs` = consecutive empty polls before giving up); busy polling "trades off CPU cycles for lower latency" (kernel 6.12 docs, Software IRQ coalescing / Busy polling) | The system has a cheap idle action (repoll once more) and an event fallback (IRQ) | Sluice at `outstanding()==0` (drain): SNP chose *smaller* restart batches (quantum 2) — an action on future work at the one moment no work is in flight. TAIL-LATENCY's reading: zero-outstanding moments coincide with drain phases and hit "exactly the requests that become the tail" (T:58-65) | `outstanding()==0` live read (SNP used it) | The NAPI-analogous legal action at drain is: present the full eligible prefix (restart big, not small), or return to event-driven parking (already the harness behavior — both threads park, bench_w03_external_loop.cpp:1-2) | None (SNP's clamp2 removal is a deletion, not an addition) | If replacing SNP's clamp2-at-zero with full-prefix presentation moves p99 from +34.48 toward Q8's +23.33 (or below), the "wrong moment" mechanism is confirmed; if it stays at S level, the drain-phase story is refuted and hypothesis 1/3 must explain it | **Adopt (as the SNP falsifier)** |

## 3. Verdict on the R4 failures (multi-cause, mechanism-level)

**S (and SNI, Q2): indiscriminate signal + stale snapshot + action not acting
on the target — all three, with the signal defect primary.** (a)
*Indiscriminate*: `completion_pressure` fires 99.92% (R:199) because it is
flow-positivity ("previous drain pass completed > 0", r3_ready_batch.hpp:474-476),
which is always true under saturation; Shenango's design (row 7) shows the
intended signal class was same-item persistence, a different object. S
degenerates to fixed Q2, and S ≈ Q2 to +0.83% (R:186) — the measured
signature of a no-op condition. (b) *Stale*: the fact is one owner-pass old
with unbounded age, and its `immediate_work` half is structurally always
false when published (row 9). (c) *Not acting on the target*: quantum-2
slicing cannot interrupt any submitted SQE (row 5); it throttles pipeline
refill, which matches S's whole-distribution right shift (p50 +13.03, T:24)
rather than targeted tail relief.

**SNP: action not acting on the target (wrong moment).** Its live
`outstanding()==0` trigger fires exactly when no beneficiary exists; NAPI's
drain protocol (row 11) never responds to emptiness with smaller batches.
SNP's p99 (+34.48) at S level with only 35% small presentations (T:50-65)
is the observed cost-benefit inversion.

**R and Q8 (the p99-only regression): the standing classification is "action
does not act on the target object", with a genuine tradeoff NOT excluded but
not established.** The literature establishes that a bounded-batch
throughput-vs-tail tension *exists* in this regime (ZygOS §3.3 disabled IX
batching because it hurt tail; row 4), so "inevitable tradeoff" is a live
candidate. But it equally shows the tension is conditional, not a law: IX
reports no tail cost when the batch never delays pending work (row 1) — and
R/Q8 never delay pending work (presentation is `min(cap, ready)`,
r3_ready_batch.hpp:497), yet still regress p99 24/24 blocks (R:75). R4's own
sign flips (d4 and btrfs improve the tail under the same batching, T:73,
95) point the coupling at completion-side objects — the three preregistered
hypotheses (T:118-126) — which the batching action does not touch. Rows 2
and 10 give the discriminating measurements. R's own action is cheap (row
6), so "execution too expensive" does not apply to R.

**The literature remedies for a true tail fix: too expensive or structurally
unavailable under this round's constraints — this is the "execution cost"
finding, and it attaches to the alternatives, not to R.** Preemption needs a
preemption point that does not exist over submitted SQEs plus IPI/dispatcher
machinery (Shinjuku §3.2-3.5; forbidden: new threads/cores); Shenango's
sampler needs a dedicated busy-spinning core (§3.1; forbidden); Perséphone's
DARC reserves cores and idles them by design at a measured cost (row below
and §3.3), needing an application-payload type classifier (§4.2 request
filters) that uniform-4KiB bench ops do not expose — and 4KiB uniformity of
*length* says nothing about service-time dispersion, so neither the
classification nor a bytes-ordered short-job rule is licensed (ask
guardrail; no classifiable evidence in R4 data — PROT-64K eligibility was
all-immediate, R:430); work stealing has no legal target (row 3). Busy
polling and friends are explicitly CPU-for-latency trades (row 11; NAPI
docs) and stay rejected.

### 3.1 Adopted (hypotheses/predicates for R5, all analysis-stage)

1. Shenango-corrected eligibility: same-item persistence / measured wait age
   at the existing decision point (row 7) — replaces `completion_pressure`.
2. Shinjuku's beneficiary precondition: never shrink at
   `outstanding()==0`; drain action = full-prefix presentation or park
   (rows 5, 11).
3. Two falsifying measurements for R's p99: per-position completion timing
   inside presented groups (row 2) and cap-re-entry cost accounting
   (row 10); plus snapshot-age counter for S's post-mortem (row 9).
4. Keep R's admission rule (bounded, never-wait presentation) as the
   reference frame (row 1).

### 3.2 Deferred (precondition absent or constraint-forbidden, revisit only
on new evidence)

- ZygOS work stealing (row 3): needs a second executor + stealable queue;
   none exists in W03/W02 shapes.
- Shenango dedicated sampler (row 8): dedicated busy-spinning core is
   outside this round's constraints; only the signal design transfers.
- Any preemption-machinery import (rows 5, 6): no userspace preemption
   point over submitted SQEs; cooperative presentation boundaries are not
   preemption.
- Perséphone DARC-style reservation (§3.3 below): no classifier input, no
   reservable cores, and non-work-conserving idling contradicts the
   resource constraints absent evidence of service-time dispersion.

### 3.3 Perséphone, read for relevance (kept minimal)

Version note: the local copy is the anonymized SOSP'21 submission ("Paper
#344", 8 header hits in the extracted text); the official published version
is verified to exist — "When Idling is Ideal: Optimizing Tail-Latency for
Heavy-Tailed Datacenter Workloads with Perséphone", Demoulin et al., SOSP
2021, ACM, DOI 10.1145/3477132.3483571 (confirmed via MIT DSpace and Google
Scholar records this session) — but byte/content equality of the submission
copy with the camera-ready is unverified. Mechanisms read: request-type
classification via programmer-supplied payload-parsing filters (§4.2),
per-type service-time moving averages + occurrence ratios with queuing-delay
SLO monitoring to update reservations (§3, "Profiling the workload and
updating reservations"), one-way cycles stealing and spillway cores, and the
quantified reservation cost (one reserved worker of 16 ≈ 5% achievable
throughput, §1; ~11,200 cycles per reservation update, §4.3). None of these
transfers to Sluice now: uniform 4KiB length provides no type field,
service-time dispersion in the fixture is unevidenced, and core reservation
is both out of constraints and work-wasting without a classifier.
Retained only as the cost-of-reservation reference point and the
preemption-floor datum (row 6).

### 3.4 Explicitly not adopted (no literature support found under the
resource constraints)

Hedging/duplicate I/O, busy polling (NAPI docs name it a CPU-for-latency
trade; row 11), SQPOLL, thread count increases, deadline-oriented I/O APIs.
None of the five papers supports buying tail latency with more machine work
in a regime where the CPU budget is the established win; IX/ZygOS/Shinjuku
all optimize *within* a fixed resource envelope.

## 4. Source provenance and retrieval honesty

Local copies (sha256, this session):

| Source | Path | sha256 |
|---|---|---|
| IX, OSDI'14 conference paper (camera-ready) | /tmp/r5-papers/ix/ix-osdi14.pdf | b07760bfdc80b47f45741d14a588eacb628d80eefc456bd5fce1e2712615e8b6 |
| Shinjuku, NSDI'19 conference paper | /tmp/r5-papers/shinjuku/shinjuku-nsdi19.pdf | b6196642b99a21e6f1d988a24d8c5916266483437d49fc2bcac07f5f2dc26683 |
| Shenango, NSDI'19 conference paper | /tmp/r5-papers/shenango/shenango-nsdi19.pdf | 6e6a97e0a17a6f33133d1d5a6ba032b69e692fa9cf89a9143b5a3825e8c9e0e8 |
| Perséphone, SOSP'21 submission #344 (preprint) | /tmp/r5-papers/persephone/persephone-sosp21.pdf | 67b1f1407c241898e28b6c1e6c7e36ea33489022e4d2d39f8f92b4d6d92467d8 |
| Linux 6.12 NAPI docs | /tmp/r5-papers/napi/napi.html | d2d122a7d2a2b0d4d04e5a32338e8f9cf75632c2f417c26b3975a2751c5fa3b2 |

Retrieval events (all this session, recorded as they happened):

- IX: first curl timed out at 120s with a truncated file (no %%EOF);
  byte-range resume refused by the server (curl exit 33); a fresh 580s
  download completed the file (380,138 bytes, %%EOF at startxref 379161).
- ZygOS: the ask's URL (marioskoglas.github.io/docs/zygos.pdf) serves a
  GitHub Pages "Site not found" page — from this host *every* github.io
  site (including abelay.github.io root) returns that page over both IPv4
  and IPv6, so the block is on this host's network path, and the search
  index still lists the dead link. EPFL Infoscience (the official open
  access record, file sosp17-final278.pdf, ~694.61 KB) is behind an AWS WAF
  "Human Verification" challenge for non-browser clients; ACM DL
  (doi/pdf and the Unpaywall-listed ft_gateway.cfm OA link) returns a bot
  block page; web.archive.org does not connect from this host; the
  Semantic Scholar API returned HTTP 429 on three attempts. Resolution: the
  full text was retrieved server-side (tavily extraction) from the mirror
  https://abelay.github.io/6828seminar/papers/prekas:zygos.pdf — the
  SOSP'17 camera-ready text (ACM ISBN header present), hosted on the
  Stanford faculty course site. Extracted key passages are preserved in
  /tmp/r5-papers/zygos/zygos-key-excerpts.txt with the section numbers
  cited in this file. No local PDF copy of ZygOS could be saved; treat the
  exact PDF bytes as unverified, the text as first-source.
- Perséphone: the cis.upenn.edu PDF downloaded cleanly and is the
  anonymized submission; the MIT DSpace camera-ready bitstream returned an
  HTML block page, so camera-ready equality is unverified (§3.3).
- NAPI docs fetched from docs.kernel.org (6.12) and stripped to
  /tmp/r5-papers/napi/napi.txt; quoted lines: Datapath API (budget
  exhausted → "serviced/polled again (without the need to be scheduled)";
  napi_complete_done; budget-1 edge), Software IRQ coalescing
  (gro_flush_timeout, napi_defer_hard_irqs), Busy polling ("trades off CPU
  cycles for lower latency").

Not done this session: no builds, no benchmark runs, no lock sessions (no
timed measurements were needed); the main repo and r4 tree were only read;
the r5 worktrees were not modified.
