# EVIDENCE-MANIFEST 校验记录 — #621 Progress/Wait 路径战役

| 字段 | 值 |
|---|---|
| 日期 | 2026-11-18 |
| 战役目录 | `docs/evidence/621-progress-wait-path-20261118/` |
| 清单文件 | `EVIDENCE-MANIFEST.sha256` |
| 清单自身 sha256 | `f925eaf79f977931ad8eed68b899f53a0251bb27f4bd9cb900b39017c3e31199` |
| 清单条目 | **2110** |
| 路径缺失 | **0** |
| 字节校验 | `sha256sum -c` exit **0**；**2110 OK / 0 FAILED** |
| 结论 | **PASS**（无缺失、无凭据/token/私钥/口令值、两个既有未跟踪目录未触碰）；1 项非阻断的 sudo 模板偏差，见 §5.3 |

> 本记录是 `EVIDENCE-MANIFEST.sha256` 的校验回执，不是产品语义权威，也不替代 `CAMPAIGN-LEDGER.md` 或 `FINAL-REPORT.md`。清单与本记录自身不进入清单：清单无法包含自身的最终哈希，记录在清单生成后写入。本记录未 commit、未 push。

## 1. 清单生成

### 1.1 范围与条目构成

保留证据按任务口径：台账、P1/P2/P3/P4 报告、原始测量 CSV/JSONL（本战役实际为 TSV/JSONL）、复算脚本、既有清单、最终报告；另纳入同口径的测量元数据/分析 JSON、原始运行回执（`.cmd`/`.stdout`/`.stderr`/perf stat `.perf`）和环境/分析文本。路径均相对战役目录。

| 类别 | 条目 | 说明 |
|---|---:|---|
| 台账 / P1–P4 报告 / 最终报告（`.md`） | 19 | 含 `CAMPAIGN-LEDGER.md`、`FINAL-REPORT.md`、`FINAL-REPORT-REVIEW.md`、`ISSUE-621-COMMENT.md`、四份 `build-manifest` / `rowlevel-build-manifest` 与各阶段报告 |
| 原始测量 JSONL（`.jsonl`） | 390 | P1 每 arm JSONL、`arms.jsonl`、P2/P4 每 run / baseline / smoke JSONL |
| 原始测量 TSV（`.tsv`） | 12 | P1 `pairs.tsv`（12 格） |
| 测量元数据/分析 JSON（`.json`） | 14 | `meta.json`、`analysis.json`、`analysis-w3.json` 等 |
| 复算/会话脚本（`.py`/`.sh`） | 6 + 2 | `tools/`、`p2/profile/analysis/`、`p4/profile/analysis/` 的脚本与 freeze/restore 脚本 |
| 既有清单（`SHA256SUMS`） | 2 | `p2/profile/analysis/SHA256SUMS`、`p4/profile/SHA256SUMS` |
| 原始运行回执 | 1560 | 356 `.perf`（perf stat 文本）、356 `.cmd`、425 `.stdout`、423 `.stderr` |
| 环境/分析/记录头/丢失样本/stat 等文本（`.txt`） | 103 | 含 `p1/session-snapshot.txt`、`analysis.txt`、`analysis-w3.txt`、`env/*.txt`、`header-*/lost-*/stat-*` |
| wall/CPU 时间文本（`.times`） | 2 | P4 P-E' baseline times |
| **合计** | **2110** | 清单文件 242,339 bytes；所列文件合计 11,591,414 bytes |

### 1.2 生成命令

在 `docs/evidence/621-progress-wait-path-20261118/` 下执行：

```bash
find . -type f \
  ! -name '*.data' ! -name '*.pyc' ! -name '*.log' \
  ! -name 'session-snapshot.*.prev.txt' \
  ! -name 'script-*.txt' ! -name 'report-*.txt' \
  ! -name 'EVIDENCE-MANIFEST.sha256' ! -name 'EVIDENCE-MANIFEST-VERIFY.md' \
  -print0 | LC_ALL=C sort -z | xargs -0 sha256sum > /tmp/e621-manifest.tmp
mv /tmp/e621-manifest.tmp EVIDENCE-MANIFEST.sha256
```

排除项就是 §3 标注的本地保留文件；清单与记录自身也被显式排除。`sha256sum -c` 应在同一战役目录下执行（清单使用 `./` 相对路径）。

## 2. 路径与字节校验

### 2.1 逐项路径存在性

```bash
ok=0; missing=0
while IFS= read -r line; do
  p="${line#*  }"
  if [ -f "$p" ]; then ok=$((ok+1)); else missing=$((missing+1)); printf 'MISSING %s\n' "$p"; fi
done < EVIDENCE-MANIFEST.sha256
echo "entries=$((ok+missing)) exists=$ok missing=$missing"
```

实际输出：

```text
entries=2110 exists=2110 missing=0
```

文件名无空格（`find . -type f -name '* *'` 无输出），因此 `hash + 两空格 + path` 的解析无歧义。

### 2.2 字节级校验

```bash
sha256sum -c EVIDENCE-MANIFEST.sha256 > /tmp/e621-check.out 2>&1
echo "exit=$?"
grep -c ': OK$' /tmp/e621-check.out
grep -v ': OK$' /tmp/e621-check.out | head
```

实际输出：

```text
exit=0
2110
```

`grep -v ': OK$'` 无输出，即 **2110/2110 OK、0 FAILED**。

### 2.3 清单未混入本地保留文件

```bash
grep -E '\.data$|\.log$|\.pyc$|/script-[^/]*\.txt$|/report-[^/]*\.txt$|session-snapshot\..*\.prev\.txt$' EVIDENCE-MANIFEST.sha256
```

实际输出：无匹配（`grep` 返回 1）。另检查清单内所有路径可读：

```bash
awk '{print $2}' EVIDENCE-MANIFEST.sha256 | while IFS= read -r p; do [ -r "$p" ] || echo "$p"; done | wc -l
```

实际输出：`0`。

## 3. 本地保留、不进入 master 的文件

按任务要求，以下 162 个文件仅本地保留，**未写入清单、不进入 master**。总计 **7,311,874,379 bytes（6,973.1 MiB）**。

| 类别 | 条目 | 字节 | 处置理由 |
|---|---:|---:|---|
| `*.data`（perf ring buffer） | 27 | 6,016,558,406 | raw perf.data；其中 p4 的 12 个为 root-owned，当前用户不可读 |
| `script-*.txt`（perf script 展开） | 29 | 1,186,257,596 | 从 raw perf.data 派生的逐样本文本；体积过大 |
| `report-*.txt`（perf report 展开） | 66 | 108,660,340 | 从 raw perf.data 派生的函数/调用图报告；体积过大 |
| `*.log`（构建/冒烟日志） | 33 | 198,300 | 构建/配置/冒烟日志；任务明确列为本地保留 |
| `session-snapshot.*.prev.txt`（历史快照） | 4 | 27,220 | 每次 freeze 前的临时备份；当前 `p1/session-snapshot.txt` 已保留 |
| `*.pyc`（字节码缓存） | 3 | 172,517 | Python 临时缓存 |
| **合计** | **162** | **7,311,874,379** | |

说明：

- `p1/session-snapshot.txt` **在清单内**（报告 `p1/AA-CALIBRATION.md:161` 与 `tools/session_restore.sh:23` 引用）；只有 `.prev` 历史备份被列为 scratch。
- p4 的 12 个 `.data` 当前用户不可读（`find . -type f ! -readable` 列出的 root-owned 文件），因此即使想纳入也无法完成非特权字节校验；它们本来就在本地保留范围。
- P2/P4 的部分逐样本复算依赖 `script-*.txt`；这些文件只在本地保留，所以“从 master 归档副本重新跑 P2/P4 逐样本归因”不可行。master 口径保留的是聚合 JSONL/TSV、`analysis*.json`、`analysis*.txt`、复算脚本和既有 SHA256SUMS。此限制与 `FINAL-REPORT.md` 对 P2/P4 证据等级的 `R` 标注一致。
- 本清单不是整棵目录树的完整清单；它只覆盖 §1.1 的保留证据。完整本地文件列表可按 §7 的命令复现。

完整本地保留路径（162 条，相对战役目录）：

```text
p1/logs/build-noring.log
p1/logs/build-ring.log
p1/logs/build-sluice-copy.log
p1/logs/readelf-assertions.log
p1/logs/sluice-copy-readelf.log
p1/logs/smoke-sluice-copy.log
p1/logs/smoke-w02-case-w4.log
p1/logs/smoke-w02-literal-filter.log
p1/logs/smoke-w03.log
p1/logs/test-release-liburing.log
p1/session-snapshot.20261118T135227.prev.txt
p1/session-snapshot.20261118T143804.prev.txt
p1/session-snapshot.20261118T152701.prev.txt
p1/session-snapshot.20261118T194722.prev.txt
p2/profile/P-A-w02-uring-4k-d16/raw/record-cycles.data
p2/profile/P-A-w02-uring-4k-d16/raw/record-offcpu.data
p2/profile/P-A-w02-uring-4k-d16/raw/record-tp-uring.data
p2/profile/P-A-w02-uring-4k-d16/raw/report-cycles-dso-symbol.txt
p2/profile/P-A-w02-uring-4k-d16/raw/report-cycles-symbol.txt
p2/profile/P-A-w02-uring-4k-d16/raw/script-cycles-sym.txt
p2/profile/P-A-w02-uring-4k-d16/raw/script-cycles.txt
p2/profile/P-A-w02-uring-4k-d16/raw/script-offcpu-sym.txt
p2/profile/P-A-w02-uring-4k-d16/raw/script-offcpu.txt
p2/profile/P-B-w02-uring-4k-d1/raw/record-cycles.data
p2/profile/P-B-w02-uring-4k-d1/raw/record-offcpu.data
p2/profile/P-B-w02-uring-4k-d1/raw/record-tp-uring.data
p2/profile/P-B-w02-uring-4k-d1/raw/report-cycles-dso-symbol.txt
p2/profile/P-B-w02-uring-4k-d1/raw/script-cycles-sym.txt
p2/profile/P-B-w02-uring-4k-d1/raw/script-cycles.txt
p2/profile/P-B-w02-uring-4k-d1/raw/script-offcpu-sym.txt
p2/profile/P-B-w02-uring-4k-d1/raw/script-offcpu.txt
p2/profile/P-C-w03-uring-4k-d16/raw/record-cycles.data
p2/profile/P-C-w03-uring-4k-d16/raw/record-offcpu.data
p2/profile/P-C-w03-uring-4k-d16/raw/record-tp-uring.data
p2/profile/P-C-w03-uring-4k-d16/raw/report-cycles-dso-symbol.txt
p2/profile/P-C-w03-uring-4k-d16/raw/script-cycles-sym.txt
p2/profile/P-C-w03-uring-4k-d16/raw/script-cycles.txt
p2/profile/P-C-w03-uring-4k-d16/raw/script-offcpu-sym.txt
p2/profile/P-C-w03-uring-4k-d16/raw/script-offcpu.txt
p2/profile/P-D-w02-tp-4k-d16-w4/raw/record-cycles.data
p2/profile/P-D-w02-tp-4k-d16-w4/raw/record-offcpu.data
p2/profile/P-D-w02-tp-4k-d16-w4/raw/record-tp-sched-syscall.data
p2/profile/P-D-w02-tp-4k-d16-w4/raw/report-cycles-dso-symbol.txt
p2/profile/P-D-w02-tp-4k-d16-w4/raw/script-cycles-sym.txt
p2/profile/P-D-w02-tp-4k-d16-w4/raw/script-cycles.txt
p2/profile/P-D-w02-tp-4k-d16-w4/raw/script-offcpu-sym.txt
p2/profile/P-D-w02-tp-4k-d16-w4/raw/script-offcpu.txt
p2/profile/P-E-sluice-copy-4k-d4-w1/raw/record-cycles.data
p2/profile/P-E-sluice-copy-4k-d4-w1/raw/record-offcpu.data
p2/profile/P-E-sluice-copy-4k-d4-w1/raw/record-tp.data
p2/profile/P-E-sluice-copy-4k-d4-w1/raw/report-cycles-dso-symbol.txt
p2/profile/P-E-sluice-copy-4k-d4-w1/raw/script-cycles-sym.txt
p2/profile/P-E-sluice-copy-4k-d4-w1/raw/script-cycles.txt
p2/profile/P-E-sluice-copy-4k-d4-w1/raw/script-offcpu-sym.txt
p2/profile/P-E-sluice-copy-4k-d4-w1/raw/script-offcpu.txt
p2/profile/analysis/__pycache__/analyze_p2.cpython-314.pyc
p4/logs/build-copy-release-g-nostrip.log
p4/logs/build-copy-release-g.log
p4/logs/build-noring-release-g-nostrip.log
p4/logs/build-noring-release-g.log
p4/logs/build-ring-release-g-nostrip.log
p4/logs/build-ring-release-g.log
p4/logs/build-ring-releasedbg.log
p4/logs/config-copy-release-g-nostrip.log
p4/logs/config-copy-release-g.log
p4/logs/config-noring-release-g-nostrip.log
p4/logs/config-noring-release-g.log
p4/logs/config-ring-release-g-nostrip.log
p4/logs/config-ring-release-g.log
p4/logs/config-ring.log
p4/logs/dwarf-library-symbols.log
p4/logs/dwarf-producer-addr2line.log
p4/logs/p1-release-contrast.log
p4/logs/restore-xmake-compile-commands.log
p4/logs/smoke-sluice-copy.log
p4/logs/smoke-w02-w03.log
p4/logs/text-compare-vs-p1.log
p4/logs/verify-dwarf-link-sha.log
p4/profile/P-C-w03-uring-4k-d16/raw/report-callgraph-begin_publication-run-01.txt
p4/profile/P-C-w03-uring-4k-d16/raw/report-callgraph-complete_publication-run-01.txt
p4/profile/P-C-w03-uring-4k-d16/raw/report-callgraph-consume-run-01.txt
p4/profile/P-C-w03-uring-4k-d16/raw/report-callgraph-context_poll-run-01.txt
p4/profile/P-C-w03-uring-4k-d16/raw/report-callgraph-driver_thread-run-01.txt
p4/profile/P-C-w03-uring-4k-d16/raw/report-callgraph-find_cookie-run-01.txt
p4/profile/P-C-w03-uring-4k-d16/raw/report-callgraph-offer_terminal-run-01.txt
p4/profile/P-C-w03-uring-4k-d16/raw/report-callgraph-release_execution-run-01.txt
p4/profile/P-C-w03-uring-4k-d16/raw/report-callgraph-reserve-run-01.txt
p4/profile/P-C-w03-uring-4k-d16/raw/report-callgraph-submitter-run-01.txt
p4/profile/P-C-w03-uring-4k-d16/raw/report-callgraph-uring_poll-run-01.txt
p4/profile/P-C-w03-uring-4k-d16/raw/report-flat-self-run-01.txt
p4/profile/P-C-w03-uring-4k-d16/raw/report-flat-self-run-02.txt
p4/profile/P-C-w03-uring-4k-d16/raw/report-inline-self-run-01.txt
p4/profile/P-C-w03-uring-4k-d16/raw/report-inline-self-run-02.txt
p4/profile/P-C-w03-uring-4k-d16/raw/run-01.data
p4/profile/P-C-w03-uring-4k-d16/raw/run-02.data
p4/profile/P-C-w03-uring-4k-d16/raw/script-inline-run-01.txt
p4/profile/P-C-w03-uring-4k-d16/raw/script-inline-run-02.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/report-callgraph-accept-run-01.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/report-callgraph-begin_publication-run-01.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/report-callgraph-claim_execution-run-01.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/report-callgraph-complete_publication-run-01.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/report-callgraph-consume-run-01.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/report-callgraph-deliver_event-run-01.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/report-callgraph-offer_terminal-run-01.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/report-callgraph-publish_one-run-01.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/report-callgraph-release_execution-run-01.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/report-callgraph-reserve-run-01.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/report-callgraph-take-run-01.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/report-callgraph-wait_for-run-01.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/report-callgraph-wait_one-run-01.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/report-flat-self-run-01.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/report-flat-self-run-02.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/report-inline-self-run-01.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/report-inline-self-run-02.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/run-01.data
p4/profile/P-D-w02-tp-4k-d16-w4/raw/run-02.data
p4/profile/P-D-w02-tp-4k-d16-w4/raw/script-inline-run-01.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/script-inline-run-02.txt
p4/profile/P-D-w02-tp-4k-d16-w4/raw/smoke-cycles.data
p4/profile/P-D-w02-tp-4k-d16-w4/raw/smoke-record.log
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/overhead-1copy.data
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/overhead-timed.data
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/report-callgraph-begin_publication-run-01.txt
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/report-callgraph-claim_execution-run-01.txt
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/report-callgraph-complete_publication-run-01.txt
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/report-callgraph-copy_context-run-01.txt
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/report-callgraph-poll_progress-run-01.txt
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/report-callgraph-publish_one-run-01.txt
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/report-callgraph-take-run-01.txt
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/report-callgraph-wait_for-run-01.txt
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/report-callgraph-wait_if_unchanged-run-01.txt
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/report-callgraph-wait_one-run-01.txt
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/report-flat-self-run-01.txt
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/report-flat-self-run-02.txt
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/report-inline-self-run-01.txt
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/report-inline-self-run-02.txt
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/run-01.data
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/run-02.data
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/script-inline-run-01.txt
p4/profile/P-E-sluice-copy-4k-d4-w1/raw/script-inline-run-02.txt
p4/profile/P-F-w02-uring-4k-d64/raw/control-d16-run-01.data
p4/profile/P-F-w02-uring-4k-d64/raw/report-callgraph-consume-run-01.txt
p4/profile/P-F-w02-uring-4k-d64/raw/report-callgraph-dispatch_one-run-01.txt
p4/profile/P-F-w02-uring-4k-d64/raw/report-callgraph-finalize-run-01.txt
p4/profile/P-F-w02-uring-4k-d64/raw/report-callgraph-find_cookie-control-d16.txt
p4/profile/P-F-w02-uring-4k-d64/raw/report-callgraph-find_cookie-run-01.txt
p4/profile/P-F-w02-uring-4k-d64/raw/report-callgraph-handle_cqe-run-01.txt
p4/profile/P-F-w02-uring-4k-d64/raw/report-callgraph-reap_cqes-run-01.txt
p4/profile/P-F-w02-uring-4k-d64/raw/report-callgraph-submit_transport-run-01.txt
p4/profile/P-F-w02-uring-4k-d64/raw/report-callgraph-take-run-01.txt
p4/profile/P-F-w02-uring-4k-d64/raw/report-callgraph-wait_for-run-01.txt
p4/profile/P-F-w02-uring-4k-d64/raw/report-flat-self-run-01.txt
p4/profile/P-F-w02-uring-4k-d64/raw/report-flat-self-run-02.txt
p4/profile/P-F-w02-uring-4k-d64/raw/report-inline-self-run-01.txt
p4/profile/P-F-w02-uring-4k-d64/raw/report-inline-self-run-02.txt
p4/profile/P-F-w02-uring-4k-d64/raw/run-01.data
p4/profile/P-F-w02-uring-4k-d64/raw/run-02.data
p4/profile/P-F-w02-uring-4k-d64/raw/script-inline-control-d16-run-01.txt
p4/profile/P-F-w02-uring-4k-d64/raw/script-inline-run-01.txt
p4/profile/P-F-w02-uring-4k-d64/raw/script-inline-run-02.txt
p4/profile/analysis/__pycache__/analyze_w3.cpython-314.pyc
tools/__pycache__/recompute_independent.cpython-314.pyc
```

## 4. 既有清单核验

### 4.1 `p2/profile/analysis/SHA256SUMS`

```bash
sha256sum -c p2/profile/analysis/SHA256SUMS
```

实际输出（省略绝对路径前缀，保留判词）：

```text
.../analysis.json: OK
.../analysis.txt: OK
.../analyze_p2.py: OK
.../SHA256SUMS: FAILED
sha256sum: WARNING: 1 computed checksum did NOT match
exit=1
```

判定：

- 3 个实质条目（`analysis.json`、`analysis.txt`、`analyze_p2.py`）**OK**；这三个文件也在新清单内，`sha256sum -c EVIDENCE-MANIFEST.sha256` 同样 OK。
- 第 4 行是 `SHA256SUMS` 对自身的哈希。文件一旦包含自身哈希就无法再自洽（自引用），因此 **FAILED 是预期伪影，不是文件损坏**。若该 `SHA256SUMS` 进入 master，建议删除自身条目或标注为“生成时快照”。
- `P2-REVIEW.md:303` 记录执行过 `sha256sum -c SHA256SUMS`；本记录补充了其未展开的自引用结果。

### 4.2 `p4/profile/SHA256SUMS`

```bash
cd p4/profile
ok=0; missing=0
while IFS= read -r line; do
  p="${line#*  }"
  if [ -f "$p" ]; then ok=$((ok+1)); else missing=$((missing+1)); printf 'MISSING %s\n' "$p"; fi
done < SHA256SUMS
echo "entries=$((ok+missing)) exists=$ok missing=$missing"
```

实际输出：

```text
entries=155 exists=155 missing=0
```

限制：其中 12 个 `.data` 为 root-owned，当前用户不可读（`[ ! -r "$p" ]` 计数 = 12）。因此本会话**未**以非特权身份执行 `sha256sum -c p4/profile/SHA256SUMS`；`p4/W3-ROWLEVEL-REVIEW.md:35` 记录的是 root + `/tmp/sluice621-sudo` 下的 155/155 OK 回执。新清单只纳入 `SHA256SUMS` 本身，不纳入其 `.data` 目标。

## 5. 敏感信息扫描

扫描对象：写入本记录之前战役目录内全部可读的既有文本证据；命令统一使用 `--exclude='*.data'`，避免读取二进制 perf ring buffer。新增的清单与本记录另行复查（§5.3 末句）。未读取 `/tmp/sluice621-sudo`，未尝试 sudo。

### 5.1 凭据 / token / 私钥 / 口令模式

| 检查 | 命令（摘录） | 结果 |
|---|---|---|
| 私钥头、云厂商 token | `grep -rIn --exclude='*.data' -E 'BEGIN (RSA \|OPENSSH \|EC \|DSA )?PRIVATE KEY\|AKIA[0-9A-Z]{16}\|ghp_[A-Za-z0-9]{20,}\|github_pat_[A-Za-z0-9_]{20,}\|xox[baprs]-[A-Za-z0-9-]{10,}\|sk-[A-Za-z0-9]{20,}' .` | **0 匹配**（rc=1） |
| `password`/`passwd` 赋值 | `grep -rIn --exclude='*.data' -iE '(password\|passwd)[[:space:]]*[:=][[:space:]]*[^[:space:]]+' .` | **0 匹配**（rc=1） |
| `secret`/`token`/API key 赋值 | `grep -rIn --exclude='*.data' -iE '(secret\|token\|api[_-]?key\|access[_-]?key)[[:space:]]*[:=][[:space:]]*[^[:space:]]+' .` | **0 匹配**（rc=1） |
| 常见凭据环境变量 / Bearer / 证书 | `grep -rIn --exclude='*.data' -E 'AWS_SECRET_ACCESS_KEY\|AWS_ACCESS_KEY_ID\|GITHUB_TOKEN\|GH_TOKEN\|CI_JOB_TOKEN\|NPM_TOKEN\|DOCKER_PASSWORD\|DOCKER_AUTH\|Authorization:[[:space:]]*Bearer\|BEGIN CERTIFICATE' .` | **0 匹配**（rc=1） |
| 可疑文件名 | `find . -type f \( -iname '*.pem' -o -iname '*.key' -o -iname 'id_rsa*' -o -iname 'id_ed25519*' -o -iname '.env*' -o -iname '*credential*' -o -iname '*secret*' -o -iname '*token*' \)` | **0 文件** |

既有证据中 `password` 文本出现 5 行，全部是说明性文字，不含口令值：

```text
FINAL-REPORT-REVIEW.md:250  "没有 password/token/secret/API key/私钥..."
tools/session_restore.sh:4   "# The password file is only ever passed to sudo through stdin."
tools/session_restore.sh:18  "restore: password file is not a readable regular file: %s"
tools/session_freeze.sh:4    "# The password file is only ever passed to sudo through stdin."
tools/session_freeze.sh:18   "freeze: password file is not a readable regular file: %s"
```

（均为说明/报错文本，不含口令值；不是独立凭据。）

### 5.2 `sudo` 形式

写入本记录前，战役目录内既有证据的 `sudo` 文本出现 **52** 次。命令形态（`grep -rIn --exclude='*.data' -E 'sudo[[:space:]]+-'` 加 `$SUDO` 使用点）逐项分类：

| 位置 | 形式 | 判定 |
|---|---|---|
| `tools/session_freeze.sh:30` | `SUDO=(sudo -S -p '')`；所有使用点 `:32,:152,:157,:159,:185,:202` 均 `< "$PWFILE"` | **PASS**（pwfile 路径形式） |
| `tools/session_restore.sh:71` | `SUDO=(sudo -S -p '')`；使用点 `:72,:102,:111,:113` 均 `< "$PWFILE"` | **PASS** |
| `p2/PROFILE-SUMMARY.md:6` | `sudo -S -p '' <命令> < /tmp/sluice621-sudo` | **PASS** |
| `p2/PROFILE-SUMMARY.md:501,509,513,517,522,528` | `$SUDO ...`，续行 `< /tmp/sluice621-sudo` | **PASS** |
| `p2/PROFILE-SUMMARY.md:527,530,533,534` | `$SUDO perf probe ...` / `$SUDO perf script ...`，**无显式 pwfile 重定向** | **偏差**（依赖 sudo 时间戳缓存；无口令明文） |
| `p4/W3-ROWLEVEL.md:8` | 行内 `< /tmp/sluice621-sudo` | **PASS** |
| `p4/W3-ROWLEVEL.md:63→69` | 续行 `< /tmp/sluice621-sudo` | **PASS** |
| `p4/W3-ROWLEVEL.md:72→76` | 续行 `< /tmp/sluice621-sudo` | **PASS** |
| `p4/W3-ROWLEVEL-REVIEW.md:35` | 行内 `< /tmp/sluice621-sudo` | **PASS** |
| `CAMPAIGN-LEDGER.md:359,372,420,428,429,447`；`FINAL-REPORT.md:211,224`；`p4/W3-ROWLEVEL.md:499` | 说明性文字（进程清理、权限、凭据不可用、off-repo runner 风险） | 文本，不含命令/口令 |

### 5.3 判定

- **未发现**任何明文凭据、token、私钥、口令值或凭据环境变量。`sudo` 的实际命令均不以口令字面量形式出现。
- **1 项非阻断偏差**：`p2/PROFILE-SUMMARY.md:527,530,533,534` 的 4 条复现模板省略了 `< /tmp/sluice621-sudo`（或 `< "$PWFILE"`），依赖同一 shell 会话中的 sudo 凭据缓存。它们不泄露口令；若该文档要进入 master，建议补齐 pwfile 重定向或前置 `sudo -v < pwfile`。
- `CAMPAIGN-LEDGER.md:428-430` 记录了仓外 `e5harness.py` 与 `runner_aa_r2.py`/`runner_ab_r2.py` 第 39 行含**明文 sudo 口令**，但本目录**没有复制该口令**；这些脚本不在战役目录内，也未纳入清单。
- 27 个 `.data` 为二进制 perf 数据，未做文本模式扫描；其中 12 个 p4 文件当前用户不可读。该限制不改变“文本证据无凭据”的结论。
- 对新增的 `EVIDENCE-MANIFEST.sha256` 与 `EVIDENCE-MANIFEST-VERIFY.md` 复查私钥/token/password 赋值模式：**0 匹配**（rc=1）。

## 6. 两个既有未跟踪目录未触碰

对象（台账 §9.1.12 明确“全程不得触碰”）：

- `docs/evidence/621-round2-R1/`
- `docs/evidence/e5-fullpath-machine-work/`

### 6.1 文件数与大小

| 目录 | 文件数 | 字节 | 台账 §1.2 记录 |
|---|---:|---:|---|
| `621-round2-R1/` | 1 | 138,781（135.5 KiB） | 1 文件 / 136K |
| `e5-fullpath-machine-work/` | 16 | 472,409（461.3 KiB） | 16 文件 / 492K |

文件数与台账一致；`du` 的块大小口径可解释 136K/492K 与字节和的差异。

### 6.2 时间戳/大小/路径状态前后一致

在写入本清单**之前**和**之后**分别执行：

```bash
for d in docs/evidence/621-round2-R1 docs/evidence/e5-fullpath-machine-work; do
  n=$(basename "$d")
  find "$d" -type f -printf '%P\t%s\t%T@\n' | sort > "/tmp/621-manifest-baseline/$n.state"
  stat -c '%n %Y %s' "$d" >> "/tmp/621-manifest-baseline/$n.state"
done
sha256sum /tmp/621-manifest-baseline/*.state
```

实际摘要（写入前 / 写入后）：

```text
621-round2-R1:            b41c2a84ef5af041cbe72da5c7a96b5c4ea4b86e5d02b18baa8d71f0c3fd6a14
e5-fullpath-machine-work: 1d2133cdedb35e3caf16acce56438eb7654791e65d1dc9212df88ab21c046e76
```

`cmp -s` 对“写入前”与“写入后”的 state 文件逐字节比较，结果为：

```text
621-round2-R1 STATE_IDENTICAL
e5-fullpath-machine-work STATE_IDENTICAL
```

state 文件包含每个文件的相对路径、字节数和 `%T@`（mtime），以及目录自身的 inode mtime/size；因此路径、大小、时间戳均未变化。

### 6.3 Git 状态

```bash
git status --porcelain=v1 --untracked-files=all docs/evidence/621-round2-R1 docs/evidence/e5-fullpath-machine-work | wc -l
```

实际输出：`17`（`REPORT.md` 1 条 + e5 目录 16 条，全部为 `??` 未跟踪；无 `M`/`A`/`D` 跟踪改动）。

## 7. 未执行 / 限制

- 未以 root 执行 `sha256sum -c p4/profile/SHA256SUMS`；12 个 root-owned `.data` 不可读。`p4/W3-ROWLEVEL-REVIEW.md:35` 的 root 回执为 155/155 OK，但本会话未复跑。
- 未对 `.data` 做文本敏感信息扫描；未读取 `/tmp/sluice621-sudo`，未执行 sudo。
- 未重跑任何构建、测试、bench、perf、eBPF、TLC 或 A/A/A-B；本记录只做清单、路径、哈希与文本扫描。
- 清单不是整棵目录树的完整清单；本地保留文件（§3）和 `EVIDENCE-MANIFEST-VERIFY.md` 自身不在清单内。完整本地文件列表可用以下命令复现：

```bash
find . -type f \( -name '*.data' -o -name '*.pyc' -o -name '*.log' \
  -o -name 'session-snapshot.*.prev.txt' -o -name 'script-*.txt' -o -name 'report-*.txt' \) \
  -printf '%P\n' | LC_ALL=C sort
```

- 未 commit、未 push；提交与 master 进入范围由 owner 决定。

## 8. 附注：Owner Gate 裁决后清单重生成（v2，2026-11-18）

Owner 裁决（`OWNER-GATES.md`）写入后，按 §1.2 的同一命令重生成清单：

| 项 | v1 | v2 |
|---|---:|---:|
| 条目 | 2110 | **2111**（仅新增 `./OWNER-GATES.md`，sha256 `e44e6a21…2320`） |
| 原有条目哈希漂移 | — | **0**（归一化逐行 `diff`；2110/2110 一致） |
| `sha256sum -c` | 2110 OK / 0 FAILED | **2111 OK / 0 FAILED**，exit 0 |
| 清单自身 sha256 | `f925eaf7…3999` | `7f13d95b…a791` |

v1 清单备份于本地 `/tmp/e621-manifest-v1.bak`（临时文件，不入档）。本附注与清单 v2 即归档版本；归档分支与固定 SHA 见 #621 Owner Gate 回写评论。`OWNER-GATES.md` 为纯治理记录，无命令、无凭据形态，敏感信息扫描口径不变。

---

*校验记录结束。*
