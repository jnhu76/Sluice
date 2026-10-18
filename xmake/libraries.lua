-- Production libraries: sluice_core, sluice_async.

local R = SLUICE_ROOT

-- Core static library: File resource, shared semantics and direct execution.
core_sources = function()
    return { R .. "src/*.cpp" }
end

target("sluice_core")
    set_kind("static")
    add_includedirs(R .. "include", {public = true})
    add_files(core_sources())
    -- Installed header set: the exact v2 public core surface (root Appendix
    -- B). The allowlist is deliberate: no glob can install a future internal
    -- header by accident.
    add_headerfiles(R .. "include/(sluice/effect.hpp)",
                    R .. "include/(sluice/error.hpp)",
                    R .. "include/(sluice/file_resource.hpp)",
                    R .. "include/(sluice/measurement.hpp)",
                    R .. "include/(sluice/result.hpp)",
                    R .. "include/(sluice/blocking/file.hpp)")
    -- Reproducible archives: binutils built without
    -- --enable-deterministic-archives embeds each member's real mtime/uid/gid,
    -- so identical objects archive to different bytes on those toolchains and
    -- the frozen archive baselines cannot reproduce. GNU ar's -D modifier
    -- zeroes that metadata; {force=true} bypasses xmake's flag check, which
    -- does not know ar modifiers. Scoped to Linux: the Windows/clang-cl
    -- archiver (lib.exe-style) does not accept -D.
    if is_plat("linux") then
        add_arflags("-D", {force = true})
    end

-- Async runtime library. OPT-IN, namespace sluice::async: kept a separate
-- static lib so the blocking default (sluice_core) carries no async surface.
target("sluice_async")
    set_kind("static")
    set_default(false)
    set_group("async")
    add_includedirs(R .. "include", {public = true})
    add_deps("sluice_core")
    add_files(R .. "src/async/*.cpp")
    -- Installed header set: the exact v2 public async surface (root Appendix
    -- B) — backend selection headers carry configs and named factories only;
    -- the backend classes, the seam and all detail headers stay internal to
    -- the library TUs and the isolated seam tests via -I include.
    add_headerfiles(R .. "include/(sluice/async/async_io_context.hpp)",
                    R .. "include/(sluice/async/request.hpp)",
                    R .. "include/(sluice/async/request_scope.hpp)",
                    R .. "include/(sluice/async/threadpool_backend.hpp)",
                    R .. "include/(sluice/async/uring_backend.hpp)")
    -- The non-recursive glob above misses src/async/detail/. The context
    -- identity domain and the RequestCore substrate that the context owns are
    -- production now, so they are listed explicitly.
    add_files(R .. "src/async/detail/context_identity.cpp",
              R .. "src/async/detail/request_core.cpp")
    if is_plat("linux") then
        add_arflags("-D", {force = true})
    end
    -- SLUICE_HAS_LIBURING propagates the build profile's io_uring
    -- availability to xmake consumers from this single config point. The
    -- installed headers read no build-profile macro (the v2 package ODR
    -- probe verifies public layouts are macro-view independent), so the
    -- define carries availability, not a layout constraint. Consumers must
    -- not hand-copy it.
    add_defines(sluice_liburing_profile_define(), {public = true})
    if has_config("liburing") then
        add_defines("SLUICE_HAS_LIBURING", {public = true})
        -- The resolved dependency identity is written to the saved config by
        -- the capability check (xmake/uring.lua). Bind its directories
        -- publicly only when plain toolchain resolution is
        -- environment-dependent; otherwise every consumer resolves the same
        -- files without the extra -I/-L reorder.
        if get_config("__sluice_liburing_bind_includes") then
            add_includedirs(get_config("__sluice_liburing_header_dir"), {public = true})
        end
        if get_config("__sluice_liburing_bind_linkdirs") then
            add_linkdirs(get_config("__sluice_liburing_library_dir"), {public = true})
        end
        add_links("uring", {public = true})
    end
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
