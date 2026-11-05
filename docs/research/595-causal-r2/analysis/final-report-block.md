# FINAL REPORT BLOCK — Sluice #595 causal-r2（任务 §14）

RESEARCH_ROUND: R2
ISSUE: #595
HOST: E5（Xeon E5-2666 v3，10C/20T，20 逻辑核；kernel 7.2.11-200.fc44.x86_64）
BASE_SHA: 271b81bfc9d64eecc0266fe7066b9040525dd710
EVIDENCE_ROOT: /home/jnhu/sluice-595-e5/causal-r2/evidence
MANIFEST_PATH: /home/jnhu/sluice-595-e5/causal-r2/evidence/MANIFEST.sha256
MANIFEST_FILES: 4739
MANIFEST_VERIFY: cd /home/jnhu/sluice-595-e5/causal-r2/evidence && sha256sum -c MANIFEST.sha256 → 4739/4739 OK
ARCHIVE_STRUCTURE: README.md; CAPABILITY_MATRIX.md; PROTOCOL.md; CAUSAL_DAG.md; COST_CLASSIFICATION.md; EVIDENCE_GAPS.md; OBSERVABILITY_LIMITS.md; RESULTS.md; ROOT_CAUSE.md; raw/; traces/; counters/; counterfactuals/; analysis/; MANIFEST.sha256

CONCLUSION: D2 的 CPU 退化（+3.70%）与延迟分布整体抬升（p50 +6.91% / p95 +4.96% / p99 +7.07%）均由 B·BOUNDED_PROGRESS 的 defer 决策链承担；D3/D16 的改善（−6.69% / −26.36%）也在同一 defer 链内，但机制分解与恢复复验不完整。只归因、不修复；未改公开 API 与 v2 契约。
CPU_CAUSE: ESTABLISHED（源级；D2 +3.70% CPU/op；源=defer 决策链的 residue signal→eventfd write；CF-1 归零 −0.26% [−0.67,+0.16]、CF-2 归零 −0.76% [−1.61,+0.06]、restore 精确重现；82% 为上限口径，非精确分解）
LATENCY_CAUSE: ESTABLISHED（源级：L1 主导；CF-1 保留 +4.30%/+4.87% 排除 L0 主导，CF-2 归零 −0.71%/−0.83%；restore 同向/同域复现，分位数 p50 差 2.4–5.7pp）
TAIL_MECHANISM_L2: PARTIAL（p99 超出 p95 的 ~+320 ns 与段级分解 G12 未观测；≥96.2% 是 p99 总效应的界，不是附加尾部的界）
DEPTH_CAUSE: PARTIAL（D3 −6.69% / D16 −26.36% 的改善在 defer 链内、干预消除到 +1.07%/+0.26%；但无 restore，且 enter 摊销只是最大单项：提交线程内核 ~54.8%/50.3%，io-wq 29.1%/21.0% UNOBSERVED，用户 16.8%/28.8% 间接）

DIRECT_MEASUREMENT: D2 线程 CPU 分解（用户 +95.24、提交线程内核 +79.37、io-wq +7.94 ns/op）；计数器 sqes_deferred 0.5589/op、flushes=actual_enters=1.0000/op；in-situ TSC signal 277.5 ns/call、write 592.5 ns/call、flush 702.3→714.4 ns/call；syscall syscw +86,912/inv、8.005 B/write、+138.25 ns/op；PMU cycles:u +5.71% / instructions:u +1.06% / IPC −4.36%；schedstat runnable +36.4 ns/op（on-CPU 增量的 7.7%）；trace flush-size 与 defer→flush delay（D2 617/1234/2777 ns）；strace enter 参数直方图；xlayer 11 节点 join 6000/6000、零丢失
COUNTERFACTUAL_SUPPORT: CF-0 C−A ≈0（D2 −0.52% [−1.28,+0.22]；D16 −0.13% [−1.45,+0.82]）排除门链本身；CF-1 sigdrop 使 CPU 增量归零而延迟保留；CF-2 deferoff 使 CPU 与延迟同时归零，restore 重现（CPU +3.94% [+2.60,+5.26]）；同一 deferoff 二进制在 D3/D16 各 8 对使改善归零（+1.07% [−0.47,+2.80] / +0.26% [−0.69,+1.04]）
UNOBSERVED: C5 指令级来源；eventfd write 内部阶段拆分；L2 段级机制与 p99−p95 附加尾部构成；D3/D16 的 io-wq 内核 CPU 下降机制；D3/D16 native submit delay；D3 p95/p99 残差成因；depth 二进制上的 595 oracle
OBSERVABILITY_LIMIT_REACHED: 非 root `:k` PMU（perf_event_paranoid=2）；tracefs root-only；`/proc/PID/delay` 不存在；10 ms utime/stime tick；`scaling_cur_freq` 非实测频率；CF-1/CF-2 会话 root bpftrace `UNOBSERVED_ADMIN_UNAVAILABLE`
CORRELATION_PARTIAL: C5 微架构停顿 ≈68–71 ns/op（PMU 事件级，无 PEBS/LBR）；xlayer 的 CORRELATION_PARTIAL 边界（block 层 N:M 未观测）
EVIDENCE_GAPS: G1 内核 PMU/syscall 时长；G4 D2 逐请求时序的 native 缺口；G7 CF-1 后 PMU 回落；G10(b)(c) 已由 CF-1/CF-2 排除；G12 延迟段级直方图未运行；G5/G6 块间绝对漂移与频率；R1 sanitizer lane 与全 mutant lane 未重跑
CORRECTNESS: phaseA 240/240 invocation exit 0、100k op；CF-1 96/96、CF-2 96/96；CF-0 204/204；CF-1 13/13 B-mode rc=0；CF-2 BASE GREEN + mutant receipt 22 FAIL rc=134（设计内拒绝）+ 12/13 B-mode；所有 timing 块 bench 字节 oracle 通过
PROTOCOL: W02-uring-4k warm tmpfs，20,000 reads/rep，WARMUP=1 MEASURED=5，taskset -c 2，AB/BA 配对，bootstrap 10,000 seed 5950；仪器块与 timing 块分离
SAMPLE_SIZE: phaseA 5 轮 ×24 对；CF-0 24 trio（D2/D16）+12 trio 重复+12 对恢复；CF-1/CF-2 各 4 块 ×12 对；depth 计数器 12 对/深度、deferoff 8 对/深度、trace 3 phase×20k dispatch、strace 3 次/臂/深度
PROVENANCE: 基线 271b81bf；冻结 timing 61c6d579…（phaseA）/0091d259…（CF）；sigdrop 4de23299…；deferoff 64189aa8…；counters 601dad6a…/b9a49946…；PMU 6670d8de…；C1 75534641…；C3 17463539…；trace depth 2f47d5c1…；g++ 16.2.1、xmake 3.1.2、liburing 2.13-2、Python 3.14.8
PRODUCT_CHANGE: NONE
PRODUCT_PR: NONE
MERGED: NO
R1_VERDICT: KEEP_IMMEDIATE（保持不变；本轮 R2 不覆盖、不重写 R1 判定）
R1_EVIDENCE_TOUCHED: NO
MAIN_WORKTREE: clean @ 271b81bf（`git status --porcelain` 空）
FINAL_REPORT_PATH: /home/jnhu/sluice-595-e5/causal-r2/evidence/analysis/final-report-block.md
ISSUE_COMMENT_PATH: /home/jnhu/sluice-595-e5/causal-r2/evidence/analysis/issue-comment-r2.md
