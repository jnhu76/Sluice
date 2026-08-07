// Driver: runs the shared conformance suite against every backend
// (sluice-CORE-024, B1). The suite itself lives in backend_conformance_test.cpp;
// this target instantiates it per backend so one `xmake test` run exercises
// all of them. Cases that need a real kernel fd are skipped on non-real_mode
// backends (Fake; Uring stub without liburing).
//
// Backend-specific MECHANISM tests (SQE pressure, ThreadPool concurrency) stay
// in their own files. This file asserts only shared SEMANTIC outcomes.
//
// Phase C1: each registered backend emits a stable machine-readable
// [conformance-meta] line BEFORE its case runs, declaring its profile and
// mode. The aggregate gate (scripts/verify-backend-conformance.py) parses
// ONLY these meta lines to classify backends — it does NOT infer mode from
// display names, skip text, or build-directory contents. The existing
// BackendFactory::real_mode field is retained (it still controls which cases
// the shared suite skips); the meta line is the higher-level classification
// surface added by C1.
#include "backend_conformance.hpp"
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/async/uring_backend.hpp>

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <string>

#if defined(SLUICE_HAS_LIBURING)
#include <liburing.h>
#endif

using namespace sluice::async;
using sluice_test::conformance::BackendFactory;

namespace {

long& temp_counter() { static long c = 0; return c; }

// Open a fresh temp file and return its fd. Used by real_mode factories.
int make_temp_fd() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("sluice_conf_" + std::to_string(temp_counter()++) + ".tmp");
    int fd = ::open(path.string().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        // Unlink so the filesystem cleans up on close; the fd stays valid.
        std::filesystem::remove(path);
    }
    return fd;
}

BackendFactory make_fake_factory() {
    BackendFactory f;
    f.name = "Fake";
    f.make_backend = [] { return std::make_unique<FakeAsyncBackend>(); };
    // Phase C2a: Fake supports bounded construction via FakeAsyncBackend(cap).
    // The zero-arg make_backend above uses the default capacity so the existing
    // 8 shared cases are unchanged; the capacity cases build a small arena here.
    f.make_backend_with_capacity = [](std::size_t cap) {
        return std::make_unique<FakeAsyncBackend>(cap);
    };
    f.make_temp_fd = nullptr;   // no real fd
    f.real_mode = false;        // not real_mode
    f.profile = "ReferenceProfile";   // Phase C1 profile
    f.mode = "deterministic";         // Phase C1 mode
    return f;
}

BackendFactory make_threadpool_factory() {
    BackendFactory f;
    f.name = "ThreadPool";
    f.make_backend = [] { return std::make_unique<ThreadPoolBackend>(); };
    // Phase C2a: ThreadPool supports bounded construction via ThreadPoolConfig.
    // worker_count=1 because capacity is the variable under test (a larger pool
    // would not change the arena bound, and one worker is enough to drive
    // cancel/reap/reset through the real syscall path).
    f.make_backend_with_capacity = [](std::size_t cap) {
        ThreadPoolConfig cfg;
        cfg.request_capacity = cap;
        cfg.worker_count = 1;
        return std::make_unique<ThreadPoolBackend>(cfg);
    };
    f.make_temp_fd = &make_temp_fd;
    f.real_mode = true;
    f.profile = "BlockingIoProfile";   // Phase C1 profile
    f.mode = "real";                   // Phase C1 mode
    return f;
}

BackendFactory make_uring_factory() {
    BackendFactory f;
#if defined(SLUICE_HAS_LIBURING)
    f.name = "Uring";
    f.make_backend = [] { return std::make_unique<UringAsyncBackend>(); };
    f.make_temp_fd = &make_temp_fd;
    f.real_mode = true;
    f.profile = "KernelIoProfile";     // Phase C1 profile
    f.mode = "real";                   // Phase C1 mode
#else
    // Stub mode: UringAsyncBackend compiles but available() is false and
    // submit_* returns backend_error. real_mode=false so fd-backed cases skip
    // cleanly; the suite still asserts the submit->error shape. The meta line
    // declares mode=stub so the aggregate gate classifies the KernelIo profile
    // as NOT CONFORMING (kernel coverage INCOMPLETE) without parsing names.
    f.name = "Uring(stub)";
    f.make_backend = [] { return std::make_unique<UringAsyncBackend>(); };
    f.make_temp_fd = nullptr;
    f.real_mode = false;
    f.profile = "KernelIoProfile";     // Phase C1 profile
    f.mode = "stub";                   // Phase C1 mode
#endif
    // Phase C2a: Uring has NOT migrated onto RequestArena (Phase D pending), so
    // make_backend_with_capacity stays null. The capacity cases do not execute
    // for Uring's driver, and the authoritative gap is the manifest's
    // uring_capacity_not_implemented record (added in commit 2). Uring is never
    // skip-as-pass for capacity.
    return f;
}

}  // namespace

SLUICE_TEST_CASE(conformance_fake) {
    const auto f = make_fake_factory();
    sluice_test::conformance::emit_meta(f);
    SLUICE_CHECK(sluice_test::conformance::run_conformance(f) == 0);
}

SLUICE_TEST_CASE(conformance_threadpool) {
    const auto f = make_threadpool_factory();
    sluice_test::conformance::emit_meta(f);
    SLUICE_CHECK(sluice_test::conformance::run_conformance(f) == 0);
}

SLUICE_TEST_CASE(conformance_uring) {
    const auto f = make_uring_factory();
    sluice_test::conformance::emit_meta(f);
    SLUICE_CHECK(sluice_test::conformance::run_conformance(f) == 0);
}

// Phase C2a — shared capacity/admission/rejection/accounting cases, driven
// per-backend. Each runs run_capacity_cases() against a backend built at a
// chosen small request_capacity via the factory's make_backend_with_capacity
// seam. The cases assert ONLY AsyncIoContext-observable state. Uring has no
// capacity seam (Phase D pending), so it has NO capacity driver case here —
// the authoritative gap is the manifest's uring_capacity_not_implemented
// record. The aggregate gate drives these per-backend in isolated subprocesses.
SLUICE_TEST_CASE(conformance_capacity_fake) {
    const auto f = make_fake_factory();
    sluice_test::conformance::emit_meta(f);
    SLUICE_CHECK(sluice_test::conformance::factory_supports_capacity(f));
    const std::string failed = sluice_test::conformance::run_capacity_cases(f);
    if (!failed.empty()) {
        std::fprintf(stderr, "[conformance] capacity FAIL Fake :: %s\n",
                     failed.c_str());
    }
    SLUICE_CHECK(failed.empty());
}

SLUICE_TEST_CASE(conformance_capacity_threadpool) {
    const auto f = make_threadpool_factory();
    sluice_test::conformance::emit_meta(f);
    SLUICE_CHECK(sluice_test::conformance::factory_supports_capacity(f));
    const std::string failed = sluice_test::conformance::run_capacity_cases(f);
    if (!failed.empty()) {
        std::fprintf(stderr, "[conformance] capacity FAIL ThreadPool :: %s\n",
                     failed.c_str());
    }
    SLUICE_CHECK(failed.empty());
}

// Phase C2e — shared close/drain/destruction cases (Issue #68 rows 15-16),
// driven per-backend. The DRIVER builds the backend at a small capacity and
// wires the two instance-level seams (AGENTS.md §15) as closures over the
// concrete backend's PUBLIC close_admission() and arena_slot_in_use() — the
// suite itself stays backend-agnostic. Each run asserts ONLY the shared
// boundary; the aggregate gate drives these per-backend in isolated
// subprocesses (conformance_close_drain_fake / conformance_close_drain_threadpool).
// Uring has no close_admission before Phase D — its gap is the manifest's
// uring_c2e_close_drain_not_implemented record (never skip-as-pass).
SLUICE_TEST_CASE(conformance_close_drain_fake) {
    const auto f = make_fake_factory();
    sluice_test::conformance::emit_meta(f);
    // Per-case fixture factory: each close/drain case builds a FRESH backend
    // (close_admission is irreversible) and the closures bind to that backend's
    // PUBLIC close_admission() / arena_slot_in_use(). The suite stays
    // backend-agnostic; these are instance-level test seams (AGENTS.md §15).
    const sluice_test::conformance::MakeCloseDrainFixture make_fx = [] {
        auto backend = std::make_unique<FakeAsyncBackend>(4);
        auto* raw = backend.get();
        sluice_test::conformance::CloseDrainFixture fx(std::move(backend));
        fx.close = [raw] { raw->close_admission(); };
        fx.slot_in_use = [raw] { return raw->arena_slot_in_use(); };
        return fx;
    };
    const std::string failed =
        sluice_test::conformance::run_close_drain_cases(f, make_fx);
    if (!failed.empty()) {
        std::fprintf(stderr, "[conformance] close/drain FAIL Fake :: %s\n",
                     failed.c_str());
    }
    SLUICE_CHECK(failed.empty());
}

SLUICE_TEST_CASE(conformance_close_drain_threadpool) {
    const auto f = make_threadpool_factory();
    sluice_test::conformance::emit_meta(f);
    const sluice_test::conformance::MakeCloseDrainFixture make_fx = [] {
        ThreadPoolConfig cfg;
        cfg.request_capacity = 4;
        cfg.worker_count = 1;
        auto backend = std::make_unique<ThreadPoolBackend>(cfg);
        auto* raw = backend.get();
        sluice_test::conformance::CloseDrainFixture fx(std::move(backend));
        fx.close = [raw] { raw->close_admission(); };
        fx.slot_in_use = [raw] { return raw->arena_slot_in_use(); };
        return fx;
    };
    const std::string failed =
        sluice_test::conformance::run_close_drain_cases(f, make_fx);
    if (!failed.empty()) {
        std::fprintf(stderr, "[conformance] close/drain FAIL ThreadPool :: %s\n",
                     failed.c_str());
    }
    SLUICE_CHECK(failed.empty());
}

SLUICE_MAIN()
