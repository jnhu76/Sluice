# Phase F Closeout Compliance Gate — Scheduler/Batch consume identity-bearing reap

> **Archived 2026-08-25 (issue #167 Step 5).** Moved from
> `docs/architecture/`; classification at move: EVIDENCE (point-in-time
> compliance/gate record). Body preserved as-written; see
> `docs/history/README.md`.

**Phase:** F (issue #98) — F1 + F2 + F3. **Status: COMPLETE.**
**Baseline:** `e2b9f37` (`master`, PR #105 / Phase F1 merged).
**Closeout lineage (semantic commits on `feat/phase-f-remaining`, this PR):**
F2 `d096f1f`; F3 docs `1f1e178`; F3 impl `95e3a2a`; F3 corrective `0e7367b`
(c2e real-liburing target lists `request_handle.cpp`); F3 authority-seal
corrective `c593ce4` (private seam + friend `AsyncIoContext`); F4 closeout +
evidence reconciliation (`fc359a1`, `9948776`, `433c4a7`, `3fb0fea`). The
current PR head is authoritative via Git history; subsequent review-driven
docs/gate-only cleanups are not re-enumerated here, to avoid a self-referential
SHA list that goes stale on every cleanup commit.
**Authority:** `ADR-explicit-io-request-contract` (Decisions 7, 9, 10);
`ADR-public-request-handle` (Accepted, this PR); Constitution AC-2 / AC-7 /
AC-13 / AC-14 / AC-15; AGENTS.md §4, §8, §10, §12, §16.

This gate records that Phase F — making the Scheduler and Batch consume
identity-bearing reap, and exposing a public request-identity surface — is
complete. The ONLY remaining identity-related work is the backend-ready wake
bridge, which is Phase G (separate, untouched).

## Sub-phase summary

- **F1 (PR #105):** production Scheduler consumes the by-value
  `ReadyEvent{RequestKey, token, lease}` via the Scheduler-owned
  `ReadyRoutingSink`; the O(N) `Completion::ready()` re-scan is removed from the
  arena path; duplicate-waiter `invalid_state` + cross-context provenance on the
  Scheduler side; `cancel_waiter` removes only the waiter. Gate:
  [phase-f1-compliance-gate](phase-f1-compliance-gate.md) (COMPLETE).
- **F2 (commit `d096f1f`):** `BatchResultOrigin` (`rejected` vs
  `accepted_and_completed`) on `BatchResult` — ADR Decision 9 (Batch consumes
  outcome origin explicitly, orthogonal to success/error).
- **F3 (commits `1f1e178`, `95e3a2a`, `0e7367b`, + seal corrective):** public
  `RequestHandle` identity surface — `ADR-public-request-handle`. The
  construction/resolution seam is sealed class-level (private
  `identity_of` / `request_handle_state` / private virtual
  `resolve_identity_state`, sole friend `AsyncIoContext`; overrides private;
  negative-compile 9/9). Gate:
  [phase-f3-compliance-gate](phase-f3-compliance-gate.md) (COMPLETE).

## Gate 0 — scope and authority

| Item | Value |
|---|---|
| F1 capability | Scheduler wait registration + identity-bearing wake routing |
| F2 capability | Batch outcome origin |
| F3 capability | public accepted-request identity |
| Classification | additive public API + identity consumer (no ownership/lifecycle authority change) |
| Governing ADRs | request-contract (7/9/10); public-request-handle |
| Constitution | AC-2, AC-7, AC-13, AC-14, AC-15 |

## Gate 1 — state machine / ownership

- No new lifecycle authority. `RequestKey = (ContextIdentity, SlotIndex,
  Generation)` remains the single authoritative identity (AC-14, AGENTS.md §26);
  the RequestArena/RequestSlot lifecycle is unchanged and is the sole identity
  authority.
- `RequestHandle` is identity, NOT ownership (ADR-public-request-handle Decision
  1): a value naming one tuple; copying pins nothing. `BatchResultOrigin`
  records admission history, not ownership.
- Handle lifecycle (via `request_state`): `outstanding` → `backend_ready` →
  `completion_ready` → `not_found` (after reset/reuse); cross-context or
  stale-generation → `not_found`.

## Gate 2 — resources / capacity

- `RequestHandle` is a trivially-copyable value; `BatchResultOrigin` is one byte.
  Zero allocation on any F2/F3 path. No new map, registry, `shared_ptr`, or
  per-op heap object (AC-7). F1's bounded `WaitRecord` pool (capacity
  `wait_capacity`, default 256) is unchanged; F2/F3 add no pool.
- The handle is derived from the Completion's existing private binding
  (`release_arena_` + `bound_slot_`): no slot scan, no second registry.

## Gate 3 — progress / wake / lock-order

- **No wake impact, no new lock edge** (Phase G untouched). `submit_*_request`
  and `request_state` use only `access_mtx_` → backend/arena leaf (G→A→L, the F1
  table). They touch no Scheduler lock, no `wait_registry_mtx_`, no wake source.
  The slot-lifecycle domain stays a leaf: `request_state` reads slot state under
  the arena mutex and returns; it calls no Scheduler, sink, or user code.
- F1's wake obligations (register-vs-reap, cancel-vs-reap, pin acknowledgement)
  are unchanged by F2/F3.

## Gate 4 — evidence (executed)

| Gate | Command | Result |
|---|---|---|
| Clang Debug | `xmake f -c -m debug --toolchain=clang -y; xmake build -g test; xmake test` | 162/162 PASS, 0 fail |
| Clang Release | same with `-m release` | 162/162 PASS, 0 fail |
| TSan | `-m tsan`; `xmake run -g test` | ALL PASS, 0 warnings |
| ASan/UBSan | `xmake f -c -m asanubsan --toolchain=clang -y; xmake build -g test; xmake run -g test` | ALL TESTS PASSED, 0 errors (run 2026-08-14) |
| Real liburing | `xmake f -c -m debug --toolchain=clang --with-liburing=true -y; xmake build -g test; xmake test` | 164/164 PASS (executed locally, liburing 2.14; matches committed `0e7367b`. Earlier sandbox mode=stub limitation obsolete. GitHub CI does not run real liburing — real-mode authority is the local + committed run, not CI) |
| Backend conformance | `scripts/verify-backend-conformance.py` + manifest self-test | RESULT: PASS — manifest self-test 209/209 OK; Fake ELIGIBLE, ThreadPool ELIGIBLE, Uring stub INCOMPLETE (manifest-declared); 47 PASS + 10 expected-stub INCOMPLETE rows; external probe PASS (quiet full run) |
| Negative compile | `scripts/verify-request-handle-authority-negative-compile.sh` | 9/9 PASS |
| Public acceptance | `tests/request_handle_test.cpp` (public headers only) | 6/6 PASS |
| Docs/arch | `check-doc-links.py`, `verify-architecture-docs.py`, `pre-push.sh` | green |

## Closed compliance rows

- **C2b row 4b** — cross-context `RequestKey` authority rejection: CLOSED by F3
  (`f3_cross_context_handle_is_not_found`).
- **C2c row 12b** — public waiter / RequestHandle consumer: CLOSED (F1 Scheduler
  half + F3 public surface).
- **C2c row 14b** — real routing-record lifetime: CLOSED (F1 + F3
  `request_state` stale-after-reuse → `f3_stale_generation_after_reuse_is_not_found`).

## Invariants (F-1 … F-25)

The Phase-F invariants (issue #98 checklist) hold: stable generation-bearing
identity (F-1); rejected ≠ accepted terminal error (F-2/F-3, F2 origin + tests);
explicit origin (F-4); non-forgeable handle (F-5, negative-compile gate);
cross-context isolation (F-6); stale-generation safety (F-7); handle is identity
not ownership (F-8); Completion sole result authority (F-9); reap sole
publication (F-10); backend-ready ≠ completion-ready (F-11); one waiter max +
duplicate `invalid_state` (F-12/F-13, F1); waiter-cancel ≠ I/O cancel (F-14,
F1); exactly-once routing + lease acknowledgement (F-16/F-17, F1); bounded
WaitRecord pool (F-18, F1); no new unbounded allocation (F-19); generation
advances before stale identity acts (F-20); shutdown-safe routing authority
(F-21, F1); explicit external-backend behavior — `not_supported`, never fake
(F-22); public acceptance needs no internal header (F-23); no authoritative doc
describes Phase F as unfinished (F-24, this gate + roadmap/divergence/as-built);
Phase G separate and untouched (F-25).

## Non-goals (Phase G and later)

backend-ready → Scheduler wake bridge; 2ms MIXED-WAKE backstop decision;
DIV-04/DIV-05 reclassification (all Phase G); handle-keyed waiter registration
or I/O-cancel-by-handle (ADR alternatives B/C, deferred); public raw
`RequestKey` (rejected).
