# PR #594 (key-path-593) 研究结果长期价值独立评估

评估人:研究价值评估员(动态工作流子代理)。日期:2026-10-29。
对象:`2714aa65fe4837d30f5fa3fcdb27db38c23b7136`("research(593): anchor final block
to PR #594",分支 `research/593-key-path`,即 OPEN 未合并的 PR #594 的树)。
性质:纯审计与非破坏性验证,未修改任何产品代码,未删除任何分支/Worktree/证据目录。

## 0. 方法与命令(全部在本会话实际执行)

| 步骤 | 命令 | 结果 |
|---|---|---|
| 枚举 PR #594 树文件 | `git ls-tree -r 2714aa65 --name-only` 过滤两个目录 | 56 个文件(docs 15 + bench 41);receipts/val-01 |
| 确认 master 不含这些路径 | `git ls-tree -r HEAD --name-only \| grep key-path-593` | 空(master 无任何 key-path-593 文件);receipts/val-07 |
| 读 #595 | `gh issue view 595 --repo jnhu76/Sluice --json ...`(默认 view 因 Projects-classic GraphQL 弃用报错,改用 --json 成功) | 全文;receipts/val-02 |
| 读 #593(R0–R4 定义出处) | `gh issue view 593 --repo jnhu76/Sluice --json ...` | 全文;receipts/val-03 |
| PR #594 状态 | `gh pr view 594 --json ...` | OPEN,未合并(mergedAt=null);receipts/val-04 |
| 校验和复核 | 在 worktree 内 `sha256sum -c patches/SHA256SUMS` 与 `harness/SHA256SUMS` | 全部 OK;k4b-final.diff = `4e9018d8…eef9`,与 k4 文档 §6 所引一致;receipts/val-05 |
| K4-b 反事实测试定位 | worktree(2714aa65)内 `git grep` patches/;`git ls-tree origin/master tests/` | 测试仅在研究补丁内,master 无;receipts/val-06 |
| 阅读文档 | 在专属验证 worktree `/tmp/sluice-val-k594-retention`(2714aa65,detached)内 Read | 见 §1 引用 |

初验更正:首次 `sha256sum -c` 在父目录执行而报 FAILED open(manifest 列裸文件名);
从 manifest 所在目录重跑后全部 OK。以 val-05 为准。

## 1. 库存(56 个文件)

- `docs/research/key-path-593/` 15 篇:README、execution-dag、critical-path-atlas、
  mechanism-ledger、k1/k2/k3/k4 四篇 lane 报告、counterfactuals、cross-path-interactions、
  opportunity-ranking、correctness-review、measurement-review、decision、evidence-index。
- `bench/research/key-path-593/` 41 个:README;patches/(14 个补丁 + SHA256SUMS);
  harness/(24 个脚本/归约器 + SHA256SUMS)。
- 原始证据 **不在** PR 内:1.33 GiB / 6,870 文件仅在 E5 主机
  `/home/jnhu/sluice-593-research/evidence`(evidence-index.md §7,
  `EVIDENCE_PRESERVATION: PARTIAL`;原始 JSONL/perf .data **未做哈希锚定**,单副本无备份)。

## 2. 长期值得保存的研究知识(不可从当前 master 重新导出)

1. **K1 负面/阈值依赖结果**(k1-completion-publication.md:3,decision.md §2):
   K1(a+b) 合法移除 3.098 次锁获取/op,W-03 d16 CPU −2.29%(15/15),但低于 4.79%
   战役带 → `NO_CHANGE_JUSTIFIED`(若否决该带则 `INCONCLUSIVE`);复活前提:K4 类
   产品变更后的新基线 + owed-scan 锁持有重设计;K1 无专属 mutant(丢失发布、
   publish_batch_ 双计、K1-b 假阴性均未突变验证,correctness-review.md §3)。
2. **K2-a ungated deferral = 机制上界、非可部署形态**(k2-submit-dispatch.md:3,
   counterfactuals.md §2):W-02 CPU −32.20/−37.66/−26.31%,但 D=1 CPU +14.12%、
   p50 ×14.1,W-03 d16 p99 +15.70%(通知 +51.6%),64 KiB p99 +6.77% →
   `ELISION_CANDIDATE_FOUND`。#595 §1 原文引用其全部数字。
3. **K3 负面结果**(k3-progress-wakeup.md:3,decision.md §2):进度机制在测量域
   休眠/被 1/D 摊薄(W-02 540,000 op 零 park、零用户信号;passes/op 1.34→0.030);
   最大信号融合臂 −1.00%(9/15,中位数在 A/A 带内);K3-a/K3-b 均
   `NO_CHANGE_JUSTIFIED`。#595 §1 明确:"K3's separate pass-tail fusion is a
   negative finding; do not reopen without new trigger"。
4. **K4-b depth/topology gate**(k4-sqe-cqe-batching.md §6–§7,counterfactuals.md §4):
   谓词 `submitter_drives_progress_locked_() && outstanding_summary()>1`;三臂演化
   (uniform → depth gate → topology+depth)每臂证伪前一门(depth gate 在 W-03 d4
   +34.46%);支持范围 **仅 D=4/16/64**,≈2/D enters 模式预测 D=2 无 enter 收益,
   D=2/D=3 未测;K4-a 无臂撤回,且 mutex 上界不覆盖 `cqe_seen` khead store(部分
   上界,已声明)。
5. **64 KiB p99 回归披露**(k4-sqe-cqe-batching.md:172、:213;decision.md:20):
   K4-b 64 KiB d16 p99 **+7.46%**、p95 +7.50%(p99 0/15 改善)而 CPU 仅 −2.99%
   → 材料级回归,产品化前必须 payload gate。#595 §1 原文引用("+7.46% / +7.50%")。
6. **"同一机制两个门"与非可加性**(README §4,decision.md §3,cross-path-interactions):
   K2-a 与 K4-b 是同一行代码(`src/async/uring_transport.cpp:87` 内联 flush)的两个门;
   组合 K4-b+K1 相对 K4-b 的 W-02 增量 +0.31%(无增益)→ `MECHANISM_ONLY_GAIN`,
   百分比不得相加,支持判定只记一次(记在 K4)。
7. **系统性缺口:form dependence**(decision.md §3.5):同一机制随
   topology × depth × payload 在 −28%…−36% / 中性 / 尾部回归之间翻转;产品形态需要
   同线程+深度+payload 三条件门,K4-b 只实现两个 → #595 P1 的直接依据。
8. **产品化前提清单**(decision.md §4 共 8 条;k4 §11 同构):payload gate、D=2/D=3
   验证、变体选择(K4-b vs K2-a 未决)、K1 组合决策、cancel/mixed-driver/deferred
   partial-submit/overflow/shutdown 均未测、#585 语义重定义、机制论证门、第二主机/
   真实设备复核 → #595 §6 回归控制矩阵的蓝本。
9. **未解释偏差记录**(measurement-review §9.5/§9.6,k4 §9):W-03 d4 +2.54%
   (低于带、在唯一实测 d4 A/A [−6.14,+6.51] 内、无归因);d1 −2.50% CI 越零、
   与 discrimination 轮 d1 +1.88% 符号不稳定 → 归为布局/时钟噪声,非机制收益。
10. **oracle 质量结论**(correctness-review.md §3,measurement-review §10 #41/42):
    `k4_residue_wake_test` 竞态(100 ms sleep、无 prepark 证明、任意 progress wake
    即通过);`k4_retry_residue_wake_test` 覆盖 inline-flush EAGAIN 而非 deferred
    多入口 pass-start retry → #595 §4"必须修复而非仅再披露"的两项。
11. **4.79% 带 provenance 撤回**(measurement-review §4):该值源自基线 A/A 最大漂移
    (W03-d4 wall),"pre-registered" 标签被撤回;K1/combo 判定随之显式阈值依赖。
    这是方法论诚实度的长期样本。
12. **测量边界**(README §8,measurement-review §6):仅 warm tmpfs 无设备 I/O;
    单主机/单内核/单拓扑;A/B 会话仅用户态 PMU,K4-b 内核侧归因继承自 K2 ungated
    0/1 record(继承而非实测);saturate 延迟未埋点(lat_n=0,+0.00% 是 reducer
    陷阱);worker 份额 32.5–40.4%(两条 0/1 record 不一致,取区间)。
13. **基线事实**(k4 §2,mechanism-ledger):基线 1.00007 enters/op、每 enter 恰
    1 SQE(所有 D)、CQE 侧已批量(11.76/28.88 CQEs/peek)——#595 §1"1.00007
    io_uring_enter per logical operation"的出处。

## 3. 可重新生成的临时测试状态(从已提交补丁+harness 可重建)

- 9 个 arm worktree 与全部构建产物(baseline/observe/k1-elision/k2-elision/K2-a/
  k3-fusion/k4-defer/K4-b/combo,HEAD 均 `bdfd0d8d`,仅在 E5 主机)——按 README §7
  三步(worktree + patch + xmake)可重建;补丁 round-trip 已验证(k4 §6:八文件
  字节一致;evidence-index 收据)。
- 11 个 mutant 构建与 mutant 日志(由补丁内 `SLUICE_*_MUTANT_*` 编译开关再生)。
- SLUICE_OBSERVE 二进制与 census JSON dump(observe-instrumentation.diff,835 行)。
- A/B 运行输出(case-summary.tsv、pair-results.txt/json):K2-a/K4-b/combo 可用已提交
  的 `harness/analyze_pairs.py`(bootstrap,seed 593)从 JSONL 重算;K1/K3 归约器
  已复制归档。**例外:K4 discrimination 轮归约器缺失**,其 pair-results.txt 无法从
  已提交脚本再生(bench README §2,measurement-review §9.13,已如实披露)。
- harness 脚本本身声明为"one-shot 实验溯源,非产品工具",硬编码研究工作区绝对路径,
  换机不可直接运行(bench README §2)——它们的价值是"审计与改编模板",不是可执行产品。
- 边界说明:仓外 1.33 GiB 原始 JSONL/perf 数据**不属于**"可再生"类——计时数据不可
  确定性复现,且未哈希锚定;#595 W0 明令"不得触碰或迁移"该目录,属 owner 保留状态。

## 4. 必答一:#595 是否依赖原始 K4-b 反事实测试与复现脚本?(k4bCounterfactualReuse)

**结论:是,直接且多处依赖;PR #594 是这些工件的唯一 in-repo 副本。**

- **反事实测试**:#595 §4 明文要求"Specific missing oracles from #593 that must be
  **repaired**, not merely re-disclosed":(1) `k4_residue_wake_test`(100 ms sleep、
  无 prepark 区分唤醒源);(2) `k4_retry_residue_wake_test`(仅覆盖 inline-flush
  EAGAIN,非 deferred 多入口 pass-start partial/retry)。这两个测试的源码只存在于
  研究补丁中:`patches/k4b-final.diff:391` 与 `:591`(另见 k4-defer-arm.patch:46、
  k4-defer-observe.patch:675、combo diff:617/817)。master 上不存在
  (receipts/val-06:origin/master tests/ 仅 `uring_owed_residue_test.cpp`)。
- **复现脚本/补丁**:#595 W0"inspect #594's research patches and reports";W2
  "**adapt the verified K4-b patch** without blindly importing its heuristics" →
  直接以 `patches/k4b-final.diff`(sha256 `4e9018d8…eef9`,本次已复核一致)为
  改编基线;W4 规定"same-host A/A + ≥15 interleaved AB/BA valid pairs"正是
  `pair-run-k4b.sh` + `analyze_pairs.py` 实现的协议;#595 §1 逐条引用由该 harness
  产出的 K2-a/K4-b 数字(−13.06/−28.07/−35.53%、0.50017/0.12577/0.03447、
  +7.46% 等)。
- **文档引用**:#595 以 `blob/research/593-key-path/...` 链接引用 k4-sqe-cqe-batching.md、
  decision.md、correctness-review.md——分支删除即链接失效。
- **归档责任判定**:key-path-593 全部 56 个文件不在 master(receipts/val-07),
  PR #594 OPEN 未合并(receipts/val-04)。**在 #594 合并(或内容以其他方式入 trunk)
  之前删除 `research/593-key-path` 分支,将唯一 in-repo 的 K4-b 补丁、两个反事实
  测试源码与复现 harness 一并销毁,直接违反"不能为减分支而丢失 #595 必需的反事实
  工具"**。复用注意三点(均已披露,不改变"必须保留"的结论):harness 为一次性溯源
  脚本、路径硬编码;K4 discrimination 归约器缺失;唤醒 oracle 必须按 #595 §4 修复
  而非原样复用。仓外原始证据由 owner 保留(#595 W0 禁止迁移),不影响本判定。

## 5. 必答二:缺失覆盖是否被如实披露?(missingCoverage)

**结论:是。四类缺口(D=2/D=3、Mixed Driver、Cancel、Shutdown/partial-submit/overflow)
在多份文档中反复、明确披露,未发现任何宣称已覆盖的表述。**

- **D=2/D=3 未测**:k4-sqe-cqe-batching.md:9-12(范围修正)、:158-161(2/D 模式
  预测 D=2 无收益)、§9 表行("no data / unmeasured")、§12;decision.md:20、:78-80、
  §4.2;README.md:103-104;counterfactuals.md:173;measurement-review.md §10 #1
  ("D≥2 over-claim … **Fixed** — scope is D=4/16/64; D=2/D=3 unmeasured")。
- **Mixed driver**:k4 §9 表行("no mixed-use load measured");counterfactuals.md
  §4 Q7("mixed-driver usage (unmeasured)");correctness-review.md §5 行 1
  (性能风险、非正确性);decision.md §4.5;measurement-review §10 结尾清单。
- **Cancel/control**:k4 §6 修正(cancel flush 不在 `issue_running_cancel_locked_`,
  延迟形态最多增加一个 pass 的取消延迟)、§9 表行、§10("a cancel storm"无收据);
  correctness-review.md §5 行 3;measurement-review §7(K1/K2/K4 三行 NOT
  executed/measured);README §8;decision.md §4.5。
- **Shutdown/deferred partial submit/overflow**:k4 §10("Not covered by any
  receipt: a deferred ledger with `size()>1` undergoing an rc<pending partial
  submit, overflow flush, or shutdown");correctness-review.md §5 行 4;§2 中
  "obligation mapping … not a deferral-shaped receipt"的自我否定;measurement-review
  §7(K3 行 "cancel-pending and shutdown **not load-measured**")与 §10 #28/29;
  decision.md §4.5 与 final block NEGATIVE_FINDINGS("cancel/partial-submit/
  early-reuse deferral load is unmeasured")。
- 此外 §10 结尾把"无法从归档解决"的发现整体列为 disclosed:raw-data 哈希、K4 归约器、
  cancel/partial-submit、D=2/D=3、mixed-driver、deferred retry 形态、竞态唤醒 oracle、
  4.79% 带预注册——披露完整度是该记录最强的部分。

## 6. 必答三:R0–R4 pilot 完成度判定(verdict593pilotSatisfied)

**结论:未完全满足(PARTIAL / NOT SATISFIED AS PILOT)。#594 是一份高质量、高度
诚实的 #593-brief 战役记录,但没有以 #593 议题自身的 R0–R4 门与收尾词汇完成试点
收口;记录中也从未主张这些门已满足(grep `R0|R1|…|METHOD_EARNED|REPLAY_NOT_ADMITTED`
在 docs/research/key-path-593/ 内零命中)。**

- **R0(源映射/接缝可行性,强制)**:映射实质完成(execution-dag.md 逐边 DAG、
  critical-path-atlas.md 分类)。但 Gate R0 要求的显式
  FULL/OBSERVE/REQUEST_REPLAY/COMPLETION_REPLAY 可行性矩阵在记录中不存在
  (grep 零命中);counterfactuals.md §0 的 BASELINE/OBSERVE/SHADOW/… 臂分类是
  另一维度(SHADOW 行声明"未构建")。→ 部分完成。
- **R1(中性语料+跨视图 oracle)**:未按议题规格实现——无版本化 trace 语料、无
  摘要锚定语料、无 fail-closed trace 校验器/再生证明;实际用的是 W-02/W-03 + G0
  checker + 逐 rep 64 MiB 字节 oracle(工作负载 oracle,非 trace 语料)。→ 未完成
  (以等效工作负载 oracle 替代)。
- **R2(observe/measure 隔离)**:实质满足——独立 SLUICE_OBSERVE 二进制(66 计数器),
  仪表开销披露(关 +0.56/+1.3/+1.5%,开 +14.65/+3.31/+15.35%),全量二进制为成本
  权威,mechanism-ledger 每 op 频率,非仪表原生参照。→ 完成。
- **R3(受控完成重放+反事实)**:反事实半边强完成——每 lane ≥2 竞争解释、预声明
  falsifier、11 份正式 RED→GREEN 突变收据(K1 缺口如实披露);C 模式(受控完成
  重放)未实现,且**未以议题要求的 `REPLAY_NOT_ADMITTED`/`INCONCLUSIVE` 等词汇
  显式裁决**(议题允许跳过 C 模式,但要求透明裁决)。→ 反事实完成,C 模式未透明收口。
- **R4(双独立评审+收尾)**:correctness-review.md(语义)+ measurement-review.md
  (测量/统计)两通道存在,42 项独立评审发现逐一处置(§10)。但 correctness-review
  §6 自我披露:"no separate independent reviewer signed off as an approver … the
  independent-review step of brief §14.6 is therefore **disclosed as not satisfied
  as an approval**"。R4 还要求回答"方法论是否暴露了 perf/flamegraph 之外的信息"——
  记录中无显式回答(grep 零命中);收尾词汇(METHOD_EARNED/PARTIAL_OBSERVATION_ONLY/
  NO_EXTRA_DIAGNOSTIC_VALUE/…)未使用;#593 议题至今 OPEN。→ 部分完成。

总评:以"试点完成度"论判 **不满足**;以"研究价值"论,该战役偏离试点设计
(trace/replay)走向基准 A/B 反事实战役,反而产出了 #595 直接可用的机制结论——
这一偏离本身未被记录为对 #593 试点问题的正式回答,属于诚实但未收口的偏差。

## 7. 结论

- 长期知识(§2)与 #595 依赖工件(K4-b 补丁+两测试+harness+四篇被引文档)必须随
  PR #594 入 trunk;**先合并/保全 #594,后清理分支**。
- 可再生状态(§3)无需归档:worktree、二进制、mutant 构建、census dump、运行输出
  均可由已提交补丁+harness 重建(K4 discrimination 归约器缺失为已披露例外)。
- 原始 1.33 GiB 证据为 owner 保留的仓外单副本,#595 明令不迁移;记录已自评
  `EVIDENCE_PRESERVATION: PARTIAL`。
- 本报告全部证据见 `receipts/val-01…val-07` 与文中行号引用;验证 worktree
  `/tmp/sluice-val-k594-retention` 留待工作流脚本统一清理(按要求未自行移除)。
