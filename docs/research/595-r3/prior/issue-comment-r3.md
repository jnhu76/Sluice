# #595 R3 裁决评论（可直接粘贴到 Issue #595）

> 本文件是待发布评论正文；本轮未发布（远端只读）。下方区块为机器可读字段（区块首尾各有一个标记行）。
> 本轮不覆盖 R1/R2 的判定；只追加 R3 证据与裁决依据。

---

BEGIN_SLUICE_595_R3
ROUND=R3
DATE=2026-11-05
BASE_SHA=0a22a79195f1b913145fcd443496e7f70b08cdcf
R2_ANCHOR=271b81bfc9d64eecc0266fe7066b9040525dd710
PLATFORM=E5 native Linux, Xeon E5-2666 v3, Fedora 44, kernel 7.2.11-200.fc44.x86_64, SMT active, schedutil
PREREGISTRATION=MISSING (evidence/PREREGISTERED_R3.md 不存在)
MEASUREMENT_ARTIFACTS=MISSING (measurement-main/product/mechanism/analysis 四目录均不存在)
AA_AB_NEGATIVE_CONTROL_RUNS=0
NON_TIMING_SMOKE=3 B-arm runs (raw/smoke/, d4/d16/d64) + 2 A0/A1 d4 runs; wall/cpu printed but not used as evidence
AA_CALIBRATION=NOT_RUN
ARMS=A0/A1/B 仅为研究标签；A_R2/B_R2 无定义；CONTEXT.md §7.2 OPEN-1 未解除
PAIRED_UNITS=0
B_MINUS_A0_CPU=UNMEASURED
B_MINUS_A0_CI95=UNMEASURED
B_MINUS_A1_CPU=UNMEASURED
PERFORMANCE_PROTECTION=NOT_ASSESSED
NEGATIVE_CONTROLS=NOT_RUN
RESOURCE_GATE=NOT_MEASURED
CORRECTNESS=PASS
BASE_GREEN=13/13 scenarios, exit 0, rerun 2026-11-05
MUTANTS=3/3 named mutants RED (M1 exit 1/6 FAIL; M2 exit 1/1 FAIL; M3 exit 1/1 FAIL)
RESTORED_GREEN=PASS (receipts + current BASE binary; rebuild step not re-executed)
REGRESSIONS=6/6 exit 0
MECHANISM_ENTER_BASELINE=1.0000 enter/op; 20k run total 60,004 measured by strace -c; 60,000 pure submit + 4 probe and to_submit>1=0 inferred from 512-request line trace (1,536+4), not a 20k per-call measurement; max SQE/enter=1
MECHANISM_ENTER_W03_D16=0.0849 enter/op theoretical (20,000 -> 1,698; -91.5%)
MECHANISM_ENTER_W03_D4=0.2773 enter/op theoretical (20,000 -> 5,545; -72.3%)
MECHANISM_ENTER_W02_D2=0.99995 enter/op (20,000 -> 19,999; -0.005%)
PROTOTYPE_SMOKE_UNPAIRED=raw/smoke receipts: W03 d4 5,955 flushes/20k; d16 11,140/20k; d64 17,781/20k (BUILD.md §7 run: 6,139/10,954/17,739; single run each, no CI, not performance evidence)
PRIMARY_EXPOSURE_SHAPE=W-03 external loop, 4 KiB warm tmpfs uring, REAL_EXPOSURE within a synthetic benchmark; NOT a #604-admitted real application
HEADLINE_SHAPE_EXPOSURE=W02-uring-4k-d2 STARTUP_ONLY -> NO_REAL_BATCH_EXPOSURE
SLUICE_COPY_EXPOSURE=default depth=1 NO_REAL_BATCH_EXPOSURE; depth>1 STARTUP_ONLY
MECHANISM_VERDICT=MECHANISM_ONLY
CLOSEOUT_MAPPING=MECHANISM_ONLY is a round-local field; mapped to issue #595 §7 closeout vocabulary as KEEP_IMMEDIATE (mechanism supported at syscall level, product not established)
PRODUCT_VERDICT=KEEP_IMMEDIATE
PRODUCT_CANDIDATE_READY=NO
PRODUCT_CANDIDATE_PR=NOT_OPENED
MERGED=NO
ISSUE_604_HANDOFF=NOT_PRESENT (#604 OPEN; no READY_FOR_595)
PUBLIC_API_CHANGED=NO (5 public async headers unchanged; isolated worktree only touches detail headers)
V2_CONTRACT_CHANGED=NO
R1_R2_EVIDENCE_CHANGED=NO
REMOTE_CHANGED=NO
MAIN_WORKTREE_STATUS=CLEAN (git status --porcelain = 0 lines)
EVIDENCE_ROOT=/home/jnhu/sluice-595-e5/r3/evidence
EVIDENCE_MANIFEST=/home/jnhu/sluice-595-e5/r3/evidence/MANIFEST.sha256 (246 entries; sha256 recorded in /home/jnhu/sluice-595-e5/r3/evidence/MANIFEST.sha256.hash; sha256sum -c 通过)
COMMENT_HASH_FILE=/home/jnhu/sluice-595-e5/r3/evidence/MANIFEST.comment.sha256 (独立记录本评论正文哈希)
FINAL_PATCH=/home/jnhu/sluice-595-e5/r3/evidence/r3-final.patch (10 files, +2883/-51; reverse check exit 0)
HISTORICAL_PATCH=/home/jnhu/sluice-595-e5/r3/evidence/r3.patch (9 files, +1436/-51; 已过期，不能 reverse apply)
NEXT_TRIGGER=先预注册 R4/R3R（A0/A1/A_R2/B_R2 定义 + 24 对 + A/A 5/3/3 pp + 负控 Holm + 停手条件），再计时
END_SLUICE_595_R3

---

## 1. 结论

**R3 没有执行任何 A/A、A/B 或负控计时；只有非计时的 smoke（`raw/smoke/`，带 wall/cpu 打印但不作性能证据）。** 证据根中不存在 `PREREGISTERED_R3.md`，也不存在 `measurement-main` / `measurement-product` / `measurement-mechanism` / `measurement-analysis` 四个目录；`harness/` 只有普查脚本，`logs/` 只有基线构建日志。因此：

- 没有 A/A 校准；
- 没有 A0/A1/B 的 24 对配对；
- 没有 `B−A0` 的 CPU 点估计与 95% CI；
- 没有负控家族、保护门、RSS/线程门。

在此前提下，产品净胜门（`B−A0` 总 CPU 点估计 ≤ −10%、95% 上界 ≤ −5%）**无法判定为通过**。按裁决规则，产品裁决为 **`KEEP_IMMEDIATE`**，**不建产品候选 PR**。

机制裁决为 **`MECHANISM_ONLY`**：syscall 层的 enter 合并事实成立且有独立证据；但“合并 enter → CPU 收益”没有被本轮测量。R2 的 CPU 证据属于另一个机制（owner-gated deferred flush）和另一个二进制/会话，不能借用。

本裁决不否定机制：R1 的 `KEEP_IMMEDIATE` 语义仍是“机制得到支持 ≠ 当前策略可部署”，R3 继续沿用这一区分。

---

## 2. 暴露形状与代表 consumer（这是本轮最有价值的部分）

> W-03 是**合成 benchmark**，不是 #604 准入的真实应用；它提供的是可辩护的代表 trace 与真实批暴露形状。机器字段用 `PRIMARY_EXPOSURE_SHAPE`，不写 `REAL_TASK`，避免被读作 #604 Real-task 门通过。

### 2.1 consumer 筛选

| consumer | 稳态行为 | 可证明的组 | 分类 |
|---|---|---|---|
| `bench/bench_w03_external_loop.cpp` | 等待后消费所有 front-ready 完成，再整窗补发（`:339` 补发、`:349` 提交、`:370` 等待、`:387` consume-all-ready） | 初始填充 + 每次补发 burst（1..depth） | **REAL_EXPOSURE（benchmark 内）** |
| `bench/bench_w02_pipeline.cpp` | 每次只 `wait_for(oldest)` 并结算 1 条，再补 1 条（`:337/:348/:384`） | 仅初始填充 `depth` 条 | **STARTUP_ONLY** |
| `apps/sluice-copy/copy_task.cpp` | 预填 `pipeline_depth` 条；稳态下一条读依赖该 slot 写完成（`:165-174/:169/:228-233`） | 仅预填；默认 `depth=1` 连预填组都没有 | 默认 **NO_REAL_BATCH_EXPOSURE**；`depth>1` **STARTUP_ONLY** |

### 2.2 就绪数分布（稳态，不含启动组）

| case / phase | 0 | 1 | 2 | 3–8 | >8 | zero-ready | 单成员占比 |
|---|---|---|---|---|---|---|---|
| W03-d16 ph1 | 0 | 83 | 116 | 264 | 1234 | 80 | 4.89% |
| W03-d16 ph2 | 0 | 128 | 54 | 86 | 1242 | 27 | 8.48% |
| W03-d16 ph3 | 0 | 16 | 5 | 9 | 1300 | 1 | 1.20% |
| W03-d4 ph1 | 0 | 256 | 408 | 4880 | 0 | 80 | 4.62% |
| W03-d4 ph2 | 0 | 167 | 283 | 4912 | 0 | 27 | 3.11% |
| W03-d4 ph3 | 0 | 65 | 96 | 5305 | 0 | 5 | 1.19% |
| W02-d2（每 phase） | 0 | 19998 | 0 | 0 | 0 | 1 | 100% |

数据：`raw/census/analysis/*-full.json` 的 `ready_distribution_steady`；`groups = 稳态组 + 1 启动组 + zero-ready` 已逐 phase 闭合。verbose 运行（512 请求）的成员身份不符、组内 take/wait、别名/重复、越界、组员数不符、反序**全部为 0**。

### 2.3 基线 enter 与拆批点

- 基线每个 `submit_read` 恰好一次 `io_uring_enter(to_submit=1, min_complete=0)`。**20k×3 phase 只用 `strace -c` 直接测得总数 60,004**；“60,000 纯 submit + 4 context probe、`to_submit>1`=0、最大 SQE/enter=1”的分解来自 512 请求逐行 trace（每个 case 1,536 次 `(to_submit=1,min_complete=0,flags=0)` + 4 次 `GETEVENTS`）与工作量推断，**不是 20k 逐调用测量**。
- 具体拆批点：`bench/bench_w03_external_loop.cpp:339` 的补发循环（提交 `:349`）。d16 稳态组示例：`ready=16, window=0, submitted=16`，slot 0..15、offset 65536..126976、len 4096、16 个不同 buffer；该组 `group_ready`→首个 `submit_begin` 为 **934 ns**，最后一个 `member_ready`→首个 `submit_begin` 为 **63 ns**（5 个 verbose 运行合计 3,572 组，last-member→first-submit 间隔 **32 ns–9,771 ns**，0 次反序）。
- 现有容量 `queue_depth=depth`（`:115-116`；`io_uring_queue_init` 于 `src/async/uring_backend.cpp:171`），组大小 `r ≤ depth`，因此一次 submit 可容纳整组。d16 每组基线 16 enter → 重切 1 enter，**差额 15/组**。

### 2.4 净 enter 节省（按现有容量）

| case / phase | 20k 基线 | 重切后 | 净节省 | 比例 |
|---|---|---|---|---|
| W03-d16 ph1/2/3 | 20,000 | 1,698 / 1,511 / 1,331 | 18,302 / 18,489 / 18,669 | 91.5 / 92.4 / 93.3% |
| W03-d4 ph1/2/3 | 20,000 | 5,545 / 5,363 / 5,467 | 14,455 / 14,637 / 14,533 | 72.3 / 73.2 / 72.7% |
| **W02-d2 ph1/2/3** | 20,000 | 19,999 | **1** | **0.005%** |
| W03-d64（512 窗口） | 512 | 8 / 10 / 9 | 504 / 502 / 503 | 98.4 / 98.0 / 98.2% |

**对 #595 的 headline shape（`W02-uring-4k-d2`），即使机制完全成功也只有 1/20,000 的 enter 可省 → `NO_REAL_BATCH_EXPOSURE`。** 真实机会只在 W-03 这类“consume-all-ready 后整窗补发”的 consumer。

补充：R2 的 B 是 owner-gated deferred flush，要求 submitter 是最近的 progress driver；W-03 的 submitter 不是 driver，因此 **R2 原型不会捕获该组**。R3 的价值是把机会移到 consumer 侧，但这也意味着它需要新的调用形态（批量准备/提交），不是 R2 的延长 defer。

---

## 3. 原型机制与已观测成本

原型（研究副本，非公开 API）：`UringAsyncBackend::submit_ready_reads(std::span<const ReadOp>)`，一次 `dispatch_mtx_` 临界区内 accept 前缀 → 逐条写独立 SQE → **只 flush 一次**；cap=8；suffix 逐条 span=1。每条请求保留独立 SQE/Request/CQE/error/cancel/borrow。

已观测的结构成本（**单次 smoke，非计时，无 CI**；回执 `raw/smoke/`）：

| case | B 臂 flush/20k（回执） | `BUILD.md` §7 的另一次运行 | 理论容量值 | 差异来源 |
|---|---|---|---|---|
| W03-d4 | 5,955 | 6,139 | 5,545 | 组形成受调度影响；cap 不绑定 |
| W03-d16 | 11,140 | 10,954 | 1,698 | cap=8 截断 + `suffix_singles=9,863` |
| W03-d64 | 17,781 | 17,739 | ~323 组 | `suffix_singles=17,464`；flush ≈ 基线 89% |

**结论**：cap=8 且 suffix 不二次批量，会把 d16/d64 的机制收益大部分抵消。回执运行与 `BUILD.md` 运行不一致，说明这些计数不是确定性常量。

另有三项归因残差必须披露：

1. `A1` 逐条走 `AsyncIoContext::submit_read`（与 A0 同路径），只余 adapter 簿记；`A0−A1` 未计时。
2. `B` 直接调用 detail seam，**绕过** `AsyncIoContext` 的 `initiation_rejection` 与 `access_mtx_`；`B−A1` 同时包含“分组”和“绕过 context”。B 不是无需新公开契约即可被真实 consumer 调用的路径。
3. **`A0` 不是 `baseSha` 原始循环**：worktree 的 refill 对所有臂先构造完整 `ready` span（`bench/bench_w03_external_loop.cpp:349-372`），A0 再用 `ctx.submit_read` 逐条提交（`bench/r3_ready_batch.hpp:88-98`）；`baseSha` 原循环是在循环内逐条构造 `ReadOp`（`:339-349`）。A0 因此含原始 baseline 没有的组准备/vector 开销；未来计时必须改用原循环或显式披露。

---

## 4. 正确性

| 项 | 结果 |
|---|---|
| BASE GREEN | 13/13 场景，exit 0；连续 5 次回执；本轮再跑一次仍全绿 |
| M1 | `SLUICE_R3_MUTANT_BATCH_INCLUDES_NOT_READY_TRANSPORT` → `--only=sq_full_residue` exit 1；首行 FAIL = `a batch at SQ-full transport residue accepts nothing` |
| M2 | `SLUICE_R3_MUTANT_BATCH_RESIDUE_NOT_ADVERTISED` → `--only=partial_submit` exit 1；唯一 FAIL = `partial kernel consumption advertises dispatch residue` |
| M3 | `SLUICE_R3_MUTANT_BATCH_REDISPATCHES_CONSUMED` → `--only=d2_positive` exit 1；唯一 FAIL = `a fully consumed group advertises no residue` |
| RESTORED GREEN | 重建回执全绿；当前 BASE 二进制全绿（重建步骤本轮未重做） |
| 既有回归 | research / fault / submit_boundary / progress_race / flush_count / external_loop 全部 exit 0 |
| 清单 | `evidence/correctness/MANIFEST.sha256` 33/33 OK |

M3 的全量运行另有 `uring_transport.cpp:415` Class-A abort（exit 134）；那是同一缺陷的连带破坏，**不作 RED 见证**，RED 只取隔离目标场景。

---

## 5. 未测与不可观测（必须随结论读）

1. A/A 校准；A0/A1/B 任何配对计时；CPU/op、wall/op、吞吐、p50/p95/p99。
2. 负控家族（D1/D2/D3、64 KiB d16、W-03 d4/d16/d64、threadpool、saturate）与 Holm。
3. 性能保护门（吞吐 ≤3% 下降、p95/p99 ≤5% 恶化）。
4. 资源门（RSS ≤ +2%、零新增 queue/thread/timer/per-request allocation）。
5. `A_R2` / `B_R2`（无定义）；`OPEN-1` 未解除。
6. #595 主 shape 上的原型行为（W-02 未接 adapter）；**W-02 d16（冻结机制门的字面 shape）未在普查中覆盖**（本轮只有 W-02 d2/d4）。
7. cap>8、suffix 二次批量、d64 完整组重用。
8. 真实块设备、cold cache、O_DIRECT、其他 block size、非 tmpfs。
9. threadpool backend 的批量路径；跨线程并发 submit 与批量的交错。
10. #604 真实 consumer（fio/DuckDB/segment/WAL producer）与 `READY_FOR_595` handoff。
11. 第二台原生主机 / 跨机可移植性。
12. kernel/system CPU 归因、io-wq 机制、eventfd 内部阶段、指令级/微架构来源。
13. R2 历史数值的本轮复现。
14. stale generation 批量、pre-park wake with batch residue、overflow 期间批量 flush 的完整时序。

---

## 6. 不可变对象复核

| 对象 | 结果 |
|---|---|
| 主工作树 | `git status --porcelain` **0 行**；HEAD `0a22a791…` |
| 公开 async 头（5 个） | **未改**（`git diff 0a22a791 -- include/sluice/async/*.hpp` 为空） |
| `include/` 全部 | 仅内部 `detail/test_access.hpp`（+11）、`detail/uring_backend.hpp`（+40） |
| v2-r1 契约 | **未改**（`docs/explicit-io-contract.md`、`docs/roadmap/v2-conformance.md` diff 为空） |
| R2 归档 | 主树 `docs/research/595-causal-r2` clean；最后提交仍为 `0a22a791` |
| R1 原始证据 | `/home/jnhu/sluice-595-e5/evidence` 1854 文件，**0 个** mtime 晚于 2026-11-05 00:00 |
| R2 原始证据 | `/home/jnhu/sluice-595-e5/causal-r2`，**0 个**文件 mtime 晚于 2026-11-05 11:00 |
| 远端 | 未 push、未改 Issue/PR、未发评论；#595 OPEN、#604 OPEN、PR 625 OPEN；#595 最新 comment 仍为 R2 |

交付缺陷（不掩盖）：

- 旧 `evidence/MANIFEST.sha256`（22 项）在正确性轮后 3 项失败；旧清单保留为 `MANIFEST.sha256.pre-delivery`，本轮已重生成新清单（246 项，`sha256sum -c` 通过）。
- 评论正文现由主清单直接覆盖，并有独立 `MANIFEST.comment.sha256`；主清单哈希记录在 `MANIFEST.sha256.hash`（评论只引用路径，不内嵌哈希，避免循环依赖）。
- `evidence/r3.patch` 已过期、不能反向应用；最终态由 `r3-final.patch`（10 文件，+2883/−51，reverse check exit 0）捕获。
- 正确性运行未持 `MEASUREMENT.lock`（自述）；因无计时数值，不影响性能证据。
- `READY_EXPOSURE.md` §0 的 `STATE_AUTHORITY.md` 前置读取记录已由 §9 更正。

---

## 7. 裁决与下一步

**MECHANISM_VERDICT = `MECHANISM_ONLY`；PRODUCT_VERDICT = `KEEP_IMMEDIATE`；PRODUCT_CANDIDATE_READY = NO。**

词表说明：`MECHANISM_ONLY` 是本轮裁决字段，**不在 issue #595 §7 的 closeout 词表内**；映射到该词表为 **`KEEP_IMMEDIATE`**（机制层获得 syscall 级支持，产品层未成立）。不把 `MECHANISM_ONLY` 当作 closeout 词使用。

理由：产品门需要“真实任务/可辩护代表 trace + 退化保护 + 正确性 + `B−A0` 数值”同时成立；本轮只有正确性成立。主 shape 是 `STARTUP_ONLY`（`NO_REAL_BATCH_EXPOSURE`），真实批暴露只在 W-03（合成 benchmark）；原型还有 cap/suffix 协调成本、B 绕过 context、A0 含共同准备开销三项残差；所有性能/保护/资源门未测。

下一阶段触发条件（建议）：

1. 先写 R3R/R4 预注册：冻结 `A0/A1/A_R2/B_R2` 定义、二进制映射、24 对、AB/BA、bootstrap seed、A/A 5/3/3 pp、负控家族、停手条件。
2. 同 host、同二进制完成 A/A；失败按规则只修一次，仍失败则该指标 `INCONCLUSIVE_NOISE`。
3. 完成 A0/A1/B 24 对，报 whole-executor/whole-system CPU/op（submitter + kernel + io-wq；SQPOLL 含 poller，披露未计入 worker）与 p50/p95/p99/吞吐的配对差 + 95% CI。
4. 完成负控 + Holm、保护门、RSS/线程/分配门。
5. 解决 cap=8/suffix 协调成本（至少 cap=8/16/64 对照或 suffix 二次批量），并显式拆分 `B−A1` 的 context 绕过残差。
6. 若要支撑 #595 主 shape，必须证明 W-02 也能形成真实组；不得靠新增等待造组。
7. 若产品化，另开窄范围产品候选 PR；在此之前保持 `KEEP_IMMEDIATE`。

证据根：`/home/jnhu/sluice-595-e5/r3/evidence`；清单：`MANIFEST.sha256`（246 项；清单 sha256 见 `MANIFEST.sha256.hash`；评论正文哈希见 `MANIFEST.comment.sha256`；`sha256sum -c` 通过）。
