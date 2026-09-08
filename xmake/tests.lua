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

local witness_workdir = os.tmpdir()

if os.isfile(R .. "apps/sluice-copy/main.cpp") then
    target("sluice-copy")
        add_tests("copy-proc-version",
                  {runargs = {"/proc/version",
                              path.join(witness_workdir, "sluice-copy-witness-output.out")},
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

if os.isfile(R .. "apps/sluice-tail/main.cpp") and os.isfile(R .. "tests/tail_witness_input.txt") then
    target("sluice-tail")
        add_tests("tail-prints-last-line-of-file",
                  {runargs = {"-n", "1", R .. "tests/tail_witness_input.txt"},
                   pass_output = "sluice-tail-witness-last-line",
                   trim_output = true,
                   run_timeout = 30000})
    target_end()
end
