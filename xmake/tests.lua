local R = SLUICE_ROOT

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
    local function request_core_target(name, test_source, with_seam)
        target(name)
            set_kind("binary")
            set_default(false)
            set_group("test")
            add_deps("sluice_core")
            add_includedirs(R .. "include")
            if with_seam then
                add_defines("SLUICE_ASYNC_INTERNAL_TESTING")
            end
            add_files(test_source, R .. "src/async/detail/request_core.cpp")
            add_tests(name)
    end

    request_core_target("request_core_protocol_test",
                        R .. "tests/request_core_protocol_test.cpp", true)
    request_core_target("request_core_publication_test",
                        R .. "tests/request_core_publication_test.cpp", true)
    request_core_target("request_core_consumer_probe",
                        R .. "tests/request_core_consumer_probe.cpp", false)
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
                table.insert(files, R .. "src/async/uring_backend.cpp")
            end
            add_files(files)
            add_tests(name)
    end

    context_ownership_target("request_core_ownership_test", false)
    if has_config("liburing") then
        context_ownership_target("request_core_ownership_uring_test", true)
    end
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
                table.insert(files, R .. "src/async/uring_backend.cpp")
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
