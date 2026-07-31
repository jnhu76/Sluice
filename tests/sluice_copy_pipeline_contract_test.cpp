// sluice-copy Version B pipeline contract tests.
//
// These tests describe the expected behavior of the pipelined copy (Version B).
// They are written against the CURRENT production code (Version A) and the
// ScriptedAsyncBackend test infrastructure.
//
// LIFETIME NOTE: The backend is owned by the Runtime (inside the copy thread).
// The test thread MUST read all backend stats BEFORE completing the last
// operation that causes the copy task to publish its result. After the copy
// task publishes, the copy thread destroys the Runtime (and backend).
//
// IMPORTANT: SLUICE_CHECK macros return early from the test function. Any
// running std::thread must be joined before the function returns. Use the
// ScopedThread RAII wrapper to ensure proper cleanup.
//
// Expected results:
//   - Contracts that Version A already satisfies → PASS (green)
//   - Contracts that require Version B → FAIL (red, expected)
//   - Contracts that require a future API → compile-guarded
//
// TEST TARGET: sluice_copy_pipeline_contract_test
// STATUS: NOT in the default `xmake test` group. Run manually:
//   xmake run sluice_copy_pipeline_contract_test

#include "harness.hpp"

#include "copy_task.hpp"

#include "support/scripted_async_backend.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace sluice_copy;
using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {

// RAII helper that joins the thread on destruction (or can be joined early).
// Prevents std::terminate when a test returns early due to SLUICE_CHECK.
struct ScopedThread {
    std::thread t;
    bool joined = false;

    template <typename F>
    explicit ScopedThread(F&& f) : t(std::forward<F>(f)) {}

    ~ScopedThread() {
        if (!joined && t.joinable()) {
            // Drain the backend before joining to unblock the copy thread.
            // We can't access the backend here, so we just detach as a last
            // resort. Actually, we must join. The caller must ensure the
            // backend is drained before this destructor runs.
            t.join();
        }
    }

    void join() {
        if (!joined && t.joinable()) {
            t.join();
            joined = true;
        }
    }

    ScopedThread(const ScopedThread&) = delete;
    ScopedThread& operator=(const ScopedThread&) = delete;
};

struct TempFile {
    int fd;
    TempFile() {
        char p[] = "/tmp/sluice_copy_ptest_XXXXXX";
        fd = ::mkstemp(p);
        SLUICE_CHECK(fd >= 0);
        ::unlink(p);
    }
    ~TempFile() { if (fd >= 0) ::close(fd); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

void seed_file(int fd, std::size_t n) {
    std::vector<std::byte> data(n);
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char b = static_cast<unsigned char>((i * 31 + 7) & 0xFF);
        data[i] = (i % 7 == 0) ? std::byte{0} : std::byte{b};
    }
    if (n > 0) {
        ::pwrite(fd, data.data(), n, 0);
    }
}

void drain_all(ScriptedAsyncBackend* raw) {
    while (raw->pending_count() > 0) {
        auto ops = raw->pending_operations();
        for (auto& op : ops) {
            switch (op.kind) {
            case OpKind::read:
                raw->complete_eof(op.id);
                break;
            case OpKind::write:
                raw->complete_bytes(op.id, op.length);
                break;
            case OpKind::sync_data:
            case OpKind::sync_all:
                raw->complete_sync_success(op.id);
                break;
            }
        }
    }
}

std::uint64_t find_op(ScriptedAsyncBackend* raw, OpKind kind) {
    auto ops = raw->pending_operations();
    for (auto& op : ops) {
        if (op.kind == kind) return op.id;
    }
    return 0;
}

}  // namespace

// ===========================================================================
// Contract 1: depth=1 — single outstanding read (SHOULD PASS with Version A)
// ===========================================================================

SLUICE_TEST_CASE(contract_depth_1_single_outstanding_read) {
    TempFile src, dst;
    seed_file(src.fd, 4096 * 3);

    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();

    std::optional<Result<CopyStats>> copy_result;
    std::atomic<bool> copy_done{false};

    ScopedThread copy_thread([&, be = std::move(backend)]() mutable {
        auto r = run_sequential_copy_with_backend(
            src.fd, dst.fd, 4096, 1, SyncPolicy::none, std::move(be));
        copy_result = r;
        copy_done.store(true, std::memory_order::release);
    });

    // Drive the copy through 3 chunks.
    for (int chunk = 0; chunk < 3; ++chunk) {
        raw->wait_until_pending(1);
        std::uint64_t rid = find_op(raw, OpKind::read);
        SLUICE_CHECK(rid != 0);
        raw->complete_bytes(rid, 4096);

        raw->wait_until_pending(1);
        std::uint64_t wid = find_op(raw, OpKind::write);
        SLUICE_CHECK(wid != 0);
        raw->complete_bytes(wid, 4096);
    }

    // Read stats BEFORE completing the EOF read.
    std::size_t max_reads = raw->max_outstanding_reads();
    std::size_t max_total = raw->max_outstanding_total();

    // Complete the EOF read (triggers copy task completion).
    raw->wait_until_pending(1);
    drain_all(raw);

    // Stats must be read before join because backend dies with Runtime.
    copy_thread.join();

    SLUICE_CHECK(copy_result.has_value());
    SLUICE_CHECK(copy_result->has_value());
    SLUICE_CHECK(copy_result->value().bytes_copied == 4096 * 3);
    SLUICE_CHECK(max_reads == 1);
    SLUICE_CHECK(max_total <= 2);
}

// ===========================================================================
// Contract 2: depth>1 — multiple outstanding reads (EXPECTED FAIL)
// ===========================================================================

SLUICE_TEST_CASE(contract_depth_gt_1_multiple_outstanding_reads) {
    TempFile src, dst;
    seed_file(src.fd, 12);

    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();

    std::optional<Result<CopyStats>> copy_result;
    std::atomic<bool> copy_done{false};

    ScopedThread copy_thread([&, be = std::move(backend)]() mutable {
        auto r = run_sequential_copy_with_backend(
            src.fd, dst.fd, 4, 1, SyncPolicy::none, std::move(be));
        copy_result = r;
        copy_done.store(true, std::memory_order::release);
    });

    // Wait for the first read to appear.
    raw->wait_until_pending(1);

    // Read stats BEFORE completing any operations.
    std::size_t max_reads = raw->max_outstanding_reads();

    // PRIMARY RED-LIGHT assertion. Must drain and join before check because
    // SLUICE_CHECK_MSG returns early.
    drain_all(raw);
    copy_thread.join();

    SLUICE_CHECK_MSG(max_reads >= 2,
                     "Version B pipeline not implemented: "
                     "max_outstanding_reads == 1, expected >= 2");
}

// ===========================================================================
// Contract 3–6, 8–12: guarded (require SLUICE_HAS_PIPELINED_COPY)
// ===========================================================================

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_out_of_order_read_in_order_write) {
    // TODO: When Version B is implemented.
}
#else
SLUICE_TEST_CASE(contract_out_of_order_read_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_slot_lifecycle) {
    // TODO: When Version B is implemented.
}
#else
SLUICE_TEST_CASE(contract_slot_lifecycle_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_short_read_retry) {
    // TODO: When Version B is implemented.
}
#else
SLUICE_TEST_CASE(contract_short_read_retry_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_partial_write_advance) {
    // TODO: When Version B is implemented.
}
#else
SLUICE_TEST_CASE(contract_partial_write_advance_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_eof_drain) {
    // TODO: When Version B is implemented.
}
#else
SLUICE_TEST_CASE(contract_eof_drain_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_read_error_drain) {
    // TODO: When Version B is implemented.
}
#else
SLUICE_TEST_CASE(contract_read_error_drain_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_write_error_drain) {
    // TODO: When Version B is implemented.
}
#else
SLUICE_TEST_CASE(contract_write_error_drain_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_submit_failure_drain) {
    // TODO: When Version B is implemented.
}
#else
SLUICE_TEST_CASE(contract_submit_failure_drain_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_bounded_memory) {
    // TODO: When Version B is implemented.
}
#else
SLUICE_TEST_CASE(contract_bounded_memory_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

// ===========================================================================
// Contract 7: write returns 0 on non-empty write → error
// ===========================================================================

SLUICE_TEST_CASE(contract_write_zero_is_error) {
    TempFile src, dst;
    seed_file(src.fd, 16);

    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();

    std::optional<Result<CopyStats>> copy_result;
    std::atomic<bool> copy_done{false};

    ScopedThread copy_thread([&, be = std::move(backend)]() mutable {
        auto r = run_sequential_copy_with_backend(
            src.fd, dst.fd, 16, 1, SyncPolicy::none, std::move(be));
        copy_result = r;
        copy_done.store(true, std::memory_order::release);
    });

    // Complete the first read.
    raw->wait_until_pending(1);
    std::uint64_t rid = find_op(raw, OpKind::read);
    SLUICE_CHECK(rid != 0);
    raw->complete_bytes(rid, 16);

    // Complete the write with 0 bytes (invalid).
    raw->wait_until_pending(1);
    std::uint64_t wid = find_op(raw, OpKind::write);
    SLUICE_CHECK(wid != 0);
    raw->complete_bytes(wid, 0);

    // Drain any remaining ops (error path should stop submitting).
    drain_all(raw);
    copy_thread.join();

    SLUICE_CHECK(copy_result.has_value());
    SLUICE_CHECK(!copy_result->has_value());
    SLUICE_CHECK(copy_result->error().code == IoError::Code::backend_error);
}

// ===========================================================================
// Contract 13: write submit failure propagates
// ===========================================================================

SLUICE_TEST_CASE(contract_write_submit_failure_propagates) {
    TempFile src, dst;
    seed_file(src.fd, 16);

    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();

    std::optional<Result<CopyStats>> copy_result;
    std::atomic<bool> copy_done{false};

    ScopedThread copy_thread([&, be = std::move(backend)]() mutable {
        auto r = run_sequential_copy_with_backend(
            src.fd, dst.fd, 16, 1, SyncPolicy::none, std::move(be));
        copy_result = r;
        copy_done.store(true, std::memory_order::release);
    });

    // Complete the first read.
    raw->wait_until_pending(1);
    std::uint64_t rid = find_op(raw, OpKind::read);
    SLUICE_CHECK(rid != 0);
    raw->complete_bytes(rid, 16);

    // Inject a write submit failure.
    raw->fail_next_submit(OpKind::write, IoError{IoError::Code::no_space});

    copy_thread.join();
    SLUICE_CHECK(copy_result.has_value());
    SLUICE_CHECK(!copy_result->has_value());
    SLUICE_CHECK(copy_result->error().code == IoError::Code::no_space);
}

// ===========================================================================
// Contract 14: read submit failure propagates
// ===========================================================================

SLUICE_TEST_CASE(contract_read_submit_failure_propagates) {
    TempFile src, dst;
    seed_file(src.fd, 16);

    auto backend = std::make_unique<ScriptedAsyncBackend>();
    auto* raw = backend.get();

    raw->fail_next_submit(OpKind::read, IoError{IoError::Code::backend_error});

    auto r = run_sequential_copy_with_backend(src.fd, dst.fd, 16, 1,
                                               SyncPolicy::none,
                                               std::move(backend));
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::backend_error);
}

SLUICE_MAIN()