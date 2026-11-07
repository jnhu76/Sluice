# RESUME — #595 恢复研究入口（不读聊天记录也能继续）

暂停时点：2026-11-07。结论快照见 `DECISION_SNAPSHOT.md`（同目录）。

## 1. 当前坐标

| 对象 | 是什么 | 怎么检出 |
|---|---|---|
| 产品 | master @ `271b81bf`（A0/IMMEDIATE，无研究代码） | `git checkout master` |
| 最佳研究基线（cleanup 后） | `research-595-r5` @ `b191bb89`（= f3b25329 + C1 `c55165a2` + C2 `b191bb89`，18 场景逐字节等价 PROVEN） | `git checkout research-595-r5` |
| R4 原始原型 | `research-595-r4` @ `f3b25329`（等价于 `research-595-r4-state-aware` 内 `evidence/impl-f3b25329.patch`） | `git checkout research-595-r4` |
| R3 原型（batch 语义） | 分支 `research-595-r3` @ `0a22a791` + patch 链：`r3-completion.patch` → `delta_w02.patch`/`delta_w03.patch`（全在 PR #626/#627 分支 `docs/research/595-r3/`） | 见 §4 命令 3 |
| R2 原型（defer 归因三变体） | master + 本目录 `worktree-state/causal-r2_*` 的 patch + untracked 头文件 | 见 §4 命令 3 |
| 诊断仪器 v2（TSan 干净） | `research-595-r5rm-diag` @ `492086ac`；cleanup 镜像 `research-595-r5rm-diag-cleanup` @ `3fd6476a` | `git checkout research-595-r5rm-diag` |
| 最后有效证据 | R5 repair round：PR #630 分支 `docs/research/595-r5-repair-round/`（FINAL-REPORT.yaml / RESULTS.md / CORRECTIONS.md） | `git checkout research/595-r5-phase-a` |

## 2. 环境重建（L3）

记录于 `environment/env.txt`。要点：

- Fedora fc44，kernel 7.2.11-200.fc44.x86_64，Xeon E5-2666 v3（20 CPU，
  schedutil），btrfs /home；perf_event_paranoid=2（无 PMU）。
- 编译：gcc 16.2.1（或 clang 22.1.8）+ xmake 3.1.2 + liburing 2.13。
- 分析：python3.14（3.14.x），numpy 2.5.2 + matplotlib 3.11.1 装在
  **user-site**（`pip install --user numpy==2.5.2 matplotlib==3.11.1`；
  figure 脚本不要用 `python3 -I`——`-I` 会跳过 user-site，R4 复建时
  明确记录过这一点）。
- 统计口径（各轮一致）：bootstrap 10000 次、seed 5950、配对块为推断
  单位、单侧 95% 界做门、Holm 按格校正——分析器自带，勿重写。
- 机器级缺口（重跑前检查）：CPU 放置/亲和性脚本在各轮 harness；双锁
  纪律 `/home/jnhu/sluice-595-e5/MEASUREMENT.lock` 的配方在
  LOCK_RECEIPT 文档；内核升级后不承诺同数字。

## 3. 重跑数据（原始数据未归档，按用户 2026-11-07 决定）

每轮"再跑"入口都在其分支内（harness + scripts + PREREGISTERED 冻结
参数 + build 配方）：

| 轮 | 重跑入口（分支内路径） |
|---|---|
| R1/E5 | `bench/research/uring-submit-policy/round-e5-20261104/`（analyze.py、pair_runner.py、PREREGISTERED.md、patch.diff） |
| R2 | `docs/research/595-causal-r2/`（PROTOCOL.md、analysis/depth-analysis/tools/run_*.sh、counterfactuals/tools）+ 本目录 worktree-state 原型 |
| R3 | `docs/research/595-r3/{completion,continuation}/harness/`（run_session_flip.sh、measure_triplets.py、analyze_triplets.py、recompute_stats.py、rebuild_all.sh） |
| R4 | `docs/research/595-r4-state-aware/`（scripts/matrix_driver.py、matrix_analysis.py、make_figures.py、PREREGISTERED.md、impl patch） |
| R5 repair | `docs/research/595-r5-repair-round/`（scripts/run_round.py、run_diag_round.py、build_arms.sh、analyze_round.py、recover_f5_position.py、measure3.py、PREREGISTERED.md） |
| flow audit | `docs/research/595-flow-audit/scripts/`（verify_inherited_evidence.py、cpu_scope_recalc.py、test_offline_checks.py） |

流程：检出分支 → 按 BASELINE.md 核对基线 SHA → build_arms/构建配方
重建二进制 → 先跑 A/A → 再按 PREREGISTERED 跑正式块。**新代码施工前
必须重做正确性验证**（R4 correctness 轮的 oracle+变异体配方在
`595-r4-state-aware/evidence/correctness/`）。

## 4. 三个命令入口

设 `$B` = bundle 路径
`/home/jnhu/sluice-research-archive/595/pause-20261107/source/sluice-595-research.bundle`
（或直接用 GitHub 远端）。

1. **只查看结果**：
   `git clone --no-checkout $B /tmp/sluice-595-view && git -C /tmp/sluice-595-view show research/595-r5-phase-a:docs/research/595-r5-repair-round/RESULTS.md`
2. **恢复研究 worktree（最佳基线）**：
   `git clone $B ~/Source/sluice-resume && git -C ~/Source/sluice-resume checkout research-595-r5`
3. **重建 R2/R3 原型 worktree**：
   `git clone $B ~/Source/sluice-r2 && git -C ~/Source/sluice-r2 checkout 271b81bf && git -C ~/Source/sluice-r2 apply docs/research/595-pause/worktree-state/causal-r2_worktree/tracked-modifications.patch && cp -r docs/research/595-pause/worktree-state/causal-r2_worktree/untracked/. .`
   （R3：checkout `0a22a791` 后依次 apply PR#627 分支内
   `docs/research/595-r3/continuation/evidence/r3-worktree-full-diff.patch`，
   或按 completion→delta 顺序；sha256 在同目录 MANIFEST 内。）

## 5. 阅读入口（≤5，仅整理已有引用；全文出处与章节见
`docs/research/595-r5-phase-a/analysis/literature-matrix.md` 的 11 行矩阵）

1. **IX**（OSDI'14，§5 批处理=执行边界）vs Sluice（组只是提交单元）——
   R4 失败假设 1（完成顺序耦合）的来源。
2. **ZygOS**（SOSP'17，§3.3/§6.2 微任务下批处理与尾延迟的双面性）——
   解释 R4 同轮 CPU 收益与 p99 代价并存的机制框架。
3. **Shinjuku**（NSDI'19，§3.4 SQ 量子抢占的前置条件"队列非空"）——
   S/SNP 失败的判据：收缩呈现不是抢占。
4. **Shenango**（NSDI'19，§4.1.1 同一 item 跨采样持续排队信号）——
   替代 completion_pressure 的正确信号类设计（flow-audit 认可的下一个
   可测假设方向）。
5. **NAPI**（kernel 文档，budget 耗尽即续轮协议 / drain 期有界 repoll）——
   R 的 cap 重入语义与 SNP 零在飞时刻的反例解释。

## 6. 未来启动实验前只需新增的工作

（不需要无条件重做历史研究；继承项见 flow-audit NEXT_EXPERIMENT §8）

- 明确的**增量假设**与**单一可删除干预**（现无：NO_COUNTERFACTUAL_SELECTED）。
- 语义等价证明（oracle 逐字节等价 + 计数器冻结点决策）。
- 固定公平口径（含 CPU 口径声明：rusage vs io-wq）。
- 预注册（门限/家族/seed 先冻结）+ fresh A/A。
- 若用尾归因：非扰动仪器（现仪器 on/off 扰动超窗，仅 lead 级）。
