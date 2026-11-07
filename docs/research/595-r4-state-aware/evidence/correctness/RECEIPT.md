# #595 R4 正确性证据回执（R/S 新逻辑 + A0/A1/B 组合）

对象：`/home/jnhu/sluice-595-e5/r4/worktree` @ `f3b25329bc8a64a201961c426a58adacfb8fdf08`
（分支 `research-595-r4`；轮次结束树已还原到该提交，porcelain=0，见 `tree_proof/tree_restored.txt`）。
新测试源码已归档：`uring_ready_reuse_correctness_test.cpp`（sha256 `5c10be9e…`），
xmake 目标注册 diff：`xmake_tests_lua.target-registration.diff`。两者本轮结束后已从工作树移除，未提交。

## 1. 结论

- 新增确定性 oracle 套件 `uring_ready_reuse_correctness_test`（18 场景函数、159 个断言位点，
  按臂×跨度参数化后实际执行约 3×10⁴ 次断言判定）：BASE GREEN、变异 RED、恢复 GREEN 全流程完成。
- 既有套件回归锚定：`uring_ready_batch_deterministic_test`、`uring_ready_batch_fault_test`、
  `uring_ready_batch_research_test`、`r4_arm_smoke`（57 PASS / 0 FAIL）全部通过（base 与 restored 两次）。
- 三个可逆变异（重复接受 / 零进展继续 / 忽略 authority 边界）全部被命名断言或超时杀死；
  变异钩子已 `git checkout` 还原，冻结头文件 sha256 与 f3b2532 的 git blob 逐位一致
  （`363f346ac3b26076322cbe0126a9d276aedbcb1d2ecd7607c108f8c32621dd81`）。
- 未发现任何臂存在真实语义错误；评审遗留的 4 个 minor 问题（防御守卫 / q8 计数失真 /
  diag 通道无机制防误设 / authority 决策不进 trace sink）按任务要求登记未修。

## 2. digestBinding（结论绑定）

| 项 | sha256 |
|---|---|
| 冻结适配器源 `bench/r3_ready_batch.hpp`（=git blob @f3b2532） | `363f346ac3b26076322cbe0126a9d276aedbcb1d2ecd7607c108f8c32621dd81` |
| 新测试源（BASE GREEN 与恢复 GREEN 同一源） | `5c10be9ee44f8471b34426ae663a78e7dc58633d0852808181f738b9136361d3` |
| BASE GREEN 二进制 | `b4b92dbc5515050061cda42ad066736ab10957c770a6b83724b474790446e56c` |
| 恢复 GREEN 二进制（与 BASE 逐位相同，可复现构建） | `b4b92dbc5515050061cda42ad066736ab10957c770a6b83724b474790446e56c` |
| M1 变异二进制 duplicate_accept | `5c14f2fc3de3e0817757590748a9d32502eb15b5eabb7c3f0515bf13e6141fe3` |
| M2 变异二进制 zero_progress_continue | `da489fc77365a5a011cfe4efbfef2fd696f0f112c699f4c63986e6ae2e32e2f7` |
| M3 变异二进制 boundary_ignored | `3860f5bbca8a4f4f3b69ce0d2843f9dde36ad694713679b3116aa91cfd5e611d` |

（`base_green/digests.sha256` 为构建时原样记录。）

## 3. 用例矩阵 → 场景映射（逐用例结果均 rc=0）

| 任务用例 | 场景（--only=） | 覆盖臂 |
|---|---|---|
| 空 span | `empty_span` | 全部 10 臂 |
| 单条 | `single_read` | 全部 10 臂 |
| 2/8/9/16/64 条 | `span_shapes` | 全部 10 臂 × 5 跨度（50 组合） |
| 多次 cap 重入 | `span_shapes`（R/SHADOW 64 条 = 8 呈现 7 重入；S 2→8→6 生长） | A0,A1,B,R,S,SNI,SNP,Q2,Q8,SHADOW |
| cap 返回且存在 transport residue | `residue_after_cap` | B,R,SHADOW,SNI,Q8,S,SNP,Q2 |
| SQ 无空间且 accepted=0 | `prefilled_residue` | S,B,Q2,R |
| partial submission 与 retry | `partial_retry` | R,B,S（scripted EAGAIN/partial_first，重试恰为未消费前缀） |
| prefix 已接受 suffix 未接受 | `capacity`（would_block 后 refill 补全）+ `middle_rejection`（suffix 未触内核） | A0,B,R,S,Q2 / A0,A1,B,R,S |
| 中间成员 rejected | `middle_rejection`（op[5] access 拒绝，suffix 缓冲零污染） | A0,A1,B,R,S |
| ineligible 混合 | `ineligible_mix`（中位 512B + 首位 512B 两型） | B,R,S |
| 重入间 cancel/control | `control_between`（sticky control 意图，取消者恰一个 terminal） | R,S |
| 重入间 shutdown/poison | `shutdown_between`（invalid_state 封闭）/ `poison_between`（可见者真结果、不可见者物理失败） | R,S |
| 并发提交与 generation/slot 重用 | `concurrent`（2 线程×16 op×2 轮，id 全局唯一、二轮 key 不复现、SQE 恰 64） | R,S |
| CQ overflow 与 publication 残留 | `overflow`（64 op 无 reap，overflow flush 后逐一发布） | R,S |
| 数据正确 | `span_shapes`/`single_read`/`capacity`/`overflow`/`publish_once`/`concurrent`/`empty_pipeline`（real-kernel 场景逐字节比对 pattern；scripted-kernel 场景无数据面，按框架惯例只验结果值与身份） | 见各行 |
| 请求身份正确 / 无重复 terminal | `span_shapes`（id 唯一+SQE 恰 N）、`control_between`/`poison_between`（terminal 恰一次，二次 take 非 ready） | 见各行 |
| borrow 在执行退休前不释放 | `borrow`（16 pipe 读逐 payload 释放，每释放恰一个成员发布、其余缓冲 sentinel 不动；完成顺序非不变量，计数断言） | R,S |
| 无通知丢失 | `overflow`（overflow flush）、`publish_once`（pass 完成数单调累计恰 16，不超过剩余） | R,S |
| 无零进展循环 | `prefilled_residue`/`control_between`（零接受 stop 呈现恰 1 次、无 re-entry 计数、有界返回）+ M2 变异证明守卫承载性 | S,B,Q2,R |
| S 状态表（outstanding/pressure/authority/消融） | `state_aware`（注入式 CQE 构造 completion pressure + 1 in-flight）、`empty_pipeline`、`residue_after_cap` | S,SNI,SNP,Q8,Q2,R,B |

liveness 证据形式：所有驱动循环带 guard 上限 + 确定性终态断言（published 单调、计数恰等、
ledger/dispatch 清零），非仅"未超时"；M2 的超时杀死是变异证据而非基线 liveness 证明。

## 4. 变异记录（流程 BASE GREEN → mutant RED → restored GREEN）

| 变异 | 注入点（bench/r3_ready_batch.hpp，已还原） | 杀死它的测试与输出摘录 |
|---|---|---|
| M1 duplicate-accept | `begin += batch.untouched_begin` → 少推进 1（重呈现最后被接受成员） | `--only=span_shapes`：321 failures，rc=1；"exactly one request per member is accepted"、"exactly one kernel SQE per member"（mutants/M1_duplicate_accept_span_shapes.out） |
| M2 zero-progress-continue | cap_reuse/chunk_continue 丢掉 `progressed` 与 stop 原因 | `--only=prefilled_residue`：s/q2 臂确定性 FAIL"the arm returns without re-deciding past the stuck gate"+"no re-entry happens without progress"，随后 r 臂零进展死循环被 `timeout -k 5 60` 杀死 rc=124；`--only=residue_after_cap` 纯 rc=124（mutants/M2_*.out） |
| M3 boundary-ignored | authority 输入 `prior_stop_control_or_recovery/residue_advertised` 置 false | `--only=residue_after_cap`：10 failures rc=1；全部 5 个 budget 臂"presentions at the residue boundary are exact"+"exits to authority at the residue boundary"（mutants/M3_boundary_ignored_residue_after_cap.out） |

## 5. 逐文件清单

- `base_green/`：新套件全量 rc=0（start/end UTC、head=f3b2532、digests）+ 既有四套件 rc=0。
- `mutants/`：M1/M2×2/M3 命令、UTC、输出、rc。
- `restored/`：header 还原后全量 rc=0 + 四套件 rc=0 + header/二进制 digest。
- `tree_proof/`：`git status --porcelain` 空行数 0、`rev-parse HEAD` = f3b2532…、header digest。
- `uring_ready_reuse_correctness_test.cpp`、`xmake_tests_lua.target-registration.diff`、`test_source.sha256`：复现材料。
- `LOCK_RECEIPT.md`：本轮锁协议收据。

## 6. 诚实声明

- scripted-kernel 场景（residue/poison/control/partial/state_aware）不做缓冲字节比对：脚本内核不执行
  真实读，`inject_cqe_for_test` 只发布结果值——与 R3 既有框架一致；数据面正确性由 real-kernel 场景覆盖。
- `borrow` 场景初版假设 pipe 读 FIFO 完成顺序，实测被证伪（io_uring 等权 pipe 读完成顺序非不变量），
  已改为顺序无关断言（每释放恰一个发布、计数单调、pending 缓冲不动）——这是测试假设错误，非产品缺陷。
- 本轮未跑 `xmake test` 全矩阵（82 用例）：未改任何产品源码；四套件 + 新套件的定向回归已锚定。
  全矩阵在 30-build-smoke-lock-session.log 已有基线记录。
- 评审遗留 4 个 minor（见任务描述）未修，未计入任何臂的判定。
