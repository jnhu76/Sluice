# #595 R3 原型实现记录（BUILD）

| 字段 | 值 |
|---|---|
| 状态 | **buildOk = true / semanticsOk = true（实现者自检；最终以 verifier 为准）** |
| 角色 | R3 原型实现者（独立审阅修复轮） |
| 基线 | `baseSha = 0a22a79195f1b913145fcd443496e7f70b08cdcf`（worktree HEAD，与 `CONTEXT.md` §2 一致） |
| 隔离 worktree | `/home/jnhu/sluice-595-e5/r3/worktree`（分支 `research-595-r3`） |
| 主工作树 | `/home/jnhu/Source/Sluice`：本轮结束 `git status --porcelain` **0 行** |
| 公开 API / v2-r1 | 未改动 `include/sluice/async/*.hpp`；只改内部 detail 头、`src/`、`bench/`、`tests/`、`xmake/` |
| 计时 | **本轮未做计时**；所有运行都是 smoke/语义 oracle，数字不是性能结论 |
| 形式计时门 | `CONTEXT.md` §7.2 `OPEN-1` **仍未解除**。`A0/A1/B` 是本原型的研究标签，不是已冻结臂定义；不得据本文件进入正式计时 |

## 1. 交付物

| 交付物 | 路径 | 摘要 |
|---|---|---|
| 最小 patch | `/home/jnhu/sluice-595-e5/r3/evidence/r3.patch` | sha256 `a9eeeac56acf34a3f9857993058b319df5b2e01b4f01643aead98b776e4cf63f` |
| patch 规模 | 9 文件，`+1436 / −51`（`git apply --stat`） | 3 个新文件：`bench/r3_ready_batch.hpp`、`tests/uring_ready_batch_research_test.cpp`、`tests/uring_ready_batch_fault_test.cpp` |
| 本文件 | `/home/jnhu/sluice-595-e5/r3/evidence/BUILD.md` | 构建/运行/证据命令 |
| 摘要清单 | `/home/jnhu/sluice-595-e5/r3/evidence/MANIFEST.sha256` | 已按 R3 重建刷新（22 项，`sha256sum -c` 全 OK）；基线旧值保留在 `BASELINE.md` §5 |

```bash
cd /home/jnhu/sluice-595-e5/r3/worktree
sha256sum /home/jnhu/sluice-595-e5/r3/evidence/r3.patch
# a9eeeac56acf34a3f9857993058b319df5b2e01b4f01643aead98b776e4cf63f
git apply --reverse --check /home/jnhu/sluice-595-e5/r3/evidence/r3.patch   # exit 0
sha256sum -c /home/jnhu/sluice-595-e5/r3/evidence/MANIFEST.sha256           # 22/22 OK
```

## 2. 入口点（research-only，不是公开 API）

```cpp
// include/sluice/async/detail/uring_backend.hpp:92-115（无条件声明，两种 profile 都可见）
UringAsyncBackend::ReadyReadBatchReport
UringAsyncBackend::submit_ready_reads(std::span<const ReadOp> ready) noexcept;

static constexpr std::size_t ready_batch_cap = 8;
enum class ReadyBatchStop { input_exhausted, rejected, cap, sq_budget, control,
                            recovery, ineligible };
struct ReadyReadBatchReport {
    std::size_t accepted;         // 已接受前缀长度
    std::size_t untouched_begin;  // == accepted；stop==rejected 时为 accepted+1
    std::size_t untouched;        // untouched_begin 之后的元素数
    ReadyBatchStop stop;
    IoError error;                // stop == rejected 时有效
    std::array<detail::RequestKey, ready_batch_cap> keys;  // accepted 前缀逐条 key
    std::size_t flush_attempts;
    bool residue_advertised;
};
```

实现与调用点：

| 位置 | 作用 |
|---|---|
| `src/async/uring_backend.cpp:444` | `submit_ready_reads` 定义；`!SLUICE_HAS_LIBURING` stub 在 `:73`（同一签名，返回 recovery/backend_error） |
| `src/async/uring_backend.cpp:626` | `admit_()`：从原 `submit_request` 抽出的 admit/prepare/accept 半段；新增 `dispatch_locked` 参数 |
| `src/async/uring_backend.cpp:614` | `reset_delivery_locked_()`：持有 `dispatch_mtx_` 时的 delivery 复位 |
| `src/async/uring_backend.cpp:721` | `submit_request()` = `admit_()` + 原 zero-op/dispatch 尾部；单条语义不变 |
| `src/async/uring_backend.cpp:383` | `retire_execution_resources()` 在 `dispatch_mtx_` 下重查 `have_ring_`、置 false、`io_uring_queue_exit` |
| `bench/r3_ready_batch.hpp:84` | 研究 adapter（A1/B 共用） |
| `bench/bench_w03_external_loop.cpp:372` | consumer 调用点（W-03 refill burst；请求容器按 depth 预留一次） |
| `tests/uring_ready_batch_research_test.cpp:446` | 非 seam oracle 入口（8 个场景） |
| `tests/uring_ready_batch_fault_test.cpp:324` | seam fault oracle 入口（partial submit / poison / control） |
| `xmake/tests.lua:582,584` | 两个研究 oracle 目标 |

## 3. 机制（只改提交分组，不改语义权威）

### 3.1 批量算法（修复后：接受与派发在同一 `dispatch_mtx_` 临界区）

1. **入口先查 `have_ring_`**；已退休的 ring 直接返回 `recovery/backend_error`，不触碰 `ring_state_->ring`。
2. **单条或首条不具资格**（`ready.size()==1 || !batch_eligible_read(head)`，资格 = `read && len==4096 && dst!=nullptr`）：直接调用原 `admit_()` + `dispatch_after_accept()`（或 zero-op 内联发布），即现有 immediate 路径；其余元素报为 untouched（`stop=ineligible`）。
3. 批量路径取 **一次** `dispatch_mtx_`，在同一临界区内完成：
   - 重查 `have_ring_`；
   - 检查既有边界：`fatal_error_`、`pending_control_count_ != 0`、`dispatch_retry_remains_locked_()`、`io_uring_sq_space_left()==0`；任一成立则不 accept，整组 untouched；
   - 前缀上限 = `min(ready.size(), 8, SQ 空闲)`；逐条调用 `admit_(..., dispatch_locked=true)`（validate → `core_->reserve` → prepare → `core_->accept`），`reserve` 失败即停止并报 `rejected`；
   - 接受前缀按输入顺序 `push_back` 到既有 `dispatch_`，用既有 `dispatch_one_locked` 逐条写**独立 SQE**；
   - 本次调用**只调用一次** `submit_transport_locked()`，随后既有 `service_pending_controls_locked_()`。
4. 解锁后，若有 accepted 则更新 `max_outstanding`；`fatal_error_` 或 `dispatch_retry_remains_locked_()` 为真时 `signal_ready_progress()`，残渣由既有 ledger/retry/readiness 路径处理。
5. **不跨调用持有 SQE**：accept → 准备 → 一次 flush 全部在本次调用内完成；没有 `bounded_progress_` 式 defer。

### 3.2 disposition 契约（修复 #10）

- `accepted` 是**连续前缀**，`keys[0..accepted)` 是逐条 disposition；
- `stop == rejected` 时，被尝试但失败的条目位于 `untouched_begin - 1 == accepted`；它既不是 accepted，也不在 untouched 统计内；
- 其余 stop（cap/sq_budget/control/recovery/ineligible）的 `untouched_begin == accepted`；
- 调用者从 `untouched_begin` 继续；`accepted` 与 `untouched` 不重叠，已接受请求不会出现在 suffix。

### 3.3 保留的不变量

- 接受权威仍是 `RequestCore::accept()`；批量只改变“何时 flush”。
- 每条请求保留独立 SQE / Request / CQE / 错误 / cancel / result / borrow；没有合并请求。
- 不在 backend 锁内运行应用回调、不等待未来请求；Request 构造在锁外。
- cancel / control / overflow / poison / shutdown 规则未改；已存在 control intent 或残渣时批量主动停止。
- 不新增 timer/thread/queue/每请求分配；report 是调用者栈上的 `std::array`（≤8）。

### 3.4 三臂（同二进制、运行期选择；研究标签，不是冻结臂）

| 臂 | 定义 | 实现 |
|---|---|---|
| **A0** | 原始 W-03 路径，不构造/不使用 adapter | consumer 的 `ctx.submit_read` 分支 |
| **A1** | 同一 adapter，逐条调用 **`AsyncIoContext::submit_read`**（与 A0 相同的 public 调用路径：precheck + `access_mtx_` + `tally_submit`） | `bench/r3_ready_batch.hpp:99` |
| **B** | 同一 adapter，整组调用一次 `submit_ready_reads`；cap/预算/control/recovery/ineligible 停止后，只把 `untouched_begin` 之后的 suffix 逐条走 span=1 | `bench/r3_ready_batch.hpp:116` |

**归因披露（修复 #4/#6）**：

- A1 现在与 A0 走**同一** context 调用路径，A0−A1 只余 adapter 的簿记开销（不改变语义路径）。
- B 通过内部 detail seam 直接调用 backend entry，因此**绕过** `AsyncIoContext::submit_read` 的 `initiation_rejection` 与 `access_mtx_`。`admit_` 仍执行 backend 级 `validate_op`，但 B−A1 同时包含“批量分组”和“context 层 precheck/mutex 被绕过”两部分，不能把差值全部归因于 enter 合并；B 也不是无需新公开契约即可被真实 consumer 调用的产品路径。任何正式计时必须显式拆分该残差，且需先解 `CONTEXT.md` §7.2 `OPEN-1`。
- 调用者提供 `std::vector<Request>` 并在 phase 内 `reserve(depth)` 一次；`Adapter::submit` 只 `clear()`，A1/B 都不再每调用 heap 分配。

运行期开关：

```bash
SLUICE_595_R3_ARM=A0|A1|B     # 缺省 A0
SLUICE_595_R3_CASES=1         # 显式启用研究专用 case；缺省不改变产品 case 列表
SLUICE_BENCH_CASE_FILTER='=<case_id>'
```

三臂使用同一 case_id（arm 只写入 record.notes 与 `r3_*` 计数），输入、顺序、buffer、校验完全相同。

## 4. 研究 case 与产品 case

**产品 case 列表未变**：W-02、W-03 的默认 case 集合与基线相同；W-02 未加 adapter。选定 consumer 是 **W-03 external loop**（`READY_EXPOSURE.md` §1/§2 的 REAL_EXPOSURE 形状）。

`SLUICE_595_R3_CASES=1` 追加：

| case_id | shape | 目的 |
|---|---|---|
| `W03-uring-4k-d2-r3d2pos` | 4096B，d2，request_count=2，max_refill=2 | D2 positive：首提交前 2 条同时 ready |
| `W03-uring-4k-d2-r3d2serial` | 4096B，d2，20k，max_refill=1 | D2 negative：每次唤醒只补 1 条 |
| `W03-uring-4k-d3-r3d3` | 4096B，d3，20k | D3 形状 |
| `W03-uring-4k-d16-r3d16cap` | 4096B，d16，count=64，max_refill=16 | 16 成员启动组 vs cap 8 |
| `W03-uring-64k-d16-r3d16-64k` | 64 KiB，d16，2000 | 不具资格输入：不得进入批量 |
| `W03-uring-4k-d4` / `-d16` / `-d64`（产品 case） | 4096B，20k | W-03 d4/d16/d64；由 `SLUICE_595_R3_ARM` 施加干预 |

说明：D2/D3/D16 在本轮以 **W-03 external-loop consumer** 表达（W-03 是普查确认的真实多 ready consumer；W-02 只具备 STARTUP_ONLY 暴露，且其 RequestScope 不能在不改 scope 语义的前提下接纳外部 Request）。shape 的 block size / depth / request count 不变。

## 5. 构建（两种 profile 都验证）

宿主与工具链（`CONTEXT.md` §4 / `PLATFORM.md`）：E5，Intel Xeon E5-2666 v3，Fedora 44，kernel `7.2.11-200.fc44.x86_64`，g++ 16.2.1 20260819，xmake v3.1.2+HEAD.5d21e9d，liburing 2.13。

### 5.1 io_uring profile（主测量二进制）

```bash
cd /home/jnhu/sluice-595-e5/r3/worktree
xmake f -m release --liburing=y
xmake build -y sluice_async uring_ready_batch_research_test uring_ready_batch_fault_test \
    bench_w03_external_loop bench_w02_pipeline sluice-copy \
    uring_submit_boundary_test uring_progress_race_test uring_flush_count_oracle_test \
    uring_external_loop_test uring_public_consumer_probe
# build ok, spent 8.937s（后续增量重建 2.726s）
```

### 5.2 默认 profile（`liburing` 缺省 false；修复 #1/#2）

```bash
rm -rf /tmp/r3-nolib
xmake f -m release --builddir=/tmp/r3-nolib          # 不带 --liburing
xmake build -y sluice_async bench_w03_external_loop bench_w02_pipeline sluice-copy
# build ok, spent 5.146s（修复前同命令 exit 255）
```

默认 profile 运行（threadpool shape；adapter B 在无 uring backend 时回落到 context 路径）：

```bash
env SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 \
    SLUICE_BENCH_CASE_FILTER='W03-threadpool-4k-d4' SLUICE_595_R3_ARM=B \
    SLUICE_BENCH_TMP_DIR=/tmp/sluice-r3-smoke SLUICE_BENCH_SKIP_DISK=1 \
    /tmp/r3-nolib/linux/x86_64/release/bench_w03_external_loop
# [W03-threadpool-4k-d4-w4 rep=0] ops=20000 ... r3 arm=2 entries=0 ... 
# === W-03 complete, failing repetitions=0 ===  (exit 0)
```

审阅给出的单 TU 语法检查也通过：

```bash
g++ -std=c++20 -Iinclude -Ibench -Isrc/async -fsyntax-only bench/bench_w03_external_loop.cpp
# exit 0
```

### 5.3 产物（`/home/jnhu/sluice-595-e5/r3/worktree/build/linux/x86_64/release/`）

| 产物 | sha256 |
|---|---|
| `libsluice_async.a` | `797bb132b99f1a512fc8e8001b0e03a71e8b9c070204a4c13234c1fee02efb38` |
| `bench_w03_external_loop` | `af6e8afd22b4f5b2e797f5b5b1501b8efa02da5e4f70d2a3a702f524aacdda21` |
| `uring_ready_batch_research_test` | `9b2719a0621d776f7e139265c969996e3d83459f5b74d71a5c2a7ea5ee211947` |
| `uring_ready_batch_fault_test` | `dc5826c1c937af20be68e59e2425f26bab3a5b76926d17236c92022f27277978` |
| `bench_w02_pipeline` | `00a5ca6257e5592013ea47a464abb149003ede3f6f0452a34c7323eea450983f` |
| `sluice-copy` | `2ff18cc0fc70901af8b711fda63017662577351671c08f6df8be891e4b12fd4b` |
| `uring_submit_boundary_test` | `e2fad3e32863c08185396fd9a08512688363940b491a500e8cd44c22a5d23ba4` |
| `uring_progress_race_test` | `52ec0ef179dfbf4eba5dbdc92c470857f08f1593b1f1da9cf4a0f719ece34890` |
| `uring_flush_count_oracle_test` | `9da1fed4cbb25f58974481e4d73a72182ab7d4b7e0c42de3dd4bea86d0c66493` |
| `uring_external_loop_test` | `77e2ab39bb0810fe491fadd0048d37b411b3b1240e522d0f7a5cf48f09792f4f` |
| `uring_public_consumer_probe` | `a67fa45a95e00fc9ee2ae94a22c708fb26c8c9bf2a2a6374bd83b09c2d070545` |

默认 profile 产物在 `/tmp/r3-nolib/linux/x86_64/release/`（临时构建目录，不作为测量产物）。

## 6. 语义 oracle（全部已运行）

```bash
cd /home/jnhu/sluice-595-e5/r3/worktree
./build/linux/x86_64/release/uring_ready_batch_research_test
# uring_ready_batch_research: all research oracles passed   (exit 0)
./build/linux/x86_64/release/uring_ready_batch_fault_test
# uring_ready_batch_fault: all research fault oracles passed   (exit 0)
```

非 seam oracle（`tests/uring_ready_batch_research_test.cpp`）：

| 场景 | 断言 |
|---|---|
| D2 positive（2 条 4 KiB） | 前缀=2、`untouched_begin==2`、suffix=0、`flush_attempts==1`、无 residue；两个独立 CQE/结果各 4096B；buffer 各命中自己的 offset pattern |
| 16 成员 vs cap 8 | 前缀=8、`stop==cap`、`untouched_begin==accepted`、suffix=8；suffix 逐条 span=1 后 16 个 RequestId 互不相同；16 条结果与 bytes 顺序正确 |
| 单条 | immediate 路径，accepted=1、`flush_attempts==1` |
| 首条 8 KiB（不具资格） | 首条 immediate、`stop==ineligible`、`untouched_begin==1`、suffix=1；第二条再 span=1；两条各 8192B |
| 批量后 cancel 一个成员 | cancel 返回合法 disposition；写满并关闭 pipe 后两个成员都 terminal，confirmed bytes 合计 4096 |
| admission 已关闭 | accepted=0、`stop==rejected`、`error==invalid_state`、`untouched_begin==1` |
| **容量在中途耗尽（新增）** | capacity=2、先占 1 槽，再批量 2：accepted=1、`stop==rejected`、`error==would_block`、`untouched_begin==2`、suffix=0；已接受与前一条各有独立 terminal |
| **shutdown 后直接调用（新增）** | ring 已退休：accepted=0、`stop==recovery`、`error==backend_error`、untouched=2，无 SIGSEGV |

seam fault oracle（`tests/uring_ready_batch_fault_test.cpp`，脚本化 fake kernel）：

| 场景 | 断言 |
|---|---|
| **partial submit** | fake 只消费 1/2：accepted=2、`flush_attempts==1`、`residue_advertised==true`、ledger=1；下一次 `poll()` 消费剩余 ledger 前缀（flush 从 1→2），随后注入 CQE，两条各 4096B |
| **批量 flush 触发 poison** | fake 返回 `-EIO`：accepted=2、residue advertised；两条都得到 `backend_error` 的 physical failure（不伪造 success）；`shutdown(drain)==completed` |
| **入口处已有 control intent** | 4 条占满 SQ 后 cancel 一条产生 sticky intent；随后批量 2 条：accepted=0、`stop==control`、`untouched_begin==0`、untouched=2；补齐 CQE 后全部结算，被取消者报 canceled，其余 4096B |

既有回归（全部 exit 0）：

```bash
for t in uring_submit_boundary_test uring_progress_race_test uring_flush_count_oracle_test \
         uring_external_loop_test uring_public_consumer_probe \
         uring_request_core_integration_test shutdown_lifecycle_test; do
  flock /home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock ./build/linux/x86_64/release/$t
done
# 2 / 23 / all checks / 9 / probe passed; 21/21 core integration; shutdown lifecycle PASS
```

单路径回归（W-02 不接入 adapter）：

```bash
env SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 SLUICE_BENCH_CASE_FILTER='=W02-uring-4k-d16' \
    SLUICE_BENCH_TMP_DIR=/tmp/sluice-r3-smoke SLUICE_BENCH_SKIP_DISK=1 \
    ./build/linux/x86_64/release/bench_w02_pipeline
# [W02-uring-4k-d16 rep=0] ops=20000 ... === W-02 complete, failing repetitions=0 === (exit 0)
```

## 7. 三臂 smoke（已运行，非计时）

统一命令（`WARMUP=0 MEASURED=1`，每个 record 仍跑 throughput + byte-oracle + instrumented 三个 phase；`flock MEASUREMENT.lock`；tmpfs `/tmp/sluice-r3-smoke`）：

```bash
env SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 \
    SLUICE_BENCH_CASE_FILTER="=<case_id>" \
    SLUICE_595_R3_CASES=1 SLUICE_595_R3_ARM=<A0|A1|B> \
    SLUICE_BENCH_TMP_DIR=/tmp/sluice-r3-smoke SLUICE_BENCH_SKIP_DISK=1 \
    ./build/linux/x86_64/release/bench_w03_external_loop
```

所有 case 在三臂下均 `rc=0`、`failing repetitions=0`（byte oracle 通过）。`SLUICE_BENCH_STATS=1` 下 d2 positive 的 A1/B 也通过 stats wiring oracle（A1 走 context tally；B 由 entry 逐条计入 `submit_calls`/`submitted_ops`）。关键分组计数（B 臂单次 record 的第一个 phase；A1 不调用 entry，`prefix_max=0`，`flushes` 是逐条 context 提交的标称计数）：

| case | A0 | A1 | B（report） | B（strace 纯 submit enter） |
|---|---|---|---|---|
| d2 positive（2 ops） | rc=0 | entries=2, singles=2, accepted=2, flushes=2 | entries=1, groups=1, accepted=2, prefix_max=2, flushes=1 | 3 phase 各 1 次 `to_submit=2`；A1 为 6 次 `to_submit=1` |
| d2 serial（20k） | rc=0 | singles=20000 | groups=0, singles=20000, prefix_max=1, flushes=20000 | 未 trace |
| d3（20k） | rc=0 | singles=20000 | groups=6667, singles=4, prefix_max=3, flushes=6671 | 未 trace |
| d16 cap（64 ops） | rc=0 | 64 singles | entries=36, groups=4, singles=32, prefix_max=8, suffix_singles=32, untouched=32, accepted=64 | 12 次 `to_submit=8`、96 次 `to_submit=1`；**无 enter > 8** |
| 64 KiB d16（2000） | rc=0 | 2000 singles | groups=3（均在首条 ineligible 停止）, singles=1997, ineligible=3, accepted=2000 | 未 trace；没有 4 KiB 批量 |
| W03 d4（20k，产品） | rc=0 | 20000 singles | groups=5656, singles=483, prefix_max=4, flushes=6139 | 未 trace |
| W03 d16（20k，产品） | rc=0 | 20000 singles | groups=1332, singles=9622, prefix_max=8, suffix_singles=9608, flushes=10954 | 未 trace |
| W03 d64（20k，产品） | rc=0 | 20000 singles | groups=323, singles=17416, prefix_max=8, suffix_singles=17416, flushes=17739 | 1016 次 `to_submit=8`、51872 次 `to_submit=1`；**无 enter > 8** |

strace（原始输出在 `/tmp/sluice-r3-smoke/logs/fix-strace-*.txt`，临时目录）：

```bash
strace -f -e trace=io_uring_enter -o <out> \
  env SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=1 \
      SLUICE_BENCH_CASE_FILTER='=W03-uring-4k-d2-r3d2pos' \
      SLUICE_595_R3_CASES=1 SLUICE_595_R3_ARM=<A1|B> ...
```

## 8. 审阅问题的处置

| # | 问题 | 处置 | 证据 |
|---|---|---|---|
| 1 | 默认 profile 不能编译 shipped library | 研究类型/入口移出 `SLUICE_HAS_LIBURING` 守卫并置于 `public:`；`!liburing` stub 保留同签名 | §5.2 默认 profile build ok |
| 2 | bench 无条件引用 liburing-only 符号 | 同上；默认 profile 下 adapter B 回落到 context 路径 | §5.2 默认 profile bench 运行 |
| 3 | 批量在 `have_ring_` 检查前读 SQ；retire 与 entry 竞争 | 入口及批量临界区内都检查 `have_ring_`；`retire_execution_resources` 在同一 `dispatch_mtx_` 下重查、`queue_exit` 前置 false；接受+派发也在同一临界区。批量不再解引用已退休 ring（内存安全窗口关闭）；并发 submit 与 shutdown 的**合法性**仍不由本入口赋予 | §6 post-shutdown oracle；§5 构建 |
| 4 | A1/B 绕过 context 且每次调用分配 | A1 改为 `AsyncIoContext::submit_read`（与 A0 同路径）；调用者预留 accepted 容器；B 的 context 绕过在 §3.4 显式披露 | §3.4；§7 计数 |
| 5 | 批量风险路径无执行证据 | 新增 5 个场景：容量中途耗尽、shutdown 后调用、partial submit/ledger、批量 poison、control intent | §6 |
| 6 | A0/A1/B 未写入冻结上下文 | 未改 `CONTEXT.md`（非本角色权限）；在 §3.4/头部明确“研究标签、OPEN-1 未解除、不得正式计时”，并披露 A1 语义 | 本文件头部 §3.4 |
| 7 | MANIFEST 校验变红 | 按 R3 重建刷新 `MANIFEST.sha256`（22 项，含 2 个新研究二进制）；基线旧值保留在 `BASELINE.md` §5 | `sha256sum -c` 22/22 OK |
| 8 | READY_EXPOSURE 称 STATE_AUTHORITY 不存在 | 在 READY_EXPOSURE 末尾追加带日期的更正；原文不改写 | `READY_EXPOSURE.md` §9 |
| 9 | 注释/命名与未用 include | 删除研究标签/语义叙述注释、删除未用 `<algorithm>`；保留 1 条 SQ 锁内读取的局部不变量注释 | `git diff` |
| 10 | report 对被拒绝条目语义含混 | 新增 `untouched_begin`；`stop==rejected` 时 rejected 条目位于 `accepted` 且不计入 untouched | §3.2；容量 oracle |

## 9. 限制与未覆盖

1. **没有计时**。表中 wall/cpu 是 smoke 单次值，含三个 phase（其中一次带 per-op clock），不构成 A/B 结论。
2. **单条 submit 与 shutdown 的既有竞争未消除**：`admit_` 的 `have_ring_` 检查不在 `dispatch_mtx_` 内；批量路径已消除“解引用已退休 ring”的窗口，但普通 `submit_read` 与并发 shutdown 的竞争是基线已有行为，本轮未改，也未注入测试。即使批量路径，提交与 shutdown 并发仍不是合法产品用法：settle 的收敛判定与 `retire_execution_resources` 之间的语义窗口不由本入口关闭，丢失完成的风险仍由调用者避免。
3. **未直接覆盖**：stale generation（consume 后槽复用）下的批量、pre-park wake with batch residue 的精确时序、overflow 期间的批量 flush。前三类依赖既有 core/overflow 路径，现有 uring 回归通过，但没有为批量入口单独构造反例。
4. **cap 对深窗口的后果**：d16/d64 的 refill burst 大于 8 时，只有前 8 条进入一次批量 flush，suffix 逐条走 immediate 路径；这是 cap=8 的设计选择，不是“整组一次 enter”的结论。
5. **B 绕过 context**：如 §3.4 所述，B−A1 不能全部归因于提交分组；正式计时前必须拆分，且 `CONTEXT.md` §7.2 `OPEN-1` 仍未解除。
6. **TestAccess 宏守卫**：为让非 seam TU 链接 shipped archive，`detail/test_access.hpp` 的 ProgressSource seam 方法加了 `SLUICE_ASYNC_INTERNAL_TESTING` 守卫；现有 seam 测试不与 non-seam TU 混链，但未来混链会构成 ODR 风险。
7. **consumer 选择**：研究 case 在 W-03 external-loop consumer 上表达；W-02 的 RequestScope 未接入 adapter，仅做单路径回归。
8. **未 push / 未改 issue / 未开 PR**；只写隔离 worktree 与 `evidence/{r3.patch,BUILD.md,MANIFEST.sha256,READY_EXPOSURE.md}`。

## 10. 最小反例

无。构建（含默认 profile）与语义自检均通过；修复轮发现并修复的真实缺陷为：adapter 只统计 accepted 而未物化 `Request`，导致 core 留下无主 public binding 并在 context 析构时 fail-fast（由 16 成员/2 成员 oracle 与 `public_bindings` 正常结算覆盖）。最终态无失败反例。
