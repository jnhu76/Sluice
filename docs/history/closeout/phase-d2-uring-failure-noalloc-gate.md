> **HISTORICAL / EVIDENCE — NOT CURRENT AUTHORITY.** Archived from `docs/architecture/` by S0-DOCS (#290, 2026-09-04). Point-in-time record; do not cite as authority for new decisions.

# Phase D2 Uring failure-injection / no-allocation compliance gate

**Status:** COMPLETE — command-backed real-liburing evidence passed on the Phase D2 final head.

**Baseline:** `origin/master` at `e7e8c4c949cfe7c7e0302196282ed09fd37114bf`
(`Merge pull request #79 from jnhu76/fix/uring-internal-testing-guard`), which is the current master
after merged PR #78. Phase D2 is not based on an intermediate PR #78 SHA.

This document is the narrow Gate 0–4 record for Phase D2. It supplements, and does not supersede,
the Accepted explicit-I/O request contract, the Phase D migration plan, the Phase D1 frozen design,
or the Phase D1 permanent-submit-failure audit.

## 1. Scope freeze

Phase D2 adds command-backed Uring evidence for the already-implemented failure and terminal
protocol. It does not redesign that protocol.

| Surface | D2 decision |
|---|---|
| state-machine changes | **NONE** |
| RequestArena authority | unchanged |
| P0-D permanent-submit recovery | unchanged |
| ring topology | unchanged: private ring, one issuer, flags `0` |
| lock order | unchanged: `dispatch_mtx_ -> RequestArena leaf` |
| wake/progress model | unchanged: D1 single-driver `poll()` / `wait_one()` |
| shutdown semantics | unchanged; D4 owns close/drain redesign |
| public API | unchanged |
| production allocations/resources | unchanged |
| test controls | guarded by both `SLUICE_HAS_LIBURING` and `SLUICE_ASYNC_INTERNAL_TESTING` |

D2 does not introduce SQPOLL, `SINGLE_ISSUER`, `DEFER_TASKRUN`, `ATTACH_WQ`,
`BackendWaitSource`, Scheduler/Batch changes, a public `RequestHandle`, or a new lifecycle
authority. Stub mode remains build/API evidence only.

Applicable architecture-constitution rules are AC-1 through AC-7 and AC-9 through AC-15. The
load-bearing D2 rules are AC-3 (transactional acceptance), AC-4 (exactly one terminal), AC-5
(reap-only publication), AC-7 (bounded resources), AC-9 (post-accept liveness), AC-10 (wake
obligation), AC-11 (cancellation), AC-12 (locks), AC-13 (failure injection), AC-14 (testing
fidelity), and AC-15 (honest conformance).

Zig classification is unchanged: source-derived design reference only. D2 adds no Zig
conformance claim and no new divergence-registry entry.

## 2. D2 gap audit (before implementation)

The strengths below classify evidence on the baseline above. `FULL` means the existing test
already exercises the relevant production machinery strongly enough for the D2 row; `PARTIAL`
means useful evidence exists but the D2 exit claim still lacks a required detector; `NONE` means
no Uring-specific detector exists.

| C2d requirement | Existing Uring evidence | Strength | Missing work |
|---|---|---|---|
| pre-commit rejection zero residue | `uring_length_over_uint_max_rejected_no_residue` drives real descriptor validation after reserve and observes idle Completion, zero outstanding, and zero slot use; constructor configuration rejection is also pre-allocation | PARTIAL | cover natural reserve/capacity rejection, binding-CAS loss, size and void descriptors, and observe dispatch/router/ledger/SQ residue; record structurally infallible stages as N/A rather than inventing production failures |
| post-commit accepted terminal guarantee | real write/CQE tests and every D1 transport/recovery case retain accepted requests to a real or proven Class-A terminal and publish through reap | PARTIAL | run the accepted terminal windows with the userspace allocator forced to fail |
| transient submit failure preserves request | `uring_submit_transient_error_recovers_on_next_poll` and `uring_wait_transients_never_return_false_drained_boundary` cover `EINTR`, `EAGAIN`, and `EBUSY` without a fabricated terminal | FULL | cite in D2 command evidence; do not duplicate |
| zero-progress preserves request | `uring_submit_zero_progress_does_not_change_request_state` | FULL | cite in D2 command evidence; do not duplicate |
| partial-return cannot drive lifecycle | `uring_scripted_partial_return_does_not_mutate_request_state` reports a positive prefix after a real submit and proves both requests retire from original CQEs; its claim is correctly limited to reported-prefix lifecycle neutrality | FULL | cite with its existing limited claim; do not call it deterministic kernel partial consumption |
| permanent submit Class-A recovery | `uring_permanent_submit_failure_retires_physical_batch_and_local_fifo` | FULL | add allocator-failure coverage around the same production recovery controller |
| possibly-consumed/Class-C work retained | `uring_poison_wait_drains_old_kernel_work_without_resubmitting_class_a` keeps an older real submitted pipe read bound while a later Class-A write is locally retired | FULL | reuse as mixed Class-C/Class-A and mutant detector evidence |
| cancel vs recovery terminal winner | `uring_class_a_control_suffix_releases_deferred_class_c_operation`, `uring_class_a_operation_and_control_retire_exactly_once`, and `uring_original_cqe_waits_for_matching_control_quiescence` cover important running/control orderings | PARTIAL | add pending/enqueued cancel versus Class-A recovery and repeated running cancel under transient/permanent transport failure; prove one publication and bounded one-control state |
| no post-accept unbounded allocation | construction-time vectors, bounded dispatch/router/ledger, and `noexcept` accepted-path code provide a strong static argument, but no Uring allocator-failure detector exists | NONE | add an always-throw userspace allocation probe across ordinary real, permanent Class-A recovery, and cancel/control accepted windows |
| size op | real read/write, transport failure, mixed recovery, and cancel tests are size-operation based | FULL | add size accepted-window allocation detector |
| void op | production `submit_void` uses the same arena/dispatch/recovery machinery, but no Uring C2d runtime case drives sync-data/sync-all | NONE | add void pre-commit and accepted/recovery allocation evidence without duplicating every size case |
| real liburing | the baseline `--with-liburing=true` Debug gate executes `uring_submit_failure_test`, `uring_backend_test`, and `uring_backend_death_test` on the real path | FULL | execute the new D2 target only as real C2d evidence and record commands separately |
| stub honesty | `uring_available_matches_build_mode`, stub submit/wait cases, and the hard-coded KernelIo fail-closed verdict distinguish stub from real execution | FULL | keep the D2 target buildable in stub mode, classify it as build/API-only (not real D2 PASS), and retain the KernelIo fail-closed verdict |

This table is the mandatory pre-coding D2 audit. Existing D1 evidence will be reused where marked
`FULL`; D2 does not rename or duplicate those cases merely to increase case count.

## 3. Gate 0 — authority and invariant reconciliation

### 3.1 Authority chain

1. Accepted ADR decisions 5, 6, 11–14, 18, and 19.
2. Phase D migration plan.
3. Phase D1 frozen design and permanent-submit-failure audit.
4. Architecture constitution and repository operating contract.
5. Current public headers and production source.
6. Existing Uring, ThreadPool C2d, Fake no-allocation, conformance, death, and formal tests.

There is no authority conflict requiring an ADR change. The stale C2d manifest/roadmap text is an
evidence-classification defect after D1, not permission to change the lifecycle.

### 3.2 Frozen state machine

No transition changes in D2:

```text
free -> reserved -> prepared -> pending -> enqueued -> running/ring-owned
     -> backend-ready -> completion-ready -> free with generation increment
```

The acceptance linearization point remains Completion `binding -> outstanding`. A prepared SQE
plus stable router/ledger identity remains the Uring execution-ownership transition. Submit return
counts remain transport evidence only. Permanent negative submit remains a separate proof-consuming
recovery controller.

### 3.3 Stop conditions

D2 stops as `BLOCKED` instead of weakening evidence if a failing detector shows that accepted
terminality requires unbounded post-accept storage, a new request-lifecycle authority,
submit-prefix RequestState semantics, or release of possibly kernel-owned work.

### 3.4 Claim layering — D1 proof vs D2 runtime evidence

Phase D2 keeps the Class-A proof layered exactly where the authority lives. It must not be read
as a second, independent reproduction of the real kernel negative-submit physical state:

```text
D1 proof (docs/history/closeout/phase-d1-uring-permanent-submit-failure-audit.md):
  real non-SQPOLL negative io_uring_submit()
  -> liburing/kernel source theorem: post-flush / zero-consumed
  -> every retained ledger entry is execution-impossible Class-A

D2 runtime (this gate):
  deterministic injected negative submit result (SubmitScript returns the
  staged step; only kRealSubmit calls liburing)
  -> exercises the production P0-D recovery controller verbatim
  -> proves accepted-terminal / no-allocation / cancel arbitration on it

D2 does NOT independently reproduce the real kernel negative-enter
physical state; that remains the D1 source proof's claim.
```

The scripted `-EIO` in `uring_d2_failure_noalloc_test.cpp` never enters `io_uring_submit()`; the
quarantined SQE stays staged in the application-side SQ, which is exactly what the M8
transport-state detector observes. D2 therefore upgrades the C2d record to IMPLEMENTED for the
*recovery-controller behavior* the injected result drives, not for the kernel classification of a
real negative enter.

## 4. Gate 1 — five-stage pre-commit failure matrix

The table distinguishes production-triggerable failure from structurally unreachable defensive
failure. A guarded seam may observe bounded state; it must not create a new production error path.

| Stage | Can fail in production? | Deterministic D2 trigger | Required state after failure |
|---|---|---|---|
| pre-reserve health/admission | yes: unavailable ring, fatal poison, admission closed | existing unavailable/poison cases; D4 still owns explicit close | Completion idle; arena/router/ledger/SQ unchanged |
| reserve | yes: configured request capacity full | fill capacity with a retained accepted request, then submit another request | synchronous `would_block`; candidate Completion idle; no new slot/borrow/dispatch/router/ledger/SQE; original request unchanged; capacity recyclable after its normal reap/reset |
| descriptor validation | yes: negative fd, null non-empty buffer, native length/offset conversion | real malformed size and void descriptors | synchronous `invalid_argument`; reserved slot rolled back; Completion idle; no borrow/dispatch/router/ledger/SQE; capacity immediately recyclable |
| prepare | no for a current reserved handle and supported fixed `OperationKind` under `dispatch_mtx_`; failure would require stale/corrupt internal identity | structural N/A, backed by the single locked call chain and RequestArena transition tests | no artificial production failure is added; existing defensive rollback remains |
| publication binding install | no for the just-prepared current handle and a valid Completion pointer/thunk in this private call chain | structural N/A | no artificial production failure is added; existing defensive rollback remains |
| begin binding | yes: Completion is not idle / another binding CAS wins | submit with an already-bound Completion while spare arena capacity exists | synchronous `invalid_state`; candidate slot rolled back; original binding/request unchanged; no new dispatch/router/ledger/SQE |
| arena commit | no for the just-prepared current handle while the same `dispatch_mtx_` critical section still owns admission | structural N/A; RequestArena separately tests invalid transitions | no artificial production failure is added; defensive `rollback_binding_before_accept` remains present but is not presented as a natural Uring failure |
| Completion binding commit | no: fixed capability install plus release-store; allocation-free/noexcept | structural N/A | this is the public acceptance LP; later failure cannot become rejection |
| enqueue | post-accept and allocation-free/noexcept; it does not return rejection | ordinary, SQ-pressure, local-cancel, and poison paths | accepted request retains one reliable terminal path |

For every triggerable pre-commit rejection, D2 will snapshot and compare:

```text
Completion state
arena slot_in_use / accepted_outstanding
dispatch entries
live router entries
transport-ledger entries
liburing SQ-ready entries
```

No RouterEntry, transport-ledger entry, or SQE exists before post-commit dispatch; those three
domains must therefore be unchanged across every pre-commit rejection.

## 5. Gate 2 — authority, locks, resources, progress, shutdown

### 5.1 Authority and lock table

| Data / transition | Authority | Synchronization | D2 change |
|---|---|---|---|
| request state, borrow, terminal winner, reap/publication | `RequestArena` / slot lifecycle | arena leaf domain | none |
| local dispatch, router install, cancel scratch, physical ledger, poison | Uring backend | `dispatch_mtx_` in the frozen single-driver topology | none |
| SQ/CQ transport | private `io_uring` instance | AsyncIoContext single-driver plus `dispatch_mtx_` for SQ/ledger mutation | none |
| Completion state | Completion publication capability / caller reset | existing atomic protocol | none |
| test observations | read-only guarded methods | invoked only by deterministic single-driver tests | compiled out of production |

The frozen lock order remains:

```text
dispatch_mtx_ -> RequestArena leaf
```

The arena remains a leaf. No D2 code calls user/Scheduler code, waits, allocates, or enters the
kernel while holding the arena domain.

### 5.2 Resource and allocation model

| Resource | Capacity / allocation | Full behavior / reclamation |
|---|---|---|
| RequestArena and prepared op scratch | `request_capacity`, construction time | synchronous `would_block`; reset releases slot |
| local dispatch queue | `request_capacity`, construction time | SQ pressure retains bounded FIFO entry; transfer/cancel/recovery removes it |
| CQE router and free list | `request_capacity`, construction time | one live router entry per ring-owned request; original/control retirement recycles array slot |
| cancel scratch | one bit per request slot, construction time | repeated cancel cannot create a second simultaneously-live control for the request |
| physical transport ledger | actual SQ ring entries, construction time | positive transport pops prefix; permanent recovery marks Class-A retired; Class-C has already left the ledger |
| Completion / fd / buffers | caller-owned | caller keeps them address-stable through completion-ready |

The accepted hot path may use fixed arrays/vectors, bounded scans, atomics, liburing SQ/CQ memory,
and kernel syscalls. It may not require a new unbounded userspace heap allocation. Kernel-internal
allocation is outside this userspace allocation contract.

### 5.3 Wake/progress and shutdown

Progress remains persistent in RequestArena ready state, the local dispatch FIFO, the physical
ledger, the SQ/CQ rings, and the stored poison. The single driver calls `poll()` or `wait_one()`;
poisoned waits use `io_uring_enter(..., to_submit=0, ...)` so quarantined Class-A SQEs cannot be
resubmitted. D2 adds no new sleeper or wake producer.

Destruction remains quiescent and fail-fast for accepted/bound work. D2 does not add implicit close,
cancel, drain, terminal publication, or waiting. D4 remains the close/drain/KernelIo-lift phase.

## 6. Gate 3 — deterministic D2 evidence plan

One dedicated target will separate real and stub modes. In real mode it compiles the authoritative
`uring_backend.cpp` with internal-test controls; in stub mode it proves only that the production
stub builds and reports unavailable. The aggregate gate must record the stub result as
`INCOMPLETE/build-only` for a real-only D2 record, never `PASS`.

New real-liburing cases will prove:

1. triggerable pre-commit rejection matrix with zero residue and immediate recycling;
2. ordinary real size and void accepted paths under an always-throw allocator;
3. permanent-submit Class-A recovery under the same allocator probe, using the production P0-D
   controller;
4. a Class-C original plus Class-A control suffix under allocator failure;
5. repeated `cancel()` under transient submit pressure and permanent poison retains at most one
   live control reference and publishes exactly once;
6. pending/enqueued local cancel is disarmed before a later permanent recovery and cannot be
   overwritten;
7. mode metadata makes real execution and stub build/API evidence mechanically distinguishable.

Existing cases retained as evidence:

- `uring_submit_transient_error_recovers_on_next_poll`;
- `uring_wait_transients_never_return_false_drained_boundary`;
- `uring_submit_zero_progress_does_not_change_request_state`;
- `uring_scripted_partial_return_does_not_mutate_request_state`;
- `uring_permanent_submit_failure_retires_physical_batch_and_local_fifo`;
- `uring_poison_wait_drains_old_kernel_work_without_resubmitting_class_a`;
- `uring_original_cqe_waits_for_matching_control_quiescence`;
- `uring_class_a_control_suffix_releases_deferred_class_c_operation`;
- `uring_class_a_operation_and_control_retire_exactly_once`.

No new formal model is planned because D2 freezes all modeled transitions, admission rules, terminal
winner rules, ledger classification, generation rules, wake rules, and shutdown behavior. The
existing `d1-uring-poison` suite remains the formal evidence and must pass on the final head.

## 7. Gate 4 — final evidence ledger

All implementation evidence below was run against the Phase D2 source after the final test
correction. The allocation probe is armed immediately before public submission, after backend,
ring, file, buffer, Completion, and hook resources are constructed. It therefore covers a
superset of the required accepted window: admission/acceptance, allocation-free enqueue,
transport, terminal recording, reap/publication, and reset.

| Evidence | Final result |
|---|---|
| focused Uring D2 real-liburing target | **PASS** — 10/10 cases; exact metadata `evidence=uring_c2d_failure_injection mode=real` |
| G2 exact pinned case-set (Issue #81 P1) | **PASS** — the manifest now pins the D2 record's required 10-case runtime set; the aggregate gate's `_drive` proves the binary executed exactly that set (each case once, no missing/extra/duplicate) before trusting its `[evidence-meta]` line. Mutants G2-A (delete one case), G2-B (metadata case only), G2-C (extra unpinned case), and G2-D (9 distinct + 1 duplicate) all classify INCOMPLETE; disabling the `_drive` case-set check turns four of these green, proving the tests traverse `_drive` end-to-end |
| pre-commit zero-residue matrix, size + void | **PASS** — natural capacity, descriptor-validation, and binding-CAS rejection; structurally infallible stages remain N/A |
| ordinary accepted no-allocation, size + void | **PASS** — real write and `sync_data` reach original CQE/reap under the always-throw probe |
| permanent Class-A recovery no-allocation | **PASS** — size and void operations use the production P0-D poison/classification/retirement controller |
| Class-C retention + control recovery no-allocation | **PASS** — older kernel-owned work remains bound to its original CQE while only proven Class-A work retires locally |
| pending cancel vs recovery one-winner | **PASS** — local execution is disarmed before cancel wins; recovery cannot overwrite its terminal |
| repeated cancel/control boundedness under transient/poison | **PASS** — repeated cancel retains at most one live control execution reference and reaches one publication |
| existing Uring submit-failure target on final head | **PASS** — 16/16 cases, including transient/zero/positive-prefix neutrality and P0-D Class-A/Class-C cases |
| C2d manifest real-mode record and stub-incomplete classification | **PASS** — manifest self-test: 176/176 (Issue #81 P1 G1 added 6 ambient-filter env-isolation regressions on top of the prior 162; G2 added 8 — a manifest pin well-formedness check plus seven `EvidenceCaseSetPinTest` cases whose G2-A/B/C/D mutants traverse `_drive` end-to-end, with a source↔manifest drift detector); aggregate real record PASS; stub record mechanically INCOMPLETE |
| C2a exact shared capacity audit | **PASS** — the existing `UringConfig` bounds satisfy the unchanged shared `run_capacity_cases`; no production work was needed |
| Clang Debug real liburing | **PASS** — 154/154 test targets |
| Clang Release real liburing | **PASS** — 154/154 test targets |
| Clang Debug stub/off | **PASS** — 152/152 test targets; D2 emits `mode=stub` and is not execution evidence |
| ASan+UBSan real liburing | **PASS** — full test group, no sanitizer report |
| TSan real liburing | **PASS** — full test group, no race report |
| negative-compile authority probes | **PASS** — Completion 12, RequestArena 6, async API 9, async identity 3, external backend authority 2 |
| aggregate backend-conformance gate | **PASS** — Fake/ThreadPool eligible; Uring C2d and capacity PASS; KernelIo remains NOT CONFORMING |
| existing `d1-uring-poison` formal suite | **PASS** — correct protocol; all three broken variants produced their expected counterexamples |
| production seam-absence audit | **PASS** — production `libsluice_async.a` contains no D2 observation/hook symbol |
| doc links / architecture verification / `git diff --check` | **PASS** |
| mutation evidence M1–M11 | **PASS** — every final claimed mutant produced a focused RED result and was restored GREEN |

The Release run exposed a bounded busy-wait in the new test helper. The helper was corrected to use
the production `wait_one()` progress primitive, then passed concurrent stress and every final
gate. This was a test-evidence correction, not a production failure or lifecycle change.

The manifest now carries implemented, real-mode-only `uring_c2d_failure_injection` evidence.
`uring_c2b_identity_not_implemented`, `uring_c2c_borrow_waiter_not_implemented`, and
`uring_c2e_close_drain_not_implemented` remain unchanged. The KernelIo fail-closed verdict remains
in force; Phase D3 and D4 are still pending and full Phase D is not complete.
