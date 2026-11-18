# Sluice 性能研究史 digest 与复现资格审计（#595 / #621 / #622 / PR #634 / PR #635）

撰写：2026-11-18（性能研究史研究员会话）。方法：只读仓库 + gh 只读取回写评论；
本文件是本次唯一写入物。所有数字逐字来自被引用的本地文档或 `gh api
repos/jnhu76/Sluice/issues/622` 取回的评论（id `6614830243`）；引用格式
`路径:行` 或 §节名。本文件区分两类内容：

- **裁决（verdict）**：各轮冻结门产出的最终判定（KEEP_IMMEDIATE、GO/FAIL、
  READY_FOR_PRODUCT_PR 等），带预注册门槛与 CI。
- **支撑证据（supporting evidence）**：成本地图、机制计数、profile 份额等
  只作定位/去重用的数字，不参与裁决，不得当 A/B 引用（round2 报告
  `A.4`/`E.4` 的原话约束）。

## 0. 共享机器底座（复现资格的公共前提）

| 项 | 值 | 出处 |
|---|---|---|
| 主机 | Intel Xeon E5-2666 v3（Haswell-EP，10C/20T，单 NUMA，2.90 GHz nominal）bare metal | `docs/evidence/621-threadpool-owed-count-20261112/REPORT.md:12`；#622 评论 `CPU_VERIFIED`（Haswell-EP，family 6 model 63，无 Intel PT / 无 RDT MBM） |
| 内核 | 7.2.11-200.fc44.x86_64（#622 轮为 7.2.11；#621 轮为 7.2.11→同族 fc44） | 同上两处 |
| 工具链 | gcc 16.2.1、xmake 3.1.2、liburing 2.13 | round-e5 verdict block；#621 round2 `A.2` |
| perf 权限 | `perf_event_paranoid=2`（用户态 PMU；内核采样需临时 sudo） | #622 评论 `PERF_STATUS`；`595-r4-state-aware/BASELINE.md:21` |
| governor | **两代口径**：#595/#622 各轮 = `schedutil`（1.2–3.5 GHz，不改 governor）；#621 两轮 = 测量期间 `performance`（结束恢复 schedutil 并读回） | `key-path-593/decision.md:165`（schedutil, governor unchanged）；`621-threadpool-owed-count REPORT.md:12`；round2 `D.1`/`D.5.3`/`D.5.5` |
| 文件系统 | /home = btrfs；tmpfs /tmp（#595/#622 计时用 tmpfs；#621 两轮用 btrfs 真实文件 256 MiB warm cache） | `595-pause/RESUME.md` §2；#622 评论；`621-threadpool REPORT.md:14` |

频率资格的关键事实（后续审计全部引用）：

- schedutil 下 cycles 带 **0.5–2.25% 频率漂移**（#622 评论
  `MEASUREMENT_UNCERTAINTY`："cycles/op carry 0.5-2.25% schedutil frequency
  drift"），检测门 \|delta\| > max(3×CV, 1%)。
- performance governor 下 idle `cur==max==3.5 GHz`，负载中实测 **3.25–3.35 GHz**
  （intel_cpufreq 硬件 turbo/功耗限制），故 round2 报告以 cycles 为主计数
  （round2 `D.9` 第 10 条）。
- 两个受控 session（r2r1/r2r2）运行中 sysfs 采样**频率下探**（cpu2 最低
  1,174,069 kHz；r2r2 段 cpu2 最低 1,151,304 kHz），三段
  `freq_eq_all=false`——"测量期间持续 cur==max" 未能确认，已如实披露
  （round2(r5) `D.5.5`；`/home/jnhu/sluice-621-r2/results/session-r2r2.summary.txt`）。
- 仓库方法论明确：**不同机器的结果不可比，只有同机配对运行可判定**
  （`docs/performance/methodology.md` "Rules" 第 2 条）；#595 pause RESUME
  另记 **"内核升级后不承诺同数字"**（`595-pause/RESUME.md` §2）。

---

## 1. #621 研究报告与最新回写（当前结论、未决项）

### 1.1 Round 1：ThreadPool owed-count 去空扫描（PR #634，已合并）

- 证据目录：`docs/evidence/621-threadpool-owed-count-20261112/`（REPORT.md、
  final-repair.md、mutant-receipts.txt、formal-ab/*.tsv、production.diff、
  MANIFEST.sha256 15/15 OK）。合并提交：`b0dc4dc0`（生产 gate）→
  `bb91a493`（evidence/ledger/changelog）→ `3d7e0685`（exact parity oracle +
  wording repair）→ merge `3f22d2d0`（2026-11-12）。
- 裁决（REPORT.md §5）：**MECHANISM: GO · END_TO_END（this consumer）: GO**。
  依据（均为主机配对证据）：sluice-copy 256 MiB @4 KiB/d4/w1，24 对 AB/BA
  交错，CPU/task −5.58% [−6.45,−4.71]%（A/A 带 ±1.2%）、wall −6.53%、
  user-space instructions −22.1%（≈6,854→5,342 条/请求）；B profile 中
  scan family（`operator[]` 2.88% + `size()` 1.58% + `any_event_owed_locked_`
  1.35% ≈ 5.8%）消失到 <0.7%。负对照 64 KiB/d1：+0.10% [−1.56,+1.77]%，
  措辞经 final-repair 修正为"无统计可辨回归（未预注册非劣界，CI 上界
  +1.77% 不能排除这么小的回归）"（final-repair.md §2）。
- 正确性：parity oracle 8 例 + 4 个注册 mutant（exit 142/1/1/134，
  `mutant-receipts.txt`）+ 四配置矩阵 80/80、80/80、59/59、59/59 + TSAN/ASAN。

### 1.2 Round 2：ProgressSource drain 单次 read（PR #635，已合并）

证据根 `/home/jnhu/sluice-621-r2/`（本机存在）。**注意两个副本的版本差**：
工作区未跟踪副本 `docs/evidence/621-round2-R1/REPORT.md`（mtime 11-12 19:38，
138,781 B）是 **r5 修订**；证据根 `/home/jnhu/sluice-621-r2/REPORT.md`
（mtime 21:28，153,836 B）是 **r6 修订**（当前结论）。r6 新增的唯一测量是
受控 session `session-r2r2`；归档包在 PR #636（draft，`do-not-merge`，
永不合并；CHANGELOG.md:36-38 引用）。

修订演进（r6 报告头部修订记录，与工作区 r5 副本一致到 r5）：
r1 `INCONCLUSIVE/PENDING_AUDIT` → r2 对账 → r3 审计后降级
（撤回"环境假象"归因、"≥6%"、"1/16 缩放"）→ r4 受控 session r2r1
（A/A 宽度 3.8789>3.0，`confirmed=false`）→ r5 口径修正（保守量级 4.7712%，
逐对正号披露）→ **r6：session-r2r2 后三项裁决改为 GO / GO /
READY_FOR_PRODUCT_PR**。

当前（r6）裁决与数字：

| 项 | 值 |
|---|---|
| `MECHANISM_VERDICT` | **GO**：语义 oracle 纯净 BASE 与 B 均 9/9；mutant 被恰好 3 条语义 re-arm 断言杀死；机制计数 `eventfd_read` 262,135→131,073（−50.0%）、bpftrace `@read_eagain` 118,115/124,547→12/7、perf tracepoint `sys_enter_read` 1.990→0.987/请求；R2 受控测量 `confirmed=true` |
| `END_TO_END_VERDICT` | **GO**（session-r2r2，36 对）：instructions **−5.0180%** [−5.7603,−4.2987]（36/36 逐对负）、cycles **−5.1111%**（36/36）、task-clock **−5.4653%**（35/36）、rusage CPU **−5.3199%**（35/36）；A/A 36 对 +0.7713% [−0.4342,+1.9748]，宽度 **2.4090≤3.0**；p99 CPU B/A −7.2690%（预注册定义：rusage 经验 p99，B≤1.03×A）；保守量级 = 四组最小绝对值 **5.018%（instructions）**，唯一可引用值（不得引用 R1 的 4.7712%、不得引用旧 36 对的 −11.968%） |
| `PRODUCT_READINESS` | **READY_FOR_PRODUCT_PR**（不等于已准 merge；Owner Merge Gate 保留）。PR #635 已于 2026-11-13 合并（merge `5d94f233`），ledger dated section + PROG-02 行 + CHANGELOG 条目在仓库内 |
| 负对照 | 64 KiB/d1 12 对 rusage −1.4611% [−2.7425,−0.2340]，每请求 0.5935 µs（主场景 0.6869 µs）——**不是零效应**（每通知一次删除），也不是反例 |

R1 session（r2r1）保留为历史对照：A/A 12 对宽度 3.8789 失败、主场景四组
−5.5528/−4.7712/−5.3220/−5.3359%、逐对正号 1/3/4/4。两个受控 session 量级
一致（−5.0~−5.5%，最大差 0.54 pp），旧 session spread（−1.4%~−12%）不再
作为量级来源；旧 10 对 perf stat session 已重建 protocol（固定 A→B 顺序、
无 sync/Writeback 记录、估计量为均值之比 −1.4160%），既非反证也非合规
负对照（r6 `E.2`/`D.5.1`、`evidence-r2/crosscheck-10pair/`）。

**当前未决项 / 披露限制**（r6 `E.1`/`E.3` "披露但不阻断"，合并后仍开放）：

1. io_uring "成功 read 与清 pending 之间内核重新 signal" 窗口**无确定性
   注入 seam**；安全性依赖 `PhysicalProbe` 兜底 + 既有 uring race 测试
   （k11b/k1/k2/23 项 race）。最坏后果是冗余通知，不丢唤醒。
2. 真实内核 EINTR 未测（仅 test-only 注入）。
3. BASE 语义 oracle 只在 release/liburing=y 跑过；BASE debug/liburing=n 未跑。
4. io_uring 端到端、wall、能效、多 worker、zero-op 密集、cold cache 未测
   （两轮共同范围声明）。
5. 旧 session spread 成因未分解；R2 的 A/A 仍有 ±9% 单对噪声（36 对压宽度）。
6. 下一候选（r6 `F.6`）：poll(-1)→阻塞 read（profile rank 1，约 5.0–5.5%
   cycles）与 mutex family ~9–11% 均需新 measured profile + 单因素 A/B，
   R2（0.90%）/R3（0.45%）已 REJECTED（低于 1% 最小有意义效应；R3 还会
   删除 `PublicationEpiloguePauseGate` 证据面——`grep -rl` 6 个测试文件）。
7. 工作区未跟踪的 `docs/evidence/621-round2-R1/REPORT.md` 是 r5 旧版，与
   已合并 CHANGELOG（引 session-r2r2 数字）不一致；最新报告只在证据根与
   PR #636。

---

## 2. #595 R1–R5 与 pause archive

pause 快照：`docs/research/595-pause/`（pause-20261107）；
`DECISION_SNAPSHOT.md` 是暂停时点有效结论的唯一汇总。产品状态：master 保持
**A0/IMMEDIATE**，零研究代码合入（271b81bf 时点）。

### 2.1 前史：key-path-593 K4-b（#595 的机制祖先）

来源 `docs/research/key-path-593/k4-sqe-cqe-batching.md`。裁决
`ELISION_PERF_SUPPORTED`（form-gated，**仅测 D=4/16/64**；D2/D3 未测，
2/D enter 模式预测 D=2 无 enter 缩减）。

- **结构性不对称（支撑证据）**：CQE reap 已自然成批——11.76 CQEs/peek @d16、
  28.88 @d64、3.47 @d4；SQE 提交永不成批——1.00 SQE/enter、
  1.00005–1.00007 enters/op（每深度）。批量化余量：passes/op 0.127 @d16、
  0.030 @d64（defer 后 ≈8/33 条 SQE 积累），D=1 无余量（passes/op 1.34）。
- **K4-b 收益（15 对 AB/BA，配对 CPU）**：W02-d16 −28.07% [−28.69,−27.34]、
  d64 −35.53%、d4 −13.06%、saturate −43.06%、64k-d16 −2.99%（CPU）；
  enters/op d4 0.50017、d16 0.12577、d64 0.03447（≈2/D）。
- **负例**：① 64 KiB d16 **p99 +7.46%** [+7.14,+8.47]、p95 +7.50%，0/15 p99
  对改善——material regression，要求 payload gate；② W-03 d4 **+2.54%**
  [+1.32,+4.79]（低于 4.79% band、在唯一 W-03-d4 A/A [−6.14,+6.51] 内，
  unattributed）；③ D=1 −2.50%（机制不可及，unexplained，符号跨 session
  不稳）；④ threadpool +1.19%——①③④共同结论：**±2.5% 量级的布局/时钟
  噪声双向存在，<3% 效应不可归因机制**（§9 no-mechanism controls）。
- **residue signal 与 eventfd write 成本**：每次 deferred submit 使
  `unsubmitted_after_dispatch=true` → `signal_ready_progress()` 取 progress
  mutex、增 epoch、可能写 eventfd（`uring_transport.cpp:96-101`、
  `progress_source.hpp:252-264`）——这是被删的 enter 换来的新增成本之一，
  census 未从 residue 路径分离（§6 "Added work per deferred submit"）。
- **W-03 mixed-driver/split-owner**：拓扑 gate（"最近两个 distinct drivers"，
  非严格 same-thread，`uring_backend.hpp:285-308`）使 W-03 d4/d16/d64
  enters 保持 1.00007（gate 不 defer）；mixed-use submitter 未实测；
  cancel/control 负载未测（所有 census 臂 `control_cqe=0`）。
- 机器条件：E5-2666 v3、schedutil 1200–3500 MHz **governor/turbo 未改**、
  warm tmpfs、taskset -c 2（SMT 兄弟未隔离）、user-space PMU only
  （`measurement-review.md:18`、`decision.md:165`）。归因限制：K4-b session
  `cycles:u` 只降 4.2% 而进程 CPU 降 28.07%——内核侧归因继承自 K2 ungated
  session 的 0/1 perf record（enter 频率不同），仅 order-of-magnitude。

### 2.2 R1（E5 submission round，`docs/research/uring-submit-policy/round-e5-20261104/`）

冻结 base `271b81bf`（== #622 anchor）。裁决 **`KEEP_IMMEDIATE`**
（PRODUCT=HOLD，PRODUCT_PR=NONE，MERGED=NO 作为产品；研究记录已并入 master）。
D1/D2/D3 低深度边界：

| 深度 | 结果 | 判定 |
|---|---|---|
| D1 | +0.09% CPU / +0.48% p99（gate 不可 defer） | neutral PASS |
| D2 | **+4.17% CPU [+3.53,+4.83]**（CPU 界内）、**p95 +5.69% / p99 +7.45% / tput −3.99% 超冻结界**，Holm-rejected、已复现（+3.05/+4.98/+5.81/−2.95）、A/A 噪声底 0.5–1.61 pp；syscall 级解释：enters 3.0002→3.0002（每 pass 一条 held request，零摊销） | **FAIL（裁决决定项）** |
| D3 | −6.81% CPU / −11.57% p95 / tput +7.34%；enters 3.0002→2.0004 | PASS |

- D16/D64：主形状 W-02 4K d16 CPU/op **−26.46%** [−26.89,−26.02]、tput
  +36.07%、p99 −29.23%、enters 3.0002→0.5630（−81.2%）；saturate 0.5759；
  D16/D64 自然批量使 gate 生效。
- 64 KiB d16：**neutral**（CPU −0.72% [−2.03,+0.60]、p95 +0.02%、p99 −0.00%、
  tput −0.82%）——"4 KiB payload gate 阻止了 K4-b 的 64 KiB p99 +7.46%
  regression"（verdict block `PAYLOAD_TAILS`）。
- W-03 mixed-driver/split-owner：d4/16/64 全部 ±1.6% 内、enters 3.0002 不变
  （owner gate）；{2,12}+0.86%、{2,8}+1.58% 为 placement noise。
- residue signal：`P3_NOTIFICATION_COALESCING` 未做；per-deferral residue
  signal 保守保留，不声称合并（coalescing 0）。
- 机器条件（verdict block 逐字）：E5-2666 v3、kernel 7.2.11-200.fc44、
  gcc 16.2.1、liburing 2.13-2、native/bare-metal、**warm tmpfs、schedutil
  1.2–3.5 GHz、perf paranoid=2**、taskset -c 2、SMT {2,12}/cross {2,8} 检查。
- 重开条件：round-e5 `decision.md` §"Next research entry" RQ1–RQ4（RQ1
  batchability admission——"second held entry" guard 为首选，须过完整 control
  family 重门）；round2 去重表引用的
  `docs/research/uring-submit-policy/round-e5-20261104/RESUME.md:84-92`
  **在仓库与 bench 侧归档中均不存在**（悬空引用，记录为缺失项）；D1/D2/D3、
  64 KiB、W-03、cancel 家族是强制 falsifier（round2 报告 `A.4` 行）。

### 2.3 R2 causal-r2（PR #625，`docs/research/595-causal-r2/`）

A·IMMEDIATE vs B·BOUNDED_PROGRESS 的**归因轮**：D2 的 CPU（+3.70%/op）与
延迟抬升由 defer 决策链承担——源级 ESTABLISHED（CF-1 信号通知成本、CF-2
deferral 时序、CF-0 三臂锚定）；D3/D16 改善分解 PARTIAL。原型三变体
（worktree/timing/trace，base 271b81bf）保存在 pause `worktree-state/`。
机器条件：`taskset -c 2`、schedutil 1.2–3.5 GHz、64 MiB warm tmpfs
（`README.md:23-27`）。

### 2.4 R3 completion/continuation（PR #626/#627，`docs/research/595-r3/`）

**completion-driven flush**（完成时点批 flush）正式裁决（continuation
`VERDICT_CONTINUATION.md`）：`FAIL_MAIN_GATE`——W-03 d16 主门实测 CPU
−14.13pp/吞吐 +16.52pp 达标，但统一完整请求口径 **p99 +37.85pp（上界
+39.94 > +5 门）**；`MECH_9_3` enter −43.75% < 50% 亦 FAIL；W-02 D2 新算法
`NONINFERIOR_PASS_WITH_POWER_NOTE`（CPU +2.17 上界 +2.72、p99 +1.49 全界内；
但 AA 面 p99 半宽 3.431>3 → 该面 NOT_ESTABLISHED）；旧 B_R2 同判据 FAIL
（tput 上界 +3.889/p95 +5.906/p99 +9.067，strace 证明两臂 enter 完全相同——
退化是门链+flush 后移，非批量缺失）。产品 `HOLD_NO_REAL_APPLICATION`（#604
无 `READY_FOR_595`，`measurement-product/HOLD.md:28` 核查）。

进度/no-strand 义务：completion/deferred-ledger 形态下"无 stranded accepted"
（shutdown(drain) 必须 settle 已接受请求；OPEN1_RECEIPTS.md:202 的确定性
反例设计）；cap=8/suffix 对账闭合（accepted 20,000 = 1,355×8 + 9,160 单条；
defer=0）；26 场景 oracle GREEN + 3 mutant RED（completion VERDICT §2）。

### 2.5 R4 state-aware（PR #629，`docs/research/595-r4-state-aware/`）与 R5 phase-A/repair（PR #630）

R4（负结果）：R（READY_REUSE suffix re-entry）W03-d16 CPU −19.62%、tput
+24.5% 达标但 **p99 +21.85%（上界 +23.6%）FAIL** +5% 保护界（24/24 块同向）；
S（STATE_AWARE）对 R 无增益且 p95/p99 更差；保护格 d4 改善、
d2serial/W02 FAIL。机器条件核验项：schedutil、freq 1.2–3.5 GHz、tmpfs
31G（`BASELINE.md:15-28`）。

R5 phase-A：G1–G5 执行图、C1/C2 cleanup（18 场景逐字节等价 PROVEN）。
R5 repair round（负结果）：正式尾归因 **NOT_ESTABLISHED**（诊断仪器 on/off
扰动 −4.9/+9.2pp 超 ±3pp 冻结窗）；三臂正式门 C vs A0：CPU −22.09%
[−22.75,−21.41]（process-rusage；io-wq ticks 另采样，R/C≤A0，**不在 rusage
内，不能称全系统收益**）、tput +28.49%、p95 +1.15% PASS，**p99 +17.97%
（上界 +20.34 > +5，Holm p=1.0）FAIL**；保护格 W03-d4 全 PASS，
d2serial/W02-d1/W02-d2 FAIL（浅队列税）。C1/C2 保留依据 = 源级等价证明，
非已证明的普适性能收益（repair RESULTS.md Q2/Q3）。

### 2.6 pause archive（`docs/research/595-pause/`）

包含：各轮文档/协议/分析器/重跑脚本（冻结在 21 个分支，见
`ARCHIVE_INDEX.json`）；R2 三变体与 bounded-progress 未提交 diff 逐字保存在
`worktree-state/`；结论快照 + 恢复入口（`DECISION_SNAPSHOT.md`/`RESUME.md`）；
环境快照。**不含原始测量数据**（用户 2026-11-07 决定：数据之后重跑）——
R1–R5 的全部数字只能通过各轮分支内的 harness 重测复现，**不能离线复算**
（L2 验收明确不做）。恢复三级：L1 源码/文档（bundle 检出）、L2 分析复算
（不做）、L3 环境重建（不承诺同数字）。撤回表述清单（DECISION_SNAPSHOT §3，
不得复写）：如"校准后已证明无害"、"≥50%-of-gap 归因规则"（分位数不可相加）、
F5 首版位置解释等。

---

## 3. #622 E5 Native 成本地图（跨层观测能力研究）

权威来源：GitHub issue #622（jnhu76/Sluice，OPEN）正文 + 2026-11-03/04 回写
评论 id `6614830243`（本次经 `gh api` 取回）。仓库内无独立 #622 目录；本地
引用见 round-e5 `README.md:60-63`（"#622 E5 observability round: … io-wq/
eventfd cost map handed to #595 on 2026-11-03"）、round2 报告 `A.4` 行
（`EXISTING_TOOLS_SUFFICIENT / CUSTOM_TOOL NOT_JUSTIFIED`）。
证据根 `/home/jnhu/sluice-622-observability/` 在 E5 本机（**本机不存在**，
见 §6 缺失材料），manifest 1057 文件。base `271b81bf`；RESEARCH_ONLY/
NATIVE_E5_ONLY/NO_PRODUCT_CHANGE；`PRODUCT_FILES_CHANGED=ZERO`。

裁决：**Verdict A `EXISTING_TOOLS_SUFFICIENT`（H4 成立）/ `CUSTOM_TOOL
NOT_JUSTIFIED`（H5 不成立，不自建引擎）**；总门 `CONDITIONAL_GO`（5 条消费
边界：指标不相加、口径随数字走、ring-vs-noring −5.6..−15.4% 是布局 artifact、
wall 收益只能由 un-instrumented paired A/B 决定、段级延迟归因需产品补
publication 时戳）。

两个 top 热点（归因证据，裁决权移交 #595/#604/#617）：

- **H1** 结构性 1-SQE/io_uring_enter 提交形状（per-dispatch flush，
  `src/async/uring_transport.cpp:228,231`）：enters/op=1.0001 三源 direct
  （tracepoint + bpftrace to_submit={1:100%} + uringscope）跨 W-02 d1/d16/d64
  与 W-03；C1 控制实验 q1b1→q16b16 使 enters/op 1.0→0.0625、instr/op
  1123→393（−65%）、cyc/op 1835→282（−85%）。
- **H2** 每请求一次 io-wq punt（tmpfs 形态）：punts/op=1.0001、local work=0、
  iou-wrk 全 stime（进程 task-clock 40% / cycles 22–31% / dwell ≈25%，三量纲
  分列不相加）；handoff 边际成本 D16 +2,217 instr/op / +475 ns（D1 约 7.5×）；
  **FS 条件性：btrfs NOCOW warm buffered = 0% punt inline（io-wq 池从未启用）**
  ——C4 direct 对照，任何"消除 io-wq"叙事必须按 workload FS 分叉。
- 事件成本（W-03）：threadpool 逻辑 signal→eventfd write→drain→notification
  = **1:1:1:1**（gate 全串行，coalescing 0）；uring 内核
  `io_eventfd_signal` 合并 6.7（d16）→9.0（d64）signals/notification；
  调度延迟 NOT material（wakeup→sched_switch p50 768–1536 ns）。

### 已证明足够的观测路径（#622 交付 D1/D4）

| 路径 | 能力 | 开销（同 workload 口径） |
|---|---|---|
| perf（root：stat/record/report/annotate/sched/trace/c2c） | cycles/instructions/task-clock/branches/LLC/ref-cycles 精确计数（重复差 1.6e-8）；u/k 只经 `cycles:u/:k`；task-clock:u/k 有同 trap 问题；rusage u/s 在 syscall 密集负载下失真 | ~0%（−1%±noise） |
| PEBS / PMU | precise_ip=3；load-latency（data-address/snoop/TLB）；uncore IMC root-only；TMA 2/4 可信；generic stalled-cycles alias UNAVAILABLE；无 Intel PT/RDT MBM | — |
| bpftrace 0.24.2（BTF=y） | 四事件域全覆盖 + 用户栈符号化；map 聚合计数 0/50,000 丢失 @969K enter/s；printf 流模式 0.44% 丢失需自测 | +12.8% |
| Phase E 单程序 join（bpftrace + 离线 python） | **100% 六段 join**（dispatch→cookie→SQE→io-wq punt→CQE-gen→reap）60,000/60,000 × 3 run，0 丢事件；键 = (ctx/lifetime, user_data)+线程序；naive cookie 键 66.7% 误 scope、req-ptr 键 97.33–99.68% 误归属（负控实测） | 吞吐 −70.8%/−41.6%（仅结构性证据） |
| uringscope（frozen 011ed7b9） | io_uring 域 Tier FULL 12/12；gap 是内核 tracepoint 语义（SQPOLL 环 complete 仅 ~8% 发出、overflow CQE 无事件、drop 计数不可观测） | +102%（≈2.0×） |
| DynamoRIO（cronbuild-11.91.20756 + hotpc-atomic client） | 实例级用户态指令分类（含 PREFIX_LOCK）、per-PC 源行；内核侧 0%、内核建线程不可见 | 30.9× |
| strace | 仅计数 sanity（perturbative） | +947%..+2032%，**永不用于性能** |

Phase F 插桩有效性：36 判定（6 注入 × 6 观测面）= 22 DETECTED / 14
NOT_DETECTED，负控全保持；14 组 NOT_DETECTED 逐一归因（域外/灵敏度边界/
诚实负发现），**无一指向缺失观测能力**。

残余缺口（非 engine-grade）：publication→observation 逐请求时戳是产品 API
形状（`include/sluice/async/request.hpp:63-70` 无字段）；churned 线程 per-tid
PMU 不可用；内核源码行级 annotate 缺 kernel-debuginfo 包；设备 FS per-op
block join 未运行。若任一升级为裁决阻塞 → 规定路径是 Verdict B（薄离线编排
层），不是新引擎。

---

## 4. PR #634 与 PR #635 的改动与测试（当前 master 事实）

### 4.1 PR #634（merge `3f22d2d0`，branch `product/621-threadpool-owed-count-gate`）

生产改动（`b0dc4dc0`，5 files +496/−12，其中 parity test 403 行 + xmake 30 行）：

- `include/sluice/async/detail/threadpool_backend.hpp`：新增派生摘要
  `owed_count_`（注释声明 `work_mtx_` 域内 `#{record|event_owed}` 的精确
  计数）+ `owed_visible_locked_()`（读 `owed_count_ != 0`；mutant
  `SLUICE_THREADPOOL_MUTANT_OWED_EMPTY_VERDICT_ALWAYS` 时恒 false）+
  test-only 访问器与 `OwedSweepEpiloguePauseGate` seam。
- `src/async/threadpool_backend.cpp`：`publish_zero_op_inline` 内
  `++owed_count_`；`poll_progress` 的 per-pass 收集 sweep 整体包进
  `if (owed_count_ != 0)`（逐记录 `--owed_count_`）；`immediate_work_remains`
  改读 `owed_visible_locked_()`；settlement（`internal_work_retired`/
  `retire_execution_resources`）保持权威 `any_event_owed_locked_()` 扫描。
- 测试：`tests/threadpool_owed_counter_parity_test.cpp` 8 例（counter↔精确
  计数扫描↔存在性扫描↔sink parity，quiescent checkpoint 读取；3d7e0685 加入
  `owed_scan_count_for_test()` 强化）+ 4 个注册 mutant
  （`threadpool_owed_mut_*`，回执 exit 142 SIGALRM / 1 / 1 / 134 SIGABRT，
  `mutant-receipts.txt`）。矩阵 release/debug × liburing y/n =
  80/80、80/80、59/59、59/59；TSAN/ASAN clean。ledger：
  `docs/roadmap/v2-conformance.md` "Dated 621 ThreadPool owed-count
  productization (2026-11-12)"（机制 justification 8 问 + authority-contraction
  audit：无权威移动、无机制变为可删）。

### 4.2 PR #635（merge `5d94f233`，branch `product/621-progress-drain-single-read`）

生产改动（`27d907d6`，`include/sluice/async/detail/progress_source.hpp`
+45/−3；总 diff 4 files +758/−3，与测量候选 `a3a14a31` 唯一差异是删 2 行
test-only 注释，重建 `sluice-copy` sha256 仍为 `db972b19…`）：

- `drain_notification_nolock_()`：成功 read（`n==8`）后在同一 `mtx_` 临界区
  置 `notification_pending_ = false` 并 `return`——删除必然 `EAGAIN` 的第二次
  probe read；`EINTR` 重试、`EAGAIN` 分支、`PhysicalProbe`、
  `wake_notification_nolock_` 的 suppression 逻辑全部保留。
- test-only（`SLUICE_ASYNC_INTERNAL_TESTING`）：`DrainObservation`
  （reads/eagain/eintr 原子计数）+ `inject_drain_eintr_for_test`；
  mutant `SLUICE_PROGRESS_SOURCE_MUTANT_DRAIN_SUCCESS_KEEPS_PENDING`。
- 测试：`tests/progress_source_drain_semantic_oracle_test.cpp`（539 行，9 例，
  纯净 BASE green——只加注册不改生产头）+ `tests/progress_source_drain_mechanism_test.cpp`
  （129 行，4 例：非空 drain 恰 1 read/0 EAGAIN、空 drain 1 EAGAIN、EINTR
  重试、retire 后 0 read；不注册为测试、不参与 BASE-GREEN）；mutant 被恰好
  3 条语义 re-arm 断言杀死。矩阵 81/81、81/81、60/60、60/60；15 套件 exit 0；
  TSAN/ASAN 3 目标带插桩验证通过（r6 在 `evidence-r3/` 补齐拆分后回执）。
  ledger："Dated 621 ProgressSource drain single-read productization (2026-11-12)"
  + PROG-02 行；CHANGELOG `[Unreleased]` 两条 #621 条目（CHANGELOG.md:20-54）。
- 性能证据：CHANGELOG 引 session-r2r2（A/A 36 对宽 2.41pp 过门、保守 5.018%），
  与 §1.2 一致。

---

## 5. 复现资格审计（核心产出）

问题：哪些历史结果可能受 **CPU 频率资格** 影响（governor 状态、频率漂移、
下探）、影响是否足以改变判定、哪些必须重测、哪些与频率无关。

### 5.1 逐裁决审计

| # | 裁决/结论 | 依赖的测量 | 频率条件 | 频率敏感度 | 是否足以翻案 |
|---|---|---|---|---|---|
| 1 | **#595 R1 `KEEP_IMMEDIATE`** | D2 保护失败（p95 +5.69/p99 +7.45/tput −3.99，Holm，复现，A/A 噪声底 0.5–1.61pp）+ enters 3.0002 不变的计数解释 | schedutil 1.2–3.5 GHz 未改；配对 AB/BA；tmpfs | **低**。决定项是尾部/吞吐越界（幅度 4–7pp，远超 schedutil cycles 漂移 0.5–2.25%），且有频率无关的计数证据（enters 不变 = 零摊销的机制解释） | 否。频率噪声最多解释 1–2pp，不可能把 +5.69/+7.45/−3.99 拉回 ±界内。重开只按 RQ1 机制条件，不因频率 |
| 2 | **#595 R3/R4/R5 负结果（FAIL_MAIN_GATE / 负裁决）** | p99 +37.85pp（R3）、+23.6pp（R4）、+17.97pp（R5），配对 bootstrap、Holm | 同上（schedutil、tmpfs、双锁纪律） | **极低**。全部失败幅度 ≥18pp，远超频率噪声；CPU 侧通过项（−22.09%）方向也与频率无关（配对同 governor） | 否。保护格失败（浅队列税）是结构性延迟效应 |
| 3 | **#595 pause「数据不归档」** | — | — | — | 与频率无关的程序性事实，但使 R1–R5 全部数字**不可离线复算**，任何复用必须重测（§5.3） |
| 4 | **#622 `EXISTING_TOOLS_SUFFICIENT`** | Phase F 36 判定（注入检测）+ Phase E 100% join + D1 覆盖矩阵 | schedutil 不改 governor；cycles 漂移 0.5–2.25% 显式登记 | **几乎为零**。判定依据是计数/覆盖率/检测判定，非频率敏感量 | 否。成本地图的 cycles 百分比本身带漂移标注（支撑证据，非裁决） |
| 5 | **#634 GO** | CPU −5.58% [−6.45,−4.71] vs A/A ±1.2%；instr −22.1%（计数）；scan family 从 profile 消失 | governor=performance（测量期），两臂同条件；btrfs | **低**。配对设计 + 5.58% 幅度 ≫ 频率漂移；instructions 是频率无关计数。残余：turbo 3.25–3.35 GHz 两臂同等 | 否。但 **64 KiB/d1 负对照 +0.10% [−1.56,+1.77] 在频率/布局噪声带内，任何方向解读均无资格**（final-repair 已改措辞） |
| 6 | **#635 GO（r6，保守 5.018%）** | session-r2r2：instr −5.0180%（36/36）、cycles −5.1111%、task-clock −5.4653%、rusage −5.3199%；A/A 36 对宽 2.4090≤3.0；p99 −7.27% | governor=performance 设置/恢复均核验；**运行中频率下探已披露**（cpu2 最低 1,151,304 kHz，`freq_eq_all=false`，"持续 cur==max 未确认"） | **中低，已披露**。指令计数（频率无关）36/36 为负且是四组中最小绝对值，即保守量级本身选了频率无关口径；cycles/task-clock/rusage 三口径与它一致（差 ≤0.45pp）。两个受控 session（r2r1/r2r2）量级一致 −5.0~−5.5% | 不足以翻案，但**若在别的 governor（如全程 schedutil）或别的内核重测，5.018% 不是承诺值**（RESUME "内核升级后不承诺同数字"）。产品声明已限定 host/cell |
| 7 | **K4-b ELISION_PERF_SUPPORTED（D=4/16/64）** | 15 对配对 CPU −13~−43% + enters/op 2/D 模式 | schedutil 未改；user-space PMU | **低**（幅度大 + 计数证据）。但其**<3% 的次级数字**（D1 −2.50%、threadpool +1.19%、W-03 d4 +2.54%、64KiB CPU −2.99%）明确无归因资格 | 大判定否；小数字不进任何裁决 |
| 8 | **DP-1/DP-3A 编译器证明、H2b/E1 产品 A/B（v0.0.3）** | frame 字节数、指令/op；paired A/B（CHANGELOG.md:72-104：H2b 11 配置 paired wall median ≤0.994、headline 0.495；E1 W-02 headline 0.698/0.620、负控 ≤1.0004） | — | 编译器证明与频率无关；A/B 为配对同机口径，与 #634 同类 | 否 |

### 5.2 频率可疑、但**不构成裁决依据**的支撑证据

- #622 成本地图全部百分比（iou-wrk 40%/30.9%/25%、H1 的 −65%/−85% 等）：
  schedutil 漂移 0.5–2.25% + 三量纲不相加 + 5 条消费边界；只能作定位。
- round2 报告 B.1/B.2 成本台账：trampoline 26.93%、futex 15.44%、mutex
  12.74% 等为 profile 份额（B.2 已在 r6 复算并注明混合口径）。
- futex 五来源互斥计数（≈0.51/1.07/0.86–0.90/0.668/1.00）：已从机制证据
  撤销（round2 `D.6`）；其中 bpftrace 系计数本身受丢事件/覆盖影响，与频率
  无关但同样无定标。
- #621 旧 session spread（−1.4%~−12%，含 10 对 perf stat 的 −2.42%）：
  跨 session 成因未分解，频率下探/会话/布局均为候选；r6 已把它移出量级
  来源。**如未来要解释该 spread，频率/环境剂量实验是必做项。**
- K4-b `cycles:u` −4.2% vs 进程 CPU −28.07% 的 divergence 归因（继承自
  K2 ungated session）：order-of-magnitude 证据，不作内核归因结论。

### 5.3 必须重测的情形（触发条件 → 对象）

1. **任何 #595 重开**（RQ1 等）：R1–R5 原始数据未归档 → 全部数字只能按各轮
   分支 harness 重测（`595-pause/RESUME.md` §3），且先 A/A 再正式块；内核
   升级后不承诺同数字。
2. **产品准入换环境**：#622 NEXT_EXPERIMENT 明确 —— 产品二进制上设备 FS
   （btrfs NOCOW / O_DIRECT）重测 H2 的 FS 条件性；任何 wall 收益必须
   un-instrumented paired A/B。
3. **#635/#634 数字外推**：wall、能效、io_uring 端到端、多 worker、cold
   cache、真实 EINTR 均未测——引用即重测。
4. **一切 <3% 的效应**（含 64 KiB/d1 对照、K4-b 次级数字、round2 负对照）：
   在新 session 需要新的同 session A/A 才有归因资格；旧带内数字无资格。
5. **跨 session spread 解释**（可选，非阻断）：需频率/静默/布局剂量实验。

### 5.4 频率无关、不受影响的结论

- **计数类机制证据**：enters/op（census 三源）、`eventfd_read`
  262,135→131,073、`@read_eagain` →12/7、`sys_enter_read` 1.990→0.987、
  join 覆盖 100%、punts/op=1.0001、to_submit 直方图、cap/suffix 对账闭合。
- **确定性正确性证据**：全部 parity/语义 oracle、mutant 回执（exit 码）、
  四配置矩阵、TSAN/ASAN、逐字节 cmp oracle、18 场景等价证明、编译器证明
  （DP-1 frame 232→96、DP-3A 单 store）。
- **裁决的方向结构**：KEEP_IMMEDIATE、R3/R4/R5 FAIL、#634/#635 GO——
  它们建立在配对同条件 + 幅度≫噪声 + 计数佐证之上，频率资格不构成翻案通道。

### 5.5 一句话结论（frequencySuspectAudit 的展开）

受频率资格影响的**只有量级表述与 <3% 的小效应**：#622 的 cycles 百分比、
#621 旧 session spread、各轮负对照/次级数字；这些全部被原记录降级为支撑
证据或披露限制，没有任何裁决依赖它们。两项产品裁决（#634 −5.58%、#635
−5.0180%）与三项机制裁决的幅度均 ≥5pp 且配对同条件，频率噪声不足以翻案；
但 #635 的保守量级选在频率无关的 instructions 口径上、并在两轮受控 session
中复现一致（−5.0~−5.5%），是当前最抗频率质疑的数字。R1–R5 因原始数据不归档，
任何复用必须整机重测；K4-b 的 D2/D3 未测、cancel/control 未测、#635 的
io_uring 注入未建——这些是重测清单的固定成员，与频率无关。

---

## 6. 缺失材料清单（本地不存在，需另行索取）

| 材料 | 声明位置 | 本机状态 |
|---|---|---|
| #622 证据根 `/home/jnhu/sluice-622-observability/`（1057 文件 manifest、COST_MAP.md、TOOL_MATRIX.md、VALIDITY.md、DECISION.md、Phase E/F 归档、tar 包 `sluice-622-e5-evidence-20261104-043116.tar.gz`） | issue #622 回写评论 id 6614830243 | **缺失**（仅评论回写与tar包 sha256 `e9eea646…` 可引用） |
| #595 研究全史 bundle `/home/jnhu/sluice-research-archive/595/pause-20261107/source/sluice-595-research.bundle`（21 refs）与 R1–R5 原始测量数据 | `595-pause/README.md`、`RESUME.md` §1/§4 | **缺失**（bundle 与 raw data 均不归档；raw 按用户决定"之后重跑"） |
| R4/R5 的 E5 原始目录 `/home/jnhu/sluice-595-e5/`（r4/raw/、r5/ 逐 invocation 记录、诊断、trace） | `595-r4-state-aware/README.md`、`595-r5-phase-a/README.md:49` | **缺失**（E5-local only，仅 digest 在仓） |
| PR #634 轮的仓外证据根 `/home/jnhu/sluice-621-round/`（binaries、run_paired.sh、analyze_paired.py、results/*.tsv、perf_4k_sym.data、b_owd_count.diff） | `621-threadpool-owed-count REPORT.md:197-204` | **缺失**（仓内 formal-ab TSV/analysis/production.diff 已足够审计结论，但 raw perf data 与 runner 不可达） |
| round2 的 `/tmp/sluice-drain-probe/` 未归档探针（1000 轮 signal+ack、64 MiB consumer trace） | round2(r5) `C.5` 末段 | **按设计不归档、不被引用**；状态未验证 |
| `docs/research/uring-submit-policy/round-e5-20261104/RESUME.md`（round2 报告 `A.4` 与 r5 报告 `A.4` 行引用 `RESUME.md:84-92` 作为 #595 重开条件） | round2 报告去重表 | **缺失**（docs 侧与 `bench/research/uring-submit-policy/round-e5-20261104/` 均无此文件）；重开条件可回退到 round-e5 `decision.md` §Next research entry RQ1–RQ4 |
| 归档 mutant 复跑输出（round2 r5 报告说"审计复跑得到 3 条语义 FAIL 但输出未归档"） | round2(r5) `C.6` | r6 声称已在 `evidence-r3/` 补齐（mutant exit 1 恰 3 条语义 FAIL、restored exit 0）——本会话未逐一打开 `evidence-r3/` 逐文件核验，仅采信 r6 报告 §C.6/§C.7 的声明 |

GitHub 侧：`gh` 可用且网络可用；已取回 issue #622 正文与评论。#595/#621 的
issue 评论（含 id 6614846173 的 #622→#595 handoff 引用）未逐一取回——HOLD.md
的转述与 round-e5 README 的转述一致，未见矛盾。
