# PROT-W02-D3 — R2 same-SHA legacy protection

| field | value |
|---|---|
| phase status | **PASS** (R2 old mechanism; NOT an R3 B result) |
| case | `W02-uring-4k-d3` |
| comparison | `B_R2 - A_R2` |
| arms | A = A_R2 (immediate, `271b81bf+7be9d014`); B = B_R2 (bounded progress, same binary) |
| pairs | 24/24 valid; records 48/48 |
| binary sha256 | `32beabcd891801eb65dab1d6e2df72aa0b89a98a2c16a114d398527cb821589a` |
| env | `['SLUICE_BENCH_595_DEPTHS=1']` |
| window | 2026-11-05T11:22:02Z .. 2026-11-05T11:23:30Z; fixture unchanged = True |
| raw | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/PROT-W02-D3/`; enter diagnostics `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/DIAG-R2-D3-A`, `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/raw/protection/DIAG-R2-D3-B` |

| metric | mean | 95% CI | one-sided 95% upper | threshold | Holm alpha | Holm p | pass |
|---|---|---|---|---|---|---|---|
| cpu | -7.327% | [-8.104%, -6.541%] | -6.673% | 5% | 0.0125 | 0.00010 | True |
| tput | -7.975% | [-8.897%, -7.058%] | -7.207% | 3% | 0.0500 | 0.00010 | True |
| p95 | -12.795% | [-14.725%, -10.359%] | -10.832% | 5% | 0.0167 | 0.00010 | True |
| p99 | -11.511% | [-16.212%, -6.390%] | -7.288% | 5% | 0.0250 | 0.00010 | True |

Positive = B_R2 worse; negative = B_R2 better. All four metrics pass the point-estimate,
one-sided-95%-upper and Holm-adjusted gates.

## Enter/SQE diagnostic (strace lane; not performance evidence)

- A_R2: total=60004, pure_submit=60000, to_submit_hist={'1': 60004}
- B_R2: total=40009, pure_submit=40005, to_submit_hist={'1': 20014, '2': 19995}

The R2 B arm batches 2 SQEs per enter for about one third of data submits
(`to_submit=2` 19,995 times); total process enters fall from 60,004 to 40,009.
This is the old R2 deferred-flush mechanism, not the R3 Context-level ready-batch
entry. `READY_EXPOSURE` classifies W-02 D3 as STARTUP_ONLY; the observed 2-SQE
merges are deferred-flush merges, not ready-group size.

## Boundary

- Cross-SHA subtraction is prohibited (`PROV-5`). This block says nothing about R3 B.
- D3 is not a product win-gate.
