# Sluice #621 Round 2 R1 最终报告：ProgressSource drain 单次 read 的机制、正确性、性能与产品裁决

> **裁决先行**
>
> | 裁决项 | 值 | 适用范围 |
> |---|---|---|
> | `MECHANISM_VERDICT` | **INCONCLUSIVE** | 删除"成功 drain read 后必然 EAGAIN 的第二次 read"这一动作的语义与计数证据未被推翻（§C、§D.6），但唯一一次同 session 受控测量 `confirmed=false`（A/A CI 宽度 3.8789 > 3.0，§D.5.5），且拆分后的归档回执仍缺（§C.6、§C.7），故不升为 GO；不覆盖 io_uring 内核写者窗口的确定性注入 |
> | `END_TO_END_VERDICT` | **NOT_ESTABLISHED** | 受控 session（session-r2r1）四组均值与 95% CI 上界为负（instructions −5.5528% `[-6.5241,-4.5699]`、cycles −4.7712% `[-5.8198,-3.7236]`、task-clock −5.3220% `[-6.6076,-4.0105]`、rusage CPU −5.3359% `[-6.6299,-4.0128]`），但**逐对并非全负**（正号：instructions 1/36、cycles 3/36、task-clock 4/36、rusage 4/36），且该 session 未通过预注册 A/A 安静度门（+0.6512% `[-1.2814,+2.5974]`，宽度 3.8789），`confirmed=false`。四组最小绝对值为 cycles 4.7712%，任何保守量级只能取 4.77%。旧 session 跨度 −1.4%~−12% 未解释，旧 36 对的 −11.968% 是大而未被复现的点估计，不是上界。端到端收益未被建立；不得引用任何单一百分比作为产品收益 |
> | `PRODUCT_READINESS` | **NOT_READY** | 阻断：受控 session 的 A/A 安静度门未过（同 session 零效应带不成立）；跨 session spread 未分解；10 对交叉检查无 protocol 记录且为 A→B 顺序（非 AB/BA 交错）；预注册门槛无独立存档（仅分析器内嵌，§E.3）；拆分后四配置矩阵/套件/sanitizer/mutant 回执均缺失（16:05 旧目标）；B.2 台账不可复算；p99 判据无定义；无 product PR/ledger。可进入 PR 讨论，**不具备合并条件** |
>
> **版本边界**：`BASE_SHA = HEAD_SHA = 3f22d2d08a53077b6377934b1895af78126de0a1`。BASE 即 PR #634 的 merge commit。候选实现 `R1-drain-eagain-read` 位于分支 `research/621-r2-R1-drain-eagain-read`、worktree `/home/jnhu/sluice-621-r2/wt-B`，最终提交 `a3a14a31`（父 `839a0164`，基线 `3f22d2d0`）；**未 push、无 product PR、未 merge**；主工作区 `/home/jnhu/Source/Sluice` 在本报告写入前位于 BASE 且工作树干净，写入后仅新增未跟踪的 `docs/evidence/621-round2-R1/REPORT.md`。同目录早期草稿的 `NO_GO / NOT_ESTABLISHED / NOT_READY` 结论来自"语义与机制混在同一 BASE-GREEN 门禁且尚无 A/B"的中间状态；尝试 3 已把测试拆分并补齐 A/B、负对照与 B profile，以本报告为准。
>
> **修订记录（r2，2026-11-12）**：r1（17:58）在 perf stat 交叉检查与 rusage 的冲突尚未解释时给出 `INCONCLUSIVE / PENDING_AUDIT`。r2 只依据新增的 `reconciliation/` 对账证据（`README.md` 实验 1–4、`plain-reused-dst.txt`、`plain-fresh-dst.txt`、`perfstat-with-sync.txt`、`recon-provenance.txt`、结构化 `pairs=8` 结果）修订 §D.4–§D.6、§E、§F 与 Q4–Q7；r1 的历史测量、计数与收据全部保留，不追改。r1 引用的 perf stat 三行（instructions −2.42%、cycles −2.10%、task-clock −1.71%）作为历史记录保留，但已从裁决依据中移除（理由见 §D.5）。
>
> **修订记录（r3，2026-11-12，审计后）**：审计指出 r2 的三处无证据声明（交叉检查"环境假象"归因、其基线稀释机制、"1/16 请求数缩放"）与若干缺失证据（10 对交叉检查的 protocol、预注册门槛原件、实验 3 的静默范围、8 对会话的同 session A/A、拆分后回执、B.2 台账复算、实验 4 原始输出）。r3 据此：(a) 把"D.5.1 rusage vs perf stat 冲突"改写为**跨 session 量级差异**，并在 §D.5.1 补上交叉检查自身的 rusage 行（A 1.489662 s / B 1.468571 s，−1.416%）；(b) 撤回"缺少静默纪律造成 −2%"的成因断言与"约 −2% 被排除"的结论；(c) 负对照改按**每请求归一化**；(d) 撤回"≥6%"表述；(e) 把实验 3 标为 **sync-only**；(f) `END_TO_END_VERDICT` 降为 `INCONCLUSIVE`、`PRODUCT_READINESS` 降为 `NOT_READY`。r1/r2 的历史数字与收据仍不追改。
>
> **修订记录（r4，2026-11-12，受控 session 后）**：r4 只依据新增的受控 session `session-r2r1`（同一 session 内 A/A 12 对、主场景 36 对、负对照 12 对，AB/BA 交错，每次运行同时采集 `perf stat` 与 `rusage`；120 个正式运行 + 6 次丢弃 warmup）修订 §0、§A.3、§D.1、§D.4、§D.5、§D.9、§D.10、§E、§F、Q4–Q7 与附录；r1–r3 的历史测量、计数与收据保留，不追改。受控 session 的四组口径一致（§D.5.5），但未通过 A/A 安静度门（宽度 3.8789 > 3.0），`confirmed=false`；据此 `MECHANISM_VERDICT` 由 GO 降为 `INCONCLUSIVE`，`END_TO_END_VERDICT` 维持 `INCONCLUSIVE`，`PRODUCT_READINESS` 维持 `NOT_READY`。r2 的"环境假象"归因与 r3 的撤回继续有效。
>
> **修订记录（r5，2026-11-12，审计后）**：r5 只做证据口径修正，不新增测量。修正：(a) 产品保守量级由 5.34% 改为四组口径的最小绝对值 **4.7712%（cycles）**，并说明 5.34% 只是 rusage/instructions 两者取小，不是四组口径的保守量级；(b) 把"四组全部为负"统一改为"四组均值与 95% CI 上界为负"，并披露受控主场景逐对正号数（instructions 1/36、cycles 3/36、task-clock 4/36、rusage 4/36）；(c) B.6 falsifier 按受控 session 负对照与跨 session 事实重写，撤回"rusage vs perf stat 冲突"与把旧 −2.06% 当作当前值的表述；(d) B.4 撤回"确实出现成本转移"，改为未解释的符号位移；(e) 把旧 36 对 −11.968% 的"上界"改为"未被复现的大幅点估计"；(f) 机制界 4.0–4.5% 改为粗估 ≤4.53%，并注明受控 cycles −4.7712% 已超过该粗估；(g) futex 汇总补入 5+5 perf tracepoint 3.149 → 2.104（B/A 0.668），并把"三次"改为五个来源；(h) 0.989 改标为 `ksys_write` 比值，read 频率改用 EAGAIN 130,152/run ÷ 131,076 = 0.993；(i) 补全旧/受控 session 的频率披露。据此 `END_TO_END_VERDICT` 由 `INCONCLUSIVE` 降为 `NOT_ESTABLISHED`，`MECHANISM_VERDICT` 维持 `INCONCLUSIVE`，`PRODUCT_READINESS` 维持 `NOT_READY`。
>
> **"本会话"的指代**：正文中未特别说明的"本会话/本轮撰写"指 **r1 撰写会话**；r2/r3/r4 修订会话实际执行的检查单列于附录 C，均未重跑任何测量、测试或构建。**受控 session（session-r2r1）** 是 r4 之前由一次性驱动脚本执行的测量会话；r4 只读取其归档产物与原始 TSV/meta（§D.5.5、附录 C.3）。
>
> **证据根**：`/home/jnhu/sluice-621-r2/`。本报告只使用该目录下的结构化证据、原始数据与本轮实际执行的检查；所有引用路径均相对于该根，除非显式写成仓库路径。历史性能数字（#634、#618、#595、#622、#600 等）只作成本地图/去重证据，不替代本轮的 A/A、A/B 与 profile。

---

## 0. 结论摘要

1. **冗余工作**：`ProgressSource::drain_notification_nolock_()` 在成功读取 eventfd 后 `continue`，随后必然再执行一次返回 `EAGAIN` 的 `read(2)`；该探测 read 在 ThreadPool 配置下每请求约 0.99 次（EAGAIN 130,152/run ÷ 131,076 = 0.993 次/请求；0.989 是 `ksys_write` 五次均值 129,656/run 的比值，不是 read 频率），纯属 drain 终止探测。
2. **删除方式**：成功 read 后在同一 `mtx_` 临界区置 `notification_pending_ = false` 并 `return`；保留 `EINTR` 重试与 `EAGAIN` 分支。生产改动只落在 `include/sluice/async/detail/progress_source.hpp`（+47/−3）；另加两个测试文件与 `xmake/tests.lua` 注册，最终 diff 为 4 文件 +760/−3。
3. **语义证据**：语义 oracle 9/9 在**纯净 BASE** 与 B 上均通过；新 mutant `progress_source_mut_drain_success_keeps_pending` 被 3 条语义 re-arm 断言杀死（exit 1；当前构建，归档回执为拆分前旧目标，见 §C.6）；机制指标测试在 B 上 4/4、在 BASE drain 体上因 "2 reads/1 EAGAIN" 而红（按设计不参与 BASE-GREEN）。四配置矩阵的结构化记录为 `81/81`、`81/81`、`60/60`、`60/60`，但拆分后的矩阵日志未保存（见 §C.7）；TSAN/ASAN 只有 app_copy 与 threadpool race 两个目标有非空回执，语义目标的对应日志为 0 字节且目标名为旧名。
4. **机制计数（已归档证据）**：`results/mech-counters.tsv` 的 256 MiB、3 次/臂 strace 探针显示 `eventfd_read` 中位数 262,135 → 131,073（−50.0%）；`raw-r2/ab/trace/syscount-A1/B1` 的 bpftrace `@read_eagain` 为 118,115/124,547 → 12/7；5+5 次 perf tracepoint 的 `sys_enter_read` 为 1.990 → 0.987 次/请求。
5. **性能（受控 session 为主证据）**：session-r2r1 在同一 session、同一纪律、AB/BA 交错下完成 A/A 12 对、主场景 36 对、负对照 12 对；每次运行同时采集 `perf stat`（task-clock/cycles/instructions）与 `rusage`。主场景四组**均值与 95% CI 上界**为负：instructions **−5.5528%** `[-6.5241,-4.5699]`、cycles **−4.7712%** `[-5.8198,-3.7236]`、task-clock **−5.3220%** `[-6.6076,-4.0105]`、rusage CPU **−5.3359%** `[-6.6299,-4.0128]`（各 36 对，CI 上界均 <0）；**逐对并非全负**：instructions 1/36、cycles 3/36、task-clock 4/36、rusage 4/36 对为正（§D.5.5）。四组最小绝对值为 cycles 4.7712%。但 A/A 安静度未过（+0.6512% `[-1.2814,+2.5974]`，宽度 3.8789 > 3.0），`confirmed=false`。旧 session（36 对 rusage −11.968%、8 对完整纪律 −6.70%~−5.99%、4 对 sync-only −8.8%~−7.7%、10 对 perf stat −2.42%~−1.416%）只作为跨 session spread 保留，不作因果解释（§D.5）。
6. **必须同时报告的反例与边界**：受控 session 的 A/A 宽度门失败，且主场景逐对符号不一致；负对照（12 对 rusage −1.4396% `[-2.7800,-0.0811]`）**不是零效应**，按每请求归一化节省 0.5880 µs/请求（主场景 0.7040 µs/请求），既非零效应也非反例；21 次 wall 停顿全部保留；运行中 sysfs 采样频率下探，无法声称"测量期间持续 cur==max"；futex 减半不可复现，已从机制证据移除（§D.6）；10 对交叉检查无 protocol 记录、且为 A→B 顺序；预注册门槛无独立存档；拆分后归档回执与 B.2 复算仍缺。因此 **`END_TO_END_VERDICT = NOT_ESTABLISHED`**；`PRODUCT_READINESS = NOT_READY`。不得外推为 wall、能效或跨后端。

---

## A. Repository Reality

### A.1 基线与 PR #634

| 项 | 值 | 来源 |
|---|---|---|
| `BASE_SHA` / `HEAD_SHA` | `3f22d2d08a53077b6377934b1895af78126de0a1` | 结构化证据 `baseSha`、`headSha`；本会话 `git -C /home/jnhu/Source/Sluice rev-parse HEAD` 同值 |
| 主工作区 | BASE 核验时 `git status --porcelain` 为空；本报告写入后仅新增未跟踪的 `docs/evidence/621-round2-R1/REPORT.md` | 结构化 `treeClean=true`；本会话复核（写入前）无输出 |
| PR #634 | `state=MERGED`，`mergedAt=2026-11-12T04:29:25Z`，`mergeCommit.oid=3f22d2d0…`，head `product/621-threadpool-owed-count-gate` | 结构化 `pr634`、`history.pr634State` |
| 合并内提交 | `b0dc4dc0`（生产 gate，5 files +496/−12）、`bb91a493`（evidence record、ledger row、changelog）、`3d7e0685`（exact parity oracle、wording repair、final verification） | `history.pr634State` |
| #634 生产 diff 哈希 | `c2187b02fbab26ca91aae93ed57e2123d8f9fca6cc5496df98a0724dd0c888a2`，与 `docs/evidence/621-threadpool-owed-count-20261112/production.diff` 相同 | `history.eliminations` #634 行；该次历史核验执行 |
| #634 证据清单 | `sha256sum -c MANIFEST.sha256` → 15/15 OK | `history.notes`；该次历史核验执行，本次撰写未重跑 |
| #634 ledger | `docs/roadmap/v2-conformance.md:71`、`:454-526` | `history.eliminations` #634 行；本会话读取确认行存在 |

PR #634 的 `owed_count_` 门控已在 BASE 中，本轮**不重复**该机制。本轮的实现分支从 BASE 新建；`wt-B` 的最终树为 `a3a14a31`，工作树干净。

### A.2 工具链与主机

| 项 | 值 |
|---|---|
| 编译器 | `gcc (GCC) 16.2.1 20261029 (Red Hat 16.2.1-3)` |
| 构建工具 | `xmake v3.1.2+HEAD.5d21e9d` |
| liburing | `2.13` |
| 内核 | `7.2.11-200.fc44.x86_64` |
| CPU / 文件系统 | 20 CPUs / btrfs |

测量二进制为 release、`--liburing=y` 构建；A 与 B 均 110,152 B，sha256 分别为 `0eca1011b2ee047679a4f8dd1bfc5b21aba60f245f850cb2746f94260792b458`（A，BASE）与 `db972b1946fcf2b282135ebdf600263b03492215b08846e867d856c6213fa0be`（B，`a3a14a31`）。用于 perf record 归因的是另建的 `-g -fno-omit-frame-pointer` 副本（A `e5b12e9f…`，B `eae49cb0…`），**不用于计时**。

### A.3 Workload、构建、执行路径与 oracle

- **workload**：`taskset -c 2,3 <bin> --buffer-size 4096 --pipeline-depth 4 --workers 1 --sync none --no-atomic <src> <dst>`；源文件 268,435,456 B，sha256 `a1f8dd971b212ae487cad4edf8f2b742bd6b1a8feef92a4c44730bbe167547f3`。
- **请求数**：每个 256 MiB 拷贝 131,076 个请求（`read_ops=65540`、`write_ops=65536`）。
- **oracle（每次运行）**：exit 0；stdout 为 `copied 268435456 bytes (read_ops=65540 write_ops=65536 short_writes=0 sync=none pipeline_depth=4 atomic=off)`；`cmp -s src dst` 字节相同（CMP_OK）。
- **覆盖**：7 次干净计时、5 次 perf stat、5 次 syscall tracepoint、3 次 uprobe、5 次 perf record、2 次 strace、1 次归档 bpftrace `read_eagain`（另一次 bpftrace 输出未归档，本报告不采用）；结构化证据记为全部 `CMP_OK`。A/A 72/72、A/B 主场景 72/72、负对照 72/72 运行时 oracle 失败数为 0。
- **受控 session 覆盖（r4 新增）**：`session-r2r1` 的 120 个正式运行（A/A 24、主场景 72、负对照 24）全部 `rc=0`、`bytes_copied=268435456`、`short_writes=0`、`dst_matches_src=True`，且 `read_ops/write_ops` 等于冻结值（4 KiB/d4 为 65540/65536，64 KiB/d1 为 4097/4096）；另 6 次丢弃 warmup（每段每臂 1 次）也 oracle ok。该复核由 r4 会话直接读取三个 TSV 完成（附录 C.3）。
- **构建命令**：`xmake f -m release -y --liburing=y && xmake build sluice-copy`；B 侧在 `xmake clean` 后重建哈希稳定。结构化 `bProfile` 记录：release 生产对象经 `nm` 检查无 `set_drain_observation_for_test` / `inject_drain_eintr_for_test` / `prepark_counter` 测试符号（本次撰写未重跑该检查）。
- **执行路径**：sluice-copy 固定走 `make_threadpool_context`（`apps/sluice-copy/copy_task.cpp:299`）；本 workload **不执行 io_uring 路径**。`--liburing=y` 只影响构建与可运行的 uring 测试面。

### A.4 历史机制去重记录

下表来自结构化 `history.eliminations` 与 `vetoes`；只作为去重与成本地图，不替代本轮证据。

| 机制 | 状态 | 已删除/已交付的工作 | 重入条件 |
|---|---|---|---|
| PR #618 item 1 RequestScope settle loop | MERGED | 删除每次 settle 迭代的 `poll_progress` + fail-fast + 第二次全覆盖就绪扫描；settlement 由 `wait_one()` 推进（squash `98825793`） | 不重复；仅当独立反例证明 `wait_one()` 漏 progress 时重开 |
| PR #618 item 2 six submit stats guards | MERGED | 六个 submit 入口的 `tally_submit`/`update_max_outstanding` 包进 `if (stats_)` | 不重复 |
| PR #618 item 3 `Entry::physical_position` | MERGED | 删除 write-only 副本；`last_physical_position_` 仍是唯一权威 | 不重复 |
| PR #618 item 4 `Slot::accepted` | MERGED | 删除写标志；`SlotObservation::accepted` 由 `phase` 派生 | 不重复 |
| PR #618 item 5 `Slot::zero_op` | MERGED | 删除写标志；零长路径由 `descriptor.zero_op` 选择 | 不重复 |
| PR #618 item 6 `Slot::cancel_intent` | MERGED | 删除三处 dead store 与 observation 镜像；取消由 `PublicCancel::requested` 报告 | 不重复 |
| ThreadPool `owed_count_` 门控（#621 / PR #634） | MERGED | driver 每 pass 两次空 O(64) delivery-record 扫描由派生 `owed_count_` 门控；`immediate_work_remains` 改读 `owed_visible_locked()` | 不重复；仅当 owed>0 分支出现新实测暴露或 counter invariant 变更时重开 |
| io_uring shape-aware submission / `BOUNDED_PROGRESS`（#595） | PAUSED | 未交付产品工作；研究臂 B 降 enter −81.2%、CPU/op −26.46%，但 D2 p95 +5.69%、p99 +7.45%、tput −3.99% 超冻结保护界 | 按 `docs/research/uring-submit-policy/round-e5-20261104/RESUME.md:84-92` 重开；D1/D2/D3、64 KiB、W-03、cancel 家族仍是强制 falsifier |
| Ownership-aware streaming buffer handoff（#632） | RESEARCH | 无产品改动；H0 明确 sluice-copy 已复用 slot buffer，可能不存在可删 memcpy | G0 ADMIT：真实多阶段 consumer、冻结 stage DAG/oracle、等量内存预算 |
| USIO-0 kernel-boundary research（#617） | PAUSED | 无；G1 未满足（无 admitted real workload 证明 material kernel-boundary cost） | G1 触发 + G2–G5 |
| E5 native cross-layer observability cost map（#622） | RESEARCH | 无产品改动；`EXISTING_TOOLS_SUFFICIENT` / `CUSTOM_TOOL NOT_JUSTIFIED` | 仅当出现 decision-blocking observation gap |
| Workload Cost Atlas（#604） | RESEARCH | 无应用层 elimination；R0 WSL2 多为负结果 | 真实 admitted consumer + 冻结 DAG/oracle/budgets |
| K1 single-lock completion/publication drain（#594/#595） | REJECTED | 研究臂 −3.098 acquisitions/op，W-03 −2.29%，低于 4.79% band；该 band 的预注册标签已撤回 | 仅在新的 K4 product change 后 + owed-scan lock 重设计 + 新 baseline |
| β/γ submit/wait lock-merge（#621 报告） | REJECTED | 未实施；估 ~0.5% 与 <0.5% | 仅当新 profile 显示具体 lock acquisition 为 measured hotspot 且有单因素 A/B |
| `offer_terminal → release_execution` fusion（#621 P2） | REJECTED | 未实施；#622 成本地图显示 completion/reap 不是 top lever | 仅当真实 consumer profile 显示 terminal/release 为 measured hotspot |
| #579 RequestScope scan/free-list、#582 ThreadPool worker handoff | REJECTED | 无；#579 在 normal FIFO W-02 回归 15–21% | 仅在新独立真实 workload（achieved concurrency、costs、safety oracles、negative controls） |
| C1 admission-dispatch restructure（#600 / PR #602） | MERGED | 删除 Precheck materialization；success-path instructions 70→40；read_at 0 B −13.3 ns 等 | 不重复；仅当新 direct workload 证明仍有 measurable excess |

**关键 veto 摘要**（完整列表见结构化 `vetoes`）：#595 产品默认仍为 `IMMEDIATE`；#617 的 #622 证据是条件性触发而非自动触发；#634 的 `owed_count_` 是 `work_mtx_` 下的派生摘要，**不是** settlement/publication 权威，权威 `any_event_owed_locked_()` 扫描必须保留；PR #618 明确不作端到端性能声明；K1 的 `NO_CHANGE_JUSTIFIED` 依赖已撤回的 4.79% band。

### A.5 本轮范围与未覆盖

- 仅评估 ThreadPool 后端、4 KiB / pipeline depth 4 / 1 worker / `sync=none` / `no-atomic` 的 warm-cache 拷贝；受控 session 另以同一后端与 worker 配置的 64 KiB / d1 作为负对照。全部测量都在本机 btrfs、warm cache 上完成。
- io_uring 未在本 workload 执行；其"内核写者在成功 read 与清 pending 之间重新 signal"的窗口没有确定性注入 seam。
- 多 worker、zero-op 密集、cold page cache、W-03 多生产者、真实 EINTR 信号、高并发 submit 形态未测。
- 生产头文件含受 `SLUICE_ASYNC_INTERNAL_TESTING` 保护的 test-only observation/injection seam；release 生产预处理命中为 0，但按 AGENTS.md 的注释/插桩风格仍需评审。

---

## B. Remaining Cost Ledger

### B.1 全链路成本排名（profile，BASE）

profile 使用 5 次 perf record 聚合（exclusive self），并用 `-g -fno-omit-frame-pointer` 的剖析副本归因；Kernel/libc/Sluice 用户态的分区为 **77.1% / 13.5% / 6.33%**（`raw-r2/record/agg-buckets.tsv`）。`topCosts` 各项口径不同（trampoline 是 exclusive self 合计，futex/poll/pwrite/pread/read 是栈归属，glibc mutex 与 `rep_movs`/`clear_highpages` 是 self），**不能相加**。

| 排名 | 成本项 | 占比 | 频率 | 归属/口径 | 关键证据 |
|---|---|---|---|---|---|
| 1 | 系统调用入口/返回 trampoline 合计 | 26.93% | 7.955 次/请求（futex 3.067 + read 1.978 + write 0.989 + poll 0.921 + pread 0.500 + pwrite 0.500），平均 3.39%/次 | Kernel；exclusive self | `raw-r2/record/agg-symbol.tsv`：15.372+4.558+4.008+1.080+0.444+1.472 |
| 2 | `__x64_sys_futex` 内核体 | 15.44% | 3.067 次/请求（401,967/run） | Kernel；栈归属（10.35% 非 park + 5.09% park） | `agg-attrib.tsv`、`script1-5.txt` |
| 3 | `__x64_sys_pwrite64` | 12.46% | 0.500 次/请求（65,536/run，每次 4 KiB） | Kernel；栈归属；data 类 6.07% | `agg-attrib.tsv` |
| 4 | `__x64_sys_poll` | 10.56% | 0.921 次/请求（120,690/run；strace 128,732） | Kernel；栈归属（park 5.03% + 非 park 5.53%） | `strace-c.txt`、`strace-detail.txt` |
| 5 | `pthread_mutex_lock + unlock` | 9.73%（glibc mutex 总 self 12.74%） | 28.399 对/请求（3,722,447/run） | Context+Core+Backend；libc mutex self | `uprobes.txt`、`agg-symbol.tsv` |
| 6 | `ksys_write`（eventfd 完成唤醒） | 4.81% | 0.989 次/请求（129,656/run） | Kernel；栈归属 | `script*.txt` |
| 7 | `__x64_sys_pread64` | 3.62% | 0.500 次/请求（65,544/run） | Kernel；栈归属；data 类 2.78% | `script*.txt` |
| 8 | `rep_movs_alternative`（4 KiB 载荷拷贝） | 2.46% | 1 次/4 KiB op（pread+pwrite 各一次） | Kernel；self（min 2.31 / max 2.77） | `agg-symbol.tsv` |
| 9 | `ksys_read`（eventfd drain） | 2.28% | 1.978 次/请求（259,316/run；约一半 EAGAIN） | Kernel；栈归属 | `agg-attrib.tsv`、`read_eagain.txt` |
| 10 | `clear_highpages_kasan_tagged` | 0.94% | 0.500 次/请求 | Kernel；self | `agg-symbol.tsv` |

每请求 26,248 cycles、17,888.2 instructions、IPC 0.682；`task-clock` 1.462 s 对 2 CPU × 0.983 s wall 说明约 **25.6% CPU 容量空闲**，`context-switches` 1.028/请求。载荷类单项：`pwrite64` 12.46%（其中 data 类 6.07%）、`pread64` 3.62%（data 2.78%）、`rep_movs` 2.46%；`rep_movs` 是 pwrite/pread 栈内的 leaf，**不能**与总路径再次相加。协调成本分布在多个不同口径中，没有单一可加总比例：单项最大为 trampoline 26.93%，其后 futex 15.44%、glibc mutex 12.74%、poll 10.56%、eventfd write 4.81% 各有不同测量基准。

### B.2 全链路步骤台账

| 步骤 | 位置 | owner | 频率/成本 | 本轮候选 |
|---|---|---|---|---|
| Application 流水线调度 | `apps/sluice-copy/copy_task.cpp:151-253` | App | `run_pipelined_copy_with_context` 0.93% cycles；每 op 1 次 `wait_take` | 无（改应用调度不属于删除现有机器工作） |
| Request construction | `copy_task.cpp:66-88`；`request_scope.cpp:137-187` | App | `submit_read` 4.83%（含 validation/reserve/accept/dispatch） | 无 |
| Validation | `threadpool_backend.cpp:135-149,195-220` | Backend | 内联，无独立 >0.05% 样本 | 无 |
| Reserve | `request_core.cpp:165-182` | Core | 1 mutex/请求；reserve futex 0.34% | 与 accept 合并（条件性，未选） |
| Accept | `request_core.cpp:184-210` | Core | 1 mutex/请求；accept futex 0.05% | 同上 |
| Backend dispatch | `threadpool_backend.cpp:299-332` | Backend | `cond_signal` 1.000/请求；work_mtx_ + futex | 合并两段 work_mtx_（~1 对/请求，未选） |
| Physical execution | `threadpool_backend.cpp:386-479,481-514` | worker | `run_syscall` 19.95%；pwrite 12.46%、pread 3.62% | 无（载荷/FS 工作） |
| Completion / terminal | `threadpool_backend.cpp:444-460`；`request_core.cpp:346-365,393-409` | Core | 2 mutex/请求；offer 0.43%、release 0.28% | `offer+release` 合并（未选） |
| Notification signal | `progress_source.hpp:252-264,426-449` | Backend/Context | `ksys_write` 4.81%；0.989 次/请求；已有 pending 合并 | 无 |
| Publication enqueue | `threadpool_backend.cpp:461-464` | Backend | work_mtx_；worker_loop mutex 2.51% | 不能与 `active_workers_--` 简单合并（锁序） |
| Publication | `threadpool_backend.cpp:550-612,356-374`；`request_core.cpp:438-484` | App | `poll_progress` 2.00%；publish_one 0.43% | `begin+complete` 融合（未选） |
| Observation | `threadpool_backend.cpp:376-384`；`request_core.cpp:513-531,560-572,710-733` | Core | claim 1.0、retire 1.0、ready 2.0 mutex/请求 | `ready+consume` 合并（未选） |
| Wait / progress pass | `request_scope.hpp:146-185`；`async_io_context.cpp:573-696,429-439`；`progress_source.hpp:108-232` | App | `wait_one` 31.63% inclusive；poll 0.921、read 1.978；wait_one mutex 3.10% | **R1（选中）**；poll→阻塞 read（profile rank 1，未入候选）；两次 access_mtx_ 合并（未选） |
| Take / discard | `request_scope.hpp:131-143`；`request_core.cpp:323-344,754` | App/Core | take 0.31%、consume 0.30% | 与 wait 的 ready 检查合并（未选） |
| Slot reclaim | `request_core.cpp:592-597,580-590` | Core | 在 mutex_ 内，无独立样本 | 无 |
| Shutdown / teardown | `copy_task.cpp:335`；`async_io_context.cpp:479-563,77-97` | App/Context/Backend | 每 run 1 次，<0.1% | 无 |

**口径复核（r3）**：上表若干步骤占比与本报告引用的 `raw-r2/record/agg-attrib.tsv` 的 caller 汇总不一致——`poll_progress` 表值 2.00% 对 caller 汇总 1.49%（1.389 + 0.057 + 0.046）；`publish_one` 0.43% 对 0.23%（0.211 + 0.014）；`wait_one` 31.63% 对 32.2%（两个重载行求和 32.197）；`run_syscall` 19.95% 对 19.75%（19.752）。B.1 的主要成本项已逐项复核，但 **B.2 按现状不能精确复算**，其数字只作步骤级定位，不作定量依据。

### B.3 三个候选与选择

| 候选 | 机器工作 | 实测/机制成本 | 判定 |
|---|---|---|---|
| **R1 `R1-drain-eagain-read`（选中）** | 每请求约 0.99 次 `read(2)`（EAGAIN 130,152/run ÷ 131,076 = 0.993 次/请求）及其 trampoline、8 字节输出写、errno 分支与循环回边 | read 栈归属 2.28%、1.978 次/请求；EAGAIN 0.99 次/请求；机制**粗估** ≤4.53% cycles（3.39% 边界 + ≤1.14% read body）——该粗估不是上界，受控 session 的 cycles 点估计 −4.7712% 已超过它，原因未分解；**机制投影**：若每次 drain 恰好删 1 次 read，syscalls/请求约 7.955 → 6.966（−12.4%），这不是 B 的实测总数 | `singleFactor=true`、`addsNewState=false`，进入实现 |
| R2 `R2-atomic-core-summaries` | 每请求 2 对只读摘要 mutex | 约 **0.90% cycles**（12.74% × 2/28.399） | 拒绝：把读侧锁成本转移到写侧 atomic RMW，收益 <1%，无 A/B，缺 W-03 oracle |
| R3 `R3-fuse-publication` | `begin_publication + complete_publication` 合并为一次 core mutex | 约 **0.45% cycles**（12.74% × 1/28.399） | 拒绝：删除有读者的 `publication_inflight` 两阶段状态与 #395 的确定性回归证据面，收益 <1% |

### B.4 选择 R1 的理由

1. **纯删除（代码层）**：不新增生产状态、不扩大临界区、代码不显式把工作转移给其他线程/内核；`addsNewState=false`。旧 session 的 profile 出现未解释的符号位移：`native_queued_spin_lock_slowpath` 0.54% → 1.92%、poll 调用 0.877 → 0.920/请求（见 §D.7）；但该位移没有 `perf sched`/wakeup tracepoint 支持，futex 计数不可复现（§D.6），受控 session 也未做符号级 profile。因此既不能断言"存在成本转移"，也不能断言"没有转移"——它只是未解释的符号变化。
2. **机制上必然**：eventfd 以 `EFD_NONBLOCK | EFD_CLOEXEC` 创建（`progress_source.hpp:74-78`），非 `EFD_SEMAPHORE`，一次成功 read 取走整个计数并清零；`signal()`/`interrupt()` 与 drain 同持 `mtx_`，因此成功 read 后的下一次 read 在所有用户态写者维度上只能 `EAGAIN`。
3. **成本可归属**：read 约 2 次/请求，其中约 1 次为 EAGAIN；trampoline 平均 3.39%/syscall，删除 1 次/请求即约 3.4 个百分点。
4. **可验证（已归档证据）**：`results/mech-counters.tsv` 的 256 MiB、3 次/臂 strace 探针显示 `eventfd_read` 中位数 262,135 → 131,073（−50.0%）；`raw-r2/ab/trace/syscount-A1/B1` 的 bpftrace `@read_eagain` 为 118,115/124,547 → 12/7；5+5 次 perf tracepoint 的 `sys_enter_read` 为 1.990 → 0.987 次/请求。

### B.5 未选择候选的原因（含反例）

**R2 不成立的关键反例**：`outstanding_` / `health_failed_` 改为 `std::atomic` 后，`accept` 的 `++`（`request_core.cpp:202`）与 `complete_publication` 的 `--`（`:473`）会从 `mutex_` 内的普通读写变成锁前缀 RMW；候选只扣了读侧 2 对锁，没有计入这 2 次写侧 RMW。W-03 多提交线程形态下同一 cache line 的共享/弹跳还会增加。收益 0.90% 低于 1% 最小有意义效应，且没有 A/B/mutant；`request_core.hpp:180-182` 的"与权威相同临界区更新"承诺也需重述。若复议，必须先给出 W-03 submit-during-pass 的确定性 oracle。

**R3 不成立的关键反例**：融合删除了 `Slot::publication_inflight`（`request_core.hpp:264`）与 `begin/complete` 两阶段协议，而 `discard_public_result` 的 published-only 门（`request_core.cpp:283-290`）与 mutant `SLUICE_PUBLIC_REQUEST_MUTANT_DISCARD_ACCEPTS_INFLIGHT` 正是靠 epilogue pause 在 inflight 窗口内被杀死（`tests/public_request_release_violation_test.cpp:176-207`；非死亡用例 `tests/public_request_test.cpp:556-607`）；`grep -rl PublicationEpiloguePauseGate tests/*.cpp` 返回 **6 个文件**。融合后该确定性注入点消失，"跟着实现重写 oracle"会违反 AGENTS.md 测试纪律。另有 `CoreOccupancy::publication_inflight` 作为 settlement/teardown 谓词（`async_io_context.cpp:475`、`threadpool_backend.cpp:891`、`uring_backend.cpp:353`）必须按 authority-contraction audit 删除。收益 0.45% <1%，且临界区取并集加宽。

**profile rank 1 未入候选**：`poll(-1)` 可由同一 eventfd 的阻塞 read 取代（约 5.0–5.5% cycles 可删），但结构化 `candidates` 没有给出落选理由；因此不能把 R1/R2/R3 称为对所有可删除工作的完整排序。若后续推进，必须先给出有界等待、drain 非阻塞语义与 dup fd 方案的独立设计。

### B.6 Falsifier

**R1 的语义 falsifier**：
- 出现不持 `mtx_` 的用户态写者，在成功 read 与清 `notification_pending_` 之间写 eventfd → 该窗口内可能丢一次唤醒；当前不可达，必须保持为显式不变式。
- io_uring 内核写者不持 `mtx_`，可在成功 read 与清 pending 之间 `eventfd_signal`；只留下未读就绪位，`PhysicalProbe` 兜底，不丢唤醒；若未来 uring 路径去掉 probe，则 falsifier 成立。
- 成功分支未清 `notification_pending_` → 后续 `signal` 的物理写被跳过而丢唤醒；mutant 已覆盖。
- 任一次非空 drain 观察到多于 1 次 read，或 EAGAIN 探针仍存在。
- 语义 oracle 在 BASE 失败（BASE-GREEN 失败）或 mutant 存活。

**R1 的性能 falsifier**：
- A/B 在干净环境下方向反转，或 CPU 效应消失。
- 跨 session 量级差异无法排除为会话/环境因素（当前 −1.4%~−12%）；受控 session 的 −5.3% 与旧 36 对的 −11.968% 未对齐。不得再把 rusage 与 perf stat 的差异写成"仪器冲突"——§D.5.1 已把它改判为跨 session 差异。
- 负对照出现**与主场景相反方向**的效应，或量级无法按每请求归一化解释，或与 #634 同 cell 的 +0.10% 冲突无法解释。受控 session 负对照 rusage −1.4396%（每请求 0.5880 µs）是**同向小效应**，与"每请求少一次 syscall"的机制预期相容，不是 R1 的反例；旧 session 的 −2.06% 是旧值，不是受控 session 的当前值。
- 任何 wall-time 主张：当前 wall CI 宽且包含 0，不成立。

---

## C. Semantic Evidence

### C.1 old / new 转换

| 项 | BASE `3f22d2d0` | B `a3a14a31` |
|---|---|---|
| 位置 | `include/sluice/async/detail/progress_source.hpp:397-419` | `include/sluice/async/detail/progress_source.hpp:416-462` |
| 成功 read 后 | `continue`（`:405-406`），依赖下一次 read 的 EAGAIN 分支（`:408-415`）清 `notification_pending_` | 在同一 `mtx_` 临界区清 `notification_pending_` 并 `return`（`:439-444`） |
| EAGAIN | 清 pending 并 return | 保留（`:449-457`） |
| EINTR | 重试 | 保留（`:445-448`） |
| wake 注释 | pending 仅在 EAGAIN 清，此时计数器可证为零 | 改为 drain 清该位（`:464-467`） |

最终 diff：`b-round2.diff` 的 sha256 = `ee7c4c3e90ed959ac26d48378f94bdc4ed02ef742830581d61509b0c09ed0199`；本会话执行 `git -C wt-B diff 3f22d2d08a53077b6377934b1895af78126de0a1..HEAD | sha256sum` 得到同值。生产文件仅 `include/sluice/async/detail/progress_source.hpp`（+47/−3）；其余为 `tests/progress_source_drain_semantic_oracle_test.cpp`（539 行）、`tests/progress_source_drain_mechanism_test.cpp`（129 行）、`xmake/tests.lua`（+45）。总 diff 4 文件 +760/−3。

### C.2 线性化点

- 非 `EFD_SEMAPHORE` eventfd 的 8 字节 read 原子取走全部计数并清零；**成功 read 返回 8 个字节即 drain 的线性化点**，`notification_pending_ = false` 紧随其后且仍在 `mtx_` 内。
- ThreadPool 下所有用户态写者（`signal` `:252-253`、`interrupt` `:234-235`）与 drain（`wait_if_unchanged` `:153-166`、`acknowledge_notification` `:274-277`）同持 `mtx_`；写者无法进入 read→clear 窗口。
- io_uring 内核写者不读也不写 `notification_pending_`，只增加 eventfd 计数器并保持 POLLIN；清 pending 后最多让下一次 `signal` 多写一次冗余唤醒，不会出现 `pending=true` 且 `counter=0` 的 strand。

### C.3 Borrow / control / slot 所有权

- `ProgressSource`（同一 `mtx_` 域）继续是唯一 drain 权威：成功 read 清零计数器的同一临界区清 pending。
- 写者（Backend worker 的 `signal`、Context 的 `interrupt`）继续承担"必须制造可读就绪"的唤醒义务；内核继续承担 eventfd 计数器语义；`EINTR` 重试语义保留。
- fd 由 Context/ProgressSource 拥有；host 只通过 `progress_notification_fd()` 借用/注册；`BackendProgressPort::register_notification_with_kernel` 只传递注册，不保留 fd；`retire_notification`（`:284-290`）关闭 fd。
- 用户 buffer/File 的 borrow 到 acquired public terminal；notification fd **不是**用户 File borrow，也不是第二 drain 权威。
- 本切片不移动生产权威，因此 authority-contraction audit 记录为"无既有 mechanism 因本切片变为可删除"。

### C.4 危险交错

1. **ThreadPool（本次测量配置）**：最危险交错不可达。`signal`/`interrupt`/drain 同持 `mtx_`，eventfd 非 semaphore，一次 read 取走整个计数并清零，成功 read 后的第二次 read 只能 EAGAIN。
2. **io_uring**：内核写者不持 `mtx_`，可在成功 read 与清 pending 之间 `eventfd_signal`。旧代码会消费它；新代码把它留成就绪，poll 立即返回一次 progress、多跑一个 pass，`PhysicalProbe`（`:68-72`、`:175-181`）可吸收，不会丢唤醒。该窗口**没有确定性注入 seam**，只能依赖语义论证与既有 `uring_progress_race_test` 的 k11b/k1/k2 用例及全量矩阵。
3. **未来不持 `mtx_` 的用户态写者**：会漏一次唤醒直到下一次 signal；"全部用户态写者持 `mtx_`"必须成为显式不变式，否则 R1 不成立。
4. **成功分支忘记清 pending**：mutant `progress_source_mut_drain_success_keeps_pending` 复现该错误；语义 oracle 在 post-acknowledge re-arm 断言处杀死它。

### C.5 确定性 oracle（语义与机制拆分）

- **语义 oracle** `progress_source_drain_semantic_oracle_test`（9 cases）：只含与 read 次数无关的断言，使用 BASE 已有的 `set_prerevalidate_pause_gate_for_test` / `set_prepark_pause_gate_for_test`。用例：`acknowledged_notification_rearms_the_next_physical_write`、`coalesced_signals_are_consumed_and_the_next_signal_writes`、`progress_signal_while_the_owner_revalidates_wakes_it`、`stale_acknowledgement_cannot_suppress_the_next_wake`、`retired_source_has_no_notification_authority`、`public_publication_is_exactly_once_across_coalesced_acknowledgements`、`undelivered_publication_pins_the_slot`、`notification_fd_borrow_survives_repeated_drains`、`cancel_refusal_and_shutdown_still_settle`。
  - 本会话在 `wt-B` 直接运行该二进制：exit 0；重复一次仍 exit 0。
  - 本会话在 `wt-base-semantic`（HEAD=`3f22d2d0`，`git diff --stat -- include src` 为空，仅未跟踪测试文件与 `xmake/tests.lua` 注册）运行同一二进制：**exit 0**，证明语义 oracle 不依赖本轮新增 seam。
- **机制指标测试** `progress_source_drain_mechanism_test`（4 cases）：`successful_drain_consumes_the_counter_in_one_read`、`empty_drain_takes_the_eagain_branch`、`interrupted_read_retries_without_losing_the_notification`、`retired_source_issues_no_drain_read`。使用 `DrainObservation` 与 `inject_drain_eintr_for_test`，断言非空 drain 恰好 1 次 read、0 次 EAGAIN、EINTR 重试 1 次 read、空 drain 1 次 EAGAIN、retire 后 0 次 read。**不注册为 `add_tests`，不参与 BASE-GREEN**。
  - 本会话在 B 上直接运行：exit 0（4/4）。
  - BASE 对照：结构化 `correctness` 记录 `wt-base-verify`（BASE drain 体 + 仅观测计数器）exit 1，复现 "2 reads/1 EAGAIN"；该回执不是纯净 BASE 头文件上的编译结果，机制测试本身依赖新增 observation/injection seam。
- **归档机制探针**（`results/mech-counters.tsv`，256 MiB，3 次/臂）：`eventfd_read` 中位数 A 262,135（261,399–262,145） vs B 131,073（131,070–131,074），−50.0%；`eventfd_write` A 131,068 vs B 131,073；`poll` A 130,832 vs B 128,390；`futex` A 720,797 vs B 720,139；`pwrite64` 两侧 65,536。
- **归档 bpftrace EAGAIN**（`raw-r2/ab/trace/syscount-A1/B1`）：`@read_eagain` A 118,115/124,547 vs B 12/7；`@read_all` A 236,420/249,262 vs B 131,229/131,226。
- **perf tracepoint 5+5 均值**（`raw-r2/ab/trace/syscalls.txt`）：`sys_enter_read` 1.990 → 0.987 次/请求；futex 3.149 → 2.104；poll 0.877 → 0.920；write 0.995 → 0.987。
- **未归档的 `/tmp` 探针**：另有 1000 轮 signal+ack 探针与 64 MiB consumer 详细 trace 位于 `/tmp/sluice-drain-probe/`；其源文件、编译命令与原始输出未进入本证据根/MANIFEST，本报告**不把它们作为结论依据**。

### C.6 Mutant 回执

- `progress_source_mut_drain_success_keeps_pending`：本会话运行 `wt-B/build/linux/x86_64/release/progress_source_mut_drain_success_keeps_pending` → **exit 1**，FAIL：
  - `[acknowledged_notification_rearms_the_next_physical_write] the post-acknowledge signal re-arms the physical write`
  - `[coalesced_signals_are_consumed_and_the_next_signal_writes] a signal after the coalesced acknowledgement re-arms the write`
  - `[stale_acknowledgement_cannot_suppress_the_next_wake] the post-acknowledge signal re-arms the physical write`
  - 杀死它的是 3 条语义 re-arm 断言（`tests/progress_source_drain_semantic_oracle_test.cpp:93、:113、:163`），**不是** read/EAGAIN 计数。
  - **归档回执与此不符（r3 修正）**：`evidence-r2/mutants/progress_source_mut_drain_success_keeps_pending.run.log`（mtime 16:05，早于拆分提交 `a3a14a31` 的 16:36:26）记录的是**拆分前组合目标**的 4 条 FAIL，用例名为 mechanism 命名：`successful_drain_consumes_the_counter_in_one_read`（2 条）、`interrupted_read_retries_without_losing_the_notification`（1 条）、`stale_acknowledgement_cannot_suppress_the_next_wake`（1 条）；它**不是** 3 条语义 re-arm 断言。审计在当前 `wt-B` 二进制上复跑得到 exit 1 与恰好 3 条语义 FAIL（与上列一致），但该输出**尚未归档**；归档的 16:05 回执属拆分前旧目标，不能作为拆分后的 mutant 证据（§F.5 gate 3）。
- 既有 uring mutant 回执（`evidence-r2/mutants/`）：`progress_source_uring_mut_final_probe_dropped` → FAIL `k1_k2_cqe_before_drain_recovered_by_final_probe`；`progress_source_mut_close_admission_always_signals` 与 `progress_source_uring_mut_close_admission_always_signals` → FAIL `threadpool_close_admission_signals_only_the_open_to_closed_transition`。
- 注意：`evidence-r2/base-gate/`、`evidence-r2/suites/` **与 `evidence-r2/mutants/`** 的部分回执生成于 16:05，早于尝试 3 的拆分提交 `a3a14a31`（`git show -s --format=%cI` = `2026-11-12T16:36:26+08:00`），目标名为 `progress_source_drain_oracle_test`；它们是上一尝试的收据，不构成本轮拆分后语义/机制分类的直接证据。本轮语义 BASE-green 与 mutant 证据由上述 r1 会话实际运行、审计在当前离线构建上的复跑（未归档）与结构化 `correctness` 记录支撑。

### C.7 Conformance matrix 与 sanitizers

| 配置 | 结果 | 来源 |
|---|---|---|
| release / liburing=y | 81/81 passed，exit 0 | 结构化 `correctness.configMatrix`；`evidence-r2/config-matrix/release-liburing-y.log`（该日志为拆分前 81/81 收据） |
| debug / liburing=y | 81/81 passed，exit 0 | 同上 |
| release / liburing=n | 60/60 passed，exit 0 | 同上 |
| debug / liburing=n | 60/60 passed，exit 0 | 同上 |

结构化 `correctness` 记录 13 个既有套件 exit 0：`threadpool_owed_counter_parity_test` 8/8、`threadpool_progress_race_test` 22/22、`threadpool_progress_pass_bound_test`（源码 2 cases）、`threadpool_external_loop_test` 20/20、`progress_notification_suppression_test`（2 cases）、`request_core_protocol_test` 46/46、`request_core_publication_test` 11/11、`request_core_init_closure_test` clean、`request_core_consumer_probe` 2/2、`request_scope_test` 25/25、`app_copy_pipeline_oracle_test` 24/24、`app_copy_consumption_test` 10/10、`threadpool_request_core_integration_test` 17/17。本会话额外直接运行 `threadpool_progress_race_test` → exit 0；`suites/` 收据为拆分前 16:05，拆分后的套件日志未保存。

`sanitizers/` 中只有 `app_copy_pipeline_oracle_test` 与 `threadpool_progress_race_test` 的 TSAN/ASAN 日志非空并通过（22/22、24/24）；`progress_source_drain_oracle_test` 对应的 tsan/asan 日志均为 **0 字节**且目标名为拆分前旧名，**不能**作为语义目标的 sanitizer 通过证据。仅在 liburing=y 配置运行；liburing=n 的 sanitizer 配置未运行。

**receipt 溯源缺口**：`evidence-r2/config-matrix/*.log` 与 `evidence-r2/suites/*.summary.txt` 的 mtime 为 16:05，早于拆分提交，且日志中的目标名为 `progress_source_drain_oracle_test`；`evidence-r2/sanitizers/` 的语义目标日志同样为 16:05、0 字节、旧目标名。本轮结构化证据声称 `progress_source_drain_semantic_oracle_test` 在四配置均运行并通过，但工作区没有保存拆分后的四配置日志与 sanitizer 回执。该缺口必须由 product PR 前的重跑补齐。

---

## D. Performance Evidence

### D.1 冻结协议、环境与污染控制

- **attempt**：2；**pairs**：36；**cell**：`256MiB 4KiB/d4/w1 sync=none no-atomic`；**negative control**：`64KiB/d1/w1`。
- **判据**：`minMeaningfulEffectPct=1`、`aaMaxAbsMeanPct=1.5`、`aaMaxCiWidthPct=3`、`p99BudgetPct=3`。
- **环境纪律**：performance governor（结束恢复 schedutil）；每对前 `Writeback==0` 且 `Dirty<=4 MiB`；2 次 warmup；停顿逐条记录。
- **顺序**：偶数对 AB、奇数对 BA，每对前 `os.sync()` + Writeback/Dirty 轮询 + 2 s；每臂独立进程，`taskset -c 2,3`；runner 绑 CPU0。
- **频率**：测量前 20 个 CPU 全部 schedutil，sudo 设为 performance 后逐一读回；结束恢复 schedutil（`meta.governor_restore.ok=true`）。`cpu2/cpu3 scaling_max_freq=3,500,000 kHz`；A/A 与 A/B 主场景各 72/72 次运行两核采样最大值均等于 max；负对照为 71/72（唯一例外 pair 5 arm B 的 cpu3 观测 3,491,996 kHz，差 0.23%）。三组均 0/72 次每采样点都在 max（workers=1 在两核间迁移，空闲核降频）：A/A 段 cpu2 最低 1,156,843 kHz、cpu3 最低 1,195,281 kHz；三组全局的 cpu3 最低值是负对照的 1,178,333 kHz（r5 更正）。如实报告。
- **Writeback 静默**：主场景 36/36 对前置达到 `Writeback==0`、`Dirty<=616 kB`，无 120 s 超时；运行期 sampler 峰值 Writeback 40,732 kB、Dirty 263,408 kB（256 MiB 输出自身脏页）。负对照前置 `Dirty<=512 kB`；峰值 Writeback 131,440 kB、Dirty 258,752 kB。环境仍不安静。
- **污染检查**：`results/ab-main.processes-before.txt`、`results/ab-neg.processes-before.txt`、`results/ab-after.probes.processes-after.txt` 完整 ps 快照中无 `xmake`/`cc1plus`/`sluice-copy`/`strace`；runner start/每对前/end guard 为空；无重跑、无样本删除。
- **两套 session**：§D.2–§D.4 记录**旧 36 对 rusage session**（主指标为 `RUSAGE_CHILDREN`，无 per-run `perf stat`）；**受控 session** `session-r2r1` 见 §D.5.5，它在同一 session 内完成 A/A 12 对、主场景 36 对与负对照 12 对，并对每次运行同时采集 `perf stat` 与 `rusage`。两套结果不可混用口径。

### D.2 A/A 校准

| 指标 | 值 |
|---|---|
| 有效对 | 36/36（72 次有效运行） |
| 二进制 | 两臂同为 `0eca1011…` |
| CPU mean A / B | 1.543526 s / 1.546018 s（ratio 100.162%） |
| per-pair `(Y−X)/X` | mean **+0.193%**，SD 2.650%，min −5.438%，max +5.489% |
| bootstrap 95% CI | `[-0.6846, +1.0385]`，宽度 **1.723%** |
| 门槛 | `|mean| = 0.193% <= 1.5%`；宽度 1.723% <= 3.0% → **A/A 校准通过** |
| p50 / p95 / p99 (CPU) | `[1.5346,1.5471]` / `[1.6061,1.5956]` / `[1.6114,1.6125]` |
| wall | mean +165.497% `[44.547,325.335]`；median +0.009%；P95 `[1.7468,1.8167]`；P99 `[3.5563,2.1223]` |
| 停顿 | 13 次 `environment_stall`（逐条见 `aa.exclusions`），CPU 统计全部保留；wall 尾部剔除 |

A/A 的 13 次停顿全部满足 `cpu <= 1.5×median`，被标记为 environment_stall；CPU 统计包含全部 72 次有效运行。

**受控 session 的 A/A 与本次不同**：受控 session 的 A/A 为 12 对，rusage +0.6512% `[-1.2814,+2.5974]`、宽度 3.8789（未过 3.0 门）；逐对 sd 3.5908%，旧 session 为 2.650%。两次 A/A 的差异未分解（样本量、宿主噪声、频率下探都可能相关），本报告不把旧 A/A 的 1.723% 宽度当作受控 session 的噪声校准（§D.5.5）。

**冻结 ops 来源偏差（如实披露）**：`results/aa2-4k-d4.meta.json` 的 `frozen_ops_source` 是 pair 1 arm B（首个有效运行是 BA 的 B 臂），而协议要求首个有效 **A** 运行；首个有效 A 运行（pair 1 arm A, pos 2）的 `read_ops=65540`/`write_ops=65536` 与冻结值相同，72 次运行全部一致，数值无影响，但来源偏差保留记录。A/B 主场景的 `frozen_ops_source` 是首个有效 A 运行（pair 1 arm A pos 2），符合协议。

### D.3 A/B 主场景

| 指标 | A（BASE） | B（`a3a14a31`） | 相对差 / 判据 |
|---|---|---|---|
| 有效对 | 36/36 | 36/36 | 72/72 oracle ok |
| CPU mean | 1.546575 s | 1.361066 s | **−11.968%**，bootstrap 95% CI `[-12.644,-11.266]`，宽度 1.378% <= 3.0%，`|mean| > 1.5%` |
| per-pair | — | — | mean −11.968%，SD 2.114%，min −15.790%，max −7.035% |
| CPU p50 / p95 / p99 | `[1.5446,1.5885,1.5958]` | `[1.3547,1.3987,1.4059]` | p95 1.5885 → 1.3987；p99 1.5958 → 1.4059 |
| wall | mean +97.396% `[-5.135,+229.087]`；median −6.563%；p95 1.7547 / p99 2.8056 | p95 1.6398 / p99 1.6530 | wall **被停顿污染，不作结论**；剔除双方无 stall 的 26 对后 −6.474% `[-7.620,-5.299]` |
| 停顿 | 12 次 `environment_stall`（10A 44.968 s、10B 7.558 s、12A 21.694 s、13A 12.312 s、14A 21.501 s、26B 18.825 s、27B 27.114 s、28B 21.706 s、30B 18.845 s、32A 16.671 s、32B 5.370 s、34A 36.155 s），CPU 均正常（1.389–1.591 s） | — | 保留在 CPU 统计，仅从 wall 尾部剔除 |
| oracle | 72/72 exit 0、bytes=268,435,456、`short_writes=0`、dst sha256 = src | — | `oracleFailures=0` |

**口径说明**：CPU 指标是 `resource.getrusage(RUSAGE_CHILDREN)` 的 user+sys delta（微秒），不是 `perf stat` 的 task-clock，也不是 wall。B 二进制 sha256 `db972b19…`，与 `wt-B` 源码树一致；A 二进制 `0eca1011…`。该 36 对结果是 r1 的冻结主指标记录；其**幅度**在后续会话中未被复现（8 对完整纪律 −6.7%；4 对 sync-only −8.8%；10 对 perf stat session 仅 −1.4%；受控 session 36 对 rusage −5.3359%），量级差异的讨论见 §D.5.4 与 §D.5.5。

### D.4 负对照（64 KiB/d1）：每请求归一化

负对照问的是"同一删除在请求数少 16 倍、每请求基线更高的 cell 上是否仍有同号效应"。两个 session 的负对照都**不是零效应**，也都**不是主结果的反例**；按每请求归一化，二者节省同量级。

| session | 对 | 口径 | A | B | 相对差 | A 每请求基线 | 每请求节省 |
|---|---|---|---|---|---|---|---|
| **受控 session（session-r2r1，r4 新增）** | 12 | rusage CPU | 0.326828 s | 0.322011 s | **−1.4396%** `[-2.7800,-0.0811]` | 39.8911 µs/请求 | **0.5880 µs/请求** |
| 受控 session（session-r2r1） | 12 | instructions | 421,222,494.6 | 413,958,605.1 | **−1.7238%** `[-1.8758,-1.6282]` | 51,412.49 条/请求 | 886.60 条/请求 |
| 旧 session（r1） | 36 | rusage CPU | 0.259043 s | 0.253560 s | **−2.059%** `[-3.136,-0.988]`，宽度 2.148% <= 3.0%，`|mean| > 1.5%` | 31.62 µs/请求 | 0.651 µs/请求 |

- 受控 session 的 12 对里，rusage 逐对值不是全部为负（`[3.023,-2.724,-0.047,-2.662,-1.872,-3.213,1.078,-5.907,-3.180,0.472,0.708,-2.951]`），但 bootstrap CI 上界 −0.0811 < 0；instructions 则 12/12 为负且 CI 极窄 `[-1.8758,-1.6282]`。**结论只能是"方向为负、量级很小"，不是"零效应"，也不是"非劣证明"**。
- 百分比更小由**每请求基线更高**解释：负对照 A 侧 39.8911 µs/请求（受控 session）/31.62 µs/请求（旧 session），主场景 A 侧 13.0043 µs/请求（受控 session）/11.80 µs/请求（旧 session）。由记录值直接相减可得每请求节省 0.5880（受控负对照）对 0.7040（受控主场景）µs/请求，同量级（均值之比分别为 −1.4739% 与 −5.4132%；逐对均值分别为 −1.4396% 与 −5.3359%）。**请求数本身不是百分比效应的缩放变量**（r3 已撤回"1/16 请求数缩放"）。
- 受控 session 负对照的其余三口径同号：cycles −1.8123% `[-2.8861,-0.7421]`、task-clock −1.6267% `[-2.7020,-0.5981]`、perf 工作负载 user+sys −1.7015% `[-2.9114,-0.5808]`；但逐对 cycles/task-clock/rusage 都含正值（例如 cycles 3 对为正），所以只是 CI 整体为负，不是每对为负。
- 停顿与 wall：受控负对照 4 次 `environment_stall`（9/B 29.598 s/0.322 s、9/A 19.388 s/0.333 s、10/B 12.351 s/0.335 s、12/A 20.825 s/0.330 s；wall_s/cpu_s），CPU 统计保留；wall mean +68.1541% `[-25.3307,+230.0370]`，**不作判据**。
- 旧 session 的负对照 p50/p95/p99（CPU）为 A `[0.2570,0.2694,0.2774]`、B `[0.2520,0.2688,0.2740]`，13 次 environment_stall，oracle 72/72 ok；wall mean +248.349%，同样不作判据。

**历史口径的每请求换算（保留，供 spread 对照）**：旧负对照 0.259043 s ÷ 8,193 请求 = 31.62 µs/请求；旧主场景 1.546575 s ÷ 131,076 = 11.80 µs/请求；旧 8 对会话 1.652859 s ÷ 131,076 = 12.61 µs/请求；按各自百分比效应折算的每请求节省为 0.651 / 0.845 / 1.412 µs/请求（负对照 / 8 对 / 36 对）。这些是旧 session 的数值，r4 不据此推断因果。

**与 #634 同 cell 的历史对照（保留）**：该 64 KiB/d1 cell 在 #634 的负对照为 **+0.10% `[-1.56,+1.77]`**（`docs/evidence/621-threadpool-owed-count-20261112/formal-ab/analysis.txt:12`；该证据 `REPORT.md` §2–§4）。两个 CI 区间重叠于 `[-1.56,-0.988]`，点估计方向相反。在该 cell 每请求效应只有约 0.59–0.65 µs（受控 session 0.5880、旧 session 0.651 µs/请求）的量级上，该差异记录为版本/二进制布局差异下的未解释项；它不构成本轮负对照的支持证据，也不足以单独阻断，但必须保留（§F.5 gate 8）。

### D.5 跨计数口径对账与受控 session（r4 新增 session-r2r1）

#### D.5.1 原记录：10 对 perf stat 交叉检查，以及被误置的"rusage vs perf stat 冲突"

| 口径 | A | B | 相对差 | 备注 |
|---|---|---|---|---|
| 冻结主指标：rusage user+sys（36 对） | 1.546575 s | 1.361066 s | **−11.968%**，CI `[-12.644,-11.266]` | 36/36 对为负 |
| 10 对 session perf stat task-clock | 1476.00 ms/run | 1450.69 ms/run | −1.71% | t=−1.69，**不显著** |
| 10 对 session perf stat instructions/request | 17,926.25 | 17,493.13 | −2.42% | 10/10 |
| 10 对 session perf stat cycles/request | 26,403.71 | 25,849.10 | −2.10% | 10/10 |
| 10 对 session perf 打印 user+sys（r3 复核补入） | 1.489662 s | 1.468571 s | **−1.416%**（均值之比；每对均值 −1.38%，10 对中 2 对为正） | 与该 session 的其余三口径一致 |

**定性更正（r3）**：上表后四行来自**同一个** 10 对 perf stat session（`raw-r2/ab/stat/stat.txt`、`stat2.txt`）。本会话复核：逐 run 提取 `seconds user` + `seconds sys`，A 均值 **1.489662 s**、B 均值 **1.468571 s**，相对差 **−1.416%**；逐对 `(B−A)/A` 均值 **−1.38%**，10 对中 2 对为正（+4.87%、+0.17%）。**该 session 内部四个口径互相一致（−1.4%~−2.4%），不存在"rusage 与 perf stat 冲突"。** r2 的"D.5.1 rusage vs perf stat 冲突"是误置：真实差异是**跨 session**——36 对 rusage session（−11.968%）与 10 对 perf stat session（约 −1.4%~−2.4%）之间约 9–10 个百分点的差距。

r2 声称的成因（"缺少 sync/Writeback 纪律造成环境假象"）**不是已证结论**：(a) `raw-r2/ab/stat/` 只有各 run 的 perf 输出，没有 sync/Writeback/warmup/顺序等 protocol 字段；(b) 该 session 的 task-clock 均值 1476.00 ms **低于** 8 对会话的 1637.42 ms 和 36 对 rusage 的 1.546575 s，与"背景回写抬高基线使百分比变小"的方向相反；(c) 没有做隔离纪律的剂量实验（§D.9）。可支持的窄结论只有：**该交叉检查没有可核验的预注册 protocol 记录，不能作为 36 对结果的反证**；它也不能被当作"环境假象"直接丢弃。

同一份 raw 文件的 run 头顺序也须更正：`grep -n '^### ' raw-r2/ab/stat/stat.txt raw-r2/ab/stat/stat2.txt` 给出 `A run 1, B run 1, A run 2, B run 2, … A run 10, B run 10`，即**每对都是 A 先 B 后，没有 D.1 协议所定义的 AB/BA 交错**。因此 r2 的"10 对交错"描述错误，该 session 存在顺序/漂移混杂（2/10 对为正与这一设计相容），不能与交错设计的 36 对 A/B 等量齐观。

#### D.5.2 对账实验 1–4

| 实验 | 配置 | 结果 | 来源 |
|---|---|---|---|
| 1 | plain harness（`/usr/bin/time` 的 user+sys），复用 dst，8 对 AB/BA，`sync; sleep 2` | README 概述约 −12% CPU；raw 逐行 A user 0.59–0.64 / sys 0.81–1.01，B user 0.45–0.53 / sys 0.71–0.88（README 正文写作 0.60–0.64 / 0.95–0.97 与 0.49–0.53 / 0.82–0.88，未覆盖 raw 的端点；结论量级不受影响） | `reconciliation/README.md:9-12`、`plain-reused-dst.txt`（16 行 = 8 对） |
| 2 | plain harness，每 run 新建 dst，6 对 | 效应保持（A ~1.40 s、B ~1.21 s CPU），排除 dst 复用导致的 btrfs 状态差异 | `README.md:14-15`、`plain-fresh-dst.txt`（12 行 = 6 对） |
| 3 | `sudo perf stat -e task-clock,cycles,instructions` + **sync-only**（`README.md:17-18` 只记录 `sync; sleep 2`，**没有** `Writeback==0`/`Dirty<=4096 kB` 轮询），4 对 | 逐对 task-clock −7.96/−10.51/−6.43/−9.43%，cycles −6.91/−8.75/−6.15/−9.02%，instructions −7.87/−11.11/−6.27/−9.92%；均值 task-clock **−8.58%**、cycles **−7.71%**、instructions **−8.79%**、rusage **−8.77%** | `README.md:17-24`、`perfstat-with-sync.txt`（8 个 arm 块） |
| 4 | 本轮 8 对 perf stat，**完整预注册纪律**（`os.sync()` + Writeback/Dirty 轮询 + 2 warmup + AB/BA），见 §D.5.3 | instructions −6.3384%、cycles −5.9915%、task-clock −6.4836%、rusage −6.7022%，CI 全 < 0 | `recon-provenance.txt:15-39`、`results/recon-perf-4k-d4.{tsv,meta.json,analysis.txt,analysis.json}` |

实验 1 的原始行只有 2 位小数（user/sys），README 的 −12% 是概述值；实验 2 只用来排除"dst 复用"这一混杂因素，不用于点估计。**实验 3 是 sync-only 会话，不是完整静默纪律**（无 Writeback/Dirty 轮询记录）；实验 4 是唯一有完整 protocol 记录的会话。二者都与同 session rusage 一致（−8.77% 与 −6.7022%），因此 r2 把实验 3 称为"受纪律/同条件会话"不准确，r3 改称 sync-only。

#### D.5.3 本轮 8 对受纪律 perf stat

协议（`recon-provenance.txt:15-39`；`results/recon-perf-4k-d4.meta.json`）：

- 8 对交错 AB/BA（奇数对 BA、偶数对 AB），每臂独立进程：`sudo perf stat -e task-clock,cycles,instructions -- taskset -c 2,3 <bin> --buffer-size 4096 --pipeline-depth 4 --workers 1 --sync none --no-atomic <src> <dst>`。
- 每对前 `os.sync()` + 轮询 `/proc/meminfo` 至 `Writeback==0` 且 `Dirty<=4096 kB`（8/8 对无 120 s 超时，正式对 pre Dirty 最大 144 kB）+ 2 s。
- 2 次丢弃 warmup（A wall 1.170 s / perf_cpu 1.5139 s；B wall 1.122 s / perf_cpu 1.4264 s；均 oracle ok）。
- 二进制：A `0eca1011…`、B `db972b19…`，测量前后各核验一次，一致；src `a1f8dd97…`，268,435,456 B。
- **该 8 对 session 无同 session A/A**：它没有 A/A 对照。§D.2 的 A/A（+0.193%，CI 宽度 1.723%）来自 36 对的 `results/aa2-4k-d4.json` 会话（同一二进制两臂），是**跨 session** 的零效应带，不能用来说明该 8 对 session 自身的噪声水平。r4 的受控 session 另有自己的同 session A/A（12 对），但宽度 3.8789 未过门（§D.5.5），因此受控 session 的 A/A 也不能反过来当作旧会话的校准。

| 指标 | A 均值 | B 均值 | 每对相对差（10,000 次 bootstrap percentile，`default_rng(5950)`） |
|---|---|---|---|
| instructions | 3.3737e9 | 3.1591e9 | **−6.3384%** `[-7.4000,-5.1915]`，8/8 为负 |
| cycles | 4.6826e9 | 4.4012e9 | **−5.9915%** `[-7.2767,-4.9090]`，8/8 |
| task-clock | 1637.42 ms | 1530.70 ms | **−6.4836%** `[-8.0801,-4.9612]`，8/8 |
| 工作负载 rusage（perf 打印 user+sys） | 1.6529 s | 1.5415 s | **−6.7022%** `[-8.3084,-5.2332]`，8/8 |
| wrapper `RUSAGE_CHILDREN`（含 sudo/perf） | 1.6938 s | 1.5824 s | −6.5453% `[-8.0959,-5.1415]`，8/8 |

- 逐对值：instructions `[-6.603,-3.14,-9.078,-6.169,-7.523,-5.823,-6.079,-6.293]`；cycles `[-4.8,-3.415,-9.775,-5.461,-6.844,-6.323,-5.598,-5.717]`；task-clock `[-5.597,-2.439,-10.883,-5.709,-7.855,-6.189,-5.932,-7.265]`；rusage `[-6.002,-2.809,-11.164,-5.921,-7.833,-6.129,-6.296,-7.462]`。均值为逐 run 算术平均，相对差按每对 `(B−A)/A` 计算，**不是两个均值的商**。
- 分布（结构化 `pairs=8` 结果）：instructions p50/p95/p99 A `3.3856/3.4435/3.4644e9`、B `3.1637/3.1849/3.1883e9`；rusage A `1.6500/1.7025/1.7165 s`、B `1.5471/1.5671/1.5686 s`。B 的每个分位都低于 A 的对应分位。
- oracle：8/8 对有效、0 次失败；16/16 正式运行（另加 2 次 warmup）`rc=0`、`bytes_copied=268,435,456`、`short_writes=0`、dst sha256 == src sha256、`read_ops/write_ops=65540/65536` 全部一致（`ops_consistency.checked_runs=18`，`mismatches=[]`）。
- 环境：governor 开始前 20 个 CPU 已是 performance（继承状态），测量前用 `sudo` 全部设回 performance 并读回 20/20；阶段结束后恢复 schedutil、读回 20/20，`meta.governor_restore.ok=true`，18:14 再次核验仍为 schedutil。`cpu2/cpu3 scaling_max_freq=3,500,000 kHz`（intel_cpufreq）；16 次正式运行中 15 次两核采样最大值均等于 3,500,000，pair 1 arm B 的 cpu3 有一次采样 3,584,946 kHz（高于 policy max，发生在 36.5 s 停顿期间）。**严格"每个采样点 `cur==max`"为 0/16**，与既有 36 对 A/B 的 0/72 相同（workers=1 在两核间迁移、空闲核降频），故无法声称连续 `cur==max`；未据此剔除任何样本。
- 停顿：`wall>5 s` 共 1 次——pair 1 arm B `wall=36.522 s`、`perf_cpu=1.554 s`、`RUSAGE_CHILDREN=1.600 s`；按预注册规则（`cpu <= 1.5×` 该臂中位数 1.587 s）标记为 `environment_stall`，**只从 wall 尾部剔除**，CPU 统计保留该运行。复测 wall 原始 B p95=24.367 s / p99=34.091 s，剔除后 B p95=1.786 s / p99=1.792 s，A p95=1.869 s / p99=1.871 s；wall mean 因此为 +237.5%，与 36 对 A/B 一样**不作判据**。
- 环境残留：正式对前 `Writeback=0`、`Dirty<=144 kB`；运行中峰值 `Writeback=9,412 kB`、`Dirty=264,280 kB`（256 MiB 输出自身的脏页）；guard 检查（`xmake`/`cc1plus`/`sluice-copy`）在开始、每对/每臂前与结束均无命中（`guard_at_start=[]`、`guard_at_end=[]`）；loadavg 0.18 → 1.18。

#### D.5.4 结论：跨 session 差异未解释；受控 session 未确认量级

**窄结论（可支持）**：10 对 perf stat 交叉检查没有可核验的 protocol 记录（`raw-r2/ab/stat/` 只有 perf 输出），且每对 A→B 顺序而非 AB/BA 交错，因此**不合规、不能作为 36 对 −11.968% 的反证**；同时它内部四口径一致，也不能被贴上"环境假象"标签，更不能据此断言 −2% 的成因。

**跨 session 量级 spread（未解释，r4 更新）**：

| session | 设计 | 口径 | 结果 |
|---|---|---|---|
| 36 对 A/B（旧） | AB/BA 交错、同 session A/A 36 对、rusage | CPU user+sys | **−11.968%** `[-12.644,-11.266]` |
| 10 对 perf stat（旧） | **A→B 顺序**，无 protocol 记录 | user+sys / task-clock / instructions / cycles | −1.416% / −1.71% / −2.42% / −2.10% |
| 4 对 sync-only（旧） | AB/BA，仅 `sync; sleep 2` | task-clock / cycles / instructions / rusage | −8.58% / −7.71% / −8.79% / −8.77% |
| 8 对完整纪律（旧） | AB/BA，Writeback/Dirty 静默 | task-clock / cycles / instructions / rusage | −6.4836% / −5.9915% / −6.3384% / −6.7022% |
| **受控 session（新，session-r2r1）** | **同 session A/A 12 + 主 36 + 负 12；AB/BA；完整静默；per-run `perf stat` + `rusage`** | task-clock / cycles / instructions / rusage | **−5.3220% / −4.7712% / −5.5528% / −5.3359%** |

量级跨度约 −1.4%~−12%，**没有任何一次会话在完整纪律下与 36 对结果对齐**。受控 session 落在 −5.3% 附近（rusage −5.3359%），位于旧 8 对（−6.7022%）与旧 10 对（−1.416%）之间，低于旧 36 对（−11.968%）。**受控 session 没有分离该跨度的任何成因**：宿主频率下探（§D.5.5）、`perf stat`/`sudo` 开销、缓存与写回状态、样本量等都只是候选因素，本轮没有剂量实验，故不作因果断言。r2 的"约 −2% 被排除"在 r3 撤回，r4 维持：8 对会话自身的数据在 95% 上确实排除 −2%（CI 上界 −4.9~−5.2），但 10 对 session 直接测得 −1.4%~−2.4%，受控 session 又测得 −5.3%；在差异未解释前，不能把"−2% 已排除"作为结论。

**纪律假说未验证**：`reconciliation/README.md:6-7` 与结构化 `priorCrossCheck` 只是**断言**"交叉检查未执行纪律"；没有原始 protocol，也没有控制 Dirty/Writeback 的剂量实验。且交叉检查的 task-clock 基线（1476.00 ms）**低于** 8 对（1637.42 ms）、36 对（1.546575 s）与受控 session 的主场景 A 侧（1647.05 ms），与"背景回写抬高基线"的预期方向相反。该假说不能作为结论。

**裁决**：`END_TO_END_VERDICT = NOT_ESTABLISHED`、`PRODUCT_READINESS = NOT_READY`。§F.5 gate 1 的受控 session 已执行（§D.5.5），但未通过 A/A 安静度门，`confirmed=false`；主场景四组虽在均值/CI 上为负，逐对正号为 1/3/4/4，且四组最小绝对值为 cycles 4.7712%。关闭条件更新为：让 A/A 宽度 ≤3.0（追加对数或更安静环境），重跑 36 对主场景，并给出跨 session spread 的一致解释；在此之前不得引用任何单一百分比作为收益。

#### D.5.5 受控 session（session-r2r1）：A/A 12 对 + 主场景 36 对 + 负对照 12 对

**目的**：执行 §F.5 gate 1 的受控复测——把 A/A、主场景与负对照放进**同一 session**，用同一纪律、AB/BA 交错，并对每次运行同时采集 `perf stat`（task-clock/cycles/instructions）与 `RUSAGE_CHILDREN` 的 user+sys。

**会话与协议**

| 项 | 值 | 来源 |
|---|---|---|
| session | `session-r2r1`，2026-11-12T18:52:27.563+08:00 → 19:07:59.743+08:00（15m32s） | `results/session-r2r1.analysis.json` 的 `session`；`results/session-r2r1.analysis.txt:1-2` |
| 段与顺序 | A/A 12 对（两臂均 A）、主场景 36 对（X=A, Y=B）、负对照 12 对（64 KiB/d1）；段内奇数对 BA、偶数对 AB | `session-r2r1/PROVENANCE.txt`；`results/session-r2r1-*.meta.json` 的 `protocol` 与 `pair.order` |
| 正式运行 | **120**（24 + 72 + 24），全部保留；另每段 2 次丢弃 warmup（每臂 1 次） | 三个 TSV 行数（24/72/24）；`session-r2r1/run.log` |
| 每 run 命令 | `sudo perf stat -e task-clock,cycles,instructions -- taskset -c 2,3 <bin> --buffer-size <4096\|65536> --pipeline-depth <4\|1> --workers 1 --sync none --no-atomic <src> <dst>` | `session-r2r1/runner_session.py`；`PROVENANCE.txt` |
| 二进制 | A `0eca1011…`、B `db972b19…`；运行前后 sha256 复核一致 | `session.binary_a_sha256_recheck`、`binary_b_sha256_recheck` |
| 源 | `data/src-256m.bin`，268,435,456 B，sha256 `a1f8dd97…`，btrfs，warm；运行前后复核一致 | `session.src_sha256_recheck`；`PROVENANCE.txt` |
| 每对前 | `os.sync()` + 轮询 `Writeback==0` 且 `Dirty<=4096 kB`（最多 120 s）+ 2 s | `pairs[].pre`；`pre_run_timeouts=[]` |
| 静默实测 | 120 次 `pre.final_wb_kb` **全为 0**；`pre.final_dirty_kb` 最大 **524 kB**（A/A 段；主/负对照段最大 188 kB）；无轮询超时 | r4 解析三个 meta（附录 C.3） |
| guard | `xmake`/`cc1plus`/`sluice-copy` 在开始、每次 warmup、每对前、pre-sync 后、每臂前与结束检查；runner 命中即 `RuntimeError` 中止；session 完成（`aborted=None`），起止快照为空 | `session-r2r1/runner_session.py:667-715`；`session.guard_at_start/end` |
| oracle | 120/120：`rc=0`、`bytes_copied=268435456`、`short_writes=0`、`dst_matches_src=True`、`read_ops/write_ops` 等于冻结值（65540/65536；4097/4096）；`oracleFailures=0` | 三个 TSV 全行复核（附录 C.3） |

**Governor / 频率**

| 项 | 值 |
|---|---|
| 设置前 | 20 个 CPU 全部 `schedutil` |
| 设置 | 18:52:27.861 设为 `performance`，rc=0，`after` 20/20 `performance`；cpu2/cpu3 `cur==max==3,500,000 kHz` |
| 恢复 | 19:07:59.743 恢复 `schedutil`，rc=0，20/20 读回 `schedutil`，`governorRestored=true` |
| 运行中采样 | **下探**：各段 cpu2 最低 1,196,290 / 1,182,595 / 1,174,069 kHz、cpu3 最低 1,170,505 / 1,173,094 / 1,196,203 kHz（aa/main/neg）；三段 `freq_eq_all=false` |
| pre-run 读数偏差 | 主段 pair 11 cpu3 为 3,339,500 kHz；arm/warmup 级低于 max 的读数：A/A warmup1 cpu2 1,197,317、pair 2/B cpu3 3,491,891、pair 4/A cpu2 3,193,079；主段 pair 11/B cpu3 3,405,094、pair 20/B cpu2 3,206,122、pair 24/A cpu2 3,033,808；负对照 pair 3/B cpu3 1,919,971、pair 10/A cpu2 3,491,840、pair 10/B cpu2 1,644,941（r5 补全） |

因此"测量期间持续 `cur==max`"在本虚拟化 Xeon E5 主机上**未能确认**；可确认的是 governor 设置、setup 时刻频率、运行前后的二进制与源 hash。该偏差如实披露，CPU 统计仍按预注册口径包含全部有效运行，未据此剔除任何样本。

**结果（每对相对差 `(Y−X)/X`，bootstrap percentile 95% CI，10,000 次，seed 5950）**

| 指标 | A/A 12 对（两臂 A） | 主场景 36 对（X=A, Y=B） | 负对照 12 对（64 KiB/d1） |
|---|---|---|---|
| instructions | −0.2454% `[-2.2373,+1.7978]`，宽度 4.0351 | **−5.5528%** `[-6.5241,-4.5699]`，宽度 1.9541 | −1.7238% `[-1.8758,-1.6282]` |
| cycles | +0.5807% `[-1.0297,+2.2418]`，宽度 3.2715 | **−4.7712%** `[-5.8198,-3.7236]`，宽度 2.0962 | −1.8123% `[-2.8861,-0.7421]` |
| task-clock | +0.6830% `[-1.3375,+2.7329]`，宽度 4.0704 | **−5.3220%** `[-6.6076,-4.0105]`，宽度 2.5970 | −1.6267% `[-2.7020,-0.5981]` |
| rusage CPU | **+0.6512%** `[-1.2814,+2.5974]`，宽度 **3.8789** | **−5.3359%** `[-6.6299,-4.0128]`，宽度 2.6171 | −1.4396% `[-2.7800,-0.0811]` |
| perf 工作负载 user+sys（补充） | +0.6579% `[-1.3655,+2.7085]`，宽度 4.0740 | −5.4768% `[-6.8028,-4.1181]` | −1.7015% `[-2.9114,-0.5808]` |

**四口径一致性（受控 session 内）**：主场景四个口径的点估计为 instructions −5.5528%、cycles −4.7712%、task-clock −5.3220%、rusage CPU −5.3359%，最大两两差 0.7816 pp（instructions 与 cycles），rusage 与 instructions 相差 0.2169 pp；四组 CI 上界全部 <0。**四组均值/CI 方向一致**，不存在旧 10 对 session 那种"rusage 与 perf stat 冲突"的形态；但逐对符号并不全负（§D.5.5 下一段）。A/A 段四口径的点估计都在 ±0.683% 内，但四组 CI 宽度都 >3.0（instructions 4.0351、cycles 3.2715、task-clock 4.0704、rusage 3.8789）——**A/A 安静度门在四个口径上都不通过**，不是 rusage 单一口径的问题。

**主场景每臂 p50 / p95 / p99（36 次/臂）**

| 指标 | A（BASE） | B（`a3a14a31`） |
|---|---|---|
| instructions | 3,397,361,214 / 3,523,081,080 / 3,543,649,028 | 3,195,531,159 / 3,348,358,374 / 3,381,347,148 |
| cycles | 4,677,097,528 / 4,872,891,398 / 4,962,205,123 | 4,443,155,044 / 4,695,369,781 / 4,773,653,368 |
| task-clock | 1,641.27 / 1,735.87 / 1,754.23 ms | 1,550.80 / 1,651.31 / 1,684.75 ms |
| rusage CPU | 1.6967 / 1.7941 / 1.8165 s | 1.6008 / 1.7109 / 1.7458 s |

B 的每个分位都低于 A 的对应分位。

**逐对符号（r5 补披露）**：36 对中并非每对为负——instructions 1/36 为正（pair 12，+1.462%）、cycles 3/36（pair 12 +2.816%、14 +1.523%、33 +0.819%）、task-clock 4/36（pair 6 +1.015%、12 +4.173%、14 +2.855%、33 +1.383%）、rusage 4/36（pair 6 +0.878%、12 +4.402%、14 +2.739%、33 +1.484%）。因此成立的表述是"四组均值与 95% CI 上界为负"，**不是**"36/36 逐对为负"；逐对正号集中在个别对上，但不改变"均值/CI 为负"这一层面。

**每请求归一化**：主场景 A 侧 1.704554 s ÷ 131,076 = 13.0043 µs/请求，B 侧 1.612283 s ÷ 131,076 = 12.3004 µs/请求，均值差 **0.7040 µs/请求**（均值之比 −5.4132%；逐对均值 −5.3359%）；instructions 25,950.05 → 24,496.66 条/请求，节省 1,453.40 条/请求（均值之比 −5.6007%）。负对照 A 39.8911 → B 39.3032 µs/请求，均值差 0.5880 µs/请求（§D.4）。A/A 段 A 12.9418 → B 13.0194 µs/请求（均值差 −0.0776 µs/请求，即同二进制两臂无系统性差异）。

**门槛判定（分析器实现：`session-r2r1/analyze_session.py:262-272`）**：`confirmed=false`。唯一未通过项是 **A/A 安静度**：`aaPairs=12`、`aaCpuRelDiff=+0.6512%`（`|x|<=1.5` 通过），但 `aaCiHigh-aaCiLow = 2.5974-(-1.2814) = 3.8789 > 3.0`。其余条件全部通过：`mainPairs=36`；主场景 instructions/cycles/task-clock/rusage 四组 CI 上界均 <0；`mainCpuRelDiff=-5.3359% <= -1%`；`oracleFailures=0`；`governorRestored=true`。A/A 噪声不是 `sudo`/`perf` 固定开销造成的：用 perf 报告的工作负载 user+sys（`perf_cpu_s`）计算 A/A 为 +0.6579% `[-1.3655,+2.7085]`（宽度 4.0740），同样未过 3.0。

**A/A 失败的量级背景（算术外推，不是重测）**：受控 session 的 A/A 是 12 对、逐对 sd 3.5908%、范围 `[-5.059,+6.806]%`；旧 36 对 A/A 的逐对 sd 为 2.650%、CI 宽度 1.723%。若仅按样本量把 12 对的宽度按 1/√3 收缩，约得 2.24%（<3.0）；但逐对 sd 也从 2.650% 升到 3.5908%，所以**样本量不是唯一因素**。本轮没有做追加 A/A 对数或更安静环境的复测，A/A 噪声来源未分解。

**停顿（wall>5 s）**：共 **21 次**（A/A 4、主场景 13、负对照 4），全部保留；按预注册环境停顿规则，21 次均满足 `cpu <= 该臂中位数×1.5`，标记 `environment_stall`，**仅用于 wall 尾部，不剔除 CPU 样本**。逐条（segment pair/arm wall_s cpu_total_s）：

- aa-4k-d4：1/B 20.950 1.729；3/B 18.471 1.678；4/B 13.598 1.667；6/B 37.441 1.720。
- main-4k-d4：6/B 40.637 1.660；7/A 20.740 1.687；9/A 21.548 1.682；11/B 12.582 1.593；11/A 9.189 1.680；13/B 37.440 1.650；26/A 46.643 1.637；27/A 12.295 1.806；28/A 7.317 1.743；29/A 21.709 1.790；31/A 19.668 1.822；33/B 16.625 1.695；33/A 20.033 1.670。
- neg-64k-d1：9/B 29.598 0.322；9/A 19.388 0.333；10/B 12.351 0.335；12/A 20.825 0.330。

**wall 尾部（剔除 environment_stall 后；括号内为未剔除原始值）**：A/A A p95/p99 = 2.178/2.413 s（原始 2.178/2.413），B = 2.429/2.639（原始 28.371/35.627）；主 A = 1.971/2.030（原始 21.588/37.916），B = 1.901/2.191（原始 21.829/39.518）；负 A = 1.660/1.952（原始 20.034/20.667），B = 0.866/0.869（原始 20.112/27.700）。wall mean：A/A +354.9464%、主场景 +88.2691% `[-25.3260,+253.7874]`、负对照 +68.1541% `[-25.3307,+230.0370]`；**全部不作判据**。

**本 session 未采集**：futex/read 系统调用计数（未运行 bpftrace/strace，避免扰动计时）；TSV 的 `read_ops`/`write_ops` 是程序计数器，已全部一致并等于冻结值。旧 session 的 bpftrace 计数是独立补充证据（§D.6），不用于本 session。

**与旧 session 的量级对比（只列数值，不作因果解释）**：受控 session 主 rusage −5.3359% 落在旧 8 对完整纪律的 −6.70% 与旧 10 对 perf stat 的 user+sys −1.42% 之间，低于旧 36 对的 −11.97%；instructions −5.5528% 接近旧 8 对的 −6.34%，比旧 10 对的 −2.42% 更负；task-clock −5.3220% 介于旧 8 对的 −6.48% 与旧 10 对的 −1.71% 之间；与旧 4 对 sync-only 的 −8.6% task-clock 不同。负对照 −1.4396% `[-2.78,-0.08]` vs 旧 36 对负对照 −2.06% `[-3.14,-0.99]`；A/A +0.6512% vs 旧 36 对 A/A +0.19% `[-0.68,+1.04]`。跨 session 的量级差异存在，本 session 没有实验能分离其原因（宿主频率下探、perf 开销、缓存/写回状态等），故不作因果断言。

### D.6 机制计数：可复现的 read 减少与不可复现的 futex

**结论先行**：futex 的"减半"在**五个来源**中给出约 −49%、−33%、−10%~−14%、+7% 与 ≈1.00 的不同结果，**没有一个能独立定标**，**不得**作为机制证据；可复现的机制证据是"每次 drain 少一次 read"。

| futex 会话/探针 | A | B | B/A |
|---|---|---|---|
| 工作流 profiler（r1 引用，bpftrace kprobe） | 397,620 / 396,489 | 202,784 / 200,481 | ≈0.51（"减半"） |
| `reconciliation/README.md` 实验 4（bpftrace，本目录未存原始输出） | 366,881 | 392,827 | 1.07（B 更高） |
| 本轮补测 bpftrace（`results/recon-futex-counts.tsv`，A,B,B,A，非计时样本） | 398,812 / 409,933 | 341,883 / 369,693 | 0.857 / 0.902（约 −10%~−14%） |
| 归档 strace（`results/mech-counters.tsv`，3 次/臂） | 718,899 / 720,817 / 720,797 | 720,138 / 720,261 / 720,139 | ≈1.00 |
| 5+5 perf tracepoint（`raw-r2/ab/trace/syscalls.txt`，A/B 各 5 run；r5 补入） | 412,811.4（3.149/请求） | 275,787.4（2.104/请求） | 0.668（−33%） |

五个来源给出的幅度互不一致（约 −49%、−33%、+7%、−10%~−14%、≈1.00），方向也不全同；strace 探针几乎为零差异。可能原因包括 bpftrace 丢事件、kprobe 与 tracepoint 覆盖不同、strace 序列化线程改变争用；本轮没有做带返回值的联合测量来定标。无论原因如何，**futex 计数不能作为本改动的机制证据**，也不能支撑"唤醒路径成本转移"的因果表述（§D.7）。

| read 探针（可复现） | A | B | 说明 |
|---|---|---|---|
| `reconciliation/README.md` 实验 4 `read_all`（每任务） | 226,025 | 129,670 | 每任务少约 96k 次 read；`read_eagain` 112,979 → 114。**README 自述该目录未存原始输出，此行的绝对数不可独立核验** |
| 本轮 `read_all`（`recon-futex-counts.tsv`，已归档） | 260,315 / 258,014 | 127,087 / 128,228 | `read_eagain` 130,043 / 128,901 → **8 / 8** |
| 归档 strace `eventfd_read`（3 次/臂中位数） | 262,135 | 131,073 | −50.0%；`eventfd_write` 131,068 → 131,073 |

机制特征是：非空 drain 由 2 次 read（1 次成功 + 1 次必然 `EAGAIN`）变成 1 次；`read_eagain` 从约 13 万降到个位数，与该删除精确对应。各会话的绝对计数不同（strace 与 bpftrace 覆盖不同），但**方向在所有会话一致**，这是机制裁决继续成立的基础。**可独立核验的数值以归档的 `260,315/258,014 → 127,087/128,228`（`read_all`）与 `262,135 → 131,073`（strace `eventfd_read`）为准**；README 实验 4 的 226,025→129,670 只作为方向性旁证，不作为数字依据。

### D.7 机制符号与 B profile

B profile 使用 5+5 次 perf record 聚合（A/B 各自 `-g -fno-omit-frame-pointer` 副本；不用于计时）：

| 符号 | A self% | B self% | 变化 |
|---|---|---|---|
| `entry_SYSRETQ_unsafe_stack` | 15.15 | 14.03 | −1.12 pp |
| `entry_SYSCALL_64` | 4.55 | 4.22 | −0.33 pp |
| `syscall_return_via_sysret` | 4.02 | 3.76 | −0.27 pp |
| `ksys_read` | 0.38 | 0.15 | −0.23 pp |
| `vfs_read` | 0.45 | 0.25 | −0.20 pp |
| `eventfd_read` | 0.17 | 0.10 | −0.07 pp |
| `finish_task_switch.isra.0` | 0.57 | 0.30 | −0.27 pp |
| `psi_group_change` | 1.14 | 0.97 | −0.17 pp |
| `ThreadPoolBackend::worker_loop()` | 1.09 | 0.87 | −0.22 pp |
| `run_pipelined_copy_with_context` | 0.49 | 0.17 | −0.32 pp |
| `__x64_sys_poll`（栈归属） | 10.85 | 9.88 | −0.97 pp；poll 调用 0.877 → 0.920/请求（+5.0%） |

**符号位移（必须如实记录，不作因果结论）**：
- `native_queued_spin_lock_slowpath`（`try_to_wake_up ← wake_up_q ← futex_wake` 的 rq 锁慢路径）从 **0.54% 升到 1.92%**（+1.38 pp；绝对样本均值 53.2 → 181.6/run，5/5 run 一致），按 B 总 cycles 估算约 +1.3% cycles。这是一次**测得的符号位移**，但它的因果归因（park 比例、真实 `wake_up_q` 入队）没有 `perf sched`/wakeup tracepoint 支持，且 futex 计数本身不可复现（§D.6），因此只记录为未解释的符号变化，不再作为"成本转移"的因果结论。无论该偏移是否为真，它都已包含在本轮实测 cycles 净值（−5.9915%）之内。
- `__x64_sys_futex` 栈归属 15.43% → 17.00%，只作为**份额变化**记录。futex 系统调用数在五个来源中幅度不一致（§D.6），因此不能据此推断唤醒次数增加或减少；"futex 减半"已从机制证据中撤销。perf tracepoint 均值 3.149 → 2.104/请求 与 `reconciliation/README.md` 实验 4 中 B 更高相矛盾，两者都不足以定标。
- `pthread_mutex_lock/unlock` 调用数 A 3,932,596 vs B 3,932,536（bpftrace，相同）；占比 9.58% → 10.53% 是分母变小造成的份额变化，绝对采样约 4503 → 4572（+1.5%），**不是新增锁工作**。
- 线程间无显著转移：A main/worker 50.47%/49.53%，B 50.72%/49.28%。

### D.8 Profile 解释

BASE 的真实机器成本以"协调"为主：系统调用边界 26.93%、futex 内核体 15.44%、glibc mutex 12.74%、eventfd 通知（write 4.81% + read 2.28%）、poll 10.56%；真实载荷单项为 `pwrite64` 12.46%（其中 data 6.07%）、`pread64` 3.62%（data 2.78%）、`rep_movs` 2.46%（`rep_movs` 是 pwrite/pread 栈内的 leaf，不能再与总路径相加）。这些百分比来自不同测量基准，**不能相加**；它们是成本地图，不是单因素因果证明。本轮 A/B 与 B profile 才是该候选的证据。

### D.9 未观测成本（明确列出）

- EAGAIN read 与 successful read 各自的精确私有 cycles：perf 采样看不到返回值，只能用 read 路径总量的一半作为上界，未做 A/B。
- 正常运行时每次 `pthread_cond_signal` 是否真正唤醒 waiter：精确 futex opcode/返回值只在 strace 扰动下取得；正常运行 `cond_wait` 0.507/请求与 `cond_signal` 1.000/请求只能说明约一半 signal 可能没有 parked waiter。
- 每次请求的 off-CPU 时长、唤醒延迟与尾部分布：未运行 `perf sched` / wakeup tracepoint / 直方图。
- 单个 mutex 获取的精确 cycles 与 cache-line bouncing：只有调用点样本占比，未做 `perf c2c`。
- io_uring 后端在本 workload 下的任何成本：未执行 uring 路径。
- 三个候选的实际收益：本轮只对 R1 做了实现与 A/B；R2/R3 未实现、无 mutant、无 A/B。
- 冷 page cache 行为：所有运行都在 warm cache。
- 分配次数/内存占用：没有 malloc 跟踪；稳态看似无动态分配，未证明。
- 频率对 cycles 的影响：performance governor 下 idle `cur==max==3.5 GHz`，负载中实测 3.25–3.35 GHz（intel_cpufreq，硬件 turbo/功耗限制），故以 cycles 为主计数。
- 应用层串行 write await 的可删机器工作比例：只能测到 1.028 ctx switch/请求、poll 0.921/请求、约 25.6% 的 2-CPU 容量空闲；没有把应用改成批量提交后的 A/B。
- **静默纪律的作用机制**：没有做"控制 Dirty/Writeback 水平"的剂量实验；"缺少静默纪律导致 perf stat 只看到约 −2%"只是 `README.md:6-7` 与 `priorCrossCheck` 的断言，且交叉检查基线（1476.00 ms）低于另两个 session，与稀释方向相反，故成因**未确定**（§D.5.4）。
- **跨 session 量级差异（−1.4%~−12%）**：未做 no-op 重建/二进制布局对照，也未在同一受控会话重跑 36 对；该跨度未分解为会话、负载、布局或样本量（§D.5.4）。
- **10 对交叉检查的 protocol**：`raw-r2/ab/stat/` 只有各 run 的 perf 输出，没有 sync/Writeback 轮询、warmup 数、A/B 顺序与环境快照，**无法重建该 session 的协议**；其每对 A→B 的固定顺序本身构成顺序/漂移混杂。
- **预注册门槛原件**：`minMeaningfulEffectPct=1`、`aaMaxAbsMeanPct=1.5`、`aaMaxCiWidthPct=3`、`p99BudgetPct=3` 只出现在结构化 workflow 记录与本报告中；本会话在证据根内、排除 `REPORT.md` 与 `wt-*` 后 grep 无归档原件，`results/*.meta.json` 的 `protocol` 也无这些键。
- **8 对会话的同 session A/A**：没有；A/A 校准来自 36 对 `aa2-4k-d4` 会话，属跨 session 零效应带（§D.5.3）。
- **futex 的真实值**：五个来源幅度不一致（§D.6），本轮没有用"kprobe + 返回值 + tracepoint"联合定标，故 futex 成本变化未定量。
- **perf stat 自身开销**：wrapper `RUSAGE_CHILDREN` −6.5453% 与工作负载 rusage −6.7022% 之间的差（约 0.16 pp）未分解到 sudo/perf 的固定开销。
- **p99 预算的判据定义**：`p99BudgetPct=3` 只有字段名，没有预注册定义（CPU 还是 wall、单侧还是双侧、绝对还是相对）与分析器实现；本轮只能按 CPU 口径报告"未超"（§D.10）。
- **受控 session 未采集系统调用计数**：为避免扰动计时，`session-r2r1` 没有运行 bpftrace/strace；其 `read_ops`/`write_ops` 只是程序计数器。因此"每 drain 少一次 read"在本 session 没有被直接计数，只能由语义与旧 session 的 bpftrace/strace 证据支持（§D.6、§D.5.5）。
- **受控 session 的 A/A 噪声来源**：A/A 12 对宽度 3.8789 > 3.0；逐对 sd 从旧 36 对的 2.650% 升到 3.5908%。未做追加 A/A 对数、更安静环境或按 CPU 逐个绑定的复测，样本量/宿主噪声/频率下探的贡献未分解。
- **跨 session spread 的成因**：受控 session 把 A/A、主场景、负对照放进同一 session，但没有做频率/功耗/缓存状态的剂量实验，也没有 no-op 重建或二进制布局对照；−1.4%~−12% 的跨度仍未分解。
- **受控 session 的 `sudo`/`perf` 固定开销**：同 session 内 `perf_cpu_s`（工作负载 user+sys）与 wrapper `RUSAGE_CHILDREN` 的差约 0.14 pp（−5.4768% vs −5.3359%），未进一步分解到 `sudo`/`perf`。
- **受控 session 的 wall 尾部**：21 次停顿只按预注册规则用于 wall 尾部剔除；没有做 writeback 剂量实验，也没有证明停顿与 btrfs 回写的因果关系。
- **受控 session 的 governor 持续性**：运行中 sysfs 采样频率下探（cpu2 最低 1,174,069 kHz、cpu3 最低 1,170,505 kHz），无法声称测量期间持续 `cur==max`；该偏差对 cycles/instructions 的影响未量化。

### D.10 p95 / p99 与 wall

- **旧 session** A/B 主场景 CPU p95/p99：A `1.5885/1.5958`，B `1.3987/1.4059`；旧负对照 p95/p99：A `0.2694/0.2774`，B `0.2688/0.2740`；旧 A/A p99 `1.6114/1.6125`。这些都是 CPU 值。**按 CPU 口径，旧 session 的 p99 未超 `p99BudgetPct=3`**：两个 cell 中 B 的 CPU p99 都低于 A，A/A 的 p99 两臂几乎相同（1.6114/1.6125）。但 `p99BudgetPct=3` 只有结构化 `frozenProtocol` 的字段名，工作区没有保存预注册定义（CPU 还是 wall、单侧/双侧、绝对还是相对）或分析器实现，因此该结论限定在 CPU 口径与旧 session，定义本身仍是 §F.5 gate 4 的回填项。负对照 raw wall P99 为 A 1.9700 s → B 2.4803 s（若按 wall 读则超过 3%），但 wall 被 writeback 停顿主导，不能作为回归判据。
- wall 的 p95/p99 被 btrfs writeback 停顿主导：主场景未剔除原始 wall P95/P99 A `[25.3090,41.8830]` s、B `[19.5605,25.2210]` s；剔除 environment_stall 后 A `[1.7547,2.8056]`、B `[1.6398,1.6530]`。wall mean 的 +97.4% 与 CI 宽度使其不能作为收益或回归判据；clean-pair 的 −6.474% 只作参考。
- 受控 session（§D.5.5）的四口径分位数显示 B 的 p50/p95/p99 全部低于 A（instructions/cycles/task-clock/rusage；例如 rusage p99 A 1.8165 s → B 1.7458 s）。受控 session 的分析器没有实现 `p99BudgetPct` 判定，因此本轮**不**据此声称 p99 预算通过；该门禁仍按 §F.5 gate 4 处理。wall 尾部（剔除后/原始）见 §D.5.5，不作判据。
- 结论：**本轮没有建立 wall-time 改善**；任何产品声明不得引用 wall。

---

## E. Final Decision

### E.1 `MECHANISM_VERDICT = INCONCLUSIVE`

- **为什么不是 GO**：删除的是 ThreadPool 配置下每请求约 0.99 次（EAGAIN 130,152/run = 0.993/请求）、必然 `EAGAIN` 的探测 read；语义 oracle 在纯净 BASE 与 B 均 9/9（r1 会话实际运行），mutant 被 3 条语义 re-arm 断言杀死（r1 会话 exit 1；审计复跑一致但输出未归档），机制测试在 B 上 4/4，归档机制计数显示 `eventfd_read` 262,135 → 131,073（−50.0%）、bpftrace `@read_eagain` 118,115/124,547 → 12/7、perf tracepoint `sys_enter_read` 1.990 → 0.987 次/请求——这些**未被推翻**。但本候选唯一的同 session 受控测量 `confirmed=false`（A/A 宽度 3.8789 > 3.0，§D.5.5），且四配置矩阵/套件/sanitizer/mutant 的**拆分后归档回执仍缺**（现有 `evidence-r2/*` 为 16:05 旧目标，§C.6、§C.7）。按本报告统一的判定口径，"机制 GO"需要语义/计数证据与受控测量同时成立；第二项未成立，故取 `INCONCLUSIVE`，不声称 GO。
- **适用范围**：仅 ThreadPool 后端、`EFD_NONBLOCK|EFD_CLOEXEC` 非 semaphore eventfd、所有用户态写者持 `mtx_` 的配置。io_uring 的"成功 read 与清 pending 之间内核重新 signal"窗口无确定性注入；其安全性依赖 `PhysicalProbe` 与既有 race 测试，不是本轮直接证明。
- **限制**：生产头文件含受 `SLUICE_ASYNC_INTERNAL_TESTING` 保护的 test-only seam；`EINTR` 仅由 test-only 注入模拟，没有真实内核 EINTR；BASE 语义 oracle 只在 release/liburing=y 跑过；BASE 的 debug 与 liburing=n 配置未跑；机制测试依赖新增 seam，不能在纯净 BASE 头文件上编译。
- **对账后的补充**：机制证据只采用"每次 drain 少一次 read"（§D.6）；futex 减半撤销。受控 session 的四个端到端口径**均值**同向（§D.5.5），但不改变机制证据的缺口；它本身也没有直接计数 read 次数（未跑 bpftrace/strace）。
- **升级条件**：在同一 session 内追加 A/A 对数或改用更安静环境，使 A/A 宽度 ≤3.0 且主场景四口径仍一致为负，同时补齐拆分后的四配置/套件/sanitizer/mutant 归档回执；此时机制裁决可回到 GO。

### E.2 `END_TO_END_VERDICT = NOT_ESTABLISHED`

**定义**：端到端收益未被建立——四组均值与 95% CI 上界同向为负，但逐对符号不一致（正号 1/3/4/4）、A/A 安静度门失败、跨 session spread 未解释。它既不是"无效应"，也不是"已确认收益"。

- **被撤回的 r2 结论（继续有效）**：(a)"工作流 perf stat 交叉检查未执行纪律、其 −2% 是环境假象"——该 session 自身四口径一致（perf 打印 user+sys −1.416%、task-clock −1.71%、instructions −2.42%、cycles −2.10%），不存在仪器冲突；(b)"约 −2% 被排除"——8 对会话自身的 CI 上界为 −4.9~−5.2，但 10 对 session 直接测得 −1.4%~−2.4%，受控 session 又测得 −5.3%，跨 session 差异未解释；(c)"≥6% 量级"——cycles 点估计为 −5.9915%、CI 下界 −4.9090%，不是下界。
- **受控 session 的新证据（r4/r5）**：同一 session、同一纪律、AB/BA 交错、per-run `perf stat` + `rusage`；主场景 36 对**均值与 CI 上界**为负（instructions −5.5528%、cycles −4.7712%、task-clock −5.3220%、rusage −5.3359%，CI 上界全部 <0），但**逐对并非全负**（instructions 1/36、cycles 3/36、task-clock 4/36、rusage 4/36 对为正，§D.5.5）；A/A 12 对 +0.6512% 但宽度 3.8789 > 3.0，`confirmed=false`。因此**方向只在均值/CI 层面被支持，逐对不稳定，量级没有被确认**。
- **跨 session 量级仍然不一致**：36 对 rusage **−11.968%**；10 对 perf stat −2.42%~−1.416%；4 对 sync-only −8.79%~−7.71%；8 对完整纪律 −6.7022%~−5.9915%；受控 session −5.5528%~−4.7712%。跨度约 −1.4%~−12%，受控 session 未分离其原因，也不与旧 36 对的 −11.968% 对齐。
- **若最终按 GO 处理，唯一可用的量级**：只能采用受控 session **四组口径中绝对值最小者**——`min(|−5.5528%|, |−4.7712%|, |−5.3220%|, |−5.3359%|) = 4.77%`（cycles）。5.34% 只是 rusage 与 instructions 两者取小，**不是四组口径的保守量级**；task-clock 的 5.3220% 大于 cycles 的 4.7712%，因此不改变最小值的选择。旧 session 的约 12% 是一个**未被受控 session 复现的大幅点估计**，不是统计上界，也不是产品声明；在 A/A 宽度门关闭前，4.77% 也只能作为"待确认的保守量级"。
- **已建立**：删除方向在所有会话的**均值**为负；oracle 等价（逐字节 `cmp` + 冻结 `read_ops/write_ops`）；每次 drain 少一次 read（旧 session 归档计数）；负对照不是零效应（受控 session rusage CI 上界 −0.0811 < 0）。
- **未建立**：可归因的效应量级（A/A 宽度门失败、逐对符号不一致）；跨 session spread 的成因；10 对交叉检查的合规协议；预注册门槛的独立存档；拆分后归档回执；B.2 复算；p99 定义。
- **不覆盖**：wall、能效、跨后端（io_uring 未执行）、多 worker、cold cache。

### E.3 `PRODUCT_READINESS = NOT_READY`

**定义**：不满足进入合并评审的证据门槛；可以继续作为研究/草稿分支讨论，**不能提交为可合并的 product PR**。

阻断项（对应 §F.5）：

1. **受控 session 的 A/A 安静度门未过**：12 对 rusage +0.6512% `[-1.2814,+2.5974]`，宽度 3.8789 > 3.0，四个口径宽度全部 >3.0，`confirmed=false`；同 session 零效应带不成立（§D.5.5）。
2. 跨 session 量级差异（−1.4%~−12%）未解释，受控 session 未分离其原因，也未复现旧 36 对的 −11.968%（§E.2、§D.5.4）。
3. 10 对 perf stat 交叉检查无 protocol 记录，且为 A→B 顺序而非 AB/BA；既不能作为反证，也不能被"环境假象"解释掉。
4. 预注册门槛（`minMeaningfulEffectPct=1`、`aaMaxAbsMeanPct=1.5`、`aaMaxCiWidthPct=3`、`p99BudgetPct=3`）在证据根内无独立归档原件。受控 session 的分析器 `session-r2r1/analyze_session.py`（mtime 18:51，早于 session 开始 18:52:27）在 `:262-272` 内嵌了 12/1.5/3.0/36/≤−1.0 的判定，这是目前最强的"先于测量"证据，但它是一次性脚本、无独立存档或评审，`p99BudgetPct` 仍未实现。
5. 同 session A/A：受控 session 已具备（12 对），但未达宽度门；旧 8 对会话仍无同 session A/A，其零效应带仍跨 session。
6. 拆分后的四配置矩阵、13 套件、语义目标 TSAN/ASAN 与 mutant 回执均缺失：现有 `evidence-r2/*` 为 16:05 旧目标；归档 drain mutant 是 4 条 mechanism 命名 FAIL，当前构建的 3 条语义 FAIL 未归档。
7. 无 product PR、无 ledger 行与 PROG-02/C1 等价论证；生产头文件含 test-only seam（release 预处理 0 命中，仍需按 AGENTS.md 评审）。
8. B.2 步骤台账不可精确复算；`p99BudgetPct=3` 无预注册定义；io_uring 内核重 signal 窗口无确定性注入。

**缺失证据清单（审计逐项，阻断）**

| # | 缺失证据 | 现状 / 路径 | 对应 gate |
|---|---|---|---|
| 1 | 拆分后 `a3a14a31` 的四配置矩阵、13 套件、语义目标 TSAN/ASAN 与 drain mutant 正式回执 | `evidence-r2/config-matrix/*.log`、`evidence-r2/sanitizers/*` mtime 16:05，目标名仍为 `progress_source_drain_oracle_test`；语义目标 tsan/asan 日志 0 字节；归档 mutant 为拆分前 4 条 mechanism 命名 FAIL | F.5-3 |
| 2 | "13 个既有套件 exit 0"的可检路径 | `evidence-r2/suites/` 为 16:05、拆分前目标；拆分后套件日志未保存（§C.7） | F.5-3 |
| 3 | 预注册门槛独立归档 | `minMeaningfulEffectPct`、`aaMaxAbsMeanPct`、`aaMaxCiWidthPct`、`p99BudgetPct` 仅见于结构化记录与 `session-r2r1/analyze_session.py:262-272` 的内嵌值（mtime 18:51，早于 session 开始 18:52:27）；p99 判定未实现 | F.5-4 |
| 4 | 10 对 perf stat 交叉检查的 protocol | `raw-r2/ab/stat/` 只有 `stat.txt`、`stat2.txt` 与 `A1–A5/B1–B5.stdout`，无 sync/Writeback 轮询、warmup 数、顺序或环境快照；每对固定 A→B | F.5-2 |
| 5 | 8 对完整纪律 session 的同 session A/A | 无；零效应带来自另一 36 对 `aa2-4k-d4` session；受控 session 的 12 对 A/A 宽度 3.8789 > 3.0，不能作校准 | F.5-1/5 |
| 6 | B.2 台账按 caller 汇总的精确复算 | 报告自述四处不一致（`poll_progress` 2.00 vs 1.49、`publish_one` 0.43 vs 0.23、`wait_one` 31.63 vs 32.2、`run_syscall` 19.95 vs 19.75） | F.5-7 |
| 7 | io_uring"成功 read 与清 pending 之间内核重新 signal"的确定性注入 | 无注入 seam；仅靠 `PhysicalProbe` 与既有 race 测试（§C.4） | F.5-9 |
| 8 | 受控 session 的 futex/read 系统调用计数 | 未运行 bpftrace/strace（§D.5.5）；per-drain read 只能引用旧 session | 记录 |
| 9 | BASE 机制测试"2 reads/1 EAGAIN"正式回执 | `wt-base-verify` 机制二进制 exit 1 已复跑，但输出只列失败断言，不含确切计数（§C.5） | F.5-6 |

已具备但不足以抵消上述阻断：语义 oracle 在纯净 BASE 与 B 上 9/9、race 回归 22/22、受控 session 120/120 oracle 通过、四组均值与 CI 上界同向为负；但端到端收益未被建立（`NOT_ESTABLISHED`），机制裁决为 `INCONCLUSIVE`。

### E.4 裁决与限制矩阵

| 维度 | 结论 | 不覆盖 |
|---|---|---|
| 机制正确性 | **INCONCLUSIVE** | uring 内核重 signal 窗口、真实 EINTR、未来非 `mtx_` 写者；受控 A/A 宽度门未过；拆分后归档回执待补 |
| 端到端量级 | **NOT_ESTABLISHED** | 均值/CI 为负但逐对正号 1/3/4/4、A/A 宽度门失败、四组最小绝对值为 cycles 4.77%；wall、能效、跨后端、多 worker、cold cache；跨 session spread 未解释 |
| 产品准入 | **NOT_READY** | 未 merge；A/A 宽度/跨 session 差异/protocol/预注册/收据/B.2/p99/io_uring 均未关闭 |
| 历史机制 | #634/#618/#600 MERGED；#595/#617 PAUSED；#632/#622/#604 RESEARCH；K1/#579/#582/R2/R3/offer fusion REJECTED | 不得把历史成本地图当作本轮 A/B |

---

## F. Product Handoff

### F.1 Minimal diff

| 文件 | 变化 |
|---|---|
| `include/sluice/async/detail/progress_source.hpp` | +47/−3（生产：成功 read 后清 `notification_pending_` 并 return；测试 seam 与注释） |
| `tests/progress_source_drain_semantic_oracle_test.cpp` | 新增 539 行，9 个语义用例 |
| `tests/progress_source_drain_mechanism_test.cpp` | 新增 129 行，4 个机制指标用例 |
| `xmake/tests.lua` | +45 行，注册语义目标与 mutant；机制目标不注册为测试 |

分支 `research/621-r2-R1-drain-eagain-read`；提交 `fd3eb932`（机制）、`839a0164`（docs，后被还原）、`a3a14a31`（拆分语义/机制）。最终树 diff 哈希 `ee7c4c3e…` 与 `b-round2.diff` 一致（r1 会话复核；r3 未重跑）。

### F.2 测试命令

```sh
cd /home/jnhu/sluice-621-r2/wt-B
xmake f -m release -y --liburing=y
xmake build progress_source_drain_semantic_oracle_test
./build/linux/x86_64/release/progress_source_drain_semantic_oracle_test        # exit 0, 9 cases
xmake build progress_source_drain_mechanism_test
./build/linux/x86_64/release/progress_source_drain_mechanism_test             # exit 0, 4 cases
xmake build progress_source_mut_drain_success_keeps_pending
./build/linux/x86_64/release/progress_source_mut_drain_success_keeps_pending  # exit 1, 3 semantic FAILs
xmake build threadpool_progress_race_test
./build/linux/x86_64/release/threadpool_progress_race_test                    # exit 0, 22/22
```

四配置矩阵：`xmake test -y -j 20 --output-on-failure`（release/debug × liburing y/n），期望 `81/81`、`81/81`、`60/60`、`60/60`。**该命令本轮撰写未重跑**；结构化证据记录为已运行，工作区收据存在拆分前的时间戳缺口。

跨计数口径对账的复现命令（`reconciliation/recon-provenance.txt:15-50`）：

```sh
cd /home/jnhu/sluice-621-r2
python3 -I reconciliation/runner_perf_recon.py \
  --binary-a wt-A-timing/build/linux/x86_64/release/sluice-copy \
  --binary-b wt-B-timing/build/linux/x86_64/release/sluice-copy \
  --src data/src-256m.bin --dst-a data/recon-perf-dst-A.bin --dst-b data/recon-perf-dst-B.bin \
  --pairs 8 --warmups 2 --buffer-size 4096 --pipeline-depth 4 --workers 1 --sync-policy none \
  --tsv results/recon-perf-4k-d4.tsv --meta results/recon-perf-4k-d4.meta.json
python3 reconciliation/analyze_perf_recon.py --tsv results/recon-perf-4k-d4.tsv \
  --meta results/recon-perf-4k-d4.meta.json --out-prefix results/recon-perf-4k-d4.analysis
python3 -I reconciliation/probe_futex_recon.py \
  --binary-a wt-A-timing/build/linux/x86_64/release/sluice-copy \
  --binary-b wt-B-timing/build/linux/x86_64/release/sluice-copy \
  --src data/src-256m.bin --dst-a data/recon-perf-dst-A.bin --dst-b data/recon-perf-dst-B.bin \
  --script raw-r2/tools/ab_syscount.bt --tsv results/recon-futex-counts.tsv \
  --meta results/recon-futex-counts.meta.json
```

脚本哈希见 `recon-provenance.txt:74-77`（`runner_perf_recon.py` `5ecccf9d…`、`analyze_perf_recon.py` `1af02cda…`、`probe_futex_recon.py` `a000262e…`）。

**受控 session（session-r2r1）的复现命令**（一次性驱动，未提交仓库；见 `session-r2r1/PROVENANCE.txt`；重跑会覆盖 `results/session-r2r1*` 与 dst）：

```sh
cd /home/jnhu/sluice-621-r2
python3 -I session-r2r1/runner_session.py \
  --src /home/jnhu/sluice-621-r2/data/src-256m.bin \
  --binary-a wt-A-timing/build/linux/x86_64/release/sluice-copy \
  --binary-b wt-B-timing/build/linux/x86_64/release/sluice-copy \
  --results-dir results --data-dir data \
  --session-meta results/session-r2r1.meta.json --label-prefix session-r2r1
python3 session-r2r1/analyze_session.py --results-dir results --prefix session-r2r1 \
  --session-meta results/session-r2r1.meta.json \
  --out results/session-r2r1.analysis.json --txt results/session-r2r1.analysis.txt
```

runner/analyzer 的 sha256 见 `session-r2r1/PROVENANCE.txt`（`runner_session.py` `1463401d…`、`analyze_session.py` `84675a45…`）。

### F.3 Evidence manifest

`/home/jnhu/sluice-621-r2/MANIFEST.sha256` 覆盖 `b-round2.diff`、`results/`、`raw-r2/`（timing/perfstat/record/strace/ab/tools）、`evidence-r2/`（mutant/sanitizer/config-matrix/suites 收据）、`data/src-256m.bin`、`data/src-8m.bin`、计时/符号二进制、runner/analyzer/probe 脚本与 `REPORT.md`；r2 追加了 `reconciliation/` 的 8 个文件与 `results/recon-perf-4k-d4.*`、`results/recon-futex-counts.*`；**r4 追加受控 session 的 13 个文件**：`results/session-r2r1-{aa-4k-d4,main-4k-d4,neg-64k-d1}.{tsv,meta.json}`、`results/session-r2r1.meta.json`、`results/session-r2r1.analysis.{json,txt}`、`session-r2r1/{runner_session.py,analyze_session.py,PROVENANCE.txt,run.log}`。排除 `wt-*`、`prof-build/`、`__pycache__/`、生成的 `dst*.bin`/`ab-*-dst*.bin`/mech probe 输出。root-owned 0600 的 perf `.data` 文件不可读，未直接哈希；其可读文本派生（`rep*.dso.txt`、`rep*.sym.txt`、`script*.txt`、`agg-*.tsv`）已纳入，清单头逐项列出。校验命令：

```sh
cd /home/jnhu/sluice-621-r2
sha256sum -c MANIFEST.sha256
```

r4 把 `REPORT.md` 行更新为修订后的哈希并追加上述 13 个文件后，执行 `sha256sum -c MANIFEST.sha256` 得到 **384/384 OK，exit 0**（371 + 13）。该检查只覆盖清单项，`/tmp/sluice-drain-probe/` 下的未归档探针与 trace 不在覆盖范围，本报告不依赖它们。**r1 记录的 357/357 无法复验**：`MANIFEST-round1.sha256` 只有 **131** 条哈希行；371 条 = r1 声称的 357 条 + r2 追加的 14 条，这只是算术一致，不是独立核验。`REPORT.md` 同时写入两个位置，内容逐字节一致（附录 C.3）。

### F.4 Owning issue 与 product PR

- **Owning issue**：`#621`（ThreadPool / progress 机制族），应用路径授权归 `#604`。
- **Product PR**：**尚未创建**。分支仅本地存在；未 push、未创建 PR、未 merge（`gh pr list --head research/621-r2-R1-drain-eagain-read` 返回 `[]`，r1 会话执行；r4 未重跑）。在 `PRODUCT_READINESS = NOT_READY` 与 `END_TO_END_VERDICT = NOT_ESTABLISHED` 下不创建 product PR。
- **Product PR URL**：**待发布阶段回填**（当前无 PR；发布阶段以本报告 §F.1–§F.3 与 §F.5 关闭记录为附件）。
- **ledger**：本轮无 docs 变更；产品合并前必须按 GOV-02 增加 PROG-02 / PROG-01 相关行与 dated 621 记录，并引用 `MANIFEST.sha256`。

### F.5 Remaining merge gates

1. **受控 session 的 A/A 安静度与量级确认（阻断）**：受控 session 已执行（§D.5.5），但 A/A 12 对宽度 **3.8789 > 3.0**，`confirmed=false`；主场景四组虽在均值/CI 上为负，逐对正号为 instructions 1/36、cycles 3/36、task-clock 4/36、rusage 4/36。需在同一 session 内追加 A/A 对数（旧 session 用 36 对，宽度 1.723%）或改用更安静环境，使宽度 ≤3.0，然后重跑 36 对主场景；并分解 A/A 噪声（受控 12 对逐对 sd 3.5908% vs 旧 36 对 2.650%）。跨 session spread（−1.4%~−12%）仍未解释；关闭前不得引用任何单一百分比作为收益，§E.2 的 4.77%（四组最小绝对值，cycles）也只是待确认的保守量级。
2. **10 对交叉检查的 protocol（阻断）**：补交该 session 的原始记录（每对前是否 `os.sync`、是否轮询 `Writeback==0`/`Dirty<=4096 kB`、warmup 数、A/B 顺序、环境快照）；若无法补交，在证据记录中明确标注"协议不可核验"，并说明其 A→B 顺序造成的顺序/漂移混杂。不得再把 −2% 归因为"缺少纪律"。
3. **归档收据（阻断）**：在拆分后的 `a3a14a31` 上重跑四配置矩阵、13 套件、语义目标 TSAN/ASAN 与 drain mutant，保存非空日志与 exit status；现有 `evidence-r2/*` 为 16:05 旧目标（含 mutant 的 4 条 mechanism 命名 FAIL），当前构建的 3 条语义 FAIL 未归档。
4. **预注册门槛与 p99（阻断）**：把 `minMeaningfulEffectPct=1`、`aaMaxAbsMeanPct=1.5`、`aaMaxCiWidthPct=3`、`p99BudgetPct=3` 的原始记录独立归档到证据根（当前最强证据是受控 session 分析器 `session-r2r1/analyze_session.py:262-272` 的内嵌阈值，mtime 18:51 早于 session 开始 18:52:27，但无独立存档/评审），并补 p99 定义（CPU/wall、单侧/双侧、绝对/相对）与分析器实现。
5. **同 session A/A（部分关闭）**：受控 session 已有同 session A/A（12 对），但未达宽度门（gate 1）；旧 8 对会话仍无同 session A/A，其零效应带仍来自 36 对会话（跨 session）。保留 `frozen_ops_source` 为 pair 1 arm B 的披露（首个有效 A 运行数值一致）。
6. **BASE-GREEN 归档**：保存纯净 BASE 的语义 oracle 9/9 与 BASE drain 体机制测试红（2 reads/1 EAGAIN）的正式回执；当前机制 BASE 红是在 BASE drain 体加 observation-only seam 后取得。
7. **B.2 台账复算（阻断）**：按 `raw-r2/record/agg-attrib.tsv` 的 caller 汇总重算 B.2 的步骤占比，或把 B.2 降级为定性索引（当前 `poll_progress` 2.00 vs 1.49、`publish_one` 0.43 vs 0.23、`wait_one` 31.63 vs 32.2、`run_syscall` 19.95 vs 19.75 四处不一致）。
8. **负对照与 #634（记录）**：受控 session 负对照 rusage −1.4396% `[-2.7800,-0.0811]`、每请求节省 0.5880 µs/请求；与 #634 同 cell 的 +0.10% 点估计方向相反、CI 重叠，原因仍未解释。`native_queued_spin_lock_slowpath` 的 park 归因已撤回；futex 计数不可复现，不得进入 PR 描述。
9. **io_uring 窗口**：为"成功 read 与清 pending 之间内核重新 signal"增加确定性注入，或明确接受 `PhysicalProbe` 兜底并记录为未证明项。
10. **生产门禁**：生产 seam/注释审计（`SLUICE_ASYNC_INTERNAL_TESTING`）、`docs/roadmap/v2-conformance.md` 的 PROG-02/C1 等价论证与 GOV-02 ledger 行、product PR 创建与 URL 回填、authority-contraction audit（"无既有 mechanism 因本切片变为可删除"，不删除 settlement/publication 权威）、MANIFEST 最终校验；#595 仍 PAUSED。

**当前状态**：`MECHANISM_VERDICT = INCONCLUSIVE`、`END_TO_END_VERDICT = NOT_ESTABLISHED`、`PRODUCT_READINESS = NOT_READY` —— 1–4、6–7 全部未关闭，5 部分关闭；在它们关闭前，本候选不得提交为可合并的 product PR。

### F.6 Next candidate recommendation

本轮的 R2（0.90%）与 R3（0.45%）均低于 1% 最小有意义效应，且 R3 会删除 #395 的确定性证据面，**不建议**进入实现。profile rank 1（poll(-1) → 阻塞 read，约 5.0–5.5% cycles）是本轮唯一未评估的大候选，但它需要单独的有界等待/非阻塞 drain/dup fd 设计与 falsifier，不能从本轮直接推导；而且在受控 session 的 A/A 宽度门关闭前，任何新候选都无法被可信地量化。issue #621 历史评论提到的下一候选是 mutex family ~9–11%（`history.eliminations` β/γ 行引用的 issue comment），但本轮 R2/R3 已证明"按站点数乘均值"的估算不可靠；若推进，必须先有新的 measured hotspot 与单因素 A/B。

**建议顺序**：保持 `NOT_READY`，先关闭 §F.5 的 1–4 与 7（受控 A/A 安静度、10 对 protocol、归档收据、预注册门槛、B.2 复算），再讨论 product PR；期间不引用任何单一百分比作为收益（包括四组最小绝对值 4.77%）。在新的 profile 之前不开启新机制。

---

## 最终 7 问

### Q1 冗余工作是什么（函数、机器操作、频率、CPU 成本）

`ProgressSource::drain_notification_nolock_()`（BASE `include/sluice/async/detail/progress_source.hpp:397-419`）在成功 `read(notification_fd_, &value, 8)` 后 `continue`，循环再次执行 `read(2)`，该次必然返回 `EAGAIN`。机器操作：约 0.99 次/请求的 `read(2)`（EAGAIN 130,152/run ÷ 131,076 = 0.993 次/请求；0.989 是 `ksys_write` 五次均值 129,656/run 的比值，不是 read 频率）及其系统调用进入/返回 trampoline、8 字节输出写、`errno` 分支与循环回边。频率：`read` 总计 1.978 次/请求（259,316/run），其中 EAGAIN 0.993 次/请求（`raw-r2/perfstat/read_eagain.txt` 的 130,152/run，约 50%；结构化证据另记 129,605 的第二次运行未归档，不采用）。CPU 成本：read 栈归属 2.28% cycles；按 trampoline 26.93% ÷ 7.955 = 3.39%/syscall，机制**粗估** ≤4.53% cycles（3.39% 边界 + ≤1.14% read body）；该粗估不是上界，受控 session 的 cycles 点估计 −4.7712% 已超过它。

### Q2 为什么可证明不必要（语义与并发）

eventfd 以 `EFD_NONBLOCK | EFD_CLOEXEC` 创建（`progress_source.hpp:74-78`），**不是** `EFD_SEMAPHORE`；一次成功 read 原子取走整个计数器并清零。ThreadPool 配置下，所有用户态写者 `signal()`（`:252-253`）与 `interrupt()`（`:234-235`）都与 drain（`:153-166`、`:274-277`）同持 `mtx_`，因此成功 read 之后、同一临界区内，下一次 read 在用户态写者维度上只能 `EAGAIN`。第二次 read 的唯一作用是把 `notification_pending_` 清零；该清零可以直接在成功 read 返回后完成，语义等价。B 的机制测试为 4/4；BASE 的对照由结构化记录（BASE drain 体 + 观测计数器）给出 2 reads/1 EAGAIN，不是纯净 BASE 头文件的可编译结果。

### Q3 谁继续承担原责任

`ProgressSource`（同一 `mtx_` 域）继续是唯一 drain 权威：成功 read 清零计数器的同一临界区清 pending；写者（Backend worker 的 `signal`、Context 的 `interrupt`）继续承担"必须制造可读就绪"的唤醒义务；内核继续承担 eventfd 计数器语义；`EINTR` 重试保留；Context 继续拥有 fd，host 只借用注册；`PhysicalProbe` 继续为"被 drain 掉的内核通知"兜底。本切片不移动生产权威。

### Q4 真实机器工作减少了多少（指令、CPU、wall、其他线程）

**主证据：受控 session（session-r2r1，36 对，同 session A/A 12 对、负对照 12 对）**

- **指令**：3,401,429,034.5 → 3,210,923,730.0，**−5.5528%** `[-6.5241,-4.5699]`，36 对 CI 上界 <0；每请求 25,950.05 → 24,496.66 条（均值之比 −5.6007%）。每臂 p50/p95/p99：A 3,397,361,214/3,523,081,080/3,543,649,028，B 3,195,531,159/3,348,358,374/3,381,347,148。
- **cycles**：4,694,279,431.5 → 4,467,941,159.7，**−4.7712%** `[-5.8198,-3.7236]`，36 对 CI 上界 <0。p50/p95/p99：A 4,677,097,528/4,872,891,398/4,962,205,123，B 4,443,155,044/4,695,369,781/4,773,653,368。
- **CPU（rusage）**：1.704554 s → 1.612283 s，**−5.3359%** `[-6.6299,-4.0128]`，36 对 CI 上界 <0；每请求 13.0043 → 12.3004 µs（均值差 0.7040 µs/请求）。p50/p95/p99：A 1.6967/1.7941/1.8165 s，B 1.6008/1.7109/1.7458 s。
- **task-clock（perf stat）**：1647.05 → 1558.12 ms，**−5.3220%** `[-6.6076,-4.0105]`，36 对 CI 上界 <0。p50/p95/p99：A 1,641.27/1,735.87/1,754.23 ms，B 1,550.80/1,651.31/1,684.75 ms。
- **四口径一致性**：instructions/cycles/task-clock/rusage 点估计最大两两差 0.7816 pp，CI 上界全部 <0；**四组均值/CI 方向一致**（§D.5.5）。但**逐对并非全负**：instructions 1/36、cycles 3/36、task-clock 4/36、rusage 4/36 对为正（§D.5.5）；四组绝对值最小的是 cycles 4.7712%。该 session 的 A/A 宽度 3.8789 > 3.0，`confirmed=false`，所以这只是"均值/CI 一致的负向"，不是"已确认的量级"。
- **wall**：受控 session 主场景 mean +88.2691% `[-25.3260,+253.7874]`，21 次停顿全部保留；剔除 environment_stall 后 A p95/p99 1.971/2.030 s、B 1.901/2.191 s（原始 21.588/37.916 与 21.829/39.518）。**不作结论**。
- **其他线程**：受控 session 未做 per-thread 分解（无 `perf record`/线程归属），因此不能回答"是否转移给其他线程"；旧 session 的 profile 显示 A main/worker 50.47%/49.53% → 50.72%/49.28%，`context-switches` 1.02637 → 1.02236/请求（−0.39%，7/10），`pthread_mutex_lock/unlock` 调用数 3,932,596 → 3,932,536（相同），但那不是本 session 的证据。
- **机制计数（旧 session，补充）**：strace `eventfd_read` 262,135 → 131,073（−50.0%）；bpftrace `read_all` 260,315/258,014 → 127,087/128,228；`read_eagain` 130,043/128,901 → **8/8**。perf tracepoint 分项 `read` 1.990 → 0.987、`poll` 0.877 → 0.920、`write` 0.995 → 0.987 次/请求。受控 session 未采集这些计数（§D.5.5）。`futex` 3.149 → 2.104 与 bpftrace/strace 不一致，**不作为机制证据**（§D.6）。总 syscall 7.955 → 6.966（−12.4%）是机制投影，不是 B 的实测总数。
- **符号位移**：旧 session 的 `native_queued_spin_lock_slowpath` 0.54% → 1.92%（+1.38 pp）是测得的符号位移，因果归因未建立，且已包含在旧 session 的 cycles 净值内；受控 session 未做符号级 profile，不能据此声称该位移在本 session 重现。

**旧 session 的量级（只作跨 session spread）**：8 对完整纪律 instructions −6.3384% `[-7.4000,-5.1915]`、cycles −5.9915% `[-7.2767,-4.9090]`、task-clock −6.4836% `[-8.0801,-4.9612]`、rusage −6.7022% `[-8.3084,-5.2332]`；36 对 rusage −11.968% `[-12.644,-11.266]`；10 对 perf stat user+sys −1.416%、task-clock −1.71%、instructions −2.42%、cycles −2.10%。**这些与受控 session 的 −5.3% 不一致，且未解释。**

### Q5 是否改善等价应用任务

**同一等价任务、均值方向为负，但端到端量级未建立**：受控 session（session-r2r1）使用同一 `sluice-copy` 4 KiB/d4/w1 任务、同一源数据、逐字节 `dst_matches_src=True`、`read_ops=65540`/`write_ops=65536` 冻结。36 对主场景的四组**均值与 95% CI 上界**为负：instructions **−5.5528%** `[-6.5241,-4.5699]`、cycles **−4.7712%** `[-5.8198,-3.7236]`、task-clock **−5.3220%** `[-6.6076,-4.0105]`、rusage **−5.3359%** `[-6.6299,-4.0128]`，CI 上界全部 <0；但**逐对正号为 instructions 1/36、cycles 3/36、task-clock 4/36、rusage 4/36**（§D.5.5）。每请求 rusage 节省 0.7040 µs、instructions 节省 1,453.40 条。负对照 12 对也是同号小幅（rusage −1.4396%，每请求 0.5880 µs）。

但该 session 未通过预注册 A/A 安静度门（+0.6512% `[-1.2814,+2.5974]`，宽度 3.8789 > 3.0），`confirmed=false`；旧 session 的量级从 −1.4% 到 −12% 不一致且未解释。因此能声称的是：**该删除在同一受控 session 的均值/CI 层面为负，且机制计数（旧 session）确认每 drain 少一次 read**；不能声称一个可确认的端到端百分比，也不能声称逐对稳定为负。

限定：(1) 不得引用任何单一百分比作为收益；若最终按 GO 处理，只能采用受控 session **四组口径中绝对值最小者** `min(|−5.5528%|, |−4.7712%|, |−5.3220%|, |−5.3359%|) = 4.77%`（cycles），旧 session 的约 12% 是未被复现的大幅点估计，不是统计上界，也不是产品声明；(2) wall 未建立（受控 session mean +88.2691%，被 writeback 停顿主导）；(3) 未做 no-op/布局控制，instructions 的因果份额仍有 #618 记录的 +1.0–1.4% 布局漂移风险；(4) 只覆盖 ThreadPool / 4 KiB / d4 / 单 worker / warm cache / 本机 btrfs；不外推 wall、能效、io_uring、多 worker。

### Q6 反例与回归区间（测试过与未测范围）

**测试过的反例/边界**：
- **受控 session 的 A/A 安静度失败**：12 对 rusage +0.6512% `[-1.2814,+2.5974]`，宽度 **3.8789** > 3.0；instructions/cycles/task-clock/rusage 四组宽度 4.0351/3.2715/4.0704/3.8789 全部 >3，`confirmed=false`（§D.5.5）。这是预注册判定中**唯一未通过的条件**，直接决定 `confirmed=false`；机制裁决的降级还叠加了拆分后归档回执缺失（§E.1）。
- **受控 session 的主场景与负对照**：36 对四组均值与 CI 上界为负，但**逐对正号** instructions 1/36、cycles 3/36、task-clock 4/36、rusage 4/36（§D.5.5）；负对照 12 对 rusage −1.4396% `[-2.7800,-0.0811]`、instructions −1.7238%，不是零效应也不是反例（§D.4）。
- **受控 session 的环境反例**：21 次 wall 停顿全部保留（A/A 4、主场景 13、负对照 4）；运行中 sysfs 频率下探（各段 cpu2 最低 1,196,290/1,182,595/1,174,069 kHz、cpu3 最低 1,170,505/1,173,094/1,196,203 kHz）；主段 pair 11 pre-run cpu3 3,339,500 kHz，另有 A/A 段 warmup1 cpu2 1,197,317、pair 2/B cpu3 3,491,891、pair 4/A cpu2 3,193,079 与负对照段 pair 3/B cpu3 1,919,971、pair 10/A cpu2 3,491,840、pair 10/B cpu2 1,644,941 的 pre-run 偏差（§D.5.5）；三段 `freq_eq_all=false`；**无法声称测量期间持续 `cur==max`**。
- **受控 session 的纪律记录**：120 次 `pre.final_wb_kb` 全为 0、`pre.final_dirty_kb` 最大 524 kB、无轮询超时；guard 起止为空且命中即 `RuntimeError` 中止；二进制与源前后 sha256 一致（§D.5.5）。
- mutant `progress_source_mut_drain_success_keeps_pending`：r1 会话在当前构建上得到 exit 1 与 3 条语义 re-arm FAIL；审计复跑得到相同结果，但**该输出未归档**。归档的 `evidence-r2/mutants/*.run.log` 是 16:05 拆分前旧目标，drain mutant 记录的是 4 条 mechanism 命名 FAIL，不是 3 条语义断言（§C.6）。
- 语义 oracle 在 BASE 与 B 均通过；BASE drain 体的机制测试红（2 reads/1 EAGAIN）。
- uring k11b（drain 后真实 CQE 仍唤醒）、k1/k2（CQE 先于 drain 由 final probe 恢复）、23 项 uring progress race、20/20 external loop、shutdown lifecycle 矩阵通过。
- 结构化记录 13 套件与四配置矩阵通过，但拆分后的日志未保存；sanitizer 只有 `app_copy_pipeline_oracle_test` 与 `threadpool_progress_race_test` 有非空通过回执，语义目标的 TSAN/ASAN 日志为 0 字节且旧目标名。
- p99：`p99BudgetPct=3` 无预注册定义；旧 session 按 CPU 口径两 cell 的 p99 均未超 3%（B 低于 A），wall 口径下旧负对照 raw P99 A 1.9700 s → B 2.4803 s 会超过 3%，但 wall 被停顿主导，不作判据；受控 session 未实现 p99 判定，只报告 B 的各分位低于 A（§D.10、§D.5.5）。
- 负对照 **不是零效应**：受控 session rusage −1.4396%（CI 上界 −0.0811 < 0）、instructions −1.7238%；按每请求归一化节省 0.5880 µs/请求（主场景 0.7040），不是零效应，也不是反例或非劣证明（§D.4）。与 #634 同 cell 的 +0.10% 点估计方向相反但 CI 重叠，原因未解释。
- futex 在**五个来源**中幅度不一致（≈0.51、1.07、0.86–0.90、0.668、strace ≈1.00），已撤销，不作为机制证据；机制证据是 read 减少（§D.6）。
- 10 对 perf stat session 为 **A→B 顺序**而非 AB/BA，且无 protocol 记录；其内部四口径一致（−1.416%~−2.42%），不能作为 36 对结果的反证（§D.5.1）。
- A/A 的 frozen ops 来源是 pair 1 arm B 而非首个有效 A 运行（数值一致，已披露）。
- 受控 session 120/120 oracle ok（主场景 72、A/A 24、负对照 24）；旧 A/B 主场景与负对照 72/72 oracle ok；旧 8 对会话 8/8 对、0 失败；旧主场景 12 次、旧 A/A 与旧负对照各 13 次 environment_stall 保留在 CPU 统计，仅从 wall 尾部剔除。

**未测/反例风险**：
- io_uring 内核写者在成功 read 与清 pending 之间重新 signal 的窗口无确定性注入；若未来去掉 `PhysicalProbe`，会丢唤醒。
- 不持 `mtx_` 的未来用户态写者会在该窗口漏一次唤醒；该不变式未由类型系统强制。
- 多 worker、zero-op 密集、W-03 多生产者、cold cache、真实 EINTR、uring 路径的 per-drain read 次数未独立测量。
- **受控 session 未采集系统调用计数**：没有 bpftrace/strace，per-drain read 次数只能引用旧 session；也没有 per-thread/`perf record` 分解，不能回答"是否转移给其他线程"。
- **受控 session 的 A/A 噪声未分解**：12 对逐对 sd 3.5908% vs 旧 36 对 2.650%；未做追加 A/A 对数、更安静环境或按核绑定的复测。
- **受控 session 的频率持续性未确认**：运行中 sysfs 采样下探，三段 `freq_eq_all=false`；对 cycles/instructions 的影响未量化。
- **跨 session 量级差异（−1.4%~−12%）未解释**；10 对 session 的 protocol、预注册门槛独立存档、B.2 的 caller 级复算均缺失；受控 session 有同 session A/A（12 对）但未过宽度门，旧 8 对仍无同 session A/A（§D.9、§F.5）。
- 负对照与 #634 的同 cell 差异未解释；futex 未定标。
- wall 回归区间为 `[-7.620,-5.299]`（clean pairs）到 `+229.087%`（全部有效对），后者被停顿主导；8 对会话 wall mean +237.5% 同样不可用。
- 产品 seam/注释、ledger 等价论证未更新；拆分后的矩阵与 mutant 日志缺失。

### Q7 是否值得进入产品（GO / NO_GO / NOT_ESTABLISHED 与理由）

**INCONCLUSIVE（机制）/ NOT_ESTABLISHED（端到端）/ NOT_READY（产品）——产品不进入**：
- 机制层：删除的是每请求约 1 次必然 `EAGAIN` 的 read，纯删除、无新状态、语义 oracle 在 BASE 与 B 均绿、机制计数（旧 session）显示每次 drain 少一次 read；但唯一一次同 session 受控测量 `confirmed=false`（A/A 宽度 3.8789 > 3.0），拆分后的归档回执仍缺 → `MECHANISM_VERDICT=INCONCLUSIVE`（§E.1）。futex 减半不属于机制证据。**这是相对 r3 的降级**：r3 的 GO 建立在"机制证据成立即足够"的口径上，r4/r5 要求受控测量同时成立。
- 端到端层：受控 session 四组均值与 CI 上界为负（instructions −5.5528%、cycles −4.7712%、task-clock −5.3220%、rusage −5.3359%），但**逐对正号为 1/3/4/4**，A/A 宽度门失败，且跨 session 从约 −1.4% 到 −12% 不一致、未解释 → `END_TO_END_VERDICT=NOT_ESTABLISHED`。若最终按 GO 处理，只能采用四组口径中绝对值最小的 4.77%（cycles）；5.34% 不是四组口径的保守量级，旧 session 的约 12% 是未被复现的大幅点估计，不是上界。
- 产品层：`PRODUCT_READINESS=NOT_READY`。§F.5 的 1–4、6–7 关闭前不得提交为可合并 PR；不得引用任何单一百分比作为收益（包括 4.77%）。

---

## 附录 A：证据索引

| 内容 | 路径 |
|---|---|
| 实现 diff | `b-round2.diff`（sha256 `ee7c4c3e…`） |
| A/A | `results/aa2-4k-d4.{tsv,json,txt,meta.json}` |
| A/B 主场景 | `results/ab-main-4k-d4.{tsv,json,txt,meta.json}` |
| 负对照 | `results/ab-neg-64k-d1.{tsv,json,txt,meta.json}` |
| 机制计数 | `results/mech-counters.{tsv,summary.json,meta.json,run.log}`；`results/mech-probe/logs/*.strace.log.gz` |
| profile | `raw-r2/record/agg-*.tsv`、`raw-r2/perfstat/*`、`raw-r2/strace/*`、`raw-r2/timing/*` |
| B profile | `raw-r2/ab/stat/*`、`raw-r2/ab/trace/*`、`raw-r2/ab/record/agg-*.tsv` |
| 正确性收据 | `evidence-r2/mutants/*`、`evidence-r2/sanitizers/*`、`evidence-r2/config-matrix/*`、`evidence-r2/suites/*` |
| 源数据 | `data/src-256m.bin`（sha256 `a1f8dd97…`） |
| 二进制 | `sluice-copy-timing-A`（`0eca1011…`）、`sluice-copy-timing-B`（`db972b19…`）、`sluice-copy-sym`（`e5b12e9f…`）、`sluice-copy-sym-B`（`eae49cb0…`） |
| 跨计数口径对账 | `reconciliation/README.md`、`plain-reused-dst.txt`、`plain-fresh-dst.txt`、`perfstat-with-sync.txt`、`recon-provenance.txt`、`runner_perf_recon.py`、`analyze_perf_recon.py`、`probe_futex_recon.py` |
| 对账 8 对 perf stat | `results/recon-perf-4k-d4.{tsv,meta.json,analysis.json,analysis.txt}` |
| 对账 futex/read 计数 | `results/recon-futex-counts.{tsv,meta.json}` |
| **受控 session 原始数据** | `results/session-r2r1-{aa-4k-d4,main-4k-d4,neg-64k-d1}.tsv`、`results/session-r2r1-{aa-4k-d4,main-4k-d4,neg-64k-d1}.meta.json`、`results/session-r2r1.meta.json` |
| **受控 session 分析** | `results/session-r2r1.analysis.json`、`results/session-r2r1.analysis.txt` |
| **受控 session 驱动与溯源** | `session-r2r1/runner_session.py`、`session-r2r1/analyze_session.py`、`session-r2r1/run.log`、`session-r2r1/PROVENANCE.txt` |
| 10 对 perf stat 交叉检查原始输出 | `raw-r2/ab/stat/stat.txt`、`stat2.txt`、`A1–A5/B1–B5.stdout`（仅 perf 输出，无 protocol 字段） |
| r1 清单 | `MANIFEST-round1.sha256`（131 条哈希行） |
| manifest | `MANIFEST.sha256` |

## 附录 B：r1 作者会话实际执行的检查（本修订未重跑）

下表是 r1 撰写会话的回执，r2 修订**没有重跑**其中任何一项，原样保留。

| 检查 | 命令 | 结果 |
|---|---|---|
| 仓库基线 | `git -C /home/jnhu/Source/Sluice rev-parse HEAD` / `git status --porcelain` | `3f22d2d0…`；写入前无输出，写入后仅 `?? docs/evidence/621-round2-R1/` |
| B 分支 | `git -C wt-B rev-parse HEAD` / `git status --porcelain` | `a3a14a31…`；无输出 |
| diff 哈希 | `sha256sum b-round2.diff`；`git -C wt-B diff 3f22d2d0..HEAD \| sha256sum` | 两者均 `ee7c4c3e…` |
| 语义 oracle（B） | `./build/linux/x86_64/release/progress_source_drain_semantic_oracle_test` | exit 0 |
| 机制测试（B） | `./build/linux/x86_64/release/progress_source_drain_mechanism_test` | exit 0 |
| mutant | `./build/linux/x86_64/release/progress_source_mut_drain_success_keeps_pending` | exit 1，3 条语义 FAIL |
| race 回归 | `./build/linux/x86_64/release/threadpool_progress_race_test` | exit 0 |
| 语义 oracle（纯净 BASE） | `wt-base-semantic` 同一二进制 | exit 0；`git diff --stat -- include src` 空 |
| PR 存在性 | `gh pr list --head research/621-r2-R1-drain-eagain-read --json number,state,url` | `[]` |
| manifest（r1 清单） | `sha256sum -c MANIFEST.sha256` | r1 声称 357/357 OK，exit 0。**无法复验**：`MANIFEST-round1.sha256` 只有 131 条哈希行 |

**未执行**：四配置矩阵重跑、A/A/A/B 复测、perf c2c、`perf sched`、io_uring 实测、真实 EINTR 注入、cold-cache 运行。本报告不把这些未执行项写成已验证结论。

## 附录 C：r2/r3/r4 修订会话实际执行的检查

### C.1 r2 会话（跨口径对账阶段）

| 检查 | 命令 | 结果 |
|---|---|---|
| 报告副本一致性（修订前） | `sha256sum /home/jnhu/sluice-621-r2/REPORT.md /home/jnhu/Source/Sluice/docs/evidence/621-round2-R1/REPORT.md` | 两路径同为 `10dadc76…`（同为 r1 内容） |
| 对账产物存在性 | `wc -l results/recon-perf-4k-d4.tsv results/recon-futex-counts.tsv` | 17 行（表头 + 16 次正式运行）、5 行（表头 + 4 次探针） |
| 对账统计 | 读取 `results/recon-perf-4k-d4.analysis.txt` | instructions −6.3384%、cycles −5.9915%、task-clock −6.4836%、rusage −6.7022%、wrapper −6.5453%；`pairs_run=8 valid_pairs=8 oracle_failures=0`；`governor_restored=True`；`freq_eq_all_in_all_runs=False` |
| 对账 meta | 读取 `results/recon-perf-4k-d4.meta.json` | `guard_at_start=[]`、`guard_at_end=[]`；loadavg 0.18 → 1.18；pre `final_dirty_kb<=144`、`final_wb_kb=0`；`ops_consistency.checked_runs=18`、`mismatches=[]` |
| futex/read 探针 | 读取 `results/recon-futex-counts.tsv` | futex A 398,812/409,933、B 341,883/369,693；`read_eagain` 130,043/128,901 → 8/8；`read_all` 260,315/258,014 → 127,087/128,228 |
| 机制计数 | 读取 `results/mech-counters.tsv` | main `eventfd_read` 262,135 / 131,073，negative 16,385 / 8,193；请求数 131,076 对 8,193 |
| 对账脚本哈希 | `sha256sum reconciliation/README.md reconciliation/plain-reused-dst.txt reconciliation/plain-fresh-dst.txt reconciliation/perfstat-with-sync.txt reconciliation/recon-provenance.txt reconciliation/runner_perf_recon.py reconciliation/analyze_perf_recon.py reconciliation/probe_futex_recon.py` | 三个 `.py` 与 `recon-provenance.txt:74-77` 一致（`5ecccf9d…`、`1af02cda…`、`a000262e…`）；全部 8 个文件已写入 manifest |
| manifest 更新 | 将 `reconciliation/` 8 个文件与 `results/recon-*` 6 个文件追加到 `MANIFEST.sha256`，并更新 `REPORT.md` 行 | 14 条新增 + 1 条更新 |
| 两路径一致性 | `sha256sum /home/jnhu/sluice-621-r2/REPORT.md /home/jnhu/Source/Sluice/docs/evidence/621-round2-R1/REPORT.md` | 两路径同值 |

### C.2 r3 会话（审计后修订）

| 检查 | 命令 / 读取 | 结果 |
|---|---|---|
| 10 对 session 的 run 顺序 | `grep -n '^### ' raw-r2/ab/stat/stat.txt raw-r2/ab/stat/stat2.txt` | `A run 1, B run 1, …, A run 10, B run 10`——**每对 A 先 B 后，非 AB/BA 交错** |
| 10 对 session 的 user+sys | `python3 -I` 解析上述两文件的 `seconds user`/`seconds sys` | A 均值 **1.489662 s**、B 均值 **1.468571 s**，均值之比 **−1.4158%**；逐对均值 **−1.3801%**，10 对中 2 对为正（+4.87%、+0.17%） |
| 每请求归一化 | 由记录的 CPU 均值 ÷ 请求数（负对照 8,193；主场景 131,076） | 31.62 / 11.80 / 12.61 µs/请求；每请求节省 0.651 / 0.845 / 1.412 µs（负对照 / 8 对 / 36 对） |
| 归档 mutant 回执 | `cat evidence-r2/mutants/progress_source_mut_drain_success_keeps_pending.run.log`；`ls -la evidence-r2/mutants/` | 16:05、旧组合目标；4 条 FAIL，用例名为 mechanism 命名（2×`successful_drain_consumes_the_counter_in_one_read`、1×`interrupted_read_retries_without_losing_the_notification`、1×`stale_acknowledgement_cannot_suppress_the_next_wake`） |
| r1 清单可复验性 | `grep -c '^[0-9a-f]\{64\}  ' MANIFEST-round1.sha256` | **131** 条哈希行，故 r1 的 357/357 无法复验 |
| 预注册门槛归档 | `grep -rl 'minMeaningfulEffectPct' .`（排除 `wt-*`、`__pycache__`） | 仅命中 `REPORT.md`；证据根内无归档原件 |
| B.2 caller 汇总 | 读取 `raw-r2/record/agg-attrib.tsv` 并求各 caller 行之和 | `poll_progress` 1.389+0.057+0.046=**1.492**；`publish_one` 0.211+0.014=**0.225**；`wait_one`=**32.197**；`run_syscall`=**19.752**（与 B.2 表值 2.00/0.43/31.63/19.95 不一致） |
| manifest 校验 | `cd /home/jnhu/sluice-621-r2 && sha256sum -c MANIFEST.sha256` | 371/371 OK，exit 0 |
| 两路径一致性 | `sha256sum /home/jnhu/sluice-621-r2/REPORT.md /home/jnhu/Source/Sluice/docs/evidence/621-round2-R1/REPORT.md` | 两路径同值（见 `MANIFEST.sha256` 的 `REPORT.md` 行） |

### C.3 r4 会话（受控 session 后修订）

| 检查 | 命令 / 读取 | 结果 |
|---|---|---|
| 两路径一致性（修订前） | `sha256sum /home/jnhu/sluice-621-r2/REPORT.md /home/jnhu/Source/Sluice/docs/evidence/621-round2-R1/REPORT.md` | 两者同为 `b5def45c0b78bd0caeeba2b550fedcc15777b6c515f3a158f7d5326d81a6d010`（r3 内容） |
| 受控 session 摘要 | 读取 `results/session-r2r1.analysis.txt` | `aaPairs=12`、`aaCpuRelDiff=0.6512%`、`aaCiWidth=3.8789`、`mainPairs=36`、instructions/cycles/task-clock/rusage 四组 CI 上界均 <0、`negPairs=12`、`negCpuRelDiff=-1.4396%`、`oracleFailures=0`、`governorRestored=True`、`confirmed=False`、`pauses=21` |
| 分位数与宽度 | `python3 -I` 解析 `results/session-r2r1.analysis.json` | 主场景 p50/p95/p99（A/B）与 A/A、负对照四口径宽度；本次修订引用值全部与 JSON 一致（如 rusage p99 A 1.8165 s / B 1.7458 s） |
| 每请求归一化 | 由记录均值 ÷ 请求数（主/A/A 131,076；负 8,193） | 主 13.0043 → 12.3004 µs/请求（均值差 0.7040）；负 39.8911 → 39.3032（0.5880）；A/A 12.9418 → 13.0194；均值之比 −5.4132% / −1.4739% / +0.5999% |
| oracle 全行复核 | `python3 -I` 读取三个 TSV | 24/72/24 行；`rc!=0` 0、`oracle_ok!=True` 0、`dst_matches_src!=True` 0、`read_ops/write_ops!=frozen` 0、`short_writes` 全 0、`bytes_copied` 全 268,435,456 |
| 静默与超时 | 解析三个 `results/session-r2r1-*.meta.json` | 120 次 `pre.final_wb_kb` 全 0；`pre.final_dirty_kb` 最大 524（aa）/188（main）/188（neg）kB；`pre_run_timeouts=[]` |
| governor | 解析 `session` 与各段 freq 字段 | 设置前 20/20 `schedutil`；`governor_set.after` 20/20 `performance`、`freq_check_khz` cpu2/cpu3 = 3500000/3500000；恢复 20/20 `schedutil`、`ok=true`；各段 `freq_eq_all=False`；pre-run 偏差见 §D.5.5（主段 pair 11 cpu3 3,339,500；另有 A/A 3 处、主段 3 处、负对照 3 处 arm/warmup 级低于 max） |
| 门禁实现 | `sed -n '250,280p' session-r2r1/analyze_session.py` | `confirmed` 谓词在 `:262-272`：`aa_pairs>=12`、`|mean|<=1.5`、`width<=3.0`、`main_pairs>=36`、四口径 `ci95_high_pct<0`、rusage `mean<=-1.0`、`oracle_failures==0`、`governor_restored`；脚本 mtime 18:51（session 开始 18:52:27） |
| guard 行为 | `sed -n '650,730p' session-r2r1/runner_session.py` | warmup 前、每对前、pre-sync 后、每臂前均调用 `guard_processes()`，命中即 `RuntimeError` 中止；session 完成且 `aborted=None` |
| 停顿清单 | `tail -40 session-r2r1/run.log` | 21 条 PAUSE，逐条 wall/cpu 与 §D.5.5 一致（4+13+4） |
| 受控 session 原始行数 | `wc -l results/session-r2r1-*.tsv` | 25 + 73 + 25 = 123 行（表头 3 + 120 次正式运行） |
| manifest | 追加受控 session 13 个文件、更新 `REPORT.md` 行后执行 `cd /home/jnhu/sluice-621-r2 && sha256sum -c MANIFEST.sha256` | **384/384 OK，exit 0**（371 + 13） |
| 两路径一致性（修订后） | 写入后 `cp` 到工作区并 `sha256sum` 两路径 | 两路径同值（见 §F.3） |

**r4 本次修订没有新增任何测量**：只读取受控 session 的归档产物与原始 TSV/meta，并执行上述解析、门禁代码读取与 manifest 校验；没有重跑 A/A、主场景、负对照、四配置矩阵、sanitizer、mutant，也没有运行 bpftrace/strace、`perf sched`、`perf c2c` 或 io_uring 实测；未修改 `results/`、`raw-r2/`、`reconciliation/` 下的测量产物（只读）。manifest 的变更仅为新增 13 条受控 session 文件与更新 `REPORT.md` 行。

### C.4 r5 会话（审计后口径修正）

r5 不新增测量，只按审计意见复算并修正口径。以下检查均由 r5 会话执行：

| 检查 | 命令 / 读取 | 结果 |
|---|---|---|
| 主场景逐对符号 | `python3 -I` 读取 `results/session-r2r1-main-4k-d4.tsv`，按每对 `(B−A)/A` 复算 | 均值 −5.552785 / −4.771177 / −5.321967 / −5.335920；正号：instructions 1/36（pair 12 +1.462）、cycles 3/36（12 +2.816、14 +1.523、33 +0.819）、task-clock 4/36（6 +1.015、12 +4.173、14 +2.855、33 +1.383）、rusage 4/36（6 +0.878、12 +4.402、14 +2.739、33 +1.484） |
| 保守量级 | 对上一行四个均值取绝对值最小 | `min(5.5528, 4.7712, 5.3220, 5.3359) = 4.7712`（cycles）；5.34% 只是 rusage/instructions 两者取小 |
| 旧 session 全局频率最低 | `python3 -I` 读取 `results/aa2-4k-d4.tsv`、`results/ab-main-4k-d4.tsv`、`results/ab-neg-64k-d1.tsv` 的 `freq2_min_khz`/`freq3_min_khz` | aa 1,156,843 / 1,195,281；main 1,196,265 / 1,196,422；neg 1,195,914 / 1,178,333 → 全局 cpu3 最低为负对照 **1,178,333**，故 §D.1 原句更正 |
| 受控 session pre-run/warmup 偏差 | `python3 -I` 解析三个 `results/session-r2r1-*.meta.json` 的 `warmups[].freq_pre_khz` 与 `pairs[].arms[].freq_pre_khz` | A/A：warmup1 cpu2 1,197,317、pair 2/B cpu3 3,491,891、pair 4/A cpu2 3,193,079；main：pair 11/B cpu3 3,405,094、pair 20/B cpu2 3,206,122、pair 24/A cpu2 3,033,808（pair 级 pair 11 cpu3 3,339,500）；neg：pair 3/B cpu3 1,919,971、pair 10/A cpu2 3,491,840、pair 10/B cpu2 1,644,941 |
| futex tracepoint 5+5 | `python3 -I` 解析 `raw-r2/ab/trace/syscalls.txt` 的 `sys_enter_futex` | A 均值 412,811.4（3.1494/请求）、B 均值 275,787.4（2.1040/请求）、B/A 0.668（−33%）；`sys_enter_read` A 260,817.0（1.9898）/B 129,321.8（0.9866） |
| 0.989 的出处 | `raw-r2/perfstat/syscalls.txt` 的 `sys_enter_write` 五次；`raw-r2/perfstat/read_eagain.txt` 的 `@eagain` | write 130,490/129,900/129,310/128,896/129,686 → 均值 129,656.4 → 0.9892/请求；EAGAIN 130,152 → 0.9930/请求 |
| 机制粗估算术 | 3.39 + 1.14 | 4.53%；受控 cycles 点估计 −4.7712% 已超过该值，故改称粗估、不称界 |
| 结论对齐 | 对照 §B.4、§B.6、§D.4、§D.6、§D.7 与 Q4–Q7 | B.4 改"未解释符号位移"；B.6 负对照 falsifier 改为"与主场景相反方向/无法归一化解释"；D.6 补第 5 行；Q4–Q7 统一为"均值/CI 为负、逐对正号 1/3/4/4、保守量级 4.77%、12% 非上界" |

**r5 未执行**：没有重跑任何测量、测试或构建；没有新增统计、CI 或样本；manifest 仅更新 `REPORT.md` 的哈希行，并复验 `sha256sum -c MANIFEST.sha256`（结果见 §F.3）。

**r2–r5 未执行**：不重跑 A/A、36 对 A/B、负对照、四配置矩阵、sanitizer、mutant 与 8 对对账测量（只读取其产物）；不执行 `perf sched`/`perf c2c`/io_uring 实测；不修改 `results/`、`raw-r2/` 与 `reconciliation/` 下任何测量产物（只读）。"审计复跑得到 3 条语义 FAIL"来自本次交办记录，不是本修订会话执行的检查。
