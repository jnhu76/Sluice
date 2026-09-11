-- Helper functions shared across test target declarations.
--
-- sluice_one_file_target declares a one-file target only when its source
-- file exists. Library deps are supplied explicitly by each call site
-- (see xmake/tests.lua); nothing is linked implicitly.

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
        add_includedirs(R .. "include")
        add_files(path)
        if group == "test" then add_tests(name) end
end
