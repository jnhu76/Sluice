# C3 内核/io-wq 深挖：syscall 面差分与 eventfd 路径成本（Sluice #595 causal-r2）

状态：ACTIVE EVIDENCE。轮次：causal-r2 C3（2026-11-04）。
范围：D2（`W02-uring-4k-d2`，4 KiB positional read，depth=2，warm tmpfs）
A·IMMEDIATE vs B·BOUNDED_PROGRESS。
配套 trace 观测：`evidence/traces/c3-kernel/README.md`（io-wq 画像、strace、
liburing 行为学探针、UNOBSERVED 清单）。Phase B 成本表：`counters/cost_table.md`。

## 0. 结论先行（直接测量 / 推断 / 未知）

**直接测量（两个独立块，互为印证）**

1. **B 专属新增内核 syscall 只有一个：`write(eventfd)`**。
   - 内核记账（无仪器，frozen 二进制，12 对）：bench 主线程 `syscw` B−A =
     **+86,912 [86,541, 87,263] / invocation**（全程 120k op）；`wchar` 差 ÷ `syscw` 差
     = **8.005 字节/次**——全部是 8 字节 eventfd write，无一例外。
   - 探针计数（C3 二进制 ON，measured-only，12 对）：write **0 → 0.24078/op**
     [+0.23858, +0.24289]（与 Phase B `wakeup_writes` 0.2353/op、C1 0.230/op 一致）。
2. **read/poll/drain 面两臂同构且 B 略少**（探针，measured-only，12 对）：
   `drain_park`（=每次 park 前 drain）0.2480 → 0.2408/op（**−0.00718 [−0.01034, −0.00375]**）；
   `read_ok`（成功 read）、`read_eagain`、`park_poll` 同步各 −0.00718/op；
   `drain_ack=0`（两臂，稳态 acknowledge 路径不执行）；poll 返回值 100% POLLIN、
   0 超时、0 EINTR（两臂）。内核记账全程口径 `syscr` B−A = −1,051 [−2,488, +329]
   （CI 含 0）与之一致。**B 的内核增量不含 read/poll syscall 面的正贡献。**
3. **eventfd/park 路径的 on-CPU 成本（探针 TSC，measured-only，12 对配对差）**：
   | 路径 | 次数 B−A /op | 单次成本（clean） | B−A ns/op |
   |---|---|---|---|
   | write(eventfd) | +0.24078 | B 562.7 ns/call（C1 同点位 592.5） | **+138.25 raw [+136.8,+139.8]**；clean +135.2 |
   | drain read 成功 | −0.00718 | A 557.6 / B 539.1 ns/call | −8.82 [−11.78,−5.89] |
   | drain read EAGAIN | −0.00718 | A 488.0 / B 486.2 ns/call | −4.21 [−5.98,−2.45] |
   | poll(2)（睡眠含在内，clean 子集空） | −0.00717 | raw mean ~6.3 µs（睡眠主导，非 CPU 口径） | 次数差×入口成本 ≈ −3.6（下界推断，未直测） |
   | 合计 eventfd 面 | — | — | **≈ +125 ns/op [≈+118, +132]** |
4. **每次 drain 的 counter 值**：A 4.034 → B 5.154（+1.12/drain）。B 每个 drain 多读
   的信号数 = write/drain 比 1.000，逐项闭环（fd 同时被内核 CQE 完成信号与 B 的用户态
   write 写入，见 `traces/c3-kernel/README.md` §1）。
5. **io-wq 不承载任何 B 专属成本**（复核 Phase B + 新增内核记账）：
   iou-wrk 线程 `syscr`/`syscw` **A/B 两臂 12/12 invocations 恒 0**（io-wq 无
   read/write syscall）；内核时间 1,771.8→1,779.8 ns/op（+0.46% [−0.44,+1.36]，n.s.，
   Phase B split）；futex 全程 2 次（进程级，两臂同）。io-wq work 内容 UNOBSERVED
   （root-only tracepoint），但"缓冲读被强制走 io-wq"假说被行为学探针排除
   （tmpfs/ext4 warm 4k read 99.6–100% 在 submit 内完成，`traces/c3-kernel/` §2）。
6. **内核增量的对账**：B 专属 eventfd write 单项 on-CPU +138 ns/op（raw）已超过
   Phase B 提交线程内核净增量 +79.37 [37.8, 124.6] ns/op（`/proc` stime，10 ms tick）；
   加上 read 面的 −13 ns/op，eventfd 面 ≈ +125 ns/op。中心值差（~+46 ns/op）是 TSC
   on-CPU 口径与 tick 记账口径的系统差方向（特权级切换时间的归属、tick 落点），不是
   未解释内核路径的证据：所有其他内核面（read、poll、enter、io-wq、futex）的 B−A
   均为 0 或负。**结论：B 的内核 CPU 增量可由 eventfd write 单路径覆盖；不需要
   io-wq、completion、调度或锁路径的额外贡献。**
7. **探针开销（同二进制 env 门控 ON/OFF，12 对独立组 bootstrap）**：CPU/op A +0.93%
   [−1.34,+2.96]、B +0.57% [−0.14,+1.35]（均 CI 含 0，约为 D2 效应的 16–25%）；
   吞吐 A −0.75% [−2.79,+1.67]、B −0.50% [−1.27,+0.19]；p95/p99 CI 全含 0；
   用户态 +2.6~3.1%（显著，TSC 探针在用户态执行）、**内核态 +0.18%/−0.16%
   （CI 含 0——内核时长读数不受探针开销污染）**。探针校正后效应 cpu +3.204% vs
   OFF +3.178% 一致。ON 块用于次数与单次成本；定量效应仍取 OFF/Phase B。
8. 本块 OFF 效应：CPU/op B−A +3.178% [+0.49,+4.83]、吞吐 −2.92%、p99 +6.32%
   （与 Phase B +3.70% [2.93,4.55] 及 C1 块 +4.24% 在块间漂移带内一致）。

**推断（非直接测量）**

- write 的 574/562.7 ns/call 中用户态 stub 仅 ~3.7 ns（C1 trace `__GI___libc_write`
  8.5 cycles/op 折算），其余为特权级切换 + 内核 eventfd 路径（fd 查找/计数加法/
  waitqueue 操作）——内部阶段拆分 UNOBSERVED（无 :k PMU / fentry）。
- poll 次数差的入口成本 ≈ −3.6 ns/op 是"次数差 × ~500 ns"的下界推断，非直测。

**未知 / 不可观测**

- `cycles:k`/`instructions:k`、内核调用图、io_uring/workqueue tracepoints、
  ftrace/kprobe：root-only（`traces/c3-kernel/README.md` §5）。
- io-wq 的 1.78 µs/op 内核时间在做什么（work 内容）：UNOBSERVED。
- write 期间特权级切换时间在 utime/stime 的精确分割：不可观测。

## 1. 方法与实验设计

### 1.1 两块设计（不同 instrumentation 不叠加）

| 块 | 二进制 | instrumentation | 结构 |
|---|---|---|---|
| `sysio-d2` | `bench_w02_pipeline_frozen`（sha256 `61c6d579…`，与 phaseA 冻结基线相同，零探针） | 无（外部 1 ms 采样器读 `/proc/<pid>/task/<tid>/{stat,io}`，不注入 bench） | 12 对 A/B 交错（偶对 A→B，奇对 B→A），warmup=1 measured=5，每 invocation 100k measured op + 20k 非 measured op，cpu2 |
| `c3-interleaved-d2` | `bench_w02_pipeline_c3`（sha256 `17463539…`，SLUICE_595_C3_COUNTERS 编译） | 源码 TSC 聚合计数器（env `SLUICE_595_C3=1` 门控，同二进制 ON/OFF） | 12 对 × 4 invocation（off-A,off-B,on-B,on-A / 奇偶镜像，C1 同款交错），其余同上 |

两块各自独占机器串行运行（loadavg 起点 0.06/0.24）；采样器 error 计数与样本数记录
在 results.jsonl。/proc 采样窗口=invocation 全程（含非 measured pass），故 /proc 口径
只用于次数差与线程归集；measured-only 密度一律取探针块。

### 1.2 C3 探针点（研究副本 `causal-r2/worktree`）

| 探针 | 位置（progress_source.hpp） | 语义 |
|---|---|---|
| `drain_park` count | `wait_if_unchanged` 的 park 前 drain 调用处 | park 路径 drain 次数 |
| `drain_ack` count | `acknowledge_notification()` | acknowledge 路径 drain 次数 |
| `read_ok` Agg | `drain_notification_nolock_()` 内成功 `::read` | 单次 on-CPU 时长 + 次数 + counter 值累计 |
| `read_eagain` Agg | 同上 EAGAIN 分支 | 同上 |
| `park_poll` Agg | `::poll(pfds,1,timeout)` | 时长含睡眠（clean 阈值 1.04 µs 内子集为空，raw 不作 CPU 口径）+ 返回分类（pollin/timeout/eintr/other） |
| `write_fd` Agg | `wake_notification_nolock_()` 的 `::write(eventfd,1)`（与 C1 同点位复制，用于同块对齐） | B 专属 write |

计数头 `include/sluice/async/detail/research_595_c3.hpp`（结构与 C1 相同：
thread_local 非原子 Entry{count,tsc_sum,max,clean}, `lfence;rdtsc`/`rdtscp;lfence`，
TSC 自校准 2,893,269–2,893,289 kHz，空 pair 29–38 ticks）。dump 前缀 `SLUICE595_C3 `，
只在 measured 吞吐 pass 使能（与 C1 同一接线锚点）。完整 diff：
`traces/c3-kernel/probe-source-diff.txt`。

工具：`tools/run_c3_blocks.py`（运行器，C1 框架改 env/prefix）、`tools/analyze_c3.py`
（分析器）、`tools/run_sysio_blocks.py`（sysio 块）。原始数据：`raw/c3-interleaved-d2/`、
`raw/sysio-d2/`；分析 JSON：`analysis/c3-interleaved-d2.json`。

## 2. 块 c3-interleaved-d2 结果（12 对，measured-only）

### 2.1 syscall/park 面次数（per measured op）

| 计数 | A (on) | B (on) | B−A 配对差 |
|---|---|---|---|
| drain_park | 0.2480 | 0.2410 | −0.00718 [−0.01034,−0.00375] |
| read_ok | 0.24794 | 0.24076 | −0.00718 [−0.01034,−0.00375] |
| read_eagain | 0.24798 | 0.24080 | −0.00718 [−0.01034,−0.00375] |
| park_poll | 0.24797 | 0.24080 | −0.00717 [−0.01033,−0.00374] |
| **write_fd** | **0.00000** | **0.24078** | **+0.24078 [+0.23858,+0.24289]** |
| drain_ack | 0 | 0 | 0 |
| poll_wake_pollin / timeout / eintr | 0.248 / 0 / 0 | 0.241 / 0 / 0 | poll 100% 被 POLLIN 唤醒（两臂） |
| read_value_mean（每次 drain 读到的信号数） | 4.034 | 5.154 | +1.12 ≈ write/drain 比 1.000 ✓ |

每次 drain 恒等于 1 次成功 read + 1 次 EAGAIN read（read_ok≈read_eagain≈drain），
与 drain 循环源码语义一致。B 的 park/drain/read 次数少 2.9%（B 的 flush 迁移改变了
pass 结构），read 面净省 −13.03 ns/op。

### 2.2 单次成本与 per-op 贡献（配对差）

| 量 | A | B | B−A |
|---|---|---|---|
| write_fd ns/call（raw / clean） | —（0 次） | 574.2 / 562.7 | — |
| write_fd ns/op（raw） | 0 | 138.2 | **+138.25 [+136.78,+139.84]** |
| write_fd clean fraction | — | 0.998 | — |
| read_ok ns/call（raw / clean） | 571.8 / 557.6 | 552.3 / 539.1 | raw −3.36% [−4.60,−2.13] |
| read_eagain ns/call（raw / clean） | 499.7 / 488.0 | 497.1 / 486.2 | −0.50% [−1.09,+0.06] |
| read 面合计 ns/op | 265.7 | 252.7 | **−13.03 [−17.61,−8.45]** |
| park_poll ns/call（raw，含睡眠） | 6,358 | 6,324 | −0.44% [−2.55,+1.47]（非 CPU 口径） |
| tsc_hz | 2.893 GHz | 同 | 空对 29–38 ticks |

### 2.3 效应与探针开销（同二进制 ON/OFF）

| 对比 | CPU/op | 吞吐 | p95 | p99 |
|---|---|---|---|---|
| OFF B vs A | +3.178% [+0.49,+4.83] | −2.92% [−4.62,−0.07] | +3.23% [−1.69,+6.02] | +6.32% [−0.54,+10.63] |
| ON B vs A | +2.836% [+1.05,+4.54] | −2.66% [−4.30,−0.89] | +3.78% [+0.13,+6.13] | +8.19% [+4.08,+11.57] |
| 探针 A：ON vs OFF | +0.93% [−1.34,+2.96] | −0.75% [−2.79,+1.67] | −0.01% [−6.08,+5.67] | +0.50% [−7.11,+7.48] |
| 探针 B：ON vs OFF | +0.57% [−0.14,+1.35] | −0.50% [−1.27,+0.19] | +0.17% [−0.35,+0.73] | +1.80% [−0.18,+3.71] |
| 用户态探针 A/B | +3.08% [+0.42,+5.75] / +2.59% [+0.18,+5.13] | | | |
| 内核态探针 A/B | +0.18% [−3.74,+3.44] / −0.16% [−1.92,+1.82] | | | |
| 探针校正效应 cpu | raw +2.836% → corrected +3.204%（vs OFF +3.178%） | | | |

48/48 invocations rc=0。探针开销显著小于效应（16–25%），且内核时长读数不受
探针污染（内核态 ON/OFF 差 CI 含 0）；仍按纪律：ON 块只用于次数/单次成本，
定量效应取 OFF 与 Phase B。

## 3. 块 sysio-d2 结果（无仪器，frozen 二进制，12 对，/proc io 内核记账）

配对差（B−A，全程 invocation，120k op = 100k measured + 20k 非 measured）：

| 计数 | B−A | 说明 |
|---|---|---|
| bench syscw | **+86,912 [86,541, 87,263]** | A 恒 70/invocation（进程级固定），B 86,751–88,055 |
| bench wchar | +695,716 [692,065, 699,089] | **wchar/syscw 差比 = 8.005**——全部 8 字节 eventfd write |
| bench syscr | −1,051 [−2,488, +329]（CI 含 0） | read 次数无 B 专属增量 |
| bench rchar | −4,201 [−9,948, +1,318] | ≈ 8 × syscr 差的一半（read_ok 与 read_eagain 各 8 字节） |
| iowq syscr/syscw | **0（12/12 invocations，min=max=0）** | **io-wq 线程零 read/write syscall（内核记账，两臂）** |

全程 write 密度 0.869/op 高于 measured-only 0.24078/op：非 measured pass
（instrument/verify/oracle/stats，~20k op）的 B signal 密度 ~3.1/op（这些 pass 的
等待结构不同）。这不影响结论：用户 write 是唯一 B 专属 syscall；measured 密度以
探针块为准。syscr 的全程 CI 含 0 与探针块 measured 口径的 −0.0072/op 同向相容
（全程含非 measured pass 的 read 流量对齐两臂）。

## 4. io-wq：画像与归属（引用 + 本轮复核）

- 每 invocation 1 个 `iou-wrk-*` 内核线程（split-d2 与 sysio-d2 均 12/12）；
  park 点 `io_wq_worker`（wchan 采样，`traces/c3-kernel/iowq_samples.txt`）。
- **零 syscall 面**（本轮内核记账，§3）；零 futex（strace 全程 2 次，两臂同）。
- 内核时间 1,771.8→1,779.8 ns/op（+0.46% [−0.44,+1.36]，n.s.，Phase B split-d2）；
  唤醒率（voluntary ctxsw）D2 0.552→0.563/op（+1.99% [1.35,2.57]，显著但量小）、
  D1（机制关闭，`sqes_deferred=0`/`wakeups=0`）0.7175→0.7199/op（split-d1x 重算：
  A/B 等量）——io-wq 活动与 arm 无关、与 depth/等待节奏相关。
- work 内容 UNOBSERVED（root-only tracepoint）；"buffered read 强制走 io-wq"假说
  被排除（inline 探针 99.6–100%，`traces/c3-kernel/` §2）。
- **判定：io-wq 不构成 D2 的 B−A 成本**（Phase B 结论经本轮内核记账复核维持）；
  不得把 B 的内核增量统称 io-wq。

## 5. 内核路径分解总表（B−A，D2）

| 内核面 | B−A 次数/op | B−A ns/op | 证据 |
|---|---|---|---|
| write(eventfd)（用户态发起，B 专属） | +0.24078 | **+138.25 raw / +135.2 clean** | C3 探针 + syscw/wchar 记账 |
| drain read（成功+EAGAIN） | −0.01436 | **−13.03 [−17.61,−8.45]** | C3 探针 + syscr 记账（CI 含 0 同向） |
| poll(2) | −0.00717 | ≈−3.6（下界推断，未直测） | C3 探针次数；时长含睡眠 |
| io_uring_enter | 0（1.0000/op 两臂） | ~0（C2：submit 时长差 CI 含 0） | C2 双仪器 |
| io-wq work（内核内部） | — | +7.94 [−7.7,+23.6]（n.s.） | Phase B /proc stime |
| futex（用户锁慢路径） | 0（两臂 2/进程） | 0 | strace（掩蔽下仍为进程级常量）；单线程执行（progress_handoffs=0）结构性无竞争 |
| completion（CQE 收割） | peek 用户态 ring，无 syscall 差异 | 0 | C2（enter 全 submit-only） |
| **提交线程内核净增量（tick 口径）** | — | **+79.37 [37.8, 124.6]**（Phase B split-d2） | /proc stime |
| **eventfd 面合计（TSC 口径）** | — | **≈+125 [≈+118, +132]** | 本轮 |

对账：write 单项 +138 > tick 净增量上限 +124.6；read 面再省 13。两种口径中心差
~46 ns/op 的候选归属：特权级切换时间在 utime/stime 的记账分割（不可观测）、
poll 入口固定成本差、C5 微架构对内核路径的影响。**没有需要 io-wq/completion/
调度/锁路径贡献的证据；它们全部测得 0 或负。**

## 6. 限制

- /proc io 与 stat 采样为 1 ms 轮询取单调末值（sysio 块 samples≈1,300/invocation，
  errors ≤ 个位数，记录于 results.jsonl）；io 文件轮询可能错过短命线程的中间值
  （iou-wrk 线程均存活至进程结束，末值可靠）。
- TSC 区间含极少量抢占样本：read_ok/read_eagain/write_fd 的 clean fraction
  0.996–0.998，>1.04 µs 尾部已分离统计；park_poll 的 clean 子集为空（睡眠主导），
  其 raw 均值只作 wall 参考不作 CPU 口径。
- sysio 块 /proc 采样窗口含非 measured pass，其 per-op 密度只作全程对账；
  measured-only 密度全部取探针块。
- strace 类观测有行为掩蔽（C2 已证），本分支只用其"进程级无其他 syscall 面"
  的排除性结论，不用其密度与时序。
