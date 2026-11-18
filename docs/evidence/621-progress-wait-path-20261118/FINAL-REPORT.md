# FINAL-REPORT — #621 Progress/Wait 路径战役最终报告（A–K）

| 字段 | 值 |
|---|---|
| 战役 | #621 Minimum Necessary Machine Work — Progress/Wait 路径（应用路径权威 #604） |
| 报告日期 | 2026-11-18 |
| `CAMPAIGN_BASE_SHA` | `5d94f233967c3aef993eb19ba9e3252c532b0951` |
| HEAD / `origin/master` | 同 BASE（本会话 `git rev-parse HEAD`、`git rev-parse origin/master` 实测） |
| 分支 / worktree | `master`；唯一 worktree `/home/jnhu/Source/Sluice`；stash 为空 |
| 报告性质 | 战役终稿。不是合并授权、不是产品语义权威、不是 A/B 实验报告；不替代 `docs/explicit-io-contract.md`、`docs/roadmap/v2-conformance.md` 或 `CAMPAIGN-LEDGER.md` |
| 修订 | 2026-11-18 按 `FINAL-REPORT-REVIEW.md` 的 H-1/H-2、M-1–M-9、L-1–L-5 修订；未删除证据路径、未降低口径 |
| 最终判词 | **`ADMIT_TO_AB = 0`；无 GO；无产品 PR；无新 A/B**。建议 `NO_FURTHER_OPTIMIZATION_JUSTIFIED`（当前没有可进入 A/B 的候选；W3/M-53 设计门仍开放，需 owner 决定是否资助；见 §K） |
| 回写稿 | `ISSUE-621-COMMENT.md`（同目录；≤50KB；无凭据） |

> **一句话结论**：本战役把 Progress/Wait 路径的“机器工作在哪里、候选为什么不能进 A/B、还缺什么”查清了，但没有产生任何新的产品收益或 A/B 收益。合并进 BASE 的 `#634`/`#635` 是仅有的两个已落地减量事实；本轮 0 个新候选达到 A/B 准入。W1 的 material 方向（blocking read / 第二等待 fd）在当前 root/拓扑下是语义阻断，不是“还没测”；唯一契约允许的 material 替代（C2 condvar 双通道）需要 owner 授权。W3 有候选级 owner-total 点估计（`offer_terminal` 1.80%、`claim_execution` 1.56% 等），但 P4R 判定其为方向性证据、单候选可删量仍 `NOT_OBSERVABLE`；M-53 的 O1/O2 已证、O3（packed 编码）未在 HEAD 实现；**若** d64 效应接近 d16 的 0.4% 量级（乐观代理假设），172/336 对不可行，P4 的 1.06%/1.13% 点估计不显著、尚未证明大于 0.4%。M-42 低于噪声；`#637` 的 `NO_GO`/`NO_GO_ABORT`/`SEMANTIC_MISMATCH` 终态未被翻案（M-16/M-42 的分类性调整为本轮裁决）。

---

## 0. 阅读约定、来源与证据等级

### 0.1 来源标签

本文每个数字都带来源标签；同一表格内未逐格重复的，表题给出共同来源与口径。

| 标签 | 文件 | 性质 |
|---|---|---|
| `[L]` | `CAMPAIGN-LEDGER.md` | 战役台账（2026-11-18；只读转述与源码核验） |
| `[P1]` | `p1/AA-CALIBRATION.md` | P1 六格 A/A 独立复算 |
| `[P2A]` | `p1/AA-CALIBRATION-2.md` | P2 五格补测 A/A 独立复算 |
| `[P2M]` | `p2/P2-COST-MAP-R1.md` | P2 成本地图修订版（profile session） |
| `[P2R]` | `p2/P2-REVIEW.md` | P2 成本归因独立复核 |
| `[P3]` | `p3/P3-ADMISSION-R1.md` | W1–W6 与 `#637` 准入矩阵修订版 |
| `[W1D]` | `p3/W1-DESIGN.md` | W1 poll 等待路径语义与设计准入 |
| `[P3R]` | `p3/P3-ADMISSION-REVIEW.md` | P3 准入矩阵独立对抗复核 |
| `[P4W]` | `p4/W3-ROWLEVEL.md` | P4 W3 行级归因 profile |
| `[P4R]` | `p4/W3-ROWLEVEL-REVIEW.md` | P4 行级归因独立复核 |
| `[M53]` | `p4/M53-OBLIGATIONS.md` | M-53 三项迁移义务审计 |
| `[B1]` | `p1/build-manifest.md` | P1 构建/测试回执 |
| `[B4]` | `p4/rowlevel-build-manifest.md` | P4 DWARF 构建回执 |
| `[V-*]` | 本会话实际执行的命令/输出 | 见 §A.6、§G.1 |
| `[I]` | 已合并历史事实（Git 祖先已核验） | 性能数字是历史回执，未在本轮重跑 |

### 0.2 证据等级（全文统一）

| 等级 | 含义 | 使用约束 |
|---|---|---|
| **V**（verified this session） | 本会话实际运行命令并读取输出 | 可作为本轮事实；命令见 §A.6/§G.1 |
| **R**（recorded） | 战役证据文档中的回执/复算；本会话只读，未重跑 | 引用时必须带文档与节号；不得写成“本轮实测” |
| **I**（inherited） | 已合并进 BASE 或已归档的历史事实；Git 祖先/路径已核验，但性能结果未重跑 | 只能作为历史事实；不得当作当前 master 的实测 |
| **U**（unconfirmed / not covered） | 没有证据、未运行或不可观测 | 必须显式标注；不得用“未失败”代替“通过” |

### 0.3 三个全局口径

1. **份额 ≠ 可删 cycles**；**阶段容器 ≠ 候选上界**；**inclusive 不能相加，也不能当收益**（`[P2M] §1`、`[P3] §0.3`）。
2. **profile 只证明 bench/reference 形态**；`sluice-copy` 是参考应用，不是 `#604` G0 真实 workload（`[L] §6.2`、`[P2R] M10`）。
3. **A/A 只回答“若效应是 X，需要多少对”**，不回答“效应是 X”；匹配格不能跨形态/深度/后端搬运（`[P1] §5`、`[P2A] §5`、`[P3] §1.3`）。

---

## A. Repository Reality

### A.1 Git 基线（V）

| 项 | 实测值 | 命令/来源 |
|---|---|---|
| `CAMPAIGN_BASE_SHA` | `5d94f233967c3aef993eb19ba9e3252c532b0951` | `[V-A1] git rev-parse HEAD` |
| `origin/master` | 同 BASE | `[V-A1] git rev-parse origin/master` |
| 分支 | `master`，跟踪 `origin/master` | `[V-A1] git status` / `git worktree list --porcelain` |
| worktree | 仅 `/home/jnhu/Source/Sluice`（主树，master @ BASE） | `[V-A1] git worktree list --porcelain` |
| stash | 空 | `[V-A1] git stash list` |
| 已跟踪工作树 | 干净：`git diff --stat` 与 `git diff --cached --stat` 均无输出 | `[V-A1]` |

### A.2 未跟踪内容与纪律（V）

本会话 `git status --porcelain=v1 --untracked-files=all` 显示：

- `docs/evidence/621-progress-wait-path-20261118/`（本战役新建证据目录，含本报告）；
- `docs/evidence/621-round2-R1/`（`#635` 的 R1 工作稿，已被 R2 r6 归档 `0e50612c` 取代）；
- `docs/evidence/e5-fullpath-machine-work/`（`#637` 战役文档的本地未跟踪副本）。

后两个目录是台账 §9.1.12 明确“全程不得触碰”的对象；本报告与回写稿**未修改、未移动，也未将其内容作为证据引用**；本报告不依赖这两个目录。`docs/evidence/621-progress-wait-path-20261118/` 在 BASE 中未跟踪，是本战役的交付目录；引用其文件时按“战役证据目录”理解，不声称它们在 BASE 已存在或已提交。

### A.3 工具链（V）

| 项 | 值 | 来源 |
|---|---|---|
| 内核 | `7.3.3-200.fc44.x86_64` | `[V-A3] uname -r`；`[B1]` |
| CPU | Intel Xeon E5-2666 v3 @ 2.90 GHz；20 logical CPU | `[V-A3] /proc/cpuinfo`；`[P2A] §1.2` |
| 编译器 | GCC 16.2.1 20261029（Red Hat 16.2.1-3） | `[V-A3] gcc --version`；`[B1]` |
| 构建器 | xmake v3.1.2+HEAD.5d21e9d | `[V-A3] xmake --version`；`[B1]` |
| liburing | 2.13（`liburing-2.13-2.fc44`、`liburing-devel-2.13-2.fc44`） | `[V-A3] pkg-config --modversion liburing`；`[B1]` |
| ccache | 启用（继承配置；fresh build dir 但编译器缓存是 warm 的） | `[B1]`、`[B4]` |

### A.4 构建 / 测试矩阵回执

> **关键纪律**：以下回执分为“P1/P4 实际执行的回执（R）”、“已合并历史回执（I）”和“本战役未运行（U）”。**本会话没有运行任何 `xmake f/build/test`**，因此当前 master 的测试总数 `N` 仍未由本会话确认。

#### A.4.1 P1 构建与 release+liburing=y 套件（R）

`[B1]` 记录，2026-11-18 在 `/tmp/621-p1-build/` 新建四个目录，均不使用仓库内 `build/`/`build-bench-*`：

| Build dir | Profile | Config exit | Build / test | 结果 |
|---|---|---:|---|---|
| `noring` | release, liburing=n | 0 | `bench_w01_direct bench_w02_pipeline bench_w03_external_loop` | exit 0 |
| `ring` | release, liburing=y | 0 | 同上 | exit 0 |
| `copy` | release, liburing=y | 0 | `sluice-copy` | exit 0 |
| `tests` | release, liburing=y | 0 | `xmake test -y` | `100% tests passed, 0 test(s) failed out of 81, spent 4.012s`；`TEST_EXIT=0` |

- `readelf -d` 断言：ring 的 W-02/W-03 链 `liburing.so.2`；noring 不链；`bench_w01_direct` 两 profile 相同；`sluice-copy` 链 liburing（`[B1]` Link-profile assertions）。
- 二进制 sha256（release ring/copy）：`bench_w02_pipeline 03dbef09…`、`bench_w03_external_loop 43054fcd…`、`sluice-copy db972b19…`（`[B1]`；与 A/A `meta.json` 一致，`[P1] §2.4`）。
- **口径**：81 是 `out of 81` 解析出的测试数，不是 `100%`；本会话没有重跑该套件。

#### A.4.2 P4 DWARF 构建（R）

`[B4]` 记录，2026-11-18 在 `/tmp/621-p4-build/` 新建三个目录：

| Build dir | 配置 | 结果 |
|---|---|---|
| `ring` | `-m release --cxflags=-g --ldflags=-g --policies=build.release.strip:n --liburing=y` | exit 0 |
| `noring` | 同上，`--liburing=n` | exit 0 |
| `copy` | 同上，`--liburing=y`，`sluice-copy` | exit 0 |

- 五个 DWARF 二进制（ring W-02/W-03、noring W-02/W-03、copy）均 `with debug_info, not stripped`；`.text/.rodata/.data` 与 P1 release **逐字节相同**，VMA 不变（`[B4] §7`；`[P4R] §4.1` 复核）。
- 用途仅为行级归因，**不得用于 A/B 或时长比较**（`[B4] §7`、`[P4W] §8`）。
- 未运行 P4 的 `xmake test`；P4 的 smoke 只是入口/格式验证，不是计时证据（`[B4] §8`）。

#### A.4.3 完整构建测试矩阵（U）

台账 §8.2 原样记录的矩阵包括：release/debug × liburing=y/n 四组全量 `xmake test`，以及 ASAN/TSAN 下的 `progress_source_drain_semantic_oracle_test threadpool_owed_counter_parity_test threadpool_progress_race_test app_copy_pipeline_oracle_test`。**本战役没有运行该完整矩阵**（`[L] §0.3`、`:431-477`）。因此：

- 不能用 P1 的 release+liburing=y 81/81 代替 debug/noring/ASAN/TSAN；
- 不能用 `#634`/`#635` 的历史矩阵回执代替当前 master；
- 任何未来产品 PR 在合并授权前必须补跑受影响 claim 的完整矩阵（AGENTS.md；`[L] §9.1`）。

#### A.4.4 历史矩阵回执（I）

| 工作 | 历史回执 | 状态 |
|---|---|---|
| `#634` | 80/80 & 59/59 × {release,debug} × {liburing y,n} | 历史回执；本轮未重跑 |
| `#635` | 81/81 ×2 + 60/60 ×2 | 历史回执；本轮未重跑 |

来源：`[L] §8.2`（`:475`）。这些数字**不是**当前 master 的实测；当前 master 的 `N` 未确认。

### A.5 历史证据继承（V/I）

| 工作 | 合并/归档 SHA | 在 BASE 中？ | 证据位置 | 继承口径 |
|---|---|---|---|---|
| `#609`/PR #618 ELIDE | `98825793` | 是（祖先已核验） | `[L] §3` | 终态 `E5_MECHANISM_VERIFIED / END_TO_END_WIN_NOT_DEMONSTRATED`；四个 native W02 格在噪声内（历史，未重跑） |
| `#634` ThreadPool owed-count gate | merge `3f22d2d0`；base `f2c42535` | 是（`3f22d2d0` 是 HEAD 祖先，已核验） | `docs/evidence/621-threadpool-owed-count-20261112/` **已跟踪，在 BASE 中** | 生产代码与测试在 BASE；性能数字（CPU −5.58%、wall −6.53%、instr −22.1%）是历史回执，未重跑 |
| `#635` ProgressSource 单读 drain | merge `5d94f233`（BASE 本身） | 是（HEAD） | 证据目录 `docs/evidence/621-progress-drain-single-read-20261112/` **不在 BASE**；只在归档 `0e50612c`（PR #636 closed/unmerged） | 产品代码/测试在 BASE；证据只在未合并归档；`0e50612c` **不是** HEAD 祖先（已核验） |

补充：

- `git cat-file -p 3f22d2d0` 显示父提交 `f2c42535`、`3d7e0685`；`git cat-file -p 5d94f233` 显示父提交 `3f22d2d0`、`f707ff39`。`git merge-base --is-ancestor` 对 `3f22d2d0`、`5d94f233`、`98825793` 返回真；对 `0e50612c` 返回假（`[V-A5]`）。
- v2 root header 的 `Implementation baseline` 仍写 `b870a7c0`（`docs/explicit-io-contract.md:10`），v2 ledger 的 `Assessed source commit` 仍为 `c001bb1e`；两者都不包含 `#634`/`#635`，不能当现状（`[L] §1.3`）。
- `.xmake` 存在既有 project-pointer 漂移（指向已不存在的旧目录）；所有构建均显式 `-P /home/jnhu/Source/Sluice`，并在结束后逐字节恢复（`[B1]`、`[B4] §9`）。这是环境卫生事实，不是产品行为。

### A.6 本会话实际执行 / 未执行（V/U）

**已执行（V）**：

| 检查 | 命令（摘录） | 结果 |
|---|---|---|
| Git 基线 | `git rev-parse HEAD`、`git rev-parse origin/master`、`git status --porcelain=v1 --untracked-files=all`、`git worktree list --porcelain`、`git stash list`、`git diff --stat`、`git diff --cached --stat` | HEAD=origin/master=BASE；单 worktree；stash 空；tracked tree 干净 |
| 祖先 | `git merge-base --is-ancestor <sha> HEAD`（`3f22d2d0`、`5d94f233`、`98825793`、`0e50612c`） | 前三者 ANCESTOR；`0e50612c` NOT_ANCESTOR |
| 工具链 | `gcc --version`、`xmake --version`、`pkg-config --modversion liburing`、`uname -r` | 见 §A.3 |
| 环境只读 | `cat scaling_governor`、`cat intel_pstate/no_turbo`、`cat perf_event_paranoid`、`stat -c %s /sys/kernel/btf/vmlinux`、`stat -f -c %T /tmp`、`findmnt -no FSTYPE,SOURCE,OPTIONS /`、`cat /sys/block/*/queue/scheduler`、`free -g` | 见 §B.1 |
| P1 A/A 复算 | `python3 -I tools/recompute_independent.py --aa-root p1/aa --json-out /tmp/final-aa1.json` | exit 0；6 格判词与 §F 表一致（C2/C5 为 0 对） |
| P2 A/A 复算 | `python3 -I tools/recompute_independent.py --aa-root p1/aa2 --json-out /tmp/final-aa2.json` | exit 0；5 格判词与 §F 表一致 |
| P2 R1 复算 | `python3 -I p2/profile/analysis/recompute_p2_r1.py --json-out /tmp/final-p2r1.json` | exit 0；P-B `__poll` 24.93%、poll boundary 9.16%、progress/wait 25.09%；P-E 15.83/4.38/24.36；P-D publication 4.13%；M-53 d16 34/47；M-42 `lock addq` 复现 |
| 独立 t/MME | 归档 `tools/recompute_independent.py` 的 `requiredN`/`mde80` 字段交叉验证；另用一次性自写 `/tmp/final_tcheck2.py`（不 import 仓库工具；Numerical-Recipes 不完全 beta + 二分）作第二实现 | required-n 与发布值一致（§G.3），仅 C7 CPU δ=2% n_half 边界差 1 对；一次性脚本未入库，归档路径的字段可复跑 |
| M-53 三个测试 | `./build/linux/x86_64/release/uring_pending_control_parity_test`；`.../backend_contract_conformance_uring_test`；`.../uring_request_core_integration_test` | 7/7、20/20、21/21，均 exit 0（现有 2026-11-12 二进制，未 rebuild） |
| 源码抽查 | `sed`/`grep`：`progress_source.hpp`、`async_io_context.cpp`、`uring_transport.cpp`、`threadpool_backend.cpp`、`uring_backend.hpp`、`uring_test_seams.hpp` | 关键坐标与证据一致（§E） |

**明确未运行（U）**：

- 任何 `xmake f`/`xmake build`/`xmake test`；当前 master 的测试总数 `N` 未确认。
- 任何新的 perf/record/stat/eBPF/uprobe/strace；任何新的 A/A 或 A/B；任何候选 build 或候选 SHA。
- 任何 TLC/`scripts/verify_progress_source_model.sh`；任何语义 oracle/mutant 的重跑（M-53 的三个测试是既有二进制，不是语义 oracle）。
- 任何 ASAN/TSAN；完整构建测试矩阵。
- P4 `analyze_w3.py` 的本次重跑（`[P4R] §1` 记录了其复跑结果：与归档 JSON 逐字节相同、`SHA256SUMS` 155/155 OK；本会话未重复）。
- 任何真实消费者 profile（`#604` G0 未准入）。

---

## B. E5 CPU & Environment

> 本节主体来自环境审计与台账（R），本会话只做了只读 sysfs/procfs 复核（V）。**没有复跑 turbostat/APERF-MPERF/PMU/eBPF 实测**；PASS 的依据是战役环境审计的原始记录。

### B.1 频率门禁：PASS（R；现状只读复核 V）

| 场景 | 实测 | 结论 |
|---|---|---|
| 空闲 | cpu0 Busy 0.15–0.63%，C6 93.4–99.2%（C3=0），PkgTmp 47–48°C，CoreTmp 39–40°C | 可进入受控负载 |
| 单核（`taskset -c 0 yes`，15×1s） | Busy 98.94–99.89%（均值 99.58%），Bzy_MHz 均值 3478（3408–3496），CoreThr=0 | 单核频率可解释、无掉频 |
| 4 物理核（`taskset -c 0,2,4,6`，避开 SMT 兄弟，20×1s） | Busy 均 100.00%，Bzy_MHz 20 点恒为 3200，CoreTmp 50.1–52.8°C | 多核频率稳定 |
| APERF/MPERF | 单核 34,653,506,826 / 28,831,697,459 → 3477 MHz；4 核 127,726,045,761 / 115,751,730,428 → 3192 MHz | 与 turbostat 吻合 |
| Turbo/节流 | `no_turbo=0`；core/package throttle_count 前后 0；dmesg 无 thermal throttle；`cpu_thermal_margin` 37°C | 未禁用 turbo，未观察到节流 |
| 功耗计数 | RAPL 空闲 35.28 W、4 核 35.20 W；MSR 0x613/0x64f 返回 Invalid argument | **功耗计数不可信**；不得用于能效声明 |

来源：`[L] §7.1`。门禁结论：**PASS**；单核 ≈3.48 GHz、≥4 核 ≈3.20 GHz；3.5 GHz 只作单核参考（`[L] §7.1`）。

**本会话只读复核（V）**：`cpu0/cpu1 governor=schedutil`；`no_turbo=0`；`perf_event_paranoid=2`；`/sys/kernel/btf/vmlinux=7,023,892 B`；`/tmp` 为 tmpfs；根 `/` 为 `/dev/sda3` btrfs `rw,relatime,seclabel,compress=zstd:1,ssd,discard=async,space_cache=v2`；`sda` scheduler `bfq`、`nvme0n1` `none`；内存 62 GiB；`irqbalance`、`tuned`/`tuned-ppd` 当前在运行。

> **注意**：A/A 测量期间 governor 被切到 `performance` 并在结束后恢复；当前是 `schedutil`。任何后续正式测量都必须重新冻结并复测频率门禁，不能直接继承本报告的 A/A 带。

### B.2 Governor / cpuidle / 噪声（R）

- `intel_pstate=passive`（`driver=intel_cpufreq`）；20 CPU 全部 `schedutil`；`scaling_min=1200000`、`scaling_max=3500000`；`no_turbo=0`、`max_perf_pct=100`、`min_perf_pct=34`；无 `energy_performance_preference`（`[L] §7.2`）。
- cpuidle：`intel_idle` + `menu`；空闲 C6 93–99%，负载核 C1/C6=0（`[L] §7.2`）。
- 环境审计做过临时 governor/上限切换并恢复；结束时无遗留 `yes`/`turbostat`/`perf`/`sudo` 进程（`[L] §7.2`）。
- 噪声源：zcode-cli 3.5%、node 1.2%、fprintd 0.2%；`tuned 'balanced'`；`irqbalance` active（实测存储 IRQ 可能迁移）；docker/containerd/abrt/mcelog；`dnf-makecache.timer`；内核 `tainted=4`（启动期 mcelog MSR 0x17f 写入，非本次审计造成）；功耗计数不可信（`[L] §7.6`）。
- `ENVIRONMENT_BLOCKED` 触发条件：频率门禁复测失败、决定性 PMU 事件缩放、权限/凭据不可用、介质/背压不可控、介质与声明不符、cold 格不可构造、噪声/温度异常（`[L] §7.7`）。任一条出现，正式实验必须判 `ENVIRONMENT_BLOCKED`，不得降级为 GO/NO_GO。

### B.3 PMU / eBPF 能力（R）

| 项 | 事实 |
|---|---|
| paranoid | `kernel.perf_event_paranoid=2`（本会话复核） |
| 可用事件（受控负载实测返回计数） | `cycles`、`instructions`、`task-clock`、`context-switches`、`cpu-migrations`、`cache-misses`、`branch-misses`、`bus-cycles`、`ref-cycles` |
| 无缩放验证 | 8 事件（含 bus/ref-cycles）在 `taskset -c 0` 的 5M 循环上无缩放；ref-cycles 46.6e9 ≈ TSC 2.89 GHz；IPC≈2.38 |
| 多路复用 | 12 事件强制测试出现缩放：cycles 50.06%、instructions 60.07%、cache-references 60.22%、bus-cycles 39.78%（39.78–60.22%）；4 事件无缩放 |
| 不支持事件 | `stalled-cycles-frontend/backend` 返回 `<not supported>` |
| 权限 | paranoid=2 下非特权只能计自身用户态事件；CPU-wide/kernel/MSR 需 root；sudo 可用，已成功计 `msr/aperf`、`msr/mperf`、`msr/tsc`、`msr/cpu_thermal_margin`、`power/energy-pkg` |
| 纪律 | 正式结论只用**无缩放**事件集；决定性指标出现任何缩放 → 该 run `NOT_POWERED` 或 `ENVIRONMENT_BLOCKED` |

来源：`[L] §7.3`。

eBPF（`[L] §7.4`）：BTF 存在（7,023,892 B，`CONFIG_DEBUG_INFO_BTF=y`）；bpftool feature probe（root）显示 `bpf()`、JIT、kprobe/tracepoint/perf_event/sched_cls/xdp/cgroup_* 等可用；`unprivileged_bpf_disabled=2`；tracefs 挂载于 `/sys/kernel/tracing`；实测 tracepoints 含 `sched:sched_switch`、`block:block_rq_issue/complete`、17 个 io_uring（含 `io_uring_submit_req`、`io_uring_queue_async_work`、`io_uring_complete`、`io_uring_cqring_wait`）；futex 无独立 tracepoint class，只有 `syscalls:sys_enter/exit_futex`；uprobe/kprobe 已验证可 attach/删除。

### B.4 测试介质（R；现状 V）

| 介质 | 事实 | 使用注意 |
|---|---|---|
| 根 FS / `/home` | `/dev/sda3` btrfs（subvol `/root` 与 `/home`），`rw,relatime,seclabel,compress=zstd:1,ssd,discard=async,space_cache=v2`；btrfs device stats 全 0；8 MiB 写测试成功并清理 | btrfs + SATA SSD；`compress=zstd:1` 参与 I/O 路径，必须在冻结协议中固定 |
| `/tmp` | tmpfs，31.3G 可用；8 MiB 写测试成功并清理 | W-02/W-03 默认 `SLUICE_BENCH_TMP_DIR=/tmp/sluice-bench`（`bench_common.hpp:437`）；tmpfs 结果不能外推为磁盘 I/O |
| `sda` | 447.1G SATA SSD（rotational=0，GS-480），scheduler `bfq` | 根 FS 所在设备；存储 IRQ 可能被 irqbalance 迁移 |
| `sdb` | 3.6T SATA HDD（rotational=1，ST4000HKVS002），scheduler `bfq` | 未用于本战役；冷介质需单独冻结 |
| `nvme0n1` | 476.9G NVMe，未挂载，scheduler `none` | 当前不可直接作为测试介质；挂载需 root 与显式介质决策 |
| clocksource | `tsc` | 与 perf 时间基准一致；记录到 meta |

来源：`[L] §7.5`；现状 `[V-B1]`。**P-E 的介质限制**：P4 的 sluice-copy copy 期间 `block:block_rq_issue/complete` 各 202（空闲 2 s 为 8/10），`fincore` 显示 src 256M/65,536 pages cached；因此不能声称 P4 的 P-E 是严格 0 block 的 page-cache-only profile，wait/progress 份额可能含少量 IO/写回等待（`[P4W] §5.3`、`[P4W] §8.11`）。

### B.5 A/A 环境与“频率”判词（R + 本会话复算 V）

- A/A 在 `performance` governor、`no_turbo=0`、`taskset -c 2,3`、tmpfs、page-cache resident 下运行；AB/BA 严格交替；每格 16 对（`[P1] §1.1`、`[P2A] §2.4`）。
- 全部 11 格、352 arm、**1,056** 个 perf event 行（每格 96 行：32 arm × 3 个事件；P1 6 格 576 行 + P2 5 格 480 行）均为 `percent_enabled=100.00`，没有 `<not counted>` / `<not supported>` / `scaled`；**没有 perf 多路复用缩放**（`[P1] §2.3`、`[P2A] §2.3`；本会话复算 JSON 的 `perfAudit.scaledRows=0`）。
- **C3/C3b/C7 的 `ENVIRONMENT_BLOCKED` 不是 perf 缩放**：P1 C3 CPU CV 5.872% vs cycles CV 1.637%、instr CV 0.111%；P2 C3b CPU CV 4.230%、cycles CV 1.073%、instr CV 0.086%；C7 CPU CV 7.121%、cycles CV 1.584%、instr CV 0.017%。预注册判据（CPU CV > 2× cycles CV 且 instr CV <0.5%）机械触发（`[P1] §3.3`、`[P2A] §4.2`）。
- **机制更正**：P2 补测复现了统计触发，但**没有复现“持续频率 excursion”**。C3b 启动后 CPU2/3 中位频率 3.436–3.437 GHz，4,556 个启动后样本仅 1 个 <2.5 GHz（2.423 GHz，约 0.02%），within-run CV 1.09%；C3b 的 CPU 变异主要由 `cpu_sys_s` 解释（sys CV 5.97%、user CV 3.47%、cycles CV 1.07%；配对 ΔCPU 与 Δsys 相关 0.983）。C7 同模式（sys CV 10.50%、user 4.43%、cycles 1.58%；ΔCPU/Δsys 相关 0.992）。正确表述是“**CPU/wall 不稳定，§1.4-7 触发；主导成分是 sys time，频率 excursion 未被直接观测复现**”，不是“已证实 5–7% 频率下降”（`[P2A] §4.3–4.4`、`[P2A] §7.2`）。
- C7 的 CPU/wall A/A 均值不正：CPU +5.043%（bootstrap CI [0.46, 9.60]，t CI [−0.17, 10.26]）、wall +2.542%（bootstrap CI [0.28, 4.85]，t CI [−0.05, 5.13]）；AB/BA 与位置检验均不显著；cycles 是唯一居中的指标（mean −0.489%，CI 含 0）。因此 C7 CPU/wall 不能作为无偏基线（`[P2A] §3.1`、§7.1）。
- 环境元数据缺口：频率只有 per-arm min/max/last（20 ms 采样，无完整序列）；IRQ 亲和性只有冻结快照、无 per-IRQ；无每 pair 的 ps guard，只有 cell 级 loadavg；`saturating` 未记录；C1 的 warm/cold 未显式记录（`[P1] §2.4`、§7.11；`[P2A] §2.4`）。这些不推翻判词，但限制“环境完全受控”的强度。

**本节状态**：频率门禁 = **R（PASS）**；PMU/eBPF 能力 = **R**；介质 = **R + V（现状只读）**；A/A 环境 = **R + V（复算）**；当前 governor = **V（schedutil，非测量态）**；完整环境复测 = **U**。

---

## C. Execution & Cost Map

> 本节是 **profile 归因**，不是 clean measurement、不是 A/B、不是收益预测。所有份额都带“形态 + 分母 + 不可相加”口径（`[P2M] §1`）。

### C.1 五种形态与分母（R）

| 形态 | 入口 | workload / 分母 | 介质 / 备注 |
|---|---|---|---|
| P-A | W-02 io_uring d16 | 540,000 ops | tmpfs，page-cache resident |
| P-B | W-02 io_uring d1 | 540,000 ops | 同上；poll 1/op、eventfd read 1/op、wait_one 1/op |
| P-C | W-03 io_uring d16（split-owner，app-owned epoll） | 540,000 ops | 同上；driver `epoll_wait`，submitter condvar |
| P-D | W-02 ThreadPool d16 w4 | 540,000 ops | 同上；4 worker + main |
| P-E | `sluice-copy` 4k/d4/w1（`--sync none --no-atomic`） | 65,536 chunks / 131,076 requests | btrfs warm page cache → tmpfs；参考应用，不是 G0 |

来源：`[P2M] §3.1`、`[P2M] §5`；P4 复核了同样的形态（`[P4W] §1.3`）。PMU 共 198 条 stat 行，全部 `percent_enabled=100.00`；15/15 个 record `Total Lost Samples: 0`（`[P2R] §0`、`[P2M] §3.1`）。

### C.2 执行 DAG（R）

```text
W-02 io_uring（P-A/P-B）
app/bench thread
  submit_read -> RequestCore::reserve -> RequestCore::accept            [user; admission]
    -> UringAsyncBackend::dispatch_after_accept
       -> dispatch_one_locked -> get_sqe / prep_sqe -> router/ledger    [user+kernel; dispatch]
       -> submit_transport_locked -> io_uring_submit
          -> sys_enter_io_uring_enter -> io_submit_sqes                 [kernel; submit]
    -> kernel io_uring: io_uring_submit_req
       -> io_uring_queue_async_work (tmpfs: 1/op)
          -> iou-wrk: io_worker_handle_work -> io_read
             -> shmem_file_read_iter -> rep_movs_alternative            [kernel worker; payload/FS]
          -> io_cq_unlock_post -> CQE                                   [kernel; reap/CQE]
    -> wait path: AsyncIoContext::wait_one
       -> run_progress_pass_ -> poll_progress
          -> ProgressSource::wait_if_unchanged -> poll(notification_fd)
             -> libc __poll -> __x64_sys_poll / do_sys_poll             [kernel+libc; poll boundary]
          -> eventfd signal / read / drain                              [kernel; progress/wait]
    -> reap_cqes -> handle_one_cqe
       -> find_live_router_cookie_ (O(R) reverse scan)                  [user; M-53]
       -> finalize_operation_terminal_ -> offer_terminal
          -> release_execution                                          [user; terminal/release]
       -> publication_pending -> publish_one
          -> begin_publication -> complete_publication                  [user; publication]
          -> deliver_event -> observer take/consume                     [user; observation]

W-02 ThreadPool（P-D）/ sluice-copy（P-E）
app thread
  submit_read / submit_write -> RequestCore::reserve/accept              [user; admission]
    -> ThreadPoolBackend::dispatch_ -> work_mtx_ push                   [user; dispatch]
    -> poll_progress / run_progress_pass_                               [caller; progress/wait]
  worker thread (worker_loop)
    -> work_cv_.wait(work_mtx_)                                         [futex; shared sync]
    -> pread64 / pwrite64                                               [kernel; backend exec]
    -> rep_movs_alternative / shmem / page cache                        [kernel; payload/FS]
    -> completion -> caller publish_one -> deliver_event                [user; publication/delivery]

W-03 split-owner（P-C）
driver_thread -> epoll_wait(progress fd)                                [kernel; progress/wait]
  -> poll_progress -> reap_cqes -> publish_one -> deliver_event         [driver; reap+publication]
submitter -> submit_read -> io_uring_enter -> ...                       [kernel; submit]
  -> condvar futex wait (completion)                                    [kernel+libc; shared sync]
  -> observation / take                                                 [user; observation]
```

来源：`[P2M] §2.1–2.3`；源码坐标 `src/async/async_io_context.cpp:573-705`、`include/sluice/async/detail/progress_source.hpp:190-232`、`src/async/threadpool_backend.cpp:386-400`、`apps/sluice-copy/copy_task.cpp:305-351`、`src/async/uring_transport.cpp:218-235,540-557`、`src/async/uring_backend.cpp:171`。P4 在同一形态上做了 DWARF 行级归因（`[P4W] §1.4`）。

### C.3 阶段 owner self 台账（R）

> 口径：每条样本只归一次“最深阶段 owner”；行和 = 100%。阶段名是启发式 owner，不是因果 owner；**阶段容器不是候选上界**。

| 阶段 / 桶 | P-A d16 | P-B d1 | P-C W-03 | P-D TP w4 | P-E copy |
|---|---:|---:|---:|---:|---:|
| admission（reserve/accept/submit_read） | 1.97% | 0.09% | 8.91% | 12.76% | 7.16% |
| dispatch（dispatch/queue/SQE prep） | 5.84% | 1.67% | 6.02% | 0.74% | 1.00% |
| submit（io_uring_enter/io_submit_sqes） | 21.35% | 8.80% | 12.49% | — | — |
| reap/CQE（reap/handle/router/seen） | 5.35% | 2.85% | 3.61% | — | — |
| terminal/release（offer/release/finalize） | 0.97% | 0.03% | 2.31% | 3.49% | 0.88% |
| publication（begin/complete/publish/deliver） | 1.46% | 0.07% | 2.82% | 4.13% | 3.30% |
| progress/wait（wait/poll/eventfd/drain，**不含 poll boundary**） | 5.27% | **25.09%** | 16.29% | 3.51% | **24.36%** |
| **poll boundary（libc `__poll` only）** | — | **9.16%** | — | 0.06% | **4.38%** |
| observation（take/consume/lookup） | 1.60% | 0.05% | 1.77% | 2.05% | 1.74% |
| payload/FS（非阶段：rep_movs/filemap/shmem 等） | 19.87% | 13.28% | 11.45% | 13.85% | 10.44% |
| io-wq punt（非阶段：io_wq/io_worker/task_work） | 10.23% | 6.65% | 6.02% | — | — |
| backend exec (TP)（非阶段：worker_loop/pread/pwrite） | — | — | — | 2.38% | 1.35% |
| worker-thread syscall trampoline（analysis 旧名 `startup/thread create`） | — | — | 4.61% | 13.67% | 8.87% |
| shared sync（非阶段：mutex/spinlock，无阶段 owner） | 5.10% | 7.07% | 5.43% | 13.27% | 7.86% |
| scheduler/IRQ（非阶段：EEVDF/PSI/IPI/futex wake） | 8.69% | 11.52% | 7.68% | 12.89% | 6.01% |
| kernel syscall/FS overhead（非阶段：fput/fdget/kmalloc 等） | 0.96% | 1.94% | 0.84% | 2.04% | 5.26% |
| syscall unattributed（入口/出口 trampoline，剩余） | 3.43% | 4.01% | 4.10% | 7.34% | 9.42% |
| bench harness（非阶段：run_*_phase/verify/memcmp） | 3.76% | 1.32% | 2.06% | 1.99% | — |
| other（未归类） | 4.15% | 6.40% | 3.59% | 5.84% | 7.97% |

来源：`[P2M] §3.3`（`recompute_p2_r1.py → forms[*].stage_r1`；本会话重跑一致）。行和：P-A/B/C/D/E 均 100.00%。

**P4 行级形态的阶段重算（R；口径不同，不可与 P2 逐字节等同）**：

| 阶段 | P-D' app% | P-C' app% | P-E' app% |
|---|---:|---:|---:|
| admission | 13.49 | 12.64 | 7.17 |
| terminal/release | 4.01 | 3.56 | 1.29 |
| publication | 3.73 | 4.11 | 3.63 |
| observation | 2.53 | 2.41 | 1.97 |
| progress/wait | 5.09 | 23.40 | 26.37 |
| poll boundary | 0.10 | — | 4.42 |
| submit | 0.00 | 20.80 | — |

来源：`[P4W] §3.3/§4.3/§5.3`；P-D' 的 `poll boundary 0.10`/`submit 0.00` 来自 `[P4R] §2.4(c)` 的完整 stage rows。**重要限制**：P4 复核发现这些表被截断（打印行和 94.72/90.44/91.95，而完整 JSON 行和为 100%），且 P4 把 `claim_execution` 错记为 `execution` 家族（其正确 stage 是 publication），publication 的 family/stage 比应为 68.6/50.1/76.0%（sample count）而不是 26.8/42.1/43.2%；observation 的 76.4/101.5/102.5% 是跨 stage 比值，不是 observation stage 覆盖率（`[P4R] §2.4`）。因此 **P4 的阶段/家族对比不能用于准入**；只能用于“候选 owner 在哪一行”的方向性定位。

### C.4 PMU 总账与 Top Hotspots（R）

**PMU 总账（`perf stat -r 3`，无缩放；540,000 ops / 65,536 chunks）**：

| 形态 | cycles:u/op | cycles:k/op | cycles 合计/op | instr 合计/op | task-clock µs/op | ctx-sw/op |
|---|---:|---:|---:|---:|---:|---:|
| P-A d16 uring | 2,666.7 | 12,423.3 | **15,090.0** | 10,652.9 | 4.983 | 0.339 |
| P-B d1 uring | 5,248.0 | 23,455.2 | **28,703.2** | 24,241.7 | 11.793 | 2.000 |
| P-C W-03 d16 | 3,689.3 | 16,635.6 | **20,324.9** | 12,692.1 | 6.864 | 0.606 |
| P-D TP d16 w4 | 5,728.1 | 14,658.0 | **20,386.1** | 12,167.2 | 6.356 | 0.668 |
| P-E copy /chunk | 10,875.1 | 33,606.4 | **44,481.5** | 30,908.9 | 16.183 | 2.040 |

用户/内核总账（PMU cycles）：P-A 17.7/82.3%、P-B 18.3/81.7%、P-C 18.2/81.8%、P-D 28.1/71.9%、P-E 24.4/75.6%。**内核侧始终是主体**；置信：总量 HIGH，user/kernel 拆分 MEDIUM（freq 模式、无 PEBS、record/PMU 分母不一致）（`[P2M] §3.1`、`[P2R] M5`）。

**逐形态 leaf 前 8（`analysis.json[*].cycles.leaf_top`）**：

| 形态 | #1 | #2 | #3 | #4 | #5 | #6 | #7 | #8 |
|---|---|---|---|---|---|---|---|---|
| P-A d16 uring | `rep_movs_alternative` 11.94% K | `_raw_spin_lock` 6.36% K | `entry_SYSRETQ_unsafe_stack` 3.81% K | `swapgs_restore_regs_and_return_to_usermode` 3.04% K | `shmem_file_read_iter` 2.83% K | `asm_sysvec_reschedule_ipi` 2.83% K | `io_cq_unlock_post` 2.53% K | `__memcmp_avx2_movbe` 1.88% L |
| P-B d1 uring | `rep_movs_alternative` 7.84% K | `entry_SYSRETQ_unsafe_stack` 6.22% K | `_raw_spin_lock` 5.41% K | `__raw_spin_lock_irqsave` 3.77% K | `__smp_call_single_queue` 2.10% K | `syscall_return_via_sysret` 2.06% K | `entry_SYSCALL_64` 2.05% K | `psi_group_change` 1.96% K |
| P-C W-03 d16 | `entry_SYSRETQ_unsafe_stack` 6.87% K | `rep_movs_alternative` 6.78% K | `_raw_spin_lock` 5.15% K | `__GI___pthread_mutex_unlock_usercnt` 3.96% L | `pthread_mutex_lock` 3.60% L | `native_queued_spin_lock_slowpath` 3.18% K | `entry_SYSCALL_64` 2.44% K | `syscall_return_via_sysret` 2.22% K |
| P-D TP d16 w4 | `entry_SYSRETQ_unsafe_stack` 13.29% K | `rep_movs_alternative` 9.04% K | `__GI___pthread_mutex_unlock_usercnt` 7.12% L | `pthread_mutex_lock` 5.96% L | `entry_SYSCALL_64` 4.69% K | `syscall_return_via_sysret` 4.28% K | `native_queued_spin_lock_slowpath` 2.50% K | `ThreadPoolBackend::worker_loop()` 2.24% B |
| P-E copy d4 w1 | `entry_SYSRETQ_unsafe_stack` 13.28% K | `__GI___pthread_mutex_unlock_usercnt` 7.60% L | `pthread_mutex_lock` 6.35% L | `entry_SYSCALL_64` 4.84% K | `syscall_return_via_sysret` 4.40% K | `rep_movs_alternative` 3.73% K | `run_pipelined_copy_with_context` 1.77% B | `clear_highpages_kasan_tagged` 1.48% K |

K = kernel，L = libc，B = bench/产品用户态。leaf 份额是分区；单个样本只归一个 leaf symbol；**不要**把同一行多个符号相加当作某个阶段成本（`[P2M] §4.1`）。

### C.5 W1 / W3 / M-53 / M-42 的形态级定位（R）

| 候选 | 形态级数字 | 口径/限制 |
|---|---|---|
| **W1** ProgressSource poll | P-B `__poll` inclusive **24.93%**（6,129/24,585）、`__x64_sys_poll` 15.77%（子集）、poll boundary **9.16%**（2,251）、progress/wait **25.09%**（6,168）；P-E `__poll` **15.83%**（5,500/34,736）、x64 11.46%、poll boundary **4.38%**（1,520）、progress/wait **24.36%**（8,461） | `__poll` 是 inclusive 栈份额，不是可删量；progress/wait 是混合容器（P-B 含 2,057 个 worker-eventfd + 3,572 app-poll + 297 poll+progress-pass + 159 progress-pass + 83 eventfd；P-E 含 3,972 poll+progress-pass + 3,537 progress-pass + 952 eventfd）；P-B 6,129 个 `__poll` 中 5,664 个没有 `wait_one` 帧，873 个是 4 帧栈；P-B app 样本 90.67% 没有 bench 帧 → W3/product leaf/`wait_one` **NOT_OBSERVABLE** |
| **W3** RequestCore 族 | 族级 self：P-A 6.00%、P-B 0.24%、P-C 15.82%、P-D 22.43%、P-E 13.07% | 阶段容器（admission+terminal+publication+observation），不是单候选上界；P-B 0.24% 因用户栈截断不可用；M-62 的 4.13% 是 publication 容器，不是候选上界 |
| **M-53** cookie O(R) | d16 leaf self：P-A 34/8,369=**0.406%**（Wilson 95% CI 0.291–0.567%）、P-C 47/16,472=**0.285%**（0.215–0.379%），≈61.3/58.0 cyc/op；d1/TP/copy 0；d64 **NOT_OBSERVABLE**（P2 未采集） | `61.3/58.0` 是 leaf share × PMU cycles/op；PMU/record 分母不一致、无 PEBS/skid，置信 MEDIUM/LOW；d64 的 P1-A 历史 1.74%/≈116 cyc/op 只是线索 |
| **M-42** `submit_flushes_` | `src/async/uring_transport.cpp:222` 的 `fetch_add` 编译为 `lock addq $0x1,0x120(%rdi)`（objdump `4241b7`）；按 19–20 cycles：P-A 0.126–0.133%、P-B 0.066–0.070%、P-C 0.093–0.098% cycles；1 instr/submit 0.0041–0.0094%；P-D/P-E N/A（TP 无 uring submit） | 19–20 cycles 是外部引用（Haswell L1 命中），本会话只确认指令，未重测锁延迟；算术上界 ≤0.14% cycles，低于匹配 uring 带 → `NOT_POWERED` |

来源：`[P2M] §6.1–6.5`；`[P2R] H1/H2/M2/M3/M4`；`[P3] §4.1–4.4`；`[M53] §6`。

### C.6 不可相加矩阵（R）

| 项 A | 项 B | 关系 | 规则 |
|---|---|---|---|
| 阶段 self | 阶段 inclusive | 同一调用栈的两种计数 | **不能相加**；inclusive 只说明“调用帧出现”，不是额外成本 |
| `poll_progress` 2,203 + 1,775 | union 2,205（13.386%） | 嵌套符号；both 1,773 | 只能用 union；R0 的 24.15% 是双计 |
| `eventfd_signal` 2,132 + 1,860 | union 2,132（8.672%） | mask 全部在 `io_eventfd_signal` 内 | 只能用 union；R0 的 16.24% 是双计 |
| `wait_one` 10,759 + 10,755 | union 10,759（30.974%） | 两个重载；both 10,755 | 只能用 union；R0 的 61.94% 是双计 |
| `publication` 4.13%（P-D） | `claim_execution` 0.61% | claim 叶样本 93/93 已归 publication | **不能相加**；publication 阶段容器 4.13%；M-62 的候选级可删量 `NOT_OBSERVABLE`，**不是上界** |
| 阶段 self | leaf symbol | 同一样本可同时计入 leaf 与 stage owner | **不能相加**；两种切法 |
| PMU `cycles:u/k` | record DSO kernel/lib/bench | 分母/采集方式不同 | **不能互相换算** |
| `progress/wait` 25.09%（P-B） | `poll boundary` 9.16% | 两个不相交阶段行 | 可相加得到容器 34.24%，但**仍不是候选上界** |
| `__poll` 24.93% | `__x64_sys_poll` 15.77% / `do_sys_poll` 15.38% | 后者是前者子集 | **不能相加**；上界只能取一个 inclusive 栈 |
| `wait_one` union 30.974%（P-E） | `__poll` 15.83% / progress/wait 24.36% | `wait_one` 含非 poll 路径与 progress pass | **不能当 poll 成本**；不能相加 |
| off-CPU 事件数 | cycles 份额 | 事件数 vs 采样份额 | 单位不同；**不能当时间占比** |
| raw report（未过滤） | analysis（过滤 comm） | 分母不同（如 P-A 8,390 vs 8,369） | 份额不可直接对比 |
| W1 / W3 / M-53 / M-42 | 彼此 | 同一进程 cycles 的不同子集/切法 | **不能相加** |
| sample-count 份额 | period 加权份额 | P4 的 `owner%` 是计数，`owner cyc/op` 是 period | 两者不能混用；边界候选在 period 口径下会翻转（`release_execution` P-D 1.141→0.941、P-C 1.052→0.811；`consume_public_result` P-D 0.976→1.291）（`[P4R] §2.5`） |

来源：`[P2M] §3.8`；`[P4R] §2.5`。

### C.7 P4 行级归因的可复现性与限制（R）

- P4 在 P-D'（TP d16 w4）、P-C'（W-03 d16）、P-E'（copy）、P-F（uring d64）上做了 DWARF 行级归因；三二进制 `.text` 与 P1 release 逐字节相同；`addr2line` 候选 self 行无 `??:0`（`[P4W] §2.1`、`[P4R] §4.1`）。
- **W3 主要候选的 owner-total（app 分母）**：
  - P-D'：`offer_terminal` 1.80%（311 cyc/op）、`claim_execution` 1.56%（274）、`release_execution` 1.14%（200）、`RequestBridge::consume` 1.02%、`consume_public_result` 0.98%、`reserve` 0.62%、`accept` 0.52%、`begin_publication` 0.50%、`complete_publication` 0.31%、`poll_progress` 0.37%、`wait_one` 0.37%。
  - P-C'：`offer_terminal` 1.76%（234）、`RequestBridge::consume` 1.34%、`consume_public_result` 1.30%、`release_execution` 1.05%、`complete_publication` 0.75%、`reserve` 0.67%、`begin_publication` 0.60%、`accept` 0.60%、`claim_execution` 0.33%。
  - P-E'：`wait_one` 3.44%（1,297 cyc/chunk）、`wait_if_unchanged` 2.08%（785）、`ThreadPoolBackend::poll_progress` 2.06%（775）、`run_pipelined_copy_with_context` 1.81%、`claim_execution` 1.19%、`begin_publication` 0.94%、`RequestBridge::consume` 0.88%、`offer_terminal` 0.84%、`consume_public_result` 0.83%。
- **P4 复核的硬限制**：
  1. **无 skid/PEBS**：`perf record` 无 `precise_ip`，552/553/557 的行级拆分不是精确退休指令归属；P2 M5 的降级理由没有继承（`[P4R] §2.2`）。
  2. **stage/家族算术错误**：`claim_execution` 在报告自己的 p2 classifier 中属于 publication（P-D 814/814、P-E 825/825），报告把它记为 `execution` 并写“跨 terminal/publication”；publication+execution union 应为 68.6/50.1/76.0%（sample count），不是 26.8/42.1/43.2%；observation 的 76.4/101.5/102.5% 是跨 stage 比值，不是覆盖率（`[P4R] §2.4`）。
  3. **P-E `wait_one` owner 主要是 mutex，不是 poll**：P-E app 有 10,930 个 libc `__poll` 样本，但 `wait_one` 的 callsite leaf `__poll` 只有 86（`wait_if` 同）；mutex 占 callsite 约 93.7%；poll 成本在 inclusive 31.29%/stage 26.37%，不在 owner 3.44%/2.08%（`[P4R] §2.8`）。
  4. **line 557 误读**：d64 `find_live_router_cookie_` 的 557 行 18 个样本是成功 `return i`（16）+ `ret`（2），不是 not-found return；not-found 在 line 556 且 0 样本（`[P4R] §2.6`）。
  5. **d64 vs d16 不显著**：self 168/15,874=1.058% vs 68/7,654=0.888%，z=1.23，p=0.22；owner 179/15,874=1.128% vs 75/7,654=0.980%，z=1.03，p=0.30；Wilson CI 大幅重叠（`[P4R] §2.3`）。
  6. **stage 表截断**：打印行和 P-D 94.72%、P-C 90.44%、P-F 91.95%，完整 JSON 行和 100%；报告却写“行和≈100%”（`[P4R] §2.4(c)`）。
  7. **sample-count vs period 翻转**：见 §C.6 最后一行（`[P4R] §2.5`）。
- 因此 P4 的行级地图可作为 **W3 RequestCore mutex 的方向性/机制性证据**，但**不能**作为 W3 定量准入或 M-53 终判依据（`[P4R] §6`）。

**本节状态**：P2 五个形态与阶段表 = **R + V（复算）**；P4 行级归因 = **R**；P4 的定量结论 = **R（部分被复核否定/降级）**；d64 M-53 终判 = **U**；真实消费者 profile = **U**。

---

## D. Candidate Admission Matrix

### D.1 准入规则与计数（R）

`ADMIT_TO_AB` 必须同时满足五条件（`[P3] §1.1`）：

1. 合法机制：不违反 PROG-01–04、CANCEL-01、BACKEND-01 等，且不需要未裁决的拓扑变更；
2. 候选级 measured hotspot（或等价证据），不是族级线索/阶段容器；
3. 语义可证：确定性 oracle、mutant、协议/模型证明或可执行的步间语义映射；
4. A/A 分辨率足以在预算内分辨预注册 MME；
5. 无历史反例。

**修订后计数（共 38 行）**：

| 裁决 | 计数 | 说明 |
|---|---:|---|
| `ADMIT_TO_AB` | **0** | 没有候选同时满足五条件 |
| `ADMIT_TO_DESIGN` | 11 | 其中 8 行是 `INCONCLUSIVE + design work`（设计/资格工作），不是原终态改写 |
| `SEMANTIC_MISMATCH` | 11 | W1 包、W1-D、W1-C1、W1-A4、W1-A5–A8、M-16、M-09、M-30、M-57、M-12、M-80 hygiene |
| `NEEDS_OWNER` | 1 | W1-C2（契约允许 condvar，但需 owner 授权第二通知通道） |
| `NO_CHANGE_JUSTIFIED` | 2 | W1-A1/A2/A3、W1-B 有界等待 |
| `NOT_APPLICABLE` | 2 | W2、W5 |
| `NOT_POWERED` | 2 | W6 包、M-42 |
| `INCONCLUSIVE` | 4 | M-61 `begin`/`complete`、M-80 `write_all_at`、M-37、M-01 |
| `NO_GO` | 4 | M-61 payload、H1、io-wq punt、tp-w8 |
| `NO_GO_ABORT` | 1 | M-28（**不是** `NOT_APPLICABLE`） |
| **合计** | **38** | 0+11+11+1+2+2+2+4+4+1 |

来源：`[P3] §0.1`；`[P3R] §2` 复核了 W1-C2 的 `NEEDS_OWNER` 与 M-28 的 `NO_GO_ABORT`，并指出 P3 原稿的 C5b/A-A、M-62 4.13% 容器表述、M-28 状态已在 R1 修正。

### D.2 W1–W6 终态看板（R）

| 工作包 | 名称 | 阶段 | 终判 / 当前裁决 | 说明与下一步 |
|---|---|---|---|---|
| **W1** | ProgressSource poll 等待路径 | **owner 点开放**（不是终判） | `SEMANTIC_MISMATCH`（D1/D2/D3/C1）/ `NEEDS_OWNER`（C2）/ `NO_CHANGE_JUSTIFIED`（A1/A2/A3/B/A4'） | D1/D2/D3/C1 语义阻断；C2 契约允许 condvar，但需 owner 授权第二通知通道；A1/A2 不 material；`#604` G0 未过；等 O-1/O-2/O-3，不启动 A/B |
| **W2** | 通知/drain/rearm | 终判 | `NOT_APPLICABLE` | `#635` 已合并；无新增候选；M-67 按 ledger §5.2 路由到 W3，不是 W2 新候选；未来改动保持 9 用例语义 oracle + mutant + 模型 |
| **W3** | RequestCore 行级族 | 语义验证中 | `ADMIT_TO_DESIGN`（包级）；成员 M-61 为 `INCONCLUSIVE`，M-62/M-67/M-10 为 `INCONCLUSIVE + design work` | 在 P4 方向性行级归因之后，补候选级可删量与步间语义（按 P-D/P-C/P-E 主形态）；M-61 R3 反例未推翻；A/A 必须按形态匹配（P-D 的 C2b 是 NOISY）；预算 M |
| **W4** | UringAsyncBackend | 语义验证中 | `ADMIT_TO_DESIGN` | M-53 先证三项迁移义务 + d64 cycles-only A/A（17–20 仅 δ=2%）；M-40 设计门；M-63/M-65 owed 家族设计门；预算 M/L |
| **W5** | ThreadPool owed-count | 终判 | `NOT_APPLICABLE` | `#634` 已合并；任何改动重跑 parity 8 用例 + 4 mutants + 完整矩阵 |
| **W6** | test-only 计数 / owed 计数 | 终判 | M-42 `NOT_POWERED`；M-28 `NO_GO_ABORT`（不重做）；M-40 转 W4 | M-42 的 `GO` 是排队，不是 A/B 准入；隔离格 + 动态回执带 + 机制门 + mutant kill + release 无 seam 符号前不启动；M-28 不是 `NOT_APPLICABLE` |

来源：`[P3] §2`；本会话只读复核了 `[P3R] §6` 的结论（`ADMIT_TO_AB=0` 成立；W1 语义阻断、M-42 `NOT_POWERED`、W2/W5 `NOT_APPLICABLE` 成立；W3 的 A/A/预算、M-62 的 4.13%、M-28 的状态已在 R1 修正）。

### D.3 W1 方向级终态（R）

| 方向 | 裁决 | 关键依据 | 下一步 / 预算 |
|---|---|---|---|
| **D1** 同 fd + `dup` + `F_SETFL` | `SEMANTIC_MISMATCH` | `dup` 共享 open-file description 与文件状态标志；`[W1D] §10.1` 的 eventfd 演示输出 `after clear on dup: original False dup False`；源码只创建一次 eventfd，flags 为 `EFD_NONBLOCK` 与 `EFD_CLOEXEC`（`progress_source.hpp:75`）；`BackendProgressPort` 禁止 backend dup/store fd（`:539-545`） | 不实现；0 A/B |
| **D2** 同 fd 直接改 blocking | `SEMANTIC_MISMATCH` | `drain_notification_nolock_`（`:414-460`）在无 readiness 时会永久阻塞；`acknowledge_notification` 与非阻塞 ack/drain 依赖 `EFD_NONBLOCK`；有界等待（P-B 20 s，`bench/bench_w02_pipeline.cpp:52`）需要 timeout；PROG-02 第 2 条要求 stale readiness 在最终 check 前确认 | 不实现；0 |
| **D3** 独立 blocking fd | `SEMANTIC_MISMATCH` | io_uring 只把 kernel CQE readiness 写给被注册的一个 fd（`src/async/uring_backend.cpp:273-278`）；host pollable handle（PROG-03）与 owner blocking fd 若不同，必有一方收不到 CQE；relay/第二通知权威违反 PROG-01/02 | 不实现；0 |
| **C1** 第二等待 fd | `SEMANTIC_MISMATCH` | 同上；kernel 只唤醒被注册 fd；relay/第二权威 | 不实现；0 |
| **C2** TP condvar + protected predicate | **`NEEDS_OWNER`** | PROG-02 明确允许 “condition variable with a protected predicate”（`docs/explicit-io-contract.md:907-912`）；但需要双通道、新模型、新 oracle，eventfd 仍留给 host | owner O-2；若批准，M/L 设计包 |
| **C3** epoll/timerfd/relay thread | `NO_GO`（本战役范围） | epoll 仍需 read；timerfd 增加 fd/syscall；relay 是新执行资源和第二通知权威 | 不实现 |
| **A1/A2/A3** 合并入口锁 / health 并入 fused block / deadline 算术 | `NO_CHANGE_JUSTIFIED` | 只删 1–2 次 mutex/少量 clock/int；不删 syscall/kernel work；预计低于 C6/C1 噪声（C6 CPU MDE80 0.747%、C1 CPU 0.589%，16 对） | 可选 hygiene PR，不得开 A/B 声称收益 |
| **A4/A4'** probe 降频 / TP-only 跳过 | 全局 `SEMANTIC_MISMATCH`；TP-only `NO_CHANGE_JUSTIFIED` | probe 是 io_uring kernel writer 的兜底（CQE 不 bump epoch）；TP-only 跳过需新能力事实，收益仅一个虚调用 | 不全局降频；TP-only 仅 hygiene |
| **A5–A8** 删除第一次重验 / P3 / drain / poll 后检查 | `SEMANTIC_MISMATCH` | 分别丢唤醒、并发 drain 窗口丢唤醒、PROG-02 第 2 条、control/POLLNVAL 破坏 | 不做；0 |
| **B** 有界等待算术 | `NO_CHANGE_JUSTIFIED` | poll 仍在；P-B 本身是 20 s 有界等待；只删 clock/int | 不单独作为候选 |

来源：`[W1D] §3–§4`；`[P3] §3.1`；`[P3R] M6`（C2 改 `NEEDS_OWNER`）。**计数注**：C3 是 W1-C 的子方向，不单独计入 D.1 的 38 行；W1 包的 `SEMANTIC_MISMATCH`/`NO_CHANGE_JUSTIFIED` 计数不变（`[P3] §3.1`）。

### D.4 `#637` 全量终态（R）

> **规则**：终态按 `#637` 最终归档（`e2f41acb`）原样记录；本战役不重新裁决。`NO_GO`/`NO_GO_ABORT` 无新证据不得复活；`SEMANTIC_MISMATCH` 先走 owner 裁决；`INCONCLUSIVE` 必须满足重开条件才可进入下一轮（`[L] §4` 规则 1–5）。

| # | 候选（`#637`） | 原终态 | 本轮终态（2026-11-18） | 翻案 / 重开条件 | 归属 / 预算 |
|---:|---|---|---|---|---|
| 1 | **M-28** `syscall_count_` 宏门控剔除 | `NO_GO_ABORT` | **`NO_GO_ABORT`**（不是 `NOT_APPLICABLE`；`syscall_count_` 仍在生产：`threadpool_backend.cpp:434`、`detail/threadpool_backend.hpp:258`） | 四要素同时满足：① 新证据把机制约束为可分离的“单指令差”；② 消除/隔离 TU 重编译布局变化、成员移除的对象布局变化、RMW 移除对 `work_mtx_` 竞争时序的影响；③ 新 session、新冻结；④ 动态回执自带接受带与隔离格判据 | `#621`；0 |
| 2 | **M-80** `write_all_at` → `prevalidated_pwrite_step` | `INCONCLUSIVE` | **`INCONCLUSIVE`** | 直呼组合写形态的 measured hotspot + “compose 仅由已校验入口进入”不变量 → 单因素 A/B | `#621`/`#604`；M |
| 3 | **M-16** uring 后端层 state-op 校验收敛 | `INCONCLUSIVE` | **`SEMANTIC_MISMATCH`** | owner 裁决 seam 冲突；隔离 backend 层 state-op 半边 → 单因素 A/B | `#621`；S/M |
| 4 | **M-37** `remove_exact(front)` → `pop_front` | `INCONCLUSIVE` | **`INCONCLUSIVE`**（`#595` 归属；本战役不启动） | 深队列派发形态的 `memmove` 份额，或证明生产全形态队列深 ≤1（→ `NO_GO`） | `#595`；M |
| 5 | **M-53** cookie O(R) 逆向扫描 → `user_data` 路由索引 | `INCONCLUSIVE` | **`ADMIT_TO_DESIGN`** | O1/O2 已证；O3 的 packed 编码 8 项实现 + HEAD envelope tests；d64 cycles-only A/A；δ=2% 17–20 对，δ=1% 58 对；若 d64 效应接近 0.4% 的乐观假设，则需 172/336 对（不是 d64 实测效应） | `#621`；M/L |
| 6 | **M-63** uring owed sweep 逐槽锁 → 单锁快照 | `INCONCLUSIVE` | **`INCONCLUSIVE` + design work** | 零长/owed 协议激活 + 交付顺序不变性 → 单因素 A/B；owed 机制本体 KEEP | `#621`；M |
| 7 | **M-09** `Slot::borrow` 只写不读 | `SEMANTIC_MISMATCH` | **`SEMANTIC_MISMATCH`** | owner 裁决：修 root 摘除 borrow facts（→ 删除 PR）或出现真实消费者（→ KEEP） | `#621`；S（无性能门） |
| 8 | **M-30** `zero_effect_cancel` 零生产者 | `SEMANTIC_MISMATCH` | **`SEMANTIC_MISMATCH`** | owner 裁决 seam 义务（KEEP）或修 root 去掉冗余第二强制点并迁移 seam 测试 | `#621`；S |
| 9 | **M-57** `acquire_execution` 生产零调用 | `SEMANTIC_MISMATCH` | **`SEMANTIC_MISMATCH`** | owner 裁决 seam 必要性；若可删走代码面收缩 PR（无性能门） | `#621`；S |
| 10 | **M-12** `close_admission` 双实现 | `SEMANTIC_MISMATCH` | **`SEMANTIC_MISMATCH`** | owner 指定存活实现 + 测试 oracle 迁移方案 | `#621`；S |
| 11 | **M-42** `submit_flushes_` 只写不读原子计数 | `GO`（排队，未启动） | **`NOT_POWERED`** | measured hotspot；隔离格（submit-only）+ 动态回执带 + 机制门七问 + mutant kill + release 无 seam 符号；出现生产读者 → `NO_GO`/重新物化；`GO` 排队 ≠ A/B 准入 | `#621`；S + 隔离格 |
| 12 | **M-40** `live_cookies_`/`live_control_sqes_` 原子冗余 | `INCONCLUSIVE`（r2 由 `NO_GO` 改判） | **`INCONCLUSIVE` + design work** | 行级 measured 归因 + 生产读者角色 + `dispatch_mtx_` 覆盖重证 + 缝读者加锁设计 → 单因素 A/B | `#621`；M |
| 13 | **M-61** `PublicationPayload` 快照仅测试可达 | `NO_GO` | **`NO_GO`** | 生产 payload 消费者出现（→ 重新物化）或 owner 批准快照下沉测试专用通道（卫生路线，非性能门） | `#621`；N/A |
| 14 | **M-01** scope O(S) 扫描，H3 开放 | `INCONCLUSIVE` | **`INCONCLUSIVE`** | `-scope64` 高重复隔离协议剔除驱动污染 + 机制可归因份额；free-list 是否重开属 owner（S2） | `#621`；M |
| 15 | **M-10** 零长发布三锁融合 | `INCONCLUSIVE` | **`INCONCLUSIVE` + design work** | zero-op 占比测量（T9）+ 步间语义映射 + `publication_inflight`/slot reuse/observer 注册不变 → 融合单因素 A/B | `#621`；M |
| 16 | **M-61** `begin`/`complete` 两锁融合 | `INCONCLUSIVE` | **`INCONCLUSIVE`**（r6 R3：0.45% < 1% MME，已拒绝） | 新 measured profile 推翻 R3（含 `publication_inflight` 反例与确定性测试注入点）+ 保步间语义的融合单因素 A/B（族 A/A 先行） | `#621`；M |
| 17 | **M-62** 无观察者跳过 claim/retire | `INCONCLUSIVE` | **`INCONCLUSIVE` + design work** | 新 measured profile 行级分离 + publication exactly-once/observer 注册协议重证 + `publication_inflight`/slot reuse 不变 → 单因素 A/B；4.13% 是 publication **容器**，不是候选上界 | `#621`；M |
| 18 | **M-65** owed `release_control` 第三锁融合 | `INCONCLUSIVE` | **`INCONCLUSIVE` + design work**（W4 owed 家族） | zero-op/owed 协议（T9）+ 第三锁与 retire 融合的步间语义 + 交付顺序不变性 + `publication_inflight`/slot reuse/observer 注册不变 → 单因素 A/B | `#621`；M |
| 19 | **M-67** sticky 双标志原子化/融合查询 | `INCONCLUSIVE` | **`INCONCLUSIVE` + design work** | 行级 profile 分离 + A/A 分辨率 ≥1% MME + ProgressSource 单读者协议重证 + 跨方法临界区合并的步间语义 → 单因素 A/B | `#621`；M |
| 20 | **M-33** TP cancel ring 摘除惰性失效 | `INCONCLUSIVE` | **`INCONCLUSIVE` + design work** | cancel 形态 measured hotspot（T9-cancel）+ 失效机制设计（两语义保持）+ 机制正当性门 → 单因素 A/B | `#621`；M |
| 21 | **M-74** `ready()` 快路径合并 | `INCONCLUSIVE` | **`INCONCLUSIVE` + design work** | ready 概率分层测量 + 合并收益隔离证据 → 单因素 A/B | `#621`；S/M |
| 22 | **M-80 hygiene** `file_semantics` 模型词表位置 | `SEMANTIC_MISMATCH` | **`SEMANTIC_MISMATCH`** | owner 裁决词表移入测试面（保持语义 oracle 共享）或出现生产消费者 | `#621`；S |
| 非 1 | **H1** 提交批形态（per-dispatch flush） | `NO_GO`（本轮） | **`NO_GO`** | `#595` RQ1 second-held-entry guard + payload gate + D1/D2/D3 + 64 KiB + W-03 + cancel falsifier 家族全过 + 整机重测（R1–R5 原始数据未归档） | `#595`；N/A |
| 非 2 | **io-wq punt** FS 条件性 | `NO_GO` | **`NO_GO`** | 无翻案路径（非 Sluice 代码）；换设备/FS 的产品准入属既列重测情形 | `#604`/`#617`；N/A |
| 非 3 | **tp-w8** `work_mtx_` 竞争（worker 超配） | `NO_GO` | **`NO_GO`** | 出现 worker 数被外部固定的 root 工作负载 + 最小机制提案过机制正当性门 → 单独预注册；唯一减法交集 M-28 已 `NO_GO_ABORT` | `#604`；N/A |

来源：`[L] §4`（终态原文）；`[P3] §3`（本轮裁决）；`[P3R] H1/H2/H3/M1`（修正）；`[M53] §4–§6`（M-53 义务与预算）。

### D.5 本轮没有新增候选

- 本轮没有产生任何新的生产机制、候选 build、候选 SHA 或 A/B 队列项。
- 唯一“新”的事实是 **M-53 的 O1/O2 已证、O3 未证**，以及 **P4 的 d64 点估计**（1.06% self/1.13% owner app；不显著、低于 C7 MDE）——两者都只把 M-53 从“无 d64 数据”推进到“有可复现点估计但不足终判”，没有改变 `ADMIT_TO_AB=0`。
- `#637` 的 `M-28` 仍 `NO_GO_ABORT`；`M-42` 仍 `NOT_POWERED`；`M-61 payload`/H1/io-wq/tp-w8 仍 `NO_GO`；语义冲突批仍待 owner。

**本节状态**：W1–W6 看板 = **R**；`#637` 全量终态 = **R + V（Git/源码坐标抽查）**；`ADMIT_TO_AB=0` = **R + V（本轮复算与源码核对）**。

---

## E. Semantic Evidence

> **核心纪律**：本战役**没有重跑任何语义 oracle、mutant 或 TLA+ 模型**（M-53 的三个测试是既有二进制，不是语义 oracle）。因此不能写“语义全过”；只能说“已合并证据记录为 X；本轮未重跑”。语义 FAIL 即 `NO_GO`（`[L] §9.1.4`）；**没有 FAIL 不等于 PASS**，只意味着没有候选进入语义门。

### E.1 已合并的语义证据（I；未重跑）

| 工作 | 语义证据 | 本轮状态 |
|---|---|---|
| `#634` ThreadPool owed-count | `tests/threadpool_owed_counter_parity_test.cpp` 8 用例（counter↔scan↔sink、zero-op、slot reuse、cancel、concurrent producers）；`xmake/tests.lua:518-537` 注册 parity + 4 mutants；权威 `any_event_owed_locked_()` 扫描保留 | **I**：产品代码/测试在 BASE；本轮未跑 parity/mutants |
| `#635` ProgressSource 单读 drain | `tests/progress_source_drain_semantic_oracle_test.cpp` 9 用例（signal/ack/rearm、coalesce、prerevalidate/prepark、stale ack 后 signal、retire、publication exactly-once、borrow/re-drain、cancel/shutdown settle）；mutant `progress_source_mut_drain_success_keeps_pending`；`ProgressSource.tla`（`InvSafePark`、`LParkWakes`、`MutParkDrain`、`MutNoProbe`） | **I**：产品代码/测试在 BASE；证据目录不在 BASE；本轮未跑 oracle/mutant/TLC |
| `#609` ELIDE | 终态 `E5_MECHANISM_VERIFIED / END_TO_END_WIN_NOT_DEMONSTRATED`；四个 native W02 格在噪声内 | **I**：历史结论；未重跑 |

来源：`[L] §3`、`[L] §8.1`（`:448`）；`[P3] §4.7`。

### E.2 W1 的语义义务与阻断（R）

`[W1D] §2` 从 root/模型整理出 10 条义务（O1–O10），每个方向都必须逐项回答：

| ID | 义务 | 当前机制 | 证据 |
|---|---|---|---|
| O1 | no-lost-wake：任何创建 actionable work/control 的 transition 必须留下持久 readiness，或与 sleep handshake 安全竞争 | epoch bump + `notification_pending_` 抑制写 + eventfd 持久 readiness；pre-park 重验 + drain 同一 `mtx_`；post-probe P3 | PROG-02 `:865-893`；`progress_source.hpp:153-192`；model `InvSafePark` `:713-719` |
| O2 | stale readiness 必须在最终 actionable-work check 之前被确认 | fused block 先 control/epoch 检查，再 drain；drain 在锁内 | PROG-02 第 2 条 `:881-883`；`progress_source.hpp:153-166`；model `OL4a` `:297-320` |
| O3 | signal before/during/after park 都不能丢 | before：epoch 变化被 pre-park check 捕获，或 drain 后 writer 再写 fd；during probe：P3 捕获 epoch 或 fd 保持 ready；during poll：fd 唤醒；after poll：post-poll 检查 | `progress_source.hpp:153-230`；semantic oracle `:117-170` |
| O4 | kernel producer 窗口：io_uring CQE 不 bump userspace epoch，只写 fd；drain 可能消费它 | `PhysicalProbe` 在 drain 之后、P3 之前读 CQ；probe 报 work 则返回 progress，不 park | PROG-02 `:907-912`；model `KernelComplete` `:477-488`；`uring_backend.cpp:285-293` |
| O5 | control/cancel/shutdown：interrupt sticky，直到 observe+ack；request_stop 唤醒 owner；shutdown 不丢 accepted work | control epoch 三代 + fd write；wait 返回 interrupted 前先检查 control；shutdown 的 wait 循环用同一 probe | PROG-04 `:937-949`；`progress_source.hpp:234-250,380-386`；`async_io_context.cpp:511-550,627-629,669-673` |
| O6 | fd close/reuse：borrow 生命周期；retire 只能在无 waiter 时；旧事件不得路由到新 context | `retire_notification` 关闭 fd；`POLLNVAL` terminate；host interest fail-fast；V23/shutdown test 复用 fd | PROG-03 `:932-935`；`progress_source.hpp:216-222,284-290`；`async_io_context.cpp:204-207,720-747`；`shutdown_lifecycle_test.cpp:588-639` |
| O7 | bounded deadline：单调时钟；deadline 不 cancel I/O；nap 到期只再跑 pass，caller deadline 才返回 `deadline_expired`；不早退 | `park_deadline`；ceil ms；`OTimerNap` vs `OTimerCaller`；final pass | PROG-04 `:937-949`；`progress_source.hpp:111-147`；`async_io_context.cpp:600-607,644-691`；model `:351-386` |
| O8 | host borrow/ack：fd 只注册；drain/rearm 由 source-level ack；fd 不是第二 drain authority | `progress_notification_fd` 标记 interest；`acknowledge_progress_notification` 调 source drain；`detach` 退役 interest | PROG-03 `:914-930`；`async_io_context.cpp:720-747`；`progress_source.hpp:274-277,524-545` |
| O9 | suppression/coalescing：只有 pending unread wake 且 readiness 仍 asserted 时才能抑制物理写；EAGAIN 视为已 pending | `notification_pending_`；`wake_notification_nolock_` 跳过 pending；EAGAIN 设置 pending | PROG-02 `:869-877`；`progress_source.hpp:466-489` |
| O10 | 单一 drain authority：成功 read 是 drain 线性化点；kernel 只拥有计数器 | `drain_notification_nolock_` 在 `mtx_` 内成功读后清 pending；`acknowledge_notification` 唯一 public drain | PROG-03 `:920-922`；`progress_source.hpp:414-460,274-277`；`[L] §2.5` |

**W1 语义阻断的实质**（`[W1D] §3.4`、`[P3] §4.1`）：

1. **D1（dup + `F_SETFL`）不可行**：`dup` 共享 open-file description 与文件状态标志。`[W1D] §10.1` 的 eventfd 演示输出为 `after clear on dup: original False dup False`——清 dup 的 `O_NONBLOCK` 同时清原 fd；源码只在 `progress_source.hpp:75` 创建一次 `eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC)`，`BackendProgressPort` 明确禁止 backend dup/store fd（`:539-545`）。这不是实现细节，是 fd 语义。
2. **D2（同 fd 改 blocking）不可行**：`drain_notification_nolock_` 在无 readiness 时会永久阻塞，而它被 `acknowledge_notification` 和 fused block 调用；PROG-03 要求 ack 是 plain drain、不能阻塞 host；`wait_if_unchanged` 的有界路径依赖 timeout；P-B 本身就是 20 s 有界等待（`bench/bench_w02_pipeline.cpp:52`、`:271,:384`）。blocking read 没有 timeout；若保留 pre-park drain，则 fd 仍是 blocking，drain 会被阻塞；若先 `poll(0)+read`，是 2 次 syscall，比当前 1 次 read 更差；若用 `notification_pending_` 跳过 drain，kernel writer 的 stale readiness 会让 poll 立即返回并 spin（`[W1D] §3.4 D2`）。
3. **D3/C1（独立 blocking fd / 第二等待 fd）不可行**：io_uring 只把 kernel CQE readiness 写给被注册的一个 fd（`src/async/uring_backend.cpp:273-278`）；host 的 pollable handle（PROG-03）与 owner 的 blocking wait fd 若不同，必有一方收不到 CQE；要桥接就需要 relay/第二通知权威，违反 PROG-01 单 owner、PROG-02 单 drain authority，以及 AGENTS.md “不添加 hidden blocking fallback”。
4. **A5–A8（删除重验/drain/P3/后检查）不可行**：A5 删除第一次 epoch/control 重验会让 signal 在 token 快照后、fused block 前的写被 drain 消费，owner 丢唤醒（model `OL4a`、`MutParkDrain`）；A6 删除 P3 会在 host ack 与 owner park 并发时丢唤醒（`progress_notification_suppression_test.cpp:75-133` 证明 host interest live 时仍可 `wait_one`）；A7 删除 stale drain 违反 PROG-02 第 2 条并造成 spurious wake/owner spin；A8 删除 poll 后检查破坏 control sticky 与 `POLLNVAL` fail-fast。
5. **A4 全局降低 probe 不可行**：probe 是 io_uring kernel writer 的唯一兜底（CQE 不 bump epoch；model `KernelComplete`）；删掉/降频会直接产生 `MutNoProbe` 类 lost-wake。
6. **C2 是唯一契约允许的 material 替代**：PROG-02 明确允许 condition variable with protected predicate（`docs/explicit-io-contract.md:907-912`）；但它需要第二通知通道、新模型、新 oracle，且 host 仍要 eventfd；`[W1D] §3.3 C2` 判 `NEEDS_OWNER`，不是硬语义冲突。
7. **A1/A2/A3 语义保持但不 material**：合并 `wait_one` 入口两次 `access_mtx_`、把 health 检查并入 fused block、复用 deadline 算术，只删 1–2 次 mutex/少量 clock/int；不删 syscall/kernel work；预计低于 C6/C1 噪声（C6 CPU MDE80 0.747%、C1 CPU 0.589%，16 对；36 对 0.479%/0.378%）（`[W1D] §3.1`、`§7.3`）。

**W1 的语义判词**：不是“收益不足”，而是 **material 方向在当前 root/拓扑下语义不成立**；唯一允许的 material 替代（C2）需要 owner 授权。历史 R1 工作稿的 `poll(-1)` → blocking read 5.0–5.5% cycles 只是线索，不是已证实收益（`[L] §5.1`；`[W1D] §0`）。

### E.3 M-53 的三项迁移义务（R + V）

| 义务 | 判词 | 依据 |
|---|---|---|
| **O1** TransportLedger 物理位置单调 | **PROVEN** | `TransportLedger::append` 强制 `physical_position == (last+1) % ring_entries`，并要求 sequence 严格 +1（`src/async/uring_internal.hpp:170-191`）；两个 append 点都在 `io_uring_get_sqe` 后立即用 `(sqe_tail-1)&ring_mask`（`uring_transport.cpp:196-199,688-692`）；reconcile/retire 只弹 FIFO 前缀（`:242-256,298-335`；`uring_internal.hpp:193-211`）；全仓 `io_uring_get_sqe` 只有三处，probe 在 ledger 启用前已消费（`uring_backend.cpp:240-247`）；claim 失败路径在 append 前回滚 `sqe_tail`（`uring_transport.cpp:151-158`） |
| **O2** 控制 CQE exact-submitted | **PROVEN** | 控制 SQE 消费记账在 `dispatch_mtx_` 内先于任何 CQE 处理；`mark_consumed_control_locked_` 要求 `prepared` + 精确 handle（`uring_transport.cpp:281-296`）；控制 CQE 要求 `submitted` + `live_control_sqes_ != 0`（`uring_completion.cpp:110-116`）；bit 63 标签与 `allocate_cookie_` 的 `< CONTROL_TAG` 守卫使控制/数据编码不可能碰撞（`uring_internal.hpp:45-58`；`uring_transport.cpp:526-538`） |
| **O3** cancel 定向（编码下） | **INCONCLUSIVE** | 现行 raw-cookie 编码下 cancel 定向可证（cookie 不复用、slot+generation 精确匹配、poison 保留 route；`uring_transport.cpp:559-599,620-693`；`uring_completion.cpp:117-142`）；但 M-53 的 packed 编码（H1：bit63 tag、slot15、cookie48）**不在 HEAD**：`validate_config_` 仍允许 `request_capacity` 到 `UINT32_MAX`（`uring_backend.cpp:150-159`），`allocate_cookie_` 仍在 `CONTROL_TAG`（2^63）而不是 2^48 处 fail-fast（`uring_transport.cpp:530-537`），`io_uring_prep_cancel64` 仍以 raw cookie 为目标（`:679`），operation SQE user_data 仍是 raw cookie（`:189`），router/ledger 仍存 raw cookie，`find_live_router_cookie_` 仍是 O(R) 扫描（`:551-557`），控制/poison/CQE 消费者未统一解码。H1 矩阵/patch 在旧 BASE `8fdcd99c` 上报告 envelope 7/7 通过，但本轮未运行、未应用到 HEAD |

来源：`[M53] §2–§4`。**关键反例**：不能把 SQ 物理槽位编进 `user_data` 作为 CQE 路由索引——kernel 消费 SQE 后槽位即可复用，早于 CQE；物理位置单调不能阻止槽位复用；安全索引必须是 **router slot**，其安全性来自 router residency + full identity 复核（`[M53] §2.4`）。

**测试证据（V，本会话实际运行）**：

```bash
./build/linux/x86_64/release/uring_pending_control_parity_test
# ok ... 7/7; "all uring pending-control parity tests passed"; exit 0

./build/linux/x86_64/release/backend_contract_conformance_uring_test
# "all 20 io_uring backend conformance tests passed"; exit 0

./build/linux/x86_64/release/uring_request_core_integration_test
# "21 of 21 uring core integration tests passed, 0 not run"; exit 0
```

**口径**：这三个二进制是 2026-11-12 21:42 的既有 release 产物；`git diff HEAD -- src include tests` 为空，但本会话**没有 clean rebuild**。因此它们是“现有源码树 + 既有二进制”的证据，不是本轮 new-build 的证据（`[M53] §7`）。

### E.4 W3 的语义证据状态（R）

- 本战役**没有**为 W3 运行新的语义 oracle/mutant。W3 的成员（M-61/M-62/M-67/M-10）仍是 `INCONCLUSIVE`，需要行级 measured 归因 + 步间语义证明 + 单因素 A/B；M-65 属 W4 owed 家族，不在 W3 成员内。
- M-61 `begin`/`complete` 有 **r6 R3 的候选级拒绝**：0.45% < 1% MME（`[L] §4` 行 16；`[P3] §1.5`）。P2 的 publication 4.13% 是不同测量范围的**阶段容器**，不能推翻 0.45% 的候选级结果（`[P2M] §7`）。
- M-62 的 4.13% 是 publication 阶段容器，不是候选上界；`claim_execution` 叶 93/93 已归 publication，4.74% 的双计已删（`[P2M] §6.2`；`[P2R] M2`）。
- M-61/M-62/M-67/M-10 的任何跨方法临界区合并，都必须证明 `publication_inflight`/slot reuse/observer 注册不被改变（`[L] §5.1` W3 门禁；`[P3] §3.2` 表头）；M-65 在 W4 owed 家族下适用同一门禁。
- P4 的行级归因可作为“这些方法在哪些行取/放 mutex、在哪个形态由 worker/main/driver 承担”的方向性证据，但不能作为 W3 定量准入；P4 复核明确 “W3 的 mutex 行级地图成立，W3 的 stage 容器算术和 W1/P-E 的 poll 归因不成立，M-53 的 d64 hotspot 终判不成立”（`[P4R] §6`）。

### E.5 语义证据总判词

| 项 | 判词 | 口径 |
|---|---|---|
| `#634` parity + mutants | **I**（已合并；本轮未重跑） | 产品代码/测试在 BASE；parity 8 用例 + 4 mutants 是回归义务 |
| `#635` drain oracle + mutant + model | **I**（已合并；本轮未重跑） | 产品代码/测试在 BASE；证据目录不在 BASE；9 用例 + mutant + TLA+ |
| W1 D1/D2/D3/C1/A4–A8 | **R：`SEMANTIC_MISMATCH`** | 不是性能不足；是 fd 语义/通知拓扑/唤醒协议阻断 |
| W1 C2 | **R：`NEEDS_OWNER`** | 契约允许 condvar；需 owner 授权第二通知通道 |
| W1 A1/A2/A3/B | **R：`NO_CHANGE_JUSTIFIED`** | 语义保持但不 material |
| M-53 O1/O2/O3 | **R + V：PROVEN / PROVEN / INCONCLUSIVE** | O3 的 packed 编码未在 HEAD 实现；三个测试本轮 exit 0 |
| W3 语义门 | **U（未进入）** | 没有候选级语义证明；不写“通过” |
| 真实消费者语义 | **U** | `#604` G0 未准入；`sluice-copy` 是参考应用 |

---

## F. A/B Experimental Results

### F.1 结论先行

**本战役没有运行任何 A/B 实验。** 没有候选 build、没有候选 SHA、没有预注册的 A/B 协议、没有单因素候选臂。所有计时数据都是 **A/A 校准**：两臂同一 binary SHA，只估计噪声，不检验候选效应。

- **0** 个 A/B session；
- **0** 个候选达到 `ADMIT_TO_AB`；
- **0** 个“收益成立”结论；
- **11** 个 A/A 格（P1 6 + P2 5），**9** 个有效格，**2** 个无效格（C2/C5），**144** 个有效配对，**288** 个有效 arm（总 352 arm）。

### F.2 A/A 校准清单（R + V 复算）

| 格 | workload | 有效对 | 判词 | 关键数字（SD / hw / MDE80） | 备注 |
|---|---|---:|---|---|---|
| C1 | sluice-copy 4k/d4/w1 | 16 | **QUIET** | CPU 0.786 pp / 0.365% / 0.589%；instr 0.012 / 0.005 / 0.009；wall 1.495 / 0.708 / 1.120；cycles 2.346 / 1.121 / 1.758 | btrfs→tmpfs 256 MiB copy；不能外推磁盘/NVMe |
| C2 | W-02 TP 4k d16（filter 未命中） | **0** | **INCONCLUSIVE** | 32/32 arm 无效；实际 case 是 `W02-threadpool-4k-d16-w4` | 只有 CAL；wall ~60 ms；不得当计时数据 |
| C3 | W-02 uring 4k d16 | 16 | **ENVIRONMENT_BLOCKED** | CPU 7.027 / 3.312 / 5.266；cycles 1.991 / 0.949 / 1.492；instr 0.143 / 0.068 / 0.107 | CPU/wall 频率触发；cycles 未认证 |
| C4 | W-03 uring 4k d16 | 16 | **QUALIFIED** | CPU 3.019 / 1.402 / 2.262；wall 1.520 / 0.722 / 1.139；cycles 8.123 / 3.843 / 6.088 | 只支持 ≥~2.3% CPU；1% CPU 需 74/143 对 |
| C5 | W-02 TP 4k d1（filter 未命中） | **0** | **INCONCLUSIVE** | 实际 case 是 `W02-threadpool-4k-d1-w1` | 只有 CAL；不得当计时数据 |
| C6 | W-02 uring 4k d1 | 16 | **ORDER_SENSITIVE**（wall） | CPU 0.997 / 0.471 / 0.747；cycles 2.876 / 1.368 / 2.155；wall 1.885 / 0.885 / 1.413 | wall 第二臂 +1.132 pp（13/16，p=0.0049）；CPU 可用；wall 必须严格 AB/BA |
| C2b | W-02 TP 4k d16 w4 | 16 | **NOISY** | CPU 3.414 / 1.604 / 2.558；wall 3.380 / 1.580 / 2.533；instr 漂移 p=0.0017 | filter 修正成功，但格不可用；TP d16 w4 仍无基线 |
| C3b | W-02 uring 4k d16 | 16 | **ENVIRONMENT_BLOCKED** | CPU 5.526 / 2.597 / 4.141；cycles 1.319 / 0.621 / 0.988 | CPU/wall 阻塞；cycles 条件带（未认证） |
| C5b | W-02 TP 4k d1 w1 | 16 | **QUIET（条件基线）** | CPU 0.964 / 0.458 / 0.723；instr 0.002 / 0.001 / 0.002；wall 1.182 / 0.564 / 0.886；cycles 3.107 / 1.481 / 2.328 | 背景负载警告；cycles 不作窄带 |
| C7 | W-02 uring 4k d64 | 16 | **ENVIRONMENT_BLOCKED** | CPU 9.782 / 4.573 / 7.331；cycles 1.889 / 0.893 / 1.416（t-hw 1.0066）；wall 4.861 / 2.282 / 3.643 | CPU/wall A/A 均值不正（CPU +5.043%）；cycles 唯一居中，条件带 |
| C8 | W-02 TP 4k d64 w4 | 16 | **NOISY** | CPU 4.938 / 2.323 / 3.700；wall 4.891 / 2.280 / 3.666；cycles 6.804 / 3.259 / 5.099 | 漂移边缘；不能给 M-53 提供 TP d64 基线 |

来源：`[P1] §0/§3.4`、`[P2A] §0/§3.1`；本会话 `recompute_independent.py` 复算 JSON 与上表一致（§G.2）。`hw` 是 n=16 的 95% CI 半宽；CPU/instr/wall/cycles 的单位是配对差百分点（pp），`hw`/`MDE` 是百分比。

### F.3 为什么没有 A/B

按 `[P3] §1.1` 的五条件逐项检查，**没有任何候选同时满足**：

| 条件 | W1 | W3 | M-53 | M-42 | `#637` 其余 |
|---|---|---|---|---|---|
| 1 合法机制 | D/C1/A4–A8 硬冲突；C2 需 owner | 合法但未证步间语义 | 合法，但 packed 编码未实现 | 合法（seam 计数） | `NO_GO`/`SEMANTIC_MISMATCH` 不满足 |
| 2 候选级 measured hotspot | 只有阶段/边界级；P-B 用户栈截断；P-A/P-C 无 poll | **有候选级 owner-total 点估计（`offer_terminal` 1.80%、`claim_execution` 1.56% 等），但 P4R 判为方向性证据；单候选可删量 `NOT_OBSERVABLE`** | d16 0.406/0.285%（低于分辨率）；d64 仅点估计、不显著 | 无 measured hotspot；算术 ≤0.14% | 多数没有 |
| 3 语义可证 | D/C1 不可证；C2 需新模型 | 缺 `publication_inflight`/slot reuse/observer 证明 | O1/O2 已证；O3 未证 | 机制门未做 | — |
| 4 A/A 分辨率 | C6/C1 可用，但 material 方向不存在 | P-D C2b NOISY；P-C C4 2% 20/38；P-E C1 4/5 | δ=2% 17–20；δ=1% 58；0.4% 172/336 | 远低于噪声 | — |
| 5 无历史反例 | — | M-61 R3 0.45% <1% MME | — | `GO` 只是排队 | `NO_GO`/`NO_GO_ABORT` 未翻案 |

结论：

1. **W1 的 material 方向不是“还没测”，而是语义不成立/需 owner**；语义保持的 A1/A2 不 material。没有合法的候选人可以进入 A/B。
2. **W3 没有候选级可删量**；阶段容器不能当上界；P-D 的匹配 A/A（C2b）是 NOISY；M-61 还有 R3 反例。进入 A/B 会违反“候选级 measured hotspot”与“无历史反例”。
3. **M-53 的 d64 分辨力不足**：16 对 C7 cycles t-hw 1.0066% > 2%/2=1.0%；17–20 对只对应 δ=2%；δ=1% 需 58 对；若 d64 效应接近 0.4% 的乐观假设，则需 172/336 对（**0.4% 不是 d64 实测效应**）。且 O3 未证。没有满足 ledger 的资格，不能开 A/B。
4. **M-42 的合理效应（≤0.14% cycles 或 1 instr 0.0041–0.0094%）低于匹配 uring 带的 MDE**；用 36 对 W-02 去“测一下”只能得到“不显著”，不能区分“无效应”和“效应太小”。
5. **`#637` 的 `NO_GO`/`NO_GO_ABORT`/`SEMANTIC_MISMATCH` 终态全部维持**；没有翻案条件被满足。
6. **`#604` G0 未准入**：即使 owner 接受 bench/reference 代理，任何结果也只能是 mechanism-level/bench-proxy，不能声明 application-level 收益；而 W1 的 ledger 门要求真实消费者 profile。

### F.4 不能从 A/A 推出的结论

- 不能把 A/A 的均值（接近 0）当效应；不能把 16 对“不显著”当“无效应”。
- 不能把 C1 的 QUIET 或 C6 的低 hw 宣传为“候选有效”；A/A 只说明基线噪声。
- 不能把 C7 的 cycles 窄带（bootstrap hw 0.893%）当作“M-53 已达 hw≤效应/2”：t-hw 是 1.0066%，且 C7 是 `ENVIRONMENT_BLOCKED` 下的条件带，用前必须 cycles-only A/A。
- 不能把 `#634`/`#635` 的历史 A/B 数字当作本轮 A/B 结果；它们是已合并历史，本轮未重跑。

**本节状态**：A/A 清单 = **R + V（复算）**；A/B = **U（0 个）**；候选 A/B 收益 = **U**。

---

## G. Independent Statistical Audit

### G.1 本会话实际执行的复算与检查（V）

| # | 检查 | 命令 | 结果 |
|---:|---|---|---|
| G1 | P1 A/A 复算 | `cd docs/evidence/621-progress-wait-path-20261118 && python3 -I tools/recompute_independent.py --aa-root p1/aa --json-out /tmp/final-aa1.json` | exit 0；C1/C2/C3/C4/C5/C6 判词与 §F 一致；C2/C5 0 对 |
| G2 | P2 A/A 复算 | `... --aa-root p1/aa2 --json-out /tmp/final-aa2.json` | exit 0；C2b/C3b/C5b/C7/C8 判词与 §F 一致 |
| G3 | P2 R1 复算 | `cd p2/profile/analysis && python3 -I recompute_p2_r1.py --json-out /tmp/final-p2r1.json` | exit 0；P-B `__poll` 6,129/24,585=24.9298%、poll boundary 2,251=9.1560%、progress/wait 6,168=25.0885%；P-E 5,500/34,736=15.8337%、1,520=4.3759%、8,461=24.3580%；P-D publication 627/15,182=4.1299%、terminal 530=3.4910%；W3 family 502/59/2,606/3,405/4,539；M-53 34/47；M-42 `lock addq` 与 0.1259–0.1325%（P-A）等一致 |
| G4 | 独立 t/MME | `python3 -I /tmp/final_tcheck2.py`（自写 Numerical-Recipes 不完全 beta + 二分；**不 import 仓库工具**） | required-n 与 `[P3] §1.4` 一致（见 §G.3）；`t_.975(15)=2.13145`、`t_.80(15)=0.86625` |
| G5 | M-53 测试 | 三个既有 release 二进制（见 §E.3） | 7/7、20/20、21/21，exit 0 |
| G6 | 源码坐标 | `sed`/`grep` 抽查 `progress_source.hpp:70-80,104-232,414-460`、`uring_transport.cpp:218-235,551-557`、`threadpool_backend.cpp:430-438`、`uring_backend.hpp:320-326`、`uring_test_seams.hpp:72-78` | 与证据文档一致；`submit_flushes_` 成员在 `uring_backend.hpp:324`，唯一读者在 `uring_test_seams.hpp:75-76` |
| G7 | Git/环境 | 见 §A.6、§B.1 | 一致 |

**未执行（U）**：任何新构建/测试、完整矩阵、TLC、sanitizer、新 perf/bench/A-A/A-B；P4 `analyze_w3.py` 的本次重跑（`[P4R] §1` 已记录其结果，本会话未重复）。

### G.2 A/A 复算与发布值对照（V）

| 格 | 指标 | 发布值 SD / hw / MDE80（`[P1]`/`[P2A]`） | 本会话复算 SD / bootHW / tHW / MDE80 | 差异 |
|---|---|---|---|---|
| C1 | CPU | 0.786 / 0.365 / 0.589 | 0.7858 / 0.3650 / 0.4187 / 0.5889 | 四舍五入一致 |
| C1 | cycles | 2.346 / 1.121 / 1.758 | 2.3462 / 1.1213 / 1.2502 / 1.7583 | 一致 |
| C3 | CPU | 7.027 / 3.312 / 5.266 | 7.0267 / 3.3122 / 3.7443 / 5.2660 | 一致 |
| C3 | cycles | 1.991 / 0.949 / 1.492 | 1.9909 / 0.9492 / 1.0609 / 1.4920 | 一致 |
| C4 | CPU | 3.019 / 1.402 / 2.262 | 3.0188 / 1.4021 / 1.6086 / 2.2623 | 一致 |
| C4 | cycles | 8.123 / 3.843 / 6.088 | 8.1230 / 3.8429 / 4.3285 / 6.0876 | 一致 |
| C6 | CPU | 0.997 / 0.471 / 0.747 | 0.9972 / 0.4706 / 0.5314 / 0.7473 | 一致 |
| C6 | wall | 1.885 / 0.885 / 1.413 | 1.8853 / 0.8850 / 1.0046 / 1.4129 | 一致；位置 +1.1324 pp（p=0.0049） |
| C2b | CPU | 3.414 / 1.604 / 2.558 | 3.4138 / 1.6042 / 1.8191 / 2.5584 | 一致 |
| C2b | wall | 3.380 / 1.580 / 2.533 | 3.3799 / 1.5797 / 1.8010 / 2.5330 | 一致 |
| C3b | cycles | 1.319 / 0.621 / 0.988 | 1.3188 / 0.6208 / 0.7027 / 0.9883 | 一致 |
| C5b | CPU | 0.964 / 0.458 / 0.723 | 0.9643 / 0.4575 / 0.5138 / 0.7227 | 一致 |
| C7 | cycles | 1.889 / 0.893 / 1.416 | 1.8890 / 0.8930 / 1.0066 / 1.4157 | 一致；t-hw 1.0066 |
| C7 | CPU | 9.782 / 4.573 / 7.331 | 9.7816 / 4.5729 / 5.2122 / 7.3305 | 一致；均值 +5.0434% |
| C8 | CPU | 4.938 / 2.323 / 3.700 | 4.9376 / 2.3233 / 2.6310 / 3.7003 | 一致 |

**结论**：发布值与独立复算在 4 位小数内一致；A/A 判词与噪声带可复现。**bootstrap hw 与 t hw 不是同一个量**：n=16 时 C7 cycles 的 bootstrap hw 0.893%、t hw 1.0066%；边缘判据必须用更保守的 t hw（ledger 的“半宽 ≤ 效应/2”按 t 口径检查）。

### G.3 独立 MME / required-n 复算（V）

公式（`[P1] §1.3`；`[P2A] §6.1`）：

```text
n80(δ)    = min n: (t_{0.975,n-1} + t_{0.80,n-1}) * SD / sqrt(n) <= δ
n_half(δ) = min n: t_{0.975,n-1} * SD / sqrt(n) <= δ/2
```

本会话自写 t 分位（不完全 beta + 二分）复算，与发布值对照：

| 格/指标 | SD pp | δ=2% n80/n_half | δ=1% | δ=1.2% | 36 对 tHW / MDE80 | 发布值 |
|---|---:|---|---:|---|---|---|
| C4 CPU | 3.019 | **20 / 38** | 74 / 143 | 52 / 100 | 1.021% / 1.450% | 一致（`[P3] §1.4`） |
| C1 CPU | 0.786 | **4 / 5** | 7 / 12 | 6 / 10 | 0.266% / 0.378% | 一致 |
| C3b cycles | 1.319 | **6 / 10** | 16 / 30 | 12 / 22 | 0.446% / 0.634% | 一致 |
| C5b CPU | 0.964 | **5 / 7** | 10 / 17 | 8 / 13 | 0.326% / 0.463% | 一致 |
| C6 CPU | 0.997 | **5 / 7** | 10 / 18 | 8 / 14 | 0.337% / 0.479% | 一致 |
| C6 cycles | 2.876 | 19 / 35 | 67 / 130 | **48 / 91** | 0.973% / 1.382% | 一致 |
| C1 cycles | 2.346 | 13 / 24 | 46 / 88 | **32 / 62** | 0.794% / 1.127% | 一致 |
| C7 cycles | 1.889 | **10 / 17** | **30 / 58** | 22 / 41 | 0.639% / 0.907% | 一致 |
| C7 cycles（0.406%） | 1.889 | — | — | — | — | **172 / 336**（`[M53] §6.2`） |
| C7 cycles（0.4%） | 1.889 | — | — | — | — | **178 / 346** |
| C7 cycles（0.285%） | 1.889 | — | — | — | — | **347 / 678** |
| C7 CPU（2%） | 9.782 | **190 / 371** | 753 / 1473 | 524 / 1024 | — | 190 / **370**（差 1 对，边界舍入） |

**归档交叉验证（V）**：本会话读取 `/tmp/final-aa1.json`、`/tmp/final-aa2.json` 的 `metrics.*.requiredN` 字段（由归档 `tools/recompute_independent.py` 生成），δ=2%/1%/0.5% 的 `n80Power`/`nHalfWidthDeltaOver2` 与本表一致：C4 CPU 20/38、C1 CPU 4/5、C3b cycles 6/10、C5b CPU 5/7、C6 CPU 5/7、C6 cycles 19/35、C1 cycles 13/24、C7 cycles 10/17、C7 CPU 190/370。一次性 `/tmp/final_tcheck2.py` 是不 import 仓库工具的第三实现；它只在 C7 CPU δ=2% 的边界给出 371（370 时 hw=1.000004%，差 4×10⁻⁶）。

**边界发现（透明披露）**：

- C7 cycles 16 对：`t_hw=1.0066%`（严格 > 2%/2=1.0%），所以 16 对在保守 t 判据下**差一对**；17 对 t_hw 0.9712%、20 对 0.8841%、58 对 0.4967%。**17–20 只对应 δ=2%**；δ=1% 的 A/A 也必须 58 对（17/20 对 hw >0.5%）（`[M53] §6.2`、`[P3] §4.3`）。
- C7 CPU δ=2% 的 n_half：本会话独立复算为 **371**；发布值 `[P2A] §6.4` 为 **370**。在 n=370 时 hw=1.000004%，仅比 1.0% 高 4×10⁻⁶；这是 t 分位/舍入边界差 1 对，不改变“当前环境 CPU NOT_POWERED”的结论。报告中凡引用 C7 CPU 预算，写“约 370–371 对”。
- C3 instr MDE80：发布 0.107%，本会话 0.1069%（四舍五入一致）。
- 所有 n 都是“每个候选、每个格、每个指标”的下限；实际预算还要加 A/A、失败重跑、多重候选修正（`[P1] §6.1`）。

### G.4 噪声带与适用条件（V/R）

**适用条件**：同 host（E5，kernel 7.3.3-200.fc44）、同 `performance` governor/`no_turbo=0`、同 binary SHA、同 workload、同介质、n=16、AB/BA 严格交替。任何条件变化都必须重新做 A/A（`[P1] §5`；`[P2A] §5`）。

| 格 | 状态 | 可用指标 | 不可用/限制 |
|---|---|---|---|
| C1 | QUIET | CPU/instr/wall | 只适用于 sluice-copy 4k/d4/w1；btrfs→tmpfs；不能外推磁盘/NVMe |
| C3 | ENVIRONMENT_BLOCKED | cycles 仅“条件性诊断带”，未认证 | CPU/wall 不可用；用前 cycles-only A/A |
| C4 | QUALIFIED | CPU（≥~2.3%） | cycles MDE80 6.088%，不可用于 1% 声明 |
| C6 | ORDER_SENSITIVE（wall） | CPU/cycles | wall 必须严格 AB/BA；丢失 arm 则 wall 不可用 |
| C2b | NOISY | 无 | TP d16 w4 仍无基线 |
| C3b | ENVIRONMENT_BLOCKED | cycles 条件带（hw 0.621%、MDE80 0.988%），未认证 | CPU/wall 不可用 |
| C5b | QUIET（条件） | CPU/instr/wall | cycles SD 3.107 不作窄带；背景负载警告；不是 W3 匹配格 |
| C7 | ENVIRONMENT_BLOCKED | cycles 条件带（hw 0.893%、t-hw 1.0066%、MDE80 1.416%） | CPU/wall A/A 均值不正；cycles 用前 17–20 对 cycles-only A/A |
| C8 | NOISY | 无 | TP d64 w4 不能替代 C7 |

**匹配规则**：P-A→C3b（cycles，条件）；P-B→C6；P-C→C4（CPU）；P-D→C2b（NOISY，无可用带）；P-E→C1（CPU）；M-53 d64→C7（cycles，条件）。C5b 不是任何 P2 W3 形态的匹配格；不能把 C5b 的 CPU 带搬到 P-D/P-C/P-E（`[P3] §1.3`、`[P3] §4.2`、`[P3R] H1`）。

### G.5 预算汇总（V/R）

| 候选 | 关键 δ | 建议格/指标 | A/A 对 | A/B 对 | 结论 |
|---|---:|---|---:|---:|---|
| W1（5% 过程级 cycles，C6/P-B） | 5% | C6 cycles | 36（实际 5–8 对即可） | ≥36 | 代理路径；material 方向仍受 O-2 约束 |
| W1（5% measured-loop → 1.2% 过程级，C6） | 1.2% | C6 cycles | 36 | **48–91**（80%/hw≤0.6%） | 36 对不够 hw；`[W1D] §7.3` |
| W1（P-E/C1，1.2% cycles） | 1.2% | C1 cycles | 36 | **32–62** | 36 对 80% 够、hw≤0.6% 不够 |
| W3（P-D） | 2% | C2b CPU | — | — | **NOISY，无可用带**；先重跑或改形态 |
| W3（P-C） | 2% | C4 CPU | 36 | **20–38** | 36 对 hw 1.021% >1%，hw≤1% 需 38 |
| W3（P-E） | 2% | C1 CPU | 36 | **4–5** | 可用 |
| W3（P-A） | 2% | C3b cycles | 36 | **6–10** | 条件带；cycles-only A/A |
| M-53（d64，δ=2%） | 2% | C7 cycles | **17–20** | 17–20 | 16 对差一对；仅 δ=2% |
| M-53（d64，δ=1%） | 1% | C7 cycles | **58** | 30（80%）/58（hw） | 17–20 不能用于 δ=1% |
| M-53（d64，0.4%） | 0.4% | C7 cycles | 346 | 178 | 合计 ≥524 对；不可行 |
| M-42 | ≤0.05% CPU | 隔离格 | — | — | **NOT_POWERED**；先建隔离格 + 动态回执带 |

**预算来源**：`[P3] §1.4`、`[P3] §6`；`[M53] §6`；`[W1D] §7.3`；本会话独立 t 复算（§G.3）。

### G.6 统计审计结论

- **可复现**：9 个有效 A/A 格的 mean/median/SD/hw/MDE 与发布值一致；P2 R1 的阶段/`__poll`/poll boundary/M-53/M-42 关键数字与发布值一致。
- **不可混淆**：bootstrap hw ≠ t hw；sample-count ≠ period 加权；inclusive ≠ 可删量；阶段容器 ≠ 候选上界；A/A ≠ A/B。
- **边界披露**：C7 cycles 16 对 t-hw 1.0066% 差一对；C7 CPU δ=2% n_half 370 vs 371 的舍入边界；C3/C3b/C7 的“频率”标签是预注册判据的机械名称，机制是 sys-time 主导；C7 CPU/wall A/A 均值不正；C6 wall 位置效应；C5b 背景负载警告。
- **未验证**：没有重跑计时；没有 clean rebuild；没有完整矩阵；没有真实消费者 profile；没有新的 A/B。

---

## H. Cross-Candidate Integration

### H.1 没有 GO，也没有集成候选

**明确结论：本战役没有 GO，没有可集成的候选组合。** 不能把 W1/W3/M-53/M-42 的份额相加，也不能把它们的“潜在删除量”拼成一个产品 PR；原因不是“还没测组合”，而是：

1. **没有单候选通过五项准入**；组合不能绕过单候选的语义门、机制门和 A/B 门（`[P3] §1.1`）。
2. **数字不可相加**：W1 的 `__poll` 24.93%、W3 的 publication 4.13%、M-53 的 d16 0.406%、M-42 的 0.14% 是同一进程 cycles 的不同子集/切法；阶段容器与 inclusive 重叠；user 部分是阶段的子集（`[P2M] §3.8`、`[P4R] §2.5`）。
3. **语义域重叠**：W1 的 `wait_one`/`wait_if_unchanged` 与 W2 的 drain/rearm、M-67 的 sticky flags 在同一 `ProgressSource`；W3 的 M-61/M-62/M-67/M-10 在同一 `RequestCore`/publication 域；W4 的 M-65/M-63 在 owed/terminal 域；M-53 的 router/CQE 与 W4 的 owed/CQE 在同一 `dispatch_mtx_` 域；M-42 的 submit 与 H1 的 cadence 在同一 submit 路径。任何组合都必须重新证明步间语义，不能把两个“单因素”结果直接叠加。

### H.2 具体重叠矩阵（R）

| 候选 A | 候选 B | 重叠 | 集成规则 |
|---|---|---|---|
| W1 poll wait | W2 drain/rearm | 同一 `ProgressSource::mtx_`；`wait_if_unchanged` 内 drain；`#635` 单读 drain | W1 的任何改动必须保持“单读清 pending”、EINTR/EAGAIN、fd 借用非第二 drain authority；C2 若引入 condvar，必须重做 W2 模型/oracle |
| W1 poll wait | M-67 sticky flags | 同一 `progress_source.hpp`；M-67 的 flags 是 wait predicate 的一部分 | M-67 路由到 W3；不能把 W1 的阶段份额与 M-67 的 flags 相加；任何融合查询都要证 `publication_inflight`/slot reuse/observer 不变 |
| W3 M-61 begin/complete | W3 M-62 claim/retire | 同一 publication 阶段；`claim_execution` 叶已归 publication | 不能把 4.13% 当 M-62 上界；M-61 有 R3 0.45% 反例；两者不能同时“各删一把锁”相加 |
| W4 M-65 release_control | W4 owed/CQE | 同一 terminal/release 与控制 CQE 路径 | M-65 归 W4 owed 家族；zero-op/owed 协议未激活前不能测；第三锁与 retire 融合必须保交付顺序 |
| W1 poll wait | M-53 CQE reap | `wait_one` 的 progress pass 调用 `reap_cqes`/`handle_one_cqe`；M-53 在 reap 路径 | `wait_one` inclusive 31.29% 包含 poll/mutex/progress pass；M-53 只是其中 router 子集；不能相加 |
| M-53 router | W4 M-63/M-65 owed | 同一 `dispatch_mtx_`/CQE 处理；M-53 的 packed 编码会改控制 CQE 与 cancel 目标 | 必须同时证 O1/O2/O3；M-53 的编码改动会影响 owed 控制的 exact-submitted 与 cancel 定向 |
| M-42 submit_flushes_ | H1 提交批形态 / `#595` | 同一 submit 路径；`submit_flushes_` 是 submit-attempt 计数 | H1 仍 `NO_GO`；M-42 是 `NOT_POWERED`；不能用 #635/#634 的收益替代 #595 的 submission-cadence 证据 |
| `#604` G0 / `#617` G1 | 所有候选 | 真实 workload / kernel-boundary 是两个独立的 scope 门 | `#621` 不能替代 #604/#617 的门；G1 未满足前 kernel-boundary 候选不进 A/B |

### H.3 集成决策

- **没有集成 A/B**：不存在两个已准入的单因素候选，因此没有“先合并两个已证收益”的实验设计。
- **没有集成 PR**：本战役没有产品改动；任何未来 PR 必须按候选单独预注册、单独语义门、单独 A/A/A/B，不能把多个候选打包成一个“优化 PR”。
- **唯一可能的未来组合**：如果 owner 授权 C2（W1 TP condvar）+ #604 G0（真实 workload）+ #617 G1（kernel-boundary），它们也必须是三个独立项目/独立证据，不是 #621 的集成包。C2 与 W2 的 drain/rearm 语义必须一起重证；M-53 与 owed/CQE 必须一起重证；W3 的锁融合必须逐候选证明步间语义。

**本节状态**：重叠关系 = **R**；集成候选 = **U（无）**；集成 GO = **否**。

---

## I. Final Decision Matrix

### I.1 W1–W6 最终决策

| 工作包 | 最终决策 | 能否翻案 | 翻案条件 | 预算 |
|---|---|---|---|---|
| W1 | 不进入 A/B；material 方向 `SEMANTIC_MISMATCH`；C2 `NEEDS_OWNER`；hygiene `NO_CHANGE_JUSTIFIED` | 仅 C2/拓扑变更 | owner O-2 授权第二通知通道 + 新模型/新 oracle/机制门 + 真实消费者或 owner 接受代理；O-1/O-3 先定 | M/L（若 C2） |
| W2 | `NOT_APPLICABLE`；`#635` 已合并 | 仅未来新改动 | 任何改动保持 9 用例 oracle + mutant + 模型；无新增候选 | N/A |
| W3 | `ADMIT_TO_DESIGN`；成员 M-61 `INCONCLUSIVE`、M-62/M-67/M-10 `INCONCLUSIVE + design work` | 是，需设计门 | 行级 measured 归因（P-D/P-C/P-E 主形态）+ 步间语义 + `publication_inflight`/slot reuse/observer 证明 + 匹配 A/A + 单因素 A/B；M-61 还需推翻 R3 | M |
| W4 | `ADMIT_TO_DESIGN` | 是，需设计门 | M-53：O3 8 项 + d64 cycles-only A/A + δ 预算；M-40/M-63/M-65 设计门 | M/L |
| W5 | `NOT_APPLICABLE`；`#634` 已合并 | 仅未来新改动 | parity 8 用例 + 4 mutants + 完整矩阵 | N/A |
| W6 | M-42 `NOT_POWERED`；M-28 `NO_GO_ABORT` | M-42 仅 owner 资助隔离格；M-28 仅四要素 | M-42：隔离格 + 动态回执带 + 机制门 + mutant + release 无 seam + 生产读者后果；M-28：四要素同时满足 | S/0 |

### I.2 `#637` 最终决策（按裁决分组）

| 裁决组 | 候选 | 最终状态 | 翻案条件（摘要） |
|---|---|---|---|
| **`NO_GO_ABORT`** | M-28 | 维持；不重做 | 四要素：单指令差、布局/时序隔离、新 session/冻结、动态回执接受带 + 隔离格 |
| **`NO_GO`** | M-61 payload、H1、io-wq punt、tp-w8 | 维持 | M-61 payload：生产消费者或 owner 批准测试通道；H1：`#595` 全门 + 整机重测；io-wq：无翻案（非 Sluice 代码）；tp-w8：外部固定 worker 的 root workload + 机制门 |
| **`SEMANTIC_MISMATCH`** | M-09、M-30、M-57、M-12、M-80 hygiene（维持）；M-16（本轮由 `INCONCLUSIVE` 改判） | 五项维持；M-16 因 owner seam 冲突改判；均先 owner 裁决 | 各自 KEEP/DELETE/修 root；裁决前不做性能实验 |
| **`NOT_POWERED`** | M-42 | 本战役记 `NOT_POWERED`；`#637` 原 `GO` 仅为排队 | 隔离格 + 动态回执带 + 机制门 + mutant kill + release 无 seam + 生产读者 → `NO_GO` |
| **`INCONCLUSIVE`** | M-61 begin/complete、M-80 write、M-37、M-01 | 维持 | M-61：推翻 R3 + 确定性注入 + 族 A/A；M-80 write：直呼组合写 measured hotspot + 不变量；M-37：深队列 memmove 或队列深 ≤1；M-01：`-scope64` 隔离 + owner S2 |
| **`INCONCLUSIVE` + design** | M-62、M-65、M-67、M-10、M-33、M-74、M-40、M-63 | 维持 + 设计/资格工作 | 行级 measured 归因 + 步间语义 + 匹配 A/A + 单因素 A/B；具体见 D.4 |
| **`ADMIT_TO_DESIGN`** | M-53 | 设计/资格 | O1/O2 已证；O3 8 项 + d64 cycles-only A/A（δ=2% 17–20；δ=1% 58；若效应接近 0.4% 则 172/336） |

> M-42 在 `#637` 的原始终态是 `GO`（排队、未启动）；本战役的最终裁决是 `NOT_POWERED`。两行合并见 D.4 行 11。

### I.3 最终判词

- **`ADMIT_TO_AB = 0`**：没有候选同时满足五条件。
- **无 GO**：没有候选通过语义门、机制门与性能门。
- **无产品 PR**：本战役没有产品改动；`#634` 在 BASE 之前合并，`#635` 就是 BASE 本身。
- **`#621` 保持 OPEN**：停在 Owner Gate；不得自动关闭、不得自动 merge（`[L] §9.1.5`、`§9.3`）。

---

## J. Product/PR Handoff

### J.1 本战役的产品交付

| 项 | 状态 |
|---|---|
| 产品代码改动 | **无**（本轮未修改 `include/`、`src/`、`apps/`、`tests/`、`xmake.lua`、`xmake/`） |
| 产品 PR | **无** |
| 合并/关闭 | **无**；`#621` 保持 OPEN，停在 Owner Gate |
| 新增运行时机制 | **无**（因此 authority-contraction 审计 = N/A：没有移动生产权威） |
| 新增证据 | 本报告 + `ISSUE-621-COMMENT.md` + 战役证据目录（A/A 原始数据、P2/P4 profile、构建/测试回执） |

### J.2 如果 owner 决定继续：每个方向的 PR 形态

| 方向 | PR 形态 | 必须包含 | 不能包含 |
|---|---|---|---|
| **W1-C2**（若 O-2 授权） | 通知拓扑/协议 PR（独立于 #621 的“最小 B”） | 扩展/新 TLA+ 模型；新语义 oracle（signal before/during/after park、host ack 与 owner park 并发、bounded timeout、fd close/reuse、retire/shutdown）；机制门七问；authority-contraction 审计；C1（TP）匹配 A/A + 单因素 A/B（C6 只适用于 uring 方向）；真实消费者或 owner 书面接受 bench 代理 | 不能把 blocking read/relay 作为“顺手优化”；不能削弱 `#635` 的单读 drain；不能新增第二 drain authority |
| **W1-A1/A2/A3**（hygiene） | 小代码面 PR | 保持全部语义义务；现有 9 用例 oracle + suppression/health/fd reuse/race 套件；不得声称收益 | 不能开 A/B；不能删 poll/probe/P3/drain |
| **W3**（M-61/M-62/M-67/M-10） | 设计/资格 PR（先）→ 单候选性能 PR（后） | 行级 measured 归因；步间语义证明（`publication_inflight`/slot reuse/observer 注册）；机制门七问；族 A/A 先行；单因素 A/B；M-61 需推翻 R3 | 不能把 publication 4.13% 当候选上界；不能把多个锁融合打包；不能跳过语义门 |
| **W4 owed 家族**（M-63/M-65/M-40） | 设计/资格 PR（先）→ 单候选性能 PR（后） | zero-op/owed 协议；交付顺序不变性；`publication_inflight`/slot reuse/observer 注册证明；机制门；族 A/A；M-40 还需 reader role 与 `dispatch_mtx_` 覆盖 | 不能把 publication/terminal 容器当候选上界；不能跳过 T9 协议 |
| **M-53** | 设计/证明 PR（packed identity）→ d64 资格 A/B | 8 项实现（`request_capacity ≤ 32768`、2^48 cookie horizon、packed operation identity、router/ledger 存 packed、cancel target packed、控制 packed、decode + full identity、所有消费者统一）；HEAD envelope tests；O1/O2/O3 证明；d64 cycles-only A/A（17–20 仅 δ=2%；δ=1% 58；0.4% 172/336） | 不能编码 SQ 物理槽位；不能跳过 O3；不能把 d16 0.406%/0.285% 当 d64 效应 |
| **M-42** | 测试/隔离格 PR（仅 owner 资助） | submit-only 紧循环；动态回执带；机制门七问；mutant kill；release 预处理与 `nm` 无 seam 符号；出现生产读者 → `NO_GO`/重新物化 | 不能用 W-02 36 对“测一下”；不能把 `GO` 当 A/B 准入 |
| **语义冲突批** | 代码面 PR（无性能门） | owner 裁决 + 回归测试 + oracle 迁移 | 不能包装成性能实验；不能先改代码再让 owner 追认 |

### J.3 合并前的通用门（AGENTS.md / ledger）

任何未来 PR 必须：

1. 引用受影响的 root requirement IDs（PROG-01–04、CANCEL-01、BACKEND-01 等）与当前/目标行为；
2. 指明实现 owner、workload、验证与剩余限制；
3. 过语义/协议 oracle + mutant（语义 FAIL 即 `NO_GO`）；
4. 过匹配 A/A + 单因素 A/B（≥15 有效交错 AB/BA 对；按判词需要 36/58 对）；
5. 过机制门七问与 authority-contraction 审计（若移动生产权威）；
6. 跑完整构建测试矩阵/sanitizer（按受影响 claim 的范围）；
7. 更新 v2 ledger 的 dated 621 节（GOV-02 状态）；
8. 不夹带一次性 runner、明文凭据、未跟踪工作副本或 `/tmp` 绝对路径；
9. 停在 Owner Gate，不自动 merge/close。

### J.4 Owner Gate 清单（O-1–O-12 修订后）

| # | 裁决点 | 选项 | 建议 | 影响 |
|---:|---|---|---|---|
| **O-1** | W1 的 profile 门：接受 bench/reference 代理，还是等 `#604` G0？ | 等 `#604` G0；接受代理并披露；只做设计/A-A；判 `NO_CHANGE_JUSTIFIED` | **等 G0 或只做设计/A-A**；接受代理只能作 mechanism-level/bench-proxy，不能声明 application-level 收益 | W1 的全部 A/B 路径 |
| **O-2** | W1 的通知拓扑：是否授权第二通知通道/blocking-read 重构？ | 授权 C2 condvar 双通道（`NEEDS_OWNER`）；授权 D 的 relay/重注册；拒绝 | **拒绝在 `#621` W1 内做**；若追，另立 `#617` G1/kernel-boundary 项目，带新模型/新 oracle/机制门 | W1-C2、W1-D |
| **O-3** | W1 的“5%”分母：process cycles / measured-loop cycles / measured-loop CPU？ | 三者选一，写入预注册 | 必须先定义；未定义则任何代理 A/B 只能记 `INCONCLUSIVE`，不得用 16/36 对结果宣传收益；material 方向仍受 O-2 约束 | W1 预算与判词 |
| **O-4** | W3 是否接受 δ=2% 作为计划值，并资助行级归因？ | 接受 δ=2%；或给别的 δ；或不做 | **接受 δ=2%，先做行级归因**（debug build/隔离格），按 P-D/P-C/P-E 主形态；P-D 先重跑 C2b A/A；不直接 A/B | M-61/M-62/M-67/M-10 |
| **O-5** | M-53 是否接受 cycles 为决定性指标，并做 d64 A/A？ | 接受 cycles + **δ=2% 的 17–20 对**；接受 cycles + δ=1%（A/A 与 A/B 各 58 对）；只接受 CPU（→ 当前环境 `NOT_POWERED`）；`NO_CHANGE_JUSTIFIED` | **接受 cycles + δ=2%（17–20）**，同时要求三项迁移证明；**17–20 不能用于 δ=1%**；若效应接近 0.4%（乐观代理假设，不是 d64 实测），需 172/336 对（A/A+A/B ≥524），应重估预算 | M-53 |
| **O-6** | M-42 是否现在资助隔离格？ | 现在建隔离格 + 动态回执带 + 机制门 + mutant kill + release 无 seam 符号；或保持排队 | **保持排队**；`GO` 排队 ≠ A/B 准入；资助前不启动 | M-42 |
| **O-7** | 语义冲突批的 owner 裁决 | M-09/M-30/M-57/M-12/M-16/M-80 hygiene 各自 KEEP/DELETE/修 root | 逐项裁决；裁决前不启动性能实验；**W1-C2 不在此批**（`NEEDS_OWNER`，走 O-2） | 6 个 `SEMANTIC_MISMATCH` |
| **O-8** | M-01 的 free-list 是否重开（S2）？ | 重开；不重开；先做 `-scope64` 隔离 profile 再定 | 先隔离 profile，再裁 free-list | M-01 |
| **O-9** | M-37/H1/tp-w8 的归属边界 | 确认 `#595`/`#604` 归属；`#621` 不启动 | **确认归属，不在 `#621` 启动** | M-37、H1、tp-w8 |
| **O-10** | M-28 是否确认不重做？ | 确认 **`NO_GO_ABORT`**；或给出同时满足 ledger §4.1 四要素的新证据 | **确认 `NO_GO_ABORT`（不重做）**；仅四要素同时满足才重开；**不是 `NOT_APPLICABLE`** | M-28 |
| **O-11** | M-61 payload 是否维持 `NO_GO`？ | 维持；或出现生产消费者/批准测试专用通道 | **维持 `NO_GO`** | M-61 payload |
| **O-12** | W2/W5 是否确认无新候选？ | 确认；或指定新改动 | **确认**；任何改动重跑 9 用例 oracle + mutant（W2）/ parity 8 + 4 mutants（W5） | W2、W5 |

> **注**：O-1–O-12 是 owner 裁决点，不是技术结论。本报告不替 owner 做选择；在裁决前，相关候选不进入 A/B（`[P3] §5`、`[P3R] §2.6`）。O-5 的 δ 说明、O-10 的 `NO_GO_ABORT` 更正、O-2 的 C2 `NEEDS_OWNER` 是 `[P3R]` 的修订落点。

---

## K. Issue #621 Completion Recommendation

### K.1 所有合理路径是否已验证？

**在 current contract 内，可以执行的合理路径已经全部裁决；严格意义上不是 100% “已验证”。**

- **已验证/已裁决（R/V）**：
  - W1 的四个方向族（A/B/C/D）全部检查：D1/D2/D3/C1/A4–A8 语义阻断；C3 为 `NO_GO`（本战役范围：epoll/timerfd/relay 无净减量，不是 fd 语义冲突）；A1/A2/A3/B 不 material；C2 契约允许但需 owner。
  - W3 的四个成员（M-61/M-62/M-67/M-10）全部检查：P4 给出了候选级 owner-total 点估计（`offer_terminal` 1.80%、`claim_execution` 1.56% 等），但 P4R 判定其为方向性证据；单候选可删量仍 `NOT_OBSERVABLE`，M-61 有 R3 0.45% 反例。
  - M-53 的三项义务：O1/O2 PROVEN，O3 INCONCLUSIVE；P4 d64 点估计可复现但不显著；app owner 1.13% 低于 MDE80 1.416%、高于 t-hw 1.0066%，all-comm period owner 0.63% 才低于两者。
  - M-42：算术上界 ≤0.14% cycles，低于噪声；`NOT_POWERED`。
  - `#637` 的 22 个候选 + 3 个非候选：没有 `NO_GO`/`NO_GO_ABORT` 被翻案，也没有新的 GO；M-16 的 owner-seam 分类（→ `SEMANTIC_MISMATCH`）与 M-42 的 `NOT_POWERED` 是本轮裁决，不改变“没有候选达到 A/B 准入”的结论。
  - A/A：9 个有效格、144 对；复算可复现；C2/C5 无效已定位。
- **未验证/未执行（U）**：
  - W1-C2（第二通知通道）——需要 owner O-2 授权；未设计、未建模、未 A/B。
  - W3 的候选级行级归因/步间语义——本轮有 P4 的候选级 owner-total 点估计，但仍缺“可删量/步间语义/匹配 A/A”，需要 debug build/隔离格或设计证明；不能把 P4R 的方向性证据当定量准入。
  - M-53 的 O3（packed 编码）——未在 HEAD 实现；H1 patch 未应用、未运行。
  - M-53 的 d64 效应——只有 P4 点估计；没有 d64 cycles-only A/A，也没有把 d64 效应测到 δ=2%/1%/0.4% 所需的对数（0.4% 是乐观代理假设，不是实测）。
  - `#604` G0 真实消费者 profile——未准入；所有 profile 都是 bench/reference 形态。
  - `#617` G1 kernel-boundary——未满足；W1 若升级为 kernel-boundary 研究需另立项目。
  - 完整构建测试矩阵、ASAN/TSAN、TLC——未运行。

**结论**：不是“所有路径都已性能验证”，而是“所有在 current contract 内可执行的路径都已裁决为 A/B 准入失败、语义阻断或本战役范围 `NO_GO`”。剩余的三条设计门（W1-C2、W3 行级、M-53 O3/d64）都需要 owner 改变范围/预算或另立项目，不属于 #621 可自行继续的路径；它们不是“状态确认”，而是 O-4/O-5 等 owner 资助决定。

### K.2 哪些真正减少了机器工作？

**本战役没有产生新的减量。** 真正减少机器工作的是 **BASE 之前或 BASE 本身已合并** 的工作（其中 `#634`/`#635` 有量化减量；`#609` 是相关历史负结果）：

| 工作 | 减量（历史回执，I） | 口径/限制 |
|---|---|---|
| `#634` ThreadPool owed-count gate | `sluice-copy` 256 MiB、4 KiB/d4/1 worker、btrfs warm、24 对：CPU/task **−5.58%** [−6.45,−4.71]、wall **−6.53%** [−7.55,−5.53]、user instr **−22.1%** | `PARTIAL_CPU_ACCOUNTING`；负控制 64 KiB/d1 +0.10% [−1.56,+1.77]；**未预注册非劣效界值**，CI 上界 +1.77% 不能排除小幅 CPU 回归（`[L] §3.1`） |
| `#635` ProgressSource 单读 drain | 36 对：instructions **−5.0180%** [−5.7603,−4.2987]、cycles **−5.1111%**、task-clock **−5.4653%**、rusage CPU **−5.3199%**；A/A +0.7713%、宽度 2.4090 | 只 `progress_source.hpp` +47/−3；测试/oracle/mutant 在 BASE；证据目录不在 BASE；TP 证据不外推到 io_uring（`[L] §3.2`） |
| `#609` ELIDE | `E5_MECHANISM_VERIFIED / END_TO_END_WIN_NOT_DEMONSTRATED`；四个 native W02 格在噪声内 | 历史负控制/重开抑制先例；不是本战役新增（`[L] §3`） |

**本战役的新增减量 = 0**：没有产品改动、没有候选 A/B、没有合并。任何把 `#634`/`#635` 的数字写成“本轮成果”的说法都是错误的。

### K.3 哪些不能优化？

| 类别 | 项 | 原因 |
|---|---|---|
| **当前契约下语义不能动** | W1-D1/D2/D3、W1-C1、W1-A4（全局）、W1-A5–A8 | fd 共享文件状态标志/非阻塞 ack/有界 timeout/单 eventfd 注册/第二 drain authority/丢唤醒/control sticky；不是性能不足 |
| **本战役范围 `NO_GO`** | W1-C3 | epoll 仍需 read；timerfd 增加 fd/syscall；relay thread 是新执行资源和第二通知权威；不是 fd 语义冲突 |
| **契约允许但需 owner** | W1-C2（TP condvar 双通道） | 需要第二通知通道/新模型/新 oracle；`NEEDS_OWNER` |
| **没有候选级可删量** | W3 M-61/M-62/M-67/M-10；W4 M-65（owed 家族） | W3 有候选级 owner-total 点估计（约 0.3–1.8%），但单候选可删量 `NOT_OBSERVABLE`；publication 4.13% 是容器，不是候选上界；M-61 R3 0.45% <1% MME |
| **低于噪声/不可测** | M-42 | `lock addq` 19–20 cycles → ≤0.14% cycles；1 instr 0.0041–0.0094%；W-02 16 对 MDE 远大于信号；需隔离格 |
| **已否决/不重做** | M-28、M-61 payload、H1、io-wq punt、tp-w8 | `NO_GO_ABORT`/`NO_GO`；无新证据不复活 |
| **owner 裁决面** | M-09/M-30/M-57/M-12/M-16/M-80 hygiene | 与 root/契约语义或 seam 义务冲突；性能实验无意义 |
| **经济上不可行（条件）** | M-53 d64（若效应接近 d16 的 0.4% 量级） | 0.4% 是乐观代理假设，不是 d64 实测；若效应为 0.4%，`hw≤效应/2` 需 336/346 对，A/A+A/B ≥524 对；P4 点估计 1.06%/1.13% 不显著，尚未证明大于 0.4%；CPU 指标 2% 需 370–371 对 |

### K.4 剩余空间是否还有投入价值？

**在 #621 的当前契约、当前环境、当前预算内：没有。**

- W1 的唯一 material 替代（C2）不是“再测一次”，而是通知拓扑重构；它需要新模型/新 oracle/机制门，且 host 仍要 eventfd；这是新项目（`#617` G1 或独立设计），不是 #621 的收尾实验。
- W3 的 RequestCore 单函数 owner 约 0.3–1.8%（P-D `offer_terminal` 1.80%、`claim_execution` 1.56%；P-C `offer_terminal` 1.76%；P-E `claim_execution` 1.19%），且大部分是“必要 mutex + 内核/worker 工作”。**W3 包级设计/资格预算为 M（2–4 个臂组，A/A + A/B 72–144 对）；单候选 A/B 对数应按匹配格和预注册 δ 另算**（例如 P-E C1 CPU 2% 需 4/5 对，P-C C4 CPU 2% 需 20/38 对）。P4 的 owner% 是候选成本点估计，不是可删收益，不能直接与包级预算比较；M-61 还有 0.45% <1% MME 的反例。
- M-53 的 d64 如果在 1% 量级，需要 58 对 cycles A/B + 58 对 A/A（δ=1%）或 17–20 对（δ=2%）；如果只有 0.4%，需要 172/336 对。当前证据（P4 点估计 1.13% app / 0.6% all，不显著）不足以支持“值得投入”的判断；先证明 d64 效应显著大于 0.4% 才可能改变结论。
- M-42 的效应 ≤0.14% cycles，低于噪声；隔离格的工程成本高于潜在收益。
- `#604` G0 未准入，任何 application-level 收益声明都不可用；`#617` G1 未满足，kernel-boundary 研究不能进 #621 的 A/B 队列。

**因此：`#621` 在当前范围内不应继续启动新的 A/B；W3/M-53 的设计门是否继续，由 owner 通过 O-4/O-5/O-6/O-8 决定预算。** 若 owner 决定投入，应把它重新定义为 `#617` 的 kernel-boundary 项目（C2/D）或 `#604` 的真实 workload 项目，并重新预注册，而不是继续 #621 的 A/B 队列。

### K.5 最终建议与剩余阻断

**建议：`NO_FURTHER_OPTIMIZATION_JUSTIFIED`（当前没有可进入 A/B 的候选；W3/M-53 设计门仍开放，需 owner 决定是否资助）。** 本战役没有新增减量；当前不建议启动新的 A/B。

- **O-2 是唯一涉及通知拓扑语义授权的 owner 点**：若 owner 拒绝，C2 不能进入设计/建模，更不能进入 A/B；但这不表示其余 owner 点都只是“归档”。
- **O-4（W3 行级归因/步间语义）、O-5（M-53 O3 + d64 cycles-only A/A）、O-6（M-42 隔离格）、O-8（M-01 `-scope64`）是设计/资格实验，不是状态确认**；在 owner 未资助/未预注册前，它们保持开放。O-1 是 W1 的 profile 范围门；O-3 是 W1 预注册分母定义；O-7/O-9/O-10/O-11/O-12 是裁决/归属/归档确认。
- 若 owner 拒绝 O-2 且不资助 O-4/O-5/O-6/O-8，则 #621 的当前研究范围可关闭；但这是 owner 的决定，不等于 W3/M-53 的设计门已验证。

**剩余阻断（明确）**：通知拓扑语义授权 = **O-2**；当前没有可进入 A/B 的候选；W3/M-53 设计门与 M-42/M-01 隔离格/profile 仍开放，需 owner 决定是否资助。O-2 只是通知拓扑语义授权点，不是总体上的唯一未完成项；O-4/O-5/O-6/O-8 不是状态确认。

---

## 附录 A：来源索引

| 标签 | 路径 | 本报告主要使用节 |
|---|---|---|
| `[L]` | `docs/evidence/621-progress-wait-path-20261118/CAMPAIGN-LEDGER.md` | A/B/C/D/F/J |
| `[P1]` | `.../p1/AA-CALIBRATION.md` | B/F/G |
| `[P2A]` | `.../p1/AA-CALIBRATION-2.md` | B/F/G |
| `[P2M]` | `.../p2/P2-COST-MAP-R1.md` | C |
| `[P2R]` | `.../p2/P2-REVIEW.md` | C/E |
| `[P3]` | `.../p3/P3-ADMISSION-R1.md` | D/G/I/J |
| `[W1D]` | `.../p3/W1-DESIGN.md` | D/E |
| `[P3R]` | `.../p3/P3-ADMISSION-REVIEW.md` | D/G |
| `[P4W]` | `.../p4/W3-ROWLEVEL.md` | C/E |
| `[P4R]` | `.../p4/W3-ROWLEVEL-REVIEW.md` | C/E |
| `[M53]` | `.../p4/M53-OBLIGATIONS.md` | E/G/I |
| `[B1]` | `.../p1/build-manifest.md` | A |
| `[B4]` | `.../p4/rowlevel-build-manifest.md` | A |

## 附录 B：本会话命令与输出（关键摘录）

```bash
# Git / worktree
git rev-parse HEAD                      # 5d94f233967c3aef993eb19ba9e3252c532b0951
git rev-parse origin/master             # 同
git worktree list --porcelain           # 单 worktree，master @ BASE
git stash list                          # 空
git diff --stat; git diff --cached --stat  # 无输出

# Ancestry
git merge-base --is-ancestor 3f22d2d0 HEAD   # true
git merge-base --is-ancestor 5d94f233 HEAD   # true
git merge-base --is-ancestor 98825793 HEAD   # true
git merge-base --is-ancestor 0e50612c HEAD   # false

# Toolchain / environment
gcc --version | head -1                 # GCC 16.2.1 20261029
xmake --version | head -1               # xmake v3.1.2+HEAD.5d21e9d
pkg-config --modversion liburing        # 2.13
uname -r                                # 7.3.3-200.fc44.x86_64
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor   # schedutil（当前）
cat /sys/devices/system/cpu/intel_pstate/no_turbo           # 0
cat /proc/sys/kernel/perf_event_paranoid                    # 2
stat -c %s /sys/kernel/btf/vmlinux                          # 7023892
stat -f -c %T /tmp                                          # tmpfs
findmnt -no FSTYPE,SOURCE,OPTIONS /                         # btrfs /dev/sda3[/root] ... compress=zstd:1

# A/A recompute
cd docs/evidence/621-progress-wait-path-20261118
python3 -I tools/recompute_independent.py --aa-root p1/aa  --json-out /tmp/final-aa1.json  # exit 0
python3 -I tools/recompute_independent.py --aa-root p1/aa2 --json-out /tmp/final-aa2.json  # exit 0
python3 -I p2/profile/analysis/recompute_p2_r1.py --json-out /tmp/final-p2r1.json          # exit 0

# Independent t/MME（一次性第二实现，未入库；归档 requiredN 字段亦作交叉验证）
python3 -I /tmp/final_tcheck2.py
# t_.975(15)=2.131450 t_.80(15)=0.866245
# C7 16 tHW=1.0066 MDE80=1.4157 tHW17=0.9712 tHW20=0.8841 tHW58=0.4967

# M-53 existing-binaries tests
./build/linux/x86_64/release/uring_pending_control_parity_test            # 7/7, exit 0
./build/linux/x86_64/release/backend_contract_conformance_uring_test      # 20/20, exit 0
./build/linux/x86_64/release/uring_request_core_integration_test          # 21/21, exit 0
```

## 附录 C：不运行 / 不可观测清单（U）

- `xmake f/build/test`、完整矩阵、ASAN/TSAN、TLC、`scripts/verify_progress_source_model.sh`。
- 任何新的 perf/record/stat/eBPF/uprobe/strace；任何新的 A/A/A-B。
- 任何候选 build/SHA；任何产品代码改动。
- `#604` G0 真实消费者 profile；`#617` G1 kernel-boundary。
- P4 `analyze_w3.py` 的本次重跑（`[P4R] §1` 已记录其结果；本会话未重复）。
- 测试二进制 clean rebuild（M-53 三个二进制是 2026-11-12 既有产物）。
- 行级内核/libc/libstdc++ 归因；skid/PEBS 误差；d64 平均扫描深度；逐请求因果链。

---

## Owner Gates（O-1–O-12 修订后；完整表见 §J.4）

1. **O-1** W1 profile 门：等 `#604` G0，或只做设计/A-A；接受 bench/reference 代理只能作 mechanism-level/bench-proxy。
2. **O-2** W1 通知拓扑：拒绝在 `#621` W1 内做第二通知通道；若追，另立 `#617` G1/kernel-boundary，带新模型/新 oracle/机制门。
3. **O-3** W1 的“5%”分母：process cycles / measured-loop cycles / measured-loop CPU；未定义则代理 A/B 只能记 `INCONCLUSIVE`。
4. **O-4** W3：接受 δ=2% 并资助行级归因（P-D/P-C/P-E 主形态）；P-D 先重跑 C2b A/A；不直接 A/B。
5. **O-5** M-53：接受 cycles + δ=2% 的 17–20 对；**17–20 不能用于 δ=1%**；δ=1% 需 58；若效应接近 0.4%（乐观代理假设，不是 d64 实测）需 172/336（A/A+A/B ≥524）。
6. **O-6** M-42：保持排队；`GO` 排队 ≠ A/B 准入；资助隔离格前不启动。
7. **O-7** 语义冲突批：M-09/M-30/M-57/M-12/M-16/M-80 hygiene 逐项 KEEP/DELETE/修 root；**W1-C2 不在此批**。
8. **O-8** M-01：先做 `-scope64` 隔离 profile，再裁 free-list（owner S2）。
9. **O-9** M-37/H1/tp-w8：确认 `#595`/`#604` 归属；`#621` 不启动。
10. **O-10** M-28：确认 **`NO_GO_ABORT`（不重做）**；不是 `NOT_APPLICABLE`；仅 ledger §4.1 四要素同时满足才重开。
11. **O-11** M-61 payload：维持 `NO_GO`；除非出现生产消费者或 owner 批准测试专用通道。
12. **O-12** W2/W5：确认无新候选；任何改动重跑 9 用例 oracle + mutant（W2）/ parity 8 + 4 mutants（W5）。

> **剩余阻断**：**O-2** 是唯一涉及通知拓扑语义授权的 owner 点；W3/M-53 的设计/资格门（O-4/O-5）与 M-42/M-01 的隔离格/profile（O-6/O-8）仍开放。若 owner 拒绝 O-2 且不资助这些设计门，#621 的当前研究范围可关闭；总体判词为 **`NO_FURTHER_OPTIMIZATION_JUSTIFIED`**，不声称研究已完成。

---

*报告结束。所有未在 §A.6/§G.1 执行的断言均为“未运行/未验证”；所有数字都标有来源与口径；不把 profile 上界当收益、不把 owner 裁决写成技术结论、不把未运行写成通过。*
