-- Production libraries: sluice_core, sluice_async.

local R = SLUICE_ROOT

-- Core static library: Reader/Writer abstractions + wrappers.
core_sources = function()
    return { R .. "src/*.cpp" }
end

target("sluice_core")
    set_kind("static")
    add_includedirs(R .. "include", {public = true})
    add_files(core_sources())

-- Async runtime library (sluice-CORE-017+). OPT-IN, namespace sluice::async.
-- Built alongside the core but kept a separate static lib so the blocking
-- default (sluice_core) carries no async surface. ADR §A6: async is opt-in and
-- BlockingIoContext/Reader/Writer are untouched.
target("sluice_async")
    set_kind("static")
    set_default(false)
    set_group("async")
    add_includedirs(R .. "include", {public = true})
    add_deps("sluice_core")
    add_files(R .. "src/async/*.cpp")
    -- CPP-STATIC-1: Clang TSA gate.
    -- ASYNC-GCC-TSA-FLAG-ROUTING-CORRECTIVE-1 (W3): the flags are scoped to
    -- the Clang frontends via the `tools` option. {force=true} previously
    -- bypassed xmake's per-compiler flag filtering, which caused GCC to
    -- receive the Clang-only -Wthread-safety and fail. Dropping force and
    -- using {tools={"clang","clang_cl"}} scopes the flags to BOTH Clang
    -- frontends (the Linux/Mac clang driver AND the Windows clang-cl driver),
    -- so Windows/clang-cl builds keep TSA coverage; GCC never receives them.
    -- Verified against the official xmake docs (add_cxxflags {tools=...}).
    add_cxxflags("-Wthread-safety", "-Werror=thread-safety",
                 {tools = {"clang", "clang_cl"}})
