# Explicit File App Consumer Census

- **Authority**: [`docs/mission.md`](../mission.md) → [`ADR-0001`](../adr/0001-explicit-io-design-doctrine.md) → [`ADR-0002`](../adr/0002-explicit-file-api-architecture.md)
- **Verified implementation baseline**: `d7511349990cbb3ef340e158c7816fe3296b0575`（该 commit 可 checkout 并验证下文记录的消费现实）
- **Non-authority**: 本文只记录四个应用对 canonical File boundary 的消费现实与 escape 分类；它不创造 File 语义。若本文与 ADR 冲突，以 ADR 为准。
- **Scope**: `apps/sluice-copy`、`apps/sluice-hash`、`apps/sluice-grep`、`apps/sluice-tail`（roadmap A2 / Issue #342）

---

## 1. 分类词汇

每个 File-resource / POSIX escape 恰好一个分类：

```text
CANONICAL_FILE_USE            已经通过 canonical File surface 消费（记录正确现状）
REQUIRED_INTEROP              canonical File 已是 resource owner / canonical surface 无法无损表达，
                              app 暂时必须通过 native_handle() 或 POSIX 观察必要事实
TEMPORARY_CONVERGENCE_GAP     属于 File 架构，但所需 canonical API 由后续 roadmap node 提供
OUT_OF_SCOPE_NAMESPACE_WORK   不属于 canonical File-data resource contract 的 namespace / 权限操作
```

`CONFORMING`（roadmap 语境）不等于 zero `native_handle()`、不等于 zero POSIX 调用；它表示：canonical File 已经是该由 File 表达的资源 authority，剩余 escape 均已分类并有权属。

## 2. Census 方法

机械扫描（全 `apps/<app>/**`，含 task/helper/cli/README 之外的代码）覆盖：

```text
::open ::close ::fstat ::stat ::ftruncate ::rename ::unlink ::mkstemp ::fchmod ::fsync
int fd / *_fd
ReadOp WriteOp SyncDataOp SyncAllOp
submit_read submit_write submit_sync_*
await_read_once await_read_fill await_write_exact
await_read_at await_write_at await_sync_data
native_handle File::open
```

扫描结果逐条落入下表；`UNCLASSIFIED_ESCAPE_COUNT = 0`。

Census exclusions：继承的 stdout/stderr/TTY 观测（如 sluice-grep main 的 `::isatty(STDOUT_FILENO)`，仅用于行刷新决策）不属于 File-resource census——它们不是本 app 打开的 File-data resource，也不是 namespace 操作，不触碰 File contract 的任何责任。

---

## 3. sluice-hash

| Location | Escape | Needed semantic | Classification | Action | Owner |
| --- | --- | --- | --- | --- | --- |
| `apps/sluice-hash/main.cpp:50` | raw `::open(O_RDONLY)` + FdCloser/`::close` | File lifetime / open-close authority | CANONICAL_FILE_USE | MIGRATED：`File::open` + move into `HashInput`；`FdCloser` 删除，File dtor 拥有 close | A2 |
| `apps/sluice-hash/hash_task.hpp:24` | `HashInput::fd`（raw fd 作为 task 世界资源身份） | resource identity | CANONICAL_FILE_USE | MIGRATED：`HashInput::file`（`sluice::File`） | A2 |
| `apps/sluice-hash/hash_task.cpp:51` | `await_read_once(ctx, fd, ...)` | positional read | CANONICAL_FILE_USE | MIGRATED：`await_read_at(in.file, ctx, offset, ...)` | A2 |
| `apps/sluice-hash/main.cpp:56` | `::fstat(file.native_handle())` + `S_ISREG` | regular-file check | REQUIRED_INTEROP | KEEP | — |

REQUIRED_INTEROP 理由：file kind / regular-file classification 是 ADR-0002 §4.3 允许的 minimal metadata，但 canonical `File` 尚未暴露该 observation，且没有任何 roadmap node（#343–#347）拥有 metadata surface；这是当前 File contract 无法表达的必要 OS observation，只能经 `native_handle()`。

## 4. sluice-grep

| Location | Escape | Needed semantic | Classification | Action | Owner |
| --- | --- | --- | --- | --- | --- |
| `apps/sluice-grep/main.cpp:47` | raw `::open(O_RDONLY)` + FdCloser/`::close` | File lifetime | CANONICAL_FILE_USE | MIGRATED：`File::open`；`FdCloser` 删除 | A2 |
| `apps/sluice-grep/grep_task.hpp:28` | `GrepInput::fd` | resource identity | CANONICAL_FILE_USE | MIGRATED：`GrepInput::file` | A2 |
| `apps/sluice-grep/grep_task.cpp:53` | `await_read_once(ctx, fd, ...)` | positional read | CANONICAL_FILE_USE | MIGRATED：`await_read_at(in.file, ...)` | A2 |
| `apps/sluice-grep/main.cpp:53` | `::fstat(file.native_handle())` + `S_ISREG` | regular-file check | REQUIRED_INTEROP | KEEP | — |

grep 的 fstat interop 理由与 §3 hash 相同：file kind / regular-file classification 是 ADR-0002 §4.3 允许的 minimal metadata，但 canonical `File` 尚未暴露该 observation，且没有任何 roadmap node（#343–#347）拥有 metadata surface。

grep main 的 `::isatty(STDOUT_FILENO)` 是 census exclusion（§2），不进入 escape 表。

hash 与 grep 的迁移判断分别独立验证：两者 read pattern 均为 one-outstanding → await → advance（无 pipeline / multiple-outstanding authority），因此已 earned 的 `File + await_read_at` 可完整表达，raw fd 仅是历史 spelling。

## 5. sluice-tail

Ownership 形状选择（task §8.4 的最小可证明形状）：**main 打开并 move 交给 TailEngine（Impl 稳定堆地址持有 `sluice::File`）；TailTask 持 `const File*` 借用**。未引入 shared_ptr / registry / runtime pin。

| Location | Escape | Needed semantic | Classification | Action | Owner |
| --- | --- | --- | --- | --- | --- |
| `apps/sluice-tail/main.cpp:47` | raw `::open` + 5 处手工 `::close` | File lifetime | CANONICAL_FILE_USE | MIGRATED：`File::open` → move into TailEngine；close 归 File dtor/close authority | A2 |
| `apps/sluice-tail/tail_task.hpp:51` | `TailEngine(int fd, ...)` raw fd 资源身份 | resource ownership | CANONICAL_FILE_USE | MIGRATED：`TailEngine(sluice::File, ...)` move-owns | A2 |
| `apps/sluice-tail/tail_task.cpp:114,129,146` | `await_read_once(ctx, fd, ...)`（主读 + 末字节 + 回扫） | positional read | CANONICAL_FILE_USE | MIGRATED：`await_read_at(*file, ...)` | A2 |
| `apps/sluice-tail/main.cpp:54` | `::fstat(file.native_handle())` + `S_ISREG` | regular-file check | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-tail/tail_task.cpp:183` | `::fstat(file->native_handle())` 取初始 size | size observation | CANONICAL_FILE_USE | MIGRATED：`blocking::size(*file)` | #343 / A3 |
| `apps/sluice-tail/tail_task.cpp:267` | `::fstat(file->native_handle())` follow 轮询 size / truncate 检测 | size / truncation observation | CANONICAL_FILE_USE | MIGRATED：`blocking::size(*file)` | #343 / A3 |

## 6. sluice-copy（resource ownership 与 operation reference 均已收敛，逐 concern 分类）

A6 之前的两条责任边界分离已解决：canonical 资源的 ownership 与 operation reference 都说 File；interop 资源经显式命名的机制引用表达。

```text
resource ownership / lifetime    canonical File（A2 已收敛 source）
explicit outstanding operation   canonical 资源经 implicit NativeFileRef 引用 File（A6 已收敛）；
                                 interop 资源（dst temp/raw fd）经显式命名 NativeFileRef{int}
```

Pipeline 拥有 multiple `PipelineSlot`、multiple `Completion`、multiple outstanding ReadOp/WriteOp、explicit submit/drain/cancellation——这是真实 explicit-outstanding authority（ADR-0002 §6.2），A2 不消除它。

| Location | Escape | Needed semantic | Classification | Action | Owner |
| --- | --- | --- | --- | --- | --- |
| `apps/sluice-copy/file_domain.cpp:37` | `File::open(src)` + source lifetime | src 打开 / lifetime | CANONICAL_FILE_USE | MIGRATED：source File move 入 `OpenCopyOutcome`；close 归 File authority | A2 |
| `apps/sluice-copy/safe_output.cpp:63` | `File::open(src)` + source lifetime | src 打开 / lifetime | CANONICAL_FILE_USE | MIGRATED：source File move 入 `SafeOpenOutcome` | A2 |
| `apps/sluice-copy/file_domain.cpp:44` | `::fstat(src_file.native_handle())` + `S_ISREG` | regular-file check | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:70` | `::fstat(src_file.native_handle())` + `S_ISREG` | regular-file check | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-copy/main.cpp:66,124` | `run_pipelined_copy` src 实参 | explicit-op resource reference（pipeline 边界） | CANONICAL_FILE_USE | MIGRATED：传 `*src_file`（canonical File 隐式转换为 `NativeFileRef`） | #346 / A6 |
| `apps/sluice-copy/main.cpp:66,124` | `NativeFileRef{oc.temp_fd}` / `NativeFileRef{oc.dst_fd}` dst 实参 | explicit-op resource reference（dst interop 资源） | REQUIRED_INTEROP | KEEP：dst 资源由 interop open 单元持有，机制引用经显式命名 `NativeFileRef{int}` | #346 / A6 |
| `apps/sluice-copy/copy_task.cpp:65` | `ReadOp{*src_file, ...}` 经 `ctx.submit_read`；multiple outstanding | explicit outstanding pipeline（src，canonical） | CANONICAL_FILE_USE | MIGRATED：ReadOp 以 canonical File 隐式引用资源，multiple-outstanding authority 不变 | #346 / A6 |
| `apps/sluice-copy/copy_task.cpp:76` | `WriteOp{dst, ...}`（dst = `NativeFileRef`）经 `ctx.submit_write`；multiple outstanding | explicit outstanding pipeline（dst，interop 资源） | REQUIRED_INTEROP | KEEP：dst 资源本身由 interop open 单元（O_NOFOLLOW/mkstemp）持有，无法无损表达为 canonical File（ADR-0002 §3.1 将 O_NOFOLLOW/mode 保持在 minimum open contract 之外）；机制级引用经显式命名的 `NativeFileRef{int}` 表达，不拥有语义权威 | #346 / A6 |
| `apps/sluice-copy/copy_task.cpp:108` | `await_read_fill`（src） | fill 组合（src，canonical） | CANONICAL_FILE_USE | MIGRATED：`await_read_fill(ctx, *src_file, ...)` | #346 / A6 |
| `apps/sluice-copy/copy_task.cpp:139` | `await_write_exact`（dst） | exact 组合（dst，interop 资源） | REQUIRED_INTEROP | KEEP：与 dst-side WriteOp 行同法——dst 资源由 interop open 单元持有，引用经显式命名 `NativeFileRef{int}` 表达，不拥有语义权威 | #346 / A6 |
| `apps/sluice-copy/copy_task.cpp:290` | `submit_sync_data(SyncDataOp{dst})` | durability (data) | REQUIRED_INTEROP | KEEP：dst 资源本身由 interop open 单元（O_NOFOLLOW/mkstemp）持有，无法无损表达为 canonical File（ADR-0002 §3.1 将 O_NOFOLLOW/mode 保持在 minimum open contract 之外）；机制级引用经显式命名的 `NativeFileRef{int}` 表达，不拥有语义权威 | #346 / A6 |
| `apps/sluice-copy/copy_task.cpp:297` | `submit_sync_all(SyncAllOp{dst})` | durability (all) | REQUIRED_INTEROP | KEEP：与 SyncData 行同法——dst 资源由 interop open 单元持有，机制级引用经显式命名的 `NativeFileRef{int}` 表达（canonical File-facing `await_sync_all` 已存在，适用于 canonical File 资源） | #346 / A6 |
| `apps/sluice-copy/main.cpp:118` | `::ftruncate(oc.dst_fd, 0)`（non-atomic 路径） | resize | REQUIRED_INTEROP | KEEP：dst 是 interop-owned raw fd（`O_NOFOLLOW` open），非 canonical File，`File::resize` 无法表达该资源引用；dst open unit 收敛属独立裁决，不在 A6 范围 | — |
| `apps/sluice-copy/file_domain.cpp:51` | `::open(dst, O_WRONLY\|O_CREAT\|O_NOFOLLOW\|O_CLOEXEC, 0644)` | dst 打开 | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-copy/file_domain.cpp:16` | `ScopedFd` dtor `::close`（仅 dst guard） | interop dst open 单元的 close authority | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-copy/main.cpp:26` | `ScopedFd` dtor `::close`（仅 dst guard） | interop dst open 单元的 close authority | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-copy/file_domain.cpp:58` | `::fstat(dst_fd)` + `S_ISREG` | regular-file check | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-copy/file_domain.cpp:65` | same-file identity（`st_dev`/`st_ino`） | same-file check | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:78`（same-file 比较在 `:81-82`） | `::stat(dst_path)` 预检（kind + same-file） | 目标 namespace 预检 | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:24` | `ScopedFd` dtor `::close`（temp/dir guard） | namespace 协议 fd 生命周期的 close 尾部 | OUT_OF_SCOPE_NAMESPACE_WORK | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:94` | `::mkstemp` | temp 文件创建 | OUT_OF_SCOPE_NAMESPACE_WORK | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:102` | `::fchmod(temp_fd, mode)` | 权限保留 | OUT_OF_SCOPE_NAMESPACE_WORK | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:103,156,165,196` | `::unlink`（失败清理） | namespace entry 删除 | OUT_OF_SCOPE_NAMESPACE_WORK | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:150,192` | `::close` temp fd | temp fd 释放 | OUT_OF_SCOPE_NAMESPACE_WORK | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:161` | `::rename(temp, dst)` | 原子替换 | OUT_OF_SCOPE_NAMESPACE_WORK | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:172` | `::open(dir, O_RDONLY\|O_DIRECTORY)` | 目录资源 | OUT_OF_SCOPE_NAMESPACE_WORK | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:57`（由 `:180` 经 `retry_on_eintr` 调用） | `::fsync(dir_fd)` | directory-entry durability | OUT_OF_SCOPE_NAMESPACE_WORK | KEEP | — |

理由说明：

- **source open/lifetime（CANONICAL_FILE_USE）**：`O_RDONLY | open_existing` 正是 `FileOpen` 默认轴（read_only/open_existing/preserve）的无损表达；`File::open` 与原 raw open 的唯一实现差异是附加 `O_CLOEXEC`，而四个 app 无 exec 行为，不构成 caller-visible 差异。source `ScopedFd` 已删除，outcome 内的 `std::optional<File>` 是 source 的唯一 owner；fstat 观察读取 `native_handle()`，pipeline 直接传 `File` 引用，不发生 fd release/dup。
- **dst open（REQUIRED_INTEROP）**：ADR-0002 §3.1 明确不把 `O_NOFOLLOW` 与 permission/mode surface 纳入 minimum open contract；canonical `FileOpen`（access/existence/contents 三轴）无法无损表达 `O_NOFOLLOW | O_CREAT | 0644`。强迁会丢失 security/behavior semantics。A6 据此将 dst-side 操作引用按显式命名的 `NativeFileRef{int}`（REQUIRED_INTEROP）收敛，open 单元本身保持 interop 分类。
- **source metadata（REQUIRED_INTEROP）**：file kind / regular-file classification / same-file identity 是 ADR-0002 §4.3 允许的 minimal metadata，但 canonical `File` 尚未暴露该 observation，#343–#347 均不拥有 metadata surface；只能经 `native_handle()` 观察。这正是 A2 的目标形状：File 保持 resource authority，native_handle escape 只出现在命名的观察边界。
- **pipeline operation reference（A6 后）**：src 是 canonical File 资源，ownership 与 operation reference 现在都说 File——`ReadOp{*src_file, ...}` 经 implicit `NativeFileRef` 转换引用 canonical 资源。dst 资源本身由 interop open 单元（`O_NOFOLLOW`/`mkstemp`）持有，无法无损表达为 canonical File，其操作引用经显式命名的 `NativeFileRef{int}` 表达：机制级值拷贝，不拥有所有权、lifetime 或 close authority（caller-borne lifetime 不变，ADR-0002 §7.2/§7.3）。
- **SyncData/SyncAll（dst interop）**：`await_sync_data(File, ...)` / `await_sync_all(File, ...)` 适用于 canonical File 资源；pipeline 的 durability op 以 dst 的 interop 资源为对象，故按 REQUIRED_INTEROP 经显式命名 `NativeFileRef{int}` 提交。
- **mkstemp/fchmod/rename/unlink/dir fsync（OUT_OF_SCOPE_NAMESPACE_WORK）**：ADR-0002 §5.4.5 明确 directory-entry durability 不属于 SyncData/SyncAll guarantee；§13 将 directory resource、rename/remove 保持未决；§4.3 将 permissions 排除在自动 Core 地位之外。这些是 atomic-replace namespace 协议的一部分，不属于 canonical File-data resource contract。source File 不参与 commit/discard 语义：`commit_atomic_copy`/`discard_atomic_copy` 只触碰 `temp_fd`/`temp_path`/`dst_dir`。

## 7. File lifetime proof（迁移后）

```text
hash / grep:
  File::open (main)
    → move into HashInput/GrepInput（vector，move-only）
    → move into HashTask/GrepTask（run_task_to_result 以引用捕获 task）
    → 每文件顺序 one-outstanding：await_read_at 观察到 terminal completion 才进入下一 read
    → task publish → slot.wait_and_take 返回 → request_stop → drain → join
    → task 析构（File 关闭）
  无 shared ownership；File 不会在 operation outstanding 期间析构。

tail:
  File::open (main)
    → move into TailEngine::Impl（unique_ptr 稳定堆地址）
    → start() 时 TailTask 持 &impl_->file 借用；lambda 由 Impl 拥有的 runtime 持有
    → 所有 read await 至 terminal；follow 循环 cancel 后 publish
    → wait(): slot.wait → request_stop → drain → join → 返回
    → TailEngine dtor（main 作用域结束）→ Impl dtor → File 关闭
  cancel 路径：request_stop → waiter wake → task publish → drain/join 完成后 wait 才返回；
  File 关闭时序与迁移前基线一致（基线也是在 wait 返回后才 ::close）。
  failure-before-start / submit-failure：无 outstanding operation，File 由 engine dtor 关闭。

copy（A6 后）:
  File::open（open_copy_files / open_atomic_copy）
    → move into OpenCopyOutcome / SafeOpenOutcome（source 唯一 owner 是 outcome 内的 File；
      失败路径的每个 early return 由 File dtor 关闭 source，恰好一次）
    → main 在 fstat 观察边界读取 native_handle()；pipeline 边界直接传 `*src_file`
      （canonical File 隐式转换）与 `NativeFileRef{dst_fd}`（显式命名机制引用）
    → run_pipelined_copy 内所有 slot op 由 await_drain 收敛至 terminal 后函数返回
    → main 作用域结束，outcome 析构 → source File 关闭
  atomic commit/discard 只触碰 temp_fd/temp_path/dst_dir，不含 source File；
  source 在 rename/dir-fsync 阶段仍是 outcome 内的普通 owning input state。
  无 shared ownership、无 release/dup；File 不会在任何 ReadOp 可触达其 handle 期间析构；
  NativeFileRef 不延长/不拥有 lifetime，caller-borne lifetime contract 不变。
```

## 8. Census completeness gate

对迁移后代码重跑 §2 机械扫描：

```text
sluice-hash:  raw ::open/::close/fd/await_read_once = 0；残余 = fstat interop（已分类）
sluice-grep:  raw ::open/::close/fd/await_read_once = 0；残余 = fstat interop（已分类）；
              isatty = census exclusion（§2）
sluice-tail:  raw ::open/::close/fd/await_read_once = 0；残余 = fstat interop（已分类）；
              size/truncation 观测已迁至 `blocking::size`（#343 / A3）
sluice-copy:  raw source open/ScopedFd = 0（RAW_MIGRATABLE_SOURCE_OWNERSHIP_COUNT = 0）；
              source lifetime = canonical File；
              src-side explicit-op reference = canonical File（implicit NativeFileRef，
              PIPELINE_RAW_OP_REFERENCE_COUNT = 0）；
              残余 = src/dst metadata interop + dst special open 与 dst-side 机制级引用
              （REQUIRED_INTEROP，显式命名 `NativeFileRef{int}`）
              + namespace 协议（OUT_OF_SCOPE_NAMESPACE_WORK）（均已分类）
```

```text
UNCLASSIFIED_ESCAPE_COUNT = 0
APP_ESCAPE_CENSUS_COMPLETE
```

## 9. 行为保持证据

四个 app 在 BASE（`795f3896`，迁移前）与迁移后 commit 上对同一 fixture 矩阵（37 用例：hash/grep/tail/copy 的正常、错误、non-regular、missing、exit code、stderr 类别、原子/非原子 copy、mode 保留、tail follow+truncate+SIGINT 会话）产出 **byte-identical** 输出（canonical 路径归一后 diff 为空）。

copy source-ownership corrective（`6aa148a3` → `5a62be39`）复跑 25 用例 copy-only differential 矩阵（normal/missing/FIFO/symlink/non-regular/same-file/atomic/non-atomic/mode 保留/dst 为 symlink/权限拒绝/empty/sync data/sync all/depth 4），stdout、stderr、exit code、结果文件树、内容与 mode 全部 **byte-identical**（argv[0] 归一后）。FIFO source 在两侧都保持阻塞 open 语义（`File::open` 不引入 `O_NONBLOCK`）。

持久测试：`tests/app_hash_consumption_test.cpp`、`tests/app_grep_consumption_test.cpp`、`tests/app_tail_consumption_test.cpp`、`tests/app_copy_consumption_test.cpp` 通过 canonical File surface 驱动四个迁移后的 engine（digest/order/error 路径、匹配与顺序、last-lines/follow/stop/truncation 生命周期；copy 的非原子/原子 roundtrip、outcome move 所有权转移、missing/directory source、same-file、destination open 失败、discard temp 清理）。
