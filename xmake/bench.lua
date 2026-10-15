-- Performance benchmark targets. Non-default: not registered as tests and not
-- built by `xmake` / `xmake test`; each depends on the shipped library targets,
-- so the measured artifact is the same archive the release build produces.
--
-- Build:  xmake build -g bench -y
-- Run:    G0_JSONL=<file> xmake run bench_w01_direct

local R = SLUICE_ROOT

target("bench_w01_direct")
    set_kind("binary")
    set_default(false)
    set_group("bench")
    add_deps("sluice_core")
    add_includedirs(R .. "bench")
    add_files(R .. "bench/bench_w01_direct.cpp")

target("bench_w02_pipeline")
    set_kind("binary")
    set_default(false)
    set_group("bench")
    add_deps("sluice_core", "sluice_async")
    add_includedirs(R .. "bench")
    add_files(R .. "bench/bench_w02_pipeline.cpp")

target("bench_w03_external_loop")
    set_kind("binary")
    set_default(false)
    set_group("bench")
    add_deps("sluice_core", "sluice_async")
    add_includedirs(R .. "bench")
    add_files(R .. "bench/bench_w03_external_loop.cpp")
