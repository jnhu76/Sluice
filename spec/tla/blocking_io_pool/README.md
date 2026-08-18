# blocking_io_pool — bounded blocking-I/O worker pool

Protocol: **bounded blocking-I/O worker pool** — `submit/try_submit`
(backpressured admission), FIFO dequeue by a fixed worker set, bounded
in-flight execution, task completion, `get` consumption, and
shutdown-drains-then-quiesces.

C++ authority: `src/blocking_io_pool.cpp`,
`include/sluice/detail/blocking_io_pool_impl.hpp`
(the manifest binding was the dangling `src/core/blocking_io_pool.cpp` before
the 2026-08-18 audit). Design/owner doc:
`docs/verification/formal/blocking-io-pool-tla-spec.md`.

## Gates

| Gate | cfg | Expect |
| ---- | --- | ------ |
| Safety (6 invariants) | `BlockingIoPool.cfg` | PASS |
| Liveness (`StarvationFree` under `FairSpec`) | `BlockingIoPool_liveness.cfg` | PASS |
| NEG-BIP-1 dequeue drops worker bound | `BlockingIoPoolBuggyUnboundedDequeue.cfg` | `WorkerBoundInvariant` violated |
| NEG-BIP-2 shutdown discards queued work | `BlockingIoPoolBuggyShutdownDiscardsQueued.cfg` | `NoLostAcceptedTask` violated |

## Audit notes (2026-08-18)

- The vacuous `Linearizable` gate (guard-enforced by `Get`, could never fail)
  was replaced in the safety cfg by `NoLostAcceptedTask` (accepted-task
  conservation), which NEG-BIP-2 actually exercises.
- Liveness boundary assumptions: `WF(Dequeue)`/`WF(Complete)` assume persistent
  workers and modeled callable termination — documented in
  `docs/verification/formal/blocking-io-pool-tla-spec.md` §assumptions. The
  C++-specific hazards OUTSIDE this model's state space: condvar lost-wake
  mechanics, parked-submitter release on shutdown, `wait_idle` starvation.
- Not modeled (documented scope): `wait_idle()`, worker-exit joins,
  `WorkerScope` thread-local guards, error-code distinction
  (`would_block` vs `invalid_state`), `PoolStats`.

## Reproduce

```sh
python3 scripts/formal/verify.py suite blocking-io-pool
```
