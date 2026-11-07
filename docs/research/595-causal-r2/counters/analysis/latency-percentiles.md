# D2 延迟分布重算（p50/p95/p99，pair 配对 bootstrap）

状态：ACTIVE EVIDENCE（综合分析师重算，2026-11-04）。
数据源（只读）：`/home/jnhu/sluice-595-e5/causal-r2/evidence/raw/phaseA/ab-d2-w02-4k/results.jsonl`
（phaseA 冻结协议 24 对，R1 原始数据；本文件不改动任何原始数据）。

命令（完整复现）：

```bash
python3 -I -c '
import json, random
recs=[]
with open("evidence/raw/phaseA/ab-d2-w02-4k/results.jsonl") as f:
    for line in f: recs.append(json.loads(line))
from collections import defaultdict
pairs=defaultdict(dict)
for r in recs: pairs[r["pair"]][r["arm"]]=r["metrics"]
ok=[(p, pairs[p]["A"], pairs[p]["B"]) for p in sorted(pairs) if "A" in pairs[p] and "B" in pairs[p]]
random.seed(5950)
for k in ("p50_ns","p95_ns","p99_ns"):
    rel=[(b[k]-a[k])/a[k]*100 for _,a,b in ok]
    m=sum(rel)/len(rel)
    bs=[]
    for _ in range(10000):
        s=[rel[random.randrange(len(rel))] for _ in range(len(rel))]
        bs.append(sum(s)/len(s))
    bs.sort()
    absd=[b[k]-a[k] for _,a,b in ok]
    print("%s: %+.2f%% [%+.2f,%+.2f]  abs %+.0f ns mean" % (k, m, bs[250], bs[9749], sum(absd)/len(absd)))
'
```

输出（2026-11-04，paired=24）：

```
p50_ns: +6.91% [+5.03,+9.23]  abs +532 ns mean
p95_ns: +4.96% [+4.23,+5.74]  abs +634 ns mean
p99_ns: +7.07% [+4.24,+11.78] abs +955 ns mean
```

臂级均值（同数据，非配对口径）：p50 A 7955.7 / B 8487.6 ns；p95 A 12831.7 / B 13465.5 ns；
p99 A 13649.4 / B 14604.2 ns。p95/p99 与冻结基线给定值（+4.96/+7.07）一致（配对方式有效）。

## 结构性结论（供 CAUSAL_DAG.md / COST_CLASSIFICATION.md / G12 引用）

1. **延迟退化是分布整体抬升，不是尾部专属加宽**：p50 相对位移（+6.91%）大于 p95（+4.96%），
   与 p99（+7.07%）同量级。若机制只加宽尾部（如偶发调度长尾），p50 应近不变；观测相反。
2. **延迟位移量远超 CPU 位移**：绝对位移 +532/+634/+955 ns（p50/p95/p99）vs CPU/op
   +189.15 ns（split-d2）/+177 ns（phaseA 口径 4776.7→4953.9）。超额 +340~770 ns/op 是
   wall/等待分量，不是 CPU 执行分量（runnable delay 直接测量仅 +36.4 ns/op，C4）。
3. 绝对位移从 p50→p99 单调增（532→634→955 ns），存在一个附加的尾部分量（p99 比 p95
   多 ~+320 ns），该分量当前 UNOBSERVED（逐事件 wakeup→run 分布、逐 park 原因 root-only）。
