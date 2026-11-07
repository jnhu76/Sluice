# #595 R3 续跑 — 就绪暴露复用与本轮核对（READY_EXPOSURE_COMPLETION）

| 字段 | 值 |
|---|---|
| 角色 | R3 完成轮裁决与交付员 |
| 前轮普查（复用输入） | `/home/jnhu/sluice-595-e5/r3/evidence/READY_EXPOSURE.md`，sha256 `0d0365c203ef1f6f28828ff636a03cb1a5c4b243fb1f9a16c13828fb6e0e2b3d10`（本 session 实测）+ `census.json` sha256 `1f28e7b0…` |
| 复用条件 | 普查针对 baseSha 冻结基线二进制（W03 `929bc12f…`）；completion patch 修改了 `bench/bench_w03_external_loop.cpp` 的臂选择但 **case 形状/fixture/输入顺序不变**（arm 只改提交路径），因此分布结论按 prereg §5.1 复用，enter 绝对数由本轮机制窗重新实测 |
| 本轮核对 | 机制窗 9 run（d16/d2pos/d2serial × A0/A1/B，bpftrace 21 probes、0 lost）；BUILD §8.4 smoke；本 session 独立重算 |

---

## 1. 前轮普查结论（复用；引 `READY_EXPOSURE.md` §10）

| consumer | 稳态组 | 分类 |
|---|---|---|
| `bench/bench_w03_external_loop.cpp`（W-03） | 每次 front-ready 补发 burst，大小 1..depth | **REAL_EXPOSURE**（合成 benchmark，非 #604 准入真实应用） |
| `bench/bench_w02_pipeline.cpp`（W-02） | 仅初始填充；稳态每次 1 条 | **STARTUP_ONLY** |
| `apps/sluice-copy/copy_task.cpp` | 仅预填；默认 depth=1 无组 | 默认 **NO_REAL_BATCH_EXPOSURE**；depth>1 **STARTUP_ONLY** |

无造组（未新增等待/前移依赖/追加预取；产品库逐字节未变）；R2 的 owner-gated deferred-flush
不会捕获 W-03 的组（submitter 不是 progress driver）。

## 2. 两个原始时间口径（934 ns / 63 ns；`READY_EXPOSURE.md` §2.2/§10.4）

d16 稳态 group 2（`raw/census/canonical/w03-d16-verbose.tsv`，512 请求 verbose 窗口）：

- 该组 `group_ready` → 首个 `submit_begin` = **934 ns**；
- 最后一个 `member_ready` → 首个 `submit_begin` = **63 ns**；
- 5 个 verbose 运行合计 3,572 组，last-member→first-submit 间隔 **32 ns–9,771 ns**，0 次反序。

含义：**组在首条提交前已完全 ready**（最后成员 ready 距首提交仅 63 ns 中位量级），
组不是等待/预取造出来的；934 ns 是整组 ready 到首提交的准备间隔。这两个数字是
syscall/时戳诊断口径，不是性能声明。

## 3. 前轮 enter 基线与预测（`READY_EXPOSURE.md` §3/§10.5）

- 冻结基线 W03 d16/d4/d64、W02：每 20k×3 phase **60,004** 次 `io_uring_enter`
  （`strace -c` 直接计数）；纯 submit/探针 60,000+4 的分解来自 512 行级 trace + 工作量推断，
  **不是 20k 逐调用测量**（原文口径声明）。
- 净 enter 节省预测（按现有容量重切）：d16 ph1/2/3 = **91.51/92.45/93.35%**；
  d4 = **72.28/73.19/72.67%**；d64（512 窗口）= **98.4/98.0/98.2%**；W02-D2 = **0.005%**
  （20k 仅省 1 enter）；sluice-copy 默认 0。

## 4. 本轮机制核对（实测；诊断口径，非性能）

### 4.1 d16 主形状（`raw/mechanism/enter/d16-*`；本 session 独立重算与执行员一致）

| 臂 | 纯 submit enter | to_submit 直方图 | bench r3 计数（timed phase） |
|---|---|---|---|
| A0 | 60,000 | {1:60,000}（+4 探针） | —（A0 无 adapter 计数） |
| A1 | 60,000 | {1:60,000} | `entry=20000, singles=20000, groups=0, prefix_max=1, flush=20000` |
| B | **29,225** | {1:24,826, 2:1, 3:2, 6:1, **8:4,395**}；max=8 | `entry=10515, groups=1355, singles=9160, accepted=20000, prefix_max=8, suffix_single=9160, untouched=9160, flush=10515, gate_stops=0, ineligible=0, rejected=0` |

- **B 相对 A0 enter 减少 51.29%**（机制存在性门 ≥50% 满足；prereg §9.3）。
- 4 个 `to_submit=1,mc=1,GETEVENTS` enter 是每次 invocation 的 statx 可用性探针；ret==to_submit
  零部分提交；driver 线程纯 submit=0（9/9 run）。
- **量级差异（如实登记，D-12）**：census 预测 91.5–93.3%，实测 51.29%。原因：census
  `d16_full` 假设整窗 16 条 ready；实测稳态补发窗口 ready 多为 1–2 条（24,826 个单条 enter
  主导），且 **cap=8** 使每组至多 8 条进批。census 预测不得引用为实测。

### 4.2 cap/suffix 对账（B 冻结策略：cap=8、suffix 逐条 span=1、不二次成组）

- 1,355 组 × 前缀 8 = 10,840 SQE；9,160 单成员 + 9,160 suffix 单条 = 20,000 accepted；
  `untouched(9,160) == suffix_single_calls(9,160)`（每个被 cap 截断的成员都逐条补交）；
  `flush_attempts(10,515) == entry_calls(10,515)` → 每次入口调用恰一次 flush，
  无跨调用持有 SQE、无 defer；`io_uring_defer` 全 run = 0。
- cap 截断证据：to_submit 直方图无 >8；8 成员 enter 4,395 次 ≈ 组数 1,355 × 3 phase + 边缘组。
- SQ 容量（queue_depth=16）理论上允许 8+8 合并，B 的冻结策略把后 8 条留作 suffix 逐条，
  观测与之自洽（观测为设计结果，不是内核限制）。

### 4.3 D2 两 ready 正控（d2pos）与 D2 单条负控（d2serial）

| case | A0 | A1 | B |
|---|---|---|---|
| d2pos（2 条同时 ready） | 6 纯 submit，全 `to_submit=1` | 同 A0 | **3 纯 submit，全 `to_submit=2`**；`group=1, accepted=2, prefix_max=2, untouched=0, flush=1`；6 data SQE/CQE(4096)；oracle ok |
| d2serial（单条补发） | 60,000×`1,0,0`+4 探针 | 同 A0 | 与 A0 完全一致；`group=0, prefix_max=1, suffix=0, untouched=0, flush=20000`；`io_uring_defer=0`；eventfd2=4、setup=4、register=12 三臂相同；timer_create/timerfd_create=0 |

- 正控判定：**相位粒度 PASS**（1 invocation × 3 相位，非 prereg §5.5 的 3 次独立 invocation，
  偏差 D-3）。
- 负控判定：机制形状 **PASS（诊断口径）**——B 在单条补发下不合发、不 defer、无 residue、
  无新增 eventfd/timer/queue；性能侧（CPU/p95/p99 ≤5%、tput ≤3%）**BLOCKED 未运行**。

### 4.4 smoke 复核（BUILD_COMPLETION §8.4；非计时）

- d2pos：A1 `entries=2 groups=0 singles=2`；B `entries=1 groups=1 accepted=2 prefix_max=2 flushes=1`。
- d16：A1 `entries=20000 singles=20000`；B `entries=11129 groups=1277 singles=9852 accepted=20000
  prefix_max=8 suffix_singles=9851 untouched=9851`（计数受调度影响，非常量）。
- 全 case `failing repetitions=0`、byte oracle 通过；默认 profile（threadpool）B 回落
  `ctx.submit_read`，`entries=0`、rc=0。

## 5. 64 KiB / ThreadPool / 不具资格形状

- 64 KiB（`W03-uring-64k-d16-r3d16-64k`，实际 id 与 prereg 差 `-r3d16-64k` 后缀，D-4）：
  A0 基线 3 invocation，6,000 纯 submit 全 `to_submit=1`；`batch_eligible_read = len==4096`
  使 64 KiB 不入批（静态审计 `src/async/uring_backend.cpp:437-439`）→ B 必须回落 immediate，
  enter 数不得减少（B 侧 BLOCKED 未运行）。
- ThreadPool（d4/d16/d64）：每 invocation 恰 1 次 enter（statx 探针），workload 相位纯
  submit=0（B 侧 BLOCKED；字面门 `=0` 偏差 D-5 已披露）。
- W-02（R3 树）：无 `SLUICE_595_R3_ARM` 分支、无 D2 case（patch 不触碰
  `bench_w02_pipeline.cpp`）→ R3 B 在 W-02 上 `NOT_ESTABLISHED`，只能用 R2 同 SHA
  A_R2/B_R2 复现回答（见 `RESULTS.md` §6）。

## 6. 不得外推

1. enter/SQE/CQE 计数不是时延或加速声明；机制窗全部带仪器。
2. 只覆盖 4 KiB（+64 KiB 负形状）、warm tmpfs、本机 uring backend；不覆盖块设备、cold
   cache、O_DIRECT、其他 block size、threadpool 正式门、并发 submit。
3. d16 稳态 ready 分布受调度影响（单条 enter 主导是本轮实测形状）；census 的 phase 比例
   不得当常量引用。
4. W-03 是合成 benchmark；其暴露/收益不能替代 #604 真实应用产品门。
