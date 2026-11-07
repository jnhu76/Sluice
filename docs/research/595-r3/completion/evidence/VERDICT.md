# #595 R3 续跑 — 完成轮裁决（VERDICT）

| 字段 | 值 |
|---|---|
| 角色 | R3 完成轮裁决与交付员 |
| 裁决时间（UTC） | 2026-11-05T12:51:29Z（本 session `date -u` 实测） |
| EXECUTION_STATUS | **INCOMPLETE_BLOCKED** |
| MECHANISM_PERFORMANCE_VERDICT | **INCONCLUSIVE** |
| PRODUCT_VERDICT | **HOLD_NO_REAL_APPLICATION** |

## 1. 三问回答

1. **执行状态**：部分执行。已实测：两个 A/A 校准块（24/24 对，全过）、三个 R2 同 SHA
   配对块（R2-D2 核心负控 **FAIL**＝复现旧 B 的 D2 退化；PROT-W02-D3/D16 **PASS**）、
   机制诊断窗（enter/PMU/分桶/资源，9+3+9 run）、SAFE-A0/DIAG 族、MUTANTS 三态复跑与
   同源码强制重建。未执行：预注册冻结的全部 R3 B/A1 正式块（`semanticsReady=false`
   全程未翻转）、`analysis/COMPLETION_GATE.json`（缺失）、两个 enabled A0 基线块
   （W03 4k d4/d64、SAFE-A0-W02-D16）。按 prereg §7.2/§11.3，本 run 只能发布 partial，
   不得声称 completion → **INCOMPLETE_BLOCKED**。
2. **机制性能**：主门（W03 d16 `B−A0` 点 ≤−10% 且上界 ≤−5% 等）**一次都没有测**
   （BLOCKED）。诊断方向为正（enter −51.29%、PMU cycles −14.1%、分桶 cpu_s −18.1%），但
   预注册明令带仪器数据不得进入门判定；A/A 通过（噪声底可用）不等于主门可判。
   → **INCONCLUSIVE**。既不写 POLICY_PERF_SUPPORTED（无正式测量），也不写 NO_NET_BENEFIT
   （CPU 门没有"测到不过"的证据）。
3. **产品**：#604 `OPEN`、body 与全部 comment 均无 `READY_FOR_595`（本 session 与
   `measurement-product/HOLD.md` 双重核查）；无可准入真实应用、无产品 harness 入口、无
   产品预注册；W-03 是合成代表 trace，明文不能替代产品门 → **HOLD_NO_REAL_APPLICATION**。
   即使机制门未来全过，产品仍需 #604 handoff + 独立产品预注册。

## 2. 支撑要点（数字全部可溯到 raw/ 或 evidence/，见 `RESULTS.md`）

- **R2-D2 核心负控 FAIL 是旧机制结论**：B_R2−A_R2 tput 上界 +3.889%>3%、p95 +5.906%>5%、
  p99 +9.067%>5%；与 R2 归档（+3.73/+3.58/+4.96/+7.07）方向一致、CI 重叠；strace 显示
  两臂 enter 完全相同（无合发），退化归因于门链+flush 后移（只登记，不下更强结论）。
  不得外推到 R3 B。
- **D3/D16 的 R2 收益（−7.3%/−28.1% CPU）也是旧机制同 SHA 复现**，不构成 R3 B 的任何
  证据；跨 SHA 只并列不相减。
- **R3 机制存在性**（诊断口径）：B 29,225 vs A0 60,000 纯 submit enter（−51.29% ≥50% 门），
  `to_submit>1`=4,399、max=8≤cap；cap/suffix 对账闭合（accepted 20,000 = 1,355×8 + 9,160
  单；untouched==suffix_single==9,160；flush==entry==10,515；defer=0）。量级与 census 预测
  （91.5–93.3%）不符，已如实登记。
- **正确性**：26 场景确定性 oracle GREEN；3 个命名变异体 RED（17F / 9F / rc=134
  fail-fast）；同一最终源码在干净 builddir 强制重建得到逐位相同二进制（`c54a9222…`）
  并 GREEN（`MUTANTS.md`、`raw/mutants/`）。但这不替代 §14.3 独立 verifier 语义回执
  （`A0_AND_SUFFIX_RECEIPT.md`、completion `correctness/` 仍缺失）。
- **资源（诊断）**：RSS +0.92%（门 +2% 内）；无新增线程/timer/队列/每请求分配
  （60,000 ops 仅 +6 次一次性 malloc）；valgrind 斜率子门 NOT_RUN（bench 无旋钮）。
- **A1 成本信号（诊断，供未来归因）**：PMU instr +5.0%、csw +15.1%、RSS +64 KB、
  malloc +6；enter 与 A0 相同。若未来 B−A1 有收益而 B−A0 未过门，必须按 prereg §9.3
  单列 adapter/入口簿记成本，不得用 A1 修正 B−A0。

## 3. 下一阶段触发条件（trigger）

1. **解锁机制测量**：按 prereg §14.3 出具独立 verifier 回执（patch sha `2596f204…`、
   二进制 SHA、26 场景 + 3 变异 + 回归回执、`A0_AND_SUFFIX_RECEIPT.md`（含 d16 suffix
   计数与 enter 计数）、OPEN-1.4/OPEN-1.14 处置、明确"允许 B/A1 计时"），将
   `semanticsReady` 翻为 true；随后从 A/A 重新开始，完整跑 §14.2 激活块
   （MAIN/CROSS/MECH/PROT/RESOURCE，24 triplet/块）。
2. **补齐本轮欠账**：`analysis/COMPLETION_GATE.json`；两个未执行的 enabled A0 基线块或
   在下一版预注册中显式移除。
3. **产品门**：#604 给出 `READY_FOR_595` handoff（真实 consumer/代表 trace + 冻结
   oracle/预算/baseline）后，另写独立产品预注册再测；在此之前产品恒为 HOLD。

## 4. 复核清单（本 session 实测）

| 复核项 | 结果 |
|---|---|
| 主工作树 `git status --porcelain` | **0 行**（HEAD `0a22a791…`） |
| 公开安装头（5 个 `include/sluice/async/*.hpp`）vs baseSha | **0 行 diff** |
| `docs/explicit-io-contract.md`（v2-r1）vs baseSha | **0 行 diff** |
| 远端（只读） | Issue #595 OPEN（updatedAt 2026-11-04T16:59:52Z，未变）；#604 OPEN、无 `READY_FOR_595`；PR #625 OPEN |
| R1/R2 原始证据 | `/home/jnhu/sluice-595-e5/evidence` 0 个文件晚于 2026-11-05T00:00Z；`r3/evidence` 0 个文件晚于本 completionRoot 起点 |
| 前轮清单 | `r3/evidence/MANIFEST.sha256` 225/246 OK（21 项为前轮 worktree 源/二进制条目，因 completion patch 前进而 FAILED，预期；证据归档条目全 OK）；`correctness/MANIFEST.sha256` 33/33 OK |
| patch 绑定 | `r3-completion.patch` sha256 `2596f204…`，reverse-check exit 0；worktree diff sha `c1a31a30…` 与 prereg 一致 |
| 未提交 | 主树/分支零提交、零 push；worktree 脏文件未提交（维持研究态） |

## 5. 可粘贴评论区块

见 `issue-comment-r3-completion.md`（内容与本节区块相同）。

---

```text
BEGIN_SLUICE_595_R3_COMPLETION
EXECUTION_STATUS: INCOMPLETE_BLOCKED
E5_IDENTITY_AND_BASE_SHA: 原生 Intel Xeon E5-2666 v3（20 逻辑核/SMT on），Fedora 44，kernel 7.2.11-200.fc44.x86_64，g++ 16.2.1，xmake v3.1.2+HEAD.5d21e9d，liburing 2.13；baseSha=0a22a79195f1b913145fcd443496e7f70b08cdcf（主树 clean，0 行 porcelain）；R2 锚点 271b81bf；R3 worktree HEAD=baseSha、15 脏行、git diff HEAD sha256 c1a31a30…；completion patch sha256 2596f204…（reverse-check exit 0）；R3 二进制 bench_w03_external_loop 0ebfb7f0…；R2 二进制 bench_w02_pipeline 32beabcd…（271b81bf+7be9d014… 干净树，diff sha b006dbbf…）
PREVIOUS_WORK_REUSED: 复用前轮 READY_EXPOSURE 普查（sha 0d0365c2…）与 census 分布/口径声明（case 形状未变，enter 绝对数由本轮重测）；前轮 r3-final.patch 只作历史（reverse-check 现为 exit 1，worktree 已前进到 completion 态）；R2 归档数值仅作方向对照、未复用进任何 CI/门；前轮 correctness 结论不绑定 completion 二进制（completion 已重建并由本轮 26 场景 + MUTANTS 重证）
PROSPECTIVE_SCOPE_AMENDMENT: 前瞻预注册 PREREGISTERED_R3_COMPLETION.md 写于 2026-11-05T10:47:31Z（任何新增计时之前，frozenBeforeTiming=true）；主形状由 W02-D2 迁移到 W03-uring-4k-d16；产品门由 benchmark 主 shape 改为 #604 真实应用准入；semanticsReady=false 为冻结输入（§14 只开 R2 同 SHA 复现 + 安全 IMMEDIATE 块）；非追溯条款：一切结论只带 CONTINUATION_ 标签
OPEN1_AND_SEMANTICS_RECEIPTS: OPEN1_RECEIPTS.md 已写 17 项定义/义务；BUILD_COMPLETION §9 给出实现回执（1.1/1.14 PARTIAL、1.16 BLOCKED、其余 PROVED/PARTIAL），但 semanticsOk=true 是实现者自检；独立 verifier 回执不存在（A0_AND_SUFFIX_RECEIPT.md 缺失、completion 树 correctness/ 目录缺失），semanticsReady 全程未翻转
A0_ORIGINAL_LOOP_RECEIPT: A0 已按冻结定义复原（bench_w03_external_loop.cpp:321-323 仅非 A0 臂构造 adapter；:409-431 循环内逐条构造 ReadOp 并立即 ctx.submit_read；无 ready span/预扫描/组缓冲）；strace d2 A0 6×to_submit=1、d16 60,000×1；PROV-6 源码形状门通过；但指定回执文件不存在，A0 未进入任何 B−A0 决策门（该门本就 BLOCKED）
SUFFIX_AND_CAP_RECEIPT: cap=8/suffix 逐条 span=1/不二次成组：d16 B bench 计数 groups=1355、prefix_max=8、accepted=20000、untouched=9160==suffix_single_calls、flush_attempts==entry_calls==10515、gate_stops=0、defer=0；to_submit 直方图无 >8（8:4395 次）；context_cap_suffix 场景 16→accepted=8/stop=cap/untouched=8，suffix 8 条逐条结算；gate_defer 证明同状态 span=1/span=2 最终接受同一集合
PREREGISTERED_FILE_SHA: 1a9c1e12c33a2f50e3bfac957c2ab25592d77e3ffae808dd7f92f08e16af3ab4（evidence/PREREGISTERED_R3_COMPLETION.md）
AA_VALID_PAIRS: AA-W03-D16 24/24 对 PASS（CPU/吞吐/p99 半宽 0.797/0.821/2.039pp ≤ 5/3/3pp）；AA-W02-D2 24/24 对 PASS（0.862/0.853/1.420pp）；repairUsed=false；5 个计时块共 240/240 invocation 有效（exit=0、attempt=1、20k ops、oracle 过）
D2_IMMEDIATE_VS_OLD_B: R2-D2 同 SHA（32beabcd…）B_R2−A_R2 24 对：cpu +3.435% [+2.673,+4.160]、单侧上界 +4.065%（≤5 过）；tput +3.298% 上界 +3.889%（>3 FAIL）；p95 +5.233% 上界 +5.906%（>5 FAIL）；p99 +7.052% [+4.844,+9.514] 上界 +9.067%（>5 FAIL）→ LEGACY_D2_NEGATIVE_CONTROL_FAIL；与 R2 归档（+3.73/+3.58/+4.96/+7.07）方向一致 CI 重叠；strace：A/B enter 完全相同（60,004，直方图 {1:180012}，无合发）——退化是门链+flush 后移，非批量缺失；仅旧机制结论，不得外推 R3 B
D2_NEW_B_VS_IMMEDIATE: 性能对照 NOT_RUN（BLOCKED_SEMANTICS_NOT_READY）；机制形状负控（d2serial，诊断口径）PASS：三臂 enter/SQE/CQE/ret/直方图逐项相同（60,000×(1,0,0)+4 探针）、io_uring_defer=0、eventfd2=4、setup=4、register=12 相同、timer=0——B 单条补发不合发、不 defer、无 residue
D2_TWO_READY: d2pos 正控（相位粒度）PASS：A0/A1 每 invocation 6 次纯 submit（每相位 2×to_submit=1）；B 3 次纯 submit 全部 to_submit=2（每相位 1 次）；group=1、accepted=2、prefix_max=2、untouched=0、flush=1；6 data SQE/CQE(4096)、byte oracle ok、exit=0。偏离：1 invocation×3 相位，非预注册的 3 次独立 invocation（已登记）
W03_D16_A0_A1_B: 正式 24-triplet 三臂门 NOT_RUN（BLOCKED_SEMANTICS_NOT_READY；measurement-main/ 仅 aa/）。诊断窗（带仪器，禁入门判定）：纯 submit enter A0=60,000、A1=60,000、B=29,225（−51.29%，≥50% 机制存在性门过；census 预测 91.5–93.3% 未达，量级差异已登记）；PMU（root u+k，单次）：cycles A1 −0.2%/B −14.1%，instr A1 +5.0%/B −8.1%，csw A1 +15.1%/B −26.2%；分桶 bench cpu_s A0 0.0700/A1 0.0652/B 0.0573 s（B vs A0 −18.1%，诊断）；RSS 5204/5268/5252 KB（B +0.92%）
W03_D4_D64: 全部 NOT_RUN——B 侧 BLOCKED_SEMANTICS_NOT_READY；enabled 的 A0 基线 4k d4/d64 从未执行且未记 NOT_RUN（登记为执行缺口）；64 KiB：A0-only descriptive 3 invocation（6,000 纯 submit 全 to_submit=1，64 KiB 静态不入批；CPU 12,351.55 ns/op），B 侧 BLOCKED
OTHER_NEGATIVE_AND_RESOURCE_GATES: PROT-W02-D3 PASS（cpu −7.327% 上界 −6.673%；p95 −12.795%；p99 −11.511%；tput −7.975%；Holm 全 reject）；PROT-W02-D16 PASS（cpu −28.111% 上界 −27.613%；p95 −32.590%；p99 −32.400%；tput −39.241%）——均为 R2 同 SHA 旧机制复现，非 R3 B；R3 侧全部保护块 BLOCKED；RESOURCE 正式门 NOT_ESTABLISHED（诊断：RSS +0.92%≤+2%、线程 3/3/3、timer 0、队列 4/4/4、malloc +6 次一次性、无每请求分配；valgrind 斜率子门 NOT_RUN——bench 无 512/1024 旋钮）；placement A0-only 代表检查：SMT{2,12} CPU +145.5%、xphys{2,8} +166.6%（A0 自身大幅变差，无 B 推断）；threadpool A0-only：workload 相位纯 submit=0（每 invocation 1 次探针 enter）；saturation NOT_ESTABLISHED（A0 诊断 9,600×1 enter）
CPU_ACCOUNTING_COVERAGE: 主指标 = 每 measured rep 的 CLOCK_PROCESS_CPUTIME_ID 差值（whole-executor：本内核含 io-wq worker，分桶证实 iou-* 线程在进程 task 内且不相加二次计数）；次指标 wait4 rusage 符号一致性已核（R2-D2 ru_secondary +3.462% vs 主 +3.435% 同号）；驱动/提交者拆分实测（main 13.7→11.0 ticks、driver 0.8→0.0、io-wq 7.3→6.0）；未归因残差 +5.2…+6.7 ticks（10ms tick 粒度）按最不利端登记（上界 ru_total A0 0.275s/B 0.222s）；IRQ 为整机上界不进臂差；PMU 仅 root u+k 单次带仪器；入口→enter 纳秒级延迟不可观测（冻结二进制 stripped）
ACTUAL_MEASUREMENT_TABLES: 详见 evidence/RESULTS.md（§1 A/A 表；§2.1 R2-D2 负控表；§2.2/§2.3 D3/D16 表；§4 机制/PMU/分桶表；§5 保护族；§6 内存/线程/timer/分配表；§8 空表检查）；独立统计复核 evidence/measurement-analysis/REVIEW_REPORT.md（与执行员数值 1e-9 内一致）
MECHANISM_PERFORMANCE_VERDICT: INCONCLUSIVE —— 主门 W03 d16 B−A0 一次未测（BLOCKED），无点估计/上界可判；诊断方向为正但被预注册排除在门外；A/A 通过只说明噪声底可用；不满足 POLICY_PERF_SUPPORTED（未测）也不满足 NO_NET_BENEFIT（无"测到但 CPU 门不过"的证据）
REAL_APPLICATION_STATUS: 无已准入真实应用：#604 OPEN（updatedAt 2026-11-03T20:37:04Z）、body/comments 0 次 READY_FOR_595；无 consumer 定义/oracle/预算/baseline；无产品 harness 入口；apps/* 均为参考应用；W-03 是合成代表 trace，明文不得替代产品门
PRODUCT_VERDICT: HOLD_NO_REAL_APPLICATION —— 解除条件：#604 READY_FOR_595 handoff（真实 consumer + 冻结 oracle/预算/baseline）+ 按预注册 §10.3 另写独立产品预注册后再测；即便机制门未来全过，产品结论也不随 GO_MECHANISM 自动改变
NOT_RUN_OR_BLOCKED_WITH_REASON: MAIN-W03-D16-A1/B 与 B−A0/B−A1 主门（BLOCKED_SEMANTICS_NOT_READY：无 §14.3 独立 verifier 回执）；CROSS-W03-D4/D64-A1-B（同上）；MECH 正式口径与 d2pos 3 次独立 invocation（同上/已按 1×3 相位执行并登记）；PROT-W03-D4/D16/D64-B、PROT-64K-B、PROT-THREADPOOL-B、PROT-SMT/XPHYS-B（同上）；PROT-W02-D16-B(R3) 与 PROT-SATURATION-B（NO_W02_R3_ARM：R3 树无 W-02 臂/D2 case，patch 不触碰 bench_w02_pipeline.cpp）；RESOURCE-B 正式门（同上）；valgrind 分配斜率（bench 无 request-count 旋钮）；SAFE-A0-W03-4k-d4/d64 与 SAFE-A0-W02-D16（enabled 但未执行，执行缺口）；analysis/COMPLETION_GATE.json（缺失，按 §7.2 不得声称 completion）；#604 真实应用（无 handoff）；TSan/ASan；no-lib profile 完整语义套件；observer-armed 批量交错；data+cancel 混合 partial submit 脚本化
REPRODUCTION_AND_EVIDENCE_PATH: /home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/evidence/{PROTOCOL,RESULTS,READY_EXPOSURE_COMPLETION,MUTANTS,VERDICT}.md 与 issue-comment-r3-completion.md；原始数据 raw/{baseline,calibration,mechanism,protection,mutants}；清单 evidence/MANIFEST.sha256（sha256sum -c 通过；文件数与清单 sha256 见 MANIFEST.sha256.hash sidecar）；harness/ 与 analysis/ 脚本 sha256 见 measurement-main/aa/harness.json
END_SLUICE_595_R3_COMPLETION
```
