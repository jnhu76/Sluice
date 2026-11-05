# R3 共享上下文（CONTEXT）

本文件是 R3 各角色（设计、实现、计时、分析）共用的冻结上下文。凡与本文件冲突的临时口述，以本文件为准；本文件只能由基线冻结员在冻结前修订，冻结后追加需注明时间与理由。

记录时间：2026-11-05
冻结状态：**PLATFORM PASS / BASELINE FROZEN**

---

## 1. 任务与路径

- 任务：在 E5 原生平台冻结 #595 的产品基线，建立与主工作树隔离的 R3 研究环境，为后续 A0/A1/B 与 A_R2/B_R2 对照计时提供唯一可信基线与证据根。
- 主工作树：`/home/jnhu/Source/Sluice`
- 隔离 worktree：`/home/jnhu/sluice-595-e5/r3/worktree`（分支 `research-595-r3`）
- 证据根：`/home/jnhu/sluice-595-e5/r3/evidence`
- 原始记录：`/home/jnhu/sluice-595-e5/r3/raw`
- 脚本与 harness：`/home/jnhu/sluice-595-e5/r3/harness`
- 日志：`/home/jnhu/sluice-595-e5/r3/logs`
- 测量锁：`/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`

## 2. 基线 SHA

- `baseSha = 0a22a79195f1b913145fcd443496e7f70b08cdcf`
- 与设计预期一致（`matchesExpected=true`）；主工作树冻结前后 `status --porcelain` 均为 0 行。
- 基线二进制摘要在 `BASELINE.md` §5 与 `raw/binary-sha256.txt`；worktree 内三次构建（两次为 clean 后重建）摘要一致，可复现。

## 3. R2 锚点

- `271b81bfc9d64eecc0266fe7066b9040525dd710`（`feat(uring): eliminate runtime modulo in bounded dispatch ring`）
- 相对 R2 锚点：**产品代码零变化**。区间内唯一提交 `0a22a791` 只新增 `docs/research/595-causal-r2/**`（2577 文件，+65557 行）。
- 逐子树对象哈希核验：`include` / `src` / `apps` / `xmake` / `tests` / `xmake.lua` 在两端 **IDENTICAL**（详见 `BASELINE.md` §3）。
- 推论：R2 的源码条件在 R3 完整保留；但**不得复用 R2 的计时数值**，必须使用本轮二进制的 R3 测量。

## 4. 宿主身份

- hostname `E5`；Intel Xeon E5-2666 v3 @ 2.90GHz；1 socket × 10 core × 2 thread = 20 logical；NUMA node0 = 0-19；L3 25 MiB。
- 原生 Linux：Fedora 44，kernel `7.2.11-200.fc44.x86_64`，非 WSL2。
- 工具链：g++ 16.2.1 20260819、xmake v3.1.2+HEAD.5d21e9d、python 3.14.8、liburing 2.13。
- 旋钮：governor = `schedutil`（20/20 CPU）、`intel_pstate` `no_turbo=0`、SMT active、`perf_event_paranoid=2`、`unprivileged_bpf_disabled=2`。
- `/tmp` = 独立 tmpfs 32 GiB（占用 1%）。
- 细节与测量风险条目见 `PLATFORM.md` §5。

## 5. 硬约束（不可协商）

1. **主工作树不变**：`/home/jnhu/Source/Sluice` 不得出现本任务产生的修改；任何时刻 `git status --porcelain` 必须为 0 行（`build/` 已被 `.gitignore` 忽略，但也不得依赖它承载基线产物）。
2. **公开 API 不变**：`include/` 下公开头与符号不得改动。
3. **v2-r1 契约不变**：`docs/explicit-io-contract.md` 及其派生约束不得改写。
4. **远端与 Issue 不变**：不 push、不改 Issue、不开 PR。
5. **R1/R2 原始证据不变**：`/home/jnhu/sluice-595-e5/evidence/**`、`docs/research/595-causal-r2/**`、`evidence/raw/phaseA/**` 等只读；不得重写、重命名或「修正」其数值。
6. **只在隔离 worktree 写代码**：所有源码/脚本改动只发生在 `/home/jnhu/sluice-595-e5/r3/worktree` 或其证据目录；不得写入其它 worktree（`archive-wt`、`causal-r2/worktree`、`causal-r2/worktree-timing`、`worktree`）。
7. **不跑无锁计时**：任何 install/rebuild 导致二进制 SHA 变化，之前的计时全部作废，需重跑 A/A 校准。

## 6. ReadyGroup 定义

**ReadyGroup**＝在首次提交前「已拥有」的一组独立读。其成员条件（全部满足）：

- 文件已拥有（file 已就绪，非待打开/待创建）；
- offset 与长度已确定；
- 已验证存在有效 buffer 可承载读结果；
- 该读所依赖的条件已确定（不依赖任何未来事件才能成立）。

**禁止的造组方式（违反任一条即该组无效，不构成 ReadyGroup）：**

- 通过**新增等待**制造「同时就绪」；
- **前移依赖工作**（把本属于后续的依赖提前完成）来凑组；
- **追加预取**（额外 preload/预读）把不属于本组的读塞进来。

判定要点：ReadyGroup 描述的是**首提交前既已独立就绪**的读集合，不是通过调度技巧构造出来的批次。若需要等待才能成组，则该组不是 ReadyGroup，必须如实记为「非 ReadyGroup」。

## 7. 臂定义

### 7.1 已在 R2 文档中冻结、可直接引用的语义

来源：`docs/research/595-causal-r2/CF0_THREE_ARM.md` §2（已核读）。

- **A = IMMEDIATE**：冻结基线形态，`bounded_progress_=false`，准入门在第一检查早退；dispatch 点立即 `submit_transport_locked()`。
- **B = BOUNDED_PROGRESS**：门链全跑（bounded→owner→payload→witness→cap），通过则 defer flush；R2 另设 **C = GATE_ONLY_IMMEDIATE**（门链全跑但强制 `return false`），仅存在于研究副本。
- 三臂共用同一二进制，由运行时环境变量选择，避免二进制布局混淆。

### 7.2 R3 标签（本轮引入，语义待冻结）

ask 指定 R3 使用 `A0` / `A1` / `B` 与 `A_R2` / `B_R2` 五类标签，但**本 ask 未给出 A0、A1、A_R2、B_R2 的干预定义**，仓库内也检索不到这些标识符的既有定义（对全仓 `ReadyGroup`、`A_R2`、`B_R2`、`A0`、`A1` 的检索均无 R3 相关命中）。因此：

| 标签 | 状态 | 说明 |
|---|---|---|
| `A0` | **语义未在本次冻结中给出** | 需要设计负责人在首个计时轮前写入本节 |
| `A1` | **语义未在本次冻结中给出** | 同上 |
| `B` | 可引用 R2 §2 的 BOUNDED_PROGRESS 语义 | 若 R3 对 B 有改定义，须显式覆盖并注明 |
| `A_R2` | **语义未在本次冻结中给出** | 推测为「R2 的 A」之对照臂，但**不得以推测代替定义** |
| `B_R2` | **语义未在本次冻结中给出** | 同上 |

> **OPEN-1（阻断计时）**：在上述四个标签的干预定义、以及它们与二进制/运行时开关的映射被写入本节并由设计负责人确认前，R3 **不得**进入正式计时。基线冻结员不代为定义，以免制造无法与实现核对的口径。

## 8. 测量协议（冻结参数）

- 配对单位：**24 对**（pair）每 phase。
- 每 invocation：`WARMUP=1`、`MEASURED=5`。
- 绑核：`taskset -c 2`。
- 存储：**warm tmpfs**（`/tmp`，32 GiB tmpfs）；每轮前记录 `df` 并预热工作集。
- 臂序：**AB/BA 交替**（消除顺序/漂移偏置）。
- 统计：**bootstrap 10k 次重采样，seed 5950**。
- A/A 噪声门：**5 / 3 / 3 pp**（cpu / tput / p99 半宽上限）。
- 负控：**单侧 5/5/5/3**。
- 每轮必须记录：governor、cpu2 `scaling_cur_freq` 采样、`df -h /tmp`、二进制 SHA、invocation 退出码。

### 8.1 平台修正项（不改变协议参数，但影响可执行性）

- **SMT 兄弟**：cpu2 的兄弟是 **cpu12**。`taskset -c 2` 不阻止其它进程占用 cpu12 共享物理核。计时轮必须同时确认 cpu12 空闲，或显式征用 `2,12`。此为本轮最大噪声风险。
- **governor = schedutil**：频率随负载浮动（冻结时 cpu2 约 1.2 GHz，max 3.5 GHz）。跨臂比较必须同状态并记录采样。
- **非 root 限制**：`perf_event_paranoid=2` 限制系统级 perf；`unprivileged_bpf_disabled=2` 使非特权 bpftrace 不可用。需要 root 的 tracing 手段须提前确认权限。

## 9. 共用锁路径

- `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`（已创建，0 字节）。
- 任何计时/构建/tracing 轮次开始前必须持有该锁；结束释放。
- 锁存在本身不代表有人持锁；持锁状态须由计时方显式写入运行记录（建议 `flock`，锁文件之外的辅助文件记录 pid/开始时间）。

## 10. 计时串行约束

**15 分钟以上的计时必须串行，且独占整台机器。** 具体：

- 同一时刻只允许一个 15 min+ 计时任务；
- 运行期间不得并行构建、不得并行其它 bench、不得进行大规模 IO；
- 运行前后各记录一次 load average、cpu2/cpu12 占用、`df -h /tmp`；
- 若因环境抢占导致失败，记录为环境失败并重跑，不得通过放宽阈值或删点规避。

## 11. 证据目录布局

```
/home/jnhu/sluice-595-e5/r3/
├── MEASUREMENT.lock          共用测量锁
├── evidence/                 结论性证据（本文件、PLATFORM.md、BASELINE.md、MANIFEST.sha256）
├── raw/                      原始命令输出（lscpu、toolchain、sysctls、df、mount、git、binary sha）
├── harness/                  R3 计时/分析脚本
└── logs/                     运行日志（构建、计时）
```

约定：
- `raw/` 只放原始输出，不做二次编辑；`evidence/` 只放可引用结论。
- 任何进入结论的数字必须能追溯到 `raw/` 或 `logs/` 的具体文件。
- 计时产物按 phase 分目录，命名包含 case、臂序、pair 号。

## 12. 交付物清单（本冻结轮）

| 交付物 | 路径 | 状态 |
|---|---|---|
| 平台证书 | `/home/jnhu/sluice-595-e5/r3/evidence/PLATFORM.md` | 已生成 |
| 基线冻结记录 | `/home/jnhu/sluice-595-e5/r3/evidence/BASELINE.md` | 已生成 |
| 共享上下文 | `/home/jnhu/sluice-595-e5/r3/evidence/CONTEXT.md` | 本文件 |
| 摘要清单 | `/home/jnhu/sluice-595-e5/r3/evidence/MANIFEST.sha256` | 已生成，`sha256sum -c` 通过 |
| 原始环境记录 | `/home/jnhu/sluice-595-e5/r3/raw/*.txt` | 已生成（7 个） |
| 构建日志 | `/home/jnhu/sluice-595-e5/r3/logs/build-baseline.log` | 已生成 |
| 隔离 worktree | `/home/jnhu/sluice-595-e5/r3/worktree` | 已创建（`research-595-r3` @ `0a22a791`） |
| 基线二进制 | worktree `build/linux/x86_64/release/` | 已构建（4 个 + 2 个静态库） |
| 测量锁 | `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock` | 已创建 |

## 13. 交接注意

1. 进入实现前先读 §5 硬约束与 §6 ReadyGroup 定义。
2. 进入计时前先解 §7.2 的 OPEN-1，并复查 §8.1 的平台修正项。
3. 二进制 SHA 若变化（任何 rebuild），此前计时作废，重跑 A/A 校准。
4. 本轮**未运行任何计时**，不存在可被误引用的 R3 性能数值。
