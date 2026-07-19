# PR #13 Corrective-1 独立对抗评审与 Corrective-2 纠正记录

审查对象：`jnhu76/Sluice#13`

审查日期：2026-07-19（Asia/Shanghai）

本 artifact 同时记录两件不同的事：对 `a97aca1b` 的独立评审结论，以及在该结论之后实施的 Corrective-2 作者自评。后者不是独立复审，不能授权 semantic closure。

## A. Verdict

```text
E10-E12-ASYNC-SYNC-API-SEMANTIC-CLOSURE-CORRECTIVE-1-REVIEW:
REQUEST-CHANGES

E10-E12-ASYNC-SYNC-API-SEMANTIC-CLOSURE-CORRECTIVE-2:
PASS — AUTHOR SELF-ASSESSMENT
INDEPENDENT RE-REVIEW:
REQUIRED
```

Corrective-1 的阻塞问题已在测试、文档和验证 gate 范围内纠正；生产实现和 formal spec 均未修改。由于纠正者与本次复审者是同一主体，最终 closure 仍然拒绝，等待新的独立复审。

## B. Baseline and scope

```text
BASE_BRANCH: master
BASE_COMMIT: d0cd915159a49ee30e88b0fdaec04a7b78260af1
INITIAL_REVIEW_HEAD: a97aca1bb9248a2c5bc05d914ba8670de590ed34
HEAD_BRANCH: audit/e10-e12-api-semantic-closure
COMMITS_REVIEWED_AT_INITIAL_HEAD: 1
FILES_CHANGED_AT_INITIAL_HEAD: 14
FINAL_HEAD: ebd91eb55b7432e381751f14ab7c0bee81139291 (resolve from final handoff)
PREEXISTING_UNTRACKED: tests/test_t3_simple.cpp, tla2tools.jar
PRODUCTION_DIFF: EMPTY
FORMAL_SPEC_DIFF: EMPTY
```

开始审查时的 worktree：

```text
?? tests/test_t3_simple.cpp
?? tla2tools.jar
```

两者均为用户已有文件；Corrective-2 没有读取、改写、暂存或提交 `tests/test_t3_simple.cpp`，`tla2tools.jar` 仅作为用户要求的 TLC runtime 输入使用，未改写或提交。

初始 `base...HEAD` 的 14 个文件为：

```text
M docs/api-reference-zh.md
M docs/api-reference.md
M docs/async-runtime-plan.md
M docs/changelog.md
A docs/e10-e12-api-semantic-closure.md
M docs/e12-condition.md
M docs/e12-queue-scheduler-integration.md
A docs/reviews/E10-E12-ASYNC-SYNC-API-SEMANTIC-CLOSURE-1-REVIEW-REQUEST.md
A tests/e12_api_contract_probes.cpp
M tests/e12_async_condition_test.cpp
M tests/e12_async_queue_test.cpp
A tests/e12_cross_primitive_parity_test.cpp
M tests/e12_event_test.cpp
M xmake.lua
```

初始 `base...HEAD` stat：3423 insertions、21 deletions。开始和完成时均执行四条强制 scope 命令；`git diff d0cd915...HEAD -- include src docs/spec` 为空。另执行包含 Corrective-2 working tree 的 `git diff d0cd915 -- include src docs/spec`，同样为空。

环境：

```text
OS: Ubuntu 26.04 LTS (WSL2)
KERNEL: Linux 6.18.33.2-microsoft-standard-WSL2
ARCH: x86_64
CLANG_VERSION: Ubuntu clang 21.1.8 (6ubuntu1)
GCC_VERSION: g++ 15.2.0-16ubuntu1
XMAKE_VERSION: 3.0.9+HEAD.2b184e1
JAVA_VERSION: OpenJDK 25.0.3
TLC_JAR: /home/hoo/Projects/io-core/tla2tools.jar
TLC_JAR_SHA256: 936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88
TLC_VERSION: TLC2 2.19 of 08 August 2024 (rev: 5a47802)
```

## C. Findings

### P0

无。初始 HEAD 和 Corrective-2 都没有触及 `include/**`、`src/**` 或 `docs/spec/**`。

### P1

#### C2-F01

```text
ID: C2-F01
SEVERITY: P1
FILE: docs/reviews/E10-E12-ASYNC-SYNC-API-SEMANTIC-CLOSURE-1-REVIEW-REQUEST.md
LINE: initial HEAD 127-135
CLAIM: T23 已把 suspended/yield 替换成 waiting_count() >= 4。
ACTUAL: initial HEAD tests/e12_event_test.cpp:1281-1319 仍先发布 suspended，再 yield，并使用固定 200/100 次 retry。
IMPACT: review request 要求评审不存在的修复，且把概率性调度描述为确定性证明。
REQUIRED_FIX: 使请求、源码和因果协议一致。
STATUS: RESOLVED IN CORRECTIVE-2。
```

#### C2-F02

```text
ID: C2-F02
SEVERITY: P1
FILE: tests/e12_event_test.cpp
LINE: initial HEAD 1281-1319
CLAIM: suspended >= 4 与 bounded retry 足以证明四个 WaitNode 已注册。
ACTUAL: flag 在 wait()/wait_until() 前发布；三个 worker 下 OS 可在发布与注册之间抢占。retry bound 由机器速度而非状态机决定。另一个更深缺陷是每轮都使用绝对 deadline 100 并重复 advance_clock(100)，第二轮起 W2 在 admission 时已到期，根本没有完成注册 timed wait。
IMPACT: T23 的注册与 timer-race 证据均为虚假 closure，正确性依赖调度运气。
REQUIRED_FIX: 使用 Scheduler-authoritative registration gate，删除 retry/yield，并使用单调增加的绝对 deadline。
STATUS: RESOLVED IN CORRECTIVE-2；当前源码等待 waiting_count() >= 4，每轮 deadline=(it+1)*100，之后仅执行一次 expire/cancel/set。
```

#### C2-F03

```text
ID: C2-F03
SEVERITY: P1
FILE: tests/e12_event_test.cpp
LINE: initial HEAD 1543-1595
CLAIM: CANCEL-2a 的 release/acquire 修复已完全确定性关闭竞态。
ACTUAL: cancel_done 的发布顺序本身正确，但 registered 在 ev_a.wait() 前发布，随后以 1000 次 poll/yield 猜测注册完成。
IMPACT: wrong-object cancel 可能在注册前完成观察，或 bounded loop 随机器速度耗尽。
REQUIRED_FIX: 先用 waiting_count() >= 1 证明 registration committed，再执行错误 Event cancel；保留 cancel_done release/acquire 对 set 的排序。
STATUS: RESOLVED IN CORRECTIVE-2；Debug 50/50 与 ASan 50/50 均无失败。
```

#### C2-F04

```text
ID: C2-F04
SEVERITY: P1
FILE: docs/e10-e12-api-semantic-closure.md
LINE: initial HEAD 1151-1153
CLAIM: Queue 没有 cancel API，但 cancellation surface 是 close() + deadline expired。
ACTUAL: close 是独立的单调 Queue 状态迁移；deadline expiry 是独立终态操作结果；Queue v1 没有 Cancelled 结果。二者都不是 cancellation surface。
IMPACT: 权威文档自相矛盾，错误定义 Queue 公共合约。
REQUIRED_FIX: 全仓统一 no public wait-epoch cancellation API/no Cancelled result 词汇。
STATUS: RESOLVED IN CORRECTIVE-2。
```

#### C2-F05

```text
ID: C2-F05
SEVERITY: P1
FILE: docs/e10-e12-api-semantic-closure.md; docs/reviews/E12-C-REVIEW.md
LINE: closure initial HEAD 1356; review artifact current 360-376
CLAIM: E12-C independent review PASS，因此 E12-C CLOSED。
ACTUAL: 历史 artifact 保留了 reviewer 的 `PASS (corrective applied)` verdict，同时在末尾继续要求 final migration data-race micro-review；仓库中没有找到 Corrective-4 后可单独归属的独立 re-review artifact。这是治理状态冲突，不授权 Corrective-2 改写历史 reviewer 文字。
IMPACT: 若只采信历史 PASS 行会把尚未留存独立复审证据的 Corrective-4 提升为 CLOSED；若直接把原 PASS 改成 REVIEW-REQUIRED，又会制造新的历史。
REQUIRED_FIX: 原 artifact 仅追加带日期、reviewed commit 和 authority 的 supersession note，历史 verdict 保持原样；当前状态由 closure/status ledger 记录为 IMPLEMENTATION COMPLETE / REVIEW-REQUIRED。
STATUS: RESOLVED IN CORRECTIVE-2；历史原文已恢复，治理分类与历史 verdict 明确分层。
```

#### C2-F06

```text
ID: C2-F06
SEVERITY: P1
FILE: tests/e12_cross_primitive_parity_test.cpp; xmake.lua; closure
LINE: initial HEAD test 4-30, 181-331; xmake 1112-1119; closure 1171-1177
CLAIM: 新 TU 在 Event/Semaphore/AsyncMutex/AsyncCondition 间统一证明 D3/D7/D8，包括 absorbing/exactly-once terminal publication。
ACTUAL: 新 TU 的动态测试只有 Event/Semaphore/AsyncMutex；Condition 与 Queue 不在该 TU。枚举数值互异和 fresh unresolved 不能证明吸收终态或并发 exactly-once publication。原命名还把 cancellation 错标成 D7、terminal vocabulary 错标成 D8；closure 的真实 D4 才是 cancellation，D7 是 fairness，D8 是 error classification。
IMPACT: 测试覆盖和 decision traceability 被过度声明。
REQUIRED_FIX: 按 PROVEN_BY_THIS_NEW_TU / EXISTING_PER_PRIMITIVE / DOCUMENTED_ONLY / N/A 分层，并修正 decision 标签。
STATUS: RESOLVED IN CORRECTIVE-2；新 TU 仅声明 D3、D4 与静态 vocabulary/fresh-node 证据。
```

#### C2-F07

```text
ID: C2-F07
SEVERITY: P1
FILE: tests/e12_api_contract_probes.cpp; xmake.lua
LINE: initial HEAD xmake 1088-1110
CLAIM: NEG_* blocks 是完整 negative-compile 测试证据。
ACTUAL: xmake 只普通编译未定义宏的正向 TU；九个 NEG_* 分支没有逐一执行。closure 仅称 spot-verified。
IMPACT: 未自动执行的分支被计入 closure matrix。
REQUIRED_FIX: 定义每个宏单独编译、要求失败、成功即 gate 失败，并检查失败原因。
STATUS: RESOLVED IN CORRECTIVE-2；新增 scripts/verify-e12-api-contract-negative-compile.sh，Clang/GCC 均 9/9。
```

#### C2-F08

```text
ID: C2-F08
SEVERITY: P1
FILE: docs/e10-e12-api-semantic-closure.md; docs/api-reference.md; docs/api-reference-zh.md
LINE: public inventory sections
CLAIM: API inventory 与安装头文件精确一致。
ACTUAL: initial HEAD 遗漏 WaitNode user/set_user 及 public intrusive fields；遗漏公开可命名 WaitQueue/TimerRegistration surface；Queue teardown 签名未准确显示 detail::QueueTeardownSession；部分 defaulted destructor、result public members 和模板约束归属不完整。
IMPACT: 权威 API 合约不完整，不能支撑 semantic closure。
REQUIRED_FIX: 逐签名同步 return/parameter/const/noexcept/nodiscard/default/visibility/destructor/template constraints。
STATUS: RESOLVED IN CORRECTIVE-2，并由头文件逐项复核。
```

### P2

#### C2-F09

```text
ID: C2-F09
SEVERITY: P2
FILE: xmake.lua / matrix summaries
LINE: configured E10-E12 targets
CLAIM: 完整矩阵是 14/14。
ACTUAL: 当前 xmake 配置有 15 个 E10/E11/E12 binary，包括两个 probe targets。
IMPACT: 汇总数量漂移，可能漏报一个目标。
REQUIRED_FIX: 报告全部 15 个 binary，不把 14 当作权威数量。
STATUS: RESOLVED IN CORRECTIVE-2 artifact/request/closure。
```

#### C2-F10

```text
ID: C2-F10
SEVERITY: P2
FILE: scripts/verify-e11-formal.sh; scripts/verify-e12-event-formal.sh
LINE: named_violation helpers
CLAIM: 当前 TLC runtime 会在 liveness negative output 中打印被违反的属性名。
ACTUAL: 两个模型均生成非 vacuous stuttering temporal counterexample，但 TLC 只输出通用 Temporal properties were violated，脚本因找不到属性名而 exit 1。
IMPACT: E11/Event formal gate 不能被写成普通 PASS；也不能在没有 baseline 对照时归因工具。
REQUIRED_FIX: 保留完整输出、matched baseline、零 spec/config diff，并按限制分类；formal spec 修改被本任务禁止。
STATUS: PREEXISTING-BASELINE-PARITY-PROVEN；仍需未来独立 formal/tooling corrective 才能使两个脚本 exit 0。
```

### P3

一处 review request 语法 `an compile-time` 已改为 `a compile-time`，无语义影响。

## D. 先验风险裁决

```text
F1: CONFIRMED
F2: SUPERSEDED BY DEEPER FINDING
F3: PARTIALLY CONFIRMED
F4: CONFIRMED
F5: CONFIRMED
F6: CONFIRMED
F7: CONFIRMED
F8: REJECTED
```

具体结论：

```text
T23_REVIEW_REQUEST_MATCHES_ACTUAL_CODE:
INITIAL_HEAD: NO
CORRECTIVE-2: YES

T23_REGISTRATION_CAUSALLY_PROVEN:
INITIAL_HEAD: NO
CORRECTIVE-2: YES

T23_CORRECTNESS_DEPENDS_ON_SCHEDULING_LUCK:
INITIAL_HEAD: YES
CORRECTIVE-2: NO

QUEUE_CANCELLATION_VOCABULARY_CONSISTENT:
INITIAL_HEAD: NO
CORRECTIVE-2: YES

E12_C_ACTUAL_STATUS:
IMPLEMENTATION COMPLETE — INDEPENDENT IMPLEMENTATION REVIEW REQUIRED
E12_C_REVIEW_ARTIFACT:
docs/reviews/E12-C-REVIEW.md
E12_C_STATUS_CONTRADICTIONS:
The historical artifact records PASS (corrective applied) and also says to
await the final migration data-race micro-review; no separately attributable
post-Corrective-4 independent re-review artifact was found. Corrective-2 keeps
that history intact and records REVIEW-REQUIRED only in the later governance
ledger/supersession note.

GEMINI_EVENT_FINDING: REJECT
GEMINI_SEMAPHORE_FINDING: REJECT
GEMINI_MUTEX_FINDING: REJECT
```

Gemini 的 Event 评论指向 single-worker cross-primitive parity test，不是
three-worker T23 stress test。三个被评论的 wrong-object case 都使用
`sched.run(1)`：waiter 发布 pre-call flag 后继续在唯一 worker 上执行，
Event::wait、Semaphore::acquire 或 AsyncMutex::lock 在发生 Fiber context
switch 前完成 register、waiting accounting 和 `make_waiting()`。OS 可以
抢占该 worker 线程，但不能在同一 worker 上凭空并发执行另一个 Fiber；
测试中也没有介于 flag store 与 blocking call 之间的显式 yield。因此三条
Gemini review thread 对其具体测试均不成立。它指出的一般风险模式只适用于
T23 一类 multi-worker topology，不能反向用于接受这三条具体评论。

## E. C1–C7 disposition

下表是对 initial review HEAD 的独立裁决；Evidence 列同时说明 Corrective-2 作者纠正状态。

| Corrective | Verdict | Evidence |
| ---------- | ------- | -------- |
| C1 | REJECTED | initial T23/CANCEL-2a 均没有 registration causal edge；Corrective-2 改用 waiting_count gate，并修复绝对 deadline，定向 stress 全绿。 |
| C2 | PROVEN | 文档明确 primitive cancel 仅 per-queue best effort、不是 Select loser authority；parent/group winner claim 必须先于不可逆 Mutex owner/Queue item commit；单 global_mtx_ 首期限制同 Scheduler；cross-Scheduler 需 coordinator；Condition mandatory reacquire 被排除或需专门设计。未发现仍把 winner-then-cancel-losers 当作充分协议的权威文本。 |
| C3 | REJECTED | initial R2/R5 与 E12-C status 不诚实；Corrective-2 从真实 artifact 重建 E12-B/C/D。E12-C 历史 PASS 与仍待 micro-review 的 next action 均保留，later ledger 保守记录 REVIEW-REQUIRED；R2 fresh formal exit 0，R5 由 plan 实文关闭。 |
| C4 | PROVEN | 文档正确区分 deleted copy/move 的 identity/address 作用与 absorbing state/Detached-only registration 的 fresh-per-epoch runtime enforcement。 |
| C5 | REJECTED | initial closure 仍称 Queue close/deadline 为 cancellation surface；Corrective-2 已统一词汇。 |
| C6 | PARTIAL | WaitOutcome 终态分类正确，substrate 术语正确；initial public visibility inventory 不完整。Corrective-2 已补齐 WaitQueue/TimerRegistration public type surface。 |
| C7 | PROVEN | 源码实现 Condition outcome 先 latch，随后 mandatory untimed/non-cancellable Mutex reacquire，ownership 恢复后返回；already-due inline 不释放 Mutex；registration failure 不重复释放/重获。 |

## F. Test matrix

### 构建与全量 binary 命令

```bash
xmake f -c -m debug --toolchain=clang -o /tmp/sluice-c2-clang-debug-20260719
xmake build <each-target>
/tmp/sluice-c2-clang-debug-20260719/linux/x86_64/debug/<each-target>

xmake f -c -m debug --toolchain=gcc -o /tmp/sluice-c2-gcc-debug-20260719
xmake build <each-target>
/tmp/sluice-c2-gcc-debug-20260719/linux/x86_64/debug/<each-target>

xmake f -c -m asan --toolchain=clang -o /tmp/sluice-c2-clang-asan-20260719
xmake build <each-target>
/tmp/sluice-c2-clang-asan-20260719/linux/x86_64/asan/<each-target>

xmake f -c -m tsan --toolchain=clang -o /tmp/sluice-c2-clang-tsan-20260719
xmake build <each-target>
TSAN_OPTIONS=halt_on_error=1 /tmp/sluice-c2-clang-tsan-20260719/linux/x86_64/tsan/<each-target>
```

`<each-target>` 是下表全部 15 项。表中数字为 process exit code；A1/A2/A3 是无 filter、无 skip、无 xfail、无 `|| true`、不禁用 LSan 的沙箱外连续三轮。

| Binary | Clang Debug | GCC Debug | ASan A1 | ASan A2 | ASan A3 | TSan |
|---|---:|---:|---:|---:|---:|---:|
| e10_wait_queue_test | 0 | 0 | 0 | 0 | 0 | 0 |
| e10_scheduler_wait_test | 0 | 0 | 0 | 0 | 0 | 0 |
| e10_corrective_c1_test | 0 | 0 | 0 | 0 | 0 | 0 |
| e10_corrective_c2_c3_test | 0 | 0 | 0 | 0 | 0 | 0 |
| e10_corrective_c5_test | 0 | 0 | 0 | 0 | 0 | 0 |
| e11_timer_wait_test | 0 | 0 | 0 | 0 | 0 | 0 |
| e12_event_test | 0 | 0 | 0 | 0 | 0 | 0 |
| e12_semaphore_test | 0 | 0 | 0 | 0 | 0 | 0 |
| e12_async_mutex_test | 0 | 0 | 0 | 0 | 0 | 0 |
| e12_async_condition_test | 0 | 0 | 0 | 0 | 0 | 0 |
| e12_async_queue_test | 0 | 0 | 0 | 0 | 0 | 0 |
| e12_async_mutex_death_test | 0 | 0 | 0 | 0 | 0 | 0 |
| e12_async_mutex_nothrow_authority_probe | 0 | 0 | 0 | 0 | 0 | 0 |
| e12_api_contract_probes | 0 | 0 | 0 | 0 | 0 | 0 |
| e12_cross_primitive_parity_test | 0 | 0 | 0 | 0 | 0 | 0 |

正式验收矩阵与额外诊断观察严格分开：

```text
AUTHOR ACCEPTANCE MATRIX:
unsandboxed ASan+LSan 15/15 ×3 (45/45, failures 0)

ADDITIONAL SANDBOX OBSERVATION:
address-only 44/45, with one T16 DEADLYSIGNAL;
baseline parity independently reproduced under the same sandbox setup.
```

Clang Debug 15/15、GCC Debug 15/15、上述 unsandboxed Clang ASan+LSan
15/15 ×3、Clang TSan 15/15；正式矩阵 failure count 均为 0。TSan 分类是
PASS，无 DATA_RACE、ASSERTION_FAILURE、SEMANTIC_FAILURE、DEADLYSIGNAL
或 TIMEOUT。沙箱观察不被合并进“ASan 全绿”表述。

### 所有额外失败记录

不能用正式矩阵的成功覆盖以下先前运行：

1. 沙箱内、leak detection 开启的 ASan preflight 启动全部 15 个 binary；测试体均运行，但每个 process 最后因 LeakSanitizer 无法访问 `ptrace`/`/proc` 非零退出：15 launches、15 failures。后续沙箱外同 binaries + LSan 45/45，证明这是受限执行环境，不是 target exclusion。
2. 沙箱内 `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` 三轮为 15/15、15/15、14/15；第三轮 unfiltered `e12_event_test` 在 T16 出现 wild-jump `AddressSanitizer:DEADLYSIGNAL`，共 45 launches、1 failure。
3. baseline 相同 Event binary/环境/worker/filter（无 filter）三轮为：T16 同类 DEADLYSIGNAL、原 CANCEL-2a `node.is_registered()` assertion failure、PASS。HEAD 未扩大 T16 失败类别，并消除了 baseline 的 CANCEL-2a 失败；T16 分类为 PREEXISTING-BASELINE-PARITY-PROVEN。
4. 第一次沙箱外矩阵 shell 命令误用 zsh 标量列表，未启动任何 test、命令 exit 3；修正为 zsh array 后才得到上表真实 45 次。该项是 harness invocation error，不是测试失败，仍在本记录中保留。

### 定向 stress

命令模板：

```bash
for i in {1..50}; do
  SLUICE_TEST_FILTER=<allowlist> <binary>
done
```

| Mode | Binary / allowlist | Runs | Exit nonzero | Failures |
|---|---|---:|---:|---:|
| Clang Debug | Event / e12_cancel2a_wrong_event_same_scheduler_loses_safely | 50 | 0 | 0 |
| Clang Debug | Event / e12_t23_multi_waiter_mixed_outcome_stress | 50 | 0 | 0 |
| Clang ASan | Event / e12_cancel2a_wrong_event_same_scheduler_loses_safely | 50 | 0 | 0 |
| Clang ASan | Event / e12_t23_multi_waiter_mixed_outcome_stress | 50 | 0 | 0 |
| Clang ASan | entire e12_cross_primitive_parity_test | 50 | 0 | 0 |
| Clang ASan | Condition / e12_cond_t30_cancel_detached_node_returns_false,e12_cond_t31_cancel_wrong_condition_returns_false | 50 | 0 | 0 |

最终 T23/CANCEL-2a 各 50 次在沙箱外启用 ASan+LSan；较早的 parity/Condition 各 50 次使用 `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`，AddressSanitizer 保持启用而 LSan 因沙箱权限明确关闭。完整三轮矩阵另在沙箱外启用 LSan，未用定向结果替代全量 Event target。

### Production release build

```bash
xmake f -c -m release --toolchain=clang -o /tmp/sluice-c2-clang-release-20260719
xmake build sluice_core
xmake build sluice_async
xmake build sluice_async_internal_testing
```

| Target | Exit | Artifact | Size |
|---|---:|---|---:|
| sluice_core | 0 | libsluice_core.a | 106566 |
| sluice_async | 0 | libsluice_async.a | 241116 |
| sluice_async_internal_testing | 0 | libsluice_async_internal_testing.a | 279042 |

三个目标均完成编译和归档，无新增 warning；测试依赖没有进入 public production API，internal testing target 仍为独立依赖。

### Negative-compile gate

```bash
CXX=clang++ scripts/verify-e12-api-contract-negative-compile.sh
CXX=g++ scripts/verify-e12-api-contract-negative-compile.sh
```

两条命令 exit 0。每条先正向 syntax compile exit 0，再分别定义下列九个宏；每个 compile 均非零且包含 deleted-member diagnostic，unexpected success 为 0：

```text
NEG_WAITNODE_COPY
NEG_WAITNODE_MOVE
NEG_EVENT_COPY
NEG_SEMAPHORE_MOVE
NEG_ASYNCMUTEX_COPY
NEG_ASYNCCONDITION_MOVE
NEG_QUEUE_COPY
NEG_QUEUE_PUSH_RESULT_COPY
NEG_QUEUE_POP_RESULT_COPY
```

## G. Formal verification

```text
FORMAL EXPECTATION MATRIX:
4/6 EXPECTED GATES PASS
2/6 BASELINE-PARITY-PROVEN TOOLCHAIN-SENSITIVE LIMITATIONS
0 HEAD-ONLY REGRESSIONS
```

所有有效 TLC 命令都在沙箱外运行，因为沙箱内第一次 E11 命令因 TLC 本地 RMI `Listen failed on port: 0 / Operation not permitted` exit 1；该执行环境失败没有被误计为模型结论。

| Target | Command | Exit | Expected | Actual | Classification |
|---|---|---:|---|---|---|
| E11 TimerWait | `TLA2TOOLS_JAR=/home/hoo/Projects/io-core/tla2tools.jar CXX=clang++ scripts/verify-e11-formal.sh` | 1 | correct safety/liveness PASS；NEG-1..6 命名属性 violation | correct 两项 PASS；NEG-1..4/6 CEX；NEG-5 有 temporal stutter CEX 但没有属性名 | PREEXISTING TOOLCHAIN-SENSITIVE LIMITATION — BASELINE PARITY PROVEN |
| E12 Event | `TLA2TOOLS_JAR=/home/hoo/Projects/io-core/tla2tools.jar CXX=clang++ scripts/verify-e12-event-formal.sh` | 1 | correct safety/liveness PASS；NEG-EVENT-1..4 命名 violation；wrong-property/compile probe PASS | correct PASS；NEG-1/3/4 CEX；NEG-2 有 temporal stutter CEX 但没有属性名；其余 gates PASS | PREEXISTING TOOLCHAIN-SENSITIVE LIMITATION — BASELINE PARITY PROVEN |
| E12 Semaphore | `TLA2TOOLS_JAR=/home/hoo/Projects/io-core/tla2tools.jar CXX=clang++ scripts/verify-e12-semaphore-formal.sh` | 0 | correct safety + NEG-1..7 + wrong-property + compile probe | 全部期望结果 | PASS — EXPECTED VIOLATIONS REPRODUCED |
| E12 AsyncMutex | `TLA2TOOLS_JAR=/home/hoo/Projects/io-core/tla2tools.jar CXX=clang++ scripts/verify-e12-async-mutex-formal.sh` | 0 | correct safety + NEG-M1..M11 + wrong-property + compile probe | 全部期望结果 | PASS — EXPECTED VIOLATIONS REPRODUCED |
| E12 AsyncCondition | `TLA2TOOLS_JAR=/home/hoo/Projects/io-core/tla2tools.jar CXX=clang++ scripts/verify-e12-async-condition-formal.sh` | 0 | correct safety + 2 reachability + NEG-C1..C10 + wrong-property + 7 compile probes | 全部期望结果 | PASS — EXPECTED VIOLATIONS REPRODUCED |
| E12 Queue | `TLA2TOOLS_JAR=/home/hoo/Projects/io-core/tla2tools.jar TLC_WORKERS=1 scripts/verify-e12-queue-formal.sh` | 0 | Model A/B + NEG-QUEUE-1..7 + wrong-property | 全部期望结果 | PASS — EXPECTED VIOLATIONS REPRODUCED |

E11 NEG-5 与 Event NEG-EVENT-2 的完整当前/baseline TLC 输出已保存在：

| Log | Lines | SHA-256 |
|---|---:|---|
| `/tmp/sluice-c2-head-e11-neg5-full.log` | 134 | `995ed9f7e2503d28c1b6f59759aa7454c936838fe48c4ee9d845947755e8f0d4` |
| `/tmp/sluice-c2-base-e11-neg5-full.log` | 98 | `b8019987e811251e89784e96538d22515337a0814bf7a55e1517e435cd37d14b` |
| `/tmp/sluice-c2-head-event-neg2-full.log` | 122 | `fb88be3bdb5b6413a8fd1d9718f70740661369e6febed306292ccd30a1a83f7c` |
| `/tmp/sluice-c2-base-event-neg2-full.log` | 122 | `ae4a434de9c962708d86eb21a8785546ffdf5fa06bab777bfe582d0eeddbb15b` |

分析：两个 config 都只检查目标 `PROPERTY`。轨迹先到达 Registered/Suspended 且资源条件成立的状态，再进入 Stuttering，目标 terminal 永远不出现；因此 antecedent 非空、不是 vacuity，negative model 能杀死 wake-one/deadline-lost mutation。失败点仅是这个 TLC runtime 不在通用 temporal violation 文本中打印 property name，而脚本有意要求命名诊断。`git diff d0cd915...a97aca1 -- docs/spec` 为空，两个 verify script 也没有 PR diff；baseline 相同命令、jar、worker、filter、环境重现同一 exit/classification。故不得写普通 formal PASS，但可写 baseline-parity-proven tooling limitation。

## H. Files modified by corrective

| File | Why |
|---|---|
| tests/e12_event_test.cpp | T23/CANCEL-2a registration causal gate；T23 绝对 deadline；删除 machine-speed retry/yield。 |
| tests/e12_cross_primitive_parity_test.cpp | 收窄为新 TU 真正证明的 Event/Semaphore/AsyncMutex D3/D4 与静态 vocabulary/fresh-node。 |
| tests/e12_api_contract_probes.cpp | 修正 decision/证据标签，保留九个独立 negative branches。 |
| scripts/verify-e12-api-contract-negative-compile.sh | 自动逐宏要求编译失败并检查 deleted-member 原因。 |
| xmake.lua | 区分普通正向 probe target 与独立 negative gate；修正 parity scope 注释。 |
| docs/e10-e12-api-semantic-closure.md | 纠正 C1-C7、API inventory、Queue/E12-C/parity/negative evidence、fresh matrix/formal/residual risks。 |
| docs/api-reference.md | 补齐精确英文 public type/signature/visibility/template-constraint inventory。 |
| docs/api-reference-zh.md | 同步中文精确 public inventory。 |
| docs/e12-condition.md | E12-B/C/D status 与真实 artifact 对齐。 |
| docs/e12-async-mutex.md | 把依赖说明中的 E12-B CLOSED 收窄为 preparation closed / implementation review-required。 |
| docs/reviews/E12-C-REVIEW.md | 仅追加带日期、reviewed commit 和 authority 的 supersession note；历史 reviewer verdict 与文字保持原样，当前 REVIEW-REQUIRED 分类留在 later governance ledger。 |
| docs/reviews/E12-D-CONDITION-PREPARATION-AUDIT-1.md | 给历史 E12-C CLOSED 前提增加 supersession notice，避免被当作当前 verdict。 |
| docs/reviews/E10-E12-ASYNC-SYNC-API-SEMANTIC-CLOSURE-1-REVIEW-REQUEST.md | 请求与实际 Corrective-2 代码、范围、matrix、formal 结果一致。 |
| 本 artifact | 持久化独立 finding、纠正、所有失败与最终自评。 |

未修改：`include/**`、`src/**`、`docs/spec/**`、任何生产 API/实现、任何 formal model/config、E13 Select 实现、两个预存 untracked 文件。

## I. Residual risks

| Risk | State | Independent reconstruction |
|---|---|---|
| R-C2-1 E12-B independent implementation review | OPEN-NON-BLOCKING for this corrective / blocking for E12-B closure | 没有独立 implementation review artifact。 |
| R-C2-2 E12-C migration data-race micro-review | OPEN-NON-BLOCKING for this corrective / blocking for E12-C closure | 历史 artifact 同时保留 PASS 与 await-final-micro-review；没有找到 Corrective-4 后可单独归属的独立 re-review artifact，later ledger 因而保守记录 REVIEW-REQUIRED。 |
| R-C2-3 E12-D preparation + implementation review | OPEN-NON-BLOCKING for this corrective / blocking for E12-D closure | formal gate exit 0 不替代独立 adversarial review。 |
| R-C2-4 E11/Event named-liveness gate | PREEXISTING-BASELINE-PARITY-PROVEN | negative model 非 vacuous，但两个脚本在该 TLC runtime 下 exit 1；不能称 6/6 ordinary PASS。 |
| R-C2-5 Event T16 ASan wild jump | PREEXISTING-BASELINE-PARITY-PROVEN | HEAD 和 baseline 都出现相同 raw-fiber/worker wild jump；HEAD 没扩大范围，正式沙箱外 45/45 通过，但早先失败保留。 |
| R-C2-6 Queue timer allocation failure | OPEN-NON-BLOCKING | allocation failure 可能导致 wait 没有 timer；本 PR 禁止生产修改。 |
| R-C2-7 E13 Select contract realization | OPEN-NON-BLOCKING | contract 已正确记录，实际设计仍需 parent/group claim、commit ordering、same-Scheduler first scope；未开始实现。 |
| R-C2-8 Corrective-2 independent re-review | OPEN-BLOCKING | 同一主体不能把自己的修复判为 independent PASS。 |
| R2 stale AsyncCondition formal evidence | CLOSED | fresh AsyncCondition script exit 0，包括所有 negative/reachability/wrong-property/compile probes。 |
| R5 stale plan ordering/status | CLOSED | `docs/async-runtime-plan.md` 实文与 dependency trunk/status 对齐。 |

## J. Final closure decision

```text
CORRECTIVE READY FOR INDEPENDENT RE-REVIEW
SEMANTIC CLOSURE STILL DENIED PENDING RE-REVIEW
```

不输出 `SEMANTIC CLOSURE AUTHORIZED`。独立复审者必须从最终 branch tip 重跑范围检查、至少复现 Clang Debug、ASan ×3、TSan、六个 formal 分类，并确认本 artifact 的 corrective diff 才能另行授权。
