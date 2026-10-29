# R5 DuckDB Native — I/O Exposure at the FileSystem Read Seam (serial lane) [RUN]

Method: `strace -f -y -e trace=openat,pread64,read,close` (and `-k` for stack
sampling) around the CLI aggregate
`SELECT SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) FROM
read_parquet('duckdb_benchmark_data/lineitem.parquet')` — the official
benchmark's own query. strace runs are never used for timing. Raw traces:
`duckdb/io-trace-{cold,warm,count,1col-price,1col-discount,kstack}.txt`.

## Cold vs warm — identical request stream [RUN]

Cold run preceded by `sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'`
(measurement-only sudo, see 00-environment.md):

| | cold | warm |
|---|---|---|
| pread64 on lineitem.parquet | 492 | 492 |
| bytes returned | 37,172,128 | 37,172,128 |
| short reads / zero-byte returns | 0 / 0 | 0 / 0 |
| ≤4 KiB calls (bytes) | 344 (48,674) | 344 (48,674) |
| 32–64 KiB calls (bytes) | 98 (6,049,876) | 98 (6,049,876) |
| 64–256 KiB calls (bytes) | 1 — footer 79,613 B | 1 |
| ≥256 KiB calls (bytes) | 49 (30,993,965) | 49 (30,993,965) |

(The complete-vs-`resumed` strace line split differs between runs — a strace
thread-interleaving artifact only; totals are exact.) **The page cache changes
latency, not the request stream**: DuckDB v1.5.6 issues byte-identical reads
cold and warm. 37,172,128 / 207,130,013 = 17.9% of the file (projection
pushdown over a 16-column table).

Read-shape histogram (both runs):

- 49 × ~634.5–634.7 KB (row-group column-chunk bulk = 83.4% of bytes)
- 98 × ~61.9 KB (55×61,935, 32×61,937, 7×61,939…)
- 1 × 79,613 B (parquet footer/metadata)
- 344 × ≤4 KiB totalling 48,674 B (147 × 256 B page headers + a 46–68 B
  thrift-metadata tail) — **70% of all calls move 0.13% of the bytes**

Per-column attribution [RUN]: `SUM(l_extendedprice)` alone → 100 preads /
31,086,130 B; `SUM(l_discount)` alone → 198 preads / 3,123,007 B (dictionary
compression); `COUNT(*)` → **2 preads / 79,621 B** (pure metadata floor — the
result comes from footer metadata without touching column data).

## The sync FileSystem Read Seam — located and symbolized [RUN]

`strace -k` stack of a representative ~634 KB data read (and the 256 B
metadata reads show the same seam), from `duckdb/io-trace-kstack.txt`:

```
__libc_pread  → pread(2), return value == requested size (exact count)
 ← duckdb::LocalFileSystem::Read(duckdb::FileHandle&, void*, long, unsigned long)
 ← duckdb::ThriftFileTransport::read(unsigned char*, unsigned int)
 ← duckdb::ParquetReader::ReadData(duckdb_apache::thrift::protocol::TProtocol&, unsigned char*, unsigned int)
 ← duckdb::ColumnReader::PreparePage(duckdb_parquet::PageHeader&)
 ← duckdb::ColumnReader::PrepareRead / ReadPageHeaders / ReadInternal
 ← duckdb::ParquetReader::Scan(duckdb::ClientContext&, duckdb::ParquetReaderScanState&, duckdb::DataChunk&)
 ← duckdb::MultiFileFunction<duckdb::ParquetMultiFileInfo>::MultiFileScan(...)
 ← duckdb::PhysicalTableScan::GetDataInternal(...)
 ← duckdb::PipelineExecutor::FetchFromSource / PipelineExecutor::Execute
```

This confirms the v1.5.6 sync chain documented in
`docs/duckdb-stable-io-map.md` §1.1 (`LocalFileSystem::Read` → `pread(2)`) on
this binary. Both data and metadata reads flow through the one seam; each scan
thread opens its own fd (28–31 `openat`s of the same file per query; all
`O_RDONLY`, no `read(2)` on the dataset at all).

Alignment facts [RUN, computed from the trace]: bulk reads sit at unaligned
offsets and lengths — e.g. offset 1,147,347 (mod 512 = 467), size 634,634
(mod 512 = 266); none of the traced reads are 512- or 4096-aligned in both
offset and length. **None of this I/O can be issued through `O_DIRECT` as-is**;
a direct-IO adapter would have to realign via bounce buffers or change the
reader.

Operator attribution (built-in `PRAGMA enable_profiling='query_tree'`, one
run, `duckdb/query-profile.txt`): scan-dominated — TABLE_SCAN over 3 projected
columns ≈ 0.33 thread-seconds of the ≈0.033 s wall at 20 threads, aggregate
and projection are minor.

## Kernel-level perf hotspot: NOT_RUN_WITH_REASON

`perf record -e cycles:u` fails on this host: `kernel.perf_event_paranoid = 2`
denies unprivileged event opening ("Failure to open any events for recording").
Raising `perf_event_paranoid` is a global security-setting change outside the
measurement-permission sudo grant, so it was not attempted. User-space
hotspot/call-chain evidence was obtained instead via `strace -k` (above) and
DuckDB's built-in profiler; syscall-level seam facts are complete, instruction
level attribution is not.
