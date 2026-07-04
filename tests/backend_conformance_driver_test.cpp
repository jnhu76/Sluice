// Driver: runs the shared conformance suite against every backend
// (sluice-CORE-024, B1). The suite itself lives in backend_conformance_test.cpp;
// this target instantiates it per backend so one `xmake test` run exercises
// all of them. Cases that need a real kernel fd are skipped on non-real_mode
// backends (Fake; Uring stub without liburing).
//
// Backend-specific MECHANISM tests (SQE pressure, ThreadPool concurrency) stay
// in their own files. This file asserts only shared SEMANTIC outcomes.
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
    return BackendFactory{
        "Fake",
        [] { return std::make_unique<FakeAsyncBackend>(); },
        nullptr,                // no real fd
        false                   // not real_mode
    };
}

BackendFactory make_threadpool_factory() {
    return BackendFactory{
        "ThreadPool",
        [] { return std::make_unique<ThreadPoolBackend>(); },
        &make_temp_fd,
        true
    };
}

BackendFactory make_uring_factory() {
#if defined(SLUICE_HAS_LIBURING)
    return BackendFactory{
        "Uring",
        [] { return std::make_unique<UringAsyncBackend>(); },
        &make_temp_fd,
        true
    };
#else
    // Stub mode: UringAsyncBackend compiles but available() is false and
    // submit_* returns backend_error. real_mode=false so fd-backed cases skip
    // cleanly; the suite still asserts the submit->error shape.
    return BackendFactory{
        "Uring(stub)",
        [] { return std::make_unique<UringAsyncBackend>(); },
        nullptr,
        false
    };
#endif
}

}  // namespace

SLUICE_TEST_CASE(conformance_fake) {
    SLUICE_CHECK(sluice_test::conformance::run_conformance(make_fake_factory()) == 0);
}

SLUICE_TEST_CASE(conformance_threadpool) {
    SLUICE_CHECK(sluice_test::conformance::run_conformance(make_threadpool_factory()) == 0);
}

SLUICE_TEST_CASE(conformance_uring) {
    SLUICE_CHECK(sluice_test::conformance::run_conformance(make_uring_factory()) == 0);
}

SLUICE_MAIN()
