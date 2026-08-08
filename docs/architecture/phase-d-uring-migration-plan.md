# Phase D — UringAsyncBackend RequestArena Migration: Audit & PR Decomposition

**Status:** PLAN READY FOR HUMAN REVIEW (Phase D is NOT implemented)
**Date:** 2026-08-08
**Author:** jnhu
**Governing ADR:** [ADR-explicit-io-request-contract](../adr/ADR-explicit-io-request-contract.md) (Accepted)
**Roadmap:** [remediation-roadmap.md](remediation-roadmap.md) — Phase D "NOT IMPLEMENTED"
**Compliance gates:** [phase-b-compliance-gate.md](phase-b-compliance-gate.md),
[phase-c1-conformance-gate.md](phase-c1-conformance-gate.md),
[phase-c2a..c2e-compliance-gate.md](phase-c2a-compliance-gate.md),
[phase-e-compliance-gate.md](phase-e-compliance-gate.md)
**Divergence registry:** [divergence-registry.md](divergence-registry.md) — DIV-02 (Uring excluded
until Phase D), DIV-14 (Uring descriptor validation open until Phase D)

This document is the Phase D0 audit and PR decomposition. It draws the current
`UringAsyncBackend` ownership/identity/SQE/CQE/cancel/failure/shutdown model completely, aligns
it with the Accepted explicit-I/O request contract and the Phase B/C2/Phase E reference
implementations, and proposes a reviewable, per-PR-verifiable implementation decomposition.
**No production Uring migration is implemented by this document.**

The central question this audit answers is not "how to stuff RequestArena into Uring" but:

> How does RequestArena become the SOLE request lifecycle / generation / terminal authority for
> Uring, while io_uring owns only execution ownership — and how do partial submit, cancel, CQE,
> and fatal failure prove that the two never form a dual authority?

---

## 1. Baseline

All facts below were re-verified on 2026-08-08 from a clean working tree.

| Item | Value |
|---|---|
| Branch | `master` |
| Baseline SHA | `1349a6fdf63f760d73cec9d567bb3fecd46fa695` (= `origin/master` = `HEAD`) |
| PR #73 | **MERGED** — merge commit `1349a6f`, mergedAt `2026-08-08T06:28:26Z`, head `25893ce` |
| Working tree | clean (`git status --short` empty) |
| Toolchain | Clang Debug (`xmake f -m debug --toolchain=clang`) |
| Test count | **151/151 passed** (`xmake test -v`, clean stub build) |
| liburing | **2.14 installed** (apt `liburing-dev` + xrepo package; `SLUICE_HAS_LIBURING` compiles/links for `sluice_async` and all uring targets EXCEPT `uring_submit_failure_test` — see finding P-D0-INF-01) |
| Kernel | `6.18.33.2-microsoft-standard-WSL2` (io_uring availability under WSL2: real-path execution to be verified in D1; stub path verified) |
| Uring conformance verdict (stub) | **NOT CONFORMING** — see §16 for the exact gate output |

### 1.1 Baseline commands and results

```text
git fetch origin                                     # OK
git status --short                                   # (empty)
git rev-parse origin/master                          # 1349a6fdf63f760d73cec9d567bb3fecd46fa695
gh pr view 73 --json state,mergedAt,mergeCommit,headRefOid
    # {"headRefOid":"25893ce3561f59586c7143a4539dd60c7571321a",
    #  "mergeCommit":{"oid":"1349a6fdf63f760d73cec9d567bb3fecd46fa695"},
    #  "mergedAt":"2026-08-08T06:28:26Z","state":"MERGED"}

xmake f -m debug --toolchain=clang -y                # configure OK
xmake build sluice_core                              # build ok
xmake build sluice_async                             # build ok
xmake build -g test                                  # build ok
xmake test -v                                        # 151/151 passed, 0 failed
python3 scripts/tests/test_backend_conformance_manifest.py   # Ran 152 tests ... OK
python3 scripts/verify-backend-conformance.py        # Fake ELIGIBLE, ThreadPool ELIGIBLE,
                                                     # Uring NOT CONFORMING (see §16)
python3 scripts/check-doc-links.py --self-test       # SELF-TEST PASS
python3 scripts/check-doc-links.py                   # VERDICT: PASS
python3 scripts/verify-architecture-docs.py          # OK: all architecture documentation checks passed
git diff --check                                     # clean
git status --short                                   # (empty)
```

Note on xmake hygiene: `xmake build <target>` in this environment accepts exactly one target
per invocation and no `-y` flag; two-target invocations error with `invalid argument`. Switching
`--with-liburing` on/off without `xmake f -c` left a stale `uring_backend.cpp.o` (the stub driver
linked against a real-path object) — all validation here is on a `xmake f -c` clean rebuild.

### 1.2 Real-liburing capability (infrastructure gap assessment)

`liburing` 2.14 is present and xmake resolves it through xrepo
(`/home/hoo/.xmake/packages/l/liburing/2.14/854b793fd77d4c9cb429b3fe37569967`). The
`--with-liburing=true` build compiles `SLUICE_HAS_LIBURING` into `sluice_async` and links
`uring`/`uring-ffi` (verified via `xmake show -t sluice_async`).

**Finding P-D0-INF-01 — pre-existing real-liburing link break (baseline, NOT introduced by Phase D):**
`uring_submit_failure_test` (xmake/experimental.lua:63-76) compiles `src/async/uring_backend.cpp`
with `SLUICE_HAS_LIBURING` + `SLUICE_URING_INTERNAL_TESTING` but links **only** `sluice_core`.
The real path references `detail::completion_authority_fail_fast()` (defined in
`src/async/fail_fast.cpp`, which is compiled into `sluice_async`, xmake/libraries.lua:35).
Stub mode never references that symbol, so the default build is unaffected; the real-liburing
build fails to link:

```text
uring_submit_failure_test ... undefined reference to
`sluice::async::detail::completion_authority_fail_fast()'
```

**Phase D consequence:** no real-path evidence can be produced by `uring_submit_failure_test`
until this target links `sluice_async` (or the fail-fast source). This must be fixed inside
Phase D (D1) as part of the test-seam migration; it is a completion blocker for real-path
evidence, not for the audit.

**Finding P-D0-INF-02 — WSL2 real-path execution unverified:** the environment is WSL2
(`6.18.33.2-microsoft-standard-WSL2`). io_uring exists on modern WSL2 kernels but the real
`io_uring_setup` may be restricted (sandbox/seccomp). D1 must verify `available()==true` and
run the real-path suite on this host; if the kernel refuses, real-path evidence is
**UNAVAILABLE on this environment** and the Phase D completion claim must state so explicitly
(the stub build alone never substitutes for real-path evidence — AGENTS.md §16.5).

---

## 2. Governing authority

| Authority | Role for Phase D |
|---|---|
| [ADR-explicit-io-request-contract](../adr/ADR-explicit-io-request-contract.md) | Accepted target contract. Decisions 1–16, invariants I1–I19, linearization points, Uring backend mapping ("RequestKey -> encoded/indirect SQE user_data -> CQE -> validate context/slot/generation -> same RequestSlot backend-ready"), Gate 0–4 obligations |
| [remediation-roadmap.md](remediation-roadmap.md) | Phase D scope ("refactor(async): migrate UringBackend to RequestSlot identity"), dependency D→F, out of scope (F/G) |
| [phase-b-compliance-gate.md](phase-b-compliance-gate.md) | Reference arena/slot lifecycle evidence and the Scheme-B arbitration protocol |
| [phase-c1-conformance-gate.md](phase-c1-conformance-gate.md) | Aggregate gate mechanics; KernelIoProfile NOT CONFORMING rule; per-backend isolation |
| [phase-c2a..c2e-compliance-gate.md](phase-c2a-compliance-gate.md) | The five C2 evidence layers Uring must close (capacity, identity/cancel, borrow/waiter, failure injection, close/drain) |
| [phase-e-compliance-gate.md](phase-e-compliance-gate.md) | The ThreadPool migration evidence ledger — the format and gate pattern Phase D mirrors |
| [divergence-registry.md](divergence-registry.md) | DIV-02 (Uring excluded until Phase D), DIV-14 (Uring descriptor validation open until Phase D), DIV-10 (cancel interruption; Uring cancel via IORING_OP_ASYNC_CANCEL is the DIV-10 revisit trigger) |
| [backend_conformance_manifest.py](../../scripts/backend_conformance_manifest.py) | Single source of truth for evidence records; Uring records `uring_capacity_not_implemented`, `uring_c2b_identity_not_implemented`, `uring_c2c_borrow_waiter_not_implemented`, `uring_c2d_failure_injection_not_implemented`, `uring_c2e_close_drain_not_implemented` |
| [verify-backend-conformance.py](../../scripts/verify-backend-conformance.py) | Verdict computation; **hard-codes KernelIoProfile = NOT CONFORMING and lifecycle/backend_specific = INCOMPLETE for KernelIo** (lines ~557-620) — this rule must be lifted inside Phase D when real evidence exists |
| AGENTS.md | §10 explicit request lifecycle invariants, §12 resource bounds, §13 lock/wake discipline, §14 shutdown, §16.2/16.3/16.5 gates, §22/§23 completion-report obligations |

Every Phase D conclusion below is classified as: **ADR requirement / current Uring behavior /
reference behavior / gap / planned owning PR / evidence required to close**.

---

## 3. Current Uring ownership model (as-built audit)

Source: `src/async/uring_backend.cpp` (743 lines), `include/sluice/async/uring_backend.hpp` (112
lines). Real path compiled under `SLUICE_HAS_LIBURING`; otherwise an unsupported stub that
rejects every submit synchronously with `backend_error`.

### 3.1 Current structure

```text
submit_read/write/sync_data/sync_all
  -> L8 idle check (Completion not idle -> invalid_state)
  -> checked_uring_length (invalid_argument)
  -> try_claim(c)                          # legacy one-stage Completion claim
  -> get_sqe_with_pressure(stats)          # io_uring_get_sqe; on pressure: FLUSH+SUBMIT+retry
       |  (nullptr -> rollback_claim_before_accept(c); return backend_error / fatal)
  -> io_uring_prep_read/write/fsync(sqe,...)
  -> io_uring_sqe_set_data(sqe, next_id)   # user_data = monotonic 64-bit op id
  -> register_op: comp_to_op.emplace(c,id); ops.emplace(id,OpRec); pending_sqes.push_back(id)
  -> next_id++
  -> return {}                              # ACCEPTED (claim committed, maps registered)

poll() / wait_one()
  -> if pending_sqes non-empty: submit_pending() -> process_submit_result(rc)
  -> reap_ready(): io_uring_peek_batch_cqe -> per CQE:
       id = cqe user_data
       if id in cancel_to_op:              # cancel CQE — informational only
           clear cancel_requested/cancel_op_id on target OpRec; erase cancel_to_op; DROP
       else: reap_op_cqe(id,res):
           publish Completion exactly once (success/error/-ECANCELED -> canceled)
           ops.erase(id); comp_to_op.erase(c)
  -> wait_one additionally: io_uring_submit_and_wait(1) when pending; io_uring_wait_cqe when idle
  -> deferred_ready surfaced as completion count

cancel(c)
  -> op_id_for(&c) via comp_to_op (O(1) reverse index)
  -> cancel_requested flag (exactly-once intent)
  -> submit_cancel: get_sqe_with_pressure; io_uring_prep_cancel64(sqe, op_id, 0);
       cancel_id = next_id++; set_data(cancel_id); cancel_to_op.emplace(cancel_id, op_id);
       pending_sqes.push_back(cancel record)

fatal submit failure (process_submit_result):
  -> transient (-EINTR/-EAGAIN/-EBUSY) or one zero-progress: retry later (suffix retained)
  -> permanent error / repeated zero / impossible accepted count:
       enter_fatal: fail_unsubmitted() completes provably-userspace-owned suffix ops
       (directly publishing their Completions); later submit rejects with the stored error;
       poll() keeps reaping CQEs; wait_one() returns the error without blocking
```

### 3.2 Per-structure authority table (task §3)

| Structure | Identity authority | Lifecycle authority | Execution ownership | Completion publication | Generation validation | Stale-event rejection | Capacity bound | Request-time allocation | Kernel-ownership transition | Phase D disposition |
|---|---|---|---|---|---|---|---|---|---|---|
| `OpRec` (per-op record) | Monotonic id → op (parallel identity, no generation) | Cancellation intent + `submitted` flag only | `submitted` marks kernel ownership | carries `Completion*` used by `reap_op_cqe` to publish | none (no generation at all) | none | none (one per outstanding op, unbounded) | none (record in map) | `accept_submitted_prefix` sets `submitted` | **Eliminate as identity.** Its bookkeeping role (per-slot scratch: prepared/kernel-owned/cancel state) moves to a construction-time fixed array keyed by slot |
| `ops` (`unordered_map<__u64,OpRec>`) | **Yes** — the only mapping from kernel-visible id to request | partial | partial | indirect (via OpRec) | none | `ops.find(id)==end` → drop | **no bound** — grows with outstanding ops | `emplace` on every submit (post-claim, post-SQE-prep) | via OpRec.submitted | **Must disappear.** Replaced by RequestSlot (identity) + fixed per-slot scratch (bookkeeping) |
| `comp_to_op` (`unordered_map<void*,__u64>`) | **Yes** — reverse Completion→id map (B3 O(1) cancel) | none | none | none | none | none | **no bound** | `emplace` on every submit | none | **Must disappear.** `RequestArena::resolve_completion` (bounded O(capacity) scan, allocation-free, generation-validated via the slot binding) replaces it |
| `cancel_to_op` (`unordered_map<__u64,__u64>`) | **Yes** — second identity system for cancel ops (cancel-id → op-id) with no generation | cancel intent | none | none | none | none | **no bound** | `emplace` per cancel SQE | none | **Must disappear.** Cancel bookkeeping moves to per-slot scratch; the cancel SQE carries its own opaque cookie and targets the op by the op's cookie value (§7.3) |
| `pending_sqes` (`deque<PendingSqe>`) | partial — ordered list of not-yet-accepted SQE ids (ops + cancels) | submit-order authority | **Yes** — "userspace-owned vs kernel-owned" split lives here | none | none | none | **no bound** | `push_back` per submit/cancel | `accept_submitted_prefix` pops the accepted prefix | **Split, not abolished.** Its *identity* and *unbounded-growth* roles disappear; its *physical-SQ submission-order* role — which `accept_submitted_prefix` used to consume the exact accepted prefix — is retained as a bounded `SqSubmissionLedger` (§5.6). Logical dispatch order moves to a construction-time bounded dispatch ring of `SlotHandle`s (mirroring ThreadPool's `BoundedDispatchQueue`); per-SQE bookkeeping (`sqe_prepared`, `kernel_owned`) moves to per-slot scratch. The ledger is necessary because neutral NOPs/cancel SQEs make the physical SQ order non-derivable from the dispatch ring. |
| `next_id` (monotonic counter) | **Yes** — the kernel-visible identity source | none | none | none | none | none | none | none | none | **Eliminated as an identity.** user_data becomes a non-authoritative opaque cookie that routes a CQE back to a scratch slot carrying the authoritative full `SlotHandle` (§7.2) |
| `OpRec.submitted` | no | no | **Yes** — kernel-ownership marker | no | no | no | no | no | set by `accept_submitted_prefix` | moves to per-slot scratch `kernel_owned` |
| `OpRec.cancel_requested` / `cancel_op_id` | no | partial (cancel intent) | no | no | no | no | no | no | no | `cancel_requested` → arena `cancel_intent_` + scratch `cancel_pending`; `cancel_op_id` → scratch `cancel_token` |
| `fatal_error` (optional<IoError>) | no | **Yes** — backend-global terminal state | no | no | no | no | no | no | no | Redesigned: per-slot permanent dispatch failure via `record_terminal`; a backend "submit poison" flag remains only as the *new-submission* rejection policy (see §10) |
| `deferred_ready` (count) | no | no | no | deferred publication | no | no | no | no | no | Eliminated — pressure-flush failures become backend_ready slots reaped by poll |
| `consecutive_zero_submits` | no | partial (fatal trigger) | no | no | no | no | no | no | no | retained as dispatch-pass policy (see §10) |

**Audit conclusion:** every one of the four containers must disappear; none may survive even as
bounded bookkeeping, because each of them is either an identity authority (`ops`,
`comp_to_op`, `cancel_to_op`, `next_id`) or a second submission-order/lifecycle authority
(`pending_sqes`). Their *bookkeeping* roles (kernel-owned marker, cancel intent, submit order)
are re-homed into (a) the RequestSlot lifecycle itself and (b) a construction-time bounded
per-slot scratch array that is **keyed by slot index** — exactly the shape the reference
`ThreadPoolBackend` uses for `prepared_ops_` (a fixed `std::vector` indexed by `h.slot.value`,
read only after a generation-validated `mark_running`). The scratch never identifies a request;
the `SlotHandle` (slot + full generation, re-validated by the arena) is the identity.

### 3.3 The three structural violations this audit confirms

1. **SQE acquisition precedes acceptance (ADR Decision 5 violation).** `submit_*` acquires and
   fills the SQE *before* `register_op` (the current acceptance point), and on pressure even
   performs an `io_uring_submit` (kernel execution!) inside submit. A pre-commit rollback after
   SQE acquisition is therefore impossible to make sound — the ADR requires the opposite:
   reserve bounded userspace dispatch bookkeeping before commit, acquire/fill SQEs only during
   post-commit dispatch (ADR `Decision 5` "Reserve"; `Decision 4` `enqueued -> running`).
2. **Post-accept allocation dependency (I9 / Decision 14 violation).** `ops.emplace`,
   `comp_to_op.emplace`, `pending_sqes.push_back` run after the SQE is prepared; the
   `TODO(P0-02)` comment in `submit_read` admits the remaining window. OOM there would strand a
   prepared SQE with no request record.
3. **Direct CQE → Completion publication (I11 / Decision 9 violation).** `reap_op_cqe` publishes
   the Completion directly with no `backend-ready` staging and no reap authority — exactly the
   "workers and CQE handlers stop at backend-ready" rule the ADR imposes. In the current code the
   backend *is* its own publish authority, so this is consistent with the legacy Completion
   authority ADR — but it cannot be the target model.

### 3.4 Current threading model (important for the target design)

- `AsyncIoContext` serializes **all** backend entry points (`submit_*`, `poll`, `cancel`,
  `outstanding`) under `access_mtx_` (src/async/async_io_context.cpp:110-253). The Uring backend
  therefore has a real single-driver execution model today: no worker threads, no concurrent ring
  access, documented in the source ("single-driver-thread ... No mutex is needed").
- `wait_one()` uses the legacy serialized contract (no `BackendWaitSource`), so a blocking
  `io_uring_wait_cqe` runs under `access_mtx_`. ApplicationRuntime rejects such backends at
  build time — Uring is a direct-driver backend.
- `close_admission()` does not exist on `AsyncBackend`/`AsyncIoContext`; the C2e conformance
  tests call it directly on the concrete backend. Phase D's Uring `close_admission()` therefore
  races a concurrent `submit_*` through the context — the reference `ThreadPoolBackend` solves
  this with a backend admission transaction mutex (`admission_mtx_`, threadpool_backend.cpp:929-949
  + :271-383). Phase D must mirror that (see §11).
- The C2d/C2e deterministic-window tests pause the driver thread inside a guarded gate and let
  the test thread call `cancel()`/`close_admission()` directly on the backend — bypassing
  `access_mtx_`. Phase D's Uring test seams therefore need the same backend-level
  admission/dispatch mutexes so the paused-window tests can interleave safely.

---

## 4. Target architecture

### 4.1 Target structure

```text
submit_read/write/sync_data/sync_all
  |  (backend admission transaction under admission_mtx_, mirroring ThreadPool)
  v
Stage 1  reserve:          arena_.reserve()  -> SlotHandle | would_block | invalid_state(closed)
Stage 1.5 descriptor validation:             -> invalid_argument (DIV-14 closure, mirroring
                                                  ThreadPool validate_*; no fcntl preflight)
Stage 2  prepare:          arena_.prepare(h, kind, borrow)   (borrow inactive until commit)
         scratch[h.slot]  := { }  (defensive re-init)
Stage 2.5 binding:         arena_.install_publication_binding(h, &c, len, publish_thunk)
Stage 3a binding CAS:      begin_binding(c)  (idle -> binding; loser rolls back slot)
Stage 3b commit:           arena_.commit(h)  (pending + enqueue pin + accepted++ + borrow active)
Stage 3c publish:          install_binding(c, &arena_, h); commit_binding(c)
                           <- release-store binding -> outstanding = ACCEPTANCE LP (Step 5)
Stage 4  enqueue:          arena_.enqueue(h)  (allocation-free, noexcept; Scheme-B terminal_noop)
         if enqueued:      dispatch_ring.push_back(h)  (bounded, allocation-free)
         (SQE acquisition happens HERE? NO — only enqueue; SQE fill is dispatch, §5)
poll() / wait_one()
  -> reap first:           arena_.reap(sink)  (publishes Completion-ready through slot binding)
  -> dispatch pass (§5):   fill SQEs for enqueued ring entries; submit; prefix -> mark_running
  -> reap again:           arena_.reap(sink)
  -> wait_one parks per §11 (kernel wait + control-plane wake)
CQE (reaped in poll/wait_one, §6)
  -> decode cookie -> bounded scratch lookup -> full SlotHandle -> arena full-generation validate
  -> neutral cookie (NOP): drop as control bookkeeping; NEVER addresses a request
  -> cancel CQE:    informational only; clear scratch cancel state; NEVER record terminal
  -> op CQE:        arena state check (running) ; drop if terminal_stored or generation mismatch
                    record_terminal(h, result)   <- the arena decides the winner (exactly once)
  -> arena_.reap(sink) publishes Completion-ready (reap is the ONLY publication path)
cancel(c)
  -> arena_.resolve_completion(&c) -> SlotHandle   (replaces comp_to_op)
  -> arena_.cancel(h):
       pending/enqueued -> terminal_won (canceled stored; dispatch never submits its SQE —
                           prepared SQEs of backend_ready slots are NOP-neutralized, §6.2)
       running          -> intent_recorded; submit IORING_OP_ASYNC_CANCEL with cancel token
       already_terminal / not_found / not_supported per arena
  -> tally canceled_ops only on terminal_won; confirmed -ECANCELED CQE winner recorded via
     record_canceled (arena round-4 refinement)
close_admission()
  -> under admission_mtx_: arena_.close_admission() + control wake (§11)
destruction
  -> quiescence snapshot check (arena_.quiescence_snapshot + ring/scratch emptiness) ->
     fail-fast in Debug AND Release if not quiescent (§11)
```

### 4.2 State mapping (arena states only; no new enum)

```text
free -> reserved -> prepared -> pending -> enqueued -> running -> backend_ready
                                                            -> completion_ready -> free
```

| Arena state | Uring meaning | SQE ownership (per-slot scratch) |
|---|---|---|
| `reserved` / `prepared` | pre-commit; invisible to progress | none |
| `pending` | committed (LP done), pin set, not yet enqueued | none |
| `enqueued` | accepted, dispatchable — on the dispatch ring | `sqe_prepared` may be true (userspace-owned SQE written, not submitted) |
| `running` | **kernel-owned** (SQE accepted by `io_uring_submit` prefix) | `kernel_owned == true`; CQE expected |
| `backend_ready` | terminal stored (ordinary/canceled/error/dispatch-failure) | scratch cleared; ready-ring entry |
| `completion_ready` | reaped; Completion ready | — |

**Deliberate decision (task §6):** "prepared but not submitted" is NOT a new arena state. It is
backend-private scratch (`sqe_prepared && !kernel_owned`). Justification: no other component
observes it; the enqueue/cancel/reap semantics are identical whether or not an SQE is prepared
(cancel on `enqueued` wins the terminal either way; reap requires `backend_ready`; the
NOP-neutralization obligation is derived from the scratch, not from any observable semantic
state). Adding an arena state would create a second observable lifecycle machine — the opposite
of the target.

### 4.3 Ownership authority table (target)

| Authority | Owner | Phase D proof |
|---|---|---|
| Request identity (slot+generation+context) | `RequestArena` / `RequestSlot` | all arena calls re-validate `SlotHandle` |
| Lifecycle transitions | `RequestArena` (single leaf mutex) | submit/enqueue/cancel/CQE all route through arena methods |
| Kernel-visible cookie | non-authoritative opaque cookie routing CQEs to a scratch slot that carries the full `SlotHandle` | §7 |
| Execution ownership | io_uring ring (SQE/CQE) — the ONLY thing io_uring owns | `mark_running` = kernel acceptance; CQE → `record_terminal` |
| Terminal winner | `RequestArena::record_terminal` / `cancel` (first wins, losers no-op) | CQE handler never publishes; only stores via the arena |
| Completion-ready publication | `RequestArena::reap` (sole authority, through the slot binding) | poll/wait_one call `arena_.reap(sink)` only |
| SQE dispatch bookkeeping | backend fixed per-slot scratch (keyed by slot, generation-gated) | scratch read only after arena-validated transitions |
| Cancel intent/terminal | arena `cancel_intent_` / `cancel()` | scratch only mirrors intent for cancel-SQE bookkeeping |
| Submit-order / dispatch queue (logical) | backend bounded dispatch ring (capacity == request_capacity) | §5 |
| Physical-SQ submission order | backend bounded `SqSubmissionLedger` (capacity == `uring_queue_depth`); non-authoritative — only the arena decides identity/terminal | §5.6 |
| Capacity / accounting | arena (`slot_in_use`, `accepted_outstanding`, high-water, rejections) | §12 |

---

## 5. Dispatch / SQE ownership split (the core Phase D restructure)

### 5.1 What the ADR freezes

> Reserve bounded userspace dispatch-queue capacity before commit, but actual SQE
> acquisition/preparation belongs to dispatch. (ADR Decision 5 "Reserve")

> Transient SQE pressure and a positive partial submit are not terminal failures: the unsubmitted
> suffix stays bound and enqueued/submission-pending for allocation-free retry. A post-commit
> failure becomes terminal `backend_error` only after the backend proves that no worker, userspace
> SQE, kernel request, or future CQE can still refer to or execute that request. (ADR Decision 5
> "Dispatch")

### 5.2 Dispatch pass (runs in `poll()` / `wait_one()` only — never in submit)

```text
dispatch_pass():
  # Phase 1 — fill SQEs for dispatchable slots, in ring order (submission order).
  for each SlotHandle h in dispatch_ring (head -> tail):
    if scratch[h].sqe_prepared: continue              # already written (previous partial submit)
    s = arena current state of h:
      enqueued  -> proceed
      backend_ready / completion_ready / free
                 -> continue                          # cancel/reap already removed ownership of h;
                                                      # ring compaction (§5.4) drops it; NEVER fail-fast here
      other     -> invariant violation (fail-fast, both modes)
    sqe = io_uring_get_sqe(&ring)
    if sqe == nullptr:                                # SQ ring pressure
      flush: submit the prepared batch (see Phase 2), then retry get_sqe once
      if still nullptr: stop the pass (enqueued slots stay enqueued; retried next driver call)
    prep sqe from arena slot data (op_kind, borrow fd/addr/len/offset)  [no per-op alloc]
    op_token = next_op_cookie()                       # §7.3 (monotonic, never reused)
    io_uring_sqe_set_data64(sqe, op_token)
    scratch[h] = { full_handle = h, op_token, sqe_prepared=true, kernel_owned=false }
    sq_ledger.append({ kind=operation, handle=h, cookie=op_token })   # §5.6 — physical-SQ order
  # Phase 2 — submit the prepared batch.
  if no new prepared SQEs: return
  n = io_uring_submit()
  if n < 0:
    transient (-EINTR/-EAGAIN/-EBUSY): keep suffix prepared; retry next pass (like today)
    permanent: for each prepared-not-submitted SQE:  # provably userspace-owned: not in ring
                 record_terminal(h, backend_error)   # the arena's terminal winner
               set submit_poisoned (new submissions reject synchronously with the error)
               return
  if n > sq_ledger.prepared_count: invariant violation (impossible accepted count) -> poison + fail-fast
  if n == 0 and previously zero: same policy as today (consecutive zero -> permanent)
  # Phase 3 — consume the EXACT physical-SQ accepted prefix from sq_ledger (§5.6), NOT from
  # dispatch_ring. The accepted SQEs may be a mix of operation / cancellation / neutral entries
  # (a neutralized NOP or a prepared cancel SQE interleaves with op SQEs in physical SQ order),
  # so "the first n dispatch_ring handles" is NOT the accepted set. Only the ledger, which is
  # 1:1 with the SQ in exact submission order, names which n SQEs the kernel took.
  sq_ledger.consume_accepted_prefix(n):
      operation  -> if arena_.mark_running(handle):                       # current-gen slot
                        scratch[handle].kernel_owned = true
                    else:
                        # a terminal winner (cancel, or §9 permanent-failure) already won for this
                        # exact generation. The op SQE already went to the kernel, so a CQE carrying
                        # cookie WILL arrive — dropped harmlessly by §6.1 (graceful backoff, NOT
                        # fail-fast; genuine invariant violations fail-fast INSIDE mark_running).
      cancel     -> mark the cancel SQE kernel-owned in scratch bookkeeping (it is an auxiliary
                    kernel op; it MUST NOT cause the target request to re-enter running, and its
                    own CQE is informational only — §6.1)
      neutral    -> no request transition (the request was already detached at neutralization)
  # suffix (prepared but not accepted) stays in sq_ledger + scratch sqe_prepared=true —
  # allocation-free retry next pass. Its identity is preserved (scratch full_handle + op_token).
```

**Why a separate SQ ledger (correction of the earlier draft):** once the SQ stream can contain
neutral NOPs (from §5.4 neutralization) and cancel SQEs (from the running-cancel dispatch
sub-path), the physical SQ order is **no longer derivable from `dispatch_ring`**: a neutralized
NOP remains a pending SQE in the kernel's queue even though its request has been removed from
`dispatch_ring` and its scratch cleared. Interpreting `io_uring_submit()`'s return value `n` as
"the first n `dispatch_ring` handles" would then `mark_running` a request whose SQE was not
actually accepted (the NOP was) — re-introducing exactly the dual authority
(arena-state ≠ actual kernel ownership) Phase D exists to eliminate. The bounded
`SqSubmissionLedger` (§5.6) is the 1:1 physical-SQ-order bookkeeping that makes the accepted
prefix unambiguous. It is **not** a request-identity authority (identity stays in RequestArena);
it is the kernel-submission-order ownership record the legacy `pending_sqes` carried — kept
bounded and de-coupled from identity, not abolished.

**Dispatch-pass state handling (correction of the earlier draft):** the earlier draft failed-fast
the dispatch pass on any non-`enqueued` state except `backend_ready`, which under the draft's own
`reap-first → dispatch-pass` ordering turned a *normal* enqueued-cancel into a fail-fast: cancel
wins → `backend_ready` → reap → `completion_ready` → dispatch ring still contains `h` → dispatch
pass sees a non-enqueued state → fail-fast. The reference `ThreadPoolBackend` shows the correct
shape: its `poll()` is `arena_.reap()` only (no dispatch pass), dispatch lives in a separate
consumer, `mark_running` treats a `backend_ready`/cancel-won slot as a **graceful backoff
(`return false; continue`)**, and fail-fast is reserved for true invariant violations (stale
identity, double-dispatch). The Phase D dispatch pass adopts the same rule: a non-`enqueued`
current-generation handle encountered in the ring is skipped (the §5.4 ring compaction removes
it), never fail-fasted; only the `other` (e.g. `reserved`/`prepared`/`pending`) branch is a real
invariant violation.

### 5.3 Partial-submit ownership proof

| Outcome | Ownership | Request state | What happens next |
|---|---|---|---|
| `io_uring_submit` returns `k` (0 < k < prepared) | prefix [0..k) kernel-owned; suffix [k..) userspace-owned, prepared | prefix → `running`; suffix stays `enqueued` | suffix SQEs remain in the SQ ring, `sqe_prepared=true`, retried by the next dispatch pass with **no re-prep and no new allocation**; identity preserved in scratch (`full_handle` + `op_token`) |
| `io_uring_submit` returns `k == prepared` | all kernel-owned | all → `running` | CQEs |
| transient error / one zero-progress | none submitted; suffix userspace-owned | stay `enqueued` | retry next pass (identical to today's `transient_submit_error` policy) |
| permanent error | none submitted (or only a provably-userspace-owned suffix) | suffix → `backend_ready(backend_error)` via `record_terminal`; kernel-owned prefix (if any) keeps its CQEs | new submissions reject with the stored error (`submit_poisoned`) — see §9 |
| impossible `k > prepared` | — | invariant violation | poison + fail-fast (both modes) |

Explicitly **forbidden** (ADR Decision 5, task §5): terminalizing the suffix, releasing the
suffix, losing suffix identity, allocating retry records after acceptance, duplicating SQEs,
treating positive partial submit as fatal, or re-submitting the already-kernel-owned prefix.

**Prepared-suffix cancellation is a production state, not only a test seam.** Because partial
submit leaves a suffix in `enqueued` + `sqe_prepared` (§5.3 row 1), a cancel landing on such a
suffix slot is a NORMAL production interleaving — the earlier draft's claim that the
prepared-but-not-submitted cancel window is "unreachable under the serialized production model,
only for test seams" is withdrawn. The §5.4 neutral-cookie protocol therefore runs on the
production path, and the §6.0 organizing invariant must hold for it.

### 5.4 Enqueued-cancel protocol — DISARM EXECUTION FIRST, terminal SECOND (§6.0 invariant)

A cancel that wins `pending -> backend_ready(canceled)` or `enqueued -> backend_ready(canceled)`
must prove the three clauses of §6.0 **before** the terminal is published. The earlier draft of
this section called `arena.cancel(h)` FIRST and then disarmed execution — that order is **wrong**
and is corrected here. `arena.cancel(h)` on a `pending`/`enqueued` slot immediately performs the
terminal transition (`state = backend_ready`, ready-ring push) under the arena leaf lock; once
that has happened, `arena.reap()` (which takes only the arena leaf lock, NOT the dispatch lock)
can publish `completion_ready` and the caller can `reset()`/release the slot — all before the
disarm runs. The dispatch lock does not serialize `arena.reap()`, so "the terminal is not yet
observable to reap" was never actually guaranteed by the old order.

The correct order mirrors `ThreadPoolBackend::cancel()` (`threadpool_backend.cpp:894-899`):
`remove_exact` **before** `arena.cancel`, both under the dispatch lock. For Uring this is:

```text
lock(dispatch domain)
  h = arena.resolve_completion(&c)
  owned = classify_execution_ownership(h):             # userspace-owned | kernel-owned | none
  if owned == userspace-owned:                         # pending/enqueued, SQE maybe prepared
      dispatch_ring.remove_exact(h)                    # clause 1: not present as executable work
      if scratch[h].sqe_prepared && !scratch[h].kernel_owned:
          # clause 2 + 3: neutralize the prepared SQE and detach the request cookie
          sq_ledger.reclassify_entry(scratch[h].cookie, kind=neutral)   # §5.6 — physical-SQ entry
          io_uring_prep_nop(sqe_at(scratch[h]))                         #   stays (kernel may still
          io_uring_sqe_set_data64(sqe_at(scratch[h]), NEUTRAL_COOKIE)   #   submit it); only its
          scratch[h].op_token = 0                                       #   kind flips to neutral
      scratch[h].clear()                               # cancel_token bookkeeping also cleared
  # NOW the three §6.0 clauses are proven for userspace-owned requests:
  #   (1) off dispatch_ring, (2) prepared SQE neutralized, (3) no future CQE carries op_cookie.
  disp = arena.cancel(h)                               # FINALLY the terminal transition
  # userspace-owned  -> terminal_won (canceled stored); clauses already proven, so a concurrent
  #                     reap is now safe (the slot has no dispatch linkage / prepared SQE / cookie).
  # kernel-owned     -> intent_recorded only (the running path below); no terminal stored here.
unlock(dispatch domain)
if disp == terminal_won: tally_canceled(); signal_ready_progress()
```

The ordering principle — **execution-ownership cleanup FIRST, terminal publication SECOND** — is
a frozen D1 invariant. It is what makes the §6.0 clauses provable at the moment the terminal
becomes observable, rather than merely "probably true a moment later". The dispatch pass's
graceful-backoff state handling (§5.2) remains a defense-in-depth: even if a race left `h`
briefly visible to a dispatch pass, it is skipped, not fail-fasted.

Note the ledger interaction: when a prepared-suffix SQE is neutralized, its `SqSubmissionLedger`
entry is **reclassified to `neutral`**, not removed (§5.6) — the physical SQE still occupies a
slot in the kernel's SQ and may yet be submitted, so the ledger must keep recording it in exact
SQ order; only its *kind* changes, so a later `consume_accepted_prefix` treats it as a no-op.
This is precisely why the ledger cannot be reconstructed from `dispatch_ring` (the neutralized
request is gone from the ring but its SQE is still in the kernel's queue).

**Running cancel** (`owned == kernel-owned`): the op is kernel-owned, so the §6.0 invariant's
clause-3 exception applies — the op's real CQE (possibly `-ECANCELED`) competes for the terminal.
`remove_exact` is a no-op (already dequeued at `mark_running`) and the prepared SQE is NOT
neutralized (it is already in the kernel). `arena.cancel(h)` records `intent_recorded` +
`cancel_intent_=true` + `scratch[h].cancel_pending=true`. The cancel SQE itself is produced by
the **unified dispatch machine**, NOT by an ad-hoc flush in the cancel path (an ad-hoc
`io_uring_submit()` inside cancel would push prepared op SQEs into the kernel without the
coordinated `mark_running`, re-introducing the dual authority this design eliminates). A
subsequent dispatch pass reserves one SQE (subject to the same ring-pressure backoff as op SQEs)
for `io_uring_prep_cancel64(sqe, op_cookie, 0)`, appends a `cancel` entry to `sq_ledger`, and
submits it in the same batch as ordinary ops. If the cancel SQE cannot be prepared this pass (SQ
pressure), it is retried next pass — the intent is already durably recorded in the slot. The
cancel CQE is informational only (§6.1). **(Precise SQE-pressure / retry / ownership detail for
the running-cancel dispatch sub-path is a D1 frozen-design item; the principle — no ad-hoc
submit in cancel, disarm-before-terminal — is fixed here.)**

### 5.5 Why ring pressure is not admission `would_block`

`request_capacity` is the arena bound; `uring_queue_depth` is the SQ/CQ ring depth. They are
independent resources (ADR Decision 13 table; AGENTS.md §12). Ring pressure at dispatch time means
"the kernel's submission queue is full" — the request is ALREADY accepted (committed) and simply
waits. `would_block` is reserved for arena-full at `reserve()` — Completion idle, no borrow, no
execution. The two are distinguishable in stats: `would_block` → `queue_full_retries` (context
`tally_submit`); SQ pressure → backend `queue_full_retries` bump on the dispatch flush (the
existing Uring `queue_full_retries` semantics are retained for the dispatch flush, matching
today's `get_sqe_with_pressure` bump).

### 5.6 The bounded SQ submission-order ledger (`SqSubmissionLedger`)

**Problem this solves.** Once the SQ stream can contain neutral NOPs (§5.4) and cancel SQEs
(§5.4 running cancel), the physical SQ order is no longer 1:1 with `dispatch_ring` entries, and
`io_uring_submit()`'s returned count `n` can no longer be interpreted as "the first n
`dispatch_ring` handles" — a neutralized NOP is still a pending SQE in the kernel's queue after
its request has left the ring. The accepted-prefix ownership transfer (`mark_running`) must
therefore consume the **exact** physical-SQ prefix, which only a structure that is 1:1 with the
SQ in submission order can name. The legacy `pending_sqes` deque carried exactly this
"SQ execution-order ledger" responsibility (`accept_submitted_prefix(count)` consumed the exact
prefix); what was wrong with it was being **unbounded, an identity authority, and coupled to
`ops`/`cancel_to_op`** — not the submission-order concept itself. Phase D replaces it with a
bounded, non-authoritative ledger.

**Definition.** A construction-time bounded ring (capacity == `uring_queue_depth`) of entries,
in exact physical-SQ submission order:

```text
struct SqLedgerEntry {
    enum class Kind { operation, cancellation, neutral };
    Kind        kind;
    SlotHandle  handle;   // meaningful for operation (the op's slot) and cancellation (the
                          //   target's slot); neutral entries carry the slot they were
                          //   neutralized FROM for diagnostics only
    uint64_t    cookie;   // the op_cookie / cancel_cookie / NEUTRAL_COOKIE this SQE carries
};
```

**Lifecycle.**
- The dispatch pass appends an `operation` entry each time it prepares an op SQE, and a
  `cancellation` entry each time it prepares a cancel SQE (§5.4), in the exact order the SQEs
  are written into the SQ ring.
- §5.4 neutralization **reclassifies** the matching entry to `neutral` (by cookie) — it does NOT
  remove it, because the physical SQE still occupies a kernel SQ slot and may be submitted; only
  its kind flips.
- On `io_uring_submit()` returning `n`, `consume_accepted_prefix(n)` pops the first `n` entries
  and applies the per-kind transition (operation → `mark_running`; cancellation → cancel-SQE
  kernel-owned bookkeeping; neutral → no-op). The remaining suffix stays for allocation-free
  retry.
- Capacity is bounded by `uring_queue_depth`: the number of simultaneously-userspace-owned SQEs
  can never exceed the SQ ring depth (each prepared SQE occupies one SQ slot until accepted).

**Authority.** `SqSubmissionLedger` is **NOT a request-identity authority**. It records only
"which physical SQE corresponds to which request cookie, in what order". Request identity,
lifecycle, generation, and terminal authority remain solely in `RequestArena`. The ledger is to
io_uring's SQ what the dispatch ring is to logical dispatch order: a bounded, non-authoritative
execution-side bookkeeping that the arena validates via full `SlotHandle` on every ownership
transfer. This is the explicit retention of the legacy `pending_sqes` *correct* responsibility
(submission-order bookkeeping) after removing its *incorrect* ones (unbounded growth, identity,
coupling to `ops`/`cancel_to_op`).

**SQPOLL is forbidden.** The entire accepted-prefix ownership proof depends on
`io_uring_submit()`'s return value being the **exact** count of SQEs the kernel accepted, in
order. That guarantee holds for the default (non-SQPOLL) mode; under `IORING_SETUP_SQPOLL` the
return value may exceed the actually-accepted count (the kernel polls the SQ asynchronously).
Phase D therefore does NOT use `IORING_SETUP_SQPOLL`; a future SQPOLL design would require a
separate, proven ownership protocol and is explicitly out of scope.

---

## 6. CQE identity / stale-token / reap

### 6.0 The organizing invariant (governs §5, §6, §7 together)

```text
A request may enter backend_ready(canceled) ONLY AFTER the backend has proved ALL of:
  1. it is not present as executable work in the dispatch ring; and
  2. no userspace-owned prepared SQE can execute it; and
  3. no future CQE carrying this request's op_token can be generated
     unless the request is already running/kernel-owned.
```

This single invariant subsumes the cancel/SQE/CQE ownership question. §5–§7 are its
decomposition. A cookie + neutral-token design (§7) is what makes (3) provable for an
enqueued-cancel: neutralization detaches the cookie from the request before the NOP can reach the
kernel, so the resulting NOP CQE carries a neutral cookie and can never address a slot.

### 6.1 CQE handler (poll/wait_one, under the driver serialization domain)

```text
decode cqe.user_data -> cookie
lookup: bounded O(capacity) scan of scratch[] for an entry whose
        op_token or cancel_token == cookie
if no match:                                  drop  # neutral cookie, or stale cookie of a released request
h = scratch[i].full_handle                    # exact full slot + full 64-bit generation
if cookie is a cancel_cookie (kind bit set):
    # informational cancel CQE (ADR Decision 11 kernel-owned; X3 exactly-once structural rule)
    if arena.validate(h) && scratch[i].cancel_pending && scratch[i].cancel_token == cookie:
        scratch[i].cancel_pending = false; scratch[i].cancel_token = 0
    else: drop (stale/late cancel CQE)
    NEVER records a terminal, NEVER publishes
else:
    # op CQE (or neutral-NOP CQE — the latter never matches any scratch entry and is dropped above)
    if !arena.validate(h):                    drop  # full-generation mismatch (released/reused slot)
    if arena state_of(h) != running:          drop  # never-submitted/neutralized/terminal
    if arena terminal already stored for h:   drop  # duplicate/late CQE; winner decided
    result = cqe->res -> TerminalResult
        res >= 0 (size op) -> ok_bytes(res) | (void op) -> ok_void
        res <  0           -> err(from_errno_value(-res))   # -ECANCELED -> canceled (confirmed
                                                             # cancel winner; tally canceled_ops)
    arena_.record_terminal(h, result)         # first winner wins; losers no-op
    scratch[i].clear()
arena_.reap(sink)                             # SOLE Completion-ready publication path
```

Identity authority: a cookie routes the CQE to a scratch slot, which yields the **authoritative
full `SlotHandle`**; the arena then re-validates the **full** generation. The cookie is never
the identity. The `state_of(h) != running` check is the terminal-eligibility authority: a CQE
can record a terminal only if the arena believes this exact generation of this slot is
kernel-owned. A neutral-NOP CQE carries the reserved neutral cookie, matches no scratch entry,
and is dropped at the top of the handler — it can never reach any request.

### 6.2 Enqueued-cancel no-execute guarantee (neutral cookie, NOT same request token)

A cancel that wins `pending -> backend_ready(canceled)` or `enqueued -> backend_ready(canceled)`
(arena `cancel()` Scheme B) means the operation SQE MUST NOT be submitted, and **no future CQE
carrying this request's op_token may be generated** (organizing invariant, clause 3). Three
windows:

1. **No SQE prepared** (enqueued, scratch empty): the dispatch pass skips the slot (state check
   at Phase 1) — the SQE is never written. The cookie was never handed to the kernel. Invariant
   satisfied trivially.
2. **SQE prepared, not yet submitted** (enqueued, `sqe_prepared`, between two dispatch passes —
   a NORMAL production state reachable via partial submit, §5.3, NOT only a test seam): the
   enqueued-cancel path (§5.4) **rewrites the prepared SQE to a NOP and rewrites its user_data
   to a NEUTRAL cookie** (`io_uring_prep_nop` + `NEUTRAL_TOKEN`), detaching the request's
   `op_token` from the SQE before it can reach the kernel, then clears the slot's scratch and
   removes the handle from the dispatch ring. The kernel therefore never sees `op_token`, so no
   future CQE can carry it; the subsequent NOP CQE carries the neutral cookie and is dropped at
   the top of §6.1 as control bookkeeping. **The request's `op_token` is unreachable by any CQE
   after neutralization, so slot release/reuse is safe even if the NOP CQE is still in flight.**
3. **Already submitted** (kernel-owned): the cancel could not have won `enqueued` (the slot is
   `running`); a `running` cancel records intent only and the op's real CQE (possibly
   `-ECANCELED`) competes for the terminal winner normally. The op's `op_token` is legitimately
   in flight, and clause 3's "unless already running" exception applies.

**Why neutral cookie, not the original request token (correction of the earlier draft):** the
earlier draft neutralized the SQE to a NOP but kept `user_data = op_token`. Because io_uring
posts exactly one CQE per SQE the kernel processes (including NOPs), that left a future NOP CQE
carrying `op_token` that could arrive **after** the canceled request had been reaped, released,
and the slot reused by a new occupant — i.e. the organizing invariant's clause 3 was violated,
and the only barrier left was the truncated generation fragment (then defended as
"defense-in-depth"). The neutral-cookie rewrite removes the violation at its source: the kernel
never receives `op_token`, so no post-release CQE can carry it. (A request execution-reference
pin that defers release until the NOP CQE arrives was considered and rejected: it complicates
the reap→release lifecycle materially; the neutral cookie is strictly simpler.)

### 6.3 Stale-CQE scenarios (task §7) — one classification per source

| Source | Mechanism | Disposition |
|---|---|---|
| duplicate op CQE (defect/injection) | `arena.validate(h)` ok but terminal already stored, or `state != running` | dropped (terminal-stored / state check) |
| late cancel informational CQE | `validate(h)` ok but scratch cancel state already cleared, or `validate(h)` fails | dropped |
| neutralized-NOP CQE | carries reserved neutral cookie; matches no scratch entry | dropped at top of §6.1 (control bookkeeping only) |
| stale injected event with a released request's cookie | the released request's scratch entry was cleared, so the cookie matches nothing | dropped (no scratch match) |
| stale injected event carrying a *reused* cookie | cookies are never reused (monotonic, fail-fast before wrap) — impossible by construction | n/a |
| generation mismatch due to bug | cookie resolves to a scratch slot whose `full_handle` generation fails `arena.validate` | dropped by full-generation validation |

**Honest answer to "can the kernel legally deliver an op CQE after slot release?":** for an
op-CQE cookie, no — under the target model the only way an `op_token` reaches the kernel is
through a real `running` submission (clause 3 exception); a prepared-but-unsubmitted op SQE is
either submitted as the real op (slot `running`, CQE expected, release forbidden until reaped)
or rewritten to a neutral NOP before submission. The neutral-NOP CQE is explicitly designed to
arrive after release and is dropped as control bookkeeping. The authoritative barrier is the
arena's **full** generation `validate` plus the cookie-never-reused property; no truncated
generation fragment is involved, and there is no residual 2^N reuse window to document.

### 6.4 Reap order

`arena_.reap` delivers in terminal-winner order via the ready-ring (Decision 9, review finding
#3) — the same authority Fake/Sync/ThreadPool use. Uring's previous "CQE order" is no longer an
ordering authority; CQEs only *store* terminals.

---

## 7. user_data encoding (task §4.1 — analysis A–F)

### 7.1 Candidate analysis

| Candidate | Stale-CQE safety | Generation representation | Capacity bound | CQE decode cost | Cancel target | 64-bit fit |
|---|---|---|---|---|---|---|
| A. raw `SlotIndex` | **none** — a stale CQE acts on the new occupant | — | bound by arena | trivial | needs separate lookup | fits |
| B. `SlotHandle` (slot + full 64-bit generation) | **absolute** (I6 + the `request_key.hpp`/`free_slot_locked_` no-wrap contract) | full 64-bit, no wrap | bound | trivial | direct | **impossible** — 32+64 bits > 64 |
| C. packed RequestKey fragment (slot + low generation bits) | defense-in-depth; residual ABA window 2^fragment releases | **wraps within the fragment** — contradicts the no-wrap generation contract | bound | shift/mask | direct | fits |
| D. `RequestSlot*` pointer | **none** — same ABA as `Completion*`; no generation | — | bound | trivial | direct | fits (64-bit pointer) |
| E. auxiliary stable operation record (token → fixed OpRec array) | depends on record's generation | — | bound | indirect | indirect | fits |
| **F. opaque cookie → fixed per-slot scratch → full SlotHandle (cookie never reuses a value)** | **absolute** (I6 holds at full 64-bit generation; the cookie is never reissued) | full 64-bit generation, no wrap; cookie monotonic with fail-fast before 2^63 (bit 63 = kind) | bound | bounded O(capacity) scratch lookup, then trivial slot generation check | direct (cookie is the cancel target) | fits |

### 7.2 Selection: F — non-authoritative opaque cookie + fixed per-slot scratch

**Selection: F — opaque kernel cookie with bounded indirection.** Rationale:

- The ADR names this exact shape: "RequestKey -> **encoded/indirect** SQE user_data -> CQE ->
  validate context/slot/generation -> same RequestSlot backend-ready" (ADR "UringAsyncBackend"
  mapping). The word **"indirect"** sanctions a bounded indirection through a fixed scratch
  entry; what the ADR forbids (Decision 2; the Uring target prose "eliminate ... parallel
  identity maps, and identity reconstruction where the slot itself suffices") is a **second
  independently-oversubscribable request store / parallel identity authority** — i.e. an
  unbounded map that reconstructs identity. A construction-time fixed array of `request_capacity`
  entries that merely carries the **authoritative full `SlotHandle`** back to the arena is not a
  second authority: the arena's full slot+generation remains the sole identity, exactly as the
  existing `resolve_completion()` deliberately uses a bounded O(capacity) scan rather than a
  parallel pointer map.
- **C is rejected (correction of the earlier draft):** a low-bits generation fragment (47–57
  bits) **wraps within the fragment** and therefore contradicts the generation contract recorded
  in `include/sluice/async/detail/request_key.hpp` ("64-bit so a stale key can NEVER collide ...
  32-bit wrap re-introduces ABA under heavy reuse") and in
  `RequestArena::free_slot_locked_` (fail-fast at `UINT64_MAX`, no wrap). The earlier draft
  argued the fragment was "defense-in-depth" because the lifecycle forbids releasing a
  kernel-owned slot — but that argument was **inconsistent with this draft's own NOP
  neutralization, which explicitly leaves a future NOP CQE that may arrive after slot release**
  (§6.2). Once such a post-release CQE is possible, the fragment is no longer defense-in-depth;
  it becomes the sole correctness barrier. Cookie F avoids the 64-bit-fitting problem entirely
  without ever truncating the generation.
- A and D are rejected because they carry no generation: the ADR's alternative-A rejection
  ("Completion* as the only identity ... cannot prevent reuse ABA") applies verbatim to a raw
  slot index or pointer.
- Full `RequestKey` (B) does not fit in 64 bits. Context is unnecessary in the token because
  CQEs arrive on **this** ring (cross-context user_data is structurally impossible; the arena's
  `validate_` re-checks context provenance on every handle).

**Cookie authority model (this is the load-bearing distinction):**

```text
RequestArena:
    full SlotHandle(slot + uint64 generation)
    = lifecycle / generation / terminal AUTHORITY (unchanged, sole identity)

fixed Uring scratch[request_capacity]:              # keyed by SlotIndex, NON-authoritative
    SlotHandle full_handle                          # carried back to the arena on every CQE
    uint64    op_token     # opaque monotonic cookie; unique per prepared SQE; NEVER reissued
    uint64    cancel_token # opaque monotonic cookie; unique per submitted cancel SQE; NEVER reissued
    execution bookkeeping  # sqe_prepared / kernel_owned / cancel_pending / ...

SQE.user_data = op_token   (or cancel_token for a cancel SQE)

CQE arrival:
    cookie
    -> bounded O(capacity) scan of scratch[] for scratch[i].op_token/cancel_token == cookie
    -> scratch[i].full_handle   (exact full slot + full 64-bit generation)
    -> RequestArena validates full_handle's generation against the slot (authoritative)
```

The cookie is **never reissued** and **fail-fasts before its 63-bit payload space is
exhausted** (a monotonic counter with bit 63 reserved as the kind marker; see §7.3 for the exact
encoding and the `2^63` bound). The scratch
entry is cleared on release, so a cookie from a released request matches nothing; and even a
hypothetical future collision is the arena's full-generation `validate_` that rejects the stale
`full_handle`, not the cookie alone. The cookie is therefore **non-authoritative**: it is a
routing hint that selects a scratch slot; it never decides identity, generation, or terminal.

**No residual ABA window.** Unlike C, candidate F has no 2^47–2^57 reuse window at all: the
cookie space never wraps (fail-fast before exhaustion), and every CQE is resolved to a full
`SlotHandle` and re-validated against the arena's full generation. I6 holds in perpetuity, not
probabilistically. The Phase D compliance gate therefore records **no generation-fragment bound**
for the token (the only recorded bound is the cookie-space fail-fast, identical in spirit to the
arena's own `request_arena_generation_exhausted_fail_fast`).

### 7.3 Cookie layout and the cancel token

```text
64-bit user_data:
  bit 63        : kind marker (0 = op SQE/CQE, 1 = cancel SQE/CQE)
  bits [0, 63)  : monotonic opaque cookie PAYLOAD (63 bits; never reissued; fail-fast before wrap)
```

Because bit 63 is the kind bit, the **payload is 63 bits, not 64** — the allocation domain is
`2^63`, not `2^64`. The encoding is frozen as:

```text
NEUTRAL_COOKIE = 0                                 # reserved; never an op or cancel cookie
KIND_BIT       = 1ULL << 63
counter        : starts at 1, monotonically incremented per prepared SQE (op or cancel)
if counter > (KIND_BIT - 1):  fail_fast()          # payload exhaustion at 2^63 - 1, BEFORE the
                                                   #   counter can reach the kind bit
op_cookie     = counter                            # bit63 = 0 (op kind)
cancel_cookie = KIND_BIT | counter                 # bit63 = 1 (cancel kind)
```

So an op cookie and a cancel cookie with the same payload are distinguishable by the kind bit,
and the counter can never produce a value that collides with `NEUTRAL_COOKIE` (0) or bleeds into
the kind bit. The payload is never reused; exhaustion (after `2^63 - 1` allocations) fail-fasts,
mirroring the arena's `request_arena_generation_exhausted_fail_fast` but at the 63-bit payload
bound. (At realistic allocation rates this is unreachable, but it is recorded as a hard bound
rather than a wrap, consistent with the no-wrap discipline adopted for generation.)

- A prepared op SQE gets `op_cookie = counter++`; a prepared cancel SQE gets
  `cancel_cookie = KIND_BIT | (counter++)`. Both are stored in the target slot's scratch so a
  CQE resolves to the slot via a bounded scan, then to the authoritative full `SlotHandle`.
- The cancel SQE targets the op by its cookie value: `io_uring_prep_cancel64(sqe, op_cookie, 0)`
  (flags=0 cancels the first request whose user_data equals `op_cookie` — io_uring's default
  `IORING_ASYNC_CANCEL_OP_USERDATA` lookup). No side-band `cancel_to_op` map is needed; the
  scratch's `cancel_cookie` + the kind bit replace it.
- A **neutral cookie** (`NEUTRAL_COOKIE = 0`) is used when neutralizing a prepared-but-
  unsubmitted SQE (§6.2): the SQE is rewritten to a NOP and its user_data is rewritten to
  `NEUTRAL_COOKIE`, detaching it from any request. The resulting NOP CQE is unconditionally
  dropped as control bookkeeping and can never address a request slot.

### 7.4 Cross-backend consistency

Fake/Sync/ThreadPool use the arena's `SlotHandle` as the identity; the Uring cookie is a
ring-local, non-authoritative projection that resolves back to that same handle. All internal
authorities (cancel, reap, waiter, reset) use the full `SlotHandle`; the cookie exists only at
the ring boundary and is validated back to the full generation on every CQE.

---

## 8. Cancel model (ADR Decision 11/12 mapping)

| Request state | arena `cancel(h)` result | Uring action | Terminal |
|---|---|---|---|
| `pending` / `enqueued` | `terminal_won` (canceled stored, ready-ring push) | §5.4 protocol: `remove_exact` + neutralize any prepared SQE to a neutral-cookie NOP + clear scratch — all before the terminal is observable (§6.0 invariant) | `canceled` — the ONLY tally of `canceled_ops` here |
| `running` (kernel-owned) | `intent_recorded` (cancel_intent_ = true) | the unified dispatch machine prepares `IORING_OP_ASYNC_CANCEL` targeting the op cookie (`prep_cancel64(sqe, op_cookie, 0)`) — never an ad-hoc submit in the cancel path (§5.4) | op CQE decides: real result verbatim, or `-ECANCELED` → `record_canceled` (confirmed winner; tally here) |
| `backend_ready` | `already_terminal` | no-op | existing terminal, never overwritten |
| `completion_ready` / released / stale | `not_found` (arena) | no-op | — |
| unsupported op | `not_supported` | no-op | — |

Cancel SQE identity: `user_data = cancel_cookie`; the target is the op cookie value
(`io_uring_prep_cancel64(sqe, op_cookie, 0)`, flags=0 = `IORING_ASYNC_CANCEL_OP_USERDATA` default).
The cancel CQE is **informational only**: it clears scratch cancel state and never records a
terminal (structural X3 exactly-once, preserved from the current design). `cancel_to_op` is gone.

Race proof obligations (each becomes a deterministic test in D3):
original CQE success vs cancel; original CQE error vs cancel; original CQE `-ECANCELED` vs cancel
CQE; cancel CQE `-ENOENT` vs original CQE; cancel CQE first vs original CQE first; duplicate/late
cancel CQE; slot reuse between cancel submission and either CQE. In every interleaving the arena's
terminal-winner rule leaves exactly one stored terminal, and `canceled_ops` is tallied at most
once.

---

## 9. Permanent dispatch failure (replaces `fatal_error` / `fail_unsubmitted` / `deferred_ready`)

| Submit outcome | Ownership proof | Terminal |
|---|---|---|
| `-EINTR` / `-EAGAIN` / `-EBUSY` | nothing accepted | none — suffix stays enqueued, retried next pass (same as today) |
| one zero-progress | nothing accepted | none — retried once (same as today) |
| repeated zero-progress | nothing accepted, kernel stuck | suffix → `backend_error` via `record_terminal`; `submit_poisoned` set |
| permanent negative error | prefix (if any) kernel-owned; suffix provably userspace-owned (never entered the SQ ring) | prefix waits for CQEs; suffix → `backend_error` per slot; `submit_poisoned` set |
| positive partial submit | prefix kernel-owned, suffix userspace-owned | none — suffix retried (never terminalized) |
| impossible accepted count > prepared | — | invariant violation — poison + fail-fast (both modes) |

**Double-execution/double-terminal proof:** a suffix slot is terminalized only when its SQE was
never submitted (prepared-but-not-accepted, or not prepared at all). The kernel cannot have seen
it (`io_uring_submit`'s return value is the exact accepted prefix — liburing semantics, confirmed
against the liburing docs), so no future CQE can arrive for it (no SQE → no CQE); the arena's
terminal winner makes a second terminal impossible. A prefix slot is never terminalized locally —
only its CQE can record a terminal. Therefore no request can be both kernel-executed and
locally-terminalized.

`submit_poisoned` (new-submission policy): after a permanent submit error the ring is no longer
trustworthy for NEW work; `submit_*` rejects synchronously with the stored error (the ADR's
pre-commit `backend_error` rejection cause). This preserves the current backend-global fatal
semantics as an admission policy while the TERMINAL handling becomes per-slot through the arena.
The plan records this as an explicit design decision (a ring that returned a permanent submit
error cannot be trusted for new work; per-slot terminalization covers the accepted suffix).

---

## 10. Bounded memory / allocation audit (task §10)

| Current container | Allocation time | Phase D replacement | Post-accept allocation? |
|---|---|---|---|
| `ops` (unordered_map) | per submit (post-SQE-prep) | eliminated — slot is the record | no |
| `comp_to_op` (unordered_map) | per submit | eliminated — `resolve_completion` O(capacity) scan | no |
| `cancel_to_op` (unordered_map) | per cancel SQE | eliminated — scratch `cancel_token` | no |
| `pending_sqes` (deque) | per submit/cancel | dispatch ring (fixed array, capacity == request_capacity) + `SqSubmissionLedger` (fixed array, capacity == `uring_queue_depth`, §5.6) + per-slot scratch | no |
| — (new) `RequestArena` slots_/free_slots_ | construction | arena | no |
| — (new) dispatch ring storage | construction | fixed array | no |
| — (new) per-slot SQE scratch | construction | fixed array (`full_handle`, `op_token`, `cancel_token`, `sqe_prepared`, `kernel_owned`, `cancel_pending`) | no |
| — (new) publication thunks | static | `publish_size_ready` / `publish_void_ready` (mirror ThreadPool) | no |

**Goal state: after acceptance, zero dependence on new unbounded allocation.** The accepted path
(submit post-commit → enqueue → dispatch → CQE → record_terminal → reap → release) touches only
construction-time storage: the slot, the ring, the scratch, the ready-ring. The I9 no-alloc proof
for Uring mirrors ThreadPool's `reference_backend_no_alloc_test` pattern with an always-throw
`operator new` around the real path (D2).

---

## 11. Close / drain / destruction (ADR Decision 15 mapped to Uring)

### 11.1 close_admission

`close_admission()` (concrete backend method, mirroring `FakeAsyncBackend` /
`ThreadPoolBackend`) takes the **admission transaction mutex** (`admission_mtx_`) and calls
`arena_.close_admission()`. The submit path holds the same mutex across the Stage 1–5 acceptance
protocol (reserve → ... → `commit_binding` release-store, the LP), so:

- a submit already inside the protocol completes its LP before close returns (close waits);
- a submit that starts after close observes `admission_closed_` at `reserve()` and rejects
  synchronously with `invalid_state`, Completion idle, zero residue;
- `arena_.commit()` deliberately carries NO admission check (C2e reference boundary: "commit() is
  only the LP's slot half") — the arbitration lives in the backend admission transaction, exactly
  as Phase E proved for ThreadPool (`tp_c2e_close_waits_for_inflight_acceptance_lp`).

**Single-driver does NOT excuse close (AGENTS.md / task §12):** even though submit/poll/cancel are
serialized by `AsyncIoContext::access_mtx_`, `close_admission()` is called directly on the
backend (the C2e conformance pattern) and races a concurrent submit through the context. The
admission mutex is required; a comment claiming single-thread is not a substitute for Decision 15
arbitration.

### 11.2 Waking a parked wait_one — DESIGN PENDING (D4)

The current `wait_one()` blocks in `io_uring_wait_cqe` / `io_uring_submit_and_wait` under the
legacy serialized contract (`AsyncIoContext::wait_one()` falls back to blocking under
`access_mtx_` when `backend_->wait_source() == nullptr`). That fallback is the documented
single-driver path; `ApplicationRuntime` rejects it for the multi-participant production path.

**The frozen D4 target is: `UringBackend` implements `BackendWaitSource` and plugs into the
existing `AsyncIoContext` wait protocol — NOT a private blocking protocol inside the backend's
own `wait_one()`.** The standard protocol (`src/async/async_io_context.cpp`) is already:
`snapshot()` → `poll()` under `access_mtx_` → park WITHOUT `access_mtx_` via
`wait_for_change(token)` → final non-blocking `poll()` on control interruption. A backend opts in
by overriding `AsyncBackend::wait_source()` to return a `BackendWaitSource*`; the reference
reusable implementation is `detail::ReadyWaitSource` (used by `ThreadPoolBackend`), and
`interrupt_backend_waiters()` routes through `wait_source()->interrupt_all()`. Building a private
eventfd-based blocking wait inside `UringBackend::wait_one()` would duplicate that machinery,
force the legacy "block under `access_mtx_`" branch (starving every other poll/reap path), and
bypass `interrupt_backend_waiters()` — the exact shape the split-wait architecture exists to avoid.

The candidate `BackendWaitSource` for Uring parks on **two** independently observable signals:
the ring fd (pollable; `EPOLLIN` when the CQ has events) for progress, and a control eventfd for
interruption:

```text
wait_for_change(token):
    ppoll { ring_fd -> EPOLLIN (CQ progress) ; control_eventfd -> interrupted }
    (distinguish progress vs control via which fd fired; bump the matching epoch)
interrupt_all():
    write(control_eventfd)   # one-shot control wake; future waits re-park normally
```

The CQ-progress eventfd registration (when used) is via `io_uring_register_eventfd(&ring, fd)`
(opcode `IORING_REGISTER_EVENTFD`) — **there is no `IORING_SETUP_EVENTFD` setup flag**; an earlier
draft cited that nonexistent flag and is corrected here. The ring fd itself is directly pollable,
so a minimal implementation may park on the ring fd for progress and use a separate eventfd only
for control wakes.

**D4 frozen-design items still required before implementation** (this section is DESIGN PENDING):
eventfd register/unregister lifecycle; counter drain semantics; spurious-notification handling
(the registered eventfd is a hint, not a 1:1 CQE count); the CQE-vs-control-wake race in
`wait_for_change`; one-shot vs epoch `interrupt_all` semantics; the proof that no wake is lost
under the snapshot/park ordering. Periodic-timeout polling (`io_uring_wait_cqe_timeout`) and
`io_uring_peek_batch` spinning remain rejected as the primary progress mechanism (banned as sole
progress). Until D4 lands, the context's legacy serialized `wait_one` path remains the
intermediate state.

### 11.3 Destruction

`~UringAsyncBackend` verifies quiescence (arena snapshot `slot_in_use == 0 &&
accepted_outstanding == 0 && backend_ready == 0`, dispatch ring empty, no prepared/kernel-owned
scratch, no cancel bookkeeping) and fail-fasts in BOTH Debug and Release otherwise
(`io_uring_queue_exit` only after proof of no kernel ownership). No implicit drain/cancel/wait
(ADR Decision 15 / AGENTS.md §14). The `AsyncIoContext` outstanding fail-fast remains the
context-level guard.

---

## 12. request_capacity vs uring_queue_depth (task §11)

Two independent configuration concepts:

| Resource | Capacity | Full behavior | Error |
|---|---|---|---|
| `request_capacity` (arena slots == dispatch ring == scratch) | construction-time | arena full at `reserve` | synchronous `would_block`, Completion idle, no borrow |
| `uring_queue_depth` (SQ/CQ ring) | construction-time (`io_uring_queue_init`) | SQ full at dispatch | no admission error — accepted requests stay `enqueued`, dispatch flushes/retries; `queue_full_retries` (backend bump) |

The relationship `request_capacity > queue_depth` MUST be legal: excess accepted requests remain
`enqueued` (userspace-owned), waiting for ring capacity — their dispatch-ring position was
reserved before commit (ADR Decision 13: "If ring depth ... is smaller than request capacity,
excess accepted requests may remain enqueued only because their queue position/linkage was
reserved before commit"). `request_capacity == queue_depth` and `<` are also legal. The two stats
never merge: `would_block` (arena) vs `queue_full_retries` (SQ pressure) — the context-level
`tally_submit` counts the former; the backend bumps the latter on a dispatch flush, exactly like
today's `get_sqe_with_pressure`.

**Public API decision (needs explicit approval; AGENTS.md §16.1):** the current constructor
`UringAsyncBackend(unsigned queue_depth = 64)` must gain a capacity dimension. Options:
(a) `UringConfig{request_capacity = 64, queue_depth = 64}` struct constructor, keeping the legacy
one as a deprecated wrapper (mirrors `ThreadPoolConfig`); (b) two-argument constructor with
defaults. Recommendation: **(a)** — mirrors Phase E's config-struct pattern, keeps one clear
vocabulary, and the legacy constructor remains source-compatible. The plan records this as a
public-API change requiring the §16.1 Release gate and `docs/api-reference.md` update.

---

## 13. Stub vs real-liburing test matrix (task §13)

| Path | Build | Requirement | Phase D evidence |
|---|---|---|---|
| Stub / no-liburing | default | project builds without liburing; stub rejects synchronously; **never presents itself as KernelIo-conforming**; no arena/lifecycle false claim | stub subset of `uring_backend_test` + stub driver cases + gate verdict: stub-mode KernelIo stays NOT CONFORMING / INCOMPLETE (never skip-as-pass) |
| Real liburing | `--with-liburing=true` | THE Phase D correctness evidence | real-path 8-case shared suite + capacity/close/drain driver cases + per-backend C2 integration targets + `uring_submit_failure_test` (after the P-D0-INF-01 link fix) + submit-failure state machine on the NEW dispatch model |

**Infrastructure blocker recorded:** P-D0-INF-01 (link break) blocks real-path evidence until
fixed in D1. **Environment caveat recorded:** WSL2 kernel io_uring availability unverified —
if `available()==false` on this host, the completion report MUST state `REAL LIBURING EVIDENCE
UNAVAILABLE` for the run environment; a stub-only green never substitutes (AGENTS.md §16.5,
task §13).

---

## 14. C2 not_implemented records closure matrix (task §14)

| Evidence record | Current state | Owning Phase D PR | Real test required to flip to `implemented` |
|---|---|---|---|
| `uring_capacity_not_implemented` | not_implemented (INCOMPLETE in Uring verdict) | D1 | `conformance_capacity_uring` driver case passing on the REAL path: accepts exact capacity, (N+1)th → `would_block` idle, exact stats split, `max_outstanding <= capacity`, recycle after cancel→reap→reset |
| `uring_c2b_identity_not_implemented` | not_implemented | D1 (core) + D3 (matrix) | identity/generation integration: token↔slot round-trip, stale-CQE generation rejection, Scheme-B pending-cancel window, cancel-winner vs enqueue, publication boundary (reap gates ready), exactly-one winner |
| `uring_c2c_borrow_waiter_not_implemented` | not_implemented | D3 | `uring` integration of the arena borrow/waiter/lease rows: borrow active from commit through backend_ready-before-reap while kernel-owned, registration orthogonal to running, wait-cancel vs I/O-cancel independence, stale waiter harmless |
| `uring_c2d_failure_injection_not_implemented` | not_implemented | D2 | `SLUICE_URING_INTERNAL_TESTING`-guarded seams: pre-commit injection at reserve/prepare/commit-boundary (real rollback), post-commit dispatch-failure injection, always-throw no-alloc accepted-terminal path, dispatch-failure vs cancel exactly-one-winner |
| `uring_c2e_close_drain_not_implemented` | not_implemented | D4 | `conformance_close_drain_uring` driver case + deterministic windows: close while pending/enqueued/running, close ‖ LP arbitration, one-shot control wake (eventfd), drained != releasable, quiescent destroy, non-quiescent death (Debug AND Release) |

**Manifest discipline (task §14):** records flip ONLY after command-backed evidence exists on the
final PR head; never before. The `verify-backend-conformance.py` KernelIoProfile hard-codes
(NOT CONFORMING; lifecycle/backend_specific → INCOMPLETE) must be lifted in the same PR that
flips the last record — the gate and the evidence land together (D4).

The Phase D exit is not "Uring tests pass": it is **all Phase-D-owned records `implemented` with
real evidence + the aggregate gate running Uring through its normal per-backend evaluation +
Uring's KernelIo verdict moved from NOT CONFORMING to the expected conforming state on the real
path (and an honest INCOMPLETE/NOT CONFORMING classification on the stub path)**.

---

## 15. Mutation / detector strategy (task §15)

For each defect class: detector case in the Phase D test corpus; deterministic seam; mutation;
RED observable. All detectors follow the C2 pattern (single-point production mutation → the
focused case fails; evidence recorded under `docs/verification/` as Phase D
mutation-evidence ledgers, e.g. `phase-d-*-mutation-evidence.md` — these files
are planned deliverables created in D2/D3, not current repository paths).

| # | Defect | Detector | Seam | RED observable |
|---|---|---|---|---|
| 1 | SQE acquisition moved back before acceptance | submit-time pressure probe: SQE fill must NOT happen in submit | `SLUICE_URING_INTERNAL_TESTING` pause at submit | a submit that pauses before enqueue must show zero SQEs written (scratch empty) / submit returns before any `io_uring_get_sqe` |
| 2 | Completion reverse map returns as identity authority | `resolve_completion` scan used; no map | grep-level negative compile + runtime | any parallel map is a compile/authority finding; stale `Completion*` cancel must resolve via the arena only |
| 3 | CQE resolves to a stale full_handle (released/reused slot) | inject a CQE carrying a released request's op_cookie after the slot is reused | CQE injection seam | the stale full_handle fails `arena.validate` → CQE dropped → new occupant unaffected → case passes; if it mutates the new occupant → case fails |
| 4 | duplicate op CQE after reap/reset | same as 3 + duplicate-CQE test | injection | duplicate CQE after reap/reset mutates the new generation → case fails |
| 4b | enqueued-cancel NOP carries the request's op_cookie (the §6.0/§6.2 invariant) | cancel a prepared-suffix enqueued slot, then submit the neutralized NOP and inspect its user_data | dispatch+submit seam | the submitted NOP's user_data == NEUTRAL_COOKIE, not the op_cookie; a later injected "NOP CQE carrying op_cookie" must be rejected → case fails if op_cookie leaks into the kernel |
| 5 | partial-submit suffix terminalized | submit hook returning k < prepared | `UringBackendSubmitTestHooks`-style submit seam (migrated to the dispatch seam) | suffix Completion terminalized → case fails (suffix must stay outstanding, retriable) |
| 6 | partial-submit prefix re-submitted | hook returns k then k' with overlapping prefix | seam | duplicate execution / double CQE for prefix → double-terminal fail-fast or case failure |
| 7 | permanent error terminalizes kernel-owned prefix | hook returns permanent error after accepting some | seam | prefix slot gets a terminal before its CQE → fail-fast or case failure |
| 8 | pending cancel then op still dispatched | Scheme-B window (cancel between commit and enqueue) | pause gate | the op's SQE appears in the ring/scratch after cancel won → case fails |
| 9 | cancel CQE overwrites ordinary result | cancel CQE injected after op CQE | injection | terminal overwritten → arena terminal-winner no-op → case fails |
| 10 | post-accept path allocation failure strands request | always-throw `operator new` | no-alloc harness | request never reaches a terminal or process aborts → case fails |
| 11 | ring pressure mapped to admission `would_block` | SQ-full probe: enqueue succeeds beyond SQ depth | real path with queue_depth < capacity | `would_block` returned after commit → case fails (must be `queue_full_retries`, request stays accepted) |
| 12 | close returns before the Step-5 LP | close ‖ in-flight submit (pause between commit and LP) | admission pause gate | a submit after close returns completes an LP → case fails |
| 13 | destructor silently drains/cancels | non-quiescent destruction | death test | destruction with accepted/kernel-owned work returns instead of fail-fast |
| 14 | CQE path publishes Completion directly, bypassing reap | CQE-handler publish probe | injection | a Completion becomes ready without `arena_.reap` → case fails |
| 14b | accepted-prefix `n` consumed from `dispatch_ring` instead of the SQ ledger (§5.6) | partial-submit then neutralize a prefix SQE, then `io_uring_submit()` returning a count that spans the neutral NOP | submit-count injection + neutralize seam | a request whose SQE was NOT accepted (a neutral NOP was accepted in its place) gets `mark_running` → arena-state ≠ kernel ownership → case fails (the ledger must consume the exact physical-SQ prefix by kind) |

---

## 16. Current gate output (stub baseline, verbatim — clean rebuild)

```text
Backend: Fake (ReferenceProfile)          mode=deterministic  shared_suite=PASS
  overall               ELIGIBLE
Backend: ThreadPool (BlockingIoProfile)   mode=real  shared_suite=PASS
  overall               ELIGIBLE
Backend: Uring (KernelIoProfile)          mode=stub  shared_suite=PASS (stub subset)
  lifecycle INCOMPLETE  (5 not_implemented records:
                        uring_capacity / uring_c2b_identity / uring_c2c_borrow_waiter /
                        uring_c2d_failure_injection / uring_c2e_close_drain)
  backend specific INCOMPLETE  (uring_backend_contract=INCOMPLETE)
  overall               NOT CONFORMING
    reason: kernel profile built as stub (Phase D not implemented)
RESULT: PASS (all mandatory gates satisfied; KernelIo NOT CONFORMING is expected before Phase D)
GATE_EXIT=0
```

Notes:
- An earlier gate run (before a `xmake f -c` clean rebuild) reported spurious `shared_suite
  RUN_FAIL` and `mode=unknown` for Uring; those were artifacts of a stale `uring_backend.cpp.o`
  left by an `--with-liburing` config round-trip, not Uring test failures. All results in this
  document are from the clean rebuild (151/151 suite + the output above).
- Phase D changes the gate so Uring is evaluated by its own evidence rather than by the
  KernelIoProfile hard-code (D4).

---

## 17. Proposed PR decomposition (task §16)

### 17.1 Why not a naive four-way split

The task's candidate split (D1 admission/identity → D2 dispatch → D3 CQE/cancel → D4
close/drain) fails the intermediate-state test:

- **D1 cannot land alone.** Replacing `ops`/`comp_to_op`/`cancel_to_op`/`pending_sqes` with the
  arena while keeping legacy SQE acquisition in submit would require either (a) commit-then-
  SQE-acquire — post-commit SQE acquisition failure cannot roll back an accepted request
  (I3/I14), or (b) SQE-acquire-then-commit — the pre-commit SQE ownership the ADR forbids, plus
  a parallel token→OpRec map (dual identity). "Arena + old submit_read" wrapped is explicitly
  rejected by the task and the ADR.
- **D2 cannot land after D1.** The dispatch ring, per-slot scratch, and token encoding are what
  make D1's admission meaningful; they are one coherent ownership cut.
- **CQE and cancel cannot be split from the identity cut.** A CQE handler that still publishes
  directly (old model) while the arena owns identity would be the exact "CQE → Completion direct
  publish bypassing RequestArena reap" violation (hard constraint 11). Cancel without arena
  `resolve_completion` keeps `comp_to_op`.

### 17.2 The dependency graph and intermediate invariants

```text
D1  production identity+admission+dispatch+terminal cut (atomic; eliminates dual authority)
 |   intermediate invariant: Uring is fully RequestArena-driven; legacy maps/deques/ids gone;
 |   not-yet-implemented: C2d seams, C2e close/eventfd, full C2b/c matrix, gate lift
 +-> D2  failure injection + no-alloc evidence (C2d seams on the D1 model)
 |       intermediate invariant: D1 semantics unchanged; only test-only seams added
 +-> D3  cancel/generation/stale race matrix (C2b/C2c integration tests + any fixes they
 |       surface; Scheme-B window; cancel-CQE races; borrow/waiter rows)
 |       intermediate invariant: no production restructuring, only race-proof tests
 +-> D4  close/drain/destruction (close_admission + BackendWaitSource wake + death tests) +
 |       manifest flips + gate KernelIo lift + real-liburing CI evidence + docs
 v
Phase D complete (all records implemented; aggregate gate drives Uring normally)
```

Every PR leaves the repository honest: Uring's manifest records stay `not_implemented` until the
PR that closes each; the gate keeps Uring NOT CONFORMING until D4 lifts it; no Phase F/G scope
enters any PR.

### 17.3 PR inventory

| PR | Scope | Must prove |
|---|---|---|
| **D1 — "refactor(async): Uring RequestArena admission + SQE ownership split"** | UringConfig; arena ownership; five-stage submit (reserve→validate→prepare→binding→commit→enqueue→ring push, allocation-free); dispatch ring + per-slot scratch; token user_data; dispatch pass (fill→submit→prefix mark_running→suffix retained; NOP neutralization; permanent-failure per-slot terminal + submit poison); CQE → record_terminal → reap; cancel via arena + cancel tokens; P-D0-INF-01 link fix; stub remains honest; 8-case shared suite real path; capacity driver case; basic unit + death tests; manifest `uring_capacity_not_implemented` flip (with real evidence) | no dual authority; I3/I9/I14/I17/I19 on the real path; token↔slot round-trip; suffix identity across partial submits; exactly-one terminal; reap-only publication |
| **D2 — "test(async): Uring failure injection + accepted-terminal no-alloc (C2d)"** | SLUICE_URING_INTERNAL_TESTING seams on the D1 model (submit-stage injection at reserve/prepare/commit-boundary; post-commit dispatch-failure injection; pause gates); migrate `uring_submit_failure_test` onto the new seams (old UringBackendSubmitTestHooks retired); always-throw no-alloc accepted path; mutation detectors 1,5,6,7,10,11 | per-stage rollback zero-residue; dispatch-failure vs cancel exactly-one-winner; zero post-accept allocation; record flip `uring_c2d_failure_injection_not_implemented` |
| **D3 — "test(async): Uring cancel/generation/stale race matrix (C2b/C2c)"** | Scheme-B pending-cancel window; enqueued-cancel no-execute (disarm-before-terminal, §5.4); running-cancel intent + cancel-SQE; cancel-CQE race matrix (original vs cancel vs -ENOENT, both orders, duplicates); stale-CQE generation; borrow/waiter rows on the real path | mutation detectors 2,3,4,4b,8,9,14b; record flips `uring_c2b_identity_not_implemented`, `uring_c2c_borrow_waiter_not_implemented` |
| **D4 — "refactor(async): Uring close/drain/destruction + KernelIo conformance closure"** | `close_admission()` + admission_mtx_ arbitration; `BackendWaitSource` implementation (registered eventfd + pollable ring fd; §11.2 frozen design first); quiescent destruction + death tests; manifest flips `uring_c2e_close_drain_not_implemented`; gate lift (verify-backend-conformance.py KernelIo hard-code removal; stub-mode honest classification); real-liburing CI evidence; docs (design doc, gate ledger, divergence-registry DIV-02/DIV-14 updates, api-reference, roadmap status) | mutation detectors 12,13,14; aggregate gate runs Uring through normal evaluation; real-path verdict conforming; roadmap Phase D complete |

---

## 18. First implementation PR (D1) — detailed design (task §17)

**PR name:** `refactor(async): Uring RequestArena admission + SQE ownership split`

**Scope**
- `UringConfig{request_capacity, queue_depth}` (defaults 64/64) + legacy constructor as a
  deprecated wrapper.
- Own a `detail::RequestArena` per backend; five-stage admission transaction in all four
  `submit_*` paths (reserve → descriptor validation → prepare → binding → commit → enqueue →
  ring push), mirroring `ThreadPoolBackend::submit_size/void` including `begin_binding` /
  `commit_binding` / `install_binding` / `rollback_binding_before_accept`.
- Construction-time bounded dispatch ring (capacity == request_capacity) of `SlotHandle`s.
- Construction-time per-slot SQE scratch array (`full_handle`, `op_token`, `cancel_token`,
  `sqe_prepared`, `kernel_owned`, `cancel_pending`).
- Opaque-cookie user_data encoding (§7.2): monotonic op/cancel/neutral cookies (never reused,
  fail-fast before wrap); `io_uring_sqe_set_data64` / `io_uring_cqe_get_data64`.
- Dispatch pass in `poll()`/`wait_one()` (§5.2) — SQE fill, submit, prefix→`mark_running`,
  suffix retained, graceful-backoff state handling, per-slot permanent-failure terminal + submit
  poison.
- CQE handler (§6.1) — bounded scratch lookup → full `SlotHandle` → arena full-generation
  validation; neutral-cookie NOP drop; `record_terminal`; reap-only publication.
- `cancel()` via `arena_.resolve_completion` + `arena_.cancel` + the §5.4 dispatch-membership
  protocol (`remove_exact` + neutral-cookie NOP neutralization for enqueued prepared-suffix) +
  unified-dispatch-machine cancel SQE for running; cancel CQE informational.
- P-D0-INF-01 fix: `uring_submit_failure_test` links `sluice_async` (and migrates onto the new
  submit seam) so real-path evidence is producible.
- Descriptor validation (DIV-14 Uring closure): negative fd, null buffer with nonzero length,
  `checked_uring_length`, offset range — synchronous `invalid_argument`, idle Completion, no
  slot/borrow/execution; non-negative closed fd accepted and completed with the real syscall
  error (AGENTS.md §9.1).
- Stub path: unchanged honest synchronous rejection; stub must not claim RequestArena
  conformance.

**Out of scope**
- C2d seams/injection, close_admission/`BackendWaitSource` wake, the full C2b/C2c race matrix, manifest
  flips beyond `uring_capacity_not_implemented`, the gate KernelIo lift, Phase F/G, registered
  buffers/files, multi-driver ring, public submit API changes.

**Files expected to change** (planned deliverables; future-file names are
shown as identifiers, not current repository paths)
- `include/sluice/async/uring_backend.hpp` (UringConfig, arena members, guarded test seams)
- `src/async/uring_backend.cpp` (full restructure)
- `xmake/experimental.lua` (link fix for the `uring_submit_failure_test` target; internal-testing
  seams mirroring the ThreadPool pattern)
- the existing targets uring_backend_test and uring_submit_failure_test, the shared
  backend_conformance_test / backend_conformance_driver_test cases (the new
  `conformance_capacity_uring` driver case), plus a planned new death-test target
  (`uring_backend_death_test.cpp` — to be created in D1)
- `scripts/backend_conformance_manifest.py` (flip `uring_capacity_not_implemented` only after
  evidence; add the capacity driver case record for Uring)
- a planned D1 compliance-gate ledger and a planned frozen design doc
  (`phase-d-compliance-gate.md`, `phase-d-uring-migration.md` — to be created in D1, not
  current repository paths),
  (frozen design, pre-implementation), `docs/architecture/divergence-registry.md` (DIV-02/DIV-14
  Uring notes), `docs/api-reference.md` (UringConfig), `docs/architecture/remediation-roadmap.md`
  (Phase D stays NOT IMPLEMENTED, sub-status D1)

**Production invariants (D1 exit criteria)**
- I1–I19 hold on the real path; specifically I3 (transactional rejection: any pre-commit failure
  leaves Completion idle with zero slot/borrow/SQE/ring residue), I9 (post-accept zero
  allocation), I14 (rejection never fabricated as terminal and vice versa), I17/I19 (Scheme-B
  enqueue no-op + pin protocol), I11 (reap-only publication).
- `request_capacity > queue_depth` legal; excess accepted requests remain enqueued.
- Stub build unaffected and honestly non-conforming.

**State transitions** — exactly the arena vocabulary (§4.2); the only backend-private state is
the scratch (`sqe_prepared`/`kernel_owned`/cancel bookkeeping), documented as non-observable
dispatch bookkeeping.

**Resource ownership** — arena slots, ring, scratch all construction-time; no post-accept
allocation; capacity pressure → `would_block` at reserve; SQ pressure → `queue_full_retries` at
dispatch.

**Lock/thread assumptions** — production: all entry points serialized by
`AsyncIoContext::access_mtx_`; the arena leaf mutex is uncontended but required (correct by
construction); the dispatch pass runs only in poll/wait_one; close_admission is D4 (until then
no admission mutex is needed because the context serializes submits — D1 documents this
intermediate state explicitly). Test seams may pause the driver thread and call cancel directly;
the scratch/ring mutations are then guarded by the same serialization domain the seams run
under (D1 design must pick the seam lock layout — mirror ThreadPool's work_mtx_ + gate
placement; this is a frozen-design item).

**user_data encoding** — §7.2 opaque cookie (kind bit + monotonic never-reused value); cancel
targets the op cookie via `prep_cancel64(sqe, op_cookie, 0)`; neutralized NOPs carry a reserved
neutral cookie.

**Rollback rules** — every pre-commit failure rolls back through
`rollback_reserved_or_prepared` (and `rollback_binding_before_accept` when the binding CAS won)
with zero residue; post-commit failures are terminal results, never rejections.

**Tests (D1)**
- Real path (when the host kernel permits): 8-case shared suite; capacity driver case;
  partial-submit suffix/prefix splits (migrated submit hooks); token round-trip; exactly-once
  terminal; reap-only publication; cancel basic; death (non-quiescent destruction).
- Stub path: existing stub subset continues to pass; driver stub case classified honestly.
- Mutation detectors 1, 5, 6, 7, 10, 11, 14b (RED on the single-point mutations; 14b proves the
  accepted-prefix is consumed from the SQ ledger, not `dispatch_ring`).

**Manifest changes** — flip `uring_capacity_not_implemented` → `implemented` ONLY after the
real-path `conformance_capacity_uring` evidence exists on the PR head; add the
`uring_backend_contract` real-mode evidence note.

**Docs changes** — the frozen design doc (required BEFORE D1 production code per AGENTS.md §8),
the D1 compliance-gate ledger (Gate 0–4 fields, state machine, lock/atomic table, capacity
model, wake model, shutdown semantics, evidence PENDING→PASS), divergence registry updates,
api-reference, roadmap planning status.

**Exit criteria** — Clang Debug full suite green (stub); real-liburing suite green on a host
where `available()==true` (recorded explicitly if unavailable); `uring_submit_failure_test`
links and passes on the new seams; no legacy container remains; `git diff --check` clean;
D1 ledger fields PASS only after the commands ran.

---

## 19. Risks / unresolved design questions (task §18-16)

1. **Token encoding (RESOLVED → opaque cookie, was: generation fragment).** An earlier draft
   selected a packed slot + generation-fragment token (candidate C) and recorded a residual
   2^47–2^57 ABA window as an accepted bound. That selection is **withdrawn**: it contradicts the
   no-wrap generation contract (`request_key.hpp` / `free_slot_locked_` fail-fast) once the design
   admits a future NOP CQE that can arrive after slot release (§6.2). The frozen selection is
   candidate F — a non-authoritative opaque cookie routing CQEs back to a scratch slot that carries
   the authoritative full `SlotHandle` (§7.2). I6 therefore holds in perpetuity (full-generation
   validation on every CQE; cookies are never reused and fail-fast before their 63-bit payload
   exhausted, mirroring the generation's own discipline). The only recorded bound is the cookie-
   space fail-fast; **no generation-fragment bound is recorded**.
2. **`submit_poisoned` policy (P1):** after a permanent `io_uring_submit` error, is rejecting
   NEW submissions synchronously (current semantics, preserved) the right admission policy, or
   should the ring be reusable after the suffix is drained? Current recommendation: keep poison
   (a ring that returned a permanent error is not trustworthy); record in the D1 design.
3. **D4 wait/wake (P1, D4) — DESIGN PENDING:** the frozen design for waking a parked `wait_one`
   must be written before D4. The target shape is for `UringBackend` to implement
   `BackendWaitSource` and plug into `AsyncIoContext`'s existing snapshot/poll-under-`access_mtx_`/
   park-WITHOUT-`access_mtx_`/final-poll protocol (§11.2) — NOT a private blocking protocol inside
   the backend. A registered eventfd (`io_uring_register_eventfd`) plus the pollable ring fd is the
   candidate `wait_for_change` implementation; its one-shot semantics (counter drain, final reap,
   spurious-notification handling, CQE-vs-close race) need the C2e-level proof. The earlier
   draft's `IORING_SETUP_EVENTFD` was a documentation error (no such setup flag exists).
4. **WSL2 real-path availability (P1, environment):** if `io_uring_setup` fails on this host,
   real-path evidence is unavailable here; CI with real liburing must be wired (or a
   maintained host designated) before the D4 gate lift. P-D0-INF-01 must be fixed regardless.
5. **Public constructor change (P2):** `UringConfig` vs two-argument constructor — needs the
   §16.1 public-API approval path and api-reference update.
6. **Cancel tallies (P2):** `canceled_ops` tally ownership — terminal_won (arena cancel) vs
   confirmed `-ECANCELED` CQE (record_canceled); the D3 matrix must pin "at most one tally" in
   every interleaving, mirroring the C2d ThreadPool invariant.
7. **D1 size:** the atomic first cut is large (identity + admission + dispatch + CQE + cancel +
   build fix). Alternative considered and rejected: any smaller cut leaves a dual-authority
   intermediate state (§17.1). Mitigation: the frozen design doc + vertical slices within D1
   (arena admission on stub-safe seams first, then dispatch, then CQE/cancel), each with a
   failing-test-first step.

---

## 20. Command-backed audit evidence (this audit, actual runs)

| Command | Result |
|---|---|
| `git fetch origin && git status --short && git rev-parse origin/master` | clean; `1349a6fdf63f760d73cec9d567bb3fecd46fa695` |
| `gh pr view 73 --json state,mergedAt,mergeCommit,headRefOid` | MERGED, `1349a6f`, 2026-08-08T06:28:26Z |
| `pkg-config --modversion liburing` | 2.14 |
| `xmake show -t sluice_async` | `SLUICE_HAS_LIBURING` + liburing 2.14 package + uring/uring-ffi links |
| `xmake f -m debug --toolchain=clang -y` + `xmake build sluice_core` + `sluice_async` + `-g test` | all build ok (clean rebuild) |
| `xmake test -v` | **151/151 passed, 0 failed** |
| `xmake f --with-liburing=true` + build `sluice_async` + `uring_backend_test` | build ok (real path compiles/links) |
| build `uring_submit_failure_test` (liburing) | **LINK FAIL** — P-D0-INF-01 (undefined `completion_authority_fail_fast`) |
| `python3 scripts/tests/test_backend_conformance_manifest.py` | Ran 152 tests, OK |
| `python3 scripts/verify-backend-conformance.py` (stub, clean rebuild) | Fake ELIGIBLE, ThreadPool ELIGIBLE, **Uring NOT CONFORMING** (5 not_implemented; kernel hard-code); `RESULT: PASS`; GATE_EXIT=0 |
| `python3 scripts/check-doc-links.py --self-test` / `check-doc-links.py` | SELF-TEST PASS / VERDICT: PASS |
| `python3 scripts/verify-architecture-docs.py` | OK |
| `git diff --check` | clean |

**REAL LIBURING EVIDENCE STATUS:** liburing 2.14 is installed and the real path compiles/links
for `sluice_async` and the uring test targets except `uring_submit_failure_test` (P-D0-INF-01).
Real-path EXECUTION on this WSL2 host is not yet demonstrated (`available()`/kernel support);
if unavailable, Phase D completion on this environment must record
`REAL LIBURING EVIDENCE UNAVAILABLE` per the task's §19 requirement.

---

## 21. Working tree / provenance

- New file: `docs/architecture/phase-d-uring-migration-plan.md` (this document).
- Roadmap: Phase D status updated to reflect D0 planning (`PLAN READY / NOT IMPLEMENTED`) — see
  the roadmap status section below; no `COMPLETE` claim.
- No production source, header, build file, manifest, or test file modified.

**Provenance (stable facts only — live head / commit count / diff stats are left to the PR UI,
which stays correct as the branch is revised; hard-coding them here would make this section
stale on every edit):**

```text
Branch:    docs/phase-d-uring-migration-plan
PR:        #76 (Draft)
Baseline:  1349a6f  (origin/master at audit time)
Scope:     docs-only (this file + remediation-roadmap.md); no production code
```

**D1 manifest-flip discipline (no gray area):** D1's exit criteria name the
`uring_capacity_not_implemented` flip, but D1 also permits the real-liburing suite to be
UNAVAILABLE on a host whose kernel refuses io_uring. To avoid a "D1 COMPLETE but the real-path
evidence never ran and the manifest is in an unknown state" outcome, the rule is binary: **the
record flips to `implemented` ONLY after command-backed real-path `conformance_capacity_uring`
evidence exists on the PR head.** If the host cannot produce real-path evidence, D1 may still

**D1 manifest-flip discipline (no gray area):** D1's exit criteria name the
`uring_capacity_not_implemented` flip, but D1 also permits the real-liburing suite to be
UNAVAILABLE on a host whose kernel refuses io_uring. To avoid a "D1 COMPLETE but the real-path
evidence never ran and the manifest is in an unknown state" outcome, the rule is binary: **the
record flips to `implemented` ONLY after command-backed real-path `conformance_capacity_uring`
evidence exists on the PR head.** If the host cannot produce real-path evidence, D1 may still
merge but the record stays `not_implemented` (INCOMPLETE) and the gap is explicitly carried into
a follow-up — it is never flipped on stub-only evidence (AGENTS.md §16.5).
