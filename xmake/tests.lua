-- Rebuilt minimal test targets for surviving async behavior (TEST-0).
-- Plain C++ executables under tests/: no external framework, no production
-- code seams, not part of production libs/apps. Tests of internal lifecycle
-- components include the internal detail headers directly.

local R = SLUICE_ROOT

do
    local dir = R .. "tests"
    if os.isfile(dir .. "/async_core_lifecycle.cpp") then
        target("sluice_async_core_lifecycle")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", dir)
            add_files(dir .. "/async_core_lifecycle.cpp")
            add_tests("sluice_async_core_lifecycle")
    end
end
