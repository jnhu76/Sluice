# Explicit File App Consumer Census

- **Authority**: [`docs/mission.md`](../mission.md) → [`ADR-0001`](../adr/0001-explicit-io-design-doctrine.md) → [`ADR-0002`](../adr/0002-explicit-file-api-architecture.md)
- **Verified implementation baseline**: `a16d2880f1d778d130d98addd6a63c6a63961f05`（该 commit 可 checkout 并验证下文记录的消费现实）
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
| `apps/sluice-grep/main.cpp:65` | `::isatty(STDOUT_FILENO)` | stdout 行刷新决策 | OUT_OF_SCOPE_NAMESPACE_WORK | KEEP | — |

grep 的 fstat interop 理由与 §3 hash 相同：file kind / regular-file classification 是 ADR-0002 §4.3 允许的 minimal metadata，但 canonical `File` 尚未暴露该 observation，且没有任何 roadmap node（#343–#347）拥有 metadata surface。

isatty 说明：stdout 是继承的 console handle，不是本 app 打开的 File-data resource；该调用不触碰 File contract 的任何责任，永久属于 File 架构之外（记录以保持 census 完整）。

hash 与 grep 的迁移判断分别独立验证：两者 read pattern 均为 one-outstanding → await → advance（无 pipeline / multiple-outstanding authority），因此已 earned 的 `File + await_read_at` 可完整表达，raw fd 仅是历史 spelling。

## 5. sluice-tail

Ownership 形状选择（task §8.4 的最小可证明形状）：**main 打开并 move 交给 TailEngine（Impl 稳定堆地址持有 `sluice::File`）；TailTask 持 `const File*` 借用**。未引入 shared_ptr / registry / runtime pin。

| Location | Escape | Needed semantic | Classification | Action | Owner |
| --- | --- | --- | --- | --- | --- |
| `apps/sluice-tail/main.cpp:47` | raw `::open` + 5 处手工 `::close` | File lifetime | CANONICAL_FILE_USE | MIGRATED：`File::open` → move into TailEngine；close 归 File dtor/close authority | A2 |
| `apps/sluice-tail/tail_task.hpp:51` | `TailEngine(int fd, ...)` raw fd 资源身份 | resource ownership | CANONICAL_FILE_USE | MIGRATED：`TailEngine(sluice::File, ...)` move-owns | A2 |
| `apps/sluice-tail/tail_task.cpp:114,129,146` | `await_read_once(ctx, fd, ...)`（主读 + 末字节 + 回扫） | positional read | CANONICAL_FILE_USE | MIGRATED：`await_read_at(*file, ...)` | A2 |
| `apps/sluice-tail/main.cpp:54` | `::fstat(file.native_handle())` + `S_ISREG` | regular-file check | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-tail/tail_task.cpp:184` | `::fstat(file->native_handle())` 取初始 size | size observation | TEMPORARY_CONVERGENCE_GAP | KEEP（经 `native_handle()`） | #343 / A3（`File::size`） |
| `apps/sluice-tail/tail_task.cpp:268` | `::fstat(file->native_handle())` follow 轮询 size / truncate 检测 | size / truncation observation | TEMPORARY_CONVERGENCE_GAP | KEEP（经 `native_handle()`） | #343 / A3（`File::size`） |

## 6. sluice-copy（代码不变，逐 concern 分类）

Pipeline 拥有 multiple `PipelineSlot`、multiple `Completion`、multiple outstanding ReadOp/WriteOp、explicit submit/drain/cancellation——这是真实 explicit-outstanding authority（ADR-0002 §6.2），A2 不消除它。

| Location | Escape | Needed semantic | Classification | Action | Owner |
| --- | --- | --- | --- | --- | --- |
| `apps/sluice-copy/copy_task.cpp:64,75` | `ReadOp{src_fd}` / `WriteOp{dst_fd}` 经 `ctx.submit_read/submit_write`；multiple outstanding | explicit outstanding pipeline | TEMPORARY_CONVERGENCE_GAP | KEEP（不迁；低层 operation 仍以 raw fd 引用资源） | #346 / A6 |
| `apps/sluice-copy/copy_task.cpp:107,137` | `await_read_fill` / `await_write_exact`（raw fd） | fill/exact 组合 | TEMPORARY_CONVERGENCE_GAP | KEEP | #346 / A6 |
| `apps/sluice-copy/copy_task.cpp:288` | `submit_sync_data(SyncDataOp{dst_fd})` | durability (data) | TEMPORARY_CONVERGENCE_GAP | KEEP | #346 / A6 |
| `apps/sluice-copy/copy_task.cpp:295` | `submit_sync_all(SyncAllOp{dst_fd})` | durability (all) | TEMPORARY_CONVERGENCE_GAP | KEEP | #345 / A5 |
| `apps/sluice-copy/main.cpp:120` | `::ftruncate(oc.dst_fd, 0)`（non-atomic 路径） | resize | TEMPORARY_CONVERGENCE_GAP | KEEP | #343 / A3（`File::resize`） |
| `apps/sluice-copy/file_domain.cpp:36` | `::open(src, O_RDONLY)` | src 打开 | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-copy/file_domain.cpp:50` | `::open(dst, O_WRONLY\|O_CREAT\|O_NOFOLLOW\|O_CLOEXEC, 0644)` | dst 打开 | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-copy/file_domain.cpp:20` | `ScopedFd` dtor `::close`（src/dst guard） | interop open 单元的 close authority | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-copy/main.cpp:31` | `ScopedFd` dtor `::close`（non-atomic src/dst guard） | interop open 单元的 close authority | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:28` | `ScopedFd` dtor `::close`（src/temp/dir guard） | interop / namespace fd 的 close authority | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-copy/file_domain.cpp:43,57` | `::fstat` src/dst + `S_ISREG` | regular-file check | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-copy/file_domain.cpp:64` | same-file identity（`st_dev`/`st_ino`） | same-file check | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:62,69` | atomic 路径 src `::open` + `::fstat` | src 打开 / kind check | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:77`（same-file 比较在 `:81-82`） | `::stat(dst_path)` 预检（kind + same-file） | 目标 namespace 预检 | REQUIRED_INTEROP | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:93` | `::mkstemp` | temp 文件创建 | OUT_OF_SCOPE_NAMESPACE_WORK | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:101` | `::fchmod(temp_fd, mode)` | 权限保留 | OUT_OF_SCOPE_NAMESPACE_WORK | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:102,156,165,196` | `::unlink`（失败清理） | namespace entry 删除 | OUT_OF_SCOPE_NAMESPACE_WORK | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:150,192` | `::close` temp fd | temp fd 释放 | OUT_OF_SCOPE_NAMESPACE_WORK | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:161` | `::rename(temp, dst)` | 原子替换 | OUT_OF_SCOPE_NAMESPACE_WORK | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:172` | `::open(dir, O_RDONLY\|O_DIRECTORY)` | 目录资源 | OUT_OF_SCOPE_NAMESPACE_WORK | KEEP | — |
| `apps/sluice-copy/safe_output.cpp:56`（由 `:180` 经 `retry_on_eintr` 调用） | `::fsync(dir_fd)` | directory-entry durability | OUT_OF_SCOPE_NAMESPACE_WORK | KEEP | — |

ScopedFd `::close` 三行说明：这三个 dtor 是 raw `::open` 单元（REQUIRED_INTEROP）与 namespace 协议 fd 的 close authority；它们服务的 fd 从未参与 canonical File operation。guard 包住 namespace 协议 fd（temp/dir）时，该 close 是对应 OUT_OF_SCOPE_NAMESPACE_WORK 生命周期的尾部，不引入 File-contract 含义；按出现位置归入 REQUIRED_INTEROP open 单元统一记录。

理由说明：

- **dst open / src open（REQUIRED_INTEROP）**：ADR-0002 §3.1 明确不把 `O_NOFOLLOW` 与 permission/mode surface 纳入 minimum open contract；canonical `FileOpen`（access/existence/contents 三轴）无法无损表达 `O_NOFOLLOW | O_CREAT | 0644` 与 copy 的 same-file 语义。强迁会丢失 security/behavior semantics。src/dst 是一个 open 判定单元（same-file 检查需要两者），因此作为整体保持 REQUIRED_INTEROP；A6 收敛 pipeline 时是重新审视该 open 单元的自然位置。
- **same-file identity（REQUIRED_INTEROP）**：ADR-0002 §4.3 将 same-file identity 列为允许赚取的 minimal metadata，但当前 canonical surface 未提供，#343–#347 均不拥有 metadata surface；这是必要 OS observation，不是可归入未来已排期 node 的 gap。
- **SyncData（KEEP with pipeline → A6）**：`await_sync_data(File, ...)` 已存在，但整个 pipeline 仍基于 explicit outstanding raw operation resource；单独把 SyncData 换成 File adapter 不改善 authority，反而制造半套 resource model。
- **mkstemp/fchmod/rename/unlink/dir fsync（OUT_OF_SCOPE_NAMESPACE_WORK）**：ADR-0002 §5.4.5 明确 directory-entry durability 不属于 SyncData/SyncAll guarantee；§13 将 directory resource、rename/remove 保持未决；§4.3 将 permissions 排除在自动 Core 地位之外。这些是 atomic-replace namespace 协议的一部分，不属于 canonical File-data resource contract。

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

copy（不变）:
  ScopedFd (main) 持有 src/dst → run_pipelined_copy 内所有 slot op 由 await_drain 收敛至 terminal
  → 函数返回后 main 释放。A6 前保持现状。
```

## 8. Census completeness gate

对迁移后代码重跑 §2 机械扫描：

```text
sluice-hash:  raw ::open/::close/fd/await_read_once = 0；残余 = fstat interop（已分类）
sluice-grep:  raw ::open/::close/fd/await_read_once = 0；残余 = fstat interop + isatty（已分类）
sluice-tail:  raw ::open/::close/fd/await_read_once = 0；残余 = fstat interop + size/truncation gap（已分类）
sluice-copy:  代码零改动；全部 escape 逐 concern 分类（§6）
```

```text
UNCLASSIFIED_ESCAPE_COUNT = 0
APP_ESCAPE_CENSUS_COMPLETE
```

## 9. 行为保持证据

四个 app 在 BASE（`795f3896`，迁移前）与迁移后 commit 上对同一 fixture 矩阵（37 用例：hash/grep/tail/copy 的正常、错误、non-regular、missing、exit code、stderr 类别、原子/非原子 copy、mode 保留、tail follow+truncate+SIGINT 会话）产出 **byte-identical** 输出（canonical 路径归一后 diff 为空）。

持久测试：`tests/app_hash_consumption_test.cpp`、`tests/app_grep_consumption_test.cpp`、`tests/app_tail_consumption_test.cpp` 通过 canonical File surface 驱动三个迁移后的 engine（digest/order/error 路径、匹配与顺序、last-lines/follow/stop/truncation 生命周期）。
