local R = SLUICE_ROOT

-- The io_uring backend class is split across uring_backend.cpp (profile
-- setup, admission, publication, public surface), uring_transport.cpp (SQ
-- dispatch, transport ledger, poison recovery, cancel controls) and
-- uring_completion.cpp (CQ reap and terminal handoff). Every seam build that
-- compiles the backend must compile the three together.
local uring_backend_sources = {
    R .. "src/async/uring_backend.cpp",
    R .. "src/async/uring_transport.cpp",
    R .. "src/async/uring_completion.cpp",
}

sluice_one_file_target("binary", "test", "file_read_test", "tests", {"sluice_core", "sluice_async"})
sluice_one_file_target("binary", "test", "file_resource_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "file_open_contract_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "file_write_test", "tests", {"sluice_core", "sluice_async"})
sluice_one_file_target("binary", "test", "file_sync_data_test", "tests", {"sluice_core", "sluice_async"})
sluice_one_file_target("binary", "test", "file_sync_all_test", "tests", {"sluice_core", "sluice_async"})
sluice_one_file_target("binary", "test", "explicit_file_ref_test", "tests", {"sluice_core", "sluice_async"})
sluice_one_file_target("binary", "test", "async_sync_admission_test", "tests", {"sluice_core", "sluice_async"})
sluice_one_file_target("binary", "test", "blocking_file_read_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "blocking_file_write_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "blocking_file_sync_data_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "blocking_file_state_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "blocking_file_sequential_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "blocking_file_sync_all_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "uring_submit_boundary_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "file_access_precedence_test", "tests",
                       {"sluice_core", "sluice_async"})

-- A1 shared File semantic oracle: decision tables, property tests and
-- reference cases. All of them compare execution paths against the one shared
-- oracle; none carries a backend-specific expected-output file.
sluice_one_file_target("binary", "test", "semantic_errno_mapping_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "semantic_open_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "semantic_range_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "semantic_short_io_reference_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "semantic_effect_outcome_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "semantic_durability_reference_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "semantic_reference_case_test", "tests",
                       {"sluice_core", "sluice_async"})
sluice_one_file_target("binary", "test", "semantic_validation_precedence_test", "tests",
                       {"sluice_core", "sluice_async"})

-- A2 direct closure: canonical File close/RAII semantics, minimal metadata and
-- identity, the direct cursor/positional distinction, and the exact/all
-- composition surfaces. `direct_w01_consumer_probe` is the core-only consumer:
-- it links `sluice_core` alone, so it proves the W-01 trace needs no async
-- runtime and no liburing.
sluice_one_file_target("binary", "test", "file_info_identity_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "direct_cursor_position_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "direct_composition_test", "tests", "sluice_core")
sluice_one_file_target("binary", "test", "direct_w01_consumer_probe", "tests", "sluice_core")

-- Fault-injection targets: the canonical direct TUs compiled with the test-only
-- native-call seam (SLUICE_FILE_INTERNAL_TESTING). Each target compiles its own
-- copy of those TUs and links no other core object, so the seam build and the
-- production build never meet in one binary.
do
    local function direct_seam_target(name, test_source)
        target(name)
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_includedirs(R .. "include", R .. "src")
            add_defines("SLUICE_FILE_INTERNAL_TESTING")
            add_files(test_source, R .. "src/file_resource.cpp", R .. "src/blocking_file.cpp")
            add_tests(name)
    end

    direct_seam_target("file_close_semantics_test", R .. "tests/file_close_semantics_test.cpp")
    direct_seam_target("direct_composition_fault_test",
                       R .. "tests/direct_composition_fault_test.cpp")
end

-- Real io_uring verification: registered only when the liburing build switch
-- is on. Both targets consume sluice_async as ordinary consumers: the macro
-- and the liburing link arrive through sluice_async's public usage
-- requirement and are never hand-copied here. The probe witnesses the
-- consumer-side definition contract; the smoke drives real submissions.
if has_config("liburing") then
    -- The same precedence scenarios and the same oracle, driven through the real
    -- io_uring backend. Registered only when the real backend is built.
    target("semantic_backend_conformance_test")
        set_kind("binary")
        set_default(false)
        set_group("test")
        add_deps("sluice_core", "sluice_async")
        add_includedirs(R .. "include")
        add_files(R .. "tests/semantic_backend_conformance_test.cpp")
        add_tests("semantic_backend_conformance_test")

    target("uring_public_consumer_probe")
        set_kind("binary")
        set_default(false)
        set_group("test")
        add_deps("sluice_core", "sluice_async")
        add_includedirs(R .. "include")
        add_files(R .. "tests/uring_public_consumer_probe.cpp")
        add_tests("uring_public_consumer_probe")

    target("uring_backend_smoke_test")
        set_kind("binary")
        set_default(false)
        set_group("test")
        add_deps("sluice_core", "sluice_async")
        add_includedirs(R .. "include")
        add_files(R .. "tests/uring_backend_smoke_test.cpp")
        add_tests("uring_backend_smoke_test")
end

-- A2 app-consumption tests: exercise the app engines through the canonical
-- File resource they now consume. Each target compiles the app's engine
-- modules with public headers only, mirroring the app target's dependency
-- shape (no test seams, no src/ include).
do
    local dir = R .. "apps/sluice-hash"
    if os.isfile(dir .. "/hash_task.cpp") then
        target("app_hash_consumption_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", dir)
            add_files(R .. "tests/app_hash_consumption_test.cpp", dir .. "/hash_task.cpp",
                      dir .. "/sha256.cpp")
            add_tests("app_hash_consumption_test")
    end
end

do
    local dir = R .. "apps/sluice-grep"
    if os.isfile(dir .. "/grep_task.cpp") then
        target("app_grep_consumption_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", dir)
            add_files(R .. "tests/app_grep_consumption_test.cpp", dir .. "/grep_task.cpp",
                      dir .. "/matcher.cpp")
            add_tests("app_grep_consumption_test")
    end
end

do
    local dir = R .. "apps/sluice-tail"
    if os.isfile(dir .. "/tail_task.cpp") then
        target("app_tail_consumption_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", dir)
            add_files(R .. "tests/app_tail_consumption_test.cpp", dir .. "/tail_task.cpp")
            add_tests("app_tail_consumption_test")
    end
end

do
    local dir = R .. "apps/sluice-copy"
    if os.isfile(dir .. "/copy_task.cpp") then
        target("app_copy_consumption_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", dir)
            add_files(R .. "tests/app_copy_consumption_test.cpp", dir .. "/copy_task.cpp",
                      dir .. "/file_domain.cpp", dir .. "/safe_output.cpp")
            add_tests("app_copy_consumption_test")
    end
end

-- RequestCore substrate tests: each target compiles the substrate TU directly
-- and links only sluice_core, so the protocol suite never links the async
-- runtime, backends or liburing. The protocol/publication targets enable the
-- internal-testing observation seam; the consumer probe consumes the public
-- surface only, proving the substrate stands alone without Scheduler, Fiber,
-- waiter/routing vocabulary or a production backend.
do
    local function request_core_target(name, test_source, with_seam, extra_define)
        target(name)
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include")
            if with_seam then
                add_defines("SLUICE_ASYNC_INTERNAL_TESTING")
            end
            if extra_define ~= nil then
                add_defines(extra_define)
            end
            add_files(test_source, R .. "src/async/detail/request_core.cpp")
            if extra_define == nil then
                add_tests(name)
            end
    end

    request_core_target("request_core_protocol_test",
                        R .. "tests/request_core_protocol_test.cpp", true)
    request_core_target("request_core_publication_test",
                        R .. "tests/request_core_publication_test.cpp", true)
    request_core_target("request_core_consumer_probe",
                        R .. "tests/request_core_consumer_probe.cpp", false)

    -- C1-B (#428) named mutation builds. Each flips one load-bearing arm of
    -- the attach/publication resolution and must be killed by the named
    -- failing assertion of the killer suite; they are executed and recorded
    -- manually, never run as regular tests.
    request_core_target("request_core_mut_attach_ignores_publication",
                        R .. "tests/request_core_protocol_test.cpp", true,
                        "SLUICE_C1_MUTANT_ATTACH_IGNORES_PUBLICATION")
    request_core_target("request_core_mut_attach_treats_terminal_as_published",
                        R .. "tests/request_core_protocol_test.cpp", true,
                        "SLUICE_C1_MUTANT_ATTACH_TREATS_TERMINAL_AS_PUBLISHED")

    -- C1-C (#429) named mutation builds over the delivery state machine.
    request_core_target("request_core_mut_delivery_claim_unbounded",
                        R .. "tests/request_core_protocol_test.cpp", true,
                        "SLUICE_C1_MUTANT_DELIVERY_CLAIM_UNBOUNDED")
    request_core_target("request_core_mut_cancel_during_delivery_returns",
                        R .. "tests/request_core_protocol_test.cpp", true,
                        "SLUICE_C1_MUTANT_CANCEL_DURING_DELIVERY_RETURNS")
    request_core_target("request_core_mut_publication_skips_queue",
                        R .. "tests/request_core_protocol_test.cpp", true,
                        "SLUICE_C1_MUTANT_PUBLICATION_SKIPS_QUEUE")
end

-- B1-A context ownership/identity evidence. The observed surface (the
-- context-owned core, the slot-table identity, the adoption seam) is
-- macro-guarded internal-testing surface, so this target compiles its own
-- copies of the async TUs it observes and links neither sluice_async nor the
-- other seam builds: the seam build and the production build never meet in one
-- binary. `src/async` is on the include path because `src/async/*.cpp` includes
-- its internal headers relative to their own directory.
do
    local function context_ownership_target(name, with_liburing)
        target(name)
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include", R .. "src/async")
            add_defines("SLUICE_ASYNC_INTERNAL_TESTING")
            local files = {
                R .. "tests/request_core_ownership_test.cpp",
                R .. "src/async/async_io_context.cpp",
                R .. "src/async/threadpool_backend.cpp",
                R .. "src/async/request_handle.cpp",
                R .. "src/async/fail_fast.cpp",
                R .. "src/async/detail/context_identity.cpp",
                R .. "src/async/detail/request_core.cpp",
            }
            if with_liburing then
                add_defines("SLUICE_HAS_LIBURING")
                add_links("uring")
                for _, tu in ipairs(uring_backend_sources) do
                    table.insert(files, tu)
                end
            end
            add_files(files)
            add_tests(name)
    end

    context_ownership_target("request_core_ownership_test", false)
    if has_config("liburing") then
        context_ownership_target("request_core_ownership_uring_test", true)
    end
end

-- C2-A (#397) ProgressSource ownership/lifetime/route evidence. Same
-- self-contained seam-build shape as the ownership target: the target compiles
-- its own copies of the async TUs it observes and links neither sluice_async
-- nor the other seam builds. The uring variant runs the same tests against the
-- real-ring backend when liburing is available.
do
    local function progress_source_target(name, with_liburing, extra_define)
        target(name)
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include", R .. "src/async")
            add_defines("SLUICE_ASYNC_INTERNAL_TESTING")
            if extra_define ~= nil then
                add_defines(extra_define)
            end
            local files = {
                R .. "tests/progress_source_ownership_test.cpp",
                R .. "src/async/async_io_context.cpp",
                R .. "src/async/threadpool_backend.cpp",
                R .. "src/async/request_handle.cpp",
                R .. "src/async/fail_fast.cpp",
                R .. "src/async/detail/context_identity.cpp",
                R .. "src/async/detail/request_core.cpp",
            }
            if with_liburing then
                add_defines("SLUICE_HAS_LIBURING")
                add_links("uring")
                for _, tu in ipairs(uring_backend_sources) do
                    table.insert(files, tu)
                end
            end
            add_files(files)
            if extra_define == nil then
                add_tests(name, {run_timeout = 120000})
            end
    end

    progress_source_target("progress_source_ownership_test", false)
    if has_config("liburing") then
        progress_source_target("progress_source_ownership_uring_test", true)
    end

    -- C2-E corrective-round mutation builds. Each restores the pre-fix
    -- unconditional close_admission progress signal and must be killed by the
    -- transition-regression tests of the ownership suite; they are executed
    -- and recorded manually, never run as regular tests.
    progress_source_target("progress_source_mut_close_admission_always_signals", false,
                           "SLUICE_C2E_MUTANT_CLOSE_ADMISSION_ALWAYS_SIGNALS")
    if has_config("liburing") then
        progress_source_target("progress_source_uring_mut_close_admission_always_signals", true,
                               "SLUICE_C2E_MUTANT_CLOSE_ADMISSION_ALWAYS_SIGNALS")
    end
end

-- C2-B (#397) ThreadPool no-lost-wake race campaign and external-loop W-03
-- evidence. Same self-contained seam-build shape, observing the ThreadPool
-- backend only; shared ProgressSource changes must still compile in the
-- liburing-enabled progress/cutover targets.
do
    local function c2b_progress_target(name, test_source)
        target(name)
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include", R .. "src/async")
            add_defines("SLUICE_ASYNC_INTERNAL_TESTING")
            add_files(test_source,
                      R .. "src/async/async_io_context.cpp",
                      R .. "src/async/batch.cpp",
                      R .. "src/async/op_helpers.cpp",
                      R .. "src/async/threadpool_backend.cpp",
                      R .. "src/async/request_handle.cpp",
                      R .. "src/async/fail_fast.cpp",
                      R .. "src/async/detail/context_identity.cpp",
                      R .. "src/async/detail/request_core.cpp")
            add_tests(name, {run_timeout = 120000})
    end

    c2b_progress_target("threadpool_progress_race_test",
                        R .. "tests/threadpool_progress_race_test.cpp")
    c2b_progress_target("threadpool_external_loop_test",
                        R .. "tests/threadpool_external_loop_test.cpp")
end

-- C2-C (#397) io_uring readiness convergence evidence: kernel-style
-- no-lost-wake race campaign, real-ring external-loop W-03 half, and the
-- kernel eventfd registration lifecycle. Real ring required; registered only
-- when the liburing build switch is on.
if has_config("liburing") then
    local function c2c_uring_progress_target(name, test_source, extra_define)
        target(name)
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include", R .. "src/async")
            add_defines("SLUICE_ASYNC_INTERNAL_TESTING", "SLUICE_HAS_LIBURING")
            if extra_define ~= nil then
                add_defines(extra_define)
            end
            add_links("uring")
            add_files(test_source,
                      R .. "src/async/async_io_context.cpp",
                      R .. "src/async/uring_transport.cpp",
                      R .. "src/async/uring_completion.cpp",
                      R .. "src/async/uring_backend.cpp",
                      R .. "src/async/request_handle.cpp",
                      R .. "src/async/fail_fast.cpp",
                      R .. "src/async/detail/context_identity.cpp",
                      R .. "src/async/detail/request_core.cpp")
            if extra_define == nil then
                add_tests(name, {run_timeout = 120000})
            end
    end

    c2c_uring_progress_target("uring_progress_race_test",
                              R .. "tests/uring_progress_race_test.cpp")
    c2c_uring_progress_target("uring_external_loop_test",
                              R .. "tests/uring_external_loop_test.cpp")
    c2c_uring_progress_target("uring_registration_lifecycle_test",
                              R .. "tests/uring_registration_lifecycle_test.cpp")

    -- C2-C corrective-round mutation builds. Each restores one abandoned
    -- advertisement/health mechanism and must be killed by the named test of
    -- the race suite; they are executed and recorded manually, never run as
    -- regular tests.
    c2c_uring_progress_target("uring_progress_race_mut_transport_retry_unadvertised",
                              R .. "tests/uring_progress_race_test.cpp",
                              "SLUICE_B1C_MUTANT_TRANSPORT_RETRY_UNADVERTISED")
    c2c_uring_progress_target("uring_progress_race_mut_overflow_flush_ignored",
                              R .. "tests/uring_progress_race_test.cpp",
                              "SLUICE_B1C_MUTANT_OVERFLOW_FLUSH_IGNORED")
    c2c_uring_progress_target("uring_progress_race_mut_overflow_absence_settles_inflight",
                              R .. "tests/uring_progress_race_test.cpp",
                              "SLUICE_E1_MUTANT_OVERFLOW_ABSENCE_SETTLES_INFLIGHT")
    c2c_uring_progress_target("uring_progress_race_mut_post_poison_flush_gated",
                              R .. "tests/uring_progress_race_test.cpp",
                              "SLUICE_E1_MUTANT_POST_POISON_FLUSH_GATED")
    c2c_uring_progress_target("uring_external_loop_mut_host_ignores_dispatch_retry",
                              R .. "tests/uring_external_loop_test.cpp",
                              "SLUICE_B1C_MUTANT_HOST_IGNORES_DISPATCH_RETRY")
end

-- B2 (#395) public Request<T> evidence. Same self-contained seam-build shape as
-- the ownership target: the target compiles its own copies of the async TUs it
-- observes and links neither sluice_async nor the other seam builds.
do
    local function public_request_target(name, test_source, with_liburing, extra_define)
        target(name)
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include", R .. "src/async")
            add_defines("SLUICE_ASYNC_INTERNAL_TESTING")
            if extra_define ~= nil then
                add_defines(extra_define)
            end
            local files = {
                test_source,
                R .. "src/async/async_io_context.cpp",
                R .. "src/async/threadpool_backend.cpp",
                R .. "src/async/request_handle.cpp",
                R .. "src/async/fail_fast.cpp",
                R .. "src/async/detail/context_identity.cpp",
                R .. "src/async/detail/request_core.cpp",
            }
            if with_liburing then
                add_defines("SLUICE_HAS_LIBURING", "SLUICE_PUBLIC_REQUEST_URING")
                add_links("uring")
                for _, tu in ipairs(uring_backend_sources) do
                    table.insert(files, tu)
                end
            end
            add_files(files)
            if extra_define == nil then
                add_tests(name)
            end
    end

    public_request_target("public_request_test", R .. "tests/public_request_test.cpp", false)
    if has_config("liburing") then
        public_request_target("public_request_uring_test",
                              R .. "tests/public_request_test.cpp", true)
        public_request_target("public_request_release_violation_uring_test",
                              R .. "tests/public_request_release_violation_test.cpp", true)
    end
    public_request_target("public_request_release_violation_test",
                          R .. "tests/public_request_release_violation_test.cpp", false)

    -- B2 named mutation builds. Each flips one load-bearing mechanism of the
    -- public-result surface and must be killed by the named failing assertion
    -- of the killer suite; they are executed and recorded manually, never run
    -- as regular tests.
    public_request_target("public_request_mut_consume_keeps_binding",
                          R .. "tests/public_request_test.cpp", false,
                          "SLUICE_B2_MUTANT_CONSUME_KEEPS_BINDING")
    public_request_target("public_request_mut_observe_ignores_publication",
                          R .. "tests/public_request_test.cpp", false,
                          "SLUICE_B2_MUTANT_OBSERVE_IGNORES_PUBLICATION")
    public_request_target("public_request_mut_observe_consumes",
                          R .. "tests/public_request_test.cpp", false,
                          "SLUICE_B2_MUTANT_OBSERVE_CONSUMES")
    public_request_target("public_request_mut_release_forgets_binding",
                          R .. "tests/public_request_test.cpp", false,
                          "SLUICE_B2_MUTANT_RELEASE_FORGETS_BINDING")
    public_request_target("public_request_mut_nonterminal_release_detaches",
                          R .. "tests/public_request_release_violation_test.cpp", false,
                          "SLUICE_B2_MUTANT_NONTERMINAL_RELEASE_DETACHES")
    public_request_target("public_request_mut_discard_accepts_inflight",
                          R .. "tests/public_request_release_violation_test.cpp", false,
                          "SLUICE_B2_MUTANT_DISCARD_ACCEPTS_INFLIGHT")
end

-- E2 (#452) shutdown lifecycle oracles: retained results at execution close,
-- the destructor death matrix, notification-fd retirement and reuse, the
-- observer settlement path, the admission-close dual winner and the
-- poison/control-pin retirement evidence. Same seam-build shape as the
-- public request targets: the target compiles its own copies of the async
-- TUs, and the uring profile links the real backend plus the fiction hooks.
do
    local function shutdown_lifecycle_target(name, with_liburing, extra_define)
        target(name)
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include", R .. "src/async")
            add_defines("SLUICE_ASYNC_INTERNAL_TESTING")
            if extra_define ~= nil then
                add_defines(extra_define)
            end
            local files = {
                R .. "tests/shutdown_lifecycle_test.cpp",
                R .. "src/async/async_io_context.cpp",
                R .. "src/async/threadpool_backend.cpp",
                R .. "src/async/request_handle.cpp",
                R .. "src/async/fail_fast.cpp",
                R .. "src/async/detail/context_identity.cpp",
                R .. "src/async/detail/request_core.cpp",
            }
            if with_liburing then
                add_defines("SLUICE_HAS_LIBURING", "SLUICE_SHUTDOWN_URING")
                add_links("uring")
                for _, tu in ipairs(uring_backend_sources) do
                    table.insert(files, tu)
                end
            end
            add_files(files)
            if extra_define == nil then
                add_tests(name)
            end
    end

    shutdown_lifecycle_target("shutdown_lifecycle_test", false)
    if has_config("liburing") then
        shutdown_lifecycle_target("shutdown_lifecycle_uring_test", true)
    end

    -- E2 named mutation builds; executed manually, never regular tests.
    -- M6, M7 and M8 are killed by the ShutdownCore TLA+ mirrors in
    -- scripts/verify_tla.sh; the rest must be killed by the named failing
    -- assertion recorded next to each target.
    if has_config("liburing") then
        shutdown_lifecycle_target("shutdown_mut_m1_occupancy_zero_destroyable", true,
                                  "SLUICE_E2_MUTANT_M1_OCCUPANCY_ZERO_DESTROYABLE")
        shutdown_lifecycle_target("shutdown_mut_m5_poison_fabricates_success", true,
                                  "SLUICE_E2_MUTANT_M5_POISON_FABRICATES_SUCCESS")
        shutdown_lifecycle_target("shutdown_mut_m9_poison_releases_running_borrow", true,
                                  "SLUICE_E2_MUTANT_M9_POISON_RELEASES_RUNNING_BORROW")
        shutdown_lifecycle_target("shutdown_mut_m11_allow_notification_borrow_during_shutdown",
                                  true,
                                  "SLUICE_E2_MUTANT_M11_ALLOW_NOTIFICATION_BORROW_DURING_SHUTDOWN")
    end
    shutdown_lifecycle_target("shutdown_mut_m2_control_pin_caller_violation", false,
                              "SLUICE_E2_MUTANT_M2_CONTROL_PIN_CALLER_VIOLATION")
    shutdown_lifecycle_target("shutdown_mut_m3_destructor_ignores_live_binding", false,
                              "SLUICE_E2_MUTANT_M3_DESTRUCTOR_IGNORES_LIVE_BINDING")
    shutdown_lifecycle_target("shutdown_mut_m4_close_requires_consumption", false,
                              "SLUICE_E2_MUTANT_M4_CLOSE_REQUIRES_CONSUMPTION")
    shutdown_lifecycle_target("shutdown_mut_m10_prejoin_stale_dispatch_violation", false,
                              "SLUICE_E2_MUTANT_M10_PREJOIN_STALE_DISPATCH_VIOLATION")
end

-- E1 (#400) cross-backend File-contract conformance. One semantic suite is
-- compiled once per profile: the ThreadPool variant runs the real worker
-- path, and the io_uring variant runs the real ring plus the deterministic
-- submit fiction for the claimed/stuck control windows. The uring variant
-- also links the ThreadPool backend so both profiles' metadata records are
-- compared in one process. The seam build compiles its own copies of the
-- async TUs, mirroring the public request target's shape.
do
    local function e1_conformance_target(name, with_liburing, extra_define)
        target(name)
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include", R .. "src/async")
            add_defines("SLUICE_ASYNC_INTERNAL_TESTING")
            if extra_define ~= nil then
                add_defines(extra_define)
            end
            local files = {
                R .. "tests/backend_conformance_e1_test.cpp",
                R .. "src/async/async_io_context.cpp",
                R .. "src/async/threadpool_backend.cpp",
                R .. "src/async/request_handle.cpp",
                R .. "src/async/fail_fast.cpp",
                R .. "src/async/detail/context_identity.cpp",
                R .. "src/async/detail/request_core.cpp",
            }
            if with_liburing then
                add_defines("SLUICE_HAS_LIBURING", "SLUICE_E1_CONFORMANCE_URING")
                add_links("uring")
                for _, tu in ipairs(uring_backend_sources) do
                    table.insert(files, tu)
                end
            end
            add_files(files)
            if extra_define == nil then
                add_tests(name)
            end
    end

    e1_conformance_target("backend_conformance_e1_threadpool_test", false)
    if has_config("liburing") then
        e1_conformance_target("backend_conformance_e1_uring_test", true)

        -- E1 named mutation builds. Each flips one load-bearing mechanism of
        -- the #400 refinements and must be killed by the named failing
        -- assertion of the conformance suite; they are executed and recorded
        -- manually, never run as regular tests.
        e1_conformance_target("e1_conformance_mut_metadata_drops_record", true,
                              "SLUICE_E1_MUTANT_METADATA_DROPS_RECORD")
        e1_conformance_target("e1_conformance_mut_byte_failure_accounted", true,
                              "SLUICE_E1_MUTANT_BYTE_FAILURE_ACCOUNTED")
        e1_conformance_target("e1_conformance_mut_sticky_intent_dropped", true,
                              "SLUICE_E1_MUTANT_STICKY_INTENT_DROPPED")
        e1_conformance_target("e1_conformance_mut_capability_probe_ignored", true,
                              "SLUICE_E1_MUTANT_CAPABILITY_PROBE_IGNORED")
        e1_conformance_target("e1_conformance_mut_cancel_progress_signal_dropped", true,
                              "SLUICE_E1_MUTANT_CANCEL_PROGRESS_SIGNAL_DROPPED")
        e1_conformance_target("e1_conformance_mut_submit_batch_invisible", true,
                              "SLUICE_E1_MUTANT_SUBMIT_BATCH_INVISIBLE")
        e1_conformance_target("e1_conformance_mut_opcode_probe_ignored", true,
                              "SLUICE_E1_MUTANT_OPCODE_PROBE_IGNORED")
        e1_conformance_target("e1_conformance_mut_masked_sq_cardinality", true,
                              "SLUICE_E1_MUTANT_MASKED_SQ_CARDINALITY")
    end
    e1_conformance_target("e1_conformance_mut_write_failure_accounted", false,
                          "SLUICE_E1_MUTANT_WRITE_FAILURE_ACCOUNTED")
    e1_conformance_target("e1_conformance_mut_cancel_reports_requested", false,
                          "SLUICE_E1_MUTANT_CANCEL_REPORTS_REQUESTED")
end

-- D1 (#398) RequestScope evidence. Same self-contained seam-build shape as the
-- public request target: the target compiles its own copies of the async TUs it
-- observes and links neither sluice_async nor the other seam builds.
do
    local function request_scope_target(name, test_source, with_liburing, extra_define)
        target(name)
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include", R .. "src/async")
            add_defines("SLUICE_ASYNC_INTERNAL_TESTING")
            if extra_define ~= nil then
                add_defines(extra_define)
            end
            local files = {
                test_source,
                R .. "src/async/async_io_context.cpp",
                R .. "src/async/request_scope.cpp",
                R .. "src/async/threadpool_backend.cpp",
                R .. "src/async/request_handle.cpp",
                R .. "src/async/fail_fast.cpp",
                R .. "src/async/detail/context_identity.cpp",
                R .. "src/async/detail/request_core.cpp",
            }
            if with_liburing then
                add_defines("SLUICE_HAS_LIBURING", "SLUICE_PUBLIC_REQUEST_URING")
                add_links("uring")
                for _, tu in ipairs(uring_backend_sources) do
                    table.insert(files, tu)
                end
            end
            add_files(files)
            if extra_define == nil then
                add_tests(name)
            end
    end

    request_scope_target("request_scope_test", R .. "tests/request_scope_test.cpp", false)
    if has_config("liburing") then
        request_scope_target("request_scope_uring_test",
                             R .. "tests/request_scope_test.cpp", true)
    end

    -- D1 named mutation builds. Each flips one load-bearing RequestScope rule
    -- and must be killed by the named failing scenario of the killer suite;
    -- they are executed and recorded manually, never run as regular tests.
    request_scope_target("request_scope_mut_reserve_after_accept",
                         R .. "tests/request_scope_test.cpp", false,
                         "SLUICE_D1_MUTANT_RESERVE_AFTER_ACCEPT")
    request_scope_target("request_scope_mut_commit_drops_request",
                         R .. "tests/request_scope_test.cpp", false,
                         "SLUICE_D1_MUTANT_COMMIT_DROPS_REQUEST")
    request_scope_target("request_scope_mut_timeout_releases_slot",
                         R .. "tests/request_scope_test.cpp", false,
                         "SLUICE_D1_MUTANT_TIMEOUT_RELEASES_SLOT")
    request_scope_target("request_scope_mut_destructor_skips_settle",
                         R .. "tests/request_scope_test.cpp", false,
                         "SLUICE_D1_MUTANT_DESTRUCTOR_SKIPS_SETTLE")
    request_scope_target("request_scope_mut_finish_swallows_error",
                         R .. "tests/request_scope_test.cpp", false,
                         "SLUICE_D1_MUTANT_FINISH_SWALLOWS_ERROR")
    request_scope_target("request_scope_mut_finish_cancels_cleanup",
                         R .. "tests/request_scope_test.cpp", false,
                         "SLUICE_D1_MUTANT_FINISH_CANCELS_CLEANUP")
end

-- B1-B ThreadPool cutover evidence. The deterministic pause gates and fault
-- injections are macro-guarded backend surface, so this target compiles its
-- own copies of the async TUs and links neither sluice_async nor the other
-- seam builds, mirroring the ownership target's shape.
do
    target("threadpool_core_cutover_test")
        set_kind("binary")
        set_default(false)
        set_group("test")
        add_deps("sluice_core")
        add_includedirs(R .. "include", R .. "src/async")
        add_defines("SLUICE_ASYNC_INTERNAL_TESTING")
        add_files(R .. "tests/threadpool_core_cutover_test.cpp",
                  R .. "src/async/async_io_context.cpp",
                  R .. "src/async/threadpool_backend.cpp",
                  R .. "src/async/request_handle.cpp",
                  R .. "src/async/fail_fast.cpp",
                  R .. "src/async/detail/context_identity.cpp",
                  R .. "src/async/detail/request_core.cpp")
        add_tests("threadpool_core_cutover_test")
end

-- B1-C io_uring cutover evidence. Same self-contained seam-build shape as the
-- ThreadPool cutover target, plus the real liburing backend TU. The
-- deterministic ordering cases inject CQEs through the seam; the real-kernel
-- cases exercise the live ring. Requires the liburing build switch.
if has_config("liburing") then
    local function uring_cutover_target(name, extra_define)
        target(name)
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include", R .. "src/async")
            add_defines("SLUICE_ASYNC_INTERNAL_TESTING", "SLUICE_HAS_LIBURING")
            if extra_define ~= nil then
                add_defines(extra_define)
            end
            add_links("uring")
            add_files(R .. "tests/uring_core_cutover_test.cpp",
                      R .. "src/async/async_io_context.cpp",
                      R .. "src/async/uring_transport.cpp",
                      R .. "src/async/uring_completion.cpp",
                      R .. "src/async/uring_backend.cpp",
                      R .. "src/async/request_handle.cpp",
                      R .. "src/async/fail_fast.cpp",
                      R .. "src/async/detail/context_identity.cpp",
                      R .. "src/async/detail/request_core.cpp")
            if extra_define == nil then
                add_tests(name)
            end
    end

    uring_cutover_target("uring_core_cutover_test", nil)

    -- B1-C named mutation builds. Each flips one load-bearing mechanism and
    -- must be killed by the named failing assertion of the same suite; they
    -- are executed and recorded manually, never run as regular tests.
    uring_cutover_target("uring_cutover_mut_cookie_reuse", "SLUICE_B1C_MUTANT_COOKIE_REUSE")
    uring_cutover_target("uring_cutover_mut_cancel_cqe_terminal",
                         "SLUICE_B1C_MUTANT_CANCEL_CQE_AS_ORIGINAL_TERMINAL")
    uring_cutover_target("uring_cutover_mut_publish_before_retire",
                         "SLUICE_B1C_MUTANT_PUBLISH_BEFORE_BORROW_RETIREMENT")
    uring_cutover_target("uring_cutover_mut_drop_outcome",
                         "SLUICE_B1C_MUTANT_DROP_ORIGINAL_OUTCOME")
    uring_cutover_target("uring_cutover_mut_remove_control_pin",
                         "SLUICE_B1C_MUTANT_REMOVE_CONTROL_PIN")
    uring_cutover_target("uring_cutover_mut_reclaim_before_control",
                         "SLUICE_B1C_MUTANT_RECLAIM_BEFORE_CONTROL_RETIREMENT")
    uring_cutover_target("uring_cutover_mut_strand_poison",
                         "SLUICE_B1C_MUTANT_STRAND_POST_ACCEPT_SUBMIT_FAILURE")
    uring_cutover_target("uring_cutover_mut_zero_op_dispatch",
                         "SLUICE_B1C_MUTANT_PREMATURE_ZERO_OP_DISPATCH")
end

-- C1-D (#430) RuntimeTaskContext waiter-adapter evidence. The scheduler and
-- runtime seams are macro-guarded internal-testing surface, so this target
-- compiles its own copy of the async TUs the runtime needs (the full
-- src/async set plus the core detail TUs) and links neither sluice_async nor
-- the other seam builds: the seam build and the production build never meet
-- in one binary. `src/async` is on the include path for the internal headers
-- the seams include relative to their own directory.
do
    target("runtime_waiter_observer_test")
        set_kind("binary")
        set_default(false)
        set_group("test")
        add_deps("sluice_core")
        add_includedirs(R .. "include", R .. "src/async")
        add_defines("SLUICE_ASYNC_INTERNAL_TESTING")
        if has_config("liburing") then
            add_defines("SLUICE_HAS_LIBURING")
            add_links("uring")
        end
        add_files(R .. "tests/runtime_waiter_observer_test.cpp",
                  R .. "src/async/*.cpp",
                  R .. "src/async/detail/context_identity.cpp",
                  R .. "src/async/detail/request_core.cpp")
        add_tests("runtime_waiter_observer_test")
end

-- Narrow stackful host: pipeline tracer, failure-path cases, progress-owner
-- composition and bound evidence. Same self-contained seam topology as the
-- runtime_waiter_observer_test target above (own copy of the async TUs with
-- the internal-testing define; never linked with the production library).
do
    local function stackful_host_target(name, extra_define)
        target(name)
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include", R .. "src/async")
            add_defines("SLUICE_ASYNC_INTERNAL_TESTING")
            if extra_define ~= nil then
                add_defines(extra_define)
            end
            if has_config("liburing") then
                add_defines("SLUICE_HAS_LIBURING")
                add_links("uring")
            end
            add_files(R .. "tests/stackful_host_test.cpp",
                      R .. "src/async/*.cpp",
                      R .. "src/async/detail/context_identity.cpp",
                      R .. "src/async/detail/request_core.cpp")
            if extra_define == nil then
                add_tests(name)
            end
    end

    stackful_host_target("stackful_host_test", nil)

    -- Named mutation builds; executed manually, never regular tests.
    stackful_host_target("stackful_host_mut_second_owner", "SLUICE_STACKFUL_HOST_MUTANT_SECOND_OWNER_ALLOWED")
    stackful_host_target("stackful_host_mut_wake_retired", "SLUICE_STACKFUL_HOST_MUTANT_WAKE_RETIRED_TASK")
    stackful_host_target("stackful_host_mut_task_error_swallowed",
                         "SLUICE_STACKFUL_HOST_MUTANT_TASK_ERROR_SWALLOWED")
    stackful_host_target("stackful_host_mut_stop_returns_unsettled",
                         "SLUICE_STACKFUL_HOST_MUTANT_STOP_RETURNS_UNSETTLED")
    stackful_host_target("stackful_host_mut_await_stop_unsettled",
                         "SLUICE_STACKFUL_HOST_MUTANT_AWAIT_STOP_RETURNS_UNSETTLED")
    stackful_host_target("stackful_host_mut_double_wake", "SLUICE_STACKFUL_HOST_MUTANT_DOUBLE_WAKE")
    stackful_host_target("stackful_host_mut_deadline_cancels",
                         "SLUICE_STACKFUL_HOST_MUTANT_DEADLINE_CANCELS")
    stackful_host_target("stackful_host_mut_stale_error_retained",
                         "SLUICE_STACKFUL_HOST_MUTANT_STALE_ERROR_RETAINED")
    stackful_host_target("stackful_host_mut_stop_interrupts_control",
                         "SLUICE_STACKFUL_HOST_MUTANT_STOP_INTERRUPTS_CONTROL")
    stackful_host_target("stackful_host_mut_stop_implicit_cancel",
                         "SLUICE_STACKFUL_HOST_MUTANT_STOP_IMPLICIT_CANCEL")
    stackful_host_target("stackful_host_mut_expired_deadline_spin",
                         "SLUICE_STACKFUL_HOST_MUTANT_EXPIRED_DEADLINE_SPIN")
    stackful_host_target("stackful_host_mut_skip_control_ack",
                         "SLUICE_STACKFUL_HOST_MUTANT_SKIP_CONTROL_ACK")
    stackful_host_target("stackful_host_mut_empty_precheck_bypass",
                         "SLUICE_STACKFUL_HOST_MUTANT_EMPTY_COMPOSITION_BYPASSES_PRECHECK")
end
