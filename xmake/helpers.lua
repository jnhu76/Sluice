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
        if deps_list then add_deps(deps_list) end
        add_includedirs(R .. "include", R .. "bench")
        add_files(path)
        if group == "test" then add_tests(name) end
end

-- Declare a one-file test target with optional platform gate.
--
-- `options.deps` is REQUIRED: this helper intentionally has no runtime
-- default. Use sluice_production_async_test / sluice_internal_async_test
-- (below) for async tests, or pass deps explicitly for one-off cases, so the
-- linked runtime is never chosen by accident.
function sluice_one_file_test(name, options)
    options = options or {}
    local source = options.source or (R .. "tests/" .. name .. ".cpp")
    if not os.isfile(source) then return end
    if options.platform_gate and not is_plat(unpack(options.platform_gate)) then return end

    -- No silent runtime default: a test that links the wrong async variant
    -- silently no longer validates the hook-free production library. xmake's
    -- sandbox exposes `raise` (not plain `assert`) at script scope.
    if not options.deps then
        raise(name .. ": explicit deps required (use a sluice_*_async_test wrapper)")
    end

    target(name)
        set_kind("binary")
        set_default(false)
        set_group("test")
        -- Resolve include dirs into a NEW table; never mutate the caller's.
        local resolved = {}
        for i, dir in ipairs(options.includedirs or {}) do
            resolved[i] = R .. dir
        end
        if #resolved > 0 then
            add_includedirs(unpack(resolved))
        else
            add_includedirs(R .. "include", R .. "tests")
        end
        add_deps(options.deps)
        if options.defines then
            add_defines(options.defines)
        end
        add_files(source)
        add_tests(name)
end

-- Async test linking the PRODUCTION sluice_async (hook-free). These never see
-- SLUICE_ASYNC_INTERNAL_TESTING. Default include path is include/ only.
function sluice_production_async_test(name, options)
    options = options or {}
    if options.deps ~= nil then
        raise(name .. ": sluice_production_async_test sets its own deps")
    end
    -- Fresh table: never mutate what the caller passed in.
    local o = {
        source = options.source,
        platform_gate = options.platform_gate,
        defines = options.defines,
        deps = {"sluice_core", "sluice_async"},
        includedirs = options.includedirs or {"include"},
    }
    sluice_one_file_test(name, o)
end

-- Async test linking sluice_async_internal_testing (NOT production
-- sluice_async). These exercise deterministic causal seams exposed by
-- SLUICE_ASYNC_INTERNAL_TESTING and need the non-installed tests/ headers.
-- Default include path is include/ + tests/.
function sluice_internal_async_test(name, options)
    options = options or {}
    if options.deps ~= nil then
        raise(name .. ": sluice_internal_async_test sets its own deps")
    end
    -- Fresh table: never mutate what the caller passed in.
    local o = {
        source = options.source,
        platform_gate = options.platform_gate,
        defines = options.defines,
        deps = {"sluice_core", "sluice_async_internal_testing"},
        includedirs = options.includedirs or {"include", "tests"},
    }
    sluice_one_file_test(name, o)
end
