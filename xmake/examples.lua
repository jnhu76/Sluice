-- Buildable examples. Built/run via `xmake -g examples`.

local examples = { "cat", "copy_file", "small_writes", "fault_write", "wal_records",
                   "mvp_copy_pipeline", "mvp_limited_copy", "mvp_wal_vector",
                   "mvp_copy_strategy", "mvp_wal_durable", "mvp_io_context_copy",
                   "mvp_memory_io_context", "sync_random_read", "blocking_io_pool" }
for _, e in ipairs(examples) do
    sluice_one_file_target("binary", "examples", e, "examples", "sluice_core")
end

-- experimental_uring_write links the experimental uring lib (stub or real).
sluice_one_file_target("binary", "examples", "experimental_uring_write", "examples",
                      {"sluice_core", "sluice_experimental_uring"})
