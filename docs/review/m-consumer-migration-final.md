# M Phase Consumer-Migration Final Record (M5)

Phase M（#402 Shared Migration Track M / 控制议题 #458）消费者迁移前提工作的终态记录。
性质：终态记录与门状态快照，不是迁移执行报告——本轮实测确认 **M0 以来零迁移执行**（见 §2）。
门状态计数口径（全文统一，行内不再重复）：

- **按迁移/裁决门计：六族 BLOCKED 或上游受阻**（M-R、M-H、M-F、M-A1 四族 BLOCKED；
  M-A2、M-A3 上游受阻）。
- **M-A4 单列**：无 M 迁移门被阻（#458 §8 该行门名即"—（已隔离，无 M gate；B-01 复核条款）"），
  其 BLOCKED 是**记录收口级**（记录票 #463 OPEN + F5 残余"整树删除 vs 保留隔离"未落）。
- 因此"七族全部 BLOCKED/上游受阻"（#458 §8 七行门表口径）与"六族迁移被阻"（迁移门口径）
  两个说法在本文各自出现时均指上述确定含义。

- 权威顺序：root v1-r3 > adopted ADR > #402 现行正文 > F0 adopted > F1 adopted >
  ledger/tests/formal models > live implementation > historical docs（M 铁律：禁止从名字推断处置；
  被删除的 consumer 不是被迁移的 consumer；禁止 multi-worker→single-owner 静默转换）。
- F1 冻结基线（`docs/review/f1-*.json`）未改写；本轮校验输出写 `/tmp/m-final/`（路径易失，
  承重输出已逐字内联进本文 §3/§7）。
- 发布状态：本文经 `m/m5-final` 分支 PR 合入 master（PR 号与 merge SHA 见 #402/#458 终态
  评论与 `git log`）。本文所有"HEAD/END_SHA"指 **M5 测量时点**的 master `1c2a9ab8…`；
  本文自身合入后 master 前进，不在任何一处测量窗口内。

## 0. Baseline、SHA 与编号图例

| 项 | 值 | 来源（本轮实测） |
|---|---|---|
| START_SHA（M0/M1 BASE，= F1 adopted） | `c782099f56c6afd62daad2929944673059aef573` | census §0；PR #456（**F1 clean-room package baseline**）merge SHA（`gh pr list --state merged`） |
| END_SHA（M5 测量时点 master HEAD） | `1c2a9ab8883f7afc5e131e9f7c2845e639b88c3a` | `git rev-parse HEAD`；工作树 clean |
| START..END 提交 | 4 个 docs-only 提交 + 1 个 merge（4ac0d9a6、1ab9344e、c245f9cd、15cca2f6、merge 1c2a9ab8） | `git log --oneline c782099f..HEAD` |
| START..END 代码面差异 | **空（0 字节）**：`git diff c782099f..HEAD -- src include apps tests xmake.lua xmake scripts formal` | 本轮实测 |
| 全差异（测量时点） | 恰 3 个新增 `docs/review/m-*` 文件，+5414/−0（census + edges JSON + family decomposition JSON）。本文自身不在该窗口内（见篇首发布状态） | `git diff --stat c782099f..HEAD` |
| F0 adopted SHA | `a34d96c64f5ac8647b7d0f57c5511e68301b0f0e` | #402 adoption record 评论（2026-10-07T06:19:06Z）；census §0 |
| F1 adopted SHA | `c782099f56c6afd62daad2929944673059aef573` | #402 adoption record 评论（2026-10-07T11:03:56Z） |
| M0 census PR merge SHA | `1c2a9ab8883f7afc5e131e9f7c2845e639b88c3a`（PR #457，2026-10-07T17:09:20Z） | `gh pr list --state merged` |
| 各家族 merge SHA | **无**——START 之后唯一合并 PR 为 #457（census 工件）；七个家族零执行、零合并 | `gh pr list --state merged --limit 8`；open PR 列表为空 |

数据来源：`docs/review/m-consumer-migration-census.md` + `docs/review/m-consumer-edges.json`
（已合入，PR #457）；`docs/review/m-family-decomposition.json`（R3 入库版）；控制议题 #458
（OPEN，0 评论）；#402 正文（814 行）及其 4 条评论。
`docs/review/m-consumer-migration-ledger.md` **不存在**（本轮 `ls docs/review/` 实测）——
M 消费者迁移台账未建/未合入，本文件兼任 M 侧终态记录；MIG-02 台账行仍为
`docs/roadmap/v1-conformance.md:54` NOT_ASSESSED（本轮实读）。

### 编号图例（本文使用的全部编号空间）

| 编号 | 定义处 |
|---|---|
| X-01..X-18 | F0 消费者迁移 DAG 边（`docs/review/f0-consumer-migration-dag.md`） |
| NEW-01a..e / NEW-02a..e / NEW-03 | M0 扫描新发现边（census §3） |
| B-01..B-08 | M0 阻塞/前置项（census §5）。**B-02** = `SLUICE_COPY_INTERNAL_TESTING` 构建宏未在任何 target/脚本/CI 定义 → apps/sluice-copy/safe_output.cpp 的 DirFsyncScript EINTR 注入 oracle 在全部构建配置下不可达；**"接线"= 在构建系统定义该宏**；owner=M 先接线、F4 消费证据 |
| U-01..U-08 | 安装面 unknown-external-use 项（census §6）：in-repo 无消费者证据、外部使用不可知的已安装面 |
| D-11..D-17 | F0 package-profile-policy 处置项（`docs/review/f0-package-profile-policy.md`） |
| PROD-02/PROD-03、L-01/L-05..L-12、W-01..W-04、MIG-02、HOST-01..03、ARCH-02 | root 需求 ID（`docs/explicit-io-v1-final-decision.md`，v1-r3） |
| C1-D、C2-E、B1-A..D、B2、D1、D2、E1、E2 | #402 各 slice 评审记录（ledger `docs/roadmap/v1-conformance.md` 记录段） |
| R2 / R3 | R2 = census §7.2 审查轮（2026-10-07，三视角评审后回写）；R3 = #458 尾注终审轮（2026-10-08，分解 JSON 入库） |
| c-*（如 c-x06-004） | m-consumer-edges.json 消费者记录 ID |

术语："零符号 include" = include 行存在而该 TU 未使用该头任何符号（census 对 c-x06-004..008
的判定）；"控制议题" = #458（M1 分解产物承载议题）；"三方归档闭合" = committed frozen
baseline == manifest-bound sha256 == fresh baseline（`scripts/verify_f1_package.py:336-340`
docstring 定义的 `--check-frozen` 闭合）。

## 1. 全家族终态表（M5 指令九列）

读法：Adopted? 列 = `m-family-decomposition.json` 的 `replacementAdopted` 字段——**替换形态
是否存在已采纳权威**（PASS ≠ 家族门通过；M-R 行的 PASS 另附"采纳 ≠ 整行 conformance 终审"
caveat）。Consumers migrated 列 = **M 窗口（c782099f..1c2a9ab8）内实际迁移的消费者数，
七族全部为 0**（树级证明见 §2）；M-A4 的"0 需要"指其处置已在 F1 执行（ALREADY_MIGRATED），
M 窗口内同样为 0。

| Family | Replacement | Adopted? | Consumers migrated | Isolated | Blocked | Evidence | Gate | Downstream |
|---|---|---|---|---|---|---|---|---|
| M-R（Completion caller-owned 流 → 公共 Request\<T\>/RequestId over context-owned RequestCore） | Request\<T\>+RequestId（request.hpp:18,59）+ RequestScope（root:713-715）+ PROG wait/poll 替换 busy-spin | **PASS**（采纳 ≠ 整行终审：ledger :41-42 两行仍 NOT_ASSESSED） | **0**（tier-1 可执行面未执行，见下行 Isolated/Blocked 与 §6.2 的能力/程序之分） | tier-2 X-16 policy-isolated **裁决已立**（#458 §6 四条）但隔离声明**未写档**（M-R-isolation 议题未建）；观察者 oracle 三测试豁免随 F2；COMPAT_TEST_RETAINED 4 | 家族门 BLOCKED：#459 收口口径裁决 OPEN；B-05 RequestHandle 拼写（F2-owned）；X-16 未收口；ledger 终审缺口。tier-1 在**能力上**不依赖未采纳面、在**程序上**被议题拓扑锁住（执行载体 M-R-exec 议题未建立，#459 实文把 tier-1 执行划归它） | census §3.1；#458 §3.1/§6；#459（OPEN）；M3 逐边复核 X-01/X-02/X-16 UNCHANGED（本轮抽查 2 锚为真，见 §2） | RETAINED_CONSUMERS_MIGRATED_OR_EXPLICITLY_ISOLATED = **BLOCKED** | F2 |
| M-H（ApplicationRuntime/Scheduler/Fiber/Group/Future/WaitPolicy → StackfulIoHost/IoTaskContext） | StackfulIoHost/IoTaskContext 窄 single-owner host（ADR-0003 D2 候选）；canonical Request 底座承接 result 流但不构成 host 替换 | **FAIL**（ADR-0003 `docs/adr/0003-narrow-stackful-io-host.md:3` = PROPOSED，本轮实读；ledger :53 IMPLEMENTED_UNVERIFIED；唯一 adopted ADR-0004 不裁 host 替换） | **0**（无目标拼写，#458 明令 M 内零迁移执行） | 无新隔离；census 既有 POLICY_ISOLATED 2 | 家族门 BLOCKED：#460 宿主替换裁决 OPEN（B-04）；U-01/U-08 外部使用政策未决（全族耦合安装面 ~17 头，见 §4 口径注） | census §3.2/§6 U-08；#458 §3.2；#460（OPEN）；ledger :53/:55 | REPLACEMENT_ADOPTED = **BLOCKED** | F3 |
| M-F（legacy Reader/Writer/FileReader/FileWriter/IoContext 第二权威 + 仓库唯一 vectored 实现 → canonical File/NativeFileRef/blocking::） | canonical sluice::File（root MIG-02 行 919）+ NativeFileRef（:920）+ blocking::*（:921）；IoContext 工厂→File::open+blocking:: 自由函数 | **PARTIAL**（canonical 半已采纳：root 行 919-921+ledger :37-40+F1 W-01 clean-room PASS；vectored 半未回答：B-07 五问、PROD-03 root:127、ledger :56 DEFERRED_OUT_OF_SCOPE） | **0**（裁决先行：vectored+第二权威两问未答前不动任何 FileReader/FileWriter 消费者） | census 既有 POLICY_ISOLATED 1 | 家族门 BLOCKED：#461 裁决 OPEN（B-07 vectored 五问 + D-09(1) 第二权威问 + U-03/U-04 外部使用政策） | census §3.3/§5 B-07；#458 §3.3；#461（OPEN）；NEW-01b 分发不对称（reader.cpp:81-96 vs writer.cpp:48-56） | VECTORED+SECOND-AUTHORITY_DECIDED = **BLOCKED** | F4 |
| M-A1（BlockingIoPool 零消费者安装面） | 无既定 replacement——候选 ThreadPoolBackend（F0 D-11 CANDIDATE UNDECIDED LOW）；准入前不得假定 | **FAIL**（PROD-03 未准入；F0 D-11 = docs/review/f0-package-profile-policy.md:149） | **0**（in-repo 消费者本身 =0；零 grep≠无用，处置依据是 D-11 裁决不是计数） | 无 | 家族门 BLOCKED：#462 准入裁决 OPEN（PROD-03 工作量/能力准入独立裁决） | census §3.4；#458 §3.4；#462（OPEN）；F0 D-11 原文 | PROD-03_ADMITTANCE_DECIDED = **BLOCKED** | F4 |
| M-A2（wal/copy/buffer/observed 高层 helper 族） | 无 adopted replacement——候选 canonical File ops（F0 D-12 CANDIDATE UNDECIDED） | **FAIL**（PROD-03 未准入；f0-package-profile-policy.md:150） | **0** | 无 | **上游受阻**：绑 M-F 裁决输出（wal::write_record_vec→Writer::write_all_vec 是仓库唯一原生 vectored 写路径，F0 D-12 明文绑定）+ 自身 PROD-03 准入；M-A2-ruling 议题尚未建立（#458 §7.6 计划项） | census §3.5/§7.2（R2：copy.hpp 有生产消费者 reader.cpp:3,:48-56=Reader::stream_to）；#458 §3.5 | PROD-03_ADMITTANCE_DECIDED（绑定 M-F 输出）= **上游受阻** | F4 |
| M-A3（测试替身面：fault.hpp Memory 双亲 + memory_io_context.hpp） | 无产品 replacement（F0 D-13 KEEP：relocate or INTERNALIZE candidate，TEST_ONLY，HIGH confidence） | **PARTIAL**（处置方向已记录；relocate-vs-internalize 落点未决） | **0** | 无（POLICY_ISOLATION_CANDIDATE——候选非已隔离） | **上游受阻**：与 M-F/L-01 联动（MemoryReader/MemoryWriter 只在 Reader/Writer 抽象存在时有意义）；M-A3-record 议题尚未建立（#458 §7.7 计划项）；L-11 缺陷记录在案随批处置 | census §3.6；#458 §3.6；memory_io_context 零 in-repo 引用（本轮抽查 grep=0，与 census U-02/M3 扫描一致） | PACKAGE_DECISION_RECORDED = **上游受阻** | F4 |
| M-A4（experimental uring 写面） | canonical uring 后端（uring_transport/uring_completion/uring_backend，F1 两 profile 归档在案） | **PASS** | **0 需要**（处置已在 F1 执行：ALREADY_MIGRATED；M 窗口内亦 0） | **已隔离且冻结**：libraries.lua:14-15 明文 experimental 刻意缺席；两份 manifest HEADERS 不含 experimental；负探针 NEGATIVE_EXPERIMENTAL_HEADERS_NOT_INSTALLED PASS（本轮 noliburing --check-frozen 复现中实测 PASS） | **无迁移门被阻**；阻在记录收口级：记录票 #463 OPEN（固化冻结证据）+ F5 残余"整树删除 vs 保留隔离"菜单未落 | census §3.7；#458 §3.7；#463（OPEN，自认 record-only 收口票）；本轮 --check-frozen 负探针输出 | —（已隔离，无 M gate；B-01 复核条款）= **BLOCKED（记录收口级，非迁移门级）** | none（F5 残余经 B-01 条款） |

## 2. M0 边数 → 最终边数（零迁移执行的三层证明）

**M0 计数**（census §4，本轮从 m-consumer-edges.json 重算复核，重算命令见 §7）：

- 边键 **29** = X-01..X-18（18）+ NEW-01a..e/NEW-02a..e/NEW-03（11）。
- EDGES 消费者记录 **216**（live-rescan 198 + f0-hunter 17 + review-r2 1）：M-R 75 / M-H 66 /
  M-F 35 / aux 38 / none 2（本轮 JSON 重算 = census §4 表逐格吻合）。
- APPS 深审记录 **47**：M-H 16 / M-R 9 / M-F 6 / none 16（同上复核吻合）。
- migration_state：EDGES 侧 UNMIGRATED 161 / COMPAT_TEST_RETAINED 37 / NOT_A_CONSUMER 9 /
  POLICY_ISOLATED 9；**APPS 侧 UNMIGRATED 30 / NOT_A_CONSUMER 17**（本轮 JSON 重算；
  COMPAT_TEST_RETAINED 与 POLICY_ISOLATED 在 APPS 侧均为 0）。合计 263。

**最终计数（M5）**：与 M0 **逐一相同，计数零漂移**。三层证据的承重归属如下（第 1、3 层与
全部计数为本轮全量实测；第 2 层为 M3 收敛轮存量工作、本轮仅抽查 2 锚）：

1. **树级（本轮全量实测）**：`git diff c782099f..HEAD -- src include apps tests xmake.lua xmake
   scripts formal` = 0 字节；全差异仅 3 个 docs/review/m-* 新文件。BASE 后代码面零变更 ⇒
   任何边的 live 状态不可能因迁移而变。
2. **逐边语义级（M3 收敛轮存量 + 本轮抽查）**：M3 轮 29/29 逐边复核，无一条消失边、无一条
   无解释——X-01..X-18 与 11 条 NEW 边全部 UNCHANGED（逐锚点实读：X-01 六锚、X-02 休眠
   fallback、X-03/X-04/X-05 布局链、X-06 负判定（行号 2-3 行漂移、语义事实真）、X-07 零外部
   消费者、X-08/X-09 符号锚、X-10/X-18 DOC_CLAIM 原样（义务未履行=与迁移 BLOCKED 一致）、
   X-11 formal 对应、X-12 ODR、X-13 posix_retry 安装边、X-14 seam 拓扑、X-15 恰 2 实现、
   X-16 await 机制全锚、X-17 make_unique\<Fiber\>+零 includer、X-18 apps.lua 源码树 include；
   NEW 边含 evented_wait_policy 暴露、read_vec_all 零调用者、B-02 未接线、NEW-03 冻结门等）。
   **本轮独立抽查其中 2 锚为真**：5 个零符号 sluice/file.hpp include 在位（行号见 §7 完整
   8 处枚举）、SLUICE_COPY_INTERNAL_TESTING 全仓仅 safe_output.cpp:14,:52。
3. **记录级（M3 轮扫描 + 本轮计数重算）**：M3 轮对 263 条记录逐一存在性扫描 = 261 直存 +
   2 复合路径解析后全在（c-x11-006 目录式路径 8 个点名 .tla 模型全在；c-x17-009 加号复合
   路径两文件全在）；M3 轮全局符号扫描（8 族 41 符号）：**37 符号全树非零命中、4 符号零值**
   （condition.hpp/semaphore.hpp/async_queue/memory_io_context），四者与 census 既有零面
   （c-x17-007、U-01、U-02）精确一致，非新消失。**本轮独立抽查 4 零值中的 memory_io_context
   与 condition/semaphore/async_queue 的 in-repo 引用 = 0**（命令见 §7），与 M3 扫描一致。

**新边**：11 条（NEW-01a..e、NEW-02a..e、NEW-03），M0 扫描新发现，全部 UNCHANGED，无新增。

**FALSE F0 假设**（census §7，4 条，均已回写册载；前 3 条为 M0/R2 轮判定，第 4 条计数本轮
重跑复核）：

1. X-11 "none maps to Completion" 不成立——RequestCore.tla:114,:432 ReleaseBind =
   compatibility Completion release flavor（formal map:268 明文；B-06 重映射义务）。
2. X-11 "active models map to core machinery only" 半翻案——ObserverCore.tla:28,:62,:364
   host waiter actor ↔ RuntimeTaskContext waiter path（C1-D）；"none maps to Scheduler" 半句仍成立。
3. F0 "无任何 install/export/package 规则、INSTALLED=NO" 在 HEAD 已被 F1 推翻
   （libraries.lua:18-21,:36-38 + verify_f1_package.py 79 头门 + 冻结 manifest）——F0 之后被
   新代码推翻，非 F0 取证错误。
4. X-01 "25 test files" 过时——`grep -rlw Completion tests/` = 27（"15 includers"与"4 apps"
   不变）；本轮在 HEAD=1c2a9ab8 重跑仍 = 27，且 `grep -rl 'Completion<' tests/` = 26
   （两口径与 census §8 记录一致，命令见 §7）。

**Consumer deletions = 0**：`git diff --diff-filter=D --name-only c782099f..HEAD` 输出为空
（本轮实测）。M 窗口内没有任何消费者被删除。census §7.1 在案的前 M 删除（9 个 app 测试
目标 + hardening.py，随 5f62b55b，早于 BASE）按 M 铁律**不计迁移证据**，本轮亦未计入。

**Package contractions = 0**：

- 六个 F1 冻结文件自 BASE 零改动：`git diff c782099f..HEAD` 对
  f1-package-manifest-{noliburing,liburing}.json、f1-archive-baseline-{noliburing,liburing}.json、
  f1-clean-room-package-baseline.md、f1-requirement-evidence-matrix.md 分别为空（exit 0，本轮实测）。
- 新鲜安装面 = **79 头 + 2 库**不变（F1_B_PACKAGE_PREFIX_READY PASS，本轮 noliburing
  --check-frozen 实测输出 "79 headers, 2 libs; census 81 minus 2 experimental"）。
- 生产 src/include 内容 vs BASE = 0 字节（本轮实测）。

特殊态：M-A4 的 2 个 experimental 头 = **POLICY_ISOLATED 维持**（M0 前已冻结的隔离态，
非本轮新处置）。零条边判 REMOVED_BY_MIGRATION / REPLACED_BY_CANONICAL_EDGE；无新增
INTENTIONALLY_RETAINED_COMPAT（census 既有 COMPAT_TEST_RETAINED 37 条为册载状态）。
消费者级 tier-2（X-16 runtime 面、M-F/M-A 族裁决前消费者）迁移处置 = BLOCKED，但其
live 边态 = UNCHANGED——"未迁移"与"边消失"是两个不同判定，本轮只有前者。

**include \<sluice/file.hpp\> 全部 8 处枚举**（本轮 grep，完整输出见 §7；5+1+2=8，无一遗漏）：

- 5 处零符号（tier-1 待清理，c-x06-004..008，owner=M）：tests/public_request_test.cpp:6、
  tests/public_request_release_violation_test.cpp:6、tests/request_core_ownership_test.cpp:7、
  tests/request_scope_test.cpp:6、tests/shutdown_lifecycle_test.cpp:6；
- 1 处真消费者：tests/semantic_range_test.cpp:2（c-x06-001，M-F TEST_ORACLE，随 F4）；
- 2 处自身 TU：src/file.cpp:1、src/io_context.cpp:2（legacy 面实现文件自身，非待清理对象）。

## 3. F1 基线完好性与当前包校验状态

**Before-baseline 完好性**：六个 F1 冻结工件（两 manifest + 两 archive baseline +
baseline MD + evidence matrix）自 F1 adopted SHA `c782099f` 起**字节零改动**（本轮
`git diff c782099f..HEAD` 对六文件分别实测为空）。未发生任何 B-01 触发事件（无公共头
增/删/改名、无已安装头内容编辑、无生产 src 变更）。

**当前包校验状态**（本轮在 HEAD=1c2a9ab8 实跑，输出 `/tmp/m-final/`）：

```
python3 scripts/verify_f1_package.py --check-frozen docs/review/f1-package-manifest-noliburing.json \
  --manifest-out /tmp/m-final/fresh-manifest-noliburing.json \
  --archive-baseline-out /tmp/m-final/fresh-baseline-noliburing.json     → exit=1
python3 scripts/verify_f1_package.py --liburing --check-frozen docs/review/f1-package-manifest-liburing.json \
  --manifest-out /tmp/m-final/fresh-manifest-liburing.json \
  --archive-baseline-out /tmp/m-final/fresh-baseline-liburing.json       → exit=1
```

功能门：noliburing **23 门 PASS / 0 FAIL**；liburing **25 门 PASS / 0 FAIL**。两 profile 门集
差异（本轮 `comm` 实测）：共享 21 门；noliburing 独有 2 门 =
P1_W03_URING_EXPLICIT_UNAVAILABLE、P1_W04_STRUCTURAL_ARMS_UNAVAILABLE（uring 缺席/
结构性不可用臂的负门）；liburing 独有 4 门 = NEGATIVE_LINK_WITHOUT_URING_FAILS、
NEGATIVE_ODR_DIVERGENT_VIEWS_DETECTED、P2_W03_URING_RUNTIME、P2_W04_STRUCTURAL_STOP
（uring 在场臂 + ODR/链接负探针）。数量差 23 vs 25 = 2 与 4 之差，即 profile 臂集不同，
非任何门失败。

两 profile 的**三方归档闭合双向 match=True**（两份日志尾行逐字内联，/tmp 路径易失、
以此为准）：

```text
--check-frozen FAILED: manifest key diffs=['PRODUCTION_BASELINE_SHA'], fresh archive baseline match=True, committed frozen baseline docs/review/f1-archive-baseline-noliburing.json match=True
--check-frozen FAILED: manifest key diffs=['PRODUCTION_BASELINE_SHA'], fresh archive baseline match=True, committed frozen baseline docs/review/f1-archive-baseline-liburing.json match=True
```

**键级 diff（本轮独立复算，脚本与输出见 §7；两 profile 结果相同）**：28 个共享非易变键
中，差异恰 2 键——

1. `PRODUCTION_BASELINE_SHA`：冻结值 `a34d96c6…`（F0 adopted，冻结时点的 merge-base）vs
   新鲜值 `1c2a9ab8…`（当前 HEAD==origin/master 时的 merge-base）——唯一**内容**差异；
2. `ARCHIVE_BASELINE.file`：仅记录的新鲜基线**输出路径**不同（docs/review/… vs
   /tmp/m-final/…），其 `sha256` 双方完全相同（noliburing `b32e1dcb…`、liburing
   `718d813d…`）——非内容差异；校验器自身的对比逻辑因此只报告
   `['PRODUCTION_BASELINE_SHA']`（见上内联日志尾行）。

`PRODUCTION_BASELINE_SHA` 由 `git merge-base HEAD origin/master` 派生
（scripts/verify_f1_package.py:382）且不在 VOLATILE_MANIFEST_KEYS 白名单（:42-48，仅
GENERATED_UTC/VERIFICATION_HEAD/VERIFICATION_HEAD_DIRTY/ORIGIN_MASTER_TIP）——freeze 后
master 上的任何新提交（本轮窗口内为 4 个 docs-only 提交）都必然使该键漂移，`--check-frozen`
总判定随之 exit=1。**如实结论**：包内容不变量（三方归档闭合 + 79 头安装集 + 消费者/ODR/
负探针全部功能门）在 HEAD 完整复现；字节级 manifest 复现只在该预期键上失败，属校验器
设计行为，不构成包面收缩或基线损坏证据。附注（如实记录）：本轮 HEAD==origin/master 使
F1_B_PROVENANCE_VERIFIED 的 merge-base..HEAD diff 区间坍缩为自比，故"生产内容不变"的
承重证据是本轮独立实测的 `git diff c782099f..HEAD -- src include` = 0 字节，而非该门输出
本身。未修改校验器、未改写任何 f1-*.json（M 禁令）。

**后续待办（记录，不在本轮执行）**：该预期失败使 `--check-frozen` 在任何 master 前进后
不再能直接作为字节级门使用。修复归 harness owner（census §5 B-01 行"各收缩 slice
（F2/F3/F4）；harness 归 M"口径，即 M）：候选方案 = 把 `PRODUCTION_BASELINE_SHA` 纳入
VOLATILE_MANIFEST_KEYS，或把该键的派生改为记录固定 adopted SHA 而非活动 merge-base。
该修复是 `scripts/verify_f1_package.py` 的 harness 变更（**不是** f1-*.json 改写），须独立
评审后落地；落地前包内容门以三方归档闭合 + 功能门为准，字节门差异按本文 §3 记录解释。

## 4. 未决权威裁决与 unknown external-use 决策

截至 M5 测量时点（END_SHA=1c2a9ab8，2026-10-08）**全部未决**，无任何一项被 M 采纳或裁断：

- **ADR-0003（宿主替换）**：`docs/adr/0003-narrow-stackful-io-host.md:3` = `Status: PROPOSED
  (Issue #399, D2 slice)`（本轮实读）；ledger :53 D2 = IMPLEMENTED_UNVERIFIED（human review
  pending）。M 无权采纳；唯一 ADOPTED 的 ADR-0004（:3，本轮实读）只冻结 shutdown 拼写/
  五态生命周期，不裁 host 替换。→ #460 M-H-ruling OPEN。
- **vectored 能力裁决（B-07）**：readv/writev 能力五问（root 是否要求/是否入 v1/现有能力
  是否有意退役/canonical File 是否该获得/是否 compat-only 可隔离）均无现行权威答案；
  PROD-03（root:127）不自动准入；ledger :56 DEFERRED_OUT_OF_SCOPE（"Workload and root
  amendment"）；唯一描述现有 vectored 能力的文本在已废止 ADR-0002。NEW-01b 分发不对称
  必须计入该裁决。→ #461 M-F-ruling OPEN。
- **外部使用政策（U-01..U-08）**：8 项安装面外部使用全部未知且未决——U-01 dormant 原语
  （condition/semaphore/async_queue）、U-02 memory_io_context、U-03 io_context、U-04
  sluice/file.hpp、U-05 wal/copy/observed（R2 修订：copy.hpp 有生产消费者 reader.cpp，
  不得以零下游为前提）、U-06 detail/posix_retry（已安装头）、U-07 旧家族头全集（79 头
  冻结集内）、U-08 Scheduler 耦合安装面。**头数口径注**：U-08 自身 = 12 头（9 公共头
  sluice/async/{select,event,future,wait_policy,wait_queue,timer_registration,select_fwd,
  lock_guard,mutex}.hpp + 3 已安装 detail 头 detail/{queue_item,queue_port,
  select_registration}.hpp）；M-H 全族耦合安装面 ~17 头 = 原记 5 头（application_runtime/
  scheduler/fiber/fiber_ctx/evented_wait_policy）+ U-08 的 12 头——12 是 U-08 项自身、
  17 是 M-H 族裁决输入全集，本文两处引用分别使用对应口径。处置一律走 F5/包面政策，
  不得以 in-repo 计数推定安全。
- **unknown external-use 决策：未做出**。本轮无任何新的外部使用证据；M3 全局符号扫描
  的 4 个零值符号与 census 既有零面精确一致（无新消失面），且本轮独立抽查 memory_io_context
  与 condition/semaphore/async_queue 零 in-repo 引用为真——这些**不构成、也不得被读作**
  "外部不存在消费者"的证明（M 铁律：零 grep≠无用）。
- **RequestHandle 最终拼写（B-05/F0 D-17）**：UNDECIDED，F2-owned，挡 identity_of arena
  fallback 移除（fallback 今日休眠——install_binding 零调用者——但仍是指定 release owner）。
- **X-16 收口 arm 选择（B-08）**：await 机制 in-place 重定型 vs runtime 先退役——F3 裁决，
  走 root/ADR 流程；M 已按 #458 §6 将 X-16 policy-isolated pending F3。

## 5. F2/F3/F4 授权状态（逐门精确，不整体翻绿/翻红）

#402 §6 "Current execution authorization"（本轮 gh 实取）现行文本：

- Current active work = **仅 M**（"dependency census and per-family authorization; retirement
  itself stays gated"）。
- Current forbidden production work = `F2_PRODUCTION_RETIREMENT` / `F3_PRODUCTION_RETIREMENT`
  / `F4_PRODUCTION_RETIREMENT`（各注明 "blocked on the family M gate"）+ `F5_FINAL_CONTRACTION`
  + `F6_FINAL_ACCEPTANCE`。

逐族映射（精确到家族门，截至 M5 测量时点零门通过；M-A4 无迁移门、其记录收口不阻塞
F2/F3/F4 中任何一方）：

| 收缩相位 | 被哪些家族门阻塞（全部未过） | 附加专属前置 |
|---|---|---|
| F2（Request/Result/Identity contraction） | M-R RETAINED_CONSUMERS_MIGRATED_OR_EXPLICITLY_ISOLATED = **BLOCKED**（#459 OPEN；tier-1 零执行、tier-2 隔离未写档） | B-05 RequestHandle 拼写（F2-owned UNDECIDED）；X-16 收口为 F2 drop-Completion& 的前置（F0 DAG F2_PREREQUISITES）；ledger :41-42 整行终审缺席（不挡开工，挡"已验证"声明） |
| F3（host/runtime contraction） | M-H REPLACEMENT_ADOPTED = **BLOCKED**（#460 OPEN；ADR-0003 PROPOSED） | B-08 helper-before-runtime 编译序未证明；U-01+U-08 外部使用政策（与 host 裁决解耦单列） |
| F4（native resource retirement） | M-F VECTORED+SECOND-AUTHORITY_DECIDED = **BLOCKED**（#461 OPEN）；M-A1 PROD-03_ADMITTANCE_DECIDED = **BLOCKED**（#462 OPEN）；M-A2 同名门 = **上游受阻**（议题未建）；M-A3 PACKAGE_DECISION_RECORDED = **上游受阻**（议题未建） | A1 共享 oracle 行 ledger :36 IMPLEMENTED_UNVERIFIED + legacy permission_denied GAP（ledger :149）；B-02 接线（M-owned，未做） |

结论：**F2/F3/F4 生产收缩全部未授权且被明令禁止**（#402 §6 forbidden 清单），逐门阻塞
状态如上——不存在任何"部分授权"的家族。#459..#463 五个裁决议题全 OPEN 是当前唯一的
合法推进载体；M-R-exec / M-R-isolation / M-A2-ruling / M-A3-record 四个 #458 §7 计划议题
未建立（与 #458 §7 计划拓扑的真实偏差，如实记录；实际建立的议题集 = #459/#460/#461/
#462/#463，其中 #463 为计划外 record 收口票）。

## 6. M_EXECUTION_COMPLETE_FOR_CURRENTLY_AUTHORIZED_WORK 判定

**判定：TRUE（就"当前被授权给 M 的工作"而言执行完毕）；同时如实记录：这不等于 M 家族门
任何一项通过，更不等于 M 阶段出口达成。**

理由（逐条可溯源）：

1. **授权面**：#402 §6 现行文本把当前 active work 界定为 "M — Consumer migration
   prerequisites by family (dependency census and per-family authorization; retirement itself
   stays gated)"——即 M 的被授权工作是**前提工件与逐族授权准备**，不是收缩执行。该授权面
   的全部交付物已完成并有合并证据：M0 census + edges JSON（PR #457 merged @1c2a9ab8）、
   M1 家族分解（#458 + R3 入库 JSON）、R2/R3 评审轮回写、M2/M3/M4 收敛核验（逐边 29/29
   UNCHANGED、263 记录全在、全局符号扫描一致）、本 M5 终态记录（本文件）。
2. **零迁移执行与授权一致，不是未完成**：START..END 代码面 diff=0 字节（本轮实测）⇒
   M0 以来零迁移执行。这与六族迁移门 BLOCKED/上游受阻的状态**一致**。唯一的表面张力在
   M-R tier-1，须把两个不同谓词分开：**能力上**，tier-1 不依赖任何未采纳面
   （readiness=PARTIALLY_READY 的本义）；**程序上**，其执行载体（#458 §7.1 计划的
   M-R-exec 议题）从未建立，实际建立的是收口口径裁决 #459（OPEN），其实文明确把 tier-1
   迁移执行划归未建的 M-R-exec——因此在当前议题拓扑下**不存在"已解锁且被指派给 M 但
   未做"的工作项**。这一程序性事实不改变 tier-1 未执行的事实本身（真实未达成项，见 §8
   遗留义务，随 #459 收口处置）。
3. **M 禁令零违反**：consumer deletions=0、package contractions=0、无 ADR 秘密采纳
   （ADR-0003 仍 PROPOSED）、X-16 policy-isolated 裁决被树态忠实执行（await 机制全锚原样）、
   无 multi-worker→single-owner 静默转换（X-16/X-17 边 UNCHANGED）、F1 冻结物未改写。
4. **剩余 OPEN 项全部不属于 M 可执行面**：#459..#463 是裁决/记录议题，其输入（采纳
   ADR-0003 与否、vectored 五问、PROD-03 准入、包面落点）的裁决权在 F3/F4/root-ADR/F5
   流程，M 无权代裁；F2/F3/F4/F5/F6 被 #402 §6 明令禁止。M 没有剩余的"当前已授权且
   可执行"的动作。
5. **判定边界（防误读）**：本判定语义 = "M 就其当前被授权的工作执行完毕"，**不**蕴含
   (a) 任何家族门 PASS（六族迁移门 BLOCKED/上游受阻，M-A4 为记录收口级 BLOCKED）；
   (b) M-R tier-1 已迁移（实际零执行）；(c) #402 M 终局门
   （RETAINED_CONSUMERS_MIGRATED_OR_EXPLICITLY_ISOLATED 与
   NO_UNQUALIFIED_CANONICAL_DOC_CLAIMS_FOR_RETIRED_AUTHORITY，#402 Final hard gates
   两项）达成——两项在逐族层面全部未过；(d) ledger MIG-02 行（:54）状态变化（仍
   NOT_ASSESSED）。

## 7. 复现

本轮实测命令（在 HEAD=1c2a9ab8 工作树 clean 状态；/tmp/m-final/ 工件易失，承重输出已
内联 §3；本文自身合入后 master 前进，重放树级窗口时以测量时点 1c2a9ab8 为准，或对
新窗口重跑并预期同样为空/一致）：

```bash
git rev-parse HEAD                                  # 1c2a9ab8883f7afc5e131e9f7c2845e639b88c3a（END_SHA）
git diff --stat c782099f..HEAD                      # 3 docs/review/m-* 文件，+5414/-0（测量时点）
git diff c782099f..HEAD -- src include apps tests xmake.lua xmake scripts formal | wc -c   # 0
git diff --diff-filter=D --name-only c782099f..HEAD # 空（consumer deletions=0）
git diff c782099f..HEAD -- docs/review/f1-package-manifest-noliburing.json \
  docs/review/f1-package-manifest-liburing.json docs/review/f1-archive-baseline-noliburing.json \
  docs/review/f1-archive-baseline-liburing.json docs/review/f1-clean-room-package-baseline.md \
  docs/review/f1-requirement-evidence-matrix.md     # 空（F1 冻结物零改动）

python3 scripts/verify_f1_package.py --check-frozen docs/review/f1-package-manifest-noliburing.json \
  --manifest-out /tmp/m-final/fresh-manifest-noliburing.json \
  --archive-baseline-out /tmp/m-final/fresh-baseline-noliburing.json   # 23 门 PASS/0 FAIL；exit=1 仅 PRODUCTION_BASELINE_SHA
python3 scripts/verify_f1_package.py --liburing --check-frozen docs/review/f1-package-manifest-liburing.json \
  --manifest-out /tmp/m-final/fresh-manifest-liburing.json \
  --archive-baseline-out /tmp/m-final/fresh-baseline-liburing.json     # 25 门 PASS/0 FAIL；同上

# 键级 diff（§3 第 2 点的来源；两 profile 同法）
python3 - <<'EOF'
import json
VOLATILE={"GENERATED_UTC","VERIFICATION_HEAD","VERIFICATION_HEAD_DIRTY","ORIGIN_MASTER_TIP"}
frozen=json.load(open('docs/review/f1-package-manifest-noliburing.json'))
fresh=json.load(open('/tmp/m-final/fresh-manifest-noliburing.json'))
keys=(set(frozen)|set(fresh))-VOLATILE
for k in sorted(keys):
    if frozen.get(k)!=fresh.get(k):
        print(k, 'frozen=', frozen.get(k), 'fresh=', fresh.get(k))
EOF
# 输出（两 profile 相同模式）：ARCHIVE_BASELINE（仅 file 路径不同、sha256 相同）+ PRODUCTION_BASELINE_SHA（值不同）

grep -rn "include <sluice/file.hpp>" tests/ apps/ src/   # 恰 8 处：5 零符号（public_request_test.cpp:6、
    # public_request_release_violation_test.cpp:6、request_core_ownership_test.cpp:7、request_scope_test.cpp:6、
    # shutdown_lifecycle_test.cpp:6）+ 1 真消费者（semantic_range_test.cpp:2）+ 2 自身 TU（src/file.cpp:1、io_context.cpp:2）
grep -rn "SLUICE_COPY_INTERNAL_TESTING" src include tests apps xmake.lua xmake scripts  # 仅 safe_output.cpp:14,:52
grep -rlw Completion tests/ | wc -l                       # 27（本轮重跑=census §8 值）
grep -rl 'Completion<' tests/ | wc -l                     # 26（同上）
grep -rln "memory_io_context" src include apps tests xmake.lua xmake scripts  # 0 命中（U-02 零面本轮复核）
grep -rn "sluice/async/condition.hpp\|sluice/async/semaphore.hpp\|sluice/async/async_queue.hpp" \
  --include="*.cpp" --include="*.hpp" src apps tests      # 0 命中（U-01 零面本轮复核）

# M0 计数重算（216/47/263 与家族、状态分布；本轮实测输出=census §4 逐格一致）
python3 - <<'EOF'
import json
d=json.load(open('docs/review/m-consumer-edges.json'))
fam={}; st={}
for e in d['EDGES']:
    for c in e.get('consumers',[]):
        fam[c.get('family','?')]=fam.get(c.get('family','?'),0)+1
        s=c.get('migration_state','?'); st[s]=st.get(s,0)+1
print('edge keys:', len(d['EDGES']), '| EDGES:', sum(fam.values()), fam, '| EDGES states:', st)
afam={}; ast={}
for a in d['APPS']:
    for c in a.get('consumers',[]):
        afam[c.get('family','?')]=afam.get(c.get('family','?'),0)+1
        s=c.get('migration_state','?'); ast[s]=ast.get(s,0)+1
print('APPS:', sum(afam.values()), afam, '| APPS states:', ast)
EOF

gh issue view 458 --json state,comments               # OPEN，0 评论
gh issue list --state all --search "in:title M-"      # #459..#463 全 OPEN
gh pr list --state merged --limit 8                   # BASE 后仅 #457 @1c2a9ab8
```

M3 轮存量工件（逐边 29/29 复核细节、全局符号扫描全量输出）产生于 M3 收敛轮，
快照在 `/tmp/m-convergence/`（edge-anchors-1.txt/-2.txt、global-symbol-scan.txt；路径易失），
其结论已由本文 §2 第 1/3 层与本轮 2+4 锚抽查独立覆盖。

## 8. 遗留义务（本记录未履行、随对应议题推进）

- **M-R tier-1 迁移**（5 零符号 include 清理、context 面可表达 submit 重定型、B-02 接线）：
  零执行；载体 M-R-exec 议题未建立，程序上待 #459 收口口径裁决先行。
- **M-R tier-2 X-16 隔离声明写档**：M-R-isolation 议题未建立。
- **四族裁决**：#459（M-R 收口）、#460（M-H 宿主替换+U-01/U-08 政策）、#461（M-F
  vectored+第二权威+U-03/U-04 政策）、#462（M-A1 准入）全 OPEN；M-A2-ruling/M-A3-record
  议题未建立（#458 §7.6/7.7 计划项）。
- **M-A4 记录收口**：#463 OPEN（固化 F1 ISOLATE 冻结证据；F5 残余菜单不在本票裁决）。
- **ledger MIG-02 行（:54）登记**：仍 NOT_ASSESSED；M 消费者迁移台账
  （m-consumer-migration-ledger.md）未建，迁移证据登记义务随迁移执行履行。
- **harness 修复**：`--check-frozen` 的 PRODUCTION_BASELINE_SHA 预期键失败处理（方案见
  §3 末段），owner=M，独立评审后落地。
- **DOC_CLAIM 义务**（X-10/X-18，含 copy README docs/history 死引用）：随各家族迁移批次
  履行（M gate：NO_UNQUALIFIED_CANONICAL_DOC_CLAIMS_FOR_RETIRED_AUTHORITY）。
