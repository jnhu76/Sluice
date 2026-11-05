# REPORT — #595 调度信息供给与缺口审计（Flash，2026-11-05）

冻结对象见 `BASELINE.md`；结构化清单见 `information-inventory.json`（30 项）；
复算见 `recalc_output.txt`（重跑命令在文末）。

## 0. 执行结论（≤500 字）

PRODUCT 的提交决策实际读取的信息只有三类：RequestCore 准入（槽位+admission_open）、
ProgressPass 后置事实（immediate/accepted/retry/health）、以及 pass 边界快照
（cq_ready、publication_pending 深度）。这些都是 USED_NOW 的权威或契约事实。
其余大量状态——dispatch 深度、ledger 残留、router/cookie 在途数、SQ 余量——
全部**在决策点可读但无人读取**（READABLE_AT_DECISION）；产品根本没有"批量"
决策点可供消费它们。R3 原型新增的唯一决策输入是：调用者呈现的 ready span、
SQ 余量、以及 stop 原因分类（cap/sq_budget/control/recovery/ineligible）。
对照参考算法：mq-deadline 的请求年龄、Kyber 的延迟直方图、VLDB 的
waiting-fibers 计数、Shenango 的周期采样拥塞信号在 Sluice 均 NOT_AVAILABLE
或 CALLER_HINT_REQUIRED，且其动作（排队调度/深度调节/核分配）与"摊销 enter"
不对应。**不缺信息，缺的是对已返回信息的第二次使用**：R3 每组呈现约 15.61 条
（RECALCULATED），前缀 cap=8 成批，剩余 ~7.61 条却逐条提交——每条一次独立
enter，把 prefix 省下的 enter 又逐条还回去。stop=cap 与 untouched_begin 已经
由批量入口原样返回，对其重入同一入口即可把每 16 条的 9 次 enter 降为 2 次，
零新增状态、零新增 hint。因此：**只凭现有信息即可做更好的无等待批量——YES**；
唯一最优先动作是 REUSE_EXISTING_STATE（suffix 成批重入），先于任何新信息。

## 1. 信息供给表（PRODUCT vs R3，完整版见 inventory）

| 信息 | PRODUCT | R3 | availability |
|---|---|---|---|
| caller ready 集合/组边界/顺序 | 逐条构造，集合不存在 | delta_w03 呈现 span | CALLER_ONLY |
| 准入（槽位/admission_open） | reserve 决策 | 同左（admit_） | USED_NOW |
| dispatch 深度 | 仅 empty 判断+drain | 同左 | READABLE_AT_DECISION |
| ledger 残留（已 prep 未消费） | empty 判断+retry 事实 | 同左+batch gate | USED_NOW |
| 内核在途 data（cookie/router） | 仅 shutdown 收敛 | 同左 | READABLE_AT_DECISION |
| cancel/control SQE | 服务+唤醒 | 同左+batch stop=control | USED_NOW |
| CQ 可取/overflow | reap 边界+park 探针 | 同左 | USED_NOW |
| publication_pending/owed | pass 边界+残留事实 | 同左 | USED_NOW |
| outstanding 汇总（派生） | accepted_work_remains | 同左 | USED_NOW |
| SQ 余量 sq_space_left | 不读（仅 reconcile 用 sq_ready） | batch 上限 | R3 USED_NOW |
| stop 原因/untouched_begin | 无此入口 | 批量入口返回，suffix 只用 untouched | R3 USED_NOW |
| 请求年龄/oldest-ready | 无任何时间戳字段 | 无 | NOT_AVAILABLE |
| 延迟直方图（库内） | 无（AsyncStats 纯计数） | 无 | NOT_AVAILABLE |
| about-to-park/runnable/fibers | 无 | 无 | CALLER_HINT_REQUIRED |
| deadline/priority | 无（wait 期限≠I/O 期限） | 无 | NOT_AVAILABLE |
| ready_ns / enter 直方图 / buckets | 无 | bench+strace | BENCH/DIAGNOSTIC_ONLY |
| ready 组大小分布 | — | 不可恢复（只有和值：呈现均 15.61=前缀 8.00+后缀） | UNAVAILABLE |

## 2. 方法信息差矩阵（全部一手资料，详见 BASELINE §3）

| 方法 | 必需信息 | 控制的动作 | Sluice 对应 | 缺口 | 最小获取 | 值否 |
|---|---|---|---|---|---|---|
| IX (OSDI'14) | 是否已有积压；批上限 | **只消费已到达积压，绝不等待成批**；上限防缓存/饿死 | R3 的 prefix+suffix 已实现"绝不等待"半条；上限=cap8 固定 | 自适应上限的负载信号（presented.size 即可） | 决策点已有 ready.size() | 部分：上限自适应非瓶颈（stop=cap 恒为 8/16，先修 suffix） |
| VLDB26 §3.3.3 | outstanding I/O 数；waiting fibers 数 | outstanding 多→推迟 enter 攒批；少→提前 flush | 分子=outstanding 可读；分母=运行时私有 | waiting-fibers | CALLER_HINT_REQUIRED（库无法猜） | 暂缓：其收益前提是拥有 fiber 调度器的 runtime |
| mq-deadline v6.17 | 每请求 fifo_time 年龄/期限/方向/类 | 到期优先；批间才查到期；防饿 | 无年龄字段（Slot/RouterEntry 均无） | 每请求时间戳（新状态） | 不加：立即路径无队可老化；R3 残留 <1 个 refill | 否 |
| Kyber v6.17 | 域延迟直方图 vs 目标；在途 token | **调在途深度**（非提交批量） | 库内无延迟测量；outstanding 即在途 | 直方图+目标输入 | 每请求完成时间戳+策略输入 | 否：动作不对应 enter 摊销 |
| Shenango NSDI'19 | 队列长度跨 5µs 检查周期的导数 | 核分配（资源） | 队列深度精确但只在 pass 边界被看 | 周期采样器+核分配职责 | DERIVABLE_WITH_LOCAL_COST | 否：超出库边界 |
| NAPI 6.12 | work budget；完成量；预算尽有残留 | 残留→继续服务义务 | **已具备**：pass 边界快照+immediate_work_remains+PROG-01/02 | 无 | 无需 | 已达标，勿动 |

关键区分核对：IX/VLDB 是"等新请求成批" vs "消费当前已 ready"——Sluice 惟一合法
形态是后者（IMMEDIATE；R3 统一口径下 p95/p99 越界正是组准备/提交把延迟转嫁给
早到成员，不是等待成批）；批量大小≠在途深度≠pass 预算（I11）；统计延迟目标≠
单请求硬期限（I26）。Shenango 摘要按二手证据更正：信号是队列长度**导数**
（增长），非"元素未推进"；动作是核分配——原表述已修正。

## 3. 安全/同步/成本分析

- stop=cap 重入不触碰任何义务路径：control/recovery/ineligible/rejected 分支
  保持现有逐条回退（R3 入口已实现）；poison/重试/shutdown 语义不变
  （I27 全部 USED_NOW 且为硬停止）。
- 全部读取已在本轮锁内：dispatch_mtx_（stop/untouched/sq_space_left），
  无新锁、无新原子、无跨核行、无时钟读。suffix 重入是**决策规则变化**，
  非新权威（TERM-01 派生摘要纪律不触发）。
- 成本每 pass/每批一次（重入 ~1–2 次/组），非每 Request；无逐请求扫描。
  绝对 ns 成本 UNMEASURED（本轮不做计时主张）。
- AsyncStats 关闭时上述信息全部存在（它们在 backend 状态，不在 stats）；
  atomic（live_cookies_/submit_flushes_）只是计数，不构成多字段一致快照，
  本方案不新增任何此类依赖。

## 4. 候选排名（≤3）

1. **REUSE_EXISTING_STATE：suffix 成批重入**（stop==cap/sq_budget 时对
   ready[untouched_begin..] 重入同一批量入口，直至 input_exhausted/义务停止）。
   消除的机器步骤：每组成员的单项 enter（复算：B 臂 kernel 侧 {1:30000,8:3750}
   → 预期转向 {8:…} 主导；per-group 9 enters→2）。反例核查：D2 呈现=1 →
   input_exhausted，行为逐位不变（W02 非劣负控）；partial submit（sq_budget）
   → 余量重入；split owner：入口本就在提交线程；shutdown/poison → recovery
   停止，回退逐条。陈旧/错误信息不适用——不消费任何 hint。
2. **ADD_INTERNAL_HINT（IX 式自适应 cap）**：以 presented.size 为上限信号。
   暂缓：recalc 显示 stop=cap 恒定（前缀恒 8），瓶颈在 suffix 形态而非 cap 值；
   在候选 1 验证前调整 cap 会混淆变量。
3. **REQUEST_CALLER_HINT（about_to_park / runnable_work）**：HOLD。W03 证据
   （B−A1 p95 −26.5 与 A1−A0 p95 +71.0，RESULTS §1）表明批内等待/组提交本身
   转嫁延迟；库内制造等待违反 IMMEDIATE；无运行时不得臆造 public API。

## 5. 决定与唯一下一步实验（本轮不实施）

**NEXT_ACTION = REUSE_EXISTING_STATE**（单一实验）：

- 冻结基线：PRODUCT `48d9be5e`；R3 冻结 patch `2596f204…` + delta
  `57f21e02…/4aa7b306…`（本实验在其上再叠 bench/lib 单一 delta；E5 复测持
  MEASUREMENT.lock）。候选=R3-B（suffix 重入）；对照=已冻结 R3-B（suffix 逐条）
  与 A0；另一对照=每臂 strace enter 直方图（§9.3 同法）。
- 固定请求与语义：W03-uring-4k-d16 与 W02-uring-4k-d2（与续跑轮完全同参：
  tmpfs、page-cache-resident、20k ops、统一 ready_ns 口径）； eligibility
  （len==4096）与 cap=8 不变；refill 粒度不变；无新 hint、无新计数。
- 一次只改一处：仅 suffix 提交形态（决策规则），prefix/admission/dispatch/
  publication 逐字节相同。
- 负控：(a) W02 呈现=1 时二进制行为须与冻结逐位等价（d2serial enter 全 1）；
  (b) stop=control/recovery/ineligible/rejected 路径回退逐条，26 场景确定性
  套件 + research/fault + shutdown_lifecycle 全绿后方可计时；(c) 信息失效负控
  =故意丢弃 stop（强制逐条）应精确复现冻结 B 的 enter 直方图。
- 指标：统一 ready→observed 全请求延迟（p50/p95/p99）、CPU/op、吞吐、全参与
  执行者 CPU（buckets 采样：app/driver/iou-wrk/in-thread-kernel）、enter 直方
  图、机制计数恒等式闭合。
- 计时前冻结门：预注册判定与门槛沿用主预注册 §6/§9（bootstrap 10k、seed 5950、
  Holm；W03 主门 p95/p99 上界 ≤+5% 且 enter −≥50%；W02 非劣 5/5/5/3）+ 停手
  条件：A/A 功效不足即停（记 NOT_ESTABLISHED），尾延迟越门即 FAIL 并停止触发
  后续保护链。有仪器计时不得作为产品收益证据；维护成本若需测量另行小探针，
  否则记 UNMEASURED。

## 6. 对必答问题的直接回答

- 产品提供什么 / R3 多什么：见 §1 表——R3 只多三样：呈现 span、SQ 余量上限、
  stop 分类报告。
- 调度器实际读取 vs 仅可观测：USED_NOW 9 项 vs READABLE_AT_DECISION 4 项+
  DERIVABLE 1 项（inventory availability 字段）。
- 相比参考算法真正缺什么：无"能改变当前动作"的缺口；IX/VLDB/ Kyber 所需信息
  要么已存在未被使用，要么属于调用者/运行时（I20/I26），且其动作与 enter 摊销
  不对应。
- 仅凭现有信息能否更好的无等待批量：**YES**（候选 1）。
- 必须由应用提供、不能靠 backend 猜测：about-to-park、runnable work、组边界/
  依赖、真实 delay budget（I20/I25/I26）。
- 新增机器工作：候选 1=0 新状态；候选 3=每 park 周期一次 hint+失效回退。
- 下一步：删开销/复用状态优先于加信息——**REUSE_EXISTING_STATE**。

## 7. 复算重跑命令

```bash
python3 -I research/595-information-audit/scripts/recalc_continuation_counts.py /tmp/sluice-pr627/docs/research/595-r3/continuation/raw
# /tmp/sluice-pr627 = git worktree add --detach /tmp/sluice-pr627 0ac2b582c36f0a9013cfc1b3d024fd3bf258d837
```

证据标签：源码=SOURCE_VERIFIED（48d9be5e / 0ac2b582 直接读取）；归档计数=
RECALCULATED（独立复算）；strace/buckets 原始记录=RAW_VERIFIED_BY_FLASH（直接
读取）；Shenango=PARTIALLY_VERIFIED（二手印证，全文拉取失败）；读取成本=
UNMEASURED；性能收益预测=无（NONE_NEW）。无独立第二审阅实例，不声称独立复核。
