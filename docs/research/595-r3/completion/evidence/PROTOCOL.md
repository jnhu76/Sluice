# #595 R3 续跑 — 协议、实际执行与偏差记录（PROTOCOL）

| 字段 | 值 |
|---|---|
| 角色 | R3 完成轮裁决与交付员 |
| completionRoot | `/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z` |
| baseSha | `0a22a79195f1b913145fcd443496e7f70b08cdcf`（主树 HEAD，clean） |
| R2 锚点 | `271b81bfc9d64eecc0266fe7066b9040525dd710` |
| 冻结预注册 | `evidence/PREREGISTERED_R3_COMPLETION.md`，sha256 `1a9c1e12c33a2f50e3bfac957c2ab25592d77e3ffae808dd7f92f08e16af3ab4`（本 session 实测） |
| 冻结输入 | `semanticsReady=false`（prereg §0.2/§14.1；全 run 未翻转：§14.3 要求的独立 verifier 回执不存在，`A0_AND_SUFFIX_RECEIPT.md` 与 completion 树内 `correctness/` 均缺失） |
| 最终 patch | `evidence/r3-completion.patch` sha256 `2596f204035f9695db47184b20ad8ece7b2168c54d8589b2060782aa7959bbc0`；`git apply --reverse --check` exit 0（本 session 复验） |
| 本文件职责 | 汇总冻结协议、实际命令、偏差、无效轮次、仪器分离与持锁记录 |

---

## 1. 冻结协议（要点，权威以 prereg 为准）

1. **臂**（prereg §4）：A0/A1/B 共用 R3 研究二进制 `bench_w03_external_loop` sha256
   `0ebfb7f0c204579fe2cc9e8d7e88cee2de9112e25c0cfb11e8502e59fc4fe2d1`；
   A_R2/B_R2 共用 R2 二进制 `bench_w02_pipeline` sha256
   `32beabcd891801eb65dab1d6e2df72aa0b89a98a2c16a114d398527cb821589a`。
2. **配对**（§6）：两臂块 24 对 AB/BA 交替；A/A 偶 m1,m2 / 奇 m2,m1；三臂块 24 triplet
   种子 5950（本轮未执行）。每 invocation `WARMUP=1 MEASURED=5 STATS=0`、`taskset -c 2`、
   warm tmpfs `/tmp/sluice-595-r3c`、每 rep 20,000 ops（64 KiB case 2,000）、exit 0 硬门。
3. **统计**（§11）：pair-relative 差；bootstrap 10,000 / seed 5950 / percentile 95% CI；
   A/A 半宽门 CPU≤5pp、吞吐≤3pp、p99≤3pp；负控/保护单侧 95% 上界 5/5/5/3 + Holm(α=0.05)。
4. **主门**（§9.2）：W03 d16 `B−A0` 点估计 ≤−10% 且 CI95 上界 ≤−5%，吞吐下降 ≤3%，
   p95/p99 ≤5%，加机制对账（§9.3）与资源门（§10）全过才 `GO_MECHANISM`。
5. **enabled/blocked**（§14.1）：`semanticsReady=false` 下只允许 R2 同 SHA 复现与安全
   IMMEDIATE（A0/A_R2）块；全部新 B/A1 块 `BLOCKED_SEMANTICS_NOT_READY`；`PRODUCT-604`
   `BLOCKED_PRODUCT_NO_604`。翻转需 §14.3 独立 verifier 回执。
6. **非追溯**（§13）：本轮任何结论只能带 `CONTINUATION_` 标签；原 R3 的 NOT_RUN 不改写；
   R2 历史数值只作对照不复用。

## 2. 实际执行的命令与块

### 2.1 计时块（性能轮；全部持 `MEASUREMENT.lock`）

5 个两臂块 × 48 invocation = **240 个 invocation**，全部 exit=0、attempt=1、oracle 通过
（独立复核见 `measurement-analysis/REVIEW_REPORT.md` §1-#3）：

| 块 | 目录 | 命令（模板） | 对数 |
|---|---|---|---|
| `AA-W03-D16` | `raw/calibration/AA-W03-D16`（镜像 `evidence/measurement-main/aa/aa-w03-d16`） | `python3 -I harness/measure_pairs.py --phase AA-W03-D16 --phase-dir … --lock-file /home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock --cpu 2 --cpus 2,12 --warmup 1 --measured 5 --stats 0 --expected-ops 20000 --tmp-dir /tmp/sluice-595-r3c --case W03-uring-4k-d16 --pairs 24 --binary …/bench_w03_external_loop --expect-sha 0ebfb7f0… --arms AA --arm-var SLUICE_595_R3_ARM --arm-a A0 --arm-b A0` | 24 |
| `AA-W02-D2` | `raw/calibration/AA-W02-D2`（镜像 `evidence/measurement-legacy/aa-w02-d2`） | 同模板，R2 二进制 `32beabcd…`、case `W02-uring-4k-d2`、`SLUICE_BENCH_595_DEPTHS=1`、`SLUICE_595_ARM` A/A | 24 |
| `R2-D2`（核心负控） | `raw/calibration/R2-D2`（镜像 `evidence/measurement-legacy/w02-d2`） | 同模板，arms AB（A_R2/B_R2 同 SHA） | 24 |
| `PROT-W02-D3` | `raw/protection/PROT-W02-D3`（镜像 `…/protection/raw/PROT-W02-D3`） | 同模板，arms AB | 24 |
| `PROT-W02-D16` | `raw/protection/PROT-W02-D16`（镜像 `…/protection/raw/PROT-W02-D16`） | 同模板，arms AB | 24 |

分析：`python3 -I harness/analyze_pairs.py --phase-dir <phase> --arms <AA|AB>`；脚本 sha256
见 `harness.json` / `measurement-main/aa/harness.json`（`measure_pairs.py 7ad40542…`、
`analyze_pairs.py a91395d3…`）；合成自测 `ANALYSIS-SELFTEST/selftest.log` 8/8 PASS。

### 2.2 安全 A0/诊断块（SAFE-A0 族，descriptive/机制，不进主门）

`raw/protection/SAFE-A0-*`：placement（cpu2/cpu8/SMT 2,12/xphys 2,8 各 8 次）、
`SAFE-A0-W03-64K-D16`（3 invocation）、`SAFE-A0-W03-THREADPOOL-{d4,d16,d64}`（各 3）、
`SAFE-A0-W02-SAT-d16`（3）、`DIAG-*`（enter strace、R2 A/B enter、SAT A0）。
静态资源审计 `raw/protection/RESOURCE-STATIC-AUDIT/audit.txt`、
`RESOURCE-SYSCALL-A0-D16/syscalls.txt`。

### 2.3 机制诊断窗（带仪器，prereg §12.1 短诊断车道）

`raw/mechanism/`（会话 11:53:16Z–12:00:21Z + phase2 12:01:27Z–12:01:38Z，持锁）：
bpftrace enter 追踪（9 run：d16/d2pos/d2serial × A0/A1/B）、PMU（root u+k，每臂 1 run）、
CPU 分桶（3 run/臂）、malloc uprobe、strace 资源核对。脚本 `raw/mechanism/scripts/`。
sudo 密码仅经 stdin，未落盘。

### 2.4 R2 语义/来源核验

`SLUICE_595_ARM=B ./uring_595_bounded_progress_test` → `all checks passed`（exit 0，
`measurement-legacy/build/` + `diag-r2/logs/bounded_progress_test.log`）；
`raw/calibration/diag-r2/`（strace 3×/臂，`results_reparsed.jsonl` 为准）。

### 2.5 本交付 session 的复核命令（只读 + MUTANTS 会话）

- `git -C /home/jnhu/Source/Sluice status --porcelain` → 0 行；`rev-parse HEAD` → `0a22a791…`。
- R3 worktree：HEAD `0a22a791…`、15 脏行、`git diff HEAD` sha256 `c1a31a30…`（与 prereg §0.3 一致）；
  五个公开安装头与 `docs/explicit-io-contract.md` 相对 baseSha diff 为 0 行。
- `sha256sum -c /home/jnhu/sluice-595-e5/r3/evidence/MANIFEST.sha256` → **225 OK / 21 FAILED**
  （21 项全部是前轮记录的 worktree 源/二进制条目；completion patch 移动了这些文件，属预期，
  详见 §5 偏差 D-11）；`correctness/MANIFEST.sha256` → 33/33 OK。
- `git apply --reverse --check evidence/r3-completion.patch` → exit 0；
  `r3-final.patch`（前轮）reverse-check 现为 exit 1（worktree 已前进到 completion 态，预期）。
- 远端只读：`gh issue view 595`（OPEN，updatedAt 2026-11-04T16:59:52Z）、
  `gh issue view 604`（OPEN，无 `READY_FOR_595`）、`gh pr view 625`（OPEN）。
- R1/R2 原始证据：`/home/jnhu/sluice-595-e5/evidence` 0 个文件晚于 2026-11-05T00:00Z；
  `/home/jnhu/sluice-595-e5/r3/evidence` 0 个文件晚于本 completionRoot 起点 08:38:18Z。
- MUTANTS 会话（见 `MUTANTS.md` 与 `raw/mutants/session.log`）：持锁 12:43:02Z–12:43:39Z
  （base+mutants）与 12:43:50Z–12:43:56Z（干净 builddir 强制重建 + 复原配置 + post-state）。

## 3. 仪器分离（prereg §12.1 三车道）

| 车道 | 内容 | 进入配对统计 |
|---|---|---|
| 性能轮 | §2.1 的 5 块：仅 bench + `/usr/bin/time -v` 等价 rusage + `/proc` 采样；无 strace/perf/bpftrace/valgrind | 是（仅这 240 invocation） |
| 短机制诊断轮 | bpftrace enter/PMU/分桶/malloc uprobe/strace 计数、D2 正控/负控机制形状、SAFE-A0/DIAG | 否（RESULTS 中单列为诊断口径） |
| 正确性/fault 轮 | 26 场景确定性测试、3 变异体、fault/research oracle、`shutdown_lifecycle_test`（BUILD_COMPLETION §8.3/§8.6 + 本轮 MUTANTS 复跑） | 否 |

PMU/bpftrace/strace 的 CPU/墙钟数字在任何情况下不得写成性能结论；bpftrace 每次追踪
21 probes、9/9 run 0 lost events、逐事件计数 == map 计数（`MECHANISM_SUMMARY.md` §0）。

## 4. 持锁记录（`/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`，inode 6941075）

| 会话 | 获取 (UTC) | 释放 (UTC) | 内容 | 记录 |
|---|---|---|---|---|
| 校准+R2 计时 | 11:03:31Z（等待 0s，fuser 51956 51965） | 11:08:21Z | R2 构建、3×48 计时 invocation、R2 strace 诊断 | `raw/calibration/lock-log.jsonl`、`measurement-main/aa/LOCK_RECEIPT.md` |
| 排他性探针 | 11:12:45Z | 11:12:49Z | 第二 open file description `flock -n` 失败 → 锁语义有效 | `raw/calibration/bucket-probe/logs/lock-exclusivity.txt` |
| PROT-W02-D3+D16 | 11:22:02Z | 11:24:33Z | 两个 R2 保护块 | `raw/protection/lock-logs/` |
| SAFE-A0 | 11:25:33Z | 11:27:24Z | SAFE-A0/DIAG 系列 | 同上 |
| DIAG-enter | 11:29:47Z | 11:29:54Z | enter 短诊断 | 同上 |
| SAT-stats / DIAG-SAT-A0 | 11:31:10Z / 11:32:04Z | 即时 | 单 invocation 统计轮 | 同上 |
| 机制窗（失败会话） | 11:52:01Z | 11:52:01Z | 脚本 bug 秒级失败，无测量 | `raw/mechanism/lock-log.jsonl` |
| 机制窗主会话 | 11:53:16Z | 12:00:21Z | 9 run enter 追踪 + PMU + 分桶 + alloc | 同上 |
| 机制窗 phase2 | 12:01:08Z–12:01:10Z / 12:01:27Z–12:01:38Z | — | 补充 run | 同上 |
| MUTANTS（本交付） | 12:43:02Z / 12:43:50Z | 12:43:39Z / 12:43:56Z | base-green + 3 变异 + 强制重建 | `raw/mutants/session.log` |

无锁运行的构建/计时/tracing：**无**。`measurement-product/HOLD.md` 的核查是纯只读
（未获取锁，也未做任何测量），不构成偏离。

## 5. 偏差登记（全部保留原始回执，不改写 prereg）

| # | 偏差 | 影响 | 处置/来源 |
|---|---|---|---|
| D-1 | `analysis/COMPLETION_GATE.json` 缺失 | 按 prereg §7.2 本 run 只能发布 partial，不能声称 completion | `REVIEW_REPORT.md` §3-a；VERDICT 据此写 `INCOMPLETE_BLOCKED` |
| D-2 | 预注册把 `MECH-*`（A1/B）列为 blocked，但机制窗按交付 ask 以**诊断口径**执行了 d16/d2pos/d2serial 的 A0/A1/B run | 不解锁任何 prereg 门；这些数据不进 §6 统计；结果单列"诊断" | `MECHANISM_SUMMARY.md` §0/§7 |
| D-3 | d2pos 正控重复数：prereg §5.5 要求 3 次独立 invocation/臂 + strace；实际 1 invocation × 3 相位（bpftrace） | 相位粒度 3/3 全部 `to_submit=2`；判定只能写"相位粒度 PASS" | `REVIEW_REPORT.md` §3-d；`blocks.json` |
| D-4 | 64 KiB case id：实际 `W03-uring-64k-d16-r3d16-64k` ≠ prereg `W03-uring-64k-d16` | 数据合规（3×5 rep×2,000 ops，enter 6,000 全 to_submit=1，不合批符合预期），但 id 偏离已披露 | `SUMMARY.json` limitations |
| D-5 | ThreadPool 字面门 `io_uring_enter=0`：实测每 invocation 恰 1 次 enter（statx 可用性探针，workload 相位纯 submit=0） | 该块本为 BLOCKED（B 侧），不构成门翻转；作 limitation | `REVIEW_REPORT.md` §3-f |
| D-6 | `raw/calibration/{AA-W03-D16,AA-W02-D2,R2-D2}` 缺 `attempts.jsonl`（prereg §7.1） | 240 条记录全部 attempt=1、无替换，无数据损失；账面偏离 | `REVIEW_REPORT.md` §3-b |
| D-7 | 两个 enabledBlocks 从未执行且未记 NOT_RUN：`SAFE-A0-W03-D4-D16-D64` 的 d4/d64 4k A0 基线与 `SAFE-A0-W02-D16` | W03 d4/d64 与 W02(R3) d16 无任何 A0 数据；VERDICT 列入 NOT_RUN | `REVIEW_REPORT.md` §3-c |
| D-8 | 目录布局偏离：SAFE-A0 数据在 `raw/protection/SAFE-A0-*`，不在 prereg §7.1 的 `measurement-main/w03-*-a0/` | 数据可定位，纯布局差异 | `REVIEW_REPORT.md` §3-j |
| D-9 | R2 臂无 bench stdout arm 读回（R2 bench 不打印 `research arm=`）；PROV-7 以 env+成员命名+行为差异佐证 | 来源门弱化（非无效） | `REVIEW_REPORT.md` §3-g |
| D-10 | AA-W03-D16 均值偏移 −0.82%/−0.87%（CI 不含 0） | prereg §8.2 只设半宽门，非失败；登记 | `REVIEW_REPORT.md` §3-h |
| D-11 | 前轮 `r3/evidence/MANIFEST.sha256` 现为 225/246 OK：21 项 worktree 源/二进制条目因 completion patch 前进而 FAILED | 预期（本 session 实测）；前轮证据归档条目仍全 OK；当前 worktree 以 `r3-completion.patch` reverse-check exit 0 绑定 | 本 session 复核（§2.5） |
| D-12 | 机制量级与 census 预测不符（观测 enter −51.29% vs 预测 91.5–93.3%） | 机制存在性门（≥50%）过；量级差异如实登记，不得引用 census 预测为实测 | `MECHANISM_SUMMARY.md` §2；`READY_EXPOSURE_COMPLETION.md` §4 |
| D-13 | diag-r2 首次解析正则不接受 strace `-f` 裸 pid 前缀，`results.jsonl` trace 字段全 0；`reparse_diag.py` 修正重读，双记录保留 | 计数以 `results_reparsed.jsonl` 为准；失败尝试保留 | `measurement-legacy/diag-r2/RECEIPT.md` |
| D-14 | 机制窗 11:52:01Z 会话因脚本 bug 秒级失败 | 无测量发生，锁即时释放 | `raw/mechanism/lock-log.jsonl` |
| D-15 | MUTANTS 第一次会话因 `set -e` 在预期 rc=1 处中断（mut1 输出已捕获） | 同锁窗口内立即 redo 全部 3 个变异；无源码/二进制变化 | `raw/mutants/session.log` NOTE |
| D-16 | BUILD_COMPLETION §1 命令块注释中的 patch hash `fc1857ce…` 为 stale 笔误（表头与实测均为 `2596f204…`） | 文档缺陷，不作任何门槛放宽依据 | `PREREGISTERED_R3_COMPLETION.md` §0.2-5 |

## 6. 无效轮次与替换

- 计时块：**0 个无效 invocation、0 次替换**（240/240 exit=0、attempt=1、oracle/ops/SHA/读回全过）。
- 无效尝试（保留原始输出，未删除）：
  1. diag-r2 第一次解析（D-13，`results.jsonl` trace=0）。
  2. 机制窗 11:52:01Z 失败会话（D-14）。
  3. MUTANTS 第一次 set -e 中断（D-15）。
- A/A 修复额度：**未使用**（`repairUsed=false`，两个 A/A 一次通过）。

## 7. 本轮明确未运行的冻结块

`MAIN-W03-D16-A1/B`、`MAIN-W03-D16-B-vs-A0/A1`、`CROSS-W03-D4/D64-A1-B`、
`MECH-ENTER-CQE-RECON`（正式口径）、`PROT-W03-D4/D16/D64-B`、`PROT-64K-D16-B`、
`PROT-THREADPOOL-B`、`PROT-SMT-2-12-B`、`PROT-XPHYS-2-8-B`、`PROT-W02-D16-B (R3)`、
`PROT-SATURATION-B`、`RESOURCE-B` → `BLOCKED_SEMANTICS_NOT_READY` / `NO_W02_R3_ARM`；
`PRODUCT-604` → `BLOCKED_PRODUCT_NO_604`。解除条件见 prereg §14.2/§14.3 与 §10.3。
