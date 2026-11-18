# 621 P2 补测 A/A 独立统计复算（AA-CALIBRATION-2）

- 数据：`p1/aa2/` 五格（C2b、C3b、C5b、C7、C8），每格 16 对、32 arm。
- 协议与判据：继承 `p1/AA-CALIBRATION.md` §1.2–1.4（配对差、统计方法、A/A 安静度门）、§2.2（独立复算路径）、§5（冻结带格式）。本报告不改变预注册判据。
- 独立性：从 `arms.jsonl`、`raw/*.perf`、`raw/*.jsonl`、`pairs.tsv`（仅交叉检查）、`meta.json` 重算；没有读取 runner 的聚合 summary。`tools/recompute_independent.py` 不 import `paired_session.py`/`aa_recompute.py`。另用第二份一次性脚本（`/tmp/aa2_audit/audit_aa2.py`，不入库）独立复核配对差、SD、精确置换 p、raw 回执。
- 重要更正：C3b **复现了 §1.4-7 的统计触发**，但**没有复现“持续频率 excursion”本身**。C3b 启动后的 `scaling_cur_freq` 序列稳定在约 3.43–3.44 GHz（within-run CV 1.09%；两个被测 CPU 共 4556 个启动后样本，仅 1 个 <2.5 GHz）；CPU/task 的变异主要由 **sys time（内核时间）** 解释（sys CV 5.97%、user CV 3.47%、cycles CV 1.07%；配对 CPU 差与 sys 差相关 0.983）。因此 C3b 仍按预注册判据判 `ENVIRONMENT_BLOCKED`，但不应再把机制写成“已证实的频率下降”。见 §4。

本报告路径相对 `docs/evidence/621-progress-wait-path-20261118/`。

---

## 0. 结论摘要

| 格 | 有效对 | 判词 | 关键依据 |
|---|---:|---|---|
| C2b-w02-tp-4k-d16-w4 | 16 | **NOISY** | wall SD 3.380 pp > 3.0；instr 漂移 p=0.0017（前 8 对 −0.228%、后 8 对 +0.108%）；CPU SD 3.414 pp 接近 3.5 上限 |
| C3b-w02-uring-4k-d16 | 16 | **ENVIRONMENT_BLOCKED** | CPU SD 5.526 pp、cycles SD 1.319 pp，CPU CV/cycles CV=3.94，instr CV 0.086% → §1.4-7 触发；CPU hw 2.597%、wall hw 1.436% |
| C5b-w02-tp-4k-d1-w1 | 16 | **QUIET**（条件基线） | CPU SD 0.964、instr SD 0.002、wall SD 1.182；CPU hw 0.458%、instr hw 0.001%、wall hw 0.564%；但 cycles SD 3.107，且 runner 报 `background_load_exceeds_measured_cpus` |
| C7-w02-uring-4k-d64 | 16 | **ENVIRONMENT_BLOCKED** | CPU SD 9.782 pp、cycles SD 1.889 pp，比例 4.50；CPU hw 4.573%、wall hw 2.282%；CPU/wall 的 A/A 均值不正（CPU +5.04%），cycles 均值 −0.49% 居中 |
| C8-w02-tp-4k-d64-w4 | 16 | **NOISY** | CPU SD 4.938 pp > 3.5、wall SD 4.891 pp > 3.0、cycles SD 6.804 pp；无顺序/位置显著项 |

要点：

1. **C5b 是 P2 唯一安静格**，也是首个可用的 TP d1 w1 基线；但 cycles 不安静（SD 3.107 pp），且背景负载警告使“正式冻结”应加条件。
2. **C2b 的修正 filter 已命中实际 case `W02-threadpool-4k-d16-w4`**（32/32 oracle 通过），但格本身 NOISY；TP d16 w4 仍没有可用 A/A 基线。
3. **C3b 比 P1 C3 更安静**（CPU SD 7.027→5.526、cycles SD 1.991→1.319、wall SD 3.653→3.094），但仍越过 §1.4-7 与 CPU 噪声门；cycles 带（hw 0.621%、MDE80 0.988%）是目前最好的 uring d16 cycles 候选带，但**未认证**，用前须做 cycles-only A/A。
4. **C7 是 d64 首次实测**。16 对 cycles：SD 1.889、hw 0.893%（bootstrap）/1.007%（t）、MDE80 1.416%；CPU 不可用（hw 4.573%、MDE80 7.331%）；wall 只对约 ≥3.6% 的效应有 80% 功效。M-53 的 d64 分辨力只可能走 cycles，且 16 对在保守 t 判据下**差一对**（n_half(2%)=17）；建议 17–20 对 cycles-only A/A。1% 效应需 58 对。
5. **频率 excursion 问题**：统计触发复现，直接频率 excursion 未复现；机制更可能是 sys time 波动（§4）。这影响 W1/W3 的指标选择：uring 格不要用 process CPU，优先 cycles 或 user CPU。

---

## 1. 协议与判据（继承 P1，不改阈值）

### 1.1 指标与配对差

主指标与 P1 §1.2 相同：

- **CPU/task**：`cpu_user_s + cpu_sys_s`（`RUSAGE_CHILDREN` 的 user+sys）；不直接信任 runner 的 `cpu_us`，从两个原始字段重算，并核对 `cpu_us` 与 wall 时间戳。
- **user instructions/task**：直接解析 `raw/*.perf` 的 `instructions:u`。
- **wall/task**：arm 的 `wall_s`（monotonic），与 `start`/`end` 时间戳差核对（容差 `max(20 ms, 2%)`）。
- **cycles/task**（辅助）：直接解析 `raw/*.perf` 的 `cycles:u`。

配对差：

```
d_i = 100 * (B_i - A_i) / A_i
```

对称分母敏感性 `200*(B_i-A_i)/(A_i+B_i)` 与主定义的 hw 差多数 <0.1 pp；最大是 C7 CPU 0.169 pp（C3b CPU 0.098 pp），`symHalfWidth` 与主 hw 同量级，未改变任何判词。

### 1.2 统计方法

- 固定种子 **621**；bootstrap **10,000** 次，percentile 95% CI，半宽 `hw=(q97.5-q2.5)/2`；种子由 `sha256(cell|metric)` 派生（`tools/recompute_independent.py:164-183`）。
- AB/BA：组均值差 + 10,000 次置换 p。
- 位置效应：pair 内“第二臂 − 第一臂”（AB 为 B−A，BA 为 A−B），10,000 次符号翻转 p。
- 漂移：pair 序号对 `d_i` 的 OLS 斜率 + 10,000 次置换 p；前 8/后 8 对均值。
- MME：配对差样本 SD + Student-t 分位数（`tools/recompute_independent.py:228-266`）：
  - 80% 功效、双侧 α=0.05：`(t_.975 + t_.80) * SD / sqrt(n) <= δ`；
  - 95% CI 半宽 ≤ δ/2：`t_.975 * SD / sqrt(n) <= δ/2`；
  - `MDE80(n=16)`：16 对时 80% 功效可分辨的最小 δ。
- **本报告表格中的 `hw` 是 seed 621 的 bootstrap 半宽**（与 P1 §5 格式一致）；`n80/n_half` 与 `MDE80` 用 t 公式。两者在 n=16 时可差 0.1–0.15 pp（例如 C7 cycles：bootstrap hw 0.893%、t hw 1.007%），边缘判据必须同时看两者，详见 §6.4。

### 1.3 A/A 安静度门（同 P1 §1.4）

1. 有效配对 ≥12 才计算；≥15 才可作正式基线；<12 → `INCONCLUSIVE`。
2. 每个有效 arm 的 oracle 必须通过；bench 每 arm 必须有 CAL + 2 warmup + 7 measured，且 target case ID 正确；任何失败 → `INCONCLUSIVE`。
3. 所有决定性指标的 perf 行 `percent_enabled=100.00`，无 `<not counted>`/`<not supported>`/`scaled`；否则 `NOT_POWERED`/`ENVIRONMENT_BLOCKED`。
4. CPU/instr/wall 中任一漂移置换 p<0.01 → `NOISY`。
5. 任一指标 |AB−BA|>1 pp 且置换 p<0.01 → `ORDER_SENSITIVE`。
6. 噪声等级（用 SD(d)，不用 hw）：
   - `QUIET`：SD_CPU ≤1.5 pp、SD_wall ≤2.5 pp、SD_instr ≤0.05 pp；
   - `QUALIFIED`：SD_CPU ≤3.5 pp、SD_wall ≤3.0 pp；
   - 超过 → `NOISY`。
7. 若 CPU/task CV > 2× cycles CV 且 instructions CV <0.5%，判为有效频率变化；该格 CPU/wall 记 `ENVIRONMENT_BLOCKED`。

**判据执行说明**：

- `verdict_for`（`tools/recompute_independent.py:1214-1311`）的 gate 顺序是：样本量 → oracle → 频率 → 漂移/噪声 → 顺序 → QUIET/QUALIFIED。C3b/C7 的 `sd_cpu` 均 >1.5，所以频率门先于 `NOISY` 触发，判 `ENVIRONMENT_BLOCKED`；即使不按频率门，它们也会因 CPU SD >3.5 判 `NOISY`。
- 脚本的 `verdict_for` 没有直接消费 `perfAudit.scaledRows`。本报告另行验证：五格共 480 个 perf event 行（5×96）全部 `percent_enabled=100.00`，0 个 scaled，0 个 bad marker；因此判词不受这个工具缺口影响。
- 脚本的 `validity_of_arm` 仍读取 runner 的 `oracle.bench.target_records/warmup_records/measured_records` 字段作部分校验；本报告另用 raw JSONL 逐 arm 复核 CAL、2 warmup、7 measured、单一 target case ID、所有 target `ok`，32/32 一致（§3.4）。因此 oracle 判词不依赖 runner 的聚合字段。

### 1.4 本报告不做的

- 不做 A/B 判词；A/A 同 binary，没有候选 SHA、mutant、收益结论。
- 不把 A/A 的均值（接近 0）当效应；不把 16 对的非显著结果当“无效应”。
- 不做跨介质/跨机器外推：全部是 tmpfs、page-cache resident、4 KiB；没有磁盘/NVMe、64K、W-03 新格。
- 不读、不引用两个未跟踪目录（`621-round2-R1/`、`e5-fullpath-machine-work/`）。

---

## 2. 数据与独立复算

### 2.1 原始文件清单

| 格 | `arms.jsonl` | `pairs.tsv` | `raw/` 文件数 | raw 组成 |
|---|---:|---:|---:|---|
| C2b | 32 行 | 32 行 + header | 160 | 32 × (cmd/jsonl/perf/stderr/stdout) |
| C3b | 32 | 32 + header | 160 | 同上 |
| C5b | 32 | 32 + header | 160 | 同上 |
| C7 | 32 | 32 + header | 160 | 同上 |
| C8 | 32 | 32 + header | 160 | 同上 |

所有 `raw/*.perf`、`*.jsonl`、`*.stdout`、`*.stderr` 路径在 `arms.jsonl` 中都存在（0 missing）；每格 32 个 arm 的 `start`/`end` 串行、无重叠（复算检查：0 overlap，gap 0.01–0.04 s）。

### 2.2 独立复算路径与命令

在 `docs/evidence/621-progress-wait-path-20261118/` 下执行：

```sh
python3 -I tools/recompute_independent.py --aa-root p1/aa2 --json-out /tmp/aa2_recompute.json
python3 -I tools/recompute_independent.py --aa-root p1/aa --json-out /tmp/aa1_recompute.json
python3 -I /tmp/aa2_audit/audit_aa2.py p1/aa2 --json-out /tmp/aa2_audit/audit_aa2.json
```

`recompute_independent.py` 的路径：

1. **perf 指标直接读 raw 文件**：`instructions:u`、`cycles:u`、`task-clock:u` 从 `raw/pair-NN-{A,B}.perf` 解析；`arms.jsonl.perf.rows` 只用于对照。
2. **CPU 从原始字段重算**：`cpu_user_s + cpu_sys_s`，检查 `cpu_us` 与重算值差 ≤2 µs；`wall_s` 与 `end-start` 对照。
3. **bench 回执直接读 raw JSONL**：每 arm 重新解析 CAL、target case ID、warmup=2、measured=7、所有 target `ok=true`、`failure` 为空。
4. **交叉检查**：`pairs.tsv` 与 `arms.jsonl` 的 `valid/wall_s/cpu_user_s/cpu_sys_s/perf_instructions` 对照，32/32 行 checked，0 mismatch。
5. **独立 validity vs runner validity**：160 个 arm 全部一致，0 mismatch。
6. **第二份复核**：`/tmp/aa2_audit/audit_aa2.py` 不 import 证据工具，独立实现配对差、SD、精确 AB/BA 置换（C(16,8)=12,870 组合）、精确符号翻转（2^16）、200k 漂移置换；并独立解析 raw JSONL 回执。它与 `recompute_independent.py` 的 mean/median/SD 逐位一致（0.000 差），bootstrap hw 在 3 个不同种子下相差 ≤0.06 pp（C8 cycles 最大 0.05 pp），不改变任何判词。

### 2.3 校验结果

| 格 | 有效对 | 无效 arm | 独立 vs runner | pairs.tsv 对照 | oracle 通过 arm | CAL | target 记录/arm | target ok | perf 行 scaling |
|---|---:|---:|---|---|---:|---|---:|---|---:|
| C2b | 16 | 0 | 0 mismatch | 32/32，0 mismatch | 32/32 | 通过 | 9（2+7） | 是 | 0/96 |
| C3b | 16 | 0 | 0 mismatch | 32/32，0 mismatch | 32/32 | 通过 | 9（2+7） | 是 | 0/96 |
| C5b | 16 | 0 | 0 mismatch | 32/32，0 mismatch | 32/32 | 通过 | 9（2+7） | 是 | 0/96 |
| C7 | 16 | 0 | 0 mismatch | 32/32，0 mismatch | 32/32 | 通过 | 9（2+7） | 是 | 0/96 |
| C8 | 16 | 0 | 0 mismatch | 32/32，0 mismatch | 32/32 | 通过 | 9（2+7） | 是 | 0/96 |

- 五格共 480 个 perf event 行；`percent_enabled` 全部 `100.00`；`badMarkers=[]`。**没有 perf 缩放/外推值。**
- 无效 arm 原因：**无**。`meta.excluded_arms=[]`；独立 validity 也为 0 invalid。
- C2b/C5b 的修正 filter 分别命中 `W02-threadpool-4k-d16-w4`、`W02-threadpool-4k-d1-w1`；C3b/C7/C8 分别命中 `W02-uring-4k-d16`、`W02-uring-4k-d64`、`W02-threadpool-4k-d64-w4`。每格 raw JSONL 只有一个 target case ID（与 filter 一致）。
- 每格 32 arm 全部 `rc=0`、`timed_out=false`；`cpu_us` 与重算值差 ≤2 µs；`wall_s` 与时间戳差在容差内。

### 2.4 元数据对照（§10.4 相关项）

| 项 | C2b | C3b | C5b | C7 | C8 |
|---|---|---|---|---|---|
| case ID | `W02-threadpool-4k-d16-w4` | `W02-uring-4k-d16` | `W02-threadpool-4k-d1-w1` | `W02-uring-4k-d64` | `W02-threadpool-4k-d64-w4` |
| backend | threadpool | uring | threadpool | uring | threadpool |
| block / depth / workers | 4096 / 16 / 4 | 4096 / 16 / 0 | 4096 / 1 / 1 | 4096 / 64 / 0 | 4096 / 64 / 4 |
| warm/cold | page_cache_resident | page_cache_resident | page_cache_resident | page_cache_resident | page_cache_resident |
| 介质 | tmpfs `/tmp/621-p1-aa2` | 同左 | 同左 | 同左 | 同左 |
| BASE SHA | `5d94f233…` | 同左 | 同左 | 同左 | 同左 |
| binary SHA（A=B） | `03dbef09…` | 同左 | 同左 | 同左 | 同左 |
| liburing linked | true | true | true | true | true |
| governor before/after | performance/performance | 同左 | 同左 | 同左 | 同左 |
| no_turbo before/after | 0/0 | 0/0 | 0/0 | 0/0 | 0/0 |
| runner warning | `background_load_exceeds_measured_cpus` | 无 | `background_load_exceeds_measured_cpus` | `background_load_exceeds_measured_cpus` | `background_load_exceeds_measured_cpus` |
| `saturating` | 未记录 | 未记录 | 未记录 | 未记录 | 未记录 |
| IRQ 亲和性 | 只有冻结快照，无 per-IRQ | 同左 | 同左 | 同左 | 同左 |
| 频率采样 | per-arm 20 ms `scaling_cur_freq` 序列 | 同左 | 同左 | 同左 | 同左 |

`background_load_exceeds_measured_cpus` 的 runner 条件是 `max(loadavg1) > len(measured_cpus)`（`tools/paired_session.py:1428-1431`）；measured CPUs 是 2、3，共 2 个。注意 loadavg 包含 workload 自身，C2b/C8 的 TP w4 在 2 个 CPU 上本来就可能 >2；但 C5b 是 w1 也到 2.71，说明确实存在额外背景负载。详见 §3.3 与 §7。

---

## 3. 逐格结果

### 3.1 主统计表

单位：mean/median 是配对差 `100*(B−A)/A` 的百分数；SD 是配对差的百分点（pp）；hw 是 seed 621 的 bootstrap 95% CI 半宽（%）；MDE80 是 n=16 时 80% 功效可分辨的最小效应（%）。

#### C2b-w02-tp-4k-d16-w4

| 指标 | n | mean % | median % | SD pp | hw % | MDE80 % | AB mean | BA mean | AB−BA | p |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| CPU/task | 16 | +1.417 | +1.154 | 3.414 | 1.604 | 2.558 | +2.546 | +0.288 | +2.258 | 0.1979 |
| instr/task | 16 | -0.060 | -0.030 | 0.336 | 0.159 | 0.252 | -0.083 | -0.038 | -0.045 | 0.7896 |
| wall/task | 16 | +1.381 | +1.067 | 3.380 | 1.580 | 2.533 | +2.525 | +0.236 | +2.290 | 0.1935 |
| cycles/task | 16 | +1.426 | +0.907 | 5.144 | 2.401 | 3.855 | +3.318 | -0.466 | +3.784 | 0.1381 |

| 指标 | 位置效应 mean | 正号 | 符号翻转 p | 漂移斜率/pair | 漂移 p | 前8对 | 后8对 | 后−前 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| CPU/task | +1.129 | 12/16 | 0.2195 | -0.1202 | 0.5401 | +1.655 | +1.179 | -0.476 |
| instr/task | -0.022 | 7/16 | 0.7986 | +0.0513 | **0.0017** | -0.228 | +0.108 | +0.336 |
| wall/task | +1.145 | 12/16 | 0.2102 | -0.1282 | 0.4978 | +1.674 | +1.087 | -0.588 |
| cycles/task | +1.892 | 11/16 | 0.1541 | +0.2867 | 0.3131 | +0.032 | +2.821 | +2.789 |

**判词：NOISY**。wall SD 3.380 > 3.0（可用上限），CPU SD 3.414 接近 3.5；instr 漂移 p=0.0017 单独就足以触发 §1.4-4。CPU/wall 的 AB/BA 差（+2.258/+2.290 pp）和位置效应（+1.129/+1.145 pp）都不显著，说明噪声不是由 AB/BA 顺序或“第二臂更慢”主导；但 instr 的后半程上漂是真实的非平稳性。C2b 的 filter 修正成功（32/32 oracle），但格本身不能作基线。

#### C3b-w02-uring-4k-d16

| 指标 | n | mean % | median % | SD pp | hw % | MDE80 % | AB mean | BA mean | AB−BA | p |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| CPU/task | 16 | -0.417 | +1.205 | 5.526 | 2.597 | 4.141 | -1.329 | +0.494 | -1.823 | 0.5227 |
| instr/task | 16 | +0.014 | -0.009 | 0.118 | 0.056 | 0.088 | +0.043 | -0.015 | +0.058 | 0.3462 |
| wall/task | 16 | -0.159 | +0.958 | 3.094 | 1.436 | 2.319 | -0.473 | +0.155 | -0.628 | 0.7014 |
| cycles/task | 16 | +0.052 | -0.056 | 1.319 | 0.621 | 0.988 | -0.009 | +0.113 | -0.122 | 0.8587 |

| 指标 | 位置效应 mean | 正号 | 符号翻转 p | 漂移斜率/pair | 漂移 p | 前8对 | 后8对 | 后−前 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| CPU/task | -0.911 | 7/16 | 0.5051 | -0.3187 | 0.3073 | +1.265 | -2.100 | -3.366 |
| instr/task | +0.029 | 9/16 | 0.3353 | +0.0078 | 0.2281 | -0.026 | +0.054 | +0.080 |
| wall/task | -0.314 | 8/16 | 0.6907 | -0.1809 | 0.2971 | +0.795 | -1.113 | -1.908 |
| cycles/task | -0.061 | 8/16 | 0.8578 | +0.0459 | 0.5447 | -0.186 | +0.290 | +0.476 |

**判词：ENVIRONMENT_BLOCKED**。CPU CV 4.230%、cycles CV 1.073%、instr CV 0.086%，比例 3.94 >2；§1.4-7 触发。CPU SD 5.526 >3.5，wall SD 3.094 >3.0；即便不看频率门也会判 NOISY。AB/BA（−1.823 pp，p=0.523）和位置效应（−0.911 pp，p=0.505）都不显著；漂移 p 0.31/0.30/0.23，平稳性比 P1 C3 好。cycles 的 SD 1.319、hw 0.621%、MDE80 0.988% 是 uring d16 最好的一组；但**本格 CPU/wall 被阻塞，cycles 只能作条件性诊断带**，不能直接当已认证基线。机制解释见 §4。

#### C5b-w02-tp-4k-d1-w1

| 指标 | n | mean % | median % | SD pp | hw % | MDE80 % | AB mean | BA mean | AB−BA | p |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| CPU/task | 16 | +0.339 | +0.448 | 0.964 | 0.458 | 0.723 | +0.338 | +0.340 | -0.002 | 0.9975 |
| instr/task | 16 | -0.001 | -0.000 | 0.002 | 0.001 | 0.002 | -0.002 | -0.000 | -0.001 | 0.2304 |
| wall/task | 16 | +0.287 | +0.355 | 1.182 | 0.564 | 0.886 | +0.084 | +0.490 | -0.405 | 0.5174 |
| cycles/task | 16 | +1.256 | +1.520 | 3.107 | 1.481 | 2.328 | +1.056 | +1.457 | -0.401 | 0.8017 |

| 指标 | 位置效应 mean | 正号 | 符号翻转 p | 漂移斜率/pair | 漂移 p | 前8对 | 后8对 | 后−前 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| CPU/task | -0.001 | 8/16 | 0.9963 | -0.0568 | 0.2934 | +0.636 | +0.042 | -0.595 |
| instr/task | -0.001 | 7/16 | 0.2482 | -0.0002 | 0.1755 | -0.000 | -0.001 | -0.001 |
| wall/task | -0.203 | 8/16 | 0.5082 | -0.0688 | 0.3007 | +0.595 | -0.022 | -0.617 |
| cycles/task | -0.201 | 8/16 | 0.8101 | +0.1422 | 0.4206 | +0.463 | +2.050 | +1.588 |

**判词：QUIET（条件基线）**。CPU SD 0.964 ≤1.5、wall 1.182 ≤2.5、instr 0.002 ≤0.05；CPU hw 0.458%、instr hw 0.001%、wall hw 0.564%。AB/BA 差 −0.002 pp（p=0.998），位置效应 −0.001 pp（p=0.996），漂移 p 0.29/0.18/0.30：没有顺序、位置或漂移信号。**但**：cycles SD 3.107 pp、hw 1.481%、MDE80 2.328%，不能当窄带；runner 报 `background_load_exceeds_measured_cpus`（loadavg1 最大 2.71）。所以 C5b 可作为 W1/W3 的**条件**基线（CPU 为决定性指标），正式使用前建议在同环境复跑 16 对或至少记录背景负载受控。

#### C7-w02-uring-4k-d64

| 指标 | n | mean % | median % | SD pp | hw % | MDE80 % | AB mean | BA mean | AB−BA | p |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| CPU/task | 16 | +5.043 | +4.396 | 9.782 | 4.573 | 7.331 | +5.668 | +4.419 | +1.250 | 0.7985 |
| instr/task | 16 | -0.009 | -0.009 | 0.020 | 0.009 | 0.015 | -0.015 | -0.004 | -0.011 | 0.2882 |
| wall/task | 16 | +2.542 | +2.576 | 4.861 | 2.282 | 3.643 | +2.996 | +2.088 | +0.908 | 0.7175 |
| cycles/task | 16 | -0.489 | -0.452 | 1.889 | 0.893 | 1.416 | -0.850 | -0.128 | -0.722 | 0.4637 |

| 指标 | 位置效应 mean | 正号 | 符号翻转 p | 漂移斜率/pair | 漂移 p | 前8对 | 后8对 | 后−前 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| CPU/task | +0.625 | 10/16 | 0.8263 | -0.0532 | 0.9215 | +6.779 | +3.307 | -3.472 |
| instr/task | -0.005 | 6/16 | 0.3170 | +0.0009 | 0.3987 | -0.016 | -0.002 | +0.013 |
| wall/task | +0.454 | 10/16 | 0.7459 | -0.0613 | 0.8232 | +3.531 | +1.553 | -1.979 |
| cycles/task | -0.361 | 6/16 | 0.4552 | -0.0511 | 0.6409 | -0.485 | -0.494 | -0.009 |

**判词：ENVIRONMENT_BLOCKED**。CPU CV 7.121%、cycles CV 1.584%、instr CV 0.017%，比例 4.50；§1.4-7 触发。CPU SD 9.782 >3.5、wall SD 4.861 >3.0；即便不看频率门也会判 NOISY。**额外问题**：CPU/wall 的 A/A 均值不为零——CPU +5.043%（bootstrap CI [0.46, 9.60]，t CI [−0.17, 10.26]）、wall +2.542%（bootstrap CI [0.28, 4.85]，t CI [−0.05, 5.13]）。AB 与 BA 的均值同号（CPU +5.67/+4.42，wall +3.00/+2.09），位置效应 p=0.83/0.75，说明这不是 AB/BA 顺序或“第二臂更慢”，而是标签/时间混杂或重尾伪影。**cycles 是唯一居中的指标**（mean −0.489%，bootstrap CI 含 0），也是唯一可能用于 M-53 的 d64 带；但 16 对 t-hw 1.007% 刚好超过 2%/2=1%，按保守判据差一对（§6.4）。

#### C8-w02-tp-4k-d64-w4

| 指标 | n | mean % | median % | SD pp | hw % | MDE80 % | AB mean | BA mean | AB−BA | p |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| CPU/task | 16 | +0.098 | -0.144 | 4.938 | 2.323 | 3.700 | +0.348 | -0.152 | +0.500 | 0.8459 |
| instr/task | 16 | +0.030 | +0.017 | 0.242 | 0.114 | 0.182 | +0.089 | -0.030 | +0.119 | 0.3416 |
| wall/task | 16 | +0.098 | -0.092 | 4.891 | 2.280 | 3.666 | +0.366 | -0.170 | +0.535 | 0.8420 |
| cycles/task | 16 | +0.475 | +1.126 | 6.804 | 3.259 | 5.099 | +0.806 | +0.143 | +0.663 | 0.8490 |

| 指标 | 位置效应 mean | 正号 | 符号翻转 p | 漂移斜率/pair | 漂移 p | 前8对 | 后8对 | 后−前 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| CPU/task | +0.250 | 6/16 | 0.8339 | -0.5088 | 0.0553 | +3.202 | -3.007 | -6.209 |
| instr/task | +0.060 | 11/16 | 0.3151 | -0.0206 | 0.1204 | +0.132 | -0.073 | -0.205 |
| wall/task | +0.268 | 6/16 | 0.8286 | -0.4997 | 0.0611 | +3.153 | -2.957 | -6.110 |
| cycles/task | +0.331 | 7/16 | 0.8369 | -0.6538 | 0.0806 | +4.509 | -3.560 | -8.070 |

**判词：NOISY**。CPU SD 4.938 >3.5、wall SD 4.891 >3.0、cycles SD 6.804；无 AB/BA 或位置显著项（p 0.83–0.85），但漂移 p 0.055/0.061/0.081 接近 0.01 阈值之外的“边缘”，前 8 对 +3.2% → 后 8 对 −3.0%（CPU/wall），不是稳定基线。C8 是 TP d64 w4 对照，不能给 M-53 提供 d64 TP 基线。

### 3.2 诊断汇总

| 格 | CPU AB−BA (p) | CPU 位置 (p) | CPU 漂移 (p) | instr 漂移 (p) | wall 位置 (p) | 顺序/位置显著项 |
|---|---|---|---|---|---|---|
| C2b | +2.258 (0.198) | +1.129 (0.220) | -0.476 (0.540) | **+0.336 (0.0017)** | +1.145 (0.210) | 无（但 instr 漂移触发 NOISY） |
| C3b | -1.823 (0.523) | -0.911 (0.505) | -3.366 (0.307) | +0.080 (0.228) | -0.314 (0.691) | 无 |
| C5b | -0.002 (0.998) | -0.001 (0.996) | -0.595 (0.293) | -0.001 (0.176) | -0.203 (0.508) | 无 |
| C7 | +1.250 (0.799) | +0.625 (0.826) | -3.472 (0.922) | +0.013 (0.399) | +0.454 (0.746) | 无（但 A/A 均值不正） |
| C8 | +0.500 (0.846) | +0.250 (0.834) | -6.209 (0.055) | -0.205 (0.120) | +0.268 (0.829) | 无（漂移边缘） |

- 五格都没有 |AB−BA|>1 pp 且 p<0.01 的顺序敏感项。P1 C6 的 wall 位置效应（+1.13 pp，p=0.0049；AB−BA +2.265 pp，p=0.0069）在 P2 新格中没有复现。
- C2b 的 instr 漂移是 P2 唯一 p<0.01 的漂移项；C8 的 CPU/wall 漂移 p≈0.055–0.061，未过门但值得注意。
- 第二份复核脚本的精确 AB/BA 置换 p（C(16,8)=12,870）与精确符号翻转 p（2^16）和上表 Monte Carlo p 一致到 0.01 量级；例如 C5b CPU AB/BA 精确 p=0.997、位置精确 p=0.997；C2b instr 漂移 200k 置换 p=0.0016。没有因 Monte Carlo 误差改变判词。

### 3.3 频率、温度、背景负载

| 格 | runner warning | loadavg1 min/med/max | ps 进程数 | ps 最高 %CPU | pkg temp min/max °C | CPU2 中位频率范围 MHz | CPU3 中位频率范围 MHz | 启动后 within-run CV mean/max |
|---|---|---|---:|---:|---|---:|---:|---:|
| C2b | `background_load_exceeds_measured_cpus` | 0.10/1.60/2.51 | 391–394 | 47.3 | 46/57 | 3436–3437 | 3436–3437 | 1.10/3.68 |
| C3b | 无 | 1.39/1.72/1.96 | 395–395 | 41.1 | 56/61 | 3436–3437 | 3436–3437 | 1.09/3.68 |
| C5b | `background_load_exceeds_measured_cpus` | 1.40/1.62/2.71 | 394–395 | 38.8 | 53/60 | 3437–3438 | 3437–3439 | 0.96/1.44 |
| C7 | `background_load_exceeds_measured_cpus` | 1.89/1.92/2.01 | 385–391 | 47.0 | 56/61 | 3436–3438 | 3436–3438 | 1.08/1.57 |
| C8 | `background_load_exceeds_measured_cpus` | 1.93/2.85/3.45 | 385–386 | 100.0 | 57/60 | 3436–3437 | 3436–3437 | 1.11/1.71 |

- **频率**：五格的启动后 CPU2/3 中位频率都在 3.436–3.439 GHz 的窄带内；within-run CV 均值 0.96–1.11%，最大值 1.44–3.68%。C3b 的最大 3.68% 来自单个 2.423 GHz 样本；C2b 的最大 3.68% 来自 pair 1/arm A 启动阶段的短暂 2.89 GHz 段（仍 ≥2.5 GHz），不是同样的 2.423 GHz 异常。两者都不是持续下探。`scaling_cur_freq` 是请求频率接口，不是实际频率计数器；它只能排除“持续、大幅的频率下降”，不能证明实际频率恒定。详见 §4。
- **温度**：最高 61°C，远低于 80°C 阈值；没有热节流迹象。C2b 最低 46°C（它是最先跑的格），C5b 53–60°C，C7/C8 56–61°C。
- **背景负载**：`ps_top_by_cpu` 每对都是 `zcode-cli`（+5% CPU，16/16 对）和 `python3`（runner，约 39–47% CPU，1/16 对）；C8 的 `ps` 自身 100% 是快照进程本身。runner 绑定 CPU0，benchmark 绑定 CPU2/3，但 zcode-cli 等未被排除在 2/3 之外。C2b/C5b/C7/C8 的 loadavg1 最大值超过 2 个 measured CPU，触发 runner 警告；C3b 最大 1.96，未触发。注意 loadavg 包含 workload 自身（C2b/C8 的 TP w4 在 2 个 CPU 上本来就可能 >2），所以警告不是“一定有外部负载”，但 C5b 的 w1 到 2.71 说明存在额外负载。
- **C5b 的 QUIET 与背景负载**：C5b 的 CPU/instr/wall 统计很安静（CPU SD 0.964），但 warning 仍在。按 §1.4 判 QUIET 成立；作为 W1/W3 的正式基线前，建议复跑或至少把“背景负载受控”作为预注册条件。这是本报告对 C5b 唯一的保守保留。

### 3.4 Oracle 与回执

| 格 | CAL 通过 | warmup/arm | measured/arm | target case ID | 所有 target `ok` | `failure` 空 | 独立回执问题 |
|---|---|---:|---:|---|---|---|---:|
| C2b | 32/32 | 2 | 7 | `W02-threadpool-4k-d16-w4` | 是 | 是 | 0 |
| C3b | 32/32 | 2 | 7 | `W02-uring-4k-d16` | 是 | 是 | 0 |
| C5b | 32/32 | 2 | 7 | `W02-threadpool-4k-d1-w1` | 是 | 是 | 0 |
| C7 | 32/32 | 2 | 7 | `W02-uring-4k-d64` | 是 | 是 | 0 |
| C8 | 32/32 | 2 | 7 | `W02-threadpool-4k-d64-w4` | 是 | 是 | 0 |

- 每 arm 的 raw JSONL 都有 1 条 CAL + 9 条 target（2 warmup + 7 measured）；target 记录的 `case_id` 唯一且与 filter 一致；所有 target `ok=true`、`failure=""`。
- 每 arm 的 `raw/*.perf` 都有 `task-clock:u`、`instructions:u`、`cycles:u` 三行，`percent_enabled=100.00`；没有 `<not counted>`/`<not supported>`/`scaled`。
- 每 arm `rc=0`、`timed_out=false`；`raw/*.stdout`、`*.stderr`、`*.cmd` 齐全；`pairs.tsv` 与 `arms.jsonl` 32/32 行一致。
- 因此五格的 oracle 回执均通过；没有任何 arm 因回执失败而无效。

### 3.5 逐格判词的可复核理由

- **C2b NOISY**：`sd wall=3.380 > usable_wall=3.0`；`sd cpu=3.414` 虽 ≤3.5 但接近；`instr drift p=0.0017 <0.01`。三个理由中任意两个独立成立。
- **C3b ENVIRONMENT_BLOCKED**：`cpuCv/cyclesCv=3.94 >2` 且 `instrCv=0.086% <0.5%`；`sd cpu=5.526 > quiet_cpu=1.5` → 频率门先触发。即使把频率门关掉，`sd cpu>3.5`、`sd wall>3.0` 也会判 NOISY。
- **C5b QUIET**：`sd cpu=0.964≤1.5`、`sd wall=1.182≤2.5`、`sd instr=0.002≤0.05`；无漂移、无顺序/位置显著项；n=16≥15。
- **C7 ENVIRONMENT_BLOCKED**：`cpuCv/cyclesCv=4.50 >2` 且 `instrCv=0.017% <0.5%`；`sd cpu=9.782 >1.5` → 频率门触发；即使关掉频率门，CPU/wall SD 也远超可用上限。
- **C8 NOISY**：`sd cpu=4.938 >3.5`、`sd wall=4.891 >3.0`；无顺序/位置显著项；漂移 p≈0.055–0.061 未 <0.01，但前/后 8 对差约 6 pp，不能当稳定基线。

---

## 4. C3b 与 P1 C3：频率 excursion 是否复现

### 4.1 对比表

P1 C3 的数字由同一 `recompute_independent.py` 从 `p1/aa/C3-w02-uring-4k-d16/` 的原始逐 arm 记录重算（命令见 §2.2），没有引用 P1 报告的数字。

| 量 | P1 C3 | P2 C3b | 变化 |
|---|---:|---:|---:|
| CPU/task mean % | -1.319 | -0.417 | +0.902 |
| CPU/task median % | -0.718 | +1.205 | +1.922 |
| CPU/task SD pp | 7.027 | 5.526 | **-1.501** |
| CPU/task hw % | 3.312 | 2.597 | -0.715 |
| CPU/task MDE80 % | 5.266 | 4.141 | -1.125 |
| CPU/task AB−BA | +5.374 (p=0.129) | -1.823 (p=0.523) | |
| CPU/task drift p | 0.0705 | 0.3073 | |
| instr/task SD pp | 0.143 | 0.118 | -0.025 |
| instr/task hw % | 0.068 | 0.056 | -0.012 |
| instr/task MDE80 % | 0.107 | 0.088 | -0.018 |
| wall/task SD pp | 3.653 | 3.094 | **-0.559** |
| wall/task hw % | 1.725 | 1.436 | -0.289 |
| wall/task MDE80 % | 2.738 | 2.319 | -0.419 |
| wall/task AB−BA | +2.999 (p=0.101) | -0.628 (p=0.701) | |
| wall/task drift p | 0.0527 | 0.2971 | |
| cycles/task SD pp | 1.991 | 1.319 | **-0.672** |
| cycles/task hw % | 0.949 | 0.621 | -0.328 |
| cycles/task MDE80 % | 1.492 | 0.988 | -0.504 |
| cycles/task AB−BA | -1.217 (p=0.244) | -0.122 (p=0.859) | |
| cycles/task drift p | 0.3534 | 0.5447 | |
| CPU CV % | 5.872 | 4.230 | -1.642 |
| cycles CV % | 1.637 | 1.073 | -0.564 |
| instr CV % | 0.111 | 0.086 | -0.025 |
| CPU CV / cycles CV | 3.587 | 3.941 | +0.354 |
| verdict | ENVIRONMENT_BLOCKED | ENVIRONMENT_BLOCKED | 不变 |

### 4.2 统计触发：复现

C3 与 C3b 都满足 P1 §1.4-7 的机械条件：

- C3：CPU CV 5.872% > 2×1.637%=3.274%，instr CV 0.111% <0.5%；
- C3b：CPU CV 4.230% > 2×1.073%=2.146%，instr CV 0.086% <0.5%。

两轮都是 `ENVIRONMENT_BLOCKED`，CPU/wall 仍然不能作小效应基线。C3b 的 CPU SD 降了 21.4%、cycles SD 降了 33.7%、wall SD 降了 15.3%，说明 P2 的环境/运行比 P1 稍好；但 CPU SD 5.526 仍远高于 3.5 的可用上限，cycles SD 1.319 仍高于 C5b 的 CPU SD 0.964。

### 4.3 直接频率观测：未复现“持续 excursion”

C3b 的 `sampler.freq` 有 20 ms `scaling_cur_freq` 序列（每 arm 约 70 个样本）。启动后（丢弃第一个样本）CPU2/3 的表现：

- 32 个 arm 的中位频率：CPU2 3.436–3.437 GHz、CPU3 3.436–3.437 GHz；arm 间差异约 1 MHz 以内。
- 每个被测 CPU 有 2278 个启动后样本（两个 CPU 共 4556 个）；只有 CPU3 的 1 个样本 <2.5 GHz（2.423 GHz，约占 0.02%）；其余最低 3.19 GHz，主体 3.43–3.44 GHz。
- within-run CV：均值 1.09%，最大 3.68%（来自上述单个 2.423 GHz 样本）；去掉该样本后其余样本的 CV 更小。

P1 C3 只有 min/max/last（没有 series），无法做同样的启动后序列检查；但它的 32 个 arm 的 last 值全部 ≥3.25 GHz（CPU2 3.271–3.493 GHz，CPU3 3.252–3.492 GHz），也没有“结束时仍在低频”的证据。P1 的 min 有 16/32（CPU2）、18/32（CPU3）低于 2.5 GHz，但 min 无法区分启动/空闲采样与持续低频；不能把 min 当作“5–7% 频率波动”的直接证据。

**结论**：按预注册判据，C3b 的**统计触发复现**；按直接频率观测，**没有复现 P1 报告推断的“有效频率约 5–7% 波动”**。C3b 的启动后频率是稳定的 3.43–3.44 GHz 窄带；`scaling_cur_freq` 不是实际频率计数器，所以这不能证明“频率绝对恒定”，但足以否定“持续、大幅频率 excursion”的强断言。

### 4.4 更可能的机制：sys time（内核时间）主导

C3/C3b 的 CPU 变异主要来自 `cpu_sys_s`，不是 user time 或 cycles：

| 量 | P1 C3 | P2 C3b |
|---|---:|---:|
| `cpu_sys_s` CV（32 arm） | 8.57% | 5.97% |
| `cpu_user_s` CV | 4.96% | 3.47% |
| `cycles:u` CV | 1.64% | 1.07% |
| `instructions:u` CV | 0.111% | 0.086% |
| sys 占 process CPU | 76.0% | 76.9% |
| 配对 ΔCPU 与 Δsys 的相关 | 0.991 | 0.983 |
| 配对 ΔCPU SD / Δsys SD | 0.187 s / 0.217 s | 0.153 s / 0.163 s |

`cycles:u` 和 `instructions:u` 只计用户态；`CPU/task` 是 user+sys。uring d16 的 sys 时间占约 77%，sys 时间的跨 arm 波动自然会让 CPU CV 远大于 cycles CV，即使频率完全不变。C3b 的配对 CPU 差与 sys 差相关 0.983，说明 §1.4-7 的触发更像是“内核时间/调度波动”而不是“频率下降”。

这不改变 C3b 的判词（CPU/wall 仍然不稳定，仍然不能作基线），但改变机制表述：

- 正确的说法是：**“CPU/wall 不稳定，§1.4-7 触发；主导成分是 sys time，频率 excursion 未被直接观测复现。”**
- 不应再说：**“已证实有效频率约 5–7% 波动。”**
- 对 W1/W3 的直接影响：uring d16/d64 的 process CPU 不是干净的计算/频率代理；要测 uring 候选，优先用 `cycles` 或 `cpu_user_s`，或者先把 sys-time 来源（IRQ/io-wq/调度/背景负载）隔离。

C7 是同一模式的更强版本：CPU CV 7.12%、sys CV 10.50%、user CV 4.43%、cycles CV 1.58%；配对 ΔCPU 与 Δsys 相关 0.992。C7 的直接频率序列同样是 3.436–3.438 GHz 窄带（within-run CV 1.08%）。所以 C7 的 CPU/wall 阻塞也应按“sys-time/环境”而不是“频率”解释。

---

## 5. 冻结噪声带（更新）

**适用条件**：同 host（E5，kernel 7.3.3-200.fc44.x86_64）、同 `performance` governor / `no_turbo=0`、同 binary SHA、同 workload、同介质、n=16、AB/BA 严格交替。任何条件变化都必须重新做 A/A。

| 格 | CPU hw / SD / MDE80 | instr hw / SD / MDE80 | wall hw / SD / MDE80 | cycles hw / SD / MDE80 | 状态 |
|---|---|---|---|---|---|
| C1 | 0.365% / 0.786pp / 0.589% | 0.005% / 0.012pp / 0.009% | 0.708% / 1.495pp / 1.120% | 1.121% / 2.346pp / 1.758% | **QUIET**（P1，不变） |
| C2 | — | — | — | — | 无数据（P1 filter 未命中；P2 C2b 见下） |
| C3 | 3.312% / 7.027pp / 5.266% | 0.068% / 0.143pp / 0.107% | 1.725% / 3.653pp / 2.738% | 0.949% / 1.991pp / 1.492% | **ENVIRONMENT_BLOCKED**（P1；见 §4） |
| C4 | 1.402% / 3.019pp / 2.262% | 0.200% / 0.425pp / 0.318% | 0.722% / 1.520pp / 1.139% | 3.843% / 8.123pp / 6.088% | **QUALIFIED**（P1） |
| C5 | — | — | — | — | 无数据（P1 filter 未命中；P2 C5b 见下） |
| C6 | 0.471% / 0.997pp / 0.747% | 0.006% / 0.014pp / 0.010% | 0.885% / 1.885pp / 1.413% | 1.368% / 2.876pp / 2.155% | **ORDER_SENSITIVE**（wall；CPU 可用）（P1） |
| **C2b** | 1.604% / 3.414pp / 2.558% | 0.159% / 0.336pp / 0.252% | 1.580% / 3.380pp / 2.533% | 2.401% / 5.144pp / 3.855% | **NOISY**：wall SD 3.380>3.0；instr 漂移 p=0.0017；CPU SD 3.414 接近 3.5。不冻结。 |
| **C3b** | 2.597% / 5.526pp / 4.141% | 0.056% / 0.118pp / 0.088% | 1.436% / 3.094pp / 2.319% | 0.621% / 1.319pp / 0.988% | **ENVIRONMENT_BLOCKED**（CPU/wall）；cycles 仅条件性诊断带（hw 0.621%、MDE80 0.988%），未认证。 |
| **C5b** | 0.458% / 0.964pp / 0.723% | 0.001% / 0.002pp / 0.002% | 0.564% / 1.182pp / 0.886% | 1.481% / 3.107pp / 2.328% | **QUIET**（CPU/instr/wall）；cycles SD 3.107 不作窄带；背景负载警告 → 条件基线。 |
| **C7** | 4.573% / 9.782pp / 7.331% | 0.009% / 0.020pp / 0.015% | 2.282% / 4.861pp / 3.643% | 0.893% / 1.889pp / 1.416% | **ENVIRONMENT_BLOCKED**（CPU/wall）；CPU/wall A/A 均值不正；cycles 仅条件性诊断带（hw 0.893%、MDE80 1.416%）。 |
| **C8** | 2.323% / 4.938pp / 3.700% | 0.114% / 0.242pp / 0.182% | 2.280% / 4.891pp / 3.666% | 3.259% / 6.804pp / 5.099% | **NOISY**：CPU SD 4.938>3.5、wall SD 4.891>3.0、cycles SD 6.804。不冻结。 |

使用规则（在 P1 §5 基础上补充）：

1. 上表 hw 是**该格 n=16 的 bootstrap 95% CI 半宽**；MDE80 是 n=16 的 80% 功效最小效应。若 A/B 用别的 n，用 SD 重新算，不要直接搬 hw。
2. **只有 C5b 的 CPU/instr/wall 可条件冻结**；C5b 的 cycles 不冻结（SD 3.107、MDE80 2.328%）。C5b 的“条件”来自 `background_load_exceeds_measured_cpus`；正式 A/B 前应复跑或把背景负载受控写入预注册。
3. **C3b/C7 的 cycles 带是条件性诊断带，不是已认证基线**：两格 CPU/wall 都 `ENVIRONMENT_BLOCKED`；P1 C3 cycles 也未冻结。若 W1/W3/M-53 要用它们，必须先预注册 cycles-only A/A，并在同一环境复证 SD/hw。
4. **C2b/C8 不冻结**：C2b 的 TP d16 w4 修正 filter 成功但格 NOISY；C8 的 TP d64 w4 也 NOISY。TP d16 w4 / TP d64 仍没有可用基线；不能拿 C5b d1 w1 的带外推到 d16/d64。
5. **C3b/C7 的 CPU/wall 带不能用于任何小效应声明**：C3b CPU MDE80 4.141%、C7 CPU MDE80 7.331%；即使 36 对，C3b CPU hw≈1.87%、C7 CPU hw≈3.31%，仍不满足 2%/2=1% 或 1%/2=0.5%。
6. **C7 的 CPU/wall 还有一个额外问题**：A/A 均值不为零（CPU +5.04%、wall +2.54%），且 AB/BA 与位置检验都不显著；这说明标签/时间混杂或重尾伪影，不能把 CPU/wall 的 hw 当作纯噪声。
7. 本报告没有 W-03 新格、没有 64K、没有磁盘/NVMe；不能把 tmpfs/4 KiB 的带外推到其他介质或 block size（ledger §9.1.9）。

---

## 6. MME 与预算

### 6.1 方法

对每个候选、格、指标，给定计划效应 δ（%）和配对差 SD（pp）：

- **n80(δ)**：`(t_.975,n−1 + t_.80,n−1) * SD / sqrt(n) <= δ` 的最小 n（双侧 α=0.05，80% 功效）。
- **n_half(δ)**：`t_.975,n−1 * SD / sqrt(n) <= δ/2` 的最小 n（95% CI 半宽 ≤ 效应/2；M-53 的 ledger 判据，比 80% 功效更严）。
- **MDE80(n=16)**：16 对时 80% 功效可分辨的最小 δ。

这些 n 是**每个候选、每个格、每个指标**的下限；实际预算还要加 A/A 的 16 对、失败重跑、多重候选修正；预算应向上取整并留余量。下表所有 n80/n_half 用 t 公式；n=16 的 hw 列用 seed 621 的 bootstrap 半宽（与冻结带一致）；正文中 n=36 的 hw/MDE 也用 t 公式。边缘判据同时看两者。

### 6.2 W1 — ProgressSource poll 等待路径（历史线索 ≤5% cycles，未证实）

W1 的 ledger 状态不变：P1，先 profile 准入，不直接开 A/B；5.0–5.5% cycles 是被取代的 R1 工作稿线索，不是已证实收益（`CAMPAIGN-LEDGER.md:257,270`）。本报告只把它当“计划效应上限”，并强调**必须预注册 5% 的分母**（process cycles / measured-loop cycles / measured-loop CPU）。

| 格 / 指标 | SD pp | n=16 hw % / MDE80 % | n80 / n_half（δ%） |
|---|---:|---|---|
| C5b / CPU | 0.964 | 0.458 / 0.723 | 5: 3/3；2: 5/7；1.2: 8/13；1: 10/17；0.5: 32/60 |
| C3b / cycles | 1.319 | 0.621 / 0.988 | 5: 3/4；2: 6/10；1.2: 12/22；1: 16/30；0.5: 57/110 |
| C7 / cycles | 1.889 | 0.893 / 1.416 | 5: 4/5；2: 10/17；1.2: 22/41；1: 30/58；0.5: 114/222 |
| C5b / cycles | 3.107 | 1.481 / 2.328 | 5: 6/9；2: 21/40；1.2: 55/106；1: 78/151；0.5: 306/596 |
| C2b / CPU（NOISY，仅参考） | 3.414 | 1.604 / 2.558 | 5: 6/10；2: 25/48；1.2: 66/127；1: 94/182；0.5: 368/719 |
| C8 / CPU（NOISY，仅参考） | 4.938 | 2.323 / 3.700 | 5: 10/18；2: 50/97；1.2: 135/263；1: 194/378；0.5: 768/1501 |
| C3b / CPU（BLOCKED） | 5.526 | 2.597 / 4.141 | 5: 12/22；2: 62/120；1.2: 169/329；1: 242/472；0.5: 961/1880 |
| C7 / CPU（BLOCKED） | 9.782 | 4.573 / 7.331 | 5: 33/62；2: 190/370；1.2: 524/1024；1: 753/1473；0.5: 3006/5884 |

判断：

- **如果 5% 是过程级 cycles**：C5b CPU（3/3）、C3b cycles（3/4）、C7 cycles（4/5）都远超 36 对的需求；36 对 A/B 足够。C5b cycles（6/9）也够，但 C5b cycles 本身 SD 3.107，不如 C5b CPU。
- **如果 5% 是 measured-loop 的 5%**：measured CPU 只占 process CPU 的约 24–26%（本报告复算：C2b 24.27%、C3b 24.14%、C5b 25.24%、C7 24.10%、C8 24.64%），过程级效应稀释到约 1.2–1.3%。此时：
  - C5b CPU：δ=1.2% 需 8/13 对；36 对 hw 0.326%、MDE80 0.463% → **36 对足够**。
  - C3b cycles：12/22；36 对 hw 0.446%、MDE80 0.633% → **36 对足够**（80% 与 hw≤0.6% 都够）。
  - C7 cycles：22/41；36 对 hw 0.639%、MDE80 0.907% → 80% 够，但 hw≤0.6% 需 41 对；**36 对差 5 对**。
  - C5b cycles：55/106；C2b CPU：66/127；C8 CPU：135/263 → **36 对不够**。
- **建议**：W1 预注册必须写清 5% 的分母和决定性指标。优先 **C5b CPU（TP d1 w1，QUIET）**：36 对可覆盖 5% 与 1.2% 的 80% 功效和 hw≤δ/2；但 C5b 有背景负载警告，正式 A/B 前复跑 A/A。若必须在 uring 上做 W1，用 **C3b cycles**（36 对足够）或 **C7 cycles**（36 对 80% 够，hw≤0.6% 需 41 对），并先做 cycles-only A/A。**不要**用 C3b/C7 CPU：C3b CPU δ=5% 需 12/22、δ=1.2% 需 169/329；C7 CPU 需 33/62、524/1024，且都被 §1.4-7 阻塞。
- 如果 owner 只接受“5% 历史线索”而不接受分母定义，该候选应保持 INCONCLUSIVE，不得把 16/36 对结果宣传为收益。

### 6.3 W3 — RequestCore 行级候选（行级未知）

W3 的 ledger 状态不变：族级 mutex 约 9–11% 是线索，不是单候选；需要先做 measured 行级归因，再逐候选 A/B（`CAMPAIGN-LEDGER.md:259`）。本报告用 **δ=2% 作为计划值**（不是实测），并给出 δ=1%/0.5% 的敏感性。

| 格 / 指标 | SD pp | n=16 hw % / MDE80 % | n80 / n_half（δ%） |
|---|---:|---|---|
| C5b / CPU | 0.964 | 0.458 / 0.723 | 2: 5/7；1: 10/17；0.5: 32/60 |
| C3b / cycles | 1.319 | 0.621 / 0.988 | 2: 6/10；1: 16/30；0.5: 57/110 |
| C7 / cycles | 1.889 | 0.893 / 1.416 | 2: 10/17；1: 30/58；0.5: 114/222 |
| C5b / cycles | 3.107 | 1.481 / 2.328 | 2: 21/40；1: 78/151；0.5: 306/596 |
| C2b / CPU（NOISY） | 3.414 | 1.604 / 2.558 | 2: 25/48；1: 94/182；0.5: 368/719 |
| C8 / CPU（NOISY） | 4.938 | 2.323 / 3.700 | 2: 50/97；1: 194/378；0.5: 768/1501 |
| C3b / CPU（BLOCKED） | 5.526 | 2.597 / 4.141 | 2: 62/120；1: 242/472；0.5: 961/1880 |
| C7 / CPU（BLOCKED） | 9.782 | 4.573 / 7.331 | 2: 190/370；1: 753/1473；0.5: 3006/5884 |

判断：

- **C5b CPU 是首个可用的 TP d1 w1 基线**：2% 需 5/7 对，1% 需 10/17 对；36 对 hw 0.326%、MDE80 0.463%，2% 和 1% 都足够。0.5% 需 32/60，36 对 80% 够但 hw≤0.25% 需 60 对。
- **C3b cycles**：2% 需 6/10，1% 需 16/30；36 对 hw 0.446%、MDE80 0.633%，2%/1% 都够；0.5% 需 57/110。用前必须预注册 cycles-only A/A。
- **C7 cycles**：2% 需 10/17；36 对 hw 0.639%、MDE80 0.907%，2% 够；1% 需 30/58，36 对 hw 0.639% >0.5%，**不够**；0.5% 需 114/222。
- **C2b/C8 不可用**：C2b 是 TP d16 w4 的修正 filter，但 NOISY；C8 是 TP d64 w4，NOISY。W3 不能声称 TP d16 w4 或 TP d64 已完成；只能用 C5b d1 w1 作 TP 代理，或用 uring d16/d64 的 cycles 作跨后端代理（需明确标注）。
- **C3b/C7 CPU 不可用**：C3b CPU 2% 需 62/120，C7 CPU 需 190/370，且都被 §1.4-7 阻塞；不要用 process CPU 做 W3 的 uring 声明。
- **建议**：先完成 row-level profile；每候选预注册 δ（建议 2%）和决定性指标。优先 C5b CPU（已 QUIET，但背景负载需复测）；若要 uring，先做 cycles-only A/A，C3b cycles 36 对可覆盖 1%，C7 cycles 1% 需 58 对。每候选至少 7–17 对（2%）或 30–58 对（1%），实际预算加 A/A 与重跑；不要用 16 对去宣称 1% 效应。

### 6.4 M-53 — W4 d64 cookie/router O(R)（ledger 要求 hw ≤ 效应/2）

M-53 的 ledger 判据是“d64 深度格 A/A 分辨力达标（半宽 ≤ 效应/2）”，并要求三项迁移义务证明（`CAMPAIGN-LEDGER.md:211`）。本次 **C7 是 d64 首次实测**（W02-uring-4k-d64）；C8 是 TP d64 w4 对照，但 NOISY，不能给 M-53 提供 TP 基线。

| 格 / 指标 | SD pp | n=16 hw % / MDE80 % | n80 / n_half（δ%） |
|---|---:|---|---|
| **C7 / cycles** | 1.889 | 0.893 / 1.416 | 5: 4/5；3: 6/9；2: 10/17；1: 30/58；0.5: 114/222 |
| C7 / CPU | 9.782 | 4.573 / 7.331 | 5: 33/62；3: 86/166；2: 190/370；1: 753/1473；0.5: 3006/5884 |
| C8 / CPU（对照，NOISY） | 4.938 | 2.323 / 3.700 | 5: 10/18；3: 24/45；2: 50/97；1: 194/378；0.5: 768/1501 |
| C8 / cycles（对照，NOISY） | 6.804 | 3.259 / 5.099 | 5: 17/31；3: 43/82；2: 93/181；1: 366/714；0.5: 1456/2848 |

判断：

- **C7 CPU 不可用**：SD 9.782、16 对 hw 4.573%、MDE80 7.331%；δ=2% 需 190 对（80%）/370 对（hw≤1%），δ=1% 需 753/1473。任何实际预算都不够；应优先修环境/改用 cycles，而不是加样本。C7 的 CPU/wall A/A 均值还不为零（+5.04%/+2.54%），更不能当 d64 CPU 基线。
- **C7 cycles 是唯一可能的 d64 带**：16 对 SD 1.889、bootstrap hw 0.893%、t hw 1.007%、MDE80 1.416%。对 δ=2%，bootstrap hw 0.893% ≤1.0%，但保守 t hw 1.007% >1.0%；n_half(2%)=17，所以**16 对差一对**。结论：**M-53 的 d64 分辨力在 16 对上处于边缘，不能按保守判据宣称已达标；17 对（t 判据）或 20 对（留余量）的 cycles-only A/A 是最低要求。**
- 如果 M-53 的计划效应是 1%：C7 cycles 需 30 对（80%）/58 对（hw≤0.5%）；36 对不够 hw≤0.5%。如果效应是 0.5%：114/222，不现实。
- **C8 不能替代**：TP d64 w4 的 CPU SD 4.938、cycles SD 6.804，NOISY；不能拿 C8 的带外推到 C7 或 M-53。
- **建议**：先做 **C7 cycles-only A/A（17–20 对）**，预注册 cycles 为决定性指标，并记录 CPU/wall 只作诊断；冻结 d64 cycles 的 SD/hw 后，再决定 M-53 A/B 的对数：
  - δ=2%：A/A 17–20 对 + A/B 17–20 对（80% 与 hw≤1% 都在 20 对内）；
  - δ=1%：A/B 58 对（hw≤0.5%），80% 需 30 对；
  - δ=3%：A/B 6–9 对；
  - 若 owner 只接受 CPU 指标：M-53 在当前环境 **NOT_POWERED**（2% 需 370 对），不应启动 A/B。

### 6.5 预算汇总与 NOT_POWERED 登记

| 候选 | 16 对是否够 | 关键 δ | 建议格/指标 | 需要的对（80% / hw≤δ/2） | 风险 |
|---|---|---|---|---|---|
| W1（5% cycles，uring d16） | 是 | 5% | C3b cycles | 3 / 4 | C3b CPU/wall BLOCKED；cycles 需 cycles-only A/A |
| W1（5% cycles，uring d64） | 是 | 5% | C7 cycles | 4 / 5 | C7 CPU/wall BLOCKED；cycles 条件带 |
| W1（5% CPU，TP d1 w1） | 是 | 5% | C5b CPU | 3 / 3 | C5b 背景负载警告；条件基线 |
| W1（5% measured-loop → 1.2% process） | 36 对：C5b/C3b 够；C7 差 5 对 | 1.2% | C5b CPU / C3b cycles / C7 cycles | 8/13；12/22；22/41 | C7 cycles hw≤0.6% 需 41 对 |
| W3（2%，TP d1 w1） | 是 | 2% | C5b CPU | 5 / 7 | 条件基线；TP d16/d64 仍无基线 |
| W3（2%，uring d16） | 是 | 2% | C3b cycles | 6 / 10 | cycles-only 预注册 |
| W3（2%，uring d64） | 是 | 2% | C7 cycles | 10 / 17 | 16 对差一对（t 判据） |
| W3（1%，uring d16） | 36 对够 | 1% | C3b cycles | 16 / 30 | cycles-only 预注册 |
| W3（1%，uring d64） | 36 对不够 | 1% | C7 cycles | 30 / 58 | 需 58 对 hw≤0.5% |
| M-53（2%，d64 cycles） | **边缘/差一对** | 2% | C7 cycles | 10 / 17 | 16 对 bootstrap hw 0.893%、t hw 1.007%；建议 17–20 对 |
| M-53（1%，d64 cycles） | 否 | 1% | C7 cycles | 30 / 58 | 36 对不够 hw≤0.5% |
| M-53（2%，d64 CPU） | **否** | 2% | C7 CPU | 190 / 370 | 当前环境 NOT_POWERED；先修环境 |
| M-42（≤0.05% CPU） | 否 | 0.05% | 隔离格 | 远大于当前 W-02 | 需隔离格；本报告不重开 |

附带说明：C5b 的 instr SD 0.0024 pp，1 条 instruction/submit = 0.0051%（180k submits / 3.525B instructions），16 对 MDE80 0.002%；若 M-42 走 instr 指标，C5b 类 TP d1 w1 格比 P1 C6（instr SD 0.014）更敏感。但 A/A 不能替代 M-42 的隔离格与机制门；不要把“instr 能分辨”当成“CPU 收益成立”。

---

## 7. 风险与限制

1. **C7 的 A/A 均值不为零（最重要）**：CPU +5.043%（bootstrap CI [0.46, 9.60]，t CI [−0.17, 10.26]）、wall +2.542%（bootstrap CI [0.28, 4.85]，t CI [−0.05, 5.13]）。AB 与 BA 均值同号（CPU +5.67/+4.42，wall +3.00/+2.09），位置效应 p=0.83/0.75；10/16 为正，符号检验 p≈0.45。bootstrap 与 t 的边界不一致，说明重尾小样本的 CI 不可靠；但“A/A 均值应约 0”这一设计假设在 C7 CPU/wall 上没有成立。CPU 带不可用；wall 带因均值偏离也不能作为无偏基线；cycles 均值 −0.489%（CI 含 0）是唯一居中的 d64 指标。
2. **C3b/C7 的 CPU 变异由 sys time 主导，不是已证实的频率**：sys CV 5.97%/10.50% vs user 3.47%/4.43%；配对 ΔCPU 与 Δsys 相关 0.983/0.992；`cycles:u` 只计用户态。§1.4-7 的“frequency”标签是预注册判据的机械名称，不等于机制已证明。C3b/C7 的直接 `scaling_cur_freq` 序列在启动后稳定在 3.43–3.44 GHz（within-run CV 1.09%/1.08%），没有持续下探；但 `scaling_cur_freq` 不是实际频率计数器，不能证明频率绝对恒定。正确表述是“CPU/wall 不稳定，频率 excursion 未被直接观测复现，sys-time 是主导成分”。
3. **C3b/C7/C8 的 measured rep 双峰**：单 rep CPU 约 0.085 s vs 0.102 s、wall 约 0.048 s vs 0.053 s；过程级指标是 7 个 measured rep 的平均，配对差对“慢 rep 比例”敏感。measured-only 的 SD 不降（C3b CPU 5.90、C7 CPU 10.62、C8 CPU 6.09），不能用 measured-only 洗掉噪声。这是 io_uring/TP 的 workload/内核行为变异，不是简单的启动开销。
4. **C2b 的 NOISY 主要来自 wall SD 与 instr 漂移**：wall SD 3.380 >3.0；instr 漂移 p=0.0017（前 8 对 −0.228%、后 8 对 +0.108%）。CPU 漂移 p=0.54，说明不是 CPU 单独的问题。C2b 的 filter 修正成功，但 TP d16 w4 仍无基线；不要因为“filter 修好了”就把它当可用格。
5. **C5b 的条件性**：C5b 是 P2 唯一 QUIET 格，但 runner 报 `background_load_exceeds_measured_cpus`（loadavg1 最大 2.71，measured CPUs=2）；cycles SD 3.107、MDE80 2.328%，不是窄带。C5b 的 CPU/instr/wall 可作为 W1/W3 的条件基线；正式 A/B 前应复跑 A/A 或把背景负载受控写入预注册。不要把 C5b 的 0.458% CPU hw 直接搬到 d16/d64 或其他后端。
6. **C2b/C8 的 TP 格不可用**：C2b TP d16 w4 NOISY（wall SD 3.380、instr 漂移）；C8 TP d64 w4 NOISY（CPU 4.938、wall 4.891、cycles 6.804）。因此 P2 没有关闭 TP d16 w4 / TP d64 的缺口；W3 的 TP 结论只能用 C5b d1 w1 代理，且必须标注后端/深度/worker 不同。
7. **C3b/C7 的 cycles 带是条件性的**：两格 CPU/wall 都 `ENVIRONMENT_BLOCKED`；P1 C3 cycles 也未冻结。M-53 若用 C7 cycles，必须先做 cycles-only A/A 并预注册；16 对在保守 t 判据下差一对（t hw 1.007% >1.0%），建议 17–20 对。W1/W3 若用 C3b/C7 cycles，同样需要 cycles-only A/A。
8. **背景负载/环境元数据的强度有限**：runner 有每 pair 的 `ps` 快照和 loadavg，但不是连续监测；`ps_top_by_cpu` 只显示快照瞬间的 %CPU；`zcode-cli` 在 16/16 对都出现（≥5%），`python3`（runner）约 39–47%；C8 的 `ps` 自身 100% 是快照进程。IRQ 亲和性只有冻结快照，没有 per-IRQ 记录；`saturating` 未记录。温度 46–61°C，未到 80°C，无热节流迹象；governor performance、no_turbo=0、binary SHA 一致、时间戳串行无重叠。这些不足以证明“环境完全受控”。
9. **统计限制**：每格 16 对，满足 ledger §9.1.8 的 ≥15 最低要求，但仍是小样本；bootstrap/permutation 是描述性；没有多重比较修正（5 格 × 4 指标 × 3 诊断）。C2b instr p=0.0017、C8 漂移 p≈0.055、C7 的 bootstrap/t 边界都应保守解读。A/A 不是 A/B：同 binary、没有候选 SHA、没有 mutant、没有语义 oracle 重新执行，不能推出任何收益。
10. **介质与 workload 外推**：全部 tmpfs、page-cache resident、4 KiB；没有磁盘/NVMe、64K、W-03 新格；不能把本报告的带外推到其他介质/深度/后端（ledger §9.1.9）。C1 的 btrfs→tmpfs copy 与 bench 格不同，不能混用。
11. **脚本工具的两个缺口**：`verdict_for` 没有直接消费 `perfAudit.scaledRows`；`validity_of_arm` 仍读 runner 的 `oracle.bench.*_records` 字段。本报告用独立 raw 检查补上了这两点（480/480 perf 行 100%、160/160 arm raw 回执一致），所以本次判词不受影响；但后续若复用该脚本，应把这两个检查并入 gate，避免“runner 字段说通过、raw 实际缩放/缺记录”的假通过。
12. **P1 C3 的频率序列缺口**：P1 只有 min/max/last，没有 series；本报告不能证明 P1 C3 也没有持续频率 excursion，只能证明 P1 的 last 全部 ≥3.25 GHz、min 低值无法区分启动/空闲。P2 C3b 的 series 是第一次直接观测；它显示没有持续下探。若 owner 要严格比较两轮频率，需要用同一 20 ms series 重跑 P1 C3。

---

## 8. 建议的下一步

1. **先修 TP 格**：C2b TP d16 w4 虽 filter 命中但 NOISY（wall SD 3.380、instr 漂移 0.0017）；C8 TP d64 w4 NOISY。W3 的 TP d16/d64 结论保持 INCONCLUSIVE。C5b TP d1 w1 可作条件基线，但建议在同环境复跑 16 对 A/A，确认背景负载警告下的 SD/hw 可复现。
2. **W1**：预注册“5% 的分母 + 决定性指标”。优先 C5b CPU（TP d1 w1，QUIET）：36 对覆盖 5% 与 1.2%；若必须在 uring 上做，用 C3b cycles（36 对够 1.2%）或 C7 cycles（36 对 80% 够，hw≤0.6% 需 41 对），并先做 cycles-only A/A。不要用 C3b/C7 CPU。
3. **W3**：先做 row-level profile；每候选预注册 δ（建议 2%）。C5b CPU 2%/1% 需 7/17 对；C3b cycles 需 10/30 对；C7 cycles 需 17/58 对。36 对预算对 2% 足够，对 1% 只有 C5b CPU 与 C3b cycles 满足 hw≤0.5%；C7 cycles 1% 需 58 对。不要用 16 对宣称 1%。
4. **M-53**：先做 **C7 cycles-only A/A（17–20 对）**，冻结 d64 cycles 的 SD/hw；不要用 CPU。若 cycles SD 复现 1.889，则 2% 的 A/B 用 20 对左右可满足 hw≤1%；1% 需 58 对；CPU 在当前环境 NOT_POWERED（2% 需 370 对），先修环境。
5. **C3b/C7 的机制复核**：如果 owner 需要“频率”结论，应用实际频率计数器（如 `cpuinfo_cur_freq`、APERF/MPERF 或 perf `cycles` 非 `:u`）复测，而不是只用 `scaling_cur_freq`；同时分解 sys/user/cycles，确认是 io_uring 内核时间、IRQ/io-wq 调度还是背景负载。不要直接把 §1.4-7 的机械触发写成“已证实频率下降”。
6. **C5b 的背景负载**：在正式 W1/W3 A/B 前复跑一次 C5b A/A，记录 `background_load_per_pair` 与 `ps` 快照；如果 loadavg 仍 >2 且 CPU SD 仍 <1 pp，可把 C5b 升级为正式基线；否则把“背景负载受控”写入协议或换格。
7. **工具修正**：把 `perfAudit.scaledRows` 和 raw JSONL 的 warmup/measured/target 检查并入 `verdict_for`/`validity_of_arm`；在 `meta` 中记录 `freq` series 的采样接口（`scaling_cur_freq` vs `cpuinfo_cur_freq`）和实际频率计数器，避免下一轮再把“请求频率”当“实际频率”。

---

## 附录 A：精确命令与复现

```sh
cd docs/evidence/621-progress-wait-path-20261118

# P2 五格
python3 -I tools/recompute_independent.py --aa-root p1/aa2 --json-out /tmp/aa2_recompute.json

# P1 C3 对照（同一脚本，从原始记录重算）
python3 -I tools/recompute_independent.py --aa-root p1/aa --json-out /tmp/aa1_recompute.json

# 第二份独立复核（一次性脚本，位于 /tmp，不入库）
python3 -I /tmp/aa2_audit/audit_aa2.py p1/aa2 --json-out /tmp/aa2_audit/audit_aa2.json
```

- 脚本版本：`recompute_independent.py` `TOOL_VERSION="1.0-independent"`。
- 种子：621；bootstrap 10,000；置换 10,000；alpha 0.05；power 0.80。
- 复核脚本不 import 证据工具；独立实现配对差、SD、精确 AB/BA 置换（12,870 组合）、精确符号翻转（65,536）、200k 漂移置换；raw JSONL/perf 独立解析。
- 所有聚合值来自上述命令的 JSON 输出，没有手抄 runner summary。

## 附录 B：逐 pair 配对差（%）

#### C2b-w02-tp-4k-d16-w4

| pair | order | CPU % | instr % | wall % | cycles % |
|---:|---|---:|---:|---:|---:|
| 1 | AB | +1.318 | -0.5537 | +1.302 | -1.991 |
| 2 | BA | -0.610 | -0.7528 | -0.585 | -8.662 |
| 3 | AB | +5.025 | -0.1145 | +5.207 | +7.096 |
| 4 | BA | +6.247 | -0.4027 | +6.143 | +5.924 |
| 5 | AB | +5.361 | -0.4630 | +5.307 | +2.351 |
| 6 | BA | -4.789 | +0.3098 | -4.785 | -5.403 |
| 7 | AB | +0.750 | +0.1982 | +0.831 | +1.665 |
| 8 | BA | -0.060 | -0.0467 | -0.025 | -0.725 |
| 9 | AB | +0.991 | +0.1935 | +0.738 | +1.993 |
| 10 | BA | +1.405 | -0.1844 | +1.303 | +0.150 |
| 11 | AB | -1.930 | -0.0131 | -1.847 | -0.428 |
| 12 | BA | +4.782 | +0.1042 | +4.571 | +9.009 |
| 13 | AB | +4.446 | -0.0891 | +4.379 | +7.955 |
| 14 | BA | -0.255 | +0.2864 | -0.381 | -0.032 |
| 15 | AB | +4.411 | +0.1809 | +4.287 | +7.904 |
| 16 | BA | -4.416 | +0.3851 | -4.356 | -3.986 |

#### C3b-w02-uring-4k-d16

| pair | order | CPU % | instr % | wall % | cycles % |
|---:|---|---:|---:|---:|---:|
| 1 | AB | +0.265 | -0.0099 | +0.454 | -0.078 |
| 2 | BA | +3.905 | -0.0851 | +1.787 | -0.404 |
| 3 | AB | -8.091 | +0.1730 | -4.546 | -0.491 |
| 4 | BA | +3.201 | -0.0773 | +1.853 | +0.723 |
| 5 | AB | +1.795 | -0.0068 | +2.025 | +1.045 |
| 6 | BA | +5.324 | -0.1464 | +2.087 | -2.198 |
| 7 | AB | +0.402 | -0.0009 | +0.666 | +0.285 |
| 8 | BA | +3.323 | -0.0576 | +2.037 | -0.371 |
| 9 | AB | +2.795 | -0.0212 | +1.860 | -1.580 |
| 10 | BA | -9.995 | +0.1801 | -5.863 | +3.100 |
| 11 | AB | +5.256 | -0.1040 | +2.816 | -1.073 |
| 12 | BA | +0.615 | -0.0077 | +0.452 | -0.035 |
| 13 | AB | -0.667 | +0.0341 | +0.039 | -0.035 |
| 14 | BA | -5.367 | +0.1331 | -2.361 | +1.128 |
| 15 | AB | -12.385 | +0.2782 | -7.098 | +1.855 |
| 16 | BA | +2.945 | -0.0606 | +1.249 | -1.041 |

#### C5b-w02-tp-4k-d1-w1

| pair | order | CPU % | instr % | wall % | cycles % |
|---:|---|---:|---:|---:|---:|
| 1 | AB | -0.050 | -0.0011 | +0.018 | -0.127 |
| 2 | BA | -0.102 | +0.0033 | -0.257 | -0.414 |
| 3 | AB | -0.966 | -0.0039 | -1.346 | -4.489 |
| 4 | BA | +1.546 | -0.0003 | +2.183 | -1.151 |
| 5 | AB | +1.805 | +0.0014 | +1.429 | +3.202 |
| 6 | BA | +1.721 | +0.0000 | +1.867 | +3.348 |
| 7 | AB | +0.522 | -0.0007 | +0.165 | +2.530 |
| 8 | BA | +0.614 | -0.0001 | +0.705 | +0.802 |
| 9 | AB | +1.148 | +0.0015 | +1.279 | +1.902 |
| 10 | BA | -0.590 | -0.0030 | -0.376 | +1.708 |
| 11 | AB | +0.884 | +0.0000 | +0.770 | +5.512 |
| 12 | BA | +0.374 | +0.0003 | +0.545 | +7.053 |
| 13 | AB | -0.741 | -0.0052 | -1.269 | -1.418 |
| 14 | BA | +0.597 | +0.0015 | +1.188 | +3.988 |
| 15 | AB | +0.102 | -0.0047 | -0.371 | +1.332 |
| 16 | BA | -1.442 | -0.0024 | -1.938 | -3.677 |

#### C7-w02-uring-4k-d64

| pair | order | CPU % | instr % | wall % | cycles % |
|---:|---|---:|---:|---:|---:|
| 1 | AB | +5.093 | -0.0171 | +3.150 | -0.209 |
| 2 | BA | -4.114 | +0.0100 | -1.556 | +2.432 |
| 3 | AB | +8.441 | -0.0398 | +4.764 | -0.696 |
| 4 | BA | -1.372 | +0.0034 | -1.102 | +0.654 |
| 5 | AB | -1.464 | -0.0038 | -1.061 | -0.805 |
| 6 | BA | +10.099 | -0.0129 | +5.725 | +0.162 |
| 7 | AB | +16.438 | -0.0317 | +8.008 | -2.636 |
| 8 | BA | +21.114 | -0.0345 | +10.321 | -2.779 |
| 9 | AB | +3.698 | +0.0026 | +1.512 | -1.136 |
| 10 | BA | -10.852 | +0.0199 | -5.478 | +2.593 |
| 11 | AB | +19.930 | -0.0281 | +9.823 | -3.807 |
| 12 | BA | +13.610 | -0.0118 | +6.907 | -2.349 |
| 13 | AB | -9.085 | +0.0090 | -4.231 | +1.967 |
| 14 | BA | +9.788 | -0.0280 | +3.850 | -1.727 |
| 15 | AB | +2.295 | -0.0072 | +2.002 | +0.520 |
| 16 | BA | -2.924 | +0.0240 | -1.964 | -0.011 |

#### C8-w02-tp-4k-d64-w4

| pair | order | CPU % | instr % | wall % | cycles % |
|---:|---|---:|---:|---:|---:|
| 1 | AB | -1.531 | +0.4252 | -1.635 | -0.338 |
| 2 | BA | +2.395 | -0.0729 | +2.341 | +3.478 |
| 3 | AB | +7.065 | +0.2107 | +7.042 | +10.795 |
| 4 | BA | -4.697 | -0.3490 | -4.613 | -8.118 |
| 5 | AB | +4.495 | +0.0048 | +4.575 | +4.505 |
| 6 | BA | +3.559 | +0.3797 | +3.410 | +6.957 |
| 7 | AB | +6.213 | +0.3156 | +6.020 | +7.193 |
| 8 | BA | +8.121 | +0.1426 | +8.083 | +11.603 |
| 9 | AB | -0.610 | +0.1958 | -0.547 | +2.443 |
| 10 | BA | -6.604 | +0.0301 | -6.670 | -7.494 |
| 11 | AB | -2.998 | -0.3424 | -2.833 | -8.043 |
| 12 | BA | +1.778 | -0.0596 | +1.663 | +2.062 |
| 13 | AB | -3.120 | -0.2257 | -2.861 | -7.213 |
| 14 | BA | +0.321 | -0.1202 | +0.363 | +0.189 |
| 15 | AB | -6.731 | +0.1316 | -6.837 | -2.892 |
| 16 | BA | -6.089 | -0.1908 | -5.934 | -7.531 |

## 附录 C：原始路径与文件清单

- C2b：`p1/aa2/C2b-w02-tp-4k-d16-w4/`（`arms.jsonl`、`pairs.tsv`、`meta.json`、`raw/` 160 文件）
- C3b：`p1/aa2/C3b-w02-uring-4k-d16/`
- C5b：`p1/aa2/C5b-w02-tp-4k-d1-w1/`
- C7：`p1/aa2/C7-w02-uring-4k-d64/`
- C8：`p1/aa2/C8-w02-tp-4k-d64-w4/`
- P1 C3 对照：`p1/aa/C3-w02-uring-4k-d16/`
- 协议：`p1/AA-CALIBRATION.md` §1.2–1.4、§2.2、§5
- 台账：`CAMPAIGN-LEDGER.md` §9.1（独立复算、≥15 对、环境门）、§7.7（`ENVIRONMENT_BLOCKED` 风险）、M-53 行 211、W1/W3/W4 行 257/259/260
- 脚本：`tools/recompute_independent.py`；一次性复核 `/tmp/aa2_audit/audit_aa2.py`（不入库）
- 二进制：`/tmp/621-p1-build/ring/linux/x86_64/release/bench_w02_pipeline`，sha256 `03dbef098d41e6d6a5280a46d46758ca7267326925ae95d1156ad8d8c3c6a896`（与 `meta.json` 的 A/B 一致）
- BASE：`5d94f233967c3aef993eb19ba9e3252c532b0951`；`git.dirty=true`（未跟踪证据目录）
