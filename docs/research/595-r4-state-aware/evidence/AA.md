# R4 A/A 精度核验（#595，正式性能数据前置）

- 轮次：R4（预注册 `/home/jnhu/sluice-595-e5/r4/PREREGISTERED.md`，frozenBeforeTiming=true）
- 执行：A/A 测量员，会话 2026-11-06T01:54:21Z–02:01:35Z（持锁串行，单会话）
- 对象：worktree `f3b25329bc8a64a201961c426a58adacfb8fdf08`（porcelain=0）的冻结二进制
  - `bench_w03_external_loop` sha256 `f029607fc1a657efe0ba96203115f27a90d598f12c274e9360b6029a5e019009`（= 预注册 §1.2，块首/块末/逐 invocation 三重核验一致）
  - `bench_w02_pipeline` sha256 `daee6b6c1ab618a29d8e1afc120f8da5e478756b2def7c77c712f54a6a4e1931`（同上）
- 机制复用：runner/分析器拷贝自 R3 完成轮
  `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/harness/measure_pairs.py`、
  `analyze_pairs.py`（唯一适配：`arm_readback` 增加对 W02 二进制无 `research cases` 后缀的
  `research arm=…` 行的匹配，有效性判据不变）；新增 `drift_check.py`（漂移诊断，纯
  Python，Spearman/OLS + 固定 seed 5951 置换检验 10000 次）。会话脚本
  `harness/run_aa.sh`。
- 分析器已在 r3 往轮数据上复算验证：对
  `r3-completion…/raw/calibration/AA-W03-D16` 复算出 0.797/0.821/2.039pp，与该轮
  CALIBRATION_SUMMARY §0 一致。

## 1. 结论（预注册 §6/§7.4 门）

24 matched blocks ×（A0 vs A0），块单位配对差，bootstrap 10000 次 seed 5950，
半宽 =（CI95_hi − CI95_lo）/2。三块**全部 PASS**；A/A 均值偏移不构成失败（§6）。

| 块 | case | 指标 | mean | 95% CI | 半宽 | 目标 | 判定 |
|---|---|---|---|---|---|---|---|
| AA-W03-D16（主形状） | `=W03-uring-4k-d16` | CPU/op | +0.183% | [−0.838%, +1.211%] | **1.025pp** | 5pp | PASS |
| | | 吞吐 | +0.120% | [−0.909%, +1.148%] | **1.029pp** | 3pp | PASS |
| | | p99 | +0.973% | [−1.120%, +3.074%] | **2.097pp** | 3pp | PASS |
| | | p95（报告项） | +0.170% | [−1.000%, +1.316%] | 1.158pp | —（参照 §5.3 功效界 5pp） | 无 A/A 门 |
| AA-W03-D2SERIAL | `=W03-uring-4k-d2-r3d2serial`（`SLUICE_595_R3_CASES=1`） | CPU/op | −0.488% | [−0.997%, +0.015%] | **0.506pp** | 5pp | PASS |
| | | 吞吐 | −0.502% | [−1.021%, +0.005%] | **0.513pp** | 3pp | PASS |
| | | p99 | +1.442% | [−1.050%, +4.368%] | **2.709pp** | 3pp | PASS（余量最薄） |
| | | p95（报告项） | −0.217% | [−0.970%, +0.620%] | 0.795pp | — | 无 A/A 门 |
| AA-W02-D2 | `=W02-uring-4k-d2`（`SLUICE_BENCH_595_DEPTHS=1`） | CPU/op | −0.114% | [−0.678%, +0.425%] | **0.552pp** | 5pp | PASS |
| | | 吞吐 | −0.131% | [−0.704%, +0.413%] | **0.558pp** | 3pp | PASS |
| | | p99 | −0.326% | [−1.576%, +1.077%] | **1.326pp** | 3pp | PASS |
| | | p95（报告项） | −0.344% | [−0.839%, +0.142%] | 0.491pp | — | 无 A/A 门 |

绝对量级（m1/m2 均值，实测重算）：W03-D16 CPU/op 3166.1/3170.9 ns、吞吐
316,125/315,650 ops/s；W03-D2SERIAL CPU/op 12,966.3/12,901.9 ns、吞吐
77,020/77,400 ops/s；W02-D2 CPU/op 4728.2/4722.3 ns、吞吐 211,316/211,571 ops/s。

对照往轮（r3 二进制，仅噪声背景，不替代本轮）：R3 完成轮 AA-W03-D16
0.797/0.821/2.039pp；本轮 1.025/1.029/2.097pp——同量级。

## 2. 设计与执行

- 顺序规则＝预注册 §5.2（A/A 专用，无 RNG）：偶数块 m1→m2，奇数块 m2→m1；
  24 块内两成员各占先手 12 次。ask 所述"预注册 seed 随机化"对 A/A 块在预注册中的
  实际条款即该确定性交替（seed 随机化条款 §5.2 只用于 4 臂格排列），本轮按预注册执行。
- 每 invocation：`taskset -c 2`、WARMUP=1/MEASURED=5（每 rep 20,000 有效读）、
  `SLUICE_BENCH_STATS=0`、臂 `SLUICE_595_R3_ARM=A0`（读回校验 A0 全部一致）、
  warm tmpfs fixture（`SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-r4aa`，64MiB，
  fixture sha256 块前/块后不变）。
- 有效性（预注册 §10.2）：三块共 144 invocation 全部 attempt=1 有效；
  `attempts.jsonl` 三块均为 0 行；exit=0、oracle ok、ops=20000×5、
  二进制 sha 块首/逐 invocation/块末三重一致、`SLUICE_595_R4_DIAG_LINES` 未设置。
- 块执行顺序：SMOKE(1 对，非计时证据) → AA-W03-D16 → AA-W03-D2SERIAL → AA-W02-D2
  （§11.1 顺序）。

## 3. LOCK_RECEIPT（预注册 §14）

- 锁文件 `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`（inode 6941075，size=0，
  未向锁文件写内容）。
- 获取 2026-11-06T01:54:21Z（等待 0s）；释放 2026-11-06T02:01:35Z；持锁 7m14s，
  会话内全部计时/分析串行，无并行编译/压缩/归档/其他实验。
- 第二 open file description 排他性检查：持锁期间 `flock -n` 立即失败
  （rc=1，输出为空）——锁语义有效。`fuser -v` 记录持锁 pid 20920/20931（本会话 bash）。
- 释放后复核：`flock -n` 立即重取成功（`post-release probe: reacquired`）。
- 原始收据：`raw/aa/lock-log.jsonl`（acquired+released 两条完整记录）。

## 4. 环境快照

- governor：cpu2/cpu12 全程 `schedutil`（三块 header/block_end 的
  cpufreq_start/cpufreq_end 全部为 schedutil，无频率策略变化）。
- affinity：bench 全部 `taskset -c 2`；runner 绑 cpu0。块内每 invocation 前的
  cpu2/cpu12 busy 采样最大值均为 0.000（48×3 份快照无缺失、无外来 CPU-bearing 进程）。
- load（1/5/15min）：会话起点 [0.15, 0.10, 0.09]（AA-W03-D16 header）→ 会话终点
  [1.90, 1.73, 0.91]（AA-W02-D2 block_end）。load 上升来自会话自身的串行 bench
  进程（loadavg 1min 滞后累积），块开始前 cpu2/12 busy=0 证明无外来负载；
  按预注册 §10.1 块开始后背景负载只记录不判无效。
- `/tmp`：可用 30.9GiB 全程稳定（> 1GiB 门）；`df -h /tmp`：tmpfs 32G 用 332M。
- kernel：`7.2.11-200.fc44.x86_64`（block header host_kernel 字段）；python 3.14.8。
- 内核 io-wq worker 与线程计数：逐 invocation sampler 已记录（results.jsonl
  `buckets` 字段），本轮未发现异常；正式资源门（§8）在后续正式轮判定，非本核验范围。

## 5. 漂移检查（ask 项 5；`drift_check.py`，seed 5951，置换 10000 次）

对每块 24 对：差值序列（d vs 块序号）Spearman ρ 与 OLS 斜率（置换 p）；
m1/m2 绝对值序列（共模漂移探测）；每 invocation wall 时间（执行顺序）。

| 块 | 差值趋势（d vs 序号） | 绝对值趋势 | wall 趋势 |
|---|---|---|---|
| AA-W03-D16 | cpu/p95/p99/tput ρ∈[−0.11,+0.01]，p≥0.61，全部 none | 全部 none | ρ=+0.007 p=0.97 none |
| AA-W03-D2SERIAL | 全部 none（p99 ρ=+0.326 p=0.12 最大） | m1_p99 ρ=−0.484 p=0.017 suggestive；其余 none | ρ=−0.095 none |
| AA-W02-D2 | 全部 none（p99 ρ=+0.309 p=0.14 最大） | m1/m2 CPU、吞吐 ρ≈±0.50 p≈0.012–0.015 suggestive（同向） | ρ=−0.459 p=0.0011 marked（变快） |

判定：**未发现严重非平稳**；两处如实披露：

1. AA-W02-D2 存在同向（共模）变快趋势：后半块两成员 CPU/op 均下降（m1 −1.18%、
   m2 −0.86%），吞吐均上升（+1.19%/+0.87%），每 invocation wall 斜率 −0.55ms。
   因偶奇交替使漂移对 m1/m2 对称进入，**配对差值序列平稳**（d 的 ρ≤0.10，p≥0.65），
   半宽门不受影响；量级（~1%）也远小于主门效应界。此为环境缓变（热化/频率行为），
   随机顺序不能"修复"共模漂移，但它不污染配对差——正式 A/B 轮沿用同一配对结构承接。
2. AA-W03-D2SERIAL 的 m1_p99 单侧 suggestive（后半 −1.42% 而 m2 +1.06%，非共模），
   属 21 次检验中的单序列波动（期望 ~1 个 p<0.05 假阳性）；差值 p99 无显著趋势，
   但该块 p99 半宽 2.709pp 是三块中最接近 3pp 门者——正式轮若依赖该形状 p99 精度
   需注意余量。

## 6. 缺陷与诚实披露

- `run_aa.sh` 会话边界 `pre-state.json`/`post-state.json` 的 heredoc 漏写
  `import sys`，两文件未落盘（会话日志留有 traceback）。会话边界状态未丢失：
  §4 的 load/tmp/governor 全部取自块级 `header.json`/`block_end.json`（测量时实拍）。
  脚本已修复（供复跑），本轮数据不受影响、不重跑、不补造"会话前"快照。
- `/proc/locks` 在本平台未列出该 flock 条目（lock-log 的 `proc_locks_holders` 为空，
  与 R3 完成轮同一现象）；排他性由第二 fd `flock -n` 检查（rc=1）证明。
- 本轮无外部网络检索；未推送、未写 GitHub；主工作树与他人 worktree 未动
  （仅读取）；`/tmp/sluice-595-r4aa` fixture 目录为本会话创建。

## 7. 原始数据与复现

- 原始数据：`/home/jnhu/sluice-595-e5/r4/raw/aa/{AA-W03-D16,AA-W03-D2SERIAL,AA-W02-D2}/`
  （header.json、results.jsonl、summary.json、drift.json、jsonl/ 逐 rep 记录、
  logs/ 逐 invocation 输出、analysis.log、drift.log）+ `lock-log.jsonl` + `logs/session.log`。
- 复现：`bash /home/jnhu/sluice-595-e5/r4/harness/run_aa.sh`（需独占整机环境）；
  分析重算：`python3 -I harness/analyze_pairs.py --phase-dir raw/aa/<块> --arms AA`、
  `python3 -I harness/drift_check.py --phase-dir raw/aa/<块>`。
- 脚本 sha256（本轮执行版本）：
  `measure_pairs.py` `b5f2b7a63bd2747c4f64e05cc9334cd8281508d146ba5aca5d50457e173c4593`
  （arm_readback 两格式适配版；r3 原版 `7ad40542…`）、
  `analyze_pairs.py` `a91395d31e31cb4252f95d7725f54a06e11e5389cc00411f0b678fde239be7a3`
  （与 r3 原版逐位一致）、
  `drift_check.py` `8c8de5760d3fb449dd04b614c455234a1e78f09270aed80479de4de346737dca`、
  `run_aa.sh` `6e688d545a32288a65b0fad703c5b4032f0ab945071a586388647d50b64daf94`
  （pre/post-state 修复后版本；执行时版本仅缺两处 `sys` import，影响见 §6）。

## 8. 判定树落点（预注册 §13.1）

三 A/A 块全部 PASS（无修复额度消耗）→ 允许进入 §11.1 第 2 步及以后的 A/B、
负控、正控、保护与 VALUE 对比；正式测量会话另行持锁。
