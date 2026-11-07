# #595 R3 续跑 — 第二轮定点修复与语义记录（BUILD_COMPLETION）

| 字段 | 值 |
|---|---|
| 角色 | R3 续跑语义补修实现者（不新增公开 API / 语义权威） |
| completionRoot | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z` |
| 基线 | `baseSha = 0a22a79195f1b913145fcd443496e7f70b08cdcf` |
| 研究 worktree | `/home/jnhu/sluice-595-e5/r3/worktree`（分支 `research-595-r3`，未提交） |
| 最终 patch | `evidence/r3-completion.patch`，sha256 `2596f204035f9695db47184b20ad8ece7b2168c54d8589b2060782aa7959bbc0` |
| patch 规模 | 15 文件，`+3999 / −64`（`git apply --stat`；含 5 个新文件） |
| 构建状态 | `buildOk = true`（liburing 主 profile；默认 no-liburing profile 仅编译 + 回落 smoke） |
| 语义状态 | `semanticsOk = true`（**实现者验证结论**；独立 verifier 尚未签字，见 §11） |
| 公开安装头 | `include/sluice/async/{async_io_context,request,request_scope,threadpool_backend,uring_backend}.hpp` 与基线 **零 diff**；`sizeof(AsyncIoContext)` 无宏/有宏均为 96 |
| 计时 | **本轮未做计时**；所有运行均为 smoke / 语义 oracle，数字不是性能结论 |
| 外部合同 | 未改 `docs/explicit-io-contract.md`、R1/R2 证据、主工作树或远端 |

## 1. 交付物与 patch 核验

| 交付物 | 路径 / sha256 |
|---|---|
| 最终 patch | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/evidence/r3-completion.patch`<br>`2596f204035f9695db47184b20ad8ece7b2168c54d8589b2060782aa7959bbc0` |
| 本文件 | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/evidence/BUILD_COMPLETION.md` |
| 主二进制 | `build/linux/x86_64/release/`（见 §8.1） |
| worktree 状态 | 10 M + 5 ??；公开安装头零 diff；detail/test-only 改动只在内部头与测试/bench |

```bash
cd /home/jnhu/sluice-595-e5/r3/worktree
git diff --check HEAD                         # exit 0
git apply --reverse --check .../r3-completion.patch
                                              # exit 0（post-image 与 worktree 一致）
git apply --stat .../r3-completion.patch      # 15 files, +3999/-64
sha256sum .../r3-completion.patch             # fc1857ce... (见上)
```

patch 只含 worktree 内容；未把二进制、`build/`、`.xmake/` 或 `/tmp` 证据写入 patch。

## 2. 独立审阅第二轮问题处置

| # | 审阅问题 | 处置 | 本轮证据 |
|---|---|---|---|
| 1 | 入口只对 `min(span,8)` precheck 且只把截断前缀交给 backend；span>cap 时 `stop=input_exhausted/untouched=0`，后 8 条既未计数也未报 suffix | **已修复**：无 precheck 错误时把完整 span 交给 backend（`test_access.hpp:102-105`）；backend 仍以 cap=8 截断并报告 `untouched=span-8`。precheck 失败时只提交有效前缀，suffix 从 `untouched_begin` 计 | `context_cap_suffix`（`tests/...:1530`）：16 元组经入口 → `accepted=8, stop=cap, untouched_begin=8, untouched=8`；suffix 8 条逐条入口接受；16 条全部结算。bench d16 B `untouched=9851`（此前恒 0） |
| 2 | 组门（control/recovery/SQ budget）只对 span≥2 生效，span=1 走 immediate 被接受；A1 与 B 的 acceptance policy 不同 | **按冻结定义保留**（单成员 immediate 是 CONTEXT_COMPLETION §5.4 与任务项 4 的明确要求；改成 gated 会使 A1≠A0 或改变 B suffix 策略）。新增 `r3_gate_stops` 计数把组门停止与单条接受分开；新增 `gate_defer` 证明同一状态下两条路径最终接受同一集合 | `gate_defer`（`:1596`）：同状态（SQ 满 + sticky control + ledger residue）下 span=1 接受、span=2 报 `control/untouched=2`，随后 suffix span=1 接受两成员；两条路径最终都结算 6/6 成员。`gate_stops` 计入 `bench/r3_ready_batch.hpp:64,168` 与 bench record |
| 3 | 在安装公共头 `async_io_context.hpp` 新增 `SLUICE_ASYNC_INTERNAL_TESTING` 条件成员，`sizeof` 96 vs 104，ABI/ODR 风险 | **已修复**：删除公共头的 `SettlementStartPauseGate` 成员/方法；settlement 暂停点移到 detail 头 `ProgressSource`（`progress_source.hpp:314-320,483`），通过 `TestAccess` 设置。公共安装头零 diff | `sizeof` 探针：无宏 96、有宏 96；`git diff HEAD --` 五个安装头为 0 行；`xmake/libraries.lua:62-65` 的“installed headers read no build-profile macro”恢复成立 |
| 4 | `admit_` 在 `dispatch_locked=false` 无锁读 `fatal_error_`，与锁内 poison 写构成 data race | **已修复**：非 locked 调用在 `dispatch_mtx_` 下读取 `fatal_error_`（`uring_backend.cpp:648-657`）；`dispatch_locked=true` 由调用者持锁。其他 `fatal_error_` 访问均在 `dispatch_mtx_` 下 | `post_poison_single`（`:1683`）：submitter 停在 `admit_` 入口，owner 通过 backend poll 毒化，恢复后 submit 观察到 poison 并返回 `backend_error`，不 reserve/不 binding；held 成员保留真实 CQE；shutdown completed |
| 5 | inert/非 Uring context 的入口返回默认成功形状（`accepted=0, stop=input_exhausted`），与 `submit_read` 的 `invalid_state` 不一致 | **已修复**：`backend_==nullptr` → `rejected/invalid_state`；非 Uring backend → `rejected/not_supported`，且 `batch_backend=false` 供 adapter 回落 | `inert_context`（`:1570`）：moved-from context 入口 `batch_backend=0, accepted=0, stop=rejected, error=invalid_state, untouched_begin=1, untouched=0`；`submit_read` 同码 |
| 6 | 非致命 transport-not-ready 报 `recovery` 但 error 默认 eof；单条 `flush_attempts` 固定 1；`sq_budget==0`/`poisoned_before` 不可达；生产宏 `SLUICE_R3_MUTANT_*` campaign 标签与语义注释 | **已修复/清理**：非致命 residue recovery 设 `error=would_block`（`uring_backend.cpp:522-527`）；单条 `flush_attempts` 取 `submit_flushes_` 差值（`:482-486`）；删除 `sq_budget==0` 早退与 batch 内不可达 `poisoned_before` 分支；宏改为 `SLUICE_READY_BATCH_MUTANT_*`；删除生产/detail 头中的 campaign/requirement 叙述注释 | `sq_full_residue`/`sq_partial_residue` 新增 `error=would_block` 断言；单条正常路径仍 `flush_attempts=1`；mutant 目标按新宏名构建并仍被杀 |

结论：问题 1/3/4/5/6 为实际缺陷，已修复并由本轮测试/探针覆盖；问题 2 是冻结臂定义的一部分（单成员 immediate），本轮不改变该语义权威，改用计数与等价性测试把测量归因显式化，并在 §11 记录限制。

## 3. Context 入口、生命周期与锁顺序

### 3.1 位置与调用链

| 位置 | 内容 |
|---|---|
| `include/sluice/async/detail/test_access.hpp:35-128` | `detail::TestAccess::ReadyReadsBatch` + `context_submit_ready_reads`（detail/test-only；非安装头） |
| `include/sluice/async/detail/initiation_precheck.hpp:12-18` | 与 `submit_read` 共用的唯一 precheck 定义 |
| `src/async/async_io_context.cpp:243-350` | `submit_*` 使用同一 `detail::initiation_rejection` |
| `bench/r3_ready_batch.hpp:84-136` | A1 span=1、B span=N、B suffix span=1 都调用同一入口；B 不再直调 backend |
| `bench/bench_w03_external_loop.cpp:351-425` | A0 逐条构造 `ReadOp` 并立即 `ctx.submit_read`；A0 不使用 adapter/ready 容器 |

```
Adapter::submit
  -> TestAccess::context_submit_ready_reads(ctx, span)
       1. 逐成员 initiation_rejection(ready[i].file, FileOperation::read)      [无锁]
       2. lock_guard(ctx.access_mtx_)
       3. backend_ == nullptr ? rejected/invalid_state
          dynamic_cast<UringAsyncBackend*> == nullptr ? rejected/not_supported
       4. execution_closed_ ? rejected/invalid_state
       5. backend_->submit_ready_reads(无 precheck 错误 ? 完整 span : 有效前缀)
             -> dispatch_mtx_
                  -> admit_(..., dispatch_locked=true)
                       -> core reserve/accept                    [core mutex_]
                       -> reset_delivery_locked_                 [dispatch_mtx_]
                  -> dispatch_one_locked -> claim_execution        [core mutex_]
                  -> submit_transport_locked -> io_uring_submit
                  -> service_pending_controls_locked_
       6. accepted keys 物化为 Request<std::size_t>（锁内，无 host 回调）
```

- 入口不会在持有 `access_mtx_` 时调用 `ctx.submit_read`（不会重取同一锁）。
- precheck 在取锁前，与 `submit_read` 的顺序一致；precheck 只扫描前 `min(span,cap)` 个成员，
  因为 backend 从不尝试超过 cap 的成员；无 precheck 错误时完整 span 交给 backend 以得到
  正确的 cap/suffix 报告（审阅问题 1）。
- `backend_ == nullptr` / 非 Uring / execution closed 都在 `access_mtx_` 下判定，返回拒绝形状，
  不再返回“成功空批”。

### 3.2 锁顺序

```
access_mtx_ -> dispatch_mtx_ -> core mutex_
access_mtx_ -> ProgressSource mtx_   (context close/stop signal)
dispatch_mtx_ -> ProgressSource mtx_ (poison signal; source 是叶子)
```

| 路径 | 锁 | 依据 |
|---|---|---|
| 新入口 batch | `access_mtx_` → backend `dispatch_mtx_` → core `mutex_` | `test_access.hpp:49-128`；`uring_backend.cpp:499-600`；`admit_` `:637-733` |
| 单条 submit | `access_mtx_` → core（reserve/accept）→ `dispatch_mtx_`（reset/dispatch） | 顺序取锁，非嵌套；`uring_backend.cpp:735-752` |
| cancel | `access_mtx_` → `dispatch_mtx_` → core | `uring_transport.cpp:593-627` |
| progress/poll | 入口 `access_mtx_`；pass 内 `dispatch_mtx_` → core | `async_io_context.cpp:441-447`；`uring_backend.cpp:837-858` |
| shutdown | 入口 `access_mtx_`；settlement 内 core/`dispatch_mtx_`/source；结束再取 `access_mtx_` 写 `execution_closed_` | `async_io_context.cpp:475-570`、`:866-890` |

`ProgressSource` 的 physical probe 在 source 锁之外（`progress_source.hpp:172-181`）；core/source
不反向调用 backend 或取 `access_mtx_`，未见锁序反转。`fatal_error_` 的所有访问现在都在
`dispatch_mtx_` 下（`admit_` 的非 locked 路径在 `:648-657` 单独取锁）。

### 3.3 生命周期

- 入口只持有 context 的 `access_mtx_` 与 backend 的既有锁；不拥有新的生命周期权威。
- context 销毁要求独占、无外部调用（THREAD-01）；drain/retire 仍由 `AsyncIoContext::shutdown`
  与 `drive_settlement_` 的既有 owner 路径完成。
- inert/moved-from context：入口与 `submit_read` 都返回 `invalid_state`。
- post-execution-closed：入口返回 `rejected/invalid_state`；retire 与 `execution_closed_` 写入之间的
  窗口（recovery + accepted=0 + backend_error + !health_failed + admission closed）也映射为
  `invalid_state`（`test_access.hpp:113-121`）。poison 保持 `backend_error`（health 事实）。

## 4. 并发 submit / driver / close：逐请求 acceptance

### 4.1 线化点

- 每个成员独立 `core_->reserve()`（`request_core.cpp:165-182`）与 `core_->accept()`
  （`:184-213`）；`accept` 在 core `mutex_` 下检查 `admission_open_`、建立 `binding_live`/
  descriptor/borrow/execution_refs 并 `outstanding_++`。
- 批量不是整组原子：`accepted` 是连续前缀，`keys[0..accepted)` 各自一个 `Request`；
  `stop==rejected` 时被尝试项在 index `accepted`，`untouched_begin==accepted+1`；其余 stop 的
  `untouched_begin==accepted`（`uring_backend.cpp:592-608`）。

### 4.2 确定性 barrier

| Gate | 位置 | 用途 |
|---|---|---|
| `UringAsyncBackend::PreAcceptCommitPauseGate`（既有；新增 `calls/target`） | `admit_` 在 reserve→prepare 后、`core_->accept` 前（`uring_backend.cpp:721`；seam `uring_test_seams.hpp:37-43`） | 把指定序号的成员停在 reserve 与 accept 之间；`target=1` 保持既有测试行为 |
| `detail::ProgressSource::PauseGate`（既有类型；新增 settlement-start 用法） | `drive_settlement_` 入口、`close_admission` 前（`async_io_context.cpp:481`；`progress_source.hpp:314-320`） | 把 owner 停在 settlement 入口，确定 close 与 submit 的先后；**不在安装公共头** |
| `UringAsyncBackend::SubmitEntryPauseGate`（既有） | `admit_` 入口（`uring_backend.cpp:645-646`） | 把单条 submit 停在 fatal/poison 检查之前 |

均为 `SLUICE_ASYNC_INTERNAL_TESTING` 下的 test seam；生产构建无此状态；测试用 atomic
pause/resume + `yield`，不用 sleep 作为并发证明。

### 4.3 确定性交错结果

| 场景 | 交错 | 结果 |
|---|---|---|
| `acceptance_barrier`（`:1900`） | owner 停在 settlement-start；producer 停在 pre-accept（已 reserve）；close 后恢复 producer | `accepted=0, stop=rejected, error=invalid_state, untouched_begin=1, untouched=1`；rollback；无 Request/binding；owner completed；occupancy 全零 |
| `mixed_acceptance`（`:1454`） | 成员 0 已 accept，成员 1 停在 pre-accept；close；恢复成员 1 | `accepted=1, stop=rejected, error=invalid_state, untouched_begin=2, untouched=1`；成员 0 的 Request 可结算；成员 2 untouched。**整批非原子** |
| `shutdown_accepted`（`:1980`） | owner 停在 settlement-start；producer 接受并派发 2 条 pipe read；close；写入 pipe | 2 条进入有限 accepted set（`accepted_live=2, public_bindings=2`）；drain 后各自 ready、字节正确、occupancy 全零 |
| `post_poison_single`（`:1683`） | submitter 停在 `admit_` 入口；owner/backend 毒化；恢复 submitter | submit 观察到 poison，返回 `backend_error`，不 accept/binding；held 成员保留真实 CQE；shutdown completed |

### 4.4 close 先于 retiry 的论证

1. `drive_settlement_` 在循环/收敛前 `core_->close_admission()`（`async_io_context.cpp:484`）。
2. `core_->accept` 在同一 core `mutex_` 下检查 `admission_open_`；close 后不会产生新 acceptance，
   close 前赢得的 accept 必在 `occupancy()` 中可见。
3. `settlement_converged_` 要求 `backend_->internal_work_retired()` 与 `occupancy()` 的
   outstanding/execution_refs/control_refs/publication_inflight/observer 全零（`:465-473`）。
4. 因此 retire 只会在已接受集合结算后发生；retire 后的入口 accept 因 admission closed 失败，
   不会留下 accepted-but-undispatchable 请求。
5. batch 的所有 ring 访问在 `dispatch_mtx_` 内并锁内重查 `have_ring_`
   （`uring_backend.cpp:499-506`）；`retire_execution_resources` 同锁写 false 后 `queue_exit`
   （`:383-414`）。`have_ring_` 为 `std::atomic<bool>`（`uring_backend.hpp:358-360`），
   `fatal_error_` 的读也在 `dispatch_mtx_` 下，合法 submit/close 并发下不再有这两类 data race。

## 5. partial submit / residual 逐请求证据

### 5.1 已消费前缀归属

`partial_submit_error_modes`（`:2048`）对 8 成员批量注入：

| 模式 | 首次 flush 内核消费 | 断言 |
|---|---|---|
| `consume_all` | 8 | ledger=0；无 residue；无第二次 submit |
| `hold`（rc=0） | 0 | ledger=8；residue=true；下一次 owner pass 重新提交同一 pending 8 SQE（不是新 SQE） |
| `partial_first`（rc=1） | 1 | ledger=7；residue=true；retry 首项 offset=4096 |
| `partial_then_eagain`（rc=-EAGAIN，已消费 2） | 2 | ledger=6；retry 首项 offset=8192 |
| `partial_then_eintr`（rc=-EINTR，已消费 2） | 2 | ledger=6；retry 首项 offset=8192 |

每个 retryable 模式断言：`first_ready-second_ready==consumed`；
`second_first_offset==consumed*4096`（第二次 submit hook 读取 SQE `off`）；
`peek_next_cookie/live_cookies` 不变（retry 只重新提交 pending SQE）；retry 后 ledger=0/
dispatch=0；8 个成员各自 CQE/结果/字节。residue 由 backend 广告并 signal，下一次 owner
pass 只消费 ledger 前缀（`uring_backend.cpp:609-619`；`uring_transport.cpp:242-335`）。

### 5.2 accepted 与 untouched

- `untouched_begin` 精确区分 accepted 前缀与未尝试 suffix；`stop==rejected` 时被尝试项在
  `accepted`，不落入 untouched。
- 入口的 span>cap 修复后，backend 看到完整 span，`accepted=8`、`stop=cap`、
  `untouched=span-8`，不会把后段静默消失（`context_cap_suffix`）。
- 调用者只从 `untouched_begin` 继续；已接受 key 不会再作为 suffix；`REDISPATCHES_CONSUMED`
  变异体因 router exhaustion 被杀。

### 5.3 poison / overflow

- `poison_report_is_recovery`（`:2136`）：8 成员、消费 1 后 `-EIO`；`stop=recovery,
  error=backend_error, residue=true`；可见成员真实 4 KiB 完成；7 个不可见成员
  `backend_error`，绝不 success；shutdown completed。
- `overflow`（`:1210`）：32 成员、CQ 16；`cq_overflow_for_test()` 在首次 reap 前观察
  `io_uring_cq_has_overflow`，随后 `get_events>=1`；32 条各自结果/字节正确。此前该断言偶发
  （前置条件未观察）；本轮先观察前置事实再断言，未放宽成员完成断言。

## 6. 批量算法约束

| 约束 | 实现 / 证据 |
|---|---|
| 单条/不具资格走现有 immediate | `ready.size()==1 || !batch_eligible_read(front)` → `admit_` + `dispatch_after_accept`/zero-op（`uring_backend.cpp:467-495`）；64 KiB case 首条 ineligible，其余逐条 |
| 只有 4 KiB read 入批 | `batch_eligible_read = len==4096 && dst!=nullptr`（`:437-439`） |
| 同 ring/同轮/输入顺序不变 | 入口按 `ready[0..)` 顺序 precheck/admit/dispatch；keys 与 ready 一一对应 |
| cap=8 只限制一个 batch | `limit=min(span,8,sq_budget)`（`:528-531`）；suffix 由调用者 span=1 同一入口 |
| 无新 timer/线程/队列/每请求分配 | 入口只用调用者 span、栈上 `ReadyReadsBatch`（固定 `array<Request,8>`）、既有 dispatch/ledger/prepared/router；adapter ready/accepted 由 consumer 预分配 |
| 不在 backend 锁内跑 host/等 future | 锁内只做 admit/dispatch/submit/service-control；signal/note_outstanding 在解锁后；不调用 deliver_event/observer/host |
| 每成员独立 SQE/Request/CQE/错误/取消/结果/borrow | 独立 reserve/accept、cookie/route/ledger/CQE；测试逐成员校验 |
| 不跨调用持 SQE、不 defer flush | accept→push→dispatch→一次 `submit_transport_locked` 在同一 `dispatch_mtx_`；`flush_attempts` 取 `submit_flushes_` 差值（覆盖 mid-batch submit） |
| A1/B 同一入口 | `bench/r3_ready_batch.hpp:94-128`；A1 span=1，B span=N，suffix span=1 |
| A0 不构造组缓冲 | `bench_w03_external_loop.cpp:351-425`；A0 内联构造并立即 `submit_read`；ready/accepted 只在 adapter 存在时 reserve |

## 7. 报告/诊断闭合（审阅问题 6）

- 非致命 transport-not-ready：`stop=recovery, error=would_block`（`uring_backend.cpp:522-527`），
  与致命 poison 的 `recovery/backend_error` 可区分；`sq_full_residue`/`sq_partial_residue`
  已加断言。
- 单条 immediate 的 `flush_attempts` 取 `submit_flushes_` 差值（`:482-486`），不再固定 1。
- 删除 `sq_budget==0` 早退（它与 `limit=0` 的报告形状相同，且 ledger/ring 不变式下不可达）
  与 batch 内不可达 `poisoned_before` 分支；`retire_undispatched_after_poison_locked_` 仍由
  单条 `dispatch_after_accept` 使用。
- 变异宏改为中性名：`SLUICE_READY_BATCH_MUTANT_INCLUDES_PENDING_TRANSPORT` /
  `..._RESIDUE_UNADVERTISED` / `..._REDISPATCHES_CONSUMED`；xmake/tests 同步。
- 删除生产/detail 头中的 campaign/requirement 叙述注释；公共安装头零 diff。

## 8. 构建、smoke 与回归（本轮实际执行）

所有构建/运行均持有 `flock /home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`。活动配置为
`xmake f -m release --liburing=y`（`builddir=build`）；no-lib 配置在 `/tmp` 且已恢复。

### 8.1 二进制 sha256（`build/linux/x86_64/release/`）

| 产物 | sha256 |
|---|---|
| `bench_w03_external_loop` | `0ebfb7f0c204579fe2cc9e8d7e88cee2de9112e25c0cfb11e8502e59fc4fe2d1` |
| `bench_w02_pipeline` | `4e719b1e4dc2f0575812f4310b54e96c443f8ce8d1c6dd39c81bf5ec91351497` |
| `uring_ready_batch_research_test` | `1372b3468b8489367bfe0944bf3f537775cb8d4de9391165ae03120485770a31` |
| `uring_ready_batch_fault_test` | `18395c0a570862147436ff11624eec7276ed1851546df8ebb4ad6590a4fe3bba` |
| `uring_ready_batch_deterministic_test` | `c54a9222ed7121935b264417e6b7a65c2df6c8a2f943e98f71c01a602d59e700` |
| `uring_submit_boundary_test` | `e2fad3e32863c08185396fd9a08512688363940b491a500e8cd44c22a5d23ba4` |
| `uring_progress_race_test` | `25bf8746029c8714124fdf5980e87b935d4532e1e1bb24ee2a5bc8ac5134d18f` |
| `uring_flush_count_oracle_test` | `e62c64415d4a1c40621998e6f0996685f5f3053a6092bb722f38f26b268894a4` |
| `uring_external_loop_test` | `5b151d26b165936449d5f1e46c25c8373eb11b1fc77bb12147bec1b57031e2c0` |
| `uring_public_consumer_probe` | `7dfc2dd2d17da59ea2ddfb4db397149f988736e43008b4741255e35729e6ded1` |
| `uring_request_core_integration_test` | `37d256a70442da42a7ca54e1d76cb9217d2895ebea677a7edb79ebd8f904417b` |
| `shutdown_lifecycle_test` | `3ccec804d5257a67ccc796b0128b14a704316ee50b591adb5467447327f7bcbf` |
| `libsluice_async.a` | `c7eed8da54cf7ff6e758b7a3e6fd183bd1e82961d812da65a85b944b04300592` |

A0/A1/B 共用同一 `bench_w03_external_loop` 二进制。

### 8.2 构建命令

```bash
cd /home/jnhu/sluice-595-e5/r3/worktree
xmake f -m release --liburing=y
xmake build -y bench_w03_external_loop bench_w02_pipeline \
  uring_ready_batch_research_test uring_ready_batch_fault_test \
  uring_ready_batch_deterministic_test \
  uring_submit_boundary_test uring_progress_race_test uring_flush_count_oracle_test \
  uring_external_loop_test uring_public_consumer_probe \
  uring_request_core_integration_test shutdown_lifecycle_test
# build ok
```

默认 profile（no liburing，仅编译 + threadpool B 回落 smoke）：

```bash
rm -rf /tmp/r3-completion-nolib2
xmake f -m release --builddir=/tmp/r3-completion-nolib2
xmake build -y sluice_async bench_w03_external_loop bench_w02_pipeline   # build ok
env SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 \
  SLUICE_BENCH_CASE_FILTER='W03-threadpool-4k-d4' SLUICE_595_R3_ARM=B \
  SLUICE_BENCH_TMP_DIR=/tmp/sluice-r3-smoke SLUICE_BENCH_SKIP_DISK=1 \
  /tmp/r3-completion-nolib2/linux/x86_64/release/bench_w03_external_loop
# === W-03 complete, failing repetitions=0 === (rc=0)
xmake f -m release --liburing=y    # 恢复
```

### 8.3 语义 oracle / 回归（全部 exit 0）

```bash
for t in uring_ready_batch_research_test uring_ready_batch_fault_test \
         uring_ready_batch_deterministic_test uring_submit_boundary_test \
         uring_progress_race_test uring_flush_count_oracle_test \
         uring_external_loop_test uring_public_consumer_probe \
         uring_request_core_integration_test shutdown_lifecycle_test; do
  ./build/linux/x86_64/release/$t
done
```

输出摘要：

| 目标 | 输出 |
|---|---|
| `uring_ready_batch_research_test` | `all research oracles passed`（`two_ready_reads_one_flush` 走 Context 入口，验证 shipped archive） |
| `uring_ready_batch_fault_test` | `all research fault oracles passed` |
| `uring_ready_batch_deterministic_test` | `all deterministic ready-batch oracles passed`（**26 场景**；连跑 2 次全绿） |
| `uring_submit_boundary_test` | `all 2 ... passed` |
| `uring_progress_race_test` | `all 23 ... passed` |
| `uring_flush_count_oracle_test` | `all checks passed` |
| `uring_external_loop_test` | `all 9 ... passed` |
| `uring_public_consumer_probe` | `consumer TU selected io_uring through the public factory` |
| `uring_request_core_integration_test` | `21 of 21 ... passed, 0 not run` |
| `shutdown_lifecycle_test` | rc=0（fail-fast 子测试按预期 terminate 后 PASS，父进程 exit 0） |

确定性场景（26 个，`--only=` 名与证明点）：

| 场景 | 证明点 |
|---|---|
| `context_precheck` | 入口 precheck 与 `submit_read` 同码；valid prefix 逐成员接受；rejected 不落 untouched |
| `context_cap_suffix` | 16 元组 → accepted=8/stop=cap/untouched=8；suffix 8 条逐条接受并结算（审阅 1） |
| `inert_context` | moved-from context 入口/`submit_read` 都 `invalid_state`，非成功空批（审阅 5） |
| `capacity_boundary` | 容量在成员间耗尽：accepted=1、rejected/would_block、untouched_begin=2 |
| `acceptance_barrier` | close 在 reserve 与 accept 间获胜：rollback、无 binding、owner 可结算 |
| `mixed_acceptance` | 成员 0 accepted、成员 1 在 accept 前 close：accepted=1/rejected/untouched_begin=2；整批非原子 |
| `shutdown_accepted` | accepted-before-close 的有限集合被 owner drain 全部发布 |
| `partial_modes` | rc=8/0/1/-EAGAIN/-EINTR 的 ledger/前缀/retry/offset/无重发 |
| `poison_report` | poison 报 recovery+backend_error；可见成员真实完成、不可见成员不伪造 success |
| `control_stop` | 既有 control intent 使入口整组 untouched；control 最终 serviced |
| `gate_defer` | 同状态下 span=1 接受、span=2 报 control/untouched；suffix 接受同一集合（审阅 2 的等价性证据） |
| `post_poison_single` | submit 在 admit 入口暂停，毒化后恢复观察到 poison 并拒绝（审阅 4） |
| `sq_partial_residue` | SQ 仍有余量但 residue 存在时整组 untouched、零 flush、error=would_block |
| `d2_positive` | A0/A1 各 2 次单 SQE enter，B 1 次 2-SQE enter；三者独立 CQE/字节 |
| `d2_negative` | 单条 refill 走 immediate，无 defer/residue/notification |
| `d1_dependency` | 依赖未解析的第二条不进入 ready group/route |
| `d3_budget` | 单条/3 条/cap 8/ring budget 4 的前缀与 suffix 精确 |
| `sq_full_residue` | SQ 满 + ledger 残渣 → accepted=0/recovery/would_block/整组 untouched |
| `partial_submit` | 2 成员 rc=1 的 ledger/retry 前缀（seam oracle） |
| `cancel_ordering` | cancel/control CQE 与 original CQE 任意顺序只有一个 terminal |
| `prepark_wake` | owner 在 pre-park gate 冻结时入口提交的完成不丢唤醒 |
| `overflow` | 32 成员、CQ overflow 先被观察，再由 get_events flush，全部完成 |
| `poison` | 2 成员 partial+poison 的可见/不可见归属 |
| `shutdown` | accepted batch 在 shutdown drain 后全部发布；retired backend 入口报 recovery（seam 契约） |
| `borrow` | 每个成员 borrow 保留到自己的 CQE；buffer 不被提前写 |
| `stale_generation` | consume 后槽复用推进 generation；旧 key stale；无重复 SQE |

原 BASE 13 场景（`correctness/CORRECTNESS.md` §2）全部包含在 26 场景中并重跑通过；其中
7/8/11/12/13 的批次提交改为经 Context 入口（7/11 的 seam 后置检查仍保留 backend 直调），
9 增加 overflow 前置观察。既有断言未放宽。

### 8.4 三臂 smoke（非计时）

命令模板同前（`WARMUP=0 MEASURED=1`、`SLUICE_595_R3_CASES=1`、`SLUICE_BENCH_STATS=1`、
tmpfs `/tmp/sluice-r3-smoke`、`SKIP_DISK=1`）。

| case | A0 | A1 | B |
|---|---|---|---|
| `W03-uring-4k-d2-r3d2pos` | rc=0 | `entries=2 groups=0 singles=2 accepted=2 prefix_max=1 flushes=2 gate_stops=0` | `entries=1 groups=1 accepted=2 prefix_max=2 flushes=1 gate_stops=0` |
| `W03-uring-4k-d16` | rc=0 | `entries=20000 singles=20000 accepted=20000` | `entries=11129 groups=1277 singles=9852 accepted=20000 prefix_max=8 suffix_singles=9851 flushes=11129 gate_stops=0 untouched=9851` |
| `W02-uring-4k-d16` | rc=0（单路径） | — | — |

所有 case `failing repetitions=0`，byte oracle 通过；d16 的计数受调度影响，不是常量。
默认 profile 的 threadpool B 回落 `ctx.submit_read`，`entries=0`、rc=0。

### 8.5 纯 submit enter 计数（strace；功能证据）

d2 positive（3 phase）：A0 6×`io_uring_enter(5,1,0,0)`；A1 6×`1`；B 3×`io_uring_enter(5,2,0,0)`。

### 8.6 变异体

```bash
xmake build -y uring_ready_batch_det_mut_includes_not_ready \
  uring_ready_batch_det_mut_residue_dropped \
  uring_ready_batch_det_mut_redispatch_consumed
```

| 变异体 | 结果 |
|---|---|
| `SLUICE_READY_BATCH_MUTANT_INCLUDES_PENDING_TRANSPORT` | rc=1，17 failures |
| `SLUICE_READY_BATCH_MUTANT_RESIDUE_UNADVERTISED` | rc=1，9 failures |
| `SLUICE_READY_BATCH_MUTANT_REDISPATCHES_CONSUMED` | rc=134，`router exhaustion` terminate |

## 9. 每项 OPEN-1 回执

> 状态词：**PROVED** = 本轮实现 + 实际运行/代码论证；**PARTIAL** = 部分实现/部分交错；
> **NOT_RUN** = 本轮未做；**BLOCKED** = 需公开契约变更/缺资源/无法修复的正常调用反例。

| ID | 状态 | 回执 |
|---|---|---|
| OPEN-1.1 | PARTIAL | A0/A1/B 同一 R3 二进制并实际运行：A0 复原（内联构造 + 立即 `submit_read`，无 ready span/组缓冲）；A1/B 同一 Context 入口；strace d2 A0/A1 6×`to_submit=1`、B 3×`to_submit=2`。A_R2/B_R2 未构建/未运行（需 `271b81bf + 7be9d014…` 树），仍 OPEN，不是 BLOCKED。 |
| OPEN-1.2 | PROVED | 单一 detail/test-only 入口；A1/B 同一函数；B 不绕过 context；锁序 `access_mtx_ → dispatch_mtx_ → core mutex_`，无反向嵌套/锁内 host。组门只决定“是否成组”，span=1 按冻结定义走 immediate；`gate_stops` 计数与 `gate_defer` 证明两条路径最终接受同一集合。 |
| OPEN-1.3 | PROVED（已测交错） | pre-park gate + 入口批量完成不丢唤醒；residue 广告并由下一次 owner pass 消费；23/23 progress race + external-loop smoke。 |
| OPEN-1.4 | PROVED（记录窗口） | `have_ring_` atomic；batch ring 访问全在 `dispatch_mtx_` 内并锁内重查；`fatal_error_` 的非 locked 读也在 `dispatch_mtx_` 下；execution closed/retire 窗口映射 `invalid_state`；`acceptance_barrier`/`mixed_acceptance`/`shutdown_accepted`/`post_poison_single` 覆盖 close/poison 交错。A0 基线路径仍可能返回 `backend_error`；未跑 TSan。 |
| OPEN-1.5 | PROVED | reserve/accept 各自线化；`acceptance_barrier`（index 0 拒绝）、`mixed_acceptance`（accepted=1、index 1 拒绝、index 2 untouched）、`capacity_boundary`（accepted=1/would_block/untouched_begin=2）证明非原子接受；accepted 前缀由 shutdown drain 结算。 |
| OPEN-1.6 | PROVED | 每个 accepted key 恰一个 `Request`（`test_access.hpp:123-125`）；rejected/untouched 不物化；borrow 测试每个 buffer 到自己 CQE 前不被写；测试正常析构。 |
| OPEN-1.7 | PROVED（data SQE） | rc=8/0/1/-EAGAIN/-EINTR 覆盖；ledger=未消费前缀；retry 首 pending offset=consumed×4096；cookie/live route 不新增；无已消费重发。data+cancel-control 混合 partial 未单独脚本化（control 消费由 cancel_ordering/control_stop 覆盖）。 |
| OPEN-1.8 | PROVED | cap 16→8、ring budget 4/6、SQ 仍有余量但 ledger residue（8/6）均精确返回 accepted/untouched；`flush_attempts` 取 `submit_flushes_` 差值；residue 时 accepted=0/flush=0/error=would_block；span>cap 的 suffix 已正确报告（审阅 1）。 |
| OPEN-1.9 | PROVED | residue 广告并 signal；下一次 owner `poll`/`wait_one` 只重试剩余前缀；23/23 progress race；ledger 清零。 |
| OPEN-1.10 | PROVED | accepted/suffix 由 `untouched_begin` 精确分开；逐 key 唯一；retry 不新建 SQE/cookie；`REDISPATCHES_CONSUMED` 因 router exhaustion 被杀。 |
| OPEN-1.11 | PROVED | `control_stop` 经 Context 入口：control intent 使整组 untouched、零 flush，后续 control 被 serviced；`cancel_ordering` 覆盖 control/original CQE 乱序、sticky intent、重复 cancel 幂等。 |
| OPEN-1.12 | PROVED | poison 批量 `stop=recovery,error=*fatal_error_`；8 成员 partial+poison 保留可见成员真实完成、不可见成员 `backend_error`、绝不 success；overflow 测试先观察 `io_uring_cq_has_overflow`，32 完成不丢。 |
| OPEN-1.13 | PROVED（无 observer） | 两批经 Context 入口：复用槽推进 generation、旧 key not_found、无新 SQE；borrow 经入口。observer-armed 批量交错未构造。 |
| OPEN-1.14 | PARTIAL | 机制界：无新 timer/thread/queue/per-request heap；栈上固定 `array<Request,8>`；既有 dispatch/ledger/prepared/router。RSS/allocation-counter/resource gate NOT_RUN。 |
| OPEN-1.15 | NOT_RUN | 无正式计时、A/A、负控、配对 bootstrap、资源门、results.jsonl；性能问题仍全部 NOT_RUN。smoke/strace 只是功能证据。 |
| OPEN-1.16 | BLOCKED | `gh issue view 604` → state OPEN、updatedAt 2026-11-03T20:37:04Z，body 无 `READY_FOR_595`；W-03 是合成 benchmark，不能替代 #604 G0 真实应用门。 |
| OPEN-1.17 | PROVED（源码形状 + 功能） | A0 内联构造/立即提交、无 ready span/组缓冲、无 adapter；`ready/accepted` 只在 adapter 存在时 reserve；strace A0 6×`to_submit=1` vs B 3×`to_submit=2`；A0/A1/B smoke rc=0。旧 A0 数值未复用；计时未重跑。 |

## 10. 本轮发现并修复的真实缺陷

1. **span>cap suffix 丢失（审阅 1）**：入口无 precheck 错误时现在把完整 span 交给 backend；
   16 元组实测 `accepted=8, stop=cap, untouched=8`，后 8 条由 suffix 逐条接受；bench
   `r3_untouched` 在 d16 变为非零（9851），测量字段恢复有效。
2. **公共安装头 ABI（审阅 3）**：删除公共头的 test-only gate 成员；settlement 暂停点移入
   detail `ProgressSource`。`sizeof(AsyncIoContext)` 无宏/有宏均 96；五个安装头零 diff。
3. **`fatal_error_` data race（审阅 4）**：`admit_` 的非 locked 路径在 `dispatch_mtx_` 下读取；
   `post_poison_single` 证明暂停的单条 submit 观察到 poison 并拒绝。
4. **inert/非 Uring 入口成功形状（审阅 5）**：改为 `rejected/invalid_state` 或
   `rejected/not_supported`；`inert_context` 覆盖。
5. **poison 报告与不可达分支（审阅 6）**：poison 报 `recovery/backend_error`；非致命 residue
   报 `would_block`；单条 `flush_attempts` 取差值；删除不可达分支；变异宏改名；删除
   campaign/requirement 注释。
6. **overflow oracle 前置条件**：先观察 `io_uring_cq_has_overflow` 再断言 `get_events>=1`；
   成员完成断言不变。

最小反例：**无残留语义反例**。上述 1/2/3/4/5 是已修复的真实缺陷；overflow 是测试前置
条件缺陷，已按“先观察前置事实再断言”修正。审阅问题 2 是冻结单成员 immediate 定义的
必然差异（见 §2），不是请求丢失或语义反例；`gate_defer` 证明最终接受集合一致。

## 11. 限制与未覆盖

1. **独立审阅**：`semanticsOk=true` 是本实现者基于本轮测试/代码论证的结论，不是独立
   verifier 签字；OPEN1_RECEIPTS 的证明义务仍应由独立审阅者复核。
2. **未跑计时**：无 A/A、A/B、负控、配对统计、资源门、RSS/线程/CPU；所有性能问题仍
   `NOT_RUN`。smoke 与 strace 计数不是性能结论。
3. **未跑 TSan/ASan/静态分析**：`fatal_error_`/`have_ring_` 的修复依据代码审查与锁序
   论证；`post_poison_single` 是确定性语义测试，不是动态 race 检测。
4. **A_R2/B_R2 未构建/未运行**：需要 `271b81bf + 7be9d014…` R2 树；本轮只做 R3 语义。
5. **组门与单成员 immediate 的冻结差异**：A1（span=1）与 B（span≥2）在同一 transport
   状态下 acceptance 时机不同；这是 CONTEXT_COMPLETION §5.4 与任务项 4 的冻结定义。
   新增 `r3_gate_stops` 与 `gate_defer` 量化/证明最终接受集合一致，但没有把 span=1 改成
   gated；改成 gated 会使 A1≠A0 或改变 B suffix 策略，需要新的臂定义授权。
6. **A0 基线错误码**：A0 的 `submit_read` 在 execution-closed/retire 窗口仍可能返回
   `backend_error`；这是冻结的 baseSha 路径，未改。新入口的 `invalid_state` 映射只属于 A1/B。
7. **data+cancel-control 混合 partial submit**：未单独脚本化；control 消费由
   `cancel_ordering`/`control_stop` 覆盖。
8. **observer-armed 批量**：未构造 generation/borrow/observer 交错；`stale_generation` 与
   `borrow` 无 observer。
9. **默认 profile**：仅编译 + threadpool B 回落 smoke；未做 no-lib 完整语义套件。
10. **拒绝路径 stats**：入口在 precheck/execution-closed 拒绝时不增加 `AsyncStats`；
    backend 级拒绝由 `note_outcome` 计数；成功路径计数与 A0/A1 一致（stats smoke 通过）。
11. **清单**：未更新 `MANIFEST.sha256`；patch post-image 已由 reverse-check 验证，二进制
    SHA 见 §8.1。
12. **测量锁**：所有构建/运行均持 `MEASUREMENT.lock`；smoke/strace 的原始输出在
    `/tmp/sluice-r3-smoke/logs/` 与 `/tmp/r3fix2-*`，为临时证据，不是持久清单。
13. **#604**：仍 OPEN、无 `READY_FOR_595` handoff；产品门 BLOCKED。
14. **A0/A1/B 归因边界**：A0 逐条 `submit_read`；A1/B 共用入口但 A1 span=1；B−A1 仍包含
    组准备/入口调用次数/组门停止时机的差异（`gate_stops` 已单列）。正式计时前仍需按冻结
    协议拆分并预注册；本轮不产生任何 A0/A1/B 性能结论。
