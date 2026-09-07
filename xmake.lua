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
-- ---------------------------------------------------------------------------

includes("xmake/helpers.lua")
includes("xmake/libraries.lua")
includes("xmake/apps.lua")
