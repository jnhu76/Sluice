# E5 战役证据归档入口（e5-fullpath-machine-work · 2026-11-18）

本分支是 #637 负结果战役的固定 SHA 原始证据入口（#636 归档先例的同类形态：
evidence 分支、不合并、无产品变更）。master 保持 `5d94f233` 零改动。

## 固定 SHA

| 角色 | SHA |
|---|---|
| BASE（master 冻结基线） | `5d94f233967c3aef993eb19ba9e3252c532b0951` |
| CANDIDATE（机制提交，diff 恰 3 文件，见 archive/candidate-f350474d.diff） | `f350474dc427412c445a719dd54519465878125d` |
| 实验证据提交（results/ 原始观测，SHA 于任何计时前登记：candidate-registration.txt `registered_before_any_timing=2026-11-18T05:02:12+08:00`，首个 perf invocation 05:16:35） | `ae5773949321948ab53d7e862513fe319b3c425f` |
| 本归档提交（本分支 tip；补协议副本/脚本/清单，不改动任何已采数字） | 本分支 HEAD |

## 内容清单

- `results/e5m28-s1-20261118/` — 原始观测：`observations.jsonl`（706 行）、
  `perf/`、`jsonl/`、`logs/`、`governor/`（三列存证）、`mutants/`（回执 +
  二进制 sha256）、`static/`（nm/objdump 三列、oracle 断言、复现入口、事故
  记录）、`stats/`（VERDICT、A/A 与 A/B 终值、三格逐对 CSV、NOT_POWERED
  预声明）、`candidate-registration.txt`。
- `results/e5m28-s1-20261118/scripts/driver/` — 驱动脚本（原
  `/tmp/e5-elim-scripts/`）：`e5harness.py`（计时 harness，P1/P2 pass、warm
  判据、AB/BA 交错）、`analyze.py`（配对差/符号检验/bootstrap）、
  `governor.sh`（governor 三列存证恢复）。
- `results/e5m28-s1-20261118/scripts/audit/` — 独立实验审计的复算脚本（原
  `/tmp/audit-e5m28/`）：`recompute.py`（判词数字独立复算）、`extra.py`、
  `trail.py`、`trail2.py`（种子敏感性等补充复核）。注意：脚本内 `S=…` 为
  本机绝对路径，异地复算时指向本 results/ 根即可。
- `results/e5m28-s1-20261118/archive/candidate-f350474d.diff` — 候选完整
  diff（BASE→CANDIDATE，47 行，恰 3 文件，单因素声明载体）。
- `docs/evidence/e5-fullpath-machine-work/` — 16 份战役文档（final-report
  r2 / candidates r3 为 Owner 2026-11-18 复核后的修订版）。
- `EVIDENCE-MANIFEST.sha256` — 上述全部文件的 sha256 校验清单（清单自身
  除外；位于仓库根）。

## 冻结协议哈希

- `experiment-protocol.md`（r2 冻结版，即 docs/ 内副本）sha256 =
  `cb62400b74eafe0f762534df11caa6b35caae26e479eb9cc8c7f8ac11d3192d8`

## 第三方复核命令

```sh
git checkout evidence/e5-fullpath-machine-work-20261118
sha256sum -c EVIDENCE-MANIFEST.sha256            # 完整性（应全 OK）
python3 results/e5m28-s1-20261118/scripts/audit/recompute.py   # 判词数字独立复算
# AB/BA 分列（1b 证据强度，r2 披露）：
awk -F, 'NR>1&&$2=="AB"{s+=$6;n++}END{printf "AB %.6f\n",s/n}' results/e5m28-s1-20261118/stats/main-cell-per-pair.csv
awk -F, 'NR>1&&$2=="BA"{s+=$6;n++}END{printf "BA %.6f\n",s/n}' results/e5m28-s1-20261118/stats/main-cell-per-pair.csv
# 预期：AB −1.944350 / BA −2.246725 / 全体 −2.095537（判词 1b 点估计越 [−2,0] 带 0.096）
```

## 纪律声明

- 本归档提交只补充第三方可访问性（Owner 复核 2026-11-18 指令），不改动、
  不重算、不覆盖任何已采数字与已冻结文本；`research/e5-c1-m28-gate` 分支
  保持原状（tip 仍为 `ae577394`）。
- 「缺第三轮事前签核，已有事后审计」的披露维持不变（final-report §G.1/§I.3）。
