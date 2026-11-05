# C2 探针开销量化（strace / liburing-shim，Sluice #595 causal-r2）

状态：ACTIVE EVIDENCE。轮次：causal-r2 C2（2026-11-04）。
用途：为 `traces/c2-syscall/README.md` 的 syscall 级证据划定可用性边界
（路径/不变量发现 vs 定量解释）。所有对照为**同臂、同块、同二进制** ON/OFF；
二进制 sha256 `755346410…33709af`（研究副本，计数器 env 关闭）。
运行条件：`taskset -c 2`，PMU_ONLY=1，fixture 复用，WARMUP=1 MEASURED=3
（tail 子块 MEASURED=1 且不开 PMU_ONLY 以获得 latency pass）。

## 1. strace（ptrace，逐 syscall 停止式）

同臂 ON/OFF（每臂 2 invocation × 3 measured rep × 20k op，n=6/臂）：

| 指标 | A OFF | A ON(strace) | Δ | B OFF | B ON(strace) | Δ |
|---|---|---|---|---|---|---|
| CPU/op (ns) | 5,092.7 | 13,139.8 | **+8,047.1（+158%）** | 5,332.9 | 13,169.0 | **+7,836.1（+147%）** |
| 吞吐 (ops/s) | 196,150 | 26,862 | **−86.3%** | 187,316 | 26,721 | **−85.7%** |

tail 子块（3-pass 模式，bench 自身 latency pass 的分位数，n=20,000/op 臂）：

| 分位 | A OFF | A ON | 倍数 | B OFF | B ON | 倍数 |
|---|---|---|---|---|---|---|
| p50 (ns) | 8,911 | 38,563 | ×4.3 | 9,066 | 77,011 | ×8.5 |
| p95 (ns) | 13,625 | 43,709 | ×3.2 | 14,197 | 85,815 | ×6.0 |
| p99 (ns) | 14,308 | 50,802 | ×3.6 | 14,866 | 94,840 | ×6.4 |

附注：ON 状态下 A/B 的 CPU/op 差消失（+0.2%），吞吐差 −0.5%——strace 扰动
（+8 µs/op 量级）远超 D2 效应（+3.7%，≈190 ns/op），且改变 B 的行为（见
`traces/c2-syscall/README.md` §0.4：eventfd write 塌缩为每 pass 1 次）。

**判定：strace 仅用于路径/参数/返回值不变量发现，禁止用于任何定量解释。**

## 2. liburing LD_PRELOAD shim（TSC 事件环，原生速度）

同臂 ON/OFF（每臂 2 invocation × 3 measured rep，n=6/臂）：

| 指标 | A OFF | A ON(shim) | Δ | B OFF | B ON(shim) | Δ |
|---|---|---|---|---|---|---|
| CPU/op (ns) | 5,085.1 | 5,201.5 | **+116.4（+2.29%）** | 5,401.0 | 5,487.8 | **+86.8（+1.61%）** |
| 吞吐 (ops/s) | 196,498 | 192,060 | **−2.26%** | 184,997 | 181,963 | **−1.64%** |

- shim 事件量 ≈3.1/op（submit begin/end + peek begin/end），每事件 ~2×(lfence;rdtsc)+存
  ≈20–40 ns，合计 +87…+116 ns/op，与 D2 效应（≈+190…+240 ns/op）同量级（约 36–61%）。
- A/B 效应方向在 shim 下保持（ON +5.50% vs OFF +6.21%，同块），说明 defer/signal 结构
  未被 shim 破坏（与 strace 相反）；但幅度被探针扰动污染。

**判定：shim 用于结构/时点发现（flush 位置、submit 时长分布、ret 分布），
不用于效应幅度定量。**

## 3. 无仪器 mini-OFF 基线（两个块的 OFF 组，非冻结协议）

| 块 | A CPU/op | B CPU/op | Δ | A tput | B tput |
|---|---|---|---|---|---|
| strace 块 OFF | 5,092.7 | 5,332.9 | **+4.72%** | 196,150 | 187,316（−4.5%） |
| shim 块 OFF | 5,085.1 | 5,401.0 | **+6.21%** | 196,498 | 184,997（−5.9%） |

两块条件与冻结协议不同（PMU_ONLY、fixture 复用、MEASURED=3、研究副本二进制），
幅度 +4.7…+6.2% 落在 Phase B 已知块间漂移带（+2.67…+4.52%）上缘之外略高，
不做效应估计使用；只作 ON/OFF 开销比值的分母。块间差异本身与 G6（频率/漂移）
一致：shim 块 submit p50 段间在 ~1.2/~3.3 µs 两档交替（≈2.75×，接近 3.5/1.2 GHz
频率比），为核频档位漂移的直接痕迹。

## 4. 事件丢失 / 采样覆盖率 / 时钟源

- strace：ptrace 停止式，**无采样、无丢失**；完整性核对：每 invocation enter 总数
  80,005（=4×20,000+4+1）、setup 5、unfinished/resumed=0、SIGCHLD=1（bench 自身 fork）。
- shim：`capped=0`（8M 事件上限，实用 ~24.7 万/invocation），**100% 调用覆盖**
  （interposed 符号的每次调用都记录，无采样）。
- 时钟：shim TSC 每进程自校准 2,893,334–2,893,359 kHz（6 次极差 <25 kHz，constant_tsc），
  与 C1 校准 2.893 GHz 一致；strace 用 CLOCK_REALTIME µs。跨仪器不做时间轴对齐；
  A/B 比较全部在单一仪器内完成。

原始数据与复现命令：`../traces/raw-c2/`（`block-runlog.txt`、`shim-runlog.txt`、
分析脚本 `analyze_c2_*.py`、`dur_stats_c2.py`、`shim_fractions.py`）。
