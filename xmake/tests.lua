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

-- Shared File semantic oracle: decision tables, property tests and
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

-- Direct execution surface: canonical File close/RAII semantics, minimal
-- metadata and identity, the direct cursor/positional distinction, and the
-- exact/all composition surfaces. `direct_w01_consumer_probe` is the
-- core-only consumer: it links `sluice_core` alone, so it proves the W-01
-- trace needs no async runtime and no liburing.
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

-- App-consumption tests: exercise the app engines through the canonical
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
            add_deps("sluice_core")
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
            add_deps("sluice_core")
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
            add_deps("sluice_core")
            add_includedirs(R .. "include", dir)
            add_files(R .. "tests/app_tail_consumption_test.cpp", dir .. "/tail_task.cpp")
            add_tests("app_tail_consumption_test")
    end
end

-- TAIL lifecycle oracles, engine layer: stop/settlement/teardown
-- contracts over the public TailEngine API. The target compiles an in-process
-- pread interposer (ELF symbol interposition, dev/ino-gated) so a follow read
-- can be held while stop is requested; no production seam is involved.
do
    local dir = R .. "apps/sluice-tail"
    if os.isfile(dir .. "/tail_task.cpp") and os.isfile(R .. "tests/app_tail_lifecycle_oracle_test.cpp") then
        target("app_tail_lifecycle_oracle_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include", dir)
            add_files(R .. "tests/app_tail_lifecycle_oracle_test.cpp", dir .. "/tail_task.cpp")
            add_tests("app_tail_lifecycle_oracle_test", {run_timeout = 120000})
    end
end

-- TAIL lifecycle oracles, CLI layer: the real sluice-tail binary
-- under SIGINT/SIGTERM and under a held pending read released by the harness.
-- Depends on the app target so the binary and the LD_PRELOAD interposer
-- (tests/support/tail_pread_shim.c, loaded only by the spawned child) are
-- always built next to the test binary.
do
    local dir = R .. "apps/sluice-tail"
    if os.isfile(dir .. "/main.cpp") and os.isfile(R .. "tests/app_tail_cli_oracle_test.cpp") and
       os.isfile(R .. "tests/support/tail_pread_shim.c") then
        target("tail_pread_shim")
            set_kind("shared")
            set_default(false)
            set_group("test")
            add_files(R .. "tests/support/tail_pread_shim.c")
        target("app_tail_cli_oracle_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice-tail", "tail_pread_shim")
            add_includedirs(R .. "include")
            add_files(R .. "tests/app_tail_cli_oracle_test.cpp")
            add_tests("app_tail_cli_oracle_test", {run_timeout = 180000})
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

-- Pipelined-copy contract oracles: the scripted-backend suite driving
-- run_pipelined_copy_with_backend through controlled submission, completion,
-- and failure ordering (read-ahead window, write ordering, short I/O, error
-- selection, settlement and bounded reuse). Test-only; compiles the production
-- copy_task.cpp unmodified, exactly like the consumption target.
do
    local dir = R .. "apps/sluice-copy"
    if os.isfile(dir .. "/copy_task.cpp") and os.isfile(R .. "tests/app_copy_pipeline_oracle_test.cpp") and
       os.isfile(R .. "tests/support/pipelined_copy_oracle_support.hpp") then
        target("app_copy_pipeline_oracle_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", dir, R .. "tests")
            add_files(R .. "tests/app_copy_pipeline_oracle_test.cpp", dir .. "/copy_task.cpp")
            add_tests("app_copy_pipeline_oracle_test")
    end
end

-- Directory-fsync fault-injection oracle for the atomic output path: this is
-- the only target defining SLUICE_COPY_INTERNAL_TESTING, which arms the
-- DirFsyncScript seam at directory_fsync (apps/sluice-copy/safe_output.cpp)
-- for the EINTR-retry and terminal-error evidence.
do
    local dir = R .. "apps/sluice-copy"
    if os.isfile(dir .. "/safe_output_test_seams.hpp") then
        target("app_copy_dir_fsync_test")
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core", "sluice_async")
            add_includedirs(R .. "include", dir)
            add_defines("SLUICE_COPY_INTERNAL_TESTING")
            add_files(R .. "tests/app_copy_dir_fsync_test.cpp", dir .. "/copy_task.cpp",
                      dir .. "/file_domain.cpp", dir .. "/safe_output.cpp")
            add_tests("app_copy_dir_fsync_test")
    end
end

-- RequestCore substrate tests: each target compiles the substrate TU directly
-- and links only sluice_core, so the protocol suite never links the async
-- runtime, backends or liburing. The protocol/publication targets enable the
-- internal-testing observation seam; the consumer probe consumes the public
-- surface only, proving the substrate stands alone without waiter/routing
-- vocabulary or a production backend.
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

    -- Named mutation builds. Each flips one load-bearing arm of
    -- the attach/publication resolution and must be killed by the named
    -- failing assertion of the killer suite; they are executed and recorded
    -- manually, never run as regular tests.
    request_core_target("request_core_mut_attach_ignores_publication",
                        R .. "tests/request_core_protocol_test.cpp", true,
                        "SLUICE_REQUEST_CORE_MUTANT_ATTACH_IGNORES_PUBLICATION")
    request_core_target("request_core_mut_attach_treats_terminal_as_published",
                        R .. "tests/request_core_protocol_test.cpp", true,
                        "SLUICE_REQUEST_CORE_MUTANT_ATTACH_TREATS_TERMINAL_AS_PUBLISHED")

    -- Named mutation builds over the delivery state machine.
    request_core_target("request_core_mut_delivery_claim_unbounded",
                        R .. "tests/request_core_protocol_test.cpp", true,
                        "SLUICE_REQUEST_CORE_MUTANT_DELIVERY_CLAIM_UNBOUNDED")
    request_core_target("request_core_mut_cancel_during_delivery_returns",
                        R .. "tests/request_core_protocol_test.cpp", true,
                        "SLUICE_REQUEST_CORE_MUTANT_CANCEL_DURING_DELIVERY_RETURNS")
    request_core_target("request_core_mut_publication_skips_queue",
                        R .. "tests/request_core_protocol_test.cpp", true,
                        "SLUICE_REQUEST_CORE_MUTANT_PUBLICATION_SKIPS_QUEUE")
end

-- Context ownership/identity evidence. The observed surface (the
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

-- ProgressSource ownership/lifetime/route evidence. Same
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

    -- Corrective-round mutation builds. Each restores the pre-fix
    -- unconditional close_admission progress signal and must be killed by the
    -- transition-regression tests of the ownership suite; they are executed
    -- and recorded manually, never run as regular tests.
    progress_source_target("progress_source_mut_close_admission_always_signals", false,
                           "SLUICE_PROGRESS_SOURCE_MUTANT_CLOSE_ADMISSION_ALWAYS_SIGNALS")
    if has_config("liburing") then
        progress_source_target("progress_source_uring_mut_close_admission_always_signals", true,
                               "SLUICE_PROGRESS_SOURCE_MUTANT_CLOSE_ADMISSION_ALWAYS_SIGNALS")
    end
end

-- ThreadPool no-lost-wake race and external-loop W-03
-- evidence. Same self-contained seam-build shape, observing the ThreadPool
-- backend only; shared ProgressSource changes must still compile in the
-- liburing-enabled progress/integration targets.
do
    local function threadpool_progress_target(name, test_source)
        target(name)
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include", R .. "src/async")
            add_defines("SLUICE_ASYNC_INTERNAL_TESTING")
            add_files(test_source,
                      R .. "src/async/async_io_context.cpp",
                      R .. "src/async/threadpool_backend.cpp",
                      R .. "src/async/fail_fast.cpp",
                      R .. "src/async/detail/context_identity.cpp",
                      R .. "src/async/detail/request_core.cpp")
            add_tests(name, {run_timeout = 120000})
    end

    threadpool_progress_target("threadpool_progress_race_test",
                        R .. "tests/threadpool_progress_race_test.cpp")
    threadpool_progress_target("progress_notification_suppression_test",
                        R .. "tests/progress_notification_suppression_test.cpp")
    threadpool_progress_target("threadpool_progress_pass_bound_test",
                        R .. "tests/threadpool_progress_pass_bound_test.cpp")
    threadpool_progress_target("threadpool_external_loop_test",
                        R .. "tests/threadpool_external_loop_test.cpp")
end

-- io_uring readiness convergence evidence: kernel-style
-- no-lost-wake race campaign, real-ring external-loop W-03 half, and the
-- kernel eventfd registration lifecycle. Real ring required; registered only
-- when the liburing build switch is on.
if has_config("liburing") then
    local function uring_progress_target(name, test_source, extra_define)
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
                      R .. "src/async/fail_fast.cpp",
                      R .. "src/async/detail/context_identity.cpp",
                      R .. "src/async/detail/request_core.cpp")
            if extra_define == nil then
                add_tests(name, {run_timeout = 120000})
            end
    end

    uring_progress_target("uring_progress_race_test",
                              R .. "tests/uring_progress_race_test.cpp")
    uring_progress_target("uring_external_loop_test",
                              R .. "tests/uring_external_loop_test.cpp")
    uring_progress_target("uring_registration_lifecycle_test",
                              R .. "tests/uring_registration_lifecycle_test.cpp")
    uring_progress_target("uring_owed_residue_test",
                              R .. "tests/uring_owed_residue_test.cpp")
    uring_progress_target("uring_owed_counter_parity_test",
                              R .. "tests/uring_owed_counter_parity_test.cpp")
    uring_progress_target("uring_pending_control_parity_test",
                              R .. "tests/uring_pending_control_parity_test.cpp")

    -- Corrective-round mutation builds. Each restores one abandoned
    -- advertisement/health mechanism and must be killed by the named test of
    -- the race suite; they are executed and recorded manually, never run as
    -- regular tests.
    uring_progress_target("uring_progress_race_mut_transport_retry_unadvertised",
                              R .. "tests/uring_progress_race_test.cpp",
                              "SLUICE_URING_PROGRESS_MUTANT_TRANSPORT_RETRY_UNADVERTISED")
    uring_progress_target("uring_progress_race_mut_overflow_flush_ignored",
                              R .. "tests/uring_progress_race_test.cpp",
                              "SLUICE_URING_PROGRESS_MUTANT_OVERFLOW_FLUSH_IGNORED")
    uring_progress_target("uring_progress_race_mut_overflow_absence_settles_inflight",
                              R .. "tests/uring_progress_race_test.cpp",
                              "SLUICE_URING_PROGRESS_MUTANT_OVERFLOW_ABSENCE_SETTLES_INFLIGHT")
    uring_progress_target("uring_progress_race_mut_post_poison_flush_gated",
                              R .. "tests/uring_progress_race_test.cpp",
                              "SLUICE_URING_EXTERNAL_LOOP_MUTANT_POST_POISON_FLUSH_GATED")
    uring_progress_target("progress_source_uring_mut_final_probe_dropped",
                              R .. "tests/uring_progress_race_test.cpp",
                              "SLUICE_PROGRESS_SOURCE_MUTANT_FINAL_PROBE_DROPPED")
    uring_progress_target("uring_external_loop_mut_host_ignores_dispatch_retry",
                              R .. "tests/uring_external_loop_test.cpp",
                              "SLUICE_URING_EXTERNAL_LOOP_MUTANT_HOST_IGNORES_DISPATCH_RETRY")

    -- Owed-count mutation builds. Each breaks one edge of the derived
    -- owed_count_ summary or its supporting invariant and must be killed by
    -- the named oracle: the parity suite grounds the counter in the
    -- authoritative delivery-record scan and external deliveries, the residue
    -- suite owns the post-pass report verdict, the external-loop suite owns
    -- the notification wake, and the control-ref reclaim mutant turns the
    -- submit-reset invariant violation into a loud failure.
    uring_progress_target("uring_owed_mut_missing_increment",
                              R .. "tests/uring_owed_counter_parity_test.cpp",
                              "SLUICE_URING_MUTANT_OWED_COUNT_MISSING_INCREMENT")
    uring_progress_target("uring_owed_mut_missing_decrement",
                              R .. "tests/uring_owed_counter_parity_test.cpp",
                              "SLUICE_URING_MUTANT_OWED_COUNT_MISSING_DECREMENT")
    uring_progress_target("uring_owed_mut_empty_verdict_always",
                              R .. "tests/uring_owed_residue_test.cpp",
                              "SLUICE_URING_MUTANT_OWED_EMPTY_VERDICT_ALWAYS")
    uring_progress_target("uring_owed_mut_reclaim_ignoring_control_refs",
                              R .. "tests/uring_owed_counter_parity_test.cpp",
                              "SLUICE_REQUEST_CORE_MUTANT_RECLAIM_IGNORES_CONTROL_REFS")
    uring_progress_target("uring_owed_mut_zero_op_wake_dropped",
                              R .. "tests/uring_external_loop_test.cpp",
                              "SLUICE_URING_MUTANT_ZERO_OP_PUBLICATION_DROPS_WAKE")

    -- Pending-control mutation builds. Each breaks one edge of the derived
    -- pending_control_count_ summary or its zero-count scan gate and must be
    -- killed by the named parity/protocol oracle of the pending-control
    -- parity suite; they are executed and recorded manually, never run as
    -- regular tests.
    uring_progress_target("uring_pending_control_mut_missing_increment",
                              R .. "tests/uring_pending_control_parity_test.cpp",
                              "SLUICE_URING_MUTANT_PENDING_CONTROL_MISSING_INCREMENT")
    uring_progress_target("uring_pending_control_mut_missing_decrement",
                              R .. "tests/uring_pending_control_parity_test.cpp",
                              "SLUICE_URING_MUTANT_PENDING_CONTROL_MISSING_DECREMENT")
    uring_progress_target("uring_pending_control_mut_gate_always_skips",
                              R .. "tests/uring_pending_control_parity_test.cpp",
                              "SLUICE_URING_MUTANT_PENDING_CONTROL_GATE_ALWAYS_SKIPS")
    uring_progress_target("uring_pending_control_mut_double_increment",
                              R .. "tests/uring_pending_control_parity_test.cpp",
                              "SLUICE_URING_MUTANT_PENDING_CONTROL_DOUBLE_INCREMENT")
    uring_progress_target("uring_pending_control_mut_retire_stale_count",
                              R .. "tests/uring_pending_control_parity_test.cpp",
                              "SLUICE_URING_MUTANT_PENDING_CONTROL_RETIRE_STALE_COUNT")
end

-- Public Request<T> evidence. Same self-contained seam-build shape as
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

    -- Named mutation builds. Each flips one load-bearing mechanism of the
    -- public-result surface and must be killed by the named failing assertion
    -- of the killer suite; they are executed and recorded manually, never run
    -- as regular tests.
    public_request_target("public_request_mut_consume_keeps_binding",
                          R .. "tests/public_request_test.cpp", false,
                          "SLUICE_PUBLIC_REQUEST_MUTANT_CONSUME_KEEPS_BINDING")
    public_request_target("public_request_mut_observe_ignores_publication",
                          R .. "tests/public_request_test.cpp", false,
                          "SLUICE_PUBLIC_REQUEST_MUTANT_OBSERVE_IGNORES_PUBLICATION")
    public_request_target("public_request_mut_observe_consumes",
                          R .. "tests/public_request_test.cpp", false,
                          "SLUICE_PUBLIC_REQUEST_MUTANT_OBSERVE_CONSUMES")
    public_request_target("public_request_mut_release_forgets_binding",
                          R .. "tests/public_request_test.cpp", false,
                          "SLUICE_PUBLIC_REQUEST_MUTANT_RELEASE_FORGETS_BINDING")
    public_request_target("public_request_mut_nonterminal_release_detaches",
                          R .. "tests/public_request_release_violation_test.cpp", false,
                          "SLUICE_PUBLIC_REQUEST_MUTANT_NONTERMINAL_RELEASE_DETACHES")
    public_request_target("public_request_mut_discard_accepts_inflight",
                          R .. "tests/public_request_release_violation_test.cpp", false,
                          "SLUICE_PUBLIC_REQUEST_MUTANT_DISCARD_ACCEPTS_INFLIGHT")
end

-- Shutdown lifecycle oracles: retained results at execution close,
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

    -- Named mutation builds; executed manually, never regular tests.
    -- M6, M7 and M8 are killed by the ShutdownCore TLA+ mirrors in
    -- scripts/verify_tla.sh; the rest must be killed by the named failing
    -- assertion recorded next to each target.
    if has_config("liburing") then
        shutdown_lifecycle_target("shutdown_mut_m1_occupancy_zero_destroyable", true,
                                  "SLUICE_SHUTDOWN_MUTANT_M1_OCCUPANCY_ZERO_DESTROYABLE")
        shutdown_lifecycle_target("shutdown_mut_m5_poison_fabricates_success", true,
                                  "SLUICE_SHUTDOWN_MUTANT_M5_POISON_FABRICATES_SUCCESS")
        shutdown_lifecycle_target("shutdown_mut_m9_poison_releases_running_borrow", true,
                                  "SLUICE_SHUTDOWN_MUTANT_M9_POISON_RELEASES_RUNNING_BORROW")
        shutdown_lifecycle_target("shutdown_mut_m11_allow_notification_borrow_during_shutdown",
                                  true,
                                  "SLUICE_SHUTDOWN_MUTANT_M11_ALLOW_NOTIFICATION_BORROW_DURING_SHUTDOWN")
        shutdown_lifecycle_target("shutdown_mut_m12_execution_closed_fast_path_before_owner_check",
                                  true,
                                  "SLUICE_SHUTDOWN_MUTANT_M12_EXECUTION_CLOSED_FAST_PATH_BEFORE_OWNER_CHECK")
    end
    shutdown_lifecycle_target("shutdown_mut_m2_control_pin_caller_violation", false,
                              "SLUICE_SHUTDOWN_MUTANT_M2_CONTROL_PIN_CALLER_VIOLATION")
    shutdown_lifecycle_target("shutdown_mut_m3_destructor_ignores_live_binding", false,
                              "SLUICE_SHUTDOWN_MUTANT_M3_DESTRUCTOR_IGNORES_LIVE_BINDING")
    shutdown_lifecycle_target("shutdown_mut_m4_close_requires_consumption", false,
                              "SLUICE_SHUTDOWN_MUTANT_M4_CLOSE_REQUIRES_CONSUMPTION")
    shutdown_lifecycle_target("shutdown_mut_m10_prejoin_stale_dispatch_violation", false,
                              "SLUICE_SHUTDOWN_MUTANT_M10_PREJOIN_STALE_DISPATCH_VIOLATION")
end

-- Cross-backend File-contract conformance. One semantic suite is compiled
-- once per profile: the ThreadPool variant runs the real worker path, and the
-- io_uring variant runs the real ring plus the deterministic submit fiction
-- for the claimed/stuck control windows. The uring variant also links the
-- ThreadPool backend so both profiles' metadata records are compared in one
-- process. The seam build compiles its own copies of the async TUs, mirroring
-- the public request target's shape.
do
    local function backend_contract_conformance_target(name, with_liburing, extra_define)
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
                R .. "tests/backend_contract_conformance_test.cpp",
                R .. "src/async/async_io_context.cpp",
                R .. "src/async/threadpool_backend.cpp",
                R .. "src/async/fail_fast.cpp",
                R .. "src/async/detail/context_identity.cpp",
                R .. "src/async/detail/request_core.cpp",
            }
            if with_liburing then
                add_defines("SLUICE_HAS_LIBURING", "SLUICE_BACKEND_CONTRACT_CONFORMANCE_URING")
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

    backend_contract_conformance_target("backend_contract_conformance_threadpool_test", false)
    if has_config("liburing") then
        backend_contract_conformance_target("backend_contract_conformance_uring_test", true)

        -- Named mutation builds. Each flips one load-bearing conformance
        -- mechanism and must be killed by the named failing assertion of the
        -- conformance suite; they are executed and recorded manually, never
        -- run as regular tests.
        backend_contract_conformance_target("backend_contract_conformance_mut_metadata_drops_record", true,
                              "SLUICE_BACKEND_CONTRACT_MUTANT_METADATA_DROPS_RECORD")
        backend_contract_conformance_target("backend_contract_conformance_mut_byte_failure_accounted", true,
                              "SLUICE_BACKEND_CONTRACT_MUTANT_BYTE_FAILURE_ACCOUNTED")
        backend_contract_conformance_target("backend_contract_conformance_mut_sticky_intent_dropped", true,
                              "SLUICE_BACKEND_CONTRACT_MUTANT_STICKY_INTENT_DROPPED")
        backend_contract_conformance_target("backend_contract_conformance_mut_capability_probe_ignored", true,
                              "SLUICE_BACKEND_CONTRACT_MUTANT_CAPABILITY_PROBE_IGNORED")
        backend_contract_conformance_target("backend_contract_conformance_mut_cancel_progress_signal_dropped", true,
                              "SLUICE_BACKEND_CONTRACT_MUTANT_CANCEL_PROGRESS_SIGNAL_DROPPED")
        backend_contract_conformance_target("backend_contract_conformance_mut_submit_batch_invisible", true,
                              "SLUICE_BACKEND_CONTRACT_MUTANT_SUBMIT_BATCH_INVISIBLE")
        backend_contract_conformance_target("backend_contract_conformance_mut_opcode_probe_ignored", true,
                              "SLUICE_BACKEND_CONTRACT_MUTANT_OPCODE_PROBE_IGNORED")
        backend_contract_conformance_target("backend_contract_conformance_mut_masked_sq_cardinality", true,
                              "SLUICE_BACKEND_CONTRACT_MUTANT_MASKED_SQ_CARDINALITY")
    end
    backend_contract_conformance_target("backend_contract_conformance_mut_write_failure_accounted", false,
                          "SLUICE_BACKEND_CONTRACT_MUTANT_WRITE_FAILURE_ACCOUNTED")
    backend_contract_conformance_target("backend_contract_conformance_mut_cancel_reports_requested", false,
                          "SLUICE_BACKEND_CONTRACT_MUTANT_CANCEL_REPORTS_REQUESTED")
end

-- Request-path error-domain re-diff: the submission-failure
-- and terminal-outcome spellings observed on the public request path are
-- compared against the same native faults driven through the direct path.
-- The async TUs compile in production shape (no internal-testing seam): the
-- claim is about the shipped configuration.
do
    local function request_error_domain_target(name, with_liburing, extra_define)
        target(name)
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include", R .. "src/async")
            if extra_define ~= nil then
                add_defines(extra_define)
            end
            local files = {
                R .. "tests/request_error_domain_test.cpp",
                R .. "src/async/async_io_context.cpp",
                R .. "src/async/threadpool_backend.cpp",
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
                add_tests(name)
            end
    end

    request_error_domain_target("request_error_domain_threadpool_test", false)
    if has_config("liburing") then
        request_error_domain_target("request_error_domain_uring_test", true)

        -- Named mutation builds. Each flips one request-path error
        -- spelling and must be killed by the named failing assertion of the
        -- error-domain suite; executed and recorded manually, never run as
        -- regular tests.
        request_error_domain_target("request_error_domain_uring_mut_admission_reports_capacity",
                                    true, "SLUICE_ERR01_MUTANT_ADMISSION_CLOSE_REPORTS_CAPACITY")
        request_error_domain_target("request_error_domain_uring_mut_physical_errno_unmapped",
                                    true, "SLUICE_ERR01_MUTANT_PHYSICAL_ERRNO_UNMAPPED")
    end
    request_error_domain_target("request_error_domain_threadpool_mut_admission_reports_capacity",
                                false, "SLUICE_ERR01_MUTANT_ADMISSION_CLOSE_REPORTS_CAPACITY")
    request_error_domain_target("request_error_domain_threadpool_mut_physical_errno_unmapped",
                                false, "SLUICE_ERR01_MUTANT_PHYSICAL_ERRNO_UNMAPPED")
end

-- File-owner moves against outstanding accepted borrows. The
-- target compiles the two core TUs itself so the named move mutation build
-- can flip the move constructor alone; the async TUs stay in production
-- shape (no internal-testing seam).
do
    local function file_move_borrow_target(name, with_liburing, extra_define)
        target(name)
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_includedirs(R .. "include", R .. "src/async")
            if extra_define ~= nil then
                add_defines(extra_define)
            end
            local files = {
                R .. "tests/file_move_borrow_test.cpp",
                R .. "src/file_resource.cpp",
                R .. "src/blocking_file.cpp",
                R .. "src/async/async_io_context.cpp",
                R .. "src/async/threadpool_backend.cpp",
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
                add_tests(name)
            end
    end

    file_move_borrow_target("file_move_borrow_test", false)
    if has_config("liburing") then
        file_move_borrow_target("file_move_borrow_uring_test", true)
    end
    -- The move mutant closes the native descriptor only at the moves the
    -- oracle performs with an outstanding accepted borrow (the suite arms it
    -- around exactly those statements), so files open and accept normally and
    -- the kill lands on post-move observations: deterministically on "second
    -- read accepted through the moved owner", where the closed moved-to owner
    -- rejects before any syscall, and on the pool profile additionally on the
    -- settling-payload arm whenever the worker's pread resolves after the
    -- close. On the ring a pre-submitted op keeps succeeding because the
    -- kernel holds its own file reference, so the payload arm stays
    -- unobservable there by construction while the deterministic arm still
    -- kills.
    file_move_borrow_target("file_move_borrow_mut_move_closes_native", false,
                            "SLUICE_LIFE_MUTANT_MOVE_CLOSES_NATIVE")
    if has_config("liburing") then
        file_move_borrow_target("file_move_borrow_uring_mut_move_closes_native", true,
                                "SLUICE_LIFE_MUTANT_MOVE_CLOSES_NATIVE")
    end
end

-- Concurrent submissions through one context. Same
-- self-contained seam-build shape as the ownership target: the submit pause
-- gates and the core snapshot need the internal-testing define.
do
    local function submit_concurrency_target(name, with_liburing, extra_define)
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
                R .. "tests/submit_concurrency_test.cpp",
                R .. "src/async/async_io_context.cpp",
                R .. "src/async/threadpool_backend.cpp",
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

    submit_concurrency_target("submit_concurrency_test", false)
    if has_config("liburing") then
        submit_concurrency_target("submit_concurrency_uring_test", true)

        -- The unserialized-submit mutation drops the acceptance
        -- serialization in the context submit path and must be killed by the
        -- reserve-window discriminator: the second submitter's arrival at its
        -- submission call is witnessed first, and under the mutation its
        -- reservation parks at the same gate, so the violated snapshot is
        -- stable and cannot be missed by the bounded poll. Executed and
        -- recorded manually, never run as a regular test.
        submit_concurrency_target("submit_concurrency_uring_mut_submit_unserialized", true,
                                  "SLUICE_THREAD01_MUTANT_SUBMIT_UNSERIALIZED")
    end
    submit_concurrency_target("submit_concurrency_mut_submit_unserialized", false,
                              "SLUICE_THREAD01_MUTANT_SUBMIT_UNSERIALIZED")
end

-- RequestScope evidence. Same self-contained seam-build shape as the
-- public request target: the target compiles its own copies of the async TUs it
-- observes and links neither sluice_async nor the other seam builds.
do
    local function request_scope_target(name, test_source, with_liburing, extra_define,
                                        extra_clang_cxxflags)
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
            if extra_clang_cxxflags ~= nil then
                add_cxxflags(extra_clang_cxxflags, {tools = {"clang", "clang_cl"}})
            end
            -- TSan replaces the global allocation functions, so the
            -- suite's strong overrides and the zero-allocation oracle compile
            -- only outside TSan (see the SLUICE_TSAN_TESTING seam guards).
            if is_mode("tsan") then
                add_defines("SLUICE_TSAN_TESTING")
            end
            local files = {
                test_source,
                R .. "src/async/async_io_context.cpp",
                R .. "src/async/request_scope.cpp",
                R .. "src/async/threadpool_backend.cpp",
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

    -- Named mutation builds. Each flips one load-bearing RequestScope rule
    -- and must be killed by the named failing scenario of the killer suite;
    -- they are executed and recorded manually, never run as regular tests.
    request_scope_target("request_scope_mut_reserve_after_accept",
                         R .. "tests/request_scope_test.cpp", false,
                         "SLUICE_REQUEST_SCOPE_MUTANT_RESERVE_AFTER_ACCEPT")
    request_scope_target("request_scope_mut_commit_drops_request",
                         R .. "tests/request_scope_test.cpp", false,
                         "SLUICE_REQUEST_SCOPE_MUTANT_COMMIT_DROPS_REQUEST")
    request_scope_target("request_scope_mut_timeout_releases_slot",
                         R .. "tests/request_scope_test.cpp", false,
                         "SLUICE_REQUEST_SCOPE_MUTANT_TIMEOUT_RELEASES_SLOT")
    -- The destructor mutant removes the TU's only read of policy_, so Clang's
    -- default-on -Wunused-private-field (promoted by -Werror) fails this one
    -- mutation build; the suppression is scoped to it and never reaches GCC
    -- or any other target. The mutant is still killed by its named assertions.
    request_scope_target("request_scope_mut_destructor_skips_settle",
                         R .. "tests/request_scope_test.cpp", false,
                         "SLUICE_REQUEST_SCOPE_MUTANT_DESTRUCTOR_SKIPS_SETTLE",
                         "-Wno-unused-private-field")
    request_scope_target("request_scope_mut_finish_swallows_error",
                         R .. "tests/request_scope_test.cpp", false,
                         "SLUICE_REQUEST_SCOPE_MUTANT_FINISH_SWALLOWS_ERROR")
    request_scope_target("request_scope_mut_finish_cancels_cleanup",
                         R .. "tests/request_scope_test.cpp", false,
                         "SLUICE_REQUEST_SCOPE_MUTANT_FINISH_CANCELS_CLEANUP")
end

-- ThreadPool request-core integration evidence. The deterministic pause gates and fault
-- injections are macro-guarded backend surface, so this target compiles its
-- own copies of the async TUs and links neither sluice_async nor the other
-- seam builds, mirroring the ownership target's shape.
do
    target("threadpool_request_core_integration_test")
        set_kind("binary")
        set_default(false)
        set_group("test")
        add_deps("sluice_core")
        add_includedirs(R .. "include", R .. "src/async")
        add_defines("SLUICE_ASYNC_INTERNAL_TESTING")
        add_files(R .. "tests/threadpool_request_core_integration_test.cpp",
                  R .. "src/async/async_io_context.cpp",
                  R .. "src/async/threadpool_backend.cpp",
                  R .. "src/async/fail_fast.cpp",
                  R .. "src/async/detail/context_identity.cpp",
                  R .. "src/async/detail/request_core.cpp")
        add_tests("threadpool_request_core_integration_test")
end

-- io_uring request-core integration evidence. Same self-contained
-- seam-build shape as the ThreadPool integration target, plus the real
-- liburing backend TU. The
-- deterministic ordering cases inject CQEs through the seam; the real-kernel
-- cases exercise the live ring. Requires the liburing build switch.
if has_config("liburing") then
    local function uring_core_integration_target(name, extra_define)
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
            add_files(R .. "tests/uring_request_core_integration_test.cpp",
                      R .. "src/async/async_io_context.cpp",
                      R .. "src/async/uring_transport.cpp",
                      R .. "src/async/uring_completion.cpp",
                      R .. "src/async/uring_backend.cpp",
                      R .. "src/async/fail_fast.cpp",
                      R .. "src/async/detail/context_identity.cpp",
                      R .. "src/async/detail/request_core.cpp")
            if extra_define == nil then
                add_tests(name)
            end
    end

    uring_core_integration_target("uring_request_core_integration_test", nil)

    -- Named mutation builds. Each flips one load-bearing mechanism and
    -- must be killed by the named failing assertion of the same suite; they
    -- are executed and recorded manually, never run as regular tests.
    uring_core_integration_target("uring_core_integration_mut_cookie_reuse", "SLUICE_URING_CORE_MUTANT_COOKIE_REUSE")
    uring_core_integration_target("uring_core_integration_mut_cancel_cqe_terminal",
                         "SLUICE_URING_CORE_MUTANT_CANCEL_CQE_AS_ORIGINAL_TERMINAL")
    uring_core_integration_target("uring_core_integration_mut_publish_before_retire",
                         "SLUICE_URING_CORE_MUTANT_PUBLISH_BEFORE_BORROW_RETIREMENT")
    uring_core_integration_target("uring_core_integration_mut_drop_outcome",
                         "SLUICE_URING_CORE_MUTANT_DROP_ORIGINAL_OUTCOME")
    uring_core_integration_target("uring_core_integration_mut_remove_control_pin",
                         "SLUICE_URING_CORE_MUTANT_REMOVE_CONTROL_PIN")
    uring_core_integration_target("uring_core_integration_mut_reclaim_before_control",
                         "SLUICE_URING_CORE_MUTANT_RECLAIM_BEFORE_CONTROL_RETIREMENT")
    uring_core_integration_target("uring_core_integration_mut_strand_poison",
                         "SLUICE_URING_CORE_MUTANT_STRAND_POST_ACCEPT_SUBMIT_FAILURE")
    uring_core_integration_target("uring_core_integration_mut_zero_op_dispatch",
                         "SLUICE_URING_CORE_MUTANT_PREMATURE_ZERO_OP_DISPATCH")
end

