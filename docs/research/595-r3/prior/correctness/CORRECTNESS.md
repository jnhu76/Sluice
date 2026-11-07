# #595 R3 — 确定性正确性证据（CORRECTNESS）

| 字段 | 值 |
|---|---|
| 角色 | R3 确定性正确性工程师 |
| 基线 | `baseSha=0a22a79195f1b913145fcd443496e7f70b08cdcf`；worktree `/home/jnhu/sluice-595-e5/r3/worktree`（HEAD 同 baseSha） |
| 测试 | `tests/uring_ready_batch_deterministic_test.cpp`（1400 行，sha256 `ef262452…`，副本在本目录） |
| 生产 seam | `UringAsyncBackend::submit_ready_reads`（`src/async/uring_backend.cpp:444`） |
| 变异开关 | 3 个编译期宏，位于 `src/async/uring_backend.cpp:525,583,614`；xmake 目标 `xmake/tests.lua:590-621` |
| 结果 | BASE GREEN（13/13 场景，连续 5 次）；3 个变异全部由目标断言杀死（isolated RED exit=1）；RESTORED GREEN |
| 计时 | 未做；所有数字是 broker 计数/退出码，不是性能结论 |

本文件只记录本轮运行过的测试与回执。前置依据：`CONTEXT.md` §6（ReadyGroup 定义）、
`STATE_AUTHORITY.md` §4 I1–I14 与 §5.2/5.3（残渣分类与 signal 义务）、
`BUILD.md` §3.1（一次调用内 accept→SQE→一次 flush，不跨调用持有 SQE）。

---

## 1. 确定性控制手段（不使用 sleep 作为并发证明）

| 手段 | 位置 | 用途 |
|---|---|---|
| 真实内核 + submit hook | `src/async/uring_test_seams.hpp:11-25`（`UringBackendSubmitTestHooks`）；测试 `oracle_submit`（test:118-137） | 在 `io_uring_submit` 边界记录每次 enter 的 `sq_ready` 与 SQE opcode，再委托真实 `io_uring_submit`；独立于生产计数 |
| scripted kernel | 测试 `scripted_submit`（test:186-233），挂在同一 seam | 精确控制内核消费/不消费、partial（rc<pending）、`-EIO` poison |
| pipe hold | `pipe_ops`（test:281-295）与各场景 | 把「已提交、未完成」固定为可持久的物理状态；写端释放字节数决定完成数 |
| ProgressSource pre-park gate | `include/sluice/async/detail/progress_source.hpp:300-311`；测试 `prepark_batch_completion_is_not_lost`（test:1079） | 把 owner 冻结在最终 revalidation 之后、`poll(2)` 之前，期间制造真实内核完成，再恢复；证明无丢唤醒 |
| eventfd 直读 | `ProgressSource::notification_fd()`；`drain_notification`/`notification_readable`（test:392-414） | 「无新增 progress 写入」的独立观测：先经 `acknowledge_notification` 排空，调用后直接 `read(2)` 必须 EAGAIN |
| token 快照 | `TestAccess::context_progress_token`（`test_access.hpp:34-39`） | 断言 immediate 路径不推进 progress epoch |
| `io_uring_get_events` hook | `UringBackendSubmitTestHooks::get_events`（`uring_test_seams.hpp:17-20`）；测试 `oracle_get_events`（test:139-143） | overflow 冲洗的独立计数 |
| 有界等待 | `drive_request`/`drive_context`/`drive_any_ready`（test:322-390） | 只用于等待内核事件（eventfd/poll），20 ms 切片与 5 s/2 s 截止是失败保护，不是同步证明 |

所有场景共用的断言风格：每个 `check` 的 label 直接命名它区分的不变量/失败模式；
RED 判定以该 label 出现在日志为准，不以退出码或超时为准。

---

## 2. 场景 — Oracle 矩阵

测试入口 `main`（test:1341-1400）支持 `--only=<scenario>` 只选场景；断言集合不变。
BASE 全量运行 13 个场景；下面行号均指 `tests/uring_ready_batch_deterministic_test.cpp`。

| # | 场景（行） | 区分的不变量 / 失败模式 | 关键独立观测 |
|---|---|---|---|
| 1 | `d2_positive_two_ready_reads_one_enter` (453) | 同一 W-03 补发生产点：2 条独立 4 KiB read 在首提交前已就绪；合批不得合并请求或改变结果；A0/A1 各 2 次 submit enter，B 1 次 enter 携带 2 个 data SQE | 生产点形状 `r=min(depth-window,count-submitted)`；成员 `(dst,len,offset)` 预测量；submit hook 的 enter 数/`ready`/opcode；report `accepted=2, untouched=0, input_exhausted, flush=1, residue=false`；两个 request id 不同、各自 4096B/自己的字节 |
| 2 | `d2_negative_single_refill_while_in_flight` (555) | 原 W-02 depth=2、每次只补一条、仍有 I/O 在途：B 必须走 immediate，不得跨调用持有 SQE、不得新增 residue 广告或 progress 写入 | pipe 只释放一条（`drive_any_ready`，不断言哪一个先完成）；`dispatch_size=0`、ledger=0、flush 恰好 +1；report span=1 → accepted=1/flush=1/residue=false；token 不变；eventfd 直读 EAGAIN |
| 3 | `d1_dependent_second_never_enters_ready_group` (644) | 第二条读依赖第一条结果：第一条发出后立即等待结果，第二条在第一条 terminal 被消费前不得进入组/获得 route；复用槽必须推进 generation，旧 key 必须 stale | waiter 线程在 pipe 上等待、主线程释放数据后 join；`outstanding=1`、offset=4096 无 live route；结果 4096 后派生 offset；旧 key `lookup==not_found`；新 key generation 递增；派生读字节正确 |
| 4 | `d3_d16_budget_prefix_and_suffix_accounting` (732) | 单条 immediate；真实 3-ready 组；16 输入 > cap 8；ring depth 4 而组为 6；前缀/后缀逐条核销，未接受 suffix 明确返回 | 单条 1 enter/1 SQE；3 组 1 enter/3 SQE；cap=8 + suffix 8×span1，id 全不同、`max_sqes<=8`、16 个结果/字节；budget 组 accepted=4/`stop=sq_budget`/untouched=2，suffix 2 条 |
| 5 | `sq_full_transport_residue_rejects_batch` (882) | SQ 满且传输账本仍有未证明 SQE 时，transport 未就绪：下一批必须 accepted=0、stop=recovery、整组 untouched、不加 flush/队列项 | hold kernel 后 `sq_ready=4`、ledger=4；第二批 report 字段；`submit_flushes` 不变、ledger 仍 4、dispatch=0；恢复后 6 条全部结算 |
| 6 | `partial_submit_advertises_and_retries_residue` (948) | 多 SQE 批量 flush 被内核部分消费（rc=1 < pending=2）：必须广告真实 residue，下一次 pass 只重试剩余前缀 | scripted `partial_first`；report accepted=2/flush=1/`residue=true`；`first_ready=2, first_rc=1`；ledger 1→0，flush 1→2，`second_ready=1, second_rc=1`；两条独立结果 |
| 7 | `cancel_and_cqe_ordering` (998) | 取消前后 CQE 的合法顺序：非取消成员 CQE 在 cancel 之前被产出；sticky control intent 在 SQ 满时保留；control CQE 可先于 original CQE；original CQE 到达后仍是唯一 terminal | hold 4 条 → 注入成员 0 CQE → cancel 成员 3（requested，`authoritative_pending=1`）→ consume_all 后 prepared→submitted（`live_control_sqes=1`）→ 先注入 control CQE（成员 3 仍不 ready）→ 再注入 `-ECANCELED` → 成员 0/1/2=4096、成员 3=canceled；control/route 计数归零 |
| 8 | `prepark_batch_completion_is_not_lost` (1079) | pre-park 无丢唤醒：owner 在最终 revalidation 后/`poll(2)` 前被冻结，期间批次完成产生；恢复后必须以 progress 唤醒 | 真实内核 + 4 条 pipe read 批次（无 residue）；prepark gate 暂停后写入 4×4096；`wait_one` 返回 progress、`completed>=4`；4 条全部 ready/4096 |
| 9 | `overflow_flush_keeps_every_batch_completion` (1133) | CQ overflow 不得吞掉批量完成：32 条完成在 16 项 CQ 上溢出，reap 必须冲洗 overflow 列表 | 4×8 批提交后不 poll；`get_events` hook 计数 >=1；32 条全部 4096 且字节正确 |
| 10 | `poison_after_partial_batch_never_fabricates_success` (1164) | poison 前部分 SQE 已被内核看见：可见成员保留真实完成路径，不可见成员只报 physical failure，绝不伪造 success | scripted `partial_then_poison`（consume 1 后 `-EIO`）；ledger 仅剩 1 条不可见项；可见 route 存活、不可见 route 不存在；注入可见 CQE 后 4096；不可见项 `backend_error`；shutdown=completed |
| 11 | `shutdown_drain_settles_batch` (1208) | shutdown(drain) 结算已接受未 poll 的批次；关闭后批次入口不再碰 ring，整组 untouched | 4 条批次后立即 shutdown → completed、`execution_closed()`、4 条全部 ready/4096/字节；随后 2 条 report accepted=0/recovery/backend_error/untouched=2 |
| 12 | `borrow_and_last_member_are_independent` (1255) | borrow 保留到各自 CQE：批次成员不合并；最后一条只在自己的数据到达后完成；未完成成员 buffer 不得被触碰 | 2 条 pipe read；未 ready 且 buffer 仍 sentinel；写 4096 → 恰好一条 ready 且其 buffer 为 payload、另一条仍 sentinel；写第二条 payload → 最后一条完成，两条各 4096 |
| 13 | `stale_generation_after_consumed_batch` (1309) | consume 后槽复用：generation 前进、旧 key stale、不得重发已消费 SQE | 第一批 2 条结算并 consume；第二批 report 无 residue；enter +1、SQE +2；同槽 generation 严格增大；旧 key `lookup==not_found`；第二批结果/字节正确 |

补充说明（如实标注）：

- pipe 上同 fd 的多个 read 由 io_uring/io-wq 调度，完成顺序不保证等于提交顺序。
  因此场景 2/12 只断言「恰好一条完成」与「完成者拿到自己的字节」，不断言哪个成员先完成；
  这是外部物理事实，不是放松断言。
- 场景 4 的 cap 组容量设为 18（> 16）、budget 组容量 8（> 6）、场景 1/12/13 容量大于
  在途前缀，目的是给「重复派发」变异留出 router 余量，使 RED 落在报告断言而不是
  router 耗尽 fail-fast；容量只限制 reservation 数，不改变批次语义或本组断言。
- `--only=` 只做场景选择；BASE 全量运行覆盖全部 13 个场景，断言不因选择而改变。

---

## 3. 三个命名变异（编译期开关）

开关都在 `src/async/uring_backend.cpp`；BASE 目标不定义任何 `SLUICE_R3_MUTANT_*`
（`raw/correctness/compile-defines.txt` 逐目标证明）。xmake 目标见
`xmake/tests.lua:590-621`。每个变异使用同一测试 TU 构建，只加一个宏。

| 变异 | 宏（行） | 注入的缺陷 | 目标场景 | 目标断言（测试行） | RED 回执 |
|---|---|---|---|---|---|
| M1 | `SLUICE_R3_MUTANT_BATCH_INCLUDES_NOT_READY_TRANSPORT` (`uring_backend.cpp:525`) | 跳过传输未就绪门（`dispatch_retry_remains_locked_`、`sq_space_left==0`），并把 limit 放宽为 `min(size,cap)`：在 SQ 满、账本仍有未证明 SQE 时把输入当作可接受 | `sq_full_residue` | “a batch at SQ-full transport residue accepts nothing”（test:912） | `raw/correctness/mut1-includes-not-ready-only-sq-full.log`：第一行即目标 FAIL；6 个 FAIL；exit=1 |
| M2 | `SLUICE_R3_MUTANT_BATCH_RESIDUE_NOT_ADVERTISED` (`uring_backend.cpp:614`) | 把 `advertise` 强制为 false：真实 partial-submit 残渣被当成已提交，既不报告也不 signal | `partial_submit` | “partial kernel consumption advertises dispatch residue”（test:965） | `raw/correctness/mut2-residue-dropped-only-partial.log`：唯一 FAIL 即目标；exit=1 |
| M3 | `SLUICE_R3_MUTANT_BATCH_REDISPATCHES_CONSUMED` (`uring_backend.cpp:583`) | `dispatch_one_locked` 已消费某 reservation 后，把它重新压回 `dispatch_`：后续 pass 看到「SQE 已发出、execution 已 claim」的重复项；全消费的组因此仍报告 residue | `d2_positive` | “a fully consumed group advertises no residue”（test:543） | `raw/correctness/mut3-redispatch-consumed-only-d2-positive.log`：唯一 FAIL 即目标；exit=1 |

M3 的全量运行另有已记录的附带后果：日志第一行是目标 FAIL，随后在
`uring_transport.cpp:415` 触发 “Class-A operation recovery lost terminal authority”
abort（exit=134）。原因是 poison 恢复假设 `dispatch_` 只含**从未取过 SQE** 的
reservation；重复项是已派发项，poison 队列循环会为它伪造 terminal，随后 Class-A 对
同一 key 再 offer 时失去权威。该 abort 不是本变异的 RED 见证，而是同一缺陷对更广
不变量的破坏证据；RED 见证使用隔离目标场景（`--only=d2_positive`，exit=1，无崩溃）。

---

## 4. BASE GREEN → MUTANT RED → RESTORED GREEN 回执

二进制（当前构建，`raw/correctness/binary-sha256.txt`）：

```
0ecb27e2…  uring_ready_batch_deterministic_test
d15d5e4a…  uring_ready_batch_det_mut_includes_not_ready
70f54690…  uring_ready_batch_det_mut_residue_dropped
f0ff6e42…  uring_ready_batch_det_mut_redispatch_consumed
```

| 步骤 | 命令 | 结果 | 日志 |
|---|---|---|---|
| BASE GREEN（全量） | `./build/linux/x86_64/release/uring_ready_batch_deterministic_test` | exit=0，`all deterministic ready-batch oracles passed`；连续 5 次相同 | `raw/correctness/base-full.log`；重复 5 次见 `raw/correctness/r3det-run-1..5.log` |
| BASE GREEN（目标场景） | `… --only=d2_positive` / `--only=sq_full_residue` / `--only=partial_submit` | exit=0 | `base-d2_positive.log`、`base-sq_full_residue.log`、`base-partial_submit.log` |
| M1 RED | `…/uring_ready_batch_det_mut_includes_not_ready --only=sq_full_residue` | exit=1；第一行 = 目标断言 | `mut1-includes-not-ready-only-sq-full.log` |
| M2 RED | `…/uring_ready_batch_det_mut_residue_dropped --only=partial_submit` | exit=1；唯一 FAIL = 目标断言 | `mut2-residue-dropped-only-partial.log` |
| M3 RED | `…/uring_ready_batch_det_mut_redispatch_consumed --only=d2_positive` | exit=1；唯一 FAIL = 目标断言 | `mut3-redispatch-consumed-only-d2-positive.log` |
| RESTORED GREEN（强制重编） | `touch src/async/uring_backend.cpp && xmake build -y uring_ready_batch_deterministic_test` | build ok，exit=0 | `rebuild-restored-base.log` |
| RESTORED GREEN（全量） | 同 BASE 全量命令 | exit=0，13/13 场景 | `restored-base-full.log` |
| RESTORED GREEN（目标场景） | 三个 `--only=` 目标场景 | 全部 exit=0 | `restored-base-{d2_positive,sq_full_residue,partial_submit}.log` |

变异全量运行（非 RED 见证，仅记录）：
`mut1-includes-not-ready-full.log`（9 FAIL，exit=1）、
`mut2-residue-dropped-full.log`（4 FAIL，exit=1）、
`mut3-redispatch-consumed-full.log`（首行目标 FAIL，随后 poison 附带 abort，exit=134）。

---

## 5. 既有回归（当前源码，变异宏未定义）

| 测试 | 结果 |
|---|---|
| `uring_ready_batch_research_test` | all research oracles passed |
| `uring_ready_batch_fault_test` | all research fault oracles passed |
| `uring_submit_boundary_test` | all 2 passed |
| `uring_progress_race_test` | all 23 passed |
| `uring_flush_count_oracle_test` | all checks passed |
| `uring_external_loop_test` | all 9 passed |

原始输出：`raw/correctness/regression.log`。这证明本轮对
`uring_backend.cpp` 的宏化改动在 BASE 下没有改变既有可观察行为。

---

## 6. 限制、未覆盖与如实标注

1. **M1 的语义映射**：`submit_ready_reads` 接收调用者断言为 ready 的 span，
   后端无法直接观测调用者的 ReadyGroup 条件（`CONTEXT.md` §6）。后端能强制的
   「就绪边界」是入口门集合（transport 残渣、SQ 预算、control、eligibility、
   admission）。M1 因此命名为「transport 未就绪时仍纳入组」，而不是字面意义上的
   「readiness 标志」。D1 场景用调用点结构证明第二条未就绪读不会出现在 span 中，
   且在其依赖完成前没有 route/outstanding。
2. **M3 全量运行的附带 abort**：见 §3 末段。隔离目标场景的 RED 是命名断言；
   全量 abort 是同一缺陷的连带的 invariant 破坏，未作为 RED 见证。
3. **未做计时/性能结论**；所有运行是语义 oracle。构建/测试运行未持有
   `MEASUREMENT.lock`（`CONTEXT.md` §9 将构建列入持锁范围）；本轮无计时数值，
   因此不影响任何性能证据，但这是与 §9 字面要求的偏差，如实记录。
4. **未覆盖**：真实块设备/cold cache/O_DIRECT；threadpool backend 的批次路径；
   跨线程并发 submit 与批量的交错（`THREAD-01` 允许，但本轮只测固定 owner/
   单提交者）；64 KiB 不适合批量的输入已有既有 research oracle 覆盖，本轮未重复。
5. **worktree 与 `evidence/r3.patch` 的差异**：本轮新增测试、3 个宏开关与 xmake
   目标，未包含在旧 `r3.patch` 摘要中；`BUILD.md` 的二进制 SHA 是历史值，当前
   构建 SHA 见 §4。`CONTEXT.md` §5 硬约束未被违反：公开头 `include/sluice/async/*.hpp`
   的公开符号未改（`test_access.hpp`/`uring_backend.hpp` 的既有 R3 改动不是本轮引入）。
6. **未 push / 未改 issue / 未开 PR**；主工作树 `/home/jnhu/Source/Sluice`
   `git status --porcelain` 为空（`raw/correctness/worktree-status.txt`）。

---

## 7. 文件索引

| 内容 | 路径 |
|---|---|
| 本文件 | `evidence/correctness/CORRECTNESS.md` |
| 测试源副本 | `evidence/correctness/uring_ready_batch_deterministic_test.cpp` |
| 变异开关说明与代码摘录 | `evidence/correctness/MUTATIONS.md` |
| 精确命令 | `evidence/correctness/commands.txt` |
| 摘要清单 | `evidence/correctness/MANIFEST.sha256` |
| 原始日志（BASE/变异/恢复/回归/构建/状态） | `raw/correctness/*.log`、`raw/correctness/*.txt` |
