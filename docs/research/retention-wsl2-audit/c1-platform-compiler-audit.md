# C1 平台与编译器假设审计（retention-wsl2-audit）

- 审计对象：`master` = `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`；C1 相关提交
  `43cb8029`（#600 C1 admission dispatch，DataOpVerdict 取代 Precheck 具现化，
  删除 `checked_posix_offset`）与 `84e1b7d5`（#608 P2C1 read compositions
  prevalidated register-resident steps + composition mutant seams）。
- 审计角色：平台审查·C1（只读审计 + 非破坏性验证；未改动任何产品代码、分支、
  worktree 或证据目录）。
- 审计日期：2026-10-29。
- 验证工具链（WSL2 宿主，与 E5 不同）：gcc 15.2.0 (Ubuntu 15.2.0-16ubuntu1)、
  clang 21.1.8 (6ubuntu1)、glibc 2.43、kernel 6.18.54.1-microsoft-standard-WSL2、
  x86_64。E5（gcc 16.2.1, Red Hat）本机不可用，未在本机重放 E5 机器码；E5 数字
  一律引用归档记录（research/600-direct-excess-cost/）并按其记录的口径标注。
- 验证 worktree：`/tmp/sluice-val-c1-plat`（detached @ 84e1b7d5，留给脚本统一清理；
  本审计未执行 `git worktree remove`）。全部构建只发生在该 worktree，
  主工作区无任何构建。

## 结论

**0 high、0 medium。2 条 low（一条为已披露的契约外行为差异确认，一条为 E5 代码
生成证据不可迁移的确认）。** 未发现未定义行为、有符号溢出、越界、严格别名违例、
未初始化读、会丢失数值的隐式 int↔off_t/ssize_t 窄化，或依赖特定编译器优化/内联
才成立的语义。C1 的语义正确性由入口 admission 的算术保证，与优化器无关。

## 逐范围审计

### DataOpVerdict 与 Precheck 消除（include/sluice/detail/file_semantics.hpp:103-197, src/blocking_file.cpp:26-30,87-94 等）

- `precheck_data_op` 优先级 closed → access → empty → range → `length >
  kMaxNativeTransfer`（file_semantics.hpp:157-176）。每个分支返回独立枚举值，
  调用方以 `complete_empty` / `is_rejection` / `rejection_error` 分发
  （blocking_file.cpp:89-94 等）。逐个核对了 blocking_file.cpp 全部 8 个生产
  调用点与 uring/threadpool 后端的 validate_* 调用点：不存在未先判
  `is_rejection()` 就取 `rejection_error()` 的路径；`rejection_error` 对非拒绝
  值的兜底返回仅是防御性注释所描述的约定，无生产触达。
- Precheck 具现化是纯性能问题（E5 账本 DIRECT_MACHINE_COST_LEDGER.md：SRA 不
  能标量化 `Precheck{optional<IoError>,bool}`）；DataOpVerdict 是单枚举，
  语义在有无内联时相同。无"依赖优化才成立"的语义。
- 三个 admission mutant（ADMIT_INVALID_RANGE / EMPTY_BEFORE_CLOSED /
  ADMIT_UNREPRESENTABLE_SIZE）在 GCC 15.2 与 clang 21.1.8 下均被击杀
  （receipts 09、10，exit=1 × 10 × 2 编译器）。

### Offset Conversion 与 off_t Boundary（blocking_file.cpp:97-101,118-122,216-218,272-285）

- 全部 4 处生产 `static_cast<off_t>(offset/new_size)`（blocking_file.cpp:100、
  121、218、279）都位于 verdict==execute 之后；execute 蕴含
  `range_is_valid(offset, length>0)`，即 `offset + length - 1 ≤ off_t::max`。
  `range_is_valid`（file_semantics.hpp:32-39）全部使用无符号算术
  （`length-1 <= kMaxNativeOffset - offset`），无任何有符号溢出；
  uint64→int64 转换即使越界在 C++20 也是模定义。该证明只依赖入口 admission
  的执行顺序，不依赖编译器。
- 组合路径 `offset + confirmed`（blocking_file.cpp:348）：confirmed < total ≤
  2^63-1 且入口已证 `offset + total ≤ 2^63`，故和 ≤ off_t::max，无回绕。为
  uint64 无符号加法，即便违约也只是定义行为，随后被 cast 前的值域证明兜住。
  P2C1 把逐步 `read_at` 改为 `prevalidated_pread_step` 后，逐步不再重验
  range——但入口 admission 已覆盖整个请求，步骤偏移是它的子区间，证明保持
  完整。这既是设计也是本轮重点核对项。
- `length > kMaxNativeTransfer`（SSIZE_MAX 上限）不是冗余：`range_is_valid(0,
  2^63)` 恰好放行 length==2^63、offset==0，由该检查拒绝。边界由
  tests/semantic_range_test.cpp:104-118
  （`native_transfer_limit_is_a_range_rejection`，`kMaxNativeTransfer + 1`）
  与 :44-50 的 kMaxNativeOffset±1 表钉住；测试 oracle 用 `unsigned __int128`
  独立复算（semantic_range_test.cpp:37），与生产算术不同源。
- 后端对称性：uring/threadpool 的提交路径在 prepare 之前统一走
  `validate_op`（uring_backend.cpp:430、threadpool_backend.cpp:218），随后的
  `static_cast<off_t>(static_cast<int64_t>(p.offset))`
  （threadpool_backend.cpp:485、499；uring_transport.cpp:169、173）受同一
  admission 约束；中间 int64 转换在已验证值域内保值。

### Admission Priority（file_semantics.hpp:157-176）

- 顺序 closed > access > empty > range：`SLUICE_SEM_MUTANT_EMPTY_BEFORE_CLOSED`
  击杀证明"empty 先于 closed"会被测试发现（receipts 09/10）；优先级 oracle
  表（frozen table vs 共享规则）在 direct_composition_fault_test main 入口
  （tests/direct_composition_fault_test.cpp:662-668）先行校验，套件通过。
- `File::access()` 在构造后不可变（file_resource.hpp:88-94），write 组合逐步
  重验（write_all_at/write_all 仍走公共 write_at/write）只能翻转 closed 位，
  与 THREAD-01 的调用方串行化要求一致。

### Zero-Length（blocking_file.cpp:91-92,112-113,131-132,340-341,356-357,370-371,384-385）

- length==0 在 closed/access 之后返回 `complete_empty`：read/write 返回
  `size_t{0}`，组合返回默认 `CompositionOutcome{}` = {0, complete, accounted,
  nullopt}（blocking/file.hpp:42-49），与单发语义一致；全程不触 fd、不进
  syscall，`offset == UINT64_MAX` 的零长请求也被放行（不执行 off_t cast）。
- nullptr 检查在 verdict 之后：仅 execute（length>0）才要求 data() 非空
  （blocking_file.cpp:95-96 等）；零长 + nullptr 短路于 complete_empty，
  不会把空指针传给 read/pread。后端一致：uring 以 `zero_op` 标记内联发布
  （uring_backend.cpp:470-477、520-521），避免把 len==0 提交进 io_uring
  （内核对 len==0 读有特殊语义）——该守卫先于 C1 存在，本轮确认未被破坏。

### Error Mapping（include/sluice/error.hpp:60-95, src/blocking_file.cpp:102-105 等 8 处）

- `retry_on_eintr`（posix_retry.hpp:8-15）仅在 `result < 0` 时读 errno，成功
  路径不读；失败返回后到 `from_errno_value(errno)` 之间没有任何函数调用，
  errno 不会被遮蔽。EINTR 由循环消化，`interrupted` 映射经该路径不可达
  （仅在无 retry 的 mutant 中出现，被 STEP_SKIPS_EINTR_RETRY 击杀证明）。
- `canonical_error_code(0) → backend_error` 防止 errno==0 误映射；
  `EWOULDBLOCK==EAGAIN` 由 `#if` 去重（error.hpp:79-82）；未映射 errno
  （含 EBADF、EINVAL）统一 backend_error 且 `os_errno` 保留原值。
  `request_error_domain_threadpool_test`（errno re-diff，ENOSPC/EISDIR 注入）
  在 GCC 15.2 下通过（receipts 08 末行）。
- P2C1 的 `primitive_error_outcome` 直接聚合初始化
  （blocking_file.cpp:266-270）只消除死零存储，错误分类链与 certainty
  （primitive_error/impossible_count → unknown；eof/no-progress → accounted）
  与旧 `direct_outcome` 映射及 `compose_progress`/`composition_error` oracle
  逐值一致（file_semantics.hpp:274-360）。FAILURE_ACCOUNTED mutant 被击杀。

### E5 vs WSL2 代码生成（重点项）

- E5 账本（research/600-direct-excess-cost/DIRECT_MACHINE_COST_LEDGER.md）的
  ≈70→≈40（read_at）等数字是 **E5 gcc 16.2.1、-O3 -DNDEBUG、success/exact-path
  口径**。WSL2 GCC 15.2.0 同开关整函数计数（receipts 13/14）：read_at=111、
  read_exact_at=163、write_all_at=178；clang 21.1.8 为 102/138/103（会话内
  objdump 计数，.comment 已验证编译器）。**计数不可跨工具链/口径比较；任何
  在 WSL2 上的 C1 性能复核必须重测，不能引用 E5 计数。** 这只影响性能证据，
  不影响语义：全部语义套件在两编译器 + ASan/UBSan 下一致通过。
- WSL2 GCC 15.2 的 read_at 把 errno→Code 映射内联为跳转表
  （receipts 13，`movslq (%rcx,%rax,4),%rsi; jmp *%rsi`），E5 账本未描述该
  形态——进一步印证两宿主机器码不同源。
- 语义层面未发现任何依赖 E5 代码生成模式的构造：无内联依赖、无优化器提示、
  无 `if constexpr` 分支语义、无内建函数依赖；测试 oracle 的
  `unsigned __int128` 仅测试用且两编译器均支持（x86_64）。

## assumptionsChecked（已检查并放心的假设）

1. `range_is_valid` 的无符号算术在任何合法输入下无回绕、无 UB；边界表
   （offset=off_t::max±1、length=SIZE_MAX、length=SSIZE_MAX+1、零长+越界
   offset）全部通过（semantic_range_test，GCC/clang 双跑 + ASan/UBSan）。
2. uint64→off_t cast 仅在 execute verdict 之后执行，值域由入口 admission
   证明；即使前置条件被违约，C++20 下该转换也是模定义而非 UB。
3. `offset + confirmed`（组合步骤）有界 ≤ off_t::max，且为定义的无符号加法。
4. `length > kMaxNativeTransfer` 检查非冗余（恰好覆盖 length==2^63,
   offset==0），且在 range 之后只影响该单一边界。
5. errno 时序：syscall 失败→retry 循环→`n<0`→`from_errno_value(errno)` 之间
   无 errno 遮蔽点；retry 循环只在 `result<0` 时读 errno。
6. 零长请求不触 fd、不进 syscall、不执行 off_t cast；nullptr 缓冲仅对
   length>0 报 invalid_argument。
7. 组合结果映射（complete/eof/no-progress→accounted；primitive_error/
   impossible_count→unknown + invalid_state 或原错误）与共享 oracle 一致；
   零长组合默认值 = complete/accounted。
8. mutant 宏只存在于 `blocking_file_mut_*` 测试目标（xmake/tests.lua:81-121），
   生产 sluice_core/sluice_async 不定义任何 SLUICE_*_MUTANT。
9. admission mutant、composition mutant 共 10 个在 GCC 15.2 与 clang 21.1.8
   下全部被击杀（exit=1）。
10. xmake release = `-O3 -DNDEBUG -Wall -Werror`（compile_commands 验证）；
    blocked 于 -Wconversion/-Wsign-conversion 的窄化扫描两编译器均零告警
    （receipts 12）——C1 文件无隐式窄化。
11. off_t/ssize_t/size_t/long 在 WSL2 x86_64 均为 64 位、`long` 同型，
    `_FILE_OFFSET_BITS` 未设也不需要（receipts 07）；file_semantics.hpp:18-24
    的 static_assert 会在 32 位无 LFS 的 ABI 上编译期失败（守卫有效；
    即使无守卫，kMaxNativeOffset 由 off_t::max 推导，逻辑仍自洽）。
12. `Result<T>` 存储为标准 placement-new union（result.hpp:11-80），无类型
    双关；C1 路径上的使用均为按值返回，无悬垂。
13. 组合循环不变式 confirmed < total 在每次 primitive 调用前成立；
    `transferred > total - confirmed` 守卫（impossible_count）阻止任何
    越界推进并发布 unknown certainty，与 oracle 一致。
14. uring 零长请求走内联发布（zero_op），不依赖内核 len==0 特殊语义；
    threadpool 零长为 count=0 的 pread/pwrite，内核直接返回 0。
15. THREAD-01 要求 File close/destroy 由调用方串行化（explicit-io-contract.md:1003-1023
    最后一行）；组合中途并发 close 是契约外调用方数据竞争。
16. 顶端边界的内核行为：offset+count ≤ off_t::max 的合法 admitted 请求在
    WSL2 kernel 6.18 上仍可能被内核以 EINVAL 拒绝（tmpfs/ext4 均如此）；
    Sluice 如实映射为 primitive_error（os_errno=22 保留、certainty 按错误
    类别发布），无 UB、无错误数据。本审计直接读到并行审计留存探针
    receipts/17-top-edge-einval-platform.txt 与 15-boundary-probe-and-clang.txt
    （含 ASan/UBSan 重跑）作为佐证，其结论与本报告的映射审计一致；
    该探针非本轮执行，仅作旁证引用。

## 未执行 / UNKNOWN

- E5（gcc 16.2.1）机器码未在本机重放：该编译器/宿主不在 WSL2 上可用。E5 数字
  均引用归档账本并按其口径标注，未独立复算 E5 侧计数（本机不可行）。
- dududu 本地副本未克隆（可选项；C1 审计范围不需要；E5 账本已记录 dududu
  adapter 的 file_info+size 重复 fstat 为 adapter 缺陷，非 Sluice C1 代码）。
- debug（-O0）模式套件未跑：审计结论是语义与优化无关（构造上无 UB、无优化
  依赖），release + release+ASan/UBSan + 双编译器已覆盖该声明的验证需要。
- `request_error_domain_uring_test`（liburing=y）未跑：本轮以 `--liburing=n`
  配置验证（liburing=y 的 79/79 矩阵属合并前已归档证据；本轮范围为 C1 直接
  路径平台假设）。

## Receipts（docs/research/retention-wsl2-audit/receipts/）

- 07-c1-toolchain.txt / 07-c1-typesize.cpp.txt — 宿主工具链身份与
  off_t/ssize_t 探针（源码+输出）。
- 08-c1-gcc152-suite-runs.txt — 9 个套件（8 个 C1 直接相关 +
  request_error_domain_threadpool_test）GCC 15.2 release 全过。
- 09-c1-gcc152-mutant-kills.txt — 10/10 mutant 击杀（GCC）。
- 10-c1-clang21-suite-and-mutants.txt — 8 套件全过 + 10/10 击杀（clang，
  .comment 验证）。
- 11-c1-gcc152-asanubsan-runs.txt — 8 套件 ASan+UBSan 全过（GCC）。
- 12-c1-wconversion-scan.txt — -Wconversion/-Wsign-conversion 双编译器零告警。
- 13-c1-blocking-file-gcc152-O3.dis / 14-c1-gcc152-instruction-counts.txt —
  GCC 15.2 -O3 -DNDEBUG 反汇编与整函数计数（read_at=111、read_exact_at=163），
  用于证明 E5 计数不可迁移。
