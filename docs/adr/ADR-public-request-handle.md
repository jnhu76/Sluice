# ADR: Public RequestHandle — Opaque Accepted-Request Identity

**Status:** Accepted (carried by the Phase-F implementation PR, following the
repository's established ADR pattern where a maintainer-directed implementation
PR carries the Accepted ADR once its design evidence is present; ratified at
human merge — see "Acceptance" below)
**Date:** 2026-08-13
**Scope:** `sluice_async` public request-identity surface — the `RequestHandle`
type, its construction authority, the additive `submit_*_request` entry points,
and the read-only `request_state` identity consumer.
**Baseline:** `e2b9f37` (`master`, PR #105 / Phase F1 merged)
**Governing authority:** `ADR-explicit-io-request-contract` (Accepted), Decision 7
in particular; Constitution AC-2 / AC-13 / AC-14 / AC-15; AGENTS.md §4.1, §4.4,
§9, §10, §12.

## Acceptance

`ADR-explicit-io-request-contract` Decision 7 states verbatim: *"No public
`RequestHandle` is introduced. If callers later need independent request identity
after Completion reset, a public handle requires a separate ADR/API PR."* This is
that ADR. It is promoted from `Proposed` to `Accepted` within the Phase-F
implementation PR that supplies its design, tests, and compliance evidence,
mirroring how `ADR-explicit-io-request-contract` itself was Accepted (carried by
its implementation PR #62, "working-tree change awaiting the user's
review/commit"). As with that precedent, acceptance does not assert the design is
beyond revision — it asserts this is the contract the implementation now
satisfies, and divergence requires a superseding ADR or closeout note.

This ADR is **narrow**: it introduces a public *identity* surface only. It does
NOT change backend reap, terminal-winner, Completion publication, cancellation
winner authority, Scheduler routing, or the wake bridge (Phase G). It does NOT
re-semanticize the synchronous `Reader`/`Writer` contract.

## Context and motivation

After Phase F1 (PR #105), every accepted I/O operation has stable, generation-
bearing identity internally (`RequestKey = (ContextIdentity, SlotIndex,
Generation)`), and the Scheduler consumes identity-bearing reap events. But the
only identity a **public** caller holds is a `Completion<T>&`: the pointer
identity of a caller-owned object. There is no public value that names *which
accepted request* a Completion is currently bound to, survives across a copy, or
can be checked for provenance and staleness independently of the Completion
pointer.

Three compliance rows remain open precisely on this gap:
- **C2b row 4b** — cross-context `RequestKey` authority rejection ("closes when
  the public RequestHandle consumer exists").
- **C2c row 12b** — real public waiter / RequestHandle / Scheduler-registration
  consumer (Scheduler half closed by F1; residual is the public surface).
- **C2c row 14b** — real Scheduler routing-record lifetime / lease
  acknowledgement (Scheduler half closed by F1; residual is the public surface).

This ADR defines the minimal public identity surface that closes those rows
without duplicating the waiter/routing machinery F1 already made public through
`RuntimeTaskContext` (which remains Completion-keyed).

## Decision 1: RequestHandle is opaque identity, NOT ownership

`RequestHandle` is a public, trivially-copyable **value** that names one accepted
request's logical identity. It is the public, library-controlled representation
of the same identity `detail::RequestKey` encodes internally.

A `RequestHandle` MUST NOT become an owner of, or extend the lifetime of:
- the `Completion<T>`;
- the fd or buffer borrow;
- the `RequestSlot`;
- the `RoutingLease` or `WaitRecord`;
- the terminal result.

The ownership model is unchanged (AGENTS.md §9, ADR Decision 8): the caller owns
the Completion; the context/backend owns the RequestSlot; the Scheduler owns the
wait record / Fiber routing. A handle copy outliving a Completion `reset()` does
NOT keep the slot alive — after reset releases the slot (generation++), the stale
handle is inert (Decision 6).

## Decision 2: representation and construction authority (non-forgeable)

`RequestHandle` lives in `sluice::async` (a public installed header; NOT
`detail/`). Its identity components are **private** members; ordinary public code
cannot read or set them. The type has:

- a public default constructor producing an **invalid** handle (`valid() == false`);
- a **private** constructor taking the identity tuple, accessible only to the
  library construction authority (`friend class AsyncBackend;`);
- public `valid()`, copy/move (trivially copyable), equality, and an optional
  debug `describe()` that returns a non-authoritative diagnostic string.

Public callers therefore **cannot** manufacture `(context=123, slot=7,
generation=4)` and use it as authority (Constitution AC-13, AC-14; AGENTS.md §10,
task §10). The construction authority is the backend commit path: the same
`install_binding_for_backend(arena, SlotHandle)` step that privately binds the
`Completion` (ADR Decision 5) is the source of the handle's identity.

Note on the existing `detail::RequestKey`: it is today a trivial aggregate with
public data members; its non-forgeability comes from per-use re-validation inside
the arena (`validate_`), not from construction control. This ADR does NOT change
`detail::RequestKey` (it remains internal). `RequestHandle` adds the
construction-controlled public layer; it does not expose `detail::RequestKey` in
its public API. A caller who separately includes `detail/request_key.hpp` can
mint a `RequestKey`, but cannot turn it into a `RequestHandle` (private ctor) and
has no public entry point that accepts a raw `RequestKey`.

## Decision 3: derived from the Completion's private binding (no second registry)

The handle is derived from the **already-bound** `Completion`, not from a new
registry. At commit the backend calls `install_binding(c, arena, SlotHandle)`,
which stores `detail::RequestArena* release_arena_` and `detail::SlotHandle
bound_slot_` privately on the Completion. From these two the full identity tuple
is recoverable: `context = release_arena_->context()`, plus `bound_slot_`.

`AsyncBackend` (a `friend` of `Completion`) exposes a **non-virtual** helper
`identity_of(Completion<T>&) -> RequestHandle` that reads the binding and
constructs the handle, returning an invalid handle when the Completion has no
arena binding (legacy/external backends — Decision 7). This requires:

- **no per-backend overrides** (all four arena backends already call
  `install_binding`);
- **no slot scan** (the binding is stored on the Completion); and
- **no new global request registry** (AGENTS.md §12, §26: one authoritative
  identity — the `RequestArena` `RequestKey`, reached through the Completion
  binding).

## Decision 4: additive submit API; synchronous reject produces no handle

New public entry points on `AsyncIoContext` (and mirrored on
`RuntimeTaskContext`) are **additive**; the existing `Result<void> submit_*` APIs
are unchanged (ADR Decision 7 preserves current signatures):

```cpp
Result<RequestHandle> submit_read_request(ReadOp, Completion<std::size_t>&);
Result<RequestHandle> submit_write_request(WriteOp, Completion<std::size_t>&);
Result<RequestHandle> submit_sync_data_request(SyncDataOp, Completion<void>&);
Result<RequestHandle> submit_sync_all_request(SyncAllOp, Completion<void>&);
```

Contract (task §12):

- **synchronous rejection ⇒ no valid handle.** A submit-time failure (queue full,
  invalid descriptor, Completion not idle, …) returns the error and produces NO
  handle; no accepted request exists, no borrow begins.
- **successful acceptance ⇒ exactly one valid handle** corresponding to the
  committed `RequestKey`.
- The `submit_*_request` path runs under the same `access_mtx_` serialization as
  `submit_*`; the handle is derived from the just-bound Completion before the
  lock is released, so there is no accept/identity TOCTOU.

## Decision 5: external / non-arena backends are explicit, not fake-conformant

`AsyncBackend` gains a non-pure virtual:

```cpp
virtual bool supports_request_identity() const noexcept { return false; }
```

The default is `false`. `submit_*_request` checks it **before** accepting; if
`false`, it returns `not_supported` and performs **no submission and no
side effect** (the Completion stays idle). This preserves the Decision 4 contract
(success ⇒ valid handle) without leaving an accepted-but-handleless operation.

The four production arena backends (Fake, Sync, ThreadPool, Uring) override
`supports_request_identity()` to return `true`. A custom/external backend that
does not implement the RequestArena identity contract keeps the default and
truthfully reports `not_supported` — it MUST NOT fabricate a `RequestKey` or
return a forgeable pointer-based handle (AGENTS.md §12.1, task §13).

This is source-compatible: existing `AsyncBackend` subclasses compile unchanged
(they simply opt out of identity).

## Decision 6: provenance, generation, and post-reap/post-reset semantics

`RequestHandle` carries context identity and generation. A read-only public
consumer closes the identity rows:

```cpp
enum class RequestHandleState {
    outstanding,        // accepted, not yet terminal (pending/enqueued/running)
    backend_ready,      // terminal won, not yet reaped to Completion-ready
    completion_ready,   // reaped; Completion::ready()
    not_found,          // stale (slot released/reused), wrong context, or unbound
};
Result<RequestHandleState> request_state(const RequestHandle&) const;
```

`request_state` resolves the handle to a `SlotHandle` and runs the arena's
identity validation (`validate_`: slot in range, generation matches, context
matches). It is a **read-only** query; it does not mutate the request, register a
waiter, or cancel I/O. Its outcomes:

- **while the generation is live** in the handle's context: `outstanding` /
  `backend_ready` / `completion_ready`, reflecting the slot's current state;
- **after terminal reap**: `completion_ready`, then `not_found` once the caller
  resets/destroys the Completion (slot release advances generation);
- **after slot reuse**: a stale handle's generation no longer matches →
  `not_found`; it can never observe or target the new occupant;
- **cross-context**: a handle minted in context A, queried through context B, is
  `not_found` (B's arena rejects the foreign context identity) — this is the
  C2b-row-4b proof;
- **invalid handle** (default-constructed or from a rejected submit):
  `not_found`.

`request_state` is the keystone operation that exercises provenance and
generation without duplicating the waiter machinery. It is implemented as a
non-pure virtual on `AsyncBackend` (default `not_supported`); arena backends
override it with a one-line delegation to their arena.

## Decision 7: distinct from waiter identity and I/O cancellation

This ADR deliberately does NOT add handle-keyed waiter registration or I/O
cancellation. Three concepts remain distinct (AGENTS.md §11, task §15):

1. **request identity** — `RequestHandle` (this ADR);
2. **waiter identity** — one registered consumer per request, registered via the
   existing Completion-keyed `RuntimeTaskContext::await_completion` /
   `cancel_waiter` (F1); duplicate waiter is synchronous `invalid_state`;
3. **I/O cancellation** — `RuntimeTaskContext`/backend `cancel(Completion&)`,
   competing under the existing terminal-winner authority; running-cancel stays
   best-effort intent.

A `RequestHandle` does NOT register a waiter, does NOT cancel I/O, and does NOT
cancel a waiter. Waiter cancellation continues to remove ONLY the waiter (the I/O
and borrow are untouched); I/O cancellation continues to compete under the
terminal-winner protocol. This keeps exactly one waiter authority (the arena slot
+ Scheduler record) and avoids a second source of truth (AGENTS.md §26).

## Decision 8: thread safety, capacity, and resource bounds

- `RequestHandle` is a trivially-copyable value (a few scalars); copying one
  allocates nothing and needs no synchronization.
- `submit_*_request` and `request_state` are serialized by `AsyncIoContext`'s
  `access_mtx_`, exactly like `submit_*`/`register_waiter`/`cancel_waiter`. They
  acquire no new lock and introduce no lock-order edge.
- No new container, map, registry, `shared_ptr`, or per-op heap allocation is
  introduced. The single authoritative identity remains the `RequestArena`
  `RequestKey`, reached through the Completion binding (AC-7, AGENTS.md §12).

## Decision 9: lock-order and wake impact

None. `submit_*_request` and `request_state` use only the existing
`access_mtx_` → backend/arena leaf path (G→A→L in the F1 lock table). They do
not touch the Scheduler `global_mtx_`, the `wait_registry_mtx_` leaf, or any wake
source. The slot-lifecycle domain remains a leaf: `request_state` reads slot
state under the arena mutex and returns; it calls no Scheduler, sink, or user
code. No wake obligation changes (Phase G is untouched).

## Decision 10: shutdown and lifetime

`RequestHandle` is a value; it imposes no shutdown obligation. Destroying a
context with outstanding accepted work remains a contract violation
(AGENTS.md §14) regardless of whether handles exist. A handle does not pin the
context or the slot; after the accepted request is reaped and the Completion
reset, the handle is simply stale (`request_state == not_found`).

## Rejected alternatives

- **B — `cancel_request(RequestHandle)` (I/O cancel by identity).** Rejected for
  v1: the existing `cancel(Completion&)` already cancels by the accepted
  request's identity (the Completion is bound to exactly one slot), and ADR
  Decision 7 explicitly permits a Completion-keyed compatibility cancel API. A
  handle-keyed cancel adds a mutating entry point with no new capability. It can
  be added by a later ADR if a use case appears.
- **C — handle-keyed waiter registration/cancellation.** Rejected: it would
  duplicate F1's Completion-keyed waiter path behind a handle facade and risks a
  second waiter authority unless carefully unified. F1 already made waiter
  register/cancel public via `RuntimeTaskContext`; a handle facade adds surface
  without new semantics. (Task §9: the handle is identity, not ownership.)
- **D — change `submit_*` to return `Result<RequestHandle>`.** Rejected: it
  breaks source compatibility for every caller and every `AsyncBackend` subclass
  (AGENTS.md §16.1, task §12–§13). The additive `submit_*_request` family is
  strictly safer.
- **E — public raw `RequestKey`.** Rejected (ADR Decision 7, AC-14): it would
  expose the internal identity tuple as a forgeable public aggregate and
  collapse the construction-controlled authority this ADR requires.

## Conformance and evidence

- Closes **C2b row 4b** (cross-context identity rejection): a foreign-context
  handle yields `request_state == not_found`; provenance is enforced by the
  arena's `validate_` context check.
- Closes **C2c row 12b** (public RequestHandle consumer): `submit_*_request` +
  `request_state` are the public identity consumer.
- Closes **C2c row 14b** (routing-record lifetime): the handle's generation
  tracks the real slot lifetime; after reset/reuse a stale handle is `not_found`,
  matching the F1 Scheduler record lifetime.
- Evidence: `tests/request_handle_*_test.cpp` (provenance, stale-generation,
  cross-context, post-reap, post-reset), a public-only acceptance test, and
  negative-compile authority gates (no forging the handle/key/lease). Full
  command-backed evidence is recorded in the Phase-F closeout compliance gate.
