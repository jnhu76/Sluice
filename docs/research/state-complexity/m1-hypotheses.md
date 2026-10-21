# M1 Hypotheses (admitted by M0)

Three mechanisms admitted, each with a concrete hypothesis, semantic-equivalence
requirements, and the experiment that can refute it. Layer A measures the
mechanism in isolation; Layer B measures its share inside the real Sluice W-02
path. **Nothing here changes production code** — candidates that survive become
`PROPOSE_UPSTREAM` proposals with a minimal change sketch, not patches to `src/`.

---

## H1 — io_uring router: linear cookie scan vs direct `user_data`-encoded slot index

- **Baseline** (`uring_transport.cpp:551-557`): every CQE and consumed control
  does a reverse linear scan of `router_` (R × 32 B entries) under
  `dispatch_mtx_` to map `user_data` (cookie) → route.
- **Candidate**: the backend already owns `user_data` end-to-end; encode the
  router slot index into it (cookie stays authoritative inside the entry and
  is re-checked on lookup, preserving the never-reuse anti-ABA property —
  `allocate_cookie_` is monotonic and fail-fasts at domain exhaustion).
  Encoding envelope: bit 63 control tag, bits [48..62] router slot, bits
  [0..47] cookie. This narrows two production domains and any proposal must
  carry both: cookie exhaustion must fail fast at 2^48 (production: 2^63),
  and `request_capacity` must be validated ≤ 32768 (slot field width);
  slot_bits + cookie_bits + 1 = 64 is the joint constraint.
  Lookup becomes one indexed load + cookie equality check.
- **Hypothesis**: at R=64..1024 the scan costs tens of ns per completion under
  a hot mutex; direct indexing removes the R-dependence. Expected crossover:
  scan wins only at R ≤ ~8 (single cache line).
- **Equivalence arm duties (both arms)**: identity check (cookie equality),
  stale/foreign `user_data` miss handling (return "not found" sentinel),
  control-tag masking, capacity bound, never-reuse semantics, and the
  poison-path full-lookup behavior must be identical.
- **Oracle**: reference model = ordered map cookie→slot + liveness bitmap,
  driven by the same op sequence (dispatch/retire/cancel/lookup); any lookup
  disagreement (wrong slot, missed miss) fails. Adversarial: stale cookie
  after retirement must miss in both arms; control tag must never resolve to
  an operation route.
- **Layer B**: per-completion share via a W-02-shaped context measurement and
  `perf stat` attribution on the existing bench binaries (ring profile).

## H2 — owed-event visibility: full `delivery_` sweeps vs maintained owed count (+ uring per-slot locking)

- **Baseline**: ThreadPool sweeps `delivery_` twice per pass under one lock
  (`threadpool_backend.cpp:569-577`, `:597-600`); io_uring sweeps once per
  pass **taking and releasing `dispatch_mtx_` once per slot index**
  (`uring_backend.cpp:636-654`) plus once more in `poll_progress`
  (`any_event_owed_locked_`, `:295-302`).
- **Candidates**: (a) snapshot-under-one-lock for uring (mirror the
  ThreadPool `owed_batch_` shape — in-tree precedent); (b) maintained owed
  counter (derived summary updated beside every `event_owed` write/clear,
  all under the same mutex; verdict checks read the counter).
- **Hypothesis**: per-pass cost is O(C) today (C lock pairs on uring); a
  counter makes it O(1) and a snapshot makes it O(1 locks + C reads). At
  C=64 and ~20 ns/lock pair the uring shape burns ~1-1.6 μs per pass; the
  share of end-to-end op cost decides PROPOSE_UPSTREAM vs KEEP.
- **Equivalence arm duties**: set/clear parity with the per-record flag,
  empty/full behavior, honest residue reporting (PROG-01/V29: a pass must
  report owed work it did not absorb), and identical delivery order.
  Double-set cannot occur in production before delivery: the owed event's
  core control ref pins the slot against reclaim until delivery, so the
  oracle's driver (which never double-sets) matches the production
  precondition. BACKEND-04 scope: a counter may serve only the per-pass
  residue verdict and wait/park fast paths; `internal_work_retired()` and
  destructor quiescence must keep consulting the authoritative `delivery_`
  scan.
- **Oracle**: reference model tracks owed set per slot; after each scripted
  op sequence (submit/zero-op/complete/pass) both arms must agree on the
  delivered set, the residue verdict, and per-slot flags. Negative control:
  drop a set-clear pair → oracle must fail the counter arm.

## H3 — RequestScope slot management: first-free scans vs LIFO free list

- **Baseline** (`request_scope.cpp:46-55`, `request_scope.hpp:114-129`):
  O(S) first-free scan per submit; O(S) ticket-id scan per `take`/`wait_for`.
- **Candidates**: (a) LIFO free-slot vector for reserve (mirrors
  `RequestCore::free_slots_` — same mechanism, already in-tree); (b) free
  list + resolve only via free list is *not* possible (resolve needs the id);
  measure whether (a) alone shifts the W-02 mix, and quantify the resolve
  share separately (the `-scope64` G0 case does this in production).
- **Hypothesis**: reserve scan is eliminated at all S; resolve stays O(S).
  Note the countervailing risk the experiment must check: under the W-02
  FIFO-oldest shape the first-free scan is already O(1) (the freed low slot
  is reused immediately), so a free list can only add work there; its win
  region is high-occupancy random-consumption patterns.
- **Equivalence arm duties**: same would_block at capacity, same
  reservation-guard rollback, same id-resolution exactness (stale ticket →
  empty observation), same release-on-take semantics.
- **Oracle**: reference model = set of live (slot,id) pairs; identical
  submit/take/finish sequences must produce identical slot assignments where
  assignment is observable (it is not public — so the oracle checks
  *behavioral* equivalence: accepted/rejected/observed results, plus
  capacity behavior), and per-slot state invariants (no double assignment,
  no leak) in both arms.
- **Layer B**: run the real W-02 binary's `-scope64` isolation case vs its
  depth-matched twin from the G0 configuration (Layer B without any code
  change), plus mechanism-level Layer A arms.

---

## Measurement protocol (applies to all)

- Host: WSL2 kernel 6.18.54.1, AMD Ryzen 7 5800H (Zen 3), 8 vCPU, 15 GB;
  pinned to one core with `taskset` for Layer A; PMU counters via
  `perf stat -e cycles,instructions,cache-misses,branch-misses` (verified
  readable on this host); `perf stat` never used alone for a verdict.
- Build: release, gcc 15.2, xmake default flags; candidate arms compiled in
  the same TU/binary with identical flags; H1 alternates arms per
  repetition, H2/H3 run arms as sequential blocks (their verdicts rely on
  disjoint repetition ranges instead of interleaving); operation sequences
  fixed (xorshift-replayable) and identical across arms; correctness oracle
  runs before timing and gates every repetition.
- Capacities: 8, 16, 32, 64, 128, 256, 1024. Mixes: lookup-heavy (H1),
  pass/idle-heavy (H2), insert/churn-heavy (H3), high-miss (all).
- Metrics reported with tool source: wall ns/op (`clock_gettime` MONOTONIC),
  cycles+instructions+cache/branch misses (`perf stat`, counter-read mode),
  p50/p95 only where sample count makes quantiles meaningful; WSL2
  noise policy: differences within the measured run-to-run spread are
  reported as "not distinguished", never as wins.
- Raw outputs land in `docs/research/state-complexity/results/` (machine
  readable JSONL) with the exact commands in `bench/mechanisms/run_mechanisms.sh`.
