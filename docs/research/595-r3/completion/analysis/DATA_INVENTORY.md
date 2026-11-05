# DATA_INVENTORY — #595 R3 完成轮 论文式分析的数据准入

生成（UTC）：2026-11-05T13:5x（分析 session；全部条目由 `analysis/scripts/recompute_stats.py`
从 `raw/` 原始记录独立重算，见 `analysis/NUMERIC_CROSSCHECK.json`）。

证据根：`/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z`
预注册：`evidence/PREREGISTERED_R3_COMPLETION.md`
sha256 `1a9c1e12c33a2f50e3bfac957c2ab25592d77e3ffae808dd7f92f08e16af3ab4`，
计时前冻结（frozenBeforeTiming=true），**semanticsReady=false 全程未翻转**。

## 1. 源码、二进制与环境（逐单元 SHA）

| 单元 | 树/补丁 | 二进制 (sha256 前 12) | 用途 |
|---|---|---|---|
| R3 主形状臂 | `r3/worktree` @ `0a22a791` + `r3-completion.patch`（sha256 `2596f204…`，reverse-check exit 0；worktree diff sha `c1a31a30…`） | `bench_w03_external_loop` = `0ebfb7f0c204` | AA-W03-D16（A0/A0）、机制窗 A0/A1/B、SAFE-A0-W03-* |
| R3 W-02 基线 | 同上 | `bench_w02_pipeline` = `4e719b1e4dc2` | SAFE-A0-W02-SAT-d16（仅 A0；该树**无 R3 研究臂/无 D2 case**） |
| R2 复现臂 | `/home/jnhu/sluice-595-e5/worktree` @ `271b81bf` + R2 patch `7be9d014…` | `bench_w02_pipeline` = `32beabcd8918` | AA-W02-D2（A/A）、R2-D2（A/B）、PROT-W02-D3/D16（A/B） |

- 每个 invocation 的 `before/after` 快照都核验 `binary_sha_ok`；arm 经
  `SLUICE_595_R3_ARM`（R3）/`SLUICE_595_ARM`（R2）并由 bench 读回（readback）核对。
- 平台：E5（Xeon E5-2666 v3，10C/20T，NUMA node0），Fedora 44 kernel
  `7.2.11-200.fc44.x86_64`，governor=schedutil（cpu2 1.2–3.5 GHz），`taskset -c 2`
  （SMT 兄弟 cpu12 同批征用），存储=warm tmpfs `/tmp`（32 GiB，每轮记录 df），
  20,000 valid reads/rep，WARMUP=1 MEASURED=5。全部计时持
  `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`（flock 回执在 raw/*/lock-log.jsonl）。
- **不混样**：每个配对块单一二进制 SHA、单一树；跨树只有 R2 同 SHA 复现块
  （A_R2/B_R2），不与 R3 臂相减（prereg PROV-5：跨 SHA 只并列不相减）。

## 2. 实验臂定义（与 OPEN1_RECEIPTS.md 一致）

- **A0 = IMMEDIATE**：冻结 baseSha `0a22a791` 的 W-03 external-loop 原始逐条补发循环
  （按原位置构造 ReadOp → 立即 `ctx.submit_read`；不构造 ready span、不预扫描、不分配组缓冲）。
- **A1 = ADAPTER_IMMEDIATE**：与 B 相同的 ready-group 准备与输入，经同一 Context 内部
  研究入口逐条（span=1）提交；不直接调 backend seam。
- **B = READY_AWARE**：合法 ReadyGroup 可用时按 min(ready, cap=8, SQ, capacity, budget)
  选前缀，一次入口调用内 accept→prepare→dispatch→一次 flush；suffix 逐条 span=1，不二次成组。
- **A_R2 / B_R2**：R2 树（271b81bf+R2 patch）的 IMMEDIATE / BOUNDED_PROGRESS（历史
  defer 策略）。**B_R2 与 R3 B 是不同算法**，全部 B_R2 结果只回答"旧策略"问题，不得外推 R3 B。

## 3. 主形状与正负控

- **主形状**：W-03 d16（`W03-uring-4k-d16`：64 MiB tmpfs、4 KiB positional reads、
  queue_depth=16、app-owned epoll 外部循环）。W-03 是合成 benchmark 的**代表调用者**
  （不是 #604 真实应用；后者单独准入，见 §9）。
- **负控**：W-02 D2 单条补发（`W02-uring-4k-d2`，R2 树，B_R2−A_R2 同 SHA 同 session）。
- **正控**：D2 两条同时 ready（d2pos，仅机制诊断窗：1 invocation × 3 phase）。

## 4. 配对块、有效性、剔除

| 块 | 臂 | case | n 对（有效/计划） | invocation 剔除/替换 | 判定 |
|---|---|---|---|---|---|
| AA-W03-D16 | A0 vs A0 | W03-uring-4k-d16 | 24/24（48/48 invocation 有效） | 0（无 attempts.jsonl） | PASS（hw 0.797/0.821/2.039 pp） |
| AA-W02-D2 | A_R2 vs A_R2 | W02-uring-4k-d2 | 24/24（48/48） | 0 | PASS（hw 0.862/0.853/1.420 pp） |
| LEGACY-W02-D2 (R2-D2) | B_R2 vs A_R2 | W02-uring-4k-d2 | 24/24（48/48） | 0 | **FAIL**（负控复现） |
| PROT-W02-D3 | B_R2 vs A_R2 | W02-uring-4k-d3 | 24/24（48/48） | 0 | PASS |
| PROT-W02-D16 | B_R2 vs A_R2 | W02-uring-4k-d16 | 24/24（48/48） | 0 | PASS |

240/240 计时 invocation 有效（自算：measured=5、warmup=1、ok、ops=20000 逐条核验，
与 results.jsonl valid 标志 0 不一致；repairUsed=false——无测量条件修复）。

## 5. A0-only 参考基线（单臂、无配对；只作绝对参考）

SAFE-A0-PLACEMENT-{cpu2,cpu8,smt-2-12,xphys-2-8}（各 8 rep）、SAFE-A0-W03-64K-D16（3）、
SAFE-A0-W03-THREADPOOL-{d4,d16,d64}（各 3）、SAFE-A0-W02-SAT-d16（3，R3 W02 bench
`4e719b1e`）、DIAG-R2-{D3,D16}-{A,B}（各 1）、DIAG-64K-A0（1）、DIAG-THREADPOOL-NOCASE
（1，case 不存在的对照）。全部 exit=0。

## 6. 机制诊断窗（带仪器，禁入门判定）

9 个 enter 追踪 run（d16/d2pos/d2serial × A0/A1/B；bpftrace attach 21 probes、
0 lost events）；PMU 每臂 1 run（root u+k，time-running=100.00 无 multiplexing）；
CPU 分桶（20 ms 逐 tid 采样，A0 6/A1 3/B 3 run）；alloc uprobe、`time -v`、strace。

关键数（本 session 从 raw 独立重算，与 MECHANISM_SUMMARY.md 逐项一致）：
- d16 纯 submit enter：A0=60,000、A1=60,000、**B=29,225**（−51.29%；直方图
  {1:24826, 2:1, 3:2, 6:1, 8:4395}）；探针 enter（ts=1,mc=1,fl=1）每 run 恰 4。
- bench r3 计数（d16 B，timed phase）：entry=10,515=flush、group=1,355、single=9,160、
  suffix=untouched=9,160、accepted=20,000、prefix_max=8。
- d2pos：A0/A1 各 6 enter×to_submit=1；**B=3 enter×to_submit=2**（group=1、accepted=2）。
- d2serial：三臂 enter/SQE/CQE 逐项相同（60,000×1），B 不合发、无 defer、无 residue。
- PMU cycles：A0 854,149,299 / A1 852,427,314 / B 733,912,392（−14.1%）；instructions
  +5.0%（A1）→B −8.1%；context-switches A0 20,090 / A1 23,125 / B 14,830。
- 峰值 RSS（time -v）：5,204 / 5,268 / 5,252 KB；malloc 调用 8,796 / 8,802 / 8,802
  （B vs A0 +6 次一次性）；clone3=4、线程峰值=3、timer/timerfd=0、io_uring_setup=4 三臂相同。

## 7. CPU 口径与覆盖

- 配对块主指标 `cpu_ns_per_op` = bench `cpu_s`（CLOCK_PROCESS_CPUTimeID 进程口径，
  **包含 `iou-*` io-wq 内核线程**，分桶证明它们在 `/proc/<pid>/task` 内）÷ ops。
  `ru_total`（wait4 rusage）作交叉旁证；两者不重复计入同一指标。
- 分桶（诊断）：main/driver/io-wq 为同一进程 CPU 的份额，不相加成第二总量；
  未归因残差 +5.2…+6.7 ticks（10 ms tick 粒度）；IRQ 只能记整机上界
  （cpu2 增量 A0≈303/A1≈273/B≈258，主要为 timer/IPI，tmpfs 无块设备 IRQ）。
- PMU 为 root u+k 全进程口径、单次带仪器运行。

## 8. 正确性与变异（语义链状态）

- 26 场景确定性套件 BASE GREEN（rc=0）；3 命名变异体 RED（rc=1（17 断言失败）、
  rc=1（9 断言失败）、rc=134 router-exhaustion fail-fast）；干净 builddir 强制重建
  逐位相同二进制（`c54a9222…`）并 GREEN（`raw/mutants/session.log`，持锁）。
- **但**：预注册 §14.3 要求的独立 verifier 回执（`A0_AND_SUFFIX_RECEIPT.md`、
  completion `correctness/` 目录）未产生 → `semanticsReady` 未翻转 →
  全部 R3 B/A1 正式性能门 BLOCKED（见 MISSING.md）。

## 9. #604 真实应用准入

`gh issue view 604` + comments：无 `READY_FOR_595` handoff（0 命中）、无已准入真实
应用、无产品 harness 入口 → 未测任何真实应用；产品裁决
HOLD_NO_REAL_APPLICATION（`evidence/measurement-product/HOLD.md`）。

## 10. 缺失数据一览

见 `analysis/MISSING.md`（enabled 未执行块、全部 BLOCKED B 门、诊断不足项）。
本分析**不引入任何理论重切分或历史数值**充当本轮性能数据；未测单元一律
NOT_RUN/INCONCLUSIVE，不显示为零。
