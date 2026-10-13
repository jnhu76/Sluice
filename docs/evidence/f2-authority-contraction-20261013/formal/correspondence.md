# F2 TLA+ correspondence note

Gate: `scripts/verify_tla.sh` against the pinned TLA+ tools release v1.7.4
(SHA-256 `936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88`,
verified by the script before any model runs). The gate log is
`tla-verify.log` in this directory and its hash is recorded in
`../gates.json`; `../gates.json` also records the status the log supports.

## What this slice changed relative to the models

F2 removed the caller-owned `Completion<T>` authority and the dead
`RequestArena` vocabulary and made `RequestCore` the single settlement
authority. It did **not** add or remove an action in any model, and it changed no
model file. The correspondence table therefore maps the same actions to the same
C++ transitions as before, with the public spellings that moved:

| Model | Actions | C++ transitions after F2 |
|---|---|---|
| `RequestCore.tla` | reserve / accept / rollback | `RequestCore::reserve/accept/rollback` (unchanged) |
| | terminal arbitration (`offer_terminal` candidates) | `RequestCore::offer_terminal` (unchanged) |
| | publication begin / seal / complete | `begin_publication`/`complete_publication` (unchanged) |
| | public consume / discard | `consume_public_result` / `discard_public_result`, reached through `Request<T>::take_result()` / `discard()` instead of a caller-owned `Completion<T>` |
| | cancel intent and win/lose | `RequestCore::cancel` (unchanged) |
| | reclaim predicate | `reclaimable_` (unchanged; still requires published, zero execution and control references, no publication in flight, released binding, retired observer) |
| `ObserverCore.tla` | attach before/after publication, claim, retire, delivery, reclaim interaction | `register_observer` / `claim_observer_delivery` / `retire_delivered_episodes` (unchanged); the attach argument is a `RequestId` instead of a `Completion&` |
| `ShutdownCore.tla` | admission close, execution close, notification retire, destruction | `close_admission`, `shutdown`, `detach_progress_host`, context destruction (unchanged by F2) |
| `ProgressSource.tla` | pass, acknowledgement, epoch freshness, control generations | `poll_progress`, `acknowledge_progress_notification`, `acknowledge_progress_control` (unchanged by F2) |

Two consequences of the contraction that the models already encode as
invariants, and that F2 made *stricter* in C++ rather than looser:

- The `Completion`-era cross-thread release rule (a release accepted while
  `publication_inflight`) is gone from the core entirely; the release edge now
  requires the published state, which is what `RequestCore.tla`'s
  consume/discard predicate models.
- The identity a caller keeps is now always the copied `RequestId`, so the
  model's generation-and-context freshness check has one C++ counterpart
  (`resolve_public_` plus `lookup`) instead of two.

## What the models do not cover

The models establish protocol-level safety and the stated liveness properties
for the core, the observer protocol, shutdown and the progress source. They do
not model the caller-owned `Completion<T>` (so its removal cannot invalidate a
modeled action), they do not model the backend transports, and they do not prove
that the C++ implements the model. That direction is carried by the production
suites, the cutover/protocol tests and the mutation campaign recorded in this
directory tree.

## Status

`verdicts.json` records the census of this run: 171 configurations queued, 170
finished with `PASS` verdicts, none failed, and `rcore-live` was still running
when the capture was closed. Its documented cost on this model is ≈18.5 h wall
with a ≈40-48 GB heap (E1/E2 records in `docs/roadmap/v1-conformance.md`), so the
run was capacity-bound, not verdict-bound.

F2 changed no model or configuration file (`git diff 0ed16588..HEAD -- formal/`
is empty), so the baseline revision's own full-gate `VERIFY_TLA: PASS` covers
that one unchanged configuration. `../gates.json` records the gate as
`INCOMPLETE: rcore-live`; it is never reported as a pass.
