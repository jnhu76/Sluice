# TAX-0B — Semantic-floor Z-ladder: build + first measurement

Issues: #250（execution truth）· #259（roadmap）· PR #260（evidence）
Preregistration: [`TAX0-A2-CONTROL-PLANE-SEMANTIC-FLOOR.md`](TAX0-A2-CONTROL-PLANE-SEMANTIC-FLOOR.md)
+ census `z_ladder_preregistration` / `seams[F05].z1b_design`（frozen, 未改写）
Canonical session: `results/tax0b-zladder-wsl2-formal4/`（60/60 combos OK，
notes.md 记录协议修正与观察；formal1–3 为原始历史，append-only）

# 1 Verdict

**TAX-0B COMPLETE — LADDER BUILT, SAME-WORK VALIDATED, SEMANTIC FLOOR MEASURED.**

- 一个 harness（`research/tax0/bench/tax0_z_ladder_bench.cpp`）实现五臂：
  **Z1** raw liburing bare floor、**Z1b** 冻结 F05 checklist 的显式机制版
  （semantic floor）、**Z1bw** Z1b + 单一 continuation consumer（reaper 线程 +
  per-slot terminal predicate 等待，lost-wake-safe）、**Z2** AsyncIoContext
  手动驱动（无 Scheduler）、**Z3** ApplicationRuntime（workers 1/4）。
- same-work 13 项冻结约束 fail-closed 强制（ops/bytes/word_sum 逐 rep 精确、
  跨臂 word_sum 相等校验、write 臂 runner 侧整文件逐块字节校验）。
- 量词固定：**capability_cost = Z1b−Z1 ≈ +37 instr/op 恒定**；
  **L1 abstraction_tax = Z2−Z1b ≈ +2015 instr/op 恒定**（4K/64K/1M 全部
  2015±17）；**runtime continuation = Z3w1−Z2 ≈ +787..863/op**。
  每处都按预注册词表标注，未使用被禁止的 Z3−Z1 伪量。
- 正确性调试期间修复的两个 harness 缺陷记录在案（append-only）：
  z1bw 跨 rep witness 累积导致 reaper 提前退出死锁；z2 write 臂缺失
  master refill。formal2/3 的 z1/z1b 双 enter 结构缺陷在 formal4 前修复。

# 2 Environment（ENVIRONMENT-LIMITED）

WSL2 kernel 6.18.33.2、8C AMD 5800H、WSL2 虚拟盘 ext4、tmpfs /tmp、
liburing 2.14、perf 7.0.12 **硬件计数器经校验循环验证为真**
（instructions/cycles/branches 计数相互独立）。分类
**QUALIFIED_BUT_VIRTUALIZED**：user-instruction 控制面归因有效；不声称
native NVMe 吞吐结论。PMU 主指标 4 事件一组（`:u`），无 multiplexing。

# 3 Protocol

- perf stat 整进程只能看到 setup/teardown → **double-difference**：
  每 combo 跑 R=7 与 R=14 两组（`--warmup 0`），每-rep 工作量 =
  `(total(14)−total(7))/7`，再 `/ops`——setup/teardown 被精确消去。
- write 臂验证外移 runner（`--runner-verify`），验证 pread 不进测量窗口；
  read 臂 word_sum 各臂 inline（shootout 先例，均匀 workload 分量）。
- lifecycle 分离：ring/context/runtime 构造、文件准备全部在测量 rep 之外。
- 复现性：stripped canonical binary 事后复测 read 4K d32 z2 = 3121 vs
  3112（+0.3%）；生产 bench 三个 session 同 cell = 3112/3112/3121。

# 4 Ladder（canonical formal4，instructions/op）

| cell (read) | Z1 | Z1b | Z1bw | Z2 | Z3w1 | Z3w4 |
| --- | --- | --- | --- | --- | --- | --- |
| 4K d1 | 1154 | 1201 | 1968 | 3827 | 5796 | 6647 |
| 4K d32 | 1043 | 1080 | 1290 | 3112 | 3899 | 3924 |
| 4K d64 | 1043 | 1080 | 1290 | 3212 | 4105 | 4138 |
| 64K d8 | 14499 | 14537 | 14783 | 16552 | 17380 | 17498 |
| 1M d8 | 229553 | 229590 | 230155 | 231605 | 232468 | 233416 |

write 臂同构（例：4K d32 Z1 626 / Z1b 665 / Z2 2843 / Z3w1 3739）。
完整表 + wall/user/sys 见 session summary.json / summary.csv；30 个
canonical validator 绑定 artifacts（kind `perf`）在
`docs/results/performance-attribution/tax0z-*.json`（P1 cells ×
{z1b,z2,z3w1} × read/write）。

# 5 Regime structure（禁止折叠成单标量）

- **small+shallow/concurrent（4K）**：control plane 主导——Z2 的每-op
  用户指令是 semantic floor 的 ~2.9 倍；+2015/op 为固定控制面工作。
- **medium（64K）/large（1M）**：data plane 主导——+2015/op 绝对值不变，
  相对值降到 ~12% / <1%。capability cost（+37/op）在所有 regime 都 <0.3%。
- **wall 与 instructions 分歧**（4K d≥8 时 z2 wall 可低于 z1/z1b）：
  本虚拟介质下 wall 由内核 medium 主导；控制面结论只以 instructions/op
  为据，wall 数字如实并列报告。

# 6 Historical cliff（4K d≥32 w4）re-judgment

**NOT REPRODUCED**（uring Z-ladder path，本环境）：z3w4 vs z3w1 在
4K d32 = +0.6% instructions / +40% wall；4K d64 = +0.8% / +54%——随
depth 平滑、无阈值突变。范围注记：历史 #221 证据是 ThreadPool backend；
本轮只对 campaign 目标路径（uring）与本环境做再裁决。

# 7 Observations（详见 session notes）

- **OBS-1（疑似生产缺陷，非税发现）**：z3w4 write 间歇出现
  spurious `canceled` 终态（IoError code 1，os_errno=125/EAGAIN，
  harness 从未请求取消）+ 间歇 teardown abort（perf 包装下）。standalone
  11/12、5/5 通过 vs session 内 bounded retry 全失败。最小恢复：该 cell
  去 perf 包装测量（wall/user/sys + same-work；instr/op = NOT RUN）。
  建议在 #250 立后续票根因。
- **OBS-2**：4K d1 write≫read wall 不对称（39–70µs vs 1.6µs，d≥8 消失）
  ——v1.1 UNKNOWN 以 ENVIRONMENT-SPECIFIC-until-native 顺延。

# SKIPPED

- 完整矩阵扩展（4K d8、64K d1/d32、1M d1/d32）——P1 代表 cell 已给出全部
  结构结论，预算留给因果消融（P2）；
- Z0 pread/pwrite 臂——预注册优先序 Z1>Z1b>Z2>Z3，Z0 为附加 floor 非分母；
- Z4 composition check——TAX-0 范围外。
