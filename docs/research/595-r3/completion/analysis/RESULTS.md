# RESULTS — Sluice #595 R3 完成轮：系统论文式结果分析

分析 session：2026-11-05（证据根 `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z`）。
本文所有统计由 `analysis/scripts/recompute_stats.py` 从 `raw/` 逐 rep 记录独立重算
（与测量时 summary.json 逐位一致，见 `NUMERIC_CROSSCHECK.json`：5 块 × 5 指标 0 diffs），
方法遵循冻结预注册 `evidence/PREREGISTERED_R3_COMPLETION.md`
（sha256 `1a9c1e12…`，计时前冻结）：24 对 AB/BA 交替、配对相对差、bootstrap
10,000 次 seed 5950、百分位 95% CI、单侧 95% 上界、Holm 家族 α=0.05。重采样单位
是 **pair**（invocation 级），不把 rep 或请求当独立样本；p99 为运行级聚合（invocation
内 5 rep 百分位均值）；未保留逐请求延迟，故无 ECDF/CCDF。未测单元一律 NOT_RUN，
不以零或理论值显示（`MISSING.md`）。

**前置事实**：预注册冻结的 `semanticsReady=false` 全程未翻转（缺 §14.3 独立
verifier 回执；`A0_AND_SUFFIX_RECEIPT.md` 与 completion `correctness/` 目录缺失），
因此**全部 R3 B/A1 正式性能门一次未测**。本轮 gate-grade 配对数据只有 5 块：
2 个 A/A 校准块 + 3 个 R2 同 SHA 复现块。B_R2 是历史 defer 策略，与 R3 B 不同算法，
其结果只回答"旧策略"问题。

---

## 1. 实验是否有效

**任务与语义等价**：全部计时块为固定任务（20,000 valid 4 KiB 读/rep，warm tmpfs 64 MiB
工作集，`taskset -c 2` 且同批征用 cpu12），每 invocation 先跑 CAL-oracle 自检
（pattern 判别），240/240 gate-grade invocation `ok=true`、exit=0、ops=20,000 逐条核验；
0 次替换（attempts.jsonl 为空/不存在）、`repairUsed=false`（未动用唯一一次测量条件修复）。

**A0 忠实性**：A0 为冻结 baseSha `0a22a791` 的原始逐条补发循环（不构造 ready span、
不预扫描、不分配组缓冲；臂经 env 开关并由 bench 读回核对）。A/A 校准在 R3 二进制
（`0ebfb7f0…`）上跑 A0 vs A0：CPU 半宽 0.797 pp、吞吐 0.821 pp、p99 2.039 pp，
全部低于 5/3/3 pp 门 → **AA-W03-D16 PASS**。R2 二进制（`32beabcd…`）上 A_R2 vs
A_R2：半宽 0.862/0.853/1.420 pp → **AA-W02-D2 PASS**。A/A 均值偏移（W03 m2 快
−0.82% [−1.62, −0.02]；W02 m2 慢 +0.58% [−0.27, +1.45]）按预注册只有半宽门，
偏移本身不构成失败，但其 CI 不含零提示存在微小顺序/漂移效应，量级远小于结论效应。

**绝对基线**（24 invocation 均值，`metrics.csv`）：W-03 d16 A0 = 3,154.7 ns CPU/op、
317,259 ops/s、p50 36.20 µs / p95 43.65 µs / p99 49.33 µs；W-02 D2 A_R2 = 4,850.4
ns CPU/op、205,994 ops/s、p50 8.31 µs / p95 12.90 µs / p99 13.86 µs。

**CPU 覆盖与归因区间**：主指标 `cpu_ns_per_op` 用进程 CPU（CLOCK_PROCESS_CPUTIME_ID，
**含 `iou-*` io-wq 线程**，分桶证明其在 `/proc/<pid>/task` 内），ru_total（wait4）
作旁证，二者不相加；诊断分桶给出 main/driver/io-wq 份额（A0 13.7/0.8/7.3 ticks；
B 11.0/0.0/6.0）与未归因残差（+5.2…+6.7 ticks，10 ms tick 粒度）；IRQ 仅能记整机
上界（cpu2 增量 A0≈303 / B≈258，无法归因到 workload，不进入臂间差异）。

**有效配对数**：5 块全部 24/24（`gates.csv`）。

**有效性边界**：以上只建立了 A/A 与 R2 臂的有效性；R3 A1/B 未进入任何配对块，
其计时有效性（含 A1 与 B 的同输入等价性）未经 A/B 层检验。

## 2. 批量是否带来净收益（对应 F1 —— NOT RUN）

**F1 无法绘制**：B−A0、B−A1、A1−A0 三个对比在 W-03 d16/d4/d64 均无 gate-grade
数据（BLOCKED_SEMANTICS_NOT_READY；两个 enabled 的 A0 基线块 SAFE-A0-W03-4k-d4/d64
亦未执行）。主门（CPU 点估计 ≤−10% 且 95% 上界 ≤−5%、吞吐下降 ≤3%、p95/p99 ≤5%）
一次未检验。按预注册词表，机制性能 **INCONCLUSIVE**；本文不以任何诊断数字充当门证据。

**诊断窗信号（单窗口、带仪器、禁入门判定；`MECHANISM_SUMMARY.md` + 本文重算一致）**，
方向与"净收益存在"一致但不构成证明：

- 纯提交 enter：B 29,225 vs A0 60,000（−51.29%）；SQE/enter=2.053；ret==to_submit
  零部分提交；`flush==entry==10,515` 无 defer；suffix 对账闭合（untouched==suffix==9,160）。
- PMU（root u+k，time-running=100%）：cycles 733.9M vs 854.1M（−14.1%）、
  instructions −8.1%、context-switches −26.2%。
- 进程口径：bench cpu_s 0.0573 vs 0.0700（−18.1%）；ru_total 0.222 vs 0.275 s（−19%）；
  通知 1,434 vs 3,151；futex 14,878 vs 33,242；epoll_wait 4,862 vs 10,075。
- **adapter 成本信号**：A1（同 adapter、逐条）enter 与 A0 完全相同（60,000×1），
  PMU cycles −0.2%（仪器噪声内）、instructions **+5.0%**、csw +15.1%、RSS +64 KB、
  malloc +6 次一次性——adapter 簿记在指令数与一次性分配上可见，在 enter 数上不可见。
  按预注册 §9.3，未来若 B−A1 有收益而 B−A0 未过门，adapter 成本须单列，**不得用
  A1 修正 B−A0**。

**"减少提交次数是否变成完整任务收益"与"adapter 吃掉多少"两问，本轮均无配对数据可答。**

## 3. D2 退化是否被消除（对应 F2）

**旧 B_R2 在 D2 已证实恶化（同 SHA、同 session、24 对）**——R2-D2 负控 FAIL：

| 指标 | 点估计 | 95% CI | 单侧 95% 界 | 门 | 判定 |
|---|---|---|---|---|---|
| CPU/op | **+3.43%** | [+2.67, +4.16] | +4.06% | ≤5% | 界内 |
| 吞吐 | **−3.30%** | [−3.98, −2.57] | −3.89% | ≥−3% | **越界** |
| p50 | +2.29% | [+1.81, +2.79] | +2.71% | （参考） | — |
| p95 | **+5.23%** | [+4.43, +6.03] | +5.91% | ≤5% | **越界** |
| p99 | **+7.05%** | [+4.84, +9.51] | +9.07% | ≤5% | **越界** |

（Holm 家族：越界三项调整后 p<0.05；绝对值：A_R2 4,850.4→B_R2 5,015.8 ns CPU/op、
205,994→199,154 ops/s、p99 13.86→14.83 µs。）

**同一旧策略在 D3/D16 为 PASS 且是改善**（PROT-W02-D3：CPU −7.33% [−8.10, −6.54]、
吞吐 +7.98%、p95 −12.80%、p99 −11.51%；PROT-W02-D16：CPU −28.11% [−28.70, −27.53]、
吞吐 +39.24%、p99 −32.40%）→ **旧 B 的退化是形状特异的**（D2 单条补发下 defer/门链
成本占主导；深窗口下批量收益占主导），与 R2 历史结论一致。

**新算法是否消除该退化：NOT RUN**。R3 树的 `bench_w02_pipeline`（sha `4e719b1e…`）
没有 R3 研究臂/没有 D2 case，新 B 在 D2 一次未测。正控（诊断口径，1 invocation×3
phase）只证明**两条同时就绪时 B 确实合发**（3 enters×to_submit=2 vs A0/A1 6×1；
group=1、accepted=2、flush=1），不能外推到 D2 稳态单条补发的非劣性。因此本文措辞
保持为："旧 B 的 D2 退化已复现并被证实；新 B 对该退化的消除**尚未识别**（未测量）"。

## 4. 机制是否成立（对应 F3）

**存在性成立（诊断窗，≥50% 机制门）**：enter −51.29%；批量为真实的 span>1 提交
（直方图 {1:24,826, 2:1, 3:2, 6:1, 8:4,395}，Σcounts=29,225 enters、Σk·counts=60,000
ops，两项恒等式闭合）；每次入口调用恰一次 flush（10,515==10,515），无跨调用 SQE、
无 defer（内核 `io_uring_defer`=0）；被 cap 截断的成员全部逐条补交
（untouched==suffix_single==9,160；1,355 组×8+9,160 单=20,000 accepted）。

**收益来自真实成批，不是工作转移**：A1 使用与 B 完全相同的 ready-group 准备与
Context 入口但 span=1，其 enter 数与 A0 逐项相同（60,000×1）——enter 减少只能来自
span>1 的提交分组本身。B−A1 是分离"分组机制"效应的受控对照（配对版未测）。

**理论潜力为何没有全部实现（census 预测 91.5–93.3% vs 实测 51.29%；口径更正见
`analysis/CORRECTIONS.md`）**：

1. **enter 直方图是拆批后的提交尺寸，不是进入算法前的组大小**：计数器可区分二者——
   计时相位 `single_calls == suffix_single_calls == 9,160`，即 span=1 入口**全部**是
   cap 拆批的 suffix 成员，该相位调用者每次入口呈现的 ready 组 ≥2 条；因此
   "24,826/29,225 个 enter 为单条"**不能**反推调用者 ready 组多为 1–2 条（2026-11-05
   所有者修正 #3，原文已更正）。进入算法前的组大小分布未被直接测量；若要归因，须在
   adapter 入口按呈现尺寸分桶（后续独立候选）。
2. **cap=8 拆批（当前证据下的主因）**：16 深窗口被 cap 截为前缀 8 + 后 8 条逐条
   suffix（设计如此，suffix 不二次成组）；SQ 容量 16 本可 8+8 合一次 enter，该策略
   把后 8 条留在逐条路径——与 census 的 `d16_full` 整窗假设不符（已登记偏差，未放宽
   任何门槛）。
3. **SQE/enter 粘连**：hist 中的 2/3/6 条 enter 表明相邻 SQE 偶尔共享一次 enter
   （flush 边界），对 census 口径的节省率是次级修正。
4. **首次提交延迟**：B 的组调用在同一 Context 入口内完成 accept→prepare→dispatch→
   一次 flush，无 park/wait 边界（纯 wait enter=0、`submit_with_flags` 仅 4 个探针）；
   入口→enter 的纳秒级延迟因二进制 stripped 不可观测（登记）。
5. **driver**：三臂 driver 纯 submit 均为 0，提交全部在 submitter 主线程——收益
   不来自 driver 侧变化。

**通知/等待侧（诊断）**：B 的 completion 通知 1,434 vs A0 3,151（更少的完成批）、
futex −55%、epoll_wait −52%、csw −26%；d2serial 负控三臂 enter/SQE/CQE/defer/eventfd
逐项相同——单条补发下 B 不合发、不 defer、无 residue、无新增 eventfd。

## 5. 代价与失效范围（对应 F4/F5）

**尾延迟（F4）**：唯一测到的恶化是旧 B_R2@D2（p95 +5.23%、p99 +7.05%，单侧界
+5.91%/+9.07% 越 +5%）；D3/D16 旧 B 改善尾延迟。**R3 B 的尾延迟代价未知**：
D1、64 KiB、threadpool、SMT/跨物理、饱和全部 NOT RUN（仅有 A0-only 参考值，如
64K d16 A0 基线存在）。A/A 证明噪声底（尾延迟半宽 0.43–2.34 pp）远小于上述效应。

**资源（诊断口径）**：B 峰值 RSS 5,252 vs 5,204 KB（+0.9%，非按请求增长）；malloc
+6 次一次性（+3,839 字节总量级不变）；clone3=4、线程峰值=3、timer/timerfd=0、
`io_uring_setup`=4 三臂相同——无新增线程/timer/队列/每请求分配的**证据在诊断窗内
成立**；valgrind 512/1024 斜率子门因 bench 无 request-count 旋钮 NOT_RUN →
资源格 INCONCLUSIVE。

**统计功效不足/无法判断项**：d2pos 正控仅 1 invocation（预注册要求 3 次独立
invocation，偏差 D-1，相位粒度 3/3 成立）；saturation R3 B；IRQ workload 级归因。

**撤回推荐的条件**：未来任一 B 门失败（W-03 d16 主门、d4/d64 保护、D2 新 B 非劣、
资源门）即撤回相应范围的推荐；本轮尚无可撤回项（无任何 B 门被测量）。

## 6. 工程裁决（按预注册词表）

- **机制性能**：`INCONCLUSIVE` —— 主门未测；诊断证据（enter −51.29%、cycles −14.1%、
  进程 CPU −18.1%）与净收益方向一致但全部带仪器，禁入门判定。
- **产品**：`HOLD_NO_REAL_APPLICATION` —— #604 无 READY_FOR_595 handoff，未测任何
  真实应用；合成结果不外推为产品收益。
- **执行状态**：`INCOMPLETE_BLOCKED`（继承完成轮裁决；两个 enabled A0 基线块未执行、
  `COMPLETION_GATE.json` 缺失）。

**唯一最值得继续的工作**：补齐预注册 §14.3 要求的独立 verifier 回执
（`A0_AND_SUFFIX_RECEIPT.md` + completion `correctness/` 目录；注意变异体三态本轮
**实际已通过**——BASE GREEN rc=0、3 变异体 RED、干净重建逐位相同 c54a9222… 后
GREEN——缺的只是回执形态而非通过事实），使 `semanticsReady` 翻转，然后在**不改动
预注册任何门槛**的前提下重开 W-03 d16 24-triplet 主门测量。这是唯一同时解锁
Q1/Q2 与全部保护门的单一先决项。

### 补测清单（新增性能实验，本轮未执行、未修改预注册）

| # | 要解决的问题 | 预期判别 | 最小补测方案 |
|---|---|---|---|
| 1 | §14.3 回执缺失 | semanticsReady 可翻转 | 独立复核员按预注册格式出具两份回执（非计时） |
| 2 | W-03 d16 净效应 | 主门 −10%/−5% 通过与否；B−A1 与 A1−A0 分离机制与 adapter | 24-triplet 平衡三臂（seed 5950，既有 harness） |
| 3 | 新 B 守 D2（Q1） | 非劣 5/5/5/3 + enter=20,000 | 为 R3 树 `bench_w02_pipeline` 增加 R3 臂与 D2 case，预注册后 24 对 |
| 4 | d2pos 正控稳健性 | 3 次独立 invocation 是否全部 to_submit=2 | 补 2 次 invocation |
| 5 | 每请求分配斜率 | 512/1024 ops 斜率是否为零 | bench 增加 request-count 旋钮后 valgrind |

## 7. 结论（≤300 字）

本轮在冻结预注册下完成：两个 A/A 校准块通过（半宽 0.797–2.039 pp），同 SHA 复现
旧 B_R2 在 W-02 D2 的退化——吞吐 −3.3%、p95 +5.2%、p99 +7.1% 三项越界（已证实恶化；
CPU +3.4% 在界内），而 D3/D16 旧 B 反而改善，退化是形状特异的。机制诊断窗证实新 B
的批量真实存在：纯提交 enter 减少 51.3%（29,225 vs 60,000），cap/suffix 对账闭合、
无 defer，两条就绪时确实合发；PMU cycles −14.1%、进程 CPU −18%，但均带仪器、
禁入门槛。因 semanticsReady 未翻转，W-03 d16 主门与新 B 在 D2 的非劣一次未测，
机制与产品裁决为 INCONCLUSIVE 与 HOLD_NO_REAL_APPLICATION。限制：变异体实际通过
但缺独立回执；d4/d64、64 KiB、threadpool、放置、饱和与资源门未测。下一步先补
§14.3 回执翻转语义门，再原样重开 24-triplet 主测量。

---

**交付物索引**：`DATA_INVENTORY.md`（准入）、`MISSING.md`（缺失/失效清单）、
`metrics.csv` / `effects.csv` / `gates.csv`（绝对值/效应/门）、`recomputed.json`、
`NUMERIC_CROSSCHECK.json`、`NUMERIC_QA.md`（50/50）、`VISUAL_QA.md`（逐图 PASS）、
`figures/F2–F5`（SVG+PNG+figure-data+图注）、`issue-comment-paper-analysis.md`
（可粘贴 #595）。重跑入口：`analysis/rebuild_all.sh`。
