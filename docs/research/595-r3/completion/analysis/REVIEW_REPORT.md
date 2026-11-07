# R3 续跑独立统计复核报告（REVIEW_REPORT）

- 复核员角色：独立统计复核员（未参与测量执行；本会话未运行任何 benchmark）。
- 复核对象：`/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z`（prereg `evidence/PREREGISTERED_R3_COMPLETION.md`，sha256 `1a9c1e12…`）。
- 数据来源：仅 `raw/{baseline,calibration,mechanism,protection}`、`evidence/measurement-*`、`logs/`、预注册文件，以及对 R2/R3 树与二进制的只读复验。
- 本复核脚本（均在 `analysis/`）：
  - `review_pairs.py` → `review_pairs_out.json`（5 个两臂块：有效性 + 配对统计 + bootstrap 10,000/seed 5950/percentile 95% CI + A/A 半宽门 + 负控单侧门/Holm + 与执行员 summary 数值比对）
  - `review_blocks.py` → `review_blocks_out.json`（SAFE-A0/DIAG/RESOURCE/SELFTEST；strace enter 独立重分类）
  - `review_mechanism.py` → `review_mechanism_out.json`（机制窗 9 个 run 的 bpftrace/bench.jsonl/PMU/alloc/bucket/resource 独立重算）
  - `review_tables.py` → `review_tables_out.json`（空表/缺失块/镜像一致性）
  - 运行方式：`python3 -I <script>`（Fedora 44，kernel 7.2.11-200.fc44）。

## 1. 逐门判定（独立重算）

| # | 门 | 判定 | 关键数字（本人重算） |
|---|---|---|---|
| 1 | AA-W03-D16 半宽 5/3/3 pp | **PASS** | cpu 0.7973 / tput 0.8207 / p99 2.0385 pp；24/24 有效对；mean −0.816% [−1.616,−0.022]% |
| 2 | AA-W02-D2 半宽 5/3/3 pp | **PASS** | cpu 0.8617 / tput 0.8534 / p99 1.4203 pp；24/24；mean +0.584% [−0.270,+1.454]% |
| 3 | 两臂块有效性（配对/顺序/exit/ops/oracle/SHA/读回） | **PASS** | 5 块各 24/24 有效对（12 AB+12 BA / AA 偶 m1m2 奇 m2m1，0 顺序违例）；240/240 invocation exit=0；每 invocation 5 measured rep × 恰 20,000 ops 且 ok=true；results 与 bench jsonl 字段逐一对应（0 不符）；每块单一二进制 SHA（R3 `0ebfb7f0…` / R2 `32beabcd…`）；48/48×5 日志含 CAL-oracle-self-test 且 failing repetitions=0；A0/A1/B readback 与 env 一致（R2 臂见 §3-g 限制） |
| 4 | NEG-LEGACY-W02-D2（R2-D2 核心负控，5/5/5/3 单侧） | **FAIL**（=执行员结论） | cpu +3.435% upper +4.065%（≤5 通过）；p95 +5.233% upper **+5.906%**（>5）；p99 +7.052% upper **+9.067%**（>5）；tput +3.298% 点估计与 upper **+3.889%** 均 >3 → `LEGACY_D2_NEGATIVE_CONTROL_FAIL`（仅旧 B_R2 机制；strace 诊断 A/B 各 3×60,004 enter、60,000 纯 submit、hist {1:60004}，无合发——退化来自门链+flush 后移，非批量） |
| 5 | PROT-W02-D3（R2 同 SHA）5/5/5/3 | **PASS** | cpu −7.327% upper −6.673%；p95 −12.795% upper −10.832%；p99 −11.511% upper −7.288%；tput −7.975% upper −7.207%；Holm 全 reject |
| 6 | PROT-W02-D16（R2 同 SHA）5/5/5/3 | **PASS** | cpu −28.111% upper −27.613%；p95 −32.590% upper −31.819%；p99 −32.400% upper −31.179%；tput −39.241% upper −38.264%；Holm 全 reject |
| 7 | R2 同 SHA 配对 + R2 归档方向 + 跨 SHA 只并列 | **PASS** | 三个 R2 块全部双臂同 SHA `32beabcd…`；我独立复验 `/home/jnhu/sluice-595-e5/worktree` HEAD `271b81bf`、`git diff HEAD`=`b006dbbf…`、reverse-check 回执 rc 0、binary sha=`32beabcd…`；方向与归档一致（D2 恶化：新 +3.43/+3.30/+5.23/+7.05 vs 归档 +3.73/+3.58/+4.96/+7.07；D3 改善：−7.33 vs −6.69；D16 改善：−28.11 vs −26.36，CI 重叠）；未发现任何 R3−R2 跨 SHA 相减（§5 bucket 表与 §3.3 历史引用均为并列/引述） |
| 8 | D2 正控（2→1 enter、2 data SQE/2 结果） | **PASS（相位粒度）** | d2pos：A0/A1 每 invocation 6 纯 submit（每相位 2×to_submit=1）；B 3 纯 submit 全部 to_submit=2（每相位 1 次）；opcode-22 data SQE=6（=3×2）；每相位 accepted=2、stats_sub/comp=2、oracle ok、exit=0；ret==to_submit 零部分提交。偏离：1 invocation×3 相位，而非 prereg §5.5 的 3 次独立 invocation（执行员已披露） |
| 9 | NEG-R3-W03-D2SERIAL 机制形状（不合发/无新增 defer/residue/eventfd） | **PASS（诊断口径）** | d2serial 三臂 enter/SQE/ret/直方图完全一致（60,000×(1,0,0)+4 探针）；B `group=0, prefix_max=1, suffix=0, untouched=0, flush=20000`；`io_uring_defer=0`；`eventfd2=4`、`iouring_setup=4`、`register=12` 三臂相同；ret==ts、stats 20,000/20,000 → 无 residue。性能侧（CPU/p95/p99≤5%、tput≤3%）为 BLOCKED，未运行 |
| 10 | W03 d16 B−A0 主门（点 ≤−10%、upper ≤−5%、tput ≤3%、p95/p99 ≤5%） | **NOT_RUN（BLOCKED_SEMANTICS_NOT_READY）** | 无 24-triplet 三臂性能块：`measurement-main/` 只有 `aa/`；B/A1 数据仅存在于诊断窗（prereg §12.1 禁止进入配对统计）。诊断方向（非门证据）：bucket bench cpu_s B 0.0573 vs A0 0.0700、PMU cycles −14.1%、enter −51.29% |
| 11 | 内存 ≤ +2% | **NOT_ESTABLISHED（门）；诊断方向在界内** | 正式 RESOURCE-B BLOCKED；诊断 `/usr/bin/time -v` RSS A0 5204 / A1 5268 / B 5252 KB → B +0.92%（≤+2%） |
| 12 | 零新增线程/timer/队列/每请求分配 | **诊断证据 PASS；正式门 NOT_ESTABLISHED** | strace -c 三臂逐项相同：clone3=4、io_uring_setup=4、register=12、eventfd2=4、mmap=82、munmap=17、brk=18、epoll_create1=3，无 timer_create/timerfd_create 行（=0）；`/proc` 峰值线程 3/3/3；malloc 8,796/8,802/8,802（+6 次一次性），60,000 ops 无每请求分配；valgrind 斜率子门 NOT_RUN（bench 无 request-count 旋钮，已登记） |
| 13 | 机制表：纯 submit/wait 分桶覆盖与残差 | **PASS** | 9/9 run 全部 enter 事件可分桶（pure submit / submit+GETEVENTS 探针 / 纯 wait=0）；d16-B 29,225 纯 submit = 24,826×1 + 4,395×8 + 3+2+1+1，max=8≤cap，driver 纯 submit=0；对账：1,355 组×8 + 9,160 单 = accepted 20,000；untouched 9,160 == suffix_single 9,160；flush 10,515 == entry 10,515（无 defer/SQE 滞留）；bucket 未归属残差 +5.2…+6.7 ticks（10ms tick 粒度，已披露）；机制存在性 ≥50% enter 减少达标（51.29%），但与 census 预测 91.5–93.3% 量级不符（执行员已如实登记） |
| 14 | 空表检查 | **无空表**（emptyTables=false） | measurement-main/aa、measurement-legacy、measurement-negative/protection、measurement-mechanism、measurement-product(HOLD) 现存表全部非空；raw↔evidence 镜像数值一致 |
| 15 | 完成门（prereg §7.2/§11.3/§14） | **FAIL** | `analysis/COMPLETION_GATE.json` **缺失**（analysis/ 在本复核前为空）；主门/PROT-B/RESOURCE-B 表未生成（blocked 可接受，但无完成门文件记录状态）；两个 enabledBlocks 从未执行且未记 NOT_RUN（见 §3-c） |

## 2. 与执行员摘要的数值比对

- 5 个两臂块：我的 bootstrap（10,000/seed 5950/percentile）与执行员 `summary.json` 的 mean/ci_lo/ci_hi/half_width/ci_upper_one_sided_95 **全部在 1e-9 内一致**（`review_pairs_out.json` `numeric_diffs` 均为空）。
- 机制表、PMU、alloc、bucket、resource、diag enter 计数：与 `MECHANISM_SUMMARY.md`/`CALIBRATION_SUMMARY.md`/protection `SUMMARY.json` 逐项一致（均为本人从 bpftrace.out/strace/perf.csv/bench.jsonl 原始输出重算）。
- A/A 校准自测 `ANALYSIS-SELFTEST/selftest.log`：8 PASS / 0 FAIL（本人重读日志）。

## 3. 差异与偏离登记（与执行员摘要的结构性差异/披露项）

a. `analysis/COMPLETION_GATE.json` 缺失 → 按 prereg §7.2 本 run 不能声称 completion（只能 partial）。执行员摘要未出具该文件。
b. `raw/calibration/{AA-W03-D16,AA-W02-D2,R2-D2}` 缺 `attempts.jsonl`（prereg §7.1 要求每块携带）；PROT 块有（空=0 次替换）。所有 240 条记录 attempt=1，无替换发生，故无数据损失，但账面偏离。
c. 两个 enabledBlocks 从未执行且未在任何回执中记为 NOT_RUN：`SAFE-A0-W03-D4-D16-D64` 的 d4/d64 4k A0 基线（d16 已由 AA-W03-D16 的 48 次 A0 invocation 覆盖）与 `SAFE-A0-W02-D16`（R3 W-02 d16 A0 描述性）。全树 grep 无这两个 case 的任何数据。
d. d2pos 正控重复数偏离：prereg §5.5 要求 3 次独立 invocation/臂 + strace；实际 1 invocation×3 相位（bpftrace）。MECHANISM_SUMMARY §7 已披露；相位粒度 3/3 全部 to_submit=2。
e. 64 KiB case id 偏离：实际 `W03-uring-64k-d16-r3d16-64k`（研究 case）≠ prereg `W03-uring-64k-d16`；SUMMARY limitations 已披露。数据本身合规：3 invocation × 5 rep × 2,000 ops（冻结值，未重标定），enter 6,000 纯 submit 全 to_submit=1（64k 不合批，符合预期）。
f. ThreadPool 字面门 `io_uring_enter=0`：实测每 invocation 恰 1 次 enter（statx 可用性探针，GETEVENTS；本人重读 9 份 strace：d4/d16/d64 各 3 份，workload 相位纯 submit=0）；执行员已作为 limitation 披露。该块本为 BLOCKED（B 侧），不构成门翻转。
g. R2 臂无 bench stdout arm 读回（R2 bench 不打印 `research arm=`）：PROV-7 只能以 results 记录的 env+成员命名佐证；臂行为差异（D3/D16 明显加速、D2 不加速且 enter 无差异）与 SLUICE_595_ARM 语义一致。登记为来源门弱化，非无效。
h. AA-W03-D16 的 CPU/tput mean 偏移 −0.82%/−0.87%（CI 不含 0）：prereg §8.2 只设半宽门，非失败；登记。
i. 机制量级与 census 预测不符（观测 51.29% vs 预测 91.5–93.3%）：机制存在性门（≥50%）过，但量级差异已由执行员如实登记（steady-state 窗口 ready 多为 1–2 条）。
j. 目录布局偏离 prereg §7.1（SAFE-A0 数据在 `raw/protection/SAFE-A0-*`，不在 `measurement-main/w03-*-a0/` 等）：数据存在且可定位，纯布局差异。

## 4. 结论

- 现有数据的统计与机制结论**全部可由原始数据复算**，与执行员摘要一致：两个 A/A PASS；R2 同 SHA 复现三块方向与 R2 归档一致；核心负控 R2-D2 **FAIL**（`LEGACY_D2_NEGATIVE_CONTROL_FAIL`，仅旧机制）；D2 正控与 R3 单条负控机制形状在诊断窗成立；机制存在性（enter ≥50% 减少）成立。
- 但本 run **不可声称 completion**：主门 B−A0 的 24-triplet 性能块未运行（prereg 冻结 blocked），`analysis/COMPLETION_GATE.json` 缺失，两个 enabled A0 基线块未执行且未记 NOT_RUN。任何 `W03-uring-4k-d16` 的 B 结论在本轮只能是 `CONTINUATION_` 诊断口径，不得写成预注册门通过。
