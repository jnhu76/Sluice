# P3-W1-DESIGN — ProgressSource poll 等待路径语义与设计准入评估

| 字段 | 值 |
|---|---|
| 角色 | 契约与并发语义审计员（只读源码与证据；只写本文件；不修改产品代码） |
| 日期 | 2026-11-18 |
| 战役 BASE | `5d94f233967c3aef993eb19ba9e3252c532b0951`（`HEAD` / `origin/master`；本轮未改产品源码） |
| 评估对象 | W1 `ProgressSource` poll 等待路径：`include/sluice/async/detail/progress_source.hpp:104-232` + `src/async/async_io_context.cpp:573-705` |
| 本文件性质 | 设计准入评估。不是测量、不是 A/B、不是合并授权；不替代 root、v2 ledger 或 `CAMPAIGN-LEDGER.md` |
| 输入 | `CAMPAIGN-LEDGER.md` §5.1 W1 行、§2.5；`p1/AA-CALIBRATION.md`、`p1/AA-CALIBRATION-2.md`；`p2/P2-COST-MAP.md`、`p2/P2-REVIEW.md`；源码与模型；`docs/evidence/621-round2-R1/REPORT.md`（未跟踪工作稿，已被 R2 r6 取代，只作线索） |
| 本轮未运行 | `xmake` 构建/测试、A/A、A/B、perf、eBPF、TLC、`scripts/verify_progress_source_model.sh`、任何 bench。仅执行了源码读取、`grep`、`dup` 文件状态标志演示与基于已发布 SD 的 required-n 算术复算（见 §10） |

---

## 0. 结论摘要

**判词：`NEEDS_OWNER`。**

理由按优先级：

1. **当前 poll/park 路径上唯一可能 material 的最小减法方向（把 `poll` 换成阻塞 eventfd read）在当前 root/拓扑下是 `SEMANTIC_MISMATCH`**，不是“还没证明性能”的问题：
   - 同一 fd 不能 `dup` 后改 blocking：`dup` 共享 open-file description 与文件状态标志；本会话已用 `os.dup` + `fcntl(F_SETFL)` 复现（§3.4、§10）。源码也没有任何 `dup`，`BackendProgressPort` 明确禁止 backend dup/store fd（`progress_source.hpp:534-545`）。
   - 同一 fd 直接去掉 `EFD_NONBLOCK` 会破坏 PROG-03 的非阻塞 acknowledge/drain、PROG-02 的“stale readiness 先于最终 actionable-work check”顺序、以及有界等待的 timeout（§3.4）。
   - 独立 blocking fd 又无法同时满足“io_uring 只注册一个 eventfd”和“PROG-03 要求 host 可 poll 的 notification handle”：kernel CQE 只能唤醒其中一个 fd；要桥接就需要 relay/第二通知权威，属于新机制、新模型、新拓扑，不是最小 B（§3.3、§3.4）。
2. **语义保持的最小 B（W1-A 微删除）是可行的，但不material**：合并 `wait_one` 入口的两次 `access_mtx_`、把 health 检查并入 park 前的 fused 临界区，可删除每 park 约 1–2 次 mutex 获取；不删任何 syscall、不删 kernel work，预计低于 C6/C1 的噪声带（§3.1、§7）。因此不能作为 A/B 候选，只能记 `NO_CHANGE_JUSTIFIED` / hygiene。
3. **有界等待方向（W1-B）不删 poll**：`poll` 仍在；只删少量 clock/整数运算。且 benchmark 的 P-B 本身是 **20 s 有界等待**（`bench/bench_w02_pipeline.cpp:52,271,384`），不是 `poll(-1)`；阻塞 read 无法服务有界等待（§3.2）。
4. **真实消费者门未过**：`#604` 仍是 `HOLD_NO_REAL_APPLICATION`；`sluice-copy` 是参考应用，不是 G0；P2-REVIEW M10 已明确指出 P2 profile 连 ledger 的 profile 门也未过（§5）。
5. **P-B/P-E 的匹配 A/A 是 C6/C1，不是 C5b/C3b/C7**；36 对只在“5% 是过程级效应”时足够。若历史 5% 是 measured-loop 的 5%、过程级稀释为约 1.2%，C6 cycles 需要 **48 对（80% 功效）/ 91 对（半宽≤0.6%）**，36 对不够；C1 cycles 需要 32/62（§7）。

**在没有 owner 裁决前，不应为 W1 开 A/B。** owner 需要裁决两件事：是否接受 bench/reference 代理作为 W1 的 profile 门（或等待 #604 G0），以及是否授权一次通知拓扑/协议变更（blocking read 或 condvar 双通道）并承担新模型/新 oracle 的成本。

---

## 1. 权威、输入与当前事实

### 1.1 规范权威（root）

- **PROG-01**：一个固定 progress owner；pass 有界、报告 residue 诚实；wait 不替代 pass。`docs/explicit-io-contract.md:828-863`。
- **PROG-02**：token 是 notification epoch；通知可合并/可虚假；suppression 只能在 pending unread wake 且 readiness 仍 asserted 时；wait 阻塞前必须依次建立：armed、stale 已确认、最终 check 后无 actionable work、token 未变、之后到达的 work 会使 armed wait 返回。`docs/explicit-io-contract.md:865-893`。还明确允许“condition variable with a protected predicate”或“persistent native readiness source with a final recheck”，但“token alone is not that proof”。`docs/explicit-io-contract.md:907-912`。
- **PROG-03**：每个可用 backend 提供 pollable notification handle；fd 由 context 借出；host 不得 close/independent drain/repurpose；drain/rearm 由 source-level acknowledge 拥有；**fd 绝不是第二 drain authority**；edge-triggered 集成必须 drain/recheck 到 quiescent；失败通知必须成为可观察 health event；fd 复用不得把旧事件路由到新 context。`docs/explicit-io-contract.md:914-935`。
- **PROG-04**：wait 区分 progress / control / deadline / health；单调时钟；deadline 不 cancel I/O；control sticky；`request_stop`/`close_admission` 必须唤醒可能睡眠的 owner。`docs/explicit-io-contract.md:937-949`。

### 1.2 当前实现的最小事实模型

`ProgressSource` 只有一个通知域：`mtx_`（`progress_source.hpp:491`）保护 epoch/control/health/`notification_pending_`/drain/wake；eventfd 是借用句柄，不是第二权威（ledger §2.5 第 5 条，`CAMPAIGN-LEDGER.md:155-161`）。

**创建与生命周期**

- `eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)`，非 `EFD_SEMAPHORE`：`progress_source.hpp:74-79`。
- 析构关闭 fd：`:81-86`；`retire_notification()` 只在 settled owner 调用，关闭并置 `-1`：`:284-290`。
- `BackendProgressPort::register_notification_with_kernel` 只把 fd 借用给 registration 回调；backend 不得 read/write/close/dup/store/retain：`:524-545`。
- io_uring 在 attach 时 `io_uring_register_eventfd` 注册这个 fd：`src/async/uring_backend.cpp:259-283`；unregister 在退役路径：`:384-390`。
- `has_immediate_physical_work()` 读 CQ ready/overflow：`src/async/uring_backend.cpp:285-293`；ThreadPool 没有 override，默认 false：`include/sluice/async/detail/async_backend.hpp:68-73`。

**`wait_if_unchanged`（`progress_source.hpp:104-232`）逐步**

| 步 | 代码 | 语义作用 |
|---|---|---|
| 1 | `:111-119` | 计算 `park_deadline`；bounded 时取 `max_park` 与 `time_point::max()` 的 min |
| 2 | `:121-127` | 单独持 `mtx_` 检查 `health_failed_`；若失败返回 `failed` |
| 3 | `:128-147` | 每轮重算 remaining；向上取整为 ms；`timeout_ms` 夹到 `INT_MAX`；已过期则 `expired=true, timeout=0` |
| 4 | `:149-151` | test-only `prerevalidate_pause_gate_` |
| 5 | `:153-166` | 同一 `mtx_` 内：control pending → `interrupted`；epoch 与 observed 不同 → `progress`；deadline expired → `deadline`；否则 `drain_notification_nolock_()` |
| 6 | `:168-173` | test-only prepark counter |
| 7 | `:175-181` | 锁外调用 `probe`；physical work → `progress` |
| 8 | `:183-192` | 第二次、**不 drain** 的 `mtx_` 重验：control → interrupted；epoch 变化 → progress |
| 9 | `:194-196` | test-only `prepark_pause_gate_` |
| 10 | `:198-203` | `poll(pfds,1,timeout_ms)`，`POLLIN`，timeout `-1` 或 ceil(ms) |
| 11 | `:204-230` | 同 `mtx_`：`rc<0` 且非 EINTR → sticky `health_failed_` → `failed`；`POLLNVAL` → terminate；`POLLIN` → control pending ? interrupted : progress |
| 12 | `:231` | 否则循环（EINTR、spurious、timeout=0 等） |

**`wait_one`（`async_io_context.cpp:573-705`）逐步**

| 步 | 代码 | 语义作用 |
|---|---|---|
| 1 | `:575-580` | backend 空 / 不支持 physical progress → error |
| 2 | `:581-593` | **两次独立 `access_mtx_`**：先查 `execution_closed_`，再 `++stats_->wait_calls` |
| 3 | `:595-598` | `drive_entry_admitted_()`（又一次 `access_mtx_`）+ `DriveGuard` |
| 4 | `:600-609` | 计算 caller deadline；`snapshot()` 取 token（`mtx_`） |
| 5 | `:611-614` | `run_progress_pass_()`；completed>0 → progress |
| 6 | `:619-629` | health → health_failure；`observe_pending_control()`（`mtx_`）→ control_interrupted |
| 7 | `:631-634` | 无 immediate/accepted/dispatch retry → quiescent progress(0) |
| 8 | `:636-652` | 构造 probe lambda；若 `dispatch_retry_remains` 则以 10 ms `kTransportRetryInterval`（`:22`）作为 pass_deadline |
| 9 | `:654-664` | unbounded 或 bounded 调 `wait_if_unchanged(token, remaining, probe, this)` |
| 10 | `:666-694` | progress/interrupted → continue；deadline：nap 到期 → continue，caller deadline → 再跑 final pass 后 `deadline_expired`；failed → health_failure |

**当前每 park 的工作与 syscall**

- 典型 park：`mtx_` 获取约 5–6 次（health、fused revalidation+drain、P3、poll 后检查；caller 侧还有 snapshot/observe/before park）+ `poll(2)` 1 次 + eventfd read 1 次（drain；可能是成功读，也可能是 EAGAIN）。
- 成功 signal 的物理写由 `notification_pending_` 抑制：`wake_notification_nolock_` 在 pending 时跳过；EAGAIN 视为已 pending：`:466-489`。
- `drain_notification_nolock_` 在成功读后同一 `mtx_` 临界区清 pending，返回；EINTR 重试，EAGAIN 清 pending 返回：`:414-460`。

**什么时候 park 真会发生**

- `docs/research/key-path-593/critical-path-atlas.md:45` 记录：W-02 540,000 ops 只有 24 次 eventfd read、**0 次 `poll(2)`**；warm tmpfs 下每次 pass 都能找到完成。该证据说明 W-02 d16 的 wait 机制是 dormant 的。
- P2 profile 中 hot 的是 **P-B `W02-uring-4k-d1`**（poll 1/op、eventfd read 1/op、wait_one 1/op）与 **P-E `sluice-copy 4k/d4/w1`**（poll 1.317/chunk、eventfd read/write ≈1.047/chunk）：`p2/P2-COST-MAP.md:356-365`。
- **读数修正（必须使用）**：`P2-COST-MAP` 的 15.8%/11.5% 是 `__x64_sys_poll` inclusive 的**下界**（libc-only `__poll` 样本被归入 `syscall unattributed`）；P2-REVIEW H1 逐样本复算的 `__poll` inclusive 是 **24.93% / 15.83%**（`p2/P2-REVIEW.md:30-50`）。`progress/wait` stage 25.53%/25.12% 是混合桶（含 iou-wrk eventfd signal、调度等），不是可删 cycles（`p2/P2-REVIEW.md:128-143`）。本文件所有 W1 讨论都用这两个修正口径。
- **P-B 的等待是有界的**：bench 用 `kWaitDeadline = 20s`（`bench/bench_w02_pipeline.cpp:52`）并在 `:271,:384` 调 `scope.wait_for(ticket, kWaitDeadline)`；`RequestScope::wait_for_slot_` 在 bounded 时传剩余时间给 `ctx_.wait_one(...)`（`include/sluice/async/request_scope.hpp:146-185`）。
- **P-E 的等待是无界的**：`apps/sluice-copy/copy_task.cpp:93` 传 `std::chrono::nanoseconds::max()`。
- P-A d16 没有 poll；P-C W-03 是 app-owned epoll，不是 `wait_one` 的 `poll(-1)`（P2 §5.1 `:362-364`）。
- 因此“`poll(-1)` 等待路径”的当前 hot 证据实际只落在 **P-E/C1**；P-B/C6 是 20 s 有界 poll，不能直接套用 blocking read（§3.2、§7）。

### 1.3 模型与现有 oracle 的约束

`formal/tla/ProgressSource.tla` 的 correspondence header 把当前 C++ 映射为：`OL4a`（health/control/token/deadline/stale drain 同一 mutex hold）、`OProbe`（probe 读 cq）、`OP3`（非 drain 的 post-probe revalidation）、`OWake`/`OTimer*`、`OPollFail`；kernel CQE 是 `KernelComplete`，只置 fd，不 bump epoch（`:12-108`、`:297-340`、`:477-488`）。安全不变量 `InvSafePark` 要求 `pc="park" && ~fd` 时 `pub=owed=cq=0`、`accT=0` 或 nap、无 control、无 sick、普通 park 时 `acc>0`（`:713-719`）。`MutParkDrain`（OL4a/P3 都不重验）和 `MutNoProbe` 是必须被杀的 mutant（`:103-106`、`:132-133`）。liveness `LParkWakes` 要求 parked+fd 最终离开 park（`:851-852`）。

现有确定性 oracle：

- `tests/progress_source_drain_semantic_oracle_test.cpp` 9 用例，覆盖 signal/ack/rearm、coalesce、prerevalidate 与 prepark 交错、stale ack 后 signal、retire、publication exactly-once、borrow/re-drain、cancel/shutdown settle（`:81-170`、`:252-496`、`:506-538`）。
- `tests/progress_notification_suppression_test.cpp`：host interest live 时 submit → fd readable → acknowledge → 再 submit → 再 rearm；并发 producer 不 strand（`:92-174`）。
- `tests/request_scope_test.cpp:1273-1304`：wait health failed 时 cleanup fail-fast，不允许返回带着 live borrow。
- `tests/shutdown_lifecycle_test.cpp:588-639`：shutdown 后 fd 真关闭；同一个 fd number 被新 eventfd 复用时不得收到 stale wake。
- `tests/progress_source_ownership_test.cpp`、`tests/threadpool_progress_race_test.cpp`、`tests/uring_progress_race_test.cpp` 等覆盖 owner/race/park；`xmake/tests.lua` 注册 9 用例 + mutant（ledger §8.1 `:448`）。

**这些 oracle 是任何 W1 B 的硬门**：语义 FAIL 即 `NO_GO`（ledger §9.1.4 `:493`）。

---

## 2. 语义义务清单（每个方向都必须逐项回答）

下表的编号在 §3 的各方向判断中复用。所有义务来自 root 或模型；不是本评估新增。

| ID | 义务 | 当前机制 | 证据 |
|---|---|---|---|
| O1 | **no-lost-wake**：任何创建 actionable work/control 的 transition 必须留下持久 readiness，或与 sleep handshake 安全竞争 | epoch bump + `notification_pending_` 抑制写 + eventfd 持久 readiness；pre-park 重验 + drain 同一 `mtx_`；post-probe P3 | PROG-02 `:865-893`; `progress_source.hpp:153-192`; model `InvSafePark` `:713-719` |
| O2 | **stale readiness 必须在最终 actionable-work check 之前被确认** | fused block 先 control/epoch 检查，再 drain；drain 在锁内 | PROG-02 第 2 条 `:881-883`; `progress_source.hpp:153-166`; model `OL4a` `:297-320` |
| O3 | **signal before / during / after park** 都不能丢 | before：epoch 变化被 pre-park check 捕获，或 drain 后 writer 再写 fd；during probe：P3 捕获 epoch 或 fd 保持 ready；during poll：fd 唤醒；after poll：post-poll 检查 | `progress_source.hpp:153-230`; semantic oracle `:117-170` |
| O4 | **kernel producer 窗口**：io_uring CQE 不 bump userspace epoch，只写 fd；drain 可能消费它 | `PhysicalProbe` 在 drain 之后、P3 之前读 CQ；probe 报 work 则返回 progress，不 park | PROG-02 `:907-912`; model `KernelComplete` `:477-488`; `uring_backend.cpp:285-293` |
| O5 | **control / cancel / shutdown**：interrupt sticky，直到 observe+ack；request_stop 唤醒 owner；shutdown 不丢 accepted work | control epoch 三代 + fd write；wait 返回 interrupted 前先检查 control；shutdown 的 wait 循环用同一 probe | PROG-04 `:937-949`; `progress_source.hpp:234-250,380-386`; `async_io_context.cpp:511-550,627-629,669-673` |
| O6 | **fd close / reuse**：borrow 生命周期；retire 只能在无 waiter 时；旧事件不得路由到新 context | `retire_notification` 关闭 fd；`POLLNVAL` terminate；host interest fail-fast；V23/shutdown test 复用 fd | PROG-03 `:932-935`; `progress_source.hpp:216-222,284-290`; `async_io_context.cpp:204-207,720-747`; `shutdown_lifecycle_test.cpp:588-639` |
| O7 | **bounded deadline**：单调时钟；deadline 不 cancel I/O；nap 到期只再跑 pass，caller deadline 才返回 deadline_expired；不早退 | `park_deadline`；ceil ms；`OTimerNap` vs `OTimerCaller`；final pass | PROG-04 `:937-949`; `progress_source.hpp:111-147`; `async_io_context.cpp:600-607,644-691`; model `:351-386` |
| O8 | **host borrow/ack**：fd 只注册；drain/rearm 由 source-level ack；fd 不是第二 drain authority | `progress_notification_fd` 标记 interest；`acknowledge_progress_notification` 调 source drain；`detach` 退役 interest | PROG-03 `:914-930`; `async_io_context.cpp:720-747`; `progress_source.hpp:274-277,524-545` |
| O9 | **suppression/coalescing**：只有 pending unread wake 且 readiness 仍 asserted 时才能抑制物理写；EAGAIN 视为已 pending | `notification_pending_`；`wake_notification_nolock_` 跳过 pending；EAGAIN 设置 pending | PROG-02 `:869-877`; `progress_source.hpp:466-489` |
| O10 | **单一 drain authority**：成功 read 是 drain 线性化点；kernel 只拥有计数器 | `drain_notification_nolock_` 在 `mtx_` 内成功读后清 pending；`acknowledge_notification` 唯一 public drain | PROG-03 `:920-922`; `progress_source.hpp:414-460,274-277`; ledger §2.5 `:161` |

---

## 3. 候选方向逐一判断

方向来自任务 ask 的 (a)–(d)。每个方向给出：语义义务、可删除的机器工作类型、与 P2 归因的对应、判词。

### 3.1 W1-A：保留 poll，删除其外围“冗余”工作

这是任务 (a)。必须逐项区分“真冗余”和“看起来冗余但承载 no-lost-wake”。

#### A1. 合并 `wait_one` 入口的两次 `access_mtx_` 获取

- 位置：`async_io_context.cpp:581-593`。当前先 lock 查 `execution_closed_`，释放；再 lock `++stats_->wait_calls`；然后 `drive_entry_admitted_()` 又 lock（`:595-598`）。
- 语义：两个临界区都只读/写 `access_mtx_` 域；中间没有可观察的协议步。`stats_` 增量与 `execution_closed_` 检查可以合并在同一 lock 内；`drive_entry_admitted_` 保持独立（它设置 `owner_thread_`/`drive_active_`，失败时现有语义是 stats 已经加过；若合并后仍保持“先 stats 再 admission”，行为不变）。
- 可删除工作：每次 `wait_one` 调用 1 次 mutex lock/unlock（以及可能的 cache line 往返）。不是 syscall、不是 kernel work。
- 语义义务：O1–O10 全部不变；不触碰 park 握手。
- P2 对应：`progress/wait` 阶段的 user 部分（P-B user 2%、P-E user 36%；`P2-COST-MAP.md:244`）与 `shared sync`；该锁不是单独可观测符号（P2 §8.1.5/6）。预计远低于 C6/C1 噪声带。
- 判词：**DESIGN_FEASIBLE（语义保持），但不material**。

#### A2. 把 health 检查并入 park 前的 fused 临界区

- 位置：`progress_source.hpp:121-127` 的独立 lock，然后 `:153-166` 的 fused block。
- 语义：`health_failed_` 的生产 setter 只有 `wait_if_unchanged` 自己在 poll 非 EINTR 失败时（`:206-213`），以及 `SLUICE_ASYNC_INTERNAL_TESTING` 的 test seam（`:331-334`）。生产中“同一 owner 设置后立即返回 failed”，所以 top-of-loop 独立检查只在“再次调用 wait”时有用；把它放进 fused block（在 control/epoch/deadline 之前）可以保持 health 优先级，并顺带覆盖 test seam 在 wait 进入后设置 health 的窗口（当前代码在该窗口仍可能 park；这是 test-only 的观察差异，不是生产语义）。
- 必须在 fused block 里保留顺序：**health → control → epoch → deadline → drain**，与 `wait_one` 的 completed > health > control 优先级一致（`async_io_context.cpp:611-629`；PROG-04 `:937-943`）。
- 可删除工作：每次 park 尝试 1 次 mutex lock/unlock。不是 syscall。
- 语义义务：O1/O2/O5/O7 不变；health 仍 sticky；不得把 health 检查移到 drain 之后（否则 health 失败时可能先 drain，虽然 drain 本身不产生工作，但会改变“sticky health 不得 park”的证明形状）。
- P2 对应：`progress/wait` user 部分；无独立符号。预计低于噪声。
- 判词：**DESIGN_FEASIBLE（语义保持），但不material**。

#### A3. deadline/clock 算术复用

- 位置：`progress_source.hpp:111-147`；`async_io_context.cpp:600-607,646-663`。当前 caller 算一次 remaining，`wait_if_unchanged` 又算一次 `park_deadline` 和每轮 remaining；多次 `steady_clock::now()`。
- 可删除工作：少量 clock read、整数除法/取整/夹取。不是 syscall。
- 语义义务：O7 不变。必须保留“ceil 到 ms 以避免早退”；必须保留 timeout 夹到 `INT_MAX`；必须保留 caller deadline 与 transport nap 的区分；EINTR 后必须用剩余时间而不是重置。
- 判词：**DESIGN_FEASIBLE（语义保持），但不material**；不建议单独作为候选。

#### A4. 降低 PhysicalProbe 频率 / 对 ThreadPool 跳过 probe

- 位置：`async_io_context.cpp:637-639` 构造 probe；`progress_source.hpp:175-181` 锁外调用；ThreadPool 的 `has_immediate_physical_work` 是默认 false（`async_backend.hpp:70-73`），但 probe 仍是虚调用 + 函数调用。
- 语义：probe 是 **kernel producer** 的唯一兜底：io_uring CQE 不 bump epoch（model `KernelComplete` `:477-488`）。一旦删掉/降低 probe，`MutNoProbe` 类的 lost-wake 立即成立（model `:103-106`、`InvSafePark` `:713-719`；`uring_backend.cpp:285-293`）。因此 **不能全局降低频率**。
- 对 ThreadPool 跳过 probe 在语义上可行（没有 kernel 写者），但需要一个新的“backend 是否有 kernel producer”能力/派生事实；当前 `signals_physical_progress()` 对 TP 也是 true（`threadpool_backend.hpp:34`），不能直接复用。新增能力要过机制门七问，收益是一个虚调用（几 cycles/park），远低于噪声。
- 判词：**全局降低 = SEMANTIC_MISMATCH；TP-only 跳过 = 可设计但不值得（NO_CHANGE_JUSTIFIED）**。

#### A5. 删除第一次 epoch/control 重验（fused block 的 `:153-161`）

- 语义：不能删。若 signal 在 token 快照之后、fused block 之前发生，它 bump epoch 并写 fd；若没有重验，随后的 drain 会消费这个 fd，owner 然后 park 并丢失唤醒。这正是 PROG-02 第 4 条和 model `OL4a` 的 load-bearing 检查。`MutParkDrain` 就是“drain 但不重验”的 mutant（model `:103-106`、`:297-320`）。
- 判词：**SEMANTIC_MISMATCH**。

#### A6. 删除 probe 后的第二次重验（P3，`:183-192`）

- 语义：不能无条件删。P3 是**不 drain** 的重验；它覆盖“probe 释放 `mtx_` 后，epoch 变化/control 到达，且另一条 drain 路径（host `acknowledge_progress_notification` 或同一 owner 的另一 wait）消费了 fd write”的窗口。当前 C++ 没有禁止 host 在 owner park 时调用 ack；`progress_notification_fd` 的文档只限制 borrow 的注册时机（`async_io_context.hpp:197-207`），`wait_one` 也不检查 `notification_interest_live_`（`async_io_context.cpp:573-598`）。`progress_notification_suppression_test.cpp:75-133` 就在 host interest live 时调 `wait_one`。因此“无人会在 probe 与 poll 之间 drain”不是一个可用的不变量；P3 是这个交错的安全网。model `OP3` 也把它作为独立步骤（`:332-340`）。
- 如果 owner 先把 host ack 与 owner park 的并发定义为非法并加 fail-fast，P3 才可能重新评估；那是协议变更，不是删除冗余。
- 判词：**SEMANTIC_MISMATCH（当前契约/API 下）**。

#### A7. 删除 stale drain（`:165`）

- 语义：不能删。PROG-02 第 2 条明确要求 stale notification state 在最终 actionable-work check 之前被确认；drain 与 epoch/control 检查同一 `mtx_` hold 是“signal 在 check 后写入时仍留下 readiness”的证明。删掉 drain 会让已观测的旧唤醒在 poll 上立即返回，造成 spurious wake；更糟的是没有 drain 时，下一次 park 仍会立即返回，形成 owner 级 spin（直到某个 read 消费）。model `OL4a` 把 drain 作为 action 的一部分（`:313-317`）。
- 判词：**SEMANTIC_MISMATCH**。

#### A8. 删除 poll 后的 control/epoch 检查（`:204-229`）

- 语义：不能删。它区分 interrupted vs progress，并在 poll 返回后重新读取 control；PROG-04 sticky control 依赖它。`POLLNVAL` 的 terminate 是 fd 生命周期违规的 fail-fast；删除会削弱 V23 的现场诊断。
- 判词：**SEMANTIC_MISMATCH**。

#### A 方向总结

| 子项 | 语义 | 删除的工作 | material? | 判词 |
|---|---|---|---|---|
| A1 合并入口两次 `access_mtx_` | 保持 | 1 lock/unlock per wait | 否 | DESIGN_FEASIBLE（hygiene） |
| A2 health 并入 fused block | 保持（需保序） | 1 lock/unlock per park | 否 | DESIGN_FEASIBLE（hygiene） |
| A3 deadline 算术 | 保持（需 ceil/不早退） | 少量 clock/int | 否 | DESIGN_FEASIBLE（hygiene） |
| A4 全局降低 probe | 破坏 kernel-writer 兜底 | probe 调用 | — | SEMANTIC_MISMATCH |
| A4' TP-only 跳过 probe | 可设计（需新能力） | 虚调用 | 否 | NO_CHANGE_JUSTIFIED |
| A5 删第一次重验 | 丢唤醒 | 1 lock + check | — | SEMANTIC_MISMATCH |
| A6 删 P3 | 并发 drain 窗口丢唤醒 | 1 lock + check | — | SEMANTIC_MISMATCH |
| A7 删 stale drain | PROG-02 第 2 条破坏 | 1 read | — | SEMANTIC_MISMATCH |
| A8 删 poll 后检查 | control/POLLNVAL 破坏 | 1 lock + check | — | SEMANTIC_MISMATCH |

**W1-A 的可进入最小设计部分：A1 + A2（可选 A3）。** 它不删除任何 syscall/kernel work，不改变 wait 语义，也不足以支撑 5% 或 1.2% 的 A/B 声明；应作为 hygiene/NO_CHANGE_JUSTIFIED，而不是 W1 性能候选。

---

### 3.2 W1-B：有界等待 / 超时路径优化

- 当前位置：`progress_source.hpp:128-147`（remaining→ceil ms、`INT_MAX` 夹取）与 `async_io_context.cpp:644-691`（transport nap 的 10 ms；caller deadline 的 final pass）。
- 可做的语义保持改动：把绝对 deadline 传入 `wait_if_unchanged`，避免内外两次 deadline 计算；用 `ppoll`/`timespec` 代替 ms 取整（如果 owner 接受 API 变更）；复用 `steady_clock::now()`。
- **不能做的**：把有界等待改成阻塞 read。eventfd 的阻塞 `read` 没有 timeout；要实现 20 s / 任意 deadline，仍需 `poll`/`ppoll`（或 timerfd/epoll，增加 syscall/fd）。因此 B 方向没有 syscall 删除，只有用户态算术。
- 语义义务：
  - O7：deadline 是单调时钟；不得早退（ceil）；EINTR 后剩余时间；transport nap 到期只再跑 pass，caller deadline 才返回 `deadline_expired`；final pass 先跑；deadline 不 cancel I/O。
  - O1/O2/O4：有界路径仍需要 pre-park 重验、drain、probe、P3；不能因为 timeout 0 就跳过 drain 或 probe。
  - O6：poll 的 `POLLNVAL` terminate 保留。
- P2 对应：`progress/wait` 的 user 部分；P-B 的 poll 栈 `__poll` inclusive 24.93% 仍是 kernel wait，不是 B 能删的；`__x64_sys_poll` 15.77% 只是下界（P2-REVIEW H1 `:30-50`）。B 不能改变这些。
- 判词：**DESIGN_FEASIBLE（语义保持），NO_CHANGE_JUSTIFIED（不material）**。它不是 P-B 的 material B；P-B 本身就是 20 s 有界等待，`poll(-1)` 的标签只适用于 P-E。

---

### 3.3 W1-C：独立等待 fd / 通知拓扑

任务 (c)。必须区分两个子方向。

#### C1. 第二个等待 fd（owner 专用 eventfd/pipe），host 仍用原 fd

- 语义问题：io_uring 当前只注册**一个** eventfd（`uring_backend.cpp:273-278`；`BackendProgressPort` 只借出 source 的 fd，`progress_source.hpp:539-545`）。kernel CQE 只会唤醒被注册的那个 fd：
  - 若注册 owner 的 blocking 等待 fd，则 host 的 pollable handle 收不到 kernel completion → W-03/PROG-03 失效；
  - 若注册 host 的 fd，则 owner 的等待 fd 收不到 kernel completion → 需要 probe/超时兜底，回到 poll；
  - 若两个 fd 都注册，当前代码/接口没有这个能力；即便 kernel 支持第二个注册，也产生两个通知权威/两条 drain 路径，违反 PROG-02 “never a second drain authority”与 PROG-03 “drain/rearm owned by the documented source-level acknowledge”。
- 语义义务：O1/O4/O6/O8/O9/O10 全部受冲击；fd 生命周期翻倍；host detach 与 owner park 的交错需要新协议；V23 fd 复用需要覆盖两个 fd。
- 判词：**SEMANTIC_MISMATCH**（当前 root/拓扑下）。不是“还没测”。

#### C2. owned-driver ThreadPool 用 condition variable + protected predicate，eventfd 仅保留为 host pollable handle

- 契约上**原则可行**：PROG-02 明确允许 “condition variable with a protected predicate”（`:907-912`）。predicate 可以是 `mtx_` 下的 epoch/control/health；wait 在 `mtx_` 下检查 predicate，writer 在 bump 后 notify；no-lost-wake 由 mutex+predicate 保证，不需要 stale fd drain。ThreadPool 没有 kernel producer，所以不需要 `PhysicalProbe`（`async_backend.hpp:68-73`）。
- 但这是一个**双通知通道**拓扑：
  - host 仍需要 eventfd（PROG-03、W-03 epoll）；`signal()`/`interrupt()` 必须同时 notify condvar 和写 eventfd；`notification_pending_` 的 suppression 只适用于 eventfd，condvar 的 notify 不能被 suppression 跳过（否则 owner 丢唤醒）。
  - host borrow/detach 与 owner wait 的模式切换需要新状态/协议；`progress_notification_fd` 当前只允许在 owner 不在 drive callback 时 borrow（`async_io_context.cpp:720-736`），但 suppression test 显示 host interest live 时仍可 `wait_one`（`progress_notification_suppression_test.cpp:75-133`）。若 owner 在 condvar 上等待而 host 在 eventfd 上 epoll，signal 必须同时到达两者；若 host 在 owner park 时 ack eventfd，condvar predicate 不受影响（epoch 仍新），不会丢唤醒；反之亦然。这个双向证明必须进模型。
  - `ProgressSource` 目前是单一 wait authority；引入 per-backend 等待原语需要新的抽象/派生能力，属于机制门七问的第 1/2/5 类（新运行时状态与选择事实）。
  - bounded wait：condvar 可用 `wait_for`；uring 仍用 poll。
  - fd close/reuse：condvar 无 fd；eventfd 生命周期不变。
  - cancel/shutdown：predicate 必须包含 control/health/execution closed；shutdown 不能用 condvar 永久阻塞（需要 notify）。
- 可删除工作（如果实现）：TP owned-driver 路径的 eventfd `poll`+`read`（P-E 的 poll 1.317/chunk、eventfd read/write ≈1.047/chunk；P2 §5.1）以及可能的 write（取决于 host interest/抑制）；这是**可能material**的，但它不是 W1 的最小 B，而是通知拓扑重构。
- 判词：**NEEDS_OWNER**（契约原则允许，但需要新模型、新 oracle、双通道协议与 owner 对“第二通知通道”的批准；不属于当前 W1 最小设计）。

#### C3. epoll/timerfd/relay thread

- epoll_wait 仍是一次 syscall，且不消费 eventfd；要 drain 仍需 read → 不比 poll+read 少。timerfd 增加 fd 与 syscall。relay thread 是新的执行资源和第二通知权威，违反“一个固定 progress owner”（PROG-01 `:828-833`）与“不添加 hidden blocking fallback”。
- 判词：**NO_GO**（本战役范围）。

**C 方向总结**：C1 = SEMANTIC_MISMATCH；C2 = NEEDS_OWNER / 非最小 B；C3 = NO_GO。

---

### 3.4 W1-D：阻塞 eventfd read

任务 (d) 明确要求注意“不能把现有 EFD_NONBLOCK fd dup 后改 blocking——共享 open-file description 与文件状态标志”。这一条是硬阻断。

#### D1. 同一 fd + `dup` + `fcntl(F_SETFL)` 去掉 `O_NONBLOCK`

- 事实：`dup` 共享 open-file description；`O_NONBLOCK` 是文件状态标志（file status flag），不是 descriptor 标志；对 dup 改 flag 会同时改原 fd。本会话用 eventfd 本身执行：

  ```text
  $ python3 -I - <<'PY'
  import os, fcntl
  fd = os.eventfd(0, os.EFD_NONBLOCK)
  dup = os.dup(fd)
  def flags(f): return fcntl.fcntl(f, fcntl.F_GETFL)
  print('eventfd original O_NONBLOCK:', bool(flags(fd) & os.O_NONBLOCK))
  print('eventfd dup      O_NONBLOCK:', bool(flags(dup) & os.O_NONBLOCK))
  fcntl.fcntl(dup, fcntl.F_SETFL, flags(dup) & ~os.O_NONBLOCK)
  print('after clear on dup: original', bool(flags(fd) & os.O_NONBLOCK),
        'dup', bool(flags(dup) & os.O_NONBLOCK))
  os.close(dup); os.close(fd)
  PY
  eventfd original O_NONBLOCK: True
  eventfd dup      O_NONBLOCK: True
  after clear on dup: original False dup False
  ```

  输出证明在 dup 上清 `O_NONBLOCK` 同时把原 eventfd 变成 blocking；反之设置亦然。
- 源码事实：`ProgressSource` 只在 `:75` 创建一次 eventfd，没有任何 `dup`/`fcntl`（grep 仅命中 `progress_source.hpp:75`）；`BackendProgressPort` 明确“must not read, write, close, dup, store” fd（`:534-545`）。
- 因此 D1 不是“实现细节”，而是**不可能同时保持原 fd 的 EFD_NONBLOCK 语义**。原 fd 的 EFD_NONBLOCK 是 PROG-03 ack/drain、`drain_notification_nolock_`、`acknowledge_notification`、host borrow 以及当前 poll 路径的基础。
- 判词：**SEMANTIC_MISMATCH**。

#### D2. 同一 fd 直接改为 blocking（不 dup）

- `drain_notification_nolock_`（`:414-460`）在无 readiness 时 read 会**永久阻塞**；它被 `acknowledge_notification()`（`:274-277`）和 `wait_if_unchanged` 的 fused block（`:165`）调用。PROG-03 要求 ack 是 plain drain，不能阻塞 host；`notification_pending_` 只覆盖 userspace 写者，不覆盖 io_uring kernel 写者。
- `wait_if_unchanged` 的有界路径（`:128-147`、`:198-203`）依赖 timeout；blocking read 没有 timeout。P-B 的 20 s 有界等待（`bench_w02_pipeline.cpp:52`）和 `RequestScope::wait_for` 的任意 deadline（`request_scope.hpp:146-185`）都会失效或需要保留 poll，从而无法删除 poll。
- PROG-02 第 2 条要求 stale readiness 在最终 check 前确认；blocking read 是“park 时消费 readiness”，在最终 check 之后。若删掉 pre-park drain 让 read 兼任 ack，wait 的 letter 违反第 2 条；model `OL4a` 的 drain action 需要改写并重新证明。若保留 pre-park drain，则 blocking fd 下 drain 必须 `poll(fd,0)+read`（2 syscalls），比当前 1 次 read 更差；若 pre-park drain 用 `notification_pending_` 条件跳过，kernel 写者的 stale readiness 会让 poll 立即返回并 spin（io_uring CQ 已 reap、eventfd counter 仍非零）。
- 错误/生命周期：blocking read 在 fd 关闭时返回 `EBADF`，不是 `POLLNVAL`；当前 `POLLNVAL` 路径 terminate（`:216-222`）是 fd 生命周期违规的 fail-fast。若改成 health_failed，V23/`shutdown_lifecycle_test.cpp:588-639` 的“fd 复用不得收到旧事件”诊断面需要重写；若 fd number 被复用，read 可能消费新对象的 eventfd（PROG-03 `:932-935`）。
- signal before/during/after park：blocking read 在 before 时立即返回（好），during 时唤醒（好），after 时 fd 已 ready（好）；但读后必须重新检查 control/health/epoch，并且对 kernel CQE 需要在读后 probe（当前 probe 在 park 前，无法覆盖 read 唤醒后的 CQE 事实）。这可以设计，但叠加在上述阻断之上没有净收益。
- bounded 路径：blocking read 不能 timeout；若仅 unbounded 用 read、bounded 用 poll，同一 blocking fd 的 bounded pre-park drain 仍会被 read 阻塞；需要两个 fd 或 `poll(0)` 桥接（见 D3/C1）。
- 判词：**SEMANTIC_MISMATCH**（PROG-02 第 2 条、PROG-03 ack、有界等待、fd 生命周期诊断）。

#### D3. 独立 blocking fd（新建 eventfd，不 dup）

- 这是绕过“dup 共享 flag”的唯一形态，但撞上 kernel registration 的单 fd 事实：
  - io_uring 只能把 kernel CQE readiness 写给被注册的 fd（`uring_backend.cpp:273-278`）。host 的 pollable handle（PROG-03）与 owner 的 blocking wait fd 必须是同一个 fd 才能都收到 kernel CQE；若是两个，必有一个收不到。
  - 若用 relay/epoll 线程把 kernel fd 的 readiness 转发到 blocking fd，则新增线程、第二通知权威、额外 syscall；违反 PROG-01 单 owner、PROG-02 单 drain authority、AGENTS.md 最小机制与“不添加 hidden blocking fallback”。
  - 若在 host interest 存在时切回原 fd、无 host 时用 blocking fd，需要 attach 时重新注册 eventfd（`io_uring_register_eventfd` 单次注册）并在模式切换时处理 in-flight CQE；这是一个新的拓扑协议，不是最小 B。
- 判词：**SEMANTIC_MISMATCH / NO_GO**（当前 root/拓扑下；只有当 owner 批准“双事件源 + relay/重注册”的新架构并重做模型时才另议，且应归 #617 G1/kernel-boundary，不是 #621 W1 的最小 B）。

#### D 方向总结

| 变体 | 阻断 | 判词 |
|---|---|---|
| D1 same fd + dup/fcntl | dup 共享文件状态标志（本会话已复现）；只创建一次 fd、无 dup；backend port 禁止 dup | **SEMANTIC_MISMATCH** |
| D2 same fd 改 blocking | 非阻塞 ack/drain 破坏；有界 timeout 丢失；PROG-02 第 2 条；read 后 kernel probe 窗口；EBADF/POLLNVAL 诊断 | **SEMANTIC_MISMATCH** |
| D3 独立 blocking fd | kernel 只注册一个 eventfd；host/owner 必有一方收不到 CQE；relay/第二权威 | **SEMANTIC_MISMATCH / NO_GO** |

**结论：W1-D 不是“待证明性能”的候选，而是当前契约下的语义不匹配。** 历史 R1 的 `poll(-1)` → blocking read 只是一句工作稿线索（`docs/evidence/621-round2-R1/REPORT.md:148,176`，未跟踪、已被 R2 r6 取代）；结构化 candidates 没有给出落选理由，本评估补上了该理由：不是收益不足，而是 fd 语义与通知拓扑不允许。

---

## 4. 方向汇总与排除

| 方向 | 语义可行性 | 可删除工作类型 | 与 P2 归因 | 判词 |
|---|---|---|---|---|
| **A1/A2/A3** 保留 poll，合并入口锁/health 锁/deadline 算术 | 可行（保序） | mutex lock/unlock、clock/int | `progress/wait` user + `shared sync`；无独立符号 | **DESIGN_FEASIBLE（hygiene）**；非 material |
| **A4** 全局降低 probe | 不可行（kernel CQE 丢唤醒） | probe 调用 | `progress/wait` | **SEMANTIC_MISMATCH** |
| **A4'** TP-only 跳过 probe | 可设计（需新能力） | 虚调用 | 不可观测 | **NO_CHANGE_JUSTIFIED** |
| **A5/A6/A7/A8** 删第一次重验/P3/drain/poll 后检查 | 不可行 | 1 lock + check / 1 read | `progress/wait` / `__poll` | **SEMANTIC_MISMATCH** |
| **B** 有界等待优化 | 可行（只算术） | clock/int；poll 仍在 | `progress/wait` user；poll 栈不变 | **NO_CHANGE_JUSTIFIED**；非 material |
| **C1** 第二等待 fd | 不可行（单 kernel 注册 fd/第二 drain 权威） | 新拓扑 | — | **SEMANTIC_MISMATCH** |
| **C2** TP condvar + predicate（eventfd 留给 host） | 契约原则可行，但双通道/新模型/新 oracle | eventfd poll+read（可能 write） | `progress/wait` / eventfd | **NEEDS_OWNER**；非最小 B |
| **C3** epoll/timerfd/relay thread | 不可行/不划算 | 不减少 syscall 或新增线程 | — | **NO_GO** |
| **D1/D2/D3** blocking eventfd read | 不可行（fd 标志/ack/timeout/kernel 注册） | poll、drain read | `__poll` inclusive 24.93%/15.83%（下界 15.8/11.5） | **SEMANTIC_MISMATCH** |

**进入最小设计（如果 owner 只批准语义保持的 hygiene）**：A1 + A2（+A3），保留以下全部：poll、EINTR 重试、POLLNVAL terminate、第一次 epoch/control 重验、stale drain、probe、P3、poll 后 control/epoch 检查、health sticky、bounded deadline/ceil、host borrow/ack、fd 生命周期。

**不得进入最小设计**：D 全部、C1/C3、A4–A8、任何“删除 drain/P3/probe”的变体。

---

## 5. 真实消费者门（#604 G0）

### 5.1 ledger 的门

`CAMPAIGN-LEDGER.md:257` 的 W1 准入门禁原文：**“真实消费者 profile + 有界等待/非阻塞 drain/dup fd 设计 + A/A 36 对 + A/B ≥36 对；若改等待语义需模型/协议重证”**。

### 5.2 当前状态

- `#604` 状态：`OPEN + HOLD_NO_REAL_APPLICATION`；`G0 真实 workload` **尚未准入**；`READY_FOR_595=false`（`CAMPAIGN-LEDGER.md:307-315`）。
- `sluice-copy` 定位：**参考应用**；其收益被 #604 评论定位为 backend coordination，不是 application-level 收益；不能把 sluice-copy A/B 当作 #604 G0 通过（`:311-315`）。
- `#637 §J/r6` 记录 #604 维持 `HOLD_NO_REAL_APPLICATION`（`:311`）；`v2-conformance` 也没有把 sluice-copy 记作 G0。
- P2 的两个 hot 形态：P-B 是 benchmark，P-E 是 sluice-copy 参考应用；P2-REVIEW M10 明确：“连 ledger 的 profile 门也未过，因为真实消费者缺席”；“PARTIAL PASS”只是在“是否有 measured hotspot”这一子门（`p2/P2-REVIEW.md:239-247`）。
- 因此当前 W1 的 profile 证据只能证明：**bench/reference 形态的等待路径有热点**，不能证明真实消费者路径有收益。

### 5.3 owner 裁决选项

| 选项 | 内容 | 代价/限制 | 建议 |
|---|---|---|---|
| **O-1 等待 #604 G0** | 不启动 W1 A/B；等 admitted real workload 后在该 workload 上重做 profile，再决定是否 A/B | 最安全；W1 暂停；可能长时间无进展 | **默认推荐** |
| **O-2 接受 bench/reference 代理并披露** | owner 书面接受 P-B/P-E 作为 W1 的 scoped proxy；判词降级为“mechanism-level / bench-proxy only”；不得用于 #604 G0、application-level 收益或 5% 产品声明 | 需要 owner 签字；结果不能替代 #604；A/A 必须用 C6/C1；预注册必须写“不是真实消费者” | 次选；若 owner 想先做机制研究 |
| **O-3 分段准入** | 现在只做设计/语义 proof/A-A（C6/C1 36 对）；A/B 与 GO 等 #604 G0 后再开 | 保留工作但不产生收益声明；A/A 结果可能过期 | 可作为 O-1 的折中 |
| **O-4 拒绝/NO_CHANGE_JUSTIFIED** | 认定 W1 当前无 material、语义可行的最小 B；记录负结论，避免重复研究 | 需要 owner 接受“等待路径热点存在但无可准入的减法” | 若 owner 不接受代理/拓扑变更，这是诚实结论 |

**本评估建议：O-1 或 O-3。** 在 owner 未裁决前，W1 保持 `NEEDS_OWNER`，不进入 A/B。

---

## 6. W1 准入建议

### 6.1 判词：`NEEDS_OWNER`

不是 `DESIGN_FEASIBLE`，因为：

- 唯一可能 material 的方向（D、C2）要么 `SEMANTIC_MISMATCH`（D/C1），要么需要 owner 批准的新拓扑/新模型（C2），不是当前 W1 的最小 B。
- 语义保持的最小 B（A1+A2）没有 material 效应，不能支撑 ledger 的 A/B 门。
- 真实消费者门未过（§5）。

不是 `NO_GO`，因为：

- `poll(-1)` 的等待热点是真实的（P-B/P-E；`__poll` inclusive 24.93%/15.83%；P2-REVIEW H1）；
- 如果 owner 接受代理并授权拓扑/协议变更，C2（或未来 #617 的 kernel-boundary 方案）仍有研究价值；
- A1/A2 可作为 hygiene 保留，不需要否决。

不是 `SEMANTIC_MISMATCH`（整个 W1），因为问题的范围是**候选方向**：A1/A2/B 语义保持；只有 D/C1 等具体方向是 SEMANTIC_MISMATCH。

### 6.2 若 owner 选择“最小语义保持 B”（A1+A2+A3）

**设计要点**

1. **A1**：在 `AsyncIoContext::wait_one` 中，把 `:581-593` 的两次 `access_mtx_` 合并为一次：
   - 同一 lock 内先检查 `execution_closed_`，再 `++stats_->wait_calls`；释放后调 `drive_entry_admitted_()`（保持现有“stats 先于 admission”的次序，若 admission 失败则 stats 已计，与当前一致）。
   - 不改 `drive_entry_admitted_` 的 owner_thread/drive_active 逻辑；不改 `DriveGuard`。
2. **A2**：在 `ProgressSource::wait_if_unchanged` 中，把 `:121-127` 的 health 检查移入 `:153-166` 的 fused block 顶部：
   - 顺序必须是 health → control → epoch → deadline → drain。
   - 删除独立的 health lock；不改变 `wait_health_failed()` 的 const 语义。
   - test seam `set_wait_health_failed_for_test` 仍能在下一次 wait 被观察；生产 setter 只有本函数。
3. **A3（可选）**：把 `wait_if_unchanged` 的 `park_deadline` 改为由 caller 传入绝对 deadline；内部只算一次 remaining。保留 ceil、`INT_MAX` 夹取、EINTR 用剩余时间、nap/caller 区分。
4. **保留不变**：`poll`、`EINTR` 重试、`POLLNVAL` terminate、第一次重验、stale drain、probe、P3、poll 后 control/epoch 检查、health sticky、bounded deadline、host borrow/ack、fd 生命周期。
5. **测试**：9 用例 drain semantic oracle、suppression test、request_scope health test、shutdown fd reuse test、ownership/race suites 全部保持；A1 可加一个“stats 计数在 admission 失败时仍增加”的既有行为断言（如果还没有），A2 可加一个“health 在 park 前被观察”的确定性 gate 用例。不得削弱任何现有断言。
6. **性能预期**：删除 2 次 mutex/等待调用，无 syscall、无 kernel work；预计远低于 C6/C1 的 MDE（C6 CPU 0.747%、C1 CPU 0.589% at 16 pairs；36 pairs 0.479/0.378%）。**不应为它开 A/B 声称收益**；如果 owner 坚持测，只能作为 NOT_POWERED/NO_CHANGE_JUSTIFIED 的负结论。

### 6.3 若 owner 选择“拓扑/协议变更”（C2 或 D 的重新设计）

必须先满足：

1. **协议/模型**：扩展 `formal/tla/ProgressSource.tla` 或新模型，覆盖第二个通知通道/模式切换（host borrow/detach、owner park、kernel CQE、bounded wait、shutdown）；`MutParkDrain`/`MutNoProbe` 等 mutant 仍必须被杀；`InvSafePark`/`LParkWakes` 不得削弱。
2. **语义 oracle**：新增确定性用例覆盖 signal before/during/after park、probe 窗口、host ack 与 owner park 并发、bounded timeout、fd close/reuse、retire/shutdown；现有 9 用例保持绿色。
3. **机制门七问**：第二 fd/condvar/模式事实的 authority、派生性、物化理由、替代机制、迁移/删除条件。
4. **真实消费者**：按 §5 的 owner 选项处理；不得用 bench/reference 代理声明 application-level 收益。
5. **A/A/A/B**：按 §7 的 C6/C1 预算；如果方向只影响 P-E/C1，P-B/C6 不适用。

**在这些条件满足前，不应把 C2/D 写入 W1 的最小 B。**

---

## 7. A/A/A/B 预算（C6/C1）

### 7.1 匹配格

P2-REVIEW H3 已确认：

- **P-B = `W02-uring-4k-d1`**，binary `03dbef09…`，与 `p1/AA-CALIBRATION.md` 的 **C6** 匹配（同 binary、同 case、同 tmpfs/4 KiB）。
- **P-E = `sluice-copy 4k/d4/w1 --sync none --no-atomic`**，binary `db972b19…`，与 **C1** 匹配（同 argv、同 binary）。
- 证据：`p2/P2-REVIEW.md:75-92`；`p2/PROFILE-SUMMARY.md:36-39` 的 binary sha256；`p1/AA-CALIBRATION.md:46-52,156`。
- **不要**再用 C5b（TP 4k d1 w1）、C3b（uring d16）、C7（d64）作为 P-B 的代理；P2-COST-MAP §5.6 的代理说法与 P2-REVIEW H3 冲突，以后者为准。

### 7.2 A/A 带（16 对实测；用于先验，不是 A/B 判据）

来源：`p1/AA-CALIBRATION.md:175-217,329-335,386-402`。

| 格 | 指标 | SD (pp) | hw 16 对 (%) | MDE80 16 对 (%) | 状态 |
|---|---|---:|---:|---:|---|
| **C6** | CPU | 0.997 | 0.471 | 0.747 | CPU 安静；wall ORDER_SENSITIVE |
| C6 | instr | 0.014 | 0.006 | 0.010 | 安静 |
| C6 | wall | 1.885 | 0.885 | 1.413 | **ORDER_SENSITIVE**（第二 arm wall +1.13 pp） |
| C6 | cycles | 2.876 | 1.368 | 2.155 | 可用但粗 |
| **C1** | CPU | 0.786 | 0.365 | 0.589 | QUIET（copy 尺度） |
| C1 | instr | 0.012 | 0.005 | 0.009 | 安静 |
| C1 | wall | 1.495 | 0.708 | 1.120 | 安静 |
| C1 | cycles | 2.346 | 1.121 | 1.758 | 可用但粗 |

注意：C6 wall 的 ORDER_SENSITIVE 要求完整 AB/BA 平衡；任何丢失 arm 导致不平衡，wall 不可用。C1 是 btrfs→tmpfs copy，不是磁盘/NVMe 结论（`AA-CALIBRATION.md:291-297`）。

### 7.3 所需对数（required n；本会话复算）

**命令**（在 `docs/evidence/621-progress-wait-path-20261118/` 下执行；只读导入 `tools/recompute_independent.py` 的 `required_n_power`/`required_n_halfwidth`/`mde80`）：

```bash
python3 -I - <<'PY'
import importlib.util
spec = importlib.util.spec_from_file_location('ri','tools/recompute_independent.py')
ri = importlib.util.module_from_spec(spec); spec.loader.exec_module(ri)
sd = {'C6 cpu':0.997,'C6 instr':0.014,'C6 wall':1.885,'C6 cycles':2.876,
      'C1 cpu':0.786,'C1 instr':0.012,'C1 wall':1.495,'C1 cycles':2.346}
for k,v in sd.items():
    print(k, 'SD=%.3f'%v,
          ' | '.join(f'd={d}%: {ri.required_n_power(v,d)}/{ri.required_n_halfwidth(v,d/2.0)}'
                     for d in (5.0,2.0,1.2,1.0,0.5)))
for k,v in sd.items():
    print(k, 'MDE80(n=36)=%.3f%%'%ri.mde80(v,36),
          'hw_t(n=36)=%.3f%%'%ri.ci_halfwidth_t(v,36))
PY
```

**输出摘要**（`n80 / n_half`，即 80% 功效所需 n / 95% CI 半宽≤δ/2 所需 n）：

| 格/指标 | δ=5% | δ=2% | δ=1.2% | δ=1% | 36 对 MDE80 / hw_t |
|---|---:|---:|---:|---:|---:|
| C6 CPU | 3/3 | 5/7 | **8/14** | 10/18 | 0.479% / 0.337% |
| C6 cycles | 5/8 | 19/35 | **48/91** | 67/130 | 1.381% / 0.973% |
| C6 wall | 4/5 | 10/17 | 22/41 | 30/58 | 0.905% / 0.638% |
| C1 CPU | 3/3 | 4/5 | **6/10** | 7/12 | 0.378% / 0.266% |
| C1 cycles | 4/6 | 13/24 | **32/62** | 46/88 | 1.127% / 0.794% |
| C1 wall | 3/4 | 7/12 | 15/27 | 20/37 | 0.718% / 0.506% |

**判读**

- 如果 W1 的计划效应是 **5% 过程级 cycles**（历史 R1 的口径），C6 cycles 只需 5/8 对，C1 cycles 4/6 对；36 对 A/B 足够。
- 如果 5% 是 **measured-loop 的 5%**、过程级稀释到约 **1.2%**（`AA-CALIBRATION.md:345-348`；`AA-CALIBRATION-2.md:451-458`），则：
  - **C6 cycles 需要 48 对（80%）/ 91 对（hw≤0.6%）**；36 对 MDE80 1.381% > 1.2%，**不够**。
  - **C1 cycles 需要 32 对（80%）/ 62 对（hw≤0.6%）**；36 对 80% 刚好，但 hw 0.794% > 0.6%，**半宽判据不够**。
  - C6/C1 CPU 在 1.2% 都足够（8/14、6/10），但 CPU 与 cycles 是不同指标；不能用 CPU 的功率替代 cycles 声明。
- **A/A 预算**：ledger §5.1 要求 A/A 36 对；16 对带只作先验。C6 cycles 16 对 MDE80 2.155%、C1 cycles 1.758%，不足以在 16 对判断 1.2% 效应；A/A 必须至少 36 对，且用匹配格 C6/C1，不得搬 C5b/C3b/C7。
- **A/A/A/B 总量**（若 owner 接受代理并做 1.2% cycles 判据）：
  - P-E/C1：A/A 36 + A/B 62 = **98 对**（hw≤0.6%）；若只要求 80% 功效，A/B 32，总 68 对。
  - P-B/C6：A/A 36 + A/B 91 = **127 对**（hw≤0.6%）；若只要求 80% 功效，A/B 48，总 84 对。
  - 若判据是 5% 过程级，按 ledger 的 A/A 36 + A/B ≥36（实际 5–8 对即可，但按 36 对执行以保留余量）→ 总 72 对。
- **不要**把 profile 的 `__poll` inclusive 24.93%/15.83% 当作可删 cycles；P2-REVIEW H1/M3 已说明 15.8/11.5 是下界，25.5/25.1 是混合 stage 桶，`__poll` inclusive 也不能直接当收益（P2-REVIEW `:30-50,128-143`）。
- **P-B 的 20 s 有界等待**：如果 B 是 blocking read，它不能用于 P-B；P-B 的 A/B 只能评估 A/B 方向的算术优化（预计 NOT_POWERED）。若 owner 要 material 的 uring d1 改进，必须另立拓扑/协议方向，不能把它算作 W1-D。

### 7.4 A/A 入口与判据（未运行）

ledger §8.1 给出入口；本评估**未运行**：

```bash
SLUICE_BENCH_WARMUP=2 SLUICE_BENCH_MEASURED=7 \
SLUICE_BENCH_CASE_FILTER='=W02-uring-4k-d1' \
SLUICE_BENCH_JSONL=/tmp/w1-c6-aa.jsonl \
SLUICE_BENCH_TMP_DIR=/tmp/621-fixture \
build-bench-ring/linux/x86_64/release/bench_w02_pipeline
```

以及 sluice-copy paired A/A（off-repo runner，ledger §8.1 `:447`；该 runner 含明文凭据，不能提交，复跑前必须参数化并从环境取凭据）。判据仍按 ledger §9.3：A/A 安静度门、AB/BA 交错、独立复算、语义 oracle/mutant 先过、环境门先行、Owner Gate 停止。

---

## 8. 对任何未来 B 的 falsifier 与停止条件

### 8.1 语义 falsifier（任一成立即 NO_GO）

1. 现有 9 用例 drain semantic oracle 任一失败；mutant `progress_source_mut_drain_success_keeps_pending` 不能被命名断言杀死（`xmake/tests.lua:568-570`）。
2. `ProgressSource.tla` 或新模型的 `InvSafePark`/`LParkWakes`/control sticky 失败；`MutParkDrain`/`MutNoProbe` 不再被杀。
3. host `acknowledge_progress_notification` 与 owner park 并发时，signal 在 probe/P3 窗口被 drain 后 owner 永久 park（P3 删除/绕过）。
4. io_uring CQE 在 drain 之后、probe 之前/之后到达时，owner 不返回 progress；或 probe 被跳过/降频后 CQE 只留 fd readiness 而 owner 睡眠。
5. `request_stop`/`close_admission`/shutdown 不能唤醒 blocking/condvar 等待；shutdown 返回而 accepted work 未结算。
6. fd close/reuse 后旧事件路由到新 context、或 `POLLNVAL`/EBADF 诊断被静默成 health（生命周期违规不再 fail-fast）。
7. 有界等待提前返回 deadline、EINTR 后重置满额 timeout、或 deadline 取消了 I/O。
8. host borrow/detach 与 owner wait 的模式切换丢唤醒，或 fd 不再是唯一 pollable handle。

### 8.2 性能 falsifier

1. A/A 不安静（超过 C6/C1 的 16 对带或 36 对门槛）；C6 wall 未保持 AB/BA 平衡。
2. 效应低于预注册 δ 的 MDE；36 对用于 1.2% C6 cycles 时直接判 NOT_POWERED。
3. 方向反转或 A/A 均值不零；负控制同向大效应；环境门（频率/governor/IRQ/介质）触发 ENVIRONMENT_BLOCKED。
4. 只有 stage 份额变化，没有独立 oracle/计数证据；把 `__poll` inclusive 或 stage 25.5% 当可删 cycles。
5. 用 bench/reference 代理声明 application-level 收益或 #604 G0 通过。

### 8.3 停止条件

- 任一语义 FAIL：立即 NO_GO，不进入性能 A/B（ledger §9.1.4）。
- 没有真实消费者且 owner 未批准代理：停在 NEEDS_OWNER，不启动 A/B。
- 设计需要第二通知权威/relay/新线程：退回 owner/架构评审，不在 W1 内实现。
- 机制门七问不能回答：不新增运行时状态/能力。

---

## 9. 本评估不能下的结论

- 不能说 W1 能删 5.0–5.5% cycles；只能说历史 R1 工作稿有此线索，且当前 profile 的等待路径有热点；该线索不是已证实收益（ledger §5.1 `:257`；P2 §5.1 `:352-365`）。
- 不能说 `__x64_sys_poll` 15.77%/11.46% 是上界；P2-REVIEW H1 已证明它是下界，`__poll` inclusive 是 24.93%/15.83%。
- 不能说 `progress/wait` 25.53%/25.12% 是 W1 可删量；它是混合 stage 桶（含 iou-wrk eventfd signal、调度等），P2-REVIEW M3 `:128-143`。
- 不能说 blocking read 只是“未测”；本评估给出的是语义不匹配（fd 标志/ack/timeout/kernel 注册），不是性能不足。
- 不能说 P-B/C6 适用于 blocking read：P-B 是 20 s 有界等待（`bench_w02_pipeline.cpp:52`）。
- 不能说 36 对 A/B 足以覆盖 1.2% C6 cycles：需要 48/91（§7.3）。
- 不能说 sluice-copy 的 A/B 是 #604 G0 或 application-level 收益（ledger §6.2 `:311-315`；P2-REVIEW M10）。
- 不能说 A1/A2 有 material 收益；它们只是 hygiene，预计低于噪声。

---

## 10. 本轮实际执行/未执行清单

### 10.1 已执行

| 检查 | 命令/方式 | 输出/结论 |
|---|---|---|
| 源码与证据读取 | `Read`/`sed`/`grep` 读取 `progress_source.hpp`、`async_io_context.cpp`、`uring_backend.cpp`、`async_backend.hpp`、`threadpool_backend.hpp`、contract、model、tests、P1/P2 文档 | 行号与引用见 §1–§7 |
| eventfd/dup/fcntl 事实 | `grep -n "eventfd(\|dup(\|dup2(\|fcntl\|EFD_" include/sluice/async/detail/progress_source.hpp src/async/*.cpp include/sluice/async/detail/*.hpp` | 仅 `progress_source.hpp:75` 的 `eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC)`；source 无 dup/fcntl |
| dup 共享 eventfd 文件状态标志 | `python3 -I - <<'PY'`（见 §3.4 D1，使用 `os.eventfd(0, os.EFD_NONBLOCK)`） | `after clear on dup: original False dup False`（清 dup 同时清原 fd） |
| io_uring 单 eventfd 注册 | `sed -n '245,285p' src/async/uring_backend.cpp; grep -rn register_notification_with_kernel` | `io_uring_register_eventfd(..., fd)` 单次注册；`BackendProgressPort` 借出 source fd |
| required-n 复算 | 见 §7.3 的 `python3 -I` 命令 | C6 cycles 1.2% = 48/91；C1 cycles = 32/62；36 对 MDE/hw 见表 |
| 工作树检查 | `git status --short`（会话开始/写入前） | 只有既有两个未跟踪 evidence 目录；未修改产品代码 |

### 10.2 未执行（明确不当作通过）

- 任何 `xmake f`/`xmake build`/`xmake test`。
- 任何 A/A、A/B、bench、`bench/build_profiles.sh`、`bench/run_matrix.sh`、`check_run.py`。
- 任何 perf/record/stat/eBPF/uprobe/strace。
- 任何 TLC/`scripts/verify_progress_source_model.sh`。
- 任何语义 oracle/mutant 运行；本评估没有重新验证现有 9 用例在当前 HEAD 的执行结果。
- 没有确认当前 master 的测试总数 `N`；ledger §8.2 的测试计数纪律仍适用。
- 没有运行 `p3` 之外的写入；未修改 `include/`、`src/`、`apps/`、`tests/`、`xmake.lua`、`xmake/`。

---

## 11. 参考坐标（便于复核）

| 主题 | 坐标 |
|---|---|
| W1 源码 | `include/sluice/async/detail/progress_source.hpp:104-232`；`src/async/async_io_context.cpp:573-705` |
| eventfd 创建/retire | `progress_source.hpp:74-86,284-290` |
| fused revalidation + drain | `progress_source.hpp:153-166` |
| probe / P3 | `progress_source.hpp:175-192`；`uring_backend.cpp:285-293` |
| poll/EINTR/POLLNVAL/POLLIN | `progress_source.hpp:198-230` |
| signal/interrupt | `progress_source.hpp:234-264` |
| drain/wake/suppression | `progress_source.hpp:414-460,466-489` |
| host borrow/ack/detach | `async_io_context.cpp:720-747`；`progress_source.hpp:274-277,524-545` |
| shutdown wait | `async_io_context.cpp:479-563`（尤其 `:511-550`） |
| bounded/retry | `async_io_context.cpp:22,600-607,644-691`；`request_scope.hpp:146-185`；`bench_w02_pipeline.cpp:52,271,384`；`copy_task.cpp:93` |
| PROG-01/02/03/04 | `docs/explicit-io-contract.md:828-949` |
| ProgressSource model | `formal/tla/ProgressSource.tla:12-108,297-340,477-488,713-719,851-852` |
| drain oracle | `tests/progress_source_drain_semantic_oracle_test.cpp:81-170,252-496,506-538` |
| suppression/borrow | `tests/progress_notification_suppression_test.cpp:92-174` |
| health fail-fast | `tests/request_scope_test.cpp:1273-1304` |
| fd reuse | `tests/shutdown_lifecycle_test.cpp:588-639` |
| ledger W1 | `CAMPAIGN-LEDGER.md:155-161,257,305-315,443-449,493,497,516-528` |
| P1 A/A | `p1/AA-CALIBRATION.md:24,29,46-52,156,175-217,329-335,345-348,386-402`；`p1/AA-CALIBRATION-2.md:26-27,397-409,435-459` |
| P2 cost map | `p2/P2-COST-MAP.md:356-365,419-431,472-483` |
| P2 review | `p2/P2-REVIEW.md:30-50,54-71,75-92,128-143,239-247,322-331` |
| R1 工作稿（已被取代） | `docs/evidence/621-round2-R1/REPORT.md:148,176`（未跟踪；只作线索） |

---

*本文件结束。所有未在 §10.1 执行的断言都是“未运行/未验证”；所有性能数字都是已发布证据或算术复算，不是本轮实测。*
