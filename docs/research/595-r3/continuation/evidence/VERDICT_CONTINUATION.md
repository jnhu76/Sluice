# VERDICT_CONTINUATION — #595 R3 续跑轮（翻转后）机器可读裁决

generated_utc: 2026-11-05T15:05Z（分析完成后写盘；所有块在 14:31–14:44Z 测量）

| 键 | 值 |
|---|---|
| EXECUTION_STATUS | `COMPLETE_MEASURED` |
| SEMANTICS_READY_FLIPPED | `true`（§14.3 回执 `FLIP_RECEIPT_14_3.md`，dd165759…；2026-11-05T14:33Z） |
| LATENCY_METRIC | `unified app-ready→observed（含准备/扫描/提交）`，delta patch 57f21e02…/4aa7b306… |
| MAIN_GATE_W03_D16_B_A0 | `FAIL`（p95 +25.75pp 上界 +27.95；p99 +37.85 上界 +39.94 > +5 门；CPU −14.13 上界 −13.68 达标；吞吐 +16.52 达标；主/次 CPU 符号一致；功效达标） |
| MECH_9_3_ENTER_GATE | `FAIL_THIS_ROUND`（B 33,750 vs A0 60,000 = −43.75% < 50%；A0/A1 =60,000 全 1；ΣSQE 60,000 闭合；完成轮同基准 51.29% PASS，两次均登记） |
| Q1_W02_D2_NEW_B | `NONINFERIOR_PASS_WITH_POWER_NOTE`（CPU +2.17 ub +2.72；吞吐 −2.11；p95 +1.75；p99 +1.49 ub +2.87；Holm 全拒；机制 entry=19,999/group=1 如登记；AA 面 p99 半宽 3.431>3 → p99 记 NOT_ESTABLISHED(面功效)） |
| Q1_LEGACY_CONTRAST | 旧 B_R2 同判据 FAIL（吞吐 −3.30 ub −3.89；p95 +5.23 ub +5.91；p99 +7.05 ub +9.07）——新 B 守住，旧 B 不守 |
| Q2_W03_D16 | `ANSWERED_NO`（CPU/吞吐改善成立；完整请求尾延迟不守） |
| AA_W03_D16 | `PASS`（1.245/1.215/2.628 pp） |
| AA_W02_D2 | `FAIL_p99_power`（3.431 pp > 3） |
| ATTRIBUTION_A1_MINUS_A0 | CPU +6.73 / p95 +71.04 / p99 +64.93 / 吞吐 −6.30 pp（adapter 准备路径代价，归因用） |
| ATTRIBUTION_B_MINUS_A1 | CPU −19.54 / p95 −26.45 / p99 −16.30 / 吞吐 +24.37 pp（成批相对 adapter 路径回收，归因用） |
| MECHANISM_PERFORMANCE_VERDICT | `FAIL_MAIN_GATE`（不再是 INCONCLUSIVE——主门已测） |
| PRODUCT_VERDICT | `HOLD_NO_REAL_APPLICATION`（#604 仍无 handoff；未变） |
| NOT_RUN | d4/d64、64K、threadpool、SMT/跨物理、饱和、RESOURCE-B、valgrind 斜率（主门 FAIL 后不再触发保护链） |
| BINS | W03 `bb142d69…`；W02 `5104ce25…`；lib `c7eed8da…`（与冻结逐位相同） |
| LOCK | 全部计时/诊断持 `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`（各块 header/block_end 内含 holders 记录） |
