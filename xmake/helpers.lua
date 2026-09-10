-- Helper functions shared across test/example/bench target declarations.
--
-- Two async-test wrappers (sluice_production_async_test /
-- sluice_internal_async_test) make the linked runtime EXPLICIT at every call
-- site. The underlying sluice_one_file_test deliberately has NO runtime
-- default and asserts that deps were supplied, so a future test that forgets
-- to pick a wrapper fails loudly instead of silently linking the test-seam
-- variant (the regression this guard closes).

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
        if deps_list then
            local deps = {}
            for dep in deps_list:gmatch("[^,%s]+") do
                table.insert(deps, dep)
            end
            add_deps(unpack(deps))
        end
        add_includedirs(R .. "include")
        add_files(path)
        if group == "test" then add_tests(name) end
end
