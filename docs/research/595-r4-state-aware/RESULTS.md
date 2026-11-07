# #595 R4 结果草稿（RESULTS，由 `scripts/render_results.py` 从 `analysis/MAIN_SUMMARY.json` 生成）

- 生成 UTC：2026-11-06T10:36:43Z；schema `r4-main-analysis/1`
- 预注册：`PREREGISTERED.md` sha256 `fbb2e7852e87f9fbe4bf0e1e12e110d9d2ad279f9d486141b16d61b79f01f0f6`（门槛/家族/seed 唯一来源）
- 分析脚本：`scripts/matrix_analysis.py` sha256 `74e92ec25496741dccd024fee74de52b63a8c63521167944f56ef519c32230b2`
- 统计：block tuple 内配对差值；bootstrap 10000 次、seed 5950、percentile；单侧 95% 上界 = bootstrap 均值 p95；非劣 p = (#{均值≥界}+1)/(B+1)；Holm 家族按 §5.4。
- 本轮所有数字均来自 `raw/` 原始记录；未使用任何历史轮次数字；NOT_RUN 一律留空并注明，不填零。
- 终审订正：本文件在脚本生成后由终审裁决员并入 §16 八问裁决与 §17 独立复核/订正；
  `figure-data.csv` 的 F5 state 行已按原始 diag 修正并重建（图本身未变），§15 的
  sha256 为订正后实测值。数字裁决以本轮 `raw/` 与 `analysis/MAIN_SUMMARY.json` 为准。

## 0. 结论摘要

- 判定树总判决：**INCONCLUSIVE/PARTIAL: F-MAIN statistical bounds FAIL for both arms (p99 upper +23.58% R, +36.25% S vs +5% limit); F-PROT has 39 bound FAIL members (4 NOT_ESTABLISHED); protocol preconditions and three resource sub-gates NOT_RUN; F-VALUE NOT_ESTABLISHED; TRAJECTORY INCOMPLETE (0/24)**。
- 产品层：**PRODUCT=HOLD (research-only, prereg 13.8)**。
- 主门（W03-d16，§7.1）：R FAIL、S FAIL —— 两臂 CPU/吞吐条件成立，尾延迟条件不成立。
- 保护族 F-PROT：41/84 成员通过；界越界 FAIL 39、NOT_ESTABLISHED 4；FAIL 格：CORE-W02-D1, CORE-W02-D16, CORE-W02-D2, CORE-W03-D2SERIAL, CORE-W03-D4, CORE-W03-D64, PROT-NONTMPFS。
- 感知额外价值 F-VALUE：**NOT_ESTABLISHED**（W03-d16 两条路线均不过；TRAJECTORY-T2 形状 INCOMPLETE → NOT_ESTABLISHED）。
- 资源门：RSS 子门 PASS、线程子门 PASS，但整体 **NOT_ESTABLISHED**（三个子门未运行，§8.1）。
- 协议前置（§10.5 正控、W03-d16 enter/CQE 对账）**NOT_RUN** → 依 §13.2主门根本不可被宣告 PASS，且 §13.7 排除 GO_MECHANISM。

## 1. 数据与有效性

| cell | case | arms | planned blocks | valid blocks | invocations | valid invocations |
|---|---|---|---|---|---|---|
| B-SUPP-W03-D16 | W03-uring-4k-d16 | 2 | 24 | 24 | 48 | 48 |
| CORE-W02-D1 | W02-uring-4k-d1 | 4 | 24 | 24 | 96 | 96 |
| CORE-W02-D16 | W02-uring-4k-d16 | 4 | 24 | 24 | 96 | 96 |
| CORE-W02-D2 | W02-uring-4k-d2 | 4 | 24 | 24 | 96 | 96 |
| CORE-W03-D2SERIAL | W03-uring-4k-d2-r3d2serial | 4 | 24 | 24 | 96 | 96 |
| CORE-W03-D4 | W03-uring-4k-d4 | 4 | 24 | 24 | 96 | 96 |
| CORE-W03-D64 | W03-uring-4k-d64 | 4 | 24 | 24 | 96 | 96 |
| MAIN-W03-D16 | W03-uring-4k-d16 | 8 | 24 | 24 | 192 | 192 |
| PROT-64K-D16 | W03-uring-64k-d16-r3d16-64k | 4 | 24 | 24 | 96 | 96 |
| PROT-NONTMPFS | W03-uring-4k-d16 | 4 | 24 | 24 | 96 | 96 |
| PROT-SMT-2-12 | W03-uring-4k-d16 | 4 | 8 | 8 | 32 | 32 |
| PROT-XPHYS-2-8 | W03-uring-4k-d16 | 4 | 8 | 8 | 32 | 32 |
| TRAJECTORY | T1/T2/T3 | 8×3 | 24 | 0 | 552 | 552 |

- MAIN/CORE 全部 24/24 有效块（每臂每块一次 invocation，WARMUP=1/MEASURED=5）；TRAJECTORY 24 块每块仅 23/24 槽位有效（见 §11），完整块数 0/24 → INCOMPLETE。
- 逐 invocation 无效判据（§10.2）在本轮 clean 块中零触发（`attempts.jsonl` 仅在 TRAJECTORY 的 SHADOW-T1 槽位有 72 条失败重试记录）。

## 2. A/A 校准（配对差值，§6）

| A/A 块 | pairs | CPU 半宽 (≤5pp) | 吞吐半宽 (≤3pp) | p99 半宽 (≤3pp) | 门 | 与 summary.json 复核 |
|---|---|---|---|---|---|---|
| AA-W02-D2 | 24 | 0.5518 pp | 0.5584 pp | 1.3262 pp | PASS | OK |
| AA-W03-D16 | 24 | 1.0245 pp | 1.0286 pp | 2.0971 pp | PASS | OK |
| AA-W03-D2SERIAL | 24 | 0.5059 pp | 0.5131 pp | 2.7086 pp | PASS | OK |

- 三块全部 PASS（配对 m1/m2 差值；W02-D2 的共模趋势已由配对方式吸收），满足 §6/§13.1，后续对比可看。
- 复核：本分析脚本用同一冻结 bootstrap 约定重算，与各 A/A `summary.json` 的半宽逐位一致（差异 <1e-9）。

## 3. 主门 F-MAIN（W03-uring-4k-d16，§7.1 合取）

| 条件 | 实测（单侧 95%） | 门槛 | 判定 |
|---|---|---|---|
| R−A0 CPU 点估计 | -19.62% | ≤ −10% | PASS |
| R−A0 CPU 单侧上界 | -19.11% | ≤ −5% | PASS |
| R−A0 吞吐下降上界 | -23.74% | ≤ +3% | PASS |
| R−A0 p95 上界 | +4.33% | ≤ +5% | PASS |
| R−A0 p99 上界 | +23.58% | ≤ +5% | FAIL |
| S−A0 CPU 点估计 | -12.81% | ≤ −10% | PASS |
| S−A0 CPU 单侧上界 | -12.39% | ≤ −5% | PASS |
| S−A0 吞吐下降上界 | -14.19% | ≤ +3% | PASS |
| S−A0 p95 上界 | +34.66% | ≤ +5% | FAIL |
| S−A0 p99 上界 | +36.25% | ≤ +5% | FAIL |

| 臂 | CPU 半宽 | 吞吐半宽 | p95 半宽 | p99 半宽 | 功效 | CPU 双口径符号 | RSS 点/上界 (≤+2%) | RSS 门 | 线程门 |
|---|---|---|---|---|---|---|---|---|---|
| R | +0.60% | +0.95% | +1.91% | +2.10% | PASS | 同号（primary -1.00 / secondary -1.00） | +0.00% / +0.00% | PASS | PASS |
| S | +0.51% | +0.67% | +1.18% | +1.95% | PASS | 同号（primary -1.00 / secondary -1.00） | +0.00% / +0.00% | PASS | PASS |

- **判决：R FAIL（p99 上界 +23.58% > +5%）；S FAIL（p95 +34.66%、p99 +36.25% > +5%）**。两臂 CPU/吞吐/功效/双口径/RSS/线程条件均通过。
- 逐 block 差值方向一致性：R 的 p99 差值 24/24 为正（+6.4%…+32.6%），S 的 p95/p99 差值 24/24 为正（+27.3%…+38.0% / +25.3%…+46.9%）——尾延迟回归不是个别块造成的。
- 协议前置（§10.5）：`r3d2pos` 正控与 W03-d16 每臂 strace enter/CQE 对账**均未运行**；按 §13.2 主门不得进入 PASS 分支，按 §13.7 不得 GO_MECHANISM。

## 4. 保护族 F-PROT（Holm，单侧 α=0.05，§5.4/§7.2）

家族规模 m = 84（6 个核心保护格 × {R,S,Q8} × 4 metric + PROT-NONTMPFS × {R,S,Q8} × 4 metric）。Holm 最小阈值 α/m = 0.000595；通过 41、**界越界 FAIL 39**、界内但 Holm 未拒绝 NOT_ESTABLISHED 4（依 §5.3/§5.4/§7.2：FAIL 只由点估计/上界越界定义，Holm 不显著故不能记 PASS，但也不等同 FAIL）。

| 格 | PASS | FAIL（界越界） | NOT_ESTABLISHED | 成员数 |
|---|---|---|---|---|
| CORE-W02-D1 | 4 | 6 | 2 | 12 |
| CORE-W02-D16 | 3 | 8 | 1 | 12 |
| CORE-W02-D2 | 5 | 6 | 1 | 12 |
| CORE-W03-D2SERIAL | 0 | 12 | 0 | 12 |
| CORE-W03-D4 | 11 | 1 | 0 | 12 |
| CORE-W03-D64 | 8 | 4 | 0 | 12 |
| PROT-NONTMPFS | 10 | 2 | 0 | 12 |

FAIL 成员（点估计或单侧上界越界；p 均 ≥ 0.243，因此没有任何 FAIL 成员依赖 Holm 拒绝——失败由界本身判定）：

| 格 | 臂 | metric | 点估计 | 单侧上界 | 界 |
|---|---|---|---|---|---|
| CORE-W02-D1 | Q8 | p99 | +4.42% | +6.13% | ≤ +5% |
| CORE-W02-D1 | Q8 | tput | +3.71% | +4.35% | ≤ +3% |
| CORE-W02-D1 | R | p99 | +5.25% | +7.83% | ≤ +5% |
| CORE-W02-D1 | R | tput | +2.75% | +3.35% | ≤ +3% |
| CORE-W02-D1 | S | p99 | +5.41% | +7.53% | ≤ +5% |
| CORE-W02-D1 | S | tput | +4.02% | +4.52% | ≤ +3% |
| CORE-W02-D16 | Q8 | cpu | +5.24% | +5.96% | ≤ +5% |
| CORE-W02-D16 | Q8 | p95 | +5.37% | +6.68% | ≤ +5% |
| CORE-W02-D16 | Q8 | p99 | +5.79% | +7.61% | ≤ +5% |
| CORE-W02-D16 | Q8 | tput | +4.96% | +5.61% | ≤ +3% |
| CORE-W02-D16 | S | cpu | +5.95% | +6.52% | ≤ +5% |
| CORE-W02-D16 | S | p95 | +6.76% | +7.93% | ≤ +5% |
| CORE-W02-D16 | S | p99 | +6.61% | +8.19% | ≤ +5% |
| CORE-W02-D16 | S | tput | +5.61% | +6.11% | ≤ +3% |
| CORE-W02-D2 | Q8 | p99 | +5.04% | +6.98% | ≤ +5% |
| CORE-W02-D2 | Q8 | tput | +3.62% | +4.02% | ≤ +3% |
| CORE-W02-D2 | R | tput | +3.21% | +3.80% | ≤ +3% |
| CORE-W02-D2 | S | cpu | +4.98% | +5.50% | ≤ +5% |
| CORE-W02-D2 | S | p99 | +5.80% | +7.56% | ≤ +5% |
| CORE-W02-D2 | S | tput | +4.72% | +5.20% | ≤ +3% |
| CORE-W03-D2SERIAL | Q8 | cpu | +9.29% | +9.74% | ≤ +5% |
| CORE-W03-D2SERIAL | Q8 | p95 | +8.88% | +10.13% | ≤ +5% |
| CORE-W03-D2SERIAL | Q8 | p99 | +9.68% | +12.07% | ≤ +5% |
| CORE-W03-D2SERIAL | Q8 | tput | +8.50% | +8.88% | ≤ +3% |
| CORE-W03-D2SERIAL | R | cpu | +6.94% | +7.65% | ≤ +5% |
| CORE-W03-D2SERIAL | R | p95 | +7.61% | +8.73% | ≤ +5% |
| CORE-W03-D2SERIAL | R | p99 | +7.17% | +10.16% | ≤ +5% |
| CORE-W03-D2SERIAL | R | tput | +6.47% | +7.08% | ≤ +3% |
| CORE-W03-D2SERIAL | S | cpu | +11.57% | +12.09% | ≤ +5% |
| CORE-W03-D2SERIAL | S | p95 | +10.37% | +12.41% | ≤ +5% |
| CORE-W03-D2SERIAL | S | p99 | +11.61% | +14.09% | ≤ +5% |
| CORE-W03-D2SERIAL | S | tput | +10.36% | +10.78% | ≤ +3% |
| CORE-W03-D4 | S | p99 | +18.39% | +19.44% | ≤ +5% |
| CORE-W03-D64 | Q8 | p99 | +20.52% | +24.92% | ≤ +5% |
| CORE-W03-D64 | R | p99 | +21.17% | +25.65% | ≤ +5% |
| CORE-W03-D64 | S | p95 | +13.78% | +14.70% | ≤ +5% |
| CORE-W03-D64 | S | p99 | +25.06% | +32.73% | ≤ +5% |
| PROT-NONTMPFS | S | p95 | +23.13% | +24.23% | ≤ +5% |
| PROT-NONTMPFS | S | p99 | +19.45% | +21.45% | ≤ +5% |

NOT_ESTABLISHED 成员（界内但 Holm 未拒绝；不阻塞 GO，但阻塞「该格无退化」的表述）：

| 格 | 臂 | metric | 点估计 | 单侧上界 | p | Holm α |
|---|---|---|---|---|---|---|
| CORE-W02-D1 | Q8 | cpu | +3.88% | +4.58% | 0.0051 | 0.00119 |
| CORE-W02-D1 | S | cpu | +4.20% | +4.75% | 0.0089 | 0.00122 |
| CORE-W02-D16 | R | tput | +2.15% | +2.71% | 0.0044 | 0.00116 |
| CORE-W02-D2 | R | p99 | +2.80% | +4.39% | 0.0131 | 0.00125 |

- 要点：W03-d2serial 上 R/S/Q8 的 CPU/吞吐/p95/p99 **全部**越界（+6.5%…+14.1%）——批量 adapter 在每跨度 1 条的稀疏形状只增加开销、没有合发机会；W03-d64 的主要问题是 p99（+21%…+25%，S 还有 p95 +14.7%）；W02 系是小幅 CPU/吞吐/p99 超出（S 在 d16 上 CPU +5.95%、d2 上上界 +5.50%；Q8 在 d16 上 +5.24%）；PROT-NONTMPFS 的 S p95/p99 越界（+23.1%/+19.4%）。
- 依 §13.6：保护族存在界越界 FAIL 成员 → **不得 GO（整体）**，按块记 FAIL；这些格不能再表述为「无退化」。

## 5. 感知额外价值 F-VALUE（§7.3）

| 路线 | 形状 | 条件实测 | p | Holm α | 状态 | 判定 |
|---|---|---|---|---|---|---|
| A | W03-D16 | cpu 上界 +9.31%（要求 ≤ -2%）; p95 上界 +31.78%（要求 ≤ +5%）; p99 上界 +11.63%（要求 ≤ +5%）; tput 上界 +8.50%（要求 ≤ +3%） | 1.0000 | 0.0125 | - | FAIL |
| A | TRAJECTORY-T2 | cpu 上界 +25.87%（要求 ≤ -2%）; p95 上界 +70.39%（要求 ≤ +5%）; p99 上界 +70.63%（要求 ≤ +5%）; tput 上界 +29.94%（要求 ≤ +3%） | 1.0000 | 0.0167 | NOT_ESTABLISHED_INCOMPLETE_SHAPE | FAIL |
| B | W03-D16 | cpu 上界 +9.31%（要求 ≤ +2%）; p99 上界 +11.63%（要求 ≤ -5%）; tput 上界 +8.50%（要求 ≤ +3%） | 1.0000 | 0.0250 | - | FAIL |
| B | TRAJECTORY-T2 | cpu 上界 +25.87%（要求 ≤ +2%）; p99 上界 +70.63%（要求 ≤ -5%）; tput 上界 +29.94%（要求 ≤ +3%） | 1.0000 | 0.0500 | NOT_ESTABLISHED_INCOMPLETE_SHAPE | FAIL |

- 路线 A/B 在 W03-d16 上均不成立：S−R 的 CPU 上界 +9.31%（要求 ≤−2%）、p99 上界 +11.63%（要求 ≤−5%）——S 相对 R 在 CPU 与尾延迟上都更差。
- TRAJECTORY-T2 两个成员因形状 INCOMPLETE（0/24）记 NOT_ESTABLISHED（p 置 1.0，不能通过 Holm）；其描述性数字见 §11。
- 结论：**未建立感知额外价值**（不得用描述性数字变通）。

## 6. 资源门（§8）

- **RSS 子门：PASS**。所有 (格,臂) 的 `(RSS_X−RSS_A0)/RSS_A0` 点估计与单侧上界均为 0.00%（同块同二进制）。主块 192 个 invocation 的 wait4 `ru_maxrss` 与 rep 内采样 `peak_rss_kb` 完全一致（差 0 KB）；`/usr/bin/time -v` 未被 runner 使用，交叉核对采用上述两个已记录字段。
- 绝对值（MAIN-W03-D16，24 块均值）：A0/R/S/Q8/SNI/SNP/Q2/SHADOW ru_maxrss 均 25336 KB（逐块相同）；B-SUPP 会话为 21433 KB（不同会话，不与主块比较）。
- **线程子门：PASS**（候选臂 R/S/Q8 全部 24/24 块 `max_user_threads` ≤ A0；各臂最大值见下表）。非候选臂 SNP 在 block 14 出现 1/24 次 +1（4 vs 3，采样 20 vs 24 个 100ms 样本，双方均只有 1 个 iou-wrk 线程）——如实记录，不计入候选臂线程子门。
- **整体资源门：NOT_ESTABLISHED** —— §8.1 三个子门未运行：静态审计 `logs/audit.txt` 不存在；W03-d16 clean 臂 strace （clone/clone3/timer_create/epoll_create*/eventfd 计数）未跑（现有 strace 仅 DIAG-64K-STRACE 与 PROT-THREADPOOL）；valgrind/massif 分配斜率未跑。依 §8.1「NOT_RUN → NOT_ESTABLISHED」，并因此整体不得 GO。

| 臂 (MAIN) | 块数 | 臂 max_user_threads | A0 max | 最差 Δ | 越界块数 |
|---|---|---|---|---|---|
| Q2 | 24 | 3 | 3 | 0 | 0 |
| Q8 | 24 | 3 | 3 | 0 | 0 |
| R | 24 | 3 | 3 | 0 | 0 |
| S | 24 | 3 | 3 | 0 | 0 |
| SHADOW | 24 | 3 | 3 | 0 | 0 |
| SNI | 24 | 3 | 3 | 0 | 0 |
| SNP | 24 | 4 | 3 | 1 | 1 |

## 7. 消融与分解（描述性，§4；不进任何门槛）

| 对比 | CPU 点/上界 | 吞吐上界 | p95 点/上界 | p99 点/上界 |
|---|---|---|---|---|
| SHADOW−R（纯读取成本） | +1.02% / +1.94% | +1.84% | +1.95% / +3.83% | +1.22% / +2.18% |
| S−Q2 | +0.83% / +1.29% | +1.25% | +0.43% / +1.96% | -0.44% / +2.35% |
| S−SNI | +1.21% / +1.72% | +1.66% | +0.69% / +1.84% | +0.69% / +2.51% |
| S−SNP | +3.89% / +4.52% | +4.31% | +19.39% / +21.51% | +1.17% / +4.36% |
| S−Q8 | +7.98% / +8.70% | +8.00% | +28.83% / +30.93% | +9.22% / +10.54% |
| S−R | +8.51% / +9.31% | +8.50% | +30.34% / +31.78% | +10.52% / +11.63% |
| R−B（补充块，正=B 比 R 差） | +9.04% / +10.25% | +9.23% | +32.17% / +34.81% | +14.50% / +16.68% |

- 读法：SHADOW−R 只差「读并记录两种 hint」（动作与 R 相同）→ 纯读取成本 +1.02%（上界 +1.94%）CPU；S−R 的 +8.51% 说明改变动作的成本远大于读取成本。
- S≈Q2（+0.83%）而 S 远差于 Q8（+7.98%）：S 的 quantum 在 d16 上 99.97% 为 2，表选择实际退化为固定 2。
- **TABLE_OVERHEAD_FLAG**：S−Q8 CPU 单侧上界 +8.70% > +3% → 置位（S−Q2 上界 +1.29% 未置位）。仅作限制披露，不翻转主门。
- **不同基准的百分比不可相加**（R、Q2、SNI、SNP、Q8、B 分别作分母）；不得用百分位差反推阶段分解；R−B 来自独立描述性补充块，不与主块混合。

## 8. 感知暴露（§4.3，诊断数据；非计时证据）

| 形状 | S 决策数 | S 分支（row） | S quantum 分布 | mean ready | mean present | present/ready | 改变动作（SHADOW 反事实 vs R 实际） |
|---|---|---|---|---|---|---|---|
| W03-D16 | 29998 | completion_pressure:29973, inflight_zero:16, quantum_base:3, remaining_single:6 | q1 6 / q2 29989 / q8 3 | 7.92 | 2.00 | 25.2% | 7500/7504 (99.95%) |
| T1 | 60000 | remaining_single:60000 | q1 60000 / q2 0 / q8 0 | 1.00 | 1.00 | 100.0% | 0/60000 (0.00%) |
| T2 | 94 | completion_pressure:72, inflight_zero:21, quantum_base:1 | q1 0 / q2 93 / q8 1 | 8.36 | 2.04 | 24.4% | 24/24 (100.00%) |
| T3 | 29995 | completion_pressure:29974, inflight_zero:13, quantum_base:4, remaining_single:4 | q1 4 / q2 29987 / q8 4 | 7.92 | 2.00 | 25.2% | 7502/7504 (99.97%) |

- 分支比例：d16 上 completion_pressure 29,973/29,998（99.92%）、inflight_zero 16、quantum_base 3、remaining_single 6；T2 上 completion_pressure 72/94、inflight_zero 21、quantum_base 1；T1 全部 remaining_single（60,000/60,000）。
- quantum 分布：d16 q2 29,989/29,998（99.97%）；T1 q1 100%；T2 q2 93/94；T3 q2 29,987/29,995。
- ready vs 实际 batch：d16 平均 ready 7.92 → 实际呈现 2.00（25.2%）；R 动作（SHADOW 记录）present=ready（100%）、accepted/present=68.7%（SQ cap 生效，stop=cap 3425/7504）。
- 改变动作的分支占比：d16 99.95%、T2 100%、T3 99.97%、T1 0%。**判定：不是 NO_ADDITIONAL_STATE_EXPOSURE** —— 状态信息在 d16/T2/T3 上几乎每个决策点都会改变呈现动作；只有 T1 稀疏单条形状上 S 与 R 动作完全相同（remaining==1 已强制 immediate），这也正是 SHADOW 冻结 oracle（shadow_outstanding_sum==0）结构失败的形状。
- 诊断行含 measured/oracle/instrumented 三阶段（各约 1/3 决策数），比例按全部行计算；诊断运行不进入任何计时统计（§9.4）。

## 9. CPU 覆盖范围与残差（MAIN-W03-D16，CPU 分桶诊断）

| 臂 | submitter(main) | driver | io-wq | 未归因残差 | 残差最大单次 | ru_total ticks 合计 |
|---|---|---|---|---|---|---|
| A0 | 45.7% | 0.5% | 31.8% | 22.0% | 23.7% | 2962 |
| Q2 | 42.6% | 0.2% | 33.3% | 23.9% | 26.0% | 2595 |
| Q8 | 38.4% | 0.0% | 35.9% | 25.7% | 27.4% | 2352 |
| R | 38.4% | 0.0% | 36.1% | 25.5% | 27.4% | 2334 |
| S | 42.5% | 0.2% | 32.7% | 24.7% | 26.7% | 2617 |
| SHADOW | 38.9% | 0.0% | 35.7% | 25.4% | 27.2% | 2360 |
| SNI | 42.6% | 0.1% | 33.3% | 23.9% | 26.6% | 2587 |
| SNP | 39.5% | 0.1% | 34.8% | 25.6% | 27.6% | 2459 |

- 覆盖口径：wait4 rusage 总 ticks = app(main/submitter) + 其他用户线程(driver) + iou-* io-wq worker ticks + 未归因残差（10ms 采样粒度）。残差 22.0%–25.7%，逐 invocation 最大 23.7%–27.6%，如实披露；它限制 CPU 的线程级归因精度，但不改变 process CPU 总量（primary 口径）。
- driver 侧纯 submit 份额 ≤0.5%（本轮分桶实测）；提交全部发生在 submitter。

## 10. enter/机制指标（MAIN-W03-D16，r4 计数器；描述性）

| 臂 | presentations/op | cap_reentries/op | chunk_reentries/op | immediate | q1/q2/q8 | active_zero | pressure | authority_exits |
|---|---|---|---|---|---|---|---|---|
| A0 | 0.000000 | 0.000000 | 0.000000 | 0 | 0/0/0 | 0 | 0 | 0 |
| R | 0.125405 | 0.061702 | 0.000000 | 0 | 0/0/0 | 0 | 0 | 0 |
| S | 0.500017 | 0.000000 | 0.434878 | 90 | 90/1199948/2 | 160601 | 1199077 | 0 |
| SNI | 0.499758 | 0.000000 | 0.434754 | 276 | 276/1198902/240 | 0 | 1199178 | 0 |
| SNP | 0.191722 | 0.000000 | 0.127737 | 461 | 461/159130/300543 | 159138 | 0 | 0 |
| Q2 | 0.500051 | 0.000000 | 0.435020 | 244 | 244/1199878/0 | 0 | 0 | 0 |
| Q8 | 0.125352 | 0.000000 | 0.061631 | 414 | 414/0/300430 | 0 | 0 | 0 |
| SHADOW | 0.125337 | 0.061561 | 0.000000 | 0 | 395/300410/3 | 150830 | 300566 | 0 |

- 绝对次数为 24 块 × 5 reps × 20,000 读 = 2,400,000 读的合计。R 每次呈现约 8 条（presentations 0.1254/op、cap_reentries 0.0617/op）；S 每次呈现 2 条（0.5000/op、chunk_reentries 0.4349/op）；Q2 与 S 几乎相同。
- SHADOW 动作与 R 相同（presentations 0.1253/op、cap_reentries 0.0616/op）且读到了 hint（shadow_outstanding_sum 合计 1203419、shadow_window_sum 合计 50875，均非零，满足 §1.4 oracle）。

## 11. TRAJECTORY（状态变化轨迹，§3）

- 有效块：**0/24 → INCOMPLETE**。
- 原因：SHADOW 计时 invocation 触发冻结 oracle `shadow_outstanding_sum==0`（§1.4）——T1 稀疏单条形状上 outstanding() 在每个决策点都是 0，SHADOW 的「读并记录」和为零；每块 SHADOW/T1 槽位 3 次尝试全部失败（合计 72 条失败记录，`attempts.jsonl`），块状态 `done_with_failed_slots`，不构成完整 octet。
- 后果：per prereg 5.1 <24 valid blocks -> INCOMPLETE; no gate may PASS on TRAJECTORY; F-VALUE shape TRAJECTORY-T2 members NOT_ESTABLISHED。
- 描述性数字（**不进门槛**）：T1 上 R−A0 CPU +7.49%、S−A0 +12.13%、S−R +4.35%；T2 上 R−A0 −25.86%、S−A0 −8.61%、S−R +23.47%（p99 S−R +65.1%）；T3 上 R−A0 −20.30%、S−A0 −12.46%、S−R +9.84%。T2 每 rep 仅 64 读，p95/p99 分位粗（§12.3 已披露）。

## 12. 逐格状态表

source_sha 全部为冻结实现提交 `f3b25329bc8a64a201961c426a58adacfb8fdf08`（§1.1）；二进制：w03 = `f029607fc1a657ef…`（全量 `f029607fc1a657efe0ba96203115f27a90d598f12c274e9360b6029a5e019009`）、w02 = `daee6b6c1ab618a2…`（全量 `daee6b6c1ab618a29d8e1afc120f8da5e478756b2def7c77c712f54a6a4e1931`）。

| cell | case | arm/segment | source_sha | binary_sha | correctness | planned | valid | AA_status | measurement_status | failure_or_not_run_reason |
|---|---|---|---|---|---|---|---|---|---|---|
| MAIN-W03-D16 | W03-uring-4k-d16 | A0/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| MAIN-W03-D16 | W03-uring-4k-d16 | R/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| MAIN-W03-D16 | W03-uring-4k-d16 | S/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| MAIN-W03-D16 | W03-uring-4k-d16 | SNI/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| MAIN-W03-D16 | W03-uring-4k-d16 | SNP/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| MAIN-W03-D16 | W03-uring-4k-d16 | Q2/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| MAIN-W03-D16 | W03-uring-4k-d16 | Q8/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| MAIN-W03-D16 | W03-uring-4k-d16 | SHADOW/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| CORE-W03-D2SERIAL | W03-uring-4k-d2-r3d2serial | A0/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| CORE-W03-D2SERIAL | W03-uring-4k-d2-r3d2serial | Q8/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| CORE-W03-D2SERIAL | W03-uring-4k-d2-r3d2serial | R/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| CORE-W03-D2SERIAL | W03-uring-4k-d2-r3d2serial | S/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| CORE-W03-D4 | W03-uring-4k-d4 | A0/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| CORE-W03-D4 | W03-uring-4k-d4 | Q8/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| CORE-W03-D4 | W03-uring-4k-d4 | R/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| CORE-W03-D4 | W03-uring-4k-d4 | S/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| CORE-W03-D64 | W03-uring-4k-d64 | A0/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| CORE-W03-D64 | W03-uring-4k-d64 | Q8/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| CORE-W03-D64 | W03-uring-4k-d64 | R/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| CORE-W03-D64 | W03-uring-4k-d64 | S/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| CORE-W02-D1 | W02-uring-4k-d1 | A0/S | f3b25329… | daee6b6c1ab6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W02-D2 PASS (0.5518/0.5584/1.3262 pp) | MEASURED |  |
| CORE-W02-D1 | W02-uring-4k-d1 | Q8/S | f3b25329… | daee6b6c1ab6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W02-D2 PASS (0.5518/0.5584/1.3262 pp) | MEASURED |  |
| CORE-W02-D1 | W02-uring-4k-d1 | R/S | f3b25329… | daee6b6c1ab6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W02-D2 PASS (0.5518/0.5584/1.3262 pp) | MEASURED |  |
| CORE-W02-D1 | W02-uring-4k-d1 | S/S | f3b25329… | daee6b6c1ab6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W02-D2 PASS (0.5518/0.5584/1.3262 pp) | MEASURED |  |
| CORE-W02-D2 | W02-uring-4k-d2 | A0/S | f3b25329… | daee6b6c1ab6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W02-D2 PASS (0.5518/0.5584/1.3262 pp) | MEASURED |  |
| CORE-W02-D2 | W02-uring-4k-d2 | Q8/S | f3b25329… | daee6b6c1ab6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W02-D2 PASS (0.5518/0.5584/1.3262 pp) | MEASURED |  |
| CORE-W02-D2 | W02-uring-4k-d2 | R/S | f3b25329… | daee6b6c1ab6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W02-D2 PASS (0.5518/0.5584/1.3262 pp) | MEASURED |  |
| CORE-W02-D2 | W02-uring-4k-d2 | S/S | f3b25329… | daee6b6c1ab6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W02-D2 PASS (0.5518/0.5584/1.3262 pp) | MEASURED |  |
| CORE-W02-D16 | W02-uring-4k-d16 | A0/S | f3b25329… | daee6b6c1ab6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W02-D2 PASS (0.5518/0.5584/1.3262 pp) | MEASURED |  |
| CORE-W02-D16 | W02-uring-4k-d16 | Q8/S | f3b25329… | daee6b6c1ab6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W02-D2 PASS (0.5518/0.5584/1.3262 pp) | MEASURED |  |
| CORE-W02-D16 | W02-uring-4k-d16 | R/S | f3b25329… | daee6b6c1ab6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W02-D2 PASS (0.5518/0.5584/1.3262 pp) | MEASURED |  |
| CORE-W02-D16 | W02-uring-4k-d16 | S/S | f3b25329… | daee6b6c1ab6… | PASS: all invocations exit 0 + bench oracle; oracle suite BASE GREEN (evidence/correctness/RECEIPT.md) | 24 | 24 | AA-W02-D2 PASS (0.5518/0.5584/1.3262 pp) | MEASURED |  |
| TRAJECTORY | W03-uring-4k-d2-r3d2serial | A0/T1 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d2-r3d2serial | R/T1 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d2-r3d2serial | S/T1 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d2-r3d2serial | SNI/T1 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d2-r3d2serial | SNP/T1 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d2-r3d2serial | Q2/T1 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d2-r3d2serial | Q8/T1 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d2-r3d2serial | SHADOW/T1 | f3b25329… | f029607fc1a6… | FAIL: shadow_outstanding_sum==0 (prereg 1.4) on all 3 attempts | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) | SHADOW slot failed 3/3 attempts per block: shadow_outstanding_sum==0 (prereg 1.4); block status done_with_failed_slots -> no complete octet |
| TRAJECTORY | W03-uring-4k-d16-r3d16cap | A0/T2 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d16-r3d16cap | R/T2 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d16-r3d16cap | S/T2 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d16-r3d16cap | SNI/T2 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d16-r3d16cap | SNP/T2 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d16-r3d16cap | Q2/T2 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d16-r3d16cap | Q8/T2 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d16-r3d16cap | SHADOW/T2 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d16 | A0/T3 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d16 | R/T3 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d16 | S/T3 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d16 | SNI/T3 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d16 | SNP/T3 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d16 | Q2/T3 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d16 | Q8/T3 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| TRAJECTORY | W03-uring-4k-d16 | SHADOW/T3 | f3b25329… | f029607fc1a6… | PASS: exit 0 + oracle on all valid invocations | 24 | 0 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | INCOMPLETE (0/24 complete blocks) |  |
| PROT-64K-D16 | W03-uring-64k-d16-r3d16-64k | A0/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| PROT-64K-D16 | W03-uring-64k-d16-r3d16-64k | R/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| PROT-64K-D16 | W03-uring-64k-d16-r3d16-64k | S/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| PROT-64K-D16 | W03-uring-64k-d16-r3d16-64k | Q8/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| PROT-NONTMPFS | W03-uring-4k-d16 | A0/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| PROT-NONTMPFS | W03-uring-4k-d16 | R/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| PROT-NONTMPFS | W03-uring-4k-d16 | S/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| PROT-NONTMPFS | W03-uring-4k-d16 | Q8/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| PROT-SMT-2-12 | W03-uring-4k-d16 | A0/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 8 | 8 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| PROT-SMT-2-12 | W03-uring-4k-d16 | R/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 8 | 8 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| PROT-SMT-2-12 | W03-uring-4k-d16 | S/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 8 | 8 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| PROT-SMT-2-12 | W03-uring-4k-d16 | Q8/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 8 | 8 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| PROT-XPHYS-2-8 | W03-uring-4k-d16 | A0/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 8 | 8 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| PROT-XPHYS-2-8 | W03-uring-4k-d16 | R/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 8 | 8 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| PROT-XPHYS-2-8 | W03-uring-4k-d16 | S/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 8 | 8 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| PROT-XPHYS-2-8 | W03-uring-4k-d16 | Q8/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 8 | 8 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED |  |
| B-SUPP-W03-D16 | W03-uring-4k-d16 | B/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED (descriptive only, not in gates) |  |
| B-SUPP-W03-D16 | W03-uring-4k-d16 | R/S | f3b25329… | f029607fc1a6… | PASS: all invocations exit 0 + bench oracle | 24 | 24 | AA-W03-D16 PASS (1.0245/1.0286/2.0971 pp); AA-W03-D2SERIAL PASS (0.5059/0.5131/2.7086 pp) | MEASURED (descriptive only, not in gates) |  |
| AA-W03-D16 | AA-W03-D16 | A0/A0/S | f3b25329… | f029607fc1a6… | PASS (pure A/A; no oracle difference expected) | 24 | 24 | PASS: cpu 1.0245 / tput 1.0286 / p99 2.0971 pp | MEASURED |  |
| AA-W03-D2SERIAL | AA-W03-D2SERIAL | A0/A0/S | f3b25329… | f029607fc1a6… | PASS (pure A/A; no oracle difference expected) | 24 | 24 | PASS: cpu 0.5059 / tput 0.5131 / p99 2.7086 pp | MEASURED |  |
| AA-W02-D2 | AA-W02-D2 | A0/A0/S | f3b25329… | daee6b6c1ab6… | PASS (pure A/A; no oracle difference expected) | 24 | 24 | PASS: cpu 0.5518 / tput 0.5584 / p99 1.3262 pp | MEASURED |  |
| PROT-SATURATION(research arms) | PROT-SATURATION(research arms) | -/- | f3b25329… | - | NOT_RUN | 0 | 0 | n/a | NOT_RUN | NOT_RUN_WITH_REASON: frozen effective_arm rule (prereg 11.1.9) |
| OPEN_LOOP | OPEN_LOOP | -/- | f3b25329… | - | NOT_RUN | 0 | 0 | n/a | NOT_RUN | NOT_RUN_WITH_REASON: no frozen open-loop harness (prereg 3.2) |
| PROT-ODIRECT-COLD | PROT-ODIRECT-COLD | -/- | f3b25329… | - | NOT_RUN | 0 | 0 | n/a | NOT_RUN | NOT_RUN_WITH_REASON: no safe frozen O_DIRECT/cold harness (prereg 11.1.10) |
| W02-d3 | W02-d3 | -/- | f3b25329… | - | NOT_RUN | 0 | 0 | n/a | NOT_RUN | NOT_RUN_WITH_REASON: no frozen W02 d3 case (prereg 2.1) |

（`arm/segment` 中 S 表示单段 invocation；TRAJECTORY 行按 T1/T2/T3 分段。扩展功能格 PROT-THREADPOOL/PROT-64K eligibility 的判定见 `analysis/extension/EXTENSION_SUMMARY.json` 与 `analysis/extension/EXTENSION_RESULTS.md`，本轮未重跑其原始调用。）

## 13. NOT_RUN / NOT_ESTABLISHED 清单

- §10.5 正控 `r3d2pos`：NOT_RUN（raw/ 中无该 case 的任何 invocation）。
- §10.5 W03-d16 enter/CQE 对账（每臂 3 次 strace）：NOT_RUN（仅有 DIAG-64K-STRACE 与 PROT-THREADPOOL 的 strace）。
- §8.1(a) 静态审计 `logs/audit.txt`：NOT_RUN（文件不存在）。
- §8.1(c) valgrind/massif 分配斜率：NOT_RUN（无证据文件）。
- §11.1.9 PROT-SATURATION 研究臂：NOT_RUN_WITH_REASON（冻结 effective_arm 规则）。
- §3.2 OPEN_LOOP：NOT_RUN_WITH_REASON（无冻结 open-loop harness）。
- §11.1.10 PROT-ODIRECT-COLD：NOT_RUN_WITH_REASON（无安全冻结 O_DIRECT/cold harness）。
- §2.1 W02-d3：NOT_RUN_WITH_REASON（冻结对象集无 W02 d3 case）。
- §12.3 逐请求 ECDF：未保存逐请求延迟（仅 rep 内 p50/p95/p99），本报告只用运行级百分位。

## 14. 判定树与总判决（§13）

1. 三 A/A：全 PASS（§6/§13.1 通过）。
2. 正控 + enter/CQE 对账：**NOT_RUN → §13.2 不满足**，主门不得宣告 PASS。
3. 主门统计条件：R FAIL（p99）、S FAIL（p95/p99）；资源门 NOT_ESTABLISHED → 不得 GO_MECHANISM。
4. F-VALUE：NOT_ESTABLISHED。
5. 保护族：成员 FAIL 存在（见 §4）→ §13.6 不得 GO（整体）。
6. 总判决：**INCONCLUSIVE/PARTIAL**，附带的确定性 FAIL 事实：主门尾延迟条件失败、F-PROT 39 个成员界越界、TRAJECTORY INCOMPLETE。
7. 产品层：**PRODUCT=HOLD**（research-only；不开产品候选 PR、不改 KEEP_IMMEDIATE）。

## 15. 复现与产物

```
cd /home/jnhu/sluice-595-e5/r4
python3 scripts/matrix_analysis.py        # -> analysis/MAIN_SUMMARY.json (stdlib only)
python3 scripts/make_figures.py           # -> figures/F1..F5 + figure-data.csv + CAPTIONS.md
python3 scripts/render_status_table.py    # -> analysis/per_cell_table.md
python3 scripts/render_results.py         # -> RESULTS.md (run last: embeds shas)
```

（不用 `python3 -I`：隔离模式排除 user site-packages，matplotlib 不可用；matrix_analysis.py 仅用标准库，`-I` 可用但为统一口径按上述命令执行。）

sha256（终审订正后实测；`MAIN_SUMMARY.json` 的 `generated_utc` 每次重跑会变，除该字段外内容逐字节一致——多次重跑已核对）：

- `analysis/MAIN_SUMMARY.json`：**内容 sha256**（剔除 `generated_utc` 后 `json.dumps(sort_keys=True)`）= `317bf9a5120a34de120dc9cbe138ae456f653fa3ba55b354a23ec97eb2e512dd`；文件 sha256 随 `generated_utc` 变化（10:03:24Z→`48381146…`、10:23:29Z→`758f07d2…`、10:31:04Z→`fc1b908e…`、10:36:43Z→`3d56dd2c…`），内容不变。以上生成值均来自本轮重跑，未替换任何统计数字。
- `scripts/matrix_analysis.py` = `74e92ec25496741dccd024fee74de52b63a8c63521167944f56ef519c32230b2`
- `scripts/make_figures.py` = `294384497644380e273f5f620f39e59efd81b0a225c2d7d5075e343c17231312`
- `scripts/render_status_table.py` = `1c3aef2ee23acf2d5a50e28141ece0c9fa2f030471ea34b9da926471cb62aed7`
- `scripts/render_results.py` = `2cf6d78b93efa090633cb4283854feb53d9fc2f2a4c12a66e26bf4f23832d6ed`
- `figures/figure-data.csv` = `c2c3294b9f8b124914602d16c9f93f178aafb14be2fb3b98e3f581aae02c8096`

产物：`figures/F1_net_effect.{svg,pdf,png}`、`F2_behavior_change.*`、`F3_decomposition.*`、`F4_cpu_tail.*`、`F5_trajectory.*`、`figures/figure-data.csv`、`figures/CAPTIONS.md`。


## 16. 终审八问裁决（R4）

裁决口径：只采用本轮已核验证据（`raw/`、`analysis/MAIN_SUMMARY.json`、`analysis/extension/EXTENSION_SUMMARY.json`、`evidence/correctness/RECEIPT.md`、本文件 §1–§15）。"FAIL" 由预注册界越界定义；"NOT_ESTABLISHED" 表示证据不足或形状 INCOMPLETE，不产生正/负结论。

### 16.1 suffix 再成批是否有效？

**结论：语义上有效且被变异证据锚定；性能上只在存在合发机会的形状有正方向证据，稀疏形状无效，且与 R3 suffix-singles 的对照仅是描述性，不能升级为门槛结论。**

- 语义/正确性：R/S 在 `stop=cap && accepted>0` 时对 untouched 后缀经同一批量入口重入（`r3_ready_batch.hpp` 冻结语义，PREREG §1.5）；M1 重复接受、M2 零进展续环、M3 authority 边界三个变异全部被杀死，恢复 GREEN 后 header sha256 与 git blob 逐位一致（`evidence/correctness/RECEIPT.md` §2–§4，sha256 `363f346a…`）。
- 性能方向：W03-d16 的 24 块描述性 B 补充块（同一冻结二进制、同协议、事前附录 `evidence/B-SUPPLEMENT-APPENDIX.md`）中，R−B 的 CPU 为 +9.04%（上界 +10.25%，正 = B 比 R 差）、吞吐 +8.21%（上界 +9.23%）、p95 +32.17%（上界 +34.81%）、p99 +14.50%（上界 +16.68%）——方向支持"后缀再成批"优于 R3 的 suffix singles。
- 反证/限制：B 补充块是描述性、不进任何门槛（PREREG §11.4），且 R−B 的横向块来自独立会话；W03-d2serial（每跨度 1 条、无可合发）上 R/S/Q8 的 CPU/吞吐/p95/p99 全部越界（+6.47%…+14.09%，12/12 FAIL），说明"再成批"在稀疏形状只有额外成本。因此不能表述为"suffix 再成批普遍有效"；有效范围限定为 W03-d16 类存在多 ready 的形状，且为描述性方向。

### 16.2 状态是否被实际消费并改变动作？

**结论：是，且已量化；不是 NO_ADDITIONAL_STATE_EXPOSURE。但这是诊断证据，不是计时收益证据。**

- 诊断 `raw/diagnostics/*.diag`（三阶段 measured/oracle/instrumented）：W03-d16 上 S 29,998 个决策，`completion_pressure` 29,973（99.92%）、`inflight_zero` 16、`quantum_base` 3、`remaining_single` 6；quantum 分布 q2 29,989（99.97%）、q1 6、q8 3。T2：completion_pressure 72/94、inflight_zero 21、quantum_base 1；T1 全部 remaining_single（60,000/60,000）；T3 与 d16 同型。
- 动作改变（同一决策点 S 表动作 vs SHADOW 记录的 R 实际动作）：W03-d16 7,500/7,504 = 99.95%、T2 24/24 = 100%、T3 7,502/7,504 = 99.97%、T1 0/60,000 = 0%。
- SHADOW 读取反编译器消除计数器非零（冻结 oracle）：W03-d16 shadow_outstanding_sum 合计 1,203,419、shadow_window_sum 50,875（`analysis/MAIN_SUMMARY.json` awareness；对应 §1.4 冻结判据）。
- 反证/限制：诊断运行不进入任何计时统计；它只证明"表选择了不同动作"，不证明该动作更好。T1 上 S 与 R 动作完全相同（remaining==1 强制 immediate）——这也是 SHADOW-T1 冻结 oracle `shadow_outstanding_sum==0` 结构失败的形状（TRAJECTORY 0/24 的根因）。"改变动作"的比率按全部诊断行计（三阶段各约 1/3）。

### 16.3 感知相对普通 batching 是否有额外价值（路线 A/B）？

**结论：F-VALUE = NOT_ESTABLISHED（两条路线均未通过；T2 成员因 INCOMPLETE 记 NOT_ESTABLISHED）。**

- 路线 A（W03-d16，S−R）：CPU 单侧上界 +9.31%（要求 ≤−2%）、p95 +31.78%（≤+5%）、p99 +11.63%（≤+5%）、吞吐 +8.50%（≤+3%）——全部不满足；Holm α=0.0125，p=1.0。TRAJECTORY-T2 成员 CPU 上界 +25.87%、p95 +70.39%、p99 +70.63%、吞吐 +29.94%，形状 0/24 INCOMPLETE → NOT_ESTABLISHED。
- 路线 B（W03-d16）：p99 上界 +11.63%（要求 ≤−5%）、CPU +9.31%（≤+2%）、吞吐 +8.50%（≤+3%）——不满足；T2 同样 NOT_ESTABLISHED。
- 机制侧旁证：S≈Q2（CPU +0.83%，上界 +1.29%）、S−Q8 +7.98%（上界 +8.70%）——表在 d16 上 99.97% 选择 quantum=2，感知退化为固定 2；SHADOW−R 纯读取成本只有 CPU +1.02%（上界 +1.94%），而 S−R 为 +8.51%（上界 +9.31%），差距主要来自"动作/量子选择"而非"读 hint"。
- 反证/限制：TABLE_OVERHEAD_FLAG 置位（S−Q8 CPU 上界 +8.70% > +3%）；T2/T3 描述性数字（T2 S−R CPU +23.47%、p99 +65.12%）不进门槛；"感知无价值"只在本轮冻结表与 W03/W02 形状上成立，不能外推为所有状态感知策略的否定。

### 16.4 对 A0 是否形成 CPU/尾延迟联合优势？

**结论：否。R 在 CPU/吞吐维度成立但 p99 反例；S 在 p95/p99 都退化；主门合取 R FAIL、S FAIL；且协议前置 NOT_RUN 使统计条件通过也不得宣告 PASS。**

- R−A0（W03-d16）：CPU 点 −19.62%（≤−10% PASS）、CPU 上界 −19.11%（≤−5% PASS）、吞吐上界 −23.74%（≤+3% PASS）、p95 上界 +4.33%（≤+5% PASS）、p99 上界 +23.58%（≤+5% **FAIL**）。p99 差值 24/24 块为正（+6.4%…+32.6%），非个别块。
- S−A0：CPU −12.81%（上界 −12.39% PASS）、吞吐上界 −14.19%（PASS）、p95 上界 +34.66%（**FAIL**）、p99 上界 +36.25%（**FAIL**）；p95/p99 差值 24/24 为正（+27.3%…+38.0% / +25.3%…+46.9%）。
- 联合优势要求 CPU 与尾延迟同时成立；本轮只有 CPU/吞吐成立，尾延迟在 R 的 p99 与 S 的 p95/p99 明确失败。另：§10.5 正控 `r3d2pos` 与 W03-d16 enter/CQE 对账 NOT_RUN → 依 PREREG §13.2 主门不得进入 PASS 分支，§13.7 排除 GO_MECHANISM。
- 反证/限制：p95 对 R 在界内（+4.33%），说明 R 的尾延迟问题集中在 p99；本轮未识别 p99 回归的物理机制（完成顺序/cap 截断/io-wq 唤醒），不能给出"修掉某一行即可"的结论。

### 16.5 哪些 workload 获益/退化？

**结论：获益限于 W03 多 ready 形状的 CPU/吞吐维度；退化集中在稀疏 d2serial、深/浅 d64/d4 的尾延迟、W02 系的小幅保护界越界、以及非 tmpfs 上 S 的尾延迟。**

- 获益（方向，非全部过门）：W03-d16 R/S 的 CPU 与吞吐（R −19.62% CPU / 吞吐上升；S −12.81%）；W03-d4 的 R/Q8 全部 8 个成员在界内（S 仅 p99 越界）；W03-d64 的 CPU 方向仍为负（R −21.33%、S −11.70%、Q8 −21.40%），但尾延迟越界。
- 退化：W03-d2serial R/S/Q8 全部 12 个成员越界（CPU +6.94%…+11.57%、吞吐 +6.47%…+10.36%、p95 +7.61%…+10.37%、p99 +7.17%…+11.61%）；W03-d64 的 Q8/R p99（+20.52/+24.92、+21.17/+25.65）与 S p95/p99（+13.78/+14.70、+25.06/+32.73）；W03-d4 S p99 +18.39%；W02-d1 6/12 FAIL、W02-d2 6/12 FAIL、W02-d16 8/12 FAIL；PROT-NONTMPFS S p95/p99 +23.13/+24.23、+19.45/+21.45。
- 中性/功能：PROT-64K-D16 eligibility 门 PASS（24/24；S/Q8 全部 immediate，enters=A0，coalesced=0，max SQE/enter=1），描述性尾延迟 +3–13%；PROT-THREADPOOL 功能门 PASS（12/12；不新增线程/队列/timer；研究臂回落产品路径）；PROT-SMT-2-12 与 PROT-XPHYS-2-8 各 8 tuples 全部在界内（safety-only，功效低）。
- 反证/限制：这些是格内配对差，不是跨机/跨 fixture 的可比绝对量；W02 系与 W03 系工作负载不同，不能合并成"普遍退化/普遍获益"；B 补充块只描述 W03-d16。

### 16.6 所有执行者 CPU 是否充分覆盖（含残差声明）？

**结论：对主指标（whole-executor process CPU）充分覆盖；对线程级归因不充分，残差 22.0%–25.7%，必须如实披露且不改变主门口径。**

- 主指标为每 measured rep 的 `CLOCK_PROCESS_CPUTIME_ID` 差值，whole-executor：本内核 io-wq worker 在该进程 task 内，分桶证实 iou-* ticks 已包含；secondary wait4 `ru_*` 与 primary 符号一致（R/S 均为 −1.00，`analysis/MAIN_SUMMARY.json` F_MAIN caliber）。
- W03-d16 分桶（§9）：submitter 38.4%–45.7%、driver ≤0.5%、io-wq 31.8%–36.1%、未归因残差 22.0%–25.7%（逐 invocation 最大 23.7%–27.6%，10ms 采样粒度）。残差限制的是"把 CPU 差归到哪个线程"的精度，不是 process CPU 总量。
- 未覆盖/未识别：无 PMU per-thread 计数（perf paranoid=2），无 IRQ 归因进臂差；入口→enter 纳秒级延迟不可观测（冻结二进制 stripped）；driver 侧纯 submit 份额 ≤0.5%。
- 反证/限制：残差 22%–26% 意味着不能声称"CPU 收益全部来自 submitter"；本轮没有做 submitter/driver 分离的受控实验，故因果归因仍受限。

### 16.7 真实应用证据（#604）？

**结论：NO。无 #604 `READY_FOR_595` handoff，本轮全部为合成 benchmark（W02/W03）；不得声称真实应用证据。**

- `gh issue view 604 --repo jnhu76/Sluice`：state=OPEN、labels=[]、updatedAt=2026-11-03T20:37:04Z；body/comments 无 READY_FOR_595（campaign 内亦无 handoff 文件；PREREG §13.8 明示 "#604 handoff 不存在"）。
- W03/W02 是合成代表 trace（4KiB/64KiB warm tmpfs、external loop/pipeline），不是产品 consumer；#604 未定义 consumer/oracle/预算/baseline。
- 反证/限制：即使机制门未来全过，产品结论也不随 GO_MECHANISM 自动改变；真实应用须独立预注册后再测。

### 16.8 推荐 A0、R、S 还是 HOLD？

**结论：HOLD（research-only）。产品默认保持 A0（KEEP_IMMEDIATE）；R 只作为 CPU/吞吐方向的研究结果；S 不推荐，且本轮不因一次失败继续调参。**

- A0：唯一无尾延迟回归、无保护族越界的基线；仍为默认。
- R：W03-d16 CPU −19.62%、吞吐 −23.74%（上界），但 p99 +23.58% 上界（24/24 块同向）→ 不能产品化；适用范围仅限 CPU 受限、尾延迟不敏感、W03-d16/d64 类多 ready warm tmpfs 的研究场景。
- S：CPU −12.81% 但 p95/p99 双 FAIL，且相对 R 无额外价值（F-VALUE NOT_ESTABLISHED）；不推荐产品化，也不建议在本轮冻结版本上继续调表。
- HOLD 的依据：主门 R/S FAIL；F-PROT 39 个成员界越界（4 个 NOT_ESTABLISHED）；资源门 NOT_ESTABLISHED（3 个子门 NOT_RUN）；协议前置 NOT_RUN；TRAJECTORY INCOMPLETE；#604 无交接。依 PREREG §13.6/§13.7/§13.8，不得 GO_MECHANISM、不得开产品候选 PR。
- 后续唯一动作建议：把本轮作为负/不确定结果归档；如需继续，先补协议正控与资源子门，再就 p99 机制（完成顺序/cap 截断/io-wq 唤醒）做一个独立预注册实验；产品工作等待 #604 `READY_FOR_595`。

## 17. 独立复核与订正（终审）

复核者在本轮只读/只写 r4 树内，未重跑计时；以下检查均在本次 ask 中实际执行。

### 17.1 复核动作与结果

1. **分析重跑**：`python3 -I scripts/matrix_analysis.py` 多次重跑；各次 `MAIN_SUMMARY.json` 除 `generated_utc` 外逐字节一致（`json.dumps(sort_keys=True)` 比较 `equal_excluding_ts=True`，内容 sha256 `317bf9a5…`）。与现有 summary 的 F-MAIN/F-PROT/F-VALUE/RESOURCE/TRAJECTORY 字段逐项一致。
2. **F-MAIN 独立重算**：直接读 `raw/matrix/MAIN-W03-D16/block-*.json` 的 24 块，逐块用 invocation `metrics` 重算配对差（cpu/tput/p95/p99）。R：cpu mean −19.6239%、p99 mean +21.8515%（24/24 为正）；S：cpu −12.8127%、p95 +33.6832%（24/24 正）、p99 +34.5726%（24/24 正）——与 summary 的 mean/upper 和各条件判定一致。
3. **F-PROT 计数**：`gates.F_PROT.m`=84，`members`=84；class 计数 PASS=41、FAIL_BOUNDS=39、NOT_ESTABLISHED=4；FAIL 成员 p 最小 0.2432（无 FAIL 依赖 Holm 拒绝）；逐格计数与 §4 表一致。
4. **F5 原始重算**：从 `raw/diagnostics/TRAJECTORY__S__{T1,T2,T3}.diag` 逐行解析（忽略 `# phase` 头），得 T1 mean_ready=1.0、mean_present=1.0、q2share=0.0（n=60,000）；T2 8.361702127659575 / 2.0425531914893615 / 0.9893617021276596（n=94）；T3 7.923387231205201 / 2.0003333888981496 / 0.9997332888814803（n=29,995）。与订正后的 CSV 一致。
5. **对象核验**：`r4/worktree` HEAD=`f3b25329…`、porcelain=0；master `271b81bf…`、porcelain=0；PREREGISTERED sha256=`fbb2e785…`；`bench/r3_ready_batch.hpp` sha256=`363f346a…`；冻结二进制 w03 `f029607f…`、w02 `daee6b6c…` 与 manifest 一致。
6. **#604**：`gh issue view 604` 返回 OPEN、无标签、updatedAt 2026-11-03T20:37:04Z；无 READY_FOR_595。
7. **扩展格**：`raw/extension` 计数 24/24（64K、NONTMPFS）与 8/8（SMT、XPHYS）；`EXTENSION_SUMMARY.json` 的成员数据与 §4/§5 引用一致。

### 17.2 发现的差异与订正

**差异 1：`figures/figure-data.csv` 的 F5 state 行 T1/T2 取错值。**
- 现象：订正前第 336–337 行（T1）与第 339–340 行（T2）的 `mean_ready`/`mean_present` 写成 T3 的值 `7.923387231205201` / `2.0003333888981496`；T3 行（342–343）正确。
- 根因：`scripts/make_figures.py` 的 F5 循环内用 `ready[-1]`/`present[-1]`（列表末元素）而非当前段索引；`q2share[-1]` 因在循环内先 append 而恰好正确。F5 图本身用完整列表绘制，故图与 RESULTS §8 的 1.00/1.00、8.36/2.04、7.92/2.00 正确；只有 CSV 受影响。
- 订正：改为 `enumerate(segs)` 并用 `ready[i]`/`present[i]`；重跑 `python3 scripts/make_figures.py`（exit 0）。订正后第 336–344 行为 T1 1.0/1.0/0.0、T2 8.361702127659575/2.0425531914893615/0.9893617021276596、T3 7.923387231205201/2.0003333888981496/0.9997332888814803。
- 影响面：仅 `figure-data.csv`（sha256 `c2c3294b…`）；五张图的 PNG sha256 与订正前逐一相同（F1 `42868c39…`、F2 `356abbd8…`、F3 `f9fe24cd…`、F4 `4530623e…`、F5 `2200fed3…`），即图内容未变。SVG 因 matplotlib 写入生成时间元数据而 sha 变化，绘制内容相同。

**差异 2：同文件 F5 `quantum2_share` 行单位与数值不一致。**
- 现象：订正前 T2 行值 `98.93617021276596`、T3 行 `99.97332888814802`，`unit` 列写 `fraction`；同文件 F2 的 `quantum_share` 用 fraction（如 `0.9996999799986666`）。
- 订正：F5 state 行改存 fraction（`share = counts/n`），与 F2 口径一致；图轴仍按百分比绘制（`100*share`）。订正后 T2 `0.9893617021276596`、T3 `0.9997332888814803`。

**未发现其他数值差异**：本次复核未发现 F-MAIN/F-PROT/F-VALUE/RESOURCE/TRAJECTORY 的摘要数字与 raw 重算不一致；`MAIN_SUMMARY.json` 的 sha 变化仅由 `generated_utc` 引起。

**文档一致性小问题（不影响判定）**：`scripts/matrix_analysis.py:16` 的 docstring 声称同时输出 `analysis/CELL_STATUS.json`，但脚本实际只写 `analysis/MAIN_SUMMARY.json`（`grep -n "CELL_STATUS"` 仅命中该 docstring；该文件不存在）。证据以 `MAIN_SUMMARY.json` 为准；未改动脚本以免改变已引用的脚本 sha。

### 17.3 复核局限（如实声明）

- 未重跑任何计时块；所有性能数字复用本轮已记录的 raw 数据与冻结二进制，未在本 ask 中重新测量。
- 未跑 `xmake test` 全 82 用例矩阵（正确性回执已注明）；未跑 §10.5 的 `r3d2pos` 正控与 W03-d16 enter/CQE 对账；未跑 §8.1(a) 静态审计、(b) strace、(c) valgrind/massif 分配斜率——这些仍是 NOT_RUN，资源门因此 NOT_ESTABLISHED。
- 未逐请求重算 ECDF（raw 只存 rep 内 p50/p95/p99，未存逐请求延迟）；未对全部 84 个保护成员逐一从 raw 重算，只核验了 summary 的成员分类/计数与失败明细；未重算 B 补充块的 bootstrap 上界（采用 summary 与附录口径）。
- 未重新构建 worktree 或二进制；对象核验只做 git/sha256 比对。
