# A/B comparison

| case | reps | B0 ops/s | arm ops/s | paired ratio (median) | paired min..max | B0 cpu/op | arm cpu/op | B0 p99 ns | arm p99 ns | p99 worst |
|---|---|---|---|---|---|---|---|---|---|---|
| W02-threadpool-4k-d16-w4 | 7 | 287247 | 284442 | 1.0158 | 0.9246..1.1062 | 11323ns | 11690ns | 77902 | 79037 | 1.0988 |
| W03-threadpool-4k-d16-w4 | 7 | 204033 | 222482 | 1.0654 | 1.0180..1.1438 | 15481ns | 14304ns | 92452 | 85944 | 1.0350 |
| W02-uring-4k-d16 | 7 | 319401 | 373252 | 1.0910 | 0.9076..1.2361 | 5420ns | 4724ns | 60968 | 57614 | 1.0533 |
