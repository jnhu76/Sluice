-- Specification-witness test targets. Public headers only; no internal test
-- macros, no src/ include path (same discipline as apps.lua). Targets appear
-- only when their source file exists.

local R = SLUICE_ROOT

local function sluice_witness_target(name)
    local path = R .. "tests/" .. name .. ".cpp"
    if not os.isfile(path) then
        return
    end
    target(name)
        set_kind("binary")
        set_default(false)
        set_group("tests")
        add_deps("sluice_core", "sluice_async")
        add_includedirs(R .. "include", R .. "tests")
        add_files(path)
        add_tests("default", {run_timeout = 60000})
end

sluice_witness_target("async_io_contracts")
sluice_witness_target("runtime_contracts")
sluice_witness_target("sync_core_contracts")

if os.isfile(R .. "apps/sluice-copy/main.cpp") then
    target("sluice-copy")
        add_tests("copy-proc-version",
                  {runargs = {"/proc/version", "/tmp/sluice-copy-witness-proc-version.out"},
                   run_timeout = 30000})
    target_end()
end

if os.isfile(R .. "apps/sluice-hash/main.cpp") then
    target("sluice-hash")
        add_tests("hash-proc-version", {runargs = {"/proc/version"}, run_timeout = 30000})
    target_end()
end

if os.isfile(R .. "apps/sluice-grep/main.cpp") then
    target("sluice-grep")
        add_tests("grep-matches-pattern-in-proc-version",
                  {runargs = {"Linux", "/proc/version"}, run_timeout = 30000})
    target_end()
end

if os.isfile(R .. "apps/sluice-tail/main.cpp") then
    target("sluice-tail")
        add_tests("tail-proc-version", {runargs = {"-n", "1", "/proc/version"}, run_timeout = 30000})
    target_end()
end
