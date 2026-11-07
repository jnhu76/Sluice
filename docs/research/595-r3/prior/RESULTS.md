# #595 R3 — 结果汇总（RESULTS）

| 字段 | 值 |
|---|---|
| 文档状态 | **DELIVERED**（2026-11-05，R3 裁决与交付轮） |
| 基线 | `baseSha = 0a22a79195f1b913145fcd443496e7f70b08cdcf` |
| 宿主 | E5；Xeon E5-2666 v3；Fedora 44；kernel `7.2.11-200.fc44.x86_64`；SMT on；`schedutil` |
| 本轮裁决 | `mechanism = MECHANISM_ONLY`；`product = KEEP_IMMEDIATE`；headline shape `NO_REAL_BATCH_EXPOSURE` |
| 关键警告 | **R3 没有任何计时结果。** 下表所有性能单元为 `NOT_RUN`；“机制表”只有 syscall/结构计数，不是 CPU、时延或吞吐。 |

符号约定（全文件统一）：CPU/op、p50/p95/p99 为 `(B−A)/A`，**正 = B 更差**；吞吐为 `(A−B)/A`，**正 = B 更慢**。进入计数 `enter` 专指 `io_uring_enter`。

---

## 0. 结论摘要

1. **正确性面通过**：13/13 确定性场景 BASE GREEN；3 个命名变异全部由目标断言杀死；当前 BASE 二进制再次全绿；6 个既有回归全绿。
2. **机制面有 syscall 级证据**：W-03 稳态存在真实多 ready 补发组；按现有容量重切可把 d16 的纯 submit enter 从 20,000 降到约 1,698/20k（−91.5%），d4 降到约 5,545（−72.3%）；W-02 D2 只省 1/20k（0.005%）。
3. **性能面完全未测**：没有 A/A、没有 A0/A1/B 计时、没有负控家族、没有保护门、没有资源门。因此 **B−A0 的 CPU 点估计与 95% CI 不存在**，产品净胜门无法判定。
4. **原型有明确协调成本**：cap=8 且 suffix 逐条提交，使 d16 的实测 smoke flush 为 11,140/20k（`BUILD.md` 10,954；理论 1,698），d64 为 17,781/20k（`BUILD.md` 17,739）。这些是单次 smoke，不是性能结论；计数随调度波动，但说明“enter 合并”在深窗口下被 cap/suffix 策略大量抵消。
5. **产品裁决**：`KEEP_IMMEDIATE`。不能写 `PRODUCT_CANDIDATE_READY`，也不能据此开产品候选 PR。headline `W02-uring-4k-d2` 是 `STARTUP_ONLY` → **NO_REAL_BATCH_EXPOSURE**。

---

## 1. 预注册与测量单元状态

| 项 | 要求 | 实际 | 判定 |
|---|---|---|---|
| `PREREGISTERED_R3.md` | 计时前冻结假设、臂、门槛、停手条件 | 文件不存在 | **MISSING** |
| `measurement-main/` | 主 shape 的 A/A + A/B 原始数据 | 目录不存在 | **MISSING** |
| `measurement-product/` | B−A0 产品净胜数据 | 目录不存在 | **MISSING** |
| `measurement-mechanism/` | 机制/成本分解数据 | 目录不存在 | **MISSING** |
| `measurement-analysis/` | 配对 bootstrap 与 CI | 目录不存在 | **MISSING** |
| harness | 计时/配对/分析脚本 | 只有 `census_run_final.sh`、`census_analyze.py` | **无计时脚本** |
| `logs/` | 计时日志 | 只有 `build-baseline.log` | **无计时日志** |

核验命令与结果见 `PROTOCOL.md` §3（P-2、P-3）。因此本文件不补写、不推测任何配对数值。

---

## 2. 配对表（含 CI）——全部未执行

### 2.1 本应产出的单元

| 单元 | case / 指标 | 计划配对 | 实际配对 | 点估计 | 95% CI | 判定 |
|---|---|---|---|---|---|---|
| A/A 校准 | W02-uring-4k-d2，CPU 半宽 | 24 | **0** | — | — | `NOT_RUN` |
| A/A 校准 | W02-uring-4k-d2，吞吐半宽 | 24 | **0** | — | — | `NOT_RUN` |
| A/A 校准 | W02-uring-4k-d2，p99 半宽 | 24 | **0** | — | — | `NOT_RUN` |
| A/A 校准 | W03-uring-4k-d16，CPU/tput/p99 | 24 | **0** | — | — | `NOT_RUN` |
| A0 vs A1 | W03 d4/d16/d64 CPU/op | 24 | **0** | — | — | `NOT_RUN` |
| A1 vs B | W03 d4/d16/d64 CPU/op | 24 | **0** | — | — | `NOT_RUN` |
| **A0 vs B（产品净胜）** | 主 shape，whole-executor CPU/op | 24 | **0** | **UNMEASURED** | **UNMEASURED** | `NOT_RUN` |
| A0 vs B | W03 d4/d16/d64 p50/p95/p99 | 24 | **0** | — | — | `NOT_RUN` |
| A0 vs B | W03 d4/d16/d64 吞吐 | 24 | **0** | — | — | `NOT_RUN` |
| 深度单元 | D2 / D3 / D16 | 24 | **0** | — | — | `NOT_RUN` |
| 负控家族 | D1/D2/D3、64 KiB d16、W-03 d4/d16/d64、threadpool、saturate | 24/项 | **0** | — | — | `NOT_RUN` |
| 资源门 | peak RSS、threads、queue/timer/allocation | 24 | **0** | — | — | `NOT_MEASURED` |

### 2.2 结论

- **不存在任何 R3 的 CPU/时延/吞吐点估计，也不存在任何 CI。**
- 因而 `B−A0 ≤ −10% 且 95% 上界 ≤ −5%` 的产品门**无法判定通过或不通过**；按裁决规则只能保守地不产品化。
- 不能把 R1/R2 的数字填入本表：它们是不同会话/二进制/块（`EVIDENCE_BASELINE.md` F9）。

---

## 3. 机制 / 成本表（syscall 与结构计数，不是性能）

### 3.1 基线 enter（`strace` 实测，独立 oracle）

| 运行 | `io_uring_enter` 总数 | 纯 submit | probe | `to_submit>1` | 每次 submit 的 enter |
|---|---|---|---|---|---|
| W03-d16/d4/d64（20k×3 phase，冻结基线） | 60,004 | 60,000 | 4 | **0** | 1.0000 |
| W02-d4（20k×3 phase，冻结基线） | 60,004 | 60,000 | 4 | **0** | 1.0000 |
| 诊断构建 unmarked（W03/W02，20k×3） | 60,004 | 60,000 | 4 | **0** | 1.0000 |
| 诊断构建 verbose（512×3 phase） | 1,540 | 1,536 | 4 | **0** | 1.0000 |

来源：`raw/census/canonical/*-strace.txt`；本轮复核三份 20k 计数均为 60,004。**口径**：20k 行只直接测得总数；纯 submit/probe 的 60,000+4 分解来自 512 请求逐行 trace（1,536 次 `(to_submit=1, min_complete=0, flags=0)` + 4 次 `GETEVENTS`）与工作量推断，不是 20k 逐调用测量。结论：**基线已合发比例 = 0%**，最大 SQE/enter = 1。

### 3.2 按现有容量重切的 enter（理论，非实现）

| case / phase | 基线纯 submit /20k | 重切后 enter /20k | 每 op enter | 节省 |
|---|---|---|---|---|
| W03-d16 ph1 | 20,000 | 1,698 | 0.0849 | 91.51% |
| W03-d16 ph2 | 20,000 | 1,511 | 0.0756 | 92.45% |
| W03-d16 ph3 | 20,000 | 1,331 | 0.0666 | 93.35% |
| W03-d4 ph1 | 20,000 | 5,545 | 0.2773 | 72.28% |
| W03-d4 ph2 | 20,000 | 5,363 | 0.2682 | 73.19% |
| W03-d4 ph3 | 20,000 | 5,467 | 0.2734 | 72.67% |
| W02-d2（每 phase） | 20,000 | 19,999 | 0.99995 | **0.005%** |
| W03-d64（512 窗口） | 512 | 8/10/9 | 0.0156/0.0195/0.0176 | 98.4/98.0/98.2% |

### 3.3 原型 B 臂的实际分组计数（**单次 smoke，非计时**）

命令：`flock MEASUREMENT.lock env … SLUICE_595_R3_ARM=B SLUICE_BENCH_STATS=1 … bench_w03_external_loop`，`WARMUP=0 MEASURED=1`，tmpfs fixture，`rc=0`、`failing repetitions=0`。回执：`raw/smoke/run.log`、`raw/smoke/b-{d4,d16,d64}.log`。

| case | 回执运行 flush/20k | `BUILD.md` §7 的另一次运行 | groups | singles | suffix_singles | prefix_max | 每 op enter（回执） |
|---|---|---|---|---|---|---|---|
| W03-d4 | **5,955** | 6,139 | 5,711 | 244 | 0 | 4 | 0.298 |
| W03-d16 | **11,140** | 10,954 | 1,274 | 9,866 | 9,863 | 8 | 0.557 |
| W03-d64 | **17,781** | 17,739 | 317 | 17,464 | 17,464 | 8 | 0.889 |

回执运行与 `BUILD.md` 的运行不一致（调度影响组形成），说明这些计数**不是确定性常量**；本表只证明量级与 cap 效应本身，且所有数值都是单次 smoke，不是性能证据。

### 3.4 机制 / 成本账本

| 项 | 观测 | 性质 |
|---|---|---|
| enter 合并 | 基线 1 enter/op；按容量重切 d16 → ~0.085/op，d4 → ~0.277/op | syscall 级、已验证 |
| cap=8 的截断 | d16 组 16 → 前 8 一条 enter，后 8 逐条；回执 `suffix_singles=9,863/20k`，flush 11,140 | 结构计数、单次 smoke |
| 深窗口退化 | d64 回执 `suffix_singles=17,464/20k`，flush 17,781（≈ 基线的 89%） | 结构计数、单次 smoke |
| A0 控制臂 | worktree 的 refill 循环**对所有臂**先构造完整 `ready` span（`bench/bench_w03_external_loop.cpp:349-372`），A0 再用 `ctx.submit_read` 逐条提交（`bench/r3_ready_batch.hpp:88-98`）；`baseSha` 原循环在循环内逐条构造 `ReadOp`（`:339-349`）。**A0 因此包含原始 baseline 没有的组准备/vector 开销** | 代码读取；**无数字** |
| adapter 成本 | `A1` 逐条走 `AsyncIoContext::submit_read`（与 A0 同路径），只余 adapter 簿记；`A0−A1` 未计时 | 代码级；**无数字** |
| context 绕过 | `B` 直接调用 detail seam `submit_ready_reads`，**绕过** `initiation_rejection` 与 `access_mtx_`；`B−A1` 含“分组”+“绕过 context”两部分 | 代码级；`BUILD.md` §3.4 披露 |
| 每请求分配 | report 为调用者栈上 `std::array<RequestKey,8>`；adapter 的 `std::vector<Request>` 每 phase `reserve(depth)` 一次 | 设计声明；**未用 RSS/allocator 验证** |
| 新增线程/定时器/队列 | 无新增 thread/timer/queue；复用既有 `dispatch_`/`TransportLedger` | 设计声明；**未用 thread/资源探针验证** |
| 请求身份 | 每条请求独立 SQE/CQE/Request/borrow；不合并请求 | 正确性 oracle 覆盖 |

**关键判断**：机制层“合并 enter”成立；但**合并 enter 是否转化为 CPU 收益，本轮没有任何测量**。R2 曾在另一实现（owner-gated defer）上测得 CPU 效应；那不是本原型的证据，且其二进制/会话不同。

---

## 4. 正控与负控

### 4.1 正确性正控（oracle 自身健全性）

| 控制 | 内容 | 结果 |
|---|---|---|
| 模式自检 | `CAL-oracle-self-test`：256 个 distinct pattern；offset confusion、单字节翻转、全零 buffer 均能检出 | 通过 |
| 字节 oracle | W-03 phase 2 `verify=true`；17 次 canonical 运行 exit 0 | 通过 |
| 确定性正控 | 13 个场景全绿，连续 5 次（回执）；本轮再跑一次仍全绿 | 通过 |
| 既有回归 | 6 个二进制（research/fault/submit_boundary/progress_race/flush_count/external_loop） | 全部 exit 0 |

### 4.2 变异负控（测试能否杀死缺陷）

| 变异 | 目标断言 | 结果 |
|---|---|---|
| M1 跳过 transport 未就绪门 | `a batch at SQ-full transport residue accepts nothing` | `BASE GREEN → M1 RED`（exit 1，6 FAIL，首行即目标） |
| M2 丢掉 residue 广告 | `partial kernel consumption advertises dispatch residue` | `BASE GREEN → M2 RED`（exit 1，唯一 FAIL 即目标） |
| M3 重复派发已消费 SQE | `a fully consumed group advertises no residue` | `BASE GREEN → M3 RED`（exit 1，唯一 FAIL 即目标） |
| 恢复 | 重建 BASE 后全量 + 三目标场景 | 回执全绿；本轮当前 BASE 二进制全绿（未重做重建） |

详见 `MUTANTS.md` 与 `correctness/CORRECTNESS.md`。

### 4.3 性能负控家族

| 家族 | 状态 |
|---|---|
| D1 / D2 / D3 / 64 KiB d16 / W-03 d4,d16,d64 / threadpool / saturate | **NOT_RUN** |
| Holm–Bonferroni 家族判定 | **NOT_RUN** |
| 负控“不退化”声明 | **不能声明** |

冻结历史（仅引用，不是 R3 结果）：R1 的 D2 在 CPU +4.17%（界内）时因 p95 +5.69%、p99 +7.45%、吞吐 3.99% 下降判 FAIL；W-03 d4 p95 / d64 p99 为功效不足。R2 没有重跑该家族。

---

## 5. 性能保护门

| 保护项 | 冻结阈值 | R3 状态 |
|---|---|---|
| 主 shape 吞吐下降 | ≤3% | **NOT_ASSESSED**（无吞吐测量） |
| 主 shape p95/p99 恶化 | ≤5% | **NOT_ASSESSED** |
| 负控 CPU/p95/p99 | ≤5% | **NOT_ASSESSED** |
| 负控吞吐下降 | ≤3% | **NOT_ASSESSED** |

补充事实：

- W-02 未接入 adapter，只做了单路径回归（`W02-uring-4k-d16` exit 0，byte oracle 通过）；这**不是**保护门证据。
- 原型的 cap/suffix 行为可能改变 tail（尤其 d16/d64 的 suffix 逐条提交），但**本轮没有任何 p95/p99 数据**，不能判断是否恶化。
- R1 的 D2 尾部/吞吐失败仍是冻结历史事实；R3 未重测，不能声称它已被修复。

---

## 6. 内存、线程与资源

| 项 | 状态 | 说明 |
|---|---|---|
| peak RSS | **NOT_MEASURED** | 无 `/usr/bin/time -v`、无 rusage 聚合 |
| thread 数 | **NOT_MEASURED** | 无 thread 探针；设计声明“不新增线程”未验证 |
| queue/timer 数 | **NOT_MEASURED** | 设计声明“无新增”；未做结构/分配计数 |
| per-request allocation | **NOT_MEASURED** | 设计声明 report 在栈上；adapter 每 phase 一次 `reserve` |
| 资源门 `RSS ≤ +2%` | **NOT_ASSESSED** | 无数据 |

因此不能声明 Resource gate 通过。

---

## 7. R2 历史复现

| 项 | 状态 |
|---|---|
| R2 的 24 对 A/A、D1/D2/D3/D16 计时 | **未复现**（R3 未计时） |
| R2 的 split-d2 线程/指令/eventfd 分解 | **未复现** |
| R2 的 CF-1/CF-2 反事实 | **未复现**（R3 原型是不同机制） |
| R2 的负控家族 | **未复现**（R2 本身也未重跑） |
| 源码条件 | R3 基线与 R2 锚点 `271b81bf` 的 `include/src/apps/xmake/tests/xmake.lua` 子树 **IDENTICAL**（`BASELINE.md` §3） |

**重要区分**：

- “源码子树相同”只说明构建输入相同，**不等于**复现了 R2 的数值；R3 二进制是新构建（`libsluice_async.a` 在普查期为 `76e59bd6…`，实现轮为 `797bb132…`，正确性轮后为 `2aeff83c…`）。
- R3 原型（consumer 侧批量、cap=8、suffix 逐条）与 R2 的 B（owner-gated defer flush）**不是同一机制**：R2 的门要求 submitter 是最近 progress driver，W-03 的 submitter 不是 driver，因此 R2 原型不会捕获 W-03 的组（`READY_EXPOSURE.md` §7 第 4 条）。
- 因此 R3 **不能**用 R2 的“D2 改善”或“82% 上限分解”支持本原型；两者不可互换引用。

### 7.1 历史数字（只作背景，不作 R3 结论）

符号已归一化为“正 = B 更差”。**R1 与 R2 是不同会话、不同二进制、不同块；两个表之间不得相减、不得横向比较（`EVIDENCE_BASELINE.md` F9）。** 每行标注其来源会话/二进制。

**R1（R1 二进制/会话；`report-595-e5.md`、`summary-tables.md`）**

| 块 | CPU/op | p50 | p95 | p99 | 吞吐下降 |
|---|---|---|---|---|---|
| R1 D2（24 对，R1 会话/二进制） | +4.17% [+3.53,+4.83] | 未报告 | +5.69% [+4.90,+6.50] | +7.45% [+5.77,+9.38] | 3.99% [+3.40,+4.60] |
| R1 D2 replication（同一 R1 会话/二进制） | +3.05% [+2.41,+3.71] | — | +4.98% [+4.22,+5.78] | +5.81% [+3.36,+7.83] | 2.95% [+2.34,+3.56] |

**R2（R2 独立会话；phaseA 二进制 `61c6d579…`；`PROTOCOL.md`/`RESULTS.md`/`ROOT_CAUSE.md`）**

| 块 | CPU/op | p50 | p95 | p99 | 吞吐下降 |
|---|---|---|---|---|---|
| R2 phaseA D2（24 对，R2 会话/二进制 `61c6d579…`） | +3.73% [+3.04,+4.42] | +6.91% [+5.03,+9.23] | +4.96%（CI 两版本，见下） | +7.07%（CI 两版本，见下） | 3.58% [+2.94,+4.22] |
| R2 split-d2（12 对，另一块，不可与 phaseA 并列） | +3.70% [+2.93,+4.55]，+189.15 ns/op | — | — | — | 3.55%（CI 幅度 [2.84,4.34]） |

来源：`EVIDENCE_BASELINE.md` §2.1。R1 D2 的 p50 未在归档报告中出现；R2 phaseA 的 p95/p99 CI 在文档中有两个版本（`[+4.21,+5.75]/[+4.19,+11.80]` 与 `[+4.23,+5.74]/[+4.24,+11.78]`），本表不择一、不重算。

---

## 8. 逐项限制与未测单元

### 8.1 未测单元（必须随结论列出）

1. 任何 A/A 校准（CPU/tput/p99 半宽）。
2. 任何 A0/A1/B 配对计时、CPU/op、wall/op、吞吐、p50/p95/p99。
3. 任何负控家族与 Holm 判定。
4. 性能保护门与资源/内存/线程门。
5. R2 历史数字的本轮复现。
6. `A_R2` / `B_R2` 臂（无定义）。
7. 真实块设备、cold cache、O_DIRECT、非 4 KiB block size、非 tmpfs 文件系统。
8. threadpool backend 下的批量路径。
9. 跨线程并发 submit 与批量的交错（`THREAD-01` 允许并发，本轮只测固定 owner/单提交者）。
10. cap > 8、suffix 二次批量、d16/d64 的完整组重用。
11. W-02 `RequestScope` 接入 adapter（本轮未接）；**W-02 d16（冻结机制门的字面 shape）未在普查中覆盖**（本轮只有 W-02 d2/d4）。
12. `sluice-copy` 插桩运行（只做源码级判定）。
13. #604 真实 consumer（fio/DuckDB/segment/WAL producer）与 `READY_FOR_595` handoff。
14. 第二台原生主机 / 跨机可移植性。
15. kernel/system vs user CPU 归因、io-wq 机制、eventfd 内部阶段。
16. 内存分配/RSS/线程数/队列数。
17. 64 KiB 不适合批量输入的完整性能影响（只有研究 oracle 语义覆盖）。
18. stale generation 批量、pre-park wake with batch residue 的精确时序、overflow 期间批量 flush（仅部分覆盖）。

### 8.2 方法与交付缺陷

1. `PREREGISTERED_R3.md` 缺失（P-2）。
2. `measurement-*` 四目录缺失（P-3）。
3. `OPEN-1` 未解除，`A0/A1/B` 只是研究标签（P-4）。
4. 交付前 `evidence/MANIFEST.sha256`（22 项）有 3 项失败；旧清单保留为 `MANIFEST.sha256.pre-delivery`，本轮已重生成并通过（P-6）。
5. `r3.patch` 已过期、不能反向应用；最终态由 `r3-final.patch` 捕获（P-7）。
6. smoke 计数非确定性（P-10）；回执见 `raw/smoke/`，但不同运行之间可差数百次 flush。
7. **A0 不是 `baseSha` 原始循环**：worktree 的 refill 对所有臂先构造 `ready` span（`bench/bench_w03_external_loop.cpp:349-372`），A0 因此含组准备开销；未来 A0/A1/B 计时必须改用 `baseSha` 原循环（`:339-349`），或显式披露这项共同开销。
8. 评论正文曾被排除在主清单外；本轮改为 `MANIFEST.comment.sha256` 独立覆盖评论、`MANIFEST.sha256.hash` 记录主清单哈希（见 `VERDICT.md` §6）。
7. 正确性运行未持测量锁（P-5）。

---

## 9. 证据索引

| 内容 | 路径 |
|---|---|
| 冻结协议与偏差 | `evidence/PROTOCOL.md` |
| 普查整合 | `evidence/READY_EXPOSURE.md` §1–§10 |
| 正确性全文 | `evidence/correctness/CORRECTNESS.md` |
| 变异全文 | `evidence/MUTANTS.md`、`evidence/correctness/MUTATIONS.md` |
| 普查原始 | `raw/census/canonical/`、`raw/census/frozen/`、`raw/census/analysis/` |
| 普查汇编 | `evidence/census.json` |
| 正确性原始 | `raw/correctness/*.log`、`raw/correctness/*.txt` |
| 最终态 patch | `evidence/r3-final.patch`（10 文件，+2883/−51，reverse check exit 0） |
| 历史 patch | `evidence/r3.patch`（9 文件，+1436/−51，已不能 reverse apply） |
| 裁决 | `evidence/VERDICT.md` |
| 清单 | `evidence/MANIFEST.sha256` |
