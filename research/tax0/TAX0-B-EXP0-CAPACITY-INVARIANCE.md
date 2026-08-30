# TAX-0B / EXP-0 — Capacity invariance: does the tax exist, and how large is it?

Issues: #227 (sole execution roadmap) · #250 (TAX-0 execution) · #251 (TAX-0A review) · #252 (TAX-0A freeze)
Preregistration: TAX-0A §17 EXP-0 (frozen @ `5537187` via #252) + the TAX-0B/EXP-0 task brief
Experiment PR: #253 (`research/tax0b-exp0-capacity`)
Canonical evidence (runner-produced, `perf-evidence-validate.py` kind `tax0capacity`):
[`tax0b-exp0-threadpool-tmpfs.json`](../../docs/results/performance-attribution/tax0b-exp0-threadpool-tmpfs.json) ·
[`tax0b-exp0-uring-tmpfs.json`](../../docs/results/performance-attribution/tax0b-exp0-uring-tmpfs.json) ·
[`tax0b-exp0-threadpool-btrfs.json`](../../docs/results/performance-attribution/tax0b-exp0-threadpool-btrfs.json) ·
[`tax0b-exp0-uring-btrfs.json`](../../docs/results/performance-attribution/tax0b-exp0-uring-btrfs.json) ·
[`tax0b-exp0-threadpool-tmpfs-w4-replication.json`](../../docs/results/performance-attribution/tax0b-exp0-threadpool-tmpfs-w4-replication.json) ·
diagnostics: [`tax0b-exp0-diagnostics/`](../../docs/results/performance-attribution/tax0b-exp0-diagnostics/)

---

# 1 Verdict

**EXP-0 PASS — CAPACITY-DEPENDENT TAX MATERIAL**

按预注册判定规则(任务 §21):**Uring 后端的每 op 成本随闲置容量 C 单调上升,且在现实容量区(C=128)cycles/op 上升 ≥5%、delta ≫ 离散度,tmpfs 主矩阵 +8.5%、btrfs 对照 +11.8%,双文件系统可复现** → 判定 C(判定规则 C)。**ThreadPool 后端容量不变**(instr/cycles/wall 全部在噪声内,双文件系统 + W=4 复现三重确认)→ 对 TP 臂适用判定规则 A:不得仅凭 `resolve_completion` 的 O(C) 源码复杂度去优化它。

```
TP:    instr/op slope b_TP     ≈ -0.02..-0.07 / C   (噪声级,符号在 fs 间翻转; W=4 复现 -0.003)
URING: instr/op slope b_URING  ≈ +6.0 / C           (R² = 1.0000, tmpfs 与 btrfs 均然)
delta_b = b_URING - b_TP ≈ +6.0 instr/op per unit C
C=8→C=128 (现实区):  uring instr +16.1% (tmpfs) / +17.7% (btrfs); cycles +8.5% / +11.8%
C=8→C=512:           uring instr +67.1% / +74.2%;   cycles +29.3% / +41.0%
```

本 PR **未改任何生产代码、未做任何优化**;因果归属(哪一条 O(C) 扫描产生 delta_b)不在本实验证明范围内(§18)。

# 2 Preregistration authority

- 假设与矩阵在测量前冻结:TAX-0A 报告 §17 EXP-0(静态拓扑审计冻结件,`5537187`,经 #251 人类对抗性 review 后 FC-1..FC-7 修正、#252 合并)+ TAX-0B/EXP-0 任务简报。
- 实现冻结:harness/runner/validator 提交于官方测量**之前**(commit `77063e5`,DRAFT PR #253 先开、后测量);种子、reps、warmup、materiality 阈值均写入 commit message 与 PR body。
- 冻结的假设:H0(容量不变)/ H1-TP(`resolve_completion` O(C) 每 await)/ H1-URING(另有 `find_live_router_cookie_` O(C) 每 CQE)。
- 执行顺序由预声明种子 `0x54415830` 在测量前生成并逐字存档于每个 artifact 的 `execution_order` 块。

# 3 Experiment head SHA

```
BASE:                 a66c0cd7e5b1b57f28b2dbed3ed56c69778bbafa (master, == origin/master)
EXPERIMENT-FREEZE:    77063e524af2c6477c82a893b9dac9a491edf700 (commit 1: harness+runner+validator)
BINARY sha256:        896995e1…(全 5 个 artifact 一致——binary_provenance 逐 artifact 绑定)
BRANCH:               research/tax0b-exp0-capacity  ·  PR: #253 (DRAFT)
```

**dirty 标志归因(诚实记录)**:TP-tmpfs artifact `dirty=false`;其后的 uring-tmpfs 与两个 btrfs artifact `dirty=true`——脏内容**仅为先跑矩阵落在工作树里的未跟踪证据 JSON**(`git status` 可复现),源码零改动,5 个 artifact 绑定同一 git SHA 与同一二进制 sha256。官方矩阵自 TP-tmpfs 起连续执行,无中途源码触碰。

# 4 Environment

实测采集(任务 §9;不照抄任务模板),与 TAX-0A §3 环境一致:

| 项 | 值 |
|---|---|
| OS / kernel | Fedora 44 KDE,`7.1.9-200.fc44.x86_64 PREEMPT_DYNAMIC` |
| CPU | Xeon E5-2666 v3 @ 2.90GHz,1 socket / 10 cores / 20 threads,SMT on,单 NUMA node |
| 拓扑 | CPU 0-9 = 物理核 0-9;CPU 10-19 = SMT 兄弟(`lscpu -e` 快照存于各 artifact `environment_extra.lscpu_extended`) |
| governor | schedutil(intel_cpufreq) |
| Placement | `taskset -c 0,2,4,6`(4 个互非兄弟的物理核;整个 benchmark 进程树含 perf 被钉在该集) |
| 文件系统 | tmpfs(`/tmp`,主矩阵)+ btrfs `/home` SATA SSD(对照;页缓存温热,非冷设备 I/O) |
| liburing | repo 构建固定 xrepo **2.14**(系统 rpm 2.13 共存,未参与链接) |
| 编译器 / perf | clang 22.1.8(Fedora 22.1.8-4.fc44)· perf 7.1.9,`perf_event_paranoid=2`(user-mode 计数非特权可用) |
| 负载环境 | 官方矩阵运行时 loadavg ≈ 0.09(空闲) |

每个 artifact 内含 `environment_id`(env 指纹哈希)与 `environment_extra`(governor/SMT/taskset/lscpu/findmnt/liburing 采集,全部在官方执行**前**探测)。

# 5 Benchmark implementation

- **Harness**:`bench/tax0_capacity_bench`(`77063e5` 新增;`e1_abstraction_tax_bench` 刻意保持 capacity==depth 不动)。生产路径:E1-L2 形态——`ApplicationRuntime`(scheduler workers=1)× 真实 `ThreadPoolBackend{C, W}` 或真实 `UringAsyncBackend{C, Q}`,单任务驱动 depth-8 submit/await 流水线。C 与 D 解耦;uring 无真实 ring 即 exit 3。
- **Same-work(fail-closed)**:splitmix64 主块(与 E1 同族同种子);每 rep 必须恰好 `ops=65536`、256 MiB、word-sum 全等;短 I/O / 零进展 / 任何拒绝 → exit 3,无哨兵性能值。`same_work` 机械断言存于每个 artifact。
- **测量模型**:每个测量行 = 一个全新 bench 进程(`--reps 1 --warmup 0`)在 `taskset perf stat -x, -e instructions:u,cycles:u,branches:u,branch-misses:u,cache-misses:u` 下运行;计数为**进程级聚合**(含进程启动/Runtime 构建/teardown 的固定成本——跨 C 格完全相同,按 op 摊销,从未扣减,如实记录)。wall/user/sys 由进程内 steady_clock+getrusage 只计 rep 窗口(lifecycle 单独记录)。
- **顺序随机化**:11 轮,每轮为 C∈{8,32,128,512} 的一个新排列,`random.Random(0x54415830).sample` 于测量前生成,执行序随 artifact 存档;runner 不依据计时重排。warmup=2 轮(全容量、不测量、无 perf)。
- **官方命令**(逐字):

```sh
# 矩阵 1/4 — ThreadPool tmpfs(主)
python3 scripts/bench/perf-attribution.py tax0 --backend threadpool --op read \
  --file /tmp/tax0b_exp0_read.bin --fs-label tmpfs --taskset "0,2,4,6" \
  --capacities 8,32,128,512 --depth 8 --workers 1 --request-size 4096 \
  --total-bytes 268435456 --reps 11 --warmup-rounds 2 --seed 0x54415830 \
  --output docs/results/performance-attribution/tax0b-exp0-threadpool-tmpfs.json --note "OFFICIAL …"
# 矩阵 2/4 — Uring tmpfs(主):同上,--backend uring --uring-queue-depth 8(去掉 --workers)
# 矩阵 3/4 — ThreadPool btrfs(对照):同 1/4,--file /home/jnhu/tax0b_exp0_read_btrfs.bin --fs-label btrfs
# 矩阵 4/4 — Uring btrfs(对照):同 2/4,--file /home/jnhu/tax0b_exp0_read_btrfs.bin --fs-label btrfs
# 二级复现(主判定后,任务 §6 预授权,不混入主结果):同 1/4,--workers 4
```

诊断层(sudo,单独调用,不参与判定;raw 输出存 `tax0b-exp0-diagnostics/`):

```sh
echo <sudo> | sudo -S taskset -c 0,2,4,6 perf stat -x, \
  -e task-clock,context-switches,cpu-migrations,cycles,instructions -- \
  build/linux/x86_64/release/tax0_capacity_bench … (--backend × {8,512})
```

# 6 Same-work proof

每个 artifact 的 `same_work.valid=true`(runner 机械断言,validator 复核):全部行 ops=65536、bytes=268435456、depth=8、request_size=4096、同一确定性 pattern(word-sum 逐行全等)、`semantic_validation=true`、child exit 0。任何一列不等即 runner abort、EXP-0 判 INVALID——未发生。5 个 artifact 共 220 行全过。

# 7 Execution order

种子 `0x54415830`,11 轮;首三轮(完整 11 轮见任一 artifact `execution_order.rounds`):

```
round 1: [128, 8, 512, 32]   round 2: [512, 8, 32, 128]   round 3: [32, 512, 128, 8]
```

四个主矩阵 + W=4 复现使用同一种子与同一容量列表,故执行序完全一致。artifact 中 `execution_order.flat` 与各 `rows[i].execution_order_index=i` 一一对应(validator 强制)。

# 8 ThreadPool tmpfs(主矩阵)

| C | instr/op med | MAD | cycles/op med | MAD | wall ns/op med | MAD | user ns/op med | sys ns/op med |
|---|---|---|---|---|---|---|---|---|
| 8 | 4,744.3 | 92.1 | 6,512.7 | 63.9 | 2,759.2 | 147.6 | 3,124.5 | 1,931.1 |
| 32 | 4,761.6 | 67.9 | 6,196.2 | 95.0 | 2,681.7 | 86.0 | 3,095.4 | 1,926.2 |
| 128 | 4,768.4 | 69.9 | 6,412.7 | 152.2 | 2,851.0 | 168.2 | 3,092.5 | 2,078.4 |
| 512 | 4,745.2 | 62.0 | 6,319.0 | 153.5 | 2,774.9 | 165.5 | 3,094.8 | 2,017.3 |

vs C=8(绝对 / 百分比;永不只报百分比):

| C | instr/op | cycles/op | wall ns/op |
|---|---|---|---|
| 32 | +17.3 (+0.4%) | −316.5 (−4.9%) | −77.6 (−2.8%) |
| 128 | +24.1 (+0.5%) | −100.1 (−1.5%) | +91.8 (+3.3%) |
| 512 | +1.0 (+0.0%) | −193.7 (−3.0%) | +15.6 (+0.6%) |

斜率:instr b=−0.019(R²=0.13)· cycles b=−0.112(R²=0.04)· wall b=+0.074(R²=0.06)。
解读:无单调趋势;个别负 delta(C=32 cycles −4.9%)与 btrfs 同格(+0.2%)方向不一致,属离散度内噪声(§15)。

# 9 Uring tmpfs(主矩阵)

| C | instr/op med | MAD | cycles/op med | MAD | wall ns/op med | MAD | user ns/op med | sys ns/op med |
|---|---|---|---|---|---|---|---|---|
| 8 | 4,502.3 | 4.9 | 3,849.6 | 94.4 | 3,682.3 | 179.0 | 1,467.6 | 3,908.9 |
| 32 | 4,644.6 | 7.9 | 3,972.8 | 55.4 | 3,837.2 | 218.4 | 1,512.5 | 3,878.4 |
| 128 | 5,225.4 | 4.5 | 4,177.2 | 53.8 | 3,807.0 | 165.5 | 1,589.7 | 3,944.4 |
| 512 | 7,523.1 | 5.6 | 4,976.1 | 92.7 | 4,088.4 | 279.6 | 1,921.5 | 3,958.5 |

| C | instr/op | cycles/op | wall ns/op |
|---|---|---|---|
| 32 | +142.3 (+3.2%) | +123.2 (+3.2%) | +154.9 (+4.2%) |
| 128 | +723.1 (+16.1%) | +327.6 (+8.5%) | +124.7 (+3.4%) |
| 512 | +3,020.8 (+67.1%) | +1,126.6 (+29.3%) | +406.1 (+11.0%) |

斜率:instr b=+5.994(R²=**1.0000**)· cycles b=+2.162(R²=0.9957)· wall b=+0.685(R²=0.8843)。
解读:强单调、近完美线性;instr MAD ≈ 5(近乎确定论)使趋势远超离散度(cycles C=128 delta ≈ 6×MAD)。

# 10 btrfs control(对照,SATA SSD,页缓存温热)

**ThreadPool btrfs**:instr/op med 4,805.2 / 4,809.1 / 4,791.7 / 4,771.0(C=8/32/128/512),斜率 b=−0.071(R²=0.93,绝对量 ≈ 34 instr/op 全程,即 ≤0.7%);cycles ±0.7% 内;wall −3~−5%(无方向一致性)。**平坦幸存**。

**Uring btrfs**:

| C | instr/op med | MAD | cycles/op med | MAD | wall ns/op med | MAD |
|---|---|---|---|---|---|---|
| 8 | 4,073.9 | 0.0 | 2,423.1 | 3.4 | 1,754.9 | 15.2 |
| 32 | 4,217.9 | 0.0 | 2,483.8 | 6.2 | 1,778.3 | 13.1 |
| 128 | 4,794.0 | 0.0 | 2,708.9 | 4.1 | 1,856.9 | 3.4 |
| 512 | 7,098.3 | 0.0 | 3,417.0 | 10.7 | 2,097.7 | 9.2 |

vs C=8:C=128 instr +720.1(+17.7%)/ cycles +11.8% / wall +5.8%;C=512 instr +74.2% / cycles +41.0% / wall +19.5%。斜率 instr b=+6.001(R²=**1.0000**)。
解读:**容量依赖的用户态指令趋势在另一存储形态完整幸存**,且量值与 tmpfs 几乎相同(b 6.001 vs 5.994)——与"税在用户态代码、不在存储层"一致。btrfs 臂整体更快(更热的页缓存路径),但不改变斜率。instr MAD=0.0:固定容量下每 op 指令数逐 rep 完全一致(确定论执行),趋势非噪声。

**W=4 二级复现**(TP tmpfs,独立 artifact):instr/op med 4,451.5 / 4,447.4 / 4,450.1 / 4,448.3,斜率 b=−0.003;C=128 cycles +2.5%(噪声内)。TP 平坦不是 W=1 特例。

# 11 Instructions/op analysis

- **TP 臂**:四个格在两个文件系统上全部落在 ±0.7% 内,斜率符号翻转(tmpfs −0.019 / btrfs −0.071 / W=4 −0.003)→ 无可复现容量趋势。
- **Uring 臂**:分段增量对 C 几乎严格成比例(tmpfs:+142/+581/+2,298 对 ΔC=24/96/384,即 5.93/6.05/5.98 instr·op⁻¹·C⁻¹),R²=1.0000。b≈6.0 与"每 CQE(或每 op)一次 O(C) 扫描、每槽 ~6 条指令(load+compare+loop 开销)"的量级一致——**这是事后解释,不是因果证明**(§18)。
- 诊断层佐证:kernel-inclusive(含 :k)instr 斜率 = (860,789,735−662,094,373)/65,536/(512−8) ≈ **6.02** instr/op/C,与 user-mode 官方斜率 5.99 一致 → 容量税**全部位于用户态指令**,内核侧指令数对 C 平坦。
- 可选事件(branches/branch-misses/cache-misses)随 artifact 存档;C=512 无超线性拐点(router 数组 ~512×32B=16KiB,L1/L2 内),线性即全部行为。

# 12 Cycles/wall analysis

- **TP**:cycles 增量 ±5% 内且跨 fs 无一致方向;wall 同。无 material 趋势。
- **Uring tmpfs**:cycles C=128 +8.5%(delta 327.6 ≈ 6×MAD 53.8)——达到并超过预注册 material 阈值;wall C=128 +3.4%(阈值下),C=512 +11.0%。btrfs 对照 cycles +11.8% / wall +5.8%。**判定规则 C 的第 1 条(cycles ≥5% 且 delta ≫ 离散度)在两个文件系统上同时满足**;第 2 条(wall)在 tmpfs 未达 5%,不构成必要条件。
- 每周期指令效率:C=512 处 instr +67% 但 cycles 只 +29% → 扫描是高 IPC 的紧凑循环,部分被超标量吸收——记录为事实,不做进一步推断。

# 13 Capacity slope

对 instructions_per_op 中位数做最小二乘(y = a + b·C,描述性,不推断渐近复杂度):

| 臂 | a | b | R² |
|---|---|---|---|
| TP tmpfs | 4,758.0 | **−0.019** | 0.13 |
| TP btrfs | 4,806.3 | **−0.071** | 0.93 |
| TP tmpfs W=4 | 4,449.8 | **−0.003** | 0.14 |
| URING tmpfs | 4,454.9 | **+5.994** | 1.0000 |
| URING btrfs | 4,025.9 | **+6.001** | 1.0000 |

```
delta_b(tmpfs) = 5.994 − (−0.019) = +6.013 instr/op per unit C
delta_b(btrfs) = 6.001 − (−0.071) = +6.072 instr/op per unit C
```

允许的结论:"Uring shows a larger capacity-dependent userspace instruction slope"(两个文件系统、R²=1.0000、W 复现)。**尚不允许**:"router scan causes delta_b"——那需要因果隔离(§18/§19)。

# 14 Cross-backend comparison

任务 §22 四种情形中命中:**TP flat + Uring slope → promote router-specific causal experiment**。补充结构信息:TP 与 URING 共享同一 await/`resolve_completion` 路径;TP 臂平坦 ⇒ 共享路径在本几何下对容量不敏感(要么该扫描未在每 op 热路径上执行,要么其常数小到不可测——区分这两种机制解释同样属于因果实验);因此 delta_b 的**候选**集中于 uring 特有的每 CQE 机制(`find_live_router_cookie_`)。此为假设排序,不是归属结论。

# 15 Noise and limitations

- **离散度**:TP instr MAD 27-92(0.6-2%),uring instr MAD ≤8(btrfs 为 0.0——确定论);uring 趋势/噪声比 ≥ 数百倍。TP 臂个别 ±5% cycles 偏移跨 fs 不可复现,判为噪声。
- **进程级计数**:instr/cycles 为进程聚合(含启动/Runtime 构建/teardown 固定成本)。固定成本跨格相同,摊销于 65,536 op;C 相关的一次性构造成本(如 512 个 slot 初始化)量级 ~50K 指令 ≈ 0.8 instr/op,远小于观测斜率。
- **governor schedutil + 温热页缓存**:wall/cycles 受频率与缓存状态影响;这正是以 instructions/op 为主指标的原因(instr 对频率不敏感,趋势在两个 fs 上一致)。
- **btrfs 对照为页缓存温热**,非冷设备 I/O;不声称任何 NVMe/冷存储结论。
- **诊断层为单次运行**(sudo perf stat ×4):只作方向性佐证(税在用户态、ctx-switch/迁移与 C 无关:TP 6.7-7.9K、uring 15.6-16.1K,迁移 ≤2),不入统计。
- **火焰图未采集**:Release 二进制无帧指针且已 strip(`readelf` 无 .symtab);TAX-0A §17 预注册把火焰图归 EXP-2 的一次性诊断构建("diagnostic builds 不入证据库")。bpftrace 本机未安装(perf 计数器已覆盖其目标问题)。
- **几何边界**:D=8、4 KiB read、W=1(主)、Q=8、单 fiber 流水线。workers 轴、write、更大 D/Q、backlog(EXP-U1 目标)均未测。

# 16 Hypothesis adjudication

- **H0(容量不变)**:TP 臂**未被拒绝**(三重确认:两 fs + W=4);uring 臂**被拒绝**(R²=1.0000 的单调上升,delta ≫ MAD)。
- **H1-TP(resolve_completion O(C) 使 TP 成本随 C 上升)**:**NOT SUPPORTED at this geometry**——TP 斜率为噪声级(≤0.07 instr/op/C 绝对量 ≤0.7%)。按预注册规则 A(对 TP 臂):不得据此优化 `resolve_completion`。
- **H1-URING(uring 上升更陡,双 O(C) 机制)**:**SUPPORTED**——b≈+6.0/C,R²=1.0000,双 fs;诊断层显示增量全在用户态。注意:该假设只预测"更陡",不指定两机制各自的贡献占比;"哪条扫描产生 delta_b"未证明。
- 任务 §3 假设书写纪律复核:测量前没有任何一条被描述为 bottleneck;本报告的机制语言均标明"事后解释/候选"。

# 17 Materiality adjudication

预注册规则(任务 §21 C):现实区 C=32/128;material 若 cycles/op C=8→C=128 ≥5% 且 delta 明显大于离散度,**或** tmpfs wall/op 在 C=128 可复现 ≥5%,**或**指令增幅大到用户态 CPU 贡献不可忽视。

| 检验 | tmpfs 主 | btrfs 对照 | 判定 |
|---|---|---|---|
| cycles C=128 | **+8.5%**(≈6×MAD) | **+11.8%**(≈70×MAD) | **满足** |
| wall C=128 | +3.4% | +5.8% | tmpfs 未达 5%(非必要条件) |
| instr C=128 | +16.1% | +17.7% | 满足(第 3 条) |

**判定:CAPACITY-DEPENDENT TAX MATERIAL(限 uring 后端、本几何)**。C=512 的 +67-74% instr / +29-41% cycles 进一步确认这不是只有极端容量才出现的形态。TP 臂按规则 A 登记:税不存在(在其上),优化优先级降级。

# 18 What is NOT proven

1. **因果归属**:delta_b 由哪条 O(C) 扫描产生(`find_live_router_cookie_` vs `resolve_completion` vs 其他)——需要消融/插桩的因果实验;TP 平坦只把候选**排序**,未定罪。
2. **机制细节**:b≈6.0 是否恰为"每 CQE 全容量扫描 × ~6 指令/槽"——量级一致,但未做逐函数计数。
3. **未测轴**:workers>1 下的 uring、write、D≠8、Q≠D(backlog,EXP-U1)、真实应用混合负载、更大 C(>512)。
4. **wall-clock 占比归因**(EXP-2)、raw-liburing matched ceiling(EXP-3)。
5. **任何优化收益**:本 PR 无优化,不构成任何"改了会更快"的证据。

# 19 Next experiment

按任务 §22 与 TAX-0A §17 梯子:

1. **EXP-U1(uring dispatch/backlog 剖面)升级为与因果隔离合并设计**:在固定 C、变 backlog 的同时,对 router 扫描做只读遥测(每 CQE 扫描迭代计数,test seam 只读)以直接检验"每 CQE O(C) 扫描 = delta_b"假设——需要单独授权与设计冻结。
2. TP 臂:`resolve_completion`/arena 查找的容量优化候选**降级**(规则 A);若未来 TP 高容量形态出现趋势,再以同 harness 复测。
3. 本 harness(`tax0` runner 子命令 + `tax0capacity` validator kind)可直接复用于上述复测。

---

## 完成声明(AGENTS.md §23)

- **授权范围**:TAX-0B/EXP-0 实验 PR(测量,非优化);生产代码零修改(`include/sluice/**`、`src/**` 未触碰)。
- **Baseline**:master `a66c0cd` == origin/master,clean;实验分支 `research/tax0b-exp0-capacity`,freeze SHA `77063e5`。
- **实际执行**:Release clang + liburing 2.14 构建 PASS;四角冒烟 PASS(非证据);官方 5 artifact(4 主/对照 + W=4 复现)全部 `same_work.valid=true` 且过 `perf-evidence-validate.py`(14 artifact);pre-push gate ALL CHECKS PASSED。
- **SKIPPED**:火焰图(二进制 strip + 无帧指针;EXP-2 预注册范围);bpftrace(本机未安装);W=4 uring(无 worker 轴);sanitizer(定时运行期禁用)。
- **剩余风险**:§15 所列几何边界与进程级计数语义;因果归属未证(§18)。
