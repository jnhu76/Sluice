-- Experimental io_uring targets + optional liburing build gate.

local R = SLUICE_ROOT

-- ---------------------------------------------------------------------------
-- SLUICE-CORE-013B: optional liburing build gate for the experimental spike.
--
-- Normal builds have NO liburing dependency. Pass `--with-liburing=true` (and
-- have liburing available via xrepo/system) to enable the experimental uring
-- targets and define CPPIO_HAS_LIBURING. Without it, the experimental sources
-- compile as unsupported-stubs so the rest of the project is unaffected.
-- ---------------------------------------------------------------------------
option("with-liburing")
    set_default(false)
    set_description("Enable the experimental io_uring spike (requires liburing).")
option_end()

-- SLUICE-CORE-026 (B3): feature gates for io_uring registered buffers/files.
-- Both OFF by default — matching Zig upstream (Io/Uring.zig uses neither
-- registered buffers nor registered files). A future job may implement them
-- under a documented lifetime contract; until then the gates exist so the
-- build can advertise the capability without the implementation. The defines
-- SLUICE_URING_REGISTERED_BUFFERS / SLUICE_URING_REGISTERED_FILES are threaded
-- onto sluice_async only when liburing is also enabled (they are meaningless
-- without a real ring).
option("with-uring-registered-buffers")
    set_default(false)
    set_description("Enable io_uring registered buffers (lifetime contract WIP; off by default).")
option_end()
option("with-uring-registered-files")
    set_default(false)
    set_description("Enable io_uring registered files descriptors (lifetime contract WIP; off by default).")
option_end()

local has_liburing = false
if has_config("with-liburing") then
    -- add_requires with optional=true lets xmake try to fetch liburing; if the
    -- user passed --with-liburing=true but it's unavailable, fail loudly here
    -- rather than silently building stubs.
    add_requires("liburing", {alias = "liburing"})
    has_liburing = true
end

-- When liburing is enabled, the async runtime's UringAsyncBackend compiles its
-- REAL io_uring path (otherwise it is an unsupported stub, sluice-CORE-020B).
-- Thread the same define + package onto sluice_async that sluice_experimental_uring
-- gets, so src/async/uring_backend.cpp sees SLUICE_HAS_LIBURING and links liburing.
if has_liburing then
    target("sluice_async")
        add_defines("SLUICE_HAS_LIBURING", {public = true})
        add_packages("liburing", {public = true})
        if has_config("with-uring-registered-buffers") then
            add_defines("SLUICE_URING_REGISTERED_BUFFERS")
        end
        if has_config("with-uring-registered-files") then
            add_defines("SLUICE_URING_REGISTERED_FILES")
        end
end

-- Dedicated real-liburing submit-failure state-machine tests. This target
-- compiles only the uring backend source with a private submit seam, so the
-- production sluice_async ABI and all other async tests remain hook-free.
if has_liburing and os.isfile(R .. "tests/uring_submit_failure_test.cpp") then
    target("uring_submit_failure_test")
        set_kind("binary")
        set_default(false)
        set_group("test")
        add_deps("sluice_core")
        add_includedirs(R .. "include")
        add_files(R .. "src/async/uring_backend.cpp",
                  R .. "tests/uring_submit_failure_test.cpp")
        add_defines("SLUICE_HAS_LIBURING",
                    "SLUICE_URING_INTERNAL_TESTING")
        add_packages("liburing")
        add_tests("uring_submit_failure_test")
end

-- Experimental uring library. Always defined so the headers/sources exist; the
-- implementation compiles either the real uring path or the unsupported stub
-- based on CPPIO_HAS_LIBURING.
target("sluice_experimental_uring")
    set_kind("static")
    set_default(false)
    set_group("experimental")
    add_includedirs(R .. "include", {public = true})
    add_deps("sluice_core")
    add_files(R .. "src/experimental/*.cpp")
    if has_liburing then
        add_defines("SLUICE_HAS_LIBURING", {public = true})
        add_packages("liburing", {public = true})
    end

-- uring_write_batch_test links the experimental uring lib (stub or real).
target("uring_write_batch_test")
    set_kind("binary")
    set_default(false)
    set_group("test")
    add_deps("sluice_core", "sluice_experimental_uring")
    add_includedirs(R .. "include", R .. "bench")
    add_files(R .. "tests/uring_write_batch_test.cpp")
    add_tests("uring_write_batch_test")

target("uring_io_context_test")
    set_kind("binary")
    set_default(false)
    set_group("test")
    add_deps("sluice_core", "sluice_experimental_uring")
    add_includedirs(R .. "include", R .. "bench")
    add_files(R .. "tests/uring_io_context_test.cpp")
    add_tests("uring_io_context_test")

target("uring_stats_test")
    set_kind("binary")
    set_default(false)
    set_group("test")
    add_deps("sluice_core", "sluice_experimental_uring")
    add_includedirs(R .. "include", R .. "bench")
    add_files(R .. "tests/uring_stats_test.cpp")
    add_tests("uring_stats_test")
