# #595 R4 — B 臂描述性补充块事前附录（PREREGISTERED §11.4 机制）

- 附录角色：预注册 `PREREGISTERED.md`（sha256 `fbb2e785…`，冻结）§11.4 规定
  "本轮若需 B 数据只能以事前附录开描述性补充块，不进门槛"。本附录即该事前附录，
  于**任何 B 补充计时开始之前**写盘（写盘 UTC 见文末回执行；sha256 由运行回执
  外部记录，不自引用）。
- 触发来源：本轮任务书（补全测量 ask）要求"在 W03-d16 跑 B 的 24 个描述性
  blocks（仅供 R−B 与 F3 对比，不进任何门槛）"。

## 1. 格（cell）与冻结对象绑定

| 项 | 值 |
|---|---|
| 格 | B-SUPP-W03-D16（描述性补充块；非 §11.1 冻结块清单成员） |
| 形状/case | `W03-uring-4k-d16`（= 主形状，精确 filter `=W03-uring-4k-d16`） |
| 二进制 | `r4/build/linux/x86_64/release/bench_w03_external_loop`，sha256 须逐 invocation 等于 `f029607fc1a657efe0ba96203115f27a90d598f12c274e9360b6029a5e019009`（§1.2 冻结值；失配→invocation 无效，禁止重建/替换） |
| 绑定/频率 | `taskset -c 2`；WARMUP=1 / MEASURED=5；`SLUICE_BENCH_STATS=0`；fixture = warm tmpfs 64MiB `/tmp/sluice-595-r4m/external.bin`；无诊断 env（`SLUICE_595_R4_DIAG_LINES` 出现→无效，§10.2） |
| 协议来源 | 计时/CPU/延迟口径、invocation 记录字段、§10.1 环境门、§10.2 有效性判据全部沿用冻结预注册（与 MAIN-W03-D16 同一实现路径 `scripts/matrix_driver.py::run_invocation`，不改任何冻结对象） |

## 2. 臂（arms）

- **B**（`SLUICE_595_R3_ARM=B`，R3_FIXED_PREFIX：整跨度一次呈现、入口内 cap=8、
  untouched 后缀走 singles；冻结语义 §1.3/§1.6）——本补充块的对象。
- **R**（`SLUICE_595_R3_ARM=R`，READY_REUSE）——同块配对锚，仅为给 R−B 提供
  **同会话 matched pair**；R 数据本身不是本附录的新结论对象。

## 3. block 数与顺序

- **24 个 matched blocks**；块内两臂各恰一次 invocation（B、R 各 24 invocations，
  共 48）。
- 顺序规则（确定性，无 RNG）：block j（0 起）顺序 = j 为偶数 → `[B, R]`；
  j 为奇数 → `[R, B]`。每臂位置 0/1 各 12 次（沿用 §5.2 A/A 块的奇偶交替精神；
  B 补充不进入任何平衡性门槛声明）。
- 块内臂间不插休眠/调参；invocation 间只做 §10.3 记录动作（与主矩阵 driver 同
  一实现）。

## 4. 描述性地位声明（冻结沿用 §11.4）

1. 本块全部数据**只作描述性用途**：不进 §7 任何门槛，不进 F-MAIN/F-PROT/
   F-VALUE 家族，不参与 Holm 校正，不产生 PASS/FAIL，不改变任何已测格的结论，
   也不得被引用为 library-wide 结论（methodology.md 边界沿用）。
2. 指定对比对象：
   - **R−B**：本会话 24 个 matched pair 内差值（`d=(X_B−X_R)/X_R`，吞吐
     `d=(tput_R−tput_B)/tput_R`，与 §5.3 同式），报告点估计与描述性双侧 95%
     percentile bootstrap CI（10,000 次、seed 5950，沿用 §5.3 实现口径；CI 仅
     描述，不设界、不判定）。
   - **F3**：任务书指称的描述性对照编号。冻结预注册与伴生文档（PREREGISTERED/
     BASELINE/DESIGN）均未定义 r4 的 F 编号；按任务书原话登记为"仅供 F3 对比"
     的描述性用途，不构成任何门槛或结论翻转依据。
3. 交叉语境（仅背景，不进对比结论）：MAIN-W03-D16 的 24 个 R invocations
   （同冻结二进制/同协议、今日更早会话）可作会话漂移的描述性参考；B−R 的正式
   对照需求按 §11.4 由 R3 完成轮（同形状、旧二进制）满足，本轮数据不替代。
4. A/A 门（§6）对 F-MAIN 等门槛的先决关系**不因本补充块改变**：本块不受 A/A
   门约束也不依赖它；描述表脚注将注明这一点。

## 5. 有效性、重试与中止规则（沿用冻结判据，不新增不削减）

- invocation 无效判据 = §10.2 全部条目（exit≠0 / oracle 失败 / 记录数不符 /
  ops≠20000 / 二进制 sha 失配 / 臂读回不符 / 计时块出现诊断 env）；
  SHADOW 专属条款（§1.4 shadow_outstanding_sum）对本块不适用（无 SHADOW 臂）。
- 无效 → 同槽位重试 ≤2 次（共 3 次尝试，全部保留于本块 `attempts.jsonl`）；
  3 次仍失败 → 该 block 记 `done_with_failed_slots`，**不补样、不替换**（§5.5）。
- 有效 block = 两槽位全有效。24 个有效 block 为目标；不足如实记 `INCOMPLETE`，
  不追加。
- 环境门 §10.1：每 block 前 governor=schedutil、load1≤4.0、cpu2/cpu12 无
  CPU-bearing 外来进程（等待 ≤30min，仍占用→`BLOCKED_BACKGROUND`）、/tmp ≥1GiB、
  fixture sha/块内不变；触发即整会话中止并保留已有数据。
- 预算：本补充会话 ≤30 分钟墙钟（48 invocations × ~1.5–3s + 环境门开销）；
  超时 → 剩余 block 记 `NOT_RUN`。

## 6. 锁与记录纪律

- 整个补充计时会话持 `/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock` flock 独占
  （fd 9 语义、1800s 获取窗）；LOCK_RECEIPT 记录获取/释放 UTC、等待时长、
  `fuser -v`、第二 fd `flock -n` 排他性检查（须 blocked）、会话命令清单；
  会话前后记录 load、cpu2/cpu12 占用、`df -h /tmp`（沿用 matrix_driver 的
  LockSession 实现）。期间禁止并行编译/压缩/归档/其他实验。
- 原始数据布局：`raw/bsupp/B-SUPP-W03-D16/`（header.json、results.jsonl、
  attempts.jsonl、block-XX.json、logs/、jsonl/、RECEIPT.md），不写入
  `raw/matrix/PROGRESS.json`、`raw/matrix/DONE`（主矩阵记录保持原样）。

## 7. 不改动声明

本补充块不修改任何冻结对象：不改 PREREGISTERED.md、不重建/替换 r4/build 二进制、
不动主工作树与他人 worktree、不推送、不写 GitHub；主矩阵（含 TRAJECTORY 失败
留痕）保持逐位原样。

---

- 事前附录写盘 UTC：2026-11-06T04:06:30Z（先于本补充块一切计时 invocation）
- 撰写者：R4 补全测量员（任务书 ask 项 3）
