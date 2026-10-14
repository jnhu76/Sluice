# RequestCoreLive.cfg fresh run — final status (F6-F04)

Run launched by the F6 audit at the frozen tree (`821c489f`), detached
2026-10-14 22:23 +0800, pid 1393294:

```
java -Xmx40g -cp <pinned tla2tools 1.7.4, SHA-256 936a2620…> tlc2.TLC \
  -workers 8 -deadlock -config formal/tla/RequestCoreLive.cfg \
  formal/tla/RequestCore.tla
```

Log: `/tmp/f6-tla-rcore-live/run.log` (captured verbatim excerpt below).

## Result: COMPLETE_PASS

```text
Model checking completed. No error has been found.
153,347,909 states generated, 26,662,372 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 53.
Finished in 03h 10min at (2026-10-15 01:33:21)
```

- Complete state space traversal (0 states left on queue); all 12 temporal-
  property branches checked clean on the complete space (final check 18min 13s;
  three earlier interim checks also clean).
- **The complete graph is 26,662,372 distinct states — exactly the E2-dated
  receipt's behavior-graph size** (attempt 2, 171/171 PASS at `eb7bf055`).
  This independently confirms at the frozen tree what the F2 reuse ruling
  verified by byte-identity: same model, same constants, same graph.
- Fingerprint-collision probability of missed states: 2.5E-5 (actual
  fingerprints), TLC's standard bound estimate.

## Standing of this result

- The **closure authority for REQ-06/L1 remains the E2-dated complete receipt
  under F2's exact historical-reuse ruling** — this fresh run was launched
  after that authority was already established and does not backdate it.
- The fresh run eliminates the recency gap that F6-F04 recorded: full
  liveness (L1/L5 under `FairSpec`, 21 invariants) is now *also* directly
  verified at the frozen tree, not only by reuse. F6-F04's own rule
  ("if it completed clean, this finding downgrades to P3") applies; its
  disposition is `SUPERSEDED_BY_NEW_EVIDENCE` in `findings-disposition.md`.
- Safety side (`RequestCore.cfg`) had already completed clean at the frozen
  tree during the audit (152,240,323+ generated / 26,566,666+ distinct at the
  final progress line / depth 53 / 8m09s); both runs traverse the same
  constants and both completed at depth 53.

## Verbatim log excerpt (final segment)

```text
Progress(53) at 2026-10-15 01:15:03: 153,347,909 states generated, 26,662,372 distinct states found, 0 states left on queue.
Checking 12 branches of temporal properties for the complete state space with 319948464 total distinct states at (2026-10-15 01:15:03)
Finished checking temporal properties in 18min 13s at (2026-10-15 01:33:17)
Model checking completed. No error has been found.
  Estimates of the probability that TLC did not check all reachable states
  because two distinct states had the same fingerprint:
  calculated (optimistic):  val = 1.8E-4
  based on the actual fingerprints:  val = 2.5E-5
153347909 states generated, 26662372 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 53.
The average outdegree of the complete state graph is 1 (minimum is 0, the maximum 11 and the 95th percentile is 3).
Finished in 03h 10min at (2026-10-15 01:33:21)
```
