-- xmake build for the sluice C++ core (Zig std.Io inspired).
-- Zig source under ./zig is design reference only; never built here.
add_rules("mode.debug", "mode.release", "mode.valgrind")
add_rules("plugin.compile_commands.autoupdate")

-- Sanitizer modes: use set_policy instead of deprecated built-in modes.
if is_mode("asan") then
    set_policy("build.sanitizer.address", true)
end
if is_mode("tsan") then
    set_policy("build.sanitizer.thread", true)
end
if is_mode("ubsan") then
    set_policy("build.sanitizer.undefined", true)
end
if is_mode("asanubsan") then
    set_policy("build.sanitizer.address", true)
    set_policy("build.sanitizer.undefined", true)
end

set_languages("c++20")
set_warnings("all", "error")

-- Root anchor so sub-file paths resolve from the project root, not the file.
SLUICE_ROOT = os.projectdir() .. "/"

option("hardened")
    set_default(false)
    set_description("Enable supported release hardening flags (use with -m release).")
option_end()

rule("sluice.hardened.release")
    on_config(function ()
        if has_config("hardened") and not is_mode("release") then
            raise("--hardened requires -m release")
        end
    end)
rule_end()
add_rules("sluice.hardened.release")

-- Hardening is opt-in because sluice_core/sluice_async are static libraries:
-- the final application remains responsible for its own linker policy.
if has_config("hardened") then
    if is_plat("linux", "macosx") then
        add_cxxflags("-fstack-protector-strong", "-D_FORTIFY_SOURCE=2", "-fPIC",
                     {tools = {"gcc", "clang"}})
    end
    if is_plat("linux") then
        add_ldflags("-Wl,-z,relro", "-Wl,-z,now", "-pie")
    end
end

-- ---------------------------------------------------------------------------
-- Sub-configurations. Each file owns one concern:
--   helpers.lua        — shared target-declaration helpers
--   libraries.lua      — production libs (core, async, bench_common)
--   experimental.lua   — io_uring spike + liburing gate
--   tests/core.lua     — core-only correctness tests
--   tests/async.lua    — async tests linking production sluice_async
--   tests/async_internal.lua — tests linking sluice_async_internal_testing
--   tests/death.lua    — POSIX death tests (fork/exec + fail-fast)
--   examples.lua       — buildable examples
--   benchmarks.lua     — microbench targets
-- ---------------------------------------------------------------------------

includes("xmake/helpers.lua")
includes("xmake/libraries.lua")
includes("xmake/experimental.lua")
includes("xmake/tests/core.lua")
includes("xmake/tests/async.lua")
includes("xmake/tests/async_internal.lua")
includes("xmake/tests/death.lua")
includes("xmake/examples.lua")
includes("xmake/benchmarks.lua")
includes("xmake/fuzz.lua")

-- ---------------------------------------------------------------------------
-- Quick reference — sanitizer / test commands:
--
--   Default build:       xmake build
--   Debug build:         xmake f -m debug && xmake build
--   Release build:       xmake f -m release && xmake build
--
--   Run all tests:       xmake run -g test
--   Run one test:        xmake run <test_name>_test
--
--   ASan:                xmake f -m asan && xmake build -g test && xmake run -g test
--   TSan:                xmake f -m tsan && xmake build -g test && xmake run -g test
--   UBSan:               xmake f -m ubsan && xmake build -g test && xmake run -g test
--   ASan+UBSan:          xmake f -m asanubsan && xmake build -g test && xmake run -g test
--   Valgrind:            xmake f -m valgrind && xmake build -g test
--                         valgrind --leak-check=full <binary>
--
--   Switch to Clang:     xmake f --toolchain=clang -c && xmake build
--   Clang + ASan:        xmake f --toolchain=clang -m asan -c && xmake build
--
--   Run all examples:    xmake run -g examples
--   Run all benches:     xmake run -g bench
-- ---------------------------------------------------------------------------
