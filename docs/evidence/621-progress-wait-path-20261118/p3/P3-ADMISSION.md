# P3-ADMISSION — #621 W1–W6 与 #637 历史候选准入裁决

| 字段 | 值 |
|---|---|
| 角色 | 准入裁决官（只读证据；只写 `p3/`；不修改产品代码） |
| 日期 | 2026-11-18 |
| 战役 BASE | `5d94f233967c3aef993eb19ba9e3252c532b0951` |
| 性质 | **准入裁决**。不是测量、不是 A/B、不是合并授权；不替代 `docs/explicit-io-contract.md`、`docs/roadmap/v2-conformance.md` 或 `CAMPAIGN-LEDGER.md` |
| 输入 | `CAMPAIGN-LEDGER.md` §4/§5/§9；`p2/P2-COST-MAP-R1.md`；`p3/W1-DESIGN.md`；`p1/AA-CALIBRATION.md`；`p1/AA-CALIBRATION-2.md`；`p2/P2-REVIEW.md` |
| 本轮执行 | 只读复算与源码抽查（见 §7）：P2 R1 复算脚本、P1/P2 A/A 复算脚本、required-n 算术、M-42/W5/W6/W1 源码坐标 |
| 本轮未运行 | `xmake` 构建/测试、任何新的 perf/bench/A/A/A/B、TLC、sanitizer、完整矩阵 |

> **一句话结论**：**本战役没有候选达到 `ADMIT_TO_AB`**。W1 的唯一 material 方向是 `SEMANTIC_MISMATCH`，hygiene 方向是 `NO_CHANGE_JUSTIFIED`；W3 与 W4 只到 `ADMIT_TO_DESIGN`（行级归因/迁移义务/隔离协议）；M-42 `NOT_POWERED`；W2/W5 与 M-28 `NOT_APPLICABLE`；#637 的 `NO_GO`/`NO_GO_ABORT`/`SEMANTIC_MISMATCH` 终态全部维持，**没有任何翻案条件被满足**。P2 profile 是形态级 hotspot 地图，不是候选级准入证据。

---

## 0. 裁决摘要

### 0.1 本矩阵覆盖与计数

本文件覆盖：

- **W1–W6 六个工作包**（看板见 §2）；
- **W1 的七个方向行**：D1/D2/D3 阻塞 read、C1 第二等待 fd、C2 condvar 双通道、A1/A2/A3 hygiene、A4/A4' probe、A5–A8 删除重验/drain/P3/后检查、B 有界等待；
- **W3 的五个行级候选**：M-61 `begin`/`complete`、M-62、M-65 owed、M-67、M-10；
- **W4/W6 的五个候选**：M-53、M-40、M-63、M-42、M-28；
- **#637 其余 15 条**：M-80 `write_all_at`、M-16、M-37、M-01、M-33、M-74、M-09、M-30、M-57、M-12、M-61 payload、M-80 hygiene、H1、io-wq punt、tp-w8。

共 38 行（W1–W6 包 6 行 + W1 方向 7 行 + W3 个体 5 行 + W4/W6 个体 5 行 + #637 其余 15 行）。

| 裁决 | 计数 | 说明 |
|---|---:|---|
| `ADMIT_TO_AB` | **0** | 没有候选同时满足五条件 |
| `ADMIT_TO_DESIGN` | 11 | W3 包、W4 包、M-62、M-65、M-67、M-10、M-53、M-40、M-63、M-33、M-74 |
| `SEMANTIC_MISMATCH` | 12 | W1 包、W1-D、W1-C1、W1-C2、W1-A4、W1-A5–A8、M-16、M-09、M-30、M-57、M-12、M-80 hygiene |
| `NO_CHANGE_JUSTIFIED` | 2 | W1-A1/A2/A3、W1-B 有界等待 |
| `NOT_APPLICABLE` | 3 | W2、W5、M-28 |
| `NOT_POWERED` | 2 | W6 包（M-42）、M-42 |
| `INCONCLUSIVE` | 4 | M-61 `begin`/`complete`、M-80 `write_all_at`、M-37、M-01 |
| `NO_GO` | 4 | M-61 payload、H1、io-wq punt、tp-w8 |

> 计数口径：W3/W4/W6 包与其成员候选分别列出；W6 包的 `NOT_POWERED` 指 M-42，M-28 单列为 `NOT_APPLICABLE`。包裁决不等于成员候选自动获得同一裁决。计数按**主裁决**：W1 包与 W1-A4 的 TP-only 子方向另有 `NO_CHANGE_JUSTIFIED` 说明，不重复计入上表。

### 0.2 五个最重要的结论

1. **W1 不是“还没测”，而是唯一可能 material 的方向在当前 root/拓扑下语义不成立。** 同一 eventfd `dup` 后改 blocking 会共享 open-file description 与文件状态标志（`p3/W1-DESIGN.md:267-293` 的 `os.dup`+`F_SETFL` 复现）；同 fd 直接改 blocking 破坏非阻塞 ack/drain、有界等待与 PROG-02 的 stale-readiness 顺序（`:298-306`）；独立 blocking fd 与 io_uring 单 eventfd 注册冲突，需 relay/第二通知权威（`:308-314`）。语义保持的 A1/A2 只删 1–2 次 mutex，预计低于 C6/C1 噪声带（`:142-170,200-214`）。
2. **W3 只有族级/阶段容器，没有单候选 measured hotspot。** P2 的 publication 4.13%（P-D）、terminal 3.49%（P-D）、progress/wait 25.09%+9.16%（P-B）都是混合容器（`p2/P2-COST-MAP-R1.md:472-497`）；P-B 的 0.24% 族级数字因 **90.67% app 样本无 bench 帧**而 `NOT_OBSERVABLE`（`p2/P2-REVIEW.md:54-71`；本会话复算 `app_frames` 12,246/11,104/1,142）。M-61 还有 r6 R3 的 0.45% < 1% MME 反例（`CAMPAIGN-LEDGER.md:222`），P2 的阶段容器不能推翻它。
3. **M-53 的 d16 热点真实但很小；d64 仍未达标。** d16 leaf self 0.406%（P-A）/0.285%（P-C），约 61.3/58.0 cycles/op（`p2/P2-COST-MAP-R1.md:499-511`；本会话复算一致）。d64 C7 cycles 16 对 `t-hw=1.007%`，刚好超过 2%/2=1.0%，需 17–20 对；三项迁移义务（TransportLedger 位置单调、控制 CQE exact-submitted、cancel 定向）仍未证（`CAMPAIGN-LEDGER.md:211`；`p1/AA-CALIBRATION-2.md:485-506`）。
4. **M-42 没有 measured hotspot，且算术上界低于噪声带。** `submit_flushes_` 编译为 `lock addq $0x1,0x120(%rdi)`（`src/async/uring_transport.cpp:222`）；按 19–20 cycles 为 P-A 0.126–0.133%、P-B 0.066–0.070%、P-C 0.093–0.098% cycles（`p2/P2-COST-MAP-R1.md:513-537`），1 条 instruction/submit 仅 0.0041–0.0094%。#637 的 `GO` 只是“排队、未启动”，不是本战役的 A/B 准入（`CAMPAIGN-LEDGER.md:243-247`）。
5. **#637 的终态没有被本轮 profile 推翻。** 所有 `NO_GO`/`NO_GO_ABORT` 的翻案条件都未满足；`SEMANTIC_MISMATCH` 仍是 owner 裁决面，不应被包装成性能实验（`CAMPAIGN-LEDGER.md:191-203,243-247`）。P2 的形态级热点只够作 #617 trigger-grade 线索，不能作 #621 的 A/B 证据（`p2/P2-REVIEW.md:322-331`）。

### 0.3 证据口径（贯穿全文）

- **份额 ≠ 可删 cycles**；**阶段容器 ≠ 候选上界**；**inclusive 不能相加，也不能当收益**（`p2/P2-COST-MAP-R1.md:82-94,341-357`）。
- **profile 只证明 bench/reference 形态**；`sluice-copy` 是参考应用，不是 #604 G0 真实 workload（`CAMPAIGN-LEDGER.md:305-315`；`p2/P2-REVIEW.md:239-247`）。
- **A/A 只能回答“若效应是 X，需要多少对”**，不能回答“效应是 X”；匹配格不能跨形态搬运（`p1/AA-CALIBRATION.md:384-402`；`p1/AA-CALIBRATION-2.md:393-419`）。
- **owner 裁决不是技术结论**。本文件把需要 owner 决定的事项单列在 §5，不替 owner 做选择，也不把“尚未裁决”写成“已证明可行”。

---

## 1. 裁决规则与证据门槛

### 1.1 `ADMIT_TO_AB` 的五条件

按 ledger §5.2 与 #621 纪律，候选必须**同时**满足：

1. **合法机制**：不违反 root 的 PROG-01–04、CANCEL-01、BACKEND-01 等语义，且不需要未裁决的拓扑变更；
2. **候选级 measured hotspot（或等价证据）**：有候选专属的 measured 份额/计数/隔离格证据，而不是族级线索或阶段容器；
3. **语义可证**：有确定性 oracle、mutant、协议/模型证明或可执行的步间语义映射；
4. **A/A 分辨率足以在预算内分辨预注册 MME**：匹配格、正确指标、预注册 δ，所需对数在预算内；
5. **无历史反例**：没有已归档的 `NO_GO`/`NO_GO_ABORT` 或被推翻的候选级测量。

不满足第 1/3 条且属于语义冲突或 owner 未裁决 → `SEMANTIC_MISMATCH`；需要先做设计/语义/隔离格/行级归因 → `ADMIT_TO_DESIGN`；无 measured hotspot 或低于噪声带 → `NOT_POWERED`/`NO_CHANGE_JUSTIFIED`；已被合并/删除/非本战役 → `NOT_APPLICABLE`；证据不足且没有已定义的设计门 → `INCONCLUSIVE`。

### 1.2 “候选级 measured hotspot”的最低口径

本裁决只接受以下之一作为候选级证据：

- 候选专属的 leaf/inclusive 份额，且能从源码坐标说明“删掉什么机器工作”；
- 候选专属的 tracepoint/uprobe 计数，且能给出每 op/chunk 的归因；
- 隔离格（submit-only/zero-op/cancel/d64 等）的 measured 比例，且带完整 oracle 与 A/A；
- 受控的机制模型 + 生产路径映射（仅当无法直接采样时，且必须说明映射假设）。

**不接受**：阶段容器（`progress/wait`、`publication`、`terminal/release`）、族级 mutex 线索（~9–11%）、inclusive 份额（`__poll` 24.93% 等）、无候选专属分母的 “payload/FS” 桶、无生产读者的测试计数。上述只能作**定位线索**，不能作准入上界。

### 1.3 A/A 分辨率（本会话独立复算）

以下数字来自本会话对归档原始数据的只读复算（命令与输出见 §7），与 `AA-CALIBRATION*.md` 的发布值一致：

| 格 | 匹配形态 | 指标 | n | SD (pp) | hw (%) | MDE80 (%) | 状态 | 本战役用途 |
|---|---|---|---:|---:|---:|---:|---|---|
| C1 | `sluice-copy 4k/d4/w1`（P-E） | CPU | 16 | 0.786 | 0.365 | 0.589 | **QUIET** | P-E W1；sluice-copy 参考应用 |
| C1 | 同上 | cycles | 16 | 2.346 | 1.121 | 1.758 | **QUIET** | P-E cycles |
| C6 | `W02-uring-4k-d1`（P-B） | CPU | 16 | 0.997 | 0.471 | 0.747 | **ORDER_SENSITIVE（仅 wall）** | P-B CPU |
| C6 | 同上 | cycles | 16 | 2.876 | 1.368 | 2.155 | **ORDER_SENSITIVE（仅 wall）** | P-B cycles |
| C3b | `W02-uring-4k-d16`（P-A） | cycles | 16 | 1.319 | 0.621 | 0.988 | **ENVIRONMENT_BLOCKED（CPU/wall）；cycles 条件带** | W3/M-53 d16 |
| C4 | `W03-uring-4k-d16`（P-C） | CPU | 16 | 3.019 | 1.402 | 2.262 | **QUALIFIED（≥~2.3% CPU）** | W3 |
| C5b | `W02-threadpool-4k-d1-w1` | CPU | 16 | 0.964 | 0.458 | 0.723 | **QUIET（条件基线）** | W3 TP d1 w1 |
| C5b | 同上 | cycles | 16 | 3.107 | 1.481 | 2.328 | **不作窄带** | 不用于 W1/W3 |
| C7 | `W02-uring-4k-d64` | cycles | 16 | 1.889 | 0.893 | 1.416 | **ENVIRONMENT_BLOCKED（CPU/wall）；cycles 条件带** | M-53 d64 |
| C7 | 同上 | CPU | 16 | 9.782 | 4.573 | 7.331 | **ENVIRONMENT_BLOCKED；A/A 均值不正** | 不用于 M-53 |
| C2b | `W02-threadpool-4k-d16-w4` | CPU | 16 | 3.414 | 1.604 | 2.558 | **NOISY** | 不用于 W3 |
| C8 | `W02-threadpool-4k-d64-w4` | CPU | 16 | 4.938 | 2.323 | 3.700 | **NOISY** | 不用于 M-53 |

来源：`p1/AA-CALIBRATION.md:384-402`；`p1/AA-CALIBRATION-2.md:393-419`；本会话 `tools/recompute_independent.py --aa-root p1/aa|p1/aa2` 复算（§7）。

### 1.4 预算内的 required-n（本会话算术）

| 格/指标 | δ=5% n80/n_half | δ=2% | δ=1.2% | δ=1% | 36 对 MDE80 / hw(t) |
|---|---:|---:|---:|---:|---:|
| C6 CPU | 3/3 | 5/7 | **8/14** | 10/18 | 0.479% / 0.337% |
| C6 cycles | 5/8 | 19/35 | **48/91** | 67/130 | 1.382% / 0.973% |
| C1 CPU | 3/3 | 4/5 | **6/10** | 7/12 | 0.378% / 0.266% |
| C1 cycles | 4/6 | 13/24 | **32/62** | 46/88 | 1.127% / 0.794% |
| C5b CPU | 3/3 | 5/7 | **8/13** | 10/17 | 0.463% / 0.326% |
| C3b cycles | 3/4 | 6/10 | **12/22** | 16/30 | 0.634% / 0.446% |
| C7 cycles | 4/5 | **10/17** | 22/41 | 30/58 | 0.907% / 0.639% |
| C4 CPU | 6/9 | 20/38 | 52/100 | 74/143 | 1.450% / 1.021% |

读法：`n80` = 80% 功效所需对数；`n_half` = 95% CI 半宽 ≤ δ/2 所需对数。**这些数字只回答“若效应是 X 需要多少对”**；它们不证明任何候选的效应是 X。

### 1.5 历史反例的效力

- **M-61 `begin`/`complete`**：r6 R3 已给出 0.45% < 1% MME 的候选级拒绝（`CAMPAIGN-LEDGER.md:222`）。P2 的 publication 阶段 4.13% 是容器，测量范围不同，不能推翻 0.45% 的候选级结果（`p2/P2-COST-MAP-R1.md:598`）。
- **M-28**：`NO_GO_ABORT`，预注册中止通道判词 1b/1c 已落地（`CAMPAIGN-LEDGER.md:233-241`）。仅“再测一次”或“换 workload”不构成翻案。
- **M-42**：`GO` 只是排队，不是已准入性能门；历史 `NOT_POWERED` 结论未被 P2 的 `lock addq` 算术推翻（`CAMPAIGN-LEDGER.md:217,247`；`p2/P2-COST-MAP-R1.md:513-537`）。

---

## 2. W1–W6 看板更新

| 工作包 | 名称 | 阶段 | 终判/当前裁决 | 一句话说明与下一步 |
|---|---|---|---|---|
| **W1** | ProgressSource poll 等待路径 | **终判** | **`SEMANTIC_MISMATCH`（material）/ `NO_CHANGE_JUSTIFIED`（hygiene）** | D1/D2/D3/C1 语义阻断；A1/A2 不 material；#604 G0 未过；等 owner 裁决代理/拓扑，不启动 A/B |
| **W2** | 通知/drain/rearm | **终判** | **`NOT_APPLICABLE`** | #635 已合并；无新增候选；未来改动保持 9 用例语义 oracle + mutant + 模型 |
| **W3** | RequestCore 行级族 | **语义验证中** | **`ADMIT_TO_DESIGN`** | 先做行级 measured 归因（release stripped/无 DWARF）；M-61 R3 反例未推翻；预算 M |
| **W4** | UringAsyncBackend | **语义验证中** | **`ADMIT_TO_DESIGN`** | M-53 先证三项迁移义务 + d64 17–20 对 cycles-only A/A；M-40/M-63/M-65 设计门；预算 M/L |
| **W5** | ThreadPool owed-count | **终判** | **`NOT_APPLICABLE`** | #634 已合并；任何改动重跑 parity 8 用例 + 4 mutants + 完整矩阵 |
| **W6** | test-only 计数 / owed 计数 | **终判** | **M-42 `NOT_POWERED`；M-28 `NO_REDO`；M-40 转 W4** | M-42 的 `GO` 是排队，不是 A/B 准入；隔离格 + 动态回执带 + 机制门前不启动 |

**看板状态说明**：本战役没有工作包进入 `已准入`、`A/B 测量中` 或 `独立复核中`。W3/W4 在 `语义验证中`（设计/资格阶段），W1/W2/W5/W6 已到 `终判`（但 W1 的 owner 裁决点仍开放）。

---

## 3. 逐候选矩阵

### 3.1 W1 方向

| 方向 | 裁决 | 证据 | 缺什么 / 翻案条件 | 下一步 | 预算 |
|---|---|---|---|---|---|
| **W1-D1/D2/D3 阻塞 eventfd read** | **`SEMANTIC_MISMATCH`** | `p3/W1-DESIGN.md:267-324`；D1 `os.dup`+`F_SETFL` 输出 `after clear on dup: original False dup False`（`:275-293`）；D2 非阻塞 ack/drain、有界 timeout、PROG-02 第 2 条（`:298-306`）；D3 io_uring 单 eventfd 注册（`src/async/uring_backend.cpp:273-278`；`include/sluice/async/detail/progress_source.hpp:539-545` 禁止 dup/store） | 不是性能缺口；要 material 等待路径必须改通知拓扑/模型/第二权威，需 owner 批准 | 不实现；若 owner 要追，另立拓扑 PR，归 #617 G1 或新模型 | 0 A/B |
| **W1-C1 第二等待 fd** | **`SEMANTIC_MISMATCH`** | `p3/W1-DESIGN.md:236-243`；kernel 只唤醒被注册 fd；host/owner 必有一方收不到 CQE；relay/第二权威违反 PROG-01/02 | 同上 | 不实现 | 0 |
| **W1-C2 TP condvar + protected predicate** | **`SEMANTIC_MISMATCH`（owner 未裁决）** | `p3/W1-DESIGN.md:245-256`；PROG-02 原则允许 condvar（`docs/explicit-io-contract.md:907-912`），但需要双通道、新模型、新 oracle；eventfd 仍是 host pollable handle | owner 是否授权第二通知通道；模型/协议/oracle/机制门七问 | 先 owner；若批准，按 §6 的 M/L 设计包执行 | M/L |
| **W1-A1/A2/A3 hygiene** | **`NO_CHANGE_JUSTIFIED`** | `p3/W1-DESIGN.md:142-170,200-214`；合并入口两次 `access_mtx_`、health 并入 fused block、deadline 算术；只删 1–2 次 mutex/少量 clock/int，不删 syscall/kernel work | material 证据；预计低于 C6/C1 MDE（C6 CPU 0.747%、C1 CPU 0.589%，16 对） | 可选代码面 hygiene PR；不得开 A/B 声称收益 | S（代码面） |
| **W1-A4/A4' probe 降频** | **`SEMANTIC_MISMATCH`（全局）/ `NO_CHANGE_JUSTIFIED`（TP-only）** | `p3/W1-DESIGN.md:172-177`；probe 是 io_uring kernel writer 的兜底（CQE 不 bump epoch）；TP-only 跳过需新能力事实 | TP-only 的能力事实与机制门；收益仅一个虚调用 | 不全局降频；TP-only 仅 hygiene | S |
| **W1-A5–A8 删除重验/drain/P3/后检查** | **`SEMANTIC_MISMATCH`** | `p3/W1-DESIGN.md:179-198`；PROG-02 第 2/4 条；model `OL4a`/`OP3`/`MutParkDrain` | 无；删除即丢唤醒/破坏 control sticky | 不做 | 0 |
| **W1-B 有界等待算术** | **`NO_CHANGE_JUSTIFIED`** | `p3/W1-DESIGN.md:218-228`；poll 仍在；P-B 本身是 20 s 有界等待（`bench/bench_w02_pipeline.cpp:52`）；只删 clock/int | material 效应；不能删 poll/kernel wait | 不单独作为候选 | 0 |

### 3.2 W3 行级候选

| 候选 | #637 原终态 | P3 裁决 | 证据 | 缺什么 / 翻案条件 | 下一步 | 预算 |
|---|---|---|---|---|---|---|
| **M-61 `begin`/`complete` 两锁融合** | `INCONCLUSIVE` | **`INCONCLUSIVE`**（原判未推翻） | `p2/P2-COST-MAP-R1.md:486-497`；publication 1.46/0.07/2.82/4.13/3.30% 是阶段容器；`claim_execution` leaf 93/93 已归 publication；r6 R3 0.45% < 1% MME 拒绝（`CAMPAIGN-LEDGER.md:222`） | 新 measured profile 推翻 R3（行级分离 + `publication_inflight` 反例处理）；融合的步间语义 | 新 profile + 语义映射；不启动 A/B | M |
| **M-62 无观察者跳过 claim/retire** | `INCONCLUSIVE` | **`ADMIT_TO_DESIGN`** | `p2/P2-COST-MAP-R1.md:486-497`；P-D publication 4.13% 容器；claim leaf 0.61% 内含；P-B 的 0.07% publication 是阶段容器且同样受用户栈截断影响，不能当候选成本（90.67% app 无 bench 帧，`p2/P2-REVIEW.md:54-71`） | 行级分离；publication exactly-once/observer 注册协议重证；单因素 A/B | 行级归因 + 协议证明；然后 A/B | M |
| **M-65 owed `release_control` 第三锁融合** | `INCONCLUSIVE` | **`ADMIT_TO_DESIGN`** | `p2/P2-COST-MAP-R1.md:490-497`；terminal/release 3.49% P-D 容器；P2 无 zero-op/owed workload；owed 机制本体 KEEP（`CAMPAIGN-LEDGER.md:212`） | zero-op/owed 协议（T9）；第三锁与 retire 融合的步间语义；交付顺序不变性 | T9 owed 协议 + 设计；然后 A/B | M |
| **M-67 sticky 双标志原子化/融合查询** | `INCONCLUSIVE` | **`ADMIT_TO_DESIGN`** | `p2/P2-COST-MAP-R1.md:491-497`；progress/wait 25.09%+9.16% P-B 含 2,057 个 worker-eventfd；sticky flags 无独立可采样符号（`p2/P2-REVIEW.md:128-142`） | 行级 profile 分离；A/A 分辨率 ≥1% MME；ProgressSource 单读者协议重证 | 行级 profile + cycles-only A/A + 协议 | M |
| **M-10 零长发布三锁融合** | `INCONCLUSIVE` | **`ADMIT_TO_DESIGN`** | `p2/P2-COST-MAP-R1.md:492-497`；zero-op 不在 W-02/W-03 workload；M-10 `NOT_OBSERVABLE`（`CAMPAIGN-LEDGER.md:221`） | zero-op 占比测量；步间语义映射；融合单因素 A/B | T9 zero-op 协议 + 测量；然后 A/B | M |

### 3.3 W4/W6 候选

| 候选 | #637 原终态 | P3 裁决 | 证据 | 缺什么 / 翻案条件 | 下一步 | 预算 |
|---|---|---|---|---|---|---|
| **M-53 cookie O(R) → `user_data` 路由索引** | `INCONCLUSIVE` | **`ADMIT_TO_DESIGN`** | d16 leaf self 34/8,369=0.406%（P-A）、47/16,472=0.285%（P-C），≈61.3/58.0 cyc/op（`p2/P2-COST-MAP-R1.md:499-511`）；d64 C7 cycles SD 1.889、16 对 `t-hw=1.007%` > 1.0%，需 17–20 对（`p1/AA-CALIBRATION-2.md:485-506`）；三项迁移义务未证（`CAMPAIGN-LEDGER.md:211`） | d64 cycles-only A/A 17–20；TransportLedger 物理位置单调；控制 CQE exact-submitted；cancel 定向；d64 profile | 先证三项义务 + d64 A/A；再 17–20（δ=2%）或 58（δ=1%）A/B | M/L |
| **M-40 `live_cookies_`/`live_control_sqes_` 原子冗余** | `INCONCLUSIVE`（r2 由 `NO_GO` 改判） | **`ADMIT_TO_DESIGN`** | `CAMPAIGN-LEDGER.md:218,262`；P2 未单独观测 live counters（inline 原子无可采样符号）；需要行级归因 + 生产读者角色 + `dispatch_mtx_` 覆盖重证 + 缝读者加锁设计 | 行级 profile；派生摘要/机制门作答；`dispatch_mtx_` 覆盖证明；缝读者锁设计 | 设计审计 + 行级 profile；然后 A/B | M |
| **M-63 uring owed sweep 逐槽锁 → 单锁快照** | `INCONCLUSIVE` | **`ADMIT_TO_DESIGN`** | `CAMPAIGN-LEDGER.md:212,274`；P2 无 zero-op/owed workload；owed 机制本体 KEEP；交付顺序不变性未证 | 零长/owed 协议激活（T9）；单锁快照的顺序/原子性设计 | T9 owed 协议 + 融合设计；然后 A/B | M |
| **M-42 `submit_flushes_` 只写不读原子计数** | `GO`（排队，未启动） | **`NOT_POWERED`** | `src/async/uring_transport.cpp:222` 的 `fetch_add` 编译为 `lock addq $0x1,0x120(%rdi)`（objdump，`p2/P2-COST-MAP-R1.md:518-523`）；19–20 cycles → P-A 0.126–0.133%、P-B 0.066–0.070%、P-C 0.093–0.098% cycles；1 instr 0.0041–0.0094%；P-D/P-E 的 workload 不执行（P-D 仅 1 次启动残差，`p2/P2-COST-MAP-R1.md:513-537`） | measured hotspot；隔离格（submit-only）+ 动态回执带 + 机制门七问；`GO` 排队 ≠ A/B 准入 | owner 决定是否资助隔离格；否则保持 `NOT_POWERED` | S（A/B）+ 隔离格 |
| **M-28 `syscall_count_` 宏门控剔除** | `NO_GO_ABORT` | **`NOT_APPLICABLE`（不重做）** | `CAMPAIGN-LEDGER.md:233-241`；预注册中止通道判词 1b/1c 已落地；无新的可分离“单指令差”证据；P2 未重开 | 翻案三条件：隔离 TU/对象布局变化、RMW 移除的锁竞争时序、新 session/新冻结 + 动态回执接受带 | 不启动；仅三条件同时满足才重开 | 0 |

### 3.4 #637 其余候选

| 候选 | #637 原终态 | P3 裁决 | 证据 | 缺什么 / 翻案条件 | 下一步 | 预算 |
|---|---|---|---|---|---|---|
| **M-80 `write_all_at` → `prevalidated_pwrite_step`** | `INCONCLUSIVE` | **`INCONCLUSIVE`** | `CAMPAIGN-LEDGER.md:208,280`；P2 `payload/FS` 10.44–19.87% 不是候选专属；无直呼组合写的 measured hotspot | 直呼组合写形态的 measured hotspot；“compose 仅由已校验入口进入”不变量 | 定向 profile + 不变量证明；然后 A/B | M |
| **M-16 uring 后端层 state-op 校验收敛** | `INCONCLUSIVE` | **`SEMANTIC_MISMATCH`** | `CAMPAIGN-LEDGER.md:209,282`；owner seam 冲突未裁决 | owner 裁决 seam；隔离 backend state-op 半边 | 先 owner；再隔离 A/B | S/M |
| **M-37 `remove_exact(front)` → `pop_front`** | `INCONCLUSIVE`（#595） | **`INCONCLUSIVE`（#595 归属；本战役不启动）** | `CAMPAIGN-LEDGER.md:210,279`；T4/T8 不在 G0 冻结集；无深队列 `remove_exact` 的 memmove 份额 | 深队列派发形态的 memmove 份额，或证明生产全形态队列深 ≤1（→ `NO_GO`） | #595 深队列 profile；否则塌缩 `NO_GO` | M（#595） |
| **M-01 scope O(S) 扫描，H3 开放** | `INCONCLUSIVE` | **`INCONCLUSIVE`** | `CAMPAIGN-LEDGER.md:220,281`；无 `-scope64` 高重复隔离 profile；free-list 是否重开属 owner（S2） | 隔离协议剔除驱动污染；机制可归因份额；owner S2 | 高重复 `-scope64` 隔离 profile；owner S2 | M |
| **M-33 TP cancel ring 摘除惰性失效** | `INCONCLUSIVE` | **`ADMIT_TO_DESIGN`** | `CAMPAIGN-LEDGER.md:226,276`；W-02/W-03 不主动 cancel；T9-cancel 未构造 | cancel 形态 measured hotspot；失效机制设计（两语义保持）；机制正当性门 | T9 cancel 协议 + 设计；然后 A/B | M |
| **M-74 `ready()` 快路径合并** | `INCONCLUSIVE` | **`ADMIT_TO_DESIGN`** | `CAMPAIGN-LEDGER.md:227,277`；ready 概率未分层；与 zero-op 子单元绑定 | ready 概率分层测量；合并收益隔离证据 | T9 ready/zero-op 测量 + 设计 | S/M |
| **M-09 `Slot::borrow` 只写不读** | `SEMANTIC_MISMATCH` | **`SEMANTIC_MISMATCH`** | `CAMPAIGN-LEDGER.md:213,283`；root 的 borrow facts 义务 | owner 裁决：修 root 摘除 borrow facts（→ 删除 PR）或出现真实消费者（→ KEEP） | owner；代码面 PR | S（无性能门） |
| **M-30 `zero_effect_cancel` 零生产者** | `SEMANTIC_MISMATCH` | **`SEMANTIC_MISMATCH`** | `CAMPAIGN-LEDGER.md:214,283`；CANCEL-01 seam 义务 | owner 裁决 seam 义务（KEEP）或修 root 去掉冗余第二强制点 | owner；代码面 PR | S |
| **M-57 `acquire_execution` 零调用** | `SEMANTIC_MISMATCH` | **`SEMANTIC_MISMATCH`** | `CAMPAIGN-LEDGER.md:215,283`；BACKEND-01 seam | owner 裁决 seam 必要性；若可删走代码面收缩 PR | owner；代码面 PR | S |
| **M-12 `close_admission` 双实现** | `SEMANTIC_MISMATCH` | **`SEMANTIC_MISMATCH`** | `CAMPAIGN-LEDGER.md:216,283`；双实现 | owner 指定存活实现 + 测试 oracle 迁移方案 | owner；代码面 PR | S |
| **M-61 `PublicationPayload` 快照仅测试可达** | `NO_GO` | **`NO_GO`** | `CAMPAIGN-LEDGER.md:219,284`；无生产 payload 消费者 | 生产消费者出现（→ 重新物化）或 owner 批准测试专用通道（卫生路线） | 不复活为性能候选；卫生路线另议 | N/A |
| **M-80 hygiene `file_semantics` 模型词表位置** | `SEMANTIC_MISMATCH` | **`SEMANTIC_MISMATCH`** | `CAMPAIGN-LEDGER.md:228,283`；词表位置与测试面 | owner 裁决词表移入测试面（保持共享语义 oracle）或出现生产消费者 | owner；代码面 PR | S |
| **H1 提交批形态** | `NO_GO` | **`NO_GO`** | `CAMPAIGN-LEDGER.md:229,285`；P2 现状 `io_uring_enter` 1/op、submit 97–99% kernel、ring flags=0（`p2/P2-COST-MAP-R1.md:539-542`） | #595 RQ1 second-held-entry guard + payload gate + D1/D2/D3、64 KiB、W-03、cancel falsifier 家族 + 整机重测 | #595 重开门前不启动 | N/A（#595） |
| **io-wq punt FS 条件性** | `NO_GO` | **`NO_GO`** | `CAMPAIGN-LEDGER.md:230,285`；P2 tmpfs `queue_async_work`=1/op、stage 6.02–10.23%（`p2/P2-COST-MAP-R1.md:539-542`）；非 Sluice 代码 | 无翻案路径；换设备/FS 的产品准入属既列重测情形 | #604/#617；不包装成跨 FS | N/A |
| **tp-w8 `work_mtx_` 竞争（worker 超配）** | `NO_GO` | **`NO_GO`** | `CAMPAIGN-LEDGER.md:231,285`；#604 配置/工作负载面；唯一减法交集 M-28 已 `NO_GO_ABORT` | 出现 worker 数被外部固定的 root 工作负载 + 最小机制提案过机制正当性门 | 单独预注册；本战役不启动 | N/A（#604） |

---

## 4. 关键裁决理由

### 4.1 W1：为什么不是 `ADMIT_TO_DESIGN`，也不是“还没测”

**唯一可能 material 的方向被语义阻断。** W1 的历史线索是“`poll(-1)` 可由同一 eventfd 的阻塞 read 取代，约 5.0–5.5% cycles 可删”（W1-DESIGN 引用的 R1 工作稿线索；未跟踪、已被 R2 r6 取代，`p3/W1-DESIGN.md:324`）。W1-DESIGN 逐一检查了 A/B/C/D 四个方向（细分见 §3.1）：

- **D1（同 fd + dup + `F_SETFL`）**：`dup` 共享 open-file description 与文件状态标志；本会话用 eventfd 复现 `after clear on dup: original False dup False`（`p3/W1-DESIGN.md:275-293`）。源码只创建一次 `eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC)`（`include/sluice/async/detail/progress_source.hpp:74-79`），且 `BackendProgressPort` 明确禁止 backend dup/store fd（`:539-545`）。判 `SEMANTIC_MISMATCH`。
- **D2（同 fd 直接改 blocking）**：`drain_notification_nolock_`（`:414-460`）在无 readiness 时 read 会永久阻塞；`acknowledge_notification` 与非阻塞 ack/drain 依赖 `EFD_NONBLOCK`；有界等待（P-B 的 20 s，`bench/bench_w02_pipeline.cpp:52`）需要 timeout；PROG-02 要求 stale readiness 在最终 actionable-work check 之前确认，blocking read 是“park 时消费”。判 `SEMANTIC_MISMATCH`。
- **D3（独立 blocking fd）**：io_uring 只把 kernel CQE readiness 写给被注册的一个 fd（`src/async/uring_backend.cpp:273-278`）；host 的 pollable handle（PROG-03）与 owner 的 blocking wait fd 若不同，必有一方收不到 CQE；relay/第二通知权威违反 PROG-01/02。判 `SEMANTIC_MISMATCH`。
- **C2（TP condvar + predicate）**：契约原则允许（`docs/explicit-io-contract.md:907-912`），但这是双通知通道/新模型/新 oracle，且 host 仍要 eventfd；不是 W1 的最小 B。判 `SEMANTIC_MISMATCH`（owner 未裁决）。

**语义保持的 A1/A2 不 material。** 合并 `wait_one` 入口两次 `access_mtx_`、把 health 检查并入 fused block，只删每 park 约 1–2 次 mutex 获取；不删 syscall、不删 kernel work（`p3/W1-DESIGN.md:142-170`）。16 对 C6 CPU MDE80 0.747%、C1 CPU 0.589%；36 对约 0.479%/0.378%。这些锁操作的效应预计低于噪声带，不能支撑 A/B 声明（`:407`）。判 `NO_CHANGE_JUSTIFIED`。

**profile 没有给出可删量。** P-B `__poll` inclusive 24.93%、poll boundary 9.16%、progress/wait 25.09%；P-E `__poll` 15.83%、poll boundary 4.38%、progress/wait 24.36%（`p2/P2-COST-MAP-R1.md:451-470`；本会话复算一致）。但：

- `__poll` 是 inclusive 栈份额，不是可删 cycles；poll boundary 是“只有 libc `__poll` 帧”的边界样本，不是收益；
- P-B 的 progress/wait 含 2,057 个 worker-eventfd 样本、3,572 个 app-poll、297 个 poll+progress-pass；P-E 含 3,972 个 poll+progress-pass、3,537 个 progress-pass、952 个 eventfd；
- P-B 的 `__poll` 6,129 个样本中 5,664 个没有 `wait_one` 帧，873 个是 4 帧栈；90.67% app 样本没有 bench 帧（`p2/P2-REVIEW.md:54-71`；本会话复算 `app_frames` 12,246/11,104/1,142）。

因此 W1 **没有**候选级 measured hotspot，**也没有**真实消费者 profile；`#604` 仍是 `HOLD_NO_REAL_APPLICATION`，`sluice-copy` 是参考应用（`CAMPAIGN-LEDGER.md:305-315`）。P2-REVIEW M10 明确：连 ledger 的 profile 门也未过（`p2/P2-REVIEW.md:239-247`）。

**如果 owner 接受 bench/reference 代理**，匹配格是 **C6/C1**，不是 C5b/C3b/C7（`p2/P2-REVIEW.md:75-92`）。即使按 1.2% 过程级效应，C6 cycles 需要 48/91 对、C1 cycles 32/62 对；36 对不够 C6 的 hw≤0.6%。但这不是当前 W1 的 A/B 路径，因为 material 方向本身不合法。

### 4.2 W3：为什么只到 `ADMIT_TO_DESIGN`

**族级线索真实，但单候选不可见。** P2 的 W3 族级 self 为 P-A 6.00%、P-B 0.24%、P-C 15.82%、P-D 22.43%、P-E 13.07%（`p2/P2-COST-MAP-R1.md:474-482`；本会话复算 `w3_family` 一致）。但这些是 **admission + terminal/release + publication + observation** 的阶段容器：

- P-D 的 22.43% 中，admission 12.76%、terminal 3.49%、publication 4.13%、observation 2.05%；
- publication 阶段包含 publish/deliver 的内核/用户工作，`claim_execution` leaf 93/93 已归 publication；M-62 的 4.13% 是上界，不是 4.74%（`p2/P2-COST-MAP-R1.md:486-497`）；
- P-B 的 0.24% 建立在 1,142 个可见 bench 帧样本上；11,104/12,246（90.67%）app 样本没有 bench 帧，`wait_one` 只可见 700 个，5,664/6,129 个 `__poll` 没有 `wait_one`（`p2/P2-REVIEW.md:54-71`）。因此 P-B 的 W3 族、product leaf、`wait_one` 份额全部 `NOT_OBSERVABLE`，不能用“严重内联”解释。

**阶段容器不是候选上界。** M-61/M-62/M-65/M-67 的数字都来自 publication/terminal/progress-wait 容器；M-10 的 zero-op 根本不在 W-02/W-03 workload 中（`p2/P2-COST-MAP-R1.md:492-497`）。W1-DESIGN 与 P2-REVIEW 都明确：这些容器不能回答“删掉两把锁/一个标志能省多少”。

**M-61 有历史反例。** r6 R3 的候选级测量是 0.45% < 1% MME，已拒绝（`CAMPAIGN-LEDGER.md:222`）。P2 的 publication 4.13% 是不同测量范围的容器，不能推翻它（`p2/P2-COST-MAP-R1.md:598`）。因此 M-61 保持 `INCONCLUSIVE`，需要新的 measured profile，而不是直接进入 A/B。

**A/A 的分辨率不是瓶颈，候选级证据才是。** C5b CPU 2% 需 5/7 对、C3b cycles 2% 需 6/10、C7 cycles 2% 需 10/17；36 对预算对 2% 足够。但 C4 CPU 的 2% 需 20/38（hw≤1% 需 38），C7 cycles 的 1% 需 30/58；且 C3b/C7 的 cycles 带是条件性的，使用前要 cycles-only A/A（`p1/AA-CALIBRATION-2.md:461-483`）。这决定 W3 的预算是 M，而不是 S。

### 4.3 M-53：d16 小热点、d64 边缘、迁移义务未证

**d16 有候选级热点，但低于 d16 的分辨率。** `find_live_router_cookie_` leaf self：P-A 34/8,369=0.406%（Wilson 95% CI 0.291–0.567%）、P-C 47/16,472=0.285%（0.215–0.379%）；≈61.3/58.0 cycles/op（`p2/P2-COST-MAP-R1.md:499-511`；本会话复算 `m53_router` 一致）。d1/TP/copy 为 0。0.4% 的效应在 36 对 C3b cycles 上 MDE80≈0.633%，仍然低于分辨率；16 对 MDE80≈0.988% 更不够。

**d64 首次有 A/A，但边缘。** C7（`W02-uring-4k-d64`）cycles SD 1.889、bootstrap hw 0.893%、**t-hw 1.007%**、MDE80 1.416%；对 δ=2%，`n_half=17`，16 对在保守 t 判据下差一对；1% 需 58 对（`p1/AA-CALIBRATION-2.md:485-506`）。C7 的 CPU/wall 被 `ENVIRONMENT_BLOCKED` 且 A/A 均值不正（CPU +5.04%），不能用于 M-53。

**三项迁移义务仍未证。** TransportLedger 物理位置单调、控制 CQE exact-submitted、cancel 定向（`CAMPAIGN-LEDGER.md:211,260`）。这些不是性能测量，而是路由索引编码的语义前提；没有它们，`user_data` 索引可能改变 publication/cancel 的定向语义。

**结论**：M-53 是 `ADMIT_TO_DESIGN`，不是 `ADMIT_TO_AB`。预算 M/L：d64 cycles-only A/A 17–20 对 + 三项迁移证明；之后 δ=2% 的 A/B 17–20 对，或 δ=1% 的 58 对。

### 4.4 M-42：`GO` 排队不等于 A/B 准入

**指令与成本。** `submit_flushes_.fetch_add(1, relaxed)` 在 `src/async/uring_transport.cpp:222`，编译为 `lock addq $0x1,0x120(%rdi)`（objdump 地址 `4241b7`，`p2/P2-COST-MAP-R1.md:518-523`）。按 19–20 cycles：P-A 0.126–0.133%、P-B 0.066–0.070%、P-C 0.093–0.098% cycles；1 条 instruction/submit 为 P-A 0.0094%、P-B 0.0041%、P-C 0.0079%；P-D/P-E 是 TP，workload 不执行该计数器（P-D 仅 1 次启动残差，`p2/P2-COST-MAP-R1.md:524-537`；本会话 `m42.forms` 复算一致）。

**低于噪声带。** 匹配 uring 格：C6 cycles 16 对 MDE80 2.155%、36 对 1.382%；C6 CPU 0.747%/0.479%；C3b cycles 0.988%/0.633%。0.066–0.133% 的算术上界远低于这些带；1 条 instruction 的信号更低。**W-02 上的 M-42 不可测**。

**`GO` 是什么。** #637 的 `GO` 是“已排队但未启动；是否启动属 owner”（`CAMPAIGN-LEDGER.md:217`；§4.2 明确 `GO` 不是“已准入性能门”，`:243-247`）。本裁决把 M-42 记为 `NOT_POWERED`：不是 `NO_GO`，但在隔离格 + 动态回执带 + 机制门完成前，不应启动 A/B。

### 4.5 语义冲突批：不是性能实验

以下候选的 blocker 是 root/契约或 owner 裁决，不是“再测一次”：

- **M-09**：`Slot::borrow` 只写不读；owner 要么修 root 摘除 borrow facts，要么出现真实消费者（`CAMPAIGN-LEDGER.md:213`）。
- **M-30**：`zero_effect_cancel` 生产零生产者；CANCEL-01 seam 义务（`:214`）。
- **M-57**：`acquire_execution` 生产零调用；BACKEND-01 seam 必要性（`:215`）。
- **M-12**：`close_admission` 双实现；owner 指定存活实现 + 测试 oracle 迁移（`:216`）。
- **M-16**：uring 后端层 state-op 校验收敛；owner seam 冲突未裁决（`:209`）。按本战役规则，owner 未裁决 → `SEMANTIC_MISMATCH`，不是 `INCONCLUSIVE` 可重试项。
- **M-80 hygiene**：`file_semantics` 模型词表位置；owner 裁决（`:228`）。

这些候选即使有潜在机器工作，也不能用性能 A/B 来解决；代码面 PR 应先于任何测量。

### 4.6 `NO_GO`/`NO_GO_ABORT`：没有翻案

- **M-28**：`NO_GO_ABORT`，性能端点 `NOT_POWERED`，预注册中止通道判词 1b/1c 已落地；翻案必须同时满足 ledger §4.1 三条（隔离 TU/对象布局/RMW 竞争时序 + 新 session/新冻结 + 动态回执接受带）。P2 没有提供这些，`NOT_APPLICABLE（不重做）`（`CAMPAIGN-LEDGER.md:233-241`）。
- **M-61 payload**：`NO_GO`；没有生产 payload 消费者。只有消费者出现或 owner 批准测试专用通道才重开（`:219,284`）。
- **H1**：`NO_GO`；P2 只是复现现状（`io_uring_enter` 1/op、submit 97–99% kernel、flags=0），不是 H1 的 A/B 证据；#595 的 RQ1 guard/payload gate/D1-D3/64K/W-03/cancel falsifier 全过才可重开（`:229`；`p2/P2-COST-MAP-R1.md:539-542`）。
- **io-wq punt**：`NO_GO`；非 Sluice 代码，tmpfs 下 1/op、FS 条件性；不能外推到磁盘/NVMe（`:230`；`p2/P2-COST-MAP-R1.md:539-542`）。
- **tp-w8**：`NO_GO`；#604 配置/工作负载面，没有外部固定 worker 数的 root 工作负载；唯一减法交集 M-28 已 `NO_GO_ABORT`（`:231`）。

### 4.7 W2/W5：已合并，不是本战役的新候选

- **W2**：`#635` 单读 drain 已合并；9 用例语义 oracle、`progress_source_mut_drain_success_keeps_pending` mutant、TLA+ 模型都在。任何后续改动必须保持“单读清 pending”、EINTR/EAGAIN 分支、fd 借用非第二 drain authority；没有新增 W2 候选（`CAMPAIGN-LEDGER.md:258`；`:182-187`）。
- **W5**：`#634` ThreadPool `owed_count_` gate 已合并；parity 8 用例 + 4 mutants 是回归义务；权威 `any_event_owed_locked_()` 扫描保留，counter 只能作派生摘要（`CAMPAIGN-LEDGER.md:261`；`:175-180`）。

---

## 5. Owner 裁决点

以下事项**不是**技术结论，必须由 owner 裁决；在裁决前，相关候选不进入 A/B。

| # | 裁决点 | 选项 | 建议 | 影响 |
|---:|---|---|---|---|
| **O-1** | W1 的 profile 门：接受 bench/reference 代理，还是等 #604 G0？ | 等 #604 G0；接受代理并披露；只做设计/A-A；判 `NO_CHANGE_JUSTIFIED` | **等 G0 或只做设计/A-A**；接受代理只能作 mechanism-level/bench-proxy，不能声明 application-level 收益 | W1 的全部 A/B 路径 |
| **O-2** | W1 的通知拓扑：是否授权第二通知通道/blocking-read 重构？ | 授权 C2 condvar 双通道；授权 D 的 relay/重注册；拒绝 | **拒绝在 #621 W1 内做**；若追，另立 #617 G1/kernel-boundary 项目，带新模型/新 oracle/机制门 | W1-C2、W1-D |
| **O-3** | W1 的“5%”分母：process cycles / measured-loop cycles / measured-loop CPU？ | 三者选一，写入预注册 | 必须先定义；未定义则任何代理 A/B 只能记 `INCONCLUSIVE`，不得用 16/36 对结果宣传收益；material 方向仍受 O-2 约束 | W1 预算与判词 |
| **O-4** | W3 是否接受 δ=2% 作为计划值，并资助行级归因？ | 接受 δ=2%；或给别的 δ；或不做 | **接受 δ=2%，先做行级归因**（debug build/隔离格），不直接 A/B | M-61/M-62/M-65/M-67/M-10 |
| **O-5** | M-53 是否接受 cycles 为决定性指标，并做 17–20 对 d64 A/A？ | 接受 cycles + 17–20；或只接受 CPU（→ 当前环境 `NOT_POWERED`）；或 `NO_CHANGE_JUSTIFIED` | **接受 cycles + 17–20**，同时要求三项迁移证明 | M-53 |
| **O-6** | M-42 是否现在资助隔离格？ | 现在建隔离格 + 动态回执带 + 机制门；或保持排队 | **保持排队**；`GO` 排队 ≠ A/B 准入；资助前不启动 | M-42 |
| **O-7** | 语义冲突批的 owner 裁决 | M-09/M-30/M-57/M-12/M-16/M-80 hygiene 各自 KEEP/DELETE/修 root | 逐项裁决；裁决前不启动性能实验 | 6 个 `SEMANTIC_MISMATCH` |
| **O-8** | M-01 的 free-list 是否重开（S2）？ | 重开；不重开；先做 `-scope64` 隔离 profile 再定 | 先隔离 profile，再裁 free-list | M-01 |
| **O-9** | M-37/H1/tp-w8 的归属边界 | 确认 #595/#604 归属；#621 不启动 | **确认归属，不在 #621 启动** | M-37、H1、tp-w8 |
| **O-10** | M-28 是否确认不重做？ | 确认 `NO_GO_ABORT`；或给出满足 ledger §4.1 三条件的新证据 | **确认不重做**；仅三条件同时满足才重开 | M-28 |
| **O-11** | M-61 payload 是否维持 `NO_GO`？ | 维持；或出现生产消费者/批准测试专用通道 | **维持 `NO_GO`** | M-61 payload |
| **O-12** | W2/W5 是否确认无新候选？ | 确认；或指定新改动 | **确认**；任何改动重跑 9 用例 oracle + mutant（W2）/ parity 8 + 4 mutants（W5） | W2、W5 |

---

## 6. 预算与执行顺序

### 6.1 总预算原则

- **本裁决不批准任何 A/B**。所有 `ADMIT_TO_DESIGN` 只批准设计/资格工作；A/B 必须在设计门、语义门、A/A 门之后重新预注册。
- 预算级别沿用 ledger：S ≤ 1 个单因素臂组（A/A 36 对 + A/B ≤36 对，1 个格组）；M = 2–4 个臂组（A/A + A/B 72–144 对，需行级 profile 或 2 个格组）；L = ≥5 个臂组或跨后端矩阵（>144 对，需新 workload/oracle/协议改造）（`CAMPAIGN-LEDGER.md:266`）。
- 每个 A/B 前必须重新做匹配格 A/A；C6 的 wall 位置效应要求严格 AB/BA 平衡；C3b/C7 的 cycles 带使用前要 cycles-only A/A；C5b 有背景负载警告，正式使用前复跑。

### 6.2 建议顺序

| 阶段 | 工作 | 门 | 预算 |
|---|---|---|---|
| 0 | Owner 裁决 O-1–O-12 | 本文件 §5 | 0 |
| 1 | W3 行级归因（debug build/隔离格；P-B 用户栈截断修复） | 候选级 leaf/stage 分离；P-B 0.24% 不再 `NOT_OBSERVABLE` | M |
| 2 | M-53 三项迁移证明 + d64 cycles-only A/A 17–20 | TransportLedger 单调/控制 CQE exact/cancel 定向；C7 cycles SD/hw 复现 | M/L |
| 3 | M-40 设计审计（读者角色/`dispatch_mtx_` 覆盖/缝读者锁）+ 行级 profile | 机制门七问；无生产读者或明确派生摘要 | M |
| 4 | T9 协议：M-63/M-65 owed、M-10 zero-op、M-33 cancel、M-74 ready | zero-op/cancel protocol oracle；交付顺序/失效语义 | M（每项） |
| 5 | M-42 隔离格（仅 owner 资助时） | submit-only 紧循环 + 动态回执带 + 机制门 | S + 隔离格 |
| 6 | 语义冲突代码面 PR（M-09/M-30/M-57/M-12/M-16/M-80 hygiene） | owner 裁决 + 回归测试；无性能门 | S（每项） |

### 6.3 不建议现在做的事

- 不要用 36 对 W-02 A/B 去“测一下”M-42；0.066–0.133% 的算术上界低于噪声，结果只能是“不显著”，不能区分“无效应”和“效应太小”。
- 不要在 W3 行级归因前启动 M-61/M-62/M-65/M-67/M-10 的 A/B；阶段容器不是候选上界。
- 不要在 W1 的 material 方向未裁决前开 A/B；即使 owner 接受代理，C6 cycles 1.2% 还需 48/91 对。
- 不要把 `sluice-copy` 的 A/B 当作 #604 G0 或 application-level 收益（`CAMPAIGN-LEDGER.md:305-315`）。

---

## 7. 本轮实际执行的检查

### 7.1 已执行（只读复算/源码抽查）

| 检查 | 命令 | 结果 |
|---|---|---|
| P2 R1 全量复算 | `cd docs/evidence/621-progress-wait-path-20261118 && python3 -I p2/profile/analysis/recompute_p2_r1.py --json-out /tmp/p2-r1-check.json` | **exit 0**；5 forms；关键数字与 `P2-COST-MAP-R1.md` 一致：P-B `__poll` 6,129/24,585=24.93%、poll boundary 2,251=9.16%、progress/wait 6,168=25.09%；P-E `__poll` 5,500/34,736=15.83%、poll boundary 1,520=4.38%、progress/wait 8,461=24.36%；P-D publication 627=4.13%、terminal 530=3.49%；W3 family P-A 502=6.00%、P-B 59=0.24%、P-C 2,606=15.82%、P-D 3,405=22.43%、P-E 4,539=13.07%；M-53 d16 34/47；P-B app 12,246/11,104/1,142、wait_one 700、poll_without_wait_one 5,664、four_frame_poll 873 |
| P1 A/A 复算 | `python3 -I tools/recompute_independent.py --aa-root p1/aa --json-out /tmp/aa1-check.json` | **exit 0**；C1 CPU SD 0.786/hw 0.365/MDE 0.589；C1 cycles 2.346/1.121/1.758；C3 CPU 7.027/3.312/5.266；C4 CPU 3.019/1.402/2.262；C6 CPU 0.997/0.471/0.747；C6 cycles 2.876/1.368/2.155；C6 wall 位置 +2.265pp（p=0.0069） |
| P2 A/A 复算 | `python3 -I tools/recompute_independent.py --aa-root p1/aa2 --json-out /tmp/aa2-check.json` | **exit 0**；C3b cycles 1.319/0.621/0.988；C5b CPU 0.964/0.458/0.723；C7 cycles 1.889/0.893（t-hw 1.007）/1.416；C2b CPU 3.414（NOISY）；C8 CPU 4.938（NOISY）；C7 CPU 9.782（BLOCKED） |
| required-n 算术 | `python3 -I` 导入 `tools/recompute_independent.py` 的 `required_n_power`/`required_n_halfwidth`/`mde80` | C6 cycles 1.2%=48/91、2%=19/35、1%=67/130；C1 cycles 1.2%=32/62；C5b CPU 1.2%=8/13；C3b cycles 1.2%=12/22；C7 cycles 1.2%=22/41、2%=10/17、1%=30/58；C4 CPU 1.2%=52/100、2%=20/38；C6 CPU 1.2%=8/14 |
| M-42 指令 | `objdump -d --start-address=0x424190 --stop-address=0x424230 /tmp/p2-621/symtwin/bench_w02_pipeline.sym`（引用 `P2-COST-MAP-R1.md:518-523`） | `4241b7: ... lock addq $0x1,0x120(%rdi)` |
| 源码坐标 | `sed -n '214,236p' src/async/uring_transport.cpp`；`sed -n '318,328p' include/sluice/async/detail/uring_backend.hpp`；`sed -n '68,80p' src/async/uring_test_seams.hpp`；`sed -n '104,232p' include/sluice/async/detail/progress_source.hpp`；`sed -n '573,593p' src/async/async_io_context.cpp`；`grep -n -e owed_count_ -e any_event_owed_locked_ src/async/threadpool_backend.cpp` | `fetch_add` 在 `uring_transport.cpp:222`；成员在 `uring_backend.hpp:324`；唯一读者 `uring_test_seams.hpp:75`；`progress_source.hpp:74-79` eventfd、`:414-460` drain、`:524-545` port；`wait_one` 两次 `access_mtx_` 在 `async_io_context.cpp:581-593`；`threadpool_backend.cpp:350/570/577/847/859` |

**限制**：上述复算只读归档原始数据，不是新的 perf/bench/A/A/A/B 测量；`recompute_p2_r1.py` 依赖 `/tmp/p2-621/kallsyms.txt` 与 `/tmp/p2-621/symtwin/*.sym`（本机存在）；A/A 复算只重算已归档的 `p1/aa`、`p1/aa2` 原始记录，没有重跑计时。

### 7.2 明确未运行

- 任何 `xmake f` / `xmake build` / `xmake test`；当前 master 的测试总数 `N` 未确认。
- 任何新的 perf/record/stat/eBPF/uprobe/strace。
- 任何新的 A/A 或 A/B；本文件的 A/A 数字是**复算**，不是本轮新测量。
- 任何 TLC/`scripts/verify_progress_source_model.sh`；没有重新验证模型。
- 任何 sanitizer（ASAN/TSAN）或完整配置矩阵。
- 没有修改 `include/`、`src/`、`apps/`、`tests/`、`xmake.lua`、`xmake/`；没有触碰两个未跟踪 evidence 目录。

---

## 8. 本裁决不能下的结论

1. 不能说 W1 能删 5.0–5.5% cycles；历史数字只是被取代的 R1 工作稿线索，P2 只证明形态级等待热点，未证明可删量。
2. 不能说 `__x64_sys_poll` 15.77%/11.46% 是上界；`__poll` inclusive 24.93%/15.83% 才是可复算的 poll 相关 inclusive，且 inclusive 不是收益。
3. 不能说 W3 的 13.07–22.43% 是单候选收益；它是阶段容器，含内核/libc/bench 与必要同步；P-B 的 0.24% 因 90.67% 缺 bench 帧而不可用。
4. 不能说 M-61/M-62/M-65/M-67/M-10 任一已 `GO`；它们只有阶段容器，M-61 还有 0.45% <1% 的历史反例。
5. 不能说 M-53 在 d64 有已证实的 hotspot；d64 只有 C7 的 A/A 条件带，且 16 对在保守 t 判据下差一对，三项迁移义务未证。
6. 不能说 M-42 可测；`lock addq` 19–20 cycles 的 0.066–0.133% 低于匹配 uring 带；P-D/P-E 不适用。
7. 不能说 `GO`（M-42）等于 A/B 准入；`GO` 只是排队，启动属 owner。
8. 不能说 sluice-copy 的 A/B 是 #604 G0 或 application-level 收益；它是参考应用，不是真实消费者。
9. 不能说本 profile 已经过了 ledger 的“真实消费者 profile”门；P2-REVIEW M10 明确未过。
10. 不能把 W1/W3/M-53/M-42 的数字相加；它们是同一进程 cycles 的不同子集/切法。
11. 不能把 `SEMANTIC_MISMATCH` 的候选当作“再测一次”就能解决；它们需要 owner 裁决或代码面 PR。
12. 不能把本轮复算当作新的 A/A/A/B 证据；它只验证已归档数字的可复现性。

---

## 9. 参考坐标

| 主题 | 坐标 |
|---|---|
| W1 源码 | `include/sluice/async/detail/progress_source.hpp:104-232`；`src/async/async_io_context.cpp:573-705` |
| eventfd 创建/retire | `progress_source.hpp:74-86,284-290` |
| fused revalidation + drain | `progress_source.hpp:153-166,414-460` |
| probe / P3 | `progress_source.hpp:175-192`；`src/async/uring_backend.cpp:285-293` |
| poll/EINTR/POLLNVAL/POLLIN | `progress_source.hpp:198-230` |
| signal/interrupt | `progress_source.hpp:234-264` |
| drain/wake/suppression | `progress_source.hpp:414-460,466-489` |
| host borrow/ack/detach | `src/async/async_io_context.cpp:720-747`；`progress_source.hpp:274-277,524-545` |
| W1 设计评估 | `p3/W1-DESIGN.md:15-30,142-344,377-419,423-510` |
| P2 成本地图 | `p2/P2-COST-MAP-R1.md:451-537,546-568,609-620` |
| P2 独立复核 | `p2/P2-REVIEW.md:30-92,116-158,239-247,322-331` |
| P1 A/A | `p1/AA-CALIBRATION.md:20-37,384-402,418-513` |
| P2 A/A | `p1/AA-CALIBRATION-2.md:12-28,186-238,393-526` |
| #637 终态表 | `CAMPAIGN-LEDGER.md:205-231,233-247,264-285` |
| W1–W6 门禁 | `CAMPAIGN-LEDGER.md:253-262` |
| #595/#604/#617 边界 | `CAMPAIGN-LEDGER.md:291-333` |
| 战役纪律 | `CAMPAIGN-LEDGER.md:488-528` |
| M-42 源码 | `src/async/uring_transport.cpp:218-235`（`:222`）；`include/sluice/async/detail/uring_backend.hpp:324`；`src/async/uring_test_seams.hpp:75-76` |
| W5 owed-count | `src/async/threadpool_backend.cpp:344-353,550-612,847-860,875-899`；`include/sluice/async/detail/threadpool_backend.hpp:203-212,239-244` |

---

*本文件结束。所有未在 §7.1 执行的断言均为“未运行/未验证”；所有性能数字都是已发布证据或只读复算，不是本轮新测量。*
