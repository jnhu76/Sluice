# 双重复审 · 正确性（correctness re-review）

- 日期：2026-10-30
- 复审对象：master `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`（P2C1 #612）及
  `43cb8029ca60b572c91a9aa9dccb17d4300e2f88`（C1 #602）
- 复审角色：独立正确性 Reviewer（非原 E5 评审者；不继承历史 APPROVE，仅凭本轮证据）
- 范围：v2 完整语义（SEM-01..05、ERR-01/02、THREAD-01）、错误优先级、API/ABI、
  并发与资源生命周期、Sanitizer 与突变证据、真实 WSL2 SDK
- 判定：**OK**
- ABI 判定：**ABI_CONSISTENT**
- 纪律：主工作区零构建、产品代码零改动；唯一写入为本报告；验证 worktree
  `/tmp/sluice-val-correctness`（HEAD）与 `/tmp/sluice-val-correctness-base`（pre-C1
  `ac995641e7394bfd82f7edb1caafca6ebea21c13` = 43cb8029^）留待脚本统一清理。

## 1. 独立执行证据（本复审在本 ask 内实际运行）

### 1.1 基线身份
- `git rev-parse HEAD` → `84e1b7d5…`（= P2C1 = origin/master，与基线身份输入一致）；
  `git log --oneline -5` 显示 43cb8029 (C1) 在 HEAD 祖先链上。
- 工作区仅 `?? docs/research/retention-wsl2-audit/`（审计报告目录），无其他改动。

### 1.2 Release 全量矩阵（自建 worktree，gcc 15.2.0，`--liburing=n`）
```
cd /tmp/sluice-val-correctness && xmake f -c -m release --liburing=n -y && xmake -y && xmake test -y
→ build ok, spent 1.677s
→ 100% tests passed, 0 test(s) failed out of 58, spent 4.000s
```

### 1.3 Killer suite 直跑（HEAD，release）
```
./build/linux/x86_64/release/direct_composition_fault_test
→ composition read_exact_at/write_all_at: 16 scenarios compared, 5 not drivable, 0 divergences
→ composition read_exact/write_all: 11 scenarios compared, 10 not drivable, 0 divergences
→ all 15 direct composition fault-injection tests passed   (exit=0)
./build/linux/x86_64/release/semantic_short_io_reference_test
→ all 9 short-I/O reference tests passed (8 primitive cases)   (exit=0)
```

### 1.4 突变独立复跑（2/10 个，覆盖 admission 与 compose 两侧）
| mutant | 命令 | 结果 |
|---|---|---|
| P2C1-5 `SLUICE_COMPOSE_MUTANT_SKIP_IMPOSSIBLE_COUNT` | `xmake build -y blocking_file_mut_skip_impossible_count` + 直跑 | exit=1，恰好一行 `FAIL: impossible_count_stops_immediately` → **RED** |
| C1-1 `SLUICE_SEM_MUTANT_ADMIT_INVALID_RANGE` | `xmake build -y blocking_file_mut_admit_invalid_range` + 直跑 | exit=1，恰好一行 `FAIL: read_with_illegal_offset_reports_invalid_argument` → **RED** |

两轮突变后 `git status --porcelain -- src/ include/ tests/ xmake/ docs/` 均为空，
HEAD 未变；P2C1-5 之后重建 clean `direct_composition_fault_test` exit=0（恢复绿）。
其余 8 个突变未重跑，采信 receipts `c1-mutation.md` / `p2c1-mutation-a.md` /
`p2c1-mutation-b.md`（BASE GREEN → MUTANT RED → RESTORED GREEN 逐个记录，与矩阵输入一致）。

### 1.5 ASan+UBSan 全量（同一 worktree 重配）
```
xmake f -c -m asanubsan --liburing=n -y && xmake -y && xmake test -y
→ 100% tests passed, 0 test(s) failed out of 58, spent 4.118s
```
另直跑 5 个核心二进制（direct_composition_fault / blocking_file_read /
blocking_file_sequential / blocking_file_state / semantic_short_io_reference），
全部 rc=0 且 `grep -c 'runtime error|AddressSanitizer'` = 0 行（零 sanitizer 报告）。

### 1.6 API/ABI
- **公开安装头零改动**：`git diff 43cb8029^..HEAD -- include/sluice/blocking
  include/sluice/async include/sluice/{effect,error,file_resource,measurement,result}.hpp`
  → 0 行。改动面仅为 `include/sluice/detail/file_semantics.hpp`（未安装 detail 头）、
  `src/blocking_file.cpp`、`xmake/tests.lua`（测试基建）。
- **归档符号集对比**（同工具链 release，`nm -g --defined-only` 全局定义符号）：
  - pre-C1 `ac995641` vs HEAD `84e1b7d5`：`libsluice_core.a` **22 符号 IDENTICAL**、
    `libsluice_async.a` **233 符号 IDENTICAL**（diff 空）。
  - `read_exact_at/write_all_at/read_at/write_at` 等 blocking 符号存在于 HEAD 归档。
- **安装面佐证**（读取 receipt `sdk.md`，本 ask 未重跑 verifier）：24/24 gates PASS
  （11 头精确安装、422 定义符号基线、ODR 宏无关、negative seam/mutant-define 探针）、
  独立 consumer probe 16/16（含 SEM-03 优先级、move/close 语义）。
- P2C1 对 detail 头的改动仅为 `compose_progress` 规则表加 `#if defined(SLUICE_*MUTANT*)`
  突变臂（无宏时惰性；宏仅存在于 `set_default(false)` 的 mutant target，
  `xmake/tests.lua:82-120` 核实，永不进入常规矩阵）。

### 1.7 语义与错误优先级（源码逐条核对）
- **SEM-03 优先级**：`precheck_data_op`（`include/sluice/detail/file_semantics.hpp:157-176`）
  顺序 closed→access→empty→range 与契约 §398-429 逐条一致。
  **C1 重构行为等价性**：pre-C1 `Precheck` 辅助（`git show 43cb8029^:src/blocking_file.cpp`：
  null 检查条件 `verdict==execute`，rejection 与 `complete_empty` 并行返回）与 HEAD
  dispatch（`src/blocking_file.cpp:89-96`：complete_empty → rejection → null）逐分支等价：
  `{nullptr,0}` span 两版均返回 0；execute+null 两版均 invalid_argument；
  closed+非空两版均 invalid_state。
- **SEM-04/cast 保值**：`range_is_valid`（:32-39）以无符号算术证明 offset+length-1 ≤
  INT64_MAX；组合步骤 offset+confirmed（confirmed < total）为已验证区间子集，
  `src/blocking_file.cpp:272-274,297-298` 注释成立，无符号回绕不可能。
- **SEM-05 组合**：HEAD `compose()`（:299-333）四出口与非权威规则表
  `compose_progress/compose_error`（file_semantics.hpp:243-281）一一映射：
  full→complete、error→prefix 保留 + `EffectCertainty::unknown`（ERR-02 "unknown
  remainder" 一致，:266-270）、transferred>remaining→invalid_state 停（不回绕）、
  0-progress→eof_before_full/write_no_progress（不空转）；EINTR 重试经
  `step_retry_on_eintr`（:78-84）仅重放未报字数的中断（契约 §469-473）。
  写组合仍逐步走公开 primitive admission（:364,391），读组合用 prevalidated 步骤
  （:347-349,377-378）——不对称即 SEM-05 披露所述事实，措辞（"the read compositions"）准确。
- **ERR-01 errno 映射**：`include/sluice/error.hpp:66-94`：EINTR→interrupted、
  EAGAIN/EWOULDBLOCK→would_block、EACCES/EPERM→permission_denied、ENOENT/ENOTDIR→not_found、
  ENOSPC/EDQUOT→no_space、其余→backend_error + `os_errno` 保留，与契约 §552-560 一致。
- **THREAD-01 并发生命周期**：契约 §1003-1022 "File move/close/destruction |
  Caller serialization" → SEM-05 契约外披露的竞争归类（caller data race）正确；
  in-contract 行为（入口 admission 不变、无泄漏、无新增 UB）与披露一致。

## 2. 矩阵与平台证据复核（读取 receipts，未全部重跑，如实标注）

- `release.md`（58/58）、`debug.md`+`18-debug-o0-suites.txt`（debug 58/58、-O0 两套件
  15/9 passed）、`asan.md`、`ubsan.md`、`asan-ubsan.md`（58/58、零报告）、
  `liburing.md`（y/n 双腿 137/137+53，路径调整如实记录）、`sdk.md`（24/24+16/16）、
  三个突变 receipts —— 存在、自洽、与矩阵输入一致。
- `receipts/15`：raw-close 探针 `end=3(primitive_error) code=backend_error os_errno=9(EBADF)`、
  全宽 off_t 偏移探针 PASS、clang+ASan/UBSan 重跑通过 —— 与 SEM-05 披露一致。
- `receipts/17`：顶边 pread EINVAL（tmpfs+ext4），规则=pos+count 符号回绕，
  provenance 标注 UNKNOWN —— 平台事实成立，Sluice 表面化良好且与 C1 基线相同（非回归）。
- `receipts/14` 头部自带口径免责："NOT comparable to it and transfer nothing semantic"。

## 3. 发现（PlatformFinding，全部 low；高危：无）

| # | what | where | whyRisky | severity | status | evidence |
|---|---|---|---|---|---|---|
| F1 | SEM-05 ledger 披露准确覆盖 EBADF-vs-invalid_state 差异，但未点名 fd 复用 flavor：THREAD-01 违反的并发 close + 描述符复用下，prevalidated 步骤可静默读到无关文件并报成功；暴露窗口 P2C1 从逐步 admission 扩大到整组合 | `src/blocking_file.cpp:275-285`（prevalidated_pread_step 无 is_open 重检；对比 :363-365/:390-391 写组合仍逐步 admission）+ `docs/roadmap/v2-conformance.md:45`（SEM-05 披露只写 "EBADF-class backend_error"）+ receipts/15 | ledger 读者可能把 EBADF 视为组合中途 close 的唯一可观察结果；错误文件静默读成功是同一已披露竞争的更危险 flavor。风险有界：该 interleaving 在两树均为 THREAD-01 调用方数据竞争（契约 §1003-1022），契约内行为不变。建议 ledger 补一句点名 fd-reuse flavor | low | verified | 本轮源码读取（不对称性独立核实）+ receipts/15 raw-close 探针 + ledger 原文；平台审查 C1/P2C1 finding 同源，本复审独立确认 |
| F2 | 平台顶边事实：SEM-04 admission 合法接受的顶边请求（offset+length-1 == INT64_MAX）被本机内核 6.18.54.1-microsoft-standard-WSL2 以 EINVAL（非 EOF）拒绝（tmpfs+ext4）；Sluice 以 primitive_error/backend_error/EINVAL 表面化，confirmed=0，与 C1 基线相同，非 P2C1 回归 | `include/sluice/detail/file_semantics.hpp:32-39` × 内核；`receipts/17-top-edge-einval-platform.txt`、`receipts/15`（top_edge PASS 行） | 在边界测试"admitted range 总会执行"的人会把内核 EINVAL 误归因于 Sluice；结果 well-formed 且基线一致，记录为平台事实即可 | low | verified | 读取 receipts/17（count/offset 矩阵 + read_exact_at outcome）；本轮未重放内核探针，采信 receipt 原始输出 |
| F3 | 测试缝潜在截断：native_pread/native_pwrite 脚本路径 `static_cast<long>(offset)`，在 ILP32+LFS（off_t 64 位、long 32 位）下会截断记录的偏移、无声削弱边界测试证据；生产步骤直接传 off_t 不受影响；审计平台 LP64 无影响 | `src/blocking_file.cpp:56,73`（仅 `SLUICE_FILE_INTERNAL_TESTING` 编译）；`file_semantics.hpp:18-24` static_assert 只约束 off_t/ssize_t 不约束 long | 若 fault-injection 目标未来在 ILP32+LFS 构建，边界偏移测试证据将失真且不报错。当前零影响 | low | verified | 本轮源码读取 + file_semantics.hpp static_assert 核对 |
| F4 | E5 (gcc 16.2.1) 机器码成本证据不可迁移 WSL2：整函数计数（GCC 15.2: read_at=111/read_exact_at=163；clang 21: 102/138）与 E5 exact-path 口径（≈40）双重不可比（编译器+口径）；WSL2 复核 C1 性能必须重测。零语义影响 | `research/600-direct-excess-cost/DIRECT_MACHINE_COST_LEDGER.md` vs `receipts/13,14` | 性能收益声明（−13.3ns~−16.6ns、70→40）被误迁移到 WSL2 会得出错误结论；receipts/14 头部已自带免责 | low | verified | 读取 receipts/13/14 与 c1-platform-compiler-audit.md:112-121；本轮未重做反汇编（如实标注） |
| F5（本复审新发现） | **证据引用不准确**：`c1-platform-compiler-audit.md:118-120` 及对应 finding 声称 "receipts/13 中 read_at 含 errno 映射跳转表 (`movslq (%rcx,%rax,4),%rsi; jmp *%rsi`)"，但 receipts/13（1517 行）中 `movslq` 与间接 `jmp *%rsi` 出现 **0 次**（grep 实测）；read_at 的 errno→Code 映射实际编译为直跳比较链（`mov $imm,%eax` / `je` / 共享错误块 `jmp 110`）。可能引自会话内另一次未归档的反汇编 | `docs/research/retention-wsl2-audit/c1-platform-compiler-audit.md:118-120` vs `receipts/13-c1-blocking-file-gcc152-O3.dis` | 该 finding 的结论（E5 计数不可迁移）不受影响——receipts/14 免责声明与 111-vs-40 口径差已独立支撑；但按引用原文核对时会得到"证据不匹配"，削弱审计报告可信度。建议将该句改为实际形态或补归档支撑反汇编 | low | verified | `grep -c "movslq\|jmp\s\*" receipts/13-…dis` = 0；`sed -n '88,116p'` 显示实际直跳形态；`grep -rn movslq docs/research/retention-wsl2-audit/` 仅命中报告自身 :119 |

## 4. UNKNOWN / 本 ask 未执行（如实标注）

1. **E5 gcc 16.2.1 本机重放**：宿主不可用，仅引用归档账本与 receipts 07（LP64 身份探针）。
2. **liburing=y 矩阵与 debug 全量矩阵**：本 ask 未重跑；采信 `liburing.md`（137/137）、
   `debug.md`/`18-debug-o0-suites.txt`。我重跑的是 release 58/58 与 asanubsan 58/58。
3. **其余 8 个突变**（C1-2/3、P2C1-1..4/6/7）：未逐个重跑；采信三个突变 receipts 的
   BASE/RED/RESTORED 记录；我独立复跑的 P2C1-5 与 C1-1 覆盖 compose 与 admission 两侧。
4. **-Wconversion/-Wsign-conversion 双编译器扫描**：采信 receipts `12-c1-wconversion-scan.txt`、
   `13-conversion-warnings.txt`，未重跑。
5. **内核顶边检查的 mainline 来源**：kernel source 未查（receipts/17 已如实标注 UNKNOWN）。
6. **io_uring/request 路径组合**：P2C1 范围外；C1/P2C1 diff 未触及任何 async TU，
   async 归档符号集 IDENTICAL 支持该范围声明（间接证据，非重放）。

## 5. 结论

C1（admission 直派发）与 P2C1（读组合 prevalidated 步骤）在契约内语义、错误优先级、
API/ABI、资源生命周期上与本复审独立核对全部一致；sanitizer 与突变证据在本机独立
复现（release 58/58、asan+ubsan 58/58 零报告、P2C1-5/C1-1 RED、pre-C1↔HEAD 符号集
IDENTICAL、公开头零改动）。四项平台 low findings 经独立核实成立且均不触及契约内
行为；另发现一处平台审计报告的指令形态引用不准确（F5，low，建议勘误）。
**verdict: OK；abiVerdict: ABI_CONSISTENT。**
