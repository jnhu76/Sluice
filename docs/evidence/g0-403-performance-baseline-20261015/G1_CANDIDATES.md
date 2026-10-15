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
| **C3** Change the granularity of the single `RequestCore::mutex_` (the suspected cause of the inversion) | B-G0-02: 2.5x throughput inversion w1→w4, 3.1x CPU/op, constant instructions/op with 2x cycles and 3.6x cache-misses/op, futex blocking 28→94 µs | Potentially large (parallel workers currently destroy throughput) | HIGH: the mutex is the atomicity authority for acceptance, publication and reclaim | Would require the full REQ-01..06 evidence set plus model correspondence | `MORE_EVIDENCE_REQUIRED` — lock hold time is NOT_OBSERVABLE on this host; a hold-time measurement must come first |
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

## 3. C3 — RequestCore mutex granularity (blocked on measurement)

* **Real bottleneck.** The suspected cause of B-G0-02's inversion. The evidence
  establishes the *symptom* rigorously (dose-response inversion, constant
  instructions/op with degrading IPC, 3.6x cache-misses/op, futex blocking
  28→94 µs, profile dominated by mutex-guarded core operations) but **not** the
  split between "one lock serialises the pipeline" and "the lock's cache line
  transfers between owners".
* **Why it is not ready.** Lock hold time and contention are NOT_OBSERVABLE here:
  `perf lock` needs kernel support unavailable at `perf_event_paranoid=2`, and no
  public hold-time instrumentation exists. Proposing a lock redesign without that
  measurement would be exactly the "guess the hotspot, then rewrite the protocol"
  failure the issue forbids.
* **What would settle it.** A hold-time/acquire-count measurement from a
  temporary instrumented build (derived test-side instrumentation, not a new
  semantic state), or an equivalent demonstration that the critical sections
  overlap in time; then a bounded experiment that shortens one section and
  measures the effect.
* **If it proceeds.** It must preserve every REQ-01..06 invariant, keep the
  publication/borrow boundary intact, and land with the full mutant and
  conformance evidence — this is a load-bearing protocol change, not a local
  tweak.
* **Decision.** `MORE_EVIDENCE_REQUIRED`.

## 4. Requirements-impact summary

| Candidate | Public contract change | Root amendment needed | RequestCore correspondence | #524 relevance |
|---|---|---|---|---|
| C1 | none (internal signalling) | no | progress source, not RequestCore state | **not covered**; must not be reused |
| C2 | none | no | occupancy read path must be shown non-authoritative | **not covered**; must not be reused |
| C3 | none, if invariants hold | no, unless a public behaviour changes | yes, full re-validation | not covered |

## 5. Explicit non-findings (things this baseline did *not* establish)

* No evidence that request-slot **allocation** materialises a cost (no counter).
* No evidence that **memory layout/cache locality of the request table** matters;
  the capacity sweeps isolate scan length, not layout.
* No evidence that **io_uring submit/reap batching** is a bottleneck: the uring
  path already runs at 1 `io_uring_enter`/op with 1.0001 syscalls/op total, and
  deeper queues did not help throughput.
* The measured `W-03` vs `W-02` gap is recorded as a gap, not as a proven
  bottleneck.
* No claim that the WSL2 virtualised storage results generalise to bare metal.
* No per-stage latency split (queue wait vs physical execution vs publication):
  not measured, because no stage clock exists on the public surface. The
  end-to-end submit-to-consume distribution is what was recorded.
