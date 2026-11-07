# CALIBRATION_SUMMARY — #595 R3 续跑 A/A 校准与 R2 同 SHA 复现

生成（UTC）：2026-11-05T11:16:26Z

## 0. 结论

| block | phase | 对照 | pairs | CPU mean [CI] | tput mean [CI] | p99 mean [CI] | 判定 |
|---|---|---|---|---|---|---|---|
| AA-W03-D16 | AA-W03-D16 | A0 vs A0 | 24 | -0.816% [-1.616%, -0.022%] | -0.870% [-1.699%, -0.057%] | +0.313% [-1.785%, +2.292%] | PASS (hw 0.797/0.821/2.039pp) |
| AA-W02-D2 / R2-AA | AA-W02-D2 | A_R2 vs A_R2 | 24 | +0.584% [-0.270%, +1.454%] | +0.539% [-0.310%, +1.397%] | -0.407% [-1.863%, +0.978%] | PASS (hw 0.862/0.853/1.420pp) |
| R2-D2 | R2-D2 | B_R2 − A_R2 | 24 | +3.435% [+2.673%, +4.160%] | +3.298% [+2.574%, +3.976%] | +7.052% [+4.844%, +9.514%] | FAIL (LEGACY_D2_NEGATIVE_CONTROL_FAIL) |

A/A 校准通过（两块的 CPU/tput/p99 半宽都远低于门槛），因此本会话的噪声底足以支撑同二进制配对比较；R2-D2 的 FAIL 是旧 B 机制的负控结论，不是 R3 B 的结论。

## 1. 二进制与来源

- R3 二进制：`/home/jnhu/sluice-595-e5/r3/worktree/build/linux/x86_64/release/bench_w03_external_loop` sha256 `0ebfb7f0c204579fe2cc9e8d7e88cee2de9112e25c0cfb11e8502e59fc4fe2d1`（= BUILD_COMPLETION §8.1）。
- R2 二进制：`/home/jnhu/sluice-595-e5/worktree/build/linux/x86_64/release/bench_w02_pipeline` sha256 `32beabcd891801eb65dab1d6e2df72aa0b89a98a2c16a114d398527cb821589a`（R2 树 HEAD 271b81bf，`git diff HEAD` = b006dbbf…，reverse-check exit 0）。
- R2 树构建：`xmake f -m release --liburing=y` + `xmake build -y bench_w02_pipeline uring_595_bounded_progress_test`，build ok；树 status 6 行、diff sha 构建前后不变。
- R2 patch 判据/flush/residue：源码门链（uring_transport.cpp:84-90, 229-263）已核对；`uring_595_bounded_progress_test`（SLUICE_595_ARM=B）输出 `all checks passed`（held-batch flush、cap、partial submit、cancel、owner/witness/payload gate、prepark residue、poison、shutdown）。

## 2. 计时协议与有效性

- 24 对/块，AB/BA 交替（A/A：偶 m1,m2 / 奇 m2,m1）；每 invocation WARMUP=1、MEASURED=5、STATS=0、`taskset -c 2`、warm tmpfs `/tmp/sluice-595-r3c`。
- 每块 24/24 有效配对；`attempts.jsonl` 为空；fixture sha 块前后不变；所有 invocation exit=0、ops=20000、byte oracle 通过。
- 锁：`/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`，获取 2026-11-05T11:03:31Z（等待 0s），释放 2026-11-05T11:08:21Z；`fuser -v` 记录持锁 pid `51956 51965`。`/proc/locks` 在本平台未列出该 flock 条目（header 的 holders 为空），探针会话的排他性检查（第二 open file description `flock -n` 失败）确认锁语义有效。

## 3. 逐块细节

### 3.1 AA-W03-D16（R3 A0 vs A0）

- CPU mean -0.816% [-1.616%, -0.022%]，半宽 0.797pp；tput 半宽 0.821pp；p99 半宽 2.039pp。
- 绝对量级：m1 CPU/op 3154.75 ns、m2 3128.33 ns；m1 tput 317,259 ops/s、m2 319,946 ops/s。
- 均值偏移（m2 略快约 0.8%）不是门；A/A 只看半宽（prereg §8.2）。

### 3.2 AA-W02-D2 / R2-AA（R2 A_R2 vs A_R2）

- CPU mean +0.584% [-0.270%, +1.454%]，半宽 0.862pp；tput 半宽 0.853pp；p99 半宽 1.420pp。
- 绝对量级：m1 CPU/op 4830.71 ns、m2 4858.23 ns；m1 tput 206,812 ops/s、m2 205,669 ops/s。

### 3.3 R2-D2（A_R2 vs B_R2，同 SHA）

- CPU +3.435% [+2.673%, +4.160%]，单侧上界 +4.065%（≤5% 通过）。
- tput +3.298% [+2.574%, +3.976%]，单侧上界 +3.889%（>3% FAIL）。
- p95 +5.233%，单侧上界 +5.906%（>5% FAIL）。
- p99 +7.052% [+4.844%, +9.514%]，单侧上界 +9.067%（>5% FAIL）。
- 绝对量级：A CPU/op 4850.41 ns、B 5015.83 ns；A tput 205,994 ops/s、B 199,155 ops/s。
- 历史 R2 D2（`docs/research/595-causal-r2/PROTOCOL.md:126`，仅作对照、不复用）：CPU +3.73% [+3.04,+4.42]、tput +3.58% [+2.94,+4.22]、p95 +4.96% [+4.21,+5.75]、p99 +7.07% [+4.19,+11.80]；本轮 +3.435%/+3.298%/+5.233%/+7.052%，CI 高度重叠。
- 机制诊断（短 strace，只作机制证据）：A/B 各 60,004 次纯 submit enter、60,004 SQE、SQE/enter=1.0、max to_submit=1、无 to_submit>1；stats submits/poll/wait 完全相同。D2 形状没有可合并的提交，B_R2 的成本来自门链检查与 flush 时点后移，而非批量收益。

## 4. enter/SQE/residue/eventfd 诊断（R2）

| 臂 | 纯 submit enter | 总 enter | SQE | SQE/enter | max to_submit | register | eventfd | to_submit 直方图 |
|---|---|---|---|---|---|---|---|---|
| A | 60004 | 60004 | 60004 | 1.000 | 1 | 12 | 4 | {'1': 180012} |
| B | 60004 | 60004 | 60004 | 1.000 | 1 | 12 | 4 | {'1': 180012} |

- 每次 invocation 跑 3 个 pass（timed/oracle/instrumented）× 20,000 ops = 60,000 data SQE，加 4 次 setup enter，合计 60,004。
- 3 次重复的 enter 数逐次相同；A/B 无差异；未观察到 deferred multi-SQE flush。
- `eventfd` 只在内核注册/注销（`io_uring_register_eventfd`），无用户态 eventfd 写入。
- residue：R2 bench 不暴露 ledger 计数；从 60,004 SQE = 60,004 to_submit 可判定本次诊断中所有 SQE 都被内核取走（无未提交 residue 残留到退出）。

## 5. 线程分桶（bucket-probe，描述性）

| 配置 | main ticks | driver ticks | io-wq ticks | in-thread stime | ru_total (s) | bench cpu_s (timed pass) | 未归因 ticks | IRQ cpu2 / cpu12 (mean) |
|---|---|---|---|---|---|---|---|---|
| R3-W03-D16-A0 | 13.3 | 0.7 | 7.3 | 15.0 | 0.276 | 0.0701 | +6.3 | 307 / 3 |
| R2-W02-D2-A | 22.3 | 0.0 | 10.0 | 23.7 | 0.347 | 0.1005 | +2.3 | 631 / 3 |
| R2-W02-D2-B | 22.7 | 0.0 | 10.0 | 24.0 | 0.358 | 0.1040 | +3.2 | 646 / 2 |

- 分桶口径：main = tid==pid；driver = 其它非 `iou-*` 用户线程；io-wq = comm 以 `iou-` 开头的内核 worker（它们出现在 `/proc/<pid>/task` 内，属于进程线程组，因此其 CPU **已包含在** `CLOCK_PROCESS_CPUTIME_ID` 中，不重复相加）。
- in-thread kernel = 各线程 stime 之和，是上述分桶的子集，不另加。
- 未归因 = `wait4` rusage ticks − 分桶 ticks；来自 10 ms tick 粒度与采样窗口外的短线程，本轮探针为 +2…+7 ticks（约 0.02–0.07 s）。
- IRQ 为 `/proc/interrupts` 的 cpu2/cpu12 列增量（整机口径上界，无法在不做 tracing 的前提下归因到本 workload；tmpfs 无块设备 IRQ，主要为 timer/IPI）。
- 计时轮的 sampler 只比较全局首末样本，driver 线程（生命周期在 invocation 内部）被低估为 0；本表用修正后的逐线程 first-seen/last-seen 探针替代，计时轮 CPU 主指标仍是 bench 的进程 CPU（包含全部线程，无重复计数）。

## 6. 边界与未做事项

- 本轮不做 PMU/tracing（除 R2 enter 短诊断的 syscall 计数外）；无 perf/ftrace/bpf。
- 未跑资源门（RSS/分配/线程/队列）的正式判定；RSS 只记录 `ru_maxrss`。
- 未跑 R3 B/A1 块（`semanticsReady=false`，按预注册 blocked）。
- 未跑 #604 真实应用产品门（PRODUCT=HOLD）。
- R2 旧 B 的 D2 结论只描述旧机制；跨 SHA 只并列、不相减。
