## MAIN-W03-D16 absolute means (per arm, mean over 24 blocks)

| arm | cpu ns/op | p50 µs | p95 µs | p99 µs | ops/s |
|---|---:|---:|---:|---:|---:|
| A0 | 3208.6 | 37.35 | 46.26 | 53.92 | 311964 |
| R | 2519.1 | 39.16 | 47.84 | 64.63 | 397684 |
| C | 2499.4 | 38.70 | 46.73 | 63.44 | 400746 |

## MAIN-W03-D16 contrasts (paired block effects, % of A0 or R)

| contrast | cpu | throughput | p95 | p99 |
|---|---|---|---|---|
| C−A0 | -22.09% [-22.75,-21.41] | +28.49% [+27.35,+29.59] | +1.15% [-1.78,+4.10] | +17.97% [+15.18,+20.82] |
| R−A0 | -21.47% [-22.25,-20.65] | +27.51% [+26.19,+28.78] | +3.46% [+1.21,+5.73] | +20.14% [+17.50,+22.88] |
| C−R | -0.73% [-2.02,+0.50] | +0.83% [-0.45,+2.20] | -2.10% [-4.86,+0.87] | -1.77% [-3.16,-0.33] |

## Gate evaluation (frozen thresholds, one-sided 95% bounds)

| cell | item | point | 1s-bound | threshold | pass | p | p_holm |
|---|---|---:|---:|---|---|---:|---:|
| MAIN-W03-D16 | MAIN_CpuReduction | -22.09% | -21.51% (upper95) | point≤-10% & upper95≤-5% | PASS | 0.0001 | 0.0004 |
| MAIN-W03-D16 | MAIN_Throughput | +28.49% | +27.53% (lower95) | lower95≥-3% | PASS | 0.0001 | 0.0004 |
| MAIN-W03-D16 | MAIN_P95 | +1.15% | +3.66% (upper95) | upper95≤+5% | PASS | 0.0041 | 0.0082 |
| MAIN-W03-D16 | MAIN_P99 | +17.97% | +20.34% (upper95) | upper95≤+5% | FAIL | 1.0000 | 1.0000 |
| MAIN-W03-D16 | **cell verdict** | | | | **FAIL** | | |
| PROT-W03-D4 | ProtCpu | -16.81% | -16.18% (upper95) | upper95≤+5% | PASS | 0.0001 | 0.0004 |
| PROT-W03-D4 | ProtTput | +20.31% | +19.40% (lower95) | lower95≥-3% | PASS | 0.0001 | 0.0004 |
| PROT-W03-D4 | ProtP95 | -22.44% | -20.57% (upper95) | upper95≤+5% | PASS | 0.0001 | 0.0004 |
| PROT-W03-D4 | ProtP99 | -19.14% | -15.91% (upper95) | upper95≤+5% | PASS | 0.0001 | 0.0004 |
| PROT-W03-D4 | **cell verdict** | | | | **PASS** | | |
| PROT-W03-D2SERIAL | ProtCpu | +3.90% | +4.90% (upper95) | upper95≤+5% | PASS | 0.0364 | 0.1456 |
| PROT-W03-D2SERIAL | ProtTput | -3.67% | -4.59% (lower95) | lower95≥-3% | FAIL | 0.8770 | 1.0000 |
| PROT-W03-D2SERIAL | ProtP95 | +9.17% | +10.83% (upper95) | upper95≤+5% | FAIL | 1.0000 | 1.0000 |
| PROT-W03-D2SERIAL | ProtP99 | +8.47% | +13.97% (upper95) | upper95≤+5% | FAIL | 0.8598 | 1.0000 |
| PROT-W03-D2SERIAL | **cell verdict** | | | | **FAIL** | | |
| PROT-W02-D1 | ProtCpu | +5.37% | +5.92% (upper95) | upper95≤+5% | FAIL | 0.8708 | 1.0000 |
| PROT-W02-D1 | ProtTput | -5.09% | -5.59% (lower95) | lower95≥-3% | FAIL | 1.0000 | 1.0000 |
| PROT-W02-D1 | ProtP95 | +4.72% | +5.55% (upper95) | upper95≤+5% | FAIL | 0.3271 | 0.9813 |
| PROT-W02-D1 | ProtP99 | +3.94% | +5.81% (upper95) | upper95≤+5% | FAIL | 0.1970 | 0.7880 |
| PROT-W02-D1 | **cell verdict** | | | | **FAIL** | | |
| PROT-W02-D2 | ProtCpu | +5.78% | +6.26% (upper95) | upper95≤+5% | FAIL | 0.9958 | 1.0000 |
| PROT-W02-D2 | ProtTput | -5.46% | -5.89% (lower95) | lower95≥-3% | FAIL | 1.0000 | 1.0000 |
| PROT-W02-D2 | ProtP95 | +4.90% | +5.83% (upper95) | upper95≤+5% | FAIL | 0.4030 | 1.0000 |
| PROT-W02-D2 | ProtP99 | +5.67% | +9.13% (upper95) | upper95≤+5% | FAIL | 0.6159 | 1.0000 |
| PROT-W02-D2 | **cell verdict** | | | | **FAIL** | | |

## Cross-binary calibration A0c−A0o (same product path, two binaries)

| cell | cpu | p99 |
|---|---|---|
| MAIN-W03-D16 | -1.45% [-2.30,-0.58] | -1.47% [-3.93,+0.98] |
| PROT-W03-D4 | -1.49% [-3.63,+0.44] | -5.35% [-7.71,-3.42] |
| PROT-W03-D2SERIAL | -2.28% [-3.85,-0.89] | +8.98% [-5.80,+23.76] |
| PROT-W02-D1 | +2.08% [+1.05,+3.22] | +3.61% [+1.10,+6.71] |
| PROT-W02-D2 | +3.38% [+1.88,+4.96] | +5.41% [+3.55,+7.33] |

## C−R raw effects vs calibration band (report only — the band is a
same-path binary effect and need not transfer to the R path; no joint
uncertainty model, so residuals are observations, not estimates)

| cell | C−R cpu | calib cpu | C−R p99 | calib p99 |
|---|---|---|---|---|
| MAIN-W03-D16 | -0.73% | -1.45% | -1.77% | -1.47% |
| PROT-W03-D4 | -3.86% | -1.49% | -2.86% | -5.35% |
| PROT-W03-D2SERIAL | -4.42% | -2.28% | -4.74% | +8.98% |
| PROT-W02-D1 | +3.22% | +2.08% | +3.50% | +3.61% |
| PROT-W02-D2 | +3.24% | +3.38% | +2.82% | +5.41% |
