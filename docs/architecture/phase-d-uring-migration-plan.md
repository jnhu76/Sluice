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
| `cancel_to_op` (`unordered_map<__u64,__u64>`) | **Yes** — second identity system for cancel ops (cancel-id → op-id) with no generation | cancel intent | none | none | none | none | **no bound** | `emplace` per cancel SQE | none | **Must disappear.** Cancel bookkeeping moves to per-slot scratch; the cancel token carries the cancel-marker bit + the same slot/generation fragment as the target op token |
| `pending_sqes` (`deque<PendingSqe>`) | partial — ordered list of not-yet-accepted SQE ids (ops + cancels) | submit-order authority | **Yes** — "userspace-owned vs kernel-owned" split lives here | none | none | none | **no bound** | `push_back` per submit/cancel | `accept_submitted_prefix` pops the accepted prefix | **Must disappear.** Replaced by a construction-time bounded dispatch ring of `SlotHandle`s (mirroring ThreadPool's `BoundedDispatchQueue`) + per-slot SQE scratch (`sqe_prepared`, `kernel_owned`) |
| `next_id` (monotonic counter) | **Yes** — the kernel-visible identity source | none | none | none | none | none | none | none | none | **Eliminated.** user_data becomes an encoded RequestKey token (slot + generation fragment + cancel marker) |
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
  -> decode token -> (slot, fragment, cancel_marker)
  -> cancel CQE: informational only; clear scratch cancel state; NEVER record terminal
  -> op CQE:     validate fragment vs generation_of(slot) (drop on mismatch)
                 arena state check (running) ; drop if terminal_stored
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
| Kernel-visible token | encoded `RequestKey` fragment (slot + generation fragment + cancel marker) | §7 |
| Execution ownership | io_uring ring (SQE/CQE) — the ONLY thing io_uring owns | `mark_running` = kernel acceptance; CQE → `record_terminal` |
| Terminal winner | `RequestArena::record_terminal` / `cancel` (first wins, losers no-op) | CQE handler never publishes; only stores via the arena |
| Completion-ready publication | `RequestArena::reap` (sole authority, through the slot binding) | poll/wait_one call `arena_.reap(sink)` only |
| SQE dispatch bookkeeping | backend fixed per-slot scratch (keyed by slot, generation-gated) | scratch read only after arena-validated transitions |
| Cancel intent/terminal | arena `cancel_intent_` / `cancel()` | scratch only mirrors intent for cancel-SQE bookkeeping |
| Submit-order / dispatch queue | backend bounded dispatch ring (capacity == request_capacity) | §5 |
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
      backend_ready (cancel won) -> if scratch[h].sqe_prepared:
                                       io_uring_prep_nop(sqe_at(scratch[h]));  # neutralize
                                    scratch[h].clear(); continue
      other     -> invariant violation (fail-fast, both modes)
    sqe = io_uring_get_sqe(&ring)
    if sqe == nullptr:                                # SQ ring pressure
      flush: submit the prepared batch (see Phase 2), then retry get_sqe once
      if still nullptr: stop the pass (enqueued slots stay enqueued; retried next driver call)
    prep sqe from arena slot data (op_kind, borrow fd/addr/len/offset)  [no per-op alloc]
    token = encode_token(h)                            # §7
    io_uring_sqe_set_data64(sqe, token)
    scratch[h] = { token, sqe_prepared=true, kernel_owned=false }
  # Phase 2 — submit the prepared batch.
  if no new prepared SQEs: return
  n = io_uring_submit()
  if n < 0:
    transient (-EINTR/-EAGAIN/-EBUSY): keep suffix prepared; retry next pass (like today)
    permanent: for each prepared-not-submitted SQE:  # provably userspace-owned: not in ring
                 record_terminal(h, backend_error)   # the arena's terminal winner
               set submit_poisoned (new submissions reject synchronously with the error)
               return
  if n > #prepared: invariant violation (impossible accepted count) -> poison + fail-fast
  if n == 0 and previously zero: same policy as today (consecutive zero -> permanent)
  # Phase 3 — the accepted prefix is kernel-owned: enqueued -> running.
  for the first n prepared SQEs (in submission order):
    scratch[h].kernel_owned = true
    if !arena_.mark_running(h):  # current-generation slot already backend_ready
        # can only happen via a test-seam/concurrent cancel; the SQE was NOT neutralized
        # (the batch already went to the kernel) -> the CQE will arrive and be dropped
        # because the slot is terminal (winner already decided). Nothing to do.
    else: scratch[h].kernel_owned = true
  # suffix (prepared but not accepted) stays enqueued with sqe_prepared=true —
  # allocation-free retry next pass. Its identity is preserved (scratch token).
```

### 5.3 Partial-submit ownership proof

| Outcome | Ownership | Request state | What happens next |
|---|---|---|---|
| `io_uring_submit` returns `k` (0 < k < prepared) | prefix [0..k) kernel-owned; suffix [k..) userspace-owned, prepared | prefix → `running`; suffix stays `enqueued` | suffix SQEs remain in the SQ ring, `sqe_prepared=true`, retried by the next dispatch pass with **no re-prep and no new allocation**; identity preserved in scratch |
| `io_uring_submit` returns `k == prepared` | all kernel-owned | all → `running` | CQEs |
| transient error / one zero-progress | none submitted; suffix userspace-owned | stay `enqueued` | retry next pass (identical to today's `transient_submit_error` policy) |
| permanent error | none submitted (or only a provably-userspace-owned suffix) | suffix → `backend_ready(backend_error)` via `record_terminal`; kernel-owned prefix (if any) keeps its CQEs | new submissions reject with the stored error (`submit_poisoned`) — see §10 |
| impossible `k > prepared` | — | invariant violation | poison + fail-fast (both modes) |

Explicitly **forbidden** (ADR Decision 5, task §5): terminalizing the suffix, releasing the
suffix, losing suffix identity, allocating retry records after acceptance, duplicating SQEs,
treating positive partial submit as fatal, or re-submitting the already-kernel-owned prefix.

### 5.4 Why ring pressure is not admission `would_block`

`request_capacity` is the arena bound; `uring_queue_depth` is the SQ/CQ ring depth. They are
independent resources (ADR Decision 13 table; AGENTS.md §12). Ring pressure at dispatch time means
"the kernel's submission queue is full" — the request is ALREADY accepted (committed) and simply
waits. `would_block` is reserved for arena-full at `reserve()` — Completion idle, no borrow, no
execution. The two are distinguishable in stats: `would_block` → `queue_full_retries` (context
`tally_submit`); SQ pressure → backend `queue_full_retries` bump on the dispatch flush (the
existing Uring `queue_full_retries` semantics are retained for the dispatch flush, matching
today's `get_sqe_with_pressure` bump).

---

## 6. CQE identity / stale-generation / reap

### 6.1 CQE handler (poll/wait_one, under the driver serialization domain)

```text
decode cqe.user_data -> (cancel_marker, slot, fragment)
if cancel_marker:
    # informational cancel CQE (ADR Decision 11 kernel-owned; X3 exactly-once structural rule)
    if scratch[slot].cancel_pending && scratch[slot].cancel_token == token:
        scratch[slot].cancel_pending = false; scratch[slot].cancel_token = 0
    else: drop (stale/late cancel CQE)
    NEVER records a terminal, NEVER publishes
else:
    # op CQE
    if fragment != lowbits(generation_of(slot)):  drop        # stale generation (defense in depth)
    if arena state_of(slot) != running:            drop        # never-submitted/neutralized/terminal
        (scratch.kernel_owned is a diagnostic cross-check, NOT the authority)
    h = SlotHandle{slot, generation_of(slot)}
    if arena terminal already stored:              drop        # duplicate/late CQE; winner decided
    result = cqe->res -> TerminalResult
        res >= 0 (size op) -> ok_bytes(res) | (void op) -> ok_void
        res <  0           -> err(from_errno_value(-res))   # -ECANCELED -> canceled (confirmed
                                                             # cancel winner; tally canceled_ops)
    arena_.record_terminal(h, result)             # first winner wins; losers no-op
    scratch[slot].clear()
arena_.reap(sink)                                 # SOLE Completion-ready publication path
```

The `state_of(slot) != running` check is the authority: a CQE can only be a real terminal
candidate if the arena believes this slot is kernel-owned. A NOP-neutralized SQE's CQE arrives
for a `backend_ready` slot → dropped by the terminal-stored/state check. A CQE for a slot whose
SQE was never submitted (impossible via the kernel, possible via injected tests) is dropped.

### 6.2 Enqueued-cancel no-execute guarantee

A cancel that wins `pending -> backend_ready(canceled)` or `enqueued -> backend_ready(canceled)`
(arena `cancel()` Scheme B) means the operation SQE MUST NOT be submitted (ADR Decision 11
"enqueued"; AGENTS.md §10.3). Three windows:

1. **No SQE prepared** (enqueued, scratch empty): the dispatch pass skips the slot (state check
   at Phase 1) — the SQE is never written. Guaranteed by the ring + state check.
2. **SQE prepared, not yet submitted** (enqueued, `sqe_prepared`, between two dispatch passes):
   the dispatch pass re-validates each prepared SQE before submit and **overwrites the SQE with
   a NOP** (`io_uring_prep_nop` + same token) for slots that are no longer `enqueued`. The NOP
   CQE arrives for a `backend_ready` slot and is dropped by the §6.1 checks. Under the serialized
   driver model this window is unreachable in production; the NOP path is REQUIRED for the
   deterministic test seams (a cancel from the test thread between two dispatch passes) and as
   defense for any future multi-driver change (explicitly out of scope — gate item 6).
3. **Already submitted** (kernel-owned): the cancel could not have won `enqueued` (the slot is
   `running`); a `running` cancel records intent only and the op's real CQE (possibly
   `-ECANCELED`) competes for the terminal winner normally.

### 6.3 Stale-CQE scenarios (task §7) — one classification per source

| Source | Mechanism | Disposition |
|---|---|---|
| duplicate CQE (defect/injection) | slot already `backend_ready`/`completion_ready`, or fragment mismatch | dropped (terminal-stored / state / fragment checks) |
| late cancel informational CQE | scratch cancel state already cleared | dropped |
| stale injected event with a reused token | fragment mismatch against the new occupant | dropped by the fragment check |
| generation mismatch due to bug | fragment mismatch | dropped |
| reused user_data (old id space) | new encoding: token = slot+generation, so a reused *slot* has a different generation | dropped by the fragment check; a reused *id* cannot exist (ids eliminated) |

**Honest answer to "can the kernel legally deliver the op CQE after slot release?":** under a
conforming lifecycle, no — a slot is released/reused only after its request reached
`completion_ready`, which requires the CQE to have been reaped first (the backend never releases
a slot while a prepared SQE, kernel request, or future CQE may reference it — ADR Decision 5
"Dispatch"; I12). The generation fragment is therefore defense-in-depth against injected,
duplicated, or defective events, not the primary mechanism. We do not rely on "the kernel
probably won't do that": the lifecycle itself forbids the release, and the fragment check makes
even a defective stale event harmless unless it survives a full 2^(fragment bits) slot-reuse
cycle (see §7.2).

### 6.4 Reap order

`arena_.reap` delivers in terminal-winner order via the ready-ring (Decision 9, review finding
#3) — the same authority Fake/Sync/ThreadPool use. Uring's previous "CQE order" is no longer an
ordering authority; CQEs only *store* terminals.

---

## 7. user_data encoding (task §4.1 — analysis A–E)

### 7.1 Candidate analysis

| Candidate | Stale-CQE safety | Generation wrap/representation | Pointer stability | Capacity bound | CQE decode cost | Cancel target | 64-bit fit | Context in token |
|---|---|---|---|---|---|---|---|---|
| A. raw `SlotIndex` | **none** — a stale CQE acts on the new occupant | — | n/a | bound by arena | trivial | needs separate lookup | fits | no |
| B. `SlotHandle` (slot + full 64-bit generation) | **absolute** (I6 wording) | none | n/a | bound | trivial | direct | **impossible** — 32+64 bits > 64 |
| C. packed RequestKey information (slot + generation fragment) | defense-in-depth; residual window 2^fragment releases | documented bound | n/a | bound | shift/mask | direct | **fits** (see 7.2) | no — ring-local |
| D. `RequestSlot*` pointer | **none** — same ABA as `Completion*`; no generation | — | stable for arena lifetime | bound | trivial | direct | fits (64-bit pointer) | no |
| E. auxiliary stable operation record (token → fixed OpRec array) | depends on record's generation | — | — | bound | indirect | indirect | fits | no |

**Selection: C — packed RequestKey information.** Rationale:

- The ADR explicitly names this shape: "RequestKey -> **encoded/indirect** SQE user_data -> CQE
  -> validate context/slot/generation -> same RequestSlot backend-ready" (ADR "UringAsyncBackend"
  mapping). "Encoded" is sanctioned; "indirect" (a stable record) is the alternative the ADR's
  "no parallel identity reconstruction" clause argues against (Decision 2: one request store).
- A and D are rejected because they carry no generation: the ADR's alternative-A rejection
  ("Completion* as the only identity ... cannot prevent reuse ABA") applies verbatim to
  `RequestSlot*`.
- Full `RequestKey` (B) does not fit in 64 bits; context is unnecessary in the token because
  CQEs arrive on **this** ring (cross-context user_data is structurally impossible; the arena's
  `validate_` re-checks context provenance on every handle).
- The cancel SQE targets the op by its token value (`io_uring_prep_cancel64(sqe, token, 0)` —
  flags=0 cancels the first match of that user_data), so the op token itself is the cancel
  target. No side-band mapping is needed.

### 7.2 Token layout and the generation-fragment bound

```text
64-bit user_data:
  bit 63          : cancel marker (1 = this SQE/CQE belongs to a cancel request)
  bits [F, 63)    : slot index            (F = fragment width)
  bits [0, F)     : generation fragment   (low F bits of the slot's arena Generation)
  F = 64 - 1 - slot_bits,  slot_bits = ceil(log2(request_capacity)), clamped to [1, 62]
```

With the default capacity 64: slot_bits = 6, F = 57. With capacity 65536: slot_bits = 16, F = 47.

**The residual ABA window:** a stale CQE token is accepted only when the slot's current occupant
has the same low-F generation bits — i.e., after 2^F releases of the same slot. The arena's own
generation is 64-bit with fail-fast at max (finding #5); the kernel-visible fragment is a
compromise forced by the 64-bit user_data limit. This is **not** a weakening of I6's primary
mechanism: the lifecycle forbids releasing a kernel-owned slot (§6.3), so in conforming operation
a stale CQE can never observe a reused slot at all. The fragment defeats injected/duplicated/
defective stale events deterministically (they carry a different generation), and its
probabilistic residue (2^47–2^57 reuses) is documented here and must be recorded in the Phase D
compliance gate as the token's explicit bound. A stricter-than-2^48 guarantee would require
either a narrower slot space or a second identity store (rejected by Decision 2).

**Cancel token:** `cancel_token = op_token | (1 << 63)`. It carries the same slot + generation
fragment as the targeted op, so (a) the cancel CQE validates against the same generation, (b)
the marker distinguishes the cancel CQE from the op CQE with the same slot+fragment, and (c) the
cancel targets the op by the op token (`prep_cancel64` argument = op token, its own user_data =
cancel token). This eliminates `cancel_to_op` entirely: the scratch's `cancel_token` + the marker
bit replace it.

### 7.3 Cross-backend consistency

Fake/Sync/ThreadPool already use the arena's `SlotHandle` as the identity; the Uring token is a
kernel-visible projection of that same handle. All internal authorities (cancel, reap, waiter,
reset) use the full `SlotHandle`; the token exists only at the ring boundary.

---

## 8. Cancel model (ADR Decision 11/12 mapping)

| Request state | arena `cancel(h)` result | Uring action | Terminal |
|---|---|---|---|
| `pending` / `enqueued` | `terminal_won` (canceled stored, ready-ring push) | none (dispatch skip / NOP-neutralize per §6.2) | `canceled` — the ONLY tally of `canceled_ops` here |
| `running` (kernel-owned) | `intent_recorded` (cancel_intent_ = true) | submit `IORING_OP_ASYNC_CANCEL` (cancel token, marker bit) | op CQE decides: real result verbatim, or `-ECANCELED` → `record_canceled` (confirmed winner; tally here) |
| `backend_ready` | `already_terminal` | no-op | existing terminal, never overwritten |
| `completion_ready` / released / stale | `not_found` (arena) | no-op | — |
| unsupported op | `not_supported` | no-op | — |

Cancel SQE identity: `user_data = cancel token`; the target is the op token value
(`io_uring_prep_cancel64(sqe, op_token, 0)`). The cancel CQE is **informational only**: it clears
scratch cancel state and never records a terminal (structural X3 exactly-once, preserved from the
current design). `cancel_to_op` is gone.

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
| `pending_sqes` (deque) | per submit/cancel | dispatch ring (fixed array, capacity == request_capacity) + per-slot scratch | no |
| — (new) `RequestArena` slots_/free_slots_ | construction | arena | no |
| — (new) dispatch ring storage | construction | fixed array | no |
| — (new) per-slot SQE scratch | construction | fixed array (`sqe_prepared`, `kernel_owned`, `cancel_pending`, `cancel_token`, `token`) | no |
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

### 11.2 Waking a parked wait_one (control-plane wake)

`wait_one()` currently blocks in `io_uring_wait_cqe` / `io_uring_submit_and_wait` under the legacy
serialized contract. Close/drain must wake a parked `wait_one` as a **one-shot control wake** (0,
no fabricated completion; a future wait parks again — no busy-spin; C2e row 15). Recommended
design: **eventfd + IORING_SETUP_EVENTFD** — the ring signals the eventfd on CQE post; `wait_one`
reaps, then blocks on `poll(eventfd)`; `close_admission()` / `interrupt_backend_waiters()` writes
the eventfd (a control-plane wake, not I/O); the woken `wait_one` performs one final non-blocking
reap and returns 0 if nothing was reaped. This is the standard io_uring wake mechanism and gives
an interruptible park without periodic polling. Alternative considered: `io_uring_wait_cqe_timeout`
with a short timeout (rejected as primary: it is periodic polling, banned as sole progress);
`io_uring_peek_batch` spin (rejected: busy-spin). **This mechanism is Phase D4 work and must be
specified in a frozen design before implementation** — it is the one place where the current
backend's blocking wait contract changes (the context's legacy serialized `wait_one` path may
remain for the pre-D4 state).

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
focused case fails; evidence recorded in `docs/verification/phase-d-*-mutation-evidence.md`).

| # | Defect | Detector | Seam | RED observable |
|---|---|---|---|---|
| 1 | SQE acquisition moved back before acceptance | submit-time pressure probe: SQE fill must NOT happen in submit | `SLUICE_URING_INTERNAL_TESTING` pause at submit | a submit that pauses before enqueue must show zero SQEs written (scratch empty) / submit returns before any `io_uring_get_sqe` |
| 2 | Completion reverse map returns as identity authority | `resolve_completion` scan used; no map | grep-level negative compile + runtime | any parallel map is a compile/authority finding; stale `Completion*` cancel must resolve via the arena only |
| 3 | user_data generation fragment ignored | stale-CQE test: reuse slot, inject old token | CQE injection seam | stale CQE publishes/mutates the new occupant → case fails |
| 4 | stale CQE hits a reused slot | same as 3 + duplicate-CQE test | injection | duplicate CQE after reap/reset mutates the new generation → case fails |
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
 +-> D4  close/drain/destruction (close_admission + eventfd wake + death tests) +
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
| **D3 — "test(async): Uring cancel/generation/stale race matrix (C2b/C2c)"** | Scheme-B pending-cancel window; enqueued-cancel no-execute; running-cancel intent + cancel-SQE; cancel-CQE race matrix (original vs cancel vs -ENOENT, both orders, duplicates); stale-CQE generation; borrow/waiter rows on the real path | mutation detectors 2,3,4,8,9; record flips `uring_c2b_identity_not_implemented`, `uring_c2c_borrow_waiter_not_implemented` |
| **D4 — "refactor(async): Uring close/drain/destruction + KernelIo conformance closure"** | `close_admission()` + admission_mtx_ arbitration; eventfd control wake (frozen design first); quiescent destruction + death tests; manifest flips `uring_c2e_close_drain_not_implemented`; gate lift (verify-backend-conformance.py KernelIo hard-code removal; stub-mode honest classification); real-liburing CI evidence; docs (design doc, gate ledger, divergence-registry DIV-02/DIV-14 updates, api-reference, roadmap status) | mutation detectors 12,13,14; aggregate gate runs Uring through normal evaluation; real-path verdict conforming; roadmap Phase D complete |

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
- Construction-time per-slot SQE scratch array (token, sqe_prepared, kernel_owned,
  cancel_pending, cancel_token).
- Token user_data encoding (§7.2); `io_uring_sqe_set_data64` / `io_uring_cqe_get_data64`.
- Dispatch pass in `poll()`/`wait_one()` (§5.2) — SQE fill, submit, prefix→`mark_running`,
  suffix retained, NOP neutralization, per-slot permanent-failure terminal + submit poison.
- CQE handler (§6.1) — fragment/state validation, `record_terminal`, reap-only publication.
- `cancel()` via `arena_.resolve_completion` + `arena_.cancel` + cancel SQE/token; cancel CQE
  informational.
- P-D0-INF-01 fix: `uring_submit_failure_test` links `sluice_async` (and migrates onto the new
  submit seam) so real-path evidence is producible.
- Descriptor validation (DIV-14 Uring closure): negative fd, null buffer with nonzero length,
  `checked_uring_length`, offset range — synchronous `invalid_argument`, idle Completion, no
  slot/borrow/execution; non-negative closed fd accepted and completed with the real syscall
  error (AGENTS.md §9.1).
- Stub path: unchanged honest synchronous rejection; stub must not claim RequestArena
  conformance.

**Out of scope**
- C2d seams/injection, close_admission/eventfd wake, the full C2b/C2c race matrix, manifest
  flips beyond `uring_capacity_not_implemented`, the gate KernelIo lift, Phase F/G, registered
  buffers/files, multi-driver ring, public submit API changes.

**Files expected to change**
- `include/sluice/async/uring_backend.hpp` (UringConfig, arena members, guarded test seams)
- `src/async/uring_backend.cpp` (full restructure)
- `xmake/experimental.lua` (link fix for `uring_submit_failure_test`; internal-testing seams
  mirroring the ThreadPool pattern)
- `tests/uring_backend_test.cpp`, `tests/uring_submit_failure_test.cpp`,
  `tests/backend_conformance_test.cpp` + `backend_conformance_driver_test.cpp`
  (`conformance_capacity_uring` driver case), new `tests/uring_backend_death_test.cpp`
- `scripts/backend_conformance_manifest.py` (flip `uring_capacity_not_implemented` only after
  evidence; add the capacity driver case record for Uring)
- `docs/architecture/phase-d-compliance-gate.md` (D1 ledger), `docs/design/phase-d-uring-migration.md`
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

**user_data encoding** — §7.2 token (marker + slot + generation fragment); cancel targets the op
token via `prep_cancel64`.

**Rollback rules** — every pre-commit failure rolls back through
`rollback_reserved_or_prepared` (and `rollback_binding_before_accept` when the binding CAS won)
with zero residue; post-commit failures are terminal results, never rejections.

**Tests (D1)**
- Real path (when the host kernel permits): 8-case shared suite; capacity driver case;
  partial-submit suffix/prefix splits (migrated submit hooks); token round-trip; exactly-once
  terminal; reap-only publication; cancel basic; death (non-quiescent destruction).
- Stub path: existing stub subset continues to pass; driver stub case classified honestly.
- Mutation detectors 1, 5, 6, 7, 10, 11 (RED on the single-point mutations).

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

1. **Generation fragment width (P1):** the token carries F=47..57 fragment bits (§7.2). The
   residual ABA window (2^F releases of one slot) must be accepted and recorded in the D1
   compliance gate as an explicit bound, or a future design must revisit capacity encoding
   (e.g., a narrower slot space or a second identity store — rejected by Decision 2). Needs a
   maintainer decision at D1 design freeze.
2. **`submit_poisoned` policy (P1):** after a permanent `io_uring_submit` error, is rejecting
   NEW submissions synchronously (current semantics, preserved) the right admission policy, or
   should the ring be reusable after the suffix is drained? Current recommendation: keep poison
   (a ring that returned a permanent error is not trustworthy); record in the D1 design.
3. **eventfd control wake (P1, D4):** the frozen design for waking a parked `wait_one` must be
   written before D4; the eventfd approach is recommended but its one-shot semantics (drain the
   counter, final reap, no busy-spin) need the C2e-level proof.
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

## 21. Working tree

- New file: `docs/architecture/phase-d-uring-migration-plan.md` (this document).
- Roadmap: Phase D status updated to reflect D0 planning (`PLAN READY / NOT IMPLEMENTED`) — see
  the roadmap status section below; no `COMPLETE` claim.
- No production source, header, build file, manifest, or test file modified.
- Not committed; not pushed (no merge requested).
