# E5 全路径机器工作消减战役 — 最终报告（A–J）

- 撰写：2026-11-18（最终报告撰写员会话；本文件是本战役 A–J 收口的唯一汇总文档）
- **修订 r2（2026-11-18，Owner 归档复核后，维持 NO_GO_ABORT 裁决）**：① 因果归因
  修正（§0/§E/§G.6）——静态减法已证实，整机差异超出单指令模型，代码 TU 布局/
  对象布局（成员移除）/RMW 移除对 `work_mtx_` 竞争时序的影响在现有对照下不可
  分离，**根因待确认**；170 cyc/op 保留为采样定位值；② 按门证据强度补强
  （§G.4/§G.6）——1b 为点估计越界 0.096 且 CI 跨接受带下界、AB/BA 分列
  −1.944/−2.247 落在下界两侧；1c 按本 session 种子触发、换种子 2/10 含 0；
  A/A 115 对精度不可移植给 24 对 A/B；③ S3 闭合（§I.1 #16/#19、§I.2）——
  r6 R3 = #16/M-61 发布锁融合（历史拒绝理由挂回 #16）、R2 与 #19 仅部分重叠、
  移除 #19 对 R3 的错引；④ 汇总计数更正（§0/§F/§G.7）——22 候选 = 2 NO_GO
  + 1 GO（排队）+ 14 INCONCLUSIVE + 5 SEMANTIC_MISMATCH；measurement-audit
  14 项 = 5 推翻 + 9 降级。「缺第三轮事前签核，已有事后审计」披露（§G.1/§I.3）
  原样保留。固定 SHA 原始证据归档入口：分支 `evidence/e5-fullpath-machine-work-20261118`
  （`results/e5m28-s1-20261118/archive-entry.md`，含协议哈希、候选 diff、驱动/
  统计脚本与校验清单）。
- 基线：master `5d94f233967c3aef993eb19ba9e3252c532b0951`（全战役冻结，未动）
- 战役证据目录：`docs/evidence/e5-fullpath-machine-work/`（12 份输入文档 + 本报告）
- 实验会话：`e5m28-s1-20261118`，结果根 `/home/jnhu/sluice-e5-elim/results/e5m28-s1-20261118/`
  （已 commit 于 worktree 分支 `research/e5-c1-m28-gate`，evidence 提交 `ae577394`，
  其父 `f350474d` = 登记 CANDIDATE_SHA，基点即 master `5d94f233`）
- **战役最终判词：候选 #1（M-28 syscall_count_ 生产侧宏门控剔除）= NO_GO
  （预注册中止通道 判词 1b + 1c）；产品 PR 未制作；master 零改动**
- 三层引用纪律（本报告全文遵守）：**【裁决】** = 冻结门产出的最终判定；
  **【定位】** = 份额/计数定位证据，不进裁决、不相加；**【回执】** = 机制/门/构建
  的运行回执（oracle、mutant、矩阵、静态三列、动态回执带）。引用格式 `文档 §节`
  或 `路径:行`；实验数字逐字取自结果根文件（本报告撰写会话已逐文件打开核对）。

## 0. 结论（一句话回答）

**本战役没有从生产路径消除任何机器工作**：唯一进入实验的候选（M-28
`syscall_count_` 门控）在静态层精确落地（生产库零 lock 指令）但**整机差异超出
单指令模型**——静态减法已证实，代码 TU 布局、对象布局（成员移除）与 RMW 移除
对 `work_mtx_` 竞争时序的影响在现有对照下不可分离，根因待确认；动态回执越带
+ uring 隔离违例，按预注册中止通道判 **NO_GO**，master 保持基线、产品 PR
不制作。战役净产出是三层证据：**裁决层**（候选 #1 NO_GO、22 候选 + 3 非候选
矩阵定稿、#595 KEEP_IMMEDIATE 维持——均由冻结协议 + 独立实验审计逐位复算
支撑，证据等级最高）；**定位层**（本内核 7.3.3 全路径成本台账与 80 边 DAG，
确认最大的机器工作落在 Sluice 用户态不可减位置：payload 拷贝本体 rep_movs
≈19%、enter 内核骨架 ≈23%、io-wq punt FS 条件性 +7304 cycles:k、tp-w8 worker
竞争——全部 MEASURED 定位级、非相加、不作裁决使用）；**回执层**（正确性四门
全绿、静态三列符合、动态回执越带——机制在静态层成立、系统层失效，是本次
NO_GO 的直接依据）。已证的可减空间仅剩两个错位的 test-only 计数器（#1 已
NO_GO 收口、#11 M-42 排队待 owner 决定是否推进）与 5 条 SEMANTIC_MISMATCH
owner 所有权裁决面（4 条运行时机制面 + 1 条词表卫生面 #22，r2 计数更正）。

---

## A. 仓库现实

来源：`repo-reality.md`（勘察 2026-11-18 00:12–00:35；本会话仅复核终态）。

- 基线 `5d94f233`（"Merge pull request #635…"）与 origin/master 一致，无分叉；
  无 stash；`/tmp` 无 Sluice 残留。
- 保护清单（全战役遵守）：`docs/evidence/621-round2-R1/`（他人 621 r5 在制品，
  唯一起始未跟踪项）；`/home/jnhu/sluice-621-r2/` 下 12 个并行 worktree。
- 构建现实：已保存配置 release + liburing=y（`.xmake/`）；tests.lua 注册测试
  liburing=y 81 个 / n 60 个；组内非注册目标 98 个（含 94 个手工执行 mutant
  构建，永不作常规测试运行）；bench 目标全部非默认、走独立 build 目录
  （`bench/build_profiles.sh`，#520 混链禁令）。
- **战役全程纪律回执**：主仓被跟踪文件零改动——本报告撰写会话终查
  `git -C /home/jnhu/Source/Sluice status --porcelain` 仅两个既存未跟踪目录
  （`docs/evidence/621-round2-R1/`、`docs/evidence/e5-fullpath-machine-work/`，
  后者为战役自身产物）；实验在独立 worktree
  `/home/jnhu/sluice-e5-elim-base`（detached 于 `5d94f233`）与
  `/home/jnhu/sluice-e5-elim`（分支 `research/e5-c1-m28-gate`）进行，harness
  脚本留在 `/tmp/e5-elim-scripts` 不入仓，复现入口存证
  `static/reproduction-entry.txt`。

## B. E5 环境资格（含 governor 处置与恢复）

来源：`env-census.md`（P0-A，判定 **PASS_WITH_LIMITS**）。

- 平台：裸机（`systemd-detect-virt`=none、无 hypervisor flag、真实 DMI），
  Xeon E5-2666 v3（Haswell-EP 10C/20T，2.90 GHz base），内核
  **7.3.3-200.fc44**——与 #595/#622 各轮的 7.2.11 不同，历史数字不可直接比
  （595-pause RESUME §2："内核升级后不承诺同数字"）；/home = btrfs
  `compress=zstd:1` 挂载在 **SATA SSD（非 NVMe）**（装置偏差，全战役如实
  披露，scratch 以 `chattr +C` 排除读路径解压并 lsattr 存证）。
- 频率资格：单核 turbo 稳态 3.44–3.46 GHz（schedutil APERF/MPERF）；
  ≥4 核满载钳制 3.20 GHz（all-core turbo 桶），10 核满载 15 s ×3 读数恒
  3200 MHz、65–69 °C / ~36.7 W，无温度降频；全程未触 BIOS/MSR/turbo/持久化。
- A/A 噪声：确定性用户态负载 `instructions` 计数 CV≈5.1e-6%（近零噪声，
  主口径依据）；wall CV ~8%（时长类指标必须配对 + 频率伴随）。
- **governor 处置与恢复（三轮全过）**：
  - P0-A：schedutil×20 存档 → performance（读回 20/20）→ 重测 → 恢复读回
    `RESTORE-OK: all 20 CPUs == schedutil`；
  - P0-C / P1-A：各轮独立三列存证 + 读回 20/20 match=yes；P1-A 的
    "两轮各三列" 声明被 measurement-audit §2-8 推翻（固定文件名被 E-redo
    覆盖，盘面仅存一轮）——教训吸收进协议（唯一文件名强制）；
  - 实验会话：`governor/{original,set-verify,restore-verify}-r1.txt`
    （05:12 / 05:12 / 05:53，先置位、末恢复；experiment-audit §5 逐 CPU
    核对 restore == original = schedutil；本会话核验三文件在位）。
- 工具限制（不影响裁决）：perf_event_paranoid=2（用户态 PMU）、cachestat
  用户态 EPERM、无 kernel-debuginfo（内核归因止于函数级）、rdmsr/stress 缺失。

## C. 全路径执行 DAG（80 边 / 22 候选要点）

来源：`dag.md`（合并 99 条原始边：TP 段 40 + UR 段 26 + CP 段 33；本会话
逐段 `grep -c '^### '` 复核边数为 40/26/25+D8=33，与合并记录一致）。

- 裁决分布：80 合并边中 **6 条 ELIMINATE_CANDIDATE + 4 条 UNPROVEN**（边级），
  其余 KEEP；去重候选 **22 条**（A 类 10 = 6 EC + 4 UNPROVEN；B 类 12 =
  3 EC + 9 UNPROVEN）。合并会话执行 20 组锚点回源码核实（dag.md §7 全部
  "属实"）。
- 四类候选位置（dag.md §3）：重复校验（M-02/M-16 双层 state-op 校验）、
  无读者 RMW（M-28 syscall_count_、M-42 submit_flushes_、M-40 live_cookies_）、
  O(n) 搬移/扫描（M-37 remove_exact(front)、M-53 cookie O(R) 逆向扫描、
  M-01/M-74 scope O(S)）、锁往返（M-61/M-62/M-65 融合族、M-67 sticky 双标志
  每 pass 3 次锁读）。
- 端到端每请求固定机工（DAG 断言，P1-A 实测落带内）：access 锁 1–2 +
  后端锁 2–3 + core 锁 7–9 + progress 锁 1–2，外加（uring）1 次锁内
  io_uring_enter 或（threadpool）1 次物理 syscall；shim 实测 uring
  16.22–19.50 锁/op（contended=0）、tp-w8 18.85 锁/op（contended 11.16%、
  futex 3.2/op 均值等待 42 µs）。
- 文档↔代码不符 7 项单列（dag.md §5）：statx 注释过期、注释政策偏差、
  声明-使用错位 ×3（syscall_count_ 写侧无门控、close_admission 双 backend
  版生产零调用、dispatch_occupancy/active_workers 仅测试调用）、
  progress_source fd 措辞张力、m1-results PROPOSE_UPSTREAM 与代码落差
  （H1/H2a 未产品化）。裁决归文档纪律流程与 owner，非性能 bug。

## D. 页缓存/FS 研究有效结果与证据边界（P0-C）

来源：`p0c-cache-study.md` + `measurement-audit.md`（审计后口径）。

有效结果（探针侧全部经审计复算通过；mincore/diskstats 判据均为直接测量）：

- 【定位】warm 页缓存命中地板 ≈ **102 instr/op、0.95 µs CPU/op、0 设备 I/O**
  （raw pread 探针，mincore 残留 100% + diskstats Δ=0 直接判据）。
- 【定位】cold buffered 全部格设备字节恰等于文件大小（512 MiB/rep，零字节
  浪费）；默认 RA(128K) 已近设备带宽上限；**SEQUENTIAL（RA×2）实测有害
  −38.3%**、RANDOM 在非随机访问下毁灭性（−70.5%，1.000 I/O/op）、
  WILLNEED +0.6% / NOREUSE ±0.06% 均在噪声带内无可归因收益——本栈上唯一
  "真正减机器工作"的缓存策略是默认 RA。
- 【定位】RWF_NOWAIT 适用条件：btrfs 支持 / tmpfs EOPNOTSUPP（负条件实测）；
  RA 覆盖顺序流上 EAGAIN 罕见（warm 0、cold ≤0.02%/1.4–1.6%）；NOWAIT 温路
  固定代价 +27 instr/op（129 vs 102，判定位）。
- 【定位】**#622 H2 在 7.3.3 精确复现**：tmpfs buffered read punt =
  1.0001/op（540028/540028）、btrfs warm ≈0（28 残差）→ FS 条件性成立；
  同格用户态机工与 FS 无关（instr 差 0.5%），内核态差 ~2.96 µs/op 全在
  punt 形态。threadpool 后端 FS 不敏感（1 pread64/op 打暖页缓存）。
- 【定位】O_DIRECT 深度悬崖：D1 35,176 → D2 5,884 ops/s（−83%），D4+ 平台
  ~3.6–3.8K，全程 1.0 I/O/op——本机 O_DIRECT 不是高深度合法对照形态。

**证据边界（P0-C §9 的 9 条 INCONCLUSIVE/NOT_CONSTRUCTIBLE/NOT_RUN，逐条
如实列出；audit §1.1 复核相符）**：

| # | 项 | 状态 | 原因 |
|---|---|---|---|
| 1 | O_DIRECT 深度悬崖机制 | INCONCLUSIVE | GS-480 FTL vs btrfs DIO 串行化未分辨 |
| 2 | cold buffered seq D16 反降机制 | INCONCLUSIVE | 探针形状/设备/FS 三者未分辨 |
| 3 | od-65536-seq-d1 数值 | INCONCLUSIVE | CV 35.66%，双离群（audit §2-13 更正：一慢一快），均值不可引 |
| 4 | readahead 竞争污染形态 | NOT_CONSTRUCTIBLE | 62 GB 空机无竞争工作集 |
| 5 | Sluice bench 随机格 | NOT_CONSTRUCTIBLE | bench 偏移严格顺序，case 列表冻结 |
| 6 | Sluice bench 64K×D1 格 | NOT_CONSTRUCTIBLE | 冻结 case 列表无此格（探针有对照） |
| 7 | Sluice bench btrfs cold 格（全部） | NOT_CONSTRUCTIBLE | write_fixture 每调用重写并驻留，无 hook |
| 8 | bench D2/D3 格 | NOT_CONSTRUCTIBLE | master case 列表无 d2/d3（协议 §6 剪枝依据） |
| 9 | nw-tmpfs 格 | NOT_RUN | tmpfs 拒绝 RWF_NOWAIT（EOPNOTSUPP），适用条件负例已登记 |

- **审计推翻项（引用禁令）**：P0-C bench 表 instr/op 与 cyc/op 绝对值被
  measurement-audit §2-1 推翻（`analyze.py:77` `env_reps=max(rep)+1` 分母
  420000 vs perf 窗口 540000，整体膨胀 ×9/7）——**不得作为绝对值引用**；
  列内相对比较、ops/s、CPU/op（rusage 逐 rep 分母）、wall、时延分位不受影响。
  P1-A §7-4 的独立发现经审计证实，P1-A 统一用 Δ/540000 口径。

## E. 机器工作成本台账（P1-A 主表口径）

来源：`p1a-cost-map.md`（5 regime × 11 段；**非相加纪律**：份额仅定位、
段与段不相加、u 与 u+k 是不同口径、profiling 构建份额不可折算吞吐；
全部经 measurement-audit §1.2 逐值重算一致）。

主格 per-op 总量（【定位】，540000-op 分母，audit 复算一致）：

| 列 | d4-btrfs | d16-btrfs | d16-tmpfs | d64-btrfs | tp-d16-w8-btrfs |
|---|---|---|---|---|---|
| ops/s 中位 | 600,914 | 584,580 | 410,477 | 537,942 | 345,805 |
| CPU/op µs（rusage） | 1.666 | 1.711 | 4.857 | 1.862 | 10.345 |
| instr u+k | 6498.6 | 6492.0 | 9698.3 | 7541.2 | 15757.4 |
| instr:u | 3858.5 | 3842.7 | 3862.2 | 4901.0 | 4033.9 |
| cycles u+k | 5955.5 | 6119.4 | 13870.5 | 6689.2 | 30653.5 |
| ctx-sw/op | 0.0001 | 0.0003 | 0.231 | 0.0002 | 1.295 |
| 锁/op（shim） | 19.50（0% 竞争） | 16.88（0%） | 16.95（0%） | 16.22（0%） | 18.85（11.16% 竞争） |

结构性计数（【回执】级，strace/bpftrace 三源一致）：enter/op=1.000052
（540028/540000，三 uring 格一致）、tp pread64/op=1.000011、tmpfs punt
1.0000/op、evfd_signals 1.000/op（uring）/0（tp）。

关键定位结论（【定位】，均不进裁决）：

- enter 内核骨架（syscall+io_submit_sqes 族）≈23.2% cycles（d16-btrfs ᴷ
  段级定位；枚举不全问题 audit §2-5 已登记，总数对）；`rep_movs` 19.2% =
  4KiB payload 拷贝本体（不可消除）；mutex 家族 6.6% = uring 形态最大用户态
  机工家族；tp-w8 内核锁机器 ≈7.1%（Top10 #7 枚举口径，audit §2-6）。
- **M-28**：`lock addq $0x1,0x198(%r15)`（`src/async/threadpool_backend.cpp:434`
  编译产物）占 worker_loop 本地 30.24% × 符号 1.83% ≈ 0.554% 全局 cycles
  ≈ 170 cyc/op @tp-w8（**采样定位值**：由 perf 份额换算而来，非确定性单指令
  模型——r2 明示）——候选 #1 的 measured hotspot 前置（p1a §4-1，
  `ann-full-tpw8.txt:371`）。
- **M-53**：cookie O(R) 逆向扫描 0.14/1.03/1.74% @d4/d16/d64（≈63/116
  cyc/op），O(D) 增长成立、精确斜率 INCONCLUSIVE（audit §3-5：换算分母
  未在报告写明）。
- **M-63**：uring owed sweep 逐槽锁在 W-02 形态 NOT_OBSERVABLE（无零长 op →
  owed_count_ 恒 0 → 循环休眠；判 NOT_OBSERVABLE 非"零成本证明"）。
- 交叉验证：#622 H1/H2 在 7.3.3 精确复现（p1a §7-1/§7-2）；P0-C §4 各格
  在 P1-A 复现（ops/s/CPU/op/p50 全部在 P0-C 噪声带内，p1a §7-3）；
  P0-C/P1-A 会话间 instr:u 差 0.03–0.5%。

## F. 消融候选清单（Top 候选与排序依据）

来源：`candidates.md`（r2，吸收 dossier-review F1–F8 与独立挑战者 F2/F3/F6
更正）+ 实验结果更新。

22 条候选终态（详见 §I 矩阵；r2 计数更正）：**2 NO_GO（#1 M-28 实验判词
1b+1c；#13 payload 快照）；1 GO 排队（#11 M-42）；14 INCONCLUSIVE（含 #12
M-40——由 NO_GO 改判：原"relaxed≈普通访存"对写侧 fetch RMW 不成立，每 read
op 2 条 lock RMW，但计数器有生产读者、宏门控不可用）；5 SEMANTIC_MISMATCH
（#7/#8/#9/#10 运行时机制面 + #22 词表卫生面）**。3 条非候选热点全部 NO_GO（H1 提交
批形态 → #595 系所有权；io-wq punt FS 条件性 → 非 Sluice 代码；tp-w8
work_mtx_ 竞争 → 配置/新权威域，唯一减法交集由 M-28 承载——现已 NO_GO）。

单因素槽位选择依据（candidates.md §3，dossier-review §1.6 复核"执行正确"）：

- **#1（M-28）获槽**：22 候选中 Top10 可测成员排名最高（Top10 #2，仅次于
  属 #595 系的 H1）；纯减法（无读者计数器移出生产 TU）；正确性证明可完成
  （12 处 tests.lua 重编译点含全部三消费套件——F3 修正后完整清单
  `:406,448,501,559,745,818,891,959,1015,1074,1139,1205`）；#635
  DrainObservation 构建先例；C 类（不动提交时序）。
- **M-53 让位**：三项迁移证明义务未证（ledger 单调/控制 CQE exact/cancel
  定向）+ 斜率 INCONCLUSIVE——正确性证明不可即时完成。
- **#11（M-42）排队**：同型宏门控但单机制规则禁止捆绑，排 #1 回执之后。
- **减法透镜总结论**（pocketjs-method-notes 十条透镜 + candidates §5，
  F8 闭合）：除 #1/#11 错位计数器与四个 owner 裁决面外，本轮测量未打开
  任何新减法空间；work_mtx_ 锁拆分被 `internal_work_retired()` 同锁联合
  快照（`threadpool_backend.cpp:856-861`）绑定，非遗漏空间。

## G. 证伪实验（session e5m28-s1-20261118）

### G.1 预注册与冻结（回执）

- 协议 r2 冻结链：dossier-review 初审 **approved=false**（F1–F8 八项必须
  修复缺陷）→ r2 复审仍 approved=false（残留 R1–R5 五项一行级修复）→
  修复落实后冻结执行。**披露：dossier-review 未出具第三轮 approved=true
  终审回执**——R1–R5 的修复由冻结文本落实（本报告 §I.3 逐项核实），
  实验按冻结文本执行，experiment-audit 事后逐条对照无绕过、无放松。
- SHA 登记（【回执】）：`candidate-registration.txt`
  `registered_before_any_timing=2026-11-18T05:02:12+08:00`，首个 perf
  invocation 05:16:35 → SHA 先于任何计时登记；BASE=`5d94f233`，
  CANDIDATE=`f350474d`（diff 恰 3 文件，单因素声明成立，uring TU 零改动；
  mutant 载体 = 休眠 test-only defines，常规构建行为零差异——F7 载体落实）。
- 二进制/环境（【回执】）：8 项 sha256 与存证一致；BASE ring bench
  `03dbef09…` 与 P1-A §0 参考逐字节一致（装置口径跨会话复现 <1%）；
  不 pin CPU（Cpus_allowed_list=0-19 逐 invocation 记录）；warm 门 =
  每 invocation 前后 diskstats Δreads/Δsectors=0，706 条 observation
  全带判据，bench_ok 706/706。

### G.2 A/A 与 NOT_POWERED（拆盲前预声明，【裁决】）

来源：`stats/aa-main-final.txt`、`stats/NOT-POWERED-predeclared-aa120.md`
（本会话逐字核对）。

- A/A 主格（W02-threadpool-4k-d16-w8，P2，540000 分母）预算封顶 120 对
  （24/48/72/96/120 逐块）：半宽轨迹 **229.5 → 156.7 → 129.0 → 107.7 →
  终态 100.5 cyc/op**（= 0.329pp of level 30517.1；115 有效对）> MME/2 = 77
  → **性能端点 NOT_POWERED**，于 05:41:53 落盘声明，先于首个 A/B invocation
  05:42（拆盲前）——experiment-audit §4 时间链核验。
- 逐块 σ_pair 589.0/571.6/539.8/535.8/556.0 无收敛；冻结功效先验的乐观支
  （σ≈337→61）不成立、悲观支（σ≈477→86）方向成立，实测 σ_pair=556 落高位
  （futex 主导形态先验兑现）。预算封顶后未再加对数。
- session 有效性（判词 2 不触发）：0.329pp ≤ 3.0pp 且逐块 mean 无漂移
  （回执快照 −35→−5→+30→−2→+17，sign 59/56；audit D1 注：71/94 两点为
  早期数据快照，终数据复算 −35.2→−5.5→+17.7→+7.6→+17.2，同样无系统趋势，
  无任何门依赖中间点）。

### G.3 配对 A/B 主格（【回执】数字 + 【裁决】前置声明）

来源：`stats/ab-main-final.txt`、`stats/main-cell-per-pair.csv`（24 行逐对
披露，AB/BA 交错、对内紧邻，P1+P2 各一；本会话核对前 24 行与 mean 值）。

- **主格 cycles/op（P2，u+k，540000 分母）：mean Δ(B−A) = +380.5 cyc/op，
  95% bootstrap CI [152.6, 598.7]**（恶化方向、CI 排 0）。**按判词 1 前置
  "不进入任何性能判读"，该列不作裁决性使用**——登记为候选 NO_GO 的旁证，
  不构成产品级效应声明（双 MME 管辖声明）。
- 支持列（同样仅旁证）：rusage CPU/op +0.103 µs [−0.044, +0.246]；
  ops/s −1.95% [−2.82, −1.09]；p99 +1.68% [0.65, 2.67]（Holm adj p=0.026）；
  Holm 全族 cycles/op 与 CPU/op adj_p=0.128、ops/s 0.068、p99 0.026；
  **p99(B)/p99(A) bootstrap 上界 1.0267 ≤ 1.05——SLO 保护界通过**。

### G.4 隔离格与负控制（【裁决】触发源之二 + 报告性格）

- **uring 隔离格（判词 1c，触发）**：A/A 先跑 6 对 P2 得 σ_uring=87.01；
  A/B 24 对 mean Δ = **−16.80 cyc/op，95% CI [−35.2585, −0.2681] 不含 0**
→ 第一析取支触发；|point|=16.8 < max(2σ=174.0, 均值 1%≈62.7) → 第二支
不触发；OR 语义 → 违例成立。改善方向 −0.27pp 与 uring TU 源零改动并存，
判"图像布局级噪声可能性"并登记。**证据强度（r2 补强）**：1c 系按本 session
实际 seed=20261118 的程序执行触发——协议文本未冻结种子与统计实现（登记为
协议缺口）；audit D4 换种子复算 8/10 排 0、2/10 含 0（CI 端点 −0.268 毫厘
级）。判词按冻结 OR 语义执行成立，但该门单独不构成强证据——中止通道由
1b + 1c + 判词 3 回归方向共同支撑（§G.6）。后续 session 须以 dated 修订
固定种子并预定义隔离误差（§G.6 先决条件）。
- **64KiB 格（判词 1d，未触发）**：CPU/op rel% +0.612% [0.325, 0.904]，
  CI 下界 < +2% → 不触发 halt-and-investigate；64k cyc/op +133.7
  [−142.4, 414.1] 如实披露；方向与主格回归一致（sign 19/5）。
- 负控制家族按协议 §6 剪枝留二（uring 隔离 + 64KiB），剪枝理由逐条经
  dossier-review §1.5 核实（D2/D3 NOT_CONSTRUCTIBLE 实核、W-03/uring 族
  零足迹、short/long 与计数长度无关）。

### G.5 正确性四门与计数层（【回执】，全绿）

- 门 1 语义 oracle：16 条计数断言逐行存证
  （`static/oracle-16-assertions.txt`：public :313/:414、scope :885/:990/
  :1133/:1189、integration :265/:289/:380/:422/:463/:487/:493/:522/:569/:943），
  三套件 **25/25 + 17/17 + 13/13 全过**（本会话核对存证文件行号与套件计数）。
- 门 2 mutant（【回执】）：两 mutant 均 exit=1 被杀——STICKY_ZERO 杀于
  integration :380/:422/:463/:943 的 ==N 断言（3/17 案例），SKEW 杀于
  scope 精确断言 + fail-fast（4/25 案例）；mutant 二进制 sha256
  `c61ee011…`/`5d30d873…` 在档；清洁构建恢复后套件复绿（exit 0）。
- 门 3 conformance 矩阵：**release+y 81/81、release+n 60/60、debug+y 81/81、
  debug+n 60/60，全部 exit 0**（本会话核对四个 `logs/matrix-*.txt` 尾行，
  与 #634/#635 口径一致；16 条计数断言行清单并存证）。
- 门 4 TSAN/ASAN：六目标 ×2 模式全绿（drain oracle 套件 silent-on-success
  产出 0 字节 .out，audit 直跑重验 exit=0 相容——回执格式瑕疵 D6 已披露）。
- 计数层静态三列（【回执】）：列 1 nm 退化（-O3 无 symtab，两臂同为空，
  如实登记不作判别）；列 2 objdump 操作性判别——**BASE 全 TU 恰 1 条
  `lock addq $0x1,0x198(%r15)`（= P1-A ann-full-tpw8.txt:371 的 M-28 写指令），
  CANDIDATE 全 TU 0 条 lock 前缀指令**；列 3 debug 测试构建符号
  `syscall_count_for_test` 与 `lock xadd` 均在。**静态三列无违例；
  违例在 §4-2 动态回执带**。

### G.6 判词链（【裁决】最终）

来源：`stats/VERDICT.md`（本会话全文核对）+ experiment-audit §6 终审。

```
判词 1b（触发）：主格 P1 instr/op Δ = −2.0955 [−2.5853, −1.5971]，sign 1/23，
  p=2.98e-06 —— 点估计越冻结接受带 [−2, 0] 下界 0.096 → §4-4 违例映射 → 中止通道。
  披露：95% CI 覆盖 −2.0（协议对回执带未写 CI 语言，按字面以测量值判带外；
  若按 CI 相容读法则 1b 不成立——但两读法下候选均 NO_GO，见下）。按运行顺序
  拆算（r2 补强，main-cell-per-pair.csv 复算）：AB 12 对均值 −1.944（带内）、
  BA 12 对均值 −2.247（带外）——分列落在接受带下界两侧；该门证据强度 =
  "点估计越界 + 方向一致（sign 1/23）"，非区间稳定越界。
判词 1c（触发）：uring 隔离 24 对 CI 排 0 → 冻结 OR 语义违例（毫厘级，
  改善方向 + uring TU 零改动 → 布局噪声可能性登记；种子敏感 8/10 排 0，
  见 §G.4 证据强度）。
→ 中止通道 → 候选 #1 NO_GO（NO_GO_ABORT）。
判词 1a：未触发（四门全绿）。判词 1d：未触发。判词 2：不触发（session 有效）。
判词 3：方向登记不裁决——主格回归方向（+380.5 [152.6, 598.7] 排 0）；
  即使按 CI 兼容读法使 1b 不成立，判词 3 亦为 NO_GO——终裁在所有可得读法下一致。
判词 4：不可达（NOT_POWERED 且回归方向）。判词 5/6：被判词 1 前置取代。
```

- **下一轮协议修订先决条件（r2，Owner 复核 2026-11-18）**：① uring 隔离格
  须预定义实际可接受的隔离误差（本轮判据事后看宽于机器噪声结构）；② 冻结
  统计实现——bootstrap 种子、重采样次数与统计脚本一并入冻结名单；③ 功效按
  裁决实际使用的 A/B 样本量规划——本轮 115 个有效 A/A 对的半宽（100.5）刻画
  的是 A/A 精度，不可移植给 24 对 A/B 的区间证据；1b/1c 的区间宽度应按 24 对
  规模预先估算并写入 MME 论证。

- **机制语义依据**（【回执】交叉回执）：u+k（P2）instr/op Δ = **+144.2
  [102.4, 184.9]**（A/A 噪声带 [−9.0, +32.9]）、ctx-sw/op Δ = **+0.0159**
  [0.0103, 0.0214]（A/A 带 ±0.0028）——"lock addq 是唯一指令差"前提在
  二进制/系统层不成立。**根因待确认（r2 修正表述）**：候选同时移除了类成员
  （对象布局变化）与 RMW 本体——该 RMW 位于 `work_mtx_` 获取之前，其移除
  可直接改变 worker 竞争时序；加上 TU 重编译的代码布局位移，三种效应在本
  实验对照下不可分离。观测到的内核态指令与上下文开关变化是混合效应的观测
  面，不构成对任一单因的归因。静态减法已证实，整机差异超出单指令模型——
  这正是 §4-2 回执带要捕的失效形态，主格回归方向与之相容。
- 实验审计结论：**回执整体可信、NO_GO_ABORT 映射成立、0 项阻断**；全部
  已采纳数字由 `/tmp/audit-e5m28/recompute.py` 独立复算逐位一致；中止判词
  对执行员自己的候选不利（其已通过全部正确性门并建成双臂）——无虚报、
  无绕门、无门放松的反向证据链完整。

### G.7 失败/降级全记录（三类审计合并）

**measurement-audit 14 项（P0-C/P1-A 复核；5 推翻 + 9 降级——r2 计数更正）**：

1. 【推翻】P0-C bench instr/op 与 cyc/op 列 ×9/7 膨胀（env_reps 分母错误），
   绝对值禁引；
2. 【降级】P1-A ops/s CV 列虚高 ~2× 且标签不实（含 2 个 warmup 轮）；
3. 【降级】P1-A dso 行混源未标注（两格大样本、三格小样本）；
4. 【推翻】P1-A 段 1 括号分解数字错（headline 对、分解错）；
5. 【降级】ᴷ 脚注枚举不全（总数 23.16% 对，缺 2 符号）；
6. 【降级】tp 内核锁机器 / tmpfs punt 机器两处份额口径漂移（定位参考，
   不进裁决）；
7. 【推翻】Top10 #9 submit_read 数字标签错（raw 0.93/1.32/1.68 为 d16/d64
   两深度）；
8. 【推翻】P1-A "governor 两轮各三列"存证不成立（被覆盖）；
9. 【降级】P0-C "fincore 16384/16384" 无档案（bench warm 改由 diskstats 承担）；
10. 【降级】P0-C "shmem DONTNEED 无效（实测）"未执行未记录（恒驻留成立）；
11. 【推翻】P0-C "5,338 ops/s ≈ 5.3K IOPS" 量纲混同（实为 ~3.9K IOPS）；
12. 【降级】turbostat 摘要色彩数字（无结论依赖）；
13. 【降级】od-65536-seq-d1 离群描述不完整（双峰）+ od-d8 CV 12.56% 未披露；
14. 【降级】punt 率舍入不一致（1.0001 vs 1.0000，零实质影响）。
   另登记 9 项复核后仍缺证据（gaps），含 campaign 轮 governor 三列不可补。

**dossier-review（未过终审的披露）**：初审 F1–F8（case id 冻结值零匹配、
M-40 事实前提错误、tests.lua 重编译点 10→12、判词不完备、双 MME 与功效
先验缺失、降级值入裁决列、mutant 载体矛盾、F8 透镜遗漏）+ r2 残留 R1–R5；
八项与五项全部实质修复后冻结执行，但复审止于"修复后即可进入执行"，
**无第三轮 approved=true 回执**（见 §I.3）；S1–S5 保留顾虑移交 owner。

**experiment-audit 6 项非阻断披露（D1–D6）**：D1 A/A 轨迹 71/94 两点混拼
（终点逐位复现，无门依赖）；D2 incident 条目 2 措辞失准（pair 66 实为
第二次尝试 NOTWARM 中止、完成且 warm 的是第一次；影响限 A/A P1 的 1/116 对，
该列不承载门）；D3 支持格 W02-threadpool-4k-d16-w4 未运行且未披露
（无判词引用，冻结负载集条目静默省略）；D4 判词 1c 种子边缘性（8/10 排 0）；
D5 JSONL `filesystem:"tmpfs"` 为硬编码标签（实际 scratch = btrfs +C，存证
一致）；D6 drain oracle 空回执文件（silent-on-success，重跑 exit=0 佐证）。

**会话级事故与排除（如实，未灌任何门）**：A/A 期 NOTWARM 6 条
（pairs 65、66 首试、94、103、108、116；/home btrfs discard=async 后台
回显/flush 阵发致 10–12 s stall 与 Δreads=1–2）——全部按冻结 warm 判据
剔除，5 对不完整即剔除，pair 66 结构性重跑一次（完成次进入分析、陈旧
NOTWARM 观测滤除；记录 `static/incident-aa-pair65-notwarm.txt`）；两次
smoke invocation（harness 调试）按 tag 前缀排除于全部分析，更早一次全套件
smoke 未产生观测。

## H. 最小产品变更

**明确不改。** 依据：

- 判词 1 中止通道（1b 回执违例 + 1c 隔离违例）→ 候选 #1 NO_GO，按
  candidates.md §1 #1 翻案条件"回执失败 → NO_GO"映射；正确性门虽全绿，
  但中止通道不因正确性而豁免（协议 §5-1 原文"任一触发即实验中止、候选
  NO_GO"）。
- **产品 PR 未制作**（判词/审计/确定性复核未获准）；master 保持 `5d94f233`，
  `syscall_count_` 写侧（`src/async/threadpool_backend.cpp:434`）与成员
  （`include/sluice/async/detail/threadpool_backend.hpp:258`）维持现状
  （生产库含 1 条 `lock addq`）。
- 候选 diff 与实验证据仅存于 `research/e5-c1-m28-gate` 分支
  （`f350474d` 机制提交 + `ae577394` 证据提交），未合并、不申请合并；r2 起
  另有固定 SHA 归档入口分支 `evidence/e5-fullpath-machine-work-20261118`
  （= `ae577394` + 归档提交：协议 r2 副本及哈希、候选 diff、驱动/审计统计
  脚本、全量校验清单，入口 `results/e5m28-s1-20261118/archive-entry.md`），
  同样未合并、不申请合并（Owner 复核 2026-11-18 指令补齐）。
- 无 ledger 产品行、无新候选槽位、无 CHANGELOG 条目（双 MME 管辖声明 +
  NO_GO 判词共同约束）；主仓被跟踪文件全程零改动。

## I. Owner 决策矩阵定稿

### I.1 22 候选 + 3 非候选终态（【裁决】）

| # | 合并边 | 候选内容 | 终态 | 关键依据 / 翻案条件 |
|---|---|---|---|---|
| 1 | M-28 | syscall_count_ 生产侧宏门控剔除 | **NO_GO（实验判词 1b+1c）** | 静态三列符合但动态回执越带 + 隔离违例；翻案：新证据证明可把机制落地约束为"单指令差"（如隔离 TU 重编译效应的根因消除）并重过全协议——新 session、新冻结 |
| 2 | M-80 | write_all_at → prevalidated_pwrite_step | INCONCLUSIVE | 直呼形态零测量；翻案：直呼组合写 measured hotspot + "compose 仅由已校验入口进入"不变量证明 → 单因素 A/B |
| 3 | M-16 | uring 后端 state-op 校验收敛 | INCONCLUSIVE | PARTIAL 份额 + owner 前证（结构保证/seam 冲突）未解 |
| 4 | M-37 | remove_exact(front) → pop_front | INCONCLUSIVE | 前提 NOT_CONSTRUCTIBLE（队列长 ≈1）；深队列形态属 #595 批量概念 B |
| 5 | M-53 | cookie O(R) 扫描 → user_data 编码（H1） | INCONCLUSIVE | 热点 MEASURED 但三项迁移义务未证 + 斜率 INCONCLUSIVE；翻案：义务证明 + d64 A/A 分辨力达标 |
| 6 | M-63 | uring owed sweep 单锁快照（H2a） | INCONCLUSIVE | W-02 形态 NOT_OBSERVABLE；翻案：零长 op/idle 形态协议 + 交付顺序不变 |
| 7 | M-09 | Slot::borrow 只写不读 | SEMANTIC_MISMATCH | root 义务存储 vs 死存储，owner 裁决（`docs/explicit-io-contract.md:586`） |
| 8 | M-30 臂 | zero_effect_cancel 生产零生产者 | SEMANTIC_MISMATCH | CANCEL-01 已由入口强制；seam 义务归属 owner |
| 9 | M-57 | acquire_execution 生产零调用 | SEMANTIC_MISMATCH | BACKEND-01 seam 面；热路径成本为零，删除只是代码面收缩 |
| 10 | M-12 | close_admission 双实现二选一 | SEMANTIC_MISMATCH | 两 backend 版生产零调用；存活实现 + oracle 迁移 owner 指定 |
| 11 | M-42 | submit_flushes_ 只写不读原子计数 | **GO（排队）** | MEASURED-small（0.01% 符号份额）；排 #1 回执后独立推进。**#1 回执已落地（NO_GO）——是否启动属 owner；设计教训（r2 修正表述）：#1 的失效形态是整机差异超出单指令模型（TU 布局/对象布局/RMW 竞争时序三因不可分离，根因待确认），#11 的机制在 uring TU，其协议必须自带 §4-2 型动态回执带 + 隔离格判据（并按 §G.6 先决条件预定义隔离误差、冻结统计实现），不能只做静态三列** |
| 12 | M-40 | live_cookies_/live_control_sqes_ 原子冗余 | INCONCLUSIVE（r2 改判） | 2–3 条 lock RMW/op 真实存在但有生产读者；翻案：行级归因 + 读者角色机制门作答 + dispatch_mtx_ 覆盖重证 |
| 13 | M-61 | PublicationPayload 快照仅测试可达 | NO_GO | 生产恒 nullptr、无 hotspot；非性能候选 |
| 14 | M-01 | scope O(S) 扫描（H3 开放） | INCONCLUSIVE | PARTIAL + 驱动污染未剔除；free-list 已 REJECT（S2：cookie_free_list_ 先例是否重开 REJECT 属 owner） |
| 15 | M-10 | 零长发布三锁融合 | INCONCLUSIVE | 族级 + W-02 形态休眠；零长占比未测 |
| 16 | M-61 | begin/complete 两锁融合 | INCONCLUSIVE | PARTIAL；步间记录读与测试暂停点语义须保（**S3 已闭合，2026-11-18**：r6 §B.3/B.5 的 R3（0.45% < 1% MME REJECTED）= begin_publication+complete_publication 融合即本候选，历史拒绝理由含 `publication_inflight` 反例与确定性测试注入点——挂回本行；翻案须新 measured profile 推翻该拒绝） |
| 17 | M-62 | 无观察者跳过 claim/retire | INCONCLUSIVE | PARTIAL（"快速返回≠零锁开销"已更正）；对位 mutex family ~9–11% 候选（r6 F.6） |
| 18 | M-65 | owed release_control 第三锁融合 | INCONCLUSIVE | 本轮零测量（ owed 门控 + 无零长 op） |
| 19 | M-67 | sticky 双标志原子化/融合 | INCONCLUSIVE | PARTIAL；对位 r6 R2（outstanding_/health_failed_ core 摘要原子化，0.90% < 1% MME REJECTED）——部分重叠非同机制；R3 不对位本候选（R3 = #16/M-61 发布锁融合，r2 修正原错引） |
| 20 | M-33 | TP cancel ring 摘除惰性失效 | INCONCLUSIVE | cancel 形态零测量；替换须新增机制，过机制正当性门 |
| 21 | M-74 | ready() 快路径合并 | INCONCLUSIVE | 收益方向因 ready 概率而异，无证据不裁决 |
| 22 | M-80 hygiene | file_semantics 模型词表位置 | SEMANTIC_MISMATCH | 词表面归属 owner 裁决 |
| 非1 | H1 提交批形态 | per-dispatch flush 批量化 | NO_GO（本轮） | 消除属 #595 系：KEEP_IMMEDIATE 在身；重开仅按 RQ1 + 整机重测 + 全 falsifier 家族 |
| 非2 | io-wq punt FS 条件性 | — | NO_GO | FS/形态选择，非 Sluice 代码；设备 FS 重测属既列重测情形 |
| 非3 | tp-w8 work_mtx_ 竞争 | — | NO_GO | 配置选择（w4 优于 w8）或新权威域；唯一减法交集 M-28 已 NO_GO |

### I.2 S1–S5 保留顾虑（移交 owner，不阻断）

- **S1**：NOT_POWERED 已被协议预声明并兑现；剩余仅 owner 对"是否值得跑满
  120 对"的成本权衡（已跑满，此顾虑闭合为记录）。
- **S2**：`cookie_free_list_`（`uring_transport.cpp:580`）先例 vs m1-results
  对 scope free-list 的 REJECT——是否重开该裁决属 owner，#14 翻案路径未提及。
- **S3（已闭合，2026-11-18 Owner 复核）**：r6 §B.3/B.5 原文直读可辨——
  **R3（0.45%）= begin_publication + complete_publication 融合 = #16 / M-61**
  （拒绝记录含 `publication_inflight` 反例与确定性测试注入点，历史拒绝理由
  已挂回 #16）；**R2（0.90%）= outstanding_/health_failed_ core 摘要原子化**，
  与 #19 / M-67 仅部分重叠。#19 原对 R3 的错引已移除（r2），无需 owner 进一步
  澄清。
- **S4**：close_admission"仅 :468,471"漏计同文件 helper 调用点
  （:481,:495,:496）；结论（生产零调用）不受影响。
- **S5**：判词 4 "性能 GO"标签歧义——本战役判词 4 不可达，顾虑自然解除。

### I.3 R1–R5 收口状态：已修复并冻结（本会话逐项核实）

| 项 | 收口证据（本会话核对的冻结文本位置） |
|---|---|
| R1（协议 §3.1 行号错配） | `experiment-protocol.md:51` 已改为 public_request_target 重编译点 `:745` ✓ |
| R2（判词 1 头与 1d 矛盾） | `experiment-protocol.md:130` 判词 1d 已写明处置路由（artifact → 判词 2 重建重跑；真实回归 → 中止 NO_GO）✓ |
| R3（§6 行 1 标签未同步） | `experiment-protocol.md:150` uring 隔离格已标"判据见 §5 判词 1c" ✓ |
| R4（判词 1c 样本量未冻结） | `experiment-protocol.md:124-126` 已冻结"24 对 AB/BA（P2 pass…冻结）"并澄清量纲"隔离格 cycles/op 均值的 1%" ✓ |
| R5（缝读者枚举缺一） | `candidates.md:140` F2 记录已补 `uring_transport.cpp:38-40`（live_control_sqes_for_test）与 sq_ready_for_test 先例 ✓ |

五项修复均经实验实际执行检验（判词 1c 按冻结 24 对 + seed 程序执行；
1d 未触发故未行使处置路由）。**披露：dossier-review 终审止于 r2
（approved=false、R1–R5 待修），修复后未再召集第三轮复审即冻结执行**——
experiment-audit 事后逐条对照冻结文本确认"无绕过、无放松"，作为事后
补偿性核验。

## J. 下一步研究计划（#604 / #595 / #621 / #617 各自走向；不建新 umbrella）

- **#595（提交策略族）**：KEEP_IMMEDIATE 维持——本轮仅在 7.3.3 复现 H1
  形状定位证据（enter/op=1.000052），未产生批量化收益或危害的新可归因
  证据；D2 FAIL 决定项幅度 4–7pp 非频率敏感、R3/R4/R5 负结果 ≥18pp 结构性。
  重开仅按 RQ1 batchability admission（"second held entry" guard）+ payload
  gate + 全 falsifier 家族（D1/D2/D3、64KiB、W-03、cancel）+ 整机重测
  （R1–R5 原始数据未归档，只能按各轮分支 harness 重测）。概念 B 本轮
  NO_GO/不实现。
- **#621（异步热路径产品线）**：两轮已产品化并合并（#634 owed-count 门、
  #635 drain 单读）；r6 遗留未决项继续有效（uring 注入 seam 缺、真实 EINTR
  未测、BASE debug/liburing=n oracle 未跑、io_uring 端到端/wall/多 worker/
  cold cache 未测——引用即重测）。r6 F.6 下一候选：poll(-1)→阻塞 read
  （profile rank 1，~5.0–5.5% cycles）与 mutex family（~9–11%）各需新
  measured profile + 单因素 A/B。threadpool 侧 M-28 经本战役 NO_GO 后维持
  现状；worker 竞争形态属配置面（w4 优于 w8）。
- **#604（workload-driven 优化 umbrella）**：维持 OPEN + 产品
  HOLD_NO_REAL_APPLICATION（`READY_FOR_595=false`，HOLD.md 核查）；portfolio
  自述 "hypothesis set, not evidence"——任何 workload 驱动机制先过 G0
  workload admission（真实 consumer/trace），不因本战役任何数字解锁。
- **#617（USIO-0 kernel-boundary 研究）**：PAUSED（621-round2 REPORT:91）；
  #622 成本地图是条件性触发而非自动触发（G1 未满足：无 admitted real
  workload 证明 material kernel-boundary cost）；595-r5 的条件性问题已移交
  #617（`595-r5-phase-a/analysis/env-context.md:90`）。本战役的 io-wq
  FS 条件性、enter 骨架、内核锁机器数字全部按 #622 消费边界（定位、
  不相加、口径随数字走）归档，供 #617/#595 未来 G1 触发时引用。
- **不建新 umbrella**（战役约束）：E5 战役以本报告收口；后续单因素工作
  （如 #11 M-42，若 owner 批准）附接既有 issue，各自独立预注册。

---

## 附录：证据索引

- 战役输入（12 文档）：`repo-reality / env-census / history-digest /
  dag（+3 分段）/ p0c-cache-study / p1a-cost-map / measurement-audit /
  pocketjs-method-notes / candidates / experiment-protocol / dossier-review /
  experiment-audit`，均在 `docs/evidence/e5-fullpath-machine-work/`。
- 实验结果根：`/home/jnhu/sluice-e5-elim/results/e5m28-s1-20261118/`
  （observations.jsonl 706 行、perf/、jsonl/、logs/、governor/、mutants/、
  static/、stats/、candidate-registration.txt）；git 分支
  `research/e5-c1-m28-gate`（`f350474d` + `ae577394`，2102 文件）；固定 SHA
  归档分支 `evidence/e5-fullpath-machine-work-20261118`（r2 补，见 §H）。
- 本报告撰写会话核验（只读）：12 输入文档全文；结果根 VERDICT/aa-main/
  ab-main/ab-uring/ab-64k/main-cell-per-pair/NOT-POWERED/mutant-receipts/
  counter-layer-three-columns/oracle-16-assertions/reproduction-entry/
  incident/candidate-registration/governor 三列/matrix 四尾行；主仓
  `git status --porcelain` 与实验 worktree `git log`；R1–R5 冻结文本五处
  grep 定位；三段 DAG 边数 grep（40/26/33）。**本会话未重跑任何测试/
  基准/统计复算**——实验数字引用 experiment-audit 的独立复算回执
  （其已逐位复算一致）。
