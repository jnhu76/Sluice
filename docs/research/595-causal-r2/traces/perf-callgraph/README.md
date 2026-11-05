# C1 用户态调用栈采样（Sluice #595 causal-r2）

状态：ACTIVE EVIDENCE。轮次：causal-r2 C1 深挖（2026-11-04）。
范围：D2（`W02-uring-4k-d2`，4 KiB 读，depth=2）A·IMMEDIATE vs B·BOUNDED_PROGRESS。
用途：**路径发现**（哪个用户态函数/库调用进入样本、B 专属符号），不作为 CPU/op 定量解释。
定量归因见 `evidence/counters/C1_COUNTERS.md`。

## 1. 结论先行

1. 用户态 `perf record -g --user-callchains -k 1` 在 `perf_event_paranoid=2`、无 root 下可用
   （普通 `-g` 无 `--user-callchains` 会因内核 callchain 被拒绝；见 `CAPABILITY_MATRIX.md` 补充）。
2. 6 个 trace（A/B 各 3，ABBA 平衡）均 **Lost Samples = 0**，样本 17,891–18,696 个/run，
   实测采样率 7,990–8,003/s（请求 8,000/s），400k measured op/run，约 0.045 samples/op
   （≈每 65k cycles 一个样本）。
3. B 专属符号证据：`__GI___libc_write` 自采样 0 → **8.5 cycles/op**（A 三 run 均为 0）；
   `dispatch_after_accept` 自身样本 **100.6 → 131.4 cycles/op（+30.8）**；
   `submit_transport_locked` 自身样本 4.0 → 32.4 cycles/op；`pthread_mutex_lock` 总样本
   299.1 → 296.4 cycles/op（**未增加**，被噪声与调用者迁移掩盖）。
4. 调用者链（`dispatch_after_accept → pthread_mutex_lock`）在 A 为 0.18–0.25%、B 为 0.30–0.39%
   的全样本占比；但 callchain 由无帧指针的 `fp` unwinder 恢复，存在 `0x1/0x2` 伪帧
   （见 §5），只能作**定性路径**证据。
5. 采样无法给出函数执行次数或单次成本（`defer_transport_flush_locked_` 太短，样本为 0）。
   因此按任务要求改用同函数聚合计数器，见 `C1_COUNTERS.md`。

## 2. 二进制与 provenance

| 项 | 值 |
|---|---|
| 源码 commit | `271b81bfc9d64eecc0266fe7066b9040525dd710`（与 R1 冻结基线一致） |
| 研究副本 | `/home/jnhu/sluice-595-e5/causal-r2/worktree`（主工作树未改动） |
| trace 二进制 | 本目录 `bench_w02_pipeline_symbols`，sha256 `af25e333623cb6eb0e86cb99239a4ab4849917c979368a1e9bf9974d225a6633` |
| 构建 | `SLUICE_595_STRIP_NONE=1 xmake f -P . -c -y --liburing=y -m release` + `xmake build bench_w02_pipeline`（release 优化不变，仅保留 symtab，无 debug info） |
| 与冻结二进制关系 | **不是字节相同**：`.text` 165,328 B vs 冻结 167,040 B（sha256 `42ef5314…` vs `58836a49…`）。差异来自研究副本的 bench PMU-only/reuse-fixture 代码与计数器空宏脚手架；同轮 A/B 共用同一二进制，故符号份额的 A/B 对比仍有效，但 trace 不用于绝对 CPU 解释。 |
| 工具链 | g++ 16.2.1、xmake 3.1.2+HEAD、perf 7.2.11-200.fc44 |
| 主机/内核 | Xeon E5-2666 v3（10C/20T）、Linux 7.2.11-200.fc44 |

## 3. 运行命令与观测配置

```
D=evidence/traces/perf-callgraph
SLUICE_BENCH_595_DEPTHS=1 SLUICE_BENCH_CASE_FILTER=W02-uring-4k-d2 \
SLUICE_BENCH_WARMUP=0 SLUICE_BENCH_MEASURED=20 SLUICE_BENCH_STATS=1 \
SLUICE_BENCH_PMU_ONLY=1 SLUICE_BENCH_REUSE_FIXTURE=1 \
SLUICE_BENCH_TMP_DIR=/tmp/sluice-595-cost SLUICE_BENCH_JSONL=$D/<tag>.jsonl \
taskset -c 3 perf record -e cycles:u -F 8000 -g --user-callchains -k 1 \
  -o $D/perf-<tag>.data -- taskset -c 2 $D/bench_w02_pipeline_symbols
```

- 顺序 `A1 B1 B2 A2 A3 B3`（`run-order.txt`）；每个 invocation 20×20k = 400k measured op。
- perf 进程固定 cpu3，bench 固定 cpu2；run 期间机器独占，loadavg 记录见 `loadavg.txt`。
- perf 头部实测：`exclude_kernel=1`、`exclude_callchain_kernel=1`、`sample_freq=8000`、
  `inherit=1`、`task=1`、`use_clockid=1`、**`clockid: monotonic (1)`**、clockid frequency 1000 MHz。
- 时钟源一致性：所有 6 个 run 同一 clockid；TSC 为 `constant_tsc/nonstop_tsc`（能力矩阵 §22）。
- 事件丢失：6/6 run `Total Lost Samples: 0`。
- 采样覆盖率：17,891–18,696 samples / 400,000 op = 0.0447–0.0467 samples/op；按 2.24–2.34 s
  窗口与 ~2960–3083 cycles/op，样本周期约 64–66k cycles。符号份额 >0.1% 至少约 18 个样本，
  更小的差异不能从采样定论。

## 4. 结果（自采样 cycles/op；由 samples_symbol/samples_total × event_count/400k 计算）

| 符号（self） | A mean (3 runs) | B mean (3 runs) | B−A |
|---|---|---|---|
| `UringAsyncBackend::dispatch_after_accept` | 100.6 | 131.4 | **+30.8** |
| `UringAsyncBackend::dispatch_one_locked` | 178.3 | 198.5 | +20.2 |
| `UringAsyncBackend::submit_transport_locked` | 4.0 | 32.4 | +28.4 |
| `UringAsyncBackend::poll`（含 clone） | 99.6 | 104.4 | +4.9 |
| `account_transport_result_locked` | 66.8 | 74.0 | +7.3 |
| `__GI___libc_write` | 0 | **8.5** | **+8.5（B 专属）** |
| `pthread_mutex_lock` | 299.1 | 296.4 | −2.8 |
| `__GI___pthread_mutex_unlock_usercnt` | 240.3 | 243.1 | +2.8 |
| `io_uring_submit` | 15.1 | 17.1 | +2.0 |
| `UringAsyncBackend::defer_transport_flush_locked_` | 0 | 0 | 0（函数太短，未被采样） |
| 总 cycles:u/op（由 event count） | 2960.0 | 3083.4 | **+123.4（+4.17%）** |

逐 run 值见 `analysis.txt`（脚本 `analyze_traces.py`）。

读法：
- `dispatch_after_accept` 是 B 的 residue 通知所在函数（`signal_ready_progress()` 被内联于此），
  自采样 +30.8 cycles/op 与 C1 计数器的 signal base（mutex+epoch，~20–22 ns/op ≈ 46–50 cycles/op
  @2.3 GHz，其中一部分落在 libc `pthread_mutex_lock` 符号）方向一致。
- `__GI___libc_write` 只在 B 出现，是 eventfd write 用户侧路径的直接符号证据；内核态 write 时间
  不在 `cycles:u` 内（内核 PMU 无权限）。
- `pthread_mutex_lock` 总样本没有可测增加：B 每次 deferral 新增 0.558 次未竞争 mutex lock/unlock，
  单次 ~10–20 cycles，总量约 +6–11 cycles/op，低于 3-run 采样噪声（A run 间 284–308）。
  调用者迁移（`dispatch_after_accept` 分支 +0.1 pp）只能定性，不能定量。
- `submit_transport_locked` 的 +28.4 cycles/op 与 C1 TSC 的 per-flush 成本 +12 ns/op
  （≈28 cycles/op @2.3 GHz）**一致**，说明 B 的 flush 从 dispatch 挪到 poll 后单次用户态
  submit 路径确实略贵；C2/C3 可继续。

## 5. 采样边界与已知偏差

- **无帧指针/DWARF**：release 二进制未带 `.debug_info`，`--call-graph fp` 只能恢复部分调用链，
  输出含 `0x1`/`0x2`/`0x10000…` 伪帧。**调用者归因（caller attribution）不可靠**；
  本文只使用 symbol self 份额与 B 专属符号，不据此做调用者级定量。
- **方法混用禁止**：本 trace 单独运行，未与 perf stat/计数器/其他 instrumentation 叠加。
  采样本身改变时序（-F 8000 约每秒 8000 次采样），因此 trace 的 A/B cycles 比
  （+4.17%）不能与无仪器 split-d2 的 +3.70% 直接比较；只用于路径发现。
- **内核不可见**：`exclude_kernel=1`，内核态 `eventfd_write`/`io_uring_enter` 的指令与 cycles
  不在本 trace 中；需要 root/CAP_PERFMON（本会话不可得）。
- **uprobe/USDT 不可用**：tracefs root-only 且 `unprivileged_bpf_disabled=2`，bpftrace/uprobe/USDT
  无法在无 root 下 attach；Sluice 源码也无 USDT 探针。函数级归因由符号化 perf 采样 + 源码聚合
  计数器完成，未使用 uprobe。
- **run1 目录**：`perf-callgraph-run1-unpinned-binary/` 是同一协议的首批 trace；其二进制在后续
  重建中被覆盖、未保留，仅有 sha256（`b59da2cb…`）与 `.text` 分析。本文结论取可复现的
  `perf-callgraph/`（二进制已随证据保存）。两批结论方向一致，未混用。

## 6. 文件

| 内容 | 路径 |
|---|---|
| 本 README | `README.md` |
| 分析脚本 / 输出 | `analyze_traces.py`、`analysis.txt` |
| perf data（6 个） | `perf-{A,B}-r{1..6}.data` |
| 每次 run 的 bench JSONL/输出/错误 | `{A,B}-r*.jsonl`、`.out`、`.err` |
| 二进制与哈希 | `bench_w02_pipeline_symbols`、`binary-sha256.txt` |
| 环境/顺序/loadavg | `env-before.txt`、`run-order.txt`、`loadavg.txt` |
