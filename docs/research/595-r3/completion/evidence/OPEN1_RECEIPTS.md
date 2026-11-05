# #595 R3 续跑 — OPEN-1 定义解除与证明义务（OPEN1_RECEIPTS）

| 字段 | 值 |
|---|---|
| 角色 | R3 续跑 OPEN-1 语义义务审阅员（只读前轮证据、源码与契约） |
| completionRoot | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z` |
| 基线 | `baseSha = 0a22a79195f1b913145fcd443496e7f70b08cdcf` |
| R2 锚点 | `271b81bfc9d64eecc0266fe7066b9040525dd710` |
| 本轮状态 | **OPEN-1 的臂定义与证明义务已写入**；**语义未证明**；未跑 benchmark、未构建、未编辑产品代码、未改 worktree |
| 证据根 | 前轮 `/home/jnhu/sluice-595-e5/r3/evidence/`；本轮 `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/evidence/` |
| 结论词汇 | 本文件只写“定义/义务/所需观测”，不写 `semanticsOk = true`，不替实现者宣称任何交错已证明 |

本文件回答四件事：

1. OPEN-1 原始缺口是什么、为什么前轮的 `A0/A1/B` 不能当冻结臂；
2. 前轮审查未证明的具体交错，逐项写成可执行证明义务或明确反例；
3. 续跑冻结的五臂定义（同时追加到 `CONTEXT_COMPLETION.md`）；
4. 原方案与续跑方案的主形状/假设/主门差异，以及本轮必须重验的旧结果。

---

## 1. OPEN-1 的原始定义缺口

### 1.1 冻结时的事实

`CONTEXT.md` §7.2（`:80-92`）原文记录：R3 使用 `A0` / `A1` / `B` 与 `A_R2` / `B_R2` 五类标签，但该 ask 未给出 `A0`、`A1`、`A_R2`、`B_R2` 的干预定义；仓库内也检索不到这些标识符的 R3 既有定义。因此该节判定：

| 标签 | 冻结时状态 | 依据 |
|---|---|---|
| `A0` | 语义未在本次冻结中给出 | `CONTEXT.md:86` |
| `A1` | 语义未在本次冻结中给出 | `CONTEXT.md:87` |
| `B` | 可引用 R2 §2 的 `BOUNDED_PROGRESS`；若 R3 改定义须显式覆盖 | `CONTEXT.md:88` |
| `A_R2` | 语义未在本次冻结中给出；不得以推测代替定义 | `CONTEXT.md:89` |
| `B_R2` | 语义未在本次冻结中给出；不得以推测代替定义 | `CONTEXT.md:90` |

`CONTEXT.md:92` 的 OPEN-1 明确要求：在上述四个标签的干预定义以及与二进制/运行时开关的映射写入本节并由设计负责人确认前，**R3 不得进入正式计时**。

### 1.2 前轮 BUILD 只给了研究标签

`BUILD.md` §3.4（`:98-110`）后来定义的是 **研究标签**，并自行声明不是冻结臂：

- `BUILD.md:12`：`A0/A1/B` 是原型研究标签，不是已冻结臂定义；不得据本文件进入正式计时。
- `BUILD.md:102`：`A0` = 原始 W-03 路径，不构造/不使用 adapter。
- `BUILD.md:103`：`A1` = 同一 adapter，逐条调用 `AsyncIoContext::submit_read`。
- `BUILD.md:104`：`B` = 同一 adapter，整组调用一次 `submit_ready_reads`；suffix 逐条 span=1。
- `BUILD.md:108-109`：A1 与 A0 走同一 context 调用路径；B 直接调用 detail seam，**绕过** `AsyncIoContext::submit_read` 的 `initiation_rejection` 与 `access_mtx_`；因此 `B−A1` 同时包含“分组”和“绕过 context”两部分。
- `BUILD.md:294`（审阅问题 #6 的处置）：未改 `CONTEXT.md`；只在 `BUILD.md` 声明研究标签、OPEN-1 未解除。
- `BUILD.md:305`（限制 5）：正式计时前必须拆分 context 绕过残差，且 OPEN-1 仍未解除。

`PROTOCOL.md` §1.1（`:49-61`）把同一事实固化为：`A0/A1/B` 实际含义只是 `BUILD.md §3.4` 的研究标签，`A_R2/B_R2` 无定义，OPEN-1 在本轮结束时仍未解除。`PROTOCOL.md:199`（P-4）与 `VERDICT.md:143`（V-7）重复了该判定。

### 1.3 因此 OPEN-1 的性质

OPEN-1 不是“语义已经证明、只是没测”的问题，而是**干预定义缺口**：

- 前轮不存在一个可复核的“A0 是 baseSha 原始 consumer 循环”的保证；`VERDICT.md:98`（§4.2 第 4 条）与 `RESULTS.md:258` 已明确：worktree 的 refill 对所有臂先构造完整 `ready` span（`bench/bench_w03_external_loop.cpp:349-372` R3 版），A0 再用 `ctx.submit_read` 逐条提交；baseSha 的原循环是在循环内逐条构造 `ReadOp`（`:339-349`）。因此前轮 A0 **含原始 baseline 没有的组准备/`std::vector` 开销**。
- 前轮 `B` 直接调用 backend seam，不是 Context 入口；A1/B 不是同一个上下文入口，无法把 `B−A1` 全部归因于分组。
- 前轮没有 `A_R2/B_R2` 的任何实现或二进制映射；R2 的 `bounded_progress_` 只存在于 R2 研究副本，不在产品树 `271b81bf` 中（见 §2.5）。

本轮把上述缺口写成**定义**（见 §2），但定义的存在不等于实现或语义已经证明；所有证明义务仍为 open。

---

## 2. 续跑冻结的臂定义

以下五条同时追加到 `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/evidence/CONTEXT_COMPLETION.md`。它们是**干预定义**，不是性能结论。

### 2.1 公共约束（五臂共用）

1. **同一臂对应同一二进制 SHA**；A0/A1/B 共用 R3 研究二进制（同一 build、同一编译选项、无 `SLUICE_595_*` 仪器宏差异），A_R2/B_R2 共用 R2 研究二进制（同一 build）。不得跨二进制把布局差异解释成臂差。
2. 所有臂使用**同一 case 输入、同一 fixture、同一顺序、同一 buffer、同一 byte oracle**；arm 只改变提交路径，不改变 `ReadOp` 的 `(fd, dst, len, offset)`。
3. `ReadyGroup` 仍按 `CONTEXT.md` §6（`:53-68`）定义：首提交前已拥有、offset/长度确定、buffer 有效、依赖已确定；禁止新增等待、前移依赖、追加预取造组。
4. 计时协议沿用 `CONTEXT.md` §8（`:94-104`）：24 对/phase、`WARMUP=1`、`MEASURED=5`、`taskset -c 2`、warm tmpfs、AB/BA 交替、bootstrap 10k/seed 5950、A/A 5/3/3 pp、负控 5/5/5/3。平台修正项按 §8.1（`:106-110`）：cpu2 的 SMT 兄弟 cpu12 必须空闲或与 `2,12` 一同征用；记录 `schedutil` 与 `scaling_cur_freq`；`perf_event_paranoid=2`、`unprivileged_bpf_disabled=2` 下的权限限制必须记录，不得把不可用的 tracing 当作已测。
5. 构建/测试/计时必须持有 `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`。前轮 `PROTOCOL.md:200`（P-5）记录正确性运行未持锁，属于协议偏离；续跑不得重复。

### 2.2 A0 = IMMEDIATE（baseSha 原始 consumer 循环）

**定义**：

- 冻结 `baseSha = 0a22a791…` 的 W-03 external-loop consumer 补发循环（`bench/bench_w03_external_loop.cpp:339-356` 原版）：在 `window.size() < depth && submitted < request_count` 的循环体内，**按原位置逐条**构造 `ReadOp`（`op.file`、`op.dst = buffers[slot].data()`、`op.len`、`op.offset`），并**立即**调用 `ctx.submit_read(op)`；成功的 `Request` 立即进入 `window`，失败即 workload failure。
- **不构造 `std::vector<ReadOp> ready`、不预扫描未来成员、不分配组缓冲、不调用 adapter 或 backend batch entry**。A0 不获得任何 R3 研究 instrumentation 的组准备/调度开销。
- 对产品 case（W-03 d4/d16/d64），`max_refill` 取 `depth`，即与 baseSha 原循环逐字等价。
- 对研究 D2 case，唯一允许的额外 case 形状参数是已有的 `max_refill`（见 `BUILD.md:115-116`）：它只限制本次 refill burst 的**窗口补发上限**，A0 仍在循环内逐条构造并立即提交，不因该上限预先构造 span。若实现把 `max_refill` 实现成“先构造组再逐条提交”，A0 失效，必须改回逐条构造。
- A0 可以记录 `AsyncStats`/bench 统计，但统计不得改变提交路径；A0 不使用 `bench/r3_ready_batch.hpp` 的 `Adapter`。

**回答的问题**：这是唯一代表“原始 baseline”的控制臂；`A0` 不再含 `VERDICT.md:98` / `RESULTS.md:258` 披露的 ready-span 共同准备开销。

### 2.3 A1 = ADAPTER_IMMEDIATE

**定义**：

- 使用与 B **完全相同**的 ready-group 准备和输入：在补发点先计算 `want = min(depth - window.size(), request_count - submitted, max_refill)`，按输入顺序构造 `ready[0..want)` 的 `ReadOp`（同一 slot/offset/len/dst），然后才开始提交。
- 逐条调用**同一个 Context 内部研究入口**（A1 与 B 必须调用同一个函数；A1 每次传长度 1 的 span），每个成员都经过与该入口整组调用相同的 context 级 precheck、`access_mtx_`、admission/tally 与 `Request` 物化路径。
- A1 **不得**直接调用 backend seam，**不得**跳过 `AsyncIoContext` 的 `initiation_rejection` 或 `access_mtx_`；否则它不是 A1，而是前轮 `B` 的 bypass 变体，不能用于 `B−A1` 归因。
- 每个成员成功后立即物化 `Request<std::size_t>` 并进入 `window`；失败按同一入口的错误返回处理。

**回答的问题**：A0−A1 只余“ready span/组准备 + adapter/入口簿记”的差异，不改变 context 语义路径；A1−B 只余“逐条 vs 整组”的差异。

### 2.4 B = READY_AWARE

**定义**：

- 使用与 A1 相同的 ready-group 准备、输入、case、budget；组必须是 `CONTEXT.md` §6 的合法 `ReadyGroup`，不得等待/预取/前移依赖造组。
- 当 ready group 合法可用时，按**实际 ready 数量与预算**选批：前缀长度 = `min(ready.size(), ready_batch_cap = 8, SQ 空闲, request capacity, budget)`；然后**一次**调用与 A1 相同的 Context 内部研究入口，传入该前缀 span；入口在**同一调用内**完成 accept → prepare → dispatch → 一次 flush，**不跨调用持有 SQE、不 defer flush**。
- 单成员（或没有合法组）时，B 走与 A1 完全相同的单成员等价 immediate 路径（同一入口、span=1），不得因为 B 而增加/减少单成员语义。
- 若前缀后仍有 suffix（cap/SQ budget/control/recovery/ineligible 等停止原因），suffix 由调用者从 `untouched_begin` 开始逐条走**同一入口** span=1；suffix 不二次成组（这是 B 的冻结策略，不是内核限制）。任何 suffix 二次批量都是一个新臂，不能冒充 B。
- B 的入口必须对每个 accepted key 物化 `Request`（见 OPEN-1.6）；B 报告 `accepted`/`untouched_begin`/`untouched`/`stop` 必须满足 `BUILD.md` §3.2（`:83-96`）的 disposition 契约。
- **B 不得绕过 Context**。如果实现仍直接调用 `backend_->submit_ready_reads`，该臂只能叫 `B_bypass`，不能用于回答 `B−A1` 或产品问题；`BUILD.md:108-109` 的 bypass 残差仍然存在。

### 2.5 A_R2 = IMMEDIATE at 271b81bf

**定义（只引 R2 原文/源码，不作推测）**：

- **二进制/树**：`271b81bfc9d64eecc0266fe7066b9040525dd710` + R2 研究补丁 `/home/jnhu/sluice-595-e5/evidence/measurement/patch.diff`，sha256 `7be9d0145a8c268a62d9fa90338b7cda95cefc5f8ad2976e4706cb192fb1eaf0`；R2 `PROTOCOL.md:15-27` 已记录该基线与补丁、`git diff HEAD` sha256 `b006dbbf751f5fb2bbe8fb9edb091ae66e57a181b8cf2470d880c9589e1f4072`，以及“arm A = IMMEDIATE（frozen baseline inline flush）”。
- **关键事实**：产品树 `271b81bf` 本身**没有** `bounded_progress_` 成员；该成员由 R2 研究补丁加入（R2 研究 worktree `src/async/uring_backend.cpp:170-171` 从 `SLUICE_595_ARM` 读取，header `:354-357` 声明）。因此 A_R2/B_R2 的二进制必须是 `271b81bf + patch`，不能直接拿主树 `271b81bf` 构建后声称有 A/B 两臂。
- **A_R2 行为**：`SLUICE_595_ARM` 未设置或非 `B` → `bounded_progress_ = false`；`defer_transport_flush_locked_()` 的第一检查 `if (!bounded_progress_) return false;`（R2 `src/async/uring_transport.cpp:229-233`）立即早退；`dispatch_after_accept` 在 dispatch 点立即 `submit_transport_locked()`（R2 `src/async/uring_transport.cpp:84-90`），不执行 owner/payload/witness/cap，也不 defer。
- **用途**：只用于 R2 原 W-02 D2 shape（`W02-uring-4k-d2`，需 `SLUICE_BENCH_595_DEPTHS=1`；见 R2 `bench/bench_w02_pipeline.cpp` 的 opt-in 分支）上回答“同 SHA 旧 B 是否仍比 IMMEDIATE 差”。A_R2 不是 R3 的新算法，也不是 baseSha 产品路径。

### 2.6 B_R2 = BOUNDED_PROGRESS at 271b81bf

**定义（逐条对应 R2 源码）**：

- 同一 `271b81bf + patch` 二进制；`SLUICE_595_ARM=B` → `bounded_progress_ = true`（R2 `src/async/uring_backend.cpp:170-171`）。
- `defer_transport_flush_locked_()` 的完整门链，顺序为（R2 `src/async/uring_transport.cpp:229-263`）：
  1. `bounded_progress_` 已为 true（不早退）；
  2. **owner**：`progress_driver_seen_ && std::this_thread::get_id() == progress_driver_recent_`，否则返回 false；
  3. **payload**：`prepared_ops_[h.slot.value].kind == read && native_length == 4096`，否则返回 false；
  4. **witness**：`live_cookies_.load(relaxed) > transport_ledger_->size()`，否则返回 false；
  5. **cap**：`transport_ledger_->size() < kDeferCap`（`kDeferCap = 8`，R2 header `:287-299`），否则返回 false。
- 全部门通过 → 返回 true；`dispatch_after_accept` 只执行 `service_pending_controls_locked_()`，**不**调用 `submit_transport_locked()`（R2 `src/async/uring_transport.cpp:84-90`），flush 延迟到后续 progress 边界。任一失败 → 立即 submit（与 A_R2 相同）。
- R2 源码/文档没有写的细节（例如“defer 之后具体由哪一次 poll 必 flush”的更强时序）不得补写成定义；只用 R2 原文和上述源码可核验事实。R2 `CF0_THREE_ARM.md:26-42` 与 R2 `PROTOCOL.md:26-27` 是文字来源。
- **用途**：与 A_R2 同一二进制、同一 W-02 D2 shape、同一 R2 协议，回答老 B 的 D2 退化是否在本轮重建的 R2 二进制上仍存在。历史 R2 数值（CPU +3.73%、p95 +4.96%、p99 +7.07%、tput −3.58%，`docs/research/595-causal-r2/RESULTS.md:38` / `PROTOCOL.md:126`）**不得复用**，必须本轮重新构建/配对。

### 2.7 二进制/开关映射表

| 臂 | 构建树 | 运行时选择 | case |
|---|---|---|---|
| A0 | R3 研究二进制（baseSha + completion patch） | `SLUICE_595_R3_ARM` 缺省/A0 | W-03 产品 d4/d16/d64；研究 D2/D3/D16 用 `SLUICE_595_R3_CASES=1` |
| A1 | 同一 R3 二进制 | `SLUICE_595_R3_ARM=A1` | 同上 |
| B | 同一 R3 二进制 | `SLUICE_595_R3_ARM=B` | 同上 |
| A_R2 | R2 研究二进制（`271b81bf` + `7be9d014…`） | `SLUICE_595_ARM` 缺省/A | W-02 D2（`SLUICE_BENCH_595_DEPTHS=1`） |
| B_R2 | 同一 R2 二进制 | `SLUICE_595_ARM=B` | 同一 W-02 D2 |

同一 build 内不得出现两个臂的二进制布局/编译宏差异；A0/A1/B 的 `r3_*` 计数与 `SLUICE_BENCH_STATS` 只作诊断，不作性能结论。

---

## 3. 前轮审查未证明的具体交错（证明义务）

下表是索引；每项的完整义务/触发/观测见 3.1–3.16。所有项默认 **open**：本轮没有执行这些测试，也没有把任何一项标成 proved。

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

### 3.1 OPEN-1.1 臂定义与二进制映射缺口

- **位置**：`CONTEXT.md:80-92`；`BUILD.md:98-110`；`PROTOCOL.md:199`；`VERDICT.md:143`；R2 `PROTOCOL.md:15-27`；R2 研究 worktree `src/async/uring_backend.cpp:170-171`、`include/sluice/async/detail/uring_backend.hpp:354-357`。
- **未解问题**：冻结时 `A0/A1/A_R2/B_R2` 没有干预定义；`BUILD.md` 只给研究标签；R2 的 `bounded_progress_` 不在 `271b81bf` 产品树中，而在 `271b81bf + patch` 的研究树中。若直接按 SHA 构建主树并设置 `SLUICE_595_ARM=B`，B 臂不存在。
- **义务**：五臂实现必须与 §2 定义逐条一致；A0/A1/B 同一 R3 二进制，A_R2/B_R2 同一 R2 二进制；记录每臂的树 SHA、研究补丁 sha256、二进制 sha256、运行时 selector 与读回；不得把跨二进制差异当臂差。
- **触发交错**：任何一次声称“A0/A1/B”或“A_R2/B_R2”的计时/对照；尤其是用主树 `271b81bf` 构建 R2 臂、或把 R3 的 `SLUICE_595_R3_ARM` 与 R2 的 `SLUICE_595_ARM` 混用。
- **所需确定性观测/反例**：`git rev-parse`、`git diff` 的 sha256（R2 应为 `b006dbbf…`）、`git apply --check --reverse`、二进制 `sha256sum`、arm selector 日志；反例 = 同一“B”二进制在 A/B 选择下产生相同的 `bounded_progress_` 读回，或 A_R2 构建树没有 R2 patch。

### 3.2 OPEN-1.2 Context 入口/锁顺序；A1/B 不同入口

- **位置**：R3 `bench/r3_ready_batch.hpp:99-131`（A1 用 `ctx.submit_read`，B 用 `backend_->submit_ready_reads`）；`src/async/async_io_context.cpp:249-269`（`initiation_rejection` → `access_mtx_` → backend）；`src/async/uring_backend.cpp:443-622`（batch 入口）；`docs/explicit-io-contract.md` THREAD-02（`:1023-1037`）、REQ-02（`:603-627`）。
- **未解问题**：前轮 B 绕过 context 的 `initiation_rejection` 与 `access_mtx_`，A1/B 不是同一入口；因此 `B−A1` 混入“分组”和“绕过 context”两部分（`BUILD.md:108-109`、`RESULTS.md:115`）。新入口还缺少公开的锁顺序说明：context `access_mtx_`、backend `dispatch_mtx_`、core mutex 的嵌套顺序必须与既有路径一致；B 的入口若在 backend 锁内回调 host 或反向取锁，会违反 THREAD-02。
- **义务**：A1/B 必须调用**同一个** Context 内部研究入口（A1 span=1，B span=N）；该入口执行与 `submit_read` 相同的逐成员 `initiation_rejection`、`access_mtx_`、stats/tally、`Request` 物化；B 不再直接调用 backend seam。锁顺序冻结为 `access_mtx_ → dispatch_mtx_ → core mutex`（与 `submit_read`/`cancel`/`poll` 现有路径一致）；任何新路径必须证明不反向嵌套、不在 backend/core 锁内调用 host。
- **触发交错**：生产者线程 A1/B submit 与 driver 线程 `poll_progress`、另一线程 `cancel`、`close_admission`/`request_stop` 并发；入口在 `access_mtx_` 前后与 backend `dispatch_mtx_` 前后被抢占；取消在 `core_->accept` 与 dispatch 之间获胜。
- **所需确定性观测/反例**：在入口加 test-only pause gate（进入前/取得 access 后/取得 dispatch 后），两线程交错；断言无死锁、无锁序反转、单成员结果与 `ctx.submit_read` 的 disposition/统计一致；TSan/ASan 无 race；反例 = A1 用 `ctx.submit_read` 而 B 走 backend 直调（当前代码即此形态），或新入口在持有 `dispatch_mtx_` 时调用 host/observer。

### 3.3 OPEN-1.3 并发 submit 与 driver；residue 唤醒

- **位置**：`docs/explicit-io-contract.md` PROG-01（`:828-863`）、PROG-02（`:865-912`）、PROG-03（`:914-935`）；R3 batch 的 `advertise`/`signal_ready_progress`（`src/async/uring_backend.cpp:592-620`）；bench driver 的 `poll_progress` 循环与 `retry_pending`（`bench/bench_w03_external_loop.cpp:221-251`）；context `wait_one` 的 retry（`src/async/async_io_context.cpp:641-652`）；前轮 `prepark_batch_completion_is_not_lost`（`correctness/CORRECTNESS.md:51`）。
- **未解问题**：B 在 context drive domain 之外接受/提交；若留下 `dispatch_`/ledger 残渣，必须由 entry 显式 signal，且 driver 必须把 `dispatch_retry_remains` 当作 immediate/需再 pass 的工作（PROG-01）。前轮 pre-park oracle 覆盖“批量完成后唤醒”，但未覆盖：生产者在 owner 最终 readiness 检查之后调用 B 入口、(a) 完成立即到达、(b) submit 返回 0/EAGAIN 只留 ledger、(c) 停在 cap/control 只留 dispatch/suffix。
- **义务**：每一个“已接受但未进入内核”的状态必须要么产生内核 eventfd 可观察 readiness，要么设置 `dispatch_retry_remains`/`immediate_work_remains` 并 signal；不得依赖周期性 poll 才暴露；driver 必须为 `dispatch_retry_remains` 安排后续 pass（`wait_one` 10ms retry；external loop 的 1ms timeout 只是实现，不是契约）。`poll_progress` 的报告必须诚实区分 immediate vs accepted。
- **触发交错**：owner 在 `progress_->wait_if_unchanged` 的最终 revalidation 之后、park 之前被冻结；生产者在此时调用 B 入口并提交/失败/停批；eventfd 先被 ack 再产生完成；entry 的 signal 与 driver 的 token 快照交错。
- **所需确定性观测/反例**：使用 `ProgressSource` pre-park gate（`include/sluice/async/detail/progress_source.hpp:300-311`，前轮 test:1079 的同一 seam）冻结 owner；生产者调用 A1/B；断言 eventfd 可读或 `ProgressPass.dispatch_retry_remains/immediate_work_remains` 为真且下一次 pass 处理精确的 ledger/dispatch 前缀；直接 `read(2)` eventfd/ token 快照；反例 = owner 无限 park、ledger 非空、没有 signal/没有下一 pass。

### 3.4 OPEN-1.4 并发 close/admission/shutdown 与 `have_ring_` 生命周期

- **位置**：`docs/explicit-io-contract.md` THREAD-01（`:1003-1021`，submit/cancel/close_admission 可并发；driving shutdown 仅 owner）、REQ-02（`:623-627`，admission close 与 acceptance 同一串行权威）、SHUT-01/02（`:1204-1253`）、BOUND-02（`:1323-1331`）；R3 batch 外层 `if (!have_ring_)`（`src/async/uring_backend.cpp:448`）与锁内重查（`:507-513`）；`admit_` 的 `have_ring_`（`:648-650`）；base `submit_request` 的 `have_ring_`（baseSha `src/async/uring_backend.cpp:433`）；`retire_execution_resources`（R3 `:383-414`，base `:375-402`）；`poll` 的首个无锁读（R3 `:835-838`）；`close_admission`（R3 `:909-922`）；`signals_physical_progress`（R3 header `:67`）。
- **未解问题**：`have_ring_` 是普通 `bool`；`retire_execution_resources` 在 `dispatch_mtx_` 内写 `have_ring_=false` 并 `io_uring_queue_exit`（R3 `:383-414`），但 batch 外层、`submit_request` 单条路径、`poll()`、`close_admission()`、`signals_physical_progress()`、`has_immediate_physical_work()` 都在锁外读它。THREAD-01 允许 submit 与 close_admission/request_stop 并发，driving shutdown 只是“owner only”，并未把“submit 与 driving shutdown 并发”写成禁止；因此存在 C++ data race/UB 和 ring 退出后解引用的风险。前轮 `BUILD.md:302`（限制 2）已承认单条 submit 与 shutdown 的竞争未消除；R3 只关闭了“取到 `dispatch_mtx_` 之后”的窗口，外层读与单条 fallback 路径仍未证明。
- **义务**：二选一并证明：(a) 契约层面明确“submit 不得与 driving shutdown 并发”，并在实现/文档中作为调用者前置条件；或 (b) 在 THREAD-01 允许的并发下，把 `have_ring_`/ring 生命周期改为原子或始终持锁读取，保证 admission close 与 acceptance 线性化、已接受请求被 settlement 保留、retire 后 entry 不再触碰 ring；post-close submit 返回 `invalid_state`/admission-closed，而不是 `backend_error`。
- **触发交错**：生产者通过 batch 外层 `have_ring_` 检查后被抢占；owner 完成 `settlement_converged_`、`stop_execution`、`retire_execution_resources`（`queue_exit`）；生产者恢复并进入 `admit_`/`io_uring_get_sqe`。或单条 `submit_request` 在 `have_ring_` 检查与 dispatch 之间被 shutdown 抢占。
- **所需确定性观测/反例**：在 batch 外层检查后加 pause gate；owner 执行 `shutdown(drain)` 并 retire；恢复生产者；ASan 检查无 ring 解引用、TSan 检查 `have_ring_` 无 race；断言返回值/无 stranded accepted。另一交错：submit 在 close 前赢得 acceptance，shutdown 必须结算它。反例 = post-close submit 触碰已退出 ring、崩溃、返回 backend_error，或 TSan 报 `have_ring_` race。

### 3.5 OPEN-1.5 逐请求 acceptance 线性化与 accepted 前缀结算

- **位置**：`docs/explicit-io-contract.md` REQ-02（`:603-627`）、REQ-03（`:629-654`）、BACKEND-02（`:1118-1121`）、BOUND-01（`:1299-1321`）；R3 batch admit 循环（`src/async/uring_backend.cpp:543-559`）；`admit_` 的 reserve/prepare/accept/rollback（`:662-726`）；report（`:445-622`）；`BUILD.md` §3.2（`:83-96`）。
- **未解问题**：批量在一次调用内对多个请求分别 `core_->reserve()`/`core_->accept()`；`close_admission` 或容量耗尽可能落在成员 k 与 k+1 之间。要求每个请求有唯一 acceptance 线性化点；accepted 前缀必须有公开 binding 和保证结算；被拒绝成员必须回滚、无 effect、无 binding；accepted 不能被后续成员失败 stranded。前轮有“容量中途耗尽”（`correctness/CORRECTNESS.md:50`）和“admission 已关闭”（`:51`）场景，但没有 close/cancel 恰好落在两个成员之间的交错。
- **义务**：批量不要求整组原子，但每个请求必须单独线性化；accepted 前缀恰好是 report.keys[0..accepted)；stop==rejected 的被尝试项位于 index accepted（`untouched_begin-1`），既非 accepted 也非 untouched；close_admission 与 accept 同一串行权威；后续成员失败不得撤销/ stranded 已接受前缀；被接受前缀必须可由 shutdown/driver 结算。
- **触发交错**：`close_admission`/`request_stop` 在 `reserve` 与 `accept` 之间；容量在第 i 个 `reserve` 耗尽；cancel 在第 j 个 accept 之后、dispatch 之前获胜；poison 在 flush 中；第 i 个 prepare 失败。
- **所需确定性观测/反例**：在 `admit_` 的 reserve 之后、accept 之前，以及成员 accept 之间加 pause gate；另一线程 close/cancel；检查 `RequestCore::snapshot()` 的 reserved/accepted/public_bindings/outstanding；report accepted/untouched_begin；每个 accepted key 恰有一个可 `ready()` 的 Request；被拒绝 key 无 lookup/binding；shutdown 后前缀全部 terminal。反例 = accepted>0 却无 Request/binding，或 rejected key 仍可 lookup，或前缀被 report 成 untouched。

### 3.6 OPEN-1.6 accepted→Request 物化与 borrow 边界

- **位置**：`docs/explicit-io-contract.md` REQ-04（`:656-671`，publication 是 release boundary）、REQ-05（`:673-687`）、HANDLE-03（`:744-755`）；R3 adapter 的 accepted→Request 循环（`bench/r3_ready_batch.hpp:118-121`、`:157-160`）；`TestAccess::make_request`（`include/sluice/async/detail/test_access.hpp:23-25`）；core `accept` 设置 `binding_live`（`src/async/detail/request_core.cpp:184-212`）；`reset_delivery_locked_` 的 `event_owed` fail-fast（`src/async/uring_backend.cpp:625-634`）。
- **未解问题**：batch report 只返回 key；调用者必须为每个 accepted key 物化 `Request`。前轮确实发现过“只统计 accepted 而未物化 Request，导致 core 留下无主 public binding、context 析构 fail-fast”的真实缺陷（`BUILD.md:312` §10 的最小反例段）。当前路径在 rejected/cancel/poison/partial 等早退前是否保证每个 accepted 都有 handle，未单独证明。borrow 在 publication 之前不能被释放；`event_owed` 的 delivery ref 必须保留到消费。
- **义务**：对每个 accepted key，在任何 early return（包括 `stop==rejected`、`control`、`recovery`、`poison`）之前恰好物化一个 `Request`；不得为 untouched/rejected key 物化；slot 在 handle consume/discard 前不得复用；publication 后 backend/cancel/observer 不得再触碰 buffer/File；`event_owed`/control 引用必须按 publication/retirement 释放。
- **触发交错**：第 i 个成员 rejected 而前缀 accepted；cancel 在 accept 后、物化前获胜；poison 在 flush 后、物化前；容量耗尽；stale generation；observer armed。
- **所需确定性观测/反例**：在 accept 与 adapter 物化之间加 pause gate，注入 cancel/poison/close；检查 `public_bindings == accepted`、每个 key 的 generation 与 Request 一致、析构无 `async_context_outstanding_fail_fast`/`uring_non_quiescent_destruction_fail_fast`；未完成成员的 buffer sentinel 未被触碰；反例 = accepted>0 但 binding 无主、或析构 abort、或 publication 后 buffer 被写。

### 3.7 OPEN-1.7 partial submit 前缀归属

- **位置**：`docs/explicit-io-contract.md` BACKEND-03（`:1123-1136`，partial submission、negative return、per-operation ownership）、ERR-02（`:562-579`）；`submit_transport_locked`（R3 `src/async/uring_transport.cpp:218-238`）；`account_transport_result_locked`（`:240-278`）；`reconcile_ledger_with_kernel_locked_`（`:298-335`）；`TransportLedger` 的单调 sequence/prefix pop（`src/async/uring_internal.hpp:150-213`）；batch flush（`src/async/uring_backend.cpp:587-591`）。
- **未解问题**：`report.accepted` 统计的是“已接受”，不等于“已被内核看见”；partial submit 时 ledger 仍有尾部。必须证明内核按 SQE 顺序消费前缀、ledger 的 `pop_front` 恰好对应这个前缀、每个 accepted key 只被计一次；control SQE 与 data SQE 交错时不能把 data 归属错位；residue 必须可重试且不伪造完成。前轮 `partial_submit_advertises_and_retries_residue`（`correctness/CORRECTNESS.md:49`）只覆盖 2 条、rc=1；未覆盖 8 条、control 交错、rc=0/负数多成员。
- **义务**：partial submit 的 consumed 前缀必须与 ledger 前 k 个 entry 的 `(sequence, kind, cookie, handle)` 一致；未消费的 accepted key 留在 ledger 且下一次 flush 只提交剩余前缀；不得对已消费项重发/重复 terminal；control entry 的消费按 ledger 顺序独立核销；report 的 `residue_advertised` 必须为真。
- **触发交错**：`io_uring_submit` 返回 `rc=k`（0<k<N）、`rc=0`、`-EINTR`/`-EAGAIN`/`-EBUSY`（可能在已消费一部分后）；batch 8 中夹杂 cancel control；下一次 flush 与 CQE 到达交错。
- **所需确定性观测/反例**：脚本化 submit hook 记录每次 enter 的 `sq_ready` 与 SQE `user_data` 顺序；断言 ledger 前 k 项 key 等于内核消费 key、剩余 key 不重复；注入已消费 key 的 CQE 后逐条 terminal；下一 pass `flush_attempts` 只处理剩余；反例 = 已消费 key 再次出现 SQE、未消费 key 被报 success、ledger 与内核前缀错位。

### 3.8 OPEN-1.8 SQ full / cap / budget 尾部归属

- **位置**：`docs/explicit-io-contract.md` BOUND-01（`:1299-1321`）、REQ-02（`:623-627`）、BACKEND-02（`:1118-1121`）；R3 batch 的 SQ/control/limit（`src/async/uring_backend.cpp:520-540`）、tail 计算（`:598-610`）；`dispatch_one_locked`（`src/async/uring_transport.cpp:130-208`，其中 `:143` 在取不到 SQE 时会 mid-batch submit）；前轮 `d3_d16_budget_prefix...`（`correctness/CORRECTNESS.md:47`）与 `sq_full_transport_residue...`（`:48`）。
- **未解问题**：当 SQ 空闲小于 ready size 时，accepted 前缀必须是 `min(size, cap, sq_free)`，suffix 必须从 `untouched_begin` 起按原顺序；cap=8 与 sq_budget 可同时截断；`dispatch_one_locked` 可能在 batch 内触发 submit（`src/async/uring_transport.cpp:143`），改变 flush 边界，但 batch 的 `report.flush_attempts` 只在 `:587-591` 的尾部 flush 时置 1，未计入 mid-batch `submit_transport_locked()`；这会直接污染 enter 归因。前轮测试覆盖“ring depth 4、组 6、accepted 4/stop sq_budget/untouched 2”和“SQ 满+ledger 残渣 accepted=0”，但未覆盖“SQ 部分空闲 + 已有 ledger 残渣/control SQE + batch”这种尾部归属，也未校验 mid-batch flush 的计数。
- **义务**：accepted 前缀与 untouched suffix 必须精确、按输入顺序、无重叠；被 accepted 的成员不得再出现在 suffix；suffix 后续单条提交必须从 `untouched_begin` 开始；cap/sq_budget/control/recovery/ineligible 的 stop 与 `untouched_begin` 一致；mid-batch submit 不得使已 accepted 的成员丢失或重排。
- **触发交错**：SQ free=1..7 时提交 8；queue depth 4、组 6；cap 8、输入 16；已有 ledger/control 占用 SQ；`dispatch_one_locked` 取 SQE 失败后 `submit_transport_locked`；control 在 flush 前后的插入。
- **所需确定性观测/反例**：submit hook 的 `sq_ready`/SQE 顺序、`dispatch_size_for_test`、`transport_ledger_size_for_test`；report accepted/stop/untouched_begin/untouched；逐 key 唯一性；suffix 的 `(slot,offset,len,dst)` 与 `ready[untouched_begin..]` 完全一致。反例 = suffix 从 accepted 内部开始、accepted 成员被跳过/重复、tail 与 report 不符。

### 3.9 OPEN-1.9 retry 责任与 residue 信号

- **位置**：`docs/explicit-io-contract.md` PROG-01（`:828-863`）、PROG-02（`:865-912`）、PROG-04（`:937-951`）；R3 batch residue（`src/async/uring_backend.cpp:587-620`）；`dispatch_retry_remains_locked_`（`src/async/uring_transport.cpp:210-216`）；`kTransportRetryInterval` 与 `wait_one` retry（`src/async/async_io_context.cpp:18-22`、`:641-652`）；`poll()` 的 dispatch/flush（R3 `src/async/uring_backend.cpp:835-856`）；前轮 fault partial（`tests/uring_ready_batch_fault_test.cpp:158-204`）。
- **未解问题**：accepted 但未进入内核的 entry 可能留在 `dispatch_` 或 ledger；`report.residue_advertised` 只说明“有残渣”，没有证明“谁负责 retry、何时 retry”。PROG-01 要求 dispatch/retry 属于 progress owner 的 immediate work；PROG-02 要求等待前有进一步 pass 的显式安排。前轮 fault oracle 只覆盖 scripted partial rc=1，不覆盖 `rc=0`/`-EAGAIN`/`-EINTR`/`-EBUSY`，也不覆盖 caller 不再调用时的 driver retry。
- **义务**：accepted 请求的 retry 是 backend/driver 的责任，不是调用者的责任；任何 residue 必须 signal 且被报告为 `dispatch_retry_remains`（或等价 immediate fact）；driver 必须安排后续 pass；不得要求调用者重新提交 accepted key；retry 不得 busy-spin 超过冻结 cadence；POLL/外部 loop 必须能看见该 residue。
- **触发交错**：batch flush 返回 0、`-EAGAIN`、`-EINTR`、`-EBUSY`；`dispatch_one_locked` 取不到 SQE；caller 提交后退出；driver 在 park 前最后一次 pass 看到 ledger 非空；external loop 的 epoll timeout 与 retry 交错。
- **所需确定性观测/反例**：scripted submit 序列；断言 `residue_advertised=true`、`ProgressPass.dispatch_retry_remains=true`、eventfd/唤醒可观察、下一次 owner pass 的 `flush_attempts` 与 ledger 前缀精确；`wait_one` 的 deadline/retry 不 spin；反例 = residue 为真但无 signal/无后续 pass，或调用者必须重提 accepted。

### 3.10 OPEN-1.10 accepted 不得作为 suffix；整组不得重发

- **位置**：`BUILD.md` §3.2（`:83-96`）；R3 report（`src/async/uring_backend.cpp:598-610`）；adapter 的 accepted/suffix（`bench/r3_ready_batch.hpp:118-131`、`:151-164`）；`dispatch_one_locked` 的 `remove_exact`（`src/async/uring_transport.cpp:196-205`）；ledger 的 `pop_front`（`src/async/uring_internal.hpp:193-213`）；前轮 cap/suffix（`correctness/CORRECTNESS.md:47`）。
- **未解问题**：`accepted` 是前缀、`untouched_begin` 之后是 suffix，rejected 项位于 `accepted` 且不算 untouched；但跨 partial/ cancel/poison/cap/ sq_budget/control/ineligible 的组合没有一条统一证明“accepted 永不作为 suffix 再提交、整组永不被重发”。若调用者重放整个 ready span，或 backend 把 accepted 重新放回 dispatch_，会产生重复 SQE/重复 terminal。
- **义务**：`accepted` 与 suffix 的 `(slot,offset,len,dst,generation)` 集合必须不相交；rejected index 不在两者；每个 accepted key 只物化一个 Request、只提交一次 SQE；backend 的 retry 只消费 ledger/dispatch 中“尚未取过 SQE”的 entry，不得把已消费项放回；调用者只能从 `untouched_begin` 继续。
- **触发交错**：cap 8/16、sq_budget、control、ineligible 首成员、partial + suffix、cancel 一个成员、poison 后重试；调用者错误地重放整组（负控应证明后端不重复，而不是要求调用者正确）。
- **所需确定性观测/反例**：在每次 entry/submit 记录 `(slot,offset,len,dst,generation)`；断言 accepted 集合 ∩ suffix 集合 = ∅、每个 RequestId 的 SQE 次数 = 1、rejected index 不在 accepted/untouched；整组 replay 后 ledger/queue 不出现同一 key 两项；反例 = accepted key 出现在 suffix/两个 SQE，或同一 key 两个 terminal。

### 3.11 OPEN-1.11 cancel/control 与批量交错

- **位置**：`docs/explicit-io-contract.md` CANCEL-01（`:1041-1061`）、REQ-03（`:645-654`）、THREAD-01（`:1008`）；R3 batch 的 control 门（`src/async/uring_backend.cpp:520-524`）；`cancel_key`（`src/async/uring_transport.cpp:584-618`）；`issue_running_cancel_locked_`（`:620-693`）；`service_pending_controls_locked_`（`:695-711`）；前轮 `cancel_and_cqe_ordering`（`correctness/CORRECTNESS.md:50`）与 fault `control_intent_stops_the_batch`（`tests/uring_ready_batch_fault_test.cpp:242-320`）。
- **未解问题**：batch 入口检查 `pending_control_count_ != 0` 就整组 untouched；但一个正在等待 `dispatch_mtx_` 的 cancel 尚未计数，batch 可能先接受/派发；若 cancel 在 claim 前获胜，`dispatch_one_locked` 的 `claim_execution` 失败会 `remove_exact` 并返回 true；若在 claim 后，cancel 只能 `requested`，control SQE 可能在 batch flush 之后才准备且未立即 submit；sticky intent 在 SQ full 时保留。必须证明任何交错下恰好一个 terminal、cancel disposition 合法、control 不丢失、batch 不 strand、重复 cancel 幂等。
- **义务**：cancel/control 与 batch 的每个交错都必须满足 CANCEL-01：`won-before-execution` 仅在能证明无 dispatch/effect 时；`requested` 不得静默丢弃 intent；control 容量不足返回显式 retryable 或保留 sticky intent；control SQE 最终 serviced；batch 不得把 cancel 成员再作为 suffix 提交；control 与 data 的 CQE 顺序合法；重复 cancel 只保留一个 control record。
- **触发交错**：cancel 在 batch 入口前、admit 循环中、dispatch claim 前后、flush 前后、service_pending_controls 前后；SQ full 导致 control SQE 分配失败；repeated cancel；cancel CQE 与 original CQE 乱序；poison 时仍有 pending intent。
- **所需确定性观测/反例**：pause gate + `pending_control_snapshot_for_test()`（derived vs authoritative parity）；cancel disposition；control/data CQE 注入顺序；`live_control_sqes`、ledger、`dispatch_size`；断言一个 terminal、一个 control record、control 最终提交/清算；反例 = disposition=requested 但无 control SQE、control 永远不 serviced、batch 把 canceled key 再提交。

### 3.12 OPEN-1.12 overflow/poison 下的成员归属与结算

- **位置**：`docs/explicit-io-contract.md` BACKEND-03（`:1123-1144`，poison/overflow/lost-completion）、ERR-02（`:562-579`）、CANCEL-01（`:1057-1061`）；R3 batch poison 分支（`src/async/uring_backend.cpp:567-594`）；`poison_and_recover_locked`（`src/async/uring_transport.cpp:337-443`）；overflow 处理（前轮 `correctness/CORRECTNESS.md:52`）；前轮 fault `poison_on_batch_flush_settles_every_member`（`tests/uring_ready_batch_fault_test.cpp:206-240`）。
- **未解问题**：batch 可能一次提交 8 条；CQ 溢出时完成可能停在 overflow list；partial submit 后 poison 时，内核已看见的成员保留真实完成路径，不可见成员只能报 physical failure，绝不能伪造 success，也不能提前释放 borrow。前轮场景 9 是 4×8 批、32 完成/16 CQ；场景 10 和 fault poison 是 2 成员；未覆盖 8 成员 + cap/suffix + control + overflow 的组合，也未覆盖 shutdown 与 overflow 同时存在。另有一个 disposition 缺口：若 poison 发生在 batch 的 flush 内，当前代码的 `report.stop` 仍可能由 accepted 数量决定成 `input_exhausted`/`cap`（`src/async/uring_backend.cpp:567-594`、`:598-610`），而不是 `recovery`；`report.error` 也未设置。必须证明或修正该报告，避免把 health failure 报成正常完成边界（accepted 成员各自的 terminal 仍需由 core 发布）。
- **义务**：overflow 不得吞掉任何已发生完成的 terminal；poison 只对 provably kernel-invisible 的 ledger 前缀做 Class-A recovery；已可见成员保留真实 CQE 路径；不可见成员报 backend_error/uncertain（按 ERR-02），绝不 success；borrow 只在 publication 后释放；shutdown(drain) 必须结算所有 accepted；不得重复 terminal。
- **触发交错**：batch 8、CQ 16 溢出、partial consume k 后 `-EIO`；visible CQE 与 poison 交错；shutdown 在 overflow list 未 flush 时进入；control CQE 与 original CQE 乱序；成员 buffer 相邻/重叠。
- **所需确定性观测/反例**：scripted kernel + `inject_cqe_for_test`/`get_events` hook；断言 visible members 的 bytes/结果正确、invisible members `backend_error`（不伪造 success）、所有 Request terminal、shutdown completed、buffer sentinel 未被提前修改、overflow 计数/`get_events` ≥1；反例 = 不可见成员 success、visible CQE 丢失、borrow 提前释放、shutdown 后仍有 pending。

### 3.13 OPEN-1.13 stale generation / slot reuse / borrow

- **位置**：`docs/explicit-io-contract.md` REQ-01（`:583-601`，generation 不回绕/不 alias）、REQ-05（`:673-687`）、REQ-04（`:656-671`）；R3 report keys 的固定数组（`include/sluice/async/detail/uring_backend.hpp:102-111`）；adapter `make_request`（`bench/r3_ready_batch.hpp:118-121`）；core accept 的 `binding_live`/generation（`src/async/detail/request_core.cpp:184-212`）；`reset_delivery_locked_`（`src/async/uring_backend.cpp:625-634`）；前轮 `stale_generation_after_consumed_batch`（`correctness/CORRECTNESS.md:56`）。
- **未解问题**：batch keys 携带 `(context,slot,generation)`；consume 后槽复用必须推进 generation，旧 key 必须 stale；batch 不得复用仍被 accepted key 占用的槽；cancel/poison 在 accept 与物化之间发生时，旧 key 不得 alias 新 generation；borrow 在 publication 前不得被复用。前轮场景 13 只覆盖 2 条 batch 的 consume+reuse；未覆盖 cap/suffix 8 条、partial、cancel/poison、observer armed 的组合。
- **义务**：每个 accepted key 的 generation 在当前 binding 存活期间唯一；旧 key 在 consume/discard 后 `lookup`/`cancel` 返回 stale/not-found；新 batch 不得复用未释放槽；slot 复用必须推进 generation；publication 后不再触碰旧 buffer；observer 的 generation 检查不能被 batch 绕过。
- **触发交错**：第一批 consume 完成后第二批复用同槽；旧 key 在第二批提交后 lookup/cancel；cancel/poison 在 accept 与 make_request 之间；observer armed；capacity 边界。
- **所需确定性观测/反例**：逐 key generation 单调递增、旧 key `lookup==not_found`、新 key 全部唯一；`public_bindings`/`outstanding` 快照；buffer sentinel；observer `delivery_retired`；反例 = 旧 key 解析到新请求、同槽两个 live binding、publication 后旧 buffer 被写。

### 3.14 OPEN-1.14 bounded resources / progress storage

- **位置**：`docs/explicit-io-contract.md` BOUND-01（`:1299-1321`，progress-storage resource bound）、BOUND-02（`:1323-1336`）、BACKEND-04（`:1146-1161`，submission batching 是机制）；R3 `ReadyReadBatchReport` 的 `std::array<RequestKey,8>`（`include/sluice/async/detail/uring_backend.hpp:102-111`）；adapter 的 caller-owned `accepted`/`ready`（`bench/r3_ready_batch.hpp:84-132`、bench `:349-352`）；`AGENTS.md` “Mechanism justification”与“Testing discipline”。
- **未解问题**：新增 Context 内部研究入口/批量路径不得引入 queue/thread/timer/per-request allocation；batch 的 report 固定 8、caller 的 `ready`/`accepted` 必须预分配；progress-pass 的批次/账本存储必须 resource-bounded 于 accepted lifetime；如果入口内部复制/增长容器，或在 flush/retry 路径分配，违反 BOUND-01。
- **义务**：批量入口在热路径零堆分配（除调用者预分配）；`ReadyReadBatchReport` 固定大小；`dispatch_`/ledger 有容量界；progress pass 存储由 setup 预分配或 per-slot 数组；分配失败必须是 owned/observable outcome，不是 accepted work hazard；不得新增线程/timer/queue；RSS 峰值门后续按资源门测。
- **触发交错**：depth 64、ready 64、cap 8、suffix 56；20k 连续调用；allocation failure；长生命周期 accepted 结果；progress pass 与新请求并发。
- **所需确定性观测/反例**：`operator new`/allocation counter 或 ASan/LSan；静态检查 batch 热路径无容器增长；`sizeof(ReadyReadBatchReport)` 固定；RSS/thread/queue 测量（计时轮）；反例 = 每 op 堆分配/RSS 线性增长/新增线程或 timer。

### 3.15 OPEN-1.15 测量映射、归因与门槛

- **位置**：`CONTEXT.md` §8（`:94-104`）、§8.1（`:106-110`）、§9-10（`:112-125`）；`PROTOCOL.md:200`（P-5）；`docs/performance/methodology.md:10-46`；`RESULTS.md:47-58`（全部 `NOT_RUN`）；`VERDICT.md:100-105`（旧 B/R2 历史）；`#604`。
- **未解问题**：即使五臂定义已写入，仍必须用配对数据回答三个问题；A0/A1/B 的组准备/调度/driver/worker 必须完整计入 CPU；W-03 是合成 benchmark；R2 旧数值不能复用；SMT 兄弟/频率/权限必须记录；A/A/负控/资源门/产品门尚未执行。
- **义务**：按冻结协议做 24 对 AB/BA、A/A、负控；每 invocation 记录 governor、cpu2 `scaling_cur_freq`、`df -h /tmp`、二进制 SHA、exit code、cpu12 状态；whole-executor CPU 包括 submitter + kernel + io-wq/worker，不能只看用户态；回答：
  1. **W-02 D2 是否守住 IMMEDIATE**：A_R2 vs B_R2 同二进制，`W02-uring-4k-d2`，CPU/op、tput、p50/p95/p99；若 B_R2 仍劣于 A_R2，则“旧 B 在 D2 差”复现；若不再差，需给出与 R2 历史差异的原因，但不能外推到 R3 新算法。
  2. **W-03 d16 是否在完整计入组准备/调度/driver/worker 后降 CPU 并守住尾延迟**：A0 vs A1 vs B on `W03-uring-4k-d16`，CPU/op、tput、p50/p95/p99；若 B 未降 CPU 或尾延迟恶化，不得产品化。
  3. **新算法是否消除旧 B 的 D2 退化**：A0/A1/B on `W03-uring-4k-d2-r3d2serial`（单条补发负控）与 `W03-uring-4k-d2-r3d2pos`（两 ready 正控）；单条时 B 必须与 A1/A0 等价（不得引入 defer/组开销），两 ready 时 B 才可合并；这是机制形状回答，不是 W-02 产品回答。
  4. **跨组大小/容量边界**：W-03 d4/d64（产品 case）上的 enter/CPU/尾延迟与 cap/suffix 归属；d4 应看到较小组合并，d64 受 cap=8 截断，用于解释机制上限，不作为产品胜出门。
- **触发交错/运行**：任一次跨臂比较；不同二进制/不同 SHA/cpu12 被占/schedutil 大幅漂移/未持锁/未做 A/A；把 smoke、单次、syscall 计数当性能结论。
- **所需确定性观测/反例**：原始 `results.jsonl`、配对 bootstrap 10k/seed 5950 的 CI、A/A 半宽 5/3/3 pp、负控 5/5/5/3、退出码/ops/case 一致性、二进制 SHA、governor/freq/df、lock 记录；反例 = A/A 超门仍比较、跨机/跨二进制比较、只报 smoke 计数、缺 cpu12/频率记录。

### 3.16 OPEN-1.16 #604 真实应用产品门（blocked）

- **位置**：Issue #604 body 的 G0 “Workload admission”行（本轮只读 `gh issue view 604 --json number,state,updatedAt,title`：`OPEN`，`updatedAt=2026-11-03T20:37:04Z`；body 无 `READY_FOR_595` handoff）；`VERDICT.md:159`（limitation 9）、`:169-177`（下一阶段条件）；`README`/`RESULTS` 的 W-03 分类。
- **未解问题**：#604 尚未给出准入的真实应用 workload/handoff；W-03 external loop 只是 benchmark 内的 `REAL_EXPOSURE` 代表 trace，不是 #604 准入真实应用。因此产品门无法由 W-03 结果替代。
- **义务**：任何 `PRODUCT_CANDIDATE_READY`/产品候选 PR 必须等待 #604 G0 workload admission（真实 consumer 或可辩护代表 trace、冻结 oracle/依赖/预算/baseline）与 #595 的 `READY_FOR_595` handoff；W-03 只能作为机制/代表形状证据。
- **触发交错**：有人用 W-03 d16 的收益/无退化直接改 `KEEP_IMMEDIATE`、开产品 PR 或写产品净胜门通过。
- **所需确定性观测/反例**：#604 的 handoff 标记/评论、G0 准入记录、真实应用 oracle/预算/baseline、负控；反例 = 仅 W-03 数据 + 声称产品门通过；当前状态就是反例（无 handoff，不能产品化）。

### 3.17 OPEN-1.17 A0 复原与旧结果重验

- **位置**：`VERDICT.md:98`（A0 不是 baseSha 原始路径）、`:145`（V-9）；`RESULTS.md:258`；baseSha `bench/bench_w03_external_loop.cpp:339-356`；R3 当前 `bench/bench_w03_external_loop.cpp:347-425`；`CONTEXT_COMPLETION.md` §1.6 的二进制 SHA 与 §3 的重验触发。
- **未解问题**：前轮 A0 先构造 `ready` span（R3 `:359-370`）再逐条 `ctx.submit_read`（`:409`），因此含原始 baseline 没有的组准备/`std::vector` 开销；前轮任何 A0 结果（smoke/计数）都不是原始 A0。当前 worktree 与 `r3-final.patch` 相同（见 §5），但续跑实现 A0 复原/共享入口会改变 bench/context；旧正确性、census、二进制 SHA 不能无条件复用。
- **义务**：A0 按 §2.2 复原；重跑受影响 census/enter 计数与确定性正确性；重新记录二进制 SHA；不得把前轮 A0 数字当原始基线；任何新 patch 与 `r3-final.patch` 的差异必须显式列出并触发对应重验。
- **触发交错**：构建 completion 二进制；引用前轮 A0/smoke/组计数；改动 `bench/`、`src/async/async_io_context.cpp`、`src/async/uring_backend.cpp`、`include/sluice/async/detail/*`、`xmake/tests.lua`、`tests/uring_ready_batch_*`。
- **所需确定性观测/反例**：A0 循环 diff（只允许 `max_refill` 边界，不允许 ready span）、strace `io_uring_enter` 计数、正确性 BASE/变异/回归重跑、二进制 SHA；反例 = A0 仍构造 `ready` span/组缓冲或每轮分配。

---

## 4. 主形状、假设与主门差异（原方案 → 续跑方案）

| 维度 | 原 R3（前轮） | 续跑 completion | 差异性质 |
|---|---|---|---|
| 主形状 | D2 两 ready（`W02-uring-4k-d2`）；主产品形状 W-02 | **W-03 d16** 为主形状；D2 单条补发为负控、D2 两 ready 为正控 | 主形状迁移到有 `REAL_EXPOSURE` 的 W-03 |
| 产品门 | 名义 `B−A0` 产品净胜门，但 W-02 主 shape 无真实批暴露；#604 未 handoff | **#604 真实应用为产品门**；W-03 只是代表 trace；W-02 D2 用于回答“是否守住 IMMEDIATE” | 产品门从 benchmark 主 shape 改为真实应用准入 |
| A0 | adapter 路径，先构造 ready span；含共同准备开销（`VERDICT.md:98`） | baseSha 原始逐条循环；不构造 span/不预扫描/不分配组缓冲 | 控制臂修正 |
| A1 | `ctx.submit_read` 逐条；与 B 不同入口 | 与 B 同一 Context 内部研究入口，逐条 span=1；相同 ready 准备 | 去除 B 的 context bypass 残差 |
| B | 直接 backend seam、cap=8、suffix 逐条；绕过 context | 同一 Context 入口整组调用；同一调用内 accept→prepare→dispatch→一次 flush；单成员等价 immediate；suffix 逐条 | 机制位置从 backend 直调移到 Context 入口 |
| A_R2/B_R2 | 无定义 | 271b81bf + R2 patch 的 A/B 两臂，仅 R2 原文/源码；W-02 D2 | 新增同 SHA 旧机制对照 |
| 尾部策略 | cap=8、suffix 逐条 | 不变（B 冻结为 cap=8、suffix 单条；任何 suffix 二次批量是新臂） | 保持可归因，不偷偷优化 |
| 测量状态 | 全部 `NOT_RUN`（`PROTOCOL.md:37-46`、`RESULTS.md:47-58`） | 本文件仍 **NOT_RUN**；只冻结定义/义务 | 本轮不产生性能数值 |

**关键假设（必须在计时前核验）**：

1. A0/A1/B 是同一 R3 二进制；A_R2/B_R2 是同一 R2 二进制；每个臂的 selector 实际生效。
2. A1/B 的 ready span 在第一次提交前已经合法拥有（`CONTEXT.md` §6）；D2 单条 case 的 `max_refill=1` 不构成“新增等待/造组”。
3. B 的“组”是 consumer 已有的 refill burst，不是预取/等待造组；W-03 的 `consume-all-ready` 产生多 ready。
4. 计时主机仍是 E5；cpu12 空闲/被征用；schedutil 频率记录；A/A 门先过。
5. 旧 R2 历史数值（`RESULTS.md:38` 等）只作历史，不在本轮复用。

---

## 5. `r3-final.patch` 与当前 worktree 的只读核验；本轮必须重验的旧结果

### 5.1 只读核验（本轮实际执行）

```bash
sha256sum /home/jnhu/sluice-595-e5/r3/evidence/r3-final.patch
# 10ce088a4b3219ba57ee0e63fa8f46d506ac763abc73eea073f833ecb8ce1622

cd /home/jnhu/sluice-595-e5/r3/worktree
git apply --reverse --check /home/jnhu/sluice-595-e5/r3/evidence/r3-final.patch
# exit 0

git status --porcelain
# 6 M + 4 ??
git diff --stat 0a22a791
# 6 files, +514/-51（不含 4 个未跟踪文件）
git apply --stat /home/jnhu/sluice-595-e5/r3/evidence/r3-final.patch
# 10 files, +2883/-51
```

结论：**当前 worktree 与 `r3-final.patch` 无差异**。`r3-final.patch` 的 10 个文件（6 M + 4 新文件）正好覆盖当前状态；reverse-check 成功说明 post-image 完全一致。当前 worktree 是**前轮最终态**，不是 completion 实现；本轮的臂定义/共享入口/A0 复原尚未落代码。

前轮 `r3-final.patch` 与 `r3.patch` 的关系：`r3.patch` 已过期（`PROTOCOL.md:202` P-7），不得用于 reverse-check 或重放；`r3-final.patch` 才是前轮最终态。旧 `MANIFEST.sha256`/二进制 SHA 是前轮状态，不得冒充 completion 状态。

### 5.2 必须重验的旧结果

| 触发 | 必须重跑 | 依据/原因 |
|---|---|---|
| 改 `src/async/async_io_context.cpp`（共享入口）或 `include/sluice/async/detail/uring_backend.hpp`/`src/async/uring_backend.cpp` | 确定性正确性 BASE 13 场景、3 个命名变异 RED、RESTORED GREEN；research/fault oracle；6 个既有回归；`shutdown_lifecycle_test`；二进制 SHA/清单 | `CONTEXT_COMPLETION.md` §3 触发 1；`correctness/CORRECTNESS.md:102-132`；共享入口改变 context/backend 路径 |
| 改 `bench/bench_w03_external_loop.cpp`（A0 复原、arm/case/计数） | W-03 canonical census、`strace io_uring_enter` 计数、byte oracle、arm 计数；二进制 SHA | `CONTEXT_COMPLETION.md` §3 触发 2；`READY_EXPOSURE.md` 的 enter/组分布依赖 consumer 源码与二进制 |
| 任何 rebuild | 重新记录 5+ 二进制 sha256；若测量引用二进制，需重新绑定 SHA；A/A 重新校准 | `CONTEXT_COMPLETION.md` §3 触发 3；`CONTEXT.md` §5 硬约束 7、§13 注意 3 |
| 引入正式计时 | 先写预注册（臂、配对、A/A、负控、统计、停手条件）；全程持 `MEASUREMENT.lock` | `PROTOCOL.md` P-2/P-5（`:197`、`:200`）；`CONTEXT.md` §8-§10 |
| 引用前轮 A0/smoke/组计数 | 不得直接复用；A0 必须按 §2.2 复原；smoke 计数受调度影响不是常量（`PROTOCOL.md:205` P-10） | `VERDICT.md:98`、`RESULTS.md:258`、`correctness/CORRECTNESS.md:58-66` |
| 引用 R2 历史 D2 数值 | 必须重建 R2 二进制、重做 R2 A/A、再跑 A_R2/B_R2 24 对；历史值只作对照 | `docs/research/595-causal-r2/RESULTS.md:38`；R2 `PROTOCOL.md:38-48`；`VERDICT.md:104` |
| 引用前轮“semanticsOk=true” | 不继承；那是实现者自检（`BUILD.md:8`），不是 verifier 证明 | `BUILD.md:8`、`BUILD.md:12` |
| 前轮 P-5 锁偏离 | 续跑所有 build/test/timing 必须持锁；不得重复 | `PROTOCOL.md:200`；`correctness/CORRECTNESS.md:146-148` |

**可复用的部分（条件）**：

- 前轮确定性正确性结论、census 结论、`MECHANISM_ONLY`/`KEEP_IMMEDIATE` 裁决，只有在相关源码/测试/xmake/bench/二进制逐字节不变时才可复用（`CONTEXT_COMPLETION.md` §2-§3）。completion 的 A0 复原/共享入口/A_R2/B_R2 都会改变部分文件/二进制，因此**不能整包复用**。
- 前轮 `r3-final.patch` 的 reverse-check 结果可证明当前 worktree 是前轮最终态；它不是 completion 实现。

---

## 6. 明确未做与限制

1. **未跑 benchmark**：本轮没有 A/A、A/B、负控、资源门、真实应用计时；没有产生任何 CPU/时延/吞吐数值；不得把 `BUILD.md`/`RESULTS.md` 的 smoke 数字当性能结论。
2. **未编辑产品代码**：只读 `include/`、`src/`、`bench/`、`tests/`、`xmake/`、docs；只写本文件与 `CONTEXT_COMPLETION.md` 的追加章节；未改 worktree、未提交、未 push。
3. **未证明语义**：`semanticsOk` 仍是实现者自检（`BUILD.md:8`）；本文件把每个交错写成 open obligation，不宣称任何一项已 proved。
4. **#604 阻塞**：Issue #604 当前 `OPEN`（`updatedAt=2026-11-03T20:37:04Z`，本轮 `gh issue view` 只读），body 无 `READY_FOR_595` handoff；W-03 是合成 benchmark，不是 #604 准入真实应用。产品门因此 **blocked**，不是靠 W-03 数据能解。
5. **R2 二进制重建**：A_R2/B_R2 必须在 `271b81bf + 7be9d014…` 树上重新构建；R2 旧二进制/旧 A/A 不得复用。若重建后 SHA 变化，旧 R2 数值全部作废。
6. **A0 的 `max_refill` 语义**：产品 case 逐字等于 baseSha 循环；研究 D2 case 仅把 `max_refill` 用作 refill burst 上限，A0 仍逐条构造并立即提交。若实现把该上限变成预构造 span，A0 失去“原始基线”资格，必须重新定义/重新计时。
7. **`have_ring_` 竞争**：OPEN-1.4 是当前最强的未解安全/并发项；在它解决前，不能声称批量入口在 shutdown 并发下安全（前轮 `BUILD.md:302` 也只关闭了部分窗口）。不得用“shutdown 不应与 submit 并发”的口头假设代替契约/实现证明。
8. **SMT/频率/权限**：cpu12 与 cpu2 共享物理核；`schedutil` 浮动；`perf_event_paranoid=2`、`unprivileged_bpf_disabled=2`。计时轮必须记录并处理，不能用不可用的 root tracing 代替独立 oracle。
9. **清单**：本轮新增两份文档尚未写入 completion 的 `MANIFEST`（本 ask 未要求生成清单）；后续交付者应把 `OPEN1_RECEIPTS.md` 与追加后的 `CONTEXT_COMPLETION.md` 纳入清单并校验。
10. **口径边界**：W-03 d16 的收益/无退化只回答“consumer-side ready batching 在代表形状上是否值得继续”；不能外推到 W-02 主 shape、#604 真实应用、块设备/cold cache/O_DIRECT、其他 block size、threadpool backend、跨线程并发 submit 的完整矩阵（`VERDICT.md:149-164`）。

---

## 7. 本轮写入的续跑上下文

- `CONTEXT_COMPLETION.md` 追加：§5 五臂冻结定义、§6 证明义务索引、§7 主形状/门差异、§8 重验触发与限制。
- 本文件：OPEN-1 缺口、逐项义务、patch 对比与重验清单。
- `open1Resolved = true` 只表示定义/义务已写入；**不表示** `semanticsOk = true`，也不表示任何交错已证明。
