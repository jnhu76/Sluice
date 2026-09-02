# rbuf-e0-probe-native-2 — POST-MEASUREMENT PROBE VALIDATION

Adversarial-review remediation artifact for RBUF-E0 (#272). This session is
a post-measurement capability-probe rerun ONLY. It is NOT part of any formal
session and MUST NOT be counted as formal evidence. The formal sessions
remain: `rbuf-e0-q0-uring-stability-native-1` (50 runs),
`rbuf-e0-steady-native-1` (112 runs), `rbuf-e0-amortization-native-1`
(56 runs) — 218 formal runs, 0 gate errors, raw evidence hash-unchanged.

## Why this rerun exists

The capability probe in `bench/rbuf_e0_bench.cpp` originally submitted
READ_FIXED and WRITE_FIXED together in one `io_uring_submit` (no read→write
dependency), so probe results could depend on kernel-side op ordering — an
ordering race for future foreign kernels / ARM hosts. The formal `--run`
path never used the probe, so this was never a formal-evidence problem.
Remediation made the probe strictly serial:

```
submit READ_FIXED -> wait CQE -> validate res == 4096 + buffer content
submit WRITE_FIXED -> wait CQE -> validate res == 4096
pread destination -> validate content
unregister_buffers
```

No IOSQE_IO_LINK, no chaining; ordering lives in the submission structure.
Formal benchmark rerun: NO (forbidden by remediation scope). Formal `--run`
C++ path: source-unchanged (diff confined to `run_probe()` + its header
comment).

## Probe rerun (2026-09-02, Host-0)

Rebuilt bench (release, clang, --with-liburing):
sha256 `9494dd48baaf3cf64feb1b035e71fa72e5cb9b5c826548c0a019bd29989dfd49`.
(The formal sessions' immutable `environment.json` files record the frozen
`7649fceb...` binary they actually ran under — unchanged.)

Command:
`python3 research/rbuf-e0/scripts/rbuf_e0.py probe rbuf-e0-probe-native-2`

Result (`probe.json`, bench exit 0, `capable: true`):

```
uring_queue_init:            PASS (errno 0)
register_buffers:            PASS (errno 0)
READ_FIXED:                  PASS (res = 4096)
read content:                PASS (read_content_ok = true)
WRITE_FIXED:                 PASS (res = 4096)
destination content:         PASS (write_content_ok = true)
unregister_buffers:          PASS (errno 0)
write_submitted_after_read_cqe: true   (new serial-protocol field)
memlock soft/hard:           8388608 / 8388608 (8 MiB; identical to the
                             original probe observation)
U2-feasible cells:           512Kx2, 1Mx2, 2Mx1, 2Mx2 (identical)
U2-INFEASIBLE cells:         2Mx4, 4Mx2 (identical)
perf instructions:           YES   FORMAL_ELIGIBLE: YES
```

## Regression guards

- `scripts/check_rbuf_e0_probe_order.py --bench <binary>`: PASS —
  source-structural assertion (one READ_FIXED prep, one WRITE_FIXED prep,
  strict read-prep → submit → wait_cqe → write-prep → submit → wait_cqe
  order, two submits, no IOSQE_IO_LINK) plus executed-probe check.
- `scripts/check_rbuf_e0_analysis.py`: PASS — setup_plus_teardown_fraction
  identity recomputed from raw, absolute lifecycle timings (register_ns
  1085916 / unregister_ns 65222), U1/U2 causal isolation re-verified in all
  4 steady cells + all 4 amortization horizons, frozen verdict vocabulary.
- Immutable-evidence hash audit (raw/runs.jsonl, raw/perf.csv,
  manifest.json, gates.json x 3 formal sessions, pre- vs post-remediation):
  12/12 sha256 UNCHANGED.
