# limitations.md — what this round does and does not establish

## Measurement scope

1. **Single host, single storage class.** One E5-2666 v3 bare-metal host, warm
   tmpfs. No device I/O, no filesystem variation, no network. The #622 round
   showed the io-wq punt fraction (and therefore part of the win) is
   FS-conditional (tmpfs 100 % punt vs btrfs NOCOW warm buffered 0 % punt);
   results here must not be extrapolated across storage classes.
2. **One workload family.** W-02 (same-owner synthetic pipeline) and W-03
   (split-owner external driver) only. No application trace (#578 not run); no
   real consumer logic beyond the bench's take loop.
3. **Latency window includes consumer scheduling.** The bench latency pass
   measures submit→`take` return; `take` is the observable consumption point,
   so p50/p95/p99 include consumer wake/schedule. This is the frozen
   definition, but it is not pure submission-path latency.
4. **Per-request submit delay is not instrumented.** It is bounded indirectly:
   max submit→take ≈ 0.1–0.2 ms on the main shape and the deterministic
   prepark oracle (held residue flushed before the park gate), far below the
   10 ms parked-owner cadence. The actual per-deferred-SQE wait distribution
   and flush attribution are unmeasured (RQ3).
5. **Enter count is submit+wait combined.** The strace ledger counts all
   `io_uring_enter` syscalls; a pure-submit split was not instrumented. The
   ≥ 50 % Mechanism-gate claim survives the worst-case archived-data bound
   (reanalysis receipt §3), but "enters fell 81.2 %" must not be quoted as
   "pure submit enters fell 81.2 %".
6. **Kernel CPU outside the process is not counted.** io-wq and syscall-context
   kernel time inside the process are counted (thread-group accounting,
   verified); unrelated kworkers/softirq on other CPUs are not. No SQPOLL was
   used, so no poller thread exists to miss.
7. **Two control cells are power-limited.** W-03 d4 p95 and W-03 d64 p99 have
   A/A noise floors (3.88 pp / 7.21 pp) at or above the 5 pp bound, so
   non-inferiority is not established there (means ≈ 0). They are disclosed,
   not silently dropped; a future round needs more pairs or a tighter quantile
   estimator for these cells.
8. **Frequency and ambient load.** schedutil 1.2–3.5 GHz, no governor change;
   the research session's own tooling occasionally appeared in loadavg
   (guard raised 1.5 → 4.0 after D1, recorded pre-run; per-invocation loadavg
   archived). This is single-host field quality, not lab isolation.

## Protocol history (must not be laundered)

9. **One measurement-condition repair happened.** The original A/A calibration
   failed the p99 half-width (3.71 pp > 3 pp) and was repaired (5 measured
   reps per invocation) before any A/B timing. Both pre- and post-repair
   receipts are preserved; [protocol.md](protocol.md) §5. A pre-timing
   machinery determination (io-wq thread-group accounting; additive sampler
   dropped as double-counting) was recorded before any timing receipt.
10. **D2 replication is confirmation, not gate membership.** It was run after
    D2 failed, to confirm direction; the registered Holm family has nine
    shapes. Adding it would only strengthen the D2 CPU failure
    (reanalysis receipt §4) — the verdict is robust to either choice.

## Research-only status

11. **Arm B is research instrumentation.** `SLUICE_595_ARM` is read once at
    backend construction; it is not a public knob and ships nowhere. No
    product PR exists; nothing here authorizes a product change.
12. **#594 numbers are priors, not this round's evidence.** The K4-b campaign
    measured −28.07 % (d16) with a different gate set and no D2 cell; this
    round's numbers stand on their own receipts.
13. **Placement findings are safety-only.** cpu2 default; cpu8/SMT checks are
    8-pair confirmations, not a placement study.

## Preservation limits

14. **Backup locality.** The full-evidence backup
    (`backup/sluice-595-e5-full-evidence-20261104.tar.gz`) lives on the same
    host and filesystem as the original tree. It is verified byte-identical
    (extract-and-check against the manifest) but is **not** off-host
    redundancy; the evidence cannot yet be called durably persisted. Any
    off-host copy remains future work for the owner.
15. **Committed raw data is decision-complete, not exhaustive.** Per-invocation
    bench JSONL and stdout logs stay only in the full archive
    (`raw-minimal/EXCLUDED.md` records what and why, with manifest hashes).
