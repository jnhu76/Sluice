# p4/W3-ROWLEVEL 独立复核

| 字段 | 值 |
|---|---|
| 日期 | 2026-11-18 |
| 复核对象 | `p4/W3-ROWLEVEL.md`（542 行）及 `p4/profile/` 原始数据 |
| 复核基线 | `5d94f233967c3aef993eb19ba9e3252c532b0951` |
| 输入 | `p4/W3-ROWLEVEL.md`；`p4/profile/{analysis,env,P-*/raw}`；`p2/P2-COST-MAP-R1.md`；`p1/AA-CALIBRATION.md`；`p1/AA-CALIBRATION-2.md`；`p4/M53-OBLIGATIONS.md`；`CAMPAIGN-LEDGER.md` |
| 性质 | 只读复核。未修改产品代码、未重跑 perf/bench/test/A-A/A-B；只重新读取归档 raw、重跑归档分析器、做独立计数与统计复算。 |
| 结论 | **W3 RequestCore 的 mutex 行级定位可复现、可作为方向性证据；但本报告不能作为 W3 定量准入或 M-53 终判依据。** M-53 的 d64 效应在 C7 16 对 A/A 的 t 半宽 1.007% / MDE80 1.416% 之下或边缘，且与 d16 差异不显著；阶段/家族对比因 claim_execution 的 stage 映射错误而不成立；W1/P-E 的 `wait_one` owner 主要是 mutex，不是 poll。 |

> 复核立场：本文只尝试推翻或限定 W3-ROWLEVEL 的候选结论。凡是复核未能复现或未能以本会话证据确认的，均写“未执行/不确定”，不替报告补数字。

---

## 0. 结论摘要

1. **DWARF/内联帧解析本身没有发现错误。** `addr2line -i` 的链顺序、`perf script --inline` 的组解析、候选 self/inlined/callsite 的代码路径与报告描述一致；重新运行归档分析器得到与 `analysis-w3.json` **逐字节相同**的 JSON。二进制 sha256、`.text`/`.rodata`/`.data` 与 P1 release 相等、DWARF 存在，均有本会话命令/日志支持。
2. **但报告的三类结论不能按原文使用：**
   - **M-53 d64**：点估计可复现（self 168/15,874=1.058%，owner 179/15,874=1.128%），但 d64 与 d16 control 的差异不显著（self +0.170pp，z=1.23，p=0.22；owner +0.148pp，z=1.03，p=0.30）；匹配的 C7 d64 A/A 16 对 cycles 的 t 半宽 **1.007%**、MDE80 **1.416%**，而 d64 的 all-comm period self/owner 仅 **0.593%/0.632%**。报告没有把 C7 的 A/A 分辨力与 `M53-OBLIGATIONS.md` 的“半宽 ≤ 效应/2”门写进 M-53 判定。
   - **阶段容器 vs 候选**：`claim_execution` 在报告自己的 p2 R1 classifier 中属于 **publication** stage（`analyze_w3.py:162-165`；`P2-COST-MAP-R1.md:494` 明说“claim 已在其中”），报告却把它记为 `execution` 家族、stage 写作“—（跨 terminal/publication）”，导致 publication 的 family/stage 比值为 26.8/42.1/43.2%，而把 execution 并入 publication 后（sample count）是 **68.6/50.1/76.0%**（period 加权 65.5/51.0/78.4%）。observation 家族也有 286/377/518 个样本落在 publication stage；报告给出的 76.4/101.5/102.5% 不是 observation stage 的覆盖率。
   - **W1/P-E 的 wait 归因**：`wait_one`/`wait_if_unchanged` 的 owner 主要是 mutex；P-E 有 10,930 个 libc `__poll` 样本，但 `wait_one` 的 callsite leaf `__poll` 只有 86 个（`wait_if` 同）。libc 的 `__internal_syscall_cancel/__syscall_cancel` 两个帧把 `__poll` 与候选隔开，`callsite-extra` 的“直接调用者”规则漏掉了 poll。W1 的 poll 相关成本（P2 `__poll` 子集 15.83%）在 inclusive/stage 中，不在 3.44%/2.08% 的 owner 里。
3. **skid 完全没有被分析。** perf record 是 `-F 4999 -g --callgraph dwarf`，raw header 无 `precise_ip`；报告没有 PEBS/skid 小节，§8 也没有把 P2 M5 的“无 PEBS/skid”限制继承下来。函数级 attribution（尤其 d64 self）较稳，但 552/553/557 的行级拆分没有 skid 证据；P2 对 M-53 d16 的置信就是因“小样本/无 PEBS/skid”降为 MEDIUM，W3 却在同一限制下把 d64 写成 hotspot。
4. **报告对“份额 ≠ 可删量”“阶段容器 ≠ 候选”的文字纪律是正确的**；问题不在它主动把 stage 当候选，而在 **stage/家族算术**、**sample-count 与 period 加权混用**、以及 **callsite-extra 的覆盖缺口**。行级份额没有被直接当成可删量；§6.4 还明确说 111 cyc/op 不是可删量。这一点应保留。

**Verdict（归因是否可用于 W3/M-53 的终判）**：**W3 的 RequestCore mutex 行级定位可作为方向性证据，但不能作为 W3 定量准入依据；当前报告不能用于 M-53 终判。** M-53 应维持 `INCONCLUSIVE`/`NOT_OBSERVABLE`（按 `M53-OBLIGATIONS.md:313` 的“d64 A/A 半宽 ≤ 效应/2”门）；W3 若要用本报告做准入，必须先修正 §3.3/§4.3/§5.3 的 stage 映射、补 period 加权、补 skid/PEBS 限制，并把 `wait_one` 的 mutex owner 与 poll 等待分开。

---

## 1. 本轮实际执行的检查

| # | 检查 | 命令 | 结果 |
|---:|---|---|---|
| 1 | 归档分析器复现 | `cd docs/evidence/621-progress-wait-path-20261118 && python3 -I p4/profile/analysis/analyze_w3.py --json-out /tmp/w3-recompute.json --txt-out /tmp/w3-recompute.txt` | exit 0，65.4 s；`sha256sum` 两文件均为 `312551b6694cf59880153d45eeb7970adeec0e4e33a2f4151f04a32fba571e6a`；`cmp -s` = 0，逐字节相同 |
| 2 | 原始数据完整性 | `cd p4/profile && sudo -S -p '' sha256sum -c SHA256SUMS < /tmp/sluice621-sudo` | exit 0；155/155 `OK`，无 FAILED |
| 3 | 二进制身份 | `sha256sum /tmp/621-p4-build/ring/.../bench_w02_pipeline`；`file` | `e65a5766...2eb9b8`，与报告 §1.1 一致；`with debug_info, not stripped` |
| 4 | 代码一致性 | `cat p4/logs/text-compare-vs-p1.log` | `.text/.rodata/.data` 全部 `IDENTICAL`（ring/noring/copy；VMA/section size 相同） |
| 5 | addr2line 链顺序 | `addr2line -e .../bench_w02_pipeline -f -C -i -a 0x4264a4 0x4235e0 0x4235e9 0x4235f9 0x423600` | 0x4264a4 链为 `__gthread_mutex_lock → std::mutex::lock → lock_guard → claim_execution:368`；0x4235e0→552、0x4235e9→552、0x4235f9→557、0x423600→556 |
| 6 | 反汇编交叉检查 | `objdump -d --start-address=0x4235a0 --stop-address=0x423610 .../bench_w02_pipeline` | 循环体 `sub/test/je/cmp in_use/cmp cookie`；成功返回 `0x4235f9 mov %rdx,%rax; ret`；未找到返回 `0x423600...`；报告 557 行描述见 §2.6 |
| 7 | 原始行级独立计数 | 本次 `python3 -I` 直接解析 `script-inline-{run-01,run-02}.txt`，按同一 group 规则统计 `find_live_router_cookie_` | self：d64 run-01 79/7,926=0.997%，run-02 89/7,948=1.120%，合计 168/15,874=1.058%；control-d16 68/7,654=0.888%。inclusive（栈中任意 find_cookie 帧）：d64 run-01 93、run-02 101；control 85。owner（self+callsite）：d64 179、control 75（aggregate）。 |
| 8 | 阶段/家族独立重算 | 本次 `python3 -I` 导入 `analyze_w3.py` 的 `parse_script/attribute_r1_dedicated/CAND`，逐样本重算 family 的 stage 分布与 stage 的 family 覆盖 | P-D execution 814/814 全在 publication；publication+execution union 1,337/1,948=68.6%；P-C 738/1,474=50.1%；P-E 1,912/2,517=76.0%；period 加权 65.5/51.0/78.4% |
| 9 | observation 跨 stage | 同上，统计 observer 家族样本的 stage | P-D 725 observation + 286 publication；P-C 499 + 377；P-E 882 + 518；P-F 535 + 69 + 8 syscall |
| 10 | `__poll` 覆盖 | 本次 `python3 -I` 统计 app 样本中有 libc `__poll` 帧的样本，并查 `wait_one`/`wait_if_unchanged` 的 group index | P-E 10,930 个 `__poll`；stage：progress/wait 7,825、poll boundary 3,068、payload/FS 37；`wait_one` callsite `__poll` 86、`wait_if` 86；P-D 227 个 `__poll`，`wait_one` callsite `__poll` 2 |
| 11 | 栈深度 | 读 `analysis-w3.json` 的 `app_depth_hist` | depth≤4：P-D 2,176/52,243=4.17%、P-C 673/35,836=1.88%、P-E 2,950/69,371=4.25%、P-F 132/15,874=0.83%；报告仅列 single-group |
| 12 | inline 展开/single-group 校验 | 本次 `python3 -I` 复算 | with-inline：P-D 49,281/52,243=94.3%、P-C 31,235/35,836=87.2%、P-E 66,456/69,371=95.8%、P-F 14,584/15,874=91.9%；single-group 522/249/655/77，均与 §2.1/§2.4 一致 |
| 13 | d64 vs d16 统计 | 本次 `python3 -I` 两比例 z 检验/Wilson CI（独立样本近似） | self +0.170pp，z=1.23，p=0.2205；owner +0.148pp，z=1.03，p=0.3042；552 +0.020pp，p=0.845；553 +0.076pp，p=0.381；557 +0.074pp，p=0.074 |
| 14 | C7 A/A 数值 | 读 `p1/AA-CALIBRATION-2.md:222-238,397-408,485-506`；`p4/M53-OBLIGATIONS.md:295-313` | C7 cycles SD 1.889、bootstrap hw 0.893%、t-hw 1.007%、MDE80 1.416%；δ=1% 需 58 对（半宽）；M-53 门为“半宽 ≤ 效应/2” |

**未执行**：没有重跑 perf/bench/AA/AB；没有 PEBS/skid 实验；没有内核 debuginfo 行级；没有把 H1 patch 应用到 HEAD；没有做真实消费者 profile。

---

## 2. 逐项复核

### 2.1 DWARF / 内联帧解析 — 未发现解析错误，但有两个限制

**已验证正确：**

- `addr2line -f -C -i -a` 的输出顺序是 **最内层 inline 在前、最外层实际函数在后**。报告 §2.3 用 `ch[-1]` 取 candidate-own line、`ch[0]` 取 actual instruction line，方向正确；`0x4264a4` 的链 `__gthread_mutex_lock(gthr-default.h:795) → std::mutex::lock(std_mutex.h:116) → lock_guard(std_mutex.h:276) → claim_execution(request_core.cpp:368)` 与报告的 line 368/`gthr-default.h` 解释一致。
- `perf script --inline` 的 group 解析：同一 IP 的 inline 帧被合并为一组（`analyze_w3.py:313-320`），`real0` 取组内第一个非 `(inlined)` 帧，self/inlined 的区分与报告的 P-D `claim_execution` 例子（`std::mutex::lock`、`lock_guard`、`claim_execution` 同 IP）一致。
- 所有 app 样本的 binary IP `unresolved=0`（JSON 字段），候选 self 行没有 `??:0`；报告 §2.1 的 957/997/897/1,050 与本次复算一致。
- inline 展开百分比、single-group 百分比分别与报告 §2.1/§2.4 的数字一致（见 §1 表 #12）。
- 二进制 DWARF/`.text` 与 P1 release 的逐字节相等；`addr2line` 解析到的源码行与当前 source tree 的 line 一致（`request_core.cpp:348/368/378/394/409`, `progress_source.hpp:203/205`, `uring_transport.cpp:552/553/557`）。

**限制（不是解析 bug，但影响结论）：**

1. **perf script 与 addr2line 的 inline 链可能不同。** 报告 §2.3 明确说“把 perf script 的 inline 函数名并入 inclusive/callsite 名称集合；两者不一致时以最内层实际指令行为准”。`analyze_w3.py:472-479` 的 `gnames` 是 perf script 名称与 addr2line 链名称的**并集**；如果两条链对同一 IP 给出不同 inline 函数，candidate 可能因为并集而进入 `in_group0`/`in_group1`，导致 callsite/inlined 的假阳性。报告没有给出不一致率或抽样核对。对 mutex 直接调用点，perf script 与 addr2line 通常一致；但这条风险没有被量化。
2. **8 KiB 用户栈 / `exclude_callchain_user=1` 未在 W3 限制中讨论。** raw header（`P-F-.../raw/header-run-01.txt` 第 1 行）有 `sample_stack_user = 8192`、`exclude_callchain_user = 1`。报告的 §2.4 只列 single-group（P-D 1.0%、P-C 0.7%、P-E 0.94%、P-F 0.49%），没有列 depth≤4 的样本；JSON `app_depth_hist` 显示 depth≤4 为 4.17/1.88/4.25/0.83%。这些栈可能仍缺 bench 根（P2 H2 的 4 帧 poll 栈就是这种形态），所以 **inclusive 是下界**；self/callsite 用 leaf/直接调用者，受根截断影响较小。报告 §8.7 只说 single-group 的 inclusive 只能到 leaf，未把“多帧截断”的 inclusive 下界说清。

**判定**：DWARF/内联解析 **PASS**；报告关于“所有 IP 解析成功、`??:0=0`、inline 展开比例”的陈述可复现。上述两个限制是证据强度问题，不是解析错误。

### 2.2 skid — 报告完全缺失，是 M-53 行级结论的硬限制

- 采集命令（`p4/W3-ROWLEVEL.md:61-77`）是 `perf record -o ... -e cycles -F 4999 -g --call-graph dwarf -- taskset ...`；raw header 的 `sample_type = IP|TID|TIME|CALLCHAIN|PERIOD|REGS_USER|STACK_USER`，**没有 `precise_ip`**，也没有 PEBS/`--aux` 配置。`perf script` 的 sampled IP 是中断 skid 后的 IP，不是精确退休指令。
- 报告 §2.3 只讨论 inline 链多义性，§8 的 20 条限制中没有 skid/PEBS；`W3-ROWLEVEL.md` 全文没有出现“skid/PEBS/precise_ip”。P2 R1 的 M5 明确把“无 PEBS/skid”列为 M-53 及小函数份额的置信降级原因（`P2-COST-MAP-R1.md:61,511,642`），W3 没有继承这条限制。
- 具体影响：d64 `find_live_router_cookie_` 的行级拆分是 552（84/168）+553（66/168）+557（18/168）；552 是 `for` 循环 init/条件，553 是 `in_use && cookie`，两个地址相邻（0x4235e0..0x4235f7）。没有 PEBS，单个采样 IP 可能滑到相邻行；报告把 89.3% 的“扫描循环”份额写成行级事实，但没有 skid 误差分析。函数级 `find_cookie` self/owner（168/179）比行级 84/66 更稳，但 168 个样本中仍有 18 个落在 557（见 §2.6），skid 可能改变 552/553/557 的分配。
- 对 W3 的 mutex 行（348/365、368/378、394/409、439/460、463/484、325/344）同理：这些是 `lock_guard` 构造/析构行，前后相邻；报告给出 326/528、220/371 等成对计数，但没有 skid 控制。mutex 调用点本身是明确的（函数体只有一把锁），所以 **函数级/调用点级** 结论比 **行级** 结论更稳；报告应把行级拆分标为 skid-limited，而不是精确事实。

**判定**：skid 检查 **FAIL**。如果 W3/M-53 的终判依赖行级份额（尤其 M-53 的 552/553 机制证据），当前报告不能提供该证据。

### 2.3 样本量 — d64 点估计可复现，但 d64-vs-d16 与 C7 噪声带不支持“hotspot”终判

**报告的数字是对的：**

- d64 `find_live_router_cookie_` self 168/15,874=1.058% app，0.503% all（sample count）；owner 179/15,874=1.128% app，0.536% all；callsite 11（0.069%）；inclusive 194（1.222%）。
- d16 control self 68/7,654=0.888% app，0.401% all；owner 75/7,654=0.980% app，0.442% all；inclusive 85（1.111%）。
- 本次独立解析的 per-run 计数：d64 run-01 79/7,926=0.997%，run-02 89/7,948=1.120%；control 68/7,654=0.888%。点估计可复现。

**但“d64 是 hotspot、d16 不是”是阈值/样本量伪影：**

- 用两比例 z 检验（独立样本近似，已是最乐观假设）：self 168/15,874 vs 68/7,654，差 +0.170pp，z=1.23，p=0.22；owner 179/15,874 vs 75/7,654，差 +0.148pp，z=1.03，p=0.30。**不显著。**
- Wilson 95% CI：d64 self 0.911–1.229%，d16 self 0.701–1.125%；两个 CI 大幅重叠。d64 owner 0.975–1.304%，d16 owner 0.782–1.226%。
- d64 是两次 run，d16 control 只有一次 run（`per_run_samples` control 只有 `control-d16-run-01`）；app 样本 15,874 vs 7,654。控制组样本量约为 d64 的一半，比较不平衡。报告把 d16 标为 below-threshold、d64 标为 hotspot，但 d16 owner 0.980% 与 d64 1.128% 的差只有 0.148pp，远小于两者的 CI 宽度。
- **C7 A/A 是决定性的外部证据**：`p1/AA-CALIBRATION-2.md:222-238` 的 C7 `W02-uring-4k-d64` cycles/task 16 对 SD=1.889pp、bootstrap hw=0.893%、t-hw=1.007%、MDE80=1.416%。`p4/M53-OBLIGATIONS.md:313` 要求 M-53 的 d64 A/A “半宽 ≤ 效应/2”；若效应取报告 app owner 1.13%，半宽需 ≤0.565%，而 C7 16 对 t-hw 是 1.007%（bootstrap 0.893%），**不达标**，按 AA-C2 §6.4 的 δ=1% 行约需 58 对；若效应取 all-comm period owner 0.632%，半宽需 ≤0.316%，约需 222 对（AA-C2 δ=0.5% 行）。报告没有引用 C7/M53-OBLIGATIONS 的这条门，反而把 d64 app owner 1.13% 写成“candidate-level measured hotspot”。
- 报告 §6.1 的自我限制“d64/d16 cyc/op 比 1.24×，不是 R 的线性比，不能外推”和“平均扫描深度 NOT_OBSERVABLE”是对的；但 §6.1/§6.4/§7 仍把 d64 写成 hotspot，且没有给 CI/p 值。结论应改为：**d64 在 app 样本的点估计约 1.1%，可复现；但相对 d16 的增量与零不可区分，且低于匹配 C7 A/A 的 MDE80；M-53 的 d64 分辨力门未达到。**

**判定**：样本量检查 **PARTIAL**。函数级 d64 self 的 168 个样本足够给出 1% 量级点估计，但不足以支持“d64 hotspot 高于 d16”或“111 cyc/op 可作 M-53 终判”的结论。

### 2.4 阶段容器 vs 候选 — 报告没有把 stage 当候选，但 stage/家族算术有实质错误

报告的文字纪律是正确的：§0.7/§3.3/§4.3/§5.3/§7 反复说 stage container 不是候选，候选 owner 不能和 stage 相加；§6.4 也说 111 cyc/op 不是可删量。**这一项不应被误判为“把阶段容器当候选”**。问题在于它自己算出的 family/stage 比值与 p2 classifier 的 stage 归属不一致：

**(a) `claim_execution` 的 stage 是 publication，不是“跨 terminal/publication”。**

- `analyze_w3.py:159-165` 的 STAGES 顺序是 `... terminal/release, publication`；publication 的 pattern 包含 `RequestCore::claim_execution`。`attribute_r1_dedicated` → `first_stage` 按 leaf→root 扫描，claim-only 栈先匹配 publication，terminal 只有在栈里同时出现 `offer_terminal`/`release_execution` 时才先匹配。
- 本次独立重算（§1 表 #8）：P-D claim_execution owner 814 个样本 **814/814 全部 stage=publication**；P-E 825/825、P-C 117/118（另 1 个 syscall unattributed）、P-F 53/53。没有任何 claim 样本落在 terminal。
- p2 R1 也明确说 “publication（claim 已在其中）” 和 “claim_execution leaf ... 全部已归 publication”（`P2-COST-MAP-R1.md:489,494`）。
- 报告 `W3-ROWLEVEL.md:230` 却写 `execution（claim/acquire/release_control） | 1.56 | —（跨 terminal/publication） | —`，并在 §3.3 把 publication family 只算 1.00%。这把 1.56% 的 claim owner 从 publication 的分子里拿走了。

修正后的 publication 家族/阶段比（sample count，family union + execution union；两者无重叠）：

| 形态 | publication family | execution (claim) | union | stage publication | 报告值 | 修正值 |
|---|---:|---:|---:|---:|---:|---:|
| P-D | 523 | 814 | 1,337（2.559%） | 1,948（3.73%） | 26.8% | **68.6%** |
| P-C | 620 | 118 | 738（2.059%） | 1,474（4.11%） | 42.1% | **50.1%** |
| P-E | 1,087 | 825 | 1,912（2.756%） | 2,517（3.63%） | 43.2% | **76.0%** |
| P-F | 138 | 53 | 191（1.203%） | 361（2.27%） | — | **52.9%** |

period 加权（owner period / stage period）同向：P-D 65.5%、P-C 51.0%、P-E 78.4%。因此 §5.3 的“publication stage 3.63% 的候选级 owner 1.57%，其余是 callee/kernel/sync”是错的：候选级 owner 是 2.76%，其余只有 0.87pp；§3.3 的“publication 1.00 vs 3.73 = 26.8%”也一样错。

**(b) observation 家族的 76.4/101.5/102.5% 是跨 stage 比值，不是 observation stage 覆盖率。**

- 独立重算的 observer 家族 stage 分布：P-D 725 observation + **286 publication**；P-C 499 + **377 publication**；P-E 882 + **518 publication**；P-F 535 + 69 publication + 8 syscall。
- 报告 §3.3 的 76.4% = family 1,011 / stage 1,323，但 family 里 286 个样本的 stage 是 publication；observation-stage 子集只有 725，覆盖率 725/1,323=**54.8%**。P-C 的 101.5% 对应 observation-only 499/863=**57.8%**；P-E 的 102.5% 对应 882/1,366=**64.6%**。报告把“family/stage 比值”当作“边界差异”解释，但 28–43% 的家族样本在另一个 stage（P-D 286/1,011、P-C 377/876、P-E 518/1,400），不是小边界。
- 这不是“把 stage 当候选”，而是 **family 与 stage 不是同一分区**；在报告没有区分“true stage coverage”和“cross-stage family/stage”的情况下，这些比值不能用于 W3 的“阶段容器里有多少候选”的论证。

**(c) 阶段表被截断，却写成“行和≈100%”。**

- §3.3 开头（`W3-ROWLEVEL.md:205`）说“p2 R1 classifier 在本数据上重算（app/all，行和≈100%）”，但打印的 P-D 表只有 11 行，和是 **94.72%**；缺 `kernel syscall/FS overhead 1.89`、`bench harness 1.49`、`dispatch 0.93`、`backend exec (TP) 0.87`、`poll boundary 0.10`、`submit 0.00`（合计 5.28pp）。P-C 表（§4.3）和是 **90.44%**；缺 `io-wq punt 3.25`、`bench harness 2.47`、`other 2.01`、`scheduler/IRQ 0.84`、`payload/FS 0.81`、`kernel syscall/FS overhead 0.18`（合计 9.56pp）。P-F 表（§6.4）和是 **91.95%**；缺 `other 3.29`、`payload/FS 1.53`、`scheduler/IRQ 1.45`、`syscall unattributed 1.18`、`kernel syscall/FS overhead 0.45`、`shared sync 0.14`（合计 8.04pp）。JSON 的完整 stage rows 确实和是 100.00%（P-D 17 行、P-C 17 行、P-F 16 行），所以不是分类器漏分；是报告的表没有说“只列选中的行”，却给出“行和≈100%”的断言。**读者无法从报告表本身复算 100%。**

**判定**：阶段容器 vs 候选的文字纪律 **PASS**；stage/家族定量对比 **FAIL**（publication/execution、observation 跨 stage、表截断）。这直接影响 W3 的“阶段容器里候选占多少”的论证，必须修正后才能用于准入。

### 2.5 行级份额 vs 可删量 — 报告的文字纪律正确，但 hotspot 标签混用了 sample-count 与 period 加权

报告明确写了“份额 ≠ 可删量”“不是 clean A/B”“不能把 111 cyc/op 当作可删量”（§0/§6.4/§8.1）。**它没有把行级份额直接换算成收益**；这一点应保留。问题在于 `owner%` 的权重：

- 所有 `self_pct_app`/`callsite_extra_pct_app`/`owner_total_pct_app` 都是 **sample-count share**（`analyze_w3.py:600-610`）；报告 §0.8 的阈值 `owner-total ≥1.00% app 且 owner 样本 ≥40` 也是样本口径。报告却没有说“owner% 是样本计数份额”；`owner cyc/op` 是 **period 加权**（`self_cycles/units` 等），两个口径在同一张表里并列（§3.1、§4.2、§5.1、§6.1、§7）。
- 用 JSON 里的 `owner_pct_period_app` 对照，边界候选会翻转：

| 候选 | 形态 | sample owner% | period owner% | 报告标签 |
|---|---|---:|---:|---|
| release_execution | P-D | 1.141 | **0.941** | hotspot |
| release_execution | P-C | 1.052 | **0.811** | hotspot |
| consume_public_result | P-D | 0.976 | **1.291** | below-threshold（边缘） |
| bridge_consume | P-D | 1.018 | 1.346 | 边缘 |
| offer_terminal | P-D | 1.795 | 1.461 | hotspot（两口径都过） |
| claim_execution | P-D | 1.558 | 1.287 | hotspot（两口径都过） |
| wait_one | P-E | 3.438 | 2.924 | hotspot（两口径都过） |
| find_live_router_cookie_ | P-F d64 | 1.128 | 1.131 | hotspot（两口径一致） |

- 因此 `release_execution` 的“hotspot”在 period 加权重只有 0.94/0.81%，`consume_public_result` 的“below-threshold”在 period 加权重是 1.29%——报告的 W3 hotspot 边界不是稳健的。主要候选（offer/claim）两口径都过；M-53 find_cookie 两口径一致。**结论的方向大体成立，但“release_execution 达到 hotspot、consume_public_result 未达到”的二分是口径伪影。**
- `perf report` 的 `Overhead` 是 **period 加权**（P-F flat report 行 552 40 samples 0.28%、553 30/0.21%、557 8/0.06%，对应 run-01 的 79 个 self；W3 的 84/66/18 是两次 run 的 sample count，且 552 含 1 个 stl_vector::size 样本）。报告没有解释 sample-count 与 perf report period-overhead 的差异，读者可能把 0.53% 与 0.28% 看成矛盾。

**判定**：不把行级份额当可删量 **PASS**（文字纪律）；hotspot 的权重口径 **FAIL/PARTIAL**（需要同时给 sample-count 和 period share，或明确阈值只用哪一种）。

### 2.6 d64 热点可复现性 — 聚合可复现，机制/深度推断不可复现

**聚合可复现：**

- 重跑 `analyze_w3.py` 得到逐字节相同的 JSON；d64 self 168、callsite 11、owner 179、lines 84/66/18。
- 独立 raw 解析：run-01 79、run-02 89 self；control 68；两次 d64 run 都接近/高于 1% app self（0.997/1.120），control 0.888。
- `report-flat-self-run-01` 的 line 552 40 / 553 30 / 557 8 / stl_vector 1 与独立解析的 79 self 一致；`report-callgraph-find_cookie-run-01` self 0.56% 是 period 加权（约 0.55% 的 all-comm cycles），与 W3 的 sample-count all 0.50 不同但可解释。

**机制/深度推断不可复现：**

1. **557 行的解释是错的。** 报告 §6.3（`W3-ROWLEVEL.md:425`）说 “557 18（0.11%）（函数 epilogue/return path，DWARF 把 not-found return 映到该行）”。`addr2line`/`objdump` 显示：557 的 18 个样本 = 0x4235f9 的 **成功 `return i`（16 个）** + 0x4235fc 的 `ret`（2 个）；not-found 的 `router_.size()` 返回在 0x423600（**line 556**），该 IP 本次 0 个 self 样本；0x423603/0x423606 虽映射 557，但本次也无样本。也就是说，557 的 18 个样本是 **找到 cookie 的成功返回/收尾**，不是 not-found。这个错误会改变对“扫描是否 miss/命中”的解读。
2. **d64 的 552/553 份额并不比 d16 高。** 报告 §6.3 说 “d64 的循环 init 份额更高（84 vs 39 绝对样本），与更深的扫描一致”。绝对样本 84 vs 39 的差异主要来自 app 样本 15,874 vs 7,654（约 2.07×）；份额是 84/15,874=0.529% vs 39/7,654=0.510%（p=0.845），553 是 0.416% vs 0.340%（p=0.381）。**没有证据说 d64 的循环行份额更高。** 557 的 0.113% vs 0.039%（p=0.074）是唯一接近边缘的差异，但那是成功返回，不是循环体。
3. **d64 vs d16 的函数级差异也不显著**（§2.3）。报告用“1.19×/1.24×”的点估计比，但没有 CI/p；在 C7 的 A/A 噪声下，这个差异不能支撑“d64 深度导致更热”的因果链。
4. **平均扫描深度/占用 NOT_OBSERVABLE**（报告 §8.15）是对的；但没有 instrumentation，无法把 111 cyc/op 与“平均迭代次数”对应。111 cyc/op 是**整个函数存在时的 period 份额**，不是“每次扫描的增量”，更不是可删量。报告 §6.4 已说不能当可删量，但 §6.1/§7 的“hotspot”仍可能被下游当成 d64 深度的证据。

**判定**：d64 的 **聚合/点估计可复现（PASS）**；**d64-vs-d16 深度推断与 557 行解释不可复现（FAIL）**。M-53 的 d64 终判不能建立在这份报告上。

### 2.7 与 P2 阶段容器的关系 — 分类器相同、数据不同；报告的“~1.5pp”不足，且 `__poll` 的 W1 结论没有对齐

- 分类器确实是 p2 R1 的 `attribute_r1_dedicated` 复制（`analyze_w3.py:131-166`），但 W3 是在新的 `perf script --inline` frames 上重算，且增加了 app/all 两列。P-D 的 app=all（无 iou-wrk），所以 P-D 的 stage 可以直接和 P2 R1 all 比；P-C/P-E/P-F 有 iou-wrk，必须用 **all 列** 与 P2 R1 的 all 表比，不能用 app 列。
- 报告 §8.13 说“差异可达 ~1.5pp（inline 帧与 per-thread 采样）”。实际 all-comm 对比：
  - P-C `progress/wait`：W3 19.02 vs P2 R1 16.29 = **+2.73pp**；`submit` 13.41 vs 12.49 = +0.92；`syscall unattributed` 2.72 vs 4.10 = -1.38；`payload/FS` 12.19 vs 11.45 = +0.74。
  - P-E `progress/wait`：W3 26.37 vs P2 R1 24.36 = **+2.01pp**；`kernel syscall/FS overhead` 4.65 vs 5.26 = -0.61；`backend exec` 0.70 vs 1.35 = -0.65。
  - P-D `backend exec (TP)`：W3 0.87 vs P2 R1 2.38 = -1.51；`progress/wait` 5.09 vs 3.51 = +1.58；`bench harness` 1.49 vs 1.99 = -0.50。
  “~1.5pp”对 P-D 大致成立，对 P-C/P-E 不成立；报告应改为“按形态 1.5–2.7pp”，并说明这是 session/inline 帧差异，不是 p2 的逐字节复现。
- **W1/P-E 的关键对齐缺失**：P2 R1 的 W1 指标是 `__poll` inclusive 15.83%（P-E）和 `progress/wait` 24.36%；W3 没有在 P-E 给出 `__poll` inclusive，只给了 `wait_one` inclusive 31.29% 和 `wait_one` owner 3.44%。P2 R1 的 `__poll` 15.83% 是 poll 相关样本；W3 的 `wait_one` 31.29% 包含 poll + mutex + progress pass。报告 §5.2 的“wait_one 的成本主要是它调用的 wait/poll 与 mutex”把两者混在一起（见 §2.8）。与 P2 阶段容器的关系应写成：W3 stage `progress/wait` 26.37% 是容器；P2 `__poll` 15.83% 是其中的 poll 子集；W3 `wait_one` owner 3.44% 主要是 mutex，不是 poll。报告没有这条分层。

**判定**：P2 分类器复制 **PASS**；差异幅度陈述 **PARTIAL**；W1/P-E 的 `__poll` 对齐 **FAIL**（见 §2.8）。

### 2.8 额外发现：W1/P-E 的 `wait_one` owner 是 mutex，不是 poll

这是本次复核中最容易被报告的“§7 跨形态 hotspot”误导的一条：

- P-E app 有 **10,930** 个样本含 libc `__poll` 帧（15.75% of 69,371）。独立重算的 stage：`progress/wait` 7,825、`poll boundary` 3,068、`payload/FS` 37。
- `ctx_wait_one.callsite_leaf_symbols`（`analysis-w3.json`）：`pthread_mutex_lock` 1,273、`mutex_unlock_usercnt` 753、`pthread_mutex_unlock` 30、`__poll` **86**、`read` 16、`steady_clock::now` 15、`errno_location` 13、`pthread_self` 8。callsite 2,194 中 mutex 约 **93.7%**，poll 只有 86（3.9%）；self 191 里还有 63/10 个 `gthr-default.h` mutex 行和 9 个 `progress_source.hpp:437` drain、9 个 `:203` poll。owner 3.44% 是 **mutex 为主**的 candidate-level 成本。
- `progress_wait_unchanged` 同样：callsite 1,328，mutex 858+334+10=1,202（90.5%），`__poll` 86。
- 原因：`__poll` 的栈是 `__poll → __internal_syscall_cancel → __syscall_cancel → wait_if_unchanged (inlined) → wait_one`（P-E raw `script-inline-run-01.txt:1021-1025`）。`callsite-extra` 只看 leaf 的**直接调用者**组；直接调用者是 `__internal_syscall_cancel`，不是 `wait_if_unchanged`/`wait_one`。只有那 86 个 libc wrapper 被内联/缺失的样本，候选才落进 group1。P-D 也一样：227 个 `__poll`，`ctx_wait_one` callsite `__poll` 2、`progress_wait_unchanged` 2。
- 因此：
  - W1 的 poll 相关成本在 **inclusive 31.29%（wait_one，含 mutex/progress pass）/stage 26.37%（含 futex/调度）**，其中 P2 `__poll` 的精确子集是 15.83%；不在 owner 3.44%/2.08%；
  - `W3-ROWLEVEL.md:350` 的 “wait_one 的成本主要是它调用的 wait/poll 与 mutex” 对 **owner** 不成立；owner 主要是 mutex，poll 是 inclusive/stage；
  - §7 的 `AsyncIoContext::wait_one | P-E | 3.44% ... hotspot (line 655 占 1.81% app)` 应改为 “mutex owner 3.44%；poll wait 见 inclusive/stage”，否则 W1 会被错误地按 mutex 行优先级排序；
  - 报告 §5.3 的 “wait_one+wait_if owner 简单相加约 5.5%，实际 union 以 progress family 6.67% 为准”是 **mutex family**，不是 W1 poll；不能把它和 stage 26.37% 当作同一候选的不同视图。

**判定**：这是 W1/P-E 的 **高严重度归因错误**。它不直接影响 W3 RequestCore mutex 候选（这些函数的锁是直接调用），也不直接影响 M-53 find_cookie（用户代码，无 poll），但影响报告作为“W1/W3 统一行级归因”的可用性。

---

## 3. 问题清单

| # | severity | where | issue | evidence |
|---:|---|---|---|---|
| 1 | **high** | `p4/W3-ROWLEVEL.md:399-414,429-444,470`（§6.1/§6.4/§7） | 把 d64 `find_live_router_cookie_` 的 app-point 1.13% 写成 candidate-level hotspot，但 d64 vs d16 不显著，且低于/边缘于匹配 C7 16 对 A/A 的 MDE80/t-hw；M-53 的 d64 分辨力门未达到。 | 本次独立计数：d64 self 168/15,874=1.058%、owner 179/15,874=1.128%；d16 self 68/7,654=0.888%、owner 75/7,654=0.980%。两比例 z 检验 self p=0.2205、owner p=0.3042；Wilson CI 大幅重叠。`p1/AA-CALIBRATION-2.md:222-238,397-408,485-506`：C7 cycles SD=1.889、bootstrap hw=0.893%、t-hw=1.007%、MDE80=1.416%；`p4/M53-OBLIGATIONS.md:313` 要求半宽 ≤ 效应/2。d64 all-comm period self/owner = 0.593%/0.632%（`analysis-w3.json` 的 `self_pct_period_app=1.061`/`owner_pct_period_app=1.131` 乘以 app/all period 比 9,800.8/17,545.6）。 |
| 2 | **high** | `p4/W3-ROWLEVEL.md:61-77,141-151,422-427,479-500`（§1.4/§2.3/§6.3/§8） | 没有 skid/PEBS 分析；`perf record` 无 `precise_ip`，却把 552/553/557 的 84/66/18 行级拆分当作机制证据；P2 M5 的“无 PEBS/skid”限制没有继承。 | raw `P-F-.../raw/header-run-01.txt`：`sample_type = IP|TID|TIME|CALLCHAIN|PERIOD|REGS_USER|STACK_USER`，无 precise_ip；`p4/W3-ROWLEVEL.md` 全文无 skid/PEBS。`P2-COST-MAP-R1.md:61,511,642` 明确把无 PEBS/skid 作为 M-53 置信降级原因。`addr2line`/`objdump` 显示 552/553 地址相邻（0x4235e0–0x4235f7），skid 可跨行。 |
| 3 | **high** | `p4/W3-ROWLEVEL.md:227-230,287-314,361-391`（§3.3/§4.3/§5.3） | 把 `claim_execution` 记为 `execution` 家族、stage 写“—（跨 terminal/publication）”，并从 publication family/stage 比值中排除它；报告自己的 p2 classifier 把 claim 归 publication，实际 publication 候选占比是 68.6/50.1/76.0%，不是 26.8/42.1/43.2%。 | `analyze_w3.py:159-165` STAGES publication 含 `RequestCore::claim_execution`；`P2-COST-MAP-R1.md:489,494` 明说 claim 已在 publication。本次独立重算（导入 analyze_w3.py 的 classifier）：P-D claim 814/814 publication；P-E 825/825；P-C 117/118；P-F 53/53。publication family ∪ execution：P-D 1,337/1,948=68.6%、P-C 738/1,474=50.1%、P-E 1,912/2,517=76.0%、P-F 191/361=52.9%；period 加权 65.5/51.0/78.4%。 |
| 4 | **high** | `p4/W3-ROWLEVEL.md:325-359,464-466`（§5.1/§5.2/§7） | P-E `wait_one`/`wait_if_unchanged` 的 owner 主要是 mutex（~90–94%），不是 poll；10,930 个 libc `__poll` 样本中只有 86 个进入 `wait_one` callsite；W1 的 poll 相关成本在 inclusive 31.29%/stage 26.37%，不在 owner 3.44%/2.08%。 | `analysis-w3.json` `cases.P-E.candidates.ctx_wait_one.callsite_leaf_symbols`：mutex 1,273+753+30、`__poll` 86；`progress_wait_unchanged`：mutex 1,202、`__poll` 86。本次独立解析 P-E app：10,930 个 `__poll`，stage progress/wait 7,825 + poll boundary 3,068 + payload 37；栈为 `__poll → __internal_syscall_cancel → __syscall_cancel → wait_if_unchanged → wait_one`（raw `script-inline-run-01.txt:1021-1025`），直接调用者规则漏掉 poll。P-D 227 个 `__poll`，`wait_one` callsite `__poll` 仅 2。 |
| 5 | **medium** | `p4/W3-ROWLEVEL.md:166-186,259-276,448-471`（§3.1/§4.2/§7） | `owner%` 是 sample-count share，`owner cyc/op` 是 period 加权；报告没有说明，且边界候选在 period 口径下翻转：release_execution P-D 1.141→0.941、P-C 1.052→0.811；consume_public_result P-D 0.976→1.291。 | `analysis-w3.json` 的 `owner_pct_period_app`：release_execution P-D 0.941、P-C 0.811；consume_public_result P-D 1.291；offer_terminal 1.461、claim 1.287、wait_one 2.924、find_cookie 1.131。`analyze_w3.py:600-610` 的 `owner_total_pct_app` 是计数；报告 §0.8 阈值也是计数。 |
| 6 | **medium** | `p4/W3-ROWLEVEL.md:227-228,310,388`（§3.3/§4.3/§5.3） | observation family 的 76.4/101.5/102.5% 是跨 stage 比值，不是 observation stage 覆盖率；observer 家族有 286/377/518 个样本在 publication stage。 | 本次独立重算：P-D observer 725 observation + 286 publication（observation-only 725/1,323=54.8%）；P-C 499+377（499/863=57.8%）；P-E 882+518（882/1,366=64.6%）；P-F 535+69+8。`analysis-w3.json` family counts：P-D observer 1,011、P-C 876、P-E 1,400。 |
| 7 | **medium** | `p4/W3-ROWLEVEL.md:203-232,287-302,429-443`（§3.3/§4.3/§6.4） | 阶段表被截断，却写“行和≈100%”：P-D 表 94.72%、P-C 90.44%、P-F 91.95%；完整 JSON rows 的和是 100%。 | 本次复算 JSON stage rows：P-D 17 行和 100.00、P-C 17 行和 100.00、P-F 16 行和 100.00；打印表缺 P-D 的 kernel syscall/FS overhead 1.89、bench harness 1.49、dispatch 0.93、backend exec 0.87、poll boundary 0.10、submit 0.00（5.28pp）；P-C 缺 9.56pp；P-F 缺 8.04pp。 |
| 8 | **medium** | `p4/W3-ROWLEVEL.md:425`（§6.3） | d64 line 557 的 18 个 self 样本被解释为 “not-found return”；实际是成功 `return i` 的 16 个 + `ret` 的 2 个，not-found return 在 line 556 且 0 样本。 | `addr2line -e ... -f -C -i -a 0x4235f9 0x4235fc 0x423600 0x423603 0x423606`：0x4235f9/0x4235fc→557；0x423600→556；0x423603/0x423606→557。`objdump -d --start-address=0x4235a0 --stop-address=0x423610`：0x4235f9 是 `mov %rdx,%rax; ret`（成功 return i），0x423600 是 size return。IP 计数：557 = 16（0x4235f9）+2（0x4235fc）。 |
| 9 | **medium** | `p4/W3-ROWLEVEL.md:19,448-473`（§0.8/§7） | `candidate-level measured hotspot` 的阈值只应用于精选候选列表，但没有说明筛选规则；P-D `tp_worker_loop` 6.72%（period 5.70）、P-C `ur_dispatch_one` 1.98、P-F `scope_wait_for` 3.02/`scope_take` 2.65/`bench_pipeline_phase` 3.01 都过 1% 但不在 §7 表。 | `analysis-w3.json` `cases.*.candidates`：P-D tp_worker_loop owner 3,510=6.719%（period 5.702）；P-C ur_dispatch_one 709=1.978%；P-F scope_wait_for 480=3.024%、scope_take 421=2.652%、bench_pipeline_phase 478=3.011%。§7 只列 find_cookie/dispatch_one 等子集，未说“W3/M-53 候选集”。 |
| 10 | **medium** | `p4/W3-ROWLEVEL.md:14,275,427`（§0.3/§4.2/§6.3） | `callsite-extra` 的“非 binary leaf”包含 IRQ/syscall boundary（`__irqentry_text_end`、`entry_SYSCALL_64`、`error_entry`、`asm_sysvec_reschedule_ipi`），不是锁/等待成本；find_cookie 的 11 个 callsite 全部是这类边界，dispatch_one 的 149 个 callsite 也是 115 IRQ+27 syscall 等。 | `analysis-w3.json`：P-F ur_find_cookie callsite 11 = `__irqentry_text_end` 8 + `entry_SYSCALL_64` 2 + `error_entry` 1；P-C ur_dispatch_one callsite 149 = IRQ 115 + entry_SYSCALL 27 + IPI 5 + error 2；P-F ur_dispatch_one 69 = IRQ 54 + entry_SYSCALL 14 + IPI 1。报告 §0.3 说 callsite-extra 是“锁/等待成本”。 |
| 11 | **medium** | `p4/W3-ROWLEVEL.md:155-157,487`（§2.4/§8.7） | 用户栈 `sample_stack_user=8192`/`exclude_callchain_user=1` 未讨论；报告只列 single-group，depth≤4 的样本为 4.17/1.88/4.25/0.83%，inclusive 可能是下界。 | raw `P-*/raw/header-run-01.txt` 的 `sample_stack_user = 8192`、`exclude_callchain_user = 1`；`analysis-w3.json` `app_depth_hist`：P-D depth≤4 2,176/52,243、P-C 673/35,836、P-E 2,950/69,371、P-F 132/15,874；报告 §2.4 只列 single-group 522/249/655/77。 |
| 12 | **medium** | `p4/W3-ROWLEVEL.md:12,141-151,190-201`（§0.1/§2.3/§3.2） | `self` 的定义写“候选函数自己的非内联代码”，但实际包含候选体内联的 callee/锁：claim_execution self 223 中 158 个实际指令在 `resolve_internal_:127`；offer_terminal 41、begin_publication 35 同类；报告只对 claim 给了 inner line。 | `analysis-w3.json`：P-D claim_execution `self_inner` `request_core.cpp:127` 158；offer_terminal `self_inner` 127 41；begin_publication 127 35；P-E claim_execution 127 116。报告 §3.2 只对 claim 写“self 主要不是 claim 逻辑”。 |
| 13 | **low** | `p4/W3-ROWLEVEL.md:493`（§8.13） | 说 stage 重算差异“可达 ~1.5pp”，实际 P-C progress/wait all +2.73pp、P-E +2.01pp；应写 1.5–2.7pp，并说明是 session/inline 差异。 | `analysis-w3.json` vs `p2/P2-COST-MAP-R1.md:233`：P-C progress/wait 19.02 vs 16.29（+2.73）；P-E 26.37 vs 24.36（+2.01）；P-D backend exec 0.87 vs 2.38（-1.51）。 |
| 14 | **low** | `p4/W3-ROWLEVEL.md:94-100`（§1.5） | P-E 表把 `all` 写成 69,371，但 raw 总样本 70,476；1,105 个 `rm`/`sh`/`taskset` 样本被排除，报告未说明“all=app+iou-wrk，不含 other comm”。 | `analysis-w3.json` P-E `samples_total=70,476`、`samples_app=69,371`、`samples_other_comm=1,105`；`comm_counts` = `rm 976 + sh 104 + taskset 25`。 |

**计数说明**：上表 14 条中 4 条 high、8 条 medium、2 条 low。high 的 1/2/3/4 分别对应 M-53 分辨力、skid、publication/execution stage 映射、W1/P-E wait owner；这四条都是“不能按原文用于终判”的问题。

---

## 4. 已确认正确的部分（应保留）

1. **p4 DWARF 构建与 P1 release 代码一致**：`.text`/`.rodata`/`.data` 逐字节相同（`p4/logs/text-compare-vs-p1.log`），sha256 与报告 §1.1 一致，`file` 显示 `with debug_info, not stripped`；这使行级归因对 P1 release 代码有代表性。
2. **分析器可复现**：`analyze_w3.py` 重跑得到逐字节相同 JSON；`SHA256SUMS` 155/155 OK。这是本报告最强的证据链。
3. **DWARF/inline 解析方向正确**：`addr2line` 最内层/最外层、perf script group、`real0`、candidate-own vs actual line 的实现与报告 §2.3 一致；候选 self 行无 `??:0`。
4. **行级锁行映射正确**：request_core.cpp 的 348/365、368/378、394/409、439/460、463/484、325/344，threadpool_backend.cpp 的 561/584、607/609，progress_source.hpp 的 203/205/166/230，async_io_context.cpp 的 655/609/595/696 与源码/反汇编一致；callsite leaf 的 mutex/unlock 计数与报告表一致。
5. **报告的文字纪律**：没有把 stage container 直接叫候选，没有把行级份额直接换算成可删 cycles；§6.4 明确说 111 cyc/op 不是可删量；§8 的限制列表（period 偏差、iou-wrk 分母、P-E block IO、单帧、inline 多义、内核/libc 行级不可得、M-53 平均深度不可观测）基本成立。
6. **P-E 介质限制披露正确**：block rq 202/202 vs idle 8/10、fincore 256M/65,536 pages；P-E baseline 0.824s/copy vs p2 0.398s/copy 的不可比性也被披露。

---

## 5. 未执行 / 不可观测

1. **未重跑 perf/bench/AA/AB**：本复核没有新采集，不能证明份额在低扰动/重复 session 下的稳定性（P2 M7 同样未证）。
2. **没有 PEBS/`precise_ip`/skid 实验**：无法给出 line-level skid 误差；552/553/557 的行级份额只能算“采样 IP 归属”，不是精确退休指令归属。
3. **内核/libc/libstdc++ 行级不可得**：内核叶子只有函数符号；libc/libstdc++ 无本机 debuginfo；callsite 行来自用户态 caller IP 的 DWARF，不是被调 DSO 的行。
4. **没有逐请求因果链**：报告 §8.18 已说；本复核也未做 submit→complete join。
5. **没有 d64 平均扫描深度/in_use instrumentation**：报告 §8.15 已说；111 cyc/op 与“每次扫描增量”不可对应。
6. **没有真实消费者 profile**：P-E 是参考应用，不是 G0；本复核不改变 `CAMPAIGN-LEDGER.md` 的 profile 门。
7. **没有验证 H1 patch/迁移义务**：`p4/M53-OBLIGATIONS.md` 的 O3 仍 INCONCLUSIVE；本复核只评 W3 报告，不重开 M-53 迁移义务。

---

## 6. Verdict

**报告是否可发布**：**可以作为 W3 方向性行级 profile 记录发布，但必须带勘误/限制；不能作为 W3 定量准入或 M-53 终判依据。**

**归因可用于 W3/M-53 终判吗：**

- **W3 RequestCore mutex 候选（offer_terminal / claim_execution / release_execution / consume_public_result / bridge_consume）**：函数的锁/解锁调用点与行号可复现，样本量足够（P-D 814–938、P-C 465–632、P-E 575–825 owner 样本）；可作为“这些方法在哪些行取/放 mutex、在哪个形态由 worker/main/driver 承担”的**方向性/机制性证据**。但不能直接用于“达到 1% 收益门/可以进入 A/B”的准入：hotspot 边界在 sample-count 与 period 加权下翻转（release_execution 0.94/0.81、consume_public_result 1.29），且 publication/observation 的 stage 对比算错。若 W3 准入只要求“measured 行级归因”，本报告在 **RequestCore mutex 行** 上部分满足；若要求候选排序/阶段分离/收益门，则不满足。
- **M-53**：**不可用于终判。** d64 点估计可复现，但：
  1. d64 vs d16 不显著（self p=0.22、owner p=0.30）；
  2. 匹配 C7 16 对 A/A 的 t-hw 1.007%、MDE80 1.416%，而 all-comm period self/owner 只有 0.593%/0.632%（app owner 1.128% 也不是 A/A 的 process-cycles 口径）；按 M-53 “半宽 ≤ 效应/2”门（`M53-OBLIGATIONS.md:313`），16 对远不够（δ=1% 需约 58 对；`M53-OBLIGATIONS.md:295-304`）；
  3. 没有 skid/PEBS；552/553 行级拆分不是精确指令级证据；
  4. line 557 被误读为 not-found return；
  5. 平均扫描深度/占用 NOT_OBSERVABLE。
  M-53 应维持 **INCONCLUSIVE**（`CAMPAIGN-LEDGER.md:211`；`M53-OBLIGATIONS.md:10,267`）。本报告最多把 d64 从“无数据”提升为“有一个约 1.1% app / 0.6% all 的可复现点估计”，不足以打开 d64 A/A/A-B 资格。

**一句话**：**W3 的 mutex 行级地图成立，W3 的 stage 容器算术和 W1/P-E 的 poll 归因不成立，M-53 的 d64 hotspot 终判不成立。** 报告应修订后再发布；修订重点见问题清单 #1–#4。

---

*复核结束。所有本会话命令、输出和对比均在上文 §1 列出；未执行项在 §5 列出。*
