# D1 io_uring Poison/Recovery Model

This finite TLA+ model covers three load-bearing Phase D1 P0-D properties:

- Class-A recovery identity is a monotonic logical SQ sequence; the wrap-masked
  physical slot is storage metadata only.
- An original operation terminal is not user-ready while its exact cancel
  control reference is still prepared or submitted.
- Once the permanent-negative enter poisons the ring, the transport submission
  count cannot advance beyond the snapshot taken at that proof boundary.

It also checks bounded ledger size, positive-prefix/Class-A disjointness,
poisoned-ledger recovery coverage, no post-poison submit, and success provenance.
The model abstracts a permanent-negative proof as one atomic `PermanentFailure`
transition; the Linux/liburing source theorem licensing that transition remains
the responsibility of the Phase D1 source audit.

Expected gates:

- `D1UringPoison.cfg`: PASS.
- `D1UringPoisonBuggyMasked.cfg`: violates `InvLogicalIdentity` only after two
  distinct logical sequences reuse the same wrap-masked SQ slot, proving that
  slot cannot serve as recovery identity.
- `D1UringPoisonBuggyControl.cfg`: violates `InvReadyControlQuiescent` when the
  original CQE publishes before its submitted control retires.
- `D1UringPoisonBuggyPostPoisonSubmit.cfg`: violates
  `InvNoSubmitAfterPoison` when an enter after poison advances the transport
  submission count beyond the snapshot taken by the failing enter.

This model does not prove the C++ implementation. The matching implementation
detectors live in `tests/uring_submit_failure_test.cpp`.

Deadlock detection remains active outside modeled terminal quiescence. The only
explicit self-loop is guarded by a ready operation with no prepared/submitted
control reference; it represents the completed system, not arbitrary progress.
