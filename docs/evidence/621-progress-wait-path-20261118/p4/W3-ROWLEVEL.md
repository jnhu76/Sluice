# #621 P4 W3 行级归因剖析（profile session）

- 日期：2026-11-18
- BASE：`5d94f233967c3aef993eb19ba9e3252c532b0951`（`git rev-parse HEAD`；采集前后 `git status --porcelain` 仅三个未跟踪 evidence 目录）
- 性质：**profile session / 归因**。不是 A/B、不是收益证明、不是 clean measurement；本文任何“份额”都不是可直接删除的 cycles。
- 原始数据根：`docs/evidence/621-progress-wait-path-20261118/p4/profile/`
- 本文件：`docs/evidence/621-progress-wait-path-20261118/p4/W3-ROWLEVEL.md`
- 采集纪律：严格串行，一个 perf 命令结束才启动下一个；所有 perf/root 命令经 `sudo -S -p '' <命令> < /tmp/sluice621-sudo`（stdin 重定向，不回显、不读取、不记录内容）。未修改产品代码；`docs/evidence/621-round2-R1/` 与 `docs/evidence/e5-fullpath-machine-work/` 全程未触碰。

> **读数口径（先读）**
>
> 1. **self**：采样 leaf 的“实际所属函数”就是候选函数（候选函数自己的非内联代码；若候选被内联到别处，见 inlined-self）。
> 2. **inlined-self**：采样 leaf 在用户二进制内，且候选函数出现在该 IP 的 inline 链中（候选被内联进另一个函数，例如 `consume_public_result` 内联进 `RequestBridge::consume`）。计为候选自有成本。
> 3. **callsite-extra**：采样 leaf 是 libc/kernel（主要是 `pthread_mutex_lock/unlock`、`lll_lock_*`、`__poll`），候选函数出现在 leaf 的**直接调用者** inline 链中。这是候选调用点的锁/等待成本。
> 4. **owner-total = self + inlined-self + callsite-extra**。这是本文的“候选级 measured 成本”主口径。
> 5. **inclusive**：候选出现在任意调用帧（含更深的 callee），会与 owner 重叠；**不可与 owner 或其它候选相加**。候选之间也可能重叠：例如 `consume_public_result` 内联进 `RequestBridge::consume`，二者的 callsite 是同一批样本；不要把候选 owner/ family 数字相加。
> 6. 份额分母默认 `app`（bench 进程/线程，排除 `iou-wrk`）；另给 `all`（app + `iou-wrk`）以便和 p2 的 all-comm 表对照。P-D/P-E 无 iou-wrk，两者相同。
> 7. “阶段容器”（stage container）是 p2 R1 的 8+1 阶段自份额；它包含阶段函数调用栈内的 kernel/sync/callee 成本，**不是**候选函数自身的代码量。本文用 candidate-family union 与 stage 对照，说明二者不是一回事。
> 8. 判定阈值（本文显式定义，只为归因分级）：**candidate-level measured hotspot = owner-total ≥ 1.00% app 且 owner 样本 ≥ 40**；0.30–1.00% 为 below-threshold；0.10–0.30% 为 marginal；<0.10% 或 0 样本为 not observed。阈值不是收益门，也不是可删量。

---

## 1. 会话、二进制、形态与采集

### 1.1 二进制（均为 p4 DWARF 构建，`-m release --cxflags=-g --ldflags=-g --policies=build.release.strip:n`）

| 形态 | 二进制 | sha256 | DWARF |
|---|---|---|---|
| P-D / P-F / d16 control | `/tmp/621-p4-build/ring/linux/x86_64/release/bench_w02_pipeline` | `e65a5766c08af7657ba2fe4d7957b2ba9d8e875fe29fc0b69edca316ca2eb9b8` | `.debug_info` / `.debug_line` 存在；`file` 报 with debug_info, not stripped |
| P-C | `/tmp/621-p4-build/ring/linux/x86_64/release/bench_w03_external_loop` | `5bc33f861c1a7c6f6bfd498bb6a46af1dd2a7815b332aaae02b91d9d6b26ece4` | 同上 |
| P-E | `/tmp/621-p4-build/copy/linux/x86_64/release/sluice-copy` | `959f8090aaeabf863208e9697a9fc0e76d53cc422843026f7d89d604ee593c54` | 同上 |

构建记录与 `.text` 与 P1 release 的逐节相等证据见 `p4/rowlevel-build-manifest.md` §3–§7（`.text`/`.rodata`/`.data` 相同，VMA 不变）。`-m releasedbg` 在本项目未接线（`xmake.lua:3` 只注册 `mode.debug`/`mode.release`/`mode.valgrind`），p4 manifest 因此采用 `-m release --cxflags=-g --ldflags=-g --policies=build.release.strip:n`；任务给出的 `.../release/bench_w02_pipeline` 等路径就是这批 DWARF 二进制。本会话的代码与 p2 的 P1 release 代码一致，差异只在 DWARF/symtab/ELID。**这些二进制仅用于归因，不用于 A/B 或时长比较**。

### 1.2 环境快照

`p4/profile/env/snapshot-before.txt`、`snapshot-after.txt`：

- kernel `7.3.3-200.fc44.x86_64`，perf `7.3.3-200.fc44.x86_64`
- 20 logical CPU（Xeon E5-2666 v3），governor 20/20 `performance`，`no_turbo=0`，`irqbalance=inactive`
- `perf_event_paranoid=2`，`perf_event_max_sample_rate=63000`，`kptr_restrict=0`
- 采集前 loadavg `0.09 0.06 0.08`；采集后 `0.17 0.46 0.41`；采集后 Package 50°C、无节流记录
- `perf probe -l` = 0；无遗留 `perf` 进程

### 1.3 形态、case 与 workload

所有 bench 形态：`SLUICE_BENCH_WARMUP=2 SLUICE_BENCH_MEASURED=7`、`SLUICE_BENCH_CASE_FILTER='=<case>'`、`SLUICE_BENCH_TMP_DIR=/tmp/621-p4-profile-fixture`（tmpfs 64 MiB fixture，page-cache resident）、`taskset -c 2,3`。每次进程 9 轮 × 3 phase（timed / oracle verify / latency）× 20,000 ops = **540,000 ops**。

| 标签 | 二进制 | 精确 case | 形态 | 分母/运行 |
|---|---|---|---|---:|
| P-D' | W-02 | `W02-threadpool-4k-d16-w4` | ThreadPool d16 / w4 | 540,000 ops |
| P-C' | W-03 | `W03-uring-4k-d16` | io_uring app-owned epoll split-owner，d16 | 540,000 ops |
| P-E' | sluice-copy | argv：`--buffer-size 4096 --pipeline-depth 4 --workers 1 --sync none --no-atomic <src> <dst>` | ThreadPool copy，d4 / w1 | 65,536 chunks / 131,076 requests per copy |
| P-F | W-02 | `W02-uring-4k-d64` | io_uring，d64 | 540,000 ops |
| P-F d16 control | W-02 | `W02-uring-4k-d16` | io_uring，d16（同 binary / 同 session，用于 M-53 深度对照） | 540,000 ops |

P-E' 介质：src `/home/jnhu/sluice-621-r2/data/src-256m.bin`（btrfs，sha256 `a1f8dd971b212ae487cad4edf8f2b742bd6b1a8feef92a4c44730bbe167547f3`，`fincore` 256M/65,536 pages cached）→ dst `/tmp/621-p4-profile-copy/dst-256m.bin`（tmpfs）。P-E' 每 run 连续 5 次 copy；两次 run 共 10 次 copy。

### 1.4 采集命令（逐条串行执行）

```bash
# bench 形态（P-D / P-C / P-F / d16 control 同模板，仅换 BIN / case / 输出名）
sudo -S -p '' env SLUICE_BENCH_WARMUP=2 SLUICE_BENCH_MEASURED=7 \
  SLUICE_BENCH_CASE_FILTER='=W02-threadpool-4k-d16-w4' \
  SLUICE_BENCH_JSONL=<raw>/run-01.jsonl \
  SLUICE_BENCH_TMP_DIR=/tmp/621-p4-profile-fixture \
  perf record -o <raw>/run-01.data -e cycles -F 4999 -g --call-graph dwarf \
  -- taskset -c 2,3 /tmp/621-p4-build/ring/linux/x86_64/release/bench_w02_pipeline \
  < /tmp/sluice621-sudo

# P-E'（5 copies/run）
sudo -S -p '' env perf record -o <raw>/run-01.data -e cycles -F 4999 -g --call-graph dwarf \
  -- taskset -c 2,3 sh -c 'for i in 1 2 3 4 5; do rm -f <dst>; \
       /tmp/621-p4-build/copy/linux/x86_64/release/sluice-copy \
       --buffer-size 4096 --pipeline-depth 4 --workers 1 --sync none --no-atomic <src> <dst>; done' \
  < /tmp/sluice621-sudo
```

每个 `perf.data` 的展开与报告：

```bash
perf report -i <data> --stdio --header-only
perf report -i <data> --stdio --inline -g none --no-children \
  -s comm,dso,symbol,srcline --show-nr-samples --full-source-path --percent-limit 0
perf report -i <data> --stdio --inline -g graph,0.5 --percent-limit 0.05 \
  --symbol-filter='<候选符号>'
perf script -i <data> --inline
```

分析脚本 `p4/profile/analysis/analyze_w3.py` 读取 `script-inline-*.txt`，对采样 IP 批量执行 `addr2line -e <binary> -f -C -i -a`，把每个 IP 解析成 inline 链；用链的**最外层**得到候选自身行，用**最内层**得到实际采样指令行。`perf report --inline` 的 `Source:Line` 列作交叉检查；它有时选择最内层 inline 行（例如 `claim_execution` 显示 `request_core.cpp:127`），因此不能单独当作候选自身行。详见 §2.3。

### 1.5 样本量、丢失、开销

| 形态 | run-01 样本 | run-02 样本 | header duration | Lost Samples | app 样本 | iou-wrk | all |
|---|---:|---:|---:|---|---:|---:|---:|
| P-D' | 26,394 | 25,902 | 1,977 / 1,951 ms | 0 / 0 | 52,243 | 0 | 52,243 |
| P-C' | 29,095 | 26,533 | 2,366 / 2,318 ms | 0 / 0 | 35,836 | 19,740 | 55,576 |
| P-E' | 34,755 | 35,721 | 3,766 / 3,884 ms（5 copies） | 0 / 0 | 69,371 | 0 | 69,371 |
| P-F d64 | 16,828 | 16,612 | 1,590 / 1,594 ms | 0 / 0 | 15,874 | 17,516 | 33,390 |
| P-F d16 control | 16,991 | — | 1,535 ms | 0 | 7,654 | 9,311 | 16,965 |

所有 8 个主 run、d16 control、P-E 1-copy overhead 的 `Total Lost Samples` 均为 **0**。原始依据：各 `raw/lost-*.txt`、`raw/header-*.txt`；汇总见 `profile/env/overhead-and-samples.txt`。

**采集开销（同一 measured timed phase 的 wall 比，baseline=1.00）**：

| 形态 | baseline measured wall | run-01 / run-02 | 开销 |
|---|---:|---:|---:|
| P-D' | 61.8 ms/phase | 67.7 / 67.1 ms | **1.10× / 1.09×** |
| P-C' | 82.7 ms/phase | 84.8 / 83.0 ms | **1.03× / 1.00×** |
| P-F d64 | 53.2 ms/phase | 55.2 / 54.9 ms | **1.04× / 1.03×** |
| P-E' | 0.824 s/copy（10 次 baseline 均值） | 0.748 / 0.738 s/copy（perf 内时间戳） | **0.90×（观测），见限制** |

P-E 的 0.90× 不是“perf 加速”结论：P-E 的 per-copy 计时在 perf 内外都受 page cache/写回/调度状态影响，且 header duration 不含进程启动；该比值只说明本次没有观测到明显 slowdown，**不用于任何时长/收益判断**。

**周期估计器**：分析脚本累加每个样本 header 中的 `period`（cycles），除以总 ops/chunks。对照 p2 的 PMU（同一 `.text` 代码）：P-D 21,258.6 cyc/op vs p2 20,386.1（+4.3%）；P-C all 24,254.6 vs 20,324.9（+19.3%，record 扰动较大）；P-E 44,345.0 cyc/chunk vs 44,481.5（−0.3%）。该估计器用于候选的 cyc/op 相对量级，不替代 PMU。

---

## 2. 归因方法与可用性

### 2.1 DWARF / inline 可用性

- 三个 p4 二进制均含 `.debug_info`、`.debug_line`、`.symtab`。`addr2line -f -C -i` 能解析产品源码（例如 `RequestCore::reserve()` → `src/async/detail/request_core.cpp:165`）。
- 每个 case 的**用户二进制 IP 全部解析成功**：P-D 957/957、P-C 997/997、P-E 897/897、P-F 1,050/1,050，unresolved=0。
- `perf script --inline` 在 app 样本中展开 inline 帧的比例：P-D 49,281/52,243（94.3%）、P-C 31,235/35,836（87.2%）、P-E 66,456/69,371（95.8%）、P-F 14,584/15,874（91.9%）。inline 帧行数占全部调用帧行的 15–22%。
- 候选 self 样本中落到 `??:0` 的数量为 **0**（P-D/P-C/P-E/P-F 均为 0）。

### 2.2 leaf 所在 DSO（app 样本）

| 形态 | kernel leaf | libc leaf | libstdc++ | liburing | 用户二进制 leaf | 其它 user |
|---|---:|---:|---:|---:|---:|---:|
| P-D | 38,083 (72.9%) | 10,539 (20.2%) | 67 | 0 | 3,321 (6.4%) | 233 |
| P-C | 26,540 (74.0%) | 5,594 (15.6%) | 32 | 154 | 3,422 (9.5%) | 94 |
| P-E | 50,897 (73.4%) | 13,018 (18.8%) | 58 | 0 | 4,890 (7.0%) | 508 |
| P-F | 11,222 (70.7%) | 1,090 (6.9%) | 31 | 106 | 3,243 (20.4%) | 182 |

这解释了为什么只看 `perf report` 的函数 self 会低估 W3 候选：候选的锁/等待成本大量落在 libc leaf 上，必须通过 inline 链归到调用点。本文的 callsite-extra 就是为此定义。

### 2.3 行级归因的两个层次

`addr2line -i` 对一个 IP 给出从**最内层 inline** 到**最外层实际函数**的链。本文同时记录：

- **candidate-own line**（链的最外层）：候选函数自己的源码行，例如 `claim_execution` 的 `request_core.cpp:369`（调用 `resolve_internal_` 的行）、`offer_terminal` 的 `349`、`reserve` 的 `177`。
- **actual instruction line**（链的最内层）：真正被采样的指令所在行，例如 `claim_execution` 的样本 158/223 落在 `resolve_internal_` 的 `request_core.cpp:127`；`reserve` 的样本落在 `std::vector::pop_back` 的 `stl_vector.h:1450`。
- **callsite line**：当 leaf 是 libc/kernel 时，候选出现在直接调用者的 inline 链中；该 inline 组 IP 的 addr2line 给出候选调用行，例如 `reserve` 的锁行 `166`、解锁行 `182`；`claim_execution` 的 `368/378`；`offer_terminal` 的 `348/365`。

`perf report --inline -s srcline` 的交叉检查示例：P-D 中 `claim_execution` 的一行显示 `Source:Line = src/async/detail/request_core.cpp:127`（最内层 `resolve_internal_`），而本文的 candidate-own line 是 `369`。两者都真实，但含义不同；报告不把 `:127` 当作 `claim_execution` 的函数体行。

**inline 链的多义性**：同一 IP 在 -O3 下可能对应多条 inline 链；`perf script --inline` 与 `addr2line -i` 可能选择不同链。本文以 `addr2line` 的链做行级聚合，并把 `perf script` 的 inline 函数名并入 inclusive/callsite 名称集合；两者不一致时以“最内层实际指令行”为准。P-E `run_pipelined_copy_with_context` 是最明显的例子：candidate-own line 是 `copy_task.cpp:334`（lambda 调用点），但实际指令行分布在 `70/82/94/198`（copy 主循环内联体），见 §5.3。

**同一候选的 self/callsite 拆分可因 TU/codegen 不同而变化**：例如 `offer_terminal` 在 P-D（bench_w02 TU）的锁样本落在 callsite（libc leaf，`365/348`），而在 P-E（sluice-copy TU）有一部分锁指令落在候选自身（leaf=self，行 `365/348`）。因此跨形态比较应使用 **owner-total**，不要比较 self 或 callsite 单项。

### 2.4 单帧样本与 role 判定

单 group（callchain 只有 leaf）的 app 样本：P-D 522/52,243（1.0%）、P-C 249/35,836（0.7%）、P-E 655/69,371（0.94%）、P-F 77/15,874（0.49%）。这些样本的 stage 只能归到 leaf；candidate 归因不受影响（leaf 已足够判断 self），但 inclusive 只能到 leaf。

role（main/worker/driver/submitter）先由调用链中的 `worker_loop`、`driver_thread`、`run_pipeline_phase`、`run_external_phase`、`run_pipelined_copy_with_context`/`run_body`/`wait_take` 判定；未知样本按 TID 的多数 role 回填（同 TID 已知样本 ≥3 且 ≥80% 一致才回填）。`iou-wrk` 不作为 app role。

---

## 3. P-D'：W02 ThreadPool 4k d16 w4

- case：`W02-threadpool-4k-d16-w4`；app 52,243 样本；period 21,258.6 cyc/op app。
- 两次 run 的 leaf top 与 p2 P-D 结构一致（`entry_SYSRETQ_unsafe_stack` 12.78%、`rep_movs_alternative` 8.66%、`pthread_mutex_lock` 7.68%、`pthread_mutex_unlock_usercnt` 7.13%、`entry_SYSCALL_64` 4.68%、`syscall_return_via_sysret` 4.10%），见 `analysis/analysis-w3.json` 的 `leaf_top`；p2 P-D 的同类 top 为 entry_SYSRETQ 13.29%、rep_movs 9.04%、mutex 族合计 13.1%（unlock 7.12% + lock 5.96%）、entry_SYSCALL 4.69%、syscall_return 4.28%。

### 3.1 候选表（app 分母；owner = self+inlined+callsite；cyc/op 为 period 估计）

| 候选 | self% (n) | inlined% (n) | callsite% (n) | owner% | owner all% | inclusive% | owner cyc/op | role |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| RequestCore::offer_terminal | 0.16 (84) | 0.00 | 1.63 (854) | **1.80** | 1.80 | 3.38 | 311 | worker 938 |
| RequestCore::claim_execution | 0.43 (223) | 0.00 | 1.13 (591) | **1.56** | 1.56 | 2.72 | 274 | worker 814 |
| RequestCore::release_execution | 0.09 (48) | 0.00 | 1.05 (548) | **1.14** | 1.14 | 2.47 | 200 | worker 596 |
| RequestBridge::consume | 0.18 (94) | 0.00 | 0.84 (438) | 1.02 | 1.02 | 1.50 | 286 | main 532 |
| RequestCore::consume_public_result | 0.00 | 0.14 (72) | 0.84 (438) | 0.98 | 0.98 | 1.46 | 274 | main 510 |
| RequestCore::reserve | 0.04 (21) | 0.00 | 0.58 (305) | 0.62 | 0.62 | 1.16 | 175 | main 326 |
| RequestCore::accept | 0.08 (43) | 0.00 | 0.44 (231) | 0.52 | 0.52 | 0.78 | 149 | main 274 |
| RequestCore::begin_publication | 0.12 (63) | 0.00 | 0.38 (199) | 0.50 | 0.50 | 0.66 | 140 | main 262 |
| ThreadPoolBackend::poll_progress | 0.13 (68) | 0.00 | 0.24 (127) | 0.37 | 0.37 | 3.06 | 107 | main 195 |
| RequestScope::take | 0.37 (193) | 0.00 | 0.00 | 0.37 | 0.37 | 1.86 | 105 | main 193 |
| AsyncIoContext::wait_one | 0.06 (29) | 0.00 | 0.31 (162) | 0.37 | 0.37 | 5.02 | 104 | main 191 |
| RequestScope::wait_for | 0.30 (159) | 0.00 | 0.03 (15) | 0.33 | 0.33 | 6.89 | 95 | main 174 |
| RequestCore::complete_publication | 0.05 (27) | 0.00 | 0.25 (133) | 0.31 | 0.31 | 0.45 | 87 | main 160 |
| RequestCore::retire_observer_delivery | 0.03 (15) | 0.00 | 0.28 (145) | 0.31 | 0.31 | 0.50 | 87 | main 160 |
| RequestCore::claim_observer_delivery | 0.02 (10) | 0.00 | 0.22 (116) | 0.24 | 0.24 | 0.35 | 69 | main 126 |
| ThreadPoolBackend::publish_one | 0.19 (101) | 0.00 | 0.00 | 0.19 | 0.19 | 2.16 | 56 | main 101 |
| ThreadPoolBackend::deliver_event | 0.00 | 0.07 (37) | 0.00 | 0.07 | 0.07 | 0.92 | 21 | main 37 |

### 3.2 行级证据（app 分母；括号为样本数）

- **reserve**（`src/async/detail/request_core.cpp:165-182`）：self 21，callsite 305，owner 0.62% / 175 cyc/op。self 实际行 `177`(8)、`181`(4)、`165`(4)；callsite 行 **`166`(171, lock)** 与 **`182`(134, unlock)**；leaf `pthread_mutex_lock` 171 / `pthread_mutex_unlock_usercnt` 132。**候选成本几乎全是 mutex 对，不是 free-list 操作。**
- **accept**（`:184-210`）：self 43，callsite 231，owner 0.52% / 149 cyc/op；callsite 行 `187`(130, lock)、`210`(101, unlock)。
- **offer_terminal**（`:346-365`）：self 84，callsite 854，owner **1.80%** / 311 cyc/op；self 行 `349`(56)；callsite 行 **`365`(528, unlock)**、**`348`(326, lock)**；leaf `pthread_mutex_unlock_usercnt` 525 / `pthread_mutex_lock` 326。worker role 938。
- **claim_execution**（`:367-378`）：self 223，callsite 591，owner **1.56%** / 274 cyc/op。self 实际行 **`127`(158, inlined `resolve_internal_`)**、`373`(35)、`368`(5)；callsite 行 **`378`(371, unlock)**、**`368`(220, lock)**。也就是说，`claim_execution` 的 self 主要不是“claim”逻辑，而是内联的 slot resolve；其锁成本在 `368/378`。
- **release_execution**（`:393-409`）：self 48，callsite 548，owner **1.14%** / 200 cyc/op；callsite 行 **`394`(355, lock)**、`409`(192, unlock)。worker role 596。
- **begin_publication**（`:438-460`）：self 63，callsite 199，owner 0.50% / 140 cyc/op；self 行 `440`(41)、`444`(10)；callsite 行 **`460`(120, unlock)**、`439`(79, lock)。main role 262。
- **complete_publication**（`:462-484`）：self 27，callsite 133，owner 0.31% / 87 cyc/op；callsite 行 `463`(72, lock)、`484`(61, unlock)。main role 160。
- **consume_public_result**（`:323-344`）：inlined-self 72，callsite 438，owner 0.98% / 274 cyc/op；inline-body 行 `341`(34)、`334`(21)、`337`(8)、`325`(4)；callsite 行 **`344`(325, unlock)**、`325`(113, lock)；main role 510。它被内联进 `RequestBridge::consume`，所以没有独立 leaf。
- **publish_one**（`src/async/threadpool_backend.cpp:356-374`）：self 101，callsite 0，owner 0.19% / 56 cyc/op；self 行 `358`(50, 调 begin_publication)、`373`(37, 调 deliver_event)、`370`(7, 调 complete_publication)。**publish_one 自己是薄调用序列，锁成本属于 begin/complete，不应计给 publish_one。**
- **deliver_event**（`:376-384`）：inlined-self 37，callsite 0，owner 0.07%；inline-body 行 `382`(28, `on_ready`)、`377`(7)、`383`(2)。薄 dispatcher。
- **wait_one**（`src/async/async_io_context.cpp:573-696`）：self 29，callsite 162，owner 0.37% / 104 cyc/op；main role 191。inclusive 5.02%。
- **wait_for**（`src/async/request_scope.cpp:246-261`）：self 159（行 `248` 128、`252` 25），callsite 15，owner 0.33% / 95 cyc/op。**take**（`:237-244`）：self 193（行 `239` 179），owner 0.37% / 105 cyc/op。

### 3.3 阶段容器 vs 候选

p2 R1 classifier 在本数据上重算（app/all，行和≈100%）：

| 阶段 | app% | all% |
|---|---:|---:|
| admission | 13.49 | 13.49 |
| terminal/release | 4.01 | 4.01 |
| publication | 3.73 | 3.73 |
| observation | 2.53 | 2.53 |
| progress/wait | 5.09 | 5.09 |
| shared sync（无阶段 owner） | 13.46 | 13.46 |
| scheduler/IRQ | 12.04 | 12.04 |
| worker-thread syscall trampoline（p2 R1 名；analysis 旧名 startup/thread create） | 13.63 | 13.63 |
| payload/FS | 13.08 | 13.09 |
| syscall (unattributed) | 7.04 | 7.04 |
| other | 6.62 | 6.70 |

Candidate-family union（每个样本每个 family 至多一次）与 stage 的比值：

| 家族 | owner app% | 对应 stage app% | family/stage |
|---|---:|---:|---:|
| admission | 1.15 | 13.49 | 8.5% |
| terminal/release | 2.94 | 4.01 | 73.2% |
| publication | 1.00 | 3.73 | 26.8% |
| observation | 1.94 | 2.53 | 76.4% |
| progress/wait | 1.17 | 5.09 | 22.9% |
| execution（claim/acquire/release_control） | 1.56 | —（跨 terminal/publication） | — |

**判定（P-D'）**：`offer_terminal` / `claim_execution` / `release_execution` 达到 candidate-level measured hotspot；`consume_public_result`（0.98%）与 `RequestBridge::consume`（1.02%）在阈值边缘；`reserve` / `accept` / `begin_publication` / `complete_publication` below-threshold；`publish_one` / `deliver_event` marginal。admission stage 13.49% 的绝大部分不是 reserve/accept 函数体（候选 owner 仅 1.15%），而是 submit/内核/调度；不能把 stage 份额当作候选可删量。

---

## 4. P-C'：W-03 4k d16（driver progress pass + submitter）

- case：`W03-uring-4k-d16`；app 35,836 样本，iou-wrk 19,740，all 55,576；period all 24,254.6 cyc/op，app 17,091.5。
- W-03 形态：driver 线程在 `epoll_wait` 等 eventfd，submitter 在 condvar futex 等完成（`bench/bench_w03_external_loop.cpp:172-249` driver_thread；`:270-469` run_external_phase）。

### 4.1 driver progress pass

| 候选 | self% (n) | callsite% (n) | owner% | inclusive% | owner cyc/op | role |
|---|---:|---:|---:|---:|---:|---|
| AsyncIoContext::poll_progress | 0.03 (11) | 0.22 (80) | 0.25 | 18.48 | 33 | driver 91 |
| AsyncIoContext::run_progress_pass_ | 0.08 (30) | 0.16 (57) | 0.24 | 15.41 | 32 | driver 87 |
| UringAsyncBackend::poll | 0.15 (55) + 0.02 inlined | 0.75 (267) | 0.92 | 14.50 | 123 | driver 330 |
| UringAsyncBackend::poll_progress | 0.07 (26) | 0.07 (25) | 0.14 | 14.95 | 19 | driver 51 |
| UringAsyncBackend::handle_one_cqe | 0.68 (243) | 0.25 (91) | 0.93 | 5.85 | 122 | driver 334 |
| UringAsyncBackend::reap_cqes | 0.21 (74) | 0.24 (86) | 0.45 | 6.49 | 57 | driver 160 |
| UringAsyncBackend::finalize_operation_terminal_ | 0.51 (184) | 0.00 | 0.51 | 4.29 | 69 | driver 184 |
| driver_thread（bench harness） | 0.12 (44) | 0.63 (225) | 0.75 | 37.95 | 98 | driver 269 |

行级：
- `UringAsyncBackend::poll` self 行 `uring_backend.cpp:651`(19)、`623`(11)、`616`(8)；callsite 行 `652`(100)、`650`(71)、`621`(42)、`643`(16)；leaf `pthread_mutex_lock` 132 / `pthread_mutex_unlock_usercnt` 130。
- `driver_thread` self 行 `bench_w03_external_loop.cpp:199`(15, epoll_wait)、`202`(14)、`204`(4)；callsite 行 `204`(60)、`203`(44)、`206`(28)、`232`(26)；leaf `epoll_wait` 24、mutex lock 96 / unlock 100。driver 的自身 bench 循环成本很小，主要是 epoll/futex 与锁。
- `AsyncIoContext::poll_progress` self 行 `async_io_context.cpp:441/442/447`；callsite 行 `447`(42)、`442`(38)，leaf mutex 43 / unlock 36。

### 4.2 submitter / RequestCore / publication

| 候选 | self% (n) | inlined% (n) | callsite% (n) | owner% | inclusive% | owner cyc/op | role |
|---|---:|---:|---:|---:|---:|---:|---|
| RequestCore::offer_terminal | 0.48 (173) | 0.00 | 1.28 (459) | **1.76** | 2.03 | 234 | driver 632 |
| RequestBridge::consume | 0.16 (59) | 0.00 | 1.17 (420) | **1.34** | 1.85 | 260 | main 479 |
| RequestCore::consume_public_result | 0.00 | 0.13 (46) | 1.17 (419) | **1.30** | 1.81 | 252 | main 465 |
| RequestCore::release_execution | 0.10 (35) | 0.00 | 0.95 (342) | **1.05** | 1.31 | 139 | driver 377 |
| RequestCore::complete_publication | 0.10 (36) | 0.00 | 0.65 (232) | 0.75 | 0.95 | 101 | driver 268 |
| RequestCore::reserve | 0.11 (39) | 0.00 | 0.56 (200) | 0.67 | 1.17 | 129 | main 239 |
| RequestCore::begin_publication | 0.09 (33) | 0.00 | 0.51 (183) | 0.60 | 0.89 | 80 | driver 216 |
| RequestCore::accept | 0.07 (26) | 0.00 | 0.53 (189) | 0.60 | 1.06 | 116 | main 215 |
| RequestCore::retire_observer_delivery | 0.10 (35) | 0.00 | 0.44 (159) | 0.54 | 0.84 | 73 | driver 194 |
| RequestCore::claim_observer_delivery | 0.07 (25) | 0.00 | 0.44 (158) | 0.51 | 0.69 | 67 | driver 183 |
| RequestCore::claim_execution | 0.05 (19) | 0.00 | 0.28 (99) | 0.33 | 0.66 | 64 | main 118 |
| UringAsyncBackend::publish_one | 0.38 (136) | 0.00 | 0.00 | 0.38 | 3.77 | 50 | driver 136 |
| UringAsyncBackend::submit_transport_locked | 0.03 (10) | 0.29 (104) | 0.43 (155) | 0.75 | 25.86 | 144 | main 259 / driver 10 |
| run_external_phase（bench submitter） | 1.14 (410) | 0.00 | 0.50 (178) | 1.64 | 58.88 | 318 | main 588 |

行级：
- `offer_terminal` self `request_core.cpp:349`(128)、`354`(18)；callsite **`365`(345, unlock)**、**`348`(114, lock)**；driver 632。
- `reserve` self `181`(22)、`182`(7)；callsite **`182`(112, unlock)**、`166`(85, lock)；main 239。
- `begin_publication` self `440`(15)、`447`(5)；callsite `439`(94, lock)、`460`(89, unlock)；driver 216。
- `complete_publication` self `462`(15)、`464`(8)；callsite `484`(139, unlock)、`463`(93, lock)；driver 268。
- `consume_public_result` inline-body `341`(25)、`326`(9)、`334`(7)；callsite `344`(346, unlock)、`325`(72, lock)；main 465。
- `release_execution` self `395`(10)、`409`(7)；callsite `394`(237, lock)、`409`(105, unlock)；driver 377。
- `run_external_phase` self `bench_w03_external_loop.cpp:349`(89, submit)、`355`(82)、`342`(55)、`344`(43)；callsite `condition_variable:156`(47)、`417`(25)、`369`(20)。这是 bench submitter 的循环/condvar，不是产品路径；不应与 RequestCore 混淆。

### 4.3 阶段容器 vs 候选

| 阶段 | app% | all% |
|---|---:|---:|
| progress/wait | 23.40 | 19.02 |
| submit | 20.80 | 13.41 |
| admission | 12.64 | 8.15 |
| dispatch | 8.46 | 5.68 |
| worker-thread syscall trampoline | 6.44 | 4.15 |
| syscall (unattributed) | 4.21 | 2.72 |
| publication | 4.11 | 2.65 |
| terminal/release | 3.56 | 2.29 |
| observation | 2.41 | 1.55 |
| shared sync | 2.26 | 5.12 |
| reap/CQE | 2.15 | 3.69 |

Candidate-family / stage：

| 家族 | owner app% | stage app% | family/stage |
|---|---:|---:|---:|
| admission | 1.27 | 12.64 | 10.0% |
| terminal/release | 3.33 | 3.56 | 93.6% |
| publication | 1.73 | 4.11 | 42.1% |
| observation | 2.44 | 2.41 | 101.5%（含 take_result/consume 的边界差异） |
| progress/wait | 1.54 | 23.40 | 6.6% |
| reap/CQE | 1.83 | 2.15 | 85.3% |

**判定（P-C'）**：`offer_terminal`（1.76%）、`RequestBridge::consume`（1.34%）、`consume_public_result`（1.30%）、`release_execution`（1.05%）达到 candidate-level measured hotspot；`reserve`/`accept`/`begin`/`complete` below-threshold。**driver progress pass 的候选级成本很小**（`ctx::poll_progress` 0.25%、`uring::poll` 0.92%），但其 inclusive 达 14.5–18.5%；progress/wait stage 23.40% 的主体是 driver 的 `epoll_wait`、futex/condvar、内核调度，而不是 `poll_progress` 函数自身。submit stage 20.80% 也不是 RequestCore 函数，而是 `io_uring_enter`/内核提交。admission stage 12.64% 与 reserve+accept owner 1.27% 之间的差距同样说明“阶段容器 ≠ 候选”。

---

## 5. P-E'：sluice-copy 4k d4 w1（pipeline wait / progress / publication）

- app 69,371 样本；period 44,345.0 cyc/chunk app（与 p2 PMU 44,481.5 相差 −0.3%）。
- 每 chunk 2 个 request（read+write）；RequestCore 候选的 cyc/request ≈ cyc/chunk ÷ 2。

### 5.1 候选表（app 分母；cyc/chunk）

| 候选 | self% (n) | inlined% (n) | callsite% (n) | owner% | inclusive% | owner cyc/chunk | role |
|---|---:|---:|---:|---:|---:|---:|---|
| AsyncIoContext::wait_one | 0.28 (191) | 0.00 | 3.16 (2194) | **3.44** | 31.29 | 1,297 | main 2,385 |
| ProgressSource::wait_if_unchanged | 0.00 | 0.16 (113) | 1.91 (1328) | **2.08** | 23.25 | 785 | main 1,441 |
| ThreadPoolBackend::poll_progress | 0.27 (190) | 0.00 | 1.78 (1237) | **2.06** | 5.22 | 775 | main 1,427 |
| sluice_copy::run_pipelined_copy_with_context | 1.81 (1254) | 0.00 | 0.00 | 1.81 | 47.18 | 683 | main 1,255 |
| RequestCore::claim_execution | 0.36 (253) | 0.00 | 0.82 (572) | **1.19** | 1.69 | 604 | worker 825 |
| RequestCore::begin_publication | 0.33 (230) | 0.00 | 0.61 (422) | 0.94 | 1.02 | 352 | main 652 |
| RequestBridge::consume | 0.25 (171) | 0.00 | 0.64 (442) | 0.88 | 0.97 | 330 | main 613 |
| RequestCore::offer_terminal | 0.15 (104) | 0.00 | 0.69 (482) | 0.84 | 0.86 | 427 | worker 586 |
| RequestCore::consume_public_result | 0.00 | 0.19 (133) | 0.64 (442) | 0.83 | 0.92 | 309 | main 575 |
| AsyncIoContext::run_progress_pass_ | 0.15 (107) | 0.00 | 0.63 (438) | 0.79 | 6.37 | 293 | main 545 |
| RequestCore::complete_publication | 0.08 (54) | 0.00 | 0.34 (233) | 0.41 | 0.44 | 156 | main 287 |
| RequestCore::retire_observer_delivery | 0.05 (36) | 0.00 | 0.35 (245) | 0.41 | 0.46 | 152 | main 281 |
| RequestCore::release_execution | 0.16 (110) | 0.00 | 0.24 (166) | 0.40 | 0.45 | 202 | worker 276 |
| RequestScope::take | 0.39 (269) | 0.00 | 0.00 | 0.39 | 1.36 | 145 | main 269 |
| RequestCore::accept | 0.07 (47) | 0.00 | 0.31 (217) | 0.38 | 0.39 | 143 | main 264 |
| RequestCore::reserve | 0.05 (35) | 0.00 | 0.32 (225) | 0.37 | 0.38 | 143 | main 260 |
| RequestScope::wait_for | 0.28 (194) | 0.00 | 0.03 (18) | 0.31 | 32.69 | 115 | main 212 |
| ThreadPoolBackend::publish_one | 0.21 (148) | 0.00 | 0.00 | 0.21 | 2.51 | 82 | main 148 |
| RequestScope::wait_for_slot_ | 0.00 | 0.16 (112) | 0.03 (18) | 0.19 | 32.57 | 71 | main 130 |
| PipelinedCopyTask::wait_take | 0.00 | 0.08 (56) | 0.00 | 0.08 | 34.13 | 31 | main 56 |

### 5.2 wait / progress / publication 行级证据

- **wait_one**（`src/async/async_io_context.cpp:573-696`）：owner 3.44% / 1,297 cyc/chunk；callsite 行 **`655`(1,256, 1.81%)**（调 `wait_if_unchanged`）、`609`(252, snapshot)、`595`(133)、`696`(124)；leaf `pthread_mutex_lock` 1,273、`mutex_unlock_usercnt` 753、`__poll` 86。self 实际行 `gthr-default.h:795/825`(63/10)、`progress_source.hpp:437`(9, drain)、`progress_source.hpp:203`(9)。**wait_one 的成本主要是它调用的 wait/poll 与 mutex，不是 wait_one 自身的循环簿记。**
- **wait_if_unchanged**（`include/sluice/async/detail/progress_source.hpp`，p2 坐标 104-232）：inlined-self 113，callsite 1,328，owner 2.08% / 785 cyc/chunk；inline-body 行 `205`(54)、`165`(17)、`203`(9)、`179`(7)；callsite 行 **`205`(698, 1.01%)**、`166`(161, 0.23%)、`203`(86)、`230`(72)；leaf mutex 858、unlock 334、`__poll` 86。这是 W1 wait 路径在 P-E 的主要落点。
- **poll_progress**（`src/async/threadpool_backend.cpp:550-...`）：self 190，callsite 1,237，owner 2.06% / 775 cyc/chunk。self 实际行 `threadpool_backend.cpp:58`(81, `BoundedHandleRing::pop_front`)、`611`(12)、`60`(11)、`550`(11)；callsite 行 **`561`(502, 0.72%, work_mtx lock)**、**`584`(494, 0.71%, work_mtx unlock)**、`607`(133)、`609`(108)；leaf mutex 635 / unlock 596。**这是 owed/publication sweep 的 work_mtx 成本，不是 publish_one 的函数体。**
- **begin_publication**（`request_core.cpp:438-460`）：self 230，callsite 422，owner 0.94%；self 行 `440`(153)、`444`(50)；callsite 行 `460`(298, unlock)、`439`(124, lock)。
- **complete_publication**（`:462-484`）：self 54，callsite 233，owner 0.41%；callsite 行 `463`(122, lock)、`484`(111, unlock)。
- **claim_execution**（`:367-378`）：self 253，callsite 572，owner 1.19%；self 行 `369`(118)、`373`(50)、`368`(43)、`378`(35)；callsite 行 `378`(368, unlock)、`368`(204, lock)。
- **publish_one**（`threadpool_backend.cpp:356-374`）：self 148，callsite 0，owner 0.21%；self 行 `358`(77, begin call)、`373`(33, deliver call)、`370`(14, complete call)。**publish_one 是薄调度器；其 inclusive 2.51% 来自 callee。**
- **consume_public_result / RequestBridge::consume**：consume inline 133 + callsite 442，owner 0.83%；bridge self 171 + callsite 442，owner 0.88%。callsite 行 `344`(315, unlock)、`325`(127, lock)。
- **copy 主路径**：`run_pipelined_copy_with_context` self 1,254，candidate-own line `apps/sluice-copy/copy_task.cpp:334`（lambda 调用点），但实际指令行是 `82`(370, `submit_slot_write`)、`70`(317, `submit_slot_read`)、`198`(258, `run_body`)、`94`(30, `wait_take`)、`73`(29)、`197`(20)、`139`(18, `await_slot_write`)。**不能把 1.81% 全部记到某一行；它是 copy 主循环内联体的聚合。**
- `wait_take`（`copy_task.cpp:91-104`）：inlined-self 56，inclusive 34.13%；inline-body 行 `94`(30)、`97`(18)、`93`(4)、`96`(2)。`wait_for`（`request_scope.cpp:246-261`）self 194（`252` 112、`248` 64），owner 0.31%，inclusive 32.69%。

### 5.3 阶段容器 vs 候选

| 阶段 | app% | all% |
|---|---:|---:|
| progress/wait | 26.37 | 26.38 |
| payload/FS | 10.18 | 10.88 |
| syscall (unattributed) | 9.60 | 9.61 |
| worker-thread syscall trampoline | 8.68 | 8.68 |
| other | 7.74 | 8.27 |
| shared sync | 7.45 | 7.53 |
| admission | 7.17 | 7.17 |
| scheduler/IRQ | 5.65 | 5.71 |
| kernel syscall/FS overhead | 4.43 | 4.65 |
| poll boundary（libc `__poll` only） | 4.42 | 4.42 |
| publication | 3.63 | 3.63 |
| observation | 1.97 | 1.97 |
| terminal/release | 1.29 | 1.29 |
| dispatch | 0.71 | 0.71 |
| backend exec (TP) | 0.70 | 0.70 |

Candidate-family / stage：

| 家族 | owner app% | stage app% | family/stage |
|---|---:|---:|---:|
| admission | 0.76 | 7.17 | 10.5% |
| terminal/release | 1.24 | 1.29 | 96.3% |
| publication | 1.57 | 3.63 | 43.2% |
| observation | 2.02 | 1.97 | 102.5%（边界差异） |
| progress/wait | 6.67 | 26.37 | 25.3% |

**判定（P-E'）**：`wait_one`（3.44%）、`wait_if_unchanged`（2.08%）、`poll_progress`（2.06%）、`claim_execution`（1.19%）达到 candidate-level measured hotspot；`begin_publication`（0.94%）、`offer_terminal`（0.84%）、`consume_public_result`（0.83%）below-threshold；`publish_one`（0.21%）marginal。progress/wait stage 26.37% 是 W1 的形态容器；`wait_one+wait_if_unchanged` owner 简单相加约 5.5%，实际 union 以 progress family 6.67% 为准（含 run_progress_pass_/poll_progress/wait_for 等），不能与 stage 26.37% 混用。publication stage 3.63% 的候选级 owner 1.57%，其余是 callee/kernel/sync。

**介质限制**：P-E 本次 copy 期间 `block:block_rq_issue/complete` 各 202（`raw/block-check.txt`）；空闲 2 s 为 8/10（`raw/block-idle.txt`）。`fincore` 显示 src 256M/65,536 pages cached。因此不能声称本次 P-E 是“严格 0 block”的 page-cache-only profile；wait/progress 份额可能包含少量 IO/写回等待。该点与 p2（block 0）不同，不允许直接跨会话比较 P-E 的 wait 份额。

---

## 6. P-F：W02 uring 4k d64（M-53 measured hotspot）

### 6.1 d64 vs d16 control

| 指标 | d64（`W02-uring-4k-d64`） | d16 control（`W02-uring-4k-d16`） | 比值 |
|---|---:|---:|---:|
| app 样本 / all | 15,874 / 33,390 | 7,654 / 16,965 | — |
| `find_live_router_cookie_` self | 168 | 68 | — |
| self app% | **1.06%** | 0.89% | 1.19× |
| self all% | **0.50%** | 0.40% | 1.25× |
| callsite-extra | 11（0.07% app） | 7（0.09% app） | — |
| owner app% / all% | **1.13% / 0.54%** | 0.98% / 0.44% | 1.15× / 1.23× |
| owner cyc/op（period） | **110.8** | 92.2 | 1.20× |
| self cyc/op（period） | **104.0** | 83.7 | 1.24× |
| inclusive app% | 1.22% | 1.11% | 1.10× |
| period est all cyc/op | 17,545.6 | 17,039.3 | 1.03× |

对照 p2 历史：P-A d16 `find_live_router_cookie_` self 0.41% all / ≈61.9 cyc/op；p2 未采集 d64，P1-A 历史 d64 为 1.74% / ≈116 cyc/op。本次 d16 control 的 all self 0.40% 与 p2 的 0.41% 吻合；d64 的 **104 cyc/op** 与历史 **116 cyc/op** 在约 10% 内，但 all self 0.50% 远低于历史 1.74%，说明历史 share 的分母/形态不可直接外推。**share 与 cyc/op 不可混用**。

### 6.2 router 表深度

- `UringConfig` 的 `request_capacity = depth`：`bench/bench_w02_pipeline.cpp:119`；d64 case 构造：`bench/bench_w02_pipeline.cpp:663`（`add_uring(4096, 20000, 64, false)`）。
- `UringAsyncBackend` 用 `router_(config.request_capacity)` 构造 router 表：`src/async/uring_backend.cpp:163`。因此 d64 的 router 表深度为 **64 entries**；d16 control 为 **16 entries**。
- 查找函数 `find_live_router_cookie_`（`src/async/uring_transport.cpp:551-557`）从 `router_.size()` 向下扫描到 0，检查 `in_use && cookie == user_data`。最坏 64 次迭代/CQE；实际平均扫描长度取决于 live entry 分布和 retirement 顺序，本次**没有** instrumentation 计数，**NOT_OBSERVABLE**。

### 6.3 d64 行级份额（app 分母 15,874）

- `find_live_router_cookie_` self 168 = **1.06% app / 0.50% all**；owner 1.13% / 0.54%；inclusive 1.22% / 0.58%。
- self 行：**`uring_transport.cpp:552` 84（0.53% app）**（loop init / `i-- > 0` 条件）、**`:553` 66（0.42% app）**（`in_use && cookie` 比较）、**`:557` 18（0.11% app）**（函数 epilogue/return path，DWARF 把 not-found return 映到该行）。**552+553 = 150/168 = 89.3%** 的 self 样本在扫描循环的 init/compare 上。
- d16 control 行：`552` 39（0.51% app）、`553` 26（0.34%）、`557` 3（0.04%）；d64 的循环 init 份额更高（84 vs 39 绝对样本），与更深的扫描一致。
- 同一 d64 reap 路径的上下文：`handle_one_cqe` self 0.83% app / 0.44% all，inclusive 4.07%；`finalize_operation_terminal_` self 0.77% / 0.38%；`reap_cqes` self 0.16% / 0.12%；`dispatch_one_locked` self 2.15% / 1.23%；`submit_transport_locked` owner 1.05% / 0.50%，inclusive 59.49%。（`analysis/analysis-w3.json` → `cases.P-F.candidates`）

### 6.4 阶段容器与 M-53 的定位

| 阶段 | app% | all% |
|---|---:|---:|
| submit | 48.91 | 23.25 |
| dispatch | 9.03 | 4.62 |
| progress/wait | 7.12 | 5.23 |
| io-wq punt | 6.73 | 9.40 |
| bench harness | 4.77 | 2.27 |
| admission | 4.66 | 2.22 |
| observation | 4.56 | 2.17 |
| reap/CQE | 2.54 | 4.13 |
| publication | 2.27 | 1.08 |
| terminal/release | 1.36 | 0.65 |

`find_live_router_cookie_` 属于 reap/CQE；reap family owner 2.31% app，find_cookie 的 owner 1.13% 约占 49%。它相对 submit stage 48.91% 很小，也不是 dispatch/observer 的主体。**M-53 判定：在 d64 app 路径达到 candidate-level measured hotspot（owner 1.13% ≥1%，111 cyc/op，168 样本）；但不是全局/阶段主导热点，且本次未测平均扫描深度，不能把 111 cyc/op 当作可删量。**

---

## 7. 跨形态候选判定（本文结论）

阈值见 §0：owner ≥1.00% app 且 ≥40 样本为 candidate-level measured hotspot。

| 候选 | 形态 | measured（app owner；cyc/unit） | 判定 |
|---|---|---|---|
| RequestCore::offer_terminal | P-D | 1.80%（311 cyc/op）；callsite 1.63%，inclusive 3.38% | **hotspot**（worker；callsite 全部在 365/348 lock/unlock） |
| RequestCore::claim_execution | P-D | 1.56%（274 cyc/op）；callsite 1.13%，inclusive 2.72% | **hotspot**（worker；self 主要在内联 resolve_internal_ :127，锁在 368/378） |
| RequestCore::release_execution | P-D | 1.14%（200 cyc/op）；callsite 1.05% | **hotspot**（worker；394/409） |
| RequestCore::consume_public_result | P-D | 0.98%（274 cyc/op）；inlined 0.14%，callsite 0.84% | below-threshold（边缘） |
| RequestCore::reserve | P-D | 0.62%（175 cyc/op）；callsite 0.58% | below-threshold；admission stage 13.49% |
| RequestCore::offer_terminal | P-C | 1.76%（234 cyc/op）；callsite 1.28%，inclusive 2.03% | **hotspot**（driver 632） |
| RequestCore::consume_public_result | P-C | 1.30%（252 cyc/op）；callsite 1.17%，inclusive 1.81% | **hotspot**（submitter 465） |
| RequestCore::release_execution | P-C | 1.05%（139 cyc/op）；callsite 0.95% | **hotspot**（driver 377） |
| RequestCore::reserve | P-C | 0.67%（129 cyc/op）；callsite 0.56% | below-threshold（submitter 239） |
| AsyncIoContext::poll_progress（driver pass） | P-C | 0.25% owner / 18.48% inclusive；uring::poll 0.92% / 14.50% | 非候选级 hotspot；progress stage 23.40% 主要是 epoll/futex/condvar |
| AsyncIoContext::wait_one | P-E | 3.44%（1,297 cyc/chunk）；callsite 3.16%，inclusive 31.29% | **hotspot**（line 655 占 1.81% app） |
| ProgressSource::wait_if_unchanged | P-E | 2.08%（785 cyc/chunk）；inlined 0.16%，callsite 1.91% | **hotspot**（progress_source.hpp:205 占 1.01%） |
| ThreadPoolBackend::poll_progress | P-E | 2.06%（775 cyc/chunk）；callsite 1.78% | **hotspot**（work_mtx 561/584 各 0.72/0.71%） |
| RequestCore::claim_execution | P-E | 1.19%（604 cyc/chunk）；callsite 0.82% | **hotspot**（worker 825） |
| RequestCore::begin_publication | P-E | 0.94%（352 cyc/chunk）；callsite 0.61% | below-threshold |
| ThreadPoolBackend::publish_one | P-E | 0.21%（82 cyc/chunk）；callsite 0 | marginal；薄调度器，成本在 callee |
| UringAsyncBackend::find_live_router_cookie_ | P-F d64 | 1.13%（111 cyc/op）；self 1.06%，inclusive 1.22% | **hotspot**（d64 app 路径；lines 552/553；router depth 64） |
| UringAsyncBackend::find_live_router_cookie_ | P-F d16 | 0.98%（92 cyc/op）；self 0.89% | below-threshold（对照） |
| UringAsyncBackend::dispatch_one_locked | P-F d64 | 2.58%（253 cyc/op）；inclusive 3.77% | **hotspot**（上下文；非 M-53） |
| UringAsyncBackend::handle_one_cqe | P-F d64 | 0.93%（92 cyc/op）；inclusive 4.07% | below-threshold；reap 容器内 |

**与阶段容器的区别（一句话）**：阶段容器是“最深阶段 owner 的整条调用栈”份额（例如 P-D admission 13.49%、P-E progress/wait 26.37%），包含内核/sync/callee；candidate owner 是“候选自身代码 + 直接调用点”的份额（例如 P-D reserve+accept 1.15%、P-E wait_one+wait_if_unchanged 合计约 5.5%，且两者有重叠）。二者不能互换，也不能把 stage 容器当作候选收益上界。

---

## 8. 限制与 NOT_OBSERVABLE

1. **份额 ≠ 可删 cycles**；没有 clean A/B、没有单因素 A/B、没有 mutant/语义门。本文不产出任何“收益已成立”结论。
2. **采样开销**：bench 形态 measured timed phase 的 wall 比约 1.00–1.10×；P-E 观测 0.90×（不是加速结论）。DWARF callgraph 的样本量/份额可用于归因，时长/吞吐不可用于 A/B。
3. **采样率与分母**：`-F 4999` 是 per-event/per-thread；总样本率随活跃线程数变化（W-02 uring 有 main + iou-wrk，P-D 有 4 worker + main）。**跨形态的 per-op 样本率不可直接比较**；本文以形态内 share 与 period estimate 为主。
4. **period estimate** 是样本 period 求和/ops，不是 PMU；与 p2 PMU 的偏差 P-D +4.3%、P-C +19.3%、P-E −0.3%。它用于候选 cyc/unit 的相对量级，不替代 PMU。
5. **内核行级 NOT_OBSERVABLE**：内核无 debuginfo，只有函数级符号；内核叶子的行号不可得。
6. **libc/libstdc++/liburing 行级 NOT_OBSERVABLE**：这些 DSO 无本机 debuginfo（`addr2line` 返回 `??:0`）；但候选的调用点行通过用户态 caller IP 的 DWARF 可得。
7. **单帧样本**：P-D 522（1.0%）、P-C 249（0.7%）、P-E 655（0.94%）、P-F 77（0.49%）；这些样本的 inclusive 只能到 leaf，阶段只能按 leaf 归因。
8. **inline 多义性**：同一 IP 可能对应多条 inline 链；`perf script --inline` 与 `addr2line -i` 可能选择不同链。本文优先 actual instruction line（最内层）并同时给 candidate-own line；P-E `run_pipelined_copy_with_context` 的 `:334` 是 lambda 调用点，实际指令行在 `70/82/94/198`，不能按单行解读。
9. **`perf report` srcline 的口径**：`-s srcline` 可能选择最内层 inline 行（例如 `claim_execution` 显示 `request_core.cpp:127`）；本文的 candidate-own line 来自 addr2line 链的最外层，两者不可混用。
10. **iou-wrk 分母**：P-C 有 19,740/55,576（35.5%）iou-wrk 样本，P-F 有 17,516/33,390（52.5%）；app 分母排除它们。所有 all 分母数字与 p2 的 all-comm 表可比，但不能与 app 分母数字互相换算。
11. **P-E 介质**：本次 copy 期间 block rq issue/complete 各 202，空闲 2s 为 8/10；src `fincore` 虽显示 256M cached，仍不能声称严格 zero-block。P-E 的 wait/progress 份额可能含少量 IO/写回等待；与 p2 的 block 0 会话不可直接比较。
12. **P-E 基线差异**：本次 baseline 0.824 s/copy，明显慢于 p2 的 0.398 s/copy；形态/系统状态不同，本文不与 p2 作时长或 A/B 比较。
13. **阶段容器重算**：使用 p2 R1 classifier（含 libc `__poll` boundary）在本会话 inline 展开的 frames 上重算；不是 p2 文件的逐字节复现，差异可达 ~1.5 pp（inline 帧与 per-thread 采样）。stage 表用于“容器 vs 候选”的定性/半定量对照，不作为新的 p2 结论。
14. **candidate-family union 可轻微重叠**：一个样本可能同时属于两个 family 的 owner（例如跨 family 的 inline 链）；family 数字不可相加；stage 数字也不可与 family 相加。
15. **M-53 平均扫描深度/占用 NOT_OBSERVABLE**：只知 router 表深度（d64=64、d16=16）与候选 cyc/op；没有 `in_use` 计数或平均迭代次数。d64/d16 的 cyc/op 比 1.24×，不是 R 的线性比，不能外推。
16. **未观测候选**：`RequestCore::observe_public_result`、`register_observer`、`acquire_execution`、`release_control`、`find_live_router_index_` 在这些 case 中 0 样本（NOT_OBSERVED，不等于不存在/未执行）；`ProgressSource::signal` / `drain_notification_nolock_` 未作为独立符号观测（多为 inline），其成本在 wait_one/wait_if_unchanged 的 inline-body 中体现。
17. **未覆盖形态**：d1、TP w1/w8、W-03 d4/d64、64K、zero-op、cancel、saturation、W-01、磁盘/cold/fsync 形态未采集；不得外推。
18. **无逐请求因果链**：本文是 aggregated（函数/行份额）+ correlated（事件/case 计数），没有 submit→complete 的逐请求 join；不报告逐请求延迟/成本。
19. **`perf record` 数据是 root-owned**：分析时用 `sudo -S -p ''` 读取；`SHA256SUMS` 由 root 计算。`/tmp/sluice621-sudo` 内容未读取、未记录。
20. **`sudo` / 环境**：`perf_event_paranoid=2`，CPU-wide/kernel 采集以 root 执行；未修改 sysctl。

---

## 9. 原始数据索引与复现

```
p4/profile/
  env/
    snapshot-before.txt, snapshot-after.txt, binaries.txt,
    overhead-and-samples.txt
  analysis/
    analyze_w3.py, analysis-w3.json, analysis-w3.txt
  P-D-w02-tp-4k-d16-w4/raw/
    run-01.data, run-02.data, smoke-cycles.data
    script-inline-run-01.txt, script-inline-run-02.txt
    report-flat-self-run-01.txt, report-flat-self-run-02.txt
    report-inline-self-run-01.txt, report-inline-self-run-02.txt
    report-callgraph-{reserve,accept,offer_terminal,claim_execution,
      release_execution,begin_publication,complete_publication,consume,
      publish_one,deliver_event,wait_one,wait_for,take}-run-01.txt
    header-run-*.txt, lost-run-*.txt, run-*.jsonl, baseline.*
  P-C-w03-uring-4k-d16/raw/  同结构 + callgraph-{uring_poll,find_cookie,
      driver_thread,submitter,context_poll}-run-01.txt
  P-E-sluice-copy-4k-d4-w1/raw/  同结构 + overhead-1copy.data,
      overhead-timed.data, block-check.txt, block-idle.txt,
      baseline.times, baseline2.times
  P-F-w02-uring-4k-d64/raw/  d64 run-01/02 + control-d16-run-01.data +
      callgraph-{find_cookie,handle_cqe,reap_cqes,finalize,dispatch_one,
      submit_transport,consume,wait_for,take}-run-01.txt +
      report-callgraph-find_cookie-control-d16.txt
  SHA256SUMS
```

- 分析脚本：`python3 -I p4/profile/analysis/analyze_w3.py`（读 `script-inline-*.txt`；写 `analysis-w3.json` / `analysis-w3.txt`）。
- P-D `raw/smoke-cycles.data` 是采集格式/解析的 smoke（warmup=1 measured=1），**不进入任何统计**；正式统计只用 `run-01/run-02`。
- 复现采集：见 §1.4；每个 data 的样本量/丢失/时长见 `profile/env/overhead-and-samples.txt` 与各 `header-*.txt` / `lost-*.txt`。
- 二进制/构建：`p4/rowlevel-build-manifest.md`；sha256 见 §1.1。
- 校验：`sha256sum -c p4/profile/SHA256SUMS`（root-owned 文件需 root 读取；`SHA256SUMS` 的路径相对 `p4/profile/`）。

---

*本文件结束。所有数字来自 `p4/profile/analysis/analysis-w3.json`（函数/行/角色/period 聚合）与 `p4/profile/*/raw/` 原始 perf 数据；任何引用请同时给出形态、分母（app 样本数或 ops/chunks）和“份额 ≠ 可删 cycles”的口径。*
