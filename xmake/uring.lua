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

local function _split_path_list(value)
    local dirs = {}
    for dir in (value or ""):gmatch("[^:]+") do
        if dir ~= "" then
            table.insert(dirs, dir)
        end
    end
    return dirs
end

-- Native gcc/clang resolution order for <liburing.h>: CPLUS_INCLUDE_PATH and
-- CPATH ahead of the system prefixes. The probe compile above remains the
-- authority on whether the header actually works.
local function _candidate_include_dirs()
    local dirs = {}
    for _, envname in ipairs({"CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "CPATH"}) do
        for _, dir in ipairs(_split_path_list(os.getenv(envname))) do
            table.insert(dirs, dir)
        end
    end
    table.insert(dirs, "/usr/local/include")
    table.insert(dirs, "/usr/include")
    return dirs
end

local function _candidate_library_dirs()
    local dirs = _split_path_list(os.getenv("LIBRARY_PATH"))
    for _, dir in ipairs({"/usr/local/lib", "/usr/local/lib64", "/usr/lib64",
                          "/usr/lib", "/lib64", "/lib"}) do
        table.insert(dirs, dir)
    end
    return dirs
end

local function _first_existing(dirs, names)
    for _, dir in ipairs(dirs) do
        for _, name in ipairs(names) do
            if os.isfile(path.join(dir, name)) then
                return path.join(dir, name)
            end
        end
    end
end

local function _first_path_component(file)
    local dir = path.directory(file)
    return (dir:gsub("^/", ""):match("^[^/]+"))
end

-- Reports the header/library pair behind the passing probes and refuses an
-- unintentionally mixed multi-prefix install. The probes are the authority
-- that the pair compiles and links; this is the evidence of where they came
-- from. Returns the refusal message instead of raising: this file scope has
-- no raise, the on_check sandbox does.
local function _report_liburing_paths()
    local header_path = _first_existing(_candidate_include_dirs(), {"liburing.h"})
    if header_path == nil then
        return "sluice --liburing=y: liburing.h was accepted by the capability probe "
            .. "but could not be located on the include search path; refusing an "
            .. "unverifiable install"
    end
    local lib_path = _first_existing(_candidate_library_dirs(),
                                     {"liburing.so", "liburing.so.2", "liburing.so.3"})
    if lib_path == nil then
        return "sluice --liburing=y: the liburing symbol probe linked, but no "
            .. "liburing.so/liburing.so.2 file could be located on the library "
            .. "search path; refusing an unverifiable install"
    end
    if _first_path_component(header_path) ~= _first_path_component(lib_path) then
        return "sluice --liburing=y: liburing header and library resolve to "
            .. "different installation prefixes (" .. header_path .. " vs "
            .. lib_path .. "); refusing an unintentionally mixed install"
    end
    print("checking for liburing capability ... ok (%s + %s)", header_path, lib_path)
end

option("__sluice_liburing_capability")
    set_showmenu(false)
    set_description("Internal: validates the liburing build-time capability when --liburing=y; never set it manually.")
    on_check(function (option)
        if not has_config("liburing") then
            option:enable(true)
            return
        end
        import("lib.detect.check_cxxsnippets")

        -- Declaration layer: the header and the exact compile-time surface the
        -- uring TUs use must compile. Library layer: the required external
        -- symbols must link. Compiles and links only; never executes a binary,
        -- so no kernel ring is required at configure time. The probes pin the
        -- resolved header/library directories explicitly (configs is the only
        -- place check_cxsnippets reads links and search paths from, so links
        -- passed outside configs would silently skip the link step).
        local header_path = _first_existing(_candidate_include_dirs(), {"liburing.h"})
        local library_path = _first_existing(_candidate_library_dirs(),
                                             {"liburing.so", "liburing.so.2",
                                              "liburing.so.3"})
        local configs = {}
        if header_path ~= nil then
            configs.includedirs = path.directory(header_path)
        end
        if library_path ~= nil then
            configs.linkdirs = path.directory(library_path)
        end

        local snippet = _sluice_uring_probe_snippet()
        local decl_ok, decl_error = check_cxxsnippets({liburing = snippet},
                                                      {configs = configs})
        if not decl_ok then
            raise("sluice --liburing=y: the installed liburing does not provide the "
                .. "header/declaration surface the uring backend compiles against: "
                .. tostring(decl_error))
        end
        local link_configs = table.clone(configs)
        link_configs.links = "uring"
        local link_ok, link_error = check_cxxsnippets({liburing = snippet},
                                                      {configs = link_configs})
        if not link_ok then
            raise("sluice --liburing=y: the installed liburing library does not "
                .. "provide the required external symbols: " .. tostring(link_error))
        end
        local refusal = _report_liburing_paths()
        if refusal ~= nil then
            raise(refusal)
        end
        option:enable(true)
    end)
option_end()
