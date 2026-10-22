# E1 Native-Linux Validation — W-02 A/B (Gate R3)

Campaign: 15 valid interleaved AB/BA-rotated pairs per config, 11 configs,
both arms on pinned CPU 2, tmpfs fixture, byte-oracle phase required for
validity. Raw: `~/sluice-e1-e5/results/ab.jsonl` (990 rows; every
(rep, cfg, arm, phase) cell present exactly once), `frequency.jsonl`
(per-run CPU frequency and loadavg), `provenance.txt`, `campaign-status.txt`.

Primary metric: timed-phase process `cpu_ns_per_op`, per-rep paired.
Reducer status: **w02 = PASS** (all preregistered gates).

## Paired E1/Base ratios (process CPU per op)

| Config (block-C-D) | median ratio | pairs < 1 | gate |
|---|---:|---:|---|
| 4096-C1024-D1 (headline) | **0.6930** | 15/15 | GAIN ✓ |
| 4096-C1024-D8 (headline) | **0.6239** | 15/15 | GAIN ✓ |
| 4096-C1024-D64 | 0.5928 | 15/15 | (stronger at higher depth) |
| 4096-C256-D1 | 0.8723 | 15/15 | dose response ✓ |
| 4096-C64-D1 | 0.9533 | 15/15 | dose response ✓ |
| 4096-C64-D64 | 0.8977 | 15/15 | |
| 4096-C16-D8 | 0.9724 | 14/15 | |
| **4096-C16-D1 (control)** | 0.9962 | 9/15 | neutral ✓ |
| 65536-C1024-D1 | 0.8612 | 15/15 | |
| 65536-C1024-D8 | 0.9171 | 15/15 | |
| **1048576-C64-D8 (control)** | 0.9958 | 10/15 | neutral ✓ |

Full ratio lists are in `verdict.json` (`w02.per_cfg[*].ratios`). Spread per
config is tight (headline D1 [0.662..0.711], D8 [0.600..0.657]); no config
shows a single pair above 1.03 outside the C16 control's noise.

## Reading

- Dose response matches the mechanism exactly: the removed work is
  O(poll passes × C), so the gain scales with capacity (C16 ≈ 1.00 →
  C64 0.95 → C256 0.87 → C1024 0.69/0.62) and grows with depth (more
  completions per pass amortize differently at D8/D64).
- Negative controls (C16, 1MiB) are neutral (0.996 both): the counter
  maintenance costs nothing measurable when P=0 and scans are rare — the
  zero-count gate costs one load+branch per service call.
- WSL2 medians for the two headline configs were 0.858 / 0.731; native E5
  shows larger gains (0.693 / 0.624). Mechanism attribution
  (`cpu-attribution.md`) shows why: the scan dominates more on native
  kernel/liburing fast paths.
- Statistics: 15/15 same-direction pairs at the headline configs
  (one-side binomial p ≈ 1.4e-5 if treated as independent — they are not
  fully independent; the preregistered 12/15 gate plus effect size plus
  mechanism attribution carry the conclusion). 11 simultaneous comparisons
  are flagged as a multiple-comparison limitation in `verdict.json`.
- `max_outstanding == depth` on every timed row (achieved in-flight depth
  confirmed at the backend boundary); kernel-side in-flight depth is not
  directly observable without tracefs and is not claimed.
