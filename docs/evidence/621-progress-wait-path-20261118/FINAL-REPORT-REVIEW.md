# FINAL-REPORT-REVIEW — #621 Progress/Wait 终稿与回写稿独立复核

| 字段 | 值 |
|---|---|
| 复核对象 | `FINAL-REPORT.md`（1107 行）、`ISSUE-621-COMMENT.md`（181 行）、`p3/P3-ADMISSION-R1.md`、`p2/P2-COST-MAP-R1.md`、`p1/AA-CALIBRATION.md`、`p1/AA-CALIBRATION-2.md`、`p4/W3-ROWLEVEL.md`、`p4/W3-ROWLEVEL-REVIEW.md`、`p4/M53-OBLIGATIONS.md` |
| 复核基线 | `5d94f233967c3aef993eb19ba9e3252c532b0951`（本会话 `git rev-parse HEAD` 与 `git rev-parse origin/master` 实测一致） |
| 日期 | 2026-11-18 |
| 性质 | 只读独立复核。未修改任何被复核文件；本文件是本轮唯一写入。 |
| 判词 | **不可按现状发布**：存在 **2 个 high** 级问题（§C.6 把 M-62 的 4.13% 阶段容器写成“上界”；§K.5 把 O-2 写成“唯一剩余阻断”并把 O-1/O-3–O-12 定性为“不是新的技术实验”），以及 9 个 medium、5 个 low 级问题。修正 high（及建议修正 medium）后可发布。 |

---

## 0. 结论摘要

1. **报告的证据链总体扎实。** 本轮独立复算了 P1/P2 A/A 与 P2 R1 成本地图，结果与报告 §F/§G 的发布值在 4 位小数内一致；`git` 基线、祖先、`#634`/`#635` 在 BASE 中的状态、M-53 三个既有测试的 7/7、20/20、21/21 均实测通过。报告对“未运行/未重建/未真实消费者 profile”的披露是充分的。
2. **但报告有两处会改变读者判断的内部不一致：**
   - `FINAL-REPORT.md:408` 在“不可相加矩阵”里写 **“M-62 的上界是 4.13%（仍只是容器）”**。这与 `P3-ADMISSION-R1.md:58,275` 明确修正的“4.13% 是 publication 阶段容器，不是候选上界”直接矛盾，也与报告自己的 `D.2`/`E.4`/`K` 表述矛盾。这是典型的“阶段容器当候选上界”，不能用于准入。
   - `FINAL-REPORT.md:1006-1009` 与 `ISSUE-621-COMMENT.md:165-168` 把 **O-2 称为“唯一剩余阻断”**，并把 O-1/O-3–O-12 说成“只是状态确认/归档，不产生新的实验路径”。但 `K.1` 自己列出 W3 行级、M-53 O3/d64 两个未验证设计门，`D.2`/`I.1` 又把 W3/W4 列为 `ADMIT_TO_DESIGN`，而 O-4/O-5/O-6/O-8 明确要求行级归因、d64 A/A、隔离格、`-scope64` profile 等实验。该“唯一阻断”结论不能用于关闭 #621。
3. **K 的负结论（没有候选达到 A/B 准入，不建议继续投入 #621 的 A/B 队列）与证据一致；但 K 的“完成”措辞（`RESEARCH_COMPLETED`、“唯一剩余阻断”）与证据不一致。** 建议改为“当前没有可进入 A/B 的候选；W3/M-53 的设计门仍开放，需 owner 决定是否资助；O-2 是唯一涉及通知拓扑变更的语义授权点，但不是唯一的未完成设计门”。
4. **回写稿没有凭据、token、明文密码，也未引用两个“全程不得触碰”的目录；但它依赖同目录的 `FINAL-REPORT.md`，而该目录在 BASE 中仍是未跟踪的。** 若评论与报告一并提交/归档，这一依赖可以成立；否则 `ISSUE-621-COMMENT.md:176` 的“不依赖任何本地未跟踪工作副本”不成立。另有若干记录值（如频率门禁 PASS）没有在评论中标注 R/I，存在被读成本轮实测的风险。

---

## 1. 本轮实际执行的检查（V）

| # | 检查 | 命令（实际执行） | 结果 |
|---:|---|---|---|
| 1 | Git 基线 | `git rev-parse HEAD`；`git rev-parse origin/master`；`git branch --show-current`；`git worktree list --porcelain`；`git stash list`；`git diff --stat`；`git diff --cached --stat` | HEAD=`5d94f233…`；origin/master 同；单 worktree；stash 空；tracked tree 干净 |
| 2 | 祖先核验 | `git merge-base --is-ancestor 3f22d2d0/5d94f233/98825793/0e50612c HEAD`；`git cat-file -t 0e50612c` | 前三者 ANCESTOR；`0e50612c` 是 commit 但 **NOT_ANCESTOR** |
| 3 | 证据目录 | `git cat-file -e HEAD:docs/evidence/621-threadpool-owed-count-20261112`；`HEAD:docs/evidence/621-progress-drain-single-read-20261112` | `#634` 证据目录 **在 HEAD**；`#635` 证据目录 **不在 HEAD** |
| 4 | P1 A/A 复算 | `cd docs/evidence/621-progress-wait-path-20261118 && python3 -I tools/recompute_independent.py --aa-root p1/aa --json-out /tmp/review-aa1.json` | exit 0；C1 QUIET、C2/C5 0 对、C3 ENVIRONMENT_BLOCKED、C4 QUALIFIED、C6 ORDER_SENSITIVE；C1 CPU SD 0.7858/hw 0.3650/MDE80 0.5889；C3 cycles 1.9909/0.9492/1.4920；C4 CPU 3.0188/1.4021/2.2623；C6 cycles 2.8762/1.3680/2.1555 |
| 5 | P2 A/A 复算 | `python3 -I tools/recompute_independent.py --aa-root p1/aa2 --json-out /tmp/review-aa2.json` | exit 0；C2b NOISY、C3b ENVIRONMENT_BLOCKED、C5b QUIET、C7 ENVIRONMENT_BLOCKED、C8 NOISY；C2b cycles 5.1440/2.4007/3.8551；C3b cycles 1.3188/0.6208/0.9883；C5b CPU 0.9643/0.4575/0.7227；C7 cycles 1.8890/0.8930/tHW 1.0066/MDE80 1.4157；C8 CPU 4.9376/2.3233/3.7003 |
| 6 | P2 R1 成本地图复算 | `python3 -I p2/profile/analysis/recompute_p2_r1.py --json-out /tmp/review-p2r1.json` | exit 0；P-B `__poll` 6,129/24,585=24.9298%、poll boundary 2,251=9.1560%、progress/wait 6,168=25.0885%；P-E 5,500/34,736=15.8337%、1,520=4.3759%、8,461=24.3580%；P-D publication 627=4.1299%、terminal 530=3.4910%；W3 family 502/59/2,606/3,405/4,539；M-53 34/47；M-42 `lock addq` 与 0.1259–0.1325% 一致 |
| 7 | 独立 t/MME | `python3 -I /tmp/final_tcheck2.py` | exit 0；`t_.975(15)=2.131450`、`t_.80(15)=0.866245`；C7 tHW16=1.0066、tHW17=0.9712、tHW20=0.8841、tHW58=0.4967；0.406%→172/336、0.4%→178/346、0.285%→347/678 |
| 8 | M-53 既有测试 | `./build/linux/x86_64/release/uring_pending_control_parity_test`；`.../backend_contract_conformance_uring_test`；`.../uring_request_core_integration_test` | 分别 **7/7、20/20、21/21**，均 exit 0；三个二进制 mtime 均为 2026-11-12 21:42 |
| 9 | 源码坐标 | `sed`/`grep` 读取 `progress_source.hpp:70-80,536-546`、`uring_backend.cpp:145-162,270-280`、`uring_transport.cpp:185-192,218-224,526-540,548-560,675-682`、`threadpool_backend.cpp:430-438`、`detail/threadpool_backend.hpp:254-262`、`uring_backend.hpp:320-326`、`uring_test_seams.hpp:72-78`、`docs/explicit-io-contract.md:824-836,905-914`、`bench/bench_w02_pipeline.cpp:48-56,265-275,380-388` | 与报告/证据文档引用的坐标一致；`submit_flushes_` 在 `uring_backend.hpp:324`，唯一读者在 `uring_test_seams.hpp:75-76`；`syscall_count_` 在 `threadpool_backend.cpp:434` 与 `detail/threadpool_backend.hpp:258`；`eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC)` 在 `progress_source.hpp:75`；PROG-02 允许 condvar 在 `docs/explicit-io-contract.md:907-912`；P-B 20 s 有界等待在 `bench_w02_pipeline.cpp:52` |
| 10 | 工作树源码 | `git diff --stat HEAD -- src include tests` | 无输出（exit 0） |

**本复核未执行**（见 §7）：任何 `xmake f/build/test`、clean rebuild、TLC、ASAN/TSAN、完整矩阵、新 perf/bench/A/A/A-B、P4 `analyze_w3.py` 重跑、H1 patch 应用、`#637` 归档 `e2f41acb` 读取、`#634`/`#635` 历史性能重跑。

---

## 2. 关键结论与证据路径（A–K）

| # | 关键结论 | 报告位置 | 证据路径 | 复核结果 |
|---:|---|---|---|---|
| 1 | HEAD/`origin/master`=`5d94f233…`；单 worktree；stash 空；tracked tree 干净 | §A.1 | 本会话 `git` 命令 | **PASS（V）** |
| 2 | `3f22d2d0`/`5d94f233`/`98825793` 是 HEAD 祖先；`0e50612c` 不是 | §A.5 | 本会话 `git merge-base` | **PASS（V）** |
| 3 | `#634` 证据目录在 BASE；`#635` 证据目录不在 BASE，只在 `0e50612c` | §A.5/§K.2 | 本会话 `git cat-file -e`；`CAMPAIGN-LEDGER.md:172-173` | **PASS（V）** |
| 4 | P1/P4 构建回执（81/81、DWARF 二进制、`.text` 相同） | §A.4 | `p1/build-manifest.md`、`p4/rowlevel-build-manifest.md` | **PASS（R；本轮未重建）** |
| 5 | 频率门禁 PASS；PMU/eBPF/介质能力 | §B.1–B.4 | `CAMPAIGN-LEDGER.md:337-424`；本会话只读 sysfs/procfs 复核 | **PASS（R + V；未复跑 turbostat/perf）** |
| 6 | 五形态 PMU 总账、阶段 self、leaf top | §C.1–C.4 | `P2-COST-MAP-R1.md:179-389`；本会话 `recompute_p2_r1.py` | **PASS（R + V）** |
| 7 | W1 `__poll` 24.93/15.83%、poll boundary 9.16/4.38%、progress/wait 25.09/24.36% | §C.5 | `P2-COST-MAP-R1.md:451-470`；本会话复算 | **PASS（R + V）** |
| 8 | W3 族级 self 6.00/0.24/15.82/22.43/13.07%；P-B 0.24% `NOT_OBSERVABLE` | §C.5 | `P2-COST-MAP-R1.md:472-497`；本会话复算；`P2-REVIEW.md:54-71` | **PASS（R + V；P-B 已降级）** |
| 9 | M-53 d16 0.406%/0.285%；d64 P4 点估计 1.06%/1.13%，不显著 | §C.5/§C.7/§D.5 | `P2-COST-MAP-R1.md:499-511`；`P4/W3-ROWLEVEL.md:399-444`；`P4/W3-ROWLEVEL-REVIEW.md:81-97` | **PASS（R；P4 方向性）** |
| 10 | M-42 `lock addq`，≤0.14% cycles，`NOT_POWERED` | §C.5 | `P2-COST-MAP-R1.md:513-537`；本会话复算 | **PASS（R + V；19–20 cycles 为外部引用）** |
| 11 | P4 的 skid、stage/家族算术、`wait_one` mutex、557 行误读、表截断、sample/period 翻转 | §C.7 | `P4/W3-ROWLEVEL-REVIEW.md:72-220` | **PASS（R；报告正确转述了限制）** |
| 12 | A/A 11 格、352 arm、9 有效/2 无效、144 对/288 arm | §F.1–F.2 | `AA-CALIBRATION.md:20-37,173-223`；`AA-CALIBRATION-2.md:12-28,164-256`；本会话复算 | **PASS（R + V）** |
| 13 | C3/C3b/C7 `ENVIRONMENT_BLOCKED` 是预注册判据，不是 perf 缩放；机制是 sys-time 主导 | §B.5 | `AA-CALIBRATION.md:268-287`；`AA-CALIBRATION-2.md:346-391` | **PASS（R）** |
| 14 | 准入计数 38 行、`ADMIT_TO_AB=0`、11/11/1/2/2/2/4/4/1 | §D.1 | `P3-ADMISSION-R1.md:20-46`；`P3-ADMISSION-REVIEW.md` 复核 | **PASS（R；计数自洽）** |
| 15 | W1 D1/D2/D3/C1 语义阻断；C2 `NEEDS_OWNER`；A1/A2 不 material | §D.3/§E.2 | `W1-DESIGN.md:245-324`；`docs/explicit-io-contract.md:907-912` | **PASS（R；C2 契约允许）** |
| 16 | M-53 O1/O2 PROVEN、O3 INCONCLUSIVE | §E.3 | `M53-OBLIGATIONS.md:51-255`；本会话三个测试 7/7、20/20、21/21 | **PASS（R + V；测试为既有二进制）** |
| 17 | W3 语义门未进入；M-61 R3 0.45% <1% MME | §E.4 | `CAMPAIGN-LEDGER.md:222`；`P3-ADMISSION-R1.md:292` | **PASS（R）** |
| 18 | 本轮 0 个 A/B、0 个候选 build/SHA | §F.1/§F.3 | 报告与证据目录中没有候选 SHA/协议；本会话未运行 A/B | **PASS（V/U：未运行）** |
| 19 | 没有集成候选；数字不可相加 | §H.1–H.3 | `P2-COST-MAP-R1.md:341-357`；`P4R:185-220` | **PASS（R）** |
| 20 | `#634`/`#635` 历史减量只作 I；本轮新增减量 0 | §K.2 | `CAMPAIGN-LEDGER.md:165-189` | **PASS（I；未重跑）** |

**结论：** 报告的主要数字与结论有可追溯的证据路径，且对 R/I/U 的区分总体正确。问题集中在少数“把条件写成结论/把容器写成上界/把包级预算写成单候选预算”的表述，以及 K 的完成判词。

---

## 3. 过度声明、内部不一致与证据等级问题

### 3.1 High（不能用于准入/完成裁决）

#### H-1 `FINAL-REPORT.md:408`：M-62 的 4.13% 被写成“上界”

- **原文**：`| publication 4.13%（P-D） | claim_execution 0.61% | claim 叶样本 93/93 已归 publication | 不能相加；M-62 的上界是 4.13%（仍只是容器） |`
- **问题**：这直接违反报告自己反复声明的“阶段容器 ≠ 候选上界”，也违反 `P3-ADMISSION-R1.md:58`（“4.13% 是 publication 阶段容器，不是候选上界”）与 `:275`（“M-62 的 4.13% 是 publication 阶段容器，不是候选上界”）。报告 §D.2（`:478`）、§E.4（`:524`）和 §C.7（`:361`）都写的是“容器/不能用于准入”。`P2-COST-MAP-R1.md:349` 仍留有“M-62 的上界就是 4.13%”的旧句，但 P3 R1 的 H2 修订已明确纠正；终稿不应把未修正的旧句放进“不可相加矩阵”。
- **影响**：读者可能把 4.13% 当作 M-62 的候选上界，从而错误地认为 W3 有 4.13% 的可用空间。该归因不能用于准入裁决。
- **修法**：改为“publication 阶段容器 4.13%；M-62 的候选级可删量 `NOT_OBSERVABLE`，不能作为上界”。

#### H-2 `FINAL-REPORT.md:1006-1009` / `ISSUE-621-COMMENT.md:165-168`：O-2 被写成“唯一剩余阻断”

- **原文（报告）**：`若 owner 认为 W1-C2… 唯一剩余阻断是 O-2`；`若 O-2 被拒绝，则 O-1/O-3–O-12 只是状态确认/归档，不产生新的实验路径`；`唯一剩余阻断（明确）：Owner Gate O-2… 其余 owner 点… 不是新的技术实验`。
- **矛盾证据**：
  - `FINAL-REPORT.md:954-963`（K.1）自己列出未验证项：W1-C2、**W3 的候选级行级归因/步间语义**、**M-53 的 O3**、**M-53 的 d64 效应**；并说“剩余的三条设计门（W1-C2、W3 行级、M-53 O3/d64）都需要 owner 改变范围/预算或另立项目”。
  - `FINAL-REPORT.md:927-929` 的 O-4/O-5 明确要求“先做行级归因”“接受 cycles + δ=2% 的 17–20 对，同时要求三项迁移证明”；`:931` 的 O-8 要求“先做 `-scope64` 隔离 profile”；`:929` 的 O-6 要求“隔离格 + 动态回执带 + 机制门 + mutant kill”。
  - `P3-ADMISSION-R1.md:393-403` 的执行顺序把 W3 行级归因、M-53 迁移证明 + d64 A/A、M-40 设计、T9 owed/cancel/zero-op、M-42 隔离格列为 stage 1/2/3/4/5。
  - `FINAL-REPORT.md:478-479,855-856` 把 W3/W4 列为 `ADMIT_TO_DESIGN`，不是“已归档”。
- **影响**：如果 owner 按这句话拒绝 O-2 并关闭 #621，W3/M-53 的设计门会被跳过；这不是“没有进一步优化价值”，而是“剩余设计门未获预算”。该结论不能用于完成裁决。
- **修法**：把 O-2 定位为“唯一涉及通知拓扑语义授权的 owner 点”，不要称“唯一剩余阻断”；把 O-1/O-3–O-12 中确有实验内容的 O-4/O-5/O-6/O-8 明确列为“需 owner 资助/预注册的设计或资格实验”；K.5 的完成判词改为“当前没有可进入 A/B 的候选；是否继续设计门由 owner 决定”。

### 3.2 Medium

#### M-1 `FINAL-REPORT.md:14` / `:987` / `ISSUE-621-COMMENT.md:118`：把“0.4% 假设”写成 d64 的效应量级

- **原文**：`M-53… d64 在 0.4% 量级不可行`；`经济上不可行 | M-53 d64（0.4% 量级）`。
- **证据**：`M53-OBLIGATIONS.md:306-323` 明确写“**0.4% 不是 d64 实测效应**；0.406%/0.285% 是 d16 leaf self 份额；把它们当 d64 的计划 δ 是**乐观代理/上界假设**，不是测量结论”。P4 的 d64 点估计是 self 1.06%/owner 1.13% app（`P4/W3-ROWLEVEL-REVIEW.md:81-97`）。报告 §K.4（`:995`）自己的表述是条件句：“如果只有 0.4%，需要 172/336 对…先证明 d64 效应显著大于 0.4%”。
- **影响**：摘要和 K.3 的写法会被读成“d64 的效应就是 0.4%”，从而把 M-53 的经济性结论建立在假设上。
- **修法**：摘要和 K.3 改为“若 d64 效应接近 d16 的 0.4% 量级，则 172/336 对不可行；P4 的 d64 点估计 1.06%/1.13% 不显著，尚未证明大于 0.4%”。

#### M-2 `FINAL-REPORT.md:489`：把 W1D 的 eventfd 演示写成“本会话式演示输出”

- **原文**：`D1… 本会话式演示输出 after clear on dup: original False dup False`。
- **证据**：`FINAL-REPORT.md:159-181` 的“本会话已执行”清单没有 eventfd/dup 演示；该演示在 `p3/W1-DESIGN.md:271-293`（§3.4 D1）与 `:563-566`（§10.1）中，由 W1-DESIGN 会话执行。报告 §E.2（`:579`）正确地把它归给 `[W1D] §10.1`。
- **影响**：把 R 写成 V，削弱了报告的证据等级纪律。虽然 fd 语义结论本身成立，但“本会话执行”不实。
- **修法**：§D.3 D1 改为“`[W1D] §10.1` 的演示输出…”。

#### M-3 `FINAL-REPORT.md:673,949`：W3“只有族级/阶段容器；单候选无”与 P4 的候选级 owner 估计矛盾

- **原文**：§F.3 条件 2 写 `W3 | 只有族级/阶段容器；单候选无`；K.1 写 `W3 的四个成员… 只有阶段容器，没有候选级可删量`。
- **证据**：`P4/W3-ROWLEVEL.md:168-186` 给出候选级 owner-total：`offer_terminal` 1.80%（P-D）、`claim_execution` 1.56%、`release_execution` 1.14% 等；`:454-467` 明确把它们标为 `candidate-level measured hotspot`。报告 §C.7（`:424-436`）也完整列出了这些数值。P4R 的正确结论是“W3 的 mutex 行级定位可作为**方向性证据**，但不能作为 W3 定量准入依据”（`P4/W3-ROWLEVEL-REVIEW.md:255`），不是“没有单候选测量”。
- **影响**：读者会误以为 P4 只给出了阶段容器，从而低估 P4 的候选级信息；也会让 D.2 的“先做行级 measured 归因”显得与 P4 已完成的工作脱节。
- **修法**：改为“W3 有候选级 owner-total 点估计（offer_terminal 1.80%、claim_execution 1.56% 等），但 P4R 判定其为方向性证据；单候选可删量仍 `NOT_OBSERVABLE`，不能用于定量准入”。

#### M-4 `FINAL-REPORT.md:994`：把 W3 的包级预算 M 写成“单候选 A/B 的预算”

- **原文**：`W3 的 RequestCore 单函数 owner 约 0.3–1.8%… 即使设计出行级归因，单候选 A/B 的预算 M（72–144 对）与预期收益不匹配`。
- **证据**：`P3-ADMISSION-R1.md:389` 定义“M = 2–4 个臂组（A/A + A/B 72–144 对，需行级 profile 或 2 个格组）”，这是**包级/多臂组**预算；W3 的预算 M 见 `P3-ADMISSION-R1.md:180`。单候选在 C1 CPU 2% 只需 4/5 对、C4 CPU 2% 只需 20/38 对。把包级预算说成“单候选 A/B 预算”会夸大单位成本。
- **影响**：K.4 的“预期收益不匹配”论证依赖一个错误的预算口径；即使结论方向仍可能成立，该论据不能原样使用。
- **修法**：改为“W3 包级设计/资格预算为 M（2–4 个臂组）；单候选所需的 A/B 对数应按匹配格和预注册 δ 另算。P4 的 owner% 是候选成本点估计，不是可删收益，不能直接与预算比较。”

#### M-5 `FINAL-REPORT.md:1003` / `ISSUE-621-COMMENT.md:159`：`RESEARCH_COMPLETED` 与“不是 100% 已验证”自相矛盾

- **原文**：`建议：NO_FURTHER_OPTIMIZATION_JUSTIFIED（在 current contract 内 RESEARCH_COMPLETED）`。
- **证据**：`FINAL-REPORT.md:945` 写“严格意义上不是 100% ‘已验证’”；`:954-961` 列出 W1-C2、W3 行级、M-53 O3/d64 未验证；`:963` 说“剩余的三条设计门”。`ISSUE-621-COMMENT.md:161` 同样说“不是 100%：W1-C2、W3 的候选级行级归因、M-53 的 O3/d64 仍未验证”。
- **影响**：`RESEARCH_COMPLETED` 是一个会进入 issue 关闭判词的标签；在三个设计门未关闭时使用它，会被读成“研究已完成，只差 owner 批准”。这与 K.1 的限定不一致。
- **修法**：改为“`NO_FURTHER_OPTIMIZATION_JUSTIFIED`（当前没有可进入 A/B 的候选；W3/M-53 设计门仍开放，需 owner 决定是否资助）”，不要用 `RESEARCH_COMPLETED`。

#### M-6 `ISSUE-621-COMMENT.md:46`：d64 app 点估计 1.13% 并不低于 C7 的 t-hw 1.0066%

- **原文**：`d64 P4 点估计 self 1.06% / owner 1.13% app… 且低于匹配 C7 16 对 A/A 的 t-hw 1.0066%/MDE80 1.416%`。
- **证据**：app owner 1.128% **大于** t-hw 1.0066%，只低于 MDE80 1.416%；只有 all-comm period self/owner 0.593%/0.632% 才低于两者（`P4/W3-ROWLEVEL-REVIEW.md:20-21,207`）。`FINAL-REPORT.md:539` 的对应句只写“低于 C7 MDE”，是正确的；评论把 t-hw 也加进来，变成数值错误。
- **影响**：读者会以为 app 点估计落在 t-hw 之下，从而低估 M-53 的“边缘”程度。M-53 的结论（不显著/未达分辨力）不变，但数字表述不能用于准入。
- **修法**：改为“app owner 1.13% 低于 MDE80 1.416%，但高于 t-hw 1.0066%；all-comm period owner 0.63% 才低于两者；因此 16 对 A/A 在保守 t 判据下仍是边缘/差一对”。

#### M-7 `ISSUE-621-COMMENT.md:176`：评论依赖同目录未跟踪的 `FINAL-REPORT.md`

- **原文**：`本评论… 不依赖任何本地未跟踪工作副本`；同时 `:5` 写“完整报告：`FINAL-REPORT.md`（随本评论一并提交…）”。
- **证据**：本会话 `git status --porcelain=v1 --untracked-files=all` 显示 `?? docs/evidence/621-progress-wait-path-20261118/…`（包括 `FINAL-REPORT.md` 与评论本身）；`FINAL-REPORT.md:80` 也承认该目录在 BASE 中未跟踪。评论没有引用两个“全程不得触碰”的目录（好），但它确实依赖同目录的报告文件；如果报告没有随评论一并提交/归档，公开读者无法访问完整证据。
- **影响**：这是一条公开回写稿的自述准确性问题，不是凭据或敏感信息问题。
- **修法**：二选一：① 先把 `FINAL-REPORT.md` 与证据目录提交/归档，再发布评论；② 把句子改为“不依赖两个未跟踪工作副本；完整报告随本评论提交于同一路径 `docs/evidence/.../FINAL-REPORT.md`（需先提交）”。

#### M-8 `ISSUE-621-COMMENT.md:20-28,175`：记录值没有逐项标注 R/I

- **原文**：§2 表“环境门禁 | 频率门禁 PASS…”；“成本地图（profile）… PMU 198/198 行…”；“A/A 校准… 144 个有效配对”；§10 声称“按‘本会话复算 / 已记录 / 已合并历史 / 未运行’区分”。
- **证据**：报告 §B.1 明确频率门禁是 **R**（`:189-200`），不是本会话复跑；§A.4.4 与 §K.2 明确 `#634`/`#635` 是 **I**。评论的 §2 表把这些放在同一“做了什么”表里，没有 R/I 标记；只有 §5 的 `#634`/`#635` 表写了“历史回执”。
- **影响**：公开读者可能把“频率门禁 PASS”“PMU 198/198 行”读成本轮实测。评论自己的 §10 声称已区分，但正文没有完全做到。
- **修法**：在 §2 表逐行加 `[V]`/`[R]`/`[I]`/`[U]`，或把 §2 拆成“本轮复算（V）”与“记录/继承（R/I）”两表；至少给频率门禁、PMU/record、构建回执加 R 标记。

#### M-9 `FINAL-REPORT.md:170,707,1066`：独立 t/MME 脚本只在 `/tmp`，未归档

- **原文**：`自写 /tmp/final_tcheck2.py（不 import 仓库工具…）`；§G.1 与附录 B 只给命令和输出。
- **证据**：本会话 `ls /tmp/final_tcheck2.py` 存在；`find tools p2/profile/analysis -maxdepth 1 -type f` 只列出 `recompute_independent.py`、`p2/profile/analysis/recompute_p2_r1.py` 等，没有该脚本的归档副本。P2 R1 的复算还依赖 `/tmp/p2-621/{kallsyms.txt,symtwin/*.sym}`（`P2-COST-MAP-R1.md:750`）。
- **影响**：报告的“独立 t/MME 复算”是可复核的（归档 `recompute_independent.py` 也含 `requiredN`，本会话用它得到了相同值），但“不 import 仓库工具”的第二条独立路径没有随证据交付；未来读者无法从证据目录重跑该独立检查。
- **修法**：把 `final_tcheck2.py` 归档到 `tools/` 或 `p4/`（并记录 sha256），或在报告中说明“独立检查已由归档 `recompute_independent.py` 的 `requiredN` 字段交叉验证；`/tmp` 脚本仅作一次性第二实现”。

### 3.3 Low

#### L-1 `FINAL-REPORT.md:866`：`SEMANTIC_MISMATCH` 组写“维持”，但 M-16 是本轮改判

- **原文**：`SEMANTIC_MISMATCH | M-09、M-30、M-57、M-12、M-16、M-80 hygiene | 维持；先 owner 裁决`。
- **证据**：`FINAL-REPORT.md:510`（D.4 行 3）写 M-16 原终态 `INCONCLUSIVE` → 本轮 `SEMANTIC_MISMATCH`；`P3-ADMISSION-R1.md:230` 同样写“INCONCLUSIVE → SEMANTIC_MISMATCH”。M-16 不是“维持”。
- **影响**：I.2 的汇总表会让读者以为 M-16 原本就是 `SEMANTIC_MISMATCH`。
- **修法**：M-16 单列“本轮由 `INCONCLUSIVE` 改判为 `SEMANTIC_MISMATCH`（owner seam 冲突）”，其余五项写“维持”。

#### L-2 `FINAL-REPORT.md:616`：E.4 把 M-65 列为 W3 成员

- **原文**：`W3 的成员（M-61/M-62/M-65/M-67/M-10）仍是 INCONCLUSIVE`。
- **证据**：`FINAL-REPORT.md:478`（D.2 W3）列的是 M-61/M-62/M-67/M-10；`:525`（D.4 行 18）把 M-65 归 **W4 owed 家族**；`P3-ADMISSION-R1.md:79` 明确“M-65 单一归属 W4 owed 家族”。
- **影响**：W3/W4 的成员归属不一致，可能影响预算/阶段计数。
- **修法**：E.4 改为“W3 的成员（M-61/M-62/M-67/M-10）”；M-65 在 W4 段落单独列。

#### L-3 `FINAL-REPORT.md:948,981`：W1-C3 被写成“语义阻断”

- **原文**：K.1 `D1/D2/D3/C1/C3/A4–A8 语义阻断`；K.3 `当前契约下语义不能动 | … W1-C3 …`。
- **证据**：`FINAL-REPORT.md:494`（D.3）把 C3 标为 `NO_GO`（本战役范围），不是 `SEMANTIC_MISMATCH`；`W1-DESIGN.md:258-265` 的理由是“epoll 仍需 read；timerfd 增加 fd/syscall；relay thread 是新执行资源和第二通知权威”，不是 fd 语义冲突。
- **影响**：把 C3 的 `NO_GO` 与 D/C1 的 `SEMANTIC_MISMATCH` 混在一起，会让 W1 的语义冲突计数看起来多一项。
- **修法**：K.1/K.3 把 C3 单列为“`NO_GO`（本战役范围：epoll/timerfd/relay 无净减量）”，不要写“语义阻断”。

#### L-4 `FINAL-REPORT.md:454-468` 与 `:494`：38 行计数不含 W1-C3，但 D.3 增加了 C3 行

- **原文**：D.1 的 38 行计数中 `NO_GO` 为 4（M-61 payload、H1、io-wq、tp-w8）；D.3 又列 C3 为 `NO_GO`。
- **证据**：`P3-ADMISSION-R1.md:25-30` 的 38 行覆盖 W1 的七个方向，不含 C3；C3 是 W1-C 的子方向。报告 D.3 把 C3 作为额外行加入，但没有说明“不计入 38 行”。
- **影响**：读者可能以为 `NO_GO` 计数应为 5，或以为 C3 被计入某个 W1 行。
- **修法**：在 D.3 C3 行加注“子方向，不单独计入 38 行；W1 包的 `SEMANTIC_MISMATCH`/`NO_CHANGE_JUSTIFIED` 计数不变”，或在 D.1 说明 C3 的归属。

#### L-5 `ISSUE-621-COMMENT.md:44`：“15.8%/11.5% 是下界”没有证据

- **原文**：`P-E __poll 15.83%… x64 11.46%… 15.8%/11.5% 是下界，不是上界`。
- **证据**：`P2-COST-MAP-R1.md:464-470` 只说 `__poll` 是 inclusive 栈份额、`__x64_sys_poll` 是子集、`progress/wait`+poll boundary 仍不是候选上界；没有说 15.8%/11.5% 是“下界”。`P3-ADMISSION-R1.md:446` 只说“不能说 `__x64_sys_poll` 15.77%/11.46% 是上界；`__poll` inclusive 24.93%/15.83% 才是可复算的 poll 相关 inclusive；inclusive 不是收益”。
- **影响**：把两个不同口径（`__poll` inclusive 与 `__x64_sys_poll` 子集）并称“下界”，会制造一个新的、未证明的定量结论。
- **修法**：改为“`__poll` inclusive 15.83% 是可复算的 poll 相关份额；`__x64_sys_poll` 11.46% 是它的内核子集；两者都不是可删量或上界”。

---

## 4. A–K 完整性

`FINAL-REPORT.md` 的 A–K 全部存在，且每节都有“来源/证据等级”说明：

| 节 | 标题 | 是否存在 | 备注 |
|---|---|---|---|
| A | Repository Reality | 是 | Git/工具链/构建回执/U 清单完整 |
| B | E5 CPU & Environment | 是 | R + 本会话只读 V；未复跑 turbostat |
| C | Execution & Cost Map | 是 | P2 R + V 复算；P4 限制转述完整 |
| D | Candidate Admission Matrix | 是 | 38 行计数、W1–W6、#637 全量 |
| E | Semantic Evidence | 是 | #634/#635 为 I；W1/M-53 为 R/V；W3 为 U |
| F | A/B Experimental Results | 是 | 明确 0 A/B；A/A 清单完整 |
| G | Independent Statistical Audit | 是 | 复算、MME、噪声带、预算 |
| H | Cross-Candidate Integration | 是 | 无 GO/无集成候选；不可相加 |
| I | Final Decision Matrix | 是 | W1–W6 与 #637 分组 |
| J | Product/PR Handoff | 是 | 无产品改动；O-1–O-12 |
| K | Issue #621 Completion Recommendation | 是 | 有 K.5 的“唯一阻断”问题（H-2） |

**结论**：A–K 齐全；不是“缺节”，而是 K.5 的完成措辞与 K.1 的事实不一致。

---

## 5. K 的推荐是否与证据一致

### 5.1 一致的部分

- **`ADMIT_TO_AB = 0`**：P3 R1 的五条件与 38 行矩阵支持；本会话复算的 A/A、P2 成本地图、M-53 测试均未产生反例。**一致。**
- **“没有候选达到 A/B 准入”**：W1 的 material 方向语义阻断/C2 需 owner；W3 缺候选级可删量与步间语义；M-53 O3 未证、d64 分辨力不足；M-42 低于噪声。**一致。**
- **“本战役没有新增减量”**：没有产品改动、没有候选 A/B；`#634`/`#635` 是 BASE 之前/本身的历史 I。**一致。**
- **“不要用 36 对测 M-42”**：0.066–0.133% cycles 远低于 C6/C3b/C4 的 MDE；**一致。**
- **M-53 的预算分层**：δ=2% 17–20、δ=1% 58、0.4% 172/336、0.285% 347/678；**与 M53/P3/AA2 一致**（但 0.4% 是假设，见 M-1）。

### 5.2 不一致的部分

- **“O-2 是唯一剩余阻断”**：与 K.1 的三条设计门、D.2/I.1 的 `ADMIT_TO_DESIGN`、O-4/O-5/O-6/O-8 的实验内容冲突（H-2）。
- **“O-1/O-3–O-12 只是状态确认”**：O-4（W3 行级归因）、O-5（M-53 d64 A/A）、O-6（M-42 隔离格）、O-8（M-01 `-scope64` profile）都是技术实验/资格工作（H-2）。
- **`RESEARCH_COMPLETED`**：与“不是 100% 已验证”及三个设计门冲突（M-5）。
- **W3 预算 M 作为“单候选 A/B 预算”**：口径错误（M-4）。
- **d64 0.4% 作为“量级”**：条件假设被写成事实（M-1）。

### 5.3 建议的 K 表述

> 当前没有候选同时满足五条件，`ADMIT_TO_AB = 0`；本战役没有新增减量。W1 的 material 方向在 current root/拓扑下语义阻断，C2 需 owner 授权第二通知通道（O-2）。W3/M-53 仍是 `ADMIT_TO_DESIGN`：W3 需要候选级/行级归因与步间语义；M-53 需要 O3 的 8 项实现与 d64 cycles-only A/A。是否继续这些设计门，需 owner 通过 O-4/O-5 等决定预算；在 O-2 拒绝且 O-4/O-5 也不资助的情况下，可关闭 #621 的当前研究范围。当前没有可进入 A/B 的候选，因此不建议启动新的 A/B。

这个表述保留了“不继续 A/B”的负结论，同时不把设计门说成“已完成”或“只是状态确认”。

---

## 6. 评论稿公开发布适用性

### 6.1 通过项

- **无凭据/敏感信息**：`ISSUE-621-COMMENT.md` 中没有 password/token/secret/API key/私钥/`sudo` 命令/本地绝对路径；只出现公开 commit SHA `0e50612c` 和公开 issue 编号。**PASS。**
- **未引用两个“全程不得触碰”的目录**：没有 `621-round2-R1/` 或 `e5-fullpath-machine-work/` 的内容引用。**PASS。**
- **数字与报告大体一致**：成本地图、A/A、准入计数、`#634`/`#635` 历史值、预算表均与报告/证据一致（除 M-6 的 t-hw 与 L-5 的“下界”）。
- **没有把未运行写成通过**：§2 明确“本轮未运行 xmake/完整矩阵/TLC/新 perf/bench/A/A/A-B/语义 oracle 重跑”。**PASS。**

### 6.2 需修正项

| # | 位置 | 问题 | 修法 |
|---:|---|---|---|
| 1 | `:165-168` | “唯一剩余阻断 O-2”与报告 K.1 的三条设计门冲突（H-2 的评论版） | 改为“O-2 是唯一涉及通知拓扑语义授权的 owner 点；W3/M-53 设计门仍开放，是否资助需 O-4/O-5 等决定” |
| 2 | `:46` | app owner 1.13% 不低于 C7 t-hw 1.0066%（M-6） | 改为“低于 MDE80 1.416%，高于 t-hw 1.0066%；all-comm 0.63% 才低于两者” |
| 3 | `:176` | “不依赖任何本地未跟踪工作副本”与同目录未跟踪的 `FINAL-REPORT.md` 冲突（M-7） | 先提交报告/证据目录，或改写为“不依赖两个未跟踪工作副本；完整报告随评论提交于同一路径” |
| 4 | `:20-28` | 频率门禁 PASS 等 R/I 事实未标等级（M-8） | 给 §2 表加 `[V]/[R]/[I]/[U]`，或拆表 |
| 5 | `:44` | “15.8%/11.5% 是下界”未证明（L-5） | 改为“`__poll` inclusive 15.83% 是 poll 相关份额；11.46% 是其内核子集；都不是可删量或上界” |
| 6 | `:118` | “d64 在 0.4% 量级不可行”把假设写成事实（M-1） | 改为“若 d64 效应接近 d16 的 0.4% 量级，则预算不可行；P4 点估计 1.06%/1.13% 不显著” |
| 7 | `:159` | `RESEARCH_COMPLETED` 与“不是 100%”冲突（M-5） | 改为“当前没有可进入 A/B 的候选；设计门仍开放，需 owner 决定是否资助” |

**总体判断**：评论稿在“无凭据/无敏感信息/未引用禁碰目录”上合格；修正 H-2（评论版）与 M-6/M-7/M-8/L-5 后，适合公开发布。否则读者会看到一个与完整报告 K.1 矛盾的“唯一阻断”结论，以及一个数值上不成立的“低于 t-hw”陈述。

---

## 7. 未执行 / 不可确认

本复核**没有**执行以下检查，因此不能把这些写成“通过”：

1. 任何 `xmake f`/`xmake build`/`xmake test`；没有 clean rebuild；当前 master 的测试总数 `N` 未确认。M-53 三个测试是对 2026-11-12 既有二进制的运行，不是本轮构建证据。
2. 任何新的 perf/record/stat/eBPF/uprobe/strace；任何新的 A/A 或 A/B；任何候选 build/SHA。
3. 任何 TLC/`scripts/verify_progress_source_model.sh`；任何语义 oracle/mutant 重跑。
4. 任何 ASAN/TSAN；完整构建测试矩阵。
5. P4 `analyze_w3.py` 的本次重跑；`SHA256SUMS` 155/155 的本次校验（本复核只读了 `P4R §1` 的回执）。
6. `#637` 归档 `e2f41acb` 的原始回执；M-61 R3 0.45%、M-28 1b/1c 的独立复算（只按 ledger 转述核对）。
7. H1 patch 的 envelope tests；H1 patch 未应用到 HEAD。
8. `#634`/`#635` 历史性能数字的重跑；它们是 I，不是本轮实测。
9. 真实消费者 profile（#604 G0）；#617 G1 kernel-boundary。
10. 行级内核/libc/libstdc++ 归因；skid/PEBS 误差；d64 平均扫描深度；逐请求因果链。

---

## 8. Verdict

**不可按现状发布。**

- **High H-1**（`FINAL-REPORT.md:408` 的“M-62 的上界是 4.13%”）必须改为“publication 阶段容器，候选级可删量 `NOT_OBSERVABLE`，不是上界”。这是报告中心口径“阶段容器 ≠ 候选上界”的直接反例，不能用于准入。
- **High H-2**（`FINAL-REPORT.md:1006-1009` 与 `ISSUE-621-COMMENT.md:165-168` 的“唯一剩余阻断 O-2”/“O-1/O-3–O-12 只是状态确认”）必须改为“O-2 是唯一涉及通知拓扑语义授权的 owner 点；W3/M-53 设计门仍开放，是否资助由 O-4/O-5 等决定”。该结论不能用于关闭 #621。
- **Medium M-1–M-9 与 Low L-1–L-5** 应一并修正，尤其是：d64 0.4% 假设的写法（M-1）、W1D 演示的 V/R 误标（M-2）、W3“只有阶段容器”的过度否定（M-3）、包级预算 M 的口径（M-4）、`RESEARCH_COMPLETED` 标签（M-5）、评论的 t-hw 数值（M-6）与未跟踪依赖（M-7）。

**修正 H-1、H-2 后，报告的主结论（`ADMIT_TO_AB = 0`、无 GO、无产品 PR、无新 A/B、W1 material 方向语义阻断、W3/M-53 仍需设计门、M-42 低于噪声、`#637` 终态未翻案）与证据一致，可以发布。**

---

*复核结束。本文件只读复核，不替代 `FINAL-REPORT.md`、`ISSUE-621-COMMENT.md`、root 或 ledger。*
