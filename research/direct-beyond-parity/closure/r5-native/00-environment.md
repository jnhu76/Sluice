# R5 Native Measurement — Environment Identity (serial measurement lane)

Date: 2026-10-29. Lane: exclusive serial (per mission hard boundary #6) — no
other benchmark ran concurrently; each timed run was preceded by a load check.

## Host [RUN]

| Field | Value | Command |
|---|---|---|
| Kernel | 7.2.11-200.fc44.x86_64 | `uname -r` |
| CPU | Intel Xeon E5-2666 v3 @ 2.90GHz, 1 socket, 20 CPUs (10C/SMT2), 1 NUMA node | `lscpu` |
| RAM | 62 GB, buff/cache 12 GB at lane start | `free -g` |
| Governor | `schedutil` (unchanged, per prep) | `cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor` |
| perf_event_paranoid | 2 (user-space sampling only; noted, not changed) | `sysctl kernel.perf_event_paranoid` |
| Load at lane start | 0.08 / 0.05 / 0.27 (1/5/15 min) | `uptime`, 09:21 |

## Filesystem [RUN]

`findmnt -T /home/jnhu/Source/dududu/build`:
`/home /dev/sda3[/home] btrfs rw,relatime,seclabel,compress=zstd:1,ssd,discard=async,space_cache=v2,subvolid=257,subvol=/home`
Matches the prep record. `/tmp` is tmpfs — no dataset or measurement file was
placed there.

## Identities carried from prep [TRANSCRIBED from r5-prep.md, re-verified where marked]

- dududu branch at measurement time: `r5-async-host-probe` (clean, contains the
  same `bbce768`/`732a8fb`/`cc053cf` prep commits); fio/DuckDB protocol files
  read from that checkout.
- `DUCKDB_SHA` = `069cc9f9b5be802405797faecc284961b07c70ef`; binary string
  prints `v0.0.1 (Unknown Version) 069cc9f9` (depth-1, no tag object — known
  prep blocker, identity is the SHA) [RUN `duckdb --version`].
- Dataset byte identity re-verified at lane start [RUN]:
  `sha256sum duckdb_benchmark_data/lineitem.parquet` →
  `b7583e65198aaa9522067fd849f9d08f2668df2e7608fef548ab386cab0ef911`,
  207,130,013 B — equals `build/duckdb-bench/dataset-sha256.txt` and the prep
  record.

## sudo usage disclosure

The ask granted sudo "仅用于测量权限" (measurement permissions only). It was
used for exactly two operations, both measurement protocol, nothing persisted:

1. `sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'` — cold-cache protocol for
   the fio `r0-sync-4k-buffered` section and the DuckDB cold I/O observation.
2. Nothing else. No kernel/governor/security setting was modified. The
   password appears in no file, log or result in this directory.

drop_caches is transient (the page cache repopulates on next touch); the prep
blocker ("需 Owner 明确授权后方可进测量 lane") is resolved by the measurement
ask's explicit measurement-permission sudo grant.

## Order of lane operations (all serial)

1. fio dataset generation (3 × 8 GiB /dev/urandom) → `01-fio-dataset.md`
2. fio formal runs, 6 sections × A/A repeats + cold-buffered → `01-fio-dataset.md`, `fio/*.json`
3. DuckDB SQL oracle + byte oracle re-verification → `02-duckdb-oracle.md`
4. DuckDB A/A timed runs (official benchmark + oracle suite) → `02-duckdb-aa.md`, `duckdb/*`
5. strace I/O exposure warm + cold → `03-duckdb-io-exposure.md`, `duckdb/io-trace-*`
6. Seam/hotspot/opportunity analysis → `04-analysis.md`
