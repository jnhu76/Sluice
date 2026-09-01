# RBUF-E0 — io_uring registered/fixed-buffer steady-state and amortization crossover (#272)

Research campaign: does io_uring buffer registration materially reduce
steady-state CPU/wall cost under equivalent I/O semantics, and what reuse
horizon amortizes the registration lifecycle cost?

**RESEARCH ONLY — no production implementation, no public API change, no
default change.** The production `UringAsyncBackend` has no registered-buffer
capability (see `RBUF-E0-AUDIT.md`); this campaign must not add one.

## Documents

| File | Content |
| --- | --- |
| `RBUF-E0-AUDIT.md` | Pre-implementation audit: ordinary uring path, registered-buffer support census (NO SUPPORT EXISTS), topology decision, Q0 design |
| `RBUF-E0-PREREGISTRATION.md` | FROZEN before formal measurement: arms, matrix, horizons, gates, decision rules, verdict vocabulary |
| `RBUF-E0-REPORT.md` | Formal report (after measurement) |
| `campaign.json` | Machine-readable frozen constants mirror |

## Layout

```text
scripts/rbuf_e0.py      campaign driver (status/probe/generate/q0/steady/
                        amort/summarize)
scripts/plot_rbuf_e0.py derived SVG plots
results/<session>/      immutable sessions (environment/manifest/gates/
                        notes/raw/runs.jsonl/raw/perf.csv/summary/analysis)
plots/                  derived SVGs (regenerable)
```

The measurement instrument is `bench/rbuf_e0_bench.cpp` (research-only
direct-liburing mechanism bench; target `rbuf_e0_bench`, built only under
`--with-liburing`). Arms: **U0** ordinary-natural, **U1** causal ordinary
control (aligned reusable storage, ordinary opcodes), **U2** SAME storage +
`io_uring_register_buffers` + `READ_FIXED`/`WRITE_FIXED`. The only U1→U2
delta is registration + fixed opcode selection; causal claims come from
U1 vs U2 only.

## Reproduce (Host-0)

```sh
xmake f -m release --toolchain=clang --with-liburing=true -y
xmake build rbuf_e0_bench
python3 research/rbuf-e0/scripts/rbuf_e0.py probe    <session-id>
python3 research/rbuf-e0/scripts/rbuf_e0.py generate <session-id>
python3 research/rbuf-e0/scripts/rbuf_e0.py q0       <session-id>
python3 research/rbuf-e0/scripts/rbuf_e0.py steady   <session-id>
python3 research/rbuf-e0/scripts/rbuf_e0.py amort    <session-id>
python3 research/rbuf-e0/scripts/rbuf_e0.py summarize <session-id>
python3 research/rbuf-e0/scripts/plot_rbuf_e0.py     <steady-session-id>
```

All conclusions are HOST-LOCAL ONLY. #262 is not closed by this campaign;
Q0 only records that the multi-worker cancel anomaly did not reproduce in
this restricted single-worker regime.
