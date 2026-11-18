# P3-ADMISSION-REVIEW — 独立对抗复核

| 字段 | 值 |
|---|---|
| 角色 | 独立准入复核员（只读证据；只写本文件；不修改产品代码） |
| 日期 | 2026-11-18 |
| 复核对象 | `p3/P3-ADMISSION.md`（403 行） |
| 输入 | `CAMPAIGN-LEDGER.md` §4/§5/§9；`p2/P2-COST-MAP-R1.md`；`p3/W1-DESIGN.md`；`p1/AA-CALIBRATION.md`；`p1/AA-CALIBRATION-2.md`；`p2/P2-REVIEW.md` |
| 方法 | 逐条核对 P3 的引用、状态词汇与算术；独立运行 P2 R1 与 P1/P2 A/A 复算脚本；用复算 JSON 的 `aa.matching` 检查 A/A 格匹配；按 ledger §9.2 的处置词汇检查裁决状态 |
| 未运行 | `xmake` 构建/测试、任何新的 perf/bench/A/A/A/B、TLC、sanitizer、#637 归档（`e2f41acb`）的原始回执 |

> **总 verdict：不能按现状整体用于后续施工。** `ADMIT_TO_AB = 0` 的结论本身成立；W1 的语义阻断、M-42 的 `NOT_POWERED`、W2/W5 的 `NOT_APPLICABLE` 也基本成立。但 **W3 的 A/A/预算结论、M-62 的 4.13% "上界"、M-28 的状态与重开条件** 必须先修正（见 §3 的 H1–H3）；M-53 的 17–20 对预算需按 δ 重新表述。修正后，`ADMIT_TO_DESIGN` 行可作为设计/资格工作起点，但不能按 §4.2 的 "36 对足够" 或 M-53 "17–20" 直接排 A/B。

---

## 0. 一句话结论与计数复核

P3 的 38 行计数和主裁决表内部自洽：

- 38 = W1–W6 包 6 + W1 方向 7 + W3 个体 5 + W4/W6 个体 5 + #637 其余 15（`P3-ADMISSION.md:29`）；
- 主裁决计数 0+11+12+2+3+2+4+4 = 38（`P3-ADMISSION.md:31-40`）。

计数本身没有算术问题。但 **`NOT_APPLICABLE` 3 里包含 M-28**，而 M-28 的 ledger 原终态是 `NO_GO_ABORT`（`CAMPAIGN-LEDGER.md:196,207`）；这不是计数错误，而是状态词汇错误（见 H3）。

---

## 1. 本轮实际执行的检查

| 检查 | 命令 | 结果 |
|---|---|---|
| P1 A/A 复算 | `cd docs/evidence/621-progress-wait-path-20261118 && python3 -I tools/recompute_independent.py --aa-root p1/aa --json-out /tmp/review-aa1.json` | **exit 0**；C1 CPU SD 0.786/hw 0.365/MDE 0.589、C1 cycles 2.346/1.121/1.758、C4 CPU 3.019/1.402/2.262、C6 CPU 0.997/0.471/0.747、C6 cycles 2.876/1.368/2.155；C6 wall 位置 +1.132pp（p=0.0049），与 P3 §1.3、`AA-CALIBRATION.md` 一致 |
| P2 A/A 复算 | `python3 -I tools/recompute_independent.py --aa-root p1/aa2 --json-out /tmp/review-aa2.json` | **exit 0**；C2b CPU 3.414（NOISY）、C3b cycles 1.319/0.621/0.988、C5b CPU 0.964/0.458/0.723、C7 cycles 1.889/0.893/1.416（t-hw 1.0066）、C8 CPU 4.938（NOISY），与 P3 §1.3、`AA-CALIBRATION-2.md` 一致 |
| P2 R1 复算 | `cd p2/profile/analysis && python3 -I recompute_p2_r1.py --json-out /tmp/review-p2r1.json` | **exit 0**；P-B `__poll` 6,129/24,585=24.93%、poll boundary 2,251=9.16%、progress/wait 6,168=25.09%；P-E 5,500/34,736=15.83%、1,520=4.38%、8,461=24.36%；P-D publication 627=4.13%、terminal 530=3.49%；W3 family 502/59/2,606/3,405/4,539；M-53 34/47；P-B app 12,246/11,104/1,142、wait_one 700、poll_without_wait_one 5,664、four_frame_poll 873，与 P3 §7.1 一致 |
| required-n 算术 | 用 `tools/recompute_independent.py` 的同一 t 公式逐项复算 | C6 cycles 1.2%=48/91、2%=19/35、1%=67/130；C1 cycles 32/62；C5b CPU 8/13；C3b cycles 12/22；C7 cycles 22/41、2%=10/17、1%=30/58；C4 CPU 52/100、2%=20/38；C6 CPU 8/14；36 对 MDE/hw 均与 P3 §1.4 一致 |
| M-42 指令 | `objdump -d --start-address=0x424190 --stop-address=0x424230 /tmp/p2-621/symtwin/bench_w02_pipeline.sym` | `4241b7: f0 48 83 87 20 01 00  lock addq $0x1,0x120(%rdi)`；与 P3 §4.4 一致 |
| 源码坐标 | `sed -n '214,236p' src/async/uring_transport.cpp`；`sed -n '68,82p' src/async/uring_test_seams.hpp`；`sed -n '318,330p' include/sluice/async/detail/uring_backend.hpp`；`sed -n '68,90p'`/`:104,232p`/`:404,465p` `include/sluice/async/detail/progress_source.hpp`；`sed -n '573,705p' src/async/async_io_context.cpp`；`grep -n 'owed_count_\|any_event_owed_locked_' src/async/threadpool_backend.cpp` | `fetch_add` 在 `uring_transport.cpp:222`；成员 `submit_flushes_` 在 `uring_backend.hpp:324`；唯一读者 `uring_test_seams.hpp:75`；`eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC)` 在 `progress_source.hpp:75`；`wait_one` 两次 `access_mtx_` 在 `async_io_context.cpp:581-593`；`owed_count_` 增量 `threadpool_backend.cpp:350`、权威扫描 `:847` |
| 合约 | `sed -n '865,950p' docs/explicit-io-contract.md` | PROG-02 允许 "condition variable with a protected predicate"（`:907-912`）；PROG-02 第 2 条、PROG-03 的 fd 借用与非第二 drain authority、PROG-04 的 control sticky 均与 P3/W1-DESIGN 引述一致 |
| A/A 匹配表 | `python3 -I` 读取 `/tmp/review-p2r1.json` 的 `aa.matching` | **C5b = "not P-D (d16) and not P-B (uring)"、verdict "QUIET conditional; not a matching cell"**；**C2b（P-D 的匹配格）= "NOISY (no usable band)"**（见 H1） |
| 未运行 | — | 任何 `xmake f/build/test`、新的 perf/record/stat/eBPF/uprobe/strace、A/A 或 A/B、TLC、sanitizer；没有打开 `#637` 归档 `e2f41acb` 的原始回执（R3/M-28 历史数字只按 ledger 转述核对） |

---

## 2. 逐项检查结果（对应任务要求的七项）

### 2.1 是否把 profile 上界当效应

**大部分没有。** P3 明确写了 "份额 ≠ 可删 cycles、阶段容器 ≠ 候选上界、inclusive 不能相加/不能当收益"（`P3-ADMISSION.md:54`），并在 §1.2（`:77-84`）、§4.1（`:214-222`）、§8（`:362-373`）反复限定。W1 的 `__poll` 24.93%/15.83% 没有被当作收益；W3 的 13.07–22.43% 被明确称为族级容器；M-42 被限定为算术上界。

**但有两处反例：**

1. **`P3-ADMISSION.md:229` 写 "M-62 的 4.13% 是上界"**，而同一文件 `:84` 明确 "不接受阶段容器…不能作准入上界"，`:232` 又写 "阶段容器不是候选上界"。这是直接自相矛盾，也是 P2-REVIEW M2/M3 已经指出的同一类错误（publication 是阶段容器，不是候选锁成本的上界）。见 H2。
2. **`P3-ADMISSION.md:240` 写 "0.4% 的效应在 36 对 C3b cycles 上 MDE80≈0.633%…"**。0.406%(P-A)/0.285%(P-C) 是 `find_live_router_cookie_` 的 leaf self 份额，不是 A/B 效应；把它称为 "0.4% 的效应"，再用 P-A 的 C3b 格去覆盖 P-C 的 0.285%，属于把 profile 上界/份额当效应、并跨形态使用匹配格。见 M1。

### 2.2 是否遗漏语义义务

W1 侧的 O1–O10 在 `W1-DESIGN.md:119-134` 完整列出，P3 的 W1 结论没有漏掉关键语义（D2 的 PROG-02 第 2 条、有界 timeout、ack 非阻塞；D3 的单 eventfd 注册；C1 的第二 drain authority）。M-53 的三项迁移义务也完整（`P3-ADMISSION.md:173,244`）。

**遗漏集中在 W3/W6 的重开门槛：**

- **W3 ledger 门禁要求任何跨方法临界区合并证明 `publication_inflight`/slot reuse/observer 注册不被改变（`CAMPAIGN-LEDGER.md:259`）**。P3 只在 M-61 行写了 `publication_inflight`（`:163`）；M-62 只写 "publication exactly-once/observer 注册"（`:164`），没有 slot reuse/publication_inflight；M-65 只写 "第三锁与 retire 融合的步间语义；交付顺序不变性"（`:165`）；M-10 只写 "步间语义映射"（`:167`）。见 M2。
- **M-61 的重开条件**：ledger 要求 "含 `publication_inflight` 反例与**确定性测试注入点**" 以及 "保步间语义的融合单因素 A/B（**族 A/A 先行**）"（`CAMPAIGN-LEDGER.md:222`）；P3 只写 "行级分离 + `publication_inflight` 反例处理" 和 "融合的步间语义"（`:163`），丢了确定性注入点和族 A/A 前置。见 M3。
- **M-42 的未来门禁**：ledger 的 W6 门要求 "无生产读者、机制门七问、动态回执带 + 隔离格判据、**mutant kill**、**release 无 seam 符号**"（`CAMPAIGN-LEDGER.md:262`）；P3 的 M-42 行只写 "measured hotspot；隔离格 + 动态回执带 + 机制门七问"（`:176`），`§4.4` 也只说唯一读者在测试宏内，没有 mutant/release-seam 证明和 "出现生产读者 → NO_GO/重新物化" 的后果。见 M4。
- **M-65 的归属/预算**：P3 §3.2 把 M-65 放在 "W3 行级候选"，§2 看板与 §6.2 stage 4 又把它放在 W4/T9；ledger 本身在 W3 行（`:259`）和 W4 行（`:260`）都提到 M-65。矩阵没有给 M-65 单一包归属，预算/阶段会冲突。见 M7。

**W1-C2 的语义定性**是另一个方向的问题：PROG-02 明确允许 condvar + protected predicate（`docs/explicit-io-contract.md:907-912`），W1-DESIGN 对 C2 的判词是 `NEEDS_OWNER`（`W1-DESIGN.md:256`），P3 却把它计入 `SEMANTIC_MISMATCH`（`P3-ADMISSION.md:153,210`）。虽然有 "（owner 未裁决）" 的括号，但 "SEMANTIC_MISMATCH" 的词汇含义是 "与 root/契约语义冲突，或需要 owner 先裁决"（`CAMPAIGN-LEDGER.md:511`），把契约允许的 C2 与 D/C1 的硬冲突混在同一计数里，会让读者以为 W1 的唯一 material 方向在语义上都不成立。见 M6。

### 2.3 是否用了错误 A/A 格

- **W1：C6/C1 正确。** P2-REVIEW H3 明确 P-B→C6、P-E→C1；P3 在 §4.1 也明确 "不是 C5b/C3b/C7"（`:222`）。这部分是 P3 相对 R0 的改进，且与我的复算一致。
- **M-42：C6/C3b 正确。** M-42 是 uring-only；P3 用 C6（d1）和 C3b（d16）的 MDE 证明 0.066–0.133% 低于噪声，没有用 C5b（TP）。正确。
- **M-53：C7 用于 d64 正确；C3b 用于 P-A d16 正确。** P-C d16 的匹配格是 C4（cycles MDE 6.088%，CPU 2.262%），P3 在 §4.3 用 C3b 的 0.633% 去覆盖 P-C 的 0.285%，格不匹配；但两者结论都是 "低于分辨率"，方向不变。见 M1。
- **W3：C5b 不是匹配格。** P3 §1.3 把 C5b 的 "本战役用途" 写成 "W3 TP d1 w1"（`:98`），§4.2 用 "C5b CPU 2% 需 5/7 对" 支撑 "36 对预算对 2% 足够"（`:236`）。但：
  - C5b 是 `W02-threadpool-4k-d1-w1`；P2 没有任何 W3 形态在 TP d1 w1 上采集；P2 的 W3 形态是 P-D（TP d16 w4，22.43%）、P-C（W-03 d16，15.82%）、P-E（copy，13.07%）。
  - P2 R1 自己的 `aa.matching` 把 C5b 标为 **"not P-D (d16) and not P-B (uring)"、"QUIET conditional; not a matching cell"**（我用 `/tmp/review-p2r1.json` 复核）；把 C2b 标为 **"P-D"、"NOISY (no usable band)"**。
  - P2-REVIEW H3 明确禁止把 C3b/C5b/C7 的带搬到 P-B（`P2-REVIEW.md:84,92`）；AA-CALIBRATION-2 §5 规则 1/4 明确 C5b 的带只适用于 C5b，不能搬到 d16/d64 或其他后端（`AA-CALIBRATION-2.md:413,416`）。
  - C5b 本身也只是 "QUIET（条件基线）"：runner 报 `background_load_exceeds_measured_cpus`（`AA-CALIBRATION-2.md:220,284-285`），即使把它当 TP d1 w1 的格，正式 A/B 前也要复跑 A/A。
  
  因此 P3 的 "A/A 分辨率不是瓶颈，36 对预算对 2% 足够" 对 W3 不成立：W3 的最大形态 P-D 根本没有可用 A/A（C2b NOISY），而 C5b 不是 P-D 的匹配格。这是 **H1**。

### 2.4 是否违反历史反例

- **M-28（NO_GO_ABORT）：违反。** P3 把 M-28 的 P3 裁决写成 `NOT_APPLICABLE（不重做）`（`:177`），§4.6 也写 `NOT_APPLICABLE（不重做）`（`:271`），§2 看板又写 `NO_REDO`（`:139`）；`NOT_APPLICABLE` 在 ledger §9.2 的定义是 "候选在 current BASE 无适用对象（已被合并/删除/非本战役范围）"（`CAMPAIGN-LEDGER.md:512`）。M-28 的 `syscall_count_` 仍在生产（`src/async/threadpool_backend.cpp:434`、`include/sluice/async/threadpool_backend.hpp:258`），不是 "无适用对象"；正确状态应保持 `NO_GO_ABORT`（或至少 `NO_REDO` 作为优先级标签，而不是裁决）。此外，P3 的 "翻案三条件"（`:177`）和 §4.6（`:271`）只列了 "隔离 TU/对象布局变化、RMW 移除的锁竞争时序、新 session/新冻结 + 动态回执接受带"，**漏掉 ledger §4.1 的第 1 条 "新证据把机制约束为可分离的'单指令差'"**（`CAMPAIGN-LEDGER.md:237`）。这会把 M-28 的重开门槛降得比 ledger 更低。见 **H3**。
- **M-61 R3（0.45% < 1% MME 拒绝）：基本遵守。** P3 在 §1.5（`:124`）、§3.2（`:163`）、§4.2（`:234`）都保留了 R3 的候选级拒绝，并没有用 publication 4.13% 容器推翻它。但 M-61 的重开条件漏了 "确定性测试注入点" 和 "族 A/A 先行"（见 M3）。
- **M-62：部分违反。** P3 的 M-62 行本身说 "P-D publication 4.13% 容器"（`:164`），这是正确的；但 §4.2（`:229`）又说 "M-62 的 4.13% 是上界"，这正是 P2-REVIEW M2/M3 指出的 "阶段容器冒充候选上界" 错误。见 H2。
- **H1：遵守。** P3 保持 `NO_GO`，明确 "P2 只是复现现状…不是 H1 的 A/B 证据"（`:195,273`），没有因 P2 的 submit 份额大而复活 H1。正确。
- **M-53 三项义务：遵守。** P3 完整列出 TransportLedger 物理位置单调、控制 CQE exact-submitted、cancel 定向（`:173,244`），并明确三项未证。正确。
- **M-42 隔离格：基本遵守。** P3 保持 `NOT_POWERED`，要求隔离格 + 动态回执带 + 机制门七问（`:176,254`），并明确 `GO` 只是排队。但它漏了 mutant kill/release no-seam/生产读者后果（见 M4）。

### 2.5 预算是否与实测噪声带一致

**算术一致。** 我用同一 t 公式复算了 P3 §1.4 的所有行，C6/C1/C5b/C3b/C7/C4 的 n80/n_half 与 36 对 MDE/hw 都与 P3 一致；C7 cycles 的 t-hw=1.0066% 与 P3 的 1.007% 一致。

**但预算的 "效应" 假设与实测证据不一致：**

- P3 §1.4 的 required-n 是 "若效应是 δ" 的条件数，本身正确；问题在 §4.2/§4.3 把这些数当成了 "分辨率足够" 的结论。
- **M-53**：17–20 对只在 δ=2% 时满足 C7 cycles 的 hw≤1%；ledger 的 M-53 判据是 "d64 A/A 半宽 ≤ 效应/2"（`CAMPAIGN-LEDGER.md:211`）。P3 没有说明 δ=2% 是计划值，也没有说明如果 d64 效应接近已测 d16 上界 0.406%/0.285%，C7 cycles 的 hw≤effect/2 需要约 172（80%）/336（hw）对，而不是 17–20。P3 还允许 δ=1% 的 A/B 58 对，却把 A/A 固定为 17–20；若走 δ=1%，A/A 的 hw≈0.97% > 0.5%，不满足 "半宽 ≤ 效应/2"。见 **M1**。
- **W3**：C5b 的 0.964 SD 是 C5b 自己的；P-D 的 C2b SD 3.414（NOISY），P-C 的 C4 CPU SD 3.019（2% 需 20/38），P-E 的 C1 CPU 2% 需 4/5。P3 的 "36 对预算对 2% 足够" 只对 C5b/C3b/C7 成立，对 P-D 根本不成立（无带），对 C4 的 hw≤1% 也不成立（需 38）。见 **H1**。
- C5b 的背景负载警告、C3b/C7 的条件性 cycles 带、C6 的 wall 位置效应，P3 都正确标注了（`:92-103,311`）。这部分没有问题。

### 2.6 是否把 owner 裁决点伪装成技术结论

**§5 的 O-1–O-12 是 P3 做得最好的部分之一。** P3 明确 "owner 裁决不是技术结论"（`:57`），把 W1 的 profile 门/拓扑、W3 的 δ、M-53 的 cycles、M-42 的隔离格、语义冲突批、M-01 的 free-list、M-28 的重开、W2/W5 都列为 owner 点。M-42 的 "GO 只是排队、启动属 owner"（`:254`）也正确。

**但有两个残留：**

1. **W1-C2**：契约允许，W1-DESIGN 说 `NEEDS_OWNER`；P3 把它标成 `SEMANTIC_MISMATCH` 并计入 12 个语义冲突（`:153,210`）。虽然有 "（owner 未裁决）" 的括号，但这把 "owner 尚未授权第二通知通道" 写成了 "语义不匹配"，与 `W1-DESIGN.md:256` 对 C2 的 `NEEDS_OWNER` 判词不一致。见 M6。
2. **W1 的 "终判"**：P3 §2 看板把 W1 标为 "**终判**"（`:134`），但 O-1/O-2/O-3 仍开放（`:290-292`），O-2 可以改变 W1 的 C2/D 结论。P3 在 `:141` 加了 "但 W1 的 owner 裁决点仍开放" 的括号，但 "终判" 与 "owner 点开放" 仍是矛盾的状态标签。见 L1。
3. **O-5 的 17–20**：把 "接受 cycles + 17–20 对" 作为 owner 选项，但没有把 "17–20 只对应 δ=2%；δ=1% 需 58 对" 写进选项；O-10 又写 "确认 NO_GO_ABORT" 与 §3.3 的 `NOT_APPLICABLE` 冲突。见 H3/M1。

### 2.7 W2/W5 的 NOT_APPLICABLE 是否成立

**成立（按 ledger 的原文）。**

- W2：`CAMPAIGN-LEDGER.md:258` 原文是 "NOT_APPLICABLE（本战役无新增 W2 候选）"；P2 的 `eventfd_signal` union 8.672% 是内核侧 `io_eventfd_signal`，不是 W2 的 ProgressSource 机制；M-67 sticky flags 按 ledger §5.2（`:271`）被路由到 W3。P3 的 W2 行（`:135,279`）与 ledger 一致。
- W5：`CAMPAIGN-LEDGER.md:261` 原文是 "NOT_APPLICABLE（无新增 W5 候选）"；#634 已合并，`tests/threadpool_owed_counter_parity_test.cpp` 与 `xmake/tests.lua:527-537` 的 4 个 mutant 都在；P2 没有 W5 专属的新候选。P3 的 W5 行（`:138,280`）与 ledger 一致。
- **保留**：W2 的 "无新增候选" 与 M-67（ProgressSource）被路由到 W3 的边界没有在 P3 中解释；读者可能把 M-67 误读为 W2 候选。这是 L5，不影响 NOT_APPLICABLE 的成立。

---

## 3. 问题清单

> 严重度：`high` = 该归因/结论不能用于准入裁决或后续施工；`medium` = 数值、门槛或语义义务错误，必须修正后才可用于预算/设计；`low` = 表述、引用或精度问题，不改变方向。

| # | 严重度 | where | issue | evidence |
|---|---|---|---|---|
| **H1** | **high** | `p3/P3-ADMISSION.md:98,236,318`（§1.3 C5b 行、§4.2、§6.2 stage 1） | W3 的 "A/A 分辨率不是瓶颈 / 36 对预算对 2% 足够" 用了 C5b（TP d1 w1），但 C5b 不是任何 P2 W3 形态的匹配格；W3 最大形态 P-D（TP d16 w4，22.43%）的匹配格 C2b 是 NOISY，"36 对足够" 对 P-D 不成立。 | `p2/P2-REVIEW.md:75-92` 明确禁止跨形态搬 A/A 带；`p1/AA-CALIBRATION-2.md:413,416` 规定 C5b 的带只适用于 C5b、不能搬到 d16/d64；我运行 `python3 -I recompute_p2_r1.py --json-out /tmp/review-p2r1.json` 后读取 `aa.matching`：C5b=`"not P-D (d16) and not P-B (uring)"`、`"QUIET conditional; not a matching cell"`，C2b=`"P-D"`、`"NOISY (no usable band)"`；`p2/P2-COST-MAP-R1.md:474-482` 给出 W3 族 P-D 22.43%、P-C 15.82%、P-E 13.07%、P-B 0.24%；P3 自己还在 `:99` 写 C5b cycles "不作窄带 | 不用于 W1/W3"，却在 `:98` 把 C5b CPU 用于 W3。 |
| **H2** | **high** | `p3/P3-ADMISSION.md:229`（§4.2），与 `:84`、`:232`、`:164` 对照 | P3 把 P-D publication 阶段 4.13% 写成 "M-62 的 4.13% 是上界"，直接违反同一文件 "阶段容器不能作准入上界" 的规则；§4.2 的 M-62 理由因此包含一个不可用的上界，和该行自己的 "容器" 说法冲突。 | P3 `:84`："不接受…阶段容器…不能作准入上界"；P3 `:232`："阶段容器不是候选上界"；P3 `:164` M-62 行自己写 "P-D publication 4.13% 容器"；P2-REVIEW M2/M3（`p2/P2-REVIEW.md:116-143`）明确 M-62 的 4.74% 双计已改为 4.13，但 4.13 仍是 publication 容器，不是候选上界；`P2-COST-MAP-R1.md:488-489,495` 标 M-61/M-62 为 "阶段容器 / NOT_OBSERVABLE"、单候选可删除量为 "NOT_OBSERVABLE"。 |
| **H3** | **high** | `p3/P3-ADMISSION.md:37-40,139,177,271,299`（§0.1 计数、§2 W6、§3.3 M-28、§4.6、O-10） | M-28 被从 `NO_GO_ABORT` 改标为 `NOT_APPLICABLE`/`NO_REDO`，但 `syscall_count_` 仍在生产，`NOT_APPLICABLE` 的 "无适用对象" 定义不成立；同时 "翻案三条件" 漏掉 ledger §4.1 第 1 条 "可分离的单指令差"，重开门槛被降低。 | `CAMPAIGN-LEDGER.md:196,207`（M-28=NO_GO_ABORT）、`:237`（第 1 条 "新证据把机制约束为可分离的单指令差"）、`:512`（NOT_APPLICABLE 定义）；`src/async/threadpool_backend.cpp:434` 的 `syscall_count_` fetch_add 与 `include/sluice/async/threadpool_backend.hpp:258` 成员仍在生产；P3 `:177`/`:271` 的三条件列表无 "单指令差"；P3 `:299` O-10 又写 "确认 NO_GO_ABORT"，与 `:177` 的 `NOT_APPLICABLE` 冲突。 |
| **M1** | medium | `p3/P3-ADMISSION.md:173,240,246,294,319`（§3.3 M-53、§4.3、§6.2 stage 2、O-5） | M-53 的 `d64 cycles-only A/A 17–20` 只对应 δ=2%；P3 把已测 d16 leaf self 0.406%/0.285% 称为 "0.4% 的效应"，但没有说明若 d64 效应接近该量级，ledger 的 "半宽 ≤ 效应/2" 需要约 172（80%）/336（hw）对；P3 又允许 δ=1% 的 A/B 58 对，却把 A/A 固定为 17–20（不满足 δ=1% 的 hw≤0.5%）。 | `p2/P2-COST-MAP-R1.md:499-511`（d16 34/47、0.406%/0.285%）；`p1/AA-CALIBRATION-2.md:485-506`（C7 cycles SD 1.889、t-hw 1.007%、n_half(2%)=17、1% 需 58）；`CAMPAIGN-LEDGER.md:211`（M-53 判据 "半宽 ≤ 效应/2"）；我的算术：C7 cycles SD 1.889、δ=0.406%/2=0.203% → n_half=336，δ=0.406% → n80=172。 |
| **M2** | medium | `p3/P3-ADMISSION.md:164-165,167`（§3.2 M-62/M-65/M-10） | W3 的 ledger 门禁要求任何跨方法临界区合并证明 `publication_inflight`/slot reuse/observer 注册不被改变；P3 只在 M-61 行写了 `publication_inflight`，M-62/M-65/M-10 的重开条件没有这份义务。 | `CAMPAIGN-LEDGER.md:259`："任何跨方法临界区合并必须证明 publication_inflight/slot reuse/observer 注册不被改变"；P3 `:163` 有 `publication_inflight`，`:164` 只有 observer 注册，`:165`/`:167` 只有泛化的 "步间语义/交付顺序"。 |
| **M3** | medium | `p3/P3-ADMISSION.md:163`（§3.2 M-61） | M-61 的重开条件漏掉 ledger 的 "确定性测试注入点" 和 "族 A/A 先行"；只写 "publication_inflight 反例处理 + 融合的步间语义"。 | `CAMPAIGN-LEDGER.md:222`："新 measured profile 推翻 r6 R3 拒绝（0.45% < 1% MME REJECTED，含 publication_inflight 反例与确定性测试注入点）+ 保步间语义的融合单因素 A/B（族 A/A 先行）"；P3 `:163` 对应文本。 |
| **M4** | medium | `p3/P3-ADMISSION.md:176,254`（§3.3 M-42、§4.4） | M-42 的未来门禁漏掉 ledger 要求的 "mutant kill" 与 "release 无 seam 符号"，也没有写 "出现生产读者 → NO_GO/重新物化" 的后果；只写隔离格 + 动态回执带 + 机制门七问。 | `CAMPAIGN-LEDGER.md:217,262`："M-42 必须证明无生产读者、过机制门七问、动态回执带 + 隔离格判据、mutant kill、release 无 seam 符号"；P3 `:176` 的门禁列表。 |
| **M5** | medium | `p3/P3-ADMISSION.md:318`（§6.2 stage 1） | W3 行级归因的第一步被写成 "P-B 用户栈截断修复"，但 P-B 的 W3 族只有 0.24%，P-D 22.43%/P-C 15.82%/P-E 13.07% 才是 W3 主体；修 P-B 栈不能产生 W3 主形态的候选级归因。 | `p2/P2-COST-MAP-R1.md:474-482`；`p2/P2-REVIEW.md:54-71`（P-B 90.67% app 无 bench 帧，W3 0.24% NOT_OBSERVABLE）；P3 `:318`。 |
| **M6** | medium | `p3/P3-ADMISSION.md:153,210`（§3.1 W1-C2） | W1-C2 被计入 `SEMANTIC_MISMATCH`，但 PROG-02 明确允许 condvar + protected predicate，W1-DESIGN 对 C2 的判词是 `NEEDS_OWNER`；这会把 "owner 尚未授权第二通知通道" 写成 "语义冲突"。 | `docs/explicit-io-contract.md:907-912`；`p3/W1-DESIGN.md:245-256`；P3 `:153`/`:210`。 |
| **M7** | medium | `p3/P3-ADMISSION.md:134-139,165,321`（§2 W3/W4 看板、§3.2 M-65、§6.2 stage 4） | M-65 在 §3.2 被列为 W3 行级候选，在 §2 看板/§6.2 stage 4 又被放在 W4/T9；ledger 本身在 W3（`:259`）和 W4（`:260`）都提到 M-65，矩阵没有给 M-65 单一包归属，预算/阶段会冲突。 | `CAMPAIGN-LEDGER.md:259-260`；P3 `:134-139`（W4 看板列 M-65）、`:165`（W3 表）、`:321`（stage 4 T9）。 |
| **L1** | low | `p3/P3-ADMISSION.md:134,141`（§2 W1） | W1 被标为 "终判"，但 O-1/O-2/O-3 仍开放，O-2 可以改变 W1 的 C2/D 结论；"终判" 与 "owner 点开放" 是矛盾的状态标签。 | P3 `:134`、`:141`、`:290-292`。 |
| **L2** | low | `p3/P3-ADMISSION.md:165,167`（§3.2 M-65/M-10 引用） | 引用行号错误：M-65 行用 `CAMPAIGN-LEDGER.md:212`（M-63 行）来支撑 "owed 机制本体 KEEP"，M-65 行实际是 `:224`；M-10 行用 `:221` 支撑 "NOT_OBSERVABLE"，但 `:221` 是 M-10 的 `INCONCLUSIVE` 行，"NOT_OBSERVABLE" 来自 P2 `P2-COST-MAP-R1.md:492`。 | `CAMPAIGN-LEDGER.md:212,221,224`；`P2-COST-MAP-R1.md:492`；P3 `:165,167`。 |
| **L3** | low | `p3/P3-ADMISSION.md:48-49,250`（§0.2/§4.4） | M-42 的 19–20 cycles 是 P2-REVIEW M4 引用的 Haswell L1 命中外部值，P2 明确 "本会话只确认指令是 lock addq，没有重新测锁延迟"；P3 把它作为算术上界时没有重复这一置信限制。M-53 的 61.3/58.0 cycles/op 是 leaf share × PMU cycles/op，P2-REVIEW M5 已把它标为 MEDIUM/LOW（PMU/record 分母不一致、无 PEBS/skid）。 | `p2/P2-REVIEW.md:146-158`（M4）、`:162-174`（M5）；`p2/P2-COST-MAP-R1.md:524`（未重测锁延迟）、`:499-511`（M-53）；P3 `:48-49,250,240`。 |
| **L4** | low | `p3/P3-ADMISSION.md:34,164-167,174-175,187-188`（§0.1/§3.2/§3.3/§3.4） | `ADMIT_TO_DESIGN` 把 8 个 #637 `INCONCLUSIVE` 候选（M-62/M-65/M-67/M-10/M-33/M-74/M-40/M-63）改成了新状态，但 ledger §4 规则 5 要求 `INCONCLUSIVE` 必须满足重开条件才可进入下一轮；P3 的设计工作本身是重开条件的一部分，宜保留 `INCONCLUSIVE + design work` 而不是改状态。 | `CAMPAIGN-LEDGER.md:199`（规则 5）、`:223-227`（M-62/M-65/M-67/M-10/M-33/M-74）、`:218`（M-40）；P3 `:34`。 |
| **L5** | low | `p3/P3-ADMISSION.md:258`（§2 W2） | W2 的 `NOT_APPLICABLE` 按 ledger 原文成立，但 P3 没有解释 M-67（ProgressSource sticky flags）按 ledger §5.2 被路由到 W3；读者可能把 M-67 误读为 W2 新候选。 | `CAMPAIGN-LEDGER.md:258,271`；P3 `:166,258`。 |

---

## 4. 对 H1–H3 的具体修正建议

### H1 — W3 A/A/预算

1. 把 §4.2 的 "A/A 分辨率不是瓶颈" 改为按形态列：
   - P-D（TP d16 w4，W3 22.43%）：匹配格 C2b **NOISY，无可用带**；必须先重跑 C2b A/A（或选择另一个已测形态）。
   - P-C（W-03 d16，15.82%）：匹配格 C4 CPU，2% 需 20/38 对（hw≤1% 需 38）；C4 cycles MDE 6.088% 不可用于 1% 声明。
   - P-E（copy，13.07%）：匹配格 C1 CPU，2% 需 4/5 对；C1 cycles 2% 需 13/24。
   - P-B（uring d1，0.24%）：匹配格 C6，但 W3 族本身 NOT_OBSERVABLE；不要用 C5b。
2. 删除 §1.3 `C5b` 行的 "本战役用途 W3 TP d1 w1"，或明确写 "C5b 不是 P-D 的匹配格；若 W3 要在 TP d1 w1 上做，必须另做 W3 形态 profile 与 A/A"。
3. 把 "36 对预算对 2% 足够" 改为 "仅对 C5b/C3b/C7 的 2% 成立；P-D 无带，C4 的 hw≤1% 需 38 对"。

### H2 — M-62 的 4.13%

1. 把 §4.2 `:229` 改为 "M-62 的证据是 publication 阶段容器 4.13%，**不是候选上界**；M-62 的候选级可删量仍 `NOT_OBSERVABLE`，需要行级 profile"。
2. 与 §3.2 M-62 行的 "P-D publication 4.13% 容器" 保持一致；不要把 4.13 写进任何 "上界" 语境。
3. 如果要在 §4.2 说明 M-62 的旧 4.74% 已删，写 "4.74% 的双计已删；4.13% 仍是 publication 容器，不是候选上界"。

### H3 — M-28

1. 把 §0.1 的 `NOT_APPLICABLE` 3 改为 `NOT_APPLICABLE` 2（W2、W5）+ `NO_GO_ABORT` 1（M-28）；或至少在 M-28 行保留 `NO_GO_ABORT` 并注明 "本战役不重做"。
2. 把 §2 W6 的 `NO_REDO` 改成 `NO_GO_ABORT（不重做）`；`NO_REDO` 是 ledger §5.2 的优先级标签（`CAMPAIGN-LEDGER.md:262`），不是处置词汇。
3. 把 `:177`/`:271` 的三条件补全为 ledger §4.1 四条要素：
   - 新证据把机制约束为可分离的 "单指令差"；
   - 消除/隔离 TU 重编译布局变化、成员移除的对象布局变化、RMW 移除对 `work_mtx_` 竞争时序的影响；
   - 新 session、新冻结；
   - 动态回执自带接受带与隔离格判据。
4. 统一 O-10 的措辞："确认 `NO_GO_ABORT`（不重做），除非同时满足上述条件"。

---

## 5. P3 做对的部分（避免只列问题）

- **`ADMIT_TO_AB = 0` 正确。** 我尝试按五条件推翻它：W1 的 material 方向 D/C1 语义不成立，C2 需 owner；W3 只有容器；M-53 d16 0.4% 低于 36 对分辨率、d64 未测；M-42 算术上界 0.066–0.133% 远低于匹配带；W2/W5 无新候选。没有候选能同时满足五条件。
- **W1 的语义分析是可用的。** D1 的 `dup`/`F_SETFL` 共享文件状态标志、D2 的非阻塞 ack/drain 与有界 timeout、D3 的单 eventfd 注册、C1 的第二 drain authority，都有源码/合约依据；`os.dup` 输出与 `progress_source.hpp:75,414-460,539-545` 一致。
- **M-42 的 `NOT_POWERED` 与 `lock addq` 证据是可用的。** 我独立执行 objdump 得到同一指令；0.066–0.133% 的算术与 P2 一致；`GO` 只是排队的判断也与 ledger 一致。
- **M-53 的三项迁移义务是完整的。** TransportLedger 物理位置单调、控制 CQE exact-submitted、cancel 定向都列了；d64 C7 的 t-hw 1.007% 与 AA-2 一致。
- **W2/W5 的 NOT_APPLICABLE 与 ledger 原文一致。** 我没有找到 P2 新增的 W2/W5 候选；#634 的 parity 8 + 4 mutants、#635 的 9 用例 oracle + mutant 都在。
- **§5 Owner 裁决点、§7 执行清单、§8 不能下的结论** 都符合 "不把未运行写成通过" 的纪律；P3 的 §7.1 数字与我的复算一致，§7.2 的未运行清单完整。

---

## 6. 结论

**这份矩阵的 "负结论" 部分（`ADMIT_TO_AB=0`、W1 语义阻断、M-42 NOT_POWERED、W2/W5 无新候选、#637 终态未推翻）可以作为后续施工的起点；但 "设计/预算" 部分不能按现状直接执行。**

具体：

- 如果施工队按 §4.2 的 "36 对预算对 2% 足够" 去排 W3，会在 P-D/C2b 上直接撞到 "NOISY，无可用带"；C5b 的 0.458% CPU hw 不能搬到 P-D。
- 如果按 M-62 的 "4.13% 上界" 去设计，会把 publication 容器当成收益上界，重犯 P2-REVIEW M2/M3 的错误。
- 如果按 M-28 的 "NOT_APPLICABLE" 去归档，会把一个仍在生产的 `NO_GO_ABORT` 候选误记为 "无适用对象"，并漏掉 "单指令差" 这一重开前提。
- M-53 的 17–20 对只在 δ=2% 下成立；如果 M-53 的真实 d64 效应接近 d16 的 0.4%，需要约 172/336 对，而不是 17–20。

修正 H1–H3 后，`ADMIT_TO_DESIGN` 的 11 行可以作为设计/资格工作起点；A/B 仍必须按 ledger §9.3 重新预注册、重跑语义门与匹配 A/A，不能直接用 P3 的预算数字。

---

## 7. 复核限制

- 没有重跑 `xmake` 构建/测试、perf、bench、A/A、A/B、TLC、sanitizer；本复核只读归档原始数据并运行既有复算脚本。
- 没有打开 `#637` 归档 `e2f41acb` 的原始回执；M-61 R3 的 0.45%、M-28 的 1b/1c 判词按 `CAMPAIGN-LEDGER.md` 的转述核对，未独立复算。
- A/A 复算只重算已归档的 `p1/aa`、`p1/aa2` 原始记录，没有重跑计时；`recompute_p2_r1.py` 依赖 `/tmp/p2-621/kallsyms.txt` 与 `/tmp/p2-621/symtwin/*.sym`（本机存在）。
- 行级归因仍 `NOT_OBSERVABLE`；本复核不能给出新的指令级/行级证据。
- 我的严重度判断以 "能否用于准入裁决/后续施工" 为准；`medium` 项不否定候选方向，但必须在施工前修正。

---

*本文件结束。所有未在 §1 执行的断言都是 "未运行/未验证"；所有数字来自已发布证据或我本轮运行的复算脚本。*
