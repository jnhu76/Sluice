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
// BOUNDED-FAILURE CONTRACT: the CopyScenario harness never joins a copy
// thread unconditionally. If the copy thread has not published a terminal
// result within the harness deadline, the harness dumps diagnostics (pending
// ops, staged ops, outstanding counts, copy_published, backend closed state,
// failures recorded so far) and aborts the test process — a buggy copy cannot
// hang the test suite forever.
//
// TEST TARGET: sluice_copy_pipeline_contract_test (in the default test group).

#include "harness.hpp"

#include "copy_task.hpp"

#include "support/scripted_async_backend.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__unix__)
#include <sys/wait.h>
#endif

using namespace sluice_copy;
using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {

// ---------------------------------------------------------------------------
// Allocation-failure injection (test-only, whole-binary).
//
// A global operator new that fails on demand: only allocations >= a threshold
// fail, and only while g_fail_big_alloc is armed (it self-disarms on the
// first failure). This lets the allocation-failure contract drive a REAL
// std::bad_alloc through run_pipelined_copy_with_backend's slot allocation
// (the first large allocation in the new start ordering) without allocating
// tens of GiB. Default state is off, so every other test case in this binary
// is unaffected.
// ---------------------------------------------------------------------------
std::atomic<bool> g_fail_big_alloc{false};
std::atomic<bool> g_big_alloc_failed{false};
std::size_t g_fail_alloc_threshold = 4096;

}  // namespace

// ---------------------------------------------------------------------------
// Allocation-failure injection (test-only, whole-binary) — ALL standard
// operator new/delete forms.
//
// A global operator new that fails on demand: only allocations >= a threshold
// fail, and only while g_fail_big_alloc is armed (it self-disarms on the
// first failure). This lets the allocation-failure contract drive a REAL
// std::bad_alloc through run_pipelined_copy_with_backend's slot allocation
// (the first large allocation in the new start ordering) without allocating
// tens of GiB. Default state is off, so every other test case in this binary
// is unaffected.
//
// Every deallocation form routes to free(), and every allocation form routes
// through the checked path, so new/delete pairs stay consistent (malloc/free)
// — including the sized and aligned forms libstdc++/ASan use. An incomplete
// override set would split pairs across the sanitizer runtime and produce
// alloc-dealloc-mismatch under ASan.
//
// The definitions are WEAK, and both sanitizer runtimes define STRONG
// operator new/delete symbols that preempt the weak override at link time:
// TSan's runtime needs them for its own accounting, and libasan's strong
// definitions win over the weak override when the ASan runtime is linked.
// So under TSan AND ASan the injection is unavailable — the
// allocation-failure case detects both sanitizers and skips. Under
// Debug/Release/UBSan these weak definitions are the only ones, so the
// injection is active there.
// ---------------------------------------------------------------------------

__attribute__((weak)) void* operator new(std::size_t n) {
    if (g_fail_big_alloc.load(std::memory_order::relaxed) &&
        n >= g_fail_alloc_threshold) {
        g_fail_big_alloc.store(false, std::memory_order::relaxed);
        g_big_alloc_failed.store(true, std::memory_order::relaxed);
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}

__attribute__((weak)) void* operator new[](std::size_t n) { return ::operator new(n); }

__attribute__((weak)) void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(n);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

__attribute__((weak)) void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    return ::operator new(n, std::nothrow);
}

__attribute__((weak)) void* operator new(std::size_t n, std::align_val_t al) {
    if (g_fail_big_alloc.load(std::memory_order::relaxed) &&
        n >= g_fail_alloc_threshold) {
        g_fail_big_alloc.store(false, std::memory_order::relaxed);
        g_big_alloc_failed.store(true, std::memory_order::relaxed);
        throw std::bad_alloc();
    }
    std::size_t a = static_cast<std::size_t>(al);
    if (a < sizeof(void*)) a = sizeof(void*);
    void* p = nullptr;
    if (::posix_memalign(&p, a, n == 0 ? 1 : n) != 0) throw std::bad_alloc();
    return p;
}

__attribute__((weak)) void* operator new[](std::size_t n, std::align_val_t al) {
    return ::operator new(n, al);
}

__attribute__((weak)) void* operator new(std::size_t n, std::align_val_t al,
                   const std::nothrow_t&) noexcept {
    try {
        return ::operator new(n, al);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

__attribute__((weak)) void* operator new[](std::size_t n, std::align_val_t al,
                     const std::nothrow_t&) noexcept {
    return ::operator new(n, al, std::nothrow);
}

__attribute__((weak)) void operator delete(void* p) noexcept { std::free(p); }
__attribute__((weak)) void operator delete(void* p, std::size_t) noexcept { std::free(p); }
__attribute__((weak)) void operator delete[](void* p) noexcept { std::free(p); }
__attribute__((weak)) void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
__attribute__((weak)) void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
__attribute__((weak)) void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
__attribute__((weak)) void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
__attribute__((weak)) void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}
__attribute__((weak)) void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
__attribute__((weak)) void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}

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

struct CopyScenario;  // defined below

// Dump full diagnostics and abort the test process. Called when a copy thread
// fails to publish within the harness deadline — the bounded-failure contract
// (Section 6): a test binary must fail loudly, never hang a suite forever.
// std::thread cannot be safely killed, so abort() is the only bounded outcome.
[[noreturn]] void watchdog_abort(CopyScenario& sc,
                                 std::chrono::milliseconds deadline);

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

    // Bounded-failure join (default deadline). See drain_and_join_for.
    void drain_and_join() { drain_and_join_for(kWaitFor * 3); }

    // Drain pending ops through the controller and join the copy thread, but
    // NEVER hang forever: if the copy thread has not published a terminal
    // outcome within `deadline`, dump diagnostics and abort the test process.
    //
    // Because the copy task CHAINS operations (completing one read/write can
    // unblock the task to submit the NEXT op), a single drain pass is not
    // enough: a new op may appear after the drain. So we loop: complete every
    // outstanding op, then wait briefly for the task to publish its terminal
    // outcome (sc.copy_result) OR to submit new ops; repeat until published or
    // the bounded deadline elapses. A real hang (no pending ops, no
    // publication, a thread that will never exit) is therefore a bounded TEST
    // FAILURE — the watchdog fires — not an infinite join. A std::thread can
    // never be safely killed, so the only bounded outcome is to terminate the
    // test process loudly (Section 6).
    void drain_and_join_for(std::chrono::milliseconds deadline) {
        if (joined) return;
        auto dl = std::chrono::steady_clock::now() + deadline;
        while (!copy_published.load(std::memory_order::acquire) &&
               std::chrono::steady_clock::now() < dl) {
            drain_all(controller);  // complete every currently-pending op
            // Give the copy thread a short window to publish or submit new ops.
            controller.wait_until_pending_for(1, std::chrono::milliseconds(20));
        }
        // Final drain in case the task published after submitting a terminal op.
        drain_all(controller);
        if (copy_published.load(std::memory_order::acquire)) {
            // Healthy: the task reached a terminal outcome, so its final
            // awaits have all completed and join() is guaranteed to return.
            if (copy_thread.joinable()) copy_thread.join();
            joined = true;
            return;
        }
        // The copy thread never published. This is a REAL hang (or a test
        // that forgot to drive the backend). Fail the process loudly instead
        // of blocking the suite forever.
        watchdog_abort(*this, deadline);
    }

    // Wait until >= min ops outstanding, bounded. Returns true if met.
    bool wait_pending(std::size_t min_count) {
        return controller.wait_until_pending_for(min_count, kWaitFor) ==
               WaitStatus::ready;
    }

    // Properly-synchronized terminal-outcome check for use WHILE the copy
    // thread is still running: publish() writes copy_result and then stores
    // copy_published (release); reading the flag with acquire synchronizes
    // the subsequent copy_result access. Directly reading copy_result while
    // the thread runs is a data race (TSan).
    bool published() const {
        return copy_published.load(std::memory_order::acquire);
    }

    // Finish: drain + join. After this, `result` reflects the terminal outcome.
    void finish() { drain_and_join(); }

    CopyScenario(const CopyScenario&) = delete;
    CopyScenario& operator=(const CopyScenario&) = delete;
};

// Watchdog implementation (declared above): dump full diagnostics and abort.
[[noreturn]] void watchdog_abort(CopyScenario& sc,
                                 std::chrono::milliseconds deadline) {
    std::fprintf(stderr,
        "\n[watchdog] copy thread did not publish a terminal result within "
        "%lld ms (copy_published=%d)\n",
        static_cast<long long>(deadline.count()),
        sc.copy_published.load(std::memory_order::acquire) ? 1 : 0);
    std::size_t staged = 0;
    for (auto& op : sc.controller.pending_operations()) {
        if (op.stage == OpStage::staged) ++staged;
        std::fprintf(stderr,
                     "[watchdog]   op id=%llu kind=%s stage=%s fd=%d "
                     "offset=%llu len=%zu\n",
                     static_cast<unsigned long long>(op.id),
                     to_string(op.kind),
                     op.stage == OpStage::staged ? "staged" : "pending",
                     op.fd, static_cast<unsigned long long>(op.offset),
                     op.length);
    }
    std::fprintf(stderr,
                 "[watchdog]   outstanding=%zu staged=%zu backend_closed=%d\n",
                 sc.controller.pending_count(), staged,
                 sc.controller.closed() ? 1 : 0);
    if (!::sluice_test::failures().empty()) {
        std::fprintf(stderr, "[watchdog] failures recorded so far:\n");
        for (auto& f : ::sluice_test::failures())
            std::fprintf(stderr, "[watchdog]   %s:%d: %s\n", f.file.c_str(),
                         f.line, f.expr.c_str());
    }
    std::fprintf(stderr,
                 "[watchdog] aborting test process (bounded-failure "
                 "contract; the copy thread can never be safely joined)\n");
    std::fflush(stderr);
    std::abort();
}

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
        if (sc.published()) break;
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
        if (sc.published()) break;
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
    // buffer_size above the app-level per-slot cap (kMaxBufferSize).
    {
        auto pair = make_scripted_backend();
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd,
                                                 kMaxBufferSize + 1,
                                                 /*depth=*/1, 1, SyncPolicy::none,
                                                 std::move(pair.backend));
        SLUICE_CHECK(!r.has_value());
        SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
    }
    // pipeline_depth above the app-level cap (kMaxPipelineDepth).
    {
        auto pair = make_scripted_backend();
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, 16,
                                                 kMaxPipelineDepth + 1, 1,
                                                 SyncPolicy::none,
                                                 std::move(pair.backend));
        SLUICE_CHECK(!r.has_value());
        SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
    }
    // Product within per-parameter caps but above the TOTAL byte cap:
    // 32 MiB * 32 slots = 1 GiB > kMaxPipelineBytes (512 MiB). This is the
    // case the per-parameter caps alone cannot catch.
    {
        auto pair = make_scripted_backend();
        constexpr std::size_t kB32 = 32 * 1024 * 1024;
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, kB32,
                                                 /*depth=*/32, 1, SyncPolicy::none,
                                                 std::move(pair.backend));
        SLUICE_CHECK(!r.has_value());
        SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
    }
    // workers above the app-level cap (kMaxWorkers).
    {
        auto pair = make_scripted_backend();
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, 16, /*depth=*/2,
                                                 kMaxWorkers + 1,
                                                 SyncPolicy::none,
                                                 std::move(pair.backend));
        SLUICE_CHECK(!r.has_value());
        SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
    }
}
#else
SLUICE_TEST_CASE(contract_rejects_invalid_arguments_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

// ===========================================================================
// Section 6: bounded-failure proof. A scenario whose copy thread never
// publishes (and has nothing outstanding) must terminate the test process
// within a bounded time via the harness watchdog — a buggy copy must never
// hang the suite forever.
//
// The hang child runs ONLY in a forked child process (selected via
// SLUICE_TEST_FILTER + SLUICE_WATCHDOG_CHILD); the parent asserts the child
// died with the watchdog diagnostic within a deadline.
// ===========================================================================
#if defined(__unix__)

SLUICE_TEST_CASE(contract_watchdog_hang_child) {
    // Child body: the copy thread parks on a gate that is never released —
    // no pending ops, no publication, forever. drain_and_join_for() must fire
    // the watchdog (diagnostics + abort) instead of hanging in join(). The
    // SLUICE_FAIL below is unreachable on the fixed code (the watchdog
    // aborts); if the watchdog regresses, the child records a failure and
    // exits non-zero, which the parent reports as a FAIL.
    if (std::getenv("SLUICE_WATCHDOG_CHILD") == nullptr) {
        return;  // only meaningful when spawned by contract_watchdog_hang_is_bounded
    }
    std::atomic<bool> gate{false};
    CopyScenario sc;
    sc.copy_thread = std::thread([&]() mutable {
        while (!gate.load(std::memory_order::acquire)) {
            std::this_thread::yield();
        }
    });
    sc.drain_and_join_for(std::chrono::seconds(2));
    SLUICE_FAIL("watchdog did not abort the hung copy thread");
}

SLUICE_TEST_CASE(contract_watchdog_hang_is_bounded) {
    // Fork a child that runs ONLY contract_watchdog_hang_child. The child's
    // watchdog must abort it within ~2s; the parent FAILS if the child is
    // still alive at the deadline (the pre-fix behavior: join() blocks
    // forever) or if the watchdog diagnostic is missing.
    int pipefd[2] = {-1, -1};
    if (::pipe(pipefd) != 0) {
        SLUICE_FAIL("pipe() failed");
        return;
    }
    pid_t pid = ::fork();
    if (pid < 0) {
        SLUICE_FAIL("fork() failed");
        return;
    }
    if (pid == 0) {
        // Child: run only the hang case with the watchdog env set; stderr is
        // redirected to the pipe so the parent can assert the diagnostics.
        ::close(pipefd[0]);
        if (::dup2(pipefd[1], STDERR_FILENO) < 0) std::_Exit(88);
        ::close(pipefd[1]);
        ::setenv("SLUICE_TEST_FILTER", "contract_watchdog_hang_child", 1);
        ::setenv("SLUICE_WATCHDOG_CHILD", "1", 1);
        char self[4096];
        ssize_t n = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n <= 0) std::_Exit(88);
        self[n] = '\0';
        ::execl(self, self, static_cast<char*>(nullptr));
        std::_Exit(88);
    }
    // Parent: bounded wait. The child MUST abort within a few seconds
    // (watchdog 2s + slack); anything else is a hang (or a watchdog
    // regression) and must FAIL the test, not block it.
    ::close(pipefd[1]);
    ::fcntl(pipefd[0], F_SETFL, ::fcntl(pipefd[0], F_GETFL, 0) | O_NONBLOCK);

    std::string captured;
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    int status = 0;
    bool exited = false;
    while (std::chrono::steady_clock::now() < deadline) {
        for (;;) {
            ssize_t r = ::read(pipefd[0], buf, sizeof(buf));
            if (r > 0) captured.append(buf, static_cast<std::size_t>(r));
            else break;
        }
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) { exited = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    for (;;) {
        ssize_t r = ::read(pipefd[0], buf, sizeof(buf));
        if (r > 0) captured.append(buf, static_cast<std::size_t>(r));
        else break;
    }
    ::close(pipefd[0]);

    if (!exited) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        SLUICE_CHECK_MSG(false,
            ("watchdog child still alive after deadline: the harness can hang "
             "forever (pre-fix behavior)\n" + captured).c_str());
        return;
    }
    // The child must have been ABORTED (SIGABRT) with the watchdog marker.
    bool signaled_abort = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    SLUICE_CHECK_MSG(signaled_abort,
        ("watchdog child did not abort (WIFSIGNALED=" +
         std::string(WIFSIGNALED(status) ? "1" : "0") + ")\n" + captured).c_str());
    SLUICE_CHECK_MSG(captured.find("[watchdog]") != std::string::npos,
        ("watchdog diagnostics missing from child stderr\n" + captured).c_str());
}

#endif  // __unix__

// ===========================================================================
// Section 6b: a backend that throws from submit_* must surface as an IoError
// result — never a silent hang.
//
// The Runtime swallows exceptions thrown by the user task at the task
// boundary (Group boundary contract, application_runtime.cpp). A task that
// lets an exception escape therefore dies SILENTLY: `done` is never
// published and the caller's done_cv wait blocks forever. A real trigger
// exists: under load, ThreadPoolBackend::enqueue_* can throw
// std::system_error when an op-thread spawn fails (observed in the Debug
// soak — the stuck process showed main + driver parked, no op threads,
// RSS 252MB). The copy task MUST translate any task-body exception into an
// IoError result.
//
// The child body runs ONLY in a forked child (SLUICE_WATCHDOG_CHILD +
// filter); the parent asserts the child EXITS 0 within a deadline. Pre-fix
// the child hangs (exception swallowed, done never published) and the parent
// FAILS; post-fix the child exits normally and the parent PASSES. The
// fork/exec pattern keeps the failure bounded even when the bug regresses.
// ===========================================================================
#if defined(__unix__)
#if SLUICE_HAS_PIPELINED_COPY

// Minimal backend whose submit_* throws std::system_error, simulating a
// ThreadPoolBackend op-thread spawn failure under resource exhaustion.
class ThrowingBackend : public sluice::async::AsyncBackend {
public:
    ~ThrowingBackend() override {
        stop_.store(true, std::memory_order::release);
        cv_.notify_all();
    }
    Result<void> submit_read(sluice::async::ReadOp,
                             sluice::async::Completion<std::size_t>&) override {
        throw std::system_error(
            std::make_error_code(std::errc::resource_unavailable_try_again));
    }
    Result<void> submit_write(sluice::async::WriteOp,
                              sluice::async::Completion<std::size_t>&) override {
        throw std::system_error(
            std::make_error_code(std::errc::resource_unavailable_try_again));
    }
    Result<void> submit_sync_data(sluice::async::SyncDataOp,
                                  sluice::async::Completion<void>&) override {
        throw std::system_error(
            std::make_error_code(std::errc::resource_unavailable_try_again));
    }
    Result<void> submit_sync_all(sluice::async::SyncAllOp,
                                 sluice::async::Completion<void>&) override {
        throw std::system_error(
            std::make_error_code(std::errc::resource_unavailable_try_again));
    }
    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override {
        // Park until destruction (no op ever completes on this backend).
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return stop_.load(std::memory_order::acquire); });
        return std::size_t{0};
    }
    std::size_t outstanding() const noexcept override { return 0; }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
};

SLUICE_TEST_CASE(contract_submit_exception_publishes_error) {
    // Child body: a copy against the throwing backend. Post-fix, the task
    // translates the exception into an IoError result and the child exits
    // normally. Pre-fix, the exception escapes the task, is swallowed by the
    // Runtime boundary, `done` is never published, and this case hangs — the
    // parent's deadline then FAILS the test (bounded) instead of hanging the
    // suite forever.
    if (std::getenv("SLUICE_WATCHDOG_CHILD") == nullptr) {
        return;  // only meaningful when spawned by the parent case below
    }
    TempFile src, dst;
    seed_file(src.fd, 8192 * 2);

    auto r = run_pipelined_copy_with_backend(
        src.fd, dst.fd, /*buffer_size=*/8192, /*depth=*/2, /*workers=*/1,
        SyncPolicy::none, std::make_unique<ThrowingBackend>());

    // The exception must surface as an IoError::backend_error Result — if any
    // exception escaped instead, the child would have crashed here.
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::backend_error);
}

SLUICE_TEST_CASE(contract_submit_exception_is_bounded_not_hang) {
    // Parent: fork a child that runs ONLY contract_submit_exception_publishes_error.
    // The child must EXIT 0 within the deadline. Pre-fix behavior: the child
    // hangs forever (exception swallowed -> done never published); the parent
    // kills it and FAILS the test — bounded, never a suite hang.
    pid_t pid = ::fork();
    if (pid < 0) {
        SLUICE_FAIL("fork() failed");
        return;
    }
    if (pid == 0) {
        ::setenv("SLUICE_TEST_FILTER", "contract_submit_exception_publishes_error", 1);
        ::setenv("SLUICE_WATCHDOG_CHILD", "1", 1);
        char self[4096];
        ssize_t n = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n <= 0) std::_Exit(88);
        self[n] = '\0';
        ::execl(self, self, static_cast<char*>(nullptr));
        std::_Exit(88);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    int status = 0;
    bool exited = false;
    while (std::chrono::steady_clock::now() < deadline) {
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) { exited = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!exited) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        SLUICE_CHECK_MSG(false,
            std::string("child still alive after deadline: a task-body "
                        "exception was swallowed, done was never published, "
                        "and the copy hung forever (pre-fix behavior)").c_str());
        return;
    }
    // The child must have EXITED 0 (the copy returned backend_error cleanly).
    bool exited_zero = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    SLUICE_CHECK_MSG(exited_zero,
        ("child did not exit 0 (WIFEXITED=" +
         std::string(WIFEXITED(status) ? "1" : "0") +
         " status=" + std::to_string(WEXITSTATUS(status)) + ")").c_str());
}

#endif  // SLUICE_HAS_PIPELINED_COPY
#endif  // __unix__

// ===========================================================================
// Section 7: bounded memory across MULTIPLE rounds of slot reuse.
//
// The original bounded-memory contract only observed the initial read window.
// Here we drive the FULL copy (chunk count >= 3*depth+1 so every slot is
// recycled at least three times) and record, for the whole execution:
//   - slot identity = the read Completion address (stable per slot);
//   - buffer base = submitted pointer minus the chunk-relative offset
//     (a short-read continuation submits buffer.data()+filled, so the raw
//     pointer is NOT a new allocation);
//   - submitted data pointer / offset / length;
//   - the outstanding-read peak.
// Acceptance: distinct allocation bases <= depth, slot identities == depth,
// no new buffers across reuse rounds, peak reads <= depth (and >= 2 for
// depth>1), and the copy result is exact.
// ===========================================================================
#if SLUICE_HAS_PIPELINED_COPY
SLUICE_TEST_CASE(contract_bounded_memory_multi_round) {
    constexpr std::size_t B = 16;
    constexpr std::size_t DEPTH = 3;
    constexpr std::size_t CHUNKS = DEPTH * 3 + 1;  // >= 3 rounds of reuse + 1
    constexpr std::size_t TOTAL = B * CHUNKS;
    TempFile src, dst;
    seed_file(src.fd, TOTAL);

    auto pair = make_scripted_backend();
    CopyScenario sc;
    sc.controller = pair.controller;

    std::set<void*> slot_ids;          // read Completion addresses
    std::set<std::byte*> buffer_bases;  // derived allocation bases
    std::map<void*, std::set<std::uint64_t>> slot_offsets;  // per-slot lifecycle
    std::size_t max_reads_observed = 0;
    std::vector<std::uint64_t> write_offsets;  // write submission order

    sc.copy_thread = std::thread([&]() mutable {
        auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, B, DEPTH, 1,
                                                 SyncPolicy::none,
                                                 std::move(pair.backend));
        sc.publish(std::move(r));
    });

    // Drive the FULL copy deterministically. Observation is race-free: an op
    // is erased from the backend map only AFTER the test stages its result
    // (complete_*), so recording each op AT COMPLETION TIME sees every op
    // exactly once. The pre-completion snapshot additionally measures the
    // true outstanding-read peak.
    for (;;) {
        std::size_t nreads = 0;
        for (auto& op : sc.controller.pending_operations()) {
            if (op.kind != OpKind::read) continue;
            ++nreads;
            if (nreads > max_reads_observed) max_reads_observed = nreads;
        }
        if (sc.published()) break;

        for (auto& op : sc.controller.pending_operations()) {
            if (op.stage != OpStage::pending || op.kind != OpKind::read) continue;
            // Record BEFORE completing: the op is in the map right now, and it
            // leaves the map only after we stage it (complete_bytes) and the
            // Runtime reaps it — so this sees every read exactly once.
            slot_ids.insert(op.completion_identity);
            // Chunk offsets are B-aligned, so the buffer base is the submitted
            // pointer minus the chunk-relative offset (offset % B == filled
            // for a continuation read in the same slot).
            std::byte* base = static_cast<std::byte*>(op.buffer) -
                              static_cast<std::ptrdiff_t>(op.offset % B);
            buffer_bases.insert(base);
            slot_offsets[op.completion_identity].insert(op.offset);
            if (op.offset >= TOTAL) sc.controller.complete_eof(op.id);
            else sc.controller.complete_bytes(op.id, B);
        }
        for (auto& op : sc.controller.pending_operations()) {
            if (op.stage != OpStage::pending || op.kind != OpKind::write) continue;
            write_offsets.push_back(op.offset);
            sc.controller.complete_bytes(op.id, op.length);
        }
        sc.controller.wait_until_pending_for(1, std::chrono::milliseconds(20));
    }

    sc.finish();

    SLUICE_CHECK(sc.copy_result.has_value());
    SLUICE_CHECK(sc.copy_result->has_value());
    SLUICE_CHECK(sc.copy_result->value().bytes_copied == TOTAL);
    // Writes: one per data chunk, strictly ascending (submission order is
    // completion order here because Version B keeps at most one write
    // outstanding).
    SLUICE_CHECK(write_offsets.size() == CHUNKS);
    bool ascending = true;
    for (std::size_t i = 1; i < write_offsets.size(); ++i)
        if (write_offsets[i] <= write_offsets[i - 1]) { ascending = false; break; }
    SLUICE_CHECK(ascending);
    // Bounded allocation across the WHOLE multi-round run.
    SLUICE_CHECK_MSG(buffer_bases.size() <= DEPTH,
                     "more distinct buffer bases than pipeline_depth");
    SLUICE_CHECK_MSG(buffer_bases.size() == DEPTH,
                     "fewer distinct buffer bases than pipeline_depth");
    SLUICE_CHECK_MSG(slot_ids.size() == DEPTH,
                     "slot identity count != pipeline_depth");
    SLUICE_CHECK_MSG(max_reads_observed <= DEPTH,
                     "peak outstanding reads > pipeline_depth");
    // Deterministic peak proof for depth>1: the BACKEND's own submit-time
    // peak (updated under its mutex) must reach >= 2 — the pipeline submits
    // the full initial read window before awaiting anything. The snapshot
    // observation above (max_reads_observed) is timing-dependent and only
    // meaningful as an upper bound.
    SLUICE_CHECK_MSG(sc.controller.max_outstanding_reads() >= 2,
                     "depth>1 never reached 2 concurrent reads (backend peak)");
    SLUICE_CHECK(sc.controller.max_outstanding_reads() <= DEPTH);
    // Every slot served >= 3 chunks (multi-round reuse actually happened).
    // Each slot's chunk sequence is an arithmetic progression with step DEPTH
    // (chunk_offset += depth*B on recycle), so with CHUNKS = 3*DEPTH+1 data
    // chunks plus EOF reads, every slot serves at least 3 chunks.
    SLUICE_CHECK(slot_offsets.size() == DEPTH);
    for (auto& [id, offsets] : slot_offsets)
        SLUICE_CHECK_MSG(offsets.size() >= 3,
                         "a slot served fewer than 3 chunks across the run");
}
#else
SLUICE_TEST_CASE(contract_bounded_memory_multi_round_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

// ===========================================================================
// Section 3: allocation failure must become IoError::no_space, must not
// escape as an exception, and must not leave a started Runtime behind.
//
// Uses the test-only (weak) global operator new at the top of this file to
// fail the slot buffer allocation deterministically: in the new start ordering
// the slots are allocated BEFORE the Runtime is built, so the first
// >= threshold allocation IS the slot buffer and the failure happens before
// any Runtime exists.
//
// The override is weak, so under TSan and ASan the sanitizer runtime's
// STRONG operator new/delete symbols preempt it at link time and the
// injection cannot fire; the case skips in those builds. Evidence comes from
// Debug/Release/UBSan, where the override is active.
// ===========================================================================
#if SLUICE_HAS_PIPELINED_COPY
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define SLUICE_CONTRACT_ALLOCATOR_INJECTION_UNAVAILABLE 1
#  endif
#  if __has_feature(address_sanitizer)
#    define SLUICE_CONTRACT_ALLOCATOR_INJECTION_UNAVAILABLE 1
#  endif
#endif
#if !defined(SLUICE_CONTRACT_ALLOCATOR_INJECTION_UNAVAILABLE) && \
    defined(__SANITIZE_THREAD__)
#  define SLUICE_CONTRACT_ALLOCATOR_INJECTION_UNAVAILABLE 1
#endif
#if !defined(SLUICE_CONTRACT_ALLOCATOR_INJECTION_UNAVAILABLE) && \
    defined(__SANITIZE_ADDRESS__)
#  define SLUICE_CONTRACT_ALLOCATOR_INJECTION_UNAVAILABLE 1
#endif

SLUICE_TEST_CASE(contract_allocation_failure_returns_no_space) {
#if defined(SLUICE_CONTRACT_ALLOCATOR_INJECTION_UNAVAILABLE)
    // TSan's and ASan's runtimes own the global operator new/delete symbols
    // (strong defs preempt the weak test override at link time), so the
    // injection cannot fire in sanitizer builds. The allocation-failure
    // contract is covered by the Debug/Release/UBSan builds.
    return;
#else
    constexpr std::size_t B = 8192;  // >= g_fail_alloc_threshold (4096)
    TempFile src, dst;
    seed_file(src.fd, B * 2);

    auto pair = make_scripted_backend();

    g_fail_big_alloc.store(true, std::memory_order::relaxed);
    g_big_alloc_failed.store(false, std::memory_order::relaxed);
    auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, B, /*depth=*/2, 1,
                                             SyncPolicy::none,
                                             std::move(pair.backend));

    // The bad_alloc must be translated to IoError::no_space — and if any
    // exception escaped instead, the test binary would have crashed here.
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::no_space);
    SLUICE_CHECK(g_big_alloc_failed.load(std::memory_order::relaxed));
    // Slots are allocated BEFORE the Runtime is built/started, so the backend
    // never received a submit (and no Runtime exists to leak).
    SLUICE_CHECK(pair.controller.pending_count() == 0);

    // A subsequent REAL copy works normally (the allocator is restored; the
    // failed run left no global state behind). Uses the production
    // ThreadPoolBackend on the real temp files, so this is a genuine
    // end-to-end recovery check.
    auto r2 = run_pipelined_copy(src.fd, dst.fd, B, /*depth=*/2, 1,
                                 SyncPolicy::none);
    SLUICE_CHECK(r2.has_value());
    SLUICE_CHECK(r2.value().bytes_copied == B * 2);
#endif
}
#else
SLUICE_TEST_CASE(contract_allocation_failure_not_implemented) {
    SLUICE_FAIL("Version B pipelined copy not implemented");
}
#endif

SLUICE_MAIN()
