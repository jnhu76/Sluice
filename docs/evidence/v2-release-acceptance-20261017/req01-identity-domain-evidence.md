# REQ-01 request identity domain / exhaustion evidence (release gate RG4)

Requirement: root §8 REQ-01 — no aliasing within the valid identity domain;
explicit failure before wrap. Ledger gap closed: G2 row "REQ-01 identity
domain / exhaustion" (formerly: core and ThreadPool admission paths do not
enforce the domain exhaustively; io_uring only).

## Implementation facts under proof (unchanged by this campaign)

- Context identity: `src/async/detail/context_identity.cpp` — a global
  monotone cursor; identities are claimed by CAS against the live context
  registry; `0` is never a live identity; the cursor yields `UINT64_MAX`
  once and then latches to `0` (exhausted), so no identity value is ever
  minted twice.
- Slot generation: `src/async/detail/request_core.cpp` — `release_slot_`
  retires the slot at generation max; `reserve` reports `exhausted` only
  when every slot is permanently retired; reserve/accept is serialized by
  the core mutex, so a retired slot can never be re-accepted.
- Public projection: `RequestId{context, slot, generation}`; both backends
  map admission-exhausted to the explicit `would_block` rejection and
  admission-closed to `invalid_state` (request_error_domain tests).

## Evidence added by this campaign (commit `a0874ae2`, tests/request_core_ownership_test.cpp)

| Failure mode | Named oracle | Backend |
|---|---|---|
| Concurrent identity allocation repeats an identity under a racing boundary cursor (3 domain values left) | `concurrent_identity_allocation_never_repeats` — 8 threads construct contexts concurrently while a boundary cursor is raced; every loser observes the explicit exhausted outcome; the latch holds after exhaustion | ThreadPool seam, boundary-injected cursor |
| Destroy/reconstruct core-address reuse adopts a stale identity | `context_address_reuse_never_adopts_a_stale_identity` — 8 rounds of same-sized destroy/reconstruct recording each round's context-core heap address; every round unconditionally enforces successor-identity distinctness, empty lookup and `not_found` cancel for all prior identities; the skip note fires only if the allocator never reused a core address (matrix runs: 0 skipped) | ThreadPool, real allocator |
| Submission into a retired slot domain before accept | `retired_slot_domain_rejects_submissions_before_accept` / `uring_retired_slot_domain_rejects_submissions_before_accept` — generation forced to `UINT64_MAX`, the slots genuinely retired through the public path, then repeated submits are rejected with the explicit exhausted-mapped `would_block` verdict, the retired domain holds no live work, admission stays open, and each stale id resolves nowhere and cancels `not_found` | ThreadPool and io_uring (real ring) |

Boundary injection statement: `claim_context_identity` is production
surface; the injected boundary cursor is a test-owned `std::atomic` passed
to it, and the generation forcing is `SLUICE_ASYNC_INTERNAL_TESTING`
seam-surface only. No 2^64-scale allocation is attempted, and no
production identity registry was added — the evidence exercises the
production cursor, slot table and admission paths as they are.

Pre-existing oracle coverage reused by this record:
`generation_exhaustion_retires_slot` and
`reuse_advances_generation_without_aliasing` (request_core_protocol_test),
the identity-domain rows of both request_error_domain variants, and the
RequestCore TLA+ model's liveness-per-identity obligations (L1/L5).

## What was NOT changed

No production code changed for RG4: the implementation already satisfied
the requirement; the gap was evidential. The three new oracles pin the
failure modes named above against the real allocation and admission paths
on both backends, and run inside `request_core_ownership_test` /
`request_core_ownership_uring_test` in every matrix cell (56/74-test
suites).
