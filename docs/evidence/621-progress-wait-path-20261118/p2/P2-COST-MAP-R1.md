# P2-COST-MAP-R1 — #621 Progress/Wait 路径机器工作成本地图（修订版）

| 字段 | 值 |
|---|---|
| 日期 | 2026-11-18 |
| 战役 BASE | `5d94f233967c3aef993eb19ba9e3252c532b0951` |
| 性质 | **profile session 的成本归因修订**。不是新测量，不是 A/B，不是收益预测；不替代语义 oracle / mutant / 完整矩阵 |
| 本文件 | `docs/evidence/621-progress-wait-path-20261118/p2/P2-COST-MAP-R1.md` |
| 修订依据 | `p2/P2-REVIEW.md` 的 H1–H3、M1–M10；`p2/P2-COST-MAP.md`（R0，保留为历史版本，未修改） |
| 输入 | `p2/PROFILE-SUMMARY.md`；`p2/profile/analysis/{analysis.json,analysis.txt,analyze_p2.py}`；`p2/profile/P-*/raw/*`；`p1/AA-CALIBRATION.md`、`p1/AA-CALIBRATION-2.md`；`CAMPAIGN-LEDGER.md` §4/§5/§6 |
| 修订脚本 | `p2/profile/analysis/recompute_p2_r1.py`（sha256 `0b148621786ffb8d75de33e80f72be008deaad11bf481dc85e3565f1b64e1d11`）；原始 `analyze_p2.py` 保持 sha256 `b822ea3a705f885c4793ba55bcd2a6d8c37e2b3a7904803ab2754eead24ab105` 不变 |
| 复算命令 | `cd p2/profile/analysis && python3 -I recompute_p2_r1.py --json-out /tmp/p2-r1.json` |
| 未执行 | **没有重跑 perf / bench / test / A/A / A/B**；只读归档的 `script-cycles-sym.txt`、`stat-*.txt`、A/A 报告与源码 |

> **一句话结论（R1）**：形态级方向不变——测到的主体是**内核与 worker 的机器工作**，不是 Sluice 用户态协调代码。但 R0 的**候选级归因与上界不成立**：W1 的 `15.8%/11.5%` 是内核 poll 栈子集，不是上界；可复算的 poll 相关 inclusive 是 `__poll` **24.93%/15.83%**，且 `progress/wait` 仍是混合阶段容器。P-B 的 W3 家庭成本因 90.7% app 样本缺 bench 帧而 **NOT_OBSERVABLE**；P-B/P-E 的匹配 A/A 格是 **C6/C1**；M-62/M-61/M-65/M-67 只有阶段容器，没有候选上界；M-42 的 `lock addq` 成本约 19–20 cycles，P-D/P-E 不适用，仍 **NOT_POWERED**。本 profile 只证明 bench/reference 形态的 hotspot，**未过 ledger 的“真实消费者 profile”门**。

---

## R1 vs R0 差异摘要

| # | 项 | R0（`P2-COST-MAP.md`） | R1（本文件） | 依据/脚本入口 |
|---:|---|---|---|---|
| 1 | P-B poll 相关 inclusive | `__x64_sys_poll` 15.77% 当作“仅 poll 栈 ≤15.8%”的上界；`__poll` 24.93% 只作陈列 | `__poll` **6,129/24,585 = 24.93%** 是 poll 相关 inclusive；`__x64_sys_poll` 15.77% 是它的内核子集，**不是上界** | `recompute_p2_r1.py` → `forms[P-B].inclusive.__poll`；R0 §5.1/§5.7 B1 |
| 2 | P-E poll 相关 inclusive | `__x64_sys_poll` 11.46% 当作“≤11.5%” | `__poll` **5,500/34,736 = 15.83%**；`__x64_sys_poll` 11.46% 是子集 | 同上，`P-E.inclusive.__poll`；R0 §5.1/§5.7 B2 |
| 3 | libc-poll-only 样本 | 归入 `syscall (unattributed)` 或 `submit`，未识别 | 单列 **poll boundary (libc __poll only)**：P-B **2,251（9.16%）**、P-E **1,520（4.38%）**；`syscall unattributed` P-B 7.99→**4.01%**、P-E 13.02→**9.42%** | `forms[*].poll_boundary`；`stage_r1` |
| 4 | P-B 阶段组成 | `progress/wait` 25.53% 混合桶 | `progress/wait` **25.09%**（6,168）+ 独立 poll boundary 9.16%；composition：app-poll 3,572（57.91% of stage）、worker-eventfd 2,057（33.35%）、progress-pass 159+297、eventfd 83 | `forms[P-B].stage_r1`、`progress_wait_composition_r1` |
| 5 | P-E 阶段组成 | `progress/wait` 25.12% 混合桶 | `progress/wait` **24.36%**（8,461）+ poll boundary 4.38%；composition：poll+progress-pass 3,972（46.94%）、progress-pass 3,537（41.80%）、eventfd 952（11.25%） | `forms[P-E].stage_r1`、`progress_wait_composition_r1` |
| 6 | P-C `poll_progress` | 24.15%（2,203+1,775 相加） | union **2,205/16,472 = 13.386%**（both 1,773） | `forms[P-C].unions.poll_progress`；R0 §4.3 |
| 7 | P-B `eventfd_signal` | 16.24%（2,132+1,860 相加） | union **2,132/24,585 = 8.672%**（both 1,860） | `forms[P-B].unions.eventfd_signal`；R0 §5.1 |
| 8 | P-E `wait_one` | 61.94%（10,759+10,755 相加） | union **10,759/34,736 = 30.974%**（both 10,755） | `forms[P-E].unions.wait_one`；R0 §5.1 |
| 9 | M-62 `publication + claim_execution` | P-D 4.74%（4.13+0.61） | **4.13%**（publication 627/15,182）；`claim_execution` leaf 93/93 已在 publication 内，不另加 | `forms[P-D].publication`、`claim_execution`；R0 §5.2 |
| 10 | M-67/M-61/M-65 | 把阶段份额写成候选上界 | 改标 **阶段容器 / NOT_OBSERVABLE**；不给候选上界 | `progress/wait`、`publication`、`terminal/release`；R0 §5.2/§5.7 |
| 11 | P-B/P-E A/A 格 | 称“没有自己的 A/A 格”，用 C5b/C3b/C7 代理 | P-B→**C6**（同 binary/case/depth），P-E→**C1**（同 binary/argv）；不再用 C5b/C3b/C7 作 P-B 代理；C6 wall 标 **ORDER_SENSITIVE** | `p1/AA-CALIBRATION.md:24,29,46-52,156,386-402`；`recompute_p2_r1.py → aa` |
| 12 | M-42 成本 | 3–5 cycles/xadd，≤0.04% cycles；P-D/P-E 也给数字 | objdump 证实 **`lock addq $0x1,0x120(%rdi)`**；按 19–20 cycles 重算：P-A **0.126–0.133%**、P-B **0.066–0.070%**、P-C **0.093–0.098%**，全局 **≤0.14%**；**删除 P-D/P-E 数字**（TP 无 uring submit） | `m42`；objdump 输出；`stat-tp-uring.txt` |
| 13 | P-B W3 0.24% / 0.49% / wait_one 2.85% | 解释为“严重内联”，当作族/用户成本 | **NOT_OBSERVABLE**：app 12,246 中 11,104（90.7%）无 bench DSO 帧；`wait_one` 只可见 700（2.85%）；5,664/6,129 个 `__poll` 无 `wait_one`；873 个 4 帧 poll 栈 | `forms[P-B].app_frames`、`wait_path`；R0 §4.6/§5.2 |
| 14 | W1 准入 | “PARTIAL PASS：达到 profile 准入” | **未过 ledger 的 profile 门**：只有 bench/reference 形态；`sluice-copy` 是参考应用，不是 G0 workload | `CAMPAIGN-LEDGER.md:257,311-315,323-326`；R0 §7 |
| 15 | 复算可复现性 | 一次性内联脚本，未入库 | 修订脚本归档到 `p2/profile/analysis/`；每个关键数字给脚本字段/计算式 | `recompute_p2_r1.py`；本文件 §A.2–A.3 |

---

## 0. R1 修正回执（逐项）

> 本节逐项回应 `p2/P2-REVIEW.md`。每条给出 R0 问题、R1 处理、证据/入口与剩余限制。所有重算均来自归档 `script-cycles-sym.txt`，没有重跑 perf/bench/test。

### 0.1 HIGH

| ID | R0 问题 | R1 处理 | 证据/入口 | 剩余限制 |
|---|---|---|---|---|
| **H1** | `__x64_sys_poll` 15.77/11.46% 被当成 poll 上界；裸 libc `__poll` 未识别；P-B 2,176 个（另 75 个含 eventfd，共 2,251）、P-E 1,520 个 libc-poll-only 样本被 R0 分散在 `syscall unattributed`（978/1,251）、`submit`（518）等桶 | 修正 `syscall_owner()`/`first_stage()`（以 `_r1` 形式归档）：精确匹配裸 `__poll`；`attribute_r1_dedicated()` 把 libc-poll-only 单列为 **poll boundary**。P-B `__poll` 24.93%、P-E 15.83%；poll boundary P-B 2,251（9.16%）、P-E 1,520（4.38%）；`syscall unattributed` P-B 4.01%、P-E 9.42%。不再把 15.8/11.5 当上界 | `recompute_p2_r1.py` `LIBC_POLL_RE`、`syscall_owner_r1()`、`first_stage_r1()`、`attribute_r1_dedicated()`；`forms[*].inclusive.__poll`、`poll_boundary`、`stage_r1` | `__poll` 仍是 inclusive 栈份额，不是可删量；poll boundary 由“无内核 poll 帧”定义，不能证明 CPU 全部耗在 libc wrapper |
| **H2** | P-B 90.7% app 样本缺 bench 帧；0.24%/0.49%/2.85% 被当“小”或“内联”，未识别用户栈截断 | P-B W3 家庭、product leaf、`wait_one` 份额全部改标 **NOT_OBSERVABLE**；记录 app/no-bench 帧、`__poll` 无 `wait_one`、4 帧 poll 栈计数；不再用“严重内联”解释 | `forms[P-B].app_frames`（12,246 / 11,104 / 1,142）、`wait_path`（700、5,664、873）；`analysis.json` 的 `single_frame` 仅作截断辅助 | 用户栈深度由 `sample_stack_user=8192`/`exclude_callchain_user=1` 限制；无 DWARF，不能恢复内联行号 |
| **H3** | 称 P-B/P-E 没有自己的 A/A 格；用 C5b/C3b/C7 代理 | 建立匹配表：P-B→C6（`W02-uring-4k-d1`，binary `03dbef09…`）；P-E→C1（`sluice-copy 4k/d4/w1`，binary `db972b19…`）。C6 cycles/CPU 带；C6 wall **ORDER_SENSITIVE**。不用 C5b/C3b/C7 作 P-B 代理 | `p1/AA-CALIBRATION.md:24,29,46-52,156,386-402`；`p2/PROFILE-SUMMARY.md:35-39`；`recompute_p2_r1.py → aa.matching` | P2 没有重跑 A/A；带是 P1/P2 旧带，且 C6 的 wall 位置效应未重测 |

### 0.2 MEDIUM

| ID | R0 问题 | R1 处理 | 证据/入口 | 剩余限制 |
|---|---|---|---|---|
| **M1** | 三个 inclusive 是嵌套符号求和 | P-C `poll_progress` union **13.386%**（2,205；both 1,773）；P-B `eventfd_signal` union **8.672%**（2,132；both 1,860）；P-E `wait_one` union **30.974%**（10,759；both 10,755） | `forms[*].unions`；公式 `union/N`，`both` 为同时出现的样本 | union 仍是 inclusive，不是 self；不能相加 |
| **M2** | M-62 把 `claim_execution` 叶份额加到 publication 阶段 | M-62 上界改为 publication 本身：P-A **1.46%**、P-B **0.07%**、P-C **2.82%**、P-D **4.13%**、P-E **3.30%**；claim leaf 10/0/12/93/139 全部已归 publication | `forms[*].publication`、`claim_execution` | 置信仍 LOW；publication 阶段包含 publish/deliver 的内核/用户工作 |
| **M3** | M-67/M-61/M-65 把阶段份额当候选上界 | 全部改标 **阶段容器 / NOT_OBSERVABLE**；不给候选上界。M-67 的 `progress/wait` 还包含 worker-eventfd 与 progress-pass；M-61/M-65 包含 publish/deliver 的其它工作 | `progress_wait_composition_r1`、`publication`、`terminal/release`；R0 §5.2 | sticky 双标志、begin/complete 两锁、release_control 第三锁在 release 符号中没有独立可采样函数；行级 NOT_OBSERVABLE |
| **M4** | `fetch_add` 按 3–5 cycles；P-D/P-E 也给了 M-42 数字 | objdump 证实 `lock addq $0x1,0x120(%rdi)`；按 19–20 cycles 重算 P-A/B/C；删除 P-D/P-E 的 M-42 数字（`io_uring_submit_req` = 1 启动残差/0）；保留 **NOT_POWERED** | `m42`；`objdump -d --start-address=0x424190 --stop-address=0x424230 /tmp/p2-621/symtwin/bench_w02_pipeline.sym`；`P-*/raw/stat-tp-uring.txt` | 19–20 cycles 是 review 引用的 Haswell L1 命中成本，**本会话没有重新测锁延迟**；P-D/P-E 不是 uring submit 形态 |
| **M5** | PMU/record 分母不一致；无 PEBS/skid；小函数份额标 HIGH | 用户/内核拆分、product leaf、M-53 cycles/op 置信降为 **MEDIUM/LOW**；记录 `perf record -F 2999`（freq 模式）、无 `precise_ip`、8 KiB 用户栈；`Lost=0` 只说明 ring buffer 未丢，不等于 callchain 完整 | `P-A/raw/header-record-cycles.txt`；R0 §2.1；`forms[*].app_frames` | 没有未插桩基线份额；没有 skid/PEBS 分析；record 与 PMU 的 kernel 份额差 P-B 10.8pp 未解释 |
| **M6** | `syscall unattributed` 被整体标“不可分”；poll 返回 task_work 被归 submit | 从 `syscall unattributed` 拆出可识别 libc-poll：P-B 978、P-E 1,251；剩余 P-B 986（4.01%）、P-E 3,273（9.42%）。R0 的 submit 桶曾含 518 个 libc-poll-only 样本（poll 返回路径的 task_work）；R1 已把它们移入 poll boundary，但这说明 R0 的 submit 标签曾是启发式标签，不是因果 owner | `poll_boundary.from_stage`；`recompute_p2_r1.py` `stage_r0` vs `stage_r1`；R0 §2.3 | 剩余 unattributed 仍不能强分给 pread/pwrite/futex；submit/reap 阶段名不是单一操作成本 |
| **M7** | 未证明 1.59× record / 2.0–3.7× tracepoint 下份额稳定 | 明确 R1 份额只作**方向性归因**；具体百分比不是 clean-run 份额，不能乘 PMU cycles/op 当候选成本；未做低扰动重复 profile | `P-A/raw/header-record-cycles.txt`（1,427.239ms vs 899.3ms）；`P-A/raw/stat-tp-uring.txt`（3,065.6ms vs 0.899s）；R0 §0.1.8/§8.1.3 | 份额稳定性 NOT_PROVEN；需独立低扰动 profile 或重复 record |
| **M8** | taskset 2,3 与 task-clock/进程 wall ≈2.9–3.0 CPU 的不一致；nmi_watchdog=1；背景负载只有 cell 级 | 列为未解释的环境/可比性问题；不写成“worker 跑在 taskset 之外”；nmi_watchdog 会计入 cycles:k/IRQ 桶；profile 期间无逐轮 loadavg/ps | `P-*/raw/stat-pmu-r3.txt`、`stat-pmu-r3.stdout`（util 181–190%）；`p2/profile/env/snapshot-before.txt`；`p1/AA-CALIBRATION-2.md:276-285` | 需要每 worker 的 `Cpus_allowed_list` 或 perf script CPU 字段；当前无法定标 task-clock 口径 |
| **M9** | 复算脚本未入库；若干小数差异 | 归档 `recompute_p2_r1.py`；每个关键数字给字段/公式；记录 P-C product leaf 前缀 883 + `driver_thread` 14 = 897 的口径差异；P-B iou-wrk `rep_movs` 等小差异保留为 MEDIUM | `recompute_p2_r1.py`；`forms[*].product_leaf_visible`；`p2/profile/analysis/SHA256SUMS` | `SHA256SUMS` 第 4 行自哈希是 stale（见 §0.3-D）；小差异不影响方向 |
| **M10** | W1 “PARTIAL PASS” 易被读成已过 ledger profile 门 | W1 准入表明确：本 profile 只证明 **bench/reference 形态 hotspot**；`sluice-copy` 是参考应用，不是 G0 workload；#604 G0 尚未准入；**未过 ledger 的“真实消费者 profile”门** | `CAMPAIGN-LEDGER.md:257,311-315,323-326`；R1 §7 | 仍需真实消费者 profile，或 owner 明确接受 bench 代理 |

### 0.3 输入与检查口径

**(A) `PROFILE-SUMMARY.md` §4 漏列 `startup/thread create` 行。** R1 继续以 `analysis.json` 的 `stage_self` 为准（R0 §0.2(A) 的更正机制成立）：P-C 4.61%、P-D 13.67%、P-E 8.87% 是 worker/driver syscall trampoline，不是线程创建成本。R1 的 stage 表保留该行并改名 `worker-thread syscall trampoline`。

**(B) `PROFILE-SUMMARY.md` §3 的 `single-frame` 数不可复算。** R1 只用 `analysis.json` 的 `single_frame`（46/1,207/173/139/362）作截断辅助，不引用 summary 的 817/2,751/2,440/4,138/8,753。

**(C) `task-clock` 两遍 stat 不同。** P-A `stat-pmu-r3` 2,593.96ms、`stat-mem-r3` 2,690.60ms；R1 沿用 `analysis.json` 的 `stat-mem-r3` 值，并保留 R0 的差异披露。

**(D) `SHA256SUMS` 自哈希 stale。** 本会话执行 `sha256sum -c SHA256SUMS`：`analysis.json`、`analysis.txt`、`analyze_p2.py` 三项 **OK**；第 4 行 `SHA256SUMS` 自身 **FAILED**（文件内容哈希 `dbe63d89…`，记录值 `87687dea…`）。这是既有的自引用/生成顺序问题，不是本轮修改造成；R1 不修改该文件，也不把它当作三项证据的校验失败。

**(E) 原始 `analyze_p2.py` 未改。** 为保持 `analysis.json` 与 `SHA256SUMS` 的绑定，R1 不改写已归档的 `analyze_p2.py`；修订逻辑以 `recompute_p2_r1.py` 的 `syscall_owner_r1()`/`first_stage_r1()`/`attribute_r1_dedicated()` 实现并归档；它们是对 `analyze_p2.py:257-274`（`syscall_owner`）、`:242-248`（`first_stage`）、`:277-288`（`attribute`）的 drop-in 替换。它是**证据生成器**，只读原始文件，不重跑 perf/bench/test。

---

## 1. 读数口径

1. **份额 ≠ 可删除 cycles**。perf cycles 采样定位“CPU 在哪里执行”，不是“删掉哪段代码省多少”。W1/W3/M-53/M-42 的数字互相重叠，**不能相加**。
2. **阶段 self 是“最深阶段 owner”份额**：每条样本只归一次；它包含该阶段调用栈内的内核/调度/同步成本，不是该函数的指令级 self。
3. **inclusive 份额互相重叠**：任意调用帧出现即计数；同一栈内的嵌套符号必须用 **union**，不能把两个 inclusive 相加。R1 对 `poll_progress`、`eventfd_signal`、`wait_one` 都改用 union。
4. **阶段容器不是候选上界**：`progress/wait`、`publication`、`terminal/release` 是混合容器，含内核/worker/必要同步；R1 只把它们用于“这部分机器工作在哪里”，不用于“候选可删多少”。
5. **非阶段桶**（payload/FS、io-wq punt、shared sync、scheduler/IRQ、kernel syscall/FS overhead、syscall unattributed、bench harness、other）不能整体视为可删除。
6. **行级归因 NOT_OBSERVABLE**：release 二进制 stripped、无 DWARF；内核无 debuginfo；只有符号级。
7. **PMU 与 record DSO 不是同一分母**：PMU 用于总量/user-kernel 总账；record 只用于相对热点。P-B 的 PMU `cycles:k` 81.7% 与 record kernel 92.5% 的差异未解释，不能互相替代。
8. **off-CPU 是 `sched_switch` 事件数，不是阻塞时长**；窗口只有一个 measured round。
9. **没有 clean A/B**：本轮没有候选 build、mutant、A/A、A/B；任何“收益已成立”都不成立。
10. **本 profile 只覆盖 bench/reference 形态**：P-A/B/C/D 是 benchmark，P-E 是 `sluice-copy` 参考应用；不是 G0 真实消费者。

---

## 2. 执行 DAG

DAG 由 `PROFILE-SUMMARY.md` §9 的源码坐标、`CAMPAIGN-LEDGER.md` §2 的锁/线程所有权，以及本会话读取的 `src/async/async_io_context.cpp:573-705`、`include/sluice/async/detail/progress_source.hpp:190-232`、`src/async/threadpool_backend.cpp:386-400`、`apps/sluice-copy/copy_task.cpp:305-351`、`src/async/uring_transport.cpp:218-235,540-557`、`src/async/uring_backend.cpp:171` 汇总。方括号内是该边的主要成本 owner。

### 2.1 W-02 io_uring（P-A d16 / P-B d1）

```text
app/bench thread
  submit_read
    -> RequestCore::reserve -> RequestCore::accept                    [user; admission]
    -> UringAsyncBackend::dispatch_after_accept
       -> dispatch_one_locked -> get_sqe / prep_sqe -> router/ledger  [user+kernel; dispatch]
       -> submit_transport_locked -> io_uring_submit
          -> sys_enter_io_uring_enter -> io_submit_sqes               [kernel; submit]
    -> kernel io_uring: io_uring_submit_req
       -> io_uring_queue_async_work (tmpfs: 1/op)
          -> iou-wrk: io_worker_handle_work -> io_read
             -> shmem_file_read_iter -> rep_movs_alternative          [kernel worker; payload/FS]
          -> io_cq_unlock_post -> CQE                                 [kernel; reap/CQE]
    -> wait path: AsyncIoContext::wait_one
       -> run_progress_pass_ -> poll_progress
          -> ProgressSource::wait_if_unchanged -> poll(notification_fd)
             -> libc __poll -> __x64_sys_poll / do_sys_poll           [kernel+libc; poll boundary]
          -> eventfd signal / read / drain                            [kernel; progress/wait]
    -> reap_cqes -> handle_one_cqe
       -> find_live_router_cookie_ (O(R) reverse scan)                [user; M-53]
       -> finalize_operation_terminal_ -> offer_terminal
          -> release_execution                                        [user; terminal/release]
       -> publication_pending -> publish_one
          -> begin_publication -> complete_publication                [user; publication]
          -> deliver_event -> observer take/consume                   [user; observation]
```

R1 的 `poll boundary` 是“只有 libc `__poll` 帧、没有内核 poll 帧”的样本；它们通常是 poll 返回路径的用户/内核边界，不能被 R0 的 `syscall unattributed` 吞掉。

### 2.2 W-02 ThreadPool（P-D d16 w4）与 sluice-copy（P-E d4 w1）

```text
app thread
  submit_read / submit_write
    -> RequestCore::reserve/accept                                    [user; admission]
    -> ThreadPoolBackend::dispatch_ -> work_mtx_ push                 [user; dispatch]
    -> poll_progress / run_progress_pass_                             [caller; progress/wait]
  worker thread (worker_loop)
    -> work_cv_.wait(work_mtx_)                                       [futex; shared sync]
    -> pread64 / pwrite64                                             [kernel; backend exec]
    -> rep_movs_alternative / shmem / page cache                      [kernel; payload/FS]
    -> completion -> caller publish_one -> deliver_event              [user; publication/delivery]

sluice-copy main
  run_pipelined_copy_with_context
    -> RequestScope::wait_for -> wait_one -> poll(notification_fd)    [kernel; progress/wait]
       -> worker_loop pread -> write/pwrite -> completion
```

### 2.3 W-03 split-owner（P-C）

```text
driver_thread -> epoll_wait(progress fd)                             [kernel; progress/wait]
  -> poll_progress -> reap_cqes -> publish_one -> deliver_event       [driver; reap+publication]
submitter -> submit_read -> io_uring_enter -> ...                     [kernel; submit]
  -> condvar futex wait (completion)                                  [kernel+libc; shared sync]
  -> observation / take                                               [user; observation]
```

### 2.4 DAG 边成本归属（摘要）

| 边 | 主要 owner | 证据 |
|---|---|---|
| `submit_transport_locked -> io_uring_submit` | kernel | submit 阶段 97.6/98.7/98.2% 为 kernel（P-A/P-B/P-C）；`io_uring_enter` 1/op |
| `io_uring -> queue_async_work -> iou-wrk` | kernel worker | `io_uring_queue_async_work` 1/op（P-A/P-B/P-C）；io-wq punt 阶段 10.23/6.65/6.02% |
| `io_read -> shmem/rep_movs` | kernel | `rep_movs_alternative` 11.94/7.84/6.78/9.04/3.73%；`shmem_file_read_iter` 2.83/1.63/1.51/1.47 |
| `wait_one -> wait_if_unchanged -> poll` | kernel+libc | progress/wait 阶段 kernel 占 66.9/97.9/87.7/46.5/64.1%；P-B `__poll` inclusive 24.93% |
| `publish_one -> begin/complete_publication -> deliver` | user/libc | publication 阶段 user 占 84/94/86/90/98%；P-D libc 62.5% |
| `worker_loop -> pread/pwrite` | TP worker | P-D/P-E `worker_loop` off-CPU futex 68.6%/48.6%；worker pread/pwrite trampoline 6.3/6.7pp |
| `driver_thread -> epoll_wait` | driver thread | P-C off-CPU epoll wait 4,645（14.7%） |
| IRQ entry / reschedule IPI | kernel IRQ | 仅 `asm_sysvec_reschedule_ipi`、`__irqentry_text_end` 等；无设备 IRQ 身份 |

---

## 3. 机器工作成本台账

### 3.1 PMU 总账（`perf stat -r 3`，无缩放）

原始：`P-*/raw/stat-pmu-r3.txt`、`stat-mem-r3.txt`。全部 198 条 stat 行 `percent_enabled=100.00`，无 `<not counted>` / `<not supported>` / `scaled`。分母：bench 540,000 ops；P-E 65,536 chunks（131,076 requests）。

| 形态 | cycles:u/op | cycles:k/op | cycles 合计/op | instr:u/op | instr:k/op | instr 合计/op | task-clock µs/op | ctx-sw/op | cache-miss/op | branch-miss/op |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| P-A d16 uring | 2,666.7 | 12,423.3 | **15,090.0** | 3,871.5 | 6,781.4 | 10,652.9 | 4.983 | 0.339 | 19.154 | 25.882 |
| P-B d1 uring | 5,248.0 | 23,455.2 | **28,703.2** | 6,676.6 | 17,565.1 | 24,241.7 | 11.793 | 2.000 | 23.320 | 43.677 |
| P-C W-03 d16 | 3,689.3 | 16,635.6 | **20,324.9** | 3,217.4 | 9,474.7 | 12,692.1 | 6.864 | 0.606 | 20.299 | 35.008 |
| P-D TP d16 w4 | 5,728.1 | 14,658.0 | **20,386.1** | 4,032.7 | 8,134.5 | 12,167.2 | 6.356 | 0.668 | 19.953 | 27.613 |
| P-E copy /chunk | 10,875.1 | 33,606.4 | **44,481.5** | 8,675.8 | 22,233.2 | 30,908.9 | 16.183 | 2.040 | 31.177 | 74.893 |
| P-E copy /request | 5,437.6 | 16,803.2 | **22,240.8** | 4,337.9 | 11,116.6 | 15,454.5 | 8.092 | 1.020 | 15.589 | 37.447 |

**用户/内核总账（PMU cycles）**：P-A 17.7% / 82.3%；P-B 18.3% / 81.7%；P-C 18.2% / 81.8%；P-D 28.1% / 71.9%；P-E 24.4% / 75.6%。**内核侧始终是主体。** 置信：总量 HIGH；user/kernel 拆分 MEDIUM（M5：freq 模式、无 PEBS、record/PMU 分母不一致）。

**PMU 总账 vs record DSO 拆分**（不同分母，不可互相替代）：

| 形态 | PMU u/k | record kernel | record lib | record bench | 备注 |
|---|---:|---:|---:|---:|---|
| P-A | 17.7 / 82.3 | 84.74% | 5.58% | 9.68% | record 内核略高于 PMU |
| P-B | 18.3 / 81.7 | 92.51% | 6.93% | 0.57% | io-wq 继承/采样偏差最大 |
| P-C | 18.2 / 81.8 | 82.84% | 10.57% | 6.59% | 与 PMU 接近 |
| P-D | 28.1 / 71.9 | 74.25% | 18.99% | 6.76% | TP worker 同 comm，不能按线程拆 |
| P-E | 24.4 / 75.6 | 73.60% | 19.19% | 7.21% | copy 主线程 + worker |

### 3.2 线程/worker 归属与用户栈截断

`record` 的 comm 过滤见 `analyze_p2.py:359-364`（bench 形态含 `bench_w02*` / `bench_w03*` / `iou-wrk*`；copy 形态含 `sluice-copy*`）。

| 形态 | 过滤后样本 | app 线程样本 | `iou-wrk` 样本 | app 无 bench 帧 | app 有 bench 帧 | TP worker 证据 |
|---|---:|---:|---:|---:|---:|---|
| P-A d16 uring | 8,369 | 4,272（51.05%） | 4,097（48.95%） | 53（1.24%） | 4,219 | — |
| P-B d1 uring | 24,585 | 12,246（49.81%） | 12,339（50.19%） | **11,104（90.67%）** | 1,142 | — |
| P-C W-03 | 16,472 | 11,010（66.84%） | 5,462（33.16%） | 137（1.24%） | 10,873 | driver 是 app-owned 线程 |
| P-D TP w4 | 15,182 | 15,182（100%，同一 comm） | — | 414（2.73%） | 14,768 | `worker_loop` off-CPU futex 22,135（68.6%）；leaf 2.24% |
| P-E copy | 34,736 | 34,736（100%，同一 comm） | — | 1,266（3.64%） | 33,470 | `worker_loop` off-CPU futex 80,918（48.6%）；leaf 1.17% |

- P-A/P-B/P-C 的 `iou-wrk` 份额是 **worker 侧采样的直接证据**；其 leaf 顶部是 `rep_movs_alternative`（P-A 24.09%、P-B 15.20%、P-C 20.10%；R1 复算 P-B 15.28%、P-C 20.18%，差异为分母/过滤口径，MEDIUM）。
- P-D/P-E 的 TP worker 与主线程同 comm，**不能按 comm 拆 cycles**；只能用 `worker_loop` 的 inclusive/leaf、off-CPU 阻塞点和重标定的 trampoline 样本作证据。
- **P-B 的 app 样本 90.67% 没有 bench DSO 帧**：`perf record` 头 `sample_stack_user = 8192`、`exclude_callchain_user = 1`；`parse_script()` 在 libc 帧处停止后不再补 bench 帧。这是 H2 的根因，也是 P-B W3/product leaf 不可用的原因。
- off-CPU：P-A 99.9% 是 `iou-wrk` idle；P-B 是 app `poll` 50.0% + `iou-wrk` idle 50.0%；P-C 是 `iou-wrk` idle 44.8% + app futex 40.5% + app epoll 14.7%；P-D 是 futex 74.9% + mutex/condvar 19.2% + scheduler 3.4% + poll 2.5%；P-E 是 poll 50.2% + futex 49.8%（`analysis.json.offcpu.primitive`）。off-CPU 是事件数，不是时长。

### 3.3 阶段 owner self 台账（R1；每条样本只归一次，行和 = 100%）

原始：`recompute_p2_r1.py → forms[*].stage_r1`（count/N）。R0 的 `progress/wait` 25.53/25.12% 拆成 `progress/wait` + `poll boundary`；`syscall unattributed` 相应下降。

| 阶段 / 桶 | P-A d16 | P-B d1 | P-C W-03 | P-D TP w4 | P-E copy |
|---|---:|---:|---:|---:|---:|
| admission（reserve/accept/submit_read） | 1.97% | 0.09% | 8.91% | 12.76% | 7.16% |
| dispatch（dispatch/queue/SQE prep） | 5.84% | 1.67% | 6.02% | 0.74% | 1.00% |
| submit（io_uring_enter/io_submit_sqes） | 21.35% | 8.80% | 12.49% | — | — |
| reap/CQE（reap/handle/router/seen） | 5.35% | 2.85% | 3.61% | — | — |
| terminal/release（offer/release/finalize） | 0.97% | 0.03% | 2.31% | 3.49% | 0.88% |
| publication（begin/complete/publish/deliver） | 1.46% | 0.07% | 2.82% | 4.13% | 3.30% |
| progress/wait（wait/poll/eventfd/drain，**不含 poll boundary**） | 5.27% | **25.09%** | 16.29% | 3.51% | **24.36%** |
| **poll boundary（libc `__poll` only）** | — | **9.16%** | — | 0.06% | **4.38%** |
| observation（take/consume/lookup） | 1.60% | 0.05% | 1.77% | 2.05% | 1.74% |
| payload/FS（非阶段：rep_movs/filemap/shmem 等） | 19.87% | 13.28% | 11.45% | 13.85% | 10.44% |
| io-wq punt（非阶段：io_wq/io_worker/task_work） | 10.23% | 6.65% | 6.02% | — | — |
| backend exec (TP)（非阶段：worker_loop/pread/pwrite） | — | — | — | 2.38% | 1.35% |
| worker-thread syscall trampoline（analysis 误标 `startup/thread create`） | — | — | 4.61% | 13.67% | 8.87% |
| shared sync（非阶段：mutex/spinlock，无阶段 owner） | 5.10% | 7.07% | 5.43% | 13.27% | 7.86% |
| scheduler/IRQ（非阶段：EEVDF/PSI/IPI/futex wake） | 8.69% | 11.52% | 7.68% | 12.89% | 6.01% |
| kernel syscall/FS overhead（非阶段：fput/fdget/kmalloc 等） | 0.96% | 1.94% | 0.84% | 2.04% | 5.26% |
| syscall unattributed（入口/出口 trampoline，剩余） | 3.43% | 4.01% | 4.10% | 7.34% | 9.42% |
| bench harness（非阶段：run_*_phase/verify/memcmp） | 3.76% | 1.32% | 2.06% | 1.99% | — |
| other（未归类） | 4.15% | 6.40% | 3.59% | 5.84% | 7.97% |

**行和**：P-A 100.00%、P-B 100.00%、P-C 100.00%、P-D 100.00%、P-E 100.00%。计数：P-B `progress/wait` 6,168、`poll boundary` 2,251；P-E `progress/wait` 8,461、`poll boundary` 1,520。

**R0 → R1 的主要变化**（同一分区，不是新测量）：

| 阶段 | P-B R0 → R1 | P-E R0 → R1 | 原因 |
|---|---:|---:|---|
| `progress/wait` | 25.53% (6,276) → **25.09%** (6,168) | 25.12% (8,724) → **24.36%** (8,461) | 108/263 个 libc-poll-only 样本移入 poll boundary |
| `poll boundary` | — → **9.16%** (2,251) | — → **4.38%** (1,520) | 裸 libc `__poll` 识别（H1） |
| `syscall (unattributed)` | 7.99% (1,964) → **4.01%** (986) | 13.02% (4,524) → **9.42%** (3,273) | 978/1,251 个从 unattributed 拆出 |
| `submit` | 10.91% (2,681) → **8.80%** (2,163) | — | 518 个 libc-poll-only 样本移入 poll boundary |
| `io-wq punt` | 7.65% (1,881) → **6.65%** (1,634) | — | 247 个移入 poll boundary |
| `scheduler/IRQ` | 12.11% (2,978) → **11.52%** (2,832) | 6.01% | 146 个移入 poll boundary |
| `shared sync` | 7.43% (1,826) → **7.07%** (1,738) | 7.86% | 88 个移入 poll boundary |
| `other` | 7.04% (1,730) → **6.40%** (1,574) | 7.99% (2,775) → **7.97%** (2,769) | 156/6 个移入 poll boundary |
| `kernel syscall/FS overhead` | 1.95% (480) → **1.94%** (478) | 5.26% | 2 个移入 poll boundary |
| `reap/CQE` | 2.88% (709) → **2.85%** (701) | — | 8 个移入 poll boundary |

**另一种允许实现（对照）**：如果只把裸 `__poll` 归入 `progress/wait`、不单列 poll boundary，P-B `progress/wait` = 7,646（31.10%）、P-E = 9,981（28.73%）。R1 选择单列，因为这样可以直接显示 978/1,251 个原 `syscall unattributed` 样本，且不把 io-wq/submit 样本混入 `progress/wait`。脚本的 `stage_minfix`、`progress_wait_composition_minfix` 保留该对照变体；两种实现的 `__poll` inclusive 与 union 相同。

**该表的性质**：阶段名是启发式 owner，不是因果 owner。尤其 R0 的 `submit` 桶曾含 518 个“poll 返回路径执行 io_uring task_work”的样本（M6）；R1 已把它们移入 `poll boundary`，但 `submit` 阶段的 `io_uring_enter` 标签仍不是单一操作成本。`reap/CQE`、`publication` 等也不是单操作精确成本。

### 3.4 `progress/wait` 的组成（R1，disjoint）

定义：先看是否有 poll（内核 poll 帧或裸 libc `__poll`），再看是否有 progress-pass 标记（`poll_progress`/`run_progress_pass_`/`reap_cqes`/`handle_one_cqe`/`publish_one`/`deliver_event`/`claim_execution`/`wait_one`/`wait_for`）；`worker-eventfd` 只给 `iou-wrk` comm 且无 poll/progress-pass 标记的 eventfd 样本。

| 形态 | app-poll only | poll + progress-pass | progress-pass only | worker-eventfd | eventfd only | 合计 |
|---|---:|---:|---:|---:|---:|---:|
| P-A | — | — | 194（43.99%） | 167（37.87%） | 80（18.14%） | 441 |
| P-B | **3,572（57.91%）** | 297（4.82%） | 159（2.58%） | **2,057（33.35%）** | 83（1.35%） | **6,168** |
| P-C | — | — | 799（29.78%） | 504（18.78%） | 130（4.85%） | 2,683 |
| P-D | — | 50（9.38%） | 427（80.11%） | — | 56（10.51%） | 533 |
| P-E | — | **3,972（46.94%）** | **3,537（41.80%）** | — | 952（11.25%） | **8,461** |

（括号内是该形态 `progress/wait` 阶段内的占比；计数来自 `recompute_p2_r1.py → progress_wait_composition_r1`。）

**读法**：P-B 的 `progress/wait` 不是“app 等待”单桶：app-poll only 3,572、worker-eventfd 2,057、poll+progress-pass 297、progress-pass only 159、eventfd 83。P-E 没有 `iou-wrk` comm，eventfd 952 不能按 comm 分给 worker；它可能是 copy 主线程或 worker 的 eventfd 操作。**这些都不是候选上界。**

### 3.5 poll boundary 的来源（H1/M6 回执）

`poll boundary` = 有裸 libc `__poll` 帧、且没有任何内核 poll 帧（`do_poll`/`__x64_sys_poll`/`poll_schedule`/`do_sys_poll`）的样本。它们被从原阶段桶中移出：

| 原阶段 | P-B 移入 poll boundary | P-E 移入 poll boundary |
|---|---:|---:|
| `syscall (unattributed)` | **978** | **1,251** |
| `submit` | 518 | — |
| `io-wq punt` | 247 | — |
| `other` | 156 | 6 |
| `scheduler/IRQ` | 146 | — |
| `progress/wait` | 108 | 263 |
| `shared sync` | 88 | — |
| `reap/CQE` | 8 | — |
| `kernel syscall/FS overhead` | 2 | — |
| **合计** | **2,251（9.16%）** | **1,520（4.38%）** |

- P-B 其中 75 个含 eventfd；P-E 无 eventfd。P-B 2,251 全部是 app comm；P-E 1,520 全部是 `sluice-copy` comm（不能按 comm 分 worker）。
- 这证明 R0 的“`syscall unattributed` 不能强分给 poll”在这两个形态上过于保守：至少 978/1,251 个可由 libc `__poll` 帧直接识别。剩余 P-B 986（4.01%）、P-E 3,273（9.42%）仍不能强分给 pread/pwrite/futex。
- 518 个 P-B 样本在 R0 划分中属 `submit`（poll 返回路径的 `tctx_task_work_run` / `__io_submit_flush_completions`）；R1 已把它们移入 `poll boundary`。这是阶段标签的启发式限制，不是“submit 成本”。

### 3.6 非阶段桶的含义

| 桶 | 含义 | 是否可删除 |
|---|---|---|
| payload/FS | 4 KiB payload 拷贝、tmpfs shmem、page cache、`__io_read` | 否；除非改变 workload/数据路径 |
| io-wq punt | 内核 io-wq 异步 punt（tmpfs 下 1/op） | 非 Sluice 代码；#637 记 NO_GO；FS 条件性 |
| backend exec (TP) | TP worker 的 `worker_loop`、pread/pwrite | 否；数据搬运本身 |
| shared sync | 无阶段 owner 的 mutex/spinlock/futex | 部分必要；不能整体删 |
| scheduler/IRQ | EEVDF、PSI、IPI、futex wake、信号返回 | 大部分必要；只有过度并发/唤醒可讨论 |
| kernel syscall/FS overhead | fput/fdget/kmalloc/security 等 | 必要内核对象管理 |
| syscall unattributed | 截断栈的 syscall 入口/出口 trampoline（已拆出可识别 libc-poll） | 剩余不能强分给 pread/pwrite/futex/poll |
| poll boundary | 只有 libc `__poll` 帧、无内核 poll 帧 | 不是可删量；是 poll 相关边界样本 |
| bench harness | 基准验证/计时/pattern 校验 | 不属于产品路径；A/B 应排除 |
| other | 未归类 | 不能当可删项 |

### 3.7 阶段内 user/kernel 分解（关键阶段）

`analysis.json[*].cycles.user_kernel`；百分比是**该阶段样本内**的 kernel vs user（bench+lib）占比，不是全形态份额。置信 MEDIUM（M5：record 无 PEBS、用户栈截断）。**注意：该表是 R0 阶段划分下的 `analysis.json` 值；R1 的 `poll boundary` 没有单独重算 user/kernel，P-B/P-E 的 `progress/wait` 行仍是 R0 口径（含少量 poll boundary 样本）。**

| 阶段 | P-A | P-B | P-C | P-D | P-E |
|---|---:|---:|---:|---:|---:|
| admission | k 12% / u 88% | k 0% / u 100% | k 71% / u 29% | k 76% / u 24% | k 70% / u 30% |
| dispatch | k 57% / u 43% | k 95% / u 5% | k 69% / u 31% | k 16% / u 84% | k 0% / u 100% |
| submit | k 98% / u 2% | k 99% / u 1% | k 98% / u 2% | — | — |
| reap/CQE | k 80% / u 20% | k 97% / u 3% | k 67% / u 33% | — | — |
| terminal/release | k 25% / u 75% | k 0% / u 100% | k 8% / u 92% | k 20% / u 80% | k 2% / u 98% |
| publication | k 16% / u 84% | k 6% / u 94% | k 14% / u 86% | k 10% / u 90% | k 2% / u 98% |
| progress/wait | k 67% / u 33% | k 98% / u 2% | k 88% / u 12% | k 47% / u 53% | k 64% / u 36% |
| observation | k 23% / u 77% | k 0% / u 100% | k 16% / u 84% | k 8% / u 92% | k 0% / u 100% |

**解读**：
- `submit` 与 `progress/wait` 在 uring 形态几乎全是 kernel（submit 97–99%，P-B wait 98%）；把它们叫“Sluice 用户态成本”是错误的。
- `publication` / `terminal/release` / `observation` 在 TP/copy 形态 user 占 80–98%，主要是 libc mutex/condvar 与产品代码；这是 W3 最相关的面，但仍是**阶段容器**，不是单候选成本。
- `admission` 在 P-C/P-D/P-E 的 kernel 占 70–76%，说明该阶段样本里大量是 `submit_read` 栈内的内核唤醒/调度/锁，不是 RequestCore 的几条用户指令。
- `progress/wait` 的 P-B/P-E kernel 占比高，但该阶段包含 worker eventfd 与 progress pass；不能把整个阶段当“app 阻塞”。

### 3.8 不可相加矩阵（显式标记）

| 项 A | 项 B | 关系 | 规则 |
|---|---|---|---|
| 阶段 self | 阶段 inclusive | 同一调用栈的两种计数 | **不能相加**；inclusive 只说明“调用帧出现”，不是额外成本 |
| `poll_progress` 2,203 + 1,775 | union 2,205（13.386%） | 嵌套符号；both 1,773 | 只能用 union；R0 的 24.15% 是双计 |
| `eventfd_signal` 2,132 + 1,860 | union 2,132（8.672%） | mask 全部在 io_eventfd_signal 内 | 只能用 union；R0 的 16.24% 是双计 |
| `wait_one` 10,759 + 10,755 | union 10,759（30.974%） | 两个重载；both 10,755 | 只能用 union；R0 的 61.94% 是双计 |
| `publication` 4.13%（P-D） | `claim_execution` 0.61% | claim 叶样本 93/93 已归 publication | **不能相加**；M-62 的上界就是 4.13% |
| 阶段 self | leaf symbol | 同一样本可同时计入 leaf 与 stage owner | **不能相加**；两种切法 |
| PMU cycles:u/k | record DSO kernel/lib/bench | 分母/采集方式不同 | **不能互相换算** |
| `progress/wait` 25.09%（P-B） | `poll boundary` 9.16% | 两个不相交阶段行 | 可以相加得到 poll/wait 容器 34.24%，但**仍不是候选上界** |
| `__poll` 24.93% | `__x64_sys_poll` 15.77% / `do_sys_poll` 15.38% | 后者是前者子集 | **不能相加**；上界只能取一个 inclusive 栈 |
| `wait_one` union 30.974%（P-E） | `__poll` 15.83% / progress/wait 24.36% | `wait_one` 含非 poll 路径与 progress pass | **不能当 poll 成本**；不能相加 |
| off-CPU 事件数 | cycles 份额 | 事件数 vs 采样份额 | 单位不同；**不能当时间占比** |
| raw report（未过滤） | analysis（过滤 comm） | 分母不同（如 P-A 8,390 vs 8,369） | 份额不可直接对比 |
| W1/W3/M-53/M-42 | 彼此 | 同一进程 cycles 的不同子集/切法 | **不能相加** |

---

## 4. Top Hotspots

### 4.1 逐形态 leaf 前 8（`analysis.json[*].cycles.leaf_top`，过滤后样本）

| 形态 | #1 | #2 | #3 | #4 | #5 | #6 | #7 | #8 |
|---|---|---|---|---|---|---|---|---|
| P-A d16 uring | `rep_movs_alternative` 11.94% K | `_raw_spin_lock` 6.36% K | `entry_SYSRETQ_unsafe_stack` 3.81% K | `swapgs_restore_regs_and_return_to_usermode` 3.04% K | `shmem_file_read_iter` 2.83% K | `asm_sysvec_reschedule_ipi` 2.83% K | `io_cq_unlock_post` 2.53% K | `__memcmp_avx2_movbe` 1.88% L |
| P-B d1 uring | `rep_movs_alternative` 7.84% K | `entry_SYSRETQ_unsafe_stack` 6.22% K | `_raw_spin_lock` 5.41% K | `__raw_spin_lock_irqsave` 3.77% K | `__smp_call_single_queue` 2.10% K | `syscall_return_via_sysret` 2.06% K | `entry_SYSCALL_64` 2.05% K | `psi_group_change` 1.96% K |
| P-C W-03 d16 | `entry_SYSRETQ_unsafe_stack` 6.87% K | `rep_movs_alternative` 6.78% K | `_raw_spin_lock` 5.15% K | `__GI___pthread_mutex_unlock_usercnt` 3.96% L | `pthread_mutex_lock` 3.60% L | `native_queued_spin_lock_slowpath` 3.18% K | `entry_SYSCALL_64` 2.44% K | `syscall_return_via_sysret` 2.22% K |
| P-D TP d16 w4 | `entry_SYSRETQ_unsafe_stack` 13.29% K | `rep_movs_alternative` 9.04% K | `__GI___pthread_mutex_unlock_usercnt` 7.12% L | `pthread_mutex_lock` 5.96% L | `entry_SYSCALL_64` 4.69% K | `syscall_return_via_sysret` 4.28% K | `native_queued_spin_lock_slowpath` 2.50% K | `ThreadPoolBackend::worker_loop()` 2.24% B |
| P-E copy d4 w1 | `entry_SYSRETQ_unsafe_stack` 13.28% K | `__GI___pthread_mutex_unlock_usercnt` 7.60% L | `pthread_mutex_lock` 6.35% L | `entry_SYSCALL_64` 4.84% K | `syscall_return_via_sysret` 4.40% K | `rep_movs_alternative` 3.73% K | `run_pipelined_copy_with_context` 1.77% B | `clear_highpages_kasan_tagged` 1.48% K |

K = kernel，L = libc，B = bench/产品用户态。leaf 份额是分区；单个样本只归一个 leaf symbol。**不要**把同一行多个符号相加当作某个阶段成本。

### 4.2 跨形态热点族

| 族 | P-A | P-B | P-C | P-D | P-E | 说明 |
|---|---:|---:|---:|---:|---:|---|
| payload 拷贝 / tmpfs / page cache | `rep_movs` 11.94 + `shmem` 2.83 + `__io_read` 1.61 | 7.84 + 1.63 | 6.78 + 1.51 | 9.04 + 1.47 + `filemap_get_entry` 1.39 | 3.73 + `clear_highpages` 1.48 | 数据搬运本身；不是 Sluice 协调 |
| syscall 边界（entry/ret/swapgs） | 3.81+3.04+1.51+1.25 ≈ 9.6 | 6.22+2.06+2.05 ≈ 10.3 | 6.87+2.44+2.22+1.11 ≈ 12.6 | 13.29+4.69+4.28 ≈ 22.3 | 13.28+4.84+4.40 ≈ 22.5 | 叶族求和；与 `syscall unattributed` / trampoline 有重叠叙述 |
| 用户态 mutex / kernel spin | spin 6.36+1.86 | spin 5.41+3.77 | libc mutex 7.56 + spin 10.15 | libc mutex 13.08 + spin 4.13 | libc mutex 13.95 + spin 0.79 | 共享同步；部分必要 |
| scheduler / PSI / IPI | resched IPI 2.83 | `smp_call` 2.10 + PSI 1.96 | PSI 1.44 + resched 1.59 | PSI 1.50 + `__schedule` 0.98 | PSI 1.15 + `smp_call` 1.11 | 不是设备 IRQ |
| io-wq | `io_worker_handle_work` 1.51 + `io_wq_put_work` 1.29 + `io_wq_worker_running` 1.05 | 1.66+1.54+1.29 | —（punt 在 stage 6.02） | — | — | tmpfs uring 的 FS 条件性 punt |

**注意**：上表的“族求和”只是为了阅读方便；叶族之间、叶族与阶段表之间都不能相加。

---

## 5. CPU / Kernel / Worker / IRQ / FS 成本归属

### 5.1 CPU

- **总 cycles/op**：P-A 15,090；P-B 28,703；P-C 20,325；P-D 20,386；P-E 44,482/chunk（22,241/request）。d1 是 d16 的约 1.9×；W-03 与 TP d16 接近；copy 每 chunk 最重。
- **user/kernel**：user 17.7–28.1%，kernel 71.9–82.3%。用户态不是主体。置信 MEDIUM（M5）。
- **context-switch**：P-A 0.339/op、P-B 2.000/op、P-C 0.606/op、P-D 0.668/op、P-E 2.040/chunk。P-B/P-E 的每 op/chunk 两次切换与 poll/futex 等待形态一致。
- **PMU 离散度限制**：P-D `cycles:u` 2.58%、P-A `instr:k` 8.05%、P-A `ctx-sw` 25.53%；相关小份额不作强结论。
- **PMU/record 分母**：`perf record` 用 `-F 2999` freq 模式，`script` 无 period；`analyze_p2.py` 不按 period 重加权。PMU 与 record 的 kernel 份额在 P-B 差 10.8pp（81.7 vs 92.51），P-E 差 2.0pp；不能把 record kernel 份额与 PMU `cycles:k` 互相换算（M5）。
- **环境/可比性**：所有 bench `taskset -c 2,3`，但 task-clock / 进程 wall 约 2.9–3.0 CPU（P-A 2,593.96ms/0.8993s≈2.88；P-B 7,211.68ms/2.3989s≈3.01；P-C 3,487.02ms/1.1520s≈3.03；P-D 3,450.13ms/1.1716s≈2.94；P-E 1,197.82ms/0.3984s≈3.01）；`nmi_watchdog=1`；profile 期间没有逐轮 loadavg/ps。**未解释，不能写成“worker 跑在 taskset 之外”**（M8）。

### 5.2 Kernel

- 内核是 71.9–82.3% cycles。主要构成：payload 拷贝（11.94/7.84/6.78/9.04/3.73% `rep_movs`）、spinlock（6.36/5.41/5.15/1.63/—% `_raw_spin_lock`）、syscall 边界、tmpfs shmem/page cache、io-wq、调度/PSI/IPI。
- 阶段内的 kernel 占比：submit 97–99%；P-B progress/wait 98%；P-C progress/wait 88%；P-E progress/wait 64%。这些阶段不是“用户态协调”，而是**内核执行提交/等待/唤醒**。
- `syscall unattributed` R1：P-B 4.01%（986）、P-E 9.42%（3,273）、P-A 3.43%、P-C 4.10%、P-D 7.34%；已把可识别 libc-poll 拆到 poll boundary。剩余仍未解析到具体 syscall；不能强行分给 pread/pwrite/futex/poll。
- R0 口径下，加上误标 trampoline 后，P-C/P-D/P-E 的 syscall boundary 合计约 8.7/21.1/21.9%。R1 把可识别 libc-poll 拆到 `poll boundary` 后，P-E 的 `syscall unattributed` 降到 9.42%，因此旧的 21.9% 不能直接复用；这些合计只是“内核 syscall 入口/出口被截断”的粗分类，不是可删量。

### 5.3 Worker

- **uring worker（iou-wrk）**：P-A 48.95%、P-B 50.19%、P-C 33.16% 的采样；leaf 顶部是 `rep_movs`（24.09/15.20/20.10% of comm；R1 复算 P-B 15.28%、P-C 20.18%）、`_raw_spin_lock`（9.59/8.08/10.42%）、`io_cq_unlock_post`（4.95/—/3.55%）。worker 主要在做 payload 与内核完成处理。
- **TP worker**：P-D/P-E 同 comm，不能按 comm 拆；`worker_loop` inclusive 68.28/48.30%，leaf 2.24/1.17%；off-CPU `worker_loop` futex 22,135（68.6%）/80,918（48.6%）。这是 worker 侧等待/执行的直接证据。
- **重标定 trampoline**：P-D 的 `pread` trampoline 951 样本（6.26pp）、P-E 的 pread/pwrite/write 2,340 样本（6.74pp）应归 backend exec；P-D/P-E 的 `lll_lock_*` trampoline 1,039/734 样本应归 shared sync。见 §0.3(A)/R0 §0.2(A)。
- **W-03 驾驶员/提交者**：driver `epoll_wait` 4,645 off-CPU（14.7%）；submitter futex 12,829（40.5%）；`poll_progress` union 2,205（13.386%）是 progress pass（driver/调用线程）在一次 poll 中做的 reap/publish，不是纯等待。

### 5.4 IRQ

- **能看到的 IRQ**：`asm_sysvec_reschedule_ipi` 2.83%（P-A）、1.59%（P-C）；`sysvec_reschedule_ipi` 0.81%（P-A raw report）、0.23%（P-C raw report）；`__smp_call_single_queue` 2.10%（P-B）、1.11%（P-E）；`asm_sysvec_call_function_single` 0.18%（P-D）；`asm_sysvec_apic_timer_interrupt` 0.01%（P-B/P-E）。
- **`__irqentry_text_end`**：原始（未过滤）`report-cycles-dso-symbol.txt` 中 P-A 3.57%、P-C 1.35%、P-D 0.21%、P-E 0.03%、P-B 0.01%。这是 perf 的 section-boundary 符号，具体 IRQ 向量未解析；与 kallsyms 的 `asm_sysvec_*` 份额是不同分母，不能相加。
- **设备 IRQ NOT_OBSERVABLE**：本会话没有 per-IRQ 记录；`block:block_rq_issue/complete` 全为 0；bench 在 tmpfs；P-E source 是 btrfs warm page cache。`irqbalance` 前后 inactive。因此没有磁盘/NVMe/设备 IRQ 成本结论。
- **不要把 `__raw_spin_lock_irqsave` 当 IRQ**：它是关中断的自旋锁，P-A 1.86%、P-B 3.77%、P-C 1.82%、P-E 0.79%，属于 shared sync。
- `nmi_watchdog=1` 会周期性打断并占用 PMU 计数器；NMI 处理会计入 cycles:k/IRQ 桶，R1 不把它单列（M8）。

### 5.5 FS / 介质

- 所有 bench 形态在 tmpfs（64 MiB fixture，page-cache resident）；P-E 是 btrfs（`compress=zstd:1`）warm page cache → tmpfs。`block` 层 0 事件；没有磁盘、NVMe、cold、fsync/durability 结论。
- `payload/FS` 10.44–19.87% 是 tmpfs shmem/page cache 与 payload 拷贝，不是磁盘 FS。
- **io-wq punt 是 FS 条件性形态成本**：P-A/P-B/P-C `io_uring_queue_async_work` = 1/op，`io_uring_task_work_run` = 0.78/1.00/0.78/op；P-D/P-E 0。P-A/P-B/P-C io-wq punt 阶段 10.23/6.65/6.02%。这与 #637/#622 的“FS 条件性、非 Sluice 代码”一致，不能外推到其他 FS/设备。
- P-E source 已 warm；profile 窗口不带 runner 的 per-arm 哈希/进程开销；不能与 P1 C1 arm 作 A/B。

### 5.6 Sluice 用户态协调成本（可见帧下界，不是族成本）

用 twin 符号解析后的 bench/sluice DSO leaf，按符号前缀分。**这些是“可见帧”的下界，不是产品族的完整成本**；P-B 因 90.7% app 样本缺 bench 帧而不可用。

| 形态 | 产品 `sluice::`/`sluice_copy::` leaf | `driver_thread` leaf | bench harness leaf | 其他 bench leaf | 可用性 |
|---|---:|---:|---:|---:|---|
| P-A | 632（7.55%） | 0 | 136（1.63%） | 42（0.50%） | 可见帧下界 |
| P-B | 120（0.49%） | 0 | 14（0.06%） | 5（0.02%） | **NOT_OBSERVABLE**（90.7% app 无 bench 帧） |
| P-C | 883（5.36%）+14 `driver_thread` = 897（5.45%） | 14 | 138（0.84%） | 51（0.31%） | 可见帧下界；M9 口径差异 |
| P-D | 854（5.63%） | 0 | 128（0.84%） | 45（0.30%） | 可见帧下界 |
| P-E | 2,410（6.94%） | 0 | 0（0.00%） | 94（0.27%） | 可见帧下界 |

单函数 leaf 例子：`dispatch_one_locked` 1.24/0.02/1.03/0/0%；`claim_execution` 0.12/0.00/0.07/0.61/0.40%；`worker_loop` 0/0/0/2.24/1.17%；`run_pipelined_copy_with_context` 1.77%（P-E）；`wait_one` 0.02/0.07/0/0.09/0.29%（P-B 的 0.07% 只来自 1,142 个可见 bench 帧样本，不能当族成本）。

**结论**：可见的产品 leaf 只有 0.5–7.6%（P-B 不可用）；阶段容器中的大头是内核/worker 机器工作。任何“删掉 Sluice 某段用户代码就能省下阶段份额”的推断都不成立。

---

## 6. 候选收益上界（R1）

> 上界是**测量份额/计数算术**，不是预期收益；上界内部互相重叠，**不能相加**；未做单因素 A/B，不能据此声称任何候选 GO。R1 进一步规定：**阶段容器不叫上界**。

### 6.1 W1 — ProgressSource poll 等待路径（修正）

| 形态 | `__poll` inclusive | `__x64_sys_poll` inclusive（子集） | `poll boundary`（libc only） | `progress/wait`（R1） | 计数证据 | 候选可删量 |
|---|---:|---:|---:|---:|---|---|
| P-B d1（最热） | **6,129/24,585 = 24.93%** | 3,878 = 15.77% | **2,251 = 9.16%** | **6,168 = 25.09%** | poll 539,964/540,000 ≈ 1/op；wait_one 60,000/60,000 = 1.000/op；ctx-sw 2.000/op；off-CPU poll 59,947 | **NOT_OBSERVABLE** |
| P-E copy | **5,500/34,736 = 15.83%** | 3,980 = 11.46% | **1,520 = 4.38%** | **8,461 = 24.36%** | poll 86,309/65,536 = 1.317/chunk；wait_one 73,524/65,536 = 1.122/chunk；ctx-sw 2.040/chunk；off-CPU poll 83,481 | **NOT_OBSERVABLE** |
| P-A d16 | 0 | 0 | 0 | 441 = 5.27% | poll 0/op；wait_one 433 个样本（inclusive 5.17%）；无 poll 计数 | 不支持候选 |
| P-C W-03 | 0 | 0 | 0 | 2,683 = 16.29% | poll 0/op；epoll_wait 0.111/op；futex 1.05–1.53/op | 不是同一 `poll(-1)` 路径 |

**拆分恒等式**：`__poll` = `__x64_sys_poll`（内核 poll 栈）+ libc-only poll boundary。P-B `3,878 + 2,251 = 6,129`；P-E `3,980 + 1,520 = 5,500`。这正是 R0 把 15.77%/11.46% 当上界时漏掉的部分。

**R1 的更正**：

1. R0 的 `≤15.8%/≤11.5%` 是 **`__x64_sys_poll` 内核栈子集**，不是 poll 相关样本的上界；`__poll` 24.93%/15.83% 才是可复算的 poll 相关 inclusive。
2. `progress/wait` 25.09%/24.36% 与 `poll boundary` 9.16%/4.38% 是两个不相交阶段行；合起来是 poll/wait 容器（P-B 34.24%、P-E 28.73%），但**仍不是候选上界**：P-B 的 `progress/wait` 25.09% 含 2,057 个 worker-eventfd + 3,572 app-poll + 297 poll+progress-pass + 159 progress-pass + 83 eventfd，另有 `poll boundary` 2,251（9.16%）；P-E 的 `progress/wait` 24.36% 含 3,972 poll+progress-pass + 3,537 progress-pass + 952 eventfd，另有 `poll boundary` 1,520（4.38%）。
3. `wait_one` 的 union 是 30.974%（P-E），不是 61.94%；它包含大量 progress pass 与非 poll 路径，不能当 poll 成本。
4. 历史 R1 工作稿（`docs/evidence/621-round2-R1/REPORT.md`）的 5.0–5.5% 未被本 profile 证实；它落在 poll 相关份额内，但没有行级/逐请求分离、没有 A/B，不能称为可删量。
5. **P-B 的 poll 相关证据仍受用户栈截断影响**：6,129 个 `__poll` 样本中 5,664 个没有 `wait_one` 帧；873 个是 4 帧栈（`entry_SYSCALL_64_after_hwframe -> __internal_syscall_cancel -> __syscall_cancel -> __poll`）。这些样本的 libc poll 边界可见，但 poll 之后的用户进度不可见。

**扣除项**：阻塞 eventfd read 仍需要等待/唤醒/eventfd 信号；`io_eventfd_signal` 2,132（8.672%）、eventfd read、`poll_schedule_timeout` 2,733（11.12%）、wake/scheduler 成本不会全部消失。因此可删除 cycles 必然显著低于 poll 相关 inclusive；R1 不给数值上界。

### 6.2 W3 — RequestCore mutex/生命周期族（阶段容器，不是候选上界）

**族级 self**（admission + terminal/release + publication + observation；R1 计数）：

| 形态 | 族级 self | 其中 user（bench+lib） | 其中 kernel | 备注 |
|---|---:|---:|---:|---|
| P-A d16 | 502 = 6.00% | 4.92% | 1.08% | 单线程 submit；不能作候选上界 |
| P-B d1 | 59 = 0.24% | 0.24% | 0.00% | **NOT_OBSERVABLE**：90.7% app 样本缺 bench 帧；不能当族成本 |
| P-C W-03 | 2,606 = 15.82% | 8.63% | 7.19% | admission 8.91 + terminal 2.31 + publication 2.82 + observation 1.77 |
| P-D TP w4 | 3,405 = 22.43% | 11.50% | 10.93% | admission 12.76 + terminal 3.49 + publication 4.13 + observation 2.05 |
| P-E copy | 4,539 = 13.07% | 7.98% | 5.09% | admission 7.16 + terminal 0.88 + publication 3.30 + observation 1.74 |

**逐候选（R1 状态；括号内是该阶段的 user 部分）**：

| 候选 | 对应阶段容器 | P-A | P-B | P-C | P-D | P-E | R1 判定 |
|---|---|---:|---:|---:|---:|---:|---|
| M-61 `begin`/`complete` 两锁融合 | publication | 1.46%（u 1.23） | 0.07%（u 0.07） | 2.82%（u 2.43） | **4.13%（u 3.71）** | 3.30%（u 3.23） | **阶段容器 / NOT_OBSERVABLE**；不给候选上界 |
| M-62 无观察者跳过 claim/retire | publication（claim 已在其中） | **1.46%** | **0.07%** | **2.82%** | **4.13%** | **3.30%** | **阶段容器 / NOT_OBSERVABLE**；R0 的 4.74% 双计已删 |
| M-65 owed `release_control` 第三锁融合 | terminal/release | 0.97%（u 0.73） | 0.03% | 2.31%（u 2.13） | **3.49%（u 2.80）** | 0.88%（u 0.86） | **阶段容器 / NOT_OBSERVABLE**；不给候选上界 |
| M-67 sticky 双标志 | progress/wait + poll boundary | 5.27%（u 见 R0 §2.5） | **25.09% + 9.16%**（u 未按 R1 重算） | 16.29%（u 见 R0 §2.5） | 3.51%（u 见 R0 §2.5） | **24.36% + 4.38%**（u 未按 R1 重算） | **混合容器 / NOT_OBSERVABLE**；含 worker-eventfd 与 progress-pass |
| M-10 零长发布三锁融合 | publication + terminal | **NOT_OBSERVABLE**（零长 op 未在本 workload 出现） | — | — | — | — | 不变 |

- `claim_execution` leaf self 0.12/0.00/0.07/0.61/0.40%；其 10/0/12/93/139 个叶样本全部已归 publication（`forms[*].claim_execution`）。M-62 的 P-D 上界是 **4.13%**，不是 4.74%。
- 置信：阶段 self 是 MEASURED（HIGH，但阶段标签是启发式）；阶段内 user/kernel 是 MEDIUM；**单候选可删除量 NOT_OBSERVABLE**。融合后仍保留至少一把锁/一次发布，不能把整个阶段当收益。
- **P-B 的 0.24% 不可用**：`RequestCore::reserve/accept` 等可见 leaf 只有 1,142 个样本；其余 11,104 个 app 样本没有 bench 帧。不能把 0.24% 当族成本，也不能用“严重内联”解释（H2）。
- `sticky 双标志`（`progress_epoch_`/`progress_exhaustion_`）在 release 符号中没有独立可采样函数；`signal`/`drain_notification_nolock_` 同样不可单独采样。R1 不把它们当候选。

### 6.3 M-53 — cookie O(R) 逆向扫描 → `user_data` 路由索引

| 形态 | `find_live_router_cookie_` leaf self | inclusive | cycles/op（self × 总 cycles/op） | 备注 |
|---|---:|---:|---:|---|
| P-A d16 | 34/8,369 = **0.406%**（Wilson 95% CI 0.291–0.567%） | 49 = 0.59% | ≈**61.3 cyc/op** | twin 解析；样本量小 |
| P-C d16 | 47/16,472 = **0.285%**（Wilson 95% CI 0.215–0.379%） | 47 = 0.29% | ≈**58.0 cyc/op** | 同上 |
| P-B d1 | 0 | 0 | ≈0 | router 表仅 1 项 |
| P-D / P-E | 0（无 uring） | 0 | 0 | — |

- 上界：d16 形态 ≤**0.41%** 进程 cycles / ≤**62 cycles/op**（self）；d1 几乎为 0。
- **d64 NOT_OBSERVABLE**：本轮未采集 d64；P1-A 历史 d64 1.74% / ≈116 cyc/op 只能作线索。`AA-CALIBRATION-2.md:27,499` 的 C7 d64 A/A 只有 cycles 可用，16 对 t-hw 1.007% 对 2%/2=1.0% 差一对，建议 17–20 对。
- **匹配 A/A**：P-A 的 d16 匹配 C3b（cycles hw 0.621%、MDE80 0.988%，但 ENVIRONMENT_BLOCKED，带未认证）；P-C 的 W-03 d16 匹配 C4（cycles MDE80 6.088%、CPU 2.262%）。R0 用 C6 CPU 0.747% 作 d16 代理是格不匹配（C6 是 d1）；R1 不再这样比较。
- 置信：d16 份额 MEDIUM（34/47 个样本、无 PEBS/skid、Wilson CI 假设独立二项）；d64 NONE；三项迁移义务（TransportLedger 位置单调、控制 CQE exact-submitted、cancel 定向）未在 profile 中验证。**未达到 ledger 准入。**

### 6.4 M-42 — `submit_flushes_` 只写不读原子计数

- 机制：`src/async/uring_transport.cpp:222` 在 ledger 空早退之后 `submit_flushes_.fetch_add(1, relaxed)`；唯一读者在测试宏内（`src/async/uring_test_seams.hpp:75-76`，成员 `include/sluice/async/detail/uring_backend.hpp:324`）。
- **objdump 证据**（本会话执行）：

```bash
objdump -d --start-address=0x424190 --stop-address=0x424230 \
  /tmp/p2-621/symtwin/bench_w02_pipeline.sym
# 4241b7: f0 48 83 87 20 01 00  lock addq $0x1,0x120(%rdi)
```

- 成本：review M4 引用 Haswell L1 命中的 locked RMW 约 **19–20 cycles**；本会话只确认指令是 `lock addq`，**没有重新测锁延迟**。

| 形态 | 每 op cycles | 19 cycles 上界 | 20 cycles 上界 | 1 instr/submit | `io_uring_submit_req` |
|---|---:|---:|---:|---:|---:|
| P-A d16 uring | 15,090.0 | **0.126%** | **0.133%** | **0.0094%** | 540,028（≈1/op） |
| P-B d1 uring | 28,703.2 | **0.066%** | **0.070%** | **0.0041%** | 540,028（≈1/op） |
| P-C W-03 d16 | 20,324.9 | **0.093%** | **0.098%** | **0.0079%** | 540,028（≈1/op） |
| P-D TP d16 w4 | — | **N/A** | **N/A** | **N/A** | 1（启动残差；TP 无 uring submit） |
| P-E copy | — | **N/A** | **N/A** | **N/A** | 0（TP 无 uring submit） |

- 计算式：`19/cycles_per_op×100`、`20/cycles_per_op×100`；`1/instructions_per_op×100`（脚本 `m42.forms`）。
- **上界：≤0.14% 进程 cycles**（保守取整，P-A 最大 0.133%）；**P-D/P-E 没有 M-42 数字**，因为 `submit_flushes_` 不会执行。
- **NOT_POWERED**：匹配格 P-A C3b instr MDE80 0.088%、cycles MDE80 0.988%；P-B C6 instr MDE80 0.010%、cycles MDE80 2.155%、CPU MDE80 0.747%；P-C C4 instr MDE80 0.318%、cycles MDE80 6.088%。1 条 instruction/submit 的信号 0.0041–0.0094% 低于这些带；19–20 cycles 的 0.066–0.133% 也远低于 matching CPU/cycles 带。C5b 的 TP instr MDE 0.002% **不是匹配格**（M-42 是 uring-only），不能引用。
- 置信：上界算术 MEDIUM（lock 成本为外部引用，未重测）；精确效应 **NOT_OBSERVABLE / NOT_POWERED**。

### 6.5 非候选：io-wq punt / H1 提交批形态

- **io-wq punt**：P-A/P-B/P-C `io_uring_queue_async_work` = 1/op，stage self 10.23/6.65/6.02%；P-D/P-E 0。它是 tmpfs uring 的 FS 条件性内核行为，非 Sluice 代码；#637 记 NO_GO（非候选 2），本轮独立复现“条件性存在”，不产生产品收益上界。
- **H1 提交批形态**：`sys_enter_io_uring_enter` = 1/op（P-A/P-B/P-C）；submit 阶段 21.35/8.80/12.49%，其中 kernel 97.6/98.7/98.2%。这是 immediate per-dispatch submit 的现状，**不是** H1 的 A/B 证据；#637 的 H1 仍为 NO_GO，无新证据不复活。

### 6.6 A/A 匹配与 MME（H3/M9 回执）

A/A 噪声带来自 `p1/AA-CALIBRATION.md` §5 与 `p1/AA-CALIBRATION-2.md` §5。**R1 只允许使用形态匹配的格**；不能把 C5b/C3b/C7 的带搬到 P-B。

| 形态 | 匹配格 | 同 binary/形态依据 | 指标 | hw %（n=16） | SD pp | MDE80 % | δ=1.2% n80/n_half | 判词/备注 |
|---|---|---|---:|---:|---:|---:|---|---|
| P-A d16 uring | C3b（P2；P1 C3 同形） | `W02-uring-4k-d16`；binary `03dbef09…` | cycles | 0.621 | 1.319 | 0.988 | 12 / 22 | ENVIRONMENT_BLOCKED；带未认证 |
| P-B d1 uring | **C6** | `W02-uring-4k-d1`；binary `03dbef09…` | cycles | 1.368 | 2.876 | **2.155** | **48 / 91** | **C6 wall ORDER_SENSITIVE**（+1.132pp, p=0.0049）；cycles/CPU 可用 |
| P-B d1 uring | **C6** | 同上 | CPU | 0.471 | 0.997 | **0.747** | **8 / 14** | 同上；CPU 是较窄指标 |
| P-C W-03 d16 | C4 | `W03-uring-4k-d16`；binary `43054fcd…` | CPU | 1.402 | 3.019 | 2.262 | 52 / 100 | QUALIFIED（≥~2.3% CPU） |
| P-C W-03 d16 | C4 | 同上 | cycles | 3.843 | 8.123 | **6.088** | 362 / 707 | 16/36 对远不够 1.2% |
| P-D TP d16 w4 | C2b | `W02-threadpool-4k-d16-w4`；binary `03dbef09…` | — | — | — | — | — | **NOISY（instr 漂移 p=0.0017）；无可用带** |
| P-E copy | **C1** | `sluice-copy 4k/d4/w1 --sync none --no-atomic`；binary `db972b19…` | cycles | 1.121 | 2.346 | **1.758** | **32 / 62** | QUIET |
| P-E copy | **C1** | 同上 | CPU | 0.365 | 0.786 | **0.589** | **6 / 10** | QUIET；CPU 是较窄指标 |

**使用规则**：

1. **C6 wall 是 ORDER_SENSITIVE**：第二臂 wall 平均慢 +1.132pp（13/16 为正，p=0.0049）；任何 C6 wall A/B 必须严格 AB/BA 平衡，不能把 wall 当“无顺序效应”的干净基线。R1 用 C6 的 cycles/CPU，不用 wall。
2. **P-B 不能再用 C5b/C3b/C7 作代理**：C5b 是 TP d1 w1，C3b 是 uring d16，C7 是 uring d64；与 P-B（uring d1）不同格/后端/深度。R0 §5.6 的“C5b 0.72 / C3b 0.99 / C7 1.42”组合不适用于 P-B。
3. **36 对不够覆盖 P-B 的 C6 cycles 1.2% 效应**：C6 cycles 80% 需 48 对、hw≤0.6% 需 91 对；36 对可覆盖 C6 CPU 的 8/14，但不够 C6 cycles 的 48/91。若 W1 预注册用 cycles，预算至少 48 对；用 CPU，至少 14 对（ledger 最低 36 对仍应保留）。
4. **P-E 的 C1 cycles 1.2% 需 32/62**：36 对够 80% 功效（32），但不够 hw≤0.6%（62）；C1 CPU 6/10 够。
5. **P-C 的 C4 cycles 1.2% 需 362/707**：36 对远不够；W3 若在 P-C 上做 1.2% 声明，必须换指标或接受更大预算。
6. **P-D 没有可用 A/A**：C2b 是 NOISY；C5b 是 TP d1 w1，不是 P-D 的 d16 w4。P-D 的 W3 预算不能引用 C5b 的 0.72% CPU 带。
7. **C3/C7 的 cycles 带即使窄也不自动可搬**：C3b/C7 的 CPU/wall 带被阻塞；C3b cycles 是“条件性诊断带”，用前须 cycles-only A/A；C7 d64 只适用于 d64 研究（M-53），不是 P-B 代理。
8. 这些数字只回答“如果效应是 X，需要多少对/哪个格”，不回答“效应是 X”；A/A 的 hw/MDE 不能跨格搬运（`AA-CALIBRATION.md:400-402`、`AA-CALIBRATION-2.md:406-419,499`）。

### 6.7 上界总表与重叠标记（R1）

| # | 候选/形态 | R1 数值 | 依据 | 置信 | 可与谁相加？ |
|---|---|---:|---|---|---|
| B1 | W1 P-B poll 相关 | `__poll` **24.93%**；`__x64_sys_poll` 15.77% 子集；poll boundary 9.16%；progress/wait 25.09% | inclusive `__poll`；stage self；poll 1/op | 份额 HIGH；可删 **NOT_OBSERVABLE** | **不可**与 B2/B3/W3/M-53/M-42 相加 |
| B2 | W1 P-E poll 相关 | `__poll` **15.83%**；`__x64_sys_poll` 11.46% 子集；poll boundary 4.38%；progress/wait 24.36% | 同上 | 份额 HIGH；可删 **NOT_OBSERVABLE** | **不可**与 B1 相加（不同形态） |
| B3 | W1 P-A | progress/wait 5.27%，但 poll=0 | stage self；wait_one 433 样本 | 不支持候选 | 不可当 W1 上界 |
| B4 | W3 族 P-C/P-D/P-E | 15.82/22.43/13.07% 阶段容器；user 8.63/11.50/7.98% | stage self；阶段内 user/kernel | MEDIUM；**不是候选上界** | **不可**与 M-61/M-62/M-65/M-67 各自相加 |
| B5 | M-61 publication | 4.13 P-D / 2.82 P-C / 3.30 P-E / 1.46 P-A / 0.07 P-B | publication stage self | LOW；**阶段容器 / NOT_OBSERVABLE** | 属于 B4 的子集；不可与 B4 相加 |
| B6 | M-62 | **4.13 P-D / 2.82 P-C / 3.30 P-E / 1.46 P-A / 0.07 P-B** | publication stage self（claim 已在其中） | LOW；**NOT_OBSERVABLE** | 子集；不可与 B5 相加；R0 4.74% 已删 |
| B7 | M-65 terminal | 3.49 P-D / 2.31 P-C / 0.97 P-A / 0.88 P-E / 0.03 P-B | terminal stage self | LOW；**阶段容器 / NOT_OBSERVABLE** | 子集；不可与 B5/B6 相加 |
| B8 | M-67 progress/wait | 25.09+9.16 P-B / 24.36+4.38 P-E / 16.29 P-C / 5.27 P-A / 3.51+0.06 P-D | 阶段容器 | LOW；**NOT_OBSERVABLE** | 与 W1 重叠；不可相加 |
| B9 | M-53 d16 | 0.406% / 61.3 cyc/op（P-A）；0.285% / 58.0 cyc/op（P-C） | twin 解析 leaf self；34/47 样本 | d16 **MEDIUM**（小样本/无 PEBS）；d64 NONE | 属于 reap/CQE 阶段子集；不可与 B10 相加 |
| B10 | M-53 d64 | **NOT_OBSERVABLE** | 未采集 d64 | NONE | — |
| B11 | M-42 | **≤0.14% cycles**（P-A 0.126–0.133%、P-B 0.066–0.070%、P-C 0.093–0.098%）；1 instr 0.0041–0.0094%；P-D/P-E **N/A** | `lock addq` 19–20 cycles × 实测 cycles/op | 算术 MEDIUM；效应 **NOT_POWERED** | 属于 submit 阶段子集；不可与 B1/B4 相加 |

**总规则：B1–B11 全部不能相加；同一候选的“阶段容器”与“user 部分”也不能相加（user 是阶段的子集）；B5–B8 不是候选上界。**

---

## 7. 与 #637 / #622 历史结论对照

来源：`CAMPAIGN-LEDGER.md` §4/§5/§6.3（对 #637 终态与 #622 的转述）。**#622 原始 cost map 不在本仓库已跟踪/可引用范围内**（其在未跟踪目录 `docs/evidence/e5-fullpath-machine-work/`，台账 §9.1.12 要求不触碰）；本图未打开或引用其内容，只对照台账转述，置信受限。`docs/evidence/621-round2-R1/REPORT.md` 是已被 R2 r6 取代的工作稿，也只作线索。

| 历史结论 | 台账来源 | 本轮独立复现？ | 不同点 / 限制 |
|---|---|---|---|
| W1 `poll(-1)` 约 5.0–5.5% cycles 可删 | `:257,270`（R1 工作稿，已被取代） | **未复现为可删量**；只复现“等待路径是热点”：P-B `__poll` 24.93%、poll boundary 9.16%、progress/wait 25.09%；P-E `__poll` 15.83%、poll boundary 4.38%、progress/wait 24.36% | 这些是 inclusive/阶段容器，不含可删量；5.0–5.5% 落在其中但没有 A/B、没有行级分离，不能称已证实 |
| W3 mutex family ~9–11% | `:259,271,223`（r6 F.6 族级线索） | **部分复现**：P-C 15.82%、P-D 22.43%、P-E 13.07%、P-A 6.00%；P-B 0.24% **NOT_OBSERVABLE** | 量级随形态变化；9–11% 不是普适值。族级存在成立，单候选仍无 |
| M-61 R3 拒绝：0.45% < 1% MME REJECTED | `:222` | **未在同口径复现**：publication 阶段 1.46/0.07/2.82/4.13/3.30%；但这是 stage container，不是候选锁成本 | 阶段包含 publish/deliver 的内核/用户工作；不能拿 4.13% 推翻 0.45% 的候选级测量。不同测量范围 |
| M-53 P1-A d64 1.74% / ≈116 cyc/op | `PROFILE-SUMMARY.md:427` 转述 | **未复现 d64**；d16 self 0.406/0.285% / 61.3/58.0 cyc/op | d16 方向相同但更小；d64 未采集。C7 d64 A/A 边缘（17–20 对）也未把 profile 的 d64 效应测出来 |
| M-42 `submit_flushes_` NOT_POWERED | `:217`；`AA-CALIBRATION.md:475-500` | **独立复现结论**：`lock addq` 19–20 cycles，≤0.14% cycles，1 instr/submit 0.0041–0.0094%；P-D/P-E N/A | 本轮是算术复现，不是新测量；仍需隔离格 |
| H1 提交批形态 NO_GO | `:229` | **现状复现**：`io_uring_enter` 1/op、submit 阶段 8.80–21.35%、ring flags=0 | 未做 H1 A/B；不能因本轮 submit 份额大就复活 H1 |
| io-wq punt FS 条件性 | `:230,323`（#637 非候选 2 / #622 转述） | **独立复现条件性**：tmpfs uring `queue_async_work`=1/op、stage 6.02–10.23%；TP 0 | 非 Sluice 代码；不能外推到磁盘/NVMe/其他 FS |
| #622：最高杠杆 H1 不跨边界；产品 ring flags=0；cost map 是 trigger-grade | `:323-326` | **部分一致**：`io_uring_queue_init(..., 0)`（`src/async/uring_backend.cpp:171`）确认 flags=0；submit 阶段 97–99% kernel；设备 IRQ 不可见 | #622 原文未打开/未引用；不能把台账转述当作本轮独立复现。G1（真实 workload）仍未满足 |

**结论**：#637 的候选终态没有被本轮 profile 推翻。本轮新增的是“形态级 measured hotspot 地图”，不是候选级 A/B；`#637` 的 `INCONCLUSIVE`/`NO_GO`/`SEMANTIC_MISMATCH` 状态继续有效。

---

## 8. W1 / W3 / M-53 / M-42 是否达到「有 measured hotspot」准入（R1）

判定只针对“是否有 measured hotspot”这一门槛；**不是 A/B 准入，也不是 GO**。**R1 额外规定：阶段容器不算候选级 measured hotspot。**

| 候选 | 有 measured hotspot？ | 证据 | 准入判定 | 还缺什么 |
|---|---|---|---|---|
| **W1** ProgressSource poll | **阶段/边界级是**（P-B/P-E）；**候选级否**；P-A/P-C 否 | P-B `__poll` 24.93%、poll boundary 9.16%、progress/wait 25.09%、poll 1/op、off-CPU poll 50.0%；P-E `__poll` 15.83%、poll boundary 4.38%、progress/wait 24.36%、poll 1.317/chunk、off-CPU poll 50.2% | **未过 ledger 的 profile 门**：本 profile 只证明 **bench/reference 形态 hotspot**；`sluice-copy` 是参考应用，不是 G0 workload；#604 G0 未准入（`CAMPAIGN-LEDGER.md:257,311-315,323-326`）。**未达到候选级准入**（可删量、行级/逐请求分离、语义设计、A/A、A/B 均未做） | 真实消费者 profile；有界等待/非阻塞 drain/dup fd 设计；5% 分母预注册；A/A 36 对（若用 C6 cycles 则 ≥48/91 对）；A/B ≥36 对；若升级为 kernel-boundary 研究，走 #617 G1 |
| **W3** RequestCore 族 | **族级是**（P-C 15.82%、P-D 22.43%、P-E 13.07%）；**单候选不是**；P-B **NOT_OBSERVABLE** | 阶段 self 与阶段内 user 分解；`RequestCore` 单函数 leaf 仅 0.12–0.61%（claim）等；P-B app 90.7% 无 bench 帧 | **未达到 W3 自身的准入**：ledger 要求“measured 行级归因”，本轮只有符号级/阶段容器；P-B d1 因用户栈截断不可分离；单候选（M-61/62/65/67）只有阶段容器，没有候选级 measured hotspot | 行级 profile（当前 NOT_OBSERVABLE，需 debug build/隔离格）；每候选的步间语义证明；族 A/A；单因素 A/B |
| **M-53** cookie O(R) | **d16 是，但很小**（P-A 0.406%、P-C 0.285%）；**d64 否** | twin 解析 34/47 样本；d1/TP/copy 0；d64 未采集；匹配 A/A C3b/C4 的分辨力不足 | **未达到 ledger 准入**：ledger 要求 d64 深度格 A/A 半宽 ≤ 效应/2 + 三项迁移义务；本 profile 没有 d64，三项义务未证 | d64 profile + d64 cycles-only A/A（17–20 对）；TransportLedger 单调、控制 CQE exact-submitted、cancel 定向证明 |
| **M-42** `submit_flushes_` | **否** | `lock addq` 19–20 cycles；≤0.14% cycles；1 instr 0.0041–0.0094%；P-D/P-E N/A；profile 无法单独解析该内联 RMW | **未达到**：没有 measured hotspot；只有算术上界，且低于匹配 uring 16 对噪声带 | 隔离格（submit-only 紧循环）+ 动态回执带 + 机制门七问；在隔离格 A/A 完成前保持 NOT_POWERED |

**一句话**：W1 只在 P-B/P-E 上达到“poll/等待阶段有 measured hotspot”的**形态级**证据，未过 ledger 的真实消费者 profile 门；W3 只达到族级、未达到单候选/行级门槛；M-53 只达到 d16 小热点、未达到 d64 门槛；M-42 没有 measured hotspot。**四者都没有达到候选级准入或 A/B 准入。**

---

## 9. 缺口与不能下的结论

### 9.1 缺口

1. **行级归因 NOT_OBSERVABLE**：release 二进制 stripped、无 DWARF；内核无 debuginfo。所有阶段/候选数字都是符号级或阶段级。
2. **没有 A/B / 语义门**：没有候选 build、mutant、A/A、A/B、TLC；不能推出任何收益。
3. **采集开销**：DWARF callgraph 使进程时长 1.6–2.2×；tracepoint stat 2.0–3.7×；uprobe 5–110×。份额可用于归因，时长/吞吐不可用于收益；份额在 1.59×/2–3.7× 下的稳定性未证明（M7）。
4. **`syscall unattributed` 剩余 P-B 4.01%（986）/P-E 9.42%（3,273）**：已拆出 978/1,251 个 libc-poll，但剩余内核栈仍在 syscall 入口/出口截断；不能强分给 pread/pwrite/futex。
5. **`shared sync` / `scheduler/IRQ` / `kernel syscall/FS overhead` 无阶段 owner**：不能整体视为可删除；其中一部分是必要同步/调度/内核工作。
6. **`startup/thread create` 是栈根 artifact**：不是线程创建成本；已按 callee 重标定，但 callee 分类仍有 ≤0.12pp 未归类。
7. **P-B 用户栈截断**：app 12,246 中 11,104（90.7%）无 bench 帧；`wait_one` 只可见 700；5,664/6,129 `__poll` 无 `wait_one`；873 个 4 帧 poll 栈。P-B 的 W3/product leaf/`wait_one` 份额 **NOT_OBSERVABLE**。
8. **P-B 阶段标签启发式**：518 个 libc-poll-only 样本在 R0 划分中属 `submit`（poll 返回路径的 task_work）；R1 已把它们移入 `poll boundary`，但 `submit` 8.80% 仍不是纯 `io_uring_enter` 栈。
9. **IRQ 只有入口/向量级**：没有 per-IRQ 亲和性/设备 IRQ 身份；`block` 0 事件；tmpfs；不能下设备 IRQ/磁盘 FS 结论。`nmi_watchdog=1` 的 NMI 成本未单列。
10. **未覆盖形态**：d64、64K、zero-op、cancel、saturation、TP w1/w8、W-03 d4/d64 未采集；M-10 零长因此 NOT_OBSERVABLE；M-53 d64 NOT_OBSERVABLE。
11. **P-E 介质**：btrfs warm page cache → tmpfs，单次 398.4ms；source 已 warm，且 profile 窗口不带 runner 的 per-arm 哈希/进程开销；不能与 P1 C1 arm 作 A/B。
12. **tracepoint stat vs record 计数不同**：P-C futex 825,214（epoll 组） vs 566,325（sched 组）；record 自身改变执行/批形。计数结论以 `perf stat` 为准。
13. **PMU vs record DSO 差异**：P-B 81.7%（PMU k） vs 92.5%（record kernel）；不能互相替代；`perf record -F 2999` 是 freq 模式，`script` 无 period，`analyze_p2.py` 不按 period 重加权。
14. **`task-clock` 两遍 stat 不同**：P-A 2,593.96 vs 2,690.60ms；R1 沿用 `analysis.json` 的 stat-mem 值。
15. **M-53 小样本**：P-A 34、P-C 47 个 leaf 样本；Wilson 95% CI 0.291–0.567% / 0.215–0.379%；无 PEBS/skid，CI 假设独立二项。
16. **`wait_one` union 30.974%（P-E）不可当等待成本**：它在大量样本的调用帧中出现，含非 poll 路径；只能作为“调用栈出现”证据。
17. **#622 原文未打开/未引用**：只对照 `CAMPAIGN-LEDGER.md:323-326` 的转述；未跟踪目录 `docs/evidence/e5-fullpath-machine-work/` 按其纪律未作为证据源引用。
18. **off-CPU 是事件数**：不是阻塞时长；窗口只有一个 measured round。
19. **`SHA256SUMS` 自哈希 stale**：前三项 OK，第 4 行自引用 FAILED；见 §0.3(D)。
20. **任务时钟/亲和性**：taskset 2,3 与 task-clock/进程 wall ≈2.9–3.0 CPU 的不一致未解释；worker 的 `Cpus_allowed_list` 未记录。

### 9.2 不能下的结论

- 不能说 W1 能删 5.0–5.5%（或任何百分比）；只能说 P-B/P-E 的等待阶段/poll 边界有形态级 measured hotspot，`__poll` inclusive 24.93/15.83%。
- 不能说 W1 的 `15.8%/11.5%` 是上界；它是 `__x64_sys_poll` 内核栈子集。
- 不能说 W3 的 13.07–22.43% 是单候选收益；它包含内核、libc、bench 和必要 mutex；单候选可删量未测；P-B 的 0.24% 不可用。
- 不能说 M-61/M-62/M-65/M-67 任一候选 GO；它们只有阶段容器；#637 的 INCONCLUSIVE 未被本轮推翻。
- 不能说 M-53 在 d64 有 hotspot；d64 未测，且 A/A 分辨力边缘。
- 不能说 M-42 可测；`lock addq` 19–20 cycles 的 0.066–0.133% 低于匹配 uring 带；P-D/P-E 不适用。
- 不能说 io-wq punt 是跨 FS 成本；它只在 tmpfs uring 形态 1/op。
- 不能说设备 IRQ、磁盘、NVMe、cold、btrfs 写回的成本；本 profile 没有这些。
- 不能把 PMU `cycles:k` 与 record kernel 份额互相换算；不能把 off-CPU 事件数当时间占比。
- 不能把 W1/W3/M-53/M-42 的数字相加；它们是同一进程 cycles 的不同子集/切法。
- 不能把本 profile 当作“真实消费者 profile”门已过；`sluice-copy` 是参考应用，不是 G0 workload。

---

## 附录 A：证据路径、脚本入口与计算式

### A.1 原始数据

```
docs/evidence/621-progress-wait-path-20261118/p2/
  PROFILE-SUMMARY.md
  P2-COST-MAP.md                      # R0，历史版本（未修改）
  P2-COST-MAP-R1.md                   # 本文件
  P2-REVIEW.md                        # 独立复核
  profile/
    analysis/analysis.json            # R0 主数据源（sha256 5bc8e758…）
    analysis/analysis.txt
    analysis/analyze_p2.py            # 原分析器（sha256 b822ea3a…，未修改）
    analysis/recompute_p2_r1.py       # R1 证据生成器（sha256 0b148621…）
    analysis/SHA256SUMS
    env/snapshot-before.txt, snapshot-after.txt, symbol-twin-verification.txt
    env/symbols-bench_w02.txt, symbols-bench_w03.txt, symbols-sluice-copy.txt
    P-A-w02-uring-4k-d16/raw/
    P-B-w02-uring-4k-d1/raw/
    P-C-w03-uring-4k-d16/raw/
    P-D-w02-tp-4k-d16-w4/raw/
    P-E-sluice-copy-4k-d4-w1/raw/
```

每个 `raw/` 的关键文件：

- `stat-pmu-r3.txt`、`stat-mem-r3.txt`：PMU（`percent_enabled` 100%）。
- `stat-tp-*.txt`：tracepoint 计数；`stat-tp-block-io.txt` 的 `sys_enter_poll` 用于 poll/op。
- `stat-uprobe-*.txt`：uprobe 计数（只用于调用计数，不用于性能）；P-B `p621_wait_one` 60,000/60,000；P-E `e_wait_one` 73,524/65,536。
- `record-cycles.data`、`record-offcpu.data`：perf 原始数据。
- `script-cycles-sym.txt`：R1 leaf/stage/comm 复算输入。
- `script-offcpu-sym.txt`：off-CPU 阻塞点。
- `report-cycles-dso-symbol.txt`：未过滤的 perf report。
- `header-record-*.txt`、`lost-record-*.txt`：采样时长/丢失回执（15/15 Lost=0）。

### A.2 复算脚本入口

```bash
cd docs/evidence/621-progress-wait-path-20261118/p2/profile/analysis
python3 -I recompute_p2_r1.py --json-out /tmp/p2-r1.json
```

脚本字段与本文数字的对应：

| 本文数字 | 脚本字段 | 计算式 |
|---|---|---|
| stage_self R1 | `forms[form].stage_r1.{bucket}.count/share` | `count / samples` |
| poll boundary | `forms[form].poll_boundary.count/share/from_stage` | `count / samples`；from_stage 为 R0 `base.attribute()` 的分布 |
| `__poll` / `__x64_sys_poll` / `do_sys_poll` / `poll_schedule_timeout` | `forms[form].inclusive.*` | `count / samples` |
| `poll_progress` union | `forms[form].unions.poll_progress` | `union / samples`；`both` 为同时出现两个重载 |
| `eventfd_signal` union | `forms[form].unions.eventfd_signal` | `union / samples`；`both` 为 mask 在 io_eventfd_signal 内 |
| `wait_one` union | `forms[form].unions.wait_one` | `union / samples`；`both` 为两个重载同时出现 |
| app/no-bench 帧 | `forms[form].app_frames` | `no_bench_frame / app` |
| `wait_one` / poll 无 wait_one / 4 帧 poll | `forms[form].wait_path` | 计数 |
| W3 族 | `forms[form].w3_family` | admission+terminal+publication+observation |
| M-62 publication/claim | `forms[form].publication`、`claim_execution` | publication stage；claim leaf 的 stage 分布 |
| M-53 | `forms[form].m53_router` | `leaf / samples`；`inclusive / samples` |
| M-42 | `m42.forms[form]` | `19/cycles_per_op×100`、`20/cycles_per_op×100`、`1/instructions_per_op×100` |
| A/A | `aa.cells[cell].metrics` | 转录 `p1/AA-CALIBRATION*.md`；`n80/n_half` 用 t 公式 |

### A.3 关键计算式（便于手算）

- **poll 相关 inclusive**：`count(sample has __poll frame) / samples`。P-B `6,129/24,585 = 0.24930`；P-E `5,500/34,736 = 0.15834`。
- **libc-poll-only**：`count(has __poll and not has do_poll|__x64_sys_poll|poll_schedule|do_sys_poll) / samples`。P-B `2,251/24,585 = 0.09156`；P-E `1,520/34,736 = 0.04376`。
- **union**：`count(any nested symbol) / samples`，不是 `sum(individual inclusive)`。P-C `2,205/16,472 = 0.13386`；P-B `2,132/24,585 = 0.08672`；P-E `10,759/34,736 = 0.30974`。
- **M-62**：`publication stage self`；`claim_execution` 叶样本已在该阶段，不能再加。P-D `627/15,182 = 0.04130`。
- **M-42**：`19 or 20 / cycles_per_op × 100`。P-A `19/15,090.0×100 = 0.1259%`、`20/15,090.0×100 = 0.1325%`；P-B `0.0662–0.0697%`；P-C `0.0935–0.0984%`。`1/instructions_per_op×100`：P-A `1/10,652.9×100 = 0.00939%`、P-B `0.00413%`、P-C `0.00788%`。
- **A/A 1.2% 对**：`(t_.975,n−1 + t_.80,n−1)·SD/√n ≤ δ`（n80）；`t_.975,n−1·SD/√n ≤ δ/2`（n_half）。C6 cycles SD 2.876 → 48/91；C1 cycles SD 2.346 → 32/62；C6 CPU SD 0.997 → 8/14；C1 CPU SD 0.786 → 6/10。

### A.4 本会话执行的检查

| 检查 | 命令 | 结果 |
|---|---|---|
| 全量复算 | `python3 -I recompute_p2_r1.py --json-out /tmp/p2-r1.json` | exit 0；5 forms；见 `/tmp/p2-r1.json` |
| 原始分析哈希 | `cd p2/profile/analysis && sha256sum -c SHA256SUMS` | `analysis.json`/`analysis.txt`/`analyze_p2.py` **OK**；`SHA256SUMS` 自哈希 **FAILED**（既有 stale，见 §0.3-D） |
| R1 脚本哈希 | `sha256sum recompute_p2_r1.py` | `0b148621786ffb8d75de33e80f72be008deaad11bf481dc85e3565f1b64e1d11` |
| M-42 指令 | `objdump -d --start-address=0x424190 --stop-address=0x424230 /tmp/p2-621/symtwin/bench_w02_pipeline.sym` | `4241b7: ... lock addq $0x1,0x120(%rdi)` |
| uring submit 计数 | `grep io_uring_submit_req P-*/raw/stat-tp-uring.txt` | P-A/B/C 540,028；P-D 1（启动残差）；P-E 0 |
| poll 计数 | `grep sys_enter_poll P-*/raw/stat-tp-block-io.txt` | P-B 539,964；P-E 86,309；P-A/P-C 0 |
| wait_one uprobe | `grep -E 'p621_wait_one|e_wait_one' P-*/raw/stat-uprobe-*.txt` | P-B 60,000/60,000；P-E 73,524/65,536 |
| 未执行 | perf / bench / test / A/A / A/B | **未运行** |

### A.5 复现限制

- `recompute_p2_r1.py` 依赖 `/tmp/p2-621/kallsyms.txt` 与 `/tmp/p2-621/symtwin/*.sym`；这些是会话内文件，不是仓库内容。若不存在，需按 `PROFILE-SUMMARY.md` §0.2/§13 重建；脚本已提供 `--kallsyms`/`--twins-dir`。
- 原 `analyze_p2.py` 未修改；R1 的修正函数是 `recompute_p2_r1.py` 中的 `syscall_owner_r1()`、`first_stage_r1()`、`attribute_r1_dedicated()`。若未来要替换 `analyze_p2.py`，必须重新生成 `analysis.json` 并更新 `SHA256SUMS`，不能只改脚本。
- 行级归因仍 NOT_OBSERVABLE；本文件只给符号级/阶段级份额。
- 任何引用本图的数字，请同时给出形态、分母（540,000 ops / 65,536 chunks）和“份额 ≠ 可删 cycles / 阶段容器 ≠ 候选上界”的口径。

---

*本文件结束。*
