# Phase F3 Compliance Gate — Public RequestHandle

**Phase:** F3 (issue #98) — public accepted-request identity surface.
**Status:** PENDING — Gate 4 evidence to be filled from executed runs.
**Authority:** `docs/adr/ADR-public-request-handle.md` (Accepted, this PR);
`ADR-explicit-io-request-contract` Decision 7; Constitution AC-2 / AC-13 / AC-14
/ AC-15; AGENTS.md §4.1, §8, §10, §12, §16.1.
**Generic gate:** `docs/architecture/design-compliance-gate.md`.

This gate covers the change class "public async API + RequestKey lifecycle
identity consumer" (AGENTS.md §8). It is design-first: the ADR above is the
contract; this gate records the as-built evidence.

## Gate 0 — Architecture classification

- **Affected capability:** public request-identity surface — `RequestHandle`
  type, additive `submit_*_request`, read-only `request_state`.
- **Layer:** `sluice::async` public headers (`request_handle.hpp`,
  `async_io_context.hpp`, `application_runtime.hpp`) + `AsyncBackend`
  non-pure/non-virtual additions + the four arena backends' thin overrides.
- **Classification:** additive public API; identity-only (no ownership/lifecycle
  authority change).
- **Governing ADR:** `ADR-public-request-handle.md`.
- **Constitution rules:** AC-2 (stable identity), AC-13 (unforgeable publication
  / state-checked caller lifecycle), AC-14 (provenance + generation), AC-15
  (identity preserved across reap, no scanning). AC-7 (bounded / caller-owned —
  handle is a value, no registry).

## Gate 1 — Ownership and state machine

- **No new lifecycle authority.** `RequestHandle` is a value naming the existing
  `RequestKey`. The RequestArena/RequestSlot lifecycle (reserve→…→free,
  generation++) is unchanged and remains the sole identity authority
  (AGENTS.md §4.1, §26).
- **Construction state machine:** default ctor → invalid; private identity ctor
  (friend `AsyncBackend`) → valid bound to one `(context, slot, generation)`.
- **Handle validity over the request lifecycle (observed via `request_state`):**
  `outstanding` → `backend_ready` → `completion_ready` → `not_found` (after
  reset/reuse). Cross-context or stale-generation → `not_found`.

## Gate 2 — Resource and failure model

- `RequestHandle` is trivially copyable (a few scalars); zero allocation on any
  path. No map, registry, `shared_ptr`, or per-op heap object is introduced
  (AC-7). The single authoritative identity remains the arena `RequestKey`,
  reached through the Completion binding (`release_arena_` + `bound_slot_`).
- **Failure semantics:** synchronous submit rejection ⇒ error, no handle, no
  accepted request; `supports_request_identity() == false` ⇒ `not_supported`
  with no side effect; cross-context/stale handle ⇒ `request_state ==
  not_found`. No new fail-fast path; existing Completion fail-fast is unchanged.

## Gate 3 — Progress and wake model

- **No wake impact.** `submit_*_request` and `request_state` use only
  `access_mtx_` → backend/arena leaf (G→A→L, F1 table). They touch no Scheduler
  lock, no `wait_registry_mtx_`, no wake source. The slot-lifecycle domain stays
  a leaf: `request_state` reads slot state under the arena mutex and returns; it
  calls no Scheduler, sink, or user code. Phase G (wake bridge) is untouched.

## Gate 4 — Evidence (PENDING until executed)

- [x] Clang Debug full suite green — 162/162 PASS, 0 fail
  (`xmake f -c -m debug --toolchain=clang -y; xmake build -g test; xmake test`).
- [x] Clang Release full suite green — 162/162 PASS, 0 fail (§16.1 public-API
  change-class).
- [x] TSan green — ALL PASS, 0 warnings (`submit_*_request`/`request_state`
  serialized under `access_mtx_`; cross-context + reset/reuse exercised; §16.3).
- [x] ASan/UBSan green — ALL PASS, 0 errors (stale handle, context destruction,
  Completion reset, slot reuse; §16.2).
- [ ] Real liburing: `supports_request_identity()` + `request_state` on the
  Uring backend, `mode=real` (run below; stub inherits not_supported by design).
- [ ] Backend conformance manifest self-test + `scripts/verify-backend-
  conformance.py` (run below).
- [x] Negative-compile authority gate: `scripts/verify-request-handle-authority-
  negative-compile.sh` — 5/5 PASS (no forging the handle / no public setter for
  context/slot/generation).
- [x] Public-only acceptance test: `tests/request_handle_test.cpp` — 6/6 PASS
  (no `src/`, no `detail/`, no `SLUICE_ASYNC_INTERNAL_TESTING`).
- [x] `scripts/check-doc-links.py` (PASS) +
  `scripts/verify-architecture-docs.py` (PASS); pre-push run below.
- [x] C2b row 4b / C2c rows 12b + 14b closed with implementation + test pointers
  (see [phase-f-compliance-gate](phase-f-compliance-gate.md) + the C2b/C2c gates).

## Non-goals (Phase G and later)

- backend-ready → Scheduler wake bridge; 2ms MIXED-WAKE backstop; DIV-04/DIV-05
  reclassification (all Phase G);
- handle-keyed waiter registration / I/O cancel-by-handle (ADR alternatives B/C,
  deferred);
- public raw `RequestKey` (rejected, ADR alternative E).
