# AA-CALIBRATION — #621 P1 A/A 独立复算、安静度判定与 MME

- 角色：独立统计师。只读原始逐 arm 数据，自写复算脚本；未复用 runner 的任何汇总数字。
- 日期：2026-11-18
- BASE：`5d94f233967c3aef993eb19ba9e3252c532b0951`（`meta.json.git.head`）
- 数据根：`docs/evidence/621-progress-wait-path-20261118/p1/aa/`（`smoke/` 已排除）
- 复算脚本：`docs/evidence/621-progress-wait-path-20261118/tools/recompute_independent.py`
- 复算命令：

```
cd docs/evidence/621-progress-wait-path-20261118
python3 -I tools/recompute_independent.py --aa-root p1/aa > /tmp/aa-recompute.json
```

- 纪律与元数据依据：`CAMPAIGN-LEDGER.md:488-501`（§9.1 硬规则）、`:568-575`（§10.4 每格必带元数据）。
- 本报告没有重新构建、没有重新运行 bench/test/perf；只对既有原始记录做只读复算。构建/测试回执属于 `p1/build-manifest.md`，不在本轮重跑。

---

## 0. 结论摘要

| 格 | workload | 有效配对 | hw CPU/task % | hw instr/task % | hw wall % | 判词 |
|---|---|---:|---:|---:|---:|---|
| C1-sluice-copy-4k-d4-w1 | sluice-copy 4 KiB / depth 4 / 1 worker | 16 | 0.365 | 0.005 | 0.708 | **QUIET** |
| C2-w02-tp-4k-d16 | W-02 ThreadPool 4 KiB d16（目标格缺失） | 0 | — | — | — | **INCONCLUSIVE** |
| C3-w02-uring-4k-d16 | W-02 io_uring 4 KiB d16 | 16 | 3.312 | 0.068 | 1.725 | **ENVIRONMENT_BLOCKED**（CPU/wall 频率漂移） |
| C4-w03-uring-4k-d16 | W-03 io_uring 4 KiB d16 | 16 | 1.402 | 0.200 | 0.722 | **QUALIFIED** |
| C5-w02-tp-4k-d1 | W-02 ThreadPool 4 KiB d1（目标格缺失） | 0 | — | — | — | **INCONCLUSIVE** |
| C6-w02-uring-4k-d1 | W-02 io_uring 4 KiB d1 | 16 | 0.471 | 0.006 | 0.885 | **ORDER_SENSITIVE**（wall 位置效应） |

要点：

1. 6 格中只有 4 格产生了有效计时数据。C2/C5 的 case filter 一个 bench case 都没选中，32/32 arm 无效；**当前没有可用的 ThreadPool A/A 基线**。
2. 全部 6 格、192 arm、576 个 perf event 行均为 `percent_enabled=100.00`，没有 `<not counted>` / `<not supported>` / `scaled` 标记；**没有 perf 多路复用缩放**。注意：C3 的 `ENVIRONMENT_BLOCKED` 来自 CPU 频率变化（CPU/task 与 cycles/instructions 的离散度背离），不是 perf 事件缩放；两者必须分开记录。
3. C1 安静；C6 的 CPU 安静但 wall 有 +1.13 pp 的“第二臂更慢”位置效应；C4 可用但只支持 ≥2.3% 的 CPU 效应（16 对、80% 功效）；C3 的 CPU/wall 被约 5–7% 的有效频率波动污染，不能作 CPU/wall 基线。
4. 冻结噪声带只在“同 host、同 kernel/governor、同 binary、同 workload、n=16”条件下成立。任何 A/B 候选换 build、换 workload 或换格都必须重新做 A/A，不能搬运本报告的 hw。
5. MME：W1 的 5% cycles 线索在 C6 类安静格和 C3 cycles 上 16 对即可分辨，但在 C4（W-03）cycles 上 16 对的 MDE 为 6.09%，**NOT_POWERED**；W3 单候选按 2% 计划需 18 对（C3 cycles，hw≤1%）或 38 对（C4 CPU）；M-53 没有 d64 A/A，d64 噪声带未测，不能外推；M-42 在 W-02 中的合理效应远低于 16 对噪声带，**NOT_POWERED**，需要隔离格。

---

## 1. 协议与预注册判据

### 1.1 格与 workload

| 格 | 类型 | 原始入口 | 说明 |
|---|---|---|---|
| C1 | `sluice_copy` | `sluice-copy --buffer-size 4096 --pipeline-depth 4 --workers 1 --sync none --no-atomic` | 256 MiB 源（btrfs，`compress=zstd:1`）→ `/tmp` tmpfs |
| C2 | `bench_w02` | `bench_w02_pipeline`，filter `=W02-threadpool-4k-d16` | 实际 case ID 带 `-w4`，filter 未命中（见 §4） |
| C3 | `bench_w02` | `bench_w02_pipeline`，filter `=W02-uring-4k-d16` | 4 KiB、d16、uring |
| C4 | `bench_w03` | `bench_w03_external_loop`，filter `=W03-uring-4k-d16` | 4 KiB、d16、外部 epoll 循环 |
| C5 | `bench_w02` | `bench_w02_pipeline`，filter `=W02-threadpool-4k-d1` | 实际 case ID 是 `W02-threadpool-4k-d1-w1`（workers=1），filter 未命中（见 §4） |
| C6 | `bench_w02` | `bench_w02_pipeline`，filter `=W02-uring-4k-d1` | 4 KiB、d1、uring |

每格 16 个 pair、32 个 arm；A/A 两臂为同一 binary sha256。AB/BA 交错规则：奇数 pair 为 AB，偶数 pair 为 BA（`meta.json.protocol.order_rule`）。时间戳逐 arm 串行、无重叠（复算脚本与独立检查均验证）。

### 1.2 指标与配对差

每个 arm 是一次完整进程调用（bench 格包含 2 个 warmup + 7 个 measured repetition；C1 是一次完整 copy）。本报告的主指标：

- **CPU/task**：`cpu_user_s + cpu_sys_s`（`RUSAGE_CHILDREN` 的 user+sys）；不直接信任 runner 的 `cpu_us`，而是从两个原始字段重算，并核对 `cpu_us` 与 wall 时间戳。
- **user instructions/task**：直接解析 `raw/*.perf` 中的 `instructions:u`；不读 `pairs.tsv` 的扁平化值。
- **wall**：arm 的 `wall_s`（monotonic），并与 `start`/`end` 时间戳差核对（容差 `max(20 ms, 2%)`）。
- **cycles/task**（辅助）：直接解析 `raw/*.perf` 的 `cycles:u`。W1 的历史线索是 cycles，故保留；但 cycles 不是 schema 要求的三项之一。

配对差定义：

```
d_i = 100 * (B_i - A_i) / A_i
```

另算对称分母 `200*(B_i-A_i)/(A_i+B_i)` 作敏感性；其 hw 与主定义差 <0.1 pp（见 §3.1 脚注）。

### 1.3 统计方法

- 固定种子 **621**；bootstrap **10,000** 次，percentile 95% CI，半宽 `hw=(q97.5-q2.5)/2`。种子由 `sha256(cell|metric)` 派生，保证可复现（`recompute_independent.py:169`）。
- AB/BA 分组差：组均值差 + 10,000 次置换检验（`recompute_independent.py` 中 `permutation_mean_diff`）。
- 漂移：pair 序号对 d_i 的 OLS 斜率 + 10,000 次置换 p；同时给出前 8 对/后 8 对均值。
- 位置效应：pair 内“第二臂 − 第一臂”（AB 为 B−A，BA 为 A−B），10,000 次符号翻转检验。它比 AB−BA 分组差更直接地度量“第二个 arm 是否系统性更慢”，且不受跨 pair 漂移影响。
- MME：使用配对差的样本 SD 与 Student-t 分位数（正则化不完全 beta 反解，无 SciPy）：
  - 80% 功效、双侧 α=0.05：`(t_.975 + t_.80) * SD / sqrt(n) <= δ`（`recompute_independent.py:228`）。
  - 95% CI 半宽 ≤ δ/2：`t_.975 * SD / sqrt(n) <= δ/2`（`recompute_independent.py:246`）。
- 所有 hw/MDE 都是**描述性**的；A/A 只估计噪声，不检验 A/B 效应。

### 1.4 A/A 安静度门（本报告预注册）

本报告在给出判词前固定以下判据；对后续 A/B 而言这是预注册门，对本次 A/A 是分析判据。阈值是统计设计选择，不是 root 要求，owner 可以挑战，但不能在看到 A/B 结果后再改。

1. **样本量**：有效配对 ≥12 才计算；≥15 才可作正式基线（ledger §9.1.8）。有效对 <12 → `INCONCLUSIVE`。
2. **回执**：每个有效 arm 的 oracle 必须通过；bench 格每 arm 必须有 CAL + 2 warmup + 7 measured，且 target case ID 正确；C1 必须 `dst_sha256 == src_sha256` 且 read/write ops 冻结一致。任何失败 → `INCONCLUSIVE`。
3. **无 perf 缩放**：所有决定性指标的 perf 行必须 `percent_enabled=100.00`，无 `<not counted>`/`<not supported>`/`scaled`。否则该格 `NOT_POWERED`/`ENVIRONMENT_BLOCKED`。
4. **平稳性**：CPU/instr/wall 中任一指标的漂移置换 p<0.01 → `NOISY`。
5. **位置/顺序**：任一指标 |AB−BA|>1 pp 且置换 p<0.01 → `ORDER_SENSITIVE`。
6. **噪声等级**（用 SD(d)，不用 hw；hw 随 n 变，SD 是格本身的稳定参数）：
   - `QUIET`：SD_CPU ≤1.5 pp、SD_wall ≤2.5 pp、SD_instr ≤0.05 pp；
   - `QUALIFIED`：SD_CPU ≤3.5 pp、SD_wall ≤3.0 pp；
   - 超过 → `NOISY`。
7. **频率**：若 CPU/task 的 CV 超过 cycles CV 的 2 倍、且 instructions CV <0.5%，判为有效频率变化；该格 CPU/wall 记 `ENVIRONMENT_BLOCKED`（这与 perf 事件缩放是两件事，见 §7）。

### 1.5 本报告不做什么

- 不做 A/B 判词；没有候选 SHA、没有候选 build、没有语义 mutant 结果，任何“收益”都不能从本报告推出。
- 不把 A/A 的均值（接近 0）当效应；不把 16 对的非显著结果当“无效应”。
- 不做跨介质/跨机器外推（ledger §9.1.9）。C1 是 btrfs→tmpfs 的 copy，不是磁盘/NVMe 结论。
- 不读、不引用两个未跟踪目录（`621-round2-R1/`、`e5-fullpath-machine-work/`）。

---

## 2. 原始数据与独立复算

### 2.1 原始文件清单

| 格 | `arms.jsonl` | `pairs.tsv` | `raw/` 文件数 | raw 组成 |
|---|---:|---:|---:|---|
| C1 | 32 行 | 32 行 + header | 128 | 32 × (cmd/perf/stderr/stdout) |
| C2 | 32 | 32 + header | 160 | 32 × (cmd/jsonl/perf/stderr/stdout) |
| C3 | 32 | 32 + header | 160 | 同上 |
| C4 | 32 | 32 + header | 160 | 同上 |
| C5 | 32 | 32 + header | 160 | 同上 |
| C6 | 32 | 32 + header | 160 | 同上 |

所有 `raw/*.stdout`、`*.stderr`、`*.perf` 路径在 `arms.jsonl` 中都存在（0 missing）；每格 32 个 arm 的 `start`/`end` 串行无重叠。

### 2.2 独立复算路径

1. **perf 指标直接读 raw 文件**：`instructions:u`、`cycles:u`、`task-clock:u` 从 `raw/pair-NN-{A,B}.perf` 解析；`arms.jsonl.perf.rows` 只用于对照，不作为指标源。
2. **CPU 从原始字段重算**：`cpu_user_s + cpu_sys_s`，并检查 `cpu_us` 与重算值差 ≤2 µs；`wall_s` 与 `end-start` 对照。
3. **bench 回执直接读 raw JSONL**：每 arm 的 `raw/pair-NN-{A,B}.jsonl` 重新解析，核对 CAL、target case ID、warmup=2、measured=7、所有 target 记录 `ok=true`、`failure` 为空。
4. **C1 回执直接读 raw stdout 与 oracle 记录**：重新解析 `raw/pair-NN-{A,B}.stdout` 的 `copied 268435456 bytes (read_ops=65540 write_ops=65536 short_writes=0 ...)`，与 oracle 的 parsed 字段逐项对照；并核对 32/32 arm 的记录哈希 `dst_sha256 == src_sha256`（dst 文件已不在盘上，不能重算文件哈希）。
5. **交叉检查**：`pairs.tsv` 与 `arms.jsonl` 的 `valid/wall_s/cpu_user_s/cpu_sys_s/perf_instructions` 对照，32/32 行 checked，**0 mismatch**。
6. **独立 validity vs runner validity**：192 个 arm 全部一致，**0 mismatch**。
7. **脚本独立性**：`recompute_independent.py` 没有 import `paired_session.py` 或 `aa_recompute.py`，没有读取它们的中间结果，只使用 Python stdlib；全部聚合从原始记录重算。

### 2.3 校验结果

| 格 | oracle 通过 arm | CAL | target 记录/arm | 全部 target ok | copy dst==src | perf 行 scaling |
|---|---|---:|---:|---|---|---:|
| C1 | 32/32 | n/a | n/a | n/a | 32/32 | 0/96 |
| C2 | 0/32 | 通过 | 0（0 warmup + 0 measured） | 无 target | n/a | 0/96 |
| C3 | 32/32 | 通过 | 9（2 + 7） | 是 | n/a | 0/96 |
| C4 | 32/32 | 通过 | 9（2 + 7） | 是 | n/a | 0/96 |
| C5 | 0/32 | 通过 | 0（0 + 0） | 无 target | n/a | 0/96 |
| C6 | 32/32 | 通过 | 9（2 + 7） | 是 | n/a | 0/96 |

- 6 格共 576 个 perf event 行；`percent_enabled` 全部 `100.00`；`badMarkers=[]`。**没有 perf 缩放/外推值。**
- C2/C5 的 `meta.json.arms_valid=0`、`arms_invalid=32`；其 `excluded_arms` 对每个 arm 给出 `target_case_missing`、`warmup_records=0!=2`、`measured_records=0!=7`。
- C1 的 32 个 arm 全部 `read_ops=65540`、`write_ops=65536`、`short_writes=0`；`dst_sha256` 全等于 `src_sha256`（`a1f8dd97…`）。

### 2.4 §10.4 元数据对照

| §10.4 项 | 状态 | 证据/缺口 |
|---|---|---|
| case_id/backend/block/depth/workers | bench 格从 raw JSONL 可得；C1 从 `meta.case` 可得 | C2/C5 的 `meta.case.case` 是字面 filter，不是实际 case ID；见 §4 |
| saturating / warm-cold | **缺** `saturating` 显式字段；bench 的 `cache_policy=page_cache_resident` | C1 未显式记录 warm/cold，只记录了 256 MiB 源文件 |
| 介质路径与 mount options | 有 | `meta.media`：C1 src btrfs `/home`（`compress=zstd:1`），dst tmpfs `/tmp`；bench 格 tmpfs `/tmp/621-p1-aa` |
| BASE SHA | 有 | `5d94f233…`，`git.dirty=true`（未跟踪证据目录） |
| 候选 SHA | A/A 同 binary | C1 `db972b19…`；C2/C3/C5/C6 `03dbef09…`；C4 `43054fcd…`；A/B 相同 |
| binary sha / build dir / liburing / 编译器 | 有 | `/tmp/621-p1-build/…`；`liburing_linked=true`；GCC 16.2.1（binary comment） |
| warmup/measured、run 顺序/rotation、AB/BA | bench 有（2/7；raw JSONL 有 `order_index`）；C1 是单进程/arm | C1 无 warmup/measured 概念；rotation 只在 bench 内 |
| governor/min/max/no_turbo | 有 | 每格 before/after `governor_all_performance=true`；`no_turbo=0` |
| 频率采样 | **弱** | 每 arm 仅 min/max/last（20 ms 间隔），无完整序列；min 可能来自空闲核 |
| IRQ 亲和性 | **缺 per-IRQ 记录** | 只有 `p1/session-snapshot.txt` 的 `irqbalance_active_after_freeze=inactive` |
| 背景负载 | **弱** | 只有 cell 级 `loadavg_before/after`，无每 pair 的 ps guard |
| 温度 | 有（per-arm min/max/last；before/after） | 包温最高 62°C，无 80°C 以上样本 |
| oracle、exit code、raw 路径 | 有 | 每 arm `rc=0`、`timed_out=false`、oracle 记录、raw 路径 |
| `0 test(s) failed out of N` | 不在计时格内 | 构建/测试回执在 `p1/build-manifest.md`（81/81），本轮未重跑 |

结论：§10.4 的核心字段（case、介质、SHA、governor、oracle、原始路径）齐全；**频率序列、IRQ 亲和性、每 pair 背景负载、saturating/warm-cold 是缺口**，在 §7 作为风险登记。

---

## 3. 逐格结果

### 3.1 主统计表

单位：mean/median 是配对差 `100*(B−A)/A` 的百分数；SD 是配对差的百分点（pp）；hw 是固定种子 bootstrap 95% CI 半宽（%）；MDE80 是 n=16 时 80% 功效可分辨的最小效应（%）。CV(d)=SD/|mean|×100。

**CPU/task**

| 格 | n | mean % | median % | SD pp | CV(d) % | hw % | t-hw % | MDE80 % |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| C1 | 16 | +0.133 | +0.090 | 0.786 | 591.4 | 0.365 | 0.419 | 0.589 |
| C2 | 0 | — | — | — | — | — | — | — |
| C3 | 16 | −1.319 | −0.718 | 7.027 | 532.5 | 3.312 | 3.744 | 5.266 |
| C4 | 16 | −0.402 | −0.810 | 3.019 | 750.3 | 1.402 | 1.609 | 2.262 |
| C5 | 0 | — | — | — | — | — | — | — |
| C6 | 16 | +0.122 | +0.156 | 0.997 | 820.2 | 0.471 | 0.531 | 0.747 |

**user instructions/task**

| 格 | n | mean % | median % | SD pp | CV(d) % | hw % | t-hw % | MDE80 % |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| C1 | 16 | +0.003 | +0.002 | 0.012 | 335.4 | 0.005 | 0.006 | 0.009 |
| C2 | 0 | — | — | — | — | — | — | — |
| C3 | 16 | +0.032 | +0.027 | 0.143 | 445.2 | 0.068 | 0.076 | 0.107 |
| C4 | 16 | −0.096 | −0.054 | 0.425 | 441.6 | 0.200 | 0.226 | 0.318 |
| C5 | 0 | — | — | — | — | — | — | — |
| C6 | 16 | −0.003 | −0.001 | 0.014 | 408.9 | 0.006 | 0.007 | 0.010 |

**wall**

| 格 | n | mean % | median % | SD pp | CV(d) % | hw % | t-hw % | MDE80 % |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| C1 | 16 | −0.036 | +0.054 | 1.495 | 4127.5 | 0.708 | 0.797 | 1.120 |
| C2 | 0 | — | — | — | — | — | — | — |
| C3 | 16 | −0.870 | −0.789 | 3.653 | 419.8 | 1.725 | 1.947 | 2.738 |
| C4 | 16 | −0.129 | −0.260 | 1.520 | 1177.8 | 0.722 | 0.810 | 1.139 |
| C5 | 0 | — | — | — | — | — | — | — |
| C6 | 16 | −0.060 | −0.205 | 1.885 | 3132.1 | 0.885 | 1.005 | 1.413 |

**cycles/task（辅助）**

| 格 | n | mean % | median % | SD pp | hw % | MDE80 % |
|---|---:|---:|---:|---:|---:|---:|
| C1 | 16 | +0.739 | +0.509 | 2.346 | 1.121 | 1.758 |
| C3 | 16 | +0.194 | −0.151 | 1.991 | 0.949 | 1.492 |
| C4 | 16 | −0.961 | −0.912 | 8.123 | 3.843 | 6.088 |
| C6 | 16 | +0.615 | +1.018 | 2.876 | 1.368 | 2.155 |

脚注：

- **CV(d) 在 A/A 中不可作噪声尺度**：A/A 的均值接近 0，CV(d)=SD/|mean| 会被放大到百分之几百；表中列出是为了满足“配对差 CV”的要求，实际应比较 **SD(d)** 和 **hw**。若改看配对比值的 CV（`sd(B/A)/mean(B/A)`），数值约等于 SD(d)（例如 C1 CPU 0.785%、C6 CPU 0.745%）。
- 对称分母 `200*(B−A)/(A+B)` 的 hw 与上表差 <0.1 pp：C1 CPU 0.369、C3 CPU 3.365、C4 CPU 1.437、C6 CPU 0.483。主定义与对称定义不改变任何判词。
- 16 对的 bootstrap hw 是 n=16 的带宽；n 变化时 hw 约按 1/√n 缩放，但 SD 是格本身的稳定参数，MME 用 SD。

### 3.2 诊断：AB/BA、位置效应、漂移

**AB/BA 与位置效应**（AB−BA 单位 pp；位置效应=第二臂−第一臂，正=第二个 arm 更慢/更大）

| 格 | 指标 | AB mean | BA mean | AB−BA | 置换 p | 位置均值 pp | 正号/16 | 符号翻转 p |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| C1 | CPU | +0.411 | −0.145 | +0.557 | 0.163 | +0.278 | 9 | 0.157 |
| C1 | instr | +0.005 | +0.002 | +0.003 | 0.695 | +0.002 | 9 | 0.708 |
| C1 | wall | +0.406 | −0.479 | +0.885 | 0.250 | +0.442 | 10 | 0.230 |
| C3 | CPU | +1.368 | −4.007 | +5.374 | 0.129 | +2.687 | 12 | 0.123 |
| C3 | instr | −0.024 | +0.088 | −0.111 | 0.119 | −0.056 | 4 | 0.118 |
| C3 | wall | +0.629 | −2.370 | +2.999 | 0.101 | +1.500 | 12 | 0.104 |
| C4 | CPU | −1.175 | +0.370 | −1.544 | 0.315 | −0.772 | 6 | 0.303 |
| C4 | instr | −0.067 | −0.126 | +0.059 | 0.806 | +0.029 | 7 | 0.811 |
| C4 | wall | −0.711 | +0.453 | −1.165 | 0.128 | −0.582 | 7 | 0.114 |
| C6 | CPU | +0.447 | −0.203 | +0.650 | 0.197 | +0.325 | 9 | 0.185 |
| C6 | instr | −0.004 | −0.002 | −0.002 | 0.986 | −0.001 | 11 | 0.990 |
| C6 | wall | +1.072 | −1.193 | +2.265 | **0.0069** | **+1.132** | **13** | **0.0049** |

- C6 的 wall 位置效应是本次 A/A 中唯一在 1% 水平附近稳定的顺序信号：第二个 arm 的 wall 平均慢 1.13 pp，13/16 为正，符号翻转 p=0.0049；AB−BA 分组差 2.265 pp（p=0.0069）约等于位置效应的两倍。CPU 的同位置效应只有 +0.325 pp（p=0.185），说明它不是“整个进程变慢”，而更像 wall 上的文件/页缓存/调度位置代价。
- 12 个 cell×metric 检验的 Bonferroni 阈值是 0.05/12≈0.0042；C6 wall 的 p=0.0049 刚好在其上方。因此本报告把它记为“**位置敏感**”而不是“已证实的顺序偏差”：足以要求严格 AB/BA 平衡和配对分析，但不足以单独作为强证据。
- C3 的位置效应（CPU +2.69 pp、wall +1.50 pp）与跨 pair 漂移混在一起，不能单独归因。

**漂移**（pair 序号 1..16 对 d_i 的 OLS 斜率；前 8/后 8 对均值）

| 格 | 指标 | 斜率 pp/pair | 置换 p | 前 8 对 % | 后 8 对 % | 后−前 pp |
|---|---|---:|---:|---:|---:|---:|
| C1 | CPU | −0.039 | 0.367 | +0.241 | +0.025 | −0.216 |
| C1 | instr | −0.001 | 0.304 | +0.006 | +0.001 | −0.005 |
| C1 | wall | −0.022 | 0.795 | +0.244 | −0.316 | −0.560 |
| C3 | CPU | −0.682 | **0.0705** | +1.088 | −3.727 | **−4.816** |
| C3 | instr | +0.014 | 0.077 | −0.012 | +0.076 | +0.088 |
| C3 | wall | −0.379 | **0.0527** | +0.532 | −2.272 | **−2.804** |
| C4 | CPU | +0.059 | 0.725 | −0.245 | −0.560 | −0.315 |
| C4 | instr | +0.008 | 0.742 | −0.063 | −0.129 | −0.066 |
| C4 | wall | −0.029 | 0.741 | +0.097 | −0.355 | −0.452 |
| C6 | CPU | −0.017 | 0.767 | +0.219 | +0.024 | −0.195 |
| C6 | instr | +0.000 | 0.866 | −0.005 | −0.001 | +0.004 |
| C6 | wall | +0.015 | 0.890 | −0.009 | −0.112 | −0.103 |

- C1、C4、C6 没有漂移信号（p≥0.30）。C3 的 CPU 漂移 p=0.0705、wall 漂移 p=0.0527，未达本报告预注册的 0.01 阈值，但前后半段差 −4.82 pp（CPU）/−2.80 pp（wall）已经与 16 对噪声带同量级；结合频率证据，C3 不能当基线。
- 前 8/后 8 的切分是描述性诊断，不是事后排除依据；C3 的 pair 1 仍保留（见 §4.2）。

### 3.3 频率、温度、负载

| 格 | 采样/arm | per-CPU 臂内 min 的范围 | busy-proxy <2.5 GHz 的 arm | 包温 max | 说明 |
|---|---:|---|---:|---:|---|
| C1 | 42–44 | cpu2: 3.193–3.435 GHz；cpu3: 3.193–3.435 GHz | 0/32 | 56°C | 频率稳定，无低频样本 |
| C2 | 3 | cpu2: 1.197–3.415；cpu3: 1.197–3.438 | 21/32 | 55°C | **CAL-only，60 ms 进程；频率/温度不可解释** |
| C3 | 68–76 | cpu2: 1.197–3.228；cpu3: 1.197–3.222 | 15/32 | 59°C | 有低频样本；CPU 时间 CV 5.87% vs cycles CV 1.64% → 有效频率波动 |
| C4 | 104–111 | cpu2: 1.197–3.287；cpu3: 1.197–3.223 | 12/32 | 60°C | cycles CV 5.61% 而 CPU CV 1.92%，不是频率主导 |
| C5 | 3–4 | cpu2: 1.197–3.439；cpu3: 1.197–3.357 | 20/32 | 59°C | **CAL-only，不可解释** |
| C6 | 294–317 | cpu2: 1.197–3.220；cpu3: 1.197–3.219 | 11/32 | 62°C | 有低频样本，但 CPU CV 0.74%、cycles CV 2.09%，未见频率主导致 |

说明：

- “busy-proxy” 取两个受测 CPU 的臂内 min 的较大者；若两个核都被采到 min，则 proxy 也可能是 1.2 GHz。它只能作为粗指标，不能当“持续频率”。
- 更直接的频率证据是 **CPU/task 与 cycles/instructions 的离散度背离**（`recompute_independent.py:675` 的 `frequency_audit`）：
  - C3：CPU CV 5.872%、cycles CV 1.637%、instr CV 0.111% → CPU CV / cycles CV = 3.59；
  - C1：0.662 / 1.621 = 0.41；C4：1.917 / 5.610 = 0.34；C6：0.745 / 2.087 = 0.36。
  - 只有 C3 呈现“CPU 时间大幅波动而 cycles/instructions 稳定”的频率变化 signature。按预注册判据，C3 的 CPU/wall 记 `ENVIRONMENT_BLOCKED`。
- 包温最高 62°C，没有 80°C 以上样本；本报告不把温度当作热节流证据。
- `loadavg`：C1 0.14→0.69；C3 0.72→1.38；C4 1.38→2.21；C6 2.21→1.18（20 个 CPU）。C6 从 2.21 的负载下仍保持 CPU SD 1.0%，说明负载不是 C3 噪声的唯一解释；但只有 cell 级 loadavg，**没有每 pair 的 ps guard**，不能排除短时背景干扰。

### 3.4 逐格判词

**C1 — QUIET（可作 copy 尺度 A/A 参考）**

- 16/16 有效配对；oracle 32/32；`dst==src` 32/32；read/write ops 冻结一致。
- SD：CPU 0.786 pp、instr 0.012 pp、wall 1.495 pp；hw：0.365%/0.005%/0.708%；MDE80（n=16）：0.589%/0.009%/1.120%。
- 漂移 p=0.30（CPU）；位置效应 p=0.16（CPU）/0.23（wall）；AB−BA 不显著。
- 每个 arm 的臂内 min 在 3.19–3.44 GHz（32/32 arm 的 busy-proxy 都 ≥3 GHz）；包温 ≤56°C。
- 限制：这是 btrfs→tmpfs 的 256 MiB copy，`--no-atomic`，不是 W-02/W-03 bench 格；不能把它的带宽直接外推到磁盘/NVMe（ledger §9.1.9）。它能作 sluice-copy 尺度的安静参考，不能替代 C3/C4/C6 的基线。

**C2 — INCONCLUSIVE（无计时数据）**

- 0/16 有效配对；32/32 arm 无效。原因：filter `=W02-threadpool-4k-d16` 未命中任何 bench case。
- `bench_w02_pipeline.cpp:463-467` 对 ThreadPool case 追加 `-w<workers>`；实际冻结 case 是 `W02-threadpool-4k-d16-w4`。`p1/build-manifest.md:98,142` 已在 smoke 记录过同一问题。
- raw JSONL 只有 CAL 记录（`records_total=1`、`target_records=0`、`warmup=0`、`measured=0`）；32 个 arm 的 wall 中位数 60.6 ms、task-clock 中位数 31.5 ms，只是 CAL/启动，**不得当计时数据**。
- 重跑：`SLUICE_BENCH_CASE_FILTER='=W02-threadpool-4k-d16-w4'`（或 owner 指定的 `-w1`/`-w8`），重新做 16+ 对 A/A；本格不能作任何基线。

**C3 — ENVIRONMENT_BLOCKED（CPU/wall 频率变化）**

- 16/16 有效配对；oracle 32/32；target 9 记录/arm（2+7）；无 perf 缩放（96/96 行 100%）。
- CPU/task：mean −1.319%、median −0.718%、SD 7.027 pp、hw 3.312%、MDE80 5.266%。
- cycles：SD 1.991 pp、hw 0.949%、MDE80 1.492%；instr：SD 0.143 pp、hw 0.068%、MDE80 0.107%。
- CPU CV 5.872% vs cycles CV 1.637%、instr CV 0.111% → 有效频率约 5–7% 波动；这是 `ENVIRONMENT_BLOCKED` 的依据，不是工作量的真实方差。
- 漂移：CPU p=0.0705（前 8 +1.09% → 后 8 −3.73%）、wall p=0.0527；AB−BA CPU +5.37 pp（p=0.129）。
- 结论：不能作 CPU/wall 基线。cycles/instr 的带宽虽窄（0.949%/0.068%），但本报告**不**把该格认证为“cycles-only 基线”：频率 excursion 同时改变了执行环境，且漂移边缘；若 owner 要 cycles-only 结论，应单独预注册一个 cycles-only A/A，而不是从本格事后挑选指标。

**C4 — QUALIFIED（W-03 基线，限 ≥~2.3% CPU 效应）**

- 16/16 有效配对；oracle 32/32；target 9 记录/arm；无 perf 缩放。
- CPU/task：mean −0.402%、median −0.810%、SD 3.019 pp、hw 1.402%、MDE80 2.262%。
- instr：SD 0.425 pp、hw 0.200%、MDE80 0.318%；wall：SD 1.520 pp、hw 0.722%、MDE80 1.139%；cycles：SD 8.123 pp、hw 3.843%、MDE80 6.088%。
- 漂移 p≥0.72；位置效应 p≥0.11；AB−BA 不显著。包温 ≤60°C；只有 CPU CV 1.92% vs cycles CV 5.61%，不是频率主导致。
- 可用性：16 对只能支持 CPU ≥2.3%、wall ≥1.2% 的 80% 功效声明；1% CPU 效应需要 74 对（80%）/143 对（hw≤0.5%）；cycles 声明很差（MDE80 6.09%，1% 需要 520 对）。

**C5 — INCONCLUSIVE（无计时数据）**

- 0/16 有效配对；32/32 arm 无效。原因：filter `=W02-threadpool-4k-d1` 未命中任何 bench case；ThreadPool d1 的实际 case 是 `W02-threadpool-4k-d1-w1`（`bench_w02_pipeline.cpp:656` 的 `add_tp(4096, 20000, 1, 1, false)`），需要 `-w1`。
- raw JSONL 只有 CAL；wall 中位数 60.8 ms、task-clock 中位数 31.6 ms；不得当计时数据。
- 重跑：先确定 workers（d1 的冻结默认是 4；若要单 worker 用 `-w1`），再用完整 case ID 重跑 A/A。

**C6 — ORDER_SENSITIVE（CPU 可作配对基线；wall 需严格平衡）**

- 16/16 有效配对；oracle 32/32；target 9 记录/arm；无 perf 缩放。
- CPU/task：mean +0.122%、median +0.156%、SD 0.997 pp、hw 0.471%、MDE80 0.747%。
- instr：SD 0.014 pp、hw 0.006%、MDE80 0.010%；wall：SD 1.885 pp、hw 0.885%、MDE80 1.413%；cycles：SD 2.876 pp、hw 1.368%、MDE80 2.155%。
- 漂移 p=0.77；CPU 位置效应 +0.325 pp（p=0.185）；**wall 位置效应 +1.132 pp，13/16 为正，p=0.0049；AB−BA +2.265 pp，p=0.0069**。
- 结论：CPU 是安静的；wall 有“第二个 arm 更慢”的位置效应，严格 AB/BA 平衡和配对分析可以消掉均值偏差，但不能把 wall 当“无顺序效应”的干净基线。任何 C6 wall A/B 必须保持完整 16 对交替；若丢失一个 arm 导致 AB/BA 不平衡，wall 结果不可用。

### 3.5 过程级指标 vs measured-only（稀释检查）

bench 进程的 CPU 包含 fixture 写、CAL、启动/teardown，以及 9 个 repetition。用 raw JSONL 的 7 个 measured 记录单独配对，得到：

| 格 | measured CPU / process CPU（均值） | measured-only CPU SD | measured-only CPU hw | measured-only wall SD | measured-only wall hw |
|---|---:|---:|---:|---:|---:|
| C3 | 24.0% | 7.831 pp | 3.672% | 4.377 pp | 2.097% |
| C4 | 26.3% | 5.882 pp | 2.776% | 3.971 pp | 1.892% |
| C6 | 25.0% | 1.045 pp | 0.488% | 2.114 pp | 1.001% |

- measured-only 的噪声并不比过程级小（C3 7.83 vs 7.03；C4 5.88 vs 3.02；C6 1.05 vs 1.00），因为 process 级把 9 个 repetition 和稳定的 fixture 开销平均了；runner 用 process 级指标是合理的。
- 但**效应会被稀释**：measured 段只占 process CPU 的 24–26%。若候选效应是“measured loop 的 5%”，过程级只剩约 1.2–1.3%。这直接决定 W1 的 MME 必须写清 5% 的分母（见 §6.2）。

---

## 4. 排除与无效清单

### 4.1 C2 / C5：filter 未命中，32/32 arm 无效

| 格 | 无效 arm | 原因（runner 与独立复算一致） | raw 证据 |
|---|---:|---|---|
| C2 | 32/32 | `target_case_missing`（32）、`warmup_records=0!=2`（32）、`measured_records=0!=7`（32） | 每 arm JSONL 仅 1 条 CAL；`target_records=0` |
| C5 | 32/32 | 同上 | 同上 |

根因：`bench/bench_w02_pipeline.cpp:463-467` 对 ThreadPool case 追加 `-w<workers>`；C2 的 d16 实际是 `W02-threadpool-4k-d16-w4`，C5 的 d1 实际是 `W02-threadpool-4k-d1-w1`（`bench_w02_pipeline.cpp:656`），两个 filter 都少了 `-w<workers>`。这与 `p1/build-manifest.md:98,142` 的 smoke 记录一致。C2/C5 的 32 个 arm 仍有 perf 数值，但进程只跑了 CAL，wall 60 ms 量级；**不得用它们计算任何 CPU/instr/wall 差值**。

重跑命令模板（必须重新登记 16+ 对；不要复用本次目录）：

```
SLUICE_BENCH_WARMUP=2 SLUICE_BENCH_MEASURED=7 \
SLUICE_BENCH_CASE_FILTER='=W02-threadpool-4k-d16-w4' \
SLUICE_BENCH_JSONL=<new>/w02-tp-4k-d16-w4.jsonl \
SLUICE_BENCH_TMP_DIR=<new> \
<bench_w02_pipeline>
```

C5 的 d1 格：实际冻结 case 是 `W02-threadpool-4k-d1-w1`（workers=1，`bench_w02_pipeline.cpp:656`），filter 用 `=W02-threadpool-4k-d1-w1`；若要其它 workers 变体，需先确认 runner 是否构造了该 case。

### 4.2 未做事后排除

- C1/C3/C4/C6 没有无效 arm，也没有按 outlier 删除任何 pair。
- C3 的 pair 1（CPU +7.71%、wall +3.29%）保留；排除 pair 1 后 C3 CPU hw=3.314%（n=15），与全量 3.312% 几乎相同，**判词不依赖单个 pair**。
- 留一法（drop one pair）的 hw 范围：C1 CPU 0.359–0.404；C3 CPU 3.011–3.585；C4 CPU 1.372–1.523；C6 CPU 0.427–0.509。C3 的下限仍 >3%，C4 的上限仍 <1.6%，判词稳定。
- 不排除 C3 pair 9/11/14 等大偏差：它们没有 oracle/perf 失败，属于环境噪声；事后删除会违反预注册纪律。

---

## 5. 冻结噪声带

**适用条件**：同 host（E5，kernel 7.3.3-200.fc44.x86_64）、同 `performance` governor / `no_turbo=0`、同 binary sha、同 workload、同介质、n=16、AB/BA 严格交替。任何条件变化都必须重新做 A/A。

| 格 | CPU hw / SD / MDE80 | instr hw / SD / MDE80 | wall hw / SD / MDE80 | cycles hw / SD / MDE80 | 状态 |
|---|---|---|---|---|---|
| C1 | 0.365% / 0.786pp / 0.589% | 0.005% / 0.012pp / 0.009% | 0.708% / 1.495pp / 1.120% | 1.121% / 2.346pp / 1.758% | **QUIET**；copy 尺度 |
| C2 | — | — | — | — | 无数据 |
| C3 | 3.312% / 7.027pp / 5.266% | 0.068% / 0.143pp / 0.107% | 1.725% / 3.653pp / 2.738% | 0.949% / 1.991pp / 1.492% | **ENVIRONMENT_BLOCKED**（CPU/wall 频率） |
| C4 | 1.402% / 3.019pp / 2.262% | 0.200% / 0.425pp / 0.318% | 0.722% / 1.520pp / 1.139% | 3.843% / 8.123pp / 6.088% | **QUALIFIED**（≥~2.3% CPU） |
| C5 | — | — | — | — | 无数据 |
| C6 | 0.471% / 0.997pp / 0.747% | 0.006% / 0.014pp / 0.010% | 0.885% / 1.885pp / 1.413% | 1.368% / 2.876pp / 2.155% | **ORDER_SENSITIVE**（wall） |

使用规则：

1. 上表的 hw 是**该格 n=16 的 95% CI 半宽**；如果 A/B 也用 16 对，则 80% 功效 MDE 就是最后一列。若候选 A/B 用别的 n，用 SD 重新算，不要直接搬 hw。
2. C1 的带只适用于 sluice-copy 4k/d4/w1；C4 的带只适用于 W-03 4k/d16；C6 的带只适用于 W-02 uring 4k/d1。不能把 C6 的 0.47% 搬到 C3/C4，也不能把 C1 的 0.365% 搬到 bench。
3. C3 的 cycles/instr 带虽窄，但**不冻结为基线**（见 §3.4）；C6 的 wall 带虽窄，但含位置效应，只冻结为“配对 wall 的带”，不是“无顺序偏差的带”。
4. 本报告没有 d64 格、没有 64K 格、没有可用的 TP 格（C2/C5 无效）；这些格的带全部为空，不能用相邻格外推。

---

## 6. MME 与预算

### 6.1 方法

对每个候选，给定计划效应 δ（%）和格/指标的 SD(d)（pp）：

- **n80(δ)**：`(t_.975,n-1 + t_.80,n-1) * SD / sqrt(n) <= δ` 的最小 n（双侧 α=0.05，80% 功效）。
- **n_half(δ)**：`t_.975,n-1 * SD / sqrt(n) <= δ/2` 的最小 n（“95% CI 半宽不超过效应的一半”；这是 M-53 的 ledger 判据，也比 80% 功效更严）。
- **MDE80(n=16)**：16 对时 80% 功效可分辨的最小 δ。

这些 n 都是**每个候选、每个格、每个指标**的下限；实际预算还要加上 A/A 的 16 对、失败重跑、以及多重候选的修正。功效公式用“t_.975 + t_.80”的分位数近似（非中心 t 的常用近似），n 很小时会有少量偏差，预算应向上取整并留余量。下表把“没有实测 d64 带”的场景明确标成 scenario，不是实测。

### 6.2 W1 — ProgressSource poll 等待路径（历史线索 ≤5% cycles，未证实）

W1 的 ledger 状态：P1，先 profile 准入，不直接开 A/B；5.0–5.5% cycles 是**被取代的 R1 工作稿线索**，不是已证实收益（`CAMPAIGN-LEDGER.md:257,270`）。本报告只把它当“计划效应上限”。

| 场景 | 格/指标 | SD pp | hw %（n=16） | MDE80 %（n=16） | δ=5% n80/n_half | δ=3% | δ=2% | δ=1% |
|---|---|---:|---:|---:|---|---|---|---|
| A | C3 W-02 d16 / cycles | 1.991 | 0.949 | 1.492 | 4 / 5 | 6 / 10 | 10 / 18 | 34 / 64 |
| B | C6 W-02 d1 / CPU | 0.997 | 0.471 | 0.747 | 3 / 3 | 4 / 5 | 5 / 7 | 10 / 18 |
| C | C4 W-03 d16 / CPU | 3.019 | 1.402 | 2.262 | 6 / 9 | 11 / 19 | 20 / 38 | 74 / 143 |
| D | C4 W-03 d16 / cycles | 8.123 | 3.843 | **6.088** | **23 / 43** | 60 / 116 | 132 / 256 | 520 / 1017 |

判断：

- **如果 5% 是过程级 cycles**：C3 cycles（16 对 MDE 1.49%）和 C6 CPU（0.75%）都远够；C4 cycles 不够——16 对的 MDE 是 6.09%，**NOT_POWERED**；C4 需要 ≥23 对（80%）或 ≥43 对（hw≤2.5%）。
- **如果 5% 是 measured loop 的 5%**：measured CPU 只占过程 CPU 的 24–26%，过程级效应稀释到约 1.2–1.3%。C6 CPU（MDE 0.75%）仍可在 16 对分辨；C4 CPU（MDE 2.26%）、C3 CPU（MDE 5.27%）和 C3 cycles（MDE 1.49%）都**NOT_POWERED**（1.2–1.3% 低于各自 16 对 MDE）；C4 cycles 更不可能。
- **如果真实效应只有 2%**：C3 cycles 需 10 对（80%）/18 对（hw≤1%）；C4 CPU 需 20/38；C4 cycles 需 132/256；C6 CPU 需 5/7。
- **建议**：W1 预注册必须写清“5% 的分母”（process cycles / measured-loop cycles / measured-loop CPU）和决定性指标。优先在 C6 类安静 uring 格上用 CPU/cycles：3 对即达 80% 功效/δ=5%，实际 A/B 用 16 对（满足 ledger 最低要求并留余量）。若必须在 W-03 上做 cycles 声明，预算 ≥23 对（80%）/≥43 对（hw≤2.5%），不能用 16 对。若 owner 只接受 5% 的“历史线索”而不接受分母定义，该候选应保持 INCONCLUSIVE，不得把 16 对结果宣传为收益。

### 6.3 W3 — RequestCore 行级候选（行级未知）

W3 的 ledger 状态：族级 mutex 约 9–11%，但**没有单候选**；需要先做 measured 行级归因，再逐候选 A/B（`CAMPAIGN-LEDGER.md:259,271`）。9–11% 不能当任何单候选的 δ。

本报告用 **δ=2% 作为计划值**（不是实测），并给出 δ=1%/3%/0.5% 的敏感性：

| 场景 | 格/指标 | SD pp | hw %（n=16） | MDE80 %（n=16） | δ=3% n80/n_half | δ=2% | δ=1% | δ=0.5% |
|---|---|---:|---:|---:|---|---|---|---|
| A | C3 W-02 d16 / cycles | 1.991 | 0.949 | 1.492 | 6 / 10 | 10 / 18 | 34 / 64 | 127 / 247 |
| B | C4 W-03 d16 / CPU | 3.019 | 1.402 | 2.262 | 11 / 19 | 20 / 38 | 74 / 143 | 289 / 563 |
| C | C3 W-02 d16 / CPU（不建议） | 7.027 | 3.312 | 5.266 | 46 / 87 | 99 / 193 | 390 / 762 | — |

判断：

- C3 CPU/wall 已被频率变化污染，**不能**用于 W3；C3 CPU 的 2% 需要 99/193 对，且仍有频率混杂，不值得。
- C3 cycles 是当前最有希望的 W3 代理，但**本报告没有把它认证为 cycles-only 基线**（§3.4/§5）：2% 需 10 对（80%）/18 对（hw≤1%）；1% 需 34/64。36 对预算在 C3 cycles 上 MDE80≈0.96%、hw≈0.67%，可以覆盖 2% 的单候选声明并有余量；1% 的 hw≤0.5% 需要 64 对。使用前必须单独预注册一个 cycles-only A/A；不能直接把本格的 cycles 数当已认证带。
- C4 CPU 是已认证的 QUALIFIED 格：2% 需 20/38；36 对可覆盖 2% 的 80% 功效（MDE≈1.45%），但 hw≤1% 需要 38 对；1% 需 74/143。
- **建议**：先完成 row-level profile；每个候选预注册 δ（建议 2%）和决定性指标。优先 C4 CPU（已认证）；若要 C3 cycles，先做 cycles-only A/A。每候选至少 **18–38 对**（取决于格/指标和 hw 要求），不要用 16 对去宣称 1% 效应。若候选的效应只体现在 CPU 时间且必须在 C3 上做，先修频率环境，否则记 NOT_POWERED。

### 6.4 M-53 — W4 d64 cookie/router O(R)（ledger 要求 hw ≤ 效应/2）

M-53 的 ledger 判据是“d64 A/A 半宽 ≤ 效应/2”，并要求三项迁移义务证明（`CAMPAIGN-LEDGER.md:211,260`）。**本次 P1 没有 d64 A/A 格**；因此 d64 的 SD 是未测的，下面三行是**场景边界**，不是 d64 实测，不能当外推值。

| 场景（假设 d64 噪声接近） | 指标 | SD pp | hw %（n=16） | MDE80 %（n=16） | δ=5% n80/n_half | δ=3% | δ=2% | δ=1% |
|---|---|---:|---:|---:|---|---|---|---|
| A：C6 d1 | CPU | 0.997 | 0.471 | 0.747 | 3 / 3 | 4 / 5 | 5 / 7 | 10 / 18 |
| B：C3 d16 | cycles | 1.991 | 0.949 | 1.492 | 4 / 5 | 6 / 10 | 10 / 18 | 34 / 64 |
| C：C3 d16 | CPU | 7.027 | 3.312 | 5.266 | 18 / 33 | 46 / 87 | 99 / 193 | 390 / 762 |

判断：

- **d64 未测**：如果 d64 的 CPU 噪声像 C3（SD 7.03），16 对的 MDE 是 5.27%，2% 效应 **NOT_POWERED**；即使 36 对，hw 仍约 2.3%（t_.975≈2.03, 7.03/6≈1.17 → 2.38%），不满足 2%/2=1% 的判据；要达到 2% 的 hw≤1% 需要 **193 对**。
- 如果 d64 像 C6（SD 1.0），2% 效应只需 5/7 对，1% 需 10/18 对；但 d64 的 router 表更深、O(R) 扫描更长，**效应可能更大、噪声也可能不同**，不能假定像 C6。
- **建议**：先跑 `W02-uring-4k-d64`（或 W-04 等价 d64 格）的 16+ 对 A/A，冻结 d64 的 SD/hw；再做 M-53 的 A/B。预算占位：
  - d64 CPU SD ≤1 pp：A/B 24–40 对可覆盖 2–3% 的 hw≤δ/2（2% 只需 7 对，实际用 24–40 留余量）；
  - d64 CPU SD ≈3 pp：2% 需 38 对、3% 需 19 对；若 owner 不接受，记 `NO_CHANGE_JUSTIFIED` 或改用 cycles（若 d64 cycles SD ≈2 pp，2% 需 18 对）；
  - d64 CPU SD ≈7 pp：2% 需 193 对、3% 需 87 对、1% 需 762 对；应优先怀疑环境/频率问题，而不是加样本。
- 在任何情况下，**不要把 C3/C6 的 d16/d1 带当 d64 实测**；本报告不提供 d64 的“已冻结噪声带”。

### 6.5 M-42 — `submit_flushes_` 只写不读原子计数（极小效应）

M-42 的 ledger 状态：GO（排队，未启动）；要求独立单因素 A/B + 机制门 + 动态回执带 + 隔离格（`CAMPAIGN-LEDGER.md:217,262`）。本报告用 W-02 的 submit 计数估算其可测性：

- C3/C6 每 arm 9 个 repetition × 20,000 submits = **180,000 submits/arm**（`workload.perArmMean.submits=180000`）。
- C6 的 process instructions 中位数 3.606B；**1 条 instruction/submit = 0.00499%**。C3 是 2.090B，1 条 = 0.00861%。
- C6 的 instr SD 0.014 pp，16 对 MDE80 0.010%；1 条/submit 的信号（0.005%）**低于** 16 对带；要 80% 功效分辨 0.005% 需 **61 对**，hw≤0.0025% 需 **118 对**。
- 若 `fetch_add` 的实际代价是 3–5 cycles/submit（C6 约 0.017–0.029% cycles；C3 约 0.037–0.062%），C6 CPU（SD 1.0 pp）的 16 对 MDE 0.75% 仍远大于信号；按 0.02% 的 CPU 效应，80% 功效需约 **19,500 对**（`required_n_power` 的量级），不可行。

| 场景 | 指标 | SD pp | hw %（n=16） | MDE80 %（n=16） | δ=1% n80/n_half | δ=0.5% | δ=0.2% | δ=0.1% | δ=0.05% |
|---|---|---:|---:|---:|---|---|---|---|---|
| C6 / CPU | CPU | 0.997 | 0.471 | 0.747 | 10 / 18 | 34 / 64 | 198 / 385 | 783 / 1531 | 3124 / 6115 |

C6 / C3 的 instructions 指标用更小的 δ（因为一条 instruction/submit 只有 0.005% 量级）：

| 场景 | 指标 | SD pp | hw %（n=16） | MDE80 %（n=16） | δ=0.05% n80/n_half | δ=0.02% | δ=0.01% | δ=0.005% | δ=0.002% |
|---|---|---:|---:|---:|---|---|---|---|---|
| C6 / instr | instr | 0.014 | 0.006 | 0.010 | 3 / 4 | 6 / 10 | 17 / 32 | 61 / 118 | 371 / 724 |
| C3 / instr | instr | 0.143 | 0.068 | 0.107 | 66 / 128 | 402 / 784 | 1599 / 3128 | 6388 / 12505 | 39915 / 78140 |

判断：

- **M-42 在 W-02 上 NOT_POWERED**：合理效应（≤0.05% CPU，或 1 条 instruction/submit = 0.005%）远低于 16 对噪声带；即使 36 对，CPU MDE 约 0.50%，仍不够。用 instr 指标，61–118 对可分辨“1 条/submit”，但这仍不是 CPU 收益的证据，且 C6 的 instr 带 0.006% 已接近分辨率极限。
- **不要**用 36 对 W-02 时间去跑 M-42；那只会得到“不显著”，不能区分“无效应”和“效应太小”。
- **建议**：按 ledger 要求先建隔离格（submit-only 紧循环 + 动态回执带），使 `submit_flushes_` 成为可测比例（例如 ≥0.1–0.5% 的 CPU/instructions），再重跑本 MME。在隔离格 A/A 完成前，M-42 保持 **NOT_POWERED**（不是 NO_GO）。
- 机制门仍须先过“无生产读者、release 无 seam 符号、动态回执带、隔离格判据”；本报告只回答功率，不回答机制。

### 6.6 NOT_POWERED / 预算风险登记

| 候选 | 16 对是否够 | 关键阈值 | 需要的对（80% / hw≤δ/2） | 风险 |
|---|---|---|---|---|
| W1（5% cycles，C4 W-03） | **否**（MDE 6.09%） | δ=5% | 23 / 43 | 如果 5% 的分母是 measured loop，过程效应仅 ~1.2%，更不够 |
| W1（5% cycles，C3 W-02） | 是（MDE 1.49%） | δ=5% | 4 / 5 | C3 CPU/wall 被频率污染；只可 cycles，且需单独预注册 |
| W1（5% CPU，C6 d1） | 是（MDE 0.75%） | δ=5% | 3 / 3 | C6 wall 位置效应；CPU 可用 |
| W3（2%，C3 cycles） | 是（MDE 1.49%，但需先做 cycles-only A/A） | δ=2% | 10 / 18 | 行级归因未做；9–11% 族级线索不能当单候选；C3 未被认证为 cycles 基线 |
| W3（2%，C4 CPU） | **否**（16 对 MDE 2.26% >2%） | δ=2% | 20 / 38 | 16 对不够；20 对才到 80% 功效，38 对才 hw≤1% |
| W3（1%，任意当前格） | **否** | δ=1% | 34–74 / 64–143 | 需要 ≥64 对（C3 cycles）或 ≥143 对（C4 CPU） |
| M-53（2%，d64 未知） | **未知/很可能否** | δ=2% | 5–99 / 7–193（取决于 d64 SD） | 无 d64 A/A；不能用 d16/d1 外推 |
| M-42（≤0.05% CPU） | **否** | δ=0.05% | ~3124 / 6115（C6 CPU） | 合理效应远低于噪声；需隔离格 |

---

## 7. 风险与限制

1. **C2/C5 无数据**：ThreadPool d16/d1 两个格都是 0 有效对，当前没有 TP 基线；W3 的 RequestCore 结论只能用 uring 格代理，不能声称 TP 已完成。
2. **C3 频率变化**：CPU/task CV 5.87% vs cycles 1.64%、instr 0.11%，有效频率约 5–7% 波动；CPU/wall 的 A/A 带宽（3.31%/1.73%）不能用于小效应。按预注册判据记 `ENVIRONMENT_BLOCKED`。这不是 perf 多路复用（576/576 行 100%），两者不要混为一谈。
3. **C3 漂移边缘**：CPU 漂移 p=0.0705、wall p=0.0527；前 8 对 +1.09% → 后 8 对 −3.73%。即使不按 0.01 阈值判 NOISY，也不能把 C3 当稳定基线。
4. **C4 CPU 噪声**：SD 3.02 pp，16 对 MDE 2.26%；1% CPU 效应需 74 对（80%）/143 对（hw≤0.5%）。C4 cycles 更差（SD 8.12 pp，MDE 6.09%），不要把 cycles 当 W-03 的默认指标。
5. **C6 wall 位置效应**：第二臂 wall 平均 +1.13 pp，13/16 为正，p=0.0049（12 检验 Bonferroni 阈值 0.0042，刚好在其上）；严格 AB/BA 平衡可消均值偏差，但不能把 C6 wall 当无顺序效应的干净基线。CPU 的位置效应 +0.33 pp（p=0.185），CPU 可用。
6. **样本量**：每格 16 对，满足 ledger ≥15 的最低要求，但低于历史 24/36。所有 hw/MME 都是 n=16 的；小效应需要更多对。16 对“不显著”不能当“无效应”。
7. **过程级指标的稀释**：bench 进程 CPU 只有 24–26% 在 measured loop；fixture/CAL/启动占其余。W1 的“5% cycles”没有预注册分母；如果 5% 指 measured loop，过程级效应约 1.2–1.3%，C4/C3 的 16 对都不够（C6 够）。这是 W1 最大的设计风险。
8. **无 d64 A/A**：M-53 的 d64 噪声带未测；本报告的 d64 数字全部是 scenario，不是外推实测。没有 d64 格就不能宣称“M-53 的 hw≤效应/2 已达标”。
9. **W3 行级未知**：9–11% 是 mutex family 线索，不是单候选；本报告的 W3 MME 用 δ=2% 作计划值，owner 必须确认这个计划值，否则预算无意义。
10. **M-42 的效应量级**：1 条 instruction/submit 只占 C6 过程 instructions 的 0.005%；CPU 效应很可能 ≤0.05%。当前 W-02 格无法分辨；需要隔离格。把 M-42 记 NOT_POWERED，不是 NO_GO。
11. **元数据缺口**：没有 per-IRQ 亲和性（只有 irqbalance inactive 的冻结快照）；没有每 pair 的 ps guard（只有 cell 级 loadavg）；频率只有 min/max/last（20 ms 采样，无序列）；`saturating` 未记录；C1 的 warm/cold 未显式记录。这些不推翻判词，但限制了“环境完全受控”的强度。
12. **背景负载**：C3 end loadavg 1.38、C4 end 2.21、C6 start 2.21（20 CPU）。没有逐 pair 背景负载，不能把 C3 的漂移全部归因于频率；但 C6 在 2.21 下仍 CPU SD 1.0%，说明负载不是唯一解释。
13. **介质与 workload 外推**：C1 是 btrfs（compress=zstd:1）→ tmpfs 的 256 MiB copy，`--no-atomic`；bench 格是 tmpfs 上的 64 MiB 文件、page-cache resident。不能把 C1 当磁盘/NVMe 结果，也不能把 tmpfs 当磁盘（ledger §9.1.9）。
14. **A/A 不是 A/B**：所有 arm 同 binary；本报告没有候选 SHA、没有 mutant、没有语义 oracle 的重新执行。A/B 必须重新登记 build/SHA、重跑语义门和 A/A；本报告的带只在同 binary 下有效。
15. **统计限制**：bootstrap/permutation 都是描述性；没有多重比较修正（12 个 cell×metric 检验的 Bonferroni 阈值 0.0042）；C6 wall 的未校正 p=0.0049 刚好高于该阈值，故只记 ORDER_SENSITIVE，不宣传为“已证实的顺序偏差”。CV(d) 在 A/A 中因均值≈0 而不可解释，已用 SD(d) 替代。
16. **没有重跑构建/测试/bench**：本报告只读原始数据；`p1/build-manifest.md` 的 81/81 测试、readelf 断言等没有被本轮重新执行。任何“语义全过”的宣称仍以原始回执为准，不是本报告的新证据。

---

## 8. 建议的下一步

1. **先修 C2/C5**：用 `=W02-threadpool-4k-d16-w4` 和 `=W02-threadpool-4k-d1-w1` 重跑 16+ 对 A/A；在此之前，W3 的 TP 结论保持 INCONCLUSIVE，不要用 uring 格代替 TP 格。
2. **W1**：预注册“5% 的分母 + 决定性指标”。优先 C6 类安静 uring 格（CPU，16 对可判 5%）；若在 W-03 上做 cycles，预算 ≥43 对（hw≤2.5%），不要 16 对；若真实效应 2%，按 §6.2 加到 5–256 对（取决于格/指标/判据；C6 CPU 5 对，C4 cycles hw≤1% 256 对）。
3. **W3**：先做 row-level measured 归因；每候选预注册 δ=2%（或 owner 给定值）。优先 C4 CPU（已认证，至少 38 对到 hw≤1%）；若要 C3 cycles（至少 18 对到 hw≤1%），先单独做 cycles-only A/A。不要用 C3 CPU/wall。
4. **M-53**：先做 d64 A/A（16+ 对），冻结 d64 SD/hw；再按 §6.4 决定 24–40 对（安静）或 193+ 对（C3-like）。d64 A/A 不安静就不启动 A/B。
5. **M-42**：不要用 W-02 A/B 去“测一下”；先建隔离格和动态回执带，使计数器成为可测比例；在隔离格 A/A 完成前保持 NOT_POWERED。
6. **更新 ledger**：把 C2/C5 的 INCONCLUSIVE、C3 的 ENVIRONMENT_BLOCKED、C4 的 QUALIFIED、C6 的 ORDER_SENSITIVE、C1 的 QUIET，以及本报告的冻结带和 MME 数字，按 GOV-02 状态写入 v2 ledger 的 dated 621 节；注明 n=16、host/kernel/governor、binary SHA 和原始路径。
7. **不要**把本报告的 C1/C6 低 hw 宣传为“已经证明候选有效”；A/A 只说明基线噪声，不说明任何 A/B 收益。

---

## 附录 A：精确命令与复现

```bash
# 1) 独立复算（本报告的全部数字）
cd docs/evidence/621-progress-wait-path-20261118
python3 -I tools/recompute_independent.py --aa-root p1/aa \
  > /tmp/aa-recompute.json

# 2) 只看判词
python3 -I -c 'import json;d=json.load(open("/tmp/aa-recompute.json"));[print(c["cell"],c["verdict"]["class"],c["verdict"]["reason"]) for c in d["cells"]]'

# 3) raw 文件完整性（每格 32 arm；C1 128 个 raw 文件，bench 160 个）
for d in p1/aa/C*; do printf '%s ' "$d"; ls "$d/raw" | wc -l; done

# 4) perf scaling 审计（脚本已内嵌；独立 grep 也可）
grep -rl 'not counted\|not supported\|scaled' p1/aa/*/raw/*.perf || echo 'no scaling markers'
```

- 脚本版本：`TOOL_VERSION = "1.0-independent"`；bootstrap 10,000；所有置换/符号翻转 10,000；seed 621。
- 本报告所有 hw 都是 bootstrap 95% CI 半宽；所有 MDE 都是 t 分位数、80% 功效、双侧 α=0.05。
- 除本报告与脚本外，没有把复算 JSON 写入仓库；复算输出在 `/tmp/aa-recompute.json`，报告数字来自该 JSON。

## 附录 B：逐 pair CPU/wall 配对差（%）

CPU/task（`100*(B−A)/A`）：

| pair | C1 | C3 | C4 | C6 |
|---:|---:|---:|---:|---:|
| 1 | +1.42 | +7.71 | −3.69 | +2.12 |
| 2 | −0.71 | −0.35 | −1.82 | −1.18 |
| 3 | +0.47 | +4.85 | −0.57 | +0.95 |
| 4 | +0.53 | −0.35 | +2.86 | −0.82 |
| 5 | +0.54 | +7.16 | +1.07 | −1.07 |
| 6 | −0.49 | −5.49 | −2.08 | +0.31 |
| 7 | +0.05 | −4.31 | −1.34 | +0.79 |
| 8 | +0.13 | −0.51 | +3.61 | +0.66 |
| 9 | −0.13 | +12.85 | −0.28 | +0.58 |
| 10 | −1.01 | −10.08 | +4.36 | −1.33 |
| 11 | +1.46 | −11.51 | −3.05 | −0.15 |
| 12 | +0.24 | −2.00 | −4.09 | +1.37 |
| 13 | −0.04 | −5.30 | −5.50 | −0.50 |
| 14 | +1.20 | −12.36 | −1.05 | −0.64 |
| 15 | −0.47 | −0.50 | +3.96 | +0.85 |
| 16 | −1.05 | −0.92 | +1.17 | +0.01 |

wall（`100*(B−A)/A`）：

| pair | C1 | C3 | C4 | C6 |
|---:|---:|---:|---:|---:|
| 1 | +1.30 | +3.29 | +0.07 | +2.75 |
| 2 | −1.75 | −0.01 | −0.44 | −4.84 |
| 3 | −0.22 | +2.91 | −0.24 | +2.41 |
| 4 | +0.66 | −0.43 | +2.36 | −1.92 |
| 5 | +1.47 | +3.51 | +0.65 | +0.47 |
| 6 | −0.96 | −3.08 | −1.69 | −0.42 |
| 7 | +0.33 | −1.47 | −1.79 | +1.27 |
| 8 | +1.12 | −0.47 | +1.87 | +0.20 |
| 9 | −1.34 | +6.24 | −0.28 | +0.95 |
| 10 | −2.50 | −5.73 | +1.59 | −1.30 |
| 11 | +2.81 | −5.90 | −2.34 | −0.97 |
| 12 | −0.56 | −1.16 | −0.41 | −0.21 |
| 13 | −2.09 | −2.92 | −2.77 | −0.74 |
| 14 | +1.14 | −7.14 | −0.83 | −0.86 |
| 15 | +0.99 | −0.63 | +1.02 | +2.44 |
| 16 | −0.99 | −0.95 | +1.19 | −0.20 |

C3 的 CPU 逐对值（+7.71、+4.85、+7.16、+12.85、−10.08、−11.51、−12.36）与 cycles/instr 的稳定离散度一起，说明其 CPU 时间波动不是工作量变化；这是 `ENVIRONMENT_BLOCKED` 判词的直接证据。

## 附录 C：原始数据路径

- C1：`p1/aa/C1-sluice-copy-4k-d4-w1/{arms.jsonl,meta.json,pairs.tsv,raw/}`
- C2：`p1/aa/C2-w02-tp-4k-d16/{...}`
- C3：`p1/aa/C3-w02-uring-4k-d16/{...}`
- C4：`p1/aa/C4-w03-uring-4k-d16/{...}`
- C5：`p1/aa/C5-w02-tp-4k-d1/{...}`
- C6：`p1/aa/C6-w02-uring-4k-d1/{...}`
- smoke：`p1/aa/smoke/` **未纳入**本报告。
- 台账：`docs/evidence/621-progress-wait-path-20261118/CAMPAIGN-LEDGER.md`（§9.1、§10.4）。
- 构建/测试回执：`p1/build-manifest.md`、`p1/logs/`；本报告没有重跑。
