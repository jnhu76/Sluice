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

-- public_api_acceptance — public-only acceptance consumer. Uses INSTALLED/
-- PUBLIC headers only (no tests/ include path, no SLUICE_ASYNC_INTERNAL_TESTING,
-- no private source). Exercises Result + AsyncIoContext + Completion + Batch
-- end-to-end against the production sluice_async. Build + run proves the public
-- async-foundation surface is usable from a consumer that sees only the
-- installed headers.
do
    local R = SLUICE_ROOT
    local p = R .. "examples/public_api_acceptance.cpp"
    if os.isfile(p) then
        target("public_api_acceptance")
            set_kind("binary")
            set_default(false)
            set_group("examples")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include")
            add_files(p)
    end
end
