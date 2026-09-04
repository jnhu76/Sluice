# SEMANTIC-DIET-0 Audit — Speculative Semantic Surface

> Research audit for the SEMANTIC-DIET-0 campaign (#227 thesis-rewrite prerequisite).
> Scope: does Sluice's public / architectural / runtime semantic surface contain
> semantics kept mainly for hypothetical future Control / specialization that no
> current correctness, lifetime, ownership, boundedness, product behavior, or
> earned capability requires?

**Date:** 2026-09-04
**Branch:** `research/semantic-diet-0`
**Baseline (audited HEAD):** `eba6e3bb` (master, merged PR #284 — G1-Control closure)
**Governing verdict:** [#283](https://github.com/jnhu76/Sluice/issues/283) — STRONG G1-CONTROL FALSIFIED / RETIRED; Semantic Authority retained candidate-specific only; Control specialization CLOSED BY DEFAULT.

This document is the Phase 0–2 deliverable (reality audit + semantic inventory +
classification + candidate list). It is research evidence, not architecture
authority and not a #227 rewrite.

---

## 1. Reality audit (Phase 0)

| Item | Finding |
| --- | --- |
| master HEAD | `eba6e3bb` == `origin/master` (clean working tree, no extra worktrees/branches) |
| #283 | OPEN (decision record); PR #284 merged 2026-09-04 → verdict frozen |
| PR #280 / #281 / #282 / #284 | all MERGED (C0 / COPY-X0 / BATCH-X0 / G1-Control closure) |
| #227 | OPEN; thesis rewrite explicitly pending "after SEMANTIC-DIET-0" |
| v0.0.1 baseline | tag `a38df5e` — **tag-only versioning** (no machine-readable version field, no semver/API-stability promise; README calls it "Reference baseline") |
| Docs IA | `docs/architecture/` (current authority), `docs/history/` (evidence), `research/*` (campaign evidence) — audited |
| Public API | `include/sluice/` (sync core) + `include/sluice/async/` + `include/sluice/experimental/` — audited |

**Drift vs prompt:** the prompt's governing-context list matches repository truth
(all four PRs merged; closeout doc present). No correction needed.

---

## 2. Method

- Read every public header (`include/sluice/**`, non-detail and detail).
- Grepped public headers for control-oriented vocabulary
  (`hint`, `prefer_*`, `allow_reorder`, `allow_coalesce`, `ExecutionClass`,
  `ResourceClass`, `SchedulingHint`, `priority`, `policy`, `reserved`,
  `deferred`, `not implemented`, `future`, `optimiz`, `specializ`, `capability`,
  `supports_*`).
- Read actual callsites/tests for every candidate (not just names).
- Grepped current-authority docs (`docs/architecture`, `docs/reference`,
  `docs/roadmap`, `docs/design`) for Control/specialization/optimization
  overclaims; scanned `research/*` + `docs/history/` separately as evidence.
- Checked v0.0.1 tag contents for the frozen-surface claim.

---

## 3. Semantic inventory and classification (Phase 1 + 2)

The evidence table below covers every relevant semantic element found. Columns
follow the campaign requirement (§7). "Location" is the public header (or doc)
that owns the contract; "Existing Consumer" cites concrete in-repo consumers.

| ID | Surface | Location | Current Contract | Existing Consumer | Correctness Need | Resource Need | Product Need | Control-only? | Classification | Proposed Disposition | Evidence |
| -- | ------- | -------- | ---------------- | ----------------- | ---------------- | ------------- | ------------ | ------------- | -------------- | -------------------- | -------- |
| S1 | `Reader`/`Writer` read_some/write_some/read_vec/write_vec, short-I/O, EOF | `reader.hpp`/`writer.hpp`/`file.hpp` | short reads allowed; 0 = EOF; zero-progress = failure | all apps, tests | yes (contract floor) | no | yes | no | K1/K3 | KEEP | COPY-X0-AUDIT §2.1 |
| S2 | positional `read_at`/`write_at`/`*_at_exact` (no shared-offset mutation) | `file.hpp` | positional I/O does not touch file offset | apps, tests | yes | no | yes | no | K1/K3 | KEEP | file.hpp:92-186 |
| S3 | `SyncableWriter` sync_data/sync_all (flush ≠ durability) | `sync.hpp` | explicit durability boundary | WalWriter, tests | yes | no | yes | no | K1/K3 | KEEP | sync-durability-model.md |
| S4 | `copy_all` + `CopyLimit` (nothing/bytes/unlimited) | `copy.hpp`/`limit.hpp` | natural bounded copy semantics | apps, tests, fuzz | yes | yes (bounded) | yes | no | K2/K3 | KEEP | limit.hpp:1-56 |
| S5 | `CopyStrategy` Auto/Scratch/BufferedFirst (implemented) | `copy_strategy.hpp` | explicit, observable path selection; Auto == BufferedFirst | tests, example, fuzz, bench | yes (no hidden policy) | no | yes | no | K3 | KEEP | copy.cpp:80-102; COPY-X0-AUDIT §4.1 F-5 |
| **S6** | **`CopyStrategy` VectorDeferred/FileRangeDeferred/SendfileDeferred/SpliceDeferred (reserved, NOT implemented)** | `copy_strategy.hpp:26-31` | "reserved slot; NOT implemented" → invalid_state or explicit fallback | tests/example/fuzz only (self-referential) | **no** | **no** | **no** | **yes — future copy mechanisms (vector copy, copy_file_range, sendfile, splice)** | **K5** | **REMOVE** | design-copy-strategy.md:39-40 ("Forward compatibility… reserved enum slots"); COPY-X0 verdict "framework NOT EARNED" |
| **S7** | **`UnsupportedStrategyPolicy` (ReturnInvalidState/FallbackToAuto)** | `copy_strategy.hpp:34-37` | policy for S6 slots | tests/example/fuzz only | no | no | no | yes (exists only for S6) | **K5** | **REMOVE** | copy.cpp:44-63 |
| **S8** | **`CopyOptions::unsupported_policy`** | `copy_strategy.hpp:44` | option selecting S7 | tests/example/fuzz only | no | no | no | yes (only for S6) | **K5** | **REMOVE** | — |
| **S9** | **`CopyDecision::unsupported_requested`** | `copy_strategy.hpp:56` | reports S6 fallback | tests/example/fuzz only | no | no | no | yes (only for S6) | **K5** | **REMOVE** | — |
| **S10** | **`CopyStats::strategy_deferred_rejected_calls` / `strategy_deferred_fallback_calls`** | `measurement.hpp:77-79` | counters for S6 dispatch | tests only | no | no | no | yes (only for S6) | **K5** | **REMOVE** | — |
| S11 | `CopyDecision` requested/selected/reason/used_buffered_fast_path/used_scratch_path | `copy_strategy.hpp:50-57` | observability of real path selection | tests, example | no | no | yes (observability) | no | K1 (AC-1a) | KEEP | copy_buffered_first_strategy_test.cpp |
| S12 | stats structs (Syscall/Buffer/Copy/Sync/Uring/Vector/Async/Pool) | `measurement.hpp`/`blocking_io_pool.hpp` | caller-owned nullable observability | tests, benches, apps | no | no | yes (observability) | no | K1 (AC-1a) | KEEP | AC-1a (#235) |
| S13 | `IoContext`/`BlockingIoContext` open_reader/open_writer | `io_context.hpp` | backend capability boundary (construction seam) | apps, tests | no | no | yes | no | K3 | KEEP | — |
| S14 | `BlockingIoPool` (fixed workers, bounded queue, backpressure) | `blocking_io_pool.hpp` | bounded sync offload | apps, tests | yes | yes (bounded) | yes | no | K2 | KEEP | ADR-024S |
| S15 | `MemoryReader/MemoryWriter`, `FaultPlan/FaultReader/FaultWriter`, `ObservedReader/Writer` | `fault.hpp`/`observed.hpp` | in-memory endpoints + deterministic fault injection + observation | tests, apps | yes (test determinism) | no | yes | no | K1/K3 | KEEP | — |
| S16 | `ReadOp/WriteOp/SyncDataOp/SyncAllOp`, `AsyncIoContext::submit_*/poll/wait_one/cancel` | `async_io_context.hpp` | L1 explicit I/O operations; positional; no deadline | runtime, apps, tests | yes | yes (capacity) | yes | no | K1/K3 | KEEP | ADR-async-io-model |
| S17 | `Completion<T>` terminal state machine (idle/binding/outstanding/publishing/ready/resetting), exactly-once, fail-fast | `completion.hpp` | terminal publication authority; caller-owned address-stable | all async backends, Batch | yes | yes | yes | no | K1 | KEEP | ADR-explicit-io-completion-authority |
| S18 | `RequestHandle` + `request_state` (context, slot, generation) | `request_handle.hpp` | non-forgeable accepted-request identity; stale = not_found | runtime, tests | yes | no | yes | no | K1 | KEEP | ADR-public-request-handle |
| S19 | RequestArena/RequestSlot/RequestKey/generation (detail) | `async/detail/*` | slot lifecycle, generation safety, release authority | backends | yes | yes (capacity) | yes | no | K1/K2 | KEEP | async-request-lifecycle.md |
| S20 | `Batch` + `BatchResultOrigin` (rejected/accepted_and_completed) | `batch.hpp` | grouped completions driver; **NO group-admission/atomicity/non-interleaving authority** | tests | yes (exactly-once per op) | yes | yes | no (contract matches BATCH-X0: no fused-admission claim) | K3 | KEEP | BATCH-X0-REPORT S9 (CURRENT BATCH DOES NOT GRANT GROUP ADMISSION) |
| S21 | `CancelToken/CancelState/CancelGuard/check_cancel`, request epoch | `cancel.hpp` | cooperative cancellation precedence, single-shot per epoch | Future/Group/apps | yes | no | yes | no | K1 | KEEP | ADR-cancel-request-epoch |
| S22 | `Future<T>`/`Group`/`TaskResultSlot` | `future.hpp`/`group.hpp`/`task_result.hpp` | single-task/multi-task awaitables, exactly-once publish | apps, tests | yes | no | yes | no | K3 | KEEP | — |
| S23 | `WaitPolicy`/`ThreadedWaitPolicy`/`EventedWaitPolicy` | `wait_policy.hpp`/`evented_wait_policy.hpp` | physical-wait seam (ADR §3); both policies implemented & used | Future/Group | yes (mechanism separation mandated by ADR) | no | yes | no | K4 | KEEP | ADR-execution-model |
| S24 | Scheduler / worker park-wake / ready routing / admission | `scheduler.hpp` | runtime progress, wake obligations, bounded resources | runtime | yes | yes | yes | no | K1/K2 | KEEP | architecture-constitution AC-N; e9 models |
| S25 | `supports_request_identity` / `has_split_wait_capability` / `has_bounded_split_wait_capability` / `supports_bounded_wait` | `async_io_context.hpp` | real mechanism capability queries with real consumers (Scheduler selects park domain) | Scheduler, runtime | yes | no | yes | no | K4 | KEEP | — |
| S26 | sync/threadpool/fake/uring backends + `SubmitPolicy` (detail) | `*_backend.hpp` | production backends; transactional submit | runtime, tests | yes | yes | yes | no | K1/K2/K4 | KEEP | — |
| S27 | `application_runtime.hpp` (RuntimeBuilder/ApplicationRuntime) | `application_runtime.hpp` | app lifecycle owner; stop/drain/join | apps, tests | yes | yes | yes | no | K1/K2/K3 | KEEP | — |
| S28 | sync primitives (async_mutex/rwlock/condition/semaphore/event/queue/select/fiber) | `async/*` | explicit synchronization; FIFO/no-barging | tests, apps | yes | no | yes | no | K1/K3 | KEEP | e12/e13 audits |
| S29 | `experimental/uring_io_context.hpp`, `uring_write_batch.hpp` | `experimental/*` | gated experimental io_uring mechanisms (stub when no liburing) | research | no | yes (queue depth) | research-only | no | K3/K4 | KEEP | — |
| S30 | docs current-authority claims (Control/specialization/optimization vocabulary) | `docs/architecture`, `docs/reference`, `docs/roadmap`, `docs/design` | — | — | — | — | — | **no overclaim found** | — | KEEP (no diet) | grep scan, §4 below |

---

## 4. Docs-only semantics check (§6.7)

Full scan of current-authority docs for control/specialization/optimization
overclaim language ("enables batching/fusion/specialization",
"explicit semantics give runtime control", "reserved for future optimizer",
"designed for specialization") returned **no overclaim**. The only hit is a
neutral phase-d1 statement about future control *kinds* in io_uring tag
authority, which is not an optimization claim.

- `Batch` docs do **not** claim group admission / atomicity / non-interleaving /
  fusion (BATCH-X0 S9 already corrected the contract; the code matches).
- `Copy` docs describe CopyStrategy as explicit path selection, no hidden
  policy, no performance promise (COPY-X0 §4.1).
- `resource identity` docs do **not** claim fixed-file optimization authority
  (C0: "FIXED-FILE PRODUCTION SUPPORT: NO SUPPORT EXISTS").
- Historical/research documents that record the old hypothesized Control chain
  (COPY-X0-AUDIT F-2, design-copy-strategy.md, reviews, archive) are **kept
  as historical evidence** per campaign §18 — not rewritten.

---

## 5. STOP GATE A result

```text
K5 count:  1 cluster (S6–S10: CopyStrategy deferred-slot mechanism)
K4 public leakage:  none found
Docs overclaim:    none found
```

K5 > 0 and one concrete cluster is identifiable → **do not STOP**; proceed to
Phase 3 candidate minimization for the S6–S10 cluster only.

---

## 6. Candidate list (Phase 3) — the S6–S10 deferred-slot cluster

### 6.1 Before / after contract

**Before:** `CopyOptions{limit, strategy, unsupported_policy}`;
`strategy ∈ {Auto, Scratch, BufferedFirst, VectorDeferred, FileRangeDeferred,
SendfileDeferred, SpliceDeferred}`; a deferred strategy returns `invalid_state`
(default) or explicitly falls back to Auto; `CopyDecision` reports
`unsupported_requested`; `CopyStats` counts deferred rejection/fallback.

**After:** `CopyOptions{limit, strategy}`; `strategy ∈ {Auto, Scratch,
BufferedFirst}`; every strategy is implemented and honored; `CopyDecision`
reports requested/selected/reason/path usage; `CopyStats` counts the three real
strategies. Requesting a not-yet-implemented copy mechanism is no longer
representable.

**Behavior preserved:** all Auto/Scratch/BufferedFirst copy behavior is
byte-identical (same paths, same decision fields, same path/strategy counters).
`copy_all` overload set unchanged.

**Removed obligation:** the ability to *name* an unimplemented future copy
mechanism (vector copy, copy_file_range, sendfile, splice) and the
reject/fallback machinery that exists only to serve those names.

**Consumer audit:** production src/ (dispatch only), tests (self-referential
mechanism tests), example, fuzz target + corpus seeds, api reference docs,
plus evidence machinery that referenced the deleted contract —
`scripts/run-wal-copy-mutations.sh` M-COPY-06 (mutation target = the deleted
deferred-rejection block), the `deferred_reject`/`deferred_fallback` curated
seed generators in `fuzz/gen_seeds.cpp`, and stale deferred comments in
`fuzz/copy_all_fault_fuzz.cpp`. The initial sweep missed the evidence
machinery; the PR #287 corrective retires it (M-COPY-06 removed, 14 → 13
mutants; stale seed generators and comments removed). **No application or
product code consumes the deferred slots.** The g1_control_copy_x0 research
bench defines its own `CopyDecisionX0` and uses only `CopyStrategy::Auto` —
unaffected.

**Risk:** future mechanism addition must re-add an enum value — acceptable on
the current tag-only, no-declared-stability-promise baseline, but it is public
surface growth, not a guaranteed-non-breaking change (no in-repo consumer
exists today; unknown external consumers of the installed headers would see an
addition, which is source-compatible). COPY-X0's "design intent evidence"
(F-2) is historical and stays in research docs; the "observable fallback"
principle (fallback legal only when explicitly requested AND explicitly
reported) remains a documented design rule in the archived design document.

### 6.2 Deletion admission rule (D1–D7)

| Rule | Assessment |
| --- | --- |
| D1 no correctness invariant depends on it | PASS — the reserved slots had no strategy-specific mechanism; a request either returned `invalid_state` (default policy) or, under `FallbackToAuto`, explicitly normalized to Auto and executed a real copy while reporting `unsupported_requested` and a fallback counter. Both behaviors existed only to serve the reservation. Removing the slots removes the request path entirely; retained Auto/Scratch/BufferedFirst behavior is byte-identical. |
| D2 no product-visible behavior depends | PASS — no current in-repo product/application code uses the deferred slots. External source consumers of the public headers are covered by the D4 compatibility disclosure, not here. |
| D3 no user/test/research consumer requires it | PASS — the only test/example/fuzz consumers are self-referential (they test the mechanism being removed); COPY-X0 is closed. |
| D4 no public compatibility promise requires it | PASS with disclosure — v0.0.1 is **tag-only** (no semver/stability promise). The removal is nonetheless a **real public source/ABI surface reduction** (enum values, a public enum type, struct fields; struct layouts change; external source naming the removed symbols stops compiling). It is intentionally accepted and disclosed, not claimed non-breaking. api.md documents the slots as "reserved (not implemented)"; this change updates the reference in the same change. |
| D5 primary rationale is speculative future Control/specialization | PASS — design-copy-strategy.md:39-40: "Forward compatibility. Future strategies (vector copy, kernel zero-copy) can be added as reserved enum slots." |
| D6 removal does not invalidate G1-Safety evidence | PASS — no G1-Safety experiment exists; deferred slots are not safety semantics. |
| D7 simpler replacement preserves remaining behavior | PASS — the three implemented strategies + decision observability are fully preserved. |

### 6.3 Adversarial checks (per campaign §14)

| Check | Result |
| --- | --- |
| A1 test expresses correctness expectation | Tests express the reserved-slot response only; without reserved slots there is no such request path. Removed with the mechanism. |
| A2 fake/real backend obligation | N/A — copy_all is sync core; no backend. |
| A3 cancellation/shutdown implicit dependency | None. |
| A4 published contract? | Documented in tag-only baseline + api.md as *not implemented*; the contract nevertheless had real observable behavior — `invalid_state` rejection (default) or explicit fallback-to-Auto execution with `unsupported_requested` + fallback stats. Removal is therefore an intentional public source/ABI surface reduction (enum set and struct layouts change), accepted and disclosed on the no-stability-promise baseline — not a no-op placeholder cleanup. |
| A5 "not useful for Control → not useful at all" | Not committed — implemented strategies stay; only empty reservations go. |
| A6 breaks G1-Safety comparability | No — not safety semantics. |

**STOP GATE B:** the cluster touches none of the protected categories
(request identity, Completion terminal semantics, cancellation precedence,
ownership/lifetime, resource boundedness, shutdown/drain, publication
ordering) → no STOP. Proceed to minimal implementation.

### 6.4 Files in the change set

Production: `include/sluice/copy_strategy.hpp`, `include/sluice/copy.hpp`,
`include/sluice/measurement.hpp`, `src/copy.cpp`, `src/copy_strategy.cpp`.
Tests: `tests/copy_deferred_strategy_test.cpp` (delete),
`tests/copy_strategy_stats_test.cpp`, `tests/copy_strategy_test.cpp`,
`xmake/tests/core.lua` (drop test target). Example: `examples/mvp_copy_strategy.cpp`.
Fuzz: `fuzz/support/copy_model.hpp`, `fuzz/copy_all_fault_fuzz.cpp`,
`fuzz/corpus/MANIFEST.md`, corpus seeds `deferred_reject`/`deferred_fallback`
(delete). Docs (authority): `docs/reference/api.md`, `docs/reference/api.zh-CN.md`.

Kept (historical): `docs/history/archive/api-audit.md`,
`docs/history/implementation-plans/design-copy-strategy.md`,
`docs/history/reviews/ARCH-FUNDAMENTAL-POSTRX1-REVIEW-AUTHOR-REPORT.md`,
`research/g1-control-copy-x0/COPY-X0-AUDIT.md`.

---

## 7. STOP decisions recorded

- **K1/K2/K3 keep:** all identity, terminal, cancellation, lifetime, boundedness,
  shutdown/drain, and natural operation semantics stay — no G1-Safety
  experimental target is touched (campaign §4 non-inference).
- **K5 remove:** the CopyStrategy deferred-slot cluster only.
- **Deferred to G1-Safety:** nothing — the cluster is not safety semantics.
- **Not removed:** three implemented CopyStrategies, CopyDecision observability,
  all stats structs, Batch, Cancel, Completion, RequestHandle, WaitPolicy seam.
