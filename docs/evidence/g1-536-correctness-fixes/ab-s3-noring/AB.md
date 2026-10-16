wrote docs/evidence/g1-536-correctness-fixes/ab-s3-noring/AB.md
ratio (median) | paired min..max | B0 cpu/op | arm cpu/op | B0 p99 ns | arm p99 ns | p99 worst |
|---|---|---|---|---|---|---|---|---|---|---|
| W02-threadpool-4k-d16-w4 | 15 | 250937 | 298559 | 1.1570 | 1.0772..1.2888 | 13530ns | 11184ns | 91351 | 74094 | 1.0853 |
| W02-threadpool-4k-d16-w1 | 15 | 233404 | 215267 | 0.9238 | 0.8564..1.2694 | 7582ns | 7661ns | 113048 | 105027 | 1.1043 |
| W02-threadpool-4k-d16-w4-saturate | 7 | 256209 | 294243 | 1.1870 | 1.0121..1.2238 | 13158ns | 11219ns | 0 | 0 | n/a |
| W03-threadpool-4k-d16-w4 | 15 | 157721 | 212592 | 1.3506 | 1.0201..2.0408 | 20046ns | 14943ns | 130844 | 89749 | 0.8437 |
