# Sluice #595 causal-r2 深度边界机制分析（D1/D2/D3/D16）

状态：ACTIVE EVIDENCE。轮次：causal-r2 深度边界分析（2026-11-05，E5）。
范围：`W02-uring-4k`（4 KiB positional read，warm tmpfs）在 depth = 1/2/3/16 下
A·IMMEDIATE 与 B·BOUNDED_PROGRESS 的机制差异；目标是解释为什么 D2 没有 enter 摊销、
D3 已出现净改善、D16 改善更大，以及 D2/D3 在提交时是否存在可区分状态。

数据源：
- Phase A 原始：`/home/jnhu/sluice-595-e5/causal-r2/evidence/raw/phaseA`（冻结二进制，无仪器，24 对/深度）。
- 成本表：`/home/jnhu/sluice-595-e5/causal-r2/evidence/counters/cost_table.md`（D2 基准）。
- 分支发现 C1–C4、反事实 CF-0/CF-1/CF-2（任务给定文本；原始证据在 `evidence/counters`、`evidence/counterfactuals`、`evidence/counterfactuals/three_arm`）。
- 本轮补测（全部新产物在 `evidence/analysis/depth-analysis/`，见 §9）。

研究纪律：只归因、不修复；未改公开 API/v2 契约；主工作树 `/home/jnhu/Source/Sluice`
未动；R1 证据与预注册门槛只读；不同 instrumentation 不同块、不叠加进同一次 timing run；
所有新运行串行、独占 cpu2。

---

## 0. 结论先行

**直接测量（计数器/调用序列/syscall 参数/PMU/线程 CPU）**

1. **D2 的 enter 摊销为零，且是精确零**：12 对计数器（同构建 b9a49946…，各 100k measured op）
   `flushes = actual_enters = 1.0000/op` 两臂相同、`sqe_preparations/actual_enters = 1.000`；
   trace 构建逐事件直方图（20,000 dispatch/phase，3 phase 一致）显示 **每一次 flush 恰好携带 1 个 SQE**
   （flush size 直方图 `{1: 20000}`）。B 每 op 延迟 0.5589 个 SQE，但每次都被下一次 poll 单独冲掉。
2. **D3 出现真摊销**：`actual_enters = 0.6905/op`（−30.95% [−31.02,−30.87]），
   `SQE/enter = 1.448`；trace flush size `{1: 7128, 2: 6436}`，即 47.5% 的 flush 携带 2 个 SQE
   （32.2% 的 SQE 与另一个 SQE 共享 enter）；
   strace `to_submit` 直方图 `{2: 39991, 1: 40018}`（120k op 窗口）独立复核。
3. **D16 摊销约 5 倍**：`actual_enters = 0.1974/op`（−80.26%），`SQE/enter = 5.066`；
   trace flush size `{1: 1343, 7: 1166, 8: 1311}`，defer 决策在 ledger=1..7 单调填充，
   **cap=8 在 6.4% 的 dispatch 上强制 immediate flush**（C1 门计数：witness pass 94.1% vs defer 87.7%）。
4. **改善随深度单调反转**（phaseA，无仪器，24 对/深度）：CPU/op D1 −0.07% [−0.73,+0.63]（中性）、
   D2 **+3.73%** [3.04,4.42]、D3 **−6.69%** [−7.25,−6.18]、D16 **−26.36%** [−26.66,−26.08]；
   p95：+4.96% / −11.38% / −31.00%；p99：+7.07% / −9.57% / −28.68%。
5. **每避免一次 enter 的内核节省约 625 ns，跨深度一致**：D3 split 块 submit-thread 内核
   −194.4 ns/op ÷ 0.3095 saved enters/op = 628 ns/enter；D16 −500.0 ÷ 0.8026 = 623 ns/enter。
6. **通知成本随深度塌缩**：eventfd write 发生率 0.2353/op（D2）、0.1862（D3）、0.0540（D16）；
   write/wakeup = 42.1% → 27.0% → 6.2%（深度越高，多个 defer 共享同一次 signal）。
7. **D2/D3 在提交时可区分**：trace 逐 dispatch 观测，**D2 有 0/20,000 个 dispatch 发生在
   ledger 非空状态下**；D3 为 6,436/20,000（32.2%，全部 ledger=1）；D16 为 80.9%。
   后端在 `dispatch_mtx_` 下可直接观察 `transport_ledger_->size()` 与 witness margin。

**反事实支持（单因素干预）**

8. 既有 CF-2（D2）：`defer_transport_flush_locked_` 入口 return false 后 B−A CPU 由 +3.90% 归零到
   −0.76% [−1.61,+0.06]。本轮把同一变异二进制用于 **D3/D16（各 8 对，同二进制 ABBA，
   400k measured op/invocation）**：D3 B−A = **+1.07% [−0.47,+2.80]**、D16 = **+0.26% [−0.69,+1.04]**
   （CPU CI 均含 0；延迟侧 D3 p50/p95 n.s.、p99 +4.60% [+0.83,+8.98] 的小残差，
   D16 p50/p95/p99 全部 n.s.）。⇒ D3 的 −6.69% 与 D16 的 −26.36% 在
   defer 动作被禁用后**完全消失**；结合机制计数器，收益由 defer→批量 flush 承担。

**未知 / 观测边界**

9. D3/D16 的逐 SQE submit delay 只在 trace 构建（单仪器，扰动 ~0.7 µs/op）下测得；
   native 的绝对延迟未测（D2 有 C2 的原生直测：29.93% 的 submit 在 <250 ns 内跟随 peek）。
10. 内核态 PMU（`cycles:k`/`instructions:k`）不可用（paranoid=2，无 root）；内核 CPU 为
    `/proc` 10 ms tick 口径。
11. D3/D16 的 runnable delay/off-CPU 调度量未测（只有 ctxsw 计数与 C4 的 D2 schedstat）。
12. io-wq 内核 CPU 随 enter 减少而下降（D3 −6.4%、D16 −17.2%），机制未观测（UNOBSERVED），
    只作为测量到的伴随收益记录。

---

## 1. 机制基线（源码）

### 1.1 提交路径与 defer 门链

`dispatch_after_accept`（`src/async/uring_transport.cpp:46-126`）在持 `dispatch_mtx_` 下
把请求写入 SQE、append 到 `transport_ledger_`，然后：

- `defer_transport_flush_locked_()`（`:246-294`）依次检查：
  1. `bounded_progress_`（仅 B 臂，`:254`）；
  2. owner 门：调用的线程必须是当前 progress driver（`:260`）；
  3. payload 门：op 必须是 4096B read（`:267-268`）；
  4. **witness 门：`live_cookies_ > transport_ledger_->size()`**（`:277`），
     即“至少有一个已 dispatch 且在核内、尚未 retire 的请求”；
  5. cap 门：`transport_ledger_->size() < kDeferCap`（`:284`，`kDeferCap = 8`，
     `include/sluice/async/detail/uring_backend.hpp:305`）。
- 通过 → 记 `sqes_deferred`（`:292`），**不 submit**；不通过 → `submit_transport_locked()`（`:103`）。
- 每次 defer 后 `advertise_residue = … || unsubmitted_after_dispatch`（`:120-121`）为真，
  调 `signal_ready_progress()`（`:123-125`）。

`submit_transport_locked()`（`:296-321`）只在 ledger 非空时真正 flush（`:299-302`，`flushes`），
并调用一次 `::io_uring_submit`（`:316-317`，`actual_enters`）。**ledger 是所有未提交 SQE 的唯一队列**，
所以一次 enter 携带的 SQE 数 = flush 时刻 ledger 的大小。

### 1.2 poll 的顺序：先无条件 flush，再 reap

`UringAsyncBackend::poll()`（`src/async/uring_backend.cpp:634-658`）：

```
640  lock dispatch_mtx_
645-648  drain dispatch_ queue (dispatch_one_locked)
651-653  (void)submit_transport_locked();      // 无条件 flush 整个 ledger
657      const std::size_t reaped = reap_cqes();
```

即：**任何一次 progress pass 都会先把 pending ledger 全部提交，然后才收割 CQE**。
tmpfs 4 KiB buffered read 在 submit 内联完成（C3：warm tmpfs/ext4 4 KiB read 99.6–100%
在 submit 返回时 CQE 已就绪；`giveup` 0.10–0.37%），因此“flush 后紧接的 reap”会在同一 pass
内收割刚提交的 SQE 的完成，`live_cookies_` 归零。

### 1.3 负载循环：窗口满才 wait

`bench/bench_w02_pipeline.cpp:384-448`：

```
385-410   while window.size() < depth && submitted < N: submit_read(); window.push
419-448   若队首未 ready → wait_for(队首)（进入 poll）；否则直接 take
```

稳态每轮：take 1 个 → 补 1 个 submit → 再 consume 1 个。**只有在“队首不 ready”时才会 wait/poll**；
如果上一轮 poll published 了多个完成，bench 可以在若干轮内连续 take 而不 poll——这正是
ledger 能跨多次 dispatch 存活、从而合并 enter 的窗口。

---

## 2. 跨深度总表（每 measured op）

计数器块：同一二进制 `bench_w02_pipeline_counters` sha256 `b9a49946…`，A/B 各 12 对
（D1x/D2 为既有块，D3/D16 为本轮新块），warmup=1、measured=5、20k op/rep；
timing/PMU/split 块使用无仪器二进制。D2 的用户/内核分解取 `split-d2`（成本表 §4），
D3/D16 取本轮 `split-d3`/`split-d16`（PMU-only+reuse fixture，21×20k op/invocation，12 对）。

| 指标 | D1 | D2 | D3 | D16 |
|---|---|---|---|---|
| nominal depth / 实测 max outstanding（phaseA） | 1 / 1 | 2 / 2 | 3 / 3 | 16 / 16 |
| `dispatch_passes` | 1.0000 | 1.0000 | 1.0000 | 1.0000 |
| `sqes_deferred`/op | 0 | **0.5589** | **0.6904** | **0.8762** |
| `flushes` = `actual_enters`/op | 1.0000 | **1.0000** | **0.6905** | **0.1974** |
| **SQE per actual submission** | 1.000 | **1.000** | **1.448** | **5.066** |
| trace flush-size 直方图（20k dispatch，phase 1） | {1:20000} | **{1:20000}** | **{1:7128, 2:6436}** | **{1:1343, 7:1166, 8:1311}** |
| dispatch 时 ledger 非空占比（trace） | — | **0.0%** | **32.2%** | **80.9%** |
| ledger 时间加权占用（trace，SQE） | — | 0.133 | 0.251 | 1.442 |
| defer→flush delay p50/p90/p99（trace，ns；phase 1） | n/a | 617 / 1234 / 2777 | 706 / 3210 / 5419 | 8363 / 14462 / 18919 |
| `poll_passes`/op | 1.3419 | 0.7942 | 0.5672 | 0.1277 |
| `wait_attempts`/op | 1.0000 | 0.5590 | 0.3810 | 0.0737 |
| `empty_polls`/op | 0.3419 | 0.2353 | 0.1862 | 0.0540 |
| `wakeups`（signal）/op | 0 | 0.5589 | 0.6904 | 0.8762 |
| eventfd `wakeup_writes`/op | 0 | 0.2353 | 0.1862 | 0.0540 |
| writes / wakeups | — | 42.1% | 27.0% | 6.2% |
| `submit_attempts`/op | 2.3419 | 1.2354 | 0.8768 | 0.2515 |
| phaseA CPU/op Δ（B−A，24 对） | −0.07% | **+3.73%** | **−6.69%** | **−26.36%** |
| phaseA 吞吐 Δ | +0.10% | −3.58% | +7.20% | +35.88% |
| phaseA p50 / p95 / p99 Δ | −0.72/−0.06/−0.14% | +6.91/+4.96/+7.07% | −3.07/−11.38/−9.57% | −32.31/−31.00/−28.68% |
| split：提交线程用户 CPU Δ | — | +95.2 ns（+7.39%） | −59.5 ns（−4.74%） | −285.7 ns（−29.42%） |
| split：提交线程内核 CPU Δ | — | +79.4 ns（+3.99%） | −194.4 ns（−11.00%） | −500.0 ns（−49.89%） |
| split：io-wq 内核 CPU Δ | — | +7.9 ns（+0.46%，n.s.） | −103.2 ns（−6.39%） | −208.3 ns（−17.24%） |
| `instructions:u`/op Δ（perf，`u` only） | — | +1.06% [0.97,1.16] | −0.50% [−0.61,−0.38] | −0.02% [−0.06,+0.03] |
| `cycles:u`/op Δ | — | +5.71% [4.55,6.87] | +1.97% [−1.65,+7.55] | −6.45% [−11.77,−1.25] |
| 用户态 IPC Δ | — | −4.39% | −2.40% | +7.31% |
| voluntary / nonvoluntary ctxsw Δ | — | −0.40% / +0.36%（C4） | −2.96% / −24.48% | +15.77% / −88.76% |
| deferoff 反事实 B−A CPU | — | −0.76% [−1.61,+0.06]（CF-2） | **+1.07% [−0.47,+2.80]** | **+0.26% [−0.69,+1.04]** |

读法：
- D1 是机制关闭的精确对照（`sqes_deferred=0`、`actual_enters=1.0`、CPU/延迟中性），
  排除计数器/arm 选择器本身的系统偏差。
- 从 D2 到 D16，`sqes_deferred` 只从 0.56 涨到 0.88（+57%），但 `actual_enters` 从
  1.0 塌到 0.197（−80%）：**收益不是来自“更愿意 defer”，而是来自“defer 能跨 poll 累积”**。
- D2 的 CPU 增量为正（+189 ns/op，成本表 §4），D3/D16 为负且量级随 enter 节省增长。

---

## 3. 为什么 D2 没有获得 enter amortization

### 3.1 直接测量

- 计数器（12 对，deterministic）：`flushes = actual_enters = 1.0000/op`，
  `sqe_preparations/actual_enters = 1.000`；B 的 `sqes_deferred = 0.5589/op`。
- trace（20k dispatch，3 phase 一致）：flush size 全部为 1（`{1: 20000}`），
  即 **从未有一次 enter 携带 2 个 SQE**；defer 决策全部发生在 ledger=1（append 后），
  immediate 决策全部发生在 ledger=1（append 前为空）；每个 flush 之后 0 个 defer
  （immediate flush，9386 次）或恰好 1 个 defer（poll flush，10614 次）。
- 逐 dispatch 状态：**0/20,000 个 dispatch 发生在 ledger 非空时**；ledger 非空的时间占比 13.3%
  （只在 defer 与紧随其后的 poll 之间）。

### 3.2 机制解释（源码 + 顺序）

1. depth=2 时，窗口填满（2 个 submit）后 bench 必须 consume；consume 先做纯 lookup
   （`AsyncIoContext::lookup`，`:404-415`，不 poll），队首未 ready 时进入 `wait_one`
   → `run_progress_pass_` → `poll()`。
2. `poll()` **先无条件 flush ledger**（`uring_backend.cpp:651-653`），再 reap（`:657`）。
   被 defer 的那个 SQE 在这一次 poll 里被单独提交，其 tmpfs 完成在 submit 内联产生
   （C3：99.6–100% CQE 在 submit 返回时就绪），并**在同一个 pass 内被 reap**，
   `live_cookies_` 回到 0。
3. 下一次 dispatch 时 witness 条件 `live(0) > ledger(0)` 不成立（`uring_transport.cpp:277`）
   → 走 immediate 分支（`:103`），立刻 flush 这一个 SQE。
4. 由于 D2 的窗口只有 2，flush 之后 bench 最多只剩 1 个 in-flight 请求；下一次 consume
   很快触发 wait → poll → 再次 flush。**两次 dispatch 之间总有一次 poll**，所以 ledger
   永远在 0/1 之间振荡，不存在“两次 defer 落到同一个 flush”的机会。

trace 的周期结构直接印证：`immediate → defer → poll(flush 1) → immediate → defer → …`；
flush 之间 defer 数的直方图是 `{0: 9386, 1: 10614}`，**没有 ≥2 的桶**。

### 3.3 代价仍然支付

D2 的 defer 没有节省任何 enter，但完整支付了通知/门链成本：signal 0.5589/op
（write 0.2353 + coalesced 0.3236）、defer 门 1.0/op；CPU 分解
用户 +95.2 ns/op、内核 +79.4 ns/op、io-wq +7.9 ns/op（n.s.），合计 +189.1 ns/op，
p50/p95/p99 +6.91%/+4.96%/+7.07%。CF-2（deferoff）把该退化归零
（B−A −0.76% [−1.61,+0.06]），CF-1（sigdrop）证明其中通知部分（signal）是 CPU 增量的
主项、而延迟位移由 defer 时序（L1）承担。

结论（直接测量 + 反事实支持）：**D2 的“延迟 flush”只改变了同一 SQE 的提交时刻和位置
（dispatch → 下一次 poll），没有改变提交次数；摊销为零是窗口=2 与 poll 无条件 flush
顺序的结构性结果。**

---

## 4. 为什么 D3 已经出现改善

### 4.1 直接测量

- 计数器：`sqes_deferred = 0.6904/op`，`flushes = actual_enters = 0.6905/op`
  （−30.95% [−31.02,−30.87]），`SQE/enter = 1.448`。
- trace：flush size `{1: 7128, 2: 6436}`；32.2% 的 dispatch 发生在 ledger 非空时
  （全部 ledger=1）；周期为 `immediate → defer → defer → poll(flush 2)`（flush 之间
  defer 数直方图 `{0: 6439, 2: 6436, 1: 689}`）。
- C1 门计数（D3 B，20k op，3 次独立运行一致）：`defer_owner_pass=19997`（99.99%）、
  `defer_witness_pass=13757–13834`（68.8–69.2%）≈ 最终 defer 率 0.6904
  ⇒ D3 的 cap（8）从不触发，所有 immediate 都是 witness fail。
- strace（120k op 窗口）：`to_submit` 直方图 `{2: 39991, 1: 40018}`，enter 总数 80,017
  vs A 臂 120,007（−33%），`ret` 全部 = to_submit；A 臂 120,000 次全部 to_submit=1。
- phaseA（无仪器，24 对）：CPU/op **−6.69% [−7.25,−6.18]**，吞吐 +7.20%，
  p50 −3.07%、p95 −11.38%、p99 −9.57%。
- 分解（split-d3，12 对，420k op/invocation）：用户 −59.5 ns/op（−4.74%）、
  提交线程内核 −194.4 ns/op（−11.00%）、io-wq 内核 −103.2 ns/op（−6.39%），
  采样合计 ≈ −357 ns/op，bench 自身总差 −354.8 ns/op（−7.67%，与 phaseA −6.69% 同向）。
- 反事实：deferoff 后 B−A = **+1.07% [−0.47,+2.80]**（8 对），改善完全消失。

### 4.2 机制解释

1. depth=3 时，一次 poll 可以 flush 2 个 SQE 并 published 2 个完成（flush 先于 reap），
   而 bench 每轮只 consume 1 个 → **上一 pass 留下 1 个“多余的 ready 完成”**。
2. 这个 surplus 让 bench 在下一轮 **不调用 wait/poll** 就能 take → 两次连续 dispatch 之间
   没有 poll → 第一次 defer 的 SQE 留在 ledger（trace：ledger=1），第二次 dispatch 时
   witness 仍成立（live≥2 > ledger=1）→ 第二次 defer（ledger=2）→ 窗口满，下一次 wait
   的 poll 一次 flush 2 个 SQE。
3. 因此 D3 的 enter 节省 = 每 3 个 op 省 1 次 enter（0.3095/op），内核 CPU 每省一次 enter
   约 625 ns；信号成本仍在（0.6904 signal/op），但 write 发生率降到 27.0%
   （0.1862 write/op，多个 defer 共享一次 signal），净效应为负（改善）。

注：D3 的 `poll_passes/op` 只降 1.28%（0.5747→0.5672），`wait_attempts/op` 降 2.39%；
**改善不是“少 poll”，而是“同样的 poll 承担更大的 flush、更少的 enter”**。
这与 C2 的结论一致：D2 的 flush 从 dispatch 迁到 poll 时 poll 变长但 flush 总数不变；
在 D3+，flush 总数本身开始下降。

---

## 5. 为什么 D16 改善更大

### 5.1 直接测量

- 计数器：`sqes_deferred = 0.8762/op`，`actual_enters = 0.1974/op`（−80.26%），
  `SQE/enter = 5.066`。
- trace：flush size `{1: 1343, 7: 1166, 8: 1311}`（phase 1；phase 2/3 为
  `{1:1340,7:1204,8:1279}` / `{1:1349,7:1170,8:1307}`，波动 <3%）；defer 决策在 ledger=1..7 各约 2478 次；
  immediate 决策只在 ledger=1（1183 次，witness fail）或 ledger=8（1311 次，**cap 强制**）。
- C1 门计数（D16 B，20k op）：`defer_owner_pass=19984`（99.92%）、
  `defer_witness_pass=18821`（94.1%）、`signal_count=17532`（≈deferral 87.7%）；
  witness pass 与 defer 之间的 6.4% 即 **ledger.size() ≥ 8 的 cap 拦截**。
- strace（120k op）：`to_submit` 直方图 `{8: 7494, 7: 7494, 1: 7590}`，enter 22,585 vs A 120,007（−81%）。
- phaseA（24 对）：CPU/op **−26.36% [−26.66,−26.08]**，吞吐 +35.88%，
  p50 −32.31%、p95 −31.00%、p99 −28.68%。
- 分解（split-d16，12 对）：用户 −285.7 ns/op（−29.42%）、内核 −500.0 ns/op（−49.89%）、
  io-wq −208.3 ns/op（−17.24%），合计 −993 ns/op（该块总 −31.0%）。
- 反事实：deferoff 后 B−A = **+0.26% [−0.69,+1.04]**（8 对），p50/p95/p99 全部 n.s.。

### 5.2 机制解释（三个乘性因素）

1. **更多的 defer**：0.876 vs D3 0.690 —— 窗口 16 使几乎每次 dispatch 都发生在
   “核内已有 ≥1 个 in-flight”的状态（witness pass 94.1%）。
2. **更大的批**：D3 的平均批 1.45，D16 为 5.07；ledger 从 1 单调填到 7/8 才被
   poll 或 cap 排空。cap=8 是这里的实际上限（`kDeferCap`，`uring_transport.cpp:284`），
   6.4% 的 dispatch 由 cap 触发 immediate flush（flush 8 个 SQE）。
3. **更低的单位通知成本**：write/wakeup 从 D3 的 27.0% 降到 6.2%（0.054 write/op），
   因为 7 个 defer 共享一次 progress pass，`notification_pending_` 合并掉绝大多数写
   （C1/D2 已测：合并路径 35 ns，write 592 ns；D16 的 write 成本折算仅 ~32 ns/op）。
4. 内核节省与 D3 同一系数：0.8026 saved enters/op × 625 ns ≈ 500 ns/op 内核；
   用户侧节省（−286 ns/op）比 D3 的按 enter 比例更大，与每次 flush 的 liburing 提交准备
   减少 0.80 次/op 一致（用户态每次 flush ~350 ns 量级，D16 每省 enter 356 ns，D3 192 ns，
   后者被 signal 用户态成本部分抵消）。

因此 D16 的 −26% 不是“深度的魔法”，而是 **enter 摊销率（5.07×）与通知合并率（94%）
同时到达上限** 的结果。D16 的 p50 绝对延迟仍然高达 34 µs（窗口 16 的排队），但相比 A 臂
的 50.6 µs 下降 32%，说明批量化在吞吐与尾延迟上同时占优。

---

## 6. 是否存在使 D2 与 D3 在提交时可区分的可观察状态？

**有，且是后端在提交点已持有的状态。**

### 6.1 直接观测到的状态差异

| 提交时状态（trace，逐 dispatch） | D2 | D3 | D16 |
|---|---|---|---|
| `transport_ledger_->size()`（append 前）> 0 | **0 / 20000（0.0%）** | **6436 / 20000（32.2%）** | **16178 / 20000（80.9%）** |
| defer 决策时的 ledger size（append 后） | 恒 1 | 1（7125）或 2（6436） | 1..7（各 ~2478） |
| immediate 决策时的 ledger size | 恒 1（witness fail） | 恒 1（witness fail） | 1（1183，witness fail）或 8（1311，cap） |
| 时间加权 ledger 占用 | 0.133 SQE | 0.251 SQE | 1.442 SQE |

`transport_ledger_->size()` 与 witness margin（`live_cookies_ − ledger.size()`）都在
`dispatch_mtx_` 下、defer 门链里直接可见（`uring_transport.cpp:277,284`），无需新增机制。
**D2 与 D3 的区分是零歧义的：D2 的提交点永远看到空 ledger；D3 有 32.2% 的提交看到
ledger 非空（且下一个 defer 会把批扩到 2）。** 这正是“上一个 defer 是否幸存到本次提交”
这一事实的直接编码。

### 6.2 但不能把该状态误读为“策略变量”

- 该状态是 **poll 无条件 flush + 窗口深度** 的下游结果，不是 D2 的偶然波动：
  D2 trace 的三个 phase、20k dispatch 中 0 次非空；计数器 12 对 deterministic 到 ±0.002。
- 后端看不到 bench 窗口里“有多少 ready 完成尚未 consume”（那决定 bench 下一次是否 wait），
  它只能看到 ledger/witness。就本工作负载而言，二者一一对应；
  换一个 consume 策略（例如 any-ready consume，或不同的 wait 批）可能改变对应关系。
- 因此若未来研究提交策略，可检验的假设是：
  **H-DEPTH-1**：在 `poll()` 中仅当被等待的请求仍在 ledger 内（或 ledger 已阻塞进度）时才
  flush，其余情况让 ledger 跨轮累积——这会让 D2 也获得 2-SQE 批，但把被 defer 请求的
  核内可见时间推迟一轮（D2 已测的 p50 +6.9% 说明该代价非零）。
  **H-DEPTH-2**：把 defer 准入与 `ledger.size()`/witness margin 绑定（例如 ledger 非空时
  降低 defer 门槛、或 cap 触发前预排空），目标是在 D3–D16 把 1.45/5.07 的批进一步推向 cap 上限；
  是否有净收益取决于每 enter 的 625 ns 内核节省与 signal 合并成本。
  本轮不实现、不评估这两个策略。

### 6.3 与 D2 延迟代价的关系

CF-1/CF-2 已裁决：D2 的延迟位移由 defer 时序（L1）承担，不随通知（L0）消失；
deferoff 后 D2 延迟归零。D3/D16 的延迟改善说明：一旦 defer 能合并成批，
“推迟一个 poll 周期”的代价被 enter 节省与吞吐提升覆盖（D3 p95 −11.4%，
D16 p50 −32.3%）。因此 H-DEPTH-1 的风险是明确的：在无法形成 ≥2 批的形状下，
它会复制 D2 的纯代价路径。

---

## 7. 观测层级与边界

| 结论 | 层级 | 依据 |
|---|---|---|
| D2 SQE/enter = 1.000，D3 1.448，D16 5.066 | **直接测量** | 计数器 12 对（同构建）；trace 20k dispatch 直方图；strace to_submit 独立复核 |
| D2 提交点 ledger 恒空、D3 32.2% 非空 | **直接测量** | trace 逐事件序列（3 phase 一致） |
| D2 无摊销的结构原因（poll 无条件 flush + 窗口 2 + 内联完成同 pass reap） | **直接测量 + 源码顺序** | `uring_backend.cpp:651-657`；`bench_w02_pipeline.cpp:384-448`；C3 内联完成直测 |
| D3/D16 的 CPU 改善由 defer 承担 | **反事实支持** | deferoff 变异同二进制 8 对：D3 +1.07% [−0.47,+2.80]、D16 +0.26% [−0.69,+1.04]；CF-2 D2 −0.76% |
| 每避免 enter ≈ 625 ns 内核 CPU | 派生（split 分解 ÷ enter 节省） | D3/D16 两个独立块同值 |
| D3/D16 通知成本随合并率下降 | 直接测量（计数）+ C1 单次成本外推 | write 0.186/0.054 per op；C1 592 ns/write、35 ns 合并路径 |
| io-wq 内核 CPU 随 enter 减少而下降 | 测量到，**机制 UNOBSERVED** | split 块 −6.4%/−17.2%；C3 在 D2（enter 不变）为 flat |
| D3/D16 的 submit delay 绝对值 | **受限**（trace 扰动 ~0.7 µs/op） | trace p50 0.62/0.71/8.36 µs；native 仅 D2 有 C2 直测 |
| 内核态 PMU、syscall 时长、D3/D16 runnable delay | **UNOBSERVED / OBSERVABILITY_LIMIT_REACHED** | 无 root（paranoid=2），无 sched tracepoint |
| D1 | 机制关闭对照 | 计数器 sqes_deferred=0、enters=1.0、CPU −0.07% 中性 |

Instrumentation 分离（未叠加）：
- 计数器块（b9a49946）：只用于路径/计数/单次率，CPU 读数为 ON 块（D2 +4.03%、D3 −9.53%、D16 −29.81%），
  与无仪器 phaseA（+3.73/−6.69/−26.36）同向但绝对值不可混用（成本表 §5 的 ON 开销已登记）。
- trace 块（本轮新构建 2f47d5c1，单仪器）：只用于事件序列/ledger 占用/延迟结构，无 timing 结论。
- strace 块（冻结二进制 61c6d579）：只用于 syscall 类别与 to_submit 参数；strace 会掩蔽 B 的
  eventfd 写（C2 已记录），故 write 计数不作为通知面证据。
- PMU 块（6670d8de，`u` only）：instructions/cycles 为用户态；内核态 PMU 不可用。
- split 块（6670d8de，PMU-only+reuse fixture，无探针）：user/kernel/io-wq 线程 CPU，采样和与
  bench `CLOCK_PROCESS_CPUTIME_ID` 差 ≤1%（D3 4601 vs 4629，D16 3175 vs 3203 ns/op）。
- deferoff 块（64189aa8，timing 构建 + 既有 mutant 宏）：单因素因果，块内 A/B 同二进制。

---

## 8. 反驳的竞争解释

1. **“D2 只是深度太小，多 defer 几次就能摊销”** —— 否。D2 的 defer 率已 55.9%，
   但 trace 证明两次 dispatch 之间必有 poll flush；0/20k 次 dispatch 见到非空 ledger。
   问题不是 defer 次数，而是 defer 是否跨 poll 幸存。
2. **“D3/D16 的改善是少 poll/少 wait 带来的”** —— 否（不是主项）。poll 只降 1.3%/10.2%，
   wait 降 2.4%/21.9%；enter 降 31%/80%，内核 CPU 降 11%/50%。
   `poll()` 的工作量本身随 flush 迁移而变化（C1：poll 366.8→658.0 ns/call 是把 flush 搬进来），
   不是新增工作。
3. **“D16 只是把延迟藏到队列里”** —— 部分但不足以解释 CPU。p50 从 50.6 µs 降到 34.2 µs，
   p99 从 72.3 µs 降到 51.5 µs；同时 CPU/op −26.4%，吞吐 +35.9%。
   延迟与 CPU 同向改善，排除“用延迟换吞吐”的零和解释。
4. **“io-wq 是新增成本/收益来源”** —— D2 已排除新增成本（+0.46% n.s.）；
   D3/D16 的 io-wq 下降是伴随收益，机制未观测，不作为归因主项。
5. **“计数器/探针本身造成 D2 的零摊销”** —— 否。零摊销在无仪器 phaseA 的
   `stats_submit_calls=1.0/op`、`stats_submitted_ops/call=1.0` 与 C2 的 strace/shim 直测中同样成立；
   计数器块与无仪器块方向一致。
6. **“D3/D16 改善来自频率/块间漂移”** —— 否（块内配对）。phaseA 同一块内 A/B ABBA 平衡、
   CI 排除 0；deferoff 同二进制块内归零。块间绝对 CPU 漂移（phaseA vs counter/split 块
   7–12%）只影响绝对值，不影响 B−A。

---

## 9. 本轮新增证据路径与复现命令

所有新产物：`/home/jnhu/sluice-595-e5/causal-r2/evidence/analysis/depth-analysis/`。

| 内容 | 路径 |
|---|---|
| 本文件 | `DEPTH_MECHANISM.md` |
| D3/D16 计数器 ledger（12 对，A/B） | `raw/cnt-d3-ab/`、`raw/cnt-d16-ab/`（results.jsonl + 每 invocation jsonl/log/err） |
| 聚合结果（JSON，含 bootstrap CI） | `depth_ledger_summary.json`（`tools/aggregate_depth.py` 生成） |
| trace 构建与逐事件序列 | `raw/trace-d2/`、`raw/trace-d3/`、`raw/trace-d16/`（trace.bin + bench.jsonl） |
| trace 解析器 | `tools/parse_trace.py` |
| strace syscall 类别（3/臂/深度） | `raw/strace-d3/`、`raw/strace-d16/`（summary.txt） |
| strace enter 参数（2/臂/深度） | `raw/enterargs-d3/`、`raw/enterargs-d16/`（enter.trace） |
| D3/D16 用户态 PMU（8 对） | `raw/pmu-d3-ab/`、`raw/pmu-d16-ab/` |
| D3/D16 split（12 对，PMU-only） | `raw/split-d3/`、`raw/split-d16/` |
| D3/D16 deferoff 反事实（8 对） | `raw/deferoff-d3/`、`raw/deferoff-d16/` |
| 运行脚本 | `tools/run_depth_ledger.sh`、`run_syscalls.sh`、`run_pmu.sh`、`run_split.sh`、`run_deferoff.sh` |

二进制与 sha256：
- 计数器 `bench_w02_pipeline_counters` = `b9a4994631dbe6714e630fec838fd1d2b98604bb872007e6882bad96f1eb3a3a`
- PMU/split `bench_w02_pipeline_pmu` = `6670d8dee37e8d8faa60549dfc5e1f6121cc4f56ee66cf58fa91cc678d16db09`
- 冻结 `bench_w02_pipeline_frozen` = `61c6d579845572cc7a4563fdc76b545e2440172a3903406d34b29bd34ae6b886`
- trace（本轮构建，worktree-trace 副本，源 diff = f939602f…，仅加 `SLUICE_595_TRACE_BUILD=1`）=
  `2f47d5c18bced037fc0511b71d5c1da864be7fa1b48cd55eab2f7565f3a264c1`
- deferoff（既有 CF-2 存档，timing 构建 + `SLUICE_595_MUTANT_DEFER_DECISION_DROPPED`）=
  `64189aa8765ca03b5c5649f95a24708ace8326bf2b7b25a6f365c0605de8d72f`

环境：E5，Xeon E5-2666 v3（10C/20T），kernel `7.2.11-200.fc44.x86_64`，
g++ 16.2.1 / xmake 3.1.2 / liburing 2.13；bench `taskset -c 2`，harness/sampler cpu3；
fixture warm tmpfs `/tmp/sluice-595-cost`；运行期 loadavg 0.26–1.35（20 核），
`scaling_cur_freq` 请求值 1.2–1.55 GHz（无 turbo 观测手段，按块内 A/B 平衡处理）。

主要命令（每块串行、独占机器）：

```
# 计数器 ledger（D3、D16；同 cnt-on-full-d2 协议）
python3 evidence/counters/tools/run_blocks.py --label cnt-d3-ab --mode direct \
  --bin evidence/counters/bin/bench_w02_pipeline_counters --pairs 12 --arms AB \
  --case '=W02-uring-4k-d3' --warmup 1 --measured 5 --counters 1 --outdir .../raw/cnt-d3-ab
# D16 同（case=W02-uring-4k-d16）

# trace（单仪器；warmup=0 measured=1；每个 depth 每臂一次）
env SLUICE_BENCH_595_DEPTHS=1 SLUICE_BENCH_CASE_FILTER='=W02-uring-4k-d3' \
  SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 SLUICE_BENCH_STATS=1 \
  SLUICE_BENCH_REUSE_FIXTURE=1 SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-cost \
  SLUICE_BENCH_JSONL=... SLUICE_595_TRACE=1 SLUICE_595_TRACE_OUT=.../trace.bin \
  taskset -c 2 worktree-trace/build/linux/x86_64/release/bench_w02_pipeline

# syscall 类别与 enter 参数（冻结二进制，strace -f）
strace -f -c -e trace=io_uring_enter,io_uring_setup,io_uring_register,write,read,ppoll,poll,futex,eventfd2 ...
strace -f -e trace=io_uring_enter -o enter.trace ...

# PMU（u-only）与 split（PMU-only，无 perf）
python3 run_blocks.py --label pmu-d3-ab --mode pmu --bin ...bench_w02_pipeline_pmu \
  --pairs 8 --arms AB --case '=W02-uring-4k-d3' --warmup 1 --measured 1 --pmu-only ...
python3 run_blocks.py --label split-d3 --mode direct --bin ...bench_w02_pipeline_pmu \
  --pairs 12 --arms AB --case '=W02-uring-4k-d3' --warmup 1 --measured 20 --pmu-only ...

# deferoff 反事实（同二进制 ABBA，8 对，PMU-only，measured=20）
env SLUICE_595_ARM=B SLUICE_BENCH_595_DEPTHS=1 SLUICE_BENCH_CASE_FILTER='=W02-uring-4k-d3' \
  SLUICE_BENCH_WARMUP=1 SLUICE_BENCH_MEASURED=20 SLUICE_BENCH_PMU_ONLY=1 \
  SLUICE_BENCH_REUSE_FIXTURE=1 ... taskset -c 2 bench_w02_pipeline_deferoff
```

统计：块内 pair 相对差 (B−A)/A，bootstrap 10,000 次、seed 5950、95% percentile CI；
phaseA 用 24 对、120 measured rep/臂；计数器/split/deferoff 用 12/12/8 对。
D2 的既有块（`cnt-on-full-d2`、`split-d2`、`pmu-d2-ab`）直接引用，未重跑。
