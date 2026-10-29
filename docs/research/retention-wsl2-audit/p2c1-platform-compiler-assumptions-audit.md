# P2C1 平台与编译器假设审查（WSL2 retention audit）

- 审查对象：master `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`（P2C1，#608/#612，已合并）
- 审查日期：2026-10-29（Asia/Shanghai）
- 工具链：gcc 15.2.0 (Ubuntu 15.2.0-16ubuntu1)、clang 21.1.8、glibc 2.43、kernel
  6.18.54.1-microsoft-standard-WSL2（与任务书给定身份一致）
- 性质：只读审计 + 非破坏性验证。产品代码零修改；构建仅在
  `/tmp/sluice-val-p2c1` 验证 worktree；产出仅本目录与 receipts/。
- 范围：私有 Prevalidated Read Step、Register-Resident Confirmed Bytes、
  Partial/EOF/Short Read、EINTR、Composition Stop Reason、Error-Path
  Materialization、File/Buffer Lifetime、THREAD-01。

## 方法

静态：`git show 84e1b7d5` 全量 diff；通读 `src/blocking_file.cpp`、
`include/sluice/detail/file_semantics.hpp`、`include/sluice/blocking/file.hpp`、
`include/sluice/result.hpp`、`include/sluice/error.hpp`、
`include/sluice/detail/posix_retry.hpp`、`src/file_test_seams.hpp`、
`src/file_resource.cpp`、`xmake/tests.lua`、`xmake/libraries.lua`、
`tests/direct_composition_fault_test.cpp`；逐条把新 `compose` 循环的四个出口
与 `detail::compose_progress`/`compose_error`/`composition_effect_certainty`
oracle 及 `CompositionOutcome` 字段序对齐；核对 `precheck_data_op`/
`range_is_valid` 对 `offset+confirmed`、`static_cast<off_t>`、`SSIZE_MAX`
上界的支撑。

动态（均在 /tmp worktree，命令与输出见 receipts 10–18）：

1. WSL2 GCC 15.2.0 release：`direct_composition_fault_test`（15/15）、
   `semantic_short_io_reference_test`（9/9）通过（receipts/10）。
2. 七个 P2C1 mutant 目标全部被其命名断言杀死（exit=1，receipts/11）。
3. ASan+UBSan（`-fno-sanitize-recover=all`）：5 个 blocking/composition 套件
   全绿（receipts/12）。
4. `-Wconversion -Wsign-conversion -Wextra -Wshadow -Wuseless-cast …`：
   blocking_file.cpp 生产形（无 seam define）与 seam 形均 0 警告；clang 同
   （receipts/13）。
5. GCC 15.2.0 -O3 反汇编 `read_exact_at`/`read_exact`（receipts/14）。
6. 边界探针（receipts/15）：off_t 上界准入/拒绝、SSIZE_MAX、raw-close
   EBADF 面、seam 全宽 offset（9223372036854775803 原样到达 native 调用）。
7. clang 21.1.8：两套件通过 + 探针 ASan/UBSan 通过（receipts/15）。
8. 顶边 EINVAL 平台事实刻画（receipts/17，tmpfs+ext4）。
9. -O0（debug）两套件通过（receipts/18）——语义不依赖优化。

## 发现

### F1（low，已验证）：THREAD-01 边界披露准确但少说了一种风险形态

- where：`src/blocking_file.cpp:347-348`、`src/blocking_file.cpp:377-378`；
  披露文本 `docs/roadmap/v2-conformance.md` SEM-05 行（本 commit 修改）。
- 已验证的部分：账本句子"并发 close 中途，读组合不再逐步重查 closed，将以
  EBADF 类 backend_error 结束，而 C1 基线是 invalid_state"两半都成立：
  - 新树：raw-close 探针实测 `end=primitive_error(3), code=backend_error,
    os_errno=9(EBADF)`（receipts/15）；
  - 基线：父提交每步调用 `read_at`（diff 可证），`precheck_data_op` 的
    `closed` 先于一切 → `reject_closed` → `IoError::invalid_state`
    （file_semantics.hpp:162-163）。
- 少说的部分：披露只给出"无复用时的 EBADF"形态。fd 在组合中途被第三方
  `open()` 复用时，新代码会从**错误的文件**读到数据并**静默成功**；该暴露
  窗口从基线的"每步 admission→pread 窄窗"扩大为"整个组合"。两类都属
  THREAD-01 已明文的 caller-serialization 违例（根 §13 表行 "File
  move/close/destruction | Caller serialization"），不是新 UB、不是合同内
  行为差异；File 被 move 走（fd_ 变 -1 后复用编号）同理。建议账本补一句
  fd-reuse 形态，避免读者把"EBADF-class"当成并发 close 的唯一可观察结果。
- severity 依据：合同外（调用方数据竞争），非数据丢失级缺陷，故 low。

### F2（low，已验证，平台事实）： admitted 顶边请求在本机内核返回 EINVAL 而非 EOF

- where：`include/sluice/detail/file_semantics.hpp:32-39`（range_is_valid 允许
  `offset+length-1 == INT64_MAX`）× kernel 6.18.54.1-microsoft-standard-WSL2。
- 证据（receipts/17）：tmpfs 与 ext4 上 `pread(fd, buf, 8,
  0x7FFFFFFFFFFFFFF8)` → -1/EINVAL；`pos+count > INT64_MAX`（以及
  `(loff_t)pos < 0`）时内核拒绝；`read_exact_at` 将其正确物化为
  `primitive_error / backend_error / os_errno=EINVAL / remaining=unknown`，
  confirmed=0，无崩溃无错误数据。
- 判定：不是 P2C1 回归——C1 基线以相同参数发同一 pread，得到同一 EINVAL，
  同样以 primitive_error 结束；errno 全程保真。仅记录为平台包络事实：
  SEM-04 允许的顶边请求在本机内核不可执行。该内核检查的 mainline/WSL2
  出处 UNKNOWN（未读内核源码）。

### F3（low，已验证，测试代码潜在可移植性）：seam 路径 off_t→long 截断隐患

- where：`src/blocking_file.cpp:56`、`src/blocking_file.cpp:73`
  （`static_cast<long>(offset)`，仅 `SLUICE_FILE_INTERNAL_TESTING` 下）。
- file_semantics.hpp:18-22 只 static_assert `off_t` 有符号且 ≥63 位，不约束
  `long`；在 ILP32+LFS（off_t 64 位、long 32 位）平台上 seam 记录的 offset
  会截断。本平台 LP64 下 long==off_t，且探针证实全宽 offset 原样透传
  （receipts/15）。仅测试形代码，产品路径直接传 `off_t`，无影响。

## 已检查并放心的假设（均本轮实测或逐行核对）

1. off_t/ssize_t 平台断言在本工具链成立（编译通过即证；file_semantics.hpp:18-24）。
2. `offset + confirmed` 无 uint64 溢出、`static_cast<off_t>` 不丢位：仅当入口
   `precheck_data_op→range_is_valid` 给 execute 才进 compose，步进偏移
   ≤ offset+length-1 ≤ INT64_MAX；越界偏移/越界 range 拒绝实测；seam 实测
   0x7FFFFFFFFFFFFFF8(+3) 全宽到达 native 调用。blocking_file.cpp:272-274
   注释成立。
3. `length > SSIZE_MAX`（kMaxNativeTransfer）在任何 syscall 前被拒
   （file_semantics.hpp:173-174；探针实测）。
4. compose 整数不变量（confirmed≤total；`total - confirmed` 不下溢；impossible
   -count guard 用无符号比较且在加法前）——源码核对 + ASM `jb` + UBSan 干净 +
   `SKIP_IMPOSSIBLE_COUNT`/`STEP_REPORTS_FULL_COUNT` mutant 被杀。
5. 四个出口（complete/eof/write_no_progress/primitive_error(含
   impossible_count→invalid_state 映射)）与 `compose_progress`/`compose_error`
   oracle 及 certainty 规则逐字段等价；`every_stop_reason_is_pinned...` 在
   GCC/clang、-O0/-O2/-O3、ASan+UBSan 全绿。
6. `primitive_error_outcome` 聚合初始化字段序与 `CompositionOutcome` 声明序
   一致（blocking/file.hpp:42-49）；死零 store 的移除只影响代码生成，不改
   errno 分类链（"dead zero store" 重构验证）。
7. EINTR：`step_retry_on_eintr→detail::retry_on_eintr`（posix_retry.hpp:7-15）
   循环内无 errno 破坏点，最终失败的 errno 保真到 `from_errno_value`；
   EINTR 重试同 buffer/offset；`STEP_SKIPS_EINTR_RETRY` mutant 被命名断言
   杀死；非 mutant 生产目标不含该 define（libraries.lua 无 mutant define，
   见编译命令）。
8. 无未初始化读/严格别名/越界：Result union 由 has_value_ 纪律保护；
   `dst.subspan(confirmed)` 由循环条件保证 confirmed<total=span size；
   ASan+UBSan 5 套件干净。
9. 语义不依赖编译器优化：-O0 套件通过；"register-resident"只是性能断言，
   GCC 15.2.0 -O3 下定性成立（confirmed 驻留 %r14，EINTR 重试复用寄存器/
   栈槽，每出口一次物化，receipts/14）；树内无指令数断言工具，commit 信息
   里的静态计数（100→68 等）是 E5 上的手工证据，不构成回归门。
10. mutant define 目标作用域正确（xmake/tests.lua `direct_mutant_target`
    仅编译自带拷贝的 TU，与生产 sluice_core 不同链）。
11. 零长/nullptr 顺序与基线一致（complete_empty 先于 nullptr 检查，
    零长不发 native call 实测）。
12. `write_all_at`/`write_all` 仍走公开 write_at/write 步（P2C1 未动写侧
    步进），提交声称的单因子纪律属实。
13. LP64 假设（long/ssize_t/off_t 同宽）在本平台成立（探针）。

## UNKNOWN / 未覆盖

- 顶边 EINVAL 内核检查的 mainline 出处（未读 6.18 内核源码）。
- E5（GCC 16.2.1）机器码等价性：按任务书不假设；本机无法验证。
- io_uring/request 路径的组合实现未审（P2C1 只动 direct blocking 读组合）。
- 多线程 mid-composition close 的真实竞争只能以 raw-close 探针确定性地复现
  新树可观察面；基线对照为静态 diff 证据（两树无法在无钩子下确定性插入
  步间 close）。

## 边界遵守

未修改产品代码；未在主工作区构建；未 merge/close；未删除任何分支、
worktree、证据；`/tmp/sluice-val-p2c1` 遵留脚本统一清理（未自删）。
receipts：10、11、12、13、14、15、17、18 号为本轮产出。
