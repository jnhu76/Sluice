# G1 optimization candidates (G0 decision record)

```text
[SLUICE-G0-403]
REPORT_VERSION = g0-1
BASE_SHA = dcc46266709e98d760b3be2957d18c09e1e55952
EVIDENCE_SHA = 2a5b85f4691a3d3189b273893f02432368ec5542
STAGE = C — G0 final decision
```

Inputs: `PREREGISTRATION.md`, `MEASUREMENT.md` and the raw JSONL. Only
evidence-backed candidates appear below; the per-candidate "measured evidence"
column names the measured cost, not a profiler hotspot.

## 0. Decision table

| Candidate | Measured evidence | Expected benefit | Semantic risk | Regression gate | Decision |
|---|---|---|---|---|---|
| **C1** Stop generating a progress notification for a completion when the notification source is already asserting readiness | 1.000 eventfd `write` per operation at every depth (strace; 3 writer threads x 20,000 writes / 20,000 ops), while the owner parked only 0.072 times/op at d16/w4 — so the large majority of signals wake nobody | Removes ~1 of ~9.9 syscalls/op plus its kernel work on the ThreadPool path (B-G0-01). Bounded: cannot close the 2.5x inversion | Internal signalling only; must preserve PROG-02 persistent readiness. No public surface change | Full ThreadPool+uring test matrix, progress/wake and observer suites, shutdown lifecycle, mutants; **progress/observer model re-validation** | `G1_CANDIDATE_READY` |
| **C2** Remove global-mutex `occupancy()` queries from hot paths in favour of a derived, non-authoritative counter | `occupancy()` is 1.1% self in the w4 profile and takes the core mutex; called from `poll_progress`, `outstanding` and the worker completion path (B-G0-02/B-G0-04). Scan length alone is 4.3% | Removes one shared-mutex acquisition per completion/query from the path that the inversion lives on. Expected: a fraction of B-G0-02, to be measured | Must stay derived and non-authoritative; `outstanding()` public meaning unchanged | REQ-05 reclaim + publication suites, `request_core_protocol_test`, `request_core_publication_test`, both backend conformance suites | `G1_CANDIDATE_READY` |
| **C3** Change the granularity of the single `RequestCore::mutex_` | B-G0-02 inverted as: the *contention* explanation is refuted (contended rate 4.93% in the fastest ThreadPool config vs 0.0001% in the slowest, 0% for the fastest path of all), leaving coherence/stall traffic as the leading explanation | Unknown after the refutation | HIGH: the mutex is the atomicity authority for acceptance, publication and reclaim | Would require the full REQ-01..06 evidence set plus model correspondence | `MORE_EVIDENCE_REQUIRED` — and the original premise is withdrawn; see §3 |
| **C9** Reduce the number of separately locked steps per operation (18-45 acquisitions per 4 KiB read on every backend) | B-G0-06: 18.6 acquisitions/op with **zero** contention on the fastest path (io_uring d16), 21.2 (w4), 32.4 (w1), 45.0 (d1/w1); cache-misses/op grow 154→550 as threads are added | Fewer coherence events and a shorter submit-to-consume path per operation; the only candidate that targets the structural per-op cost rather than an execution policy | Moderate: every removed/merged step must keep the invariant it protected; a wrong merge is a correctness defect, not a slowdown | REQ-01..06 + publication/reclaim suites + both backend conformance suites + mutants | `G1_CANDIDATE_READY` (investigation-level: the first step is writing down the per-step invariant accounting, not changing code) |
| **C4** RequestScope per-`take` slot scan | 3.2-4.4% (`-scope64` vs depth-matched control) | ≤4% | Low | Scope suites | `NOT_WORTH_OPTIMIZING` |
| **C5** External-loop integration gap | uring d16: 144,144 (W-03) vs 303,295 (W-02) ops/s; threadpool 97,284 vs 112,027 | Unknown | Unknown | — | `MORE_EVIDENCE_REQUIRED` (B-G0-05: gap measured, cause not attributed) |
| **C6** io_uring queue depth > 16 | d64 288,424 ops/s (34.2% spread) with p95 428,113 ns vs d16 303,295 (6.8%) with p95 67,466 ns | None; deeper is neutral on throughput and 6.4x worse on p95 | — | — | `NOT_WORTH_OPTIMIZING` (execution-policy finding, must not become a contract) |
| **C7** W-01 direct path | 1.00 `pread64`/op, p50 711 ns for 4 KiB on tmpfs | None identified | — | — | `NO_OPTIMIZATION_JUSTIFIED` |
| **C8** Allocation/result-storage cost per request | No allocation counter exists (NOT_OBSERVABLE); RSS recorded per repetition and not a differentiator | Unknown | — | — | `MORE_EVIDENCE_REQUIRED` (no evidence either way) |

No candidate requires amending the v1 root: C1, C2 and C3 are internal
mechanisms behind an unchanged public contract. If any of them turns out to need
a public-behaviour change, it leaves #403 and becomes a root amendment first.

## 1. C1 — suppress redundant completion notifications

* **Real bottleneck.** B-G0-01: a fixed per-operation cross-thread handshake on
  the ThreadPool path. Measured: exactly one eventfd write per completed
  operation (independent of depth), ~2 drain reads and one `poll` per parked
  wait, and ~5 futex per operation; totals ~9.9 syscalls/op against 1 syscall/op
  for direct execution and 1 `io_uring_enter`/op for uring.
* **Affected workload.** W-02 and W-03 on ThreadPool; W-01 unaffected.
* **Where the improvement comes from.** The completion path currently
  materialises a notification per completion. When the notification source is
  *already* asserting readiness (which the eventfd counter does), an additional
  write adds a syscall and kernel work without changing what the owner will
  observe.
* **Metrics that could get worse.** Nothing structurally; the risk is a lost
  wake, which is a correctness failure rather than a performance regression. If
  suppression is implemented with a shared atomic, it adds one shared cache line
  to the completion path, which could slightly worsen the contention in B-G0-02.
* **Minimal implementation.** In the progress source's signal path, suppress the
  write when readiness is provably already asserted, under the documented
  arm/acknowledge protocol. No public API change, no new state that is not
  derived from the existing counter.
* **Requirement IDs.** PROG-01, PROG-02, PROG-03, REQ-06, W-02, W-03.
* **Correctness tests that must keep passing.** The no-lost-wake regressions
  (`threadpool_progress_race_test`, `uring_progress_race_test`), the external
  loop suites (`threadpool_external_loop_test`, `uring_external_loop_test`), the
  coalescing/saturation cases inside them, `shutdown_lifecycle_*`,
  `public_request_*`, and both `backend_conformance_e1_*` suites — plus the
  released-scope cases that depend on `signals_physical_progress()`.
* **Effect on formal correspondence.** This directly touches the PROG-02
  persistent-readiness obligation, so the progress/observer model correspondence
  must be re-checked. **#524 (RequestCore.tla ReleaseBind convergence) does not
  cover the progress-source signal protocol and its result must not be reused as
  evidence for this change.**
* **Decision.** `G1_CANDIDATE_READY`.

## 2. C2 — derived occupancy instead of mutex-guarded scans on hot paths

* **Real bottleneck.** Part of B-G0-02: the global core mutex is acquired on
  every completion and every progress/outstanding query, and each acquisition is
  accompanied by an O(capacity) slot scan (`RequestCore::occupancy()`, 1.1% self
  in the w4 profile, called from `ThreadPoolBackend::poll_progress` line 587,
  `ThreadPoolBackend::outstanding` line 774 and the worker completion path lines
  834-846).
* **Affected workload.** W-02/W-03 on ThreadPool at depth ≥ 4 with workers ≥ 4;
  the uring backend also calls `poll_progress` but has no worker contention.
* **Where the improvement comes from.** Removing an O(capacity) scan *and* its
  mutex acquisition from the completion and progress paths shortens the section
  that all workers and the submitter serialise on. The scan length alone is worth
  4.3% (measured); the mutex acquisition on top of it is the part that matters
  for contention.
* **Metrics that could get worse.** `outstanding()`/`admission` diagnostics could
  become momentarily stale if the derived counter is not maintained exactly at
  the same linearization points; a wrong counter would be a correctness defect,
  not a slowdown.
* **Minimal implementation.** Maintain a derived live-count alongside the
  authoritative slot table, updated inside the existing critical sections, and
  read it without the mutex on the query paths. Classified as a *derived summary*
  (never an authority): the slot table remains the sole source of truth, and no
  publication/reclaim decision may read the counter.
* **Requirement IDs.** REQ-01, REQ-04, REQ-05, REQ-06, PROG-01.
* **Correctness tests that must keep passing.** `request_core_protocol_test`,
  `request_core_publication_test`, `request_core_ownership_*`, both
  `backend_conformance_e1_*`, `request_scope_*`, `submit_concurrency_*`, plus the
  REQ-05 reclaim regressions that pin slot reuse.
* **Effect on formal correspondence.** `outstanding`/occupancy is part of the
  RequestCore state that the model reasons about; if the counter is derived and
  non-authoritative the model need not change, but the mapping must be re-checked
  to prove no decision depends on it. **#524's result is not evidence for this
  change.**
* **Decision.** `G1_CANDIDATE_READY`.

## 3. C3 — RequestCore mutex granularity (premise withdrawn)

* **What changed.** The original premise was "the pipeline serialises behind one
  lock and contention stalls dominate". The LD_PRELOAD lock probe refutes it: the
  contended-acquisition rate is **4.93% in the fastest ThreadPool configuration**
  (d16/w1) and **0.0001% in the slowest** (d1/w1), and the fastest path of all
  (io_uring d16) has **zero** contended acquisitions. Contention is
  anti-correlated with slowness, so "make the mutex finer-grained because
  contention hurts" is not an evidence-backed direction.
* **What survives.** B-G0-02's inversion and stall counters are still real and
  still unexplained: 2.5x throughput loss from w1 to w4 with constant
  instructions/op, 2x cycles/op, 3.6x cache-misses/op and a 33% recovery from
  pinning to fewer cores. The lead explanation is now coherence/stall traffic
  from many shared-cache-line touches per operation, i.e. C9's territory rather
  than "reduce blocking".
* **Why it is still not ready.** Cross-core invalidation counts and lock hold
  time remain NOT_OBSERVABLE here (`perf lock` unavailable at
  `perf_event_paranoid=2`; no hold-time instrumentation). Proposing a lock
  redesign without those measurements would be the "guess the hotspot, rewrite
  the protocol" failure the issue forbids.
* **If it ever proceeds.** It must preserve every REQ-01..06 invariant, keep the
  publication/borrow boundary intact, and land with the full mutant and
  conformance evidence.
* **Decision.** `MORE_EVIDENCE_REQUIRED`, with the original contention premise
  explicitly withdrawn.

## 3b. C9 — fewer separately locked steps per operation

* **Real bottleneck.** B-G0-06: the request path takes **18-45 mutex
  acquisitions per 4 KiB read** on every backend, including 18.6 on the
  single-threaded io_uring path where none of them can be contended. Each
  acquisition is a read-modify-write on a shared cache line, and adding threads
  multiplies the number of cores touching those lines: cache-misses/op rise
  154 → 261 → 550 from w1 → w4 → w8.
* **Affected workload.** W-02/W-03, both backends; W-01 is unaffected (one
  `pread64`, no shared state).
* **Where the improvement comes from.** Fewer coherence events per operation and
  a shorter submit-to-consume path. This is the only candidate that attacks the
  structural per-operation cost; every other candidate either targets an
  execution policy (C4, C6) or a single lock step (C2).
* **Metrics that could get worse.** Merging steps enlarges individual critical
  sections; if a merged section blocks a worker longer, tail latency could
  worsen even while throughput improves — must be reported per backend, per
  AGENTS.md's "no single microbenchmark generalises" rule.
* **Minimal implementation.** First deliverable is an *inventory*, not code:
  for every acquisition on the submit, dispatch, completion, publication and
  reclaim paths, name the invariant it protects and whether two acquisitions
  already share one linearization point. Only then merge the steps that are
  provably redundant, one at a time.
* **Requirement IDs.** REQ-01..REQ-06, PROG-01, W-02, W-03.
* **Correctness tests that must keep passing.** `request_core_protocol_test`,
  `request_core_publication_test`, `request_core_ownership_*`,
  `backend_conformance_e1_*`, `request_scope_*`, `public_request_*`,
  `request_error_domain_*`, plus the mutant campaign for the touched protocol.
* **Effect on formal correspondence.** Any change to where acceptance,
  publication or reclaim linearize touches the RequestCore model mapping and
  must be re-validated. **The running #524 work is not evidence for it.**
* **Decision.** `G1_CANDIDATE_READY` as an *investigation* (produce the
  invariant inventory and a bounded before/after on one step), not as an
  approved protocol change.

## 4. Requirements-impact summary

| Candidate | Public contract change | Root amendment needed | RequestCore correspondence | #524 relevance |
|---|---|---|---|---|
| C1 | none (internal signalling) | no | progress source, not RequestCore state | **not covered**; must not be reused |
| C2 | none | no | occupancy read path must be shown non-authoritative | **not covered**; must not be reused |
| C3 | none, if invariants hold | no, unless a public behaviour changes | yes, full re-validation | not covered |
| C9 | none, if invariants hold | no | yes for any moved linearization point | not covered |

## 5. Explicit non-findings (things this baseline did *not* establish)

* No evidence that request-slot **allocation** materialises a cost (no counter).
* No evidence that **memory layout/cache locality of the request table** matters;
  the capacity sweeps isolate scan length, not layout.
* No evidence that **io_uring submit/reap batching** is a bottleneck: the uring
  path already runs at 1 `io_uring_enter`/op with 1.0001 syscalls/op total, and
  deeper queues did not help throughput.
* No evidence that **lock contention** is a bottleneck: the contended rate is
  highest in the fastest ThreadPool configuration and zero on the fastest path
  (B-G0-02 alternative (b)). The claim "the mutex is the problem" is
  explicitly *not* supported by G0.
* The measured `W-03` vs `W-02` gap is recorded as a gap, not as a proven
  bottleneck.
* No claim that the WSL2 virtualised storage results generalise to bare metal.
* No per-stage latency split (queue wait vs physical execution vs publication):
  not measured, because no stage clock exists on the public surface. The
  end-to-end submit-to-consume distribution is what was recorded.
