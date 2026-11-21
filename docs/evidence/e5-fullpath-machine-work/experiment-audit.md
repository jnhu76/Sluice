# 独立实验审计回执（e5m28-s1-20261118，候选 #1 M-28）

- 审计：实验审计员，2026-11-18。对象：实验执行员回执（verdict=NO_GO_ABORT，
  resultsRoot=/home/jnhu/sluice-e5-elim/results/e5m28-s1-20261118）。
- 依据：冻结协议 `experiment-protocol.md`（r2）§2/§3/§4/§5/§6/§7、`candidates.md` §1 #1。
- 方法：只读复核全部回执文件；所有廉价检查均在本审计会话重跑（nm/objdump/单套件/
  统计复算/sha256），复算脚本不入仓库（/tmp/audit-e5m28/）。
- 结论先行：**回执整体可信，NO_GO_ABORT 判词映射成立；未发现虚报、绕门或门放松。
  0 项阻断；6 项非阻断降级/披露项（见 §5）。**

## 1. 冻结项与候选完整性（§2）

- BASE worktree `/home/jnhu/sluice-e5-elim-base`：detached 于
  `5d94f233967c3aef993eb19ba9e3252c532b0951`，`git status --porcelain` 0 行（本会话重跑）。
- CANDIDATE `/home/jnhu/sluice-e5-elim`：HEAD=ae577394（evidence 提交，`--name-only` 全部
  位于 `results/`），其父 f350474d 即登记的 CANDIDATE_SHA；f350474d diff 恰为 3 文件
  （hpp 成员移入测试宏块、cpp 写侧 `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)` +
  内嵌 `!defined(..._STICKY_ZERO)`、seams 访问器 skew mutant）——单因素声明成立，
  uring TU 零改动属实。mutant 守卫全部位于测试宏内侧 → 常规构建休眠 ✓。
- 登记 `candidate-registration.txt` registered_before_any_timing=05:02:12+08:00，首次
  perf 启动 05:16:35（perf/main-p001-11-A.csv 头）→ SHA 先于任何计时登记 ✓。
- 二进制 sha256：本会话重算 8 项（两臂 × ring/noring × bench+lib）与
  `static/bin-sha256.txt` 全部一致；BASE ring bench `03dbef09…` 与 p1a §0 参考一致 ✓。
- 工具链/构建入口：`build-base-profiles.log`/`build-candidate-profiles.log` 记录双臂
  双 profile 构建 + profile 身份断言 3 条 ok（ring 依赖 liburing、noring 不依赖）✓。

## 2. 正确性四门（§3，判词 1a 未触发）

- 门 1 语义 oracle：16 条断言行号逐一在候选源码核实（public :313/:414、scope
  :885/:990/:1133/:1189、integration :265/:289/:380/:422/:463/:487/:493/:522/:569/:943，
  `sed -n` 逐行输出与 oracle-16-assertions.txt 一致）。logs/gate1-*.out：
  25/25 + 17/17 + 13/13 全过。本会话重跑 release `request_scope_test` → exit 0、
  25/25 passed。
- 门 2 mutant：`mutants/mutant-receipts.txt` exit=1 ×2；杀点映射核实——STICKY_ZERO 的
  3 个失败测试（:360/:407/:913 起）恰好含 ：380/:422/:463/:943 的 ==N 断言；SKEW 的
  4 个失败测试（:854/:950/:1088/:1144 起）恰含 ：885/:990/:1133/:1189 等 ==N 断言。
  mutant 二进制 sha256 回执在档（c61ee011…/5d30d873…）。恢复构建后
  post-restore-request_scope_test.out 25/25 ✓。
- 门 3 conformance 矩阵：logs/matrix-{release,debug}-{y,n}.txt 尾行 81/81、60/60、
  81/81、60/60，0 failed ✓（与 VERDICT.md 一致）。
- 门 4 TSAN/ASAN：6 目标 ×2 模式回执在档；其中
  progress_source_drain_semantic_oracle_test 两份 .out 为 0 字节（该套件
  silent-on-success，仅 FAIL 时输出——tests/progress_source_drain_semantic_oracle_test.cpp:36
  只有 fprintf(stderr,"FAIL…")）。本会话直跑两二进制：asan exit=0、tsan exit=0、
  输出 0 字节 → 与回执相容（回执格式瑕疵见 §5-D6）。tsan request_scope 24/24
  （asan/release 为 25——TSAN 下差 1 条件测试，非失败）。

## 3. 计数层（§4，判词 1b）

- 三列存证本会话全部重跑（`ar x` 两臂 release ring lib → threadpool_backend.cpp.o）：
  - 列1 nm：两臂 `syscall_count` 匹配均 = 0（退化列，执行员已如实登记）；
  - 列2 objdump：BASE 全 TU 恰 1 条 lock 前缀指令 `2e3a: lock addq $0x1,0x198(%r15)`
    （worker_loop 内计 1）；CANDIDATE 全 TU/worker_loop 均 0 条 —— 与
    counter-layer-three-columns.txt 完全一致；
  - 列3 测试构建（debug）：nm 存在 `W _ZNK…22syscall_count_for_testEv`，worker_loop
    `4199d1: lock xadd %rdx,(%rax)` —— 一致。
  → §4-1 静态三列无违例；违例确实不在静态列。
- §4-2 动态回执（本会话独立复算，非执行员脚本）：主格 P1 instr/op 24 对
  Δ(B−A) = **−2.09553**，CI [−2.5853, −1.59714]，sign 1/23，p=2.98e-06 —— 与回执逐位
  一致。点估计越 [−2, 0] 带下界 0.0955 → 判词 1b 按原文触发 ✓。
  交叉佐证（复算一致）：P2 u+k instr/op Δ = +144.2 [102.4, 184.9]（A/A 噪声带
  [−8.99, +32.85]）、ctx-sw/op Δ = +0.0159 [0.0103, 0.0214]（A/A 带 ±0.0028）——
  「lock addq 是唯一指令差」前提在二进制/系统层不成立（TU 重编译布局位移），正是
  该回执带要捕的失效形态。VERDICT.md 已如实披露 CI 兼容读法（CI 含 −2.0）不违例的
  另一解释；按 §4-4 原文「Δ 落在带外」取点估计读法是从严而非放松，且两读法下
  「CI 完全在带内」的第三读法同样违例（下界 −2.585 越带）。

## 4. 统计、隔离格与 64KiB（§5，判词 1c/1d/2/3/4）

独立复算（自研实现，/tmp/audit-e5m28/recompute.py，同法：逐对差 + sign 两项检验 +
bootstrap percentile 10000 + Holm 4 族）与全部已采纳数字**逐位一致**：

- A/A 主格（main-p，P2，540000 分母）：n=115（剔除对 65/94/103/108/116 ✓），
  mean 17.2、std 556、CI [−81.71, 119.3]、半宽 **100.5** = 0.329pp@30517.1
  **> MME/2 = 77** → NOT_POWERED 判定成立；sign 59/56、p=0.8522。A/A P1 instr/op
  n=116、CI [−0.066, 0.449] ✓。终态 1.98×556/√120 ≈ 100.8 与实测一致。
- A/A 试点轨迹：24 对（mean −35.23/std 589/hw 229.5/sign 12/12）、48 对（−5.53/571.6/
  156.7/25/23）与回执一致；71/94 两点为早期数据快照（见 §5-D1），但不承载任何门。
- A/B 主格（abm，24 对 AB/BA 交错、对内紧邻）：cycles/op +380.478 [152.575, 598.706]
  （恶化方向、CI 排 0）；CPU/op +0.103393 [−0.0443, +0.2456]；ops/s −1.953%
  [−2.821, −1.085]；p99 +1.680% [0.649, 2.673]；Holm 0.1278/0.1278/0.0680/0.0264；
  p99(B)/p99(A) bootstrap 上界 **1.0267 ≤ 1.05**（SLO 保护界通过）✓ 全部逐位一致。
- uring 隔离格（判词 1c）：A/A 先跑 6 对 P2（au，05:50，先于 abu）→ σ_uring =
  87.0124 ✓；A/B 24 对 P2：mean −16.7985、CI [−35.2585, −0.2681]（seed=20261118
  逐位一致）→ **CI 不含 0，第一析取支触发**；|point|=16.8 < max(2σ=174.02,
  均值 1%≈62.7) → 第二支不触发；OR 语义 → 违例成立 ✓。判据与冻结文本一致，
  无放松。改善方向（−0.27pp）与 uring TU 零改动并存 → 按「机制外溢」登记合理。
- 64KiB 格（判词 1d）：CPU/op rel% +0.6124% [0.3246, 0.9035]，CI 下界 +0.325%
  **< +2%** → 不触发 halt-and-investigate ✓；sign 19/5、raw p=0.0066 ✓；
  64k cyc/op +133.7 [−142.4, 414.1] 如实披露 ✓。
- 判词 2（session 有效性）：0.329pp ≤ 3.0pp ✓；逐块 mean 无漂移——终数据复算
  （−35.2→−5.5→+17.7→+7.6→+17.2）与回执快照值（−35→−5→+30→−2→+17）均无系统性
  趋势 → 不触发 ✓。判词 3 方向仅作回执级登记、不作裁决使用 ✓；判词 4 不可达
  （NOT_POWERED 且回归方向）✓；5/6 被判词 1 前置取代 ✓。
- NOT_POWERED 预声明：stats/NOT-POWERED-predeclared-aa120.md 05:41:53 落盘，首个
  A/B invocation 05:42（perf/abm-p001-11-A.csv）→ 拆盲前声明 ✓；A/A 先于 A/B
  同 session ✓；预算 24→48→72→96→120 封顶 ✓。NOT_POWERED 未被用作跳过 A/B——
  机制门通道（A/B 24 对 + uring + 64k）照常执行 ✓，非借口。

## 5. 执行纪律核对（§2/§7）与非阻断发现

- warm 判据：harness 每 invocation 前后读 `/proc/diskstats` sda reads/sectors
  （字段 f[3]/f[5] ✓）；706 条 observation 全部带 dreads/dsectors；恰 6 条 NOTWARM
  （全在 A/A，tags 与 incident 文件一致），bench_ok 706/706。NOTWARM 对按
  warm_ok 过滤剔除，无无效 observation 进表 ✓。
- placement/governor：全部 observation cpus_allowed_list=0-19（不 pin ✓）、
  governor_cpu0=performance（计时全程 ✓）；三列唯一文件名
  original/set-verify/restore-verify-r1.txt（05:12/05:12/05:53——先置位、末恢复），
  restore 与 original 逐 CPU 一致（schedutil）✓。
- perf 口径：perf CSV 事件名证实 P1=instructions:u,cycles:u（无 sudo）、
  P2=cycles,instructions,task-clock,context-switches（sudo）✓；6 个抽样 CSV 与
  observations.jsonl 逐值一致 ✓；n_measured_reps 全部=7（MEASURED=7 生效）✓；
  ops 分母 540000（20000×9×3）/54000（2000×9×3）与 bench jsonl request_count 一致 ✓；
  fixture kFixtureBytes=64MiB（bench_w02_pipeline.cpp:50）✓；'=' 精确 case 过滤
  （bench_common.hpp:492-500）✓ F1 落实；主格/隔离格/64k case_id 均为 make_meta
  真实全名 ✓。
- 逐对披露：stats/main-cell-per-pair.csv、uring-cell-per-pair.csv、
  64k-cell-per-pair.csv 抽样与 driver log/observations 一致（abm-p001 d=+677.996、
  abu-p001 d=−17.846、abk-p001 d_cpu=+0.2295 等）✓。

非阻断发现（降级/披露，不影响任何判词）：

- **D1（轨迹快照混拼）**：A/A 轨迹 71/94 两点不源自终数据——71 点行
  （mean +30.15/std 539.8/sign 40/31）自 pair66-72 复跑块**之前**的快照逐位复现
  （observations 截至第 289 行），其 hw 129.0 在该快照+冻结种子下不复现（复得
  125.2；终数据 71 点 hw=128.6）。终点（n=115）逐位复现。无任何门依赖中间点；
  建议在 NOT-POWERED 文件标注中间行为当时快照。
- **D2（incident 措辞失准）**：incident-aa-pair65-notwarm.txt 条目 2 称 pair 66
  「second attempt warm; analysis uses the completing attempt」，而 driver log 与
  observations 显示第二次尝试在 main-p066-12-A NOTWARM（dreads=1）中止、完成且
  warm 的是第一次尝试。分析器按 (pos,pass) 取最后有效值：pair 66 的 P1 侧 1 用了
  第二次尝试的 obs（289 行），P2 两侧均一致取第一次尝试。影响限于 A/A P1 的
  1/116 对（该列不承载门）；主指标 P2 配对时间一致。措辞应更正。
- **D3（支持格未运行未披露）**：协议 §2 负载集列有支持格 W02-threadpool-4k-d16-w4
  （非裁决），本 session 未运行且回执/VERDICT 未披露该省略。无判词引用该格，
  不影响结论；属冻结负载集条目静默省略，应补披露。
- **D4（1c 种子边缘性）**：1c 的 CI 上界 −0.268 距 0 毫厘；协议未冻结种子。本审计
  独立 10 种子扫描：8/10 排 0、2/10 含 0（上界 +0.019/+0.158；执行员自报
  +0.02/+0.22，为其自选种子集，定性结论一致）。判词不单靠 1c（1b 独立成立），
  且执行员如实披露敏感性并按冻结程序执行——但后续 session 应以 dated 修订固定
  种子约定，避免"选种子"空间。
- **D5（JSONL filesystem 标签）**：bench JSONL `filesystem:"tmpfs"` 为
  make_meta 硬编码标签（bench_w02_pipeline.cpp:473），非 scratch 实际 FS；实际
  scratch /home/jnhu/e5-elim-scratch-e5m28-s1 为 btrfs+chattr +C（lsattr/mount 存证
  一致，compress=zstd:1 如披露）。防未来误读，登记即可。
- **D6（空回执文件）**：drain oracle 的 asan/tsan .out 为 0 字节、无 exit 码记录；
  套件 silent-on-success，本会话重跑 exit=0 佐证。回执格式应记 exit 码。

## 6. 判词映射终审

- 触发链：判词 1b（1b 点估计越带，从严读法、另一读法已披露）+ 判词 1c（冻结 OR
  判据第一支）→ 中止通道 → 候选 NO_GO；1a 全绿、1d 未触发、判词 2 不触发、
  3 仅登记、4 不可达、5/6 前置取代 —— 与协议 §5 原文逐条相符，无绕过、无放松。
- receiptsValid=false 为如实申报（1b 成立即回执无效）；oracleGreen=true 经 §2 复核。
- candidates.md §1 #1 翻案条件核对：正确性门全绿（该翻案支未触发）；协议 §5-1
  中止通道明文「任一触发即实验中止、候选 NO_GO」，回执失败经此映射为 NO_GO，
  且判词 3 方向（+380.5 [152.6, 598.7] 排 0）在非前置世界亦为 NO_GO —— 终裁
  NO_GO 在所有可得读法下一致。执行员回执用语「正确性门/回执失败 → NO_GO」中
  「回执失败」一词是对协议 §5-1b→candidates 映射的缩写，非 candidates.md 原文逐字，
  实质一致。
- 最重违例专项检查（虚报/绕门）：未发现。反向证据：中止判词对执行员自己的候选
  不利（其已通过全部正确性门并建成双臂）；全部已采纳数字可由原始
  observations.jsonl 独立复算逐位重现；原始层（perf CSV/driver log/jsonl）与
  派生层（stats/VERDICT）无矛盾。

## 7. 审计检查清单（本会话实际执行）

| 检查 | 命令/方法 | 结果 |
|---|---|---|
| worktree 状态与 diff | git log/show/status（两 worktree） | ✓ 单因素 3 文件 |
| 8 项二进制 sha256 | sha256sum | ✓ 全部一致 |
| 三列 nm/objdump | ar x + nm/objdump -d（两臂 .o、debug 测试二进制） | ✓ 与存证一致 |
| 16 断言行号 | sed -n 逐行 | ✓ |
| 门 1/2/3/4 回执 | 读 gate1-*/matrix-*/mutants/*.out | ✓（drain 2 文件空，重跑佐证） |
| 单套件重跑 | release request_scope_test、asan/tsan drain oracle 直跑 | ✓ exit 0 |
| 统计复算 | /tmp/audit-e5m28/recompute.py（自研实现） | ✓ 逐位一致 |
| 种子敏感性 | 10 种子 bootstrap | ✓ 8/10 排 0（D4） |
| perf CSV 抽样 | 6 tag 逐值比对 | ✓ |
| warm/placement/governor | observations 全量扫描 | ✓ |
| 轨迹快照溯源 | pre-resume 截断复算 | D1 |
| bench 参数支持 | grep bench 源（env vars/kFixtureBytes/'=' filter/ failing repetitions） | ✓ |
