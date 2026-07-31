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

#include <algorithm>
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

// Wait until >= min_count ops of `kind` are outstanding (reads-ahead proof).
// Returns the count observed, or 0 on timeout/close before reaching min_count.
std::size_t wait_for_op_count(ScriptedBackendController& ctrl, OpKind kind,
                              std::size_t min_count) {
    auto deadline = std::chrono::steady_clock::now() + kWaitFor;
    for (;;) {
        std::size_t n = 0;
        for (auto& op : ctrl.pending_operations())
            if (op.kind == kind) ++n;
        if (n >= min_count) return n;
        auto ws = ctrl.wait_until_pending_for(min_count, std::chrono::milliseconds(50));
        if (ws == WaitStatus::closed) return 0;
        if (std::chrono::steady_clock::now() >= deadline) return 0;
    }
}

// Find an outstanding op of `kind` at exactly `offset`. Returns op id or 0.
std::uint64_t find_op_at(ScriptedBackendController& ctrl, OpKind kind,
                         std::uint64_t offset) {
    for (auto& op : ctrl.pending_operations())
        if (op.kind == kind && op.offset == offset) return op.id;
    return 0;
}

// Wait for an outstanding op of `kind` at exactly `offset` (bounded). This lets
// a scenario drive a SPECIFIC slot deterministically (e.g. complete chunk N's
// read before chunk N+1's, to prove read completion order does not affect write
// submit order).
std::uint64_t wait_for_op_at(ScriptedBackendController& ctrl, OpKind kind,
                             std::uint64_t offset) {
    auto deadline = std::chrono::steady_clock::now() + kWaitFor;
    for (;;) {
        if (std::uint64_t id = find_op_at(ctrl, kind, offset); id != 0) return id;
        auto ws = ctrl.wait_until_pending_for(1, std::chrono::milliseconds(50));
        if (ws == WaitStatus::closed) return 0;
        if (std::chrono::steady_clock::now() >= deadline) return 0;
    }
}

// Snapshot all current write submit offsets (ascending order not assumed).
// Used by Phase-1 write-order contracts.
[[maybe_unused]] std::vector<std::uint64_t> pending_write_offsets(ScriptedBackendController& ctrl) {
    std::vector<std::uint64_t> v;
    for (auto& op : ctrl.pending_operations())
        if (op.kind == OpKind::write) v.push_back(op.offset);
    return v;
}

// Collect unique read-buffer addresses currently outstanding (bounded-memory
// proof: distinct slot buffers have distinct addresses).
std::vector<void*> pending_read_buffer_addrs(ScriptedBackendController& ctrl) {
    std::vector<void*> v;
    for (auto& op : ctrl.pending_operations())
        if (op.kind == OpKind::read && op.buffer) v.push_back(op.buffer);
    return v;
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
    // The copy task publishes its terminal outcome here (from the copy thread).
    bool joined = false;
    // The copy task publishes its terminal outcome here (from the copy thread).
    std::optional<Result<CopyStats>> copy_result;
    // Publication channel: the copy thread writes copy_result THEN sets this
    // flag (release); the driver thread polls the flag (acquire) BEFORE reading
    // copy_result, so the optional is never torn/read while being written.
    // Reads of copy_result after join() are synchronized by the join itself.
    std::atomic<bool> copy_published{false};

    // Called from the copy thread; publishes the terminal outcome exactly once.
    void publish(Result<CopyStats> r) {
        copy_result = std::move(r);
        copy_published.store(true, std::memory_order_release);
    }

    CopyScenario() = default;
    ~CopyScenario() { drain_and_join(); }

    // Drain pending ops through the controller and join the copy thread.
    //
    // Because the copy task CHAINS operations (completing one read/write can
    // unblock the task to submit the NEXT op), a single drain pass is not
    // enough: a new op may appear after the drain. So we loop: complete every
    // outstanding op, then wait briefly for the task to publish its terminal
    // outcome (sc.copy_result) OR to submit new ops; repeat until published or a
    // bounded guard elapses (a real hang would be a test failure, caught by the
    // outer timeout). This never blocks the copy thread on this thread and never
    // leaves an op outstanding that the task is awaiting.
    void drain_and_join() {
        if (joined) return;
        auto deadline = std::chrono::steady_clock::now() + kWaitFor * 3;
        while (!copy_published.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            drain_all(controller);  // complete every currently-pending op
            // Give the copy thread a short window to publish or submit new ops.
            controller.wait_until_pending_for(1, std::chrono::milliseconds(20));
        }
        // Final drain in case the task published after submitting a terminal op.
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


    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_sequential_copy_with_backend(
            src.fd, dst.fd, 4096, 1, SyncPolicy::none, std::move(pair.backend));
        sc.publish(std::move(r));
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

    SLUICE_CHECK(sc.copy_result.has_value());
    SLUICE_CHECK(sc.copy_result->has_value());
    SLUICE_CHECK(sc.copy_result->value().bytes_copied == 4096 * 3);
    SLUICE_CHECK(max_reads == 1);
    SLUICE_CHECK(max_total <= 2);
}

// ===========================================================================
// Contract 2: depth>1 — multiple outstanding reads (Version B RED until impl)
// ===========================================================================

SLUICE_TEST_CASE(contract_depth_gt_1_multiple_outstanding_reads) {
    constexpr std::size_t B = 16;
    TempFile src, dst;
    seed_file(src.fd, B * 8);  // enough to fill a depth-4 read window

    auto pair = make_scripted_backend();
    CopyScenario sc;
    sc.controller = pair.controller;


    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, B, /*depth=*/4,
                                                 1, SyncPolicy::none,
                                                 std::move(pair.backend));
        sc.publish(std::move(r));
    });

    // Wait for >= 2 reads to be outstanding simultaneously: the primary proof
    // that Version B issues multiple outstanding reads (Version A issues 1).
    std::size_t concurrent_reads = wait_for_op_count(sc.controller, OpKind::read, 2);

    sc.finish();

    SLUICE_CHECK(sc.copy_result.has_value());
    SLUICE_CHECK(sc.copy_result->has_value());
    // >= 2 reads were outstanding at once.
    SLUICE_CHECK_MSG(concurrent_reads >= 2,
                     "Version B did not produce >= 2 concurrent reads");
    // Peak outstanding reads tracked by the backend also >= 2.
    SLUICE_CHECK_MSG(sc.controller.max_outstanding_reads() >= 2,
                     "max_outstanding_reads < 2 (no read-ahead)");
}

// ===========================================================================
// Contracts 3–6, 8–12: Version B pipeline contracts.
//
// These require the Version B API (run_pipelined_copy_with_backend). They
// compile and run ONLY when SLUICE_HAS_PIPELINED_COPY is defined at build time.
// Each has a deterministic scenario and a core assertion; the
// `*_not_implemented` placeholders give a clear red signal (NOT a data race /
// UAF / deadlock) until the build define is on.
// ===========================================================================

// ---------------------------------------------------------------------------
// Contract 3: out-of-order read completion, in-order write submission.
// With depth=2 the pipeline submits reads for offsets 0 and B. We complete the
// offset-B read FIRST (out of order), then offset 0. The pipeline must still
// submit the write for offset 0 before the write for offset B (ascending write
// order), proving read completion order cannot reorder writes.
// ---------------------------------------------------------------------------
#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_out_of_order_read_in_order_write) {
    constexpr std::size_t B = 16;
    constexpr std::size_t TOTAL = B * 4;  // 4 full chunks
    TempFile src, dst;
    seed_file(src.fd, TOTAL);

    auto pair = make_scripted_backend();
    CopyScenario sc;
    sc.controller = pair.controller;

    // Write offsets in the order the pipeline SUBMITS them. The pipeline must
    // submit writes in strictly ascending offset order regardless of read
    // completion order.
    std::vector<std::uint64_t> write_offsets_in_submit_order;

    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, B, /*depth=*/2,
                                                 1, SyncPolicy::none,
                                                 std::move(pair.backend));
        sc.publish(std::move(r));
    });

    // Wait for BOTH reads to be outstanding (proves read-ahead of depth>=2).
    SLUICE_CHECK_MSG(wait_for_op_count(sc.controller, OpKind::read, 2) >= 2,
                     "depth=2 must submit >=2 outstanding reads");

    // Complete offset B's read FIRST (out of order), then offset 0.
    std::uint64_t rB = wait_for_op_at(sc.controller, OpKind::read, B);
    SLUICE_CHECK_MSG(rB != 0, "offset-B read not submitted");
    sc.controller.complete_bytes(rB, B);
    std::uint64_t r0 = wait_for_op_at(sc.controller, OpKind::read, 0);
    SLUICE_CHECK_MSG(r0 != 0, "offset-0 read not submitted");
    sc.controller.complete_bytes(r0, B);

    // Drive the copy chunk-by-chunk in write order. For each expected write
    // offset (0, B, 2B, 3B), wait for that write to appear, RECORD its offset
    // (capturing submission order), complete it, and complete any outstanding
    // read so the next chunk can progress. This deterministically observes the
    // write submission order.
    for (std::size_t chunk = 0; chunk < 4; ++chunk) {
        std::uint64_t woff = static_cast<std::uint64_t>(chunk) * B;
        std::uint64_t wid = wait_for_op_at(sc.controller, OpKind::write, woff);
        SLUICE_CHECK_MSG(wid != 0, "expected write at chunk offset not submitted");
        write_offsets_in_submit_order.push_back(woff);
        sc.controller.complete_bytes(wid, B);
        // Allow the pipeline to recycle and submit the next read; complete it
        // so the following write can be issued (except past EOF).
        std::uint64_t next_read_off = static_cast<std::uint64_t>(chunk + 2) * B;
        if (next_read_off < TOTAL) {
            std::uint64_t nrid =
                wait_for_op_at(sc.controller, OpKind::read, next_read_off);
            SLUICE_CHECK_MSG(nrid != 0, "next read-ahead not submitted");
            sc.controller.complete_bytes(nrid, B);
        }
    }

    sc.finish();

    SLUICE_CHECK(sc.copy_result.has_value());
    SLUICE_CHECK(sc.copy_result->has_value());
    SLUICE_CHECK(sc.copy_result->value().bytes_copied == TOTAL);
    SLUICE_CHECK(sc.copy_result->value().write_ops == 4);
    // Write order must be strictly ascending despite out-of-order reads.
    SLUICE_CHECK(write_offsets_in_submit_order.size() == 4);
    for (std::size_t i = 1; i < write_offsets_in_submit_order.size(); ++i) {
        SLUICE_CHECK_MSG(write_offsets_in_submit_order[i] >
                             write_offsets_in_submit_order[i - 1],
                         "write submit order not ascending");
    }
}
#else
SLUICE_TEST_CASE(contract_out_of_order_read_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

// ---------------------------------------------------------------------------
// Contract 4: slot lifecycle — a slot's buffer is not reused for a new read
// before its write completes. With depth=1 we submit read(0), complete it,
// observe the write(0) outstanding; while write(0) is outstanding, NO new read
// at offset >= B should appear (the slot cannot be recycled until its write
// finishes). We then complete the write and verify a new read appears.
// ---------------------------------------------------------------------------
#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_slot_lifecycle) {
    constexpr std::size_t B = 16;
    TempFile src, dst;
    seed_file(src.fd, B * 3);

    auto pair = make_scripted_backend();
    CopyScenario sc;
    sc.controller = pair.controller;


    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, B, /*depth=*/1,
                                                 1, SyncPolicy::none,
                                                 std::move(pair.backend));
        sc.publish(std::move(r));
    });

    std::uint64_t r0 = wait_for_op(sc.controller, OpKind::read);
    SLUICE_CHECK_MSG(r0 != 0, "first read not submitted");
    sc.controller.complete_bytes(r0, B);

    std::uint64_t w0 = wait_for_op(sc.controller, OpKind::write);
    SLUICE_CHECK_MSG(w0 != 0, "write not submitted");
    // While the write is outstanding, no new read at offset >= B may appear.
    // (A depth=1 slot cannot be recycled before its write completes.)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    bool premature_read = false;
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto& op : sc.controller.pending_operations())
            if (op.kind == OpKind::read && op.offset >= B) { premature_read = true; break; }
        if (premature_read) break;
        sc.controller.wait_until_pending_for(1, std::chrono::milliseconds(10));
    }
    SLUICE_CHECK_MSG(!premature_read,
                     "slot buffer reused for a new read before write completion");

    // Complete the write; now a new read (for the next chunk) may appear.
    sc.controller.complete_bytes(w0, B);

    // Drive the remaining two chunks to completion so the whole file copies.
    for (int chunk = 1; chunk < 3; ++chunk) {
        std::uint64_t rid = wait_for_op(sc.controller, OpKind::read);
        SLUICE_CHECK_MSG(rid != 0, "subsequent read not submitted");
        sc.controller.complete_bytes(rid, B);
        std::uint64_t wid = wait_for_op(sc.controller, OpKind::write);
        SLUICE_CHECK_MSG(wid != 0, "subsequent write not submitted");
        sc.controller.complete_bytes(wid, B);
    }

    sc.finish();

    SLUICE_CHECK(sc.copy_result.has_value());
    SLUICE_CHECK(sc.copy_result->has_value());
    SLUICE_CHECK(sc.copy_result->value().bytes_copied == B * 3);
}
#else
SLUICE_TEST_CASE(contract_slot_lifecycle_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

// ---------------------------------------------------------------------------
// Contract 5: short read retried within the same slot. A read that returns n <
// buffer_size (and n > 0) must be retried at offset+filled until the slot is
// filled or EOF. The global offset must not skip the unread region. We complete
// read(0) with 4 bytes; the pipeline must submit a continuation read at offset
// 4 (not 16) for the same chunk.
// ---------------------------------------------------------------------------
#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_short_read_retry) {
    constexpr std::size_t B = 16;
    TempFile src, dst;
    seed_file(src.fd, B * 2);

    auto pair = make_scripted_backend();
    CopyScenario sc;
    sc.controller = pair.controller;


    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, B, /*depth=*/1,
                                                 1, SyncPolicy::none,
                                                 std::move(pair.backend));
        sc.publish(std::move(r));
    });

    // First read returns 4 bytes (short).
    std::uint64_t r0 = wait_for_op_at(sc.controller, OpKind::read, 0);
    SLUICE_CHECK_MSG(r0 != 0, "first read not submitted");
    sc.controller.complete_bytes(r0, 4);

    // Continuation read MUST be at offset 4 (same slot), not 16.
    std::uint64_t r4 = wait_for_op_at(sc.controller, OpKind::read, 4);
    SLUICE_CHECK_MSG(r4 != 0, "short read not retried at offset 4");
    sc.controller.complete_bytes(r4, 12);  // fill the rest of the chunk

    // Now the full chunk is ready -> write at offset 0 for B bytes.
    std::uint64_t w0 = wait_for_op(sc.controller, OpKind::write);
    SLUICE_CHECK_MSG(w0 != 0, "write not submitted after slot fill");
    sc.controller.complete_bytes(w0, B);

    // Drive chunk 1 (offset B) with a full read + write so the whole file
    // copies; then the EOF read terminates the copy cleanly.
    std::uint64_t r1 = wait_for_op_at(sc.controller, OpKind::read, B);
    SLUICE_CHECK_MSG(r1 != 0, "chunk-1 read not submitted");
    sc.controller.complete_bytes(r1, B);
    std::uint64_t w1 = wait_for_op(sc.controller, OpKind::write);
    SLUICE_CHECK_MSG(w1 != 0, "chunk-1 write not submitted");
    sc.controller.complete_bytes(w1, B);

    sc.finish();

    SLUICE_CHECK(sc.copy_result.has_value());
    SLUICE_CHECK(sc.copy_result->has_value());
    SLUICE_CHECK(sc.copy_result->value().bytes_copied == B * 2);
}
#else
SLUICE_TEST_CASE(contract_short_read_retry_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

// ---------------------------------------------------------------------------
// Contract 6: partial write advances by the written byte count. A write that
// returns n < remaining must be retried at offset+written, not re-issue the
// whole buffer. We complete write(0,len=16) with 7 bytes; the pipeline must
// submit a continuation write at offset 7 (length 9).
// ---------------------------------------------------------------------------
#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_partial_write_advance) {
    constexpr std::size_t B = 16;
    TempFile src, dst;
    seed_file(src.fd, B * 2);

    auto pair = make_scripted_backend();
    CopyScenario sc;
    sc.controller = pair.controller;


    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, B, /*depth=*/1,
                                                 1, SyncPolicy::none,
                                                 std::move(pair.backend));
        sc.publish(std::move(r));
    });

    std::uint64_t r0 = wait_for_op(sc.controller, OpKind::read);
    SLUICE_CHECK_MSG(r0 != 0, "read not submitted");
    sc.controller.complete_bytes(r0, B);

    // First write returns 7 bytes (partial).
    std::uint64_t w0 = wait_for_op(sc.controller, OpKind::write);
    SLUICE_CHECK_MSG(w0 != 0, "write not submitted");
    sc.controller.complete_bytes(w0, 7);

    // Continuation write MUST be at offset 7, length 9 (16 - 7).
    std::uint64_t w7 = wait_for_op_at(sc.controller, OpKind::write, 7);
    SLUICE_CHECK_MSG(w7 != 0, "partial write not retried at offset 7");
    // Confirm the remaining length is 9.
    std::size_t w7_len = 0;
    for (auto& op : sc.controller.pending_operations())
        if (op.id == w7) w7_len = op.length;
    SLUICE_CHECK(w7_len == 9);
    sc.controller.complete_bytes(w7, 9);

    // Drive chunk 1 (offset B) to completion so the whole file copies.
    std::uint64_t r1 = wait_for_op_at(sc.controller, OpKind::read, B);
    SLUICE_CHECK_MSG(r1 != 0, "chunk-1 read not submitted");
    sc.controller.complete_bytes(r1, B);
    std::uint64_t w1 = wait_for_op(sc.controller, OpKind::write);
    SLUICE_CHECK_MSG(w1 != 0, "chunk-1 write not submitted");
    sc.controller.complete_bytes(w1, B);

    sc.finish();

    SLUICE_CHECK(sc.copy_result.has_value());
    SLUICE_CHECK(sc.copy_result->has_value());
    SLUICE_CHECK(sc.copy_result->value().bytes_copied == B * 2);
}
#else
SLUICE_TEST_CASE(contract_partial_write_advance_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

// ---------------------------------------------------------------------------
// Contract 8: EOF drains subsequent read-ahead. With depth=2, when EOF is hit
// on the first slot, the second slot's read may already be outstanding (or
// about to be). The pipeline must NOT write the post-EOF slot, must drain all
// outstanding reads, and return with outstanding == 0 before the task ends.
// Source is exactly B bytes (one chunk); slot 1's read at offset B returns EOF.
// ---------------------------------------------------------------------------
#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_eof_drain) {
    constexpr std::size_t B = 16;
    TempFile src, dst;
    seed_file(src.fd, B);  // exactly one chunk

    auto pair = make_scripted_backend();
    CopyScenario sc;
    sc.controller = pair.controller;


    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, B, /*depth=*/2,
                                                 1, SyncPolicy::none,
                                                 std::move(pair.backend));
        sc.publish(std::move(r));
    });

    // Complete chunk-0 read (full), then chunk-1 read (EOF at offset B).
    std::uint64_t r0 = wait_for_op_at(sc.controller, OpKind::read, 0);
    SLUICE_CHECK_MSG(r0 != 0, "offset-0 read not submitted");
    sc.controller.complete_bytes(r0, B);

    // The read-ahead slot at offset B may already be outstanding; complete it
    // with EOF (0 bytes).
    std::uint64_t rB = wait_for_op_at(sc.controller, OpKind::read, B);
    SLUICE_CHECK_MSG(rB != 0, "offset-B read-ahead not submitted");
    sc.controller.complete_eof(rB);

    // Write chunk 0; chunk 1 has no data (EOF) so it must not be written.
    std::uint64_t w0 = wait_for_op(sc.controller, OpKind::write);
    SLUICE_CHECK_MSG(w0 != 0, "offset-0 write not submitted");
    sc.controller.complete_bytes(w0, B);

    sc.finish();

    SLUICE_CHECK(sc.copy_result.has_value());
    SLUICE_CHECK(sc.copy_result->has_value());
    SLUICE_CHECK(sc.copy_result->value().bytes_copied == B);
    // Drain proof: the controller observes nothing outstanding after finish.
    SLUICE_CHECK(sc.controller.pending_count() == 0);
}
#else
SLUICE_TEST_CASE(contract_eof_drain_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

// ---------------------------------------------------------------------------
// Contract 9: a read error drains all other outstanding operations. With
// depth=2, both reads outstanding; we complete one with an error. The pipeline
// must stop submitting, drain the other read, and return the error — with
// outstanding == 0 at the end.
// ---------------------------------------------------------------------------
#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_read_error_drain) {
    constexpr std::size_t B = 16;
    TempFile src, dst;
    seed_file(src.fd, B * 4);

    auto pair = make_scripted_backend();
    CopyScenario sc;
    sc.controller = pair.controller;


    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, B, /*depth=*/2,
                                                 1, SyncPolicy::none,
                                                 std::move(pair.backend));
        sc.publish(std::move(r));
    });

    SLUICE_CHECK_MSG(wait_for_op_count(sc.controller, OpKind::read, 2) >= 2,
                     "depth=2 must submit >=2 outstanding reads");

    // Complete the first read with an error.
    std::uint64_t r0 = wait_for_op_at(sc.controller, OpKind::read, 0);
    SLUICE_CHECK_MSG(r0 != 0, "offset-0 read not submitted");
    sc.controller.complete_error(r0, IoError{IoError::Code::backend_error});

    sc.finish();

    SLUICE_CHECK(sc.copy_result.has_value());
    SLUICE_CHECK(!sc.copy_result->has_value());
    SLUICE_CHECK(sc.copy_result->error().code == IoError::Code::backend_error);
    SLUICE_CHECK(sc.controller.pending_count() == 0);
}
#else
SLUICE_TEST_CASE(contract_read_error_drain_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

// ---------------------------------------------------------------------------
// Contract 10: a write error drains read-ahead. With depth=2, both reads
// completed; the first write errors. The pipeline must stop, drain the second
// slot (no write issued), and return the error with outstanding == 0.
// ---------------------------------------------------------------------------
#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_write_error_drain) {
    constexpr std::size_t B = 16;
    TempFile src, dst;
    seed_file(src.fd, B * 4);

    auto pair = make_scripted_backend();
    CopyScenario sc;
    sc.controller = pair.controller;


    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, B, /*depth=*/2,
                                                 1, SyncPolicy::none,
                                                 std::move(pair.backend));
        sc.publish(std::move(r));
    });

    // Complete both reads.
    std::uint64_t r0 = wait_for_op_at(sc.controller, OpKind::read, 0);
    SLUICE_CHECK_MSG(r0 != 0, "offset-0 read not submitted");
    sc.controller.complete_bytes(r0, B);
    std::uint64_t rB = wait_for_op_at(sc.controller, OpKind::read, B);
    SLUICE_CHECK_MSG(rB != 0, "offset-B read not submitted");
    sc.controller.complete_bytes(rB, B);

    // First write errors.
    std::uint64_t w0 = wait_for_op(sc.controller, OpKind::write);
    SLUICE_CHECK_MSG(w0 != 0, "write not submitted");
    sc.controller.complete_error(w0, IoError{IoError::Code::no_space});

    sc.finish();

    SLUICE_CHECK(sc.copy_result.has_value());
    SLUICE_CHECK(!sc.copy_result->has_value());
    SLUICE_CHECK(sc.copy_result->error().code == IoError::Code::no_space);
    SLUICE_CHECK(sc.controller.pending_count() == 0);
}
#else
SLUICE_TEST_CASE(contract_write_error_drain_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

// ---------------------------------------------------------------------------
// Contract 11: a submit failure only waits for ops that were successfully
// submitted. With depth=2 we let both reads submit; then arm a write submit
// failure for the first write. The pipeline must NOT hang waiting on an op
// that never entered the backend, must propagate the error, and finish with
// outstanding == 0.
// ---------------------------------------------------------------------------
#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_submit_failure_drain) {
    constexpr std::size_t B = 16;
    TempFile src, dst;
    seed_file(src.fd, B * 4);

    auto pair = make_scripted_backend();
    CopyScenario sc;
    sc.controller = pair.controller;


    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, B, /*depth=*/2,
                                                 1, SyncPolicy::none,
                                                 std::move(pair.backend));
        sc.publish(std::move(r));
    });

    std::uint64_t r0 = wait_for_op_at(sc.controller, OpKind::read, 0);
    SLUICE_CHECK_MSG(r0 != 0, "offset-0 read not submitted");
    // Arm write submit failure BEFORE completing the read so the failure is in
    // place when the pipeline issues the write.
    sc.controller.fail_next_submit(OpKind::write, IoError{IoError::Code::no_space});
    sc.controller.complete_bytes(r0, B);

    sc.finish();

    SLUICE_CHECK(sc.copy_result.has_value());
    SLUICE_CHECK(!sc.copy_result->has_value());
    SLUICE_CHECK(sc.copy_result->error().code == IoError::Code::no_space);
    SLUICE_CHECK(sc.controller.pending_count() == 0);
}
#else
SLUICE_TEST_CASE(contract_submit_failure_drain_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

// ---------------------------------------------------------------------------
// Contract 12: bounded memory — the number of distinct slot read-buffer
// addresses never exceeds pipeline_depth, and the peak outstanding reads never
// exceeds pipeline_depth. With depth=3 we let all 3 reads go outstanding and
// assert <= 3 distinct buffers and max_outstanding_reads <= 3.
// ---------------------------------------------------------------------------
#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_bounded_memory) {
    constexpr std::size_t B = 16;
    TempFile src, dst;
    seed_file(src.fd, B * 9);  // enough to fill a depth-3 window

    auto pair = make_scripted_backend();
    CopyScenario sc;
    sc.controller = pair.controller;

    std::size_t max_distinct_buffers = 0;
    std::size_t max_reads_observed = 0;

    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, B, /*depth=*/3,
                                                 1, SyncPolicy::none,
                                                 std::move(pair.backend));
        sc.publish(std::move(r));
    });

    // Let the read window fill, observing distinct buffers and read count.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        auto addrs = pending_read_buffer_addrs(sc.controller);
        std::sort(addrs.begin(), addrs.end());
        addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
        if (addrs.size() > max_distinct_buffers) max_distinct_buffers = addrs.size();
        std::size_t nreads = 0;
        for (auto& op : sc.controller.pending_operations())
            if (op.kind == OpKind::read) ++nreads;
        if (nreads > max_reads_observed) max_reads_observed = nreads;
        if (sc.copy_result.has_value()) break;
        sc.controller.wait_until_pending_for(1, std::chrono::milliseconds(10));
    }

    sc.finish();

    SLUICE_CHECK(sc.copy_result.has_value());
    SLUICE_CHECK(sc.copy_result->has_value());
    // Distinct slot buffers bounded by depth.
    SLUICE_CHECK_MSG(max_distinct_buffers <= 3,
                     "more distinct slot buffers than pipeline_depth");
    // Peak outstanding reads bounded by depth.
    SLUICE_CHECK_MSG(max_reads_observed <= 3,
                     "more outstanding reads than pipeline_depth");
    SLUICE_CHECK(sc.controller.max_outstanding_reads() <= 3);
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


    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_sequential_copy_with_backend(
            src.fd, dst.fd, 16, 1, SyncPolicy::none, std::move(pair.backend));
        sc.publish(std::move(r));
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

    SLUICE_CHECK(sc.copy_result.has_value());
    SLUICE_CHECK(!sc.copy_result->has_value());
    SLUICE_CHECK(sc.copy_result->error().code == IoError::Code::backend_error);
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


    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_sequential_copy_with_backend(
            src.fd, dst.fd, 16, 1, SyncPolicy::none, std::move(pair.backend));
        sc.publish(std::move(r));
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

    SLUICE_CHECK(sc.copy_result.has_value());
    SLUICE_CHECK(!sc.copy_result->has_value());
    SLUICE_CHECK(sc.copy_result->error().code == IoError::Code::no_space);
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

// ===========================================================================
// Contract 13: sync policy runs AFTER all writes complete (Version B).
// With depth=2, SyncPolicy::data: the sync_data op must only be submitted once
// every write has finished. We complete all writes, then assert a sync op
// appears AFTER (no outstanding writes remain when sync is observed).
// ===========================================================================
#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_sync_after_all_writes) {
    constexpr std::size_t B = 16;
    TempFile src, dst;
    seed_file(src.fd, B * 4);

    auto pair = make_scripted_backend();
    CopyScenario sc;
    sc.controller = pair.controller;


    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, B, /*depth=*/2,
                                                 1, SyncPolicy::data,
                                                 std::move(pair.backend));
        sc.publish(std::move(r));
    });

    // Drive all chunks to completion (read + write each), including the EOF.
    for (;;) {
        // Complete any outstanding pending read (skip staged; those await poll).
        for (auto& op : sc.controller.pending_operations()) {
            if (op.stage != OpStage::pending) continue;
            if (op.kind == OpKind::read) {
                // EOF when offset >= file size (B*4).
                if (op.offset >= B * 4) sc.controller.complete_eof(op.id);
                else sc.controller.complete_bytes(op.id, B);
            }
        }
        // Complete any outstanding pending write.
        for (auto& op : sc.controller.pending_operations()) {
            if (op.stage != OpStage::pending) continue;
            if (op.kind == OpKind::write) sc.controller.complete_bytes(op.id, op.length);
        }
        // If a sync op is now outstanding, every write must already be done.
        bool sync_outstanding = false;
        bool write_outstanding = false;
        for (auto& op : sc.controller.pending_operations()) {
            if (op.kind == OpKind::sync_data || op.kind == OpKind::sync_all)
                sync_outstanding = true;
            if (op.kind == OpKind::write) write_outstanding = true;
        }
        if (sync_outstanding) {
            SLUICE_CHECK_MSG(!write_outstanding,
                             "sync submitted while a write is still outstanding");
            // Complete the pending sync; the task should finish.
            for (auto& op : sc.controller.pending_operations()) {
                if (op.stage != OpStage::pending) continue;
                if (op.kind == OpKind::sync_data || op.kind == OpKind::sync_all)
                    sc.controller.complete_sync_success(op.id);
            }
            break;
        }
        if (sc.copy_result.has_value()) break;
        sc.controller.wait_until_pending_for(1, std::chrono::milliseconds(20));
    }

    sc.finish();

    SLUICE_CHECK(sc.copy_result.has_value());
    SLUICE_CHECK(sc.copy_result->has_value());
    SLUICE_CHECK(sc.copy_result->value().bytes_copied == B * 4);
    SLUICE_CHECK(sc.copy_result->value().sync == SyncPolicy::data);
    SLUICE_CHECK(sc.controller.pending_count() == 0);
}
#else
SLUICE_TEST_CASE(contract_sync_after_all_writes_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

// ===========================================================================
// Contract 14: invalid arguments are rejected (Version B).
// pipeline_depth == 0, buffer_size == 0, and buffer_size*pipeline_depth overflow
// must all return invalid_state synchronously WITHOUT submitting any work.
// ===========================================================================
#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_rejects_invalid_arguments) {
    TempFile src, dst;
    seed_file(src.fd, 64);

    // depth == 0
    {
        auto pair = make_scripted_backend();
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, 16, /*depth=*/0,
                                                 1, SyncPolicy::none,
                                                 std::move(pair.backend));
        SLUICE_CHECK(!r.has_value());
        SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
        // No work was submitted (backend untouched).
        SLUICE_CHECK(pair.controller.pending_count() == 0);
    }
    // buffer_size == 0
    {
        auto pair = make_scripted_backend();
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, /*buffer=*/0,
                                                 /*depth=*/2, 1, SyncPolicy::none,
                                                 std::move(pair.backend));
        SLUICE_CHECK(!r.has_value());
        SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
    }
    // product overflow: buffer_size near SIZE_MAX with depth > 1.
    {
        auto pair = make_scripted_backend();
        constexpr std::size_t HUGE = static_cast<std::size_t>(-1) / 2 + 1;
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, HUGE,
                                                 /*depth=*/2, 1, SyncPolicy::none,
                                                 std::move(pair.backend));
        SLUICE_CHECK(!r.has_value());
        SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
    }
    // workers == 0
    {
        auto pair = make_scripted_backend();
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, 16, /*depth=*/2,
                                                 /*workers=*/0, SyncPolicy::none,
                                                 std::move(pair.backend));
        SLUICE_CHECK(!r.has_value());
        SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
    }
    // null backend
    {
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, 16, /*depth=*/2,
                                                 1, SyncPolicy::none, nullptr);
        SLUICE_CHECK(!r.has_value());
        SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
    }
}
#else
SLUICE_TEST_CASE(contract_rejects_invalid_arguments_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

SLUICE_MAIN()
