# #595 R3 — 就绪组机会普查（READY_EXPOSURE）

| 字段 | 值 |
|---|---|
| 角色 | R3 就绪组机会普查员（只读分析 + 隔离 worktree 内 test-only 诊断） |
| 基线 | `baseSha=0a22a79195f1b913145fcd443496e7f70b08cdcf`，worktree `/home/jnhu/sluice-595-e5/r3/worktree`（分支 `research-595-r3`） |
| 宿主 | E5；Fedora 44；kernel `7.2.11-200.fc44.x86_64`；Xeon E5-2666 v3 |
| 结论 | **主暴露形状（representative consumer）= W-03 external loop，benchmark 内 REAL_EXPOSURE**；W-03 是合成 benchmark，**不是 #604 准入的真实应用**。W-02 主 shape 仅 STARTUP_ONLY；sluice-copy 默认 depth=1 时为 NO_REAL_BATCH_EXPOSURE、depth>1 时仅 STARTUP_ONLY |
| 未做 | 未做性能计时、未跑正式 A/B、未改产品语义/公开头文件 |

本文件的所有 enter 数字均为 syscall 计数（`strace` + 独立诊断记录），不是时延或加速声明。唯一写入的仓库变更在隔离 worktree 的 `bench/`（test-only 诊断），`libsluice_async.a` 与冻结基线逐字节相同（`76e59bd6…`，见 `evidence/BASELINE.md` §5）。

前置读取：`evidence/CONTEXT.md`、`evidence/EVIDENCE_BASELINE.md`；`evidence/STATE_AUTHORITY.md` **未生成**（路径不存在），故本普查不引用该文件。

---

## 0. ReadyGroup 判定口径（来自 `evidence/CONTEXT.md` §6）

一组读在**第一次基线提交之前**已经同时满足：文件已拥有；offset/长度已确定；有效 buffer 已验证；该读依赖的条件已确定、不需要未来事件。禁止用新增等待、前移依赖工作或追加预取造组。

本普查的独立 oracle 是 **producer 操作图**：从 consumer 自己的状态（`window.size()`、`submitted`、`depth`、`blocks`、已打开 fd、预分配 buffers）可以唯一确定每个成员 `(slot, offset, len, dst)`；诊断在提交前记录这些预测值，再与真实提交逐项比对。若某成员依赖另一成员的结果，或组内出现 wait/take，则该组无效。

---

## 1. consumer 筛选

| Consumer | 原调用点（baseline `0a22a791`） | 现有行为 | 可证明的组 | 分类 |
|---|---|---|---|---|
| `bench/bench_w02_pipeline.cpp` | 填充/补发 `:337`，`scope.submit_read` `:348`，有序等待 `:384` | 每次外循环只 `wait_for(oldest)` 并结算 1 条，再补 1 条 | 仅初始填充 `depth` 条；稳态每轮 1 条 | **STARTUP_ONLY** |
| `bench/bench_w03_external_loop.cpp` | 补发 `:339`，`ctx.submit_read` `:349`，现有完成等待 `:370`，consume-all-ready `:387` | 等待后消费所有 front-ready 完成，再一次性补满窗口 | 初始填充 + **稳态每次补发** 都是多成员组 | **REAL_EXPOSURE** |
| `apps/sluice-copy/copy_task.cpp` | 预填 `:165-174`，`submit_slot_read` `:169`；稳态下一条读 `:228-233` | 预填 `pipeline_depth` 条；稳态每个 slot 的下一条读依赖该 slot 的写完成（`chunk_offset` 在写完成后才推进） | 仅预填；默认 `pipeline_depth=1`（`cli_parse.hpp:16`）连预填组都没有 | 默认 **NO_REAL_BATCH_EXPOSURE**；depth>1 时 **STARTUP_ONLY** |

筛选结论：只有 W-03 在**稳态**拥有可证明的多 ready 组，因此选它作为主暴露形状（representative consumer）。W-03 是合成 benchmark，不是 #604 准入的真实应用；其价值是提供可辩护的代表 trace，而不是应用级加速证明。W-02 的数字仍然保留，用来说明 #595 主 shape 的真实上限。

---

## 2. 主任务：W-03 external loop（REAL_EXPOSURE）

### 2.1 组为什么合法

在 `bench/bench_w03_external_loop.cpp:339` 的补发 burst 开始时，可见状态是：

- `window.size() = w < depth`，`submitted = s`；
- 组大小 `r = min(depth - w, request_count - s)`；
- 成员 `j=0..r-1`：`slot=(s+j)%depth`，`offset=((s+j)%blocks)*4096`，`dst=buffers[slot].data()`，`len=4096`；
- 源文件在 `:273` 已打开，`buffers` 在 `:316-319` 已分配并填充 pattern；
- 这些值只由 consumer 自己的计数器与常量决定，**不读取任何完成结果**；
- 组内没有 `wait_begin/wait_end`，也没有 `take` 事件（诊断逐组检查）。

因此组员在第一条基线提交前已经“合法可发”。这不是通过等待/预取造出来的组；组是在**既有**完成等待（`:370`）之后、既有 consume-all-ready（`:387`）结束时自然出现的补发 burst。补发大小不是固定的 1，而是本次唤醒时 front-ready 的完成数（1..depth）；这正是 §2.3 分布的核心。原型不得把该等待延长来“长成”更大的组。

### 2.2 具体拆批点（至少一个）

**位置**：`bench/bench_w03_external_loop.cpp:339`（补发循环），实际提交在 `:349`。

**当时可见状态**：`w=0, s=64, r=64`（d64 稳态 group 2）或 `w=0, s=16, r=16`（d16 稳态 group 2）。成员身份示例（d16 phase 1 group 2，来自 `raw/census/canonical/w03-d16-verbose.tsv`）：

- `group_ready ts=5079642908035`，`ready=16, window=0, submitted=16`；
- 16 个 `member_ready`：slot 0..15，offset 65536..126976，len 4096，16 个互不相同的 buffer；
- 该组 `group_ready` → 首个 `submit_begin` 为 **934 ns**；最后一个 `member_ready` → 首个 `submit_begin` 为 **63 ns**（5 个 verbose 运行合计 3,572 个组，last-member→first-submit 间隔 **32 ns–9,771 ns**，0 次反序）；
- 16 条 `submit_read` 全部成功，`group_done=16`。

**真实 enter 差额**：

- 基线：该组 16 条 `submit_read` → **16 次纯 submit enter**（每次 `io_uring_enter(fd, to_submit=1, min_complete=0, …)`，测量见 §3）；
- 按现有容量重切：`queue_depth=depth=16`（`bench/bench_w03_external_loop.cpp:115-116`，`io_uring_queue_init(queue_depth)` 于 `src/async/uring_backend.cpp:171`），`r=16 ≤ 16`，一次 submit 可容纳全部 16 个 SQE → **1 enter**；
- **差额 = 15 enter/组**。d4 的 group 2 是 `r=4`，差额 3；d64 的 group 2 是 `r=64`，差额 63。

**适用范围**：仅 W-03 external loop 的 consume-all-ready 补发 burst，4 KiB、warm tmpfs、uring backend；不适用于 W-02 默认稳态（有序单等待）或 sluice-copy 稳态（读依赖写完成）。若 consumer 每次唤醒只拿到 1 个完成，则该轮只有单成员组，不构成拆批点（见 §5 的 1–8% 单成员比例）。

### 2.3 就绪数分布（0 / 1 / 2 / 3–8 / >8）

稳态组（不含启动组；全量 20k，counts-only 诊断）：

| case / phase | 可行组数（含启动组；zero 另计） | 0 | 1 | 2 | 3–8 | >8 | 单成员占比 |
|---|---|---|---|---|---|---|---|
| W03-d16 ph1 | 1698（zero 80） | 0 | 83 | 116 | 264 | 1234 | 4.89% |
| W03-d16 ph2 | 1511（zero 27） | 0 | 128 | 54 | 86 | 1242 | 8.48% |
| W03-d16 ph3 | 1331（zero 1） | 0 | 16 | 5 | 9 | 1300 | 1.20% |
| W03-d4 ph1 | 5545（zero 80） | 0 | 256 | 408 | 4880 | 0 | 4.62% |
| W03-d4 ph2 | 5363（zero 27） | 0 | 167 | 283 | 4912 | 0 | 3.11% |
| W03-d4 ph3 | 5467（zero 5） | 0 | 65 | 96 | 5305 | 0 | 1.19% |

- 启动组：d16=16、d4=4、d64=64（每次 phase 一个）。
- 512 请求的 verbose 窗口（`raw/census/canonical/w03-*-verbose.tsv`）：d16 各 phase 恰为 16 的组数为 30/33、25/33、13/36，其余为 8–15；d4 的 128/128/128 组全部恰为 4；d64 的 phase 1 为 8 组全 64，phase 2/3 主要为 45–64。与全量分布一致（多 ready 主导，组大小受完成批大小影响）。
- minimal 记录（只记每组一个计数，避免 per-member 开销）复现同样的多 ready 主导：d16 ph1/2/3 的 `>8` 组分别为 1240/1257/1236，enter 节省 92.86%/91.45%/91.07%。

### 2.4 组内顺序、依赖、字节与请求身份

verbose 运行逐组检查（全部 0 违例）：

| case | 检查组数 | member 身份不符 | 组内 take | 组内 wait | slot/offset/dst 别名或重复 | 越界 | 组员数不符 | 反序次数（最后 ready ≥ 首 submit） |
|---|---|---|---|---|---|---|---|---|
| W03-d16（512） | 101 | 0 | 0 | 0 | 0 | 0 | 0 | 0（delta 54–3400 ns） |
| W03-d4（512） | 384 | 0 | 0 | 0 | 0 | 0 | 0 | 0（43–4642 ns） |
| W03-d64（512） | 27 | 0 | 0 | 0 | 0 | 0 | 0 | 0（55–180 ns） |
| W02-d2（512） | 1533 | 0 | 0 | 0 | 0 | 0 | 0 | 0（35–277 ns） |
| W02-d4（512） | 1527 | 0 | 0 | 0 | 0 | 0 | 0 | 0（32–9771 ns） |

“member 身份”指诊断预测的 `(slot, offset, dst, len)` 与真实 `submit_read` 完全一致；字节数对每组为 `r × 4096`。组内提交顺序为 member 0..r-1，无跨成员结果依赖；不同 slot 使用不同 buffer，offset 为连续 block，互不别名。phase 2 是 bench 自带的 byte-oracle pass（`verify=true`），17 次 canonical 运行全部 exit 0，说明这些读的结果按 pattern 校验通过。

---

## 3. 独立 enter oracle：基线与已有合发

用 `strace -f -e trace=io_uring_enter`（行级 trace 用 `-ttt`，计数用 `-c`）作为独立 syscall oracle：

| 运行 | io_uring_enter 次数 | 纯 submit（to_submit=1, min_complete=0） | context probe（min_complete=1） | to_submit>1（已合发） | 最大 SQE/enter |
|---|---|---|---|---|---|
| 冻结基线 W03-d16/d4/d64（20k×3 phase） | 60,004 | 60,000 | 4 | **0** | 1 |
| 冻结基线 W02-d4（20k×3 phase） | 60,004 | 60,000 | 4 | **0** | 1 |
| 诊断构建 unmarked W03-d16/d4/d64、W02-d2（20k×3 phase） | 60,004 | 60,000 | 4 | **0** | 1 |
| 诊断构建 verbose 512（每 case） | 1,540 | 1,536 | 4 | **0** | 1 |

- **口径（重要）**：20k×3 phase 的行只用 `strace -c` **直接测得总数 60,004**；表中“60,000 纯 submit + 4 probe / `to_submit>1`=0 / 最大 SQE/enter=1”的分解来自 512 请求逐行 trace（每个 case 1,536 次 `(to_submit=1, min_complete=0, flags=0)` + 4 次 `GETEVENTS` probe）加“每次 `submit_read` 一次 enter、context 构造有 4 个 probe”的代码/工作量推断，**不是 20k 逐调用测量**。20k 逐行 trace 未归档。
- 每个 `submit_read` 恰好对应一次 `io_uring_enter(to_submit=1, min_complete=0)`；`60,000 = 3 phase × 20,000` 是由 512 行级 trace 与工作量（20k×3）推出的分解。
- 4 次 probe 来自 context 构造时的 statx 探测（`submit_and_wait`），不属于提交路径。
- 所有行级 trace 中，enter 都发生在 submitter 线程（pid）；driver 线程 0 次 enter。现有 `dispatch/poll` 没有合发任何 SQEs：**已合发比例 = 0%**（0/60,000 纯 submit enter，0 个 SQE）。
- **诊断不改变提交行为**：同一诊断构建在记录开/关时均为 60,004；冻结基线（从 `baseSha` 源码重建，SHA 与 `BASELINE.md` §5 一致：W03 `929bc12f…`、W02 `78d0db9d…`）也是 60,004。

---

## 4. 净 enter 节省（按现有容量）

公式：一组 `r` 个成员，现有容量 `C=min(depth, ring queue_depth)=depth`，基线 `r` 次 enter，重切为 `ceil(r/C)=1` 次（本数据中 `r ≤ depth`），节省 `r-1`。

| case | 每 20k ops 基线纯 submit | 重切后 enter | 净节省 | 比例 |
|---|---|---|---|---|
| W03-d16 ph1 | 20,000 | 1,698 | **18,302** | 91.5% |
| W03-d16 ph2 | 20,000 | 1,511 | **18,489** | 92.4% |
| W03-d16 ph3 | 20,000 | 1,331 | **18,669** | 93.3% |
| W03-d16 minimal ph1/2/3 | 20,000 | 1,428/1,711/1,786 | 18,572/18,289/18,214 | 92.9%/91.4%/91.1% |
| W03-d4 ph1/2/3 | 20,000 | 5,545/5,363/5,467 | **14,455/14,637/14,533** | 72.3%/73.2%/72.7% |
| W03-d64（512 窗口）ph1/2/3 | 512 | 8/10/9 | 504/502/503 | 98.4%/98.0%/98.2% |
| W02-d2 ph1/2/3 | 20,000 | 19,999 | **1** | 0.005% |
| W02-d4（512 窗口） | 512 | 509 | 3（= depth−1，一次性启动） | 0.586% |

W-02 的净节省按 20k 记为 `depth−1` 次：D2=1、D4=3；即使用满 20k，D2 也只有 **1/20,000 = 0.005%**。这正是“只有初始填充可证明”的 STARTUP_ONLY 量级。

---

## 5. 影子筛选（第二条请求的到达、依赖、未出现样本）

| case / phase | 单成员稳态组 | 零 ready 组（窗口满、无补发） | 组间到达间隔 p50/p95（ns） | 组内提交间隔 p50（ns，含诊断开销） |
|---|---|---|---|---|
| W03-d16 ph1 | 83 | 80 | 201,307 / 281,164 | 77,241 |
| W03-d16 ph2 | 128 | 27 | 225,393 / 325,679 | 83,790 |
| W03-d16 ph3 | 16 | 1 | 110,584 / 130,146 | 98,384 |
| W03-d4 ph1 | 256 | 80 | 238,645 / 388,390 | 85,109 |
| W03-d4 ph2 | 167 | 27 | 202,794 / 273,048 | 78,243 |
| W03-d4 ph3 | 65 | 5 | 135,612 / 187,454 | 42,367 |
| W02-d2 ph1/2/3 | 19,998 | 1 | 42,708–49,103 / 50,456–65,413 | 41,135–53,917 |

解释：

- **依赖**：组内成员之间没有数据依赖；单成员组里的“下一条读”也不依赖上一条读的结果，但它的**发起点**受 consumer 既有完成等待（W03）或 oldest-ticket 等待（W02）门控。把它并入当前批次需要提前/新增等待，属于禁止的造组方式，因此**不计入**真实暴露。
- **未出现样本**：上表单成员组与零 ready 组就是“第二条 ready 请求未出现”的样本；W03-d16 的单成员比例 1.2–8.5%，即绝大多数稳态轮次第二条请求在首提交前已经 ready。
- **使用边界**：影子数据只用于筛选假设，不能证明“等一等会更快”；本普查没有新增任何跨调用等待。

---

## 6. 是否存在造组

**没有。** 具体核查：

1. 未新增等待/睡眠/重试/停车；W03 的 cv 等待（`:370`）与 W02 的 `scope.wait_for`（`:384`）保持原样。
2. 未前移依赖工作：诊断只读取 consumer 已有的 `window.size()`、`submitted`、`depth`、`blocks`、`buffers` 并记录预测的 `(slot, offset, dst)`；不构造 ReadOp、不代产品提交、不代产品消费。
3. 未追加预取：同一批读在同一位置提交，没有额外 preload/预读。
4. 产品库未变：`libsluice_async.a` SHA `76e59bd6…` 与 `BASELINE.md` §5 相同；`include/` 未改；主工作树 `git status --porcelain` 为 0 行。
5. 唯一 workload 变更：W02 的 D2/D3 case 只在 `SLUICE_CENSUS_D2=1` 时加入（复刻 R1/R2 研究补丁的冻结 shape；默认构建不变），不改变任何提交语义；短窗口通过 `SLUICE_CENSUS_REQUESTS` 仅减少重复次数。

诊断实现：`bench/census_diag.hpp`（header-only，运行时由 `SLUICE_CENSUS_LOG` 开启；`SLUICE_CENSUS_VERBOSE` 记录逐成员；`SLUICE_CENSUS_MINIMAL` 只记每组计数）。完整补丁在 `raw/census/diagnostic/bench-diagnostics.patch`，header 副本在同目录。

---

## 7. 限制与不得外推

1. 未做性能计时/正式 A/B；enter 数字不是时延或加速声明。
2. r 分布来自诊断构建；minimal 记录（每组一次 clock + 一次 push）复现多 ready 主导，且记录开/关的 enter 数完全相同；但绝对比例受工作负载/调度/phase 影响（例如 d16 ph2 单成员比例 8.5%，ph3 仅 1.2%）。
3. W03 的组位于既有完成等待之后；不得据此新增等待或延长 defer 来扩大批次。
4. **R2 B-arm 的 deferred-flush 门是 owner-gated**（submitter 必须是最近的 progress driver，见 `patch.diff:127-131` 与 `evidence/EVIDENCE_BASELINE.md` §2.4）；W03 的 submitter 不是 driver，因此 R2 原型不会捕获该组。要利用该拆批点需要 consumer 侧的多 SQE 准备/批量提交，而不是更长的 defer。
5. 只覆盖 4 KiB、warm tmpfs、本机；未覆盖块设备、cold cache、O_DIRECT、其他 block size 或 threadpool backend。
6. W02 的 D2/D3 是 census-only case（冻结基线 case list 无 d2）；sluice-copy 仅做源码级判定，未插桩运行。
7. 证据目录中的 `raw/census/exploratory/` 保留早期探索运行，不作为本结论的 canonical 依据；canonical 依据是 `raw/census/canonical/`、`raw/census/frozen/` 与 `raw/census/analysis/`。

---

## 8. 证据索引与命令

| 内容 | 路径 |
|---|---|
| 冻结上下文 / 定义 | `evidence/CONTEXT.md` §6；`evidence/EVIDENCE_BASELINE.md` §2.4 |
| 基线 SHA 与二进制摘要 | `evidence/BASELINE.md` §1/§5；`raw/binary-sha256.txt` |
| canonical 运行清单（17 条命令、exit 0、二进制 SHA、host/kernel） | `raw/census/canonical/run.log` |
| canonical 原始输出（out / strace / tsv） | `raw/census/canonical/` |
| 冻结基线原始输出（从 `baseSha` 源码重建，SHA 匹配） | `raw/census/frozen/`；二进制 `raw/census/bin/*.frozen` |
| 诊断构建二进制（canonical 运行所用，已从 build 目录另存） | `raw/census/bin/*.diagnostic`（W03 `c451dba4…`、W02 `4a27ac55…`） |
| 分析 JSON（每 phase 的分布/节省/校验/enter trace） | `raw/census/analysis/*.json` |
| 诊断补丁与 header | `raw/census/diagnostic/bench-diagnostics.patch`、`census_diag.hpp` |
| 运行脚本 / 分析脚本 | `harness/census_run_final.sh`、`harness/census_analyze.py` |
| 机器可读结论 | `evidence/census.json` |

代表性命令（完整 17 条见 `raw/census/canonical/run.log`）：

```bash
# 512-request verbose + 行级 enter trace（W03-d16）
env SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 \
    SLUICE_BENCH_CASE_FILTER='=W03-uring-4k-d16' \
    SLUICE_BENCH_TMP_DIR=/tmp/sluice-census-fixture SLUICE_BENCH_SKIP_DISK=1 \
    SLUICE_CENSUS_VERBOSE=1 SLUICE_CENSUS_REQUESTS=512 \
    SLUICE_CENSUS_LOG=.../w03-d16-verbose.tsv \
    flock .../MEASUREMENT.lock strace -f -ttt -e trace=io_uring_enter \
    -o .../w03-d16-verbose-enter.txt .../bench_w03_external_loop

# 20k counts-only（W03-d16）
env SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 \
    SLUICE_BENCH_CASE_FILTER='=W03-uring-4k-d16' \
    SLUICE_BENCH_TMP_DIR=/tmp/sluice-census-fixture SLUICE_BENCH_SKIP_DISK=1 \
    SLUICE_CENSUS_VERBOSE=0 SLUICE_CENSUS_LOG=.../w03-d16-full.tsv \
    flock .../MEASUREMENT.lock strace -f -c -e trace=io_uring_enter \
    -o .../w03-d16-full-strace.txt .../bench_w03_external_loop
```

复现诊断构建（隔离 worktree）：`xmake build bench_w02_pipeline bench_w03_external_loop`；补丁 SHA `6c501298…`，header SHA `51b116a2…`。所有运行均持有 `MEASUREMENT.lock`；未运行计时。

诊断运行结束后，worktree 的两个 bench 源文件已还原为 `baseSha` 原样、`bench/census_diag.hpp` 已移出（完整内容保存在 `raw/census/diagnostic/`），`git status --porcelain` 为 0 行；`build/linux/x86_64/release/` 下的两个 bench 二进制也已由冻结副本恢复（`bench_w02_pipeline` `78d0db9d…`、`bench_w03_external_loop` `929bc12f…`），因此 `evidence/MANIFEST.sha256`（含这两个 build 产物）重新 `sha256sum -c` 全通过。canonical 运行所用的诊断二进制另存在 `raw/census/bin/*.diagnostic`，SHA 与 `raw/census/canonical/run.log` 一致。复现时按 `raw/census/diagnostic/bench-diagnostics.patch` 应用补丁并放回 header 即可。

---

## 9. R3 实现者更正（2026-11-05）

§0 写「`evidence/STATE_AUTHORITY.md` **未生成**（路径不存在）」与事实不符：该文件存在
（`/home/jnhu/sluice-595-e5/r3/evidence/STATE_AUTHORITY.md`，mtime 12:02），早于本文件的
mtime 12:45。本普查的筛选、enter 计数与结论不读取该文件，因此结论不受影响；此条只更正
前置读取记录，原文保留不改写。

---

## 10. R3 裁决整合（2026-11-05，交付轮）

本节是**整合层**，只汇总 §1–§9 已落盘的普查结论并加入交付轮的独立复核；不新增计时，不改写原文数字。所有引用以本节表格为准；原文保留为细节证据。

### 10.1 结论（先读）

| 问题 | 结论 |
|---|---|
| 主暴露形状（representative consumer） | **W-03 external event loop**（`bench/bench_w03_external_loop.cpp`），4 KiB、warm tmpfs、uring backend，benchmark 内 **REAL_EXPOSURE**；**合成 benchmark，不是 #604 准入真实应用** |
| #595 主 shape（`W02-uring-4k-d2`） | **STARTUP_ONLY**：稳态每轮只补 1 条，20k ops 仅省 **1 enter（0.005%）** |
| `sluice-copy` | 默认 `pipeline_depth=1` → **NO_REAL_BATCH_EXPOSURE**；`depth>1` → 只预填组，**STARTUP_ONLY** |
| 是否存在“造组” | 否（§6：未新增等待/前移依赖/追加预取；产品库逐字节未变） |
| 是否可据此做产品声明 | **否**。普查只证明“有真实多 ready 组可合并”，不证明合并更快；R3 没有 CPU/时延/吞吐测量（见 `PROTOCOL.md` §6） |

### 10.2 consumer 与独立 oracle

| consumer | 调用点（baseSha） | 稳态组 | 分类 |
|---|---|---|---|
| `bench/bench_w03_external_loop.cpp` | 补发 `:339`，提交 `:349`，完成等待 `:370`，consume-all-ready `:387` | 每次 front-ready 补发 burst，大小 1..depth | **REAL_EXPOSURE** |
| `bench/bench_w02_pipeline.cpp` | 填充/补发 `:337`，`scope.submit_read` `:348`，有序等待 `:384` | 仅初始填充；稳态每次 1 条 | **STARTUP_ONLY** |
| `apps/sluice-copy/copy_task.cpp` | 预填 `:165-174`，`submit_slot_read` `:169`；稳态下一条 `:228-233` | 仅预填；读依赖该 slot 写完成 | 默认 **NO_REAL_BATCH_EXPOSURE**；`depth>1` **STARTUP_ONLY** |

**独立 oracle（两层）**：

1. **syscall 层**：`strace -f -e trace=io_uring_enter`。每个 `submit_read` 恰好一次 `io_uring_enter(to_submit=1, min_complete=0)`；`to_submit>1` 计数为 **0**（0% 合发）。
2. **结构层**：诊断预测 `(slot, offset, len, dst)` 与真实 `submit_read` 逐项比对；组内 `take`/`wait`/别名/越界/反序逐组检查。

### 10.3 就绪数分布（稳态，不含启动组；zero 另计）

数据：`raw/census/analysis/*-full.json` 的 `ready_distribution_steady`；本轮逐项复核。

| case / phase | 0 | 1 | 2 | 3–8 | >8 | zero-ready 组 | 稳态组合计 | 单成员占比 |
|---|---|---|---|---|---|---|---|---|
| W03-d16 ph1 | 0 | 83 | 116 | 264 | 1234 | 80 | 1697 | 4.89% |
| W03-d16 ph2 | 0 | 128 | 54 | 86 | 1242 | 27 | 1510 | 8.48% |
| W03-d16 ph3 | 0 | 16 | 5 | 9 | 1300 | 1 | 1330 | 1.20% |
| W03-d4 ph1 | 0 | 256 | 408 | 4880 | 0 | 80 | 5544 | 4.62% |
| W03-d4 ph2 | 0 | 167 | 283 | 4912 | 0 | 27 | 5362 | 3.11% |
| W03-d4 ph3 | 0 | 65 | 96 | 5305 | 0 | 5 | 5466 | 1.19% |
| W02-d2 每 phase | 0 | 19998 | 0 | 0 | 0 | 1 | 19998 | 100% |
| W03-d64（512 窗口） | — | 0 | 0 | 0 | 8/10/9 组 | — | 8/10/9 | 0% |

一致性校验：

- `groups = 稳态组 + 启动组(1) + zero-ready 组`。例：d16 ph1 `1697+1+80=1778`（JSON `groups=1778`）；d4 ph1 `5544+1+80=5625`；d2 `19998+1+1=20000`。
- 启动组：d16=16 成员、d4=4、d64=64、d2=2，每次 phase 一个。
- `>8` 在 JSON 的 `ready_distribution` 中含启动组；本表用 `ready_distribution_steady`，故比 §2.3 原表少 1（原表已注明“含启动组”口径）。
- verbose 运行（512 请求）的成员身份不符、组内 take、组内 wait、别名/重复、越界、组员数不符、反序**全部为 0**（9 个 phase 逐项复核 `*-verbose.json`）。

### 10.4 基线 enter 与具体拆批点

**基线 enter（syscall 实测）**：

| 运行 | `io_uring_enter` 总数 | 纯 submit | context probe | `to_submit>1` | 最大 SQE/enter |
|---|---|---|---|---|---|
| 冻结基线 W03-d16/d4/d64（20k×3 phase） | 60,004 | 60,000 | 4 | 0 | 1 |
| 冻结基线 W02-d4（20k×3 phase） | 60,004 | 60,000 | 4 | 0 | 1 |
| 诊断构建 unmarked（W03/W02，20k×3） | 60,004 | 60,000 | 4 | 0 | 1 |
| 诊断构建 verbose（512×3 phase） | 1,540 | 1,536 | 4 | 0 | 1 |

**口径**：20k×3 phase 的行只用 `strace -c` 直接测得总数 **60,004**；纯 submit/probe 的 60,000+4 分解由 512 请求逐行 trace（1,536 次 `(to_submit=1, min_complete=0, flags=0)` + 4 次 `GETEVENTS`）与工作量推断得到，**不是 20k 逐调用测量**。

**拆批点（W-03）**：

- 位置：`bench/bench_w03_external_loop.cpp:339`（补发循环），实际提交 `:349`。
- 当时可见状态：`w=0, s=depth, r=depth`（d16 稳态 group 2：ready=16、window=0、submitted=16）。
- 成员：slot 0..15，offset 65536..126976，len 4096，16 个不同 buffer；该组 `group_ready` → 首个 `submit_begin` 为 **934 ns**，最后一个 `member_ready` → 首个 `submit_begin` 为 **63 ns**（5 个 verbose 运行合计 3,572 组，last-member→first-submit 间隔 **32 ns–9,771 ns**，0 次反序）。
- 现有容量：`queue_depth=depth`（`bench_w03_external_loop.cpp:115-116`；`io_uring_queue_init` 于 `src/async/uring_backend.cpp:171`），`r ≤ depth` → 一次 submit 可容纳整组。
- 基线：16 次纯 submit enter/组；重切后 1 次；**差额 15 enter/组**（d4 差 3，d64 差 63）。

### 10.5 净 enter 节省（按现有容量，不新增容量）

| case / phase | 每 20k 基线纯 submit | 重切后 enter | 净节省 | 比例 |
|---|---|---|---|---|
| W03-d16 ph1 | 20,000 | 1,698 | **18,302** | **91.51%** |
| W03-d16 ph2 | 20,000 | 1,511 | **18,489** | **92.45%** |
| W03-d16 ph3 | 20,000 | 1,331 | **18,669** | **93.35%** |
| W03-d4 ph1 | 20,000 | 5,545 | **14,455** | **72.28%** |
| W03-d4 ph2 | 20,000 | 5,363 | **14,637** | **73.19%** |
| W03-d4 ph3 | 20,000 | 5,467 | **14,533** | **72.67%** |
| W02-d2 ph1/2/3 | 20,000 | 19,999 | **1** | **0.005%** |
| W03-d64（512 窗口） | 512 | 8/10/9 | 504/502/503 | 98.4/98.0/98.2% |

### 10.6 分类裁决

| consumer / shape | 是否有真实多 ready 组 | 分类 | 能否支撑 #595 产品声明 |
|---|---|---|---|
| **W-02 `W02-uring-4k-d2`（R1/R2 主 shape）** | 仅初始填充 2 条；稳态 19,998/20,000 为单成员 | **STARTUP_ONLY** | **不能**；即使机制层也只省 1 enter/20k |
| W-03 external loop d4/d16/d64 | 是；稳态多成员组占 95.4/96.9/98.8%（d4，其中 3–8 占 88.0/91.6/97.1%）与 95.1/91.5/98.8%（d16，其中 >8 占 72.7/82.3/97.7%） | **REAL_EXPOSURE**（合成 benchmark；非 #604 真实应用） | 机制层可以；性能层本轮未测，不能 |
| `sluice-copy` 默认 | 否（depth=1） | **NO_REAL_BATCH_EXPOSURE** | 不能 |
| `sluice-copy` depth>1 | 仅预填 | **STARTUP_ONLY** | 不能 |

**结论**：对 #595 的 headline shape，**没有真实批暴露（NO_REAL_BATCH_EXPOSURE）**；真实暴露只存在于 W-03 external loop，且需要 consumer 侧多 SQE 准备/批量提交。R2 的 owner-gated deferred-flush 原型**不会捕获**该组（W-03 的 submitter 不是 progress driver，见 §7 第 4 条）。

### 10.7 交付轮独立复核

| 复核 | 结果 |
|---|---|
| `strace` 计数 | `canonical/w03-d16-full-strace.txt`、`w03-d4-full-strace.txt`、`w03-d16-unmarked-strace.txt` 均为 **60,004**；与 §3 一致 |
| JSON 分布 | `ready_distribution_steady` 与 §10.3 逐格一致；`groups = 稳态 + 1 启动 + zero` 三处闭合 |
| 组合法性 | 9 个 verbose phase 的 6 类违例全为 0 |
| 二进制对照 | 诊断 W03 `c451dba4…`、冻结 W03 `929bc12f…`；诊断 W02 `4a27ac55…`、冻结 W02 `78d0db9d…`；诊断构建产品库 `76e59bd6…` 与基线相同 |
| 产品库未变 | `libsluice_async.a` 在普查后与 `BASELINE.md` §5 相同；`include/sluice/async/*.hpp` 未改 |

### 10.8 不得外推

1. enter 数字**不是**时延或加速声明；R3 没有计时（`PROTOCOL.md` §6）。
2. 只覆盖 4 KiB、warm tmpfs、本机、uring backend；不覆盖块设备、cold cache、O_DIRECT、其他 block size、threadpool backend。
3. r 分布受工作负载/调度/phase 影响（单成员比例 1.2–8.5%）；不得把某一 phase 的比例当作常量。
4. W-03 的组位于既有完成等待之后；不得据此新增等待、延长 defer 或追加预取。
5. W-02 的 D2/D3 是 census-only case；`sluice-copy` 只做源码级判定，未插桩运行。
6. 不得把本节结论写成“产品可部署”或“R2 结论被推翻”。
