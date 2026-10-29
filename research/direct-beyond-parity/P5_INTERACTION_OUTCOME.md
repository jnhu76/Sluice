# P5 Interaction Outcome — A+B 组合矩阵终裁（评审异议闭合版）

- 依据：`MISSION.md` §7（P5 集成与独立评审）；`P5_INTERACTION_PLAN.md` §4
  预注册裁决规则（S0–S6 合取与 fallback）。本文档是计划 §4 要求的最终裁决
  载体，闭合独立评审（2026-10-28，五项 APPROVE + 终项 REVISE 七条）指出的
  "矩阵终态悬空"。
- 裁决日期：2026-10-28（评审后闭合会话）。裁决对象：堆叠分支
  `direct-compose-stack-readpreval-writeexit` HEAD `4ab6ef17` 及其归档测量。
- 证据基础：`p5-raw/` 全部归档（41 cell 原始 16 deltas + 分析尾、A/A、PMU、
  strace、env-pin、kit-smoke、identity、`P5AB/` 门禁与静态档案）。本文档每个
  数字均由本闭合会话从归档文件直接读出或重算（下文逐处标注文件名）；评审员
  的独立重derive（41/41 MATCH）作为第二来源佐证，不替代本文档自身读数。
- 单候选冻结证据（`P2_CANDIDATE_P2C1.md`/`P2_CANDIDATE_P2C2.md`、`p2-raw/`）
  未做任何改动；评审对单候选的两条非阻塞观察记录在本文档 §10（按评审处置
  要求"记录到文档"）。

## 1. 身份（identity.txt，本会话复读）

四 worktree HEAD：B1 `77e5399a` / A `deaf0301` / B `476bd9ce` / A+B
`4ab6ef17`；四归档 sha256：B1 `d1e351a5…3007f0`、C1 `07d06803…2049e`、
C2 `67251028…20a807`、**AB `07d06803…2049e`（= C1）**；kit 二进制：
`ladder 89450367…010e8`、`ladder-c1 8305d22c…0381`、`ladder-c2
8bb6473b…0065`、**`ladder-ab 8305d22c…0381`（= ladder-c1）**；kit 源
四二进制统一 `2364be4a`（P4 Amendment 1 条款）。identity.txt 自带注记：
堆叠归档与 kit 二进制均与 C1 字节相同（R1 整取 P2C1 的
`src/blocking_file.cpp`）。静态档案（`P5AB/static/static-recount-20261028.md`）
对象级确认：AB 的 `blocking_file.cpp.o` sha256 = C1 的 `d5c58bb3…87a8b`。

## 2. S0 正确性前置 — PASS

- `p5-combined-mutants.sh` exit 0 @ `4ab6ef17`：8 条缝腿（6 个并集缝
  define）逐一 GREEN→RED(精确命名断言)→RESTORED GREEN + rules 控制腿在
  `FAILURE_ACCOUNTED` 突变下 GREEN。本闭合会话运行档案：
  `P5AB/mutants-20261028T215835`（主工作区相对路径调用；前一会话档案
  `mutants-20261028T184656`/`184128` 在档），mutants-head.txt = `4ab6ef17`。
  评审员另在位复跑 exit 0。
- `p5-combined-matrix.sh` exit 0：四腿 release/debug/asan/asanubsan 各
  "100% tests passed, 0 test(s) failed out of 58"。本闭合会话运行档案
  `P5AB/matrix-20261028T215804`（四腿亲读）；评审员复跑 release 腿 exit 0
  并逐腿读出终 HEAD 归档 `matrix-20261028T185735` 各 58/58。
- 权限收缩审计重记：堆叠树无新增运行时状态（组合循环仅局部变量
  `confirmed_bytes`/`transferred` + 模板参数；`direct_outcome`/
  `publish_certainty` 已删除；`detail::compose_progress` 规则表保留为
  非权威 SEM-05 oracle，规则套件在突变下仍绿由控制腿证明）。

## 3. S1 静态保持 — 读侧 PASS，写侧 FAIL → INTERFERENCE_STATIC

`P5AB/static/static-recount-20261028.md`（四归档审计约定重计，8/8 锚点
复现；本闭合会话复读全部表格与分解）：

- 读侧（= A 终态）：`read_exact_at` 68≤75 / 弧 25≤26；`read_exact`
  55≤60 / 弧 21≤21（恰在界上，与 C1 自身 §2.1 判决一致）。M1/M2/M3 机检
  过（步循环零重测、`movaps 0x20/0x10(%rsp)` 0/0、单一出口物化）。
- **写侧 FAIL：`write_all_at` 每附加步弧 39 > 上限 37（+2，超出任何 ±1
  口径歧义）** → 按计划 §4 S1 记 **INTERFERENCE_STATIC**，写侧堆叠声明
  不被采纳（无 wall 数可修复失败的静态界）。其余写侧界过：
  `write_all_at` 1-step 79≤93、`write_all` 61≤77 / 弧 29≤30。弧 +4 vs
  C2 自身 35 的分解在档案：A 的回边形态（advance→complete 检查→零进度
  检查→循环条件，confirmed 更新 mov 在回边内）未融合 C2 的合并块。
- 公开操作机器同一：四公开操作地址剥离指令流 AB vs B1 sha 相等
  （read_at 762b2ae4… / read 27b981d9… / write_at a2e22afb… /
  write 8dba982c…）。
- 不可能计数守卫四回边在位：read_exact_at 1130/1133、read_exact
  1780/1783、write_all_at 14a0/14a3、write_all 1a90/1a93。
- 机制重复审计（item 7）：`src/blocking_file.cpp` 改行数 B1→AB = 128 =
  B1→C1 —— **B 的文本终态零行存活**（详见 §8）。

## 4. S2 — A 的赢面单元在堆叠二进制上：STACK_LOSS ×3

（`p5-delta-comp-*-4k-ab.txt` 分析尾，本闭合会话逐文件读出）

| 单元 | Δ_AB 中位 | 95% CI | 判定 |
|---|---|---|---|
| comp-read-4k | −10.880 | [−20.020, +3.034] 含 0 | **STACK_LOSS** |
| eofmix-4k | −10.244 | [−14.102, +9.670] 含 0 | **STACK_LOSS** |
| cursor-4k | −8.107 | [−18.592, +1.860] 含 0 | **STACK_LOSS** |

三单元中位数均过 −5.535 钉线但 CI 全含 0 → 按预注册 S2 规则
（"CI-spanning → STACK_LOSS(u)"）记 STACK_LOSS；非 REGRESSION（无
CI-排除的不利向）、非 INCONCLUSIVE（预算内采样已足 16 对，失败是 CI
跨零，非样本不足的悬而未决）。**S2 FAIL。**

功率归因（记录，非声明）：本会话 A/A 读轨 sd 15.475/20.243/19.445 →
新鲜 MDE′ 8.2/10.8/10.4（跨二进制取 max-of-pair ≈ 10.4–10.8），与效应
中位 8.1–10.9 同量级；同会话组3 参考腿上 A 的赢面在 2/3 单元 CI-排除
复现（Δ_A comp-read −21.928 CI[−30.708,−4.178]、cursor −14.571
CI[−22.442,−4.431]；eofmix −13.332 CI 含 0，见 §11）。因 ladder-ab ≡
ladder-c1（§1），组4 与组3 是**同一库代码的两次独立采样**：S2 失败是
本会话噪声功率问题，不是机制消失的证据——作为归因记录，不改变
STACK_LOSS 判定，不做加采样（预算外采样需编号修订）。

## 5. S3 — B 的指令口径边际：FAIL

（`p5-pmu-{base,ab}-wcomp-4k.txt`，本闭合会话从六窗口原始计数重算）

- P_base_w：WX 均值 1,145,237,961 − W1x 均值 1,144,975,712 = 262,249/4096
  = **+64.026 instr/op**（逐窗 63.97/64.06/64.03）。
- P_ab_w：WX 均值 1,145,156,036 − W1x 均值 1,144,975,569 = 180,466/4096
  = **+44.063 instr/op**（逐窗 44.045/44.058/44.075）。
- 判据 P_ab_w ≤ P_base_w − 20 → 44.063 ≤ 44.026 **不成立 → FAIL，边际
  0.037 instr/op**（逐窗 2/3 FAIL，1 窗以 0.002 过线）。与 P2C2 自身 G3
  通过边际 0.046 同量级、同向——按预注册规则原文记 **FAIL**，不以边际
  放行；与 S1 写弧 +4 同源互证（堆叠树写路径 = A 的 compose，非 B 的）。
- wall 分量合规：Δ_AB_w = −7.915，CI[−21.948, +25.706] 含 0 → 非 EXCESS
  （`p5-delta-wcomp-4k-ab.txt`）。**S3 FAIL**（PMU 边际 + S1 写界双失败）。

## 6. S4 负控 — PASS

（`p5-ab-neg{read,write}-*.txt`，本闭合会话读出）：negread-4k +4.291
CI[−4.099,+17.003] 含 0；negread-1b −1.663 CI 含 0；negwrite-4k −5.017
CI[−21.121,+30.978] 含 0；negwrite-1b −2.003 CI 含 0。四控全 PARITY，
无 EXCESS。**S4 PASS。**

## 7. S5 回归面 — PASS

（`p5-reg-*.txt`，本闭合会话读出）：readat +1.860 / writeat −3.098 /
meta-oc +1.469 / meta-fi −2.344，CI 全含 0 → PARITY/NO_CLAIM；cold
+134.407 CI[−579.172,+399.377] 含 0、≪ 钉定冷底 1416.185 → NO_CLAIM
（claim 上界成立）。机器级排除由 S1 公开操作 sha 相等承担。**S5 PASS。**

## 8. S6 端点与交互归因（记录；仅 D_ab_w 入合取）

（`p5-ab-*-4k.txt`，EXCESS 约定 D = AX−A1x = −(A1x−AX)；本闭合会话读出）

- **D_ab = +8.248，CI[+3.433, +11.288] 排除 0**；量级阈
  max(MDE 5.535, 1%·N1 中位 9.333) = 9.333 → **8.248 < 9.333，按 P1 §5.5
  规则原文两个口径（钉定 5.535 / 新鲜 10.36）均非 EXCESS**。评审提出的
  "钉定口径 EXCESS / 新鲜口径非 EXCESS"双报分歧源于 MDE-only 读法（漏
  1%·N1 项）；本文档按计划 §4 逐字规则（含 1%·N1）裁决为非 EXCESS，
  CI-排除这一事实如实披露，不触发端点 cap（P2C1 §12.4 式 cap 仅在端点
  EXCESS 时触发）。两种读法均不改变任何 S 门。
- D_ab_eof = +6.895 CI 含 0；D_ab_cur = +2.777 CI 含 0；**D_ab_w =
  +1.123 CI 含 0 → 非 EXCESS（S6 合取项满足）**。
- 交互项（归因记录，永不作声明；±30–80% 静态→wall 系统性口径注记随行）：
  I(comp-read) = −10.880−(−21.928) = +11.048；I(eofmix) = +3.088；
  I(cursor) = +6.464；I_w = −7.915−(−10.817) = +2.902 —— 堆叠臂同会话
  效应小于单因子臂，方向与"堆叠 = A 的库代码 + 会话功率"一致。
- 组2 新鲜锚（记录）：S_base = +12.079 CI 排除（≥ max(5.535, 9.421) →
  EXCESS）；S_eof_base = +16.912（0/16）；S_cur_base = +13.647；
  S_base_w = +13.732 CI 含 0（写侧 instruction-caliber 事实再现）。
  读侧 PMU 佐证（非门禁）：P_base +128.03 → P_ab +65.92 instr/op
  （本闭合会话从 `p5-pmu-{base,ab}-composition-4k.txt` 六窗口重算），
  与 P2C1 会话的 +128.0/+66.1 同带——读侧机器工作保持 = A 终态。

## 9. 堆叠裁决合取 — 非 STACK_ADMITTED；fallback = 集成候选 A alone

S0 PASS ∧ S1 **FAIL（写侧 INTERFERENCE_STATIC）** ∧ S2 **FAIL
（STACK_LOSS ×3）** ∧ S3 **FAIL** ∧ S4 PASS ∧ S5 PASS，S6 已记录
（D_ab_w 非 EXCESS）→ 合取不成立，**不采纳 STACK_ADMITTED**。

按计划 §4 fallback 规则（"if S2/S4/S5 fail, likewise whichever factor's
own gates still hold"）：**记录的集成候选 = A alone**。A 的 KEEP 立于
P2/P4 冻结证据（P2C1 §12.5；P4 WIN_CONFIRMED ×3），本会话组3 参考 2/3
单元 CI-排除同向佐证；B 的单因子 KEEP（instruction caliber，P2C2 §9）
不受 P5 wall 数撤回。终态词汇：STACK_LOSS(unit) ×3 + INTERFERENCE_STATIC
（写侧）+ S3 FAIL，fallback A alone；OPEN 状态，无合并（计划 §0 规则 6）。

## 10. A+B 实为 A′ 的结构性记录（评审 REVISE 第 2 条）

- 计划 §2 R1 预登记写侧"语义并集"；实际交付整取 P2C1 的
  `src/blocking_file.cpp`（B1→AB 改行 128 = B1→C1；AB 归档、对象、kit
  二进制均与 C1 字节相同；B 的写侧对象代码零行存活；写弧 39 劣于 B 自身
  35）。B 的出口暂存变换在语义轴上被 A 的共享模板承载（每出口风味单点
  物化 + 寄存器驻留 + 公开写步保留每步重测），但非逐指令承载。
- **交互矩阵实际测量的是 BASE / A / B / A′（A′=A 的库代码，独立采样）**。
  该矩阵语义变化与 §7.1 机制重复问题的否定性结论（按预注册配方合并即取
  A；两因子在共享 compose 模板上非可加）由 identity.txt 与静态档案量化
  披露，本文档将其纳入正式裁决记录（本节）。
- 计划 §6 行 1 预先声明的幂等预期（"write side = B's bounds met, with
  A's exit machinery already carrying much of B's transform — outcome
  recorded either way"）的实测落点：读侧 = A 终态（bounds 过），写侧
  B 的 bounds 未保持（arc 39 > 37）→ 落在"either way"的失败支，照记。

## 11. 持续会话噪声正式披露（计划 §5.1；评审 REVISE 第 4 条）

- A/A 接受 cell（读轨各经一次 §11.10 45 s quiesce rerun，rerun 值生效；
  attempt1 均在档）：read sd **15.475 / 20.243 / 19.445**（b1/c1/ab）、
  write sd **53.481 / 33.026 / 28.272**（b1/c2/ab）、cold sd 476.079 —
  **读、写轨全部 > 10 ns #600 噪声带**。持久会话噪声按 §5.1 披露，不以
  重复吸收，不加修订不追加采样；新鲜 MDE′ 与钉定底线双报已用于上文各
  判定（S2 单向降级规则无触发点：CI 含 0 使任何底线下的声明都不成立）。
- 写轨首对离群：`raw_deltas[0]` 在 7 个写 cell 落于 **+140.6…+204.2 ns**
  （base-wcomp 187.697 / ab-wcomp 197.209 / delta-wcomp-ab 140.764 /
  delta-wcomp-b 175.391 / negwrite-4k 181.610 / reg-writeat 153.839 /
  aa-write-b1 204.171；negwrite-1b 干净 −1.776）。16 对中位数对单一首对
  离群稳健（中位数不受第 1 对影响当 pos≤8）；均值/CI 受牵动，已在各 cell
  记录中随行，不删样（§5 item 7 禁止）。
- 组3 eofmix Δ_A = −13.332 CI 含 0：方向与冻结证据一致（无矛盾）→ 按
  计划 §3 组3 归类为 **attribution reference，非环境 alarm**（alarm 仅
  在新鲜参考与冻结证据矛盾时记录）；欠功率如实记载，不升级为声明。

## 12. 评审单候选观察登记（冻结 P2 证据不改；评审 P2C1 条目 problems）

1. **THREAD-01 并发 close 语义边界（P2C1 观察 1，适用于 A 与 A+B 树——
   两者生产 TU 字节相同）**：`read_exact_at`/`read_exact` 的预验证步不再
   逐步重跑准入。调用方违反 THREAD-01 串行化（组合进行中并发 close）时，
   B1 会在下一步检出并以 invalid_state 停机；A/C1 树继续对已关闭描述符
   pread，表现为 backend/EBADF 类原语错误。该交错在契约外（SEM-03 调用
   方前置条件、THREAD-01 调用方串行化+"无未决借用"），不构成违规；
   P2C1 §3 义务表未载此差异，在此补记。范围注记：写侧两函数步原语仍为
   公开 `write_at`/`write`（逐步准入保留），不受此差异影响。
2. **primitive_error 出口死存（P2C1 观察 2）**：`0x1067 movq $0,0x30(%rsp)`
   于 `0x1074` 被覆写，仅错误路径、不在任何成功路径——P2C1 §2.1 已披露
   的 `[RUN-IMPL]` 例外，此处交叉引用，维持披露态。
3. 正确性评审携带的性能事实：INTERFERENCE_STATIC（§3）与 B 零行存活
   （§10）——已入裁决正文。

## 13. 可复现性脚注与门禁复跑（评审 REVISE 第 1/7 条的闭合侧）

- **门禁脚本可复跑**：两脚本提交于分支（`p5-scripts/p5-combined-
  {matrix,mutants}.sh`，Amendment 1 记录命名偏差），主工作区
  `research/direct-beyond-parity/p5-scripts/` 存 sha256 相等副本（matrix
  `3efd53b4…fdce25`、mutants `c805f416…993d8`）；内部 `WT` 绝对路径自
  行 cd，任意 CWD 可调。本闭合会话以主工作区相对路径（CWD=主工作区根）
  复跑：`bash research/direct-beyond-parity/p5-scripts/p5-combined-matrix.sh`
  → exit 0（`P5AB/matrix-20261028T215804`，四腿 0 failed）；同路径
  mutants → exit 0（`P5AB/mutants-20261028T215835`，8 缝腿命名击杀 +
  控制腿 GREEN）。前一会话同路径运行档案
  `matrix-20261028T184625`/`mutants-20261028T184656` 亦在档。评审后无
  代码/脚本改动，裁决不依赖新运行。复跑注意：相对路径调用要求 CWD 在
  主工作区根（本闭合会话一次 CWD 停在 p5-raw 的调用得到 127，属调用
  位置错误，非脚本缺陷——绝对路径调用不受影响）。
- **A/A 分析器配对约定**（评审脚注）：P4 分析器用交替符号翻转约定，P5
  归档分析尾用连续配对约定；两会话各自内部自洽，评审员按各自约定均精确
  重现。本文档 A/A 数字全部读自 P5 归档尾（连续配对）；§5 item 8 本就
  禁止跨会话数值比较，故该约定差不影响任何裁决。
- 其余复现件：`analyze2-p5.py` 归档于 p5-raw（关闭 P4 的 analyzer 未归档
  缺口）；env-pin 会话开 pre-cell 检查 0 匹配进程 + close 记录在档
  （`env-pin-2026-10-28.txt`）；kit-smoke 162 行检查
  "ALL GATED GROUPS IDENTICAL"（`kit-smoke.txt`，含 ladder-ab ≡ ladder-c1
  显式注记）；strace 12 文件全覆盖组4/6/7（配对计数相等 →
  PHYSICAL_WORK_DELTA=0）。

## 14. 评审异议 → 处置映射

| 评审 REVISE 条目 | 处置 |
|---|---|
| 1 缺综合裁决文档 | 本文档全文（§2–§9 合取推导与终态） |
| 2 A+B 退化为 A、须入裁决记录 | §1、§8、§10 |
| 3 S3 判定缺失、不得以边际放行 | §5（FAIL 边际 0.037，逐窗披露） |
| 4 持续噪声未正式披露 | §11（A/A 表 + 写轨首对离群 7 cell） |
| 5 S6 D_ab 双报处置与 cap 语义 | §8（按规则原文含 1%·N1 项裁决，两种读法并陈） |
| 6 组3 eofmix 欠功率归类 | §11（attribution reference 非 alarm） |
| 7 A/A 配对约定脚注 | §13 |
| P2C1 观察 1/2（单候选，不改冻结证据） | §12 |

计划 Amendment log 不因本文档新增条目：本文档是 §4 预注册裁决的执行
载体，未改变任何计划参数。
