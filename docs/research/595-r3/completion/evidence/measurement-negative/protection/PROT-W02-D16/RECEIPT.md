# PROT-W02-D16 — R2 same-SHA legacy protection (R3 B not executable)

| field | value |
|---|---|
| phase status | **PASS** for the R2 same-SHA part; R3 B part **NOT_ESTABLISHED / NO_W02_R3_ARM** |
| case | `W02-uring-4k-d16` |
| comparison | `B_R2 - A_R2` |
| arms | A = A_R2 (immediate); B = B_R2 (bounded progress, same binary) |
| pairs | 24/24 valid; records 48/48 |
| binary sha256 | `32beabcd891801eb65dab1d6e2df72aa0b89a98a2c16a114d398527cb821589a` |
| env | `['SLUICE_BENCH_595_DEPTHS=1']` |
| window | 2026-11-05T11:23:31Z .. 2026-11-05T11:24:32Z; fixture unchanged = True |
| raw | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/PROT-W02-D16/`; enter diagnostics `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/DIAG-R2-D16-A`, `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/DIAG-R2-D16-B` |

| metric | mean | 95% CI | one-sided 95% upper | threshold | Holm alpha | Holm p | pass |
|---|---|---|---|---|---|---|---|
| cpu | -28.111% | [-28.702%, -27.527%] | -27.613% | 5% | 0.0125 | 0.00010 | True |
| tput | -39.241% | [-40.395%, -38.098%] | -38.264% | 3% | 0.0500 | 0.00010 | True |
| p95 | -32.590% | [-33.479%, -31.652%] | -31.819% | 5% | 0.0167 | 0.00010 | True |
| p99 | -32.400% | [-33.907%, -30.971%] | -31.179% | 5% | 0.0250 | 0.00010 | True |

Positive = B_R2 worse; negative = B_R2 better. The R2 B arm is substantially
better at this depth; this PASS is a protection result for the old R2 mechanism
only.

## Enter/SQE diagnostic (strace lane; not performance evidence)

- A_R2: total=60004, pure_submit=60000, to_submit_hist={'1': 60004}
- B_R2: total=11293, pure_submit=11289, to_submit_hist={'1': 3799, '7': 3747, '8': 3747}

B_R2 merges up to 8 SQEs (`to_submit=8` 3,747 times, `7` 3,747 times, `1` 3,799
times including 4 setup probes); total process enters fall from 60,004 to 11,293.
This is the old owner/payload/witness/cap deferred-flush mechanism, not the R3
Context-level ready-batch entry.

## Boundary

- The completion patch does not touch `bench/bench_w02_pipeline.cpp`; R3 B cannot
  execute on W-02. The R3 B part is therefore `NOT_ESTABLISHED`, not PASS.
- Cross-SHA subtraction is prohibited (`PROV-5`).
