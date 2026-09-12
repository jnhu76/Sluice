local R = SLUICE_ROOT

sluice_one_file_target("binary", "test", "file_read_test", "tests", {"sluice_core", "sluice_async"})
sluice_one_file_target("binary", "test", "file_resource_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "file_open_contract_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "file_write_test", "tests", {"sluice_core", "sluice_async"})
sluice_one_file_target("binary", "test", "file_sync_data_test", "tests", {"sluice_core", "sluice_async"})
sluice_one_file_target("binary", "test", "file_sync_all_test", "tests", {"sluice_core", "sluice_async"})
sluice_one_file_target("binary", "test", "explicit_file_ref_test", "tests", {"sluice_core", "sluice_async"})
sluice_one_file_target("binary", "test", "async_sync_admission_test", "tests", {"sluice_core", "sluice_async"})
sluice_one_file_target("binary", "test", "blocking_file_read_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "blocking_file_write_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "blocking_file_sync_data_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "blocking_file_state_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "blocking_file_sequential_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "blocking_file_sync_all_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "io_validation_boundary_test", "tests", "sluice_core")

-- Real io_uring verification: registered only when the liburing build switch
-- is on. Both targets consume sluice_async as ordinary consumers: the macro
-- and the liburing link arrive through sluice_async's public usage
-- requirement and are never hand-copied here. The probe witnesses the
-- consumer-side definition contract; the smoke drives real submissions.
if has_config("liburing") then
    target("uring_public_consumer_probe")
        set_kind("binary")
        set_default(false)
        set_group("test")
        add_deps("sluice_core", "sluice_async")
        add_includedirs(R .. "include")
        add_files(R .. "tests/uring_public_consumer_probe.cpp")
        add_tests("uring_public_consumer_probe")
end

-- A2 app-consumption tests: exercise the app engines through the canonical
-- File resource they now consume. Each target compiles the app's engine
-- modules with public headers only, mirroring the app target's dependency
-- shape (no test seams, no src/ include).
do
    local dir = R .. "apps/sluice-hash"
    if os.isfile(dir .. "/hash_task.cpp") then
        target("app_hash_consumption_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", dir)
            add_files(R .. "tests/app_hash_consumption_test.cpp", dir .. "/hash_task.cpp",
                      dir .. "/sha256.cpp")
            add_tests("app_hash_consumption_test")
    end
end

do
    local dir = R .. "apps/sluice-grep"
    if os.isfile(dir .. "/grep_task.cpp") then
        target("app_grep_consumption_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", dir)
            add_files(R .. "tests/app_grep_consumption_test.cpp", dir .. "/grep_task.cpp",
                      dir .. "/matcher.cpp")
            add_tests("app_grep_consumption_test")
    end
end

do
    local dir = R .. "apps/sluice-tail"
    if os.isfile(dir .. "/tail_task.cpp") then
        target("app_tail_consumption_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", dir)
            add_files(R .. "tests/app_tail_consumption_test.cpp", dir .. "/tail_task.cpp")
            add_tests("app_tail_consumption_test")
    end
end

do
    local dir = R .. "apps/sluice-copy"
    if os.isfile(dir .. "/copy_task.cpp") then
        target("app_copy_consumption_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", dir)
            add_files(R .. "tests/app_copy_consumption_test.cpp", dir .. "/copy_task.cpp",
                      dir .. "/file_domain.cpp", dir .. "/safe_output.cpp")
            add_tests("app_copy_consumption_test")
    end
end
