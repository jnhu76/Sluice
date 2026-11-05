# #595 R3 — 冻结协议、实际命令、偏差、无效轮次与仪器分离

| 字段 | 值 |
|---|---|
| 文档状态 | **DELIVERED**（2026-11-05，R3 裁决与交付轮） |
| 基线 | `baseSha = 0a22a79195f1b913145fcd443496e7f70b08cdcf`；隔离 worktree `/home/jnhu/sluice-595-e5/r3/worktree`（分支 `research-595-r3`，HEAD 同 baseSha，工作树脏） |
| 宿主 | E5；Fedora 44；kernel `7.2.11-200.fc44.x86_64`；Xeon E5-2666 v3 @ 2.90GHz（20 logical / SMT on） |
| 本轮性质 | **只做证据汇总、独立复核与裁决；未新增任何计时轮、未改产品代码、未改远端** |
| 核心事实 | **R3 没有执行任何 A/A、A/B 或负控计时**。`PREREGISTERED_R3.md` 与 `measurement-{main,product,mechanism,analysis}/` 在证据根中**不存在**（已逐路径核验）。因此 R3 不能回答任何 CPU/时延/吞吐的配对问题。 |
| 结论词汇 | 产品/closeout 词表：`KEEP_IMMEDIATE`；本轮机制标签 `MECHANISM_ONLY` 是裁决字段，映射到 issue #595 §7 词表为 `KEEP_IMMEDIATE`（机制层支持、产品未成立），不冒充 closeout 词。另有 `NO_REAL_BATCH_EXPOSURE` 描述 headline shape。详见 `VERDICT.md` §3.3 |

本文件描述的是**实际发生了什么**，不是原本计划要发生什么。凡计划存在而未执行者，一律标注 `NOT_RUN`。

---

## 0. 证据根与交付边界

| 对象 | 路径 | 本轮是否写入 |
|---|---|---|
| 证据根 | `/home/jnhu/sluice-595-e5/r3/evidence` | 是（本文件及同轮交付文档） |
| 原始记录 | `/home/jnhu/sluice-595-e5/r3/raw` | 否（只读） |
| 脚本 | `/home/jnhu/sluice-595-e5/r3/harness` | 否（只读） |
| 日志 | `/home/jnhu/sluice-595-e5/r3/logs` | 否（只读） |
| 隔离 worktree | `/home/jnhu/sluice-595-e5/r3/worktree` | 否（只读；仅生成过一次 `git diff` 到证据目录） |
| 主工作树 | `/home/jnhu/Source/Sluice` | **否**（`git status --porcelain` 空，复核见 §5） |
| R1 原始证据 | `/home/jnhu/sluice-595-e5/evidence/**`（1854 文件） | 否（0 个文件 mtime 晚于 2026-11-05 00:00） |
| R2 归档 | 主树 `docs/research/595-causal-r2/**`；`/home/jnhu/sluice-595-e5/causal-r2/**` | 否（git clean；0 个文件 mtime 晚于 2026-11-05 11:00） |

---

## 1. 冻结协议（R3 本应执行的测量协议）

来源：`evidence/CONTEXT.md` §8、`evidence/EVIDENCE_BASELINE.md` §2.2/§2.3/§3.1、`/home/jnhu/sluice-595-e5/evidence/measurement/PREREGISTERED.md:41-136`。

| 参数 | 冻结值 | 本轮执行状态 |
|---|---|---|
| 配对单位 | 24 对 / phase | **NOT_RUN** |
| 每 invocation | `WARMUP=1`、`MEASURED=5` | **NOT_RUN** |
| 绑核 | `taskset -c 2`（并须处理 SMT 兄弟 cpu12） | **NOT_RUN** |
| 存储 | warm tmpfs（`/tmp`，32 GiB） | 仅普查用到 `/tmp` fixture；无计时 |
| 臂序 | AB/BA 交替 | **NOT_RUN** |
| 统计 | bootstrap 10k，seed 5950 | **NOT_RUN**（脚本不存在） |
| A/A 门 | CPU / tput / p99 半宽 ≤ 5 / 3 / 3 pp | **NOT_RUN** |
| 负控门 | 单侧 CPU/p95/p99 ≤5%，吞吐下降 ≤3%，Holm–Bonferroni 家族 | **NOT_RUN** |
| 产品净胜门 | whole-executor CPU/op 点估计 ≤ −10%，95% 上界 ≤ −5% | **NOT_RUN**（无 B−A0 数据） |
| 资源门 | 零新增 queue/thread/timer/per-request allocation；peak RSS ≤ +2% | **NOT_RUN** |
| 正确性门 | 全部 mandatory oracles + `BASE GREEN → NAMED MUTANT RED → RESTORED GREEN` | **已执行**（见 §2.5、`correctness/CORRECTNESS.md`、`MUTANTS.md`） |

### 1.1 R3 实际采用的臂标签与状态

`CONTEXT.md` §7.2 `OPEN-1` 明确记录：`A0` / `A1` / `A_R2` / `B_R2` 的干预定义在冻结时**未给出**，阻断正式计时。`BUILD.md` §3.4 后来把 `A0` / `A1` / `B` 定义为**研究标签**，并明确不是冻结臂定义：

| 标签 | 实际含义（BUILD.md §3.4） | 是否冻结 | 是否计时 |
|---|---|---|---|
| `A0` | 原始 W-03 路径，不构造/不使用 adapter | 否 | 否 |
| `A1` | 同一 adapter，逐条调用 `AsyncIoContext::submit_read`（与 A0 同一 public 路径） | 否 | 否 |
| `B` | 同一 adapter，整组调用一次 `submit_ready_reads`；suffix 逐条 span=1 | 否 | 否 |
| `A_R2` | 无定义 | — | 否 |
| `B_R2` | 无定义 | — | 否 |

`OPEN-1` 在本轮结束时**仍未解除**。任何把 `A0/A1/B` 当作冻结臂的引用都不成立。

---

## 2. 实际执行的命令与阶段

### 2.1 平台冻结（PLATFORM.md，2026-11-05T11:46–11:50）

```bash
lscpu
grep -i "model name" /proc/cpuinfo | sort -u
uname -a
g++ --version; xmake --version; python3 --version; pkg-config --modversion liburing
cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor
cat /proc/sys/kernel/perf_event_paranoid
cat /proc/sys/kernel/unprivileged_bpf_disabled
df -h /tmp; mount | grep -E ' on / | on /tmp | on /home | on /dev/shm '
cat /sys/devices/system/cpu/cpu2/topology/thread_siblings_list
```

结果：原生 E5 判定 PASS；`schedutil`（20/20）；SMT active 且 cpu2 兄弟为 cpu12；`perf_event_paranoid=2`；`unprivileged_bpf_disabled=2`；`/tmp` 为 32 GiB tmpfs。未改任何 governor/turbo/SMT/THP/隔离设置。

### 2.2 基线冻结与构建（BASELINE.md）

```bash
git -C /home/jnhu/Source/Sluice rev-parse HEAD            # 0a22a791…
git -C /home/jnhu/Source/Sluice status --porcelain        # 0 行
git -C /home/jnhu/Source/Sluice worktree add \
  /home/jnhu/sluice-595-e5/r3/worktree -b research-595-r3 0a22a791…
cd /home/jnhu/sluice-595-e5/r3/worktree
xmake f -c -y --liburing=y -m release
xmake build bench_w02_pipeline bench_w03_external_loop sluice-copy uring_submit_boundary_test
```

三次构建（含两次 `xmake clean` 后重建）四个二进制 SHA 一致，判定 DETERMINISTIC。**偏差 P-1**：ask 原文形式 `xmake -P <worktree> f …` 在本机 xmake v3.1.2 下不可用（`f` 被当作 target），且 `-P` 不改变 builddir 解析；最终采用 `cd <worktree> && xmake f … && xmake build …`。首次误落 `/tmp/build` 已删除。

### 2.3 就绪组机会普查（READY_EXPOSURE.md / census.json）

- 诊断实现：`bench/census_diag.hpp`（header-only，运行时开关），补丁 `raw/census/diagnostic/bench-diagnostics.patch`。
- 17 条 canonical 运行（`raw/census/canonical/run.log`），形态：

```bash
env SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 \
    SLUICE_BENCH_CASE_FILTER='=<case>' \
    SLUICE_BENCH_TMP_DIR=/tmp/sluice-census-fixture SLUICE_BENCH_SKIP_DISK=1 \
    SLUICE_CENSUS_D2=1 SLUICE_CENSUS_VERBOSE=<0|1> SLUICE_CENSUS_MINIMAL=<0|1> \
    SLUICE_CENSUS_REQUESTS=<20000|512> SLUICE_CENSUS_LOG=<tsv> \
    flock /home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock \
    strace -f (-ttt|-c) -e trace=io_uring_enter -o <trace> <bench>
```

- 独立 oracle：`strace` 的 `io_uring_enter` 计数，与诊断 header 的记录分开。
- 诊断构建与冻结基线的 `libsluice_async.a` 逐字节相同（`76e59bd6…`）；诊断二进制另存 `raw/census/bin/*.diagnostic`；运行结束后 worktree 源文件还原、`git status` 0 行（census 阶段）。

### 2.4 原型实现与 smoke（BUILD.md）

```bash
cd /home/jnhu/sluice-595-e5/r3/worktree
xmake f -m release --liburing=y
xmake build -y sluice_async uring_ready_batch_research_test uring_ready_batch_fault_test \
    bench_w03_external_loop bench_w02_pipeline sluice-copy \
    uring_submit_boundary_test uring_progress_race_test uring_flush_count_oracle_test \
    uring_external_loop_test uring_public_consumer_probe
# 默认 profile（无 liburing）
xmake f -m release --builddir=/tmp/r3-nolib && xmake build -y sluice_async ...
```

三臂 smoke（**非计时**）：

```bash
env SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 \
    SLUICE_BENCH_CASE_FILTER="=<case_id>" SLUICE_595_R3_CASES=1 \
    SLUICE_595_R3_ARM=<A0|A1|B> SLUICE_BENCH_STATS=1 \
    SLUICE_BENCH_TMP_DIR=/tmp/sluice-r3-smoke SLUICE_BENCH_SKIP_DISK=1 \
    ./build/linux/x86_64/release/bench_w03_external_loop
```

本交付轮又独立重跑了三条 B 臂 smoke（d4/d16/d64）与 A0/A1 d4 smoke，均在 `flock MEASUREMENT.lock` 下，结果见 `RESULTS.md` §3.3。这些是单次、无配对、无 CI 的 smoke，**不是性能证据**。

### 2.5 确定性正确性与变异（correctness/）

```bash
cd /home/jnhu/sluice-595-e5/r3/worktree
xmake build -y uring_ready_batch_deterministic_test
./build/linux/x86_64/release/uring_ready_batch_deterministic_test
./build/linux/x86_64/release/uring_ready_batch_deterministic_test --only={d2_positive,sq_full_residue,partial_submit}
xmake build -y uring_ready_batch_det_mut_includes_not_ready \
    uring_ready_batch_det_mut_residue_dropped uring_ready_batch_det_mut_redispatch_consumed
./build/linux/x86_64/release/uring_ready_batch_det_mut_includes_not_ready --only=sq_full_residue
./build/linux/x86_64/release/uring_ready_batch_det_mut_residue_dropped --only=partial_submit
./build/linux/x86_64/release/uring_ready_batch_det_mut_redispatch_consumed --only=d2_positive
touch src/async/uring_backend.cpp && xmake build -y uring_ready_batch_deterministic_test
./build/linux/x86_64/release/uring_ready_batch_deterministic_test
```

完整清单见 `correctness/commands.txt`；回执见 `raw/correctness/*.log`。

**本交付轮独立重跑（未重建）**：

```bash
cd /home/jnhu/sluice-595-e5/r3/worktree/build/linux/x86_64/release
./uring_ready_batch_deterministic_test                       # exit 0，0 FAIL
./uring_ready_batch_det_mut_includes_not_ready --only=sq_full_residue   # exit 1，6 FAIL
./uring_ready_batch_det_mut_residue_dropped --only=partial_submit       # exit 1，1 FAIL
./uring_ready_batch_det_mut_redispatch_consumed --only=d2_positive      # exit 1，1 FAIL
./uring_ready_batch_research_test  ./uring_ready_batch_fault_test \
./uring_submit_boundary_test ./uring_progress_race_test \
./uring_flush_count_oracle_test ./uring_external_loop_test               # 全部 exit 0
```

四个二进制 SHA 与 `correctness/CORRECTNESS.md` §4 完全一致；`sha256sum -c evidence/correctness/MANIFEST.sha256`（在 `r3/` 下执行）33/33 OK。**RESTORED GREEN 的“重新编译”步骤本轮没有重做**，只复核了重建产物的当前运行结果与回执。

### 2.6 本交付轮的只读复核命令

```bash
git -C /home/jnhu/Source/Sluice status --porcelain                        # 0 行
git -C /home/jnhu/sluice-595-e5/r3/worktree status --porcelain            # 6 M + 4 ??
git -C /home/jnhu/sluice-595-e5/r3/worktree diff --stat 0a22a791          # 6 files, +514/−51（不含 4 个未跟踪文件）
git -C /home/jnhu/sluice-595-e5/r3/worktree diff --stat 0a22a791 -- include/sluice/async/*.hpp  # 空
git -C /home/jnhu/sluice-595-e5/r3/worktree diff --stat 0a22a791 -- docs/explicit-io-contract.md docs/roadmap/v2-conformance.md  # 空
sha256sum -c evidence/MANIFEST.sha256.pre-delivery                        # 19 OK / 3 FAILED（交付前旧清单，见 §3）
sha256sum -c evidence/correctness/MANIFEST.sha256                         # 33/33 OK
sha256sum evidence/r3.patch                                               # a9eeeac5…
git apply --reverse --check evidence/r3.patch                             # FAIL（已过期）
git apply --reverse --check evidence/r3-final.patch                       # exit 0（最终态）
gh issue view 595 --json number,state,updatedAt                           # OPEN
gh api repos/jnhu76/Sluice/issues/595/comments --jq '.[-1]'               # 仍为 R2 comment
```

---

## 3. 偏差与缺陷（必须随结论一起读）

| # | 偏差 | 证据 | 影响 |
|---|---|---|---|
| **P-1** | `xmake -P <dir> f …` 不可用；builddir 相对 CWD 解析 | `BASELINE.md` §4.2 | 最终构建在 worktree 内完成；首次 `/tmp/build` 产物已删除，未计入复现证据 |
| **P-2** | **`PREREGISTERED_R3.md` 不存在** | `find /home/jnhu/sluice-595-e5 -iname "*PREREGISTERED*"` 仅命中 R1 的 `evidence/measurement/PREREGISTERED.md` | R3 的测量假设、臂定义、门槛与停手条件**没有预注册**；任何事后统计都不具备预注册效力 |
| **P-3** | **`measurement-main` / `measurement-product` / `measurement-mechanism` / `measurement-analysis` 均不存在** | 逐路径 `test -e` 全部 MISSING；`find` 无同名目录 | R3 从未进入计时；`RESULTS.md` 的配对表只能全部为 `NOT_RUN` |
| **P-4** | `OPEN-1` 未解除：`A0/A1/A_R2/B_R2` 无冻结定义；`BUILD.md` 只给了研究标签 `A0/A1/B` | `CONTEXT.md` §7.2；`BUILD.md` §3.4 | 即使有计时，也不能把 `B−A0` 当作冻结臂差；本轮更无计时 |
| **P-5** | 正确性测试运行**未持有** `MEASUREMENT.lock` | `correctness/CORRECTNESS.md` §6.3 自述 | 与 `CONTEXT.md` §9 字面要求偏差；因无计时数值，不影响性能证据，但属于协议偏离 |
| **P-6** | 交付前的 `evidence/MANIFEST.sha256`（22 项）有 **3 项校验失败**；本轮已重生成清单 | 交付前 `sha256sum -c`：`libsluice_async.a`（现 `2aeff83c…` vs 旧清单 `797bb132…`）、`uring_ready_batch_research_test`（现 `e16d1379…` vs `9b2719a0…`）、`uring_ready_batch_fault_test`（现 `88c74188…` vs `dc5826c1…`）。旧清单保留为 `MANIFEST.sha256.pre-delivery`；新清单覆盖当前态并通过 | 旧清单在正确性轮重建后未刷新；`BUILD.md` §1 “22/22 OK”是历史状态 |
| **P-7** | `evidence/r3.patch` **不再能反向应用** | `git apply --reverse --check` → `xmake/tests.lua` 与 `src/async/uring_backend.cpp` 均 patch does not apply | 旧 patch 只覆盖实现轮（9 文件 +1436/−51），不含正确性轮的确定性测试与 3 个变异宏；最终态由本轮新增的 `r3-final.patch`（10 文件 +2883/−51，reverse check exit 0）捕获 |
| **P-8** | `READY_EXPOSURE.md` §0 曾称 `STATE_AUTHORITY.md` 不存在 | `READY_EXPOSURE.md` §9 已追加更正 | 已由原文末尾更正，结论不受影响 |
| **P-9** | R1 文档内吞吐符号约定不一致（drop 号 vs B 更差号） | `EVIDENCE_BASELINE.md` §2.0 | 本文件统一采用 §7 约定；引用 R1/R2 时先归一化 |
| **P-10** | `BUILD.md` §7 smoke 计数与本轮回执不同（d16：`groups=1332/singles=9622/flushes=10954` vs 回执 1274/9866/11140） | `BUILD.md` §7；`raw/smoke/run.log`、`raw/smoke/b-{d4,d16,d64}.log` | 组形成受调度影响，**smoke 计数不是确定性常量**；只能作量级/机制说明 |
| **P-11** | `CONTEXT.md` §13 称“本轮未运行任何计时”，但 `BUILD.md`/smoke 运行了带 `MEASURED=1` 的 bench | `CONTEXT.md` §13；`BUILD.md` §7 | 这些运行是 smoke（单次、无配对、无 A/A），不是计时证据；本文件不把它们计入 A/B |

---

## 4. 无效轮次与剔除记录

| 类别 | 内容 | 处理 |
|---|---|---|
| 探索性普查 | `raw/census/exploratory/**`（早期运行、旧窗口） | 不作 canonical 依据；canonical 只取 `raw/census/canonical/`、`raw/census/frozen/`、`raw/census/analysis/` |
| M3 全量运行 | `raw/correctness/mut3-redispatch-consumed-full.log`：首行目标 FAIL，随后 `uring_transport.cpp:415` Class-A abort（exit=134） | **不作 RED 见证**；RED 见证使用隔离目标场景 `--only=d2_positive`（exit=1） |
| 变异全量运行（M1/M2） | 9 FAIL / 4 FAIL，exit=1 | 仅记录，不作命名断言见证 |
| 首次误落构建 | `/tmp/build`（`xmake -P` 解析导致） | 已 `rm -rf`；未记录 SHA，不计入复现性证据 |
| A/A 轮 | **不存在** | 无失败轮可剔除；也无通过轮可引用 |
| A/B 轮 | **不存在** | — |
| 计时轮 | **不存在** | — |
| 性能离群点 | **不存在** | 未做计时，无离群点处理规则被触发 |

---

## 5. 仪器分离（instrument separation）

| 观测 | 仪器 | 是否独立于被测机制 | 本轮用途 |
|---|---|---|---|
| 每次 submit 的 `io_uring_enter(to_submit, min_complete)` | `strace -f -e trace=io_uring_enter`（行级 `-ttt`，计数 `-c`） | 是（内核 syscall 边界，不经产品计数） | 基线 enter 数、合发检查、诊断开销对照 |
| 组员身份 / 组大小 / 组内顺序 | `bench/census_diag.hpp` 诊断记录（TSV） | 半独立：与 `submit_read` 参数逐项比对，但由 bench 代码记录 | 分布、成员身份、别名/依赖检查 |
| 每次 enter 的 `sq_ready` 与 SQE opcode | `UringBackendSubmitTestHooks`（`src/async/uring_test_seams.hpp`） | 是（测试 seam，委托真实 `io_uring_submit`） | 确定性 oracle 的 enter/SQE 观察 |
| 内核部分消费 / poison | `scripted_submit`（同一 seam） | 是（脚本化 fake kernel） | partial submit、residue、poison 场景 |
| 产品统计 | `SLUICE_BENCH_STATS=1`（`AsyncStats`） | 否（被测代码内计数，仅诊断） | smoke 的 groups/singles/flushes 计数 |
| progress 通知 | `ProgressSource::notification_fd()` 直读 | 是（eventfd 直接 `read(2)`） | 断言 “无新增 progress 写入” |
| progress epoch | `TestAccess::context_progress_token` | 是（测试 seam） | immediate 路径不推进 epoch |

分离结论：

1. 普查的 enter 结论以 `strace` 为准；诊断记录只用于组结构与分布。同一诊断构建在记录开/关时 enter 数完全相同（60,004），冻结基线亦为 60,004。
2. 正确性场景的 enter/SQE 观察来自测试 seam，不是产品 `AsyncStats`。
3. **性能计时与仪器从未分离，因为性能计时根本没有发生**。`BUILD.md` 的 smoke 表同时含 wall/cpu 与 `SLUICE_BENCH_STATS`，只能作机制计数说明；本文件明确禁止把其 wall/cpu 当性能结论。
4. 未使用 xlayer/双仪器；R2 的 xlayer 规则（`EVIDENCE_BASELINE.md` F8）本轮不适用。

---

## 6. 统计与预注册状态

| 项 | 状态 |
|---|---|
| 预注册文件 | **MISSING**（P-2） |
| A/A 校准 | **NOT_RUN** |
| 配对 bootstrap（seed 5950，10k） | **NOT_RUN**；无脚本、无数据 |
| 负控家族（D1/D2/D3、64 KiB d16、W-03 d4/d16/d64、threadpool、saturate） | **NOT_RUN** |
| 性能保护门（吞吐 ≤3% 下降、p95/p99 ≤5% 恶化） | **NOT_ASSESSED** |
| 资源门（RSS +2%、零新增结构） | **NOT_MEASURED** |
| 真实 consumer 计时（#604 G0/G3） | **NOT_RUN**；#604 未 handoff `READY_FOR_595` |

因此：

- 不存在任何可引用的 R3 CPU/时延/吞吐点估计或 CI；
- 不存在 `B−A0`、`B−A1` 的数值；
- 不能判定产品净胜门通过或不通过，只能判定**未被检验**；
- 本轮的机制结论只能来自 syscall enter 计数与确定性正确性，不能来自性能数字。

---

## 7. 本文件采用的符号约定

- CPU/op、p50/p95/p99：`(B−A)/A`，**正 = B 更差**（更慢/更高延迟）。
- 吞吐：`(A−B)/A`，**正 = B 更慢**（B 更快时为负）。
- 进入计数：`enter` 专指 `io_uring_enter`；`纯 submit enter` = `to_submit>0 && min_complete==0`；`context probe` = `min_complete==1`。
- `B` 在 R2 历史数字中指 R2 的 BOUNDED_PROGRESS 臂；在 R3 中指 `BUILD.md` 的批量原型臂。两者**不是同一实现**，不得混用。
- 引用 R1/R2 时先按其原文符号归一化到上述约定；不得把 `−3.58%` 与 `+3.58%` 当作两个结果。

---

## 8. 复核与复现命令（本交付轮实际执行）

```bash
# 不可变对象
git -C /home/jnhu/Source/Sluice status --porcelain
git -C /home/jnhu/Source/Sluice rev-parse HEAD
git -C /home/jnhu/sluice-595-e5/r3/worktree status --porcelain
git -C /home/jnhu/sluice-595-e5/r3/worktree diff --stat 0a22a791 -- include/sluice/async/*.hpp
git -C /home/jnhu/sluice-595-e5/r3/worktree diff --stat 0a22a791 -- docs/explicit-io-contract.md

# 清单
cd /home/jnhu/sluice-595-e5/r3 && sha256sum -c evidence/correctness/MANIFEST.sha256
cd /home/jnhu/sluice-595-e5/r3/evidence && sha256sum -c MANIFEST.sha256

# 正确性
cd /home/jnhu/sluice-595-e5/r3/worktree/build/linux/x86_64/release
./uring_ready_batch_deterministic_test
./uring_ready_batch_det_mut_includes_not_ready --only=sq_full_residue
./uring_ready_batch_det_mut_residue_dropped --only=partial_submit
./uring_ready_batch_det_mut_redispatch_consumed --only=d2_positive

# 远端只读
gh issue view 595 --json number,state,updatedAt
gh issue view 604 --json number,state,updatedAt
gh pr view 625 --json number,state
```

---

## 9. 未做事项（本文件不提供）

1. 未做 A/A、A/B、负控、保护门、资源门、真实 consumer 计时。
2. 未生成 `PREREGISTERED_R3.md`，也未补写事后门槛；不把 R1 的 `PREREGISTERED.md` 冒充 R3 预注册。
3. 未重跑 RESTORED GREEN 的重建步骤（只复核当前 BASE 二进制与回执）。
4. 未重新生成 `BUILD.md` 的 smoke 数字（除本轮独立重跑的三条 B 臂与 A0/A1 d4）。
5. 未修改任何产品代码、公开头、v2-r1 文档、R1/R2 证据、远端或 Issue。
