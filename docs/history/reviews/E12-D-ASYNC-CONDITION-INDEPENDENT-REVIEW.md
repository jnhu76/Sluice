# E12-D AsyncCondition 独立 Review 报告

## A. Verdict

**PASS / CLOSED** — 无修正项需要。满足或超过每个指定的设计要求。

## B. 实现质量 / 正确性

实现语义正确，贯穿整个协议：

- **两阶段协议正确**：使用两个不同的 `WaitNode` 实例——调用者提供的 `condition_node` 和栈上的 `reacquire_node`。`Scheduler::condition_wait_prepare` / `condition_wait_prepare_until` 只在 Condition epoch 已解析后返回；wake/cancel/expiry resolver 将 Condition node 终态化并从 Condition queue 取消链接。随后 `AsyncCondition::wait*` 才创建 `reacquire_node` 并执行强制 Mutex reacquire。
- **原子释放和等待**：`condition_wait_prepare`（`scheduler.cpp:2628-2670`）在单个 `global_mtx_` CS 内注册条件节点、释放互斥锁并挂起纤程。通知窗口在注册完成之前保持关闭。
- **通知不接触互斥锁所有者**：`condition_notify_one` 调用 `wake_wait_one_locked(cond_waiters)`（`scheduler.cpp:2680`）—— 仅解析条件队列，不修改互斥锁状态。获胜者自行重获取。
- **抢占优先的终端路径正确**：`condition_wait_prepare_until`（`scheduler.cpp:2711-2722`）在未调度已过期时间时将预检查优先级提升到表达式 3：立即返回 `Expired`，`released_mutex=false`，无重获取。`wait_until` 调用者通过 `if (!released_mutex) return reason;`（`condition.hpp:297`）跳过重获取。
- **C8 失败路径安全**：`register_wait_locked` 失败设置 `released_mutex=false`（`scheduler.cpp:2738-2741`）；调用者保留所有权，无重获取，无定时器。
- **通知边界正确**：`condition_notify_all`（`scheduler.cpp:2704-2707`）在连续 `global_mtx_` CS 下循环 `wake_wait_one_locked`。新等待者（需要 `global_mtx_`）无法在此 CS 期间注册。测试 T11 确认了这一点。
- **`active_waits_` 始终平衡**：`ActiveWaitGuard` RAII 包装器在进入时递增，在所有退出路径上通过析构函数递减，包括内联过期路径和 C8 失败路径。

**一个发现（次要，非阻塞）**：防御性编程路径 `condition_wait_prepare:2660-2663`（手递后的 `is_terminal()`）如果执行会泄漏 `waiting_waitq_count_`，但在逻辑上不可达（`global_mtx_` 持有排除任何并发解析器 —— 通知、取消、过期泵）。这是代码库范围模式（与 `await_wait`、`mutex_lock` 相同），不是实际错误。

## C. 锁顺序 / 无死锁

锁顺序是**顺序的**（不是嵌套的）—— 两个队列锁从不同时持有：

| 操作 | 锁顺序 |
|---|---|
| `condition_wait_prepare` | `global_mtx_` → `cond_waiters.mtx()`（已释放）→ `mutex_waiters.mtx()`（在手递内部） |
| `condition_notify_one/all` | `global_mtx_` → `cond_waiters.mtx()` |
| `condition_cancel_wait` | `global_mtx_` → `cond_waiters.mtx()` |

顺序锁拓扑在规范中记录（文档 §6.3）；通过反证法（测试 T31 取消-错误的-条件）在代码中强制执行。`mutex_handoff_one_locked`（`scheduler.cpp:2082-2106`）在接收到正确的互斥锁队列后上锁该队列。

## D. 通知 / 取消 / 过期竞争

所有竞争场景均由 `WaitNode::resolve_` CAS 协议正确仲裁：

- **竞争类型**：单个获胜者 CAS 确定谁转换状态。失败者看到 `resolve_` 返回 false，不执行取消链接或递减计数。
- **通知 vs 取消**：`condition_notify_one:wake_one_locked` 对头部执行 CAS；如果头部已被取消者 CAS 解析，`resolve_` 返回 false，`wake_one_locked` 返回 null。取消者在同一个 CS 中已经取消链接并递减计数。
- **通知全 vs 取消**：每个 `wake_one_locked` 调用独立解析一个节点；竞争在逐节点基础上解决。
- **过期 vs 通知**：`condition_wait_prepare_until` 绑定 `{cond_node, cond_waiters}` 到 `timer_pool_`。通知/取消解析器在同一个 `global_mtx_` CS 中通过 `retire_timer_for_node_locked` 撤回定时器注册。过期泵通过 `try_claim_expiry` 安全地声明过期，在每次声明后检查 `global_mtx_`。
- **测试覆盖率**：T12a（取消胜过通知全）、T12b（通知全胜过取消）、T13a（过期胜过通知全）、T13b（通知全胜过过期）直接竞争。

## E. 通知全边界

`condition_notify_all` 在连续 `global_mtx_` CS 下循环 `wake_wait_one_locked`：

```cpp
while (wake_wait_one_locked(waiters)) {}  // global_mtx_ held
```

新的等待者通过 `condition_wait_prepare` 需要 `global_mtx_` 才能注册，因此在发送期间不能介入。测试 T11（`e12_cond_t11_notify_all_excludes_late_waiter`）明确测试了这一点。

## F. 测试覆盖率和质量

**32 个测试用例，可分为：**

| 类别 | 测试 | 强度 |
|---|---|---|
| 确定/边界条件 | T0–T6–T10–T2–T7–T16 | 确定性 |
| 竞争 | T12a/b, T13a/b, T4 | 确定性 + 50/50 竞争 |
| FIFO 顺序 | T8, T15a/b | 确定性 |
| 失败路径 | T1, T27, T30, T31 | 确定性 |
| 迁移/外部 | T24, T25 | 确定性 |
| 应力/重复 | T30(50), T31(500), T32(50), T33(200), T34(200) | 50–500 迭代 |
| 跨原语对等 | D3(3), D7(3), D8 | 确定性 |

**临界覆盖（来自规范第 5 节的所有场景已测试）**：
- ❌ 通知-前-等待（T2 检查）
- ❌ 已到期-内联（T1 检查）
- ❌ 通知全晚期（T11 检查）
- ❌ 通知全取消竞争（T12a/b 检查）
- ❌ 通知-全过期竞争（T13a/b 检查）
- ❌ 强制重获取 FIFO（T15a/b 检查）
- ❌ 取消-错误的-条件（T31 检查）
- ❌ 取消-分离的（T30 检查）

**缺失测试**：T28（销毁活动等待者期间析构）、T29（重获取阶段期间析构）—— 明确排除在测试目标之外，因为缺少死亡测试支持框架；仅在 dbg/ASan 构建中验证。形式模型 NEG-C7（InvDestructionPrecondition）在 TLC 下覆盖了这一点。

## G. TLA+ 形式验证

所有 13 个运行已完成，全部通过：

```
PASS  E12AsyncCondition [safety, Inv]    (80,755 states)
CEX   REACH OrdThenReq                   (89,790 states)
CEX   REACH ReqThenOrd                   (89,772 states)
CEX   NEG-C1 NonOwnerWait                (3 states)
CEX   NEG-C2 NotifyAnyNonRegistered      (3 states)
CEX   NEG-C3 ReturnOwnedNoGrant          (633 states)
CEX   NEG-C4 CancelReacquireEpoch        (3 states)
CEX   NEG-C5 NotifyAllNoDrain            (106 states)
CEX   NEG-C6 ReacquireNonFIFO            (118 states)
CEX   NEG-C7 DestroyWithActiveWaiters    (113 states)
CEX   NEG-C8 WaitReleaseBeforeRegister   (14 states)
CEX   NEG-C9 HandoffNonFIFO              (60,830 states)
CEX   NEG-C10 SeparateQueues             (61,022 states)
OK    WRONG-PROPERTY gate                (InvEligiblePreMutexQueue passes)
OK    COMPILE-PROBE gates x7             (all 7 sealed surfaces fail to compile)
```

**已验证的不变量**：InvConditionQueueWellFormed、InvFIFOGrant、InvDestructionPrecondition、InvNoLostNotifyWindow、InvEligiblePreMutexQueue、InvOrdinaryAndReacquireFIFO、InvTerminalAttemptFinality、InvReturnedOwnsMutex、InvConditionWaiterDoesNotOwnMutex、InvConditionResolvedFinality。

所有 10 个负面模型违反了**预期的命名属性**（不仅仅是任何通用的"违反不变量"消息）。错误属性门确认缺陷是特定于属性的。TLC 运行版本：TLC2 v2.19（2024-08-08，rev 5a47802）。

## H. 权威密封 / 编译探测

7 个独立的编译失败案例测试了无法访问的接口：

| 案例 | 目标 | 结果 |
|---|---|---|
| 1 | `wait_queue()` 访问器 | 封闭：编译失败 |
| 2 | `mutex()` 访问器 | 封闭：编译失败 |
| 3 | `waiting_count()` 访问器 | 封闭：编译失败 |
| 4 | `notify_n()` 访问器 | 封闭：编译失败 |
| 5 | `reacquire_node()` 访问器 | 封闭：编译失败 |
| 6 | `Scheduler` 私有接缝 | 封闭：编译失败 |
| 7 | `wake_wait_one` 绕过 | 封闭：编译失败 |

每个案例都独立编译（每个案例严格禁止单文件/单错误弱门）。

## I. 清理工具验证

| 配置 | 结果 | 重复 |
|---|---|---|
| Clang debug（构建 21.1.8） | ALL TESTS PASSED | 1x（32 次测试），100x（3200 次调用） |
| GCC debug（构建 15.2.0） | ALL TESTS PASSED | 1x |
| ASan（Clang） | ALL TESTS PASSED | 1x，50x |
| TSan（Clang） | ALL TESTS PASSED | 1x |
| 跨原语对等（Clang debug） | ALL TESTS PASSED | 1x |
| 权威探测（Clang） | 7/7 封闭（编译失败） | — |

## J. API 设计/文档

**公共 API 表面**：
```cpp
class AsyncCondition {
    WaitOutcome wait(CvAwareMutex& mutex);
    WaitOutcome wait_until(CvAwareMutex& mutex, Clock::time_point deadline);
    void notify_one();
    void notify_all();
    bool cancel(WaitNode& node);
};
```

**文档**（`docs/e12-condition.md`）：全面规范，包含 12 个协议点、15 个排序保证、锁顺序图、违反不变量列表、过期表达式优先级（P1–P3）、与 Pthreads 和 C++ 标准 cv 的差异。附有跨原语对等矩阵。

**封装**：没有公开 `WaitQueue`、`AsyncMutex`、`Scheduler` 或内部状态的引用。`cancel()` 需要 `contains_locked` 作为门，强制执行队列身份验证。

## K. 设计一致性

| 规范点 | 状态 |
|---|---|
| 1. 两阶段：条件条件 + 重获取条件 | ✓ 不同节点，条件在重获取前变终端 |
| 2. 原子释放-注册-挂起 | ✓ 全局互斥锁单个 CS |
| 3. 通知-一个/全部解析条件队列 | ✓ 不接触互斥锁所有者 |
| 4. wait()/wait_until() 强制重获取 | ✓ `mutex_.lock(reacquire_node)` 在返回之前 |
| 5. FIFO 条件队列 | ✓ WaitQueue 单获胜者 CAS，FIFO |
| 6. 基于截止时间的过期，带 timer_pool_ | ✓ condition_wait_prepare_until 绑定 {cond_node, cond_waiters} |
| 7. 队列身份关卡取消 | ✓ contains_locked + CAS，测试 T30/T31 |
| 8. active_waits_ RAII 守卫 | ✓ ActiveWaitGuard 在所有路径上递增/递减 |
| 9. active_waits_==0 析构断言 | ✓ acquire-load，配对的 acq_rel |
| 10. 已到期内联保留所有权 | ✓ 测试 T1，优先级 P3 |
| 11. C8 注册失败返回而不释放 | ✓ released_mutex=false，无重获取 |
| 12. 通知全 drain 循环在连续全局互斥锁下 | ✓ 逻辑上排除晚期等待者，测试 T11 |

## L. 最终授权

**PASS / CLOSED**

无需纠正措施。实现完整性通过所有质量门验证：确定性测试套件、压力测试套件、ASan/TSan、GCC 编译、形式模型验证（包含特定属性负面模型 + 错误属性门）以及权威密封编译探测。

E12-D 交付物可提升为 **CLOSED**。
