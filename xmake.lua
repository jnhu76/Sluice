-- xmake build for the cppio C++ core (Zig std.Io inspired).
-- Zig source under ./zig is design reference only; never built here.
add_rules("mode.debug", "mode.release")

set_languages("c++20")
set_warnings("all", "error")

-- Core static library: Reader/Writer abstractions + wrappers.
target("cppio_core")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/*.cpp")

-- Declare a test/example target only when its source file exists, so xmake
-- does not warn about missing files for slices not yet written.
function cppio_one_file_target(kind, group, name, subdir, deps_list)
    local path = subdir .. "/" .. name .. ".cpp"
    if not os.isfile(path) then return end
    target(name)
        set_kind(kind)
        set_default(false)
        set_group(group)
        if deps_list then add_deps(deps_list) end
        add_includedirs("include")
        add_files(path)
        if group == "test" then add_tests(name) end
end

-- Correctness tests, one binary per slice. Built/run via `xmake -g test`.
local tests = {
    "result", "writer", "reader", "fault", "buffer",
    "observed", "copy", "wal", "file", "posix_retry",
    "wrapper_noncopyable", "limit",
}
for _, t in ipairs(tests) do
    cppio_one_file_target("binary", "test", t .. "_test", "tests", "cppio_core")
end

-- Buildable examples. Built/run via `xmake -g examples`.
local examples = { "cat", "copy_file", "small_writes", "fault_write", "wal_records" }
for _, e in ipairs(examples) do
    cppio_one_file_target("binary", "examples", e, "examples", "cppio_core")
end
