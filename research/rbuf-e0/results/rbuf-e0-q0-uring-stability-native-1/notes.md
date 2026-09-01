# rbuf-e0-q0-uring-stability-native-1 — notes

Phase Q0 io_uring stability qualification for RBUF-E0 (#272), Host-0,
2026-09-02. 50 runs of U1 (ordinary uring, aligned reusable storage) at
2 MiB x d2, 1 GiB copy, single submission/completion thread, full same-work
gates per run (bench-internal fail-closed + driver-side hash/perf gates).

Result: 50/50 valid, 0 unexpected canceled, 0 error terminals,
0 hash failures, 0 gate errors -> Q0 PASS per prereg §11.

This QUALIFIES the single-worker direct-liburing regime for formal RBUF-E0
measurement. It does NOT close #262 (multi-worker production-runtime cancel
anomaly, different machinery): #262 did not reproduce in this restricted
regime.

Context note: these 50 runs also form an H1 steady-state reference for
U1 @ 2M×d2 on this host (transfer_ns medians recorded in raw/runs.jsonl),
but formal steady-state conclusions come from the interleaved steady session.
