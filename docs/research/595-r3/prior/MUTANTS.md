# #595 R3 — 变异测试回执索引（MUTANTS）

| 字段 | 值 |
|---|---|
| 文档状态 | **DELIVERED**（2026-11-05，R3 裁决与交付轮） |
| 被测 seam | `UringAsyncBackend::submit_ready_reads`（`src/async/uring_backend.cpp:444`） |
| 测试 TU | `tests/uring_ready_batch_deterministic_test.cpp`（1400 行，sha256 `ef262452…`；副本 `correctness/`） |
| 变异方式 | 3 个编译期宏，同一测试 TU，逐目标只多一个 `-D` |
| 结论 | **BASE GREEN（13/13）→ M1/M2/M3 各自目标断言 RED → RESTORED GREEN** |
| 独立复核 | 本轮（交付轮）重跑 BASE 全量与三个隔离 RED，结果与回执一致；**未重做 rebuild 步骤** |

---

## 1. 回执总表（BASE GREEN → MUTANT RED → RESTORED GREEN）

| 步骤 | 二进制 | 命令 | 退出码 | 关键输出 | 回执 |
|---|---|---|---|---|---|
| BASE GREEN（全量） | `uring_ready_batch_deterministic_test` `0ecb27e2…` | `./…/uring_ready_batch_deterministic_test` | 0 | `all deterministic ready-batch oracles passed`，0 FAIL | `raw/correctness/base-full.log`；重复 5 次 `r3det-run-1..5.log` |
| BASE GREEN（目标 1） | 同上 | `--only=d2_positive` | 0 | 0 FAIL | `raw/correctness/base-d2_positive.log` |
| BASE GREEN（目标 2） | 同上 | `--only=sq_full_residue` | 0 | 0 FAIL | `raw/correctness/base-sq_full_residue.log` |
| BASE GREEN（目标 3） | 同上 | `--only=partial_submit` | 0 | 0 FAIL | `raw/correctness/base-partial_submit.log` |
| **M1 RED** | `uring_ready_batch_det_mut_includes_not_ready` `d15d5e4a…` | `--only=sq_full_residue` | **1** | 6 FAIL；首行 = `a batch at SQ-full transport residue accepts nothing` | `raw/correctness/mut1-includes-not-ready-only-sq-full.log` |
| **M2 RED** | `uring_ready_batch_det_mut_residue_dropped` `70f54690…` | `--only=partial_submit` | **1** | 唯一 FAIL = `partial kernel consumption advertises dispatch residue` | `raw/correctness/mut2-residue-dropped-only-partial.log` |
| **M3 RED** | `uring_ready_batch_det_mut_redispatch_consumed` `f0ff6e42…` | `--only=d2_positive` | **1** | 唯一 FAIL = `a fully consumed group advertises no residue` | `raw/correctness/mut3-redispatch-consumed-only-d2-positive.log` |
| RESTORED GREEN（重建） | `uring_ready_batch_deterministic_test` | `touch src/async/uring_backend.cpp && xmake build -y …` | 0 | `build ok` | `raw/correctness/rebuild-restored-base.log` |
| RESTORED GREEN（全量） | 重建后的 BASE | 同 BASE 全量 | 0 | 13/13，0 FAIL | `raw/correctness/restored-base-full.log` |
| RESTORED GREEN（目标 1/2/3） | 重建后的 BASE | 三个 `--only=` | 0/0/0 | 0 FAIL | `restored-base-{d2_positive,sq_full_residue,partial_submit}.log` |

二进制 SHA 原件：`raw/correctness/binary-sha256.txt`；逐目标宏证明：`raw/correctness/compile-defines.txt`。

---

## 2. 交付轮独立复核（本次实际执行）

```bash
cd /home/jnhu/sluice-595-e5/r3/worktree/build/linux/x86_64/release
sha256sum uring_ready_batch_deterministic_test uring_ready_batch_det_mut_*
./uring_ready_batch_deterministic_test
./uring_ready_batch_det_mut_includes_not_ready --only=sq_full_residue
./uring_ready_batch_det_mut_residue_dropped --only=partial_submit
./uring_ready_batch_det_mut_redispatch_consumed --only=d2_positive
```

| 检查 | 结果 |
|---|---|
| 四二进制 SHA | `0ecb27e2…` / `d15d5e4a…` / `f0ff6e42…` / `70f54690…`，与 `CORRECTNESS.md` §4 逐字一致 |
| BASE 全量 | exit **0**，`all deterministic ready-batch oracles passed`，`FAIL` 行数 **0** |
| M1 `--only=sq_full_residue` | exit **1**，6 条 FAIL；首行即目标断言 |
| M2 `--only=partial_submit` | exit **1**，1 条 FAIL = 目标断言 |
| M3 `--only=d2_positive` | exit **1**，1 条 FAIL = 目标断言 |
| `sha256sum -c evidence/correctness/MANIFEST.sha256`（在 `r3/` 下） | **33/33 OK** |

**未复核项**：RESTORED GREEN 的“强制重编”步骤本轮没有重新执行；只确认当前 BASE 二进制（即重建产物）全绿，且重建回执在清单内。

---

## 3. 三个变异的语义映射

### M1 — 把 transport 未就绪输入纳入组

- 宏：`SLUICE_R3_MUTANT_BATCH_INCLUDES_NOT_READY_TRANSPORT`（`src/async/uring_backend.cpp:525`）
- 注入：跳过 `dispatch_retry_remains_locked_()` 与 `io_uring_sq_space_left()==0` 门，limit 放宽为 `min(size, cap)`。
- 目标场景：`sq_full_residue`（test:882）
- 目标断言：`a batch at SQ-full transport residue accepts nothing`（test:912）
- 观察：BASE 在 SQ 满 + ledger 仍有 4 条未证明 SQE 时整组 untouched；M1 接受该组并 flush。
- 额外 FAIL（6 条）说明同一缺陷破坏了“stop=recovery / 整组 untouched / 不增 flush”三个相邻不变量。
- RED 见证：隔离目标场景 exit 1；全量 `mut1-includes-not-ready-full.log` 9 FAIL（仅记录）。

### M2 — 丢掉真实 residue 广告

- 宏：`SLUICE_R3_MUTANT_BATCH_RESIDUE_NOT_ADVERTISED`（`src/async/uring_backend.cpp:614`）
- 注入：`advertise = false`，partial-submit 残渣被当作已提交，不报告也不 signal。
- 目标场景：`partial_submit`（test:948）
- 目标断言：`partial kernel consumption advertises dispatch residue`（test:965）
- 观察：BASE 在 scripted `rc=1 < pending=2` 时 `residue_advertised=true`，下一 pass 重试剩余 ledger；M2 丢失该义务。
- RED 见证：唯一 FAIL 即目标；全量 `mut2-residue-dropped-full.log` 4 FAIL（仅记录）。

### M3 — 重复派发已消费 SQE

- 宏：`SLUICE_R3_MUTANT_BATCH_REDISPATCHES_CONSUMED`（`src/async/uring_backend.cpp:583`）
- 注入：`dispatch_one_locked` 成功后把同一个 reservation 重新 `push_back` 到 `dispatch_`。
- 目标场景：`d2_positive`（test:453）
- 目标断言：`a fully consumed group advertises no residue`（test:543）
- 观察：BASE 全消费组 `residue_advertised=false`；M3 因重复项仍报告 residue。
- RED 见证：隔离目标场景 exit 1，唯一 FAIL 即目标。
- **附带破坏（不作为 RED 见证）**：全量运行 `mut3-redispatch-consumed-full.log` 首行目标 FAIL 后，在 `uring_transport.cpp:415` 触发 `Class-A operation recovery lost terminal authority` abort（exit=134）。原因是 poison 恢复假设 `dispatch_` 只含“从未取过 SQE”的 reservation；重复项是已派发项，会被伪造 terminal，随后 Class-A 对同一 key 再 offer 时失去权威。该 abort 只说明缺陷影响更广，不替代命名断言。

源码摘录：`correctness/mutation-source-excerpts.txt`；锚点：`correctness/source-anchors.txt`。

---

## 4. 构建与运行命令（原始）

```bash
cd /home/jnhu/sluice-595-e5/r3/worktree
xmake build -y uring_ready_batch_deterministic_test \
    uring_ready_batch_det_mut_includes_not_ready \
    uring_ready_batch_det_mut_residue_dropped \
    uring_ready_batch_det_mut_redispatch_consumed

B=./build/linux/x86_64/release
$B/uring_ready_batch_deterministic_test
$B/uring_ready_batch_deterministic_test --only=d2_positive
$B/uring_ready_batch_deterministic_test --only=sq_full_residue
$B/uring_ready_batch_deterministic_test --only=partial_submit

$B/uring_ready_batch_det_mut_includes_not_ready --only=sq_full_residue
$B/uring_ready_batch_det_mut_residue_dropped --only=partial_submit
$B/uring_ready_batch_det_mut_redispatch_consumed --only=d2_positive
```

逐目标宏（`raw/correctness/compile-defines.txt`）：

| 目标 | 额外 `-D` |
|---|---|
| `uring_ready_batch_deterministic_test` | 无（只有 `SLUICE_ASYNC_INTERNAL_TESTING`、`SLUICE_HAS_LIBURING`） |
| `…_mut_includes_not_ready` | `SLUICE_R3_MUTANT_BATCH_INCLUDES_NOT_READY_TRANSPORT` |
| `…_mut_residue_dropped` | `SLUICE_R3_MUTANT_BATCH_RESIDUE_NOT_ADVERTISED` |
| `…_mut_redispatch_consumed` | `SLUICE_R3_MUTANT_BATCH_REDISPATCHES_CONSUMED` |

---

## 5. 回执索引（逐文件）

| 文件 | 内容 | 是否在 `correctness/MANIFEST.sha256` |
|---|---|---|
| `raw/correctness/base-full.log` | BASE 全量 GREEN | 是 |
| `raw/correctness/r3det-run-1..5.log` | BASE 连续 5 次 GREEN | 是 |
| `raw/correctness/base-{d2_positive,sq_full_residue,partial_submit}.log` | BASE 目标场景 GREEN | 是 |
| `raw/correctness/mut1-includes-not-ready-only-sq-full.log` | **M1 RED 见证** | 是 |
| `raw/correctness/mut2-residue-dropped-only-partial.log` | **M2 RED 见证** | 是 |
| `raw/correctness/mut3-redispatch-consumed-only-d2-positive.log` | **M3 RED 见证** | 是 |
| `raw/correctness/mut1-includes-not-ready-full.log` | M1 全量（9 FAIL，仅记录） | 是 |
| `raw/correctness/mut2-residue-dropped-full.log` | M2 全量（4 FAIL，仅记录） | 是 |
| `raw/correctness/mut3-redispatch-consumed-full.log` | M3 全量（目标 FAIL + abort 134，仅记录） | 是 |
| `raw/correctness/rebuild-restored-base.log` | 强制重编回执 | 是 |
| `raw/correctness/restored-base-full.log` | RESTORED 全量 GREEN | 是 |
| `raw/correctness/restored-base-{d2_positive,sq_full_residue,partial_submit}.log` | RESTORED 目标 GREEN | 是 |
| `raw/correctness/regression.log` | 6 个既有回归 | 是 |
| `raw/correctness/binary-sha256.txt` | 四二进制 SHA | 是 |
| `raw/correctness/compile-defines.txt` | 逐目标宏 | 是 |
| `raw/correctness/source-anchors.txt` | 宏/目标/场景行号 | 是 |
| `raw/correctness/mutation-source-excerpts.txt` | 宏所在代码摘录 | 是 |
| `raw/correctness/worktree-status.txt` | 主树 clean、R3 worktree 状态 | 是 |
| `raw/correctness/build-all-targets.log` | 构建日志 | 是 |

---

## 6. 限制

1. 三个变异都是**测试用编译期宏**，不是产品运行时开关；它们只验证测试的杀伤力，不验证生产配置。
2. M1 的“就绪”映射是后端可观测的就绪边界（transport 残渣 / SQ 预算 / control / eligibility / admission），不是调用者 ReadyGroup 条件本身；D1 场景另行用调用点结构证明依赖读不入组。
3. M3 的全量 abort 不作为 RED 见证；只用隔离目标场景。
4. 本轮没有重新执行“变异后重建 BASE”的步骤；RESTORED GREEN 依赖回执 + 当前 BASE 二进制全绿。
5. 这些正确性结果**不证明**性能收益，也不证明保护门通过。
