# 测量复核审计：P0-C 与 P1-A（e5-fullpath-machine-work）

- 审计：独立测量审计员，2026-11-18。冷眼复核，只读原始输出；本文件是本任务唯一写入物。
- 对象：`p0c-cache-study.md`、`p1a-cost-map.md`；原始输出 `/tmp/sluice-p0c/`、`/tmp/sluice-p1a/`；
  交叉对照 `dag.md`（80 边/22 候选）与 `history-digest.md`。
- 方法：对两份报告的脚本与原始日志**重算**（非复述）：跑 `/tmp/sluice-p0c/analyze.py`（bench+probe 两模式）、
  `/tmp/sluice-p1a/analyze_stats.py`（5 regime）、逐文件比对 perf stat `-x,` 原始行、shim/strace/e1/e2 计数、
  freq-samples/turbostat 统计、governor 三列、符号/annotate 份额、cache/probe-audit/diskstats 状态证据、
  bench 源码锚点与 case 冻结列表、二进制与数据集 sha256。
- 结论分级：**确认**（重算一致）/ **推翻**（数字或标签与原始数据矛盾）/ **降级**（结论方向成立但
  口径/标签/披露不过关，不得按原表述引用）/ **gap**（复核后仍缺的档案）。

## 1. 复核确认（清单 1–7 逐项）

### 1.1 warm/cold 标签与状态证据（清单 1）

- 探针侧每格状态门实装于 `probe_cell.sh`：warm 需 `res_pct≥99.9`（否则 NOTWARM 标记）、cold 每 rep
  `cachectl drop`+`stat` 需 `res_pct≤1`（否则 INCONCLUSIVE 标记）。实测 41 份 `logs/probe-audit-*.txt`
  共 206 条 rep 行、INCONCLUSIVE/NOTWARM 计数 **0**（`grep -c -E 'INCONCLUSIVE|NOTWARM'` 全零）；
  唯一失败 rep 是 `nw-tmpfs`（rc=1，EOPNOTSUPP）→ 205 个有效 rep 与报告 §2 的"205 rep 零标记"一致。
- cold 有效性：`cache-buf-cold-*` 每格 5 条 `drop-residual … resident:0, res_pct:0.0000`（0/131072 页），
  warm 格恒 `100.0000`。**未发现任何无状态证据的冷缓存胜利**；bench 侧 btrfs cold 判
  NOT_CONSTRUCTIBLE 的机制前提（`main()` 先 `write_fixture`，bench_w02_pipeline.cpp:613-619；
  偏移严格顺序 :340-347；case 冻结列表无 64k-d1/d2/d3，:661-672 逐行核实）成立。
- bench 侧 warm 的直接验证：`logs/diskstats-B02-*-B*.txt` 全部 Δ=0（例 `75091 6081904 -> 75091 6081904`）；
  全程未碰 drop_caches（脚本无该调用）。tmpfs 格按"恒驻留"登记有 post-cell `stat=100%` 记录支撑。
- INCONCLUSIVE 纪律：§9 清单（OD 悬崖机制、D16 反降机制、od-65536-seq-d1 数值、污染形态、
  nw-tmpfs NOT_RUN、task-clock 不引用）逐条与原始证据相符；`od-65536-seq-d1` CV 35.66% 复算一致。

### 1.2 headline 数字重算（清单 2；抽验 ≥8 项，实抽 40+）

- **P0-C 探针（analyze.py probe 模式整体复跑，与 §5/§6 表逐格一致）**：
  warm-4k-seq-d1 = 1,044,935 ops/s、102 instr/op、0.95 µs CPU/op、p50 939 ns、cache 100%；
  cold-4k-seq-d1 = 109,393、1.69、102、136 reads/rep；8 个 cold buffered 格 sector 对账合计均为
  **5,242,880 sectors = 512 MiB/rep 整**（零字节浪费，含 rand 格）。RQ-A 七个 Δ% 逐个重算一致
  （SEQUENTIAL −38.31%、RANDOM −70.55%、WILLNEED +0.585%、NOREUSE cold −0.044%、
  NOREUSE warm +0.058%、SEQ@D16 +2.56%、RANDOM@D16 −96.16%），设备 I/O（680/665/655360/709/680）
  与 diskstats deltas 一致；RQ-C EAGAIN：warm d1/d16 = 0，cold d1 = [3,27,2,5,3]（报告"2–27"），
  cold d16 = [1822,1963,2027,1983,1804]（报告"~1,900、1.4–1.6%"），warm +27 instr（129 vs 102）成立。
- **P0-C bench（analyze.py bench 模式复跑，ops/s、CPU/op、wall、p50/p95/p99、CV(ops/s)、CV(instr)
  逐格一致**；instr/op 与 cyc/op 列见 §2-1 推翻项）。
- **P1-A §1 主表（analyze_stats.py 复跑 5 regime）**：ops/s 中位 600,914/584,580/410,477/537,942/345,805、
  CPU/op 1.666/1.711/4.857/1.862/10.345、p50 5,440/25,953/37,513/119,951/44,077、
  instr u+k 6498.58/6492.05/9698.26/7541.18/15757.39、instr:u 3858.45/3842.70/3862.16/4901.03/4033.93、
  instr:k 2639.53/2630.49/5731.63/2628.93/11431.58、cycles u+k/u/k、task-clock、ctx-sw
  （0.231/1.295）**全部与表一致**；B-set branches/branch-misses/LLC-misses 五格 ×3 事件一致，
  min_mux=66.0% 印证"66% enable scaled"口径。分母硬编码 540000（analyze_stats.py:86）正确。
- **计数层**：strace enter 540,028 → 1.000052/op（三 uring 格一致）；tp pread64 540,006 → 1.000011/op；
  tp futex 1,728,258 → 3.2005/op、均值 42 µs（strace usecs/call=42）。bpftrace e1：punts 540,028
  （tmpfs）/28（btrfs 三格），submits 540,028，comps 540,000/540,028，evfd_signals 540,000（1.000/op）、
  1,029,584（1.906/op）、0（tp）；twr_runs 489600（批形 [1]=89.71%、[2,4)=10.27%）、稳态 28（残差）；
  s2c 直方图 [2K,4K)=77.1%（tmpfs）、[512,1K)=95.2%（btrfs d16）；tp preads 540,002。
  Δcycles:k 11213.09−3909.37 = **+7303.7（"+7304"）**、Δtask-clock 4.676−1.807 = **+2.869（"+2.87"）**、
  Δctx-sw **+0.230** 全部对上。shim：locks 1,170,121/1,012,621/1,017,129/973,267/1,131,104 →
  **19.502/16.877/16.952/16.221/18.852 锁/op**，contended 0/0/0/0/**126,203 = 11.16%**，
  最大簇 11.21/op@9.1% + 5.14/op@20.7% 逐簇对上。
- **符号/annotate 层**（big-symbol 12K/12K/13K/51K/176K 样本，Total Lost 0）：段1 五格 headline
  2.03/2.16/0.82/2.63/0.59 = raw 符号和（0.93+0.51+0.42+0.17；1.32+0.32+0.38+0.14；0.45+0.17+0.05+0.15；
  1.68+0.29+0.52+0.14；0.23+0.18+0.07+0.11）；段2 dispatch_after_accept 0.88/0.82/0.40/0.82/0.10；
  段3 dispatch_one_locked 2.34/2.22/1.37/2.16；段6 四格 ≈24.1/24.2/13.9/7.5 逐符号对上
  （rep_movs 19.14/19.19/9.68/5.52、filemap_read 3.10/3.30/1.16、filemap_get_read_batch 1.12/1.02/0.85、
  shmem_file_read_iter 2.61、filemap_get_pages 0.72/0.66/0.56、filemap_get_entry 1.65）；
  段8 M-53 0.14/1.03/1.74/0.30ᵈ 与合计 1.68/2.36/1.19/2.06；段9 offer+release 0.59/0.63/0.26/0.52/0.22；
  段10 publish_one 0.55/0.43/0.15/0.44/0.23；Top10 #9 wait_for 0.87/1.36/3.04、take 0.62/1.08/2.34、
  run_pipeline_phase 2.53–2.98（"2.5-3.0%"）；M-28 行级 **30.24%**（`ann-full-tpw8.txt:371`
  `lock addq $0x1,0x198(%r15)`）× worker_loop 1.83% = 0.5534% 全局 × 30653.5 cyc/op = 169.8 cyc/op
  ≈ 48 ns @3.5 GHz，算术链全对；ᴷ 23.16% 总数对（21.75 + 未列出 1.02+0.39，见 §2-5）。
- **交叉验证**：P1-A §7-3 对 P0-C 的复现对照（ops/s/CPU/op/p50 四格）在两者原始输出内逐值核实；
  §7-4 的分母差发现成立（见 §2-1）；P1-A instr:u 3842.7 与 P0-C 重算真值 3843.9 差 0.03%；
  P0-C tmpfs instr/op 4968×7/9 = 3864.0 vs P1-A 3862.16（+0.5% FS 无关结论成立）。

### 1.3 非相加纪律（清单 3）

- 未发现三量纲（task-clock/cycles/dwell）相加、跨段份额求和冒充总量、或段份额相加进结论的表述。
- P0-C §4.2 核心分解按三列独立比较（instr 差 0.5%、CPU/op 2.7×、ops/s +38%）；P1-A §0 总口径声明、
  §2 "行内证据类型不同不得纵向求和"、ᴷ 脚注"跨符号求和仅作段级定位，仍非相加口径"、Top 10
  "非按采样占比"约束成立。同量纲符号和（ᴷ、段6、段7）均限定为定位用途——纪律守住，
  但其中的枚举/口径问题见 §2-5/§2-6。

### 1.4 PARTIAL / NOT_OBSERVABLE 诚实性（清单 4）

- P1-A §6 逐项与结构事实相符：M-63 owed 门（uring_backend.cpp:658-663）+ W-02 无零长 op →
  循环休眠，判 NOT_OBSERVABLE（bench 形态）而非"零成本证明"；M-70/71 evfd_reads 0–4、E2 n≈32
  （e2 tmpfs bench 13+18+1=32 条）判统计弱；kernel annotate 无 debuginfo；publication 时戳缺 API 字段；
  M-37 NOT_CONSTRUCTIBLE（dispatch 即时清空 + 两处 front() 调用点，DAG 已核）；"strace 永不计时"。
- 族级映射判 PARTIAL（无调用点证据）、M-16 判 PARTIAL（内联限制）、M-53 迁移收益 PARTIAL、
  M-61/62 融合族 PARTIAL——均未把计数/份额冒充为单候选裁决；每条注明"须单因素 A/B"。
- P0-C 推断值检查：判据（mincore/diskstats）均为直接测量；"页缓存命中地板 102 instr/op"是测量值；
  §8 跨 harness 数字全部标注"假设，需 P1-A 受控配对才能裁决"。未发现推断值冒充测量值。

### 1.5 governor 与频率（清单 5）

- P0-C：三列齐（original 20×schedutil → set-verify 20×performance → restore-verify 20/20 match=yes）；
  freq-samples.csv 17,614 行 ×20 CPU（0.2 s，~58.7 min）、中位 3,500,000 kHz、min 1,163,779 kHz
  逐值复算一致。
- P1-A：freq-samples.csv 3,612 行、min 1,168,676 kHz、中位 3,500,000、**36.4% 样本 <3.3 GHz** 逐值
  复算一致；turbostat 3,520 行全程伴随（interval 5）；campaign-header 存 paranoid=2、nmi=1、
  git head `5d94f233`、双 sha256。轮间频率下探已如实披露（与 history-digest §0 的 #621 惯例一致）。
  但"两轮各三列"存证不成立（§2-8）。

### 1.6 Top 10 与 DAG 22 候选（清单 6）

- 逐条映射：#2→候选#1（M-28）、#3→候选#5（M-53）、#5→候选#17（M-62）、#6→候选#16（M-61）、
  #8→候选#19（M-67）、#9→候选#14/#21（M-01/M-74）、#10→候选#11/#12（M-42/M-40）；#1（H1 提交批
  形态）、#4（io-wq punt）、#7（tp-w8 竞争）不是 22 候选成员，报告亦未冒充（分别标注属 #595 系/
  FS 形态选择/现象）。22 条候选中本轮可测者全部有测量落点（Top10 或 §4 逐条），不可测者逐条
  给出 NOT_OBSERVABLE/NOT_CONSTRUCTIBLE/未行使。**无定位证据被当裁决使用**：所有 MEASURED 行
  的消除机制均指向后续 A/B 或归 #595 系，份额仅作定位。
- M-28 的"measured hotspot 前置已落地"表述准确：仅落地前置之一，未宣布候选通过机制门。

### 1.7 复现入口（清单 7）

- 资产在位且一致：release bench sha256 `03dbef09…`（=campaign-header=P1-A §0）；profiling 构建
  `e929c1fe…`；数据集 `b5c6fc41…`（lsattr 实证 `+C`）；W-01 跨 profile `4d4a948a…`（build log 两条）；
  `bin/ioprobe`、`bin/cachectl` 及其源码；governor.sh/cell.sh/redo_e.sh/campaign.sh/analyze_stats.py/
  ann_top.py 均在。bench_cell.sh 的 perf 事件集（instructions:u,cycles:u,task-clock:u,context-switches:u）
  与 §11 一致；60 个 stat full log 全含 `failing repetitions=0`（正确性门实证）。
- `SLUICE_BENCH_CASE_FILTER` 语义核实（bench_common.hpp:492-500）：`'='` 前缀 = 精确匹配，
  "…-d1 是 …-d16 前缀"的顾虑成立且被 '=' 形式解决。
- 限制（如实登记）：全部装置脚本在 /tmp、不入仓库（两报告已声明）；脚本内嵌 sudo 密码，仅本机可重跑；
  P0-C 复现入口未列 punt.bt/perf 调用细节（在脚本内），P1-A 未列 turbostat 调用（在 campaign.sh:38）。
  本机另开 shell 可按 §9/§11 + 脚本重跑；跨机不可。

## 2. 推翻 / 降级（overturned，附原始数据依据）

1. **【推翻·归一错误】P0-C bench 表的 instr/op 与 cyc/op 列（§3-1 自述分母不成立）**：
   `analyze.py:77` `env_reps = max(rep)+1`，而 JSONL measured 段 rep 重编 0–6（B02-uring-4k-d16.jsonl
   10 条 = 1 self-test + 2 warmup(rep 0,1) + 7 measured(rep 0-6)）→ 分母 7×3×20,000 = 420,000；
   perf 窗口实为 warmup+measured = 9 rounds = 540,000 ops（`perf-B02-uring-4k-d16-full1.txt`
   task-clock 995.05 ms ≈ 540,000 × 1.84 µs；P0-C 原始 instructions:u 2,082,072,982 与 P1-A §7-4
   引用逐字节一致）。重算真值：d16-btrfs 3843.9（表 4942 = ×9/7）、d4 3858.6（表 4961）、
   d64 4900.1（表 6300）、tp-w8 4033.7（表 5186）、tp-d1-w1 6514.4（表 8376）；cyc/op 同
   （2814 → 2188.3 = P1-A cycles:u 2190.5 独立一致）。64K 格同错（表 8445 → 真 ≈6569；
   64K request_count=2000，bench_w02_pipeline.cpp:669-670）。**影响**：所有 bench instr/op、
   cyc/op 绝对值 +28.57%；与 §3-1 自述"9 rounds×3 phases×ops"直接矛盾。**不受影响**：列内相对
   比较（d64 "+27% vs d16"、5062→5186 等）、ops/s、CPU/op（rusage per-rep 分母）、wall、时延分位。
   P1-A §7-4 已披露但 P0-C 正文未更正。**处置**：bench instr/op 绝对值降级为"×9/7 膨胀值"，
   不得进入候选裁决；引用时必须除以 9/7 或改用 P1-A 口径。
2. **【降级·标签错误】P1-A §1 "ops/s（JSONL 中位，7 reps）"与 CV 列**：`analyze_stats.py`
   jsonl_summary 按 `case_id.startswith("W02")` 过滤，不排 warmup → n=9（含 2 个 warmup 轮）。
   重算：d4 CV 7.11%（仅 measured 7 rep = **2.34%**）、tp-w8 5.47%（**2.96%**）、d64 1.59%（**1.22%**）、
   tmpfs 中位 410,477（measured-only 410,026）。中位受影响 ≤0.4%（可忽略）；CV 列虚高至 ~2×
   且标签不实。CV 列不得当作 measured-rep 离散度引用。
3. **【降级·混源未标注】P1-A §1 dso 行**：d4/d64 两格与 22-round 大样本重算一致（perf report
   `--sort dso` 复算 = 64.75/21.26/10.94 与 61.18/26.03/10.72）；d16/tmpfs/tp 三格与归档 `-dso.txt`
   （小样本、mtime 03:17:47-52，早于 big.data 完成 03:18）一致，而 d16 大样本实为
   64.57/22.14/10.68 ≠ 表 65.4/22.1/9.8。同一行两种样本量未标注，归档 dso 报告与表不完全对应。
   dso 行仅作粗定位，不作跨格比较。
4. **【推翻·分解数字错，总数对】P1-A §2 段1 括号分解**：d16 "(1.32+0.98+0.38+0.14)" —— raw 为
   scope 1.32 + context 0.32 + accept 0.38 + reserve 0.14（"0.98" 应为 0.32，reserve/accept 两值互换）；
   d64 "(1.68+1.05+0.52+0.14)" —— context raw 0.29（"1.05" 无对应用符号）。五格 headline
   （2.03/2.16/0.82/2.63/0.59）全部与 raw 符号和一致，错仅在分解数字。
5. **【降级·枚举不全，总数对】P1-A ᴷ 脚注**：列出 7 符号和 = 21.75%，而 23.16% 需再加未列出的
   `entry_SYSCALL_64_after_hwframe` 1.02% + `entry_SYSCALL_64_safe_stack` 0.39%（raw 一致）。
   总数正确、枚举不完整。
6. **【降级·内部不一致】tp 内核锁机器总量与 tmpfs punt 机器总量两处口径漂移**：段7 tp 列
   "qsl+futex_wake+__futex_hash+psi+smp_call ≈8.3%" vs Top10 #7 "≈7.1%（2.65+1.40+1.14+1.04+0.86）"
   ——后者与 raw 一致，前者无枚举不可复核；Top10 #4 tmpfs "≈10.5%（…+qsl 3.21）" vs 段7 tmpfs
   "≈8.3%（…+ spin_lock 份额部分，未指明）"。两处均为定位参考，不进裁决。
7. **【推翻·标签错】Top10 #9 "submit_read 1.32→1.68%（d4/d16/d64）"**：raw d4=0.93、d16=1.32、
   d64=1.68 —— 两个数实为 d16/d64，且三深度只给两数。
8. **【推翻·存证声明不成立】P1-A §0/§8/§10 "governor 两轮各三列"**：`governor.sh` 写固定文件名，
   E-redo 轮（mtime 03:18:21-52）覆盖 campaign 轮（~02:59）三列；盘面仅存一轮（20/20 match=yes）。
   campaign 轮的 governor save/readback 证据不可复核。
9. **【降级·引用无档案】P0-C §2 "fincore 实测 pipeline.bin 16384/16384 页驻留"**：`/tmp/sluice-p0c`
   全目录无该 fincore 输出，脚本亦无 bench fincore 调用。bench warm 改由 diskstats Δ=0 承担（成立），
   该句不得作为独立"实测"引用。
10. **【降级·实测未执行】P0-C §2 "shmem DONTNEED 无效（实测 drop 后残留 100%）"**：tmpfs 探针格以
    `state=none` 运行（probe_screening.sh:33-35），cache 日志仅 3 条 stat=100%，**无 drop-residual
    记录**。"恒驻留"登记成立；"DONTNEED 无效"未经执行未经记录。
11. **【推翻·量纲混同】P0-C §5.1 "5,338 ops/s ≈ 本盘 4K 随机极限 ~5.3K IOPS"**：该格设备读
    96,723/24.56 s ≈ **3.9K IOPS**（0.74 I/O/op）；ops/s ≠ 设备 IOPS。"cold rand 受设备 I/O 主导"的
    方向成立，等式不成立。
12. **【降级·色彩数字】P1-A §0 turbostat 摘要**："active CPU Bzy_MHz 3.1–3.4 GHz"（日志全行
    2506–3450；>3000 行为 3001–3450）、"LLC 命中 63–97%"（package 行 75.5–99.2，per-CPU 行
    57.6–99.98，无一口径得 63–97）。无结论依赖，仅登记。
13. **【降级·表述】P0-C §9 "od-65536-seq-d1（5 reps 中 1 个离群慢 rep）"**：raw
    [3484, 3477, 4007, 6131, 2362] —— 一个慢离群**和一个快离群**（双峰）；INCONCLUSIVE（均值不可引）
    判定本身成立。另：§5.2 表无 CV 列，od-4096-seq-d8 CV 12.56% 未披露（"D4+ 平坦"主要靠 d4/d16
    的低 CV 格支撑）。
14. **【降级·琐碎】punt 率舍入不一致**：540028/540000 = 1.000052，P0-C 写 1.0001/op、P1-A 写
    1.0000/op；零实质影响，引用时统一。

## 3. 复核后仍缺失的证据（gaps）

1. P0-C bench fincore（pipeline.bin 16384/16384）原始输出——需补档或把 §2 该句改为 diskstats 依据。
2. P0-C tmpfs DONTNEED-ineffective 的 drop 实测记录（若要保留"实测"措辞需补跑并记录）。
3. P1-A campaign 轮 governor 三列（已被 E-redo 覆盖）——本战役内无法补，登记为披露缺口。
4. P1-A 大样本 dso 报告未归档；§1 dso 行 d16/tmpfs/tp 的来源样本量未标注。
5. M-53 "每条目 ≈7.9 vs 3.6-4 cyc/条" 的分母（平均扫描深度假设，约 D/2）未在报告写明；
   斜率已判 INCONCLUSIVE，但换算口径缺失。
6. P0-C §1 "GS-480/vendor ATA" 在 env-snapshot.txt 无记录（本审计经 /sys/block/sda/device/{model,vendor}
   活体核实为真，但当时快照缺此项）。
7. writeback 背景对照：P1-A §6 "wb_dirty_folio ~3.5K/60s 为全机背景（空闲对照窗同样有）" 的对照窗
   记录未归档（本轮 e1 实测 63–387 个/窗；方向支持"无可归因 writeback"，数字来源缺失）。
8. G0 对照（18.6 锁/op）无本地原始数据（history-digest §6 已登记 #622 证据根缺失），该对照保持
   "记录不裁决"。
9. 复现入口依赖 /tmp 未入库脚本与内嵌 sudo 凭据；仓库内无归档副本——跨 shell/跨机不可完整重跑
   （两报告已声明"不入仓库"，此处登记为复现资格边界）。

## 4. 审计结论

- **P1-A 主干成立**：§1 主表、§3 锁账、§4 各候选量化、段份额 headline、计数层（strace/bpftrace/shim）
  在 540000-op 分母下全部重算一致；非相加与 PARTIAL/NOT_OBSERVABLE 纪律守住；Top 10 与 DAG 22 候选
  对得上且无定位证据越权。降级项均为标签/枚举/混源类，不动摇任何方向性结论。
- **P0-C 探针侧成立、bench 侧 instr/cyc 列被推翻**：探针（含 RQ-A/RQ-C、512 MiB sector 对账、
  205-rep 状态门）全部复核通过；bench 表 instr/op 与 cyc/op 因 env_reps=7 分母错误整体膨胀 9/7，
  绝对值不得引用（相对比较与 ops/s/CPU/op/时延不受影响）。P1-A §7-4 的披露经本审计独立证实。
- 按审计规则：§2-1/2-4/2-7/2-8/2-11 为推翻项，其涉及的原文表述不得进入候选裁决；其余为降级项，
  引用须按 §2 标注的修正口径。
