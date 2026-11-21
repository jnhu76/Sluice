# P0-C 页缓存/文件系统成本研究（e5-fullpath-machine-work）

- 执行：页缓存研究员（P0-C），2026-11-18 01:20–02:40（本地时间）
- 基线：master `5d94f233967c3aef993eb19ba9e3252c532b0951`（与 repo-reality.md 冻结基线一致）
- 性质：纯测量研究（RESEARCH_ONLY；无任何生产/公共 API 改动；未修改任何被跟踪文件，
  会话前后 `git status --porcelain` 完全一致 = 两个既有未跟踪目录）
- 原始数据与临时脚本：全部在 `/tmp/sluice-p0c/`（不入仓库；见 §11 清单）
- 本内核 **7.3.3-200.fc44**：与 #595/#622 各轮的 7.2.11 不同，**历史数字不可直接比**；
  本阶段建立本内核新基线（docs/performance/methodology.md Freeze/Baseline；595-pause
  RESUME §2 "内核升级后不承诺同数字"）

## 0. Freeze（测量前冻结的参数）

| 项 | 值 |
|---|---|
| 基准构建 | `bash bench/build_profiles.sh release`（exit 0；三断言全过：ring 二进制依赖 liburing、noring 不依赖、W-01 跨 profile 字节一致 sha256 `4d4a948a…`；profile 身份由 readelf 断言，非目录名） |
| 计时使用的二进制 | `build-bench-ring/linux/x86_64/release/bench_w02_pipeline`、`bench_w03_external_loop`（sha256 见 build-profiles.log） |
| bench 环境 | `SLUICE_BENCH_WARMUP=2 SLUICE_BENCH_MEASURED=7`（7 measured reps/格）；`SLUICE_BENCH_CASE_FILTER='=<case>'` 精确匹配（不用子串——`…-d1` 是 `…-d16` 的前缀）；`SLUICE_BENCH_JSONL=<cell>.jsonl` |
| fixture | bench 自建 64 MiB（`kFixtureBytes = 64u << 20`，bench_w02_pipeline.cpp:50 constexpr，不可改）；探针独立数据集 512 MiB /dev/urandom（> 25 MiB L3；≥256 MiB 要求满足） |
| 探针重复 | 每格 5 reps；bench 每格 7 reps；instr 另做 3×(full−null) Δ |
| governor | 逐 CPU 存原值 → performance（读回验证 20/20）→ 计时全程 → 结束恢复并读回 20/20 match=yes。三列存档：`/tmp/sluice-p0c/governor/{original.txt,set-verify.txt,restore-verify.txt}`。频率采样 17614 样本（~59 min，0.2s 间隔，全 20 CPU）：负载中位 3,500,000 kHz、最低 1,163,779 kHz（空闲间隙），与 #621 观察的 intel_cpufreq 3.25–3.5 GHz 带一致 |
| 主口径 | user-space instructions/op（`perf stat -e instructions:u`，paranoid=2 下明确 :u 修饰）为对抗频率漂移的主计数；CPU/op（rusage u+s）、cycles:u/op、wall、ops/s、p50/p95/p99 为辅列；**各列独立报告，不相加** |
| 正确性门 | bench 每 rep 自带 3-pass（throughput + verify-oracle + latency），任何失败即 record.ok=false 且进程非零退出；驱动逐格断言 `failing repetitions=0` 且 exit=0。探针断言整块读回（short_reads=0）+ 每次 rep 前 mincore 状态验证 |

## 1. 装置事实（两处与任务前提不符，如实登记）

1. **"/home=btrfs（NVMe）"不成立**：/home 在 `/dev/sda3`（btrfs，ROTA=0，型号 **GS-480**，
   vendor ATA——SATA SSD，**非 NVMe**）。nvme0n1（476.9G）存在但未挂载（本阶段不挂载、
   不改系统）。所有"设备 FS"格的真实设备是这块 SATA SSD。
2. **/home 挂载带 `compress=zstd:1`**：scratch 目录用 `chattr +C`（NOCOW，语义上禁用压缩），
   数据集经 `sudo compsize` 权威验证 `Type=none, Disk=512M=Uncompressed` → **读路径解压
   工作按构造排除**。（旁注：无 +C 的全零文件实测仍满块分配——挂载压缩选项的实际行为
   未定论；与本测量无关，因数据集按构造不可压缩。）
3. 其他装置事实：CPU E5-2666 v3 10C/20T 单 NUMA；62 GB RAM；/tmp=tmpfs（shmem）；
   `read_ahead_kb=128`（sda）；`max_sectors_kb=4096`；perf_event_paranoid=2；
   bpftrace 0.24.2（BTF）；fincore 可用（mincore 回退自建 `cachectl` 实测一致）。
4. **内核 7.3.3 新事实**：tmpfs 支持 O_DIRECT（open+整块读成功）；tmpfs 拒绝
   RWF_NOWAIT（preadv2 → EOPNOTSUPP，errno 95）；io_uring tracepoint 集：
   `io_uring_submit_req / io_uring_complete / io_uring_queue_async_work /
   io_uring_local_work_run / io_uring_task_work_run`（7.2.11 时代的 `io_uring_task_run`
   已不存在，punt 计数改用 `queue_async_work`，与 submit_req 对账）。

## 2. 缓存状态纪律与实测有效性

- **warm**：整文件预读 + mincore 验证。探针格逐 rep 验证 `res_pct ≥ 99.9%`
  （实测恒 100.0000%）。bench 格的 warm 由装置构造成立：每次调用先 `write_fixture`
  （64 MiB buffered write + `sync_data`），写即驻留——`fincore` 实测
  `pipeline.bin` 16384/16384 页驻留。
- **cold-ish**：对该 scratch 文件 `POSIX_FADV_DONTNEED` + mincore 复核。**实测有效性
  100%**：每次 drop 后残留 0.0000%（0/131072 页），且逐 rep 复验；205 个探针 rep
  **零** INCONCLUSIVE/NOTWARM 标记。sector 对账：每个 cold buffered 格设备读取
  恰好 512 MiB（=文件大小，零字节浪费）。
- **禁止全局 drop_caches**：未使用（全程未碰 /proc/sys/vm/drop_caches）。
- **bench 侧 cold 格不可构造（装置限制，非跳过）**：`main()` 在任何 case 之前调用
  `write_fixture`（bench_w02_pipeline.cpp:613-619），每次调用 truncate+重写并驻留
  页缓存；被跟踪文件不可改 → 无 hook 可在"fixture 写"与"测量 pass"之间插 DONTNEED。
  故 **Sluice bench 的 btrfs cold 格判 NOT_CONSTRUCTIBLE**，cold 维度由探针（可验证
  DONTNEED）承担。未硬凑、未报告任何冷缓存胜利。
- **tmpfs 无 cold 状态**：shmem 页 fadvise DONTNEED 无效（实测 drop 后残留 100%）——
  tmpfs 格一律按"恒驻留"登记。

## 3. 口径注记（读数前必读）

1. **bench instr/op 是 3-phase 合成口径**：每 rep 含 throughput + verify-oracle
   （memcmp 全块）+ latency 三 pass。数值 = `perf stat(instructions:u) [full−null] /
   (9 rounds × 3 phases × ops)`，null = 仅 fixture 写 + oracle 自检（Δ 法扣除）。
   组成恒定，**格间相对比较成立**；绝对值不可当作"单次 read 的用户态指令数"。
2. **task-clock:u 与 rusage 口径不一致**（tp-4k-d1-w1：17.06 vs 10.22 µs/op）：
   仅记录不引用；与 #622 "rusage 在 syscall 密集负载下失真"同族。三量纲
   （instructions/cycles/wall）独立列示、不相加。
3. **punt 计数的 comm 过滤**：完成发生在 iou-wrk worker 上下文时 comm 变为
   `iou-wrk-*`，被进程 comm 过滤排除（tmpfs 格 `@comps=0` 即此因）；punt 率以
   `@punts/@submits` 计。punt 计数窗口含 28 个启动残差事件（context 构造探测），
   占 5.2e-5/op，忽略。
4. **W-03 comm 截断教训**：task comm 15 字符上限，`bench_w03_external_loop` →
   `bench_w03_exter`；首跑用 16 字符串过滤全 0，已修正重跑（过程记录保留在日志）。
5. **探针与 Sluice 数字不混表**（§5 vs §6 独立表）；跨表观察仅在 §8 以
   "跨 harness 假设"形式登记，不作为受控结论。
6. bench JSONL 的 `filesystem`/`cache_policy` 字段是**硬编码标签**（bench_w02_pipeline.cpp:473-474
   恒为 "tmpfs"/"page_cache_resident"），不是探测值；本文以实际 SLUICE_BENCH_TMP_DIR
   与 fincore 为准。

## 4. 筛选矩阵——Sluice 侧（bench，ring profile）

所有格：btrfs warm（§2 构造+验证）或 tmpfs（恒驻留）；7 reps；CV 为 ops/s 与
instr/op 的样本 CV。CPU/op=进程 rusage Δ/ops（throughput pass）。

### 4.1 btrfs warm buffered

| 格 | ops/s | CV | CPU/op µs | instr/op | CV(instr) | cyc/op | wall s | p50/p95/p99 ns |
|---|---|---|---|---|---|---|---|---|
| W02-uring-4k-d1 | 551,996 | 4.25% | 1.82 | 6407 | 0.00% | 3262 | 0.0363 | 451/497/554 |
| W02-uring-4k-d4 | 600,421 | 2.08% | 1.67 | 4961 | 0.01% | 2685 | 0.0333 | 5427/5782/11349 |
| W02-uring-4k-d16 | 571,785 | 3.26% | 1.75 | 4942 | 0.02% | 2814 | 0.0350 | 26462/28835/38952 |
| W02-uring-4k-d64 | 526,703 | 3.55% | 1.90 | 6300 | 0.01% | 3360 | 0.0380 | 118567/129580/143772 |
| W02-uring-64k-d16 | 117,960 | 4.43% | 8.59 | 8445 | 0.01% | 8632 | 0.0170 | 124057/138040/146682 |
| W02-tp-4k-d1-w1 | 119,831 | 1.21% | 10.22 | 8376 | 0.00% | 10890 | 0.1669 | 6905/7755/19023 |
| W02-tp-4k-d16-w1 | 312,744 | 0.90% | 5.72 | 5491 | 0.30% | 8935 | 0.0640 | 47938/64859/75141 |
| W02-tp-4k-d16-w4 | 369,670 | 1.12% | 8.68 | 5062 | 0.05% | 9648 | 0.0541 | 41885/53899/59741 |
| W02-tp-4k-d16-w8 | 342,228 | 2.61% | 10.66 | 5186 | 0.02% | 10439 | 0.0585 | 44809/54806/60869 |
| W02-tp-64k-d16-w4 | 306,767 | 2.67% | 15.11 | 9055 | 0.05% | 14014 | 0.0065 | 50597/61981/68585 |
| W03-uring-4k-d16 | 269,673 | 1.50% | 5.44 | 4146 | 0.02% | 4817 | 0.0742 | 34009/57268/67748 |

### 4.2 tmpfs 参考格（对照 #622 H2）

| 格 | ops/s | CPU/op µs | instr/op | punt 率 | p50/p95/p99 ns |
|---|---|---|---|---|---|
| W02-uring-4k-d16 (tmpfs) | 412,768 | 4.71 | 4968 | **1.0001/op**（540028/540028） | 38200/47962/55006 |
| W02-tp-4k-d16-w4 (tmpfs) | 362,522 | 8.76 | 5062 | n/a（pread64=1.000/op：540002/540000） | 42938/55013/61191 |
| W03-uring-4k-d16 (tmpfs) | 276,076 | 7.21 | 4247 | **1.0001/op**（540028/540028） | 36891/57853/67233 |

**#622 H2 在 7.3.3 精确复现**：tmpfs 上 buffered io_uring read 每 op 1.0001 次 io-wq
punt；btrfs warm buffered（4k-d1/d16、64k-d16、W-03）punt ≈ 0（28/540028 = 5.2e-5，
纯启动残差），completion 计数精确对账（540000 = ops）。**FS 条件性成立**。

**核心分解（同格对比）**：uring-4k-d16 btrfs warm vs tmpfs——
user **instr/op 4942 vs 4968（差 0.5%）**、CPU/op **1.75 vs 4.71 µs（2.7×）**、
ops/s 571,785 vs 412,768（+38%）。即：**Sluice 用户态机工与 FS 位置无关；
内核态机工由 punt 形态决定（tmpfs 每 op 多付 ~2.96 µs 内核 CPU）**。W-03 同构
（punt delta +1.77 µs/op，epoll 外驱形态下摊薄）。

threadpool 后端对 FS 位置不敏感（btrfs 8.68 vs tmpfs 8.76 µs CPU/op），因为它
每 op 恰 1 次 pread64（bpftrace 实测 540002/540000）打在暖页缓存上。

## 5. 筛选矩阵——O_DIRECT/机器工作对照（独立探针，不与 Sluice 混表）

ioprobe（/tmp/sluice-p0c/src/ioprobe.c，自编译；plain pread/preadv2 线程池）。
数据集 512 MiB btrfs（+C，未压缩）。每格 5 reps；`perf stat instructions:u` 直接
按 ops 除（单 pass 口径，无 verify/clock 混入）。cache 列 = mincore 逐 rep 残留/驻留；
disk 列 = /proc/diskstats sda 每 rep deltas（reads_completed, sectors_read）。

### 5.1 buffered（btrfs）

| 格 | ops/s | CPU/op µs | instr/op | p50 ns | cache | 设备 I/O /rep | 设备字节/rep |
|---|---|---|---|---|---|---|---|
| warm-4k-seq-d1 | 1,044,935 | 0.95 | **102** | 939 | 100% | **0** | **0** |
| warm-4k-rand-d1 | 984,021 | 1.01 | 122 | 991 | 100% | 0 | 0 |
| warm-4k-seq-d16 | 7,631,044 | 2.04 | 103 | 1879 | 100% | 0 | 0 |
| warm-64k-seq-d1 | 158,698 | 6.28 | 120 | 6096 | 100% | 0 | 0 |
| warm-64k-seq-d16 | 787,797 | 19.32 | 125 | 20914 | 100% | 0 | 0 |
| cold-4k-seq-d1 | 109,393 | 1.69 | 102 | 956 | 0% | 136（合并后） | 512 MiB |
| cold-4k-seq-d16 | 102,655 | 3.96 | 103 | 2527（p99 9.0ms） | 0% | 393 | 512 MiB |
| cold-4k-rand-d1 | 5,338 | 21.83 | 122 | 147120 | 0% | 96,723（0.74/op） | 512 MiB |
| cold-4k-rand-d16 | 4,946 | 39.93 | 123 | 205547（p99 29ms） | 0% | 98,343 | 512 MiB |
| cold-64k-seq-d1 | 6,826 | 19.50 | 120 | 6594 | 0% | 135 | 512 MiB |
| cold-64k-rand-d1 | 1,485 | 80.51 | 140 | 626257 | 0% | 6,840 | 512 MiB |
| tmpfs-4k-seq-d16 | 8,315,514 | 1.91 | 103 | 1884 | n/a（恒驻留） | 0 | 0 |

### 5.2 O_DIRECT（btrfs，对齐纪律：posix_memalign(4096)、4K/64K 倍数偏移）

| 格 | ops/s | CPU/op µs | instr/op | 设备 I/O /op |
|---|---|---|---|---|
| 4k-seq-d1 | 35,176 | 9.54 | 102 | 1.000 |
| 4k-seq-d2 | 5,884 | 25.97 | 106 | 1.000 |
| 4k-seq-d4 | 3,760 | 50.56 | 106 | 1.000 |
| 4k-seq-d8 | 3,568 | 50.73 | 107 | 1.000 |
| 4k-seq-d16 | 3,849 | 51.32 | 103 | 1.000 |
| 4k-rand-d1 | 3,591 | 26.43 | 122 | 1.000 |
| 4k-rand-d16 | 3,677 | 52.29 | 123 | 1.000 |
| 64k-seq-d1 | 3,892（**CV 35.66%**，见 §9） | 21.56 | 120 | 1.000 |
| 64k-seq-d16 | 1,994 | 77.24 | 126 | 1.000 |
| 64k-rand-d1 | 1,546 | 66.03 | 140 | 1.000 |
| tmpfs-4k-seq-d16 | 8,896,315 | 1.77 | 103 | 0（shmem O_DIRECT=内存拷贝） |

要点：
- **warm 页缓存命中地板 ≈ 102 instr/op、0.95 µs CPU/op、0 设备 I/O**——"Page Cache
  满足"与"必须进磁盘"的判据由 mincore + diskstats 直接给出，而非推断。
- **cold buffered 全部格设备字节恰为 512 MiB（零字节浪费）**；cold-seq-d1 每 rep
  仅 136 次 reads_completed = 块层把 128 KiB RA bio 合并为 ~3.8 MiB 请求
  （read_ahead_kb=128）。cold-rand-d1 的 0.74 I/O/op 由 ~1.1% 的 128 KiB RA 窗口
  覆盖后续随机块解释（纯时机收益，无字节收益）。
- **本存储栈存在 O_DIRECT 深度悬崖**：4K seq D1 35,176 → D2 5,884（−83%）→
  D4+ 平坦 ~3.6–3.8K，全程 1.0 I/O/op、零合并（DIO 直发不经 plug）。机制归因
  **INCONCLUSIVE**（GS-480 FTL 深度行为 vs btrfs DIO 路径串行化，未分辨）。
  含义：**在本机器上 O_DIRECT 不是高深度合法对照的可用形态**；"消除页缓存"叙事
  若在本机验证必须以 D1 或 buffered 对照分叉（同 #622 C4 的 FS 条件性教训）。
- cold buffered seq D16 反而劣于 D1（102,655 vs 109,393 ops/s，p99 9 ms）——
  并发顺序读者与 RA 窗口的交互在本栈为负收益；机制 INCONCLUSIVE（探针形状/
  设备/FS 三者未分辨，探针 chunk=64 块分发与 Sluice 滑窗不同形）。
- rand vs seq（warm）：984K vs 1045K ops/s（D1）——页缓存命中下布局无关；
  rand 冷/热差完全来自设备 I/O（5,338 ops/s ≈ 本盘 4K 随机极限 ~5.3K IOPS）。

## 6. 研究项（筛选后选 2 项）

### RQ-A：posix_fadvise 家族真实收益（cold seq 4K D1 基线 109,393 ops/s）

| fadvise | ops/s | Δ | CPU/op µs | 设备 I/O /rep | 结论 |
|---|---|---|---|---|---|
| none（基线） | 109,393 | — | 1.69 | 680 | 默认 RA(128K) 已近设备带宽上限（428 MB/s） |
| SEQUENTIAL（RA×2） | **67,496** | **−38.3%** | 1.79 | 665 | **有害**：RA 翻倍反降（CV 2.24%，远超噪声） |
| RANDOM（关 RA） | 32,209 | −70.5% | 11.89 | 655,360（1.000/op） | 按设计工作：RA 全关→每 op 全设备延迟 |
| WILLNEED（整文件） | 110,033 | +0.6% | 1.80 | 709 | 中性：+0.6% 在 3×CV(2.37pp) 带内不可归因 |
| NOREUSE（cold） | 109,345 | −0.04% | 1.82 | 680 | 无效应；pass 后驻留仍 100% |
| NOREUSE（warm） | 1,045,542 | +0.06% | 0.95 | 0 | 无效应；**驻留 100%——该 flag 在本内核为 no-op** |
| SEQUENTIAL @D16 | 105,284 | +2.6% vs D16 基线 | 3.70 | 1318 | 与 D16 基线 CV 带重叠，不可归因 |
| RANDOM @D16 | 3,942 | −96.2% | 54.65 | 1.000/op | 同 D1：关 RA 毁灭性 |

DONTNEED 即本研究的 cold 机构本身（§2，100% 有效）。**回答研究问题**：在本栈上
唯一"真正减少总机器工作"的缓存策略是**默认 RA**；SEQUENTIAL（教科书建议）实测
有害 −38%，WILLNEED/NOREUSE 无可观测收益，RANDOM 只在确属随机访问时使用。
"readahead 过量与污染"维度：单文件整 pass 形态下 **RA 零字节浪费**（设备字节恒
等于文件大小）；竞争性工作集的污染形态在本装置（62 GB RAM 空机）无法诚实构造，
判 NOT_CONSTRUCTIBLE，未报告任何污染结论。

### RQ-C：RWF_NOWAIT 适用条件与 EAGAIN 成本（preadv2+RWF_NOWAIT，含 blocking 回退）

| 格 | EAGAIN 率 | ops/s | CPU/op µs | instr/op |
|---|---|---|---|---|
| warm btrfs 4k D1 | **0 / 131072** | 964,690 | 1.03 | 129（基线 102，**+27**） |
| warm btrfs 4k D16 | 0 | 7,731,836 | 2.04 | 130 |
| cold btrfs 4k D1 | 2–27 / 131072（**≤0.02%**） | 97,255（CV 18.3%） | 1.78 | 129 |
| cold btrfs 4k D16 | ~1,900 / 131072（**1.4–1.6%**） | 103,262 | 4.38 | 131 |
| tmpfs 4k D16 | **EOPNOTSUPP（errno 95）首 op 即拒** | — | — | — |

- **适用条件**：btrfs 支持（FMODE_NOWAIT），tmpfs 拒绝（EOPNOTSUPP）——负条件实测。
- **EAGAIN 成本**：在 RA 覆盖的顺序流上 EAGAIN 罕见（0.02%/1.5%，RA 竞速使页
  大多已驻留），重试（2 syscall）在测得速率下成本可忽略；"EAGAIN 风暴"担忧在
  buffered+RA 场景不成立。真随机冷流未测（装置按需可加，未列入本轮）。
- **NOWAIT 的真实代价在温路**：+27 instr/op（129 vs 102，CV 0.01%，判定位）——
  若 Sluice 未来考虑 NOWAIT 类机制，温路固定开销先于 EAGAIN 风险。
- 口径限制：本测量是 preadv2 syscall 语义，**不是 io_uring NOWAIT 路径**。

## 7. 深化格（≤6，只加在关键格）

1. **W02-uring-4k-d4**：600,421 ops/s、CPU/op 1.67 µs、instr/op 4961、p50 5.4 µs
   ——**吞吐与 CPU/op 双最优深度**（d1 用户态 6407 instr 未摊销；d4 已全额摊销）。
2. **W02-uring-4k-d64**：526,703 ops/s（<d16）、instr/op 6300（+27% vs d16）、
   p50 118 µs——**过深反降**（scope/路由扫描随深度增长；与 #595 历史 d64 形状不同，
   本内核/本 FS 新基线下 d64 无收益）。
3. **W02-tp-4k-d16-w1**：312,744 ops/s、CPU/op 5.72 µs、instr/op 5491——单 worker
   时 CPU/op 最低（无 worker 竞争）但吞吐 −15%。
4. **W02-tp-4k-d16-w8**：342,228 ops/s、CPU/op 10.66 µs（w4 的 +23%）——**worker
   超配反降**（work_mtx_/core 竞争）；instr/op 几乎不变（5062→5186），增量全在
   等待/自旋，不在用户态指令。
5. **O_DIRECT 深度阶梯 D2/D4/D8**（§5.2）：悬崖定位于 D1→D2（35,176→5,884），
   D4+ 平台 ~3.7K；1.0 I/O/op 恒定排除 RA/合并混淆。
6. **tmpfs-O_DIRECT 对照格**（8.9M ops/s，instr/op 103）：7.3.3 shmem O_DIRECT
   ≈ 页缓存速度——"绕过页缓存"在 tmpfs 上不是真绕过（内存内拷贝）。

（split-owner W-03 形态已入筛选矩阵 §4：btrfs 269,673 vs tmpfs 276,076 ops/s，
punt 率 0 vs 1.0001/op。）

## 8. 跨 harness 观察（仅假设登记，非受控结论，供 P1-A 提问）

- 同为 btrfs warm 4K 页缓存命中：raw pread D1 = 0.95 µs CPU/op（102 instr），
  Sluice uring d1 = 1.82 µs（6407 instr，3-phase 合成口径）——Sluice 请求机制
  在 D1 的增量假设为 ~0.9 µs/op 量级（口径不同构，需 P1-A 受控配对才能裁决）。
  D16 下 Sluice uring 1.75 µs **低于** 16 线程 pread 探针的 2.04 µs——滑窗提交
  的批摊销假设优于线程池形状。
- Sluice 内部对照（同 bench 同格同 FS）：threadpool 8.68 vs uring 1.75 µs CPU/op
  （5.0×）——两者都是 buffered+页缓存命中，差异纯属请求执行形态（1 pread/op +
  worker 调度 vs SQE 提交 + 内联完成），与页缓存无关。

## 9. INCONCLUSIVE / NOT_CONSTRUCTIBLE / 口径限制清单

| 项 | 状态 | 原因 |
|---|---|---|
| Sluice bench 侧随机格 | NOT_CONSTRUCTIBLE | W-02/W-03 偏移严格顺序（bench_w02_pipeline.cpp:340-347, 342, bench_w03_external_loop.cpp:342），case 列表冻结；随机维度由探针 §5 承担 |
| Sluice bench 64K×D1 格 | NOT_CONSTRUCTIBLE | 冻结 case 列表无 64k-d1；探针已有 64k-d1 对照 |
| Sluice bench btrfs cold 格（全部） | NOT_CONSTRUCTIBLE | write_fixture 每调用重写并驻留页缓存，无 hook（§2） |
| bench D2/D3 格 | NOT_CONSTRUCTIBLE | 冻结 case 列表无（#595 轮的 d2/d3 不在 master case 列表） |
| OD 深度悬崖机制 | INCONCLUSIVE | 设备 FTL vs btrfs DIO 串行化未分辨（§5.2） |
| cold buffered seq D16 反降机制 | INCONCLUSIVE | 探针形状/设备/FS 三者未分辨 |
| od-65536-seq-d1 | INCONCLUSIVE（数值） | CV 35.66%（5 reps 中 1 个离群慢 rep），均值不可引用；只引 1.0 I/O/op 与 instr/op |
| readahead 竞争污染形态 | NOT_CONSTRUCTIBLE | 空机 62 GB RAM 无竞争工作集；未报告污染结论 |
| nw-tmpfs 格 | NOT_RUN（装置事实） | tmpfs 拒绝 RWF_NOWAIT（EOPNOTSUPP），吞吐数字不可构造；适用条件负例已登记 |
| task-clock:u 列 | 不引用 | 与 rusage 口径不一致（§3-2） |
| tmpfs 格 #622 punts/op 精确等值 | 已复现 | 1.0001/op（540028/540028），7.2.11→7.3.3 该形态未变 |

## 10. 给 P1-A 的 regime 推荐（按归因价值排序）

1. **W02-uring-4k-d4 @ btrfs warm**（吞吐/CPU 双最优深度 + 用户态摊销完成点）：
   Sluice 请求机制增量的最干净归因起点（Δ vs raw pread 假设 ~0.9 µs/op）。
2. **W02-uring-4k-d16 @ tmpfs vs btrfs warm（配对）**：punt=1.0001/op 已复现且
   CPU/op 2.7× 差完全落在内核侧——io-wq handoff 成本的受控对照对。
3. **W02-tp-4k-d16-w4 vs W02-uring-4k-d16 @ btrfs warm**：5.0× CPU/op 差，
   全部是请求执行形态（1 pread/op+worker 调度 vs SQE+内联），与页缓存无关——
   threadpool 形态成本的第一张归因靶。
4. **W02-uring-4k-d64 @ btrfs warm**（过深反降 +27% instr/op）：scope/路由 O(D)
   扫描份额（H3 系列）在本内核新基线下的直接测量格。
5. **W02-tp-4k-d16-w8（worker 超配反降）**：CPU/op +23% 而 instr/op 不变——
   分离"锁竞争等待"与"用户态指令"的天然对照格。

## 11. 证据文件清单（全部在 /tmp/sluice-p0c/，不入仓库）

- `env-snapshot.txt`：内核/CPU/挂载/governor/工具快照
- `governor/{original,set-verify,restore-verify}.txt`：governor 三列存档（20/20 恢复验证）
- `freq-samples.csv`：17614 样本 × 20 CPU 频率记录（0.2s 间隔）
- `build-profiles.log`：build_profiles.sh 全输出与 sha256
- `raw/bench/*.jsonl`：每格 10 records（1 self-test + 9 reps）；`*-punt.jsonl` 为 bpftrace 窗口内运行
- `raw/probe/*.jsonl`、`*-lat.jsonl`：探针吞吐/延迟记录（含 nowait/eagain 字段）
- `logs/perf-<cell>-{full,null,r*}.txt`：perf stat -x, 原始输出（instructions:u/cycles:u/task-clock:u/cs:u）
- `logs/punt-<cell>.txt|.err`：bpftrace 计数（submit_req/queue_async_work/local/twr/complete/pread64）
- `logs/probe-audit-<cell>.txt`：逐 rep rc/cache_res_pct/diskstats deltas
- `logs/cache-<cell>.txt`：cachectl warm/drop/stat 全记录
- `logs/diskstats-<cell>-B*.txt`：bench perf run 的 diskstats deltas
- `src/{cachectl,ioprobe}.c`、`*.sh`、`punt.bt`、`analyze.py`：全部装置与驱动
- 数据集：`/home/jnhu/p0c-scratch/data/p0c-rand-512m.bin`（sha256 b5c6fc41…，+C，未压缩）、
  `/tmp/sluice-p0c/tmpfs-rand-512m.bin`（sha256 be580972…）

复现入口：`bash bench/build_profiles.sh release`；单格
`SLUICE_BENCH_TMP_DIR=<dir> SLUICE_BENCH_JSONL=<out> SLUICE_BENCH_WARMUP=2 SLUICE_BENCH_MEASURED=7
SLUICE_BENCH_CASE_FILTER='=<case>' <build-bench-ring>/linux/x86_64/release/bench_w02_pipeline`；
探针 `bin/ioprobe <file> <bs> <seq|rand> <D> [--od|--nowait|--fadvise=X]`；
状态 `bin/cachectl warm|drop|stat <file>`。
