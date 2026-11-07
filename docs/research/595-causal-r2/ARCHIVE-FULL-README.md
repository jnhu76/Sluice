# Sluice #595 — E5 causal-r2 证据归档

状态：**FINAL ARCHIVE（R2，2026-11-05）**。研究问题：Sluice `W02-uring-4k` 形状上
A·IMMEDIATE 与 B·BOUNDED_PROGRESS 的 CPU/延迟差异由什么造成；边界扩展到 D1/D3/D16
的深度机制。只归因、不修复，不改公开 API 与 v2 契约。

**与 R1 的关系**：本轮是独立 R2，**不覆盖 R1 的 `KEEP_IMMEDIATE` 判定**，也不改写
`/home/jnhu/sluice-595-e5/evidence/`（R1 证据只读；其 `MANIFEST.sha256` 从
`/home/jnhu/sluice-595-e5` 基准验证通过）。本轮 `PRODUCT_CHANGE: NONE`、
`PRODUCT_PR: NONE`、`MERGED: NO`。

## 1. 裁决摘要

| 域 | 裁决 | 命题范围 |
|---|---|---|
| CPU（D2 退化） | **ESTABLISHED（源级）** | +3.70% CPU/op（+189.15 ns/op）由 defer 决策链的 residue 通知（`signal_ready_progress()` → eventfd `write`）承担；**82% 是上限口径，不是 100% 精确分解**（残差 ≈27–35 ns/op UNOBSERVED） |
| 延迟/尾（D2 p50/p95/p99） | **ESTABLISHED（源级）** | 分布整体抬升（p50 +6.91%、p95 +4.96%、p99 +7.07%）由 **L1 deferral 时序**（flush 从 dispatch 推迟到下一次 poll）承担；**L0 主导被排除**（信号残余约 1pp 未单独量化） |
| 尾部分量 L2（p99−p95 ~+320 ns） | **PARTIAL** | 归属在 defer 链内（CF-2 使 p99 总差归零，保守 ≥96.2%）；段级机制与附加尾部构成 **UNOBSERVED**（G12 未运行） |
| 深度边界（D3/D16 改善） | **PARTIAL** | D3 −6.69% / D16 −26.36% 的改善在 defer 链内（同二进制干预后 +1.07% / +0.26%）；**无 restore**，机制分解（enter 摊销 vs io-wq/用户分量）未闭合 |

完整裁决与逐条门槛：`ROOT_CAUSE.md`。实测总表：`RESULTS.md`。因果图与冻结的反事实
规格：`CAUSAL_DAG.md`。

## 2. §13 归档结构

```
evidence/
├── README.md                    本文件：结构、provenance、校验方法
├── CAPABILITY_MATRIX.md         E5 观测能力矩阵（C1–C5 补充探测、root 恢复、坑）
├── PROTOCOL.md                  R2 phaseA 无仪器基线协议（A/A、D1/D2/D3/D16、偏差）
├── CAUSAL_DAG.md                D2 因果图、节点证据、CF-1/CF-2 冻结规格与门槛
├── COST_CLASSIFICATION.md       成本分类账与候选机制排序
├── EVIDENCE_GAPS.md             G1–G12 缺口与关闭记录
├── OBSERVABILITY_LIMITS.md      观测能力边界（UNOBSERVED / LIMIT_REACHED）
├── RESULTS.md                   实测结果总表（phaseA/成本/延迟/反事实/深度/xlayer）
├── ROOT_CAUSE.md                最终根因裁决（含独立复核与附录）
├── CF0_THREE_ARM.md             CF-0 三臂锚定文档（见下方说明）
├── raw/                         phaseA 五轮原始数据 + 能力探测原始输出
├── traces/                      C1 调用栈、C2 syscall、C3 内核、C4 调度、xlayer 跨层
├── counters/                    cost_table、C1–C4 专题、raw、analysis、bin
├── counterfactuals/             CF-0/CF-1/CF-2：文档、raw、bin、tools、frozen 子清单
├── analysis/                    depth-analysis、ARCHIVE_MOVES、最终报告块、Issue 评论
└── MANIFEST.sha256              全量证据清单（相对路径）
```

**关于根目录的 `CF0_THREE_ARM.md`**：它是 CF-0 的锚定文档，逻辑上属于
`counterfactuals/` 组；保留在根是为了让 `counterfactuals/CF2_DEFEROFF.md` 中的
`../CF0_THREE_ARM.md` 相对引用继续成立，从而保持 CF-2 frozen 子清单的字节可验证性。
`counterfactuals/three_arm/` 同样保留原名，使 CF-1/CF-2 文档中的 `three_arm/...`
相对引用无需改写。

### 2.1 各组内容

- `raw/phaseA/`：`aa-calib-w02-4k-d16`、`ab-d1-w02-4k`、`ab-d2-w02-4k`、
  `ab-d3-w02-4k`、`ab-main-w02-4k-d16`，每目录含 `results.jsonl`、`header.json`、
  `summary.json`、`logs/`、`jsonl/`；另有 `binary-sha256.txt`、`toolchain.txt`、
  `workspace-provenance.txt`、`oracle-smoke-armB.log`。
- `raw/`（根）：perf/bpftrace/trace-cmd 能力探测原始输出与 C 臂补丁。
- `traces/`：`c2-syscall/`、`raw-c2/`（strace/shim 原始）、`c3-kernel/`、
  `c4-sched/`、`raw-c4/`、`perf-callgraph/`（run1 unpinned 保留为方法学记录）、
  `xlayer/`（11 节点事件环 + bpftrace 原始 + 三件套工具）。
- `counters/`：`cost_table.md`、`C1_COUNTERS.md`、`C2_PROBE_OVERHEAD.md`、
  `C3_COUNTERS.md`、`C4_SCHED.md`、`PROVENANCE.md`；`raw/` 下 pmu/split/schedstat/
  sysio/c1/c3/cnt-* 块；`analysis/` 汇总 JSON 与 latency-percentiles；`bin/` 五个
  研究二进制。
- `counterfactuals/`：`CF1_SIGDROP.md`、`CF2_DEFEROFF.md`；`raw/` 下 ab-d2-* 计时块、
  cnt-companion*、sysio-*、trace-companion*、shim-companion-cf2、correctness、meta*；
  `bin/` 九个冻结/变异二进制；`three_arm/`（CF-0 raw/analysis/correctness/tools）。
- `analysis/depth-analysis/`：`DEPTH_MECHANISM.md`、`depth_ledger_summary.json`、
  `raw/`（cnt/trace/strace/enterargs/pmu/split/deferoff）、`tools/` 七个运行/解析脚本。
- `analysis/ARCHIVE_MOVES.md`：目录映射、删除项、完整性验证、frozen 子清单结果。

## 3. Provenance

| 项 | 值 |
|---|---|
| 基线 commit | `271b81bfc9d64eecc0266fe7066b9040525dd710`（= R1 冻结基线；主工作树 clean） |
| R1 研究补丁 | `/home/jnhu/sluice-595-e5/evidence/measurement/patch.diff`，sha256 `7be9d0145a8c268a62d9fa90338b7cda95cefc5f8ad2976e4706cb192fb1eaf0`，未改 |
| 研究副本 | `causal-r2/worktree`（branch `research-595-r2-causal`）、`worktree-timing`（`research-595-r2-timing`）、`worktree-trace` |
| 主机/内核 | Xeon E5-2666 v3（10C/20T）；kernel `7.2.11-200.fc44.x86_64`；native |
| 工具链 | g++ 16.2.1、xmake 3.1.2+HEAD、liburing-devel 2.13-2、Python 3.14.8 |
| 亲和/频率 | bench `taskset -c 2`；sampler/perf cpu3/cpu5；schedutil 请求频率 1.2–1.55/3.5 GHz（非实测） |
| 运行纪律 | 全部 timing 块串行独占；loadavg 峰：phaseA 1.94、CF-1 ≤1.56、CF-2 ≤1.39、depth 0.26–1.35（20 核） |
| 样本量 | phaseA 5 轮 ×24 对；CF-0 204 invocation；CF-1/CF-2 各 96 invocation；depth 计数器 12 对/深度、deferoff 8 对/深度、trace 3 phase×20k dispatch |

### 3.1 二进制 SHA-256（完整值）

| 二进制 | SHA-256 |
|---|---|
| phaseA 冻结 timing `61c6d579…` | `61c6d579845572cc7a4563fdc76b545e2440172a3903406d34b29bd34ae6b886` |
| CF 冻结 timing `0091d259…` | `0091d2592242f605b5c64b11f22faa8e8b9e9b729146f93f263ba1288118f81c` |
| sigdrop timing `4de23299…` | `4de2329923b6f889dd269edfb3657ff7d7f54b6553c1226f7b778b8051b778e8` |
| deferoff timing `64189aa8…` | `64189aa8765ca03b5c5649f95a24708ace8326bf2b7b25a6f365c0605de8d72f` |
| CF 冻结 counters `601dad6a…` | `601dad6a8fe61fa0bebe8bf535b0da76665f206d99094f0ba5c75f870af41ae9` |
| deferoff counters `ed832d97…` | `ed832d97d421a17ad82d202a1088b9588e446f125539bf609902f94ab88920b9` |
| trace 冻结 / sigdrop / deferoff | `1b131f4f…` / `58c49d26…` / `66b2d0dd…`（完整值见 `counterfactuals/raw/meta*/binary-sha256.txt` 与 `ROOT_CAUSE.md` §10.1） |
| phaseA counters `b9a49946…` | `b9a4994631dbe6714e630fec838fd1d2b98604bb872007e6882bad96f1eb3a3a` |
| PMU `6670d8de…` | `6670d8dee37e8d8faa60549dfc5e1f6121cc4f56ee66cf58fa91cc678d16db09` |
| C1 `75534641…` | `755346410fd11f39d8f713e2fad32e0e5b1c8cd6de52f16960eeca41033709af` |
| C3 `17463539…` | `17463539a54d3e68157fc486a84531ea3b793276a2b98892b70fd269ebe2cae8` |
| depth trace 构建 `2f47d5c1…` | 见 `analysis/depth-analysis/DEPTH_MECHANISM.md` §9（构建于 `worktree-trace`，二进制未归档；`trace.bin` 与工具已归档） |

## 4. 清单与校验

**全量清单**（相对 `evidence/` 的路径，覆盖 4,739 个证据文件，不含清单自身）：

```
cd /home/jnhu/sluice-595-e5/causal-r2/evidence
sha256sum -c MANIFEST.sha256
# → 4739/4739 OK
```

**frozen 子清单**（保留原位置与原字节；验证时先进入各自目录）：

```
cd counterfactuals            && sha256sum -c MANIFEST.sha256       # 374/375 OK
cd counterfactuals            && sha256sum -c MANIFEST-CF2.sha256   # 419/419 OK
cd counterfactuals/three_arm  && sha256sum -c MANIFEST.sha256       # 736/736 OK
cd analysis/depth-analysis    && sha256sum -c RAW_MANIFEST.sha256   # 32/32 OK
```

**关于 CF-1 的 374/375**：`counterfactuals/MANIFEST.sha256` 第 8 行把清单文件自身
（`./MANIFEST.sha256`）写入清单，文件不可能包含自身的真实哈希，因此该行必然失败。
这是归档前既有的自引用缺陷（本次未重写历史清单）；其余 374 条全部通过。CF-2、CF-0、
depth 子清单全部通过。

归档完整性：移动前后 4,734 个非缓存文件逐文件 SHA-256 比对，`missing=0 changed=0
extra=0`；唯一删除项是 4 个 `__pycache__/*.pyc`（50,102 B，非证据）。详见
`analysis/ARCHIVE_MOVES.md`。

## 5. 复现入口

```bash
# phaseA（无仪器，24 对/轮）
python3 -I /home/jnhu/sluice-595-e5/harness/pair_runner.py \
  --phase <phase> --case <case> --pairs 24 --measured 5 \
  --binary .../bench_w02_pipeline --cpu 2 --arms AB

# 分析（pair 级相对差 + bootstrap）
python3 -I /home/jnhu/sluice-595-e5/harness/analyze.py summary \
  --phase-dir ... --arms AB

# depth 计数器 / trace / strace / PMU / split / deferoff
bash analysis/depth-analysis/tools/run_depth_ledger.sh
python3 -I analysis/depth-analysis/tools/parse_trace.py analysis/depth-analysis/raw/trace-d2/B/trace.bin
```

精确命令、块序与偏差见 `PROTOCOL.md`、`counterfactuals/CF1_SIGDROP.md` §6、
`counterfactuals/CF2_DEFEROFF.md` §5、`analysis/depth-analysis/DEPTH_MECHANISM.md` §9。

## 6. 边界

- 只覆盖 `W02-uring-4k` warm-tmpfs 4 KiB read（D1/D2/D3/D16，以 D2 为主）；
  不覆盖真实块设备、cold cache、O_DIRECT、其他 block size、其他 workload。
- `deferoff` 变异体按设计不满足 arm-B held-batch 进度契约（595 oracle rc=134），
  结论限定于无 held-batch 停滞的形状。
- 非 root：`:k` PMU、tracefs、root bpftrace 不可用；相关项标记
  `UNOBSERVED_ADMIN_UNAVAILABLE` / `OBSERVABILITY_LIMIT_REACHED`。
- `PRODUCT_CHANGE: NONE`、`PRODUCT_PR: NONE`、`MERGED: NO`；主工作树未改动。
