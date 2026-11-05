# #595 R3 续跑 — 冻结预注册（PREREGISTERED_R3_COMPLETION）

| 字段 | 值 |
|---|---|
| 文档 | `PREREGISTERED_R3_COMPLETION.md` |
| 角色 | #595 R3 续跑预注册员（只读上下文、只写本文件；不运行 benchmark、不看 A/B 结果） |
| completionRoot | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z` |
| baseSha | `0a22a79195f1b913145fcd443496e7f70b08cdcf` |
| R2 锚点 | `271b81bfc9d64eecc0266fe7066b9040525dd710` |
| 原型 patch | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/evidence/r3-completion.patch`<br>实测 sha256 `2596f204035f9695db47184b20ad8ece7b2168c54d8589b2060782aa7959bbc0` |
| R3 研究 worktree | `/home/jnhu/sluice-595-e5/r3/worktree`，HEAD `0a22a791…`，10 M + 5 ??；`git diff HEAD` sha256 `c1a31a30a9f02db99fac2ab2d0a8af028245a6fff0220314b8119c26e30d3003` |
| 冻结开始（`date -u`） | **2026-11-05T10:42:09Z**（本 ask 的第一条命令） |
| 写盘时刻（`date -u`） | **2026-11-05T10:47:31Z**（写入前最后一次 `date -u`） |
| `frozenBeforeTiming` | **true**（本文件写盘前未运行任何新增计时；见 §16 的命令清单） |
| `semanticsReady` | **false**（独立语义签字缺失；`A0_AND_SUFFIX_RECEIPT.md` 缺失；见 §0.2、§3.6） |
| 文档性质 | **续跑前瞻协议**；不是原 R3 预注册的事后补写。原 `PREREGISTERED_R3.md` 不存在（`test -e` MISSING；见 §2） |
| 门槛来源 | Issue #595 的冻结口径 + R1 `PREREGISTERED.md` 的更严值 + 本 ask 的本轮建议值；**不按任何结果修改** |
| 结论词汇 | 本文件只冻结协议与判据，不产生任何 CPU/时延/吞吐结论；空表不得写成 `PASS` |

---

## 0. 已读输入、实测核验与缺失输入

### 0.1 本 ask 指定输入的读取状态

| 输入 | 状态 | 本协议使用的部分 |
|---|---|---|
| `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/evidence/CONTEXT_COMPLETION.md` | **已读** | §1 基线、§5 五臂冻结定义、§6 证明义务索引、§7 原方案/续跑差异、§8 重验触发 |
| `…/evidence/OPEN1_RECEIPTS.md` | **已读** | §1 OPEN-1 缺口、§2 五臂定义、§3.1–§3.17 证明义务、§4 主形状/门差异、§5 patch/重验、§6 限制 |
| `…/evidence/BUILD_COMPLETION.md` | **已读** | §1 patch、§3 Context 入口/锁序、§6 批量约束、§7 报告、§8 二进制/smoke/strace、§9 OPEN-1 回执、§11 限制 |
| `…/evidence/A0_AND_SUFFIX_RECEIPT.md` | **MISSING — 无法读取** | 见 §3.6；未用推测替代 |
| `…/evidence/correctness/`（completion 树内） | **MISSING** | completion 的正确性回执只在 `BUILD_COMPLETION.md` §8.3/§9；见 §0.2 |
| `/home/jnhu/sluice-595-e5/r3/evidence/correctness/CORRECTNESS.md`（前轮） | **已读** | 前轮 BASE 13/13、3 变异、RESTORED GREEN；其 §6.3 自述未持锁，不能充当本轮回执 |
| `/home/jnhu/sluice-595-e5/r3/evidence/READY_EXPOSURE.md` | **已读** | §1 consumer 分类、§2.3 分布、§3 enter 基线、§10.3/§10.5/§10.6/§10.8 |
| `/home/jnhu/sluice-595-e5/r3/evidence/census.json` | **已读** | `meta.no_timing_or_ab_run=true`；d16/d4/d64 分布与节省、W-02 D2 单成员、60,004 enter |
| `/home/jnhu/sluice-595-e5/evidence/measurement/PREREGISTERED.md` | **已读** | R1 冻结配对/统计/门槛/资源门/单次修复条款 |
| `/home/jnhu/Source/Sluice/docs/research/595-causal-r2/PROTOCOL.md` | **已读** | R2 基线/补丁 sha/diff sha/二进制 sha/A-R2 与 B-R2 行为/历史数值（仅历史） |
| `bench/bench_w03_external_loop.cpp`（R3 worktree） | **已读** | `:321-323` A0 不构造 adapter；`:347-359` `max_refill`/`want`；`:409-431` A0 内联构造 + `ctx.submit_read`；`:552-559` case id；`:734-769` case 列表 |
| `bench/r3_ready_batch.hpp`（R3 worktree） | **已读** | `:26-29` arm selector；`:70-135` A0/A1/B 路径；`:120-134` suffix span=1；`:150-165` 计数 |
| `bench/bench_w02_pipeline.cpp`（R3 worktree） | **已读** | `:463-465` case id；`:644-660` case 列表；**无 D2 case、无 `SLUICE_BENCH_595_DEPTHS`** |
| `bench/bench_w02_pipeline.cpp`（R2 worktree） | **已读** | `:909-911` `SLUICE_BENCH_595_DEPTHS` opt-in 加入 D2 |

### 0.2 关键事实与缺失回执

1. **原 R3 预注册不存在。** `test -e /home/jnhu/sluice-595-e5/r3/evidence/PREREGISTERED_R3.md` →
   `MISSING`；前轮 `PROTOCOL.md:9` 与 `:197`（P-2）明确记录 `PREREGISTERED_R3.md` 与
   `measurement-{main,product,mechanism,analysis}/` 不存在，前轮所有 A/A、A/B、负控、
   资源门均为 `NOT_RUN`（`PROTOCOL.md:37-46`）。**本文件不能写成“原预注册早已存在”。**
2. **`A0_AND_SUFFIX_RECEIPT.md` 不存在。** `test -e …/evidence/A0_AND_SUFFIX_RECEIPT.md`
   → `MISSING`；`find /home/jnhu/sluice-595-e5 -maxdepth 5 -iname "*receipt*" -type f`
   没有该文件；completion `evidence/` 只有 `BASELINE_COMPLETION.md`、`BUILD_COMPLETION.md`、
   `CONTEXT_COMPLETION.md`、`MANIFEST_PRE.sha256`、`OPEN1_RECEIPTS.md`、`r3-completion.patch`。
   因此 A0 复原/suffix 的**独立回执不可核验**；本协议不把 `BUILD_COMPLETION.md` 的
   smoke/计数改写成该回执。
3. **completion 树内没有 `correctness/` 目录。** completion 的正确性证据是
   `BUILD_COMPLETION.md` §8.3（26 场景）与 §9；前轮 `r3/evidence/correctness/CORRECTNESS.md`
   针对前轮树，且其 §6.3 自述构建/测试**未持 `MEASUREMENT.lock`**（前轮 P-5 偏离），
   不能充当本轮锁定回执。
4. **`semanticsReady=false` 的依据。** `BUILD_COMPLETION.md:12` 的 `semanticsOk=true` 是
   **实现者验证结论**；§11.1 明确“不是独立 verifier 签字”；§11.3 未跑 TSan/ASan。
   `OPEN1_RECEIPTS.md` §3 的证明义务在 `BUILD_COMPLETION.md` §9 被实现者标为
   PROVED/PARTIAL（其中 OPEN-1.14 的资源门仍 `NOT_RUN`），但这些回执没有独立 verifier
   签字。因此本轮独立语义就绪度为 **false**；只有独立 verifier 回执（见 §14.3）才能翻为 true。
5. **BUILD_COMPLETION 自身的 hash 笔误。** §1 表头写 patch sha256
   `2596f204…`（与实际计算一致），但 §1 命令块注释写 `fc1857ce…`（stale）。
   本协议以实测 `2596f204…` 与 reverse-check 为准，并把该不一致登记为文档缺陷，
   不作为任何门槛放宽理由。

### 0.3 本 ask 内实际执行过的检查（只读，无计时）

| 检查 | 命令/结果 |
|---|---|
| 时间 | `date -u +"%Y-%m-%dT%H:%M:%SZ"` → `2026-11-05T10:42:09Z`；写前 → `2026-11-05T10:47:31Z` |
| patch 完整性 | `sha256sum r3-completion.patch` → `2596f204035f9695db47184b20ad8ece7b2168c54d8589b2060782aa7959bbc0` |
| patch post-image | `cd r3/worktree && git apply --reverse --check …/r3-completion.patch` → **exit 0** |
| patch 规模 | `git apply --stat …` → `15 files changed, 3999 insertions(+), 64 deletions(-)` |
| worktree 状态 | `git rev-parse HEAD` → `0a22a791…`；`git status --porcelain` → 15 行（10 M + 5 ??）；`git diff HEAD \| sha256sum` → `c1a31a30…` |
| R3 二进制 | `sha256sum build/linux/x86_64/release/{bench_w03_external_loop,bench_w02_pipeline,uring_ready_batch_deterministic_test,uring_ready_batch_research_test,uring_ready_batch_fault_test}` → 与 `BUILD_COMPLETION.md` §8.1 逐项一致（见 §3.2） |
| 锁 | `ls -la /home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock` → 0 字节；`fuser -v` 无输出（无持锁者） |
| 主树 | `git status --porcelain \| wc -l` → 0；`git rev-parse HEAD` → `0a22a791…` |
| R2 树 | HEAD `271b81bf…`；17 个脏项；`git diff HEAD \| sha256sum` → `f939602f…`；`git apply --reverse --check patch.diff` → **exit 1**；`bench_w02_pipeline` sha256 `66b2d0dd…`（≠ R2 PROTOCOL 的 `61c6d579…`）；`bench_w03_external_loop` `3ae513d0…`（= R2 PROTOCOL） |
| 工具/平台 | `strace`、`perf`、`valgrind`、`taskset`、`flock`、`cpupower` 存在；`heaptrack` MISSING；`perf_event_paranoid=2`；`unprivileged_bpf_disabled=2`；cpu2 governor `schedutil`，`scaling_cur_freq=1200000`，siblings `2,12`；`nproc=20`；`df -h /tmp` → 32G，3% 使用 |

**未执行**：任何 bench 计时、A/A、A/B、负控、strace/PMU/tracing 轮次、构建、正确性运行。
因此 `frozenBeforeTiming=true` 成立。

---

## 1. 冻结声明

1. 本文件在**第一秒新增计时之前**冻结。自本文件写盘起，下列臂、case、配对、门槛、统计、
   排除规则、目录与停手条件**不得因任何 A/B、A/A、负控或保护结果而修改**。
2. 唯一允许的测量条件变更是 §8 的**一次** A/A 修复（`taskset -c 2,12` 或 `WARMUP=2`，
   先记录后实施，双回执，门槛不变）。A/B、负控、保护、产品门没有任何修复额度。
3. 本文件是**续跑前瞻协议**：它不追溯证明原 R3 方案，也不把原 R3 的 `NOT_RUN` 项改写为
   已测；续跑通过不得记成原预注册通过（§2、§13）。
4. `semanticsReady=false` 是本轮的冻结输入。按 §14，当前只允许 R2 同 SHA 复现与安全
   IMMEDIATE 块；所有新 B/A1 块列入 `blockedBlocks`。翻为 true 只能由 §14.3 的独立
   verifier 回执触发，且翻转为 true 之前收集的任何 B 数据不得用于 B 门。

---

## 2. 原方案 vs 续跑方案（主形状/假设/主门差异，非追溯改写）

来源：`CONTEXT_COMPLETION.md` §7、`OPEN1_RECEIPTS.md` §4、前轮 `PROTOCOL.md:9`/`:197`（P-2）。

| 维度 | 原 R3（前轮） | 续跑 completion（本文件） | 差异性质 |
|---|---|---|---|
| 预注册 | **不存在** `PREREGISTERED_R3.md`；前轮全部计时 `NOT_RUN` | 本文件，写于任何新增计时之前 | 前瞻补位；**不是**事后补写成“早已存在” |
| 主形状 | D2 两 ready（`W02-uring-4k-d2`）；主产品形状 W-02 | **W-03 external loop d16** 为主形状 | 主形状迁移到有 `REAL_EXPOSURE` 的 W-03 |
| 产品门 | 名义 `B−A0` 产品净胜；W-02 无真实批暴露 | **#604 真实应用为产品门**；W-03 只是合成代表 trace；W-02 D2 回答“是否守住 IMMEDIATE” | 产品门从 benchmark 主 shape 改为真实应用准入 |
| A0 | adapter 路径，先构造完整 `ready` span，含共同准备开销（`VERDICT.md:98`） | baseSha 原始逐条循环；不构造 span/不预扫描/不分配组缓冲 | 控制臂修正 |
| A1 | `ctx.submit_read` 逐条，与 B 不同入口 | 与 B 同一 Context 内部研究入口，span=1；同 adapter/同输入 | 去除 B 的 context bypass 残差 |
| B | 直接 backend seam、cap=8、suffix 逐条；绕过 context | 同一 Context 入口整组调用；同一调用内 accept→prepare→dispatch→一次 flush；单成员等价 immediate；suffix 逐条 span=1 | 机制位置移入 Context |
| A_R2/B_R2 | 无定义 | `271b81bf + 7be9d014…` 的 A/B 两臂；W-02 D2；仅历史复现 | 新增同 SHA 旧机制对照 |
| 测量状态 | 全部 `NOT_RUN` | **本文件仍无任何计时数值**；只冻结协议 | 本轮不产生性能结论 |

**非追溯条款（写死）**：

1. 续跑在 `W03-uring-4k-d16` 上通过，**不等于**原 R3 在 `W02-uring-4k-d2` 或 W-02 主 shape
   上的问题通过；原 D2/W-02 问题保持 `NOT_RUN`/`UNANSWERED`，直到本文件对应块实际执行。
2. 续跑的任何结论必须带 `CONTINUATION_` 标签与 case id；不得写成 `ORIGINAL_R3_PASS`、
   `R3_PREREGISTERED_PASS` 或“issue #595 预注册门通过”。
3. 前轮 R2 历史数值（例如 D2 CPU +3.73%、p99 +7.07%，`docs/research/595-causal-r2/PROTOCOL.md:126`）
   只作历史对照，**不得复用**到本轮任何 CI/门槛/裁决。

---

## 3. 平台、二进制、fixture 与来源门

### 3.1 平台（冻结；每次 invocation 复记）

| 项 | 冻结值/记录方式 |
|---|---|
| 主机 | Intel Xeon E5-2666 v3，20 logical，SMT on；Fedora 44；kernel `7.2.11-200.fc44.x86_64` |
| cpu2 | `schedutil`；`scaling_cur_freq` 每次 invocation 前记录；siblings `2,12` |
| 权限 | `perf_event_paranoid=2`；`unprivileged_bpf_disabled=2`；不可用的 tracing 不得记为已测 |
| 存储 | `/tmp` = 32 GiB tmpfs；每次 invocation 前 `df -h /tmp` 记录 |
| 工具 | `strace`、`valgrind`、`taskset`、`flock`、`cpupower` 可用；`heaptrack` 不可用（记录为工具限制） |

### 3.2 R3 研究二进制（A0/A1/B 共用；同一 SHA）

实测（2026-11-05，`build/linux/x86_64/release/`）：

| 产物 | sha256 | 与 `BUILD_COMPLETION.md` §8.1 |
|---|---|---|
| `bench_w03_external_loop` | `0ebfb7f0c204579fe2cc9e8d7e88cee2de9112e25c0cfb11e8502e59fc4fe2d1` | 一致 |
| `bench_w02_pipeline` | `4e719b1e4dc2f0575812f4310b54e96c443f8ce8d1c6dd39c81bf5ec91351497` | 一致 |
| `uring_ready_batch_deterministic_test` | `c54a9222ed7121935b264417e6b7a65c2df6c8a2f943e98f71c01a602d59e700` | 一致 |
| `uring_ready_batch_research_test` | `1372b3468b8489367bfe0944bf3f537775cb8d4de9391165ae03120485770a31` | 一致 |
| `uring_ready_batch_fault_test` | `18395c0a570862147436ff11624eec7276ed1851546df8ebb4ad6590a4fe3bba` | 一致 |

A0/A1/B 必须来自**同一** `bench_w03_external_loop` 二进制；同一 build 内不得出现臂相关
编译宏/布局差异。`r3_*` 计数与 `SLUICE_BENCH_STATS` 只作诊断，不作性能结论。

### 3.3 R2 研究二进制（A_R2/B_R2 共用；同一 SHA）

- 目标树：`271b81bfc9d64eecc0266fe7066b9040525dd710` + `/home/jnhu/sluice-595-e5/evidence/measurement/patch.diff`
  （实测 sha256 `7be9d0145a8c268a62d9fa90338b7cda95cefc5f8ad2976e4706cb192fb1eaf0`，
  与 R2 `PROTOCOL.md:17-19` 一致）。
- 目标 `git diff HEAD` sha256：`b006dbbf751f5fb2bbe8fb9edb091ae66e57a181b8cf2470d880c9589e1f4072`
  （R2 `PROTOCOL.md:20-22`）。
- **当前 R2 worktree 不满足该目标**：`git diff HEAD` 实测 `f939602f…`；
  `git apply --reverse --check patch.diff` **exit 1**；`bench_w02_pipeline` 实测
  `66b2d0dd…` ≠ R2 `PROTOCOL.md:42` 的 `61c6d579…`。因此 legacy 块**必须**在
  `271b81bf + 7be9d014…` 的干净树重建，记录新二进制 SHA，并重做 R2 A/A；
  **不得**直接用当前 R2 worktree 的二进制。
- A_R2/B_R2 的 `bounded_progress_` 只存在于 `271b81bf + patch` 研究树；产品树 `271b81bf`
  本身没有该成员（`CONTEXT_COMPLETION.md` §5.5、`OPEN1_RECEIPTS.md` §2.5）。

### 3.4 fixture

- 64 MiB pattern 文件，置于 warm tmpfs（`/tmp/sluice-595-r3c/`）；块内复用，首次调用预热；
  bench 每个 measured rep 自行做 byte oracle；任何 oracle 失败使该 rep/ invocation 无效。
- 4 KiB 主 case：`request_count=20000`（每个 measured rep 20,000 次有效读）；`depth` 按 case。
- 64 KiB case：使用冻结的 `request_count=2000`，交付数据按原值报告，**不得重标定为 20k**。
- `SLUICE_BENCH_SKIP_DISK=1` 只跳过落盘同步，不跳过 byte oracle。

### 3.5 来源门（任一失败 → 该块 `BLOCKED_PROVENANCE`，不得产出裁决）

| 门 | 条件 |
|---|---|
| PROV-1 | patch sha256 = `2596f204…`；`git apply --reverse --check` exit 0；R3 worktree HEAD = `0a22a791…` |
| PROV-2 | 每块开始前重记 worktree `git status --porcelain`（15 行）与 `git diff HEAD` sha256；若变化，本协议下所有 R3 计时作废（二进制可能已变） |
| PROV-3 | 每个 invocation 记录二进制 sha256；与 §3.2 表不一致 → invocation 无效（SHA 失败） |
| PROV-4 | legacy 块：干净 `271b81bf + 7be9d014…` 树；`git diff HEAD` = `b006dbbf…`；reverse-check exit 0；新二进制 SHA 记录并绑定；否则 `BLOCKED_PROVENANCE` |
| PROV-5 | 跨 SHA 只并列、不相减：不得计算 R3 B − R2 B、R3 A0 − R2 A 等跨树差值进入任何 CI/门槛；只允许 `DESCRIPTIVE_ONLY` 并列表 |
| PROV-6 | A0 形状门：A0 分支不构造 `ready` span/adapter（`bench_w03_external_loop.cpp:321-323` 只有 `arm != a0` 才 `adapter.emplace`；A0 走 `:409-431` 内联构造 + `ctx.submit_read`）；`ready/accepted` 只在 adapter 存在时 reserve（`:349-354`） |
| PROV-7 | selector 读回：每次 invocation 记录 bench 标准输出的 `research arm=…` 与 `research cases=…`；与请求臂不符 → invocation 无效 |
| PROV-8 | 若任何 rebuild 发生 → 重新记录 5 个二进制 SHA；本协议下所有依赖旧 SHA 的块重新开始（A/A 重新校准）；不得把旧 SHA 的数据与新 SHA 数据配对 |

### 3.6 A0/suffix 回执缺失（明确登记）

- 本 ask 指定的 `…/evidence/A0_AND_SUFFIX_RECEIPT.md` **不存在**（§0.2 第 2 条）。
- 因此：
  1. A0 复原的**独立回执**无法核验；本协议只以源码形状门 PROV-6 与 `BUILD_COMPLETION.md`
     §8.4/§8.5 的功能计数作辅助，不把它们改写成“A0/suffix 回执已交付”。
  2. A0 可以进入 §14 的 `SAFE_IMMEDIATE` 基线块（A/A、资源基线、enter 计数），但
     **A0 不得进入任何 B−A0 决策门**，直到该回执以指定路径存在、包含 patch reverse-check、
     A0 源码形状 diff、d16 suffix 计数与 enter 计数，并被清单哈希绑定。
  3. 该缺失是**文档缺陷**，不是把任何门槛放宽的理由；`semanticsReady=false` 已使所有
     B−A0 门处于 `blockedBlocks`。

---

## 4. 冻结臂定义（本 ask 项 2）

公共约束（`CONTEXT_COMPLETION.md` §5.1；`OPEN1_RECEIPTS.md` §2.1）：

1. 同一臂 = 同一二进制 SHA；A0/A1/B 共用 R3 二进制，A_R2/B_R2 共用 R2 二进制。
2. 所有臂使用同一 case、同一 fixture、同一输入顺序、同一 buffer、同一 byte oracle；
   arm 只改变提交路径，不改变 `ReadOp` 的 `(fd,dst,len,offset)`。
3. `ReadyGroup` 仍按 `CONTEXT.md` §6：首提交前已拥有、offset/长度确定、buffer 有效、
   依赖已确定；禁止新增等待/前移依赖/追加预取造组。
4. 计时协议按 §6；A0/A1/B 的 `r3_*` 计数只作诊断。

### 4.1 A0 = IMMEDIATE（baseSha 原始 consumer 循环）

- 代码形状（本 session 实读）：`bench/bench_w03_external_loop.cpp:321-323` 仅当
  `arm != Arm::a0` 才 `adapter.emplace`；A0 走 `:409-431`：在
  `window.size()<depth && submitted<request_count` 循环内逐条构造 `ReadOp`
  （`op.file/op.dst/op.len/op.offset`）并立即 `ctx.submit_read(op)`，成功后 `Request`
  立即进入 `window`，不做预扫描、不构造 `std::vector<ReadOp> ready`、不分配组缓冲、
  不调用 adapter/backend batch entry。
- 产品 case（W-03 d4/d16/d64）`max_refill = depth`（`params.max_refill != 0 ? … : params.depth`，
  `:347-348`），与 baseSha 原循环（`git show 0a22a791:bench/bench_w03_external_loop.cpp:339-356`）
  逐字等价。
- 研究 D2 case 的唯一额外参数是 `max_refill`（`:753-765` 的 `-r3d2serial` 用 1），它只限制
  本次 refill burst 的补发上限；A0 仍在循环内逐条构造并立即提交。
- A0 可记录 `AsyncStats`/bench 统计，但统计不得改变提交路径；A0 不使用
  `bench/r3_ready_batch.hpp` 的 `Adapter`。

### 4.2 A1 = ADAPTER_IMMEDIATE

- 使用与 B **完全相同**的 ready-group 准备和输入：在补发点计算
  `want = min(depth-window.size, request_count-submitted, max_refill)`（`:357-359`），
  按输入顺序构造 `ready[0..want)` 的 `ReadOp`（同 slot/offset/len/dst），然后开始提交。
- 逐条调用**同一个 Context 内部研究入口**：`bench/r3_ready_batch.hpp:97-113`
  （`TestAccess::context_submit_ready_reads(ctx_, span(&op,1))`）；每个成员经过与该入口
  整组调用相同的 context 级 precheck、`access_mtx_`、admission/tally 与 `Request` 物化路径。
- A1 **不得**直接调用 backend seam，**不得**跳过 `initiation_rejection`/`access_mtx_`；
  否则它不是 A1，不能用于 `A1−B` 归因。
- 成功后立即物化 `Request<std::size_t>` 并进入 `window`；失败按同一入口错误处理。

### 4.3 B = READY_AWARE

- 使用与 A1 相同的 ready-group 准备、输入、case、budget；组必须是合法 `ReadyGroup`，
  不得等待/预取/前移依赖造组。
- 组可用时：前缀长度 = `min(ready.size(), ready_batch_cap=8, SQ 空闲, request capacity, budget)`；
  然后**一次**调用与 A1 相同的 Context 内部研究入口，传入该前缀 span
  （`bench/r3_ready_batch.hpp:115-135`）；入口在**同一调用内**完成 accept → prepare →
  dispatch → 一次 flush，不跨调用持有 SQE、不 defer flush。
- 单成员（或无合法组）时走与 A1 完全相同的单成员等价 immediate 路径（同一入口、span=1）。
- 前缀后仍有 suffix（cap/SQ budget/control/recovery/ineligible）时，suffix 由调用者从
  `untouched_begin` 开始逐条走**同一入口** span=1（`:120-134`）；suffix **不二次成组**。
  任何 suffix 二次批量都是新臂，不能冒充 B。
- B 必须对每个 accepted key 物化 `Request`，并满足 `BUILD.md` §3.2 的 disposition 契约；
  B 不得绕过 Context。

### 4.4 A_R2 = IMMEDIATE at 271b81bf

定义沿用 `OPEN1_RECEIPTS.md` §2.5 / `CONTEXT_COMPLETION.md` §5.5（本 session 未重开 R2
生产源；当前 R2 worktree 行号相对 OPEN1 引用有偏移，且树不等于目标 patch，见 §3.3）：

- 二进制/树：`271b81bf + patch.diff`（`7be9d014…`）。
- `SLUICE_595_ARM` 未设置或非 `B` → `bounded_progress_=false`；
  `defer_transport_flush_locked_()` 第一检查 `if (!bounded_progress_) return false;` 立即早退；
  `dispatch_after_accept` 在 dispatch 点立即 `submit_transport_locked()`，不执行
  owner/payload/witness/cap，也不 defer。
- 仅用于 R2 原 W-02 D2 shape（`W02-uring-4k-d2`，需 `SLUICE_BENCH_595_DEPTHS=1`）；
  不是 R3 新算法，也不是 baseSha 产品路径。

### 4.5 B_R2 = BOUNDED_PROGRESS at 271b81bf

定义沿用 `OPEN1_RECEIPTS.md` §2.6 / `CONTEXT_COMPLETION.md` §5.6：

- 同一 `271b81bf + patch` 二进制；`SLUICE_595_ARM=B` → `bounded_progress_=true`。
- `defer_transport_flush_locked_()` 门链顺序：`bounded_progress_` → owner
  （`progress_driver_seen_ && this_thread == progress_driver_recent_`）→ payload
  （`kind==read && native_length==4096`）→ witness
  （`live_cookies_ > transport_ledger_->size()`）→ cap
  （`transport_ledger_->size() < kDeferCap=8`）。
- 全门通过 → `dispatch_after_accept` 只 service control，不 submit；flush 延迟到后续
  progress 边界。任一失败 → 立即 submit（与 A_R2 相同）。
- 仅用于与 A_R2 同一二进制的 W-02 D2/D3/D16 同 SHA 对照；历史 R2 数值不得复用。

### 4.6 臂/二进制/开关映射

| 臂 | 构建树 | 运行时选择 | 可执行 case（本协议） |
|---|---|---|---|
| A0 | R3 研究二进制（baseSha + completion patch） | `SLUICE_595_R3_ARM` 缺省/A0 | `W03-uring-4k-d4/d16/d64`；`W03-uring-64k-d16`；`W03-threadpool-4k-*-w4`；研究 case 加 `SLUICE_595_R3_CASES=1` |
| A1 | 同一 R3 二进制 | `SLUICE_595_R3_ARM=A1` | 同上（adapter span=1） |
| B | 同一 R3 二进制 | `SLUICE_595_R3_ARM=B` | 同上（adapter 动态选批） |
| A_R2 | R2 研究二进制（干净 `271b81bf + 7be9d014…`） | `SLUICE_595_ARM` 缺省/A | `W02-uring-4k-d2/d3`（`SLUICE_BENCH_595_DEPTHS=1`）、`W02-uring-4k-d16` |
| B_R2 | 同一 R2 二进制 | `SLUICE_595_ARM=B` | 同一 W-02 D2/D3/D16 |

**W-02 的 R3 臂不可执行**：completion patch 不触碰 `bench/bench_w02_pipeline.cpp`
（`grep -c bench_w02_pipeline r3-completion.patch` = 0），R3 worktree 的 W-02 bench 无
`SLUICE_595_R3_ARM` 分支、无 D2 case（`grep "595_DEPTHS"` 无结果；case 列表 `:644-660`
只有 d1/d4/d16/d64/saturate/64k）。因此任何“R3 B 在 W-02 上不退化”的声明只能标
`NOT_ESTABLISHED`，不能用 R2 B_R2 代替。

---

## 5. 主形状、复核形状、负控、正控与产品门（本 ask 项 1）

### 5.1 主形状：W-03 external loop d16

| 项 | 冻结值 |
|---|---|
| case id | `W03-uring-4k-d16`（`bench/bench_w03_external_loop.cpp:552-559` 的 `W03-<backend>-<4k/64k>-d<depth>`） |
| workload | 4 KiB positional reads on 64 MiB warm tmpfs；`request_count=20000`；`depth=16`；queue_depth=16 |
| 线程 | app-owned epoll loop；**driver thread + submitter**（`bench_w03_external_loop.cpp:306-315` 创建 driver；submitter 为主线程） |
| 暴露 | `READY_EXPOSURE`；d16 稳态多 ready 组主导，20k 中 enter 可省 91.5–93.3%（`READY_EXPOSURE.md` §10.5；`census.json` d16_full saving 0.9151/0.92445/0.93345） |
| 臂 | A0 / A1 / B（同一 R3 二进制） |
| 设计 | 24 个 triplet（每个 triplet 含三臂各一次），见 §6.2 |
| 用途 | 主机制门（§9.2）；唯一允许产生 `GO_MECHANISM` 的形状 |

### 5.2 W-03 d4：跨组大小复核

| 项 | 冻结值 |
|---|---|
| case id | `W03-uring-4k-d4`；`request_count=20000`；`depth=4` |
| 目的 | 跨组大小复核：组大小 3–8 占稳态 88.0/91.6/97.1%，理论 enter 节省 72.3–73.2%（`READY_EXPOSURE.md` §10.3/§10.5；`census.json` d4_full 0.72275/0.73185/0.72665） |
| 裁决 | **不是产品胜出门**；只作机制/归因与保护。B 的 CPU 收益可以小于 d16，这是允许的发现；但 B 相对 A0 的 CPU/p95/p99 单侧上界必须 ≤5%、吞吐下降上界 ≤3%，否则 `D4_PROTECTION_FAIL`，主形状结论不得外推到 d4 |
| 臂 | A1/B 块在 `semanticsReady=false` 时 blocked；A0-only 基线 enabled |

### 5.3 W-03 d64：suffix/容量边界与保护

| 项 | 冻结值 |
|---|---|
| case id | `W03-uring-4k-d64`；`request_count=20000`；`depth=64`；queue_depth=64 |
| 机制边界 | B 的 cap=8 只允许一个 batch 前缀 ≤8；组 r>8 时前缀=8、`stop=cap`、`untouched=r−8`；suffix 由 adapter 从 `untouched_begin` 逐条 span=1 提交（`bench/r3_ready_batch.hpp:115-135`） |
| 必须记录/断言 | `prefix_max ≤ 8`；对 `stop ∈ {input_exhausted, cap, sq_budget, control, recovery, ineligible}` 每组 `accepted + untouched == r`；对 `stop=rejected` 每组 `accepted + untouched == r−1`（被尝试项位于 index `accepted`，既非 accepted 也非 untouched）；`suffix_single_calls > 0`（当出现 r>8）；`untouched == Σ max(r−8,0)`；accepted 总数=20,000；settled=20,000；无重复 key/SQE；所有结果/字节正确 |
| 保护 | CPU/p95/p99 单侧上界 ≤5%，吞吐下降上界 ≤3%（对 A0）；d64 的 CPU **不要求**改善（cap=8 会限制收益，suffix 逐条有成本）；若保护失败 → `D64_PROTECTION_FAIL`，主形状不得外推 |
| 臂 | A1/B 块在 `semanticsReady=false` 时 blocked；A0-only 基线 enabled |

### 5.4 W-02 D2 单条补发：核心负控

**(a) 历史机制同 SHA 复现（核心负控）**

| 项 | 冻结值 |
|---|---|
| case id | `W02-uring-4k-d2`（R2 树 `bench/bench_w02_pipeline.cpp:909-911` 的 `SLUICE_BENCH_595_DEPTHS` opt-in；R3 树无此 case） |
| 二进制 | 干净 `271b81bf + 7be9d014…`（§3.3） |
| 臂 | A_R2 / B_R2；24 个 AB/BA 对 |
| 暴露事实 | 稳态 19,998/19,998 组为单成员；20k ops 仅省 1 enter（0.005%）（`READY_EXPOSURE.md` §10.3/§10.5；`census.json`；`VERDICT.md:82`） |
| 负控门 | B_R2 相对 A_R2：CPU/p95/p99 单侧 95% 上界 ≤5%，吞吐下降上界 ≤3%（Holm 家族成员）；点估计也必须在界内（§9.5） |
| 结果语义 | 若超界 → `LEGACY_D2_NEGATIVE_CONTROL_FAIL`（复现旧 B 的 D2 退化）；这是**旧机制**的负控结论，不得外推到 R3 B，也不得记成 R3 B 的 PASS |
| 状态 | 本块 enabled（R2 同 SHA 复现），但必须先过 PROV-4 干净树/新 SHA |

**(b) R3 新算法的单条补发负控（机制形状）**

| 项 | 冻结值 |
|---|---|
| case id | `W03-uring-4k-d2-r3d2serial`（`bench_w03_external_loop.cpp:765`，`max_refill=1`，`request_count=20000`，`depth=2`） |
| 臂 | A0 / A1 / B（同一 R3 二进制） |
| 负控门 | 单条补发时 B 必须与 A1/A0 等价：B 的纯 submit enter = 20,000（不得合发）；CPU/p95/p99 单侧上界 ≤5%；吞吐下降上界 ≤3%；不得新增 defer/residue/组开销 |
| 结果语义 | 若 B 在单条时合发或超界 → 机制负控 FAIL，主形状 B 门不得 PASS |
| 状态 | `semanticsReady=false` 时 **blocked** |

### 5.5 D2 两条同时 ready：机制正控

| 项 | 冻结值 |
|---|---|
| case id | `W03-uring-4k-d2-r3d2pos`（`:764`，`request_count=2`，`depth=2`，`max_refill=2`） |
| 臂 | A0 / A1 / B；3 次重复/臂；`WARMUP=0 MEASURED=1`；`SLUICE_595_R3_CASES=1`；`SLUICE_BENCH_STATS=1`；行级 `strace -f -e trace=io_uring_enter` |
| A0 期望 | 2 次纯 submit enter，每次 `to_submit=1`；2 个 data SQE；2 个 CQE；2 个 4096B pattern-correct 结果 |
| A1 期望 | 2 次纯 submit enter，每次 `to_submit=1`；`r3.entries=2, singles=2, groups=0`；2 个 data SQE/CQE；2 个正确结果 |
| B 期望 | **1 次**纯 submit enter，`to_submit=2`；`r3.entries=1, groups=1, prefix_max=2`；2 个 data SQE；2 个 CQE；2 个正确结果；`untouched=0`；`flush_attempts=1`；`residue=false` |
| 判定 | 3/3 重复全部匹配 → 正控 PASS；任一不匹配 → 正控 FAIL，主形状 B 门不得 PASS |
| 依据 | `BUILD_COMPLETION.md` §8.4/§8.5（A0 6×`enter(1)`、A1 6×`1`、B 3×`enter(2)` 的 3 phase 计数）；`correctness/CORRECTNESS.md` 场景 1；`OPEN1_RECEIPTS.md` §3.15 第 3 项 |
| 状态 | **blocked**（需要 A1/B；`semanticsReady=false`） |

### 5.6 #604 真实应用：产品门

| 项 | 冻结值 |
|---|---|
| 当前状态 | Issue #604 `OPEN`、无 `READY_FOR_595` handoff（`CONTEXT_COMPLETION.md` §1.7；`OPEN1_RECEIPTS.md` §3.16；`BUILD_COMPLETION.md` §11.13） |
| 门 | 真实 consumer 或可辩护代表 trace + 冻结 oracle/依赖/预算/baseline + 语义/端到端/全执行者 CPU/尾延迟/资源门；W-03 是合成 benchmark，**不能替代** |
| 本轮裁决 | `PRODUCT=HOLD`；`measurement-product/` 只能记录 HOLD/blocked，不能产生产品 PASS |
| 状态 | **blocked**（与 `semanticsReady` 无关；即使为 true 也需 #604 handoff） |

### 5.7 其余形状（保护族，详见 §9.5）

| 形状 | case | 说明 |
|---|---|---|
| W-02 D3 | `W02-uring-4k-d3`（R2 树 `SLUICE_BENCH_595_DEPTHS=1`） | R2 同 SHA 复现；R3 树无 D3 |
| W-02 D16 | `W02-uring-4k-d16`（R3 树为 single immediate path；R2 树有 A/B 臂） | R3 B 不可执行 → NOT_ESTABLISHED；R2 A_R2/B_R2 可复现 |
| 64 KiB d16 | `W03-uring-64k-d16`（R3，`request_count=2000`） | B 的 `batch_eligible_read = len==4096` 不满足，必须回落 immediate；enter 数不得减少 |
| ThreadPool no-enter | `W03-threadpool-4k-d4-w4` / `-d16-w4` / `-d64-w4` | 必须 `io_uring_enter=0`、rc=0、byte oracle 通过；B 走 `batch_backend=false` 回落 |
| 适用 saturation | `W02-uring-4k-d16-saturate`（R3，`request_count=16`，`depth=16`，`saturating=true`） | W-03 无 saturation case；不得造 case。R3 B 不可执行 → NOT_ESTABLISHED |
| SMT {2,12} | `W03-uring-4k-d16` + `taskset -c 2,12` | 代表检查，8 对；safety-only |
| 跨物理 {2,8} | `W03-uring-4k-d16` + `taskset -c 2,8` | 代表检查，8 对；safety-only |

---

## 6. 计时协议（本 ask 项 3）

### 6.1 每个 invocation 的冻结参数

| 参数 | 冻结值 |
|---|---|
| 配对/区组 | **24 个有效配对/区组**；不足 24 个有效 → `INCOMPLETE`/`NOT_ESTABLISHED`，不得 PASS |
| warmup/measured | `SLUICE_BENCH_WARMUP=1`、`SLUICE_BENCH_MEASURED=5`（每个 invocation 5 个 measured rep，每 rep 固定 20,000 有效读；64 KiB case 2,000） |
| 绑核 | `taskset -c 2`（主块）；SMT/跨物理代表检查见 §9.5 |
| 存储 | warm tmpfs `/tmp/sluice-595-r3c/`；64 MiB pattern fixture；`df -h /tmp` 每 invocation 记录 |
| 块大小 | 4 KiB（主/复核/负控/正控）；64 KiB 仅 `W03-uring-64k-d16`，按 2,000 读报告 |
| case filter | `SLUICE_BENCH_CASE_FILTER='=<case-id>'`（精确匹配） |
| 统计开关 | 性能轮 `SLUICE_BENCH_STATS=0`；诊断轮可 `=1`，但诊断轮不进性能分析 |
| 臂 selector | `SLUICE_595_R3_ARM`（A0/A1/B）或 `SLUICE_595_ARM`（A_R2/B_R2），按 §4.6 |
| 研究 case | `SLUICE_595_R3_CASES=1` 仅在研究 case（d2pos/d2serial/d3/d16cap/64k）使用；产品 case 不使用 |
| 退出码 | 每个 invocation 必须 exit 0（硬门） |

### 6.2 配对与三臂交错

**(a) 两臂块（A/A、A_R2/B_R2、A0/A1 对照若单列）**：

- 24 对；pair i 为偶数 → A 先 B 后；奇数 → B 先 A 后（AB/BA 交替）。
- A/A：两成员同一臂、同一二进制、同一 env；pair i 偶数 → member1 先、member2 后；
  奇数 → member2 先、member1 后（顺序平衡）。
- 每对两个成员是两次**独立 bench invocation**，不是同一 invocation 的两个 phase。

**(b) 三臂块（A0/A1/B 主形状）**：

- 固定种子 **5950** 生成一次平衡交错，写死在运行脚本中，并在 `header.json` 记录生成代码与
  其 sha256；生成方式冻结为：
  1. `orders = list(itertools.permutations(['A0','A1','B']))`（6 个排列）；
  2. 用 `random.Random(5950).shuffle(orders)` 打乱一次；
  3. 重复该 6 排列序列 4 轮，得到 24 个 triplet；
  4. triplet j 的第 k 个 invocation 按 `orders[j % 6][k]` 执行。
- 24 个 triplet 产生三组配对的 24 对：`A1−A0`、`B−A0`、`B−A1`（同一 triplet 内计算）；
  重采样单位为 **triplet**（不是单个臂 invocation）。
- 该交错是平衡的（每个臂在每个位置出现 8 次；每个有序对出现 4 次），且种子固定。

### 6.3 每个 invocation 前后必须记录

| 记录 | 时点/方法 |
|---|---|
| case id、臂 selector、env、`taskset` mask | 启动前写入 `header.json`/`results.jsonl` |
| 二进制 sha256（bench + libsluice） | 启动前 `sha256sum`；与 §3.2 比对 |
| patch sha256、worktree HEAD、`git diff HEAD` sha256 | 块开始前一次；每 invocation 复记 HEAD |
| governor（cpu2）、`scaling_cur_freq`（cpu2） | 启动前读 sysfs；结束再读一次 |
| cpu2/cpu12 占用 | 启动前 100 ms `/proc/stat` 的 cpu2/cpu12 busy 比例；另记 `ps -eo pid,psr,pcpu,comm` 中 pin 在 2/12 的进程 |
| `df -h /tmp` | 启动前 |
| fixture sha256/大小、warm 状态 | 块开始前；fixture 变更 → 块作废 |
| 开始/结束 UTC | `date -u` 或等价的单调时钟 + UTC |
| exit code、bench oracle、stats gate、ops/request_count | 结束后 |
| 峰值 RSS（`/usr/bin/time -v` Maximum resident set size；`wait4 ru_maxrss` 交叉） | 结束后 |
| 线程数（`/proc/<pid>/status Threads` 最大采样） | invocation 期间 100 ms 采样 |
| 锁获取/等待/释放时间 | 块开始/结束（§12） |

### 6.4 每 invocation 的聚合公式（与 R1/R2 一致）

对 invocation 的 5 个 measured rep：

- `cpu/op = Σ cpu_s(m) / Σ ops(m)`（`cpu_s` = bench 每个 measured rep 的
  `CLOCK_PROCESS_CPUTIME_ID` 进程 CPU 差值；该内核上包含 io-wq worker；
  `PREREGISTERED.md:84-92` 的预计时机制判定）。
- `throughput = Σ ops(m) / Σ wall(m)`（ops/s）。
- `p50/p95/p99 = (1/5) Σ_m quantile_m(q)`（每个 rep 仍是固定 20,000 读的独立 pass；
  这是 per-invocation 汇总，不是 pooled histogram）。
- secondary：whole-invocation `wait4` rusage（utime+stime）；主门 CPU 的符号必须与
  主指标一致，否则该块 `NOT_ESTABLISHED`。

### 6.5 有效性、替换与无事后剔除

- **只有**以下失败使 invocation/配对无效：bench oracle 失败、bench stats gate 失败、
  exit code ≠ 0、ops/request_count 或 case id 不符、二进制/库 SHA 不符、arm selector
  读回不符。**没有**基于数值离群点的排除。
- 无效 invocation 必须被**替换**：同一 pair/triplet 槽位最多重试 2 次（共 3 次尝试）；
  所有尝试、失败原因、原始输出都保留在 `logs/`/`attempts.jsonl`；不得静默丢弃。
- 若某槽位 3 次尝试仍无效 → 该块 `INCOMPLETE`；`INCOMPLETE` 不得 PASS，按
  `NOT_ESTABLISHED` 记录。
- 背景负载/cpu12 占用不构成无效理由（按“只有上述失败才无效”）；但必须记录，并在
  裁决中作为 limitation；块开始前若 cpu12 有 CPU-bearing 外来进程，等待其空闲并记录
  等待时间（块开始后不再以此剔除任何 invocation）。
- **空表不得写成 PASS**：没有 `results.jsonl`/`summary.json` 的块只能写 `NOT_RUN` 或
  `BLOCKED_*`；没有 24 个有效配对的块只能写 `INCOMPLETE`/`NOT_ESTABLISHED`。

### 6.6 重采样单位

- 两臂块：**pair** 为重采样单位。
- 三臂块：**triplet** 为重采样单位；三组对比在同一 triplet 内计算，bootstrap 对 triplet
  索引重采样，保持三臂相关性。
- 不得把 5 个 measured rep 当作独立样本做跨臂 bootstrap（rep 是 invocation 内部汇总）。

---

## 7. 目录布局与完成门（本 ask 项 4）

### 7.1 冻结目录

| 目录 | 内容 | 当前状态 |
|---|---|---|
| `…/measurement-main/` | W-03 d16 三臂 + A/A；`aa-w03-d16/`、`w03-d16-a0/`、`w03-d16-a1/`（blocked）、`w03-d16-b/`（blocked） | 尚无数据 |
| `…/measurement-mechanism/` | D2 2-ready 正控、D2 单条负控（R3）、enter/CQE 对账；`d2pos/`、`d2serial/`、`enter-cqe/` | 尚无数据 |
| `…/measurement-negative/` | W-02 D2 核心负控的裁决表（原始数据在 legacy）、保护族表；`w02-d2/`、`protection/` | 尚无数据 |
| `…/measurement-legacy/` | 同 SHA A_R2/B_R2 的原始数据（W-02 D2/D3/D16）；`w02-d2/`、`w02-d3/`、`w02-d16/` | 尚无数据；需先建干净 R2 树 |
| `…/measurement-product/` | 真实应用或 HOLD；`hold/` | 尚无数据；预期 HOLD |
| `…/analysis/` | bootstrap 表、Holm 家族、完成门、最终裁决；`COMPLETION_GATE.json` | 目录已存在但空 |

每个块目录必须含：`header.json`（协议参数、臂、case、二进制/patch SHA、交错种子与
生成代码、fixture、锁记录）、`results.jsonl`（每 invocation 一行）、`summary.json`
（24 对/triplet 的配对差、bootstrap CI、状态）、`logs/`（原始 stdout/stderr、
strace/valgrind 输出）、`attempts.jsonl`（无效尝试）、`RECEIPT.md`（该块结论与限制）。

### 7.2 完成门

- `analysis/COMPLETION_GATE.json` 必须为 §14 的每个 block 写一个状态：
  `PASS`/`FAIL`/`NOT_ESTABLISHED`/`NOT_RUN`/`BLOCKED_*`/`HOLD`/`INCOMPLETE`。
- 任一**必需**块缺失、目录为空、`results.jsonl` 为空、或 summary 未绑定 24 个有效配对 →
  `COMPLETION_GATE=FAIL`；该 run 不得声称 completion（只能发布 partial）。
- `blockedBlocks` 必须写 `BLOCKED_*` 与解除条件，不能以空表冒充 PASS。
- 每个 `PASS` 必须能被 `results.jsonl` + `summary.json` + 二进制 SHA + 24 配对完整复算；
  不能只写结论。

---

## 8. A/A 校准（本 ask 项 5）

### 8.1 两个 A/A 块

| 块 | case | 二进制 | 臂 | 对 | 门 |
|---|---|---|---|---|---|
| `AA-W03-D16-A0` | `W03-uring-4k-d16` | R3（`0ebfb7f0…`） | A0 vs A0（同二进制/同 env） | 24 | CPU 半宽 ≤5 pp；吞吐半宽 ≤3 pp；p99 半宽 ≤3 pp |
| `AA-W02-D2-AR2` | `W02-uring-4k-d2`（`SLUICE_BENCH_595_DEPTHS=1`） | 干净 R2（`271b81bf + 7be9d014…`，新 SHA） | A_R2 vs A_R2（该 case 的 immediate 控制臂；W-02 无 R3 A0 consumer-loop 臂） | 24 | 同上 |

说明：W-02 D2 的“同 A0 臂”按该 case 的 immediate 控制臂解释为 A_R2；不得把 R3 A0 与
W-02 D2 混配，也不得用 R3 二进制（无 D2 case）冒充。

### 8.2 A/A 判据

- 对每个 metric，计算 24 对的 pair-relative 差 `d_i`，bootstrap 10,000（seed 5950，
  percentile 95% CI）；`half_width = (CI_hi − CI_lo)/2`。
- `AA PASS` 当且仅当 CPU half_width ≤5 pp、吞吐 half_width ≤3 pp、p99 half_width ≤3 pp。
  A/A 的 mean 偏移（如 R2 观察到的约 +0.7%）不构成失败，只有半宽门。
- 两个 A/A 块都必须 PASS，才允许任何 A/B、负控、正控的性能/保护对比。A/A 本身不产生
  机制结论。

### 8.3 单次测量条件修复（预声明）

- 若某 A/A 块失败任一 half-width 门：**恰好一次**修复，预声明选项为
  **(a) `taskset -c 2,12`**（两成员都改）**或 (b) `WARMUP=2`**（两成员都改）；
  二选一，不得混用，不得改门槛。
- 选择必须在实施**之前**写入 `measurement-*/aa-*/REPAIR_RECEIPT.md`，内容包括：失败的
  A/A 块、失败 metric、half-width、选项选择与理由、将重跑的完整 24 对。
- 修复后**整块重跑 24 对**；原失败回执与修复回执都保留（双回执）。
- 修复后仍失败 → 受影响 metric 记 `INCONCLUSIVE_NOISE`；依赖该 metric 的所有 A/B/负控/
  保护门不得 PASS（只能 `NOT_ESTABLISHED`），不得再用第二次修复。
- 本轮额度为 **1 次**；R1/R2 轮已用过的修复不消耗本轮额度，但本轮也不得援引 R1 的
  修复来绕过自己的 A/A。

---

## 9. 机制门、正控、负控与保护（本 ask 项 6、7、8）

### 9.1 机制正控（D2 两 ready）

见 §5.5。判定是**功能性**的，不计入性能统计；正控 FAIL → 主形状 B 门不得 PASS。

### 9.2 主机制门（W-03 d16，B−A0）

记 `X` 为臂，`Y` 为对照；pair/triplet 内：

- `d_cpu(X,Y)_i = (cpu_X,i − cpu_Y,i) / cpu_Y,i`
- `d_tput(X,Y)_i = (tput_Y,i − tput_X,i) / tput_Y,i`（正 = X 吞吐更低/更差）
- `d_lat(X,Y)_i(q) = (lat_X,i(q) − lat_Y,i(q)) / lat_Y,i(q)`（正 = X 更差）

对 24 个 triplet 求均值与 bootstrap 95% percentile CI（10,000，seed 5950）。主门
`B vs A0` **全部**满足才 PASS：

| 条件 | 门槛 |
|---|---|
| CPU 点估计 | `mean(d_cpu(B,A0)) ≤ −10%`（B 比 A0 至少少 10% 全执行者 CPU/op） |
| CPU 95% 上界 | `CI95_upper(d_cpu(B,A0)) ≤ −5%` |
| 吞吐下降 | `mean(d_tput(B,A0)) ≤ +3%` 且 `CI95_upper(d_tput(B,A0)) ≤ +3%` |
| p95 恶化 | `mean(d_lat(B,A0,95)) ≤ +5%` 且 `CI95_upper ≤ +5%` |
| p99 恶化 | `mean(d_lat(B,A0,99)) ≤ +5%` 且 `CI95_upper ≤ +5%` |
| 机制对账 | §9.3 PASS |
| 资源门 | §10 PASS（RSS、queue/thread/timer、per-Request allocation） |
| 功效 | 各 metric 的 CI half-width 不超过对应门（§9.7）；否则 `NOT_ESTABLISHED` |

CPU 指标定义：primary = bench 每 measured rep 的进程 CPU 差值（`cpu_s`），
whole-executor（该内核含 io-wq worker）；secondary = whole-invocation `wait4` rusage。
主门要求 primary 与 secondary 的**符号一致**；不一致 → `NOT_ESTABLISHED`，不得 PASS。

### 9.3 A1/B 的 enter/CQE 对账（机制存在性；单列 A1 额外成本）

**独立诊断轮**（不是性能轮；`WARMUP=0 MEASURED=1`，3 次/臂，`W03-uring-4k-d16`，
`taskset -c 2`，`strace -f -ttt -e trace=io_uring_enter`，`SLUICE_BENCH_STATS=1`）：

| 观测 | 定义/来源 | 门 |
|---|---|---|
| `pure_submit_enters` | `io_uring_enter` 且 `to_submit>0` 的行数 | A0=20,000；A1=20,000；B <20,000 且 `B ≤ 0.5×A0`（≥50% 减少；census 预测 91.5–93.3%） |
| `data_sqes` | bench `submitted_ops` 差值；并核对 strace `to_submit` 总和（W-03 无 cancel，故 `Σto_submit` = data SQE） | 每臂 =20,000 |
| `cqes` | bench `completed_ops` 差值 | 每臂 =20,000 |
| 结果 | bench `settled` + byte oracle | 每臂 =20,000，全部 pattern-correct |
| `to_submit>1` | strace 行 | B >0；A0/A1 =0 |
| `max SQE/enter` | strace `to_submit` 最大值 | B ≤8（cap）；A0/A1=1 |
| driver 提交 | 行级 trace 的 tid | driver thread 的纯 submit enter =0；所有纯 submit 在 submitter |
| A1 额外成本 | 单独报告 `d_cpu(A1,A0)`、`d_tput(A1,A0)`、`d_lat(A1,A0,q)`、A1−A0 enter 数 | **只作归因，不参与主门**；不得用 A1 的成本去“修正” B−A0 |

- 若 A0/A1 的 `pure_submit_enters` ≠20,000（例如 retry 导致额外 enter）→ 对账 FAIL；
  必须记录 retry 原因，不得把 retry 计数洗掉。
- 若 B 的 `pure_submit_enters` 未达 ≥50% 减少 → 机制不存在/未生效；主门不得 PASS，
  即使 CPU 点估计看起来改善。
- 对账轮的 CPU/墙钟数字**不是**性能证据；不得进入 §6 的配对统计。

### 9.4 负控（核心负控 + R3 单条负控）

1. **`NEG-LEGACY-W02-D2`（核心负控，R2 同 SHA）**：见 §5.4(a)。B_R2 相对 A_R2 的
   CPU/p95/p99 单侧 95% 上界 ≤5%、吞吐下降上界 ≤3%、点估计在界内；超界 →
   `LEGACY_D2_NEGATIVE_CONTROL_FAIL`。该结果只描述旧 B，不得外推到 R3 B。
2. **`NEG-R3-W03-D2SERIAL`（R3 机制形状负控）**：见 §5.4(b)。B 相对 A0 的纯 submit
   enter 必须 =20,000（单条不合发）、CPU/p95/p99 单侧上界 ≤5%、吞吐下降上界 ≤3%；
   若 B 合发或超界 → `R3_D2_NEGATIVE_CONTROL_FAIL`，主门不得 PASS。
3. 负控不允许“因为只省 1 enter 所以不测”的豁免；必须执行并记录。

### 9.5 保护族（形状、单侧界、Holm）

保护族包括（`semanticsReady=false` 时 B 部分 blocked；A0/A_R2 部分 enabled）：

| 保护块 | case | 对照 | 界 |
|---|---|---|---|
| `PROT-W03-D4` | `W03-uring-4k-d4` | B vs A0 | CPU/p95/p99 单侧上界 ≤5%；吞吐下降上界 ≤3% |
| `PROT-W03-D16` | `W03-uring-4k-d16` | B vs A0 | 同上（主形状的保护面；与 §9.2 同时判） |
| `PROT-W03-D64` | `W03-uring-4k-d64` | B vs A0 | 同上 + §5.3 的 suffix/容量断言 |
| `PROT-W02-D3` | `W02-uring-4k-d3` | B_R2 vs A_R2 | 同上（R2 同 SHA） |
| `PROT-W02-D16` | `W02-uring-4k-d16` | B_R2 vs A_R2（R2 同 SHA）；R3 B 不可执行 | 同上；R3 B 部分 `NOT_ESTABLISHED` |
| `PROT-64K-D16` | `W03-uring-64k-d16` | B vs A0 | 同上；且 B 必须回落 immediate、enter 数 = A0 |
| `PROT-THREADPOOL` | `W03-threadpool-4k-{d4,d16,d64}-w4` | B vs A0（功能） | `io_uring_enter=0`、rc=0、byte oracle；不得新增线程/队列/timer |
| `PROT-SATURATION` | `W02-uring-4k-d16-saturate` | B_R2 vs A_R2（R2）；R3 B 不可执行 | CPU/p95/p99 单侧上界 ≤5%；吞吐下降上界 ≤3%；R3 B 部分 `NOT_ESTABLISHED` |
| `PROT-SMT-2-12` | `W03-uring-4k-d16` + `taskset -c 2,12` | B vs A0，8 对 | safety-only；不得用于 PASS；违反则 `PLACEMENT_RISK` |
| `PROT-XPHYS-2-8` | `W03-uring-4k-d16` + `taskset -c 2,8` | B vs A0，8 对 | 同上 |

判定：

- 每个非 placement 保护块：点估计与单侧 95% 上界都必须在界内；否则 `FAIL`。
- 保护族另做 **Holm–Bonferroni** 单侧检验（family α=0.05）：one-sided p =
  bootstrap 中 `mean ≥ 界` 的比例（与 R1 `harness/analyze.py:9-10` 的口径一致）；
  Holm 调整后仍显著（p_adj < 0.05）才算该块“未恶化”成立。**每块必须同时通过
  单侧界与 Holm 调整**，否则该保护块 `FAIL`。
- placement 块（SMT/跨物理）是代表检查，不进入 Holm 家族、不用于 PASS；两个 placement
  块都显示超界 → `PLACEMENT_RISK`，主形状结论不得外推到该放置；只有一个超界 → 记录
  为 limitation，不用它反证或证明主门。
- 64 KiB d16：`batch_eligible_read = len==4096`，B 必须逐条回落；若 B 的 enter 数少于
  A0 或出现合发 → FAIL。
- ThreadPool：B 的 adapter 因 `dynamic_cast<UringAsyncBackend*> == nullptr` 走
  `batch_backend=false` → `ctx.submit_read`；不得出现 io_uring enter；不得新增用户态
  线程/队列/timer。
- Saturation：W-03 没有 saturation case；不得新建 case。R3 B 在 W-02 上不可执行，
  故 `PROT-SATURATION` 的 R3 B 部分只能 `NOT_ESTABLISHED`，不得用 R2 B_R2 冒充。

### 9.6 d4/d64 的特殊裁决

- d4：若 B 的 enter 减少达到 census 量级（约 72%）但 CPU 未改善，这是允许的发现；
  不得因此把 d4 标 FAIL；只有保护界超界才 `D4_PROTECTION_FAIL`。
- d64：cap=8 会截断 64-ready 组，suffix 逐条有成本；**不要求** CPU 改善；但
  `prefix_max≤8`、`accepted+untouched==r`（`stop=rejected` 为 `r−1`）、
  `untouched==Σ max(r−8,0)`、suffix 归属、20k 结算必须全对，且保护界
  不得超界。d64 的失败不得被 d16 的收益抵消。

### 9.7 功效不足（NOT_ESTABLISHED）规则

对每个 metric 与对应门槛 `b`（CPU 5 pp、吞吐 3 pp、p95/p99 5 pp；主 CPU 上界 5 pp）：

- 若 24 个配对/triplet 的 95% CI `half_width > b` → 该 metric `NOT_ESTABLISHED`；
  即使点估计在界内也不得写 PASS。
- 若有效配对 <24 → 该块 `NOT_ESTABLISHED`/`INCOMPLETE`。
- 若 metric 缺失、工具不可用（如 valgrind 失败）→ 对应门 `NOT_RUN`，整体不得 PASS。
- `NOT_ESTABLISHED` 不是 FAIL，也不是 PASS；它不产生“机制成立/不成立”的结论。

### 9.8 升级/降级决策树（写死）

1. 两个 A/A 都 PASS → 才看 A/B。
2. 正控 PASS 且 enter/CQE 对账 PASS → 才看主门。
3. 主门（§9.2 全部条件）PASS 且资源门 PASS → `GO_MECHANISM`（仅限 W-03 d16 代表形状）。
4. 若 `B−A1` 有收益但 `B−A0` 任一条件不过 → **不升级**；记 `B_A1_ONLY`，不得开产品
   候选 PR、不得改 `KEEP_IMMEDIATE`。
5. 若 B 的 CPU 改善但 enter 对账不过 → 机制未证明；不得 PASS（改善不得归因于批量）。
6. 若保护/负控任一 FAIL → 不得 `GO_MECHANISM`；按失败块记 `FAIL`，主形状结论不得外推。
7. 若任何门 `NOT_ESTABLISHED` → 不得 `GO_MECHANISM`；只能 `INCONCLUSIVE`/`PARTIAL`。
8. 产品层永远是 §10.4 的独立门；机制 `GO_MECHANISM` 不等于产品 GO。

---

## 10. 产品与资源门（本 ask 项 9）

### 10.1 峰值 RSS

- 每 invocation 用 `/usr/bin/time -v` 记录 `Maximum resident set size`（KiB），并用
  `wait4` 的 `ru_maxrss` 交叉核对。
- 主形状（W-03 d16）B vs A0：`(RSS_B − RSS_A0)/RSS_A0` 的点估计 ≤ **+2%**，95% 上界
  ≤ **+2%**；A1 同样单独报告。
- RSS 门在**同一 case、同一 depth、同一 fixture**下比较；不得跨 case 比较。
- 若 `/usr/bin/time -v` 不可用或字段缺失 → RSS 门 `NOT_RUN`，整体不得 PASS。

### 10.2 零新增 queue/thread/timer/每 Request allocation

三路证据，缺一不可：

1. **静态审计**：批量入口/ adapter 只使用调用者 span、栈上固定 `ReadyReadBatch`
   （`std::array<Request,8>`）、既有 dispatch/ledger/prepared/router；热路径无容器增长、
   无 `new`/`malloc`、无新 `std::thread`/`timer`/`queue`。以 `BUILD_COMPLETION.md` §6
   的约束为基线；源码审计记录到 `logs/audit.txt`。
2. **运行时刻**：`/proc/<pid>/status Threads` 100 ms 采样；B 的用户态最大线程数不得
   超过 A0；strace 诊断轮统计 `clone`/`clone3`/`timer_create`/`epoll_create*`/
   `eventfd`/`eventfd2` 的新增次数；B vs A0 必须为 0 新增。kernel io-wq worker 是
   物理线程，记录其数量，不作为“新增用户态线程”判定。
3. **分配斜率**：`valgrind --tool=memcheck --xtree-memory=full`（或等价的
   `--tool=massif` + 分配计数）在 `W03-uring-4k-d16` 上跑 512 与 1024 读的诊断 pass，
   每臂各一次；`slope = (allocs_1024 − allocs_512)/512`；要求 B/A1 的 slope
   ≤0.01 allocations/read（即无每 Request 分配），且 B slope ≤ A0 slope +0.01。
   valgrind 运行是诊断轮，不进计时；若 valgrind 失败/不可用 → 分配门 `NOT_RUN`，
   整体不得 PASS。
- 任一子门 FAIL → 资源门 FAIL，整体不得 `GO_MECHANISM`；任一子门 NOT_RUN → 资源门
  `NOT_ESTABLISHED`，整体不得 PASS。

### 10.3 真实应用（#604）

- 只有 #604 提供 `READY_FOR_595` handoff 与 G0 workload admission（真实 consumer 或
  可辩护代表 trace、冻结 oracle/依赖/预算/baseline）后，才允许 `measurement-product/` 的
  真实应用计时。
- 真实应用必须另行写**独立预注册**（可在本文件冻结后、应用计时前写），至少覆盖：
  语义/端到端 oracle、全执行者 CPU、p50/p95/p99、吞吐、峰值 RSS、线程/队列/timer/
  每 Request allocation、负控、A/A、退出码/SHA。
- W-03 d16 的任何收益/无退化**不能**替代真实应用门；`KEEP_IMMEDIATE` 的改变必须有
  真实应用净胜数据 + 负控 + 资源门 + v2 Ledger 更新。
- 当前 #604 `OPEN`、无 handoff → `PRODUCT=HOLD`；`measurement-product/` 只记录 HOLD。

### 10.4 产品裁决

- 本轮只允许 `PRODUCT=HOLD`（除非 #604 handoff 在本文件写盘后出现且满足 §10.3）。
- 即使机制 `GO_MECHANISM`，产品仍 HOLD；不得开产品候选 PR、不得改 `KEEP_IMMEDIATE`。

---

## 11. 统计、排除与完成门（本 ask 项 10）

### 11.1 统计量

- 每对/triplet 的 pair-relative 差：
  - CPU/尾延迟：`(B−A)/A`（负 = B 更好）；
  - 吞吐：`(A−B)/A`（正 = B 更差，即吞吐下降）。
- 汇总：24 个 pair/triplet 的均值；bootstrap **10,000** 次，**seed 5950**，
  **percentile 95% CI**（`numpy.percentile` 或等价实现；实现 sha256 记录）。
- A/A：24 对，半宽门 5/3/3 pp。
- 主门/保护/负控：单侧 95% 上界；保护族另做 Holm–Bonferroni（family α=0.05，
  one-sided）。
- 三臂块的 bootstrap 以 triplet 为重采样单位；不得把同一 triplet 的三臂拆成独立样本。

### 11.2 排除规则

- 只有 §6.5 列出的 bench oracle/stats/exit/ops/SHA 失败才无效；没有事后离群点剔除、
  没有“看起来异常就重跑”的规则。
- 无效 invocation 的替换上限 2 次/槽位；所有尝试保留。
- 背景负载、cpu12 占用、频率漂移不构成无效理由；记录并在 limitation 中说明。
- 任何“因为 A/A 没过就改用别的 metric”或“因为 B 只在某个 phase 好就只报该 phase”
  都是禁止的；所有 24 对/5 rep 全部进入统计。

### 11.3 空表与完成门

- 空 `results.jsonl`/空 `summary.json` = `NOT_RUN`，**不得写成 PASS**。
- 没有 24 个有效配对 = `INCOMPLETE`/`NOT_ESTABLISHED`，不得 PASS。
- `analysis/COMPLETION_GATE.json` 任一必需块缺失/空/未绑定 SHA → `COMPLETION_GATE=FAIL`；
  该 run 只能发布 partial，不能声称 completion。
- 一个 `PASS` 必须能被原始 `results.jsonl` + 二进制 SHA + 24 配对完整复算；不能只写结论。

### 11.4 分析脚本

- 两臂块可复用 `/home/jnhu/sluice-595-e5/harness/analyze.py`，前提是 sha256 记录、
  合成数据自测通过、并确认它实现 pair-relative/bootstrap 10k/seed 5950/percentile；
  该脚本当前只支持 `AA`/`AB`，不支持三臂。
- 三臂块必须使用**新**分析脚本（放在 completion `harness/`，不提交 master；sha256 记录）。
  新脚本必须先通过合成自测：
  1. 注入已知效应（例如 B−A0 CPU −12%、tput +1%、p99 +2%）→ 正确判 PASS；
  2. 零效应 → CI 覆盖 0，主门 FAIL/不显著；
  3. 注入 +6% CPU → 保护/主门按规则 FAIL；
  4. 固定输入重复运行 → 输出逐字节相同（seed 固定）。
  自测输出归档；自测不过 → 分析无效，所有结果 `NOT_ESTABLISHED`。
- 所有脚本的 sha256、python 版本、命令行写入 `analysis/harness.json`。

---

## 12. 仪器分离与测量锁（本 ask 项 11）

### 12.1 三条车道（不得混用）

| 车道 | 内容 | 是否进性能统计 |
|---|---|---|
| 性能轮 | 仅 bench + `/usr/bin/time -v` + `/proc` 采样；无 strace/perf/valgrind/PMU/tracing | 是 |
| 短机制诊断轮 | `strace` enter/SQE/CQE 计数、`SLUICE_BENCH_STATS=1`、D2 正控/enter 对账 | 否（只作机制/功能证据） |
| 正确性/fault 轮 | 26 场景确定性测试、变异体、fault oracle | 否 |

- PMU/tracing **不得**进入任何计时 invocation；`perf_event_paranoid=2` 与
  `unprivileged_bpf_disabled=2` 下的权限限制必须记录；不可用的 tracing 不得记为已测。
- strace/valgrind 的 wall/CPU 数字不得作为性能结论；不得与性能轮的 JSONL 混合分析。

### 12.2 测量锁

- 构建、计时、tracing 都必须持有 `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock` 的
  **排他 `flock`**；块开始到结束全程持锁。
- 每次获取记录：UTC 时间、等待时长、锁文件 inode/mtime、持锁者（应为空）、释放时间；
  写入块 `header.json` 与 `analysis/lock-log.jsonl`。
- 与其它 E5 研究错峰：若锁被占用，等待并在日志记录；不得无锁运行。前轮 P-5
  （正确性运行未持锁，`PROTOCOL.md:200`；`correctness/CORRECTNESS.md` §6.3）
  **不得重复**。
- 本 session 只做了只读检查，未持锁；这不构成构建/计时/tracing 的无锁偏离。

---

## 13. 非追溯条款（本 ask 项 12）

1. 原 R3 **没有** `PREREGISTERED_R3.md`；本文件不是它的补写，也不能被引用为“原 R3
   早已预注册”。任何把本文件回溯成 2026-11-05 之前存在的说法都是伪造。
2. 原 R3 的主形状是 D2/`W02-uring-4k-d2`；续跑主形状是 `W03-uring-4k-d16`。两者回答
   的问题不同：原问题是“W-02 D2 两 ready 是否值得”；续跑问题是“W-03 d16 真实暴露
   形状在完整 CPU 计入后是否值得”。续跑 PASS **不回答**原问题，原问题保持
   `NOT_RUN`/`UNANSWERED`，直到对应块实际执行。
3. 续跑的任何结果标签必须含 `CONTINUATION_` 与 case id；不得写成
   `ORIGINAL_R3_PASS`、`R3_PREREGISTERED_PASS` 或“issue #595 预注册门通过”。
4. 原 R3 的 `NOT_RUN` 项（A/A、A/B、负控、资源门、产品门）在本文件中仍是 `NOT_RUN`
   或被 `BLOCKED_*`；本文件不把它们改写成已测。
5. 历史 R2 数值（`docs/research/595-causal-r2/PROTOCOL.md:126` 的 +3.73%/+4.96%/+7.07%/
   −3.58%）只作历史对照；本轮 legacy 块必须重新构建/配对，不得复用。

---

## 14. enabledBlocks / blockedBlocks（本 ask 项 13）

### 14.1 当前冻结：`semanticsReady=false`

按本 ask 项 13：`semanticsReady=false` 时只启用 **R2 同 SHA 复现**与**安全 IMMEDIATE**
块；所有新 B/A1 块列入 `blockedBlocks`。

#### enabledBlocks（当前允许测量）

| ID | 目录 | case / 内容 | 臂 | 备注 |
|---|---|---|---|---|
| `AA-W03-D16-A0` | `measurement-main/aa-w03-d16/` | `W03-uring-4k-d16` A/A | A0 vs A0 | 安全 IMMEDIATE；A/A 门 5/3/3 |
| `SAFE-A0-W03-D4-D16-D64` | `measurement-main/w03-a0-baseline/` | W-03 d4/d16/d64 A0-only 基线 | A0 | 每 case 24 次 invocation；descriptive/resource/enter 基线；无 B 门 |
| `SAFE-A0-W03-64K-D16` | `measurement-main/w03-64k-a0/` | `W03-uring-64k-d16` | A0 | 2,000 读/rep；按原值报告 |
| `SAFE-A0-W03-THREADPOOL` | `measurement-main/w03-threadpool-a0/` | threadpool d4/d16/d64 | A0 | 3 次/case；`io_uring_enter=0` |
| `SAFE-A0-W02-D16` | `measurement-negative/w02-d16-a0/` | `W02-uring-4k-d16` R3 single path | A0 | descriptive；不是 R3 B 保护 |
| `SAFE-A0-W02-SATURATE` | `measurement-negative/w02-sat-a0/` | `W02-uring-4k-d16-saturate` | A0 | descriptive；R3 B 不可执行 |
| `SAFE-A0-PLACEMENT` | `measurement-main/placement-a0/` | `W03-uring-4k-d16` + `taskset -c 2,12` / `2,8` | A0 | 8 次/放置；代表检查 |
| `AA-W02-D2-AR2` | `measurement-negative/aa-w02-d2/` | `W02-uring-4k-d2` A/A | A_R2 vs A_R2 | R2 同 SHA；需先过 PROV-4 |
| `LEGACY-W02-D2-AR2-BR2` | `measurement-legacy/w02-d2/` | `W02-uring-4k-d2` 核心负控 | A_R2/B_R2 | 24 对；同 SHA 复现；需先过 PROV-4 |
| `LEGACY-W02-D3-AR2-BR2` | `measurement-legacy/w02-d3/` | `W02-uring-4k-d3` | A_R2/B_R2 | 24 对；同 SHA 复现；需先过 PROV-4 |
| `LEGACY-W02-D16-AR2-BR2` | `measurement-legacy/w02-d16/` | `W02-uring-4k-d16` | A_R2/B_R2 | 24 对；同 SHA 复现；需先过 PROV-4 |

说明：`SAFE-A0-*` 只产生 A0 的 descriptive/资源/enter 基线；不得与 B 做配对对比，
不得进入 §9 的主门/保护/Holm。`LEGACY-*` 只描述 R2 旧机制，跨 SHA 只并列不相减。

#### blockedBlocks（当前不得测量）

| ID | 目录 | 阻塞原因 | 解除条件 |
|---|---|---|---|
| `MAIN-W03-D16-A1` | `measurement-main/w03-d16-a1/` | `SEMANTICS_NOT_READY`（新 A1 入口） | §14.3 独立 verifier 回执 |
| `MAIN-W03-D16-B` | `measurement-main/w03-d16-b/` | `SEMANTICS_NOT_READY`（新 B 入口） | 同上 |
| `MAIN-W03-D16-B-vs-A0` | `analysis/` | `SEMANTICS_NOT_READY`；主门依赖 B/A1 | 同上 |
| `MAIN-W03-D16-B-vs-A1` | `analysis/` | `SEMANTICS_NOT_READY` | 同上 |
| `CROSS-W03-D4-A1-B` | `measurement-main/w03-d4-a1b/` | `SEMANTICS_NOT_READY` | 同上 |
| `CROSS-W03-D64-A1-B` | `measurement-main/w03-d64-a1b/` | `SEMANTICS_NOT_READY` | 同上 |
| `MECH-POS-D2POS-A1-B` | `measurement-mechanism/d2pos/` | `SEMANTICS_NOT_READY`；正控需 A1/B | 同上 |
| `MECH-NEG-W03-D2SERIAL-A1-B` | `measurement-mechanism/d2serial/` | `SEMANTICS_NOT_READY` | 同上 |
| `MECH-ENTER-CQE-RECON-A1-B` | `measurement-mechanism/enter-cqe/` | `SEMANTICS_NOT_READY` | 同上 |
| `PROT-W03-D4-B` | `measurement-negative/protection/w03-d4/` | `SEMANTICS_NOT_READY` | 同上 |
| `PROT-W03-D16-B` | `measurement-negative/protection/w03-d16/` | `SEMANTICS_NOT_READY` | 同上 |
| `PROT-W03-D64-B` | `measurement-negative/protection/w03-d64/` | `SEMANTICS_NOT_READY` | 同上 |
| `PROT-64K-D16-B` | `measurement-negative/protection/64k/` | `SEMANTICS_NOT_READY` | 同上 |
| `PROT-THREADPOOL-B` | `measurement-negative/protection/threadpool/` | `SEMANTICS_NOT_READY` | 同上 |
| `PROT-SMT-2-12-B` | `measurement-negative/protection/smt/` | `SEMANTICS_NOT_READY` | 同上 |
| `PROT-XPHYS-2-8-B` | `measurement-negative/protection/xphys/` | `SEMANTICS_NOT_READY` | 同上 |
| `PROT-W02-D16-B` | `measurement-negative/protection/w02-d16/` | `NO_W02_R3_ARM`：R3 树无 W-02 arm/D2；patch 不触碰 `bench_w02_pipeline.cpp` | 新臂定义 + 新 bench 面（超出本协议） |
| `PROT-SATURATION-B` | `measurement-negative/protection/saturation/` | `NO_W02_R3_ARM`（R3 B 不可执行）；W-03 无 saturation case | 同上 |
| `RESOURCE-B` | `measurement-main/resource-b/` | `SEMANTICS_NOT_READY`；RSS/alloc/thread/timer 需 B/A1 | §14.3 + B 可测 |
| `PRODUCT-604` | `measurement-product/` | `PRODUCT_NO_604_HANDOFF`（Issue #604 OPEN、无 `READY_FOR_595`） | #604 handoff + §10.3 独立预注册 |

### 14.2 `semanticsReady=true` 时的激活规则

- 激活**所有新 B/A1 块**：`MAIN-W03-D16-A1/B`、`CROSS-W03-D4/D64-A1-B`、
  `MECH-POS-D2POS-A1-B`、`MECH-NEG-W03-D2SERIAL-A1-B`、`MECH-ENTER-CQE-RECON-A1-B`、
  `PROT-W03-D4/D16/D64-B`、`PROT-64K-D16-B`、`PROT-THREADPOOL-B`、
  `PROT-SMT-2-12-B`、`PROT-XPHYS-2-8-B`、`RESOURCE-B`。
- **不激活**：`PROT-W02-D16-B`、`PROT-SATURATION-B`（结构上无 R3 W-02 臂；保持
  `NO_W02_R3_ARM`，只能 `NOT_ESTABLISHED`）、`PRODUCT-604`（仍受 #604 门约束）。
- 翻转前收集的任何 B 数据（若有）不得用于 B 门；翻转后必须重新从 A/A 开始，
  24 对/triplet 全新采集。
- 翻转必须由独立 verifier 回执触发，且回执必须包含：patch sha `2596f204…`、二进制
  SHA §3.2、26 场景正确性回执、A0/suffix 回执（§3.6）、OPEN-1.4/OPEN-1.14 的处置、
  以及“允许 B/A1 计时”的明确句子。实现者自述的 `semanticsOk=true` 不足以翻转。

### 14.3 翻转回执的最低内容

1. 独立 verifier 姓名/角色与日期（`date -u`）。
2. 被验证的 patch sha256、worktree HEAD、`git diff HEAD` sha256、二进制 SHA。
3. 正确性：26 场景列表与结果、3 变异 RED/RESTORED GREEN、既有回归、`shutdown_lifecycle_test`；
   所有运行持 `MEASUREMENT.lock` 的记录。
4. A0/suffix 回执：`A0_AND_SUFFIX_RECEIPT.md` 存在、哈希绑定、A0 源码形状、d16 suffix
   计数、enter 计数。
5. OPEN-1.4（`have_ring_` 生命周期）、OPEN-1.14（资源界）的处置/残余限制。
6. 明确声明 `semanticsReady=true` 及适用范围（哪些臂/case 可测）。
7. 该回执本身的 sha256 与清单绑定。

---

## 15. 状态词汇与完成门

| 状态 | 含义 |
|---|---|
| `PASS` | 该块所有预声明条件满足，24 个有效配对/triplet，CI 可复算，SHA/exit/ops/oracle 全过 |
| `FAIL` | 任一预声明界被违反（点估计或单侧上界） |
| `NOT_ESTABLISHED` | 功效不足（half-width > 门）、有效配对 <24、metric 缺失、工具不可用；**不是 PASS** |
| `NOT_RUN` | 未执行；空表只能是此状态 |
| `BLOCKED_SEMANTICS_NOT_READY` | `semanticsReady=false` 下的新 B/A1 块 |
| `BLOCKED_PROVENANCE` | patch/树/二进制/SHA 来源门失败 |
| `BLOCKED_PRODUCT_NO_604` | #604 无 handoff |
| `NO_W02_R3_ARM` | R3 W-02 B 结构上不可执行 |
| `INCOMPLETE` | 有效配对不足且替换额度耗尽 |
| `INCONCLUSIVE_NOISE` | A/A 修复后仍失败 |
| `HOLD` | 产品层（#604 未 handoff） |
| `GO_MECHANISM` | 仅 W-03 d16 主门 + 机制对账 + 资源门 + 保护/负控全过；不等于产品 GO |

完成门：`analysis/COMPLETION_GATE.json` 必须为 §14 每个 block 写状态；任一必需块空/缺失 →
`COMPLETION_GATE=FAIL`；`blockedBlocks` 只能写对应 `BLOCKED_*` 与解除条件，不能以空表
冒充 `PASS`。

---

## 16. 冻结完整性、未做事项与签名

### 16.1 时间戳

- `date -u` 首条：**2026-11-05T10:42:09Z**
- `date -u` 写前：**2026-11-05T10:47:31Z**
- 本文件写盘后，其 sha256 由 `PreregResult.sha256` 返回，并应由交付者写入外部清单；
  本文件不包含自身哈希（写入会使哈希改变）。

### 16.2 本 session 实际运行过的命令（全部只读）

```text
date -u +"%Y-%m-%dT%H:%M:%SZ"
ls -la /home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/evidence/
ls -la .../evidence/correctness/                     # MISSING
find /home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z -maxdepth 4 -type d
find /home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z -maxdepth 4 -type f
find /home/jnhu/sluice-595-e5 -maxdepth 5 -iname "*receipt*" -type f
find /home/jnhu/sluice-595-e5 -name "*A0*" -o -name "*SUFFIX*"
grep -rn "A0_AND_SUFFIX" /home/jnhu/sluice-595-e5
sha256sum .../evidence/r3-completion.patch
cd /home/jnhu/sluice-595-e5/r3/worktree && git rev-parse HEAD && git status --porcelain
git diff --stat HEAD; git diff HEAD | sha256sum
git apply --reverse --check .../r3-completion.patch; git apply --stat ...
sha256sum build/linux/x86_64/release/{bench_w03_external_loop,bench_w02_pipeline,
  uring_ready_batch_deterministic_test,uring_ready_batch_research_test,uring_ready_batch_fault_test}
ls -la /home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock; fuser -v ...
cd /home/jnhu/sluice-595-e5/causal-r2/worktree && git rev-parse HEAD && git status --porcelain
git diff HEAD | sha256sum; git apply --reverse --check .../patch.diff; git apply --stat ...
sha256sum .../causal-r2/worktree/build/linux/x86_64/release/{bench_w02_pipeline,bench_w03_external_loop}
cd /home/jnhu/Source/Sluice && git status --porcelain | wc -l && git rev-parse HEAD
command -v strace perf valgrind heaptrack taskset flock cpupower
cat /proc/sys/kernel/{perf_event_paranoid,unprivileged_bpf_disabled}
cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor
cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_cur_freq
cat /sys/devices/system/cpu/cpu2/topology/thread_siblings_list
nproc; df -h /tmp
python3 -I -c '<读取 census.json 的只读 JSON 检查>'
sed/grep 读取上述文档与 bench 源码（bench_w03_external_loop.cpp、r3_ready_batch.hpp、
  bench_w02_pipeline.cpp、R2 PROTOCOL.md、R3 前轮 PROTOCOL.md、correctness/CORRECTNESS.md）
test -e .../A0_AND_SUFFIX_RECEIPT.md .../correctness .../PREREGISTERED_R3.md
```

**没有运行**：任何 bench 进程、A/A、A/B、负控、正控、保护、strace/PMU/tracing 轮、
`xmake` 构建、确定性测试、`valgrind`、`taskset` 下的 bench、任何写产品/测试/bench 的
命令。`frozenBeforeTiming=true` 成立。

### 16.3 明确未做/无法核验

1. `A0_AND_SUFFIX_RECEIPT.md` **不存在**，无法读取或核验（§0.2/§3.6）。
2. completion 树内 `correctness/` **不存在**；completion 正确性只能引用
   `BUILD_COMPLETION.md` §8.3/§9，且其 §11.1 明确无独立 verifier 签字。
3. R2 当前 worktree **不等于** `271b81bf + 7be9d014…`（reverse-check exit 1；diff
   `f939602f…` ≠ `b006dbbf…`），legacy 块必须重建（§3.3）。
4. `heaptrack` 不可用；分配门使用 `valgrind`（可用）作为替代，但尚未运行。
5. `perf_event_paranoid=2`、`unprivileged_bpf_disabled=2`；PMU/bpf 深层 tracing 可能不可用；
   不可用即记 `NOT_RUN`，不得当作已测。
6. 本文件不产生任何性能数值；`BUILD_COMPLETION.md` §8.4 的 smoke 计数、
   `census.json` 的 enter 计数、`READY_EXPOSURE.md` 的分布均**不是**性能证据。

### 16.4 签名与不可变性

- 本文件由 #595 R3 续跑预注册员在 **2026-11-05T10:47:31Z** 写盘；写盘前无新增计时。
- 自写盘起，除 §8 的单次 A/A 修复（且门槛不变）外，本文件的臂、case、配对、门槛、
  统计、排除、目录、enabled/blocked 列表**不可修改**。
- 任何后续偏离必须新建 `DEVIATION_*.md`，引用本文件 sha256、偏离条款、原因、影响范围，
  并保留原始回执；不得静默改写本文件。
- 本文件 sha256 由 `PreregResult.sha256` 返回；建议交付者将其写入 completion `MANIFEST`
  并在 `CONTEXT_COMPLETION.md` 追加一行链接（本文件不自行修改其他文件）。

---

**冻结结论**：本文件冻结的是**续跑前瞻协议**，不是原 R3 预注册的补写；`semanticsReady=false`
下只允许 R2 同 SHA 复现与安全 IMMEDIATE 块，所有新 B/A1 块等待独立 verifier 回执。
任何 `W03-uring-4k-d16` 的通过都只能标为 `CONTINUATION_W03_D16`，不能记成原 R3 预注册
通过，也不能替代 #604 真实应用产品门。
