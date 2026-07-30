-- Reference applications. Built/run via `xmake -g apps`.
-- These live under apps/ (NOT examples/): they prove multiple APIs compose
-- into a real program, using installed/public headers only. No test include
-- dirs, no SLUICE_ASYNC_INTERNAL_TESTING, no src/ include.

local R = SLUICE_ROOT

-- sluice-copy (M1-A, Version A): sequential asynchronous positional file copy
-- over ApplicationRuntime + ThreadPoolBackend. Multi-file target (main + the
-- copy_task library used by both the CLI and the tests). Public headers only;
-- production async library only.
do
    local dir = R .. "apps/sluice-copy"
    if os.isfile(dir .. "/main.cpp") then
        target("sluice-copy")
            set_kind("binary")
            set_default(false)
            set_group("apps")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", dir)
            add_files(dir .. "/main.cpp", dir .. "/copy_task.cpp")
    end
end
