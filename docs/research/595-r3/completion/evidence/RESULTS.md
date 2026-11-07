# #595 R3 续跑 — 结果汇总（RESULTS）

| 字段 | 值 |
|---|---|
| 角色 | R3 完成轮裁决与交付员 |
| 统计口径 | pair-relative 差（cpu/p50/p95/p99=(X−Y)/Y，tput=(Y−X)/Y，正 = X 更差）；bootstrap 10,000 / seed 5950 / percentile 95% CI；单侧 95% 上界；Holm α=0.05 |
| 数据来源 | `raw/calibration/*`、`raw/protection/*`、`raw/mechanism/*`（绝对路径见各节）；独立复核 `measurement-analysis/REVIEW_REPORT.md`（数值与执行员 summary 全部在 1e-9 内一致） |
| 主门状态 | **W03 d16 B−A0 三臂 24-triplet 性能块 NOT_RUN（BLOCKED_SEMANTICS_NOT_READY）**；本文件不含任何 R3 B 的性能门结论 |

---

## 1. A/A 校准（24 对/块；门：CPU≤5pp、吞吐≤3pp、p99≤3pp）

| 块 | 对照 | n | CPU mean [CI]（hw） | 吞吐 mean [CI]（hw） | p99 mean [CI]（hw） | 判定 |
|---|---|---|---|---|---|---|
| AA-W03-D16（R3 `0ebfb7f0…`） | A0 vs A0 | 24 | −0.816% [−1.616, −0.022]（**0.797pp**） | −0.870% [−1.699, −0.057]（**0.821pp**） | +0.313% [−1.785, +2.292]（**2.039pp**） | **PASS** |
| AA-W02-D2（R2 `32beabcd…`） | A_R2 vs A_R2 | 24 | +0.584% [−0.270, +1.454]（**0.862pp**） | +0.539% [−0.310, +1.397]（**0.853pp**） | −0.407% [−1.863, +0.978]（**1.420pp**） | **PASS** |

绝对量级（每臂 24 invocation 均值）：AA-W03-D16 m1/m2 CPU/op 3154.75/3128.33 ns、
tput 317,259/319,946 ops/s；AA-W02-D2 m1/m2 4830.71/4858.23 ns、206,812/205,669 ops/s。
A/A 修复额度未使用（`repairUsed=false`）。

## 2. R2 同 SHA 配对块（A_R2/B_R2 同二进制 `32beabcd…`；24 对；负控/保护门 5/5/5/3 + Holm）

### 2.1 R2-D2 核心负控（`W02-uring-4k-d2`）——**FAIL**

| metric | mean | CI95 | 单侧 95% 上界 | 门槛 | 点在界内 | 上界在界内 | 判定 |
|---|---|---|---|---|---|---|---|
| cpu | +3.435% | [+2.673, +4.160] | +4.065% | ≤5% | 是 | 是 | PASS |
| p95 | +5.233% | [+4.433, +6.030] | **+5.906%** | ≤5% | 否 | 否 | **FAIL** |
| p99 | +7.052% | [+4.844, +9.514] | **+9.067%** | ≤5% | 否 | 否 | **FAIL** |
| tput | +3.298% | [+2.574, +3.976] | **+3.889%** | ≤3% | 否 | 否 | **FAIL** |

**总判定：`LEGACY_D2_NEGATIVE_CONTROL_FAIL`**（B_R2 相对 A_R2 在旧 W-02 D2 形状退化）。
绝对量级：A CPU/op 4850.41 ns / B 5015.83 ns；A tput 205,994 / B 199,155 ops/s；
p50 +2.294% [+1.807,+2.786]、rss +0.032%。
与 R2 归档历史（CPU +3.73/tput +3.58/p95 +4.96/p99 +7.07%，仅对照不复用）CI 高度重叠
（`docs/research/595-causal-r2/PROTOCOL.md:126`）。
机制诊断（strace 3×/臂）：A/B 各 60,004 enter、SQE/enter=1.000、to_submit 直方图
`{1:180012}` 完全相同——**D2 形状无可合并提交**，退化来自门链检查与 flush 后移，
不是批量收益缺失（`measurement-legacy/diag-r2/RECEIPT.md`）。
**该结论只描述旧 B_R2 机制，不得外推到 R3 B。**

### 2.2 PROT-W02-D3（`W02-uring-4k-d3`）——PASS

| metric | mean | CI95 | 单侧上界 | Holm |
|---|---|---|---|---|
| cpu | **−7.327%** | [−8.104, −6.541] | −6.673% | reject |
| p95 | −12.795% | [−14.725, −10.359] | −10.832% | reject |
| p99 | −11.511% | [−16.212, −6.390] | −7.288% | reject |
| tput | −7.975%（=B 吞吐更高） | [−8.897, −7.058] | −7.207% | reject |

绝对量级：A 4355.55 ns / B 4035.45 ns；tput A 229,372 / B 247,603 ops/s。
enter 诊断：A 60,000×1；B 40,005 纯 submit，直方图 {1:20,014, **2:19,995**}——旧 B 在
D3 确实合发。

### 2.3 PROT-W02-D16（`W02-uring-4k-d16`）——PASS

| metric | mean | CI95 | 单侧上界 | Holm |
|---|---|---|---|---|
| cpu | **−28.111%** | [−28.702, −27.527] | −27.613% | reject |
| p95 | −32.590% | [−33.479, −31.652] | −31.819% | reject |
| p99 | −32.400% | [−33.907, −30.971] | −31.179% | reject |
| tput | −39.241% | [−40.395, −38.098] | −38.264% | reject |

绝对量级：A 2982.92 ns / B 2143.56 ns；tput A 335,222 / B 466,585 ops/s。
enter 诊断：B 11,289 纯 submit，直方图 {1:3,799, 7:3,747, 8:3,747}。
跨 SHA 只并列不相减（PROV-5）：本节数字与 §4 诊断数字**不得**互相配对。

## 3. R3 三臂主门与交叉形状——**未测（BLOCKED）**

| 块 | 状态 |
|---|---|
| MAIN-W03-D16（B−A0 主门：点 ≤−10%、上界 ≤−5%、tput ≤3%、p95/p99 ≤5%） | **NOT_RUN / BLOCKED_SEMANTICS_NOT_READY**（无 24-triplet 数据；`measurement-main/` 仅有 `aa/`） |
| B−A1 / A1−A0 配对统计 | 同上（A1 数据仅存在于诊断窗，禁止进入配对统计） |
| CROSS-W03-D4 / D64（A1/B） | 同上 |
| W03 d4 / d64 **A0 基线**（enabledBlocks `SAFE-A0-W03-D4-D16-D64` 的 4k d4/d64 部分） | **NOT_RUN**（从未执行、未记回执；偏差 D-7）。d16 部分已由 AA-W03-D16 的 48 次 A0 invocation 覆盖 |
| SAFE-A0-W02-D16（R3 W-02 d16 A0 descriptive） | **NOT_RUN**（偏差 D-7） |

## 4. 机制诊断窗（带仪器；**不是性能证据**；`raw/mechanism/`）

### 4.1 enter / SQE / CQE 对账（9 run；本 session 独立重算一致）

| run | 纯 submit enter | to_submit 直方图 | data SQE | CQE(4096) |
|---|---|---|---|---|
| d16-A0 | 60,000 | {1:60,000} | 60,000 | 60,000 |
| d16-A1 | 60,000 | {1:60,000} | 60,000 | 60,000 |
| d16-B | **29,225（−51.29%）** | {1:24,826, 2:1, 3:2, 6:1, 8:4,395} | 60,000 | 60,000 |
| d2pos-A0/A1 | 6 | {1:6} | 6 | 6 |
| d2pos-B | **3** | {2:3} | 6 | 6 |
| d2serial-A0/A1/B | 60,000 | {1:60,000} | 60,000 | 60,000 |

ret==to_submit（零部分提交）；每 invocation +4 次 statx 探针 enter；driver 纯 submit=0；
`io_uring_defer=0`；B d16 `to_submit>1` enter=4,399、max=8≤cap；机制存在性门（≥50% enter
减少）满足；量级与 census 预测（91.5–93.3%）不符（见 `READY_EXPOSURE_COMPLETION.md` §4.1）。

### 4.2 PMU（root u+k；每臂 1 次带仪器运行；无 multiplexing，全部 100.00%）

| 事件 | A0 | A1 | B | B−A0 |
|---|---|---|---|---|
| cycles | 854,149,299 | 852,427,314（−0.2%） | 733,912,392 | **−14.1%** |
| instructions | 709,777,889 | 745,464,639（+5.0%） | 651,993,702 | −8.1% |
| cache-misses | 1,611,585 | 1,616,737 | 1,651,593 | +2.5% |
| branch-misses | 1,548,700 | 1,613,861 | 1,438,557 | −7.1% |
| context-switches | 20,090 | 23,125（+15.1%） | 14,830 | −26.2% |
| IPC | 0.831 | 0.875 | 0.888 | — |

A1 指令数 +5.0% 而 cycles 持平（IPC 变化）——**A1 适配器/入口簿记成本的方向性信号**，
只登记不解释；正式 B−A1/B−A0 归因需 §3 的未运行三臂块。

### 4.3 CPU 分桶（3 run/臂；同进程份额，不相加）

| 臂 | main(submitter) ticks | driver | io-wq | bench cpu_s（进程口径） | ru_total s | 未归因 ticks |
|---|---|---|---|---|---|---|
| A0 | 13.7 | 0.8 | 7.3 | 0.0700 | 0.275 | +5.7 |
| A1 | 12.3 | 0.3 | 6.3 | 0.0652 | 0.257 | +6.7 |
| B | 11.0 | 0.0 | 6.0 | 0.0573 | 0.222 | +5.2 |

最不利端登记：整 invocation CPU 上界 = ru_total（A0 0.275s / B 0.222s），下界 = bench
进程 cpu_s（0.0700/0.0573s）。IRQ（cpu2 增量 A0≈303/B≈258，整机上界）不可归因到本
workload，不进入臂间差异。这是**诊断口径**，不是 §3 主门证据。

### 4.4 D2 正控 / 负控（机制形状）

见 `READY_EXPOSURE_COMPLETION.md` §4.3：正控**相位粒度 PASS**（B 每 phase 1 次
`to_submit=2`）；负控**诊断口径 PASS**（三臂 enter/SQE/CQE/defer/eventfd 逐项相同）。
性能侧（≤5%/≤3%）BLOCKED 未运行。

## 5. 保护族与资源门（`measurement-negative/protection/`）

| 块 | 判定 | 说明 |
|---|---|---|
| PROT-W02-D3 / PROT-W02-D16 | **PASS**（R2 同 SHA；§2.2/§2.3） | 不是 R3 B 结果 |
| PROT-W03-D4 / D16 / D64-B | BLOCKED_SEMANTICS_NOT_READY | 未运行 |
| PROT-64K-D16 | BLOCKED_SEMANTICS_NOT_READY | A0 基线：3 invocation，6,000 纯 submit 全 `to_submit=1`（64 KiB 不合批，符合静态审计预期）；CPU 12,351.55 ns/op（descriptive） |
| PROT-THREADPOOL | BLOCKED_SEMANTICS_NOT_READY | A0：d4/d16/d64 各 3 invocation rc=0；workload 相位纯 submit=0（每 invocation 1 次探针 enter，偏差 D-5）；线程峰值 6 |
| PROT-SAT | NOT_ESTABLISHED（NO_W02_R3_ARM + 未在 enabledBlocks） | A0 诊断：9,600 纯 submit 全 `to_submit=1`，CPU 3,338.31 ns/op（descriptive） |
| PROT-PLACEMENT（A0-only 代表检查） | NOT_ESTABLISHED（safety-only） | cpu2 3,153.54 ns/op；cpu8 −0.10% CPU；**SMT{2,12} CPU +145.5%、p99 +65.0%；xphys{2,8} CPU +166.6%、p99 +75.8%**——双核放置对 A0 本身大幅变差，无法做任何 R3 B 放置推断 |
| RESOURCE（B 正式资源门） | NOT_ESTABLISHED（BLOCKED） | 正式门未运行；诊断方向见 §6 |

## 6. 内存 / 线程 / 队列 / timer / 分配（诊断口径；正式门 NOT_ESTABLISHED）

| 项 | A0 | A1 | B | 结论 |
|---|---|---|---|---|
| `/usr/bin/time -v` 峰值 RSS | 5,204 KB | 5,268 KB | 5,252 KB | B **+48 KB（+0.92%）** vs A0（门 +2% 内；非按请求增长） |
| malloc 调用 / 字节 | 8,796 / 6,531,664 | 8,802 / 6,535,504 | 8,802 / 6,535,503 | B==A1；vs A0 +6 次（适配器 ready/accepted 向量，一次性）；60,000 ops 无每请求分配 |
| free / calloc | 8,840 / 5 | 8,846 / 5 | 8,846 / 5 | 一致 |
| clone3（线程创建） | 4 | 4 | 4 | 无新增线程；`/proc` 峰值线程 3/3/3 |
| timer_create / timerfd_create | 0 / 0 | 0 / 0 | 0 / 0 | 无新增 timer |
| io_uring_setup / register / eventfd2 | 4 / 12 / 4 | 4 / 12 / 4 | 4 / 12 / 4 | 无新增队列/通知 |
| mmap / munmap / brk | 82 / 17 / 18 | 82 / 17 / 18 | 82 / 17 / 18 | 一致 |
| 自愿/非自愿 csw | 13,895 / 9,182 | 16,533 / 10,936 | 9,406 / 5,218 | B 最少（A1 最多——与 PMU csw +15.1% 同向） |
| valgrind 512/1024 分配斜率 | NOT_RUN | NOT_RUN | NOT_RUN | 冻结 bench 无 request-count 旋钮（prereg §10.2 子门无法执行，登记 NOT_RUN） |

静态审计：批量入口只用调用者 span、栈上固定 `std::array<Request,8>`、既有
dispatch/ledger/prepared/router，热路径无容器增长/`new`/新线程/timer/queue
（`raw/protection/RESOURCE-STATIC-AUDIT/audit.txt`；`BUILD_COMPLETION.md` §6）。
Python `wait4` RSS 交叉核对**方法无效**（posix_spawn CLONE_VM|CLONE_VFORK 污染
ru_maxrss），以 `/usr/bin/time -v` 为准。

## 7. R2 同 SHA 复现的来源（PROV-4）

- 树 `/home/jnhu/sluice-595-e5/worktree`：HEAD `271b81bf…`，`git diff HEAD` sha256
  `b006dbbf…`（= R2 `PROTOCOL.md:20-22` 目标），`git apply --reverse --check patch.diff`
  exit 0，patch sha256 `7be9d014…`。
- bench 二进制 `32beabcd891801eb65dab1d6e2df72aa0b89a98a2c16a114d398527cb821589a`
  （与 2026-11-04 R2 轮同一二进制；构建 up-to-date 未重编译）；`uring_595_bounded_progress_test`
  `865de828…`（`SLUICE_595_ARM=B` → `all checks passed`）。
- 原始：`raw/calibration/build/`、`measurement-main/aa/R2_PROVENANCE.md`。
- `causal-r2/worktree`（diff `f939602f…`、reverse-check exit 1）被明确排除，未用作 R2 目标树。

## 8. 空表检查与数据完整性

- 现存表全部非空：`measurement-main/aa/{aa-w03-d16,aa-w02-d2}`、`measurement-legacy/*`、
  `measurement-negative/protection/*`、`measurement-mechanism/*`、`measurement-product/HOLD.md`
  （独立复核 `review_tables_out.json` `emptyTables=false`；raw↔evidence 镜像数值一致，
  本 session 抽验 `aa-w02-d2` 镜像逐字节相同）。
- `analysis/COMPLETION_GATE.json` **缺失**（偏差 D-1）→ 按 prereg §7.2 本 run 不得声称
  completion；两个 enabled A0 基线块未执行未记 NOT_RUN（D-7）。
- 240/240 计时 invocation exit=0、attempt=1、每 invocation 5 rep × 20,000 ops、oracle
  全过；无离群点剔除、无 A/A 修复。
- 独立统计复核（`measurement-analysis/`）与执行员数值全部在 1e-9 内一致；机制/PMU/alloc/
  bucket/resource 由复核员从原始输出重算一致；本 session 再抽验 d16-B/d2pos/d2serial
  enter 重算与 PMU cycles/instructions 逐项一致。

## 9. 本轮未测单元（全部）

R3 B/A1 的全部正式性能与保护块（W03 d16 三臂 24-triplet、d4/d64、64K、threadpool、
SMT/xphys placement、W02 R3、saturation、RESOURCE-B 正式门）、d2pos 的 3 次独立
invocation 正控、valgrind 分配斜率子门、R3 B 在 W-02 的任何测量、#604 真实应用、
TSan/ASan、no-lib profile 完整语义套件、observer-armed 批量交错、data+cancel-control
混合 partial submit 脚本化（见 `BUILD_COMPLETION.md` §11 与 `PROTOCOL.md` §7）。
