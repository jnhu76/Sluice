# TAX-0 Router Fix Shootout — Prior Art (#255, phase 1 of #250 T0-U-ROUTER)

Purpose: inspect representative io_uring completion-correlation implementations
BEFORE freezing the candidate list for the router fix shootout. This report
records what each system actually does (with source provenance) and compares
CONTRACTS. It never argues "system X does Y, therefore Sluice should do Y".

Provenance: sources fetched from the upstream default branches on 2026-08-31
(blob SHA recorded where the API provided it); liburing semantics from the
official liburing docs via the context7 index. Line numbers refer to the
fetched snapshots.

## 1. The resolved question

Every io_uring consumer must map a CQE's 64-bit `user_data` back to whatever
application state owns the in-flight request. The kernel contract (liburing
docs, `io_uring_enter(2)`): `user_data` is OPAQUE to the kernel — submitted
value copied verbatim into the matching CQE; exactly one CQE per successfully
submitted SQE; `IORING_OP_ASYNC_CANCEL` matches pending requests by user_data
VALUE equality. Everything else — lookup structure, reuse policy, stale-CQE
defense — is application policy. The surveyed systems implement three families:

| family | systems | mechanism |
| --- | --- | --- |
| raw pointer | fio, Seastar, Boost.Asio | user_data IS a pointer to an object that outlives the in-flight request |
| recycled index | Tokio (mainline driver), tokio-uring, Monoio, Glommio | user_data IS a slab/free-list index; O(1) array-style lookup; slot kept occupied until its own CQE is consumed |
| no-wrap unique key | **Sluice (current production)** | user_data is a never-reused 64-bit cookie; stale CQEs cannot alias anything; cost = cookie→entry lookup per CQE |

## 2. Per-system records

### 2.1 liburing (helper library, not a runtime)

- user_data payload: anything (u64 via `io_uring_sqe_set_data64`, pointer via
  `io_uring_sqe_set_data`). Documented idiom: store a heap context POINTER at
  submit, `io_uring_cqe_get_data` at completion.
- CQE resolution: none — the library returns the value; the application owns
  all correlation.
- lookup complexity: application-defined.
- identity reuse: application-defined.
- stale completion behavior: undefined by the library; kernel guarantees no
  duplicate/unsolicited CQEs for a submitted SQE, but a cancel that loses can
  still leave the ORIGINAL op's CQE to arrive later — apps must tolerate
  completions for requests they have logically abandoned.
- generation/ABA: none provided.
- ownership/lifetime: application-defined.
- steady-state allocation: none by the library.
- bounded memory: ring-sized by the application.

### 2.2 fio `engines/io_uring.c` (master, 2026-08-31)

- user_data payload: raw `io_u` POINTER — `sqe->user_data = (unsigned long) io_u;`
  (line 813/879).
- CQE resolution: direct cast — `io_u = (struct io_u *) (uintptr_t) cqe->user_data;`
  (`fio_ioring_event`, line 966).
- lookup complexity: O(1) (pointer dereference).
- identity reuse: `io_u` structs are preallocated per-thread and recycled, but
  an io_u is not resubmitted until its previous CQE was reaped (fio's io_u
  flight discipline), so the pointer never aliases an in-flight request.
- stale completion behavior: none expected; fio issues no cancels that orphan
  CQEs mid-flight; a CQE for an already-reaped io_u would silently corrupt the
  recycled io_u — accepted as impossible under fio's discipline.
- generation/ABA: none.
- ownership/lifetime: io_u owned by the io_u queue; pointer must stay valid
  from submit until its CQE is fetched.
- steady-state allocation: zero (preallocated io_u array).
- bounded memory: `iod->io_u` array = iodepth-bounded.

### 2.3 Tokio mainline io_uring driver (`tokio/src/runtime/io/driver/uring.rs`, master, 2026-08-31)

- user_data payload: `Slab<Lifecycle>` INDEX — `let index = ctx.ops.insert(...);`
  then `entry.user_data(index as u64)`.
- CQE resolution: `let idx = cqe.user_data() as usize; match ops.get_mut(idx) { ... }`
  — O(1) slab lookup; `None => panic!("no op at index {idx}")`.
- identity reuse: slab indices ARE reused after `ops.remove(idx)`; removal
  happens only while consuming that op's own CQE (or its cancel CQE), so a
  stale CQE for a removed index is assumed impossible; a reused index hit by
  a late CQE would alias the new occupant — no generation bits.
- stale completion behavior: unknown index = panic; recycled-index aliasing is
  closed by discipline (slot never freed while its CQE may still arrive), not
  by encoding.
- cancel: separate lifecycle (`Lifecycle::Cancelled`) keeps the slot occupied
  until the completion arrives.
- ownership/lifetime: waker + lifecycle live in the slab until completion.
- steady-state allocation: slab growth amortized; not fixed-capacity.
- bounded memory: bounded by concurrent in-flight ops (slab grows to peak).

### 2.4 tokio-uring (`src/runtime/driver/mod.rs`, blob `352b8710`, master, 2026-08-31)

- user_data payload: `Slab<op::Lifecycle>` index — `let index = self.ops.insert();`
  `sqe.user_data(index as _)`.
- CQE resolution: `let index = cqe.user_data() as _; self.ops.complete(index, cqe);`
  — O(1).
- identity reuse: slab recycling, same discipline as Tokio mainline; test
  `op_stays_in_slab_on_drop` pins the "entry survives until its CQE" rule.
- reserved values: cancel SQEs carry `user_data == u64::MAX` (sentinel), and
  the driver SKIPS that CQE (informational-only cancel CQE — same shape as
  Sluice's tagged control CQEs, different encoding).
- generation/ABA: none (discipline-based).
- steady-state allocation: slab with initial capacity 64; amortized growth.
- bounded memory: bounded by in-flight ops.

### 2.5 Monoio (`monoio/src/driver/uring/mod.rs`, master, 2026-08-31)

- user_data payload: slab index (`OpAble::...user_data(op.index as _)`), PLUS
  a reserved control range: `EVENTFD_USERDATA`, `POLLER_USERDATA`,
  `TIMEOUT_USERDATA`, and everything `>= MIN_REVERSED_USERDATA` is ignored —
  a reserved-subrange scheme for control-plane SQEs.
- CQE resolution: `self.ops.complete(index as _, ...)` — O(1) slab access
  (`unsafe`, `unwrap_unchecked` — no unknown-index check in this path).
- cancel: `AsyncCancel::new(index as u64).user_data(u64::MAX)` — cancels
  target the in-flight request BY its user_data value; cancel CQEs carry the
  `u64::MAX` sentinel and are informational.
- identity reuse: slab recycling; slot removed only after its own completion
  (or via drop_op which re-checks the lifecycle).
- generation/ABA: none.
- steady-state allocation: zero per-op (slab of preconstructed lifecycle cells).
- bounded memory: bounded by in-flight ops.

### 2.6 Glommio (`glommio/src/sys/uring.rs`, master, 2026-08-31)

- user_data payload: `SourceId` = index into `SourceMap = FreeList<...>` PLUS 1
  — `to_user_data(id) = id.to_raw() + 1`; `from_user_data(u) = u - 1`. O(1)
  free-list resolution.
- reserved values: `user_data == 0` is the control sentinel — poll-remove and
  cancel SQEs are submitted with `user_data = 0` and their CQEs are skipped
  (`if value.user_data() == 0 { continue; }`).
- cancel: `UringOpDescriptor::Cancel(to_user_data(id))` — kernel matches the
  victim by its user_data value.
- identity reuse: FreeList indices recycle after the source is consumed;
  consumption happens on the source's own CQE; late CQEs for a consumed source
  find the free-list slot vacant.
- generation/ABA: none.
- steady-state allocation: none per-op (freelist of pinned Rc cells).
- bounded memory: bounded by live sources.

### 2.7 Seastar (`src/core/reactor_backend.cc`, master, 2026-08-31)

- user_data payload: raw POINTER to a `kernel_completion` —
  `::io_uring_sqe_set_data(sqe, static_cast<kernel_completion*>(desc));`
- CQE resolution: `auto completion = reinterpret_cast<kernel_completion*>(cqe->user_data);`
  `completion->complete_with(cqe->res);` — O(1).
- identity reuse: completion objects are heap-allocated per request
  (`desc.release()` into the io sink) and destroyed by the completion chain —
  the object provably outlives its in-flight request because its owner keeps
  it until `complete_with` runs.
- stale completion behavior: none expected; ownership-until-completion makes a
  stale CQE impossible by construction (nothing is freed early).
- generation/ABA: none (ownership-based).
- steady-state allocation: one small completion object per in-flight request
  (pool-fed; this is a throughput runtime, not a no-allocation runtime).
- bounded memory: io_queue depth bounds bound the in-flight set.

### 2.8 Boost.Asio (`include/asio/detail/{io_uring_service.hpp,impl/io_uring_service.ipp}`, master, 2026-08-31)

- user_data payload: raw pointer to the per-descriptor, per-op-type `io_queue`
  embedded in a pooled `io_object` — `::io_uring_sqe_set_data(sqe,
  &io_obj->queues_[op_type]);`. The QUEUE is the correlation authority; the
  individual `io_uring_operation` is found by draining the queue.
- CQE resolution: `get_user_data<io_queue>(cqe)->perform_io(result)` — O(1) to
  the queue, then queue pop.
- reserved values: `get_sqe` pre-sets `user_data = 0` (null sentinel) for every
  obtained SQE so an unfilled SQE can never execute with a stale identity.
- identity reuse: `io_object`s come from a service pool and are reused only
  after descriptor deregistration (no outstanding CQEs reference them).
- generation/ABA: none (stable-object discipline).
- steady-state allocation: io_object pool allocation amortized; ops queued
  allocation-light.
- bounded memory: bounded by registered descriptors + op queues.

## 3. Contract comparison against the Sluice identity contract

Sluice current production properties (bound at base `9bbe3a24`, EXP-U0 §4):

- P1 operation cookie is a no-wrap, NEVER-reused 64-bit value (fail-fast at
  the control-tag boundary) — kernel-visible identity is unique for the
  backend lifetime;
- P2 a stale CQE (cookie whose router entry retired) resolves to NOTHING —
  the ABA window that existed under the old `router_slot+1` encoding is
  closed by the key's uniqueness, not by lifetime discipline;
- P3 the router entry identity and RequestArena generation validation remain
  two coherent layers (router routes by cookie; the arena re-validates
  slot+generation at terminal recording);
- P4 tagged control identity uses the reserved high bit; cancel CQEs are
  informational and can never publish terminals;
- P5 bounded request capacity with construction-time fixed metadata and zero
  steady-state allocation;
- P6 reap/publication authority and terminal-winner rules are external to the
  router (the router is transport metadata only).

Comparison (contract-first, not "X does Y"):

1. Every surveyed runtime resolves CQEs in O(1) via pointer or recycled
   index; none pays a capacity-proportional scan. Sluice's linear scan is a
   deliberate outlier purchased for P1/P2 — the question for the shootout is
   how to buy O(1)-family performance WITHOUT weakening P1–P5.
2. The recycled-index family (Tokio/tokio-uring/Monoio/Glommio) achieves O(1)
   by making slot lifetime cover the CQE window: the slot is never freed
   while its completion may still arrive. That is a REAL design pattern, but
   adopting it verbatim would change the Sluice contract: Sluice retires the
   router entry at reap (release does not wait on kernel progress), accepts
   capacity-driven slot recycling, and its cancel paths can produce CQEs the
   logical request no longer waits for. A recycled index under Sluice's
   retirement timing would REOPEN stale-CQE aliasing — see R4 audit.
3. The pointer family (fio/Seastar/Asio) achieves O(1) through
   ownership-until-completion. Sluice's borrowed-buffer + caller-owned
   Completion model could support a pointer-valued user_data only if the
   pointed object's address stability and lifetime covered arbitrary kernel
   delay after cancel/teardown — which conflicts with P5's fixed bounded
   metadata and with quiescent destruction. Pointer-only user_data also
   abandons the integer user_data API the backend documents on every
   liburing target.
4. Control-plane sentinels are universal: glommio reserves 0, tokio-uring and
   Monoio reserve u64::MAX, Monoio reserves a whole subrange, Asio pre-sets 0.
   Sluice's high-bit control tag is the same idea; any candidate must keep a
   disjoint control-tag space intact (P4).
5. None of the surveyed systems defends against arbitrary stale CQEs; they
   exclude them by construction/discipline. Sluice is STRICTER: it demands
   that a stale CQE be *safely droppable* regardless of when it arrives
   (double CQE tolerance is tested). The prior art therefore does NOT contain
   a drop-in fix — it contains the menu of lookup structures whose adoption
   must be re-derived under the no-wrap-cookie contract:
   - a bounded open-addressed cookie→index table (R3) gives the index
     family's O(1) lookup while KEEPING the no-wrap cookie as kernel-visible
     identity — P1/P2/P3 untouched, table erased at retirement;
   - placement (which physical array slots the live set occupies) is a
     free variable nobody else needs because nobody else linearly scans —
     R1/R2 exploit it under the existing scan representation.

## 4. Papers / additional sources

No surveyed runtime paper materially discusses completion-correlation tables
beyond the implementations above; the io_uring kernel contract cited is the
liburing official documentation set (`submission-api.md`, `completion-api.md`,
`types.md` autodocs) and `io_uring_prep_cancel64`'s user_data-matching
semantics in `src/include/liburing.h`. The design-relevant literature fact is
already encoded in AGENTS.md §12: fixed-capacity, allocation-free request
tables are the accepted resource-boundary idiom for this project.
