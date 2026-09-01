# aligne1-validate-native-6 — notes

VALID (harness validation GREEN): 48 runs, 0 gate errors. 3 modules x
{4K,64K,1M} x {d1,d4} x 2 reps + 12-run cycles probe (12 more runs, all
ok). File size 128 MiB (AMENDMENT 1).

Per-op probe values (from raw/runs.jsonl, cycles:u / (read+write ops)):
  replica-natural 4K  d1:  9042.6 -> 9192.2 -> 9219.7  (diffs +149.6, +27.5)
  replica-aligned 4K  d1: 10375.2 -> 9230.5 -> 8887.6  (diffs -1144.7, -342.9)
  replica-natural 64K d1: 11898.6 -> 11364.8 -> 13458.6 (diffs -533.8, +2093.8)
  replica-aligned 64K d1: 11496.9 -> 14704.6 -> 13397.5 (diffs +3207.7, -1307.1)
Negative consecutive double-differences present -> cycles:u DEMOTED
(prereg §6 rule), same conclusion as ALIGN-E0 B.6 (turbo/frequency
scaling between process runs). instructions:u per-op stable per cell
(7801 @4K, 8449-8450 @64K, identical across modules) — the quantitative
instruction pair.

Machine: bare metal Fedora 44, kernel 7.1.9-200.fc44.x86_64, Xeon
E5-2666 v3 10C/20T, btrfs zstd:1 SATA SSD, page 4096; collocated with the
ALIGN-E0 native sessions. Environment: environment.json (HEAD f3b38909
+ driver probe-eval fix, clean tree at run time).