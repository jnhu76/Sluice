# Explicit File Vectored Operation Decision

- **Authority**: [`docs/mission.md`](../mission.md) → [`ADR-0001`](../adr/0001-explicit-io-design-doctrine.md) → [`ADR-0002`](../adr/0002-explicit-file-api-architecture.md)（vectored 的 evidence-gated 地位由 ADR-0002 §5.3 冻结）
- **Verified against**: `d9ae9fde0e78bd626ff708533b57f995a7ca984c`
- **Non-authority rule**: 本文记录一个基于 fresh census 的 disposition，不重新定义架构、不新增 canonical API；若本文与 ADR 冲突，以 ADR 为准。verdict 词汇使用 ADR-0002 §14 的 `KEEP / CONVERGE / ADD_MINIMAL / DELETE / RESEARCH / OUT_OF_SCOPE`。

---

## 1. Decision

**Vectored 不作为 required Phase A canonical File semantic surface 携带。**

- canonical File-facing vectored API 为零：无 `blocking::read_vec/write_vec`，无 `await_*_vec`。
- 不做 async parity 工作（sync vec 存在不构成 async parity 义务；ADR-0002 §5.3 的 parity 要求仅在 vectored 被证明必须 canonical 之后触发）。
- legacy / composition 层 vectored surface 暂时 KEEP；其 DELETE 与否归 future legacy-surface audit（即 ADR-0002 §13 所指 master-based architecture-gap audit 的 legacy 部分；tracking issue = #355）裁决定夺（见 §5）。

架构满足恰恰以**不携带**一个未被 earn 的 surface 达成：ADR-0002 §5.3 将 vectored 冻结为 evidence-gated extension，本决策即是该 gate 的第一次裁决——extension 保持 gated。

## 2. Fresh census（全部命令在本决策 commit 上重跑，非引用旧数据）

| # | 问题 | 命令 | 结果 |
| --- | --- | --- | --- |
| C1 | 四个 app 的 vectored 使用 | `grep -rEn 'read_vec\|write_vec\|read_vec_at\|write_vec_at\|IoSlice\|ConstIoSlice\|readv\|writev\|preadv\|pwritev' apps/sluice-copy apps/sluice-hash apps/sluice-grep apps/sluice-tail` | **0 matches**（33 个文件全部扫描） |
| C2 | WAL 的 vec 调用点 | `grep -n 'write_all_vec' src/wal.cpp` | 1 hit：`src/wal.cpp:105`，位于 `write_record_vec`（header\|payload\|trailer 3-slice scatter） |
| C3 | `sluice/wal.hpp` 的 consumer | `grep -rn 'sluice/wal.hpp' include/ src/ apps/ tests/ --include='*.cpp' --include='*.hpp'` | 1 hit = `src/wal.cpp:1`（自身）；**external consumer = 0** |
| C4 | WAL record 函数在 wal 之外的 caller | `grep -rn 'write_record\|read_record' src/ include/ apps/ tests/`（排除 wal 自身文件） | 0（连 tests 也无） |
| C5 | `IoSlice/ConstIoSlice` 引用文件 | `grep -rl 'IoSlice\|ConstIoSlice' include/ src/ apps/ tests/ --include='*.cpp' --include='*.hpp'` | 10 files：`iovec/reader/writer/observed/file` 的 hpp+cpp + `wal.cpp`；apps/tests = 0 |
| C6 | canonical blocking File vec | `grep -rn 'read_vec\|write_vec' include/sluice/blocking/` | 0；`blocking::read/write/read_at/write_at/sync_data/size/resize/sync_all` 全 scalar |
| C7 | async File-facing vec | `grep -n 'await_' include/sluice/async/file.hpp` | surface = `await_read_at / await_write_at / await_sync_data / await_sync_all`，无 `await_*_vec` |
| C8 | threadpool backend syscalls | `grep -n '::pread\|::pwrite\|fdatasync\|::fsync' src/async/threadpool_backend.cpp` | `::pread`(:403) / `::pwrite`(:412) / `::fdatasync`(:420) / `::fsync`(:426)，scalar-only |
| C9 | uring opcodes | `grep -n 'io_uring_prep' src/async/uring_backend.cpp` | `prep_read`(:589) / `prep_write`(:593) / `prep_fsync`(:597,600) / `prep_cancel64`(:1373)；**无 READV/WRITEV opcode** |
| C10 | `SLUICE_HAS_LIBURING` 是否进构建 | `grep -rn 'SLUICE_HAS_LIBURING' xmake.lua xmake/` | **0 hits**（仅存在于 src/include 的 `#if` guard）→ uring 在本 repo 构建为 honest stub |

> **Historical scope note**: C1–C10 是 `d9ae9fde0e78bd626ff708533b57f995a7ca984c` 上的 decision-time census。其后 #359 为 `--liburing=y` 构建接入了真实 liburing path；这不改写 C10 的历史事实，也不改变本决策，因为 backend capability 本身不授予 vectored canonical semantic authority。

Composition / legacy 层现存 vectored surface（全部保留现状，本决策不触碰）：

```text
Reader::read_vec            默认实现（src/reader.cpp:58）：逐 slice read_some fallback，
                            短读即停并返回跨 buffer 累计推进量
Reader::read_vec_all        默认实现（src/reader.cpp:81）：循环填满全部 slice
Writer::write_vec           默认实现（src/writer.cpp:26）：逐 slice write_some fallback，短写 break
Writer::write_all_vec       默认实现（src/writer.cpp:48）：head-offset 重提交 remainder，zero-progress → invalid_state
ObservedReader/Writer vec   src/observed.cpp:20,54：vec 包装 + VectorStats fallback 计数
FileReader::read_vec/at     src/file.cpp:114,212 → ::readv / ::preadv
FileWriter::write_vec/at    src/file.cpp:357,458 → ::writev / ::pwritev
VectorStats                 include/sluice/measurement.hpp:72-80
IoSlice / ConstIoSlice      include/sluice/iovec.hpp
```

## 3. 已存在的语义负担（值得记录的现状）

```text
multi-buffer single-syscall        legacy File vec 一次系统调用携带多个 caller buffer
                                   （每次调用 ≤ iov_max 个 iovec）
cross-buffer short-I/O advancement  短读/短写可推进多个 buffer；返回值即确切推进位置
IOV_MAX chunking                   iov_max() = IOV_MAX 宏或 sysconf(_SC_IOV_MAX)（fallback 16）；
                                   超限时循环分块提交，语义仍是跨 buffer 推进而非失败
buffer-lifetime surface            每个 op 同时引用多个 caller-owned buffer；
                                   若进入 async，悬挂点上的多 buffer 生命周期是真实的正确性问题
```

## 4. Two-sided adversarial analysis

### Case AGAINST（vectored 未被 earn 为 canonical Phase A File surface）

1. **Zero app demand**：C1 = 0。四个 app 的全部 read 均为单 buffer scalar positional read（copy 为多 slot scalar 流水线，但每个 op 仍是 scalar positional）；没有任何 consumer 需要 multi-buffer canonical I/O。
2. **ADR-0002 §5.3 gate**：vectored 不享有 sequential / positional / durability 的 architecture authority；public surface 的 vectored 形态必须凭真实 consumer / semantic 证据独立赚取。
3. **消费链根不成立**：唯一 in-tree 消费链（C2）根植于 `WAL`——它 external consumer 为零（C3/C4），且 WAL 自身命运（Core / consumer / 移出）是 ADR-0002 §13 的 open audit 问题。一个命运未决模块的消费链不能反过来为 canonical surface 作证。
4. **backend capability ≠ semantic authority**：内核有 readv/writev、uring 有 READV/WRITEV opcode，都不等于 public API 必须表达它（ADR-0001 §3；conformance §2.1 边界 2）。在本决策 pinned baseline 中，repo 构建里的 uring 还是 honest stub（C10）；后续 #359 接入 real liburing path 只改变 execution capability，不改变本条 authority 判断。
5. **sync-API-exists ≠ async-parity-required**：legacy sync vec 的存在不构成 async parity 义务；parity 要求只在 vectored 被证明必须 canonical 之后触发（ADR-0002 §5.3）。
6. **Minimal mechanism**：canonical blocking 与 async File surface 当前全 scalar 且无 unclassified 漂移（C5–C7）；加 vec 是扩张而非收敛（ADR-0001 §6 minimum mechanism）。

### Case FOR（vectored 应被 earn）

1. WAL 的 scatter-write（header|payload|trailer 单次系统调用）是一个真实存在的 composition shape，不是假想需求。
2. legacy blocking surface 已实现完整语义负担（跨 buffer 推进、IOV_MAX chunking、VectorStats 观测）——但该 surface 当前 in-tree 零测试覆盖（见 §5，属 legacy audit 的输入事实）。
3. `Reader`/`Writer` interface 已有 vec methods 且带 default fallback impl——接口形状已经统一，提升为 canonical 表面"成本很低"。
4. 若 DELETE legacy vec，`write_record_vec` 需要重写为多次 `write_all`（或接受 per-record 拆分系统调用），似乎构成删除阻力。

### Verdict

关于 census 的 "measured/use-case value" 一项：本决策以 use-case 半边满足之；按 ADR-0001 §8，performance 与 semantic authority 相互独立，且 ADR-0001 §6/§10 禁止 microbenchmark 效应直接进入 public surface，故性能测量不能也不应作为 canonical 地位的依据（性能归 Phase B）。

**AGAINST 胜。** Case FOR 的每一条事实都只证明"composition/legacy 层已有机制"，没有一条证明"canonical File surface 需要它"。ADR-0001 §3（机制/能力/信息都不是 semantic authority）与 ADR-0002 §5.3（evidence-gated）决定了 consumer 证据是 canonical 地位的必要条件：census 显示真实 consumer 群（四个 app）为零使用，唯一 in-tree 链根植于 zero-consumer 且命运未决的 WAL。kernel/uring 能力与 legacy sync 机制的存在都不构成 authority；"删除阻力"属于 legacy-surface audit 的考量，不属于 canonical promotion 的证据。

按 §14 词汇：vectored 的 canonical Phase A promotion 未满足 `ADD_MINIMAL` 的证据门槛；同时不满足 `DELETE` 的条件（存在 in-tree 消费链且其根模块命运未决，zero-consumer-alone 不足以 DELETE，见 §5）。

## 5. Legacy surface disposition 与 reopen 条件

- **Disposition**：legacy vectored surface（`FileReader`/`FileWriter` vec methods、`Reader`/`Writer` vec defaults、`IoSlice`/`ConstIoSlice`、`VectorStats`（含 `io_context.hpp` factory options 的 `VectorStats*` 观测管线）、`Observed*` vec plumbing、`WalWriter::write_record_vec`——经 C2 追踪，根植于 zero-consumer WAL）＝ **KEEP**（interim，非 ADR-0002 §14 终局 verdict），位于 legacy/composition 层，现状不动。
- **边界**：该 KEEP 不使 vectored 成为 canonical File semantic surface；`FileReader`/`FileWriter` 整体的最终命运仍由 future legacy-surface audit（ADR-0002 §13 的 master-based architecture-gap audit legacy 部分；tracking issue = #355）裁决。
- **Gate 链**：legacy-surface audit 对 vec surface 的 verdict 必须以自身证据满足 ADR-0002 §14；且该分析被 WAL 自身的命运（同属 ADR-0002 §13 的 open audit）gate——先裁决 WAL，再裁决它的唯一 in-tree 消费链。
- **遗留事实**：上述 legacy vec surface 当前 in-tree 零测试覆盖（census 复核：tests/ 无任何 vec/IoSlice/VectorStats 引用）；该覆盖状态是 legacy-surface audit 必须采纳的输入事实。
- **本决策不做的事**：无 canonical File-facing vectored API、无 async parity、无代码删除、无 baseline pin 变更（docs-only）。
- **Reopen condition**：真实 consumer 需要 multi-buffer canonical I/O，且有证据 scalar composition 不足（指 correctness 需求无法表达为 per-op scalar 序列，如 O_APPEND/pipe 场景下多 buffer 记录的 interleaving/contiguity 完整性——注意 readv/writev 本身不提供跨 buffer 原子性；单纯的 syscall 计数或性能考量不构成"不足"，性能归 Phase B 实证）→ 开新的 narrow node。届时 async 与 sync 必须共享同一 vectored semantics（ADR-0002 §5.3），不允许长期 sync-vector / async-scalar 两套语义世界。
