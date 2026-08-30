-- SE-2 research probes (tests/research/se2/).
--
-- Deliberate hazard-injection probes executed per-probe under the SE-2
-- evidence protocol (plain / ASan / TSan runs recorded in
-- docs/results/safety/se2-detection-matrix.json). They are intentionally NOT
-- registered with add_tests(): several probes commit memory-safety violations
-- BY DESIGN (borrow destroyed while a request is in flight, freed-buffer
-- consumption, deliberate data races as litmus controls) and MUST NOT run in
-- default CI test groups. Run them explicitly:
--
--   xmake f -m debug -y && xmake build -g research && xmake run -g research <probe>
--
-- Conventional-half probes are standalone C programs (raw POSIX AIO /
-- pthreads / liburing); they do not link any Sluice target.

local R = SLUICE_ROOT

-- Sluice-side research probes: link the PRODUCTION sluice_async (hook-free).
local function se2_sluice_probe(name)
    local path = R .. "tests/research/se2/" .. name .. ".cpp"
    if not os.isfile(path) then return end
    target(name)
        set_kind("binary")
        set_default(false)
        set_group("research")
        add_deps("sluice_core", "sluice_async")
        add_includedirs(R .. "include")
        add_files(path)
end

-- Conventional-half research probes: standalone C, optional system links.
local function se2_conventional_probe(name, links)
    local path = R .. "tests/research/se2/conventional/" .. name .. ".c"
    if not os.isfile(path) then return end
    target(name)
        set_kind("binary")
        set_default(false)
        set_group("research")
        add_includedirs(R .. "tests/research/se2/conventional")
        add_files(path)
        if links and #links > 0 then
            add_syslinks(unpack(links))
        end
end

se2_sluice_probe("se2_h01_borrow_destroy_probe")

se2_conventional_probe("conv_h01_aio_buffer_lifetime", {"rt"})
se2_conventional_probe("conv_h02_uring_user_data_reuse", {"uring", "pthread"})
se2_conventional_probe("conv_h03_uring_fd_reuse", {"uring", "pthread"})
se2_conventional_probe("conv_h06_lio_listio_partial", {"rt", "pthread"})
se2_conventional_probe("conv_h07_partial_write_retry", {"pthread"})
se2_conventional_probe("conv_h09_lost_wake", {"pthread"})
se2_conventional_probe("conv_h13_sb_litmus", {"pthread"})
