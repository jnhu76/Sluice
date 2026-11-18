# P3-ADMISSION-R1 — #621 W1–W6 与 #637 历史候选准入裁决（修订版）

| 字段 | 值 |
|---|---|
| 角色 | 准入矩阵修订员（只读证据；只写 `p3/`；不修改产品代码） |
| 日期 | 2026-11-18 |
| 修订对象 | `p3/P3-ADMISSION.md`（403 行）；依据 `p3/P3-ADMISSION-REVIEW.md` 的 H1–H3、M1–M7、L1/L4 |
| 战役 BASE | `5d94f233967c3aef993eb19ba9e3252c532b0951` |
| 性质 | **修订版准入矩阵**。不是新测量、不是 A/B、不是合并授权；不替代 `docs/explicit-io-contract.md`、`docs/roadmap/v2-conformance.md` 或 `CAMPAIGN-LEDGER.md` |
| 输入 | `CAMPAIGN-LEDGER.md` §4/§5/§9；`p2/P2-COST-MAP-R1.md`；`p2/P2-REVIEW.md`；`p3/W1-DESIGN.md`；`p1/AA-CALIBRATION.md`；`p1/AA-CALIBRATION-2.md`；`p3/P3-ADMISSION-REVIEW.md` |
| 本轮执行 | 只读复算与源码抽查（见 §7）：P2 R1 复算、P1/P2 A/A 复算、required-n 算术、M-42 objdump、源码坐标 |
| 本轮未运行 | `xmake` 构建/测试、任何新的 perf/bench/A/A/A-B、TLC、sanitizer、完整矩阵、`#637` 归档（`e2f41acb`）的原始回执 |

> **一句话结论（修订后）**：**`ADMIT_TO_AB = 0` 不变。** W1 的 D1/D2/D3/C1 是硬语义阻断；C2 是 `NEEDS_OWNER`（契约原则允许 condvar，但不能替代 owner 对第二通知通道的授权）；W3/W4 只到 `ADMIT_TO_DESIGN`，且 W3 的 A/A 必须按形态匹配（P-D 的 C2b 是 NOISY，无可用带）；M-42 `NOT_POWERED`；**M-28 恢复 `NO_GO_ABORT`（不是 `NOT_APPLICABLE`）**；W2/W5 `NOT_APPLICABLE`；`#637` 的 `NO_GO`/`NO_GO_ABORT`/`SEMANTIC_MISMATCH` 终态全部维持，**没有任何翻案条件被满足**。P2 profile 是形态级 hotspot 地图，不是候选级准入证据。

---

## 0. 修订摘要与计数

### 0.1 覆盖与修订后计数

本文件覆盖：

- **W1–W6 六个工作包**（看板见 §2）；
- **W1 的七个方向行**：D1/D2/D3 阻塞 read、C1 第二等待 fd、C2 condvar 双通道、A1/A2/A3 hygiene、A4/A4' probe、A5–A8 删除重验/drain/P3/后检查、B 有界等待；
- **W3 的四个行级候选**：M-61 `begin`/`complete`、M-62、M-67、M-10；
- **W4/W6 的六个候选**：M-53、M-40、M-63、M-65（W4 owed 家族）、M-42、M-28；
- **`#637` 其余 15 条**：M-80 `write_all_at`、M-16、M-37、M-01、M-33、M-74、M-09、M-30、M-57、M-12、M-61 payload、M-80 hygiene、H1、io-wq punt、tp-w8。

共 **38 行**（W1–W6 包 6 行 + W1 方向 7 行 + W3 个体 4 行 + W4/W6 个体 6 行 + `#637` 其余 15 行）。

| 裁决 | 计数 | 与 P3 的差异 | 说明 |
|---|---:|---|---|
| `ADMIT_TO_AB` | **0** | 不变 | 没有候选同时满足五条件 |
| `ADMIT_TO_DESIGN` | 11 | 不变（M-65 归 W4；8 行保留 `INCONCLUSIVE` + design work） | W3 包、W4 包、M-62、M-65、M-67、M-10、M-53、M-40、M-63、M-33、M-74 |
| `SEMANTIC_MISMATCH` | 11 | **−1**（W1-C2 移出） | W1 包、W1-D、W1-C1、W1-A4、W1-A5–A8、M-16、M-09、M-30、M-57、M-12、M-80 hygiene |
| `NEEDS_OWNER` | **1** | **+1**（W1-C2） | 契约允许 condvar，但需 owner 授权第二通知通道；不计入硬语义冲突 |
| `NO_CHANGE_JUSTIFIED` | 2 | 不变 | W1-A1/A2/A3、W1-B 有界等待 |
| `NOT_APPLICABLE` | **2** | **−1**（M-28 移出） | W2、W5 |
| `NOT_POWERED` | 2 | 不变 | W6 包（M-42）、M-42 |
| `INCONCLUSIVE` | 4 | 不变 | M-61 `begin`/`complete`、M-80 `write_all_at`、M-37、M-01 |
| `NO_GO` | 4 | 不变 | M-61 payload、H1、io-wq punt、tp-w8 |
| `NO_GO_ABORT` | **1** | **+1**（M-28 恢复） | M-28 `syscall_count_` 宏门控剔除；本战役不重做 |
| **合计** | **38** | — | 0+11+11+1+2+2+2+4+4+1 = 38 |

> **计数口径**：W3/W4/W6 包与其成员候选分别列出；W6 包的 `NOT_POWERED` 指 M-42，M-28 单列为 `NO_GO_ABORT`。包裁决不等于成员候选自动获得同一裁决。W1 包与 W1-A4 的 TP-only 子方向另有 `NO_CHANGE_JUSTIFIED` 说明，不重复计入上表。`NEEDS_OWNER` 是 `W1-DESIGN` 的判词，不是 ledger §9.2 的处置词汇；它表示“契约原则允许、但需 owner 授权新拓扑”，在 ledger §9.2 口径下仍属“需要 owner 先裁决”，因此“owner 未裁决前不进入性能实验”的约束不变。`NO_GO_ABORT` 是 `#637` 归档终态词（ledger §4 行 1，`:207`），按原样保留；它是 `NO_GO` 的预注册中止子类（判词 1b/1c），不是 `NOT_APPLICABLE`。

### 0.2 五个最重要的结论（修订后）

1. **W1 的 material 方向分两类。** D1/D2/D3/C1 在当前 root/拓扑下语义不成立：`dup` 共享文件状态标志、同 fd 改 blocking 破坏非阻塞 ack/drain 与有界等待、io_uring 单 eventfd 注册与第二等待 fd 冲突（`p3/W1-DESIGN.md:267-324`）。C2（TP condvar + protected predicate）**契约原则允许**（`docs/explicit-io-contract.md:907-912`），但需要 owner 授权第二通知通道、新模型与新 oracle，判 **`NEEDS_OWNER`**，不是硬语义冲突。语义保持的 A1/A2 只删 1–2 次 mutex，低于 C6/C1 噪声带。
2. **W3 只有族级/阶段容器，没有单候选 measured hotspot。** P2 的 publication 4.13%（P-D）、terminal 3.49%（P-D）、progress/wait 25.09%+9.16%（P-B）都是混合容器（`p2/P2-COST-MAP-R1.md:472-497`）；**4.13% 是 publication 阶段容器，不是候选上界**，M-62 的候选级可删量仍是 `NOT_OBSERVABLE`。P-B 的 0.24% 族级数字因 90.67% app 样本无 bench 帧而 `NOT_OBSERVABLE`（`p2/P2-REVIEW.md:54-71`；本会话复算 `app_frames` 12,246/11,104/1,142）。M-61 还有 r6 R3 的 0.45% < 1% MME 反例（`CAMPAIGN-LEDGER.md:222`）。
3. **M-53 的 d16 热点真实但很小；d64 仍未达标。** d16 leaf self 0.406%（P-A）/0.285%（P-C），约 61.3/58.0 cycles/op（`p2/P2-COST-MAP-R1.md:499-511`）。**17–20 对只对应 δ=2%**；δ=1% 的 A/A 与 A/B 都需要 58 对；若 d64 效应接近 d16 的 0.4%，hw≤效应/2 需要约 172/336 对（0.406%）或 347/678 对（0.285%）。三项迁移义务仍未证（`CAMPAIGN-LEDGER.md:211`）。
4. **M-42 没有 measured hotspot，且算术上界低于噪声带。** `submit_flushes_` 编译为 `lock addq $0x1,0x120(%rdi)`（`src/async/uring_transport.cpp:222`；objdump `4241b7`）；按 19–20 cycles 为 P-A 0.126–0.133%、P-B 0.066–0.070%、P-C 0.093–0.098% cycles（`p2/P2-COST-MAP-R1.md:513-537`），1 条 instruction/submit 仅 0.0041–0.0094%。`GO` 只是“排队、未启动”，不是本战役的 A/B 准入（`CAMPAIGN-LEDGER.md:217,243-247`）。
5. **`#637` 的终态没有被本轮 profile 推翻。** 所有 `NO_GO`/`NO_GO_ABORT` 的翻案条件都未满足；**M-28 的终态是 `NO_GO_ABORT`，不是 `NOT_APPLICABLE`**：`syscall_count_` 仍在生产（`src/async/threadpool_backend.cpp:434`；`include/sluice/async/detail/threadpool_backend.hpp:258`），所以不是“无适用对象”。`SEMANTIC_MISMATCH` 仍是 owner 裁决面，不应被包装成性能实验（`CAMPAIGN-LEDGER.md:191-203,233-239`）。P2 的形态级热点只够作 `#617` trigger-grade 线索，不能作 `#621` 的 A/B 证据（`p2/P2-REVIEW.md:322-331`）。

### 0.3 证据口径（贯穿全文）

- **份额 ≠ 可删 cycles**；**阶段容器 ≠ 候选上界**；**inclusive 不能相加，也不能当收益**（`p2/P2-COST-MAP-R1.md:82-94,341-357`）。**4.13% 是 publication 阶段容器，不是候选上界。**
- **profile 只证明 bench/reference 形态**；`sluice-copy` 是参考应用，不是 `#604` G0 真实 workload（`CAMPAIGN-LEDGER.md:305-315`；`p2/P2-REVIEW.md:239-247`）。
- **A/A 只能回答“若效应是 X，需要多少对”**，不能回答“效应是 X”；**匹配格不能跨形态搬运**（`p1/AA-CALIBRATION.md:384-402`；`p1/AA-CALIBRATION-2.md:393-419`）。C5b 的 CPU 带只适用于 C5b（TP d1 w1）；它不是 P-D/P-C/P-E/P-A/P-B 的匹配格。
- **17–20 对只对应 δ=2%**；δ=1% 需要 58 对；若 d64 效应接近 d16 的 0.4%，需要 172/336 对。不得把“δ=2% 的最小 A/A”写成通用预算。
- **owner 裁决不是技术结论**。本文件把需要 owner 决定的事项单列在 §5，不替 owner 做选择，也不把“尚未裁决”写成“已证明可行”。

### 0.4 修正回执表（H1–H3、M1–M7、L1/L4）

> 本表是 P3 → R1 的逐项回执。每行“R1 落点”给出修订后所在章节；“回执/依据”给出本会话运行/读取的证据。所有修正均不改变已成立的负结论（`ADMIT_TO_AB=0`、W1 D/C1 语义阻断、M-42 `NOT_POWERED`、W2/W5 `NOT_APPLICABLE`、`#637` 终态未推翻）。

| 编号 | P3 原问题（落点） | R1 已应用修订 | 回执 / 依据 |
|---|---|---|---|
| **H1** | P3 §1.3 把 C5b 标为“W3 TP d1 w1”（`:98`），§4.2 用“C5b CPU 2% 需 5/7”支撑“A/A 分辨率不是瓶颈、36 对预算对 2% 足够”（`:236`），§6.2 stage 1 又写 P-B 用户栈截断修复（`:318`）。但 P2 没有任何 W3 形态在 TP d1 w1 上采集；W3 主形态是 P-D（TP d16 w4，22.43%）、P-C（W-03 d16，15.82%）、P-E（copy，13.07%）。 | §1.3 删除 C5b 的 W3 用途，改标“**不是任何 P2 W3 形态的匹配格**”；§4.2 新增按形态表：P-D→C2b **NOISY，无可用带**；P-C→C4 CPU（2% 需 20/38，hw≤1% 需 38）；P-E→C1 CPU（2% 需 4/5）；P-A→C3b cycles（补充行）；P-B 族 **NOT_OBSERVABLE**。把“36 对对 2% 足够”改为“**仅对 C5b/C3b/C7 成立**；P-D 无带，C4 的 hw≤1% 需 38 对”。§6.2 stage 1 改为按 P-D/P-C/P-E 主形态列。 | 本会话运行 `python3 -I p2/profile/analysis/recompute_p2_r1.py --json-out /tmp/r1-check.json`（exit 0）后读取 `aa.matching`：`P-D = C2b (W02-TP-4k-d16-w4; NOISY, no usable band)`；`P-C = C4 (... QUALIFIED)`；`P-E = C1 (... QUIET)`；`P-B = C6 (...)`；`C5b = "not P-D (d16) and not P-B (uring)"`、`"QUIET conditional; not a matching cell"`。required-n 复算：C4 CPU 2%=20/38、C1 CPU 2%=4/5、C3b cycles 2%=6/10、C5b CPU 2%=5/7、C7 cycles 2%=10/17。依据 `AA-CALIBRATION-2.md:413-416`、`P2-REVIEW.md:75-92`、`P2-COST-MAP-R1.md:474-482`。 |
| **H2** | P3 §4.2 写“M-62 的 4.13% 是上界”（`:229`），与同文件“阶段容器不能作准入上界”（`:84`）、“阶段容器不是候选上界”（`:232`）自相矛盾；§3.2 M-62 行自己写的是“P-D publication 4.13% 容器”（`:164`）。 | §4.2 改为“**M-62 的证据是 publication 阶段容器 4.13%，不是候选上界；M-62 的候选级可删量仍 `NOT_OBSERVABLE`，需要行级 profile**”。§3.2 M-62 行统一写“P-D publication 4.13% 阶段容器；4.74% 的双计已删；4.13 仍不是候选上界”。全文不再把 4.13 放入任何“上界”语境。 | `p2/P2-COST-MAP-R1.md:488-495`：M-61/M-62 标“阶段容器 / NOT_OBSERVABLE；不给候选上界”，`claim_execution` 叶样本 P-D 93/93 已归 publication；`P2-REVIEW.md:116-143`（M2/M3）。本会话复算 P-D `publication` 627/15,182 = 4.13%。 |
| **H3** | P3 把 M-28 从 `NO_GO_ABORT` 改标为 `NOT_APPLICABLE`/`NO_REDO`（`:37-40,139,177,271`），但 `syscall_count_` 仍在生产；翻案“三条件”（`:177,271`）漏掉 ledger §4.1 第 1 条“可分离的单指令差”。 | §0.1 改为 `NOT_APPLICABLE` 2（W2、W5）+ `NO_GO_ABORT` 1（M-28）；§2 W6 行改“`NO_GO_ABORT（不重做）`”；§3.3/§4.6/O-10 统一。翻案条件补全为四要素：① 新证据把机制约束为可分离的“单指令差”；② 消除/隔离 TU 重编译布局变化、成员移除的对象布局变化、RMW 移除对 `work_mtx_` 竞争时序的影响；③ 新 session、新冻结；④ 动态回执自带接受带与隔离格判据。 | `CAMPAIGN-LEDGER.md:207`（M-28 终态 `NO_GO_ABORT`）、`:235-239`（§4.1 三项，其中第 3 项含“新 session、新冻结”与“动态回执自带接受带与隔离格判据”两个并列要素）、`:512`（`NOT_APPLICABLE` 定义是“无适用对象”）。本会话 `grep -n syscall_count_`：`src/async/threadpool_backend.cpp:434`、`include/sluice/async/detail/threadpool_backend.hpp:258`。`NO_REDO` 是 ledger §5.2 的优先级标签（`:262`），不是处置词汇。 |
| **M1** | P3 §3.3/§4.3/O-5 把“d64 cycles-only A/A 17–20 对”写成通用预算（`:173,246,294`）；§4.3 把 d16 leaf self 0.406%/0.285% 称为“0.4% 的效应”（`:240`）；又允许 δ=1% 的 A/B 58 对，却把 A/A 固定为 17–20（`:246`）；并用 C3b 的 0.633% 覆盖 P-C 的 0.285%（格不匹配）。 | §4.3/O-5 写明：**17–20 对只对应 δ=2%**（C7 cycles 的 hw≤1%）；**δ=1% 的 A/B 58 对不能配 A/A 17–20**：17 对 hw≈0.971%、20 对≈0.884%，均 >0.5%；δ=1% 路径的 A/A 也需 58 对（hw≈0.497%）。若 d64 效应接近 d16 的 0.406%，`hw≤效应/2` 需 **336** 对（80% 功效需 **172** 对）；若接近 0.285%，需 **678/347** 对。明确 0.406%/0.285% 是 leaf self 份额，不是 A/B 效应；P-C 的 0.285% 改引其匹配格 C4 CPU（36 对 hw 1.021%/MDE80 1.450%），不再用 C3b 覆盖。 | 本会话 required-n 复算（同一 `t` 公式）：C7 cycles SD 1.889；0.406%→n80=172、n_half(0.203%)=336；0.285%→n80=347、n_half(0.1425%)=678；δ=1%→30/58；hw(17)=0.971%、hw(20)=0.884%、hw(58)=0.497%。C4 CPU 36 对 hw 1.021%/MDE80 1.450%。依据 `P2-COST-MAP-R1.md:499-511`、`AA-CALIBRATION-2.md:485-506`、`CAMPAIGN-LEDGER.md:211`。 |
| **M2** | P3 的 M-62/M-65/M-10 重开条件没有 W3 门禁的 `publication_inflight`/slot reuse/observer 注册义务；只有 M-61 行写了 `publication_inflight`（`:163-167`）。 | §3.2 M-62、§3.3 M-65（W4 owed）、§3.2 M-10 的“缺什么/翻案条件”补入：“任何跨方法临界区合并必须证明 `publication_inflight`/slot reuse/observer 注册不被改变”。W3 表后加注：该门禁对任何跨方法临界区合并生效；M-67 若走融合查询同样适用。 | `CAMPAIGN-LEDGER.md:259`（W3 门禁原文）。 |
| **M3** | P3 的 M-61 重开条件只写“行级分离 + `publication_inflight` 反例处理”和“融合的步间语义”（`:163`），漏掉 ledger 的“确定性测试注入点”和“族 A/A 先行”。 | §3.2 M-61 行补全为“新 measured profile 推翻 r6 R3 拒绝（0.45% < 1% MME，含 `publication_inflight` 反例与**确定性测试注入点**）+ 保步间语义的融合单因素 A/B（**族 A/A 先行**）”。 | `CAMPAIGN-LEDGER.md:222`（M-61 `begin`/`complete` 重开条件原文）。 |
| **M4** | P3 的 M-42 门禁只写“measured hotspot；隔离格 + 动态回执带 + 机制门七问”（`:176`），§4.4 只说明唯一读者在测试宏内（`:254`），漏掉 mutant kill、release 无 seam 符号和“出现生产读者 → NO_GO/重新物化”。 | §3.3 M-42 与 §4.4 补全为：“无生产读者、机制门七问、动态回执带（不能只做静态三列）+ 隔离格判据、**mutant kill**、**release 预处理与 `nm` 无 seam 符号**；**出现生产读者 → `NO_GO`/重新物化**”。 | `CAMPAIGN-LEDGER.md:217,262`；`src/async/uring_test_seams.hpp:75-76`（唯一读者，文件受 `SLUICE_HAS_LIBURING && SLUICE_ASYNC_INTERNAL_TESTING` 保护）；`include/sluice/async/detail/uring_backend.hpp:324`（成员）。 |
| **M5** | P3 §6.2 stage 1 的第一步写成“P-B 用户栈截断修复”（`:318`），但 P-B 的 W3 族只有 0.24%，P-D 22.43%/P-C 15.82%/P-E 13.07% 才是 W3 主体；修 P-B 栈不能产生 W3 主形态的候选级归因。 | §6.2 stage 1 改为“W3 行级归因（debug build/隔离格）**按 P-D/P-C/P-E 主形态分层**”；门禁写明“P-D 的 C2b A/A 重跑或改形态；P-B 0.24% 保持 `NOT_OBSERVABLE`；**不以 P-B 栈截断修复代替主形态归因**”。 | `P2-COST-MAP-R1.md:474-482`（族级 self）；`P2-REVIEW.md:54-71`（P-B 90.67% app 无 bench 帧）。 |
| **M6** | P3 把 W1-C2 计入 `SEMANTIC_MISMATCH`（`:153,210`），但 `PROG-02` 明确允许 condvar + protected predicate（`docs/explicit-io-contract.md:907-912`），`W1-DESIGN` 对 C2 的判词是 `NEEDS_OWNER`（`:256`）。 | §3.1 C2 行、§4.1、§5 O-2 改标 **`NEEDS_OWNER`**；§0.1 计数改为 `SEMANTIC_MISMATCH` 11 + `NEEDS_OWNER` 1，并注明 `NEEDS_OWNER` 不是 §9.2 处置词汇、不并入硬冲突计数。 | 本会话读取 `docs/explicit-io-contract.md:905-914`（“condition variable with a protected predicate”）；`p3/W1-DESIGN.md:245-256`（C2 判词 `NEEDS_OWNER`）。 |
| **M7** | M-65 同时出现在 W3 表（§3.2）、W4 看板（§2）与 §6.2 stage 4；矩阵没有单一包归属，预算/阶段冲突（P3 `:134-139,165,321`）。 | M-65 单一归属 **W4 owed 家族**：从 §3.2 移入 §3.3；W3 个体改为 4 行，W4/W6 个体改为 6 行（总数仍 38）。预算 **M**，阶段 **T9 owed 协议（§6.2 stage 4）**，与 M-63 同激活前提。 | `CAMPAIGN-LEDGER.md:224`（#637 行 18：M-65 与 #6 同激活前提）、`:260`（W4 门禁“owed 家族”）、`:274`（§5.2 P2 “M-63 / M-65 owed sweep 融合”）。注：ledger `:271` 的 W3 族清单也列了 M-65，但那是“行级分离”观察面，不构成预算/阶段归属；R1 按 owed 激活前提归 W4。 |
| **L1** | P3 §2 把 W1 标为“**终判**”（`:134`），但 O-1/O-2/O-3 仍开放，O-2 可改变 W1 的 C2/D 结论；“终判”与“owner 点开放”矛盾。 | §2 W1 阶段列改为 **“owner 点开放”**；看板说明改为“W2/W5/W6 已到终判；W1 不是终判，owner 点开放（O-1/O-2/O-3）”。 | P3 `:134,141,290-292`；本文件 §2、§5。 |
| **L4** | P3 把 8 个 `#637` `INCONCLUSIVE` 候选（M-62/M-65/M-67/M-10/M-33/M-74/M-40/M-63）直接改写成 `ADMIT_TO_DESIGN`，但 ledger §4 规则 5 要求 `INCONCLUSIVE` 必须满足重开条件才可进入下一轮；设计工作本身是重开条件的一部分。 | 这 8 行的裁决列保留 **“`INCONCLUSIVE` + design work（`ADMIT_TO_DESIGN` 仅指设计/资格工作）”**；`ADMIT_TO_DESIGN` 计数 11 不变，但注明其中 8 行是 design work，原终态未改。 | `CAMPAIGN-LEDGER.md:199`（规则 5）、`:218`（M-40）、`:223-227`（M-62/M-65/M-67/M-10/M-33/M-74）。 |

**附带低危修正（不改变裁决）**：

| 编号 | P3 原问题 | R1 修订 | 回执 / 依据 |
|---|---|---|---|
| L2 | M-65 行用 `CAMPAIGN-LEDGER.md:212`（M-63 行）支撑“owed 机制本体 KEEP”；M-10 行用 `:221` 支撑“`NOT_OBSERVABLE`”，但 `:221` 是 M-10 的 `INCONCLUSIVE` 行。 | 更正为 `CAMPAIGN-LEDGER.md:224`（M-65 行）；M-10 的 `NOT_OBSERVABLE` 改引 `p2/P2-COST-MAP-R1.md:492`。 | 本会话 `nl -ba CAMPAIGN-LEDGER.md` 读取 `:212`（M-63）、`:221`（M-10 `INCONCLUSIVE`）、`:224`（M-65）；`P2-COST-MAP-R1.md:492`。 |
| L3 | P3 把 M-42 的 19–20 cycles 当算术上界时未重复“外部引用、未重测锁延迟”的置信限制；M-53 的 61.3/58.0 cycles/op 未重复 PMU/record 分母不一致的限制。 | §4.3/§4.4 加注：“19–20 cycles 是外部引用，本会话只确认 `lock addq`，未重测锁延迟”；“61.3/58.0 cycles/op 是 leaf share × PMU cycles/op，PMU/record 分母不一致、无 PEBS/skid，置信 MEDIUM/LOW”。 | `P2-REVIEW.md:146-158,162-174`；`P2-COST-MAP-R1.md:524`。 |
| L5 | W2 的 `NOT_APPLICABLE` 成立，但 P3 没有解释 M-67（ProgressSource sticky flags）按 ledger §5.2 被路由到 W3。 | §2 W2 行加注：“M-67 按 ledger §5.2 路由到 W3，不是 W2 新候选”。 | `CAMPAIGN-LEDGER.md:258,271`。 |

---

## 1. 裁决规则与证据门槛

### 1.1 `ADMIT_TO_AB` 的五条件

按 ledger §5.2 与 `#621` 纪律，候选必须**同时**满足：

1. **合法机制**：不违反 root 的 PROG-01–04、CANCEL-01、BACKEND-01 等语义，且不需要未裁决的拓扑变更；
2. **候选级 measured hotspot（或等价证据）**：有候选专属的 measured 份额/计数/隔离格证据，而不是族级线索或阶段容器；
3. **语义可证**：有确定性 oracle、mutant、协议/模型证明或可执行的步间语义映射；
4. **A/A 分辨率足以在预算内分辨预注册 MME**：匹配格、正确指标、预注册 δ，所需对数在预算内；
5. **无历史反例**：没有已归档的 `NO_GO`/`NO_GO_ABORT` 或被推翻的候选级测量。

处置映射（修订）：

- 不满足第 1/3 条且属于**契约/root 硬冲突** → `SEMANTIC_MISMATCH`；
- 契约原则允许、但需要 owner 授权新拓扑/第二通知通道 → `NEEDS_OWNER`（`W1-DESIGN` 判词；在 §9.2 口径下仍属“需要 owner 先裁决”，裁决前不进入性能实验）；
- 需要先做设计/语义/隔离格/行级归因 → `ADMIT_TO_DESIGN`；
- 无 measured hotspot 或低于噪声带 → `NOT_POWERED`/`NO_CHANGE_JUSTIFIED`；
- 已被合并/删除/非本战役 → `NOT_APPLICABLE`；
- 证据不足且没有已定义的设计门 → `INCONCLUSIVE`；
- 已归档的否决且翻案条件未满足 → `NO_GO`/`NO_GO_ABORT`。

### 1.2 “候选级 measured hotspot”的最低口径

本裁决只接受以下之一作为候选级证据：

- 候选专属的 leaf/inclusive 份额，且能从源码坐标说明“删掉什么机器工作”；
- 候选专属的 tracepoint/uprobe 计数，且能给出每 op/chunk 的归因；
- 隔离格（submit-only/zero-op/cancel/d64 等）的 measured 比例，且带完整 oracle 与 A/A；
- 受控的机制模型 + 生产路径映射（仅当无法直接采样时，且必须说明映射假设）。

**不接受**：阶段容器（`progress/wait`、`publication`、`terminal/release`）、族级 mutex 线索（~9–11%）、inclusive 份额（`__poll` 24.93% 等）、无候选专属分母的 “payload/FS” 桶、无生产读者的测试计数。上述只能作**定位线索**，不能作准入上界。**M-62 的 4.13% 属于 publication 阶段容器，不是候选上界；M-62 的候选级可删量是 `NOT_OBSERVABLE`。**

### 1.3 A/A 分辨率与匹配形态（本会话复算）

以下数字来自本会话对归档原始数据的只读复算（命令与输出见 §7），与 `AA-CALIBRATION*.md` 的发布值一致：

| 格 | 匹配形态 | 指标 | n | SD (pp) | hw (%) | MDE80 (%) | 状态 | 本战役用途（修订） |
|---|---|---|---:|---:|---:|---:|---|---|
| C1 | `sluice-copy 4k/d4/w1`（P-E） | CPU | 16 | 0.786 | 0.365 | 0.589 | **QUIET** | **P-E 的匹配格**；2% 需 4/5；36 对 hw 0.266%/MDE80 0.378% |
| C1 | 同上 | cycles | 16 | 2.346 | 1.121 | 1.758 | **QUIET** | P-E cycles；2% 需 13/24 |
| C6 | `W02-uring-4k-d1`（P-B） | CPU | 16 | 0.997 | 0.471 | 0.747 | **ORDER_SENSITIVE（仅 wall）** | **P-B 的匹配格**；但 P-B 的 W3 族 `NOT_OBSERVABLE`，不能当 W3 成本 |
| C6 | 同上 | cycles | 16 | 2.876 | 1.368 | 2.155 | **ORDER_SENSITIVE（仅 wall）** | P-B cycles；同上，W3 族不可用 |
| C3b | `W02-uring-4k-d16`（P-A） | cycles | 16 | 1.319 | 0.621 | 0.988 | **ENVIRONMENT_BLOCKED（CPU/wall）；cycles 条件带** | **P-A / M-53 d16 的匹配格**；2% 需 6/10；36 对 hw 0.446%/MDE80 0.634%；用前 cycles-only A/A |
| C4 | `W03-uring-4k-d16`（P-C） | CPU | 16 | 3.019 | 1.402 | 2.262 | **QUALIFIED（≥~2.3% CPU）** | **P-C 的匹配格**；2% 需 20/38（hw≤1% 需 38）；36 对 hw 1.021%/MDE80 1.450%；C4 cycles MDE80 6.088% 不可用于 1% 声明 |
| C5b | `W02-threadpool-4k-d1-w1` | CPU | 16 | 0.964 | 0.458 | 0.723 | **QUIET（条件基线）** | **不是任何 P2 W3 形态的匹配格**；P2 无 W3 形态在 TP d1 w1。仅作 TP d1 w1 基线（2% 需 5/7）；若 W3 要在 TP d1 w1 上做，必须另做该形态 profile + A/A |
| C5b | 同上 | cycles | 16 | 3.107 | 1.481 | 2.328 | **不作窄带** | 不用于 W1/W3 |
| C7 | `W02-uring-4k-d64` | cycles | 16 | 1.889 | 0.893 | 1.416 | **ENVIRONMENT_BLOCKED（CPU/wall）；cycles 条件带** | **M-53 d64 的匹配格**；2% 需 10/17；1% 需 30/58；17–20 只对应 δ=2%；条件带，用前 cycles-only A/A |
| C7 | 同上 | CPU | 16 | 9.782 | 4.573 | 7.331 | **ENVIRONMENT_BLOCKED；A/A 均值不正** | 不用于 M-53 |
| C2b | `W02-threadpool-4k-d16-w4` | CPU | 16 | 3.414 | 1.604 | 2.558 | **NOISY** | **P-D 的匹配格；NOISY，无可用带**；W3 在 P-D 上必须先重跑 C2b A/A（或改测已测形态），不能直接排 A/B |
| C8 | `W02-threadpool-4k-d64-w4` | CPU | 16 | 4.938 | 2.323 | 3.700 | **NOISY** | 不用于 M-53 |

来源：`p1/AA-CALIBRATION.md:384-402`；`p1/AA-CALIBRATION-2.md:393-419`；本会话 `tools/recompute_independent.py --aa-root p1/aa|p1/aa2` 复算（§7）。

> **匹配规则（修订）**：上表 hw 是**该格 n=16 的 bootstrap 95% CI 半宽**；MDE80 是该格 n=16 的 80% 功效最小效应；A/B 用别的 n 时用 SD 重算，不要直接搬 hw。**C5b 的带只适用于 C5b（TP d1 w1），不能搬到 d16/d64 或其他后端**（`AA-CALIBRATION-2.md:413-416`）。P-D 的匹配格是 C2b（NOISY），P-C 是 C4（CPU），P-E 是 C1（CPU），P-A 是 C3b（cycles），P-B 是 C6（但族级不可观测）。

### 1.4 预算内的 required-n（本会话算术）

| 格/指标 | δ=5% n80/n_half | δ=2% | δ=1.2% | δ=1% | 36 对 MDE80 / hw(t) |
|---|---:|---:|---:|---:|---:|
| C6 CPU | 3/3 | 5/7 | **8/14** | 10/18 | 0.479% / 0.337% |
| C6 cycles | 5/8 | 19/35 | **48/91** | 67/130 | 1.382% / 0.973% |
| C1 CPU | 3/3 | **4/5** | **6/10** | 7/12 | 0.378% / 0.266% |
| C1 cycles | 4/6 | 13/24 | **32/62** | 46/88 | 1.127% / 0.794% |
| C5b CPU | 3/3 | **5/7** | **8/13** | 10/17 | 0.463% / 0.326% |
| C3b cycles | 3/4 | **6/10** | **12/22** | 16/30 | 0.634% / 0.446% |
| C7 cycles | 4/5 | **10/17** | 22/41 | 30/58 | 0.907% / 0.639% |
| C4 CPU | 6/9 | **20/38** | 52/100 | 74/143 | 1.450% / 1.021% |

读法：`n80` = 80% 功效所需对数；`n_half` = 95% CI 半宽 ≤ δ/2 所需对数。**这些数字只回答“若效应是 X 需要多少对”**；它们不证明任何候选的效应是 X。

> **修订注（M1）**：`C7 cycles` 的 17–20 对只对应 **δ=2%**；δ=1% 需要 58 对。若 d64 真实效应接近 d16 的 0.406%（P-A）/0.285%（P-C），`hw≤效应/2` 需要 **336/678** 对（80% 功效需 **172/347** 对），17–20 对不成立。0.406%/0.285% 是 leaf self 份额，不是 A/B 效应。

### 1.5 历史反例的效力

- **M-61 `begin`/`complete`**：r6 R3 已给出 0.45% < 1% MME 的候选级拒绝（`CAMPAIGN-LEDGER.md:222`）。P2 的 publication 阶段 4.13% 是容器，测量范围不同，不能推翻 0.45% 的候选级结果（`p2/P2-COST-MAP-R1.md:598`）。
- **M-28**：`NO_GO_ABORT`，预注册中止通道判词 1b/1c 已落地（`CAMPAIGN-LEDGER.md:207,235-239`）。仅“再测一次”或“换 workload”不构成翻案；翻案必须同时满足 §4.1 四要素。
- **M-42**：`GO` 只是排队，不是已准入性能门；历史 `NOT_POWERED` 结论未被 P2 的 `lock addq` 算术推翻（`CAMPAIGN-LEDGER.md:217,247`；`p2/P2-COST-MAP-R1.md:513-537`）。

---

## 2. W1–W6 看板更新

| 工作包 | 名称 | 阶段 | 终判/当前裁决 | 一句话说明与下一步 |
|---|---|---|---|---|
| **W1** | ProgressSource poll 等待路径 | **owner 点开放** | **`SEMANTIC_MISMATCH`（D1/D2/D3/C1）/ `NEEDS_OWNER`（C2）/ `NO_CHANGE_JUSTIFIED`（hygiene）** | D1/D2/D3/C1 语义阻断；C2 契约允许 condvar，但需 owner 授权第二通知通道；A1/A2 不 material；`#604` G0 未过；等 O-1/O-2/O-3，不启动 A/B |
| **W2** | 通知/drain/rearm | **终判** | **`NOT_APPLICABLE`** | `#635` 已合并；无新增候选；M-67 按 ledger §5.2 路由到 W3，不是 W2 新候选；未来改动保持 9 用例语义 oracle + mutant + 模型 |
| **W3** | RequestCore 行级族 | **语义验证中** | **`ADMIT_TO_DESIGN`** | 先做行级 measured 归因（按 P-D/P-C/P-E 主形态）；M-61 R3 反例未推翻；A/A 必须按形态匹配（P-D 的 C2b 是 NOISY）；预算 M |
| **W4** | UringAsyncBackend | **语义验证中** | **`ADMIT_TO_DESIGN`** | M-53 先证三项迁移义务 + d64 cycles-only A/A（17–20 仅 δ=2%）；M-40 设计门；M-63/M-65 owed 家族设计门；预算 M/L |
| **W5** | ThreadPool owed-count | **终判** | **`NOT_APPLICABLE`** | `#634` 已合并；任何改动重跑 parity 8 用例 + 4 mutants + 完整矩阵 |
| **W6** | test-only 计数 / owed 计数 | **终判** | **M-42 `NOT_POWERED`；M-28 `NO_GO_ABORT`（不重做）；M-40 转 W4** | M-42 的 `GO` 是排队，不是 A/B 准入；隔离格 + 动态回执带 + 机制门 + mutant kill + release 无 seam 符号前不启动。M-28 不是 `NOT_APPLICABLE` |

**看板状态说明（修订，L1）**：本战役没有工作包进入 `已准入`、`A/B 测量中` 或 `独立复核中`。W3/W4 在 `语义验证中`（设计/资格阶段）；W2/W5/W6 已到 `终判`；**W1 不是终判，而是 `owner 点开放`（O-1/O-2/O-3）**，其中 O-2 可以改变 C2/D 的结论。

---

## 3. 逐候选矩阵

### 3.1 W1 方向

| 方向 | 裁决 | 证据 | 缺什么 / 翻案条件 | 下一步 | 预算 |
|---|---|---|---|---|---|
| **W1-D1/D2/D3 阻塞 eventfd read** | **`SEMANTIC_MISMATCH`** | `p3/W1-DESIGN.md:267-324`；D1 `os.dup`+`F_SETFL` 输出 `after clear on dup: original False dup False`（`:275-293`）；D2 非阻塞 ack/drain、有界 timeout、PROG-02 第 2 条（`:298-306`）；D3 io_uring 单 eventfd 注册（`src/async/uring_backend.cpp:273-278`；`include/sluice/async/detail/progress_source.hpp:539-545` 禁止 dup/store） | 不是性能缺口；要 material 等待路径必须改通知拓扑/模型/第二权威，需 owner 批准 | 不实现；若 owner 要追，另立拓扑 PR，归 `#617` G1 或新模型 | 0 A/B |
| **W1-C1 第二等待 fd** | **`SEMANTIC_MISMATCH`** | `p3/W1-DESIGN.md:236-243`；kernel 只唤醒被注册 fd；host/owner 必有一方收不到 CQE；relay/第二权威违反 PROG-01/02 | 同上 | 不实现 | 0 |
| **W1-C2 TP condvar + protected predicate** | **`NEEDS_OWNER`** | `p3/W1-DESIGN.md:245-256`；PROG-02 原则允许 condvar（`docs/explicit-io-contract.md:907-912`），但需要双通道、新模型、新 oracle；eventfd 仍是 host pollable handle | **owner 是否授权第二通知通道**；模型/协议/oracle/机制门七问。不是契约硬冲突，因此不计入 `SEMANTIC_MISMATCH` | 先 owner；若批准，按 §6 的 M/L 设计包执行 | M/L |
| **W1-A1/A2/A3 hygiene** | **`NO_CHANGE_JUSTIFIED`** | `p3/W1-DESIGN.md:142-170,200-214`；合并入口两次 `access_mtx_`、health 并入 fused block、deadline 算术；只删 1–2 次 mutex/少量 clock/int，不删 syscall/kernel work | material 证据；预计低于 C6/C1 MDE（C6 CPU 0.747%、C1 CPU 0.589%，16 对） | 可选代码面 hygiene PR；不得开 A/B 声称收益 | S（代码面） |
| **W1-A4/A4' probe 降频** | **`SEMANTIC_MISMATCH`（全局）/ `NO_CHANGE_JUSTIFIED`（TP-only）** | `p3/W1-DESIGN.md:172-177`；probe 是 io_uring kernel writer 的兜底（CQE 不 bump epoch）；TP-only 跳过需新能力事实 | TP-only 的能力事实与机制门；收益仅一个虚调用 | 不全局降频；TP-only 仅 hygiene | S |
| **W1-A5–A8 删除重验/drain/P3/后检查** | **`SEMANTIC_MISMATCH`** | `p3/W1-DESIGN.md:179-198`；PROG-02 第 2/4 条；model `OL4a`/`OP3`/`MutParkDrain` | 无；删除即丢唤醒/破坏 control sticky | 不做 | 0 |
| **W1-B 有界等待算术** | **`NO_CHANGE_JUSTIFIED`** | `p3/W1-DESIGN.md:218-228`；poll 仍在；P-B 本身是 20 s 有界等待（`bench/bench_w02_pipeline.cpp:52`）；只删 clock/int | material 效应；不能删 poll/kernel wait | 不单独作为候选 | 0 |

### 3.2 W3 行级候选（4 行）

> **W3 门禁（适用于任何跨方法临界区合并）**：必须先给出 measured 行级归因；**任何跨方法临界区合并必须证明 `publication_inflight`/slot reuse/observer 注册不被改变**；机制门七问 + 单因素 A/B（`CAMPAIGN-LEDGER.md:259`）。M-67 若走融合查询同样适用本门禁。

| 候选 | `#637` 原终态 | P3 裁决（修订） | 证据 | 缺什么 / 翻案条件 | 下一步 | 预算 |
|---|---|---|---|---|---|---|
| **M-61 `begin`/`complete` 两锁融合** | `INCONCLUSIVE` | **`INCONCLUSIVE`**（原判未推翻） | `p2/P2-COST-MAP-R1.md:486-497`；publication 1.46/0.07/2.82/4.13/3.30% 是阶段容器；`claim_execution` leaf 93/93 已归 publication；r6 R3 0.45% < 1% MME 拒绝（`CAMPAIGN-LEDGER.md:222`） | 新 measured profile 推翻 R3（行级分离 + `publication_inflight` 反例处理 + **确定性测试注入点**）；保步间语义的融合单因素 A/B（**族 A/A 先行**） | 新 profile + 语义映射；不启动 A/B | M |
| **M-62 无观察者跳过 claim/retire** | `INCONCLUSIVE` | **`INCONCLUSIVE` + design work**（`ADMIT_TO_DESIGN` 仅指设计/资格工作） | `p2/P2-COST-MAP-R1.md:486-497`；P-D publication **4.13% 阶段容器**，**不是候选上界**；claim leaf 0.61% 内含；4.74% 双计已删；P-B 的 0.07% publication 是阶段容器且受用户栈截断影响（90.67% app 无 bench 帧，`p2/P2-REVIEW.md:54-71`） | 行级分离（候选级可删量 `NOT_OBSERVABLE`）；publication exactly-once/observer 注册协议重证；**任何跨方法临界区合并证明 `publication_inflight`/slot reuse/observer 注册不被改变**；单因素 A/B | 行级归因 + 协议证明；然后 A/B | M |
| **M-67 sticky 双标志原子化/融合查询** | `INCONCLUSIVE` | **`INCONCLUSIVE` + design work**（`ADMIT_TO_DESIGN` 仅指设计/资格工作） | `p2/P2-COST-MAP-R1.md:491-497`；progress/wait 25.09%+9.16% P-B 含 2,057 个 worker-eventfd；sticky flags 无独立可采样符号（`p2/P2-REVIEW.md:128-142`） | 行级 profile 分离；A/A 分辨率 ≥1% MME；ProgressSource 单读者协议重证；若跨方法合并临界区，同样证明 `publication_inflight`/slot reuse/observer 注册不变 | 行级 profile + cycles-only A/A + 协议 | M |
| **M-10 零长发布三锁融合** | `INCONCLUSIVE` | **`INCONCLUSIVE` + design work**（`ADMIT_TO_DESIGN` 仅指设计/资格工作） | `p2/P2-COST-MAP-R1.md:492-497`；zero-op 不在 W-02/W-03 workload；M-10 `NOT_OBSERVABLE`（`p2/P2-COST-MAP-R1.md:492`） | zero-op 占比测量（T9）；步间语义映射；**任何跨方法临界区合并证明 `publication_inflight`/slot reuse/observer 注册不被改变**；融合单因素 A/B | T9 zero-op 协议 + 测量；然后 A/B | M |

### 3.3 W4/W6 候选（6 行）

| 候选 | `#637` 原终态 | P3 裁决（修订） | 证据 | 缺什么 / 翻案条件 | 下一步 | 预算 |
|---|---|---|---|---|---|---|
| **M-53 cookie O(R) → `user_data` 路由索引** | `INCONCLUSIVE` | **`ADMIT_TO_DESIGN`** | d16 leaf self 34/8,369=0.406%（P-A）、47/16,472=0.285%（P-C），≈61.3/58.0 cyc/op（`p2/P2-COST-MAP-R1.md:499-511`）；d64 C7 cycles SD 1.889、16 对 `t-hw=1.007%` > 1.0%，需 17–20 对（δ=2%；`p1/AA-CALIBRATION-2.md:485-506`）；三项迁移义务未证（`CAMPAIGN-LEDGER.md:211`） | **17–20 只对应 δ=2%**；δ=1% 的 A/A 与 A/B 各 58 对；若 d64 效应接近 d16 的 0.406%/0.285%，`hw≤效应/2` 需 336/678 对（80% 功效 172/347 对）。另需：TransportLedger 物理位置单调；控制 CQE exact-submitted；cancel 定向；d64 profile | 先证三项义务 + d64 A/A；再按 δ 选 17–20（δ=2%）或 58（δ=1%）A/B；0.4% 量级需重估预算 | M/L |
| **M-40 `live_cookies_`/`live_control_sqes_` 原子冗余** | `INCONCLUSIVE`（r2 由 `NO_GO` 改判） | **`INCONCLUSIVE` + design work**（`ADMIT_TO_DESIGN` 仅指设计/资格工作） | `CAMPAIGN-LEDGER.md:218,262`；P2 未单独观测 live counters（inline 原子无可采样符号）；需要行级归因 + 生产读者角色 + `dispatch_mtx_` 覆盖重证 + 缝读者加锁设计 | 行级 profile；派生摘要/机制门作答；`dispatch_mtx_` 覆盖证明；缝读者锁设计 | 设计审计 + 行级 profile；然后 A/B | M |
| **M-63 uring owed sweep 逐槽锁 → 单锁快照** | `INCONCLUSIVE` | **`INCONCLUSIVE` + design work**（`ADMIT_TO_DESIGN` 仅指设计/资格工作） | `CAMPAIGN-LEDGER.md:212,274`；P2 无 zero-op/owed workload；owed 机制本体 KEEP；交付顺序不变性未证 | 零长/owed 协议激活（T9）；单锁快照的顺序/原子性设计 | T9 owed 协议 + 融合设计；然后 A/B | M |
| **M-65 owed `release_control` 第三锁融合** | `INCONCLUSIVE` | **`INCONCLUSIVE` + design work**（`ADMIT_TO_DESIGN` 仅指设计/资格工作） | `p2/P2-COST-MAP-R1.md:490-497`；terminal/release 3.49% P-D 容器；P2 无 zero-op/owed workload；owed 机制本体 KEEP（`CAMPAIGN-LEDGER.md:224`） | zero-op/owed 协议（T9）；第三锁与 retire 融合的步间语义；交付顺序不变性；**任何跨方法临界区合并证明 `publication_inflight`/slot reuse/observer 注册不被改变** | **W4 owed 家族**；T9 owed 协议 + 设计；然后 A/B | M |
| **M-42 `submit_flushes_` 只写不读原子计数** | `GO`（排队，未启动） | **`NOT_POWERED`** | `src/async/uring_transport.cpp:222` 的 `fetch_add` 编译为 `lock addq $0x1,0x120(%rdi)`（objdump `4241b7`，`p2/P2-COST-MAP-R1.md:518-523`）；19–20 cycles → P-A 0.126–0.133%、P-B 0.066–0.070%、P-C 0.093–0.098% cycles；1 instr 0.0041–0.0094%；P-D/P-E 的 workload 不执行（P-D 仅 1 次启动残差，`p2/P2-COST-MAP-R1.md:513-537`） | measured hotspot；隔离格（submit-only）+ **动态回执带（不能只做静态三列）** + 机制门七问；**mutant kill**；**release 预处理与 `nm` 无 seam 符号**；**出现生产读者 → `NO_GO`/重新物化**；`GO` 排队 ≠ A/B 准入 | owner 决定是否资助隔离格；否则保持 `NOT_POWERED` | S（A/B）+ 隔离格 |
| **M-28 `syscall_count_` 宏门控剔除** | `NO_GO_ABORT` | **`NO_GO_ABORT`（本战役不重做）** | `CAMPAIGN-LEDGER.md:207,235-239`；预注册中止通道判词 1b/1c 已落地；`syscall_count_` 仍在生产（`src/async/threadpool_backend.cpp:434`；`include/sluice/async/detail/threadpool_backend.hpp:258`）；P2 未重开 | **四要素同时满足才可重开**：① 新证据把机制约束为可分离的“单指令差”；② 消除/隔离 TU 重编译布局变化、成员移除的对象布局变化、RMW 移除对 `work_mtx_` 竞争时序的影响；③ 新 session、新冻结；④ 动态回执自带接受带与隔离格判据 | 不启动；仅四要素同时满足才重开 | 0 |

### 3.4 `#637` 其余候选（15 行）

| 候选 | `#637` 原终态 | P3 裁决（修订） | 证据 | 缺什么 / 翻案条件 | 下一步 | 预算 |
|---|---|---|---|---|---|---|
| **M-80 `write_all_at` → `prevalidated_pwrite_step`** | `INCONCLUSIVE` | **`INCONCLUSIVE`** | `CAMPAIGN-LEDGER.md:208,280`；P2 `payload/FS` 10.44–19.87% 不是候选专属；无直呼组合写的 measured hotspot | 直呼组合写形态的 measured hotspot；“compose 仅由已校验入口进入”不变量 | 定向 profile + 不变量证明；然后 A/B | M |
| **M-16 uring 后端层 state-op 校验收敛** | `INCONCLUSIVE` | **`SEMANTIC_MISMATCH`** | `CAMPAIGN-LEDGER.md:209,282`；owner seam 冲突未裁决 | owner 裁决 seam；隔离 backend state-op 半边 | 先 owner；再隔离 A/B | S/M |
| **M-37 `remove_exact(front)` → `pop_front`** | `INCONCLUSIVE`（`#595`） | **`INCONCLUSIVE`（`#595` 归属；本战役不启动）** | `CAMPAIGN-LEDGER.md:210,279`；T4/T8 不在 G0 冻结集；无深队列 `remove_exact` 的 memmove 份额 | 深队列派发形态的 memmove 份额，或证明生产全形态队列深 ≤1（→ `NO_GO`） | `#595` 深队列 profile；否则塌缩 `NO_GO` | M（`#595`） |
| **M-01 scope O(S) 扫描，H3 开放** | `INCONCLUSIVE` | **`INCONCLUSIVE`** | `CAMPAIGN-LEDGER.md:220,281`；无 `-scope64` 高重复隔离 profile；free-list 是否重开属 owner（S2） | 隔离协议剔除驱动污染；机制可归因份额；owner S2 | 高重复 `-scope64` 隔离 profile；owner S2 | M |
| **M-33 TP cancel ring 摘除惰性失效** | `INCONCLUSIVE` | **`INCONCLUSIVE` + design work**（`ADMIT_TO_DESIGN` 仅指设计/资格工作） | `CAMPAIGN-LEDGER.md:226,276`；W-02/W-03 不主动 cancel；T9-cancel 未构造 | cancel 形态 measured hotspot；失效机制设计（两语义保持）；机制正当性门 | T9 cancel 协议 + 设计；然后 A/B | M |
| **M-74 `ready()` 快路径合并** | `INCONCLUSIVE` | **`INCONCLUSIVE` + design work**（`ADMIT_TO_DESIGN` 仅指设计/资格工作） | `CAMPAIGN-LEDGER.md:227,277`；ready 概率未分层；与 zero-op 子单元绑定 | ready 概率分层测量；合并收益隔离证据 | T9 ready/zero-op 测量 + 设计 | S/M |
| **M-09 `Slot::borrow` 只写不读** | `SEMANTIC_MISMATCH` | **`SEMANTIC_MISMATCH`** | `CAMPAIGN-LEDGER.md:213,283`；root 的 borrow facts 义务 | owner 裁决：修 root 摘除 borrow facts（→ 删除 PR）或出现真实消费者（→ KEEP） | owner；代码面 PR | S（无性能门） |
| **M-30 `zero_effect_cancel` 零生产者** | `SEMANTIC_MISMATCH` | **`SEMANTIC_MISMATCH`** | `CAMPAIGN-LEDGER.md:214,283`；CANCEL-01 seam 义务 | owner 裁决 seam 义务（KEEP）或修 root 去掉冗余第二强制点 | owner；代码面 PR | S |
| **M-57 `acquire_execution` 零调用** | `SEMANTIC_MISMATCH` | **`SEMANTIC_MISMATCH`** | `CAMPAIGN-LEDGER.md:215,283`；BACKEND-01 seam | owner 裁决 seam 必要性；若可删走代码面收缩 PR | owner；代码面 PR | S |
| **M-12 `close_admission` 双实现** | `SEMANTIC_MISMATCH` | **`SEMANTIC_MISMATCH`** | `CAMPAIGN-LEDGER.md:216,283`；双实现 | owner 指定存活实现 + 测试 oracle 迁移方案 | owner；代码面 PR | S |
| **M-61 `PublicationPayload` 快照仅测试可达** | `NO_GO` | **`NO_GO`** | `CAMPAIGN-LEDGER.md:219,284`；无生产 payload 消费者 | 生产消费者出现（→ 重新物化）或 owner 批准测试专用通道（卫生路线） | 不复活为性能候选；卫生路线另议 | N/A |
| **M-80 hygiene `file_semantics` 模型词表位置** | `SEMANTIC_MISMATCH` | **`SEMANTIC_MISMATCH`** | `CAMPAIGN-LEDGER.md:228,283`；词表位置与测试面 | owner 裁决词表移入测试面（保持共享语义 oracle）或出现生产消费者 | owner；代码面 PR | S |
| **H1 提交批形态** | `NO_GO` | **`NO_GO`** | `CAMPAIGN-LEDGER.md:229,285`；P2 现状 `io_uring_enter` 1/op、submit 97–99% kernel、ring flags=0（`p2/P2-COST-MAP-R1.md:539-542`） | `#595` RQ1 second-held-entry guard + payload gate + D1/D2/D3、64 KiB、W-03、cancel falsifier 家族 + 整机重测 | `#595` 重开门前不启动 | N/A（`#595`） |
| **io-wq punt FS 条件性** | `NO_GO` | **`NO_GO`** | `CAMPAIGN-LEDGER.md:230,285`；P2 tmpfs `queue_async_work`=1/op、stage 6.02–10.23%（`p2/P2-COST-MAP-R1.md:539-542`）；非 Sluice 代码 | 无翻案路径；换设备/FS 的产品准入属既列重测情形 | `#604`/`#617`；不包装成跨 FS | N/A |
| **tp-w8 `work_mtx_` 竞争（worker 超配）** | `NO_GO` | **`NO_GO`** | `CAMPAIGN-LEDGER.md:231,285`；`#604` 配置/工作负载面；唯一减法交集 M-28 已 `NO_GO_ABORT` | 出现 worker 数被外部固定的 root 工作负载 + 最小机制提案过机制正当性门 | 单独预注册；本战役不启动 | N/A（`#604`） |

---

## 4. 关键裁决理由

### 4.1 W1：为什么不是 `ADMIT_TO_DESIGN`，也不是“还没测”

**material 方向分两类。** W1 的历史线索是“`poll(-1)` 可由同一 eventfd 的阻塞 read 取代，约 5.0–5.5% cycles 可删”（W1-DESIGN 引用的 R1 工作稿线索；未跟踪、已被 R2 r6 取代，`p3/W1-DESIGN.md:324`）。W1-DESIGN 逐一检查了 A/B/C/D 四个方向（细分见 §3.1）：

- **D1（同 fd + dup + `F_SETFL`）**：`dup` 共享 open-file description 与文件状态标志；本会话用 eventfd 复现 `after clear on dup: original False dup False`（`p3/W1-DESIGN.md:275-293`）。源码只创建一次 `eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC)`（`include/sluice/async/detail/progress_source.hpp:74-79`），且 `BackendProgressPort` 明确禁止 backend dup/store fd（`:539-545`）。判 `SEMANTIC_MISMATCH`。
- **D2（同 fd 直接改 blocking）**：`drain_notification_nolock_`（`:414-460`）在无 readiness 时 read 会永久阻塞；`acknowledge_notification` 与非阻塞 ack/drain 依赖 `EFD_NONBLOCK`；有界等待（P-B 的 20 s，`bench/bench_w02_pipeline.cpp:52`）需要 timeout；PROG-02 要求 stale readiness 在最终 actionable-work check 之前确认，blocking read 是“park 时消费”。判 `SEMANTIC_MISMATCH`。
- **D3（独立 blocking fd）**：io_uring 只把 kernel CQE readiness 写给被注册的一个 fd（`src/async/uring_backend.cpp:273-278`）；host 的 pollable handle（PROG-03）与 owner 的 blocking wait fd 若不同，必有一方收不到 CQE；relay/第二通知权威违反 PROG-01/02。判 `SEMANTIC_MISMATCH`。
- **C2（TP condvar + predicate）**：契约原则允许（`docs/explicit-io-contract.md:907-912`），但这是双通知通道/新模型/新 oracle，且 host 仍要 eventfd；不是 W1 的最小 B。判 **`NEEDS_OWNER`**（契约允许，但需 owner 授权第二通知通道；不是契约硬冲突）。**这是 M6 的修订：C2 不再计入 `SEMANTIC_MISMATCH`。**

**语义保持的 A1/A2 不 material。** 合并 `wait_one` 入口两次 `access_mtx_`、把 health 检查并入 fused block，只删每 park 约 1–2 次 mutex 获取；不删 syscall、不删 kernel work（`p3/W1-DESIGN.md:142-170`）。16 对 C6 CPU MDE80 0.747%、C1 CPU 0.589%；36 对约 0.479%/0.378%。这些锁操作的效应预计低于噪声带，不能支撑 A/B 声明（`:407`）。判 `NO_CHANGE_JUSTIFIED`。

**profile 没有给出可删量。** P-B `__poll` inclusive 24.93%、poll boundary 9.16%、progress/wait 25.09%；P-E `__poll` 15.83%、poll boundary 4.38%、progress/wait 24.36%（`p2/P2-COST-MAP-R1.md:451-470`；本会话复算一致）。但：

- `__poll` 是 inclusive 栈份额，不是可删 cycles；poll boundary 是“只有 libc `__poll` 帧”的边界样本，不是收益；
- P-B 的 progress/wait 含 2,057 个 worker-eventfd 样本、3,572 个 app-poll、297 个 poll+progress-pass；P-E 含 3,972 个 poll+progress-pass、3,537 个 progress-pass、952 个 eventfd；
- P-B 的 `__poll` 6,129 个样本中 5,664 个没有 `wait_one` 帧，873 个是 4 帧栈；90.67% app 样本没有 bench 帧（`p2/P2-REVIEW.md:54-71`；本会话复算 `app_frames` 12,246/11,104/1,142）。

因此 W1 **没有**候选级 measured hotspot，**也没有**真实消费者 profile；`#604` 仍是 `HOLD_NO_REAL_APPLICATION`，`sluice-copy` 是参考应用（`CAMPAIGN-LEDGER.md:305-315`）。P2-REVIEW M10 明确：连 ledger 的 profile 门也未过（`p2/P2-REVIEW.md:239-247`）。

**如果 owner 接受 bench/reference 代理**，匹配格是 **C6/C1**，不是 C5b/C3b/C7（`p2/P2-REVIEW.md:75-92`）。即使按 1.2% 过程级效应，C6 cycles 需要 48/91 对、C1 cycles 32/62 对；36 对不够 C6 的 hw≤0.6%。但这不是当前 W1 的 A/B 路径，因为 material 方向本身不合法或需 owner 授权。

### 4.2 W3：为什么只到 `ADMIT_TO_DESIGN`

**族级线索真实，但单候选不可见。** P2 的 W3 族级 self 为 P-A 6.00%、P-B 0.24%、P-C 15.82%、P-D 22.43%、P-E 13.07%（`p2/P2-COST-MAP-R1.md:474-482`；本会话复算 `w3_family` 一致）。但这些是 **admission + terminal/release + publication + observation** 的阶段容器：

- P-D 的 22.43% 中，admission 12.76%、terminal 3.49%、publication 4.13%、observation 2.05%；
- **M-62 的 4.13% 是 publication 阶段容器，不是候选上界。** `claim_execution` leaf 93/93 已归 publication；4.74% 的双计已删（`p2/P2-COST-MAP-R1.md:486-495`；`p2/P2-REVIEW.md:116-143`）。M-62 的候选级可删量仍 `NOT_OBSERVABLE`。
- P-B 的 0.24% 建立在 1,142 个可见 bench 帧样本上；11,104/12,246（90.67%）app 样本没有 bench 帧，`wait_one` 只可见 700 个，5,664/6,129 个 `__poll` 没有 `wait_one`（`p2/P2-REVIEW.md:54-71`）。因此 P-B 的 W3 族、product leaf、`wait_one` 份额全部 `NOT_OBSERVABLE`，不能用“严重内联”解释。

**W3 的 A/A 必须按形态匹配（H1 修订）。** `P2` 没有 W3 形态在 TP d1 w1 上采集；C5b 不是 P-D/P-C/P-E/P-A/P-B 的匹配格。按形态列：

| W3 形态 | W3 族级 self | 匹配 A/A 格 | 2% 所需 n80/n_half | 可用性结论 |
|---|---:|---|---:|---|
| **P-D** TP d16 w4 | 22.43% | **C2b CPU** | —（NOISY，无可用带；16 对 SD 3.414、hw 1.604%、MDE80 2.558%） | **无可用带**；W3 不能在 P-D 上排 A/B，必须先重跑 C2b A/A 或改测已测形态 |
| **P-C** W-03 d16 | 15.82% | **C4 CPU** | **20/38** | CPU 可用（QUALIFIED ≥~2.3%）；36 对 hw 1.021% > 1%，**hw≤1% 需 38 对**；C4 cycles MDE80 6.088% 不可用于 1% 声明 |
| **P-E** copy 4k/d4/w1 | 13.07% | **C1 CPU** | **4/5** | 可用（QUIET）；C1 cycles 2% 需 13/24 |
| **P-A** uring d16（补充行） | 6.00% | **C3b cycles** | **6/10** | 条件带（ENVIRONMENT_BLOCKED；cycles-only A/A 先行）；36 对 hw 0.446%/MDE80 0.634% |
| **P-B** uring d1 | 0.24% | C6（名义） | — | 族级 `NOT_OBSERVABLE`（90.67% app 样本无 bench 帧）；不能当 W3 成本 |

**“36 对对 2% 足够”只对 C5b/C3b/C7 成立**：C5b CPU 2% 需 5/7、C3b cycles 2% 需 6/10、C7 cycles 2% 需 10/17。P-D 无带；P-C 的 C4 hw≤1% 需 38 对（36 对差 2 对）；P-E 的 C1 CPU 2% 需 4/5（36 对足够，但这是 P-E 的格）。C5b 本身是 TP d1 w1 的条件基线（背景负载告警），**不是任何 P2 W3 形态的匹配格**；若 W3 要在 TP d1 w1 上做，必须另做该形态 profile 与 A/A。

**阶段容器不是候选上界。** M-61/M-62/M-65/M-67 的数字都来自 publication/terminal/progress-wait 容器；M-10 的 zero-op 根本不在 W-02/W-03 workload 中（`p2/P2-COST-MAP-R1.md:492-497`）。W1-DESIGN 与 P2-REVIEW 都明确：这些容器不能回答“删掉两把锁/一个标志能省多少”。

**M-61 有历史反例。** r6 R3 的候选级测量是 0.45% < 1% MME，已拒绝（`CAMPAIGN-LEDGER.md:222`）。P2 的 publication 4.13% 是不同测量范围的容器，不能推翻它（`p2/P2-COST-MAP-R1.md:598`）。因此 M-61 保持 `INCONCLUSIVE`，需要新的 measured profile（含 `publication_inflight` 反例与确定性测试注入点）和族 A/A 先行，而不是直接进入 A/B。

### 4.3 M-53：d16 小热点、d64 边缘、迁移义务未证

**d16 有候选级热点，但低于 d16 的分辨率。** `find_live_router_cookie_` leaf self：P-A 34/8,369=0.406%（Wilson 95% CI 0.291–0.567%）、P-C 47/16,472=0.285%（0.215–0.379%）；≈61.3/58.0 cycles/op（`p2/P2-COST-MAP-R1.md:499-511`；本会话复算 `m53_router` 一致）。d1/TP/copy 为 0。**0.406%/0.285% 是 leaf self 份额，不是 A/B 效应。** P-A（uring d16）的匹配格是 C3b cycles：0.406% 的份额在 36 对上 MDE80≈0.633%，仍低于分辨率；16 对 MDE80≈0.988% 更不够。**P-C（W-03 d16）的 0.285% 不能用 C3b 的 0.633% 覆盖**（C3b 是 P-A 的格，P-C 匹配 C4）；在 C4 CPU 上，36 对 hw 1.021%/MDE80 1.450%，16 对 MDE80 2.262%，0.285% 远低于分辨率。两种形态的结论都是低于分辨率，但匹配格必须分开引用。`61.3/58.0 cycles/op` 是 leaf share × PMU cycles/op，PMU/record 分母不一致、无 PEBS/skid，置信 MEDIUM/LOW（`p2/P2-REVIEW.md:162-174`）。

**d64 首次有 A/A，但边缘。** C7（`W02-uring-4k-d64`）cycles SD 1.889、bootstrap hw 0.893%、**t-hw 1.007%**、MDE80 1.416%；对 δ=2%，`n_half=17`，16 对在保守 t 判据下差一对；1% 需 58 对（`p1/AA-CALIBRATION-2.md:485-506`）。C7 的 CPU/wall 被 `ENVIRONMENT_BLOCKED` 且 A/A 均值不正（CPU +5.04%），不能用于 M-53。

**预算必须按 δ 分开写（M1 修订）：**

- **δ=2%**：A/A 17–20 对（16 对 t-hw 1.007%，17 对 0.971%，20 对 0.884%）；A/B 17–20 对。
- **δ=1%**：A/B 58 对（80% 只需 30 对），但 **A/A 不能用 17–20**：17 对 hw 0.971%、20 对 0.884%，均 >0.5%；δ=1% 的 A/A 也必须 58 对（58 对 hw 0.497%）。
- **若 d64 真实效应接近 d16 的 0.406%**：`hw≤效应/2`（0.203%）需 **336** 对；80% 功效需 **172** 对。
- **若接近 0.285%**：`hw≤效应/2`（0.1425%）需 **678** 对；80% 功效需 **347** 对。
- 因此“17–20 对”不是 M-53 的通用预算，只是 δ=2% 的最小 A/A；不能把它与 δ=1% 的 58 对 A/B 混用。

**三项迁移义务仍未证。** TransportLedger 物理位置单调、控制 CQE exact-submitted、cancel 定向（`CAMPAIGN-LEDGER.md:211,260`）。这些不是性能测量，而是路由索引编码的语义前提；没有它们，`user_data` 索引可能改变 publication/cancel 的定向语义。

**结论**：M-53 是 `ADMIT_TO_DESIGN`，不是 `ADMIT_TO_AB`。预算 M/L：d64 cycles-only A/A（δ=2% 17–20；δ=1% 58；0.4% 量级 172/336）+ 三项迁移证明；之后按 δ 选择 A/B 对数。

### 4.4 M-42：`GO` 排队不等于 A/B 准入

**指令与成本。** `submit_flushes_.fetch_add(1, relaxed)` 在 `src/async/uring_transport.cpp:222`，编译为 `lock addq $0x1,0x120(%rdi)`（objdump 地址 `4241b7`，`p2/P2-COST-MAP-R1.md:518-523`）。按 19–20 cycles：P-A 0.126–0.133%、P-B 0.066–0.070%、P-C 0.093–0.098% cycles；1 条 instruction/submit 为 P-A 0.0094%、P-B 0.0041%、P-C 0.0079%；P-D/P-E 是 TP，workload 不执行该计数器（P-D 仅 1 次启动残差，`p2/P2-COST-MAP-R1.md:524-537`；本会话 `m42.forms` 复算一致）。**19–20 cycles 是 P2-REVIEW M4 引用的 Haswell L1 命中外部值；本会话只确认指令是 `lock addq`，没有重测锁延迟**（`p2/P2-REVIEW.md:146-158`；`p2/P2-COST-MAP-R1.md:524`）。

**低于噪声带。** 匹配 uring 格：C6 cycles 16 对 MDE80 2.155%、36 对 1.382%；C6 CPU 0.747%/0.479%；C3b cycles 0.988%/0.633%。0.066–0.133% 的算术上界远低于这些带；1 条 instruction 的信号更低。**W-02 上的 M-42 不可测**。

**`GO` 是什么。** `#637` 的 `GO` 是“已排队但未启动；是否启动属 owner”（`CAMPAIGN-LEDGER.md:217`；§4.2 明确 `GO` 不是“已准入性能门”，`:243-247`）。本裁决把 M-42 记为 `NOT_POWERED`：不是 `NO_GO`，但在以下门禁完成前，不应启动 A/B：

- measured hotspot；
- 隔离格（submit-only）+ **动态回执带（不能只做静态三列）** + 隔离格判据；
- 机制门七问；
- **mutant kill**；
- **release 预处理与 `nm` 证明无 seam 符号**；
- **出现生产读者 → `NO_GO`/重新物化**。

### 4.5 语义冲突批：不是性能实验

以下候选的 blocker 是 root/契约或 owner 裁决，不是“再测一次”：

- **M-09**：`Slot::borrow` 只写不读；owner 要么修 root 摘除 borrow facts，要么出现真实消费者（`CAMPAIGN-LEDGER.md:213`）。
- **M-30**：`zero_effect_cancel` 生产零生产者；CANCEL-01 seam 义务（`:214`）。
- **M-57**：`acquire_execution` 生产零调用；BACKEND-01 seam 必要性（`:215`）。
- **M-12**：`close_admission` 双实现；owner 指定存活实现 + 测试 oracle 迁移（`:216`）。
- **M-16**：uring 后端层 state-op 校验收敛；owner seam 冲突未裁决（`:209`）。按本战役规则，owner 未裁决 → `SEMANTIC_MISMATCH`，不是 `INCONCLUSIVE` 可重试项。
- **M-80 hygiene**：`file_semantics` 模型词表位置；owner 裁决（`:228`）。

这些候选即使有潜在机器工作，也不能用性能 A/B 来解决；代码面 PR 应先于任何测量。**W1-C2 不在本批**：它是 `NEEDS_OWNER`，契约原则允许 condvar，不是契约硬冲突。

### 4.6 `NO_GO`/`NO_GO_ABORT`：没有翻案

- **M-28**：**`NO_GO_ABORT`，不是 `NOT_APPLICABLE`。** `syscall_count_` 仍在生产（`src/async/threadpool_backend.cpp:434`；`include/sluice/async/detail/threadpool_backend.hpp:258`），所以不是“无适用对象”（`CAMPAIGN-LEDGER.md:512`）。性能端点 `NOT_POWERED`，预注册中止通道判词 1b/1c 已落地。翻案必须同时满足 ledger §4.1 四要素：
  1. 新证据把机制约束为可分离的“单指令差”；
  2. 消除/隔离 TU 重编译布局变化、成员移除的对象布局变化、RMW 移除对 `work_mtx_` 竞争时序的影响；
  3. 新 session、新冻结；
  4. 动态回执自带接受带与隔离格判据。

  > 注：ledger §4.1 原文编号 1–3；第 3 条包含“新 session、新冻结”与“动态回执自带接受带与隔离格判据”两个并列要素。本矩阵按四要素列示，不改变 ledger 文本。仅“再测一次”或“换 workload”不构成翻案（`CAMPAIGN-LEDGER.md:235-241`）。P2 没有提供这些，本战役不重做。
- **M-61 payload**：`NO_GO`；没有生产 payload 消费者。只有消费者出现或 owner 批准测试专用通道才重开（`:219,284`）。
- **H1**：`NO_GO`；P2 只是复现现状（`io_uring_enter` 1/op、submit 97–99% kernel、flags=0），不是 H1 的 A/B 证据；`#595` 的 RQ1 guard/payload gate/D1-D3/64K/W-03/cancel falsifier 全过才可重开（`:229`；`p2/P2-COST-MAP-R1.md:539-542`）。
- **io-wq punt**：`NO_GO`；非 Sluice 代码，tmpfs 下 1/op、FS 条件性；不能外推到磁盘/NVMe（`:230`；`p2/P2-COST-MAP-R1.md:539-542`）。
- **tp-w8**：`NO_GO`；`#604` 配置/工作负载面，没有外部固定 worker 数的 root 工作负载；唯一减法交集 M-28 已 `NO_GO_ABORT`（`:231`）。

### 4.7 W2/W5：已合并，不是本战役的新候选

- **W2**：`#635` 单读 drain 已合并；9 用例语义 oracle、`progress_source_mut_drain_success_keeps_pending` mutant、TLA+ 模型都在。任何后续改动必须保持“单读清 pending”、EINTR/EAGAIN 分支、fd 借用非第二 drain authority；没有新增 W2 候选（`CAMPAIGN-LEDGER.md:258`；`:182-187`）。**M-67（ProgressSource sticky flags）按 ledger §5.2 路由到 W3，不是 W2 新候选**（`:271`）。
- **W5**：`#634` ThreadPool `owed_count_` gate 已合并；parity 8 用例 + 4 mutants 是回归义务；权威 `any_event_owed_locked_()` 扫描保留，counter 只能作派生摘要（`CAMPAIGN-LEDGER.md:261`；`:175-180`）。

---

## 5. Owner 裁决点

以下事项**不是**技术结论，必须由 owner 裁决；在裁决前，相关候选不进入 A/B。

| # | 裁决点 | 选项 | 建议 | 影响 |
|---:|---|---|---|---|
| **O-1** | W1 的 profile 门：接受 bench/reference 代理，还是等 `#604` G0？ | 等 `#604` G0；接受代理并披露；只做设计/A-A；判 `NO_CHANGE_JUSTIFIED` | **等 G0 或只做设计/A-A**；接受代理只能作 mechanism-level/bench-proxy，不能声明 application-level 收益 | W1 的全部 A/B 路径 |
| **O-2** | W1 的通知拓扑：是否授权第二通知通道/blocking-read 重构？ | 授权 C2 condvar 双通道（**`NEEDS_OWNER`**）；授权 D 的 relay/重注册；拒绝 | **拒绝在 `#621` W1 内做**；若追，另立 `#617` G1/kernel-boundary 项目，带新模型/新 oracle/机制门 | W1-C2、W1-D |
| **O-3** | W1 的“5%”分母：process cycles / measured-loop cycles / measured-loop CPU？ | 三者选一，写入预注册 | 必须先定义；未定义则任何代理 A/B 只能记 `INCONCLUSIVE`，不得用 16/36 对结果宣传收益；material 方向仍受 O-2 约束 | W1 预算与判词 |
| **O-4** | W3 是否接受 δ=2% 作为计划值，并资助行级归因？ | 接受 δ=2%；或给别的 δ；或不做 | **接受 δ=2%，先做行级归因**（debug build/隔离格），按 P-D/P-C/P-E 主形态；P-D 先重跑 C2b A/A；不直接 A/B | M-61/M-62/M-67/M-10 |
| **O-5** | M-53 是否接受 cycles 为决定性指标，并做 d64 A/A？ | 接受 cycles + **δ=2% 的 17–20 对**；接受 cycles + δ=1%（A/A 与 A/B 各 58 对）；只接受 CPU（→ 当前环境 `NOT_POWERED`）；`NO_CHANGE_JUSTIFIED` | **接受 cycles + δ=2%（17–20）**，同时要求三项迁移证明；**17–20 不能用于 δ=1%**；若效应预期 0.4%，需 172/336 对，应重估预算 | M-53 |
| **O-6** | M-42 是否现在资助隔离格？ | 现在建隔离格 + 动态回执带 + 机制门 + mutant kill + release 无 seam 符号；或保持排队 | **保持排队**；`GO` 排队 ≠ A/B 准入；资助前不启动 | M-42 |
| **O-7** | 语义冲突批的 owner 裁决 | M-09/M-30/M-57/M-12/M-16/M-80 hygiene 各自 KEEP/DELETE/修 root | 逐项裁决；裁决前不启动性能实验；**W1-C2 不在此批**（`NEEDS_OWNER`，走 O-2） | 6 个 `SEMANTIC_MISMATCH` |
| **O-8** | M-01 的 free-list 是否重开（S2）？ | 重开；不重开；先做 `-scope64` 隔离 profile 再定 | 先隔离 profile，再裁 free-list | M-01 |
| **O-9** | M-37/H1/tp-w8 的归属边界 | 确认 `#595`/`#604` 归属；`#621` 不启动 | **确认归属，不在 `#621` 启动** | M-37、H1、tp-w8 |
| **O-10** | M-28 是否确认不重做？ | 确认 **`NO_GO_ABORT`**；或给出同时满足 ledger §4.1 四要素的新证据 | **确认 `NO_GO_ABORT`（不重做）**；仅四要素同时满足才重开；不是 `NOT_APPLICABLE` | M-28 |
| **O-11** | M-61 payload 是否维持 `NO_GO`？ | 维持；或出现生产消费者/批准测试专用通道 | **维持 `NO_GO`** | M-61 payload |
| **O-12** | W2/W5 是否确认无新候选？ | 确认；或指定新改动 | **确认**；任何改动重跑 9 用例 oracle + mutant（W2）/ parity 8 + 4 mutants（W5） | W2、W5 |

> **M-65 归属不是 owner 点**：R1 已按 ledger 把 M-65 单一归入 W4 owed 家族（预算 M，阶段 T9 owed），见 §3.3/§6.2 stage 4。其 zero-op/owed 协议激活属 W4 设计门，不需要单独的 owner 裁决。

---

## 6. 预算与执行顺序

### 6.1 总预算原则

- **本裁决不批准任何 A/B**。所有 `ADMIT_TO_DESIGN` 只批准设计/资格工作；A/B 必须在设计门、语义门、A/A 门之后重新预注册。
- 预算级别沿用 ledger：S ≤ 1 个单因素臂组（A/A 36 对 + A/B ≤36 对，1 个格组）；M = 2–4 个臂组（A/A + A/B 72–144 对，需行级 profile 或 2 个格组）；L = ≥5 个臂组或跨后端矩阵（>144 对，需新 workload/oracle/协议改造）（`CAMPAIGN-LEDGER.md:266`）。
- 每个 A/B 前必须重新做匹配格 A/A：**P-D→C2b（先重跑，因 NOISY）；P-C→C4 CPU；P-E→C1 CPU；P-A→C3b cycles（cycles-only）；P-B→C6（但族级不可观测）**。C6 的 wall 位置效应要求严格 AB/BA 平衡；C3b/C7 的 cycles 带使用前要 cycles-only A/A；C5b 有背景负载警告，正式使用前复跑，且 C5b 不是 W3 匹配格。
- **17–20 对只对应 δ=2%**；δ=1% 用 58 对；0.4% 量级用 172/336 对。

### 6.2 建议顺序

| 阶段 | 工作 | 门 | 预算 |
|---|---|---|---|
| 0 | Owner 裁决 O-1–O-12 | 本文件 §5 | 0 |
| 1 | **W3 行级归因**（debug build/隔离格）**按 P-D/P-C/P-E 主形态分层**；M-61/M-62/M-67 | 候选级 leaf/stage 分离；**P-D 的 C2b A/A 重跑或改形态**；P-B 0.24% 保持 `NOT_OBSERVABLE`；**不以 P-B 栈截断修复代替主形态归因**；M-62 的 4.13% 只作容器 | M |
| 2 | M-53 三项迁移证明 + d64 cycles-only A/A | TransportLedger 单调/控制 CQE exact/cancel 定向；C7 cycles SD/hw 复现；**17–20 只对 δ=2%；δ=1% 58；0.4% 量级 172/336** | M/L |
| 3 | M-40 设计审计（读者角色/`dispatch_mtx_` 覆盖/缝读者锁）+ 行级 profile | 机制门七问；无生产读者或明确派生摘要 | M |
| 4 | **T9 协议（W4 owed 家族）**：M-63/M-65 owed、M-10 zero-op、M-33 cancel、M-74 ready | zero-op/cancel protocol oracle；交付顺序/失效语义；**跨方法临界区合并证明 `publication_inflight`/slot reuse/observer 注册不变** | M（每项） |
| 5 | M-42 隔离格（仅 owner 资助时） | submit-only 紧循环 + 动态回执带 + 机制门 + **mutant kill + release 无 seam 符号 + 出现生产读者 → `NO_GO`/重新物化** | S + 隔离格 |
| 6 | 语义冲突代码面 PR（M-09/M-30/M-57/M-12/M-16/M-80 hygiene） | owner 裁决 + 回归测试；无性能门 | S（每项） |

### 6.3 不建议现在做的事

- 不要用 36 对 W-02 A/B 去“测一下”M-42；0.066–0.133% 的算术上界低于噪声，结果只能是“不显著”，不能区分“无效应”和“效应太小”。
- 不要在 W3 行级归因前启动 M-61/M-62/M-65/M-67/M-10 的 A/B；阶段容器不是候选上界；**M-62 的 4.13% 不是上界**。
- **不要用 C5b 的 CPU 带去排 W3 的 P-D/P-C/P-E A/B**：P-D 的匹配格 C2b 是 NOISY；P-C 的 C4 在 36 对上 hw 1.021% > 1%（需 38）；P-E 用 C1 CPU（4/5）。C5b 只在 TP d1 w1 的新形态下使用。
- 不要把 M-53 的 17–20 对用于 δ=1%（需 58），也不要把 d16 的 0.406%/0.285% 份额当成 0.4% 的 A/B 效应（0.4% 量级需 172/336 对）。
- 不要在 W1 的 material 方向未裁决前开 A/B；即使 owner 接受代理，C6 cycles 1.2% 还需 48/91 对。
- 不要把 `sluice-copy` 的 A/B 当作 `#604` G0 或 application-level 收益（`CAMPAIGN-LEDGER.md:305-315`）。

---

## 7. 本轮实际执行的检查

### 7.1 已执行（只读复算/源码抽查）

| 检查 | 命令 | 结果 |
|---|---|---|
| P2 R1 全量复算 | `cd docs/evidence/621-progress-wait-path-20261118 && python3 -I p2/profile/analysis/recompute_p2_r1.py --json-out /tmp/r1-check.json` | **exit 0**；5 forms；关键数字与 `P2-COST-MAP-R1.md` 一致：P-B `__poll` 6,129/24,585=24.93%、poll boundary 2,251=9.16%、progress/wait 6,168=25.09%；P-E `__poll` 5,500/34,736=15.83%、poll boundary 1,520=4.38%、progress/wait 8,461=24.36%；P-D publication 627=4.13%、terminal 530=3.49%；W3 family P-A 502=6.00%、P-B 59=0.24%、P-C 2,606=15.82%、P-D 3,405=22.43%、P-E 4,539=13.07%；M-53 d16 34/47；P-B app 12,246/11,104/1,142、wait_one 700、poll_without_wait_one 5,664、four_frame_poll 873。`aa.matching`：P-D→C2b（NOISY，no usable band）、P-C→C4、P-E→C1、P-B→C6、P-A→C3b；C5b=`"not P-D (d16) and not P-B (uring)"`、`"QUIET conditional; not a matching cell"`。 |
| P1 A/A 复算 | `python3 -I tools/recompute_independent.py --aa-root p1/aa --json-out /tmp/r1-aa1.json` | **exit 0**；C1 CPU SD 0.786/MDE 0.589、C1 cycles 2.346/1.758；C3 CPU 7.027/5.266、C3 cycles 1.991/1.492；C4 CPU 3.019/2.262、C4 cycles 8.123/6.088；C6 CPU 0.997/0.747、C6 cycles 2.876/2.155；C6 wall 1.885/1.413 |
| P2 A/A 复算 | `python3 -I tools/recompute_independent.py --aa-root p1/aa2 --json-out /tmp/r1-aa2.json` | **exit 0**；C2b CPU SD 3.414/MDE 2.558、C2b cycles 5.144/3.855；C3b cycles 1.319/0.988；C5b CPU 0.964/0.723、C5b cycles 3.107/2.328；C7 cycles 1.889/1.416、C7 CPU 9.782/7.331；C8 CPU 4.938/3.700 |
| required-n 算术 | `python3 -I` 导入 `tools/recompute_independent.py` 的 `required_n_power`/`required_n_halfwidth`/`mde80` | C4 CPU 2%=20/38、C1 CPU 2%=4/5、C3b cycles 2%=6/10、C5b CPU 2%=5/7、C7 cycles 2%=10/17、C1 cycles 2%=13/24；C7 cycles 1%=30/58；C7 cycles 0.406%→n80=172、n_half(0.203%)=336；0.285%→n80=347、n_half(0.1425%)=678；C7 hw(17)=0.971%、hw(20)=0.884%、hw(58)=0.497% |
| M-42 指令 | `objdump -d --start-address=0x424190 --stop-address=0x424230 /tmp/p2-621/symtwin/bench_w02_pipeline.sym` | `4241b7: f0 48 83 87 20 01 00  lock addq $0x1,0x120(%rdi)` |
| 源码坐标 | `sed -n '218,224p' src/async/uring_transport.cpp`；`grep -n syscall_count_ ...`；`sed -n '72,78p' src/async/uring_test_seams.hpp`；`sed -n '72,80p' include/sluice/async/detail/progress_source.hpp`；`sed -n '905,914p' docs/explicit-io-contract.md`；`nl -ba CAMPAIGN-LEDGER.md` 读 `:207,212,217,219,221-224,235-241,259-262,271,274,512`；`nl -ba p3/W1-DESIGN.md` 读 `:245-256` | `fetch_add` 在 `uring_transport.cpp:222`；`submit_flushes_` 成员 `uring_backend.hpp:324`、唯一读者 `uring_test_seams.hpp:75-76`；`syscall_count_` fetch_add `threadpool_backend.cpp:434`、成员 `include/sluice/async/detail/threadpool_backend.hpp:258`；`eventfd(0, EFD_NONBLOCK / EFD_CLOEXEC)` 在 `progress_source.hpp:75`；PROG-02 允许 condvar 在 `docs/explicit-io-contract.md:907-912`；C2 判词 `NEEDS_OWNER` 在 `W1-DESIGN.md:256` |

> **坐标校正（小）**：复核文本中的 `include/sluice/async/threadpool_backend.hpp:258` 路径不存在；`syscall_count_` 成员实际在 `include/sluice/async/detail/threadpool_backend.hpp:258`（本会话 `grep -n syscall_count_ include/ src/` 验证）。这不是结论变化，只是引用路径校正。

### 7.2 明确未运行

- 任何 `xmake f` / `xmake build` / `xmake test`；当前 master 的测试总数 `N` 未确认。
- 任何新的 perf/record/stat/eBPF/uprobe/strace。
- 任何新的 A/A 或 A/B；本文件的 A/A 数字是**复算**，不是本轮新测量。
- 任何 TLC/`scripts/verify_progress_source_model.sh`；没有重新验证模型。
- 任何 sanitizer（ASAN/TSAN）或完整配置矩阵。
- 没有打开 `#637` 归档 `e2f41acb` 的原始回执；M-61 R3 的 0.45%、M-28 的 1b/1c 判词按 `CAMPAIGN-LEDGER.md` 的转述核对，未独立复算。
- 没有修改 `include/`、`src/`、`apps/`、`tests/`、`xmake.lua`、`xmake/`；没有触碰两个未跟踪 evidence 目录（`docs/evidence/621-round2-R1/`、`docs/evidence/e5-fullpath-machine-work/`）。

---

## 8. 本裁决不能下的结论

1. 不能说 W1 能删 5.0–5.5% cycles；历史数字只是被取代的 R1 工作稿线索，P2 只证明形态级等待热点，未证明可删量。
2. 不能说 `__x64_sys_poll` 15.77%/11.46% 是上界；`__poll` inclusive 24.93%/15.83% 才是可复算的 poll 相关 inclusive，且 inclusive 不是收益。
3. 不能说 W3 的 13.07–22.43% 是单候选收益；它是阶段容器，含内核/libc/bench 与必要同步；P-B 的 0.24% 因 90.67% 缺 bench 帧而不可用。
4. 不能说 M-61/M-62/M-65/M-67/M-10 任一已 `GO`；它们只有阶段容器，M-61 还有 0.45% <1% 的历史反例；**M-62 的 4.13% 不是候选上界**。
5. 不能说 M-53 在 d64 有已证实的 hotspot；d64 只有 C7 的 A/A 条件带，且 **17–20 只对应 δ=2%**；δ=1% 需 58；0.4% 量级需 172/336；三项迁移义务未证。
6. 不能说 M-42 可测；`lock addq` 19–20 cycles 的 0.066–0.133% 低于匹配 uring 带；P-D/P-E 不适用。
7. 不能说 `GO`（M-42）等于 A/B 准入；`GO` 只是排队，启动属 owner。
8. 不能说 sluice-copy 的 A/B 是 `#604` G0 或 application-level 收益；它是参考应用，不是真实消费者。
9. 不能说本 profile 已经过了 ledger 的“真实消费者 profile”门；P2-REVIEW M10 明确未过。
10. 不能把 W1/W3/M-53/M-42 的数字相加；它们是同一进程 cycles 的不同子集/切法。
11. 不能把 `SEMANTIC_MISMATCH` 的候选当作“再测一次”就能解决；它们需要 owner 裁决或代码面 PR。**W1-C2 是 `NEEDS_OWNER`，不是硬语义冲突**，但同样不能在 owner 授权前进入 A/B。
12. 不能把本轮复算当作新的 A/A/A-B 证据；它只验证已归档数字的可复现性。
13. **不能把 M-28 记为 `NOT_APPLICABLE`**；它是 `NO_GO_ABORT`，`syscall_count_` 仍在生产；没有 ledger §4.1 四要素同时满足的新证据，不得重开。
14. **不能用 C5b 的 CPU 带支撑 W3 的 P-D/P-C/P-E A/B**；C5b 不是任何 P2 W3 形态的匹配格；P-D 的 C2b 是 NOISY，无可用带。

---

## 9. 参考坐标

| 主题 | 坐标 |
|---|---|
| W1 源码 | `include/sluice/async/detail/progress_source.hpp:104-232`；`src/async/async_io_context.cpp:573-705` |
| eventfd 创建/retire | `progress_source.hpp:74-86,284-290` |
| fused revalidation + drain | `progress_source.hpp:153-166,414-460` |
| probe / P3 | `progress_source.hpp:175-192`；`src/async/uring_backend.cpp:285-293` |
| poll/EINTR/POLLNVAL/POLLIN | `progress_source.hpp:198-230` |
| signal/interrupt | `progress_source.hpp:234-264` |
| drain/wake/suppression | `progress_source.hpp:414-460,466-489` |
| host borrow/ack/detach | `src/async/async_io_context.cpp:720-747`；`progress_source.hpp:274-277,524-545` |
| W1 设计评估 | `p3/W1-DESIGN.md:15-30,142-344,377-419,423-510` |
| P2 成本地图 | `p2/P2-COST-MAP-R1.md:451-537,546-568,609-620` |
| P2 独立复核 | `p2/P2-REVIEW.md:30-92,116-158,239-247,322-331` |
| P1 A/A | `p1/AA-CALIBRATION.md:20-37,384-402,418-513` |
| P2 A/A | `p1/AA-CALIBRATION-2.md:12-28,186-238,393-526` |
| `#637` 终态表 | `CAMPAIGN-LEDGER.md:205-231,233-239,264-285` |
| W1–W6 门禁 | `CAMPAIGN-LEDGER.md:253-262` |
| `#595`/`#604`/`#617` 边界 | `CAMPAIGN-LEDGER.md:291-333` |
| 战役纪律 | `CAMPAIGN-LEDGER.md:488-528` |
| M-42 源码 | `src/async/uring_transport.cpp:218-235`（`:222`）；`include/sluice/async/detail/uring_backend.hpp:324`；`src/async/uring_test_seams.hpp:75-76` |
| W5 owed-count | `src/async/threadpool_backend.cpp:344-353,550-612,847-860,875-899`；`include/sluice/async/detail/threadpool_backend.hpp:203-212,239-244` |

---

*本文件结束。所有未在 §7.1 执行的断言均为“未运行/未验证”；所有性能数字都是已发布证据或只读复算，不是本轮新测量。*
