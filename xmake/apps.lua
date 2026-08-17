-- Reference applications. Built/run via `xmake -g apps`.
-- These live under apps/ (NOT examples/): they prove multiple APIs compose
-- into a real program, using installed/public headers only. No test include
-- dirs, no SLUICE_ASYNC_INTERNAL_TESTING, no src/ include.

local R = SLUICE_ROOT

-- sluice-copy (M1-A Version A/B + Version C): sequential async positional file
-- copy and the bounded reusable-buffer pipeline (Version B), both over
-- ApplicationRuntime + ThreadPoolBackend; Version C adds the safe atomic
-- output path (temp file in the destination directory + rename + directory
-- durability, safe_output.{hpp,cpp}). Multi-file target (main + the
-- copy_task/safe_output libraries used by both the CLI and the tests, plus
-- the small cli_parse / file_domain modules compiled into the CLI and its
-- tests). Public headers only; production async library only.
do
    local dir = R .. "apps/sluice-copy"
    if os.isfile(dir .. "/main.cpp") then
        target("sluice-copy")
            set_kind("binary")
            set_default(false)
            set_group("apps")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", dir)
            add_files(dir .. "/main.cpp", dir .. "/copy_task.cpp",
                      dir .. "/cli_parse.cpp", dir .. "/file_domain.cpp",
                      dir .. "/safe_output.cpp")
    end
end

-- sluice-hash: bounded streaming SHA-256 file hashing over ApplicationRuntime
-- + ThreadPoolBackend. Multi-file target (main + the hash engine / SHA-256 /
-- CLI modules also compiled into the app's tests). Public headers only.
do
    local dir = R .. "apps/sluice-hash"
    if os.isfile(dir .. "/main.cpp") then
        target("sluice-hash")
            set_kind("binary")
            set_default(false)
            set_group("apps")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", dir)
            add_files(dir .. "/main.cpp", dir .. "/hash_task.cpp",
                      dir .. "/sha256.cpp", dir .. "/cli_parse.cpp")
    end
end

-- sluice-grep: bounded streaming literal line search over ApplicationRuntime
-- + ThreadPoolBackend. Multi-file target (main + the engine / matcher / CLI
-- modules also compiled into the app's tests). Public headers only.
do
    local dir = R .. "apps/sluice-grep"
    if os.isfile(dir .. "/main.cpp") then
        target("sluice-grep")
            set_kind("binary")
            set_default(false)
            set_group("apps")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", dir)
            add_files(dir .. "/main.cpp", dir .. "/grep_task.cpp",
                      dir .. "/matcher.cpp", dir .. "/cli_parse.cpp")
    end
end

-- sluice-tail: bounded last-N backward scan + follow-mode tailing over
-- ApplicationRuntime + ThreadPoolBackend. The long-lived workload of the
-- file-tools track: explicit start/request_stop/wait engine lifecycle, and a
-- sigwait signal thread in the CLI for clean Ctrl-C cancellation. Public
-- headers only.
do
    local dir = R .. "apps/sluice-tail"
    if os.isfile(dir .. "/main.cpp") then
        target("sluice-tail")
            set_kind("binary")
            set_default(false)
            set_group("apps")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", dir)
            add_files(dir .. "/main.cpp", dir .. "/tail_task.cpp",
                      dir .. "/cli_parse.cpp")
    end
end
