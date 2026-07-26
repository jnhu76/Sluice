-- Bench targets. Built/run via `xmake -g bench`.

-- Core microbench targets (SLUICE-CORE-010C-F).
local benches = { "small_writes_bench", "copy_strategy_bench", "wal_write_bench",
                  "sync_smoke_bench" }
for _, b in ipairs(benches) do
    sluice_one_file_target("binary", "bench", b, "bench", {"sluice_core", "sluice_bench_common"})
end

-- W1-W4 blocking bench matrix (sluice-CORE-022S). Linked against
-- sluice_bench_common (which carries BlockingIoPool + the matrix CSV helpers).
local sync_benches = { "w1_write_bench", "w2_read_bench", "w3_copy_bench",
                       "w4_durability_bench" }
for _, b in ipairs(sync_benches) do
    sluice_one_file_target("binary", "bench", b, "bench",
                          {"sluice_core", "sluice_bench_common"})
end

-- uring_write_bench needs the experimental uring lib too (stub or real).
sluice_one_file_target("binary", "bench", "uring_write_bench", "bench",
                      {"sluice_core", "sluice_bench_common", "sluice_experimental_uring"})

-- pool_throughput_bench (sluice-CORE-024S): production pool scalability sweep.
sluice_one_file_target("binary", "bench", "pool_throughput_bench", "bench",
                      {"sluice_core", "sluice_bench_common"})

-- async_writes_bench (sluice-CORE-022) needs the async runtime lib too.
sluice_one_file_target("binary", "bench", "async_writes_bench", "bench",
                      {"sluice_core", "sluice_bench_common", "sluice_async"})
