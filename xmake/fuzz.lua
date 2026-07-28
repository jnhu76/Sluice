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
--   - Clang-only: libFuzzer is a Clang feature. The WHOLE FILE is gated on the
--     configured toolchain below, so under a non-Clang compiler (gcc, msvc...)
--     not a single fuzz target is declared. That keeps `xmake f --toolchain=gcc`
--     clean: no fuzz on_config runs, no spurious errors, and the GCC production
--     / test gate works. (We cannot use a per-target on_config guard alone,
--     because xmake runs on_config for every declared target during `xmake f`,
--     including set_default(false) ones — is_enabled() is independent of
--     set_default().)
--
-- Toolchain detection note: there is NO global is_toolchain() builtin in xmake.
-- The configured toolchain is exposed as the "toolchain" config key (set by
-- `xmake f --toolchain=...`), so we read it with is_config(). xmake evaluates
-- the description scope more than once during `xmake f`; the early passes see a
-- nil toolchain (targets simply not declared that pass) and the final pass sees
-- the real value, which is what governs whether the targets exist.
--
-- Usage:
--   xmake f -m debug --toolchain=clang -y
--   xmake build -g fuzz            # builds all three fuzz targets
--   xmake run wal_read_record_fuzz -- -runs=0   # smoke-check one target
local R = SLUICE_ROOT

-- Declare the fuzz targets ONLY under a Clang toolchain. Accept both the
-- Linux/macOS "clang" driver and the Windows "clang-cl" driver. When the
-- toolchain is anything else (gcc, msvc, env/...) or unset, this entire file
-- declares nothing, so `xmake f --toolchain=gcc` succeeds and the GCC
-- production/test builds are unaffected.
if not is_config("toolchain", "clang", "clang-cl") then
    return
end

-- ---------------------------------------------------------------------------
-- Defense-in-depth Clang guard. The file-level is_config() gate above is the
-- primary mechanism: under a non-Clang toolchain the fuzz targets are not
-- declared at all. This per-target on_config is a SECONDARY check that still
-- pays off if a fuzz target is later given its own per-target toolchain via
-- set_toolchains("clang") (which overrides the global --toolchain=...). In that
-- case the file gate could pass under a non-clang global toolchain while the
-- target itself compiles with something else; this guard catches that.
--
-- It uses the target-scoped API (target:toolchains()) rather than the global
-- config, because it must reflect the toolchain ACTUALLY applied to THIS target
-- (including per-target set_toolchains), not just the global default.
-- ---------------------------------------------------------------------------
local function clang_only_fuzz_guard(target)
    on_config(function (t)
        -- Resolve the standalone (C/C++) toolchain for this target. This sees
        -- both explicitly-added toolchains and the platform default.
        local cc = ""
        for _, tc in ipairs(t:toolchains()) do
            if tc:is_standalone() then
                cc = tc:name()
                break
            end
        end
        -- Accept both the Linux/Mac "clang" driver and the Windows "clang-cl"
        -- driver. Reject everything else (gcc, msvc, ...).
        if cc ~= "clang" and cc ~= "clang-cl" then
            raise("fuzz target '" .. t:name() ..
                  "' requires Clang (libFuzzer is Clang-only). " ..
                  "Current C++ toolchain is '" .. cc ..
                  "'. Reconfigure with --toolchain=clang, or build only the " ..
                  "non-fuzz targets with the current compiler.")
        end
    end)
end

-- ---------------------------------------------------------------------------
-- Instrumented production core. Same sources as sluice_core, compiled with
-- fuzzer-no-link (coverage instrumentation without libFuzzer's main) plus
-- ASan/UBSan, so every production TU is coverage-instrumented. -fno-omit-
-- frame-pointer keeps stack traces readable for libFuzzer crash reports.
--
-- The clang_only_fuzz_guard (above) is the authoritative compiler check: it
-- fails the configuration loudly when a non-Clang compiler is used. The
-- sanitizer flags are therefore applied unconditionally here; the guard ensures
-- they only ever reach a Clang that supports them. (Link flags must be forced
-- because xmake's auto-ignore would otherwise drop -fsanitize=fuzzer and the
-- fuzz executables would have no main from libFuzzer.)
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
    clang_only_fuzz_guard(target("sluice_core_fuzz"))

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
        clang_only_fuzz_guard(target(name))
end

fuzz_target("wal_read_record_fuzz")
fuzz_target("wal_roundtrip_fuzz")
fuzz_target("copy_all_fault_fuzz")
