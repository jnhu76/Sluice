# P2-REVIEW — 独立复核 P2 成本归因与上界

| 字段 | 值 |
|---|---|
| 角色 | 独立复核员（只读原始证据，只写本文件） |
| 日期 | 2026-11-18 |
| 复核对象 | `p2/P2-COST-MAP.md`（584 行）、`p2/PROFILE-SUMMARY.md`（551 行）、`p2/profile/` 原始数据、`p1/AA-CALIBRATION.md`、`p1/AA-CALIBRATION-2.md` |
| 方法 | 不重跑 perf/bench/test；用 `python3 -I` 加载 `p2/profile/analysis/analyze_p2.py`（sha256 `b822ea3a…`，与 `analysis/SHA256SUMS` 一致）的 `make_resolver()` / `parse_script()` / `attribute()`，从 5 个形态的 `script-cycles-sym.txt` 逐样本独立重算；另用内联脚本做针对性重叠/截断统计；`objdump` 核对 `submit_flushes_` 的指令；`grep`/`sha256sum` 核对 stat 行、Lost 回执与源码引用 |
| 结论摘要 | **形态级方向性归因成立**（内核/worker 机器工作是主体；P-B/P-E 等待路径确有热点；M-53 d16 很小；M-42 效应很小）。**候选级归因与上界不成立**：W1 的 poll 上界实为下界；P-B 用户态 90.7% 样本缺 bench 帧；P-B/P-E 的 matching A/A 格被误称不存在；M-62/M-67 有双计/阶段冒充候选上界；M-42 的 locked RMW 成本被低估且 P-D/P-E 格不适用。**不可用于 W1–W6 的准入裁决或 A/B 预算。** |

**严重度定义**：`high` = 该归因/上界不能用于准入裁决；`medium` = 数值或方法错误，必须修正后才可用于预算/归因结论；`low` = 表述/精度问题，不改变方向。

---

## 0. 复核范围与已执行的检查

本复核逐项执行了任务要求的检查。**没有重跑 perf/bench/test**（与 `P2-COST-MAP.md:11` 的“没有重跑”一致）；所有数字来自本会话对 raw `script-cycles-sym.txt` / `stat-*.txt` / `analysis.json` 的独立重算。已执行：

1. `stat-*.txt` 全量审计：9 个文件、**198/198 行 `percent_enabled=100.00`**，无 `<not counted>` / `<not supported>` / `scaled`（与 map §2.1 的声明一致）。
2. `lost-record-*` 全量审计：**15/15 文件 `Total Lost Samples: 0`**（与 map §7.1 一致）。
3. `analysis.json` / `analysis.txt` / `analyze_p2.py` 的 sha256 与 `SHA256SUMS` 逐项 MATCH。
4. 5 个形态的 `stage_self`、leaf、inclusive、user/kernel、off-CPU primitive 逐样本重算；**map §2.3 的 stage_self 表与 `analysis.json` 完全一致**（含 P-C/P-D/P-E 的 trampoline 行；行和 = 100%）。
5. 针对重叠、截断、A/A 格、M-42 指令、worker 归属的专项复算（见各问题）。
6. 源码核对：`src/async/uring_transport.cpp:222` 的 `fetch_add`、`:551-557` 的 O(R) 反向扫描、`src/async/uring_backend.cpp:171` 的 `io_uring_queue_init(..., 0)`、`include/sluice/async/detail/uring_backend.hpp:324` 的成员、`src/async/uring_test_seams.hpp:75-76` 的唯一读者。

**未执行**：`python3 -I analyze_p2.py` 的完整重跑（会重写 `/tmp/p2-621/analysis.json`，且需要 kallsyms/twin 会话文件；本复核改用同一脚本的解析函数逐样本重算）、任何 perf/bench/test、语义 oracle、A/B。

---

## 1. HIGH 问题（不能用于准入裁决）

### H1 — W1 的 poll 上界被写成下界；`__poll` 边界样本被排除在 progress/wait 之外

**where**：`P2-COST-MAP.md:360-361`（§5.1 P-B/P-E 行）、`:437-438`（§5.7 B1/B2）、`:478`（§7 W1 准入）。

**issue**：map 用 `__x64_sys_poll` inclusive 15.77%（P-B）/ 11.46%（P-E）作为“仅 poll syscall 栈 ≤15.8%/≤11.5%”的上界。但同一行的 `__poll` inclusive 是 **24.93% / 15.83%**；两者之差是 **只有 libc `__poll` 帧、没有内核 poll 帧** 的样本。`analyze_p2.py` 的 `syscall_owner()` 只匹配 `do_poll|__x64_sys_poll|poll_schedule|do_sys_poll`（`analyze_p2.py:264`），不匹配裸 `__poll`；这些样本因此落入 `syscall (unattributed)`（或 `submit`），不计入 progress/wait。**15.8%/11.5% 是 poll 相关样本的下界，不是上界。**

**evidence**（本会话逐样本复算，输入 `p2/profile/P-*/raw/script-cycles-sym.txt`，复用 `analyze_p2.py` 的 resolver/parser/attribute）：

- P-B（24,585 filtered samples）：
  - exact `__poll` = **6,129（24.93%）**；exact `__x64_sys_poll` = **3,878（15.77%）**。
  - 含 libc `__poll`、无任何内核 poll 帧的样本 = **2,176（8.85%）**；加上 75 个含 eventfd 的同类样本共 **2,251**。这 2,251 个的 stage 分解：`syscall (unattributed)` 978、`submit` 518、`io-wq punt` 247、`other` 156、`scheduler/IRQ` 146、`progress/wait` 108、`shared sync` 88、`reap/CQE` 8。
  - 典型 4 帧栈（873 个）：`entry_SYSCALL_64_after_hwframe -> __internal_syscall_cancel -> __syscall_cancel -> __poll`；没有 `do_sys_poll`/`__x64_sys_poll`，因此被归为 `syscall (unattributed)`。
- P-E（34,736 samples）：
  - exact `__poll` = **5,500（15.83%）**；exact `__x64_sys_poll` = **3,980（11.46%）**。
  - 含 libc `__poll`、无内核 poll 帧 = **1,520（4.38%）**；其中 **1,251** 被归为 `syscall (unattributed)`，263 为 `progress/wait`。
- 因此 map 的 `syscall unattributed`（P-B 7.99% / P-E 13.02%）并不是“完全不可分”：**978 / 1,251 个样本可由 libc `__poll` 帧直接识别为 poll 边界**。map §8.1.4（`:494`）的“不能强分给 … poll”在这两个形态上过于保守。
- map 自己在 §5.1 行内已经列出 `__poll` 24.93%/15.83%，却选了更小的 `__x64_sys_poll` 当“上界”，并把较大的 `__poll` 仅作为“poll 相关 inclusive”陈列；这违反“inclusive 份额不能相加/不能替代”的读数规则（§0.1.3、§2.6）。

**后果**：W1 的“≤25.5% 阶段 / ≤15.8% poll 栈”（B1）和“≤25.1% / ≤11.5%”（B2）都不能作为候选上界。`progress/wait` 阶段 25.5% 同时**包含** 2,057 个 iou-wrk 的 eventfd signal 样本（占该阶段 32.8%）和 **排除** 上述 2,176 个 libc-poll 边界样本；它是混合桶，既不是 app wait 的上界，也不是干净下界。历史 5.0–5.5% 是否落在“上界”内不能由这个数字回答。

---

### H2 — P-B 的 90.7% app 样本没有 bench 帧；W3 的 0.24%、user leaf 0.49%、`wait_one` 2.85% 是可见帧的下界，“内联”解释未证明

**where**：`P2-COST-MAP.md:336-348`（§4.6）、`:384-391`（§5.2，特别是 `:391` 的“严重内联”）、`:360`（W1 的 `wait_one` 2.85%）、`:498`（§8.1.8）。

**issue**：map 把 P-B 的 W3 族级 0.24% 解释为 `reserve/accept/offer/release/begin/complete` “被内联进 `wait_one`/`run_progress_pass_`”。逐样本数据显示，真正的原因是 **用户栈在 libc syscall wrapper 处被截断**：P-B 的 app 线程 12,246 个样本中，**11,104（90.7%）没有任何 bench DSO 帧**，只有 1,142 个可见 bench 帧。`wait_one` 只出现在 700/24,585 个样本（2.85%），而 uprobe `p621_wait_one` 是 **60,000/60,000 = 1.000/op**。因此：

- 0.24% 不是 RequestCore 的族成本，甚至不是“用户态可见成本”的可信上界；它只是 1,142 个可见 bench 帧样本里的贡献。
- `wait_one` 2.85% 不是“等待路径用户代码只有 2.85%”的证据；它是 5,664/6,129 个 `__poll` 样本缺 `wait_one` 帧（其中 873 个只剩 4 帧）的截断伪影。
- `analysis.json` 的 `single_frame`（46/1,207/173/139/362）只统计“恰好 1 帧”；P-B 的 ≤4 帧样本有 **5,297** 个，`PROFILE-SUMMARY.md` 的 2,751 等数字与 `analysis.json` 不一致（map §0.2(B) 已指出），但 map 用“选择后者”代替了对截断规模的检查。

**evidence**：

- P-B app/worker 拆分（本会话）：app 12,246；`all-kernel` 154；**no bench frame 11,104**；with bench frame 1,142。作为对照，P-A 53/4,272、P-C 137/11,010、P-D 414/15,182、P-E 1,266/34,736 缺 bench 帧。
- 4 帧 poll 栈样本 873 个；`__poll` 6,129 中 5,664 个没有 `wait_one` 帧。
- uprobe 计数：`P-B-w02-uring-4k-d1/raw/stat-uprobe-waitpath.txt` 第 2 行 `60000 ... p621_wait_one`；`jsonl-uprobe-waitpath.jsonl` 的短窗 = 60,000 ops → 1.000/op。
- `analyze_p2.py:359-364` 只按 comm 过滤，不保留 pid；`parse_script()` 在 libc 帧处停止后不会再补 bench 帧。`perf record` 头 `sample_stack_user = 8192`、`exclude_callchain_user = 1`（`P-A/raw/header-record-cycles.txt`）也限制了用户栈深度。

**后果**：W3 在 P-B 的“0.24% / INCONCLUSIVE”结论方向正确（确实不可用），但 map 给出的**原因**（内联）和**数值**（0.24%、0.49%、2.85%）不能用于任何 W3 准入判断；它们必须改标为“用户帧缺失，NOT_OBSERVABLE”，而不是“小”。

---

### H3 — P-B/P-E 在 P1 A/A 中有完全匹配的格（C6/C1），map 却称“没有自己的 A/A 格”并改用更有利的非匹配代理

**where**：`P2-COST-MAP.md:425-426`（§5.6 表）、`:431`（使用规则）、`:478`（W1 准入）。

**issue**：map 写“P-B/P-E 都没有自己的 A/A 格，C5b/C3b/C7 只是相邻格代理”。这是**事实错误**：

- P-B = `W02-uring-4k-d1`，binary `03dbef09…`，与 `AA-CALIBRATION.md` 的 **C6**（`C6-w02-uring-4k-d1`，同 binary、同 case、同 tmpfs/4 KiB）完全匹配。
- P-E = `sluice-copy 4k/d4/w1 --sync none --no-atomic`，binary `db972b19…`，与 **C1**（`C1-sluice-copy-4k-d4-w1`，同 argv、同 binary）完全匹配。

map 本会话没有重跑 A/A 是事实，但“没有自己的 A/A 格”不成立。更严重的是：map 选了 **C3b cycles 0.988%**（uring d16）和 **C5b CPU 0.723%**（TP d1 w1）作为 P-B 的代理；P-B 自己的 C6 是 **cycles MDE80 2.155% / CPU 0.747%**，C1 是 **cycles 1.758% / CPU 0.589%**。map 在 `:431` 自己规定“A/A 的 hw/MDE 不能搬到别的格/后端/深度”，随后把 d16 的 cycles 带和 TP 的 CPU 带搬到 P-B（uring d1）。按同一 `AA-CALIBRATION-2` 的 MME 公式（`required_n_power` / `required_n_halfwidth`）复算 1.2% measured-loop 稀释效应：**C3b cycles 需 12/22 对，而 P-B 匹配的 C6 cycles 需 48/91 对**（C1 cycles 32/62；C6 CPU 8/14；C1 CPU 6/10）。map 的“1.2% 在 36 对可测（C5b/C3b）”在 P-B 自己的格上不成立——36 对连 C6 cycles 的 80% 功效都不够（需 48 对），更不满足 hw≤0.6%（需 91 对）。

**evidence**：

- `p1/AA-CALIBRATION.md:46-52`（C1/C6 的 case 与 argv）、`:386-402`（§5 冻结带）；C6 cycles hw 1.368% / SD 2.876pp / MDE80 2.155%；C1 cycles 1.121% / 2.346pp / 1.758%，CPU 0.365% / 0.786pp / 0.589%。
- `p2/PROFILE-SUMMARY.md:37-39` 的 binary sha256 与 C1/C6 的 A/A binary 一致；`p1/AA-CALIBRATION.md:156` 明确 C1 `db972b19…`、C2/C3/C5/C6 `03dbef09…`。
- `p2/P2-COST-MAP.md:425-426` 的代理清单与 `:431` 的使用规则自相矛盾。

**后果**：W1 的功率/预算判断不能使用 map §5.6 的“C5b 0.72 / C3b 0.99 / C7 1.42”组合；P-B 必须用 C6、P-E 必须用 C1，或明确标注“P2 未重跑 A/A，带为 P1 旧带且 C6 wall ORDER_SENSITIVE”。这不是“profile 结果冒充 A/B”，而是**A/A 格选择错误**，直接影响 W1 是否值得开 36 对 A/B 的准入判断。

---

## 2. MEDIUM 问题

### M1 — 三个 inclusive 数字是嵌套符号的求和（双计），违反 map 自己的“inclusive 不能相加”

**where**：`P2-COST-MAP.md:319`（§4.3 `poll_progress` 24.15%）、`:360`（§5.1 `eventfd_signal` 16.24%）、`:361`（§5.1 P-E `wait_one` 61.94%）、`:261`（§2.6 规则）。

**issue**：map 反复声明 inclusive 行不能相加，但三处把同一栈内的嵌套符号相加后当成单个函数/族的 inclusive：

| 位置 | map 数值 | 实际组成 | 去重后的 union |
|---|---:|---|---:|
| P-C `poll_progress` | 24.15%（3,978/16,472） | `AsyncIoContext::poll_progress()` 2,203（13.374%）+ `UringAsyncBackend::poll_progress()` 1,775（10.776%）；**both 1,773** | **2,205（13.386%）** |
| P-B `eventfd_signal` | 16.24%（3,992/24,585） | `io_eventfd_signal` 2,132（8.672%）+ `eventfd_signal_mask` 1,860（7.566%）；**mask 全部在 io_eventfd_signal 内** | **2,132（8.672%）** |
| P-E `wait_one` | 61.94%（21,514/34,736） | `wait_one()` 10,759（30.974%）+ `wait_one(duration)` 10,755（30.962%）；**both 10,755** | **10,759（30.974%）** |

**evidence**：本会话用 `analyze_p2.py` 的 `parse_script()`/resolver 逐样本统计 both/union（命令形态见 §5）。三个 union 都是直接可复算的；map 的 24.15/16.24/61.94 分别是 3,978/3,992/21,514 的和。

**后果**：P-C 的“driver progress pass 24.15%”被夸大近 2×；P-B 的“eventfd_signal 16.24%”被夸大近 2×；P-E 的 61.94% 是 map 自己用来警告“不可当 poll”的数字，却本身双计。它们不改变 stage_self 分区（每条样本只归一次），但会误导 DAG 和“扣除项”的叙述。P-E 的正确 union 30.97% 仍高于 progress/wait 25.12%，警告方向不变。

---

### M2 — M-62 的 4.74% 把 `claim_execution` 叶份额加到已包含它的 publication 阶段上（双计）

**where**：`P2-COST-MAP.md:384`（§5.2 M-62 行）、`:442`（§5.7 B6）。

**issue**：M-62 行写 `publication + claim_execution`，P-D 4.74% = publication 4.13% + `claim_execution` 0.61%。但 `analyze_p2.py:63-66` 的 publication 阶段 regex **已经包含 `claim_execution`**；`attribute()` 对 leaf=`claim_execution` 的样本返回 `publication`。因此 0.61% 是 4.13% 的子集，不能相加。B6 写“属于 B5 的子集；不可与 B5 相加”，但 4.74% > 4.13%，自相矛盾。

**evidence**：本会话逐样本统计 `claim_execution` 叶样本的 stage：P-A 10/10 publication、P-C 12/12 publication、P-D 93/93 publication、P-E 139/139 publication、P-B 0。`analyze_p2.py:65` 的 regex 明确含 `claim_execution`。

**后果**：M-62 的 P-D 上界应为 **4.13%**（publication 阶段本身），不是 4.74%；P-A 1.58→1.46、P-C 2.89→2.82、P-E 3.70→3.30。置信仍是 LOW，但“上界”必须去掉 0.61/0.12/0.07/0.40 的重复。

---

### M3 — M-67/M-61/M-65 把“阶段份额”当作“候选上界”；M-67 的 25.5% 是整个等待/eventfd/调度阶段，不是 sticky 双标志

**where**：`P2-COST-MAP.md:383-386`（§5.2 逐候选表）、`:444`（§5.7 B8）。

**issue**：§5.2 表标题是“逐候选（stage self）”，但 M-67 的“上界”直接取 progress/wait 阶段 25.53%（P-B）/25.12%（P-E）/16.29%（P-C）。该阶段的实际组成（本会话）：

- P-B 6,276：poll-only app 3,572（56.9%）、eventfd-only（主要是 iou-wrk）2,127（33.9%）、user wait-only 192、poll+user wait 297、其他 88。
- P-E 8,724：poll+user wait 3,972（45.5%）、user wait-only（progress pass）3,800（43.6%）、eventfd-only 952（10.9%）。
- P-C 2,683：poll_progress/run_progress_pass 类 1,575（58.7%）、eventfd 603（22.5%）、user wait 505（18.8%）。

sticky 双标志（`progress_epoch_`/`progress_exhaustion_`，`include/sluice/async/detail/progress_source.hpp:34,158-160,492-494`）只是 ProgressSource 内的一小段标志/原子查询，release 符号中没有独立函数（与 `signal`/`drain_notification_nolock_` 一样内联；map §8 只对后者给出 0 命中，但两者都不是可单独采样的候选符号）。25.5% 不是它的上界，甚至连“ProgressSource 等待路径”的上界也不是（含 iou-wrk 的 eventfd signal 和 scheduler）。M-61（publication 4.13%）和 M-65（terminal 3.49%）同理：阶段包含 publish/deliver 的内核/用户工作，不等于两把锁/第三把锁的融合成本。

**evidence**：上述逐样本分类；`analyze_p2.py:36-42` 的 progress/wait regex 包含 `eventfd`、`io_eventfd_signal`、`eventfd_signal`；`PROFILE-SUMMARY.md:336` 说 `signal`/`drain_notification_nolock_`/`PhysicalProbe` 在 release 符号中不存在（NOT_OBSERVABLE）。map §5.2 自己标“单候选可删除量 LOW/PARTIAL”，但“上界”列仍会被读成候选机会。

**后果**：M-67/M-61/M-65 应标 **NOT_OBSERVABLE / 仅阶段容器**，而不是“阶段上界”。W3 的候选级准入不能基于这些数字；map §7 的“W3 未达到”结论正确，但 §5.2 的表会误导预算。

---

### M4 — M-42：`fetch_add` 编译为 `lock addq`，3–5 cycles 假设严重偏低；P-D/P-E 根本没有 uring submit

**where**：`P2-COST-MAP.md:408-411`（§5.4）、`:447`（B11）。

**issue**：

1. `submit_flushes_` 的 `fetch_add` 在 release 二进制中编译为 **`lock addq $0x1,0x120(%rdi)`**（不是普通 add）。Haswell 上 L1 命中的 locked RMW 约 **19–20 cycles**，不是 map 写的 3–5 cycles。按 19–20 cycles 重算，M-42 的 cycles 上界约为 P-A 0.126–0.133%、P-B 0.066–0.070%、P-C 0.093–0.098%（若按 2 submit/chunk 假设，P-E 0.085–0.090%，但 P-E 实际不执行该计数器）；map 的 **≤0.04%** 低估约 3–7×。结论仍是低于 1% MME，但数值错误。
2. P-D 和 P-E 是 ThreadPool 形态，tracepoint `io_uring_submit_req` = 0（`P-D/P-E/raw/stat-tp-uring.txt`），`submit_flushes_` 不会执行。§5.4 仍给 P-D 0.0082% instr / P-E ≈0.0065% instr 和 0.015–0.025%/0.007–0.011% cycles，这些数字无对象。P-E 的 cycles 范围还用了 1 submit/chunk（3–5/44,481.5），而 instr 用了 2 submit/chunk（2/30,908.9），内部不一致。
3. `lock add` 的 cache line 在 submit 线程独占时约 19 cycles；若与其它 reader 竞争会更贵。map 的“远低于 16 对噪声带”在 P-A 上仍成立（0.13% vs ~1%），但不能再引用“0.020–0.033%”。

**evidence**：`objdump -d --start-address=0x424190 --stop-address=0x424230 /tmp/p2-621/symtwin/bench_w02_pipeline.sym` 输出 `4241b7: f0 48 83 87 20 01 00  lock addq $0x1,0x120(%rdi)`；`src/async/uring_transport.cpp:222`；`P-D/P-E/raw/stat-tp-uring.txt` 的 `io_uring_submit_req` 为 1（启动残差）/0；map §5.4 的 P-E cycles/instr 不一致。

**后果**：M-42 仍 **NOT_POWERED**，但 map 的“≤0.04%”不能作为上界；若未来在隔离格做 M-42，必须用 `lock` 指令的真实成本，而不是 3–5 cycles。

---

### M5 — PMU 与 record 的用户/内核分母不一致，且没有 skid/PEBS 分析；小函数份额被当成 HIGH

**where**：`P2-COST-MAP.md:155-163`（§2.1 PMU vs record）、`:397-404`（§5.3 M-53）、`:306`（§4.1 离散度）。

**issue**：

1. PMU 与 record 的 kernel 份额在 P-B 差 **10.8pp**（81.7% vs 92.51%），P-E 差 2.0pp（75.6% vs 73.60%），P-A/P-C/P-D 差 1–2.5pp。map 只说“io-wq 继承/采样偏差”，没有证明 record 与 PMU 覆盖同一组长寿 worker/主线程。`perf record` 用 `-F 2999`（freq 模式，header `sample_freq=2999, freq=1`），采样按线程时间配额而非按 cycles period 加权；`script` 输出没有 period，`analyze_p2.py` 也不按 period 重加权。对 10.8pp 的差异，至少应区分“线程覆盖不同”“skid”“unwind 缺失”，否则 `self share × PMU cycles/op = cycles/op`（M-53 的 61/58 cyc/op）会带系统性偏差。
2. `perf record` 没有 `precise_ip`/PEBS；header 显示 `max_precise=3` 可用但未用。Haswell 的 cycles 采样有 skid；`dispatch_one_locked` 1.24%、`find_live_router_cookie_` 0.4% 这类短函数可能被抹到相邻函数。M-53 的 Wilson 95% CI（34/47 样本）把 perf 样本当独立二项样本，忽略了自相关和 skid；map 却给 d16 份额 **HIGH**。M-53 最终未准入（d16 低于 A/A MDE，d64 未采集），所以不影响 M-53 的 disposition，但“HIGH”置信标签不成立。
3. 用户栈 `sample_stack_user = 8192` + `exclude_callchain_user = 1`；P-B 的 4 帧 poll 栈和 90.7% 无 bench 帧（H2）就是其后果。map 把 `Total Lost Samples: 0` 当作数据完整性证据，但 lost=0 只说明 ring buffer 没丢，不等于 callchain 完整。

**evidence**：`P-A/raw/header-record-cycles.txt`（`sample_freq=2999, freq=1, sample_stack_user=8192, exclude_callchain_user=1, max_precise=3`）；`analysis.json` 与 record DSO 计数；本会话的 P-B `__poll` 6,129 vs `wait_one` 700；map §2.1 表；M-53 34/8,369 与 47/16,472 的 Wilson CI。

**后果**：用户/内核拆分、product leaf、M-53 cycles/op 应标 MEDIUM/LOW，而不是 HIGH；不能把 record 的 kernel 份额与 PMU 的 `cycles:k` 互相换算（map 自己说了），但 M-53/M-42 的 per-op 换算又依赖这个混用。M-53/M-42 的最终 NOT_ADMITTED/NOT_POWERED 不变。

---

### M6 — `syscall unattributed` 的“不能拆分”对 978/1,251 个 libc-poll 样本不成立；stage regex 把 poll 退出的 task_work 归成 submit

**where**：`P2-COST-MAP.md:494`（§8.1.4）、`:208-216`（§2.3 trampoline/syscall boundary）、`:187-189`（§2.3 submit）。

**issue**：

1. H1 已证明 P-B 978 / P-E 1,251 个 `syscall (unattributed)` 样本含 libc `__poll`，可归到 poll 边界；map 的“不能强分给 … poll”应改为“可由 libc 帧识别一部分，剩余不可分”。
2. `attribute()`/`first_stage()` 对 leaf 非 trampoline 的样本按帧内 stage regex 取第一个匹配。P-B 有 **518** 个样本的栈是：内核 `io_free_batch_list -> __io_submit_flush_completions -> ctx_flush_and_put -> tctx_task_work_run -> tctx_task_work -> task_work_run -> get_signal -> exit_to_user_mode_loop -> do_syscall_64 -> entry_SYSCALL_64_after_hwframe`，用户 `__poll -> wait_one -> wait_for -> run_pipeline_phase`。因为 `__io_submit_flush_completions` 在 `STAGES["submit"]`（`analyze_p2.py:54-55`），这些样本被归为 **submit**，尽管 CPU 在 poll 返回路径上执行 io_uring task_work。submit 阶段 21.35%（P-A）/12.49%（P-C）因此不是纯 `io_uring_enter` 栈；8 个 stage 名与内核符号的对应是启发式的，不是“最深阶段 owner”的严格语义。
3. 这类样本进一步说明：stage self 的“行和=100%”是分区正确，但**分区标签**不是因果 owner；W1/W3 之外的“submit/reap”也不应被当成单一操作的精确成本。

**evidence**：`analyze_p2.py:54-59`（submit regex 含 `__io_submit_flush_completions`）、`:242-248`（first_stage 取帧序第一个匹配）；本会话打印的 22 帧样本（3 个重复）；518 个计数。

**后果**：stage 表仍可用于“这部分是内核机器工作”的粗分类，但 submit/reap 等阶段的名称不能用于逐操作归因；`syscall unattributed` 应拆分出“可识别 libc poll / 可识别其它 libc wrapper”的子桶，而不是整体标为不可分。

---

### M7 — 观测效应下“份额不变”未证明；record 1.59×/tracepoint 3.4× 的执行形态可能改变批处理

**where**：`P2-COST-MAP.md:28`（§0.1.8）、`:493`（§8.1.3）、`PROFILE-SUMMARY.md:297-312`（§7.2）。

**issue**：map 说“份额可用于归因，时长/吞吐不可用于收益”。但份额本身也是在 1.59×（record cycles）/2.0–3.7×（tracepoint stat）扰动下采集的：P-B 的 poll 批处理、futex 唤醒频率、io-wq punt 比例都可能随扰动变化。map 没有未插桩基线份额，也没有 A/A/重复 profile 来证明 stage 比例在 1.59× 下稳定。P-A 的 tp-uring stat 进程 wall 3.07s vs 基线 0.899s（3.41×），但 measured timed-phase wall 只有 1.15×——说明**不同 phase 的扰动不同**；stage 表是 540k ops（9 轮 × 3 phase）的混合，不能假设 timed/verify/latency 的权重与 clean run 相同。AA-CALIBRATION §3.5 已指出 measured phase 只占进程 CPU 的 24–26%，process-level 份额会被 non-timed phase 稀释。

**evidence**：`P-A/raw/header-record-cycles.txt` sample duration 1,427.239ms vs 基线 899.3ms；`P-A/raw/stat-tp-uring.txt` runtime 3,065.6ms vs 0.899s；`jsonl-tp-uring.jsonl` 10 条记录、timed wall 和 0.492s；`AA-CALIBRATION.md:339-348`（measured CPU 24–26%）。

**后果**：stage 份额的**方向**可用；具体百分比（25.5%、15.8%、4.13% 等）不能当作 clean-run 的份额，更不能直接乘 PMU cycles/op 得到候选成本。需要至少一次无插桩/低扰动 profile 或重复 record 来给出份额稳定性。

---

### M8 — 环境/A/A 可比性：io-wq worker 可能跑在 taskset 之外，`nmi_watchdog=1`，背景负载在 profile 期间未被逐轮记录

**where**：`P2-COST-MAP.md:65-91`（§1 执行 DAG 未提 worker 亲和性）、`PROFILE-SUMMARY.md:20-31`（环境快照）、`p1/AA-CALIBRATION-2.md:284`（背景负载警告）。

**issue**：

1. 所有 bench 用 `taskset -c 2,3`，但 PMU `task-clock` 与进程 wall 的比例超出 2 CPU 上限：P-A 2,593.96ms / 0.8993s ≈ 2.88 CPU，P-B 7,211.68ms / 2.3989s ≈ 3.01，P-C 3,487.02ms / 1.1520s ≈ 3.03，P-D 3,450.13ms / 1.1716s ≈ 2.94，P-E 1,197.82ms / 0.3984s ≈ 3.01；bench 自身报告 `util=181–190%`。可能的解释是：`perf stat -r 3` 的 task-clock 聚合口径与 `runtime_ns` 不是同一分母（本复核没有重跑 `-r 3` 来定标），或 io-wq/TP worker 并未全部受 `taskset` 限制。map 没有记录 worker 的 `Cpus_allowed_list`，也没有解释这个比例。若是后者，profile 的总 cycles/op 包含 2,3 号 CPU 以外的 worker 执行，而 A/A 的 `CPU/task` 是 `RUSAGE_CHILDREN` 的 user+sys（内核 io-wq worker 不是 bench 子任务，可能不在该指标内）；两者的“过程 CPU”不是同一分母。**在 owner 澄清 task-clock 口径前，不能把这一项写成“worker 跑在 taskset 之外”，但必须把它列为未解释的环境/可比性问题。**
2. `snapshot-before.txt` 的 `nmi_watchdog=1`；NMI watchdog 会周期性打断并占用一个 PMU 计数器。198 行 `percent_enabled=100` 说明没有复用缩放，但 NMI 处理会计入 cycles:k/IRQ 桶，map 的 §4.4 没有提。
3. profile 只有 cell 级 before/after loadavg（0.02→0.45）；AA-CALIBRATION-2 记录 `zcode-cli` 在 16/16 对出现（≥5% CPU）。profile 期间没有逐轮 `ps`/loadavg 记录，`scheduler/PSI/IPI` 6–12% 中可能含背景负载，不能全部归给 workload。

**evidence**：`P-A/raw/stat-pmu-r3.txt` task-clock 2,593.96ms；`P-A/raw/stat-pmu-r3.stdout` 的 `util=181.6–190.4%`；`p2/profile/env/snapshot-before.txt` `nmi_watchdog=1`；`p1/AA-CALIBRATION-2.md:276-285`；`P-A/raw/script-cycles-sym.txt` 的 iou-wrk comm。

**后果**：这不推翻“kernel/worker 主体”的结论（PMU 直接计数），但影响“同机同 taskset 2,3”的严格性，以及 A/A CPU 带与 profile cycles/op 的可比性。W1 的 app poll 路径不受 worker 归属影响；W3 的 TP worker 会受影响（TP 线程继承 affinity，但 `RUSAGE` 是否含全部线程需核实）。建议在报告中记录 worker 的 `/proc/<pid>/task/*/status Cpus_allowed_list` 或 `perf script` 的 CPU 字段。

---

### M9 — 复算脚本未入库，map 的“记录员复算”数字有可复现性缺口与若干小数差异

**where**：`P2-COST-MAP.md:11`（复算声明）、`:576-580`（§A.3“没有把复算脚本入库”）。

**issue**：map 的 trampoline callee 分解、per-comm leaf、product leaf、inclusive 家族数字来自一次性 `python3 -I - <<'PY'`，脚本不落盘。本复核用同一 `analyze_p2.py` 函数重算，发现：

- P-C product leaf：map 897（5.45%），本复核 **883（5.36%）**；差异 14 个 `(anonymous namespace)::driver_thread` 样本（map 的“按 `sluice::`/`sluice_copy::` 前缀”规则不包含它）。
- P-B iou-wrk `rep_movs`：map 15.20%，本复核 **15.28%**（分母/过滤差异）；P-C 20.10% vs **20.18%**；`_raw_spin_lock` P-A 9.59% vs **9.96%**。
- P-C product 的 bench harness/other 分类也有 22 个 `_init`/`std::sort` 样本的差异。
- 上述 M1 的三个双计数、M2 的 claim 双计、H1 的 poll 边界样本都来自同一类“子串/嵌套 inclusive 聚合”错误。

**evidence**：本会话 `python3 -I /tmp/p2review/recompute.py`（import `analyze_p2.py`）输出 `/tmp/p2review/recompute.json`；`analysis.json` sha256 与 `SHA256SUMS` MATCH；`P2-COST-MAP.md:576-580` 自述脚本未入库。

**后果**：map 的“记录员复算，MEDIUM 置信”不能按原数字复现；至少应把复算脚本以只读方式随证据归档（不违反一次性脚本纪律：它是证据生成器，不是产品工具），或把每个数字的计算式写入表注。否则 H1/M1/M2 这类错误无法被后续 reviewer 快速发现。

---

### M10 — W1 的“PARTIAL PASS”只是 bench/reference 形态的 hotspot，不是 ledger 的“真实消费者 profile”门

**where**：`P2-COST-MAP.md:478`（§7 W1）、`CAMPAIGN-LEDGER.md:257`（W1 门禁）。

**issue**：ledger 的 W1 准入门槛是“**真实消费者 profile** + 有界等待/非阻塞 drain/dup fd 设计 + A/A 36 对 + A/B ≥36 对”。map 的 P-B 是 benchmark，P-E 是 `sluice-copy` 参考应用；ledger §6.2 明确 `sluice-copy` 是参考应用、不是 G0 真实 workload，#604 G0 尚未准入。map 的 §7 把判定限定为“是否有 measured hotspot”，并写“未达到候选级准入”，方向正确；但“PARTIAL PASS”与 ledger 的“profile 准入”同词，容易被读成已过 W1 的 profile 门。应明确：**连 ledger 的 profile 门也未过**，因为真实消费者缺席。

**evidence**：`CAMPAIGN-LEDGER.md:257`（W1 行）、`:323-326`（#604 G0/HOLD_NO_REAL_APPLICATION、sluice-copy 定位）；`P2-COST-MAP.md:478`；`PROFILE-SUMMARY.md:59`（P-E 是 sluice-copy）。

**后果**：W1 可以进入“继续做语义/消费者准入”的队列，但不能以本 profile 作为“profile 门已过”的证据；A/A/A/B 预算仍应等待真实 workload 或 owner 明确接受 bench 代理。

---

## 3. LOW / 检查通过项

- **L1（通过）**：stat 行 198/198 `100.00`，无缩放；Lost 15/15 = 0。map §2.1/§7.1 的这两个硬声明成立。
- **L2（通过）**：stage_self 表与 `analysis.json` 完全一致，行和 100%；P-C/P-D/P-E 的 trampoline 行（4.61/13.67/8.87%）确实是 analysis 的 `startup/thread create`，且 `syscall_owner()` 的 clone3 搜索确实会把 worker 栈根标成 startup（`analyze_p2.py:272-273`）。map §0.2(A) 的更正机制成立。
- **L3（通过）**：d16→d64 没有被外推。M-53 的 d64 明确 NOT_OBSERVABLE（§5.3/§5.7 B10），只引用 C7 d64 A/A 的 16 对边缘（t-hw 1.007% vs 1.0%，17–20 对），与 `AA-CALIBRATION-2.md:27,499` 一致。
- **L4（通过）**：没有把 profile 当 A/B。§5.1/§5.2/§5.6/§6/§7/§8.2 都有“不是 A/B、不能当收益”的限定；§6 对 #637 的 NO_GO/INCONCLUSIVE 保持不推翻。
- **L5（通过）**：M-53 源码 O(R) 反向扫描、`io_uring_queue_init(..., 0)` flags=0、`submit_flushes_` 的唯一读者在测试 seam，均与源码一致（`src/async/uring_transport.cpp:551-557`、`src/async/uring_backend.cpp:171`、`src/async/uring_test_seams.hpp:75-76`）。
- **L6（低）**：P-B `wait_one` 2.85% 与 P-E 30.97% 的 union 已在 M1/H2 修正；`__poll` 24.93% 与 `__x64_sys_poll` 15.77% 的选择已在 H1 修正。
- **L7（低）**：P-D/P-E `worker_loop` inclusive 68.28%/48.30%、leaf 2.24%/1.17%、off-CPU futex 22,135/80,918（68.6%/48.6%）均与 `analysis.json` 一致；TP worker 成本没有“缺失”，只是不能用 comm 拆分（`work_mtx_`/`worker_loop`/off-CPU site 足够）。
- **L8（低）**：P-A/P-B/P-C 的 iou-wrk 样本（48.95/50.19/33.16%）和 leaf `rep_movs`（24.09/15.28/20.18%）说明 worker 侧 payload 拷贝被记录；不是“只测了主线程”。
- **L9（低）**：`__irqentry_text_end` 的 raw report 份额（P-A 3.57%、P-B 0.01%、P-C 1.35%、P-D 0.21%、P-E 0.03%）与 map §4.4 一致；设备 IRQ 确实不可观测（block 0 事件、tmpfs、irqbalance inactive）。
- **L10（低）**：uprobe 只用于调用计数、未被用于性能份额；`stat-uprobe-1` 的 110× 是独立 run，没有污染 cycles record。map 的这一隔离正确。
- **L11（低）**：`P2-COST-MAP.md:34-48` 的 P-C/P-D/P-E trampoline callee 分解（lll_lock/pread/pwrite/read）机制成立；P-D 2,031 vs 2,075、P-E 2,340 vs 3,082 的差异 map 已披露（“其余/复算”说明），不改变“不是线程创建成本”的结论。

---

## 4. 逐项检查矩阵（对应任务要求）

| 检查项 | 结论 | 依据 |
|---|---|---|
| 双重计数 / overlap | **发现问题（M1, M2）**；stage_self 分区本身不双计 | `poll_progress` 24.15→13.39；`eventfd_signal` 16.24→8.67；`wait_one` 61.94→30.97；M-62 4.74→4.13（P-D） |
| 采样偏差（skid / missing samples） | **发现问题（H2, M5）** | 无 precise_ip/PEBS；8KB 用户栈；P-B 11,104/12,246 app 样本无 bench 帧；P-B 10.8pp PMU/record kernel 差 |
| 内核/worker 成本缺失 | **未发现系统缺失**（L7/L8）；但 record/PMU 覆盖与亲和性未证明（M5/M8） | iou-wrk 33–50% 样本；worker_loop inclusive 68.28/48.30%；off-CPU 68.6/48.6%；PMU task-clock ≈2.9 CPU |
| uprobe 开销污染 | **未发现污染**（L10）；uprobe 数字未进入份额 | `stat-uprobe-1` 110×、`stat-uprobe-waitpath` 4.4×；uprobe 只在 §8/§5.1 计数 |
| 事件丢失 | **Lost=0 成立**（§0.1）；但 `lost=0` 不等于 callchain 完整（H2/M5） | 15/15 `Total Lost Samples: 0`；4 帧 poll 栈 873 个 |
| 以函数/阶段份额冒充可删 cycles | **发现问题（M3；H1 的上界；M2）** | M-67 25.5% 是整阶段；M-61 4.13% 是 publication 阶段；M-62 双计；map 自己多处限定“不是可删量” |
| 以 d16 结论外推 d64 | **未发现**（L3） | M-53 d64 NOT_OBSERVABLE；C7 d64 只按 A/A 分辨力引用 |
| 以 profile 结果冒充 A/B 证据 | **未发现**（L4）；但 W1 “PARTIAL PASS”措辞需限定（M10） | 全文无 A/B 判词；§7 明确“未达到 A/B 准入” |

---

## 5. 复现命令与限制

本复核执行的命令（均在 `docs/evidence/621-progress-wait-path-20261118/` 下；没有重跑 perf/bench/test）：

```bash
# 1) 独立逐样本复算（import analyze_p2.py，复用 resolver/parser/attribute）
python3 -I /tmp/p2review/recompute.py
#    -> /tmp/p2review/recompute.json
#    analyze_p2.py sha256 = b822ea3a705f885c4793ba55bcd2a6d8c37e2b3a7904803ab2754eead24ab105

# 2) stat 行与 Lost 回执
python3 -I - <<'PY'  # 见 §0；9 文件 198 行全部 100.00
PY
for f in p2/profile/P-*/raw/lost-record-*.txt; do grep -m1 'Total Lost Samples' "$f"; done

# 3) M-42 指令
objdump -d --start-address=0x424190 --stop-address=0x424230 \
  /tmp/p2-621/symtwin/bench_w02_pipeline.sym | sed -n '1,25p'
#    4241b7: f0 48 83 87 20 01 00  lock addq $0x1,0x120(%rdi)

# 4) 哈希
cd p2/profile/analysis && sha256sum -c SHA256SUMS

# 5) 针对 H1/H2/M1/M2 的内联统计
#    - exact __poll / __x64_sys_poll / wait_one / eventfd_signal / poll_progress 的 both/union
#    - libc __poll but no kernel poll 的 stage 分布
#    - claim_execution leaf 的 stage 分布
#    - app comm 的 no-bench-frame / frame-count 分布
#    均用 analyze_p2.py 的 make_resolver()/parse_script()/attribute()
```

**限制**：

- 没有重跑 `analyze_p2.py`（会重写 `/tmp/p2-621/analysis.json`，且依赖会话内 kallsyms/twin）；本复核用同一脚本的函数，输入是已归档的 `script-cycles-sym.txt` 与 `analysis.json`（sha256 MATCH）。
- 没有重跑任何 perf/bench/test、语义 oracle、A/A、A/B；因此本复核不能给出新的时间/吞吐数字，只能重算既有 profile 份额。
- 内核无 debuginfo、release 无 DWARF：本复核和 map 一样只能到符号级；行级结论仍是 NOT_OBSERVABLE。
- `P2-COST-MAP.md` 的一次性复算脚本未入库（§A.3）；H1/M1/M2/M9 的某些数字依赖本复核的内联脚本，命令形态已记录，但脚本本身未落盘。

---

## 6. 最终 verdict：能否用于 W1–W6 准入裁决

**不能。** 具体：

- **形态级 hotspot 筛选：可以。** PMU 直接计数（kernel 71.9–82.3%）、off-CPU（P-B poll 59,947/119,969、P-E poll 83,481/166,425）、tracepoint（poll 1/op、1.317/chunk、futex 2.79–3.84/op）、leaf 符号（rep_movs、pthread_mutex、entry trampoline）足以支持“内核/worker 机器工作是主体；P-B/P-E 的等待路径有热点；M-53 d16 很小；M-42 效应很小”的方向性结论。这可以作为 #617 trigger-grade / 后续 profile 的入口。
- **W1：不能用于准入。** 等待路径 hotspot 真实（poll 1/op、off-CPU 50%、stage 25.5%），但 15.8%/11.5% 的“poll 栈上界”实为下界（H1）；`__poll` 24.93%/15.83% 才是可复算的 inclusive；stage 25.5% 混入 iou-wrk eventfd signal 并排除 libc-poll 边界。P-B 的匹配 A/A 是 C6（不是 C5b/C3b/C7），P-E 的匹配 A/A 是 C1（H3）。真实消费者 profile 仍未满足（M10）。
- **W3：不能用于准入。** 只有族级/符号级；P-B 的 0.24% 和 0.49% user leaf 建立在 9.3% 可见 bench 帧上（H2）；单候选 M-61/M-62/M-65/M-67 只有阶段/双计数字，没有候选级 measured hotspot（M2/M3）。map 的“W3 未达到”结论正确，但 §5.2 的数字不能作为预算。
- **W4/M-53：不能用于准入。** d16 份额 HIGH 标签不成立（无 PEBS/skid、34/47 样本、低于 A/A MDE）；d64 未采集；三项迁移义务未证。map 的“未达到”正确。
- **W6/M-42：不能用于准入。** `lock addq` 成本被低估（M4）；P-D/P-E 格不适用；隔离格/机制门未做。NOT_POWERED 正确。
- **W2/W5：NOT_APPLICABLE**，本 profile 不改变其状态；W4 的 M-40/M-63 等未被本 profile 覆盖。

**建议的最小修正顺序**（不改变本 verdict）：

1. 修 `syscall_owner()`/`first_stage()`：把裸 `__poll` 加入 poll 识别，或把 libc-poll-only 样本单列 `poll-boundary`；重算 W1 的 inclusive 上界（P-B 24.93%、P-E 15.83%）和 stage 组成（app poll / worker eventfd / progress-pass 分开）。
2. P-B 用 C6、P-E 用 C1 的 A/A 带（或明确标注 P2 未重跑和 C6 wall 顺序敏感）；不要再引用 C5b/C3b/C7 作为 P-B 的“代理上界”。
3. 把 M-62 的 claim 重叠去掉；M-67/M-61/M-65 改标“阶段容器 / NOT_OBSERVABLE”，不给候选上界。
4. M-42 用 `lock add` 实测/隔离格重估上界；删除 P-D/P-E 的 M-42 数字。
5. 归档复算脚本或把每个数字的计算式写入表注；补一次低扰动 profile 以证明 1.59× 下份额稳定。
6. 在 W1 的准入表里明确：本 profile 只证明 bench/reference 形态的 hotspot，未过 ledger 的“真实消费者 profile”门。

---

*本文件结束。所有未在 §0/§5 执行清单中的断言都标明为未运行/未验证；没有把推测写成结果。*
