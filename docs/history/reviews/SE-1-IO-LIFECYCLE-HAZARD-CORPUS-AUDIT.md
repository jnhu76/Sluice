# SE-1 — I/O Lifecycle Hazard Corpus — Completion / Bias Audit

**VERDICT-CLASS ARTIFACT: untracked human review artifact（按任务 §30 不 commit、不开 PR）。**
本报告是 SE-1 的收口审计记录；仓库内权威是 tracked 的
`docs/results/safety/se1-hazard-corpus.json`（DATA AUTHORITY）与
`docs/verification/se1-hazard-corpus.md`（解释），以及 Draft PR。

- 审计日期：2026-08-30
- 执行者：编码代理（SE-1 任务书授权，#227 Lane A）
- PR：https://github.com/jnhu76/Sluice/pull/245 （DRAFT，未 ready、未 merge）

---

## VERDICT

**A. SE-1 PASS — BIDIRECTIONAL HAZARD CORPUS FROZEN, READY FOR SE-2**

不选 B：无 NO-VALID-ENTRY 族（13/13 族全部有 REAL 或 DOCUMENTED 锚点）。
不选 C：5 个 C0 一手来源 + 8 个 C1 权威文档来源，常规半区出处充分。
不选 D：schema v1 冻结、校验器 PASS、pairing 词表闭合，comparability 已逐条标注（含 1 条诚实 BLOCKED）。
不选 E：未发现新的实质性 production bug（唯一 OPEN 的 group.hpp idiom 为既有登记缺陷，非本次发现，已按 §17 裁决 OUT-OF-SE1 保留）。

按任务书 §32："Do not manufacture A" —— 本裁决的支撑边界见 BIAS AUDIT 与 CLAIMS STILL FORBIDDEN：A 的含义仅是"语料冻结且覆盖完整"，不是任何安全性结论。

---

## BASE / HEAD

```text
SE1_BASE_SHA = 7437c8c58209f239051a8e814fd7ab44eabaada5
git fetch origin 已执行；HEAD == origin/master == 7437c8c（机械验证，与授权声明一致）
branch: master（只读阶段）→ research/se1-hazard-corpus（施工阶段）
HEAD（branch tip）= f152c08（3 commits）
工作树：仅既有 3 个人工 untracked 报告，全程未动；无 tracked 脏状态
```

Commits（语义内聚、逐个可回滚）：

1. `92eb80c` research(safety): freeze SE-1 hazard corpus (schema v1, bidirectional entries)
2. `703b947` test(research): add SE-1 corpus integrity validator (fail-closed)
3. `f152c08` docs(safety): record SE-1 corpus interpretation and bias audit

JSON 未按"schema / conventional / induced"拆成三个中间 commit，因为单文件部分暂存会产生语法无效的中间态，违反"每个 commit 可独立构建/审查"的仓库纪律；改为单 commit 冻结全量 schema+条目。

---

## CORPUS SCHEMA

- `se1-corpus-schema` version **1**，位置 `docs/results/safety/se1-hazard-corpus.json`（先核验了仓库约定：`docs/results/` 是机器可读证据产物的既有权威位置，`safety/` 为新子目录）。
- 闭集枚举：origin / primary_bucket（6）/ corpus_eligibility / provenance_quality（C0–C3,S0）/ sluice_current_status（8）/ comparison_validity / pairing（PAIR-A..X）。
- 比较单元 = `normalized_semantic_trace`；API 拼写只是 provenance。
- score 类字段被校验器结构性禁止；禁令性 net-safety claim 全文匹配禁止。
- 校验器 `scripts/verify-se1-hazard-corpus.py`：fail-closed，本地运行，**未接入 CI/pre-push**（等人工评审授权）。

---

## FAMILY COVERAGE H01-H13

| Family | Outcome | Quality | Entry | Pairing |
| --- | --- | --- | --- | --- |
| H01 lifetime/buffer UAF | DOCUMENTED (+MINIMAL companion) | C1+C3 | SE1-CA-H01-1/-2 | PAIR-E |
| H02 reuse/generation ABA | DOCUMENTED | C1 | SE1-CA-H02-1（POSIX aio aiocb 复用 UB） | PAIR-B |
| H03 stale completion/after-close | REAL | C0 | SE1-CA-H03-1（CVE-2023-1872） | PAIR-C |
| H04 cancel-vs-complete | DOCUMENTED | C1 | SE1-CA-H04-1（io_uring cancel best-effort） | PAIR-A |
| H05 double terminal | REAL | C0 | SE1-CA-H05-1（Axboe double-CQE-for-close 补丁） | PAIR-A |
| H06 submit-failure rollback | DOCUMENTED | C1 | SE1-CA-H06-1（io_uring_enter 提交语义） | PAIR-C |
| H07 partial/zero-progress | DOCUMENTED | C1 | SE1-CA-H07-1（OpenSSL SSL_write） | PAIR-A（PARTIAL validity） |
| H08 timeout-vs-grant | DOCUMENTED | C1 | SE1-CA-H08-1（connect SO_ERROR 协议） | PAIR-A（PARTIAL validity） |
| H09 lost wake | REAL | C0 | SE1-CA-H09-1（glibc BZ 25847） | PAIR-D |
| H10 shutdown w/ in-flight | DOCUMENTED | C1 | SE1-CA-H10-1（libuv EBUSY / Asio 析构弃置） | PAIR-C |
| H11 leak/double-retire | REAL | C0 | SE1-CA-H11-1（io_uring IOPOLL/CQE_SKIP leak fix） | PAIR-C（PARTIAL validity） |
| H12 durability/ordering | REAL | C0 | SE1-CA-H12-1（PostgreSQL fsyncgate） | PAIR-X（BLOCKED） |
| H13 weak-memory publication | DOCUMENTED | C1 | SE1-CA-H13-1（memory-barriers.txt） | PAIR-D（PARTIAL validity） |

**REAL=5 · DOCUMENTED=8 · CONVENTIONAL-MINIMAL(唯一锚点)=0 · NO-VALID-ENTRY=0。无空格。**

---

## CONVENTIONAL SOURCE QUALITY

| Quality | Count | Cases |
| --- | ---: | --- |
| C0（真实缺陷+修复一手记录） | 5 | CVE-2023-1872；Axboe double CQE close；glibc BZ 25847；Begunkov IOPOLL/CQE_SKIP leak；PostgreSQL fsyncgate |
| C1（权威 API 文档警告） | 8 | Asio buffer lifetime；aio_read(3) aiocb；io_uring_enter（cancel/zero-copy/提交语义）；OpenSSL SSL_write；connect(2)；libuv EBUSY+Asio 析构；memory-barriers.txt；+H01 复用 |
| C2（论文工件/官方语料） | 0 | 本轮未使用（fsyncgate 的 OSDI'16 工件仅作可选支撑，未计入） |
| C3（CONVENTIONAL-MINIMAL） | 1 | SE1-CA-H01-2（显式标注，仅作 SE-2 probe 骨架） |

一手来源核实方式：NVD/STAR Labs（CVE）、lore.kernel.org 线程（两个 io_uring 补丁）、sourceware bugzilla + patchwork（glibc）、PostgreSQL wiki/release notes/LWN（fsyncgate）、man7.org 与官方文档页直抓（全部 C1 条目；引句逐条核对，未凭记忆引述）。

---

## SLUICE-INDUCED INVENTORY

13 个种子逐一重裁（对 THESIS-LEDGER-1 的 13 项 inventory，逐项核对 tracked 证据链：PR #241/#242/#243、AC-2b/2c、#229）：

**IN-SE1（8）：**

| ID | 种子 | Bucket | Family | 当前状态 | 根因 |
| --- | --- | --- | --- | --- | --- |
| SE1-SB-01 | Q-LIV-1 Queue liveness drift | production-runtime | H09 | DETERMINISTICALLY_REPRODUCIBLE（DST witness+修复+4 回归证人） | wake/liveness reconciliation |
| SE1-SB-02 | R2-ALLOC register-before-arm | production-runtime | H06(别名H11) | DYNAMICALLY_DETECTED（人审抓取；已修 prepare-before-register） | temporal ordering |
| SE1-SB-05 | FE P1-1 终局后分配可抛 | production-runtime | H06(别名H05) | FAIL_FAST（noexcept tail + exit 86 + PUB1/PUBCTL） | failure atomicity |
| SE1-SB-06 | FE P1-2 QueuePort lifetime hole | production-runtime | H10 | DYNAMICALLY_DETECTED（pin transfer + QPIN/QD1） | lifetime ownership |
| SE1-SB-07 | FE P1-3 rwlock owner check 在 G 外 | production-runtime | H13(别名H02) | DYNAMICALLY_DETECTED（TSan+M3+死亡子进程） | synchronization |
| SE1-SB-08 | nullable ResumeTarget token | production-runtime | H09(别名H03) | STATICALLY_REJECTED（Kind::none + static_assert + M4） | representation invalid state |
| SE1-SB-09 | #229 test-seam race | test-only | H13 | DYNAMICALLY_DETECTED（TSan） | test-seam defect |
| SE1-SB-10 | select_registry 一次性 TSan hang | test-only | H09 | **UNKNOWN（未确认）** | unknown（疑似 no-progress termination vs plain-wait residency） |

**OUT-OF-SE1（5，全部保留可见）：**

| ID | 种子 | Bucket | 排除理由 |
| --- | --- | --- | --- |
| SE1-SB-03 | deadline heap `reserve(size+1)`（已修） | production-runtime | 资源增长/性能缺陷，非生命周期安全；PR #242 round3 已修+证人 |
| SE1-SB-04 | **group.hpp `reserve(size()+1)`（OPEN，`include/sluice/async/group.hpp:202-203` 本轮在 base 重验）** | production-runtime | 同 SB-03 类；事务性保护已使其生命周期维度干净；§17 明示裁决并禁止为计数而纳入 |
| SE1-SB-11 | ~30 deadline 位点 duplication（AC-2b 已收敛） | structural-authority | 结构性风险无自身具体失败执行（§18） |
| SE1-SB-12 | R2 authority duplication / AsyncQueue drift（AC-2c/FE 已收敛） | structural-authority | 同上；具体 drift bug 由 SB-01 单独承载 |
| SE1-SB-13 | RX-1 18 个无效 CONTROL run | experiment-process | 实验方法缺陷≠产品安全缺陷（§9） |

---

## ROOT-CAUSE DEDUPLICATION

| 根因类 | 主条目 |
| --- | --- |
| authority duplication | SB-11, SB-12（均 OUT，结构性） |
| temporal ordering | SB-02 |
| lifetime ownership | SB-06 |
| failure atomicity | SB-05（R2-ALLOC 的 accounting residue 作别名归 SB-02，未双计） |
| accounting closure | —（无独立主条目） |
| wake/liveness reconciliation | SB-01, SB-10 |
| synchronization | SB-07 |
| representation invalid state | SB-08 |
| complexity/resource-growth | SB-03, SB-04（同根因类跨两 site，别名互链，root-cause map 计一次类） |
| test-seam | SB-09 |
| experiment method | SB-13 |

无"bug + review finding + regression test 计三个 hazard"情形；aliases 字段机械校验解析到存在 ID。

---

## OUT-OF-SE1 REJECTED SEEDS

见上表 5 项 + 常规侧 rejected_candidates 5 项（musl condvar 无法锚定一手 commit；memory-barriers.txt 无逐字 SB litmus——改用其 SLEEP AND WAKE-UP 节并如实记录框架差异；generic UAF 不满足 H01 因果性；Go Dialer 语义不精确；Star Labs exploit 文属二手，改用 Begunkov 一手补丁）。全部留痕于 JSON `rejected_candidates`，防樱桃采摘可审计。

---

## COMPARABILITY AUDIT

- FAIR：7 个常规族（H02/H03/H04/H05/H06/H09/H10）+ 4 个 induced（SB-01/06/07/09）。
- PARTIAL：H01×2、H07、H08、H11、H13 + induced SB-02/05/08/10。
- COMPARABILITY_BLOCKED：H12（fsyncgate 的承重机制——内核清错后二次 fsync 返回成功——在 Sluice 边界之下，任何 user-space 义务都无法表示；Sluice 侧已核验 `src/file.cpp` do_sync 仅 EINTR 重试、typed 错误、WAL 仅成功推进 durable_lsn_，即"策略半区"已表示，"内核机制半区"诚实阻断）+ 结构/过程 OUT 条目。
- 常规半区 pairing 计数：A×4、B×1、C×4、D×2、E×2、X×1；全部 induced IN-SE1 条目即 PAIR-F 实例。

---

## SILENT / UNKNOWN CASES

1. **SE1-CA-H01-1/-2（H01）**：borrow 在请求 in-flight 期间被销毁——义务在契约中显式（caller-owned Completion + borrow 规则），但违反本身 Sluice 无机械检测（ASan 类外部工具才可见）。这是 §16 要求保留的第一类 silent case，已列为 SE-2 优先 probe。
2. **SE1-SB-10（H09, test-only）**：一次性 TSan hang，未复现（20/20 清白）；**证据仅存在于 untracked E4 工件**，tracked 文档中无记录——这本身是文档缺口发现。状态 UNKNOWN，SE-2 优先 probe；若人审确认为真缺陷，将独立成条并做根因分类。

---

## BIAS AUDIT

- **BIAS FOUND（残余，如实记录）：**
  1. 选择熟悉度：Sluice 半区来自项目自身评审史（证据密度高），常规半区需新检索——两侧检索深度不对称；
  2. C2 级常规来源为零（论文工件/官方语料未纳入）；
  3. C1 锚定的 4 个族（H07/H08/H13 及 H04 部分）存在更强 C0 来源的可能性。
- **CORRECTIONS：**首轮草稿后执行对抗过审——保留全部不利案例（H01 silent 对、H12 BLOCKED、SB-10 UNKNOWN、整个 induced 半区）；OUT 裁决对称适用（若把 SB-03/SB-04 计入反而会人为抬高"Sluice 造 bug"侧，同样被拒）；rejected_candidates 落盘；silent/unknown 案例升格为优先 probe 而非脚注。
- **UNRESOLVED BIAS：**上述三项残余在 SE-1 范围内不可消（需 SE-2/后续检索轮）；已写入 PR body 请求人审定向核查 H07/H08/H13。

---

## NEW FINDINGS

- **P0/P1：无。**
- **P2：**`select_event_registry_test` 一次性 TSan hang 的记录仅存于 untracked 工件，无 tracked 文档——建议人工裁决后要么补 tracked 记录、要么明确销案（对应 SB-10）。
- **P3：**group.hpp `reserve(size()+1)` 维持 OPEN（既有登记，非新发现）；本轮未修（§17/§23 禁止）。附带核验：该 site 已有事务性 reserve-before-register 保护，生命周期维度干净，纯增长/性能维度。
- 未发现新的 production 生命周期缺陷；SE-1 的轻量检视（completion/arena/scheduler 锚点抽查、sync 路径重试检查）未产生新失败模式假设。

---

## SE-2 READINESS

- SCHEMA STABLE：**YES**（v1 冻结，fail-closed 校验器 PASS：27 条目；conventional C0=5/C1=8/C3=1；induced IN-SE1=8；OUT=5；REAL=5/DOC=8/MINIMAL=0/NO-VALID=0）。
- CORPUS FROZEN：**YES**（Draft PR #245，base 7437c8c，3 commits）。
- CAN SE-2 CONSUME WITHOUT REDESIGN：**YES**——每条目已携带 `se2_probe_candidates`；既有检测证据（TLA+/GenMC/DST/死亡测试/变异/TSan 证人）已按 §22 只记录、不成矩阵；闭集枚举使 hazard×layer 矩阵可直接外积。
- SE-2 首批建议 probe：H01 borrow-销毁 probe（当前 SILENT 对）；SB-10 定向 stress+TSan 复现；H12 边界文档化 probe（验证"不可表示"论断本身）。

---

## CLAIMS STILL FORBIDDEN

Sluice is safer than POSIX/liburing/Asio；reduces bugs overall；prevents most async bugs；superior concurrency safety；formal verification 证明实现正确；DST 系统性覆盖并发；任何 net-safety score。本 corpus 的一切计数（FAIR/PARTIAL/BLOCKED、8 种 status 分布、bucket 分布）均为描述性，不构成得分。T-S1a 保持 PER-HAZARD SCOPE；T-S1b、T-S2 保持 **NOT YET TESTED**。

---

## FINAL NEXT STEP

**HUMAN ADVERSARIAL CORPUS REVIEW**（PR #245，DRAFT）。

人审定向建议：(1) 常规半区 H07/H08/H13 找更强 C0；(2) H12 的 BLOCKED 裁决是否接受；(3) SB-10 的处置（补 tracked 记录或销案）；(4) 校验器是否授权接入 CI/pre-push；(5) induced 半区是否有 #227 窗口内被漏检的 hazard。SE-1 侧停止：不开 ready、不 merge、不更新 #227、不启动 SE-2。

---

# SE-1-CORRECTIVE-1

执行日期：2026-08-30。BASE = f152c08（旧 PR head，机械核对 gh pr view 245 == HEAD）；NEW_HEAD = a9b6ea7（2 个 additive corrective commits，未 rewrite 原有三提交）。全程未动 4 个人工 untracked 工件。

## B1 H08

old source:
- connect(2) man page（EINPROGRESS → writability → SO_ERROR），C1 DOCUMENTED。
- 不支持步骤：'caller treats timeout as terminal while the grant (connection) also arrived' —— connect(2) 文档只定义完成检查协议，不定义 timeout↔grant 仲裁；该承重步骤是自插推断。

new source / NO VALID ENTRY:
- **REAL SOURCE FOUND（C0）**。io_uring linked timeout 与其绑定请求的完成竞态（真实上游 bug + 修复）：
  - bug record：lore.kernel.org/io-uring/69c46bf6ce37fec4fdcd98f0882e18eb07ce693a.1620990121.git.asml.silence@gmail.com/ —— Begunkov "[PATCH] io_uring: fix ltout double free on completion race"（2021-05-14），Reported-and-tested-by syzbot，Fixes: 90cd7e424969d；commit message 明确竞态语义（'we race with the completion of the linked work'，双 put → UAF）。
  - upstream fix：mainline 447c19f3b5074409c794b350b10306e1da1ef4ba（torvalds tree 上机械核验 subject/diff：fs/io_uring.c，unlink 先于 refcount grab，经 io_uring-5.13 pull 合入）。
  - official contract：liburing io_uring_prep_link_timeout 文档 —— 'if the linked request does not complete before the timeout, it is cancelled with -ETIME'，契约本身定义仲裁（恰好一个终局）。
- SOURCE FACT（验收测试）：请求 R 绑定超时 T；T 触发与 R 完成竞态；契约规定唯一终局（R 真实结果 或 R 被 -ETIME 取消）；败者必须 unlink+retire 恰好一次。ADDED INFERENCE: NONE。SLUICE COMPARISON：same-object（link timeout 绑定同一请求 ↔ Sluice deadline 参与同一 wait 的 resolve_ CAS）、operational。COMPARISON VALIDITY：**FAIR**（旧 PARTIAL 源于 connect 的 cross-object 错配）。

verdict: H08 = C0 REAL SOURCE FOUND，PAIR-A / FAIR / UNREPRESENTABLE。旧 connect 锚降级进 rejected_candidates。H08 不再需要 coverage gap；未发生"制造 13/13"——13/13 是用真实一手来源补足的。

## B2 provenance

old mechanical rule:
- quality ∈ {C0,C1,C2} ⇒ sources 字符串含 "http"（+ conventional-real ⇒ C0/C2）。等同只查 URL 存在性；validator 注释声称 root-cause dedup 但实际只查 alias 目标存在。

new mechanical rule:
- provenance.sources → 结构化 {url, role, authority[, note]}；闭集 role ∈ {bug_record, upstream_fix, official_contract, official_bug_corpus, repo_evidence, supporting}；authority ∈ {upstream-primary, official-primary, repo-primary, supporting}；role↔authority 一致性表机械校验。
- 质量契约：C0 ⇒ ≥1 bug_record 且 ≥1 upstream_fix（同一 URL 双 role 允许，但"该工件确同时含诊断+修复"留给人审）；C1 ⇒ ≥1 official_contract；C2 ⇒ ≥1 official_bug_corpus；S0 ⇒ ≥1 repo_evidence；C3 禁止 bug_record/upstream_fix role；supporting 永不满足 C0/C1/C2/S0。
- validator docstring 只声明机械检查，显式区分 MECHANICAL PROVENANCE SHAPE 与 HUMAN SEMANTIC PROVENANCE REVIEW。

H03 fix source:
- **机械核验推翻了 advisory 的标签**：STAR Labs advisory 标注的 "primary fix" da24142b1ef9 在 torvalds tree 404、在 stable tree 实为另一提交（"io_uring: ensure that io_init_req() passes in the right issue_flags"）——不是 CVE 修复本体，未采用。
- 已验证修复：stable 08681391b84da27133deefaaddefd0acfa90c2be "io_uring: add missing lock in io_get_file_fixed"（作者 Billy Jheng/STAR Labs；committer Greg K-H 2023-03-03）——对被引用的 fixed-file UAF 逐项核对成立（对 io-wq 路径的 fixed-file 查找加 uring 锁，正对"unregister 竞态 free"根因）；其 message 诚实记录 "No single upstream patch exists for this issue, it was fixed as part of the file assignment changes that went into the 5.18 cycle"。
- 角色如实登记：NVD = official_bug_corpus/official-primary；STAR advisory = bug_record/supporting；08681391 = upstream_fix/upstream-primary。
- H05/H09/H11/H12 的 C0 链同步结构化：H05/H11 用"同一工件双 role"编码（诊断+修复同件）；H11 补了 v2 message-id permalink（替换 lore 搜索页 URL）；H09 bugzilla+patchwork 分列 bug_record/upstream_fix；H12 wiki=bug_record(official-primary)、release notes=upstream_fix。

mutation evidence: 见下 §MUTATIONS（M1/M1b/M2 全 RED，字节级还原）。

## B3 population/dedup

old alias ambiguity:
- `aliases` 混两义：H01-2 指向 H01-1（同语义人群 case/probe 伴随）与 SB↔CA 互指（语义相关条目）；validator 声称 dedup 实际只查 alias 目标存在，不保 denominator 完整性。

entry_role: 闭集 {population-case, probe-companion}；全 27 条目必填；probe-companion 不入任何 denominator。
same_case_as: 仅 probe-companion 可携带；指向存在且为 population-case 的父条目；family 不匹配需显式 justification（当前无豁免案例）。H01-2 → same_case_as=SE1-CA-H01-1。
related_entries: 语义/族际关系专用（SB↔CA 互链、SB-03↔SB-04、SB-11↔SB-12）；目标存在性机械校验；绝不影响计数。
root_cause_key: 8 个 induced population case 必填非空、全局唯一（重复即 FAIL）；键为具体根因实例（如 timed-admission-register-before-timer-arming-alloc），广类另存 root_cause_class。
population formula: POPULATION = corpus_eligibility==IN-SE1 AND entry_role==population-case（冻结进 JSON population_law 字段 + validator + 本文档 + PR body）。
new counts: 记录 27 = population 21（conventional 13 + induced 8）+ probe companion 1 + OUT-OF-SE1 5。质量（conventional population 内）：C0=6（H03/H05/**H08**/H09/H11/H12）、C1=7、C2=0、C3=0（C3 唯一条目 H01-2 已移出 denominator）。族覆盖：REAL=6、DOCUMENTED=7、MINIMAL_ONLY=0、NO_VALID_ENTRY=0。状态分布（21 population）：UNREPRESENTABLE 4 / STATICALLY_REJECTED 2 / DYNAMICALLY_DETECTED 4 / FAIL_FAST 4 / DETERMINISTICALLY_REPRODUCIBLE 4 / SILENT_OR_UNDETECTED 1（H01-2 移出后由 2 降 1）/ UNKNOWN 1 / NOT_APPLICABLE 1。comparability（population）：FAIR 11 / PARTIAL 9 / BLOCKED 1。

## N1 H03

FAIR → **PARTIAL**（共享 in-flight vs resource-lifetime 语义；未机械证明 Sluice 侧存在与 kernel fixed-file table 等价的 registered-resource slot lifecycle——Sluice fail-fast 覆盖的是"有 accepted 请求时销毁 backend/context"这一相邻但不同的资源权威）。族映射保留，条目未删。

## N2 H12

两半区分写入 comparison_notes：policy half = PARTIALLY COMPARABLE（fsync 错误→该不该盲重试/该不该推进 durability marker；Sluice 有对应行为：非 EINTR 错误 typed 传播、durable_lsn 仅成功推进）；kernel mechanism half = COMPARABILITY_BLOCKED（内核清 dirty/error 态、二次 fsync 返回成功——在 Sluice 边界之下）。整体维持 PAIR-X / BLOCKED。不声称 Sluice 解决 fsyncgate，也不把整族说成无意义。

## N3 status semantics

sluice_current_status 保持标量，JSON 新增 status_semantics 权威声明：PRIMARY DESCRIPTIVE OUTCOME FOR CORPUS NORMALIZATION ONLY；非完整 detection profile（FAIL_FAST/DETERMINISTICALLY_REPRODUCIBLE/TSan-detectable 现实中可重叠）；hazard × detection-layer 矩阵归 SE-2。已同步 JSON、本文档（§0/§6）与 Markdown。

## CI integration

- 接线点：`scripts/gates/pre-push.sh` 新增 Gate 5e（file-scoped、无 diff 依赖——与 failure-envelope 同一先例）；CI "Repository mechanical gates" step 复用同一 `pre-push.sh --range`，无新 workflow。
- 成功属性已验证：M1–M5 与 M-CI1/2/3/4 对应形态全部 RED（见下）；本地全门禁 `bash scripts/gates/pre-push.sh` ALL CHECKS PASSED；推送后 CI 在最终 head a9b6ea7 上执行（结果见最终报告）。

## MUTATIONS（全部临时改 → validator 必须红 → 字节级还原，sha256 核对 YES）

| # | 变异 | validator 结果 |
| --- | --- | --- |
| M1 | H08 删 upstream_fix role | FAIL: C0 requires >=1 bug_record AND >=1 upstream_fix |
| M1b | H08 C0→C1（覆盖失效） | FAIL: coverage_gate overstates provenance |
| M-CI1 | H08 整条降 OUT-OF-SE1 | FAIL: OUT-OF-SE1 entry missing exclusion_reason（更早的 fail-closed 拦截） |
| M2 | H09 删 bug_record role | FAIL: C0 requires… |
| M3 | SB-09 复制 SB-07 的 root_cause_key | FAIL: duplicate root_cause_key |
| M4a | H01-2 probe→population（保留 same_case_as） | FAIL: population-case must not carry same_case_as |
| M4b | H01-2 probe 丢 same_case_as | FAIL: probe-companion requires same_case_as |
| M5 | 顶层加 safety_score | FAIL: score-like field forbidden |

## final SE-1 verdict

**SE-1-CORRECTIVE-1 PASS — CORPUS EVIDENCE INTEGRITY CLOSED, SE-1 READY FOR HUMAN FREEZE**（终裁选项 A；H08 找到并核验了真实 C0，无 coverage gap；三条机械真命题成立：SOURCE 支持 NORMALIZED TRACE、PROVENANCE QUALITY 匹配 VALIDATOR CLAIM、POPULATION CASE 匹配 DENOMINATOR）。SE-1 corpus 终局裁决维持：13/13 族有 REAL/DOCUMENTED 锚点，等 human freeze；SE-2 不得启动。


---

# ADDENDUM — SE-1-CORRECTIVE-1 ROUND 2（PR #245 review 5060124249，2026-08-30）

人审复核结论：**SE-1-CORRECTIVE-1: PARTIAL PASS — B1/B3 CLOSED; B2 primary-provenance + SB-08 bucket authority 需窄修**。本轮按授权执行三刀，不扩 corpus、不动 production、不启动 SE-2、PR 保持 Draft。

## FIX 1 — C0 primary-incident 语义 + origin↔quality 机械约束

- validator：C0 现在要求 **>=1 primary-authority incident record**（`bug_record` 或 `official_bug_corpus`，authority ∈ {upstream-primary, official-primary}）**+ >=1 upstream_fix**；supporting-authority 的 `bug_record` 只作注解，不再能成立 C0。新增封闭 origin↔quality 关系：conventional-real→{C0,C2}、conventional-documentation→{C1}、conventional-minimal→{C3}、sluice-induced→{S0}。
- 数据：H03 的 STAR Labs advisory 由 `bug_record/supporting` 降为 `supporting/supporting`；H03 的 C0 现在唯一落在 NVD（official_bug_corpus/official-primary）+ 08681391（upstream_fix/upstream-primary）。

## FIX 2 — SB-08 桶归位

`primary_bucket`: production-runtime → **internal/seam**。权威依据：#227 issue comment（bucket taxonomy 锁定）原文 "internal/seam representation hazard: nullable Fiber-kind token"，与 production runtime hazards 正交分列，"Do not mix these counts in SE-1/SE-2 net-safety statistics"。仅桶归位，induced population 总数不变。

## FIX 3 — SB-10 unconfirmed candidate（P2，本轮一并完成）

- `corpus_eligibility`: IN-SE1 → **OUT-OF-SE1** + exclusion_reason（UNCONFIRMED CANDIDATE：唯一证据是 untracked E4 工件、20/20 复跑清白、UNKNOWN）。
- 唯一 source authority: repo-primary → **repo-untracked**（新 authority，validator 同步引入）。同一 untracked 文件在 SB-11 的次要引用同步改为 repo-untracked（SB-11 仍有 tracked PR #238 源支撑 S0；SB-12 的源本就是两个 tracked PR，无此问题）。
- 升级法（population_law 声明）：SE-2 复现 → 以正式条目进入 population 并做根因分类；始终不能复现 → 显式销案。保留 ≠ 算作已建立 hazard。

## validator 负向探针（fail-closed 证据，全部临时变异、验证后还原）

| # | 场景 | 结果 |
| --- | --- | --- |
| N1b | 复现人审场景：supporting `bug_record` + upstream_fix、无 primary incident | FAIL: C0 requires >=1 primary-authority incident record … |
| N1 | official_bug_corpus 配 supporting authority | FAIL: role 'official_bug_corpus' may not carry authority 'supporting' |
| N2 | conventional-real 配 C1 | FAIL: origin cannot carry provenance_quality 'C1'（allowed ['C0','C2']） |
| N3 | E4-only（全 repo-untracked）条目改回 IN-SE1 | FAIL: must be OUT-OF-SE1 (unconfirmed candidate) |
| N4 | C0 无 upstream_fix（旧规则回归） | FAIL: C0 requires … AND >=1 upstream_fix |

## 正向结果（新 counts）

```text
PASS: SE-1 hazard corpus integrity
  records total:         27
  population cases:      20 (conventional 13 + induced 7)
  conventional quality:  C0=6 C1=7 C2=0 C3=0
  probe companions:      1   OUT-OF-SE1: 6
  family outcomes:       REAL=6 DOCUMENTED=7 NO_VALID_ENTRY=0
  induced root causes:   7 unique keys
induced buckets (IN-SE1): production-runtime 5, internal/seam 1 (SB-08), test-only 1 (SB-09)
```

其余机械门禁：check-doc-links PASS、verify-architecture-docs OK、claim-hygiene OK、`git diff --check` clean。CI 结果以 push 后 PR #245 实际 run 为准。

## round-2 verdict

**SE-1-CORRECTIVE-1 ROUND 2: 三刀完成，等待 CI + 人审终裁 freeze。**SB-10 为 SE-2 priority probe；SE-2 仍不得启动；PR #245 保持 Draft。
