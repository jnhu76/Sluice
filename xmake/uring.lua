-- The liburing build switch: configuration contract, build-time capability
-- detection, and the configuration-identity define (#520).
--
-- Contract (unchanged): --liburing=n keeps an honest stub with no
-- SLUICE_HAS_LIBURING anywhere; --liburing=y requires a real liburing and
-- never falls back silently. Real kernel capabilities (ring creation,
-- required opcodes, statx) stay owned by the UringAsyncBackend construction
-- path; configure only validates the header/library surface the build needs.
--
-- Configuration identity: xmake's incremental check compares the stored
-- per-object compile flags index by index, so a define appended at the tail
-- by a config switch (the conditional SLUICE_HAS_LIBURING) is invisible and a
-- --liburing switch in one build directory reused the previous profile's
-- uring objects. SLUICE_LIBURING_PROFILE is therefore defined unconditionally
-- and always occupies the same flag position with a different value per
-- profile, making a switch change an existing index; the affected target and
-- every consumer then recompile. It carries no public layout statement;
-- SLUICE_HAS_LIBURING remains the only public usage requirement and is still
-- defined only in the enabled branch.
--
-- Dependency identity: what the capability probe compiles and links must be
-- what the production targets compile and link. The authority is the active
-- toolchain's own resolution — the header its `-E -H` trace opens for
-- <liburing.h> and the file its `-Wl,--trace` link opens for -luring — not a
-- hand-written candidate list (env paths rank ahead of -I for gcc/clang and
-- C_INCLUDE_PATH is invisible to C++ compiles, so a scanned candidate can be
-- a file the toolchain never picks; the driver also checks configured
-- directories before LIBRARY_PATH). The probe re-traces with the resolved
-- directories pinned to prove the pin resolves to the same files, checks the
-- required surface against exactly that pair, and binds the directories as
-- public usage requirements only when plain resolution is
-- environment-dependent, so consumers resolve the same files in other
-- environments.

option("liburing")
    set_default(false)
    set_description("Build the real io_uring backend (SLUICE_HAS_LIBURING + link liburing). Requires liburing dev + runtime; without it the backend is an honest stub.")
option_end()

-- The exact io_uring surface the production uring TUs reference. Everything
-- here must at least compile as a declaration; the external symbols among
-- them must additionally link against -luring.
local uring_required_functions = {
    "io_uring_queue_init",
    "io_uring_queue_exit",
    "io_uring_get_sqe",
    "io_uring_get_probe_ring",
    "io_uring_free_probe",
    "io_uring_opcode_supported",
    "io_uring_submit",
    "io_uring_submit_and_wait",
    "io_uring_get_events",
    "io_uring_register_eventfd",
    "io_uring_unregister_eventfd",
    "io_uring_peek_cqe",
    "io_uring_peek_batch_cqe",
    "io_uring_cqe_seen",
    "io_uring_cq_ready",
    "io_uring_cq_has_overflow",
    "io_uring_sq_ready",
    "io_uring_sqe_set_data64",
    "io_uring_cqe_get_data64",
    "io_uring_prep_read",
    "io_uring_prep_write",
    "io_uring_prep_fsync",
    "io_uring_prep_statx",
    "io_uring_prep_cancel64"}

local uring_required_constants = {
    "IORING_OP_READ",
    "IORING_OP_WRITE",
    "IORING_OP_FSYNC",
    "IORING_OP_STATX",
    "IORING_OP_ASYNC_CANCEL",
    "AT_FDCWD",
    "AT_EMPTY_PATH",
    "AT_SYMLINK_NOFOLLOW",
    "STATX_TYPE",
    "STATX_MODE",
    "STATX_SIZE",
    "STATX_INO"}

function sluice_liburing_profile_define()
    return "SLUICE_LIBURING_PROFILE=" .. (has_config("liburing") and "on" or "off")
end

local function _sluice_uring_probe_snippet()
    local lines = {
        "#include <liburing.h>",
        "using sluice_probe_fn = void (*)();",
        "static const sluice_probe_fn kRequiredSymbols[] = {"}
    for _, name in ipairs(uring_required_functions) do
        table.insert(lines, "    reinterpret_cast<sluice_probe_fn>(&" .. name .. "),")
    end
    table.insert(lines, "};")
    table.insert(lines, "static const long kRequiredConstants[] = {")
    for _, name in ipairs(uring_required_constants) do
        table.insert(lines, "    (long)" .. name .. ",")
    end
    table.insert(lines, "};")
    table.insert(lines, "int main() {")
    table.insert(lines, "    return kRequiredSymbols[0] == nullptr ||")
    table.insert(lines, "           kRequiredConstants[0] == -1 ? 1 : 0;")
    table.insert(lines, "}")
    return table.concat(lines, "\n")
end

-- Tiny TU for the -luring resolution trace. Resolution must stay independent
-- of capability: an empty main needs no symbols, so the trace only reports
-- which liburing file the linker opens, never whether an ABI matches. It is
-- only compiled and linked, never executed, so no kernel ring is involved.
local function _sluice_uring_link_snippet()
    return {"int main() { return 0; }"}
end

option("__sluice_liburing_capability")
    set_showmenu(false)
    set_description("Internal: validates the liburing build-time capability when --liburing=y; never set it manually.")
    on_check(function (option)
        if not has_config("liburing") then
            option:enable(true)
            return
        end
        import("core.tool.compiler")
        import("core.project.config")

        local compinst = compiler.load("cxx")
        if compinst == nil then
            raise("sluice --liburing=y: no C++ compiler is configured for the "
                .. "active toolchain")
        end
        local program = compinst:program()

        -- Everything below runs here (not at file scope): the trace helpers
        -- need os.tmpfile/os.iorunv, which only the on_check sandbox exposes.
        local function _write_snippet_file(lines)
            local file = os.tmpfile()
            local handle = io.open(file, "w")
            handle:write(table.concat(lines, "\n") .. "\n")
            handle:close()
            return file
        end

        local function _no_dependency_env()
            return {CPATH = "", CPLUS_INCLUDE_PATH = "", C_INCLUDE_PATH = "",
                    LIBRARY_PATH = ""}
        end

        local function _run_capture(argv, envs)
            local captured = {ok = true, output = "", errors = ""}
            local opt = {}
            if envs ~= nil then
                opt.envs = envs
            end
            try {
                function ()
                    local output, errors = os.iorunv(program, argv, opt)
                    captured.output = tostring(output or "")
                    captured.errors = tostring(errors or "")
                end,
                catch {function (errors)
                    captured.ok = false
                    captured.errors = tostring(errors)
                end}}
            return captured
        end

        local function _trace_header(include_dir, envs)
            local argv = {"-E", "-H", "-x", "c++"}
            if include_dir ~= nil then
                table.insert(argv, "-I")
                table.insert(argv, include_dir)
            end
            local snippet = _write_snippet_file({"#include <liburing.h>",
                                                 "int sluice_uring_identity;"})
            table.insert(argv, snippet)
            local captured = _run_capture(argv, envs)
            os.tryrm(snippet)
            local resolved = (captured.output .. "\n" .. captured.errors)
                :match("%.%s+(%S*liburing%.h)")
            return captured, resolved
        end

        local function _trace_library(library_dir, envs)
            local argv = {"-x", "c++"}
            if library_dir ~= nil then
                table.insert(argv, "-L")
                table.insert(argv, library_dir)
            end
            local snippet = _write_snippet_file(_sluice_uring_link_snippet())
            table.insert(argv, snippet)
            local binary = os.tmpfile()
            argv = table.join(argv, {"-luring", "-Wl,--trace", "-o", binary})
            local captured = _run_capture(argv, envs)
            os.tryrm(snippet)
            os.tryrm(binary)
            local resolved = (captured.output .. "\n" .. captured.errors)
                :match("(%S*liburing%.so[%w%.]*)")
            return captured, resolved
        end

        local function _resolve_identity()
            local header_captured, header_file = _trace_header(nil, nil)
            if header_file == nil then
                raise("sluice --liburing=y: could not resolve <liburing.h> in "
                    .. "the active C++ include resolution (" .. tostring(program)
                    .. "): " .. header_captured.errors)
            end
            local library_captured, library_file = _trace_library(nil, nil)
            if library_file == nil then
                raise("sluice --liburing=y: no linker resolution for -luring in "
                    .. "the active library search (" .. tostring(program) .. "): "
                    .. library_captured.errors)
            end
            return header_file, library_file
        end

        -- The probe pins the resolved directories; the pin must resolve to
        -- exactly the files the unpinned production resolution picks.
        local function _verify_pinned_identity(header_file, library_file)
            local _, header_pinned = _trace_header(path.directory(header_file), nil)
            if header_pinned ~= header_file then
                raise("sluice --liburing=y: pinning "
                    .. path.directory(header_file) .. " changes the <liburing.h> "
                    .. "resolution to " .. tostring(header_pinned) .. " instead of "
                    .. header_file)
            end
            local _, library_pinned = _trace_library(path.directory(library_file), nil)
            if library_pinned ~= library_file then
                raise("sluice --liburing=y: pinning "
                    .. path.directory(library_file) .. " changes the -luring "
                    .. "resolution to " .. tostring(library_pinned) .. " instead of "
                    .. library_file)
            end
        end

        local function _check_capability(snippet, header_dir, library_dir)
            import("lib.detect.check_cxxsnippets")

            -- Declaration layer: the header and the exact compile-time surface
            -- the uring TUs use must compile. Library layer: the required
            -- external symbols must link. Compiles and links only; never
            -- executes a binary, so no kernel ring is required at configure
            -- time. (configs is the only place check_cxsnippets reads links
            -- from, so links passed outside configs would silently skip the
            -- link step.)
            local configs = {includedirs = header_dir}
            local decl_ok, decl_error = check_cxxsnippets({liburing = snippet},
                                                          {configs = configs})
            if not decl_ok then
                raise("sluice --liburing=y: the resolved liburing does not provide "
                    .. "the header/declaration surface the uring backend compiles "
                    .. "against: " .. tostring(decl_error))
            end
            local link_configs = {includedirs = header_dir,
                                  linkdirs = library_dir,
                                  links = "uring"}
            local link_ok, link_error = check_cxxsnippets({liburing = snippet},
                                                          {configs = link_configs})
            if not link_ok then
                raise("sluice --liburing=y: the resolved liburing library does not "
                    .. "provide the required external symbols: "
                    .. tostring(link_error))
            end
        end

        local header_file, library_file = _resolve_identity()
        _verify_pinned_identity(header_file, library_file)
        _check_capability(_sluice_uring_probe_snippet(),
                          path.directory(header_file),
                          path.directory(library_file))

        -- Consumer binding: bind the resolved directories publicly only when
        -- the plain resolution depends on this environment's variables; an
        -- environment-independent pair is what every consumer resolves
        -- anyway, and pinning /usr/include ahead of the C++ standard headers
        -- would only reorder the default search.
        local _, header_cleared = _trace_header(nil, _no_dependency_env())
        if header_cleared ~= header_file then
            config.set("__sluice_liburing_header_dir", path.directory(header_file),
                       {force = true})
            config.set("__sluice_liburing_bind_includes", true, {force = true})
        else
            config.set("__sluice_liburing_bind_includes", false, {force = true})
        end
        local _, library_cleared = _trace_library(nil, _no_dependency_env())
        if library_cleared ~= library_file then
            config.set("__sluice_liburing_library_dir", path.directory(library_file),
                       {force = true})
            config.set("__sluice_liburing_bind_linkdirs", true, {force = true})
        else
            config.set("__sluice_liburing_bind_linkdirs", false, {force = true})
        end
        config.save()

        print("checking for liburing capability ... ok (%s + %s)",
              header_file, library_file)
        option:enable(true)
    end)
option_end()
