# ARCHIVE_MOVES — causal-r2 证据归档的目录映射与完整性记录

状态：归档操作记录（2026-11-05，E5）。本文件记录 §13 重组做了什么、没有做什么，以及
如何证明**文件内容未被改动**。所有路径以
`/home/jnhu/sluice-595-e5/causal-r2/evidence` 为根。

## 1. 归档前清点

| 项 | 值 |
|---|---|
| 文件数 | 4,738 |
| 总字节 | 435,205,315 |
| 目录数 | 183 |
| 清单方式 | `find . -type f -print0 \| xargs -0 sha256sum` → `/tmp/r2-hashes.txt`（4,734 条，`__pycache__` 已排除） |

## 2. 目录映射（最终状态）

| 旧路径 | 新路径 | 说明 |
|---|---|---|
| `counterfactual/` | `counterfactuals/` | CF-1 / CF-2 文档、raw、bin、tools、两份 frozen 子清单 |
| `three_arm/` | `counterfactuals/three_arm/` | CF-0 三臂 raw/analysis/correctness/tools 与子清单；**保留原名**，使 CF-1/CF-2 文档内的 `three_arm/...` 相对引用继续成立 |
| `depth-analysis/` | `analysis/depth-analysis/` | 深度边界机制分析（D2/D3/D16）全部 raw/tools/摘要 |
| `CF0_THREE_ARM.md` | `CF0_THREE_ARM.md`（未移动） | CF-0 锚定文档保留在根；CF-2 文档以 `../CF0_THREE_ARM.md` 引用它，移动会破坏该相对路径 |
| `raw/`、`traces/`、`counters/` | 不变 | §13 目标目录名与原名一致 |

最终顶层结构：`README.md`、`CAPABILITY_MATRIX.md`、`PROTOCOL.md`、`CAUSAL_DAG.md`、
`COST_CLASSIFICATION.md`、`EVIDENCE_GAPS.md`、`OBSERVABILITY_LIMITS.md`、`RESULTS.md`、
`ROOT_CAUSE.md`、`CF0_THREE_ARM.md`（CF-0 锚定文档）、`raw/`、`traces/`、`counters/`、
`counterfactuals/`、`analysis/`、`MANIFEST.sha256`。

## 3. 完整性验证（移动前后逐文件 SHA-256 比对）

方法：对归档前 4,734 个非 `__pycache__` 文件按上表映射到新路径，逐个重算 SHA-256 并与
移动前清单比对。一次性校验脚本位于 `/tmp/verify_moves.py`（研究纪律：一次性探针不入
evidence）。

```
old_entries=4734 skipped_pycache=0
missing=0 changed=0 extra=0
```

⇒ **没有任何证据文件在移动中发生字节变化**；`extra=0` 表示没有产生映射之外的新文件
（当时尚未写入 README/RESULTS/analysis 文档）。

## 4. 删除项（唯一一类）

| 项 | 数量 | 字节 | 理由 |
|---|---|---|---|
| `__pycache__/*.pyc` | 4 | 50,102 | 解释器缓存，非证据；不在任何 frozen 子清单内 |

归档后：**4,734 个证据文件、435,155,213 字节、181 个目录**（写入本文件与 §13 新增
文档之前）。没有删除任何 trace、stderr、log、jsonl 或二进制；没有压缩或转码。

## 5. 未删除的“重复”文件及理由

按 SHA-256 查重，归档前只有 **9 组**字节相同文件，且没有大体积重复 trace：

- 冻结/变异 bench 二进制在 `bin/` 与若干 raw run 目录各有一份（单份 200–300 KB）；
- 空的 `.err` / `.stderr` 文件（0 字节，e3b0c442…）；
- `traces/perf-callgraph{,-run1-unpinned-binary}/` 共享的 2 个工具文件。

这些文件被**保留**，因为冻结子清单（CF-1 `MANIFEST.sha256`、CF-2 `MANIFEST-CF2.sha256`、
CF-0 `three_arm/MANIFEST.sha256`）逐条枚举了它们；删除会使历史子清单不可验证。
6 个体积最大的 `depth-analysis/raw/trace-*/{A,B}/trace.bin`（22.7–25.6 MB）两两不同，
不是重复 trace。

## 6. 文档路径修订（仅路径前缀，不改语义）

以下 6 个文档在移动后做了**仅路径前缀**的替换；没有改数字、结论、证据等级或表格：

| 文档 | 修订 |
|---|---|
| `ROOT_CAUSE.md` | `counterfactual/` → `counterfactuals/`；`depth-analysis/` → `analysis/depth-analysis/`；`three_arm/` → `counterfactuals/three_arm/`；补全裸文档名引用 |
| `CF0_THREE_ARM.md` | `evidence/three_arm/` → `evidence/counterfactuals/three_arm/` |
| `analysis/depth-analysis/DEPTH_MECHANISM.md` | `evidence/depth-analysis/` → `evidence/analysis/depth-analysis/`；`evidence/three_arm` → `evidence/counterfactuals/three_arm` |
| `counterfactuals/CF1_SIGDROP.md` | **未改**（保持 frozen 字节；其 `three_arm/` 相对引用因目录保留原名而继续成立） |
| `counterfactuals/CF2_DEFEROFF.md` | **未改**（同上；`../CF0_THREE_ARM.md` 因 CF0 留根而继续成立） |
| `CF0_THREE_ARM.md` 之外的反事实文档 | 无 |

修订差异已逐一核对为路径行（`diff` 仅显示相应行）。

## 7. frozen 子清单验证结果

| 子清单 | 位置（相对 `counterfactuals/` 或 `analysis/depth-analysis/`） | 条目 | 结果 |
|---|---|---|---|
| CF-1 | `counterfactuals/MANIFEST.sha256` | 375 | **374/375 OK**；唯一失败是其自身条目 `./MANIFEST.sha256`（生成时把清单自身哈希写入，文件不可能包含自身真实哈希）。这是归档前既有的自引用缺陷，**未重写**。 |
| CF-2 | `counterfactuals/MANIFEST-CF2.sha256` | 419 | **419/419 OK** |
| CF-0 | `counterfactuals/three_arm/MANIFEST.sha256` | 736 | **736/736 OK** |
| depth raw | `analysis/depth-analysis/RAW_MANIFEST.sha256` | 32 | **32/32 OK** |

验证命令见 `README.md` §4。CF-1 的自引用条目在归档前同样无法通过（不是本次移动引入），
如实登记而不修改历史清单。

## 8. 未触碰项

- R1 证据 `/home/jnhu/sluice-595-e5/evidence/`：只读；其 `MANIFEST.sha256` 从
  `/home/jnhu/sluice-595-e5` 基准验证通过（尾部 `... OK`），未写入任何文件。
- 主工作树 `/home/jnhu/Source/Sluice`：未改动（归档结束时 `git status --porcelain` 为空）。
- 研究副本 `causal-r2/worktree{,-timing,-trace}`：未改动。
