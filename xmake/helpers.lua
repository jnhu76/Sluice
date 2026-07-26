-- Helper functions shared across test/example/bench target declarations.

local unpack = table.unpack or unpack
local R = SLUICE_ROOT

-- Declare a test/example/bench target only when its source file exists, so
-- xmake does not warn about missing files for slices not yet written.
function sluice_one_file_target(kind, group, name, subdir, deps_list)
    local path = R .. subdir .. "/" .. name .. ".cpp"
    if not os.isfile(path) then return end
    target(name)
        set_kind(kind)
        set_default(false)
        set_group(group)
        if deps_list then add_deps(deps_list) end
        add_includedirs(R .. "include", R .. "bench")
        add_files(path)
        if group == "test" then add_tests(name) end
end

-- Declare a one-file test target with optional platform gate and deps override.
-- Defaults: links sluice_core + sluice_async_internal_testing, includes include/tests.
function sluice_one_file_test(name, options)
    options = options or {}
    local source = options.source or (R .. "tests/" .. name .. ".cpp")
    if not os.isfile(source) then return end
    if options.platform_gate and not is_plat(unpack(options.platform_gate)) then return end

    target(name)
        set_kind("binary")
        set_default(false)
        set_group("test")
        local inc = options.includedirs
        if inc and #inc > 0 then
            for i = 1, #inc do inc[i] = R .. inc[i] end
            add_includedirs(unpack(inc))
        else
            add_includedirs(R .. "include", R .. "tests")
        end
        if options.deps then
            add_deps(options.deps)
        else
            add_deps("sluice_core", "sluice_async_internal_testing")
        end
        if options.defines then
            add_defines(options.defines)
        end
        add_files(source)
        add_tests(name)
end
