# 裁决矩阵与预注册协议挑战报告（独立挑战者，e5-fullpath-machine-work）

- 挑战：2026-11-18。对象：`candidates.md`、`experiment-protocol.md`（裁决官产出）。
- 交叉输入（只读）：`dag.md`、`p1a-cost-map.md`、`measurement-audit.md`（§2 推翻/降级
  清单按硬约束执行）、`history-digest.md`、`pocketjs-method-notes.md`；另回源核实了
  `p0c-cache-study.md` 与 `repo-reality.md` 的被引节、`docs/explicit-io-contract.md`、
  bench/tests 源码锚点。本文件是本次挑战唯一写入物；未修改任何其他文件。
- 方法：对两份产出逐条抽裁（实抽 13 条裁决：候选 #1/#5/#6/#7/#8/#10/#11/#12/#13/
  #14/#17/#19 + 3 条非候选热点），对每个被引文件§节与源码行号回源核对（命令与命中
  见 §5）；门禁-效应对齐做算术复核；PocketJS 减法透镜 10 条逐条对照裁决矩阵。

## 0. 结论

**不通过（approved = false）**：发现 8 项必须修复缺陷（F1–F7 涉及执行阻断、裁决
事实前提、证据记录、预注册完备性与引用纪律；F8 为透镜遗漏处置）。裁决主干
（#1 选定、B/C 概念分离、KEEP_IMMEDIATE 不动摇、负控制剪枝主体）经核实成立，但
下列缺陷必须在协议冻结/实验执行前修复。

## 1. 逐项挑战结果

### 1.1 裁决抽查（挑战项 1）——13 条实抽，12 条依据成立，1 条事实前提错误

- **#1 M-28 GO**：30.24%×1.83%≈0.554%≈170 cyc/op 链条与 p1a §4-1/审计 §1.2 逐值一致；
  写侧 `threadpool_backend.cpp:434`、成员 hpp:258 无门控、访问器 hpp:66-76 在宏内、
  seams:4/108-110（本会话全部复核）；16 条计数断言行号逐条核实命中；#635
  DrainObservation 先例与 history-digest §4.2 一致。**成立**。
- **#5 M-53 INCONCLUSIVE**：0.14/1.03/1.74% 与 ≈63/116 cyc/op、三项迁移义务
  （dag.md M-53）、斜率 INCONCLUSIVE（p1a §4-2/§6）均属实。**成立**。
- **#6/#18（M-63/M-65）**：NOT_OBSERVABLE 前提（无零长 op → owed_count_ 恒 0，
  p1a §4-3；#634 门控，history-digest §4.1）属实。**成立**。
- **#7/#8/#10（SEMANTIC_MISMATCH）**：borrow 只写（dag.md §7-2，contract
  `docs/explicit-io-contract.md:586` 实核命中）、zero_effect_cancel 零生产者
  （dag.md §7-8）、close_admission 双 backend 版零生产调用（dag.md §6-2/§7-11）均
  属实。细节：`progress_source_ownership_test.cpp` 实有 5 处调用（:468,471 直呼 +
  :481,495,496 经 helper），"仅 :468,471" 表述不完整但同一文件、结论不受影响。
- **#11 M-42 GO（排队）**：:222 fetch_add 在早退后、0.01% 符号份额、读者在测试宏
  （dag.md §7-6）逐条核实。**成立**。
- **#12 M-40 NO_GO：事实前提错误，见 F2**。
- **#13 M-61 payload NO_GO**：begin_publication 恒 nullptr（dag.md §7-15/M-61）属实。**成立**。
- **#14 M-01 INCONCLUSIVE**：submit_read 三深度 0.93/1.32/1.68 已按审计 §2-7 修正
  （修正正确）；驱动污染剔除注明。**成立**。
- **#17 M-62 INCONCLUSIVE**：dag.md §6-1"快速返回≠零锁开销"更正、§1.2 项 6 对位
  mutex family 均属实。**成立**。
- **#19 M-67 INCONCLUSIVE**：行级未分离属实；但对位锚引 R2/R3 存疑，见 S3。
- **非候选热点 3 条**：H1（enter/op=1.000052、≈23.2% ᴷ、#622 C1 −65%/−85% 均按
  "定位"引用，与 history-digest §5.2 消费边界一致）、io-wq（FS 条件性成立，
  但 ≈7–10.5% 引用违反降级纪律，见 F6）、tp-w8（11.16%/3.2 futex/42 µs/≈7.1%
  枚举引用正确）。**2 条成立、1 条引用缺陷**。
- 被推翻/降级值清点：P0-C bench instr/op ×9/7 膨胀值**零引用**（头部纪律如实）、
  Top10 #9 修正值正确、governor §2-8 教训已被协议吸收（唯一文件名）。**唯二违例
  为 F2（事实性）与 F6（降级值入裁决列）**。

### 1.2 门禁-效应对齐（挑战项 2）

- 门形状本身正确：MME 155 cyc/op < 预期效应 ≈170 cyc/op；分辨力要求半宽 ≤MME/2
  （77 cyc/op）不是松门；A/A 无效界（>3.0pp）与 #635 先例（2.4090≤3.0）同层。
- 两处缺陷：MME 政策与预算功效（F5）、失败判词不完备（F4）。

### 1.3 P1-B 三概念（挑战项 3）

- **无 B/C 混装**：C 类清单（#1/#11 无读者 RMW；#5/#19/#15/16/17/18 协调面）逐条
  核对均不动提交时序/批量拓扑；B 类全部改提交时序，交集为空。
- **KEEP_IMMEDIATE 无越权**：§4-A 明确本轮 H1 证据是"定位证据"、enter/op=1.000052
  来自 kernel 7.3.3 的 p1a §7-1 复现；D2 FAIL 幅度 4–7pp + enters 不变计数解释的
  引用与 history-digest §5.1#1 逐字一致；重开条件（RQ1 + 整机重测 + A.4 falsifier
  家族）与 §2.2/§5.3-1 一致。**未发现把定位证据当裁决的越权**。

### 1.4 PocketJS 减法透镜（挑战项 4）

- 10 条透镜逐条对照：H1→#595/概念 B（正确归属）；M-53→#5；M-61/M-62 融合→#16/#17
  （族级绑定如实）；io-wq→非候选 NO_GO（"不发明伪候选"执行正确）；M-28→#1；
  ReferenceReadySink 证据通道先例（hpp:9-16 实核）→协议 §1/§3。
- **两处遗漏/错价**：live_cookies_/live_control_sqes_ 降级被错价为零机工（F2，
  透镜条目 10 说"有空间"）；work_mtx_ 拆分（透镜条目 7"次级小空间"）未被处置
  （F8——经核实其真实约束是 `internal_work_retired` 的同锁联合快照
  `threadpool_backend.cpp:856-861`，结论"无最小可减机制"成立但缺该绑定引用）。
- **未采纳但登记**：cookie_free_list_ 先例 vs m1 REJECT（见 S2）。

### 1.5 协议（挑战项 5）

- 负控制剪枝：逐条核实成立——D2/D3 NOT_CONSTRUCTIBLE 引 `p0c §9` 实核命中
  （§9 表确有"bench D2/D3 格 NOT_CONSTRUCTIBLE"行，且冻结 case 列表
  `bench_w02_pipeline.cpp:655-672` 实无任何 d2/d3 格）；W-03/uring 族剪枝与候选
  零足迹一致；short/long 与计数长度无关成立；:487-569 计数断言实核命中。
  **但保留的两个报告性对照无判词（F4）**。
- worktree/基线冻结：新建 `/home/jnhu/sluice-e5-elim/`、绝不复用
  `/home/jnhu/sluice-621-r2/`（repo-reality §2.2 的 12 worktree 保护清单实核一致）、
  独立 build 目录 #520 禁令（repo-reality §4 实核命中）、BASE_SHA=5d94f233、双臂
  sha256、governor 唯一文件名——防混链设计成立。**但 CANDIDATE 描述与 mutant
  载体矛盾（F7）、主格 case id 冻结值错误（F1）**。

### 1.6 选择规则（挑战项 6）

- Top10 中 22 候选成员的可测排名：#2（M-28）>#3（M-53）>#10（M-42）；M-53 因三项
  迁移义务未证 + 斜率 INCONCLUSIVE 正确让位；MEASURED-small 的 #11 排 #1 回执之后
  （单机制规则）。**"profile 排名最高且正确性证明可完成"执行正确**。
- NO_CHANGE_JUSTIFIED 拒绝理由三条（MEASURED 定位 + 可完成证明 + 现成构建先例）
  逐一核实成立。**未被无理跳过**。

## 2. 必须修复缺陷（F1–F8）

- **F1【执行阻断·冻结值错误】** 协议 §2 负载集 case id 与 bench 冻结 case_id 不匹配：
  冻结表写 `W02-tp-4k-d16-w8`/`W02-tp-4k-d16-w4`/`W02-tp-64k-d16-w4`，而
  `make_meta`（`bench/bench_w02_pipeline.cpp:463-467`）用 `backend->name`（"threadpool"，
  :624）拼 id，真实 id 为 `W02-threadpool-…`（p1a §0 亦用全名）。协议自述
  `SLUICE_BENCH_CASE_FILTER='=<case>'` 为精确匹配（`bench/bench_common.hpp:492-500`
  `case_id == filter+1`；审计 §1.7），冻结值将零匹配、格位不运行。改用真实 case id
  或注明映射。
- **F2【裁决事实前提错误】** candidates.md #12（M-40）NO_GO 依据"无可消除机工：
  x86 relaxed 原子 ≈ 普通访存，机制增量 ≈0"对写侧不成立：`live_cookies_.fetch_add/
  fetch_sub`（`src/async/uring_transport.cpp:195,581`）、`live_control_sqes_.fetch_add/
  fetch_sub`（:295,:427,:490；`uring_completion.cpp:118`）是 lock 前缀 RMW——与 #11
  （M-42 单条 xadd/op，p1a §4-4 明称"lock-free xadd"，MEASURED-small、GO 排队）同类。
  诚实的差别是：M-40 计数器有生产读者（`uring_backend.cpp:350-351,371-372`），
  宏门控路径不可用、降级需改缝读者（`uring_test_seams.hpp:76-78`）——"机工 ≈0"
  不成立。须改写 #12 依据（可消除机工 ≈2-3 条 lock RMW/op）并按此重裁（可仍 NO_GO/
  INCONCLUSIVE，但理由必须真），同时登记源 dag.md M-40 的同源错误。否则 #12 是被
  错价的减法候选，与 #11 的同族处理自相矛盾。
- **F3【证据记录枚举错误】** candidates.md §6 与协议 §3.1 的"tests.lua 10 处重编译点
  （:406,448,501,559,745,818,891,959,1015,1074）"实际为 **12 处**：漏 :1139
  （`request_scope_target`，定义 :1114、define :1122）与 :1205
  （`threadpool_request_core_integration_test`，define :1202）——恰是三消费套件中的
  两个。结论本身（三目标均 define+重编译）经独立核实为真，但该清单是"哪些目标在
  无宏下重编译该 TU"的安全面，必须完整。
- **F4【预注册·失败判词不完备】** 协议 §5 判词 1-6 未覆盖：(a) uring 隔离格自述
  "候选必须对 uring 路径 Δ≈0"却无数值判据、违例无判词（与"必须"自相矛盾）；
  (b) 64KiB 负对照"无回归"无判据——#635 的 64KiB 对照实测 −1.46%
  [−2.74,−0.23]（history-digest §1.2），"非零"是现实可能，无预注册带则判词不可
  执行；(c) §4-1 三列存证（nm/objdump）与 §4-2 回执带 [−2,0] 违例未映射到任何
  判词（应并入判词 1 的"中止"通道）。补充判据与判词后方可冻结。
- **F5【预注册·MME 政策不一致 + 预算无功效依据】** (a) 协议 §5 MME=155 cyc/op
  （≈0.50pp）且判词 4 允许在该条上判"性能 GO"，而同套产出 #19 的翻案条件引用
  1% MME 先例（r6 R2 0.90%/R3 0.45% 已 REJECTED，history-digest §1.2 项 6）——
  同一战役两套 MME。须声明：≤1% 的已分辨回执不构成产品级效应声明、不产生新候选
  槽位（或把 MME 提到与 #19 一致）。(b) 24→120 对阶梯无预注册功效依据：协议自己
  引"#635 先例…0.5% 量级在该分辨力下不可分辨"，又断言"加对数到可分辨"。现有
  数据可估 σ：P1-A 3-rep CV cycles u+k tp-w8 = 1.1%（σ≈337 cyc/op → 120 对半宽
  ≈60-70，勉强可达 77）；#635 rusage A/A 36 对半宽 ≈1.2pp（σ_pair≈3.7% → 120 对
  半宽 ≈200，必 NOT_POWERED）。tp-w8 是 futex 等待主导形态（3.2 futex/op、均值
  42 µs），σ 落高位的概率大。须写入 σ 依据与阶梯的先验可达性判断，或预声明
  NOT_POWERED 为预期结局并相应缩小预算。
- **F6【引用纪律·降级值入裁决列】** candidates.md 非候选 io-wq 行引用
  "io_wq 机器 ≈7–10.5%"：measurement-audit §2-6 已判段7/Top10 两处口径漂移
  （tmpfs 10.5% vs 8.3%）且"均为定位参考，不进裁决"；7.1% 属 tp-w8 行（行 3，
  已按纪律引用）。该行把三者混成一个无出处区间放进裁决依据列，违反本文件头部
  自设的引用纪律。改为按审计标注口径引用（或删数值——该行 NO_GO 方向不依赖它）。
- **F7【冻结描述与 mutant 载体矛盾】** 协议 §2 冻结 "CANDIDATE_SHA = BASE + 仅 §1
  的门控 diff"，而 §3.2 要求"候选提交注册 ≥2 个 mutant（#634 回执格式）"——#634
  的 mutant 是休眠 defines 形态活在产品头文件里（history-digest §4.1：
  `SLUICE_THREADPOOL_MUTANT_OWED_EMPTY_VERDICT_ALWAYS` 在 hpp 内），本候选的两个
  mutant（写侧抑制 + 访问器错报）同样是 threadpool TU/hpp 的代码改动，超出"仅门控
  diff"。须写明 mutant 载体（随 CANDIDATE 提交的休眠 defines——则 §2 描述改为
  "门控 diff + 2 个 test-only mutant defines"，mutant 构建 sha 另行登记；或临时
  patch 载体并登记 diff），否则冻结的单因素声明与 SHA 描述不真。
- **F8【透镜遗漏处置】** pocketjs-method-notes 条目 7 指出 work_mtx_ 同锁护两个
  事实、"锁拆分是次级小空间"；candidates.md §5-4/非候选行 3 以"无最小可减机制"
  概括但未处置该观察。经核实真实约束是 `internal_work_retired()` 的同锁联合快照
  （`threadpool_backend.cpp:856-861`：dispatch 空 + active_workers_==0 +
  publication_pending_ 空 + owed 判定同持 work_mtx_；dag.md M-29 同源）——拆分即
  破坏 settlement 联合快照。在 §5 补一句该绑定引用即可闭合"无新空间"的完备性声明。

## 3. 保留顾虑（披露给 owner，不阻断）

- **S1 性能端点的现实结局**：tp-w8 等待主导（futex 均值 42 µs ≫ CPU/op 10.35 µs 的
  指令部分），A/A 单对噪声大概率偏大，NOT_POWERED 是合理预期；裁决本就走机制门
  独立路径、回执端点（§4-2 −1.000 instr/op + 三列存证）才是决定性证据。owner 可
  权衡是否值得跑满 120 对。
- **S2 free-list 先例 vs m1 REJECT**：透镜条目 9 以仓内 `cookie_free_list_`
  （`uring_transport.cpp:580` 实核在库）为 O(S)→O(1) 的低风险先例，而 #14 引
  m1-results REJECT（dag.md M-01）。REJECT 的原始理由不在本轮输入内，仓内先例
  是否重开该裁决属 owner 问题，矩阵未提。
- **S3 R2/R3 身份不可辨**：history-digest §1.2 项 6 未说明 r6 的 R2（0.90%）/R3
  （0.45%）各指什么候选；`PublicationEpiloguePauseGate`（`uring_backend.hpp:137/
  333`、`threadpool_backend.hpp:88/271` 实核）是发布 epilogue 暂停缝，提示 R3 指向
  M-61 族（#16 的对位）而非 M-67。#19 的"对位"需按 r6 F.6 原文更正或删除；若 R3
  确为发布族候选且已 REJECTED，#16 的翻案条件也应引用该先例。本轮输入无法裁定，
  留给 owner。
- **S4 close_admission 计数**：dag.md/candidates.md 的"仅 :468,471"漏计同文件
  helper 调用点（:481,495,496）；结论（生产零调用、所有权裁决）不受影响。
- **S5 判词 4 的标签**：即使 F5(a) 补充声明后，"性能 GO"字样仍可能被下游当作
  产品信号；建议改称"回执分辨达成"（可选）。

## 4. 复核命令记录（本会话实跑，只读）

- 源码锚点：`sed -n '425,440p' src/async/threadpool_backend.cpp`（:434 fetch_add 无门控）、
  `sed -n '250,262p'`/`sed -n '60,80p' include/sluice/async/detail/threadpool_backend.hpp`
  （:258 成员无门控、:66 起访问器宏块）、`sed -n '1,10p'`/`sed -n '100,115p'
  src/async/threadpool_test_seams.hpp`（:4 文件宏、:108-110 定义）、
  `sed -n '215,235p' src/async/uring_transport.cpp`（:222）、
  `grep -n 'live_cookies_|live_control_sqes_'`（:195,:295,:427,:490,:581 全 RMW；
  uring_backend.cpp:350-351,371-372 生产读者）、`sed -n '545,562p'`（M-53 逆向扫描）、
  `sed -n '40,60p' src/async/request_scope.cpp`、`sed -n '575,585p'`（cookie_free_list_）、
  `sed -n '850,865p' src/async/threadpool_backend.cpp`（internal_work_retired 联合快照）、
  `sed -n '458,468p'`、`sed -n '1,20p' include/sluice/async/detail/reference_ready_sink.hpp`、
  `sed -n '470,485p' src/async/detail/request_core.cpp`、`sed -n '258,276p'`（M-13）、
  `sed -n '108,118p' src/async/uring_completion.cpp`（exact-submitted + fetch_sub）、
  `ls bench/mechanisms/`、`grep -n close_admission tests/progress_source_ownership_test.cpp`。
- bench/tests：`sed -n '455,470p' bench/bench_w02_pipeline.cpp`（case_id 拼装）、
  `sed -n '600,650p'`（case 冻结列表，无 d2/d3）、`grep -n kFixtureBytes`（:50）、
  `sed -n '490,502p' bench/bench_common.hpp`（'=' 精确匹配）、
  `grep -n 'threadpool_backend.cpp' xmake/tests.lua`（**12** 处）、
  `grep -c SLUICE_ASYNC_INTERNAL_TESTING xmake/tests.lua`（14）、
  libraries.lua/apps.lua 零命中（apps.lua:4 注释）、`sed -n '1100,1145p'`/`1195,1215p'`/
  `'725,760p' xmake/tests.lua`（三消费目标 define+重编译实核）、
  `grep -rn syscall_count tests/`（16 条，行号逐条命中）、
  `grep -rn syscall_count src/ include/ apps/ bench/`（仅 :434/hpp:72,258/seams:108-109）。
- 交叉输入节：p0c §4.2/§5.2/§7 条目 4/§9（headers `grep -n '^## \|^### '` 实核，§9 表
  含 D2/D3 NOT_CONSTRUCTIBLE 行）；repo-reality §2.2（12 worktree 保护清单）/§4
  （#520 禁令）；`sed -n '583,590p' docs/explicit-io-contract.md`（:586 borrow facts）。
- **未执行**：任何构建、测试或基准（协议为草案、本挑战为只读审查）。

---

# 复审 r2（2026-11-18，裁决官按 F1–F8 修订后）

## r2.0 结论

原 F1–F8 **八项全部实质修复**（逐项核验见 r2.1）。但修订文本引入/遗留 **5 处小型
内部一致性缺陷（R1–R5）**，均属预注册文档不允许的含糊或行号错配，一行可修——
**仍不通过（approved = false）**，修复 R1–R5 后即可进入执行。

## r2.1 原缺陷修复核验（全部实跑复核）

- **F1 ✓**：协议 §2 负载集改用真实 case_id（W02-threadpool-4k-d16-w8 / -w4 /
  W02-uring-4k-d16 / W02-threadpool-64k-d16-w4）并写明拼装规则与"缩写禁止使用"。
  行号核验：`grep -n "BackendChoice threadpool\|BackendChoice uring" bench/bench_w02_pipeline.cpp`
  → **:621/:622**（r2 记录准确）；`grep -n "CaseMeta make_meta"` → :460、case_id 拼装
  :462-467 ✓。陈旧值扫描：两文档内残留的 "W02-tp-…"/"≈7–10.5%"/"10 处" 全部位于
  r2 修订说明自身，无操作性残留（`grep -n "10 处\|W02-tp-\|≈7–10.5" 两文件` 实跑）。
- **F2 ✓**：#12 改判 NO_GO→INCONCLUSIVE，依据改写为 fetch_add/fetch_sub RMW 计数
  （:195,:581,:295,:427,:490 + uring_completion.cpp:118 = 每 read op 2 条 lock RMW，
  control op 另计——与挑战认定一致）；生产读者 :350-351/:371-372、缝读者
  `uring_test_seams.hpp:79-80`（实核：`sed -n '74,82p'` → live_cookies_for_test
  恰在 :79-80，r2 行号正确）；dag.md M-40 同源前提错误与 Top10 #10 伞标签不准确
  均已登记；翻案条件含"dispatch_mtx_ 覆盖重证 + 缝读者加锁设计"。残留 R5。
- **F3 ✓**：12 处重编译点完整列出（:406,448,501,559,745,818,891,959,1015,1074,
  1139,1205），初版 `| head` 截断成因如实披露；candidates §3/§5-1/§6 与协议 §3.1
  同步更新。残留 R1（协议 §3.1 一处行号错配）。
- **F4 ✓**：判词 1 扩为中止通道 a–d：1b 计数层回执违例（§4-4 新增映射）、1c uring
  隔离格数值判据、1d 64KiB 回归绊线（CI 下界 > +2% → halt-and-investigate，并如实
  引 #635 −1.4611% [−2.7425,−0.2340] 先例、"非零是现实可能"）。残留 R2/R3/R4。
- **F5 ✓**：(a) 双 MME 管辖声明落 §5——0.50pp 只辖本候选性能分辨，1% MME 辖新候选
  准入，"≤1% 已分辨回执不构成产品级效应声明、不进 ledger 产品行、不产生新候选槽位，
  升格须按 r6 F.6 重新立项"，判词 4 内联同款告诫；(b) 功效先验冻结——σ=337
  （P1-A CV 1.1%）、配对 120 对半宽 ≈61（可达）、独立 √2 放大 ≈477→≈86（不可达）、
  #635 外推 σ_pair≈3.6%→≈200（必 NOT_POWERED，标注跨负载外推仅量级警示）、futex
  主导高位先验、**NOT_POWERED 预声明为现实结局**、扩展决策只依试点实测 σ_pair。
  算术复核：1.98×337/√120≈60.6；1.2045pp×6/2.03≈3.56%（σ_pair）→ 1.98×1103/10.95≈199。
- **F6 ✓**：io-wq 行撤出 ≈7–10.5%，改用审计 §1.2 逐值复核计数（punt 1.0000/op
  540028/540028、Δcycles:k +7304、Δtask-clock +2.87、Δctx-sw +0.230），并显式声明
  io_wq 符号合计份额"定位参考、口径漂移已登记、本行不引用其数值"。
- **F7 ✓**：§2 CANDIDATE_SHA 描述改为"门控 diff + 2 个休眠 test-only mutant
  defines"（#634 载体先例、mutant 构建二进制 sha 另行登记、常规构建行为零差异、
  生产 diff 仍仅门控）；§3.2 写明载体（写侧抑制 mutant 包在门控后测试宏块内、访问器
  错报 mutant 在 threadpool_test_seams.hpp）。mutant 杀伤设计复核可行：sticky-zero
  被 ==1/==2/==4 断言杀（:885==0 会过但 :380/:1133/:943/:1189/:463 杀）、skew ±1 被
  全部精确断言杀。
- **F8 ✓**：§5-5 新增 settlement 联合快照绑定（threadpool_backend.cpp:856-861、
  dag.md M-29、pocketjs §三-7 互证），§5-6 结论与 tp-w8 非候选行同步锚定。

## r2.2 残留缺陷（R1–R5，必须修复后放行）

- **R1【F3 修复文本内行号错配】** 协议 §3.1："`public_request_target`（:731 起、
  重编译 :448 所在文件表）"——:448 属 `progress_source_target`（helper :435、文件表
  :443-450，`sed -n '430,462p' xmake/tests.lua` 实核），public_request_target 的
  重编译点是 **:745**（`sed -n '731,750p'` 实核：helper :731、define :738、文件表内
  threadpool_backend.cpp :745）。该声明本身为真（目标确实 define+重编译），仅引行错；
  但 F3 的整改对象恰是这份安全面清单，行号必须准确。改 ":448" → ":745"。
- **R2【判词 1 头与 1d 处置矛盾】** 判词 1 头部"任一触发即实验中止、候选 NO_GO"
  与 1d"halt-and-investigate：纯计数器移除不可能真实回归，回归指示构建/配置错误…
  格为报告性"冲突——若 1d 触发且根因为构建/配置 artifact，正确处置是判词 2 的
  session 作废重建通道 + 根因记录，而非自动 NO_GO。在 1d 写明处置路由（artifact →
  重建重跑；排除 artifact 的真实回归 → 中止 NO_GO）。
- **R3【§6 行 1 标签未同步】** §6 uring 隔离格行仍写"保留（报告性）"，而判词 1c
  已把隔离违例定为中止通道；64KiB 行已同步"判据见 §5 判词 1d"，行 1 缺"判据见
  §5 判词 1c"。一行同步，消除"报告 vs 中止"的双读。
- **R4【判词 1c 样本量未冻结】** 1c 冻结了"隔离格先跑 6 对同 pass 型 A/A 得
  σ_uring"，但未冻结 uring A/B 配对 Δ 的对数（"95% CI 不含 0"在 n 未定下不可执行）；
  且 "max(2σ_uring, 隔离格均值 1%)" 措辞含糊（应写明"隔离格 cycles/op 均值的 1%"）。
  补 A/B 对数（建议与主格试点同量级并写死）并澄清量纲。
- **R5【F2 记录缝读者枚举缺一】** candidates.md §6 F2 记录的测试缝读者仅列
  uring_test_seams.hpp:79-80；`live_control_sqes_for_test` 在
  `src/async/uring_transport.cpp:38-40`（文件级测试宏块内、#endif 于 :41，实核
  `sed -n '33,42p'`）——降级设计的"缝读者加锁"面必须完整（F3 同类教训）；同函数
  `sq_ready_for_test`（:33-36）已持 dispatch_mtx_ 可作先例。补 :38-40。

## r2.3 保留顾虑（更新）

- **S1（已弱化）**：NOT_POWERED 已被协议预声明为现实结局并由功效先验覆盖；剩余
  只是 owner 对"是否值得跑满 120 对"的成本权衡。
- **S2（不变）**：free-list 先例（cookie_free_list_，uring_transport.cpp:580）vs
  m1-results REJECT 的重开问题，属 owner 裁决，#14 翻案路径未提及。
- **S3（不变）**：r6 R2（0.90%）/R3（0.45%）身份在可得输入内不可辨；R3 的
  PublicationEpiloguePauseGate 证据面提示其指向 M-61 发布族（#16 的对位）而非 M-67。
  #19 的"对位"引用维持原样（r2 未改），留 owner 按 r6 F.6 原文澄清。
- **S4（不变）**：close_admission"仅 :468,471"漏计同文件 helper 调用点
  （:481,:495,:496）；结论不受影响。
- **S5（基本解除）**：判词 4 已内联"≤1% 回执不构成产品级效应声明"告诫，标签歧义
  余量可接受。

## r2.4 复核命令记录（本会话实跑）

- `grep -n "BackendChoice threadpool\|BackendChoice uring" bench/bench_w02_pipeline.cpp`
  → :621/:622；`grep -n "CaseMeta make_meta"` → :460。
- `sed -n '430,462p' xmake/tests.lua` → :448 属 progress_source_target（helper :435、
  define :441、文件表 :443-450）；`sed -n '731,750p'` → public_request_target
  helper :731、define :738、重编译 :745。
- `sed -n '74,82p' src/async/uring_test_seams.hpp` → live_cookies_for_test :79-80 ✓；
  `sed -n '33,42p' src/async/uring_transport.cpp` → live_control_sqes_for_test
  :38-40 在测试宏块内（#endif :41）。
- `grep -n "10 处\|W02-tp-\|≈7–10.5\|7–10.5" candidates.md experiment-protocol.md`
  → 残留命中全部位于 r2 修订说明自身，无操作性陈旧值。
- 两份修订文档全文重读；#12 改判、判词 1a–1d、双 MME 声明、功效先验、mutant 载体、
  §5-5 绑定逐段核对。未执行任何构建/测试/基准（协议仍未冻结）。
