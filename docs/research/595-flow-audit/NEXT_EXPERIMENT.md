# #595 Flow Audit — 唯一下一项实验协议：C-A（产品 transport 层 enter 摊销）

DESIGN ONLY — 本轮未执行。基线冻结：PRODUCT master `271b81bf`。
实现落位：独立研究分支（自 master 切出），**不进 master、不改公开
API/请求契约；默认产品行为不变**（研究分支上以环境变量臂化）。

## 1. 研究问题与干预

- 可被推翻的问题：**产品 transport 层自行摊销 `io_uring_submit`
  （Q=8 延迟 flush）能否在 warm-buffered W03-d16 上复现 R 族的
  CPU/吞吐收益，而 p99 不超保护门——即 d16 p99 差与浅队列税是否
  主要属于研究呈现层而非摊销本身？**
- 唯一干预：`UringAsyncBackend` 提交路径改为——`dispatch_one_locked`
  照常准备 SQE/ledger；`submit_transport_locked` 不再在每个
  `dispatch_after_accept` 内调用，改为满足任一条件时执行：
  (a) 本 burst 内已准备 SQE 数 ≥ Q=8；(b) `dispatch_retry_remains`
  residue 广告（现有 `signal_ready_progress` 路径不变）；
  (c) 任何 `poll()` pass 开头的既有 residue flush（现有
  `M:uring_backend.cpp:620-634`，不变）。A0 臂 = 同分支、
  `SLUICE_URING_FLUSH_Q=1`（等价现行为，逐语句可证）。
- 对照变量：同 bench、同 fixture、同线程拓扑、同锁；唯一自由度 =
  flush 触发。无 adapter、无 seam、无决策表参与任何臂。

## 2. 形状与对照

- 主形状 W03-uring-4k-d16（R 族唯一 FAIL 项所在）。
- 必要对照（不机械补全矩阵）：W03-d4（R 曾全面变好，摊销不得倒退）、
  W03-d2serial + W02-d1 + W02-d2（浅队列：直接测"研究层移除后税是否
  消失"与"residue 信号跳的代价"——C-A 在深度 1 时每 op 依赖
  signal→epoll→pass 才进内核，这是本设计最大自损风险，必须放保护格）。
- 连续性参考臂：R_cleanup（`b191bb89` 二进制）仅在 MAIN 的 6/24 块
  并跑作锚，不进门。

## 3. Useful task 与端点

- 同一 useful task：20000×4KiB warm tmpfs 读、逐块 pattern 校验
  （oracle 相位）、顺序应用端点（front-only take）不变。
- **延迟起点（两臂统一，预注册）**：T0 = ReadOp 构建完成、submit
  调用之前（产品 bench 现为 submit 返回后；本次实验两臂同步前移，
  与研究 A0 语义一致——含自身提交成本与 flush 等待，不隐藏任何
  pre-kernel 等待；不允许丢弃/重建已 ready 请求或重置 T0）。
- 终点 = take_result 返回。错误/取消/借出/唯一 terminal/publication/
  shutdown 语义全部走现有套件；内存预算沿用 RSS +2% 门。

## 4. 正确性 oracle 与负控（先于计时）

- 既有套件全绿（async/uring 全量 + fault 注入 + shutdown 生命周期 +
  submit concurrency）。
- 18 场景确定性 transport oracle 改造版：逐请求结果字节不变；flush
  计数按 Q 断言（`enters=ceil(n/Q)+residue 次数`，在受控 seam 下
  精确断言）。
- **杀死性负控**：mutant 删除 residue 广播（(b) 通道）→ oracle 必须
  挂起/失败，证明 undispatched 工作依赖该信号才能最终进内核（PROG-01
  residue 义务的真实性检查，而非只数完成数）。
- 新增确定性回归：SQ 持有 <Q 未 flush 工作时，progress pass 触发
  flush 并发布（无第二次 submit）。

## 5. 统计与噪声（先冻结）

- 24 配对块/格，AB/BA 交错，块为独立单元；逐块 p99 → 配对 bootstrap
  单侧 95%；MAIN 四假设 Holm。复用 r5-repair 的
  `analyze_round.py`/`run_round.py` 家族（复用协议，不保证所有形状
  功效充分；A/A 半宽并列报告，精度不足记 NOT_ESTABLISHED，不补样）。
- 双锁、LOCK_RECEIPT、无并行构建；诊断与干净计时隔离。

## 6. 性能端点与口径

- MAIN（C-A−A0）：CPU 点 ≤−10% 且单侧上界 ≤−5%；吞吐下界 ≥−3%；
  p95/p99 单侧上界 ≤+5%（R4 全口径）。保护格：CPU 上界 ≤+5%、
  吞吐下界 ≥−3%、p95/p99 上界 ≤+5%。
- CPU 主口径 = CLOCK_PROCESS_CPUTIME_ID（沿用）；附 rusage utime/stime
  与线程 tick 桶；io-wq ticks 单列不并入。PMU 不可用（paranoid=2）
  → 不做用户/内核 PMU 拆分，登记；不得宣称全系统 CPU。
- kernel entries：由 Q 与 submitted 推导 + oracle 精确断言（见 §4）。

## 7. 三种结论与停止规则

- 成功：MAIN 全过且保护格全过 → 下一轮才讨论产品化（默认值/配置
  面、机制正当化审计：flush 触发=SQ 占用的派生摘要，非新权威）。
- 失败（任一门 FAIL）：停止 C-A，**不做 Q 搜索、不自动转入 cap/阈值
  调参**；结论二选一并写明：p99 FAIL → "摊销时机本身在后位成员上
  有不可接受代价"（此时若 d2serial/W02 同时变好，则浅队列税确证
  属研究层）；浅队列 FAIL → "residue 信号跳在稀到达下不可接受，
  摊销需要到达密度前提"。
- 精度不足：A/A 不达的格按 NOT_ESTABLISHED 记录，不重跑到过。
- 候选 B（残差感知预算）维持不放行；本实验不依赖逐请求归因仪器。

## 8. 预算

约 5 格 × 24 块 × 2 臂 + A/A + oracle/负控构建：一个测量日以内；
无新线程/定时器/每请求分配（静态审计 + RSS 门沿用）。
