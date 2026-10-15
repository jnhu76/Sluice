# perf capture integrity

Raw `perf.data` files are not committed; this table is what makes them
checkable. `manifest` compares the file's SHA-256 with MANIFEST.sha256
section 2. `case` and `reps` are read back from the run's own JSONL,
`samples` from `perf report --stats`.

| case | recorded case | warmup+measured | SAMPLE events | size | manifest |
|---|---|---|---|---|---|
| w02-tp-4k-d16-w4 | W02-threadpool-4k-d16-w4 | 1+40 | 249009 | 34M | ok |
| w02-uring-4k-d16 | W02-uring-4k-d16 | 1+40 | 132283 | 19M | ok |
| w02-tp-4k-d16-w1 | W02-threadpool-4k-d16-w1 | 1+20 | 52099 | 7.1M | ok |
| w02-tp-4k-d16-w8 | W02-threadpool-4k-d16-w8 | 1+20 | 252993 | 35M | ok |
| w02-uring-4k-d64 | W02-uring-4k-d64 | 1+20 | 78853 | 11M | ok |
| w02-tp-64k-d16-w4 | W02-threadpool-64k-d16-w4 | 1+20 | 20030 | 3.2M | ok |
| w03-tp-4k-d16-w4 | W03-threadpool-4k-d16-w4 | 1+20 | 301030 | 42M | ok |
| w03-uring-4k-d16 | W03-uring-4k-d16 | 1+20 | 120026 | 17M | ok |
| w01-read-4k-seq | W01-read_at-4k-seq | 1+20 | 37475 | 6.2M | ok |
| w01-write-4k-seq | W01-write_at-4k-seq | 1+20 | 50861 | 8.5M | ok |
| w01-sync-data-4k | W01-sync_data-4k | 1+20 | 3881 | 936K | ok |

All 11 captures match the manifest.
