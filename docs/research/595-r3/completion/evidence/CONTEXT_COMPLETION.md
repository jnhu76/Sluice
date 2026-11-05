# #595 R3 续跑 — 上下文（CONTEXT_COMPLETION）

> 本文件当前只包含**基线部分**。续跑各阶段可在此追加自己的章节；基线事实以
> `BASELINE_COMPLETION.md` 与其 `raw/baseline/` 原始输出为准。

| 字段 | 值 |
|---|---|
| completionRoot | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z` |
| 生成时间（UTC） | 2026-11-05T08:38:18Z |
| baseSha | `0a22a79195f1b913145fcd443496e7f70b08cdcf` |
| R2 锚点 | `271b81bfc9d64eecc0266fe7066b9040525dd710` |
| R3 worktree | `/home/jnhu/sluice-595-e5/r3/worktree`（分支 `research-595-r3`，HEAD = baseSha，脏：6 M + 4 ??） |
| 共享测量锁 | `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`（存在，当前无持锁者） |
| 冻结状态 | **frozen = true**（原生 E5；主树 clean；worktree HEAD = 基线；前轮清单 246/246 + 33/33 可校验；锁可用） |

## 1. 基线（本轮冻结，不可变输入）

1. **平台**：原生 Intel Xeon E5-2666 v3 @ 2.90GHz，20 logical / SMT on，Fedora 44，
   kernel `7.2.11-200.fc44.x86_64`，g++ 16.2.1，xmake v3.1.2+HEAD.5d21e9d，
   python 3.14.8，liburing 2.13；cpu2 `schedutil` / `scaling_cur_freq=1200000` /
   siblings `2,12`；`perf_event_paranoid=2`；`unprivileged_bpf_disabled=2`；
   `/tmp` = 32 GiB tmpfs；`/home` = btrfs。
2. **主工作树** `/home/jnhu/Source/Sluice`：HEAD `0a22a791…`，`status --porcelain` 0 行。
3. **R3 worktree** `/home/jnhu/sluice-595-e5/r3/worktree`：HEAD `0a22a791…`，
   分支 `research-595-r3`，**唯一**属于 R3 的 worktree；当前脏文件（不得提交）：
   - M `bench/bench_common.hpp`、`bench/bench_w03_external_loop.cpp`、
     `include/sluice/async/detail/test_access.hpp`、
     `include/sluice/async/detail/uring_backend.hpp`、
     `src/async/uring_backend.cpp`、`xmake/tests.lua`
   - ?? `bench/r3_ready_batch.hpp`、`tests/uring_ready_batch_deterministic_test.cpp`、
     `tests/uring_ready_batch_fault_test.cpp`、`tests/uring_ready_batch_research_test.cpp`
   - `git diff --stat 0a22a791`：6 files, +514/−51（不含 4 个未跟踪文件）。
   - 公开头 `include/sluice/async/*.hpp`、v2-r1 文档 diff 为空。
4. **产品源码相对 R2 锚点**：仅 1 个提交 `0a22a791`，新增 2577 个文件全部在
   `docs/research/595-causal-r2/`；`include/src/apps/bench/tests/xmake` 零差异。
5. **前轮证据**：`/home/jnhu/sluice-595-e5/r3/evidence/`；主清单 246/246 OK，
   正确性清单 33/33 OK，`MANIFEST.sha256.hash` sidecar OK；
   `r3-final.patch` sha256 = `10ce088a4b3219ba57ee0e63fa8f46d506ac763abc73eea073f833ecb8ce1622`。
6. **当前二进制**（worktree `build/linux/x86_64/release/`）：
   - `bench_w02_pipeline` `00a5ca62…`
   - `bench_w03_external_loop` `af6e8afd…`
   - `uring_ready_batch_research_test` `e16d1379…`
   - `uring_ready_batch_fault_test` `88c74188…`
   - `uring_ready_batch_deterministic_test` `0ecb27e2…`
7. **R1/R2 只读**：`/home/jnhu/sluice-595-e5/evidence` 1854 文件、0 个 mtime >
   `2026-11-05T00:00Z`；`/home/jnhu/sluice-595-e5/worktree` 5451 文件、0 个 mtime >
   `2026-11-05T11:00Z`；Issue #595 OPEN（`updatedAt=2026-11-04T16:59:52Z`）、
   Issue #604 OPEN（body 无 `READY_FOR_595` handoff）、PR #625 OPEN。

## 2. 前轮可复用结论（R3 已交付）

| 结论 | 证据 | 可否复用 |
|---|---|---|
| 正确性：BASE GREEN（13/13）→ 3 个命名变异 RED → RESTORED GREEN；既有回归全绿 | `correctness/CORRECTNESS.md`、`correctness/MANIFEST.sha256`、`raw/correctness/*` | 可复用，前提是本轮不改动相关源码/测试/xmake 或重建二进制 |
| 就绪组普查：W-03 external loop 为 `REAL_EXPOSURE`（d16 91–93%、d4 72–73% 的 enter 可省）；W-02 主 shape 为 `STARTUP_ONLY`（20k 仅省 1 enter，0.005%）；`sluice-copy` 默认 `NO_REAL_BATCH_EXPOSURE` | `READY_EXPOSURE.md`、`census.json`、`raw/census/*` | 可复用；结论依赖 consumer 源码与基线二进制不变 |
| 机制/产品裁决：机制 `MECHANISM_ONLY`；产品 `KEEP_IMMEDIATE`；不建产品候选 PR | `VERDICT.md` | 可复用为前轮裁决记录 |
| 计时：A/A、A/B、负控、保护门、资源门全部 `NOT_RUN`；无 `PREREGISTERED_R3.md` | `PROTOCOL.md` §1/§6 | 不可复用为性能证据；续跑若要做计时必须按新冻结协议执行 |

## 3. 本轮改动对前轮结果的影响

本轮（基线核验）**未修改任何源码、测试、xmake 或二进制**，因此：

- 前轮确定性正确性结论、普查（census）结论与二进制哈希**在基线上仍然有效**，
  无需因本轮操作重验。
- 前轮计时结论不存在（全部 `NOT_RUN`），不存在“因改动失效”的计时数据。

**需要重验的触发条件**（续跑后续阶段若发生，必须重新执行对应证据）：

1. 修改 `src/async/uring_backend.cpp`、`include/sluice/async/detail/*`、
   `xmake/tests.lua`、`tests/uring_ready_batch_*` 或 `bench/r3_ready_batch.hpp`
   → 重跑确定性正确性（BASE 13 场景、3 变异 RED、RESTORED GREEN）、research/fault
   oracle、既有 uring 回归，并刷新二进制哈希与清单。
2. 修改 `bench/bench_w03_external_loop.cpp` / `bench/bench_w02_pipeline.cpp`
   或重建 bench 二进制 → 重跑相应 census/enter 计数，并刷新清单。
3. 任何重建 → 重新记录 5 个二进制 sha256；若测量结论引用二进制，需重新绑定 SHA。
4. 若续跑引入正式计时 → 必须先写预注册（臂、配对、A/A、负控、统计、停手条件），
   并全程持有 `MEASUREMENT.lock`；前轮 `PROTOCOL.md` 的 `NOT_RUN` 项不得被引用为已测。

## 4. 续跑约束（继承）

- 不重建/删除/rebase R3 worktree；不提交其脏文件。
- 不改主工作树、公开 API、v2-r1 文档、远端、Issue/PR 状态、R1/R2 原始证据。
- 构建/测量/正确性运行使用共享 `MEASUREMENT.lock`（前轮 P-5 的偏离不得重复）。
- 不把 smoke、单次运行或 syscall 计数当作性能收益；前轮所有 `NOT_RUN` 项仍需显式标注。

---

## 5. 续跑冻结臂定义（OPEN-1 解除，2026-11-05）

> 本节只冻结**干预定义**，不产生性能结论；`semanticsOk` 仍未证明。前轮
> `A0/A1/B` 只是 `BUILD.md` §3.4 的研究标签，`A_R2/B_R2` 无定义
> （`CONTEXT.md` §7.2、`PROTOCOL.md` P-4、`VERDICT.md` V-7）。逐项证明义务与
> 触发交错见 `OPEN1_RECEIPTS.md` §3。

### 5.1 公共约束

1. 同一臂对应同一二进制 SHA：A0/A1/B 共用 R3 研究二进制；A_R2/B_R2 共用
   R2 研究二进制。不得跨二进制把布局差异解释成臂差。
2. 所有臂使用同一 case、fixture、输入顺序、buffer、byte oracle；arm 只改变
   提交路径，不改变 `ReadOp` 的 `(fd,dst,len,offset)`。
3. `ReadyGroup` 仍按 `CONTEXT.md` §6：首提交前已拥有、offset/长度确定、
   buffer 有效、依赖已确定；禁止新增等待/前移依赖/追加预取造组。
4. 计时协议沿用 `CONTEXT.md` §8：24 对/phase、`WARMUP=1`、`MEASURED=5`、
   `taskset -c 2`、warm tmpfs、AB/BA、bootstrap 10k/seed 5950、A/A 5/3/3 pp、
   负控 5/5/5/3。平台修正按 §8.1：cpu2 的 SMT 兄弟 cpu12 必须空闲或与
   `2,12` 一同征用；记录 `schedutil`/`scaling_cur_freq`；`perf_event_paranoid=2`、
   `unprivileged_bpf_disabled=2` 的权限限制必须记录。
5. 所有 build/test/timing 持有 `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`；
   前轮 P-5（`PROTOCOL.md`）的未持锁偏离不得重复。

### 5.2 A0 = IMMEDIATE（baseSha 原始 consumer 循环）

冻结 `baseSha=0a22a791…` 的 W-03 external-loop consumer 补发循环
（`bench/bench_w03_external_loop.cpp:339-356` 原版）：在
`window.size()<depth && submitted<request_count` 的循环体内，按原位置逐条构造
`ReadOp` 并立即调用 `ctx.submit_read(op)`；成功的 `Request` 立即进入 `window`，
失败即 workload failure。**不构造 `std::vector<ReadOp> ready`、不预扫描、不分配
组缓冲、不调用 adapter/backend batch entry**。产品 case（W-03 d4/d16/d64）的
`max_refill=depth`，与 baseSha 原循环逐字等价；研究 D2 case 的唯一额外参数是
`max_refill`（只限制 refill burst 上限），A0 仍逐条构造/立即提交，不预构造 span。
A0 可记录 `AsyncStats`，但统计不得改变提交路径；不使用 `bench/r3_ready_batch.hpp`
的 `Adapter`。

### 5.3 A1 = ADAPTER_IMMEDIATE

使用与 B 完全相同的 ready-group 准备和输入：在补发点计算
`want=min(depth-window.size, request_count-submitted, max_refill)`，按输入顺序
构造 `ready[0..want)` 的 `ReadOp`，然后才开始提交。逐条调用**同一个 Context
内部研究入口**（A1 与 B 必须调用同一函数；A1 每次传长度 1 的 span），每个成员
经过与该入口整组调用相同的 context 级 precheck、`access_mtx_`、admission/tally
与 `Request` 物化路径。A1 **不得**直接调用 backend seam，**不得**跳过
`initiation_rejection`/`access_mtx_`；否则它不是 A1，而是前轮 B 的 bypass 变体。
成功后立即物化 `Request<std::size_t>` 并进入 `window`。

### 5.4 B = READY_AWARE

使用与 A1 相同的 ready-group 准备/输入/case/budget；组必须是 `CONTEXT.md` §6
的合法 `ReadyGroup`，不得等待/预取/前移依赖造组。组合法可用时，按实际 ready
数量与预算选批：前缀长度 = `min(ready.size(), ready_batch_cap=8, SQ 空闲,
request capacity, budget)`；然后**一次**调用与 A1 相同的 Context 内部研究入口，
传入该前缀 span；入口在**同一调用内**完成 accept→prepare→dispatch→一次 flush，
**不跨调用持有 SQE、不 defer flush**。单成员（或没有合法组）时走与 A1 完全相同的
单成员等价 immediate 路径（同一入口、span=1）。若前缀后仍有 suffix
（cap/SQ budget/control/recovery/ineligible 等停止原因），suffix 由调用者从
`untouched_begin` 开始逐条走**同一入口** span=1；suffix **不二次成组**（这是 B 的
冻结策略；任何 suffix 二次批量都是新臂，不能冒充 B）。B 必须对每个 accepted key
物化 `Request`，并满足 `BUILD.md` §3.2 的 disposition 契约。**B 不得绕过 Context**；
若实现仍直接调用 `backend_->submit_ready_reads`，该臂只能叫 `B_bypass`，不能用于
`B−A1` 或产品问题。

### 5.5 A_R2 = IMMEDIATE at 271b81bf

二进制/树：`271b81bfc9d64eecc0266fe7066b9040525dd710` + R2 研究补丁
`/home/jnhu/sluice-595-e5/evidence/measurement/patch.diff`
（sha256 `7be9d0145a8c268a62d9fa90338b7cda95cefc5f8ad2976e4706cb192fb1eaf0`；
R2 `PROTOCOL.md:15-27` 记录的 `git diff HEAD` sha256
`b006dbbf751f5fb2bbe8fb9edb091ae66e57a181b8cf2470d880c9589e1f4072`）。
产品树 `271b81bf` 本身没有 `bounded_progress_`；该成员由 R2 研究补丁加入
（R2 研究 worktree `src/async/uring_backend.cpp:170-171`、header `:354-357`）。
因此 A_R2/B_R2 必须从 `271b81bf + patch` 构建，不能直接拿主树 `271b81bf` 构建后
声称有 A/B 两臂。

A_R2 行为：`SLUICE_595_ARM` 未设置或非 `B` → `bounded_progress_=false`；
`defer_transport_flush_locked_()` 第一检查 `if (!bounded_progress_) return false;`
（R2 `src/async/uring_transport.cpp:229-233`）立即早退；`dispatch_after_accept`
在 dispatch 点立即 `submit_transport_locked()`（R2 `src/async/uring_transport.cpp:84-90`），
不执行 owner/payload/witness/cap，也不 defer。仅用于 R2 原 W-02 D2
（`W02-uring-4k-d2`，需 `SLUICE_BENCH_595_DEPTHS=1`），回答“同 SHA 旧 B 是否仍比
IMMEDIATE 差”；它不是 R3 新算法，也不是 baseSha 产品路径。

### 5.6 B_R2 = BOUNDED_PROGRESS at 271b81bf

同一 `271b81bf + patch` 二进制；`SLUICE_595_ARM=B` → `bounded_progress_=true`。
`defer_transport_flush_locked_()` 的完整门链，顺序为
（R2 `src/async/uring_transport.cpp:229-263`）：

1. `bounded_progress_` 已为 true（不早退）；
2. **owner**：`progress_driver_seen_ && std::this_thread::get_id() ==
   progress_driver_recent_`，否则返回 false；
3. **payload**：`prepared_ops_[h.slot.value].kind == read && native_length == 4096`，
   否则返回 false；
4. **witness**：`live_cookies_.load(relaxed) > transport_ledger_->size()`，否则返回
   false；
5. **cap**：`transport_ledger_->size() < kDeferCap`（`kDeferCap=8`，R2 header
   `:287-299`），否则返回 false。

全部门通过 → 返回 true；`dispatch_after_accept` 只执行
`service_pending_controls_locked_()`，**不**调用 `submit_transport_locked()`，
flush 延迟到后续 progress 边界。任一失败 → 立即 submit（与 A_R2 相同）。
R2 源码/文档没有写的细节不得补写成定义；只用 R2 原文和上述源码可核验事实
（`CF0_THREE_ARM.md:26-42`、R2 `PROTOCOL.md:26-27`）。仅用于与 A_R2 同一二进制的
W-02 D2 对照；历史 R2 数值（`docs/research/595-causal-r2/RESULTS.md:38`）不得复用。

### 5.7 二进制/开关映射

| 臂 | 构建树 | 运行时选择 | case |
|---|---|---|---|
| A0 | R3 研究二进制（baseSha + completion patch） | `SLUICE_595_R3_ARM` 缺省/A0 | W-03 产品 d4/d16/d64；研究 D2/D3/D16 用 `SLUICE_595_R3_CASES=1` |
| A1 | 同一 R3 二进制 | `SLUICE_595_R3_ARM=A1` | 同上 |
| B | 同一 R3 二进制 | `SLUICE_595_R3_ARM=B` | 同上 |
| A_R2 | R2 研究二进制（`271b81bf` + `7be9d014…`） | `SLUICE_595_ARM` 缺省/A | W-02 D2（`SLUICE_BENCH_595_DEPTHS=1`） |
| B_R2 | 同一 R2 二进制 | `SLUICE_595_ARM=B` | 同一 W-02 D2 |

同一 build 内不得出现两个臂的二进制布局/编译宏差异；`r3_*` 与
`SLUICE_BENCH_STATS` 只作诊断，不作性能结论。

## 6. 证明义务索引（详见 `OPEN1_RECEIPTS.md` §3）

| ID | 交错/问题 | 状态 |
|---|---|---|
| OPEN-1.1 | 臂定义与二进制映射缺口 | open |
| OPEN-1.2 | Context 入口/锁顺序；A1/B 不同入口 | open |
| OPEN-1.3 | 并发 submit 与 driver；residue 唤醒 | open |
| OPEN-1.4 | 并发 close/admission/shutdown 与 `have_ring_` 生命周期 | open |
| OPEN-1.5 | 逐请求 acceptance 线性化与 accepted 前缀结算 | open |
| OPEN-1.6 | accepted→Request 物化与 borrow 边界 | open |
| OPEN-1.7 | partial submit 前缀归属 | open |
| OPEN-1.8 | SQ full / cap / budget 尾部归属 | open |
| OPEN-1.9 | retry 责任与 residue 信号 | open |
| OPEN-1.10 | accepted 不得作为 suffix；整组不得重发 | open |
| OPEN-1.11 | cancel/control 与批量交错 | open |
| OPEN-1.12 | overflow/poison 下的成员归属与结算 | open |
| OPEN-1.13 | stale generation / slot reuse / borrow | open |
| OPEN-1.14 | bounded resources / progress storage | open |
| OPEN-1.15 | 测量映射、归因与门槛 | open |
| OPEN-1.16 | #604 真实应用产品门 | blocked |
| OPEN-1.17 | A0 复原与旧结果重验 | open |

`open` 的含义是“义务已写入、尚未由本轮执行/证明”；不是 failure 判定。
`blocked` 只用于 #604：Issue #604 当前 `OPEN`，本轮只读核验无 `READY_FOR_595`
handoff；W-03 是合成 benchmark，不能替代真实应用产品门。

## 7. 原方案 → 续跑方案的主形状/假设/主门差异

| 维度 | 原 R3（前轮） | 续跑 completion |
|---|---|---|
| 主形状 | D2 两 ready（`W02-uring-4k-d2`）；主产品形状 W-02 | **W-03 d16** 为主形状；D2 单条补发负控、D2 两 ready 正控 |
| 产品门 | 名义 `B−A0` 产品净胜门；W-02 主 shape 无真实批暴露 | **#604 真实应用为产品门**；W-03 只是代表 trace；W-02 D2 回答“是否守住 IMMEDIATE” |
| A0 | adapter 路径，先构造 ready span；含共同准备开销（`VERDICT.md:98`） | baseSha 原始逐条循环；不构造 span/不预扫描/不分配组缓冲 |
| A1 | `ctx.submit_read` 逐条；与 B 不同入口 | 与 B 同一 Context 内部研究入口，逐条 span=1；相同 ready 准备 |
| B | 直接 backend seam、cap=8、suffix 逐条；绕过 context | 同一 Context 入口整组调用；同一调用内 accept→prepare→dispatch→一次 flush；单成员等价 immediate；suffix 逐条 |
| A_R2/B_R2 | 无定义 | 271b81bf + R2 patch 的 A/B 两臂；仅 R2 原文/源码；W-02 D2 |
| 尾部策略 | cap=8、suffix 逐条 | 不变（B 冻结为 cap=8、suffix 单条；suffix 二次批量是新臂） |
| 测量状态 | 全部 `NOT_RUN` | 本文件仍 **NOT_RUN**；只冻结定义/义务 |

关键假设：A0/A1/B 同一 R3 二进制，A_R2/B_R2 同一 R2 二进制；A1/B 的 ready span
在首提交前合法拥有；B 的组是 consumer 已有 refill burst，不是造组；计时主机
E5、cpu12 空闲/被征用、schedutil 频率记录、A/A 先过；旧 R2 历史数值不在本轮复用。

## 8. 重验触发与限制（继承并具体化）

1. 改 `src/async/async_io_context.cpp` 或 `include/sluice/async/detail/uring_backend.hpp`/
   `src/async/uring_backend.cpp` → 重跑确定性正确性 BASE 13、3 个命名变异 RED、
   RESTORED GREEN、research/fault oracle、6 个既有回归、`shutdown_lifecycle_test`；
   刷新二进制 SHA/清单。
2. 改 `bench/bench_w03_external_loop.cpp`（A0 复原、arm/case/计数）→ 重跑 W-03
   canonical census、`strace io_uring_enter` 计数、byte oracle、arm 计数；刷新 SHA。
3. 任何 rebuild → 重新记录 5+ 二进制 sha256；引用二进制的测量需重新绑定 SHA；
   A/A 重新校准（`CONTEXT.md` §5 硬约束 7、§13 注意 3）。
4. 正式计时 → 先写预注册（臂、配对、A/A、负控、统计、停手条件），全程持
   `MEASUREMENT.lock`；前轮 `NOT_RUN` 项不得引用为已测。
5. 前轮 A0/smoke/组计数不得直接复用：A0 必须按 §5.2 复原；smoke 计数受调度影响
   不是常量（`PROTOCOL.md` P-10）。
6. R2 历史 D2 数值不得复用：必须重建 `271b81bf + 7be9d014…` 二进制、重做 R2 A/A、
   再跑 A_R2/B_R2 24 对。
7. 前轮 `semanticsOk=true` 是实现者自检（`BUILD.md:8`），不继承；本轮
   `open1Resolved=true` 只表示定义/义务已写入，不表示语义已证明。
8. 当前 worktree 与 `r3-final.patch` 无差异（reverse-check 已由 OPEN-1 审阅执行）；
   它是前轮最终态，不是 completion 实现。任何 completion 变更必须显式列出并触发
   对应重验。
9. `have_ring_` 的并发 close/shutdown 竞争（OPEN-1.4）在解决前，不得声称批量入口
   在 shutdown 并发下安全；不得用“调用者不应并发”的口头假设代替契约/实现证明。
10. 产品门：Issue #604 `OPEN` 且无 `READY_FOR_595` handoff；W-03 的收益/无退化
    只回答机制/代表形状问题，不能替代 #604 真实应用产品门。`KEEP_IMMEDIATE`
    的最终改变必须有 #604 准入 workload + 冻结 oracle/预算/baseline + 负控 + 产品
    净胜数据，并更新 v2 Ledger。
