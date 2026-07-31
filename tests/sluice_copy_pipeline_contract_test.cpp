// sluice-copy Version B pipeline contract tests.
//
// These tests describe the expected behavior of the pipelined copy (Version B).
// They drive the copy task via the public run_*_copy* entry points against the
// ScriptedAsyncBackend + controller test infrastructure.
//
// LIFETIME MODEL (Phase 0): the backend is owned by the copy thread's Runtime
// (a unique_ptr). The test thread drives EVERYTHING through a
// ScriptedBackendController that shares state with the backend. The controller
// outlives the backend; after the backend is destroyed, waiting returns
// WaitStatus::closed and completion control throws ScriptedBackendClosed —
// there is NEVER a cross-thread raw-pointer dereference (no TOCTOU window).
//
// The CopyScenario RAII harness joins the copy thread, drains pending ops via
// the controller before any assertion can early-return, and never detaches.
//
// RED-LIGHT principle: Version-B-only contracts are written as real scenarios
// that fail for the intended reason (e.g. max_outstanding_reads == 1) until
// Version B is implemented. Once SLUICE_HAS_PIPELINED_COPY is defined at build
// time, the full contract bodies run against the Version B implementation.
//
// TEST TARGET: sluice_copy_pipeline_contract_test
// STATUS: NOT in the default `xmake test` group until Version B is complete.

#include "harness.hpp"

#include "copy_task.hpp"

#include "support/scripted_async_backend.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace sluice_copy;
using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {

// Test timeout guard: bounded waits so a buggy copy cannot hang the test
// forever. A timeout here means a TEST FAILURE, not normal scheduling.
constexpr auto kWaitFor = std::chrono::seconds(5);

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

// Compare two files byte-for-byte (full content, not just size).
// Used by Phase-1 file-content contracts.
[[maybe_unused]] bool files_equal(int a, int b, std::size_t n) {
    std::vector<std::byte> da(n), db(n);
    if (n == 0) return true;
    ssize_t ra = ::pread(a, da.data(), n, 0);
    ssize_t rb = ::pread(b, db.data(), n, 0);
    if (ra != static_cast<ssize_t>(n) || rb != static_cast<ssize_t>(n))
        return false;
    return std::memcmp(da.data(), db.data(), n) == 0;
}

// Find the operation id of a given kind currently outstanding.
std::uint64_t find_op(ScriptedBackendController& ctrl, OpKind kind) {
    auto ops = ctrl.pending_operations();
    for (auto& op : ops)
        if (op.kind == kind) return op.id;
    return 0;
}

// Wait until a (genuinely new) op of `kind` is outstanding, bounded.
//
// Rationale: `outstanding` = pending + staged. After the test completes an op,
// that op remains outstanding (stage=staged) until the Runtime driver polls it.
// A bare "wait until outstanding >= N" can therefore return immediately on the
// just-staged op instead of a new one. Waiting for a specific KIND makes the
// scenario deterministic: we want the next read / the next write, not the
// previous op's staged result. Returns the op id, or 0 on timeout/close.
std::uint64_t wait_for_op(ScriptedBackendController& ctrl, OpKind kind) {
    auto deadline = std::chrono::steady_clock::now() + kWaitFor;
    for (;;) {
        if (std::uint64_t id = find_op(ctrl, kind); id != 0) return id;
        auto ws = ctrl.wait_until_pending_for(1, std::chrono::milliseconds(50));
        if (ws == WaitStatus::closed) return 0;
        if (std::chrono::steady_clock::now() >= deadline) return 0;
    }
}

// Best-effort drain of every outstanding op so the copy thread can reach a
// terminal result and publish. Safe to call repeatedly; no-op after close.
void drain_all(ScriptedBackendController& ctrl) {
    ctrl.complete_all_for_cleanup();
}

// ---------------------------------------------------------------------------
// CopyScenario — unified thread-driven scenario harness.
//
// Owns the controller (shared state with the backend). The copy runs on
// copy_thread; the test thread drives the controller. On destruction (or
// drain_and_join) the harness drains pending ops via the controller and joins
// the thread — no detached threads, no raw backend pointer, no hang on an
// early-return assertion.
// ---------------------------------------------------------------------------
struct CopyScenario {
    ScriptedBackendController controller;
    std::thread copy_thread;
    bool joined = false;

    CopyScenario() = default;
    ~CopyScenario() { drain_and_join(); }

    void drain_and_join() {
        if (joined) return;
        // Best-effort drain so the copy task can publish and the thread exits.
        // If the copy already errored and stopped submitting, this is a no-op.
        drain_all(controller);
        if (copy_thread.joinable()) copy_thread.join();
        joined = true;
    }

    // Wait until >= min ops outstanding, bounded. Returns true if met.
    bool wait_pending(std::size_t min_count) {
        return controller.wait_until_pending_for(min_count, kWaitFor) ==
               WaitStatus::ready;
    }

    // Finish: drain + join. After this, `result` reflects the terminal outcome.
    void finish() { drain_and_join(); }

    CopyScenario(const CopyScenario&) = delete;
    CopyScenario& operator=(const CopyScenario&) = delete;
};

}  // namespace

// ===========================================================================
// Contract 1: depth=1 — single outstanding read (Version A: PASS)
// ===========================================================================

SLUICE_TEST_CASE(contract_depth_1_single_outstanding_read) {
    TempFile src, dst;
    seed_file(src.fd, 4096 * 3);

    auto pair = make_scripted_backend();
    CopyScenario sc;
    sc.controller = pair.controller;

    std::optional<Result<CopyStats>> copy_result;

    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_sequential_copy_with_backend(
            src.fd, dst.fd, 4096, 1, SyncPolicy::none, std::move(pair.backend));
        copy_result = r;
    });

    // Drive the copy through 3 chunks.
    for (int chunk = 0; chunk < 3; ++chunk) {
        std::uint64_t rid = wait_for_op(sc.controller, OpKind::read);
        SLUICE_CHECK_MSG(rid != 0, "timeout waiting for read op in chunk");
        sc.controller.complete_bytes(rid, 4096);

        std::uint64_t wid = wait_for_op(sc.controller, OpKind::write);
        SLUICE_CHECK_MSG(wid != 0, "timeout waiting for write op in chunk");
        sc.controller.complete_bytes(wid, 4096);
    }

    // Read stats BEFORE completing the EOF read.
    std::size_t max_reads = sc.controller.max_outstanding_reads();
    std::size_t max_total = sc.controller.max_outstanding_total();

    // Complete the EOF read (triggers copy task completion).
    std::uint64_t eof_rid = wait_for_op(sc.controller, OpKind::read);
    SLUICE_CHECK_MSG(eof_rid != 0, "timeout waiting for EOF read");
    sc.finish();

    SLUICE_CHECK(copy_result.has_value());
    SLUICE_CHECK(copy_result->has_value());
    SLUICE_CHECK(copy_result->value().bytes_copied == 4096 * 3);
    SLUICE_CHECK(max_reads == 1);
    SLUICE_CHECK(max_total <= 2);
}

// ===========================================================================
// Contract 2: depth>1 — multiple outstanding reads (Version B RED until impl)
// ===========================================================================

SLUICE_TEST_CASE(contract_depth_gt_1_multiple_outstanding_reads) {
    TempFile src, dst;
    seed_file(src.fd, 12);

    auto pair = make_scripted_backend();
    CopyScenario sc;
    sc.controller = pair.controller;

    std::optional<Result<CopyStats>> copy_result;

    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_sequential_copy_with_backend(
            src.fd, dst.fd, 4, 1, SyncPolicy::none, std::move(pair.backend));
        copy_result = r;
    });

    // Wait for the first read to appear (bounded).
    if (wait_for_op(sc.controller, OpKind::read) == 0)
        SLUICE_FAIL("timeout waiting for first read");

    // Read stats BEFORE completing any operations.
    std::size_t max_reads = sc.controller.max_outstanding_reads();

    sc.finish();

    // PRIMARY RED-LIGHT assertion. Version A submits exactly one read at a time,
    // so max_reads == 1. Version B with pipeline_depth>1 must reach >= 2.
    SLUICE_CHECK_MSG(max_reads >= 2,
                     "Version B pipeline not implemented: "
                     "max_outstanding_reads == 1, expected >= 2");
}

// ===========================================================================
// Contracts 3–6, 8–12: guarded (require SLUICE_HAS_PIPELINED_COPY)
// ===========================================================================
//
// These bodies require the Version B API (run_pipelined_copy_with_backend). They
// compile ONLY when SLUICE_HAS_PIPELINED_COPY is defined at build time. Until
// then the `*_not_implemented` placeholders provide a clear red signal that is
// NOT the same as a data race / UAF / deadlock.

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_out_of_order_read_in_order_write) {
    SLUICE_FAIL("contract body not implemented");
}
#else
SLUICE_TEST_CASE(contract_out_of_order_read_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_slot_lifecycle) {
    SLUICE_FAIL("contract body not implemented");
}
#else
SLUICE_TEST_CASE(contract_slot_lifecycle_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_short_read_retry) {
    SLUICE_FAIL("contract body not implemented");
}
#else
SLUICE_TEST_CASE(contract_short_read_retry_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_partial_write_advance) {
    SLUICE_FAIL("contract body not implemented");
}
#else
SLUICE_TEST_CASE(contract_partial_write_advance_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_eof_drain) {
    SLUICE_FAIL("contract body not implemented");
}
#else
SLUICE_TEST_CASE(contract_eof_drain_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_read_error_drain) {
    SLUICE_FAIL("contract body not implemented");
}
#else
SLUICE_TEST_CASE(contract_read_error_drain_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_write_error_drain) {
    SLUICE_FAIL("contract body not implemented");
}
#else
SLUICE_TEST_CASE(contract_write_error_drain_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_submit_failure_drain) {
    SLUICE_FAIL("contract body not implemented");
}
#else
SLUICE_TEST_CASE(contract_submit_failure_drain_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_bounded_memory) {
    SLUICE_FAIL("contract body not implemented");
}
#else
SLUICE_TEST_CASE(contract_bounded_memory_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

// ===========================================================================
// Contract 7: write returns 0 on non-empty write → error (Version A: PASS)
// ===========================================================================

SLUICE_TEST_CASE(contract_write_zero_is_error) {
    TempFile src, dst;
    seed_file(src.fd, 16);

    auto pair = make_scripted_backend();
    CopyScenario sc;
    sc.controller = pair.controller;

    std::optional<Result<CopyStats>> copy_result;

    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_sequential_copy_with_backend(
            src.fd, dst.fd, 16, 1, SyncPolicy::none, std::move(pair.backend));
        copy_result = r;
    });

    // Complete the first read.
    std::uint64_t rid = wait_for_op(sc.controller, OpKind::read);
    SLUICE_CHECK_MSG(rid != 0, "timeout waiting for read");
    sc.controller.complete_bytes(rid, 16);

    // Complete the write with 0 bytes (invalid).
    std::uint64_t wid = wait_for_op(sc.controller, OpKind::write);
    SLUICE_CHECK_MSG(wid != 0, "timeout waiting for write");
    sc.controller.complete_bytes(wid, 0);

    // Drain any remaining ops (error path should stop submitting).
    sc.finish();

    SLUICE_CHECK(copy_result.has_value());
    SLUICE_CHECK(!copy_result->has_value());
    SLUICE_CHECK(copy_result->error().code == IoError::Code::backend_error);
}

// ===========================================================================
// Contract 13: write submit failure propagates (Version A: PASS)
// ===========================================================================

SLUICE_TEST_CASE(contract_write_submit_failure_propagates) {
    TempFile src, dst;
    seed_file(src.fd, 16);

    auto pair = make_scripted_backend();
    CopyScenario sc;
    sc.controller = pair.controller;

    std::optional<Result<CopyStats>> copy_result;

    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_sequential_copy_with_backend(
            src.fd, dst.fd, 16, 1, SyncPolicy::none, std::move(pair.backend));
        copy_result = r;
    });

    // Wait for the first read to appear.
    std::uint64_t rid = wait_for_op(sc.controller, OpKind::read);
    SLUICE_CHECK_MSG(rid != 0, "timeout waiting for read");

    // ARM the write submit failure BEFORE completing the read. This ensures
    // the failure is in place before the Runtime thread polls the read
    // completion and submits the write.
    sc.controller.fail_next_submit(OpKind::write, IoError{IoError::Code::no_space});

    // Now complete the read, allowing the Runtime to proceed to submit write.
    sc.controller.complete_bytes(rid, 16);

    sc.finish();

    SLUICE_CHECK(copy_result.has_value());
    SLUICE_CHECK(!copy_result->has_value());
    SLUICE_CHECK(copy_result->error().code == IoError::Code::no_space);
}

// ===========================================================================
// Contract 14: read submit failure propagates (Version A: PASS)
// ===========================================================================

SLUICE_TEST_CASE(contract_read_submit_failure_propagates) {
    TempFile src, dst;
    seed_file(src.fd, 16);

    auto pair = make_scripted_backend();
    pair.controller.fail_next_submit(OpKind::read,
                                     IoError{IoError::Code::backend_error});

    auto r = run_sequential_copy_with_backend(src.fd, dst.fd, 16, 1,
                                              SyncPolicy::none,
                                              std::move(pair.backend));
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::backend_error);
}

SLUICE_MAIN()
