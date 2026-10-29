# R5 Native Baseline — R0 analysis and verdict (serial lane)

Companion evidence: `00-environment.md`, `01-fio-dataset.md`,
`02-duckdb-oracle-aa.md`, `03-duckdb-io-exposure.md` + raw files under
`fio/` and `duckdb/`. All claims below are [RUN] in those files unless
marked otherwise.

## §8.5 questions

1. **fio Native Baseline real?** Yes. Six frozen sections (ioengine ×
   iodepth × bs × rw-mix × cache policy × dataset) executed on fio 3.40 /
   kernel 7.2.11 / btrfs compress=zstd:1, with per-run dataset SHA, governor
   and load records. Settled A/A pair spread ≤2.3% on read sections; round-1
   post-allocation outlier disclosed, not deleted.
2. **DuckDB Native Baseline real?** Yes. Official harness
   (`parquet_projection_direct.benchmark`, upstream result oracle) + 4-query
   SQL oracle, dataset byte identity, on the pinned upstream SHA
   `069cc9f9b5be…` (= remote `refs/tags/v1.5.6`, prep-verified).
3. **A/A noise acceptable?** Yes for the of-record configurations: DuckDB
   pinned baseline CV 4.5% (29 pairs), oracle-suite wall CV 1.7% (15 pairs),
   fio settled sections ≤2.3% (pair spreads). The ≥15-pair requirement is met
   on every workload (73 pairs total). The unpinned default-thread config
   (CV 7.4%, max pair 23.9%) is documented as the noise regime any future A/B
   must avoid by pinning + declared thread counts. Statistical limit: paired
   A/B at n=15 resolves ≈ ±3–5% on the pinned benchmark workload, ≈ ±1% on
   the oracle suite.
4. **SQL and byte oracle pass?** Yes — 4/4 CSV hashes byte-identical across
   lane runs and equal to the prep record; upstream result oracle passed in
   62/62 benchmark-runner invocations (zero INCORRECT); CLI aggregate printed
   the exact expected value.
5. **Sync FileSystem Read Seam located?** Yes — symbolized end to end:
   `PipelineExecutor → PhysicalTableScan → ParquetReader::Scan →
   ColumnReader → ThriftFileTransport::read → LocalFileSystem::Read →
   pread(2)`, exact-count (0 short of 492 calls), matching
   `docs/duckdb-stable-io-map.md` §1.1.
6. **Sync Direct adapter admissible?** See "Opportunity assessment" — on this
   host, for this workload, `O_DIRECT` as such has no identified win; the
   evidence points at a different (syscall-count / metadata-read) lever.
7. **True async host protocol boundary?** Out of this lane's scope — §8.4
   async admission analysis is `closure/r5-async-host.md`. What this lane
   contributes: the observed v1.5.6 read path is the synchronous pread chain
   on scan threads (one fd per thread), i.e. any adapter boundary starts from
   a sync seam, and wait-based wrappers must not be presented as
   completion-driven async.
8. **Next-stage adaptation/experiments:** an A/B of a vectored/metadata-batch
   seam against the same oracle + harness (not Direct-vs-buffered), on the
   W1b/W2 noise model, ≥15 pairs, pinning cores and thread counts.

## Opportunity assessment (Sluice Direct / composition levers) [RUN + analysis]

Host facts that bound the opportunity space:

- **No hidden device performance behind the page cache at small/random
  shapes.** fio `r0-sync-4k-direct` ≈ `r0-sync-4k-buffered` ≈ cold-buffered
  (~3.2–3.3k IOPS, ±noise): on btrfs `compress=zstd:1,ssd`, direct I/O buys
  nothing for 4k random reads, and the page cache buys nothing either. The
  only bandwidth-bearing shape is large sequential reads (~330–389 MiB/s).
- **DuckDB's reads are already large and projection-pruned.** 492 preads /
  37.17 MB per query (17.9% of the file), 83.4% of bytes in 49 × ~634 KB
  reads; 0 short reads. The storage-side shape is the *good* shape already.
- **`O_DIRECT` is not even directly issuable**: offsets/lengths unaligned
  (e.g. offset mod 512 = 467, size mod 512 = 266). A Direct adapter needs
  realignment copies, and fio says the underlying device offers no
  4k-random win to pay them with. Warm page cache removes ≈ 8–14 ms
  (~25–40% of the first query, ~30% of one steady query's I/O component)
  after drop_caches; Direct would forfeit that caching for no observed
  small-I/O benefit.
- **The real lever this baseline exposes is syscall/metadata traffic, not
  cache policy**: 70% of preads (344/492) are ≤4 KiB metadata reads moving
  0.13% of bytes (147 × 256 B page headers + thrift tails). At ~1–2 µs per
  syscall plus per-read locking this is a CPU/latency tax on the scan thread
  path, precisely the shape AC-1-style vectored reads or a metadata-batch
  seam targets. On the W1b noise model only effects ≥~5% (pinned benchmark)
  are resolvable at n=15; a metadata-read elimination A/B must therefore be
  sized against that threshold, or measured on the oracle-suite workload
  (±1%).

Conclusion: the honest R0 statement for #578 on this host is **"sync seam
exposed and characterized; no Direct-I/O opportunity demonstrated; a bounded
syscall-reduction opportunity identified and measurable with the frozen A/A
methodology."** This is not an application acceleration claim — it is the
native floor + seam identity the next owner gate needs.

## Verdict

`R0_READY_FOR_NEXT_OWNER_GATE` — fio and DuckDB native baselines are real,
oracles pass, noise is characterized with ≥15 pairs per workload, the sync
read seam is located and symbolized, and the opportunity space is bounded by
measured evidence. Residual gaps (none blocking the gate):

- kernel-level perf hotspot attribution: NOT_RUN_WITH_REASON
  (`perf_event_paranoid=2`; relaxing a security setting was not authorized;
  user-space seam evidence obtained via `strace -k` and the built-in
  profiler instead).
- `duckdb --version` display string remains `v0.0.1 (Unknown Version) 069cc9f9`
  (depth-1 tag object missing after prep's network failure — cosmetic; all
  identity recorded by SHA).
- fio A/A is n=2–3 per section (baseline identity + pair noise, no
  hypothesis-test power claim); randrw section under-sampled (2 rounds,
  mutating dataset).
- cold-cache protocol (drop_caches) was executed in this lane under the ask's
  measurement-permission sudo grant; the prep-phase blocker is superseded by
  that grant and every use is logged in `00-environment.md`.
