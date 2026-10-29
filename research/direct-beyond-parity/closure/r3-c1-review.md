# R3 — C1 Product PR #602 完整复审（独立 Correctness Review）

- 日期：2026-10-29（E5 native Linux，host 身份见
  `closure/E5_CLOSURE_SNAPSHOT.md` §1：E5 / Fedora 44 / 7.2.11-200.fc44.x86_64 /
  Xeon E5-2666 v3，gcc 16.2.1，xmake v3.1.2+HEAD.5d21e9d）。
- 执行者：R3 C1 复审工程师（E5 Closure workflow，独立复审，未沿用任何先前
  APPROVE 结论；下述每个 [RUN] 均为本 session 实测）。
- 评审对象：PR #602 head `77e5399a45998dea396699fa2e063c94a665945a`
  （= B1 锚点；`gh pr view 602` 实测 OPEN / 非 draft / base=master /
  headRefOid=77e5399a / mergeable=MERGEABLE / updatedAt 2026-10-26T16:51:50Z；
  `git ls-remote origin refs/heads/sluice-600-direct-c1` 实测同一 SHA）。
- 隔离工作区：`git worktree add /home/jnhu/Source/Sluice-e5-c1 --detach 77e5399a…`
  （新建，不触碰冻结 worktree `Sluice-600-base`/`Sluice-600-wt`）。
- Diff 范围实测（`gh pr diff 602 --name-only` 与
  `git diff --name-only bdfd0d8d..77e5399a` 一致）：5 文件 ——
  `docs/roadmap/v2-conformance.md`（ledger SEM 行证据回写）、
  `include/sluice/detail/file_semantics.hpp`（删 `checked_posix_offset` +
  3 个编译期 mutant 钩子）、`src/blocking_file.cpp`（`Precheck` 物化 →
  `DataOpVerdict` 直派）、`tests/semantic_range_test.cpp`（被删 helper 的唯一
  单测改写为 `precheck_data_op` 层断言）、`xmake/tests.lua`（3 个
  `set_default(false)`、无 `add_tests` 的 mutant target）。

## 0. 结论

```text
C1_PRODUCT_REVIEW = APPROVE
PR602             = OPEN_NOT_MERGED（未 merge、未 push、未 force-push；
                    本轮无需修复提交，fixesPushed = none）
代码身份保持       = true（热路径 blob 与冻结 E5 性能证据完全一致 →
                    冻结性能证据对当前 head 继续有效，无需性能复验；
                    本阶段按任务书 §6 未运行任何 benchmark）
```

发现的问题：0 个阻断项。3 条非阻断观察见 §7。

## 1. 测试矩阵（[RUN]）

全部在 `/home/jnhu/Source/Sluice-e5-c1`（head 77e5399a，树全程 clean）按
冻结证据口径（`xmake f -m <mode> --liburing=n -y && xmake -y && xmake test -v`）
执行：

| 配置 | 命令 | 结果 |
|---|---|---|
| Release | `xmake f -m release --liburing=n -y && xmake -y && xmake test -v` | **58/58 passed, 0 failed**（exit 0） |
| Debug | 同上 `-m debug` | **58/58 passed, 0 failed** |
| ASan | 同上 `-m asan` | **58/58 passed, 0 failed** |
| UBSan | 同上 `-m ubsan` | **58/58 passed, 0 failed**（日志 0 条 `runtime error`） |
| ASan+UBSan | 同上 `-m asanubsan` | **58/58 passed, 0 failed**（0 条 `runtime error`） |
| Release + liburing=y | 同上 `-m release --liburing=y` | **79/79 passed, 0 failed**（含 io_uring 后端套件） |

与 PR 正文/冻结证据 `PHASE3_VALIDATION.md` V1 表（release/debug/asan/asan+ubsan
58/58）一致 [TRANSCRIBED 对照 + 本轮 RUN 重放]。

## 2. 三个 C1 命名 Mutation：BASE_GREEN → MUTANT_RED → RESTORED_GREEN（[RUN]）

- **BASE_GREEN**：上表 Release 58/58。
- 构建：`xmake build -y blocking_file_mut_admit_invalid_range
  blocking_file_mut_empty_before_closed
  blocking_file_mut_admit_unrepresentable_size`（release，exit 0）。
- 逐个运行（`./build/linux/x86_64/release/blocking_file_mut_<name>`）：

| Mutant define | Exit | 失败断言（唯一一行输出） | 与 PR 正文/ledger 命名一致 |
|---|---|---|---|
| `SLUICE_SEM_MUTANT_ADMIT_INVALID_RANGE` | 1 | `FAIL: read_with_illegal_offset_reports_invalid_argument` | ✓ |
| `SLUICE_SEM_MUTANT_EMPTY_BEFORE_CLOSED` | 1 | `FAIL: sequential_read_on_write_only_file_rejected_upfront` | ✓ |
| `SLUICE_SEM_MUTANT_ADMIT_UNREPRESENTABLE_SIZE` | 1 | `FAIL: resize_overflow_reports_invalid_argument` | ✓ |

每个 mutant 输出**恰好 1 行 FAIL**（runner 首个失败即退出），即 kill 精度为
单一命名断言，无附带失败。

Kill 链有效性证明（本复审独立核实，非沿用记录）：

- ADMIT_INVALID_RANGE：`precheck_data_op` 在 range 检查前直接
  `return execute`（file_semantics.hpp:168-170）。killer
  `read_with_illegal_offset_reports_invalid_argument`
  （tests/blocking_file_read_test.cpp:122-137）以 `read_at(file, UINT64_MAX, 1B)`
  期望 `invalid_argument`。mutant 下 cast 回绕为 −1 → `pread` 返回 EINVAL →
  `canonical_error_code`（error.hpp:66-88）中 EINVAL **不在映射表** →
  `backend_error ≠ invalid_argument` → 断言失败。kill 不依赖 errno 巧合：
  EINVAL/任意 errno 均不可能映射为 `invalid_argument`（映射表仅
  EACCES/EPERM/ENOENT/ENOTDIR/ENOSPC/EDQUOT/EINTR/EAGAIN/EWOULDBLOCK）。
- EMPTY_BEFORE_CLOSED：mutant 把 `length==0 → complete_empty` 提到 closed 与
  access 检查之前（file_semantics.hpp:158-161）。killer
  `sequential_read_on_write_only_file_rejected_upfront`
  （tests/blocking_file_sequential_test.cpp:392-415）第二段对 write-only 文件做
  **零长度** read 并要求 `invalid_argument`；mutant 下提前返回 0（has_value）→
  断言失败。closed+empty 臂
  `sequential_zero_length_read_after_close_reports_invalid_state`（:418-437）
  是同一优先级规则的第二个探针。
- ADMIT_UNREPRESENTABLE_SIZE：mutant 删除 `size_is_representable` 检查
  （file_semantics.hpp:192-195）。killer `resize_overflow_reports_invalid_argument`
  （tests/blocking_file_state_test.cpp:320+）以 UINT64_MAX resize 期望
  `invalid_argument`；mutant 下 `ftruncate` 收到回绕负值 → EINVAL →
  `backend_error` → 断言失败。

- **RESTORED_GREEN**：mutant 仅为 target-scoped define（`xmake/tests.lua:357`
  `add_defines(..., define)`；mutant target 无 `add_tests`、`set_default(false)`，
  常规 `xmake test` 不含也不受影响）。源码零改动
  （`git status --porcelain -- src/ include/ tests/` 为空）后重跑常规 Release
  套件：**58/58 passed, 0 failed**。

## 3. Installed SDK 独立消费者验证（[RUN]）

1. **Package verifier**（v2 包边界的在树权威）：
   `python3 scripts/verify_v2_package.py --mode release --manifest-out
   /tmp/r3c1_v2-package-manifest-noliburing.json --keep-prefix`
   → **24/24 gates PASS**（含 `V2_B_PACKAGE_PREFIX_READY`（恰好 11 个契约头）、
   `CLEAN_ROOM_*`、全部 `P1_*`、`NEGATIVE_V2_REMOVED_HEADERS_UNAVAILABLE`、
   `NEGATIVE_V2_SEAM_UNAVAILABLE`、`NEGATIVE_V2_MUTANT_DEFINES_NOT_INSTALLED`、
   `NEGATIVE_ASYNC_SYMBOLS_NOT_IN_CORE`）。
   manifest `VERIFICATION_HEAD=77e5399a…`。
2. **独立消费者探针**（复审者自写，`/tmp/r3c1_sdk_consumer_probe.cpp`，仅
   `#include <sluice/blocking/file.hpp>` 尖括号包含，对保留前缀
   `/tmp/sluice-v2-prefix-gi_kpflm` 编译链接 `-lsluice_core`）：
   **21/21 PASS, failures=0**。覆盖：UINT64_MAX offset → `invalid_argument`；
   closed 文件零长度 → `invalid_state`（closed 先于 empty）；write-only 文件
   非零/零长度 read → `invalid_argument`（access 先于 empty）；null buffer
   非零 → `invalid_argument`、null buffer 零长度 → 0（empty 先于 null）；
   open 文件零长度 → 0；resize UINT64_MAX → `invalid_argument`、合法 resize
   生效；write_all_at/read_exact_at 往返字节相等；past-EOF read_exact_at →
   `eof_before_full` + `confirmed_bytes`=前缀长。
   （探针首轮曾 2 FAIL——probe 自己用 read_only 打开却 resize，SDK 正确按
   access 规则拒绝；修正 probe 后全绿。这同时是对 access 检查的意外正向确认。）
3. **syscall 计数 oracle**（`strace -f -e trace=pread64,pwrite64`）：admission
   拒绝与 zero-length 探针**零** pread/pwrite；`write_all_at` 1×pwrite64；
   `read_exact_at` 满足时 1×pread64；31B 请求 23B 文件的 past-EOF 组合 =
   23B 一步 + `pread64(...,8,23)=0` EOF 步（每步恰 1 syscall）。
4. **安装面负探针**（文件系统级）：`ls <prefix>/include/sluice/detail` →
   不存在；`detail/file_semantics.hpp`（含被删的 `checked_posix_offset` 与
   mutant 钩子）不在安装集，与 ledger G9 行一致。

## 4. §6 十项语义清单（逐项，对照 head 源码）

1. **DataOpVerdict Admission Precedence — PASS。**
   `precheck_data_op`（file_semantics.hpp:157-176）：closed → access →
   complete_empty → range → transfer-cap → execute，与被删 `Precheck`
   （master `src/blocking_file.cpp` blob b9c0def8 的 `precheck()`）的观测顺序
   逐臂等价：rejection → `rejection_of(verdict)`；execute+null → invalid_argument；
   complete_empty → 0。9 个调用点（4 data ops、4 composition 入口、resize 经
   `precheck_resize`）全部经同一 verdict 派发；错误码映射
   （reject_closed→invalid_state、reject_access/reject_range→invalid_argument，
   hpp:116-129）未变。
2. **Range/Offset — PASS。** `range_is_valid`（hpp:32-39）offset ≤
   `kMaxNativeOffset` 且尾字节 `length-1 <= kMaxNativeOffset - offset` 无溢出；
   transfer-cap 检查（hpp:173-174）保留，与 range 同映射 `reject_range`。
   组合推进 offset（`offset + confirmed`）受入口验证支配：confirmed < total
   （blocking_file.cpp:273-281）⟹ offset+confirmed ≤ offset+size−1 ≤ OFF_MAX，
   uint64 加法无溢出。
3. **`static_cast<off_t>` 安全证明 — PASS（证明成立）。** 静态断言
   （hpp:18-22）锁定 off_t 有符号 64 位；`kMaxNativeOffset` =
   `static_cast<uint64_t>(OFF_MAX)`（hpp:26-27）。三个 cast 点
   （blocking_file.cpp:91、112、209）都仅在 verdict==execute 时到达：
   data op execute ⟹ length ≥ 1 且 `range_is_valid` 通过 ⟹ offset ≤ OFF_MAX；
   resize execute ⟹ `size_is_representable`（hpp:193-195）⟹ new_size ≤ OFF_MAX。
   被删 `checked_posix_offset` 的失败臂在每个旧调用点均为支配不可达
   （execute 已蕴含可表示；empty 臂更早返回），删除为纯语义保持收缩；其唯一
   单测改写为 `precheck_data_op` 层同规则断言
   （tests/semantic_range_test.cpp:89-101 → reject_range/invalid_argument），
   端到端规范位测由 `read_with_illegal_offset_reports_invalid_argument` 承担。
4. **Null Buffer — PASS。** 8 个调用点全部在 verdict==execute 后（即所有
   closed/access/empty/range 拒绝之后）检查 `data()==nullptr` →
   invalid_argument（blocking_file.cpp:86-87、107-108、126-127、144-145、
   294-296、310-312、324-326、338-340），与旧 `precheck()` 内
   `verdict==execute && buffer==nullptr` 完全同序。零长度 null span 仍走
   complete_empty → 0。
5. **Zero-Length — PASS。** complete_empty → `Result{0}` /
   `CompositionOutcome{}`，先于 null 检查、零 syscall（strace 实测，§3.3）。
6. **Closed / Access Permission — PASS。** reject_closed→invalid_state、
   reject_access→invalid_argument（hpp:116-129）；mutant 优先级证据（§2）与
   独立探针（§3.2）双向确认 closed>empty、access>empty。
7. **EINTR / Error Mapping — PASS。** 所有 native 调用仍包 `retry_on_eintr`；
   errno 仍只在 n<0/rc<0 后经 `from_errno_value` 读取（blocking_file.cpp:93-94、
   114-115、132-133、150-151、165-166、181-182、211-212、226-227）；映射表
   （error.hpp:66-88）未被 diff 触碰。
8. **Composition — PASS。** `compose` 循环、`compose_progress`/`compose_error`/
   `direct_outcome`、`CompositionStop`/effect-certainty 规则均不在 diff 内；
   四个组合入口仅替换前置派发（complete_empty→`CompositionOutcome{}`、
   rejection→error），步内 re-admission 行为不变。探针往返 + EOF/前缀规则 +
   strace 每步 1 syscall 实测通过。
9. **File Lifetime — PASS。** `src/file_resource.cpp` 不在 diff
   （blob 62e1a678 master=head 相同）；open/close/move/借出语义未动；
   file_resource/lifetime 相关套件在全部 6 个配置中绿。
10. **Public ABI / Installed SDK — PASS。** `include/sluice/blocking/file.hpp`
    blob master=head 相同（e62a33c39425de0fc0eb2ea10d34fa33c3177447）；
    `sluice_core`/`sluice_async` 安装面 11 头恰好（verifier）；detail 头不安装；
    mutant define 不外泄（NEGATIVE_V2_MUTANT_DEFINES_NOT_INSTALLED PASS，该 gate
    在 master `scripts/verify_v2_package.py` 已存在，非本 PR 引入）；
    独立消费者对已安装前缀编译链接运行成功。

## 5. 热路径代码身份（[RUN]）

冻结 E5 性能证据（#601 分支 `research/600-direct-excess-cost/`，
`PHASE3_VALIDATION.md` / `EVIDENCE_INDEX.md` [TRANSCRIBED]）记录
BASE_SHA `bdfd0d8d` → CANDIDATE_SHA `ea2fc4fca1a6`，库 sha256
baseline `489104b5…` / candidate `d1e351a5…`。

本复审实测：

- `git diff ea2fc4fc 77e5399a -- src/ include/ apps/ xmake.lua xmake/` →
  **空**（生产路径零差异）；两 tree 的全部差异仅
  `docs/roadmap/v2-conformance.md`。
- 热路径 blob（ea2fc4fc / 77e5399a 逐位相同）：
  `src/blocking_file.cpp` = `6660a42367ef030d250e72e11a7a4c48b1ce76d0`、
  `include/sluice/detail/file_semantics.hpp` =
  `e39760ae534a2b65b3c5e3f5efee4ed6e815a7e0`、
  `src/file_resource.cpp` = `62e1a678…`、
  `include/sluice/blocking/file.hpp` = `e62a33c3…`。

结论：**冻结 E5 性能证据对 #602 当前 head 继续有效**（V2 ABBA 表：
read_at 0B −13.3 ns、1B −16.4、64B −15.8、512B −16.2、4KiB −16.6、64KiB n.s.、
read_exact_at 4KiB −33.8 ns；syscall/op=1.000 [TRANSCRIBED]）。
**不标记需要补充性能复验**；按任务书 §6 本阶段未运行任何 benchmark。

## 6. Conformance Ledger 与 CI 审查（[RUN]）

- **Ledger**：head `docs/roadmap/v2-conformance.md:44` SEM-01..04/ERR-01 行的
  Status 列含 "live mutation evidence 2026-10-27 … three named admission
  mutants … each killed by its named suite assertion; release/debug/asan/
  asan+ubsan 58/58; package verifier all gates"。本复审逐项重放（§1–§3）与该
  记录一致；证据列（v1 campaign、in-tree semantic 套件、#531 request-path
  re-diff）原样保留，缺口列（G9 uninstalled-header 负探针）与 §3.4 实测一致。
  状态用词符合 GOV-02 的 VERIFIED（carried + live evidence）形态。
- **CI**：`gh pr checks 602` → 6/6 pass（C++ comment authority guard、
  CodeRabbit〔skip: manual review required〕、Linux asan tail CLI oracle、
  Linux debug build + tests、Linux release build + tests、Package verifier
  surface self-tests）。`gh run view 40363163278` → headSha = 77e5399a…、
  completed/success。**无失败检查，无失败原因需要分析**。
  （注：CodeRabbit 为 "Review skipped"，非实质评审门。）

## 7. 观察记录（非阻断）

1. `blocking_file.cpp:88/109/207` 的注释 "The execute verdict proved offset
   fits off_t; the cast cannot lose bits." 与 `:267-268` 的 compose 不变式注释
   属 AGENTS.md 允许的"非显然局部不变式/危险点"短注（cast 的证明义务恰好是
   删注释后最易犯错处），予以保留认可。
2. mutant 钩子位于生产 detail 头（file_semantics.hpp:158-161/168-170/192-195），
   但为编译期、target-scoped、默认关闭，无运行时状态；verifier 负探针确认
   不外泄安装面；`xmake/tests.lua:346-350` 注明 "executed and recorded
   manually, never run as regular tests"。分类：test-only evidence，机制
   合理性成立。
3. 本地测试矩阵以 `--liburing=n`（58/58×5 配置，与冻结证据与 CI 同口径）为主，
   另补 release+liburing=y 79/79；#531 的 liburing=y 68/68 request-path
   re-diff 为 carried 证据未重放（PR 不触碰 request 路径）。
   未重跑 Phase I 成本探索（任务书 §6 明示不需要）。

## 8. 边界遵守

未 merge 任何 PR；未采纳新公开 API；未改 v2-r1 契约；未 push（无需修复提交，
更无 force-push）；未运行任何 benchmark；未削弱任何 oracle（所有失败期望
均为负向断言，无 sleep/retry 放水）；未触碰冻结 worktree 与其他 Agent 文件
（本报告为 closure/ 下新增文件）。测量/重放标注：本文件 §1–§6 全部为 [RUN]；
冻结性能数字为 [TRANSCRIBED]；无 [UNCERTAIN]，无 NOT_RUN_WITH_REASON 项。
