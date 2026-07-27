-- Fuzz targets: an instrumented sluice_core_fuzz static library plus three
-- libFuzzer executables. Opt-in only (set_default(false), group "fuzz"); never
-- built by default and never part of the normal test group.
--
-- Design goals:
--   - sluice_core_fuzz compiles the SAME authoritative production sources as
--     sluice_core (via the shared core_sources() manifest), so coverage
--     instrumentation reaches production code — not just the harness.
--   - Production sluice_core stays sanitizer-clean by default; instrumentation
--     is gated behind an explicit fuzz configuration.
--   - Clang-only: libFuzzer is a Clang feature. The flags below are Clang
--     sanitizer flags and are forced onto the target tooling.
--
-- Usage:
--   xmake f -m debug --toolchain=clang -y
--   xmake build -g fuzz            # builds all three fuzz targets
--   xmake run wal_read_record_fuzz -- -runs=0   # smoke-check one target
local R = SLUICE_ROOT

-- ---------------------------------------------------------------------------
-- Instrumented production core. Same sources as sluice_core, compiled with
-- fuzzer-no-link (coverage instrumentation without libFuzzer's main) plus
-- ASan/UBSan, so every production TU is coverage-instrumented. -fno-omit-
-- frame-pointer keeps stack traces readable for libFuzzer crash reports.
-- ---------------------------------------------------------------------------
target("sluice_core_fuzz")
    set_kind("static")
    set_default(false)
    set_group("fuzz")
    add_includedirs(R .. "include", {public = true})
    add_files(core_sources())
    add_cxxflags("-fsanitize=fuzzer-no-link,address,undefined", "-fno-omit-frame-pointer",
                 {force = true})
    add_ldflags("-fsanitize=fuzzer-no-link,address,undefined", {force = true})

-- ---------------------------------------------------------------------------
-- Shared declaration for the three fuzz executables. Each links only the
-- instrumented core, is not built by default, lives in group "fuzz", and
-- contains exactly one LLVMFuzzerTestOneInput entry point (so it uses the
-- real libFuzzer main). The support/ headers are added to the include path so
-- targets can write #include <fuzz/support/...>.
-- ---------------------------------------------------------------------------
local function fuzz_target(name)
    target(name)
        set_kind("binary")
        set_default(false)
        set_group("fuzz")
        -- Project root (so #include <fuzz/support/...> resolves) plus the
        -- public headers (so #include <sluice/...> resolves).
        add_includedirs(R .. "include", R)
        add_deps("sluice_core_fuzz")
        add_files(R .. "fuzz/" .. name .. ".cpp")
        add_cxxflags("-fsanitize=fuzzer,address,undefined", "-fno-omit-frame-pointer",
                     {force = true})
        add_ldflags("-fsanitize=fuzzer,address,undefined", {force = true})
end

fuzz_target("wal_read_record_fuzz")
fuzz_target("wal_roundtrip_fuzz")
fuzz_target("copy_all_fault_fuzz")
