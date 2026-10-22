# P>0 Mechanism Cost — Controlled Seam Bound (Gate R4, Path B)

## Status

- **`P_POSITIVE_SEAM_BOUND = PASS`** — all four P>0 phases have E1/Base
  median paired CPU ratio ≤ 0.97 against the preregistered ≤ 1.05 line.
- **`P_POSITIVE_NATIVE = NOT_MEASURED`** — retained; see
  `../reachability.md` and `../native-p5-probe/`. The seam bound is
  mechanism-scope evidence for the owner's admission decision, not a
  native-kernel performance result.

## What was measured

Both arms compile the identical harness
(`pending_control_seam_ab.cpp`) against their own backend sources with the
internal test seam:

| Arm | Sources | Build |
|---|---|---|
| base | `~/sluice-e1-prodbase` @ `859e4b56` (master) | `-std=c++20 -O3 -DNDEBUG -DSLUICE_ASYNC_INTERNAL_TESTING -DSLUICE_HAS_LIBURING` |
| e1 | `~/sluice-e1-product` @ product HEAD | same flags + `-DSLUICE_E1_SEAM_ARM` (adds only untimed snapshot validation reads) |

The submit seam (`switchable_submit`) is a fake kernel: hold mode consumes
nothing so `io_uring_get_sqe` fails once the ring is full; release mode
mimics a kernel submit that consumed every pending SQE. **What is measured
is the whole controlled execution path's user-space cost** (no syscalls on
the submit path), under a workload that holds P>0 for most of every cycle.
It isolates the counter's maintenance cost plus the gated scan; it does not
measure end-to-end native performance, and it is not presented as one.

Per cycle: fill the Q=8 SQ ring (R canceled ops + Q−R uncanceled fillers),
cancel the R running ops (all retained as sticky intents), hold M=24
pressure passes (both arms scan the 64-entry router each pass while P>0;
E1 additionally maintains the count), release, issue the R retained
intents, inject CQEs, settle everything, assert retirement.

## P>0 evidence (both arms)

- E1 arm: the in-lock snapshot observes `derived_count` = R per cycle
  (`p_max`: 1 for low_p, 8 for high_p / repeated_cancel / high_rotation),
  parity holds in every sampled state (0 violations across 240 rows), and
  the count returns to zero at settle.
- Base arm: during the hold, `live_control_sqes == 0` **and** the transport
  ledger cardinality equals exactly the Q data-op entries (a prepared
  control would append a `cancel_control` entry) — no control was prepared,
  so the intents were genuinely retained (P>0), then all R are issued after
  release (`live_control_sqes == R`).

## Results (15 interleaved AB/BA rep pairs; per-rep pairing, repeats averaged)

| Phase | Median E1/Base CPU | Pairs < 1.0 | Min | Max | Gate ≤ 1.05 |
|---|---|---|---|---|---|
| low_p (R=1) | 0.9382 | 14/15 | 0.9250 | 1.0317 | PASS |
| high_p (R=8) | 0.9596 | 15/15 | 0.9450 | 0.9761 | PASS |
| repeated_cancel (R=8, 3 cancels/op) | 0.9568 | 15/15 | 0.9419 | 0.9776 | PASS |
| high_rotation (R=8, 400 cycles) | 0.9613 | 15/15 | 0.9397 | 0.9757 | PASS |

Counters per row: cancel attempts, `requested` dispositions, sticky
transitions, control SQEs prepared/submitted, settled ops, p_max, parity
violations — all rows `ok=true` (240/240; raw in `results/seam.jsonl`,
hashes in `results/provenance.txt`). Cancel-phase CPU (the `cancel_ns`
column) is E1-favorable or neutral in every phase (e.g. high_p medians
≈98.0 µs base vs ≈99.0 µs E1 per invocation): the counter's set/clear work
on the cancel path is within noise of the measurement floor.

## Interpretation limits

- The seam world removes kernel syscalls, so user-space scan work is a
  larger fraction of total cost than natively; the ratios here bound the
  mechanism cost, they do not predict native end-to-end gains (those are
  Gate R5's `product-ab/`).
- E1 being faster *under P>0* is expected: within each cycle the release
  and settle windows have count==0, where E1 skips scans base still pays;
  the P>0 hold windows perform identical scans on both arms. The bound the
  gate needs is that E1 is **not slower**, and it is not.
- Sticky-intent states in this campaign are produced by the fake-kernel
  seam; their retention, retry and settlement semantics are additionally
  pinned by the deterministic suite (`tests/uring_pending_control_parity_test.cpp`).
